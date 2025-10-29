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

std::shared_ptr<juce::AudioFormatManager> QtMainWindow::sharedFormatManager = {};

QtMainWindow::QtMainWindow(QWidget* parent) : QWidget(parent)
{
    setWindowTitle("BetaPulseX - Professional DJ Software");
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_StyledBackground, true);
    
    // Remove window decorations and make frameless
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setStyleSheet("QtMainWindow { background-color: #141a1f; border: none; }");
    
    menuBar = new MenuBar(this);
    
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
        userRenderLatencySec = s.value("renderLatency/global", 0.03).toDouble();
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
    libraryManager = new LibraryManager(sharedFormatManager.get(), this);
    
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

    crossfader = new QSlider(Qt::Horizontal, this);
    crossfader->setRange(0, 100);
    crossfader->setValue(50);
    connect(crossfader, &QSlider::valueChanged, this, &QtMainWindow::onCrossfader);

    // Rekordbox-style layout with Serato-style overview waveforms at top
    // Top section: Two stacked overview waveforms (Serato style)
    auto overviewLayout = new QVBoxLayout;
    
    // Style the overview waveforms (increase height ~2x as requested)
    overviewTopA->setFixedHeight(70);
    overviewTopB->setFixedHeight(70);
    overviewTopA->setStyleSheet("border: 1px solid #333; background-color: #0a0a0a;");
    overviewTopB->setStyleSheet("border: 1px solid #333; background-color: #0a0a0a;");

    overviewLayout->setSpacing(0);
    overviewLayout->setContentsMargins(0, 0, 0, 0);
    overviewLayout->addWidget(overviewTopA);
    overviewLayout->addWidget(overviewTopB);

    updateOverviewLabel(true);
    updateOverviewLabel(false);
    
    // Main deck controls side by side (more compact spacing)
    auto decksLayout = new QHBoxLayout;
    decksLayout->setSpacing(6);
    decksLayout->setContentsMargins(0, 0, 0, 0);
    decksLayout->addWidget(deckA->getControlsWidget(), 2);
    
    // Mixer section in the center (more compact)
    auto mixerSection = new QVBoxLayout;
    mixerSection->setSpacing(2);
    mixerSection->setContentsMargins(4, 4, 4, 4);
    auto crossfaderLabel = new QLabel("CROSSFADER", this);
    crossfaderLabel->setAlignment(Qt::AlignCenter);
    crossfaderLabel->setStyleSheet("font-weight: bold; color: #fff; font-size: 9px;");
    crossfaderLabel->setFixedHeight(14);
    mixerSection->addWidget(crossfaderLabel);
    mixerSection->addWidget(crossfader);
    
    // Add EQ knobs: top to bottom High, Mid, Low, then Filter (smaller)
    auto eqLayout = new QHBoxLayout;
    eqLayout->setSpacing(4);
    auto leftEqLayout = new QVBoxLayout;
    leftEqLayout->setSpacing(2);
    auto rightEqLayout = new QVBoxLayout;
    rightEqLayout->setSpacing(2);

    leftHigh = new QDial(this);
    leftHigh->setRange(-100, 100);
    leftHigh->setNotchesVisible(true);
    leftHigh->setToolTip("Left High");
    leftMid = new QDial(this);
    leftMid->setRange(-100, 100);
    leftMid->setNotchesVisible(true);
    leftMid->setToolTip("Left Mid");
    leftLow = new QDial(this);
    leftLow->setRange(-100, 100);
    leftLow->setNotchesVisible(true);
    leftLow->setToolTip("Left Low");
    leftFilter = new QDial(this);
    leftFilter->setRange(-100, 100);
    leftFilter->setNotchesVisible(true);
    leftFilter->setToolTip("Left Filter");

    rightHigh = new QDial(this);
    rightHigh->setRange(-100, 100);
    rightHigh->setNotchesVisible(true);
    rightHigh->setToolTip("Right High");
    rightMid = new QDial(this);
    rightMid->setRange(-100, 100);
    rightMid->setNotchesVisible(true);
    rightMid->setToolTip("Right Mid");
    rightLow = new QDial(this);
    rightLow->setRange(-100, 100);
    rightLow->setNotchesVisible(true);
    rightLow->setToolTip("Right Low");
    rightFilter = new QDial(this);
    rightFilter->setRange(-100, 100);
    rightFilter->setNotchesVisible(true);
    rightFilter->setToolTip("Right Filter");

    // Set all knobs to start centered (0) and make them smaller
    leftHigh->setValue(0); leftMid->setValue(0); leftLow->setValue(0); leftFilter->setValue(0);
    rightHigh->setValue(0); rightMid->setValue(0); rightLow->setValue(0); rightFilter->setValue(0);
    
    // Make knobs smaller
    leftHigh->setFixedSize(32, 32); leftMid->setFixedSize(32, 32); 
    leftLow->setFixedSize(32, 32); leftFilter->setFixedSize(32, 32);
    rightHigh->setFixedSize(32, 32); rightMid->setFixedSize(32, 32);
    rightLow->setFixedSize(32, 32); rightFilter->setFixedSize(32, 32);

    // Arrange top-to-bottom: High, Mid, Low, Filter
    leftEqLayout->addWidget(leftHigh);
    leftEqLayout->addWidget(leftMid);
    leftEqLayout->addWidget(leftLow);
    leftEqLayout->addWidget(leftFilter);

    rightEqLayout->addWidget(rightHigh);
    rightEqLayout->addWidget(rightMid);
    rightEqLayout->addWidget(rightLow);
    rightEqLayout->addWidget(rightFilter);

    eqLayout->addLayout(leftEqLayout);
    eqLayout->addLayout(rightEqLayout);

    mixerSection->addLayout(eqLayout);
    
    // Add volume sliders below the filter knobs
    auto volumeLayout = new QHBoxLayout;
    volumeLayout->setSpacing(4);
    
    auto leftVolLayout = new QVBoxLayout;
    leftVolLayout->setSpacing(1);
    leftVolLayout->setAlignment(Qt::AlignCenter);
    auto leftVolLabel = new QLabel("Vol A", this);
    leftVolLabel->setAlignment(Qt::AlignCenter);
    leftVolLabel->setStyleSheet("color: #fff; font-size: 9px; font-weight: bold;");
    leftVolLabel->setFixedHeight(12);
    
    leftVolumeSlider = new QSlider(Qt::Vertical, this);
    leftVolumeSlider->setRange(0, 100);
    leftVolumeSlider->setValue(100);
    leftVolumeSlider->setFixedHeight(50);
    leftVolumeSlider->setFixedWidth(18);
    
    leftVolLayout->addWidget(leftVolLabel);
    leftVolLayout->addWidget(leftVolumeSlider);
    
    auto rightVolLayout = new QVBoxLayout;
    rightVolLayout->setSpacing(1);
    rightVolLayout->setAlignment(Qt::AlignCenter);
    auto rightVolLabel = new QLabel("Vol B", this);
    rightVolLabel->setAlignment(Qt::AlignCenter);
    rightVolLabel->setStyleSheet("color: #fff; font-size: 9px; font-weight: bold;");
    rightVolLabel->setFixedHeight(12);
    
    rightVolumeSlider = new QSlider(Qt::Vertical, this);
    rightVolumeSlider->setRange(0, 100);
    rightVolumeSlider->setValue(100);
    rightVolumeSlider->setFixedHeight(50);
    rightVolumeSlider->setFixedWidth(18);
    
    rightVolLayout->addWidget(rightVolLabel);
    rightVolLayout->addWidget(rightVolumeSlider);
    
    volumeLayout->addLayout(leftVolLayout);
    volumeLayout->addLayout(rightVolLayout);
    
    mixerSection->addLayout(volumeLayout);
    mixerSection->addStretch();
    
    auto mixerWidget = new QWidget(this);
    mixerWidget->setLayout(mixerSection);
    mixerWidget->setFixedWidth(120);
    mixerWidget->setStyleSheet("background-color: #2a2a2a; border: 1px solid #555; border-radius: 0px;");
    
    decksLayout->addWidget(mixerWidget, 1);
    decksLayout->addWidget(deckB->getControlsWidget(), 2);

    // Connect knobs to slots to control EQ and filter
    connect(leftHigh, &QDial::valueChanged, this, &QtMainWindow::onLeftHighChanged);
    connect(leftMid, &QDial::valueChanged, this, &QtMainWindow::onLeftMidChanged);
    connect(leftLow, &QDial::valueChanged, this, &QtMainWindow::onLeftLowChanged);
    connect(leftFilter, &QDial::valueChanged, this, &QtMainWindow::onLeftFilterChanged);

    connect(rightHigh, &QDial::valueChanged, this, &QtMainWindow::onRightHighChanged);
    connect(rightMid, &QDial::valueChanged, this, &QtMainWindow::onRightMidChanged);
    connect(rightLow, &QDial::valueChanged, this, &QtMainWindow::onRightLowChanged);
    connect(rightFilter, &QDial::valueChanged, this, &QtMainWindow::onRightFilterChanged);
    
    // Add double-click reset functionality for all mixer controls
    // EQ controls reset to 0 (neutral)
    leftHigh->installEventFilter(this);
    leftMid->installEventFilter(this);
    leftLow->installEventFilter(this);
    leftFilter->installEventFilter(this);
    rightHigh->installEventFilter(this);
    rightMid->installEventFilter(this);
    rightLow->installEventFilter(this);
    rightFilter->installEventFilter(this);
    
    // Volume sliders reset to 100 (full volume)
    leftVolumeSlider->installEventFilter(this);
    rightVolumeSlider->installEventFilter(this);
    
    // Crossfader resets to 50 (center)
    crossfader->installEventFilter(this);
    
    // Connect volume sliders
    connect(leftVolumeSlider, &QSlider::valueChanged, this, &QtMainWindow::onLeftVolumeChanged);
    connect(rightVolumeSlider, &QSlider::valueChanged, this, &QtMainWindow::onRightVolumeChanged);
    
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

        // Request an audio buffer size close to 5 ms to minimize audible glitches on seek/keylock
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup(setup);
            const double sr = (setup.sampleRate > 0.0) ? setup.sampleRate : currentDevice->getCurrentSampleRate();
            const int targetSamples = (sr > 0.0) ? std::max(32, (int) std::lround(sr * 0.005)) : 256; // ~5 ms

            int chosen = targetSamples;
