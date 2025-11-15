#pragma once

#include <QWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOption>

class CustomFader : public QWidget
{
    Q_OBJECT

public:
    enum Orientation {
        Vertical,
        Horizontal
    };

    explicit CustomFader(Orientation orientation = Vertical, QWidget *parent = nullptr);

    void setMinimum(int min);
    void setMaximum(int max);
    void setValue(int value);
    int value() const { return m_value; }
    void setOrientation(Orientation orientation);

signals:
    void valueChanged(int value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void updateValueFromPosition(const QPoint &pos);
    int valueToPixel(int value) const;
    int pixelToValue(int pixel) const;

    Orientation m_orientation;
    int m_minimum;
    int m_maximum;
    int m_value;
    bool m_dragging;
    
    // Visual constants - smaller and more elegant
    static constexpr int TRACK_WIDTH = 4;
    static constexpr int HANDLE_WIDTH = 22;
    static constexpr int HANDLE_HEIGHT = 12;
};
