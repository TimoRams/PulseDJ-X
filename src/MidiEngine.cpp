#include "MidiEngine.h"
#include "DJAudioPlayer.h"
#include "QtMainWindow.h"
#include <QDebug>
#include <QApplication>

MidiEngine::MidiEngine(QObject* parent)
    : QObject(parent)
    , learnTimer(new QTimer(this))
    , playerA(nullptr)
    , playerB(nullptr)
    , mainWindow(nullptr)
{
    // Setup learn timeout timer
    learnTimer->setSingleShot(true);
    learnTimer->setInterval(10000); // 10 seconds timeout for MIDI learn
    connect(learnTimer, &QTimer::timeout, this, &MidiEngine::onLearnTimeout);
    
    // Initialize available devices
    updateAvailableDevices();
    
    qDebug() << "MidiEngine initialized with" << availableDevices.size() << "MIDI devices detected";
}

void MidiEngine::setPlayers(DJAudioPlayer* newPlayerA, DJAudioPlayer* newPlayerB)
{
    playerA = newPlayerA;
    playerB = newPlayerB;
    qDebug() << "MidiEngine: Players connected - A:" << (playerA != nullptr) << "B:" << (playerB != nullptr);
}

void MidiEngine::setMainWindow(QtMainWindow* window)
{
    mainWindow = window;
    qDebug() << "MidiEngine: MainWindow connected:" << (mainWindow != nullptr);
}

MidiEngine::~MidiEngine()
{
    closeMidiDevice();
}

void MidiEngine::updateAvailableDevices()
{
    availableDevices = juce::MidiInput::getAvailableDevices();
}

QStringList MidiEngine::getAvailableMidiDevices() const
{
    // Update devices list (need to cast away const for this operation)
    const_cast<MidiEngine*>(this)->updateAvailableDevices();
    QStringList deviceNames;
    
    for (const auto& device : availableDevices) {
        deviceNames << QString::fromStdString(device.name.toStdString());
    }
    
    qDebug() << "Available MIDI devices:" << deviceNames;
    return deviceNames;
}

bool MidiEngine::openMidiDevice(const QString& deviceName)
{
    // Close existing device first
    closeMidiDevice();
    
    // Find the device
    for (const auto& device : availableDevices) {
        if (QString::fromStdString(device.name.toStdString()) == deviceName) {
            midiInput = juce::MidiInput::openDevice(device.identifier, this);
            if (midiInput) {
                midiInput->start();
                currentDeviceName = deviceName;
                qDebug() << "MIDI device opened successfully:" << deviceName;
                emit deviceOpened(deviceName);
                return true;
            } else {
                qDebug() << "Failed to open MIDI device:" << deviceName;
                emit deviceError("Failed to open MIDI device: " + deviceName);
                return false;
            }
        }
    }
    
    qDebug() << "MIDI device not found:" << deviceName;
    emit deviceError("MIDI device not found: " + deviceName);
    return false;
}

void MidiEngine::closeMidiDevice()
{
    if (midiInput) {
        midiInput->stop();
        midiInput.reset();
        QString closedDevice = currentDeviceName;
        currentDeviceName.clear();
        qDebug() << "MIDI device closed:" << closedDevice;
        emit deviceClosed();
    }
}

