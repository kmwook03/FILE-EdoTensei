#include "../include/disk_io.hpp"
#include <fstream>
#include <iostream>

NTFSReader::NTFSReader() {}

NTFSReader::~NTFSReader() {
    closeImage();
}

bool NTFSReader::openImage(const std::string& path) {
    diskImage.open(path, std::ios::binary);

    if (!diskImage.is_open()) {
        std::cerr << "Error: Failed to open disk image: " << path << std::endl;
        return false;
    }
    return true;
}

void NTFSReader::closeImage() {
    if (diskImage.is_open()) { diskImage.close(); }
}

bool NTFSReader::readVBR(NTFS_VBR& vbr) {
    return readRaw(0, &vbr, sizeof(NTFS_VBR));
}

bool NTFSReader::readRaw(uint64_t offset, void* buffer, size_t size) {
    if (!diskImage.is_open()) {
        std::cerr << "Error: Disk image is not open." << std::endl;
        return false;
    }

    diskImage.seekg(offset, std::ios::beg);
    if (!diskImage.good()) {
        std::cerr << "Error: Failed to seek to offset " << offset << std::endl;
        return false;
    }

    diskImage.read(reinterpret_cast<char*>(buffer), size);

    if (diskImage.gcount() != static_cast<std::streamsize>(size)) {
        std::cerr << "Error: Failed to read " << size << " bytes from offset " << offset << std::endl;
        return false;
    }

    return true;
} 
