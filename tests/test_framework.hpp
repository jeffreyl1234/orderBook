#pragma once

// A ~100-line self-contained test framework. Deliberately zero third-party
// dependencies: the build stays hermetic and offline, and swapping in Catch2 or
// doctest later is a mechanical change. Provides TEST_CASE registration plus
// CHECK (non-fatal) and REQUIRE (fatal) assertions with source locations.

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace tf {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Counters {
    int checks = 0;
    int failed = 0;
};
inline Counters& counters() {
    static Counters c;
    return c;
}

struct Registrar {
    Registrar(const char* n, std::function<void()> f) {
        registry().push_back({n, std::move(f)});
    }
};

// Thrown by REQUIRE to abort the current test case only.
struct RequireFailure : std::exception {};

inline void report_fail(const char* file, int line, const std::string& expr) {
    ++counters().failed;
    std::printf("    FAIL %s:%d  %s\n", file, line, expr.c_str());
}

} // namespace tf

#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)

#define TEST_CASE(name)                                                       \
    static void TF_CAT(tf_test_, __LINE__)();                                 \
    static ::tf::Registrar TF_CAT(tf_reg_, __LINE__)(                         \
        name, TF_CAT(tf_test_, __LINE__));                                    \
    static void TF_CAT(tf_test_, __LINE__)()

#define CHECK(expr)                                                           \
    do {                                                                      \
        ++::tf::counters().checks;                                            \
        if (!(expr)) ::tf::report_fail(__FILE__, __LINE__, #expr);            \
    } while (0)

#define REQUIRE(expr)                                                         \
    do {                                                                      \
        ++::tf::counters().checks;                                            \
        if (!(expr)) {                                                        \
            ::tf::report_fail(__FILE__, __LINE__, #expr);                     \
            throw ::tf::RequireFailure{};                                     \
        }                                                                     \
    } while (0)

// Equality check that prints both sides on failure.
#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++::tf::counters().checks;                                            \
        auto tf_va = (a);                                                     \
        auto tf_vb = (b);                                                     \
        if (!(tf_va == tf_vb)) {                                              \
            ::tf::report_fail(__FILE__, __LINE__,                             \
                              std::string(#a " == " #b) + "  (" +            \
                                  std::to_string(tf_va) + " vs " +           \
                                  std::to_string(tf_vb) + ")");              \
        }                                                                     \
    } while (0)
