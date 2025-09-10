#include "QtDeckWidget.h"
#include "DJAudioPlayer.h"
#include "WaveformGenerator.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QTimer>
#include <QMimeData>
#include <QRunnable>
#include <QThreadPool>
#include <QThread>
#include <QPointer>
#include <chrono>
#include <thread>
#include <iostream>

QtDeckWidget::QtDeckWidget(DJAudioPlayer* player_, QWidget* parent, const QString& deckTitle, bool isLeftDeck)
    : QWidget(parent), player(player_)
{
    qDebug() << "QtDeckWidget constructor called for" << deckTitle << "isLeftDeck:" << isLeftDeck;
    std::cout << "=== QtDeckWidget constructor for " << deckTitle.toStdString() << " ===" << std::endl;
    waveform = new DeckWaveformOverview(this);
    
    // Initialize status timer for play state synchronization
    statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, &QtDeckWidget::syncPlayState);
    // UPDATED: Use faster interval for responsive loop visualization
    statusTimer->start(100); // Check every 100ms for responsive loop updates
    
    // Initialize cue click timer for double-click detection
    cueClickTimer = new QTimer(this);
    cueClickTimer->setSingleShot(true);
    connect(cueClickTimer, &QTimer::timeout, this, [this]() { cueClickPending = false; });
    
    // Create separate controls widget
    controlsWidget = new QWidget(this);
    controlsWidget->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333;");
    
    deckTitleLabel = new QLabel(deckTitle, controlsWidget);
    deckTitleLabel->setAlignment(Qt::AlignCenter);
    deckTitleLabel->setStyleSheet("font-weight: bold; font-size: 18px; color: #fff; padding: 10px;");
    
    songNameLabel = new QLabel("No Track Loaded", controlsWidget);
    songNameLabel->setAlignment(Qt::AlignCenter);
    songNameLabel->setStyleSheet("font-size: 12px; color: #ccc; padding: 5px;");
    // Prevent songNameLabel from expanding and changing layout when file names vary in length
    songNameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    songNameLabel->setFixedHeight(18);
    deckTitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    deckTitleLabel->setFixedHeight(32);

    turntable = new QtTurntableWidget(controlsWidget);
    playPauseBtn = new QPushButton("Play", controlsWidget);
    loadBtn = new QPushButton("Load", controlsWidget);
    cueBtn = new QPushButton("Cue", controlsWidget);
    keylockBtn = new QPushButton("Key", controlsWidget);
    quantizeBtn = new QPushButton("Q", controlsWidget);
    syncBtn = new QPushButton("Sync", controlsWidget);
    speedSlider = new QSlider(Qt::Vertical, controlsWidget);
    tempoValueLabel = new QLabel("1.000x", controlsWidget);
    tempoSpin = new QDoubleSpinBox(controlsWidget);
    bpmDefaultLabel = new QLabel("BPM: --", controlsWidget);
    bpmCurrentLabel = new QLabel("Curr: --", controlsWidget);
    speedLabel = new QLabel("Speed", controlsWidget);

    // Style the controls (more compact)
    playPauseBtn->setStyleSheet("QPushButton { background-color: #0066cc; color: white; border: none; padding: 4px; font-weight: bold; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #0052a3; }");
    loadBtn->setStyleSheet("QPushButton { background-color: #666; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #777; }");
    cueBtn->setStyleSheet("QPushButton { background-color: #ff6600; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #e55a00; }");
    keylockBtn->setStyleSheet("QPushButton { background-color: #333; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #444; } QPushButton:checked { background-color: #00cc66; }");
    quantizeBtn->setStyleSheet("QPushButton { background-color: #333; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #444; } QPushButton:checked { background-color: #cc6600; }");
    syncBtn->setStyleSheet("QPushButton { background-color: #008844; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #00733a; } QPushButton:checked { background-color: #00aa55; }");
    
    // Make keylock and quantize buttons checkable
    keylockBtn->setCheckable(true);
    quantizeBtn->setCheckable(true);
    
    // Add tooltips
    keylockBtn->setToolTip("Keylock - maintains original pitch when speed changes");
    quantizeBtn->setToolTip("Quantize - snaps cues and loops to nearest beat");
    syncBtn->setToolTip("Sync tempo & phase to the other deck");
    
    // Tempo fader: ±16% with 0.001 precision via slider
    speedSlider->setRange(840, 1160);     // store factor*1000
    speedSlider->setSingleStep(1);        // 0.001 per step
    speedSlider->setPageStep(5);          // 0.005 per page
    speedSlider->setTracking(true);       // continuous updates
    speedSlider->setValue(1000);          // 1.000x default
    // High precision spin (4 decimals)
    tempoSpin->setDecimals(4);
    tempoSpin->setRange(0.8400, 1.1600);
    tempoSpin->setSingleStep(0.0005);
    tempoSpin->setValue(1.0000);
    tempoSpin->setKeyboardTracking(false);
    
    speedLabel->setStyleSheet("color: #fff; font-size: 9px; font-weight: bold;");
    bpmDefaultLabel->setStyleSheet("color: #0088ff; font-size: 9px; font-weight: bold;");
    bpmCurrentLabel->setStyleSheet("color: #ff8800; font-size: 9px; font-weight: bold;");
    tempoValueLabel->setStyleSheet("color: #fff; font-size: 10px; font-weight: bold;");

    connect(playPauseBtn, &QPushButton::clicked, this, &QtDeckWidget::onPlayPause);
    connect(loadBtn, &QPushButton::clicked, this, &QtDeckWidget::onLoad);
    connect(cueBtn, &QPushButton::clicked, this, &QtDeckWidget::onCue);
    connect(cueBtn, &QPushButton::pressed, this, &QtDeckWidget::onCuePressed);
    connect(cueBtn, &QPushButton::released, this, &QtDeckWidget::onCueReleased);
    connect(keylockBtn, &QPushButton::clicked, this, &QtDeckWidget::onKeylockToggle);
    connect(quantizeBtn, &QPushButton::clicked, this, &QtDeckWidget::onQuantizeToggle);
    syncBtn->setCheckable(true);
    connect(syncBtn, &QPushButton::clicked, this, &QtDeckWidget::onSync);          // immediate one-shot sync
    connect(syncBtn, &QPushButton::toggled, this, &QtDeckWidget::onSyncToggled);   // follow mode on/off
    connect(speedSlider, &QSlider::valueChanged, this, &QtDeckWidget::onSpeedChanged);
    // Two-way bind spin <-> slider
    connect(tempoSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &QtDeckWidget::onTempoSpinChanged);
    // Double-click reset on slider
    speedSlider->installEventFilter(this);
    
    // Connect overview click/drag to seek position and broadcast immediately
    connect(waveform, &DeckWaveformOverview::positionClicked, this, [this](double relative) {
        if (player) {
            relative = std::clamp(relative, 0.0, 1.0);
            player->setPositionRelative(relative);
            // Immediately notify listeners (top waveform) so UI follows while paused
            emit playheadUpdated(relative);
            // If paused, ensure next Play resumes from here
            if (!playing) {
                // DJAudioPlayer updates pausedPosSec internally, but keep UI in sync
                // Nothing else needed here; handled in player
            }
        }
    });
    
    // Layout for controls widget (More compact)
    auto controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setSpacing(2);  // Reduced from 3
    controlsLayout->setContentsMargins(4, 4, 4, 4);  // Reduced from 6
    
    // Header section: Title and track name
    auto headerLayout = new QVBoxLayout;
    headerLayout->setSpacing(1);
    deckTitleLabel->setFixedHeight(20);
    songNameLabel->setFixedHeight(20); // Increased from 14 to 20 for better readability
    headerLayout->addWidget(deckTitleLabel);
    headerLayout->addWidget(songNameLabel);
    controlsLayout->addLayout(headerLayout);
    
    // Waveform overview (more compact)
    waveform->setFixedHeight(25);
    waveform->setStyleSheet("border: 1px solid #444; border-radius: 0px;");
    controlsLayout->addWidget(waveform);
    waveform->setAcceptDrops(true);
    connect(waveform, &DeckWaveformOverview::fileDropped, this, [this](const QString &path){ this->loadFile(path); });
    
    // Main control section: Layout depends on deck position
    // For left deck (A): Controls | Turntable
    // For right deck (B): Turntable | Controls  
    auto mainControlsLayout = new QHBoxLayout;
    mainControlsLayout->setSpacing(6);
    
    // Performance Pads section (standalone)
    PerformancePads::DeckId deckId = isLeftDeck ? PerformancePads::DeckId::A : PerformancePads::DeckId::B;
    qDebug() << "QtDeckWidget: About to create PerformancePads for deck" << (deckId == PerformancePads::DeckId::A ? "A" : "B");
    std::cout << "=== About to create PerformancePads ===" << std::endl;
    pads = new PerformancePads(deckId, controlsWidget);
    qDebug() << "QtDeckWidget: PerformancePads created successfully";
    std::cout << "=== PerformancePads created successfully ===" << std::endl;
    pads->setAudioPlayer(player);
    pads->setMaximumHeight(120);
    pads->setMaximumWidth(380);
    
    // Connect performance pads cue points to waveform displays (after pads are created)
    connect(pads, &PerformancePads::cuePointsChanged, waveform, &DeckWaveformOverview::setCuePoints);
    
    // Turntable section: Transport buttons, turntable, BPM and tempo slider below
    auto turntableSection = new QVBoxLayout;
    turntableSection->setSpacing(3);
    
    // Transport buttons above turntable
    auto transportLayout = new QHBoxLayout;
    transportLayout->setSpacing(2);  // Reduced spacing for smaller buttons
    playPauseBtn->setFixedHeight(20);  // Smaller height
    loadBtn->setFixedHeight(20);       // Smaller height
    cueBtn->setFixedHeight(20);        // Smaller height
    keylockBtn->setFixedHeight(20);    // Smaller height
    quantizeBtn->setFixedHeight(20);   // Smaller height
    playPauseBtn->setFixedWidth(40);   // Smaller width
    loadBtn->setFixedWidth(40);        // Smaller width
    cueBtn->setFixedWidth(30);         // Smaller width for Cue
    keylockBtn->setFixedWidth(30);     // Smaller width for Key
    quantizeBtn->setFixedWidth(25);    // Smaller width for Q
    transportLayout->addWidget(playPauseBtn);
    transportLayout->addWidget(loadBtn);
    transportLayout->addWidget(cueBtn);
    transportLayout->addWidget(keylockBtn);
    transportLayout->addWidget(quantizeBtn);
    transportLayout->addWidget(syncBtn);
    turntableSection->addLayout(transportLayout);
    
    // Turntable (smaller to save space)
    turntable->setFixedSize(90, 90);

    // BPM + Tempo panel (to sit beside the turntable)
    auto bpmLayout = new QVBoxLayout;
    bpmLayout->setSpacing(1);
    bpmDefaultLabel->setFixedHeight(12);
    bpmCurrentLabel->setFixedHeight(12);
    bpmDefaultLabel->setFixedWidth(50);
    bpmCurrentLabel->setFixedWidth(50);
    bpmDefaultLabel->setAlignment(Qt::AlignCenter);
    bpmCurrentLabel->setAlignment(Qt::AlignCenter);
    bpmLayout->addWidget(bpmDefaultLabel);
    bpmLayout->addWidget(bpmCurrentLabel);

    auto speedSection = new QVBoxLayout;
    speedSection->setSpacing(1);
    speedSection->setAlignment(Qt::AlignCenter);
    speedLabel->setFixedHeight(12);
    speedSlider->setFixedHeight(50); // Much smaller
    speedSlider->setFixedWidth(20);  // Narrower
    tempoValueLabel->setFixedHeight(12);
    tempoSpin->setFixedWidth(60);
    speedSection->addWidget(speedLabel, 0, Qt::AlignCenter);
    speedSection->addWidget(speedSlider, 0, Qt::AlignCenter);
    speedSection->addWidget(tempoValueLabel, 0, Qt::AlignCenter);
    speedSection->addWidget(tempoSpin, 0, Qt::AlignCenter);

    auto bpmTempoPanel = new QVBoxLayout;
    bpmTempoPanel->setSpacing(2);
    bpmTempoPanel->addLayout(bpmLayout);
    bpmTempoPanel->addLayout(speedSection);

    // Place platter pinned to the deck edge and panel next to it
    auto platterRow = new QHBoxLayout;
    platterRow->setSpacing(6);
    if (isLeftDeck) {
        // Deck A: pin platter to right edge, place panel to its left
        platterRow->addStretch();
        platterRow->addLayout(bpmTempoPanel, 0);
        platterRow->addWidget(turntable, 0, Qt::AlignRight | Qt::AlignTop);
    } else {
        // Deck B: pin platter to left edge, place panel to its right
        platterRow->addWidget(turntable, 0, Qt::AlignLeft | Qt::AlignTop);
        platterRow->addLayout(bpmTempoPanel, 0);
        platterRow->addStretch();
    }
    turntableSection->addLayout(platterRow);
    turntableSection->addStretch();
    
    // Arrange sections based on deck position
    if (isLeftDeck) {
        // Deck A: Performance Pads on left, turntable on right (closer to mixer)
        mainControlsLayout->addWidget(pads, 2, Qt::AlignTop);
        mainControlsLayout->addLayout(turntableSection, 1);
    } else {
        // Deck B: Turntable on left (closer to mixer), performance pads on right
        mainControlsLayout->addLayout(turntableSection, 1);
        mainControlsLayout->addWidget(pads, 2, Qt::AlignTop);
    }
    
    controlsLayout->addLayout(mainControlsLayout);
    controlsWidget->setLayout(controlsLayout);

    // accept drops on the controls widget and forward them
    controlsWidget->setAcceptDrops(true);
    controlsWidget->installEventFilter(this);

    // Main deck widget layout (only controls widget, waveform is integrated)
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(controlsWidget);
    setLayout(mainLayout);

    setAcceptDrops(true);

    // Poll player position and update waveform playhead at ~60 FPS for smooth visuals
    QTimer* t = new QTimer(this);
    t->setTimerType(Qt::PreciseTimer);
    t->setInterval(16);
    connect(t, &QTimer::timeout, [this]() {
        if (!player) return;
        // Always reflect current position, even when paused, to keep displays in sync
        static double lastPos = -1.0;
        double len = std::max(1e-9, player->getLengthInSeconds());
        double pos = std::clamp(player->getCurrentPositionSeconds() / len, 0.0, 1.0);
        waveform->setPlayhead(pos);

        // Update turntable with current position and BPM for beat synchronization
        turntable->setPlayheadPosition(pos);
        if (detectedBpm > 0.0) {
            turntable->setBpm(detectedBpm);
            double trackLengthSec = player->getLengthInSeconds();
            if (trackLengthSec > 0.0) {
                turntable->setTrackLength(trackLengthSec);
            }
        }

        // Emit only when the position changed to minimize redundant updates
        if (std::abs(pos - lastPos) > 1e-6) {
            emit playheadUpdated(pos);
            lastPos = pos;
        }
    });
    t->start();
}

