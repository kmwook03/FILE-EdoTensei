#pragma once

#include "recovery.hpp"

#include <fstream>
#include <string>

class JsonlReport {
public:
    bool open(const std::string& path, const std::string& inputPath,
              uint64_t inputSize, std::string& error);
    bool writeResult(const RecoveryResult& result, std::string& error);
    bool writeSummary(const CarvingSummary& summary, std::string& error);
    static std::string escape(const std::string& value);

private:
    std::ofstream stream_;
    std::string inputPath_;
    uint64_t inputSize_ = 0;
};
