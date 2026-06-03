#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/MonitorResources.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>

#include "monitor_discovery.hpp"
#include "pose_data.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
uint64_t g_mirrorSaveChecks = 0;
SP<Render::ITexture> g_virtualMonitorTexture;
Vector2D g_virtualMonitorTextureSize;
GLuint g_curvedMeshProgram = 0;
GLuint g_curvedMeshVao = 0;
GLuint g_curvedMeshVbo = 0;
GLint g_curvedMeshTextureUniform = -1;
std::string g_fakeMirrorSourceName;
std::string g_fakeMirrorTargetName;
bool g_poseBaselineSet = false;
Quat g_poseBaseline;
bool g_previousPoseSet = false;
Quat g_previousPose;
int g_stillPoseFrames = 0;
bool g_poseOffsetInitialized = false;
PoseOffset g_smoothedPoseOffset;

constexpr double BREEZY_PI = 3.14159265358979323846;
constexpr double DEFAULT_POSE_SMOOTHING_ALPHA = 0.45;
constexpr double DEFAULT_POSE_MAX_STEP_RATIO = 0.35;
constexpr double DEFAULT_POSE_NORMALIZED_DEADZONE = 0.03;
constexpr double DEFAULT_POSE_NORMALIZED_LIMIT = 6.0;
constexpr double DEFAULT_DRIFT_CORRECTION_ALPHA = 0.001;
constexpr double DEFAULT_DRIFT_STILLNESS_RADIANS = 0.002;
constexpr double DEFAULT_DRIFT_CORRECTION_ZONE = 0.25;
constexpr int DRIFT_STILL_FRAMES_REQUIRED = 30;
constexpr double DEFAULT_XR_SCREEN_SCALE = 0.95;
constexpr double DEFAULT_XR_CURVATURE = 0.0;
constexpr double XR_CENTERED_THRESHOLD = 0.35;
constexpr int CURVED_TEXTURE_SEGMENTS = 128;
constexpr const char* RECENTER_DISPATCHER = "breezy_recenter";
constexpr const char* RECENTER_HYPRCTL_COMMAND = "breezy-recenter";
constexpr const char* CONFIG_SCREEN_SCALE = "plugin:breezy:screen_scale";
constexpr const char* CONFIG_CURVATURE = "plugin:breezy:curvature";
constexpr const char* CONFIG_SMOOTHING_ALPHA = "plugin:breezy:smoothing_alpha";
constexpr const char* CONFIG_MAX_STEP_RATIO = "plugin:breezy:max_step_ratio";
constexpr const char* CONFIG_DEADZONE = "plugin:breezy:deadzone";
constexpr const char* CONFIG_MOVEMENT_LIMIT = "plugin:breezy:movement_limit";
constexpr const char* CONFIG_DRIFT_CORRECTION_ALPHA = "plugin:breezy:drift_correction_alpha";
constexpr const char* CONFIG_DRIFT_STILLNESS_RADIANS = "plugin:breezy:drift_stillness_radians";
constexpr const char* CONFIG_DRIFT_CORRECTION_ZONE = "plugin:breezy:drift_correction_zone";

using RenderMonitorFn = void (*)(Render::IHyprRenderer*, PHLMONITOR, bool);

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
    g_previousPoseSet = false;
    g_previousPose = {};
    g_stillPoseFrames = 0;
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

Quat quatNormalize(const Quat& q) {
    const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length <= 0.0)
        return {};

    return {.x = q.x / length, .y = q.y / length, .z = q.z / length, .w = q.w / length};
}

double quatDot(const Quat& a, const Quat& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

double quatAngularDistance(const Quat& a, const Quat& b) {
    const double dot = std::clamp(std::abs(quatDot(a, b)), 0.0, 1.0);
    return 2.0 * std::acos(dot);
}

Quat quatNlerp(const Quat& from, Quat to, const double alpha) {
    if (quatDot(from, to) < 0.0)
        to = {.x = -to.x, .y = -to.y, .z = -to.z, .w = -to.w};

    return quatNormalize({
        .x = from.x + (to.x - from.x) * alpha,
        .y = from.y + (to.y - from.y) * alpha,
        .z = from.z + (to.z - from.z) * alpha,
        .w = from.w + (to.w - from.w) * alpha,
    });
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
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_CURVATURE, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_XR_CURVATURE)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_SMOOTHING_ALPHA, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_SMOOTHING_ALPHA)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_MAX_STEP_RATIO, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_MAX_STEP_RATIO)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_DEADZONE, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_NORMALIZED_DEADZONE)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_MOVEMENT_LIMIT, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_POSE_NORMALIZED_LIMIT)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_DRIFT_CORRECTION_ALPHA, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_DRIFT_CORRECTION_ALPHA)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_DRIFT_STILLNESS_RADIANS, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_DRIFT_STILLNESS_RADIANS)));
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_DRIFT_CORRECTION_ZONE, Hyprlang::CConfigValue(static_cast<Hyprlang::FLOAT>(DEFAULT_DRIFT_CORRECTION_ZONE)));
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

