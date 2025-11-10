#include "DeckWidget.h"
#include "DJAudioPlayer.h"
#include "WaveformGenerator.h"
#include "ScratchEngine.h"
#include "FrameTiming.h"
#include "WaveformTheme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QTimer>
#include <QMimeData>
#include <algorithm>
#include <cmath>
#include <ranges>
#include <span>
#include <utility>
#include <QRunnable>
#include <QThreadPool>
#include <QThread>
#include <QPointer>
#include <chrono>
#include <iostream>
#include <QAction>
#include <QPoint>

namespace {
    using namespace std::chrono_literals;

    constexpr auto kStatusTimerInterval = 100ms;
    constexpr auto kPlayToggleDebounce = 50ms;
    constexpr auto kCueDoubleClickWindow = 300ms;
    constexpr auto kPlayStateGuardWindow = 500ms;
    constexpr int kSmallOverviewBinCount = 4000;
    constexpr int kPlayStateSyncStride = 50;
    constexpr double kPrerollRelativePosition = -0.5;
}

QtDeckWidget::QtDeckWidget(DJAudioPlayer* player_, QWidget* parent, const QString& deckTitle, bool isLeftDeck)
    : QWidget(parent), player(player_)
{
    qDebug() << "QtDeckWidget constructor called for" << deckTitle << "isLeftDeck:" << isLeftDeck;
    std::cout << "=== QtDeckWidget constructor for " << deckTitle.toStdString() << " ===" << std::endl;
    waveform = new DeckWaveformOverview(this);

    statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, &QtDeckWidget::syncPlayState);
    statusTimer->start(static_cast<int>(kStatusTimerInterval.count()));

    cueClickTimer = new QTimer(this);
    cueClickTimer->setSingleShot(true);
    connect(cueClickTimer, &QTimer::timeout, this, [this]() { cueClickPending = false; });
    
    controlsWidget = new QWidget(this);
    controlsWidget->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333;");
    
    deckTitleLabel = new QLabel(deckTitle, controlsWidget);
    deckTitleLabel->setAlignment(Qt::AlignCenter);
    deckTitleLabel->setStyleSheet("font-weight: bold; font-size: 18px; color: #fff; padding: 10px;");
    
    songNameLabel = new QLabel("No Track Loaded", controlsWidget);
    songNameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    songNameLabel->setStyleSheet("font-size: 12px; color: #f6f8fb; padding: 0px;");
    songNameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    deckTitleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    deckTitleLabel->setStyleSheet("color: #4fb0ff; font-weight: bold; font-size: 10px; padding: 0px 6px 0px 0px;");
    deckTitleLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    trackInfoLabel = new QLabel("No track loaded", controlsWidget);
    trackInfoLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    trackInfoLabel->setStyleSheet("font-size: 11px; color: #b8bfd0; padding: 0px;");
    trackInfoLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    coverArtLabel = new QLabel(controlsWidget);
    coverArtLabel->setFixedSize(60, 60);  // Square that spans both rows
    coverArtLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #444; border-radius: 2px;");
    coverArtLabel->setAlignment(Qt::AlignCenter);
    coverArtLabel->setText("🎵");
    coverArtLabel->setToolTip("Album Cover Art");
    coverArtLabel->setScaledContents(false);  // Don't stretch - we'll scale manually
    
    coverArtLabelWave = nullptr;

    turntable = new QtTurntableWidget(controlsWidget);
    playPauseBtn = new QPushButton("Play", controlsWidget);
    loadBtn = new QPushButton("Load", controlsWidget);
    unloadBtn = new QPushButton("Unload", controlsWidget);
    cueBtn = new QPushButton("Cue", controlsWidget);
    keylockBtn = new QPushButton("Key", controlsWidget);
    quantizeBtn = new QPushButton("Q", controlsWidget);
    syncBtn = new QPushButton("Sync", controlsWidget);
    tempoRangeBtn = new QPushButton("±16%", controlsWidget);
    speedSlider = new QSlider(Qt::Vertical, controlsWidget);
    tempoValueLabel = new QLabel("1.000x", controlsWidget);
    tempoSpin = new QDoubleSpinBox(controlsWidget);
    bpmDefaultLabel = new QLabel("BPM: --", controlsWidget);
    bpmCurrentLabel = new QLabel("Curr: --", controlsWidget);
    speedLabel = new QLabel("Speed", controlsWidget);

    playPauseBtn->setStyleSheet("QPushButton { background-color: #0066cc; color: white; border: none; padding: 4px; font-weight: bold; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #0052a3; }");
    loadBtn->setStyleSheet("QPushButton { background-color: #666; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #777; }");
    cueBtn->setStyleSheet("QPushButton { background-color: #ff6600; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #e55a00; }");
    keylockBtn->setStyleSheet("QPushButton { background-color: #333; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #444; } QPushButton:checked { background-color: #00cc66; }");
    quantizeBtn->setStyleSheet("QPushButton { background-color: #333; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #444; } QPushButton:checked { background-color: #cc6600; }");
    unloadBtn->setStyleSheet("QPushButton { background-color: #555; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #666; }");
    tempoRangeBtn->setStyleSheet("QPushButton { background-color: #333; color: #ddd; border: none; padding: 2px 6px; border-radius: 0px; font-size: 9px; } QPushButton:hover { background-color: #444; }");
    tempoRangeBtn->setFixedSize(42, 18); // keep size constant regardless of label
    syncBtn->setStyleSheet("QPushButton { background-color: #008844; color: white; border: none; padding: 4px; border-radius: 0px; font-size: 10px; } QPushButton:hover { background-color: #00733a; } QPushButton:checked { background-color: #00aa55; }");
    
    keylockBtn->setCheckable(true);
    quantizeBtn->setCheckable(true);
    
    keylockBtn->setToolTip("Keylock - maintains original pitch when speed changes");
    quantizeBtn->setToolTip("Quantize - snaps cues and loops to nearest beat");
    syncBtn->setToolTip("Sync tempo & phase to the other deck");
    
    speedSlider->setRange(840, 1160);     // store factor*1000
    speedSlider->setSingleStep(1);        // 0.001 per step
    speedSlider->setPageStep(5);          // 0.005 per page
    speedSlider->setTracking(true);       // continuous updates
    speedSlider->setInvertedAppearance(true); // Down = faster, Up = slower
    speedSlider->setInvertedControls(true);   // Arrow/Page keys match inverted appearance
    speedSlider->setValue(1000);          // 1.000x default
    // High precision spin (4 decimals)
    tempoSpin->setDecimals(4);
    tempoSpin->setRange(0.8400, 1.1600);
    tempoSpin->setSingleStep(0.0005);
    tempoSpin->setValue(1.0000);
    tempoSpin->setKeyboardTracking(false);

    playPauseBtn->setEnabled(false);
    
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
    connect(unloadBtn, &QPushButton::clicked, this, &QtDeckWidget::onUnload);
    syncBtn->setCheckable(true);
    connect(syncBtn, &QPushButton::clicked, this, &QtDeckWidget::onSync);          // immediate one-shot sync
    connect(syncBtn, &QPushButton::toggled, this, &QtDeckWidget::onSyncToggled);   // follow mode on/off
    connect(speedSlider, &QSlider::valueChanged, this, &QtDeckWidget::onSpeedChanged);
    // Two-way bind spin <-> slider
    connect(tempoSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &QtDeckWidget::onTempoSpinChanged);
    // Double-click reset on slider
    speedSlider->installEventFilter(this);
    // Tempo range: cycle on click (no dropdown)
    connect(tempoRangeBtn, &QPushButton::clicked, this, [this]() {
        tempoRangeIndex = (tempoRangeIndex + 1) % 4;
        switch (tempoRangeIndex) {
            case 0: setTempoRangePm6(); break;
            case 1: setTempoRangePm8(); break;
            case 2: setTempoRangePm16(); break;
            case 3: setTempoRangeWide(); break;
        }
    });
    
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
    
    auto topSection = new QHBoxLayout;
    topSection->setSpacing(4);
    topSection->setContentsMargins(0, 0, 0, 0);
    
    topSection->addWidget(coverArtLabel, 0, Qt::AlignTop);
    
    auto rightStack = new QVBoxLayout;
    rightStack->setSpacing(2);
    rightStack->setContentsMargins(0, 0, 0, 0);
    
    auto headerRow = new QWidget(controlsWidget);
    headerRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto headerRowLayout = new QHBoxLayout(headerRow);
    headerRowLayout->setContentsMargins(4, 4, 4, 4);
    headerRowLayout->setSpacing(8);
    headerRowLayout->addWidget(deckTitleLabel);
    headerRowLayout->addWidget(songNameLabel, 1);
    headerRowLayout->addWidget(trackInfoLabel, 0);
    rightStack->addWidget(headerRow);
    
    waveform->setFixedHeight(25);
    waveform->setStyleSheet("border: 1px solid #444; border-radius: 0px;");
    rightStack->addWidget(waveform);
    
    topSection->addLayout(rightStack, 1);
    controlsLayout->addLayout(topSection);
    waveform->setAcceptDrops(true);
    connect(waveform, &DeckWaveformOverview::fileDropped, this, [this](const QString &path){ this->loadFile(path); });
    
    auto mainControlsLayout = new QHBoxLayout;
    mainControlsLayout->setSpacing(6);
    
    PerformancePads::DeckId deckId = isLeftDeck ? PerformancePads::DeckId::A : PerformancePads::DeckId::B;
    qDebug() << "QtDeckWidget: About to create PerformancePads for deck" << (deckId == PerformancePads::DeckId::A ? "A" : "B");
    std::cout << "=== About to create PerformancePads ===" << std::endl;
    pads = new PerformancePads(deckId, controlsWidget);
    qDebug() << "QtDeckWidget: PerformancePads created successfully";
    std::cout << "=== PerformancePads created successfully ===" << std::endl;
    pads->setAudioPlayer(player);
    pads->setMaximumHeight(120);
    pads->setMaximumWidth(380);
    
    connect(pads, &PerformancePads::cuePointsChanged, waveform, &DeckWaveformOverview::setCuePoints);
    
    auto turntableSection = new QVBoxLayout;
    turntableSection->setSpacing(3);
    
    auto transportLayout = new QHBoxLayout;
    transportLayout->setSpacing(2);  // Reduced spacing for smaller buttons
    playPauseBtn->setFixedHeight(20);  // Smaller height
    loadBtn->setFixedHeight(20);       // Smaller height
    unloadBtn->setFixedHeight(20);     // Smaller height
    cueBtn->setFixedHeight(20);        // Smaller height
    keylockBtn->setFixedHeight(20);    // Smaller height
    quantizeBtn->setFixedHeight(20);   // Smaller height
    playPauseBtn->setFixedWidth(40);   // Smaller width
    loadBtn->setFixedWidth(40);        // Smaller width
    unloadBtn->setFixedWidth(50);      // Slightly wider
    cueBtn->setFixedWidth(30);         // Smaller width for Cue
    keylockBtn->setFixedWidth(30);     // Smaller width for Key
    quantizeBtn->setFixedWidth(25);    // Smaller width for Q
    transportLayout->addWidget(playPauseBtn);
    transportLayout->addWidget(loadBtn);
    transportLayout->addWidget(unloadBtn);
    transportLayout->addWidget(cueBtn);
    transportLayout->addWidget(keylockBtn);
    transportLayout->addWidget(quantizeBtn);
    transportLayout->addWidget(syncBtn);
    transportLayout->addWidget(tempoRangeBtn);
    turntableSection->addLayout(transportLayout);
    
    turntable->setFixedSize(90, 90);

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
    
    if (isLeftDeck) {
        mainControlsLayout->addWidget(pads, 2, Qt::AlignTop);
        mainControlsLayout->addLayout(turntableSection, 1);
    } else {
        mainControlsLayout->addLayout(turntableSection, 1);
        mainControlsLayout->addWidget(pads, 2, Qt::AlignTop);
    }
    
    controlsLayout->addLayout(mainControlsLayout);
    controlsWidget->setLayout(controlsLayout);

    controlsWidget->setAcceptDrops(true);
    controlsWidget->installEventFilter(this);

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(controlsWidget);
    setLayout(mainLayout);

    setAcceptDrops(true);

    setTempoRangePm16(); // also sets tempoRangeIndex to 2 implicitly
    if (unloadBtn) unloadBtn->setEnabled(false);

    QTimer* t = new QTimer(this);
    t->setTimerType(Qt::PreciseTimer);
    t->setInterval(FrameTiming::kFrameIntervalMs);
    connect(t, &QTimer::timeout, [this]() {
        if (!player) return;
        // Always reflect current position, even when paused, to keep displays in sync
    static double lastPos = -1.0;
    double curSec = player->getCurrentPositionSeconds();
    double relative = player->getPositionRelative();
    waveform->setPlayhead(relative);

        // Update turntable with absolute seconds and BPM for beat synchronization
        double trackLengthSec = player->getLengthInSeconds();
        if (trackLengthSec > 0.0) {
            turntable->setTrackLength(trackLengthSec);
        }
        turntable->setPositionSeconds(curSec); // supports preroll negative seconds
        if (detectedBpm > 0.0) {
            turntable->setBpm(detectedBpm);
        }

        if (scratchEngine) {
            ScratchEngine::TrackConfig config;
            config.lengthSeconds = trackLengthSec;
            config.prerollSeconds = turntable ? turntable->getPrerollSeconds() : 8.0;
            scratchEngine->setTrackConfig(config);
            if (!scratchEngine->isScratching()) {
                scratchEngine->syncExternalPosition(curSec);
            }
        }

        // Emit only when the position changed to minimize redundant updates
        if (std::abs(relative - lastPos) > 1e-6) {
            emit playheadUpdated(relative);
            lastPos = relative;
        }
    });
    t->start();
}

