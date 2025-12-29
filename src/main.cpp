#include "../include/disk_io.hpp"
#include <iostream>

int main() {
    NTFSReader reader;
    NTFS_VBR vbr;

    if (reader.openImage("disk_image.img")) {
        if (reader.readVBR(vbr)) {
            // check OEM ID
            std::cout << "OEM ID: ";
            for (int i=0; i<8; ++i) std::cout << vbr.oem_id[i];
            std::cout << std::endl;

            std::cout << "Bytes per Sector: " << vbr.bytes_per_sector << std::endl;
            std::cout << "Sectors per Cluster: " << (int)vbr.sectors_per_cluster << std::endl;
            std::cout << "MFT Start LCN: " << vbr.mft_lcn << std::endl;
        }
        reader.closeImage();
    }
    return 0;
}