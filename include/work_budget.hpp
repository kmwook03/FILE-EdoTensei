#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

enum class StopReason { None, TimeLimit, Cancelled, CandidateLimit, MatchLimit, MetadataLimit };

const char* toString(StopReason reason);

struct WorkLimits {
    std::chrono::milliseconds timeLimit{7200000}; // zero disables the deadline
    uint64_t maximumCandidates = 100000;
    uint64_t maximumMatches = 1000000;
};

class WorkBudget {
public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    explicit WorkBudget(WorkLimits limits = {}, Now now = Clock::now);
    void reset();
    bool check(); // discovery limits still allow validation and extraction
    bool discoveryAllowed();
    bool claimCandidate();
    bool claimMatch();
    void stopDiscovery(StopReason reason);
    void cancel() { cancelled_.store(true); }
    StopReason stopReason() const;
    uint64_t candidateCount() const { return candidates_; }
    uint64_t matchCount() const { return matches_; }
    uint64_t elapsedMilliseconds() const;

private:
    WorkLimits limits_;
    Now now_;
    Clock::time_point started_;
    StopReason hardStop_ = StopReason::None;
    StopReason discoveryStop_ = StopReason::None;
    std::atomic<bool> cancelled_{false};
    uint64_t candidates_ = 0;
    uint64_t matches_ = 0;
};