void QtDeckWidget::loadFile(const QString &path) {
    currentFilePath = path;  // Store the current file path
    // Don't generate waveform on UI thread; schedule lightweight background generation
    if (!path.isEmpty()) {
        class SmallOverviewTask : public QRunnable {
        public:
            SmallOverviewTask(DeckWaveformOverview* w, QString p)
                : wf(w), filePath(std::move(p)) { setAutoDelete(true); }
            void run() override {
                if (!wf) return;
                try {
                    QThread::currentThread()->setPriority(QThread::LowestPriority);
                    WaveformGenerator gen;
                    WaveformGenerator::Result res;
                    const int bins = 4000;
                    if (!gen.generate(juce::File(filePath.toStdString()), bins, res)) return;
                    auto data = std::make_shared<std::vector<float>>(res.maxBins.size());
                    for (size_t i = 0; i < res.maxBins.size(); ++i) {
                        (*data)[i] = std::min(1.0f, std::abs(res.maxBins[i]));
                    }
                    const double audioStart = res.audioStartOffsetSec;
                    const double lengthSec = res.lengthSeconds;
                    QMetaObject::invokeMethod(wf, [w = wf, data, audioStart, lengthSec]() {
                        if (w) w->setWaveformData(*data, audioStart, lengthSec);
                    }, Qt::QueuedConnection);
                } catch (...) {}
            }
        private:
            QPointer<DeckWaveformOverview> wf;
            QString filePath;
        };
        // Use global QThreadPool to run background task
        QThreadPool::globalInstance()->start(new SmallOverviewTask(waveform, path));
    }
    QFileInfo fi(path);
    songNameLabel->setText(fi.fileName());
    if (player) {
        // NEW: Start threaded loading instead of blocking synchronous load
        emit fileLoadingStarted(path);  // Signal to start background loading
        
        // Update UI immediately to show loading state
        playPauseBtn->setText("Loading...");
        playPauseBtn->setEnabled(false);
        loadBtn->setText("Loading...");
        loadBtn->setEnabled(false);
        
        waveform->setPlayhead(0.0);
        playing = false;
        turntable->stop();
        
        // Reset cue point and cueing state when loading new file
        cuePosition = 0.0;
        isCueing = false;
    }
    // do NOT start turntable here
}

