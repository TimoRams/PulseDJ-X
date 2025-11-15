#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QSlider>
#include <QPushButton>
#include <QLineEdit>
#include <QVector>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QColorDialog>
#include <QFontDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QBrush>
#include <QSettings>
#include <QRegularExpression>
#include "DeckSettings.h"
#include "MidiEngine.h"

// Forward declaration
class QtMainWindow;
#include "AppConfig.h"
#include "MidiEngine.h"

/**
 * BetaPulseX Preferences Dialog
 * 
 * Umfassendes Settings-Fenster mit Tabs für:
 * - Audio Settings (Devices, Latency, Quality)
 * - Deck Settings (Keylock, Quantize, etc.)
 * - Interface Settings (Theme, Colors, Fonts)
 * - Library Settings (Paths, Scanning, Cache)
 * - Performance Settings (CPU, Memory, Threads)
 */
class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget* parent = nullptr);
    ~PreferencesDialog() = default;
    
    // Set references to players and main window for MIDI integration
    void setPlayerReferences(DJAudioPlayer* playerA, DJAudioPlayer* playerB, QtMainWindow* mainWindow);

signals:
    void settingsChanged();

private slots:
    void onOkClicked();
    void onCancelClicked();
    void onApplyClicked();
    void onRestoreDefaultsClicked();
    
    // Audio Settings
    void onMasterDeviceChanged();
    void onMasterChannelChanged();
    void onCueDeviceChanged();
    void onCueChannelChanged();
    void onBufferSizeChanged(int size);
    void onSampleRateChanged();
    void onKeylockQualityChanged();
    
    // Interface Settings
    void onThemeChanged();
    void onWaveformColorChanged();
    void onBeatGridColorChanged();
    void onFontChanged();
    
    // Library Settings
    void onLibraryPathChanged();
    void onCachePathChanged();
    void onRescanLibrary();
    void onClearCache();
    
    // Performance Settings
    void onCpuCoresChanged(int cores);
    void onMemoryLimitChanged(int limitMB);

