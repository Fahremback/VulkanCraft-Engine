// RenderPassMetrics.cpp — Agente 1 (task_plan B): the ONLY adapter for the
// public renderer-telemetry contract (IRenderPassMetrics). Deterministic
// aggregator: per-pass CPU/GPU timing windows (sliding, p95/p99 from sorted
// samples), memory pool residency, streaming counters. The Vulkan seam only
// samples clocks/counters; nothing here touches the GPU.

#include "engine/rendering/IRenderPassMetrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {
namespace {

double percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) return 0.0;
    // Nearest-rank percentile (deterministic).
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

struct PassWindow {
    std::string name;
    std::vector<double> cpuMs;
    std::vector<double> gpuMs;
};

struct PoolState {
    std::string name;
    std::vector<std::uint64_t> samples;  // one per recordMemory call
    std::uint64_t currentBytes{ 0 };
};

struct StreamState {
    std::string name;
    std::uint64_t frameLoaded{ 0 };
    std::uint64_t frameEvicted{ 0 };
    std::uint64_t frameBytes{ 0 };
    std::uint64_t totalLoaded{ 0 };
    std::uint64_t totalEvicted{ 0 };
    std::uint64_t totalBytes{ 0 };
    std::uint64_t peakBytesInFlight{ 0 };
};

class RenderPassMetrics final : public IRenderPassMetrics {
public:
    explicit RenderPassMetrics(std::size_t capacity)
        : capacity_(capacity == 0 ? kRenderMetricsDefaultWindow : capacity) {}

    bool recordPass(const std::string& pass, double cpuMs, double gpuMs) override {
        if (pass.empty() || !std::isfinite(cpuMs) || !std::isfinite(gpuMs) ||
            cpuMs < 0.0 || gpuMs < 0.0) {
            return false;
        }
        PassWindow& w = windowOf(pass);
        w.cpuMs.push_back(cpuMs);
        w.gpuMs.push_back(gpuMs);
        if (w.cpuMs.size() > capacity_) w.cpuMs.erase(w.cpuMs.begin());
        if (w.gpuMs.size() > capacity_) w.gpuMs.erase(w.gpuMs.begin());
        return true;
    }

    bool recordMemory(const std::string& pool, std::uint64_t bytes) override {
        if (pool.empty()) return false;
        PoolState& p = poolOf(pool);
        p.currentBytes = bytes;
        p.samples.push_back(bytes);
        return true;
    }

    bool recordStreaming(const std::string& stream,
                         std::uint64_t loaded, std::uint64_t evicted,
                         std::uint64_t bytesLoaded) override {
        if (stream.empty()) return false;
        StreamState& s = streamOf(stream);
        s.frameLoaded += loaded;
        s.frameEvicted += evicted;
        s.frameBytes += bytesLoaded;
        return true;
    }

    void endFrame() override {
        for (auto& [name, s] : streams_) {
            s.totalLoaded += s.frameLoaded;
            s.totalEvicted += s.frameEvicted;
            s.totalBytes += s.frameBytes;
            const std::uint64_t inFlight =
                s.totalLoaded > s.totalEvicted ? s.totalLoaded - s.totalEvicted : 0;
            s.peakBytesInFlight = std::max(s.peakBytesInFlight, inFlight);
            s.frameLoaded = 0;
            s.frameEvicted = 0;
            s.frameBytes = 0;
        }
    }

    void clear() override {
        passes_.clear();
        pools_.clear();
        streams_.clear();
    }

    RenderMetricsSnapshot snapshot() const override {
        RenderMetricsSnapshot out;
        // Passes in order of first appearance (insertion order).
        for (const auto& [name, w] : passes_) {
            if (w.cpuMs.empty()) continue;
            RenderPassStats s;
            s.name = name;
            s.samples = w.cpuMs.size();

            std::vector<double> cpu = w.cpuMs;
            std::vector<double> gpu = w.gpuMs;
            std::sort(cpu.begin(), cpu.end());
            std::sort(gpu.begin(), gpu.end());

            s.cpuMsMin = cpu.front();
            s.cpuMsMax = cpu.back();
            double cpuSum = 0.0;
            for (double v : cpu) cpuSum += v;
            s.cpuMsAvg = cpuSum / static_cast<double>(cpu.size());
            s.cpuMsP95 = percentile(cpu, 95.0);
            s.cpuMsP99 = percentile(cpu, 99.0);

            s.gpuMsMin = gpu.front();
            s.gpuMsMax = gpu.back();
            double gpuSum = 0.0;
            for (double v : gpu) gpuSum += v;
            s.gpuMsAvg = gpuSum / static_cast<double>(gpu.size());
            s.gpuMsP95 = percentile(gpu, 95.0);
            s.gpuMsP99 = percentile(gpu, 99.0);

            out.passes.push_back(std::move(s));
        }
        for (const auto& [name, p] : pools_) {
            if (p.samples.empty()) continue;
            RenderMemoryPoolStats s;
            s.name = name;
            s.samples = p.samples.size();
            s.currentBytes = p.samples.back();
            s.minBytes = *std::min_element(p.samples.begin(), p.samples.end());
            s.maxBytes = *std::max_element(p.samples.begin(), p.samples.end());
            s.peakBytes = s.maxBytes;
            out.pools.push_back(std::move(s));
        }
        for (const auto& [name, st] : streams_) {
            RenderStreamingStats s;
            s.name = name;
            s.totalLoaded = st.totalLoaded;
            s.totalEvicted = st.totalEvicted;
            s.bytesLoaded = st.totalBytes;
            s.peakBytesInFlight = st.peakBytesInFlight;
            out.streams.push_back(std::move(s));
        }
        return out;
    }

