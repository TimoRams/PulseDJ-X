# LibraryManager - Erweiterte Musikbibliothek

Die neue LibraryManager Komponente ersetzt die einfache Dateienkliste mit einem vollwertigen Musikbibliotheks-System.

## Features

### ID3 Tag Support
- **Titel**: Zeigt den Songtitel aus ID3 Tags an (oder Dateiname als Fallback)
- **Interpret**: Anzeige des Künstlers aus ID3 Tags
- **Album**: Album-Information
- **Genre**: Musik-Genre
- **Jahr**: Erscheinungsjahr
- **BPM**: Beat pro Minute (falls im Tag vorhanden)
- **Dauer**: Automatische Berechnung der Track-Länge
- **Dateigröße**: Anzeige der Dateigröße

### Sortierungsmöglichkeiten
Das Dropdown-Menü "Sort by" ermöglicht Sortierung nach:
- **Title** (Titel)
- **Artist** (Interpret)
- **Album**
- **Duration** (Dauer)
- **BPM**
- **Genre**
- **Year** (Jahr)
- **File Size** (Dateigröße)

### Filter/Suche
- Suchfeld filtert in Echtzeit durch alle Metadaten
- Sucht in Titel, Interpret, Album und Genre
- Zeigt Anzahl der gefilterten vs. Gesamttracks an

### Dateiverwaltung
- **"Add Files..."**: Einzelne Audiodateien hinzufügen
- **"Add Folder..."**: Komplette Ordner (mit Unterordnern) hinzufügen
- **"Clear Library"**: Bibliothek komplett leeren
- Unterstützte Formate: MP3, WAV, FLAC, AAC, OGG, M4A

### Verwendung

#### Tracks laden
1. **Doppelklick** auf einen Track lädt ihn ins aktuell fokussierte Deck
2. **Drag & Drop** von Tracks auf die Decks
3. Auswahl mit Pfeiltasten und Enter

#### Automatische Initialisierung
- Beim Start werden automatisch Dateien aus dem "Music" Ordner geladen
- ID3 Tags werden im Hintergrund analysiert (mit Fortschrittsanzeige)

## Technische Details

### Hintergrundverarbeitung
- ID3 Tag-Extraktion läuft in separaten Threads
- Keine UI-Blockierung während des Ladens
- Fortschrittsanzeige für große Bibliotheken

### Speicheroptimierung
- Lazy Loading der Audiodaten
- Effiziente Tabellendarstellung
- Minimaler Speicherverbrauch für Metadaten

### Drag & Drop Integration
- Vollständige Integration mit den bestehenden Deck-Widgets
- MIME-Type Unterstützung für Dateitransfer
- Mehrfachauswahl möglich

## Shortcuts

- **F5/F6**: Deck A Timing-Trim (±1ms)
- **F7/F8**: Deck B Timing-Trim (±1ms)
- **+/-**: Beat Grid Zoom
- **0**: Beat Grid Zoom zurücksetzen

## Erweiterungen für die Zukunft

Die LibraryManager Architektur ist darauf ausgelegt, zukünftig erweitert zu werden:

- **Playlists**: Sammlung von Tracks
- **Bewertungen**: 5-Sterne System
- **Schlüssel-Erkennung**: Harmonische Kompatibilität
- **Intelligente Filter**: BPM-Bereiche, Kompatible Schlüssel
- **Statistiken**: Spielhäufigkeit, zuletzt gespielt
- **Auto-BPM**: Integration mit BPM-Analyzer
- **Cue Points**: Gespeicherte Sprungpunkte
- **Waveform Previews**: Mini-Waveforms in der Liste

## Integration mit bestehenden Features

- **BPM Analyzer**: Erkannte BPM werden automatisch in der Bibliothek gespeichert
- **Beat Grid**: Synchronisation mit erkannten Beat-Informationen
- **Waveform Display**: Kompatibel mit der bestehenden Waveform-Architektur
