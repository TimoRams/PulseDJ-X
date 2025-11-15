#include "MainWindow.h"
#include "DJAudioPlayer.h"
#include "BpmAnalyzer.h"
#include "WaveformDisplay.h"
#include "BeatIndicator.h"
#include "PreferencesDialog.h"
#include "ScratchEngine.h"
#include "StereoAudioCallback.h"
#include "MainWindowTasks.h"
#include "AppConfig.h"
#include "DeckSettings.h"
#include "CustomFader.h"
#include <QApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QProgressDialog>
#include <QMenu>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDir>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QThreadPool>
#include <QSettings>
#include <QTimer>
#include <QCursor>
#include <iostream>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <ranges>
#include <type_traits>
#include <vector>
#include <ranges>
#include <vector>

std::shared_ptr<juce::AudioFormatManager> QtMainWindow::sharedFormatManager = {};

QtMainWindow::QtMainWindow(QWidget* parent) : QWidget(parent)
{
    setWindowTitle("BetaPulseX - Professional DJ Software");
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_StyledBackground, true);

    if (auto* app = qApp) {
        app->installEventFilter(this);
    }
    
    // Remove window decorations and make frameless
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setStyleSheet("QtMainWindow { background-color: #141a1f; border: none; }");
    
    // Apply modern auto-hide scrollbar style globally
    if (auto* app = qobject_cast<QApplication*>(QApplication::instance())) {
        app->setStyleSheet(
            // Modern thin scrollbars - small and transparent by default
            "QScrollBar:vertical {"
            "    background: transparent;"
            "    width: 8px;"
            "    margin: 0px;"
            "    border: none;"
            "}"
            "QScrollBar::handle:vertical {"
            "    background: rgba(255, 255, 255, 0.15);"
            "    min-height: 20px;"
            "    border-radius: 4px;"
            "    margin: 2px;"
            "}"
            "QScrollBar::handle:vertical:hover {"
            "    background: rgba(255, 255, 255, 0.35);"
            "}"
            "QScrollBar::handle:vertical:pressed {"
            "    background: rgba(255, 255, 255, 0.5);"
            "}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
            "    height: 0px;"
            "    background: none;"
            "}"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "    background: none;"
            "}"
            // Horizontal scrollbar
            "QScrollBar:horizontal {"
            "    background: transparent;"
            "    height: 8px;"
            "    margin: 0px;"
            "    border: none;"
            "}"
            "QScrollBar::handle:horizontal {"
            "    background: rgba(255, 255, 255, 0.15);"
            "    min-width: 20px;"
            "    border-radius: 4px;"
            "    margin: 2px;"
            "}"
            "QScrollBar::handle:horizontal:hover {"
            "    background: rgba(255, 255, 255, 0.35);"
            "}"
            "QScrollBar::handle:horizontal:pressed {"
            "    background: rgba(255, 255, 255, 0.5);"
            "}"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
            "    width: 0px;"
            "    background: none;"
            "}"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
            "    background: none;"
            "}"
            // Expand on hover for better usability
            "QScrollBar:vertical:hover {"
            "    width: 12px;"
            "    background: rgba(0, 0, 0, 0.2);"
            "}"
            "QScrollBar:horizontal:hover {"
            "    height: 12px;"
            "    background: rgba(0, 0, 0, 0.2);"
            "}"
        );
    }
    
    menuBar = new MenuBar(this);
    
    // Setup latency monitoring timer
    auto* latencyTimer = new QTimer(this);
    connect(latencyTimer, &QTimer::timeout, this, [this]() {
        if (menuBar && (playerA || playerB)) {
            // Get latency from both decks and use the max (or from the playing deck)
            const double latencyA = playerA ? playerA->getMeasuredLatencyMs() : 0.0;
            const double latencyB = playerB ? playerB->getMeasuredLatencyMs() : 0.0;
            // Prefer the audible deck when exactly one is playing so the readout
            // tracks the perception; otherwise fall back to the higher value.
            const bool deckAPlaying = playerA && playerA->isAudible();
            const bool deckBPlaying = playerB && playerB->isAudible();
            const double maxLatency = deckAPlaying && !deckBPlaying ? latencyA
                                     : deckBPlaying && !deckAPlaying ? latencyB
                                     : std::max(latencyA, latencyB);

            auto* device = deviceManager.getCurrentAudioDevice();
            const double sampleRate = device ? device->getCurrentSampleRate() : 0.0;
            const int bufferSize = device ? device->getCurrentBufferSizeSamples() : 0;

            menuBar->updateAudioLatency(maxLatency, sampleRate, bufferSize);
            
            // Update waveform displays with measured latency for perfect audio-visual sync
            const double latencyASec = latencyA / 1000.0; // Convert ms to seconds
            const double latencyBSec = latencyB / 1000.0;
            
            if (overviewTopA) {
                overviewTopA->setOutputLatencyComp(latencyASec + userRenderLatencySec);
            }
            if (overviewTopB) {
                overviewTopB->setOutputLatencyComp(latencyBSec + userRenderLatencySec);
            }
        }
    });
    latencyTimer->start(100); // Update every 100ms
    
    if (!sharedFormatManager) {
        auto manager = std::make_shared<juce::AudioFormatManager>();
        manager->registerBasicFormats();

        sharedFormatManager = std::move(manager);
    }

    bpmThreadPool = std::make_unique<QThreadPool>();

    const int idealThreads = QThread::idealThreadCount();
    const int maxBmpThreads = std::min(4, std::max(2, idealThreads / 2));
    bpmThreadPool->setMaxThreadCount(maxBmpThreads);
    bpmThreadPool->setExpiryTimeout(30000);

    // Create a dedicated single-threaded, low-priority pool for waveform streaming
    waveformThreadPool = std::make_unique<QThreadPool>();
    waveformThreadPool->setMaxThreadCount(1);
    waveformThreadPool->setExpiryTimeout(60000);

    playerA = std::make_unique<DJAudioPlayer>(*sharedFormatManager);
    playerB = std::make_unique<DJAudioPlayer>(*sharedFormatManager);

    deckA = new QtDeckWidget(playerA.get(), this, "DECK 1", true);
    deckB = new QtDeckWidget(playerB.get(), this, "DECK 2", false);

    scratchEngineA = std::make_unique<ScratchEngine>(this);
    scratchEngineB = std::make_unique<ScratchEngine>(this);
    if (deckA) {
        deckA->setScratchEngine(scratchEngineA.get());
        if (deckA->getTurntable()) {
            ScratchEngine::TrackConfig cfg;
            cfg.lengthSeconds = 0.0;
            cfg.prerollSeconds = deckA->getTurntable()->getPrerollSeconds();
            scratchEngineA->setTrackConfig(cfg);
        }
    }
    if (deckB) {
        deckB->setScratchEngine(scratchEngineB.get());
        if (deckB->getTurntable()) {
            ScratchEngine::TrackConfig cfg;
            cfg.lengthSeconds = 0.0;
            cfg.prerollSeconds = deckB->getTurntable()->getPrerollSeconds();
            scratchEngineB->setTrackConfig(cfg);
        }
    }

    QTimer::singleShot(100, this, [this]() {
        applyDeckSettings();
    });

    // Top overview waveforms (two stacked, centered playhead)
    overviewTopA = new WaveformDisplay(this);
    overviewTopB = new WaveformDisplay(this);
    overviewTopA->setScrollMode(true);
    overviewTopB->setScrollMode(true);
    overviewTopA->setScratchEngine(scratchEngineA.get());
    overviewTopB->setScratchEngine(scratchEngineB.get());
    // Load and apply global render latency compensation for waveform alignment
    {
        QSettings s("DJDavid", "David");
        userRenderLatencySec = s.value("renderLatency/global", 0.0).toDouble();
        overviewTopA->setOutputLatencyComp(userRenderLatencySec);
        overviewTopB->setOutputLatencyComp(userRenderLatencySec);
    }
    // Click-to-seek on top overview waveforms (works while paused)
    connect(overviewTopA, &WaveformDisplay::positionClicked, this, [this](double absRel){
        if (!playerA) return;
        absRel = std::clamp(absRel, 0.0, 1.0);
        playerA->setPositionRelative(absRel);
        // Update visuals immediately so paused seeking feels instant
        overviewTopA->setPlayhead(absRel);
        if (deckA && deckA->getWaveform()) deckA->getWaveform()->setPlayhead(absRel);
        // Also update beat indicator time to audible-relative base (no latency here on paused seek)
        if (beatIndicator) {
            double len = std::max(1e-9, playerA->getLengthInSeconds());
            beatIndicator->setTrackPositionDeckA(absRel * len);
        }
    });
    connect(overviewTopB, &WaveformDisplay::positionClicked, this, [this](double absRel){
        if (!playerB) return;
        absRel = std::clamp(absRel, 0.0, 1.0);
        playerB->setPositionRelative(absRel);
        overviewTopB->setPlayhead(absRel);
        if (deckB && deckB->getWaveform()) deckB->getWaveform()->setPlayhead(absRel);
        if (beatIndicator) {
            double len = std::max(1e-9, playerB->getLengthInSeconds());
            beatIndicator->setTrackPositionDeckB(absRel * len);
        }
    });

    connect(overviewTopA, &WaveformDisplay::tempoDragRequested, this, [this](double factor) {
        if (deckA) {
            deckA->setTempoFactor(factor);
        } else if (playerA) {
            playerA->setSpeed(factor);
        }
    });
    connect(overviewTopB, &WaveformDisplay::tempoDragRequested, this, [this](double factor) {
        if (deckB) {
            deckB->setTempoFactor(factor);
        } else if (playerB) {
            playerB->setSpeed(factor);
        }
    });

    connWaveformRegionA = connect(overviewTopA, &WaveformDisplay::waveformRegionNeeded, this, [this](double startSec, double endSec) {
        handleWaveformRegionRequest(true, startSec, endSec);
    });
    connWaveformRegionB = connect(overviewTopB, &WaveformDisplay::waveformRegionNeeded, this, [this](double startSec, double endSec) {
        handleWaveformRegionRequest(false, startSec, endSec);
    });

    // Beat indicator for showing current beat position
    beatIndicator = new BeatIndicator(this);
    
    // Connect beat indicator to deck widgets for performance pads
    deckA->setBeatIndicator(beatIndicator);
    deckB->setBeatIndicator(beatIndicator);

    auto connectScratchEngine = [this](ScratchEngine* engine, bool isDeckA) {
        if (!engine) {
            return;
        }
        connect(engine, &ScratchEngine::scratchStarted, this, [this, isDeckA](ScratchEngine::Controller controller) {
            Q_UNUSED(controller);
            handleScratchStart(isDeckA);
        });
        connect(engine, &ScratchEngine::positionChanged, this, [this, isDeckA](double seconds, double relative) {
            Q_UNUSED(seconds);
            applyScratchPosition(isDeckA, relative);
        });
        connect(engine, &ScratchEngine::velocityChanged, this, [this, isDeckA](double velocity) {
            handleScratchVelocityChanged(isDeckA, velocity);
        });
        connect(engine, &ScratchEngine::scratchEnded, this, [this, isDeckA](double releaseVelocity) {
            handleScratchEnd(isDeckA, releaseVelocity);
        });
    };

    connectScratchEngine(scratchEngineA.get(), true);
    connectScratchEngine(scratchEngineB.get(), false);

    connect(deckA, &QtDeckWidget::fileLoadingStarted, [this](const QString& filePath) {
        if (!filePath.isEmpty()) {
            // Start audio file loading in background thread
            bpmThreadPool->start(new AudioFileLoadTask(this, filePath, true));
        }
    });
    
    connect(deckB, &QtDeckWidget::fileLoadingStarted, [this](const QString& filePath) {
        if (!filePath.isEmpty()) {
            // Start audio file loading in background thread
            bpmThreadPool->start(new AudioFileLoadTask(this, filePath, false));
        }
    });

        // When deck files load, forward to overview waveforms and reset position
    connect(deckA, &QtDeckWidget::fileLoadingStarted, this, [this](const QString&) {
        if (overviewTopA)
            overviewTopA->clearCuePoints();
    });

    connect(deckA, &QtDeckWidget::fileLoaded, [this]() {
        if (playerA) [[likely]] playerA->setPitchBendRatio(1.0);
        const QString filePath = deckA->getCurrentFilePath();
        if (!filePath.isEmpty()) [[likely]] {
            // Run top waveform display setup on dedicated waveform thread to avoid any interference
            if (waveformThreadPool) waveformThreadPool->start(new TopWaveformDisplayTask(this, filePath, true));
            startDeckAnalysisIfNeeded(filePath, true);
        }
    });

    connect(deckA, &QtDeckWidget::fileLoaded, this, [this]() {
        applyStoredCuePoints(deckA, true);
        applyStoredBeatGrid(deckA, true);
        loadAndApplyCoverArt(deckA, true);  // NEW: Load cover art from database
    });

    connect(deckB, &QtDeckWidget::fileLoadingStarted, this, [this](const QString&) {
        if (overviewTopB)
            overviewTopB->clearCuePoints();
    });

    connect(deckB, &QtDeckWidget::fileLoaded, [this]() {
        if (playerB) [[likely]] playerB->setPitchBendRatio(1.0);
        const QString filePath = deckB->getCurrentFilePath();
        if (!filePath.isEmpty()) [[likely]] {
            if (waveformThreadPool) waveformThreadPool->start(new TopWaveformDisplayTask(this, filePath, false));
            startDeckAnalysisIfNeeded(filePath, false);
        }
    });    connect(deckB, &QtDeckWidget::fileLoaded, this, [this]() {
        applyStoredCuePoints(deckB, false);
        applyStoredBeatGrid(deckB, false);
        loadAndApplyCoverArt(deckB, false);  // NEW: Load cover art from database
    });

    // Clear visuals and indicator when a deck unloads
    connect(deckA, &QtDeckWidget::fileUnloaded, this, [this]() {
        if (overviewTopA) {
            overviewTopA->clearDisplay();
        }
        if (beatIndicator) {
            beatIndicator->setBeatGridAvailableDeckA(false);
        }
        if (playerA) {
            playerA->setPitchBendRatio(1.0);
        }
        analysisActiveA = false; analysisFailedA = false; analysisProgressA = 0.0; algorithmA.clear();
        updateOverviewLabel(true);
    });
    connect(deckB, &QtDeckWidget::fileUnloaded, this, [this]() {
        if (overviewTopB) {
            overviewTopB->clearDisplay();
        }
        if (beatIndicator) {
            beatIndicator->setBeatGridAvailableDeckB(false);
        }
        if (playerB) {
            playerB->setPitchBendRatio(1.0);
        }
        analysisActiveB = false; analysisFailedB = false; analysisProgressB = 0.0; algorithmB.clear();
        updateOverviewLabel(false);
    });
    // When playhead updates on deck, update overview playhead and beat indicator
    connect(deckA, &QtDeckWidget::playheadUpdated, this, [this](double relative) {
        double deviceLatencySec = 0.0;
        if (auto* dev = deviceManager.getCurrentAudioDevice()) {
            const double sr = dev->getCurrentSampleRate();
            if (sr > 0.0) {
                // Prefer device-reported output latency, else approximate with 1.5x buffer size
                const int buf = dev->getCurrentBufferSizeSamples();
                const int outLat = dev->getOutputLatencyInSamples();
                if (outLat > 0) deviceLatencySec = outLat / sr; else deviceLatencySec = (buf > 0 ? (1.5 * buf) / sr : 0.0);
            }
        }
        double pipelineLatencySec = playerA ? playerA->getPipelineLatencySeconds() : 0.0;
        // Align center with audible output: subtract total playback latency
        double visualDelay = std::clamp(pipelineLatencySec + deviceLatencySec, 0.0, 0.25);
        // Predict frame/display pipeline delay to the next vsync to avoid visual lead
        constexpr double uiFudgeSec = 0.012; // ~12 ms safety (display/vsync)
        // Apply optional user trim (positive delays visuals more)
        double totalDelay = visualDelay + uiFudgeSec + std::clamp(userVisualTrimA, -0.05, 0.05);
        // Compute audible-relative playhead and feed it directly
        if (playerA) {
            double displayRel = relative;
            if (relative >= 0.0) {
                double len = playerA->getLengthInSeconds();
                if (len > 1e-6) {
                    displayRel = relative - (totalDelay / len);
                }
                displayRel = std::clamp(displayRel, 0.0, 1.0);
            }
            overviewTopA->setPlayhead(displayRel);
            if (deckA && deckA->getWaveform()) deckA->getWaveform()->setPlayhead(displayRel);
        } else {
            overviewTopA->setPlayhead(relative);
            if (deckA && deckA->getWaveform()) deckA->getWaveform()->setPlayhead(relative);
        }
        // Update beat indicator with audible time in seconds (support preroll)
        if (playerA) {
            double curSec = playerA->getCurrentPositionSeconds();
            double audibleTimeSec = curSec - totalDelay; // do not clamp; may be negative in preroll
            beatIndicator->setTrackPositionDeckA(audibleTimeSec);
        }
    });
    connect(deckB, &QtDeckWidget::playheadUpdated, this, [this](double relative) {
        double deviceLatencySec = 0.0;
        if (auto* dev = deviceManager.getCurrentAudioDevice()) [[likely]] {
            const double sr = dev->getCurrentSampleRate();
            if (sr > 0.0) [[likely]] {
                const int buf = dev->getCurrentBufferSizeSamples();
                const int outLat = dev->getOutputLatencyInSamples();
                deviceLatencySec = (outLat > 0) ? outLat / sr : ((buf > 0) ? (1.5 * buf) / sr : 0.0);
            }
        }
        const double pipelineLatencySec = playerB ? playerB->getPipelineLatencySeconds() : 0.0;
        constexpr double uiFudgeSec = 0.012;
        const double totalDelay = std::clamp(pipelineLatencySec + deviceLatencySec, 0.0, 0.25) + uiFudgeSec + std::clamp(userVisualTrimB, -0.05, 0.05);
        if (playerB) [[likely]] {
            double displayRel = relative;
            if (relative >= 0.0) [[likely]] {
                const double len = playerB->getLengthInSeconds();
                if (len > 1e-6) [[likely]] displayRel = std::clamp(relative - (totalDelay / len), 0.0, 1.0);
            }
            overviewTopB->setPlayhead(displayRel);
            if (deckB && deckB->getWaveform()) [[likely]] deckB->getWaveform()->setPlayhead(displayRel);
            beatIndicator->setTrackPositionDeckB(playerB->getCurrentPositionSeconds() - totalDelay);
        } else {
            overviewTopB->setPlayhead(relative);
            if (deckB && deckB->getWaveform()) deckB->getWaveform()->setPlayhead(relative);
        }
    });

    connect(deckA, &QtDeckWidget::tempoFactorChanged, overviewTopA, &WaveformDisplay::setTempoFactor);
    connect(deckB, &QtDeckWidget::tempoFactorChanged, overviewTopB, &WaveformDisplay::setTempoFactor);
    
    connect(deckA, &QtDeckWidget::playStateChanged, this, [this](bool playing) {
        if (!playing && overviewTopA) overviewTopA->stopPlayback();
    });
    connect(deckB, &QtDeckWidget::playStateChanged, this, [this](bool playing) {
        if (!playing && overviewTopB) overviewTopB->stopPlayback();
    });
    
    // Connect cue points from performance pads to top waveform displays
    if (deckA->getPerformancePads()) {
        connect(deckA->getPerformancePads(), &PerformancePads::cuePointsChanged, overviewTopA, &WaveformDisplay::setCuePoints);
        connect(deckA->getPerformancePads(), &PerformancePads::cuePointsChanged, this, [this](const std::array<double, 8>& cues){
            if (!libraryManager || !deckA)
                return;
            const QString filePath = deckA->getCurrentFilePath();
            if (filePath.isEmpty())
                return;
            libraryManager->saveCuePointsForTrack(filePath, cues);
        });
    }
    if (deckB->getPerformancePads()) {
        connect(deckB->getPerformancePads(), &PerformancePads::cuePointsChanged, overviewTopB, &WaveformDisplay::setCuePoints);
        connect(deckB->getPerformancePads(), &PerformancePads::cuePointsChanged, this, [this](const std::array<double, 8>& cues){
            if (!libraryManager || !deckB)
                return;
            const QString filePath = deckB->getCurrentFilePath();
            if (filePath.isEmpty())
                return;
            libraryManager->saveCuePointsForTrack(filePath, cues);
        });
    }
    
    // Connect cue points from performance pads to deck waveform overviews
    if (deckA->getPerformancePads() && deckA->getWaveform()) {
        connect(deckA->getPerformancePads(), &PerformancePads::cuePointsChanged, deckA->getWaveform(), &DeckWaveformOverview::setCuePoints);
    }
    if (deckB->getPerformancePads() && deckB->getWaveform()) {
        connect(deckB->getPerformancePads(), &PerformancePads::cuePointsChanged, deckB->getWaveform(), &DeckWaveformOverview::setCuePoints);
    }
    
    // Connect loop status from decks to top waveform displays
    connect(deckA, &QtDeckWidget::loopChanged, overviewTopA, &WaveformDisplay::setLoopRegion);
    connect(deckB, &QtDeckWidget::loopChanged, overviewTopB, &WaveformDisplay::setLoopRegion);
    
    // Connect loop status from decks to deck waveform overviews
    if (deckA->getWaveform()) {
        connect(deckA, &QtDeckWidget::loopChanged, deckA->getWaveform(), &DeckWaveformOverview::setLoopRegion);
    }
    if (deckB->getWaveform()) {
        connect(deckB, &QtDeckWidget::loopChanged, deckB->getWaveform(), &DeckWaveformOverview::setLoopRegion);
    }
    
    // Connect ghost loop status from performance pads to top waveform displays
    if (deckA->getPerformancePads()) {
        connect(deckA->getPerformancePads(), &PerformancePads::ghostLoopChanged, overviewTopA, &WaveformDisplay::setGhostLoopRegion);
    }
    if (deckB->getPerformancePads()) {
        connect(deckB->getPerformancePads(), &PerformancePads::ghostLoopChanged, overviewTopB, &WaveformDisplay::setGhostLoopRegion);
    }
    
    // Connect ghost loop status from performance pads to deck waveform overviews
    if (deckA->getPerformancePads() && deckA->getWaveform()) {
        connect(deckA->getPerformancePads(), &PerformancePads::ghostLoopChanged, deckA->getWaveform(), &DeckWaveformOverview::setGhostLoopRegion);
    }
    if (deckB->getPerformancePads() && deckB->getWaveform()) {
        connect(deckB->getPerformancePads(), &PerformancePads::ghostLoopChanged, deckB->getWaveform(), &DeckWaveformOverview::setGhostLoopRegion);
    }
    
    // Also update BeatIndicator per-deck when tempo factor changes and follow sync
    connect(deckA, &QtDeckWidget::tempoFactorChanged, this, [this](double factor){
        if (beatIndicator) beatIndicator->setTempoFactorDeckA(factor);
        if (syncUpdateInProgress) return;
        // If Deck B is set to follow (sync enabled on B), update B to A
        if (syncBEnabled && deckB && deckA) {
            double masterBpm = deckA->getDetectedBpm();
            double masterEff = masterBpm > 0 ? masterBpm * deckA->getTempoFactor() : 0.0;
            double targetBpm = deckB->getDetectedBpm();
            if (masterEff > 0.0 && targetBpm > 0.0) {
                double desired = masterEff / targetBpm;
                syncUpdateInProgress = true; // Prevent feedback loop
                deckB->setTempoFactor(desired);
                if (playerB) playerB->setSpeed(desired); // Direct audio sync
                
                // Force immediate waveform update for visual sync
                if (overviewTopB) {
                    overviewTopB->setTempoFactor(desired);
                    overviewTopB->update();
                }
                syncUpdateInProgress = false;
            }
        }
    });
    connect(deckB, &QtDeckWidget::tempoFactorChanged, this, [this](double factor){
        if (beatIndicator) beatIndicator->setTempoFactorDeckB(factor);
        if (syncUpdateInProgress) return;
        // If Deck A is set to follow (sync enabled on A), update A to B
        if (syncAEnabled && deckA && deckB) {
            double masterBpm = deckB->getDetectedBpm();
            double masterEff = masterBpm > 0 ? masterBpm * deckB->getTempoFactor() : 0.0;
            double targetBpm = deckA->getDetectedBpm();
            if (masterEff > 0.0 && targetBpm > 0.0) {
                double desired = masterEff / targetBpm;
                syncUpdateInProgress = true; // Prevent feedback loop
                deckA->setTempoFactor(desired);
                if (playerA) playerA->setSpeed(desired); // Direct audio sync
                
                // Force immediate waveform update for visual sync
                if (overviewTopA) {
                    overviewTopA->setTempoFactor(desired);
                    overviewTopA->update();
                }
                syncUpdateInProgress = false;
            }
        }
    });

    // Synchronize zoom levels between all waveform displays
    connect(overviewTopA, &WaveformDisplay::zoomLevelChanged, this, [this](int newLevel) {
        // Synchronize all other waveform displays to the same zoom level
        if (overviewTopB) {
            overviewTopB->setBeatGridZoomLevel(newLevel);
        }
    });
    connect(overviewTopB, &WaveformDisplay::zoomLevelChanged, this, [this](int newLevel) {
        // Synchronize all other waveform displays to the same zoom level
        if (overviewTopA) {
            overviewTopA->setBeatGridZoomLevel(newLevel);
        }
    });

    auto doSync = [this](QtDeckWidget* requester){
        if (!requester || !deckA || !deckB || !playerA || !playerB) [[unlikely]] return;
        QtDeckWidget* masterDeck = (requester == deckA) ? deckB : deckA;
        QtDeckWidget* targetDeck = requester;
        DJAudioPlayer* masterPlayer = (requester == deckA) ? playerB.get() : playerA.get();
        DJAudioPlayer* targetPlayer = (requester == deckA) ? playerA.get() : playerB.get();

        const double masterBpm = masterDeck->getDetectedBpm();
        const double targetBpm = targetDeck->getDetectedBpm();
        if (masterBpm <= 0.0 || targetBpm <= 0.0) [[unlikely]] return;
        const double masterFactor = masterDeck->getTempoFactor();
        const double desiredFactor = (masterBpm * masterFactor) / targetBpm;
        
        targetDeck->setTempoFactor(desiredFactor);

        if (requester == deckA && overviewTopA) {
            overviewTopA->setTempoFactor(desiredFactor);
            overviewTopA->update();
        } else if (requester == deckB && overviewTopB) {
            overviewTopB->setTempoFactor(desiredFactor);
            overviewTopB->update();
        }

        double mBpm = masterPlayer->getTrackBpm();
        double mOffset = masterPlayer->getFirstBeatOffset();
        double tBpm = targetPlayer->getTrackBpm();
        double tOffset = targetPlayer->getFirstBeatOffset();
        if (mBpm > 0.0 && tBpm > 0.0) {
            double mBeatLen = 60.0 / (mBpm * masterFactor);
            // Current absolute times
            double mTime = masterPlayer->getCurrentPositionSeconds();
            double tTime = targetPlayer->getCurrentPositionSeconds();
            // Phase within bar [0, beatLen)
            auto phase = [](double time, double offset, double beatLen){
                double rel = time - offset;
                double mod = std::fmod(rel, beatLen);
                if (mod < 0) mod += beatLen;
                return mod;
            };
            double masterPhase = phase(mTime, mOffset, mBeatLen);
            // Target beat length at its new speed equals master beat length (by construction)
            double targetPhase = phase(tTime, tOffset, mBeatLen);
            double delta = masterPhase - targetPhase; // seconds to nudge
            if (std::abs(delta) > mBeatLen/2.0) {
                if (delta > 0) delta -= mBeatLen; else delta += mBeatLen;
            }
            double newTime = tTime + delta;
            // Clamp within track
            newTime = std::clamp(newTime, 0.0, targetPlayer->getLengthInSeconds());
            targetPlayer->setPositionSeconds(newTime);
        }
    };
    connect(deckA, &QtDeckWidget::syncRequested, this, doSync);
    connect(deckB, &QtDeckWidget::syncRequested, this, doSync);

    // SYNC toggle: enable/disable follow mode
    connect(deckA, &QtDeckWidget::syncToggled, this, [this, doSync](QtDeckWidget* who, bool enabled){
        // who == deckA means A wants to follow B when enabled
        syncAEnabled = enabled;
        if (enabled) doSync(who); // immediate align
    });
    connect(deckB, &QtDeckWidget::syncToggled, this, [this, doSync](QtDeckWidget* who, bool enabled){
        // who == deckB means B wants to follow A when enabled
        syncBEnabled = enabled;
        if (enabled) doSync(who); // immediate align
    });

    // Update overview labels to show only original analyzed BPM (not speed-scaled)
    connect(deckA, &QtDeckWidget::displayedBpmChanged, this, [this](double){ updateOverviewLabel(true); });
    connect(deckB, &QtDeckWidget::displayedBpmChanged, this, [this](double){ updateOverviewLabel(false); });

    // Defer audio device initialization until after Qt setup
    QTimer::singleShot(100, this, [this]() {
        initializeAudio();
    });

    // Initialize new LibraryManager with ID3 tag support
    libraryManager = new LibraryManager(sharedFormatManager, this);
    
    // Connect library manager signals to deck loading
    connect(libraryManager, &LibraryManager::fileSelected, this, [this](const QString& filePath) {
        // Double-click loads to the currently focused deck or deck A by default
        if (deckA && deckA->hasFocus()) {
            deckA->loadFile(filePath);
        } else if (deckB && deckB->hasFocus()) {
            deckB->loadFile(filePath);
        } else {
            // Default to deck A if no deck has focus
            if (deckA) deckA->loadFile(filePath);
        }
    });

    // Explicit context-menu deck loading
    connect(libraryManager, &LibraryManager::loadToDeck, this, &QtMainWindow::onLibraryLoadToDeck);
    connect(libraryManager, &LibraryManager::analyzeTracksRequested, this, &QtMainWindow::onAnalyzeTracksRequested);
    connect(libraryManager, &LibraryManager::analyzeTracksAdvancedRequested, this, &QtMainWindow::onAnalyzeTracksAdvancedRequested);
    
    // Auto-populate with user's Music folder on startup
    QTimer::singleShot(500, this, [this]() {
        QDir musicDir(QDir::homePath());
        musicDir.cd("Music");
        if (musicDir.exists()) {
            libraryManager->addDirectory(musicDir.absolutePath(), false); // Non-recursive for quick startup
        }
    });

    crossfader = new CustomFader(CustomFader::Horizontal, this);
    crossfader->setMinimum(-100);
    crossfader->setMaximum(100);
    crossfader->setValue(0);
    connect(crossfader, &CustomFader::valueChanged, this, &QtMainWindow::onCrossfader);

    // Rekordbox-style layout with Serato-style overview waveforms at top
    // Top section: Two stacked overview waveforms (Serato style)
    auto overviewLayout = new QVBoxLayout;
    
    // Style the overview waveforms (increase height ~2x as requested)
    overviewTopA->setFixedHeight(70);
    overviewTopB->setFixedHeight(70);
    overviewTopA->setStyleSheet("border: none; background-color: #0a0a0a;");
    overviewTopB->setStyleSheet("border: none; background-color: #0a0a0a;");

    overviewLayout->setSpacing(0);
    overviewLayout->setContentsMargins(0, 0, 0, 0);
    overviewLayout->addWidget(overviewTopA);
    overviewLayout->addWidget(overviewTopB);

    updateOverviewLabel(true);
    updateOverviewLabel(false);
    
    // Main deck controls side by side
    auto decksLayout = new QHBoxLayout;
    decksLayout->setSpacing(8);
    decksLayout->setContentsMargins(0, 0, 0, 0);
    decksLayout->addWidget(deckA->getControlsWidget(), 3);  // Deck A gets more space
    
    // ========== FLAT MIXER SECTION ==========
    auto mixerSection = new QVBoxLayout;
    mixerSection->setSpacing(4);
    mixerSection->setContentsMargins(8, 6, 8, 6);
    
    // Main mixer row - flat responsive layout
    auto mainMixerRow = new QHBoxLayout;
    mainMixerRow->setSpacing(8);
    
    // === DECK A SECTION ===
    auto deckASection = new QHBoxLayout;
    deckASection->setSpacing(4);
    
    // Deck A Knobs (Trim + EQ + Filter)
    auto leftEqLayout = new QVBoxLayout;
    leftEqLayout->setSpacing(2);
    leftEqLayout->setAlignment(Qt::AlignCenter);
    
    leftTrim = new DJKnob(this);
    leftTrim->setRange(-100, 100);
    leftTrim->setValue(0);
    leftTrim->setToolTip("Trim A (Gain)");
    
    leftHigh = new DJKnob(this);
    leftHigh->setRange(-100, 100);
    leftHigh->setValue(0);
    leftHigh->setToolTip("High");
    
    leftMid = new DJKnob(this);
    leftMid->setRange(-100, 100);
    leftMid->setValue(0);
    leftMid->setToolTip("Mid");
    
    leftLow = new DJKnob(this);
    leftLow->setRange(-100, 100);
    leftLow->setValue(0);
    leftLow->setToolTip("Low");
    
    leftFilter = new DJKnob(this);
    leftFilter->setRange(-100, 100);
    leftFilter->setValue(0);
    leftFilter->setToolTip("Filter");
    
    leftEqLayout->addWidget(leftTrim);
    leftEqLayout->addWidget(leftHigh);
    leftEqLayout->addWidget(leftMid);
    leftEqLayout->addWidget(leftLow);
    leftEqLayout->addWidget(leftFilter);
    
    // Deck A VU Meter - schmaler und links neben den Knobs
    auto deckAVULayout = new QVBoxLayout;
    deckAVULayout->setSpacing(0);
    deckAVULayout->setAlignment(Qt::AlignCenter);
    auto deckAVULabel = new QLabel("A", this);
    deckAVULabel->setAlignment(Qt::AlignCenter);
    deckAVULabel->setStyleSheet("color: #888; font-size: 8px; font-weight: bold;");
    vuMeterDeckA = new VUMeter(VUMeter::Channel, this);
    vuMeterDeckA->setFixedSize(8, 170);  // Schmaler (8px) und Höhe = 5 Knobs (32*5 + 4*2 spacing = 170)
    deckAVULayout->addWidget(deckAVULabel);
    deckAVULayout->addWidget(vuMeterDeckA);
    
    // VU Meter links, dann Knobs
    deckASection->addLayout(deckAVULayout);
    deckASection->addLayout(leftEqLayout);
    
    // Deck A Volume + Cue
    auto leftVolLayout = new QVBoxLayout;
    leftVolLayout->setSpacing(2);
    leftVolLayout->setAlignment(Qt::AlignCenter);
    
    auto leftVolLabel = new QLabel("VOL", this);
    leftVolLabel->setAlignment(Qt::AlignCenter);
    leftVolLabel->setStyleSheet("color: #888; font-size: 8px; font-weight: bold;");
    
    leftVolumeSlider = new CustomFader(CustomFader::Vertical, this);
    leftVolumeSlider->setMinimum(0);
    leftVolumeSlider->setMaximum(100);
    leftVolumeSlider->setValue(100);
    leftVolumeSlider->setMinimumHeight(70);
    
    leftCueButton = new QPushButton("CUE", this);
    leftCueButton->setCheckable(true);
    leftCueButton->setFixedHeight(20);
    leftCueButton->setFixedWidth(35);
    leftCueButton->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: #666; border: 1px solid #333; border-radius: 2px; font-size: 8px; font-weight: bold; }"
        "QPushButton:checked { background: #00ff00; color: #000; border: 1px solid #00cc00; }"
        "QPushButton:hover { background: #2a2a2a; }"
    );
    
    leftVolLayout->addWidget(leftVolLabel);
    leftVolLayout->addWidget(leftVolumeSlider, 1);
    leftVolLayout->addWidget(leftCueButton);
    
    deckASection->addLayout(leftVolLayout);
    
    // === MASTER SECTION ===
    auto masterSection = new QVBoxLayout;
    masterSection->setSpacing(2);
    masterSection->setAlignment(Qt::AlignCenter);
    
    auto masterLabel = new QLabel("MASTER", this);
    masterLabel->setAlignment(Qt::AlignCenter);
    masterLabel->setStyleSheet("color: #aaa; font-size: 9px; font-weight: bold;");
    
    auto masterMetersLayout = new QHBoxLayout;
    masterMetersLayout->setSpacing(4);
    
    auto masterLLayout = new QVBoxLayout;
    masterLLayout->setSpacing(1);
    auto masterLLabel = new QLabel("L", this);
    masterLLabel->setAlignment(Qt::AlignCenter);
    masterLLabel->setStyleSheet("color: #666; font-size: 8px;");
    vuMeterMasterL = new VUMeter(VUMeter::Master, this);
    masterLLayout->addWidget(masterLLabel);
    masterLLayout->addWidget(vuMeterMasterL);
    
    auto masterRLayout = new QVBoxLayout;
    masterRLayout->setSpacing(1);
    auto masterRLabel = new QLabel("R", this);
    masterRLabel->setAlignment(Qt::AlignCenter);
    masterRLabel->setStyleSheet("color: #666; font-size: 8px;");
    vuMeterMasterR = new VUMeter(VUMeter::Master, this);
    masterRLayout->addWidget(masterRLabel);
    masterRLayout->addWidget(vuMeterMasterR);
    
    masterMetersLayout->addLayout(masterLLayout);
    masterMetersLayout->addLayout(masterRLayout);
    
    masterSection->addWidget(masterLabel);
    masterSection->addLayout(masterMetersLayout);
    
    // === DECK B SECTION ===
    auto deckBSection = new QHBoxLayout;
    deckBSection->setSpacing(4);
    
    // Deck B Volume + Cue
    auto rightVolLayout = new QVBoxLayout;
    rightVolLayout->setSpacing(2);
    rightVolLayout->setAlignment(Qt::AlignCenter);
    
    auto rightVolLabel = new QLabel("VOL", this);
    rightVolLabel->setAlignment(Qt::AlignCenter);
    rightVolLabel->setStyleSheet("color: #888; font-size: 8px; font-weight: bold;");
    
    rightVolumeSlider = new CustomFader(CustomFader::Vertical, this);
    rightVolumeSlider->setMinimum(0);
    rightVolumeSlider->setMaximum(100);
    rightVolumeSlider->setValue(100);
    rightVolumeSlider->setMinimumHeight(70);
    
    rightCueButton = new QPushButton("CUE", this);
    rightCueButton->setCheckable(true);
    rightCueButton->setFixedHeight(20);
    rightCueButton->setFixedWidth(35);
    rightCueButton->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: #666; border: 1px solid #333; border-radius: 2px; font-size: 8px; font-weight: bold; }"
        "QPushButton:checked { background: #00ff00; color: #000; border: 1px solid #00cc00; }"
        "QPushButton:hover { background: #2a2a2a; }"
    );
    
    rightVolLayout->addWidget(rightVolLabel);
    rightVolLayout->addWidget(rightVolumeSlider, 1);
    rightVolLayout->addWidget(rightCueButton);
    
    // Deck B Knobs (Trim + EQ + Filter)
    auto rightEqLayout = new QVBoxLayout;
    rightEqLayout->setSpacing(2);
    rightEqLayout->setAlignment(Qt::AlignCenter);
    
    rightTrim = new DJKnob(this);
    rightTrim->setRange(-100, 100);
    rightTrim->setValue(0);
    rightTrim->setToolTip("Trim B (Gain)");
    
    rightHigh = new DJKnob(this);
    rightHigh->setRange(-100, 100);
    rightHigh->setValue(0);
    rightHigh->setToolTip("High");
    
    rightMid = new DJKnob(this);
    rightMid->setRange(-100, 100);
    rightMid->setValue(0);
    rightMid->setToolTip("Mid");
    
    rightLow = new DJKnob(this);
    rightLow->setRange(-100, 100);
    rightLow->setValue(0);
    rightLow->setToolTip("Low");
    
    rightFilter = new DJKnob(this);
    rightFilter->setRange(-100, 100);
    rightFilter->setValue(0);
    rightFilter->setToolTip("Filter");
    
    rightEqLayout->addWidget(rightTrim);
    rightEqLayout->addWidget(rightHigh);
    rightEqLayout->addWidget(rightMid);
    rightEqLayout->addWidget(rightLow);
    rightEqLayout->addWidget(rightFilter);
    
    // Deck B VU Meter - schmaler und links neben den Knobs
    auto deckBVULayout = new QVBoxLayout;
    deckBVULayout->setSpacing(0);
    deckBVULayout->setAlignment(Qt::AlignCenter);
    auto deckBVULabel = new QLabel("B", this);
    deckBVULabel->setAlignment(Qt::AlignCenter);
    deckBVULabel->setStyleSheet("color: #888; font-size: 8px; font-weight: bold;");
    vuMeterDeckB = new VUMeter(VUMeter::Channel, this);
    vuMeterDeckB->setFixedSize(8, 170);  // Schmaler (8px) und Höhe = 5 Knobs
    deckBVULayout->addWidget(deckBVULabel);
    deckBVULayout->addWidget(vuMeterDeckB);
    
    // Volume links, dann VU Meter, dann Knobs
    deckBSection->addLayout(rightVolLayout);
    deckBSection->addLayout(deckBVULayout);
    deckBSection->addLayout(rightEqLayout);
    
    // Assemble main mixer row with proper spacing
    mainMixerRow->addLayout(deckASection);
    mainMixerRow->addStretch(1);
    mainMixerRow->addLayout(masterSection);
    mainMixerRow->addStretch(1);
    mainMixerRow->addLayout(deckBSection);
    
    mixerSection->addLayout(mainMixerRow, 1);
    
    // Crossfader at the bottom
    auto crossfaderLabel = new QLabel("CROSSFADER", this);
    crossfaderLabel->setAlignment(Qt::AlignCenter);
    crossfaderLabel->setStyleSheet("color: #888888; font-size: 9px; font-weight: bold;");
    
    crossfader->setMinimumHeight(24);
    
    mixerSection->addWidget(crossfaderLabel);
    mixerSection->addWidget(crossfader);
    
    // Create flat mixer widget
    auto mixerWidget = new QWidget(this);
    mixerWidget->setLayout(mixerSection);
    mixerWidget->setMinimumWidth(220);  // Fixed minimum width
    mixerWidget->setMaximumWidth(280);  // Fixed maximum width to prevent growing
    mixerWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    mixerWidget->setStyleSheet("QWidget { background-color: #2a2a2a; }");
    
    decksLayout->addWidget(mixerWidget, 0);  // No stretch - stays fixed width
    decksLayout->addWidget(deckB->getControlsWidget(), 3);  // Deck B gets more space

    // Connect knobs to slots to control EQ and filter
    connect(leftHigh, &DJKnob::valueChanged, this, &QtMainWindow::onLeftHighChanged);
    connect(leftMid, &DJKnob::valueChanged, this, &QtMainWindow::onLeftMidChanged);
    connect(leftLow, &DJKnob::valueChanged, this, &QtMainWindow::onLeftLowChanged);
    connect(leftFilter, &DJKnob::valueChanged, this, &QtMainWindow::onLeftFilterChanged);

    connect(rightHigh, &DJKnob::valueChanged, this, &QtMainWindow::onRightHighChanged);
    connect(rightMid, &DJKnob::valueChanged, this, &QtMainWindow::onRightMidChanged);
    connect(rightLow, &DJKnob::valueChanged, this, &QtMainWindow::onRightLowChanged);
    connect(rightFilter, &DJKnob::valueChanged, this, &QtMainWindow::onRightFilterChanged);
    
    // Connect trim knobs to audio callback
    connect(leftTrim, &DJKnob::valueChanged, this, [this](int value) {
        if (stereoCallback) {
            // Convert knob value (-100 to 100) to dB (-24 to +24)
            float trimDb = (value / 100.0f) * 24.0f;
            stereoCallback->setTrimA(trimDb);
        }
    });
    
    connect(rightTrim, &DJKnob::valueChanged, this, [this](int value) {
        if (stereoCallback) {
            // Convert knob value (-100 to 100) to dB (-24 to +24)
            float trimDb = (value / 100.0f) * 24.0f;
            stereoCallback->setTrimB(trimDb);
        }
    });
    
    // Add double-click reset functionality for volume sliders and crossfader
    // Volume sliders reset to 100 (full volume)
    leftVolumeSlider->installEventFilter(this);
    rightVolumeSlider->installEventFilter(this);
    
    // Crossfader resets to 50 (center)
    crossfader->installEventFilter(this);
    
    // Connect volume sliders
    connect(leftVolumeSlider, &CustomFader::valueChanged, this, &QtMainWindow::onLeftVolumeChanged);
    connect(rightVolumeSlider, &CustomFader::valueChanged, this, &QtMainWindow::onRightVolumeChanged);
    
    // Bottom section: Library (now with LibraryManager)
    auto libLayout = new QVBoxLayout;
    auto libraryLabel = new QLabel("MUSIC LIBRARY", this);
    libraryLabel->setStyleSheet("font-weight: bold; font-size: 14px; color: #fff; padding: 5px;");
    libLayout->addWidget(libraryLabel);
    libLayout->addWidget(libraryManager, 1);

    // Main layout: Vertical stack (Compact Serato style)
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(2);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    
    // Add menu bar to the top of the layout
    mainLayout->addWidget(menuBar);
    
    // Beat indicator at the very top
    auto beatIndicatorLayout = new QHBoxLayout;
    beatIndicatorLayout->addStretch();
    beatIndicatorLayout->addWidget(beatIndicator);
    beatIndicatorLayout->addStretch();
    mainLayout->addLayout(beatIndicatorLayout, 0);
    
    mainLayout->addLayout(overviewLayout, 0);    // Overview waveforms at top (fixed size)
    mainLayout->addLayout(decksLayout, 1);       // Deck controls + mixer (compact)
    mainLayout->addLayout(libLayout, 3);         // Library at bottom (more space)
    setLayout(mainLayout);

    positionUpdateTimer = new QTimer(this);
    positionUpdateTimer->setInterval(80);
    connect(positionUpdateTimer, &QTimer::timeout, this, &QtMainWindow::updatePlaybackPositions);
    positionUpdateTimer->start();
    
    // VU meter update timer - polls audio levels and updates meters
    vuMeterUpdateTimer = new QTimer(this);
    vuMeterUpdateTimer->setInterval(30);
    connect(vuMeterUpdateTimer, &QTimer::timeout, this, [this]() {
        if (stereoCallback) {
            vuMeterDeckA->setLevel(stereoCallback->getDeckALevel());
            vuMeterDeckB->setLevel(stereoCallback->getDeckBLevel());
            vuMeterMasterL->setLevel(stereoCallback->getMasterLevelL());
            vuMeterMasterR->setLevel(stereoCallback->getMasterLevelR());
        }
    });
    vuMeterUpdateTimer->start();
    
    // Continuous waveform fill-in timer - keeps loading chunks until complete
    waveformFillInTimer = new QTimer(this);
    waveformFillInTimer->setInterval(50);
    connect(waveformFillInTimer, &QTimer::timeout, this, [this]() {
        continuousWaveformFillIn(true);
        continuousWaveformFillIn(false);
    });
    waveformFillInTimer->start();

    {
        AppConfig::instance().createDirectories();
        DeckSettings::instance().loadSettings();
        userVisualTrimA = std::clamp(DeckSettings::instance().getDeckA().visualTrim, -0.05, 0.05);
        userVisualTrimB = std::clamp(DeckSettings::instance().getDeckB().visualTrim, -0.05, 0.05);
        
        updateOverviewLabel(true);
        updateOverviewLabel(false);
    }
}


