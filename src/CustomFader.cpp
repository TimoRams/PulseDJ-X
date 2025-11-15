#include "CustomFader.h"
#include <QPainter>
#include <QLinearGradient>
#include <algorithm>

CustomFader::CustomFader(Orientation orientation, QWidget *parent)
    : QWidget(parent)
    , m_orientation(orientation)
    , m_minimum(0)
    , m_maximum(100)
    , m_value(50)
    , m_dragging(false)
{
    setMouseTracking(true);
    
    if (m_orientation == Vertical) {
        setMinimumSize(30, 100);
        setMaximumWidth(30);
    } else {
        setMinimumSize(100, 24);
        setMaximumHeight(24);
    }
}

void CustomFader::setMinimum(int min)
{
    m_minimum = min;
    update();
}

void CustomFader::setMaximum(int max)
{
    m_maximum = max;
    update();
}

void CustomFader::setValue(int value)
{
    int newValue = std::clamp(value, m_minimum, m_maximum);
    if (m_value != newValue) {
        m_value = newValue;
        update();
        emit valueChanged(m_value);
    }
}

void CustomFader::setOrientation(Orientation orientation)
{
    m_orientation = orientation;
    
    if (m_orientation == Vertical) {
        setMinimumSize(30, 100);
        setMaximumWidth(30);
        setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        setMinimumSize(100, 24);
        setMaximumHeight(24);
        setMaximumWidth(QWIDGETSIZE_MAX);
    }
    
    update();
}

void CustomFader::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    int trackWidth = TRACK_WIDTH;
    int handleWidth = HANDLE_WIDTH;
    int handleHeight = HANDLE_HEIGHT;
    
    if (m_orientation == Vertical) {
        // Vertical fader
        int trackX = (width() - trackWidth) / 2;
        int trackY = handleHeight / 2;
        int trackHeight = height() - handleHeight;
        
        // Draw track background (dark groove) - flatter
        painter.fillRect(trackX, trackY, trackWidth, trackHeight, QColor(20, 20, 20));
        
        // Draw track border - subtle
        painter.setPen(QColor(50, 50, 50));
        painter.drawRect(trackX, trackY, trackWidth, trackHeight);
        
        // Calculate handle position (inverted: top = max, bottom = min)
        int handleY = valueToPixel(m_value);
        
        // Draw center line at 0 position (if range includes 0)
        if (m_minimum < 0 && m_maximum > 0) {
            int zeroY = valueToPixel(0);
            painter.setPen(QPen(QColor(100, 100, 100), 1, Qt::DashLine));
            painter.drawLine(trackX - 5, zeroY, trackX + trackWidth + 5, zeroY);
        }
        
        // Draw handle
        int handleX = (width() - handleWidth) / 2;
        QRect handleRect(handleX, handleY - handleHeight/2, handleWidth, handleHeight);
        
        // Flat handle with minimal gradient
        QLinearGradient gradient(handleRect.topLeft(), handleRect.bottomRight());
        gradient.setColorAt(0.0, QColor(70, 70, 70));
        gradient.setColorAt(1.0, QColor(55, 55, 55));
        painter.fillRect(handleRect, gradient);
        
        // Handle border - subtle
        painter.setPen(QColor(90, 90, 90));
        painter.drawRect(handleRect);
        
        // Draw single centered line on handle
        painter.setPen(QPen(QColor(35, 35, 35), 1));
        int centerY = handleY;
        painter.drawLine(handleX + 6, centerY, handleX + handleWidth - 6, centerY);
        
    } else {
        // Horizontal fader (crossfader)
        int trackY = (height() - trackWidth) / 2;
        int trackX = handleWidth / 2;
        int trackLength = width() - handleWidth;
        
        // Draw track background - flatter
        painter.fillRect(trackX, trackY, trackLength, trackWidth, QColor(20, 20, 20));
        
        // Draw track border - subtle
        painter.setPen(QColor(50, 50, 50));
        painter.drawRect(trackX, trackY, trackLength, trackWidth);
        
        // Calculate handle position
        int handleX = valueToPixel(m_value);
        
        // Draw center line at 0 position
        if (m_minimum < 0 && m_maximum > 0) {
            int zeroX = valueToPixel(0);
            painter.setPen(QPen(QColor(100, 100, 100), 1, Qt::DashLine));
            painter.drawLine(zeroX, trackY - 5, zeroX, trackY + trackWidth + 5);
        }
        
        // Draw handle
        int handleY = (height() - handleHeight) / 2;
        QRect handleRect(handleX - handleWidth/2, handleY, handleWidth, handleHeight);
        
        // Flat handle with minimal gradient
        QLinearGradient gradient(handleRect.topLeft(), handleRect.bottomRight());
        gradient.setColorAt(0.0, QColor(70, 70, 70));
        gradient.setColorAt(1.0, QColor(55, 55, 55));
        painter.fillRect(handleRect, gradient);
        
        // Handle border - subtle
        painter.setPen(QColor(90, 90, 90));
        painter.drawRect(handleRect);
        
        // Draw single centered line on handle
        painter.setPen(QPen(QColor(35, 35, 35), 1));
        int centerX = handleX;
        painter.drawLine(centerX, handleY + 3, centerX, handleY + handleHeight - 3);
    }
}

void CustomFader::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        updateValueFromPosition(event->pos());
    }
}

void CustomFader::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        updateValueFromPosition(event->pos());
    }
}

void CustomFader::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

void CustomFader::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        // Reset to center position if range includes 0, otherwise to middle
        if (m_minimum < 0 && m_maximum > 0) {
            setValue(0);  // Reset to center (0) for crossfader
        } else {
            setValue((m_minimum + m_maximum) / 2);  // Reset to middle for volume faders
        }
    }
}

void CustomFader::updateValueFromPosition(const QPoint &pos)
{
    int pixel = m_orientation == Vertical ? pos.y() : pos.x();
    setValue(pixelToValue(pixel));
}

int CustomFader::valueToPixel(int value) const
{
    if (m_orientation == Vertical) {
        int trackHeight = height() - HANDLE_HEIGHT;
        int trackTop = HANDLE_HEIGHT / 2;
        // Invert: max at top, min at bottom
        float ratio = static_cast<float>(m_maximum - value) / (m_maximum - m_minimum);
        return trackTop + static_cast<int>(ratio * trackHeight);
    } else {
        int trackLength = width() - HANDLE_WIDTH;
        int trackLeft = HANDLE_WIDTH / 2;
        float ratio = static_cast<float>(value - m_minimum) / (m_maximum - m_minimum);
        return trackLeft + static_cast<int>(ratio * trackLength);
    }
}

int CustomFader::pixelToValue(int pixel) const
{
    if (m_orientation == Vertical) {
        int trackHeight = height() - HANDLE_HEIGHT;
        int trackTop = HANDLE_HEIGHT / 2;
        pixel = std::clamp(pixel, trackTop, trackTop + trackHeight);
        // Invert: top = max, bottom = min
        float ratio = static_cast<float>(pixel - trackTop) / trackHeight;
        return m_maximum - static_cast<int>(ratio * (m_maximum - m_minimum));
    } else {
        int trackLength = width() - HANDLE_WIDTH;
        int trackLeft = HANDLE_WIDTH / 2;
        pixel = std::clamp(pixel, trackLeft, trackLeft + trackLength);
        float ratio = static_cast<float>(pixel - trackLeft) / trackLength;
        return m_minimum + static_cast<int>(ratio * (m_maximum - m_minimum));
    }
}
