#include "../include/disk_io.hpp"
#include <iostream>

int main() {
    NTFSReader reader;

    // Open disk image
    if (!reader.openImage("/mnt/c/Users/kmwook/Desktop/diskImage.001")) return 1;
    
    // Find NTFS partition offset
    uint64_t partition_offset = reader.findNTFSPartitionOffset();

    if (partition_offset != 0) {
        std::cout << "NTFS partition found at offset: " << partition_offset << std::endl;
        NTFS_VBR vbr;
        if (reader.readRaw(partition_offset, &vbr, sizeof(NTFS_VBR))) {
            // Calculate MFT offset and entry size
            uint64_t mft_offset = partition_offset + (vbr.mft_lcn * vbr.sectors_per_cluster * vbr.bytes_per_sector);
            uint32_t entry_size = (vbr.mft_record_size_raw < 0) ? (1 << (-vbr.mft_record_size_raw)) : (vbr.mft_record_size_raw * vbr.sectors_per_cluster * vbr.bytes_per_sector);
            uint32_t bytes_per_cluster = vbr.sectors_per_cluster * vbr.bytes_per_sector;
            std::cout << "MFT starts at offset: " << mft_offset << " (Entry size: " << entry_size << ")" << std::endl;
            
            // Scan for deleted files in MFT
            reader.scanAllMFTSegments(partition_offset, mft_offset, bytes_per_cluster, entry_size);
        }
    } else {
        std::cerr << "Error: No NTFS partition found." << std::endl;
        reader.closeImage();
    }
    reader.closeImage();

    return 0;
}
