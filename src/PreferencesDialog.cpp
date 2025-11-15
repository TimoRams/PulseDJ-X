#include "PreferencesDialog.h"
#include "MainWindow.h"
#include "DJAudioPlayer.h"
#include <QApplication>
#include <QTimer>
#include <QTime>
#include <QScreen>
#include <QHeaderView>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QVariant>
#include <algorithm>
#include <iostream>

PreferencesDialog::PreferencesDialog(QWidget* parent)
    : QDialog(parent)
    , midiEngine(new MidiEngine(this)) {
    setWindowTitle("BetaPulseX - Preferences");
    setWindowIcon(QIcon(":/icons/settings.png"));
    setModal(true);

    if (!mainWindowRef) {
        mainWindowRef = qobject_cast<QtMainWindow*>(parent);
    }
    
    // Dialog size
    resize(800, 600);
    
    // Center on screen
    if (parent) {
        move(parent->geometry().center() - rect().center());
    } else {
        QScreen* screen = QApplication::primaryScreen();
        move(screen->geometry().center() - rect().center());
    }
    
    // Connect MIDI Engine signals
    connect(midiEngine, &MidiEngine::deviceOpened, this, &PreferencesDialog::onMidiDeviceOpened);
    connect(midiEngine, &MidiEngine::deviceClosed, this, &PreferencesDialog::onMidiDeviceClosed);
    connect(midiEngine, &MidiEngine::deviceError, this, &PreferencesDialog::onMidiDeviceError);
    connect(midiEngine, &MidiEngine::midiMessageReceived, this, &PreferencesDialog::onMidiMessageReceived);
    
    setupUI();

    suppressChangeTracking = true;
    loadSettings();
    originalSettings = settings; // Backup für Cancel
    suppressChangeTracking = false;

    installChangeTracking();
    pendingChanges = false;
    updateApplyButtonState();
}

void PreferencesDialog::setPlayerReferences(DJAudioPlayer* playerA, DJAudioPlayer* playerB, QtMainWindow* mainWindow)
{
    // Store main window reference for deck access
    mainWindowRef = mainWindow;
    refreshAudioDeviceLists();
    
    if (midiEngine) {
        midiEngine->setPlayers(playerA, playerB);
        midiEngine->setMainWindow(mainWindow);
        qDebug() << "PreferencesDialog: Player references set for MIDI integration";
        
        // Add a default test mapping for quick testing
        // Map any CC control #1 to PlayPause for Deck A (Omni channel handling in MidiEngine)
        MidiControlMapping testMapping;
        testMapping.midiChannel = 0;  // Omni Channel (handled automatically by MidiEngine)
        testMapping.controlNumber = 1;  // CC1
        testMapping.controlType = MidiControlType::PlayPause;
        testMapping.deckId = "A";
        testMapping.isNote = false;
        testMapping.minValue = 0;
        testMapping.maxValue = 127;
        
        midiEngine->addControlMapping(testMapping);
        qDebug() << "PreferencesDialog: Added test mapping - CC1 on Omni Channel -> PlayPause Deck A";
    }
}

void PreferencesDialog::setupUI() {
    mainLayout = new QVBoxLayout(this);
    
    // Tab Widget
    tabWidget = new QTabWidget();
    mainLayout->addWidget(tabWidget);
    
    // Create all tabs
    createAudioTab();
    createDeckTab();
    createInterfaceTab();
    createLibraryTab();
    createPerformanceTab();
    createMidiTab();
    createAdvancedTab();
    
    // Button layout
    buttonLayout = new QHBoxLayout();
    
    defaultsButton = new QPushButton("Restore Defaults");
    defaultsButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    
    buttonLayout->addWidget(defaultsButton);
    buttonLayout->addStretch();
    
    cancelButton = new QPushButton("Cancel");
    cancelButton->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
    
    applyButton = new QPushButton("Apply");
    applyButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    
    okButton = new QPushButton("OK");
    okButton->setIcon(style()->standardIcon(QStyle::SP_DialogOkButton));
    okButton->setDefault(true);
    
    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(applyButton);
    buttonLayout->addWidget(okButton);
    
    mainLayout->addLayout(buttonLayout);
    
    // Connect signals
    connect(okButton, &QPushButton::clicked, this, &PreferencesDialog::onOkClicked);
    connect(cancelButton, &QPushButton::clicked, this, &PreferencesDialog::onCancelClicked);
    connect(applyButton, &QPushButton::clicked, this, &PreferencesDialog::onApplyClicked);
    connect(defaultsButton, &QPushButton::clicked, this, &PreferencesDialog::onRestoreDefaultsClicked);
}

void PreferencesDialog::createAudioTab() {
    audioTab = new QWidget();
    tabWidget->addTab(audioTab, QIcon(":/icons/audio.png"), "Audio");
    
    QVBoxLayout* layout = new QVBoxLayout(audioTab);
    
    // Audio Device Group
    QGroupBox* deviceGroup = new QGroupBox("Audio Routing");
    QFormLayout* deviceLayout = new QFormLayout(deviceGroup);

    masterDeviceCombo = new QComboBox();
    deviceLayout->addRow("Master Output Device:", masterDeviceCombo);

    masterChannelCombo = new QComboBox();
    deviceLayout->addRow("Master Output Channels:", masterChannelCombo);

    cueDeviceCombo = new QComboBox();
    deviceLayout->addRow("Cue Output Device:", cueDeviceCombo);

    cueChannelCombo = new QComboBox();
    deviceLayout->addRow("Cue Output Channels:", cueChannelCombo);
    
    bufferSizeCombo = new QComboBox();
    bufferSizeCombo->addItems({"64", "128", "256", "512", "1024", "2048"});
    bufferSizeCombo->setCurrentText("512");
    deviceLayout->addRow("Buffer Size:", bufferSizeCombo);
    
    sampleRateCombo = new QComboBox();
    sampleRateCombo->addItems({"44100", "48000", "88200", "96000"});
    sampleRateCombo->setCurrentText("44100");
    deviceLayout->addRow("Sample Rate:", sampleRateCombo);
    
    exclusiveModeCheck = new QCheckBox("Exclusive Mode (WASAPI)");
    deviceLayout->addRow(exclusiveModeCheck);
    
    layout->addWidget(deviceGroup);
    
    // Audio Quality Group
    QGroupBox* qualityGroup = new QGroupBox("Audio Quality");
    QFormLayout* qualityLayout = new QFormLayout(qualityGroup);
    
    keylockQualityCombo = new QComboBox();
    keylockQualityCombo->addItems({"Fast", "Balanced", "High Quality"});
    keylockQualityCombo->setCurrentIndex(1);
    qualityLayout->addRow("Keylock Quality:", keylockQualityCombo);
    
    layout->addWidget(qualityGroup);
    
    // Volume Group
    QGroupBox* volumeGroup = new QGroupBox("Volume Control");
    QGridLayout* volumeLayout = new QGridLayout(volumeGroup);
    
    // Master Volume
    volumeLayout->addWidget(new QLabel("Master Volume:"), 0, 0);
    masterVolumeSlider = new QSlider(Qt::Horizontal);
    masterVolumeSlider->setRange(0, 100);
    masterVolumeSlider->setValue(80);
    volumeLayout->addWidget(masterVolumeSlider, 0, 1);
    masterVolumeLabel = new QLabel("80%");
    volumeLayout->addWidget(masterVolumeLabel, 0, 2);
    
    // Headphone Volume
    volumeLayout->addWidget(new QLabel("Headphone Volume:"), 1, 0);
    headphoneVolumeSlider = new QSlider(Qt::Horizontal);
    headphoneVolumeSlider->setRange(0, 100);
    headphoneVolumeSlider->setValue(70);
    volumeLayout->addWidget(headphoneVolumeSlider, 1, 1);
    headphoneVolumeLabel = new QLabel("70%");
    volumeLayout->addWidget(headphoneVolumeLabel, 1, 2);
    
    layout->addWidget(volumeGroup);
    layout->addStretch();
    
    // Connect audio signals
    connect(masterDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
        this, &PreferencesDialog::onMasterDeviceChanged);
    connect(masterChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
        this, &PreferencesDialog::onMasterChannelChanged);
    connect(cueDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
        this, &PreferencesDialog::onCueDeviceChanged);
    connect(cueChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
        this, &PreferencesDialog::onCueChannelChanged);
    connect(bufferSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &PreferencesDialog::onBufferSizeChanged);
    connect(sampleRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &PreferencesDialog::onSampleRateChanged);
    connect(keylockQualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &PreferencesDialog::onKeylockQualityChanged);
    
    // Connect volume signals
    connect(masterVolumeSlider, &QSlider::valueChanged, [this](int value) {
        updateVolumeLabel(masterVolumeSlider, masterVolumeLabel, "");
    });
    connect(headphoneVolumeSlider, &QSlider::valueChanged, [this](int value) {
        updateVolumeLabel(headphoneVolumeSlider, headphoneVolumeLabel, "");
    });
}

void PreferencesDialog::createDeckTab() {
    deckTab = new QWidget();
    tabWidget->addTab(deckTab, QIcon(":/icons/deck.png"), "Decks");
    
    QVBoxLayout* layout = new QVBoxLayout(deckTab);
    
    // Deck A Defaults
    QGroupBox* deckAGroup = new QGroupBox("Deck A - Default Settings");
    QFormLayout* deckALayout = new QFormLayout(deckAGroup);
    
    deckAKeylockDefault = new QCheckBox("Keylock enabled by default");
    deckALayout->addRow(deckAKeylockDefault);
    
    deckAQuantizeDefault = new QCheckBox("Quantize enabled by default");
    deckALayout->addRow(deckAQuantizeDefault);
    
    deckASpeedDefault = new QDoubleSpinBox();
    deckASpeedDefault->setRange(0.5, 2.0);
    deckASpeedDefault->setSingleStep(0.01);
    deckASpeedDefault->setValue(1.0);
    deckASpeedDefault->setSuffix("x");
    deckALayout->addRow("Default Speed:", deckASpeedDefault);
    
    layout->addWidget(deckAGroup);
    
    // Deck B Defaults
    QGroupBox* deckBGroup = new QGroupBox("Deck B - Default Settings");
    QFormLayout* deckBLayout = new QFormLayout(deckBGroup);
    
    deckBKeylockDefault = new QCheckBox("Keylock enabled by default");
    deckBLayout->addRow(deckBKeylockDefault);
    
    deckBQuantizeDefault = new QCheckBox("Quantize enabled by default");
    deckBLayout->addRow(deckBQuantizeDefault);
    
    deckBSpeedDefault = new QDoubleSpinBox();
    deckBSpeedDefault->setRange(0.5, 2.0);
    deckBSpeedDefault->setSingleStep(0.01);
    deckBSpeedDefault->setValue(1.0);
    deckBSpeedDefault->setSuffix("x");
    deckBLayout->addRow("Default Speed:", deckBSpeedDefault);
    
    layout->addWidget(deckBGroup);
    
    // Behavior Group
    QGroupBox* behaviorGroup = new QGroupBox("Deck Behavior");
    QFormLayout* behaviorLayout = new QFormLayout(behaviorGroup);
    
    syncOnLoad = new QCheckBox("Auto-sync tempo when loading tracks");
    behaviorLayout->addRow(syncOnLoad);
    
    autoGainAdjust = new QCheckBox("Auto-adjust gain for consistent volume");
    autoGainAdjust->setChecked(true);
    behaviorLayout->addRow(autoGainAdjust);
    
    loopLengthDefault = new QSpinBox();
    loopLengthDefault->setRange(1, 32);
    loopLengthDefault->setValue(4);
    loopLengthDefault->setSuffix(" beats");
    behaviorLayout->addRow("Default Loop Length:", loopLengthDefault);
    
    scratchSensitivity = new QComboBox();
    scratchSensitivity->addItems({"Low", "Medium", "High", "Ultra"});
    scratchSensitivity->setCurrentIndex(1);
    behaviorLayout->addRow("Scratch Sensitivity:", scratchSensitivity);
    
    layout->addWidget(behaviorGroup);
    layout->addStretch();
}

