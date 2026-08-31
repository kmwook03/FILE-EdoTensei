#include "carver.hpp"

#include "disk_io.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <tuple>

FileCarver::FileCarver(CarverOptions options) : options_(std::move(options)) {}

FileCarver::FileCarver(const std::string& path) { options_.inputPath = path; }

bool FileCarver::initialize() {
    if (!image_.open(options_.inputPath, lastError_)) return false;
    scanner_.build(registry_.descriptors());
    if (!options_.reportPath.empty() &&
        !report_.open(options_.reportPath, options_.inputPath, image_.size(), lastError_)) return false;
    return true;
}

bool FileCarver::scanRaw(std::vector<RecoveryCandidate>& candidates) {
    constexpr size_t bufferSize = 1024 * 1024;
    std::vector<uint8_t> buffer(bufferSize);
    uint64_t position = 0;
    scanner_.reset();
    while (position < image_.size()) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(bufferSize, image_.size() - position));
        size_t got = 0;
        if (!image_.read(position, buffer.data(), wanted, got, lastError_)) return false;
        if (got == 0) { lastError_ = "input ended before its reported size"; return false; }
        const auto matches = scanner_.feed(buffer.data(), got, position);
        for (const auto& match : matches) {
            if (match.metadata.kind != PatternKind::Header) continue;
            const FormatCarver* carver = registry_.byIndex(match.metadata.formatIndex);
            RecoveryCandidate candidate;
            if (carver && carver->detect(match, candidate)) candidates.push_back(std::move(candidate));
        }
        position += got;
    }
    return true;
}

bool FileCarver::reportResult(const RecoveryResult& result) {
    std::string error;
    if (!report_.writeResult(result, error)) { lastError_ = error; return false; }
    return true;
}

bool FileCarver::processCandidates(std::vector<RecoveryCandidate>& candidates) {
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::tie(a.startOffset, a.formatId) < std::tie(b.startOffset, b.formatId);
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.startOffset == b.startOffset && a.formatId == b.formatId && a.method == b.method;
    }), candidates.end());

    struct Evaluated { RecoveryCandidate candidate; ValidationResult validation; int score; };
    std::vector<Evaluated> evaluated;
    evaluated.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const FormatCarver* carver = registry_.byId(candidate.formatId);
        if (!carver) continue;
        ValidationResult validation = carver->validate(image_, candidate);
        evaluated.push_back({candidate, validation, carver->score(candidate, validation)});
    }

    std::set<size_t> suppressed;
    for (size_t i = 0; i < evaluated.size(); ++i) {
        if (evaluated[i].validation.state != ValidationState::Valid) continue;
        const uint64_t iEnd = evaluated[i].validation.validatedEndOffset;
        for (size_t j = i + 1; j < evaluated.size(); ++j) {
            if (evaluated[j].candidate.startOffset >= iEnd) break;
            if (evaluated[i].candidate.formatId != evaluated[j].candidate.formatId ||
                evaluated[j].validation.state != ValidationState::Valid) continue;
            const size_t loser = evaluated[i].score >= evaluated[j].score ? j : i;
            suppressed.insert(loser);
        }
    }

    for (size_t i = 0; i < evaluated.size(); ++i) {
        auto& item = evaluated[i];
        const FormatCarver* carver = registry_.byId(item.candidate.formatId);
        RecoveryResult result;
        result.candidate = item.candidate;
        result.validationState = item.validation.state;
        result.truncationReason = item.validation.truncationReason;
        result.evidence = item.candidate.evidence;
        result.evidence.insert(result.evidence.end(), item.validation.evidence.begin(), item.validation.evidence.end());
        result.warnings = item.candidate.warnings;
        result.warnings.insert(result.warnings.end(), item.validation.diagnostics.begin(), item.validation.diagnostics.end());
        result.confidence = item.score;
        if (result.candidate.extents.empty() &&
            item.validation.validatedEndOffset > item.candidate.startOffset) {
            result.candidate.extents.push_back(
                {item.candidate.startOffset,
                 item.validation.validatedEndOffset - item.candidate.startOffset, false});
        }
        if (suppressed.count(i)) result.warnings.push_back("suppressed by stronger overlapping candidate");
        const bool eligible = item.validation.state != ValidationState::Invalid &&
                              item.score >= options_.minimumConfidence && !suppressed.count(i);
        if (eligible) {
            std::string error;
            if (!carver->recover(image_, item.candidate, item.validation,
                                 options_.outputDirectory, result, error)) {
                result.warnings.push_back(error);
                lastError_ = error;
                ++summary_.errors;
            } else {
                result.accepted = true;
                ++summary_.accepted;
                std::cout << "[Saved] " << result.outputPath << " (confidence "
                          << result.confidence << ")\n";
            }
        }
        if (!result.accepted) ++summary_.rejected;
        if (!reportResult(result)) return false;
    }
    return true;
}

bool FileCarver::startCarving() {
    std::vector<RecoveryCandidate> candidates;
    bool ntfsDetected = false;
    if (options_.mode != RecoveryMode::Raw) {
        NTFSReader ntfs;
        std::string ntfsError;
        if (ntfs.openImage(options_.inputPath, ntfsError)) {
            ntfsDetected = ntfs.detectVolume(ntfsError);
            if (ntfsDetected) {
                auto metadataCandidates = ntfs.findDeletedFiles(ntfsError);
                candidates.insert(candidates.end(), metadataCandidates.begin(), metadataCandidates.end());
            }
        }
        if (options_.mode == RecoveryMode::Ntfs && !ntfsDetected) {
            lastError_ = ntfsError.empty() ? "no supported NTFS volume found" : ntfsError;
            return false;
        }
    }
    if (options_.mode != RecoveryMode::Ntfs && !scanRaw(candidates)) return false;
    if (!processCandidates(candidates)) return false;
    if (!report_.writeSummary(summary_.accepted, summary_.rejected, summary_.errors, lastError_)) return false;
    return summary_.errors == 0;
}
