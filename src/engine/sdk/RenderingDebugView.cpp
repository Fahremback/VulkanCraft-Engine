// RenderingDebugView.cpp — Agente 1 (task_plan A.17): the HEADLESS debug-view
// data model. Aggregates the state of every rendering contract into a
// serializable snapshot with a deterministic JSON emitter, so the editor/
// profiler/CLI/MCP can render the debug views without the concrete backends.
// Self-contained (std + glm).

#include "engine/rendering/IRenderingDebugView.hpp"

#include <cstdio>
#include <sstream>

namespace Engine::Rendering {
namespace {

void write_float(std::ostringstream& out, float value) {
    // %.6g is the canonical deterministic float form used across the SDK.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    out << buffer;
}

void write_vec3(std::ostringstream& out, const glm::vec3& v) {
    out << '[';
    write_float(out, v.x);
    out << ',';
    write_float(out, v.y);
    out << ',';
    write_float(out, v.z);
    out << ']';
}

void write_vec4(std::ostringstream& out, const glm::vec4& v) {
    out << '[';
    write_float(out, v.x);
    out << ',';
    write_float(out, v.y);
    out << ',';
    write_float(out, v.z);
    out << ',';
    write_float(out, v.w);
    out << ']';
}

void write_ivec4(std::ostringstream& out, const glm::ivec4& v) {
    out << '[' << v.x << ',' << v.y << ',' << v.z << ',' << v.w << ']';
}

class RenderingDebugView final : public IRenderingDebugView {
public:
    RenderingDebugView() = default;

    void refresh() override {
        // The snapshot already holds the bound state (bind_* mutate it); refresh
        // is a no-op that exists so callers have an explicit re-sync point.
    }

    const RenderingDebugSnapshot& snapshot() const noexcept override {
        return snapshot_;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"cardCount\":" << snapshot_.cardCount
            << ",\"cards\":[";
        for (std::size_t i = 0; i < snapshot_.cards.size(); ++i) {
            if (i) out << ',';
            const DebugCard& c = snapshot_.cards[i];
            out << "{\"center\":";
            write_vec3(out, c.center);
            out << ",\"normal\":";
            write_vec3(out, c.normal);
            out << ",\"albedo\":";
            write_vec4(out, c.albedo);
            out << ",\"emissive\":";
            write_vec3(out, c.emissive);
            out << ",\"cascade\":" << static_cast<unsigned>(c.cascade) << '}';
        }
        out << "],\"cardsPerCascade\":[";
        for (std::size_t i = 0; i < snapshot_.cardsPerCascade.size(); ++i) {
            if (i) out << ',';
            out << snapshot_.cardsPerCascade[i];
        }
        out << "],\"capturedCount\":" << snapshot_.capturedCount
            << ",\"pendingCount\":" << snapshot_.pendingCount
            << ",\"vramBytes\":" << snapshot_.vramBytes
            << ",\"probeCount\":" << snapshot_.probeCount
            << ",\"pendingProbes\":" << snapshot_.pendingProbes
            << ",\"sunRevision\":" << snapshot_.sunRevision
            << ",\"probes\":[";
        for (std::size_t i = 0; i < snapshot_.probes.size(); ++i) {
            if (i) out << ',';
            const DebugProbe& p = snapshot_.probes[i];
            out << "{\"radianceVisibility\":";
            write_vec4(out, p.radianceVisibility);
            out << ",\"worldCellCascade\":";
            write_ivec4(out, p.worldCellCascade);
            out << '}';
        }
        out << "],\"tracePaths\":[";
        for (std::size_t i = 0; i < snapshot_.tracePaths.size(); ++i) {
            if (i) out << ',';
            const DebugTracePath& t = snapshot_.tracePaths[i];
            out << "{\"origin\":";
            write_vec3(out, t.origin);
            out << ",\"direction\":";
            write_vec3(out, t.direction);
            out << ",\"hit\":" << (t.hit ? "true" : "false")
                << ",\"distance\":";
            write_float(out, t.distance);
            out << ",\"steps\":" << t.steps << '}';
        }
        out << "],\"disoccludedPixels\":" << snapshot_.disoccludedPixels
            << ",\"confidenceLevel\":" << snapshot_.confidenceLevel << '}';
        return out.str();
    }

    void bind_cards(const std::vector<DebugCard>& cards,
                    const std::vector<std::uint32_t>& cardsPerCascade) override {
        snapshot_.cards = cards;
        snapshot_.cardCount = static_cast<std::uint32_t>(cards.size());
        snapshot_.cardsPerCascade = cardsPerCascade;
    }
    void bind_capture(std::uint32_t captured, std::uint32_t pending,
                      std::uint64_t vramBytes) override {
        snapshot_.capturedCount = captured;
        snapshot_.pendingCount = pending;
        snapshot_.vramBytes = vramBytes;
    }
    void bind_probes(const std::vector<DebugProbe>& probes,
                     std::uint32_t pending, std::uint32_t sunRevision) override {
        snapshot_.probes = probes;
        snapshot_.probeCount = static_cast<std::uint32_t>(probes.size());
        snapshot_.pendingProbes = pending;
        snapshot_.sunRevision = sunRevision;
    }
    void add_trace_path(const DebugTracePath& path) override {
        snapshot_.tracePaths.push_back(path);
    }
    void bind_disocclusion(std::uint32_t pixels, std::uint32_t confidence) override {
        snapshot_.disoccludedPixels = pixels;
        snapshot_.confidenceLevel = confidence;
    }

private:
    RenderingDebugSnapshot snapshot_;
};

}  // namespace

std::unique_ptr<IRenderingDebugView> create_rendering_debug_view(
    std::string& errorOut) {
    errorOut.clear();
    return std::make_unique<RenderingDebugView>();
}

}  // namespace Engine::Rendering
