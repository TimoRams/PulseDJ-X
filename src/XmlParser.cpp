#include <vector>
#include <string>
#include "XmlParser.h"
#include "LibraryAudioItem.h"

void XmlParser::loadXmlFile(const File &xmlFile, std::vector<LibraryAudioItem> &container) {
    if (!xmlFile.exists())
        throw std::runtime_error{"XmlParser::loadXmlFile - xml file doesn't exist"};

    XmlDocument xmlDoc(xmlFile);
    std::unique_ptr<XmlElement> mainElem = xmlDoc.getDocumentElement();

    if (mainElem == nullptr)
        throw std::runtime_error{"XmlParser::loadXmlFile - xmlElement is null"};

    // if the main tag in the xml file is incorrect
    if (mainElem->getTagName().toStdString() != XmlParser::LIBRARY_AUDIO_ITEMS_TAG.toString())
        throw std::runtime_error{"XmlParser::loadXmlFile - the main xml tag is incorrect"};

    // string of filenames that have incorrect file paths
    std::string lostFilesNames;

    for (auto *elem: mainElem->getChildIterator()) {
        if (elem->hasTagName(XmlParser::LIBRARY_AUDIO_ITEM_TAG)) {
            std::string fileName = elem->getStringAttribute(XmlParser::FILENAME_TAG).toStdString();
            String fullFilePath = elem->getStringAttribute(XmlParser::FULL_FILEPATH_TAG);
            File file = File(fullFilePath);
            if (!file.existsAsFile()) {
                lostFilesNames += ("\n" + fileName);
                continue;
            }
            double duration = elem->getDoubleAttribute(XmlParser::DURATION_TAG);
            std::string extension = elem->getStringAttribute(XmlParser::EXTENSION_TAG).toStdString();

            auto item = std::make_unique<LibraryAudioItem>(
                    LibraryAudioItem{file, fileName, duration, extension});

            // add to LibraryAudioItems vector
            container.emplace_back(*item);
        }
    }

    if (!lostFilesNames.empty()) {
        NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                              "File(s) no longer exist",
                                              "The below file(s):\n" + lostFilesNames +
                                              "\n\nno longer exist. Maybe they were moved?");
    }
}

void XmlParser::saveXmlFile(const File &xmlFile, std::vector<LibraryAudioItem> &libraryItems) {
    // create file output stream
    FileOutputStream output(xmlFile);

    if (output.openedOk()) {
        // set position to start and overwrite the file
        output.setPosition(0);
        output.truncate();

        // create outer node
        XmlElement libAudioItems(XmlParser::LIBRARY_AUDIO_ITEMS_TAG);

        for (auto &item: libraryItems) {
            auto *libAudioItem = new XmlElement(XmlParser::LIBRARY_AUDIO_ITEM_TAG);
            libAudioItem->setAttribute(XmlParser::FULL_FILEPATH_TAG, item.getFile().getFullPathName());
            libAudioItem->setAttribute(XmlParser::FILENAME_TAG, item.getFileName());
            libAudioItem->setAttribute(XmlParser::DURATION_TAG, item.getDuration());
            libAudioItem->setAttribute(XmlParser::EXTENSION_TAG, item.getExtension());
            libAudioItems.addChildElement(libAudioItem);
        }

        auto xmlString = libAudioItems.toString();
        output.writeText(xmlString, false, false, nullptr);
    } else {
        throw std::runtime_error{"output stream failed to open correctly"};
    }
}