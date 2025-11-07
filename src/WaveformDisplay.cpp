#include "WaveformDisplay.h"
#include "WaveformGenerator.h"
#include "WaveformTheme.h"
#include "FrameTiming.h"
#include <QPainter>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QOpenGLContext>
#include <QDebug>
#include <QOpenGLShader>
#include <QVector4D>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>

WaveformDisplay::WaveformDisplay(QWidget* parent)
    : QOpenGLWidget(parent)
{
    QSurfaceFormat fmt = format();
    if (fmt.samples() < 4) fmt.setSamples(4);
    fmt.setSwapInterval(1);
    setFormat(fmt);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    // Avoid unnecessary background compositing with OpenGL, helps reduce flicker
    setAutoFillBackground(false);

    formatManager.registerBasicFormats();
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    setFocusPolicy(Qt::StrongFocus);
    renderTimer = new QTimer(this);
    renderTimer->setTimerType(Qt::PreciseTimer);
    renderTimer->setInterval(FrameTiming::kFrameIntervalMs);
    connect(renderTimer, &QTimer::timeout, this, [this]() { if (isVisible()) update(); });

    // Default: keep potentially-distracting overlays disabled unless explicitly enabled
    missingSegmentsOverlayEnabled = false;
    prerollOverlayEnabled = false;
}

WaveformDisplay::~WaveformDisplay()
{
    if (renderTimer) renderTimer->stop();

    for (auto& connection : scratchEngineConnections) QObject::disconnect(connection);
    scratchEngineConnections.clear();

    // Ensure streaming stops and internal state is cleared before GL teardown
    resetStreamingState();

    QOpenGLContext* ctx = context();
    if (ctx && ctx->isValid()) {
        makeCurrent();
        destroyGlResources();
        doneCurrent();
    } else {
        // No valid context; ensure we don't try to use GL after this
        glResources.initialized = false;
        glResources.fillVbo[0] = glResources.fillVbo[1] = 0;
        glResources.topLineVbo[0] = glResources.topLineVbo[1] = 0;
        glResources.bottomLineVbo[0] = glResources.bottomLineVbo[1] = 0;
        glResources.fillVertexCountFront = 0;
        glResources.topLineVertexCountFront = 0;
        glResources.bottomLineVertexCountFront = 0;
    }
}

namespace
{
const char* kWaveformFillVertexShaderCore = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec3 color;
out vec3 vColor;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vColor = color;
}
)";

const char* kWaveformFillFragmentShaderCore = R"(#version 330 core
in vec3 vColor;
out vec4 fragColor;
void main()
{
    fragColor = vec4(vColor, 0.78);
}
)";

const char* kWaveformFillVertexShaderEs = R"(#version 300 es
layout(location = 0) in vec2 position;
layout(location = 1) in vec3 color;
out mediump vec3 vColor;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vColor = color;
}
)";

const char* kWaveformFillFragmentShaderEs = R"(#version 300 es
precision mediump float;
in mediump vec3 vColor;
out vec4 fragColor;
void main()
{
    fragColor = vec4(vColor, 0.78);
}
)";

const char* kWaveformLineVertexShaderCore = R"(#version 330 core
layout(location = 0) in vec2 position;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

const char* kWaveformLineFragmentShaderCore = R"(#version 330 core
out vec4 fragColor;
uniform vec4 uColor;
void main()
{
    fragColor = uColor;
}
)";

const char* kWaveformLineVertexShaderEs = R"(#version 300 es
layout(location = 0) in vec2 position;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

const char* kWaveformLineFragmentShaderEs = R"(#version 300 es
precision mediump float;
out vec4 fragColor;
uniform vec4 uColor;
void main()
{
    fragColor = uColor;
}
)";
}

// Centralized audio/display mapping using the current geometry cache and tempo snapshot
double WaveformDisplay::mapAudioToDisplay(double audioSec) const {
    const double safeTempo = (geometryCache.lastTempoFactor > 1e-6) ? geometryCache.lastTempoFactor : 1.0;
    const double nudgeSec = geometryCache.waveformNudgeSec;
    if (viewMode == ViewMode::BeatLocked) {
        return geometryCache.displayCenterSec + (audioSec - geometryCache.playheadSec) / safeTempo + geometryCache.alignShiftSec - nudgeSec;
    }
    return audioSec + geometryCache.alignShiftSec - nudgeSec;
}

double WaveformDisplay::mapDisplayToAudio(double displaySec) const {
    const double safeTempo = (geometryCache.lastTempoFactor > 1e-6) ? geometryCache.lastTempoFactor : 1.0;
    const double adjustedDisplay = displaySec + geometryCache.waveformNudgeSec;
    if (viewMode == ViewMode::BeatLocked) {
        return geometryCache.playheadSec + (adjustedDisplay - geometryCache.displayCenterSec - geometryCache.alignShiftSec) * safeTempo;
    }
    return adjustedDisplay - geometryCache.alignShiftSec;
}