void QtMainWindow::initializeAudio()
{
    try {
        if (stereoCallback) {
            deviceManager.removeAudioCallback(stereoCallback.get());
        }
        deviceManager.removeAudioCallback(&masterLevelMonitor);

        juce::String audioError = deviceManager.initialiseWithDefaultDevices(0, 2);
        if (audioError.isNotEmpty()) [[unlikely]] {
            return;
        }

        auto* currentDevice = deviceManager.getCurrentAudioDevice();
        if (!currentDevice) [[unlikely]] {
            return;
        }

        // Request audio settings based on saved preferences (fallback to low-latency defaults)
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup(setup);

            QSettings config(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat);
            const QString currentTypeQt = QString::fromStdString(deviceManager.getCurrentAudioDeviceType().toStdString());
            const QString currentOutputNameQt = QString::fromStdString(setup.outputDeviceName.toStdString());

            const QString savedDeviceType = config.value("Audio/MasterDeviceType", currentTypeQt).toString();
            const QString savedDeviceName = config
                                                .value("Audio/MasterDevice",
                                                       config.value("Audio/Device", currentOutputNameQt).toString())
                                                .toString();
            const int savedChannelStart = std::max(0, config.value("Audio/MasterChannelStart", 0).toInt());
            const int savedChannelCount = std::max(1, config.value("Audio/MasterChannelCount", 2).toInt());
            const double savedSampleRate = config.value("Audio/SampleRate", 44100).toDouble();
            const int savedBufferSize = config.value("Audio/BufferSize", 128).toInt();

            if (!savedDeviceType.isEmpty() && savedDeviceType != currentTypeQt) {
                deviceManager.setCurrentAudioDeviceType(savedDeviceType.toStdString(), true);
                currentDevice = deviceManager.getCurrentAudioDevice();
                if (!currentDevice) [[unlikely]] {
                    return;
                }
                deviceManager.getAudioDeviceSetup(setup);
            }

            if (!savedDeviceName.isEmpty()) {
                setup.outputDeviceName = juce::String(savedDeviceName.toStdString());
            }

            setup.useDefaultOutputChannels = false;
            setup.outputChannels.clear();
            for (int ch = 0; ch < savedChannelCount; ++ch) {
                setup.outputChannels.setBit(savedChannelStart + ch);
            }
            if (setup.outputChannels.isZero()) {
                setup.outputChannels.setBit(0);
                setup.outputChannels.setBit(1);
            }
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();

            const double srHint = (setup.sampleRate > 0.0) ? setup.sampleRate : currentDevice->getCurrentSampleRate();
            const double effectiveSr = (srHint > 0.0) ? srHint : 44100.0;

            const auto juceRates = currentDevice->getAvailableSampleRates();
            std::vector<double> availableRates;
            availableRates.reserve((size_t) juceRates.size());
            for (int i = 0; i < juceRates.size(); ++i) {
                availableRates.push_back(juceRates.getUnchecked(i));
            }
            const auto pickRate = [&]() -> double {
                const double targetRate = (savedSampleRate > 0.0) ? savedSampleRate : effectiveSr;
                if (availableRates.empty()) {
                    return targetRate;
                }
                if (std::ranges::any_of(availableRates, [targetRate](double rate) {
                        return std::abs(rate - targetRate) < 1.0;
                    })) {
                    return targetRate;
                }
                const auto nearest = std::ranges::min_element(availableRates, [targetRate](double lhs, double rhs) {
                    return std::abs(lhs - targetRate) < std::abs(rhs - targetRate);
                });
                return (nearest != availableRates.end()) ? *nearest : targetRate;
            }();

            const auto juceSizes = currentDevice->getAvailableBufferSizes();
            std::vector<int> availableSizes;
            availableSizes.reserve((size_t) juceSizes.size());
            for (int i = 0; i < juceSizes.size(); ++i) {
                availableSizes.push_back(juceSizes.getUnchecked(i));
            }
            const auto pickSize = [&]() -> int {
                const int targetSamples = std::clamp(savedBufferSize, 48, 4096);
                if (availableSizes.empty()) {
                    return targetSamples;
                }
                if (auto exact = std::ranges::find(availableSizes, targetSamples); exact != availableSizes.end()) {
                    return *exact;
                }
                const auto nearest = std::ranges::min_element(availableSizes, [targetSamples](int lhs, int rhs) {
                    return std::abs(lhs - targetSamples) < std::abs(rhs - targetSamples);
                });
                return std::clamp((nearest != availableSizes.end()) ? *nearest : targetSamples, 48, 4096);
            }();

            setup.sampleRate = pickRate;
            setup.bufferSize = pickSize;

            const juce::String error = deviceManager.setAudioDeviceSetup(setup, true);
            if (error.isNotEmpty()) {
                qWarning() << "Failed to apply saved audio device setup:" << QString::fromStdString(error.toStdString());
            }

            currentDevice = deviceManager.getCurrentAudioDevice();
            if (!currentDevice) [[unlikely]] {
                return;
            }

            {
                const int bs = currentDevice->getCurrentBufferSizeSamples();
                const double srNow = currentDevice->getCurrentSampleRate();
                const double ms = (srNow > 0.0) ? (1000.0 * (double) bs / srNow) : 0.0;
                const double hwLatencyMs = (srNow > 0.0)
                                              ? (1000.0 * currentDevice->getOutputLatencyInSamples() / srNow)
                                              : 0.0;
                qDebug() << "Audio device configured:" << bs << "samples (~" << ms
                        << "ms) @" << srNow << "Hz, hw latency" << hwLatencyMs << "ms";
            }
        }

        const int deviceLatencySamples = currentDevice->getOutputLatencyInSamples() + currentDevice->getInputLatencyInSamples();
        const int actualBuffer = currentDevice->getCurrentBufferSizeSamples();
        const double actualSampleRate = currentDevice->getCurrentSampleRate();
        const int keylockIndex = std::clamp(QSettings(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat)
                                                .value("Audio/KeylockQuality", 1)
                                                .toInt(),
                                            0,
                                            2);
        const auto resolvedKeylock = static_cast<DJAudioPlayer::KeylockQuality>(keylockIndex);

        if (playerA) [[likely]] {
            playerA->prepareToPlay(actualBuffer, actualSampleRate);
            playerA->setHardwareLatencySamples(deviceLatencySamples > 0 ? deviceLatencySamples : actualBuffer);
            playerA->setKeylockQuality(resolvedKeylock);
        }
        if (playerB) [[likely]] {
            playerB->prepareToPlay(actualBuffer, actualSampleRate);
            playerB->setHardwareLatencySamples(deviceLatencySamples > 0 ? deviceLatencySamples : actualBuffer);
            playerB->setKeylockQuality(resolvedKeylock);
        }

    stereoCallback = std::make_unique<StereoAudioCallback>(playerA.get(), playerB.get());
        deviceManager.addAudioCallback(stereoCallback.get());
        deviceManager.addAudioCallback(&masterLevelMonitor);

        if (menuBar) {
            const double latencyMs = (actualSampleRate > 0.0)
                                         ? ((actualBuffer * 1000.0) / actualSampleRate)
                                         : 0.0;
            menuBar->updateAudioLatency(latencyMs, actualSampleRate, actualBuffer);
        }

        if (crossfader) [[likely]] {
            onCrossfader(crossfader->value());
        }

    } catch (...) {}
}

