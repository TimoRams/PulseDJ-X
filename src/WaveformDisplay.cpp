#include "WaveformDisplay.h"
#include "WaveformGenerator.h"
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
#include <cmath>
#include <algorithm>
#include <chrono>
#include <limits>

WaveformDisplay::WaveformDisplay(QWidget* parent)
    : QOpenGLWidget(parent)
{
    QSurfaceFormat fmt = format();
    if (fmt.samples() < 4) fmt.setSamples(4);
    fmt.setSwapInterval(1);
    setFormat(fmt);

    formatManager.registerBasicFormats();
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    setFocusPolicy(Qt::StrongFocus);
    renderTimer = new QTimer(this);
    renderTimer->setTimerType(Qt::PreciseTimer);
    renderTimer->setInterval(FrameTiming::kFrameIntervalMs);
    connect(renderTimer, &QTimer::timeout, this, [this]() { if (isVisible()) update(); });
}

WaveformDisplay::~WaveformDisplay()
{
    if (renderTimer) renderTimer->stop();

    for (auto& connection : scratchEngineConnections) QObject::disconnect(connection);
    scratchEngineConnections.clear();

    QOpenGLContext* ctx = context();
    if (ctx && ctx->isValid()) {
        makeCurrent();
        destroyGlResources();
        doneCurrent();
    } else {
        // No valid context; ensure we don't try to use GL after this
        glResources.initialized = false;
        glResources.fillVbo = glResources.topLineVbo = glResources.bottomLineVbo = 0;
    }
}

namespace
{
// Shared cue colors
static const QColor kCueColors[8] = {
    QColor(255, 100, 100), QColor(100, 255, 100), QColor(100, 100, 255), QColor(255, 255, 100),
    QColor(255, 100, 255), QColor(100, 255, 255), QColor(255, 200, 100), QColor(200, 100, 255)
};

const char* kWaveformFillVertexShaderCore = R"(#version 330 core
layout(location = 0) in vec2 position;
out float vY;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vY = position.y;
}
)";

const char* kWaveformFillFragmentShaderCore = R"(#version 330 core
in float vY;
out vec4 fragColor;
uniform vec4 uColorTop;
uniform vec4 uColorMid;
uniform vec4 uColorBottom;
uniform float uMidStrength;
void main()
{
    float t = clamp((vY + 1.0) * 0.5, 0.0, 1.0);
    vec4 base = mix(uColorTop, uColorBottom, t);
    float centerBias = 1.0 - clamp(abs(t - 0.5) * 2.0, 0.0, 1.0);
    fragColor = mix(base, uColorMid, centerBias * uMidStrength);
}
)";

const char* kWaveformFillVertexShaderEs = R"(#version 300 es
layout(location = 0) in vec2 position;
out float vY;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vY = position.y;
}
)";

const char* kWaveformFillFragmentShaderEs = R"(#version 300 es
precision mediump float;
in float vY;
out vec4 fragColor;
uniform vec4 uColorTop;
uniform vec4 uColorMid;
uniform vec4 uColorBottom;
uniform float uMidStrength;
void main()
{
    float t = clamp((vY + 1.0) * 0.5, 0.0, 1.0);
    vec4 base = mix(uColorTop, uColorBottom, t);
    float centerBias = 1.0 - clamp(abs(t - 0.5) * 2.0, 0.0, 1.0);
    fragColor = mix(base, uColorMid, centerBias * uMidStrength);
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
    if (viewMode == ViewMode::BeatLocked) {
        return geometryCache.displayCenterSec + (audioSec - geometryCache.playheadSec) / safeTempo + geometryCache.alignShiftSec;
    }
    return audioSec + geometryCache.alignShiftSec;
}

