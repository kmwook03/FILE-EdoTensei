#include "candidate_reader.hpp"
#include "carver.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
int failures = 0;
void expect(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        char pattern[] = "/tmp/fileedo-p0-tests-XXXXXX";
        const char* directory = mkdtemp(pattern);
        if (!directory) throw std::runtime_error("cannot create test directory");
        path = directory;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot create fixture");
}

std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void readerTests() {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "source.img";
    std::vector<uint8_t> bytes(64);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i);
    writeBytes(path, bytes);
    ImageReader image;
    std::string error;
    expect(image.open(path.string(), error), "reader fixture opens");

    RecoveryCandidate fragmented;
    fragmented.extents = {{40, 3, false}, {4, 4, false}, {0, 2, true}, {20, 2, false}};
    fragmented.logicalSize = 10;
    CandidateReader reader(image, fragmented);
    std::vector<uint8_t> output(10);
    expect(reader.valid() && reader.size() == 10 && reader.readExact(0, output.data(), output.size(), error),
           "reverse and sparse extents form a bounded logical stream");
    expect(output == std::vector<uint8_t>({40, 41, 42, 4, 5, 6, 7, 0, 0, 20}),
           "logical stream preserves order and inserts sparse zeroes");
    std::vector<Extent> sliced;
    expect(reader.physicalExtents(2, 8, sliced, error) && sliced == std::vector<Extent>(
               {{42, 1, false}, {4, 4, false}, {0, 2, true}, {20, 1, false}}),
           "range slicing trims both fragment boundaries");
    expect(reader.readExact(10, nullptr, 0, error), "empty read at EOF is valid");
    expect(!reader.readExact(9, output.data(), 2, error), "read cannot exceed declared logical size");
    expect(!reader.readExact(std::numeric_limits<uint64_t>::max(), output.data(), 2, error),
           "overflowing read range is rejected");
    expect(!reader.physicalExtents(1, std::numeric_limits<uint64_t>::max(), sliced, error) && sliced.empty(),
           "overflowing slice produces no extents");

    RecoveryCandidate raw;
    raw.startOffset = 60;
    CandidateReader rawReader(image, raw);
    expect(rawReader.size() == 4 && rawReader.readExact(0, output.data(), 4, error) && output[3] == 63,
           "raw length is the remaining image length");
    raw.logicalSize = 5;
    expect(!CandidateReader(image, raw).valid(), "declared raw length beyond input is rejected");
    raw.startOffset = std::numeric_limits<uint64_t>::max();
    expect(!CandidateReader(image, raw).valid(), "raw offset outside image is rejected");

    RecoveryCandidate invalid;
    invalid.extents = {{63, 2, false}};
    expect(!CandidateReader(image, invalid).valid(), "physical extent outside image is rejected");
    invalid.extents = {{std::numeric_limits<uint64_t>::max(), 2, false}};
    expect(!CandidateReader(image, invalid).valid(), "overflowing physical range is rejected");
    invalid.extents = {{0, std::numeric_limits<uint64_t>::max(), true}, {0, 1, true}};
    expect(!CandidateReader(image, invalid).valid(), "logical extent sum overflow is rejected");
    invalid.extents = {{0, 0, true}};
    expect(!CandidateReader(image, invalid).valid(), "zero-length extent is rejected");
    invalid.extents = fragmented.extents;
    invalid.logicalSize = 12;
    expect(!CandidateReader(image, invalid).valid(), "extent stream cannot promise missing bytes");

    RecoveryCandidate large;
    const uint64_t fourGiB = 1ULL << 32;
    large.extents = {{std::numeric_limits<uint64_t>::max(), fourGiB + 10, true}};
    CandidateReader largeReader(image, large);
    std::array<uint8_t, 2> tail{{1, 1}};
    expect(largeReader.size() == fourGiB + 10 && largeReader.readExact(fourGiB + 8, tail.data(), 2, error) &&
           tail[0] == 0 && tail[1] == 0, "logical reads above 4GiB work without materializing the file");

    const std::vector<uint8_t> jpeg = {0xff, 0xd8, 0xff, 0xe0, 0, 2, 0xff, 0xd9};
    RecoveryCandidate resident;
    resident.startOffset = std::numeric_limits<uint64_t>::max();
    resident.residentData = jpeg;
    FormatRegistry registry;
    const auto* carver = registry.byId("jpeg");
    const auto validation = carver->validate(image, resident);
    expect(validation.state == ValidationState::Valid && validation.validatedLength == jpeg.size(),
           "validated length is independent of the physical start offset");
    RecoveryResult result;
    expect(carver->recover(image, resident, validation, (temporary.path / "resident").string(), result, error) &&
           readBytes(result.outputPath) == jpeg, "resident bytes recover without a physical extent mapping");
    resident.startOffset = 0;
    resident.residentData.insert(resident.residentData.end(), 4, 0);
    resident.extents = {{0, 12, false}};
    const auto paddedValidation = carver->validate(image, resident);
    expect(carver->recover(image, resident, paddedValidation, (temporary.path / "padded").string(), result, error) &&
           readBytes(result.outputPath) == jpeg && result.candidate.extents == std::vector<Extent>({{0, 8, false}}),
           "resident extraction trims trailing data and its reported physical extents");

    bytes[60] = jpeg[0];
    std::copy(jpeg.begin() + 1, jpeg.end(), bytes.begin() + 10);
    writeBytes(path, bytes);
    RecoveryCandidate splitHeader;
    splitHeader.extents = {{60, 1, false}, {10, 7, false}};
    CandidateReader splitReader(image, splitHeader);
    expect(registry.identify(splitReader) == "jpeg", "registry identifies a header across physical fragments");
    const auto splitValidation = carver->validate(image, splitHeader);
    expect(splitValidation.state == ValidationState::Valid && splitValidation.validatedLength == jpeg.size(),
           "format parser validates logical content across fragments");
    expect(carver->recover(image, splitHeader, splitValidation, (temporary.path / "split").string(), result, error) &&
           readBytes(result.outputPath) == jpeg && result.candidate.extents == splitHeader.extents,
           "reverse fragments are copied in logical order and reported accurately");

    {
        std::fstream largeFile(path, std::ios::binary | std::ios::in | std::ios::out);
        largeFile.seekp(static_cast<std::streamoff>(fourGiB + 2));
        largeFile.write("ok", 2);
        expect(static_cast<bool>(largeFile), "sparse fixture above 4GiB is created");
    }
    expect(image.open(path.string(), error), "large physical input reopens");
    raw.startOffset = fourGiB + 2;
    raw.logicalSize = 2;
    CandidateReader physicalLargeReader(image, raw);
    expect(physicalLargeReader.readExact(0, tail.data(), tail.size(), error) && tail[0] == 'o' && tail[1] == 'k',
           "physical reads above 4GiB retain all offset bits");
}