QtMainWindow::~QtMainWindow()
{
    qDebug() << "[DESTRUCTOR] MainWindow destructor called";

    if (auto* app = qApp) {
        app->removeEventFilter(this);
    }
    
    if (cursorOverridden) {
        QApplication::restoreOverrideCursor();
        cursorOverridden = false;
        currentCursorShape = Qt::ArrowCursor;
    }
    
    if (!cleanupCompleted) {
        qDebug() << "[DESTRUCTOR] Cleanup not done yet, calling performCleanup()";
        performCleanup();
    }
    
    // CRITICAL: Manually destroy objects in safe order to prevent destructor crashes
    // C++ destroys members in REVERSE declaration order, which can cause issues
    qDebug() << "[DESTRUCTOR] Manually destroying critical objects in safe order...";
    
    // 1. Destroy callback FIRST (no more audio processing)
    stereoCallback.reset();
    
    // 2. Destroy scratch engines (may reference players)
    scratchEngineA.reset();
    scratchEngineB.reset();
    
    // 3. Destroy players
    playerA.reset();
    playerB.reset();
    
    // 4. Destroy BPM analyzer
    bpmAnalyzer.reset();
    
    // 5. CRITICAL: Reset shared format manager to prevent static destruction issues
    if (sharedFormatManager && sharedFormatManager.use_count() == 1) {
        qDebug() << "[DESTRUCTOR] Resetting sharedFormatManager (last reference)";
        sharedFormatManager.reset();
    }
    
    qDebug() << "[DESTRUCTOR] MainWindow destructor completed";
}

