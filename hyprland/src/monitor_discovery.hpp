#pragma once

#include <string>
#include <vector>

struct MonitorInfo {
    std::string name;
    std::string description;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    float scale = 1.0F;
    float refreshRate = 0.0F;
    bool enabled = false;
    bool breezyVirtual = false;
    bool likelyHeadless = false;
    bool supportedHeadset = false;
};

struct MonitorSummary {
    std::vector<MonitorInfo> monitors;
    int enabledCount = 0;
    int breezyVirtualCount = 0;
    int likelyHeadlessCount = 0;
    int supportedHeadsetCount = 0;
    std::string targetHeadsetName;
    std::string firstBreezyVirtualName;

    std::string fingerprint() const;
    std::string notificationText() const;
};

MonitorSummary discoverMonitors();
