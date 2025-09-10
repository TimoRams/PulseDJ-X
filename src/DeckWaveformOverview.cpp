#include "DeckWaveformOverview.h"
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include "WaveformGenerator.h"
#include <QPainter>
#include <QTimer>
#include <QTime>
#include <QMimeData>
#include <QLinearGradient>
#include <algorithm>

DeckWaveformOverview::DeckWaveformOverview(QWidget* parent)
    : QOpenGLWidget(parent)
{
    formatManager.registerBasicFormats(); // Includes MP3 support with JUCE_USE_MP3AUDIOFORMAT=1
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setAcceptDrops(true);
    
    // Initialize cue points as invalid
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    
    // Smooth playhead timer (exponential smoothing for ultra-smooth marker movement)
    smoothTimer = new QTimer(this);
    smoothTimer->setInterval(16); // ~60 FPS
    connect(smoothTimer, &QTimer::timeout, this, [this]() {
        if (playheadPos < 0.0) { update(); return; }
        if (displayedPlayheadPos < 0.0) displayedPlayheadPos = playheadPos;
        double diff = playheadPos - displayedPlayheadPos;
        // Exponential smoothing: responsive but stable
        const double alpha = 0.35; // 0..1, higher = faster follow
        displayedPlayheadPos += diff * alpha;
        // Snap when very close to avoid micro-jitter
        if (std::abs(playheadPos - displayedPlayheadPos) < 0.0008) displayedPlayheadPos = playheadPos;
        update();
    });
    smoothTimer->start();
}

DeckWaveformOverview::~DeckWaveformOverview()
{
    makeCurrent();
    if (program) { delete program; program = nullptr; }
    if (lineProgram) { delete lineProgram; lineProgram = nullptr; }
    if (vbo.isCreated()) vbo.destroy();
    if (vao.isCreated()) vao.destroy();
    if (lineVbo.isCreated()) lineVbo.destroy();
    if (lineVao.isCreated()) lineVao.destroy();
    doneCurrent();
}

