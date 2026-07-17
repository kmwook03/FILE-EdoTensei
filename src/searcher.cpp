#include "searcher.hpp"
#include <algorithm>
#include <queue>

Searcher::Node::Node() : fail(0) {
    std::fill(std::begin(next), std::end(next), -1);
}

void Searcher::build(const std::vector<FormatDescriptor>& formats) {
    nodes_.clear();
    nodes_.push_back(Node());
    maxPatternLength_ = 0;

    for (size_t formatIndex = 0; formatIndex < formats.size(); ++formatIndex) {
        for (const auto& pattern : formats[formatIndex].patterns) {
            if (pattern.bytes.empty()) continue;

            size_t nodeIndex = 0;
            for (uint8_t byte : pattern.bytes) {
                int& nextNode = nodes_[nodeIndex].next[byte];
                if (nextNode == -1) {
                    nextNode = static_cast<int>(nodes_.size());
                    nodes_.push_back(Node());
                }
                nodeIndex = static_cast<size_t>(nextNode);
            }

            nodes_[nodeIndex].outputs.push_back({formatIndex, pattern.kind, pattern.bytes.size()});
            maxPatternLength_ = std::max(maxPatternLength_, pattern.bytes.size());
        }
    }

    std::queue<size_t> pending;
    for (size_t byte = 0; byte < 256; ++byte) {
        int child = nodes_[0].next[byte];
        if (child == -1) {
            nodes_[0].next[byte] = 0;
        } else {
            nodes_[static_cast<size_t>(child)].fail = 0;
            pending.push(static_cast<size_t>(child));
        }
    }

    while (!pending.empty()) {
        size_t current = pending.front();
        pending.pop();

        for (size_t byte = 0; byte < 256; ++byte) {
            int child = nodes_[current].next[byte];
            if (child == -1) {
                nodes_[current].next[byte] = nodes_[nodes_[current].fail].next[byte];
                continue;
            }

            size_t childIndex = static_cast<size_t>(child);
            size_t failIndex = static_cast<size_t>(nodes_[nodes_[current].fail].next[byte]);
            nodes_[childIndex].fail = failIndex;
            nodes_[childIndex].outputs.insert(nodes_[childIndex].outputs.end(),
                                              nodes_[failIndex].outputs.begin(),
                                              nodes_[failIndex].outputs.end());
            pending.push(childIndex);
        }
    }
}

std::vector<SearchMatch> Searcher::findAll(const std::vector<uint8_t>& haystack, size_t startOffset) const {
    std::vector<SearchMatch> matches;
    if (nodes_.empty() || startOffset >= haystack.size()) return matches;

    size_t nodeIndex = 0;
    for (size_t i = startOffset; i < haystack.size(); ++i) {
        nodeIndex = static_cast<size_t>(nodes_[nodeIndex].next[haystack[i]]);

        for (const auto& metadata : nodes_[nodeIndex].outputs) {
            size_t start = i + 1 - metadata.length;
            if (start >= startOffset) {
                matches.push_back({start, metadata});
            }
        }
    }

    std::sort(matches.begin(), matches.end(), [](const SearchMatch& lhs, const SearchMatch& rhs) {
        if (lhs.offset != rhs.offset) return lhs.offset < rhs.offset;
        if (lhs.metadata.kind != rhs.metadata.kind) {
            return lhs.metadata.kind == PatternKind::Header;
        }
        if (lhs.metadata.formatIndex != rhs.metadata.formatIndex) {
            return lhs.metadata.formatIndex < rhs.metadata.formatIndex;
        }
        return lhs.metadata.length > rhs.metadata.length;
    });

    return matches;
}

size_t Searcher::maxPatternLength() const {
    return maxPatternLength_;
}

int64_t Searcher::search(const std::vector<uint8_t>& haystack,
                         const std::vector<uint8_t>& needle,
                         size_t startOffset) {
    size_t n = haystack.size();
    size_t m = needle.size();

    if (m == 0 || n < m + startOffset) return -1;

    // create skip table
    size_t skip[256];
    for (int i = 0; i < 256; ++i) {
        skip[i] = m;
    }

    for (size_t i = 0; i < m - 1; ++i) {
        skip[needle[i]] = m - 1 - i;
    }

    // start searching
    size_t i = startOffset + m - 1;
    
    while (i < n) {
        size_t k = 0;
        while (k < m && haystack[i - k] == needle[m - 1 - k]) {
            k++;
        }

        if (k == m) {
            return i - m + 1; // Match found
        }

        i += skip[haystack[i]];
    }
    
    return -1; // No match found
}
