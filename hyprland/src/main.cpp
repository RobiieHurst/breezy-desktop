#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include "monitor_discovery.hpp"
#include "pose_data.hpp"

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

#ifndef BREEZY_DESKTOP_VERSION_STR
#define BREEZY_DESKTOP_VERSION_STR "dev"
#endif

inline HANDLE PHANDLE = nullptr;

namespace {
using namespace std::chrono_literals;

SP<CEventLoopTimer> g_posePollTimer;
bool g_hadPoseSample = false;
bool g_lastPoseValid = false;
std::string g_lastMonitorFingerprint;

struct SessionState {
    bool poseActive = false;
    bool headsetPresent = false;
    bool ready = false;
    int breezyVirtualCount = 0;
    std::string headsetName;
    std::string virtualMonitorName;

    std::string fingerprint() const {
        return std::to_string(poseActive) + ':' +
               std::to_string(headsetPresent) + ':' +
               std::to_string(ready) + ':' +
               std::to_string(breezyVirtualCount) + ':' +
               headsetName + ':' +
               virtualMonitorName;
    }
};

struct PoseOffset {
    double x = 0.0;
    double y = 0.0;
};

struct Quat {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

SessionState g_session;
std::string g_lastSessionFingerprint;
CHyprSignalListener g_renderPreListener;
CHyprSignalListener g_renderStageListener;
PHLMONITORREF g_currentRenderMonitor;
bool g_renderHookSawTarget = false;
CFunctionHook* g_renderMonitorHook = nullptr;
SP<SHyprCtlCommand> g_recenterHyprctlCommand;
uint64_t g_targetRenderHits = 0;
uint64_t g_virtualRenderHits = 0;
uint64_t g_virtualCacheHits = 0;
uint64_t g_virtualTextureRenderHits = 0;
SP<CTexture> g_virtualMonitorTexture;
Vector2D g_virtualMonitorTextureSize;
std::string g_fakeMirrorSourceName;
std::string g_fakeMirrorTargetName;
bool g_poseBaselineSet = false;
Quat g_poseBaseline;
bool g_poseOffsetInitialized = false;
PoseOffset g_smoothedPoseOffset;

constexpr double BREEZY_PI = 3.14159265358979323846;
constexpr double DEFAULT_POSE_SMOOTHING_ALPHA = 0.45;
constexpr double DEFAULT_POSE_MAX_STEP_RATIO = 0.35;
constexpr double DEFAULT_POSE_NORMALIZED_DEADZONE = 0.01;
constexpr double DEFAULT_POSE_NORMALIZED_LIMIT = 6.0;
constexpr double DEFAULT_XR_SCREEN_SCALE = 0.95;
constexpr double XR_CENTERED_THRESHOLD = 0.35;
constexpr const char* RECENTER_DISPATCHER = "breezy_recenter";
constexpr const char* RECENTER_HYPRCTL_COMMAND = "breezy-recenter";
constexpr const char* CONFIG_SCREEN_SCALE = "plugin:breezy:screen_scale";
constexpr const char* CONFIG_SMOOTHING_ALPHA = "plugin:breezy:smoothing_alpha";
constexpr const char* CONFIG_MAX_STEP_RATIO = "plugin:breezy:max_step_ratio";
constexpr const char* CONFIG_DEADZONE = "plugin:breezy:deadzone";
constexpr const char* CONFIG_MOVEMENT_LIMIT = "plugin:breezy:movement_limit";

using RenderMonitorFn = void (*)(CHyprRenderer*, PHLMONITOR, bool);

void notify(const std::string& message, const CHyprColor& color, uint64_t timeMs = 4000) {
    HyprlandAPI::addNotification(PHANDLE, message, color, timeMs);
}

void logDebug(const std::string& message) {
#ifdef BREEZY_HYPRLAND_DEBUG
    std::ofstream log("/tmp/breezy-hyprland.log", std::ios::app);
    log << message << '\n';
#else
    (void)message;
#endif
}

void resetPoseFilter() {
    g_poseBaselineSet = false;
    g_poseBaseline = {};
    g_poseOffsetInitialized = false;
    g_smoothedPoseOffset = {};
}

Quat quatConjugate(const Quat& q) {
    return {.x = -q.x, .y = -q.y, .z = -q.z, .w = q.w};
}

Quat quatMultiply(const Quat& a, const Quat& b) {
    return {
        .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Vec3 rotateVector(const Vec3& v, const Quat& q) {
    const Quat rotated = quatMultiply(quatMultiply(q, {.x = v.x, .y = v.y, .z = v.z, .w = 0.0}), quatConjugate(q));
    return {.x = rotated.x, .y = rotated.y, .z = rotated.z};
}

double configFloat(const std::string& name, const double fallback) {
    const auto* value = HyprlandAPI::getConfigValue(PHANDLE, name);
    if (!value)
        return fallback;

    try {
        return static_cast<double>(std::any_cast<Hyprlang::FLOAT>(value->getValue()));
    } catch (const std::bad_any_cast&) {
        return fallback;
    }
}

void addConfigValues() {
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_SCREEN_SCALE, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_XR_SCREEN_SCALE)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_SMOOTHING_ALPHA, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_SMOOTHING_ALPHA)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_MAX_STEP_RATIO, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_MAX_STEP_RATIO)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_DEADZONE, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_NORMALIZED_DEADZONE)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_MOVEMENT_LIMIT, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_NORMALIZED_LIMIT)));
}