// NEW: Handle completion of threaded file loading
void QtDeckWidget::onFileLoadingComplete(const QString& filePath) {
    if (player && currentFilePath == filePath) {
        // Re-enable controls after loading is complete
        playPauseBtn->setText("Play");
        playPauseBtn->setEnabled(true);
        loadBtn->setText("Unload");
        loadBtn->setEnabled(true);
        
        // Emit the file loaded signal for other components (BPM analysis, etc.)
        emit fileLoaded();
    }
}

void QtDeckWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void QtDeckWidget::dropEvent(QDropEvent* event) {
    auto urls = event->mimeData()->urls();
    if (!urls.isEmpty()) {
        auto path = urls.first().toLocalFile();
        loadFile(path);
    }
}

bool QtDeckWidget::eventFilter(QObject* obj, QEvent* event) {
    // Forward drag/drop events that occur on the controlsWidget to the deck
    if ((event->type() == QEvent::DragEnter) || (event->type() == QEvent::Drop)) {
        QDropEvent* drop = dynamic_cast<QDropEvent*>(static_cast<QEvent*>(event));
        QDragEnterEvent* drag = dynamic_cast<QDragEnterEvent*>(static_cast<QEvent*>(event));
        if (event->type() == QEvent::DragEnter) {
            auto de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::Drop) {
            auto de = static_cast<QDropEvent*>(event);
            auto urls = de->mimeData()->urls();
            if (!urls.isEmpty()) {
                auto path = urls.first().toLocalFile();
                loadFile(path);
                return true;
            }
        }
    }
    // Double-click on speed slider resets tempo to 1.000x
    if (obj == speedSlider && event->type() == QEvent::MouseButtonDblClick) {
        setTempoFactor(1.0);
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void QtDeckWidget::onPlayPause() {
    if (!player) return;
    
    std::cout << "### QtDeckWidget::onPlayPause() CALLED ###" << std::endl;
    std::cout << "  Current playing state: " << playing << std::endl;
    std::cout << "  Button text: " << playPauseBtn->text().toStdString() << std::endl;
    std::cout << "  Current file: " << currentFilePath.toStdString() << std::endl;
    
    // Add debouncing to prevent rapid clicks causing issues
    static auto lastClickTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClickTime).count() < 50) {
        std::cout << "  DEBOUNCE: Ignoring rapid click" << std::endl;
        return; // Ignore clicks within 50ms (reduced from 100ms)
    }
    lastClickTime = now;
    
    // Check if a file is loaded before trying to play
    if (currentFilePath.isEmpty()) {
        std::cout << "  No file loaded - triggering load dialog" << std::endl;
        // If no file loaded, trigger load dialog
        onLoad();
        return;
    }
    
    // Get current state and immediately update UI for instant responsiveness
    bool wasPlaying = playing;
    std::cout << "  Was playing: " << wasPlaying << std::endl;
    
    if (wasPlaying) {
        std::cout << "  STOPPING playback..." << std::endl;
        // Immediate UI update
        playPauseBtn->setText("Play");
        turntable->stop();
        playing = false;
        emit playStateChanged(playing);
        // Start/stop are now ultra-lightweight; call directly for deterministic behavior
        if (player) {
            std::cout << "  Calling player->stop()" << std::endl;
            player->stop();
        }
        std::cout << "  STOP sequence completed" << std::endl;
    } else {
        std::cout << "  STARTING playback..." << std::endl;
        // Track when play was pressed for delayed sync checking
        lastPlayPressTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        // Immediate UI update
        playPauseBtn->setText("Pause");
        turntable->start();
        playing = true;
        emit playStateChanged(playing);
        if (player) {
            std::cout << "  Calling player->start()" << std::endl;
            player->start();
        }
        std::cout << "  START sequence completed" << std::endl;
    }
    std::cout << "### QtDeckWidget::onPlayPause() END ###" << std::endl;
}