void MidiEngine::loadSettings(QSettings& settings)
{
    midiEnabled = settings.value("midi/enabled", false).toBool();
    midiChannel = settings.value("midi/channel", 1).toInt();
    learnMode = settings.value("midi/learnMode", false).toBool();
    
    qDebug() << "MidiEngine: Loading settings - enabled:" << midiEnabled << "channel:" << midiChannel;
    
    QString deviceName = settings.value("midi/device", "").toString();
    if (midiEnabled && !deviceName.isEmpty()) {
        qDebug() << "MidiEngine: Attempting to open saved device:" << deviceName;
        if (!openMidiDevice(deviceName)) {
            qDebug() << "MidiEngine: Failed to open saved device, will need manual setup";
        }
    } else {
        qDebug() << "MidiEngine: No device to open - enabled:" << midiEnabled << "device:" << deviceName;
    }
    
    // Load control mappings
    clearAllMappings();
    int mappingCount = settings.value("midi/mappingCount", 0).toInt();
    
    for (int i = 0; i < mappingCount; ++i) {
        settings.beginGroup(QString("midi/mapping_%1").arg(i));
        
        MidiControlMapping mapping;
        mapping.midiChannel = settings.value("channel", 1).toInt();
        mapping.controlNumber = settings.value("controlNumber", -1).toInt();
        mapping.controlType = stringToControlType(settings.value("controlType", "Unknown").toString());
        mapping.deckId = settings.value("deckId", "").toString();
        mapping.isNote = settings.value("isNote", false).toBool();
        mapping.minValue = settings.value("minValue", 0).toInt();
        mapping.maxValue = settings.value("maxValue", 127).toInt();
        
        settings.endGroup();
        
        if (mapping.controlNumber >= 0) {
            addControlMapping(mapping);
        }
    }
    
    qDebug() << "MIDI settings loaded - enabled:" << midiEnabled << "device:" << deviceName << "mappings:" << mappingCount;
}

void MidiEngine::saveSettings(QSettings& settings) const
{
    settings.setValue("midi/enabled", midiEnabled);
    settings.setValue("midi/device", currentDeviceName);
    settings.setValue("midi/channel", midiChannel);
    settings.setValue("midi/learnMode", learnMode);
    
    // Save control mappings
    QList<MidiControlMapping> mappings = getAllMappings();
    settings.setValue("midi/mappingCount", mappings.size());
    
    for (int i = 0; i < mappings.size(); ++i) {
        const auto& mapping = mappings[i];
        settings.beginGroup(QString("midi/mapping_%1").arg(i));
        
        settings.setValue("channel", mapping.midiChannel);
        settings.setValue("controlNumber", mapping.controlNumber);
        settings.setValue("controlType", controlTypeToString(mapping.controlType));
        settings.setValue("deckId", mapping.deckId);
        settings.setValue("isNote", mapping.isNote);
        settings.setValue("minValue", mapping.minValue);
        settings.setValue("maxValue", mapping.maxValue);
        
        settings.endGroup();
    }
    
    qDebug() << "MIDI settings saved - mappings:" << mappings.size();
}

void MidiEngine::setLearnMode(bool enabled)
{
    learnMode = enabled;
    if (!enabled) {
        learnTimer->stop();
        pendingControlType = MidiControlType::Unknown;
        pendingDeckId.clear();
    }
    qDebug() << "MIDI learn mode" << (enabled ? "enabled" : "disabled");
}

void MidiEngine::assignNextMidiControl(MidiControlType controlType, const QString& deckId)
{
    if (!learnMode) {
        qDebug() << "Cannot assign MIDI control - learn mode is disabled";
        return;
    }
    
    pendingControlType = controlType;
    pendingDeckId = deckId;
    learnTimer->start();
    
    qDebug() << "Waiting for MIDI input to assign to" << controlTypeToString(controlType) 
             << "for deck" << deckId << "(timeout: 10s)";
}

void MidiEngine::onLearnTimeout()
{
    qDebug() << "MIDI learn timeout - no input received";
    pendingControlType = MidiControlType::Unknown;
    pendingDeckId.clear();
}

void MidiEngine::addControlMapping(const MidiControlMapping& mapping)
{
    auto key = std::make_pair(mapping.midiChannel, mapping.controlNumber);
    
    if (mapping.isNote) {
        noteMappings[key] = mapping;
    } else {
        controlMappings[key] = mapping;
    }
    
    qDebug() << "Added MIDI mapping:" << controlTypeToString(mapping.controlType) 
             << "CH" << mapping.midiChannel << "CC/Note" << mapping.controlNumber 
             << "Deck" << mapping.deckId;
}

void MidiEngine::removeControlMapping(int midiChannel, int controlNumber, bool isNote)
{
    auto key = std::make_pair(midiChannel, controlNumber);
    
    if (isNote) {
        noteMappings.erase(key);
    } else {
        controlMappings.erase(key);
    }
    
    qDebug() << "Removed MIDI mapping: CH" << midiChannel << (isNote ? "Note" : "CC") << controlNumber;
}

void MidiEngine::clearAllMappings()
{
    controlMappings.clear();
    noteMappings.clear();
    qDebug() << "All MIDI mappings cleared";
}