PoseOffset normalizedOffsetForPose(const Quat& current, const Quat& baseline, const double horizontalHalfTangent, const double verticalHalfTangent) {
    const Quat relative = quatMultiply(quatConjugate(current), baseline);
    const Vec3 anchorDirection = rotateVector({.x = 0.0, .y = 0.0, .z = -1.0}, relative);
    const double depth = std::max(-anchorDirection.z, 0.001);
    const double movementLimit = configFloat(CONFIG_MOVEMENT_LIMIT, DEFAULT_POSE_NORMALIZED_LIMIT);

    return {
        .x = std::clamp((anchorDirection.x / depth) / horizontalHalfTangent, -movementLimit, movementLimit),
        .y = std::clamp((anchorDirection.y / depth) / verticalHalfTangent, -movementLimit, movementLimit),
    };
}

void updateDriftCorrection(const Quat& current, const PoseOffset& normalizedOffset) {
    if (g_previousPoseSet) {
        const double angularStep = quatAngularDistance(current, g_previousPose);
        if (angularStep <= configFloat(CONFIG_DRIFT_STILLNESS_RADIANS, DEFAULT_DRIFT_STILLNESS_RADIANS))
            g_stillPoseFrames = std::min(g_stillPoseFrames + 1, DRIFT_STILL_FRAMES_REQUIRED);
        else
            g_stillPoseFrames = 0;
    }

    g_previousPose = current;
    g_previousPoseSet = true;

    const double correctionZone = configFloat(CONFIG_DRIFT_CORRECTION_ZONE, DEFAULT_DRIFT_CORRECTION_ZONE);
    const bool nearCenter = std::abs(normalizedOffset.x) <= correctionZone && std::abs(normalizedOffset.y) <= correctionZone;
    if (g_stillPoseFrames < DRIFT_STILL_FRAMES_REQUIRED || !nearCenter)
        return;

    const double alpha = std::clamp(configFloat(CONFIG_DRIFT_CORRECTION_ALPHA, DEFAULT_DRIFT_CORRECTION_ALPHA), 0.0, 1.0);
    if (alpha <= 0.0)
        return;

    g_poseBaseline = quatNlerp(g_poseBaseline, current, alpha);
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
            if (const auto resources = source->resources(); resources)
                resources->enableMirror();
            return;
        }
    }

    source->m_mirrors.emplace_back(target->m_self);
    if (const auto resources = source->resources(); resources)
        resources->enableMirror();
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
    const Quat current = quatNormalize({
        .x = -q[1],
        .y = q[2],
        .z = -q[0],
        .w = q[3],
    });

    if (!g_poseBaselineSet) {
        g_poseBaselineSet = true;
        g_poseBaseline = current;
    }

    const double diagonalFovRadians = std::clamp(static_cast<double>(pose->diagonalFov), 1.0, 179.0) * BREEZY_PI / 180.0;
    const double aspect = source->m_transformedSize.x / source->m_transformedSize.y;
    const double diagonalHalfTangent = std::tan(diagonalFovRadians / 2.0);
    const double verticalHalfTangent = diagonalHalfTangent / std::sqrt((aspect * aspect) + 1.0);
    const double horizontalHalfTangent = verticalHalfTangent * aspect;

    auto normalizedOffset = normalizedOffsetForPose(current, g_poseBaseline, horizontalHalfTangent, verticalHalfTangent);
    updateDriftCorrection(current, normalizedOffset);
    normalizedOffset = normalizedOffsetForPose(current, g_poseBaseline, horizontalHalfTangent, verticalHalfTangent);

    const double normalizedX = applyDeadzone(normalizedOffset.x);
    const double normalizedY = applyDeadzone(normalizedOffset.y);

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

struct CurvedMeshVertex {
    GLfloat x;
    GLfloat y;
    GLfloat u;
    GLfloat v;
};

