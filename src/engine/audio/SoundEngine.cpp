#include <miniaudio.h>
#include "SoundEngine.hpp"
#include "World.hpp"
#include <algorithm>
#include <cmath>
#include "engine/core/logging/Log.hpp"
#include <iostream>
#include <filesystem>

namespace {
std::string asset(const char* relative) {
    return (std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "audio" / relative).string();
}
}

SoundEngine::~SoundEngine() { shutdown(); }

void SoundEngine::register_sound(const std::string& name, const std::string& path) {
    soundFiles[name] = asset(path.c_str());
}

bool SoundEngine::register_audio_asset(const std::string& name, const std::string& path) {
    if (name.empty() || path.empty() || !std::filesystem::exists(path)) return false;
    soundFiles[name] = path;
    return true;
}

void SoundEngine::start_loop(const std::string& name, const std::string& path) {
    if (!engine) return;
    auto* sound = new ma_sound{};
    const std::string fullPath = asset(path.c_str());
    if (ma_sound_init_from_file(engine, fullPath.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound) != MA_SUCCESS) {
        delete sound;
        VC_LOG_ERROR("[Audio] Failed to load ambience: {}", fullPath);
        return;
    }
    ma_sound_set_looping(sound, MA_TRUE);
    ma_sound_set_volume(sound, 0.0f);
    ma_sound_start(sound);
    loops[name] = sound;
}

void SoundEngine::init() {
    if (engine) return;
    engine = new ma_engine{};
    if (ma_engine_init(nullptr, engine) != MA_SUCCESS) {
        delete engine; engine = nullptr;
        VC_LOG_ERROR("[Audio] Could not initialize audio device.");
        return;
    }
    register_sound("step", "step.wav"); register_sound("water_splash", "water_splash.wav");
    register_sound("break_grass", "break_grass.wav"); register_sound("break_wood", "break_wood.wav");
    register_sound("break_leaves", "break_leaves.wav"); register_sound("break_stone", "break_stone.wav");
    register_sound("place_block", "place_block.wav"); register_sound("fall_impact", "fall_impact.wav");
    register_sound("wood_hammering", "wood_hammering.wav"); register_sound("fire_burn", "fire_burn.wav");
    start_loop("wind", "forest_wind.wav"); start_loop("birds", "birds_chirping.wav");
    start_loop("water", "river_flow.wav"); start_loop("night", "crickets_night.wav");
    start_loop("leaves", "leaves_rustle.wav");
    VC_LOG_INFO("[Audio] Multichannel soundscape initialized: {} ambient loops, {} sound effects.", loops.size(), soundFiles.size());
}

void SoundEngine::shutdown() {
    for (auto& [_, sound] : loops) { ma_sound_uninit(sound); delete sound; }
    loops.clear();
    if (engine) { ma_engine_uninit(engine); delete engine; engine = nullptr; }
}

void SoundEngine::play_sound(const std::string& name) {
    if (!engine) return;
    auto it = soundFiles.find(name);
    if (it != soundFiles.end()) ma_engine_play_sound(engine, it->second.c_str(), nullptr);
}

void SoundEngine::update_ambience(const glm::vec3& listener, const World& world, float daylight, bool submerged, float dt) {
    if (!engine) return;
    scanTimer += dt;
    if (scanTimer >= 0.75f) {
        scanTimer = 0.0f;
        int openRays = 0, waterHits = 0, samples = 0;
        for (int dx = -12; dx <= 12; dx += 4) for (int dz = -12; dz <= 12; dz += 4) {
            bool open = true;
            for (int dy = 2; dy <= 18; dy += 4) {
                // A.2: registry-driven queries — as_builtin_block mapped dynamic
                // blocks to Air, so ambience never heard JSON-defined blocks.
                const RuntimeBlockId b =
                    world.get_block_at(listener + glm::vec3(dx, dy, dz));
                if (world.is_solid_block_id(b)) open = false;
                if (world.is_fluid_runtime_id(b)) ++waterHits;
                ++samples;
            }
            if (open) ++openRays;
        }
        openness = std::clamp(float(openRays) / 49.0f, 0.0f, 1.0f);
        waterPresence = std::clamp(float(waterHits) / 12.0f, 0.0f, 1.0f);
    }
    ma_engine_listener_set_position(engine, 0, listener.x, listener.y, listener.z);
    auto fade = [&](const char* name, float target) {
        auto it = loops.find(name); if (it == loops.end()) return;
        float current = ma_sound_get_volume(it->second);
        ma_sound_set_volume(it->second, current + (target - current) * std::min(dt * 1.8f, 1.0f));
    };
    const float muffled = submerged ? 0.18f : 1.0f;
    fade("wind", (0.08f + openness * 0.20f) * muffled);
    fade("leaves", (1.0f - openness) * 0.16f * muffled);
    fade("birds", daylight * 0.18f * muffled);
    fade("night", (1.0f - daylight) * 0.22f * muffled);
    fade("water", std::max(waterPresence * 0.30f, submerged ? 0.25f : 0.0f));
}

void SoundEngine::play_break_sound_for_block(RuntimeBlockId block) {
    // A.2: registry-id driven — as_builtin_block collapsed every dynamic block
    // to Air, so breaking a JSON-defined block always played the stone sound.
    // Compare against builtin ids; anything else falls back to stone.
    if (block == static_cast<RuntimeBlockId>(BlockType::Grass)) play_sound("break_grass");
    else if (block == static_cast<RuntimeBlockId>(BlockType::Wood) ||
             block == static_cast<RuntimeBlockId>(BlockType::Planks)) play_sound("break_wood");
    else if (block == static_cast<RuntimeBlockId>(BlockType::Leaves)) play_sound("break_leaves");
    else play_sound("break_stone");
}
void SoundEngine::play_place_sound() { play_sound("place_block"); }
void SoundEngine::play_footstep_sound() { play_sound("step"); }
void SoundEngine::play_splash_sound() { play_sound("water_splash"); }
void SoundEngine::play_fall_impact_sound() { play_sound("fall_impact"); }
void SoundEngine::play_hammer_sound() { play_sound("wood_hammering"); }
void SoundEngine::play_fire_sound() { play_sound("fire_burn"); }
