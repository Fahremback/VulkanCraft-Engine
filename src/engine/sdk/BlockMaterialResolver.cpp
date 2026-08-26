// BlockMaterialResolver.cpp — Agente 1 (task_plan B.1), the deterministic
// per-face block material resolver behind the public IBlockMaterialResolver
// contract. Consumes the data-driven BlockRegistry (voxel domain contract)
// and resolves the effective material per face with the documented precedence:
// active named state (>= 1) overrides the block; states[0] / out-of-range =
// the block level. Bit-exact for the same inputs on every machine. No clock.

#include "engine/rendering/IBlockMaterialResolver.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

std::uint32_t fnv1a(const std::string& text, std::uint32_t seed) {
    std::uint32_t h = seed;
    for (char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

class BlockMaterialResolver final : public IBlockMaterialResolver {
public:
    BlockMaterialResolver() : config_(BlockMaterialConfig{}) {}

    bool configure(const BlockMaterialConfig& config,
                   std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const BlockMaterialConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        BlockMaterialConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"variantSeed\": " << config_.variantSeed << " }";
        return o.str();
    }

    bool resolveFace(const engine::registry::BlockDefinition& def,
                     int stateIndex, BlockFace face,
                     FaceMaterial& out) const noexcept override {
        if (face >= BlockFace::Count) {
            return false;
        }
        const bool hasState = stateIndex >= 1 &&
                              stateIndex < static_cast<int>(def.states.size());
        glm::vec4 color;
        if (hasState) {
            const engine::registry::BlockState& s = def.states[stateIndex];
            color = faceColor(s.faceTopSet, s.faceTop, s.faceBottomSet,
                              s.faceBottom, s.faceSideSet, s.faceSide, face,
                              s.color);
        } else {
            color = faceColor(def.faceTopSet, def.faceTop, def.faceBottomSet,
                              def.faceBottom, def.faceSideSet, def.faceSide,
                              face, def.color);
        }
        out.color = color;
        out.variantKey = variantKey(def, stateIndex);
        return true;
    }

    BlockRenderInfo renderInfo(
        const engine::registry::BlockDefinition& def,
        int stateIndex) const noexcept override {
        BlockRenderInfo info;
        const bool hasState = stateIndex >= 1 &&
                              stateIndex < static_cast<int>(def.states.size());
        info.baseColor = hasState ? def.states[stateIndex].color : def.color;
        info.lightEmission =
            hasState ? def.states[stateIndex].lightEmission : def.lightEmission;
        info.opaque = def.opaque;
        info.occludes = def.occludes;
        info.renderLayer = def.renderLayer;
        info.blockClass = def.blockClass;
        return info;
    }

    std::uint32_t variantKey(
        const engine::registry::BlockDefinition& def,
        int stateIndex) const noexcept override {
        std::string key = def.namespaced();
        const bool hasState = stateIndex >= 1 &&
                              stateIndex < static_cast<int>(def.states.size());
        if (hasState) {
            key += "|";
            key += def.states[stateIndex].name;
        }
        return fnv1a(key, config_.variantSeed);
    }

private:
    static glm::vec4 faceColor(bool topSet, const glm::vec4& top,
                               bool bottomSet, const glm::vec4& bottom,
                               bool sideSet, const glm::vec4& side,
                               BlockFace face, const glm::vec4& base) {
        switch (face) {
            case BlockFace::Top:
                return topSet ? top : base;
            case BlockFace::Bottom:
                return bottomSet ? bottom : base;
            case BlockFace::SideNorth:
            case BlockFace::SideSouth:
            case BlockFace::SideEast:
            case BlockFace::SideWest:
                return sideSet ? side : base;
            default:
                return base;
        }
    }

    static bool parseJson(const std::string& text, BlockMaterialConfig& out,
                          std::string& errorOut) {
        struct Pair {
            std::string key;
            std::string value;
        };
        std::vector<Pair> pairs;
        {
            std::size_t i = 0;
            auto skipWs = [&]() {
                while (i < text.size() &&
                       (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                        text[i] == '\r')) {
                    ++i;
                }
            };
            skipWs();
            if (i >= text.size() || text[i] != '{') {
                errorOut = "BlockMaterialResolver config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "BlockMaterialResolver config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "BlockMaterialResolver config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "BlockMaterialResolver config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "BlockMaterialResolver config: unterminated string";
                        return false;
                    }
                    ++i;
                } else {
                    while (i < text.size() && text[i] != ',' && text[i] != '}') {
                        value.push_back(text[i++]);
                    }
                }
                pairs.push_back({key, value});
                skipWs();
                if (i < text.size() && text[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < text.size() && text[i] == '}') {
                    ++i;
                    break;
                }
                errorOut = "BlockMaterialResolver config: expected ',' or '}'";
                return false;
            }
        }

        BlockMaterialConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "BlockMaterialResolver config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "variantSeed") {
                parsed.variantSeed =
                    static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "BlockMaterialResolver config: unknown key '" + p.key +
                           "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "BlockMaterialResolver config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "BlockMaterialResolver config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    BlockMaterialConfig config_{};
};

}  // namespace

bool BlockMaterialConfig::valid(std::string& errorOut) const {
    if (variantSeed == 0) {
        errorOut = "variantSeed must be non-zero";
        return false;
    }
    return true;
}

std::unique_ptr<IBlockMaterialResolver> create_block_material_resolver(
    std::string& errorOut) {
    auto impl = std::make_unique<BlockMaterialResolver>();
    if (!impl) {
        errorOut = "BlockMaterialResolver: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IBlockMaterialResolver> create_block_material_resolver_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<BlockMaterialResolver>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering
