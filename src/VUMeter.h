#pragma once

#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <vector>
#include <algorithm>
#include <cmath>

class VUMeter : public QWidget {
    Q_OBJECT
    
public:
    enum Type {
        Channel,  // Channel VU meter (blinks red/normal)
        Master    // Master VU meter (dims when blinking)
    };
    
    explicit VUMeter(Type type = Master, QWidget* parent = nullptr) : QWidget(parent), m_type(type) {
        setFixedSize(12, 80);
        
        // Smooth decay timer
        m_decayTimer = new QTimer(this);
        connect(m_decayTimer, &QTimer::timeout, this, &VUMeter::decay);
        m_decayTimer->start(30); // Update every 30ms
        
        // Initialize global blink timer on first VUMeter creation
        if (s_globalBlinkTimer == nullptr) {
            s_globalBlinkTimer = new QTimer();
            s_globalBlinkTimer->setInterval(200); // Blink every 200ms
            connect(s_globalBlinkTimer, &QTimer::timeout, []() {
                if (s_activeBlinkingMeters > 0) {
                    // Toggle blink state globally for synchronized rhythm
                    s_blinkState = !s_blinkState;
                } else if (s_blinkState) {
                    // Reset blink phase when nothing is blinking anymore
                    s_blinkState = false;
                } else {
                    return;
                }
                // Trigger update on all VU meters
                for (auto* meter : s_allMeters) {
                    if (meter) {
                        meter->update();
                    }
                }
            });
            s_globalBlinkTimer->start();
        }
        
        // Register this meter in the global list
        s_allMeters.push_back(this);
    }
    
    ~VUMeter() override {
        // Ensure we are no longer counted for blinking
        updateBlinkRegistration(false);
        // Unregister from global list
        s_allMeters.erase(std::remove(s_allMeters.begin(), s_allMeters.end(), this), s_allMeters.end());
    }
    
    void setLevel(float level) {
        // Convert to dB scale for better visualization
        float dbLevel = 20.0f * std::log10(std::max(level, 0.0001f));
        // Normalize to 0.0 - 1.0 range (-55dB to 0dB)
        constexpr float minDb = -55.0f;
        float normalized = std::clamp((dbLevel - minDb) / (0.0f - minDb), 0.0f, 1.0f);
        
        // Apply perceptual curve so moderate levels feel steady while peaks still reach the top
        normalized = std::pow(normalized, 0.7f);
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        
        // Store the raw normalized value for clipping detection
        m_rawLevel = normalized;
        
        // Update current level (always update for smooth decay)
        m_currentLevel = std::max(m_currentLevel, normalized);
        
        // Update peak immediately when new peak is reached
        if (normalized > m_peakLevel) {
            m_peakLevel = normalized;
            m_peakHoldTime = 40; // Hold peak for ~1.3 seconds
        }
        
        // Two-stage clipping detection: warning (pre-clip) and hard clip
        constexpr float warningThreshold = 0.90f;  // start warning blink
        constexpr float clipThreshold = 0.97f;     // hard clip
        const bool aboveClip = m_rawLevel >= clipThreshold;
        const bool aboveWarning = m_rawLevel >= warningThreshold;
        
        // Clip state has priority and immediate response
        if (aboveClip) {
            m_clipFrames = std::min(m_clipFrames + 3, 30);
        } else if (m_clipFrames > 0) {
            m_clipFrames = std::max(0, m_clipFrames - 1);
        }
        m_clipActive = m_clipFrames > 0;
        
        // Only warn when not actively clipping
        if (!m_clipActive) {
            if (aboveWarning) {
                m_warningFrames = std::min(m_warningFrames + 2, 30);
            } else if (m_warningFrames > 0) {
                m_warningFrames = std::max(0, m_warningFrames - 1);
            }
        } else {
            m_warningFrames = 0;
        }
        m_warningActive = m_warningFrames > 0;
        
        updateBlinkRegistration(m_warningActive || m_clipActive);
        update();
    }
    
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        const int w = width();
        const int h = height();
        
        // Background - darker like Engine DJ
        painter.fillRect(0, 0, w, h, QColor(8, 8, 10));
        
    const bool blinkPhase = s_blinkState;
    const bool highlightClip = m_clipActive && blinkPhase;
    const bool highlightWarning = (!m_clipActive && m_warningActive && blinkPhase);
        
        // Calculate fill height (use full height, no margins)
        int fillHeight = static_cast<int>(m_currentLevel * h);
        fillHeight = std::min(fillHeight, h); // Clamp to height
        