void DeckWaveformOverview::initializeGL()
{
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Professional waveform shader with gradients and anti-aliasing
    const char* vsrc = R"GLSL(
        #version 330 core
        layout(location=0) in vec2 aPos; // x in [0,1], y amplitude [0,1]
        layout(location=1) in float aIntensity; // intensity for color variation
        
        uniform vec2 uResolution;
        out vec2 vUV;
        out float vAmp;
        out float vIntensity;
        out vec2 vScreenPos;
        
        void main(){
            float x = aPos.x * 2.0 - 1.0; // NDC x
            float y = aPos.y * 2.0 - 1.0; // NDC y (already mapped)
            gl_Position = vec4(x, y, 0.0, 1.0);
            
            vUV = aPos;
            vAmp = clamp(aPos.y, 0.0, 1.0);
            vIntensity = aIntensity;
            vScreenPos = (gl_Position.xy + 1.0) * 0.5 * uResolution;
        }
    )GLSL";
    
    const char* fsrc = R"GLSL(
        #version 330 core
        in vec2 vUV;
        in float vAmp;
        in float vIntensity;
        in vec2 vScreenPos;
        
        uniform vec3 uBaseColor;
        uniform vec3 uHighlightColor;
        uniform vec2 uResolution;
        uniform float uTime;
        
        out vec4 FragColor;
        
        // Smooth step function for anti-aliasing
        float smoothEdge(float edge, float x) {
            float w = fwidth(x) * 0.5;
            return smoothstep(edge - w, edge + w, x);
        }
        
        // Generate subtle noise for texture
        float noise(vec2 p) {
            return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
        }
        
        void main(){
            // Distance from center line for gradient effect
            float centerDist = abs(vUV.y - 0.5) * 2.0;
            
            // Create vertical gradient from center
            float gradient = 1.0 - pow(centerDist, 1.5);
            gradient = max(gradient, 0.1); // Minimum visibility
            
            // Intensity-based color mixing (higher amplitude = brighter)
            vec3 color = mix(uBaseColor, uHighlightColor, vIntensity * 0.7);
            
            // Add subtle noise texture for realism
            float noiseVal = noise(vScreenPos * 0.1) * 0.05;
            color += noiseVal;
            
            // Amplitude-based brightness
            float brightness = 0.4 + vAmp * 0.6;
            color *= brightness;
            
            // Apply gradient
            color *= gradient;
            
            // Edge softening for anti-aliasing
            float edgeSoft = smoothEdge(0.02, vAmp);
            float alpha = edgeSoft * (0.8 + vIntensity * 0.2);
            
            // Subtle glow effect
            float glow = exp(-centerDist * 3.0) * vIntensity * 0.3;
            color += glow * uHighlightColor;
            
            FragColor = vec4(color, alpha);
        }
    )GLSL";

    program = new QOpenGLShaderProgram(this);
    program->addShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program->addShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program->link();

    // Create VAO/VBO for main waveform
    vao.create();
    vao.bind();
    vbo.create();
    vbo.bind();
    vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    
    program->bind();
    program->enableAttributeArray(0);
    program->setAttributeBuffer(0, GL_FLOAT, 0, 3, sizeof(float)*3); // x, y, intensity
    vao.release();
    vbo.release();
    program->release();

    // Create simple line shader for playhead
    const char* lineVsrc = R"GLSL(
        #version 330 core
        layout(location=0) in vec2 aPos;
        void main(){
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )GLSL";
    const char* lineFsrc = R"GLSL(
        #version 330 core
        uniform vec3 uColor;
        out vec4 FragColor;
        void main(){
            FragColor = vec4(uColor, 0.9);
        }
    )GLSL";
    
    lineProgram = new QOpenGLShaderProgram(this);
    lineProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, lineVsrc);
    lineProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, lineFsrc);
    lineProgram->link();

    // Playhead line VAO/VBO
    lineVao.create();
    lineVao.bind();
    lineVbo.create();
    lineVbo.bind();
    lineVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    lineProgram->bind();
    lineProgram->enableAttributeArray(0);
    lineProgram->setAttributeBuffer(0, GL_FLOAT, 0, 2, sizeof(float)*2);
    lineVao.release();
    lineVbo.release();
    lineProgram->release();
}

void DeckWaveformOverview::resizeGL(int w, int h)
{
    viewportW = std::max(1, w);
    viewportH = std::max(1, h);
    glViewport(0, 0, viewportW, viewportH);
    meshDirty = true;
}

void DeckWaveformOverview::paintGL()
{
    // Professional dark background with subtle gradient
    glClearColor(0.02f, 0.02f, 0.025f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!waveform.empty() && viewportW > 0 && viewportH > 0 && program) {
        rebuildMeshIfNeeded();
        if (vertexCount > 0) {
            program->bind();
            
            // Set uniforms for professional look
            program->setUniformValue("uResolution", QVector2D(viewportW, viewportH));
            program->setUniformValue("uBaseColor", QVector3D(0.2f, 0.4f, 0.8f));      // Professional blue
            program->setUniformValue("uHighlightColor", QVector3D(0.4f, 0.8f, 1.0f)); // Bright highlight
            program->setUniformValue("uTime", (float)(QTime::currentTime().msecsSinceStartOfDay() * 0.001f));
            
            vao.bind();
            glDrawArrays(GL_TRIANGLE_STRIP, 0, vertexCount);
            vao.release();
            program->release();
        }
    }

    // Draw cue points as vertical lines
    if (cuePointsValid && totalLength > 0.0) {
        drawCuePoints();
    }
    
    // Draw ghost loop region first (behind active loop)
    if (ghostLoopEnabled && totalLength > 0.0) {
        drawGhostLoopRegion();
    }
    
    // Draw active loop region
    qDebug() << "DeckWaveformOverview::paintGL loop check: loopEnabled=" << loopEnabled 
             << ", loopStartSec=" << loopStartSec << ", loopEndSec=" << loopEndSec 
             << ", totalLength=" << totalLength;
    if (loopEnabled && totalLength > 0.0) {
        qDebug() << "DeckWaveformOverview::paintGL calling drawLoopRegion";
        drawLoopRegion();
    }
    
    // DEBUG: Always draw a red indicator if loop is enabled (for debugging)
    if (loopEnabled) {
        QPainter debugP(this);
        debugP.setPen(QPen(QColor(255, 0, 0), 2));
        debugP.drawRect(5, 5, 50, 10);
        debugP.drawText(8, 13, QString("LOOP %1-%2").arg(loopStartSec, 0, 'f', 1).arg(loopEndSec, 0, 'f', 1));
    }

    // Professional playhead with glow effect
    if (displayedPlayheadPos >= 0.0f && lineProgram) {
        // Main playhead line
        const float x = (float)(displayedPlayheadPos * 2.0 - 1.0);
        const float verts[4] = { x, -1.0f, x, 1.0f };
        
        lineVao.bind();
        lineVbo.bind();
        if (lineVbo.size() < (int)sizeof(verts)) lineVbo.allocate(sizeof(verts));
        lineVbo.write(0, verts, sizeof(verts));
        
        lineProgram->bind();
        
        // Draw glow effect (thicker, transparent)
        lineProgram->setUniformValue("uColor", QVector3D(0.0f, 1.0f, 0.5f));
        glLineWidth(6.0f);
        glDrawArrays(GL_LINES, 0, 2);
        
        // Draw main line (sharp)
        lineProgram->setUniformValue("uColor", QVector3D(1.0f, 1.0f, 1.0f));
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, 2);
        
        lineProgram->release();
        lineVbo.release();
        lineVao.release();
    }
}