#if JUCE_MODULE_AVAILABLE_juce_audio_devices
            // Prefer a supported device buffer size closest to our target
            const auto sizes = currentDevice->getAvailableBufferSizes();
            if (sizes.size() > 0) {
                int best = sizes[0];
                double bestDiff = std::abs(best - targetSamples);
                for (int i = 1; i < sizes.size(); ++i) {
                    const int v = sizes[i];
                    const double d = std::abs(v - targetSamples);
                    if (d < bestDiff) { best = v; bestDiff = d; }
                }
                chosen = best;
            }
#endif
            setup.bufferSize = chosen;
            if (setup.sampleRate <= 0.0) setup.sampleRate = (sr > 0.0 ? sr : 48000.0);
            deviceManager.setAudioDeviceSetup(setup, true);

            // Refresh device pointer after potential re-open
            currentDevice = deviceManager.getCurrentAudioDevice();
            if (!currentDevice) [[unlikely]] {
                return;
            }

            // Log the actual buffer size and sample rate the device settled on
            {
                const int bs = currentDevice->getCurrentBufferSizeSamples();
                const double srNow = currentDevice->getCurrentSampleRate();
                const double ms = (srNow > 0.0) ? (1000.0 * (double) bs / srNow) : 0.0;
                qDebug() << "Audio device configured:" << bs << "samples (~" << ms << "ms) @" << srNow << "Hz";
            }
        }

        if (playerA) [[likely]] {
            playerA->prepareToPlay(currentDevice->getCurrentBufferSizeSamples(), currentDevice->getCurrentSampleRate());
        }
        if (playerB) [[likely]] {
            playerB->prepareToPlay(currentDevice->getCurrentBufferSizeSamples(), currentDevice->getCurrentSampleRate());
        }

    stereoCallback = std::make_unique<StereoAudioCallback>(playerA.get(), playerB.get());
        deviceManager.addAudioCallback(stereoCallback.get());
        deviceManager.addAudioCallback(&masterLevelMonitor);

        if (crossfader) [[likely]] {
            onCrossfader(crossfader->value());
        }

    } catch (...) {}
}

