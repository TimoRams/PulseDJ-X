#include "WaveformDisplay.h"
#include "WaveformGenerator.h"
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <chrono>

// REMOVED: Static global zoom level to prevent deck interference
// Each deck now has its own beatGridZoomLevel instance variable

WaveformDisplay::WaveformDisplay(QWidget* parent)
    : QOpenGLWidget(parent)
{
    // Request multisampling and vsync for ultra-smooth rendering
    QSurfaceFormat fmt = format();
    if (fmt.samples() < 4) fmt.setSamples(4); // MSAA x4
    fmt.setSwapInterval(1); // vsync on supported platforms
    setFormat(fmt);

    formatManager.registerBasicFormats(); // JUCE's basic formats include MP3 with JUCE_USE_MP3AUDIOFORMAT=1
    waveformImage = QImage(800, 200, QImage::Format_ARGB32_Premultiplied);
    waveformImage.fill(Qt::black);
    
    // Initialize cue points as invalid
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    
    // Enable keyboard focus
    setFocusPolicy(Qt::StrongFocus);
    
    // Setup smooth update throttling for fluid motion
    updateThrottleTimer = new QTimer(this);
    updateThrottleTimer->setSingleShot(true);
    updateThrottleTimer->setInterval(16); // 60 FPS for ultra smooth motion
    connect(updateThrottleTimer, &QTimer::timeout, this, [this]() {
        if (pendingUpdate) {
            pendingUpdate = false;
            update();
        }
    });

    // Dedicated render timer to keep frame pacing steady at ~60 FPS
    renderTimer = new QTimer(this);
    renderTimer->setTimerType(Qt::PreciseTimer);
    renderTimer->setInterval(16);
    connect(renderTimer, &QTimer::timeout, this, [this]() {
        // Only repaint when visible to save CPU/GPU
        if (isVisible()) update();
    });
    renderTimer->start();
}