void DeckWaveformOverview::loadAndRenderWaveform()
{
    // No heavy work on UI thread; rely on setWaveformData from background
    // Keep as no-op to avoid blocking. If waveform already set, just refresh.
    meshDirty = true;
    update();
}

void DeckWaveformOverview::loadFile(const QString& path)
{
    currentFilePath = path;
    // Reset audio start offset so it gets recalculated
    audioStartOffset = 0.0;
    QTimer::singleShot(10, this, &DeckWaveformOverview::loadAndRenderWaveform);
}

void DeckWaveformOverview::setPlayhead(double relative)
{
    // Adjust playhead position relative to the audio start offset and visual latency comp
    if (totalLength > 0.0) {
    // No extra visual lead here; we already feed audible-relative time from the host
    double absoluteTime = std::clamp(relative, 0.0, 1.0) * totalLength;
        if (absoluteTime >= audioStartOffset) {
            double displayedDuration = totalLength - audioStartOffset;
            playheadPos = (absoluteTime - audioStartOffset) / displayedDuration;
        } else {
            playheadPos = -1.0; // Hide playhead if before audio start
        }
    } else {
        playheadPos = relative;
    }
    update();
}

void DeckWaveformOverview::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && width() > 0) {
        isDragging = true;
        // fallthrough to move logic
        mouseMoveEvent(event);
    }
}

void DeckWaveformOverview::mouseMoveEvent(QMouseEvent* event)
{
    if (!isDragging || width() <= 0) return;

    double relativeInDisplay = event->position().x() / width();
    relativeInDisplay = std::clamp(relativeInDisplay, 0.0, 1.0);

    double absoluteRelative = relativeInDisplay;
    if (totalLength > 0.0) {
        double displayedDuration = totalLength - audioStartOffset;
        double absoluteTime = audioStartOffset + (relativeInDisplay * displayedDuration);
        absoluteRelative = absoluteTime / totalLength;
    }

    // Update the immediate displayed position for responsiveness
    displayedPlayheadPos = absoluteRelative;
    // Keep internal playhead in sync so smoothing doesn't pull it back while paused
    playheadPos = absoluteRelative;
    // Emit to host/transport to actually seek
    emit positionClicked(absoluteRelative);
}

void DeckWaveformOverview::mouseReleaseEvent(QMouseEvent* event)
{
    Q_UNUSED(event);
    isDragging = false;
}

void DeckWaveformOverview::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void DeckWaveformOverview::dropEvent(QDropEvent* event) {
    auto urls = event->mimeData()->urls();
    if (!urls.isEmpty()) {
        auto path = urls.first().toLocalFile();
        emit fileDropped(path);
    }
}

