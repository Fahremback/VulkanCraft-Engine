// FrameProfiler.cpp — the ONLY TU with the frame-profiler behavior (agente 2
// §B). Pure and deterministic: sliding window of frame times + heap, with
// min/max/avg/p95/p99, spike counting and a JSON snapshot. No clocks, no RNG,
// no globals — time only enters via record(frameMs).

#include "engine/profiling/IFrameProfiler.hpp"

#include <algorithm>
#include <deque>
#include <sstream>
#include <vector>

namespace engine {
namespace profiling {

namespace {

class FrameProfilerImpl final : public IFrameProfiler {
public:
    FrameProfilerImpl(std::size_t capacity, double spikeThresholdMs)
        : capacity_(capacity == 0 ? 600 : capacity),
          spikeThresholdMs_(spikeThresholdMs) {}

    bool record(double frameMs, double heapMb) override {
        if (frameMs < 0.0 || heapMb < 0.0) return false;
        frames_.push_back(frameMs);
        heap_.push_back(heapMb);
        if (frames_.size() > capacity_) {
            frames_.pop_front();
            heap_.pop_front();
        }
        if (spikeThresholdMs_ > 0.0 && frameMs > spikeThresholdMs_) {
            ++spikeCount_;
        }
        if (heapMb > heapPeakMb_) heapPeakMb_ = heapMb;
        return true;
    }

    void clear() override {
        frames_.clear();
        heap_.clear();
        spikeCount_ = 0;
        heapPeakMb_ = 0.0;
    }

    ProfilerSnapshot snapshot() const override {
        ProfilerSnapshot snap;
        snap.samples = frames_.size();
        snap.fps = 0.0;
        if (frames_.empty()) return snap;

        // Last-sample fps + heap.
        const double lastMs = frames_.back();
        snap.fps = 1000.0 / lastMs;
        snap.heapMb = heap_.back();
        snap.heapPeakMb = heapPeakMb_;

        // min/max/avg over the window.
        double sum = 0.0;
        double minMs = frames_.front();
        double maxMs = frames_.front();
        for (const double ms : frames_) {
            sum += ms;
            if (ms < minMs) minMs = ms;
            if (ms > maxMs) maxMs = ms;
        }
        snap.minMs = minMs;
        snap.maxMs = maxMs;
        snap.avgMs = sum / static_cast<double>(frames_.size());

        // Percentiles: nearest-rank on the sorted window copy.
        std::vector<double> sorted(frames_.begin(), frames_.end());
        std::sort(sorted.begin(), sorted.end());
        snap.p95Ms = percentile(sorted, 0.95);
        snap.p99Ms = percentile(sorted, 0.99);

        snap.spikeCount = spikeCount_;
        return snap;
    }

    std::string to_json() const override {
        const ProfilerSnapshot snap = snapshot();
        std::ostringstream out;
        out.setf(std::ios::fixed, std::ios::floatfield);
        out.precision(3);
        out << "{\"samples\":" << snap.samples
            << ",\"min\":" << snap.minMs
            << ",\"max\":" << snap.maxMs
            << ",\"avg\":" << snap.avgMs
            << ",\"p95\":" << snap.p95Ms
            << ",\"p99\":" << snap.p99Ms
            << ",\"spike_count\":" << snap.spikeCount
            << ",\"fps\":" << snap.fps
            << ",\"heap_mb\":" << snap.heapMb
            << ",\"heap_peak_mb\":" << snap.heapPeakMb << "}";
        return out.str();
    }

private:
    static double percentile(const std::vector<double>& sorted, double q) {
        if (sorted.empty()) return 0.0;
        const std::size_t rank = static_cast<std::size_t>(
            q * static_cast<double>(sorted.size()));
        const std::size_t clamped = std::min(rank, sorted.size() - 1);
        return sorted[clamped];
    }

    std::size_t capacity_;
    double spikeThresholdMs_;
    std::deque<double> frames_;
    std::deque<double> heap_;
    std::uint64_t spikeCount_{ 0 };
    double heapPeakMb_{ 0.0 };
};

}  // namespace

std::unique_ptr<IFrameProfiler> create_frame_profiler(
    std::size_t capacity, double spikeThresholdMs) {
    return std::make_unique<FrameProfilerImpl>(capacity, spikeThresholdMs);
}

}  // namespace profiling
}  // namespace engine
