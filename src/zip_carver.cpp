#include "format_carver.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <utility>

namespace {
constexpr uint32_t localSignature = 0x04034b50;
constexpr uint32_t centralSignature = 0x02014b50;
constexpr uint32_t endSignature = 0x06054b50;
constexpr uint32_t zip64Signature = 0x06064b50;
constexpr uint32_t locatorSignature = 0x07064b50;
constexpr uint32_t descriptorSignature = 0x08074b50;
constexpr uint32_t sentinel32 = 0xffffffffU;
constexpr size_t blockSize = 64 * 1024;

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t le32(const uint8_t* p) {
    return le16(p) | (static_cast<uint32_t>(le16(p + 2)) << 16);
}
uint64_t le64(const uint8_t* p) {
    return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

struct ZipEntry {
    uint64_t offset = 0;
    uint64_t data = 0;
    uint64_t packed = 0;
    uint64_t unpacked = 0;
    uint32_t crc = 0;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint16_t version = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> name;
    bool zip64 = false;
    bool verified = false;
    bool matched = false;
};

// Walk records from offset zero. Declared offsets are relative to this candidate,
// so an interior local header cannot become a second recovered archive.
class ZipParser {
public:
    ZipParser(const CandidateReader& reader, ZipLimits limits)
        : reader_(reader), limits_(limits), limit_(std::min(reader.size(), limits.maximumArchiveSize)) {}

    ValidationResult run() {
        if (!parse()) return result_;
        result_.state = ValidationState::Valid;
        result_.validatedLength = position_;
        result_.evidence = {{"zip_records", 30, "local headers, directory and end records agree"},
                            {"zip_crc_and_lengths", 30, std::to_string(entries_.size()) + " entries verified"},
                            {"zip_streams", 20, "Store/Deflate verified; original archive bytes preserved"}};
        return result_;
    }

private:
    bool fail(const std::string& message) {
        result_.diagnostics.push_back(message);
        return false;
    }
    bool limited(const char* reason) {
        result_.state = ValidationState::NotValidated;
        result_.stopReason = reason;
        return fail(reason);
    }
    bool check() {
        return !reader_.budget() || reader_.budget()->check();
    }
    bool read(uint64_t offset, void* data, size_t size) {
        if (!check()) return false;
        if (offset > limit_ || size > limit_ - offset) {
            if (limit_ < reader_.size()) return limited("zip_archive_size_limit");
            result_.truncationReason = "zip_end_of_input";
            return fail("zip_truncated_record_or_data");
        }
        std::string error;
        return reader_.readExact(offset, data, size, error) || fail(error);
    }
    bool signature(uint32_t& value) {
        std::array<uint8_t, 4> bytes{};
        if (!read(position_, bytes.data(), bytes.size())) return false;
        value = le32(bytes.data());
        return true;
    }
    bool supported(uint16_t flags, uint16_t method, uint16_t version) {
        if (flags & (1U | 0x40U | 0x2000U)) return fail("zip_unsupported_encryption");
        if (method != 0 && method != 8) return fail("zip_unsupported_compression_method_" + std::to_string(method));
        if (flags & ~0x080eU) return fail("zip_unsupported_flags");
        if (method == 0 && (flags & 6U)) return fail("zip_invalid_store_flags");
        if (version > 45) return fail("zip_unsupported_version");
        if (version < (method == 8 ? 20 : 10)) return fail("zip_invalid_version");
        return true;
    }

    // ZIP64 values appear only for sentinel fields, in their specified order.
    bool extra(const std::vector<uint8_t>& bytes, bool needUnpacked, bool needPacked,
               bool needOffset, bool needDisk, uint64_t& unpacked, uint64_t& packed,
               uint64_t& offset, uint32_t& disk, bool& hasZip64) {
        hasZip64 = false;
        size_t position = 0;
        while (position < bytes.size()) {
            if (bytes.size() - position < 4) return fail("zip_malformed_extra_field");
            const uint16_t id = le16(bytes.data() + position);
            const size_t length = le16(bytes.data() + position + 2);
            position += 4;
            if (length > bytes.size() - position) return fail("zip_malformed_extra_field");
            if (id == 1) {
                if (hasZip64) return fail("zip_duplicate_zip64_extra");
                hasZip64 = true;
                size_t within = 0;
                auto take64 = [&](bool needed, uint64_t& value) {
                    if (!needed) return true;
                    if (length - within < 8) return false;
                    value = le64(bytes.data() + position + within);
                    within += 8;
                    return true;
                };
                if (!take64(needUnpacked, unpacked) || !take64(needPacked, packed) ||
                    !take64(needOffset, offset)) return fail("zip_missing_zip64_value");
                if (needDisk) {
                    if (length - within < 4) return fail("zip_missing_zip64_disk");
                    disk = le32(bytes.data() + position + within);
                }
            }
            position += length;
        }
        if ((needUnpacked || needPacked || needOffset || needDisk) && !hasZip64)
            return fail("zip_missing_zip64_extra");
        return true;
    }

    bool descriptorMatches(const uint8_t* bytes, bool wide, bool signedRecord,
                           uint32_t crc, uint64_t packed, uint64_t unpacked) const {
        if (signedRecord) {
            if (le32(bytes) != descriptorSignature) return false;
            bytes += 4;
        }
        if (le32(bytes) != crc) return false;
        return wide ? le64(bytes + 4) == packed && le64(bytes + 12) == unpacked
                    : packed <= sentinel32 && unpacked <= sentinel32 &&
                      le32(bytes + 4) == packed && le32(bytes + 8) == unpacked;
    }

    bool descriptor(const ZipEntry& entry) {
        // Try both spellings even if a CRC happens to equal the optional signature.
        const size_t base = entry.zip64 ? 20 : 12;
        std::array<uint8_t, 24> bytes{};
        if (!read(position_, bytes.data(), base)) return false;
        if (descriptorMatches(bytes.data(), entry.zip64, false, entry.crc, entry.packed, entry.unpacked)) {
            position_ += base;
            return true;
        }
        if (!read(position_, bytes.data(), base + 4)) return false;
        if (!descriptorMatches(bytes.data(), entry.zip64, true, entry.crc, entry.packed, entry.unpacked))
            return fail("zip_data_descriptor_mismatch");
        position_ += base + 4;
        return true;
    }

    // Store has no stream terminator. Search bounded windows for a descriptor
    // followed by another record, checking its length and the running CRC.
    bool storedDescriptor(ZipEntry& entry) {
        std::array<uint8_t, blockSize + 28> buffer{};
        const size_t base = entry.zip64 ? 20 : 12;
        uint64_t consumed = 0;
        uLong crc = crc32(0, Z_NULL, 0);
        while (entry.data + consumed < limit_) {
            if (!check()) return false;
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(blockSize, limit_ - entry.data - consumed));
            const size_t available = static_cast<size_t>(std::min<uint64_t>(amount + 28, limit_ - entry.data - consumed));
            if (!read(entry.data + consumed, buffer.data(), available)) return false;
            size_t hashed = 0;
            for (size_t i = 0; i < amount; ++i) {
                // Size fields eliminate almost all positions without updating CRC byte by byte.
                for (bool signedRecord : {false, true}) {
                    const size_t length = base + (signedRecord ? 4 : 0);
                    if (i + length + 4 > available) continue;
                    const uint32_t next = le32(buffer.data() + i + length);
                    if (next != localSignature && next != centralSignature) continue;
                    const auto* fields = buffer.data() + i + (signedRecord ? 4 : 0);
                    const uint64_t size = consumed + i;
                    if (entry.zip64 ? le64(fields + 4) != size || le64(fields + 12) != size
                                    : size > sentinel32 || le32(fields + 4) != size || le32(fields + 8) != size) continue;
                    crc = crc32(crc, buffer.data() + hashed, static_cast<uInt>(i - hashed));
                    hashed = i;
                    if (!descriptorMatches(buffer.data() + i, entry.zip64, signedRecord,
                                           static_cast<uint32_t>(crc), size, size)) continue;
                    if (size > limits_.maximumUncompressedSize - totalUnpacked_)
                        return limited("zip_uncompressed_size_limit");
                    entry.packed = entry.unpacked = size;
                    entry.crc = static_cast<uint32_t>(crc);
                    entry.verified = true;
                    position_ = entry.data + size; // descriptor() checks and advances it
                    return true;
                }
            }
            crc = crc32(crc, buffer.data() + hashed, static_cast<uInt>(amount - hashed));
            consumed += amount;
            if (consumed > limits_.maximumUncompressedSize - totalUnpacked_)
                return limited("zip_uncompressed_size_limit");
        }
        if (limit_ < reader_.size()) return limited("zip_archive_size_limit");
        result_.truncationReason = "zip_end_of_input";
        return fail("zip_missing_store_descriptor");
    }

    bool stream(ZipEntry& entry, bool unknown) {
        std::array<uint8_t, blockSize> input{}, output{};
        if (entry.data > limit_) return fail("zip_data_outside_candidate");
        const uint64_t available = limit_ - entry.data;
        if (!unknown && entry.packed > available) {
            if (limit_ < reader_.size()) return limited("zip_archive_size_limit");
            result_.truncationReason = "zip_end_of_input";
            return fail("zip_truncated_entry_data");
        }
        uint64_t remaining = unknown ? available : entry.packed;
        uint64_t loaded = 0, consumed = 0, produced = 0;
        uLong crc = crc32(0, Z_NULL, 0);
        z_stream state{};
        struct InflateGuard {
            z_stream* state;
            bool active;
            ~InflateGuard() { if (active) inflateEnd(state); }
        } guard{&state, false};
        if (entry.method == 8) {
            if (inflateInit2(&state, -MAX_WBITS) != Z_OK) return fail("zip_inflate_initialization_failed");
            guard.active = true;
        }
        for (;;) {
            if (!check()) return false;
            if (state.avail_in == 0 && remaining > 0) {
                const size_t amount = static_cast<size_t>(std::min<uint64_t>(input.size(), remaining));
                if (!read(entry.data + loaded, input.data(), amount)) return false;
                loaded += amount;
                remaining -= amount;
                state.next_in = input.data();
                state.avail_in = static_cast<uInt>(amount);
            }
            int status = Z_OK;
            size_t amount = 0;
            if (entry.method == 0) {
                amount = state.avail_in;
                consumed += amount;
                crc = crc32(crc, input.data(), static_cast<uInt>(amount));
                state.avail_in = 0;
            } else {
                state.next_out = output.data();
                state.avail_out = static_cast<uInt>(output.size());
                const uInt before = state.avail_in;
                status = inflate(&state, Z_NO_FLUSH);
                consumed += before - state.avail_in;
                amount = output.size() - state.avail_out;
                crc = crc32(crc, output.data(), static_cast<uInt>(amount));
                if (unknown && status == Z_BUF_ERROR && remaining == 0 && state.avail_in == 0 &&
                    limit_ < reader_.size()) return limited("zip_archive_size_limit");
                if (status != Z_OK && status != Z_STREAM_END) return fail("zip_invalid_deflate_stream");
                if (status != Z_STREAM_END && amount == 0 && before == state.avail_in)
                    return fail("zip_truncated_deflate_stream");
            }
            if (amount > limits_.maximumUncompressedSize - totalUnpacked_ - produced)
                return limited("zip_uncompressed_size_limit");
            produced += amount;
            if (!unknown && produced > entry.unpacked) return fail("zip_uncompressed_length_mismatch");
            if (entry.method == 8 ? status == Z_STREAM_END : remaining == 0) break;
        }
        if (unknown) {
            entry.packed = consumed;
            entry.unpacked = produced;
            entry.crc = static_cast<uint32_t>(crc);
        } else if (consumed != entry.packed || produced != entry.unpacked || crc != entry.crc) {
            return fail("zip_crc_or_length_mismatch");
        }
        entry.verified = true;
        return true;
    }

    bool local() {
        if (entries_.size() >= limits_.maximumEntries) return limited("zip_entry_limit");
        ZipEntry entry;
        entry.offset = position_;
        std::array<uint8_t, 30> header{};
        if (!read(position_, header.data(), header.size())) return false;
        entry.version = le16(header.data() + 4);
        entry.flags = le16(header.data() + 6);
        entry.method = le16(header.data() + 8);
        entry.timestamp = le32(header.data() + 10);
        entry.crc = le32(header.data() + 14);
        entry.packed = le32(header.data() + 18);
        entry.unpacked = le32(header.data() + 22);
        if (!supported(entry.flags, entry.method, entry.version)) return false;
        const size_t nameSize = le16(header.data() + 26), extraSize = le16(header.data() + 28);
        const uint64_t metadataSize = header.size() + nameSize + extraSize;
        if (metadataSize > limits_.maximumDirectorySize - localMetadata_)
            return limited("zip_directory_size_limit");
        localMetadata_ += metadataSize;
        position_ += header.size();
        entry.name.resize(nameSize);
        std::vector<uint8_t> extraBytes(extraSize);
        if (!read(position_, entry.name.data(), nameSize)) return false;
        position_ += nameSize;
        if (!read(position_, extraBytes.data(), extraSize)) return false;
        position_ += extraSize;
        uint64_t unusedOffset = 0;
        uint32_t unusedDisk = 0;
        const bool widePacked = entry.packed == sentinel32, wideUnpacked = entry.unpacked == sentinel32;
        if (!extra(extraBytes, wideUnpacked, widePacked, false, false, entry.unpacked, entry.packed,
                   unusedOffset, unusedDisk, entry.zip64)) return false;
        if (entry.zip64 && entry.version < 45) return fail("zip_invalid_zip64_version");
        entry.data = position_;
        const uint64_t declaredPacked = entry.packed, declaredUnpacked = entry.unpacked;
        const uint32_t declaredCrc = entry.crc;
        if (entry.flags & 8) {
            if (entry.method == 8) {
                if (!stream(entry, true)) return false;
                position_ += entry.packed;
            } else if (declaredPacked != 0) {
                if (entry.packed != entry.unpacked) return fail("zip_store_length_mismatch");
                // CRC is allowed to be zero in the local header when bit 3 is set.
                entry.crc = 0;
                if (!storedKnown(entry)) return false;
                position_ += entry.packed;
            } else if (!storedDescriptor(entry)) return false;
            if ((declaredPacked != 0 && declaredPacked != entry.packed) ||
                (declaredUnpacked != 0 && declaredUnpacked != entry.unpacked) ||
                (declaredCrc != 0 && declaredCrc != entry.crc)) return fail("zip_local_descriptor_mismatch");
            if (!descriptor(entry)) return false;
        } else {
            if (entry.unpacked > limits_.maximumUncompressedSize - totalUnpacked_)
                return limited("zip_uncompressed_size_limit");
            if (entry.method == 0 && entry.packed != entry.unpacked) return fail("zip_store_length_mismatch");
            if (entry.packed > limit_ - position_) {
                if (limit_ < reader_.size()) return limited("zip_archive_size_limit");
                result_.truncationReason = "zip_end_of_input";
                return fail("zip_truncated_entry_data");
            }
            position_ += entry.packed;
        }
        totalUnpacked_ += entry.unpacked;
        entries_.push_back(std::move(entry));
        return true;
    }

    bool storedKnown(ZipEntry& entry) {
        // Compute the CRC before reading the descriptor, whose CRC is authoritative.
        std::array<uint8_t, blockSize> bytes{};
        uLong crc = crc32(0, Z_NULL, 0);
        if (entry.unpacked > limits_.maximumUncompressedSize - totalUnpacked_)
            return limited("zip_uncompressed_size_limit");
        uint64_t done = 0;
        while (done < entry.packed) {
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(bytes.size(), entry.packed - done));
            if (!read(entry.data + done, bytes.data(), amount)) return false;
            crc = crc32(crc, bytes.data(), static_cast<uInt>(amount));
            done += amount;
        }
        entry.crc = static_cast<uint32_t>(crc);
        entry.verified = true;
        return true;
    }

