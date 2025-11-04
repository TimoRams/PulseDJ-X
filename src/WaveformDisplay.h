#pragma once

#include <QWidget>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QString>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <JuceHeader.h>
#include <array>
#include <span>
#include <vector>
#include <chrono>
#include <utility>
#include <cstdint>
#include <shared_mutex>
#include <mutex>
#include "ScratchEngine.h"

class WaveformDisplay : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    enum class ViewMode { TimeLocked, BeatLocked };
    
    explicit WaveformDisplay(QWidget* parent = nullptr);
    ~WaveformDisplay() override;
    
    void loadFile(const QString& path);
    void setPlayhead(double relative);
    void setBeats(const QVector<double>& beats) {
        beatPositions.clear();
        if (!beats.isEmpty()) {
            beatPositions.reserve(beats.size());
            for (double b : beats) {
                // Accept either relative [0..1] or absolute seconds; normalize to seconds
                double sec = b;
                if (trackLengthSec > 0.0 && b >= 0.0 && b <= 1.05) {
                    sec = b * trackLengthSec;
                }
                beatPositions.append(sec);
            }
            std::sort(beatPositions.begin(), beatPositions.end());
            useAnalyzedBeats = true;
            firstBeatOffset = beatPositions.first();
        } else {
            useAnalyzedBeats = false;
            firstBeatOffset = 0.0;
        }
        recomputeBeatPhaseShift();
        update();
    }
    void setOriginalBpm(double bpm, double trackLengthSeconds);
    void setSourceBins(const std::vector<float>& maxBins,
                       const std::vector<float>& minBins,
                       double audioStartOffsetSec,
                       double lengthSeconds);

    void beginStreaming(int totalBinCount,
                        double audioStartOffsetSec,
                        double lengthSeconds,
                        int preloadBins = 4000,
                        int maxCachedBins = 40000);

    void appendStreamBins(int startBin,
                          const std::vector<float>& maxBins,
                          const std::vector<float>& minBins,
                          bool isFinalChunk = false);

    // Extended streaming method with 3-band energy for colorful rendering
    void appendStreamBins(int startBin,
                          const std::vector<float>& maxBins,
                          const std::vector<float>& minBins,
                          const std::vector<float>& lowBins,
                          const std::vector<float>& midBins,
                          const std::vector<float>& highBins,
                          bool isFinalChunk = false);

    std::pair<int, int> getCachedBinRange() const;
    
    void setBeatInfo(double bpm, double firstBeatOffset, double totalLength) {
        originalBpm = bpm;
        trackLengthSec = totalLength;
        this->firstBeatOffset = firstBeatOffset;
        update();
    }
    
    // Set track length for default beat grid
    void setTrackLength(double lengthSeconds) { trackLengthSec = lengthSeconds; generateDefaultGrid(); }

    void setPrerollEnabled(bool enabled) { prerollEnabled = enabled; update(); }
    void setPrerollTime(double seconds) { prerollTimeSec = seconds; update(); }
    bool isPrerollEnabled() const { return prerollEnabled; }
    // Control whether the visual preroll shading is drawn (does not affect preroll behavior)
    void setPrerollOverlayEnabled(bool enabled) { prerollOverlayEnabled = enabled; update(); }
    bool isPrerollOverlayEnabled() const { return prerollOverlayEnabled; }

    void setTempoFactor(double factor) { 
        tempoFactor = factor; 
        update();
    }

    void setScrollMode(bool enabled) {
        if (scrollMode == enabled)
            return;
        scrollMode = enabled;
        updateRenderActivity();
        update();
    }

    void updateTempo(double newBpm);
    void refreshBeatGrid();
    
    void setCuePoints(const std::array<double, 8>& cuePoints);
    void clearCuePoints();
    
    void setLoopRegion(bool enabled, double startSec = 0.0, double endSec = 0.0);
    void clearLoop();
    void setLoopOverlayEnabled(bool enabled) { loopOverlayEnabled = enabled; update(); }
    bool isLoopOverlayEnabled() const { return loopOverlayEnabled; }
    
    void setGhostLoopRegion(bool enabled, double startSec = 0.0, double endSec = 0.0);
    void setGhostLoopOverlayEnabled(bool enabled) { ghostLoopOverlayEnabled = enabled; update(); }
    bool isGhostLoopOverlayEnabled() const { return ghostLoopOverlayEnabled; }

    void clearDisplay();

    void setScratchEngine(ScratchEngine* engine);
    ScratchEngine* getScratchEngine() const { return scratchEngine; }

    double trackLengthSec{0.0};
    double originalBpm{120.0};

    void increaseBeatGridZoom();
    void decreaseBeatGridZoom();
    void resetBeatGridZoom();
    void setBeatGridZoomLevel(int level);
    int getBeatGridZoomLevel() const { return beatGridZoomLevel; }
    double getBeatGridZoomFactor() const { return beatGridZoomFactors[beatGridZoomLevel]; }
    
    bool isScratching() const { return scratching; }
    double getLastScratchVelocity() const { return lastScratchVelocity; }
    double getPlayheadRelative() const { return playheadPos; }
    double getPrerollTimeSeconds() const { return prerollTimeSec; }
    
    void setUseFixedPixelsPerSecond(bool use) { useFixedPixelsPerSecond = use; update(); }
    bool isUsingFixedPixelsPerSecond() const { return useFixedPixelsPerSecond; }
    void setPixelsPerSecond(double pixelsPerSec) { 
        localPixelsPerSecond = pixelsPerSec;
        update(); 
    }
    double getPixelsPerSecond() const { return localPixelsPerSecond; }
    void setViewMode(ViewMode m) { viewMode = m; update(); }
    ViewMode getViewMode() const { return viewMode; }
    void setVisualLatencyComp(double seconds) { visualLatencyComp = std::clamp(seconds, -0.25, 0.25); }
    // Compensate for audio output latency so the visual center matches what you hear.
    // Positive values shift the waveform/grid to the right.
    void setOutputLatencyComp(double seconds) { renderLatencySec = std::clamp(seconds, -0.25, 0.25); update(); }
    double getOutputLatencyComp() const { return renderLatencySec; }

    void setAnalysisActive(bool active) {
        analysisActive = active;
        updateRenderActivity();
        update();
    }
    void setAnalysisProgress(double p) {
        analysisProgress = std::clamp(p, 0.0, 1.0);
        update();
    }
    void setAnalysisFailed(bool failed) {
        analysisFailed = failed;
        updateRenderActivity();
        update();
    }

