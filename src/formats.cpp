#include "format_carver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace {
uint16_t be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}
uint32_t be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}
uint64_t candidateSize(const ImageReader& image, const RecoveryCandidate& candidate,
                       uint64_t maximum) {
    if (candidate.extents.empty()) {
        return candidate.startOffset < image.size()
            ? std::min(maximum, image.size() - candidate.startOffset) : 0;
    }
    uint64_t total = 0;
    for (const auto& extent : candidate.extents) {
        if (extent.length > std::numeric_limits<uint64_t>::max() - total) return maximum;
        total += extent.length;
        if (total >= maximum) return maximum;
    }
    return total;
}
bool readCandidate(const ImageReader& image, const RecoveryCandidate& candidate,
                   uint64_t logicalOffset, void* destination, size_t size,
                   std::string& error) {
    if (!candidate.residentData.empty()) {
        if (logicalOffset > candidate.residentData.size() ||
            size > candidate.residentData.size() - static_cast<size_t>(logicalOffset)) {
            error = "resident candidate ended early"; return false;
        }
        std::copy_n(candidate.residentData.data() + logicalOffset, size,
                    static_cast<uint8_t*>(destination));
        return true;
    }
    if (candidate.extents.empty()) {
        if (logicalOffset > std::numeric_limits<uint64_t>::max() - candidate.startOffset) {
            error = "candidate read offset overflow"; return false;
        }
        return image.readExact(candidate.startOffset + logicalOffset, destination, size, error);
    }
    auto* output = static_cast<uint8_t*>(destination);
    size_t completed = 0;
    uint64_t skip = logicalOffset;
    for (const auto& extent : candidate.extents) {
        if (skip >= extent.length) { skip -= extent.length; continue; }
        const uint64_t available = extent.length - skip;
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(size - completed, available));
        if (extent.sparse) std::fill(output + completed, output + completed + amount, 0);
        else {
            if (skip > std::numeric_limits<uint64_t>::max() - extent.offset ||
                !image.readExact(extent.offset + skip, output + completed, amount, error)) return false;
        }
        completed += amount;
        skip = 0;
        if (completed == size) return true;
    }
    error = "candidate extent stream ended early";
    return false;
}

class BaseCarver : public FormatCarver {
public:
    explicit BaseCarver(const FormatDescriptor& descriptor) : descriptor_(descriptor) {}
    const FormatDescriptor& descriptor() const override { return descriptor_; }
    bool detect(const SearchMatch& match, RecoveryCandidate& candidate) const override {
        if (match.metadata.kind != PatternKind::Header) return false;
        candidate.formatId = descriptor_.id;
        candidate.startOffset = match.offset;
        candidate.method = RecoveryMethod::RawContiguous;
        candidate.evidence.push_back({"header_signature", 20, match.metadata.patternId});
        return true;
    }
    uint64_t estimateEnd(const ImageReader& image, const RecoveryCandidate& candidate) const override {
        const uint64_t size = candidateSize(image, candidate, descriptor_.maxFileSize);
        return candidate.startOffset > std::numeric_limits<uint64_t>::max() - size
            ? std::numeric_limits<uint64_t>::max() : candidate.startOffset + size;
    }
    bool recover(const ImageReader& image, const RecoveryCandidate& candidate,
                 const ValidationResult& validation, const std::string& outputDirectory,
                 RecoveryResult& result, std::string& error) const override {
        if (validation.validatedEndOffset <= candidate.startOffset) {
            error = "candidate has no validated extent";
            return false;
        }
        RecoveryCandidate actual = candidate;
        const uint64_t length = validation.validatedEndOffset - candidate.startOffset;
        if (actual.extents.empty()) actual.extents = {{candidate.startOffset, length, false}};
        const std::string name = "recovered_" + std::to_string(candidate.startOffset) +
                                 "." + descriptor_.extension;
        result.candidate = actual;
        result.validationState = validation.state;
        result.truncationReason = validation.truncationReason;
        result.evidence = candidate.evidence;
        result.evidence.insert(result.evidence.end(), validation.evidence.begin(), validation.evidence.end());
        result.warnings = candidate.warnings;
        result.warnings.insert(result.warnings.end(), validation.diagnostics.begin(), validation.diagnostics.end());
        result.confidence = score(candidate, validation);
        return writeRecoveredFile(image, actual.extents, length, outputDirectory, name,
                                  result.outputPath, result.byteCount, error, &actual.residentData);
    }
    int score(const RecoveryCandidate& candidate, const ValidationResult& validation) const override {
        int total = validation.confidenceContribution;
        for (const auto& item : candidate.evidence) total += item.score;
        for (const auto& item : validation.evidence) total += item.score;
        if (validation.state == ValidationState::Invalid) total = std::min(total, 19);
        if (validation.state == ValidationState::Truncated) total -= 20;
        return std::max(0, std::min(100, total));
    }
protected:
    FormatDescriptor descriptor_;
};