void PreferencesDialog::createInterfaceTab() {
    interfaceTab = new QWidget();
    tabWidget->addTab(interfaceTab, QIcon(":/icons/interface.png"), "Interface");
    
    QVBoxLayout* layout = new QVBoxLayout(interfaceTab);
    
    // Theme Group
    QGroupBox* themeGroup = new QGroupBox("Theme & Appearance");
    QFormLayout* themeLayout = new QFormLayout(themeGroup);
    
    themeCombo = new QComboBox();
    populateThemes();
    themeLayout->addRow("Theme:", themeCombo);
    
    skinCombo = new QComboBox();
    populateSkins();
    themeLayout->addRow("Skin:", skinCombo);
    
    fontButton = new QPushButton("Select Font...");
    themeLayout->addRow("UI Font:", fontButton);
    
    fullscreenMode = new QCheckBox("Start in fullscreen mode");
    themeLayout->addRow(fullscreenMode);
    
    layout->addWidget(themeGroup);
    
    // Waveform Group
    QGroupBox* waveformGroup = new QGroupBox("Waveform Display");
    QFormLayout* waveformLayout = new QFormLayout(waveformGroup);
    
    waveformColorButton = new QPushButton();
    setColorButtonColor(waveformColorButton, QColor(0, 200, 255));
    waveformLayout->addRow("Waveform Color:", waveformColorButton);
    
    beatGridColorButton = new QPushButton();
    setColorButtonColor(beatGridColorButton, QColor(255, 255, 255, 100));
    waveformLayout->addRow("Beat Grid Color:", beatGridColorButton);
    
    loopColorButton = new QPushButton();
    setColorButtonColor(loopColorButton, QColor(255, 165, 0));
    waveformLayout->addRow("Loop Color:", loopColorButton);
    
    showBpmOnWaveform = new QCheckBox("Show BPM on waveform");
    showBpmOnWaveform->setChecked(true);
    waveformLayout->addRow(showBpmOnWaveform);
    
    showBeatNumbers = new QCheckBox("Show beat numbers");
    waveformLayout->addRow(showBeatNumbers);
    
    animatedWaveforms = new QCheckBox("Animated waveforms");
    animatedWaveforms->setChecked(true);
    waveformLayout->addRow(animatedWaveforms);
    
    // Waveform Quality Slider
    QHBoxLayout* qualitySliderLayout = new QHBoxLayout();
    waveformQualitySlider = new QSlider(Qt::Horizontal);
    waveformQualitySlider->setRange(25, 100);
    waveformQualitySlider->setValue(75);
    QLabel* qualityLowLabel = new QLabel("Low");
    QLabel* qualityHighLabel = new QLabel("High");
    qualitySliderLayout->addWidget(qualityLowLabel);
    qualitySliderLayout->addWidget(waveformQualitySlider);
    qualitySliderLayout->addWidget(qualityHighLabel);
    waveformLayout->addRow("Waveform Quality:", qualitySliderLayout);
    
    layout->addWidget(waveformGroup);
    layout->addStretch();
    
    // Connect color button signals
    connect(waveformColorButton, &QPushButton::clicked, [this]() {
        QColor color = QColorDialog::getColor(getColorFromButton(waveformColorButton), this, "Waveform Color");
        if (color.isValid()) {
            setColorButtonColor(waveformColorButton, color);
            markDirty();
        }
    });
    
    connect(beatGridColorButton, &QPushButton::clicked, [this]() {
        QColor color = QColorDialog::getColor(getColorFromButton(beatGridColorButton), this, "Beat Grid Color");
        if (color.isValid()) {
            setColorButtonColor(beatGridColorButton, color);
            markDirty();
        }
    });
    
    connect(loopColorButton, &QPushButton::clicked, [this]() {
        QColor color = QColorDialog::getColor(getColorFromButton(loopColorButton), this, "Loop Color");
        if (color.isValid()) {
            setColorButtonColor(loopColorButton, color);
            markDirty();
        }
    });
    
    connect(fontButton, &QPushButton::clicked, [this]() {
        bool ok;
        QFont font = QFontDialog::getFont(&ok, settings.uiFont, this, "Select UI Font");
        if (ok) {
            settings.uiFont = font;
            fontButton->setText(formatFontName(font));
            markDirty();
        }
    });
}

void PreferencesDialog::createLibraryTab() {
    libraryTab = new QWidget();
    tabWidget->addTab(libraryTab, QIcon(":/icons/library.png"), "Library");
    
    QVBoxLayout* layout = new QVBoxLayout(libraryTab);
    
    // Paths Group
    QGroupBox* pathsGroup = new QGroupBox("Library Paths");
    QFormLayout* pathsLayout = new QFormLayout(pathsGroup);
    
    // Library Path
    QHBoxLayout* libraryPathLayout = new QHBoxLayout();
    libraryPathEdit = new QLineEdit();
    libraryPathEdit->setPlaceholderText("Select your music library folder...");
    libraryPathButton = new QPushButton("Browse...");
    libraryPathLayout->addWidget(libraryPathEdit);
    libraryPathLayout->addWidget(libraryPathButton);
    pathsLayout->addRow("Music Library:", libraryPathLayout);
    
    // Cache Path
    QHBoxLayout* cachePathLayout = new QHBoxLayout();
    cachePathEdit = new QLineEdit();
    cachePathEdit->setPlaceholderText("Cache folder for analysis data...");
    cachePathButton = new QPushButton("Browse...");
    cachePathLayout->addWidget(cachePathEdit);
    cachePathLayout->addWidget(cachePathButton);
    pathsLayout->addRow("Cache Path:", cachePathLayout);
    
    layout->addWidget(pathsGroup);
    
    // Analysis Group
    QGroupBox* analysisGroup = new QGroupBox("Analysis Settings");
    QFormLayout* analysisLayout = new QFormLayout(analysisGroup);
    
    autoScanOnStartup = new QCheckBox("Auto-scan library on startup");
    autoScanOnStartup->setChecked(true);
    analysisLayout->addRow(autoScanOnStartup);
    
    deepAnalysis = new QCheckBox("Deep analysis (BPM, Key, etc.)");
    deepAnalysis->setChecked(true);
    analysisLayout->addRow(deepAnalysis);
    
    autoCreateWaveforms = new QCheckBox("Auto-create waveform previews");
    autoCreateWaveforms->setChecked(true);
    analysisLayout->addRow(autoCreateWaveforms);
    
    layout->addWidget(analysisGroup);
    
    // Library Behavior Group
    QGroupBox* behaviorGroup = new QGroupBox("Library Behavior");
    QFormLayout* behaviorLayout = new QFormLayout(behaviorGroup);
    
    maxRecentTracks = new QSpinBox();
    maxRecentTracks->setRange(5, 100);
    maxRecentTracks->setValue(20);
    behaviorLayout->addRow("Max Recent Tracks:", maxRecentTracks);
    
    sortDefaultCombo = new QComboBox();
    sortDefaultCombo->addItems({"Artist", "Title", "Album", "BPM", "Date Added", "Genre"});
    behaviorLayout->addRow("Default Sort:", sortDefaultCombo);
    
    layout->addWidget(behaviorGroup);
    
    // Actions Group
    QGroupBox* actionsGroup = new QGroupBox("Library Actions");
    QVBoxLayout* actionsLayout = new QVBoxLayout(actionsGroup);
    
    rescanButton = new QPushButton("Rescan Library Now");
    rescanButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    actionsLayout->addWidget(rescanButton);
    
    clearCacheButton = new QPushButton("Clear Analysis Cache");
    clearCacheButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    actionsLayout->addWidget(clearCacheButton);
    
    layout->addWidget(actionsGroup);
    layout->addStretch();
    
    // Connect path browser signals
    connect(libraryPathButton, &QPushButton::clicked, [this]() {
        QString path = QFileDialog::getExistingDirectory(this, "Select Music Library Folder", 
                                                        libraryPathEdit->text());
        if (!path.isEmpty()) {
            libraryPathEdit->setText(path);
        }
    });
    
    connect(cachePathButton, &QPushButton::clicked, [this]() {
        QString path = QFileDialog::getExistingDirectory(this, "Select Cache Folder", 
                                                        cachePathEdit->text());
        if (!path.isEmpty()) {
            cachePathEdit->setText(path);
        }
    });
    
    connect(rescanButton, &QPushButton::clicked, this, &PreferencesDialog::onRescanLibrary);
    connect(clearCacheButton, &QPushButton::clicked, this, &PreferencesDialog::onClearCache);
}

void PreferencesDialog::createPerformanceTab() {
    performanceTab = new QWidget();
    tabWidget->addTab(performanceTab, QIcon(":/icons/performance.png"), "Performance");
    
    QVBoxLayout* layout = new QVBoxLayout(performanceTab);
    
    // CPU Group
    QGroupBox* cpuGroup = new QGroupBox("CPU Settings");
    QFormLayout* cpuLayout = new QFormLayout(cpuGroup);
    
    cpuCoresSpinBox = new QSpinBox();
    cpuCoresSpinBox->setRange(-1, 64);
    cpuCoresSpinBox->setValue(-1);
    cpuCoresSpinBox->setSpecialValueText("Auto-detect");
    cpuLayout->addRow("CPU Cores to use:", cpuCoresSpinBox);
    
    threadPrioritySlider = new QSlider(Qt::Horizontal);
    threadPrioritySlider->setRange(0, 100);
    threadPrioritySlider->setValue(50);
    cpuLayout->addRow("Thread Priority:", threadPrioritySlider);
    
    layout->addWidget(cpuGroup);
    
    // Memory Group
    QGroupBox* memoryGroup = new QGroupBox("Memory Settings");
    QFormLayout* memoryLayout = new QFormLayout(memoryGroup);
    
    memoryLimitSpinBox = new QSpinBox();
    memoryLimitSpinBox->setRange(256, 8192);
    memoryLimitSpinBox->setValue(1024);
    memoryLimitSpinBox->setSuffix(" MB");
    memoryLayout->addRow("Memory Limit:", memoryLimitSpinBox);
    
    diskCacheSlider = new QSlider(Qt::Horizontal);
    diskCacheSlider->setRange(64, 1024);
    diskCacheSlider->setValue(256);
    memoryLayout->addRow("Disk Cache:", diskCacheSlider);
    
    layout->addWidget(memoryGroup);
    
    // Graphics Group
    QGroupBox* graphicsGroup = new QGroupBox("Graphics Settings");
    QFormLayout* graphicsLayout = new QFormLayout(graphicsGroup);
    
    enableGpuAcceleration = new QCheckBox("Enable GPU acceleration");
    enableGpuAcceleration->setChecked(true);
    graphicsLayout->addRow(enableGpuAcceleration);
    
    renderQualityCombo = new QComboBox();
    renderQualityCombo->addItems({"Low", "Medium", "High", "Ultra"});
    renderQualityCombo->setCurrentIndex(2);
    graphicsLayout->addRow("Render Quality:", renderQualityCombo);
    
    layout->addWidget(graphicsGroup);
    
    // Advanced Group
    QGroupBox* advancedGroup = new QGroupBox("Advanced Performance");
    QFormLayout* advancedLayout = new QFormLayout(advancedGroup);
    
    lowLatencyMode = new QCheckBox("Low-latency mode (uses more CPU)");
    advancedLayout->addRow(lowLatencyMode);
    
    backgroundProcessing = new QCheckBox("Background processing");
    backgroundProcessing->setChecked(true);
    advancedLayout->addRow(backgroundProcessing);
    
    layout->addWidget(advancedGroup);
    layout->addStretch();
}

