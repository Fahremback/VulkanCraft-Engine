#pragma once

// Block entity concreta do showcase (Lote 2 46/47): um contador determinístico
// por célula tickado via BlockTick do scheduler. Definida num header para que o
// VulkanEngineApp.hpp (que a detém como unique_ptr) e qualquer TU que inclua o
// header possa destruí-la (o tipo precisa estar completo no destrutor).

#include "engine/voxel/IVoxelBlockEntity.hpp"

#include <cstdint>
#include <string>
#include <vector>

class ShowcaseBlockEntity final : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "showcase.counter"; }
    void on_tick(uint64_t worldTick) override {
        ++ticks_;
        lastWorldTick_ = worldTick;
    }
    uint32_t data_version() const override { return 1; }
    std::vector<uint8_t> serialize_state() const override {
        std::vector<uint8_t> blob(16, 0);
        const uint64_t t = ticks_;
        const uint64_t w = lastWorldTick_;
        for (int i = 0; i < 8; ++i) {
            blob[static_cast<size_t>(i)] = static_cast<uint8_t>((t >> (i * 8)) & 0xFF);
            blob[static_cast<size_t>(i + 8)] = static_cast<uint8_t>((w >> (i * 8)) & 0xFF);
        }
        return blob;
    }
    bool deserialize_state(const std::vector<uint8_t>& data,
                           uint32_t version) override {
        if (data.size() < 16) return false;
        ticks_ = 0;
        lastWorldTick_ = 0;
        for (int i = 0; i < 8; ++i) {
            ticks_ |= static_cast<uint64_t>(data[static_cast<size_t>(i)]) << (i * 8);
            lastWorldTick_ |= static_cast<uint64_t>(data[static_cast<size_t>(i + 8)]) << (i * 8);
        }
        return true;
    }
    std::uint64_t ticks() const { return ticks_; }
    std::uint64_t last_world_tick() const { return lastWorldTick_; }

private:
    std::uint64_t ticks_{ 0 };
    std::uint64_t lastWorldTick_{ 0 };
};