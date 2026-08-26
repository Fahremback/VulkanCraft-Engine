import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

export const COMPONENT_SCHEMAS = Object.freeze({
  Transform: { px: 0, py: 0, pz: 0, rx: 0, ry: 0, rz: 0, sx: 1, sy: 1, sz: 1 },
  Camera: { fov: 60, near: 0.1, far: 2000, primary: false },
  Light: { r: 1, g: 1, b: 1, intensity: 1000, range: 50, castShadows: true, type: 0 },
  MeshRenderer: { mesh: "", material: "", visible: true, castShadows: true },
  Rigidbody: { mass: 1, friction: 0.5, restitution: 0.1, kinematic: false, gravity: true },
  Weapon: { damage: 25, rpm: 600, magazine: 30, reserve: 90, automatic: true, spread: 1.5, hitscan: true },
  ParticleEmitter: {
    px: 0, py: 0, pz: 0, dx: 0, dy: 1, dz: 0, cone: 0.4, rate: 20,
    speedMin: 1, speedMax: 3, lifeMin: 0.5, lifeMax: 1.5, sizeStart: 0.12, sizeEnd: 0,
    cr: 1, cg: 1, cb: 1, ca: 1, er: 1, eg: 1, eb: 1, ea: 0,
    ax: 0, ay: -9.81, az: 0, drag: 0.05, turbulence: 0, restitution: 0.35,
    burst: 0, collide: false, emitting: true
  },
  Vehicle: {
    enginePower: 4200, maxSteer: 0.55, brake: 6000, wheelRadius: 0.36,
    suspRest: 0.45, wheelBase: 2.6, track: 1.6, mass: 1200, fwd: true, enabled: true
  },
  Ragdoll: { enabled: true, blend: 0.8, fromSkeleton: false, massPerBone: 1, ox: 0, oy: 0, oz: 0 },
  Mission: { id: "Mission", objective: "Complete the mission", target: 1, completeEvent: "MissionComplete", autoStart: true, active: false },
  Dialogue: { id: "Dialogue", character: "NPC", line: "Hello!", choice: "Continue", next: "", playOnStart: false, playing: false },
  Destruction: { csx: 0.5, csy: 0.5, csz: 0.5, chunks: 8, health: 25, radius: 3, impulse: 8, enabled: true, destroyed: false },
  Navigation: { gw: 32, gh: 32, cell: 1, speed: 3, enabled: true },
  Audio: { clip: "", volume: 1, pitch: 1, spatial: true, looping: false, playOnStart: false, playing: false },
  Material: { ar: 1, ag: 1, ab: 1, roughness: 0.5, metallic: 0, er: 0, eg: 0, eb: 0, emissiveIntensity: 0 },
  VoxelVolume: { chunkBudget: 256, seed: 1, seaLevel: 62, enableFarLod: true },
  Hierarchy: { parent_id: "" }
});

export const SCRIPT_NODE_KINDS = Object.freeze([
  "Event", "ConstantFloat", "ConstantInteger", "ConstantBoolean", "GetVariable", "SetVariable",
  "AddFloat", "SubtractFloat", "MultiplyFloat", "Branch", "Wait", "EmitEvent", "Return",
  "Function", "FunctionCall", "Log", "Scope", "ScopeEnd"
]);

// Registry asset kinds the MCP authors (FALTANTES item 23 / prioridade 5). Each
// document mirrors EXACTLY the versioned JSON the public C++ registries parse
// (BlockRegistry/ItemRegistry/FluidRegistry/RecipeRegistry in
// engine/registry/*, IBiomeRegistry + IStructureGenerator in
// engine/procgen/*), so an authored asset loads through the public factories
// unchanged (proved by tests/McpRegistryGateTests.cpp).
export const REGISTRY_KINDS = Object.freeze(["block", "item", "fluid", "recipe", "biome", "structure"]);

// Vehicle assembly asset kinds (FALTANTES §17 item 12): the MCP authors a
// vehicle (rigid wheeled chassis, `VehicleAsset`) or a beam chassis
// (node/beam deformable, `BeamGraphAsset`) as a versioned JSON document under
// Content/Vehicles/<name>.json. Each document mirrors EXACTLY the versioned
// JSON the public C++ factories parse (engine/vehicles/IVehicleAsset.hpp and
// IBeamGraphAsset.hpp, implemented by src/engine/sdk/VehicleAsset.cpp and
// BeamGraphAsset.cpp — all-or-nothing, never clamped), so an authored asset
// loads through `VehicleAsset::load_from_json` / `BeamGraphAsset::load_from_json`
// unchanged (proved by tests/VehicleAssetGateTests.cpp).
export const VEHICLE_KINDS = Object.freeze(["vehicle", "beam"]);
const VEHICLE_SHAPES = new Set(["box", "sphere", "capsule"]);
const VEHICLE_KIND_ENUM = new Set(["wheeled", "motorcycle", "tracked"]);
const PROPULSION_KINDS = new Set(["wing", "thruster", "buoyancy"]);

// Ability/powers asset kind (FALTANTES §19 — abilities data-driven): the MCP
// authors an ability (data-driven attributes/tags/cost/cooldown/conditions,
// composable effects, targeting) as a versioned JSON document under
// Content/Abilities/<name>.json. Each document mirrors EXACTLY the versioned
// JSON the public C++ factory parses (engine/gameplay/IAbilitySystem.hpp,
// implemented by src/engine/sdk/AbilitySystem.cpp — all-or-nothing, never
// clamped), so an authored asset loads through `AbilityDefinition::
// load_from_json` unchanged (proved by tests/AbilitySystemTests.cpp).
export const ABILITY_KINDS = Object.freeze(["ability"]);
const ABILITY_TARGET_MODES = new Set(["self", "direction", "point", "body"]);
const ABILITY_CONDITION_KINDS = new Set(["ownerTag", "targetTag", "ownerAttribute", "targetAttribute", "distance"]);
const ABILITY_EFFECT_TYPES = new Set(["damage", "heal", "impulse", "telekinesis", "flight", "blockEdit", "periodic"]);
const ABILITY_MAX_BLOCK_EDIT_VOLUME = 4096;

// Compact field contracts surfaced by game_capabilities so an agent can author
// an ability without reading the engine source. Single source for the exported
// JSON Schema (buildAbilityJsonSchema) and the author tool.
export const ABILITY_FIELD_SCHEMAS = Object.freeze({
  ability: [
    { name: "name", type: "string", required: true, description: "ability name (becomes the file name)" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from 'abilities:<name>' when omitted" },
    { name: "version", type: "integer", required: false, default: 1 },
    { name: "cooldownSeconds", type: "number", required: false, default: 0, description: "cooldown after a cast, >= 0" },
    { name: "cancelable", type: "boolean", required: false, default: true },
    { name: "interruptible", type: "boolean", required: false, default: true },
    { name: "attributes", type: "array[object]", required: false, default: [], description: "named numeric attributes: [{ name, value }]" },
    { name: "tags", type: "array[string]", required: false, default: [], description: "named tags ('movement', 'fire', 'ultimate') the runtime never interprets" },
    { name: "cost", type: "object", required: false, description: "resource cost spent from the caster at cast: { resource, amount >= 0 }" },
    { name: "conditions", type: "array[object]", required: false, default: [], description: "AND-ed gates: [{ kind: ownerTag|targetTag|ownerAttribute|targetAttribute|distance, tag, attribute, minValue, maxDistance }]" },
    { name: "targeting", type: "object", required: false, description: "{ mode: self|direction|point|body, range >= 0, radius >= 0 }" },
    { name: "effects", type: "array[object]", required: true, description: "composable effects applied in order: [{ type: damage|heal|impulse|telekinesis|flight|blockEdit|periodic, ... }] — see ability_effect_fields" }
  ]
});

// The authoring ARG names (snake_case) differ from the DOCUMENT keys
// (camelCase — what the public C++ factory parses). Same mapping as
// buildAbilityDocument, single source.
const ABILITY_DOC_KEYS = Object.freeze({
  ability: {
    cooldown_seconds: "cooldownSeconds",
    min_value: "minValue",
    max_distance: "maxDistance",
    hold_offset: "holdOffset",
    grab_force: "grabForce",
    duration_seconds: "durationSeconds",
    interval_seconds: "intervalSeconds",
    block_id: "blockId",
    cast_animation: "castAnimation",
    particle_effect: "particleEffect",
    sound_effect: "soundEffect"
  }
});

// Per-effect-type fields (same order the C++ parse_effect emits) so the
// capability document can describe composable effects compactly.
export const ABILITY_EFFECT_FIELDS = Object.freeze({
  damage: [{ name: "amount", type: "number", required: false, default: 0, description: "health subtracted from the target, >= 0" }],
  heal: [{ name: "amount", type: "number", required: false, default: 0, description: "health added to the target, >= 0" }],
  impulse: [{ name: "force", type: "number", required: false, default: 0, description: "impulse along the cast direction, >= 0" }],
  telekinesis: [
    { name: "holdOffset", type: "array[3]", required: false, default: [0, 1.5, 0], description: "hold offset relative to the CASTER position" },
    { name: "grabForce", type: "number", required: false, default: 240, description: "spring stiffness pulling the body to the hold point, >= 0" },
    { name: "durationSeconds", type: "number", required: false, default: 0, description: "> 0 ends the hold automatically; 0 = until cancel" }
  ],
  flight: [
    { name: "thrust", type: "number", required: false, default: 320, description: "upward thrust applied to the caster each update, >= 0" },
    { name: "durationSeconds", type: "number", required: false, default: 0, description: "> 0 ends flight automatically; 0 = until cancel" }
  ],
  blockEdit: [
    { name: "min", type: "array[3]", required: false, default: [-1, -1, -1], description: "box corner (relative or absolute) — integers" },
    { name: "max", type: "array[3]", required: false, default: [1, 1, 1], description: "box corner (relative or absolute) — integers; max >= min per axis" },
    { name: "blockId", type: "number", required: false, default: 1, description: "block to write (0 = air); box volume <= 4096 cells" },
    { name: "relative", type: "boolean", required: false, default: true, description: "min/max relative to the target point (true) or absolute world coordinates (false)" }
  ],
  periodic: [
    { name: "intervalSeconds", type: "number", required: false, default: 0.5, description: "repeat interval, > 0" },
    { name: "ticks", type: "integer", required: false, default: 4, description: "repeat count, >= 1" },
    { name: "subEffect", type: "object", required: false, description: "the repeated effect (any effect type, recursively)" }
  ]
});

// Presentation hooks every effect accepts (particles/audio/animation).
const ABILITY_EFFECT_HOOKS = ["castAnimation", "particleEffect", "soundEffect"];

// Mission/dialogue asset kind (FALTANTES item 23 "missões e diálogos"): the
// MCP authors a mission (objectives reach/collect/kill/interact, dialogue
// graph with condition-gated choices, unlock conditions, reward) as a
// versioned JSON document under Content/Missions/<name>.json. Mirrors EXACTLY
// the versioned JSON the public C++ factory parses
// (engine/gameplay/IMissionAsset.hpp, implemented by
// src/engine/sdk/MissionAsset.cpp — all-or-nothing, never clamped), so an
// authored asset loads through `MissionDefinition::load_from_json` unchanged
// (proved by tests/MissionAssetTests.cpp).
export const MISSION_KINDS = Object.freeze(["mission"]);
const MISSION_CONDITION_KINDS = new Set(["flag", "counter", "objectiveDone", "attribute"]);
const MISSION_OBJECTIVE_KINDS = new Set(["reach", "collect", "kill", "interact"]);
const MISSION_OPS = new Set(["==", "!=", ">=", "<=", ">", "<"]);

// Compact field contracts surfaced by game_capabilities so an agent can author
// a mission without reading the engine source. Single source for the exported
// JSON Schema (buildMissionJsonSchema) and the author tool.
export const MISSION_FIELD_SCHEMAS = Object.freeze({
  mission: [
    { name: "name", type: "string", required: true, description: "mission name (becomes the file name)" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from 'missions:<name>' when omitted" },
    { name: "version", type: "integer", required: false, default: 1 },
    { name: "objectives", type: "array[object]", required: true, description: "ALL must complete: [{ id, kind: reach|collect|kill|interact, target, count >= 1, x, z, radius >= 0, conditions }]" },
    { name: "dialogue", type: "array[object]", required: false, default: [], description: "dialogue graph: [{ id (must include 'start'), speaker, text, choices: [{ text, next, conditions }] }]" },
    { name: "unlockConditions", type: "array[object]", required: false, default: [], description: "ALL must pass to accept: [{ kind: flag|counter|objectiveDone|attribute, key, op: ==|!=|>=|<=|>|<, value, flagValue }]" },
    { name: "reward", type: "object", required: false, description: "{ itemId, count >= 0, xp >= 0, setFlag } applied on completion" },
    { name: "repeatable", type: "boolean", required: false, default: false }
  ]
});

// Per-kind contract shapes surfaced in the capability document (objective and
// condition field contracts).
export const MISSION_OBJECTIVE_FIELDS = Object.freeze({
  reach: [{ name: "x", type: "number", required: false, default: 0 }, { name: "z", type: "number", required: false, default: 0 }, { name: "radius", type: "number", required: false, default: 0, description: ">= 0; distance within which the reach completes" }],
  collect: [{ name: "target", type: "string", required: true, description: "item id the world counts" }, { name: "count", type: "integer", required: false, default: 1, description: ">= 1" }],
  kill: [{ name: "target", type: "string", required: true, description: "entity id the world counts" }, { name: "count", type: "integer", required: false, default: 1, description: ">= 1" }],
  interact: [{ name: "target", type: "string", required: true, description: "interaction id the world counts" }, { name: "count", type: "integer", required: false, default: 1, description: ">= 1" }]
});

const MISSION_CONDITION_FIELDS = [
  { name: "kind", type: "enum", values: [...MISSION_CONDITION_KINDS], required: true },
  { name: "key", type: "string", required: true, description: "world key (flag/counter/attribute) or objective id (objectiveDone)" },
  { name: "op", type: "enum", values: [...MISSION_OPS], required: false, default: ">=", description: "counter/attribute comparison" },
  { name: "value", type: "number", required: false, default: 0, description: "counter/attribute threshold" },
  { name: "flagValue", type: "boolean", required: false, default: true }
];

// Compact field contracts surfaced by game_capabilities so an agent can author
// a vehicle assembly without reading the engine source. Single source for the
// exported JSON Schema (buildVehicleJsonSchema) and the author tool.
export const VEHICLE_FIELD_SCHEMAS = Object.freeze({
  vehicle: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from 'vehicles:<name>' when omitted" },
    { name: "version", type: "integer", required: false, default: 1 },
    { name: "provider", type: "enum", values: ["jolt", "chrono", "jsbsim"], required: false, default: "jolt", description: "physics provider (item 5/6): only jolt is vendored — chrono/jsbsim are refused at creation (never a silent fallback)" },
    { name: "kind", type: "enum", values: [...VEHICLE_KIND_ENUM], required: false, default: "wheeled", description: "Jolt controller family: car / motorcycle / tracked" },
    { name: "position", type: "array[3]", required: false, default: [0, 0, 0], description: "assembly (spawn) position of the chassis body" },
    { name: "rotation", type: "array[4]", required: false, default: [0, 0, 0, 1], description: "assembly rotation quaternion [x, y, z, w]" },
    { name: "chassis", type: "object", required: true, description: "{ shape: box|sphere|capsule, halfExtents: [x,y,z], radius, halfHeight, mass > 0, friction >= 0, restitution >= 0 }" },
    { name: "wheels", type: "array[object]", required: true, description: "[{ localPosition: [x,y,z], radius > 0, suspensionRestLength > 0, suspensionTravel >= 0, springStrength >= 0, damperStrength >= 0, tireGrip >= 0, maxDriveForce >= 0, maxBrakeForce >= 0, maxSteerAngle >= 0, steering: bool, driven: bool }]" },
    { name: "drivetrain", type: "object", required: true, description: "{ engineMaxTorque >= 0, engineMinRPM > 0, engineMaxRPM > engineMinRPM, differentialRatio > 0, gearRatios: [> 0] }" },
    { name: "propulsion", type: "array[object]", required: false, default: [], description: "[{ kind: wing|thruster|buoyancy, localPosition, axis (non-zero), maxForce, area, liftCoefficient, fluidDensity, waterLevel }]" },
    { name: "seats", type: "array[object]", required: false, default: [], description: "occupant seats (item 8): [{ name, localPosition, exitOffset }]" },
    { name: "power", type: "object", required: false, description: "fuel/energy/controls (item 7): { fuel?: { capacity, initialLevel, burnPerSecond, idleBurnPerSecond, minLevelToRun }, energy?: { capacity, initialCharge, drawPerSecond, regenPerSecond, minChargeToRun }, controls?: { throttleDeadzone, throttleSensitivity, throttleInvert, steeringDeadzone, steeringSensitivity, steeringInvert, brakeDeadzone, brakeSensitivity } }" }
  ],
  beam: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from 'beams:<name>' when omitted" },
    { name: "version", type: "integer", required: false, default: 1 },
    { name: "provider", type: "enum", values: ["jolt", "chrono", "jsbsim"], required: false, default: "jolt", description: "physics provider (item 5/6): only jolt is vendored — chrono/jsbsim are refused at creation (never a silent fallback)" },
    { name: "position", type: "array[3]", required: false, default: [0, 0, 0] },
    { name: "rotation", type: "array[4]", required: false, default: [0, 0, 0, 1] },
    { name: "mass", type: "number", required: false, default: 1200, description: "total chassis mass (kg), > 0" },
    { name: "nodes", type: "array[object]", required: true, description: "mass points: [{ position: [x,y,z], fixed: bool }] (1..4096)" },
    { name: "beams", type: "array[object]", required: true, description: "distance constraints: [{ a: int, b: int (valid node indices), stiffness in (0, 1] }]" },
    { name: "wheels", type: "array[object]", required: false, default: [], description: "wheel mounts: [{ node: int, steering: bool, driven: bool, wheel: { localPosition, radius, suspensionRestLength, suspensionTravel, springStrength, damperStrength, tireGrip, maxDriveForce, maxBrakeForce, maxSteerAngle, steering, driven } }]" },
    { name: "seats", type: "array[object]", required: false, default: [], description: "occupant seats (item 8)" },
    { name: "power", type: "object", required: false, description: "fuel/energy/controls (item 7)" },
    { name: "solver", type: "object", required: false, description: "XPBD solver config: { substeps: 1..16, solverIterations: 1..64, stiffness in (0, 1], damping in [0, 1), gravity: [x,y,z] (magnitude <= 1000) }" }
  ]
});

// The authoring ARG names (snake_case) differ from the DOCUMENT keys
// (camelCase — what the public C++ factories parse). Same mapping as
// buildVehicleDocument, single source.
const VEHICLE_DOC_KEYS = Object.freeze({
  vehicle: {
    half_extents: "halfExtents", half_height: "halfHeight",
    suspension_rest_length: "suspensionRestLength", suspension_travel: "suspensionTravel",
    spring_strength: "springStrength", damper_strength: "damperStrength",
    tire_grip: "tireGrip", max_drive_force: "maxDriveForce", max_brake_force: "maxBrakeForce",
    max_steer_angle: "maxSteerAngle", engine_max_torque: "engineMaxTorque",
    engine_min_rpm: "engineMinRPM", engine_max_rpm: "engineMaxRPM",
    differential_ratio: "differentialRatio", gear_ratios: "gearRatios",
    local_position: "localPosition", max_force: "maxForce",
    lift_coefficient: "liftCoefficient", fluid_density: "fluidDensity",
    water_level: "waterLevel", exit_offset: "exitOffset",
    initial_level: "initialLevel", burn_per_second: "burnPerSecond",
    idle_burn_per_second: "idleBurnPerSecond", min_level_to_run: "minLevelToRun",
    initial_charge: "initialCharge", draw_per_second: "drawPerSecond",
    regen_per_second: "regenPerSecond", min_charge_to_run: "minChargeToRun",
    throttle_deadzone: "throttleDeadzone", throttle_sensitivity: "throttleSensitivity",
    throttle_invert: "throttleInvert", steering_deadzone: "steeringDeadzone",
    steering_sensitivity: "steeringSensitivity", steering_invert: "steeringInvert",
    brake_deadzone: "brakeDeadzone", brake_sensitivity: "brakeSensitivity"
  },
  beam: {}
});