PoseOffset clampPoseStep(const PoseOffset& previous, const PoseOffset& next, const CBox& box) {
    const double maxStep = std::max(box.w, box.h) * configFloat(CONFIG_MAX_STEP_RATIO, DEFAULT_POSE_MAX_STEP_RATIO);
    return {
        .x = previous.x + std::clamp(next.x - previous.x, -maxStep, maxStep),
        .y = previous.y + std::clamp(next.y - previous.y, -maxStep, maxStep),
    };
}

PoseOffset smoothPoseOffset(const PoseOffset& target, const CBox& box) {
    if (!g_poseOffsetInitialized) {
        g_poseOffsetInitialized = true;
        g_smoothedPoseOffset = target;
        return g_smoothedPoseOffset;
    }

    const PoseOffset filtered{
        .x = g_smoothedPoseOffset.x + (target.x - g_smoothedPoseOffset.x) * configFloat(CONFIG_SMOOTHING_ALPHA, DEFAULT_POSE_SMOOTHING_ALPHA),
        .y = g_smoothedPoseOffset.y + (target.y - g_smoothedPoseOffset.y) * configFloat(CONFIG_SMOOTHING_ALPHA, DEFAULT_POSE_SMOOTHING_ALPHA),
    };
    g_smoothedPoseOffset = clampPoseStep(g_smoothedPoseOffset, filtered, box);
    return g_smoothedPoseOffset;
}

double applyDeadzone(double value) {
    const double deadzone = configFloat(CONFIG_DEADZONE, DEFAULT_POSE_NORMALIZED_DEADZONE);
    if (std::abs(value) < deadzone)
        return 0.0;

    return value > 0.0 ? value - deadzone : value + deadzone;
}

bool monitorRefMatchesName(const PHLMONITORREF& ref, const std::string& name) {
    const auto monitor = ref.lock();
    return monitor && monitor->m_name == name;
}

void removeFakeMirrorCapture() {
    if (!g_pCompositor || g_fakeMirrorSourceName.empty() || g_fakeMirrorTargetName.empty())
        return;

    const auto source = g_pCompositor->getMonitorFromName(g_fakeMirrorSourceName);
    if (!source) {
        g_fakeMirrorSourceName.clear();
        g_fakeMirrorTargetName.clear();
        return;
    }

    auto& mirrors = source->m_mirrors;
    for (auto it = mirrors.begin(); it != mirrors.end();) {
        if (monitorRefMatchesName(*it, g_fakeMirrorTargetName))
            it = mirrors.erase(it);
        else
            ++it;
    }

    logDebug("removed fake mirror capture source=" + g_fakeMirrorSourceName + " target=" + g_fakeMirrorTargetName);
    g_fakeMirrorSourceName.clear();
    g_fakeMirrorTargetName.clear();
}