    bool central() {
        const uint64_t start = position_;
        uint64_t count = 0;
        for (;;) {
            uint32_t next = 0;
            if (!signature(next)) return false;
            if (next != centralSignature) break;
            if (count >= entries_.size()) return fail("zip_directory_entry_count_mismatch");
            std::array<uint8_t, 46> header{};
            if (!read(position_, header.data(), header.size())) return false;
            const size_t nameSize = le16(header.data() + 28), extraSize = le16(header.data() + 30);
            const size_t commentSize = le16(header.data() + 32);
            const uint64_t size = header.size() + nameSize + extraSize + commentSize;
            if (position_ - start > limits_.maximumDirectorySize ||
                size > limits_.maximumDirectorySize - (position_ - start)) return limited("zip_directory_size_limit");
            position_ += header.size();
            std::vector<uint8_t> name(nameSize), extraBytes(extraSize), comment(commentSize);
            if (!read(position_, name.data(), name.size())) return false;
            position_ += nameSize;
            if (!read(position_, extraBytes.data(), extraBytes.size())) return false;
            position_ += extraSize;
            if (!read(position_, comment.data(), comment.size())) return false;
            position_ += commentSize;
            uint64_t packed = le32(header.data() + 20), unpacked = le32(header.data() + 24);
            uint64_t offset = le32(header.data() + 42);
            uint32_t disk = le16(header.data() + 34);
            bool wide = false;
            if (!extra(extraBytes, unpacked == sentinel32, packed == sentinel32, offset == sentinel32,
                       disk == 0xffff, unpacked, packed, offset, disk, wide)) return false;
            if (disk != 0) return fail("zip_unsupported_split_archive");
            const uint16_t version = le16(header.data() + 6);
            if (!supported(le16(header.data() + 8), le16(header.data() + 10), version)) return false;
            if (wide && version < 45) return fail("zip_invalid_zip64_version");
            const auto found = std::lower_bound(entries_.begin(), entries_.end(), offset,
                [](const ZipEntry& entry, uint64_t value) { return entry.offset < value; });
            if (found == entries_.end() || found->offset != offset || found->matched)
                return fail("zip_directory_local_offset_mismatch");
            // A writer can discover that a local offset needs ZIP64 only when
            // writing the directory, after the local header was already emitted.
            const bool versionAgrees = found->version == version ||
                                       (wide && version == 45 && found->version < 45);
            if (found->name != name || found->flags != le16(header.data() + 8) ||
                found->method != le16(header.data() + 10) || !versionAgrees ||
                found->timestamp != le32(header.data() + 12) || found->crc != le32(header.data() + 16) ||
                found->packed != packed || found->unpacked != unpacked)
                return fail("zip_directory_local_mismatch");
            found->matched = true;
            ++count;
        }
        if (count != entries_.size()) return fail("zip_directory_entry_count_mismatch");
        return end(start, position_ - start, count);
    }

