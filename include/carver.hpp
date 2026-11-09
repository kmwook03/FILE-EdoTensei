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
    WorkLimits limits;
};

class FileCarver {
public:
    explicit FileCarver(CarverOptions options, WorkBudget::Now now = WorkBudget::Clock::now);
    bool initialize();
    bool startCarving();
    const std::string& lastError() const { return lastError_; }
    const CarvingSummary& summary() const { return summary_; }
    void cancel() { budget_.cancel(); }

private:
    bool scanRaw(std::vector<RecoveryCandidate>& candidates);
    bool processCandidates(std::vector<RecoveryCandidate>& candidates);

    CarverOptions options_;
    WorkBudget budget_;
    ImageReader image_;
    FormatRegistry registry_;
    Searcher scanner_;
    JsonlReport report_;
    CarvingSummary summary_;
    std::string lastError_;
};
