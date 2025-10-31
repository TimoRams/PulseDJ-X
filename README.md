# **PulseDJ-X**

PulseDJ-X is a next-generation DJ performance and library-management suite built with **JUCE** and **Qt**.  
It combines club-ready playback, precise beat analysis, and flexible metadata control in one open project.

---

## 🚀 **Key Features (Roadmap)**

- 🎧 **Dual Deck Playback** — with waveform overviews, cue pads, tempo controls, and turntable emulation  
- 🧠 **Beat & BPM Analysis** — automatic grid detection using available libraries, with in-house fallbacks in progress  
- 📚 **Library Browser** — powerful filtering, metadata editing, and missing-file indicators  
- 🎛️ **MIDI & Scratch Engine** — optimized for low-latency performance workflows  
- 🧩 **Cross-Platform Architecture** — built with CMake, tested on Linux (ALSA / PipeWire)  
- ⚡ **Modern C++23 Core** — efficient signal path and performance-oriented design  

---

## 🧰 **Building PulseDJ-X**

```bash
cmake -S . -B build
cmake --build build -j
```

After building, launch the binary:

```bash
./build/David
```

Make sure you have:
- **aubio**
- **JUCE dependencies**
- **Audio backend (ALSA / PipeWire / etc.)**

installed on your system.

---

## ⚙️ **Development Status**

PulseDJ-X is **under active development**.  
Expect:
- Frequent updates and UI changes  
- Experimental features  
- Occasional instability while synchronization and analysis systems evolve  

The listed features reflect near-term milestones.

---

## 🎯 **Long-Term Vision**

- Seamless support for **Linux**, **Windows**, and **macOS** with native backends  
- Scalable, **resilient music-library management** that preserves metadata integrity  
- Offline analysis for **beat grids**, **key detection**, and **waveform caching**  
- Fully **extensible controller & plugin ecosystem**  
