#pragma once

#include "recovery.hpp"
#include "work_budget.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ImageReader {
public:
    ImageReader() = default;
    ~ImageReader();
    ImageReader(const ImageReader&) = delete;
    ImageReader& operator=(const ImageReader&) = delete;

    bool open(const std::string& path, std::string& error);
    bool read(uint64_t offset, void* buffer, size_t size, size_t& bytesRead,
              std::string& error) const;
    bool readExact(uint64_t offset, void* buffer, size_t size, std::string& error) const;
    uint64_t size() const { return size_; }
    void setBudget(WorkBudget* budget) { budget_ = budget; }
    WorkBudget* budget() const { return budget_; }

private:
    void close();

    int fd_ = -1;
    uint64_t size_ = 0;
    WorkBudget* budget_ = nullptr;
};

bool writeRecoveredFile(const ImageReader& image, const std::vector<Extent>& extents,
                        uint64_t byteLimit, const std::string& outputDirectory,
                        const std::string& baseName, std::string& finalPath,
                        uint64_t& bytesWritten, std::string& error,
                        const std::vector<uint8_t>* residentData = nullptr,
                        WorkBudget* budget = nullptr);