void QtDeckWidget::onLoad() {
    if (loadBtn->text() == "Load") {
        // Load file dialog
        QString fn = QFileDialog::getOpenFileName(this, "Open audio file");
        if (!fn.isEmpty()) loadFile(fn);
    } else {
        // Unload current file
        currentFilePath.clear();
        songNameLabel->setText("No Track Loaded");
        loadBtn->setText("Load");
        playPauseBtn->setText("Play");
        if (player) {
            player->stop();
            // Clear the loaded file
        }
        playing = false;
        turntable->stop();
        waveform->setPlayhead(0.0);
        waveform->update();
    }
}

void QtDeckWidget::onCue() {
    if (!player || currentFilePath.isEmpty()) return;
    
    // Handle double-click to set cue point
    if (cueClickPending) {
        // This is the second click - set cue point at current position
        double rawPos = player->getCurrentPositionSeconds();
        // Apply quantization if enabled
        cuePosition = player->quantizePosition(rawPos);
        cueClickPending = false;
        cueClickTimer->stop();
        // Visual feedback could be added here (e.g., brief color change)
    } else {
        // This is the first click - start timer for double-click detection
        cueClickPending = true;
        cueClickTimer->start(300); // 300ms window for double-click
    }
}

void QtDeckWidget::onCuePressed() {
    if (!player || currentFilePath.isEmpty()) return;
    
    if (!isCueing) {
        // Store current position as cue point if not already set
        if (cuePosition == 0.0) {
            double rawPos = player->getCurrentPositionSeconds();
            // Apply quantization if enabled
            cuePosition = player->quantizePosition(rawPos);
        }
        
        // Start cueing: play from cue point
        isCueing = true;
        player->setPositionSeconds(cuePosition);
        if (!playing) {
            player->start();
            turntable->start();
        }
        waveform->setPlayhead(cuePosition / player->getLengthInSeconds());
    }
}