void coverageTests() {
    PhysicalCoverage coverage;
    expect(coverage.add({{100, 10, false}, {20, 10, false}, {0, 1000, true}}), "coverage accepts reversed ranges");
    expect(!coverage.overlaps({{50, 5, false}}), "fragment gap stays available to another file");
    expect(!coverage.overlaps({{30, 70, false}}), "adjacent ranges do not overlap");
    expect(coverage.overlaps({{25, 1, false}}) && coverage.overlaps({{109, 2, false}}), "both physical fragments conflict");
    expect(!coverage.overlaps({{100, 10, true}}), "sparse bytes do not claim physical space");
    expect(coverage.add({{30, 70, false}}) && coverage.overlaps({{50, 1, false}}), "coverage merges adjacent claims");
    expect(!coverage.add({{std::numeric_limits<uint64_t>::max(), 1, false}}), "coverage rejects overflowing intervals");
}

void budgetTests() {
    WorkBudget::Clock::time_point now{};
    WorkLimits limits;
    limits.timeLimit = std::chrono::milliseconds(10);
    limits.maximumCandidates = 1;
    WorkBudget budget(limits, [&] { return now; });
    expect(budget.claimCandidate() && !budget.claimCandidate(), "candidate cap stops further discovery");
    expect(budget.stopReason() == StopReason::CandidateLimit && budget.check() && !budget.discoveryAllowed(),
           "candidate cap permits processing retained candidates");
    now += std::chrono::milliseconds(10);
    expect(!budget.check() && budget.stopReason() == StopReason::TimeLimit, "deadline stops processing at its boundary");
    now -= std::chrono::milliseconds(10);
    expect(!budget.check(), "deadline exhaustion remains latched");
    budget.reset();
    expect(budget.check() && budget.candidateCount() == 0, "reset starts a new job");
    budget.cancel();
    expect(!budget.check() && budget.stopReason() == StopReason::Cancelled, "explicit cancellation stops processing");

    limits.maximumMatches = 1;
    limits.timeLimit = std::chrono::milliseconds(0);
    WorkBudget matches(limits, [&] { return now; });
    Searcher scanner;
    scanner.build({{"a", "a", "a", {{"a.header", PatternKind::Header, {'a'}}}, 10}});
    const uint8_t data[] = {'a', 'a', 'a'};
    size_t processed = 0;
    const auto found = scanner.feed(data, sizeof(data), 100, &matches, &processed);
    expect(found.size() == 1 && processed == 2 && matches.stopReason() == StopReason::MatchLimit,
           "match cap retains prior matches and records actual scan progress");
    expect(matches.claimCandidate() && matches.check(), "retained matches can become candidates after the match cap");
}