void QtDeckWidget::loadFile(const QString &path) {
    const auto generation = waveformTaskGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    currentFilePath = path;  // Store the current file path
    if (pads) {
        pads->clearAllCuePoints(false);
    }
    if (waveform) {
        waveform->clearCuePoints();
    }
    cueClickPending = false;
    if (cueClickTimer) {
        cueClickTimer->stop();
    }
    isCueing = false;
    cuePosition = 0.0;
    lastLoopEnabled = false;
    lastLoopStart = -1.0;
    lastLoopEnd = -1.0;
    if (waveform) {
        waveform->setLoopRegion(false, 0.0, 0.0);
    }
    emit loopChanged(false, 0.0, 0.0);

    detectedBpm = 0.0;
    emit displayedBpmChanged(0.0);
    if (bpmDefaultLabel) {
        bpmDefaultLabel->setText("BPM: --");
    }
    if (bpmCurrentLabel) {
        bpmCurrentLabel->setText("Curr: --");
    }

    if (path.isEmpty()) {
        resetDeckUiToEmptyState();
        return;
    }

    class SmallOverviewTask : public QRunnable {
    public:
        SmallOverviewTask(QPointer<QtDeckWidget> deckPtr, QString p, std::uint64_t generation)
            : deck(std::move(deckPtr)), filePath(std::move(p)), generation(generation) {
            setAutoDelete(true);
        }
            void run() override {
            if (!deck) {
                return;
            }
            const auto currentGeneration = generation;
                try {
                    QThread::currentThread()->setPriority(QThread::LowestPriority);
                    WaveformGenerator gen;
                    WaveformGenerator::Result res;
                if (!gen.generate(juce::File(filePath.toStdString()), kSmallOverviewBinCount, res)) {
                    return;
                }

                if (!deck || !deck->isWaveformGenerationCurrent(currentGeneration)) {
                    return;
                }

                const auto binCount = res.maxBins.size();
                if (binCount == 0) {
                    return;
                }

                auto amplitudes = std::make_shared<std::vector<float>>(binCount);
                auto colours = std::make_shared<std::vector<float>>(binCount * 3);
                const auto fallback = WaveformTheme::fallbackColor();
                auto* colourData = colours->data();

                for (auto&& [index, amplitudeSlot] : std::views::enumerate(*amplitudes)) {
                    const float maxVal = res.maxBins[index];
                    const float minVal = index < res.minBins.size() ? res.minBins[index] : 0.0f;
                    const float amplitude = std::clamp(
                        WaveformTheme::computeColumnAmplitude(minVal, maxVal),
                        0.0f,
                        1.0f);
                    amplitudeSlot = amplitude;

                    const float low  = index < res.lowBins.size()  ? res.lowBins[index]  : amplitude;
                    const float mid  = index < res.midBins.size()  ? res.midBins[index]  : amplitude;
                    const float high = index < res.highBins.size() ? res.highBins[index] : amplitude;

                    auto rgb = WaveformTheme::computeSpectrumColor(low, mid, high, 1);
                    if (!std::isfinite(rgb.r) || !std::isfinite(rgb.g) || !std::isfinite(rgb.b)) {
                        rgb = fallback;
                    }

                    const auto base = index * 3;
                    colourData[base + 0] = rgb.r;
                    colourData[base + 1] = rgb.g;
                    colourData[base + 2] = rgb.b;
                }

                const auto audioStart = res.audioStartOffsetSec;
                const auto lengthSec = res.lengthSeconds;
                auto callback = [deck = deck,
                                 currentGeneration,
                                 amplitudes = std::move(amplitudes),
                                 colours = std::move(colours),
                                 audioStart,
                                 lengthSec]() mutable {
                    if (deck) {
                        deck->handleOverviewWaveformResult(
                            currentGeneration,
                            std::move(amplitudes),
                            std::move(colours),
                            audioStart,
                            lengthSec);
                    }
                };
                if (!deck || !deck->isWaveformGenerationCurrent(currentGeneration)) {
                    return;
                }
                QMetaObject::invokeMethod(deck, std::move(callback), Qt::QueuedConnection);
                } catch (...) {}
            }
    private:
        QPointer<QtDeckWidget> deck;
        QString filePath;
        std::uint64_t generation;
    };
    QThreadPool::globalInstance()->start(new SmallOverviewTask(QPointer<QtDeckWidget>(this), path, generation));
    QFileInfo fi(path);
    QString baseName = fi.completeBaseName();
    if (baseName.isEmpty())
        baseName = fi.fileName();
    setTrackNameDisplay(baseName, fi.fileName());
    setTrackInfoDisplay("Loading…", "font-size: 11px; color: #4fb0ff; padding: 2px;", QStringLiteral("Preparing analysis"));
    if (player) {
        emit fileLoadingStarted(path);  // Signal to start background loading

        if (playPauseBtn) {
            playPauseBtn->setEnabled(false);
            playPauseBtn->setText("Play");
        }
        loadBtn->setText("Loading...");
        loadBtn->setEnabled(false);
        
    const double prerollPosition = kPrerollRelativePosition; // -0.5 in relative coordinates = -4 seconds in preroll
        waveform->setPlayhead(prerollPosition);
        playing = false;
        turntable->stop();
        
        cuePosition = 0.0;
        isCueing = false;
        
        if (player) {
            player->setPositionRelative(prerollPosition); // Use relative positioning for preroll
            std::cout << "QtDeckWidget: New track positioned in preroll at " << prerollPosition 
                      << " relative (-4 seconds)" << std::endl;
        }
    }
    // do NOT start turntable here
}

