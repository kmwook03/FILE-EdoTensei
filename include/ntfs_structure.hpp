#pragma once
#include <cstdint>

// prevent structure alignment
#pragma pack(push, 1)

struct NTFS_VBR {
    // Jump instruction and OEM ID
    uint8_t jmp[3]; // 0x00 - 0x02 Jump instruction
    char oem_id[8]; // 0x03 - 0x0A System(OEM) ID (ASCII)

    // BIOS Parameter Block (BPB)
    uint16_t bytes_per_sector; // 0x0B - 0x0C Bytes per sector (Default: 512)
    uint8_t sectors_per_cluster; // 0x0D - 0x0D Sectors per cluster
    uint16_t reserved_sectors; // 0x0E - 0x0F Reserved sectors (Always 0 for NTFS)
    uint8_t zero1[3]; // 0x10 - 0x12 Always 0
    uint16_t unused1; // 0x13 - 0x14 Unused
    uint8_t media_descriptor; // 0x15 - 0x15 Media descriptor (0xF8 for fixed HDD)
    uint16_t zero2; // 0x16 - 0x17 Always 0
    uint16_t sectors_per_track; // 0x18 - 0x19 Sector per track
    uint16_t num_heads; // 0x1A - 0x1B Number of heads
    uint32_t hidden_sectors; // 0x1C - 0x1F Hidden sectors
    uint32_t unused2; // 0x20 - 0x23 Unused

    // Extended BPB for NTFS
    uint32_t unused3; // 0x24 - 0x27 Unused
    uint64_t total_sectors; // 0x28 - 0x2F Total sectors
    uint64_t mft_lcn; // 0x30 - 0x37 Logical cluster number for the $MFT (Start address)
    uint64_t mft_mirr_lcn; // 0x38 - 0x3F Logical cluster number for the $MFTMirr (Start address)
    
    // Other NTFS specific fields
    // if the value is negative, it indicates clusters per file record segment as a power of 2
    int8_t mft_record_size_raw; // 0x40 - 0x40 Size of MFT record in clusters (-10 -> 1024 bytes)
    uint8_t reserved1[3]; // 0x41 - 0x43 Padding
    int8_t index_buffer_size_raw; // 0x44 - 0x44 Size of index buffer in clusters
    uint8_t reserved2[3]; // 0x45 - 0x47 Padding

    uint64_t volume_serial_number; // 0x48 - 0x4F Volume serial number
    uint32_t checksum; // 0x50 - 0x53 Checksum of the boot sector, ignored by NTFS

    uint8_t boot_code[426]; // 0x54 - 0x1FD Boot code and error message
    uint16_t signature; // 0x1FE - 0x1FF Signature (0x55AA)  
};

#pragma pack(pop)
