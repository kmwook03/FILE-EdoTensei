#pragma once
#include <cstdint>
#include <array>
#include <cstddef>
#include <string>
#include <vector>
#include "signature.hpp"
#include "work_budget.hpp"

struct PatternMetadata {
    size_t formatIndex;
    std::string patternId;
    PatternKind kind;
    size_t length;
};

struct SearchMatch {
    uint64_t offset;
    PatternMetadata metadata;
};

class Searcher {
public:
    void build(const std::vector<FormatDescriptor>& formats);
    void reset();
    std::vector<SearchMatch> feed(const uint8_t* data, size_t size, uint64_t absoluteOffset,
                                  WorkBudget* budget = nullptr, size_t* bytesProcessed = nullptr);

private:
    struct Node {
        std::array<int, 256> next;
        size_t fail;
        std::vector<PatternMetadata> outputs;

        Node();
    };

    std::vector<Node> nodes_;
    size_t state_ = 0;
    uint64_t nextOffset_ = 0;
    bool streaming_ = false;
};
