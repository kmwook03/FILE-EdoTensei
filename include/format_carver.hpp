#pragma once

#include "io.hpp"
#include "candidate_reader.hpp"
#include "recovery.hpp"
#include "searcher.hpp"
#include "signature.hpp"

#include <memory>
#include <string>
#include <vector>

class FormatCarver {
public:
    virtual ~FormatCarver() = default;
    virtual const FormatDescriptor& descriptor() const = 0;
    virtual bool detect(const SearchMatch& match, RecoveryCandidate& candidate) const = 0;
    ValidationResult validate(const ImageReader& image, const RecoveryCandidate& candidate,
                              WorkBudget* budget = nullptr) const;
    virtual bool recover(const ImageReader& image, const RecoveryCandidate& candidate,
                         const ValidationResult& validation, const std::string& outputDirectory,
                         RecoveryResult& result, std::string& error,
                         WorkBudget* budget = nullptr) const = 0;
    virtual int score(const RecoveryCandidate& candidate,
                      const ValidationResult& validation) const = 0;

protected:
    uint64_t estimateSize(const CandidateReader& reader) const;
    virtual ValidationResult validateContent(const CandidateReader& reader) const = 0;
};

struct ZipLimits {
    uint64_t maximumArchiveSize = 64ULL * 1024 * 1024 * 1024;
    uint64_t maximumEntries = 100000;
    uint64_t maximumDirectorySize = 64ULL * 1024 * 1024;
    uint64_t maximumUncompressedSize = 128ULL * 1024 * 1024 * 1024;
};

class FormatRegistry {
public:
    explicit FormatRegistry(ZipLimits zipLimits = {});
    const std::vector<FormatDescriptor>& descriptors() const { return descriptors_; }
    const FormatCarver* byIndex(size_t index) const;
    const FormatCarver* byId(const std::string& id) const;
    std::string identify(const CandidateReader& reader) const;

private:
    std::vector<FormatDescriptor> descriptors_;
    std::vector<std::unique_ptr<FormatCarver>> carvers_;
};
