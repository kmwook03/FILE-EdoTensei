#include "candidate_reader.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

CandidateReader::CandidateReader(const ImageReader& image, const RecoveryCandidate& candidate,
                                 WorkBudget* budget)
    : image_(image), candidate_(candidate), budget_(budget ? budget : image.budget()) {
    uint64_t available = 0;
    for (const auto& extent : candidate.extents) {
        if (budget_ && !budget_->check()) { error_ = toString(budget_->stopReason()); return; }
        if (extent.length == 0 || extent.length > std::numeric_limits<uint64_t>::max() - available) {
            error_ = "invalid or overflowing candidate extent length";
            return;
        }
        if (!extent.sparse && (extent.offset > image.size() || extent.length > image.size() - extent.offset)) {
            error_ = "candidate extent outside input";
            return;
        }
        available += extent.length;
        ends_.push_back(available);
    }
    if (!candidate.residentData.empty()) {
        if (!candidate.extents.empty() && available < candidate.logicalSize.value_or(candidate.residentData.size())) {
            error_ = "resident data exceeds candidate extents";
            return;
        }
        available = candidate.residentData.size();
    } else if (candidate.extents.empty()) {
        if (candidate.startOffset > image.size()) {
            error_ = "candidate start outside input";
            return;
        }
        available = image.size() - candidate.startOffset;
    }
    size_ = candidate.logicalSize.value_or(available);
    if (size_ > available) {
        error_ = "candidate logical length exceeds source";
        size_ = 0;
    }
}

bool CandidateReader::readExact(uint64_t offset, void* destination, size_t size,
                               std::string& error) const {
    if (!valid()) { error = error_; return false; }
    if (offset > size_ || size > size_ - offset || (size != 0 && destination == nullptr)) {
        error = "candidate read outside logical bounds";
        return false;
    }
    if (budget_ && !budget_->check()) { error = toString(budget_->stopReason()); return false; }
    if (size == 0) return true;
    auto* output = static_cast<uint8_t*>(destination);
    size_t completed = 0;
    size_t index = static_cast<size_t>(std::upper_bound(ends_.begin(), ends_.end(), offset) - ends_.begin());
    while (completed < size) {
        if (budget_ && !budget_->check()) { error = toString(budget_->stopReason()); return false; }
        size_t amount = std::min<size_t>(1024 * 1024, size - completed);
        if (!candidate_.residentData.empty()) {
            std::copy_n(candidate_.residentData.data() + static_cast<size_t>(offset), amount, output + completed);
        } else if (candidate_.extents.empty()) {
            if (!image_.readExact(candidate_.startOffset + offset, output + completed, amount, error)) {
                readError_ = error;
                return false;
            }
        } else {
            const auto& extent = candidate_.extents[index];
            const uint64_t within = offset - (index == 0 ? 0 : ends_[index - 1]);
            amount = static_cast<size_t>(std::min<uint64_t>(amount, extent.length - within));
            if (extent.sparse) std::fill_n(output + completed, amount, 0);
            else if (!image_.readExact(extent.offset + within, output + completed, amount, error)) {
                readError_ = error;
                return false;
            }
            if (offset + amount == ends_[index]) ++index;
        }
        offset += amount;
        completed += amount;
    }
    return true;
}

bool CandidateReader::physicalExtents(uint64_t offset, uint64_t length,
                                     std::vector<Extent>& extents, std::string& error) const {
    extents.clear();
    if (!valid()) { error = error_; return false; }
    if (budget_ && !budget_->check()) { error = toString(budget_->stopReason()); return false; }
    if (offset > size_ || length > size_ - offset) {
        error = "candidate extent slice outside logical bounds";
        return false;
    }
    if (length == 0) return true;
    if (candidate_.extents.empty()) {
        if (candidate_.residentData.empty()) extents.push_back({candidate_.startOffset + offset, length, false});
        return true; // resident bytes need not have a physical mapping
    }
    size_t index = static_cast<size_t>(std::upper_bound(ends_.begin(), ends_.end(), offset) - ends_.begin());
    while (length > 0) {
        if (budget_ && !budget_->check()) { error = toString(budget_->stopReason()); extents.clear(); return false; }
        const auto& extent = candidate_.extents[index];
        const uint64_t within = offset - (index == 0 ? 0 : ends_[index - 1]);
        const uint64_t amount = std::min(length, extent.length - within);
        extents.push_back({extent.sparse ? 0 : extent.offset + within, amount, extent.sparse});
        offset += amount;
        length -= amount;
        ++index;
    }
    return true;
}

bool PhysicalCoverage::overlaps(const std::vector<Extent>& extents) const {
    for (const auto& extent : extents) {
        if (extent.sparse || extent.length == 0) continue;
        auto next = ranges_.upper_bound(extent.offset);
        if (next != ranges_.begin() && std::prev(next)->second > extent.offset) return true;
        if (next != ranges_.end() && next->first - extent.offset < extent.length) return true;
    }
    return false;
}

bool PhysicalCoverage::add(const std::vector<Extent>& extents) {
    for (const auto& extent : extents)
        if (!extent.sparse && extent.length > std::numeric_limits<uint64_t>::max() - extent.offset) return false;
    for (const auto& extent : extents) {
        if (extent.sparse || extent.length == 0) continue;
        uint64_t start = extent.offset;
        uint64_t end = start + extent.length;
        auto next = ranges_.lower_bound(start);
        if (next != ranges_.begin() && std::prev(next)->second >= start) {
            --next;
            start = next->first;
        }
        while (next != ranges_.end() && next->first <= end) {
            end = std::max(end, next->second);
            next = ranges_.erase(next);
        }
        ranges_.emplace(start, end);
    }
    return true;
}