void ensureFakeMirrorCapture() {
    if (!g_pCompositor || !g_session.ready || g_session.virtualMonitorName.empty() || g_session.headsetName.empty()) {
        removeFakeMirrorCapture();
        return;
    }

    if (g_fakeMirrorSourceName != g_session.virtualMonitorName || g_fakeMirrorTargetName != g_session.headsetName)
        removeFakeMirrorCapture();

    const auto source = g_pCompositor->getMonitorFromName(g_session.virtualMonitorName);
    const auto target = g_pCompositor->getMonitorFromName(g_session.headsetName);
    if (!source || !target)
        return;

    for (const auto& mirror : source->m_mirrors) {
        if (monitorRefMatchesName(mirror, target->m_name)) {
            g_fakeMirrorSourceName = source->m_name;
            g_fakeMirrorTargetName = target->m_name;
            return;
        }
    }

    source->m_mirrors.emplace_back(target->m_self);
    g_fakeMirrorSourceName = source->m_name;
    g_fakeMirrorTargetName = target->m_name;
    logDebug("added fake mirror capture source=" + source->m_name + " target=" + target->m_name);
}

PoseOffset currentPoseOffset(PHLMONITOR target, PHLMONITOR source, const CBox& box) {
    if (!target || !source)
        return {};

    const auto pose = readPoseData();
    if (pose && !pose->enabled) {
        resetPoseFilter();
        return {};
    }

    if (!pose || !pose->valid)
        return g_poseOffsetInitialized ? g_smoothedPoseOffset : PoseOffset{};

    const auto* q = pose->orientationNwu;
    const Quat current{
        .x = -q[1],
        .y = q[2],
        .z = -q[0],
        .w = q[3],
    };

    if (!g_poseBaselineSet) {
        g_poseBaselineSet = true;
        g_poseBaseline = current;
    }

    const Quat relative = quatMultiply(quatConjugate(current), g_poseBaseline);
    const Vec3 anchorDirection = rotateVector({.x = 0.0, .y = 0.0, .z = -1.0}, relative);

    const double diagonalFovRadians = std::clamp(static_cast<double>(pose->diagonalFov), 1.0, 179.0) * BREEZY_PI / 180.0;
    const double aspect = source->m_transformedSize.x / source->m_transformedSize.y;
    const double diagonalHalfTangent = std::tan(diagonalFovRadians / 2.0);
    const double verticalHalfTangent = diagonalHalfTangent / std::sqrt((aspect * aspect) + 1.0);
    const double horizontalHalfTangent = verticalHalfTangent * aspect;
    const double depth = std::max(-anchorDirection.z, 0.001);

    const double movementLimit = configFloat(CONFIG_MOVEMENT_LIMIT, DEFAULT_POSE_NORMALIZED_LIMIT);
    const double normalizedX = applyDeadzone(std::clamp((anchorDirection.x / depth) / horizontalHalfTangent, -movementLimit, movementLimit));
    const double normalizedY = applyDeadzone(std::clamp((anchorDirection.y / depth) / verticalHalfTangent, -movementLimit, movementLimit));

    (void)target;
    const PoseOffset targetOffset{
        .x = normalizedX * target->m_transformedSize.x * 0.5,
        .y = normalizedY * target->m_transformedSize.y * 0.5,
    };
    return smoothPoseOffset(targetOffset, box);
}

bool readPoseValid(bool notifyInactive) {
    std::string error;
    const auto pose = readPoseData(&error);
    if (!pose) {
        if (notifyInactive)
            notify("[Breezy Desktop] XR pose data unavailable: " + error, CHyprColor{1.0, 0.7, 0.2, 1.0}, 5000);
        return false;
    }

    if (pose->valid)
        return true;

    if (notifyInactive) {
        notify(
            "[Breezy Desktop] XR pose data found but inactive"
            " version=" + std::to_string(pose->version) +
            " enabled=" + std::to_string(pose->enabled) +
            " stale=" + std::to_string(pose->stale),
            CHyprColor{1.0, 0.7, 0.2, 1.0},
            6000);
    }

    return false;
}

