#pragma once

#include <QWidget>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <JuceHeader.h>
#include <vector>
#include "GlobalBeatGrid.h"

class WaveformDisplay : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    enum class ViewMode { TimeLocked, BeatLocked };
    explicit WaveformDisplay(QWidget* parent = nullptr);
    void loadFile(const QString& path);
    // update playhead position (0.0-1.0)
    void setPlayhead(double relative);
    // Set beat positions in seconds relative to current file length (0.0 - 1.0 positions expected)
    void setBeats(const QVector<double>& beats) { 
        beatPositions = beats; 
        useAnalyzedBeats = !beats.isEmpty(); 
        // Capture first beat offset for use in default grid
        if (!beats.isEmpty()) {
            firstBeatOffset = beats.first() * trackLengthSec;
            // Keep global grid aligned with analysis: bpm + firstBeatOffset + track length
            GlobalBeatGrid::getInstance().setBeatGridParams(originalBpm, firstBeatOffset, trackLengthSec);
        }
        // Re-align the grid after new beats are provided
        recomputeBeatPhaseShift();
        update(); 
    }
    // Set original BPM from analysis to generate adaptive beat grid
    void setOriginalBpm(double bpm, double trackLengthSeconds);
    // New: accept precomputed high-res bins from a background task
    void setSourceBins(const std::vector<float>& maxBins,
                       const std::vector<float>& minBins,
                       double audioStartOffsetSec,
                       double lengthSeconds) {
        sourceMaxBins = maxBins;
        sourceMinBins = minBins;
        sourceWidth = (int) maxBins.size();
        audioStartOffset = audioStartOffsetSec;
        audioLength = lengthSeconds;
        // Ensure track metadata and initial viewport are valid before playback
        trackLengthSec = lengthSeconds;
        if (playheadPos < 0.0) {
            // Default to start so the waveform is visible immediately
            playheadPos = 0.0;
        }
    // New bins imply a new track; wait for analysis before drawing beat grid
    useAnalyzedBeats = false;
    beatPositions.clear();
        waveformImage = QImage(); // ensure we render from source bins
        update();
    }
    
    // NEW: Set beat info with global beat grid integration
    void setBeatInfo(double bpm, double firstBeatOffset, double totalLength) {
        originalBpm = bpm;
        trackLengthSec = totalLength;
        
        // Update global beat grid
        GlobalBeatGrid::getInstance().setBeatGridParams(bpm, firstBeatOffset, totalLength);
        
        update();
    }
    
    // Set track length for default beat grid
    void setTrackLength(double lengthSeconds) { trackLengthSec = lengthSeconds; generateDefaultGrid(); }

    // Set tempo factor to adjust beat grid timing (deck-specific)
    void setTempoFactor(double factor) { 
    // Update tempo factor immediately for visual sync (no threshold to reflect tiny slider steps)
    tempoFactor = factor; 
    update(); // Direct update without throttling for sync responsiveness
    }

    // Scrolling overview mode: keep playhead centered and scroll waveform
    void setScrollMode(bool enabled) { scrollMode = enabled; }

    // PUBLIC: Method to update tempo without changing track length
    void updateTempo(double newBpm);
    
    // HELPER: Method to refresh beat grid when external tempo changes occur
    void refreshBeatGrid();
    
    // NEW: Cue points support
    void setCuePoints(const std::array<double, 8>& cuePoints);
    void clearCuePoints();
    
    // NEW: Loop visualization support  
    void setLoopRegion(bool enabled, double startSec = 0.0, double endSec = 0.0);
    void clearLoop();
    
    // NEW: Ghost loop visualization support
    void setGhostLoopRegion(bool enabled, double startSec = 0.0, double endSec = 0.0);

    // Public access to track length and original BPM for external updates
    double trackLengthSec{0.0};
    double originalBpm{120.0}; // Public access for label updates

    // Beat grid zoom controls
    void increaseBeatGridZoom(); // Zoom in (more beats visible)
    void decreaseBeatGridZoom(); // Zoom out (fewer beats visible)
    void resetBeatGridZoom();    // Reset to standard (1x)
    void setBeatGridZoomLevel(int level); // Set zoom level directly (for synchronization)
    int getBeatGridZoomLevel() const { return beatGridZoomLevel; }
    double getBeatGridZoomFactor() const { return beatGridZoomFactors[beatGridZoomLevel]; }
    
    // NEW: Fixed pixels-per-second system for consistent beat grid display
    void setUseFixedPixelsPerSecond(bool use) { useFixedPixelsPerSecond = use; update(); }
    bool isUsingFixedPixelsPerSecond() const { return useFixedPixelsPerSecond; }
    void setPixelsPerSecond(double pixelsPerSec) { 
        GlobalBeatGrid::getInstance().setPixelsPerSecond(pixelsPerSec); 
        update(); 
    }
    double getPixelsPerSecond() const { return GlobalBeatGrid::getInstance().getPixelsPerSecond(); }
    // View mode control
    void setViewMode(ViewMode m) { viewMode = m; update(); }
    ViewMode getViewMode() const { return viewMode; }
    // Visual latency compensation in seconds (UI leads audio by this amount)
    void setVisualLatencyComp(double seconds) { visualLatencyComp = std::clamp(seconds, -0.25, 0.25); }

    // Analysis progress/error UI hooks
    void setAnalysisActive(bool active) { analysisActive = active; update(); }
    void setAnalysisProgress(double p) { analysisProgress = std::clamp(p, 0.0, 1.0); update(); }
    void setAnalysisFailed(bool failed) { analysisFailed = failed; update(); }

