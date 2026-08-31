#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class PatternKind { Header, Footer, StructuralAnchor };

struct BytePattern {
    std::string id;
    PatternKind kind;
    std::vector<uint8_t> bytes;
};

struct FormatDescriptor {
    std::string id;
    std::string extension;
    std::string name;
    std::vector<BytePattern> patterns;
    uint64_t maxFileSize;
};

class SignatureDB {
public:
    static std::vector<FormatDescriptor> getFormats() {
        constexpr uint64_t mib = 1024ULL * 1024ULL;
        return {
            {"jpeg", "jpg", "JPEG image",
             {{"jpeg.soi", PatternKind::Header, {0xff, 0xd8, 0xff}},
              {"jpeg.eoi", PatternKind::Footer, {0xff, 0xd9}}}, 100 * mib},
            {"png", "png", "PNG image",
             {{"png.signature", PatternKind::Header,
               {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a}},
              {"png.iend", PatternKind::Footer,
               {0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82}}}, 100 * mib},
            {"pdf", "pdf", "PDF document",
             {{"pdf.header", PatternKind::Header, {'%', 'P', 'D', 'F', '-'}},
              {"pdf.eof", PatternKind::Footer, {'%', '%', 'E', 'O', 'F'}},
              {"pdf.startxref", PatternKind::StructuralAnchor,
               {'s', 't', 'a', 'r', 't', 'x', 'r', 'e', 'f'}}}, 500 * mib}
        };
    }
};