void WaveformDisplay::buildWaveformGeometry(int viewWidth, int viewHeight, double zoomFactor, double renderPlayheadRel)
{
    geometryCache.valid = false;
    geometryCache.leftSecond = 0.0;
    geometryCache.rightSecond = 0.0;
    geometryCache.timeRange = 0.0;
    geometryCache.playheadSec = 0.0;
    geometryCache.displayCenterSec = 0.0;
    geometryCache.bufferSec = 0.0;
    geometryCache.halfViewportTime = 0.0;
    geometryCache.alignShiftSec = 0.0;
    geometryCache.fetchLeftSecond = 0.0;
    geometryCache.fetchRightSecond = 0.0;
    geometryCache.waveformNudgeSec = 0.0;

    auto& upperPoints = upperPointBuffer;
    auto& lowerPoints = lowerPointBuffer;
    auto& missingSegments = missingSegmentBuffer;
    upperPoints.clear();
    lowerPoints.clear();
    missingSegments.clear();

    // ATOMARER Lock: einmal nehmen und durchgehend halten
    std::shared_lock<std::shared_mutex> sharedLock(sourceMutex);
    
    // Chunk-Cache prüfen (ohne Lock zu wechseln)
    if (chunkCacheDirty) {
        // Upgrade zu exclusive lock nur wenn wirklich nötig
        sharedLock.unlock();
        std::unique_lock<std::shared_mutex> exclusiveLock(sourceMutex);
        
        // Double-check nach Lock-Upgrade
        if (chunkCacheDirty) {
            rebuildChunkCacheIfNeeded_Locked(exclusiveLock);
        }
        
        // Downgrade zurück zu shared
        exclusiveLock.unlock();
        sharedLock.lock();
    }

    // Lokale atomare Kopien der kritischen Daten erstellen
    // Lock nur für die Dauer der Kopie halten, dann sofort freigeben
    std::vector<float> localSourceMinBins;
    std::vector<float> localSourceMaxBins;
    std::vector<float> localSourceLowBins;
    std::vector<float> localSourceMidBins;
    std::vector<float> localSourceHighBins;
    int localSourceWidth;
    int localAvailableStartBin;
    int localAvailableEndBin;
    double localAudioLength;
    
    {
        // Atomare Kopie unter Lock - dann sofort freigeben!
        localSourceMinBins = sourceMinBins;
        localSourceMaxBins = sourceMaxBins;
        localSourceLowBins = sourceLowBins;
        localSourceMidBins = sourceMidBins;
        localSourceHighBins = sourceHighBins;
        localSourceWidth = sourceWidth;
        localAvailableStartBin = availableStartBin;
        localAvailableEndBin = availableEndBin;
        localAudioLength = audioLength;
    } // sharedLock wird hier automatisch durch Scope-Ende freigegeben
    
    sharedLock.unlock(); // Explizit freigeben bevor wir weitermachen
    
    if (viewWidth <= 0 || viewHeight <= 0 || localAudioLength <= 0.0 || localSourceWidth <= 0) {
        return;
    }

    const int centerY = viewHeight / 2;
    const int viewportWidth = std::max(viewWidth, 1);
    const int pixelWidth = viewportWidth;

    const double basePixelsPerSecond = useFixedPixelsPerSecond
        ? localPixelsPerSecond
        : static_cast<double>(viewWidth) / std::max(1.0, localAudioLength);
    const double safeTempo = tempoFactor > 1e-6 ? tempoFactor : 1.0;
    const double pixelsPerSecond = basePixelsPerSecond * zoomFactor;

    double playheadRel = renderPlayheadRel;
    double playheadSec = 0.0;
    if (playheadRel < 0.0 && prerollEnabled) {
        playheadSec = playheadRel * prerollTimeSec;
    } else {
        playheadRel = std::clamp(playheadRel, 0.0, 1.0);
        playheadSec = playheadRel * localAudioLength;
    }

    // Shift display center by output latency so visual center matches what you hear
    // BUG #15 FIX: Adaptive Snap-Grid basierend auf Playback-Velocity
    // Paused: Ganzes Pixel für Stabilität | Playing: Sub-Pixel für Smoothness
    const double rawDisplayCenterSec = (viewMode == ViewMode::BeatLocked)
        ? ((playheadSec + renderLatencySec) / safeTempo)
        : (playheadSec + renderLatencySec);
    
    // Calculate seconds per pixel BEFORE snapping
    const double bufferSec = std::max(0.05, 0.5 / std::max(1.0, zoomFactor));
    const double halfViewportTime = static_cast<double>(viewportWidth) / (2.0 * pixelsPerSecond);
    const double secondsPerPixel = (pixelsPerSecond > 1e-6) 
        ? (1.0 / pixelsPerSecond) : 0.0;
    
    // FIX #2: isPausedMode mit Member Variable + Hysterese mit Deadband
    const double velocityMag = std::abs(std::isfinite(estimatedPlaybackRate) ? estimatedPlaybackRate : 0.0);
    
    // Hysterese mit Dead-Zone (verhindert Toggling bei kleinen Schwankungen)
    const double deadband = 0.001;
    if (isPausedMode_ && velocityMag > (0.002 + deadband)) {
        isPausedMode_ = false;
    } else if (!isPausedMode_ && velocityMag < (0.0005 - deadband)) {
        isPausedMode_ = true;
    }
    
    const double snapGrid = isPausedMode_ 
        ? secondsPerPixel          // Paused: 1.0px snap
        : secondsPerPixel * 0.5;   // Playing: 0.5px snap
    
    const double displayCenterSec = std::round(rawDisplayCenterSec / snapGrid) * snapGrid;

    const double visibleLeftSecond = displayCenterSec - halfViewportTime;
    const double visibleRightSecond = displayCenterSec + halfViewportTime;
    const double timeRange = visibleRightSecond - visibleLeftSecond;
    if (timeRange <= 0.0) {
        return;
    }

    const double secondsPerPixelDisplay = timeRange / static_cast<double>(viewportWidth);

    double dpiX = static_cast<double>(logicalDpiX());
    if (dpiX <= 1.0) dpiX = 96.0; // sensible fallback
    const double pixelsPerCentimeter = dpiX / 2.54; // 1 inch = 2.54 cm
    
    // BUG #19 FIX: Behalte als double (kein int cast) für Sub-Pixel-Präzision
    const double waveformNudgePx = 0.5 - (pixelsPerCentimeter * 0.35);
    
    const double extraLeftDisplaySec = std::max(0.0, -waveformNudgePx) * secondsPerPixelDisplay;
    const double extraRightDisplaySec = std::max(0.0, waveformNudgePx) * secondsPerPixelDisplay;

    const double fetchLeftSecond = visibleLeftSecond - bufferSec - extraLeftDisplaySec;
    const double fetchRightSecond = visibleRightSecond + bufferSec + extraRightDisplaySec;
    geometryCache.waveformNudgeSec = waveformNudgePx * secondsPerPixelDisplay;

    geometryCache.playheadSec = playheadSec;
    geometryCache.displayCenterSec = displayCenterSec;
    geometryCache.leftSecond = visibleLeftSecond;
    geometryCache.rightSecond = visibleRightSecond;
    geometryCache.timeRange = timeRange;
    geometryCache.bufferSec = bufferSec;
    geometryCache.halfViewportTime = halfViewportTime;
    geometryCache.fetchLeftSecond = fetchLeftSecond;
    geometryCache.fetchRightSecond = fetchRightSecond;
    geometryCache.lastWidth = viewWidth;
    geometryCache.lastHeight = viewHeight;
    geometryCache.lastZoomFactor = zoomFactor;
    geometryCache.lastTempoFactor = tempoFactor;
    geometryCache.lastPlayheadPos = renderPlayheadRel;
    geometryCache.lastAvailableStartBin = localAvailableStartBin;
    geometryCache.lastAvailableEndBin = localAvailableEndBin;

    const double leftSecond = visibleLeftSecond;
    const double rightSecond = visibleRightSecond;

    const double binPerSecond = static_cast<double>(localSourceWidth) / std::max(localAudioLength, 1e-6);

    const double audioHalfViewport = (viewMode == ViewMode::BeatLocked)
        ? (halfViewportTime * safeTempo)
        : halfViewportTime;
    const double audioBuffer = (viewMode == ViewMode::BeatLocked)
        ? (bufferSec * safeTempo)
        : bufferSec;

    const double audioMarginScale = (viewMode == ViewMode::BeatLocked) ? safeTempo : 1.0;
    const double audioMarginLeft = extraLeftDisplaySec * audioMarginScale;
    const double audioMarginRight = extraRightDisplaySec * audioMarginScale;

    double audioFetchLeftSec = playheadSec - audioHalfViewport - audioBuffer - audioMarginLeft;
    double audioFetchRightSec = playheadSec + audioHalfViewport + audioBuffer + audioMarginRight;

    if (!std::isfinite(audioFetchLeftSec)) audioFetchLeftSec = 0.0;
    if (!std::isfinite(audioFetchRightSec)) audioFetchRightSec = 0.0;

    audioFetchLeftSec = std::max(0.0, audioFetchLeftSec);
    audioFetchRightSec = std::max(audioFetchRightSec, 0.0);
    if (localAudioLength > 0.0) {
        audioFetchRightSec = std::min(audioFetchRightSec, localAudioLength);
    }

    if (audioFetchRightSec <= audioFetchLeftSec) {
        return;
    }

    // Use pure track-time (0.0 = track start) for bin selection
    int leftBin = std::max(0, static_cast<int>(std::floor(audioFetchLeftSec * binPerSecond)));
    int rightBin = std::min(localSourceWidth, static_cast<int>(std::ceil(audioFetchRightSec * binPerSecond)));

    if (streamingMode && !streamingComplete && binPerSecond > 0.0) {
        // Aggressive preloading: ±30 seconds ahead/behind
        const int aggressivePreload = streamingPreloadBins * 8; // Was *1, now *8
        const int neededStartBin = std::max(0, leftBin - aggressivePreload);
        int neededEndBin = rightBin + aggressivePreload;
        if (streamingTotalBins > 0) {
            neededEndBin = std::min(streamingTotalBins, neededEndBin);
        }
        requestStreamingWindowIfNeeded(neededStartBin, neededEndBin, binPerSecond);
    }

    if (leftBin >= rightBin && fetchRightSecond > 0.0) {
        return;
    }

    // Align start: when near or before track start (including preroll), keep audioSec=0 centered.
    // Use tolerance of ~0.75 pixel in time to avoid jitter from tiny positive playhead values.
    const double alignSnapSec = secondsPerPixel * 0.75; // tolerance band (about 0.75px)
    // Hysteresis around zero to avoid toggling and micro-jitter when hovering near start
    const double enterPadSec = secondsPerPixel * 2.0; // enter latch within ~2px
    const double exitPadSec  = secondsPerPixel * 6.0; // release latch after ~6px past threshold
    if (viewMode == ViewMode::TimeLocked) {
        if (!alignZeroLatchActive && playheadSec <= alignSnapSec + enterPadSec) {
            alignZeroLatchActive = true;
        } else if (alignZeroLatchActive && playheadSec >= alignSnapSec + exitPadSec) {
            alignZeroLatchActive = false;
        }
        if (alignZeroLatchActive) {
            alignZeroShiftSec = displayCenterSec + geometryCache.waveformNudgeSec;
        }
    } else {
        alignZeroLatchActive = false;
    }
    double alignShiftSec = alignZeroLatchActive ? alignZeroShiftSec : geometryCache.waveformNudgeSec;
    geometryCache.alignShiftSec = alignShiftSec;
    upperPoints.reserve(pixelWidth + 2);
    lowerPoints.reserve(pixelWidth + 2);
    if (missingSegmentsOverlayEnabled) {
        missingSegments.reserve(4);
    }

    // FIX #5: History Buffer Smart Allocation - verhindert Heap-Fragmentierung
    const int currentSize = static_cast<int>(pixelUpperScratch.size());
    
    if (currentSize != pixelWidth) {
        // Working Buffers: Immer resize (werden jeden Frame überschrieben)
        pixelUpperScratch.resize(pixelWidth);
        pixelLowerScratch.resize(pixelWidth);
        pixelCoverageScratch.resize(pixelWidth);
        pixelColorScratch.resize(pixelWidth * 3);
    }
        
    // FIX #4: History Buffer Smart Resize - verhindert Memory-Leak
    const size_t requiredSize = static_cast<size_t>(pixelWidth);
    const size_t currentHistSize = pixelUpperHistory.size();
    
    // Nur resize wenn:
    // 1. Buffer zu klein (< required)
    // 2. Buffer VIEL zu groß (> 150% required) → shrink
    if (currentHistSize < requiredSize || currentHistSize > requiredSize * 1.5) {
        // Bei Wachsen: 20% Reserve
        // Bei Schrumpfen: exakt required (kein Overhead)
        const size_t targetSize = (currentHistSize < requiredSize) 
            ? static_cast<size_t>(requiredSize * 1.2)
            : requiredSize;
        
        pixelUpperHistory.resize(targetSize);
        pixelLowerHistory.resize(targetSize);
        pixelColorHistory.resize(targetSize * 3);
        pixelCenterHistory.resize(targetSize);
        pixelHistoryValid.resize(targetSize);
        
        // Invalidate all nach Resize
        std::fill(pixelHistoryValid.begin(), pixelHistoryValid.end(), 0);
    }
    
    // Fill nur auf aktiven Bereich (nicht gesamten Buffer)
    std::fill(pixelUpperScratch.begin(), pixelUpperScratch.end(), 0.0f);
    std::fill(pixelLowerScratch.begin(), pixelLowerScratch.end(), 0.0f);
    std::fill(pixelCoverageScratch.begin(), pixelCoverageScratch.end(), 0);
    std::fill(pixelColorScratch.begin(), pixelColorScratch.end(), 0.0f);

    const double pixelHeight = static_cast<double>(viewHeight);
    const float waveformHeightScale = 0.42f;

    // FIXED: Balanced oversampling - enough for quality, not too much for performance
    // 2x at normal zoom, up to 3x at high zoom (was 4x, caused performance issues)
    const double oversampleFactor = std::max(2.0, std::min(3.0, 1.0 + zoomFactor * 0.25));
    const double audioWidthPerPixel = secondsPerPixelDisplay * safeTempo / oversampleFactor;
    
    // Fine alignment: nudge waveform sampling so visuals line up with audio and beat grid.

    // Streaming-aware local bin window
    // Arbeite mit lokalen Kopien - keine Race Conditions mehr
    const int totalLocalBins = static_cast<int>(localSourceMaxBins.size());
    const auto clampLocal = [totalLocalBins](int idx) { 
        return std::clamp(idx, 0, std::max(0, totalLocalBins - 1)); 
    };

    // Bei Seeks: History komplett invalidieren um Artefakte zu vermeiden
    if (isInSeekMode) {
        std::fill(pixelHistoryValid.begin(), pixelHistoryValid.end(), 0);
    }

    int currentMissingStart = -1;
    // Keep color continuous in missing regions by reusing the last valid color
    const auto fallbackColor = WaveformTheme::fallbackColor();
    float prevR = fallbackColor.r;
    float prevG = fallbackColor.g;
    float prevB = fallbackColor.b;

    const double reuseWindowSec = std::max(secondsPerPixelDisplay * 6.0, 0.012);
    const double relaxedReuseWindowSec = std::max(reuseWindowSec * 4.0, reuseWindowSec + 0.045);

    auto reuseColumnIfPossible = [&](int idx, double audioCenterSec, double toleranceSec, float& prevRRef, float& prevGRef, float& prevBRef) -> bool {
        if (idx < 0 || idx >= pixelWidth) return false;
        if (pixelHistoryValid.empty() || pixelCenterHistory.empty() ||
            pixelUpperHistory.empty() || pixelLowerHistory.empty() || pixelColorHistory.empty()) {
            return false;
        }
        if (idx >= static_cast<int>(pixelHistoryValid.size()) ||
            idx >= static_cast<int>(pixelCenterHistory.size()) ||
            idx * 3 + 2 >= static_cast<int>(pixelColorHistory.size())) {
            return false;
        }
        if (!pixelHistoryValid[idx]) return false;

        const double lastCenter = pixelCenterHistory[idx];
        if (!std::isfinite(lastCenter)) return false;
        if (std::isfinite(toleranceSec) && std::abs(lastCenter - audioCenterSec) > toleranceSec) return false;

        pixelUpperScratch[idx] = pixelUpperHistory[idx];
        pixelLowerScratch[idx] = pixelLowerHistory[idx];
        pixelCoverageScratch[idx] = 1;
        const float r = pixelColorHistory[idx * 3 + 0];
        const float g = pixelColorHistory[idx * 3 + 1];
        const float b = pixelColorHistory[idx * 3 + 2];
        pixelColorScratch[idx * 3 + 0] = r;
        pixelColorScratch[idx * 3 + 1] = g;
        pixelColorScratch[idx * 3 + 2] = b;
        prevRRef = r;
        prevGRef = g;
        prevBRef = b;
        return true;
    };

    auto tryReuseColumn = [&](int idx, double audioCenterSec, float& prevRRef, float& prevGRef, float& prevBRef) -> bool {
        const double tolerances[] = { reuseWindowSec, relaxedReuseWindowSec };
        for (double tolerance : tolerances) {
            if (reuseColumnIfPossible(idx, audioCenterSec, tolerance, prevRRef, prevGRef, prevBRef)) {
                return true;
            }
        }
        return false;
    };

    auto invalidateHistory = [&](int idx) {
        if (!pixelHistoryValid.empty()) pixelHistoryValid[idx] = 0;
        if (!pixelCenterHistory.empty()) pixelCenterHistory[idx] = std::numeric_limits<double>::quiet_NaN();
    };

    auto setBaselineColumn = [&](int idx, float lastR, float lastG, float lastB) {
        pixelUpperScratch[idx] = 0.0f;
        pixelLowerScratch[idx] = 0.0f;
        pixelCoverageScratch[idx] = 1;
        pixelColorScratch[idx * 3 + 0] = lastR;
        pixelColorScratch[idx * 3 + 1] = lastG;
        pixelColorScratch[idx * 3 + 2] = lastB;
        invalidateHistory(idx);
    };

    auto touchHistory = [&](int idx, double centerSec) {
        if (idx < 0 || idx >= pixelWidth) return;
        if (!pixelHistoryValid.empty()) pixelHistoryValid[idx] = 1;
        if (!pixelCenterHistory.empty()) pixelCenterHistory[idx] = centerSec;
    };

    // FIX #2: Optimiere storeHistory - FAST PATH ohne komplexe Berechnungen
    auto storeHistory = [&](int idx, double centerSec) {
        // Bounds check only
        if (idx < 0 || idx >= pixelWidth) return;
        
        // Simple direct writes (keine komplexen Toleranz-Checks)
        if (!pixelHistoryValid.empty()) pixelHistoryValid[idx] = 1;
        if (!pixelCenterHistory.empty()) pixelCenterHistory[idx] = centerSec;
        if (!pixelUpperHistory.empty()) pixelUpperHistory[idx] = pixelUpperScratch[idx];
        if (!pixelLowerHistory.empty()) pixelLowerHistory[idx] = pixelLowerScratch[idx];
        
        // Color nur wenn Array groß genug (no overhead)
        if (static_cast<size_t>(idx * 3 + 2) < pixelColorHistory.size()) {
            pixelColorHistory[idx * 3 + 0] = pixelColorScratch[idx * 3 + 0];
            pixelColorHistory[idx * 3 + 1] = pixelColorScratch[idx * 3 + 1];
            pixelColorHistory[idx * 3 + 2] = pixelColorScratch[idx * 3 + 2];
        }
    };

    auto reuseOrBaseline = [&](int idx, double audioCenterSec, float& lastR, float& lastG, float& lastB) {
        if (tryReuseColumn(idx, audioCenterSec, lastR, lastG, lastB)) return true;
        if (missingSegmentsOverlayEnabled && currentMissingStart < 0) currentMissingStart = idx;
        setBaselineColumn(idx, lastR, lastG, lastB);
        return false;
    };

    // FIX #1: Snap Mode State AUSSERhalb der Pixel-Loop (EINMAL pro Frame berechnen!)
    static bool useSnapMode = false;
    static double lastModeSwitch = 0.0;
    static auto lastModeUpdate = std::chrono::steady_clock::now();
    
    // Update Mode nur einmal pro Frame (nicht 1920×!)
    const auto modeUpdateNow = std::chrono::steady_clock::now();
    const double timeSinceUpdate = std::chrono::duration<double>(modeUpdateNow - lastModeUpdate).count();
    
    if (timeSinceUpdate > 0.01) {  // Max 100 Updates/Sekunde (statt 115k!)
        const double playbackSpeed = std::abs(std::isfinite(estimatedPlaybackRate) ? estimatedPlaybackRate : 0.0);
        
        // Hysterese mit größerer Dead-Zone
        if (!useSnapMode && playbackSpeed > 0.03) {  // 0.02 → 0.03
            useSnapMode = true;
            lastModeSwitch = std::chrono::duration<double>(modeUpdateNow.time_since_epoch()).count();
        } else if (useSnapMode && playbackSpeed < 0.003 && 
                   (std::chrono::duration<double>(modeUpdateNow.time_since_epoch()).count() - lastModeSwitch) > 0.5) {
            // Min 500ms zwischen Mode-Wechseln (nicht 300ms!)
            useSnapMode = false;
            lastModeSwitch = std::chrono::duration<double>(modeUpdateNow.time_since_epoch()).count();
        }
        
        lastModeUpdate = modeUpdateNow;
    }
    
    // Simple Lambda OHNE Zeit-Checks (wird 1920× aufgerufen):
    auto sampleBinSmooth = [&](const std::vector<float>& bins, double binIndex) -> float {
        if (bins.empty()) return 0.0f;
        const int maxIdx = static_cast<int>(bins.size()) - 1;
        if (binIndex <= 0.0) return bins[0];
        if (binIndex >= maxIdx) return bins[maxIdx];
        
        // KEIN std::chrono hier! Nutze vorgecachten useSnapMode
        if (!isInSeekMode && useSnapMode) {
            const int binIdx = static_cast<int>(std::round(binIndex));
            return bins[std::clamp(binIdx, 0, maxIdx)];
        }
        
        // Smooth Interpolation
        const int i0 = static_cast<int>(std::floor(binIndex));
        const int i1 = std::min(i0 + 1, maxIdx);
        const float t = static_cast<float>(binIndex - i0);
        return bins[i0] * (1.0f - t) + bins[i1] * t;
    };

    for (int x = 0; x < pixelWidth; ++x) {
        const double displayCenterXSec = leftSecond + (static_cast<double>(x) + 0.5) * secondsPerPixelDisplay;
        const double audioCenterSec = mapDisplayToAudio(displayCenterXSec);

        // Reuse fast path nur im Pause/Seek Modus (Playback braucht neue Daten)
        if ((isPausedMode_ || isInSeekMode || velocityMag < 0.0005) &&
            tryReuseColumn(x, audioCenterSec, prevR, prevG, prevB)) {
            touchHistory(x, audioCenterSec);
            continue;
        }

        double audioStart = audioCenterSec - 0.5 * audioWidthPerPixel;
        double audioEnd   = audioCenterSec + 0.5 * audioWidthPerPixel;
        if (audioEnd <= 0.0 || audioStart >= localAudioLength) {
            reuseOrBaseline(x, audioCenterSec, prevR, prevG, prevB);
            continue;
        }
        audioStart = std::max(0.0, audioStart);
        audioEnd   = std::min(localAudioLength, audioEnd);

        const int gb0 = static_cast<int>(std::floor(audioStart * binPerSecond));
        const int gb1 = std::max(gb0 + 1, static_cast<int>(std::ceil(audioEnd * binPerSecond)));

        float minVal = 0.0f, maxVal = 0.0f;
        float sumLow = 0.0f, sumMid = 0.0f, sumHigh = 0.0f;
        bool have = false;
        int count = 0;

        const bool useChunksNow = useAdaptiveChunking_ &&
            (isPausedMode_ || isInSeekMode || velocityMag < 0.01);
        
        // === LAYER 1: FALLBACK WAVEFORM (always shown as red baseline) ===
        bool haveFallback = false;
        if (fallbackComplete_) {
            std::lock_guard<std::mutex> fbLock(fallbackMutex_);
            if (!fallbackMaxBins_.empty()) {
                const int fallbackBins = static_cast<int>(fallbackMaxBins_.size());
                const double fallbackBinPerSec = streamingTotalBins > 0 
                    ? (double)fallbackBins / audioLength : 1.0;
                
                int fb0 = static_cast<int>(std::floor(audioStart * fallbackBinPerSec));
                int fb1 = static_cast<int>(std::ceil(audioEnd * fallbackBinPerSec));
                fb0 = std::clamp(fb0, 0, fallbackBins - 1);
                fb1 = std::clamp(fb1, fb0 + 1, fallbackBins);
                
                for (int i = fb0; i < fb1; ++i) {
                    if (i >= 0 && i < fallbackBins) {
                        float vmin = fallbackMinBins_[i];
                        float vmax = fallbackMaxBins_[i];
                        if (!haveFallback) {
                            minVal = vmin;
                            maxVal = vmax;
                            haveFallback = true;
                        } else {
                            minVal = std::min(minVal, vmin);
                            maxVal = std::max(maxVal, vmax);
                        }
                    }
                }
            }
        }
        
    // === LAYER 2: ADAPTIVE CHUNKS (high-quality overlay) ===
    // FIX: Nur bei Pause/Seek oder sehr niedriger Geschwindigkeit nutzen
    // Verhindert teure Kopien/Locks während aktiver Wiedergabe
    if (useChunksNow) {
            std::vector<AdaptiveChunk> localChunks;
            {
                std::lock_guard<std::mutex> lock(adaptiveChunksMutex_);
                localChunks = adaptiveChunks_; // Atomare Kopie unter Lock
            } // Lock wird hier freigegeben
            
            if (!localChunks.empty()) {
                for (const auto& chunk : localChunks) {
                    if (chunk.endBin <= gb0 || chunk.startBin >= gb1) continue;
                    
                    int overlapStart = std::max(gb0, chunk.startBin);
                    int overlapEnd = std::min(gb1, chunk.endBin);
                    int localStart = overlapStart - chunk.startBin;
                    int localEnd = overlapEnd - chunk.startBin;
                    
                    for (int i = localStart; i < localEnd; ++i) {
                        if (i < 0 || i >= static_cast<int>(chunk.maxBins.size())) continue;
                        
                        float vmin = chunk.minBins[i];
                        float vmax = chunk.maxBins[i];
                        
                        if (!have) {
                            minVal = vmin;
                            maxVal = vmax;
                            have = true;
                        } else {
                            minVal = std::min(minVal, vmin);
                            maxVal = std::max(maxVal, vmax);
                        }
                        
                        // Aggregate band energies from chunks if available
                        if (i < static_cast<int>(chunk.lowBins.size())) {
                            sumLow += std::max(0.0f, chunk.lowBins[i]);
                            ++count;
                        }
                        if (i < static_cast<int>(chunk.midBins.size())) {
                            sumMid += std::max(0.0f, chunk.midBins[i]);
                        }
                        if (i < static_cast<int>(chunk.highBins.size())) {
                            sumHigh += std::max(0.0f, chunk.highBins[i]);
                        }
                    }
                }
            }
        }
        
        // === LAYER 3: LEGACY STREAMING BINS (fallback if no chunks) ===
        // OPTIMIZED: Use oversampling with smooth interpolation for better zoom quality
        if (!have) {
            const int oversampleSteps = std::max(2, static_cast<int>(oversampleFactor));
            const double stepWidth = audioWidthPerPixel / oversampleSteps;
            
            for (int step = 0; step < oversampleSteps; ++step) {
                const double sampleSec = audioStart + step * stepWidth;
                if (sampleSec < 0.0 || sampleSec >= localAudioLength) continue;
                
                const double exactBinIndex = sampleSec * binPerSecond;
                const int binIdx = static_cast<int>(exactBinIndex);
                const int localIdx = binIdx - localAvailableStartBin;
                
                if (localIdx >= 0 && localIdx < totalLocalBins) {
                    // Smooth interpolated sampling
                    float vmin = sampleBinSmooth(localSourceMinBins, exactBinIndex - localAvailableStartBin);
                    float vmax = sampleBinSmooth(localSourceMaxBins, exactBinIndex - localAvailableStartBin);
                    
                    if (!have) {
                        minVal = vmin; maxVal = vmax; have = true;
                    } else {
                        minVal = std::min(minVal, vmin);
                        maxVal = std::max(maxVal, vmax);
                    }
                    
                    // Aggregate band energies with interpolation
                    if (!localSourceLowBins.empty()) {
                        sumLow += std::max(0.0f, sampleBinSmooth(localSourceLowBins, exactBinIndex - localAvailableStartBin));
                        ++count;
                    }
                    if (!localSourceMidBins.empty()) {
                        sumMid += std::max(0.0f, sampleBinSmooth(localSourceMidBins, exactBinIndex - localAvailableStartBin));
                    }
                    if (!localSourceHighBins.empty()) {
                        sumHigh += std::max(0.0f, sampleBinSmooth(localSourceHighBins, exactBinIndex - localAvailableStartBin));
                    }
                }
            }
        }
        
        // If still no data, use baseline
        if (!have && !haveFallback) {
            reuseOrBaseline(x, audioCenterSec, prevR, prevG, prevB);
            continue;
        }
        
        have = have || haveFallback;

        if (missingSegmentsOverlayEnabled) {
            if (currentMissingStart >= 0) {
                // Flush previous missing segment
                missingSegments.emplace_back(currentMissingStart, x);
                currentMissingStart = -1;
            }
        }

        // Normalize & clamp
        minVal *= chunkNormalizationFactor;
        maxVal *= chunkNormalizationFactor;
        
        const float peakLimit = 1.0f;
        minVal = std::max(-peakLimit, std::min(0.0f, minVal));
        maxVal = std::min(peakLimit, std::max(0.0f, maxVal));

        // FIX #1: KEIN Temporal Smoothing! Verursacht Lag-Gefühl!
        // Nutze History nur für Cache-Reuse, NICHT für Smoothing
        pixelUpperScratch[x] = maxVal; // Direct assignment
        pixelLowerScratch[x] = minVal;
        pixelCoverageScratch[x] = 1;

        // Compute per-pixel RGB with improved color balance
        if (count > 0) {
            const auto color = WaveformTheme::computeSpectrumColor(sumLow, sumMid, sumHigh, count);
            pixelColorScratch[x * 3 + 0] = color.r;
            pixelColorScratch[x * 3 + 1] = color.g;
            pixelColorScratch[x * 3 + 2] = color.b;
            prevR = color.r;
            prevG = color.g;
            prevB = color.b;
        } else if (haveFallback && !have) {
            const auto warn = WaveformTheme::fallbackWarningColor();
            pixelColorScratch[x * 3 + 0] = warn.r;
            pixelColorScratch[x * 3 + 1] = warn.g;
            pixelColorScratch[x * 3 + 2] = warn.b;
            prevR = warn.r;
            prevG = warn.g;
            prevB = warn.b;
        } else {
            // Reuse last valid color or use neutral
            pixelColorScratch[x * 3 + 0] = prevR;
            pixelColorScratch[x * 3 + 1] = prevG;
            pixelColorScratch[x * 3 + 2] = prevB;
        }

        storeHistory(x, audioCenterSec);
    }

    if (missingSegmentsOverlayEnabled) {
        if (currentMissingStart >= 0) {
            missingSegments.emplace_back(currentMissingStart, pixelWidth);
        }
    }

    const auto fallbackBase = WaveformTheme::fallbackColor();
    const float fallbackR = fallbackBase.r;
    const float fallbackG = fallbackBase.g;
    const float fallbackB = fallbackBase.b;

    auto copyColumn = [&](int dst, int src) {
        pixelUpperScratch[dst] = pixelUpperScratch[src];
        pixelLowerScratch[dst] = pixelLowerScratch[src];
        pixelColorScratch[dst * 3 + 0] = pixelColorScratch[src * 3 + 0];
        pixelColorScratch[dst * 3 + 1] = pixelColorScratch[src * 3 + 1];
        pixelColorScratch[dst * 3 + 2] = pixelColorScratch[src * 3 + 2];
        pixelCoverageScratch[dst] = 1;
    };

    int lastValid = -1;
    for (int x = 0; x < pixelWidth; ++x) {
        if (pixelCoverageScratch[x]) {
            lastValid = x;
            continue;
        }
        if (lastValid >= 0) {
            copyColumn(x, lastValid);
        }
    }

    int nextValid = -1;
    for (int x = pixelWidth - 1; x >= 0; --x) {
        if (pixelCoverageScratch[x]) {
            nextValid = x;
            continue;
        }
        if (nextValid >= 0) {
            copyColumn(x, nextValid);
        }
    }

    if (lastValid < 0 && nextValid < 0) {
        for (int x = 0; x < pixelWidth; ++x) {
            pixelUpperScratch[x] = 0.0f;
            pixelLowerScratch[x] = 0.0f;
            pixelColorScratch[x * 3 + 0] = fallbackR;
            pixelColorScratch[x * 3 + 1] = fallbackG;
            pixelColorScratch[x * 3 + 2] = fallbackB;
            pixelCoverageScratch[x] = 1;
        }
    } else if (lastValid < 0) {
        for (int x = 0; x < pixelWidth; ++x) {
            if (!pixelCoverageScratch[x] && nextValid >= 0) {
                copyColumn(x, nextValid);
            }
        }
    } else if (nextValid < 0) {
        for (int x = 0; x < pixelWidth; ++x) {
            if (!pixelCoverageScratch[x]) {
                copyColumn(x, lastValid);
            }
        }
    }

    for (int x = 0; x < pixelWidth; ++x) {
        if (!pixelCoverageScratch[x]) continue;
        
        // P1 BUG #4 FIX: screenX muss INTEGER sein (kein double)!
        // Runde Y-Werte VOR emplace_back um Sub-Pixel-Jitter zu eliminieren
        const int screenX = x; // Integer, nicht double(x)!
        double upperY = centerY - static_cast<double>(pixelUpperScratch[x]) * pixelHeight * waveformHeightScale;
        double lowerY = centerY - static_cast<double>(pixelLowerScratch[x]) * pixelHeight * waveformHeightScale;
        
        // P1 BUG #4 FIX: Runde Y-Werte zu ganzen Pixeln
        upperY = std::round(upperY);
        lowerY = std::round(lowerY);
        
        upperPoints.emplace_back(static_cast<double>(screenX), upperY);
        lowerPoints.emplace_back(static_cast<double>(screenX), lowerY);
    }

    if (upperPoints.size() < 2 || lowerPoints.size() < 2) {
        upperPoints.clear();
        lowerPoints.clear();
        upperPoints.emplace_back(0.0, centerY);
        upperPoints.emplace_back(static_cast<double>(pixelWidth), centerY);
        lowerPoints = upperPoints;
    }

    // missingSegments already built per-pixel above

    geometryCache.valid = true;
}
bool WaveformDisplay::geometryNeedsUpdate(int viewWidth, int viewHeight, double zoomFactor, double renderPlayhead) const
{
    // Priorisiert Flags: FullRedraw > Update > bestehende Gueltigkeit
    if (renderCache.needsFullRedraw) return true;   // harte Ungueltigkeit, sofort neu aufbauen
    if (renderCache.needsUpdate) return true;       // sanfte Ungueltigkeit, alte Geometrie bleibt bis Rebuild
    if (!renderCache.geometryValid) return true;    // noch keine brauchbare Geometrie vorhanden

    // Erst danach auf inhaltliche Aenderungen pruefen
    if (renderCache.lastWidth != viewWidth || renderCache.lastHeight != viewHeight) return true;
    if (std::abs(renderCache.lastZoomFactor - zoomFactor) >= 0.01) return true; // Less sensitive to zoom changes
    if (std::abs(renderCache.lastTempoFactor - tempoFactor) >= 0.001) return true;

    // Playhead threshold: rebuild only when visual motion exceeds ~0.5 pixel
    double basePps = 0.0;
    if (useFixedPixelsPerSecond) basePps = std::max(10.0, localPixelsPerSecond);
    else if (audioLength > 0.0) basePps = static_cast<double>(viewWidth) / std::max(audioLength, 1e-3);
    if (basePps <= 0.0) basePps = std::max(10.0, localPixelsPerSecond);
    const double pps = std::max(1.0, basePps * zoomFactor);
    // Dynamischer Threshold: nutze gemessene Rate ODER gefilterte Rate (was groesser ist)
    const double filteredVelocity = std::abs(std::isfinite(estimatedPlaybackRate) ? estimatedPlaybackRate : 0.0);

    // Direkt-Messung der aktuellen Bewegung (nicht gefiltert)
    double instantVelocity = 0.0;
    if (renderCache.geometryValid && std::isfinite(renderCache.lastPlayheadPos)) {
        const double deltaPos = std::abs(renderPlayhead - renderCache.lastPlayheadPos);
        const double totalLen = (audioLength > 0.0) ? audioLength : trackLengthSec;
        if (totalLen > 0.0) {
            const double deltaSec = deltaPos * totalLen;
            const auto now = std::chrono::steady_clock::now();
            const auto timeSinceLastUpdate = std::chrono::duration<double>(now - renderCache.lastUpdate).count();
            if (timeSinceLastUpdate > 1e-6 && timeSinceLastUpdate < 1.0) {  // Limit to 1 second max
                instantVelocity = std::min(deltaSec / timeSinceLastUpdate, 100.0);  // Cap at 100x speed
            }
        }
    }

    // Nutze das Maximum beider Messungen fuer aggressivere Updates
    const double playbackVelocity = std::max(filteredVelocity, instantVelocity);

    // Wenn beide Messungen sehr klein sind (z.B. nach Stop), nimm konservativen Threshold
    const double minDetectableVelocity = 1e-4;
    const bool hasMovement = playbackVelocity > minDetectableVelocity;

    // FIX #6: NOCH aggressivere Geometry Rebuild Frequenz - spart MASSIV CPU
    // 16px Playback, 24px Pause (war 12/16)
    const double pixelThreshold = hasMovement ? 16.0 : 24.0;
    const double secondsPerPixelThreshold = pixelThreshold / pps;

    const double totalLen = (audioLength > 0.0) ? audioLength : trackLengthSec;
    double thresholdRel = (totalLen > 0.0) ? (secondsPerPixelThreshold / totalLen) : 0.0001;

    // Nur im Seek-Mode enger
    if (isInSeekMode) thresholdRel *= 0.25;

    thresholdRel = std::clamp(thresholdRel, 1e-6, 1e-3);
    if (std::abs(renderCache.lastPlayheadPos - renderPlayhead) >= thresholdRel) return true;

    // BUG #17 FIX: Check if streaming window changed SIGNIFICANTLY (nicht bei jedem Chunk)
    // Verhindert Rebuild bei jedem kleinen Streaming-Update → massive Stutter-Reduktion
    if (streamingMode && streamingPreloadBins > 0) {
        const int binDeltaThreshold = streamingPreloadBins / 4; // 25% des Preload-Fensters
        const int startDelta = std::abs(renderCache.lastAvailableStartBin - availableStartBin);
        const int endDelta = std::abs(renderCache.lastAvailableEndBin - availableEndBin);
        
        if (startDelta > binDeltaThreshold || endDelta > binDeltaThreshold) {
            return true; // Nur bei großen Änderungen rebuilden
        }
    } else {
        // Non-Streaming: Jede Änderung ist signifikant
        if (renderCache.lastAvailableStartBin != availableStartBin ||
            renderCache.lastAvailableEndBin != availableEndBin) return true;
    }

    return false;
}

