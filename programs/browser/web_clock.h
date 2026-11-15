#pragma once
#include <cstdint>
#include <time.h>

namespace SOS2 {
// Microseconds on the monotonic clock. Frame, refinement and GPU probe times
// all use it, so their values compare across files.
inline uint64_t microseconds() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return uint64_t(value.tv_sec) * 1000000 + value.tv_nsec / 1000;
}
}