signals:
    void positionClicked(double relative);
    void scratchStart();
    void scratchMove(double relative);
    void scratchEnd();
    void scratchVelocityChanged(double velocity); // Signal for scratch speed
    void tempoDragRequested(double factor);
    void tempoDragEnded();
    void zoomLevelChanged(int newLevel);
    void waveformRegionNeeded(double startSeconds, double endSeconds);

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glResources.initialized = false;
        glResources.fillProgram.removeAllShaders();
        glResources.lineProgram.removeAllShaders();
        if (glResources.vao.isCreated()) {
            glResources.vao.destroy();
        }
        glResources.fillVbo[0] = glResources.fillVbo[1] = 0;
        glResources.topLineVbo[0] = glResources.topLineVbo[1] = 0;
        glResources.bottomLineVbo[0] = glResources.bottomLineVbo[1] = 0;
        glResources.frontIndex = 0;
        glResources.fillVertexCountFront = 0;
        glResources.topLineVertexCountFront = 0;
        glResources.bottomLineVertexCountFront = 0;
        glResources.usingGles = context() ? context()->isOpenGLES() : false;
        renderCache.geometryValid = false;
        renderCache.needsFullRedraw = true;
    }
    void resizeGL(int w, int h) override { Q_UNUSED(w); Q_UNUSED(h); }
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    QSize sizeHint() const override { return QSize(1100, 240); }
    QSize minimumSizeHint() const override { return QSize(700, 160); }
    void resizeEvent(QResizeEvent* event) override {
        QOpenGLWidget::resizeEvent(event);
    }

