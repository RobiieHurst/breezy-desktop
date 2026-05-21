#include "pose_data.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
constexpr const char* SHM_PATH = "/dev/shm/breezy_desktop_imu";

constexpr int UINT8_SIZE = sizeof(uint8_t);
constexpr int BOOL_SIZE = UINT8_SIZE;
constexpr int UINT_SIZE = sizeof(uint32_t);
constexpr int FLOAT_SIZE = sizeof(float);

constexpr int OFFSET_INDEX = 0;
constexpr int SIZE_INDEX = 1;
constexpr int COUNT_INDEX = 2;

constexpr int dataViewEnd(const int info[3]) {
    return info[OFFSET_INDEX] + info[SIZE_INDEX] * info[COUNT_INDEX];
}

constexpr int VERSION[3] = {0, UINT8_SIZE, 1};
constexpr int ENABLED[3] = {dataViewEnd(VERSION), BOOL_SIZE, 1};
constexpr int LOOK_AHEAD_CFG[3] = {dataViewEnd(ENABLED), FLOAT_SIZE, 4};
constexpr int DISPLAY_RES[3] = {dataViewEnd(LOOK_AHEAD_CFG), UINT_SIZE, 2};
constexpr int DISPLAY_FOV[3] = {dataViewEnd(DISPLAY_RES), FLOAT_SIZE, 1};
constexpr int LENS_DISTANCE_RATIO[3] = {dataViewEnd(DISPLAY_FOV), FLOAT_SIZE, 1};
constexpr int SBS_ENABLED[3] = {dataViewEnd(LENS_DISTANCE_RATIO), BOOL_SIZE, 1};
constexpr int CUSTOM_BANNER_ENABLED[3] = {dataViewEnd(SBS_ENABLED), BOOL_SIZE, 1};
constexpr int SMOOTH_FOLLOW_ENABLED[3] = {dataViewEnd(CUSTOM_BANNER_ENABLED), BOOL_SIZE, 1};
constexpr int SMOOTH_FOLLOW_ORIGIN_DATA[3] = {dataViewEnd(SMOOTH_FOLLOW_ENABLED), FLOAT_SIZE, 16};
constexpr int POSE_POSITION_DATA[3] = {dataViewEnd(SMOOTH_FOLLOW_ORIGIN_DATA), FLOAT_SIZE, 3};
constexpr int POSE_DATE_MS[3] = {dataViewEnd(POSE_POSITION_DATA), UINT_SIZE, 2};
constexpr int POSE_ORIENTATION_ENTRIES = 4;
constexpr int POSE_ORIENTATION_DATA[3] = {dataViewEnd(POSE_DATE_MS), FLOAT_SIZE, 4 * POSE_ORIENTATION_ENTRIES};
constexpr int POSE_PARITY_BYTE[3] = {dataViewEnd(POSE_ORIENTATION_DATA), UINT8_SIZE, 1};
constexpr int LENGTH = dataViewEnd(POSE_PARITY_BYTE);
constexpr uint8_t EXPECTED_VERSION = 5;

bool checkParityByte(const char* data) {
    const auto parityByte = static_cast<uint8_t>(data[POSE_PARITY_BYTE[OFFSET_INDEX]]);
    uint8_t parity = 0;

    const int dateBytes = POSE_DATE_MS[COUNT_INDEX] * POSE_DATE_MS[SIZE_INDEX];
    for (int i = 0; i < dateBytes; ++i) {
        parity ^= static_cast<uint8_t>(data[POSE_DATE_MS[OFFSET_INDEX] + i]);
    }

    const int quatBytes = POSE_ORIENTATION_DATA[COUNT_INDEX] * POSE_ORIENTATION_DATA[SIZE_INDEX];
    for (int i = 0; i < quatBytes; ++i) {
        parity ^= static_cast<uint8_t>(data[POSE_ORIENTATION_DATA[OFFSET_INDEX] + i]);
    }

    return parityByte == parity;
}

uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}

std::optional<PoseData> readPoseData(std::string* error) {
    std::ifstream file(SHM_PATH, std::ios::binary);
    if (!file) {
        if (error) *error = "pose shared memory is not available";
        return std::nullopt;
    }

    std::vector<char> buffer(std::istreambuf_iterator<char>(file), {});
    if (buffer.size() != static_cast<size_t>(LENGTH)) {
        if (error) *error = "pose shared memory has an unexpected size";
        return std::nullopt;
    }

    const char* data = buffer.data();
    if (!checkParityByte(data)) {
        if (error) *error = "pose shared memory parity check failed";
        return std::nullopt;
    }

    PoseData pose;
    pose.version = static_cast<uint8_t>(data[VERSION[OFFSET_INDEX]]);
    pose.enabled = static_cast<uint8_t>(data[ENABLED[OFFSET_INDEX]]) != 0;
    std::memcpy(&pose.timestampMs, data + POSE_DATE_MS[OFFSET_INDEX], sizeof(pose.timestampMs));
    std::memcpy(&pose.diagonalFov, data + DISPLAY_FOV[OFFSET_INDEX], sizeof(pose.diagonalFov));
    std::memcpy(&pose.positionNwu, data + POSE_POSITION_DATA[OFFSET_INDEX], sizeof(pose.positionNwu));
    std::memcpy(&pose.orientationNwu, data + POSE_ORIENTATION_DATA[OFFSET_INDEX], sizeof(pose.orientationNwu));

    pose.stale = nowMs() - pose.timestampMs >= 5000;
    pose.valid = pose.enabled && pose.version == EXPECTED_VERSION && !pose.stale && pose.diagonalFov != 0.0F;

    return pose;
}
