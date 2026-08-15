#pragma once

// AudioEditorModel — UI-independent document model for authoring audio
// content: an Audio Event (clip variations with weights, randomized pitch,
// volume, 3D distances, looping), an Audio Mixer (buses with gain/mute and
// effect chains) and Reverb/Ambient zones (position, size, wet/decay). The
// model tracks dirty state via EditorDocumentModel, supports snapshot-based
// undo/redo and serializes to JSON (see AudioEditorModel.cpp).

#include "EditorToolModel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine::Editor {

struct AudioVariationModel {
    UUID clip{0, 0};
    float weight{1.0f};
    float volume{1.0f};
    float pitch{1.0f};
};

enum class AudioBusEffectKind : uint8_t { Gain, LowPass, Delay };

struct AudioBusEffectModel {
    AudioBusEffectKind kind{AudioBusEffectKind::Gain};
    float amount{0.0f};
};

struct AudioBusModel {
    uint32_t id{0};
    std::string name;
    uint32_t parent{0}; // 0 = master/invalid (no parent)
    float gain{1.0f};
    bool muted{false};
    std::vector<AudioBusEffectModel> effects;
};

struct AudioReverbZoneModel {
    std::string name{"ReverbZone"};
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{5.0f};
    float wet{0.3f};
    float decay{0.35f};
    float preDelay{0.02f};
};

struct AudioAmbientZoneModel {
    std::string name{"AmbientZone"};
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{10.0f};
    std::string clipPath; // asset path of the ambient clip
    float gain{0.7f};
};

class AudioEditorModel final : public EditorDocumentModel {
public:
    // --- the audio event being edited ---
    std::string name{"Untitled Audio Event"};
    std::string bus{"Master"};
    float volume{1.0f};
    float minPitch{1.0f};
    float maxPitch{1.0f};
    float minDistance{1.0f};
    float maxDistance{100.0f};
    bool spatial{true};
    bool looping{false};
    std::vector<AudioVariationModel> variations;

    // --- mixer ---
    std::vector<AudioBusModel> buses;

    // --- zones ---
    std::vector<AudioReverbZoneModel> reverbZones;
    std::vector<AudioAmbientZoneModel> ambientZones;

    // --- event editing ---
    void add_variation(AudioVariationModel v) {
        Snapshot before = snapshot();
        variations.push_back(std::move(v));
        push_command("Add Audio Variation", before, snapshot());
    }
    bool remove_variation(std::size_t index) {
        if (index >= variations.size()) return false;
        Snapshot before = snapshot();
        variations.erase(variations.begin() + static_cast<std::ptrdiff_t>(index));
        push_command("Remove Audio Variation", before, snapshot());
        return true;
    }
    bool set_variation(std::size_t index, AudioVariationModel v) {
        if (index >= variations.size()) return false;
        Snapshot before = snapshot();
        variations[index] = std::move(v);
        push_command("Edit Audio Variation", before, snapshot());
        return true;
    }