private:
    struct ViewportMetrics {
        double playheadSeconds{0.0};
        double safeTempo{1.0};
        double leftSecond{0.0};
        double rightSecond{0.0};
        double fetchLeftSecond{0.0};
        double fetchRightSecond{0.0};
        double displayCenterSecond{0.0};
        double timeRange{0.0};
        double waveformNudgeSec{0.0};
        int viewportWidth{0};
    };

    bool computeViewportMetrics(ViewportMetrics& metrics) const;
    double secondsAtViewportX(double x) const;
    double secondsToRelative(double seconds) const;
    bool seekToMousePosition(double x);
    void updateTempoDragFromMouse(double x);
    void requestStreamingWindowIfNeeded(int neededStartBin, int neededEndBin, double binPerSecond);
    void resetStreamingState();
    void markDirtyAndSchedule();
    void postSourceConfigured();
    void appendStreamBinsImpl(std::unique_lock<std::shared_mutex>& lock,
                              int startBin,
                              std::span<const float> maxBins,
                              std::span<const float> minBins,
                              std::span<const float> lowBins,
                              std::span<const float> midBins,
                              std::span<const float> highBins,
                              bool isFinalChunk);
    
    void buildWaveformGeometry(int viewWidth, int viewHeight, double zoomFactor, double renderPlayheadRel);
    double acquireVisualPlayhead();
    void resetVisualPlayhead(double relative);
    void updateRenderActivity();
    void invalidateChunkCache();
    void rebuildChunkCacheIfNeeded();
    void rebuildChunkCacheIfNeeded_Locked(std::unique_lock<std::shared_mutex>& lock);
    void ensureGlResources();
    void destroyGlResources();
    bool compileWaveformShaders();
    bool geometryNeedsUpdate(int viewWidth, int viewHeight, double zoomFactor, double renderPlayhead) const;
    void updateWaveformVertexBuffers(int viewWidth, int viewHeight);
    void drawWaveformGl();
    void drawWaveformOverlays(QPainter& painter, int viewWidth, int viewHeight, double zoomFactor);
    void drawMissingSegments(QPainter& painter, int viewWidth, int viewHeight) const;

