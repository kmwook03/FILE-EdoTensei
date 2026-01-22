#include "searcher.hpp"
#include <algorithm>

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