QtMainWindow::~QtMainWindow()
{
    if (cursorOverridden) {
        QApplication::restoreOverrideCursor();
        cursorOverridden = false;
        currentCursorShape = Qt::ArrowCursor;
    }
    if (!cleanupCompleted) {
        performCleanup();
    }
}

void QtMainWindow::performCleanup()
{
    if (cleanupCompleted) [[unlikely]] return;
    
    try {
        // Stop periodic UI updates early to avoid timers firing during teardown
        if (positionUpdateTimer) {
            positionUpdateTimer->stop();
        }

        // Proactively disconnect waveform region requests to avoid late queued invokes
        if (connWaveformRegionA) {
            disconnect(connWaveformRegionA);
        }
        if (connWaveformRegionB) {
            disconnect(connWaveformRegionB);
        }

        if (playerA) playerA->stop();
        if (playerB) playerB->stop();
        
        if (stereoCallback) {
            deviceManager.removeAudioCallback(stereoCallback.get());
        }
        deviceManager.removeAudioCallback(&masterLevelMonitor);
        
        deviceManager.closeAudioDevice();
        
        if (bpmThreadPool) {
            bpmThreadPool->waitForDone(1000);
        }
        if (waveformThreadPool) {
            waveformThreadPool->waitForDone(1000);
        }
        
        playerA.reset();
        playerB.reset();
        bpmAnalyzer.reset();
        
        if (sharedFormatManager && sharedFormatManager.use_count() == 1) {
            sharedFormatManager.reset();
        }
        
        cleanupCompleted = true;
        
    } catch (...) {
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
    QApplication::quit();
}

void QtMainWindow::onCrossfader(int v) {
    if (stereoCallback) [[likely]] {
        stereoCallback->setCrossfader((static_cast<float>(v) - 50.0f) * 0.02f);
    }
}

void QtMainWindow::setCrossfaderPosition(float normalizedValue) {
    if (crossfader) [[likely]] {
        crossfader->setValue(std::clamp(static_cast<int>(normalizedValue * 100.0f), 0, 100));
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
    const int microSize = std::max(256, std::min(1024, session.chunkBinSize));
    int microStart = std::clamp(requestCenterBin - microSize / 2, 0, std::max(0, session.totalBins - microSize));

    // If we have no cache yet, start directly at the aligned request
    if (!session.hasCache) {
        // Schedule a tiny center chunk first for instant paint, then the aligned chunk
        if (!session.pendingChunks.contains(microStart)) {
            session.pendingChunks.insert(microStart);
            const int count = std::min(microSize, session.totalBins - microStart);
            scheduleWaveformChunk(deckIsA, microStart, count);
        }
        if (!session.pendingChunks.contains(alignedStart)) {
            session.pendingChunks.insert(alignedStart);
            const int count = std::min(session.chunkBinSize, session.totalBins - alignedStart);
            scheduleWaveformChunk(deckIsA, alignedStart, count);
        }
        return;
    }

    // Determine if requested region is outside or far from current cached window
    const bool outsideLeft = endBin <= session.cachedStartBin;
    const bool outsideRight = startBin >= session.cachedEndBin;
    const bool farFromCached = (outsideLeft || outsideRight) ||
        (std::min(std::abs(alignedStart - session.cachedStartBin), std::abs(alignedStart - session.cachedEndBin)) > session.chunkBinSize * 2);

    if (farFromCached) {
        // Jump directly to requested region: schedule that chunk and a neighbor forward for faster fill-in
        auto scheduleIfNeeded = [&](int chunkStart){
            if (chunkStart < 0 || chunkStart >= session.totalBins) return;
            if (!session.pendingChunks.contains(chunkStart)) {
                session.pendingChunks.insert(chunkStart);
                const int count = std::min(session.chunkBinSize, session.totalBins - chunkStart);
                scheduleWaveformChunk(deckIsA, chunkStart, count);
            }
        };
        // First, schedule a small micro-chunk centered on the request to get instant pixels
        if (!session.pendingChunks.contains(microStart)) {
            session.pendingChunks.insert(microStart);
            const int count = std::min(microSize, session.totalBins - microStart);
            scheduleWaveformChunk(deckIsA, microStart, count);
        }
        scheduleIfNeeded(alignedStart);
        scheduleIfNeeded(std::min(session.totalBins, alignedStart + session.chunkBinSize));
        return;
    }

    // Otherwise, extend cache progressively toward the requested region as before
    if (startBin < session.cachedStartBin) {
        const int nextStart = std::max(0, session.cachedStartBin - session.chunkBinSize);
        if (!session.pendingChunks.contains(nextStart) && session.cachedStartBin > 0) {
            session.pendingChunks.insert(nextStart);
            const int count = std::min(session.chunkBinSize, session.totalBins - nextStart);
            scheduleWaveformChunk(deckIsA, nextStart, count);
        }
        return;
    }

    if (endBin > session.cachedEndBin) {
        const int nextStart = session.cachedEndBin;
        if (nextStart < session.totalBins && !session.pendingChunks.contains(nextStart)) {
            session.pendingChunks.insert(nextStart);
            const int count = std::min(session.chunkBinSize, session.totalBins - nextStart);
            scheduleWaveformChunk(deckIsA, nextStart, count);
        }
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

