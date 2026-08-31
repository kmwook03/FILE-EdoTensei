#pragma once

#include "format_carver.hpp"
#include "io.hpp"
#include "report.hpp"
#include "searcher.hpp"

#include <cstdint>
#include <string>
#include <vector>

enum class RecoveryMode { Raw, Ntfs, Hybrid };

struct CarverOptions {
    std::string inputPath;
    std::string outputDirectory = ".";
    std::string reportPath;
    RecoveryMode mode = RecoveryMode::Hybrid;
    int minimumConfidence = 0;
};

struct CarvingSummary {
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t errors = 0;
};

class FileCarver {
public:
    explicit FileCarver(CarverOptions options);
    explicit FileCarver(const std::string& path);
    bool initialize();
    bool startCarving();
    const std::string& lastError() const { return lastError_; }
    const CarvingSummary& summary() const { return summary_; }

private:
    bool scanRaw(std::vector<RecoveryCandidate>& candidates);
    bool processCandidates(std::vector<RecoveryCandidate>& candidates);
    bool reportResult(const RecoveryResult& result);

    CarverOptions options_;
    ImageReader image_;
    FormatRegistry registry_;
    Searcher scanner_;
    JsonlReport report_;
    CarvingSummary summary_;
    std::string lastError_;
};
