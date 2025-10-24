#include "MainWindow.h"
#include "DJAudioPlayer.h"
#include "BpmAnalyzer.h"
#include "WaveformDisplay.h"
#include "BeatIndicator.h"
#include "PreferencesDialog.h"
#include "ScratchEngine.h"
#include "StereoAudioCallback.h"
#include "MainWindowTasks.h"
#include <iostream>
#include <QApplication>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QVector>
#include <QProgressDialog>
#include <QMenu>
#include <QDebug>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <QCursor>
#include <limits>
#include <QHoverEvent>
#include <QWindow>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDir>
#include <QListWidgetItem>
#include <QStringList>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QThreadPool>
#include <QSettings>
#include <QMenuBar>
#include <QAction>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include "AppConfig.h"
#include "DeckSettings.h"

// Static members for shared format manager
std::shared_ptr<juce::AudioFormatManager> QtMainWindow::sharedFormatManager = {};
QtMainWindow::QtMainWindow(QWidget* parent) : QWidget(parent)
{
    setWindowTitle("BetaPulseX - Professional DJ Software");
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_StyledBackground, true);
    if (qApp)
        qApp->installEventFilter(this);
    
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

    connect(overviewTopA, &WaveformDisplay::pitchBendRequested, this, [this](double ratio) {
        if (playerA) {
            playerA->setPitchBendRatio(ratio);
        }
    });
    connect(overviewTopA, &WaveformDisplay::pitchBendEnded, this, [this]() {
        if (playerA) {
            playerA->setPitchBendRatio(1.0);
        }
    });
    connect(overviewTopB, &WaveformDisplay::pitchBendRequested, this, [this](double ratio) {
        if (playerB) {
            playerB->setPitchBendRatio(ratio);
        }
    });
    connect(overviewTopB, &WaveformDisplay::pitchBendEnded, this, [this]() {
        if (playerB) {
            playerB->setPitchBendRatio(1.0);
        }
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
        if (playerA) {
            playerA->setPitchBendRatio(1.0);
        }
        QString filePath = deckA->getCurrentFilePath();
        if (!filePath.isEmpty()) {
            bpmThreadPool->start(new TopWaveformDisplayTask(this, filePath, true));
            
            // Start BPM analysis only if stored metadata isn't usable
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
        if (playerB) {
            playerB->setPitchBendRatio(1.0);
        }
        QString filePath = deckB->getCurrentFilePath();
        if (!filePath.isEmpty()) {
            bpmThreadPool->start(new TopWaveformDisplayTask(this, filePath, false));

            // Start BPM analysis only if stored metadata isn't usable
            startDeckAnalysisIfNeeded(filePath, false);
        }
    });

    connect(deckB, &QtDeckWidget::fileLoaded, this, [this]() {
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
        if (auto* dev = deviceManager.getCurrentAudioDevice()) {
            const double sr = dev->getCurrentSampleRate();
            if (sr > 0.0) {
                const int buf = dev->getCurrentBufferSizeSamples();
                const int outLat = dev->getOutputLatencyInSamples();
                if (outLat > 0) deviceLatencySec = outLat / sr; else deviceLatencySec = (buf > 0 ? (1.5 * buf) / sr : 0.0);
            }
        }
        double pipelineLatencySec = playerB ? playerB->getPipelineLatencySeconds() : 0.0;
    double visualDelay = std::clamp(pipelineLatencySec + deviceLatencySec, 0.0, 0.25);
    constexpr double uiFudgeSec = 0.012; // ~12 ms safety (display/vsync)
    double totalDelay = visualDelay + uiFudgeSec + std::clamp(userVisualTrimB, -0.05, 0.05);
        if (playerB) {
            double displayRel = relative;
            if (relative >= 0.0) {
                double len = playerB->getLengthInSeconds();
                if (len > 1e-6) {
                    displayRel = relative - (totalDelay / len);
                }
                displayRel = std::clamp(displayRel, 0.0, 1.0);
            }
            overviewTopB->setPlayhead(displayRel);
            if (deckB && deckB->getWaveform()) deckB->getWaveform()->setPlayhead(displayRel);
        } else {
            overviewTopB->setPlayhead(relative);
            if (deckB && deckB->getWaveform()) deckB->getWaveform()->setPlayhead(relative);
        }
        // Update beat indicator with audible time in seconds (support preroll)
        if (playerB) {
            double curSec = playerB->getCurrentPositionSeconds();
            double audibleTimeSec = curSec - totalDelay; // do not clamp; may be negative in preroll
            beatIndicator->setTrackPositionDeckB(audibleTimeSec);
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
        if (!requester || !deckA || !deckB || !playerA || !playerB) return;
        // Determine master and target
        QtDeckWidget* masterDeck = (requester == deckA) ? deckB : deckA;
        QtDeckWidget* targetDeck = requester;
    DJAudioPlayer* masterPlayer = (requester == deckA) ? playerB.get() : playerA.get();
    DJAudioPlayer* targetPlayer = (requester == deckA) ? playerA.get() : playerB.get();

        // Compute target tempo factor so target effective BPM equals master's effective BPM
        double masterBpm = masterDeck->getDetectedBpm();
        double masterFactor = masterDeck->getTempoFactor();
        double targetBpm = targetDeck->getDetectedBpm();
        if (masterBpm <= 0.0 || targetBpm <= 0.0) return; // need BPM info on both
        double masterEffective = masterBpm * masterFactor;
        double desiredFactor = masterEffective / targetBpm;
        
        // Apply tempo precisely (not limited by slider quantization)
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
    connect(deckA, &QtDeckWidget::displayedBpmChanged, this, [this](double displayed){
    Q_UNUSED(displayed);
    updateOverviewLabel(true);
    });
    connect(deckB, &QtDeckWidget::displayedBpmChanged, this, [this](double displayed){
    Q_UNUSED(displayed);
    updateOverviewLabel(false);
    });

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
    decksLayout->setSpacing(8);
    decksLayout->addWidget(deckA->getControlsWidget(), 2);
    
    // Mixer section in the center (more compact)
    auto mixerSection = new QVBoxLayout;
    mixerSection->setSpacing(4);
    auto crossfaderLabel = new QLabel("CROSSFADER", this);
    crossfaderLabel->setAlignment(Qt::AlignCenter);
    crossfaderLabel->setStyleSheet("font-weight: bold; color: #fff; font-size: 10px;");
    crossfaderLabel->setFixedHeight(16);
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
    leftHigh->setFixedSize(35, 35); leftMid->setFixedSize(35, 35); 
    leftLow->setFixedSize(35, 35); leftFilter->setFixedSize(35, 35);
    rightHigh->setFixedSize(35, 35); rightMid->setFixedSize(35, 35);
    rightLow->setFixedSize(35, 35); rightFilter->setFixedSize(35, 35);

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
    leftVolumeSlider->setFixedHeight(60);
    leftVolumeSlider->setFixedWidth(20);
    
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
    rightVolumeSlider->setFixedHeight(60);
    rightVolumeSlider->setFixedWidth(20);
    
    rightVolLayout->addWidget(rightVolLabel);
    rightVolLayout->addWidget(rightVolumeSlider);
    
    volumeLayout->addLayout(leftVolLayout);
    volumeLayout->addLayout(rightVolLayout);
    
    mixerSection->addLayout(volumeLayout);
    mixerSection->addStretch();
    
    auto mixerWidget = new QWidget(this);
    mixerWidget->setLayout(mixerSection);
    mixerWidget->setFixedWidth(130);
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
    mainLayout->setSpacing(3);
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
    mainLayout->addLayout(decksLayout, 2);       // Deck controls + mixer (reduced from 4 to 2)
    mainLayout->addLayout(libLayout, 2);         // Library at bottom (increased from 1 to 2)
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
        if (audioError.isNotEmpty()) {
            std::cerr << "Audio initialization failed: " << audioError.toRawUTF8() << std::endl;
            return;
        }

        auto* currentDevice = deviceManager.getCurrentAudioDevice();
        if (!currentDevice) {
            std::cerr << "No audio device available" << std::endl;
            return;
        }

        int availableChannels = currentDevice->getActiveOutputChannels().toInteger();
        if (availableChannels < 2) {
            std::cerr << "Audio device provides limited output channels: " << availableChannels << std::endl;
        }

        if (playerA) {
            playerA->prepareToPlay(currentDevice->getCurrentBufferSizeSamples(), currentDevice->getCurrentSampleRate());
        }
        if (playerB) {
            playerB->prepareToPlay(currentDevice->getCurrentBufferSizeSamples(), currentDevice->getCurrentSampleRate());
        }

        stereoCallback = std::make_unique<StereoAudioCallback>(playerA.get(), playerB.get());
        deviceManager.addAudioCallback(stereoCallback.get());
        deviceManager.addAudioCallback(&masterLevelMonitor);

        if (crossfader) {
            onCrossfader(crossfader->value());
        }

    } catch (const std::exception& e) {
        std::cerr << "Audio initialization threw: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Audio initialization threw unknown exception" << std::endl;
    }
}

QtMainWindow::~QtMainWindow()
{
    std::cout << "QtMainWindow destructor called" << std::endl;
    if (qApp)
        qApp->removeEventFilter(this);
    if (cursorOverridden) {
        QApplication::restoreOverrideCursor();
        cursorOverridden = false;
        currentCursorShape = Qt::ArrowCursor;
    }
    // Only clean up if closeEvent hasn't already done it
    if (!cleanupCompleted) {
        performCleanup();
    }
}

void QtMainWindow::performCleanup()
{
    if (cleanupCompleted) return; // Prevent double cleanup
    
    std::cout << "Performing cleanup..." << std::endl;
    try {
        // 1. Stop all audio players
        if (playerA) {
            playerA->stop();
            std::cout << "Player A stopped" << std::endl;
        }
        if (playerB) {
            playerB->stop();
            std::cout << "Player B stopped" << std::endl;
        }
        
        // 2. Remove audio callbacks before closing device
        // deviceManager.removeAudioCallback(&mixer); // Removed - no longer using mixer
        if (stereoCallback) {
            deviceManager.removeAudioCallback(stereoCallback.get());
        }
        deviceManager.removeAudioCallback(&masterLevelMonitor);
        std::cout << "Audio callbacks removed" << std::endl;
        
        // 4. No sources to disconnect (using custom callback now)
        std::cout << "No sources to disconnect (using stereo callback)" << std::endl;
        
        // 5. Close audio device
        deviceManager.closeAudioDevice();
        std::cout << "Audio device closed" << std::endl;
        
        // 6. Wait for any pending BPM analysis
        if (bpmThreadPool) {
            bpmThreadPool->waitForDone(1000); // Reduced timeout
            std::cout << "BPM thread pool finished" << std::endl;
        }
        
        // 7. Delete players safely
    playerA.reset();
    std::cout << "Player A deleted" << std::endl;

    playerB.reset();
    std::cout << "Player B deleted" << std::endl;
        
    bpmAnalyzer.reset();
    std::cout << "BPM analyzer deleted" << std::endl;
        
        // 8. Handle shared format manager cleanup
        if (sharedFormatManager && sharedFormatManager.use_count() == 1) {
            sharedFormatManager.reset();
            std::cout << "Format manager cleaned up" << std::endl;
        }
        
        cleanupCompleted = true;
        std::cout << "Cleanup complete" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "Exception during cleanup: " << e.what() << std::endl;
        cleanupCompleted = true; // Mark as completed even if there was an error
    } catch (...) {
        std::cout << "Unknown exception during cleanup" << std::endl;
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

    std::cout << "QtMainWindow::closeEvent called - shutting down..." << std::endl;

    // BetaPulseX: Speichere alle Deck-Einstellungen
    try {
        DeckSettings::instance().setVisualTrim(0, userVisualTrimA);  // Deck A
        DeckSettings::instance().setVisualTrim(1, userVisualTrimB);  // Deck B

        DeckSettings::instance().saveSettings();
        qDebug() << "BetaPulseX: All deck settings saved successfully";
    } catch (...) {
        qWarning() << "Failed to save deck settings";
    }

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

    std::cout << "Accepting close event and quitting application" << std::endl;
    event->accept();
    QApplication::quit();
}

void QtMainWindow::onCrossfader(int v) {
    std::cout << "Crossfader changed to: " << v << std::endl;
    // v: 0 => full A (left), 50 => center, 100 => full B (right)
    // Convert to -1.0f (full A) to +1.0f (full B)
    float crossPos = (float(v) - 50.0f) / 50.0f;  // -1.0 to +1.0
    if (stereoCallback) {
        stereoCallback->setCrossfader(crossPos);
    }
}

// MIDI control access method
void QtMainWindow::setCrossfaderPosition(float normalizedValue) {
    // normalizedValue: 0.0 = full A, 1.0 = full B
    // Convert to slider range: 0-100
    int sliderValue = static_cast<int>(normalizedValue * 100.0f);
    sliderValue = std::max(0, std::min(100, sliderValue));
    
    if (crossfader) {
        crossfader->setValue(sliderValue);
        // This will trigger onCrossfader automatically through the signal connection
        std::cout << "MIDI: Crossfader set to " << sliderValue << " (normalized: " << normalizedValue << ")" << std::endl;
    }
}

// MIDI control access methods for Play/Pause
void QtMainWindow::setDeckAPlayPause(bool shouldPlay) {
    if (deckA) {
        // The onPlayPause() method handles both play and pause - it toggles the state
        // For MIDI, we can trigger it when shouldPlay changes the current state
        deckA->onPlayPause();
        std::cout << "MIDI: Deck A Play/Pause triggered (target state: " << (shouldPlay ? "PLAY" : "PAUSE") << ")" << std::endl;
    }
}

void QtMainWindow::setDeckBPlayPause(bool shouldPlay) {
    if (deckB) {
        // The onPlayPause() method handles both play and pause - it toggles the state  
        // For MIDI, we can trigger it when shouldPlay changes the current state
        deckB->onPlayPause();
        std::cout << "MIDI: Deck B Play/Pause triggered (target state: " << (shouldPlay ? "PLAY" : "PAUSE") << ")" << std::endl;
    }
}

// MIDI control access methods for Tempo/Pitch
void QtMainWindow::setDeckATempo(float normalizedValue) {
    if (playerA && deckA) {
        // Get the current tempo range from the deck widget
        double minTempo = deckA->getMinTempoFactor();
        double maxTempo = deckA->getMaxTempoFactor();
        
        // Convert 0.0-1.0 MIDI range to the actual tempo range of the deck
        // 0.0 = minimum tempo, 0.5 = normal (1.0), 1.0 = maximum tempo
        double pitchValue;
        if (normalizedValue <= 0.5f) {
            // Scale from min to 1.0
            pitchValue = minTempo + (normalizedValue * 2.0f) * (1.0 - minTempo);
        } else {
            // Scale from 1.0 to max
            pitchValue = 1.0 + ((normalizedValue - 0.5f) * 2.0f) * (maxTempo - 1.0);
        }
        
        // Clamp to the deck's actual range
        pitchValue = std::max(minTempo, std::min(maxTempo, pitchValue));
        
        // Update both the player and the UI via deck widget
        deckA->setTempoFactor(pitchValue);
        std::cout << "MIDI: Deck A Tempo set to " << pitchValue << " (normalized: " << normalizedValue 
                  << ", range: " << minTempo << " - " << maxTempo << ")" << std::endl;
    }
}

void QtMainWindow::setDeckBTempo(float normalizedValue) {
    if (playerB && deckB) {
        // Get the current tempo range from the deck widget
        double minTempo = deckB->getMinTempoFactor();
        double maxTempo = deckB->getMaxTempoFactor();
        
        // Convert 0.0-1.0 MIDI range to the actual tempo range of the deck
        // 0.0 = minimum tempo, 0.5 = normal (1.0), 1.0 = maximum tempo
        double pitchValue;
        if (normalizedValue <= 0.5f) {
            // Scale from min to 1.0
            pitchValue = minTempo + (normalizedValue * 2.0f) * (1.0 - minTempo);
        } else {
            // Scale from 1.0 to max
            pitchValue = 1.0 + ((normalizedValue - 0.5f) * 2.0f) * (maxTempo - 1.0);
        }
        
        // Clamp to the deck's actual range
        pitchValue = std::max(minTempo, std::min(maxTempo, pitchValue));
        
        // Update both the player and the UI via deck widget
        deckB->setTempoFactor(pitchValue);
        std::cout << "MIDI: Deck B Tempo set to " << pitchValue << " (normalized: " << normalizedValue 
                  << ", range: " << minTempo << " - " << maxTempo << ")" << std::endl;
    }
}

// MIDI control access methods for Volume
void QtMainWindow::setDeckAVolume(float normalizedValue) {
    if (playerA && stereoCallback) {
        // Convert 0.0-1.0 to volume range: 0.0 = mute, 1.0 = full volume
        float volumeValue = std::max(0.0f, std::min(1.0f, normalizedValue));
        
        // Update the mixer volume for Deck A
        stereoCallback->setVolumeA(volumeValue);
        std::cout << "MIDI: Deck A Volume set to " << volumeValue << " (normalized: " << normalizedValue << ")" << std::endl;
    }
}

void QtMainWindow::setDeckBVolume(float normalizedValue) {
    if (playerB && stereoCallback) {
        // Convert 0.0-1.0 to volume range: 0.0 = mute, 1.0 = full volume
        float volumeValue = std::max(0.0f, std::min(1.0f, normalizedValue));
        
        // Update the mixer volume for Deck B
        stereoCallback->setVolumeB(volumeValue);
        std::cout << "MIDI: Deck B Volume set to " << volumeValue << " (normalized: " << normalizedValue << ")" << std::endl;
    }
}

// EQ/filter slot implementations
void QtMainWindow::onLeftHighChanged(int v) {
    std::cout << "onLeftHighChanged called with value: " << v << std::endl;
    // map -100..100 to -1.0..1.0
    double val = v / 100.0;
    if (playerA) {
        std::cout << "  Calling playerA->setHighGain(" << val << ")" << std::endl;
        playerA->setHighGain(val);
    } else {
        std::cout << "  ERROR: playerA is null!" << std::endl;
    }
}

void QtMainWindow::onLeftMidChanged(int v) {
    std::cout << "onLeftMidChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerA) {
        std::cout << "  Calling playerA->setMidGain(" << val << ")" << std::endl;
        playerA->setMidGain(val);
    } else {
        std::cout << "  ERROR: playerA is null!" << std::endl;
    }
}

void QtMainWindow::onLeftLowChanged(int v) {
    std::cout << "onLeftLowChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerA) playerA->setLowGain(val);
}

void QtMainWindow::onLeftFilterChanged(int v) {
    std::cout << "onLeftFilterChanged called with value: " << v << std::endl;
    // map -100..100 to -1..1 (center 0 = bypass)
    double norm = v / 100.0;
    if (playerA) {
        std::cout << "  Calling playerA->setFilterCutoff(" << norm << ")" << std::endl;
        playerA->setFilterCutoff(norm);
    } else {
        std::cout << "  ERROR: playerA is null!" << std::endl;
    }
}

void QtMainWindow::onRightHighChanged(int v) {
    std::cout << "onRightHighChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerB) playerB->setHighGain(val);
}

void QtMainWindow::onRightMidChanged(int v) {
    std::cout << "onRightMidChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerB) playerB->setMidGain(val);
}

void QtMainWindow::onRightLowChanged(int v) {
    std::cout << "onRightLowChanged called with value: " << v << std::endl;
    double val = v / 100.0;
    if (playerB) playerB->setLowGain(val);
}

void QtMainWindow::onRightFilterChanged(int v) {
    // map -100..100 to -1..1 (center 0 = bypass)
    double norm = v / 100.0;
    if (playerB) playerB->setFilterCutoff(norm);
}

void QtMainWindow::onLeftVolumeChanged(int v) {
    std::cout << "Left volume changed to: " << v << std::endl;
    if (stereoCallback) {
        float volume = juce::jlimit(0.0f, 1.0f, (float)v / 100.0f);
        stereoCallback->setVolumeA(volume);
    }
}

void QtMainWindow::onRightVolumeChanged(int v) {
    std::cout << "Right volume changed to: " << v << std::endl;
    if (stereoCallback) {
        float volume = juce::jlimit(0.0f, 1.0f, (float)v / 100.0f);
        stereoCallback->setVolumeB(volume);
    }
}

void QtMainWindow::keyPressEvent(QKeyEvent* event) {
    // Check if focus is on a line edit or text widget to avoid interfering with text input
    QWidget* focusWidget = QApplication::focusWidget();
    if (focusWidget && (qobject_cast<QLineEdit*>(focusWidget) || 
                       qobject_cast<QTextEdit*>(focusWidget) ||
                       qobject_cast<QPlainTextEdit*>(focusWidget))) {
        // Let the focused text widget handle the key event
        QWidget::keyPressEvent(event);
        return;
    }

    // Global keyboard shortcuts for beat grid zoom
    switch (event->key()) {
        case Qt::Key_F5: // Deck A: -1 ms
            userVisualTrimA = std::clamp(userVisualTrimA - 0.001, -0.05, 0.05);
            updateOverviewLabel(true);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckA", userVisualTrimA);
            }
            event->accept();
            break;
        case Qt::Key_F6: // Deck A: +1 ms
            userVisualTrimA = std::clamp(userVisualTrimA + 0.001, -0.05, 0.05);
            updateOverviewLabel(true);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckA", userVisualTrimA);
            }
            event->accept();
            break;
        case Qt::Key_F7: // Deck B: -1 ms
            userVisualTrimB = std::clamp(userVisualTrimB - 0.001, -0.05, 0.05);
            updateOverviewLabel(false);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckB", userVisualTrimB);
            }
            event->accept();
            break;
        case Qt::Key_F8: // Deck B: +1 ms
            userVisualTrimB = std::clamp(userVisualTrimB + 0.001, -0.05, 0.05);
            updateOverviewLabel(false);
            {
                QSettings settings("DJDavid", "David");
                settings.setValue("visualTrim/deckB", userVisualTrimB);
            }
            event->accept();
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:  // Handle both + and = key (same physical key)
            // Increase beat grid zoom on both waveforms
            if (overviewTopA) {
                overviewTopA->increaseBeatGridZoom();
            }
            if (overviewTopB) {
                overviewTopB->increaseBeatGridZoom();
            }
            event->accept();
            break;
            
        case Qt::Key_Minus:
        case Qt::Key_Underscore:  // Handle both - and _ key (same physical key)
            // Decrease beat grid zoom on both waveforms
            if (overviewTopA) {
                overviewTopA->decreaseBeatGridZoom();
            }
            if (overviewTopB) {
                overviewTopB->decreaseBeatGridZoom();
            }
            event->accept();
            break;
            
        case Qt::Key_0:
            // Reset beat grid zoom on both waveforms
            if (overviewTopA) {
                overviewTopA->resetBeatGridZoom();
            }
            if (overviewTopB) {
                overviewTopB->resetBeatGridZoom();
            }
            event->accept();
            break;
            
        default:
            // Let the base class handle other keys
            QWidget::keyPressEvent(event);
            break;
    }
}




