#include "format_carver.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void put(std::vector<uint8_t>& bytes, size_t at, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}
std::vector<uint8_t> makeZip() {
    const std::vector<uint8_t> payload(2 * 1024 * 1024, 'a');
    const uint32_t crc = static_cast<uint32_t>(crc32(0, payload.data(), static_cast<uInt>(payload.size())));
    std::vector<uint8_t> bytes(30, 0);
    put(bytes, 0, 0x04034b50, 4);
    put(bytes, 4, 10, 2);
    put(bytes, 14, crc, 4);
    put(bytes, 18, payload.size(), 4);
    put(bytes, 22, payload.size(), 4);
    put(bytes, 26, 1, 2);
    bytes.push_back('a');
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const size_t directory = bytes.size();
    bytes.resize(bytes.size() + 47, 0);
    put(bytes, directory, 0x02014b50, 4);
    put(bytes, directory + 4, 10, 2);
    put(bytes, directory + 6, 10, 2);
    put(bytes, directory + 16, crc, 4);
    put(bytes, directory + 20, payload.size(), 4);
    put(bytes, directory + 24, payload.size(), 4);
    put(bytes, directory + 28, 1, 2);
    bytes.back() = 'a';
    const size_t end = bytes.size();
    bytes.resize(bytes.size() + 22, 0);
    put(bytes, end, 0x06054b50, 4);
    put(bytes, end + 8, 1, 2);
    put(bytes, end + 10, 1, 2);
    put(bytes, end + 12, 47, 4);
    put(bytes, end + 16, directory, 4);
    return bytes;
}
}

int main() {
    char pattern[] = "/tmp/fileedo-p1-unit-XXXXXX";
    const char* temporary = mkdtemp(pattern);
    if (!temporary) throw std::runtime_error("cannot create temporary directory");
    const std::filesystem::path root(temporary);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    const auto zip = makeZip();
    const size_t split = 100001;
    // Split the header and payload across reversed physical ranges. P1 does not
    // discover these ranges, but existing NTFS extent mappings must still work.
    std::vector<uint8_t> physical(zip.begin() + split, zip.end());
    physical.insert(physical.end(), 512, 0);
    const uint64_t start = physical.size();
    physical.insert(physical.end(), zip.begin(), zip.begin() + split);
    const auto source = root / "image";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(physical.data()), static_cast<std::streamsize>(physical.size()));
    }
    ImageReader image;
    std::string error;
    expect(image.open(source.string(), error), "source opens");
    RecoveryCandidate candidate;
    candidate.formatId = "zip";
    candidate.startOffset = start;
    candidate.extents = {{start, split, false}, {0, zip.size() - split, false}};
    candidate.logicalSize = zip.size();
    FormatRegistry registry;
    const auto* carver = registry.byId("zip");
    const auto valid = carver->validate(image, candidate);
    expect(valid.state == ValidationState::Valid && valid.validatedLength == zip.size(),
           "ZIP CRC validation follows logical extents");
    RecoveryResult recovered;
    expect(carver->recover(image, candidate, valid, (root / "out").string(), recovered, error),
           "validated ZIP extracts over reversed extents");
    std::ifstream input(recovered.outputPath, std::ios::binary);
    const std::vector<uint8_t> output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    expect(output == zip, "extraction preserves every original byte");

    WorkLimits limits;
    limits.timeLimit = std::chrono::milliseconds(1);
    size_t checks = 0;
    WorkBudget measuring(limits, [&] { ++checks; return WorkBudget::Clock::time_point{}; });
    expect(carver->validate(image, candidate, &measuring).state == ValidationState::Valid,
           "unexpired budget permits verification");
    const size_t expireAt = checks * 3 / 4;
    checks = 0;
    WorkBudget expiring(limits, [&] {
        return WorkBudget::Clock::time_point{} + std::chrono::milliseconds(++checks >= expireAt ? 1 : 0);
    });
    const auto interrupted = carver->validate(image, candidate, &expiring);
    expect(interrupted.state == ValidationState::NotValidated && interrupted.validatedLength == 0 &&
           interrupted.stopReason == "time_limit" && interrupted.truncationReason.empty(),
           "deadline during ZIP verification remains unfinished, without a partial success");
    expect(!carver->recover(image, candidate, interrupted, (root / "interrupted").string(), recovered, error) &&
           !std::filesystem::exists(root / "interrupted"), "unfinished ZIP is never published");

    RecoveryCandidate resident;
    resident.residentData = zip;
    const auto crcInvalid = [&] {
        resident.residentData[100] ^= 0x80;
        return carver->validate(image, resident);
    }();
    expect(crcInvalid.state == ValidationState::Invalid && crcInvalid.validatedLength == 0,
           "corrupt resident ZIP is rejected");
    expect(!carver->recover(image, resident, crcInvalid, (root / "invalid").string(), recovered, error),
           "invalid ZIP cannot be extracted through direct API calls");
    auto truncated = valid;
    truncated.state = ValidationState::Truncated;
    expect(!carver->recover(image, candidate, truncated, (root / "truncated").string(), recovered, error),
           "ZIP rejects the partial-output policy used by legacy formats");
    if (failures) return EXIT_FAILURE;
    std::cout << "All P1 unit tests passed\n";
    return EXIT_SUCCESS;
}
