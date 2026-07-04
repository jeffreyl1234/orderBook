#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace ob {

// Portable high-resolution time source for the benchmark harness.
//
// Design note you can defend in an interview:
//   * `rdtsc` is the classic HFT choice, but it is x86-only, counts reference
//     cycles (needs the `constant_tsc`/`invariant TSC` CPU feature to be
//     meaningful), and must be *serialized* with a fence or it reorders around
//     the code you are timing. It is NOT available on this arm64 machine.
//   * On arm64 the analogue is the virtual counter `cntvct_el0`, but on Apple
//     silicon it ticks at only ~24 MHz (~41 ns granularity) — too coarse to
//     resolve sub-100 ns operations.
//   * `std::chrono::steady_clock` is backed by `mach_absolute_time` on macOS,
//     which is cheap (~20-30 ns/call) and nanosecond-resolution. That is our
//     portable default; the raw counters are exposed for x86 experiments.
inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#if defined(__x86_64__) || defined(_M_X64)
// Serialized read: lfence bounds the timestamp so it cannot float across the
// measured region in either direction.
inline std::uint64_t rdtsc_serialized() {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
#elif defined(__aarch64__)
inline std::uint64_t arm_cntvct() {
    std::uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
inline std::uint64_t arm_cntfrq() {
    std::uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
#endif

} // namespace ob