void WaveformDisplay::paintGL()
{
    glViewport(0, 0, width(), height());
    glClearColor(8/255.0f, 8/255.0f, 10/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    QPainter p(this);
    
    // Professional antialiasing for smooth lines
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    
    // Background already cleared by GL

    if (sourceMaxBins.empty() || audioLength <= 0.0) {
        p.setPen(QPen(QColor(120, 120, 120), 1));
        p.setFont(QFont("Arial", 12));
        p.drawText(rect(), Qt::AlignCenter, "NO TRACK LOADED");
        return;
    }
    
    // Center line position (always fixed in the middle)
    int centerX = width() / 2;
    int centerY = height() / 2;
    
    // Get current zoom factor
    double zoomFactor = getBeatGridZoomFactor();
    
    // Current tempo factor (used for labels only; mapping uses pure track time)
    double deckTempoFactor = tempoFactor;
    
    // Calculate viewport timing: BeatLocked keeps scroll speed static (pps independent of tempo)
    // Use LOCAL calculation instead of GlobalBeatGrid to prevent deck interference
    double basePps = useFixedPixelsPerSecond
        ? 100.0  // Fixed local value instead of GlobalBeatGrid
        : ((double)width() / std::max(1.0, audioLength));
    double safeTempo = (tempoFactor > 1e-6 ? tempoFactor : 1.0);
    double pixelsPerSecond = basePps * zoomFactor; // do not scale by tempo for static scroll speed
    
    // Calculate current playhead position in seconds
    // PREROLL SUPPORT: Allow negative playhead positions but handle them correctly
    double effectivePlayhead = playheadPos;
    
    // CRITICAL FIX: Handle negative positions properly for preroll
    double playheadSec;
    if (effectivePlayhead < 0.0 && prerollEnabled) {
        // In preroll area: negative position maps to negative time
        // IMPORTANT: Don't multiply by audioLength for negative values - use direct mapping
        playheadSec = effectivePlayhead * prerollTimeSec; // This gives us seconds before track start
    } else {
        // Normal area: clamp to valid range and map normally
        effectivePlayhead = std::clamp(effectivePlayhead, 0.0, 1.0);
        playheadSec = effectivePlayhead * audioLength;
    }
    
    // Display center time: in BeatLocked, anchor by real-time proxy (playheadSec / tempo) for constant scroll speed
    double displayCenterSec = (viewMode == ViewMode::BeatLocked) ? (playheadSec / safeTempo) : playheadSec;
    
    // Calculate visible time range centered on playhead
    // PREROLL SUPPORT: Proper viewport calculation for negative times
    double bufferSec = std::max(0.05, 0.5 / std::max(1.0, zoomFactor));
    double halfViewportTime = (double)width() / (2.0 * pixelsPerSecond);
    double leftSecond = displayCenterSec - halfViewportTime - bufferSec;
    double rightSecond = displayCenterSec + halfViewportTime + bufferSec;
    
    // IMPORTANT: Don't artificially limit leftSecond - let it go negative for preroll
    // The rendering code will handle negative times appropriately
    
    // Convert to source bin indices for waveform data
    // PREROLL SUPPORT: Only use bins for positive time range
    double binPerSecond = (double)sourceWidth / audioLength;
    
    // Calculate bins only for the positive portion of the viewport
    double posLeftSecond = std::max(0.0, leftSecond);
    double posRightSecond = std::max(0.0, rightSecond);
    
    int leftBin = std::max(0, (int)((posLeftSecond - audioStartOffset) * binPerSecond));
    int rightBin = std::min(sourceWidth, (int)((posRightSecond - audioStartOffset) * binPerSecond));
    
    // Ensure valid range
    if (leftBin < 0) leftBin = 0;
    if (rightBin > sourceWidth) rightBin = sourceWidth;
    if (leftBin >= rightBin && rightSecond > 0.0) return; // Only return early if we're not in preroll
    
    // Smooth viewport calculations
    int pixelWidth = width();
    double timeRange = rightSecond - leftSecond;
    
    // Create REAL WAVEFORM like Serato/Rekordbox - separate upper and lower parts
    std::vector<QPointF> upperPoints, lowerPoints;
    upperPoints.reserve(pixelWidth);
    lowerPoints.reserve(pixelWidth);
    
    // Render pixel by pixel with consistent sampling strategy
    for (int screenX = 0; screenX < pixelWidth; ++screenX) {
        // Map screen pixel to time
        double screenRatio = (double)screenX / (double)pixelWidth;
        double timeSec = leftSecond + screenRatio * timeRange;
        
        // Calculate audio bin position
            double scaledTime;
            if (viewMode == ViewMode::BeatLocked) {
                // Map visual time (timeSec around displayCenterSec) to track time around playheadSec
                // Faster tempo -> multiply to compress waveform features
                double deltaVis = timeSec - displayCenterSec;
                scaledTime = playheadSec + (deltaVis * safeTempo);
            } else {
                // Pure track time
                scaledTime = timeSec;
            }
            double audioBinFloat = (scaledTime - audioStartOffset) * binPerSecond;
            
            // PREROLL SUPPORT: Handle negative times (before track start)
            if (scaledTime < 0.0 && prerollEnabled) {
                // In preroll area - render NO waveform (flat center line)
                upperPoints.emplace_back(screenX, centerY);
                lowerPoints.emplace_back(screenX, centerY);
                continue;
            }
            
            if (audioBinFloat < 0 || audioBinFloat >= sourceWidth) {
            upperPoints.emplace_back(screenX, centerY);
            lowerPoints.emplace_back(screenX, centerY);
            continue;
        }
        
        // Get REAL waveform min/max values with zoom-consistent sampling
        float minVal = 0.0f, maxVal = 0.0f;
        
        // Consistent sampling strategy based on zoom to prevent "wackling"
        if (zoomFactor < 1.0) {
            // Zoomed out: sample multiple bins to get stable peaks
            int sampleRadius = (int)(1.5 / zoomFactor);
            int startBin = std::max(0, (int)audioBinFloat - sampleRadius);
            int endBin = std::min(sourceWidth, (int)audioBinFloat + sampleRadius + 1);
            
            // Find peak values in range for stable display
            for (int b = startBin; b < endBin; ++b) {
                if (b >= 0 && b < sourceWidth && b < sourceMinBins.size() && b < sourceMaxBins.size()) {
                    minVal = std::min(minVal, sourceMinBins[b]);
                    maxVal = std::max(maxVal, sourceMaxBins[b]);
                }
            }
        } else {
            // Zoomed in: use smooth interpolation
            int bin = (int)audioBinFloat;
            float frac = audioBinFloat - bin;
            
            if (bin >= 0 && bin < sourceWidth - 1 && bin + 1 < sourceMinBins.size() && bin + 1 < sourceMaxBins.size()) {
                // Simple linear interpolation for consistency
                minVal = sourceMinBins[bin] * (1.0f - frac) + sourceMinBins[bin + 1] * frac;
                maxVal = sourceMaxBins[bin] * (1.0f - frac) + sourceMaxBins[bin + 1] * frac;
            } else if (bin >= 0 && bin < sourceWidth && bin < sourceMinBins.size() && bin < sourceMaxBins.size()) {
                minVal = sourceMinBins[bin];
                maxVal = sourceMaxBins[bin];
            }
        }
        
        // Convert to screen coordinates - REAL waveform with positive and negative parts
        float upperY = centerY - maxVal * height() * 0.45f;
        float lowerY = centerY - minVal * height() * 0.45f;
        
        upperPoints.emplace_back(screenX, upperY);
        lowerPoints.emplace_back(screenX, lowerY);
    }
    
    // Draw the REAL waveform like Serato/Rekordbox
    if (upperPoints.size() > 1 && lowerPoints.size() > 1) {
        // Create filled waveform area
        QPainterPath waveformPath;
        
        // Start with upper curve
        waveformPath.moveTo(upperPoints[0]);
        for (size_t i = 1; i < upperPoints.size(); ++i) {
            waveformPath.lineTo(upperPoints[i]);
        }
        
        // Connect to lower curve and go back
        waveformPath.lineTo(lowerPoints.back());
        for (int i = (int)lowerPoints.size() - 2; i >= 0; --i) {
            waveformPath.lineTo(lowerPoints[i]);
        }
        waveformPath.closeSubpath();
        
        // Fill with gradient like professional DJ software
        QLinearGradient waveGradient(0, 0, 0, height());
        waveGradient.setColorAt(0.0, QColor(100, 180, 255, 140));  // Top
        waveGradient.setColorAt(0.5, QColor(60, 140, 220, 80));    // Center
        waveGradient.setColorAt(1.0, QColor(100, 180, 255, 140));  // Bottom
        
        p.fillPath(waveformPath, QBrush(waveGradient));
        
        // Draw crisp outline lines
        QPen outlinePen(QColor(120, 200, 255), zoomFactor > 6.0 ? 1.8f : 1.2f);
        outlinePen.setCapStyle(Qt::RoundCap);
        outlinePen.setJoinStyle(Qt::RoundJoin);
        p.setPen(outlinePen);
        
        // Draw upper waveform line
        QPainterPath upperLine;
        upperLine.moveTo(upperPoints[0]);
        for (size_t i = 1; i < upperPoints.size(); ++i) {
            upperLine.lineTo(upperPoints[i]);
        }
        p.drawPath(upperLine);
        
        // Draw lower waveform line
        QPainterPath lowerLine;
        lowerLine.moveTo(lowerPoints[0]);
        for (size_t i = 1; i < lowerPoints.size(); ++i) {
            lowerLine.lineTo(lowerPoints[i]);
        }
        p.drawPath(lowerLine);
    }
    
    // Removed center horizontal line to eliminate remaining grey lines
    
    // Draw beat grid only after analysis is available
    if (useAnalyzedBeats) {
        drawBeatGrid(p, playheadSec, leftSecond, rightSecond, timeRange);
    }
    
    // Calculate audio-time viewport for cue points (unaffected by tempo)
    double audioTimeLeftSec = playheadSec - halfViewportTime - bufferSec;
    double audioTimeRightSec = playheadSec + halfViewportTime + bufferSec;
    double audioTimeRange = audioTimeRightSec - audioTimeLeftSec;
    
    // Draw cue points using audio time (not tempo-scaled time)
    if (cuePointsValid && audioLength > 0.0) {
        drawCuePoints(p, audioTimeLeftSec, audioTimeRightSec, audioTimeRange);
    }
    
    // Draw loops using display time (tempo-scaled) for correct visual sizing
    // Draw ghost loop region first (behind active loop) using display time
    if (ghostLoopEnabled && audioLength > 0.0 && ghostLoopEndSec > ghostLoopStartSec) {
        drawGhostLoopRegion(p, leftSecond, rightSecond, timeRange);
    }

    // PREROLL VISUALIZATION: Draw preroll area if enabled and visible
    if (prerollEnabled && leftSecond < 0.0) {
        drawPrerollRegion(p, leftSecond, rightSecond, timeRange);
    }

    // Draw active loop only when enabled
    std::cout << "WaveformDisplay::paintGL loop check: loopEnabled=" << loopEnabled 
              << ", loopStartSec=" << loopStartSec << ", loopEndSec=" << loopEndSec 
              << ", audioLength=" << audioLength << std::endl;
    if (loopEnabled && loopEndSec > loopStartSec && audioLength > 0.0) {
        std::cout << "WaveformDisplay::paintGL calling drawLoopRegion" << std::endl;
        drawLoopRegion(p, leftSecond, rightSecond, timeRange);
    }
    
    // DEBUG: Always draw a red indicator if loop is enabled (for debugging)
    if (loopEnabled) {
        p.setPen(QPen(QColor(255, 0, 0), 3));
        p.drawRect(10, 10, 100, 20);
        p.drawText(15, 25, QString("LOOP ENABLED: %1-%2").arg(loopStartSec, 0, 'f', 1).arg(loopEndSec, 0, 'f', 1));
    }
    
    // Draw playhead
    p.setPen(QPen(QColor(255, 100, 100), 2));
    p.drawLine(centerX, 0, centerX, height());
    
    // Zoom indicator
    if (zoomFactor != 1.0) {
        p.setPen(QPen(QColor(150, 180, 220), 1));
        p.setFont(QFont("Arial", 8));
        QString zoomText = QString("%1x").arg(zoomFactor, 0, 'f', 1);
        p.drawText(8, height() - 15, zoomText);
    }
}

void WaveformDisplay::loadAndRenderWaveform()
{
    // Avoid heavy generation on UI thread; expect setSourceBins from background
    if (currentFilePath.isEmpty()) return;
    update();
}

void WaveformDisplay::loadFile(const QString& path)
{
    currentFilePath = path;
    audioStartOffset = 0.0;
    // Reset analysis UI state on new file
    analysisActive = false;
    analysisFailed = false;
    analysisProgress = 0.0;
    QTimer::singleShot(10, this, &WaveformDisplay::loadAndRenderWaveform);
}

void WaveformDisplay::setOriginalBpm(double bpm, double trackLengthSeconds)
{
    originalBpm = bpm;
    trackLengthSec = trackLengthSeconds;
    // REMOVED: GlobalBeatGrid update to prevent deck interference
    // Each deck now uses its own originalBpm for beat grid rendering
    update();
}

void WaveformDisplay::setPlayhead(double relative)
{
    // High precision threshold for exact positioning  
    const double threshold = 0.0001;
    
    // PREROLL SUPPORT: UNLIMITED negative positions for DJ-style cueing
    double minPos = prerollEnabled ? -999.0 : 0.0; // Match mouseMoveEvent logic
    double maxPos = 1.0;
    
    // Debug: Log setPlayhead calls for preroll positions
    if (relative < 0.0) {
        std::cout << "setPlayhead: relative=" << relative << ", minPos=" << minPos 
                  << " (unlimited preroll)" << std::endl;
    }
    
    // Clamp to valid range including unlimited preroll
    relative = std::clamp(relative, minPos, maxPos);
    
    if (std::abs(playheadPos - relative) > threshold) {
        playheadPos = relative;
        
        // Use throttled update for smooth 30 FPS performance
        throttledUpdate();
    }
}

void WaveformDisplay::throttledUpdate() {
    if (!updateThrottleTimer->isActive()) {
        updateThrottleTimer->start();
        update();
    } else {
        pendingUpdate = true;
    }
}

void WaveformDisplay::updateTempo(double newBpm) {
    if (originalBpm <= 0.0) {
        update();
        return;
    }
    // Compute tempo factor relative to original BPM
    double factor = newBpm / originalBpm;
    if (factor <= 0.0) factor = 1.0;
    
    // REMOVED: GlobalBeatGrid::getInstance().setTempoFactor(factor); - now deck-specific
    tempoFactor = factor; // Store locally for this deck
    
    // REMOVED: GlobalBeatGrid update to prevent deck interference
    // Each deck now uses its own originalBpm and tempoFactor
    update();
}

void WaveformDisplay::refreshBeatGrid() {
    update();
}

// Scratching system with mouse drag for DJ-style control - ANTI-PLAY VERSION
void WaveformDisplay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && trackLengthSec > 0.0) {
        // Start scratching mode
        scratching = true;
        lastScratchX = event->position().x();
        scratchStartPos = playheadPos; // Current playhead position when scratch started
        
        // IMPORTANT: Store the original scratch start X position for reference
        scratchStartX = event->position().x();
        
        // Initialize timing for smooth velocity calculation
        static auto scratchStartTime = std::chrono::steady_clock::now();
        scratchStartTime = std::chrono::steady_clock::now();
        
        // CRITICAL: Emit scratchStart FIRST before any other operations
        // This should immediately disable normal playback and timer updates
        emit scratchStart();
        
        // Set cursor to indicate scratching mode
        setCursor(Qt::ClosedHandCursor);
        
        // DEBUG: Reduced logging to prevent spam
        std::cout << "SCRATCH STARTED at pos: " << scratchStartPos 
                  << ", preroll: " << (prerollEnabled ? "YES" : "NO") << std::endl;
    }
}

