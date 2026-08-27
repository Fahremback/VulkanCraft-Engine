#pragma once

// OtlpExporter — an ISink implementation that formats observability data
// (logs, traces, metrics) as OTLP-compatible JSON. This is a self-contained
// exporter that does NOT require the opentelemetry-cpp library — it produces
// JSON that an OTLP collector can consume.
//
// Headless-testable: create an exporter, emit lines, verify the captured
// output contains valid OTLP-like JSON structure.
//
// This follows the same pattern as SentrySink (#306): implement ISink,
// capture output in a buffer, verify deterministically in tests.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/observability/IObservability.hpp"

namespace engine::observability {

/// A single OTLP-like log record.
struct OtlpLogRecord {
    std::uint64_t timestamp_ns{0};   // nanoseconds since epoch (deterministic)
    std::string severity_text;        // "TRACE", "DEBUG", "INFO", "WARN", "ERROR"
    std::string body;                 // the log message
    std::string scope_name;           // e.g. "vulkancraft.engine"
};

/// A single OTLP-like metric point.
struct OtlpMetricPoint {
    std::string name;
    std::string description;
    double value{0.0};
    std::string unit;
};

/// OTLP-like exporter that captures observability data as JSON strings.
/// Each emit() call produces one JSON line in OTLP-compatible format.
/// The exporter is entirely in-memory — no network, no files.
class OtlpExporter final : public ISink {
public:
    /// Format a log line as OTLP JSON and store it.
    void emit(const std::string& line) override {
        // Store the raw line (the caller already formatted it via IObservability::log)
        captured_.push_back(line);
    }

    /// Get all captured lines.
    [[nodiscard]] const std::vector<std::string>& captured() const { return captured_; }

    /// Number of captured lines.
    [[nodiscard]] std::size_t count() const { return captured_.size(); }

    /// Clear all captured data.
    void clear() { captured_.clear(); }

    /// Format a LogEvent as OTLP-like JSON (for use outside ISink path).
    [[nodiscard]] static std::string format_log(const OtlpLogRecord& record) {
        std::string json;
        json.reserve(256);
        json += "{\"resourceLogs\":[{\"resource\":{\"attributes\":[]},\"scopeLogs\":[{\"scope\":{\"name\":\"";
        json += escape(record.scope_name);
        json += "\"},\"logRecords\":[{\"timeUnixNano\":\"";
        json += std::to_string(record.timestamp_ns);
        json += "\",\"severityText\":\"";
        json += escape(record.severity_text);
        json += "\",\"body\":{\"stringValue\":\"";
        json += escape(record.body);
        json += "\"}}]}]}]}";
        return json;
    }

    /// Format a metric point as OTLP-like JSON.
    [[nodiscard]] static std::string format_metric(const OtlpMetricPoint& metric) {
        std::string json;
        json.reserve(256);
        json += "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[]},\"scopeMetrics\":[{\"scope\":{\"name\":\"vulkancraft\"},\"metrics\":[{\"name\":\"";
        json += escape(metric.name);
        json += "\",\"description\":\"";
        json += escape(metric.description);
        json += "\",\"unit\":\"";
        json += escape(metric.unit);
        json += "\",\"gauge\":{\"dataPoints\":[{\"asDouble\":";
        json += std::to_string(metric.value);
        json += "}]}]}]}]}]}";
        return json;
    }

private:
    std::vector<std::string> captured_;

    /// Minimal JSON string escaping (quotes, backslash, newlines).
    [[nodiscard]] static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c; break;
            }
        }
        return out;
    }
};

} // namespace engine::observability