void WaveformDisplay::paintGL()
{
    if (!context() || !context()->isValid()) return;
    const int widgetWidth = width();
    const int widgetHeight = height();
    
    // Use framebuffer size for OpenGL viewport (handles high-DPI automatically)
    const qreal dpr = devicePixelRatioF();
    const int framebufferWidth = static_cast<int>(widgetWidth * dpr);
    const int framebufferHeight = static_cast<int>(widgetHeight * dpr);
    
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glClearColor(0.055f, 0.095f, 0.135f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (widgetWidth <= 0 || widgetHeight <= 0) {
        return;
    }

    ensureGlResources();

    const double zoomFactor = getBeatGridZoomFactor();
    const double renderPlayhead = acquireVisualPlayhead();
    activeRenderPlayhead = renderPlayhead;

    // Flag-Semantik:
    //  - needsUpdate: Geometrie sollte zeitnah erneuert werden, bestehende Daten bleiben nutzbar.
    //  - needsFullRedraw: Geometrie ist veraltet und darf nicht mehr verwendet werden.
    //  - geometryValid: aktuelle Geometrie/VBOs sind zeichnungsfaehig.

    if (geometryNeedsUpdate(widgetWidth, widgetHeight, zoomFactor, renderPlayhead)) {
        buildWaveformGeometry(widgetWidth, widgetHeight, zoomFactor, renderPlayhead);
        updateWaveformVertexBuffers(widgetWidth, widgetHeight);

        const bool geometryReady = geometryCache.valid && glResources.fillVertexCountFront >= 4;
        renderCache.geometryValid = geometryReady;
        renderCache.lastWidth = widgetWidth;
        renderCache.lastHeight = widgetHeight;
        renderCache.lastZoomFactor = zoomFactor;
        renderCache.lastTempoFactor = tempoFactor;
        renderCache.lastPlayheadPos = renderPlayhead;
        renderCache.lastAvailableStartBin = availableStartBin;
        renderCache.lastAvailableEndBin = availableEndBin;

        if (geometryReady) {
            renderCache.needsUpdate = false;
            renderCache.needsFullRedraw = false;
        } else {
            renderCache.needsFullRedraw = true;
            renderCache.needsUpdate = false;
        }
    }

    const bool haveBuffers = glResources.fillVertexCountFront >= 4;
    const bool readyToDraw = renderCache.geometryValid && haveBuffers;

    // Zeichnen nur mit gueltiger Geometrie, sonst Full-Redraw markieren
    if (readyToDraw) {
        drawWaveformGl();
    } else {
        renderCache.geometryValid = false;
        renderCache.needsFullRedraw = true;
    }

    renderCache.lastUpdate = std::chrono::steady_clock::now();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawWaveformOverlays(painter, widgetWidth, widgetHeight, zoomFactor);
}

bool WaveformDisplay::compileWaveformShaders()
{
    auto& fillProgram = glResources.fillProgram;
    auto& lineProgram = glResources.lineProgram;
    fillProgram.removeAllShaders();
    lineProgram.removeAllShaders();

    const char* fillVertexSrc = glResources.usingGles ? kWaveformFillVertexShaderEs : kWaveformFillVertexShaderCore;
    const char* fillFragmentSrc = glResources.usingGles ? kWaveformFillFragmentShaderEs : kWaveformFillFragmentShaderCore;
    const char* lineVertexSrc = glResources.usingGles ? kWaveformLineVertexShaderEs : kWaveformLineVertexShaderCore;
    const char* lineFragmentSrc = glResources.usingGles ? kWaveformLineFragmentShaderEs : kWaveformLineFragmentShaderCore;

    if (!fillProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, fillVertexSrc)) {
        qWarning() << "WaveformDisplay: failed to compile fill vertex shader" << fillProgram.log();
        return false;
    }
    if (!fillProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, fillFragmentSrc)) {
        qWarning() << "WaveformDisplay: failed to compile fill fragment shader" << fillProgram.log();
        return false;
    }
    if (!fillProgram.link()) {
        qWarning() << "WaveformDisplay: failed to link fill shader" << fillProgram.log();
        return false;
    }

    if (!lineProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, lineVertexSrc)) {
        qWarning() << "WaveformDisplay: failed to compile line vertex shader" << lineProgram.log();
        return false;
    }
    if (!lineProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, lineFragmentSrc)) {
        qWarning() << "WaveformDisplay: failed to compile line fragment shader" << lineProgram.log();
        return false;
    }
    if (!lineProgram.link()) {
        qWarning() << "WaveformDisplay: failed to link line shader" << lineProgram.log();
        return false;
    }

    return true;
}