// Scratching motion - STABILIZED to prevent bugs and accidental play
void WaveformDisplay::mouseMoveEvent(QMouseEvent* event)
{
    if (scratching && trackLengthSec > 0.0) {
        // Calculate mouse movement from LAST position for incremental movement
        int currentX = event->position().x();
        int deltaX = currentX - lastScratchX; // Use lastScratchX for smooth incremental movement
        
        // DIRECTION FIX: Invert deltaX so left mouse = backward waveform movement
        deltaX = -deltaX;
        
        // STABILITY: Skip tiny movements and prevent jitter
        if (std::abs(deltaX) < 3) { // Increased threshold from 2 to 3 for more stability
            return;
        }
        
        // STABILITY: Limit maximum movement per frame to prevent jumps
        if (std::abs(deltaX) > 50) { // Clamp extreme movements
            deltaX = (deltaX > 0) ? 50 : -50;
        }

        // ADVANCED CONSISTENCY: Calculate sensitivity based on visible time range
        // Use the same seconds-per-pixel as rendering. Honor fixed vs variable PPS.
        double zoomFactor = getBeatGridZoomFactor();
        double basePps = useFixedPixelsPerSecond ? 100.0 : ((double)width() / std::max(1.0, audioLength));
        double pixelsPerSecond = basePps * zoomFactor;

        // Calculate time-based sensitivity: seconds per pixel
        double secondsPerPixel = 1.0 / std::max(1e-6, pixelsPerSecond);

        // STABILITY: Apply smoothing factor to reduce sudden jumps
        double smoothingFactor = 0.8; // Smooth out 20% of the movement
        double dxSmoothed = (double)deltaX * smoothingFactor;

        // Convert deltaX (pixels) to deltaSec (seconds)
        double deltaSec = dxSmoothed * secondsPerPixel;

        // Map current playhead to absolute seconds for unified handling
        double currentSec;
        if (playheadPos < 0.0 && prerollEnabled) {
            currentSec = playheadPos * prerollTimeSec;
        } else {
            currentSec = std::clamp(playheadPos, 0.0, 1.0) * audioLength;
        }

        // STABILITY: Limit seconds change per frame to match the prior "perfect" preroll feel
        // Prior clamp was 0.01 relative in preroll -> equals 0.01 * prerollTimeSec seconds
        double referencePrerollSec = std::max(0.001, prerollTimeSec);
        double maxDeltaSec = 0.01 * referencePrerollSec;
        deltaSec = std::clamp(deltaSec, -maxDeltaSec, maxDeltaSec);

        // Calculate new absolute seconds and map back to relative
        double newSec = currentSec + deltaSec;
        double newPos;
        if (newSec < 0.0 && prerollEnabled) {
            newPos = newSec / referencePrerollSec;
        } else {
            newPos = (audioLength > 0.0) ? (newSec / audioLength) : playheadPos;
        }

        // PREROLL: UNLIMITED negative space - no minimum limit for DJ cueing
        double minPos = prerollEnabled ? -999.0 : 0.0; // Effectively unlimited preroll
        double maxPos = 1.0;

        // Clamp to valid range
        newPos = std::clamp(newPos, minPos, maxPos);

        // ANTI-JITTER: Only update if change is significant enough
        if (std::abs(newPos - playheadPos) > 0.0001) {

            // Debug (reduced frequency to avoid spam)
            static int debugCounter = 0;
            if (++debugCounter % 10 == 0) { // Only every 10th movement
                std::cout << "SCRATCH: deltaX=" << deltaX
                          << " deltaSec=" << deltaSec
                          << " oldPos=" << playheadPos
                          << " newPos=" << newPos
                          << " (preroll=" << (newPos < 0.0 ? "YES" : "NO") << ")" << std::endl;
            }

            // Update position
            playheadPos = newPos;

            // CRITICAL: Emit scratchMove to update audio position
            // This should NOT trigger play - the audio player handles scratch mode separately
            emit scratchMove(newPos);

            // Update display
            update();
        }
        
        // Update reference for next move (incremental)
        lastScratchX = currentX;
    }
}

