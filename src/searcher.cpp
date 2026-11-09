#include "searcher.hpp"

#include <algorithm>
#include <limits>
#include <queue>

Searcher::Node::Node() : fail(0) { next.fill(-1); }

void Searcher::build(const std::vector<FormatDescriptor>& formats) {
    nodes_.assign(1, Node());
    for (size_t fi = 0; fi < formats.size(); ++fi) {
        for (const auto& pattern : formats[fi].patterns) {
            if (pattern.bytes.empty()) continue;
            size_t node = 0;
            for (uint8_t byte : pattern.bytes) {
                if (nodes_[node].next[byte] < 0) {
                    nodes_[node].next[byte] = static_cast<int>(nodes_.size());
                    nodes_.emplace_back();
                }
                node = static_cast<size_t>(nodes_[node].next[byte]);
            }
            nodes_[node].outputs.push_back(
                {fi, pattern.id, pattern.kind, pattern.bytes.size()});
        }
    }
    std::queue<size_t> queue;
    for (size_t b = 0; b < 256; ++b) {
        int child = nodes_[0].next[b];
        if (child < 0) nodes_[0].next[b] = 0;
        else queue.push(static_cast<size_t>(child));
    }
    while (!queue.empty()) {
        const size_t current = queue.front();
        queue.pop();
        for (size_t b = 0; b < 256; ++b) {
            int child = nodes_[current].next[b];
            if (child < 0) {
                nodes_[current].next[b] = nodes_[nodes_[current].fail].next[b];
                continue;
            }
            const size_t ci = static_cast<size_t>(child);
            const size_t fallback = static_cast<size_t>(nodes_[nodes_[current].fail].next[b]);
            nodes_[ci].fail = fallback;
            nodes_[ci].outputs.insert(nodes_[ci].outputs.end(),
                                      nodes_[fallback].outputs.begin(),
                                      nodes_[fallback].outputs.end());
            queue.push(ci);
        }
    }
    reset();
}

void Searcher::reset() {
    state_ = 0;
    nextOffset_ = 0;
    streaming_ = false;
}

std::vector<SearchMatch> Searcher::feed(const uint8_t* data, size_t size,
                                        uint64_t absoluteOffset, WorkBudget* budget, size_t* bytesProcessed) {
    std::vector<SearchMatch> matches;
    if (bytesProcessed) *bytesProcessed = 0;
    if (nodes_.empty() || data == nullptr || size == 0) return matches;
    if (size > std::numeric_limits<uint64_t>::max() - absoluteOffset) return matches;
    if (!streaming_ || absoluteOffset != nextOffset_) state_ = 0;
    streaming_ = true;
    size_t processed = 0;
    bool stopped = false;
    for (size_t i = 0; i < size && !stopped; ++i) {
        if (budget && i % 4096 == 0 && !budget->discoveryAllowed()) break;
        state_ = static_cast<size_t>(nodes_[state_].next[data[i]]);
        processed = i + 1;
        for (const auto& metadata : nodes_[state_].outputs) {
            if (budget && !budget->claimMatch()) { stopped = true; break; }
            const uint64_t end = absoluteOffset + static_cast<uint64_t>(i) + 1;
            if (end >= metadata.length) matches.push_back({end - metadata.length, metadata});
        }
    }
    nextOffset_ = absoluteOffset + processed;
    if (bytesProcessed) *bytesProcessed = processed;
    std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        if (a.metadata.kind != b.metadata.kind) return a.metadata.kind == PatternKind::Header;
        return a.metadata.patternId < b.metadata.patternId;
    });
    return matches;
}