double WaveformDisplay::mapDisplayToAudio(double displaySec) const {
    const double safeTempo = (geometryCache.lastTempoFactor > 1e-6) ? geometryCache.lastTempoFactor : 1.0;
    if (viewMode == ViewMode::BeatLocked) {
        return geometryCache.playheadSec + (displaySec - geometryCache.displayCenterSec - geometryCache.alignShiftSec) * safeTempo;
    }
    return displaySec - geometryCache.alignShiftSec;
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

    auto& upperPoints = upperPointBuffer;
    auto& lowerPoints = lowerPointBuffer;
    auto& missingSegments = missingSegmentBuffer;
    upperPoints.clear();
    lowerPoints.clear();
    missingSegments.clear();

    if (viewWidth <= 0 || viewHeight <= 0 || audioLength <= 0.0 || sourceWidth <= 0) {
        return;
    }

    const int centerY = viewHeight / 2;

    const double basePixelsPerSecond = useFixedPixelsPerSecond
        ? localPixelsPerSecond
        : static_cast<double>(viewWidth) / std::max(1.0, audioLength);
    const double safeTempo = tempoFactor > 1e-6 ? tempoFactor : 1.0;
    const double pixelsPerSecond = basePixelsPerSecond * zoomFactor;

    double playheadRel = renderPlayheadRel;
    double playheadSec = 0.0;
    if (playheadRel < 0.0 && prerollEnabled) {
        playheadSec = playheadRel * prerollTimeSec;
    } else {
        playheadRel = std::clamp(playheadRel, 0.0, 1.0);
        playheadSec = playheadRel * audioLength;
    }

    const double displayCenterSec = (viewMode == ViewMode::BeatLocked) ? (playheadSec / safeTempo) : playheadSec;
    const double bufferSec = std::max(0.05, 0.5 / std::max(1.0, zoomFactor));
    const double halfViewportTime = static_cast<double>(viewWidth) / (2.0 * pixelsPerSecond);
    
    const double leftSecond = displayCenterSec - halfViewportTime - bufferSec;
    const double rightSecond = displayCenterSec + halfViewportTime + bufferSec;

    geometryCache.playheadSec = playheadSec;
    geometryCache.displayCenterSec = displayCenterSec;
    geometryCache.leftSecond = leftSecond;
    geometryCache.rightSecond = rightSecond;
    geometryCache.timeRange = rightSecond - leftSecond;
    geometryCache.bufferSec = bufferSec;
    geometryCache.halfViewportTime = halfViewportTime;
    geometryCache.lastWidth = viewWidth;
    geometryCache.lastHeight = viewHeight;
    geometryCache.lastZoomFactor = zoomFactor;
    geometryCache.lastTempoFactor = tempoFactor;
    geometryCache.lastPlayheadPos = renderPlayheadRel;
    geometryCache.lastAvailableStartBin = availableStartBin;
    geometryCache.lastAvailableEndBin = availableEndBin;

    const double binPerSecond = static_cast<double>(sourceWidth) / std::max(audioLength, 1e-6);

    // Use pure track-time (0.0 = track start) for bin selection
    double audioLeftSec = std::max(0.0, playheadSec - halfViewportTime * safeTempo - bufferSec * safeTempo);
    double audioRightSec = std::max(0.0, playheadSec + halfViewportTime * safeTempo + bufferSec * safeTempo);

    int leftBin = std::max(0, static_cast<int>(audioLeftSec * binPerSecond));
    int rightBin = std::min(sourceWidth, static_cast<int>(audioRightSec * binPerSecond));

    if (streamingMode && !streamingComplete && binPerSecond > 0.0) {
        const int neededStartBin = std::max(0, leftBin - streamingPreloadBins);
        int neededEndBin = rightBin + streamingPreloadBins;
        if (streamingTotalBins > 0) {
            neededEndBin = std::min(streamingTotalBins, neededEndBin);
        }
        requestStreamingWindowIfNeeded(neededStartBin, neededEndBin, binPerSecond);
    }

    if (leftBin >= rightBin && rightSecond > 0.0) {
        return;
    }

    const int pixelWidth = viewWidth;
    const double timeRange = geometryCache.timeRange;
    if (timeRange <= 0.0 || pixelWidth <= 0) {
        return;
    }

    rebuildChunkCacheIfNeeded();

    // Align start: when near or before track start (including preroll), keep audioSec=0 centered.
    // Use a tolerance of ~0.75 pixel in time to avoid jitter from tiny positive playhead values.
    const double secondsPerPixel = (pixelsPerSecond > 1e-6) ? (1.0 / pixelsPerSecond) : 0.0;
    const double alignSnapSec = secondsPerPixel * 0.75; // tolerance band
    double alignShiftSec = 0.0;
    if (playheadSec <= alignSnapSec) {
        // Shift display space so that audioSec=0 maps to the visual center
        alignShiftSec = displayCenterSec;
    }
    geometryCache.alignShiftSec = alignShiftSec;
    upperPoints.reserve(pixelWidth + 2);
    lowerPoints.reserve(pixelWidth + 2);
    missingSegments.reserve(4);

    // Prepare per-pixel buffers using logical pixel width
    if (static_cast<int>(pixelUpperScratch.size()) != pixelWidth) pixelUpperScratch.assign(pixelWidth, 0.0f); else std::fill(pixelUpperScratch.begin(), pixelUpperScratch.end(), 0.0f);
    if (static_cast<int>(pixelLowerScratch.size()) != pixelWidth) pixelLowerScratch.assign(pixelWidth, 0.0f); else std::fill(pixelLowerScratch.begin(), pixelLowerScratch.end(), 0.0f);
    if (static_cast<int>(pixelCoverageScratch.size()) != pixelWidth) pixelCoverageScratch.assign(pixelWidth, 0); else std::fill(pixelCoverageScratch.begin(), pixelCoverageScratch.end(), 0);

    const double pixelHeight = static_cast<double>(viewHeight);
    const float waveformHeightScale = 0.42f;

    // Exact per-pixel aggregation in AUDIO domain mapped from DISPLAY
    const double secondsPerPixelDisplay = (timeRange > 0.0) ? (timeRange / static_cast<double>(pixelWidth)) : 0.0;
    const double audioWidthPerPixel = secondsPerPixelDisplay * safeTempo; // BeatLocked: dAudio = safeTempo * dDisplay

    // Streaming-aware local bin window
    const int totalLocalBins = static_cast<int>(sourceMaxBins.size());
    const auto clampLocal = [&](int idx) { return std::clamp(idx, 0, std::max(0, totalLocalBins - 1)); };

    int currentMissingStart = -1;

    for (int x = 0; x < pixelWidth; ++x) {
        const double displayCenterXSec = leftSecond + (static_cast<double>(x) + 0.5) * secondsPerPixelDisplay;
        const double audioCenterSec = mapDisplayToAudio(displayCenterXSec);
        double audioStart = audioCenterSec - 0.5 * audioWidthPerPixel;
        double audioEnd   = audioCenterSec + 0.5 * audioWidthPerPixel;
        if (audioEnd <= 0.0 || audioStart >= audioLength) {
            // Out of track range
            if (currentMissingStart < 0) currentMissingStart = x;
            continue;
        }
        audioStart = std::max(0.0, audioStart);
        audioEnd   = std::min(audioLength, audioEnd);

        const int gb0 = static_cast<int>(std::floor(audioStart * binPerSecond));
        const int gb1 = std::max(gb0 + 1, static_cast<int>(std::ceil(audioEnd * binPerSecond)));

        // Map to local indices within current cached window
        int li0 = gb0 - availableStartBin;
        int li1 = gb1 - availableStartBin;

        if (li1 <= 0 || li0 >= totalLocalBins) {
            // No data cached here
            if (currentMissingStart < 0) currentMissingStart = x;
            continue;
        }

        li0 = clampLocal(li0);
        li1 = std::min(std::max(li0 + 1, li1), totalLocalBins);

        float minVal = 0.0f, maxVal = 0.0f;
        bool have = false;
        for (int i = li0; i < li1; ++i) {
            float vmin = sourceMinBins[i];
            float vmax = sourceMaxBins[i];
            if (!have) {
                minVal = vmin; maxVal = vmax; have = true;
            } else {
                minVal = std::min(minVal, vmin);
                maxVal = std::max(maxVal, vmax);
            }
        }

        if (!have) {
            if (currentMissingStart < 0) currentMissingStart = x;
            continue;
        }

        if (currentMissingStart >= 0) {
            // Flush previous missing segment
            missingSegments.emplace_back(currentMissingStart, x);
            currentMissingStart = -1;
        }

        // Normalize & clamp
        minVal *= chunkNormalizationFactor;
        maxVal *= chunkNormalizationFactor;
        const float peakLimit = 1.0f;
        minVal = std::max(-peakLimit, std::min(0.0f, minVal));
        maxVal = std::min(peakLimit, std::max(0.0f, maxVal));

        pixelUpperScratch[x] = maxVal;
        pixelLowerScratch[x] = minVal;
        pixelCoverageScratch[x] = 1;
    }

    if (currentMissingStart >= 0) {
        missingSegments.emplace_back(currentMissingStart, pixelWidth);
    }

    for (int x = 0; x < pixelWidth; ++x) {
        if (!pixelCoverageScratch[x]) continue;
        const double screenX = static_cast<double>(x) + 0.5;
        double upperY = centerY - static_cast<double>(pixelUpperScratch[x]) * pixelHeight * waveformHeightScale;
        double lowerY = centerY - static_cast<double>(pixelLowerScratch[x]) * pixelHeight * waveformHeightScale;
        upperPoints.emplace_back(screenX, upperY);
        lowerPoints.emplace_back(screenX, lowerY);
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
    // Only rebuild if significant changes occurred
    if (!renderCache.geometryValid || renderCache.needsFullRedraw) return true;
    if (renderCache.lastWidth != viewWidth || renderCache.lastHeight != viewHeight) return true;
    if (std::abs(renderCache.lastZoomFactor - zoomFactor) >= 0.01) return true; // Less sensitive to zoom changes
    if (std::abs(renderCache.lastTempoFactor - tempoFactor) >= 0.001) return true;
    
    // More relaxed playhead threshold to reduce rebuilds during playback
    const double playheadThreshold = isInSeekMode ? 0.00001 : 0.0001;
    if (std::abs(renderCache.lastPlayheadPos - renderPlayhead) >= playheadThreshold) return true;
    
    // Check if streaming window changed
    if (renderCache.lastAvailableStartBin != availableStartBin ||
        renderCache.lastAvailableEndBin != availableEndBin) return true;
    
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

    if (geometryNeedsUpdate(widgetWidth, widgetHeight, zoomFactor, renderPlayhead)) {
        buildWaveformGeometry(widgetWidth, widgetHeight, zoomFactor, renderPlayhead);
        updateWaveformVertexBuffers(widgetWidth, widgetHeight);

        const bool geometryReady = geometryCache.valid && glResources.fillVertexCount >= 4;
        renderCache.geometryValid = geometryReady;
        renderCache.needsFullRedraw = !geometryReady;
        renderCache.lastWidth = widgetWidth;
        renderCache.lastHeight = widgetHeight;
        renderCache.lastZoomFactor = zoomFactor;
        renderCache.lastTempoFactor = tempoFactor;
        renderCache.lastPlayheadPos = renderPlayhead;
        renderCache.lastAvailableStartBin = availableStartBin;
        renderCache.lastAvailableEndBin = availableEndBin;
    }

    const bool readyToDraw = renderCache.geometryValid && glResources.fillVertexCount >= 4;
    if (readyToDraw) {
        drawWaveformGl();
        renderCache.needsFullRedraw = false;
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

    glGenBuffers(1, &glResources.fillVbo);
    glGenBuffers(1, &glResources.topLineVbo);
    glGenBuffers(1, &glResources.bottomLineVbo);
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
        if (glResources.fillVbo) {
        glDeleteBuffers(1, &glResources.fillVbo);
        glResources.fillVbo = 0;
        }
        if (glResources.topLineVbo) {
        glDeleteBuffers(1, &glResources.topLineVbo);
        glResources.topLineVbo = 0;
        }
        if (glResources.bottomLineVbo) {
        glDeleteBuffers(1, &glResources.bottomLineVbo);
        glResources.bottomLineVbo = 0;
        }
    }

    glResources.initialized = false;
    glResources.fillVertexCount = 0;
    glResources.topLineVertexCount = 0;
    glResources.bottomLineVertexCount = 0;
}

void WaveformDisplay::updateWaveformVertexBuffers(int viewWidth, int viewHeight)
{
    if (!glResources.initialized || viewWidth <= 0 || viewHeight <= 0) {
        glResources.fillVertexCount = 0;
        glResources.topLineVertexCount = 0;
        glResources.bottomLineVertexCount = 0;
        return;
    }

    if (glResources.fillVbo == 0 || glResources.topLineVbo == 0 || glResources.bottomLineVbo == 0) {
        glResources.fillVertexCount = 0;
        glResources.topLineVertexCount = 0;
        glResources.bottomLineVertexCount = 0;
        return;
    }

    const size_t pointCount = std::min(upperPointBuffer.size(), lowerPointBuffer.size());
    if (pointCount < 2) {
        glResources.fillVertexCount = 0;
        glResources.topLineVertexCount = 0;
        glResources.bottomLineVertexCount = 0;
        return;
    }

    fillVertexData.clear();
    topLineVertexData.clear();
    bottomLineVertexData.clear();

    fillVertexData.reserve(pointCount * 4);
    topLineVertexData.reserve(pointCount * 2);
    bottomLineVertexData.reserve(pointCount * 2);

    const double invWidth = viewWidth > 0 ? 1.0 / static_cast<double>(viewWidth) : 0.0;
    const double invHeight = viewHeight > 0 ? 1.0 / static_cast<double>(viewHeight) : 0.0;

    auto toNdc = [&](const QPointF& pt) -> std::pair<float, float> {
        const double normX = pt.x() * invWidth;
        const double normY = pt.y() * invHeight;
        const float x = static_cast<float>(normX * 2.0 - 1.0);
        const float y = static_cast<float>(1.0 - normY * 2.0);
        return {x, y};
    };

    for (size_t i = 0; i < pointCount; ++i) {
        const auto upper = toNdc(upperPointBuffer[i]);
        const auto lower = toNdc(lowerPointBuffer[i]);

        fillVertexData.push_back(upper.first);
        fillVertexData.push_back(upper.second);
        fillVertexData.push_back(lower.first);
        fillVertexData.push_back(lower.second);

        topLineVertexData.push_back(upper.first);
        topLineVertexData.push_back(upper.second);
        bottomLineVertexData.push_back(lower.first);
        bottomLineVertexData.push_back(lower.second);
    }

    auto upload = [this](GLuint vbo, const std::vector<float>& data) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                     data.data(),
                     GL_DYNAMIC_DRAW);
    };

    upload(glResources.fillVbo,      fillVertexData);
    upload(glResources.topLineVbo,   topLineVertexData);
    upload(glResources.bottomLineVbo,bottomLineVertexData);

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glResources.fillVertexCount = static_cast<int>(fillVertexData.size() / 2);
    glResources.topLineVertexCount = static_cast<int>(topLineVertexData.size() / 2);
    glResources.bottomLineVertexCount = static_cast<int>(bottomLineVertexData.size() / 2);
}