private:
    void setupUI();
    void createAudioTab();
    void createDeckTab();
    void createInterfaceTab();
    void createLibraryTab();
    void createPerformanceTab();
    void createMidiTab();
    void createAdvancedTab();
    
    void loadSettings();
    void saveSettings();
    void applySettings();
    void restoreDefaults();
    void updateUIFromSettings();
    
    // Main layout
    QTabWidget* tabWidget;
    QVBoxLayout* mainLayout;
    
    // Button box
    QHBoxLayout* buttonLayout;
    QPushButton* okButton;
    QPushButton* cancelButton;
    QPushButton* applyButton;
    QPushButton* defaultsButton;
    
    // === AUDIO TAB ===
    QWidget* audioTab;
    QComboBox* masterDeviceCombo;
    QComboBox* masterChannelCombo;
    QComboBox* cueDeviceCombo;
    QComboBox* cueChannelCombo;
    QComboBox* bufferSizeCombo;
    QComboBox* sampleRateCombo;
    QComboBox* keylockQualityCombo;
    QCheckBox* exclusiveModeCheck;
    QSlider* masterVolumeSlider;
    QLabel* masterVolumeLabel;
    QSlider* headphoneVolumeSlider;
    QLabel* headphoneVolumeLabel;
    
    // === DECK TAB ===
    QWidget* deckTab;
    QCheckBox* deckAKeylockDefault;
    QCheckBox* deckAQuantizeDefault;
    QDoubleSpinBox* deckASpeedDefault;
    QCheckBox* deckBKeylockDefault;
    QCheckBox* deckBQuantizeDefault;
    QDoubleSpinBox* deckBSpeedDefault;
    QCheckBox* syncOnLoad;
    QCheckBox* autoGainAdjust;
    QSpinBox* loopLengthDefault;
    QComboBox* scratchSensitivity;
    
    // === INTERFACE TAB ===
    QWidget* interfaceTab;
    QComboBox* themeCombo;
    QComboBox* skinCombo;
    QPushButton* waveformColorButton;
    QPushButton* beatGridColorButton;
    QPushButton* loopColorButton;
    QPushButton* fontButton;
    QCheckBox* showBpmOnWaveform;
    QCheckBox* showBeatNumbers;
    QCheckBox* animatedWaveforms;
    QSlider* waveformQualitySlider;
    QCheckBox* fullscreenMode;
    
    // === LIBRARY TAB ===
    QWidget* libraryTab;
    QLineEdit* libraryPathEdit;
    QPushButton* libraryPathButton;
    QLineEdit* cachePathEdit;
    QPushButton* cachePathButton;
    QCheckBox* autoScanOnStartup;
    QCheckBox* deepAnalysis;
    QCheckBox* autoCreateWaveforms;
    QPushButton* rescanButton;
    QPushButton* clearCacheButton;
    QSpinBox* maxRecentTracks;
    QComboBox* sortDefaultCombo;
    
    // === PERFORMANCE TAB ===
    QWidget* performanceTab;
    QSpinBox* cpuCoresSpinBox;
    QSpinBox* memoryLimitSpinBox;
    QSlider* threadPrioritySlider;
    QCheckBox* enableGpuAcceleration;
    QCheckBox* lowLatencyMode;
    QComboBox* renderQualityCombo;
    QCheckBox* backgroundProcessing;
    QSlider* diskCacheSlider;
    
    // === MIDI TAB ===
    QWidget* midiTab;
    QComboBox* midiDeviceCombo;
    QCheckBox* midiEnabled;
    QLabel* midiStatusLabel;
    QPushButton* midiRefreshButton;
    QPushButton* midiActivateButton;
    QCheckBox* midiLearnMode;
    QTableWidget* midiMappingTable;
    QPushButton* midiTestButton;
    QLabel* midiInputLabel;
    QLabel* midiActivityLabel;
    bool midiTestActive = false;
    
    // === ADVANCED TAB ===
    QWidget* advancedTab;
    QLineEdit* configPathEdit;
    QPushButton* configPathButton;
    QCheckBox* debugLogging;
    QCheckBox* crashReporting;
    QCheckBox* betaFeatures;
    QPushButton* exportSettingsButton;
    QPushButton* importSettingsButton;
    QPushButton* resetAllButton;
    
    // Settings storage
    struct AppSettings {
        // Audio
        QString masterAudioDevice;
        QString masterAudioDeviceType;
        int masterOutputChannelStart = 0;   // zero-based channel index
        int masterOutputChannelCount = 2;   // number of channels requested (default stereo)
        QString cueAudioDevice;
        QString cueAudioDeviceType;
        int cueOutputChannelStart = 0;
        int cueOutputChannelCount = 2;
        int bufferSize = 512;
        int sampleRate = 44100;
        int keylockQuality = 1; // 0=Fast, 1=Balanced, 2=Quality
        bool exclusiveMode = false;
        double masterVolume = 0.8;
        double headphoneVolume = 0.7;
        
        // Decks
        bool deckAKeylockDefault = false;
        bool deckAQuantizeDefault = false;
        double deckASpeedDefault = 1.0;
        bool deckBKeylockDefault = false;
        bool deckBQuantizeDefault = false;
        double deckBSpeedDefault = 1.0;
        bool syncOnLoad = false;
        bool autoGainAdjust = true;
        int loopLengthDefault = 4;
        int scratchSensitivity = 50;
        
        // Interface
        QString theme = "Dark";
        QString skin = "Default";
        QColor waveformColor = QColor(0, 200, 255);
        QColor beatGridColor = QColor(255, 255, 255, 100);
        QColor loopColor = QColor(255, 165, 0);
        QFont uiFont;
        bool showBpmOnWaveform = true;
        bool showBeatNumbers = false;
        bool animatedWaveforms = true;
        int waveformQuality = 75;
        bool fullscreenMode = false;
        
        // Library
        QString libraryPath;
        QString cachePath;
        bool autoScanOnStartup = true;
        bool deepAnalysis = true;
        bool autoCreateWaveforms = true;
        int maxRecentTracks = 20;
        QString sortDefault = "Artist";
        
        // Performance
        int cpuCores = -1; // -1 = auto-detect
        int memoryLimitMB = 1024;
        int threadPriority = 50;
        bool enableGpuAcceleration = true;
        bool lowLatencyMode = false;
        QString renderQuality = "High";
        bool backgroundProcessing = true;
        int diskCacheMB = 256;
        
        // MIDI
        bool midiEnabled = false;
        QString midiDevice = "";
        bool midiLearnMode = false;
        
        // Advanced
        QString configPath;
        bool debugLogging = false;
        bool crashReporting = true;
        bool betaFeatures = false;
        bool operator==(const AppSettings& other) const = default;
        bool operator!=(const AppSettings& other) const = default;
    } settings;
    
    AppSettings originalSettings; // Für Cancel-Funktionalität
    
    // MIDI Engine
    MidiEngine* midiEngine;
    
    // Main Window Reference
    QtMainWindow* mainWindowRef;

    // Change tracking helpers
    AppSettings collectCurrentSettings() const;
    void installChangeTracking();
    void markDirty();
    void updateApplyButtonState();
    bool pendingChanges = false;
    bool suppressChangeTracking = false;
    
    // Helper methods
    void updateVolumeLabel(QSlider* slider, QLabel* label, const QString& prefix);
    void setColorButtonColor(QPushButton* button, const QColor& color);
    QColor getColorFromButton(const QPushButton* button) const;
    QString formatFontName(const QFont& font);
    struct ChannelOption {
        int startIndex{0};
        int channelCount{2};
        QString label;
    };

    struct DeviceOption {
        QString deviceName;
        QString deviceType;
        QString displayName;
        QVector<ChannelOption> channelOptions;
    };

    void refreshAudioDeviceLists();
    void updateChannelComboForDevice(QComboBox* combo,
                                     const QString& deviceName,
                                     const QString& deviceType,
                                     int preferredStart,
                                     int preferredCount);
    void ensureChannelSelectionValid(AppSettings& targetSettings,
                                     QComboBox* combo,
                                     const QString& deviceName,
                                     const QString& deviceType);
    DeviceOption* findDeviceOption(const QString& deviceName, const QString& deviceType);
    const DeviceOption* findDeviceOption(const QString& deviceName, const QString& deviceType) const;
    QVector<DeviceOption> audioOutputDevices;
    void populateThemes();
    void populateSkins();
    void populateMidiDevices();
    void addMidiMappingRow(const QString& controlName, const QString& midiInput, const QString& status);
    void onMidiDeviceRefresh();
    void onMidiDeviceActivate();
    void onMidiDeviceTest();
    void onMidiDeviceChanged();
    void onMidiEnabledChanged();
    void onMidiLearnToggle();
    void onMidiMappingLearn(int row);
    void onMidiMappingClear(int row);
    void saveMidiMappings(const QString& fileName);
    void loadMidiMappings(const QString& fileName);
    
    // MIDI Engine slots
    void onMidiDeviceOpened(const QString& deviceName);
    void onMidiDeviceClosed();
    void onMidiDeviceError(const QString& error);
    void onMidiMessageReceived(int channel, int controlNumber, int value, bool isNote);
};