void WaveformDisplay::ensureGlResources()
{
    if (QOpenGLContext* ctx = context()) {
        glResources.usingGles = ctx->isOpenGLES();
    } else if (QOpenGLContext* current = QOpenGLContext::currentContext()) {
        glResources.usingGles = current->isOpenGLES();
    }

    if (glResources.initialized) {
        if (!glResources.fillProgram.isLinked() || !glResources.lineProgram.isLinked()) {
            compileWaveformShaders();
        }
        return;
    }

    if (!compileWaveformShaders()) {
        return;
    }

    // Create VAO
    if (!glResources.vao.isCreated()) {
        if (!glResources.vao.create()) {
            qWarning() << "WaveformDisplay: Failed to create VAO";
        }
    }

    glGenBuffers(2, glResources.fillVbo);
    glGenBuffers(2, glResources.topLineVbo);
    glGenBuffers(2, glResources.bottomLineVbo);
    glResources.initialized = true;
}

void WaveformDisplay::destroyGlResources()
{
    if (!glResources.initialized &&
        !glResources.fillProgram.isLinked() &&
        !glResources.lineProgram.isLinked()) {
        return;
    }

    if (glResources.fillProgram.isLinked()) {
        glResources.fillProgram.removeAllShaders();
    }
    if (glResources.lineProgram.isLinked()) {
        glResources.lineProgram.removeAllShaders();
    }

    if (glResources.vao.isCreated()) {
        glResources.vao.destroy();
    }

    if (QOpenGLContext::currentContext()) {
        if (glResources.fillVbo[0] || glResources.fillVbo[1]) {
            glDeleteBuffers(2, glResources.fillVbo);
            glResources.fillVbo[0] = glResources.fillVbo[1] = 0;
        }
        if (glResources.topLineVbo[0] || glResources.topLineVbo[1]) {
            glDeleteBuffers(2, glResources.topLineVbo);
            glResources.topLineVbo[0] = glResources.topLineVbo[1] = 0;
        }
        if (glResources.bottomLineVbo[0] || glResources.bottomLineVbo[1]) {
            glDeleteBuffers(2, glResources.bottomLineVbo);
            glResources.bottomLineVbo[0] = glResources.bottomLineVbo[1] = 0;
        }
    }

    glResources.initialized = false;
    glResources.fillVertexCountFront = 0;
    glResources.topLineVertexCountFront = 0;
    glResources.bottomLineVertexCountFront = 0;
}

void WaveformDisplay::updateWaveformVertexBuffers(int viewWidth, int viewHeight)
{
    // Atomic update: keep previous GPU buffers/counts if we can't produce a new valid upload
    if (!glResources.initialized || viewWidth <= 0 || viewHeight <= 0) {
        return;
    }

    // VBOs are created in ensureGlResources; proceed to upload into the back buffer

    const size_t pointCount = std::min(upperPointBuffer.size(), lowerPointBuffer.size());
    if (pointCount < 2) {
        return;
    }

    fillVertexData.clear();
    topLineVertexData.clear();
    bottomLineVertexData.clear();

    // Each vertex: 2 floats position + 3 floats color
    fillVertexData.reserve(pointCount * 10);
    topLineVertexData.reserve(pointCount * 2);
    bottomLineVertexData.reserve(pointCount * 2);

    const double invWidth = viewWidth > 0 ? 1.0 / static_cast<double>(viewWidth) : 0.0;
    const double invHeight = viewHeight > 0 ? 1.0 / static_cast<double>(viewHeight) : 0.0;

    auto toNdc = [&](const QPointF& pt) -> std::pair<float, float> {
        // Round to pixel boundaries in screen space BEFORE converting to NDC
        // This prevents sub-pixel jiggling
        const double pixelX = std::round(pt.x());
        const double pixelY = std::round(pt.y());
        
        const double normX = pixelX * invWidth;
        const double normY = pixelY * invHeight;
        const float x = static_cast<float>(normX * 2.0 - 1.0);
        const float y = static_cast<float>(1.0 - normY * 2.0);
        return {x, y};
    };

    for (size_t i = 0; i < pointCount; ++i) {
        const auto upper = toNdc(upperPointBuffer[i]);
        const auto lower = toNdc(lowerPointBuffer[i]);
        // Determine color for this x (from pixelColorScratch)
        int xIndex = std::clamp(static_cast<int>(std::round(upperPointBuffer[i].x() - 0.5)), 0, viewWidth - 1);
    const auto fallbackRgb = WaveformTheme::fallbackColor();
    float r = fallbackRgb.r;
    float g = fallbackRgb.g;
    float b = fallbackRgb.b;
        if (static_cast<int>(pixelColorScratch.size()) >= (xIndex * 3 + 3)) {
            r = pixelColorScratch[xIndex * 3 + 0];
            g = pixelColorScratch[xIndex * 3 + 1];
            b = pixelColorScratch[xIndex * 3 + 2];
        }

        // Upper vertex (pos + color)
        fillVertexData.push_back(upper.first);
        fillVertexData.push_back(upper.second);
        fillVertexData.push_back(r);
        fillVertexData.push_back(g);
        fillVertexData.push_back(b);
        // Lower vertex (pos + color - same color per column)
        fillVertexData.push_back(lower.first);
        fillVertexData.push_back(lower.second);
        fillVertexData.push_back(r);
        fillVertexData.push_back(g);
        fillVertexData.push_back(b);

        topLineVertexData.push_back(upper.first);
        topLineVertexData.push_back(upper.second);
        bottomLineVertexData.push_back(lower.first);
        bottomLineVertexData.push_back(lower.second);
    }

    const int back = 1 - glResources.frontIndex;

    // BUG #21 FIX: Direkter Upload ohne redundantes Orphaning
    auto streamUpload = [this](GLuint vbo, const std::vector<float>& data) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const GLsizeiptr size = static_cast<GLsizeiptr>(data.size() * sizeof(float));
        // GL_STREAM_DRAW = optimal für jedes Frame neu geschriebene Daten
        if (size > 0) glBufferData(GL_ARRAY_BUFFER, size, data.data(), GL_STREAM_DRAW);
    };

    streamUpload(glResources.fillVbo[back],       fillVertexData);
    streamUpload(glResources.topLineVbo[back],    topLineVertexData);
    streamUpload(glResources.bottomLineVbo[back], bottomLineVertexData);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // FIX #7: GPU Upload ohne blocking glFlush() - Swap sofort!
    // GPU Upload passiert asynchron - kein CPU-Wait nötig
    // glFlush() würde hier blocken und Performance kosten

    // Publish counts atomically with front/back swap
    const int newFillCount   = static_cast<int>(fillVertexData.size() / 5);
    const int newTopCount    = static_cast<int>(topLineVertexData.size() / 2);
    const int newBottomCount = static_cast<int>(bottomLineVertexData.size() / 2);

    glResources.frontIndex = back;
    glResources.fillVertexCountFront    = newFillCount;
    glResources.topLineVertexCountFront = newTopCount;
    glResources.bottomLineVertexCountFront = newBottomCount;
}

void WaveformDisplay::drawWaveformGl()
{
    if (!glResources.initialized || glResources.fillVertexCountFront < 4) return;

    if (!glResources.vao.isCreated()) {
        if (!glResources.vao.create()) {
            qWarning() << "WaveformDisplay: Failed to create VAO";
            return;
        }
    }

    QOpenGLVertexArrayObject::Binder vaoBinder(&glResources.vao);

    const int front = glResources.frontIndex;
    glBindBuffer(GL_ARRAY_BUFFER, glResources.fillVbo[front]);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(float) * 5), reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(float) * 5), reinterpret_cast<const void*>(sizeof(float) * 2));
    glEnableVertexAttribArray(1);

    auto& fillProgram = glResources.fillProgram;
    fillProgram.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, glResources.fillVertexCountFront);
    fillProgram.release();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
}

void WaveformDisplay::drawMissingSegments(QPainter& painter, int viewWidth, int viewHeight) const
{
    // Safety: only draw if overlay is enabled and we actually have segments
    if (!missingSegmentsOverlayEnabled || missingSegmentBuffer.empty()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const auto& segment : missingSegmentBuffer) {
        const int segStart = std::clamp(segment.first, 0, viewWidth);
        const int segEnd = std::clamp(segment.second, segStart + 1, viewWidth);
        const int segWidth = std::max(1, segEnd - segStart);
        QRectF segRect(segStart, 0, segWidth, viewHeight);

    QLinearGradient loadingGradient(segRect.left(), 0, segRect.left(), viewHeight);
    loadingGradient.setColorAt(0.0, QColor(30, 55, 85, 160));
    loadingGradient.setColorAt(0.5, QColor(60, 110, 170, 110));
    loadingGradient.setColorAt(1.0, QColor(30, 55, 85, 160));
        painter.fillRect(segRect, loadingGradient);

        painter.setPen(QPen(QColor(130, 190, 255, 140), 1, Qt::DashLine));
        painter.drawLine(segStart, 0, segStart, viewHeight);
        painter.drawLine(segEnd - 1, 0, segEnd - 1, viewHeight);
    }
    painter.restore();
}

void WaveformDisplay::drawWaveformOverlays(QPainter& painter, int viewWidth, int viewHeight, double zoomFactor)
{
    if (geometryCache.valid && geometryCache.timeRange > 0.0) {
        if (streamingMode && !streamingComplete && missingSegmentsOverlayEnabled) {
            drawMissingSegments(painter, viewWidth, viewHeight);
        }

        if (useAnalyzedBeats) {
            drawBeatGrid(painter,
                         geometryCache.playheadSec,
                         geometryCache.displayCenterSec,
                         geometryCache.leftSecond,
                         geometryCache.rightSecond,
                         geometryCache.timeRange);
        }

        // Visible AUDIO window via centralized inverse mapping
    const double audioTimeLeftSec = mapDisplayToAudio(geometryCache.leftSecond);
    const double audioTimeRightSec = mapDisplayToAudio(geometryCache.rightSecond);
        
        const double audioTimeRange = audioTimeRightSec - audioTimeLeftSec;

        if (cuePointsValid && audioLength > 0.0) {
            drawCuePoints(painter, audioTimeLeftSec, audioTimeRightSec, audioTimeRange);
        }

        if (ghostLoopOverlayEnabled && ghostLoopEnabled && audioLength > 0.0 && ghostLoopEndSec > ghostLoopStartSec) {
            drawGhostLoopRegion(painter, geometryCache.leftSecond, geometryCache.rightSecond, geometryCache.timeRange);
        }

        // Preroll overlay (disabled by default). Keep behavior but stop drawing unless explicitly enabled.
        if (prerollOverlayEnabled && prerollEnabled && audioLength > 0.0) {
            const double showEps = std::max(0.0, prerollShowThresholdSec);
            const double hideEps = std::max(0.0, prerollHideThresholdSec);
            const double phAudioSec = geometryCache.playheadSec; // negative while in preroll

            const bool wantShow = (phAudioSec < -showEps) && (audioTimeLeftSec < -showEps);
            if (!prerollVisible) {
                if (wantShow) prerollVisible = true;
            } else {
                const bool mustHide = (phAudioSec >= -hideEps) || (audioTimeLeftSec >= -hideEps);
                if (mustHide) prerollVisible = false;
            }

            if (prerollVisible) {
                drawPrerollRegion(painter, geometryCache.leftSecond, geometryCache.rightSecond, geometryCache.timeRange);
            } else {
                lastPrerollEdgeX = -1; // reset cached edge when hidden
            }
        } else {
            // Ensure no residual state causes artifacts when overlay is disabled
            prerollVisible = false;
            lastPrerollEdgeX = -1;
        }

        if (loopOverlayEnabled && loopEnabled && loopEndSec > loopStartSec && audioLength > 0.0) {
            drawLoopRegion(painter, geometryCache.leftSecond, geometryCache.rightSecond, geometryCache.timeRange);
        }
    }

    const int centerX = viewWidth / 2;
    painter.setPen(QPen(QColor(255, 100, 100), 2));
    painter.drawLine(centerX, 0, centerX, viewHeight);

    if (zoomFactor != 1.0) {
        painter.setPen(QPen(QColor(150, 180, 220), 1));
        painter.setFont(QFont("Arial", 8));
        painter.drawText(8, viewHeight - 15, QString("%1x").arg(zoomFactor, 0, 'f', 1));
    }
}

void WaveformDisplay::loadAndRenderWaveform()
{
    if (currentFilePath.isEmpty()) return;
    updateRenderActivity();
    update();
}

void WaveformDisplay::loadFile(const QString& path)
{
    currentFilePath = path;
    audioStartOffset = 0.0;
    resetStreamingState();
    sourceMaxBins.clear();
    sourceMinBins.clear();
    analysisActive = false;
    analysisFailed = false;
    analysisProgress = 0.0;
    QTimer::singleShot(10, this, &WaveformDisplay::loadAndRenderWaveform);
}

void WaveformDisplay::markDirtyAndSchedule() {
    renderCache.needsUpdate = true;  // Deferred-Update: alte Geometrie bleibt sichtbar bis Ersatz bereitsteht
    renderCache.needsFullRedraw = false;
    // Don't invalidate chunk cache here - too aggressive, causes flickering
    // invalidateChunkCache();
    updateRenderActivity();
    update();
}

void WaveformDisplay::postSourceConfigured() {
    if (playheadPos < 0.0) playheadPos = 0.0;
    resetVisualPlayhead(playheadPos);
    useAnalyzedBeats = false;
    beatPositions.clear();
    markDirtyAndSchedule();
}

void WaveformDisplay::setSourceBins(const std::vector<float>& maxBins,
                                    const std::vector<float>& minBins,
                                    double audioStartOffsetSec,
                                    double lengthSeconds)
{
    if (maxBins.size() != minBins.size()) {
        qWarning() << "WaveformDisplay::setSourceBins - size mismatch!";
        return;
    }

    std::unique_lock<std::shared_mutex> lock(sourceMutex);
    resetStreamingState();

    sourceMaxBins = maxBins;
    sourceMinBins = minBins;
    sourceLowBins.clear();
    sourceMidBins.clear();
    sourceHighBins.clear();
    sourceWidth = static_cast<int>(maxBins.size());
    streamingTotalBins = sourceWidth;
    availableStartBin = 0;
    availableEndBin = sourceWidth;
    streamingMode = false;
    streamingComplete = true;
    streamingExpectedNextBin = availableEndBin;

    audioStartOffset = audioStartOffsetSec;
    audioLength = lengthSeconds;
    trackLengthSec = lengthSeconds;

    postSourceConfigured();
}

void WaveformDisplay::beginStreaming(int totalBinCount,
                                     double audioStartOffsetSec,
                                     double lengthSeconds,
                                     int preloadBins,
                                     int maxCachedBins)
{
    std::unique_lock<std::shared_mutex> lock(sourceMutex);
    resetStreamingState();

    streamingMode = true;
    streamingTotalBins = totalBinCount;
    sourceWidth = totalBinCount;
    streamingPreloadBins = std::max(0, preloadBins);
    streamingMaxCacheBins = std::max(streamingPreloadBins * 2, std::max(0, maxCachedBins));
    availableStartBin = 0;
    availableEndBin = 0;
    streamingExpectedNextBin = 0;

    audioStartOffset = audioStartOffsetSec;
    audioLength = lengthSeconds;
    trackLengthSec = lengthSeconds;

    sourceMaxBins.clear();
    sourceMinBins.clear();
    sourceLowBins.clear();
    sourceMidBins.clear();
    sourceHighBins.clear();
    
    // === CREATE LOW-RES FALLBACK WAVEFORM (simple red baseline) ===
    {
        std::lock_guard<std::mutex> fbLock(fallbackMutex_);
        const int fallbackBins = std::min(2000, totalBinCount); // Max 2000 bins for full song
        fallbackMaxBins_.assign(fallbackBins, 0.3f); // Simple constant amplitude
        fallbackMinBins_.assign(fallbackBins, -0.3f);
        fallbackComplete_ = true;
        qDebug() << "[Fallback] Created low-res waveform with" << fallbackBins << "bins";
    }

    postSourceConfigured();
}