QList<MidiControlMapping> MidiEngine::getAllMappings() const
{
    QList<MidiControlMapping> allMappings;
    
    for (const auto& pair : controlMappings) {
        allMappings.append(pair.second);
    }
    
    for (const auto& pair : noteMappings) {
        allMappings.append(pair.second);
    }
    
    return allMappings;
}

void MidiEngine::testMidiDevice()
{
    if (!midiInput) {
        emit deviceError("No MIDI device is open");
        return;
    }
    
    qDebug() << "Testing MIDI device:" << currentDeviceName;
    qDebug() << "Send any MIDI message from your controller...";
    
    // The test feedback will come through handleIncomingMidiMessage
}

void MidiEngine::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    Q_UNUSED(source)
    
    if (!midiEnabled) {
        return;
    }
    
    int channel = message.getChannel();
    bool isNote = message.isNoteOnOrOff();
    bool isCC = message.isController();
    
    if (!isNote && !isCC) {
        return; // Ignore other MIDI message types for now
    }
    
    int controlNumber = isNote ? message.getNoteNumber() : message.getControllerNumber();
    int value = isNote ? message.getVelocity() : message.getControllerValue();
    
    qDebug() << "=== MIDI MESSAGE RECEIVED ===" << (isNote ? "Note" : "CC") 
             << "CH" << channel << "Num" << controlNumber << "Val" << value;
    
    // Show current mapping status for debugging
    auto key = std::make_pair(channel, controlNumber);
    const auto& mappings = isNote ? noteMappings : controlMappings;
    auto it = mappings.find(key);
    if (it != mappings.end()) {
        qDebug() << "  -> MAPPED TO:" << controlTypeToString(it->second.controlType) << "Deck" << it->second.deckId;
    } else {
        // Also check Omni Channel (0) mappings
        auto omniKey = std::make_pair(0, controlNumber);
        auto omniIt = mappings.find(omniKey);
        if (omniIt != mappings.end()) {
            qDebug() << "  -> OMNI MAPPING FOUND:" << controlTypeToString(omniIt->second.controlType) << "Deck" << omniIt->second.deckId;
        } else {
            qDebug() << "  -> NO MAPPING FOUND (neither direct nor omni)";
            qDebug() << "  -> Total mappings:" << (controlMappings.size() + noteMappings.size());
        }
    }
    
    emit midiMessageReceived(channel, controlNumber, value, isNote);
    
    // Handle MIDI Learn Mode
    if (learnMode && pendingControlType != MidiControlType::Unknown) {
        MidiControlMapping newMapping;
        newMapping.midiChannel = channel;
        newMapping.controlNumber = controlNumber;
        newMapping.controlType = pendingControlType;
        newMapping.deckId = pendingDeckId;
        newMapping.isNote = isNote;
        newMapping.minValue = 0;
        newMapping.maxValue = 127;
        
        addControlMapping(newMapping);
        emit midiLearnAssigned(pendingControlType, channel, controlNumber);
        
        // Reset learn state
        learnTimer->stop();
        pendingControlType = MidiControlType::Unknown;
        pendingDeckId.clear();
        
        qDebug() << "MIDI Learn completed:" << controlTypeToString(newMapping.controlType);
        return;
    }
    
    // Process mapped controls with Omni Channel support
    // (reuse the mappings variable already declared above)
    
    // Check for direct channel match first
    auto directKey = std::make_pair(channel, controlNumber);
    auto directIt = mappings.find(directKey);
    if (directIt != mappings.end()) {
        processMidiControl(directIt->second, value);
        return;
    }
    
    // Check for Omni Channel (Channel 0) mappings - accepts any incoming channel
    auto omniKey = std::make_pair(0, controlNumber);
    auto omniIt = mappings.find(omniKey);
    if (omniIt != mappings.end()) {
        qDebug() << "MIDI: Using Omni Channel mapping for CH" << channel << "CC/Note" << controlNumber;
        processMidiControl(omniIt->second, value);
        return;
    }
    
    // Check if current MIDI channel setting is Omni (0) - accept all channels for any mapping
    if (midiChannel == 0) {
        for (const auto& mapping : mappings) {
            if (mapping.first.second == controlNumber) {
                qDebug() << "MIDI: Omni mode - accepting CH" << channel << "for mapping on CH" << mapping.first.first;
                processMidiControl(mapping.second, value);
                return;
            }
        }
    }
    
    qDebug() << "MIDI: No mapping found for CH" << channel << (isNote ? "Note" : "CC") << controlNumber;
}