void QtMainWindow::performCleanup()
{
    if (cleanupCompleted) [[unlikely]] return;
    
    try {
        // CRITICAL STEP 1: Set shutdown flag IMMEDIATELY to stop all audio processing
        qDebug() << "[CLEANUP] Step 1: Setting shutdown flag...";
        if (stereoCallback) {
            stereoCallback->detachPlayers();
        }
        
        // CRITICAL STEP 2: Wait to ensure any in-flight callbacks complete
        qDebug() << "[CLEANUP] Step 2: Waiting 100ms for callbacks to finish...";
        QThread::msleep(100); // Increased from 30ms
        
        // Stop periodic UI updates early to avoid timers firing during teardown
        qDebug() << "[CLEANUP] Step 3: Stopping timers...";
        if (positionUpdateTimer) {
            positionUpdateTimer->stop();
            positionUpdateTimer->disconnect();
        }
        
        if (waveformFillInTimer) {
            waveformFillInTimer->stop();
            waveformFillInTimer->disconnect();
        }

        // Proactively disconnect waveform region requests to avoid late queued invokes
        if (connWaveformRegionA) {
            disconnect(connWaveformRegionA);
        }
        if (connWaveformRegionB) {
            disconnect(connWaveformRegionB);
        }

        // Stop players (audio callback already disabled by shutdown flag)
        qDebug() << "[CLEANUP] Step 4: Stopping and unloading players...";
        if (playerA) {
            playerA->stop();
            playerA->unload();
        }
        if (playerB) {
            playerB->stop();
            playerB->unload();
        }

        if (deckA) {
            deckA->detachPlayer();
        }
        if (deckB) {
            deckB->detachPlayer();
        }
        
        // Wait again before touching audio system
        qDebug() << "[CLEANUP] Step 5: Waiting 100ms before audio system cleanup...";
        QThread::msleep(100);
        
        // Now safe to remove audio callbacks (no processing happening anymore)
        qDebug() << "[CLEANUP] Step 6: Removing audio callbacks...";
        if (stereoCallback) {
            deviceManager.removeAudioCallback(stereoCallback.get());
        }
        deviceManager.removeAudioCallback(&masterLevelMonitor);
        
        // Close audio device
        qDebug() << "[CLEANUP] Step 7: Closing audio device...";
        deviceManager.closeAudioDevice();
        
        // Final wait to ensure device is fully closed
        qDebug() << "[CLEANUP] Step 8: Waiting 50ms for device close...";
        QThread::msleep(50);
        
        // Wait for all background tasks to complete
        qDebug() << "[CLEANUP] Step 9: Waiting for thread pools...";
        if (bpmThreadPool) {
            bpmThreadPool->waitForDone(2000); // Increased timeout
        }
        if (waveformThreadPool) {
            waveformThreadPool->waitForDone(2000); // Increased timeout
        }
        
        // Now safe to destroy players (RubberBand already cleaned up by unload())
        qDebug() << "[CLEANUP] Step 10: Destroying player objects...";
        playerA.reset();
        playerB.reset();
        bpmAnalyzer.reset();
        
        // CRITICAL: Destroy stereoCallback AFTER players to prevent destructor issues
        qDebug() << "[CLEANUP] Step 11: Destroying stereoCallback...";
        stereoCallback.reset();
        
        if (sharedFormatManager && sharedFormatManager.use_count() == 1) {
            sharedFormatManager.reset();
        }
        
        qDebug() << "[CLEANUP] COMPLETED SUCCESSFULLY";
        cleanupCompleted = true;
        
    } catch (const std::exception& e) {
        qWarning() << "Exception during cleanup:" << e.what();
        cleanupCompleted = true;
    } catch (...) {
        qWarning() << "Unknown exception during cleanup";
        cleanupCompleted = true;
    }
}