void WaveformDisplay::appendStreamBinsImpl(std::unique_lock<std::shared_mutex>& lock,
                                           int startBin,
                                           std::span<const float> maxBins,
                                           std::span<const float> minBins,
                                           std::span<const float> lowBins,
                                           std::span<const float> midBins,
                                           std::span<const float> highBins,
                                           bool isFinalChunk)
{
    Q_UNUSED(lock);

    if (startBin < 0) {
#ifdef WAVEFORM_DEBUG
        qDebug() << "[WaveformDisplay] Verwerfe Chunk mit negativem Start" << startBin;
#endif
        qWarning() << "WaveformDisplay::appendStreamBins - negative startBin" << startBin;
        return;
    }

    const auto chunkSize = static_cast<int>(maxBins.size());
    if (chunkSize <= 0) {
#ifdef WAVEFORM_DEBUG
        qDebug() << "[WaveformDisplay] Verwerfe Chunk ohne Samples" << startBin;
#endif
        qWarning() << "WaveformDisplay::appendStreamBins - empty chunk";
        return;
    }
    if (minBins.size() != maxBins.size()) {
        qWarning() << "WaveformDisplay::appendStreamBins - invalid data sizes";
        return;
    }
    if (!lowBins.empty() && (lowBins.size() != maxBins.size() || midBins.size() != maxBins.size() || highBins.size() != maxBins.size())) {
        qWarning() << "WaveformDisplay::appendStreamBins - band size mismatch";
        return;
    }

    const int chunkEnd = startBin + chunkSize;
    if (chunkEnd <= startBin) {
#ifdef WAVEFORM_DEBUG
        qDebug() << "[WaveformDisplay] Verwerfe Chunk mit leerem Bereich" << startBin << chunkEnd;
#endif
        qWarning() << "WaveformDisplay::appendStreamBins - invalid bin range" << startBin << chunkEnd;
        return;
    }
    if (streamingTotalBins > 0 && (startBin >= streamingTotalBins || chunkEnd > streamingTotalBins)) {
#ifdef WAVEFORM_DEBUG
    qDebug() << "[WaveformDisplay] Chunk ausserhalb des erwarteten Fensters" << startBin << chunkEnd << streamingTotalBins;
#endif
        qWarning() << "WaveformDisplay::appendStreamBins - chunk outside expected range" << startBin << chunkEnd;
        return;
    }
    const bool hasColor = !lowBins.empty();
    const bool maintainBands = hasColor || !sourceLowBins.empty() || !sourceMidBins.empty() || !sourceHighBins.empty();
    const int prevStart = availableStartBin;
    const int prevEnd = availableEndBin;
    const bool hadData = !sourceMaxBins.empty();

    auto insertFrontZeros = [](auto& vec, int count) {
        if (count > 0) vec.insert(vec.begin(), count, 0.0f);
    };
    auto insertBackZeros = [](auto& vec, int count) {
        if (count > 0) vec.insert(vec.end(), count, 0.0f);
    };
    auto trimFront = [](auto& vec, int count) {
        if (count <= 0 || vec.empty()) return;
        count = std::min(count, static_cast<int>(vec.size()));
        vec.erase(vec.begin(), vec.begin() + count);
    };
    auto trimBack = [](auto& vec, int count) {
        if (count <= 0 || vec.empty()) return;
        count = std::min(count, static_cast<int>(vec.size()));
        vec.erase(vec.end() - count, vec.end());
    };
    auto copyInto = [](auto& dest, std::span<const float> src, int offset) {
        if (src.empty() || dest.empty() || offset < 0) return;
        const auto required = offset + static_cast<int>(src.size());
        if (required > static_cast<int>(dest.size())) return;
        std::copy(src.begin(), src.end(), dest.begin() + offset);
    };
    auto zeroRange = [](auto& dest, int offset, int count) {
        if (count <= 0 || dest.empty() || offset < 0) return;
        const auto required = offset + count;
        if (required > static_cast<int>(dest.size())) return;
        std::fill_n(dest.begin() + offset, count, 0.0f);
    };
    auto ensureBandSize = [&](std::size_t targetSize) {
        if (!maintainBands) return;
        if (sourceLowBins.size() != targetSize) sourceLowBins.resize(targetSize, 0.0f);
        if (sourceMidBins.size() != targetSize) sourceMidBins.resize(targetSize, 0.0f);
        if (sourceHighBins.size() != targetSize) sourceHighBins.resize(targetSize, 0.0f);
    };

    if (!hadData) {
        sourceMaxBins.assign(maxBins.begin(), maxBins.end());
        sourceMinBins.assign(minBins.begin(), minBins.end());
        availableStartBin = startBin;
        availableEndBin = chunkEnd;

        if (maintainBands) {
            const auto targetSize = sourceMaxBins.size();
            if (hasColor) {
                sourceLowBins.assign(lowBins.begin(), lowBins.end());
                sourceMidBins.assign(midBins.begin(), midBins.end());
                sourceHighBins.assign(highBins.begin(), highBins.end());
            } else {
                sourceLowBins.assign(targetSize, 0.0f);
                sourceMidBins.assign(targetSize, 0.0f);
                sourceHighBins.assign(targetSize, 0.0f);
            }
        } else {
            sourceLowBins.clear();
            sourceMidBins.clear();
            sourceHighBins.clear();
        }
    } else {
        const int newStart = std::min(availableStartBin, startBin);
        const int newEnd = std::max(availableEndBin, chunkEnd);

        if (newStart < availableStartBin) {
            const int prepend = availableStartBin - newStart;
            insertFrontZeros(sourceMaxBins, prepend);
            insertFrontZeros(sourceMinBins, prepend);
            if (maintainBands) {
                insertFrontZeros(sourceLowBins, prepend);
                insertFrontZeros(sourceMidBins, prepend);
                insertFrontZeros(sourceHighBins, prepend);
            }
            availableStartBin = newStart;
        }

        if (newEnd > availableEndBin) {
            const int append = newEnd - availableEndBin;
            insertBackZeros(sourceMaxBins, append);
            insertBackZeros(sourceMinBins, append);
            if (maintainBands) {
                insertBackZeros(sourceLowBins, append);
                insertBackZeros(sourceMidBins, append);
                insertBackZeros(sourceHighBins, append);
            }
            availableEndBin = newEnd;
        }

        ensureBandSize(sourceMaxBins.size());

        const int offset = startBin - availableStartBin;
        copyInto(sourceMaxBins, maxBins, offset);
        copyInto(sourceMinBins, minBins, offset);
        if (hasColor) {
            copyInto(sourceLowBins, lowBins, offset);
            copyInto(sourceMidBins, midBins, offset);
            copyInto(sourceHighBins, highBins, offset);
        } else if (maintainBands) {
            zeroRange(sourceLowBins, offset, chunkSize);
            zeroRange(sourceMidBins, offset, chunkSize);
            zeroRange(sourceHighBins, offset, chunkSize);
        }
    }

    int cachedBins = availableEndBin - availableStartBin;
    
    // DISABLE trimming when adaptive chunks are active - chunks manage memory instead
    if (!useAdaptiveChunking_ && streamingMaxCacheBins > 0 && cachedBins > streamingMaxCacheBins) {
        int trimNeeded = std::min(cachedBins, cachedBins - streamingMaxCacheBins);
        if (trimNeeded > 0) {
            const bool extendedForward = chunkEnd > prevEnd;
            const bool extendedBackward = startBin < prevStart;
            int trimFrontCount = 0;
            int trimBackCount = 0;

            if (extendedForward && !extendedBackward) {
                trimFrontCount = trimNeeded;
            } else if (extendedBackward && !extendedForward) {
                trimBackCount = trimNeeded;
            } else {
                trimFrontCount = trimNeeded / 2;
                trimBackCount = trimNeeded - trimFrontCount;
            }

            trimFrontCount = std::min(trimFrontCount, cachedBins);
            if (trimFrontCount > 0) {
                trimFront(sourceMaxBins, trimFrontCount);
                trimFront(sourceMinBins, trimFrontCount);
                if (maintainBands) {
                    trimFront(sourceLowBins, trimFrontCount);
                    trimFront(sourceMidBins, trimFrontCount);
                    trimFront(sourceHighBins, trimFrontCount);
                }
                availableStartBin += trimFrontCount;
                cachedBins = availableEndBin - availableStartBin;
            }

            trimBackCount = std::min(trimBackCount, cachedBins);
            if (trimBackCount > 0) {
                trimBack(sourceMaxBins, trimBackCount);
                trimBack(sourceMinBins, trimBackCount);
                if (maintainBands) {
                    trimBack(sourceLowBins, trimBackCount);
                    trimBack(sourceMidBins, trimBackCount);
                    trimBack(sourceHighBins, trimBackCount);
                }
                availableEndBin -= trimBackCount;
            }
        }
    }

    streamingExpectedNextBin = availableEndBin;
    streamingComplete = streamingComplete || isFinalChunk;

    // === ADAPTIVE CHUNK SYSTEM ===
    if (useAdaptiveChunking_ && !maxBins.empty()) {
        std::lock_guard<std::mutex> lock(adaptiveChunksMutex_);
        
        // Determine chunk priority based on distance to playhead
        double playheadSec = playheadPos * audioLength;
        double binPerSec = streamingTotalBins > 0 ? (double)streamingTotalBins / audioLength : 1.0;
        double chunkCenterSec = ((startBin + chunkEnd) / 2.0) / binPerSec;
        double distance = std::abs(chunkCenterSec - playheadSec);
        
        // BUG #20 FIX: Preroll-aware priority (verhindert dass Chunks weit voraus ULTRA bekommen)
        // Signed distance: negativ = behind playhead, positiv = ahead
        double signedDistance = chunkCenterSec - playheadSec;
        const double maxPrerollSec = 5.0; // Max 5s vorausschauend als ULTRA
        
        int priority = 3; // LOW
        if (distance <= 2.0 && signedDistance <= maxPrerollSec) {
            priority = 0; // ULTRA (±2s, aber max 5s voraus)
        } else if (distance <= 10.0) {
            priority = 1; // HIGH (±10s)
        } else if (distance <= 30.0) {
            priority = 2; // MEDIUM (±30s)
        }
        
        // Check if chunk already exists
        bool exists = false;
        for (auto& chunk : adaptiveChunks_) {
            if (chunk.startBin == startBin && chunk.endBin == chunkEnd) {
                // Update existing chunk
                chunk.maxBins.assign(maxBins.begin(), maxBins.end());
                chunk.minBins.assign(minBins.begin(), minBins.end());
                if (!lowBins.empty()) chunk.lowBins.assign(lowBins.begin(), lowBins.end());
                if (!midBins.empty()) chunk.midBins.assign(midBins.begin(), midBins.end());
                if (!highBins.empty()) chunk.highBins.assign(highBins.begin(), highBins.end());
                chunk.lastAccessTime = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                chunk.priority = priority;
                exists = true;
                break;
            }
        }
        
        if (!exists) {
            AdaptiveChunk newChunk;
            newChunk.startBin = startBin;
            newChunk.endBin = chunkEnd;
            newChunk.maxBins.assign(maxBins.begin(), maxBins.end());
            newChunk.minBins.assign(minBins.begin(), minBins.end());
            if (!lowBins.empty()) newChunk.lowBins.assign(lowBins.begin(), lowBins.end());
            if (!midBins.empty()) newChunk.midBins.assign(midBins.begin(), midBins.end());
            if (!highBins.empty()) newChunk.highBins.assign(highBins.begin(), highBins.end());
            newChunk.priority = priority;
            newChunk.lastAccessTime = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            adaptiveChunks_.push_back(newChunk);
            
            // Smart eviction: only if we exceed limit significantly
            if (adaptiveChunks_.size() > (size_t)maxAdaptiveChunks_) {
                // Sort by priority (LOW first), then by distance from playhead
                std::sort(adaptiveChunks_.begin(), adaptiveChunks_.end(),
                    [playheadSec, binPerSec](const AdaptiveChunk& a, const AdaptiveChunk& b) {
                        // First sort by priority (higher priority value = lower importance = remove first)
                        if (a.priority != b.priority) return a.priority > b.priority;
                        
                        // Same priority: sort by distance (farthest first)
                        double aCenterSec = ((a.startBin + a.endBin) / 2.0) / binPerSec;
                        double bCenterSec = ((b.startBin + b.endBin) / 2.0) / binPerSec;
                        double aDist = std::abs(aCenterSec - playheadSec);
                        double bDist = std::abs(bCenterSec - playheadSec);
                        return aDist > bDist;
                    });
                
                // Only remove LOW priority chunks that are far away
                int toRemove = std::min(50, (int)(adaptiveChunks_.size() - maxAdaptiveChunks_));
                adaptiveChunks_.erase(adaptiveChunks_.begin(), adaptiveChunks_.begin() + toRemove);
                
                qDebug() << "[AdaptiveChunk] Evicted" << toRemove 
                         << "low-priority chunks, keeping" << adaptiveChunks_.size();
            } else {
                qDebug() << "[AdaptiveChunk] Added chunk [" << startBin << "-" << chunkEnd 
                         << "] Priority:" << priority << "Size:" << newChunk.maxBins.size() 
                         << "Total:" << adaptiveChunks_.size() << "/" << maxAdaptiveChunks_;
            }
        }
    }

    if (hasPendingRegionRequest) {
        const int requestSpan = pendingRequestEndBin - pendingRequestStartBin;
        const int overlapStart = std::max(pendingRequestStartBin, availableStartBin);
        const int overlapEnd = std::min(pendingRequestEndBin, availableEndBin);
        const int overlap = std::max(0, overlapEnd - overlapStart);
        const bool requestCovered = (pendingRequestStartBin >= availableStartBin) && (pendingRequestEndBin <= availableEndBin);
        const bool spanValid = requestSpan > 0;
        const bool overlapSatisfies = spanValid && (overlap * 5 >= requestSpan * 4); // >=80%

        if (!spanValid || overlapSatisfies || requestCovered) {
#ifdef WAVEFORM_DEBUG
            qDebug() << "[WaveformDisplay] Streaming-Request erfuellt" << pendingRequestStartBin << pendingRequestEndBin
                     << "Overlap" << overlap << "Span" << requestSpan << "Covered" << requestCovered;
#endif
            hasPendingRegionRequest = false;
            pendingRequestStartBin = 0;
            pendingRequestEndBin = 0;
            lastRegionRequestTime = {};
        }
    }

    markDirtyAndSchedule();
}

void WaveformDisplay::appendStreamBins(int startBin,
                                       const std::vector<float>& maxBins,
                                       const std::vector<float>& minBins,
                                       bool isFinalChunk)
{
    if (maxBins.size() != minBins.size() || maxBins.empty()) {
        qWarning() << "WaveformDisplay::appendStreamBins - invalid data sizes";
        return;
    }

    if (!streamingMode) {
        setSourceBins(maxBins, minBins, audioStartOffset, audioLength);
        streamingComplete = isFinalChunk;
        return;
    }

    std::unique_lock<std::shared_mutex> lock(sourceMutex);
    const std::span<const float> maxSpan{maxBins};
    const std::span<const float> minSpan{minBins};
    appendStreamBinsImpl(lock,
                         startBin,
                         maxSpan,
                         minSpan,
                         {},
                         {},
                         {},
                         isFinalChunk);
}