void MidiEngine::processMidiControl(const MidiControlMapping& mapping, int value)
{
    // Normalize value to 0.0 - 1.0 range
    float normalizedValue = static_cast<float>(value - mapping.minValue) / 
                           static_cast<float>(mapping.maxValue - mapping.minValue);
    normalizedValue = std::max(0.0f, std::min(1.0f, normalizedValue));
    
    qDebug() << "Processing MIDI control:" << controlTypeToString(mapping.controlType) 
             << "Deck" << mapping.deckId << "Value" << normalizedValue;
    
    executeControlAction(mapping.controlType, mapping.deckId, normalizedValue);
}

void MidiEngine::executeControlAction(MidiControlType controlType, const QString& deckId, float normalizedValue)
{
    DJAudioPlayer* targetPlayer = getPlayerForDeck(deckId);
    
    switch (controlType) {
        case MidiControlType::PlayPause:
            if (normalizedValue > 0.5f) { // Button press (> 50%)
                // Route through MainWindow MIDI methods for consistent behavior
                if (mainWindow) {
                    if (deckId == "A") {
                        qDebug() << "MIDI: Triggering Play/Pause for Deck A via QtMainWindow";
                        mainWindow->setDeckAPlayPause(true); // The method handles toggle internally
                    } else if (deckId == "B") {
                        qDebug() << "MIDI: Triggering Play/Pause for Deck B via QtMainWindow";
                        mainWindow->setDeckBPlayPause(true); // The method handles toggle internally
                    }
                } else {
                    // Fallback to direct player control if no UI available
                    if (targetPlayer) {
                        if (targetPlayer->isPlaying()) {
                            targetPlayer->stop();
                            qDebug() << "MIDI: Stopped deck" << deckId << "(direct)";
                        } else {
                            targetPlayer->start();
                            qDebug() << "MIDI: Started deck" << deckId << "(direct)";
                        }
                    }
                }
            }
            break;
            
        case MidiControlType::Cue:
            if (targetPlayer && normalizedValue > 0.5f) {
                // Cue functionality would need to be implemented in DJAudioPlayer
                qDebug() << "MIDI: Cue requested for deck" << deckId << "(not implemented)";
            }
            break;
            
        case MidiControlType::ChannelFader:
            if (mainWindow) {
                // Route through QtMainWindow for consistent volume control
                if (deckId == "A") {
                    qDebug() << "MIDI: Triggering Volume for Deck A via QtMainWindow";
                    mainWindow->setDeckAVolume(normalizedValue);
                } else if (deckId == "B") {
                    qDebug() << "MIDI: Triggering Volume for Deck B via QtMainWindow";
                    mainWindow->setDeckBVolume(normalizedValue);
                }
            }
            break;
            
        case MidiControlType::PitchBend:
            if (mainWindow) {
                // Route through QtMainWindow for consistent tempo control
                if (deckId == "A") {
                    qDebug() << "MIDI: Triggering Tempo for Deck A via QtMainWindow";
                    mainWindow->setDeckATempo(normalizedValue);
                } else if (deckId == "B") {
                    qDebug() << "MIDI: Triggering Tempo for Deck B via QtMainWindow";
                    mainWindow->setDeckBTempo(normalizedValue);
                }
            } else if (targetPlayer) {
                // Fallback to direct player control
                float pitchValue = (normalizedValue - 0.5f) * 2.0f;
                targetPlayer->setSpeed(1.0 + pitchValue * 0.5); // ±50% speed range
                qDebug() << "MIDI: Pitch bend deck" << deckId << "=" << pitchValue << "(direct)";
            }
            break;
            
        case MidiControlType::Crossfader:
            if (mainWindow) {
                // Crossfader affects both decks through main window
                qDebug() << "MIDI: Crossfader =" << normalizedValue;
                mainWindow->setCrossfaderPosition(normalizedValue);
            } else {
                qDebug() << "MIDI: Crossfader control - no main window available";
            }
            break;
            
        // EQ Controls - these would need EQ implementation in DJAudioPlayer
        case MidiControlType::EqHigh:
        case MidiControlType::EqMid:
        case MidiControlType::EqLow:
        case MidiControlType::Filter:
            qDebug() << "MIDI: EQ control" << controlTypeToString(controlType) 
                     << "deck" << deckId << "=" << normalizedValue;
            // TODO: Implement EQ controls in DJAudioPlayer
            break;
            
        // Loop and Hot Cues - would need implementation
        case MidiControlType::Loop:
        case MidiControlType::HotCue1:
        case MidiControlType::HotCue2:
        case MidiControlType::HotCue3:
        case MidiControlType::HotCue4:
            qDebug() << "MIDI: Control" << controlTypeToString(controlType) 
                     << "deck" << deckId << "value" << normalizedValue;
            // TODO: Implement loop and hot cue functionality
            break;
            
        default:
            qDebug() << "MIDI: Unhandled control type" << controlTypeToString(controlType);
            break;
    }
}