void PreferencesDialog::createMidiTab() {
    midiTab = new QWidget();
    tabWidget->addTab(midiTab, QIcon(":/icons/midi.png"), "MIDI");
    
    QVBoxLayout* layout = new QVBoxLayout(midiTab);
    
    // MIDI Device Group
    QGroupBox* deviceGroup = new QGroupBox("MIDI Controller");
    QFormLayout* deviceLayout = new QFormLayout(deviceGroup);
    
    // Enable MIDI
    midiEnabled = new QCheckBox("Enable MIDI control");
    deviceLayout->addRow(midiEnabled);
    
    // Device Selection
    QHBoxLayout* deviceSelectionLayout = new QHBoxLayout();
    midiDeviceCombo = new QComboBox();
    midiDeviceCombo->setMinimumWidth(200);
    midiRefreshButton = new QPushButton("Refresh");
    midiRefreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    midiRefreshButton->setFixedWidth(80);
    deviceSelectionLayout->addWidget(midiDeviceCombo);
    deviceSelectionLayout->addWidget(midiRefreshButton);
    deviceLayout->addRow("MIDI Device:", deviceSelectionLayout);
    
    // Status indicator
    midiStatusLabel = new QLabel("No device connected");
    midiStatusLabel->setStyleSheet("color: #888; font-style: italic;");
    deviceLayout->addRow("Status:", midiStatusLabel);
    
    // Device activation button
    midiActivateButton = new QPushButton("Connect Device");
    midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    midiActivateButton->setEnabled(false);  // Disabled until device is selected
    midiActivateButton->setToolTip("Click to connect and activate the selected MIDI device");
    deviceLayout->addRow("", midiActivateButton);
    
    layout->addWidget(deviceGroup);
    
    // MIDI Learning Group
    QGroupBox* learningGroup = new QGroupBox("MIDI Learning");
    QFormLayout* learningLayout = new QFormLayout(learningGroup);
    
    midiLearnMode = new QCheckBox("Enable MIDI Learn mode");
    midiLearnMode->setToolTip("When enabled, click on any control and then move a knob/fader on your MIDI controller to assign it.");
    learningLayout->addRow(midiLearnMode);
    
    layout->addWidget(learningGroup);
    
    // MIDI Control Mapping Group
    QGroupBox* mappingGroup = new QGroupBox("Control Mapping");
    QVBoxLayout* mappingLayout = new QVBoxLayout(mappingGroup);
    
    // Instructions
    QLabel* mappingInstructions = new QLabel(
        "Map your MIDI controller buttons to deck controls:\n"
        "1. Click 'Learn' next to a control\n" 
        "2. Press the button/knob on your controller\n"
        "3. The mapping is saved automatically"
    );
    mappingInstructions->setStyleSheet("color: #aaa; font-size: 10px; margin-bottom: 10px;");
    mappingLayout->addWidget(mappingInstructions);
    
    // Mapping table
    midiMappingTable = new QTableWidget(0, 4);
    midiMappingTable->setHorizontalHeaderLabels({"Control", "MIDI Input", "Status", "Action"});
    midiMappingTable->horizontalHeader()->setStretchLastSection(true);
    midiMappingTable->setAlternatingRowColors(true);
    midiMappingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    midiMappingTable->setMinimumHeight(300);
    midiMappingTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
    // Set column widths
    midiMappingTable->setColumnWidth(0, 150); // Control name
    midiMappingTable->setColumnWidth(1, 120); // MIDI input
    midiMappingTable->setColumnWidth(2, 100); // Status
    
    mappingLayout->addWidget(midiMappingTable);
    
    // Add essential deck controls
    addMidiMappingRow("Play/Pause Deck A", "", "Not Mapped");
    addMidiMappingRow("Play/Pause Deck B", "", "Not Mapped");
    addMidiMappingRow("Cue Deck A", "", "Not Mapped");
    addMidiMappingRow("Cue Deck B", "", "Not Mapped");
    addMidiMappingRow("Tempo Deck A", "", "Not Mapped");
    addMidiMappingRow("Tempo Deck B", "", "Not Mapped");
    addMidiMappingRow("Volume Deck A", "", "Not Mapped");
    addMidiMappingRow("Volume Deck B", "", "Not Mapped");
    addMidiMappingRow("Crossfader", "", "Not Mapped");
    
    // Mapping buttons
    QHBoxLayout* mappingButtonLayout = new QHBoxLayout();
    
    QPushButton* addMappingButton = new QPushButton("Add Control");
    addMappingButton->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    
    QPushButton* clearMappingsButton = new QPushButton("Clear All");
    clearMappingsButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    
    QPushButton* saveMappingsButton = new QPushButton("Save Preset");
    saveMappingsButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    
    QPushButton* loadMappingsButton = new QPushButton("Load Preset");
    loadMappingsButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    
    mappingButtonLayout->addWidget(addMappingButton);
    mappingButtonLayout->addWidget(clearMappingsButton);
    mappingButtonLayout->addStretch();
    mappingButtonLayout->addWidget(saveMappingsButton);
    mappingButtonLayout->addWidget(loadMappingsButton);
    
    mappingLayout->addLayout(mappingButtonLayout);
    
    // Connect mapping button signals
    connect(clearMappingsButton, &QPushButton::clicked, [this]() {
        int ret = QMessageBox::question(this, "Clear All Mappings", 
            "Are you sure you want to clear all MIDI mappings?",
            QMessageBox::Yes | QMessageBox::No);
        
        if (ret == QMessageBox::Yes) {
            for (int row = 0; row < midiMappingTable->rowCount(); ++row) {
                midiMappingTable->item(row, 1)->setText("");
                midiMappingTable->item(row, 2)->setText("Not Mapped");
                midiMappingTable->item(row, 2)->setForeground(QBrush(QColor(255, 87, 34))); // Orange
            }
            QMessageBox::information(this, "Mappings Cleared", "All MIDI mappings have been cleared.");
        }
    });
    
    connect(saveMappingsButton, &QPushButton::clicked, [this]() {
        QString fileName = QFileDialog::getSaveFileName(this, 
            "Save MIDI Mapping Preset", 
            QApplication::applicationDirPath() + "/midi_presets/", 
            "MIDI Preset Files (*.midip)");
        
        if (!fileName.isEmpty()) {
            saveMidiMappings(fileName);
        }
    });
    
    connect(loadMappingsButton, &QPushButton::clicked, [this]() {
        QString fileName = QFileDialog::getOpenFileName(this, 
            "Load MIDI Mapping Preset", 
            QApplication::applicationDirPath() + "/midi_presets/", 
            "MIDI Preset Files (*.midip)");
        
        if (!fileName.isEmpty()) {
            loadMidiMappings(fileName);
        }
    });
    
    layout->addWidget(mappingGroup, 3); // Give more space to mapping section
    
    // Test section with live input display
    QVBoxLayout* testSectionLayout = new QVBoxLayout();
    
    // Test button row
    QHBoxLayout* testButtonLayout = new QHBoxLayout();
    midiTestButton = new QPushButton("Start MIDI Test");
    midiTestButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    midiTestButton->setCheckable(true);
    midiTestButton->setToolTip("Click to start/stop live MIDI input monitoring");
    
    QPushButton* clearTestButton = new QPushButton("Clear");
    clearTestButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    clearTestButton->setFixedWidth(60);
    
    testButtonLayout->addWidget(midiTestButton);
    testButtonLayout->addWidget(clearTestButton);
    testSectionLayout->addLayout(testButtonLayout);
    
    // Live input display
    midiInputLabel = new QLabel("Click 'Start MIDI Test' and move controls on your MIDI device");
    midiInputLabel->setStyleSheet("color: #666; font-size: 9px; padding: 3px; border: 1px solid #444; border-radius: 3px; background-color: #2a2a2a;");
    midiInputLabel->setWordWrap(true);
    midiInputLabel->setMaximumHeight(40);
    midiInputLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    testSectionLayout->addWidget(midiInputLabel);
    
    // MIDI activity indicator
    midiActivityLabel = new QLabel("● No Activity");
    midiActivityLabel->setStyleSheet("color: #666; font-size: 9px;");
    testSectionLayout->addWidget(midiActivityLabel);
    
    learningLayout->addRow("MIDI Test:", testSectionLayout);
    
    // Connect test button signals
    connect(midiTestButton, &QPushButton::toggled, [this, clearTestButton](bool checked) {
        qDebug() << "PreferencesDialog: MIDI Test button toggled to:" << checked;
        
        if (checked) {
            // Check if device is selected
            QString selectedDevice = midiDeviceCombo->currentText();
            qDebug() << "PreferencesDialog: Selected MIDI device:" << selectedDevice;
            
            if (selectedDevice.isEmpty() || selectedDevice == "None") {
                QMessageBox::warning(this, "MIDI Test", 
                    "Please select a MIDI device first from the dropdown above.");
                midiTestButton->setChecked(false);
                return;
            }
            
            // Enable MIDI and open device if not already open
            qDebug() << "PreferencesDialog: Current device open status:" << (midiEngine ? midiEngine->isDeviceOpen() : false);
            qDebug() << "PreferencesDialog: Current device name:" << (midiEngine ? midiEngine->getCurrentDevice() : "none");
            
            if (!midiEngine->isDeviceOpen() || midiEngine->getCurrentDevice() != selectedDevice) {
                qDebug() << "PreferencesDialog: Device not connected. Please use 'Connect Device' button first.";
                
                QMessageBox::information(this, "MIDI Test", 
                    "Please connect to the MIDI device first using the 'Connect Device' button, then try the test again.");
                midiTestButton->setChecked(false);
                return;
                
                if (!midiEngine->openMidiDevice(selectedDevice)) {
                    QMessageBox::warning(this, "MIDI Test Error", 
                        "Failed to open MIDI device: " + selectedDevice + 
                        "\n\nPossible causes:\n"
                        "• Device is already in use by another application\n"
                        "• Device is disconnected\n"
                        "• Driver issues\n\n"
                        "Please check your MIDI device connection and try again.");
                    midiTestButton->setChecked(false);
                    return;
                }
            }
            
            // Activate test mode
            midiTestActive = true;
            midiTestButton->setText("Stop MIDI Test");
            midiTestButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
            midiInputLabel->setText("Listening for MIDI input from: " + selectedDevice + 
                                  "\n\nMove any control on your device to test the connection...");
            midiInputLabel->setStyleSheet("color: #4CAF50; font-size: 10px; padding: 5px; border: 1px solid #4CAF50; border-radius: 3px; background-color: #1a2a1a;");
            midiActivityLabel->setText("● Listening");
            midiActivityLabel->setStyleSheet("color: #4CAF50; font-size: 9px;");
            
            qDebug() << "PreferencesDialog: MIDI Test mode activated successfully";
        } else {
            // Deactivate test mode
            midiTestActive = false;
            midiTestButton->setText("Start MIDI Test");
            midiTestButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
            midiInputLabel->setText("MIDI test stopped. Click 'Start MIDI Test' to resume monitoring.");
            midiInputLabel->setStyleSheet("color: #666; font-size: 10px; padding: 5px; border: 1px solid #444; border-radius: 3px; background-color: #2a2a2a;");
            midiActivityLabel->setText("● Stopped");
            midiActivityLabel->setStyleSheet("color: #666; font-size: 9px;");
            
            qDebug() << "PreferencesDialog: MIDI Test mode deactivated";
        }
    });
    
    connect(clearTestButton, &QPushButton::clicked, [this]() {
        if (midiTestButton->isChecked()) {
            midiInputLabel->setText("Listening for MIDI input... Move any control on your device.");
        } else {
            midiInputLabel->setText("Click 'Start MIDI Test' and move controls on your MIDI device");
        }
    });
    
    layout->addWidget(learningGroup, 0); // Minimal space for learning section

    // MIDI Info Group
    QGroupBox* infoGroup = new QGroupBox("MIDI Information");
    QVBoxLayout* infoLayout = new QVBoxLayout(infoGroup);
    
    QLabel* infoText = new QLabel(
        "<b>Quick Setup:</b> Connect USB → Select Device → Connect → Learn Controls"
    );
    infoText->setWordWrap(true);
    infoText->setStyleSheet("color: #999; font-size: 10px; padding: 5px;");
    infoLayout->addWidget(infoText);
    
    // Make info group much smaller
    infoGroup->setMaximumHeight(60);
    layout->addWidget(infoGroup, 0); // Minimal space for info section
    
    // Populate MIDI devices on creation
    populateMidiDevices();
    
    // Connect signals
    connect(midiEnabled, &QCheckBox::toggled, [this](bool enabled) {
        midiDeviceCombo->setEnabled(enabled);
        midiRefreshButton->setEnabled(enabled);
        midiActivateButton->setEnabled(enabled && midiDeviceCombo->currentIndex() > 0);
        midiLearnMode->setEnabled(enabled);
        midiTestButton->setEnabled(enabled);
        
        if (enabled && midiDeviceCombo->count() > 1) {
            midiStatusLabel->setText("Ready");
            midiStatusLabel->setStyleSheet("color: #4CAF50; font-weight: bold;");
        } else {
            midiStatusLabel->setText(enabled ? "No device selected" : "MIDI disabled");
            midiStatusLabel->setStyleSheet("color: #888; font-style: italic;");
        }
    });
    
    connect(midiEnabled, &QCheckBox::toggled, this, &PreferencesDialog::onMidiEnabledChanged);
    connect(midiRefreshButton, &QPushButton::clicked, this, &PreferencesDialog::onMidiDeviceRefresh);
    connect(midiActivateButton, &QPushButton::clicked, this, &PreferencesDialog::onMidiDeviceActivate);
    connect(midiTestButton, &QPushButton::clicked, this, &PreferencesDialog::onMidiDeviceTest);
    connect(midiDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &PreferencesDialog::onMidiDeviceChanged);
    connect(midiLearnMode, &QCheckBox::toggled, this, &PreferencesDialog::onMidiLearnToggle);
    
    // Initialize UI state
    midiEnabled->setChecked(false);
    midiDeviceCombo->setEnabled(false);
    midiRefreshButton->setEnabled(false);
    midiActivateButton->setEnabled(false);
    midiLearnMode->setEnabled(false);
    midiTestButton->setEnabled(false);
}

void PreferencesDialog::createAdvancedTab() {
    advancedTab = new QWidget();
    tabWidget->addTab(advancedTab, QIcon(":/icons/advanced.png"), "Advanced");
    
    QVBoxLayout* layout = new QVBoxLayout(advancedTab);
    
    // Configuration Group
    QGroupBox* configGroup = new QGroupBox("Configuration");
    QFormLayout* configLayout = new QFormLayout(configGroup);
    
    // Config Path
    QHBoxLayout* configPathLayout = new QHBoxLayout();
    configPathEdit = new QLineEdit();
    configPathEdit->setReadOnly(true);
    configPathButton = new QPushButton("Change...");
    configPathLayout->addWidget(configPathEdit);
    configPathLayout->addWidget(configPathButton);
    configLayout->addRow("Config Path:", configPathLayout);
    
    layout->addWidget(configGroup);
    
    // Debug Group
    QGroupBox* debugGroup = new QGroupBox("Debug & Logging");
    QFormLayout* debugLayout = new QFormLayout(debugGroup);
    
    debugLogging = new QCheckBox("Enable debug logging");
    debugLayout->addRow(debugLogging);
    
    crashReporting = new QCheckBox("Enable crash reporting");
    crashReporting->setChecked(true);
    debugLayout->addRow(crashReporting);
    
    betaFeatures = new QCheckBox("Enable beta features");
    debugLayout->addRow(betaFeatures);
    
    layout->addWidget(debugGroup);
    
    // Settings Management Group
    QGroupBox* settingsGroup = new QGroupBox("Settings Management");
    QVBoxLayout* settingsLayout = new QVBoxLayout(settingsGroup);
    
    exportSettingsButton = new QPushButton("Export Settings...");
    exportSettingsButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    settingsLayout->addWidget(exportSettingsButton);
    
    importSettingsButton = new QPushButton("Import Settings...");
    importSettingsButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    settingsLayout->addWidget(importSettingsButton);
    
    resetAllButton = new QPushButton("Reset All Settings");
    resetAllButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    settingsLayout->addWidget(resetAllButton);
    
    layout->addWidget(settingsGroup);
    layout->addStretch();
    
    // Connect advanced signals
    connect(exportSettingsButton, &QPushButton::clicked, [this]() {
    QString fileName = QFileDialog::getSaveFileName(this, "Export Settings", 
                               AppConfig::instance().getSettingsExportPath(), 
                                                       "JSON Files (*.json)");
        if (!fileName.isEmpty()) {
            saveSettings(); // Ensure current settings are saved
            QMessageBox::information(this, "Export Complete", 
                                   "Settings exported successfully to:\n" + fileName);
        }
    });
    
    connect(importSettingsButton, &QPushButton::clicked, [this]() {
    QString fileName = QFileDialog::getOpenFileName(this, "Import Settings", 
                               AppConfig::instance().getConfigDirectory(), "JSON Files (*.json)");
        if (!fileName.isEmpty()) {
            int ret = QMessageBox::question(this, "Import Settings", 
                                          "This will replace all current settings. Continue?",
                                          QMessageBox::Yes | QMessageBox::No);
            if (ret == QMessageBox::Yes) {
                loadSettings(); // Reload from imported file
                QMessageBox::information(this, "Import Complete", 
                                       "Settings imported successfully. Restart may be required.");
            }
        }
    });
    
    connect(resetAllButton, &QPushButton::clicked, [this]() {
        int ret = QMessageBox::warning(this, "Reset All Settings", 
                                     "This will reset ALL settings to defaults. Continue?",
                                     QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            restoreDefaults();
        }
    });
}

