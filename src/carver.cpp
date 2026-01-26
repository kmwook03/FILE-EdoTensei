#include "carver.hpp"
#include "searcher.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>


FileCarver::FileCarver(const std::string& path) : filePath_(path) {}

FileCarver::~FileCarver() {
    if (fd_ != -1) close(fd_);
    if (out_fd_ != -1) close(out_fd_);
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
    size_t currentBufferIdx = 0;
    size_t bufferSize = buffer.size();

    while (currentBufferIdx < bufferSize) {
        // Search Header
        if (!isExtracting_) {
            bool foundHeader = false;
            int64_t bestFoundIdx = -1;
            const FileSignature* bestSig = nullptr;

            // Find the earliest header in the buffer
            for (const auto& sig : signatures_) {
                int64_t foundIdx = Searcher::search(buffer, sig.header, currentBufferIdx);
                if (foundIdx != -1) {
                    if (bestFoundIdx == -1 || foundIdx < bestFoundIdx) {
                        bestFoundIdx = foundIdx;
                        bestSig = &sig;
                    }
                }
            }

            if (bestFoundIdx != -1) {
                size_t foundPos = static_cast<size_t>(bestFoundIdx);
                
                isExtracting_ = true;
                activeSignature_ = bestSig;

                uint64_t headerOffset = currentOffset + foundPos;
                startNewFile(headerOffset);
                writeData(bestSig->header.data(), bestSig->header.size());

                if (bestSig->extension == "pdf") {
                    std::cout << "[Debug] Found PDF Start at offset: " << headerOffset << std::endl;
                }

                currentBufferIdx = foundPos + bestSig->header.size();
                // back to top of while loop
                continue; 
            }

            // no header found, exit loop
            break; 
        } 
        
        // Data extraction and collision/Footer detection
        else {
            size_t writeEndIdx = bufferSize; // 기본적으로 버퍼 끝까지 씀
            bool collisionDetected = false;
            size_t collisionIdx = 0;

            // 1. [Collision Detection] Search for 'other file headers' within the buffer
            // When extracting PDF, ignore JPG headers (FF D8) due to Embedded Images
            // But if other PDF or PNG headers appear, we should stop.
            
            for (const auto& sig : signatures_) {
                // When extracting PDF: only consider same PDF headers or PNG headers as collisions (ignore JPG)
                if (activeSignature_->extension == "pdf") {
                    if (sig.extension == "jpg") continue; 
                }

                // Search from current position
                int64_t foundIdx = Searcher::search(buffer, sig.header, currentBufferIdx);
                
                // If found and within current processing range, it's a collision!
                if (foundIdx != -1) {
                    // Find the earliest collision point
                    if (!collisionDetected || static_cast<size_t>(foundIdx) < collisionIdx) {
                        collisionIdx = static_cast<size_t>(foundIdx);
                        collisionDetected = true;
                    }
                }
            }

            // 2. [Footer Search] Search for the active file's footer
            int64_t footerIdx = -1;
            if (activeSignature_->hasFooter) {
                footerIdx = Searcher::search(buffer, activeSignature_->footer, currentBufferIdx);
            }

            // Footer vs New Header vs Buffer End
            
            // Case A: Collision (new file) occurred before Footer, or collision occurred without Footer
            if (collisionDetected && (footerIdx == -1 || collisionIdx < static_cast<size_t>(footerIdx))) {
                // Write data up to collision point
                writeData(buffer.data() + currentBufferIdx, collisionIdx - currentBufferIdx);
                
                std::cout << "[Debug] Collision detected! Switching file..." << std::endl;
                
                // Force close current file
                finalizeIncrementalFile(); 
                
                // Move the index to the collision point and since isExtracting_ is now false,
                // the next loop will execute [Mode 1] to find a new file.
                currentBufferIdx = collisionIdx;
                continue;
            }

            // Case B: Footer found (no collision or Footer before collision)
            if (footerIdx != -1) {
                size_t foundPos = static_cast<size_t>(footerIdx);
                
                // Write data up to Footer
                if (foundPos > currentBufferIdx) {
                    writeData(buffer.data() + currentBufferIdx, foundPos - currentBufferIdx);
                }
                
                // Write Footer
                writeData(activeSignature_->footer.data(), activeSignature_->footer.size());
                size_t footerSize = activeSignature_->footer.size();
                size_t nextIdx = foundPos + footerSize;

                if (activeSignature_->isIncremental) {
                    recordCandidateEndOfFile(); // For PDF, do not close but record candidate point
                    currentBufferIdx = nextIdx;
                    continue; // Continue scanning
                } else {
                    finishFile(); // For JPG, PNG, finish immediately
                    isExtracting_ = false;
                    activeSignature_ = nullptr;
                    currentBufferIdx = nextIdx;
                    continue;
                }
            }

            // Case C: Nothing found (just data)
            writeData(buffer.data() + currentBufferIdx, bufferSize - currentBufferIdx);
            break; // Load next buffer
        }
    }
}
void FileCarver::startNewFile(uint64_t offset) {
    std::string fileName = "recovered_" + std::to_string(offset) + "." + activeSignature_->extension;

    // O_WRONLY: Open for write only
    // O_CREAT: Create file if it does not exist
    // O_TRUNC: Truncate file to zero length if it already exists
    // 0644: File permissions - owner can read/write, others can read
    out_fd_ = open(fileName.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd_ == -1) {
        std::cerr << "Error creating file: " << fileName << std::endl;
    }
}

void FileCarver::writeData(const uint8_t* data, size_t size) {
    if (out_fd_ < 0) return;

    const off_t MAX_FILE_SIZE = 100 * 1024 * 1024; // 100 MB
    off_t currentSize = lseek(out_fd_, 0, SEEK_CUR);

    if (currentSize + static_cast<off_t>(size) > MAX_FILE_SIZE) {
        std::cerr << "[-] Max file size reached. Force finalizing." << std::endl;
        finalizeIncrementalFile(); 
        return;
    }

    ssize_t written = write(out_fd_, data, size);
    if (written == -1) {
        perror("[-] Write error");
        close(out_fd_);
        out_fd_ = -1;
    }
}

void FileCarver::finishFile() {
    if (out_fd_ != -1) {
        close(out_fd_);
        out_fd_ = -1;
        std::cout << " [Saved] File recovery complete." << std::endl;
    }
}

void FileCarver::recordCandidateEndOfFile() {
    if (out_fd_ < 0) return;
    lastValidFooterOffset_ = lseek64(out_fd_, 0, SEEK_CUR);
}

void FileCarver::finalizeIncrementalFile() {
    if (out_fd_ < 0) return;

    if (activeSignature_ && activeSignature_->isIncremental && lastValidFooterOffset_ > 0) {
        off_t currentSize = lseek(out_fd_, 0, SEEK_CUR);
        if (currentSize > lastValidFooterOffset_) {
            if (ftruncate(out_fd_, lastValidFooterOffset_) == -1) {
                perror("[-] Error truncating file");
            } else {
                // std::cout << " [Info] Truncated PDF junk data." << std::endl;
            }
        }
    }

    close(out_fd_);
    out_fd_ = -1;
    isExtracting_ = false;
    activeSignature_ = nullptr;
}
