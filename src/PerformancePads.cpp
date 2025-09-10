#include "PerformancePads.h"
#include "DJAudioPlayer.h"
#include "BeatIndicator.h"
#include "GlobalBeatGrid.h"
#include <QString>
#include <QTimer>
#include <QFont>
#include <cmath>
#include <iostream>

PerformancePads::PerformancePads(DeckId deck, QWidget* parent)
    : QWidget(parent), deckId(deck), currentMode(Mode::Cue), player(nullptr), beatIndicator(nullptr)
{
    qDebug() << "PerformancePads constructor called for deck" << (deck == DeckId::A ? "A" : "B");
    std::cout << "=== PerformancePads constructor for deck " << (deck == DeckId::A ? "A" : "B") << " ===" << std::endl;
    // init cues as empty
    for (auto &c : cuePoints) c = -1.0;

    auto* root = new QVBoxLayout(this);
    root->setSpacing(2);
    root->setContentsMargins(2,2,2,2);

    // Mode buttons (consistent sizing) - improved styling
    auto* modes = new QHBoxLayout;
    modes->setSpacing(2);  // Slightly more spacing for better look
    cueModeBtn = new QPushButton("Cue", this);
    loopModeBtn = new QPushButton("Loop", this);  
    jumpModeBtn = new QPushButton("Jump", this);  
    cueModeBtn->setCheckable(true);
    loopModeBtn->setCheckable(true);
    jumpModeBtn->setCheckable(true);
    cueModeBtn->setChecked(true);
    
    // Make mode buttons wider for better appearance
    int buttonWidth = 55;  // Increased from 50 for better proportions
    int buttonHeight = 26; // Back to 26 for better readability
    cueModeBtn->setFixedSize(buttonWidth, buttonHeight);
    loopModeBtn->setFixedSize(buttonWidth, buttonHeight);
    jumpModeBtn->setFixedSize(buttonWidth, buttonHeight);
    
    // Improved styling for mode buttons
    QString modeButtonStyle = "QPushButton { font-size: 9px; font-weight: bold; padding: 3px; border-radius: 0px; border: 1px solid #666; } "
                             "QPushButton:checked { background-color: #0066cc; color: white; border: 1px solid #0088ff; } "
                             "QPushButton:!checked { background-color: #444; color: #ccc; } "
                             "QPushButton:hover { background-color: #555; }";
    cueModeBtn->setStyleSheet(modeButtonStyle);
    loopModeBtn->setStyleSheet(modeButtonStyle);
    jumpModeBtn->setStyleSheet(modeButtonStyle);
    
    modes->addWidget(cueModeBtn);
    modes->addWidget(loopModeBtn);
    modes->addWidget(jumpModeBtn);
    root->addLayout(modes);

    connect(cueModeBtn, &QPushButton::clicked, this, &PerformancePads::setModeCue);
    connect(loopModeBtn, &QPushButton::clicked, this, &PerformancePads::setModeLoop);
    connect(jumpModeBtn, &QPushButton::clicked, this, &PerformancePads::setModeJump);
    qDebug() << "Connected mode buttons for PerformancePads";

    // Pads: 2 columns x 4 rows, wider for better usability
    auto* grid = new QGridLayout;
    grid->setSpacing(3);
    grid->setContentsMargins(0,0,0,0);

    for (int i = 0; i < 8; ++i) {
        auto* btn = new QPushButton(this);
        // Compact but still readable
        btn->setMinimumHeight(28);
        btn->setMaximumHeight(32);
        btn->setMinimumWidth(110);
        // Ensure the button expands horizontally when layout allows
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // Use an explicit font so text remains readable even when styles are reapplied
        QFont f = btn->font();
        f.setPointSize(10);
        f.setBold(true);
        btn->setFont(f);
        // initial simple alignment via stylesheet; full styles applied in refreshPadStyles()
        btn->setStyleSheet("padding:5px; text-align:center;");
        pads[i] = btn;
        int col = (i / 4); // 0 oder 1
        int row = (i % 4); // 0..3
        grid->addWidget(btn, row, col);
        connect(btn, &QPushButton::clicked, this, [this, i](){ 
            qDebug() << "Pad button" << i << "lambda triggered"; 
            onPadPressed(i); 
        });
        qDebug() << "Connected pad button" << i << "to onPadPressed lambda";
    }
    root->addLayout(grid);

    updatePadLabels();
    
    // Timer to continuously update pad styles (especially for loop highlighting)
    styleUpdateTimer = new QTimer(this);
    connect(styleUpdateTimer, &QTimer::timeout, this, &PerformancePads::refreshPadStyles);
    styleUpdateTimer->start(200); // Update every 200ms
}