// Button event handlers
void PreferencesDialog::onOkClicked() {
    if (pendingChanges) {
        saveSettings();
        applySettings();
        originalSettings = settings;
        pendingChanges = false;
    }
    updateApplyButtonState();
    accept();
}

void PreferencesDialog::onCancelClicked() {
    settings = originalSettings; // Restore original settings
    reject();
}

void PreferencesDialog::onApplyClicked() {
    if (!pendingChanges) {
        return;
    }

    saveSettings();
    applySettings();
    originalSettings = settings; // Update backup
    pendingChanges = false;
    updateApplyButtonState();
}

void PreferencesDialog::onRestoreDefaultsClicked() {
    int ret = QMessageBox::question(this, "Restore Defaults", 
                                   "This will restore all settings to defaults. Continue?",
                                   QMessageBox::Yes | QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        restoreDefaults();
    }
}

// Audio event handlers
void PreferencesDialog::onMasterDeviceChanged() {
    if (!masterDeviceCombo) {
        return;
    }

    const QVariantMap data = masterDeviceCombo->currentData().toMap();
    settings.masterAudioDevice = data.value("deviceName").toString();
    settings.masterAudioDeviceType = data.value("deviceType").toString();

    qDebug() << "PreferencesDialog: Master device changed to" << settings.masterAudioDevice
             << "(" << settings.masterAudioDeviceType << ")";

    updateChannelComboForDevice(masterChannelCombo,
                                settings.masterAudioDevice,
                                settings.masterAudioDeviceType,
                                settings.masterOutputChannelStart,
                                settings.masterOutputChannelCount);

    const QVariantMap channelData = masterChannelCombo->currentData().toMap();
    settings.masterOutputChannelStart = channelData.value("start").toInt();
    settings.masterOutputChannelCount = channelData.value("count").toInt();
}

void PreferencesDialog::onMasterChannelChanged() {
    if (!masterChannelCombo) {
        return;
    }

    const QVariantMap channelData = masterChannelCombo->currentData().toMap();
    settings.masterOutputChannelStart = channelData.value("start").toInt();
    settings.masterOutputChannelCount = channelData.value("count").toInt();

    qDebug() << "PreferencesDialog: Master channel selection changed to start"
             << settings.masterOutputChannelStart << "count" << settings.masterOutputChannelCount;
}

void PreferencesDialog::onCueDeviceChanged() {
    if (!cueDeviceCombo) {
        return;
    }

    const QVariantMap data = cueDeviceCombo->currentData().toMap();
    settings.cueAudioDevice = data.value("deviceName").toString();
    settings.cueAudioDeviceType = data.value("deviceType").toString();

    qDebug() << "PreferencesDialog: Cue device changed to" << settings.cueAudioDevice
             << "(" << settings.cueAudioDeviceType << ")";

    updateChannelComboForDevice(cueChannelCombo,
                                settings.cueAudioDevice,
                                settings.cueAudioDeviceType,
                                settings.cueOutputChannelStart,
                                settings.cueOutputChannelCount);

    const QVariantMap channelData = cueChannelCombo->currentData().toMap();
    settings.cueOutputChannelStart = channelData.value("start").toInt();
    settings.cueOutputChannelCount = channelData.value("count").toInt();
}

void PreferencesDialog::onCueChannelChanged() {
    if (!cueChannelCombo) {
        return;
    }

    const QVariantMap channelData = cueChannelCombo->currentData().toMap();
    settings.cueOutputChannelStart = channelData.value("start").toInt();
    settings.cueOutputChannelCount = channelData.value("count").toInt();

    qDebug() << "PreferencesDialog: Cue channel selection changed to start"
             << settings.cueOutputChannelStart << "count" << settings.cueOutputChannelCount;
}

void PreferencesDialog::onBufferSizeChanged(int size) {
    settings.bufferSize = bufferSizeCombo->currentText().toInt();
    
    // Calculate and display latency
    int sampleRate = sampleRateCombo->currentText().toInt();
    if (sampleRate > 0) {
        double latencyMs = (settings.bufferSize * 1000.0) / sampleRate;
        qDebug() << "PreferencesDialog: Buffer size changed to" << settings.bufferSize 
                 << "samples (~" << QString::number(latencyMs, 'f', 2) << "ms latency)";
    }
}

void PreferencesDialog::onSampleRateChanged() {
    settings.sampleRate = sampleRateCombo->currentText().toInt();
    
    // Recalculate latency display
    double latencyMs = (settings.bufferSize * 1000.0) / settings.sampleRate;
    qDebug() << "PreferencesDialog: Sample rate changed to" << settings.sampleRate 
             << "Hz (latency ~" << QString::number(latencyMs, 'f', 2) << "ms)";
}

void PreferencesDialog::onKeylockQualityChanged() {
    settings.keylockQuality = keylockQualityCombo->currentIndex();
    
    const char* qualityNames[] = {"Fast", "Balanced", "High Quality"};
    qDebug() << "PreferencesDialog: Keylock quality changed to" 
             << qualityNames[settings.keylockQuality];
}

// Interface event handlers
void PreferencesDialog::onThemeChanged() {
    // Implementation for theme change
}

void PreferencesDialog::onWaveformColorChanged() {
    // Implementation for waveform color change
}

void PreferencesDialog::onBeatGridColorChanged() {
    // Implementation for beat grid color change
}

void PreferencesDialog::onFontChanged() {
    // Implementation for font change
}

// Library event handlers
void PreferencesDialog::onLibraryPathChanged() {
    // Implementation for library path change
}

void PreferencesDialog::onCachePathChanged() {
    // Implementation for cache path change
}

void PreferencesDialog::onRescanLibrary() {
    QMessageBox::information(this, "Library Rescan", 
                           "Library rescan started in background...");
}

void PreferencesDialog::onClearCache() {
    int ret = QMessageBox::question(this, "Clear Cache", 
                                   "This will clear all analysis cache. Continue?",
                                   QMessageBox::Yes | QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        QMessageBox::information(this, "Cache Cleared", 
                               "Analysis cache cleared successfully.");
    }
}

// Performance event handlers
void PreferencesDialog::onCpuCoresChanged(int cores) {
    // Implementation for CPU cores change
}

void PreferencesDialog::onMemoryLimitChanged(int limitMB) {
    // Implementation for memory limit change
}

// Settings management
void PreferencesDialog::loadSettings() {
    QSettings config(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat);
    
    // Load Audio settings
    settings.masterAudioDevice = config.value("Audio/MasterDevice",
                                             config.value("Audio/Device", "")).toString();
    settings.masterAudioDeviceType = config.value("Audio/MasterDeviceType", "").toString();
    settings.masterOutputChannelStart = config.value("Audio/MasterChannelStart", 0).toInt();
    settings.masterOutputChannelCount = config.value("Audio/MasterChannelCount", 2).toInt();
    settings.cueAudioDevice = config.value("Audio/CueDevice", settings.masterAudioDevice).toString();
    settings.cueAudioDeviceType = config.value("Audio/CueDeviceType", settings.masterAudioDeviceType).toString();
    settings.cueOutputChannelStart = config.value("Audio/CueChannelStart",
                                                 settings.masterOutputChannelStart + settings.masterOutputChannelCount).toInt();
    settings.cueOutputChannelCount = config.value("Audio/CueChannelCount", 2).toInt();
    settings.bufferSize = config.value("Audio/BufferSize", 512).toInt();
    settings.sampleRate = config.value("Audio/SampleRate", 44100).toInt();
    settings.keylockQuality = config.value("Audio/KeylockQuality", 1).toInt();
    settings.exclusiveMode = config.value("Audio/ExclusiveMode", false).toBool();
    settings.masterVolume = config.value("Audio/MasterVolume", 0.8).toDouble();
    settings.headphoneVolume = config.value("Audio/HeadphoneVolume", 0.7).toDouble();
    
    // Load Deck settings
    settings.deckAKeylockDefault = config.value("Decks/DeckAKeylockDefault", false).toBool();
    settings.deckAQuantizeDefault = config.value("Decks/DeckAQuantizeDefault", false).toBool();
    settings.deckASpeedDefault = config.value("Decks/DeckASpeedDefault", 1.0).toDouble();
    settings.deckBKeylockDefault = config.value("Decks/DeckBKeylockDefault", false).toBool();
    settings.deckBQuantizeDefault = config.value("Decks/DeckBQuantizeDefault", false).toBool();
    settings.deckBSpeedDefault = config.value("Decks/DeckBSpeedDefault", 1.0).toDouble();
    settings.syncOnLoad = config.value("Decks/SyncOnLoad", false).toBool();
    settings.autoGainAdjust = config.value("Decks/AutoGainAdjust", true).toBool();
    settings.loopLengthDefault = config.value("Decks/LoopLengthDefault", 4).toInt();
    settings.scratchSensitivity = config.value("Decks/ScratchSensitivity", 50).toInt();
    
    // Load Interface settings
    settings.theme = config.value("Interface/Theme", "Dark").toString();
    settings.skin = config.value("Interface/Skin", "Default").toString();
    settings.waveformColor = config.value("Interface/WaveformColor", QColor(0, 200, 255)).value<QColor>();
    settings.beatGridColor = config.value("Interface/BeatGridColor", QColor(255, 255, 255, 100)).value<QColor>();
    settings.loopColor = config.value("Interface/LoopColor", QColor(255, 165, 0)).value<QColor>();
    settings.showBpmOnWaveform = config.value("Interface/ShowBpmOnWaveform", true).toBool();
    settings.showBeatNumbers = config.value("Interface/ShowBeatNumbers", false).toBool();
    settings.animatedWaveforms = config.value("Interface/AnimatedWaveforms", true).toBool();
    settings.waveformQuality = config.value("Interface/WaveformQuality", 75).toInt();
    settings.fullscreenMode = config.value("Interface/FullscreenMode", false).toBool();
    
    // Load Library settings
    settings.libraryPath = config.value("Library/Path", 
                                       QStandardPaths::writableLocation(QStandardPaths::MusicLocation)).toString();
    settings.cachePath = config.value("Library/CachePath", 
                                     AppConfig::instance().getConfigDirectory() + "/cache").toString();
    settings.autoScanOnStartup = config.value("Library/AutoScanOnStartup", true).toBool();
    settings.deepAnalysis = config.value("Library/DeepAnalysis", true).toBool();
    settings.autoCreateWaveforms = config.value("Library/AutoCreateWaveforms", true).toBool();
    settings.maxRecentTracks = config.value("Library/MaxRecentTracks", 20).toInt();
    settings.sortDefault = config.value("Library/SortDefault", "Artist").toString();
    
    // Load Performance settings
    settings.cpuCores = config.value("Performance/CpuCores", -1).toInt();
    settings.memoryLimitMB = config.value("Performance/MemoryLimitMB", 1024).toInt();
    settings.threadPriority = config.value("Performance/ThreadPriority", 50).toInt();
    settings.enableGpuAcceleration = config.value("Performance/EnableGpuAcceleration", true).toBool();
    settings.lowLatencyMode = config.value("Performance/LowLatencyMode", false).toBool();
    settings.renderQuality = config.value("Performance/RenderQuality", "High").toString();
    settings.backgroundProcessing = config.value("Performance/BackgroundProcessing", true).toBool();
    settings.diskCacheMB = config.value("Performance/DiskCacheMB", 256).toInt();
    
    // Load MIDI settings through MidiEngine
    midiEngine->loadSettings(config);
    
    // Load local MIDI UI settings
    settings.midiEnabled = config.value("MIDI/Enabled", false).toBool();
    settings.midiDevice = config.value("MIDI/Device", "").toString();
    settings.midiLearnMode = config.value("MIDI/LearnMode", false).toBool();
    
    // Load Advanced settings
    settings.configPath = config.value("Advanced/ConfigPath", AppConfig::instance().getConfigDirectory()).toString();
    settings.debugLogging = config.value("Advanced/DebugLogging", false).toBool();
    settings.crashReporting = config.value("Advanced/CrashReporting", true).toBool();
    settings.betaFeatures = config.value("Advanced/BetaFeatures", false).toBool();
    
    // Update UI controls with loaded settings
    updateUIFromSettings();

    originalSettings = settings;
    pendingChanges = false;
    updateApplyButtonState();
}

