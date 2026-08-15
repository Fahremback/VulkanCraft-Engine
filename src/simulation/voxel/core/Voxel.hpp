#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <array>
#include <cstddef>
#include <string>

enum class BlockType : uint8_t {
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Bedrock,
    Sand,
    Wood,
    Leaves,
    Planks,
    Cobblestone,
    Glass,
    Bricks,
    Water,
    Lava,
    Clay, // Deliberately color-only: proves textures are optional.
    WoodOak = Wood,
    LeavesOak = Leaves,
    PlanksOak = Planks,
    CoalOre = 15,
    IronOre,
    GoldOre,
    DiamondOre,
    EmeraldOre,
    RedstoneOre,
    LapisOre,
    CopperOre,
    WoodBirch,
    LeavesBirch,
    PlanksBirch,
    WoodSpruce,
    LeavesSpruce,
    PlanksSpruce,
    Granite,
    Diorite,
    Andesite,
    Deepslate,
    Blackstone,
    Basalt,
    Netherrack,
    EndStone,
    Obsidian,
    Sandstone,
    Terracotta,
    Glowstone,
    SeaLantern,
    MagmaBlock,
    CraftingTable,
    Furnace,
    Chest,
    TNT,
    Bookshelf,
    Prismarine,
    MossyCobble,
    SnowBlock,
    Count
};

// Compact runtime block id. The builtin BlockType enum keeps ids 0..Count-1
// (the engine's stable baked-in blocks); ids >= Count are DYNAMIC blocks whose
// identity lives in the block registry (UUID) and whose material/behavior come
// from its BlockDefinition. ChunkData stores these ids, not the game enum:
// the registry is the source of truth for identity, the enum is just the
// builtin prefix of the id space (Prioridade 0 item 1, FALTANTES).
//
// Dynamic ids are allocated deterministically (sorted by UUID) so the same
// registry content yields the same ids regardless of JSON load order.
using RuntimeBlockId = uint16_t;

constexpr RuntimeBlockId kRuntimeAirId = 0;

inline RuntimeBlockId runtime_id(BlockType type) {
    return static_cast<RuntimeBlockId>(type);
}

// True for the builtin prefix of the id space.
inline bool is_builtin_block(RuntimeBlockId id) {
    return id < static_cast<RuntimeBlockId>(BlockType::Count);
}

// Narrow a runtime id to the builtin enum. Dynamic ids (>= Count) are NOT
// representable in the enum: consumers that only understand baked-in blocks
// (game AI, audio, player) see Air for them, which is the correct "unknown"
// answer for a block the game does not define.
inline BlockType as_builtin_block(RuntimeBlockId id) {
    return is_builtin_block(id) ? static_cast<BlockType>(id) : BlockType::Air;
}

// Material/behavior snapshot for a DYNAMIC (registry-defined) block, copied
// into the world and into mesh snapshots so workers never touch the registry.
struct RuntimeBlockInfo {
    std::string uuid;              // persistent identity (empty = builtin)
    glm::vec4 color{ 1.0f };       // data-driven base color (no texture layer)
    bool solid{ true };            // collision / raycast
    bool transparent{ false };     // render pass / culling hints
    bool fluid{ false };
    // Discrete lighting (META section 12), 0..15. lightAbsorption is the
    // propagation cost of a cell (15 = opaque, blocks sky and block light);
    // the JSON float (0..1) is scaled by 15 when the runtime table is built.
    uint8_t lightEmission{ 0 };
    uint8_t lightAbsorption{ 15 };
};

// Every texture occupies one layer in TextureManager's 2D array.
// Adding a texture means adding one named value here and its pixels in TextureManager.
enum class TextureIndex : uint16_t {
    GrassTop = 0,
    GrassSide,
    Dirt,
    Stone,
    Bedrock,
    Sand,
    WoodSide,
    WoodTop,
    Leaves,
    Planks,
    Cobblestone,
    Glass,
    Bricks,
    Water,
    Lava,
    GrassBlade,
    PlayerSkin,
    CoalOre,
    IronOre,
    GoldOre,
    DiamondOre,
    EmeraldOre,
    RedstoneOre,
    LapisOre,
    CopperOre,
    BirchWoodSide,
    BirchWoodTop,
    BirchLeaves,
    BirchPlanks,
    SpruceWoodSide,
    SpruceWoodTop,
    SpruceLeaves,
    SprucePlanks,
    Granite,
    Diorite,
    Andesite,
    Deepslate,
    Blackstone,
    Basalt,
    Netherrack,
    EndStone,
    Obsidian,
    Sandstone,
    Terracotta,
    Glowstone,
    SeaLantern,
    MagmaBlock,
    CraftingTable,
    Furnace,
    Chest,
    TNT,
    Bookshelf,
    Prismarine,
    MossyCobble,
    SnowBlock,
    Count
};

struct FaceTextures {
    std::optional<TextureIndex> top;
    std::optional<TextureIndex> side;
    std::optional<TextureIndex> bottom;

    static FaceTextures none() { return {}; }
    static FaceTextures all(TextureIndex texture) { return { texture, texture, texture }; }
    static FaceTextures directional(TextureIndex topTexture, TextureIndex sideTexture, TextureIndex bottomTexture) {
        return { topTexture, sideTexture, bottomTexture };
    }
};

struct BlockMaterial {
    glm::vec4 color{ 1.0f };
    FaceTextures textures{ FaceTextures::none() };
    bool solid{ true };
    bool transparent{ false };
};