void WaveformDisplay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && scratching) {
        // CRITICAL: End scratching mode FIRST
        scratching = false;
        
        // IMPORTANT: Emit scratchEnd to restore normal playback behavior
        // The audio player will handle whether to resume playing or stay paused
        emit scratchEnd();
        
        // Reset cursor
        setCursor(Qt::ArrowCursor);
        
        // DEBUG: Simple end message
        std::cout << "SCRATCH ENDED at pos: " << playheadPos << std::endl;
    }
}

// Zoom controls with + and - keys
void WaveformDisplay::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
        case Qt::Key_Plus:
        case Qt::Key_Equal: // Handle both + and = key
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

// Zoom functionality implementation
void WaveformDisplay::increaseBeatGridZoom() {
    if (beatGridZoomLevel < 9) { // Extended to 9 for 16x zoom
        beatGridZoomLevel++;
        emit zoomLevelChanged(beatGridZoomLevel);
        update();
    }
}

void WaveformDisplay::decreaseBeatGridZoom() {
    if (beatGridZoomLevel > 0) {
        beatGridZoomLevel--;
        emit zoomLevelChanged(beatGridZoomLevel);
        throttledUpdate();
    }
}

void WaveformDisplay::resetBeatGridZoom() {
    beatGridZoomLevel = 4; // Reset to standard (1x)
    emit zoomLevelChanged(beatGridZoomLevel);
    throttledUpdate();
}

