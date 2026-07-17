#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <cstdint>

enum class PatternKind {
    Header,
    Footer
};

enum class FooterPolicy {
    None,
    Required,
    Incremental
};

struct BytePattern {
    PatternKind kind;
    std::vector<uint8_t> bytes;
    std::string label;

    BytePattern(PatternKind patternKind, std::vector<uint8_t> patternBytes, std::string patternLabel)
        : kind(patternKind), bytes(patternBytes), label(patternLabel) {}
};

struct FormatDescriptor {
    std::string extension;              // File extension used for recovered output
    std::string name;                   // Human-readable format name
    std::vector<BytePattern> patterns;  // Header/footer byte patterns used by the carver
    FooterPolicy footerPolicy;
    bool allowEmbeddedJpgHeaders;
    size_t maxFileSize;

    FormatDescriptor(std::string ext,
                     std::string formatName,
                     std::vector<BytePattern> formatPatterns,
                     FooterPolicy policy,
                     bool allowJpgHeaders = false,
                     size_t maxSize = 100 * 1024 * 1024)
        : extension(ext),
          name(formatName),
          patterns(formatPatterns),
          footerPolicy(policy),
          allowEmbeddedJpgHeaders(allowJpgHeaders),
          maxFileSize(maxSize) {}

    const BytePattern* primaryHeader() const {
        for (const auto& pattern : patterns) {
            if (pattern.kind == PatternKind::Header) {
                return &pattern;
            }
        }
        return nullptr;
    }

    const BytePattern* primaryFooter() const {
        for (const auto& pattern : patterns) {
            if (pattern.kind == PatternKind::Footer) {
                return &pattern;
            }
        }
        return nullptr;
    }

    bool hasFooter() const {
        return footerPolicy != FooterPolicy::None && primaryFooter() != nullptr;
    }

    bool isIncremental() const {
        return footerPolicy == FooterPolicy::Incremental;
    }
};

class SignatureDB {
public:
    static std::vector<FormatDescriptor> getFormats() {
        return {
            FormatDescriptor(
                "jpg",
                "JPEG image",
                {
                    BytePattern(PatternKind::Header, {0xFF, 0xD8, 0xFF}, "SOI"),
                    BytePattern(PatternKind::Footer, {0xFF, 0xD9}, "EOI")
                },
                FooterPolicy::Required
            ),
            FormatDescriptor(
                "png",
                "PNG image",
                {
                    BytePattern(PatternKind::Header, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}, "PNG signature"),
                    BytePattern(PatternKind::Footer, {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}, "IEND")
                },
                FooterPolicy::Required
            ),
            FormatDescriptor(
                "pdf",
                "PDF document",
                {
                    BytePattern(PatternKind::Header, {0x25, 0x50, 0x44, 0x46, 0x2D}, "PDF header"),
                    BytePattern(PatternKind::Footer, {0x25, 0x25, 0x45, 0x4F, 0x46}, "EOF")
                },
                FooterPolicy::Incremental,
                true
            )
        };
    }
};
