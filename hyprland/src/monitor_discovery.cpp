#include "monitor_discovery.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>

#include <algorithm>
#include <array>
#include <sstream>

namespace {
bool containsInsensitive(std::string haystack, std::string needle) {
    std::ranges::transform(haystack, haystack.begin(), [](unsigned char c) { return std::tolower(c); });
    std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return std::tolower(c); });
    return haystack.find(needle) != std::string::npos;
}

bool equalsInsensitive(std::string left, std::string right) {
    std::ranges::transform(left, left.begin(), [](unsigned char c) { return std::tolower(c); });
    std::ranges::transform(right, right.begin(), [](unsigned char c) { return std::tolower(c); });
    return left == right;
}

constexpr std::array<const char*, 20> SUPPORTED_MONITOR_PRODUCTS = {
    "VITURE",
    "nreal air",
    "Air",
    "Air 2",
    "Air 2 Pro",
    "Air 2 Ultra",
    "One",
    "One Pro",
    "XREAL One",
    "XREAL One Pro",
    "XREAL 1S",
    "SmartGlasses",
    "RayNeo Air 4 Pro",
    "RayNeo Air 3s",
    "RayNeo Air 3",
    "RayNeo Air 2",
    "Rokid Max",
    "Rokid Max 2",
    "Rokid Air",
    "MetaMonitor",
};

bool isBreezyVirtual(const CMonitor& monitor) {
    return monitor.m_name.rfind("breezy-virtual", 0) == 0;
}

bool isLikelyHeadless(const CMonitor& monitor) {
    return isBreezyVirtual(monitor) || containsInsensitive(monitor.m_name, "headless") || containsInsensitive(monitor.m_description, "headless");
}

bool matchesSupportedProduct(const std::string& value) {
    if (value.empty())
        return false;

    return std::ranges::any_of(SUPPORTED_MONITOR_PRODUCTS, [&](const char* product) {
        return equalsInsensitive(value, product);
    });
}

bool descriptionContainsExplicitSupportedProduct(const std::string& description) {
    if (description.empty())
        return false;

    // Avoid broad matches like "Air" in arbitrary monitor descriptions.
    constexpr std::array<const char*, 12> explicitProducts = {
        "XREAL One",
        "XREAL One Pro",
        "XREAL 1S",
        "SmartGlasses",
        "RayNeo Air 4 Pro",
        "RayNeo Air 3s",
        "RayNeo Air 3",
        "RayNeo Air 2",
        "Rokid Max",
        "Rokid Max 2",
        "Rokid Air",
        "VITURE",
    };

    return std::ranges::any_of(explicitProducts, [&](const char* product) {
        return containsInsensitive(description, product);
    });
}

bool isSupportedHeadset(const CMonitor& monitor) {
    if (isLikelyHeadless(monitor))
        return false;

    if (matchesSupportedProduct(monitor.m_output ? monitor.m_output->model : ""))
        return true;
    if (matchesSupportedProduct(monitor.m_output ? monitor.m_output->parsedEDID.model : ""))
        return true;
    if (matchesSupportedProduct(monitor.m_output ? monitor.m_output->make : ""))
        return true;

    return descriptionContainsExplicitSupportedProduct(monitor.m_description) ||
           descriptionContainsExplicitSupportedProduct(monitor.m_output ? monitor.m_output->description : "");
}
}

std::string MonitorSummary::fingerprint() const {
    std::ostringstream out;
    for (const auto& monitor : monitors) {
        out << monitor.name << ':'
            << monitor.description << ':'
            << monitor.x << ',' << monitor.y << ':'
            << monitor.width << 'x' << monitor.height << ':'
            << monitor.enabled << ':'
            << monitor.breezyVirtual << ':'
            << monitor.likelyHeadless << ':'
            << monitor.supportedHeadset << ';';
    }
    return out.str();
}

std::string MonitorSummary::notificationText() const {
    std::ostringstream out;
    out << "[Breezy Desktop] Monitors: " << enabledCount << " enabled";
    if (breezyVirtualCount > 0)
        out << ", " << breezyVirtualCount << " Breezy virtual";
    else if (likelyHeadlessCount > 0)
        out << ", " << likelyHeadlessCount << " headless";

    if (supportedHeadsetCount > 0)
        out << ", headset " << targetHeadsetName;

    if (!monitors.empty()) {
        out << " (";
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (i != 0)
                out << ", ";
            out << monitors[i].name;
        }
        out << ")";
    }

    return out.str();
}

MonitorSummary discoverMonitors() {
    MonitorSummary summary;
    if (!g_pCompositor)
        return summary;

    for (const auto& monitor : g_pCompositor->m_monitors) {
        if (!monitor)
            continue;

        MonitorInfo info;
        info.name = monitor->m_name;
        info.description = monitor->m_description;
        info.x = static_cast<int>(monitor->m_position.x);
        info.y = static_cast<int>(monitor->m_position.y);
        info.width = static_cast<int>(monitor->m_size.x);
        info.height = static_cast<int>(monitor->m_size.y);
        info.scale = monitor->m_scale;
        info.refreshRate = monitor->m_refreshRate;
        info.enabled = monitor->m_enabled;
        info.breezyVirtual = isBreezyVirtual(*monitor);
        info.likelyHeadless = isLikelyHeadless(*monitor);
        info.supportedHeadset = isSupportedHeadset(*monitor);

        if (info.enabled)
            summary.enabledCount++;
        if (info.breezyVirtual)
            summary.breezyVirtualCount++;
        if (info.breezyVirtual && summary.firstBreezyVirtualName.empty())
            summary.firstBreezyVirtualName = info.name;
        if (info.likelyHeadless)
            summary.likelyHeadlessCount++;
        if (info.supportedHeadset) {
            summary.supportedHeadsetCount++;
            if (summary.targetHeadsetName.empty())
                summary.targetHeadsetName = info.name;
        }

        summary.monitors.push_back(info);
    }

    std::ranges::sort(summary.monitors, {}, &MonitorInfo::name);
    return summary;
}
