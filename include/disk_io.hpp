#pragma once

#include "io.hpp"
#include "recovery.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct MFT_Segment {
    uint64_t lcn = 0;
    uint64_t length = 0;
    bool sparse = false;
};

class NTFSReader {
public:
    bool openImage(const std::string& path, std::string& error);
    bool detectVolume(std::string& error);
    std::vector<RecoveryCandidate> findDeletedFiles(std::string& error);

    static bool parseDataRuns(const uint8_t* data, size_t size,
                              std::vector<MFT_Segment>& segments, std::string& error);
    static bool applyFixups(std::vector<uint8_t>& record, uint16_t bytesPerSector,
                            std::string& error);

private:
    bool parseBootSector(uint64_t offset, std::string& error);
    bool parseRecord(uint64_t diskOffset, std::vector<uint8_t>& record,
                     RecoveryCandidate& candidate, std::string& warning);

    ImageReader image_;
    uint64_t partitionOffset_ = 0;
    uint64_t mftOffset_ = 0;
    uint32_t bytesPerCluster_ = 0;
    uint32_t recordSize_ = 0;
    uint16_t bytesPerSector_ = 0;
    std::vector<Extent> mftExtents_;
    bool detected_ = false;
};