// Event filter for double-click reset functionality and frameless resizing/dragging

void QtMainWindow::updatePlaybackPositions() {
    // Only update when not scratching to prevent interference
    static double lastPosA = -999.0;
    static double lastPosB = -999.0;
    
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    
    bool canUpdateA = playerA && overviewTopA && !overviewTopA->isScratching() && 
                      (currentTime - lastScratchEndA > 100); // 100ms delay after scratch end
    
    if (canUpdateA) {
        double relativePos = playerA->getPositionRelative();
        
        bool isPlaybackUpdate = playerA->isPlaying() || relativePos < 0.0;
        
        double threshold = (relativePos < 0.0) ? 0.0002 : 0.008; // Preroll: 0.0002, Song: 0.008
        
        if (isPlaybackUpdate && std::abs(relativePos - lastPosA) > threshold) {
            lastPosA = relativePos;
            
            overviewTopA->setPlayhead(relativePos);
            if (deckA && deckA->getWaveform()) {
                deckA->getWaveform()->setPlayhead(relativePos);
            }
            
            if (beatIndicator && playerA) {
                double lenSec = std::max(1e-9, playerA->getLengthInSeconds());
                constexpr double prerollSec = 8.0; // Keep in sync with WaveformDisplay/DJAudioPlayer
                double seconds = (relativePos < 0.0) ? (relativePos * prerollSec) : (relativePos * lenSec);
                beatIndicator->setTrackPositionDeckA(seconds);
            }
            
        }
    } else if (overviewTopA && overviewTopA->isScratching()) {
        lastScratchEndA = currentTime;
    }
    
    bool canUpdateB = playerB && overviewTopB && !overviewTopB->isScratching() && 
                      (currentTime - lastScratchEndB > 100); // 100ms delay after scratch end
    
    if (canUpdateB) {
        double relativePos = playerB->getPositionRelative();
        
        bool isPlaybackUpdate = playerB->isPlaying() || relativePos < 0.0;
        
        double threshold = (relativePos < 0.0) ? 0.0002 : 0.008; // Preroll: 0.0002, Song: 0.008
        
        if (isPlaybackUpdate && std::abs(relativePos - lastPosB) > threshold) {
            lastPosB = relativePos;
            
            overviewTopB->setPlayhead(relativePos);
            if (deckB && deckB->getWaveform()) {
                deckB->getWaveform()->setPlayhead(relativePos);
            }
            
            if (beatIndicator && playerB) {
                double lenSec = std::max(1e-9, playerB->getLengthInSeconds());
                constexpr double prerollSec = 8.0; // Keep in sync with WaveformDisplay/DJAudioPlayer
                double seconds = (relativePos < 0.0) ? (relativePos * prerollSec) : (relativePos * lenSec);
                beatIndicator->setTrackPositionDeckB(seconds);
            }
            
        }
    } else if (overviewTopB && overviewTopB->isScratching()) {
        lastScratchEndB = currentTime;
    }
}

