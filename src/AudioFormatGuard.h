#pragma once

#include <mutex>

class AudioFormatManagerGuard
{
public:
    AudioFormatManagerGuard();
    ~AudioFormatManagerGuard();

private:
    std::recursive_mutex& mutex;
    std::unique_lock<std::recursive_mutex> lock;
};