void PerformancePads::setAudioPlayer(DJAudioPlayer* p) {
    player = p;
}

void PerformancePads::setBeatIndicator(BeatIndicator* indicator) {
    beatIndicator = indicator;
}

void PerformancePads::setModeCue() {
    currentMode = Mode::Cue;
    cueModeBtn->setChecked(true); loopModeBtn->setChecked(false); jumpModeBtn->setChecked(false);
    updatePadLabels();
    refreshPadStyles();
    emit modeChanged(currentMode);
}
void PerformancePads::setModeLoop() {
    qDebug() << "PerformancePads::setModeLoop called";
    currentMode = Mode::BeatLoop;
    cueModeBtn->setChecked(false); loopModeBtn->setChecked(true); jumpModeBtn->setChecked(false);
    updatePadLabels();
    // leaving loop mode: clear highlight semantics but keep loop running until user toggles off
    refreshPadStyles();
    emit modeChanged(currentMode);
}
void PerformancePads::setModeJump() {
    currentMode = Mode::BeatJump;
    cueModeBtn->setChecked(false); loopModeBtn->setChecked(false); jumpModeBtn->setChecked(true);
    updatePadLabels();
    emit modeChanged(currentMode);
}

void PerformancePads::updatePadLabels() {
    if (currentMode == Mode::Cue) {
        for (int i = 0; i < 8; ++i) {
            QString label = QString("CUE %1").arg(i+1);
            if (cuePoints[i] >= 0.0 && player) {
                // Single line format: "CUE 1: 1.2s" instead of two lines
                label = QString("CUE %1: %2s").arg(i+1).arg(QString::number(cuePoints[i], 'f', 1));
            }
            pads[i]->setText(label);
        }
    } else if (currentMode == Mode::BeatLoop) {
        // Two banks: left column: 1,2,4,8; right column: 16,32,1/2,1/4
        const char* texts[8] = {"1", "2", "4", "8", "16", "32", "1/2", "1/4"};
        for (int i = 0; i < 8; ++i) pads[i]->setText(QString("Loop %1").arg(texts[i]));
    } else {
        const char* texts[8] = {"-32", "-16", "-8", "-4", "+4", "+8", "+16", "+32"};
        for (int i = 0; i < 8; ++i) pads[i]->setText(QString("Jump %1").arg(texts[i]));
    }
    refreshPadStyles();
}

void PerformancePads::onPadPressed(int idx) {
    qDebug() << "PerformancePads::onPadPressed called with idx:" << idx << "currentMode:" << (int)currentMode << "hasPlayer:" << (player != nullptr);
    if (!player) {
        qDebug() << "PerformancePads::onPadPressed - No player available!";
        return;
    }
    switch (currentMode) {
        case Mode::Cue:
            qDebug() << "PerformancePads::onPadPressed - Cue mode";
            if (cuePoints[idx] < 0.0) storeCue(idx); else recallCue(idx);
            break;
        case Mode::BeatLoop:
            qDebug() << "PerformancePads::onPadPressed - BeatLoop mode, calling triggerLoop";
            triggerLoop(idx);
            break;
        case Mode::BeatJump:
            qDebug() << "PerformancePads::onPadPressed - BeatJump mode";
            triggerJump(idx);
            break;
    }
}

void PerformancePads::storeCue(int idx) {
    double rawPos = player->getCurrentPositionSeconds();
    // Apply quantization only if quantize is enabled for this deck
    if (player && player->isQuantizeEnabled()) {
        cuePoints[idx] = quantizeToNearestBeat(rawPos);
    } else {
        cuePoints[idx] = rawPos; // Use raw position without quantization
    }
    updatePadLabels();
    // Emit signal so waveforms can update their cue point display
    emit cuePointsChanged(cuePoints);
}

