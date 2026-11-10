#include "carver.hpp"

#include "disk_io.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <tuple>
#include <utility>

FileCarver::FileCarver(CarverOptions options, WorkBudget::Now now)
    : options_(std::move(options)), budget_(options_.limits, std::move(now)), registry_(options_.zipLimits) {}

bool FileCarver::initialize() {
    if (!image_.open(options_.inputPath, lastError_)) return false;
    scanner_.build(registry_.descriptors());
    if (!options_.reportPath.empty()) {
        std::error_code error;
        if (std::filesystem::equivalent(options_.inputPath, options_.reportPath, error)) {
            lastError_ = "report path refers to the input";
            return false;
        }
        if (!report_.open(options_.reportPath, options_.inputPath, image_.size(), lastError_)) return false;
    }
    return true;
}

bool FileCarver::scanRaw(std::vector<RecoveryCandidate>& candidates) {
    constexpr size_t bufferSize = 1024 * 1024;
    std::vector<uint8_t> buffer(bufferSize);
    uint64_t position = 0;
    summary_.rawScanPerformed = true;
    scanner_.reset();
    while (position < image_.size() && budget_.discoveryAllowed()) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(bufferSize, image_.size() - position));
        size_t got = 0;
        if (!image_.read(position, buffer.data(), wanted, got, lastError_)) return false;
        if (got == 0) { lastError_ = "input ended before its reported size"; return false; }
        size_t processed = 0;
        const auto matches = scanner_.feed(buffer.data(), got, position, &budget_, &processed);
        position += processed;
        summary_.rawScannedBytes = position;
        for (const auto& match : matches) {
            if (!budget_.check()) break;
            if (match.metadata.kind != PatternKind::Header) continue;
            const FormatCarver* carver = registry_.byIndex(match.metadata.formatIndex);
            RecoveryCandidate candidate;
            if (carver && carver->detect(match, candidate)) {
                if (!budget_.claimCandidate()) break;
                candidates.push_back(std::move(candidate));
            }
        }
        if (processed != got) break;
    }
    summary_.rawScanComplete = position == image_.size() && budget_.discoveryAllowed();
    return true;
}

bool FileCarver::processCandidates(std::vector<RecoveryCandidate>& candidates) {
    auto key = [](const RecoveryCandidate& c) {
        return std::tie(c.startOffset, c.formatId, c.method, c.logicalSize, c.extents, c.residentData);
    };
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        return key(a) == key(b);
    }), candidates.end());

    struct Evaluated {
        const FormatCarver* carver = nullptr;
        ValidationResult validation;
        std::vector<Extent> extents;
        int score = 0;
        bool suppressed = false;
    };
    std::vector<Evaluated> evaluated(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto& item = evaluated[i];
        if (!budget_.check()) {
            item.validation.state = ValidationState::NotValidated;
            item.validation.stopReason = toString(budget_.stopReason());
            continue;
        }
        item.carver = registry_.byId(candidates[i].formatId);
        if (!item.carver) { item.validation.diagnostics.push_back("unknown format"); continue; }
        item.validation = item.carver->validate(image_, candidates[i], &budget_);
        if (!item.validation.stopReason.empty() && summary_.stopReason.empty())
            summary_.stopReason = item.validation.stopReason;
        if (item.validation.stopReason == "io_error") {
            ++summary_.errors;
            lastError_ = item.validation.diagnostics.empty() ? "input read failed" : item.validation.diagnostics.back();
        }
        item.score = item.carver->score(candidates[i], item.validation);
        if (item.validation.validatedLength > 0) {
            CandidateReader reader(image_, candidates[i], &budget_);
            std::string error;
            if (!reader.physicalExtents(0, item.validation.validatedLength, item.extents, error)) {
                if (!budget_.check()) item.validation.stopReason = toString(budget_.stopReason());
                else {
                    item.validation.state = ValidationState::Invalid;
                    item.validation.validatedLength = 0;
                    item.validation.diagnostics.push_back(error);
                }
            }
        }
    }

    // Stronger candidates claim their actual ranges; fragment gaps remain available.
    std::vector<size_t> order(evaluated.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (evaluated[a].score != evaluated[b].score) return evaluated[a].score > evaluated[b].score;
        const bool aMetadata = candidates[a].method != RecoveryMethod::RawContiguous &&
                               candidates[a].method != RecoveryMethod::RawReassembled;
        const bool bMetadata = candidates[b].method != RecoveryMethod::RawContiguous &&
                               candidates[b].method != RecoveryMethod::RawReassembled;
        return aMetadata && !bMetadata;
    });
    std::map<std::string, PhysicalCoverage> coverage;
    for (size_t index : order) {
        if (!budget_.check()) break;
        auto& item = evaluated[index];
        if (item.validation.state != ValidationState::Valid) continue;
        auto& occupied = coverage[candidates[index].formatId];
        if (occupied.overlaps(item.extents)) item.suppressed = true;
        else if (!occupied.add(item.extents)) {
            item.validation.state = ValidationState::Invalid;
            item.validation.diagnostics.push_back("physical extent overflow");
        }
    }

    for (size_t i = 0; i < evaluated.size(); ++i) {
        auto& item = evaluated[i];
        RecoveryResult result = makeRecoveryResult(candidates[i], item.validation, item.score);
        if (item.validation.validatedLength > 0) {
            result.candidate.extents = item.extents;
            result.candidate.logicalSize = item.validation.validatedLength;
        }
        if (item.suppressed) result.warnings.push_back("suppressed by stronger overlapping candidate");
        const bool eligible = (item.validation.state == ValidationState::Valid ||
                               item.validation.state == ValidationState::Truncated) &&
                              item.score >= options_.minimumConfidence && !item.suppressed;
        if (!result.skipped && eligible) {
            std::string error;
            if (!item.carver->recover(image_, candidates[i], item.validation,
                                     options_.outputDirectory, result, error, &budget_)) {
                if (!budget_.check()) {
                    result.skipped = true;
                    result.stopReason = toString(budget_.stopReason());
                } else {
                    result.warnings.push_back(error);
                    lastError_ = error;
                    ++summary_.errors;
                }
            } else {
                result.accepted = true;
                ++summary_.accepted;
                std::cout << "[Saved] " << result.outputPath << " (confidence " << result.confidence << ")\n";
            }
        }
        if (result.skipped) ++summary_.skipped;
        if (!result.accepted) ++summary_.rejected;
        if (!report_.writeResult(result, lastError_)) return false;
    }
    return true;
}

