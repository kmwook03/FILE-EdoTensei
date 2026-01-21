#include "disk_io.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <codecvt>
#include <locale>

/* --- Helper --- */

// Helper function: parse Data Runs
std::vector<MFT_Segment> NTFSReader::parseDataRuns(const uint8_t* runlist, size_t max_size) {
    std::vector<MFT_Segment> segments;
    size_t i = 0;
    int64_t last_lcn = 0; // For relative LCN calculation

    while (i < max_size && runlist[i] != 0x00) {
        uint8_t header = runlist[i++];
        uint8_t len_size = header & 0x0F; // Length field size
        uint8_t offset_size = (header >> 4) & 0x0F; // Offset field size

        // Read Cluster Count
        uint64_t cnt = 0;
        for (uint8_t j=0; j<len_size; ++j) cnt |= (static_cast<uint64_t>(runlist[i++]) << (j * 8));

        // Read LCN Offset
        int64_t offset = 0;
        for (uint8_t j=0; j<offset_size; ++j) offset |= (static_cast<uint64_t>(runlist[i++]) << (j * 8));

        // Sign extend
        if (offset_size > 0 && (offset & (1ULL << (offset_size * 8 - 1)))) {
            for (uint8_t j=offset_size; j<8; ++j) offset |= (0xFFULL << (j * 8));
        }

        last_lcn += offset;
        segments.push_back({static_cast<uint64_t>(last_lcn), cnt});
    }
    return segments;
}

// Helper function: Convert UTF-16LE to UTF-8
std::string NTFSReader::utf16_to_utf8(const std::vector<uint16_t>& utf16_vector) {
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
    std::u16string u16str(utf16_vector.begin(), utf16_vector.end());
    return convert.to_bytes(u16str);
}

void NTFSReader::scanBatch(uint64_t start_offset, uint64_t total_entries, uint32_t entry_size) {
    const uint32_t ENTRIES_PER_BATCH = 1024; // Read 1024 entries at a time (1 MB)
    uint32_t batch_size = ENTRIES_PER_BATCH * entry_size;
    std::vector<char> buffer(batch_size);

    for (uint64_t i=0; i<total_entries; i+=ENTRIES_PER_BATCH) {
        uint32_t current_batch_cnt = std::min((uint64_t)ENTRIES_PER_BATCH, total_entries - i);
        uint32_t current_read_size = current_batch_cnt * entry_size;

        if (!readRaw(start_offset + (i * entry_size), buffer.data(), current_read_size)) continue;

        for (uint32_t j=0; j<current_batch_cnt; ++j) {
            MFT_ENTRY_HEADER* entry_ptr = reinterpret_cast<MFT_ENTRY_HEADER*>(&buffer[j * entry_size]);

            if (entry_ptr->signature[0] == 'F' && entry_ptr->signature[1] == 'I' &&
                entry_ptr->signature[2] == 'L' && entry_ptr->signature[3] == 'E') {
                    
                if (!(entry_ptr->flags & 0x01)) {
                    uint64_t global_pos = start_offset + ((i + j) * entry_size);
                    std::cout << "[Found Deleted File] MFT Index: " << global_pos << (entry_ptr->flags & 0x02 ? " (Directory)" : " (File)") << std::endl;
                    parseAttributes(global_pos, *entry_ptr);
                }
            }
        }
    }
}

/* --- Method of NTFSReader --- */
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

uint64_t NTFSReader::findNTFSPartitionOffset() {
    NTFS_MBR mbr;

    if (!readRaw(0, &mbr, sizeof(NTFS_MBR))) return 0;

    if (mbr.signature != 0xAA55) {
        std::cerr << "Error: Invalid MBR signature." << std::endl;
        return 0;
    }

    // Search for NTFS type in partition table
    for (int i=0; i<4; ++i) {
        // 0x07 = NTFS
        if (mbr.partition[i].fs_type == 0x07) {
            return (uint64_t)mbr.partition[i].start_lba * 512; // assuming 512 bytes per sector
        }
    }

    return 0;
}

// Parse attributes of a given MFT entry
void NTFSReader::parseAttributes(uint64_t entry_pos, const MFT_ENTRY_HEADER& header) {
    uint32_t current_offset = header.first_attr_offset;

    while (current_offset < header.used_size) {
        COMMON_ATTRIBUTE_HEADER attr_header;
        if (!readRaw(entry_pos + current_offset, &attr_header, sizeof(COMMON_ATTRIBUTE_HEADER))) break;
        // End marker
        if (attr_header.type == 0xFFFFFFFF) break;
        // $FILE_NAME attribute
        if (attr_header.type == 0x30) {
            RESIDENT_ATTRIBUTE_HEADER resident_header;
            readRaw(entry_pos + current_offset, &resident_header, sizeof(RESIDENT_ATTRIBUTE_HEADER));

            uint64_t name_info_pos = entry_pos + current_offset + resident_header.value_offset;
            uint8_t name_length;
            readRaw(name_info_pos + 64, &name_length, 1);

            std::vector<uint16_t> unicode_name(name_length);
            readRaw(name_info_pos + 66, unicode_name.data(), name_length * 2);

            std::string file_name = utf16_to_utf8(unicode_name);
            std::cout << " - File Name: " << file_name << std::endl;
        }
        // Move to next attribute
        if (attr_header.attribute_length == 0) break;
        current_offset += attr_header.attribute_length;
    }
}