void updateSessionState(const SessionState& next, bool notifyInitial = false) {
    const auto fingerprint = next.fingerprint();
    if (!notifyInitial && fingerprint == g_lastSessionFingerprint)
        return;

    const bool readinessChanged = notifyInitial || next.ready != g_session.ready;
    g_session = next;
    g_lastSessionFingerprint = fingerprint;

    if (!readinessChanged)
        return;

    g_renderHookSawTarget = false;
    resetPoseFilter();

    if (g_session.ready) {
        notify(
            "[Breezy Desktop] XR session ready on " + g_session.headsetName +
            " with " + std::to_string(g_session.breezyVirtualCount) + " virtual display(s)",
            CHyprColor{0.2, 1.0, 0.4, 1.0},
            5000);
        return;
    }

    std::string reason;
    if (!g_session.poseActive && !g_session.headsetPresent)
        reason = "waiting for pose data and headset monitor";
    else if (!g_session.poseActive)
        reason = "waiting for pose data";
    else if (g_session.breezyVirtualCount == 0)
        reason = "waiting for Breezy virtual display";
    else
        reason = "waiting for headset monitor";

    notify("[Breezy Desktop] XR session not ready: " + reason, CHyprColor{1.0, 0.7, 0.2, 1.0}, notifyInitial ? 6000 : 5000);
}

bool pollPoseData(bool notifyInitial = false) {
    const bool poseValid = readPoseValid(notifyInitial);

    if (!g_hadPoseSample) {
        g_hadPoseSample = true;
        g_lastPoseValid = poseValid;
        if (poseValid)
            notify("[Breezy Desktop] XR pose data active", CHyprColor{0.2, 0.8, 1.0, 1.0}, 4000);
        return poseValid;
    }

    if (poseValid == g_lastPoseValid)
        return poseValid;

    g_lastPoseValid = poseValid;
    if (poseValid) {
        notify("[Breezy Desktop] XR pose data became active", CHyprColor{0.2, 0.8, 1.0, 1.0}, 4000);
    } else {
        notify("[Breezy Desktop] XR pose data became inactive", CHyprColor{1.0, 0.7, 0.2, 1.0}, 4000);
    }

    return poseValid;
}

MonitorSummary pollMonitors(bool notifyInitial = false) {
    const auto summary = discoverMonitors();
    const auto fingerprint = summary.fingerprint();
    if (!notifyInitial && fingerprint == g_lastMonitorFingerprint)
        return summary;

    g_lastMonitorFingerprint = fingerprint;
    notify(summary.notificationText(), CHyprColor{0.2, 0.8, 1.0, 1.0}, notifyInitial ? 5000 : 4000);
    return summary;
}

void pollRuntimeState(bool notifyInitial = false) {
    const bool poseActive = pollPoseData(notifyInitial);
    const auto monitors = pollMonitors(notifyInitial);

    SessionState next;
    next.poseActive = poseActive;
    next.headsetPresent = monitors.supportedHeadsetCount > 0;
    next.breezyVirtualCount = monitors.breezyVirtualCount;
    next.headsetName = monitors.targetHeadsetName;
    next.virtualMonitorName = monitors.firstBreezyVirtualName;
    next.ready = next.poseActive && next.headsetPresent && next.breezyVirtualCount > 0;
    updateSessionState(next, notifyInitial);

    if (g_session.virtualMonitorName.empty()) {
        g_virtualMonitorTexture.reset();
        g_virtualMonitorTextureSize = {};
        removeFakeMirrorCapture();
    }

    ensureFakeMirrorCapture();

    if (g_session.headsetPresent && g_pCompositor && g_pHyprRenderer) {
        if (const auto monitor = g_pCompositor->getMonitorFromName(g_session.headsetName); monitor)
            g_pHyprRenderer->damageMonitor(monitor);
        if (!g_session.virtualMonitorName.empty()) {
            if (const auto monitor = g_pCompositor->getMonitorFromName(g_session.virtualMonitorName); monitor) {
                g_pHyprRenderer->damageMonitor(monitor);
                g_pCompositor->scheduleFrameForMonitor(monitor);
            }
        }
    }
}

