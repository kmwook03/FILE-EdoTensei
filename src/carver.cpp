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
    formats_ = SignatureDB::getFormats();           // Load file format descriptors
    scanner_.build(formats_);
    return true;
}

void FileCarver::startCarving() {
    std::vector<uint8_t> buffer(bufferSize_);   // Buffer for reading file data
    uint64_t currentOffset = 0;                 // Current offset in the disk image
    const size_t overlap = scanner_.maxPatternLength() > 0 ? scanner_.maxPatternLength() - 1 : 0;

    while (currentOffset < diskSize_) {
        lseek64(fd_, currentOffset, SEEK_SET);
        ssize_t bytesRead = read(fd_, buffer.data(), bufferSize_);
        if (bytesRead <= 0) break; // Error or end(0) of file
        
        if (static_cast<size_t>(bytesRead) < bufferSize_) buffer.resize(bytesRead);

        scanBuffer(buffer, currentOffset);
        
        if (isExtracting_) currentOffset += bytesRead;
        else {
            if (currentOffset + bytesRead < diskSize_) {
                currentOffset += bytesRead > static_cast<ssize_t>(overlap) ? bytesRead - overlap : bytesRead;
            } else {
            currentOffset += bytesRead;
            }
        }
    }
}

void FileCarver::scanBuffer(const std::vector<uint8_t>& buffer, uint64_t currentOffset) {
    size_t currentBufferIdx = 0;
    size_t bufferSize = buffer.size();
    std::vector<SearchMatch> matches = scanner_.findAll(buffer);

    while (currentBufferIdx < bufferSize) {
        // Search Header
        if (!isExtracting_) {
            const SearchMatch* headerMatch = findNextHeaderMatch(matches, currentBufferIdx);

            if (headerMatch) {
                size_t foundPos = headerMatch->offset;
                const FormatDescriptor& format = formats_[headerMatch->metadata.formatIndex];
                const BytePattern* header = format.primaryHeader();
                if (!header) break;
                
                isExtracting_ = true;
                activeFormat_ = &format;

                uint64_t headerOffset = currentOffset + foundPos;
                startNewFile(headerOffset);
                writeData(header->bytes.data(), header->bytes.size());

                if (format.extension == "pdf") {
                    std::cout << "[Debug] Found PDF Start at offset: " << headerOffset << std::endl;
                }

                currentBufferIdx = foundPos + header->bytes.size();
                // back to top of while loop
                continue; 
            }

            // no header found, exit loop
            break; 
        } 
        
        // Data extraction and collision/Footer detection
        else {
            // 1. [Collision Detection] Search for 'other file headers' within the buffer
            // When extracting PDF, ignore JPG headers (FF D8) due to Embedded Images
            // But if other PDF or PNG headers appear, we should stop.
            const SearchMatch* collisionMatch = findNextCollisionMatch(matches, currentBufferIdx);

            // 2. [Footer Search] Search for the active file's footer
            const SearchMatch* footerMatch = findNextFooterMatch(matches, currentBufferIdx);
            size_t footerIdx = 0;
            const BytePattern* footer = activeFormat_->primaryFooter();
            bool footerDetected = footerMatch && footer;
            if (footerDetected) footerIdx = footerMatch->offset;

            // Footer vs New Header vs Buffer End
            
            // Case A: Collision (new file) occurred before Footer, or collision occurred without Footer
            if (collisionMatch && (!footerDetected || collisionMatch->offset < footerIdx)) {
                // Write data up to collision point
                writeData(buffer.data() + currentBufferIdx, collisionMatch->offset - currentBufferIdx);
                
                std::cout << "[Debug] Collision detected! Switching file..." << std::endl;
                
                // Force close current file
                finalizeIncrementalFile(); 
                
                // Move the index to the collision point and since isExtracting_ is now false,
                // the next loop will execute [Mode 1] to find a new file.
                currentBufferIdx = collisionMatch->offset;
                continue;
            }

            // Case B: Footer found (no collision or Footer before collision)
            if (footerDetected) {
                size_t foundPos = footerIdx;
                
                // Write data up to Footer
                if (foundPos > currentBufferIdx) {
                    writeData(buffer.data() + currentBufferIdx, foundPos - currentBufferIdx);
                }
                
                // Write Footer
                writeData(footer->bytes.data(), footer->bytes.size());
                size_t footerSize = footer->bytes.size();
                size_t nextIdx = foundPos + footerSize;

                if (activeFormat_->isIncremental()) {
                    recordCandidateEndOfFile(); // For PDF, do not close but record candidate point
                    currentBufferIdx = nextIdx;
                    continue; // Continue scanning
                } else {
                    finishFile(); // For JPG, PNG, finish immediately
                    isExtracting_ = false;
                    activeFormat_ = nullptr;
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

const SearchMatch* FileCarver::findNextHeaderMatch(const std::vector<SearchMatch>& matches, size_t startIdx) const {
    for (const auto& match : matches) {
        if (match.offset < startIdx || match.metadata.kind != PatternKind::Header) continue;
        if (match.metadata.formatIndex >= formats_.size()) continue;
        return &match;
    }
    return nullptr;
}

const SearchMatch* FileCarver::findNextCollisionMatch(const std::vector<SearchMatch>& matches, size_t startIdx) const {
    for (const auto& match : matches) {
        if (match.offset < startIdx || match.metadata.kind != PatternKind::Header) continue;
        if (match.metadata.formatIndex >= formats_.size()) continue;

        const FormatDescriptor& format = formats_[match.metadata.formatIndex];
        if (activeFormat_ && activeFormat_->allowEmbeddedJpgHeaders && format.extension == "jpg") {
            continue;
        }

        return &match;
    }
    return nullptr;
}

const SearchMatch* FileCarver::findNextFooterMatch(const std::vector<SearchMatch>& matches, size_t startIdx) const {
    if (!activeFormat_ || !activeFormat_->hasFooter()) return nullptr;

    size_t activeFormatIndex = formats_.size();
    for (size_t i = 0; i < formats_.size(); ++i) {
        if (&formats_[i] == activeFormat_) {
            activeFormatIndex = i;
            break;
        }
    }

    if (activeFormatIndex == formats_.size()) return nullptr;

    for (const auto& match : matches) {
        if (match.offset < startIdx || match.metadata.kind != PatternKind::Footer) continue;
        if (match.metadata.formatIndex == activeFormatIndex) return &match;
    }
    return nullptr;
}

void FileCarver::startNewFile(uint64_t offset) {
    std::string fileName = "recovered_" + std::to_string(offset) + "." + activeFormat_->extension;
    lastValidFooterOffset_ = 0;

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

    const off_t maxFileSize = activeFormat_ ? static_cast<off_t>(activeFormat_->maxFileSize) : 100 * 1024 * 1024;
    off_t currentSize = lseek(out_fd_, 0, SEEK_CUR);

    if (currentSize + static_cast<off_t>(size) > maxFileSize) {
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

    if (activeFormat_ && activeFormat_->isIncremental() && lastValidFooterOffset_ > 0) {
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
    activeFormat_ = nullptr;
}
