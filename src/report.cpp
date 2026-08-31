#include "report.hpp"

#include <iomanip>
#include <sstream>

std::string JsonlReport::escape(const std::string& value) {
    std::ostringstream output;
    for (unsigned char c : value) {
        switch (c) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (c < 0x20) output << "\\u" << std::hex << std::setw(4)
                                     << std::setfill('0') << static_cast<int>(c) << std::dec;
                else output << static_cast<char>(c);
        }
    }
    return output.str();
}

bool JsonlReport::open(const std::string& path, const std::string& inputPath,
                       uint64_t inputSize, std::string& error) {
    stream_.open(path, std::ios::out | std::ios::trunc);
    if (!stream_) { error = "cannot open report: " + path; return false; }
    inputPath_ = inputPath;
    inputSize_ = inputSize;
    return true;
}

bool JsonlReport::writeResult(const RecoveryResult& result, std::string& error) {
    if (!stream_.is_open()) return true;
    stream_ << "{\"record_type\":\"candidate\",\"report_version\":1"
            << ",\"input\":{\"path\":\"" << escape(inputPath_)
            << "\",\"size\":" << inputSize_ << "}"
            << ",\"offset\":" << result.candidate.startOffset
            << ",\"format\":\"" << escape(result.candidate.formatId) << "\""
            << ",\"source_name\":\"" << escape(result.candidate.sourceName) << "\""
            << ",\"method\":\"" << toString(result.candidate.method) << "\""
            << ",\"accepted\":" << (result.accepted ? "true" : "false")
            << ",\"output_path\":\"" << escape(result.outputPath) << "\""
            << ",\"byte_count\":" << result.byteCount
            << ",\"confidence\":" << result.confidence
            << ",\"validation\":\"" << toString(result.validationState) << "\""
            << ",\"truncation_reason\":\"" << escape(result.truncationReason) << "\""
            << ",\"extents\":[";
    for (size_t i = 0; i < result.candidate.extents.size(); ++i) {
        const auto& extent = result.candidate.extents[i];
        if (i) stream_ << ',';
        stream_ << "{\"offset\":" << extent.offset << ",\"length\":" << extent.length
                << ",\"sparse\":" << (extent.sparse ? "true" : "false") << '}';
    }
    stream_ << "],\"evidence\":[";
    for (size_t i = 0; i < result.evidence.size(); ++i) {
        if (i) stream_ << ',';
        stream_ << "{\"name\":\"" << escape(result.evidence[i].name)
                << "\",\"score\":" << result.evidence[i].score
                << ",\"detail\":\"" << escape(result.evidence[i].detail) << "\"}";
    }
    stream_ << "],\"warnings\":[";
    for (size_t i = 0; i < result.warnings.size(); ++i) {
        if (i) stream_ << ',';
        stream_ << '"' << escape(result.warnings[i]) << '"';
    }
    stream_ << "]}\n";
    stream_.flush();
    if (!stream_) { error = "failed to write report"; return false; }
    return true;
}

bool JsonlReport::writeSummary(uint64_t accepted, uint64_t rejected, uint64_t errors,
                               std::string& error) {
    if (!stream_.is_open()) return true;
    stream_ << "{\"record_type\":\"summary\",\"report_version\":1,\"accepted\":"
            << accepted << ",\"rejected\":" << rejected << ",\"errors\":" << errors << "}\n";
    stream_.flush();
    if (!stream_) { error = "failed to write report summary"; return false; }
    return true;
}
