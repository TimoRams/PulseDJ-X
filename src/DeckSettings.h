#pragma once

#include <QSettings>
#include <QString>
#include <QDebug>
#include "AppConfig.h"

/**
 * BetaPulseX Deck Settings Manager
 * 
 * Speichert und lädt alle wichtigen Deck-Einstellungen für beide Decks:
 * - Keylock (Pitch Lock)
 * - Quantize (Beat Snap)
 * - Speed/Tempo
 * - EQ Einstellungen (High/Mid/Low)
 * - Filter Position
 * - Gain/Volume
 * - Visual Trim
 * - Loop Einstellungen
 */
class DeckSettings {
public:
    // Einzelne Deck-Konfiguration
    struct DeckConfig {
        // Transport & Tempo
        bool keylockEnabled = false;
        bool quantizeEnabled = false;
        double speedFactor = 1.0;       // 1.0 = original speed
        double gain = 0.75;             // 0.0 - 1.0
        
        // EQ Einstellungen
        double highGain = 0.0;          // -1.0 bis +1.0
        double midGain = 0.0;           // -1.0 bis +1.0
        double lowGain = 0.0;           // -1.0 bis +1.0
        
        // Filter
        double filterPosition = 0.0;    // -1.0 bis +1.0 (negativ=lowpass, positiv=highpass)
        
        // Visual & Display
        double visualTrim = 0.0;        // Visual offset in seconds
        
        // Loop Einstellungen
        bool loopEnabled = false;
        double loopStartSec = 0.0;
        double loopLengthSec = 4.0;     // Standard 4-Beat Loop
        
        // Scratch & Performance
        bool scratchMode = false;
        
        // Zuletzt geladener Track (für Session-Wiederherstellung)
        QString lastTrackPath;
        double lastPosition = 0.0;      // Position in Sekunden
        
        // Cue Points (bis zu 8 Cue Points pro Deck)
        struct CuePoint {
            bool active = false;
            double position = 0.0;      // Position in Sekunden
            QString label;              // Optional: Benutzerdefiniertes Label
        };
        std::array<CuePoint, 8> cuePoints;
    };
    
    static DeckSettings& instance() {
        static DeckSettings instance;
        return instance;
    }
    
    // Lade alle Deck-Settings
    void loadSettings() {
        QString settingsPath = AppConfig::instance().getConfigDirectory() + "/deck_settings.ini";
        QSettings settings(settingsPath, QSettings::IniFormat);
        
        // Deck A laden
        deckA = loadDeckConfig(settings, "DeckA");
        
        // Deck B laden  
        deckB = loadDeckConfig(settings, "DeckB");
        
        qDebug() << "BetaPulseX: Deck settings loaded from" << settingsPath;
        qDebug() << "  Deck A: Keylock=" << deckA.keylockEnabled << "Quantize=" << deckA.quantizeEnabled;
        qDebug() << "  Deck B: Keylock=" << deckB.keylockEnabled << "Quantize=" << deckB.quantizeEnabled;
    }
    
    // Speichere alle Deck-Settings
    void saveSettings() {
        // Stelle sicher, dass Config-Verzeichnis existiert
        AppConfig::instance().createDirectories();
        
        QString settingsPath = AppConfig::instance().getConfigDirectory() + "/deck_settings.ini";
        QSettings settings(settingsPath, QSettings::IniFormat);
        
        // Deck A speichern
        saveDeckConfig(settings, "DeckA", deckA);
        
        // Deck B speichern
        saveDeckConfig(settings, "DeckB", deckB);
        
        // Explizit synchronisieren
        settings.sync();
        
        qDebug() << "BetaPulseX: Deck settings saved to" << settingsPath;
    }
    
    // Getter für Deck-Konfigurationen
    DeckConfig& getDeckA() { return deckA; }
    DeckConfig& getDeckB() { return deckB; }
    const DeckConfig& getDeckA() const { return deckA; }
    const DeckConfig& getDeckB() const { return deckB; }
    
    // Helper: Deck-Config by Index (0=A, 1=B)
    DeckConfig& getDeck(int deckIndex) {
        return (deckIndex == 0) ? deckA : deckB;
    }
    
    const DeckConfig& getDeck(int deckIndex) const {
        return (deckIndex == 0) ? deckA : deckB;
    }
    
    // Quick-Access für häufige Einstellungen
    void setKeylock(int deckIndex, bool enabled) {
        getDeck(deckIndex).keylockEnabled = enabled;
        autoSave();
    }
    
    void setQuantize(int deckIndex, bool enabled) {
        getDeck(deckIndex).quantizeEnabled = enabled;
        autoSave();
    }
    
    void setSpeedFactor(int deckIndex, double factor) {
        getDeck(deckIndex).speedFactor = factor;
        autoSave();
    }
    
    void setEQ(int deckIndex, double high, double mid, double low) {
        auto& deck = getDeck(deckIndex);
        deck.highGain = high;
        deck.midGain = mid;
        deck.lowGain = low;
        autoSave();
    }
    
    void setFilter(int deckIndex, double position) {
        getDeck(deckIndex).filterPosition = position;
        autoSave();
    }
    
    void setGain(int deckIndex, double gain) {
        getDeck(deckIndex).gain = gain;
        autoSave();
    }
    
    void setVisualTrim(int deckIndex, double trimSec) {
        getDeck(deckIndex).visualTrim = trimSec;
        autoSave();
    }
    
