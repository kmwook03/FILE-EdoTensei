#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ValidationState { Valid, Invalid, Truncated };
enum class RecoveryMethod { RawContiguous, NtfsMetadata };

struct Extent {
    uint64_t offset = 0;
    uint64_t length = 0;
    bool sparse = false;
};

struct Evidence {
    std::string name;
    int score = 0;
    std::string detail;
};

struct RecoveryCandidate {
    std::string formatId;
    uint64_t startOffset = 0;
    std::vector<Extent> extents;
    std::vector<uint8_t> residentData;
    std::vector<Evidence> evidence;
    std::vector<std::string> warnings;
    RecoveryMethod method = RecoveryMethod::RawContiguous;
    std::string sourceName;
};

struct ValidationResult {
    ValidationState state = ValidationState::Invalid;
    int confidenceContribution = 0;
    uint64_t validatedEndOffset = 0; // exclusive
    std::vector<Evidence> evidence;
    std::vector<std::string> diagnostics;
    std::string truncationReason;
};

struct RecoveryResult {
    RecoveryCandidate candidate;
    std::string outputPath;
    uint64_t byteCount = 0;
    int confidence = 0;
    ValidationState validationState = ValidationState::Invalid;
    std::string truncationReason;
    std::vector<Evidence> evidence;
    std::vector<std::string> warnings;
    bool accepted = false;
};

inline const char* toString(ValidationState state) {
    switch (state) {
        case ValidationState::Valid: return "valid";
        case ValidationState::Truncated: return "truncated";
        default: return "invalid";
    }
}

inline const char* toString(RecoveryMethod method) {
    return method == RecoveryMethod::NtfsMetadata ? "ntfs_metadata" : "raw_contiguous";
}