// Scan MFT entries for deleted files
void NTFSReader::scanDeletedFiles(uint64_t mft_offset, uint32_t entry_size) {
    std::cout << "\n--- Scanning for Deleted Files ---\n";

    MFT_ENTRY_HEADER entry_header;
    if (!readRaw(mft_offset, &entry_header, sizeof(MFT_ENTRY_HEADER))) return;

    uint64_t real_mft_size = 0;
    uint32_t attr_offset = entry_header.first_attr_offset;

    while (attr_offset + sizeof(COMMON_ATTRIBUTE_HEADER) <= entry_header.used_size) {
        COMMON_ATTRIBUTE_HEADER attr_header;
        readRaw(mft_offset + attr_offset, &attr_header, sizeof(COMMON_ATTRIBUTE_HEADER));
        if (attr_header.type == 0xFFFFFFFF) break;
        if (attr_header.type == 0x80) {
            NON_RESIDENT_ATTRIBUTE_HEADER data_attr;
            readRaw(mft_offset + attr_offset, &data_attr, sizeof(NON_RESIDENT_ATTRIBUTE_HEADER));
            real_mft_size = data_attr.data_size;
            break;
        }
        attr_offset += attr_header.attribute_length;
    }

    uint32_t total_entries = (real_mft_size > 0) ? static_cast<uint32_t>(real_mft_size / entry_size) : 10000;
    uint32_t safety_limit = total_entries + (total_entries / 10);
    
    std::cout << "Total MFT Entries to Scan: " << total_entries << std::endl;
    std::cout << "Safety Limit for Scanning: " << safety_limit << std::endl;

    // Buffering logic
    const uint32_t ENTRIES_PER_BATCH = 1024; // Read 1024 entries at a time (1 MB)
    uint32_t batch_size = ENTRIES_PER_BATCH * entry_size;
    std::vector<char> buffer(batch_size);

    uint32_t empty_batch_streak = 0;

    for (uint32_t i=0; i<total_entries; i+=ENTRIES_PER_BATCH) {
        uint32_t current_batch_cnt = std::min(ENTRIES_PER_BATCH, safety_limit - i);
        uint32_t current_read_size = current_batch_cnt * entry_size;

        if (!readRaw(mft_offset + (static_cast<uint64_t>(i) * entry_size), buffer.data(), current_read_size)) continue;

        bool found_valid_in_batch = false;

        for (uint32_t j=0; j<current_batch_cnt; ++j) {
            MFT_ENTRY_HEADER* entry_ptr = reinterpret_cast<MFT_ENTRY_HEADER*>(&buffer[j * entry_size]);

            if (entry_ptr->signature[0] == 'F' && entry_ptr->signature[1] == 'I' &&
                entry_ptr->signature[2] == 'L' && entry_ptr->signature[3] == 'E') {
                    found_valid_in_batch = true;
                    empty_batch_streak = 0;
                    if (!(entry_ptr->flags & 0x01)) {
                        uint32_t global_idx = i + j;
                        uint64_t global_pos = mft_offset + (static_cast<uint64_t>(global_idx) * entry_size);

                        std::cout << "[Found Deleted File] MFT Index: " << global_idx << (entry_ptr->flags & 0x02 ? " (Directory)" : " (File)") << std::endl;
                        parseAttributes(global_pos, *entry_ptr);
                    }
                }
        }
        if (!found_valid_in_batch) empty_batch_streak++;

        if (empty_batch_streak >= 5) {
            std::cout << "--- Scan reached end of valid MFT data ---\n";
            break;
        }
    }
}

void NTFSReader::scanAllMFTSegments(uint64_t partition_offset, uint64_t mft_base_offset, uint32_t bytes_per_cluster, uint32_t entry_size) {
    // read #0 MFT
    MFT_ENTRY_HEADER mft_self;

    if(!readRaw(mft_base_offset, &mft_self, sizeof(MFT_ENTRY_HEADER))) return;

    uint32_t attr_offset = mft_self.first_attr_offset;
    std::vector<MFT_Segment> mft_runs;

    // find $DATA
    while (attr_offset < mft_self.used_size) {
        COMMON_ATTRIBUTE_HEADER attr_header;
        readRaw(mft_base_offset + attr_offset, &attr_header, sizeof(COMMON_ATTRIBUTE_HEADER));
        if (attr_header.type == 0xFFFFFFFF) break;

        if (attr_header.type == 0x80) {
            NON_RESIDENT_ATTRIBUTE_HEADER nr_data;
            readRaw(mft_base_offset + attr_offset, &nr_data, sizeof(NON_RESIDENT_ATTRIBUTE_HEADER));

            std::vector<uint8_t> run_data(attr_header.attribute_length - nr_data.data_run_offset);
            readRaw(mft_base_offset + attr_offset + nr_data.data_run_offset, run_data.data(), run_data.size());

            mft_runs = parseDataRuns(run_data.data(), run_data.size());
            break;
        }
        attr_offset += attr_header.attribute_length;
    }

    // Batch scan each MFT segment
    for (const auto& run: mft_runs) {
        uint64_t run_start_byte = partition_offset + (run.lcn * bytes_per_cluster);
        uint64_t run_total_entries = (run.length * bytes_per_cluster) / entry_size;

        std::cout << "Scanning MFT Run: LCN " << run.lcn << " (Entries: " << run_total_entries << ")\n";

        scanBatch(run_start_byte, run_total_entries, entry_size);
    }
}
