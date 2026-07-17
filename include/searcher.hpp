#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>
#include "signature.hpp"

struct PatternMetadata {
    size_t formatIndex;
    PatternKind kind;
    size_t length;
};

struct SearchMatch {
    size_t offset;
    PatternMetadata metadata;
};

class Searcher {
public:
    void build(const std::vector<FormatDescriptor>& formats);
    std::vector<SearchMatch> findAll(const std::vector<uint8_t>& haystack, size_t startOffset = 0) const;
    size_t maxPatternLength() const;

    /**
     * @brief Boyer-Moore-Horspol string search algorithm
     * 
     * @param haystack The data to search within
     * @param needle The byte pattern to search for
     * @param startOffset The offset in haystack to start searching from
     * @return index of the first occurrence of needle in haystack after startOffset, or -1 if not found
     */
    static int64_t search(const std::vector<uint8_t>& haystack,
                          const std::vector<uint8_t>& needle,
                          size_t startOffset = 0);

private:
    struct Node {
        int next[256];
        size_t fail;
        std::vector<PatternMetadata> outputs;

        Node();
    };

    std::vector<Node> nodes_;
    size_t maxPatternLength_ = 0;
};
