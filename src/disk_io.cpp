#include "disk_io.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace {
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}
bool addMul(uint64_t base, uint64_t value, uint64_t multiplier, uint64_t& result) {
    if (value != 0 && multiplier > (std::numeric_limits<uint64_t>::max() - base) / value) return false;
    result = base + value * multiplier;
    return true;
}
std::string formatFromBytes(const uint8_t* data, size_t size) {
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return "jpeg";
    const uint8_t png[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (size >= sizeof(png) && std::equal(std::begin(png), std::end(png), data)) return "png";
    if (size >= 5 && std::equal(data, data + 5, reinterpret_cast<const uint8_t*>("%PDF-"))) return "pdf";
    return {};
}
std::string simpleUtf16Name(const uint8_t* data, size_t characters) {
    std::string output;
    for (size_t i = 0; i < characters; ++i) {
        const uint16_t value = le16(data + i * 2);
        if (value >= 0x20 && value < 0x7f) output.push_back(static_cast<char>(value));
        else output.push_back('_');
    }
    return output;
}
bool readExtentStream(const ImageReader& image, const std::vector<Extent>& extents,
                      uint64_t logicalOffset, void* destination, size_t size,
                      std::string& error) {
    auto* output = static_cast<uint8_t*>(destination);
    size_t completed = 0;
    uint64_t skip = logicalOffset;
    for (const auto& extent : extents) {
        if (skip >= extent.length) { skip -= extent.length; continue; }
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(
            size - completed, extent.length - skip));
        if (extent.sparse) std::fill(output + completed, output + completed + amount, 0);
        else if (!image.readExact(extent.offset + skip, output + completed, amount, error)) return false;
        completed += amount;
        skip = 0;
        if (completed == size) return true;
    }
    error = "MFT extent stream ended early";
    return false;
}
}

bool NTFSReader::openImage(const std::string& path, std::string& error) {
    detected_ = false;
    return image_.open(path, error);
}

bool NTFSReader::parseBootSector(uint64_t offset, std::string& error) {
    std::array<uint8_t, 512> boot{};
    if (!image_.readExact(offset, boot.data(), boot.size(), error)) return false;
    if (!std::equal(boot.begin() + 3, boot.begin() + 11,
                    reinterpret_cast<const uint8_t*>("NTFS    ")) ||
        boot[510] != 0x55 || boot[511] != 0xaa) return false;
    const uint16_t sector = le16(boot.data() + 11);
    const uint8_t sectorsPerCluster = boot[13];
    if (sector < 256 || sector > 4096 || (sector & (sector - 1)) != 0 ||
        sectorsPerCluster == 0 || (sectorsPerCluster & (sectorsPerCluster - 1)) != 0) {
        error = "invalid NTFS sector or cluster size"; return false;
    }
    const uint64_t cluster = static_cast<uint64_t>(sector) * sectorsPerCluster;
    if (cluster > 2 * 1024 * 1024) { error = "unsupported NTFS cluster size"; return false; }
    const int8_t rawRecordSize = static_cast<int8_t>(boot[64]);
    uint64_t recordSize = 0;
    if (rawRecordSize < 0) {
        const unsigned shift = static_cast<unsigned>(-rawRecordSize);
        if (shift >= 32) { error = "invalid NTFS record size"; return false; }
        recordSize = 1ULL << shift;
    } else recordSize = cluster * static_cast<uint8_t>(rawRecordSize);
    if (recordSize < sector || recordSize > 64 * 1024 || recordSize % sector != 0) {
        error = "unsupported NTFS record size"; return false;
    }
    uint64_t mft = 0;
    if (!addMul(offset, le64(boot.data() + 48), cluster, mft) || mft >= image_.size()) {
        error = "NTFS MFT offset overflow or outside input"; return false;
    }
    partitionOffset_ = offset;
    mftOffset_ = mft;
    bytesPerCluster_ = static_cast<uint32_t>(cluster);
    recordSize_ = static_cast<uint32_t>(recordSize);
    bytesPerSector_ = sector;
    detected_ = true;
    mftExtents_.clear();
    return true;
}