void PerformancePads::recallCue(int idx) {
    if (cuePoints[idx] >= 0.0) {
        player->setPositionSeconds(cuePoints[idx]);
    }
}

void PerformancePads::triggerLoop(int idx) {
    // If the same pad is already active, toggle loop off
    if (activeLoopPad == idx) {
        // Store ghost loop for visual feedback
        if (player && player->isLoopEnabled()) {
            ghostLoopStartSec = player->getLoopStart();
            ghostLoopEndSec = player->getLoopEnd();
            ghostLoopEnabled = true;
            
            // Emit ghost loop to waveforms
            emit ghostLoopChanged(true, ghostLoopStartSec, ghostLoopEndSec);
        }
        
        player->disableLoop();
        activeLoopPad = -1;
        refreshPadStyles();
        return;
    }
    
    // Map pad idx to beat counts: [1,2,4,8,16,32,0.5,0.25]
    static const double beats[8] = {1,2,4,8,16,32,0.5,0.25};
    
    // CRITICAL FIX: Use original BPM for loop length calculation, not tempo-scaled BPM
    double originalSecPerBeat = getOriginalSecondsPerBeat();
    double lengthSec = beats[idx] * originalSecPerBeat;
    
    double start;
    
    // If a loop is already active, keep the same start point and only change length
    if (activeLoopPad >= 0 && player && player->isLoopEnabled()) {
        start = player->getLoopStart(); // Keep existing start point
        qDebug() << "PerformancePads::triggerLoop - Keeping existing start point:" << start;
    } else {
        // New loop: calculate start position
        double rawStart = player->getCurrentPositionSeconds();
        
        // Apply quantization for loop START position only if quantize is enabled
        if (player && player->isQuantizeEnabled()) {
            start = quantizeToNearestBeat(rawStart);
        } else {
            start = rawStart; // Use raw position without quantization
        }
    }
    
    // Clear ghost loop when starting a new active loop
    ghostLoopEnabled = false;
    emit ghostLoopChanged(false, 0.0, 0.0);
    
    // DEBUG: Log loop parameters
    qDebug() << "PerformancePads::triggerLoop - Pad:" << idx 
             << "Beats:" << beats[idx]
             << "OriginalSecPerBeat:" << originalSecPerBeat 
             << "LengthSec:" << lengthSec
             << "Start:" << start
             << "QuantizeEnabled:" << (player ? player->isQuantizeEnabled() : false)
             << "KeepingExistingStart:" << (activeLoopPad >= 0);
    
    qDebug() << "PerformancePads::triggerLoop - About to call player->enableLoop";
    player->enableLoop(start, lengthSec);
    qDebug() << "PerformancePads::triggerLoop - After enableLoop, isLoopEnabled:" << player->isLoopEnabled();
    activeLoopPad = idx;
    refreshPadStyles();
}

void PerformancePads::triggerJump(int idx) {
    static const int beats[8] = {-32,-16,-8,-4, +4,+8,+16,+32};
    double secPerBeat = getSecondsPerBeat();
    double delta = beats[idx] * secPerBeat;
    double pos = player->getCurrentPositionSeconds();
    double len = player->getLengthInSeconds();
    double rawTarget = std::max(0.0, std::min(pos + delta, len));
    
    // Apply quantization for jump target only if quantize is enabled
    double target;
    if (player && player->isQuantizeEnabled()) {
        target = quantizeToNearestBeat(rawTarget);
    } else {
        target = rawTarget; // Use raw target position without quantization
    }
    
    player->setPositionSeconds(target);
}

void PerformancePads::refreshPadStyles() {
    // Highlight active loop pad when in loop mode; otherwise normal style
    for (int i = 0; i < 8; ++i) {
        bool active = (currentMode == Mode::BeatLoop) && (i == activeLoopPad) && player && player->isLoopEnabled();
        if (active) {
            pads[i]->setStyleSheet("QPushButton { background-color: #00ff41; color: #000; font-weight: bold; font-size: 10px; border: 2px solid #fff; border-radius: 0px; padding:5px; text-align:center; } ");
        } else {
            pads[i]->setStyleSheet("QPushButton { background-color: #444; color: #fff; font-size: 10px; border: 1px solid #666; border-radius: 0px; padding:5px; text-align:center; } "
                                  "QPushButton:hover { background-color: #555; } "
                                  "QPushButton:pressed { background-color: #333; }");
        }
    }
}