void WaveformDisplay::appendStreamBins(int startBin,
                                       const std::vector<float>& maxBins,
                                       const std::vector<float>& minBins,
                                       const std::vector<float>& lowBins,
                                       const std::vector<float>& midBins,
                                       const std::vector<float>& highBins,
                                       bool isFinalChunk)
{
    if (maxBins.size() != minBins.size() || maxBins.empty()) {
        qWarning() << "WaveformDisplay::appendStreamBins - invalid data sizes";
        return;
    }
    if (maxBins.size() != lowBins.size() || maxBins.size() != midBins.size() || maxBins.size() != highBins.size()) {
        appendStreamBins(startBin, maxBins, minBins, isFinalChunk);
        return;
    }

    if (!streamingMode) {
        setSourceBins(maxBins, minBins, audioStartOffset, audioLength);
        streamingComplete = isFinalChunk;
        return;
    }

    std::unique_lock<std::shared_mutex> lock(sourceMutex);
    const std::span<const float> maxSpan{maxBins};
    const std::span<const float> minSpan{minBins};
    const std::span<const float> lowSpan{lowBins};
    const std::span<const float> midSpan{midBins};
    const std::span<const float> highSpan{highBins};
    appendStreamBinsImpl(lock,
                         startBin,
                         maxSpan,
                         minSpan,
                         lowSpan,
                         midSpan,
                         highSpan,
                         isFinalChunk);
}

std::pair<int, int> WaveformDisplay::getCachedBinRange() const
{
    if (!streamingMode) {
        return {0, sourceWidth};
    }
    return {availableStartBin, availableEndBin};
}

void WaveformDisplay::setOriginalBpm(double bpm, double trackLengthSeconds)
{
    originalBpm = bpm;
    trackLengthSec = trackLengthSeconds;
    update();
}

void WaveformDisplay::setPlayhead(double relative)
{
    if (!std::isfinite(relative)) return;
    
    // BUG #23 FIX: Seek-Threshold auf 15% erhöht (weniger empfindlich)
    // Verhindert dass kleine Playhead-Sprünge als Seek behandelt werden
    const double seekThreshold = 0.15; // War 0.05
    double positionDelta = std::abs(relative - playheadPos);
    const auto now = std::chrono::steady_clock::now();
    bool treatedAsSeek = false;
    
    if (positionDelta > seekThreshold && audioLength > 0.0) {
        isInSeekMode = true;
        lastSeekPosition = relative;
        lastSeekTime = now;
        treatedAsSeek = true;
        
        if (streamingMode && sourceWidth > 0 && audioLength > 0.0) {
            const double binPerSecond = static_cast<double>(sourceWidth) / audioLength;
            const double playheadSeconds = relative * audioLength;
            const int playheadBin = static_cast<int>(playheadSeconds * binPerSecond);
            
            qDebug() << "[SEEK] Playhead:" << relative << "AudioLen:" << audioLength 
                     << "Bin:" << playheadBin << "/" << sourceWidth;
            
            const int windowSize = streamingPreloadBins * 10;
            const int startBin = std::max(0, playheadBin - windowSize);
            const int endBin = std::min(sourceWidth, playheadBin + windowSize);

            hasPendingRegionRequest = false;
            streamingExpectedNextBin = startBin;
            
            renderCache.needsFullRedraw = true;
            renderCache.geometryValid = false;
            geometryCache.valid = false;
            
            const double startSec = audioStartOffset + (static_cast<double>(startBin) / binPerSecond);
            const double endSec = audioStartOffset + (static_cast<double>(endBin) / binPerSecond);
            
            qDebug() << "[SEEK] Emitting waveformRegionNeeded:" << startSec << "-" << endSec 
                     << "Bins:" << startBin << "-" << endBin;
            emit waveformRegionNeeded(startSec, endSec);
            
            const int microSize = 8192;
            const int microStart = std::clamp(playheadBin - microSize / 2, 0, std::max(0, sourceWidth - microSize));
            requestStreamingWindowIfNeeded(microStart, microStart + microSize, binPerSecond);
            requestStreamingWindowIfNeeded(startBin, endBin, binPerSecond);
            update();
        }
        
        renderCache.needsFullRedraw = true;
        renderCache.geometryValid = false;
    } else {
        auto timeSinceSeek = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeekTime).count();
        if (isInSeekMode && timeSinceSeek > 500) {
            isInSeekMode = false;
        }
    }
    
    double adjustedRelative = relative;
    const double minVisualRel = prerollEnabled ? -1.2 : -0.1;
    const double maxVisualRel = 1.1;

    if (treatedAsSeek || !visualPlayheadInitialized) {
        adjustedRelative = std::clamp(adjustedRelative, minVisualRel, maxVisualRel);
        playheadPos = adjustedRelative;
        resetVisualPlayhead(adjustedRelative);
    } else {
        double dt = std::chrono::duration<double>(now - lastPlayheadUpdateTime).count();
        if (!std::isfinite(dt) || dt <= 0.0) dt = 0.0;

        if (dt > 1e-5 && std::isfinite(lastReportedPlayhead)) {
            double rawRate = (relative - lastReportedPlayhead) / dt;
            const double maxRate = 12.0;
            if (std::isfinite(rawRate)) {
                rawRate = std::clamp(rawRate, -maxRate, maxRate);
                const double rateBlend = std::clamp(dt * 8.0, 0.0, 1.0);

                const double minMeaningfulRate = 1e-6;
                if (std::isfinite(estimatedPlaybackRate) && std::abs(estimatedPlaybackRate) > minMeaningfulRate) {
                    estimatedPlaybackRate += (rawRate - estimatedPlaybackRate) * rateBlend;
                } else {
                    estimatedPlaybackRate = rawRate;
                }

                if (std::abs(rawRate) < minMeaningfulRate && std::abs(estimatedPlaybackRate) > minMeaningfulRate) {
                    estimatedPlaybackRate *= 0.95;
                }
            }
        }

        const double transportRate = std::isfinite(estimatedPlaybackRate) ? estimatedPlaybackRate : 0.0;
        const double directionalJitter = 0.00012;
        if (std::isfinite(lastReportedPlayhead) && dt > 0.0) {
            if (transportRate > 0.02 && relative + directionalJitter < lastReportedPlayhead) {
                adjustedRelative = lastReportedPlayhead - directionalJitter;
            } else if (transportRate < -0.02 && relative - directionalJitter > lastReportedPlayhead) {
                adjustedRelative = lastReportedPlayhead + directionalJitter;
            }
        }

        adjustedRelative = std::clamp(adjustedRelative, minVisualRel, maxVisualRel);
        playheadPos = adjustedRelative;
        targetPlayheadPos = adjustedRelative;
        lastPlayheadUpdateTime = now;
        lastReportedPlayhead = relative;
    }
    
    updateRenderActivity();
    update();
}

void WaveformDisplay::resetVisualPlayhead(double relative) {
    double safeRelative = std::isfinite(relative) ? relative : 0.0;
    const auto now = std::chrono::steady_clock::now();
    visualPlayheadPos = safeRelative;
    targetPlayheadPos = safeRelative;
    lastReportedPlayhead = safeRelative;
    // NICHT auf 0 setzen - verhindert korrekten Start
    // estimatedPlaybackRate wird von setPlayhead() neu initialisiert
    if (!std::isfinite(estimatedPlaybackRate)) {
        estimatedPlaybackRate = 0.0;
    }
    lastPlayheadUpdateTime = now;
    lastVisualUpdateTime = now;
    activeRenderPlayhead = safeRelative;
    visualPlayheadInitialized = true;
    updateRenderActivity();
}

void WaveformDisplay::updateRenderActivity() {
    if (!renderTimer) {
        return;
    }

    const double velocity = std::abs(std::isfinite(estimatedPlaybackRate) ? estimatedPlaybackRate : 0.0);
    const bool needsContinuous = (scrollMode && velocity > 1e-3) ||
                                 isInSeekMode ||
                                 (streamingMode && !streamingComplete) ||
                                 analysisActive;

    if (needsContinuous) {
        if (!renderTimer->isActive()) {
            renderTimer->start();
        }
    } else if (renderTimer->isActive()) {
        renderTimer->stop();
    }
}

void WaveformDisplay::invalidateChunkCache() {
    chunkCacheDirty = true;
}

void WaveformDisplay::rebuildChunkCacheIfNeeded() {
    std::shared_lock<std::shared_mutex> sharedLock(sourceMutex);
    if (!chunkCacheDirty) {
        return;
    }

    sharedLock.unlock();
    std::unique_lock<std::shared_mutex> exclusiveLock(sourceMutex);
    rebuildChunkCacheIfNeeded_Locked(exclusiveLock);
}

void WaveformDisplay::rebuildChunkCacheIfNeeded_Locked(std::unique_lock<std::shared_mutex>& lock) {
    if (!lock.owns_lock()) {
        return;
    }
    if (!chunkCacheDirty) {
        return;
    }

    chunkCacheDirty = false;
    chunkCache.clear();
    chunkNormalizationFactor = 1.0f;
    secondsPerBin = 0.0;

    if (sourceMaxBins.empty() || sourceMinBins.empty() || audioLength <= 0.0) {
        return;
    }

    const int totalBins = static_cast<int>(sourceMaxBins.size());
    if (totalBins <= 0) {
        return;
    }

    int referenceWidth = sourceWidth > 0 ? sourceWidth : totalBins;
    if (streamingMode && referenceWidth <= 0 && streamingTotalBins > 0) {
        referenceWidth = streamingTotalBins;
    }

    if (referenceWidth <= 0) {
        return;
    }

    const double binPerSecond = static_cast<double>(referenceWidth) / std::max(audioLength, 1e-6);
    if (binPerSecond <= 0.0) {
        return;
    }

    secondsPerBin = 1.0 / binPerSecond;

#ifdef WAVEFORM_DEBUG
    qDebug() << "[WaveformDisplay] Chunk-Cache wird neu aufgebaut" << totalBins;
#endif

    // Robust normalization: use a high percentile of per-bin peaks across the available data
    // to avoid inflating very quiet parts. Never amplify above 1.0.
    {
        const int stride = std::max(1, totalBins / 1000);
        std::vector<float> sampledPeaks;
        sampledPeaks.reserve(std::min(totalBins / stride + 1, 2000));
        float maxPeak = 0.0f;
        for (int i = 0; i < totalBins; i += stride) {
            const float peak = std::max(std::abs(sourceMinBins[i]), std::abs(sourceMaxBins[i]));
            sampledPeaks.push_back(peak);
            if (peak > maxPeak) maxPeak = peak;
        }

        float robustPeak = maxPeak;
        if (sampledPeaks.size() >= 16) {
            // 98th percentile via nth_element (approximate robust peak)
            size_t idx = static_cast<size_t>(std::floor(0.98 * (sampledPeaks.size() - 1)));
            std::nth_element(sampledPeaks.begin(), sampledPeaks.begin() + idx, sampledPeaks.end());
            robustPeak = std::max(0.0f, sampledPeaks[idx]);
        }

        // Aim for ~90% of vertical space at robust peak, but do not amplify
        const float targetPeak = 0.9f;
        if (robustPeak > 1e-6f) {
            float factor = targetPeak / robustPeak;
            // Never amplify; keep within a sensible attenuation range
            factor = std::clamp(factor, 0.3f, 1.0f);
            chunkNormalizationFactor = factor;
        } else {
            // No meaningful signal yet; avoid amplification
            chunkNormalizationFactor = 1.0f;
        }
    }

    const int samplesPerChunk = chunkSampleResolution;

    for (int base = 0; base < totalBins; base += chunkBinSize) {
        const int chunkBins = std::min(chunkBinSize, totalBins - base);
        if (chunkBins <= 0) {
            continue;
        }

        WaveformChunk chunk;
        chunk.startBin = availableStartBin + base;
        chunk.endBin = chunk.startBin + chunkBins;
        // Track-time (seconds) for the first sample in this chunk in pure TRACK timeline (0 = track start)
        // Do NOT add audioStartOffset so that track start maps to display center when playheadSec==0
        chunk.startTimeSec = static_cast<double>(chunk.startBin) * secondsPerBin;

        const int sampleCountPerChunk = std::max(1, std::min(chunkBins, samplesPerChunk));
        chunk.upper.resize(sampleCountPerChunk);
        chunk.lower.resize(sampleCountPerChunk);

        const double chunkDuration = chunkBins * secondsPerBin;
        chunk.sampleDurationSec = (sampleCountPerChunk > 1)
            ? chunkDuration / static_cast<double>(sampleCountPerChunk - 1)
            : 0.0;

        for (int s = 0; s < sampleCountPerChunk; ++s) {
            const double relStart = (static_cast<double>(s) / sampleCountPerChunk) * chunkBins;
            const double relEnd = (static_cast<double>(s + 1) / sampleCountPerChunk) * chunkBins;
            int binStart = base + static_cast<int>(std::floor(relStart));
            int binEnd = base + static_cast<int>(std::ceil(relEnd));
            binStart = std::clamp(binStart, base, base + chunkBins);
            binEnd = std::clamp(binEnd, base, base + chunkBins);
            if (binEnd <= binStart) {
                binEnd = std::min(base + chunkBins, binStart + 1);
            }

            float minVal = 0.0f;
            float maxVal = 0.0f;
            bool haveSample = false;
            for (int b = binStart; b < binEnd; ++b) {
                if (b < 0 || b >= totalBins) {
                    continue;
                }
                const float curMin = sourceMinBins[b];
                const float curMax = sourceMaxBins[b];
                if (!haveSample) {
                    minVal = curMin;
                    maxVal = curMax;
                    haveSample = true;
                } else {
                    minVal = std::min(minVal, curMin);
                    maxVal = std::max(maxVal, curMax);
                }
            }

            if (!haveSample) {
                minVal = 0.0f;
                maxVal = 0.0f;
            }

            // Apply normalization and a small noise gate to avoid drawing near-silence as large shapes
            minVal *= chunkNormalizationFactor;
            maxVal *= chunkNormalizationFactor;
            const float noiseGate = 0.01f; // ~1% of full scale
            if (std::max(std::abs(minVal), std::abs(maxVal)) < noiseGate) {
                minVal = 0.0f;
                maxVal = 0.0f;
            }
            const float peakLimit = 1.0f;
            minVal = std::max(-peakLimit, std::min(0.0f, minVal));
            maxVal = std::min(peakLimit, std::max(0.0f, maxVal));
            chunk.upper[s] = maxVal;
            chunk.lower[s] = minVal;
        }

        chunkCache.emplace_back(std::move(chunk));
    }
}

double WaveformDisplay::acquireVisualPlayhead() {
    double fallback = std::isfinite(playheadPos) ? playheadPos : 0.0;
    if (!visualPlayheadInitialized) {
        resetVisualPlayhead(fallback);
        return visualPlayheadPos;
    }

    const auto now = std::chrono::steady_clock::now();
    const double previousVisual = visualPlayheadPos;
    double frameDt = std::chrono::duration<double>(now - lastVisualUpdateTime).count();
    if (!std::isfinite(frameDt) || frameDt < 0.0) {
        frameDt = 0.0;
    }
    frameDt = std::min(frameDt, 0.05);

    double measurement = targetPlayheadPos;
    if (!std::isfinite(measurement)) {
        measurement = fallback;
        targetPlayheadPos = measurement;
    }

    const double playVelocity = std::isfinite(estimatedPlaybackRate) ? estimatedPlaybackRate : 0.0;
    double predicted = visualPlayheadPos + playVelocity * frameDt;
    if (!std::isfinite(predicted)) {
        predicted = visualPlayheadPos;
    }

    measurement += visualLatencyComp * playVelocity;

    double error = measurement - predicted;

    // FIXED: Immediate stop on pause - detect near-zero velocity
    const double velocityMag = std::abs(playVelocity);
    const bool isPaused = velocityMag < 0.001; // Detect pause state
    
    double catchHz = (velocityMag > 1e-4 ? 22.0 : 14.0) + std::min(velocityMag * 18.0, 24.0);
    if (isInSeekMode || isPaused) {
        catchHz = std::max(catchHz, 60.0); // Instant catch-up on pause/seek
    }

    double correction = error * catchHz * frameDt;
    
    // FIXED: Instant snap on pause, faster catch on seek
    double baseCatchPerSec = isPaused ? 10.0 : (isInSeekMode ? 2.4 : (0.24 + velocityMag * 0.4));
    double minCatchPerFrame = isPaused ? 0.01 : (isInSeekMode ? 0.0004 : 0.00008);
    double maxCatch = std::max(minCatchPerFrame, baseCatchPerSec * frameDt);
    correction = std::clamp(correction, -maxCatch, maxCatch);

    double newVisual = predicted + correction;

    // BUG #18 FIX: Anti-Rewind ERST anwenden, DANN Snap (Snap hat Vorrang!)
    // Anti-Rewind NUR bei aktivem Playback (NICHT bei Pause/Seek)
    if (!isInSeekMode && !isPaused && velocityMag > 0.02) {
        const double slop = 0.00008;
        if (playVelocity > 0.02) {
            newVisual = std::max(newVisual, previousVisual - slop);
        } else if (playVelocity < -0.02) {
            newVisual = std::min(newVisual, previousVisual + slop);
        }
    }

    // JETZT erst Snap anwenden (überschreibt Anti-Rewind falls nötig)
    if (isPaused || std::abs(error) < 1e-5) {
        newVisual = measurement; // Direct snap when paused - hat VORRANG!
    } else if (std::abs(error) < 0.0001) {
        newVisual = predicted + error * 0.35; // Smooth bleed for tiny errors
    }

    if (!std::isfinite(newVisual)) {
        newVisual = fallback;
    }

    const double minRel = prerollEnabled ? -1.2 : -0.1;
    const double maxRel = 1.1;
    visualPlayheadPos = std::clamp(newVisual, minRel, maxRel);

    lastVisualUpdateTime = now;
    return visualPlayheadPos;
}

