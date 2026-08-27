// otel-probe.cpp — prove the vendored opentelemetry-cpp is usable on Windows/MSVC
// by running the full OTLP **file exporter** pipeline (no network):
//   - tracer provider -> span -> JSON file contains the span
//   - logger provider -> log record -> JSON file contains the log body
//   - meter provider -> counter -> JSON file contains the metric name
// All three write deterministic OTLP JSON to disk; the probe greps the files.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "opentelemetry/exporters/otlp/otlp_file_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_file_log_record_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_factory.h"
#include "opentelemetry/logs/provider.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/logs/logger_provider.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/logs/provider.h"
#include "opentelemetry/sdk/logs/simple_log_record_processor_factory.h"
#include "opentelemetry/sdk/metrics/meter_context_factory.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/provider.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/trace/provider.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"

namespace otlp = opentelemetry::exporter::otlp;
namespace sdktrace = opentelemetry::sdk::trace;
namespace sdklogs = opentelemetry::sdk::logs;
namespace sdkmetrics = opentelemetry::sdk::metrics;
namespace trace_api = opentelemetry::trace;
namespace logs_api = opentelemetry::logs;
namespace metrics_api = opentelemetry::metrics;

static int g_checks = 0;
static int g_fails = 0;

static void check(bool ok, const char* what)
{
  g_checks++;
  if (!ok) {
    g_fails++;
    std::printf("  FAIL: %s\n", what);
  } else {
    std::printf("  ok:   %s\n", what);
  }
}

