#include "test_framework.hpp"

#include <cstdio>

// Single runner linked with all test translation units. Each TEST_CASE
// self-registers at static-init time, so main just iterates the registry.
int main() {
    int failed_cases = 0;
    for (const auto& tc : tf::registry()) {
        const int before = tf::counters().failed;
        std::printf("[ RUN  ] %s\n", tc.name);
        try {
            tc.fn();
        } catch (const tf::RequireFailure&) {
            // Assertion already reported; move on to the next case.
        } catch (const std::exception& e) {
            ::tf::report_fail("<exception>", 0, e.what());
        }
        if (tf::counters().failed > before) {
            ++failed_cases;
            std::printf("[ FAIL ] %s\n", tc.name);
        } else {
            std::printf("[  OK  ] %s\n", tc.name);
        }
    }

    std::printf("\n%d checks run, %d failed, %d/%zu cases failed\n",
                tf::counters().checks, tf::counters().failed, failed_cases,
                tf::registry().size());
    return tf::counters().failed == 0 ? 0 : 1;
}
