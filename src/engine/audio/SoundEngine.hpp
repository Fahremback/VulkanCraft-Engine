#pragma once

#include "Voxel.hpp"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <memory>

class World;
struct ma_engine;
struct ma_sound;

class SoundEngine {
public:
    SoundEngine() = default;
    ~SoundEngine();

    void init();
    void shutdown();
    void update_ambience(const glm::vec3& listener, const World& world, float daylight, bool submerged, float dt);

    void play_sound(const std::string& soundName);
    void play_break_sound_for_block(BlockType block);
    void play_place_sound();
    void play_footstep_sound();
    void play_splash_sound();
    void play_fall_impact_sound();
    void play_hammer_sound();
    void play_fire_sound();

private:
    ma_engine* engine{ nullptr };
    std::unordered_map<std::string, std::string> soundFiles;
    std::unordered_map<std::string, ma_sound*> loops;
    float scanTimer{ 0.0f };
    float openness{ 1.0f };
    float waterPresence{ 0.0f };

    void register_sound(const std::string& soundName, const std::string& filePath);
    void start_loop(const std::string& name, const std::string& path);
};