// Beat and BPM utilities
double PerformancePads::getCurrentBpm() const {
    // Get deck-specific BPM from BeatIndicator if available
    if (beatIndicator) {
        double effectiveBpm = 0.0;
        if (deckId == DeckId::A) {
            effectiveBpm = beatIndicator->getEffectiveBpmDeckA();
        } else {
            effectiveBpm = beatIndicator->getEffectiveBpmDeckB();
        }
        
        if (effectiveBpm > 0.0) {
            qDebug() << "PerformancePads: Using effective BPM" << effectiveBpm << "for deck" << (deckId == DeckId::A ? "A" : "B");
            return effectiveBpm;
        }
    }
    
    // Fallback to GlobalBeatGrid
    double globalBpm = GlobalBeatGrid::getInstance().getCurrentBpm();
    if (globalBpm > 0.0) {
        qDebug() << "PerformancePads: Using GlobalBeatGrid BPM" << globalBpm;
        return globalBpm;
    }
    // Last resort fallback
    qDebug() << "PerformancePads: Using fallback BPM 120.0";
    return 120.0;
}

double PerformancePads::getSecondsPerBeat() const {
    double bpm = getCurrentBpm();
    return (bpm > 0.0) ? (60.0 / bpm) : 0.5; // 0.5 seconds per beat at 120 BPM
}

// NEW: Get original BPM (not tempo-scaled) for loop length calculations
double PerformancePads::getOriginalSecondsPerBeat() const {
    // Get the original track BPM from GlobalBeatGrid (not tempo-scaled)
    double originalBpm = GlobalBeatGrid::getInstance().getCurrentBpm();
    if (originalBpm > 0.0) {
        qDebug() << "PerformancePads: Using original BPM" << originalBpm << "for loop length calculation";
        return 60.0 / originalBpm;
    }
    
    // Fallback: try to get original BPM from beat indicator
    if (beatIndicator) {
        // Note: This might still be tempo-scaled, but it's better than nothing
        double fallbackBpm = 0.0;
        if (deckId == DeckId::A) {
            fallbackBpm = beatIndicator->getEffectiveBpmDeckA();
        } else {
            fallbackBpm = beatIndicator->getEffectiveBpmDeckB();
        }
        
        if (fallbackBpm > 0.0) {
            qDebug() << "PerformancePads: Using fallback BPM" << fallbackBpm << "for loop length calculation";
            return 60.0 / fallbackBpm;
        }
    }
    
    // Last resort fallback
    qDebug() << "PerformancePads: Using fallback 0.5 seconds per beat (120 BPM) for loop length calculation";
    return 0.5; // 0.5 seconds per beat at 120 BPM
}

double PerformancePads::quantizeToNearestBeat(double positionSeconds) const {
    if (!player) return positionSeconds;
    
    // Use the actual beat grid from GlobalBeatGrid for precise quantization
    const auto& beatPositions = GlobalBeatGrid::getInstance().getBeatPositionsSeconds();
    if (beatPositions.empty()) {
        // Fallback to simple math if no beat grid is available
        double secPerBeat = getSecondsPerBeat();
        if (secPerBeat <= 0.0) return positionSeconds;
        double beatPosition = positionSeconds / secPerBeat;
        double nearestBeat = std::round(beatPosition);
        return nearestBeat * secPerBeat;
    }
    
    // Find the closest beat in the actual beat grid
    double closestBeat = beatPositions[0];
    double minDistance = std::abs(positionSeconds - closestBeat);
    
    for (double beatTime : beatPositions) {
        double distance = std::abs(positionSeconds - beatTime);
        if (distance < minDistance) {
            minDistance = distance;
            closestBeat = beatTime;
        }
    }
    
    return closestBeat;
}