// Mirrors BlockType::Count in src/simulation/voxel/core/Voxel.hpp (51).
const MAX_BUILTIN_BLOCK_ID = 50;
const BLOCK_CLASSES = new Set(["solid", "transparent", "nonsolid", "non_solid", "fluid"]);

// Engine baked-in block names (FALTANTES item 9 cross-reference validation).
// Mirrors the kBuiltinBlocks table in src/engine/sdk/BlockRegistry.cpp — a
// stable public engine contract: a fluid may drive any of these blocks even
// when the project does not author it.
const ENGINE_BUILTIN_BLOCKS = new Set([
  "vulkancraft:air", "vulkancraft:grass", "vulkancraft:dirt", "vulkancraft:stone", "vulkancraft:bedrock",
  "vulkancraft:sand", "vulkancraft:wood", "vulkancraft:leaves", "vulkancraft:planks", "vulkancraft:cobblestone",
  "vulkancraft:glass", "vulkancraft:bricks", "vulkancraft:water", "vulkancraft:lava", "vulkancraft:clay",
  "vulkancraft:coal_ore", "vulkancraft:iron_ore", "vulkancraft:gold_ore", "vulkancraft:diamond_ore", "vulkancraft:emerald_ore",
  "vulkancraft:redstone_ore", "vulkancraft:lapis_ore", "vulkancraft:copper_ore", "vulkancraft:birch_wood", "vulkancraft:birch_leaves",
  "vulkancraft:birch_planks", "vulkancraft:spruce_wood", "vulkancraft:spruce_leaves", "vulkancraft:spruce_planks", "vulkancraft:granite",
  "vulkancraft:diorite", "vulkancraft:andesite", "vulkancraft:deepslate", "vulkancraft:blackstone", "vulkancraft:basalt",
  "vulkancraft:netherrack", "vulkancraft:end_stone", "vulkancraft:obsidian", "vulkancraft:sandstone", "vulkancraft:terracotta",
  "vulkancraft:glowstone", "vulkancraft:sea_lantern", "vulkancraft:magma_block", "vulkancraft:crafting_table", "vulkancraft:furnace",
  "vulkancraft:chest", "vulkancraft:tnt", "vulkancraft:bookshelf", "vulkancraft:prismarine", "vulkancraft:mossy_cobblestone",
  "vulkancraft:snow_block"
]);
const CLIMATE_AXES = Object.freeze(["temperature", "moisture", "continentalness", "erosion", "weirdness", "river"]);
const STRUCTURE_SYMMETRIES = new Set([1, 2, 4, 8]);