// Single registry for block appearance and physical flags. A block only needs a
// color; FaceTextures::none() is a fully supported material, not a fallback error.
inline const std::array<BlockMaterial, static_cast<std::size_t>(BlockType::Count)> BLOCK_MATERIALS{{
    { glm::vec4(0.0f), FaceTextures::none(), false, true },
    { glm::vec4(1.0f), FaceTextures::directional(TextureIndex::GrassTop, TextureIndex::GrassSide, TextureIndex::Dirt) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Dirt) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Stone) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Bedrock) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Sand) },
    { glm::vec4(1.0f), FaceTextures::directional(TextureIndex::WoodTop, TextureIndex::WoodSide, TextureIndex::WoodTop) },
    { glm::vec4(0.85f, 1.0f, 0.85f, 0.90f), FaceTextures::all(TextureIndex::Leaves), false, true },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Planks) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Cobblestone) },
    { glm::vec4(1.0f, 1.0f, 1.0f, 0.45f), FaceTextures::all(TextureIndex::Glass), true, true },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Bricks) },
    { glm::vec4(0.80f, 0.90f, 1.0f, 0.65f), FaceTextures::all(TextureIndex::Water), false, true },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Lava), false, false },
    { glm::vec4(0.64f, 0.31f, 0.22f, 1.0f), FaceTextures::none() },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::CoalOre) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::IronOre) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::GoldOre) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::DiamondOre) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::EmeraldOre) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::RedstoneOre) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::LapisOre) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::CopperOre) },
    { glm::vec4(1.0f), FaceTextures::directional(TextureIndex::BirchWoodTop, TextureIndex::BirchWoodSide, TextureIndex::BirchWoodTop) },
    { glm::vec4(0.85f, 1.0f, 0.85f, 0.90f), FaceTextures::all(TextureIndex::BirchLeaves), false, true },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::BirchPlanks) },
    { glm::vec4(1.0f), FaceTextures::directional(TextureIndex::SpruceWoodTop, TextureIndex::SpruceWoodSide, TextureIndex::SpruceWoodTop) },
    { glm::vec4(0.85f, 1.0f, 0.85f, 0.90f), FaceTextures::all(TextureIndex::SpruceLeaves), false, true },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::SprucePlanks) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Granite) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Diorite) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Andesite) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Deepslate) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Blackstone) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Basalt) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Netherrack) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::EndStone) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Obsidian) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Sandstone) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Terracotta) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Glowstone) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::SeaLantern) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::MagmaBlock) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::CraftingTable) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Furnace) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Chest) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::TNT) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Bookshelf) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Prismarine) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::MossyCobble) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::SnowBlock) }
}};

static_assert(static_cast<std::size_t>(BlockType::Air) == 0,
              "Block IDs are serialized/material-registry IDs and must remain stable");
static_assert(BLOCK_MATERIALS.size() == static_cast<std::size_t>(BlockType::Count),
              "Every block ID must have exactly one fixed material entry");
static_assert(static_cast<std::size_t>(TextureIndex::GrassTop) == 0,
              "Texture array layers are a stable GPU ABI and must remain append-only");

inline const BlockMaterial& get_block_material(BlockType type) {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index < BLOCK_MATERIALS.size()) return BLOCK_MATERIALS[index];
    static const BlockMaterial missing{ glm::vec4(1.0f, 0.0f, 1.0f, 1.0f), FaceTextures::none() };
    return missing;
}

inline std::optional<TextureIndex> get_block_texture(BlockType type, const glm::vec3& normal) {
    const FaceTextures& textures = get_block_material(type).textures;
    if (normal.y > 0.5f) return textures.top;
    if (normal.y < -0.5f) return textures.bottom;
    return textures.side;
}

inline float get_block_texture_layer(BlockType type, const glm::vec3& normal) {
    const auto texture = get_block_texture(type, normal);
    return texture ? static_cast<float>(*texture) : -1.0f;
}

inline glm::vec4 get_block_color(BlockType type, const glm::vec3&) {
    return get_block_material(type).color;
}

inline bool is_water_block(BlockType type) { return type == BlockType::Water; }
inline bool is_lava_block(BlockType type) { return type == BlockType::Lava; }
inline bool is_fluid_block(BlockType type) { return is_water_block(type) || is_lava_block(type); }
inline bool is_leaf_block(BlockType type) {
    return type == BlockType::Leaves || type == BlockType::LeavesBirch || type == BlockType::LeavesSpruce;
}
inline bool is_emissive_block(BlockType type) {
    return type == BlockType::Lava || type == BlockType::Glowstone ||
           type == BlockType::SeaLantern || type == BlockType::MagmaBlock;
}
inline bool is_transparent_block(BlockType type) { return get_block_material(type).transparent; }
inline bool is_solid_block(BlockType type) { return get_block_material(type).solid; }

constexpr uint8_t WATER_SOURCE_LEVEL = 0;
constexpr uint8_t WATER_MAX_LEVEL = 7;
constexpr uint8_t WATER_FALLING_FLAG = 0x80;
constexpr uint8_t WATER_LEVEL_NONE = 0xff;

inline uint8_t water_base_level(uint8_t level) { return level & WATER_MAX_LEVEL; }
inline bool water_is_falling(uint8_t level) { return (level & WATER_FALLING_FLAG) != 0; }
inline float water_render_height(uint8_t level) {
    if (water_is_falling(level)) return 1.0f;
    return 0.92f - static_cast<float>(water_base_level(level)) * 0.105f;
}

struct VoxelVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 color;
    glm::vec3 uv; // uv.z == -1 means color-only material.
};

struct GrassInstance {
    // xyz = base position. floor(w) is the edge mask; fract(w) is the rotation turn.
    // Mask bits: west, east, south, north. Shared grass edges remain unrestricted.
    glm::vec4 positionRotation;
};

struct FoliageInstance {
    glm::vec4 positionScale; // xyz = cluster anchor, w = card size
};