void WaveformDisplay::setBeatGridZoomLevel(int level) {
    if (level >= 0 && level <= 9) { // Extended range
        beatGridZoomLevel = level;
        throttledUpdate();
    }
}

// Utility functions
double WaveformDisplay::mapXToAbsRel(double x) const
{
    return std::clamp(x / std::max(1, width()), 0.0, 1.0);
}

void WaveformDisplay::recomputeBeatPhaseShift() {}
void WaveformDisplay::generateDefaultGrid() {}

// NEW: Cue points support
void WaveformDisplay::setCuePoints(const std::array<double, 8>& newCuePoints) {
    cuePoints = newCuePoints;
    cuePointsValid = true;
    throttledUpdate();
}

void WaveformDisplay::clearCuePoints() {
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    throttledUpdate();
}

// NEW: Loop region support
void WaveformDisplay::setLoopRegion(bool enabled, double startSec, double endSec) {
    std::cout << "WaveformDisplay::setLoopRegion called - enabled: " << enabled 
              << ", startSec: " << startSec << ", endSec: " << endSec << std::endl;
    loopEnabled = enabled;
    loopStartSec = startSec;
    loopEndSec = endSec;
    std::cout << "WaveformDisplay loop state updated: loopEnabled=" << loopEnabled 
              << ", loopStartSec=" << loopStartSec << ", loopEndSec=" << loopEndSec << std::endl;
    throttledUpdate();
}

void WaveformDisplay::clearLoop() {
    loopEnabled = false;
    loopStartSec = 0.0;
    loopEndSec = 0.0;
    throttledUpdate();
}

// NEW: Ghost loop region support
void WaveformDisplay::setGhostLoopRegion(bool enabled, double startSec, double endSec) {
    ghostLoopEnabled = enabled;
    ghostLoopStartSec = startSec;
    ghostLoopEndSec = endSec;
    throttledUpdate();
}

