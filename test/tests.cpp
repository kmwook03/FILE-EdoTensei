#include "disk_io.hpp"
#include "report.hpp"
#include "searcher.hpp"
#include "signature.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

void scannerTests() {
    std::vector<FormatDescriptor> descriptors = {
        {"one", "one", "one", {{"one.ab", PatternKind::Header, {'a', 'b'}},
                                   {"one.b", PatternKind::Footer, {'b'}},
                                   {"one.zero", PatternKind::StructuralAnchor, {0, 'x'}}}, 100},
        {"two", "two", "two", {{"two.ab", PatternKind::Header, {'a', 'b'}}}, 100}
    };
    Searcher scanner;
    scanner.build(descriptors);
    const uint8_t first[] = {'z', 'a'};
    const uint8_t second[] = {'b', 0, 'x'};
    expect(scanner.feed(first, sizeof(first), 100).empty(), "partial chunk has no premature match");
    const auto matches = scanner.feed(second, sizeof(second), 102);
    size_t atAb = 0;
    bool zero = false;
    for (const auto& match : matches) {
        if (match.offset == 101 && match.metadata.patternId.find(".ab") != std::string::npos) ++atAb;
        if (match.offset == 103 && match.metadata.patternId == "one.zero") zero = true;
    }
    expect(atAb == 2, "multiple outputs and boundary match retained");
    expect(zero, "binary-zero pattern matched at absolute offset");

    const std::vector<uint8_t> overlap = {'a', 'b', 'a', 'b'};
    const auto all = scanner.findAll(overlap, 500);
    size_t headers = 0;
    for (const auto& match : all) if (match.metadata.kind == PatternKind::Header) ++headers;
    expect(headers == 4, "overlapping/repeated patterns produce every output");
    expect(Searcher::search(overlap, {'b', 'a'}) == 1, "BMH compatibility helper works");
}

void runlistTests() {
    std::vector<MFT_Segment> segments;
    std::string error;
    const uint8_t valid[] = {0x11, 0x03, 0x05, 0x01, 0x02, 0x00};
    expect(NTFSReader::parseDataRuns(valid, sizeof(valid), segments, error), "valid data runs parse");
    expect(segments.size() == 2 && segments[0].lcn == 5 && segments[0].length == 3 &&
           segments[1].sparse && segments[1].length == 2, "sparse and physical runs decoded");
    const uint8_t invalid[] = {0x99, 1};
    expect(!NTFSReader::parseDataRuns(invalid, sizeof(invalid), segments, error), "malformed run rejected");

    std::vector<uint8_t> record(1024, 0);
    record[4] = 48;
    record[6] = 3;
    record[48] = 0xaa; record[49] = 0xbb;
    record[50] = 1; record[51] = 2;
    record[52] = 3; record[53] = 4;
    record[510] = 0xaa; record[511] = 0xbb;
    record[1022] = 0xaa; record[1023] = 0xbb;
    expect(NTFSReader::applyFixups(record, 512, error), "valid update sequence fixups apply");
    expect(record[510] == 1 && record[511] == 2 && record[1022] == 3 && record[1023] == 4,
           "fixup replacement words restored");
    record[510] = 0;
    expect(!NTFSReader::applyFixups(record, 512, error), "corrupt fixup is rejected");
}

void reportTests() {
    expect(JsonlReport::escape("a\n\"\\") == "a\\n\\\"\\\\", "JSON escaping is stable");
}
}

int main() {
    scannerTests();
    runlistTests();
    reportTests();
    if (failures) return EXIT_FAILURE;
    std::cout << "All unit tests passed\n";
    return EXIT_SUCCESS;
}