    bool end(uint64_t directoryOffset, uint64_t directorySize, uint64_t count) {
        uint32_t next = 0;
        if (!signature(next)) return false;
        bool hasZip64 = next == zip64Signature;
        if (hasZip64) {
            const uint64_t zip64Offset = position_;
            std::array<uint8_t, 56> bytes{};
            if (!read(position_, bytes.data(), bytes.size())) return false;
            const uint64_t size = le64(bytes.data() + 4);
            if (size < 44) return fail("zip_invalid_zip64_end_length");
            if (size > limits_.maximumDirectorySize) return limited("zip_directory_size_limit");
            if (le16(bytes.data() + 14) > 45) return fail("zip_unsupported_zip64_version");
            if (le16(bytes.data() + 14) < 45) return fail("zip_invalid_zip64_version");
            if (le32(bytes.data() + 16) != 0 || le32(bytes.data() + 20) != 0)
                return fail("zip_unsupported_split_archive");
            if (le64(bytes.data() + 24) != count || le64(bytes.data() + 32) != count ||
                le64(bytes.data() + 40) != directorySize || le64(bytes.data() + 48) != directoryOffset)
                return fail("zip_zip64_directory_mismatch");
            if (size > limit_ - position_ - 12) {
                if (limit_ < reader_.size()) return limited("zip_archive_size_limit");
                result_.truncationReason = "zip_end_of_input";
                return fail("zip_truncated_zip64_end");
            }
            position_ += 12 + size;
            std::array<uint8_t, 20> locator{};
            if (!read(position_, locator.data(), locator.size())) return false;
            if (le32(locator.data()) != locatorSignature || le64(locator.data() + 8) != zip64Offset)
                return fail("zip_invalid_zip64_locator");
            if (le32(locator.data() + 4) != 0 || le32(locator.data() + 16) != 1)
                return fail("zip_unsupported_split_archive");
            position_ += locator.size();
        }
        std::array<uint8_t, 22> bytes{};
        if (!read(position_, bytes.data(), bytes.size())) return false;
        if (le32(bytes.data()) != endSignature) return fail("zip_missing_end_record_or_unsupported_record");
        auto agrees = [hasZip64](uint64_t value, uint64_t sentinel, uint64_t expected) {
            return value == sentinel ? hasZip64 : value == expected;
        };
        if (!agrees(le16(bytes.data() + 4), 0xffff, 0) || !agrees(le16(bytes.data() + 6), 0xffff, 0))
            return fail("zip_unsupported_split_archive");
        if (!agrees(le16(bytes.data() + 8), 0xffff, count) ||
            !agrees(le16(bytes.data() + 10), 0xffff, count) ||
            !agrees(le32(bytes.data() + 12), sentinel32, directorySize) ||
            !agrees(le32(bytes.data() + 16), sentinel32, directoryOffset))
            return fail("zip_end_directory_mismatch");
        position_ += bytes.size();
        std::vector<uint8_t> comment(le16(bytes.data() + 20));
        if (!read(position_, comment.data(), comment.size())) return false;
        position_ += comment.size();
        return true;
    }

