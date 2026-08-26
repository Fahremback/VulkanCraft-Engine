// TimelinePolicy.cpp — the only TU implementing the public timeline policy
// (Agente 4 §6 item 68 CORE): deterministic retention budget (prune) and
// snapshot compaction (dedup of identical world+path). Pure std.

#include "engine/world/ITimelinePolicy.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace engine {
namespace world {
namespace {

class TimelinePolicy final : public ITimelinePolicy {
public:
    TimelinePolicy() = default;

    bool configure(const TimelinePolicyConfig& config,
                   std::string& errorOut) override {
        if (config.maxStates == 0) {
            errorOut = "timeline policy: maxStates must be >= 1";
            return false;
        }
        config_ = config;
        return true;
    }

    std::vector<std::string> prune(
        const std::vector<TimelinePolicyState>& states) const override {
        std::vector<std::string> out;
        if (states.size() <= config_.maxStates) return out;
        // Mantém os maxStates primeiros em ordem lexicográfica de nome.
        std::vector<std::string> names;
        names.reserve(states.size());
        for (const TimelinePolicyState& state : states) names.push_back(state.name);
        std::sort(names.begin(), names.end());
        std::map<std::string, bool> keep;
        for (std::size_t i = 0; i < config_.maxStates; ++i) keep[names[i]] = true;
        for (const TimelinePolicyState& state : states) {
            if (keep.count(state.name) == 0) out.push_back(state.name);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> compact(
        const std::vector<TimelinePolicyState>& states) const override {
        std::vector<std::string> out;
        if (!config_.compactionEnabled) return out;
        // Chave = worldName + '\x1f' + path; mantém o primeiro (lexicográfico).
        std::map<std::string, std::string> firstByName;   // chave → nome mantido
        std::map<std::string, std::string> keptByKey;     // chave → nome
        for (const TimelinePolicyState& state : states) {
            const std::string key = state.worldName + "\x1f" + state.path;
            const auto found = keptByKey.find(key);
            if (found == keptByKey.end()) {
                keptByKey[key] = state.name;
            } else if (state.name < found->second) {
                // Nome menor vence; o anterior vira remoção.
                out.push_back(found->second);
                found->second = state.name;
            } else {
                out.push_back(state.name);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::size_t max_states() const override { return config_.maxStates; }

private:
    TimelinePolicyConfig config_{};
};

}  // namespace

std::unique_ptr<ITimelinePolicy> create_timeline_policy() {
    return std::make_unique<TimelinePolicy>();
}

}  // namespace world
}  // namespace engine