bool compileCurvedMeshShader(GLuint shader, const char* source) {
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return true;

#ifdef BREEZY_HYPRLAND_DEBUG
    std::array<GLchar, 1024> error{};
    GLsizei length = 0;
    glGetShaderInfoLog(shader, error.size(), &length, error.data());
    logDebug("curved mesh shader compile failed: " + std::string(error.data(), static_cast<size_t>(length)));
#endif
    return false;
}

bool ensureCurvedMeshProgram() {
    if (g_curvedMeshProgram != 0)
        return true;

    constexpr const char* vertexShaderSource = R"glsl(
#version 320 es
precision highp float;
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
out vec2 vUv;
void main() {
    vUv = uv;
    gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";

    constexpr const char* fragmentShaderSource = R"glsl(
#version 320 es
precision highp float;
in vec2 vUv;
uniform sampler2D sourceTexture;
out vec4 fragColor;
void main() {
    fragColor = texture(sourceTexture, vUv);
}
)glsl";

    const GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    const GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compileCurvedMeshShader(vertexShader, vertexShaderSource) || !compileCurvedMeshShader(fragmentShader, fragmentShaderSource)) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
#ifdef BREEZY_HYPRLAND_DEBUG
        std::array<GLchar, 1024> error{};
        GLsizei length = 0;
        glGetProgramInfoLog(program, error.size(), &length, error.data());
        logDebug("curved mesh shader link failed: " + std::string(error.data(), static_cast<size_t>(length)));
#endif
        glDeleteProgram(program);
        return false;
    }

    g_curvedMeshProgram = program;
    g_curvedMeshTextureUniform = glGetUniformLocation(g_curvedMeshProgram, "sourceTexture");
    glGenVertexArrays(1, &g_curvedMeshVao);
    glGenBuffers(1, &g_curvedMeshVbo);
    return true;
}

void drawCurvedMeshTexture(SP<Render::ITexture> texture, const CBox& box, const double curvature) {
    auto monitor = g_pHyprRenderer && g_pHyprRenderer->m_renderData.pMonitor ? g_pHyprRenderer->m_renderData.pMonitor.lock() : nullptr;
    if (!monitor || !texture || texture->m_texID == 0 || !ensureCurvedMeshProgram())
        return;

    const double clampedCurvature = std::clamp(curvature, 0.0, 1.0);
    const bool flat = clampedCurvature <= 0.001;
    const double maxAngle = flat ? 0.0 : std::clamp(clampedCurvature * (BREEZY_PI / 2.0), 0.001, 1.25);
    const double centerX = box.x + (box.w / 2.0);
    const double centerY = box.y + (box.h / 2.0);
    const double wrapWidth = box.w * (1.0 + (clampedCurvature * 0.6));
    const double halfWidth = wrapWidth / 2.0;
    const double viewportW = std::max(monitor->m_transformedSize.x, 1.0);
    const double viewportH = std::max(monitor->m_transformedSize.y, 1.0);

    auto projectedX = [&](const double normalized) {
        if (flat)
            return centerX + halfWidth * normalized;

        const double sinMaxAngle = std::sin(maxAngle);
        if (std::abs(sinMaxAngle) < 0.001)
            return centerX + halfWidth * normalized;
        return centerX + halfWidth * (std::sin(normalized * maxAngle) / sinMaxAngle);
    };

    auto toNdcX = [&](const double x) {
        return static_cast<GLfloat>((x / viewportW) * 2.0 - 1.0);
    };
    auto toNdcY = [&](const double y) {
        return static_cast<GLfloat>(1.0 - (y / viewportH) * 2.0);
    };

    std::vector<CurvedMeshVertex> vertices;
    vertices.reserve((CURVED_TEXTURE_SEGMENTS + 1) * 2);
    for (int i = 0; i <= CURVED_TEXTURE_SEGMENTS; ++i) {
        const double u = static_cast<double>(i) / CURVED_TEXTURE_SEGMENTS;
        const double normalized = (u * 2.0) - 1.0;
        const double x = projectedX(normalized);
        const double y0 = centerY - (box.h / 2.0);
        const double y1 = centerY + (box.h / 2.0);

        vertices.push_back({toNdcX(x), toNdcY(y0), static_cast<GLfloat>(u), 1.0F});
        vertices.push_back({toNdcX(x), toNdcY(y1), static_cast<GLfloat>(u), 0.0F});
    }

    GLint previousProgram = 0;
    GLint previousVao = 0;
    GLint previousArrayBuffer = 0;
    GLint previousTexture = 0;
    GLint previousActiveTexture = 0;
    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_curvedMeshProgram);
    glUniform1i(g_curvedMeshTextureUniform, 0);
    glBindTexture(GL_TEXTURE_2D, texture->m_texID);
    glBindVertexArray(g_curvedMeshVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_curvedMeshVbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(CurvedMeshVertex)), vertices.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CurvedMeshVertex), reinterpret_cast<void*>(offsetof(CurvedMeshVertex, x)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(CurvedMeshVertex), reinterpret_cast<void*>(offsetof(CurvedMeshVertex, u)));
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(vertices.size()));

    glBindBuffer(GL_ARRAY_BUFFER, previousArrayBuffer);
    glBindVertexArray(previousVao);
    glBindTexture(GL_TEXTURE_2D, previousTexture);
    glActiveTexture(previousActiveTexture);
    glUseProgram(previousProgram);
    if (blendEnabled)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
}