    void setLoop(int deckIndex, bool enabled, double startSec = 0.0, double lengthSec = 4.0) {
        auto& deck = getDeck(deckIndex);
        deck.loopEnabled = enabled;
        deck.loopStartSec = startSec;
        deck.loopLengthSec = lengthSec;
        autoSave();
    }
    
    void setCuePoint(int deckIndex, int cueIndex, bool active, double position, const QString& label = "") {
        if (cueIndex >= 0 && cueIndex < 8) {
            auto& cue = getDeck(deckIndex).cuePoints[cueIndex];
            cue.active = active;
            cue.position = position;
            cue.label = label;
            autoSave();
        }
    }
    
    void setLastTrack(int deckIndex, const QString& trackPath, double position = 0.0) {
        auto& deck = getDeck(deckIndex);
        deck.lastTrackPath = trackPath;
        deck.lastPosition = position;
        autoSave();
    }
    
    // Reset alle Settings auf Default-Werte
    void resetToDefaults() {
        deckA = DeckConfig(); // Default constructor
        deckB = DeckConfig(); // Default constructor
        
        // Lösche alle gespeicherten Settings
        QSettings settings(AppConfig::instance().getSettingsPath(), QSettings::IniFormat);
        settings.clear();
        
        // Speichere Default-Werte
        if (autoSaveEnabled) {
            saveSettings();
        }
        
        qDebug() << "BetaPulseX: All deck settings reset to defaults";
    }
    
    // Auto-Save aktivieren/deaktivieren
    void setAutoSave(bool enabled) {
        autoSaveEnabled = enabled;
    }

private:
    DeckConfig deckA;
    DeckConfig deckB;
    bool autoSaveEnabled = true;
    
    DeckSettings() = default;
    
    void autoSave() {
        if (autoSaveEnabled) {
            saveSettings();
        }
    }
    
    DeckConfig loadDeckConfig(QSettings& settings, const QString& deckSection) {
        settings.beginGroup(deckSection);
        
        DeckConfig config;
        
        // Transport & Tempo
        config.keylockEnabled = settings.value("keylock", false).toBool();
        config.quantizeEnabled = settings.value("quantize", false).toBool();
        config.speedFactor = settings.value("speedFactor", 1.0).toDouble();
        config.gain = settings.value("gain", 0.75).toDouble();
        
        // EQ
        config.highGain = settings.value("eq/high", 0.0).toDouble();
        config.midGain = settings.value("eq/mid", 0.0).toDouble();
        config.lowGain = settings.value("eq/low", 0.0).toDouble();
        
        // Filter
        config.filterPosition = settings.value("filter", 0.0).toDouble();
        
        // Visual
        config.visualTrim = settings.value("visualTrim", 0.0).toDouble();
        
        // Loop
        config.loopEnabled = settings.value("loop/enabled", false).toBool();
        config.loopStartSec = settings.value("loop/start", 0.0).toDouble();
        config.loopLengthSec = settings.value("loop/length", 4.0).toDouble();
        
        // Performance
        config.scratchMode = settings.value("scratchMode", false).toBool();
        
        // Session
        config.lastTrackPath = settings.value("session/lastTrack", "").toString();
        config.lastPosition = settings.value("session/lastPosition", 0.0).toDouble();
        
        // Cue Points
        for (int i = 0; i < 8; ++i) {
            QString cueGroup = QString("cue%1").arg(i);
            settings.beginGroup(cueGroup);
            
            config.cuePoints[i].active = settings.value("active", false).toBool();
            config.cuePoints[i].position = settings.value("position", 0.0).toDouble();
            config.cuePoints[i].label = settings.value("label", "").toString();
            
            settings.endGroup();
        }
        
        settings.endGroup();
        return config;
    }
    
    void saveDeckConfig(QSettings& settings, const QString& deckSection, const DeckConfig& config) {
        settings.beginGroup(deckSection);
        
        // Transport & Tempo
        settings.setValue("keylock", config.keylockEnabled);
        settings.setValue("quantize", config.quantizeEnabled);
        settings.setValue("speedFactor", config.speedFactor);
        settings.setValue("gain", config.gain);
        
        // EQ
        settings.setValue("eq/high", config.highGain);
        settings.setValue("eq/mid", config.midGain);
        settings.setValue("eq/low", config.lowGain);
        
        // Filter
        settings.setValue("filter", config.filterPosition);
        
        // Visual
        settings.setValue("visualTrim", config.visualTrim);
        
        // Loop
        settings.setValue("loop/enabled", config.loopEnabled);
        settings.setValue("loop/start", config.loopStartSec);
        settings.setValue("loop/length", config.loopLengthSec);
        
        // Performance
        settings.setValue("scratchMode", config.scratchMode);
        
        // Session
        settings.setValue("session/lastTrack", config.lastTrackPath);
        settings.setValue("session/lastPosition", config.lastPosition);
        
        // Cue Points
        for (int i = 0; i < 8; ++i) {
            QString cueGroup = QString("cue%1").arg(i);
            settings.beginGroup(cueGroup);
            
            settings.setValue("active", config.cuePoints[i].active);
            settings.setValue("position", config.cuePoints[i].position);
            settings.setValue("label", config.cuePoints[i].label);
            
            settings.endGroup();
        }
        
        settings.endGroup();
    }
};

// Convenience Makros
#define DECK_SETTINGS DeckSettings::instance()
#define DECK_A_CONFIG DECK_SETTINGS.getDeckA()
#define DECK_B_CONFIG DECK_SETTINGS.getDeckB()
