#ifndef SIGNATURE_HPP
#define SIGNATURE_HPP

#include <string>
#include <vector>
#include <cstdint>

struct FileSignature {
    std::string extension;                     // Type of the file (e.g., "JPEG", "PNG")
    std::vector<uint8_t> header;          // Byte sequence representing the file header
    std::vector<uint8_t> footer;          // Byte sequence representing the file footer (not always present)
    bool hasFooter;                       // Indicates if the file type has a footer

    FileSignature(std::string ext, std::vector<uint8_t> h, std::vector<uint8_t> f, bool hf)
        : extension(ext), header(h), footer(f), hasFooter(hf) {}
};

class SignatureDB {
public:
    static std::vector<FileSignature> getSignatures() {
        return {
            // JPG
            FileSignature("jpg", {0xFF, 0xD8, 0xFF}, {0xFF, 0xD9}, true),
            // PNG
            FileSignature("png", {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}, 
                                 {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}, true),
            // PDF
            FileSignature("pdf", {0x25, 0x50, 0x44, 0x46, 0x2D}, {0x25, 0x25, 0x45, 0x4F, 0x46}, true)
        };
    }
};

#endif // SIGNATURE_HPP