bool NTFSReader::detectVolume(std::string& error) {
    error.clear();
    if (parseBootSector(0, error)) return true;
    error.clear();
    std::array<uint8_t, 512> mbr{};
    if (!image_.readExact(0, mbr.data(), mbr.size(), error)) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) { error = "no NTFS VBR or valid MBR"; return false; }
    for (size_t i = 0; i < 4; ++i) {
        const size_t entry = 446 + i * 16;
        if (mbr[entry + 4] != 0x07) continue;
        const uint64_t offset = static_cast<uint64_t>(le32(mbr.data() + entry + 8)) * 512;
        if (parseBootSector(offset, error)) return true;
    }
    if (error.empty()) error = "no supported NTFS partition found";
    return false;
}

bool NTFSReader::applyFixups(std::vector<uint8_t>& record, uint16_t bytesPerSector,
                             std::string& error) {
    if (record.size() < 8 || bytesPerSector == 0 || record.size() % bytesPerSector != 0) {
        error = "invalid record/fixup geometry"; return false;
    }
    const uint16_t offset = le16(record.data() + 4);
    const uint16_t count = le16(record.data() + 6);
    const size_t sectors = record.size() / bytesPerSector;
    if (count != sectors + 1 || offset > record.size() ||
        static_cast<size_t>(count) * 2 > record.size() - offset) {
        error = "invalid update sequence array"; return false;
    }
    const uint16_t sequence = le16(record.data() + offset);
    for (size_t i = 0; i < sectors; ++i) {
        const size_t tail = (i + 1) * bytesPerSector - 2;
        if (le16(record.data() + tail) != sequence) { error = "MFT fixup mismatch"; return false; }
        record[tail] = record[offset + 2 + i * 2];
        record[tail + 1] = record[offset + 3 + i * 2];
    }
    return true;
}

bool NTFSReader::parseDataRuns(const uint8_t* data, size_t size,
                               std::vector<MFT_Segment>& segments, std::string& error) {
    segments.clear();
    size_t position = 0;
    int64_t currentLcn = 0;
    while (position < size && data[position] != 0) {
        const uint8_t header = data[position++];
        const uint8_t lengthBytes = header & 0x0f;
        const uint8_t offsetBytes = header >> 4;
        if (lengthBytes == 0 || lengthBytes > 8 || offsetBytes > 8 ||
            lengthBytes + offsetBytes > size - position) {
            error = "malformed NTFS data run"; return false;
        }
        uint64_t length = 0;
        for (uint8_t i = 0; i < lengthBytes; ++i) length |= static_cast<uint64_t>(data[position++]) << (i * 8);
        if (length == 0) { error = "zero-length NTFS data run"; return false; }
        if (offsetBytes == 0) { segments.push_back({0, length, true}); continue; }
        uint64_t raw = 0;
        for (uint8_t i = 0; i < offsetBytes; ++i) raw |= static_cast<uint64_t>(data[position++]) << (i * 8);
        if (offsetBytes < 8 && (raw & (1ULL << (offsetBytes * 8 - 1)))) raw |= (~0ULL) << (offsetBytes * 8);
        const int64_t delta = static_cast<int64_t>(raw);
        if ((delta > 0 && currentLcn > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && currentLcn < std::numeric_limits<int64_t>::min() - delta)) {
            error = "NTFS data run LCN overflow"; return false;
        }
        currentLcn += delta;
        if (currentLcn < 0) { error = "negative NTFS data run LCN"; return false; }
        segments.push_back({static_cast<uint64_t>(currentLcn), length, false});
    }
    if (position >= size) { error = "unterminated NTFS data run"; return false; }
    return true;
}

