#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

class Searcher {
public:
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
};