        // Draw level bar with smooth gradient (Engine DJ style)
        if (fillHeight > 1) {
            QLinearGradient gradient(0, h, 0, 0);
            if (highlightClip) {
                // Hard clip: blink bright red regardless of meter type
                gradient.setColorAt(0.0, QColor(210, 0, 0));
                gradient.setColorAt(0.5, QColor(245, 0, 0));
                gradient.setColorAt(1.0, QColor(255, 80, 80));
            } else if (highlightWarning) {
                if (m_type == Channel) {
                    // Channel warning: amber/orange pulse
                    gradient.setColorAt(0.0, QColor(120, 70, 0));
                    gradient.setColorAt(0.6, QColor(200, 140, 0));
                    gradient.setColorAt(1.0, QColor(255, 200, 40));
                } else {
                    // Master warning: dim overall to draw attention without red
                    gradient.setColorAt(0.0, QColor(0, 70, 0));
                    gradient.setColorAt(0.55, QColor(20, 90, 0));
                    gradient.setColorAt(0.70, QColor(50, 90, 0));
                    gradient.setColorAt(0.82, QColor(80, 70, 0));
                    gradient.setColorAt(0.90, QColor(90, 55, 0));
                    gradient.setColorAt(0.96, QColor(90, 30, 0));
                    gradient.setColorAt(1.0, QColor(90, 10, 0));
                }
            } else {
                // Normal bright colors
                gradient.setColorAt(0.0, QColor(0, 240, 0));
                gradient.setColorAt(0.55, QColor(40, 255, 0));
                gradient.setColorAt(0.70, QColor(150, 255, 0));
                gradient.setColorAt(0.82, QColor(255, 240, 0));
                gradient.setColorAt(0.90, QColor(255, 180, 0));
                gradient.setColorAt(0.96, QColor(255, 100, 0));
                gradient.setColorAt(1.0, QColor(255, 30, 0));
            }
            
            painter.fillRect(0, h - fillHeight, w, fillHeight, gradient);
        }
        
        // Draw peak indicator line - always visible, positioned correctly
        if (m_peakLevel > 0.02f) {
            int peakY = h - static_cast<int>(m_peakLevel * h);
            peakY = std::clamp(peakY, 0, h - 2); // Keep peak within bounds
            
            // Peak color based on level
            QColor peakColor;
            if (highlightClip) {
                peakColor = QColor(255, 40, 40, 240);
            } else if (highlightWarning) {
                peakColor = QColor(230, 160, 0, 220);
            } else {
                // Normal bright peak colors
                if (m_peakLevel > 0.94f) {
                    peakColor = QColor(255, 10, 10, 240);
                } else if (m_peakLevel > 0.86f) {
                    peakColor = QColor(255, 160, 0, 230);
                } else if (m_peakLevel > 0.72f) {
                    peakColor = QColor(255, 230, 0, 220);
                } else {
                    peakColor = QColor(80, 255, 80, 210);
                }
            }
            
            // Draw a 2px thick peak line
            painter.fillRect(0, peakY, w, 2, peakColor);
        }
    }
    
private slots:
    void decay() {
        bool needsUpdate = false;
        
        // Smooth and faster decay for level
        if (m_currentLevel > 0.0f) {
            m_currentLevel *= 0.86f; // Faster decay for more responsive feel
            if (m_currentLevel < 0.003f) {
                m_currentLevel = 0.0f;
            }
            needsUpdate = true;
        }
        
        // Peak always follows - holds briefly then slowly falls
        if (m_peakHoldTime > 0) {
            // Hold phase - peak stays at current position
            m_peakHoldTime--;
            needsUpdate = true;
        } else if (m_peakLevel > m_currentLevel + 0.01f) {
            // Fall phase - peak slowly falls towards current level
            m_peakLevel *= 0.982f; // Slow fall (1.8% per frame)
            if (m_peakLevel < m_currentLevel) {
                m_peakLevel = m_currentLevel;
            }
            needsUpdate = true;
        } else if (m_peakLevel != m_currentLevel) {
            // Peak follows current level when very close
            m_peakLevel = m_currentLevel;
            needsUpdate = true;
        }
        
        if (needsUpdate) {
            update();
        }
    }
    
private:
    float m_currentLevel = 0.0f;
    float m_peakLevel = 0.0f;
    float m_rawLevel = 0.0f;
    int m_peakHoldTime = 0;
    Type m_type;
    QTimer* m_decayTimer;
    
    // Per-meter blinking state
    int m_warningFrames = 0;
    int m_clipFrames = 0;
    bool m_warningActive = false;
    bool m_clipActive = false;
    bool m_registeredForBlink = false;
    
    void updateBlinkRegistration(bool shouldBlink) {
        if (shouldBlink && !m_registeredForBlink) {
            m_registeredForBlink = true;
            ++s_activeBlinkingMeters;
        } else if (!shouldBlink && m_registeredForBlink) {
            m_registeredForBlink = false;
            s_activeBlinkingMeters = std::max(0, s_activeBlinkingMeters - 1);
            if (s_activeBlinkingMeters == 0) {
                s_blinkState = false;
                for (auto* meter : s_allMeters) {
                    if (meter) {
                        meter->update();
                    }
                }
            }
        }
    }
    
    // Static members for synchronized blinking rhythm
    static bool s_blinkState;
    static QTimer* s_globalBlinkTimer;
    static std::vector<VUMeter*> s_allMeters;
    static int s_activeBlinkingMeters;
};