private:
    juce::AudioFormatManager formatManager;
    QString currentFilePath;
    double playheadPos{-1.0};
    double visualPlayheadPos{-1.0};
    double targetPlayheadPos{-1.0};
    double lastReportedPlayhead{-1.0};
    double estimatedPlaybackRate{0.0};
    double activeRenderPlayhead{-1.0};
    std::chrono::steady_clock::time_point lastPlayheadUpdateTime{};
    std::chrono::steady_clock::time_point lastVisualUpdateTime{};
    bool visualPlayheadInitialized{false};
    bool scrollMode{false};
    
    std::vector<float> sourceMaxBins;
    std::vector<float> sourceMinBins;
    // Optional per-bin 3-band RMS energies (low/mid/high) aligned with source bins
    std::vector<float> sourceLowBins;
    std::vector<float> sourceMidBins;
    std::vector<float> sourceHighBins;
    mutable std::shared_mutex sourceMutex;
    int sourceWidth{0};
    double audioLength{0.0};
    int availableStartBin{0};
    int availableEndBin{0};
    bool streamingMode{false};
    bool streamingComplete{false};
    int streamingTotalBins{0};
    int streamingPreloadBins{4000};
    int streamingMaxCacheBins{40000};
    
    // === ADAPTIVE CHUNK SYSTEM ===
    struct AdaptiveChunk {
        int startBin{0};
        int endBin{0};
        std::vector<float> maxBins;
        std::vector<float> minBins;
        std::vector<float> lowBins;  // Band energies for spectrum coloring
        std::vector<float> midBins;
        std::vector<float> highBins;
        double lastAccessTime{0.0};
        int priority{3}; // 0=ULTRA(128), 1=HIGH(512), 2=MEDIUM(2048), 3=LOW(8192)
    };
    std::vector<AdaptiveChunk> adaptiveChunks_;
    mutable std::mutex adaptiveChunksMutex_; // Thread-safety for chunk access
    int maxAdaptiveChunks_{500}; // Keep many chunks cached for fast seeking
    bool useAdaptiveChunking_{true}; // Re-enabled with optimizations
    
    // === FULL-SONG FALLBACK WAVEFORM (low-res red baseline) ===
    std::vector<float> fallbackMaxBins_;
    std::vector<float> fallbackMinBins_;
    mutable std::mutex fallbackMutex_;
    bool fallbackComplete_{false};
    
    int streamingExpectedNextBin{0};
    bool hasPendingRegionRequest{false};
    int pendingRequestStartBin{0};
    int pendingRequestEndBin{0};
    std::chrono::steady_clock::time_point lastRegionRequestTime{};
    
    std::array<double, 8> cuePoints;
    bool cuePointsValid{false};
    
    bool loopEnabled{false};
    double loopStartSec{0.0};
    double loopEndSec{0.0};
    bool loopOverlayEnabled{true};
    
    bool ghostLoopEnabled{false};
    double ghostLoopStartSec{0.0};
    double ghostLoopEndSec{0.0};
    bool ghostLoopOverlayEnabled{true};

    ScratchEngine* scratchEngine{nullptr};
    std::vector<QMetaObject::Connection> scratchEngineConnections;

    void applyScratchResult(const ScratchEngine::UpdateResult& result);
    double relativeToSeconds(double relative) const;
    
    QTimer* renderTimer{nullptr};
    
    struct RenderCache {
        double lastPlayheadPos{-1.0};
        double lastTempoFactor{1.0};
        double lastZoomFactor{1.0};
        int lastWidth{0};
        int lastHeight{0};
        int lastAvailableStartBin{0};
        int lastAvailableEndBin{0};
        bool geometryValid{false};
        bool needsFullRedraw{true};
        bool needsUpdate{false}; // Merker um vorhandene Geometrie weiter zu nutzen, bis frische Daten angekommen sind
        std::chrono::steady_clock::time_point lastUpdate;
    } renderCache;

    struct GeometryCache {
        bool valid{false};
        int lastWidth{0};
        int lastHeight{0};
        double lastZoomFactor{1.0};
        double lastTempoFactor{1.0};
        double lastPlayheadPos{-1.0};
        int lastAvailableStartBin{0};
        int lastAvailableEndBin{0};
        double playheadSec{0.0};
        double displayCenterSec{0.0};
        double leftSecond{0.0};
        double rightSecond{0.0};
        double timeRange{0.0};
        double halfViewportTime{0.0};
        double bufferSec{0.0};
        double fetchLeftSecond{0.0};
        double fetchRightSecond{0.0};
        double waveformNudgeSec{0.0};
        // Optional visual alignment shift (in display seconds) applied to audio->display mapping for this frame.
        // Used to ensure that at playhead==0 the very start of the track (audioSec=0) is exactly centered.
        double alignShiftSec{0.0};
    } geometryCache;

    // Hysteresis latch to stabilize alignment near track start (prevents tiny oscillations)
    bool alignZeroLatchActive{false};
    double alignZeroShiftSec{0.0};

    struct GlResources {
        bool initialized{false};
        bool usingGles{false};
        QOpenGLVertexArrayObject vao;
        GLuint fillVbo[2]{0,0};
        GLuint topLineVbo[2]{0,0};
        GLuint bottomLineVbo[2]{0,0};
        int frontIndex{0};
        int fillVertexCountFront{0};
        int topLineVertexCountFront{0};
        int bottomLineVertexCountFront{0};
        QOpenGLShaderProgram fillProgram;
        QOpenGLShaderProgram lineProgram;
    } glResources;

    std::vector<float> fillVertexData;
    std::vector<float> topLineVertexData;
    std::vector<float> bottomLineVertexData;
    
    double lastSeekPosition{-1.0};
    std::chrono::steady_clock::time_point lastSeekTime{};
    bool isInSeekMode{false};
    
    mutable std::vector<QPointF> upperPointBuffer;
    mutable std::vector<QPointF> lowerPointBuffer;
    mutable std::vector<std::pair<int, int>> missingSegmentBuffer;
    mutable std::vector<float> pixelUpperScratch;
    mutable std::vector<float> pixelLowerScratch;
    mutable std::vector<std::uint8_t> pixelCoverageScratch;
    // Per-pixel color (R,G,B) computed from band energies
    mutable std::vector<float> pixelColorScratch;
    // Per-pixel persistence so transient streaming gaps reuse the last good geometry
    mutable std::vector<float> pixelUpperHistory;
    mutable std::vector<float> pixelLowerHistory;
    mutable std::vector<float> pixelColorHistory;
    mutable std::vector<double> pixelCenterHistory;
    mutable std::vector<std::uint8_t> pixelHistoryValid;

    struct WaveformChunk {
        int startBin{0};
        int endBin{0};
        double startTimeSec{0.0};
        double sampleDurationSec{0.0};
        std::vector<float> upper;
        std::vector<float> lower;
    };

    std::vector<WaveformChunk> chunkCache;
    bool chunkCacheDirty{true};
    float chunkNormalizationFactor{1.0f};
    double secondsPerBin{0.0};
    static constexpr int chunkBinSize{2048};
    static constexpr int chunkSampleResolution{256};
    
    QVector<double> beatPositions;
    double tempoFactor{1.0};
    bool useAnalyzedBeats{false};
    double firstBeatOffset{0.0};
    
    int beatGridZoomLevel{4};
    static constexpr double beatGridZoomFactors[10] = {0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0};
    
    bool useFixedPixelsPerSecond{true};
    ViewMode viewMode{ViewMode::BeatLocked};
    double visualLatencyComp{0.0};
    // Static audio output latency compensation (seconds). Shifts display center relative to playhead.
    double renderLatencySec{0.03};

    bool analysisActive{false};
    double analysisProgress{0.0};
    bool analysisFailed{false};
    
    double audioStartOffset{0.0};
    double beatPhaseShiftSec{0.0};
    int manualBeatLineOffsetBeats{-1};
    
    bool scratching{false};
    bool seekActive{false};
    bool tempoDragActive{false};
    double scratchVelocity{0.0};
    double scratchLastSeconds{0.0};
    double lastScratchVelocity{0.0};
    
    double localPixelsPerSecond{100.0}; // Local pixels per second instead of global
    double tempoDragAnchorX{0.0};
    double tempoDragAnchorFactor{1.0};

    static constexpr double tempoDragDeadZonePx{1.5};
    static constexpr double tempoDragNormalizedSensitivity{1.0};
    static constexpr double tempoDragMinFactor{0.5};
    static constexpr double tempoDragMaxFactor{1.5};
    
    bool prerollEnabled{true};
    double prerollTimeSec{8.0};

    // Preroll overlay state to avoid edge flicker: simple Schmitt trigger + last edge cache
    bool prerollVisible{false};
    // Show overlay when strictly below this negative-time threshold; hide when above hide threshold
    double prerollShowThresholdSec{0.06}; // 60 ms
    double prerollHideThresholdSec{0.02}; // 20 ms
    int lastPrerollEdgeX{-1};

    // Global toggle for drawing the preroll overlay; keep behavior but hide visuals by default
    bool prerollOverlayEnabled{false};

    // Optional: overlay for streaming gaps (blue loading stripes) – disabled by default
public:
    void setMissingSegmentsOverlayEnabled(bool enabled) { missingSegmentsOverlayEnabled = enabled; update(); }
    bool isMissingSegmentsOverlayEnabled() const { return missingSegmentsOverlayEnabled; }

private:
    bool missingSegmentsOverlayEnabled{false};
    
    void loadAndRenderWaveform();
    void generateDefaultGrid();
    void recomputeBeatPhaseShift();
    double mapXToAbsRel(double x) const;
    
    void drawBeatGrid(QPainter& p, double playheadSec, double displayCenterSec,
                      double leftSecond, double rightSecond, double timeRange);
    
    void drawCuePoints(QPainter& p, double leftSecond, double rightSecond, double timeRange);
    
    void drawLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange);
    
    void drawPrerollRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange);
    
    void drawGhostLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange);

    // Generic overlay drawer to reduce duplication between loop and ghost loop
    void drawRangeOverlay(QPainter& p,
                          double audioStartSec,
                          double audioEndSec,
                          double leftSecond,
                          double rightSecond,
                          double timeRange,
                          const QColor& boxColor,
                          const QPen& boundaryPen,
                          const QString& label,
                          int labelY,
                          int labelBgAlpha);

    // Mapping helpers to keep audio/display transforms consistent everywhere
    double mapAudioToDisplay(double audioSec) const;
    double mapDisplayToAudio(double displaySec) const;
};