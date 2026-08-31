#pragma once

#include "io.hpp"
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
    virtual uint64_t estimateEnd(const ImageReader& image,
                                 const RecoveryCandidate& candidate) const = 0;
    virtual ValidationResult validate(const ImageReader& image,
                                      const RecoveryCandidate& candidate) const = 0;
    virtual bool recover(const ImageReader& image, const RecoveryCandidate& candidate,
                         const ValidationResult& validation, const std::string& outputDirectory,
                         RecoveryResult& result, std::string& error) const = 0;
    virtual int score(const RecoveryCandidate& candidate,
                      const ValidationResult& validation) const = 0;
};

class FormatRegistry {
public:
    FormatRegistry();
    const std::vector<FormatDescriptor>& descriptors() const { return descriptors_; }
    const FormatCarver* byIndex(size_t index) const;
    const FormatCarver* byId(const std::string& id) const;

private:
    std::vector<FormatDescriptor> descriptors_;
    std::vector<std::unique_ptr<FormatCarver>> carvers_;
};
