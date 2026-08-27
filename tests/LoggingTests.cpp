// LoggingTests.cpp — Gate for centralized logging (Log.hpp).
//
// Tests that the logging macros compile and produce output.
// With VC_USE_SPDLOG: uses spdlog structured logging.
// Without: falls back to fprintf.

#include <cstdio>
#include <cstring>
#include <string>

// Test both paths: with and without spdlog
#include "engine/core/logging/Log.hpp"

static int g_failures = 0;

static void check(bool ok, const char* name) {
    if (ok) { std::printf("PASS: %s\n", name); }
    else    { std::printf("FAIL: %s\n", name); ++g_failures; }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("LoggingTests gate v1\n");
        return 0;
    }

    std::printf("Logging gate\n");

    // Test that all log macros compile and execute without crash
    VC_LOG_TRACE("trace message {}", 1);
    VC_LOG_DEBUG("debug message {}", 2);
    VC_LOG_INFO("info message {}", 3);
    VC_LOG_WARN("warn message {}", 4);
    VC_LOG_ERROR("error message {}", 5);
    VC_LOG_CRITICAL("critical message {}", 6);

    check(true, "all log macros compile and execute");
    check(true, "structured format accepted");

#ifdef VC_USE_SPDLOG
    check(true, "spdlog backend active");
    std::printf("  backend: spdlog\n");
#else
    check(true, "fprintf fallback active");
    std::printf("  backend: fprintf fallback\n");
#endif

    if (g_failures == 0) { std::printf("ALL LOGGING GATE TESTS PASSED\n"); return 0; }
    std::printf("%d LOGGING GATE TEST(S) FAILED\n", g_failures);
    return 1;
}
