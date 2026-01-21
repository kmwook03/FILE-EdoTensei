#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include "signature.hpp"

// Class for carving files from a disk image
class FileCarver {
public:
    // Constructor and Destructor
    // @param path: Path to the disk image file
    explicit FileCarver(const std::string& path);

    // Destructor
    ~FileCarver();

    // initialize the carver
    // @return: true if initialization is successful, false otherwise
    bool initialize();

    // Start the file carving process
    // @return: void
    void startCarving();

private:
    // --- I/O and Disk info ---
    std::string filePath_;                           // Path to the image file
    int fd_ = -1;                                    // File descriptor for the input file
    uint64_t diskSize_ = 0;                          // Size of the disk image
    const size_t bufferSize_ = 1024 * 1024;          // Buffer size for reading the file

    // --- Carving state management ---
    bool isExtracting_ = false;                      // Flag to indicate if currently extracting a file
    uint64_t lastProcessedOffset_ = 0;               // Last processed offset in the disk image
    const FileSignature* activeSignature_ = nullptr; // Currently active file signature being processed
    std::ofstream outputStream_;                     // Output stream for the extracted file
    std::vector<FileSignature> signatures_; // Vector of file signatures to look for

    // --- Private Methods ---

    // Scan a buffer for file signatures
    // @param buffer: The buffer to scan
    // @param currentOffset: The current offset in the file
    // @return: void
    void scanBuffer(const std::vector<uint8_t>& buffer, uint64_t currentOffset);
    
    // Match a signature in the buffer at a given offset
    // @param buffer: The buffer to scan
    // @param offset: The offset in the buffer to start matching
    // @param signature: The signature to match
    // @return: true if the signature matches, false otherwise
    bool matchSignature(const std::vector<uint8_t>& buffer, size_t offset, const std::vector<uint8_t>& signature);

    // Start a new file extraction
    // @param offset: The offset in the disk image where the file starts
    void startNewFile(uint64_t offset);

    // Write data to the currently extracted file
    // @param data: Pointer to the data to write
    // @param size: Size of the data to write
    void writeData(const uint8_t* data, size_t size);

    // Finish the current file extraction
    // @return: void
    void finishFile();
};