void QtDeckWidget::onCueReleased() {
    if (!player || currentFilePath.isEmpty()) return;
    
    if (isCueing) {
        isCueing = false;
        // Return to cue point and pause
        player->setPositionSeconds(cuePosition);
        player->stop();
        playing = false;
        playPauseBtn->setText("Play");
        turntable->stop();
        waveform->setPlayhead(cuePosition / player->getLengthInSeconds());
        emit playStateChanged(playing);
    }
}

void QtDeckWidget::onVolumeChanged(int v) {
    if (!player) return;
    player->setGain(v / 100.0);
}

void QtDeckWidget::onSpeedChanged(int v) {
    if (!player) return;
    // Slider value encodes factor*1000 for 0.001 precision
    double factor = v / 1000.0;
    applyTempo(factor);
    // update displayed BPM according to detected BPM * factor
    double displayed = detectedBpm > 0.0 ? detectedBpm * factor : 0.0;
    if (bpmCurrentLabel) {
        bpmCurrentLabel->setText(displayed > 0.0 ? QString("Curr: %1").arg(QString::number(displayed, 'f', 1)) : "Curr: --");
    }
    emit displayedBpmChanged(displayed);
    // CRITICAL FIX: Emit tempo factor change for beat grid adjustment
    emit tempoFactorChanged(factor);
}
double QtDeckWidget::getTempoFactor() const {
    if (!speedSlider) return 1.0;
    return speedSlider->value() / 1000.0;
}

