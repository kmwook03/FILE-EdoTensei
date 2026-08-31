#include "io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <limits>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
bool writeAll(int fd, const uint8_t* data, size_t size, std::string& error) {
    size_t done = 0;
    while (done < size) {
        const ssize_t count = ::write(fd, data + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            error = std::string("write failed: ") + std::strerror(errno);
            return false;
        }
        done += static_cast<size_t>(count);
    }
    return true;
}
}

ImageReader::~ImageReader() { close(); }

bool ImageReader::open(const std::string& path, std::string& error) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        error = std::string("cannot open input: ") + std::strerror(errno);
        return false;
    }
    struct stat info {};
    if (fstat(fd_, &info) != 0 || info.st_size < 0) {
        error = std::string("cannot determine input size: ") + std::strerror(errno);
        close();
        return false;
    }
    size_ = static_cast<uint64_t>(info.st_size);
    if (S_ISBLK(info.st_mode)) {
        if (ioctl(fd_, BLKGETSIZE64, &size_) != 0) {
            error = std::string("cannot determine block device size: ") + std::strerror(errno);
            close();
            return false;
        }
    }
    path_ = path;
    return true;
}

void ImageReader::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    size_ = 0;
    path_.clear();
}

bool ImageReader::read(uint64_t offset, void* buffer, size_t size, size_t& bytesRead,
                       std::string& error) const {
    bytesRead = 0;
    if (fd_ < 0) { error = "input is not open"; return false; }
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        error = "read offset is not representable"; return false;
    }
    auto* output = static_cast<uint8_t*>(buffer);
    while (bytesRead < size) {
        const uint64_t current = offset + bytesRead;
        if (current < offset || current > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
            error = "read offset overflow"; return false;
        }
        const ssize_t count = pread(fd_, output + bytesRead, size - bytesRead,
                                    static_cast<off_t>(current));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = std::string("read failed: ") + std::strerror(errno); return false; }
        if (count == 0) break;
        bytesRead += static_cast<size_t>(count);
    }
    return true;
}

bool ImageReader::readExact(uint64_t offset, void* buffer, size_t size,
                            std::string& error) const {
    size_t count = 0;
    if (!read(offset, buffer, size, count, error)) return false;
    if (count != size) { error = "unexpected end of input"; return false; }
    return true;
}

bool ImageReader::readVector(uint64_t offset, size_t size, std::vector<uint8_t>& output,
                             std::string& error) const {
    output.resize(size);
    size_t count = 0;
    if (!read(offset, output.data(), size, count, error)) return false;
    output.resize(count);
    return true;
}

bool writeRecoveredFile(const ImageReader& image, const std::vector<Extent>& extents,
                        uint64_t byteLimit, const std::string& outputDirectory,
                        const std::string& baseName, std::string& finalPath,
                        uint64_t& bytesWritten, std::string& error,
                        const std::vector<uint8_t>* residentData) {
    namespace fs = std::filesystem;
    bytesWritten = 0;
    std::error_code ec;
    fs::create_directories(outputDirectory, ec);
    if (ec) { error = "cannot create output directory: " + ec.message(); return false; }

    fs::path chosen = fs::path(outputDirectory) / baseName;
    const fs::path stem = chosen.stem();
    const fs::path extension = chosen.extension();
    for (uint64_t suffix = 1;; ++suffix) {
        const fs::path tempCheck = chosen.string() + ".tmp";
        if (!fs::exists(chosen, ec) && !fs::exists(tempCheck, ec)) break;
        chosen = fs::path(outputDirectory) / (stem.string() + "_" +
                 std::to_string(suffix) + extension.string());
    }
    const fs::path temporary = chosen.string() + ".tmp";
    int out = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) { error = std::string("cannot create temporary output: ") + std::strerror(errno); return false; }

    bool ok = true;
    std::vector<uint8_t> buffer(1024 * 1024);
    uint64_t remaining = byteLimit;
    if (residentData && !residentData->empty()) {
        if (byteLimit > residentData->size()) {
            error = "resident data is shorter than the promised length";
            ok = false;
        } else {
            ok = writeAll(out, residentData->data(), static_cast<size_t>(byteLimit), error);
            if (ok) { bytesWritten = byteLimit; remaining = 0; }
        }
    }
    for (const auto& extent : extents) {
        if (residentData && !residentData->empty()) break;
        uint64_t position = extent.offset;
        uint64_t extentRemaining = std::min(extent.length, remaining);
        while (extentRemaining > 0 && ok) {
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(buffer.size(), extentRemaining));
            if (extent.sparse) {
                std::fill(buffer.begin(), buffer.begin() + amount, 0);
            } else {
                size_t got = 0;
                if (!image.read(position, buffer.data(), amount, got, error) || got != amount) {
                    if (error.empty()) error = "source extent ended early";
                    ok = false;
                    break;
                }
                position += amount;
            }
            ok = writeAll(out, buffer.data(), amount, error);
            extentRemaining -= amount;
            remaining -= amount;
            bytesWritten += amount;
        }
        if (remaining == 0 || !ok) break;
    }
    if (remaining != 0 && ok) { error = "extents did not contain the promised length"; ok = false; }
    if (ok && fsync(out) != 0) { error = std::string("fsync failed: ") + std::strerror(errno); ok = false; }
    if (::close(out) != 0 && ok) { error = std::string("close failed: ") + std::strerror(errno); ok = false; }
    if (!ok) { ::unlink(temporary.c_str()); return false; }
    if (::rename(temporary.c_str(), chosen.c_str()) != 0) {
        error = std::string("rename failed: ") + std::strerror(errno);
        ::unlink(temporary.c_str());
        return false;
    }
    finalPath = chosen.string();
    return true;
}