void DeckWaveformOverview::setBeatInfo(double bpm_, double firstBeatOffset_, double totalLength_) {
    bpm = bpm_;
    firstBeatOffset = firstBeatOffset_;
    totalLength = totalLength_;
    
    // Don't override audioStartOffset here - it's already calculated from actual audio start
    // Just reload to make sure we have the most recent info
    if (audioStartOffset == 0.0) {
        // If we haven't detected audio start yet, reload to detect it
        loadAndRenderWaveform();
    }
}

void DeckWaveformOverview::setAudioStartOffset(double audioStartTime) {
    audioStartOffset = audioStartTime;
    loadAndRenderWaveform();
}

// NEW: Cue points support
void DeckWaveformOverview::setCuePoints(const std::array<double, 8>& newCuePoints) {
    cuePoints = newCuePoints;
    cuePointsValid = true;
    update();
}

void DeckWaveformOverview::clearCuePoints() {
    cuePoints.fill(-1.0);
    cuePointsValid = false;
    update();
}

// NEW: Loop region support
void DeckWaveformOverview::setLoopRegion(bool enabled, double startSec, double endSec) {
    std::cout << "DeckWaveformOverview::setLoopRegion called - enabled: " << enabled 
              << ", startSec: " << startSec << ", endSec: " << endSec << std::endl;
    loopEnabled = enabled;
    loopStartSec = startSec;
    loopEndSec = endSec;
    std::cout << "DeckWaveformOverview loop state updated: loopEnabled=" << loopEnabled 
              << ", loopStartSec=" << loopStartSec << ", loopEndSec=" << loopEndSec << std::endl;
    update();
}

void DeckWaveformOverview::clearLoop() {
    loopEnabled = false;
    loopStartSec = 0.0;
    loopEndSec = 0.0;
    update();
}

// NEW: Ghost loop region support
void DeckWaveformOverview::setGhostLoopRegion(bool enabled, double startSec, double endSec) {
    ghostLoopEnabled = enabled;
    ghostLoopStartSec = startSec;
    ghostLoopEndSec = endSec;
    update();
}

void DeckWaveformOverview::rebuildMeshIfNeeded()
{
    if (!meshDirty || waveform.empty()) return;
    meshDirty = false;
    
    // Build high-quality triangle strip with intensity data
    std::vector<float> verts;
    const size_t n = waveform.size();
    
    // Pre-allocate for performance: 3 floats per vertex, 2 vertices per sample
    verts.reserve(n * 2 * 3);
    
    // Calculate intensity (derivative-based for dynamic highlighting)
    std::vector<float> intensity(n, 0.0f);
    for (size_t i = 1; i < n - 1; ++i) {
        float derivative = std::abs(waveform[i + 1] - waveform[i - 1]);
        intensity[i] = std::min(1.0f, derivative * 8.0f); // Scale for visibility
    }
    
    for (size_t i = 0; i < n; ++i) {
        float x = (float)i / (float)(n - 1); // 0..1
        float amplitude = std::min(1.0f, waveform[i]);
        float intens = intensity[i];
        
        // Create triangle strip: bottom vertex at center (0.5), top at amplitude
        float yCenter = 0.5f;  // Center line in [0,1] space
        float yTop = 0.5f + amplitude * 0.45f; // Upper half with margin
        
        // Bottom vertex (center line)
        verts.push_back(x);
        verts.push_back(yCenter);
        verts.push_back(intens * 0.3f); // Lower intensity at center
        
        // Top vertex (amplitude peak)
        verts.push_back(x);
        verts.push_back(yTop);
        verts.push_back(intens);
    }

    vertexCount = (int)(verts.size() / 3);
    
    vao.bind();
    vbo.bind();
    const int bytes = (int)verts.size() * (int)sizeof(float);
    if (vbo.size() < bytes) vbo.allocate(bytes);
    vbo.write(0, verts.data(), bytes);
    
    program->bind();
    program->enableAttributeArray(0);
    program->setAttributeBuffer(0, GL_FLOAT, 0, 3, sizeof(float)*3);
    program->release();
    
    vbo.release();
    vao.release();
}

