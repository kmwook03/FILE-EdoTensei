#include "searcher.hpp"

#include <algorithm>
#include <limits>
#include <queue>

Searcher::Node::Node() : fail(0) { next.fill(-1); }

void Searcher::build(const std::vector<FormatDescriptor>& formats) {
    nodes_.assign(1, Node());
    maxPatternLength_ = 0;
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
            maxPatternLength_ = std::max(maxPatternLength_, pattern.bytes.size());
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
                                        uint64_t absoluteOffset) {
    std::vector<SearchMatch> matches;
    if (nodes_.empty() || data == nullptr || size == 0) return matches;
    if (!streaming_ || absoluteOffset != nextOffset_) state_ = 0;
    streaming_ = true;
    for (size_t i = 0; i < size; ++i) {
        state_ = static_cast<size_t>(nodes_[state_].next[data[i]]);
        for (const auto& metadata : nodes_[state_].outputs) {
            const uint64_t end = absoluteOffset + static_cast<uint64_t>(i) + 1;
            if (end >= metadata.length) matches.push_back({end - metadata.length, metadata});
        }
    }
    nextOffset_ = absoluteOffset + size;
    std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        if (a.metadata.kind != b.metadata.kind) return a.metadata.kind == PatternKind::Header;
        return a.metadata.patternId < b.metadata.patternId;
    });
    return matches;
}

std::vector<SearchMatch> Searcher::findAll(const std::vector<uint8_t>& data,
                                           uint64_t absoluteOffset) const {
    Searcher copy = *this;
    copy.reset();
    return copy.feed(data.data(), data.size(), absoluteOffset);
}

size_t Searcher::maxPatternLength() const { return maxPatternLength_; }

int64_t Searcher::search(const std::vector<uint8_t>& haystack,
                         const std::vector<uint8_t>& needle, size_t startOffset) {
    if (needle.empty() || startOffset > haystack.size() ||
        needle.size() > haystack.size() - startOffset) return -1;
    std::array<size_t, 256> skip;
    skip.fill(needle.size());
    for (size_t i = 0; i + 1 < needle.size(); ++i) skip[needle[i]] = needle.size() - i - 1;
    size_t i = startOffset + needle.size() - 1;
    while (i < haystack.size()) {
        size_t k = 0;
        while (k < needle.size() && haystack[i - k] == needle[needle.size() - 1 - k]) ++k;
        if (k == needle.size()) return static_cast<int64_t>(i - needle.size() + 1);
        const size_t advance = skip[haystack[i]];
        if (i > std::numeric_limits<size_t>::max() - advance) break;
        i += advance;
    }
    return -1;
}