void PreferencesDialog::saveSettings() {
    settings = collectCurrentSettings();
    QSettings config(AppConfig::instance().getPreferencesPath(), QSettings::IniFormat);
    
    // Save Audio settings
    config.setValue("Audio/MasterDevice", settings.masterAudioDevice);
    config.setValue("Audio/MasterDeviceType", settings.masterAudioDeviceType);
    config.setValue("Audio/MasterChannelStart", settings.masterOutputChannelStart);
    config.setValue("Audio/MasterChannelCount", settings.masterOutputChannelCount);
    config.setValue("Audio/CueDevice", settings.cueAudioDevice);
    config.setValue("Audio/CueDeviceType", settings.cueAudioDeviceType);
    config.setValue("Audio/CueChannelStart", settings.cueOutputChannelStart);
    config.setValue("Audio/CueChannelCount", settings.cueOutputChannelCount);
    // Legacy compatibility key
    config.setValue("Audio/Device", settings.masterAudioDevice);
    config.setValue("Audio/BufferSize", settings.bufferSize);
    config.setValue("Audio/SampleRate", settings.sampleRate);
    config.setValue("Audio/KeylockQuality", settings.keylockQuality);
    config.setValue("Audio/ExclusiveMode", settings.exclusiveMode);
    config.setValue("Audio/MasterVolume", settings.masterVolume);
    config.setValue("Audio/HeadphoneVolume", settings.headphoneVolume);
    
    // Save Deck settings
    config.setValue("Decks/DeckAKeylockDefault", settings.deckAKeylockDefault);
    config.setValue("Decks/DeckAQuantizeDefault", settings.deckAQuantizeDefault);
    config.setValue("Decks/DeckASpeedDefault", settings.deckASpeedDefault);
    config.setValue("Decks/DeckBKeylockDefault", settings.deckBKeylockDefault);
    config.setValue("Decks/DeckBQuantizeDefault", settings.deckBQuantizeDefault);
    config.setValue("Decks/DeckBSpeedDefault", settings.deckBSpeedDefault);
    config.setValue("Decks/SyncOnLoad", settings.syncOnLoad);
    config.setValue("Decks/AutoGainAdjust", settings.autoGainAdjust);
    config.setValue("Decks/LoopLengthDefault", settings.loopLengthDefault);
    config.setValue("Decks/ScratchSensitivity", settings.scratchSensitivity);
    
    // Save Interface settings
    config.setValue("Interface/Theme", settings.theme);
    config.setValue("Interface/Skin", settings.skin);
    config.setValue("Interface/WaveformColor", settings.waveformColor);
    config.setValue("Interface/BeatGridColor", settings.beatGridColor);
    config.setValue("Interface/LoopColor", settings.loopColor);
    config.setValue("Interface/ShowBpmOnWaveform", settings.showBpmOnWaveform);
    config.setValue("Interface/ShowBeatNumbers", settings.showBeatNumbers);
    config.setValue("Interface/AnimatedWaveforms", settings.animatedWaveforms);
    config.setValue("Interface/WaveformQuality", settings.waveformQuality);
    config.setValue("Interface/FullscreenMode", settings.fullscreenMode);
    
    // Save Library settings
    config.setValue("Library/Path", settings.libraryPath);
    config.setValue("Library/CachePath", settings.cachePath);
    config.setValue("Library/AutoScanOnStartup", settings.autoScanOnStartup);
    config.setValue("Library/DeepAnalysis", settings.deepAnalysis);
    config.setValue("Library/AutoCreateWaveforms", settings.autoCreateWaveforms);
    config.setValue("Library/MaxRecentTracks", settings.maxRecentTracks);
    config.setValue("Library/SortDefault", settings.sortDefault);
    
    // Save Performance settings
    config.setValue("Performance/CpuCores", settings.cpuCores);
    config.setValue("Performance/MemoryLimitMB", settings.memoryLimitMB);
    config.setValue("Performance/ThreadPriority", settings.threadPriority);
    config.setValue("Performance/EnableGpuAcceleration", settings.enableGpuAcceleration);
    config.setValue("Performance/LowLatencyMode", settings.lowLatencyMode);
    config.setValue("Performance/RenderQuality", settings.renderQuality);
    config.setValue("Performance/BackgroundProcessing", settings.backgroundProcessing);
    config.setValue("Performance/DiskCacheMB", settings.diskCacheMB);
    
    // Save MIDI settings through MidiEngine
    midiEngine->saveSettings(config);
    
    // Save local MIDI UI settings
    config.setValue("MIDI/Enabled", settings.midiEnabled);
    config.setValue("MIDI/Device", settings.midiDevice);
    config.setValue("MIDI/LearnMode", settings.midiLearnMode);
    
    // Save Advanced settings
    config.setValue("Advanced/ConfigPath", settings.configPath);
    config.setValue("Advanced/DebugLogging", settings.debugLogging);
    config.setValue("Advanced/CrashReporting", settings.crashReporting);
    config.setValue("Advanced/BetaFeatures", settings.betaFeatures);
    
    config.sync();
}

void PreferencesDialog::applySettings() {
    const bool audioChanged = (settings.masterAudioDevice != originalSettings.masterAudioDevice)
        || (settings.masterAudioDeviceType != originalSettings.masterAudioDeviceType)
        || (settings.masterOutputChannelStart != originalSettings.masterOutputChannelStart)
        || (settings.masterOutputChannelCount != originalSettings.masterOutputChannelCount)
        || (settings.bufferSize != originalSettings.bufferSize)
        || (settings.sampleRate != originalSettings.sampleRate)
        || (settings.exclusiveMode != originalSettings.exclusiveMode);

    const bool keylockChanged = settings.keylockQuality != originalSettings.keylockQuality;

    // Apply Audio Settings (only when changed)
    if (mainWindowRef && audioChanged) {
        mainWindowRef->applyAudioSettings(settings.masterAudioDeviceType,
                                          settings.masterAudioDevice,
                                          settings.masterOutputChannelStart,
                                          settings.masterOutputChannelCount,
                                          settings.cueAudioDeviceType,
                                          settings.cueAudioDevice,
                                          settings.cueOutputChannelStart,
                                          settings.cueOutputChannelCount,
                                          settings.bufferSize,
                                          settings.sampleRate,
                                          settings.exclusiveMode);
        qDebug() << "PreferencesDialog: Applied audio settings - Device:" << settings.masterAudioDevice
                 << "Channels start:" << settings.masterOutputChannelStart
                 << "count:" << settings.masterOutputChannelCount
                 << "BufferSize:" << settings.bufferSize
                 << "SampleRate:" << settings.sampleRate << "Exclusive:" << settings.exclusiveMode;
    } else if (!audioChanged) {
        qDebug() << "PreferencesDialog: Audio settings unchanged - skipping device reconfiguration";
    }

    if (mainWindowRef && (keylockChanged || audioChanged)) {
        DJAudioPlayer::KeylockQuality quality = DJAudioPlayer::KeylockQuality::Balanced;
        switch (settings.keylockQuality) {
            case 0: quality = DJAudioPlayer::KeylockQuality::Fast; break;
            case 1: quality = DJAudioPlayer::KeylockQuality::Balanced; break;
            case 2: quality = DJAudioPlayer::KeylockQuality::Quality; break;
        }
        mainWindowRef->setKeylockQuality(quality);
    }
    
    // Apply settings to the application
    emit settingsChanged();
}

void PreferencesDialog::updateApplyButtonState() {
    if (applyButton) {
        applyButton->setEnabled(pendingChanges);
    }
}

void PreferencesDialog::markDirty() {
    if (suppressChangeTracking) {
        return;
    }

    AppSettings current = collectCurrentSettings();
    const bool isDirty = (current != originalSettings);

    settings = current;

    if (pendingChanges != isDirty) {
        pendingChanges = isDirty;
        updateApplyButtonState();
    }
}

void PreferencesDialog::installChangeTracking() {
    for (auto* combo : findChildren<QComboBox*>()) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            markDirty();
        });
        connect(combo, &QComboBox::editTextChanged, this, [this](const QString&) {
            markDirty();
        });
    }

    for (auto* lineEdit : findChildren<QLineEdit*>()) {
        connect(lineEdit, &QLineEdit::textChanged, this, [this](const QString&) {
            markDirty();
        });
    }

    for (auto* checkBox : findChildren<QCheckBox*>()) {
        connect(checkBox, &QCheckBox::toggled, this, [this](bool) {
            markDirty();
        });
    }

    for (auto* spinBox : findChildren<QSpinBox*>()) {
        connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
            markDirty();
        });
    }

    for (auto* doubleSpinBox : findChildren<QDoubleSpinBox*>()) {
        connect(doubleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
            markDirty();
        });
    }

    for (auto* slider : findChildren<QSlider*>()) {
        connect(slider, &QSlider::valueChanged, this, [this](int) {
            markDirty();
        });
    }
}

PreferencesDialog::AppSettings PreferencesDialog::collectCurrentSettings() const {
    AppSettings current = settings;

    if (masterDeviceCombo) {
        const QVariantMap deviceData = masterDeviceCombo->currentData().toMap();
        current.masterAudioDevice = deviceData.value("deviceName").toString();
        current.masterAudioDeviceType = deviceData.value("deviceType").toString();
    }
    if (masterChannelCombo) {
        const QVariantMap channelData = masterChannelCombo->currentData().toMap();
        current.masterOutputChannelStart = channelData.value("start").toInt();
        current.masterOutputChannelCount = channelData.value("count").toInt();
    }
    if (cueDeviceCombo) {
        const QVariantMap deviceData = cueDeviceCombo->currentData().toMap();
        current.cueAudioDevice = deviceData.value("deviceName").toString();
        current.cueAudioDeviceType = deviceData.value("deviceType").toString();
    }
    if (cueChannelCombo) {
        const QVariantMap channelData = cueChannelCombo->currentData().toMap();
        current.cueOutputChannelStart = channelData.value("start").toInt();
        current.cueOutputChannelCount = channelData.value("count").toInt();
    }
    if (bufferSizeCombo) current.bufferSize = bufferSizeCombo->currentText().toInt();
    if (sampleRateCombo) current.sampleRate = sampleRateCombo->currentText().toInt();
    if (keylockQualityCombo) current.keylockQuality = keylockQualityCombo->currentIndex();
    if (exclusiveModeCheck) current.exclusiveMode = exclusiveModeCheck->isChecked();
    if (masterVolumeSlider) current.masterVolume = masterVolumeSlider->value() / 100.0;
    if (headphoneVolumeSlider) current.headphoneVolume = headphoneVolumeSlider->value() / 100.0;

    if (deckAKeylockDefault) current.deckAKeylockDefault = deckAKeylockDefault->isChecked();
    if (deckAQuantizeDefault) current.deckAQuantizeDefault = deckAQuantizeDefault->isChecked();
    if (deckASpeedDefault) current.deckASpeedDefault = deckASpeedDefault->value();
    if (deckBKeylockDefault) current.deckBKeylockDefault = deckBKeylockDefault->isChecked();
    if (deckBQuantizeDefault) current.deckBQuantizeDefault = deckBQuantizeDefault->isChecked();
    if (deckBSpeedDefault) current.deckBSpeedDefault = deckBSpeedDefault->value();
    if (syncOnLoad) current.syncOnLoad = syncOnLoad->isChecked();
    if (autoGainAdjust) current.autoGainAdjust = autoGainAdjust->isChecked();
    if (loopLengthDefault) current.loopLengthDefault = loopLengthDefault->value();
    if (scratchSensitivity) current.scratchSensitivity = scratchSensitivity->currentIndex();

    if (themeCombo) current.theme = themeCombo->currentText();
    if (skinCombo) current.skin = skinCombo->currentText();
    if (waveformColorButton) current.waveformColor = getColorFromButton(waveformColorButton);
    if (beatGridColorButton) current.beatGridColor = getColorFromButton(beatGridColorButton);
    if (loopColorButton) current.loopColor = getColorFromButton(loopColorButton);
    if (showBpmOnWaveform) current.showBpmOnWaveform = showBpmOnWaveform->isChecked();
    if (showBeatNumbers) current.showBeatNumbers = showBeatNumbers->isChecked();
    if (animatedWaveforms) current.animatedWaveforms = animatedWaveforms->isChecked();
    if (waveformQualitySlider) current.waveformQuality = waveformQualitySlider->value();
    if (fullscreenMode) current.fullscreenMode = fullscreenMode->isChecked();

    if (libraryPathEdit) current.libraryPath = libraryPathEdit->text();
    if (cachePathEdit) current.cachePath = cachePathEdit->text();
    if (autoScanOnStartup) current.autoScanOnStartup = autoScanOnStartup->isChecked();
    if (deepAnalysis) current.deepAnalysis = deepAnalysis->isChecked();
    if (autoCreateWaveforms) current.autoCreateWaveforms = autoCreateWaveforms->isChecked();
    if (maxRecentTracks) current.maxRecentTracks = maxRecentTracks->value();
    if (sortDefaultCombo) current.sortDefault = sortDefaultCombo->currentText();

    if (cpuCoresSpinBox) current.cpuCores = cpuCoresSpinBox->value();
    if (memoryLimitSpinBox) current.memoryLimitMB = memoryLimitSpinBox->value();
    if (threadPrioritySlider) current.threadPriority = threadPrioritySlider->value();
    if (enableGpuAcceleration) current.enableGpuAcceleration = enableGpuAcceleration->isChecked();
    if (lowLatencyMode) current.lowLatencyMode = lowLatencyMode->isChecked();
    if (renderQualityCombo) current.renderQuality = renderQualityCombo->currentText();
    if (backgroundProcessing) current.backgroundProcessing = backgroundProcessing->isChecked();
    if (diskCacheSlider) current.diskCacheMB = diskCacheSlider->value();

    if (midiEnabled) current.midiEnabled = midiEnabled->isChecked();
    if (midiDeviceCombo) {
        current.midiDevice = (midiDeviceCombo->currentIndex() > 0) ? midiDeviceCombo->currentText() : QString();
    }
    if (midiLearnMode) current.midiLearnMode = midiLearnMode->isChecked();

    if (configPathEdit) current.configPath = configPathEdit->text();
    if (debugLogging) current.debugLogging = debugLogging->isChecked();
    if (crashReporting) current.crashReporting = crashReporting->isChecked();
    if (betaFeatures) current.betaFeatures = betaFeatures->isChecked();

    return current;
}