bool NTFSReader::parseRecord(uint64_t diskOffset, std::vector<uint8_t>& record,
                             RecoveryCandidate& candidate, std::string& warning) {
    if (record.size() < 48 || !std::equal(record.begin(), record.begin() + 4,
                                          reinterpret_cast<const uint8_t*>("FILE"))) return false;
    if ((le16(record.data() + 22) & 0x01) != 0 || (le16(record.data() + 22) & 0x02) != 0) return false;
    std::string error;
    if (!applyFixups(record, bytesPerSector_, error)) { warning = error; return false; }
    if (le64(record.data() + 32) != 0) { warning = "attribute-list-dependent MFT extension record"; return false; }
    const uint32_t used = le32(record.data() + 24);
    size_t position = le16(record.data() + 20);
    if (used > record.size() || position >= used) return false;
    uint64_t logicalSize = 0;
    std::array<uint8_t, 8> prefix{};
    size_t prefixSize = 0;
    while (position + 16 <= used) {
        const uint32_t type = le32(record.data() + position);
        if (type == 0xffffffffU) break;
        const uint32_t length = le32(record.data() + position + 4);
        if (length < 16 || length > used - position) { warning = "malformed MFT attribute"; return false; }
        const bool nonResident = record[position + 8] != 0;
        const uint16_t flags = le16(record.data() + position + 12);
        const uint8_t nameLength = record[position + 9];
        if (type == 0x30 && !nonResident && length >= 24) {
            const uint32_t valueLength = le32(record.data() + position + 16);
            const uint16_t valueOffset = le16(record.data() + position + 20);
            if (valueOffset <= length && valueLength <= length - valueOffset && valueLength >= 66) {
                const uint8_t characters = record[position + valueOffset + 64];
                if (66ULL + characters * 2ULL <= valueLength)
                    candidate.sourceName = simpleUtf16Name(record.data() + position + valueOffset + 66, characters);
            }
        }
        if (type == 0x80 && nameLength == 0) {
            if (flags & 0x4001) { warning = "compressed or encrypted NTFS data is unsupported"; return false; }
            if (!nonResident) {
                if (length < 24) return false;
                const uint32_t valueLength = le32(record.data() + position + 16);
                const uint16_t valueOffset = le16(record.data() + position + 20);
                if (valueOffset > length || valueLength > length - valueOffset) return false;
                logicalSize = valueLength;
                const uint64_t valueDiskOffset = diskOffset + position + valueOffset;
                candidate.extents = {{valueDiskOffset, valueLength, false}};
                candidate.residentData.assign(record.begin() + position + valueOffset,
                                              record.begin() + position + valueOffset + valueLength);
                prefixSize = std::min<size_t>(prefix.size(), valueLength);
                std::copy_n(record.data() + position + valueOffset, prefixSize, prefix.begin());
            } else {
                if (length < 64) return false;
                const uint16_t runOffset = le16(record.data() + position + 32);
                logicalSize = le64(record.data() + position + 48);
                if (runOffset >= length) return false;
                std::vector<MFT_Segment> runs;
                if (!parseDataRuns(record.data() + position + runOffset, length - runOffset, runs, warning)) return false;
                uint64_t remaining = logicalSize;
                for (const auto& run : runs) {
                    if (run.length > std::numeric_limits<uint64_t>::max() / bytesPerCluster_) return false;
                    const uint64_t bytes = std::min(remaining, run.length * bytesPerCluster_);
                    uint64_t offset = 0;
                    if (!run.sparse && !addMul(partitionOffset_, run.lcn, bytesPerCluster_, offset)) return false;
                    candidate.extents.push_back({offset, bytes, run.sparse});
                    remaining -= bytes;
                    if (remaining == 0) break;
                }
                if (remaining != 0) { warning = "NTFS runs shorter than logical data size"; return false; }
                if (!candidate.extents.empty() && !candidate.extents[0].sparse) {
                    std::string readError;
                    size_t got = 0;
                    image_.read(candidate.extents[0].offset, prefix.data(), prefix.size(), got, readError);
                    prefixSize = got;
                }
            }
        }
        position += length;
    }
    candidate.formatId = formatFromBytes(prefix.data(), prefixSize);
    if (candidate.formatId.empty() || logicalSize == 0 || candidate.extents.empty()) return false;
    candidate.method = RecoveryMethod::NtfsMetadata;
    candidate.startOffset = candidate.extents.front().offset;
    candidate.evidence.push_back({"ntfs_deleted_record", 25, "deleted MFT record"});
    candidate.evidence.push_back({"ntfs_extent_consistency", 20, std::to_string(logicalSize) + " bytes"});
    if (candidate.extents.size() > 1) candidate.warnings.push_back("fragmented metadata extent validation is limited");
    return true;
}