void QtDeckWidget::setTempoFactor(double factor) {
    if (!speedSlider) return;
    applyTempo(factor);
}

void QtDeckWidget::onSync() {
    emit syncRequested(this);
}

void QtDeckWidget::onSyncToggled(bool enabled) {
    emit syncToggled(this, enabled);
}

void QtDeckWidget::onTempoSpinChanged(double v) {
    if (!speedSlider) return;
    int asSlider = (int)std::round(v * 1000.0);
    asSlider = std::clamp(asSlider, speedSlider->minimum(), speedSlider->maximum());
    if (speedSlider->value() != asSlider) {
        speedSlider->setValue(asSlider); // triggers onSpeedChanged
    } else {
        applyTempo(v);
    }
}

void QtDeckWidget::applyTempo(double factor) {
    if (!player) return;
    // clamp to UI limits
    double clamped = std::clamp(factor, 0.8400, 1.1600);
    // Update player/turntable
    player->setSpeed(clamped);
    turntable->setSpeed(clamped);
    // update numeric labels
    if (tempoValueLabel) tempoValueLabel->setText(QString::number(clamped, 'f', 3) + "x");
    // keep spin in sync exactly (4dp)
    if (tempoSpin && std::abs(tempoSpin->value() - clamped) > 0.00005) {
        tempoSpin->blockSignals(true);
        tempoSpin->setValue(clamped);
        tempoSpin->blockSignals(false);
    }
    // keep slider in sync (quantized to 0.001, best effort)
    if (speedSlider) {
        int s = (int)std::round(clamped * 1000.0);
        s = std::clamp(s, speedSlider->minimum(), speedSlider->maximum());
        if (speedSlider->value() != s) {
            speedSlider->blockSignals(true);
            speedSlider->setValue(s);
            speedSlider->blockSignals(false);
        }
    }
    // update BPM label and emit signals
    double displayed = detectedBpm > 0.0 ? detectedBpm * clamped : 0.0;
    if (bpmCurrentLabel) {
        bpmCurrentLabel->setText(displayed > 0.0 ? QString("Curr: %1").arg(QString::number(displayed, 'f', 1)) : "Curr: --");
    }
    emit displayedBpmChanged(displayed);
    emit tempoFactorChanged(clamped);
}