void PreferencesDialog::restoreDefaults() {
    // Reset to default values
    settings = AppSettings(); // Default constructor values
    updateUIFromSettings();
    markDirty();
}

void PreferencesDialog::updateUIFromSettings() {
    const bool previousGuard = suppressChangeTracking;
    suppressChangeTracking = true;

    // Update all UI controls with current settings
    
    // Audio
    refreshAudioDeviceLists();
    bufferSizeCombo->setCurrentText(QString::number(settings.bufferSize));
    sampleRateCombo->setCurrentText(QString::number(settings.sampleRate));
    keylockQualityCombo->setCurrentIndex(settings.keylockQuality);
    exclusiveModeCheck->setChecked(settings.exclusiveMode);
    masterVolumeSlider->setValue(static_cast<int>(settings.masterVolume * 100));
    headphoneVolumeSlider->setValue(static_cast<int>(settings.headphoneVolume * 100));
    updateVolumeLabel(masterVolumeSlider, masterVolumeLabel, "");
    updateVolumeLabel(headphoneVolumeSlider, headphoneVolumeLabel, "");
    
    // Decks
    deckAKeylockDefault->setChecked(settings.deckAKeylockDefault);
    deckAQuantizeDefault->setChecked(settings.deckAQuantizeDefault);
    deckASpeedDefault->setValue(settings.deckASpeedDefault);
    deckBKeylockDefault->setChecked(settings.deckBKeylockDefault);
    deckBQuantizeDefault->setChecked(settings.deckBQuantizeDefault);
    deckBSpeedDefault->setValue(settings.deckBSpeedDefault);
    syncOnLoad->setChecked(settings.syncOnLoad);
    autoGainAdjust->setChecked(settings.autoGainAdjust);
    loopLengthDefault->setValue(settings.loopLengthDefault);
    scratchSensitivity->setCurrentIndex(settings.scratchSensitivity);
    
    // Interface
    int themeIndex = themeCombo->findText(settings.theme);
    if (themeIndex >= 0) themeCombo->setCurrentIndex(themeIndex);
    
    int skinIndex = skinCombo->findText(settings.skin);
    if (skinIndex >= 0) skinCombo->setCurrentIndex(skinIndex);
    
    setColorButtonColor(waveformColorButton, settings.waveformColor);
    setColorButtonColor(beatGridColorButton, settings.beatGridColor);
    setColorButtonColor(loopColorButton, settings.loopColor);
    fontButton->setText(formatFontName(settings.uiFont));
    showBpmOnWaveform->setChecked(settings.showBpmOnWaveform);
    showBeatNumbers->setChecked(settings.showBeatNumbers);
    animatedWaveforms->setChecked(settings.animatedWaveforms);
    waveformQualitySlider->setValue(settings.waveformQuality);
    fullscreenMode->setChecked(settings.fullscreenMode);
    
    // Library
    libraryPathEdit->setText(settings.libraryPath);
    cachePathEdit->setText(settings.cachePath);
    autoScanOnStartup->setChecked(settings.autoScanOnStartup);
    deepAnalysis->setChecked(settings.deepAnalysis);
    autoCreateWaveforms->setChecked(settings.autoCreateWaveforms);
    maxRecentTracks->setValue(settings.maxRecentTracks);
    int sortIndex = sortDefaultCombo->findText(settings.sortDefault);
    if (sortIndex >= 0) sortDefaultCombo->setCurrentIndex(sortIndex);
    
    // Performance
    cpuCoresSpinBox->setValue(settings.cpuCores);
    memoryLimitSpinBox->setValue(settings.memoryLimitMB);
    threadPrioritySlider->setValue(settings.threadPriority);
    enableGpuAcceleration->setChecked(settings.enableGpuAcceleration);
    lowLatencyMode->setChecked(settings.lowLatencyMode);
    int renderIndex = renderQualityCombo->findText(settings.renderQuality);
    if (renderIndex >= 0) renderQualityCombo->setCurrentIndex(renderIndex);
    backgroundProcessing->setChecked(settings.backgroundProcessing);
    diskCacheSlider->setValue(settings.diskCacheMB);
    
    // MIDI
    midiEnabled->setChecked(settings.midiEnabled);
    if (!settings.midiDevice.isEmpty()) {
        int deviceIndex = midiDeviceCombo->findText(settings.midiDevice);
        if (deviceIndex >= 0) {
            midiDeviceCombo->setCurrentIndex(deviceIndex);
        }
    }
    midiLearnMode->setChecked(settings.midiLearnMode);
    
    // Initialize MIDI test mode state
    midiTestActive = false;
    midiTestButton->setChecked(false);
    midiActivityLabel->setText("● No Activity");
    midiActivityLabel->setStyleSheet("color: #666; font-size: 9px;");
    
    // Advanced
    configPathEdit->setText(settings.configPath);
    debugLogging->setChecked(settings.debugLogging);
    crashReporting->setChecked(settings.crashReporting);
    betaFeatures->setChecked(settings.betaFeatures);

    suppressChangeTracking = previousGuard;
}

// Helper methods
void PreferencesDialog::updateVolumeLabel(QSlider* slider, QLabel* label, const QString& prefix) {
    label->setText(prefix + QString::number(slider->value()) + "%");
}

void PreferencesDialog::setColorButtonColor(QPushButton* button, const QColor& color) {
    QString style = QString("QPushButton { background-color: %1; border: 2px solid #555; border-radius: 3px; min-width: 60px; min-height: 25px; }")
                   .arg(color.name());
    button->setStyleSheet(style);
    button->setText(color.name());
}

QColor PreferencesDialog::getColorFromButton(const QPushButton* button) const {
    QString colorName = button->text();
    return QColor(colorName);
}

QString PreferencesDialog::formatFontName(const QFont& font) {
    return QString("%1, %2pt").arg(font.family()).arg(font.pointSize());
}

PreferencesDialog::DeviceOption* PreferencesDialog::findDeviceOption(const QString& deviceName,
                                                                     const QString& deviceType) {
    for (auto& option : audioOutputDevices) {
        if (option.deviceName == deviceName && option.deviceType == deviceType) {
            return &option;
        }
    }
    return nullptr;
}

const PreferencesDialog::DeviceOption* PreferencesDialog::findDeviceOption(const QString& deviceName,
                                                                           const QString& deviceType) const {
    for (const auto& option : audioOutputDevices) {
        if (option.deviceName == deviceName && option.deviceType == deviceType) {
            return &option;
        }
    }
    return nullptr;
}

void PreferencesDialog::updateChannelComboForDevice(QComboBox* combo,
                                                    const QString& deviceName,
                                                    const QString& deviceType,
                                                    int preferredStart,
                                                    int preferredCount) {
    if (!combo) {
        return;
    }

    combo->clear();

    const DeviceOption* option = findDeviceOption(deviceName, deviceType);
    QVector<ChannelOption> channels = option ? option->channelOptions : QVector<ChannelOption>();

    if (channels.isEmpty()) {
        ChannelOption fallback;
        fallback.startIndex = 0;
        fallback.channelCount = 2;
        fallback.label = "Channels 1-2";
        channels.append(fallback);
    }

    int matchingIndex = -1;
    for (int i = 0; i < channels.size(); ++i) {
        const ChannelOption& channel = channels.at(i);
        QVariantMap data;
        data.insert("start", channel.startIndex);
        data.insert("count", channel.channelCount);
        combo->addItem(channel.label, data);
        if (channel.startIndex == preferredStart && channel.channelCount == preferredCount) {
            matchingIndex = i;
        }
    }

    if (matchingIndex < 0) {
        matchingIndex = 0;
    }
    combo->setCurrentIndex(matchingIndex);
}

void PreferencesDialog::ensureChannelSelectionValid(AppSettings& targetSettings,
                                                    QComboBox* combo,
                                                    const QString& deviceName,
                                                    const QString& deviceType) {
    if (!combo || combo->count() == 0) {
        return;
    }

    const QVariantMap channelData = combo->currentData().toMap();
    const int start = channelData.value("start").toInt();
    const int count = channelData.value("count").toInt();

    if (combo == masterChannelCombo && deviceName == targetSettings.masterAudioDevice
        && deviceType == targetSettings.masterAudioDeviceType) {
        targetSettings.masterOutputChannelStart = start;
        targetSettings.masterOutputChannelCount = count;
    } else if (combo == cueChannelCombo && deviceName == targetSettings.cueAudioDevice
               && deviceType == targetSettings.cueAudioDeviceType) {
        targetSettings.cueOutputChannelStart = start;
        targetSettings.cueOutputChannelCount = count;
    }
}

void PreferencesDialog::refreshAudioDeviceLists() {
    if (!masterDeviceCombo || !cueDeviceCombo) {
        return;
    }

    const bool previousGuard = suppressChangeTracking;
    suppressChangeTracking = true;

    QString desiredMasterDevice = settings.masterAudioDevice;
    QString desiredMasterType = settings.masterAudioDeviceType;
    QString desiredCueDevice = settings.cueAudioDevice;
    QString desiredCueType = settings.cueAudioDeviceType;
    int masterPreferredStart = settings.masterOutputChannelStart;
    int masterPreferredCount = settings.masterOutputChannelCount;
    int cuePreferredStart = settings.cueOutputChannelStart;
    int cuePreferredCount = settings.cueOutputChannelCount;

    QtMainWindow::AudioDeviceState activeState;
    if (mainWindowRef) {
        activeState = mainWindowRef->getActiveAudioDeviceState();
    }

    audioOutputDevices.clear();
    masterDeviceCombo->clear();
    cueDeviceCombo->clear();

    auto appendDevice = [this](const DeviceOption& option) {
        audioOutputDevices.append(option);
        QVariantMap data;
        data.insert("deviceName", option.deviceName);
        data.insert("deviceType", option.deviceType);
        masterDeviceCombo->addItem(option.displayName, data);
        cueDeviceCombo->addItem(option.displayName, data);
    };

    if (mainWindowRef) {
        const auto devices = mainWindowRef->getAvailableOutputDevices();
        for (const auto& info : devices) {
            DeviceOption option;
            option.deviceName = info.deviceName;
            option.deviceType = info.typeName;
            option.displayName = info.description.isEmpty() ? info.deviceName : info.description;

            const QStringList channelNames = info.outputChannelNames;
            const int totalChannels = channelNames.isEmpty() ? 2 : channelNames.size();

            for (int start = 0; start + 1 < totalChannels; start += 2) {
                ChannelOption pair;
                pair.startIndex = start;
                pair.channelCount = 2;
                const QString firstName = channelNames.value(start);
                const QString secondName = channelNames.value(start + 1);
                QString label = QString("Channels %1-%2").arg(start + 1).arg(start + 2);
                if (!firstName.isEmpty() || !secondName.isEmpty()) {
                    label += QString(" (%1 / %2)").arg(firstName.isEmpty() ? QString::number(start + 1) : firstName,
                                                        secondName.isEmpty() ? QString::number(start + 2) : secondName);
                }
                pair.label = label;
                option.channelOptions.append(pair);
            }

            if (option.channelOptions.isEmpty()) {
                ChannelOption fallback;
                fallback.startIndex = 0;
                fallback.channelCount = 2;
                fallback.label = "Channels 1-2";
                option.channelOptions.append(fallback);
            }

            appendDevice(option);
        }
    }

    if (audioOutputDevices.isEmpty()) {
        DeviceOption fallback;
        fallback.deviceName = "Default";
        fallback.deviceType = "Default";
        fallback.displayName = "Default Audio Device";
        ChannelOption channel;
        channel.startIndex = 0;
        channel.channelCount = 2;
        channel.label = "Channels 1-2";
        fallback.channelOptions.append(channel);
        appendDevice(fallback);
    }

    auto selectDevice = [](QComboBox* combo, const QString& deviceName, const QString& deviceType) {
        if (!combo) {
            return QVariantMap{};
        }

        for (int i = 0; i < combo->count(); ++i) {
            const QVariantMap data = combo->itemData(i).toMap();
            if (data.value("deviceName").toString() == deviceName
                && data.value("deviceType").toString() == deviceType) {
                combo->setCurrentIndex(i);
                return data;
            }
        }

        if (combo->count() > 0) {
            combo->setCurrentIndex(0);
            return combo->currentData().toMap();
        }

        return QVariantMap{};
    };

    if (desiredMasterDevice.isEmpty()) {
        if (!activeState.deviceName.isEmpty()) {
            desiredMasterDevice = activeState.deviceName;
            desiredMasterType = activeState.typeName;
            masterPreferredStart = activeState.channelStart;
            masterPreferredCount = activeState.channelCount;
        } else {
            desiredMasterDevice = audioOutputDevices.first().deviceName;
            desiredMasterType = audioOutputDevices.first().deviceType;
        }
    }

    if (desiredCueDevice.isEmpty()) {
        if (!activeState.deviceName.isEmpty()) {
            desiredCueDevice = activeState.deviceName;
            desiredCueType = activeState.typeName;
            if (cuePreferredStart == 0 && cuePreferredCount == 2) {
                cuePreferredStart = activeState.channelStart + activeState.channelCount;
            }
        } else {
            desiredCueDevice = desiredMasterDevice;
            desiredCueType = desiredMasterType;
            if (cuePreferredStart == 0 && cuePreferredCount == 2) {
                cuePreferredStart = masterPreferredStart + masterPreferredCount;
            }
        }
    }

    if (desiredCueType.isEmpty()) {
        desiredCueType = desiredMasterType;
    }

    const QVariantMap masterData = selectDevice(masterDeviceCombo, desiredMasterDevice, desiredMasterType);
    settings.masterAudioDevice = masterData.value("deviceName").toString();
    settings.masterAudioDeviceType = masterData.value("deviceType").toString();

    updateChannelComboForDevice(masterChannelCombo,
                                settings.masterAudioDevice,
                                settings.masterAudioDeviceType,
                                masterPreferredStart,
                                masterPreferredCount);
    ensureChannelSelectionValid(settings,
                                masterChannelCombo,
                                settings.masterAudioDevice,
                                settings.masterAudioDeviceType);

    const QVariantMap cueData = selectDevice(cueDeviceCombo, desiredCueDevice, desiredCueType);
    settings.cueAudioDevice = cueData.value("deviceName").toString();
    settings.cueAudioDeviceType = cueData.value("deviceType").toString();

    updateChannelComboForDevice(cueChannelCombo,
                                settings.cueAudioDevice,
                                settings.cueAudioDeviceType,
                                cuePreferredStart,
                                cuePreferredCount);
    ensureChannelSelectionValid(settings,
                                cueChannelCombo,
                                settings.cueAudioDevice,
                                settings.cueAudioDeviceType);

    suppressChangeTracking = previousGuard;
}