void QtMainWindow::closeEvent(QCloseEvent* event)
{
    if (shutdownInitiated)
    {
        event->accept();
        return;
    }

    QMessageBox confirmBox(this);
    confirmBox.setIcon(QMessageBox::Question);
    confirmBox.setWindowTitle(tr("BetaPulseX beenden?"));
    confirmBox.setText(tr("Möchtest du BetaPulseX wirklich schließen?\nAlle laufenden Analysen werden gestoppt und die Datenbank wird sauber getrennt."));
    confirmBox.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    confirmBox.setDefaultButton(QMessageBox::Ok);

    if (confirmBox.exec() != QMessageBox::Ok)
    {
        event->ignore();
        return;
    }

    shutdownInitiated = true;

    QProgressDialog shutdownProgress(tr("Programm wird beendet..."), QString(), 0, 0, this);
    shutdownProgress.setWindowTitle(tr("Beenden"));
    shutdownProgress.setCancelButton(nullptr);
    shutdownProgress.setWindowModality(Qt::ApplicationModal);
    shutdownProgress.setAutoClose(false);
    shutdownProgress.setAutoReset(false);
    shutdownProgress.setMinimumDuration(0);
    shutdownProgress.setLabelText(tr("Deck-Einstellungen werden gespeichert..."));
    shutdownProgress.show();
    QApplication::processEvents();

    try {
        DeckSettings::instance().setVisualTrim(0, userVisualTrimA);
        DeckSettings::instance().setVisualTrim(1, userVisualTrimB);
        DeckSettings::instance().saveSettings();
        QSettings("DJDavid", "David").setValue("renderLatency/global", userRenderLatencySec);
    } catch (...) {}

    shutdownProgress.setLabelText(tr("Decks werden deaktiviert..."));
    QApplication::processEvents();

    if (deckA) deckA->getControlsWidget()->setEnabled(false);
    if (deckB) deckB->getControlsWidget()->setEnabled(false);

    shutdownProgress.setLabelText(tr("Audio-Engine wird heruntergefahren..."));
    QApplication::processEvents();

    performCleanup();

    shutdownProgress.setLabelText(tr("Aufräumen abgeschlossen"));
    QApplication::processEvents();
    shutdownProgress.close();

    event->accept();
    
    // CRITICAL: Delay quit to ensure window is fully destroyed first
    QTimer::singleShot(100, []() {
        qDebug() << "[APP] Quitting application...";
        QApplication::quit();
    });
}