    std::string to_json() const override {
        const RenderMetricsSnapshot snap = snapshot();
        std::string json = "{\"passes\":[";
        bool firstPass = true;
        for (const RenderPassStats& p : snap.passes) {
            if (!firstPass) json += ",";
            firstPass = false;
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "{\"name\":\"%s\",\"samples\":%zu,\"cpu\":{\"min\":%.6g,"
                "\"max\":%.6g,\"avg\":%.6g,\"p95\":%.6g,\"p99\":%.6g},"
                "\"gpu\":{\"min\":%.6g,\"max\":%.6g,\"avg\":%.6g,\"p95\":%.6g,"
                "\"p99\":%.6g}}",
                jsonEscape(p.name).c_str(), p.samples,
                p.cpuMsMin, p.cpuMsMax, p.cpuMsAvg, p.cpuMsP95, p.cpuMsP99,
                p.gpuMsMin, p.gpuMsMax, p.gpuMsAvg, p.gpuMsP95, p.gpuMsP99);
            json += buf;
        }
        json += "],\"pools\":[";
        bool firstPool = true;
        for (const RenderMemoryPoolStats& p : snap.pools) {
            if (!firstPool) json += ",";
            firstPool = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "{\"name\":\"%s\",\"samples\":%zu,\"current\":%llu,"
                "\"min\":%llu,\"max\":%llu,\"peak\":%llu}",
                jsonEscape(p.name).c_str(), p.samples,
                static_cast<unsigned long long>(p.currentBytes),
                static_cast<unsigned long long>(p.minBytes),
                static_cast<unsigned long long>(p.maxBytes),
                static_cast<unsigned long long>(p.peakBytes));
            json += buf;
        }
        json += "],\"streams\":[";
        bool firstStream = true;
        for (const RenderStreamingStats& s : snap.streams) {
            if (!firstStream) json += ",";
            firstStream = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "{\"name\":\"%s\",\"loaded\":%llu,\"evicted\":%llu,"
                "\"bytes\":%llu,\"peakInFlight\":%llu}",
                jsonEscape(s.name).c_str(),
                static_cast<unsigned long long>(s.totalLoaded),
                static_cast<unsigned long long>(s.totalEvicted),
                static_cast<unsigned long long>(s.bytesLoaded),
                static_cast<unsigned long long>(s.peakBytesInFlight));
            json += buf;
        }
        json += "]}";
        return json;
    }

private:
    PassWindow& windowOf(const std::string& name) {
        for (auto& [n, w] : passes_) {
            if (n == name) return w;
        }
        PassWindow w;
        w.name = name;
        passes_.push_back({ name, std::move(w) });
        return passes_.back().second;
    }

    PoolState& poolOf(const std::string& name) {
        for (auto& [n, p] : pools_) {
            if (n == name) return p;
        }
        PoolState p;
        p.name = name;
        pools_.push_back({ name, std::move(p) });
        return pools_.back().second;
    }

    StreamState& streamOf(const std::string& name) {
        for (auto& [n, s] : streams_) {
            if (n == name) return s;
        }
        StreamState s;
        s.name = name;
        streams_.push_back({ name, std::move(s) });
        return streams_.back().second;
    }

    std::size_t capacity_;
    std::vector<std::pair<std::string, PassWindow>> passes_;
    std::vector<std::pair<std::string, PoolState>> pools_;
    std::vector<std::pair<std::string, StreamState>> streams_;
};

}  // namespace

std::unique_ptr<IRenderPassMetrics> create_render_pass_metrics(std::size_t capacity) {
    return std::make_unique<RenderPassMetrics>(capacity);
}

}  // namespace Engine::Rendering