    bool parse() {
        uint32_t next = 0;
        if (!signature(next)) return false;
        if (next != localSignature && next != endSignature && next != zip64Signature)
            return fail("zip_invalid_start_or_unsupported_sfx");
        while (next == localSignature) {
            if (!local() || !signature(next)) return false;
        }
        if (!central()) return false;
        // Known-size streams are checked only after the complete record graph agrees.
        // This also rejects interior headers before decompressing their payloads.
        const uint64_t total = totalUnpacked_;
        totalUnpacked_ = 0;
        for (auto& entry : entries_) {
            if (!check()) return false;
            if (!entry.verified && !stream(entry, false)) return false;
            totalUnpacked_ += entry.unpacked;
        }
        return totalUnpacked_ == total || fail("zip_total_length_mismatch");
    }

    const CandidateReader& reader_;
    ZipLimits limits_;
    uint64_t limit_;
    uint64_t position_ = 0;
    uint64_t totalUnpacked_ = 0;
    uint64_t localMetadata_ = 0;
    std::vector<ZipEntry> entries_;
    ValidationResult result_;
};

class ZipCarver final : public FormatCarver {
public:
    ZipCarver(FormatDescriptor descriptor, ZipLimits limits) : descriptor_(std::move(descriptor)), limits_(limits) {}
    const FormatDescriptor& descriptor() const override { return descriptor_; }
    bool detect(const SearchMatch& match, RecoveryCandidate& candidate) const override {
        if (match.metadata.kind != PatternKind::Header) return false;
        candidate.formatId = "zip";
        candidate.startOffset = match.offset;
        candidate.evidence.push_back({"header_signature", 20, match.metadata.patternId});
        return true;
    }
    int score(const RecoveryCandidate& candidate, const ValidationResult& validation) const override {
        if (validation.state != ValidationState::Valid) return 0;
        int total = 0;
        for (const auto& evidence : candidate.evidence) total += evidence.score;
        for (const auto& evidence : validation.evidence) total += evidence.score;
        return std::min(100, std::max(0, total));
    }
    bool recover(const ImageReader& image, const RecoveryCandidate& candidate, const ValidationResult& validation,
                 const std::string& outputDirectory, RecoveryResult& result, std::string& error,
                 WorkBudget* budget) const override {
        if (validation.state != ValidationState::Valid || !validation.stopReason.empty() || validation.validatedLength == 0) {
            error = "ZIP requires complete validation";
            return false;
        }
        CandidateReader reader(image, candidate, budget);
        result = makeRecoveryResult(candidate, validation, score(candidate, validation));
        if (!reader.physicalExtents(0, validation.validatedLength, result.candidate.extents, error)) return false;
        result.candidate.logicalSize = validation.validatedLength;
        return writeRecoveredFile(image, result.candidate.extents, validation.validatedLength, outputDirectory,
                                  "recovered_" + std::to_string(candidate.startOffset) + ".zip", result.outputPath,
                                  result.byteCount, error, &candidate.residentData, budget);
    }
protected:
    ValidationResult validateContent(const CandidateReader& reader) const override {
        return ZipParser(reader, limits_).run();
    }
private:
    FormatDescriptor descriptor_;
    ZipLimits limits_;
};
}

std::unique_ptr<FormatCarver> makeZipCarver(const FormatDescriptor& descriptor, ZipLimits limits) {
    return std::make_unique<ZipCarver>(descriptor, limits);
}