class JpegCarver final : public BaseCarver {
public:
    using BaseCarver::BaseCarver;
    ValidationResult validate(const ImageReader& image,
                              const RecoveryCandidate& candidate) const override {
        ValidationResult result;
        std::array<uint8_t, 4> head{};
        std::string error;
        if (!readCandidate(image, candidate, 0, head.data(), head.size(), error) ||
            head[0] != 0xff || head[1] != 0xd8 || head[2] != 0xff ||
            head[3] == 0x00 || head[3] == 0xd8 || head[3] == 0xd9) {
            result.diagnostics.push_back("invalid SOI or first marker");
            return result;
        }
        result.evidence.push_back({"jpeg_plausible_first_marker", 15, "marker follows SOI"});
        const uint64_t logicalLimit = estimateEnd(image, candidate) - candidate.startOffset;
        uint64_t position = 2;
        bool inScan = false;
        bool sawSegment = false;
        std::array<uint8_t, 4> bytes{};
        while (position + 1 < logicalLimit) {
            if (!readCandidate(image, candidate, position, bytes.data(), 2, error)) break;
            if (bytes[0] != 0xff) {
                if (!inScan) { result.diagnostics.push_back("expected JPEG marker"); return result; }
                ++position;
                continue;
            }
            uint8_t marker = bytes[1];
            while (marker == 0xff && position + 2 < logicalLimit) {
                ++position;
                if (!readCandidate(image, candidate, position + 1, &marker, 1, error)) break;
            }
            if (inScan && marker == 0x00) { position += 2; continue; }
            if (marker >= 0xd0 && marker <= 0xd7) { position += 2; continue; }
            if (marker == 0xd9) {
                result.state = ValidationState::Valid;
                result.validatedEndOffset = candidate.startOffset + position + 2;
                result.confidenceContribution = 20;
                result.evidence.push_back({"jpeg_structure", 25, sawSegment ? "marker segments parsed" : "minimal stream"});
                result.evidence.push_back({"jpeg_eoi", 20, "EOI marker found"});
                return result;
            }
            inScan = false;
            if (marker == 0x01 || marker == 0xd8) { position += 2; continue; }
            if (position + 4 > logicalLimit ||
                !readCandidate(image, candidate, position + 2, bytes.data(), 2, error)) break;
            const uint16_t length = be16(bytes.data());
            if (length < 2 || 2ULL + length > logicalLimit - position) {
                result.diagnostics.push_back("invalid or truncated JPEG segment length");
                break;
            }
            sawSegment = true;
            position += 2ULL + length;
            if (marker == 0xda) inScan = true;
        }
        result.state = ValidationState::Truncated;
        result.validatedEndOffset = candidate.startOffset + logicalLimit;
        result.confidenceContribution = 10;
        result.truncationReason = logicalLimit < descriptor_.maxFileSize ? "end_of_input_before_eoi" : "maximum_size_before_eoi";
        return result;
    }
};

class PngCarver final : public BaseCarver {
public:
    using BaseCarver::BaseCarver;
    ValidationResult validate(const ImageReader& image,
                              const RecoveryCandidate& candidate) const override {
        static const std::array<uint8_t, 8> signature{{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a}};
        ValidationResult result;
        std::array<uint8_t, 12> header{};
        std::string error;
        if (!readCandidate(image, candidate, 0, header.data(), 8, error) ||
            !std::equal(signature.begin(), signature.end(), header.begin())) {
            result.diagnostics.push_back("invalid PNG signature"); return result;
        }
        uint64_t position = 8;
        const uint64_t logicalLimit = estimateEnd(image, candidate) - candidate.startOffset;
        bool first = true;
        bool sawIdat = false;
        std::vector<uint8_t> buffer(64 * 1024);
        size_t chunkCount = 0;
        while (position + 12 <= logicalLimit) {
            if (!readCandidate(image, candidate, position, header.data(), 8, error)) break;
            const uint32_t length = be32(header.data());
            const std::string type(reinterpret_cast<char*>(header.data() + 4), 4);
            const uint64_t total = 12ULL + length;
            if (total > logicalLimit - position) break;
            if (first && (type != "IHDR" || length != 13)) {
                result.diagnostics.push_back("IHDR is not the first PNG chunk"); return result;
            }
            uint32_t crc = crc32Update(0xffffffffU, header.data() + 4, 4);
            uint64_t dataPosition = position + 8;
            uint64_t remaining = length;
            while (remaining > 0) {
                const size_t amount = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
                if (!readCandidate(image, candidate, dataPosition, buffer.data(), amount, error)) { remaining = 0; break; }
                crc = crc32Update(crc, buffer.data(), amount);
                dataPosition += amount;
                remaining -= amount;
            }
            uint8_t storedBytes[4];
            if (!readCandidate(image, candidate, position + 8ULL + length, storedBytes, 4, error)) break;
            if ((crc ^ 0xffffffffU) != be32(storedBytes)) {
                result.diagnostics.push_back("PNG chunk CRC mismatch in " + type); return result;
            }
            ++chunkCount;
            position += total;
            first = false;
            if (type == "IDAT") sawIdat = true;
            if (type == "IEND") {
                if (length != 0 || !sawIdat) {
                    result.diagnostics.push_back(length != 0 ? "IEND has nonzero length" : "PNG has no IDAT chunk");
                    return result;
                }
                result.state = ValidationState::Valid;
                result.validatedEndOffset = candidate.startOffset + position;
                result.confidenceContribution = 20;
                result.evidence.push_back({"png_chunk_structure", 25, std::to_string(chunkCount) + " chunks"});
                result.evidence.push_back({"png_crc", 20, "all chunk CRCs valid"});
                result.evidence.push_back({"png_iend", 15, "IEND parsed"});
                return result;
            }
        }
        result.state = ValidationState::Truncated;
        result.validatedEndOffset = candidate.startOffset + logicalLimit;
        result.confidenceContribution = 5;
        result.truncationReason = logicalLimit < descriptor_.maxFileSize ? "end_of_input_before_iend" : "maximum_size_before_iend";
        return result;
    }
};