// Compact field contracts surfaced by game_capabilities so an agent can author
// a registry asset without reading the engine source.
export const REGISTRY_FIELD_SCHEMAS = Object.freeze({
  block: [
    { name: "name", type: "string", required: true, description: "short block name (e.g. titanium)" },
    { name: "namespace", type: "string", required: false, default: "vulkancraft", description: "namespaced prefix" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from ns:name when omitted" },
    { name: "class", type: "enum", values: [...BLOCK_CLASSES], required: false, default: "solid" },
    { name: "hardness", type: "number", required: false, default: 1.0 },
    { name: "light_emission", type: "number", required: false, default: 0.0 },
    { name: "light_absorption", type: "number", required: false, default: 1.0 },
    { name: "opaque", type: "boolean", required: false, default: true },
    { name: "collidable", type: "boolean", required: false, default: true },
    { name: "collision_shape", type: "enum", values: ["full", "cross", "none"], required: false, default: "full", description: "collision shape (item 2): none = not solid, cross = thin hitbox (physics milestone); none wins over collidable:true" },
    { name: "selection_shape", type: "enum", values: ["full", "cross", "none"], required: false, default: "full", description: "selection/pick-box shape (item 2): feeds the editor milestone" },
    { name: "builtin_id", type: "integer", required: false, description: `unsupported today: the builtin table occupies every id in 0..${MAX_BUILTIN_BLOCK_ID}, so any declared mapping is refused; author catalog-only blocks (omit builtin_id)` },
    { name: "color", type: "array[3..4]", required: false, description: "[r, g, b(, a)] 0..1 — base color (also the fallback for unset faces)" },
    { name: "face_top", type: "array[3..4]", required: false, description: "[r, g, b(, a)] 0..1 — color of the +Y face (overrides color)" },
    { name: "face_bottom", type: "array[3..4]", required: false, description: "[r, g, b(, a)] 0..1 — color of the -Y face (overrides color)" },
    { name: "face_side", type: "array[3..4]", required: false, description: "[r, g, b(, a)] 0..1 — color of horizontal faces (overrides color)" },
    { name: "occlusion", type: "boolean", required: false, default: true, description: "false draws faces even against opaque neighbors (fences/plants)" },
    { name: "render_layer", type: "integer", required: false, default: 0, description: "render layer hint in [0, 255]" },
    { name: "states", type: "array[object]", required: false, default: [], description: "named states (FALTANTES item 5): [{ name, color, face_top?, face_side?, face_bottom?, light_emission? }]; states[0] = default state" },
    { name: "transitions", type: "array[object]", required: false, default: [], description: "versioned transition rules (FALTANTES item 5): [{ from, to, trigger }]; \"\" = default state" },
    { name: "fluid", type: "object", required: false, description: "inline fluid binding (FALTANTES item 7): the block drives this fluid directly, no separate fluid asset needed — { viscosity, density, range, tick_interval, source, falling, evaporation, damage_per_tick, compressible }" },
    { name: "sound_place", type: "string", required: false, description: "namespaced sound id for placing the block (item 4)" },
    { name: "sound_break", type: "string", required: false, description: "namespaced sound id for breaking the block (item 4)" },
    { name: "sound_step", type: "string", required: false, description: "namespaced sound id for stepping on the block (item 4)" },
    { name: "sound_hit", type: "string", required: false, description: "namespaced sound id for hitting the block (item 4)" },
    { name: "particle_break", type: "string", required: false, description: "namespaced particle id emitted when the block breaks (item 4)" },
    { name: "tool", type: "string", required: false, default: "any", description: "required tool class (item 4): any|pickaxe|axe|shovel|hoe|sword" },
    { name: "tool_tier", type: "integer", required: false, default: 0, description: "required mining tier in [0, 4] (item 4)" },
    { name: "resistance", type: "number", required: false, default: 0, description: "explosion/destruction resistance, >= 0 (item 4)" },
    { name: "friction", type: "number", required: false, default: 0.5, description: "physics friction in 0..1 (item 4)" },
    { name: "bounciness", type: "number", required: false, default: 0, description: "physics restitution in 0..1 (item 4)" },
    { name: "density", type: "number", required: false, default: 1, description: "physics density, > 0 (item 4)" },
    { name: "flammability", type: "number", required: false, default: 0, description: "heat/explosion ignition axis in 0..1 — explosions burn blocks with flammability > 0 (item 10)" },
    { name: "behavior", type: "string", required: false, description: "declarative behavior reference (item 6): namespaced id (ns:name) or empty" },
    { name: "tags", type: "array[string]", required: false, default: [] },
    { name: "drops", type: "array[string]", required: false, default: ["vulkancraft:<name>"] },
    { name: "version", type: "integer", required: false, default: 1 }
  ],
  item: [
    { name: "name", type: "string", required: true },
    { name: "namespace", type: "string", required: false, default: "vulkancraft" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from ns:name when omitted" },
    { name: "max_stack", type: "integer", required: false, default: 64 },
    { name: "durability", type: "integer", required: false, default: 0 },
    { name: "icon", type: "string", required: false, default: "" },
    { name: "model", type: "string", required: false, default: "" },
    { name: "use_cooldown", type: "integer", required: false, default: 0, description: "use cooldown in ms [0, 60000] (item 8)" },
    { name: "use_mode", type: "enum", values: ["none", "instant", "continuous"], required: false, default: "none", description: "use mode (item 8)" },
    { name: "equip_slot", type: "enum", values: ["none", "hand", "offhand", "head", "chest", "legs", "feet"], required: false, default: "none", description: "equipment slot (item 8)" },
    { name: "attack_damage", type: "number", required: false, default: 0, description: "melee attack damage [0, 100] (item 8)" },
    { name: "armor", type: "number", required: false, default: 0, description: "armor value [0, 100] (item 8)" },
    { name: "behavior", type: "string", required: false, description: "declarative behavior reference (item 8): namespaced id (ns:name) or empty" },
    { name: "tags", type: "array[string]", required: false, default: [] },
    { name: "version", type: "integer", required: false, default: 1 }
  ],
  fluid: [
    { name: "block", type: "string", required: true, description: "namespaced block the fluid drives, e.g. vulkancraft:sludge" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from block when omitted" },
    { name: "viscosity", type: "number", required: false, default: 0.5, description: "0..1 rheology hint" },
    { name: "density", type: "number", required: false, default: 1.0 },
    { name: "range", type: "integer", required: false, default: 7, description: "horizontal spread budget 1..7" },
    { name: "tick_interval", type: "number", required: false, default: 0.08 },
    { name: "source", type: "boolean", required: false, default: true },
    { name: "falling", type: "boolean", required: false, default: true },
    { name: "evaporation", type: "boolean", required: false, default: true },
    { name: "damage_per_tick", type: "number", required: false, default: 0.0 },
    { name: "compressible", type: "boolean", required: false, default: false },
    { name: "color", type: "array[3..4]", required: false, description: "[r, g, b(, a)] 0..1" },
    { name: "version", type: "integer", required: false, default: 1 }
  ],
  recipe: [
    { name: "name", type: "string", required: true },
    { name: "namespace", type: "string", required: false, default: "vulkancraft" },
    { name: "id", type: "string", required: false, description: "persistent UUID; derived from ns:name when omitted" },
    { name: "station", type: "string", required: false, description: "namespaced crafting station (ns:name)" },
    { name: "time", type: "number", required: false, default: 1.0 },
    { name: "energy", type: "number", required: false, default: 0.0 },
    { name: "fuel", type: "string", required: false, description: "namespaced fuel item" },
    { name: "conditions", type: "array[string]", required: false, default: [] },
    { name: "tags", type: "array[string]", required: false, default: [] },
    { name: "inputs", type: "array", required: true, description: "[{item|tag, count>=1, alternatives: [ns:item, ...]}]" },
    { name: "outputs", type: "array", required: true, description: "[{item, count>=1, chance in (0,1]}]" },
    { name: "byproducts", type: "array", required: false, description: "same shape as outputs" },
    { name: "version", type: "integer", required: false, default: 1 }
  ],
  biome: [
    { name: "name", type: "string", required: false, description: "asset file name (e.g. my_biomes); carried by the file name, NOT a document property" },
    { name: "biomes", type: "array", required: true, description: "[{name, engine_biome_index 0..255, climate, surface}]" },
    { name: "version", type: "integer", required: false, default: 1 }
  ],
  structure: [
    { name: "name", type: "string", required: false, description: "asset file name; carried by the file name, NOT a document property" },
    { name: "sample_width", type: "integer", required: true, description: ">= 1" },
    { name: "sample_height", type: "integer", required: true, description: ">= 1" },
    { name: "sample", type: "array[integer]", required: true, description: "block ids, row-major (z * w + x), length == sample_width * sample_height" },
    { name: "pattern_size", type: "integer", required: false, default: 3, description: "1..min(sample_width, sample_height)" },
    { name: "symmetry", type: "integer", required: false, default: 1, description: "1, 2, 4 or 8" },
    { name: "periodic_output", type: "boolean", required: false, default: false },
    { name: "ground", type: "boolean", required: false, default: false },
    { name: "seed", type: "integer", required: false, default: 0 },
    { name: "profiles", type: "array", required: false, description: "[{block_id != 0, layers: [non-empty]}]" },
    { name: "version", type: "integer", required: false, default: 1 }
  ]
});

// The authoring ARG names (snake_case, what an agent passes to
// author_registry_asset) differ from the DOCUMENT keys (camelCase, what the
// public C++ factories parse and what lands in Content/Registry/*.json). The
// exported JSON Schema validates DOCUMENTS, so its property names are the C++
// keys — single source: buildRegistryDocument (same mapping as here).
const DOC_KEYS = Object.freeze({
  block: {
    collision_shape: "collisionShape", selection_shape: "selectionShape",
    light_emission: "lightEmission", light_absorption: "lightAbsorption",
    render_layer: "renderLayer", builtin_id: "builtinId",
    face_top: "faceTop", face_bottom: "faceBottom", face_side: "faceSide",
    sound_place: "soundPlace", sound_break: "soundBreak", sound_step: "soundStep",
    sound_hit: "soundHit", particle_break: "particleBreak", tool_tier: "toolTier",
    behavior: "behaviorId"
  },
  item: {
    max_stack: "maxStack", use_cooldown: "useCooldown", use_mode: "useMode",
    equip_slot: "equipSlot", attack_damage: "attackDamage", behavior: "behaviorId"
  },
  fluid: { tick_interval: "tickInterval", damage_per_tick: "damagePerTick" },
  recipe: {},
  biome: {},
  structure: {
    sample_width: "sampleWidth", sample_height: "sampleHeight",
    pattern_size: "patternSize", periodic_output: "periodicOutput"
  }
});

// JSON Schema (draft-07) for one registry kind, generated from the SAME
// REGISTRY_FIELD_SCHEMAS contracts the MCP authors against — a single source
// of truth so the exported schema can never drift from what the mirror
// validation accepts. This is the artifact the editor/IDE (intellisense),
// scripting and CI consume (FALTANTES item 10); the C++ factories ignore
// unknown fields, so additionalProperties stays permissive.
export function buildRegistryJsonSchema(kind) {
  const fields = REGISTRY_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported registry kind '${kind}'`);
  const docKeys = DOC_KEYS[kind] ?? {};
  const properties = {};
  const required = [];
  for (const field of fields) {
    const docKey = docKeys[field.name] ?? field.name;
    let schema = { description: field.description ?? `"${field.name}" registry field` };
    if (field.default !== undefined) schema.default = field.default;
    switch (field.type) {
      case "string": schema.type = "string"; break;
      case "boolean": schema.type = "boolean"; break;
      case "number": schema.type = "number"; break;
      case "integer": schema.type = "integer"; break;
      case "enum": schema.enum = [...field.values]; break;
      case "object": schema.type = "object"; break;
      case "array": schema.type = "array"; break;
      case "array[string]": schema.type = "array"; schema.items = { type: "string" }; break;
      case "array[integer]": schema.type = "array"; schema.items = { type: "integer" }; break;
      case "array[boolean]": schema.type = "array"; schema.items = { type: "boolean" }; break;
      case "array[object]": schema.type = "array"; schema.items = { type: "object" }; break;
      case "array[3..4]":
        schema.type = "array";
        schema.minItems = 3;
        schema.maxItems = 4;
        schema.items = { type: "number" };
        break;
      default: throw new Error(`unknown field type '${field.type}' in ${kind} schema`);
    }
    properties[docKey] = schema;
    if (field.required) required.push(docKey);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/registry/${kind}.json`,
    title: `${kind} registry asset`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

// JSON Schema (draft-07) for one vehicle-assembly kind, generated from the
// SAME VEHICLE_FIELD_SCHEMAS contracts the author tool mirrors against — the
// artifact the editor/IDE, scripting and CI consume (FALTANTES §17 item 12).
// The C++ factories ignore unknown fields, so additionalProperties stays
// permissive; required/type/enum mirror the all-or-nothing validation.
export function buildVehicleJsonSchema(kind) {
  const fields = VEHICLE_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported vehicle kind '${kind}'`);
  const docKeys = VEHICLE_DOC_KEYS[kind] ?? {};
  const properties = {};
  const required = [];
  for (const field of fields) {
    const docKey = docKeys[field.name] ?? field.name;
    let schema = { description: field.description ?? `"${field.name}" vehicle field` };
    if (field.default !== undefined) schema.default = field.default;
    switch (field.type) {
      case "string": schema.type = "string"; break;
      case "boolean": schema.type = "boolean"; break;
      case "number": schema.type = "number"; break;
      case "integer": schema.type = "integer"; break;
      case "enum": schema.enum = [...field.values]; break;
      case "object": schema.type = "object"; break;
      case "array": schema.type = "array"; break;
      case "array[object]": schema.type = "array"; schema.items = { type: "object" }; break;
      case "array[3]": schema.type = "array"; schema.minItems = 3; schema.maxItems = 3; schema.items = { type: "number" }; break;
      case "array[4]": schema.type = "array"; schema.minItems = 4; schema.maxItems = 4; schema.items = { type: "number" }; break;
      default: throw new Error(`unknown vehicle field type '${field.type}' in ${kind} schema`);
    }
    properties[docKey] = schema;
    if (field.required) required.push(docKey);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/vehicles/${kind}.json`,
    title: `${kind} vehicle assembly`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

export function buildAbilityJsonSchema(kind) {
  const fields = ABILITY_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported ability kind '${kind}'`);
  const docKeys = ABILITY_DOC_KEYS[kind] ?? {};
  const properties = {};
  const required = [];
  for (const field of fields) {
    const docKey = docKeys[field.name] ?? field.name;
    let schema = { description: field.description ?? `"${field.name}" ability field` };
    if (field.default !== undefined) schema.default = field.default;
    switch (field.type) {
      case "string": schema.type = "string"; break;
      case "boolean": schema.type = "boolean"; break;
      case "number": schema.type = "number"; break;
      case "integer": schema.type = "integer"; break;
      case "enum": schema.enum = [...field.values]; break;
      case "object": schema.type = "object"; break;
      case "array": schema.type = "array"; break;
      case "array[object]": schema.type = "array"; schema.items = { type: "object" }; break;
      case "array[string]": schema.type = "array"; schema.items = { type: "string" }; break;
      case "array[3]": schema.type = "array"; schema.minItems = 3; schema.maxItems = 3; schema.items = { type: "number" }; break;
      case "array[4]": schema.type = "array"; schema.minItems = 4; schema.maxItems = 4; schema.items = { type: "number" }; break;
      default: throw new Error(`unknown ability field type '${field.type}' in ${kind} schema`);
    }
    properties[docKey] = schema;
    if (field.required) required.push(docKey);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/abilities/${kind}.json`,
    title: `${kind} ability`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

const PROJECT_NAME = /^[A-Za-z][A-Za-z0-9_-]{0,63}$/;
const PROFILE_NAMES = new Set(["Debug", "Development", "Shipping", "Server", "Editor"]);
const PLATFORM_NAMES = new Set(["windows-x64", "linux-x64", "macos-universal", "dedicated-server"]);

function uuid() {
  return crypto.randomUUID();
}

function ensureProjectName(name) {
  if (typeof name !== "string" || !PROJECT_NAME.test(name)) {
    throw new Error("project must match ^[A-Za-z][A-Za-z0-9_-]{0,63}$");
  }
  return name;
}

function ensureSceneName(name) {
  if (typeof name !== "string" || !/^[A-Za-z][A-Za-z0-9 _-]{0,63}$/.test(name)) {
    throw new Error("scene name contains unsupported characters");
  }
  return name;
}

function atomicWriteJson(filePath, value) {
  atomicWrite(filePath, `${JSON.stringify(value, null, 2)}\n`);
}

function atomicWrite(filePath, content) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  const temp = path.join(path.dirname(filePath), `.${path.basename(filePath)}.${process.pid}.${uuid()}.tmp`);
  fs.writeFileSync(temp, content, "utf8");
  try {
    fs.renameSync(temp, filePath);
  } catch (error) {
    try { fs.rmSync(temp, { force: true }); } catch {}
    throw error;
  }
}

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

function sha256File(filePath) {
  return crypto.createHash("sha256").update(fs.readFileSync(filePath)).digest("hex");
}

function projectPaths(engineRoot, projectName) {
  const project = ensureProjectName(projectName);
  const root = path.join(engineRoot, "Projects", project);
  return {
    project,
    root,
    manifest: path.join(root, "project.json"),
    config: path.join(root, "ProjectConfig.json"),
    content: path.join(root, "Content"),
    scenes: path.join(root, "Content", "Scenes"),
    scripts: path.join(root, "Content", "Scripts"),
    assets: path.join(root, "Assets"),
    materials: path.join(root, "Content", "Materials"),
    audioEvents: path.join(root, "Content", "AudioEvents"),
    physicsMaterials: path.join(root, "Content", "PhysicsMaterials"),
    registry: path.join(root, "Content", "Registry"),
    vehicles: path.join(root, "Content", "Vehicles"),
    abilities: path.join(root, "Content", "Abilities")
  };
}

function requireProject(engineRoot, projectName) {
  const result = projectPaths(engineRoot, projectName);
  if (!fs.existsSync(result.manifest)) throw new Error(`project '${result.project}' does not exist`);
  return result;
}

function scenePath(engineRoot, projectName, sceneName) {
  const project = requireProject(engineRoot, projectName);
  const scene = ensureSceneName(sceneName);
  return { project, scene, file: path.join(project.scenes, `${scene}.scene`) };
}

function requireScene(engineRoot, projectName, sceneName) {
  const resolved = scenePath(engineRoot, projectName, sceneName);
  if (!fs.existsSync(resolved.file)) throw new Error(`scene '${sceneName}' does not exist in project '${projectName}'`);
  const document = readJson(resolved.file);
  if (document.format !== "VulkanEngine.Scene" || document.version !== 1 || !Array.isArray(document.entities)) {
    throw new Error("unsupported or invalid VulkanEngine scene");
  }
  return { ...resolved, document };
}

function writeProjectConfig(filePath, { name, version, initialScene, profile, platform, plugins }) {
  const lines = [
    "VCPROJECT 1",
    `name ${name}`,
    `version ${version}`,
    `initialScene ${initialScene}`,
    `profile ${profile}`,
    `platform ${platform}`,
    `plugins ${plugins.length}`,
    ...plugins.map((plugin) => `  plugin ${plugin}`),
    ""
  ];
  atomicWrite(filePath, lines.join("\n"));
}

function createEmptyScene(name) {
  return {
    format: "VulkanEngine.Scene",
    version: 1,
    scene_id: uuid(),
    name,
    entities: []
  };
}

function createStarterEntities(document) {
  const cameraId = uuid();
  const sunId = uuid();
  document.entities.push({
    id: cameraId,
    name: "Main Camera",
    Transform: { ...COMPONENT_SCHEMAS.Transform, py: 2, pz: 5 },
    Camera: { ...COMPONENT_SCHEMAS.Camera, fov: 70, primary: true }
  });
  document.entities.push({
    id: sunId,
    name: "Sun",
    Transform: { ...COMPONENT_SCHEMAS.Transform, rx: -45, ry: 35 },
    Light: { ...COMPONENT_SCHEMAS.Light, r: 1, g: 0.95, b: 0.85, intensity: 10000, range: 1000, castShadows: true, type: 0 }
  });
}

function capabilityDocument() {
  return {
    schema_version: 1,
    purpose: "Author games through stable engine assets without modifying engine source code.",
    portable_project_format: true,
    supported_operations: [
      "create/list/inspect projects", "create/inspect scenes", "create/remove entities",
      "add/update/remove components", "author visual scripts", "stage source assets", "validate projects"
      , "author materials", "author audio events", "author physics materials",
      "author registry assets (blocks/items/fluids/recipes/biomes/structures)",
      "inspect registry assets", "dry-run registry asset updates",
      "author vehicle assemblies (rigid VehicleAsset / beam BeamGraphAsset)",
      "inspect vehicle assemblies", "dry-run vehicle assembly updates",
      "author ability assets (data-driven abilities: attributes/tags/cost/cooldown/conditions, composable effects)",
      "inspect ability assets", "dry-run ability asset updates"
    ],
    components: COMPONENT_SCHEMAS,
    light_types: { Directional: 0, Point: 1, Spot: 2, Area: 3 },
    script_node_kinds: SCRIPT_NODE_KINDS,
    registry_asset_kinds: REGISTRY_KINDS.map((kind) => ({
      kind,
      file: `Content/Registry/${kind}/<name>.json`,
      fields: REGISTRY_FIELD_SCHEMAS[kind]
    })),
    // Vehicle assembly assets (FALTANTES §17 item 12): rigid VehicleAsset
    // (chassis/wheels/drivetrain/propulsion) and beam BeamGraphAsset
    // (nodes/beams/wheel mounts) under Content/Vehicles/<name>.json.
    vehicle_asset_kinds: VEHICLE_KINDS.map((kind) => ({
      kind,
      file: `Content/Vehicles/<name>.json`,
      fields: VEHICLE_FIELD_SCHEMAS[kind]
    })),
    vehicle_schemas: Object.fromEntries(
      VEHICLE_KINDS.map((kind) => [kind, buildVehicleJsonSchema(kind)])
    ),
    vehicle_validation: {
      note: "Each document mirrors exactly the versioned JSON the public C++ factories parse (VehicleAsset::load_from_json / BeamGraphAsset::load_from_json, implemented by src/engine/sdk/VehicleAsset.cpp and BeamGraphAsset.cpp); the MCP validates structure only — the runtime validates the same document again on load (all-or-nothing).",
      dry_run: "author_vehicle_asset accepts dry_run: true to validate and preview the document/diff without writing.",
      rollback: "Updates return the previous document; re-authoring it with update: true restores the prior state."
    },
    // Ability assets (FALTANTES §19 — abilities data-driven): data-driven
    // abilities with attributes/tags/cost/cooldown/conditions, composable
    // effects and targeting under Content/Abilities/<name>.json.
    ability_asset_kinds: ABILITY_KINDS.map((kind) => ({
      kind,
      file: `Content/Abilities/<name>.json`,
      fields: ABILITY_FIELD_SCHEMAS[kind],
      effect_fields: ABILITY_EFFECT_FIELDS
    })),
    ability_schemas: Object.fromEntries(
      ABILITY_KINDS.map((kind) => [kind, buildAbilityJsonSchema(kind)])
    ),
    ability_validation: {
      note: "Each document mirrors exactly the versioned JSON the public C++ factory parses (AbilityDefinition::load_from_json, implemented by src/engine/sdk/AbilitySystem.cpp); the MCP validates structure only — the runtime validates the same document again on load (all-or-nothing).",
      dry_run: "author_ability_asset accepts dry_run: true to validate and preview the document/diff without writing.",
      rollback: "Updates return the previous document; re-authoring it with update: true restores the prior state."
    },
    // Full JSON Schema (draft-07) per registry kind (FALTANTES item 10): the
    // editor/IDE, scripting and CI validate or auto-complete assets against
    // these; they are generated from REGISTRY_FIELD_SCHEMAS, the same
    // contracts the author tool mirrors.
    registry_schemas: Object.fromEntries(
      REGISTRY_KINDS.map((kind) => [kind, buildRegistryJsonSchema(kind)])
    ),
    registry_validation: {
      note: "Each document mirrors exactly the versioned JSON the public C++ factories parse (BlockRegistry/ItemRegistry/FluidRegistry/RecipeRegistry, IBiomeRegistry, IStructureGenerator); the MCP validates structure only — item/tag references in recipes are validated by the C++ RecipeRegistry against a registered ItemRegistry.",
      dry_run: "author_registry_asset accepts dry_run: true to validate and preview the document/diff without writing.",
      rollback: "Updates return the previous document; re-authoring it with update: true restores the prior state."
    },
    engine_source_modification_required: false
  };
}

export function semanticToolDefinitions() {
  return [
    {
      name: "game_capabilities",
      description: "Return the stable game-authoring capabilities, component schemas, light types, and visual-script node kinds exposed by the engine.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "list_game_projects",
      description: "List portable game projects authored under engine/Projects.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "create_game_project",
      description: "Create a portable game project using engine-owned scene/component/script formats, without modifying engine source code.",
      inputSchema: {
        type: "object",
        required: ["name"],
        properties: {
          name: { type: "string", pattern: "^[A-Za-z][A-Za-z0-9_-]{0,63}$" },
          version: { type: "string", default: "0.1.0" },
          profile: { type: "string", enum: ["Debug", "Development", "Shipping", "Server", "Editor"], default: "Development" },
          platform: { type: "string", enum: ["windows-x64", "linux-x64", "macos-universal", "dedicated-server"], default: "windows-x64" },
          plugins: { type: "array", items: { type: "string" }, default: [] },
          initial_scene: { type: "string", default: "Initial" },
          starter_scene: { type: "boolean", default: true }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_game_project",
      description: "Inspect a project's portable manifest, scenes, scripts, assets, and validation status.",
      inputSchema: {
        type: "object",
        required: ["project"],
        properties: { project: { type: "string" } },
        additionalProperties: false
      }
    },
    {
      name: "create_scene",
      description: "Create a VulkanEngine scene inside an existing game project.",
      inputSchema: {
        type: "object",
        required: ["project", "name"],
        properties: {
          project: { type: "string" },
          name: { type: "string" },
          starter_entities: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_scene",
      description: "Return a compact entity/component map for a project scene.",
      inputSchema: {
        type: "object",
        required: ["project", "scene"],
        properties: { project: { type: "string" }, scene: { type: "string" } },
        additionalProperties: false
      }
    },
    {
      name: "create_entity",
      description: "Create an entity with Transform and optional engine components in a scene.",
      inputSchema: {
        type: "object",
        required: ["project", "scene", "name"],
        properties: {
          project: { type: "string" }, scene: { type: "string" }, name: { type: "string", minLength: 1 },
          transform: { type: "object" }, components: { type: "object" }
        },
        additionalProperties: false
      }
    },
    {
      name: "remove_entity",
      description: "Remove one entity from a scene by UUID, including hierarchy references to it.",
      inputSchema: {
        type: "object",
        required: ["project", "scene", "entity_id"],
        properties: { project: { type: "string" }, scene: { type: "string" }, entity_id: { type: "string", format: "uuid" } },
        additionalProperties: false
      }
    },
    {
      name: "set_component",
      description: "Add or update one engine component on a scene entity. Values use the schema returned by game_capabilities.",
      inputSchema: {
        type: "object",
        required: ["project", "scene", "entity_id", "component", "values"],
        properties: {
          project: { type: "string" }, scene: { type: "string" }, entity_id: { type: "string", format: "uuid" },
          component: { type: "string", enum: Object.keys(COMPONENT_SCHEMAS) }, values: { type: "object" }, replace: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "remove_component",
      description: "Remove one component from a scene entity. Transform cannot be removed.",
      inputSchema: {
        type: "object",
        required: ["project", "scene", "entity_id", "component"],
        properties: {
          project: { type: "string" }, scene: { type: "string" }, entity_id: { type: "string", format: "uuid" },
          component: { type: "string", enum: Object.keys(COMPONENT_SCHEMAS).filter((name) => name !== "Transform") }
        },
        additionalProperties: false
      }
    },
    {
      name: "create_visual_script",
      description: "Create or replace a typed VulkanEngine visual-script graph in a project without modifying engine code.",
      inputSchema: {
        type: "object",
        required: ["project", "name", "nodes", "links"],
        properties: {
          project: { type: "string" }, name: { type: "string" }, scene_companion: { type: "boolean", default: false },
          nodes: {
            type: "array", items: {
              type: "object", required: ["key", "kind"],
              properties: {
                key: { type: "string" }, kind: { type: "string", enum: SCRIPT_NODE_KINDS },
                event: { type: "string" }, variable: { type: "string" }, literal: {}
              }, additionalProperties: false
            }
          },
          links: {
            type: "array", items: {
              type: "object", required: ["from", "to"],
              properties: { from: { type: "string" }, to: { type: "string" } }, additionalProperties: false
            }
          }
        },
        additionalProperties: false
      }
    },
    {
      name: "create_material",
      description: "Create a native VulkanEngine PBR material asset in a game project.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          albedo: { type: "object", properties: { r: { type: "number" }, g: { type: "number" }, b: { type: "number" } }, additionalProperties: false },
          roughness: { type: "number", minimum: 0, maximum: 1, default: 0.5 },
          metallic: { type: "number", minimum: 0, maximum: 1, default: 0 },
          emissive: { type: "object", properties: { r: { type: "number" }, g: { type: "number" }, b: { type: "number" }, intensity: { type: "number" } }, additionalProperties: false },
          texture_ids: { type: "object", properties: { albedo: { type: "string" }, normal: { type: "string" }, roughness: { type: "string" }, metallic: { type: "string" } }, additionalProperties: false }
        }, additionalProperties: false
      }
    },
    {
      name: "create_audio_event",
      description: "Create a native VulkanEngine audio-event asset with pitch variation and spatial settings.",
      inputSchema: {
        type: "object", required: ["project", "name", "clip"],
        properties: {
          project: { type: "string" }, name: { type: "string" }, clip: { type: "string" },
          volume: { type: "number", minimum: 0, default: 1 }, min_pitch: { type: "number", default: 0.9 },
          max_pitch: { type: "number", default: 1.1 }, max_distance: { type: "number", minimum: 0, default: 100 },
          spatial: { type: "boolean", default: true }, looping: { type: "boolean", default: false }
        }, additionalProperties: false
      }
    },
    {
      name: "create_physics_material",
      description: "Create a native VulkanEngine physics-material asset.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" }, friction: { type: "number", minimum: 0, default: 0.5 },
          restitution: { type: "number", minimum: 0, maximum: 1, default: 0.1 }, density: { type: "number", minimum: 0, default: 1000 },
          hardness: { type: "number", minimum: 0, maximum: 1, default: 0.5 }
        }, additionalProperties: false
      }
    },
    {
      name: "stage_asset",
      description: "Copy a source asset into a portable project Assets directory and write import metadata. Cooking remains the engine's responsibility.",
      inputSchema: {
        type: "object",
        required: ["project", "source"],
        properties: {
          project: { type: "string" }, source: { type: "string" }, destination: { type: "string" },
          import_settings: { type: "object", default: {} }
        },
        additionalProperties: false
      }
    },
    {
      name: "author_registry_asset",
      description: "Author a registry asset (block/item/fluid/recipe/biome/structure) as a versioned JSON document in Content/Registry/<kind>/, mirroring exactly the public C++ registry JSON schemas. Validates structure against the public contracts; dry_run previews the document/diff without writing; update replaces an existing asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object",
        required: ["project", "kind", "name"],
        properties: {
          project: { type: "string" },
          kind: { type: "string", enum: REGISTRY_KINDS },
          name: { type: "string", minLength: 1, description: "asset name (becomes the file name)" },
          dry_run: { type: "boolean", default: false },
          update: { type: "boolean", default: false },
          id: { type: "string", description: "persistent UUID; derived from the namespaced name when omitted" },
          namespace: { type: "string", default: "vulkancraft" },
          version: { type: "integer", default: 1 },
          // block
          class: { type: "string", enum: [...BLOCK_CLASSES] },
          hardness: { type: "number" }, light_emission: { type: "number" }, light_absorption: { type: "number" },
          opaque: { type: "boolean" }, collidable: { type: "boolean" }, builtin_id: { type: "integer" },
          color: { type: "array", items: { type: "number" } }, tags: { type: "array", items: { type: "string" } }, drops: { type: "array", items: { type: "string" } },
          states: { type: "array", items: { type: "object", properties: { name: { type: "string" }, color: { type: "array", items: { type: "number" } }, face_top: { type: "array", items: { type: "number" } }, face_bottom: { type: "array", items: { type: "number" } }, face_side: { type: "array", items: { type: "number" } }, light_emission: { type: "number" } }, additionalProperties: false } },
          transitions: { type: "array", items: { type: "object", properties: { from: { type: "string" }, to: { type: "string" }, trigger: { type: "string" } }, additionalProperties: false } },
          fluid: { type: "object", properties: { viscosity: { type: "number" }, density: { type: "number" }, range: { type: "integer" }, tick_interval: { type: "number" }, source: { type: "boolean" }, falling: { type: "boolean" }, evaporation: { type: "boolean" }, damage_per_tick: { type: "number" }, compressible: { type: "boolean" } }, additionalProperties: false },
          sound_place: { type: "string" }, sound_break: { type: "string" }, sound_step: { type: "string" }, sound_hit: { type: "string" },
          particle_break: { type: "string" }, tool: { type: "string" }, tool_tier: { type: "integer" },
          resistance: { type: "number" }, friction: { type: "number" }, bounciness: { type: "number" }, density: { type: "number" },
          behavior: { type: "string" },
          // item (§2 item 8: use/equipment/behavior components)
          max_stack: { type: "integer" }, durability: { type: "integer" }, icon: { type: "string" }, model: { type: "string" },
          use_cooldown: { type: "integer", description: "use cooldown in ms (0..60000)" },
          use_mode: { type: "string", enum: ["none", "instant", "continuous"] },
          equip_slot: { type: "string", enum: ["none", "hand", "offhand", "head", "chest", "legs", "feet"] },
          attack_damage: { type: "number", description: "0..100" },
          armor: { type: "number", description: "0..100" },
          behavior: { type: "string", description: "namespaced behavior id (ns:name) or empty" },
          // fluid
          block: { type: "string", description: "namespaced block the fluid drives (required for kind=fluid)" },
          viscosity: { type: "number" }, density: { type: "number" }, range: { type: "integer" },
          tick_interval: { type: "number" }, source: { type: "boolean" }, falling: { type: "boolean" },
          evaporation: { type: "boolean" }, damage_per_tick: { type: "number" }, compressible: { type: "boolean" },
          // recipe
          station: { type: "string" }, time: { type: "number" }, energy: { type: "number" }, fuel: { type: "string" },
          conditions: { type: "array", items: { type: "string" } },
          inputs: {
            type: "array", items: {
              type: "object", properties: { item: { type: "string" }, tag: { type: "string" }, count: { type: "integer" }, alternatives: { type: "array", items: { type: "string" } } }, additionalProperties: false
            }
          },
          outputs: {
            type: "array", items: {
              type: "object", required: ["item"], properties: { item: { type: "string" }, count: { type: "integer" }, chance: { type: "number" } }, additionalProperties: false
            }
          },
          byproducts: {
            type: "array", items: {
              type: "object", required: ["item"], properties: { item: { type: "string" }, count: { type: "integer" }, chance: { type: "number" } }, additionalProperties: false
            }
          },
          // biome
          biomes: {
            type: "array", items: {
              type: "object", required: ["name"], properties: {
                name: { type: "string" }, engine_biome_index: { type: "integer" },
                climate: { type: "object", additionalProperties: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 } },
                surface: {
                  type: "array", items: {
                    type: "object", properties: {
                      block_id: { type: "integer" }, min_depth: { type: "integer" }, max_depth: { type: "integer" },
                      min_height: { type: "integer" }, max_height: { type: "integer" }, min_slope: { type: "number" }
                    }, additionalProperties: false
                  }
                }
              }, additionalProperties: false
            }
          },
          // structure
          sample_width: { type: "integer" }, sample_height: { type: "integer" },
          sample: { type: "array", items: { type: "integer" } },
          pattern_size: { type: "integer" }, symmetry: { type: "integer" },
          periodic_output: { type: "boolean" }, ground: { type: "boolean" }, seed: { type: "integer" },
          profiles: {
            type: "array", items: {
              type: "object", required: ["block_id", "layers"], properties: {
                block_id: { type: "integer" }, layers: { type: "array", items: { type: "integer" } }
              }, additionalProperties: false
            }
          }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_registry_assets",
      description: "List and validate every registry asset (block/item/fluid/recipe/biome/structure) under Content/Registry for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_vehicle_asset",
      description: "Author a vehicle assembly (vehicle = rigid wheeled VehicleAsset; beam = node/beam deformable BeamGraphAsset) as a versioned JSON document in Content/Vehicles/, mirroring exactly the public C++ vehicle JSON schemas. Validates structure against the public contracts; dry_run previews the document/diff without writing; update replaces an existing asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object",
        required: ["project", "kind", "name"],
        properties: {
          project: { type: "string" },
          kind: { type: "string", enum: VEHICLE_KINDS },
          name: { type: "string", minLength: 1, description: "asset name (becomes the file name)" },
          dry_run: { type: "boolean", default: false },
          update: { type: "boolean", default: false },
          id: { type: "string" },
          version: { type: "integer", default: 1 },
          provider: { type: "string", enum: ["jolt", "chrono", "jsbsim"], default: "jolt", description: "physics provider (items 5/6): only jolt is vendored — chrono/jsbsim refused at creation" },
          // vehicle (rigid) — the Jolt controller family of a rigid vehicle
          vehicle_kind: { type: "string", enum: [...VEHICLE_KIND_ENUM], description: "Jolt controller family (wheeled|motorcycle|tracked) for kind=vehicle" },
          position: { type: "array", items: { type: "number" } },
          rotation: { type: "array", items: { type: "number" } },
          chassis: {
            type: "object", properties: {
              shape: { type: "string", enum: [...VEHICLE_SHAPES] }, half_extents: { type: "array", items: { type: "number" } },
              halfExtents: { type: "array", items: { type: "number" } }, radius: { type: "number" }, half_height: { type: "number" },
              halfHeight: { type: "number" }, mass: { type: "number" }, friction: { type: "number" }, restitution: { type: "number" }
            }, additionalProperties: false
          },
          wheels: {
            type: "array", items: {
              type: "object", properties: {
                localPosition: { type: "array", items: { type: "number" } }, radius: { type: "number" },
                suspension_rest_length: { type: "number" }, suspensionRestLength: { type: "number" },
                suspension_travel: { type: "number" }, suspensionTravel: { type: "number" },
                spring_strength: { type: "number" }, springStrength: { type: "number" },
                damper_strength: { type: "number" }, damperStrength: { type: "number" },
                tire_grip: { type: "number" }, tireGrip: { type: "number" },
                max_drive_force: { type: "number" }, maxDriveForce: { type: "number" },
                max_brake_force: { type: "number" }, maxBrakeForce: { type: "number" },
                max_steer_angle: { type: "number" }, maxSteerAngle: { type: "number" },
                steering: { type: "boolean" }, driven: { type: "boolean" }
              }, additionalProperties: false
            }
          },
          drivetrain: {
            type: "object", properties: {
              engine_max_torque: { type: "number" }, engineMaxTorque: { type: "number" },
              engine_min_rpm: { type: "number" }, engineMinRPM: { type: "number" },
              engine_max_rpm: { type: "number" }, engineMaxRPM: { type: "number" },
              differential_ratio: { type: "number" }, differentialRatio: { type: "number" },
              gear_ratios: { type: "array", items: { type: "number" } }, gearRatios: { type: "array", items: { type: "number" } }
            }, additionalProperties: false
          },
          propulsion: {
            type: "array", items: {
              type: "object", properties: {
                kind: { type: "string", enum: [...PROPULSION_KINDS] }, localPosition: { type: "array", items: { type: "number" } },
                axis: { type: "array", items: { type: "number" } }, max_force: { type: "number" }, maxForce: { type: "number" },
                area: { type: "number" }, lift_coefficient: { type: "number" }, liftCoefficient: { type: "number" },
                fluid_density: { type: "number" }, fluidDensity: { type: "number" }, water_level: { type: "number" }, waterLevel: { type: "number" }
              }, additionalProperties: false
            }
          },
          seats: {
            type: "array", items: {
              type: "object", properties: {
                name: { type: "string" }, localPosition: { type: "array", items: { type: "number" } },
                exitOffset: { type: "array", items: { type: "number" } }
              }, additionalProperties: false
            }
          },
          power: {
            type: "object", properties: {
              fuel: { type: "object", properties: { capacity: { type: "number" }, initial_level: { type: "number" }, initialLevel: { type: "number" }, burn_per_second: { type: "number" }, burnPerSecond: { type: "number" }, idle_burn_per_second: { type: "number" }, idleBurnPerSecond: { type: "number" }, min_level_to_run: { type: "number" }, minLevelToRun: { type: "number" } }, additionalProperties: false },
              energy: { type: "object", properties: { capacity: { type: "number" }, initial_charge: { type: "number" }, initialCharge: { type: "number" }, draw_per_second: { type: "number" }, drawPerSecond: { type: "number" }, regen_per_second: { type: "number" }, regenPerSecond: { type: "number" }, min_charge_to_run: { type: "number" }, minChargeToRun: { type: "number" } }, additionalProperties: false },
              controls: { type: "object", properties: { throttle_deadzone: { type: "number" }, throttleDeadzone: { type: "number" }, throttle_sensitivity: { type: "number" }, throttleSensitivity: { type: "number" }, throttle_invert: { type: "boolean" }, throttleInvert: { type: "boolean" }, steering_deadzone: { type: "number" }, steeringDeadzone: { type: "number" }, steering_sensitivity: { type: "number" }, steeringSensitivity: { type: "number" }, steering_invert: { type: "boolean" }, steeringInvert: { type: "boolean" }, brake_deadzone: { type: "number" }, brakeDeadzone: { type: "number" }, brake_sensitivity: { type: "number" }, brakeSensitivity: { type: "number" } }, additionalProperties: false }
            }, additionalProperties: false
          },
          // beam (node/beam deformable)
          mass: { type: "number" },
          nodes: { type: "array", items: { type: "object", properties: { position: { type: "array", items: { type: "number" } }, fixed: { type: "boolean" } }, additionalProperties: false } },
          beams: { type: "array", items: { type: "object", properties: { a: { type: "integer" }, b: { type: "integer" }, stiffness: { type: "number" } }, additionalProperties: false } },
          solver: { type: "object", properties: { substeps: { type: "integer" }, solver_iterations: { type: "integer" }, solverIterations: { type: "integer" }, stiffness: { type: "number" }, damping: { type: "number" }, gravity: { type: "array", items: { type: "number" } } }, additionalProperties: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_vehicle_assets",
      description: "List and validate every vehicle assembly asset (rigid vehicle / beam) under Content/Vehicles for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_ability_asset",
      description: "Author a data-driven ability (FALTANTES §19) as a versioned JSON document in Content/Abilities/, mirroring exactly the public C++ ability JSON schema (AbilityDefinition::load_from_json). Attributes/tags/cost/cooldown/conditions gate the cast; composable effects (damage/heal/impulse/telekinesis/flight/blockEdit/periodic) apply in order; targeting resolves self/direction/point/body. Validates structure against the public contract; dry_run previews the document/diff without writing; update replaces an existing asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object",
        required: ["project", "name", "effects"],
        properties: {
          project: { type: "string" },
          name: { type: "string", minLength: 1, description: "ability name (becomes the file name)" },
          dry_run: { type: "boolean", default: false },
          update: { type: "boolean", default: false },
          id: { type: "string" },
          version: { type: "integer", default: 1 },
          cooldown_seconds: { type: "number", default: 0, description: "cooldown after a cast, >= 0" },
          cancelable: { type: "boolean", default: true },
          interruptible: { type: "boolean", default: true },
          attributes: {
            type: "array", items: {
              type: "object", properties: { name: { type: "string" }, value: { type: "number" } }, additionalProperties: false
            }
          },
          tags: { type: "array", items: { type: "string" } },
          cost: {
            type: "object", properties: { resource: { type: "string" }, amount: { type: "number" } }, additionalProperties: false
          },
          conditions: {
            type: "array", items: {
              type: "object", properties: {
                kind: { type: "string", enum: [...ABILITY_CONDITION_KINDS] },
                tag: { type: "string" }, attribute: { type: "string" },
                min_value: { type: "number" }, minValue: { type: "number" },
                max_distance: { type: "number" }, maxDistance: { type: "number" }
              }, additionalProperties: false
            }
          },
          targeting: {
            type: "object", properties: {
              mode: { type: "string", enum: [...ABILITY_TARGET_MODES] },
              range: { type: "number" }, radius: { type: "number" }
            }, additionalProperties: false
          },
          effects: {
            type: "array", items: {
              type: "object", properties: {
                type: { type: "string", enum: [...ABILITY_EFFECT_TYPES] },
                amount: { type: "number" }, force: { type: "number" },
                hold_offset: { type: "array", items: { type: "number" } }, holdOffset: { type: "array", items: { type: "number" } },
                grab_force: { type: "number" }, grabForce: { type: "number" },
                duration_seconds: { type: "number" }, durationSeconds: { type: "number" },
                thrust: { type: "number" },
                min: { type: "array", items: { type: "number" } }, max: { type: "array", items: { type: "number" } },
                block_id: { type: "number" }, blockId: { type: "number" }, relative: { type: "boolean" },
                interval_seconds: { type: "number" }, intervalSeconds: { type: "number" },
                ticks: { type: "integer" },
                subEffect: { type: "object", description: "periodic sub-effect (any effect type, recursively)" },
                cast_animation: { type: "string" }, castAnimation: { type: "string" },
                particle_effect: { type: "string" }, particleEffect: { type: "string" },
                sound_effect: { type: "string" }, soundEffect: { type: "string" }
              }, additionalProperties: false
            }
          }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_ability_assets",
      description: "List and validate every ability asset under Content/Abilities for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "validate_game_project",
      description: "Validate the portable project, scenes, entity UUIDs, components, hierarchy, scripts, registry assets, and asset metadata without compiling the engine.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    }
  ];
}

export function callSemanticTool(engineRoot, name, args = {}) {
  switch (name) {
    case "game_capabilities": return capabilityDocument();
    case "list_game_projects": return listProjects(engineRoot);
    case "create_game_project": return createProject(engineRoot, args);
    case "inspect_game_project": return inspectProject(engineRoot, args.project);
    case "create_scene": return createScene(engineRoot, args);
    case "inspect_scene": return inspectScene(engineRoot, args);
    case "create_entity": return createEntity(engineRoot, args);
    case "remove_entity": return removeEntity(engineRoot, args);
    case "set_component": return setComponent(engineRoot, args);
    case "remove_component": return removeComponent(engineRoot, args);
    case "create_visual_script": return createVisualScript(engineRoot, args);
    case "create_material": return createMaterial(engineRoot, args);
    case "create_audio_event": return createAudioEvent(engineRoot, args);
    case "create_physics_material": return createPhysicsMaterial(engineRoot, args);
    case "stage_asset": return stageAsset(engineRoot, args);
    case "author_registry_asset": return authorRegistryAsset(engineRoot, args);
    case "inspect_registry_assets": return inspectRegistryAssets(engineRoot, args.project);
    case "author_vehicle_asset": return authorVehicleAsset(engineRoot, args);
    case "inspect_vehicle_assets": return inspectVehicleAssets(engineRoot, args.project);
    case "author_ability_asset": return authorAbilityAsset(engineRoot, args);
    case "inspect_ability_assets": return inspectAbilityAssets(engineRoot, args.project);
    case "validate_game_project": return validateProject(engineRoot, args.project);
    default: return undefined;
  }
}

function listProjects(engineRoot) {
  const root = path.join(engineRoot, "Projects");
  fs.mkdirSync(root, { recursive: true });
  const projects = fs.readdirSync(root, { withFileTypes: true })
    .filter((entry) => entry.isDirectory() && !entry.name.startsWith("."))
    .map((entry) => {
      const manifest = path.join(root, entry.name, "project.json");
      return { name: entry.name, managed: fs.existsSync(manifest), path: `Projects/${entry.name}` };
    });
  return { projects };
}

function createProject(engineRoot, args) {
  const paths = projectPaths(engineRoot, args.name);
  if (fs.existsSync(paths.root) && fs.readdirSync(paths.root).length > 0) throw new Error(`project '${paths.project}' already exists`);
  const version = String(args.version ?? "0.1.0");
  const profile = String(args.profile ?? "Development");
  const platform = String(args.platform ?? "windows-x64");
  const plugins = [...new Set((args.plugins ?? []).map(String))];
  const initialScene = ensureSceneName(args.initial_scene ?? "Initial");
  if (!PROFILE_NAMES.has(profile)) throw new Error(`unsupported profile '${profile}'`);
  if (!PLATFORM_NAMES.has(platform)) throw new Error(`unsupported platform '${platform}'`);

  for (const directory of [paths.content, paths.scenes, paths.scripts, paths.assets, paths.materials, paths.audioEvents, paths.physicsMaterials, paths.registry, paths.vehicles, path.join(paths.root, "Config"), path.join(paths.root, "Intermediate"), path.join(paths.root, "Build")]) {
    fs.mkdirSync(directory, { recursive: true });
  }
  const scene = createEmptyScene(initialScene);
  if (args.starter_scene !== false) createStarterEntities(scene);
  atomicWriteJson(path.join(paths.scenes, `${initialScene}.scene`), scene);
  const manifest = {
    format: "VulkanEngine.Project",
    version: 1,
    name: paths.project,
    gameVersion: version,
    engine: "../..",
    initialScene: `Content/Scenes/${initialScene}.scene`,
    profile,
    platform,
    plugins
  };
  atomicWriteJson(paths.manifest, manifest);
  writeProjectConfig(paths.config, {
    name: paths.project, version, initialScene: manifest.initialScene, profile, platform, plugins
  });
  atomicWrite(path.join(paths.root, "Config", "Plugins.ini"), `${plugins.map((plugin) => `${plugin}=true`).join("\n")}${plugins.length ? "\n" : ""}`);
  return {
    created: true,
    project: paths.project,
    path: `Projects/${paths.project}`,
    initial_scene: manifest.initialScene,
    portable: true,
    engine_source_modified: false
  };
}

function inspectProject(engineRoot, projectName) {
  const paths = requireProject(engineRoot, projectName);
  const manifest = readJson(paths.manifest);
  const files = (directory, extension) => fs.existsSync(directory)
    ? fs.readdirSync(directory, { withFileTypes: true }).filter((entry) => entry.isFile() && (!extension || entry.name.endsWith(extension))).map((entry) => entry.name).sort()
    : [];
  return {
    project: paths.project,
    manifest,
    scenes: files(paths.scenes, ".scene"),
    scripts: [...files(paths.scripts, ".script"), ...files(paths.scenes, ".script")],
    assets: fs.existsSync(paths.assets) ? fs.readdirSync(paths.assets, { withFileTypes: true }).filter((entry) => entry.isFile() && !entry.name.endsWith(".import.json")).map((entry) => entry.name).sort() : [],
    vehicles: files(paths.vehicles, ".json"),
    validation: validateProject(engineRoot, paths.project)
  };
}

function createScene(engineRoot, args) {
  const resolved = scenePath(engineRoot, args.project, args.name);
  if (fs.existsSync(resolved.file)) throw new Error(`scene '${resolved.scene}' already exists`);
  const document = createEmptyScene(resolved.scene);
  if (args.starter_entities) createStarterEntities(document);
  atomicWriteJson(resolved.file, document);
  return { created: true, project: resolved.project.project, scene: resolved.scene, path: path.relative(resolved.project.root, resolved.file).replaceAll(path.sep, "/"), scene_id: document.scene_id };
}

function inspectScene(engineRoot, args) {
  const scene = requireScene(engineRoot, args.project, args.scene);
  return {
    project: scene.project.project,
    scene: scene.scene,
    scene_id: scene.document.scene_id,
    entity_count: scene.document.entities.length,
    entities: scene.document.entities.map((entity) => ({ id: entity.id, name: entity.name, components: Object.keys(entity).filter((key) => !["id", "name"].includes(key)) }))
  };
}

function normalizeComponent(component, values, replace = false) {
  const defaults = COMPONENT_SCHEMAS[component];
  if (!defaults) throw new Error(`unsupported component '${component}'`);
  if (!values || typeof values !== "object" || Array.isArray(values)) throw new Error("component values must be an object");
  const unknown = Object.keys(values).filter((key) => !(key in defaults));
  if (unknown.length) throw new Error(`unknown ${component} fields: ${unknown.join(", ")}`);
  return replace ? { ...defaults, ...values } : values;
}

function createEntity(engineRoot, args) {
  const scene = requireScene(engineRoot, args.project, args.scene);
  const entity = {
    id: uuid(),
    name: String(args.name),
    Transform: { ...COMPONENT_SCHEMAS.Transform, ...normalizeComponent("Transform", args.transform ?? {}) }
  };
  for (const [component, values] of Object.entries(args.components ?? {})) {
    if (component === "Transform") continue;
    entity[component] = { ...COMPONENT_SCHEMAS[component], ...normalizeComponent(component, values) };
  }
  scene.document.entities.push(entity);
  atomicWriteJson(scene.file, scene.document);
  return { created: true, project: scene.project.project, scene: scene.scene, entity_id: entity.id, name: entity.name, components: Object.keys(entity).filter((key) => !["id", "name"].includes(key)) };
}

function findEntity(scene, entityId) {
  const entity = scene.document.entities.find((candidate) => candidate.id === entityId);
  if (!entity) throw new Error(`entity '${entityId}' does not exist`);
  return entity;
}

function removeEntity(engineRoot, args) {
  const scene = requireScene(engineRoot, args.project, args.scene);
  const before = scene.document.entities.length;
  scene.document.entities = scene.document.entities.filter((entity) => entity.id !== args.entity_id);
  if (scene.document.entities.length === before) throw new Error(`entity '${args.entity_id}' does not exist`);
  for (const entity of scene.document.entities) {
    if (entity.Hierarchy?.parent_id === args.entity_id) delete entity.Hierarchy;
  }
  atomicWriteJson(scene.file, scene.document);
  return { removed: true, entity_id: args.entity_id };
}

function setComponent(engineRoot, args) {
  const scene = requireScene(engineRoot, args.project, args.scene);
  const entity = findEntity(scene, args.entity_id);
  const component = String(args.component);
  const values = normalizeComponent(component, args.values, Boolean(args.replace));
  entity[component] = args.replace || !entity[component]
    ? { ...COMPONENT_SCHEMAS[component], ...values }
    : { ...entity[component], ...values };
  atomicWriteJson(scene.file, scene.document);
  return { updated: true, entity_id: entity.id, component, value: entity[component] };
}

function removeComponent(engineRoot, args) {
  if (args.component === "Transform") throw new Error("Transform is mandatory and cannot be removed");
  const scene = requireScene(engineRoot, args.project, args.scene);
  const entity = findEntity(scene, args.entity_id);
  if (!entity[args.component]) throw new Error(`entity does not have component '${args.component}'`);
  delete entity[args.component];
  atomicWriteJson(scene.file, scene.document);
  return { removed: true, entity_id: entity.id, component: args.component };
}

function literalToEngineJson(value) {
  if (typeof value === "boolean") return { type: "bool", value };
  if (typeof value === "number" && Number.isInteger(value)) return { type: "int", value };
  if (typeof value === "number") return { type: "float", value };
  if (typeof value === "string") return { type: "string", value };
  if (value && typeof value === "object" && ["bool", "int", "float", "string", "uuid"].includes(value.type)) return value;
  throw new Error("unsupported script literal; use boolean, number, string, or an engine typed literal");
}

function createVisualScript(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const scriptName = ensureSceneName(args.name);
  const keys = new Map();
  const nodes = args.nodes.map((node) => {
    if (keys.has(node.key)) throw new Error(`duplicate script node key '${node.key}'`);
    if (!SCRIPT_NODE_KINDS.includes(node.kind)) throw new Error(`unsupported script node kind '${node.kind}'`);
    const id = uuid();
    keys.set(node.key, id);
    const result = { id, kind: node.kind };
    if (node.event !== undefined) result.event = String(node.event);
    if (node.variable !== undefined) result.variable = String(node.variable);
    if (node.literal !== undefined) result.literal = literalToEngineJson(node.literal);
    return result;
  });
  const links = args.links.map((link) => {
    if (!keys.has(link.from) || !keys.has(link.to)) throw new Error(`script link references an unknown key: ${link.from} -> ${link.to}`);
    return { from: keys.get(link.from), to: keys.get(link.to) };
  });
  const document = { id: uuid(), name: scriptName, nodes, links };
  const directory = args.scene_companion ? project.scenes : project.scripts;
  const file = path.join(directory, `${scriptName}.script`);
  atomicWriteJson(file, document);
  return { created: true, project: project.project, script: scriptName, path: path.relative(project.root, file).replaceAll(path.sep, "/"), node_ids: Object.fromEntries(keys), nodes: nodes.length, links: links.length };
}

function assetName(name) {
  return ensureSceneName(name).replaceAll(" ", "_");
}

function createMaterial(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const name = assetName(args.name);
  const file = path.join(project.materials, `${name}.material`);
  if (fs.existsSync(file)) throw new Error(`material '${name}' already exists`);
  const texture = args.texture_ids ?? {};
  const zeroUuid = "00000000-0000-0000-0000-000000000000";
  const document = {
    format: "VulkanEngine.Material", version: 1, material_id: uuid(), name: String(args.name),
    albedo: { r: Number(args.albedo?.r ?? 1), g: Number(args.albedo?.g ?? 1), b: Number(args.albedo?.b ?? 1) },
    roughness: Number(args.roughness ?? 0.5), metallic: Number(args.metallic ?? 0),
    emissiveColor: { r: Number(args.emissive?.r ?? 0), g: Number(args.emissive?.g ?? 0), b: Number(args.emissive?.b ?? 0) },
    emissiveIntensity: Number(args.emissive?.intensity ?? 0),
    albedoMapID: String(texture.albedo ?? zeroUuid), normalMapID: String(texture.normal ?? zeroUuid),
    roughnessMapID: String(texture.roughness ?? zeroUuid), metallicMapID: String(texture.metallic ?? zeroUuid)
  };
  atomicWriteJson(file, document);
  return { created: true, project: project.project, material: name, material_id: document.material_id, path: path.relative(project.root, file).replaceAll(path.sep, "/") };
}

function createAudioEvent(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const name = assetName(args.name);
  const file = path.join(project.audioEvents, `${name}.audioevent`);
  if (fs.existsSync(file)) throw new Error(`audio event '${name}' already exists`);
  const minPitch = Number(args.min_pitch ?? 0.9);
  const maxPitch = Number(args.max_pitch ?? 1.1);
  if (minPitch > maxPitch) throw new Error("min_pitch cannot exceed max_pitch");
  const document = {
    format: "VulkanEngine.AudioEvent", version: 1, audio_event_id: uuid(), name: String(args.name), clipPath: String(args.clip),
    volume: Number(args.volume ?? 1), minPitch, maxPitch, maxDistance: Number(args.max_distance ?? 100),
    is3D: args.spatial !== false, isLooping: Boolean(args.looping)
  };
  atomicWriteJson(file, document);
  return { created: true, project: project.project, audio_event: name, audio_event_id: document.audio_event_id, path: path.relative(project.root, file).replaceAll(path.sep, "/") };
}

function createPhysicsMaterial(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const name = assetName(args.name);
  const file = path.join(project.physicsMaterials, `${name}.physicsmaterial`);
  if (fs.existsSync(file)) throw new Error(`physics material '${name}' already exists`);
  const document = {
    format: "VulkanEngine.PhysicsMaterial", version: 1, physics_material_id: uuid(), name: String(args.name),
    friction: Number(args.friction ?? 0.5), restitution: Number(args.restitution ?? 0.1),
    density: Number(args.density ?? 1000), hardness: Number(args.hardness ?? 0.5)
  };
  atomicWriteJson(file, document);
  return { created: true, project: project.project, physics_material: name, physics_material_id: document.physics_material_id, path: path.relative(project.root, file).replaceAll(path.sep, "/") };
}

function stageAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const source = path.resolve(String(args.source));
  if (!fs.existsSync(source) || !fs.statSync(source).isFile()) throw new Error("asset source does not exist or is not a file");
  const destinationName = args.destination ? path.basename(String(args.destination)) : path.basename(source);
  const destination = path.join(project.assets, destinationName);
  if (fs.existsSync(destination)) throw new Error(`asset '${destinationName}' already exists in the project`);
  fs.copyFileSync(source, destination, fs.constants.COPYFILE_EXCL);
  const metadata = {
    format: "VulkanEngine.ImportSettings",
    version: 1,
    source: destinationName,
    sha256: sha256File(destination),
    settings: args.import_settings ?? {}
  };
  atomicWriteJson(`${destination}.import.json`, metadata);
  return { staged: true, project: project.project, asset: `Assets/${destinationName}`, sha256: metadata.sha256, cooking_required: true };
}

function validateRegistryBlock(document, errors) {
  if (typeof document.name !== "string" || !document.name) errors.push("block 'name' is required");
  if (typeof document.namespace !== "string" || !document.namespace) errors.push("block 'namespace' cannot be empty");
  if (document.class !== undefined && !BLOCK_CLASSES.has(String(document.class))) errors.push(`block 'class' must be one of ${[...BLOCK_CLASSES].join(", ")}`);
  if (document.builtinId !== undefined) {
    if (!Number.isInteger(document.builtinId) || document.builtinId < 0 || document.builtinId > MAX_BUILTIN_BLOCK_ID) {
      errors.push(`block 'builtinId' must be an integer in [0, ${MAX_BUILTIN_BLOCK_ID}] (BlockType::Count)`);
    } else {
      // The builtin table registers every id in [0, Count); a JSON block that
      // declares a builtinId is refused by BlockRegistry::add as "already
      // used". The MCP authors catalog-only blocks: omit builtin_id.
      errors.push(`block 'builtinId' ${document.builtinId} is already used by the builtin table; omit builtin_id (catalog-only block)`);
    }
  }
  if (document.color !== undefined && (!Array.isArray(document.color) || document.color.length < 3 || document.color.some((channel) => typeof channel !== "number" || channel < 0 || channel > 1))) {
    errors.push("block 'color' must be [r, g, b(, a)] with each channel in 0..1");
  }
  for (const face of ["faceTop", "faceBottom", "faceSide"]) {
    const faceValue = document[face];
    if (faceValue !== undefined && (!Array.isArray(faceValue) || faceValue.length < 3 || faceValue.some((channel) => typeof channel !== "number" || channel < 0 || channel > 1))) {
      errors.push(`block '${face}' must be [r, g, b(, a)] with each channel in 0..1`);
    }
  }
  if (document.occlusion !== undefined && typeof document.occlusion !== "boolean") {
    errors.push("block 'occlusion' must be a boolean");
  }
  // Collision/selection shapes (FALTANTES item 2), mirrored from the strict
  // C++ enums: explicit unknown values are refused, never guessed.
  for (const shapeField of ["collisionShape", "selectionShape"]) {
    const shape = document[shapeField];
    if (shape !== undefined && !["full", "cross", "none"].includes(String(shape))) {
      errors.push(`block '${shapeField}' must be one of full, cross, none`);
    }
  }
  if (document.renderLayer !== undefined && (!Number.isInteger(document.renderLayer) || document.renderLayer < 0 || document.renderLayer > 255)) {
    errors.push("block 'renderLayer' must be an integer in [0, 255]");
  }
  for (const field of ["tags", "drops"]) {
    if (document[field] !== undefined && (!Array.isArray(document[field]) || document[field].some((value) => typeof value !== "string"))) {
      errors.push(`block '${field}' must be an array of strings`);
    }
  }
  // Named states + versioned transitions (FALTANTES item 5), mirrored from the
  // C++ BlockRegistry::add (all-or-nothing: duplicates, unknown refs, empty
  // trigger and self/duplicate rules are refused).
  const states = Array.isArray(document.states) ? document.states : [];
  const stateNames = new Set();
  for (const state of states) {
    if (!state || typeof state !== "object" || Array.isArray(state)) {
      errors.push("block 'states' entries must be objects");
      continue;
    }
    if (typeof state.name !== "string" || !state.name) errors.push("block state name cannot be empty");
    else if (stateNames.has(state.name)) errors.push(`block has duplicate state '${state.name}'`);
    stateNames.add(state.name);
    if (state.color !== undefined && (!Array.isArray(state.color) || state.color.length < 3 || state.color.some((channel) => typeof channel !== "number" || channel < 0 || channel > 1))) {
      errors.push(`block state '${state.name}' 'color' must be [r, g, b(, a)] with each channel in 0..1`);
    }
    for (const face of ["faceTop", "faceBottom", "faceSide"]) {
      const faceValue = state[face];
      if (faceValue !== undefined && (!Array.isArray(faceValue) || faceValue.length < 3 || faceValue.some((channel) => typeof channel !== "number" || channel < 0 || channel > 1))) {
        errors.push(`block state '${state.name}' '${face}' must be [r, g, b(, a)] with each channel in 0..1`);
      }
    }
    if (state.lightEmission !== undefined && (typeof state.lightEmission !== "number" || state.lightEmission < 0 || state.lightEmission > 1)) {
      errors.push(`block state '${state.name}' 'lightEmission' must be a number in 0..1`);
    }
  }
  const transitions = Array.isArray(document.transitions) ? document.transitions : [];
  const seenRules = new Set();
  for (const transition of transitions) {
    if (!transition || typeof transition !== "object" || Array.isArray(transition)) {
      errors.push("block 'transitions' entries must be objects");
      continue;
    }
    const from = String(transition.from ?? "");
    const to = String(transition.to ?? "");
    const trigger = String(transition.trigger ?? "");
    if (!trigger) errors.push("block transition trigger cannot be empty");
    if (from === to) errors.push(`block transition '${trigger}' from and to state are identical`);
    if (from && !stateNames.has(from)) errors.push(`block transition '${trigger}' references unknown from-state '${from}'`);
    if (to && !stateNames.has(to)) errors.push(`block transition '${trigger}' references unknown to-state '${to}'`);
    const rule = `${from}|${trigger}`;
    if (seenRules.has(rule)) errors.push(`block has duplicate transition (from '${from}', trigger '${trigger}')`);
    seenRules.add(rule);
  }
  // Inline fluid binding (FALTANTES item 7), mirrored from BlockRegistry::add:
  // out-of-contract inline fluid values are refused (never clamped).
  if (document.fluid !== undefined) {
    if (!document.fluid || typeof document.fluid !== "object" || Array.isArray(document.fluid)) {
      errors.push("block 'fluid' must be an object");
    } else {
      const fluid = document.fluid;
      if (fluid.range !== undefined && (!Number.isInteger(fluid.range) || fluid.range < 1 || fluid.range > 7)) {
        errors.push("block 'fluid.range' must be an integer in 1..7");
      }
      if (fluid.viscosity !== undefined && (typeof fluid.viscosity !== "number" || fluid.viscosity < 0 || fluid.viscosity > 1)) {
        errors.push("block 'fluid.viscosity' must be a number in 0..1");
      }
      if (fluid.density !== undefined && (typeof fluid.density !== "number" || fluid.density < 0)) {
        errors.push("block 'fluid.density' cannot be negative");
      }
      if (fluid.tickInterval !== undefined && (typeof fluid.tickInterval !== "number" || fluid.tickInterval < 0)) {
        errors.push("block 'fluid.tickInterval' cannot be negative");
      }
      if (fluid.damagePerTick !== undefined && (typeof fluid.damagePerTick !== "number" || fluid.damagePerTick < 0)) {
        errors.push("block 'fluid.damagePerTick' cannot be negative");
      }
    }
  }
  // Sound/particle/tool/resistance/physics component (FALTANTES item 4),
  // mirrored from BlockRegistry::add (all-or-nothing, never clamped).
  for (const ref of ["soundPlace", "soundBreak", "soundStep", "soundHit", "particleBreak"]) {
    if (document[ref] !== undefined && document[ref] !== "" && !String(document[ref]).includes(":")) {
      errors.push(`block '${ref}' must be namespaced (ns:name)`);
    }
  }
  if (document.tool !== undefined && !["any", "pickaxe", "axe", "shovel", "hoe", "sword"].includes(String(document.tool))) {
    errors.push("block 'tool' must be any|pickaxe|axe|shovel|hoe|sword");
  }
  if (document.toolTier !== undefined && (!Number.isInteger(document.toolTier) || document.toolTier < 0 || document.toolTier > 4)) {
    errors.push("block 'toolTier' must be an integer in [0, 4]");
  }
  if (document.resistance !== undefined && (typeof document.resistance !== "number" || document.resistance < 0)) {
    errors.push("block 'resistance' cannot be negative");
  }
  if (document.friction !== undefined && (typeof document.friction !== "number" || document.friction < 0 || document.friction > 1)) {
    errors.push("block 'friction' must be a number in 0..1");
  }
  if (document.bounciness !== undefined && (typeof document.bounciness !== "number" || document.bounciness < 0 || document.bounciness > 1)) {
    errors.push("block 'bounciness' must be a number in 0..1");
  }
  if (document.density !== undefined && (typeof document.density !== "number" || document.density <= 0)) {
    errors.push("block 'density' must be positive");
  }
  // Declarative behavior reference (FALTANTES item 6), mirrored from the C++.
  if (document.behaviorId !== undefined && document.behaviorId !== "" && (!String(document.behaviorId).includes(":"))) {
    errors.push("block 'behaviorId' must be namespaced (ns:name)");
  }
}

function validateRegistryItem(document, errors) {
  if (typeof document.name !== "string" || !document.name) errors.push("item 'name' is required");
  if (typeof document.namespace !== "string" || !document.namespace) errors.push("item 'namespace' cannot be empty");
  for (const field of ["icon", "model"]) {
    if (document[field] !== undefined && typeof document[field] !== "string") errors.push(`item '${field}' must be a string`);
  }
  if (document.tags !== undefined && (!Array.isArray(document.tags) || document.tags.some((value) => typeof value !== "string"))) {
    errors.push("item 'tags' must be an array of strings");
  }
  // §2 item 8 — use/equipment/behavior components, mirrored from the C++
  // ItemRegistry validation (all-or-nothing, never clamp/guess).
  if (document.maxStack !== undefined && (!Number.isInteger(document.maxStack) || document.maxStack < 1 || document.maxStack > 64)) {
    errors.push("item 'maxStack' must be an integer in 1..64");
  }
  if (document.useCooldown !== undefined && (!Number.isInteger(document.useCooldown) || document.useCooldown < 0 || document.useCooldown > 60000)) {
    errors.push("item 'useCooldown' must be an integer in 0..60000");
  }
  if (document.useMode !== undefined && !["none", "instant", "continuous"].includes(document.useMode)) {
    errors.push("item 'useMode' must be none|instant|continuous");
  }
  if (document.equipSlot !== undefined && !["none", "hand", "offhand", "head", "chest", "legs", "feet"].includes(document.equipSlot)) {
    errors.push("item 'equipSlot' must be none|hand|offhand|head|chest|legs|feet");
  }
  if (document.attackDamage !== undefined && (typeof document.attackDamage !== "number" || document.attackDamage < 0 || document.attackDamage > 100)) {
    errors.push("item 'attackDamage' must be a number in 0..100");
  }
  if (document.armor !== undefined && (typeof document.armor !== "number" || document.armor < 0 || document.armor > 100)) {
    errors.push("item 'armor' must be a number in 0..100");
  }
  if (document.behaviorId !== undefined && document.behaviorId !== "" && (!String(document.behaviorId).includes(":"))) {
    errors.push("item 'behaviorId' must be namespaced (ns:name)");
  }
}

function validateRegistryFluid(document, errors) {
  if (typeof document.block !== "string" || !document.block) errors.push("fluid 'block' is required (the namespaced block name it drives)");
  if (document.viscosity !== undefined && (typeof document.viscosity !== "number" || document.viscosity < 0 || document.viscosity > 1)) errors.push("fluid 'viscosity' must be a number in 0..1");
  if (document.range !== undefined && (!Number.isInteger(document.range) || document.range < 1 || document.range > 7)) errors.push("fluid 'range' must be an integer in 1..7");
  if (document.damagePerTick !== undefined && (typeof document.damagePerTick !== "number" || document.damagePerTick < 0)) errors.push("fluid 'damagePerTick' cannot be negative");
}

function validateRegistryRecipe(document, errors) {
  if (typeof document.name !== "string" || !document.name) errors.push("recipe 'name' is required");
  if (typeof document.namespace !== "string" || !document.namespace) errors.push("recipe 'namespace' cannot be empty");
  if (document.station !== undefined && document.station !== "" && !String(document.station).includes(":")) errors.push("recipe 'station' must be namespaced (ns:name)");
  if (document.time !== undefined && (typeof document.time !== "number" || document.time < 0)) errors.push("recipe 'time' cannot be negative");
  if (document.energy !== undefined && (typeof document.energy !== "number" || document.energy < 0)) errors.push("recipe 'energy' cannot be negative");
  const inputs = Array.isArray(document.inputs) ? document.inputs : [];
  if (inputs.length === 0) errors.push("recipe needs at least one input");
  inputs.forEach((input, index) => {
    if (!input || typeof input !== "object" || Array.isArray(input)) return errors.push(`recipe input ${index} must be an object`);
    if ((!input.item || typeof input.item !== "string") && (!input.tag || typeof input.tag !== "string")) errors.push(`recipe input ${index} requires 'item' or 'tag'`);
    if (input.count !== undefined && (!Number.isInteger(input.count) || input.count < 1)) errors.push(`recipe input ${index} 'count' must be an integer >= 1`);
    if (input.alternatives !== undefined && (!Array.isArray(input.alternatives) || input.alternatives.some((value) => typeof value !== "string"))) errors.push(`recipe input ${index} 'alternatives' must be an array of strings`);
  });
  const outputs = [...(Array.isArray(document.outputs) ? document.outputs : []), ...(Array.isArray(document.byproducts) ? document.byproducts : [])];
  if (outputs.length === 0) errors.push("recipe needs at least one output");
  outputs.forEach((output, index) => {
    if (!output || typeof output !== "object" || Array.isArray(output)) return errors.push(`recipe output ${index} must be an object`);
    if (!output.item || typeof output.item !== "string") errors.push(`recipe output ${index} requires 'item'`);
    if (output.count !== undefined && (!Number.isInteger(output.count) || output.count < 1)) errors.push(`recipe output ${index} 'count' must be an integer >= 1`);
    if (output.chance !== undefined && (typeof output.chance !== "number" || output.chance <= 0 || output.chance > 1)) errors.push(`recipe output ${index} 'chance' must be in (0, 1]`);
  });
  for (const field of ["conditions", "tags"]) {
    if (document[field] !== undefined && (!Array.isArray(document[field]) || document[field].some((value) => typeof value !== "string"))) {
      errors.push(`recipe '${field}' must be an array of strings`);
    }
  }
}

function validateRegistryBiome(document, errors) {
  if (!Array.isArray(document.biomes) || document.biomes.length === 0) {
    errors.push("biome registry needs a non-empty 'biomes' array");
    return;
  }
  document.biomes.forEach((biome, index) => {
    if (!biome || typeof biome !== "object" || Array.isArray(biome)) return errors.push(`biome ${index} must be an object`);
    if (typeof biome.name !== "string" || !biome.name) errors.push(`biome ${index} has an empty name`);
    if (biome.engineBiomeIndex !== undefined && (!Number.isInteger(biome.engineBiomeIndex) || biome.engineBiomeIndex < 0 || biome.engineBiomeIndex > 255)) errors.push(`biome ${index} 'engineBiomeIndex' must be an integer in 0..255`);
    if (biome.climate !== undefined) {
      if (!biome.climate || typeof biome.climate !== "object" || Array.isArray(biome.climate)) return errors.push(`biome ${index} 'climate' must be an object`);
      for (const axis of CLIMATE_AXES) {
        const bounds = biome.climate[axis];
        if (bounds === undefined) continue;
        if (!Array.isArray(bounds) || bounds.length !== 2 || bounds.some((value) => typeof value !== "number")) {
          errors.push(`biome ${index} climate '${axis}' must be [min, max]`);
        } else if (bounds[0] > bounds[1]) {
          errors.push(`biome ${index} climate '${axis}' has inverted bounds`);
        }
      }
    }
    if (biome.surface !== undefined) {
      if (!Array.isArray(biome.surface)) return errors.push(`biome ${index} 'surface' must be an array`);
      biome.surface.forEach((rule, ruleIndex) => {
        if (!rule || typeof rule !== "object" || Array.isArray(rule)) return errors.push(`biome ${index} surface rule ${ruleIndex} must be an object`);
        if (!Number.isInteger(rule.blockId) || rule.blockId <= 0) errors.push(`biome ${index} surface rule ${ruleIndex} 'blockId' must be a non-zero integer`);
        if (rule.minDepth !== undefined && rule.maxDepth !== undefined && rule.maxDepth < rule.minDepth) errors.push(`biome ${index} surface rule ${ruleIndex} has inverted depth bounds`);
        if (rule.minHeight !== undefined && rule.maxHeight !== undefined && rule.maxHeight < rule.minHeight) errors.push(`biome ${index} surface rule ${ruleIndex} has inverted height bounds`);
        if (rule.minSlope !== undefined && (typeof rule.minSlope !== "number" || rule.minSlope < 0)) errors.push(`biome ${index} surface rule ${ruleIndex} 'minSlope' cannot be negative`);
      });
    }
  });
}

function validateRegistryStructure(document, errors) {
  if (!Number.isInteger(document.sampleWidth) || document.sampleWidth < 1 || !Number.isInteger(document.sampleHeight) || document.sampleHeight < 1) {
    errors.push("structure 'sampleWidth'/'sampleHeight' must be integers >= 1");
    return;
  }
  if (!Array.isArray(document.sample) || document.sample.length !== document.sampleWidth * document.sampleHeight || document.sample.some((value) => !Number.isInteger(value) || value < 0)) {
    errors.push("structure 'sample' must be an array of block ids with length sampleWidth * sampleHeight");
    return;
  }
  if (document.patternSize !== undefined && (!Number.isInteger(document.patternSize) || document.patternSize < 1 || document.patternSize > Math.min(document.sampleWidth, document.sampleHeight))) {
    errors.push("structure 'patternSize' must be an integer in 1..min(sampleWidth, sampleHeight)");
  }
  if (document.symmetry !== undefined && !STRUCTURE_SYMMETRIES.has(Number(document.symmetry))) errors.push("structure 'symmetry' must be 1, 2, 4 or 8");
  if (document.profiles !== undefined) {
    if (!Array.isArray(document.profiles)) return errors.push("structure 'profiles' must be an array");
    document.profiles.forEach((profile, index) => {
      if (!profile || typeof profile !== "object" || Array.isArray(profile)) return errors.push(`structure profile ${index} must be an object`);
      if (!Number.isInteger(profile.blockId) || profile.blockId <= 0) errors.push(`structure profile ${index} 'blockId' must be a non-zero integer`);
      if (!Array.isArray(profile.layers) || profile.layers.length === 0 || profile.layers.some((value) => !Number.isInteger(value))) errors.push(`structure profile ${index} 'layers' must be a non-empty array of block ids`);
    });
  }
}

// Structured validation mirroring the public C++ factories. Returns
// { valid, errors } with one human-readable diagnostic per rule breach.
export function validateRegistryDocument(kind, document) {
  const errors = [];
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["registry asset must be a JSON object"] };
  }
  if (document.version !== undefined && document.version !== 1) errors.push("registry asset 'version' must be 1");
  if (kind === "block") validateRegistryBlock(document, errors);
  else if (kind === "item") validateRegistryItem(document, errors);
  else if (kind === "fluid") validateRegistryFluid(document, errors);
  else if (kind === "recipe") validateRegistryRecipe(document, errors);
  else if (kind === "biome") validateRegistryBiome(document, errors);
  else if (kind === "structure") validateRegistryStructure(document, errors);
  else errors.push(`unsupported registry kind '${kind}'`);
  return { valid: errors.length === 0, errors };
}

function buildRegistryDocument(kind, args) {
  const value = (key, fallback, alternateKeys = []) => {
    if (args[key] !== undefined) return args[key];
    for (const alternate of alternateKeys) if (args[alternate] !== undefined) return args[alternate];
    return fallback;
  };
  const namespaced = (key) => (args[key] !== undefined ? String(args[key]) : "vulkancraft");
  if (kind === "block") {
    const document = {
      version: Number(value("version", 1)),
      namespace: namespaced("namespace"),
      name: String(args.name),
      class: String(value("class", "solid")),
      hardness: Number(value("hardness", 1)),
      lightEmission: Number(value("light_emission", 0)),
      lightAbsorption: Number(value("light_absorption", 1)),
      opaque: Boolean(value("opaque", true)),
      collidable: Boolean(value("collidable", true)),
      // Collision/selection shapes (FALTANTES item 2): strict enums — an
      // explicit unknown value is refused by the mirror validation. A
      // collisionShape of "none" wins over collidable:true in the C++ runtime
      // (the voxel raycast skips the block).
      collisionShape: String(value("collision_shape", "full")),
      selectionShape: String(value("selection_shape", "full")),
      tags: value("tags", [], "tags") ?? [],
      drops: value("drops", [], "drops") ?? []
    };
    if (args.id !== undefined) document.id = String(args.id);
    if (args.builtin_id !== undefined) document.builtinId = Number(args.builtin_id);
    if (args.color !== undefined) document.color = [...args.color];
    if (args.face_top !== undefined) document.faceTop = [...args.face_top];
    if (args.face_bottom !== undefined) document.faceBottom = [...args.face_bottom];
    if (args.face_side !== undefined) document.faceSide = [...args.face_side];
    if (args.occlusion !== undefined) document.occlusion = Boolean(args.occlusion);
    if (args.render_layer !== undefined) document.renderLayer = Number(args.render_layer);
    // Named states + versioned transitions (FALTANTES item 5).
    if (args.states !== undefined) {
      document.states = args.states.map((state) => {
        const entry = {
          name: String(state.name),
          color: Array.isArray(state.color) ? [...state.color] : [1, 1, 1, 1]
        };
        if (state.face_top !== undefined) entry.faceTop = [...state.face_top];
        if (state.face_bottom !== undefined) entry.faceBottom = [...state.face_bottom];
        if (state.face_side !== undefined) entry.faceSide = [...state.face_side];
        if (state.light_emission !== undefined) entry.lightEmission = Number(state.light_emission);
        return entry;
      });
    }
    if (args.transitions !== undefined) {
      document.transitions = args.transitions.map((transition) => ({
        from: String(transition.from ?? ""),
        to: String(transition.to ?? ""),
        trigger: String(transition.trigger ?? "")
      }));
    }
    // Inline fluid binding (FALTANTES item 7): the block declares the fluid
    // behavior it drives; no separate fluid asset needed.
    if (args.fluid !== undefined) {
      document.fluid = {
        viscosity: Number(args.fluid.viscosity ?? 0.5),
        density: Number(args.fluid.density ?? 1.0),
        range: Number(args.fluid.range ?? 7),
        tickInterval: Number(args.fluid.tick_interval ?? 0.08),
        source: Boolean(args.fluid.source ?? true),
        falling: Boolean(args.fluid.falling ?? true),
        evaporation: Boolean(args.fluid.evaporation ?? true),
        damagePerTick: Number(args.fluid.damage_per_tick ?? 0.0),
        compressible: Boolean(args.fluid.compressible ?? false)
      };
    }
    // Sound/particle/tool/resistance/physics component (FALTANTES item 4).
    if (args.sound_place !== undefined) document.soundPlace = String(args.sound_place);
    if (args.sound_break !== undefined) document.soundBreak = String(args.sound_break);
    if (args.sound_step !== undefined) document.soundStep = String(args.sound_step);
    if (args.sound_hit !== undefined) document.soundHit = String(args.sound_hit);
    if (args.particle_break !== undefined) document.particleBreak = String(args.particle_break);
    if (args.tool !== undefined) document.tool = String(args.tool);
    if (args.tool_tier !== undefined) document.toolTier = Number(args.tool_tier);
    if (args.resistance !== undefined) document.resistance = Number(args.resistance);
    if (args.friction !== undefined) document.friction = Number(args.friction);
    if (args.bounciness !== undefined) document.bounciness = Number(args.bounciness);
    if (args.density !== undefined) document.density = Number(args.density);
    // Declarative behavior reference (FALTANTES item 6).
    if (args.behavior !== undefined) document.behaviorId = String(args.behavior);
    return document;
  }
  if (kind === "item") {
    const document = {
      version: Number(value("version", 1)),
      namespace: namespaced("namespace"),
      name: String(args.name),
      maxStack: Number(value("max_stack", 64)),
      durability: Number(value("durability", 0)),
      icon: String(value("icon", "")),
      model: String(value("model", "")),
      tags: value("tags", [], "tags") ?? []
    };
    if (args.id !== undefined) document.id = String(args.id);
    if (args.use_cooldown !== undefined) document.useCooldown = Number(args.use_cooldown);
    if (args.use_mode !== undefined) document.useMode = String(args.use_mode);
    if (args.equip_slot !== undefined) document.equipSlot = String(args.equip_slot);
    if (args.attack_damage !== undefined) document.attackDamage = Number(args.attack_damage);
    if (args.armor !== undefined) document.armor = Number(args.armor);
    if (args.behavior !== undefined) document.behaviorId = String(args.behavior);
    return document;
  }
  if (kind === "fluid") {
    const document = {
      version: Number(value("version", 1)),
      block: String(args.block),
      viscosity: Number(value("viscosity", 0.5)),
      density: Number(value("density", 1)),
      range: Number(value("range", 7)),
      tickInterval: Number(value("tick_interval", 0.08)),
      source: Boolean(value("source", true)),
      falling: Boolean(value("falling", true)),
      evaporation: Boolean(value("evaporation", true)),
      damagePerTick: Number(value("damage_per_tick", 0)),
      compressible: Boolean(value("compressible", false))
    };
    if (args.id !== undefined) document.id = String(args.id);
    if (args.color !== undefined) document.color = [...args.color];
    return document;
  }
  if (kind === "recipe") {
    const normalizeInput = (input) => {
      const result = {};
      if (input.item !== undefined) result.item = String(input.item);
      if (input.tag !== undefined) result.tag = String(input.tag);
      if (input.count !== undefined) result.count = Number(input.count);
      if (input.alternatives !== undefined) result.alternatives = input.alternatives.map(String);
      return result;
    };
    const normalizeOutput = (output) => {
      const result = { item: String(output.item) };
      if (output.count !== undefined) result.count = Number(output.count);
      if (output.chance !== undefined) result.chance = Number(output.chance);
      return result;
    };
    const document = {
      version: Number(value("version", 1)),
      namespace: namespaced("namespace"),
      name: String(args.name),
      station: String(value("station", "")),
      time: Number(value("time", 1)),
      energy: Number(value("energy", 0)),
      fuel: String(value("fuel", "")),
      conditions: value("conditions", [], "conditions") ?? [],
      tags: value("tags", [], "tags") ?? [],
      inputs: (value("inputs", [], "inputs") ?? []).map(normalizeInput),
      outputs: (value("outputs", [], "outputs") ?? []).map(normalizeOutput)
    };
    if (args.id !== undefined) document.id = String(args.id);
    if (args.byproducts !== undefined) document.byproducts = args.byproducts.map(normalizeOutput);
    return document;
  }
  if (kind === "biome") {
    const biomes = (value("biomes", [], "biomes") ?? []).map((biome) => {
      const entry = { name: String(biome.name) };
      if (biome.engine_biome_index !== undefined) entry.engineBiomeIndex = Number(biome.engine_biome_index);
      if (biome.engineBiomeIndex !== undefined) entry.engineBiomeIndex = Number(biome.engineBiomeIndex);
      if (biome.climate !== undefined) {
        entry.climate = {};
        for (const axis of CLIMATE_AXES) {
          if (biome.climate[axis] !== undefined) entry.climate[axis] = [...biome.climate[axis]];
        }
      }
      if (biome.surface !== undefined) {
        entry.surface = biome.surface.map((rule) => {
          const result = { blockId: Number(rule.block_id ?? rule.blockId) };
          if (rule.min_depth !== undefined) result.minDepth = Number(rule.min_depth);
          if (rule.max_depth !== undefined) result.maxDepth = Number(rule.max_depth);
          if (rule.min_height !== undefined) result.minHeight = Number(rule.min_height);
          if (rule.max_height !== undefined) result.maxHeight = Number(rule.max_height);
          if (rule.min_slope !== undefined) result.minSlope = Number(rule.min_slope);
          return result;
        });
      }
      return entry;
    });
    return { version: Number(value("version", 1)), biomes };
  }
  if (kind === "structure") {
    const document = {
      version: Number(value("version", 1)),
      sampleWidth: Number(args.sample_width),
      sampleHeight: Number(args.sample_height),
      sample: (value("sample", [], "sample") ?? []).map(Number),
      patternSize: Number(value("pattern_size", 3)),
      symmetry: Number(value("symmetry", 1)),
      periodicOutput: Boolean(value("periodic_output", false)),
      ground: Boolean(value("ground", false)),
      seed: Number(value("seed", 0)),
      profiles: (value("profiles", [], "profiles") ?? []).map((profile) => ({
        blockId: Number(profile.block_id ?? profile.blockId),
        layers: (profile.layers ?? []).map(Number)
      }))
    };
    return document;
  }
  throw new Error(`unsupported registry kind '${kind}'`);
}

// --- vehicle assembly authoring (FALTANTES §17 item 12) ----------------------

const finiteNumber = (value) => typeof value === "number" && Number.isFinite(value);
const finiteVec = (value, size) => Array.isArray(value) && value.length === size && value.every(finiteNumber);
const finiteQuat = (value) => finiteVec(value, 4);

// Wheel component document — the exact camelCase keys VehicleAsset/
// BeamGraphAsset parse (mirrors emit_wheel in both SDK adapters).
function buildVehicleWheel(args, fallback = {}) {
  const wheel = {
    localPosition: finiteVec(args.localPosition, 3) ? [...args.localPosition] : (finiteVec(fallback.localPosition, 3) ? [...fallback.localPosition] : [0, 0, 0]),
    radius: args.radius !== undefined ? Number(args.radius) : (fallback.radius ?? 0.36),
    suspensionRestLength: args.suspension_rest_length !== undefined ? Number(args.suspension_rest_length) : (fallback.suspensionRestLength ?? 0.45),
    suspensionTravel: args.suspension_travel !== undefined ? Number(args.suspension_travel) : (fallback.suspensionTravel ?? 0.18),
    springStrength: args.spring_strength !== undefined ? Number(args.spring_strength) : (fallback.springStrength ?? 26000.0),
    damperStrength: args.damper_strength !== undefined ? Number(args.damper_strength) : (fallback.damperStrength ?? 3200.0),
    tireGrip: args.tire_grip !== undefined ? Number(args.tire_grip) : (fallback.tireGrip ?? 1.35),
    maxDriveForce: args.max_drive_force !== undefined ? Number(args.max_drive_force) : (fallback.maxDriveForce ?? 4200.0),
    maxBrakeForce: args.max_brake_force !== undefined ? Number(args.max_brake_force) : (fallback.maxBrakeForce ?? 6000.0),
    maxSteerAngle: args.max_steer_angle !== undefined ? Number(args.max_steer_angle) : (fallback.maxSteerAngle ?? 0.55),
    steering: args.steering !== undefined ? Boolean(args.steering) : (fallback.steering ?? false),
    driven: args.driven !== undefined ? Boolean(args.driven) : (fallback.driven ?? true)
  };
  return wheel;
}

function buildVehiclePower(args) {
  const power = {};
  const fuel = args.fuel ?? {};
  power.fuel = {
    capacity: Number(fuel.capacity ?? 0),
    initialLevel: Number(fuel.initial_level ?? fuel.initialLevel ?? 1.0),
    burnPerSecond: Number(fuel.burn_per_second ?? fuel.burnPerSecond ?? 0.05),
    idleBurnPerSecond: Number(fuel.idle_burn_per_second ?? fuel.idleBurnPerSecond ?? 0.0),
    minLevelToRun: Number(fuel.min_level_to_run ?? fuel.minLevelToRun ?? 0.0)
  };
  const energy = args.energy ?? {};
  power.energy = {
    capacity: Number(energy.capacity ?? 0),
    initialCharge: Number(energy.initial_charge ?? energy.initialCharge ?? 1.0),
    drawPerSecond: Number(energy.draw_per_second ?? energy.drawPerSecond ?? 0.0),
    regenPerSecond: Number(energy.regen_per_second ?? energy.regenPerSecond ?? 0.0),
    minChargeToRun: Number(energy.min_charge_to_run ?? energy.minChargeToRun ?? 0.0)
  };
  const controls = args.controls ?? {};
  power.controls = {
    throttleDeadzone: Number(controls.throttle_deadzone ?? controls.throttleDeadzone ?? 0.0),
    throttleSensitivity: Number(controls.throttle_sensitivity ?? controls.throttleSensitivity ?? 1.0),
    throttleInvert: Boolean(controls.throttle_invert ?? controls.throttleInvert ?? false),
    steeringDeadzone: Number(controls.steering_deadzone ?? controls.steeringDeadzone ?? 0.0),
    steeringSensitivity: Number(controls.steering_sensitivity ?? controls.steeringSensitivity ?? 1.0),
    steeringInvert: Boolean(controls.steering_invert ?? controls.steeringInvert ?? false),
    brakeDeadzone: Number(controls.brake_deadzone ?? controls.brakeDeadzone ?? 0.0),
    brakeSensitivity: Number(controls.brake_sensitivity ?? controls.brakeSensitivity ?? 1.0)
  };
  return power;
}

function buildVehicleSeats(args) {
  return (args.seats ?? []).map((seat) => ({
    name: String(seat.name ?? ""),
    localPosition: finiteVec(seat.localPosition, 3) ? [...seat.localPosition] : (finiteVec(seat.local_position, 3) ? [...seat.local_position] : [0, 0, 0]),
    exitOffset: finiteVec(seat.exitOffset, 3) ? [...seat.exitOffset] : (finiteVec(seat.exit_offset, 3) ? [...seat.exit_offset] : [0, 1.2, 1.0])
  }));
}

// Builds the exact camelCase document the public C++ factory parses.
function buildVehicleDocument(kind, args) {
  if (kind === "vehicle") {
    const chassisArgs = args.chassis ?? {};
    const document = {
      name: String(args.name),
      version: args.version !== undefined ? Number(args.version) : 1,
      provider: String(args.provider ?? "jolt"),
      kind: String(args.vehicle_kind ?? "wheeled"),
      position: finiteVec(args.position, 3) ? [...args.position] : [0, 0, 0],
      rotation: finiteVec(args.rotation, 4) ? [...args.rotation] : [0, 0, 0, 1],
      chassis: {
        shape: String(chassisArgs.shape ?? "box"),
        halfExtents: finiteVec(chassisArgs.halfExtents, 3) ? [...chassisArgs.halfExtents] : (finiteVec(chassisArgs.half_extents, 3) ? [...chassisArgs.half_extents] : [0.9, 0.35, 0.56]),
        radius: Number(chassisArgs.radius ?? 0.5),
        halfHeight: Number(chassisArgs.half_height ?? chassisArgs.halfHeight ?? 0.65),
        mass: Number(chassisArgs.mass ?? 1200.0),
        friction: Number(chassisArgs.friction ?? 0.55),
        restitution: Number(chassisArgs.restitution ?? 0.05)
      },
      wheels: (args.wheels ?? []).map((wheel) => buildVehicleWheel(wheel)),
      drivetrain: {
        engineMaxTorque: Number(args.drivetrain?.engine_max_torque ?? args.drivetrain?.engineMaxTorque ?? 0),
        engineMinRPM: Number(args.drivetrain?.engine_min_rpm ?? args.drivetrain?.engineMinRPM ?? 1000.0),
        engineMaxRPM: Number(args.drivetrain?.engine_max_rpm ?? args.drivetrain?.engineMaxRPM ?? 6000.0),
        differentialRatio: Number(args.drivetrain?.differential_ratio ?? args.drivetrain?.differentialRatio ?? 3.42),
        gearRatios: (args.drivetrain?.gear_ratios ?? args.drivetrain?.gearRatios ?? [2.66, 1.78, 1.3, 1.0, 0.74]).map(Number)
      },
      propulsion: (args.propulsion ?? []).map((module) => ({
        kind: String(module.kind ?? "thruster"),
        localPosition: finiteVec(module.localPosition, 3) ? [...module.localPosition] : (finiteVec(module.local_position, 3) ? [...module.local_position] : [0, 0, 0]),
        axis: finiteVec(module.axis, 3) ? [...module.axis] : [0, 1, 0],
        maxForce: Number(module.max_force ?? module.maxForce ?? 1000.0),
        area: Number(module.area ?? 4.0),
        liftCoefficient: Number(module.lift_coefficient ?? module.liftCoefficient ?? 0.8),
        fluidDensity: Number(module.fluid_density ?? module.fluidDensity ?? 1000.0),
        waterLevel: Number(module.water_level ?? module.waterLevel ?? 0.0)
      })),
      seats: buildVehicleSeats(args),
      power: buildVehiclePower(args)
    };
    if (args.id !== undefined) document.id = String(args.id);
    return document;
  }
  if (kind === "beam") {
    const document = {
      name: String(args.name),
      version: args.version !== undefined ? Number(args.version) : 1,
      provider: String(args.provider ?? "jolt"),
      position: finiteVec(args.position, 3) ? [...args.position] : [0, 0, 0],
      rotation: finiteVec(args.rotation, 4) ? [...args.rotation] : [0, 0, 0, 1],
      mass: Number(args.mass ?? 1200.0),
      nodes: (args.nodes ?? []).map((node) => ({
        position: finiteVec(node.position, 3) ? [...node.position] : [0, 0, 0],
        fixed: Boolean(node.fixed ?? false)
      })),
      beams: (args.beams ?? []).map((beam) => ({
        a: Number(beam.a ?? 0),
        b: Number(beam.b ?? 0),
        stiffness: Number(beam.stiffness ?? 0.9)
      })),
      wheels: (args.wheels ?? []).map((mount) => ({
        node: Number(mount.node ?? 0),
        steering: Boolean(mount.steering ?? true),
        driven: Boolean(mount.driven ?? true),
        wheel: buildVehicleWheel(mount.wheel ?? {}, mount.wheel ?? {})
      })),
      seats: buildVehicleSeats(args),
      power: buildVehiclePower(args),
      solver: {
        substeps: Number(args.solver?.substeps ?? 2),
        solverIterations: Number(args.solver?.solver_iterations ?? args.solver?.solverIterations ?? 8),
        stiffness: Number(args.solver?.stiffness ?? 0.8),
        damping: Number(args.solver?.damping ?? 0.1),
        gravity: finiteVec(args.solver?.gravity, 3) ? [...args.solver.gravity] : [0, -9.81, 0]
      }
    };
    if (args.id !== undefined) document.id = String(args.id);
    return document;
  }
  throw new Error(`unsupported vehicle kind '${kind}'`);
}

// Structured validation mirroring the public C++ factories (VehicleAsset::
// load_from_json / BeamGraphAsset::load_from_json — all-or-nothing, never
// clamp or guess). Returns { valid, errors }.
export function validateVehicleDocument(kind, document) {
  const errors = [];
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["vehicle asset must be a JSON object"] };
  }
  if (document.version !== undefined && document.version !== 1) errors.push("vehicle asset 'version' must be 1");
  if (typeof document.name !== "string" || !document.name) errors.push("vehicle asset 'name' is required");
  // Physics provider (items 5/6): jolt|chrono|jsbsim — the document carries
  // the asset's choice; chrono/jsbsim are refused at CREATION by the C++
  // factory (never a silent fallback), which the mirror reflects by keeping
  // the enum strict but NOT rejecting the document (it must round-trip).
  if (document.provider !== undefined && !["jolt", "chrono", "jsbsim"].includes(String(document.provider))) {
    errors.push("vehicle asset 'provider' must be jolt|chrono|jsbsim");
  }
  if (!finiteVec(document.position, 3)) errors.push("vehicle asset 'position' must be a finite [x,y,z] array");
  if (!finiteQuat(document.rotation)) errors.push("vehicle asset 'rotation' must be a finite [x,y,z,w] array");
  const failPower = (label, message) => errors.push(`vehicle asset ${label}: ${message}`);
  const validateFuel = (fuel, label) => {
    if (!fuel || typeof fuel !== "object") return failPower(label, "fuel must be an object");
    for (const key of ["capacity", "initialLevel", "burnPerSecond", "idleBurnPerSecond", "minLevelToRun"]) {
      if (!finiteNumber(fuel[key])) failPower(label, `fuel '${key}' must be finite`);
    }
    if (finiteNumber(fuel.capacity) && fuel.capacity < 0) failPower(label, "fuel capacity must be >= 0");
    if (finiteNumber(fuel.burnPerSecond) && fuel.burnPerSecond < 0) failPower(label, "fuel burnPerSecond must be >= 0");
    if (finiteNumber(fuel.idleBurnPerSecond) && fuel.idleBurnPerSecond < 0) failPower(label, "fuel idleBurnPerSecond must be >= 0");
    if (finiteNumber(fuel.initialLevel) && (fuel.initialLevel < 0 || fuel.initialLevel > 1)) failPower(label, "fuel initialLevel must be in [0, 1]");
    if (finiteNumber(fuel.minLevelToRun) && (fuel.minLevelToRun < 0 || fuel.minLevelToRun > 1)) failPower(label, "fuel minLevelToRun must be in [0, 1]");
  };
  const validateEnergy = (energy, label) => {
    if (!energy || typeof energy !== "object") return failPower(label, "energy must be an object");
    for (const key of ["capacity", "initialCharge", "drawPerSecond", "regenPerSecond", "minChargeToRun"]) {
      if (!finiteNumber(energy[key])) failPower(label, `energy '${key}' must be finite`);
    }
    if (finiteNumber(energy.capacity) && energy.capacity < 0) failPower(label, "energy capacity must be >= 0");
    if (finiteNumber(energy.drawPerSecond) && energy.drawPerSecond < 0) failPower(label, "energy drawPerSecond must be >= 0");
    if (finiteNumber(energy.regenPerSecond) && energy.regenPerSecond < 0) failPower(label, "energy regenPerSecond must be >= 0");
    if (finiteNumber(energy.initialCharge) && (energy.initialCharge < 0 || energy.initialCharge > 1)) failPower(label, "energy initialCharge must be in [0, 1]");
    if (finiteNumber(energy.minChargeToRun) && (energy.minChargeToRun < 0 || energy.minChargeToRun > 1)) failPower(label, "energy minChargeToRun must be in [0, 1]");
  };
  const validateControls = (controls, label) => {
    if (!controls || typeof controls !== "object") return failPower(label, "controls must be an object");
    for (const key of ["throttleDeadzone", "throttleSensitivity", "steeringDeadzone", "steeringSensitivity", "brakeDeadzone", "brakeSensitivity"]) {
      if (!finiteNumber(controls[key])) failPower(label, `controls '${key}' must be finite`);
    }
    for (const key of ["throttleDeadzone", "steeringDeadzone", "brakeDeadzone"]) {
      if (finiteNumber(controls[key]) && (controls[key] < 0 || controls[key] > 1)) failPower(label, `controls '${key}' must be in [0, 1]`);
    }
    for (const key of ["throttleSensitivity", "steeringSensitivity", "brakeSensitivity"]) {
      if (finiteNumber(controls[key]) && !(controls[key] > 0)) failPower(label, `controls '${key}' must be > 0`);
    }
  };
  const validateWheel = (wheel, label) => {
    if (!wheel || typeof wheel !== "object" || Array.isArray(wheel)) return errors.push(`vehicle asset ${label} must be an object`);
    if (!finiteVec(wheel.localPosition, 3)) errors.push(`vehicle asset ${label} 'localPosition' must be a finite [x,y,z] array`);
    for (const key of ["radius", "suspensionRestLength", "suspensionTravel", "springStrength", "damperStrength", "tireGrip", "maxDriveForce", "maxBrakeForce", "maxSteerAngle"]) {
      if (!finiteNumber(wheel[key])) errors.push(`vehicle asset ${label} '${key}' must be finite`);
    }
    if (finiteNumber(wheel.radius) && !(wheel.radius > 0)) errors.push(`vehicle asset ${label} 'radius' must be > 0`);
    if (finiteNumber(wheel.suspensionRestLength) && !(wheel.suspensionRestLength > 0)) errors.push(`vehicle asset ${label} 'suspensionRestLength' must be > 0`);
    for (const key of ["suspensionTravel", "springStrength", "damperStrength", "tireGrip", "maxDriveForce", "maxBrakeForce", "maxSteerAngle"]) {
      if (finiteNumber(wheel[key]) && wheel[key] < 0) errors.push(`vehicle asset ${label} '${key}' must be >= 0`);
    }
  };
  const validateSeats = (seats) => {
    (Array.isArray(seats) ? seats : []).forEach((seat, index) => {
      if (!seat || typeof seat !== "object" || Array.isArray(seat)) return errors.push(`vehicle asset seat ${index} must be an object`);
      if (!finiteVec(seat.localPosition, 3)) errors.push(`vehicle asset seat ${index} 'localPosition' must be a finite [x,y,z] array`);
      if (!finiteVec(seat.exitOffset, 3)) errors.push(`vehicle asset seat ${index} 'exitOffset' must be a finite [x,y,z] array`);
    });
  };
  const validatePower = (power) => {
    if (power === undefined) return;
    if (!power || typeof power !== "object" || Array.isArray(power)) return failPower("", "power must be an object");
    if (power.fuel !== undefined) validateFuel(power.fuel, "power");
    if (power.energy !== undefined) validateEnergy(power.energy, "power");
    if (power.controls !== undefined) validateControls(power.controls, "power");
  };
  validatePower(document.power);
  if (kind === "vehicle") {
    if (!VEHICLE_KIND_ENUM.has(String(document.kind))) errors.push("vehicle asset 'kind' must be wheeled|motorcycle|tracked");
    const chassis = document.chassis;
    if (!chassis || typeof chassis !== "object" || Array.isArray(chassis)) return { valid: errors.length === 0, errors };
    if (!VEHICLE_SHAPES.has(String(chassis.shape))) errors.push("vehicle asset chassis 'shape' must be box|sphere|capsule");
    for (const key of ["mass", "friction", "restitution"]) {
      if (!finiteNumber(chassis[key])) errors.push(`vehicle asset chassis '${key}' must be finite`);
    }
    if (finiteNumber(chassis.mass) && !(chassis.mass > 0)) errors.push("vehicle asset chassis 'mass' must be > 0");
    if (finiteNumber(chassis.friction) && chassis.friction < 0) errors.push("vehicle asset chassis 'friction' must be >= 0");
    if (finiteNumber(chassis.restitution) && chassis.restitution < 0) errors.push("vehicle asset chassis 'restitution' must be >= 0");
    if (String(chassis.shape) === "box" && !(finiteVec(chassis.halfExtents, 3) && chassis.halfExtents.every((value) => value > 0))) {
      errors.push("vehicle asset chassis 'halfExtents' must be [x,y,z] with all values > 0 for a box");
    }
    if (String(chassis.shape) === "sphere" && (!finiteNumber(chassis.radius) || !(chassis.radius > 0))) {
      errors.push("vehicle asset chassis 'radius' must be > 0 for a sphere");
    }
    if (String(chassis.shape) === "capsule") {
      if (!finiteNumber(chassis.radius) || !(chassis.radius > 0)) errors.push("vehicle asset chassis 'radius' must be > 0 for a capsule");
      if (!finiteNumber(chassis.halfHeight) || !(chassis.halfHeight > 0)) errors.push("vehicle asset chassis 'halfHeight' must be > 0 for a capsule");
    }
    if (!Array.isArray(document.wheels) || document.wheels.length === 0) {
      errors.push("vehicle asset needs at least one wheel");
    } else {
      document.wheels.forEach((wheel, index) => validateWheel(wheel, `wheel ${index}`));
    }
    const drivetrain = document.drivetrain;
    if (!drivetrain || typeof drivetrain !== "object") {
      errors.push("vehicle asset 'drivetrain' must be an object");
    } else {
      if (!finiteNumber(drivetrain.engineMaxTorque)) errors.push("vehicle asset drivetrain 'engineMaxTorque' must be finite");
      if (finiteNumber(drivetrain.engineMaxTorque) && drivetrain.engineMaxTorque < 0) errors.push("vehicle asset drivetrain 'engineMaxTorque' must be >= 0");
      if (!finiteNumber(drivetrain.engineMinRPM) || !(drivetrain.engineMinRPM > 0)) errors.push("vehicle asset drivetrain 'engineMinRPM' must be > 0");
      if (!finiteNumber(drivetrain.engineMaxRPM) || !(drivetrain.engineMaxRPM > drivetrain.engineMinRPM)) errors.push("vehicle asset drivetrain 'engineMaxRPM' must exceed engineMinRPM");
      if (!finiteNumber(drivetrain.differentialRatio) || !(drivetrain.differentialRatio > 0)) errors.push("vehicle asset drivetrain 'differentialRatio' must be > 0");
      if (!Array.isArray(drivetrain.gearRatios) || drivetrain.gearRatios.length === 0) {
        errors.push("vehicle asset drivetrain needs at least one gear ratio");
      } else {
        drivetrain.gearRatios.forEach((ratio, index) => {
          if (!finiteNumber(ratio) || !(ratio > 0)) errors.push(`vehicle asset drivetrain gear ratio ${index} must be finite and > 0`);
        });
      }
    }
    (Array.isArray(document.propulsion) ? document.propulsion : []).forEach((module, index) => {
      if (!module || typeof module !== "object" || Array.isArray(module)) return errors.push(`vehicle asset propulsion module ${index} must be an object`);
      if (!PROPULSION_KINDS.has(String(module.kind))) errors.push(`vehicle asset propulsion module ${index} 'kind' must be wing|thruster|buoyancy`);
      if (!finiteVec(module.localPosition, 3)) errors.push(`vehicle asset propulsion module ${index} 'localPosition' must be a finite [x,y,z] array`);
      if (!finiteVec(module.axis, 3) || module.axis.every((value) => value === 0)) errors.push(`vehicle asset propulsion module ${index} 'axis' must be a finite non-zero [x,y,z] array`);
      for (const key of ["maxForce", "area", "liftCoefficient", "fluidDensity"]) {
        if (!finiteNumber(module[key]) || module[key] < 0) errors.push(`vehicle asset propulsion module ${index} '${key}' must be >= 0`);
      }
    });
    validateSeats(document.seats);
  } else if (kind === "beam") {
    if (!finiteNumber(document.mass) || !(document.mass > 0)) errors.push("beam asset 'mass' must be finite and > 0");
    if (!Array.isArray(document.nodes) || document.nodes.length === 0) {
      errors.push("beam asset needs at least one node");
    } else {
      if (document.nodes.length > 4096) errors.push("beam asset node count exceeds 4096");
      document.nodes.forEach((node, index) => {
        if (!node || typeof node !== "object" || Array.isArray(node)) return errors.push(`beam asset node ${index} must be an object`);
        if (!finiteVec(node.position, 3)) errors.push(`beam asset node ${index} 'position' must be a finite [x,y,z] array`);
      });
    }
    if (!Array.isArray(document.beams) || document.beams.length === 0) {
      errors.push("beam asset needs at least one beam");
    } else {
      document.beams.forEach((beam, index) => {
        if (!beam || typeof beam !== "object" || Array.isArray(beam)) return errors.push(`beam asset beam ${index} must be an object`);
        if (!Number.isInteger(beam.a) || !Number.isInteger(beam.b) || beam.a < 0 || beam.b < 0 || beam.a >= document.nodes.length || beam.b >= document.nodes.length) {
          errors.push(`beam asset beam ${index} references an invalid node`);
        }
        if (!finiteNumber(beam.stiffness) || !(beam.stiffness > 0) || beam.stiffness > 1) errors.push(`beam asset beam ${index} 'stiffness' must be in (0, 1]`);
      });
    }
    (Array.isArray(document.wheels) ? document.wheels : []).forEach((mount, index) => {
      if (!mount || typeof mount !== "object" || Array.isArray(mount)) return errors.push(`beam asset wheel mount ${index} must be an object`);
      if (!Number.isInteger(mount.node) || mount.node < 0 || mount.node >= document.nodes.length) errors.push(`beam asset wheel mount ${index} mounts an invalid node`);
      validateWheel(mount.wheel, `wheel mount ${index}`);
    });
    validateSeats(document.seats);
    const solver = document.solver;
    if (solver !== undefined) {
      if (!solver || typeof solver !== "object" || Array.isArray(solver)) errors.push("beam asset 'solver' must be an object");
      else {
        if (!Number.isInteger(solver.substeps) || solver.substeps < 1 || solver.substeps > 16) errors.push("beam asset solver 'substeps' must be an integer in [1, 16]");
        if (!Number.isInteger(solver.solverIterations) || solver.solverIterations < 1 || solver.solverIterations > 64) errors.push("beam asset solver 'solverIterations' must be an integer in [1, 64]");
        if (!finiteNumber(solver.stiffness) || !(solver.stiffness > 0) || solver.stiffness > 1) errors.push("beam asset solver 'stiffness' must be in (0, 1]");
        if (!finiteNumber(solver.damping) || solver.damping < 0 || solver.damping >= 1) errors.push("beam asset solver 'damping' must be in [0, 1)");
        if (!finiteVec(solver.gravity, 3)) errors.push("beam asset solver 'gravity' must be a finite [x,y,z] array");
        else if (Math.hypot(...solver.gravity) > 1000) errors.push("beam asset solver 'gravity' magnitude must be <= 1000");
      }
    }
  } else {
    errors.push(`unsupported vehicle kind '${kind}'`);
  }
  return { valid: errors.length === 0, errors };
}

function vehicleDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorVehicleAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const kind = String(args.kind);
  if (!VEHICLE_KINDS.includes(kind)) throw new Error(`unsupported vehicle kind '${kind}' (supported: ${VEHICLE_KINDS.join(", ")})`);
  const name = assetName(args.name);
  const document = buildVehicleDocument(kind, args);
  const validation = validateVehicleDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: "vehicle asset fails public-contract validation; nothing was written"
    };
  }
  const file = path.join(project.vehicles, `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? vehicleDiff(previous, document) : null;
  const relative = path.relative(project.root, file).replaceAll(path.sep, "/");
  if (args.dry_run) {
    return {
      dry_run: true,
      would_write: relative,
      project: project.project,
      kind,
      name,
      document,
      diagnostics: [],
      diff
    };
  }
  if (previous && !args.update) throw new Error(`vehicle asset '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
  atomicWriteJson(file, document);
  return {
    created: !previous,
    updated: Boolean(previous),
    project: project.project,
    kind,
    name,
    path: relative,
    sha256: sha256File(file),
    diagnostics: [],
    diff,
    rollback: previous ? { document: previous, hint: "re-author with update: true and this document to restore" } : undefined
  };
}

// Reads every vehicle assembly asset of a project with its structural
// diagnostics (mirrors readRegistryAssets).
function readVehicleAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.vehicles)) return assets;
  for (const fileName of fs.readdirSync(project.vehicles).filter((file) => file.endsWith(".json")).sort()) {
    const file = path.join(project.vehicles, fileName);
    const baseName = fileName.replace(/\.json$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    // Which factory parses this document? A document with a 'nodes'/'beams'
    // array is a BeamGraphAsset; otherwise it is a VehicleAsset. Unknown
    // shapes are flagged but not guessed.
    let kind = null;
    if (document && typeof document === "object" && !Array.isArray(document)) {
      kind = Array.isArray(document.nodes) || Array.isArray(document.beams) ? "beam" : "vehicle";
    }
    if (document && kind) diagnostics.push(...validateVehicleDocument(kind, document).errors);
    else if (document) diagnostics.push("vehicle asset cannot be classified (no nodes/beams and no vehicle fields)");
    assets.push({
      kind: kind ?? "unknown",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectVehicleAssets(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readVehicleAssets(project);
  return { project: project.project, vehicle_assets: assets, count: assets.length };
}

// ---- abilities (FALTANTES §19 — data-driven, mirror of AbilitySystem.cpp) ----

// Builds one effect object from author args. snake_case args map to the
// camelCase DOCUMENT keys the C++ factory parses (see ABILITY_DOC_KEYS); the
// defaults match AbilitySystem.cpp parse_effect exactly.
function buildAbilityEffect(effect) {
  const built = { type: String(effect.type ?? "damage") };
  switch (built.type) {
    case "damage":
    case "heal":
      built.amount = Number(effect.amount ?? 0);
      break;
    case "impulse":
      built.force = Number(effect.force ?? 0);
      break;
    case "telekinesis":
      built.holdOffset = finiteVec(effect.holdOffset, 3) ? effect.holdOffset.map(Number) : (finiteVec(effect.hold_offset, 3) ? effect.hold_offset.map(Number) : [0, 1.5, 0]);
      built.grabForce = Number(effect.grab_force ?? effect.grabForce ?? 240);
      built.durationSeconds = Number(effect.duration_seconds ?? effect.durationSeconds ?? 0);
      break;
    case "flight":
      built.thrust = Number(effect.thrust ?? 320);
      built.durationSeconds = Number(effect.duration_seconds ?? effect.durationSeconds ?? 0);
      break;
    case "blockEdit":
      built.min = finiteVec(effect.min, 3) ? effect.min.map(Number) : [-1, -1, -1];
      built.max = finiteVec(effect.max, 3) ? effect.max.map(Number) : [1, 1, 1];
      built.blockId = Number(effect.block_id ?? effect.blockId ?? 1);
      built.relative = effect.relative !== undefined ? Boolean(effect.relative) : true;
      break;
    case "periodic":
      built.intervalSeconds = Number(effect.interval_seconds ?? effect.intervalSeconds ?? 0.5);
      built.ticks = Number(effect.ticks ?? 4);
      if (effect.subEffect !== undefined && effect.subEffect !== null) {
        built.subEffect = buildAbilityEffect(effect.subEffect);
      }
      break;
  }
  for (const [argKey, docKey] of [
    ["cast_animation", "castAnimation"], ["particle_effect", "particleEffect"], ["sound_effect", "soundEffect"],
    ["castAnimation", "castAnimation"], ["particleEffect", "particleEffect"], ["soundEffect", "soundEffect"]
  ]) {
    if (effect[argKey] !== undefined) built[docKey] = String(effect[argKey]);
  }
  return built;
}

// Builds the full ability document from author args. Defaults match
// AbilitySystem.cpp load_from_json exactly (all-or-nothing, never clamped).
function buildAbilityDocument(kind, args) {
  if (kind !== "ability") throw new Error(`unsupported ability kind '${kind}'`);
  const document = {
    name: String(args.name),
    version: args.version !== undefined ? Number(args.version) : 1,
    cooldownSeconds: Number(args.cooldown_seconds ?? args.cooldownSeconds ?? 0),
    cancelable: args.cancelable !== undefined ? Boolean(args.cancelable) : true,
    interruptible: args.interruptible !== undefined ? Boolean(args.interruptible) : true,
    attributes: (args.attributes ?? []).map((attribute) => ({ name: String(attribute.name ?? ""), value: Number(attribute.value ?? 0) })),
    tags: (args.tags ?? []).map(String),
    cost: { resource: String(args.cost?.resource ?? ""), amount: Number(args.cost?.amount ?? 0) },
    conditions: (args.conditions ?? []).map((condition) => ({
      kind: String(condition.kind ?? "ownerTag"),
      tag: String(condition.tag ?? ""),
      attribute: String(condition.attribute ?? ""),
      minValue: Number(condition.min_value ?? condition.minValue ?? 0),
      maxDistance: Number(condition.max_distance ?? condition.maxDistance ?? 0)
    })),
    targeting: {
      mode: String(args.targeting?.mode ?? "self"),
      range: Number(args.targeting?.range ?? 0),
      radius: Number(args.targeting?.radius ?? 0)
    },
    effects: (args.effects ?? []).map(buildAbilityEffect)
  };
  if (args.id !== undefined) document.id = String(args.id);
  return document;
}

// Structured validation mirroring the public C++ factory (AbilityDefinition::
// load_from_json — all-or-nothing, never clamp or guess). Returns { valid, errors }.
function validateAbilityEffect(effect, index, errors) {
  const fail = (message) => errors.push(`ability asset effect ${index}: ${message}`);
  if (!effect || typeof effect !== "object" || Array.isArray(effect)) return fail("must be an object");
  const type = String(effect.type ?? "");
  if (!ABILITY_EFFECT_TYPES.has(type)) return fail(`unknown type '${type}' (must be damage|heal|impulse|telekinesis|flight|blockEdit|periodic)`);
  switch (type) {
    case "damage":
    case "heal":
      if (!finiteNumber(effect.amount) || effect.amount < 0) fail("'amount' must be finite and >= 0");
      break;
    case "impulse":
      if (!finiteNumber(effect.force) || effect.force < 0) fail("'force' must be finite and >= 0");
      break;
    case "telekinesis":
      if (!finiteVec(effect.holdOffset, 3)) fail("'holdOffset' must be a finite [x,y,z] array");
      if (!finiteNumber(effect.grabForce) || effect.grabForce < 0) fail("'grabForce' must be finite and >= 0");
      if (!finiteNumber(effect.durationSeconds) || effect.durationSeconds < 0) fail("'durationSeconds' must be finite and >= 0");
      break;
    case "flight":
      if (!finiteNumber(effect.thrust) || effect.thrust < 0) fail("'thrust' must be finite and >= 0");
      if (!finiteNumber(effect.durationSeconds) || effect.durationSeconds < 0) fail("'durationSeconds' must be finite and >= 0");
      break;
    case "blockEdit": {
      if (!finiteVec(effect.min, 3) || !finiteVec(effect.max, 3) ||
          effect.min.some((value) => !Number.isInteger(value)) || effect.max.some((value) => !Number.isInteger(value))) {
        fail("'min'/'max' must be [x,y,z] integer arrays");
      } else {
        const [dx, dy, dz] = [0, 1, 2].map((axis) => effect.max[axis] - effect.min[axis] + 1);
        if (dx <= 0 || dy <= 0 || dz <= 0) fail("'min'/'max' must satisfy max >= min per axis");
        else if (dx * dy * dz > ABILITY_MAX_BLOCK_EDIT_VOLUME) fail(`blockEdit box exceeds the ${ABILITY_MAX_BLOCK_EDIT_VOLUME} cell volume limit`);
      }
      if (!finiteNumber(effect.blockId) || effect.blockId < 0) fail("'blockId' must be finite and >= 0");
      if (effect.relative !== undefined && typeof effect.relative !== "boolean") fail("'relative' must be a boolean");
      break;
    }
    case "periodic":
      if (!finiteNumber(effect.intervalSeconds) || effect.intervalSeconds <= 0) fail("'intervalSeconds' must be finite and > 0");
      if (!Number.isInteger(effect.ticks) || effect.ticks < 1) fail("'ticks' must be an integer >= 1");
      if (effect.subEffect !== undefined) validateAbilityEffect(effect.subEffect, `${index}.subEffect`, errors);
      break;
  }
  for (const key of ABILITY_EFFECT_HOOKS) {
    if (effect[key] !== undefined && typeof effect[key] !== "string") fail(`'${key}' must be a string`);
  }
}

export function validateAbilityDocument(kind, document) {
  const errors = [];
  const fail = (message) => errors.push(`ability asset ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["ability asset must be a JSON object"] };
  }
  if (kind !== "ability") return { valid: false, errors: [`unsupported ability kind '${kind}'`] };
  if (document.version !== undefined && document.version !== 1) fail("'version' must be 1");
  if (typeof document.name !== "string" || !document.name) fail("'name' is required");
  if (!finiteNumber(document.cooldownSeconds) || document.cooldownSeconds < 0) fail("'cooldownSeconds' must be finite and >= 0");
  if (document.cancelable !== undefined && typeof document.cancelable !== "boolean") fail("'cancelable' must be a boolean");
  if (document.interruptible !== undefined && typeof document.interruptible !== "boolean") fail("'interruptible' must be a boolean");
  if (document.attributes !== undefined) {
    if (!Array.isArray(document.attributes)) fail("'attributes' must be an array");
    else document.attributes.forEach((attribute, index) => {
      if (!attribute || typeof attribute !== "object" || Array.isArray(attribute)) return fail(`attribute ${index} must be an object`);
      if (typeof attribute.name !== "string" || !attribute.name) fail(`attribute ${index} 'name' must be a non-empty string`);
      if (!finiteNumber(attribute.value)) fail(`attribute ${index} 'value' must be finite`);
    });
  }
  if (document.tags !== undefined && (!Array.isArray(document.tags) || document.tags.some((tag) => typeof tag !== "string"))) {
    fail("'tags' must be an array of strings");
  }
  if (document.cost !== undefined) {
    if (!document.cost || typeof document.cost !== "object" || Array.isArray(document.cost)) fail("'cost' must be an object");
    else if (!finiteNumber(document.cost.amount) || document.cost.amount < 0) fail("cost 'amount' must be finite and >= 0");
  }
  if (document.conditions !== undefined) {
    if (!Array.isArray(document.conditions)) fail("'conditions' must be an array");
    else document.conditions.forEach((condition, index) => {
      if (!condition || typeof condition !== "object" || Array.isArray(condition)) return fail(`condition ${index} must be an object`);
      const kind = String(condition.kind ?? "");
      if (!ABILITY_CONDITION_KINDS.has(kind)) return fail(`condition ${index} 'kind' must be ownerTag|targetTag|ownerAttribute|targetAttribute|distance`);
      if ((kind === "ownerTag" || kind === "targetTag") && (typeof condition.tag !== "string" || !condition.tag)) fail(`condition ${index} requires a 'tag'`);
      if ((kind === "ownerAttribute" || kind === "targetAttribute") && (typeof condition.attribute !== "string" || !condition.attribute)) fail(`condition ${index} requires an 'attribute'`);
      if (kind === "distance" && (!finiteNumber(condition.maxDistance) || condition.maxDistance < 0)) fail(`condition ${index} 'maxDistance' must be finite and >= 0`);
      if (condition.minValue !== undefined && !finiteNumber(condition.minValue)) fail(`condition ${index} 'minValue' must be finite`);
    });
  }
  if (document.targeting !== undefined) {
    if (!document.targeting || typeof document.targeting !== "object" || Array.isArray(document.targeting)) fail("'targeting' must be an object");
    else {
      if (!ABILITY_TARGET_MODES.has(String(document.targeting.mode ?? "self"))) fail("targeting 'mode' must be self|direction|point|body");
      if (!finiteNumber(document.targeting.range) || document.targeting.range < 0) fail("targeting 'range' must be finite and >= 0");
      if (!finiteNumber(document.targeting.radius) || document.targeting.radius < 0) fail("targeting 'radius' must be finite and >= 0");
    }
  }
  if (!Array.isArray(document.effects) || document.effects.length === 0) {
    fail("'effects' must be a non-empty array");
  } else {
    document.effects.forEach((effect, index) => validateAbilityEffect(effect, index, errors));
  }
  return { valid: errors.length === 0, errors };
}

function abilityDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorAbilityAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const kind = String(args.kind ?? "ability");
  if (!ABILITY_KINDS.includes(kind)) throw new Error(`unsupported ability kind '${kind}' (supported: ${ABILITY_KINDS.join(", ")})`);
  const name = assetName(args.name);
  const document = buildAbilityDocument(kind, args);
  const validation = validateAbilityDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: "ability asset fails public-contract validation; nothing was written"
    };
  }
  const file = path.join(project.abilities, `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? abilityDiff(previous, document) : null;
  const relative = path.relative(project.root, file).replaceAll(path.sep, "/");
  if (args.dry_run) {
    return {
      dry_run: true,
      would_write: relative,
      project: project.project,
      kind,
      name,
      document,
      diagnostics: [],
      diff
    };
  }
  if (previous && !args.update) throw new Error(`ability asset '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
  atomicWriteJson(file, document);
  return {
    created: !previous,
    updated: Boolean(previous),
    project: project.project,
    kind,
    name,
    path: relative,
    sha256: sha256File(file),
    diagnostics: [],
    diff,
    rollback: previous ? { document: previous, hint: "re-author with update: true and this document to restore" } : undefined
  };
}

// Reads every ability asset of a project with its structural diagnostics
// (mirrors readVehicleAssets).
function readAbilityAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.abilities)) return assets;
  for (const fileName of fs.readdirSync(project.abilities).filter((file) => file.endsWith(".json")).sort()) {
    const file = path.join(project.abilities, fileName);
    const baseName = fileName.replace(/\.json$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validateAbilityDocument("ability", document).errors);
    } else if (document) {
      diagnostics.push("ability asset must be a JSON object");
    }
    assets.push({
      kind: "ability",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectAbilityAssets(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readAbilityAssets(project);
  return { project: project.project, ability_assets: assets, count: assets.length };
}

function registryDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorRegistryAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const kind = String(args.kind);
  if (!REGISTRY_KINDS.includes(kind)) throw new Error(`unsupported registry kind '${kind}' (supported: ${REGISTRY_KINDS.join(", ")})`);
  const name = assetName(args.name ?? (kind === "fluid" ? args.block : ""));
  const document = buildRegistryDocument(kind, args);
  const validation = validateRegistryDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: "registry asset fails public-contract validation; nothing was written"
    };
  }
  const file = path.join(project.registry, kind, `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? registryDiff(previous, document) : null;
  const relative = path.relative(project.root, file).replaceAll(path.sep, "/");
  if (args.dry_run) {
    return {
      dry_run: true,
      would_write: relative,
      project: project.project,
      kind,
      name,
      document,
      diagnostics: [],
      diff
    };
  }
  if (previous && !args.update) throw new Error(`registry asset '${kind}/${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
  atomicWriteJson(file, document);
  return {
    created: !previous,
    updated: Boolean(previous),
    project: project.project,
    kind,
    name,
    path: relative,
    sha256: sha256File(file),
    diagnostics: [],
    diff,
    rollback: previous ? { document: previous, hint: "re-author with update: true and this document to restore" } : undefined
  };
}

function countRegistryAssets(project) {
  let count = 0;
  for (const kind of REGISTRY_KINDS) {
    const directory = path.join(project.registry, kind);
    if (!fs.existsSync(directory)) continue;
    count += fs.readdirSync(directory).filter((file) => file.endsWith(".json")).length;
  }
  return count;
}

// Reads every registry asset of a project with its structural diagnostics.
// The parsed document is kept for the cross-reference pass (item 9).
function readRegistryAssets(project) {
  const assets = [];
  for (const kind of REGISTRY_KINDS) {
    const directory = path.join(project.registry, kind);
    if (!fs.existsSync(directory)) continue;
    for (const fileName of fs.readdirSync(directory).filter((file) => file.endsWith(".json")).sort()) {
      const file = path.join(directory, fileName);
      let document = null;
      const diagnostics = [];
      try {
        document = readJson(file);
      } catch (error) {
        diagnostics.push(`malformed JSON: ${error.message}`);
      }
      if (document) diagnostics.push(...validateRegistryDocument(kind, document).errors);
      assets.push({
        kind,
        name: fileName.replace(/\.json$/, ""),
        path: path.relative(project.root, file).replaceAll(path.sep, "/"),
        document,
        valid: diagnostics.length === 0,
        diagnostics
      });
    }
  }
  return assets;
}

// Cross-reference validation (FALTANTES item 9), mirroring the C++ factories:
// every block drop must be namespaced and resolve to an authored item (a block
// without drops auto-fills "<ns>:<name>" exactly like BlockRegistry::add);
// every fluid's driven block must resolve to an authored or engine-builtin
// block. Diagnostics append to each asset; structurally invalid documents are
// skipped (their own errors already flag them).
function collectCrossReferenceDiagnostics(assets) {
  const items = new Map();
  const blocks = new Map();
  const docsOf = (asset) => (Array.isArray(asset.document) ? asset.document : [asset.document]);
  for (const asset of assets) {
    if (!asset.valid) continue;
    for (const entry of docsOf(asset)) {
      if (!entry || typeof entry !== "object") continue;
      const ns = entry.namespace ?? "vulkancraft";
      if (asset.kind === "item" && entry.name) items.set(`${ns}:${entry.name}`, true);
      if (asset.kind === "block" && entry.name) blocks.set(`${ns}:${entry.name}`, true);
    }
  }
  for (const asset of assets) {
    if (!asset.valid) continue;
    for (const entry of docsOf(asset)) {
      if (!entry || typeof entry !== "object") continue;
      const ns = entry.namespace ?? "vulkancraft";
      if (asset.kind === "block" && entry.name) {
        const drops = Array.isArray(entry.drops) && entry.drops.length ? entry.drops : [`${ns}:${entry.name}`];
        for (const drop of drops) {
          if (!String(drop).includes(":")) {
            asset.diagnostics.push(`block '${ns}:${entry.name}': drop '${drop}' must be namespaced (ns:name)`);
          } else if (!items.has(String(drop))) {
            asset.diagnostics.push(`block '${ns}:${entry.name}': drop '${drop}' references an unknown item`);
          }
        }
      }
      if (asset.kind === "fluid" && entry.block) {
        const ref = String(entry.block);
        if (!blocks.has(ref) && !ENGINE_BUILTIN_BLOCKS.has(ref)) {
          asset.diagnostics.push(`fluid for block '${ref}' references an unknown block`);
        }
      }
    }
    asset.valid = asset.valid && asset.diagnostics.length === 0;
  }
  return assets;
}

function inspectRegistryAssets(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = collectCrossReferenceDiagnostics(readRegistryAssets(project));
  return { project: project.project, registry_assets: assets, count: assets.length };
}

function validateProject(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const errors = [];
  const warnings = [];
  let manifest;
  try { manifest = readJson(project.manifest); } catch (error) { errors.push(`project.json: ${error.message}`); }
  if (manifest) {
    if (manifest.format !== "VulkanEngine.Project" || manifest.version !== 1) errors.push("project.json: unsupported format/version");
    if (manifest.engine !== "../..") warnings.push("project.json: engine path is not the portable relative path '../..'");
    if (!fs.existsSync(path.join(project.root, manifest.initialScene ?? ""))) errors.push(`initial scene does not exist: ${manifest.initialScene}`);
  }
  if (!fs.existsSync(project.config)) errors.push("ProjectConfig.json is missing");

  const sceneFiles = fs.existsSync(project.scenes) ? fs.readdirSync(project.scenes).filter((file) => file.endsWith(".scene")) : [];
  for (const fileName of sceneFiles) {
    try {
      const document = readJson(path.join(project.scenes, fileName));
      if (document.format !== "VulkanEngine.Scene" || document.version !== 1 || !Array.isArray(document.entities)) {
        errors.push(`${fileName}: unsupported scene format`);
        continue;
      }
      const ids = new Set();
      for (const entity of document.entities) {
        if (typeof entity.id !== "string" || ids.has(entity.id)) errors.push(`${fileName}: invalid or duplicate entity UUID '${entity.id}'`);
        ids.add(entity.id);
        if (!entity.Transform) errors.push(`${fileName}: entity '${entity.name}' has no Transform`);
        for (const component of Object.keys(entity).filter((key) => !["id", "name"].includes(key))) {
          if (!COMPONENT_SCHEMAS[component]) errors.push(`${fileName}: unknown component '${component}'`);
          else {
            const unknown = Object.keys(entity[component] ?? {}).filter((key) => !(key in COMPONENT_SCHEMAS[component]));
            if (unknown.length) errors.push(`${fileName}: ${component} has unknown fields ${unknown.join(", ")}`);
          }
        }
      }
      for (const entity of document.entities) {
        if (entity.Hierarchy?.parent_id && !ids.has(entity.Hierarchy.parent_id)) errors.push(`${fileName}: entity '${entity.name}' has missing parent '${entity.Hierarchy.parent_id}'`);
      }
      const cameras = document.entities.filter((entity) => entity.Camera?.primary);
      if (cameras.length === 0) warnings.push(`${fileName}: no primary camera`);
      if (cameras.length > 1) warnings.push(`${fileName}: multiple primary cameras`);
    } catch (error) {
      errors.push(`${fileName}: ${error.message}`);
    }
  }

  for (const directory of [project.scripts, project.scenes]) {
    if (!fs.existsSync(directory)) continue;
    for (const fileName of fs.readdirSync(directory).filter((file) => file.endsWith(".script"))) {
      try {
        const script = readJson(path.join(directory, fileName));
        const ids = new Set((script.nodes ?? []).map((node) => node.id));
        for (const node of script.nodes ?? []) if (!SCRIPT_NODE_KINDS.includes(node.kind)) errors.push(`${fileName}: unsupported node kind '${node.kind}'`);
        for (const link of script.links ?? []) if (!ids.has(link.from) || !ids.has(link.to)) errors.push(`${fileName}: dangling script link`);
      } catch (error) { errors.push(`${fileName}: ${error.message}`); }
    }
  }

  // Registry assets: structural validation + cross-reference validation
  // (FALTANTES item 9 — block drops resolve to items, fluid blocks resolve to
  // authored/builtin blocks), mirrored from the public C++ factories.
  const registryAssets = collectCrossReferenceDiagnostics(readRegistryAssets(project));
  for (const asset of registryAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Registry/${asset.kind}/${asset.name}.json: ${diagnostic}`);
    }
  }

  // Vehicle assembly assets (FALTANTES §17 item 12): structural validation
  // mirroring the public C++ VehicleAsset/BeamGraphAsset factories.
  const vehicleAssets = readVehicleAssets(project);
  for (const asset of vehicleAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Vehicles/${asset.name}.json: ${diagnostic}`);
    }
  }

  // Ability assets (FALTANTES §19 — abilities data-driven): structural
  // validation mirroring the public C++ AbilityDefinition factory.
  const abilityAssets = readAbilityAssets(project);
  for (const asset of abilityAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Abilities/${asset.name}.json: ${diagnostic}`);
    }
  }

  return { project: project.project, valid: errors.length === 0, errors, warnings, scenes: sceneFiles.length, registry_assets: registryAssets.length, vehicle_assets: vehicleAssets.length, ability_assets: abilityAssets.length };
}
