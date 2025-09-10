#pragma once

#include <QListWidget>
#include <QDrag>
#include <QMimeData>
#include <QUrl>

class DraggableListWidget : public QListWidget {
    Q_OBJECT
public:
    explicit DraggableListWidget(QWidget* parent = nullptr) : QListWidget(parent) {}
protected:
    void startDrag(Qt::DropActions supportedActions) override {
        auto items = selectedItems();
        if (items.isEmpty()) return;
        QMimeData* mime = new QMimeData();
        QList<QUrl> urls;
        for (auto it : items) {
            QString path = it->data(Qt::UserRole).toString();
            if (path.isEmpty()) path = it->text();
            urls << QUrl::fromLocalFile(path);
        }
        mime->setUrls(urls);
        QDrag* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(supportedActions);
    }
};