DJAudioPlayer* MidiEngine::getPlayerForDeck(const QString& deckId) const
{
    if (deckId == "A" && playerA) {
        return playerA;
    } else if (deckId == "B" && playerB) {
        return playerB;
    }
    return nullptr;
}

QString MidiEngine::controlTypeToString(MidiControlType type) const
{
    switch (type) {
        case MidiControlType::PlayPause: return "PlayPause";
        case MidiControlType::Cue: return "Cue";
        case MidiControlType::Sync: return "Sync";
        case MidiControlType::Loop: return "Loop";
        case MidiControlType::HotCue1: return "HotCue1";
        case MidiControlType::HotCue2: return "HotCue2";
        case MidiControlType::HotCue3: return "HotCue3";
        case MidiControlType::HotCue4: return "HotCue4";
        case MidiControlType::EqHigh: return "EqHigh";
        case MidiControlType::EqMid: return "EqMid";
        case MidiControlType::EqLow: return "EqLow";
        case MidiControlType::Filter: return "Filter";
        case MidiControlType::Crossfader: return "Crossfader";
        case MidiControlType::ChannelFader: return "ChannelFader";
        case MidiControlType::PitchBend: return "PitchBend";
        case MidiControlType::JogWheel: return "JogWheel";
        case MidiControlType::LoadTrackA: return "LoadTrackA";
        case MidiControlType::LoadTrackB: return "LoadTrackB";
        case MidiControlType::TempoUp: return "TempoUp";
        case MidiControlType::TempoDown: return "TempoDown";
        default: return "Unknown";
    }
}

MidiControlType MidiEngine::stringToControlType(const QString& str) const
{
    if (str == "PlayPause") return MidiControlType::PlayPause;
    if (str == "Cue") return MidiControlType::Cue;
    if (str == "Sync") return MidiControlType::Sync;
    if (str == "Loop") return MidiControlType::Loop;
    if (str == "HotCue1") return MidiControlType::HotCue1;
    if (str == "HotCue2") return MidiControlType::HotCue2;
    if (str == "HotCue3") return MidiControlType::HotCue3;
    if (str == "HotCue4") return MidiControlType::HotCue4;
    if (str == "EqHigh") return MidiControlType::EqHigh;
    if (str == "EqMid") return MidiControlType::EqMid;
    if (str == "EqLow") return MidiControlType::EqLow;
    if (str == "Filter") return MidiControlType::Filter;
    if (str == "Crossfader") return MidiControlType::Crossfader;
    if (str == "ChannelFader") return MidiControlType::ChannelFader;
    if (str == "PitchBend") return MidiControlType::PitchBend;
    if (str == "JogWheel") return MidiControlType::JogWheel;
    if (str == "LoadTrackA") return MidiControlType::LoadTrackA;
    if (str == "LoadTrackB") return MidiControlType::LoadTrackB;
    if (str == "TempoUp") return MidiControlType::TempoUp;
    if (str == "TempoDown") return MidiControlType::TempoDown;
    return MidiControlType::Unknown;
}