    // --- mixer editing ---
    uint32_t add_bus(std::string busName, uint32_t parent = 0) {
        if (parent != 0 && !find_bus(parent)) return 0;
        Snapshot before = snapshot();
        uint32_t id = 1;
        for (const auto& bus : buses) id = std::max(id, bus.id + 1);
        buses.push_back(AudioBusModel{id, std::move(busName), parent, 1.0f, false, {}});
        push_command("Add Audio Bus", before, snapshot());
        return id;
    }
    bool remove_bus(uint32_t id) {
        if (!find_bus(id)) return false;
        Snapshot before = snapshot();
        buses.erase(std::remove_if(buses.begin(), buses.end(),
                                   [&](const auto& b) { return b.id == id; }),
                    buses.end());
        // Children are re-parented to master.
        for (auto& bus : buses) {
            if (bus.parent == id) bus.parent = 0;
        }
        push_command("Remove Audio Bus", before, snapshot());
        return true;
    }
    bool set_bus_gain(uint32_t id, float gain) {
        AudioBusModel* bus = find_bus(id);
        if (!bus) return false;
        Snapshot before = snapshot();
        bus->gain = gain;
        push_command("Set Bus Gain", before, snapshot());
        return true;
    }
    bool set_bus_muted(uint32_t id, bool muted) {
        AudioBusModel* bus = find_bus(id);
        if (!bus) return false;
        Snapshot before = snapshot();
        bus->muted = muted;
        push_command("Mute Bus", before, snapshot());
        return true;
    }
    bool add_bus_effect(uint32_t id, AudioBusEffectKind kind, float amount) {
        AudioBusModel* bus = find_bus(id);
        if (!bus) return false;
        Snapshot before = snapshot();
        bus->effects.push_back(AudioBusEffectModel{kind, amount});
        push_command("Add Bus Effect", before, snapshot());
        return true;
    }
    bool clear_bus_effects(uint32_t id) {
        AudioBusModel* bus = find_bus(id);
        if (!bus || bus->effects.empty()) return false;
        Snapshot before = snapshot();
        bus->effects.clear();
        push_command("Clear Bus Effects", before, snapshot());
        return true;
    }

    // --- zone editing ---
    bool add_reverb_zone(AudioReverbZoneModel zone) {
        Snapshot before = snapshot();
        reverbZones.push_back(std::move(zone));
        push_command("Add Reverb Zone", before, snapshot());
        return true;
    }
    bool remove_reverb_zone(std::size_t index) {
        if (index >= reverbZones.size()) return false;
        Snapshot before = snapshot();
        reverbZones.erase(reverbZones.begin() + static_cast<std::ptrdiff_t>(index));
        push_command("Remove Reverb Zone", before, snapshot());
        return true;
    }
    bool add_ambient_zone(AudioAmbientZoneModel zone) {
        Snapshot before = snapshot();
        ambientZones.push_back(std::move(zone));
        push_command("Add Ambient Zone", before, snapshot());
        return true;
    }
    bool remove_ambient_zone(std::size_t index) {
        if (index >= ambientZones.size()) return false;
        Snapshot before = snapshot();
        ambientZones.erase(ambientZones.begin() + static_cast<std::ptrdiff_t>(index));
        push_command("Remove Ambient Zone", before, snapshot());
        return true;
    }

    // --- undo / redo ---
    void undo() {
        if (undoStack_.empty()) return;
        Command command = std::move(undoStack_.back());
        undoStack_.pop_back();
        command.undo();
        redoStack_.push_back(std::move(command));
    }
    void redo() {
        if (redoStack_.empty()) return;
        Command command = std::move(redoStack_.back());
        redoStack_.pop_back();
        command.redo();
        undoStack_.push_back(std::move(command));
    }
    [[nodiscard]] bool can_undo() const noexcept { return !undoStack_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redoStack_.empty(); }
    [[nodiscard]] std::size_t undo_depth() const noexcept { return undoStack_.size(); }
    [[nodiscard]] std::size_t redo_depth() const noexcept { return redoStack_.size(); }
    void clear_undo() noexcept {
        undoStack_.clear();
        redoStack_.clear();
    }

    AudioBusModel* find_bus(uint32_t id) {
        const auto it = std::find_if(buses.begin(), buses.end(),
                                     [&](const auto& b) { return b.id == id; });
        return it == buses.end() ? nullptr : &*it;
    }
    const AudioBusModel* find_bus(uint32_t id) const {
        const auto it = std::find_if(buses.begin(), buses.end(),
                                     [&](const auto& b) { return b.id == id; });
        return it == buses.end() ? nullptr : &*it;
    }

