#include "MainWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QHoverEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QWindow>

#include <iostream>

#include <algorithm>
#include <limits>

bool QtMainWindow::eventFilter(QObject* obj, QEvent* event)
{
    bool handled = false;

    if (auto widget = qobject_cast<QWidget*>(obj))
    {
        if (widget->window() == this)
        {
            if (!widget->hasMouseTracking())
                widget->setMouseTracking(true);
            widget->setAttribute(Qt::WA_Hover, true);

            const bool isButton = static_cast<bool>(qobject_cast<QAbstractButton*>(widget));
            const bool isMenu = static_cast<bool>(qobject_cast<QMenu*>(widget));
            QWidget* topRightCorner = menuBar ? menuBar->cornerWidget(Qt::TopRightCorner) : nullptr;
            const bool isWindowControl = topRightCorner && (widget == topRightCorner || topRightCorner->isAncestorOf(widget));
            const bool isMenuBarSubtree = menuBar && (widget == menuBar || menuBar->isAncestorOf(widget));

            switch (event->type())
            {
                case QEvent::MouseButtonPress:
                {
                    auto mouseEvent = static_cast<QMouseEvent*>(event);
                    if (mouseEvent->button() == Qt::LeftButton)
                    {
                        const QPoint globalPos = mouseEvent->globalPosition().toPoint();
                        const QPoint windowPos = mapFromGlobal(globalPos);

                        ResizeRegion region = detectResizeRegion(windowPos);
                        if (region != ResizeRegion::None && !isButton && !isMenu && !isWindowControl)
                        {
                            if (beginSystemResizeForRegion(region))
                            {
                                systemResizeActive = true;
                                isResizing = false;
                                handled = true;
                                event->accept();
                            }
                            else
                            {
                                currentResizeRegion = region;
                                isResizing = true;
                                isDragging = false;
                                resizeStartPosition = globalPos;
                                resizeStartGeometry = geometry();
                                updateCursorForRegion(region);
                                handled = true;
                                event->accept();
                            }
                        }
                        else if (isMenuBarSubtree && menuBar && menuBar->isGlobalPointInDragHandle(globalPos))
                        {
                            beginWindowDragInternal(globalPos, false);
                            handled = true;
                            event->accept();
                        }
                    }
                    break;
                }
                case QEvent::MouseMove:
                {
                    auto mouseEvent = static_cast<QMouseEvent*>(event);
                    const QPoint globalPos = mouseEvent->globalPosition().toPoint();
                    if (systemResizeActive && (mouseEvent->buttons() & Qt::LeftButton))
                    {
                        handled = true;
                        event->accept();
                    }
                    else if (isResizing && (mouseEvent->buttons() & Qt::LeftButton))
                    {
                        performResize(globalPos);
                        updateCursorForRegion(currentResizeRegion);
                        handled = true;
                        event->accept();
                    }
                    else if ((isDragging || systemMoveActive) && (mouseEvent->buttons() & Qt::LeftButton))
                    {
                        updateWindowDragInternal(globalPos);
                        handled = true;
                        event->accept();
                    }
                    else if (!(mouseEvent->buttons() & Qt::LeftButton))
                    {
                        if (!isButton && !isMenu && !isWindowControl)
                        {
                            ResizeRegion region = detectResizeRegion(mapFromGlobal(globalPos));
                            currentResizeRegion = region;
                            if (!isResizing)
                                updateCursorForRegion(region);
                        }
                        else if (!isResizing && !isDragging)
                        {
                            currentResizeRegion = ResizeRegion::None;
                            updateCursorForRegion(ResizeRegion::None);
                        }
                    }
                    break;
                }
                case QEvent::MouseButtonRelease:
                {
                    auto mouseEvent = static_cast<QMouseEvent*>(event);
                    if (mouseEvent->button() == Qt::LeftButton)
                    {
                        if (systemResizeActive)
                        {
                            systemResizeActive = false;
                            ResizeRegion region = detectResizeRegion(mapFromGlobal(mouseEvent->globalPosition().toPoint()));
                            currentResizeRegion = region;
                            updateCursorForRegion(region);
                            handled = true;
                            event->accept();
                        }
                        else if (isResizing)
                        {
                            isResizing = false;
                            ResizeRegion region = detectResizeRegion(mapFromGlobal(mouseEvent->globalPosition().toPoint()));
                            currentResizeRegion = region;
                            updateCursorForRegion(region);
                            handled = true;
                            event->accept();
                        }
                        else if (isDragging || systemMoveActive || externalDragActive)
                        {
                            endWindowDragInternal();
                            ResizeRegion region = detectResizeRegion(mapFromGlobal(mouseEvent->globalPosition().toPoint()));
                            currentResizeRegion = region;
                            updateCursorForRegion(region);
                            handled = true;
                            event->accept();
                        }
                    }
                    break;
                }
                case QEvent::HoverMove:
                {
                    auto hoverEvent = static_cast<QHoverEvent*>(event);
                    const QPoint globalPos = widget->mapToGlobal(hoverEvent->position().toPoint());
                    if (!isButton && !isMenu && !isWindowControl)
                    {
                        ResizeRegion region = detectResizeRegion(mapFromGlobal(globalPos));
                        currentResizeRegion = region;
                        if (!isResizing)
                            updateCursorForRegion(region);
                    }
                    else if (!isResizing && !isDragging)
                    {
                        currentResizeRegion = ResizeRegion::None;
                        updateCursorForRegion(ResizeRegion::None);
                    }
                    break;
                }
                case QEvent::Leave:
                    if (widget == this && !isResizing && !isDragging)
                    {
                        currentResizeRegion = ResizeRegion::None;
                        updateCursorForRegion(ResizeRegion::None);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (handled)
        return true;

    if (event->type() == QEvent::MouseButtonDblClick)
    {
        auto mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton)
        {
            if (obj == leftHigh || obj == leftMid || obj == leftLow || obj == leftFilter ||
                obj == rightHigh || obj == rightMid || obj == rightLow || obj == rightFilter)
            {
                if (auto* dial = qobject_cast<QDial*>(obj))
                {
                    std::cout << "Double-click reset: " << dial->toolTip().toStdString() << " to 0 (neutral)" << std::endl;
                    dial->setValue(0);
                    return true;
                }
            }
            else if (obj == leftVolumeSlider || obj == rightVolumeSlider)
            {
                if (auto* slider = qobject_cast<QSlider*>(obj))
                {
                    std::cout << "Double-click reset: Volume slider to 100 (full volume)" << std::endl;
                    slider->setValue(100);
                    return true;
                }
            }
            else if (obj == crossfader)
            {
                if (auto* slider = qobject_cast<QSlider*>(obj))
                {
                    std::cout << "Double-click reset: Crossfader to 50 (center)" << std::endl;
                    slider->setValue(50);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void QtMainWindow::beginWindowDragInternal(const QPoint& globalPos, bool fromExternalSource)
{
    externalDragActive = fromExternalSource;
    systemMoveActive = false;

    if (QWindow* window = windowHandle())
    {
        if (window->startSystemMove())
        {
            systemMoveActive = true;
            isDragging = false;
            return;
        }
    }

    isDragging = true;
    dragStartPosition = globalPos - frameGeometry().topLeft();
}

void QtMainWindow::updateWindowDragInternal(const QPoint& globalPos)
{
    if (systemMoveActive)
        return;

    if (isDragging)
        move(globalPos - dragStartPosition);
}

void QtMainWindow::endWindowDragInternal()
{
    if (!systemMoveActive)
        isDragging = false;

    systemMoveActive = false;
    externalDragActive = false;
}

void QtMainWindow::beginExternalWindowDrag(const QPoint& globalPos)
{
    beginWindowDragInternal(globalPos, true);
}

void QtMainWindow::updateExternalWindowDrag(const QPoint& globalPos)
{
    updateWindowDragInternal(globalPos);
}

void QtMainWindow::endExternalWindowDrag()
{
    endWindowDragInternal();
}

void QtMainWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        const QPoint globalPos = event->globalPosition().toPoint();
        const QPoint localPos = event->pos();
        ResizeRegion region = detectResizeRegion(localPos);
        if (region != ResizeRegion::None)
        {
            if (beginSystemResizeForRegion(region))
            {
                systemResizeActive = true;
                isResizing = false;
                event->accept();
                return;
            }

            currentResizeRegion = region;
            isResizing = true;
            isDragging = false;
            resizeStartPosition = globalPos;
            resizeStartGeometry = geometry();
            updateCursorForRegion(region);
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void QtMainWindow::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint globalPos = event->globalPosition().toPoint();
    const QPoint localPos = event->pos();

    if (systemResizeActive && (event->buttons() & Qt::LeftButton))
    {
        event->accept();
        return;
    }

    if (isResizing && (event->buttons() & Qt::LeftButton))
    {
        performResize(globalPos);
        updateCursorForRegion(currentResizeRegion);
        event->accept();
        return;
    }

    if (!(event->buttons() & Qt::LeftButton))
    {
        ResizeRegion region = detectResizeRegion(localPos);
        currentResizeRegion = region;
        updateCursorForRegion(region);
    }

    if ((isDragging || systemMoveActive) && (event->buttons() & Qt::LeftButton))
    {
        updateWindowDragInternal(globalPos);
        event->accept();
        return;
    }

    QWidget::mouseMoveEvent(event);
}

void QtMainWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (systemResizeActive)
        {
            systemResizeActive = false;
            ResizeRegion region = detectResizeRegion(mapFromGlobal(event->globalPosition().toPoint()));
            currentResizeRegion = region;
            updateCursorForRegion(region);
            event->accept();
            return;
        }
        if (isResizing)
        {
            isResizing = false;
            ResizeRegion region = detectResizeRegion(mapFromGlobal(event->globalPosition().toPoint()));
            currentResizeRegion = region;
            updateCursorForRegion(region);
            event->accept();
            return;
        }
        if (isDragging || systemMoveActive || externalDragActive)
        {
            endWindowDragInternal();
            event->accept();
            return;
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void QtMainWindow::leaveEvent(QEvent* event)
{
    if (!isResizing)
    {
        currentResizeRegion = ResizeRegion::None;
        updateCursorForRegion(ResizeRegion::None);
    }
    QWidget::leaveEvent(event);
}

QtMainWindow::ResizeRegion QtMainWindow::detectResizeRegion(const QPoint& pos) const
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
        return ResizeRegion::None;

    if (pos.x() < 0 || pos.y() < 0 || pos.x() >= w || pos.y() >= h)
        return ResizeRegion::None;

    const int margin = std::max(1, std::min(resizeMargin, std::min(w, h) / 2));
    const bool onLeft = pos.x() <= margin;
    const bool onRight = pos.x() >= w - margin;
    const bool onTop = pos.y() <= margin;
    const bool onBottom = pos.y() >= h - margin;

    if (onTop && onLeft)
        return ResizeRegion::TopLeft;
    if (onTop && onRight)
        return ResizeRegion::TopRight;
    if (onBottom && onLeft)
        return ResizeRegion::BottomLeft;
    if (onBottom && onRight)
        return ResizeRegion::BottomRight;
    if (onLeft)
        return ResizeRegion::Left;
    if (onRight)
        return ResizeRegion::Right;
    if (onTop)
        return ResizeRegion::Top;
    if (onBottom)
        return ResizeRegion::Bottom;
    return ResizeRegion::None;
}

void QtMainWindow::updateCursorForRegion(ResizeRegion region)
{
    if (region == ResizeRegion::None && !isResizing)
    {
        if (cursorOverridden)
        {
            QApplication::restoreOverrideCursor();
            cursorOverridden = false;
            currentCursorShape = Qt::ArrowCursor;
        }
        return;
    }

    Qt::CursorShape desiredShape = Qt::ArrowCursor;
    switch (region)
    {
        case ResizeRegion::TopLeft:
        case ResizeRegion::BottomRight:
            desiredShape = Qt::SizeFDiagCursor;
            break;
        case ResizeRegion::TopRight:
        case ResizeRegion::BottomLeft:
            desiredShape = Qt::SizeBDiagCursor;
            break;
        case ResizeRegion::Left:
        case ResizeRegion::Right:
            desiredShape = Qt::SizeHorCursor;
            break;
        case ResizeRegion::Top:
        case ResizeRegion::Bottom:
            desiredShape = Qt::SizeVerCursor;
            break;
        case ResizeRegion::None:
            desiredShape = Qt::ArrowCursor;
            break;
    }

    if (cursorOverridden)
    {
        if (currentCursorShape != desiredShape)
        {
            QApplication::restoreOverrideCursor();
            cursorOverridden = false;
        }
        else
        {
            return;
        }
    }

    QApplication::setOverrideCursor(QCursor(desiredShape));
    cursorOverridden = true;
    currentCursorShape = desiredShape;
}

Qt::Edges QtMainWindow::edgesForRegion(ResizeRegion region) const
{
    Qt::Edges edges;
    switch (region)
    {
        case ResizeRegion::TopLeft:
            edges |= Qt::TopEdge;
            edges |= Qt::LeftEdge;
            break;
        case ResizeRegion::TopRight:
            edges |= Qt::TopEdge;
            edges |= Qt::RightEdge;
            break;
        case ResizeRegion::BottomLeft:
            edges |= Qt::BottomEdge;
            edges |= Qt::LeftEdge;
            break;
        case ResizeRegion::BottomRight:
            edges |= Qt::BottomEdge;
            edges |= Qt::RightEdge;
            break;
        case ResizeRegion::Top:
            edges |= Qt::TopEdge;
            break;
        case ResizeRegion::Bottom:
            edges |= Qt::BottomEdge;
            break;
        case ResizeRegion::Left:
            edges |= Qt::LeftEdge;
            break;
        case ResizeRegion::Right:
            edges |= Qt::RightEdge;
            break;
        case ResizeRegion::None:
            break;
    }
    return edges;
}

bool QtMainWindow::beginSystemResizeForRegion(ResizeRegion region)
{
    if (region == ResizeRegion::None)
        return false;

    if (QWindow* window = windowHandle())
    {
        const Qt::Edges edges = edgesForRegion(region);
        if (edges == Qt::Edges())
            return false;

        if (window->startSystemResize(edges))
            return true;
    }

    return false;
}

void QtMainWindow::performResize(const QPoint& globalPos)
{
    if (systemResizeActive || !isResizing || currentResizeRegion == ResizeRegion::None)
        return;

    QPoint delta = globalPos - resizeStartPosition;
    const int minW = minimumWidth();
    const int minH = minimumHeight();
    int maxW = maximumWidth();
    int maxH = maximumHeight();

    if (maxW <= 0 || maxW >= std::numeric_limits<int>::max())
        maxW = std::numeric_limits<int>::max();
    if (maxH <= 0 || maxH >= std::numeric_limits<int>::max())
        maxH = std::numeric_limits<int>::max();

    int newX = resizeStartGeometry.x();
    int newY = resizeStartGeometry.y();
    int newW = resizeStartGeometry.width();
    int newH = resizeStartGeometry.height();

    auto clampWidth = [&](int width) {
        width = std::max(width, minW);
        if (maxW != std::numeric_limits<int>::max())
            width = std::min(width, maxW);
        return width;
    };

    auto clampHeight = [&](int height) {
        height = std::max(height, minH);
        if (maxH != std::numeric_limits<int>::max())
            height = std::min(height, maxH);
        return height;
    };

    if (currentResizeRegion == ResizeRegion::Left ||
        currentResizeRegion == ResizeRegion::TopLeft ||
        currentResizeRegion == ResizeRegion::BottomLeft)
    {
        int targetWidth = clampWidth(resizeStartGeometry.width() - delta.x());
        newX = resizeStartGeometry.right() - targetWidth + 1;
        newW = targetWidth;
    }

    if (currentResizeRegion == ResizeRegion::Right ||
        currentResizeRegion == ResizeRegion::TopRight ||
        currentResizeRegion == ResizeRegion::BottomRight)
    {
        int targetWidth = clampWidth(resizeStartGeometry.width() + delta.x());
        newW = targetWidth;
    }

    if (currentResizeRegion == ResizeRegion::Top ||
        currentResizeRegion == ResizeRegion::TopLeft ||
        currentResizeRegion == ResizeRegion::TopRight)
    {
        int targetHeight = clampHeight(resizeStartGeometry.height() - delta.y());
        newY = resizeStartGeometry.bottom() - targetHeight + 1;
        newH = targetHeight;
    }

    if (currentResizeRegion == ResizeRegion::Bottom ||
        currentResizeRegion == ResizeRegion::BottomLeft ||
        currentResizeRegion == ResizeRegion::BottomRight)
    {
        int targetHeight = clampHeight(resizeStartGeometry.height() + delta.y());
        newH = targetHeight;
    }

    setGeometry(newX, newY, newW, newH);
}
