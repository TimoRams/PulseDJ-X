#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <QColor>
#include <QVector3D>

namespace WaveformTheme {

struct RgbColor {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
};

inline constexpr RgbColor fallbackColor() {
    return {0.43f, 0.74f, 1.0f};
}

inline constexpr RgbColor fallbackWarningColor() {
    return {0.8f, 0.1f, 0.1f};
}

inline RgbColor computeSpectrumColor(float lowSum, float midSum, float highSum, int sampleCount) {
    if (sampleCount <= 0) {
        return fallbackColor();
    }

    const float invCount = 1.0f / static_cast<float>(std::max(1, sampleCount));
    float lowAvg = std::max(0.0f, lowSum * invCount);
    float midAvg = std::max(0.0f, midSum * invCount);
    float highAvg = std::max(0.0f, highSum * invCount);

    const float total = std::max(1.0e-3f, lowAvg + midAvg + highAvg);
    lowAvg = std::clamp(lowAvg / total, 0.1f, 1.0f);
    midAvg = std::clamp(midAvg / total, 0.1f, 1.0f);
    highAvg = std::clamp(highAvg / total, 0.1f, 1.0f);

    auto gamma = [](float v) {
        return std::pow(v, 0.75f);
    };

    float r = gamma(lowAvg);
    float g = gamma(midAvg);
    float b = gamma(highAvg);

    constexpr float boost = 1.1f;
    r = std::min(1.0f, r * boost);
    g = std::min(1.0f, g * boost);
    b = std::min(1.0f, b * boost);
    return {r, g, b};
}

inline float computeColumnAmplitude(float minVal, float maxVal) {
    return std::max(std::abs(minVal), std::abs(maxVal));
}

inline const std::array<QColor, 8>& cueColors() {
    static const std::array<QColor, 8> kColors{
        QColor(255, 100, 100), QColor(100, 255, 100), QColor(100, 100, 255), QColor(255, 255, 100),
        QColor(255, 100, 255), QColor(100, 255, 255), QColor(255, 200, 100), QColor(200, 100, 255)
    };
    return kColors;
}

inline QColor loopBaseColor() {
    return QColor(100, 255, 100);
}

inline QColor loopBorderColor() {
    return QColor(0, 200, 0);
}

inline QColor ghostLoopBaseColor() {
    return QColor(100, 255, 100);
}

inline QColor ghostLoopBorderColor() {
    return QColor(0, 200, 0);
}

inline QVector3D overviewBaseColor() {
    const auto base = fallbackColor();
    return QVector3D(base.r * 0.55f, base.g * 0.65f, base.b * 0.95f);
}

inline QVector3D overviewHighlightColor() {
    const auto base = fallbackColor();
    return QVector3D(std::min(1.0f, base.r * 1.2f),
                     std::min(1.0f, base.g * 1.2f),
                     std::min(1.0f, base.b * 1.2f));
}

} // namespace WaveformTheme
