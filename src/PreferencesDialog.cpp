#include "PreferencesDialog.h"
#include <QApplication>
#include <QScreen>
#include <QHeaderView>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

PreferencesDialog::PreferencesDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("BetaPulseX - Preferences");
    setWindowIcon(QIcon(":/icons/settings.png"));
    setModal(true);
    
    // Dialog size
    resize(800, 600);
    
    // Center on screen
    if (parent) {
        move(parent->geometry().center() - rect().center());
    } else {
        QScreen* screen = QApplication::primaryScreen();
        move(screen->geometry().center() - rect().center());
    }
    
    setupUI();
    loadSettings();
    originalSettings = settings; // Backup für Cancel
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
    QGroupBox* deviceGroup = new QGroupBox("Audio Device");
    QFormLayout* deviceLayout = new QFormLayout(deviceGroup);
    
    audioDeviceCombo = new QComboBox();
    populateAudioDevices();
    deviceLayout->addRow("Device:", audioDeviceCombo);
    
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
    
    // Connect signals
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
        }
    });
    
    connect(beatGridColorButton, &QPushButton::clicked, [this]() {
        QColor color = QColorDialog::getColor(getColorFromButton(beatGridColorButton), this, "Beat Grid Color");
        if (color.isValid()) {
            setColorButtonColor(beatGridColorButton, color);
        }
    });
    
    connect(loopColorButton, &QPushButton::clicked, [this]() {
        QColor color = QColorDialog::getColor(getColorFromButton(loopColorButton), this, "Loop Color");
        if (color.isValid()) {
            setColorButtonColor(loopColorButton, color);
        }
    });
    
    connect(fontButton, &QPushButton::clicked, [this]() {
        bool ok;
        QFont font = QFontDialog::getFont(&ok, settings.uiFont, this, "Select UI Font");
        if (ok) {
            settings.uiFont = font;
            fontButton->setText(formatFontName(font));
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
                                                       "BetaPulseX_Settings.json", 
                                                       "JSON Files (*.json)");
        if (!fileName.isEmpty()) {
            saveSettings(); // Ensure current settings are saved
            QMessageBox::information(this, "Export Complete", 
                                   "Settings exported successfully to:\n" + fileName);
        }
    });
    
    connect(importSettingsButton, &QPushButton::clicked, [this]() {
        QString fileName = QFileDialog::getOpenFileName(this, "Import Settings", 
                                                       "", "JSON Files (*.json)");
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
    saveSettings();
    applySettings();
    accept();
}

void PreferencesDialog::onCancelClicked() {
    settings = originalSettings; // Restore original settings
    reject();
}

void PreferencesDialog::onApplyClicked() {
    saveSettings();
    applySettings();
    originalSettings = settings; // Update backup
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
void PreferencesDialog::onAudioDeviceChanged() {
    // Implementation for audio device change
}

void PreferencesDialog::onBufferSizeChanged(int size) {
    // Implementation for buffer size change
}

void PreferencesDialog::onSampleRateChanged() {
    // Implementation for sample rate change
}

void PreferencesDialog::onKeylockQualityChanged() {
    // Implementation for keylock quality change
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
    QSettings config(AppConfig::instance().getConfigDirectory() + "/preferences.ini", QSettings::IniFormat);
    
    // Load Audio settings
    settings.audioDevice = config.value("Audio/Device", "").toString();
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
    
    // Load Advanced settings
    settings.configPath = config.value("Advanced/ConfigPath", AppConfig::instance().getConfigDirectory()).toString();
    settings.debugLogging = config.value("Advanced/DebugLogging", false).toBool();
    settings.crashReporting = config.value("Advanced/CrashReporting", true).toBool();
    settings.betaFeatures = config.value("Advanced/BetaFeatures", false).toBool();
    
    // Update UI controls with loaded settings
    updateUIFromSettings();
}

void PreferencesDialog::saveSettings() {
    QSettings config(AppConfig::instance().getConfigDirectory() + "/preferences.ini", QSettings::IniFormat);
    
    // Save Audio settings
    config.setValue("Audio/Device", audioDeviceCombo->currentText());
    config.setValue("Audio/BufferSize", bufferSizeCombo->currentText().toInt());
    config.setValue("Audio/SampleRate", sampleRateCombo->currentText().toInt());
    config.setValue("Audio/KeylockQuality", keylockQualityCombo->currentIndex());
    config.setValue("Audio/ExclusiveMode", exclusiveModeCheck->isChecked());
    config.setValue("Audio/MasterVolume", masterVolumeSlider->value() / 100.0);
    config.setValue("Audio/HeadphoneVolume", headphoneVolumeSlider->value() / 100.0);
    
    // Save Deck settings
    config.setValue("Decks/DeckAKeylockDefault", deckAKeylockDefault->isChecked());
    config.setValue("Decks/DeckAQuantizeDefault", deckAQuantizeDefault->isChecked());
    config.setValue("Decks/DeckASpeedDefault", deckASpeedDefault->value());
    config.setValue("Decks/DeckBKeylockDefault", deckBKeylockDefault->isChecked());
    config.setValue("Decks/DeckBQuantizeDefault", deckBQuantizeDefault->isChecked());
    config.setValue("Decks/DeckBSpeedDefault", deckBSpeedDefault->value());
    config.setValue("Decks/SyncOnLoad", syncOnLoad->isChecked());
    config.setValue("Decks/AutoGainAdjust", autoGainAdjust->isChecked());
    config.setValue("Decks/LoopLengthDefault", loopLengthDefault->value());
    config.setValue("Decks/ScratchSensitivity", scratchSensitivity->currentIndex());
    
    // Save Interface settings
    config.setValue("Interface/Theme", themeCombo->currentText());
    config.setValue("Interface/Skin", skinCombo->currentText());
    config.setValue("Interface/WaveformColor", getColorFromButton(waveformColorButton));
    config.setValue("Interface/BeatGridColor", getColorFromButton(beatGridColorButton));
    config.setValue("Interface/LoopColor", getColorFromButton(loopColorButton));
    config.setValue("Interface/ShowBpmOnWaveform", showBpmOnWaveform->isChecked());
    config.setValue("Interface/ShowBeatNumbers", showBeatNumbers->isChecked());
    config.setValue("Interface/AnimatedWaveforms", animatedWaveforms->isChecked());
    config.setValue("Interface/WaveformQuality", waveformQualitySlider->value());
    config.setValue("Interface/FullscreenMode", fullscreenMode->isChecked());
    
    // Save Library settings
    config.setValue("Library/Path", libraryPathEdit->text());
    config.setValue("Library/CachePath", cachePathEdit->text());
    config.setValue("Library/AutoScanOnStartup", autoScanOnStartup->isChecked());
    config.setValue("Library/DeepAnalysis", deepAnalysis->isChecked());
    config.setValue("Library/AutoCreateWaveforms", autoCreateWaveforms->isChecked());
    config.setValue("Library/MaxRecentTracks", maxRecentTracks->value());
    config.setValue("Library/SortDefault", sortDefaultCombo->currentText());
    
    // Save Performance settings
    config.setValue("Performance/CpuCores", cpuCoresSpinBox->value());
    config.setValue("Performance/MemoryLimitMB", memoryLimitSpinBox->value());
    config.setValue("Performance/ThreadPriority", threadPrioritySlider->value());
    config.setValue("Performance/EnableGpuAcceleration", enableGpuAcceleration->isChecked());
    config.setValue("Performance/LowLatencyMode", lowLatencyMode->isChecked());
    config.setValue("Performance/RenderQuality", renderQualityCombo->currentText());
    config.setValue("Performance/BackgroundProcessing", backgroundProcessing->isChecked());
    config.setValue("Performance/DiskCacheMB", diskCacheSlider->value());
    
    // Save Advanced settings
    config.setValue("Advanced/ConfigPath", configPathEdit->text());
    config.setValue("Advanced/DebugLogging", debugLogging->isChecked());
    config.setValue("Advanced/CrashReporting", crashReporting->isChecked());
    config.setValue("Advanced/BetaFeatures", betaFeatures->isChecked());
    
    config.sync();
}

void PreferencesDialog::applySettings() {
    // Apply settings to the application
    // This would be connected to the main application to apply changes
    emit settingsChanged();
}

void PreferencesDialog::restoreDefaults() {
    // Reset to default values
    settings = AppSettings(); // Default constructor values
    updateUIFromSettings();
}

void PreferencesDialog::updateUIFromSettings() {
    // Update all UI controls with current settings
    
    // Audio
    if (!settings.audioDevice.isEmpty()) {
        int index = audioDeviceCombo->findText(settings.audioDevice);
        if (index >= 0) audioDeviceCombo->setCurrentIndex(index);
    }
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
    
    // Advanced
    configPathEdit->setText(settings.configPath);
    debugLogging->setChecked(settings.debugLogging);
    crashReporting->setChecked(settings.crashReporting);
    betaFeatures->setChecked(settings.betaFeatures);
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

QColor PreferencesDialog::getColorFromButton(QPushButton* button) {
    QString colorName = button->text();
    return QColor(colorName);
}

QString PreferencesDialog::formatFontName(const QFont& font) {
    return QString("%1, %2pt").arg(font.family()).arg(font.pointSize());
}

void PreferencesDialog::populateAudioDevices() {
    audioDeviceCombo->addItem("Default Audio Device");
    audioDeviceCombo->addItem("ASIO Driver");
    audioDeviceCombo->addItem("DirectSound");
    audioDeviceCombo->addItem("WASAPI");
    // Add more devices as detected by the system
}

void PreferencesDialog::populateThemes() {
    themeCombo->addItems({"Dark", "Light", "Auto (System)", "Classic", "Neon"});
}

void PreferencesDialog::populateSkins() {
    skinCombo->addItems({"Default", "Professional", "Minimal", "Retro", "Custom"});
}
