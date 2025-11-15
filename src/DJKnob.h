#pragma once

#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <cmath>

class DJKnob : public QWidget {
    Q_OBJECT
    
public:
    explicit DJKnob(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(32, 32);
        setMouseTracking(true);
    }
    
    void setRange(int min, int max) {
        m_minimum = min;
        m_maximum = max;
        m_value = std::clamp(m_value, m_minimum, m_maximum);
        update();
    }
    
    void setValue(int value) {
        int newValue = std::clamp(value, m_minimum, m_maximum);
        if (newValue != m_value) {
            m_value = newValue;
            emit valueChanged(m_value);
            update();
        }
    }
    
    int value() const { return m_value; }
    
signals:
    void valueChanged(int value);
    
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        const int size = std::min(width(), height());
        const int cx = width() / 2;
        const int cy = height() / 2;
        const int radius = size / 2 - 4;
        
        // Calculate angle from value (top is 0/neutral, -135° to +135° range, 270° total)
        double normalized = (double)(m_value - m_minimum) / (double)(m_maximum - m_minimum);
        double angle = -225.0 + (normalized * 270.0); // Start at -225° (top), end at +45°
        
        // Draw value arc (shows how far the knob is turned)
        QPainterPath arcPath;
        QRectF arcRect(cx - radius - 2, cy - radius - 2, (radius + 2) * 2, (radius + 2) * 2);
        
        // Arc goes from top (90° in Qt coordinates) to current position
        // Qt angles: 0° = 3 o'clock, 90° = 12 o'clock (top), counterclockwise
        double startAngle = 90.0 * 16; // Top position in Qt (90°)
        double spanAngle = -(angle - (-90.0)) * 16; // Negative span for correct direction
        
        arcPath.arcMoveTo(arcRect, startAngle / 16.0);
        arcPath.arcTo(arcRect, startAngle / 16.0, spanAngle / 16.0);
        
        painter.setPen(QPen(QColor(180, 100, 255), 3, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(arcPath);
        
        // Draw flat knob body (simple circle)
        painter.setBrush(QColor(40, 40, 45));
        painter.setPen(QPen(QColor(60, 60, 65), 2));
        painter.drawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
        
        // Draw indicator line sticking out from the circle
        double angleRad = angle * M_PI / 180.0;
        int lineStart = radius - 5;
        int lineEnd = radius + 6; // Extends beyond the circle
        
        int startX = cx + (int)(std::cos(angleRad) * lineStart);
        int startY = cy + (int)(std::sin(angleRad) * lineStart);
        int endX = cx + (int)(std::cos(angleRad) * lineEnd);
        int endY = cy + (int)(std::sin(angleRad) * lineEnd);
        
        painter.setPen(QPen(QColor(200, 120, 255), 2.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(startX, startY, endX, endY);
    }
    
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_lastY = event->pos().y();
            setCursor(Qt::SizeVerCursor);
        }
        event->accept();
    }
    
    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_dragging) {
            int delta = m_lastY - event->pos().y();
            int range = m_maximum - m_minimum;
            int newValue = m_value + (delta * range) / 200; // sensitivity adjustment
            setValue(newValue);
            m_lastY = event->pos().y();
        }
        event->accept();
    }
    
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = false;
            setCursor(Qt::ArrowCursor);
        }
        event->accept();
    }
    
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            setValue(0); // Reset to center
        }
        event->accept();
    }
    
private:
    int m_minimum = -100;
    int m_maximum = 100;
    int m_value = 0;
    bool m_dragging = false;
    int m_lastY = 0;
};
