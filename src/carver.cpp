#include "../include/carver.hpp"
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
        
        if (currentOffset + bytesRead < diskSize_) {
            currentOffset += bytesRead - overlap;
        } else {
        currentOffset += bytesRead;
        }
    }
}

void FileCarver::scanBuffer(const std::vector<uint8_t>& buffer, uint64_t currentOffset) {
    // Scan the buffer for file signatures
    for (size_t i = 0; i < buffer.size(); ++i) {
        uint64_t absolutePos = currentOffset + i;
        if (!isExtracting_) {
            for (const auto& sig : signatures_) {
                if (matchSignature(buffer, i, sig.header)) {
                    isExtracting_ = true;
                    activeSignature_ = &sig;

                    startNewFile(currentOffset + i);
                    writeData(sig.header.data(), sig.header.size());

                    lastProcessedOffset_ = absolutePos + sig.header.size() - 1;
                    i += sig.header.size() - 1;
                    break;
                }
            }
        } else {
            if (absolutePos <= lastProcessedOffset_) {
                continue;
            }

            if (activeSignature_->hasFooter && matchSignature(buffer, i, activeSignature_->footer)) {
                writeData(activeSignature_->footer.data(), activeSignature_->footer.size());
                size_t footerSize = activeSignature_->footer.size();
                finishFile();

                isExtracting_ = false;
                activeSignature_ = nullptr;

                i += footerSize - 1;
            } else {
                uint8_t currentByte = buffer[i];
                writeData(&currentByte, 1);
            }
        }
    }
}

bool FileCarver::matchSignature(const std::vector<uint8_t>& buffer, size_t offset, const std::vector<uint8_t>& signature) {
    if (offset + signature.size() > buffer.size()) return false;
    return std::memcmp(buffer.data() + offset, signature.data(), signature.size()) == 0;
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