void interruptionTests() {
    TemporaryDirectory temporary;
    const auto source = temporary.path / "source.img";
    const std::vector<uint8_t> jpeg = {0xff, 0xd8, 0xff, 0xe0, 0, 2, 0xff, 0xd9};
    std::vector<uint8_t> content = jpeg;
    content.insert(content.end(), jpeg.begin(), jpeg.end());
    writeBytes(source, content);
    CarverOptions options;
    options.inputPath = source.string();
    options.outputDirectory = (temporary.path / "output").string();
    options.reportPath = (temporary.path / "report.jsonl").string();
    options.mode = RecoveryMode::Raw;
    options.limits.timeLimit = std::chrono::milliseconds(10);
    const auto firstOutput = fs::path(options.outputDirectory) / "recovered_0.jpg";
    FileCarver carver(options, [&] {
        return WorkBudget::Clock::time_point{} + std::chrono::milliseconds(fs::exists(firstOutput) ? 10 : 0);
    });
    expect(carver.initialize() && !carver.startCarving(), "deadline during extraction yields an incomplete job");
    expect(carver.summary().accepted == 1 && carver.summary().skipped == 1 && carver.summary().errors == 0 &&
           !carver.summary().complete && carver.summary().stopReason == "time_limit", "deadline reports completed and pending work separately");
    expect(readBytes(firstOutput) == jpeg && !fs::exists(fs::path(options.outputDirectory) / "recovered_8.jpg"),
           "completed recovery survives the deadline and pending recovery is not published");

    ImageReader image;
    std::string error;
    expect(image.open(source.string(), error), "interruption source opens");
    RecoveryCandidate candidate;
    candidate.residentData = jpeg;
    WorkLimits limits;
    limits.timeLimit = std::chrono::milliseconds(3);
    int ticks = 0;
    WorkBudget validationBudget(limits, [&] {
        return WorkBudget::Clock::time_point{} + std::chrono::milliseconds(ticks++);
    });
    FormatRegistry registry;
    const auto result = registry.byId("jpeg")->validate(image, candidate, &validationBudget);
    expect(result.state == ValidationState::NotValidated && result.validatedLength == 0 &&
           result.stopReason == "time_limit" && result.truncationReason.empty(),
           "budget exhaustion is not reported as damaged or truncated source data");

    const auto destination = temporary.path / "partial";
    fs::create_directory(destination);
    const auto partial = destination / "result.bin.tmp";
    limits.timeLimit = std::chrono::milliseconds(10);
    WorkBudget writeBudget(limits, [&] {
        return WorkBudget::Clock::time_point{} + std::chrono::milliseconds(
            fs::exists(partial) && fs::file_size(partial) > 0 ? 10 : 0);
    });
    std::string finalPath;
    uint64_t byteCount = 99;
    expect(!writeRecoveredFile(image, {{0, content.size(), false}}, content.size(), destination.string(),
                               "result.bin", finalPath, byteCount, error, nullptr, &writeBudget),
           "deadline after writing interrupts publication");
    expect(finalPath.empty() && byteCount == 0 && fs::is_empty(destination), "interrupted temporary output is removed");

    // Change the source after open: this is an I/O short read, not a carved-file truncation.
    std::ofstream(source, std::ios::binary | std::ios::trunc).close();
    RecoveryCandidate raw;
    const auto ioResult = registry.byId("jpeg")->validate(image, raw);
    expect(ioResult.state == ValidationState::NotValidated && ioResult.stopReason == "io_error" &&
           ioResult.validatedLength == 0 && ioResult.truncationReason.empty(),
           "source read failure is distinct from candidate corruption and budget exhaustion");
}
}

int main() {
    readerTests();
    coverageTests();
    budgetTests();
    interruptionTests();
    if (failures) return EXIT_FAILURE;
    std::cout << "All P0 unit tests passed\n";
    return EXIT_SUCCESS;
}