void startPosePolling() {
    if (!g_pEventLoopManager)
        return;

    g_posePollTimer = Hyprutils::Memory::makeShared<CEventLoopTimer>(
        1s,
        [](SP<CEventLoopTimer> self, void*) {
            pollRuntimeState();
            self->updateTimeout(1s);
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_posePollTimer);
}

void stopPosePolling() {
    if (!g_posePollTimer)
        return;

    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(g_posePollTimer);
    g_posePollTimer->cancel();
    g_posePollTimer.reset();
}

bool isTargetRenderMonitor(PHLMONITOR monitor) {
    return monitor && g_session.headsetPresent && monitor->m_name == g_session.headsetName;
}

bool isVirtualRenderMonitor(PHLMONITOR monitor) {
    return monitor && !g_session.virtualMonitorName.empty() && monitor->m_name == g_session.virtualMonitorName;
}

void noteTargetRenderMonitor(PHLMONITOR monitor) {
    if (!isTargetRenderMonitor(monitor) || g_renderHookSawTarget)
        return;

    g_renderHookSawTarget = true;
    logDebug("render hook reached target monitor " + monitor->m_name);
    notify(
        "[Breezy Desktop] XR render hook active for " + monitor->m_name,
        CHyprColor{0.2, 1.0, 0.4, 1.0},
        4000);
}

void scheduleActiveXrFrames(PHLMONITOR target = nullptr, PHLMONITOR source = nullptr) {
    if (!g_session.ready || !g_pCompositor || !g_pHyprRenderer)
        return;

    if (!target && !g_session.headsetName.empty())
        target = g_pCompositor->getMonitorFromName(g_session.headsetName);
    if (!source && !g_session.virtualMonitorName.empty())
        source = g_pCompositor->getMonitorFromName(g_session.virtualMonitorName);

    if (target) {
        g_pHyprRenderer->damageMonitor(target);
        g_pCompositor->scheduleFrameForMonitor(target);
    }

    if (source) {
        g_pHyprRenderer->damageMonitor(source);
        g_pCompositor->scheduleFrameForMonitor(source);
    }
}

bool xrDesktopCenteredInView(PHLMONITOR monitor, const CBox& box) {
    if (!monitor)
        return false;

    const double boxCenterX = box.x + box.w / 2.0;
    const double boxCenterY = box.y + box.h / 2.0;
    const double viewCenterX = monitor->m_transformedSize.x / 2.0;
    const double viewCenterY = monitor->m_transformedSize.y / 2.0;
    const double thresholdX = monitor->m_transformedSize.x * XR_CENTERED_THRESHOLD;
    const double thresholdY = monitor->m_transformedSize.y * XR_CENTERED_THRESHOLD;

    return std::abs(boxCenterX - viewCenterX) <= thresholdX && std::abs(boxCenterY - viewCenterY) <= thresholdY;
}

void recenterPose(const bool showNotification = true) {
    resetPoseFilter();
    scheduleActiveXrFrames();
    if (showNotification)
        notify("[Breezy Desktop] XR view recentered", CHyprColor{0.2, 0.8, 1.0, 1.0}, 2500);
}

void cacheVirtualMonitorTexture() {
    auto monitor = g_currentRenderMonitor.lock();
    if (!isVirtualRenderMonitor(monitor) || !g_pHyprOpenGL)
        return;

    auto textureFromFramebuffer = [](CFramebuffer* fb) -> SP<CTexture> {
        if (!fb || !fb->isAllocated())
            return nullptr;

        auto texture = fb->getTexture();
        if (!texture || texture->m_texID == 0)
            return nullptr;

        return texture;
    };

    auto& monitorRenderData = g_pHyprOpenGL->m_monitorRenderResources[monitor];
    const auto mirrorTexture = textureFromFramebuffer(&monitorRenderData.monitorMirrorFB);
    const auto currentTexture = textureFromFramebuffer(g_pHyprOpenGL->m_renderData.currentFB);
    const auto mainTexture = textureFromFramebuffer(g_pHyprOpenGL->m_renderData.mainFB);
    const auto outTexture = textureFromFramebuffer(g_pHyprOpenGL->m_renderData.outFB);
    auto texture = mirrorTexture ? mirrorTexture : (currentTexture ? currentTexture : (mainTexture ? mainTexture : outTexture));
    if (!texture) {
        if (g_virtualCacheHits % 60 == 0) {
            auto sizeOf = [](CFramebuffer* fb) {
                if (!fb)
                    return std::string("null");
                return std::to_string(static_cast<int>(fb->m_size.x)) + "x" + std::to_string(static_cast<int>(fb->m_size.y));
            };
            logDebug(
                "virtual texture unavailable current=" + sizeOf(g_pHyprOpenGL->m_renderData.currentFB) +
                " main=" + sizeOf(g_pHyprOpenGL->m_renderData.mainFB) +
                " out=" + sizeOf(g_pHyprOpenGL->m_renderData.outFB));
        }
        return;
    }

    g_virtualMonitorTexture = texture;
    if (texture->m_size.x > 0 && texture->m_size.y > 0)
        g_virtualMonitorTextureSize = texture->m_size;
    else if (mirrorTexture)
        g_virtualMonitorTextureSize = monitorRenderData.monitorMirrorFB.m_size;
    else if (currentTexture)
        g_virtualMonitorTextureSize = g_pHyprOpenGL->m_renderData.currentFB->m_size;
    else if (mainTexture)
        g_virtualMonitorTextureSize = g_pHyprOpenGL->m_renderData.mainFB->m_size;
    else
        g_virtualMonitorTextureSize = g_pHyprOpenGL->m_renderData.outFB->m_size;
    g_virtualCacheHits++;
    if (g_virtualCacheHits % 60 == 1)
        logDebug(
            "virtual texture cached count=" + std::to_string(g_virtualCacheHits) +
            " tex=" + std::to_string(g_virtualMonitorTexture->m_texID) +
            " size=" + std::to_string(static_cast<int>(g_virtualMonitorTextureSize.x)) +
            "x" + std::to_string(static_cast<int>(g_virtualMonitorTextureSize.y)));
}

void renderVirtualMonitorTexture() {
    auto monitor = g_currentRenderMonitor.lock();
    if (!g_session.ready || !isTargetRenderMonitor(monitor) || !g_pHyprRenderer || !g_virtualMonitorTexture || g_session.virtualMonitorName.empty())
        return;

    const auto mirrored = g_pCompositor ? g_pCompositor->getMonitorFromName(g_session.virtualMonitorName) : nullptr;
    if (!mirrored || !g_pHyprOpenGL)
        return;

    auto& monitorRenderData = g_pHyprOpenGL->m_monitorRenderResources[mirrored];
    auto* mirrorFB = &monitorRenderData.monitorMirrorFB;
    if (!mirrorFB->isAllocated() || !mirrorFB->getTexture())
        return;

    g_virtualTextureRenderHits++;
    if (g_virtualTextureRenderHits % 60 == 1)
        logDebug("virtual texture rendered on headset count=" + std::to_string(g_virtualTextureRenderHits));

    const double scale = std::min(
        monitor->m_transformedSize.x / mirrored->m_transformedSize.x,
        monitor->m_transformedSize.y / mirrored->m_transformedSize.y) * configFloat(CONFIG_SCREEN_SCALE, DEFAULT_XR_SCREEN_SCALE);
    CBox box = {0, 0, mirrored->m_transformedSize.x * scale, mirrored->m_transformedSize.y * scale};
    box.transform(Math::wlTransformToHyprutils(mirrored->m_transform), mirrored->m_transformedSize.x * scale, mirrored->m_transformedSize.y * scale);
    box.x = (monitor->m_transformedSize.x - box.w) / 2.0;
    box.y = (monitor->m_transformedSize.y - box.h) / 2.0;
    const auto offset = currentPoseOffset(monitor, mirrored, box);
    box.x += offset.x;
    box.y -= offset.y;

    const double backgroundAlpha = xrDesktopCenteredInView(monitor, box) ? 1.0 : 0.0;
    g_pHyprRenderer->m_renderPass.add(Hyprutils::Memory::makeUnique<CClearPassElement>(CClearPassElement::SClearData{CHyprColor(0, 0, 0, backgroundAlpha)}));

    CTexPassElement::SRenderData data;
    data.tex = mirrorFB->getTexture();
    data.box = box;
    data.a = 1.0F;
    data.damage = CRegion(box);
    data.replaceProjection = Mat3x3::identity()
                                 .translate(monitor->m_pixelSize / 2.0)
                                 .transform(Math::wlTransformToHyprutils(monitor->m_transform))
                                 .transform(Math::wlTransformToHyprutils(Math::invertTransform(mirrored->m_transform)))
                                 .translate(-monitor->m_transformedSize / 2.0);

    g_pHyprRenderer->m_renderPass.add(Hyprutils::Memory::makeUnique<CTexPassElement>(std::move(data)));
    scheduleActiveXrFrames(monitor, mirrored);
}

void handleRenderStage(eRenderStage stage) {
    if (stage == RENDER_LAST_MOMENT) {
        cacheVirtualMonitorTexture();
        renderVirtualMonitorTexture();
    }

    if (stage == RENDER_POST)
        g_currentRenderMonitor.reset();
}

void hkRenderMonitor(CHyprRenderer* renderer, PHLMONITOR monitor, bool commit) {
    g_currentRenderMonitor = monitor;
    noteTargetRenderMonitor(monitor);
    if (isTargetRenderMonitor(monitor)) {
        g_targetRenderHits++;
        if (g_targetRenderHits % 120 == 1)
            logDebug("target render hit count=" + std::to_string(g_targetRenderHits));
    }
    if (isVirtualRenderMonitor(monitor)) {
        g_virtualRenderHits++;
        if (g_virtualRenderHits % 60 == 1)
            logDebug("virtual render hit count=" + std::to_string(g_virtualRenderHits) + " monitor=" + monitor->m_name);
    }

    const auto original = reinterpret_cast<RenderMonitorFn>(g_renderMonitorHook->m_original);
    original(renderer, monitor, commit);

    g_currentRenderMonitor.reset();
}

void startRenderHooks() {
    if (!Event::bus() || !g_pHyprRenderer)
        return;

    g_renderStageListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        handleRenderStage(stage);
    });

    g_renderMonitorHook = HyprlandAPI::createFunctionHook(
        PHANDLE,
        reinterpret_cast<void*>(&CHyprRenderer::renderMonitor),
        reinterpret_cast<void*>(&hkRenderMonitor));
    if (g_renderMonitorHook)
        g_renderMonitorHook->hook();
}