void QtDeckWidget::setDetectedBpm(double bpm) {
    detectedBpm = bpm;
    // if speed slider is at some value, update displayed BPM
    double factor = speedSlider ? speedSlider->value() / 1000.0 : 1.0;
    double displayed = detectedBpm > 0.0 ? detectedBpm * factor : 0.0;
    if (bpmDefaultLabel) {
        bpmDefaultLabel->setText(detectedBpm > 0.0 ? QString("BPM: %1").arg(QString::number(detectedBpm, 'f', 1)) : "BPM: --");
    }
    if (bpmCurrentLabel) {
        bpmCurrentLabel->setText(displayed > 0.0 ? QString("Curr: %1").arg(QString::number(displayed, 'f', 1)) : "Curr: --");
    }
    
    // Update turntable with detected BPM for beat synchronization
    if (turntable && detectedBpm > 0.0) {
        turntable->setBpm(detectedBpm);
        // Set track length if available
        if (player) {
            double trackLengthSec = player->getLengthInSeconds();
            if (trackLengthSec > 0.0) {
                turntable->setTrackLength(trackLengthSec);
            }
        }
    }
    
    emit displayedBpmChanged(displayed);
}

void QtDeckWidget::syncPlayState() {
    // Smart sync: Only update UI when there's actually a change to avoid transport interference
    if (!player) return;
    
    // Don't interfere with play state during file loading
    if (playPauseBtn->text() == "Loading...") {
        return;
    }
    
    static int lastUpdateCount = 0;
    static int currentUpdateCount = 0;
    currentUpdateCount++;
    
    // Only sync occasionally to avoid transport interference for play state changes
    bool shouldUpdatePlayState = (currentUpdateCount - lastUpdateCount >= 50);
    if (shouldUpdatePlayState) {
        lastUpdateCount = currentUpdateCount;
    }
    
    // Don't interfere immediately after a play button press to allow transport time to start
    qint64 currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool recentPlayPress = (lastPlayPressTime > 0 && (currentTime - lastPlayPressTime) < 500);
    
    if (shouldUpdatePlayState && !recentPlayPress) {
        bool actuallyPlaying = player->isPlaying();
        bool uiShowsPlaying = (playPauseBtn->text() == "Pause");
        
        // Only update UI if there's a mismatch, don't call any transport actions
        if (actuallyPlaying != uiShowsPlaying) {
            playPauseBtn->setText(actuallyPlaying ? "Pause" : "Play");
            std::cout << "*** UI corrected: UI=" << (actuallyPlaying ? 1 : 0) 
                      << ", Actual=" << (actuallyPlaying ? 1 : 0) << std::endl;
        }
        
        // If no file is loaded, ensure button shows correct text
        if (currentFilePath.isEmpty()) {
            if (playPauseBtn->text() != "Load File") {
                playPauseBtn->setText("Load File");
            }
            if (playing) {
                std::cout << "*** syncPlayState: No file loaded, stopping UI state" << std::endl;
                playing = false;
                turntable->stop();
                emit playStateChanged(playing);
            }
            return;
        }
        
        // CRITICAL: Only update UI, NEVER call player->stop() to avoid oscillation!
        if (actuallyPlaying != playing) {
            std::cout << "*** syncPlayState: State mismatch detected! Correcting UI ONLY..." << std::endl;
            std::cout << "    UI was: " << playing << ", player is: " << actuallyPlaying << std::endl;
            
            playing = actuallyPlaying;
            
            if (playing) {
                std::cout << "    Updating UI to PLAYING state (no player action)" << std::endl;
                playPauseBtn->setText("Pause");
                turntable->start();
            } else {
                std::cout << "    Updating UI to STOPPED state (no player action)" << std::endl;
                playPauseBtn->setText("Play");
                turntable->stop();
            }
            
            emit playStateChanged(playing);
        }
    }
    
    // IMPORTANT: Check for loop status changes every timer tick (not just every 50 ticks)
    // This ensures loop visualization updates immediately when PerformancePads trigger loops
    bool currentLoopEnabled = player->isLoopEnabled();
    double currentLoopStart = player->getLoopStart();
    double currentLoopEnd = player->getLoopEnd();
    
    std::cout << "QtDeckWidget::syncPlayState - Loop status check: enabled=" << currentLoopEnabled 
              << ", lastEnabled=" << lastLoopEnabled << ", start=" << currentLoopStart 
              << ", end=" << currentLoopEnd << std::endl;
    
    if (currentLoopEnabled != lastLoopEnabled || 
        currentLoopStart != lastLoopStart || 
        currentLoopEnd != lastLoopEnd) {
        
        std::cout << "QtDeckWidget::syncPlayState - LOOP STATUS CHANGED! Updating waveforms..." << std::endl;
        
        // Update the waveform displays
        waveform->setLoopRegion(currentLoopEnabled, currentLoopStart, currentLoopEnd);
        
        // Emit signal so main waveform displays can also update
        std::cout << "QtDeckWidget::syncPlayState - Emitting loopChanged signal..." << std::endl;
        emit loopChanged(currentLoopEnabled, currentLoopStart, currentLoopEnd);
        
        // Store current values for next comparison
        lastLoopEnabled = currentLoopEnabled;
        lastLoopStart = currentLoopStart;
        lastLoopEnd = currentLoopEnd;
        
        std::cout << "QtDeckWidget::syncPlayState - Loop status update complete" << std::endl;
    }
}

void QtDeckWidget::onKeylockToggle() {
    if (!player) return;
    
    bool enabled = keylockBtn->isChecked();
    player->setKeylockEnabled(enabled);
    
    // Update button appearance
    if (enabled) {
        keylockBtn->setText("KEY ✓");
    } else {
        keylockBtn->setText("Key");
    }
}

void QtDeckWidget::onQuantizeToggle() {
    if (!player) return;
    
    bool enabled = quantizeBtn->isChecked();
    player->setQuantizeEnabled(enabled);
    
    // Update button appearance
    if (enabled) {
        quantizeBtn->setText("Q ✓");
    } else {
        quantizeBtn->setText("Q");
    }
}

void QtDeckWidget::setBeatIndicator(BeatIndicator* indicator) {
    if (pads) {
        pads->setBeatIndicator(indicator);
    }
}