// NEW: Draw cue points as vertical lines using simple Qt painting over OpenGL
void DeckWaveformOverview::drawCuePoints() {
    // Use QPainter over OpenGL context for simple line drawing
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    qDebug() << "DeckWaveformOverview::drawLoopRegion called: loopEnabled=" << loopEnabled << ", loopStartSec=" << loopStartSec << ", loopEndSec=" << loopEndSec;
    
    // Define cue point colors (same as WaveformDisplay for consistency)
    static const QColor cueColors[8] = {
        QColor(255, 80, 80),   // Red
        QColor(255, 150, 80),  // Orange  
        QColor(255, 220, 80),  // Yellow
        QColor(150, 255, 80),  // Light Green
        QColor(80, 255, 150),  // Cyan
        QColor(80, 180, 255),  // Blue
        QColor(150, 80, 255),  // Purple
        QColor(255, 80, 200)   // Magenta
    };
    
    for (int i = 0; i < 8; ++i) {
        if (cuePoints[i] < 0.0) continue; // Skip unset cue points
        
        double cueTimeSec = cuePoints[i];
        
        // Calculate relative position in track (accounting for audio start offset)
        double effectiveLength = totalLength - audioStartOffset;
        if (effectiveLength <= 0.0) continue;
        
        double relativePos = 0.0;
        if (cueTimeSec >= audioStartOffset) {
            relativePos = (cueTimeSec - audioStartOffset) / effectiveLength;
        } else {
            continue; // Cue point before audio start, don't show
        }
        
        if (relativePos < 0.0 || relativePos > 1.0) continue;
        
        // Calculate screen position
        int screenX = (int)(relativePos * width());
        
        // Draw cue line
        QPen cuePen(cueColors[i], 1.5);
        cuePen.setStyle(Qt::SolidLine);
        p.setPen(cuePen);
        p.drawLine(screenX, 0, screenX, height());
        
        // Draw small cue number at bottom
        p.setFont(QFont("Arial", 6, QFont::Bold));
        QString cueLabel = QString::number(i + 1);
        QRect labelRect = p.fontMetrics().boundingRect(cueLabel);
        
        // Position label at bottom, centered on line
        int labelX = screenX - labelRect.width() / 2;
        int labelY = height() - 2;
        
        // Draw label background for better readability
        QRect bgRect(labelX - 1, labelY - labelRect.height(), labelRect.width() + 2, labelRect.height());
        p.fillRect(bgRect, QColor(0, 0, 0, 200));
        
        // Draw label text
        p.setPen(QPen(cueColors[i], 1));
        p.drawText(labelX, labelY - 1, cueLabel);
    }
}

// NEW: Draw loop region as semi-transparent box
void DeckWaveformOverview::drawLoopRegion() {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    
    // Calculate relative positions in track (accounting for audio start offset)
    double effectiveLength = totalLength - audioStartOffset;
    if (effectiveLength <= 0.0) return;
    
    double relativeStart = 0.0;
    double relativeEnd = 0.0;
    
    if (loopStartSec >= audioStartOffset) {
        relativeStart = (loopStartSec - audioStartOffset) / effectiveLength;
    } else {
        return; // Loop start before audio start, don't show
    }
    
    if (loopEndSec >= audioStartOffset) {
        relativeEnd = (loopEndSec - audioStartOffset) / effectiveLength;
    } else {
        return; // Loop end before audio start, don't show
    }
    
    if (relativeStart < 0.0 || relativeStart > 1.0 || 
        relativeEnd < 0.0 || relativeEnd > 1.0 || 
        relativeEnd <= relativeStart) return;
    
    // Calculate screen positions
    int screenStartX = (int)(relativeStart * width());
    int screenEndX = (int)(relativeEnd * width());
    
    if (screenEndX <= screenStartX) return;
    
    // Draw semi-transparent loop region
    QColor loopColor(100, 255, 100, 60); // Green with 60/255 transparency (less than main waveform)
    p.fillRect(screenStartX, 0, screenEndX - screenStartX, height(), loopColor);
    
    // Draw loop boundaries with more opaque lines
    QPen loopBoundaryPen(QColor(0, 200, 0, 180), 1.5);
    loopBoundaryPen.setStyle(Qt::SolidLine);
    p.setPen(loopBoundaryPen);
    
    // Draw start and end lines
    p.drawLine(screenStartX, 0, screenStartX, height());
    p.drawLine(screenEndX, 0, screenEndX, height());
    
    // Draw small "L" label at top of loop region (compact for overview)
    p.setFont(QFont("Arial", 6, QFont::Bold));
    QString loopLabel = "L";
    QRect labelRect = p.fontMetrics().boundingRect(loopLabel);
    
    // Position label near start of loop region
    int labelX = screenStartX + 2;
    int labelY = 12;
    
    // Draw label background
    QRect bgRect(labelX - 1, labelY - labelRect.height(), labelRect.width() + 2, labelRect.height());
    p.fillRect(bgRect, QColor(0, 0, 0, 180));
    
    // Draw label text
    p.setPen(QPen(QColor(100, 255, 100), 1));
    p.drawText(labelX, labelY - 1, loopLabel);
}