bool FileCarver::startCarving() {
    budget_.reset();
    summary_ = {};
    lastError_.clear();
    image_.setBudget(&budget_);
    std::vector<RecoveryCandidate> candidates;
    bool discoveryOk = true;
    bool ntfsDetected = false;
    if (options_.mode != RecoveryMode::Raw && budget_.discoveryAllowed()) {
        NTFSReader ntfs;
        std::string ntfsError;
        if (ntfs.openImage(options_.inputPath, ntfsError, &budget_)) {
            ntfsDetected = ntfs.detectVolume(ntfsError);
            if (ntfsDetected) {
                auto metadataCandidates = ntfs.findDeletedFiles(ntfsError, registry_);
                candidates.insert(candidates.end(), std::make_move_iterator(metadataCandidates.begin()),
                                  std::make_move_iterator(metadataCandidates.end()));
                if (!ntfsError.empty() && budget_.check()) { lastError_ = ntfsError; discoveryOk = false; }
            }
        }
        if (options_.mode == RecoveryMode::Ntfs && !ntfsDetected && budget_.check()) {
            lastError_ = ntfsError.empty() ? "no supported NTFS volume found" : ntfsError;
            discoveryOk = false;
        }
    }
    if (options_.mode != RecoveryMode::Ntfs && budget_.discoveryAllowed() && !scanRaw(candidates) && budget_.check())
        discoveryOk = false;
    if (!discoveryOk) ++summary_.errors;
    if (!processCandidates(candidates)) ++summary_.errors;
    budget_.check();
    summary_.discovered = budget_.candidateCount();
    summary_.matches = budget_.matchCount();
    summary_.elapsedMilliseconds = budget_.elapsedMilliseconds();
    if (budget_.stopReason() != StopReason::None) summary_.stopReason = toString(budget_.stopReason());
    if (summary_.errors != 0 && summary_.stopReason.empty()) summary_.stopReason = "io_error";
    summary_.complete = summary_.errors == 0 && summary_.stopReason.empty();
    if (!summary_.complete && lastError_.empty()) lastError_ = summary_.stopReason;
    std::string reportError;
    if (!report_.writeSummary(summary_, reportError)) {
        lastError_ = reportError;
        summary_.complete = false;
        ++summary_.errors;
        return false;
    }
    return summary_.complete;
}
