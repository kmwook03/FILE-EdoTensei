#include "work_budget.hpp"
#include <utility>

const char* toString(StopReason reason) {
    switch (reason) {
        case StopReason::None: return "";
        case StopReason::TimeLimit: return "time_limit";
        case StopReason::Cancelled: return "cancelled";
        case StopReason::CandidateLimit: return "candidate_limit";
        case StopReason::MatchLimit: return "match_limit";
        case StopReason::MetadataLimit: return "metadata_record_limit";
    }
    return "unknown";
}

WorkBudget::WorkBudget(WorkLimits limits, Now now) : limits_(limits), now_(std::move(now)) {
    reset();
}

void WorkBudget::reset() {
    started_ = now_();
    hardStop_ = discoveryStop_ = StopReason::None;
    cancelled_.store(false);
    candidates_ = matches_ = 0;
}

bool WorkBudget::check() {
    if (hardStop_ != StopReason::None) return false;
    if (cancelled_.load()) hardStop_ = StopReason::Cancelled;
    else if (limits_.timeLimit.count() > 0 &&
             std::chrono::duration_cast<std::chrono::milliseconds>(now_() - started_) >= limits_.timeLimit)
        hardStop_ = StopReason::TimeLimit;
    return hardStop_ == StopReason::None;
}

bool WorkBudget::discoveryAllowed() {
    return check() && discoveryStop_ == StopReason::None;
}

bool WorkBudget::claimCandidate() {
    // A scanner batch can already contain matches when its match limit is reached.
    if (!check() || discoveryStop_ == StopReason::CandidateLimit) return false;
    if (candidates_ >= limits_.maximumCandidates) {
        stopDiscovery(StopReason::CandidateLimit);
        return false;
    }
    ++candidates_;
    return true;
}

bool WorkBudget::claimMatch() {
    if (!discoveryAllowed()) return false;
    if (matches_ >= limits_.maximumMatches) {
        stopDiscovery(StopReason::MatchLimit);
        return false;
    }
    ++matches_;
    return true;
}

void WorkBudget::stopDiscovery(StopReason reason) {
    if (discoveryStop_ == StopReason::None) discoveryStop_ = reason;
}

StopReason WorkBudget::stopReason() const {
    return hardStop_ != StopReason::None ? hardStop_ : discoveryStop_;
}

uint64_t WorkBudget::elapsedMilliseconds() const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_() - started_).count();
    return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}
