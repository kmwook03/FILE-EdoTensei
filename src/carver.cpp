#include "carver.hpp"
#include "searcher.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>


FileCarver::FileCarver(const std::string& path) : filePath_(path) {}

FileCarver::~FileCarver() {
    if (fd_ != -1) close(fd_);
    if (outputStream_.is_open()) outputStream_.close();
}

bool FileCarver::initialize() {
    fd_ = open(filePath_.c_str(), O_RDONLY | O_LARGEFILE);
    if (fd_ < 0) {
        perror("Error opening file");
        return false;
    }

    diskSize_ = lseek64(fd_, 0, SEEK_END);          // Get the size of the disk image
    lseek64(fd_, 0, SEEK_SET);                      // Reset file offset to the beginning
    signatures_ = SignatureDB::getSignatures();     // Load file signatures
    return true;
}

void FileCarver::startCarving() {
    std::vector<uint8_t> buffer(bufferSize_);   // Buffer for reading file data
    uint64_t currentOffset = 0;                 // Current offset in the disk image
    const size_t overlap = 16;                  // Overlap size to handle signatures across buffer boundaries

    while (currentOffset < diskSize_) {
        lseek64(fd_, currentOffset, SEEK_SET);
        ssize_t bytesRead = read(fd_, buffer.data(), bufferSize_);
        if (bytesRead <= 0) break; // Error or end(0) of file
        
        if (static_cast<size_t>(bytesRead) < bufferSize_) buffer.resize(bytesRead);

        scanBuffer(buffer, currentOffset);
        
        if (isExtracting_) currentOffset += bytesRead;
        else {
            if (currentOffset + bytesRead < diskSize_) {
                currentOffset += bytesRead - overlap;
            } else {
            currentOffset += bytesRead;
            }
        }
    }
}

void FileCarver::scanBuffer(const std::vector<uint8_t>& buffer, uint64_t currentOffset) {
    // Scan the buffer for file signatures
    size_t currentBufferIdx = 0;
    size_t bufferSize = buffer.size();

    while (currentBufferIdx < bufferSize) {
        // fine Header
        if (!isExtracting_) {
            bool foundHeader = false;
            for (const auto& sig : signatures_) {
                int64_t foundIdx = Searcher::search(buffer, sig.header, currentBufferIdx);
                if (foundIdx != -1) {
                    size_t foundPos = static_cast<size_t>(foundIdx);

                    isExtracting_ = true;
                    activeSignature_ = &sig;

                    uint64_t headerOffset = currentOffset + foundPos;
                    startNewFile(headerOffset);
                    writeData(sig.header.data(), sig.header.size());

                    currentBufferIdx = foundPos + sig.header.size();
                    foundHeader = true;
                    break;
                }
            }
            
            if (!foundHeader) break;
        } else {
            if (activeSignature_->hasFooter) {
                int64_t foundIdx = Searcher::search(buffer, activeSignature_->footer, currentBufferIdx);

                if (foundIdx != -1) {
                    size_t foundPos = static_cast<size_t>(foundIdx);

                    size_t dataSize = foundPos - currentBufferIdx;
                    if (dataSize > 0) {
                        writeData(buffer.data() + currentBufferIdx, dataSize);
                    }

                    writeData(activeSignature_->footer.data(), activeSignature_->footer.size());
                    size_t footerSize = activeSignature_->footer.size();

                    finishFile();

                    isExtracting_ = false;
                    activeSignature_ = nullptr;

                    currentBufferIdx = foundPos + footerSize;
                } else {
                    if (outputStream_.tellp() > 50 * 1024 * 1024) {
                        std::cerr << " [!] Warning: File too large." << std::endl;
                        finishFile();
                        isExtracting_ = false;
                        activeSignature_ = nullptr;
                        break;
                    }
                    writeData(buffer.data() + currentBufferIdx, bufferSize - currentBufferIdx);
                    break;
                }
            } else break;
        }
    }
}

void FileCarver::startNewFile(uint64_t offset) {
    std::string fileName = "recovered_" + std::to_string(offset) + "." + activeSignature_->extension;
    outputStream_.open(fileName, std::ios::binary);
    if (!outputStream_) {
        std::cerr << "Error creating file: " << fileName << std::endl;
    }
}

void FileCarver::writeData(const uint8_t* data, size_t size) {
    if (outputStream_.is_open()) {
        outputStream_.write(reinterpret_cast<const char*>(data), size);
    }
}

void FileCarver::finishFile() {
    if (outputStream_.is_open()) {
        outputStream_.close();
        std::cout << " [Saved] File recovery complete." << std::endl;
    }
}