// NEW: Beat grid rendering using global beat grid system
void WaveformDisplay::drawBeatGrid(QPainter& p, double playheadSec, double leftSecond, 
                                   double rightSecond, double timeRange) {
    
    if (timeRange <= 0.0) return;
    
    // Use LOCAL beat data and BPM - completely independent from GlobalBeatGrid
    std::vector<double> localBeats;
    double localBpm = originalBpm; // Use this deck's original BPM
    
    if (!beatPositions.isEmpty()) {
        localBeats.reserve(beatPositions.size());
        for (double rel : beatPositions) localBeats.push_back(rel * trackLengthSec);
    } else if (localBpm > 0.0) {
        // Generate local beat grid based on this deck's BPM - NO GlobalBeatGrid dependency
        double beatInterval = 60.0 / localBpm;
        for (double t = 0.0; t < trackLengthSec + beatInterval; t += beatInterval) {
            localBeats.push_back(t);
        }
    }
    
    if (localBeats.empty() && localBpm <= 0.0) return;
    
    // Tempo used for transform in BeatLocked mode
    double deckTempoFactor = tempoFactor;
    double safeTempoLocal = (deckTempoFactor > 1e-6 ? deckTempoFactor : 1.0);
    
    // Calculate current effective BPM for visual styling - use local BPM
    double currentBpm = localBpm * deckTempoFactor;
    
    // Common orange line style for all beat lines that are multiple of 4
    QPen orangeBeatPen(QColor(255, 150, 50, 200), 3.0);
    orangeBeatPen.setStyle(Qt::SolidLine);
    
    // White line style for regular beats
    QPen whiteBeatPen(QColor(200, 220, 255, 160), 1.5);
    whiteBeatPen.setStyle(Qt::SolidLine);
    
    int beatIndex = 0;
    const double displayCenterSecLocal = (leftSecond + rightSecond) * 0.5;
    
    // PREROLL BEAT GRID: Handle negative time region ONLY - completely independent
    if (leftSecond < 0.0 && prerollEnabled && localBpm > 0.0) {
        double beatInterval = 60.0 / (localBpm * deckTempoFactor); // Use local BPM with tempo
        
        // Only draw in negative time region - start from -beatInterval and go backwards
        for (double beatTime = -beatInterval; beatTime >= leftSecond; beatTime -= beatInterval) {
            if (beatTime >= 0.0) continue; // Only negative times for preroll
            
            double visualTime;
            if (viewMode == ViewMode::BeatLocked) {
                visualTime = displayCenterSecLocal + (beatTime - playheadSec) / std::max(1e-6, safeTempoLocal);
            } else {
                visualTime = beatTime;
            }
            
            double relativePos = (visualTime - leftSecond) / timeRange;
            int screenX = (int)(relativePos * width());
            
            if (screenX >= 0 && screenX < width()) {
                // Calculate which beat this is (-1, -2, -3, ...)
                int beatNumber = (int)std::round(beatTime / beatInterval);
                
                // Every 4th beat gets orange line
                if (beatNumber % 4 == 0) {
                    p.setPen(orangeBeatPen);
                    p.drawLine(screenX, 0, screenX, height());
                    
                    // Orange line number: -1, -2, -3, etc.
                    int orangeLineNumber = beatNumber / 4;
                    
                    // Draw number labels
                    p.setPen(QPen(QColor(255, 180, 100, 200), 1));
                    p.setFont(QFont("Arial", 9, QFont::Bold));
                    p.drawText(screenX + 3, 15, QString::number(orangeLineNumber));
                } else {
                    // Regular beats - white lines
                    p.setPen(whiteBeatPen);
                    p.drawLine(screenX, height()/3, screenX, 2*height()/3);
                }
            }
        }
        
        // Special case: Draw the "0" line at song start (beatTime = 0.0) if visible
        if (rightSecond > 0.0 && leftSecond < 0.1) {
            double visualTime;
            if (viewMode == ViewMode::BeatLocked) {
                visualTime = displayCenterSecLocal + (0.0 - playheadSec) / std::max(1e-6, safeTempoLocal);
            } else {
                visualTime = 0.0;
            }
            
            double relativePos = (visualTime - leftSecond) / timeRange;
            int screenX = (int)(relativePos * width());
            
            if (screenX >= 0 && screenX < width()) {
                p.setPen(orangeBeatPen);
                p.drawLine(screenX, 0, screenX, height());
                
                // Draw "0" label at song start
                p.setPen(QPen(QColor(255, 180, 100, 200), 1));
                p.setFont(QFont("Arial", 9, QFont::Bold));
                p.drawText(screenX + 3, 15, "0");
            }
        }
    }
    
    // REGULAR BEAT GRID: Handle positive time region (existing track) - start from first beat AFTER 0
    for (double beatTime : localBeats) {
        // Only process beats well after track start (> 0.1) to avoid any overlap
        if (beatTime <= 0.1) {
            beatIndex++;
            continue;
        }
        double visualTime;
        if (viewMode == ViewMode::BeatLocked) {
            // Transform track beat time to visual time so that center corresponds to playhead
            // Faster tempo -> divide to bring beat lines closer together
            visualTime = displayCenterSecLocal + (beatTime - playheadSec) / std::max(1e-6, safeTempoLocal);
        } else {
            visualTime = beatTime;
        }
        double relativePos = (visualTime - leftSecond) / timeRange;
        int screenX = (int)(relativePos * width());
        
        if (screenX < 0 || screenX >= width()) {
            beatIndex++;
            continue;
        }
        
        // Every 4th beat gets an orange line with sequential numbering
        if (beatIndex % 4 == 0) {
            // Use the common orange style for ALL beats that are multiples of 4
            p.setPen(orangeBeatPen);
            p.drawLine(screenX, 0, screenX, height()); // Full height line
            
            // Calculate sequential number for ALL orange lines
            int orangeLineNumber = (beatIndex / 4) + 1; // Continuous numbering
            
            p.setPen(QPen(QColor(255, 180, 100, 200), 1));
            p.setFont(QFont("Arial", 9, QFont::Bold));
            p.drawText(screenX + 3, 15, QString::number(orangeLineNumber));
        } else {
            // Regular beats - WHITE lines (no numbering)
            p.setPen(whiteBeatPen);
            p.drawLine(screenX, height()/3, screenX, 2*height()/3);
        }
        
        beatIndex++;
    }
    
    // BPM indicator with analysis status
    {
        p.setFont(QFont("Arial", 8));
        int rightX = width() - 8;
        int y = 15;
        if (analysisActive) {
            // Show progress percent
            int percent = (int)std::round(analysisProgress * 100.0);
            p.setPen(QPen(QColor(180, 200, 255), 1));
            QString txt = QString("Analyzing %1%") .arg(percent);
            int w = p.fontMetrics().horizontalAdvance(txt);
            p.drawText(rightX - w, y, txt);
        } else if (analysisFailed) {
            p.setPen(QPen(QColor(255, 120, 120), 1));
            QString txt("Analysis failed");
            int w = p.fontMetrics().horizontalAdvance(txt);
            p.drawText(rightX - w, y, txt);
        } else if (currentBpm > 0.0) {
            p.setPen(QPen(QColor(150, 180, 220), 1));
            QString bpmText = QString("BPM: %1").arg(currentBpm, 0, 'f', 1);
            int w = p.fontMetrics().horizontalAdvance(bpmText);
            p.drawText(rightX - w, y, bpmText);
        }
        // Optional: pixels-per-second info on next line
        if (useFixedPixelsPerSecond) {
            double pixelsPerSec = 100.0; // Local fixed value instead of GlobalBeatGrid
            QString ratioText = QString("%1px/s").arg(pixelsPerSec, 0, 'f', 0);
            int w = p.fontMetrics().horizontalAdvance(ratioText);
            p.setPen(QPen(QColor(150, 180, 220), 1));
            p.drawText(rightX - w, 30, ratioText);
        }
    }
}

