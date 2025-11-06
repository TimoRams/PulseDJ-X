#include "CoverArtExtractor.h"

#ifdef TAGLIB_FOUND
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4coverart.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/wavfile.h>
#include <iostream>
#endif

std::pair<QByteArray, QString> CoverArtExtractor::extractCoverArt(const QString& filePath)
{
#ifdef TAGLIB_FOUND
    QString ext = filePath.section('.', -1).toLower();
    
    if (ext == "mp3")
        return extractFromMP3(filePath);
    else if (ext == "flac")
        return extractFromFLAC(filePath);
    else if (ext == "m4a" || ext == "mp4" || ext == "aac")
        return extractFromMP4(filePath);
    else if (ext == "ogg")
        return extractFromOGG(filePath);
    else if (ext == "wav")
        return extractFromWAV(filePath);
#endif
    
    return {};
}

QString CoverArtExtractor::detectImageFormat(const QByteArray& data)
{
    if (data.size() < 4)
        return QString();
    
    // JPEG magic bytes
    if ((unsigned char)data[0] == 0xFF && 
        (unsigned char)data[1] == 0xD8 && 
        (unsigned char)data[2] == 0xFF)
        return "JPEG";
    
    // PNG magic bytes
    if ((unsigned char)data[0] == 0x89 && 
        (unsigned char)data[1] == 0x50 && 
        (unsigned char)data[2] == 0x4E && 
        (unsigned char)data[3] == 0x47)
        return "PNG";
    
    return QString();
}

#ifdef TAGLIB_FOUND

std::pair<QByteArray, QString> CoverArtExtractor::extractFromMP3(const QString& filePath)
{
    TagLib::MPEG::File file(filePath.toUtf8().constData());
    if (!file.isValid())
        return {};
    
    auto* tag = file.ID3v2Tag();
    if (!tag)
        return {};
    
    auto frameList = tag->frameListMap()["APIC"];
    if (frameList.isEmpty())
        return {};
    
    auto* frame = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frameList.front());
    if (!frame)
        return {};
    
    TagLib::ByteVector picture = frame->picture();
    QByteArray data(picture.data(), picture.size());
    QString format = detectImageFormat(data);
    
    if (!format.isEmpty())
    {
        std::cout << "CoverArtExtractor: Extracted " << data.size() 
                  << " bytes from MP3 (" << format.toStdString() << ")" << std::endl;
        return {data, format};
    }
    
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromFLAC(const QString& filePath)
{
    TagLib::FLAC::File file(filePath.toUtf8().constData());
    if (!file.isValid())
        return {};
    
    auto pictures = file.pictureList();
    if (pictures.isEmpty())
        return {};
    
    auto* picture = pictures.front();
    if (!picture)
        return {};
    
    TagLib::ByteVector pictureData = picture->data();
    QByteArray data(pictureData.data(), pictureData.size());
    QString format = detectImageFormat(data);
    
    if (!format.isEmpty())
    {
        std::cout << "CoverArtExtractor: Extracted " << data.size() 
                  << " bytes from FLAC (" << format.toStdString() << ")" << std::endl;
        return {data, format};
    }
    
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromMP4(const QString& filePath)
{
    TagLib::MP4::File file(filePath.toUtf8().constData());
    if (!file.isValid())
        return {};
    
    auto* tag = file.tag();
    if (!tag)
        return {};
    
    auto items = tag->itemMap();
    if (!items.contains("covr"))
        return {};
    
    auto coverList = items["covr"].toCoverArtList();
    if (coverList.isEmpty())
        return {};
    
    TagLib::ByteVector pictureData = coverList.front().data();
    QByteArray data(pictureData.data(), pictureData.size());
    QString format = detectImageFormat(data);
    
    if (!format.isEmpty())
    {
        std::cout << "CoverArtExtractor: Extracted " << data.size() 
                  << " bytes from MP4 (" << format.toStdString() << ")" << std::endl;
        return {data, format};
    }
    
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromOGG(const QString& filePath)
{
    TagLib::Ogg::Vorbis::File file(filePath.toUtf8().constData());
    if (!file.isValid())
        return {};
    
    auto* tag = dynamic_cast<TagLib::Ogg::XiphComment*>(file.tag());
    if (!tag)
        return {};
    
    auto fieldMap = tag->fieldListMap();
    if (!fieldMap.contains("METADATA_BLOCK_PICTURE"))
        return {};
    
    auto field = fieldMap["METADATA_BLOCK_PICTURE"];
    if (field.isEmpty())
        return {};
    
    TagLib::ByteVector base64 = field.front().data(TagLib::String::UTF8);
    TagLib::ByteVector pictureData = TagLib::ByteVector::fromBase64(base64);
    
    // Skip FLAC picture block header (32 bytes)
    if (pictureData.size() > 32)
    {
        QByteArray data(pictureData.data() + 32, pictureData.size() - 32);
        QString format = detectImageFormat(data);
        
        if (!format.isEmpty())
        {
            std::cout << "CoverArtExtractor: Extracted " << data.size() 
                      << " bytes from OGG (" << format.toStdString() << ")" << std::endl;
            return {data, format};
        }
    }
    
    return {};
}

std::pair<QByteArray, QString> CoverArtExtractor::extractFromWAV(const QString& filePath)
{
    TagLib::RIFF::WAV::File file(filePath.toUtf8().constData());
    if (!file.isValid())
        return {};
    
    auto* tag = file.ID3v2Tag();
    if (!tag)
        return {};
    
    auto frameList = tag->frameListMap()["APIC"];
    if (frameList.isEmpty())
        return {};
    
    auto* frame = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frameList.front());
    if (!frame)
        return {};
    
    TagLib::ByteVector picture = frame->picture();
    QByteArray data(picture.data(), picture.size());
    QString format = detectImageFormat(data);
    
    if (!format.isEmpty())
    {
        std::cout << "CoverArtExtractor: Extracted " << data.size() 
                  << " bytes from WAV (" << format.toStdString() << ")" << std::endl;
        return {data, format};
    }
    
    return {};
}

#endif // TAGLIB_FOUND
