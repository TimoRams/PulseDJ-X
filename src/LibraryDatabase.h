#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <optional>

class QSqlDatabase;
struct TrackInfo;

class LibraryDatabase : public QObject
{
    Q_OBJECT
public:
    explicit LibraryDatabase(QObject* parent = nullptr);
    ~LibraryDatabase() override;

    bool open(const QString& databasePath);
    QVector<TrackInfo> loadAllTracks() const;
    std::optional<TrackInfo> loadTrackByPath(const QString& filePath) const;
    bool upsertTrack(const TrackInfo& track);
    bool removeTrack(const QString& filePath);
    bool removeAllTracks();
    bool vacuum();

    QString databasePath() const { return dbPath; }

private:
    bool ensureSchema();
    bool createSchema();

    QString connectionName;
    QString dbPath;
    mutable bool schemaReady = false;
};
