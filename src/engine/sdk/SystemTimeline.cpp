// SystemTimeline.cpp — the ONLY TU with the system-timeline behavior. Pure and
// deterministic aggregator for per-system CPU frame breakdown: begin_frame /
// record_system / end_frame feed a sliding window per system + a global window
// for the frame total; snapshot/to_json expose min/max/avg/p95/p99 (nearest-
// rank on the sorted window, matching IRenderPassMetrics' percentile). No
// clocks, no RNG, no globals — time only enters via record_system/end_frame.
//
// Frame semantics: record_system(name, ms) REPLACES that system's value for the
// open frame (the caller records each system's accumulated elapsed once after
// it runs, not per call-site delta). end_frame(frameTotalMs) closes the frame.

#include "engine/profiling/ISystemTimeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace profiling {
namespace {

double percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) return 0.0;
    // Nearest-rank percentile (deterministic; same as RenderPassMetrics).
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sorted.size()))) - 1;
    return sorted[std::min(rank, sorted.size() - 1)];
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

struct SystemWindow {
    std::string name;
    std::vector<double> ms;      // one total per closed frame
    double pendingMs{ 0.0 };     // value recorded for the current open frame
    bool pending{ false };
};

class SystemTimeline final : public ISystemTimeline {
public:
    explicit SystemTimeline(std::size_t capacity)
        : capacity_(capacity == 0 ? kSystemTimelineDefaultWindow : capacity) {}

    bool begin_frame() override {
        // Discard partial data for the frame that was never closed; a fresh
        // frame starts with no pending system values.
        for (auto& [name, w] : systems_) {
            (void)name;
            w.pending = false;
            w.pendingMs = 0.0;
        }
        return true;
    }

    bool record_system(const std::string& name, double ms) override {
        if (name.empty() || !std::isfinite(ms) || ms < 0.0) return false;
        SystemWindow& w = windowOf(name);
        w.pending = true;
        w.pendingMs = ms;
        return true;
    }

    bool end_frame(double frameTotalMs) override {
        if (!std::isfinite(frameTotalMs) || frameTotalMs < 0.0) return false;
        ++frameCount_;
        frameTotals_.push_back(frameTotalMs);
        if (frameTotals_.size() > capacity_) frameTotals_.erase(frameTotals_.begin());
        // Push each recorded system's value into its window.
        for (auto& [name, w] : systems_) {
            (void)name;
            if (w.pending) {
                w.ms.push_back(w.pendingMs);
                if (w.ms.size() > capacity_) w.ms.erase(w.ms.begin());
            }
            w.pending = false;
            w.pendingMs = 0.0;
        }
        return true;
    }

    void clear() override {
        systems_.clear();
        frameTotals_.clear();
        frameCount_ = 0;
    }

    SystemTimelineSnapshot snapshot() const override {
        SystemTimelineSnapshot out;
        out.frameCount = frameCount_;
        for (const auto& [name, w] : systems_) {
            if (w.ms.empty()) continue;
            SystemTimelineStats s;
            s.name = name;
            s.frames = w.ms.size();
            std::vector<double> sorted = w.ms;
            std::sort(sorted.begin(), sorted.end());
            s.minMs = sorted.front();
            s.maxMs = sorted.back();
            double sum = 0.0;
            for (double v : sorted) sum += v;
            s.avgMs = sum / static_cast<double>(sorted.size());
            s.p95Ms = percentile(sorted, 95.0);
            s.p99Ms = percentile(sorted, 99.0);
            out.systems.push_back(std::move(s));
        }
        if (!frameTotals_.empty()) {
            std::vector<double> sorted = frameTotals_;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0.0;
            for (double v : sorted) sum += v;
            out.totalAvgMs = sum / static_cast<double>(sorted.size());
            out.totalP95Ms = percentile(sorted, 95.0);
            out.totalP99Ms = percentile(sorted, 99.0);
        }
        return out;
    }

    std::string to_json() const override {
        const SystemTimelineSnapshot snap = snapshot();
        std::string json = "{\"frames\":" + std::to_string(snap.frameCount);
        json += ",\"total\":{\"avg\":" + fmt(snap.totalAvgMs);
        json += ",\"p95\":" + fmt(snap.totalP95Ms);
        json += ",\"p99\":" + fmt(snap.totalP99Ms) + "}";
        json += ",\"systems\":[";
        bool first = true;
        for (const SystemTimelineStats& s : snap.systems) {
            if (!first) json += ",";
            first = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "{\"name\":\"%s\",\"frames\":%zu,\"min\":%.6g,\"max\":%.6g,"
                "\"avg\":%.6g,\"p95\":%.6g,\"p99\":%.6g}",
                jsonEscape(s.name).c_str(), s.frames,
                s.minMs, s.maxMs, s.avgMs, s.p95Ms, s.p99Ms);
            json += buf;
        }
        json += "]}";
        return json;
    }

private:
    static std::string fmt(double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        return buf;
    }

    SystemWindow& windowOf(const std::string& name) {
        for (auto& [n, w] : systems_) {
            if (n == name) return w;
        }
        SystemWindow w;
        w.name = name;
        systems_.push_back({ name, std::move(w) });
        return systems_.back().second;
    }

    std::size_t capacity_;
    std::vector<std::pair<std::string, SystemWindow>> systems_;
    std::vector<double> frameTotals_;
    std::size_t frameCount_{ 0 };
};

}  // namespace

std::unique_ptr<ISystemTimeline> create_system_timeline(std::size_t capacity) {
    return std::make_unique<SystemTimeline>(capacity);
}

}  // namespace profiling
}  // namespace engine