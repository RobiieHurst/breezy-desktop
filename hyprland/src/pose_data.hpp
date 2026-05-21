#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct PoseData {
    uint8_t version = 0;
    bool enabled = false;
    bool valid = false;
    bool stale = true;
    uint64_t timestampMs = 0;
    float diagonalFov = 0.0F;
    float positionNwu[3] = {0.0F, 0.0F, 0.0F};
    float orientationNwu[16] = {0.0F};
};

std::optional<PoseData> readPoseData(std::string* error = nullptr);