class PdfCarver final : public BaseCarver {
public:
    using BaseCarver::BaseCarver;
    ValidationResult validate(const ImageReader& image,
                              const RecoveryCandidate& candidate) const override {
        ValidationResult result;
        uint8_t header[8]{};
        std::string error;
        if (!readCandidate(image, candidate, 0, header, sizeof(header), error) ||
            std::string(reinterpret_cast<char*>(header), 5) != "%PDF-" ||
            !std::isdigit(header[5]) || header[6] != '.' || !std::isdigit(header[7])) {
            result.diagnostics.push_back("invalid PDF header/version"); return result;
        }
        result.evidence.push_back({"pdf_version", 15, std::string(reinterpret_cast<char*>(header + 5), 3)});
        const uint64_t logicalLimit = estimateEnd(image, candidate) - candidate.startOffset;
        std::vector<uint8_t> buffer(1024 * 1024 + 16);
        std::string carry;
        uint64_t position = 0;
        uint64_t lastEof = 0;
        bool sawStartXref = false;
        bool sawXref = false;
        while (position < logicalLimit) {
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(1024 * 1024, logicalLimit - position));
            if (!readCandidate(image, candidate, position, buffer.data(), amount, error)) break;
            std::string text = carry + std::string(reinterpret_cast<char*>(buffer.data()), amount);
            size_t found = 0;
            while ((found = text.find("%%EOF", found)) != std::string::npos) {
                const uint64_t base = position >= carry.size() ? position - carry.size() : 0;
                uint64_t end = base + found + 5;
                bool validBoundary = end == logicalLimit;
                while (end < logicalLimit) {
                    uint8_t byte = 0;
                    if (!readCandidate(image, candidate, end, &byte, 1, error) ||
                        (byte != '\r' && byte != '\n' && byte != ' ' && byte != '\t')) break;
                    validBoundary = true;
                    ++end;
                }
                if (validBoundary) lastEof = std::max(lastEof, end);
                found += 5;
            }
            sawStartXref = sawStartXref || text.find("startxref") != std::string::npos;
            sawXref = sawXref || text.find("xref") != std::string::npos || text.find("/XRef") != std::string::npos;
            carry = text.size() > 16 ? text.substr(text.size() - 16) : text;
            position += amount;
        }
        if (lastEof > 0) {
            result.state = ValidationState::Valid;
            result.validatedEndOffset = candidate.startOffset + lastEof;
            result.confidenceContribution = 15;
            result.evidence.push_back({"pdf_eof", 20, "EOF marker found"});
            if (sawStartXref) result.evidence.push_back({"pdf_startxref", 15, "startxref found"});
            if (sawXref) result.evidence.push_back({"pdf_xref", 10, "xref evidence found"});
            else result.diagnostics.push_back("no xref evidence; accepting weak PDF");
            return result;
        }
        result.state = ValidationState::Truncated;
        result.validatedEndOffset = candidate.startOffset + logicalLimit;
        result.confidenceContribution = 5;
        result.truncationReason = logicalLimit < descriptor_.maxFileSize ? "end_of_input_before_pdf_eof" : "maximum_size_before_pdf_eof";
        return result;
    }
};
}

FormatRegistry::FormatRegistry() : descriptors_(SignatureDB::getFormats()) {
    carvers_.push_back(std::make_unique<JpegCarver>(descriptors_[0]));
    carvers_.push_back(std::make_unique<PngCarver>(descriptors_[1]));
    carvers_.push_back(std::make_unique<PdfCarver>(descriptors_[2]));
}

const FormatCarver* FormatRegistry::byIndex(size_t index) const {
    return index < carvers_.size() ? carvers_[index].get() : nullptr;
}

const FormatCarver* FormatRegistry::byId(const std::string& id) const {
    for (const auto& carver : carvers_) if (carver->descriptor().id == id) return carver.get();
    return nullptr;
}
