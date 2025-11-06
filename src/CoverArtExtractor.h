#pragma once

#include <QByteArray>
#include <QString>
#include <QImage>

class CoverArtExtractor
{
public:
    // Extract cover art from audio file and return as QByteArray + format
    static std::pair<QByteArray, QString> extractCoverArt(const QString& filePath);

private:
    static std::pair<QByteArray, QString> extractFromMP3(const QString& filePath);
    static std::pair<QByteArray, QString> extractFromFLAC(const QString& filePath);
    static std::pair<QByteArray, QString> extractFromMP4(const QString& filePath);
    static std::pair<QByteArray, QString> extractFromOGG(const QString& filePath);
    static std::pair<QByteArray, QString> extractFromWAV(const QString& filePath);
    
    static QString detectImageFormat(const QByteArray& data);
};
