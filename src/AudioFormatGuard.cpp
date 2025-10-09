#include "AudioFormatGuard.h"

namespace
{
std::recursive_mutex& sharedAudioFormatMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}
}

AudioFormatManagerGuard::AudioFormatManagerGuard()
    : mutex(sharedAudioFormatMutex()), lock(mutex)
{
}

AudioFormatManagerGuard::~AudioFormatManagerGuard() = default;
