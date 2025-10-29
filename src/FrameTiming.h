#pragma once

namespace FrameTiming {
inline constexpr int kTargetFrameRate = 60;
inline constexpr int kFrameIntervalMs = 1000 / kTargetFrameRate;
inline constexpr int kVSyncSwapInterval = 1;
}