void startControlHooks() {
    HyprlandAPI::addDispatcherV2(PHANDLE, RECENTER_DISPATCHER, [](std::string) -> SDispatchResult {
        recenterPose();
        return {};
    });

    SHyprCtlCommand command;
    command.name = RECENTER_HYPRCTL_COMMAND;
    command.exact = true;
    command.fn = [](eHyprCtlOutputFormat, std::string) -> std::string {
        recenterPose(false);
        return "ok\n";
    };
    g_recenterHyprctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, command);
}

void stopControlHooks() {
    HyprlandAPI::removeDispatcher(PHANDLE, RECENTER_DISPATCHER);
    if (g_recenterHyprctlCommand) {
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_recenterHyprctlCommand);
        g_recenterHyprctlCommand.reset();
    }
}

void stopRenderHooks() {
    removeFakeMirrorCapture();

    if (g_renderMonitorHook) {
        g_renderMonitorHook->unhook();
        HyprlandAPI::removeFunctionHook(PHANDLE, g_renderMonitorHook);
        g_renderMonitorHook = nullptr;
    }

    g_renderPreListener.reset();
    g_renderStageListener.reset();
    g_currentRenderMonitor.reset();
    g_renderHookSawTarget = false;
}
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();

    if (compositorHash != clientHash) {
        notify(
            "[Breezy Desktop] Mismatched Hyprland headers. Plugin not loaded.",
            CHyprColor{1.0, 0.2, 0.2, 1.0},
            5000);
        throw std::runtime_error("Breezy Desktop Hyprland plugin version mismatch");
    }

    notify("[Breezy Desktop] Hyprland plugin loaded", CHyprColor{0.2, 0.8, 1.0, 1.0}, 3000);
    logDebug("plugin loaded");
    addConfigValues();
    pollRuntimeState(true);
    startPosePolling();
    startRenderHooks();
    startControlHooks();

    return {
        "breezy-desktop",
        "Breezy Desktop XR compositor backend for Hyprland",
        "wheaney",
        BREEZY_DESKTOP_VERSION_STR,
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    stopControlHooks();
    stopRenderHooks();
    stopPosePolling();
    logDebug("plugin unloaded");
    notify("[Breezy Desktop] Hyprland plugin unloaded", CHyprColor{0.8, 0.8, 0.8, 1.0}, 3000);
}