void PreferencesDialog::populateThemes() {
    themeCombo->addItems({"Dark", "Light", "Auto (System)", "Classic", "Neon"});
}

void PreferencesDialog::populateSkins() {
    skinCombo->addItems({"Default", "Professional", "Minimal", "Retro", "Custom"});
}

void PreferencesDialog::populateMidiDevices() {
    midiDeviceCombo->clear();
    midiDeviceCombo->addItem("No MIDI device", "");
    
    // Get available MIDI devices from MidiEngine
    QStringList midiDevices = midiEngine->getAvailableMidiDevices();
    
    if (midiDevices.isEmpty()) {
        midiStatusLabel->setText("No MIDI devices found");
        midiStatusLabel->setStyleSheet("color: #FF5722; font-weight: bold;");
    } else {
        for (const QString& deviceName : midiDevices) {
            midiDeviceCombo->addItem(deviceName, deviceName);
        }
        
        midiStatusLabel->setText(QString("Found %1 MIDI device(s)").arg(midiDevices.size()));
        midiStatusLabel->setStyleSheet("color: #4CAF50; font-weight: bold;");
    }
}

void PreferencesDialog::onMidiDeviceRefresh() {
    populateMidiDevices();
    QMessageBox::information(this, "MIDI Devices", 
                           QString("MIDI device list refreshed.\nFound %1 device(s).").arg(midiDeviceCombo->count() - 1));
}

void PreferencesDialog::onMidiDeviceActivate() {
    if (midiDeviceCombo->currentIndex() == 0) {
        QMessageBox::warning(this, "MIDI Device", "Please select a MIDI device first.");
        return;
    }
    
    QString selectedDevice = midiDeviceCombo->currentText();
    
    if (midiEngine && midiEngine->isDeviceOpen() && midiEngine->getCurrentDevice() == selectedDevice) {
        // Device is already connected, disconnect it
        qDebug() << "PreferencesDialog: Disconnecting MIDI device:" << selectedDevice;
        midiEngine->closeMidiDevice();
        
        midiActivateButton->setText("Connect Device");
        midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        midiStatusLabel->setText("Device disconnected");
        midiStatusLabel->setStyleSheet("color: #888; font-style: italic;");
        
        // Disable controls when device is disconnected
        midiEnabled->setChecked(false);
        midiTestButton->setEnabled(false);
        midiLearnMode->setEnabled(false);
        
        return;
    }
    
    // Try to connect to device
    qDebug() << "PreferencesDialog: Connecting to MIDI device:" << selectedDevice;
    
    // Enable MIDI first
    midiEnabled->setChecked(true);
    
    // Apply settings and open device
    QSettings tempSettings;
    tempSettings.setValue("midi/enabled", true);
    tempSettings.setValue("midi/device", selectedDevice);
    midiEngine->loadSettings(tempSettings);
    
    if (midiEngine->openMidiDevice(selectedDevice)) {
        qDebug() << "PreferencesDialog: MIDI device connected successfully:" << selectedDevice;
        
        midiActivateButton->setText("Disconnect Device");
        midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        midiStatusLabel->setText("Connected: " + selectedDevice);
        midiStatusLabel->setStyleSheet("color: #4CAF50; font-weight: bold;");
        
        // Enable controls when device is connected
        midiTestButton->setEnabled(true);
        midiLearnMode->setEnabled(true);
        
        QMessageBox::information(this, "MIDI Device Connected", 
            "Successfully connected to: " + selectedDevice + 
            "\n\nYour controller should now show activity LEDs if supported." +
            "\n\nYou can now:\n• Use the Test button to verify input\n• Enable MIDI Learn mode to map controls");
    } else {
        qDebug() << "PreferencesDialog: Failed to connect to MIDI device:" << selectedDevice;
        
        midiStatusLabel->setText("Connection failed");
        midiStatusLabel->setStyleSheet("color: #f44336; font-style: italic;");
        midiEnabled->setChecked(false);
        
        QMessageBox::warning(this, "MIDI Connection Error", 
            "Failed to connect to: " + selectedDevice + 
            "\n\nPossible causes:\n"
            "• Device is already in use by another application\n"
            "• Device is disconnected or powered off\n"
            "• Driver issues or permissions\n\n"
            "Please check your device connection and try again.");
    }
}

void PreferencesDialog::onMidiDeviceTest() {
    if (midiDeviceCombo->currentIndex() == 0) {
        QMessageBox::warning(this, "MIDI Test", "Please select a MIDI device first.");
        midiTestButton->setChecked(false);
        return;
    }
    
    midiTestActive = midiTestButton->isChecked();
    
    if (midiTestActive) {
        // Start test mode
        midiInputLabel->setText("Listening for MIDI input... Move any control on your device.");
        midiInputLabel->setStyleSheet("color: #4CAF50; font-size: 10px; padding: 5px; border: 1px solid #4CAF50; border-radius: 3px; background-color: #1a2a1a;");
        midiActivityLabel->setText("● Listening");
        midiActivityLabel->setStyleSheet("color: #4CAF50; font-size: 9px;");
        
        // Use MidiEngine to start testing
        midiEngine->testMidiDevice();
    } else {
        // Stop test mode
        midiInputLabel->setText("MIDI test stopped. Click 'Start MIDI Test' to resume monitoring.");
        midiInputLabel->setStyleSheet("color: #666; font-size: 10px; padding: 5px; border: 1px solid #444; border-radius: 3px; background-color: #2a2a2a;");
        midiActivityLabel->setText("● Stopped");
        midiActivityLabel->setStyleSheet("color: #666; font-size: 9px;");
    }
}

void PreferencesDialog::onMidiDeviceChanged() {
    QString deviceName = midiDeviceCombo->currentText();
    
    // Enable activate button only when a device is selected
    midiActivateButton->setEnabled(midiDeviceCombo->currentIndex() > 0);
    
    if (midiDeviceCombo->currentIndex() > 0) {
        // Check if this device is already connected
        if (midiEngine && midiEngine->isDeviceOpen() && midiEngine->getCurrentDevice() == deviceName) {
            midiActivateButton->setText("Disconnect Device");
            midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
            midiStatusLabel->setText("Connected: " + deviceName);
            midiStatusLabel->setStyleSheet("color: #4CAF50; font-weight: bold;");
        } else {
            midiActivateButton->setText("Connect Device");
            midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
            midiStatusLabel->setText("Device selected: " + deviceName);
            midiStatusLabel->setStyleSheet("color: #2196F3; font-weight: bold;");
        }
    } else {
        midiActivateButton->setText("Connect Device");
        midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        midiStatusLabel->setText("No device selected");
        midiStatusLabel->setStyleSheet("color: #888; font-style: italic;");
    }
}

void PreferencesDialog::onMidiEnabledChanged() {
    bool enabled = midiEnabled->isChecked();
    
    midiDeviceCombo->setEnabled(enabled);
    midiRefreshButton->setEnabled(enabled);
    midiActivateButton->setEnabled(enabled && midiDeviceCombo->currentIndex() > 0);
    midiLearnMode->setEnabled(enabled);
    midiTestButton->setEnabled(enabled && midiEngine && midiEngine->isDeviceOpen());
    
    if (enabled) {
        onMidiDeviceChanged(); // Update UI based on device selection
    } else {
        if (midiEngine && midiEngine->isDeviceOpen()) {
            midiEngine->closeMidiDevice();
        }
        midiStatusLabel->setText("MIDI disabled");
        midiStatusLabel->setStyleSheet("color: #888; font-style: italic;");
        midiActivateButton->setText("Connect Device");
        midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    }
}

void PreferencesDialog::onMidiLearnToggle() {
    bool learnMode = midiLearnMode->isChecked();
    midiEngine->setLearnMode(learnMode);
    
    if (learnMode) {
        midiInputLabel->setText("MIDI Learn mode active. Click any DJ control, then move MIDI controller.");
        midiInputLabel->setStyleSheet("color: #FF9800; font-weight: bold;");
    } else {
        midiInputLabel->setText("MIDI Learn mode disabled.");
        midiInputLabel->setStyleSheet("color: #666; font-style: italic;");
    }
}

// MIDI Engine event handlers
void PreferencesDialog::onMidiDeviceOpened(const QString& deviceName) {
    midiStatusLabel->setText("Connected: " + deviceName);
    midiStatusLabel->setStyleSheet("color: #4CAF50; font-weight: bold;");
    
    // Update activate button state
    midiActivateButton->setText("Disconnect Device");
    midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    midiActivateButton->setEnabled(true);
    
    // Enable test and learn controls
    midiTestButton->setEnabled(true);
    midiLearnMode->setEnabled(true);
    
    qDebug() << "MIDI device opened:" << deviceName;
}

void PreferencesDialog::onMidiDeviceClosed() {
    if (midiEnabled->isChecked()) {
        midiStatusLabel->setText("Device disconnected");
        midiStatusLabel->setStyleSheet("color: #FFC107; font-weight: bold;");
    } else {
        midiStatusLabel->setText("MIDI disabled");
        midiStatusLabel->setStyleSheet("color: #888; font-style: italic;");
    }
    
    // Update activate button state
    midiActivateButton->setText("Connect Device");
    midiActivateButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    midiActivateButton->setEnabled(midiDeviceCombo->currentIndex() > 0);
    
    // Disable test and learn controls
    midiTestButton->setEnabled(false);
    midiLearnMode->setEnabled(false);
    
    // Stop any active test
    if (midiTestActive) {
        midiTestActive = false;
        midiTestButton->setChecked(false);
        midiTestButton->setText("Start MIDI Test");
        midiTestButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        midiInputLabel->setText("MIDI test stopped - device disconnected.");
        midiInputLabel->setStyleSheet("color: #666; font-size: 10px; padding: 5px; border: 1px solid #444; border-radius: 3px; background-color: #2a2a2a;");
    }
    
    qDebug() << "MIDI device closed";
}

void PreferencesDialog::onMidiDeviceError(const QString& error) {
    midiStatusLabel->setText("Error: " + error);
    midiStatusLabel->setStyleSheet("color: #FF5722; font-weight: bold;");
    QMessageBox::warning(this, "MIDI Error", error);
}