    std::vector<ValidationIssue> validate() const override {
        std::vector<ValidationIssue> issues;
        const auto errors = [&](std::string field, std::string message) {
            issues.push_back({ValidationSeverity::Error, std::move(field), std::move(message)});
        };
        const auto warnings = [&](std::string field, std::string message) {
            issues.push_back({ValidationSeverity::Warning, std::move(field), std::move(message)});
        };

        if (variations.empty()) warnings("variations", "Audio event has no clips");
        if (volume < 0.0f || minPitch <= 0.0f || maxPitch < minPitch || maxDistance < minDistance) {
            errors("audio", "Invalid audio event ranges");
        }
        // Buses: duplicate names, missing parents, parent cycles.
        for (std::size_t i = 0; i < buses.size(); ++i) {
            for (std::size_t j = i + 1; j < buses.size(); ++j) {
                if (buses[i].name == buses[j].name) {
                    warnings(buses[i].name, "Duplicate bus name");
                }
            }
            if (buses[i].parent != 0 && !find_bus(buses[i].parent)) {
                errors(buses[i].name, "Bus parent does not exist");
            }
            if (buses[i].gain < 0.0f) errors(buses[i].name, "Bus gain cannot be negative");
        }
        if (bus_has_cycle()) errors("mixer", "Bus hierarchy contains a cycle");
        // Zones.
        for (const auto& zone : reverbZones) {
            if (zone.wet < 0.0f || zone.wet > 1.0f) warnings(zone.name, "Reverb wet should be in [0, 1]");
            if (zone.decay <= 0.0f) warnings(zone.name, "Reverb decay should be positive");
        }
        for (const auto& zone : ambientZones) {
            if (zone.gain < 0.0f) errors(zone.name, "Ambient gain cannot be negative");
        }
        return issues;
    }

    // --- serialization ---
    // In-memory JSON round-trip (also used by save/load_to_file).
    [[nodiscard]] std::string to_json() const;
    bool load_from_json_text(const std::string& document);
    bool save_to_file(const std::filesystem::path& path) const;
    bool load_from_file(const std::filesystem::path& path);

private:
    struct Snapshot {
        std::string name, bus;
        float volume, minPitch, maxPitch, minDistance, maxDistance;
        bool spatial, looping;
        std::vector<AudioVariationModel> variations;
        std::vector<AudioBusModel> buses;
        std::vector<AudioReverbZoneModel> reverbZones;
        std::vector<AudioAmbientZoneModel> ambientZones;
    };
    struct Command {
        std::string name;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    [[nodiscard]] Snapshot snapshot() const {
        return {name, bus, volume, minPitch, maxPitch, minDistance, maxDistance,
                spatial, looping, variations, buses, reverbZones, ambientZones};
    }
    void restore(const Snapshot& state) {
        name = state.name;
        bus = state.bus;
        volume = state.volume;
        minPitch = state.minPitch;
        maxPitch = state.maxPitch;
        minDistance = state.minDistance;
        maxDistance = state.maxDistance;
        spatial = state.spatial;
        looping = state.looping;
        variations = state.variations;
        buses = state.buses;
        reverbZones = state.reverbZones;
        ambientZones = state.ambientZones;
    }
    void push_command(std::string cmdName, Snapshot before, Snapshot after) {
        undoStack_.push_back(Command{
            std::move(cmdName),
            [this, before = std::move(before)]() {
                restore(before);
                changed();
            },
            [this, after = std::move(after)]() {
                restore(after);
                changed();
            }});
        redoStack_.clear();
        changed();
    }

    [[nodiscard]] bool bus_has_cycle() const {
        std::unordered_map<uint32_t, std::vector<uint32_t>> adjacency;
        for (const auto& b : buses) adjacency[b.id].push_back(b.parent);
        std::unordered_set<uint32_t> visited;
        std::unordered_set<uint32_t> inStack;
        std::function<bool(uint32_t)> dfs = [&](uint32_t id) -> bool {
            if (inStack.count(id) != 0) return true;
            if (visited.count(id) != 0) return false;
            visited.insert(id);
            inStack.insert(id);
            for (const uint32_t next : adjacency[id]) {
                if (next != 0 && dfs(next)) return true;
            }
            inStack.erase(id);
            return false;
        };
        for (const auto& entry : adjacency) {
            if (dfs(entry.first)) return true;
        }
        return false;
    }

    std::vector<Command> undoStack_;
    std::vector<Command> redoStack_;
};

} // namespace Engine::Editor