void WaveformDisplay::updateTempo(double newBpm) {
    if (originalBpm <= 0.0) {
        update();
        return;
    }
    // Compute tempo factor relative to original BPM
    double factor = newBpm / originalBpm;
    if (factor <= 0.0) factor = 1.0;
    tempoFactor = factor; // Store locally for this deck
    update();
}

void WaveformDisplay::refreshBeatGrid() {
    update();
}

void WaveformDisplay::mousePressEvent(QMouseEvent* event) { QOpenGLWidget::mousePressEvent(event); }
void WaveformDisplay::mouseMoveEvent(QMouseEvent* event)  { QOpenGLWidget::mouseMoveEvent(event); }
void WaveformDisplay::mouseReleaseEvent(QMouseEvent* event){ QOpenGLWidget::mouseReleaseEvent(event); }
void WaveformDisplay::leaveEvent(QEvent* event)            { QOpenGLWidget::leaveEvent(event); }

void WaveformDisplay::applyScratchResult(const ScratchEngine::UpdateResult& result) { Q_UNUSED(result); }

double WaveformDisplay::relativeToSeconds(double relative) const {
    if (relative < 0.0 && prerollEnabled) {
        return relative * prerollTimeSec;
    }
    double totalLength = (audioLength > 0.0) ? audioLength : trackLengthSec;
    if (totalLength > 0.0) {
        return std::clamp(relative, 0.0, 1.0) * totalLength;
    }
    return 0.0;
}

bool WaveformDisplay::computeViewportMetrics(ViewportMetrics& metrics) const {
    const int viewportWidth = std::max(width(), 1);
    if (viewportWidth <= 1) return false;

    const double zoomFactor = getBeatGridZoomFactor();
    const double safeTempo = tempoFactor > 1e-6 ? tempoFactor : 1.0;

    double totalLength = (audioLength > 0.0) ? audioLength : trackLengthSec;
    if (totalLength <= 0.0 && !prerollEnabled) totalLength = 0.0;

    double basePps = 0.0;
    if (useFixedPixelsPerSecond) basePps = std::max(10.0, localPixelsPerSecond);
    else if (audioLength > 0.0) basePps = static_cast<double>(viewportWidth) / std::max(audioLength, 1e-3);
    if (basePps <= 0.0) basePps = std::max(10.0, localPixelsPerSecond);

    const double pixelsPerSecond = std::max(1.0, basePps * zoomFactor);

    double visualRel = std::isfinite(activeRenderPlayhead) ? activeRenderPlayhead : playheadPos;
    double playheadSeconds = 0.0;
    if (visualRel < 0.0 && prerollEnabled) playheadSeconds = visualRel * prerollTimeSec;
    else if (totalLength > 0.0) playheadSeconds = std::clamp(visualRel, 0.0, 1.0) * totalLength;

    // Shift display center by output latency so overlays match perceived audio
    const double displayCenterSeconds = (viewMode == ViewMode::BeatLocked)
        ? ((playheadSeconds + renderLatencySec) / safeTempo)
        : (playheadSeconds + renderLatencySec);

    const double bufferSec = std::max(0.05, 0.5 / std::max(1.0, zoomFactor));
    const double halfViewportTime = static_cast<double>(viewportWidth) / (2.0 * pixelsPerSecond);

    const double visibleLeftSecond = displayCenterSeconds - halfViewportTime;
    const double visibleRightSecond = displayCenterSeconds + halfViewportTime;
    const double timeRange = visibleRightSecond - visibleLeftSecond;
    if (timeRange <= 0.0) return false;

    const double secondsPerPixelDisplay = timeRange / static_cast<double>(viewportWidth);

    double dpiX = static_cast<double>(logicalDpiX());
    if (dpiX <= 1.0) dpiX = 96.0;
    const double pixelsPerCentimeter = dpiX / 2.54;
    const double waveformNudgePx = 0.75 - (pixelsPerCentimeter * 0.5);

    const double extraLeftDisplaySec = std::max(0.0, -waveformNudgePx) * secondsPerPixelDisplay;
    const double extraRightDisplaySec = std::max(0.0, waveformNudgePx) * secondsPerPixelDisplay;

    const double fetchLeftSecond = visibleLeftSecond - bufferSec - extraLeftDisplaySec;
    const double fetchRightSecond = visibleRightSecond + bufferSec + extraRightDisplaySec;

    metrics.playheadSeconds = playheadSeconds;
    metrics.displayCenterSecond = displayCenterSeconds;
    metrics.safeTempo = safeTempo;
    metrics.leftSecond = visibleLeftSecond;
    metrics.rightSecond = visibleRightSecond;
    metrics.fetchLeftSecond = fetchLeftSecond;
    metrics.fetchRightSecond = fetchRightSecond;
    metrics.timeRange = timeRange;
    metrics.waveformNudgeSec = waveformNudgePx * secondsPerPixelDisplay;
    metrics.viewportWidth = viewportWidth;
    return true;
}

double WaveformDisplay::secondsAtViewportX(double x) const {
    ViewportMetrics metrics;
    if (!computeViewportMetrics(metrics)) return std::numeric_limits<double>::quiet_NaN();

    const double clampedX = std::clamp(x, 0.0, static_cast<double>(metrics.viewportWidth));
    const double positionRatio = clampedX / static_cast<double>(metrics.viewportWidth);
    const double visualSeconds = metrics.leftSecond + (positionRatio * metrics.timeRange);
    return mapDisplayToAudio(visualSeconds);
}

