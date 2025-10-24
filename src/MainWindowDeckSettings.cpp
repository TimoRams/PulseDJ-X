#include "MainWindow.h"

#include "DeckSettings.h"
#include "DJAudioPlayer.h"

#include <QDebug>
#include <QPushButton>
#include <QSlider>
#include <QDial>

#include <algorithm>
#include <iostream>

void QtMainWindow::applyDeckSettings()
{
    std::cout << "*** applyDeckSettings() CALLED ***" << std::endl;
    if (!deckA || !deckB)
    {
        std::cout << "*** ERROR: Cannot apply deck settings - deck widgets not created yet ***" << std::endl;
        qWarning() << "Cannot apply deck settings - deck widgets not created yet";
        return;
    }

    qDebug() << "BetaPulseX: Applying deck settings to UI controls";

    const auto& configA = DeckSettings::instance().getDeckA();

    if (deckA->getKeylockButton())
    {
        deckA->getKeylockButton()->setChecked(configA.keylockEnabled);
        if (playerA)
            playerA->setKeylockEnabled(configA.keylockEnabled);
    }

    if (deckA->getQuantizeButton())
    {
        deckA->getQuantizeButton()->setChecked(configA.quantizeEnabled);
        if (playerA)
            playerA->setQuantizeEnabled(configA.quantizeEnabled);
    }

    if (deckA->getSpeedSlider() && deckA->getSpeedSlider()->isEnabled())
    {
        int speedValue = static_cast<int>(configA.speedFactor * 1000.0);
        speedValue = std::clamp(speedValue, 840, 1160);
        deckA->getSpeedSlider()->setValue(speedValue);
    }

    if (leftHigh && leftMid && leftLow)
    {
        leftHigh->setValue(0);
        leftMid->setValue(0);
        leftLow->setValue(0);
    }

    if (leftFilter)
        leftFilter->setValue(0);

    if (leftVolumeSlider)
        leftVolumeSlider->setValue(100);

    const auto& configB = DeckSettings::instance().getDeckB();

    if (deckB->getKeylockButton())
    {
        deckB->getKeylockButton()->setChecked(configB.keylockEnabled);
        if (playerB)
            playerB->setKeylockEnabled(configB.keylockEnabled);
    }

    if (deckB->getQuantizeButton())
    {
        deckB->getQuantizeButton()->setChecked(configB.quantizeEnabled);
        if (playerB)
            playerB->setQuantizeEnabled(configB.quantizeEnabled);
    }

    if (deckB->getSpeedSlider() && deckB->getSpeedSlider()->isEnabled())
    {
        int speedValue = static_cast<int>(configB.speedFactor * 1000.0);
        speedValue = std::clamp(speedValue, 840, 1160);
        deckB->getSpeedSlider()->setValue(speedValue);
    }

    if (rightHigh && rightMid && rightLow)
    {
        rightHigh->setValue(0);
        rightMid->setValue(0);
        rightLow->setValue(0);
    }

    if (rightFilter)
        rightFilter->setValue(0);

    if (rightVolumeSlider)
        rightVolumeSlider->setValue(100);

    qDebug() << "BetaPulseX: Deck settings applied successfully";
    qDebug() << "  Deck A: Keylock=" << configA.keylockEnabled
             << "Quantize=" << configA.quantizeEnabled
             << "Speed=" << configA.speedFactor;
    qDebug() << "  Deck B: Keylock=" << configB.keylockEnabled
             << "Quantize=" << configB.quantizeEnabled
             << "Speed=" << configB.speedFactor;

    connectDeckSettings();
}

void QtMainWindow::connectDeckSettings()
{
    std::cout << "*** connectDeckSettings() STARTED ***" << std::endl;
    qDebug() << "BetaPulseX: Connecting deck controls to settings system";

    QWidget* centralWidget = this;
    std::cout << "*** Using QtMainWindow as search widget ***" << std::endl;

    QDial* leftHighDial = centralWidget->findChild<QDial*>("leftHigh");
    QDial* leftMidDial = centralWidget->findChild<QDial*>("leftMid");
    QDial* leftLowDial = centralWidget->findChild<QDial*>("leftLow");
    QDial* leftFilterDial = centralWidget->findChild<QDial*>("leftFilter");

    std::cout << "*** EQ Dials Deck A: leftHigh=" << (leftHighDial ? "FOUND" : "NOT FOUND")
              << ", leftMid=" << (leftMidDial ? "FOUND" : "NOT FOUND")
              << ", leftLow=" << (leftLowDial ? "FOUND" : "NOT FOUND")
              << ", leftFilter=" << (leftFilterDial ? "FOUND" : "NOT FOUND") << std::endl;

    QDial* rightHighDial = centralWidget->findChild<QDial*>("rightHigh");
    QDial* rightMidDial = centralWidget->findChild<QDial*>("rightMid");
    QDial* rightLowDial = centralWidget->findChild<QDial*>("rightLow");
    QDial* rightFilterDial = centralWidget->findChild<QDial*>("rightFilter");

    std::cout << "*** EQ Dials Deck B: rightHigh=" << (rightHighDial ? "FOUND" : "NOT FOUND")
              << ", rightMid=" << (rightMidDial ? "FOUND" : "NOT FOUND")
              << ", rightLow=" << (rightLowDial ? "FOUND" : "NOT FOUND")
              << ", rightFilter=" << (rightFilterDial ? "FOUND" : "NOT FOUND") << std::endl;

    if (deckA)
    {
        if (deckA->getKeylockButton())
        {
            connect(deckA->getKeylockButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setKeylock(0, checked);
                qDebug() << "Deck A Keylock saved:" << checked;
            });
        }

        if (deckA->getQuantizeButton())
        {
            connect(deckA->getQuantizeButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setQuantize(0, checked);
                qDebug() << "Deck A Quantize saved:" << checked;
            });
        }

        if (deckA->getSpeedSlider())
        {
            connect(deckA->getSpeedSlider(), &QSlider::valueChanged, [this](int value) {
                double factor = value / 1000.0;
                DeckSettings::instance().setSpeedFactor(0, factor);
            });
        }
    }

    if (leftVolumeSlider)
    {
        connect(leftVolumeSlider, &QSlider::valueChanged, [this](int value) {
            double gain = value / 100.0;
            DeckSettings::instance().setGain(0, gain);
        });
    }

    if (deckB)
    {
        if (deckB->getKeylockButton())
        {
            connect(deckB->getKeylockButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setKeylock(1, checked);
                qDebug() << "Deck B Keylock saved:" << checked;
            });
        }

        if (deckB->getQuantizeButton())
        {
            connect(deckB->getQuantizeButton(), &QPushButton::toggled, [this](bool checked) {
                DeckSettings::instance().setQuantize(1, checked);
                qDebug() << "Deck B Quantize saved:" << checked;
            });
        }

        if (deckB->getSpeedSlider())
        {
            connect(deckB->getSpeedSlider(), &QSlider::valueChanged, [this](int value) {
                double factor = value / 1000.0;
                DeckSettings::instance().setSpeedFactor(1, factor);
            });
        }
    }

    if (rightVolumeSlider)
    {
        connect(rightVolumeSlider, &QSlider::valueChanged, [this](int value) {
            double gain = value / 100.0;
            DeckSettings::instance().setGain(1, gain);
        });
    }

    qDebug() << "BetaPulseX: Deck settings connections established";
}
