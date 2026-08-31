#include "carver.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void usage(const char* program) {
    std::cerr << "Usage: " << program << " [options] <disk_image_path>\n"
              << "Options:\n"
              << "  --output <directory>       Recovery destination (default: current directory)\n"
              << "  --report <path>            Write a JSON Lines recovery report\n"
              << "  --mode raw|ntfs|hybrid     Recovery source (default: hybrid)\n"
              << "  --min-confidence <0-100>   Filter low-confidence candidates\n";
}

bool parseArguments(int argc, char* argv[], CarverOptions& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&](const char* option) -> const char* {
            if (++i >= argc) { error = std::string("missing value for ") + option; return nullptr; }
            return argv[i];
        };
        if (argument == "--output") {
            const char* v = value("--output"); if (!v) return false; options.outputDirectory = v;
        } else if (argument == "--report") {
            const char* v = value("--report"); if (!v) return false; options.reportPath = v;
        } else if (argument == "--mode") {
            const char* v = value("--mode"); if (!v) return false;
            const std::string mode = v;
            if (mode == "raw") options.mode = RecoveryMode::Raw;
            else if (mode == "ntfs") options.mode = RecoveryMode::Ntfs;
            else if (mode == "hybrid") options.mode = RecoveryMode::Hybrid;
            else { error = "invalid mode: " + mode; return false; }
        } else if (argument == "--min-confidence") {
            const char* v = value("--min-confidence"); if (!v) return false;
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(v, &end, 10);
            if (errno != 0 || end == v || *end != '\0' || parsed < 0 || parsed > 100) {
                error = "minimum confidence must be an integer from 0 to 100"; return false;
            }
            options.minimumConfidence = static_cast<int>(parsed);
        } else if (argument == "--help" || argument == "-h") {
            usage(argv[0]); std::exit(0);
        } else if (!argument.empty() && argument[0] == '-') {
            error = "unknown option: " + argument; return false;
        } else if (options.inputPath.empty()) options.inputPath = argument;
        else { error = "more than one input path was provided"; return false; }
    }
    if (options.inputPath.empty()) { error = "an input path is required"; return false; }
    return true;
}
}

int main(int argc, char* argv[]) {
    CarverOptions options;
    std::string error;
    if (!parseArguments(argc, argv, options, error)) {
        std::cerr << "Error: " << error << '\n';
        usage(argv[0]);
        return 2;
    }
    std::cout << "[*] Initializing FILEEdo for: " << options.inputPath << '\n';
    FileCarver carver(std::move(options));
    if (!carver.initialize()) {
        std::cerr << "Error: " << carver.lastError() << '\n';
        return 1;
    }
    if (!carver.startCarving()) {
        std::cerr << "Error: " << carver.lastError() << '\n';
        return 1;
    }
    const auto& summary = carver.summary();
    std::cout << "[*] Complete: " << summary.accepted << " accepted, "
              << summary.rejected << " rejected, " << summary.errors << " errors\n";
    return 0;
}