// NEW: Draw ghost loop region as very transparent box for last used loop
void DeckWaveformOverview::drawGhostLoopRegion() {
    if (!ghostLoopEnabled) return;
    
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    
    // Calculate relative positions in track (accounting for audio start offset)
    double effectiveLength = totalLength - audioStartOffset;
    if (effectiveLength <= 0.0) return;
    
    double relativeStart = 0.0;
    double relativeEnd = 0.0;
    
    if (ghostLoopStartSec >= audioStartOffset) {
        relativeStart = (ghostLoopStartSec - audioStartOffset) / effectiveLength;
    } else {
        return; // Ghost loop start before audio start, don't show
    }
    
    if (ghostLoopEndSec >= audioStartOffset) {
        relativeEnd = (ghostLoopEndSec - audioStartOffset) / effectiveLength;
    } else {
        return; // Ghost loop end before audio start, don't show
    }
    
    if (relativeStart < 0.0 || relativeStart > 1.0 || 
        relativeEnd < 0.0 || relativeEnd > 1.0 || 
        relativeEnd <= relativeStart) return;
    
    // Calculate screen positions
    int screenStartX = (int)(relativeStart * width());
    int screenEndX = (int)(relativeEnd * width());
    
    if (screenEndX <= screenStartX) return;
    
    // Draw very transparent ghost loop region (much lighter than active loop)
    QColor ghostLoopColor(100, 255, 100, 25); // Green with 25/255 transparency (lighter than active)
    p.fillRect(screenStartX, 0, screenEndX - screenStartX, height(), ghostLoopColor);
    
    // Draw ghost loop boundaries with lighter opacity
    QPen ghostBoundaryPen(QColor(0, 200, 0, 60), 1.0);
    ghostBoundaryPen.setStyle(Qt::DashLine); // Use dashed line to distinguish from active loop
    p.setPen(ghostBoundaryPen);
    
    // Draw start and end lines
    p.drawLine(screenStartX, 0, screenStartX, height());
    p.drawLine(screenEndX, 0, screenEndX, height());
    
    // Draw small "G" label at top of ghost loop region (compact for overview)
    p.setFont(QFont("Arial", 5, QFont::Normal)); // Even smaller than active loop
    QString ghostLabel = "G";
    QRect labelRect = p.fontMetrics().boundingRect(ghostLabel);
    
    // Position label near start of ghost loop region, slightly offset from active loop
    int labelX = screenStartX + 2;
    int labelY = 22; // Lower than active loop label
    
    // Draw label background with lower opacity
    QRect bgRect(labelX - 1, labelY - labelRect.height(), labelRect.width() + 2, labelRect.height());
    p.fillRect(bgRect, QColor(0, 0, 0, 80)); // Less opaque background
    
    // Draw label text with lighter color
    p.setPen(QPen(QColor(100, 255, 100, 100), 1)); // More transparent text
    p.drawText(labelX, labelY - 1, ghostLabel);
}
