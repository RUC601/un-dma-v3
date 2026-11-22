#pragma once

#include <Windows.h>

namespace V2::Output {
inline bool FerrumLabMotionEnabled() {
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("UN_DMA_LAB_FERRUM_MOTION", value, sizeof(value)) == 1 &&
            value[0] == '1';
    }();
    return enabled;
}

inline long long FerrumLabQpc() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}
}
