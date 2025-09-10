#pragma once

#include <vector>

/**
 * Global Beat Grid System für PulseDJ-X
 * 
 * Verwaltet ein globales Beat-Grid mit fester Pixel-pro-Sekunde Ratio für konsistente
 * Beat-Grid-Darstellung. Das System ermöglicht:
 * - Feste Pixel-pro-Sekunde Ratio (z.B. 1 Sekunde = 50 Pixel)
 * - Globales Beat-Offset aus BPM-Analyzer
 * - Präzise Beat-Grid-Darstellung mit konstanten Abständen
 * - Synchronisation zwischen allen Waveform-Komponenten
 */
class GlobalBeatGrid {
public:
    static GlobalBeatGrid& getInstance() {
        static GlobalBeatGrid instance;
        return instance;
    }
    
    // Konfiguration der festen Pixel-pro-Sekunde Ratio
    void setPixelsPerSecond(double pixelsPerSec) { 
        pixelsPerSecond = pixelsPerSec;
        updateBeatPositions();
    }
    
    double getPixelsPerSecond() const { return pixelsPerSecond; }
    
    // Berechne Waveform-Breite für gegebene Song-Länge
    int calculateWaveformWidth(double songLengthSec) const {
        return (int)(songLengthSec * pixelsPerSecond);
    }
    
    // Beat-Grid-Parameter aus BPM-Analyse
    void setBeatGridParams(double bpm, double firstBeatOffsetSec, double trackLengthSec) {
        currentBpm = bpm;
        firstBeatOffset = firstBeatOffsetSec;
        currentTrackLength = trackLengthSec;
        updateBeatPositions();
    }
    
    // REMOVED: Global tempo factor - now handled per deck
    
    // Beat-Positionen in Sekunden relativ zum Song-Anfang
    const std::vector<double>& getBeatPositionsSeconds() const { return beatPositionsSeconds; }
    
    // Beat-Positionen als Pixel-Offsets vom Song-Anfang
    const std::vector<int>& getBeatPositionsPixels() const { return beatPositionsPixels; }
    
    // Hilfsfunktion: Berechne Beat-Position für gegebene Zeit
    double getBeatPositionAtTime(double timeSeconds) const {
        if (currentBpm <= 0.0) return 0.0;
        
        // Use original BPM for beat calculations
        double beatPeriod = 60.0 / currentBpm;
        
        return (timeSeconds - firstBeatOffset) / beatPeriod;
    }
    
    // Konvertiere Zeit zu Pixel-Position
    int timeToPixels(double timeSeconds) const {
        return (int)(timeSeconds * pixelsPerSecond);
    }
    
    // Konvertiere Pixel-Position zu Zeit
    double pixelsToTime(int pixels) const {
        return (double)pixels / pixelsPerSecond;
    }
    
    // Parameter-Zugriff
    double getCurrentBpm() const { return currentBpm; }
    double getFirstBeatOffset() const { return firstBeatOffset; }
    double getCurrentTrackLength() const { return currentTrackLength; }
    // REMOVED: getTempoFactor() - now handled per deck
    
private:
    GlobalBeatGrid() = default;
    
    void updateBeatPositions() {
        beatPositionsSeconds.clear();
        beatPositionsPixels.clear();
        
        if (currentBpm <= 0.0 || currentTrackLength <= 0.0) {
            return;
        }
        
        // Use original BPM for beat positions (no tempo factor applied here)
        double beatPeriod = 60.0 / currentBpm;
        
        // Generiere Beats für die gesamte Track-Länge
        for (double beatTime = firstBeatOffset; beatTime <= currentTrackLength; beatTime += beatPeriod) {
            if (beatTime >= 0.0) {
                beatPositionsSeconds.push_back(beatTime);
                beatPositionsPixels.push_back(timeToPixels(beatTime));
            }
        }
        
        // Füge auch Beats vor dem Offset hinzu (negative Zeiten bis zum Song-Anfang)
        for (double beatTime = firstBeatOffset - beatPeriod; beatTime >= -beatPeriod && beatTime >= 0.0; beatTime -= beatPeriod) {
            beatPositionsSeconds.insert(beatPositionsSeconds.begin(), beatTime);
            beatPositionsPixels.insert(beatPositionsPixels.begin(), timeToPixels(beatTime));
        }
    }
    
    // Feste Parameter
    double pixelsPerSecond{50.0}; // Standard: 50 Pixel pro Sekunde (1 Sekunde = ~2mm bei 96 DPI)
    
    // Beat-Grid-Parameter
    double currentBpm{120.0};
    double firstBeatOffset{0.0};
    double currentTrackLength{0.0};
    // REMOVED: double tempoFactor{1.0}; - now handled per deck
    
    // Berechnete Beat-Positionen
    std::vector<double> beatPositionsSeconds;
    std::vector<int> beatPositionsPixels;
};
