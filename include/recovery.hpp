#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class ValidationState { Valid, Invalid, Truncated, NotValidated };
enum class RecoveryMethod { RawContiguous, NtfsMetadata, RawReassembled, Fat32Metadata, ExfatMetadata };

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
    std::optional<uint64_t> logicalSize; // unset: use the available source length
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
    uint64_t validatedLength = 0; // logical bytes, independent of physical placement
    std::string stopReason;
    std::vector<Evidence> evidence;
    std::vector<std::string> diagnostics;
    std::string truncationReason;
};

struct RecoveryResult {
    RecoveryCandidate candidate;
    std::string outputPath;
    uint64_t byteCount = 0;
    int confidence = 0;
    ValidationState validationState = ValidationState::NotValidated;
    uint64_t validatedLength = 0;
    bool skipped = false;
    std::string stopReason;
    std::string truncationReason;
    std::vector<Evidence> evidence;
    std::vector<std::string> warnings;
    bool accepted = false;
};

inline RecoveryResult makeRecoveryResult(const RecoveryCandidate& candidate,
                                        const ValidationResult& validation, int confidence) {
    RecoveryResult result;
    result.candidate = candidate;
    result.validationState = validation.state;
    result.validatedLength = validation.validatedLength;
    result.stopReason = validation.stopReason;
    result.truncationReason = validation.truncationReason;
    result.evidence = candidate.evidence;
    result.evidence.insert(result.evidence.end(), validation.evidence.begin(), validation.evidence.end());
    result.warnings = candidate.warnings;
    result.warnings.insert(result.warnings.end(), validation.diagnostics.begin(), validation.diagnostics.end());
    result.confidence = confidence;
    result.skipped = validation.state == ValidationState::NotValidated || !validation.stopReason.empty();
    return result;
}

struct CarvingSummary {
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t errors = 0;
    uint64_t skipped = 0;
    uint64_t discovered = 0;
    uint64_t matches = 0;
    uint64_t rawScannedBytes = 0;
    uint64_t elapsedMilliseconds = 0;
    bool rawScanPerformed = false;
    bool rawScanComplete = false;
    bool complete = false;
    std::string stopReason;
};

inline bool operator==(const Extent& a, const Extent& b) {
    return a.offset == b.offset && a.length == b.length && a.sparse == b.sparse;
}

inline bool operator<(const Extent& a, const Extent& b) {
    if (a.offset != b.offset) return a.offset < b.offset;
    if (a.length != b.length) return a.length < b.length;
    return a.sparse < b.sparse;
}

inline const char* toString(ValidationState state) {
    switch (state) {
        case ValidationState::Valid: return "valid";
        case ValidationState::Truncated: return "truncated";
        case ValidationState::NotValidated: return "not_validated";
        default: return "invalid";
    }
}

inline const char* toString(RecoveryMethod method) {
    switch (method) {
        case RecoveryMethod::RawContiguous: return "raw_contiguous";
        case RecoveryMethod::NtfsMetadata: return "ntfs_metadata";
        case RecoveryMethod::RawReassembled: return "raw_reassembled";
        case RecoveryMethod::Fat32Metadata: return "fat32_metadata";
        case RecoveryMethod::ExfatMetadata: return "exfat_metadata";
    }
    return "unknown";
}