signals:
    void positionClicked(double relative);
    void scratchStart();
    void scratchMove(double relative);
    void scratchEnd();
    void scratchVelocityChanged(double velocity); // Signal for scratch speed
    void zoomLevelChanged(int newLevel); // Signal when zoom level changes

protected:
    // OpenGL-backed rendering for ultra-smooth visuals
    void initializeGL() override {
        initializeOpenGLFunctions();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    void resizeGL(int w, int h) override { Q_UNUSED(w); Q_UNUSED(h); }
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    QSize sizeHint() const override { return QSize(1100, 240); }
    QSize minimumSizeHint() const override { return QSize(700, 160); }
    void resizeEvent(QResizeEvent* event) override {
    QOpenGLWidget::resizeEvent(event);
        scaledDirty = true; // ensure static-mode scaled cache updates on next paint
    }

private:
    // Simplified rendering methods
    void renderSimpleScrollMode(QPainter& p, double dispPlayheadRel);
    void renderSimpleStaticMode(QPainter& p, double dispPlayheadRel);
    void renderSimpleBeatGrid(QPainter& p, double dispPlayheadRel);

private:
    // Waveform rendering - optimized for performance
    QImage waveformImage;
    QPixmap cachedScaled; // cached scaled image for static mode
    bool scaledDirty{true};
    juce::AudioFormatManager formatManager;
    QString currentFilePath;
    double playheadPos{-1.0};
    bool scrollMode{false};
    int imageWidth{0};
    int imageHeight{0};
    
    // High-resolution source data for viewport rendering
    std::vector<float> sourceMaxBins;
    std::vector<float> sourceMinBins;
    int sourceWidth{0};
    double audioLength{0.0};
    
    // NEW: Cue points for display
    std::array<double, 8> cuePoints; // Cue points in seconds
    bool cuePointsValid{false};
    
    // NEW: Loop region for display
    bool loopEnabled{false};
    double loopStartSec{0.0};
    double loopEndSec{0.0};
    
    // NEW: Ghost loop region for display
    bool ghostLoopEnabled{false};
    double ghostLoopStartSec{0.0};
    double ghostLoopEndSec{0.0};
    
    // Performance optimization: Image cache and update throttling
    QTimer* updateThrottleTimer{nullptr};
    QTimer* renderTimer{nullptr};
    bool pendingUpdate{false};
    static constexpr int UPDATE_THROTTLE_MS = 16; // ~60 FPS for ultra-smooth performance
    
    // ENHANCED: Smart caching system for better performance with multiple loaded songs
    struct RenderCache {
        QPixmap waveformPixmap;
        QPixmap beatGridPixmap;
        double lastPlayheadPos{-1.0};
        double lastTempoFactor{1.0};
        int lastWidth{0};
        int lastHeight{0};
        bool waveformValid{false};
        bool beatGridValid{false};
        std::chrono::steady_clock::time_point lastUpdate;
    } renderCache;
    
    // Memory-efficient buffer pools to reduce allocations
    mutable std::vector<float> tempAudioBufferPool[2]; // Double buffering
    mutable std::vector<QRgb> pixelBufferPool[2];
    mutable int currentBufferIndex{0};
    
    // Beat grid system
    QVector<double> beatPositions;
    double tempoFactor{1.0}; // Current tempo factor from speed slider
    bool useAnalyzedBeats{false}; // Flag to switch between default grid and analyzed beats
    double firstBeatOffset{0.0}; // Offset of first beat from start of track (seconds)
    
    // Beat grid zoom system (independent of tempo)
    static int globalBeatGridZoomLevel; // Shared zoom level between all instances
    int beatGridZoomLevel{4}; // 0-9, where 4 is standard (1x)
    static constexpr double beatGridZoomFactors[10] = {0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0}; // Extended zoom levels
    
    // NEW: Fixed pixels-per-second system
    bool useFixedPixelsPerSecond{true}; // Use fixed pixels per second instead of scaling to widget width
    ViewMode viewMode{ViewMode::BeatLocked}; // Default to beat-locked visualization
    double visualLatencyComp{0.0};

    // BPM analysis UI state
    bool analysisActive{false};
    double analysisProgress{0.0}; // 0..1
    bool analysisFailed{false};
    
    double audioStartOffset{0.0}; // Offset where visible audio starts (seconds)
    double beatPhaseShiftSec{0.0}; // uniform phase shift we apply when drawing beats
    int manualBeatLineOffsetBeats{-1}; // manual whole-beat shift (negative = left)
    
    // Envelope-driven alignment data
    std::vector<float> ampEnvelope; // per-pixel max amplitude, size == imageWidth
    bool envelopeReady{false};
    
    // Analysis caches
    std::vector<float> noveltyFlux; // frame-wise positive energy differences
    double noveltyFluxHopSec{0.0};
    bool noveltyReady{false};

    // Scratch interaction state - SIMPLIFIED VINYL BEHAVIOR
    bool scratching{false};
    double scratchStartX{0.0};          // Initial mouse X when scratch began
    double scratchStartPos{0.0};        // Initial track position (0..1) when scratch began
    double scratchVelocity{0.0};        // Current scratch velocity for audio feedback
    QTimer* scratchTimer{nullptr};      // Timer for velocity updates
    
    // Additional scratch variables for the working implementation
    double scratchViewOffset{0.0};      // Current view offset during scratching
    double lastScratchX{0.0};           // Last mouse X position
    double scratchInitialDisplayPos{0.0}; // Initial display position when scratch started
    double scratchInitialAbsPos{0.0};   // Initial absolute position when scratch started
    
    // Viewport caching for scroll mode
    mutable bool viewportDirty{true};
    mutable bool cachedValidViewport{false};
    mutable double cachedSrcStartX{0.0};
    mutable double cachedSrcEndX{0.0};
    mutable double cachedLeftPadPx{0.0};
    mutable double cachedScaleFactor{1.0};

    // Private methods
    void loadAndRenderWaveform();
    void generateDefaultGrid(); // Generate default beat grid based on current BPM
    void recomputeBeatPhaseShift(); // Optimize phase so lines hit peaks in main section
    void computeEnergyFluxNovelty(const juce::AudioBuffer<float>& buffer, double sampleRateFromReader);
    double mapXToAbsRel(double x) const; // Helper to map x-position to absolute relative position (0..1 along full track)
    
    // NEW: Beat grid rendering with global beat grid
    void drawBeatGrid(QPainter& p, double playheadSec, double leftSecond, 
                      double rightSecond, double timeRange);
    
    // NEW: Cue points rendering
    void drawCuePoints(QPainter& p, double leftSecond, double rightSecond, double timeRange);
    
    // NEW: Loop region rendering
    void drawLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange);
    
    // NEW: Ghost loop region rendering
    void drawGhostLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange);
    
    // Performance optimization: Throttled update method
    void throttledUpdate();
    
    // Viewport and rendering methods
    void updateViewportCache() const;
    void renderScrollModeWaveform(QPainter& p, double dispPlayheadRel);
    void renderStaticModeWaveform(QPainter& p, double dispPlayheadRel);
    void renderBeatGrid(QPainter& p, double dispPlayheadRel);
    void renderBeatLine(QPainter& p, int x, int beatIndex); // Helper for drawing beat lines
};