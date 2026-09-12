#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace vc::rendering::provider_selection {

struct Selection {
    std::string provider;
    std::string capability;
    int priority{0};
};

inline std::unordered_map<std::string, Selection>& selections() {
    static std::unordered_map<std::string, Selection> value;
    return value;
}

inline std::mutex& selection_mutex() {
    static std::mutex value;
    return value;
}

inline void record(std::string system, std::string provider,
                   std::string capability, int priority = 0) {
    std::scoped_lock lock(selection_mutex());
    auto& all = selections();
    auto it = all.find(system);
    if (it != all.end() && it->second.priority > priority) return;
    all[std::move(system)] =
        Selection{std::move(provider), std::move(capability), priority};
}

inline void record_canonical(std::string system, std::string provider,
                             std::string capability) {
    record(std::move(system), std::move(provider), std::move(capability), 100);
}

inline bool find(const std::string& system, Selection& out) {
    std::scoped_lock lock(selection_mutex());
    const auto it = selections().find(system);
    if (it == selections().end()) return false;
    out = it->second;
    return true;
}

inline void retire(const std::string& system) {
    std::scoped_lock lock(selection_mutex());
    selections().erase(system);
}

} // namespace vc::rendering::provider_selection