void QtMainWindow::onCrossfader(int v) {
    if (stereoCallback) [[likely]] {
        // Map slider range (-100 .. 100) to audio crossfader range (-1.0 .. 1.0)
        stereoCallback->setCrossfader(static_cast<float>(v) / 100.0f);
    }
}

void QtMainWindow::setCrossfaderPosition(float normalizedValue) {
    if (crossfader) [[likely]] {
        // normalizedValue: 0.0 = full Deck A, 0.5 = center, 1.0 = full Deck B
        int sliderValue = static_cast<int>((std::clamp(normalizedValue, 0.0f, 1.0f) * 200.0f) - 100.0f);
        crossfader->setValue(sliderValue);
    }
}

void QtMainWindow::setDeckAPlayPause(bool shouldPlay) {
    if (deckA) [[likely]] { deckA->onPlayPause(); }
}

void QtMainWindow::setDeckBPlayPause(bool shouldPlay) {
    if (deckB) [[likely]] { deckB->onPlayPause(); }
}

void QtMainWindow::setDeckATempo(float normalizedValue) {
    if (playerA && deckA) [[likely]] {
        const double minTempo = deckA->getMinTempoFactor();
        const double maxTempo = deckA->getMaxTempoFactor();
        const double pitchValue = (normalizedValue <= 0.5f) 
            ? minTempo + (normalizedValue * 2.0) * (1.0 - minTempo)
            : 1.0 + ((normalizedValue - 0.5) * 2.0) * (maxTempo - 1.0);
        deckA->setTempoFactor(std::clamp(pitchValue, minTempo, maxTempo));
    }
}

void QtMainWindow::setDeckBTempo(float normalizedValue) {
    if (playerB && deckB) [[likely]] {
        const double minTempo = deckB->getMinTempoFactor();
        const double maxTempo = deckB->getMaxTempoFactor();
        const double pitchValue = (normalizedValue <= 0.5f) 
            ? minTempo + (normalizedValue * 2.0) * (1.0 - minTempo)
            : 1.0 + ((normalizedValue - 0.5) * 2.0) * (maxTempo - 1.0);
        deckB->setTempoFactor(std::clamp(pitchValue, minTempo, maxTempo));
    }
}

void QtMainWindow::setDeckAVolume(float normalizedValue) {
    if (stereoCallback) [[likely]] {
        stereoCallback->setVolumeA(std::clamp(normalizedValue, 0.0f, 1.0f));
    }
}

void QtMainWindow::setDeckBVolume(float normalizedValue) {
    if (stereoCallback) [[likely]] {
        stereoCallback->setVolumeB(std::clamp(normalizedValue, 0.0f, 1.0f));
    }
}

