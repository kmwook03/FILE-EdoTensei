#ifndef DISK_IO_HPP
#define DISK_IO_HPP

#pragma once
#include "ntfs_structure.hpp"
#include <string>
#include <vector>
#include <fstream>

struct MFT_Segment {
    uint64_t lcn; // Logical Cluster Number
    uint64_t length; // Number of clusters
};

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
    // Read MBR and get partition offset
    uint64_t findNTFSPartitionOffset();
    // Parse Attributes
    void parseAttributes(uint64_t entry_pos, const MFT_ENTRY_HEADER& header);
    // Scan MFT for deleted files
    void scanDeletedFiles(uint64_t mft_offset, uint32_t entry_size);
    // Scan all MFT segments
    void scanAllMFTSegments(uint64_t partition_offset, uint64_t mft_base_offset, uint32_t bytes_per_cluster, uint32_t entry_size);

    private:
    // Disk image file stream
    std::ifstream diskImage;

    // Helper function: Parse Data Runs
    std::vector<MFT_Segment> parseDataRuns(const uint8_t* runlist, size_t max_size);
    // Helper funtion: Scan a batch of MFT entries
    void scanBatch(uint64_t start_offset, uint64_t total_entries, uint32_t entry_size);
    // Helper function: Convert UTF-16 to UTF-8
    std::string utf16_to_utf8(const std::vector<uint16_t>& utf16_vector);
};

#endif // DISK_IO_HPP