void QtDeckWidget::setTrackNameDisplay(const QString& text, const QString& tooltip) {
    if (!songNameLabel)
        return;

    const QString tip = tooltip.isEmpty() ? text : tooltip;
    songNameLabel->setText(text);
    songNameLabel->setToolTip(tip);
}

void QtDeckWidget::setTrackInfoDisplay(const QString& text, const QString& style, const QString& tooltip) {
    if (!trackInfoLabel)
        return;

    static const QString defaultStyle = QStringLiteral("font-size: 11px; color: #b8bfd0; padding: 0px;");
    trackInfoLabel->setText(text);
    trackInfoLabel->setToolTip(tooltip.isEmpty() ? text : tooltip);
    trackInfoLabel->setStyleSheet(style.isEmpty() ? defaultStyle : style);
}

void QtDeckWidget::setCoverArt(const QByteArray& imageData, const QString& format) {
    if (!coverArtLabel) return;
    
    if (imageData.isEmpty()) {
        // Reset to placeholder
        coverArtLabel->clear();
        coverArtLabel->setText("🎵");
        coverArtLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #444; border-radius: 2px; color: #666; font-size: 24px;");
        return;
    }
    
    // Load image from data
    QPixmap pixmap;
    if (pixmap.loadFromData(imageData, format.isEmpty() ? nullptr : format.toUtf8().constData())) {
        // Scale to fit label size while maintaining aspect ratio
        QPixmap scaled = pixmap.scaled(coverArtLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        coverArtLabel->setPixmap(scaled);
        coverArtLabel->setText("");  // Clear emoji when showing image
        // Update stylesheet to remove background when showing image
        coverArtLabel->setStyleSheet("border: 1px solid #444; border-radius: 2px; background-color: #0a0a0a;");
        std::cout << "QtDeckWidget: Cover art loaded successfully (" << format.toStdString() << ", " 
                  << pixmap.width() << "x" << pixmap.height() << ")" << std::endl;
    } else {
        // Failed to load - keep placeholder
        std::cerr << "QtDeckWidget: Failed to load cover art image (format: " << format.toStdString() 
                  << ", size: " << imageData.size() << " bytes)" << std::endl;
        coverArtLabel->clear();
        coverArtLabel->setText("❌");
        coverArtLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #444; border-radius: 2px; color: #ff0000; font-size: 24px;");
    }
}

void QtDeckWidget::onFileLoadingComplete(const QString& filePath) {
    qDebug() << "QtDeckWidget::onFileLoadingComplete called for" << filePath 
             << "current:" << currentFilePath << "player:" << (player != nullptr);
    
    if (player && currentFilePath == filePath) {
        // Re-enable controls after loading is complete
        playPauseBtn->setText("Play");
        playPauseBtn->setEnabled(true);
        loadBtn->setText("Load");
        loadBtn->setEnabled(true);
        if (unloadBtn) unloadBtn->setEnabled(true);
        
        qDebug() << "QtDeckWidget::onFileLoadingComplete - emitting fileLoaded() signal";
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
    
    // Debounce rapid toggles to avoid duplicate transport requests.
    using Clock = std::chrono::steady_clock;
    static auto lastClickTime = Clock::now();
    const auto now = Clock::now();
    if (now - lastClickTime < kPlayToggleDebounce) {
        std::cout << "  DEBOUNCE: Ignoring rapid click" << std::endl;
        return;
    }
    lastClickTime = now;
    
    if (currentFilePath.isEmpty()) {
        std::cout << "  No file loaded - ignoring play request" << std::endl;
        return;
    }
    
    // Get current state and immediately update UI for instant responsiveness
    bool wasPlaying = playing;
    std::cout << "  Was playing: " << wasPlaying << std::endl;
    
    if (wasPlaying) {
        std::cout << "  STOPPING playback..." << std::endl;
        playPauseBtn->setText("Play");
        turntable->stop();
        playing = false;
        emit playStateChanged(playing);
        if (player) {
            std::cout << "  Calling player->stop()" << std::endl;
            player->stop();
        }
        std::cout << "  STOP sequence completed" << std::endl;
    } else {
        std::cout << "  STARTING playback..." << std::endl;
        lastPlayPressTime = Clock::now();
        
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
        onUnload();
    }
}

void QtDeckWidget::onUnload() {
    waveformTaskGeneration.fetch_add(1, std::memory_order_acq_rel);
    // Unload current file
    currentFilePath.clear();
    if (player) {
        player->stop();
        player->unload();
        player->setSpeed(1.0);
        player->disableLoop();
        player->setQuantizeEnabled(false);
        player->setKeylockEnabled(false);
    }
    resetDeckUiToEmptyState();
    emit playStateChanged(false);
    emit displayedBpmChanged(0.0);
    emit tempoFactorChanged(1.0);
    emit loopChanged(false, 0.0, 0.0);
    emit playheadUpdated(0.0);
    emit fileUnloaded();
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
    cueClickTimer->start(static_cast<int>(kCueDoubleClickWindow.count()));
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
    // clamp to UI limits (dynamic range)
    double clamped = std::clamp(factor, minTempoFactor, maxTempoFactor);
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

void QtDeckWidget::onTempoRangeSelected() {}

void QtDeckWidget::setTempoRangePm6() {
    minTempoFactor = 1.0 - 0.06;
    maxTempoFactor = 1.0 + 0.06;
    if (tempoRangeBtn) tempoRangeBtn->setText("±6%");
    updateTempoControlsForRange();
    tempoRangeIndex = 0;
}

void QtDeckWidget::setTempoRangePm8() {
    minTempoFactor = 1.0 - 0.08;
    maxTempoFactor = 1.0 + 0.08;
    if (tempoRangeBtn) tempoRangeBtn->setText("±8%");
    updateTempoControlsForRange();
    tempoRangeIndex = 1;
}

void QtDeckWidget::setTempoRangePm16() {
    minTempoFactor = 1.0 - 0.16;
    maxTempoFactor = 1.0 + 0.16;
    if (tempoRangeBtn) tempoRangeBtn->setText("±16%");
    updateTempoControlsForRange();
    tempoRangeIndex = 2;
}

void QtDeckWidget::setTempoRangeWide() {
    // Choose a sensible wide range
    minTempoFactor = 0.5;
    maxTempoFactor = 1.5;
    if (tempoRangeBtn) tempoRangeBtn->setText("WIDE");
    updateTempoControlsForRange();
    tempoRangeIndex = 3;
}

void QtDeckWidget::updateTempoControlsForRange() {
    if (!speedSlider || !tempoSpin) return;
    int minS = (int)std::round(minTempoFactor * 1000.0);
    int maxS = (int)std::round(maxTempoFactor * 1000.0);
    speedSlider->blockSignals(true);
    speedSlider->setRange(minS, maxS);
    speedSlider->blockSignals(false);
    tempoSpin->blockSignals(true);
    tempoSpin->setRange(minTempoFactor, maxTempoFactor);
    tempoSpin->blockSignals(false);
    // Ensure current factor is clamped and UI synced
    double current = std::clamp(getTempoFactor(), minTempoFactor, maxTempoFactor);
    applyTempo(current);
}

bool QtDeckWidget::isWaveformGenerationCurrent(std::uint64_t generation) const noexcept {
    return waveformTaskGeneration.load(std::memory_order_acquire) == generation;
}

void QtDeckWidget::handleOverviewWaveformResult(
    std::uint64_t generation,
    std::shared_ptr<std::vector<float>> amplitudes,
    std::shared_ptr<std::vector<float>> colours,
    double audioStart,
    double lengthSec) {
    if (!isWaveformGenerationCurrent(generation) || !waveform || !amplitudes || !colours) {
        return;
    }

    waveform->setWaveformData(*amplitudes, *colours, audioStart, lengthSec);
}

void QtDeckWidget::resetDeckUiToEmptyState() {
    cueClickPending = false;
    if (cueClickTimer) {
        cueClickTimer->stop();
    }
    isCueing = false;
    cuePosition = 0.0;
    playing = false;

    if (playPauseBtn) {
        playPauseBtn->setText("Play");
        playPauseBtn->setEnabled(false);
    }
    if (keylockBtn) {
        keylockBtn->setChecked(false);
        keylockBtn->setText("Key");
    }
    if (quantizeBtn) {
        quantizeBtn->setChecked(false);
        quantizeBtn->setText("Q");
    }
    if (syncBtn) {
        syncBtn->setChecked(false);
    }
    if (loadBtn) {
        loadBtn->setText("Load");
        loadBtn->setEnabled(true);
    }
    if (unloadBtn) {
        unloadBtn->setEnabled(false);
    }

    if (tempoValueLabel) {
        tempoValueLabel->setText("1.000x");
    }
    if (speedSlider) {
        speedSlider->blockSignals(true);
        speedSlider->setValue(1000);
        speedSlider->blockSignals(false);
    }
    if (tempoSpin) {
        tempoSpin->blockSignals(true);
        tempoSpin->setValue(1.0);
        tempoSpin->blockSignals(false);
    }

    if (songNameLabel) {
        songNameLabel->setText("No Track Loaded");
        songNameLabel->setToolTip("No Track Loaded");
    }
    if (trackInfoLabel) {
        trackInfoLabel->setText("No track loaded");
        trackInfoLabel->setToolTip("No track loaded");
        trackInfoLabel->setStyleSheet("font-size: 11px; color: #b8bfd0; padding: 0px;");
    }
    if (coverArtLabel) {
        coverArtLabel->clear();
        coverArtLabel->setText("🎵");
        coverArtLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #444; border-radius: 2px; color: #666; font-size: 24px;");
    }

    detectedBpm = 0.0;
    if (bpmDefaultLabel) {
        bpmDefaultLabel->setText("BPM: --");
    }
    if (bpmCurrentLabel) {
        bpmCurrentLabel->setText("Curr: --");
    }

    lastLoopEnabled = false;
    lastLoopStart = -1.0;
    lastLoopEnd = -1.0;

    if (waveform) {
        waveform->clearDisplay();
        waveform->clearCuePoints();
        waveform->setPlayhead(0.0);
        waveform->setLoopRegion(false, 0.0, 0.0);
    }
    if (pads) {
        pads->clearAllCuePoints(false);
    }
    if (turntable) {
        turntable->stop();
        turntable->setSpeed(1.0);
        turntable->setTrackLength(0.0);
        turntable->setPositionSeconds(0.0);
        turntable->setBpm(0.0);
    }
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
    if (!player) return;

    const bool hasTrack = !currentFilePath.isEmpty();

    if (!hasTrack) {
        if (playPauseBtn && playPauseBtn->isEnabled()) {
            resetDeckUiToEmptyState();
        }
        if (playPauseBtn) {
            playPauseBtn->setEnabled(false);
            if (playPauseBtn->text() != "Play") {
                playPauseBtn->setText("Play");
            }
        }
        if (playing) {
            playing = false;
            turntable->stop();
            emit playStateChanged(playing);
        }
        return;
    }

    if (playPauseBtn && !playPauseBtn->isEnabled() && !playing) {
        return;
    }

    // Throttle expensive transport sync to a manageable cadence.
    static int updateCounter = 0;
    const bool shouldUpdatePlayState = (++updateCounter >= kPlayStateSyncStride);
    if (shouldUpdatePlayState) {
        updateCounter = 0;
    }

    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const bool hasPlayTimestamp = lastPlayPressTime != Clock::time_point{};
    const bool recentPlayPress = hasPlayTimestamp && (now - lastPlayPressTime) < kPlayStateGuardWindow;
    
    if (shouldUpdatePlayState && !recentPlayPress) {
        bool actuallyPlaying = player->isPlaying();
        bool uiShowsPlaying = (playPauseBtn->text() == "Pause");
        
        // Only update UI if there's a mismatch, don't call any transport actions
        if (actuallyPlaying != uiShowsPlaying) {
            playPauseBtn->setText(actuallyPlaying ? "Pause" : "Play");
        }
        
        if (actuallyPlaying != playing) {
            playing = actuallyPlaying;

            if (playing) {
                playPauseBtn->setText("Pause");
                turntable->start();
            } else {
                playPauseBtn->setText("Play");
                turntable->stop();
            }

            emit playStateChanged(playing);
        }
    }
    
    // Check loop status each tick to keep the UI responsive to external changes
    bool currentLoopEnabled = player->isLoopEnabled();
    double currentLoopStart = player->getLoopStart();
    double currentLoopEnd = player->getLoopEnd();
    
    if (currentLoopEnabled != lastLoopEnabled || 
        currentLoopStart != lastLoopStart || 
        currentLoopEnd != lastLoopEnd) {
        // Update the waveform displays
        waveform->setLoopRegion(currentLoopEnabled, currentLoopStart, currentLoopEnd);
        
        // Emit signal so main waveform displays can also update
        emit loopChanged(currentLoopEnabled, currentLoopStart, currentLoopEnd);
        
        // Store current values for next comparison
        lastLoopEnabled = currentLoopEnabled;
        lastLoopStart = currentLoopStart;
        lastLoopEnd = currentLoopEnd;
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

void QtDeckWidget::detachPlayer() {
    // Stop any outstanding timers to prevent queued timeouts from running during teardown.
    const auto timers = findChildren<QTimer*>();
    for (auto* timer : timers) {
        if (timer) {
            timer->stop();
        }
    }

    if (pads) {
        pads->setAudioPlayer(nullptr);
    }

    player = nullptr;
    playing = false;
}

void QtDeckWidget::setScratchEngine(ScratchEngine* engine) {
    scratchEngine = engine;
    if (turntable) {
        turntable->setScratchEngine(engine);
    }
}
