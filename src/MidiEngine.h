#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QSettings>
#include <functional>
#include <map>
#include <juce_audio_devices/juce_audio_devices.h>

// Forward declarations for DJ components
class DJAudioPlayer;
class QtMainWindow;

// MIDI Control Types for DJ functions
enum class MidiControlType {
    PlayPause,
    Cue,
    Sync,
    Loop,
    HotCue1, HotCue2, HotCue3, HotCue4,
    EqHigh, EqMid, EqLow, Filter,
    Crossfader, ChannelFader,
    PitchBend, JogWheel,
    LoadTrackA, LoadTrackB,
    TempoUp, TempoDown,
    Unknown
};

// MIDI Control Mapping structure
struct MidiControlMapping {
    int midiChannel = 1;
    int controlNumber = -1;  // CC number or note number
    MidiControlType controlType = MidiControlType::Unknown;
    QString deckId;  // "A" or "B" for deck-specific controls, empty for global
    bool isNote = false;  // true for Note On/Off, false for Control Change
    int minValue = 0;
    int maxValue = 127;
};

// MIDI Input Handler Class
class MidiEngine : public QObject, private juce::MidiInputCallback
{
    Q_OBJECT
    
public:
    explicit MidiEngine(QObject* parent = nullptr);
    ~MidiEngine();
    
    // Player connection methods
    void setPlayers(DJAudioPlayer* playerA, DJAudioPlayer* playerB);
    void setMainWindow(QtMainWindow* mainWindow);    // Device Management
    QStringList getAvailableMidiDevices() const;
    bool openMidiDevice(const QString& deviceName);
    void closeMidiDevice();
    QString getCurrentDevice() const { return currentDeviceName; }
    bool isDeviceOpen() const { return midiInput != nullptr; }
    
    // Enable/Disable MIDI
    void setEnabled(bool enabled) { midiEnabled = enabled; }
    bool isEnabled() const { return midiEnabled; }

    // Settings Management
    void loadSettings(QSettings& settings);
    void saveSettings(QSettings& settings) const;

    // MIDI Learn Mode
    void setLearnMode(bool enabled);
    bool isLearnMode() const { return learnMode; }
    void assignNextMidiControl(MidiControlType controlType, const QString& deckId = "");

    // Control Mapping
    void addControlMapping(const MidiControlMapping& mapping);
    void removeControlMapping(int midiChannel, int controlNumber, bool isNote);
    void clearAllMappings();
    QList<MidiControlMapping> getAllMappings() const;

    // DJ Component Integration (implemented in .cpp file)

    // Testing and Debug
    void testMidiDevice();

    // JUCE MidiInputCallback implementation
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

signals:
    void deviceOpened(const QString& deviceName);
    void deviceClosed();
    void deviceError(const QString& error);
    void midiMessageReceived(int channel, int controlNumber, int value, bool isNote);
    void midiLearnAssigned(MidiControlType controlType, int channel, int controlNumber);

private slots:
    void onLearnTimeout();

private:
    // Device Management
    std::unique_ptr<juce::MidiInput> midiInput;
    QString currentDeviceName;
    juce::Array<juce::MidiDeviceInfo> availableDevices;

    // Settings
    bool midiEnabled = false;
    int midiChannel = 1;
    bool learnMode = false;

    // MIDI Learn
    QTimer* learnTimer;
    MidiControlType pendingControlType = MidiControlType::Unknown;
    QString pendingDeckId;

    // Control Mappings
    std::map<std::pair<int, int>, MidiControlMapping> controlMappings; // Key: (channel, controlNumber)
    std::map<std::pair<int, int>, MidiControlMapping> noteMappings;    // Key: (channel, noteNumber)

    // DJ Component References
    QtMainWindow* mainWindow = nullptr;
    DJAudioPlayer* playerA = nullptr;
    DJAudioPlayer* playerB = nullptr;

    // Helper Methods
    void updateAvailableDevices();
    void processMidiControl(const MidiControlMapping& mapping, int value);
    void executeControlAction(MidiControlType controlType, const QString& deckId, float normalizedValue);
    QString controlTypeToString(MidiControlType type) const;
    MidiControlType stringToControlType(const QString& str) const;
    DJAudioPlayer* getPlayerForDeck(const QString& deckId) const;
};