// file read helper
static std::string slurp(const char* path)
{
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static bool contains(const std::string& hay, const std::string& needle)
{
  return hay.find(needle) != std::string::npos;
}

int main()
{
  // Keep all providers alive until verification is complete.
  // The SDK provider factories return unique_ptr<SDK> which implicitly
  // converts to shared_ptr<API> — the batch example pattern.
  std::shared_ptr<trace_api::TracerProvider> trace_sp;
  std::shared_ptr<logs_api::LoggerProvider> log_sp;
  std::shared_ptr<metrics_api::MeterProvider> metric_sp;

  // ---- TRACE: span through OTLP file exporter ----
  std::printf("[trace] creating provider with OTLP file exporter\n");
  {
    otlp::OtlpFileExporterOptions opts;
    otlp::OtlpFileClientFileSystemOptions fs;
    fs.file_pattern = "otel-probe-traces.json";   // fixed name: single file
    fs.flush_interval = std::chrono::microseconds(1000000);
    opts.backend_options = fs;
    auto exporter = otlp::OtlpFileExporterFactory::Create(opts);
    auto processor =
        sdktrace::SimpleSpanProcessorFactory::Create(std::move(exporter));
    trace_sp = sdktrace::TracerProviderFactory::Create(std::move(processor));
    sdktrace::Provider::SetTracerProvider(trace_sp);
    auto tracer = trace_api::Provider::GetTracerProvider()->GetTracer("otel-probe", "1.0.0");
    auto span = tracer->StartSpan("probe.span.alpha");
    span->SetAttribute("probe.attr", "vulkan_craft");
    span->End();
    static_cast<sdktrace::TracerProvider*>(trace_sp.get())->ForceFlush();
  }

  // ---- LOG: log record through OTLP file exporter ----
  std::printf("[logs] creating provider with OTLP file exporter\n");
  {
    otlp::OtlpFileLogRecordExporterOptions opts;
    otlp::OtlpFileClientFileSystemOptions fs;
    fs.file_pattern = "otel-probe-logs.json";
    fs.flush_interval = std::chrono::microseconds(1000000);
    opts.backend_options = fs;
    auto exporter = otlp::OtlpFileLogRecordExporterFactory::Create(opts);
    auto processor =
        sdklogs::SimpleLogRecordProcessorFactory::Create(std::move(exporter));
    log_sp = sdklogs::LoggerProviderFactory::Create(std::move(processor));
    sdklogs::Provider::SetLoggerProvider(log_sp);
    auto logger = logs_api::Provider::GetLoggerProvider()->GetLogger("otel-probe-logger", "1.0.0");
    auto rec = logger->CreateLogRecord();
    rec->SetSeverity(logs_api::Severity::kInfo);
    rec->SetBody("probe log body hello");
    rec->SetAttribute("probe.log.attr", "42");
    logger->EmitLogRecord(std::move(rec));
    static_cast<sdklogs::LoggerProvider*>(log_sp.get())->ForceFlush();
  }

  // ---- METRIC: counter through OTLP file exporter ----
  std::printf("[metrics] creating provider with OTLP file exporter\n");
  {
    otlp::OtlpFileMetricExporterOptions opts;
    otlp::OtlpFileClientFileSystemOptions fs;
    fs.file_pattern = "otel-probe-metrics.json";
    fs.flush_interval = std::chrono::microseconds(1000000);
    opts.backend_options = fs;
    auto exporter = otlp::OtlpFileMetricExporterFactory::Create(opts);
    sdkmetrics::PeriodicExportingMetricReaderOptions ropts;
    ropts.export_interval_millis = std::chrono::milliseconds(1000);
    ropts.export_timeout_millis = std::chrono::milliseconds(500);
    auto reader =
        sdkmetrics::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), ropts);
    std::vector<std::shared_ptr<sdkmetrics::MetricReader>> readers;
    readers.push_back(std::move(reader));
    auto context = sdkmetrics::MeterContextFactory::Create();
    for (auto &r : readers) context->AddMetricReader(r);
    metric_sp = sdkmetrics::MeterProviderFactory::Create(std::move(context));
    sdkmetrics::Provider::SetMeterProvider(metric_sp);
    auto meter = static_cast<sdkmetrics::MeterProvider*>(metric_sp.get())->GetMeter("otel-probe-meter", "1.0.0");
    auto counter = meter->CreateUInt64Counter("probe.counter.calls");
    counter->Add(7);
    static_cast<sdkmetrics::MeterProvider*>(metric_sp.get())->ForceFlush(std::chrono::milliseconds(2000));
  }

  // ---- Force-flush and release providers so file exporters write to disk ----
  trace_api::Provider::SetTracerProvider(opentelemetry::nostd::shared_ptr<trace_api::TracerProvider>{});
  logs_api::Provider::SetLoggerProvider(opentelemetry::nostd::shared_ptr<logs_api::LoggerProvider>{});
  static_cast<sdkmetrics::MeterProvider*>(metric_sp.get())->ForceFlush(std::chrono::milliseconds(2000));
  sdkmetrics::Provider::SetMeterProvider(opentelemetry::nostd::shared_ptr<metrics_api::MeterProvider>{});
  trace_sp.reset();
  log_sp.reset();
  metric_sp.reset();

  // ---- verify the three files ----
  std::printf("[verify] checking OTLP JSON output files\n");
  // traces
  {
    std::string out = slurp("otel-probe-traces.json");
    check(!out.empty(), "trace file written");
    if (!out.empty()) {
      check(contains(out, "resourceSpans"), "trace has resourceSpans");
      check(contains(out, "probe.span.alpha"), "trace contains span name");
      check(contains(out, "vulkan_craft"), "trace contains attribute value");
      check(contains(out, "otel-probe"), "trace contains instrumentation scope");
    }
  }
  // logs
  {
    std::string out = slurp("otel-probe-logs.json");
    check(!out.empty(), "log file written");
    if (!out.empty()) {
      check(contains(out, "resourceLogs"), "log has resourceLogs");
      check(contains(out, "probe log body hello"), "log contains body");
      check(contains(out, "probe.log.attr"), "log contains attribute key");
    }
  }
  // metrics
  {
    std::string out = slurp("otel-probe-metrics.json");
    check(!out.empty(), "metric file written");
    if (!out.empty()) {
      check(contains(out, "resourceMetrics"), "metric has resourceMetrics");
      check(contains(out, "probe.counter.calls"), "metric contains counter name");
    }
  }

  std::printf("\nRESULT: %d checks, %d failures\n", g_checks, g_fails);
  std::fflush(stdout);
  return g_fails == 0 ? 0 : 1;
}
