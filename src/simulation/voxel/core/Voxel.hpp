#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

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
// FNV-1a hash used for the renderer's material variant dedup key (the same
// algorithm Engine::Rendering::IBlockMaterialResolver::variantKey applies to
// (namespaced name + state) at the headless/SDK layer). build time embeds this
// into the immutable RuntimeBlockInfo/RuntimeBlockState so the mesher and
// renderer never need the registry or the resolver instance on the worker.
inline std::uint32_t runtime_block_variant_key(const std::string& name) noexcept {
    std::uint32_t h = 2166136261u;
    for (const char c : name) {
        h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    if (h == 0) h = 1;  // 0 is reserved for "no material" in the variant contract
    return h;
}

struct RuntimeBlockInfo {
    std::string uuid;              // persistent identity (empty = builtin)
    // Renderer material dedup key at the BLOCK level (embedded at dispatch;
    // resolver-compatible). 0 = not materialized.
    std::uint32_t variantKey{ 0 };
    glm::vec4 color{ 1.0f };       // data-driven base color (no texture layer)
    // Per-face material overrides (FALTANTES §14): mirror of
    // BlockDefinition.faceTop/Bottom/Side; the mesher picks by face normal.
    glm::vec4 faceTop{ 1.0f };
    glm::vec4 faceBottom{ 1.0f };
    glm::vec4 faceSide{ 1.0f };
    bool faceTopSet{ false };
    bool faceBottomSet{ false };
    bool faceSideSet{ false };
    bool occludes{ true };         // face-culling hint (false = draw vs opaque)
    uint8_t renderLayer{ 0 };
    bool solid{ true };            // collision / raycast
    bool transparent{ false };     // render pass / culling hints
    bool fluid{ false };
    // Collision/selection shapes (FALTANTES item 2): mirror of
    // BlockDefinition.collisionShape/selectionShape. collisionShape: 0 =
    // full, 1 = cross, 2 = none (none wins over the `solid` flag in raycast
    // consumers that check the shape); selectionShape feeds the editor
    // pick-box milestone.
    uint8_t collisionShape{ 0 };
    uint8_t selectionShape{ 0 };
    // Tool/physics component (FALTANTES item 4): mirror of
    // BlockDefinition.tool/toolTier/resistance/friction/bounciness/density/
    // flammability. Consumed by the gameplay/physics milestones (tool
    // gating, destruction scaling, dynamic-body response, explosion heat);
    // mirrored here so consumers never touch the registry. tool: 0 = any,
    // 1..5 = pickaxe/axe/shovel/hoe/sword.
    uint8_t tool{ 0 };
    uint8_t toolTier{ 0 };
    float resistance{ 0.0f };
    float friction{ 0.5f };
    float bounciness{ 0.0f };
    float density{ 1.0f };
    float flammability{ 0.0f };   // 0..1 (FALTANTES §16 item 10 heat axis)
    std::string soundPlace;
    std::string soundBreak;
    std::string soundStep;
    std::string soundHit;
    std::string particleBreak;
    // Behavior component (FALTANTES item 6): namespaced behavior reference;
    // resolved by the abilities/block entity milestone.
    std::string behaviorId;
    // Discrete lighting (META section 12), 0..15. lightAbsorption is the
    // propagation cost of a cell (15 = opaque, blocks sky and block light);
    // the JSON float (0..1) is scaled by 15 when the runtime table is built.
    uint8_t lightEmission{ 0 };
    uint8_t lightAbsorption{ 15 };

    // Named states (FALTANTES item 5): mirror of BlockDefinition.states with
    // light already discretized. states[0] is the default; the mesher resolves
    // per-state material via VoxelMesher::resolve_state_material.
    struct RuntimeBlockState {
        std::string name;
        glm::vec4 color{ 1.0f };
        glm::vec4 faceTop{ 1.0f };
        glm::vec4 faceBottom{ 1.0f };
        glm::vec4 faceSide{ 1.0f };
        bool faceTopSet{ false };
        bool faceBottomSet{ false };
        bool faceSideSet{ false };
        uint8_t lightEmission{ 0 };
        // Renderer material dedup key for THIS state (embedder-computed; 0 when
        // the block declares no states — the block-level key then applies).
        std::uint32_t variantKey{ 0 };
    };
    std::vector<RuntimeBlockState> states;
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

// Appearance-only block material (FALTANTES §8 item 166): color + optional
// textures. SIMULATION flags (solidity, light absorption) do NOT live here —
// they belong to BlockSimProps / the runtime block table. A visual change can
// never alter physics, and vice versa. A block only needs a color;
// FaceTextures::none() is a fully supported material, not a fallback error.
struct BlockMaterial {
    glm::vec4 color{ 1.0f };
    FaceTextures textures{ FaceTextures::none() };
};

// Simulation-only properties of builtin blocks (FALTANTES §8 item 166): the
// physical flags live in their OWN table, never in the visual material.
// Data-driven (registry) blocks get the same properties from BlockDefinition
// via RuntimeBlockInfo instead; the two paths share no struct.
struct BlockSimProps {
    bool solid{ true };            // collision / raycast
    bool transparent{ false };     // light passes (0 absorption)
    uint8_t lightAbsorption{ 15 }; // 0..15 propagation cost
};

// Builtin simulation table: opaque solids absorb fully (15); glass is solid
// for collision but passes light (0); water/lava and leaves attenuate to 1;
// air absorbs nothing. This mirrors the historical material-table flags 1:1
// (behavior preserved, verified by test_material_simulation_separation).
inline const std::array<BlockSimProps, static_cast<std::size_t>(BlockType::Count)> BLOCK_SIM_PROPS = [] {
    std::array<BlockSimProps, static_cast<std::size_t>(BlockType::Count)> table;
    table.fill(BlockSimProps{ true, false, 15 });  // opaque solid default
    table[static_cast<std::size_t>(BlockType::Air)] = BlockSimProps{ false, true, 0 };
    table[static_cast<std::size_t>(BlockType::Leaves)] = BlockSimProps{ false, true, 1 };
    table[static_cast<std::size_t>(BlockType::Glass)] = BlockSimProps{ true, true, 0 };
    table[static_cast<std::size_t>(BlockType::Water)] = BlockSimProps{ false, true, 1 };
    table[static_cast<std::size_t>(BlockType::Lava)] = BlockSimProps{ false, false, 1 };
    table[static_cast<std::size_t>(BlockType::LeavesBirch)] = BlockSimProps{ false, true, 1 };
    table[static_cast<std::size_t>(BlockType::LeavesSpruce)] = BlockSimProps{ false, true, 1 };
    return table;
}();

inline const std::array<BlockMaterial, static_cast<std::size_t>(BlockType::Count)> BLOCK_MATERIALS{{
    { glm::vec4(0.0f), FaceTextures::none() },
    { glm::vec4(1.0f), FaceTextures::directional(TextureIndex::GrassTop, TextureIndex::GrassSide, TextureIndex::Dirt) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Dirt) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Stone) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Bedrock) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Sand) },
    { glm::vec4(1.0f), FaceTextures::directional(TextureIndex::WoodTop, TextureIndex::WoodSide, TextureIndex::WoodTop) },
    { glm::vec4(0.85f, 1.0f, 0.85f, 0.90f), FaceTextures::all(TextureIndex::Leaves) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Planks) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Cobblestone) },
    { glm::vec4(1.0f, 1.0f, 1.0f, 0.45f), FaceTextures::all(TextureIndex::Glass) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Bricks) },
    { glm::vec4(0.80f, 0.90f, 1.0f, 0.65f), FaceTextures::all(TextureIndex::Water) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::Lava) },
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
    { glm::vec4(0.85f, 1.0f, 0.85f, 0.90f), FaceTextures::all(TextureIndex::BirchLeaves) },
    { glm::vec4(1.0f), FaceTextures::all(TextureIndex::BirchPlanks) },
    { glm::vec4(1.0f), FaceTextures::directional(TextureIndex::SpruceWoodTop, TextureIndex::SpruceWoodSide, TextureIndex::SpruceWoodTop) },
    { glm::vec4(0.85f, 1.0f, 0.85f, 0.90f), FaceTextures::all(TextureIndex::SpruceLeaves) },
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
static_assert(BLOCK_SIM_PROPS.size() == static_cast<std::size_t>(BlockType::Count),
              "Every block ID must have exactly one fixed simulation entry");
static_assert(static_cast<std::size_t>(TextureIndex::GrassTop) == 0,
              "Texture array layers are a stable GPU ABI and must remain append-only");

inline const BlockMaterial& get_block_material(BlockType type) {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index < BLOCK_MATERIALS.size()) return BLOCK_MATERIALS[index];
    static const BlockMaterial missing{ glm::vec4(1.0f, 0.0f, 1.0f, 1.0f), FaceTextures::none() };
    return missing;
}

inline BlockSimProps get_block_sim(BlockType type) {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index < BLOCK_SIM_PROPS.size()) return BLOCK_SIM_PROPS[index];
    return BlockSimProps{ true, false, 15 };
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
// Simulation predicates read the SIMULATION table (FALTANTES §8 item 166) —
// never the visual material. is_solid_block drives collision/raycast,
// is_transparent_block drives light propagation (glass passes light).
inline bool is_transparent_block(BlockType type) { return get_block_sim(type).transparent; }
inline bool is_solid_block(BlockType type) { return get_block_sim(type).solid; }

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