class CurvedTexturePassElement : public IPassElement {
  public:
    CurvedTexturePassElement(SP<Render::ITexture> texture, const CBox& box, const double curvature) :
        m_texture(texture), m_box(box), m_curvature(std::clamp(curvature, 0.0, 1.0)) {}

    std::vector<UP<IPassElement>> draw() override {
        std::vector<UP<IPassElement>> elements;
        if (!m_texture || m_box.w <= 0 || m_box.h <= 0)
            return elements;

        drawCurvedMeshTexture(m_texture, m_box, m_curvature);
        return elements;
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    const char* passName() override {
        return "BreezyCurvedTexture";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

    std::optional<CBox> boundingBox() override {
        return m_box;
    }

    bool disableSimplification() override {
        return true;
    }

  private:
    SP<Render::ITexture> m_texture;
    CBox m_box;
    double m_curvature = 0.0;
};

void cacheVirtualMonitorTexture() {
    auto monitor = g_currentRenderMonitor.lock();
    if (!isVirtualRenderMonitor(monitor) || !g_pHyprRenderer)
        return;

    const auto resources = monitor->resources();
    const auto glBackend = g_pHyprRenderer->glBackend().lock();
    const bool needsCopy = monitor->needsACopyFB();
    if (needsCopy && resources && !resources->hasMirrorFB())
        resources->mirrorFB();
    g_mirrorSaveChecks++;
    if (g_mirrorSaveChecks % 60 == 1)
        logDebug(
            "mirror save check count=" + std::to_string(g_mirrorSaveChecks) +
            " needsCopy=" + std::to_string(needsCopy) +
            " glBackend=" + std::to_string(static_cast<bool>(glBackend)) +
            " hasMirrorFB=" + std::to_string(resources && resources->hasMirrorFB()) +
            " hasMirrorTexture=" + std::to_string(resources && resources->m_mirrorTex && resources->m_mirrorTex->m_texID != 0));
    if (needsCopy && glBackend) {
        const CBox monitorBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
        glBackend->saveBufferForMirror(monitorBox);
        if (g_mirrorSaveChecks % 60 == 1)
            logDebug(
                "mirror save after count=" + std::to_string(g_mirrorSaveChecks) +
                " hasMirrorFB=" + std::to_string(resources && resources->hasMirrorFB()) +
                " hasMirrorTexture=" + std::to_string(resources && resources->m_mirrorTex && resources->m_mirrorTex->m_texID != 0));
    }

    auto textureFromFramebuffer = [](const SP<Render::IFramebuffer>& fb) -> SP<Render::ITexture> {
        if (!fb || !fb->isAllocated())
            return nullptr;

        auto texture = fb->getTexture();
        if (!texture || texture->m_texID == 0)
            return nullptr;

        return texture;
    };

    const auto mirrorFB = resources && resources->hasMirrorFB() ? resources->mirrorFB() : nullptr;
    auto texture = resources ? resources->getMirrorTexture() : nullptr;
    if (!texture)
        texture = textureFromFramebuffer(mirrorFB);
    if (!texture) {
        if (g_virtualCacheHits % 60 == 0) {
            auto sizeOf = [](const SP<Render::IFramebuffer>& fb) {
                if (!fb)
                    return std::string("null");
                return std::to_string(static_cast<int>(fb->m_size.x)) + "x" + std::to_string(static_cast<int>(fb->m_size.y));
            };
            const bool hasMirrorFB = resources && resources->hasMirrorFB();
            const bool hasMirrorTexture = resources && resources->m_mirrorTex && resources->m_mirrorTex->m_texID != 0;
            logDebug(
                "virtual mirror texture unavailable hasMirrorFB=" + std::to_string(hasMirrorFB) +
                " hasMirrorTexture=" + std::to_string(hasMirrorTexture) +
                " mirror=" + sizeOf(mirrorFB) +
                " current=" + sizeOf(g_pHyprRenderer->m_renderData.currentFB) +
                " main=" + sizeOf(g_pHyprRenderer->m_renderData.mainFB) +
                " out=" + sizeOf(g_pHyprRenderer->m_renderData.outFB));
        }
        return;
    }

    g_virtualMonitorTexture = texture;
    if (texture->m_size.x > 0 && texture->m_size.y > 0)
        g_virtualMonitorTextureSize = texture->m_size;
    else if (mirrorFB)
        g_virtualMonitorTextureSize = mirrorFB->m_size;
    g_virtualCacheHits++;
    if (g_virtualCacheHits % 60 == 1)
        logDebug(
            "virtual texture cached count=" + std::to_string(g_virtualCacheHits) +
            " tex=" + std::to_string(g_virtualMonitorTexture->m_texID) +
            " size=" + std::to_string(static_cast<int>(g_virtualMonitorTextureSize.x)) +
            "x" + std::to_string(static_cast<int>(g_virtualMonitorTextureSize.y)));
    scheduleActiveXrFrames();
}

void renderVirtualMonitorTexture() {
    auto monitor = g_currentRenderMonitor.lock();
    if (!g_session.ready || !isTargetRenderMonitor(monitor) || !g_pHyprRenderer || !g_virtualMonitorTexture || g_session.virtualMonitorName.empty())
        return;

    const auto mirrored = g_pCompositor ? g_pCompositor->getMonitorFromName(g_session.virtualMonitorName) : nullptr;
    if (!mirrored)
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
    box.y += offset.y;

    g_pHyprRenderer->m_renderPass.add(Hyprutils::Memory::makeUnique<CClearPassElement>(CClearPassElement::SClearData{CHyprColor(0, 0, 0, 1.0)}));

    g_virtualMonitorTexture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_virtualMonitorTexture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    const double curvature = std::clamp(configFloat(CONFIG_CURVATURE, DEFAULT_XR_CURVATURE), 0.0, 1.0);
    g_pHyprRenderer->m_renderPass.add(Hyprutils::Memory::makeUnique<CurvedTexturePassElement>(g_virtualMonitorTexture, box, curvature));
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

void hkRenderMonitor(Render::IHyprRenderer* renderer, PHLMONITOR monitor, bool commit) {
    g_currentRenderMonitor = monitor;
    noteTargetRenderMonitor(monitor);
    if (isTargetRenderMonitor(monitor)) {
        g_targetRenderHits++;
        if (g_targetRenderHits % 120 == 1)
            logDebug("target render hit count=" + std::to_string(g_targetRenderHits));
    }
    if (isVirtualRenderMonitor(monitor)) {
        g_virtualRenderHits++;
        if (g_virtualRenderHits % 60 == 1) {
            const auto resources = monitor->resources();
            logDebug(
                "virtual render hit count=" + std::to_string(g_virtualRenderHits) +
                " monitor=" + monitor->m_name +
                " mirrors=" + std::to_string(monitor->m_mirrors.size()) +
                " needsCopy=" + std::to_string(monitor->needsACopyFB()) +
                " hasMirrorFB=" + std::to_string(resources && resources->hasMirrorFB()));
        }
    }

    const auto original = reinterpret_cast<RenderMonitorFn>(g_renderMonitorHook->m_original);
    if (isVirtualRenderMonitor(monitor)) {
        ensureFakeMirrorCapture();
        if (g_virtualRenderHits % 60 == 1) {
            const auto resources = monitor->resources();
            logDebug(
                "virtual before original mirrors=" + std::to_string(monitor->m_mirrors.size()) +
                " needsCopy=" + std::to_string(monitor->needsACopyFB()) +
                " hasMirrorFB=" + std::to_string(resources && resources->hasMirrorFB()));
        }
    }

    original(renderer, monitor, commit);

    if (isVirtualRenderMonitor(monitor)) {
        if (g_virtualRenderHits % 60 == 1) {
            const auto resources = monitor->resources();
            logDebug(
                "virtual after original mirrors=" + std::to_string(monitor->m_mirrors.size()) +
                " needsCopy=" + std::to_string(monitor->needsACopyFB()) +
                " hasMirrorFB=" + std::to_string(resources && resources->hasMirrorFB()));
        }
        cacheVirtualMonitorTexture();
    }

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
        reinterpret_cast<void*>(&Render::IHyprRenderer::renderMonitor),
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