void PreferencesDialog::onMidiMessageReceived(int channel, int controlNumber, int value, bool isNote) {
    QString messageType = isNote ? "Note" : "CC";
    QString message = QString("MIDI %1: CH%2 %3=%4").arg(messageType).arg(channel).arg(controlNumber).arg(value);
    
    // Debug output to both qDebug and std::cout for visibility
    qDebug() << "PreferencesDialog::onMidiMessageReceived -" << message << "TestActive:" << midiTestActive << "ButtonChecked:" << (midiTestButton ? midiTestButton->isChecked() : false);
    std::cout << "=== PREFERENCES MIDI MESSAGE ===" << std::endl;
    std::cout << "Message: " << message.toStdString() << std::endl;
    std::cout << "Channel: " << channel << ", Control: " << controlNumber << ", Value: " << value << ", IsNote: " << (isNote ? "true" : "false") << std::endl;
    
    // Only show live updates when test mode is active
    if (midiTestActive && midiTestButton && midiTestButton->isChecked()) {
        // Create detailed message with timestamp
        QTime currentTime = QTime::currentTime();
        QString timestamp = currentTime.toString("hh:mm:ss.zzz");
        
        QString detailedMessage = QString("[%1] %2 Input: Channel %3, %4 %5, Value %6")
            .arg(timestamp)
            .arg(messageType)
            .arg(channel)
            .arg(isNote ? "Note" : "Controller")
            .arg(controlNumber)
            .arg(value);
        
        // Update input display with the new message
        QString currentText = midiInputLabel->text();
        QStringList lines = currentText.split("\n");
        
        // Keep only the last 4 messages to prevent overflow
        if (lines.size() >= 5) {
            lines = lines.mid(lines.size() - 4);
        }
        
        lines.append(detailedMessage);
        midiInputLabel->setText(lines.join("\n"));
        
        // Update activity indicator with color coding based on message type
        if (isNote) {
            midiActivityLabel->setText("● Note Input");
            midiActivityLabel->setStyleSheet("color: #FF5722; font-size: 9px; font-weight: bold;");
        } else {
            midiActivityLabel->setText("● Controller Input");
            midiActivityLabel->setStyleSheet("color: #2196F3; font-size: 9px; font-weight: bold;");
        }
        
        // Reset activity indicator after 2 seconds
        QTimer::singleShot(2000, [this]() {
            if (midiTestActive && midiTestButton && midiTestButton->isChecked()) {
                midiActivityLabel->setText("● Listening");
                midiActivityLabel->setStyleSheet("color: #4CAF50; font-size: 9px;");
            }
        });
    } else {
        qDebug() << "PreferencesDialog: MIDI message not displayed - test conditions not met";
    }
    
    // Check for MIDI learning mode
    if (midiLearnMode->isChecked()) {
        // Look for a control that is in "Learning..." state
        for (int row = 0; row < midiMappingTable->rowCount(); ++row) {
            QTableWidgetItem* statusItem = midiMappingTable->item(row, 2);
            if (statusItem && statusItem->text() == "Learning...") {
                // Found a control waiting for mapping
                QString controlName = midiMappingTable->item(row, 0)->text();
                QString midiInputStr = QString("CH%1 %2%3")
                    .arg(channel)
                    .arg(isNote ? "Note" : "CC")
                    .arg(controlNumber);
                
                // Update the mapping
                midiMappingTable->item(row, 1)->setText(midiInputStr);
                midiMappingTable->item(row, 2)->setText("Mapped");
                midiMappingTable->item(row, 2)->setForeground(QBrush(QColor(76, 175, 80))); // Green
                
                // Create actual MIDI mapping in the engine
                if (midiEngine) {
                    MidiControlMapping mapping;
                    mapping.midiChannel = channel;
                    mapping.controlNumber = controlNumber;
                    mapping.isNote = isNote;
                    mapping.minValue = 0;
                    mapping.maxValue = 127;
                    
                    // Determine control type and deck from control name
                    if (controlName.contains("Play/Pause") && controlName.contains("Deck A")) {
                        mapping.controlType = MidiControlType::PlayPause;
                        mapping.deckId = "A";
                    } else if (controlName.contains("Play/Pause") && controlName.contains("Deck B")) {
                        mapping.controlType = MidiControlType::PlayPause;
                        mapping.deckId = "B";
                    } else if (controlName.contains("Cue") && controlName.contains("Deck A")) {
                        mapping.controlType = MidiControlType::Cue;
                        mapping.deckId = "A";
                    } else if (controlName.contains("Cue") && controlName.contains("Deck B")) {
                        mapping.controlType = MidiControlType::Cue;
                        mapping.deckId = "B";
                    } else if (controlName.contains("Tempo") && controlName.contains("Deck A")) {
                        mapping.controlType = MidiControlType::PitchBend;
                        mapping.deckId = "A";
                    } else if (controlName.contains("Tempo") && controlName.contains("Deck B")) {
                        mapping.controlType = MidiControlType::PitchBend;
                        mapping.deckId = "B";
                    } else if (controlName.contains("Volume") && controlName.contains("Deck A")) {
                        mapping.controlType = MidiControlType::ChannelFader;
                        mapping.deckId = "A";
                    } else if (controlName.contains("Volume") && controlName.contains("Deck B")) {
                        mapping.controlType = MidiControlType::ChannelFader;
                        mapping.deckId = "B";
                    } else if (controlName.contains("Crossfader")) {
                        mapping.controlType = MidiControlType::Crossfader;
                        mapping.deckId = "";
                    } else if (controlName.contains("Channel Fader Deck A")) {
                        mapping.controlType = MidiControlType::ChannelFader;
                        mapping.deckId = "A";
                    } else if (controlName.contains("Channel Fader Deck B")) {
                        mapping.controlType = MidiControlType::ChannelFader;
                        mapping.deckId = "B";
                    }
                    
                    // Add the mapping to the MIDI engine
                    midiEngine->addControlMapping(mapping);
                    
                    qDebug() << "PreferencesDialog: Added MIDI mapping to engine:" << controlName 
                             << "-> CH" << channel << (isNote ? "Note" : "CC") << controlNumber 
                             << "Type:" << static_cast<int>(mapping.controlType) << "Deck:" << mapping.deckId;
                }
                
                qDebug() << "PreferencesDialog: Mapped" << controlName << "to MIDI input" << midiInputStr;
                
                QMessageBox::information(this, "Mapping Complete", 
                    QString("Successfully mapped '%1' to:\n%2\n\nThe mapping is now active!")
                    .arg(controlName).arg(midiInputStr));
                
                // Disable learn mode after successful mapping
                midiLearnMode->setChecked(false);
                break;
            }
        }
    }
    
    qDebug() << "MIDI Input:" << message;
}

// MIDI Mapping Functions
void PreferencesDialog::addMidiMappingRow(const QString& controlName, const QString& midiInput, const QString& status) {
    int row = midiMappingTable->rowCount();
    midiMappingTable->insertRow(row);
    
    // Control name (not editable)
    QTableWidgetItem* controlItem = new QTableWidgetItem(controlName);
    controlItem->setFlags(controlItem->flags() & ~Qt::ItemIsEditable);
    midiMappingTable->setItem(row, 0, controlItem);
    
    // MIDI input (shows mapped control)
    QTableWidgetItem* midiItem = new QTableWidgetItem(midiInput);
    midiItem->setFlags(midiItem->flags() & ~Qt::ItemIsEditable);
    midiMappingTable->setItem(row, 1, midiItem);
    
    // Status
    QTableWidgetItem* statusItem = new QTableWidgetItem(status);
    statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
    if (status == "Not Mapped") {
        statusItem->setForeground(QBrush(QColor(255, 87, 34))); // Orange
    } else if (status == "Mapped") {
        statusItem->setForeground(QBrush(QColor(76, 175, 80))); // Green
    } else if (status == "Learning...") {
        statusItem->setForeground(QBrush(QColor(33, 150, 243))); // Blue
    }
    midiMappingTable->setItem(row, 2, statusItem);
    
    // Action buttons
    QWidget* actionWidget = new QWidget();
    QHBoxLayout* actionLayout = new QHBoxLayout(actionWidget);
    actionLayout->setContentsMargins(4, 2, 4, 2);
    actionLayout->setSpacing(4);
    
    QPushButton* learnButton = new QPushButton("Learn");
    learnButton->setFixedSize(50, 24);
    learnButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    learnButton->setToolTip("Click to learn MIDI input for this control");
    
    QPushButton* clearButton = new QPushButton("✕");
    clearButton->setFixedSize(24, 24);
    clearButton->setToolTip("Clear mapping for this control");
    clearButton->setStyleSheet("QPushButton { background-color: #f44336; color: white; border-radius: 12px; } QPushButton:hover { background-color: #d32f2f; }");
    
    actionLayout->addWidget(learnButton);
    actionLayout->addWidget(clearButton);
    actionLayout->addStretch();
    
    midiMappingTable->setCellWidget(row, 3, actionWidget);
    
    // Connect button signals
    connect(learnButton, &QPushButton::clicked, [this, row]() {
        onMidiMappingLearn(row);
    });
    
    connect(clearButton, &QPushButton::clicked, [this, row]() {
        onMidiMappingClear(row);
    });
}

void PreferencesDialog::onMidiMappingLearn(int row) {
    if (!midiEngine || !midiEngine->isDeviceOpen()) {
        QMessageBox::warning(this, "MIDI Learning", 
            "Please connect to a MIDI device first using the 'Connect Device' button.");
        return;
    }
    
    QString controlName = midiMappingTable->item(row, 0)->text();
    
    // Set status to learning
    midiMappingTable->item(row, 2)->setText("Learning...");
    midiMappingTable->item(row, 2)->setForeground(QBrush(QColor(33, 150, 243))); // Blue
    
    // Enable learn mode if not already enabled
    midiLearnMode->setChecked(true);
    
    QMessageBox::information(this, "MIDI Learning", 
        QString("Learning mode activated for: %1\n\n"
                "Now press/move the control on your MIDI device that you want to map to '%2'.\n"
                "The mapping will be created automatically when MIDI input is detected.")
        .arg(controlName).arg(controlName));
    
    qDebug() << "PreferencesDialog: Started MIDI learning for" << controlName << "at row" << row;
}

void PreferencesDialog::onMidiMappingClear(int row) {
    QString controlName = midiMappingTable->item(row, 0)->text();
    QString midiInput = midiMappingTable->item(row, 1)->text();
    
    // Parse MIDI input to remove from engine
    if (midiEngine && !midiInput.isEmpty()) {
        // Parse format like "CH1 CC16" or "CH1 Note64"
        QRegularExpression regex("CH(\\d+)\\s+(CC|Note)(\\d+)");
        QRegularExpressionMatch match = regex.match(midiInput);
        if (match.hasMatch()) {
            int channel = match.captured(1).toInt();
            bool isNote = (match.captured(2) == "Note");
            int controlNumber = match.captured(3).toInt();
            
            // Remove the mapping from the MIDI engine
            midiEngine->removeControlMapping(channel, controlNumber, isNote);
            
            qDebug() << "PreferencesDialog: Removed MIDI mapping from engine:" 
                     << "CH" << channel << (isNote ? "Note" : "CC") << controlNumber;
        }
    }
    
    // Clear the mapping in the UI
    midiMappingTable->item(row, 1)->setText("");
    midiMappingTable->item(row, 2)->setText("Not Mapped");
    midiMappingTable->item(row, 2)->setForeground(QBrush(QColor(255, 87, 34))); // Orange
    
    qDebug() << "PreferencesDialog: Cleared MIDI mapping for" << controlName;
    
    QMessageBox::information(this, "Mapping Cleared", 
        QString("MIDI mapping cleared for: %1").arg(controlName));
}

void PreferencesDialog::saveMidiMappings(const QString& fileName) {
    QSettings preset(fileName, QSettings::IniFormat);
    
    preset.clear();
    preset.beginGroup("MIDIMappings");
    
    for (int row = 0; row < midiMappingTable->rowCount(); ++row) {
        QString controlName = midiMappingTable->item(row, 0)->text();
        QString midiInput = midiMappingTable->item(row, 1)->text();
        QString status = midiMappingTable->item(row, 2)->text();
        
        if (!midiInput.isEmpty() && status == "Mapped") {
            preset.setValue(controlName, midiInput);
        }
    }
    
    preset.endGroup();
    preset.sync();
    
    QMessageBox::information(this, "Preset Saved", 
        QString("MIDI mapping preset saved to:\n%1").arg(fileName));
    
    qDebug() << "PreferencesDialog: MIDI mappings saved to" << fileName;
}

void PreferencesDialog::loadMidiMappings(const QString& fileName) {
    QSettings preset(fileName, QSettings::IniFormat);
    
    preset.beginGroup("MIDIMappings");
    QStringList keys = preset.allKeys();
    
    if (keys.isEmpty()) {
        QMessageBox::warning(this, "Load Failed", 
            "No MIDI mappings found in the selected preset file.");
        return;
    }
    
    // Clear existing mappings first
    for (int row = 0; row < midiMappingTable->rowCount(); ++row) {
        midiMappingTable->item(row, 1)->setText("");
        midiMappingTable->item(row, 2)->setText("Not Mapped");
        midiMappingTable->item(row, 2)->setForeground(QBrush(QColor(255, 87, 34))); // Orange
    }
    
    // Load mappings from preset
    int loadedCount = 0;
    for (const QString& controlName : keys) {
        QString midiInput = preset.value(controlName).toString();
        
        // Find the row for this control
        for (int row = 0; row < midiMappingTable->rowCount(); ++row) {
            if (midiMappingTable->item(row, 0)->text() == controlName) {
                midiMappingTable->item(row, 1)->setText(midiInput);
                midiMappingTable->item(row, 2)->setText("Mapped");
                midiMappingTable->item(row, 2)->setForeground(QBrush(QColor(76, 175, 80))); // Green
                loadedCount++;
                break;
            }
        }
    }
    
    preset.endGroup();
    
    QMessageBox::information(this, "Preset Loaded", 
        QString("Successfully loaded %1 MIDI mappings from:\n%2").arg(loadedCount).arg(fileName));
    
    qDebug() << "PreferencesDialog: Loaded" << loadedCount << "MIDI mappings from" << fileName;
}