std::vector<RecoveryCandidate> NTFSReader::findDeletedFiles(std::string& error) {
    std::vector<RecoveryCandidate> candidates;
    if (!detected_) { error = "NTFS volume was not detected"; return candidates; }
    std::vector<uint8_t> record(recordSize_);
    if (!image_.readExact(mftOffset_, record.data(), record.size(), error)) return candidates;
    std::vector<uint8_t> mftHeader = record;
    uint64_t mftLogicalSize = image_.size() - mftOffset_;
    std::string fixupError;
    if (std::equal(mftHeader.begin(), mftHeader.begin() + 4,
                   reinterpret_cast<const uint8_t*>("FILE")) &&
        applyFixups(mftHeader, bytesPerSector_, fixupError)) {
        const uint32_t used = le32(mftHeader.data() + 24);
        size_t attribute = le16(mftHeader.data() + 20);
        while (attribute + 16 <= used && used <= mftHeader.size()) {
            const uint32_t type = le32(mftHeader.data() + attribute);
            if (type == 0xffffffffU) break;
            const uint32_t length = le32(mftHeader.data() + attribute + 4);
            if (length < 16 || length > used - attribute) break;
            if (type == 0x80 && mftHeader[attribute + 8] != 0 &&
                mftHeader[attribute + 9] == 0 && length >= 64) {
                const uint16_t runOffset = le16(mftHeader.data() + attribute + 32);
                mftLogicalSize = le64(mftHeader.data() + attribute + 48);
                std::vector<MFT_Segment> runs;
                if (runOffset < length && parseDataRuns(mftHeader.data() + attribute + runOffset,
                                                        length - runOffset, runs, error)) {
                    uint64_t remaining = mftLogicalSize;
                    for (const auto& run : runs) {
                        if (run.sparse || run.length > std::numeric_limits<uint64_t>::max() / bytesPerCluster_) {
                            mftExtents_.clear(); break;
                        }
                        const uint64_t bytes = std::min(remaining, run.length * bytesPerCluster_);
                        uint64_t offset = 0;
                        if (!addMul(partitionOffset_, run.lcn, bytesPerCluster_, offset)) {
                            mftExtents_.clear(); break;
                        }
                        mftExtents_.push_back({offset, bytes, false});
                        remaining -= bytes;
                        if (remaining == 0) break;
                    }
                    if (remaining != 0) mftExtents_.clear();
                }
                break;
            }
            attribute += length;
        }
    }
    if (mftExtents_.empty()) mftExtents_.push_back({mftOffset_, mftLogicalSize, false});
    unsigned emptyStreak = 0;
    constexpr uint64_t maximumRecords = 1000000;
    const uint64_t recordCount = std::min<uint64_t>(maximumRecords, mftLogicalSize / recordSize_);
    for (uint64_t index = 0; index < recordCount && emptyStreak < 256; ++index) {
        const uint64_t logicalOffset = index * recordSize_;
        if (!readExtentStream(image_, mftExtents_, logicalOffset, record.data(), record.size(), error)) break;
        if (!std::equal(record.begin(), record.begin() + 4,
                        reinterpret_cast<const uint8_t*>("FILE"))) { ++emptyStreak; continue; }
        emptyStreak = 0;
        RecoveryCandidate candidate;
        std::string warning;
        uint64_t physicalOffset = mftOffset_ + logicalOffset;
        uint64_t skip = logicalOffset;
        for (const auto& extent : mftExtents_) {
            if (skip < extent.length) { physicalOffset = extent.offset + skip; break; }
            skip -= extent.length;
        }
        if (parseRecord(physicalOffset, record, candidate, warning)) candidates.push_back(std::move(candidate));
    }
    error.clear();
    return candidates;
}