void QtMainWindow::applyAudioSettings(const QString& masterDeviceType,
                                      const QString& masterDeviceName,
                                      int masterChannelStart,
                                      int masterChannelCount,
                                      const QString& cueDeviceType,
                                      const QString& cueDeviceName,
                                      int cueChannelStart,
                                      int cueChannelCount,
                                      int bufferSize,
                                      int sampleRate,
                                      bool exclusiveMode) {
    Q_UNUSED(cueDeviceType);
    Q_UNUSED(cueDeviceName);
    Q_UNUSED(cueChannelStart);
    Q_UNUSED(cueChannelCount);
    Q_UNUSED(exclusiveMode);

    qDebug() << "QtMainWindow::applyAudioSettings - MasterDevice:" << masterDeviceName
             << "Type:" << masterDeviceType
             << "Channels start:" << masterChannelStart << "count:" << masterChannelCount
             << "BufferSize:" << bufferSize
             << "SampleRate:" << sampleRate;

    auto* currentDevice = deviceManager.getCurrentAudioDevice();
    if (!currentDevice) {
        qWarning() << "No audio device available for settings change";
        QMessageBox::warning(this,
                             "Audio Settings Error",
                             "No audio device available. Please check your audio configuration.");
        return;
    }

    juce::AudioDeviceManager::AudioDeviceSetup currentSetup;
    deviceManager.getAudioDeviceSetup(currentSetup);
    const juce::String currentType = deviceManager.getCurrentAudioDeviceType();

    const QString currentTypeQt = QString::fromStdString(currentType.toStdString());
    const QString currentDeviceQt = QString::fromStdString(currentSetup.outputDeviceName.toStdString());

    QString requestedTypeQt = masterDeviceType.isEmpty() ? currentTypeQt : masterDeviceType;
    QString requestedDeviceQt = masterDeviceName.isEmpty() ? currentDeviceQt : masterDeviceName;

    if (requestedDeviceQt.isEmpty()) {
        requestedDeviceQt = currentDeviceQt;
    }
    if (requestedTypeQt.isEmpty()) {
        requestedTypeQt = currentTypeQt;
    }

    masterChannelStart = std::max(0, masterChannelStart);
    masterChannelCount = std::clamp(masterChannelCount, 1, 16);

    juce::BigInteger requestedChannels;
    for (int i = 0; i < masterChannelCount; ++i) {
        requestedChannels.setBit(masterChannelStart + i);
    }
    if (requestedChannels.isZero()) {
        requestedChannels.setBit(0);
        requestedChannels.setBit(1);
        masterChannelStart = 0;
        masterChannelCount = 2;
    }

    if (sampleRate <= 0) {
        sampleRate = static_cast<int>(std::round(currentDevice->getCurrentSampleRate()));
    }
    if (bufferSize <= 0) {
        bufferSize = currentDevice->getCurrentBufferSizeSamples();
    }

    const bool bufferMatches = currentDevice->getCurrentBufferSizeSamples() == bufferSize;
    const bool rateMatches = std::abs(currentDevice->getCurrentSampleRate() - static_cast<double>(sampleRate)) < 1.0;
    const bool typeMatches = requestedTypeQt == currentTypeQt;
    const bool deviceMatches = requestedDeviceQt == currentDeviceQt;
    const bool channelMatches = currentSetup.outputChannels == requestedChannels;

    if (bufferMatches && rateMatches && typeMatches && deviceMatches && channelMatches) {
        qDebug() << "Audio settings already active - skipping reconfiguration";
        if (menuBar) {
            const double latencyMs = (sampleRate > 0) ? ((bufferSize * 1000.0) / sampleRate) : 0.0;
            menuBar->updateAudioLatency(latencyMs, sampleRate, bufferSize);
        }
        return;
    }

    const double latencyMsEstimate = (sampleRate > 0) ? ((bufferSize * 1000.0) / sampleRate) : 0.0;
    const QString deviceCaption = QString("%1 (%2)").arg(requestedDeviceQt).arg(requestedTypeQt);
    const QString channelCaption = QString("%1-%2")
                                       .arg(masterChannelStart + 1)
                                       .arg(masterChannelStart + masterChannelCount);

    const QString prompt = QString("Changing audio settings will temporarily stop playback.\n\n"
                                   "New settings:\n"
                                   "Device: %1\n"
                                   "Channels: %2\n"
                                   "Buffer Size: %3 samples (~%4 ms)\n"
                                   "Sample Rate: %5 Hz\n\n"
                                   "Continue?")
                               .arg(deviceCaption)
                               .arg(channelCaption)
                               .arg(bufferSize)
                               .arg(QString::number(latencyMsEstimate, 'f', 2))
                               .arg(sampleRate);

    if (QMessageBox::question(this, "Apply Audio Settings", prompt, QMessageBox::Yes | QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    const bool deckAWasPlaying = playerA && playerA->isPlaying();
    const bool deckBWasPlaying = playerB && playerB->isPlaying();
    const double deckAPosition = playerA ? playerA->getCurrentPositionSeconds() : 0.0;
    const double deckBPosition = playerB ? playerB->getCurrentPositionSeconds() : 0.0;

    qDebug() << "Stored state - DeckA playing:" << deckAWasPlaying << "pos:" << deckAPosition;
    qDebug() << "Stored state - DeckB playing:" << deckBWasPlaying << "pos:" << deckBPosition;

    if (stereoCallback) {
        stereoCallback->setShuttingDown(true);
        deviceManager.removeAudioCallback(stereoCallback.get());
        qDebug() << "Removed audio callback";
    }

    QThread::msleep(50);

    if (playerA) {
        playerA->stop();
        playerA->releaseResources();
    }
    if (playerB) {
        playerB->stop();
        playerB->releaseResources();
    }

    qDebug() << "Players stopped and resources released";

    deviceManager.closeAudioDevice();
    QThread::msleep(100);
    qDebug() << "Audio device closed";

    const juce::String requestedType = juce::String(requestedTypeQt.toStdString());
    const juce::String requestedDevice = juce::String(requestedDeviceQt.toStdString());

    if (!typeMatches) {
        deviceManager.setCurrentAudioDeviceType(requestedType, true);
        currentDevice = deviceManager.getCurrentAudioDevice();
        if (!currentDevice) {
            QMessageBox::critical(this,
                                  "Audio Settings Error",
                                  QString("Failed to switch to audio device type '%1'.").arg(requestedTypeQt));
            if (stereoCallback) {
                deviceManager.addAudioCallback(stereoCallback.get());
                stereoCallback->setShuttingDown(false);
            }
            return;
        }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    setup.outputDeviceName = requestedDevice;
    setup.useDefaultOutputChannels = false;
    setup.useDefaultInputChannels = false;
    setup.outputChannels = requestedChannels;
    setup.inputChannels.clear();
    setup.bufferSize = bufferSize;
    setup.sampleRate = sampleRate;

    qDebug() << "Applying new audio setup for device" << requestedDeviceQt;

    const juce::String error = deviceManager.setAudioDeviceSetup(setup, true);

    if (error.isNotEmpty()) {
        const QString errorMsg = QString::fromStdString(error.toStdString());
        qCritical() << "Failed to apply audio settings:" << errorMsg;

        QMessageBox::critical(this,
                              "Audio Settings Error",
                              QString("Failed to apply audio settings:\n%1\n\n"
                                      "Attempting to restore previous configuration...")
                                  .arg(errorMsg));

        deviceManager.initialise(0, 2, nullptr, true);

        if (stereoCallback) {
            deviceManager.addAudioCallback(stereoCallback.get());
            stereoCallback->setShuttingDown(false);
        }

        if (auto* recoveryDevice = deviceManager.getCurrentAudioDevice()) {
            const int recBufferSize = recoveryDevice->getCurrentBufferSizeSamples();
            const double recSampleRate = recoveryDevice->getCurrentSampleRate();
            if (playerA) playerA->prepareToPlay(recBufferSize, recSampleRate);
            if (playerB) playerB->prepareToPlay(recBufferSize, recSampleRate);
            qDebug() << "Recovery successful - using buffer size:" << recBufferSize;
        }

        return;
    }

    qDebug() << "Audio device reconfigured successfully";

    auto* newDevice = deviceManager.getCurrentAudioDevice();
    if (!newDevice) {
        qCritical() << "Failed to get audio device after reconfiguration!";
        QMessageBox::critical(this,
                              "Audio Settings Error",
                              "Failed to reopen audio device after configuration change.\n"
                              "Please restart the application.");
        return;
    }

    const int actualBufferSize = newDevice->getCurrentBufferSizeSamples();
    const double actualSampleRate = newDevice->getCurrentSampleRate();
    const QString activeDeviceName = QString::fromStdString(newDevice->getName().toStdString());

    qDebug() << "New audio device settings - Device:" << activeDeviceName
             << "BufferSize:" << actualBufferSize
             << "SampleRate:" << actualSampleRate;

    if (playerA) {
        playerA->prepareToPlay(actualBufferSize, actualSampleRate);
        qDebug() << "Player A prepared with new settings (length:" << playerA->getLengthInSeconds() << "s)";
    }

    if (playerB) {
        playerB->prepareToPlay(actualBufferSize, actualSampleRate);
        qDebug() << "Player B prepared with new settings (length:" << playerB->getLengthInSeconds() << "s)";
    }

    if (stereoCallback) {
        deviceManager.addAudioCallback(stereoCallback.get());
        stereoCallback->setShuttingDown(false);

        const int hwLatency = newDevice->getOutputLatencyInSamples();
        if (playerA) playerA->setHardwareLatencySamples(hwLatency);
        if (playerB) playerB->setHardwareLatencySamples(hwLatency);

        qDebug() << "Audio callback reconnected, hw latency:" << hwLatency << "samples";
    }

    if (menuBar) {
        const double latencyMs = (actualSampleRate > 0.0)
                                     ? ((actualBufferSize * 1000.0) / actualSampleRate)
                                     : 0.0;
        menuBar->updateAudioLatency(latencyMs, actualSampleRate, actualBufferSize);
        qDebug() << "MenuBar updated with new audio settings";
    }

    QThread::msleep(150);

    if (playerA && playerA->getLengthInSeconds() > 0.0 && deckAPosition > 0.0) {
        const double relPos = std::clamp(deckAPosition / playerA->getLengthInSeconds(), 0.0, 1.0);
        playerA->setPositionRelative(relPos);
        qDebug() << "Deck A position restored to" << relPos << "(" << deckAPosition << "seconds)";
    }

    if (playerB && playerB->getLengthInSeconds() > 0.0 && deckBPosition > 0.0) {
        const double relPos = std::clamp(deckBPosition / playerB->getLengthInSeconds(), 0.0, 1.0);
        playerB->setPositionRelative(relPos);
        qDebug() << "Deck B position restored to" << relPos << "(" << deckBPosition << "seconds)";
    }

    if (deckAWasPlaying && playerA && playerA->getLengthInSeconds() > 0.0) {
        QTimer::singleShot(500, [this]() {
            if (playerA && playerA->getLengthInSeconds() > 0.0) {
                playerA->start();
                qDebug() << "Deck A playback resumed after audio settings change";
            }
        });
    }

    if (deckBWasPlaying && playerB && playerB->getLengthInSeconds() > 0.0) {
        QTimer::singleShot(500, [this]() {
            if (playerB && playerB->getLengthInSeconds() > 0.0) {
                playerB->start();
                qDebug() << "Deck B playback resumed after audio settings change";
            }
        });
    }

    QMessageBox::information(this,
                             "Audio Settings Applied",
                             QString("Audio settings changed successfully:\n\n"
                                     "Device: %1\n"
                                     "Channels: %2\n"
                                     "Buffer Size: %3 samples (~%4 ms)\n"
                                     "Sample Rate: %5 Hz\n\n"
                                     "Playback state has been restored.")
                                 .arg(activeDeviceName)
                                 .arg(channelCaption)
                                 .arg(actualBufferSize)
                                 .arg(QString::number((actualBufferSize * 1000.0) / actualSampleRate, 'f', 2))
                                 .arg(actualSampleRate));
}

QVector<QtMainWindow::AudioOutputDeviceInfo> QtMainWindow::getAvailableOutputDevices() {
    QVector<AudioOutputDeviceInfo> devices;
    auto& types = deviceManager.getAvailableDeviceTypes();
    for (int i = 0; i < types.size(); ++i) {
        if (auto* type = types[i]) {
            type->scanForDevices();
            const juce::StringArray names = type->getDeviceNames(false);
            for (int n = 0; n < names.size(); ++n) {
                AudioOutputDeviceInfo info;
                info.typeName = QString::fromStdString(type->getTypeName().toStdString());
                info.deviceName = QString::fromStdString(names[n].toStdString());

                std::unique_ptr<juce::AudioIODevice> device(type->createDevice(names[n], {}));
                if (device) {
                    info.description = QString::fromStdString(device->getName().toStdString());
                    const juce::StringArray channelNames = device->getOutputChannelNames();
                    for (int ch = 0; ch < channelNames.size(); ++ch) {
                        info.outputChannelNames.append(QString::fromStdString(channelNames[ch].toStdString()));
                    }
                } else {
                    info.description = info.deviceName;
                }

                devices.append(info);
            }
        }
    }

    return devices;
}

QtMainWindow::AudioDeviceState QtMainWindow::getActiveAudioDeviceState() const {
    AudioDeviceState state;
    state.typeName = QString::fromStdString(deviceManager.getCurrentAudioDeviceType().toStdString());

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    state.deviceName = QString::fromStdString(setup.outputDeviceName.toStdString());

    int firstSetBit = setup.outputChannels.findNextSetBit(0);
    if (firstSetBit >= 0) {
        state.channelStart = firstSetBit;
        int count = 0;
        for (int bit = firstSetBit; bit >= 0; bit = setup.outputChannels.findNextSetBit(bit + 1)) {
            ++count;
        }
        state.channelCount = std::max(1, count);
    }

    return state;
}

void QtMainWindow::setKeylockQuality(DJAudioPlayer::KeylockQuality quality) {
    qDebug() << "QtMainWindow::setKeylockQuality - Quality:" << static_cast<int>(quality);
    
    if (playerA) {
        playerA->setKeylockQuality(quality);
    }
    
    if (playerB) {
        playerB->setKeylockQuality(quality);
    }
    
    const char* qualityNames[] = {"Fast", "Balanced", "High Quality"};
    qDebug() << "Keylock quality set to" << qualityNames[static_cast<int>(quality)] << "for both decks";
}

void QtMainWindow::onLeftHighChanged(int v) {
    if (playerA) [[likely]] { playerA->setHighGain(v * 0.01); }
}

void QtMainWindow::onLeftMidChanged(int v) {
    if (playerA) [[likely]] { playerA->setMidGain(v * 0.01); }
}

void QtMainWindow::onLeftLowChanged(int v) {
    if (playerA) [[likely]] { playerA->setLowGain(v * 0.01); }
}

void QtMainWindow::onLeftFilterChanged(int v) {
    if (playerA) [[likely]] { playerA->setFilterCutoff(v * 0.01); }
}

void QtMainWindow::onRightHighChanged(int v) {
    if (playerB) [[likely]] { playerB->setHighGain(v * 0.01); }
}

void QtMainWindow::onRightMidChanged(int v) {
    if (playerB) [[likely]] { playerB->setMidGain(v * 0.01); }
}

void QtMainWindow::onRightLowChanged(int v) {
    if (playerB) [[likely]] { playerB->setLowGain(v * 0.01); }
}

void QtMainWindow::onRightFilterChanged(int v) {
    if (playerB) [[likely]] { playerB->setFilterCutoff(v * 0.01); }
}

void QtMainWindow::onLeftVolumeChanged(int v) {
    if (stereoCallback) [[likely]] {
        stereoCallback->setVolumeA(juce::jlimit(0.0f, 1.0f, v * 0.01f));
    }
}

void QtMainWindow::onRightVolumeChanged(int v) {
    if (stereoCallback) [[likely]] {
        stereoCallback->setVolumeB(juce::jlimit(0.0f, 1.0f, v * 0.01f));
    }
}

void QtMainWindow::keyPressEvent(QKeyEvent* event) {
    if (QWidget* focusWidget = QApplication::focusWidget()) [[unlikely]] {
        if (qobject_cast<QLineEdit*>(focusWidget) || qobject_cast<QTextEdit*>(focusWidget) || qobject_cast<QPlainTextEdit*>(focusWidget)) {
            QWidget::keyPressEvent(event);
            return;
        }
    }

    switch (event->key()) {
        case Qt::Key_F5:
            userVisualTrimA = std::clamp(userVisualTrimA - 0.001, -0.05, 0.05);
            updateOverviewLabel(true);
            QSettings("DJDavid", "David").setValue("visualTrim/deckA", userVisualTrimA);
            event->accept();
            break;
        case Qt::Key_F6:
            userVisualTrimA = std::clamp(userVisualTrimA + 0.001, -0.05, 0.05);
            updateOverviewLabel(true);
            QSettings("DJDavid", "David").setValue("visualTrim/deckA", userVisualTrimA);
            event->accept();
            break;
        case Qt::Key_F7:
            userVisualTrimB = std::clamp(userVisualTrimB - 0.001, -0.05, 0.05);
            updateOverviewLabel(false);
            QSettings("DJDavid", "David").setValue("visualTrim/deckB", userVisualTrimB);
            event->accept();
            break;
        case Qt::Key_F8:
            userVisualTrimB = std::clamp(userVisualTrimB + 0.001, -0.05, 0.05);
            updateOverviewLabel(false);
            QSettings("DJDavid", "David").setValue("visualTrim/deckB", userVisualTrimB);
            event->accept();
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            if (overviewTopA) [[likely]] overviewTopA->increaseBeatGridZoom();
            if (overviewTopB) [[likely]] overviewTopB->increaseBeatGridZoom();
            event->accept();
            break;
        case Qt::Key_Minus:
        case Qt::Key_Underscore:
            if (overviewTopA) [[likely]] overviewTopA->decreaseBeatGridZoom();
            if (overviewTopB) [[likely]] overviewTopB->decreaseBeatGridZoom();
            event->accept();
            break;
        case Qt::Key_0:
            if (overviewTopA) [[likely]] overviewTopA->resetBeatGridZoom();
            if (overviewTopB) [[likely]] overviewTopB->resetBeatGridZoom();
            event->accept();
            break;
        case Qt::Key_F9: {
            // Decrease global render latency compensation by 5 ms
            userRenderLatencySec = std::clamp(userRenderLatencySec - 0.005, -0.25, 0.25);
            if (overviewTopA) overviewTopA->setOutputLatencyComp(userRenderLatencySec);
            if (overviewTopB) overviewTopB->setOutputLatencyComp(userRenderLatencySec);
            QSettings("DJDavid", "David").setValue("renderLatency/global", userRenderLatencySec);
            event->accept();
            break;
        }
        case Qt::Key_F10: {
            // Increase global render latency compensation by 5 ms
            userRenderLatencySec = std::clamp(userRenderLatencySec + 0.005, -0.25, 0.25);
            if (overviewTopA) overviewTopA->setOutputLatencyComp(userRenderLatencySec);
            if (overviewTopB) overviewTopB->setOutputLatencyComp(userRenderLatencySec);
            QSettings("DJDavid", "David").setValue("renderLatency/global", userRenderLatencySec);
            event->accept();
            break;
        }
        default:
            QWidget::keyPressEvent(event);
            break;
    }
}




// Event filter for double-click reset functionality and frameless resizing/dragging

void QtMainWindow::handleWaveformRegionRequestDeckA(double startSec, double endSec)
{
    handleWaveformRegionRequest(true, startSec, endSec);
}

void QtMainWindow::handleWaveformRegionRequestDeckB(double startSec, double endSec)
{
    handleWaveformRegionRequest(false, startSec, endSec);
}

void QtMainWindow::handleWaveformRegionRequest(bool deckIsA, double startSec, double endSec)
{
    WaveformStreamSession& session = deckIsA ? streamSessionA : streamSessionB;
    if (!session.valid || session.binsPerSecond <= 0.0) {
        return;
    }

    QtDeckWidget* deck = deckIsA ? deckA : deckB;
    if (!deck || deck->getCurrentFilePath() != session.filePath) {
        return;
    }

    if (endSec <= startSec) {
        endSec = startSec + 0.01;
    }

    const double clampedStartSec = std::max(0.0, startSec);
    const double clampedEndSec = std::max(clampedStartSec + 0.01, endSec);

    int startBin = static_cast<int>(std::floor((clampedStartSec - session.metadata.audioStartOffsetSec) * session.binsPerSecond));
    int endBin = static_cast<int>(std::ceil((clampedEndSec - session.metadata.audioStartOffsetSec) * session.binsPerSecond));

    startBin = std::max(0, startBin);
    endBin = std::min(session.totalBins, std::max(startBin + 1, endBin));
    if (startBin >= endBin) {
        return;
    }

    const int alignedStart = std::max(0, startBin - (startBin % session.chunkBinSize));
    const int requestCenterBin = (startBin + endBin) / 2;
    
    auto scheduleIfNeeded = [&](int chunkStart){
        if (chunkStart < 0 || chunkStart >= session.totalBins) return;
        if (session.pendingChunks.contains(chunkStart)) return;
        session.pendingChunks.insert(chunkStart);
        const int count = std::min(session.chunkBinSize, session.totalBins - chunkStart);
        scheduleWaveformChunk(deckIsA, chunkStart, count);
    };

    if (!session.hasCache) {
        const int microSize = 1024;
        const int microStart = std::clamp(requestCenterBin - microSize / 2, 0, std::max(0, session.totalBins - microSize));
        scheduleIfNeeded(microStart);
        scheduleIfNeeded(alignedStart);
        scheduleIfNeeded(alignedStart + session.chunkBinSize);
        scheduleIfNeeded(alignedStart + session.chunkBinSize * 2);
        return;
    }

    const bool outsideCache = (endBin <= session.cachedStartBin) || (startBin >= session.cachedEndBin);
    
    if (outsideCache) {
        const int microSize = 1024;
        const int microStart = std::clamp(requestCenterBin - microSize / 2, 0, std::max(0, session.totalBins - microSize));
        scheduleIfNeeded(microStart);
        scheduleIfNeeded(alignedStart);
        scheduleIfNeeded(alignedStart + session.chunkBinSize);
        scheduleIfNeeded(alignedStart + session.chunkBinSize * 2);
        scheduleIfNeeded(std::max(0, alignedStart - session.chunkBinSize));
        return;
    }
}

void QtMainWindow::scheduleWaveformChunk(bool deckIsA, int startBin, int binCount)
{
    if (!waveformThreadPool || binCount <= 0) {
        return;
    }

    const WaveformStreamSession& session = deckIsA ? streamSessionA : streamSessionB;
    if (!session.valid) {
        return;
    }

    QtDeckWidget* deck = deckIsA ? deckA : deckB;
    if (!deck || deck->getCurrentFilePath() != session.filePath) {
        return;
    }

    auto task = new WaveformStreamChunkTask(this,
                                            session.filePath,
                                            deckIsA,
                                            session.metadata,
                                            session.totalBins,
                                            startBin,
                                            binCount);
    waveformThreadPool->start(task);
}

void QtMainWindow::handleWaveformChunkResult(bool deckIsA,
                                             const QString& filePath,
                                             int startBin,
                                             std::shared_ptr<std::vector<float>> maxBins,
                                             std::shared_ptr<std::vector<float>> minBins,
                                             bool success,
                                             std::shared_ptr<std::vector<float>> lowBins,
                                             std::shared_ptr<std::vector<float>> midBins,
                                             std::shared_ptr<std::vector<float>> highBins)
{
    WaveformStreamSession& session = deckIsA ? streamSessionA : streamSessionB;
    session.pendingChunks.remove(startBin);

    if (!session.valid || session.filePath != filePath) {
        return;
    }

    QtDeckWidget* deck = deckIsA ? deckA : deckB;
    if (!deck || deck->getCurrentFilePath() != filePath) {
        return;
    }

    WaveformDisplay* wf = deckIsA ? overviewTopA : overviewTopB;
    if (!wf) {
        return;
    }

    if (!success || !maxBins || !minBins || maxBins->size() != minBins->size()) {
        wf->setAnalysisActive(false);
        wf->setAnalysisFailed(true);
        if (deckIsA) {
            analysisActiveA = false;
            analysisFailedA = true;
        } else {
            analysisActiveB = false;
            analysisFailedB = true;
        }
        updateOverviewLabel(deckIsA);
        return;
    }

    const bool wasEmpty = !session.hasCache;
    const int chunkSize = static_cast<int>(maxBins->size());

    if (lowBins && midBins && highBins &&
        lowBins->size() == maxBins->size() &&
        midBins->size() == maxBins->size() &&
        highBins->size() == maxBins->size()) {
        wf->appendStreamBins(startBin, *maxBins, *minBins, *lowBins, *midBins, *highBins, false);
    } else {
        wf->appendStreamBins(startBin, *maxBins, *minBins, false);
    }
    wf->setAnalysisFailed(false);

    const auto range = wf->getCachedBinRange();
    session.cachedStartBin = std::max(0, range.first);
    session.cachedEndBin = std::clamp(range.second, 0, session.totalBins);
    session.hasCache = session.cachedEndBin > session.cachedStartBin;

    const double coverage = session.hasCache
        ? static_cast<double>(session.cachedEndBin - session.cachedStartBin) /
              std::max(1, session.totalBins)
        : 0.0;

    const bool fullyCovered = session.hasCache && session.cachedStartBin <= 0 && session.cachedEndBin >= session.totalBins;
    wf->setAnalysisActive(!fullyCovered);
    wf->setAnalysisProgress(std::clamp(coverage, 0.0, 1.0));

    if (deckIsA) {
        analysisActiveA = !fullyCovered;
        analysisFailedA = false;
        analysisProgressA = std::max(analysisProgressA, coverage);
    } else {
        analysisActiveB = !fullyCovered;
        analysisFailedB = false;
        analysisProgressB = std::max(analysisProgressB, coverage);
    }

    if (wasEmpty) {
        reapplyStoredDeckMetadata(deckIsA);
    }

    updateOverviewLabel(deckIsA);
}

void QtMainWindow::updatePlaybackPositions() {
    static double lastPosA = -999.0, lastPosB = -999.0;
    const qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    
    if (playerA && overviewTopA && !overviewTopA->isScratching() && (currentTime - lastScratchEndA > 100)) [[likely]] {
        const double relativePos = playerA->getPositionRelative();
        if ((playerA->isPlaying() || relativePos < 0.0) && std::abs(relativePos - lastPosA) > ((relativePos < 0.0) ? 0.0002 : 0.008)) [[likely]] {
            lastPosA = relativePos;
            overviewTopA->setPlayhead(relativePos);
            if (deckA && deckA->getWaveform()) [[likely]] deckA->getWaveform()->setPlayhead(relativePos);
            if (beatIndicator) [[likely]] {
                constexpr double prerollSec = 8.0;
                beatIndicator->setTrackPositionDeckA((relativePos < 0.0) ? (relativePos * prerollSec) : (relativePos * std::max(1e-9, playerA->getLengthInSeconds())));
            }
        }
    } else if (overviewTopA && overviewTopA->isScratching()) [[unlikely]] {
        lastScratchEndA = currentTime;
    }
    
    if (playerB && overviewTopB && !overviewTopB->isScratching() && (currentTime - lastScratchEndB > 100)) [[likely]] {
        const double relativePos = playerB->getPositionRelative();
        if ((playerB->isPlaying() || relativePos < 0.0) && std::abs(relativePos - lastPosB) > ((relativePos < 0.0) ? 0.0002 : 0.008)) [[likely]] {
            lastPosB = relativePos;
            overviewTopB->setPlayhead(relativePos);
            if (deckB && deckB->getWaveform()) [[likely]] deckB->getWaveform()->setPlayhead(relativePos);
            if (beatIndicator) [[likely]] {
                constexpr double prerollSec = 8.0;
                beatIndicator->setTrackPositionDeckB((relativePos < 0.0) ? (relativePos * prerollSec) : (relativePos * std::max(1e-9, playerB->getLengthInSeconds())));
            }
        }
    } else if (overviewTopB && overviewTopB->isScratching()) [[unlikely]] {
        lastScratchEndB = currentTime;
    }
}

void QtMainWindow::continuousWaveformFillIn(bool deckIsA)
{
    WaveformStreamSession& session = deckIsA ? streamSessionA : streamSessionB;
    
    if (!session.valid || session.totalBins <= 0) return;
    
    const bool fullyLoaded = (session.cachedStartBin <= 0 && session.cachedEndBin >= session.totalBins);
    if (fullyLoaded) return;
    
    if (session.pendingChunks.size() >= 8) return;
    
    auto scheduleIfNeeded = [&](int chunkStart) {
        if (chunkStart < 0 || chunkStart >= session.totalBins) return false;
        if (session.pendingChunks.contains(chunkStart)) return false;
        
        session.pendingChunks.insert(chunkStart);
        const int count = std::min(session.chunkBinSize, session.totalBins - chunkStart);
        scheduleWaveformChunk(deckIsA, chunkStart, count);
        return true;
    };
    
    int scheduled = 0;
    const int maxSchedulePerCycle = 4;
    
    while (scheduled < maxSchedulePerCycle && session.cachedEndBin < session.totalBins) {
        const int nextStart = session.cachedEndBin;
        if (!scheduleIfNeeded(nextStart)) break;
        scheduled++;
    }
    
    while (scheduled < maxSchedulePerCycle && session.cachedStartBin > 0) {
        const int nextStart = std::max(0, session.cachedStartBin - session.chunkBinSize);
        if (!scheduleIfNeeded(nextStart)) break;
        scheduled++;
    }
}