void WaveformDisplay::drawWaveformGl()
{
    if (!glResources.initialized || glResources.fillVertexCount < 4) return;

    if (!glResources.vao.isCreated()) {
        if (!glResources.vao.create()) {
            qWarning() << "WaveformDisplay: Failed to create VAO";
            return;
        }
    }

    QOpenGLVertexArrayObject::Binder vaoBinder(&glResources.vao);

    glBindBuffer(GL_ARRAY_BUFFER, glResources.fillVbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(float) * 2), nullptr);
    glEnableVertexAttribArray(0);

    auto& fillProgram = glResources.fillProgram;
    fillProgram.bind();
    static const QVector4D topColor(110.0f/255.0f, 190.0f/255.0f, 255.0f/255.0f, 0.62f);
    static const QVector4D midColor(60.0f/255.0f, 140.0f/255.0f, 220.0f/255.0f, 0.40f);
    static const QVector4D bottomColor(110.0f/255.0f, 190.0f/255.0f, 255.0f/255.0f, 0.62f);
    fillProgram.setUniformValue("uColorTop", topColor);
    fillProgram.setUniformValue("uColorMid", midColor);
    fillProgram.setUniformValue("uColorBottom", bottomColor);
    fillProgram.setUniformValue("uMidStrength", 0.75f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, glResources.fillVertexCount);
    fillProgram.release();

    if (glResources.topLineVertexCount >= 2) {
        auto& lineProgram = glResources.lineProgram;
        lineProgram.bind();

        float outlineWidth = 1.0f;
        const double zoom = geometryCache.lastZoomFactor;
        if (zoom > 8.0) {
            outlineWidth = 2.0f;
        } else if (zoom > 4.0) {
            outlineWidth = 1.5f;
        } else if (zoom < 0.5) {
            outlineWidth = 0.8f;
        }
        const float glowWidth = std::max(3.0f, outlineWidth * 3.0f);

        static const QVector4D glowColor(100.0f/255.0f, 210.0f/255.0f, 255.0f/255.0f, 0.31f);
        static const QVector4D lineColor(130.0f/255.0f, 210.0f/255.0f, 255.0f/255.0f, 0.86f);

        auto drawStrip = [&](GLuint vbo, int count) {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(float) * 2), nullptr);
            glDrawArrays(GL_LINE_STRIP, 0, count);
        };

        glLineWidth(glowWidth);
        lineProgram.setUniformValue("uColor", glowColor);
        drawStrip(glResources.topLineVbo, glResources.topLineVertexCount);
        drawStrip(glResources.bottomLineVbo, glResources.bottomLineVertexCount);

        glLineWidth(outlineWidth);
        lineProgram.setUniformValue("uColor", lineColor);
        drawStrip(glResources.topLineVbo, glResources.topLineVertexCount);
        drawStrip(glResources.bottomLineVbo, glResources.bottomLineVertexCount);

        lineProgram.release();
        glLineWidth(1.0f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(0);
}

