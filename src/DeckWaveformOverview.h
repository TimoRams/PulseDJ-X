#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLShaderProgram>
#include <QImage>
#include <vector>
#include <QString>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <JuceHeader.h>
#include "GlobalBeatGrid.h"

// Compact per-deck waveform overview rendered with OpenGL (upper half fill)
class DeckWaveformOverview : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit DeckWaveformOverview(QWidget* parent = nullptr);
    ~DeckWaveformOverview() override;
    void loadFile(const QString& path);
    void setPlayhead(double relative);
    void setBeatInfo(double bpm, double firstBeatOffset, double totalLength);
    // Set audio start offset for accurate display trimming
    void setAudioStartOffset(double audioStartTime);
    
    // NEW: Cue points support
    void setCuePoints(const std::array<double, 8>& cuePoints);
    void clearCuePoints();
    
    // NEW: Loop visualization support
    void setLoopRegion(bool enabled, double startSec = 0.0, double endSec = 0.0);
    void clearLoop();
    
    // NEW: Ghost loop visualization support
    void setGhostLoopRegion(bool enabled, double startSec = 0.0, double endSec = 0.0);
    // Visual latency compensation in seconds (UI leads audio by this amount)
    void setVisualLatencyComp(double seconds) { visualLatencyComp = std::clamp(seconds, -0.25, 0.25); }
    // New: set precomputed waveform data from a background thread result (called on UI thread)
    void setWaveformData(const std::vector<float>& data, double audioStartOffsetSec, double lengthSec) {
        waveform = data;
        audioStartOffset = audioStartOffsetSec;
        totalLength = lengthSec;
        meshDirty = true;
        update();
    }

signals:
    void fileDropped(const QString& path);

signals:
    void positionClicked(double relative);

protected:
    // QOpenGLWidget overrides
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    // UI interaction
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    // GPU resources
    QOpenGLShaderProgram* program{nullptr};
    QOpenGLShaderProgram* lineProgram{nullptr};
    QOpenGLBuffer vbo{ QOpenGLBuffer::VertexBuffer };
    QOpenGLVertexArrayObject vao;
    QOpenGLBuffer lineVbo{ QOpenGLBuffer::VertexBuffer };
    QOpenGLVertexArrayObject lineVao;

    // CPU-side waveform samples (0..1 upper-half amplitude per column)
    std::vector<float> waveform;
    bool meshDirty{true};
    int vertexCount{0}; // number of vertices in VBO
    float amplitudeScale{1.2f}; // Increased for better visibility

    juce::AudioFormatManager formatManager;
    QString currentFilePath;
    double playheadPos{-1.0};
    double visualLatencyComp{0.0};
    int viewportW{0};
    int viewportH{0};
    
    // Beat info for grid display  
    double bpm{0.0};
    double firstBeatOffset{0.0};
    double totalLength{0.0};
    double audioStartOffset{0.0}; // Time when audio actually starts
    
    // NEW: Cue points for display
    std::array<double, 8> cuePoints; // Cue points in seconds
    bool cuePointsValid{false};
    
    // NEW: Loop region for display
    bool loopEnabled{false};
    double loopStartSec{0.0};
    double loopEndSec{0.0};
    
    // NEW: Ghost loop region for display
    bool ghostLoopEnabled{false};
    double ghostLoopStartSec{0.0};
    double ghostLoopEndSec{0.0};

    void loadAndRenderWaveform();
    void rebuildMeshIfNeeded();
    void drawCuePoints(); // NEW: Draw cue points as lines
    void drawLoopRegion(); // NEW: Draw loop region
    void drawGhostLoopRegion(); // NEW: Draw ghost loop region
    void uploadMesh();
    void drawPlayhead();
    // Smooth display of playhead to avoid janky movement
    QTimer* smoothTimer{nullptr};
    double displayedPlayheadPos{ -1.0 };
    bool isDragging{false};
};
