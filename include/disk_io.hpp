#ifndef DISK_IO_HPP
#define DISK_IO_HPP

#pragma once
#include "ntfs_structure.hpp"
#include <string>
#include <vector>
#include <fstream>

class NTFSReader {
public:
    NTFSReader();
    ~NTFSReader();

    // Open and close disk image
    bool openImage(const std::string& path);
    void closeImage();

    // Read Volume Boot Record (VBR)
    bool readVBR(NTFS_VBR& vbr);

    // Read MFT Entry at specific offset
    bool readRaw(uint64_t offset, void* buffer, size_t size);

private:
    // Handle to the disk image
    void* imageHandle;
    std::ifstream diskImage;
};

#endif // DISK_IO_HPP
