#include "DeckWaveformOverview.h"
#include "FrameTiming.h"
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include "WaveformGenerator.h"
#include "WaveformTheme.h"
#include <QPainter>
#include <QTimer>
#include <QTime>
#include <QMimeData>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

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
    smoothTimer->setInterval(FrameTiming::kFrameIntervalMs);
    connect(smoothTimer, &QTimer::timeout, this, [this]() {
        // PREROLL SUPPORT: Allow animation for negative positions (preroll)
        // Initialize displayedPlayheadPos to current position if not set
        if (std::isnan(displayedPlayheadPos) || std::abs(displayedPlayheadPos) > 999.0) {
            displayedPlayheadPos = playheadPos;
        }
        
        double diff = playheadPos - displayedPlayheadPos;
        // Exponential smoothing: responsive but stable (works for negative positions too)
        const double alpha = 0.35; // 0..1, higher = faster follow
        displayedPlayheadPos += diff * alpha;
        // Snap when very close to avoid micro-jitter
        if (std::abs(playheadPos - displayedPlayheadPos) < 0.0008) displayedPlayheadPos = playheadPos;
        update();
    });
    smoothTimer->start();

    overlayTitle.clear();
    overlayDetail.clear();
    overlayShowProgress = false;
    overlayProgress = 0.0;
    overlayFailed = false;
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
            layout(location=1) in vec3 aColor; // RGB colour per column
            
            uniform vec2 uResolution;
            out vec2 vUV;
            out float vAmp;
            out vec3 vColor;
            out vec2 vScreenPos;
            
            void main(){
                float x = aPos.x * 2.0 - 1.0; // NDC x
                float y = aPos.y * 2.0 - 1.0; // NDC y (already mapped)
                gl_Position = vec4(x, y, 0.0, 1.0);
                
                vUV = aPos;
                vAmp = clamp(aPos.y, 0.0, 1.0);
                vColor = aColor;
                vScreenPos = (gl_Position.xy + 1.0) * 0.5 * uResolution;
            }
        )GLSL";
        
        const char* fsrc = R"GLSL(
            #version 330 core
            in vec2 vUV;
            in float vAmp;
            in vec3 vColor;
            in vec2 vScreenPos;
            
            uniform vec2 uResolution;
            uniform float uTime;
            
            out vec4 FragColor;
            
            float smoothEdge(float edge, float x) {
                float w = fwidth(x) * 0.5;
                return smoothstep(edge - w, edge + w, x);
            }
            
            float noise(vec2 p) {
                return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
            }
            
            void main(){
                float centerDist = abs(vUV.y - 0.5) * 2.0;
                float gradient = 1.0 - pow(centerDist, 1.5);
                gradient = max(gradient, 0.1);

                vec3 color = vColor;
                float noiseVal = noise(vScreenPos * 0.1 + uTime * 0.015) * 0.04;
                color += noiseVal;

                float brightness = 0.45 + vAmp * 0.55;
                color *= brightness;
                color *= gradient;

                float edgeSoft = smoothEdge(0.02, vAmp);
                float alpha = edgeSoft * (0.75 + vAmp * 0.25);
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
    program->setAttributeBuffer(0, GL_FLOAT, 0, 2, sizeof(float) * 5); // x, y
    program->enableAttributeArray(1);
    program->setAttributeBuffer(1, GL_FLOAT, sizeof(float) * 2, 3, sizeof(float) * 5); // r, g, b
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
    if (loopEnabled && totalLength > 0.0) {
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
    // PREROLL SUPPORT: Show playhead even in preroll (negative positions)
    if (lineProgram && displayedPlayheadPos > -999.0) { // Show for any valid position including preroll
        // Main playhead line - handles negative positions (preroll) correctly
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

    if (!overlayTitle.isEmpty() || !overlayDetail.isEmpty() || overlayShowProgress) {
        QPainter overlayPainter(this);
        overlayPainter.setRenderHint(QPainter::Antialiasing, true);

        const int padding = 8;
        const int spacing = 4;
        const int progressHeight = 6;

        QFont titleFont("Lato", 9, QFont::Bold);
        QFont detailFont("Lato", 8, QFont::Normal);

        QString titleText = overlayTitle;
        QString detailText = overlayDetail;

        const int maxTextWidth = std::max(40, width() - padding * 4);

        overlayPainter.setFont(titleFont);
        if (!titleText.isEmpty()) {
            titleText = overlayPainter.fontMetrics().elidedText(titleText, Qt::ElideRight, maxTextWidth);
        }
        QRect titleRect = titleText.isEmpty()
            ? QRect(0, 0, 0, overlayPainter.fontMetrics().height())
            : overlayPainter.fontMetrics().boundingRect(titleText);

        overlayPainter.setFont(detailFont);
        if (!detailText.isEmpty()) {
            detailText = overlayPainter.fontMetrics().elidedText(detailText, Qt::ElideRight, maxTextWidth);
        }
        QRect detailRect = detailText.isEmpty()
            ? QRect(0, 0, 0, overlayPainter.fontMetrics().height())
            : overlayPainter.fontMetrics().boundingRect(detailText);

        int contentWidth = std::max(titleRect.width(), detailRect.width());
        int contentHeight = 0;

        if (!titleText.isEmpty()) {
            contentHeight += titleRect.height();
        }
        if (!detailText.isEmpty()) {
            if (contentHeight > 0) contentHeight += spacing;
            contentHeight += detailRect.height();
        }
        if (overlayShowProgress) {
            if (contentHeight > 0) contentHeight += spacing;
            contentHeight += progressHeight;
            contentWidth = std::max(contentWidth, width() / 5);
        }

        QRect infoRect(padding,
                       padding,
                       contentWidth + padding * 2,
                       contentHeight + padding * 2);

        overlayPainter.setPen(Qt::NoPen);
        overlayPainter.setBrush(QColor(12, 16, 30, 180));
        overlayPainter.drawRoundedRect(infoRect, 7, 7);

        int cursorY = infoRect.top() + padding;
        int textX = infoRect.left() + padding;

        if (!titleText.isEmpty()) {
            overlayPainter.setPen(QColor(215, 225, 255));
            overlayPainter.setFont(titleFont);
            cursorY += titleRect.height();
            overlayPainter.drawText(textX, cursorY, titleText);
        }

        if (!detailText.isEmpty()) {
            if (!titleText.isEmpty()) cursorY += spacing;
            overlayPainter.setPen(overlayFailed ? QColor(255, 140, 140)
                                                : QColor(180, 190, 210));
            overlayPainter.setFont(detailFont);
            cursorY += detailRect.height();
            overlayPainter.drawText(textX, cursorY, detailText);
        }

        if (overlayShowProgress) {
            if (!titleText.isEmpty() || !detailText.isEmpty()) cursorY += spacing;
            int barWidth = infoRect.width() - padding * 2;
            QRect progressRect(textX, cursorY, barWidth, progressHeight);

            overlayPainter.setPen(Qt::NoPen);
            overlayPainter.setBrush(QColor(35, 45, 70));
            overlayPainter.drawRoundedRect(progressRect, 3, 3);

            int filledWidth = (int)std::round(std::clamp(overlayProgress, 0.0, 1.0) * barWidth);
            if (filledWidth > 0) {
                QRect filledRect(progressRect.left(), progressRect.top(), filledWidth, progressRect.height());
                overlayPainter.setBrush(QColor(0, 190, 255));
                overlayPainter.drawRoundedRect(filledRect, 3, 3);
            }
        }
    }
}

void DeckWaveformOverview::loadAndRenderWaveform()
{
    // No heavy work on UI thread; rely on setWaveformData from background
    // Keep as no-op to avoid blocking. If waveform already set, just refresh.
    meshDirty = true;
    update();
}

void DeckWaveformOverview::setWaveformData(const std::vector<float>& amplitudes,
                                           const std::vector<float>& colours,
                                           double audioStartOffsetSec,
                                           double lengthSec)
{
    waveform = amplitudes;
    waveformColors = colours;
    audioStartOffset = audioStartOffsetSec;
    totalLength = lengthSec;
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
    // PREROLL SUPPORT: Allow negative positions for DJ-style cueing
    // Don't clamp to 0.0-1.0 range; support unlimited preroll like main WaveformDisplay
    
    if (totalLength > 0.0) {
        // Handle preroll positions (negative relative values)
        if (relative < 0.0) {
            // In preroll: show playhead position proportionally in the preroll area
            // Map preroll range [-1.0, 0.0] to display range [-1.0, 0.0]
            playheadPos = relative; // Direct mapping for preroll
        } else {
            // Normal playback: adjust for audio start offset
            double absoluteTime = relative * totalLength;
            if (absoluteTime >= audioStartOffset) {
                double displayedDuration = totalLength - audioStartOffset;
                playheadPos = (absoluteTime - audioStartOffset) / displayedDuration;
            } else {
                playheadPos = -0.1; // Show playhead slightly before start
            }
        }
    } else {
        // Fallback: direct relative positioning
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
    
    // Build high-quality triangle strip with per-column colours
    std::vector<float> verts;
    const size_t n = waveform.size();
    const auto fallbackColour = WaveformTheme::fallbackColor();
    const bool hasColours = waveformColors.size() >= n * 3;
    
    // Pre-allocate for performance: 5 floats per vertex, 2 vertices per sample
    verts.reserve(n * 2 * 5);
    
    for (size_t i = 0; i < n; ++i) {
        float x = (float)i / (float)(n - 1); // 0..1
        float amplitude = std::min(1.0f, waveform[i]);
        const size_t colorIndex = i * 3;
        float r = fallbackColour.r;
        float g = fallbackColour.g;
        float b = fallbackColour.b;
        if (hasColours && colorIndex + 2 < waveformColors.size()) {
            r = waveformColors[colorIndex + 0];
            g = waveformColors[colorIndex + 1];
            b = waveformColors[colorIndex + 2];
        }
        
        // Create triangle strip: bottom vertex at center (0.5), top at amplitude
        float yCenter = 0.5f;  // Center line in [0,1] space
        float yTop = 0.5f + amplitude * 0.45f; // Upper half with margin
        
        // Bottom vertex (center line)
        verts.push_back(x);
        verts.push_back(yCenter);
        verts.push_back(r);
        verts.push_back(g);
        verts.push_back(b);
        
        // Top vertex (amplitude peak)
        verts.push_back(x);
        verts.push_back(yTop);
        verts.push_back(r);
        verts.push_back(g);
        verts.push_back(b);
    }

    vertexCount = (int)(verts.size() / 5);
    
    vao.bind();
    vbo.bind();
    const int bytes = (int)verts.size() * (int)sizeof(float);
    if (vbo.size() < bytes) vbo.allocate(bytes);
    vbo.write(0, verts.data(), bytes);
    
    program->bind();
    program->enableAttributeArray(0);
    program->setAttributeBuffer(0, GL_FLOAT, 0, 2, sizeof(float) * 5);
    program->enableAttributeArray(1);
    program->setAttributeBuffer(1, GL_FLOAT, sizeof(float) * 2, 3, sizeof(float) * 5);
    program->release();
    
    vbo.release();
    vao.release();
}

// NEW: Draw cue points as vertical lines using simple Qt painting over OpenGL
void DeckWaveformOverview::drawCuePoints() {
    // Use QPainter over OpenGL context for simple line drawing
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    
    const auto& cueColors = WaveformTheme::cueColors();
    
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
    QColor loopColor = WaveformTheme::loopBaseColor();
    loopColor.setAlpha(60); // Less opaque than main waveform
    p.fillRect(screenStartX, 0, screenEndX - screenStartX, height(), loopColor);
    
    // Draw loop boundaries with more opaque lines
    QColor loopStroke = WaveformTheme::loopBorderColor();
    loopStroke.setAlpha(180);
    QPen loopBoundaryPen(loopStroke, 1.5);
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
    p.setPen(QPen(WaveformTheme::loopBaseColor(), 1));
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
    QColor ghostLoopColor = WaveformTheme::ghostLoopBaseColor();
    ghostLoopColor.setAlpha(25); // Lighter than active loop
    p.fillRect(screenStartX, 0, screenEndX - screenStartX, height(), ghostLoopColor);
    
    // Draw ghost loop boundaries with lighter opacity
    QColor ghostStroke = WaveformTheme::ghostLoopBorderColor();
    ghostStroke.setAlpha(60);
    QPen ghostBoundaryPen(ghostStroke, 1.0);
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
    QColor ghostText = WaveformTheme::ghostLoopBaseColor();
    ghostText.setAlpha(100);
    p.setPen(QPen(ghostText, 1));
    p.drawText(labelX, labelY - 1, ghostLabel);
}

void DeckWaveformOverview::setOverlayStatus(const QString& title, const QString& detail,
                                            bool showProgress, double progress, bool failed)
{
    overlayTitle = title;
    overlayDetail = detail;
    overlayShowProgress = showProgress;
    overlayProgress = std::clamp(progress, 0.0, 1.0);
    overlayFailed = failed;
    update();
}