void WaveformDisplay::drawMissingSegments(QPainter& painter, int viewWidth, int viewHeight) const
{
    if (missingSegmentBuffer.empty()) {
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
        if (streamingMode && !streamingComplete) {
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

        if (ghostLoopEnabled && audioLength > 0.0 && ghostLoopEndSec > ghostLoopStartSec) {
            drawGhostLoopRegion(painter, geometryCache.leftSecond, geometryCache.rightSecond, geometryCache.timeRange);
        }

        if (prerollEnabled && geometryCache.leftSecond < 0.0) {
            drawPrerollRegion(painter, geometryCache.leftSecond, geometryCache.rightSecond, geometryCache.timeRange);
        }

        if (loopEnabled && loopEndSec > loopStartSec && audioLength > 0.0) {
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
    renderCache.geometryValid = false;
    renderCache.needsFullRedraw = true;
    invalidateChunkCache();
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

    resetStreamingState();

    sourceMaxBins = maxBins;
    sourceMinBins = minBins;
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

    postSourceConfigured();
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

    const int chunkSize = static_cast<int>(maxBins.size());
    const int chunkEnd = startBin + chunkSize;

    const int prevStart = availableStartBin;
    const int prevEnd = availableEndBin;
    const bool hadData = !sourceMaxBins.empty();

    if (!hadData) {
        sourceMaxBins = maxBins;
        sourceMinBins = minBins;
        availableStartBin = startBin;
        availableEndBin = chunkEnd;
    } else {
        int newStart = std::min(availableStartBin, startBin);
        int newEnd = std::max(availableEndBin, chunkEnd);

        if (newStart < availableStartBin) {
            const int prepend = availableStartBin - newStart;
            sourceMaxBins.insert(sourceMaxBins.begin(), prepend, 0.0f);
            sourceMinBins.insert(sourceMinBins.begin(), prepend, 0.0f);
            availableStartBin = newStart;
        }

        if (newEnd > availableEndBin) {
            const int append = newEnd - availableEndBin;
            sourceMaxBins.insert(sourceMaxBins.end(), append, 0.0f);
            sourceMinBins.insert(sourceMinBins.end(), append, 0.0f);
            availableEndBin = newEnd;
        }

        const int offset = startBin - availableStartBin;
        if (offset >= 0 && offset + chunkSize <= static_cast<int>(sourceMaxBins.size())) {
            std::copy(maxBins.begin(), maxBins.end(), sourceMaxBins.begin() + offset);
            std::copy(minBins.begin(), minBins.end(), sourceMinBins.begin() + offset);
        }
    }

    int cachedBins = availableEndBin - availableStartBin;
    if (streamingMaxCacheBins > 0 && cachedBins > streamingMaxCacheBins) {
        int trimNeeded = cachedBins - streamingMaxCacheBins;
        trimNeeded = std::min(trimNeeded, cachedBins);
        if (trimNeeded > 0) {
            bool extendedForward = chunkEnd > prevEnd;
            bool extendedBackward = startBin < prevStart;

            int trimFront = 0;
            int trimBack = 0;

            if (extendedForward && !extendedBackward) {
                trimFront = trimNeeded;
            } else if (extendedBackward && !extendedForward) {
                trimBack = trimNeeded;
            } else {
                trimFront = trimNeeded / 2;
                trimBack = trimNeeded - trimFront;
            }

            if (trimFront > 0) {
                trimFront = std::min(trimFront, availableEndBin - availableStartBin);
                sourceMaxBins.erase(sourceMaxBins.begin(), sourceMaxBins.begin() + trimFront);
                sourceMinBins.erase(sourceMinBins.begin(), sourceMinBins.begin() + trimFront);
                availableStartBin += trimFront;
            }

            if (trimBack > 0) {
                trimBack = std::min(trimBack, availableEndBin - availableStartBin);
                sourceMaxBins.erase(sourceMaxBins.end() - trimBack, sourceMaxBins.end());
                sourceMinBins.erase(sourceMinBins.end() - trimBack, sourceMinBins.end());
                availableEndBin -= trimBack;
            }
        }
        cachedBins = availableEndBin - availableStartBin;
    }

    streamingExpectedNextBin = availableEndBin;
    streamingComplete = streamingComplete || isFinalChunk;

    if (hasPendingRegionRequest) {
        if (pendingRequestStartBin >= availableStartBin && pendingRequestEndBin <= availableEndBin) {
            hasPendingRegionRequest = false;
        }
    }

    markDirtyAndSchedule();
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
    if (!std::isfinite(relative)) {
        return;
    }
    
    const double seekThreshold = 0.05;
    double positionDelta = std::abs(relative - playheadPos);
    const auto now = std::chrono::steady_clock::now();
    bool treatedAsSeek = false;
    
    if (positionDelta > seekThreshold && audioLength > 0.0) {
        isInSeekMode = true;
        lastSeekPosition = relative;
        lastSeekTime = std::chrono::steady_clock::now();
        treatedAsSeek = true;
        
        // Immediately request a large region around the jump position
        if (streamingMode && sourceWidth > 0) {
            double binPerSecond = static_cast<double>(sourceWidth) / audioLength;
            double seekSeconds = relative * audioLength;
            int seekBin = static_cast<int>(seekSeconds * binPerSecond);
            
            // Request a much larger window for smooth playback after jumping
            int windowSize = streamingPreloadBins * 6; // 6x larger for jumps
            int startBin = std::max(0, seekBin - windowSize);
            int endBin = std::min(sourceWidth, seekBin + windowSize);
            
            // Force immediate request by clearing pending state
            hasPendingRegionRequest = false;
            requestStreamingWindowIfNeeded(startBin, endBin, binPerSecond);
        }
        
        // Invalidate geometry cache to force immediate rebuild at new position
        geometryCache.valid = false;
        renderCache.geometryValid = false;
        renderCache.needsFullRedraw = true;
    } else {
        auto now = std::chrono::steady_clock::now();
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
        if (!std::isfinite(dt) || dt <= 0.0) {
            dt = 0.0;
        }

        if (dt > 1e-5 && std::isfinite(lastReportedPlayhead)) {
            double rawRate = (relative - lastReportedPlayhead) / dt;
            const double maxRate = 12.0; // allow up to 12x for scratching jogs
            if (std::isfinite(rawRate)) {
                rawRate = std::clamp(rawRate, -maxRate, maxRate);
                const double rateBlend = std::clamp(dt * 8.0, 0.0, 1.0);
                if (std::isfinite(estimatedPlaybackRate)) {
                    estimatedPlaybackRate += (rawRate - estimatedPlaybackRate) * rateBlend;
                } else {
                    estimatedPlaybackRate = rawRate;
                }
            }
        }

        const double transportRate = std::isfinite(estimatedPlaybackRate) ? estimatedPlaybackRate : 0.0;
        const double directionalJitter = 0.00012; // filter tiny transport jitter when running
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
    estimatedPlaybackRate = 0.0;
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

    const int stride = std::max(1, totalBins / 1000);
    double sumSq = 0.0;
    int sampleCount = 0;
    for (int i = 0; i < totalBins; i += stride) {
        float maxVal = sourceMaxBins[i];
        float minVal = sourceMinBins[i];
        float peak = std::max(std::abs(minVal), std::abs(maxVal));
        sumSq += peak * peak;
        ++sampleCount;
    }

    if (sampleCount > 0) {
        double rms = std::sqrt(sumSq / sampleCount);
        if (rms > 0.05) {
            chunkNormalizationFactor = static_cast<float>(0.8 / rms);
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

            minVal *= chunkNormalizationFactor;
            maxVal *= chunkNormalizationFactor;
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

    const double velocityMag = std::abs(playVelocity);
    double catchHz = (velocityMag > 1e-4 ? 22.0 : 14.0) + std::min(velocityMag * 18.0, 24.0);
    if (isInSeekMode) {
        catchHz = std::max(catchHz, 36.0);
    }

    double correction = error * catchHz * frameDt;
    double baseCatchPerSec = isInSeekMode ? 2.4 : (0.24 + velocityMag * 0.4);
    double minCatchPerFrame = isInSeekMode ? 0.0004 : 0.00008;
    double maxCatch = std::max(minCatchPerFrame, baseCatchPerSec * frameDt);
    correction = std::clamp(correction, -maxCatch, maxCatch);

    double newVisual = predicted + correction;

    if (std::abs(error) < 1e-5) {
        newVisual = predicted + error * 0.35; // leave a tiny bias so we bleed off any residual drift
    }

    if (!std::isfinite(newVisual)) {
        newVisual = fallback;
    }

    if (!isInSeekMode) {
        const double slop = 0.00008;
        if (playVelocity > 0.02) {
            newVisual = std::max(newVisual, previousVisual - slop);
        } else if (playVelocity < -0.02) {
            newVisual = std::min(newVisual, previousVisual + slop);
        }
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

    const double displayCenterSeconds = (viewMode == ViewMode::BeatLocked)
        ? (playheadSeconds / safeTempo)
        : playheadSeconds;

    const double bufferSec = std::max(0.05, 0.5 / std::max(1.0, zoomFactor));
    const double halfViewportTime = static_cast<double>(viewportWidth) / (2.0 * pixelsPerSecond);
    const double leftSecond = displayCenterSeconds - halfViewportTime - bufferSec;
    const double rightSecond = displayCenterSeconds + halfViewportTime + bufferSec;

    metrics.playheadSeconds = playheadSeconds;
    metrics.displayCenterSecond = displayCenterSeconds;
    metrics.safeTempo = safeTempo;
    metrics.leftSecond = leftSecond;
    metrics.rightSecond = rightSecond;
    metrics.timeRange = rightSecond - leftSecond;
    metrics.viewportWidth = viewportWidth;
    return metrics.timeRange > 0.0;
}

double WaveformDisplay::secondsAtViewportX(double x) const {
    ViewportMetrics metrics;
    if (!computeViewportMetrics(metrics)) return std::numeric_limits<double>::quiet_NaN();

    const double clampedX = std::clamp(x, 0.0, static_cast<double>(metrics.viewportWidth));
    const double positionRatio = clampedX / static_cast<double>(metrics.viewportWidth);
    const double visualSeconds = metrics.leftSecond + (positionRatio * metrics.timeRange);

    if (viewMode == ViewMode::BeatLocked) {
        const double deltaVis = visualSeconds - metrics.displayCenterSecond;
        return metrics.playheadSeconds + (deltaVis * metrics.safeTempo);
    }
    return visualSeconds;
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

    // Larger preload buffer for better jump performance
    const int largerPreload = streamingPreloadBins * 3;
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

    // Check if we already have this region
    if (desiredStart >= availableStartBin && desiredEnd <= availableEndBin) return;

    const auto now = std::chrono::steady_clock::now();
    
    // Immediate request on position jumps (seek mode)
    const bool isJump = std::abs(neededStartBin - availableEndBin) > streamingPreloadBins * 2;
    
    if (hasPendingRegionRequest && !isJump) {
        const bool coversPending = desiredStart >= pendingRequestStartBin && desiredEnd <= pendingRequestEndBin;
        const auto sinceLast = now - lastRegionRequestTime;
        // Shorter debounce for better responsiveness
        if (coversPending && sinceLast < std::chrono::milliseconds(50)) return;
    }

    const double startSec = audioStartOffset + (static_cast<double>(desiredStart) / binPerSecond);
    const double endSec = audioStartOffset + (static_cast<double>(desiredEnd) / binPerSecond);

    hasPendingRegionRequest = true;
    pendingRequestStartBin = desiredStart;
    pendingRequestEndBin = desiredEnd;
    lastRegionRequestTime = now;

    emit waveformRegionNeeded(startSec, endSec);
}

void WaveformDisplay::resetStreamingState()
{
    availableStartBin = 0;
    availableEndBin = 0;
    streamingMode = false;
    streamingComplete = false;
    streamingTotalBins = 0;
    streamingPreloadBins = 4000;
    streamingMaxCacheBins = 40000;
    streamingExpectedNextBin = 0;
    hasPendingRegionRequest = false;
    pendingRequestStartBin = 0;
    pendingRequestEndBin = 0;
    lastRegionRequestTime = {};
    chunkCache.clear();
    chunkCacheDirty = true;
    chunkNormalizationFactor = 1.0f;
    secondsPerBin = 0.0;
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
    
    if (analysisActive) {
        int percent = static_cast<int>(std::round(analysisProgress * 100.0));
        p.setPen(QPen(QColor(180, 200, 255), 1));
        QString txt = QString("Analyzing %1%").arg(percent);
        int w = p.fontMetrics().horizontalAdvance(txt);
        p.drawText(rightX - w, y, txt);
    } else if (analysisFailed) {
        p.setPen(QPen(QColor(255, 120, 120), 1));
        QString txt("Analysis failed");
        int w = p.fontMetrics().horizontalAdvance(txt);
        p.drawText(rightX - w, y, txt);
    } else if (originalBpm > 0.0) {
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
        
        p.setPen(QPen(kCueColors[i], 2));
        p.drawLine(x, 0, x, widgetHeight);
        
        p.fillRect(x - 3, 5, 6, 20, kCueColors[i]);
        p.setPen(Qt::white);
        p.drawText(x - 10, 20, QString::number(i + 1));
    }
    
    p.restore();
}

// NEW: Draw loop region as semi-transparent box
void WaveformDisplay::drawLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (timeRange <= 0.0 || audioLength <= 0.0) return;
    QColor loopColor(100, 255, 100, 160);
    QPen loopPen(QColor(0, 200, 0, 200), 2.5);
    loopPen.setStyle(Qt::SolidLine);
    drawRangeOverlay(p, loopStartSec, loopEndSec, leftSecond, rightSecond, timeRange,
                     loopColor, loopPen, QStringLiteral("LOOP"), 15, 200);
}

// NEW: Draw ghost loop region as very transparent box for last used loop
void WaveformDisplay::drawGhostLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (!ghostLoopEnabled || timeRange <= 0.0 || audioLength <= 0.0) return;
    QColor ghostColor(100, 255, 100, 20);
    QPen ghostPen(QColor(0, 200, 0, 80), 1.5);
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
    if (rightSecond <= 0.0) {
        double screenStartX = 0;
        double screenEndX = width();
        
        QColor prerollColor(30, 50, 80, 120);
        p.fillRect(QRect(screenStartX, 0, screenEndX - screenStartX, height()), prerollColor);
        
        p.setPen(QPen(QColor(60, 100, 160), 1));
        for (int x = screenStartX; x < screenEndX; x += 20) {
            p.drawLine(x, 0, x + 10, height());
        }
        
        p.setFont(QFont("Arial", 10, QFont::Bold));
        p.setPen(QPen(QColor(120, 180, 255), 1));
        p.drawText(width()/2 - 30, height()/2, "PREROLL");
        
    } else if (leftSecond < 0.0) {
        double prerollRatio = -leftSecond / timeRange;
        int screenEndX = (int)(prerollRatio * width());
        
        QColor prerollColor(30, 50, 80, 120);
        p.fillRect(QRect(0, 0, screenEndX, height()), prerollColor);
        
        p.setPen(QPen(QColor(60, 100, 160), 1));
        for (int x = 0; x < screenEndX; x += 15) {
            p.drawLine(x, 0, x + 8, height());
        }
        
        const double startDisplaySec = mapAudioToDisplay(0.0);
        const double trackStartRatio = (startDisplaySec - geometryCache.leftSecond) / geometryCache.timeRange;
        const int trackStartX = (int)std::round(trackStartRatio * width());
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.drawLine(trackStartX, 0, trackStartX, height());
        
        // Label
        p.setFont(QFont("Arial", 8, QFont::Bold));
        p.setPen(QPen(QColor(120, 180, 255), 1));
        if (screenEndX > 60) {
            p.drawText(10, 20, "PREROLL");
        }
    }
}