// NEW: Draw cue points as vertical lines
void WaveformDisplay::drawCuePoints(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (timeRange <= 0.0 || audioLength <= 0.0) return;
    
    // Define cue point colors (rainbow-like sequence for easy identification)
    static const QColor cueColors[8] = {
        QColor(255, 80, 80),   // Red
        QColor(255, 150, 80),  // Orange  
        QColor(255, 220, 80),  // Yellow
        QColor(150, 255, 80),  // Light Green
        QColor(80, 255, 150),  // Cyan
        QColor(80, 180, 255),  // Blue
        QColor(150, 80, 255),  // Purple
        QColor(255, 80, 200)   // Magenta
    };
    
    // Get tempo and viewport info for proper scaling
    double safeTempo = (tempoFactor > 1e-6 ? tempoFactor : 1.0);
    double displayCenterSec = (leftSecond + rightSecond) * 0.5;
    double playheadSec = std::clamp(playheadPos, 0.0, 1.0) * audioLength;
    
    for (int i = 0; i < 8; ++i) {
        if (cuePoints[i] < 0.0) continue; // Skip unset cue points
        
        double cueAudioTime = cuePoints[i]; // Original cue time in audio seconds
        
        // Convert cue time from audio time to display time based on view mode
        double cueDisplayTime;
        if (viewMode == ViewMode::BeatLocked) {
            // BeatLocked: Convert audio time to visual time using same logic as beat grid
            // visual = displayCenter + (audio - playhead) / tempo
            cueDisplayTime = displayCenterSec + (cueAudioTime - playheadSec) / safeTempo;
        } else {
            // TimeLocked: visual time equals audio time
            cueDisplayTime = cueAudioTime;
        }
        
        // Check if cue point is within visible range (using display time)
        if (cueDisplayTime < leftSecond || cueDisplayTime > rightSecond) continue;
        
        // Calculate screen position using display time
        double relativePos = (cueDisplayTime - leftSecond) / timeRange;
        int screenX = (int)(relativePos * width());
        
        // Ensure screen position is valid
        if (screenX < 0 || screenX >= width()) continue;
        
        // Draw cue line with distinctive style
        QPen cuePen(cueColors[i], 2.5);
        cuePen.setStyle(Qt::SolidLine);
        p.setPen(cuePen);
        p.drawLine(screenX, 0, screenX, height());
        
        // Draw small cue number label at bottom
        p.setFont(QFont("Arial", 7, QFont::Bold));
        QString cueLabel = QString::number(i + 1);
        QRect labelRect = p.fontMetrics().boundingRect(cueLabel);
        
        // Position label at bottom to avoid overlap with beat grid
        int labelX = screenX + 3;
        int labelY = height() - 2; // Bottom of waveform
        
        // Draw label background for better readability
        QRect bgRect(labelX - 1, labelY - labelRect.height() + 1, labelRect.width() + 2, labelRect.height());
        p.fillRect(bgRect, QColor(0, 0, 0, 180));
        
        // Draw label text
        p.setPen(QPen(cueColors[i], 1));
        p.drawText(labelX, labelY, cueLabel);
    }
}

// NEW: Draw loop region as semi-transparent box
void WaveformDisplay::drawLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (timeRange <= 0.0 || audioLength <= 0.0) return;
    
    // Convert loop times from audio time to display time for correct visual sizing
    double safeTempo = (tempoFactor > 1e-6 ? tempoFactor : 1.0);
    double displayLoopStartSec, displayLoopEndSec;
    
    if (viewMode == ViewMode::BeatLocked) {
        // BeatLocked display maps visual time around playhead: 
        // visual = displayCenter + (audio - playhead) / tempo
        // USE SAME LOGIC AS GHOST LOOP SINCE THAT WORKS
        const double displayCenterSecLocal = (leftSecond + rightSecond) * 0.5;
        const double phSec = std::clamp(playheadPos, 0.0, 1.0) * audioLength;
        displayLoopStartSec = displayCenterSecLocal + (loopStartSec - phSec) / safeTempo;
        displayLoopEndSec   = displayCenterSecLocal + (loopEndSec   - phSec) / safeTempo;
    } else {
        // TimeLocked: visual time equals audio time
        displayLoopStartSec = loopStartSec;
        displayLoopEndSec   = loopEndSec;
    }
    
    // Check if loop region is within visible range
    if (displayLoopEndSec < leftSecond || displayLoopStartSec > rightSecond) return;
    
    // Calculate screen positions
    double relativeStart = (displayLoopStartSec - leftSecond) / timeRange;
    double relativeEnd = (displayLoopEndSec - leftSecond) / timeRange;
    
    // Clamp to visible area
    relativeStart = std::max(0.0, std::min(1.0, relativeStart));
    relativeEnd = std::max(0.0, std::min(1.0, relativeEnd));
    
    int screenStartX = (int)(relativeStart * width());
    int screenEndX = (int)(relativeEnd * width());
    
    if (screenEndX <= screenStartX) return;
    
    // Draw semi-transparent loop region (more opaque than ghost)
    QColor loopColor(100, 255, 100, 160); // Green with 160/255 transparency
    p.fillRect(screenStartX, 0, screenEndX - screenStartX, height(), loopColor);
    
    // Draw loop boundaries with more opaque lines
    QPen loopBoundaryPen(QColor(0, 200, 0, 200), 2.5);
    loopBoundaryPen.setStyle(Qt::SolidLine);
    p.setPen(loopBoundaryPen);
    
    // Draw start and end lines
    p.drawLine(screenStartX, 0, screenStartX, height());
    p.drawLine(screenEndX, 0, screenEndX, height());
    
    // Draw small "LOOP" label at top of loop region
    p.setFont(QFont("Arial", 8, QFont::Bold));
    QString loopLabel = "LOOP";
    QRect labelRect = p.fontMetrics().boundingRect(loopLabel);
    
    // Center label in loop region
    int labelX = screenStartX + (screenEndX - screenStartX - labelRect.width()) / 2;
    int labelY = 15;
    
    // Draw label background
    QRect bgRect(labelX - 2, labelY - labelRect.height() + 1, labelRect.width() + 4, labelRect.height());
    p.fillRect(bgRect, QColor(0, 0, 0, 200));
    
    // Draw label text
    p.setPen(QPen(QColor(100, 255, 100), 1));
    p.drawText(labelX, labelY, loopLabel);
}

