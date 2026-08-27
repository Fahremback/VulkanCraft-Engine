// spdlog-probe.cpp - section 7 / BUG-018: prove spdlog is a REAL usable logger.
// Header-only: compiles against the vendored headers (fmt bundled). Creates a
// memory (ringbuffer) sink, logs at multiple levels, and VERIFIES the messages
// were actually captured + formatted — real consumption, not just include.
// Self-contained, deterministic, no network, exit 0 = evidence of use.
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/pattern_formatter.h>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    // 1. Memory sink to capture what the logger produces.
    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);

    // 2. A logger bound to that sink (pattern includes level + message).
    auto logger = std::make_shared<spdlog::logger>("probe", sink);
    logger->set_pattern("%l|%v");
    logger->set_level(spdlog::level::trace);

    // 3. Log across levels + a formatted/structured message.
    logger->info("hello mk {} {}", 1, "two");
    logger->warn("watch out: {:.2f}", 3.14159);
    logger->error("fail code={}", 42);

    // 4. VERIFY the sink actually captured the 3 lines, in order, formatted.
    const std::vector<std::string> lines = sink->last_formatted();
    if (lines.size() != 3) {
        std::fprintf(stderr, "FAIL: expected 3 logged lines, got %zu\n", lines.size());
        return 1;
    }
    if (lines[0].find("info|hello mk 1 two") == std::string::npos) {
        std::fprintf(stderr, "FAIL: line0 not info|formatted: '%s'\n", lines[0].c_str());
        return 1;
    }
    if (lines[1].find("warning|watch out: 3.14") == std::string::npos) {
        std::fprintf(stderr, "FAIL: line1 not warn|formatted: '%s'\n", lines[1].c_str());
        return 1;
    }
    if (lines[2].find("error|fail code=42") == std::string::npos) {
        std::fprintf(stderr, "FAIL: line2 not error|formatted: '%s'\n", lines[2].c_str());
        return 1;
    }

    // 5. flush + default logger also functional (falls to default which is a no-op here).
    logger->flush();
    spdlog::drop("probe");

    std::printf("spdlog-consumer-ok levels=3 captured=1 structured=1");
    return 0;
}
