#pragma once
#include <cstdint>

// prevent structure alignment
#pragma pack(push, 1)

// NTFS Volume Boot Record (VBR) Structure
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

// MFT Entry Header Structure
struct MFT_ENTRY_HEADER {
    uint8_t signature[4]; // 0x00 - 0x03 'FILE' signature
    uint16_t fixup_offset; // 0x04 - 0x05 Offset to the fixup array
    uint16_t fixup_entry_count; // 0x06 - 0x07 Number of entries in the fixup array
    uint64_t logfile_sequence_number; // 0x08 - 0x0F $LogFile sequence number (LSN)
    uint16_t sequence_number; // 0x10 - 0x11 Sequence number
    uint16_t link_count; // 0x12 - 0x13 Link count
    uint16_t first_attr_offset; // 0x14 - 0x15 Offset to the first attribute
    uint16_t flags;      // 0x16 - 0x17 Flags
    uint32_t used_size;  // 0x18 - 0x1B Size of the used portion of the entry
    uint32_t allocated_size; // 0x1C - 0x1F Allocated size of MFT Entry
    uint64_t base_ref;   // 0x20 - 0x27 Reference to the base MFT entry if this is a fragment
    uint16_t next_attr_id; // 0x28 - 0x29 Next attribute ID
    uint16_t padding;    // 0x2A - 0x2B Padding for alignment
    uint32_t record_number; // 0x2C - 0x2F Record number of this MFT entry
};

// Attribute Header Structure
struct COMMON_ATTRIBUTE_HEADER {
    // Common attribute fields
    uint32_t type; // 0x00 - 0x03 Attribute type identifier
    uint32_t attribute_length; // 0x04 - 0x07 Length of the attribute
    uint8_t non_resident_flag; // 0x08 - 0x08 Non-resident flag (0 = resident, 1 = non-resident)
    uint8_t name_length; // 0x09 - 0x09 Length of the attribute name in characters
    uint16_t name_offset; // 0x0A - 0x0B Offset to the attribute name
    uint16_t flags; // 0x0C - 0x0D Attribute flags
    uint16_t attribute_id; // 0x0E - 0x0F Attribute identifier
};

struct RESIDENT_ATTRIBUTE_HEADER : public COMMON_ATTRIBUTE_HEADER {
    // Resident attribute specific fields
    uint32_t value_length; // 0x10 - 0x13 Length of the attribute value
    uint16_t value_offset; // 0x14 - 0x15 Offset to the attribute value
    uint8_t indexed_flag; // 0x16 - 0x16 Indexed flag
    uint8_t unused; // 0x17 - 0x17 Padding for alignment
};

struct NON_RESIDENT_ATTRIBUTE_HEADER : public COMMON_ATTRIBUTE_HEADER {
    // Non-resident attribute specific fields
    uint64_t starting_vcn; // 0x10 - 0x17 Starting VCN of the attribute
    uint64_t last_vcn; // 0x18 - 0x1F Last VCN of the attribute
    uint16_t data_run_offset; // 0x20 - 0x21 Offset to the data runs
    uint16_t compression_unit_size; // 0x22 - 0x23 Compression unit size
    uint32_t unused; // 0x24 - 0x27 Padding for alignment
    uint64_t allocated_size; // 0x28 - 0x2F Allocated size of the attribute
    uint64_t data_size; // 0x30 - 0x37 Actual size of the attribute data
    uint64_t initialized_size; // 0x38 - 0x3F Initialized size of the attribute data
};

#pragma pack(pop)