// NEW: Draw ghost loop region as very transparent box for last used loop
void WaveformDisplay::drawGhostLoopRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    if (!ghostLoopEnabled || timeRange <= 0.0 || audioLength <= 0.0) return;
    
    // Convert ghost loop times from audio time to display time for correct visual sizing
    double safeTempo = (tempoFactor > 1e-6 ? tempoFactor : 1.0);
    double displayGhostLoopStartSec, displayGhostLoopEndSec;
    
    if (viewMode == ViewMode::BeatLocked) {
        // BeatLocked display maps visual time around playhead: 
        // visual = displayCenter + (audio - playhead) / tempo
        const double displayCenterSecLocal = (leftSecond + rightSecond) * 0.5;
        const double phSec = std::clamp(playheadPos, 0.0, 1.0) * audioLength;
        displayGhostLoopStartSec = displayCenterSecLocal + (ghostLoopStartSec - phSec) / safeTempo;
        displayGhostLoopEndSec   = displayCenterSecLocal + (ghostLoopEndSec   - phSec) / safeTempo;
    } else {
        // TimeLocked: visual time equals audio time
        displayGhostLoopStartSec = ghostLoopStartSec;
        displayGhostLoopEndSec   = ghostLoopEndSec;
    }
    
    // Check if ghost loop region is within visible range
    if (displayGhostLoopEndSec < leftSecond || displayGhostLoopStartSec > rightSecond) return;
    
    // Calculate screen positions
    double relativeStart = (displayGhostLoopStartSec - leftSecond) / timeRange;
    double relativeEnd = (displayGhostLoopEndSec - leftSecond) / timeRange;
    
    // Clamp to visible area
    relativeStart = std::max(0.0, std::min(1.0, relativeStart));
    relativeEnd = std::max(0.0, std::min(1.0, relativeEnd));
    
    int screenStartX = (int)(relativeStart * width());
    int screenEndX = (int)(relativeEnd * width());
    
    if (screenEndX <= screenStartX) return;
    
    // Draw very transparent ghost loop region (much lighter than active loop)
    QColor ghostLoopColor(100, 255, 100, 20); // Green with 20/255 transparency (lighter than active)
    p.fillRect(screenStartX, 0, screenEndX - screenStartX, height(), ghostLoopColor);
    
    // Draw ghost loop boundaries with lighter opacity
    QPen ghostBoundaryPen(QColor(0, 200, 0, 80), 1.5);
    ghostBoundaryPen.setStyle(Qt::DashLine); // Use dashed line to distinguish from active loop
    p.setPen(ghostBoundaryPen);
    
    // Draw start and end lines
    p.drawLine(screenStartX, 0, screenStartX, height());
    p.drawLine(screenEndX, 0, screenEndX, height());
    
    // Draw small "GHOST" label at top of ghost loop region
    p.setFont(QFont("Arial", 7, QFont::Normal)); // Smaller and lighter than active loop
    QString ghostLabel = "GHOST";
    QRect labelRect = p.fontMetrics().boundingRect(ghostLabel);
    
    // Center label in ghost loop region
    int labelX = screenStartX + (screenEndX - screenStartX - labelRect.width()) / 2;
    int labelY = 30; // Slightly lower than active loop label
    
    // Draw label background with lower opacity
    QRect bgRect(labelX - 2, labelY - labelRect.height() + 1, labelRect.width() + 4, labelRect.height());
    p.fillRect(bgRect, QColor(0, 0, 0, 100)); // Less opaque background
    
    // Draw label text with lighter color
    p.setPen(QPen(QColor(100, 255, 100, 150), 1)); // More transparent text
    p.drawText(labelX, labelY, ghostLabel);
}

// NEW: Draw preroll region for DJ-style cueing
void WaveformDisplay::drawPrerollRegion(QPainter& p, double leftSecond, double rightSecond, double timeRange) {
    // Only draw if preroll area is visible
    if (rightSecond <= 0.0) {
        // Entire viewport is in preroll
        double screenStartX = 0;
        double screenEndX = width();
        
        // Dark blue-gray background for preroll area
        QColor prerollColor(30, 50, 80, 120);
        p.fillRect(QRect(screenStartX, 0, screenEndX - screenStartX, height()), prerollColor);
        
        // Diagonal stripes pattern to indicate preroll
        p.setPen(QPen(QColor(60, 100, 160), 1));
        for (int x = screenStartX; x < screenEndX; x += 20) {
            p.drawLine(x, 0, x + 10, height());
        }
        
        // Preroll label
        p.setFont(QFont("Arial", 10, QFont::Bold));
        p.setPen(QPen(QColor(120, 180, 255), 1));
        p.drawText(width()/2 - 30, height()/2, "PREROLL");
        
    } else if (leftSecond < 0.0) {
        // Partial preroll visible
        double prerollRatio = -leftSecond / timeRange;
        int screenEndX = (int)(prerollRatio * width());
        
        // Dark blue-gray background for preroll area
        QColor prerollColor(30, 50, 80, 120);
        p.fillRect(QRect(0, 0, screenEndX, height()), prerollColor);
        
        // Diagonal stripes pattern
        p.setPen(QPen(QColor(60, 100, 160), 1));
        for (int x = 0; x < screenEndX; x += 15) {
            p.drawLine(x, 0, x + 8, height());
        }
        
        // Track start line (at position 0.0)
        double trackStartRatio = -leftSecond / timeRange;
        int trackStartX = (int)(trackStartRatio * width());
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