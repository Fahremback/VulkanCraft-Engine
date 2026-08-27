// OtlpExporterTests.cpp — headless gate for OTLP exporter.
// Tests: ISink capture, OTLP JSON formatting, log/metric structure,
// edge cases (empty strings, special chars), and determinism.
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "engine/observability/OtlpExporter.hpp"

using namespace engine::observability;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(expr, msg) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_fails; \
        std::cerr << "  FAIL: " << msg << " (" #expr ")\n"; \
    } \
} while(0)

// ---------------------------------------------------------------------------
// 1. ISink capture
// ---------------------------------------------------------------------------
static void test_sink_capture() {
    std::cerr << "[OTLP] sink capture\n";
    OtlpExporter exp;
    CHECK(exp.count() == 0, "empty initially");

    exp.emit("line1");
    exp.emit("line2");
    CHECK(exp.count() == 2, "captured 2");
    CHECK(exp.captured()[0] == "line1", "first line");
    CHECK(exp.captured()[1] == "line2", "second line");
    std::cerr << "[OTLP] sink capture OK\n";
}

// ---------------------------------------------------------------------------
// 2. Clear
// ---------------------------------------------------------------------------
static void test_clear() {
    std::cerr << "[OTLP] clear\n";
    OtlpExporter exp;
    exp.emit("data");
    exp.clear();
    CHECK(exp.count() == 0, "cleared");
    std::cerr << "[OTLP] clear OK\n";
}

// ---------------------------------------------------------------------------
// 3. Log formatting
// ---------------------------------------------------------------------------
static void test_log_format() {
    std::cerr << "[OTLP] log format\n";
    OtlpLogRecord rec;
    rec.timestamp_ns = 1234567890;
    rec.severity_text = "INFO";
    rec.body = "Hello, world!";
    rec.scope_name = "vulkancraft.engine";

    std::string json = OtlpExporter::format_log(rec);
    CHECK(json.find("\"timeUnixNano\":\"1234567890\"") != std::string::npos, "timestamp");
    CHECK(json.find("\"severityText\":\"INFO\"") != std::string::npos, "severity");
    CHECK(json.find("\"body\":{\"stringValue\":\"Hello, world!\"}") != std::string::npos, "body");
    CHECK(json.find("\"scope\":{\"name\":\"vulkancraft.engine\"}") != std::string::npos, "scope");
    CHECK(json.find("resourceLogs") != std::string::npos, "OTLP structure");
    std::cerr << "[OTLP] log format OK\n";
}

// ---------------------------------------------------------------------------
// 4. Metric formatting
// ---------------------------------------------------------------------------
static void test_metric_format() {
    std::cerr << "[OTLP] metric format\n";
    OtlpMetricPoint metric;
    metric.name = "engine.fps";
    metric.description = "Frames per second";
    metric.value = 60.0;
    metric.unit = "Hz";

    std::string json = OtlpExporter::format_metric(metric);
    CHECK(json.find("\"name\":\"engine.fps\"") != std::string::npos, "name");
    CHECK(json.find("\"description\":\"Frames per second\"") != std::string::npos, "description");
    CHECK(json.find("\"asDouble\":60") != std::string::npos, "value");
    CHECK(json.find("\"unit\":\"Hz\"") != std::string::npos, "unit");
    CHECK(json.find("resourceMetrics") != std::string::npos, "OTLP structure");
    std::cerr << "[OTLP] metric format OK\n";
}

// ---------------------------------------------------------------------------
// 5. JSON escaping
// ---------------------------------------------------------------------------
static void test_json_escaping() {
    std::cerr << "[OTLP] json escaping\n";
    OtlpLogRecord rec;
    rec.timestamp_ns = 1;
    rec.severity_text = "INFO";
    rec.body = "Line with \"quotes\" and \\backslash and\nnewline";
    rec.scope_name = "test";

    std::string json = OtlpExporter::format_log(rec);
    CHECK(json.find("\\\"quotes\\\"") != std::string::npos, "escaped quotes");
    CHECK(json.find("\\\\backslash") != std::string::npos, "escaped backslash");
    CHECK(json.find("\\n") != std::string::npos, "escaped newline");
    // The JSON should be valid (no unescaped special chars breaking structure)
    CHECK(json.find("\"body\":{\"stringValue\":\"") != std::string::npos, "body starts");
    std::cerr << "[OTLP] json escaping OK\n";
}

// ---------------------------------------------------------------------------
// 6. Empty strings
// ---------------------------------------------------------------------------
static void test_empty_strings() {
    std::cerr << "[OTLP] empty strings\n";
    OtlpLogRecord rec;
    rec.timestamp_ns = 0;
    rec.severity_text = "";
    rec.body = "";
    rec.scope_name = "";

    std::string json = OtlpExporter::format_log(rec);
    CHECK(json.find("\"severityText\":\"\"") != std::string::npos, "empty severity");
    CHECK(json.find("\"body\":{\"stringValue\":\"\"}") != std::string::npos, "empty body");
    CHECK(json.find("resourceLogs") != std::string::npos, "still valid structure");
    std::cerr << "[OTLP] empty strings OK\n";
}

// ---------------------------------------------------------------------------
// 7. Determinism (same input → same output)
// ---------------------------------------------------------------------------
static void test_determinism() {
    std::cerr << "[OTLP] determinism\n";
    OtlpLogRecord rec;
    rec.timestamp_ns = 999;
    rec.severity_text = "ERROR";
    rec.body = "deterministic test";
    rec.scope_name = "test.scope";

    std::string a = OtlpExporter::format_log(rec);
    std::string b = OtlpExporter::format_log(rec);
    CHECK(a == b, "same input → same output");

    OtlpMetricPoint m;
    m.name = "test.metric";
    m.description = "desc";
    m.value = 42.0;
    m.unit = "count";

    std::string ma = OtlpExporter::format_metric(m);
    std::string mb = OtlpExporter::format_metric(m);
    CHECK(ma == mb, "metric determinism");
    std::cerr << "[OTLP] determinism OK\n";
}

// ---------------------------------------------------------------------------
// 8. ISink integration with IObservability
// ---------------------------------------------------------------------------
static void test_observability_integration() {
    std::cerr << "[OTLP] observability integration\n";
    // Create observability + exporter + register
    std::string initErr;
    auto obs = create_observability("otlp-test", 100, initErr);
    OtlpExporter exp;

    std::string regErr;
    CHECK(obs->register_sink("otlp", &exp, regErr), "register sink");
    obs->set_enabled(true);

    // Log some messages
    std::string logErr;
    CHECK(obs->log(LogLevel::Info, "test", "hello from otlp", logErr), "log info");
    CHECK(obs->log(LogLevel::Error, "test", "error message", logErr), "log error");

    // The exporter should have captured the formatted lines
    CHECK(exp.count() >= 2, "exporter captured logs");

    // Unregister
    obs->remove_sink("otlp");
    std::cerr << "[OTLP] observability integration OK\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    test_sink_capture();
    test_clear();
    test_log_format();
    test_metric_format();
    test_json_escaping();
    test_empty_strings();
    test_determinism();
    test_observability_integration();

    std::cerr << "\n[otlp_exporter] " << g_checks << " checks, "
              << g_fails << " failures\n";
    return g_fails == 0 ? 0 : 1;
}