double WaveformDisplay::secondsToRelative(double seconds) const {
    if (std::isnan(seconds) || std::isinf(seconds)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    if (seconds < 0.0 && prerollEnabled) {
        const double denom = std::max(0.001, prerollTimeSec);
        return std::clamp(seconds / denom, -1.0, 0.0);
    }

    const double totalLength = (audioLength > 0.0) ? audioLength : trackLengthSec;
    if (totalLength <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::clamp(seconds / totalLength, 0.0, 1.0);
}

bool WaveformDisplay::seekToMousePosition(double x) { Q_UNUSED(x); return false; }

void WaveformDisplay::updateTempoDragFromMouse(double x) { Q_UNUSED(x); }

void WaveformDisplay::requestStreamingWindowIfNeeded(int neededStartBin, int neededEndBin, double binPerSecond)
{
    if (!streamingMode || binPerSecond <= 0.0) {
        return;
    }

    if (neededEndBin <= neededStartBin) {
        return;
    }

    // Aggressive preloading for smooth playback and seeking
    const int largerPreload = streamingPreloadBins * 6; // Increased from *3 to *6
    const int desiredStart = std::max(0, neededStartBin - largerPreload);
    int desiredEnd = neededEndBin + largerPreload;
    if (streamingTotalBins > 0) {
        desiredEnd = std::min(desiredEnd, streamingTotalBins);
    }

    if (desiredEnd <= desiredStart) {
        desiredEnd = desiredStart + 1;
    }

    if (desiredEnd <= desiredStart) {
        return;
    }

    if (desiredStart >= availableStartBin && desiredEnd <= availableEndBin) {
        return; // Nichts anzufordern, bereits im Cache
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto requestTimeout = std::chrono::seconds(2);
    constexpr auto coverDebounce = std::chrono::milliseconds(100);
    constexpr auto expandDebounce = std::chrono::milliseconds(180);
    constexpr auto overlapDebounce = std::chrono::milliseconds(140);

    const bool outsideCurrentWindow = (desiredEnd <= availableStartBin) || (desiredStart >= availableEndBin);
    const bool farFromEitherEdge = (availableEndBin > availableStartBin)
        ? (std::min(std::abs(desiredStart - availableStartBin), std::abs(desiredEnd - availableEndBin)) > streamingPreloadBins * 2)
        : true;
    const bool isJump = isInSeekMode || outsideCurrentWindow || farFromEitherEdge;

    auto clearPendingRequest = [&]() {
        hasPendingRegionRequest = false;
        pendingRequestStartBin = 0;
        pendingRequestEndBin = 0;
        lastRegionRequestTime = {};
    };

    if (hasPendingRegionRequest) {
        const bool neverSent = (lastRegionRequestTime == std::chrono::steady_clock::time_point{});
        const auto sinceLast = neverSent ? std::chrono::steady_clock::duration::zero() : (now - lastRegionRequestTime);
        const bool timedOut = !neverSent && (sinceLast >= requestTimeout);
        const bool coversPending = desiredStart >= pendingRequestStartBin && desiredEnd <= pendingRequestEndBin;
        const bool identicalPending = coversPending && (desiredStart == pendingRequestStartBin) && (desiredEnd == pendingRequestEndBin);
        const bool expandsLeft = desiredStart < pendingRequestStartBin;
        const bool expandsRight = desiredEnd > pendingRequestEndBin;
        const bool expandsPending = expandsLeft || expandsRight;
        const bool widerRequest = (desiredEnd - desiredStart) > (pendingRequestEndBin - pendingRequestStartBin);

        if (isJump) {
#ifdef WAVEFORM_DEBUG
            qDebug() << "[WaveformDisplay] Streaming-Request wird wegen Sprung sofort ersetzt"
                     << desiredStart << desiredEnd;
#endif
            clearPendingRequest();
        } else if (neverSent || timedOut) {
#ifdef WAVEFORM_DEBUG
            qDebug() << "[WaveformDisplay] Streaming-Request abgelaufen, ersetze Altanfrage"
                     << pendingRequestStartBin << pendingRequestEndBin;
#endif
            clearPendingRequest();
        } else if (coversPending) {
            if (sinceLast < coverDebounce) {
#ifdef WAVEFORM_DEBUG
                qDebug() << "[WaveformDisplay] Streaming-Request gedeckt, warte noch"
                         << desiredStart << desiredEnd;
#endif
                return;
            }
#ifdef WAVEFORM_DEBUG
            qDebug() << "[WaveformDisplay] Streaming-Request gedeckt, ersetze nach Wartezeit"
                     << desiredStart << desiredEnd;
#endif
            clearPendingRequest();
        } else if (expandsPending || widerRequest) {
            if (sinceLast < expandDebounce) {
#ifdef WAVEFORM_DEBUG
                qDebug() << "[WaveformDisplay] Groesseres Fenster angefragt, warte noch"
                         << desiredStart << desiredEnd;
#endif
                return;
            }
#ifdef WAVEFORM_DEBUG
            qDebug() << "[WaveformDisplay] Groesseres Fenster wird nach Wartezeit angefordert"
                     << desiredStart << desiredEnd;
#endif
            clearPendingRequest();
        } else {
            if (sinceLast < overlapDebounce) {
#ifdef WAVEFORM_DEBUG
                qDebug() << "[WaveformDisplay] Streaming-Request ueberlappt, debounce aktiv"
                         << desiredStart << desiredEnd;
#endif
                return;
            }
#ifdef WAVEFORM_DEBUG
            qDebug() << "[WaveformDisplay] Streaming-Request ueberlappt, ersetze nach Wartezeit"
                     << desiredStart << desiredEnd;
#endif
            clearPendingRequest();
        }
    }

    const double startSec = audioStartOffset + (static_cast<double>(desiredStart) / binPerSecond);
    const double endSec = audioStartOffset + (static_cast<double>(desiredEnd) / binPerSecond);

    hasPendingRegionRequest = true;
    pendingRequestStartBin = desiredStart;
    pendingRequestEndBin = desiredEnd;
    lastRegionRequestTime = now;

#ifdef WAVEFORM_DEBUG
    qDebug() << "[WaveformDisplay] Streaming-Request gesendet" << desiredStart << desiredEnd;
#endif
    emit waveformRegionNeeded(startSec, endSec);
}

void WaveformDisplay::resetStreamingState()
{
    availableStartBin = 0;
    availableEndBin = 0;
    streamingMode = false;
    streamingComplete = false;
    streamingTotalBins = 0;
    streamingPreloadBins = 8000;      // Increased from 4000 (±20s @ 400 bins/sec)
    streamingMaxCacheBins = 160000;   // Increased from 40000 (±2min coverage)
    streamingExpectedNextBin = 0;
    hasPendingRegionRequest = false;
    pendingRequestStartBin = 0;
    pendingRequestEndBin = 0;
    lastRegionRequestTime = {};
    chunkCache.clear();
    chunkCacheDirty = true;
    chunkNormalizationFactor = 1.0f;
    secondsPerBin = 0.0;
    
    // Clear adaptive chunks and fallback for new file
    {
        std::lock_guard<std::mutex> lock(adaptiveChunksMutex_);
        adaptiveChunks_.clear();
    }
    {
        std::lock_guard<std::mutex> fbLock(fallbackMutex_);
        fallbackMaxBins_.clear();
        fallbackMinBins_.clear();
        fallbackComplete_ = false;
    }
    
    renderCache.geometryValid = false;
    renderCache.needsFullRedraw = true;
    renderCache.needsUpdate = true; // Nach Reset komplette Geometrie neu aufbauen
    updateRenderActivity();
}

void WaveformDisplay::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            increaseBeatGridZoom();
            break;
        case Qt::Key_Minus:
            decreaseBeatGridZoom();
            break;
        case Qt::Key_0:
            resetBeatGridZoom();
            break;
        default:
            QWidget::keyPressEvent(event);
            break;
    }
}

void WaveformDisplay::increaseBeatGridZoom() {
    if (beatGridZoomLevel < 9) {
        beatGridZoomLevel++;
        emit zoomLevelChanged(beatGridZoomLevel);
        update();
    }
}

void WaveformDisplay::decreaseBeatGridZoom() {
    if (beatGridZoomLevel > 0) {
        beatGridZoomLevel--;
        emit zoomLevelChanged(beatGridZoomLevel);
        update();
    }
}

void WaveformDisplay::resetBeatGridZoom() {
    beatGridZoomLevel = 4;
    emit zoomLevelChanged(beatGridZoomLevel);
    update();
}

void WaveformDisplay::setBeatGridZoomLevel(int level) {
    beatGridZoomLevel = std::clamp(level, 0, 9);
    update();
}

double WaveformDisplay::mapXToAbsRel(double x) const
{
    return std::clamp(x / std::max(1, width()), 0.0, 1.0);
}

void WaveformDisplay::recomputeBeatPhaseShift() {}
void WaveformDisplay::generateDefaultGrid() {}

void WaveformDisplay::setCuePoints(const std::array<double, 8>& newCuePoints) {
    cuePoints = newCuePoints;
    cuePointsValid = true;
    update();
}

void WaveformDisplay::clearCuePoints() {
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    update();
}

void WaveformDisplay::setLoopRegion(bool enabled, double startSec, double endSec) {
    loopEnabled = enabled;
    loopStartSec = startSec;
    loopEndSec = endSec;
    update();
}

void WaveformDisplay::clearLoop() {
    loopEnabled = false;
    loopStartSec = 0.0;
    loopEndSec = 0.0;
    update();
}

void WaveformDisplay::clearDisplay() {
    resetStreamingState();
    sourceMaxBins.clear();
    sourceMinBins.clear();
    sourceWidth = 0;
    audioLength = 0.0;
    trackLengthSec = 0.0;
    originalBpm = 0.0;
    firstBeatOffset = 0.0;
    beatPositions.clear();
    useAnalyzedBeats = false;
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    loopEnabled = false;
    loopStartSec = 0.0;
    loopEndSec = 0.0;
    ghostLoopEnabled = false;
    ghostLoopStartSec = 0.0;
    ghostLoopEndSec = 0.0;
    playheadPos = -1.0;
    resetVisualPlayhead(playheadPos);
    visualPlayheadInitialized = false;
    renderCache = RenderCache{};
    update();
}

void WaveformDisplay::setScratchEngine(ScratchEngine* engine) {
    for (const auto& connection : scratchEngineConnections) {
        QObject::disconnect(connection);
    }
    scratchEngineConnections.clear();
    scratchEngine = nullptr;
    Q_UNUSED(engine);
}

void WaveformDisplay::setGhostLoopRegion(bool enabled, double startSec, double endSec) {
    ghostLoopEnabled = enabled;
    ghostLoopStartSec = startSec;
    ghostLoopEndSec = endSec;
    update();
}

void WaveformDisplay::drawBeatGrid(QPainter& p, double playheadSec, double displayCenterSec,
                                   double leftSecond, double rightSecond, double timeRange) {
    
    if (originalBpm <= 0.0 || trackLengthSec <= 0.0 || timeRange <= 0.0) {
        return;
    }
    
    const int widgetWidth = width();
    const int widgetHeight = height();
    if (widgetWidth <= 0) {
        return;
    }
    
    const double safeTempo = (geometryCache.lastTempoFactor > 1e-6) ? geometryCache.lastTempoFactor : 1.0;
    const double secondsPerBeat = (originalBpm > 0.0) ? (60.0 / originalBpm) : 0.5; // AUDIO-time spacing
    
    const double toPixel = (timeRange > 0.0) ? (static_cast<double>(widgetWidth) / timeRange) : 0.0;
    auto timeToX = [&](double displaySec) -> int {
        return static_cast<int>((displaySec - leftSecond) * toPixel);
    };
    
    // AUDIO time range from display via centralized inverse mapping
    const double audioLeftSec = mapDisplayToAudio(leftSecond);
    const double audioRightSec = mapDisplayToAudio(rightSecond);
    
    p.save();
    static const QPen downbeatPen(QColor(255, 150, 50, 200), 3.0);
    static const QPen regularBeatPen(QColor(200, 220, 255, 160), 1.5);
    static const QFont barFont("Arial", 9, QFont::Bold);
    static const QFont infoFont("Arial", 8);

    const bool haveAnalyzedBeats = useAnalyzedBeats && !beatPositions.isEmpty();
    if (haveAnalyzedBeats) {
        // Draw using analyzed beat times (in seconds) for perfect alignment
        const double start = std::max(0.0, audioLeftSec);
        const double end = std::min(trackLengthSec > 0.0 ? trackLengthSec : audioRightSec, audioRightSec);
        auto bBegin = beatPositions.begin();
        auto bEnd = beatPositions.end();
        auto lb = std::lower_bound(bBegin, bEnd, start);
        int startIdx = std::max(0, static_cast<int>(lb - bBegin) - 2);
        int endIdx = std::min(static_cast<int>(beatPositions.size()) - 1,
                              static_cast<int>(std::upper_bound(bBegin, bEnd, end) - bBegin) + 2);

        for (int i = startIdx; i <= endIdx; ++i) {
            const double beatTimeAudio = beatPositions[i];
            if (!std::isfinite(beatTimeAudio) || beatTimeAudio < 0.0) continue;
            if (trackLengthSec > 0.0 && beatTimeAudio > trackLengthSec) break;

            const double beatTimeDisplay = mapAudioToDisplay(beatTimeAudio);
            int x = timeToX(beatTimeDisplay);
            if (x < 0 || x >= widgetWidth) continue;

            // Try to infer downbeats using original BPM spacing relative to firstBeatOffset
            bool isDownbeat = false;
            if (originalBpm > 0.0) {
                const double firstBeat = (firstBeatOffset > 0.0) ? firstBeatOffset : 0.0;
                const double approxIdx = std::round((beatTimeAudio - firstBeat) / secondsPerBeat);
                isDownbeat = static_cast<long long>(approxIdx) % 4 == 0;
            }

            if (isDownbeat) {
                p.setPen(downbeatPen);
                p.drawLine(x, 0, x, widgetHeight);
                p.setFont(barFont);
                p.setPen(QPen(QColor(255, 180, 100, 200), 1));
                long long barNumber = (originalBpm > 0.0)
                    ? (static_cast<long long>(std::round((beatTimeAudio - std::max(0.0, firstBeatOffset)) / secondsPerBeat)) / 4 + 1)
                    : 0;
                if (barNumber > 0) p.drawText(x + 3, 15, QString::number(barNumber));
            } else {
                p.setPen(regularBeatPen);
                p.drawLine(x, widgetHeight / 3, x, 2 * widgetHeight / 3);
            }
        }
    } else {
        // Fallback: constant spacing from BPM/firstBeatOffset
        const double firstBeat = (firstBeatOffset > 0.0) ? firstBeatOffset : 0.0;
        int startBeatIdx = static_cast<int>(std::floor((std::max(0.0, audioLeftSec) - firstBeat) / secondsPerBeat)) - 2;
        int endBeatIdx = static_cast<int>(std::ceil((std::min(trackLengthSec, audioRightSec) - firstBeat) / secondsPerBeat)) + 2;
        for (int beatIdx = startBeatIdx; beatIdx <= endBeatIdx; ++beatIdx) {
            const double beatTimeAudio = firstBeat + beatIdx * secondsPerBeat;
            if (beatTimeAudio < 0.0 || (trackLengthSec > 0.0 && beatTimeAudio > trackLengthSec)) continue;
            const double beatTimeDisplay = mapAudioToDisplay(beatTimeAudio);
            int x = timeToX(beatTimeDisplay);
            if (x < 0 || x >= widgetWidth) continue;
            const bool isDownbeat = (beatIdx % 4 == 0);
            if (isDownbeat) {
                p.setPen(downbeatPen);
                p.drawLine(x, 0, x, widgetHeight);
                p.setFont(barFont);
                p.setPen(QPen(QColor(255, 180, 100, 200), 1));
                int barNumber = (beatIdx / 4) + 1;
                p.drawText(x + 3, 15, QString::number(barNumber));
            } else {
                p.setPen(regularBeatPen);
                p.drawLine(x, widgetHeight / 3, x, 2 * widgetHeight / 3);
            }
        }
    }
    
    p.setFont(infoFont);
    int rightX = widgetWidth - 8;
    int y = 15;
    
    const bool hasBpm = originalBpm > 0.0;
    const double cappedProgress = std::clamp(analysisProgress, 0.0, 1.0);
    const int percent = static_cast<int>(std::floor(cappedProgress * 100.0 + 0.0001));
    const bool showProgress = (!hasBpm) && analysisActive && percent < 100;

    if (analysisFailed) {
        p.setPen(QPen(QColor(255, 120, 120), 1));
        QString txt(QStringLiteral("Analysis failed"));
        int w = p.fontMetrics().horizontalAdvance(txt);
        p.drawText(rightX - w, y, txt);
    } else if (showProgress) {
        p.setPen(QPen(QColor(180, 200, 255), 1));
    QString txt = QStringLiteral("Analyzing %1%").arg(percent);
        int w = p.fontMetrics().horizontalAdvance(txt);
        p.drawText(rightX - w, y, txt);
    } else if (hasBpm) {
        p.setPen(QPen(QColor(150, 180, 220), 1));
        const double deckBpm = originalBpm * safeTempo;
        QString bpmText = QString("BPM: %1").arg(deckBpm, 0, 'f', 1);
        int w = p.fontMetrics().horizontalAdvance(bpmText);
        p.drawText(rightX - w, y, bpmText);
    }
    
    if (useFixedPixelsPerSecond) {
        QString ratioText = QString("%1px/s").arg(localPixelsPerSecond, 0, 'f', 0);
        int w = p.fontMetrics().horizontalAdvance(ratioText);
        p.setPen(QPen(QColor(150, 180, 220), 1));
        p.drawText(rightX - w, 30, ratioText);
    }
    
    p.restore();
}

void WaveformDisplay::drawCuePoints(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (!cuePointsValid || timeRange <= 0.0) {
        return;
    }

    const int widgetWidth = width();
    const int widgetHeight = height();
    
    auto timeToX = [&](double sec) -> int {
        double frac = (sec - leftSecond) / timeRange;
        return static_cast<int>(frac * widgetWidth);
    };

    const auto& cuePalette = WaveformTheme::cueColors();

    p.save();
    
    for (int i = 0; i < 8; ++i) {
        double cueSec = cuePoints[i];
        if (cueSec < 0.0) {
            continue;
        }
        
        if (cueSec < leftSecond || cueSec > rightSecond) {
            continue;
        }
        
        int x = timeToX(cueSec);
        
        p.setPen(QPen(cuePalette[i], 2));
        p.drawLine(x, 0, x, widgetHeight);
        
        p.fillRect(x - 3, 5, 6, 20, cuePalette[i]);
        p.setPen(Qt::white);
        p.drawText(x - 10, 20, QString::number(i + 1));
    }
    
    p.restore();
}

// NEW: Draw loop region as semi-transparent box
void WaveformDisplay::drawLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (timeRange <= 0.0 || audioLength <= 0.0) return;
    QColor loopColor = WaveformTheme::loopBaseColor();
    loopColor.setAlpha(160);
    QColor loopStroke = WaveformTheme::loopBorderColor();
    loopStroke.setAlpha(200);
    QPen loopPen(loopStroke, 2.5);
    loopPen.setStyle(Qt::SolidLine);
    drawRangeOverlay(p, loopStartSec, loopEndSec, leftSecond, rightSecond, timeRange,
                     loopColor, loopPen, QStringLiteral("LOOP"), 15, 200);
}

// NEW: Draw ghost loop region as very transparent box for last used loop
void WaveformDisplay::drawGhostLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (!ghostLoopEnabled || timeRange <= 0.0 || audioLength <= 0.0) return;
    QColor ghostColor = WaveformTheme::ghostLoopBaseColor();
    ghostColor.setAlpha(20);
    QColor ghostStroke = WaveformTheme::ghostLoopBorderColor();
    ghostStroke.setAlpha(80);
    QPen ghostPen(ghostStroke, 1.5);
    ghostPen.setStyle(Qt::DashLine);
    drawRangeOverlay(p, ghostLoopStartSec, ghostLoopEndSec, leftSecond, rightSecond, timeRange,
                     ghostColor, ghostPen, QStringLiteral("GHOST"), 30, 100);
}

void WaveformDisplay::drawRangeOverlay(QPainter& p,
                          double audioStartSec,
                          double audioEndSec,
                          double leftSecond,
                          double rightSecond,
                          double timeRange,
                          const QColor& boxColor,
                          const QPen& boundaryPen,
                          const QString& label,
                          int labelY,
                          int labelBgAlpha) {
    if (audioEndSec <= audioStartSec) return;
    const double displayStart = mapAudioToDisplay(audioStartSec);
    const double displayEnd   = mapAudioToDisplay(audioEndSec);
    if (displayEnd < leftSecond || displayStart > rightSecond) return;

    const double toPixel = (timeRange > 0.0) ? (static_cast<double>(width()) / timeRange) : 0.0;
    const int x0 = static_cast<int>(std::clamp((displayStart - leftSecond) * toPixel, 0.0, static_cast<double>(width())));
    const int x1 = static_cast<int>(std::clamp((displayEnd   - leftSecond) * toPixel, 0.0, static_cast<double>(width())));
    if (x1 <= x0) return;

    p.fillRect(x0, 0, x1 - x0, height(), boxColor);
    p.setPen(boundaryPen);
    p.drawLine(x0, 0, x0, height());
    p.drawLine(x1, 0, x1, height());

    if (!label.isEmpty()) {
        p.setFont(QFont("Arial", 8, QFont::Bold));
        const QRect textRect = p.fontMetrics().boundingRect(label);
        const int labelX = x0 + ((x1 - x0) - textRect.width()) / 2;
        const QRect bg(labelX - 2, labelY - textRect.height() + 1, textRect.width() + 4, textRect.height());
        p.fillRect(bg, QColor(0, 0, 0, labelBgAlpha));
        p.setPen(QPen(QColor(100, 255, 100), 1));
        p.drawText(labelX, labelY, label);
    }
}

// NEW: Draw preroll region for DJ-style cueing
void WaveformDisplay::drawPrerollRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    // Convert the display-time edges to AUDIO-time to make decisions robust in all view modes
    const double audioLeftSec = mapDisplayToAudio(leftSecond);
    const double audioRightSec = mapDisplayToAudio(rightSecond);

    // Nothing to draw if the entire window is after track start
    if (audioLeftSec >= 0.0) return;

    const int w = width();
    const int h = height();

    // Entire viewport is preroll (audioRightSec <= 0)
    if (audioRightSec <= 0.0) {
        QColor prerollColor(30, 50, 80, 120);
        p.fillRect(QRect(0, 0, w, h), prerollColor);

        p.setPen(QPen(QColor(60, 100, 160), 1));
        for (int x = 0; x < w; x += 20) {
            p.drawLine(x, 0, x + 10, h);
        }

        p.setFont(QFont("Arial", 10, QFont::Bold));
        p.setPen(QPen(QColor(120, 180, 255), 1));
        p.drawText(w/2 - 30, h/2, "PREROLL");
        return;
    }

    // Partial preroll: shade from left edge up to the exact pixel where audio time 0.0 appears
    const double startDisplaySec = mapAudioToDisplay(0.0);
    const double range = std::max(geometryCache.timeRange, 1e-9);
    const double trackStartRatio = (startDisplaySec - geometryCache.leftSecond) / range;
    // Use floor with a tiny bias and clamp to stabilize at pixel boundaries and avoid +/-1 px toggling
    int trackStartX = static_cast<int>(std::floor(trackStartRatio * w + 0.001));
    trackStartX = std::clamp(trackStartX, 0, w);

    // Apply a small screen-edge dead zone to avoid flicker when the edge is at the very first/last pixel
    const int leftDead = 1;
    const int rightDead = 1;

    if (trackStartX <= leftDead) {
        lastPrerollEdgeX = trackStartX;
        return; // nothing visible beyond a hairline at the very left
    }
    if (trackStartX >= w - rightDead) {
        lastPrerollEdgeX = trackStartX;
        return; // skip drawing when start is effectively beyond the right edge to prevent late flashes
    }

    // Remember last edge to further reduce tiny oscillations (+/-1 px). Prefer the edge that moves inward.
    if (lastPrerollEdgeX >= 0) {
        // If the new edge is within 1px of the previous, keep the previous to avoid shimmer
        if (std::abs(trackStartX - lastPrerollEdgeX) <= 1) {
            trackStartX = lastPrerollEdgeX;
        } else {
            lastPrerollEdgeX = trackStartX;
        }
    } else {
        lastPrerollEdgeX = trackStartX;
    }

    QColor prerollColor(30, 50, 80, 120);
    p.fillRect(QRect(0, 0, trackStartX, h), prerollColor);

    p.setPen(QPen(QColor(60, 100, 160), 1));
    for (int x = 0; x < trackStartX; x += 15) {
        p.drawLine(x, 0, x + 8, h);
    }

    // Draw the track start line
    p.setPen(QPen(QColor(255, 255, 255), 2));
    p.drawLine(trackStartX, 0, trackStartX, h);

    // Label
    p.setFont(QFont("Arial", 8, QFont::Bold));
    p.setPen(QPen(QColor(120, 180, 255), 1));
    if (trackStartX > 60) {
        p.drawText(10, 20, "PREROLL");
    }
}