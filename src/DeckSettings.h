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
    // Einzelne Deck-Konfiguration - VEREINFACHT: Nur wichtige Einstellungen speichern
    struct DeckConfig {
        // Transport & Tempo - NUR DIESE WERDEN GESPEICHERT
        bool keylockEnabled = false;
        bool quantizeEnabled = false;
        
        // ALLE ANDEREN WERTE SIND NICHT PERSISTENT (springen auf Standard zurück)
        // Diese werden nur zur Laufzeit verwendet aber NICHT gespeichert:
        double speedFactor = 1.0;       // Reset bei jedem Start
        double gain = 0.75;             // Reset bei jedem Start
        
        // EQ Einstellungen - NICHT GESPEICHERT (immer Standard)
        double highGain = 0.0;          // Immer 0.0 beim Start
        double midGain = 0.0;           // Immer 0.0 beim Start  
        double lowGain = 0.0;           // Immer 0.0 beim Start
        
        // Filter - NICHT GESPEICHERT (immer Standard)
        double filterPosition = 0.0;    // Immer 0.0 beim Start
        
        // Visual & Display - NICHT GESPEICHERT
        double visualTrim = 0.0;        // Immer 0.0 beim Start
        
        // Loop Einstellungen - NICHT GESPEICHERT  
        bool loopEnabled = false;       // Immer false beim Start
        double loopStartSec = 0.0;      // Immer 0.0 beim Start
        double loopLengthSec = 4.0;     // Immer 4.0 beim Start
        
        // Scratch & Performance - NICHT GESPEICHERT
        bool scratchMode = false;       // Immer false beim Start
        
        // Session Restore - NICHT GESPEICHERT
        QString lastTrackPath;          // Leer beim Start
        double lastPosition = 0.0;      // Immer 0.0 beim Start
        
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
    
    // Lade alle Deck-Settings - NUR KEYLOCK UND QUANTIZE
    void loadSettings() {
        QString settingsPath = AppConfig::instance().getConfigDirectory() + "/deck_settings.ini";
        QSettings settings(settingsPath, QSettings::IniFormat);
        
        // Deck A laden - NUR keylock und quantize
        settings.beginGroup("DeckA");
        deckA.keylockEnabled = settings.value("keylock", false).toBool();
        deckA.quantizeEnabled = settings.value("quantize", false).toBool();
        // ALLE ANDEREN WERTE BLEIBEN AUF STANDARD (werden nicht geladen)
        settings.endGroup();
        
        // Deck B laden - NUR keylock und quantize
        settings.beginGroup("DeckB");
        deckB.keylockEnabled = settings.value("keylock", false).toBool();
        deckB.quantizeEnabled = settings.value("quantize", false).toBool();
        // ALLE ANDEREN WERTE BLEIBEN AUF STANDARD (werden nicht geladen)
        settings.endGroup();
        
        qDebug() << "BetaPulseX: Nur Keylock/Quantize geladen:";
        qDebug() << "  Deck A: Keylock=" << deckA.keylockEnabled << "Quantize=" << deckA.quantizeEnabled;
        qDebug() << "  Deck B: Keylock=" << deckB.keylockEnabled << "Quantize=" << deckB.quantizeEnabled;
    }
    
    // Speichere alle Deck-Settings - NUR KEYLOCK UND QUANTIZE
    void saveSettings() {
        // Stelle sicher, dass Config-Verzeichnis existiert
        AppConfig::instance().createDirectories();
        
        QString settingsPath = AppConfig::instance().getConfigDirectory() + "/deck_settings.ini";
        QSettings settings(settingsPath, QSettings::IniFormat);
        
        // Lösche alte Settings komplett für saubere Neuanlage
        settings.clear();
        
        // Deck A speichern - NUR keylock und quantize
        settings.beginGroup("DeckA");
        settings.setValue("keylock", deckA.keylockEnabled);
        settings.setValue("quantize", deckA.quantizeEnabled);
        settings.endGroup();
        
        // Deck B speichern - NUR keylock und quantize  
        settings.beginGroup("DeckB");
        settings.setValue("keylock", deckB.keylockEnabled);
        settings.setValue("quantize", deckB.quantizeEnabled);
        settings.endGroup();
        
        // Explizit synchronisieren
        settings.sync();
        
        qDebug() << "BetaPulseX: Nur Keylock/Quantize gespeichert nach" << settingsPath;
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
    
    // Quick-Access für wichtige Einstellungen - NUR KEYLOCK UND QUANTIZE GESPEICHERT
    void setKeylock(int deckIndex, bool enabled) {
        getDeck(deckIndex).keylockEnabled = enabled;
        autoSave(); // Wird gespeichert
    }
    
    void setQuantize(int deckIndex, bool enabled) {
        getDeck(deckIndex).quantizeEnabled = enabled;
        autoSave(); // Wird gespeichert
    }
    
    // FOLGENDE FUNKTIONEN AKTUALISIEREN NUR LAUFZEIT-WERTE (NICHT GESPEICHERT)
    void setSpeedFactor(int deckIndex, double factor) {
        getDeck(deckIndex).speedFactor = factor;
        // NICHT gespeichert - autoSave() NICHT aufgerufen
    }
    
    void setEQ(int deckIndex, double high, double mid, double low) {
        auto& deck = getDeck(deckIndex);
        deck.highGain = high;
        deck.midGain = mid;
        deck.lowGain = low;
        // NICHT gespeichert - autoSave() NICHT aufgerufen
    }
    
    void setFilter(int deckIndex, double position) {
        getDeck(deckIndex).filterPosition = position;
        // NICHT gespeichert - autoSave() NICHT aufgerufen
    }
    
    void setGain(int deckIndex, double gain) {
        getDeck(deckIndex).gain = gain;
        // NICHT gespeichert - autoSave() NICHT aufgerufen
    }
    
    void setVisualTrim(int deckIndex, double trimSec) {
        getDeck(deckIndex).visualTrim = trimSec;
        // NICHT gespeichert - autoSave() NICHT aufgerufen
    }
    
    void setLoop(int deckIndex, bool enabled, double startSec = 0.0, double lengthSec = 4.0) {
        auto& deck = getDeck(deckIndex);
        deck.loopEnabled = enabled;
        deck.loopStartSec = startSec;
        deck.loopLengthSec = lengthSec;
        // NICHT gespeichert - autoSave() NICHT aufgerufen
    }
    
    void setCuePoint(int deckIndex, int cueIndex, bool active, double position, const QString& label = "") {
        if (cueIndex >= 0 && cueIndex < 8) {
            auto& cue = getDeck(deckIndex).cuePoints[cueIndex];
            cue.active = active;
            cue.position = position;
            cue.label = label;
            // NICHT gespeichert - autoSave() NICHT aufgerufen
        }
    }
    
    void setLastTrack(int deckIndex, const QString& trackPath, double position = 0.0) {
        auto& deck = getDeck(deckIndex);
        deck.lastTrackPath = trackPath;
        deck.lastPosition = position;
        // NICHT gespeichert - autoSave() NICHT aufgerufen
    }
    
    // Reset alle Settings auf Default-Werte
    void resetToDefaults() {
        deckA = DeckConfig(); // Default constructor - alle Werte auf Standard
        deckB = DeckConfig(); // Default constructor - alle Werte auf Standard
        
        // Lösche alle gespeicherten Settings
        QString settingsPath = AppConfig::instance().getConfigDirectory() + "/deck_settings.ini";
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.clear();
        
        // Speichere Standard-Werte (nur Keylock/Quantize = false)
        if (autoSaveEnabled) {
            saveSettings();
        }
        
        qDebug() << "BetaPulseX: Alle Deck-Settings auf Standard zurückgesetzt";
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
};

// Convenience Makros
#define DECK_SETTINGS DeckSettings::instance()
#define DECK_A_CONFIG DECK_SETTINGS.getDeckA()
#define DECK_B_CONFIG DECK_SETTINGS.getDeckB()
