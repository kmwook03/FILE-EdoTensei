#pragma once

#include "io.hpp"

#include <map>

// The candidate and image must outlive this view and remain unchanged.
class CandidateReader {
public:
    CandidateReader(const ImageReader& image, const RecoveryCandidate& candidate,
                    WorkBudget* budget = nullptr);
    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    const std::string& readError() const { return readError_; }
    uint64_t size() const { return size_; }
    WorkBudget* budget() const { return budget_; }
    bool readExact(uint64_t offset, void* destination, size_t size, std::string& error) const;
    bool physicalExtents(uint64_t offset, uint64_t length, std::vector<Extent>& extents,
                         std::string& error) const;

private:
    const ImageReader& image_;
    const RecoveryCandidate& candidate_;
    WorkBudget* budget_;
    uint64_t size_ = 0;
    std::string error_;
    mutable std::string readError_;
    std::vector<uint64_t> ends_;
};

// Stores the union of physical, non-sparse ranges, without filling fragment gaps.
class PhysicalCoverage {
public:
    bool overlaps(const std::vector<Extent>& extents) const;
    bool add(const std::vector<Extent>& extents);

private:
    std::map<uint64_t, uint64_t> ranges_;
};
