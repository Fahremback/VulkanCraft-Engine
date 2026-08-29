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

// Per-kind required fields, mirroring what the real ScriptCompiler produces a
// MEANINGFUL program from (ScriptRuntime.cpp): an Event/Function without a
// name registers eventEntries[""] (never dispatched by start_event); a
// Get/SetVariable without a variable reads/writes the "" slot; a Constant*
// without a literal pushes an empty value; a Wait without a literal never
// waits (execute_one reads the duration only from a double). All-or-nothing:
// refuse instead of emitting a dead node the runtime would silently ignore.
export const SCRIPT_NODE_REQUIRED_FIELDS = Object.freeze({
  Event: ["event"], Function: ["event"],
  GetVariable: ["variable"], SetVariable: ["variable"],
  ConstantFloat: ["literal"], ConstantInteger: ["literal"], ConstantBoolean: ["literal"],
  Wait: ["literal"]
});

// Engine literal semantics (ScriptRuntime.cpp): the literal TYPE follows the
// node kind — ConstantFloat/Wait carry doubles, ConstantInteger int64,
// ConstantBoolean bool (script_value_to_json emits type "float"/"int"/"bool").
// A JS number is always a float semantically; emitting {type:"int"} for 1.0
// (Number.isInteger(1.0) is true) makes the VM store an int64 (observable type
// drift vs the engine's own authoring) and — worse — makes a Wait node a silent
// no-op, since execute_one only honors a double operand. Coerce/validate per kind.
export const SCRIPT_LITERAL_KINDS = Object.freeze({
  ConstantFloat: "float", Wait: "float", ConstantInteger: "int", ConstantBoolean: "bool"
});

export function scriptLiteralForKind(kind, value) {
  const expected = SCRIPT_LITERAL_KINDS[kind];
  if (!expected) return literalToEngineJson(value);
  if (value && typeof value === "object" && ["bool", "int", "float", "string", "uuid"].includes(value.type)) {
    if (value.type !== expected) throw new Error(`script literal type '${value.type}' does not match ${kind} (expected '${expected}')`);
    return value;
  }
  if (expected === "float") {
    if (typeof value !== "number" || !Number.isFinite(value)) throw new Error(`${kind} requires a finite number literal`);
    return { type: "float", value };
  }
  if (expected === "int") {
    if (typeof value !== "number" || !Number.isInteger(value)) throw new Error(`${kind} requires an integer literal`);
    return { type: "int", value };
  }
  if (expected === "bool") {
    if (typeof value !== "boolean") throw new Error(`${kind} requires a boolean literal`);
    return { type: "bool", value };
  }
  return literalToEngineJson(value);
}

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

// World-profile asset kind (FALTANTES item 23 "editar geração procedural" —
// the C++ contract is engine/procgen/IWorldProfile.hpp by AGENT-3, findings
// #120-worldprofile): ONE versioned JSON document that composes the whole
// generation pipeline — height (noise graph) + baseHeight/amplitude, climate
// axes (each an optional noise graph), biomes, caves/ores (density + scale/
// offset + ore table), carver, decorators and structures (definitions +
// spawn rules) — into the generator the world registers. The MCP validates
// the TOP-LEVEL structure only (version, section types, amplitude >= 0,
// finite scale/offset); every present section is validated all-or-nothing by
// its OWN subsystem parser when the C++ factory loads the document
// (create_world_profile_from_json) — the MCP never re-implements the
// subsystem parsers (the same "MCP validates structure only" rule as
// registry/vehicle/ability/mission assets). File: Content/Profiles/<name>.json.
export const WORLD_PROFILE_KINDS = Object.freeze(["world_profile"]);
const WORLD_PROFILE_CLIMATE_AXES = ["temperature", "moisture", "continentalness", "erosion", "weirdness", "river"];

// Compact field contracts surfaced by game_capabilities so an agent can author
// a world profile without reading the engine source.
export const WORLD_PROFILE_FIELD_SCHEMAS = Object.freeze({
  world_profile: [
    { name: "name", type: "string", required: false, description: "asset file name (e.g. my_world); carried by the file name, NOT a document property" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "height", type: "object", required: false, description: "2D height field: a noise graph JSON document" },
    { name: "baseHeight", type: "integer", required: false, default: 0, description: "height = baseHeight + round(h * amplitude); only read when 'height' is present" },
    { name: "amplitude", type: "integer", required: false, default: 1, description: ">= 0; only read when 'height' is present" },
    { name: "climate", type: "object", required: false, description: "optional axes, each a noise graph JSON document: { temperature?, moisture?, continentalness?, erosion?, weirdness?, river? }" },
    { name: "biomes", type: "object", required: false, description: "biome registry JSON document (surface + climate per biome)" },
    { name: "caves", type: "object", required: false, description: "{ density?: noise graph JSON, scale?: number, offset?: number }" },
    { name: "ores", type: "object", required: false, description: "{ density?: noise graph JSON, scale?: number, offset?: number, table?: ore table JSON }" },
    { name: "carver", type: "object", required: false, description: "carver JSON document" },
    { name: "decorators", type: "object", required: false, description: "decorator set JSON document" },
    { name: "structures", type: "object", required: false, description: "structure placement document (definitions + spawn rules)" }
  ]
});

// MCP authors a gait asset (FALTANTES item 23 "animações"): a data-driven
// creature locomotion asset — cycle timing, per-leg phase offsets and the
// hip-anchored two-bone leg chains — that the public C++ contract parses via
// GaitAsset::load_from_json (src/engine/sdk/GaitPlanner.cpp, bit-exact %.9g
// round-trip, all-or-nothing). The planner (IContactPlanner) consumes the
// asset to map body state + gait clock to per-foot targets. File:
// Content/Animations/<name>.json.
export const GAIT_KINDS = Object.freeze(["gait"]);

// Compact field contracts surfaced by game_capabilities so an agent can author
// a gait without reading the engine source. Single source for the exported
// JSON Schema (buildGaitJsonSchema) and the author tool.
export const GAIT_FIELD_SCHEMAS = Object.freeze({
  gait: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name); must be non-empty" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "cycleDuration", type: "number", required: false, default: 1.0, description: "duration of one full stride cycle (s), > 0" },
    { name: "stanceFraction", type: "number", required: false, default: 0.6, description: "stance fraction of the cycle; the rest is swing, in (0, 1)" },
    { name: "stepHeight", type: "number", required: false, default: 0.25, description: "vertical foot lift during swing, >= 0" },
    { name: "maxStride", type: "number", required: false, default: 0.5, description: "max horizontal stride per step, > 0" },
    { name: "legPhases", type: "array[number]", required: false, default: [], description: "per-leg phase offset (fraction of the cycle) in [0, 1); size must equal legs" },
    { name: "legs", type: "array[object]", required: true, description: "leg chains (one per leg): [{ name (non-empty), hipOffset: [x,y,z], upperLength > 0, lowerLength > 0, restOffset: [x,y,z], maxReach >= 0 (0 = auto upper+lower), hipBone/kneeBone/footBone (>= 0 binds; -1 = unbound; distinct when set) }]" }
  ]
});

// MCP authors a simulation LOD spec (FALTANTES §20 — the AGENT-4 Item 20
// contract, findings #123-sim-lod): the data-driven budget that maps region
// RELEVANCE to simulation tiers (Full/Coarse/Aggregate/Sleeping) plus the
// world clock and region grid. The public C++ factory parses it via
// SimulationLodSpec::load_from_json (src/engine/sdk/SimulationLod.cpp,
// bit-exact %.9g round-trip, all-or-nothing) and the pure ISimulationLod
// runtime consumes it. File: Content/SimulationLod/<name>.json.
export const SIMULATION_LOD_KINDS = Object.freeze(["simulation_lod"]);
const SIMULATION_LOD_MODES = new Set(["full", "coarse", "aggregate", "sleeping"]);

// Compact field contracts surfaced by game_capabilities so an agent can author
// a simulation LOD spec without reading the engine source. Single source for
// the exported JSON Schema (buildSimulationLodJsonSchema) and the author tool.
export const SIMULATION_LOD_FIELD_SCHEMAS = Object.freeze({
  simulation_lod: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "cellSize", type: "number", required: false, default: 16.0, description: "region cell size in world units, > 0" },
    { name: "fullRadius", type: "number", required: false, default: 48.0, description: "distance within which relevance == 1, >= 0" },
    { name: "falloffRadius", type: "number", required: false, default: 320.0, description: "distance at which relevance == 0, > fullRadius" },
    { name: "dayLengthSeconds", type: "number", required: false, default: 240.0, description: "length of a world day (s), > 0" },
    { name: "daysPerSeason", type: "integer", required: false, default: 30, description: ">= 1 (4 seasons)" },
    { name: "tiers", type: "array[object]", required: true, description: "simulation budget tiers, non-empty, sorted by minRelevance DESCENDING: [{ name (unique), mode: full|coarse|aggregate|sleeping, minRelevance in [0, 1] (strictly descending), updateInterval >= 0 (s between ticks; 0 = every update), sleepAfterIdle >= 0, maxRegions >= 0 (0 = unlimited), aggregateInterval >= 0 }]" }
  ]
});

// Prefab assets (FALTANTES item 23 — "cenas, entidades, componentes e
// prefabs"): a prefab is a REUSABLE entity set extracted from a scene — a
// scene-shaped document with the component payload preserved and entity ids
// stripped (they are regenerated on instantiation). The MCP authors it from an
// existing scene (create_prefab) and instantiates it into a target scene
// (instantiate_prefab) with fresh UUIDs and internal Hierarchy.parent_id
// remapping. File: Content/Prefabs/<name>.prefab.
export const PREFAB_KINDS = Object.freeze(["prefab"]);

// Compact field contracts surfaced by game_capabilities so an agent can author
// a prefab without reading the engine source. Single source for the exported
// JSON Schema (buildPrefabJsonSchema) and the author tool.
export const PREFAB_FIELD_SCHEMAS = Object.freeze({
  prefab: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "entities", type: "array[object]", required: true, description: "reusable entity set extracted from a scene: each entry is an entity document WITHOUT its id (regenerated on instantiate) — { name, Transform, ...components }" },
    { name: "root_entity", type: "string", required: false, description: "name of the prefab root entity (instantiation origin when offset is applied)" },
    { name: "source_scene", type: "string", required: false, description: "scene the prefab was extracted from (informational)" },
    { name: "source_entity_ids", type: "array[string]", required: false, description: "entity UUIDs the prefab was extracted from (informational)" }
  ]
});

// Particle emitter assets (FALTANTES item 23 — "Configurar física, partículas,
// áudio e navegação"): a REUSABLE particle emitter configuration. The emitter
// fields are EXACTLY the public ParticleEmitter component schema (single
// source of truth — derived from COMPONENT_SCHEMAS, no duplication); the MCP
// validates the fields and apply_particle_asset writes them into an entity's
// ParticleEmitter component. File: Content/Particles/<name>.particle.
export const PARTICLE_KINDS = Object.freeze(["particle"]);

// Compact field contracts surfaced by game_capabilities so an agent can author
// a particle asset without reading the engine source. Single source for the
// exported JSON Schema (buildParticleJsonSchema) and the author tool — the
// emitter fields come straight from COMPONENT_SCHEMAS.ParticleEmitter.
export const PARTICLE_FIELD_SCHEMAS = Object.freeze({
  particle: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    ...Object.entries(COMPONENT_SCHEMAS.ParticleEmitter).map(([field, defaultValue]) => ({
      name: field,
      type: typeof defaultValue === "boolean" ? "boolean" : "number",
      required: false,
      default: defaultValue,
      description: `ParticleEmitter.${field} (component schema default ${JSON.stringify(defaultValue)})`
    }))
  ]
});

// ---------------------------------------------------------------------------
// §4.4 + §4.5 — rendering + voxel/world CONFIG asset kinds (2026-08-28). Each
// document mirrors EXACTLY the versioned JSON the public C++ contracts parse
// (all-or-nothing, never clamped):
//   shader         -> IShaderCompiler.ShaderCompilerConfig + GLSL source
//   render_graph   -> IRenderGraph (resources, passes, dependencies DAG)
//   light          -> Light component schema + light_types (game_capabilities)
//   gi             -> GiClipmapConfig + DiffuseGiConfig (IGlobalIlluminationProvider/IDiffuseGlobalIllumination)
//   ocean          -> FftOceanConfig (IFftOceanSurface)
//   post_process   -> ToneMappingConfig + CasConfig + quality (IToneMapping/ICasSharpening/IRenderingPresets)
//   fluid_sim      -> FluidConfig (IFluidSimulation)
//   world          -> WorldSpec + portals (IWorldManager)
//   chunk          -> StreamingSnapshot budgets (IVoxelStreaming)
//   transaction    -> BlockEdit[] + TransactionLimits (IVoxelWorld)
//   block_entity   -> IVoxelBlockEntity (type_id/data_version/script/components)
//   inventory      -> Inventory serialize_json (engine/registry/Inventory.hpp)
export const CONFIG_KINDS = Object.freeze([
  "shader", "render_graph", "light", "gi", "ocean", "post_process", "fluid_sim",
  "world", "chunk", "transaction", "block_entity", "inventory"
]);

// Enum surfaces mirroring the public C++ contracts.
const SHADER_STAGES = Object.freeze(["vertex", "fragment", "compute"]);
const TONE_OPERATORS = Object.freeze(["reinhard", "aces", "filmic", "none"]);
const QUALITY_LEVELS = Object.freeze(["low", "medium", "high", "ultra", "cinematic"]);
const LIGHT_TYPE_NAMES = Object.freeze(["directional", "point", "spot", "area"]);
const RESOURCE_KINDS = Object.freeze(["buffer", "image"]);
const RENDER_QUEUES = Object.freeze(["graphics", "compute", "transfer"]);
const RENDER_ACCESS = Object.freeze(["read", "write", "readwrite"]);
const RESOURCE_STATES = Object.freeze(["undefined", "shader_read", "color_attachment", "depth_attachment", "general", "transfer_source", "transfer_destination", "present"]);
const BLOCK_ENTITY_COMPONENT_TYPES = Object.freeze(["inventory", "script", "custom"]);

// Compact field contracts surfaced by game_capabilities so an agent can author
// every config kind without reading the engine source. Single source for the
// exported JSON Schema (buildConfigJsonSchema) and the author tools.
export const CONFIG_FIELD_SCHEMAS = Object.freeze({
  shader: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "stage", type: "enum", values: [...SHADER_STAGES], required: false, default: "fragment", description: "shader stage (vertex/fragment/compute) — mirror of IShaderCompiler.ShaderStage" },
    { name: "source", type: "string", required: true, description: "GLSL source text" },
    { name: "target_env", type: "string", required: false, default: "", description: "SPIR-V target env (e.g. 'spirv1.5'); empty = compiler default" },
    { name: "opt_level", type: "integer", required: false, default: 0, description: "optimization level: 0 = none, 1 = size, 2 = speed" },
    { name: "defines", type: "array[string]", required: false, default: [], description: "macro defines, e.g. ['MAX_LIGHTS=64']" }
  ],
  render_graph: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "resources", type: "array[object]", required: true, description: "[{ name (unique), kind: buffer|image, byte_size, width, height, depth, transient, imported, initial_state }]" },
    { name: "passes", type: "array[object]", required: true, description: "[{ name (unique), queue: graphics|compute|transfer, enabled, resources: [{ resource (name), access: read|write|readwrite, state }] }]" },
    { name: "dependencies", type: "array[object]", required: false, default: [], description: "[{ before, after }] — pass names; the graph must be acyclic" }
  ],
  light: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "type", type: "enum", values: [...LIGHT_TYPE_NAMES], required: false, default: "directional", description: "light type (0=Directional, 1=Point, 2=Spot, 3=Area)" },
    { name: "color", type: "array[3]", required: false, default: [1, 1, 1], description: "RGB color 0..1" },
    { name: "intensity", type: "number", required: false, default: 1000, description: "light intensity > 0" },
    { name: "range", type: "number", required: false, default: 50, description: "range >= 0 (directional ignores)" },
    { name: "cast_shadows", type: "boolean", required: false, default: true }
  ],
  gi: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "cascade_count", type: "integer", required: false, default: 6, description: "probe clipmap cascades [1, 6]" },
    { name: "resolution", type: "integer", required: false, default: 16, description: "probes per axis [4, 32]" },
    { name: "probes_per_frame", type: "integer", required: false, default: 192, description: "bake budget per frame >= 1" },
    { name: "base_spacing", type: "number", required: false, default: 4.0, description: "cascade base spacing meters >= 0.5" },
    { name: "cascade_scale", type: "number", required: false, default: 4.0, description: ">= 2" },
    { name: "sun_refresh_angle_degrees", type: "number", required: false, default: 2.0, description: "sun-revision threshold [0.25, 15]" },
    { name: "bounces", type: "integer", required: false, default: 2, description: "multi-bounce gather iterations [1, 8]" },
    { name: "skylight", type: "array[3]", required: false, default: [0.05, 0.07, 0.10], description: "shadowed ambient RGB term" },
    { name: "max_distance", type: "number", required: false, default: 128.0, description: "form-factor cull distance > 0" },
    { name: "intensity", type: "number", required: false, default: 1.0, description: "global scale on gathered light [0.01, 64]" }
  ],
  ocean: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "size", type: "integer", required: false, default: 64, description: "tile size per axis (power of two) [16, 1024]" },
    { name: "tile_size_meters", type: "number", required: false, default: 256.0, description: "world size of the tile [16, 8192]" },
    { name: "wind_speed", type: "number", required: false, default: 18.0, description: "wind speed m/s [0.5, 40]" },
    { name: "wind_dir_rad", type: "number", required: false, default: 0.7, description: "main wave direction (radians), finite" },
    { name: "choppiness", type: "number", required: false, default: 1.2, description: "horizontal displacement scale [0, 4]" },
    { name: "amplitude", type: "number", required: false, default: 0.9, description: "spectrum amplitude scale [0.01, 8]" },
    { name: "seed", type: "integer", required: false, default: 1, description: "deterministic phase seed" }
  ],
  post_process: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "operator", type: "enum", values: [...TONE_OPERATORS], required: false, default: "aces", description: "tone mapping operator (IToneMapping.ToneOperator)" },
    { name: "exposure", type: "number", required: false, default: 1.0, description: "manual exposure multiplier [0.01, 64]" },
    { name: "use_ev", type: "boolean", required: false, default: false, description: "when true, EV-based exposure overrides manual" },
    { name: "ev100", type: "number", required: false, default: 0.0, description: "EV value [-16, 16] -> 1/(1.2 * 2^EV)" },
    { name: "white_point", type: "number", required: false, default: 11.2, description: "Filmic white point [1, 64]" },
    { name: "sharpness", type: "number", required: false, default: 0.4, description: "CAS sharpness [0, 1]" },
    { name: "clamp_values", type: "boolean", required: false, default: true, description: "CAS contrast clamp (anti-ringing)" },
    { name: "quality", type: "enum", values: [...QUALITY_LEVELS], required: false, default: "high", description: "rendering quality preset (IRenderingPresets)" }
  ],
  fluid_sim: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "grid_size", type: "integer", required: false, default: 64, description: "NxN heightfield, > 0" },
    { name: "cell_size", type: "number", required: false, default: 1.0, description: "world units per cell, > 0" },
    { name: "gravity", type: "number", required: false, default: 9.81, description: "> 0" },
    { name: "dt", type: "number", required: false, default: 0.0166667, description: "time step, > 0" },
    { name: "solver_iterations", type: "integer", required: false, default: 4, description: "Jacobi iterations per step, > 0" },
    { name: "damping", type: "number", required: false, default: 0.999, description: "velocity damping [0, 1]" },
    { name: "viscosity", type: "number", required: false, default: 0.001, description: "viscosity coefficient >= 0" },
    { name: "surface_tension", type: "number", required: false, default: 0.0, description: "surface tension coefficient >= 0" }
  ],
  world: [
    { name: "name", type: "string", required: true, description: "unique world name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "seed", type: "integer", required: false, default: 0, description: "per-world generator/behavior RNG seed" },
    { name: "rules_json", type: "string", required: false, default: "", description: "optional opaque rules document (must be well-formed JSON when non-empty)" },
    { name: "profile", type: "string", required: false, default: "", description: "name of a world profile asset in Content/Profiles (WorldSpec.profileJson)" },
    { name: "save_path", type: "string", required: false, default: "", description: "default persistence location for load_world/save_world" },
    { name: "portals", type: "array[object]", required: false, default: [], description: "[{ from_world, from: [x,y,z], to_world, to: [x,y,z], yaw_degrees }]" }
  ],
  chunk: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "chunk_budget", type: "integer", required: false, default: 16, description: "streaming budget (chunk radius), >= 0" },
    { name: "memory_budget_bytes", type: "integer", required: false, default: 0, description: "RAM-budgeted chunk cache bytes; 0 = unlimited" },
    { name: "far_lod_percent", type: "integer", required: false, default: 0, description: "far-LOD endpoint 0..100" },
    { name: "worker_threads", type: "integer", required: false, default: 0, description: "world worker pool size; 0 = unknown/auto" }
  ],
  transaction: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "max_edits", type: "integer", required: false, default: 0, description: "per-transaction edit cap; 0 = unlimited" },
    { name: "max_box_volume", type: "integer", required: false, default: 0, description: "bounding-box volume cap; 0 = unlimited" },
    { name: "edits", type: "array[object]", required: true, description: "[{ position: [x,y,z] (integers), block_id (uint, 0 = Air) }]" }
  ],
  block_entity: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "type_id", type: "string", required: true, description: "stable namespaced type id, e.g. 'project:furnace'" },
    { name: "data_version", type: "integer", required: false, default: 1, description: "project-owned data version >= 1" },
    { name: "script_id", type: "string", required: false, default: "", description: "optional project-owned script id (e.g. 'project:door_open')" },
    { name: "components", type: "array[object]", required: false, default: [], description: "[{ type: inventory|script|custom, version, blob (opaque payload string) }]" }
  ],
  inventory: [
    { name: "name", type: "string", required: true, description: "asset name (becomes the file name)" },
    { name: "version", type: "integer", required: false, default: 1, description: "must be 1" },
    { name: "slots", type: "array", required: true, description: "slot contents: [{ item (namespaced id), count, damage, data } | null]" },
    { name: "filters", type: "array[object]", required: false, default: [], description: "[{ slot (int), allow_items: [ns ids], allow_tags: [tag], allow_any }]" }
  ]
});

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

export function buildMissionJsonSchema(kind) {
  const fields = MISSION_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported mission kind '${kind}'`);
  const properties = {};
  const required = [];
  for (const field of fields) {
    let schema = { description: field.description ?? `\"${field.name}\" mission field` };
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
      default: throw new Error(`unknown mission field type '${field.type}' in ${kind} schema`);
    }
    properties[field.name] = schema;
    if (field.required) required.push(field.name);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/missions/${kind}.json`,
    title: `${kind} mission asset`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

export function buildWorldProfileJsonSchema(kind) {
  const fields = WORLD_PROFILE_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported world profile kind '${kind}'`);
  const properties = {};
  const required = [];
  for (const field of fields) {
    let schema = { description: field.description ?? `\"${field.name}\" world profile field` };
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
      default: throw new Error(`unknown world profile field type '${field.type}' in ${kind} schema`);
    }
    properties[field.name] = schema;
    if (field.required) required.push(field.name);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/profiles/${kind}.json`,
    title: `${kind} world profile`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

export function buildGaitJsonSchema(kind) {
  const fields = GAIT_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported gait kind '${kind}'`);
  const properties = {};
  const required = [];
  for (const field of fields) {
    let schema = { description: field.description ?? `\"${field.name}\" gait field` };
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
      case "array[number]": schema.type = "array"; schema.items = { type: "number" }; break;
      case "array[string]": schema.type = "array"; schema.items = { type: "string" }; break;
      default: throw new Error(`unknown gait field type '${field.type}' in ${kind} schema`);
    }
    properties[field.name] = schema;
    if (field.required) required.push(field.name);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/animations/${kind}.json`,
    title: `${kind} animation asset`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

export function buildSimulationLodJsonSchema(kind) {
  const fields = SIMULATION_LOD_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported simulation lod kind '${kind}'`);
  const properties = {};
  const required = [];
  for (const field of fields) {
    let schema = { description: field.description ?? `\"${field.name}\" simulation lod field` };
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
      case "array[number]": schema.type = "array"; schema.items = { type: "number" }; break;
      case "array[string]": schema.type = "array"; schema.items = { type: "string" }; break;
      default: throw new Error(`unknown simulation lod field type '${field.type}' in ${kind} schema`);
    }
    properties[field.name] = schema;
    if (field.required) required.push(field.name);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/simulation/${kind}.json`,
    title: `${kind} simulation LOD spec`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

export function buildPrefabJsonSchema(kind) {
  const fields = PREFAB_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported prefab kind '${kind}'`);
  const properties = {};
  const required = [];
  for (const field of fields) {
    let schema = { description: field.description ?? `\"${field.name}\" prefab field` };
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
      case "array[number]": schema.type = "array"; schema.items = { type: "number" }; break;
      case "array[string]": schema.type = "array"; schema.items = { type: "string" }; break;
      default: throw new Error(`unknown prefab field type '${field.type}' in ${kind} schema`);
    }
    properties[field.name] = schema;
    if (field.required) required.push(field.name);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/scene/${kind}.json`,
    title: `${kind} reusable entity set`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

export function buildParticleJsonSchema(kind) {
  const fields = PARTICLE_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported particle kind '${kind}'`);
  const properties = {};
  const required = [];
  for (const field of fields) {
    let schema = { description: field.description ?? `\"${field.name}\" particle field` };
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
      case "array[number]": schema.type = "array"; schema.items = { type: "number" }; break;
      case "array[string]": schema.type = "array"; schema.items = { type: "string" }; break;
      default: throw new Error(`unknown particle field type '${field.type}' in ${kind} schema`);
    }
    properties[field.name] = schema;
    if (field.required) required.push(field.name);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/scene/${kind}.json`,
    title: `${kind} particle emitter asset`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

// JSON Schema (draft-07) for one CONFIG kind (shader/render_graph/light/gi/
// ocean/post_process/fluid_sim/world/chunk/transaction/block_entity/
// inventory), generated from the SAME CONFIG_FIELD_SCHEMAS contracts the
// author tools mirror against — a single source of truth so the exported
// schema can never drift from what the mirror validation accepts (the same
// pattern as the registry/vehicle/ability/... builders).
export function buildConfigJsonSchema(kind) {
  const fields = CONFIG_FIELD_SCHEMAS[kind];
  if (!fields) throw new Error(`unsupported config kind '${kind}'`);
  const properties = {};
  const required = [];
  for (const field of fields) {
    let schema = { description: field.description ?? `"${field.name}" config field` };
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
      default: throw new Error(`unknown config field type '${field.type}' in ${kind} schema`);
    }
    properties[field.name] = schema;
    if (field.required) required.push(field.name);
  }
  return {
    $schema: "http://json-schema.org/draft-07/schema#",
    $id: `https://vulkancraft.engine/schema/config/${kind}.json`,
    title: `${kind} config asset`,
    type: "object",
    additionalProperties: true,
    properties,
    ...(required.length > 0 ? { required } : {})
  };
}

// ─── CONFIG asset document builders ─────────────────────────────────────────
// Each builder mirrors the public C++ contract's JSON surface; snake_case
// authoring args map to the C++ camelCase document keys exactly as the
// factories parse them (all-or-nothing, never clamped).
function buildConfigDocument(kind, args) {
  const name = String(args.name ?? "");
  const num = (value, fallback) => (value === undefined || value === null ? fallback : Number(value));
  const int = (value, fallback) => (value === undefined || value === null ? fallback : Math.trunc(Number(value)));
  const bool = (value, fallback) => (value === undefined || value === null ? fallback : Boolean(value));
  const arr3 = (value, fallback) => (value === undefined || value === null ? [...fallback] : (Array.isArray(value) ? value.map(Number) : []));
  const doc = { name, version: int(args.version, 1) };
  switch (kind) {
    case "shader": {
      doc.stage = String(args.stage ?? "fragment");
      doc.source = String(args.source ?? "");
      doc.targetEnv = String(args.target_env ?? "");
      doc.optLevel = int(args.opt_level, 0);
      doc.defines = Array.isArray(args.defines) ? args.defines.map(String) : [];
      break;
    }
    case "render_graph": {
      doc.resources = Array.isArray(args.resources) ? args.resources.map((r) => ({
        name: String(r.name ?? ""),
        kind: String(r.kind ?? "image"),
        byteSize: num(r.byte_size, 0),
        width: int(r.width, 1),
        height: int(r.height, 1),
        depth: int(r.depth, 1),
        transient: bool(r.transient, true),
        imported: bool(r.imported, false),
        initialState: String(r.initial_state ?? "undefined")
      })) : [];
      doc.passes = Array.isArray(args.passes) ? args.passes.map((p) => ({
        name: String(p.name ?? ""),
        queue: String(p.queue ?? "graphics"),
        enabled: bool(p.enabled, true),
        resources: Array.isArray(p.resources) ? p.resources.map((ra) => ({
          resource: String(ra.resource ?? ""),
          access: String(ra.access ?? "read"),
          state: String(ra.state ?? "shader_read")
        })) : []
      })) : [];
      doc.dependencies = Array.isArray(args.dependencies) ? args.dependencies.map((d) => ({
        before: String(d.before ?? ""),
        after: String(d.after ?? "")
      })) : [];
      break;
    }
    case "light": {
      doc.type = String(args.type ?? "directional");
      doc.color = arr3(args.color, [1, 1, 1]);
      doc.intensity = num(args.intensity, 1000);
      doc.range = num(args.range, 50);
      doc.castShadows = bool(args.cast_shadows, true);
      break;
    }
    case "gi": {
      doc.cascadeCount = int(args.cascade_count, 6);
      doc.resolution = int(args.resolution, 16);
      doc.probesPerFrame = int(args.probes_per_frame, 192);
      doc.baseSpacing = num(args.base_spacing, 4.0);
      doc.cascadeScale = num(args.cascade_scale, 4.0);
      doc.sunRefreshAngleDegrees = num(args.sun_refresh_angle_degrees, 2.0);
      doc.bounces = int(args.bounces, 2);
      doc.skylight = arr3(args.skylight, [0.05, 0.07, 0.10]);
      doc.maxDistance = num(args.max_distance, 128.0);
      doc.intensity = num(args.intensity, 1.0);
      break;
    }
    case "ocean": {
      doc.size = int(args.size, 64);
      doc.tileSizeMeters = num(args.tile_size_meters, 256.0);
      doc.windSpeed = num(args.wind_speed, 18.0);
      doc.windDirRad = num(args.wind_dir_rad, 0.7);
      doc.choppiness = num(args.choppiness, 1.2);
      doc.amplitude = num(args.amplitude, 0.9);
      doc.seed = int(args.seed, 1);
      break;
    }
    case "post_process": {
      doc.operator = String(args.operator ?? "aces");
      doc.exposure = num(args.exposure, 1.0);
      doc.useEV = bool(args.use_ev, false);
      doc.ev100 = num(args.ev100, 0.0);
      doc.whitePoint = num(args.white_point, 11.2);
      doc.sharpness = num(args.sharpness, 0.4);
      doc.clampValues = bool(args.clamp_values, true);
      doc.quality = String(args.quality ?? "high");
      break;
    }
    case "fluid_sim": {
      doc.gridSize = int(args.grid_size, 64);
      doc.cellSize = num(args.cell_size, 1.0);
      doc.gravity = num(args.gravity, 9.81);
      doc.dt = num(args.dt, 1 / 60);
      doc.solverIterations = int(args.solver_iterations, 4);
      doc.damping = num(args.damping, 0.999);
      doc.viscosity = num(args.viscosity, 0.001);
      doc.surfaceTension = num(args.surface_tension, 0.0);
      break;
    }
    case "world": {
      doc.seed = int(args.seed, 0);
      doc.rulesJson = String(args.rules_json ?? "");
      doc.profile = String(args.profile ?? "");
      doc.savePath = String(args.save_path ?? "");
      doc.portals = Array.isArray(args.portals) ? args.portals.map((p) => ({
        fromWorld: String(p.from_world ?? ""),
        from: Array.isArray(p.from) ? p.from.map(Number) : [],
        toWorld: String(p.to_world ?? ""),
        to: Array.isArray(p.to) ? p.to.map(Number) : [],
        yawDegrees: num(p.yaw_degrees, 0.0)
      })) : [];
      break;
    }
    case "chunk": {
      doc.chunkBudget = int(args.chunk_budget, 16);
      doc.memoryBudgetBytes = int(args.memory_budget_bytes, 0);
      doc.farLodPercent = int(args.far_lod_percent, 0);
      doc.workerThreads = int(args.worker_threads, 0);
      break;
    }
    case "transaction": {
      doc.maxEdits = int(args.max_edits, 0);
      doc.maxBoxVolume = int(args.max_box_volume, 0);
      doc.edits = Array.isArray(args.edits) ? args.edits.map((e) => {
        const position = Array.isArray(e.position) ? e.position.map(Number) : [];
        return { position, blockId: int(e.block_id, 0) };
      }) : [];
      break;
    }
    case "block_entity": {
      doc.typeId = String(args.type_id ?? "");
      doc.dataVersion = int(args.data_version, 1);
      doc.scriptId = String(args.script_id ?? "");
      doc.components = Array.isArray(args.components) ? args.components.map((c) => ({
        type: String(c.type ?? "custom"),
        version: int(c.version, 1),
        blob: String(c.blob ?? "")
      })) : [];
      break;
    }
    case "inventory": {
      doc.slots = Array.isArray(args.slots) ? args.slots.map((slot) => (slot === null ? null : {
        item: String(slot.item ?? ""),
        count: int(slot.count, 1),
        damage: int(slot.damage, 0),
        data: String(slot.data ?? "")
      })) : [];
      doc.filters = Array.isArray(args.filters) ? args.filters.map((f) => ({
        slot: int(f.slot, 0),
        allowItems: Array.isArray(f.allow_items) ? f.allow_items.map(String) : [],
        allowTags: Array.isArray(f.allow_tags) ? f.allow_tags.map(String) : [],
        allowAny: bool(f.allow_any, false)
      })) : [];
      break;
    }
    default:
      throw new Error(`unsupported config kind '${kind}'`);
  }
  return doc;
}

// ─── CONFIG asset validators (all-or-nothing, mirroring the C++ ranges) ────
function finiteNum(value) {
  return typeof value === "number" && Number.isFinite(value);
}

function isPow2(value) {
  return value > 0 && (value & (value - 1)) === 0;
}

export function validateConfigDocument(kind, document) {
  const errors = [];
  const fail = (message) => errors.push(`${kind} asset ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["config asset must be a JSON object"] };
  }
  if (!CONFIG_KINDS.includes(kind)) return { valid: false, errors: [`unsupported config kind '${kind}'`] };
  if (document.version !== undefined && document.version !== 1) fail("'version' must be 1");
  if (typeof document.name !== "string" || !document.name) fail("'name' is required");

  // The C++ contracts apply DOCUMENTED DEFAULTS before validating (the same
  // pattern as GaitAsset::load_from_json): an omitted optional field takes its
  // default and is never treated as missing/NaN.
  const stage = document.stage ?? "fragment";
  const targetEnv = document.targetEnv ?? "";
  const optLevel = document.optLevel ?? 0;
  const defines = document.defines ?? [];
  const type = document.type ?? "directional";
  const color = document.color ?? [1, 1, 1];
  const intensity = document.intensity ?? 1000;
  const range = document.range ?? 50;
  const cascadeCount = document.cascadeCount ?? 6;
  const resolution = document.resolution ?? 16;
  const probesPerFrame = document.probesPerFrame ?? 192;
  const baseSpacing = document.baseSpacing ?? 4.0;
  const cascadeScale = document.cascadeScale ?? 4.0;
  const sunRefreshAngleDegrees = document.sunRefreshAngleDegrees ?? 2.0;
  const bounces = document.bounces ?? 2;
  const skylight = document.skylight ?? [0.05, 0.07, 0.10];
  const maxDistance = document.maxDistance ?? 128.0;
  const giIntensity = document.intensity ?? 1.0;
  const size = document.size ?? 64;
  const tileSizeMeters = document.tileSizeMeters ?? 256.0;
  const windSpeed = document.windSpeed ?? 18.0;
  const windDirRad = document.windDirRad ?? 0.7;
  const choppiness = document.choppiness ?? 1.2;
  const amplitude = document.amplitude ?? 0.9;
  const seed = document.seed ?? 1;
  const operator = document.operator ?? "aces";
  const exposure = document.exposure ?? 1.0;
  const ev100 = document.ev100 ?? 0.0;
  const whitePoint = document.whitePoint ?? 11.2;
  const sharpness = document.sharpness ?? 0.4;
  const quality = document.quality ?? "high";
  const gridSize = document.gridSize ?? 64;
  const cellSize = document.cellSize ?? 1.0;
  const gravity = document.gravity ?? 9.81;
  const dt = document.dt ?? 1 / 60;
  const solverIterations = document.solverIterations ?? 4;
  const damping = document.damping ?? 0.999;
  const viscosity = document.viscosity ?? 0.001;
  const surfaceTension = document.surfaceTension ?? 0.0;
  const chunkBudget = document.chunkBudget ?? 16;
  const memoryBudgetBytes = document.memoryBudgetBytes ?? 0;
  const farLodPercent = document.farLodPercent ?? 0;
  const workerThreads = document.workerThreads ?? 0;
  const maxEdits = document.maxEdits ?? 0;
  const maxBoxVolume = document.maxBoxVolume ?? 0;
  const dataVersion = document.dataVersion ?? 1;
  const scriptId = document.scriptId ?? "";
  const components = document.components ?? [];
  const filters = document.filters ?? [];
  const portals = document.portals ?? [];

  switch (kind) {
    case "shader": {
      if (!SHADER_STAGES.includes(stage)) fail(`'stage' must be one of ${SHADER_STAGES.join(", ")}`);
      if (typeof document.source !== "string" || !document.source.trim()) fail("'source' GLSL text is required");
      if (typeof targetEnv !== "string") fail("'targetEnv' must be a string");
      if (!Number.isInteger(optLevel) || optLevel < 0 || optLevel > 2) fail("'optLevel' must be 0, 1 or 2");
      if (!Array.isArray(defines) || !defines.every((d) => typeof d === "string")) fail("'defines' must be an array of strings");
      break;
    }
    case "render_graph": {
      const resourceNames = new Set();
      if (!Array.isArray(document.resources) || document.resources.length === 0) {
        fail("'resources' must be a non-empty array");
        return { valid: errors.length === 0, errors };
      }
      document.resources.forEach((resource, index) => {
        const where = `resource ${index}`;
        if (!resource || typeof resource !== "object" || Array.isArray(resource)) return fail(`${where} must be an object`);
        if (typeof resource.name !== "string" || !resource.name) return fail(`${where} 'name' must not be empty`);
        if (resourceNames.has(resource.name)) return fail(`${where} 'name' '${resource.name}' is duplicated`);
        resourceNames.add(resource.name);
        if (!RESOURCE_KINDS.includes(resource.kind)) fail(`${where} 'kind' must be buffer or image`);
        if (!RESOURCE_STATES.includes(resource.initialState)) fail(`${where} 'initialState' is unknown`);
        ["width", "height", "depth"].forEach((dim) => {
          if (!Number.isInteger(resource[dim]) || resource[dim] < 1) fail(`${where} '${dim}' must be an integer >= 1`);
        });
        if (!finiteNum(resource.byteSize) || resource.byteSize < 0) fail(`${where} 'byteSize' must be finite and >= 0`);
      });
      const passNames = new Set();
      if (!Array.isArray(document.passes) || document.passes.length === 0) {
        fail("'passes' must be a non-empty array");
        return { valid: errors.length === 0, errors };
      }
      document.passes.forEach((pass, index) => {
        const where = `pass ${index}`;
        if (!pass || typeof pass !== "object" || Array.isArray(pass)) return fail(`${where} must be an object`);
        if (typeof pass.name !== "string" || !pass.name) return fail(`${where} 'name' must not be empty`);
        if (passNames.has(pass.name)) return fail(`${where} 'name' '${pass.name}' is duplicated`);
        passNames.add(pass.name);
        if (!RENDER_QUEUES.includes(pass.queue)) fail(`${where} 'queue' must be graphics/compute/transfer`);
        if (!Array.isArray(pass.resources)) fail(`${where} 'resources' must be an array`);
        else pass.resources.forEach((access, ai) => {
          if (!access || typeof access !== "object") return fail(`${where} resource[${ai}] must be an object`);
          if (!resourceNames.has(access.resource)) return fail(`${where} resource[${ai}] references unknown resource '${access.resource}'`);
          if (!RENDER_ACCESS.includes(access.access)) fail(`${where} resource[${ai}] 'access' must be read/write/readwrite`);
          if (!RESOURCE_STATES.includes(access.state)) fail(`${where} resource[${ai}] 'state' is unknown`);
        });
      });
      if (!Array.isArray(document.dependencies)) fail("'dependencies' must be an array");
      else document.dependencies.forEach((dep, index) => {
        const where = `dependency ${index}`;
        if (!dep || typeof dep !== "object") return fail(`${where} must be an object`);
        if (!passNames.has(dep.before)) return fail(`${where} references unknown pass '${dep.before}'`);
        if (!passNames.has(dep.after)) return fail(`${where} references unknown pass '${dep.after}'`);
        if (dep.before === dep.after) return fail(`${where} cannot depend on itself`);
      });
      // Cycle check (topological) — mirrors IRenderGraph::compile erroring on
      // a cyclic graph instead of hanging.
      const adjacency = new Map([...passNames].map((name) => [name, []]));
      for (const dep of document.dependencies) adjacency.get(dep.before).push(dep.after);
      const visiting = new Set();
      const visited = new Set();
      const visit = (name) => {
        if (visiting.has(name)) return false;  // cycle
        if (visited.has(name)) return true;
        visiting.add(name);
        for (const next of adjacency.get(name) ?? []) {
          if (!visit(next)) return false;
        }
        visiting.delete(name);
        visited.add(name);
        return true;
      };
      for (const name of passNames) if (!visit(name)) { fail("'dependencies' form a cycle"); break; }
      break;
    }
    case "light": {
      if (!LIGHT_TYPE_NAMES.includes(type)) fail(`'type' must be one of ${LIGHT_TYPE_NAMES.join(", ")}`);
      if (!Array.isArray(color) || color.length !== 3 || !color.every((v) => finiteNum(v) && v >= 0 && v <= 1)) {
        fail("'color' must be [r, g, b] with each in 0..1");
      }
      if (!finiteNum(intensity) || intensity <= 0) fail("'intensity' must be finite and > 0");
      if (!finiteNum(range) || range < 0) fail("'range' must be finite and >= 0");
      break;
    }
    case "gi": {
      const checks = [
        ["cascadeCount", Number.isInteger(cascadeCount) && cascadeCount >= 1 && cascadeCount <= 6],
        ["resolution", Number.isInteger(resolution) && resolution >= 4 && resolution <= 32],
        ["probesPerFrame", Number.isInteger(probesPerFrame) && probesPerFrame >= 1],
        ["baseSpacing", finiteNum(baseSpacing) && baseSpacing >= 0.5],
        ["cascadeScale", finiteNum(cascadeScale) && cascadeScale >= 2],
        ["sunRefreshAngleDegrees", finiteNum(sunRefreshAngleDegrees) && sunRefreshAngleDegrees >= 0.25 && sunRefreshAngleDegrees <= 15],
        ["bounces", Number.isInteger(bounces) && bounces >= 1 && bounces <= 8],
        ["maxDistance", finiteNum(maxDistance) && maxDistance > 0],
        ["intensity", finiteNum(giIntensity) && giIntensity >= 0.01 && giIntensity <= 64]
      ];
      for (const [field, ok] of checks) if (!ok) fail(`'${field}' is out of the contract range`);
      if (!Array.isArray(skylight) || skylight.length !== 3 || !skylight.every(finiteNum)) {
        fail("'skylight' must be [r, g, b] of finite numbers");
      }
      break;
    }
    case "ocean": {
      if (!Number.isInteger(size) || !isPow2(size) || size < 16 || size > 1024) {
        fail("'size' must be a power of two in [16, 1024]");
      }
      const checks = [
        ["tileSizeMeters", finiteNum(tileSizeMeters) && tileSizeMeters >= 16 && tileSizeMeters <= 8192],
        ["windSpeed", finiteNum(windSpeed) && windSpeed >= 0.5 && windSpeed <= 40],
        ["windDirRad", finiteNum(windDirRad)],
        ["choppiness", finiteNum(choppiness) && choppiness >= 0 && choppiness <= 4],
        ["amplitude", finiteNum(amplitude) && amplitude >= 0.01 && amplitude <= 8]
      ];
      for (const [field, ok] of checks) if (!ok) fail(`'${field}' is out of the contract range`);
      if (!Number.isInteger(seed) || seed < 0) fail("'seed' must be a non-negative integer");
      break;
    }
    case "post_process": {
      if (!TONE_OPERATORS.includes(operator)) fail(`'operator' must be one of ${TONE_OPERATORS.join(", ")}`);
      if (!QUALITY_LEVELS.includes(quality)) fail(`'quality' must be one of ${QUALITY_LEVELS.join(", ")}`);
      const checks = [
        ["exposure", finiteNum(exposure) && exposure >= 0.01 && exposure <= 64],
        ["ev100", finiteNum(ev100) && ev100 >= -16 && ev100 <= 16],
        ["whitePoint", finiteNum(whitePoint) && whitePoint >= 1 && whitePoint <= 64],
        ["sharpness", finiteNum(sharpness) && sharpness >= 0 && sharpness <= 1]
      ];
      for (const [field, ok] of checks) if (!ok) fail(`'${field}' is out of the contract range`);
      break;
    }
    case "fluid_sim": {
      const checks = [
        ["gridSize", Number.isInteger(gridSize) && gridSize > 0],
        ["cellSize", finiteNum(cellSize) && cellSize > 0],
        ["gravity", finiteNum(gravity) && gravity > 0],
        ["dt", finiteNum(dt) && dt > 0],
        ["solverIterations", Number.isInteger(solverIterations) && solverIterations > 0],
        ["damping", finiteNum(damping) && damping >= 0 && damping <= 1],
        ["viscosity", finiteNum(viscosity) && viscosity >= 0],
        ["surfaceTension", finiteNum(surfaceTension) && surfaceTension >= 0]
      ];
      for (const [field, ok] of checks) if (!ok) fail(`'${field}' is out of the contract range`);
      break;
    }
    case "world": {
      const worldSeed = document.seed ?? 0;
      const rulesJson = document.rulesJson ?? "";
      const profile = document.profile ?? "";
      if (!Number.isInteger(worldSeed) || worldSeed < 0) fail("'seed' must be a non-negative integer");
      if (typeof rulesJson !== "string") fail("'rulesJson' must be a string");
      if (rulesJson.trim()) {
        try { JSON.parse(rulesJson); } catch { fail("'rulesJson' must be well-formed JSON when non-empty"); }
      }
      if (typeof profile !== "string" || !profile) fail("'profile' must name a world profile asset in Content/Profiles");
      if (!Array.isArray(portals)) fail("'portals' must be an array");
      else portals.forEach((portal, index) => {
        const where = `portal ${index}`;
        if (!portal || typeof portal !== "object") return fail(`${where} must be an object`);
        if (typeof portal.fromWorld !== "string" || !portal.fromWorld) return fail(`${where} 'fromWorld' must not be empty`);
        if (typeof portal.toWorld !== "string" || !portal.toWorld) return fail(`${where} 'toWorld' must not be empty`);
        for (const axis of ["from", "to"]) {
          if (!Array.isArray(portal[axis]) || portal[axis].length !== 3 || !portal[axis].every(finiteNum)) {
            fail(`${where} '${axis}' must be [x, y, z] of finite numbers`);
          }
        }
        if (!finiteNum(portal.yawDegrees)) fail(`${where} 'yawDegrees' must be finite`);
      });
      break;
    }
    case "chunk": {
      const checks = [
        ["chunkBudget", Number.isInteger(chunkBudget) && chunkBudget >= 0],
        ["memoryBudgetBytes", Number.isInteger(memoryBudgetBytes) && memoryBudgetBytes >= 0],
        ["farLodPercent", Number.isInteger(farLodPercent) && farLodPercent >= 0 && farLodPercent <= 100],
        ["workerThreads", Number.isInteger(workerThreads) && workerThreads >= 0]
      ];
      for (const [field, ok] of checks) if (!ok) fail(`'${field}' is out of the contract range`);
      break;
    }
    case "transaction": {
      const edits = document.edits ?? [];
      if (!Number.isInteger(maxEdits) || maxEdits < 0) fail("'maxEdits' must be a non-negative integer");
      if (!Number.isInteger(maxBoxVolume) || maxBoxVolume < 0) fail("'maxBoxVolume' must be a non-negative integer");
      if (!Array.isArray(edits) || edits.length === 0) {
        fail("'edits' must be a non-empty array");
        return { valid: errors.length === 0, errors };
      }
      if (maxEdits > 0 && edits.length > maxEdits) {
        fail(`edit count ${edits.length} exceeds maxEdits ${maxEdits}`);
      }
      let minX = Infinity; let maxX = -Infinity; let minY = Infinity; let maxY = -Infinity; let minZ = Infinity; let maxZ = -Infinity;
      edits.forEach((edit, index) => {
        const where = `edit ${index}`;
        if (!edit || typeof edit !== "object") return fail(`${where} must be an object`);
        if (!Array.isArray(edit.position) || edit.position.length !== 3 || !edit.position.every((v) => Number.isInteger(v))) {
          return fail(`${where} 'position' must be [x, y, z] of integers`);
        }
        if (!Number.isInteger(edit.blockId) || edit.blockId < 0) fail(`${where} 'blockId' must be a non-negative integer (0 = Air)`);
        const [x, y, z] = edit.position;
        minX = Math.min(minX, x); maxX = Math.max(maxX, x);
        minY = Math.min(minY, y); maxY = Math.max(maxY, y);
        minZ = Math.min(minZ, z); maxZ = Math.max(maxZ, z);
      });
      if (maxBoxVolume > 0) {
        const volume = (maxX - minX + 1) * (maxY - minY + 1) * (maxZ - minZ + 1);
        if (volume > maxBoxVolume) fail(`bounding-box volume ${volume} exceeds maxBoxVolume ${maxBoxVolume}`);
      }
      break;
    }
    case "block_entity": {
      if (typeof document.typeId !== "string" || !document.typeId.includes(":")) fail("'typeId' must be a namespaced id (ns:name)");
      if (!Number.isInteger(dataVersion) || dataVersion < 1) fail("'dataVersion' must be an integer >= 1");
      if (typeof scriptId !== "string") fail("'scriptId' must be a string");
      if (!Array.isArray(components)) fail("'components' must be an array");
      else components.forEach((component, index) => {
        const where = `component ${index}`;
        if (!component || typeof component !== "object") return fail(`${where} must be an object`);
        if (!BLOCK_ENTITY_COMPONENT_TYPES.includes(component.type)) fail(`${where} 'type' must be inventory/script/custom`);
        if (!Number.isInteger(component.version) || component.version < 1) fail(`${where} 'version' must be an integer >= 1`);
        if (typeof component.blob !== "string") fail(`${where} 'blob' must be a string`);
      });
      break;
    }
    case "inventory": {
      const slots = document.slots ?? [];
      if (!Array.isArray(slots)) fail("'slots' must be an array");
      else slots.forEach((slot, index) => {
        const where = `slot ${index}`;
        if (slot === null) return;
        if (!slot || typeof slot !== "object") return fail(`${where} must be an object or null`);
        if (typeof slot.item !== "string" || !slot.item.includes(":")) fail(`${where} 'item' must be a namespaced id (ns:name)`);
        if (!Number.isInteger(slot.count) || slot.count < 0) fail(`${where} 'count' must be a non-negative integer`);
        if (!Number.isInteger(slot.damage) || slot.damage < 0) fail(`${where} 'damage' must be a non-negative integer`);
        if (typeof slot.data !== "string") fail(`${where} 'data' must be a string`);
      });
      if (!Array.isArray(filters)) fail("'filters' must be an array");
      else filters.forEach((filter, index) => {
        const where = `filter ${index}`;
        if (!filter || typeof filter !== "object") return fail(`${where} must be an object`);
        if (!Number.isInteger(filter.slot) || filter.slot < 0 || filter.slot >= slots.length) {
          fail(`${where} 'slot' must be a valid slot index (0..${slots.length - 1})`);
        }
        if (!Array.isArray(filter.allowItems) || !filter.allowItems.every((id) => typeof id === "string")) fail(`${where} 'allowItems' must be an array of strings`);
        if (!Array.isArray(filter.allowTags) || !filter.allowTags.every((tag) => typeof tag === "string")) fail(`${where} 'allowTags' must be an array of strings`);
      });
      break;
    }
    default:
      return { valid: false, errors: [`unsupported config kind '${kind}'`] };
  }
  return { valid: errors.length === 0, errors };
}

// ─── CONFIG asset read/inspect/author (mirrors the other author_* tools) ────
function configDir(project, kind) {
  const dirs = {
    shader: project.shaders, render_graph: project.renderGraphs, light: project.lights,
    gi: project.gi, ocean: project.ocean, post_process: project.postProcess,
    fluid_sim: project.fluidSims, world: project.worlds, chunk: project.chunks,
    transaction: project.transactions, block_entity: project.blockEntities,
    inventory: project.inventories
  };
  return dirs[kind];
}

function readConfigAssets(project, kind) {
  const directory = configDir(project, kind);
  const assets = [];
  if (!fs.existsSync(directory)) return assets;
  for (const fileName of fs.readdirSync(directory).filter((file) => file.endsWith(".json")).sort()) {
    const file = path.join(directory, fileName);
    const baseName = fileName.replace(/\.json$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validateConfigDocument(kind, document).errors);
    } else if (document) {
      diagnostics.push("config asset must be a JSON object");
    }
    assets.push({
      kind,
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

// Result key per config kind — matches the inspect_* tool names so the JSON
// is predictable for consumers (inspect_gi_configs -> gi_configs, ...).
const CONFIG_INSPECT_KEYS = Object.freeze({
  shader: "shaders", render_graph: "render_graphs", light: "lights", gi: "gi_configs",
  ocean: "ocean_configs", post_process: "post_processings", fluid_sim: "fluid_simulations",
  world: "worlds", chunk: "chunk_configs", transaction: "block_transactions",
  block_entity: "block_entities", inventory: "inventories"
});

function inspectConfigAssets(engineRoot, projectName, kind) {
  const project = requireProject(engineRoot, projectName);
  const assets = readConfigAssets(project, kind);
  const key = CONFIG_INSPECT_KEYS[kind] ?? `${kind}s`;
  return { project: project.project, [key]: assets, count: assets.length };
}

function configDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorConfigAsset(engineRoot, args, kind) {
  const project = requireProject(engineRoot, args.project);
  if (!CONFIG_KINDS.includes(kind)) throw new Error(`unsupported config kind '${kind}' (supported: ${CONFIG_KINDS.join(", ")})`);
  const name = assetName(args.name);
  const document = buildConfigDocument(kind, args);
  const validation = validateConfigDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: `${kind} fails public-contract validation; nothing was written`
    };
  }
  const file = path.join(configDir(project, kind), `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? configDiff(previous, document) : null;
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
  if (previous && !args.update) throw new Error(`${kind} '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
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
    abilities: path.join(root, "Content", "Abilities"),
    missions: path.join(root, "Content", "Missions"),
    profiles: path.join(root, "Content", "Profiles"),
    animations: path.join(root, "Content", "Animations"),
    simulationLod: path.join(root, "Content", "SimulationLod"),
    prefabs: path.join(root, "Content", "Prefabs"),
    particles: path.join(root, "Content", "Particles"),
    shaders: path.join(root, "Content", "Shaders"),
    renderGraphs: path.join(root, "Content", "RenderGraphs"),
    lights: path.join(root, "Content", "Lights"),
    gi: path.join(root, "Content", "GI"),
    ocean: path.join(root, "Content", "Ocean"),
    postProcess: path.join(root, "Content", "PostProcess"),
    fluidSims: path.join(root, "Content", "FluidSims"),
    worlds: path.join(root, "Content", "Worlds"),
    chunks: path.join(root, "Content", "Chunks"),
    transactions: path.join(root, "Content", "Transactions"),
    blockEntities: path.join(root, "Content", "BlockEntities"),
    inventories: path.join(root, "Content", "Inventories")
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
      "inspect ability assets", "dry-run ability asset updates",
      "author mission assets (data-driven missions: reach/collect/kill/interact objectives, dialogue graph with condition-gated choices, unlock conditions, rewards)",
      "inspect mission assets", "dry-run mission asset updates",
      "author world profiles (ONE JSON composing height/climate/biomes/caves/ores/carver/decorators/structures into the world generator — IWorldProfile)",
      "inspect world profiles", "dry-run world profile updates",
      "author gait assets (data-driven creature locomotion: cycle timing + per-leg phase offsets + hip-anchored two-bone leg chains — IContactPlanner/GaitAsset)",
      "inspect gait assets", "dry-run gait asset updates",
      "author simulation LOD specs (data-driven region simulation budgets: tiers Full/Coarse/Aggregate/Sleeping by relevance + world clock + region grid — ISimulationLod)",
      "inspect simulation LOD specs", "dry-run simulation LOD spec updates",
      "author prefabs (reusable entity sets extracted from scenes — entity ids stripped, internal Hierarchy links remapped to entity names; Content/Prefabs/<name>.prefab)",
      "instantiate prefabs (insert into a target scene with fresh UUIDs, internal Hierarchy remapped, optional offset/parent)",
      "inspect prefabs",
      "author particle assets (reusable emitter configurations — fields exactly the public ParticleEmitter component schema; Content/Particles/<name>.particle)",
      "apply particle assets (write an asset into an entity's ParticleEmitter component through the scene component validation)",
      "inspect particle assets"
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
    // Mission assets (FALTANTES item 23 — missões e diálogos): data-driven
    // missions (reach/collect/kill/interact objectives, unlock conditions,
    // rewards) with a dialogue graph (condition-gated choices) under
    // Content/Missions/<name>.json.
    mission_asset_kinds: MISSION_KINDS.map((kind) => ({
      kind,
      file: `Content/Missions/<name>.json`,
      fields: MISSION_FIELD_SCHEMAS[kind],
      objective_fields: MISSION_OBJECTIVE_FIELDS,
      condition_fields: MISSION_CONDITION_FIELDS
    })),
    mission_schemas: Object.fromEntries(
      MISSION_KINDS.map((kind) => [kind, buildMissionJsonSchema(kind)])
    ),
    mission_validation: {
      note: "Each document mirrors exactly the versioned JSON the public C++ factory parses (MissionDefinition::load_from_json, implemented by src/engine/sdk/MissionAsset.cpp); the MCP validates structure only — the runtime validates the same document again on load (all-or-nothing).",
      dry_run: "author_mission_asset accepts dry_run: true to validate and preview the document/diff without writing.",
      rollback: "Updates return the previous document; re-authoring it with update: true restores the prior state."
    },
    // World profiles (FALTANTES item 23 "editar geração procedural"): ONE
    // versioned JSON composing the whole generation pipeline into the world
    // generator (IWorldProfile, engine/procgen/IWorldProfile.hpp by AGENT-3
    // — findings #120-worldprofile) under Content/Profiles/<name>.json.
    world_profile_asset_kinds: WORLD_PROFILE_KINDS.map((kind) => ({
      kind,
      file: `Content/Profiles/<name>.json`,
      fields: WORLD_PROFILE_FIELD_SCHEMAS[kind],
      climate_axes: WORLD_PROFILE_CLIMATE_AXES
    })),
    world_profile_schemas: Object.fromEntries(
      WORLD_PROFILE_KINDS.map((kind) => [kind, buildWorldProfileJsonSchema(kind)])
    ),
    world_profile_validation: {
      note: "The MCP validates the TOP-LEVEL structure (version, section types, amplitude >= 0, finite scale/offset); every present section is validated all-or-nothing by its OWN subsystem parser when the C++ factory loads the document (create_world_profile_from_json, src/engine/sdk/WorldProfile.cpp) — the MCP never re-implements the subsystem parsers.",
      dry_run: "author_world_profile_asset accepts dry_run: true to validate and preview the document/diff without writing.",
      rollback: "Updates return the previous document; re-authoring it with update: true restores the prior state."
    },
    // Gait assets (FALTANTES item 23 "animações"): a data-driven creature
    // locomotion asset (cycle timing + per-leg phase offsets + hip-anchored
    // two-bone leg chains) under Content/Animations/<name>.json.
    gait_asset_kinds: GAIT_KINDS.map((kind) => ({
      kind,
      file: `Content/Animations/<name>.json`,
      fields: GAIT_FIELD_SCHEMAS[kind]
    })),
    gait_schemas: Object.fromEntries(
      GAIT_KINDS.map((kind) => [kind, buildGaitJsonSchema(kind)])
    ),
    gait_validation: {
      note: "Each document mirrors exactly the versioned JSON the public C++ factory parses (GaitAsset::load_from_json / LegChainAsset::load_from_json, implemented by src/engine/sdk/GaitPlanner.cpp — bit-exact %.9g round-trip, all-or-nothing); the MCP validates structure only — the runtime validates the same document again on load.",
      dry_run: "author_gait_asset accepts dry_run: true to validate and preview the document/diff without writing.",
      rollback: "Updates return the previous document; re-authoring it with update: true restores the prior state."
    },
    // Simulation LOD specs (FALTANTES §20 — SimulationLodSpec, the AGENT-4
    // Item 20 contract): region simulation budgets by relevance under
    // Content/SimulationLod/<name>.json.
    simulation_lod_asset_kinds: SIMULATION_LOD_KINDS.map((kind) => ({
      kind,
      file: `Content/SimulationLod/<name>.json`,
      fields: SIMULATION_LOD_FIELD_SCHEMAS[kind],
      tier_modes: [...SIMULATION_LOD_MODES]
    })),
    simulation_lod_schemas: Object.fromEntries(
      SIMULATION_LOD_KINDS.map((kind) => [kind, buildSimulationLodJsonSchema(kind)])
    ),
    simulation_lod_validation: {
      note: "Each document mirrors exactly the versioned JSON the public C++ factory parses (SimulationLodSpec::load_from_json, implemented by src/engine/sdk/SimulationLod.cpp — bit-exact %.9g round-trip, all-or-nothing); the MCP validates structure only — the runtime validates the same document again on load.",
      dry_run: "author_simulation_lod_spec accepts dry_run: true to validate and preview the document/diff without writing.",
      rollback: "Updates return the previous document; re-authoring it with update: true restores the prior state."
    },
    // Prefabs (FALTANTES item 23 — "cenas, entidades, componentes e
    // prefabs"): reusable entity sets extracted from scenes under
    // Content/Prefabs/<name>.prefab; instantiated into target scenes with
    // fresh UUIDs (internal Hierarchy.parent_id remapped).
    prefab_asset_kinds: PREFAB_KINDS.map((kind) => ({
      kind,
      file: `Content/Prefabs/<name>.prefab`,
      fields: PREFAB_FIELD_SCHEMAS[kind]
    })),
    prefab_schemas: Object.fromEntries(
      PREFAB_KINDS.map((kind) => [kind, buildPrefabJsonSchema(kind)])
    ),
    prefab_validation: {
      note: "A prefab is a scene-shaped document with entity ids STRIPPED (they are regenerated on instantiation). create_prefab extracts entity documents from an existing scene preserving every component; instantiate_prefab inserts them into a target scene with fresh UUIDs and remaps internal Hierarchy.parent_id links (links to entities outside the prefab are preserved — the target scene must provide them).",
      dry_run: "create_prefab accepts dry_run: true to preview the extracted document without writing.",
      rollback: "create_prefab with update: true replaces the asset and returns the previous document for rollback; instantiate_prefab is append-only (never rewrites existing entities)."
    },
    // Particle emitter assets (FALTANTES item 23 — partículas): reusable
    // emitter configurations under Content/Particles/<name>.particle, applied
    // to an entity's ParticleEmitter component via apply_particle_asset.
    particle_asset_kinds: PARTICLE_KINDS.map((kind) => ({
      kind,
      file: `Content/Particles/<name>.particle`,
      fields: PARTICLE_FIELD_SCHEMAS[kind]
    })),
    particle_schemas: Object.fromEntries(
      PARTICLE_KINDS.map((kind) => [kind, buildParticleJsonSchema(kind)])
    ),
    particle_validation: {
      note: "A particle asset is EXACTLY the public ParticleEmitter component schema (derived from COMPONENT_SCHEMAS.ParticleEmitter — single source of truth, no duplication) plus name/version. create_particle_asset validates every emitter field (type + finite); apply_particle_asset writes the asset into an entity's ParticleEmitter component through the same component validation the scene uses.",
      dry_run: "create_particle_asset accepts dry_run: true to preview the document without writing.",
      rollback: "create_particle_asset with update: true replaces the asset and returns the previous document for rollback."
    },
    // §4.4/§4.5 — rendering + voxel/world CONFIG assets (2026-08-28): the
    // data surfaces the public C++ rendering/voxel contracts parse. Each
    // document is a versioned JSON mirror of the C++ contract JSON surface
    // (all-or-nothing; never clamped); game_capabilities surfaces the field
    // contracts + generated draft-07 schemas (single source for export).
    config_asset_kinds: CONFIG_KINDS.map((kind) => ({
      kind,
      file: `Content/${kindDirName(kind)}/<name>.json`,
      fields: CONFIG_FIELD_SCHEMAS[kind]
    })),
    config_schemas: Object.fromEntries(
      CONFIG_KINDS.map((kind) => [kind, buildConfigJsonSchema(kind)])
    ),
    config_validation: {
      note: "Each config asset mirrors exactly the versioned JSON the public C++ contracts parse — IShaderCompiler (shader), IRenderGraph (render_graph), Light component (light), IGlobalIlluminationProvider/IDiffuseGlobalIllumination (gi), IFftOceanSurface (ocean), IToneMapping/ICasSharpening/IRenderingPresets (post_process), IFluidSimulation (fluid_sim), IWorldManager (world), IVoxelStreaming (chunk), IVoxelWorld (transaction), IVoxelBlockEntity (block_entity), engine/registry/Inventory.hpp (inventory). The MCP validates structure + ranges all-or-nothing; the runtime validates the same document again on load.",
      dry_run: "Every author_*/create_* config tool accepts dry_run: true to validate and preview the document/diff without writing.",
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

// Maps a config kind to its content directory name (for capability docs).
function kindDirName(kind) {
  const names = {
    shader: "Shaders", render_graph: "RenderGraphs", light: "Lights", gi: "GI",
    ocean: "Ocean", post_process: "PostProcess", fluid_sim: "FluidSims",
    world: "Worlds", chunk: "Chunks", transaction: "Transactions",
    block_entity: "BlockEntities", inventory: "Inventories"
  };
  return names[kind];
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
      name: "author_mission_asset",
      description: "Author a data-driven mission (FALTANTES item 23) as a versioned JSON document in Content/Missions/, mirroring exactly the public C++ mission JSON schema (MissionDefinition::load_from_json). Objectives (reach/collect/kill/interact) all must complete; unlockConditions gate acceptance; a dialogue graph (nodes with condition-gated choices) must declare a 'start' node; reward (itemId/count/xp/setFlag) applies on completion; repeatable missions can be accepted again. Validates structure against the public contract; dry_run previews the document/diff without writing; update replaces an existing asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object",
        required: ["project", "name", "objectives"],
        properties: {
          project: { type: "string" },
          name: { type: "string", minLength: 1, description: "mission name (becomes the file name)" },
          dry_run: { type: "boolean", default: false },
          update: { type: "boolean", default: false },
          id: { type: "string" },
          version: { type: "integer", default: 1 },
          objectives: {
            type: "array", minItems: 1, items: {
              type: "object", properties: {
                id: { type: "string" },
                kind: { type: "string", enum: [...MISSION_OBJECTIVE_KINDS] },
                target: { type: "string", description: "item/entity/interaction id (required for collect/kill/interact)" },
                count: { type: "integer", default: 1, description: ">= 1" },
                x: { type: "number", default: 0 },
                z: { type: "number", default: 0 },
                radius: { type: "number", default: 0, description: ">= 0" },
                conditions: {
                  type: "array", items: {
                    type: "object", properties: {
                      kind: { type: "string", enum: [...MISSION_CONDITION_KINDS] },
                      key: { type: "string" },
                      op: { type: "string", enum: [...MISSION_OPS] },
                      value: { type: "number" },
                      flagValue: { type: "boolean" }
                    }, additionalProperties: false
                  }
                }
              }, additionalProperties: false
            }
          },
          dialogue: {
            type: "array", items: {
              type: "object", properties: {
                id: { type: "string", description: "must include 'start'" },
                speaker: { type: "string" },
                text: { type: "string" },
                choices: {
                  type: "array", items: {
                    type: "object", properties: {
                      text: { type: "string" },
                      next: { type: "string", description: "target node id or empty to end" },
                      conditions: {
                        type: "array", items: {
                          type: "object", properties: {
                            kind: { type: "string", enum: [...MISSION_CONDITION_KINDS] },
                            key: { type: "string" },
                            op: { type: "string", enum: [...MISSION_OPS] },
                            value: { type: "number" },
                            flagValue: { type: "boolean" }
                          }, additionalProperties: false
                        }
                      }
                    }, additionalProperties: false
                  }
                }
              }, additionalProperties: false
            }
          },
          unlockConditions: {
            type: "array", items: {
              type: "object", properties: {
                kind: { type: "string", enum: [...MISSION_CONDITION_KINDS] },
                key: { type: "string" },
                op: { type: "string", enum: [...MISSION_OPS] },
                value: { type: "number" },
                flagValue: { type: "boolean" }
              }, additionalProperties: false
            }
          },
          reward: {
            type: "object", properties: {
              itemId: { type: "string" },
              count: { type: "integer", default: 0, description: ">= 0" },
              xp: { type: "integer", default: 0, description: ">= 0" },
              setFlag: { type: "string" }
            }, additionalProperties: false
          },
          repeatable: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_mission_assets",
      description: "List and validate every mission asset under Content/Missions for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_world_profile_asset",
      description: "Author a data-driven world profile (FALTANTES item 23 — geração procedural) as a versioned JSON document in Content/Profiles/, mirroring the public C++ world-profile document (create_world_profile_from_json, IWorldProfile). ONE document composes the whole generation pipeline: height (noise graph) + baseHeight/amplitude, climate axes (each an optional noise graph), biomes, caves/ores (density + scale/offset + ore table), carver, decorators and structures (definitions + spawn rules). The MCP validates the top-level structure only; every present section is validated all-or-nothing by its OWN subsystem parser when the C++ factory loads the document. dry_run previews the document/diff without writing; update replaces an existing asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object",
        required: ["project", "name"],
        properties: {
          project: { type: "string" },
          name: { type: "string", minLength: 1, description: "profile name (becomes the file name)" },
          dry_run: { type: "boolean", default: false },
          update: { type: "boolean", default: false },
          version: { type: "integer", default: 1 },
          height: { type: "object", description: "noise graph JSON document (2D height field)" },
          baseHeight: { type: "integer", default: 0 },
          amplitude: { type: "integer", default: 1, description: ">= 0" },
          climate: {
            type: "object", description: "optional axes, each a noise graph JSON document",
            properties: Object.fromEntries(WORLD_PROFILE_CLIMATE_AXES.map((axis) => [axis, { type: "object" }])),
            additionalProperties: false
          },
          biomes: { type: "object", description: "biome registry JSON document" },
          caves: { type: "object", description: "{ density?: noise graph JSON, scale?: number, offset?: number }" },
          ores: { type: "object", description: "{ density?: noise graph JSON, scale?: number, offset?: number, table?: ore table JSON }" },
          carver: { type: "object", description: "carver JSON document" },
          decorators: { type: "object", description: "decorator set JSON document" },
          structures: { type: "object", description: "structure placement document (definitions + spawn rules)" }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_world_profile_assets",
      description: "List and validate every world profile under Content/Profiles for a project (top-level structural diagnostics).",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_gait_asset",
      description: "Author a data-driven gait asset (FALTANTES item 23 — animações) as a versioned JSON document in Content/Animations/, mirroring exactly the public C++ gait JSON schema (GaitAsset::load_from_json, IContactPlanner — creature locomotion: cycle timing + per-leg phase offsets + hip-anchored two-bone leg chains). Validates structure against the public contract (name non-empty, cycleDuration > 0, stanceFraction in (0, 1), stepHeight >= 0, maxStride > 0, legs non-empty with distinct bone indices when set, legPhases in [0, 1) sized to legs); dry_run previews the document/diff without writing; update replaces an existing asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object",
        required: ["project", "name"],
        properties: {
          project: { type: "string" },
          name: { type: "string", minLength: 1, description: "gait name (becomes the file name)" },
          dry_run: { type: "boolean", default: false },
          update: { type: "boolean", default: false },
          version: { type: "integer", default: 1 },
          cycleDuration: { type: "number", default: 1.0, description: "> 0" },
          stanceFraction: { type: "number", default: 0.6, description: "in (0, 1)" },
          stepHeight: { type: "number", default: 0.25, description: ">= 0" },
          maxStride: { type: "number", default: 0.5, description: "> 0" },
          legPhases: { type: "array", items: { type: "number" }, description: "in [0, 1); size must equal legs" },
          legs: {
            type: "array",
            items: {
              type: "object",
              required: ["name"],
              properties: {
                name: { type: "string", minLength: 1 },
                hipOffset: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 },
                upperLength: { type: "number", description: "> 0" },
                lowerLength: { type: "number", description: "> 0" },
                restOffset: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 },
                maxReach: { type: "number", description: ">= 0; 0 = auto (upper + lower)" },
                hipBone: { type: "integer", description: "-1 = unbound" },
                kneeBone: { type: "integer", description: "-1 = unbound" },
                footBone: { type: "integer", description: "-1 = unbound" }
              },
              additionalProperties: false
            }
          }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_gait_assets",
      description: "List and validate every gait asset under Content/Animations for a project (structural diagnostics mirroring GaitAsset::load_from_json).",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_simulation_lod_spec",
      description: "Author a data-driven simulation LOD spec (FALTANTES §20) as a versioned JSON document in Content/SimulationLod/, mirroring exactly the public C++ simulation LOD JSON schema (SimulationLodSpec::load_from_json, ISimulationLod — region simulation budgets: tiers Full/Coarse/Aggregate/Sleeping selected by relevance, world clock + region grid). Validates structure against the public contract (version 1, cellSize > 0, fullRadius >= 0, falloffRadius > fullRadius, dayLengthSeconds > 0, daysPerSeason >= 1, tiers non-empty with unique names, strictly descending minRelevance in [0, 1], finite non-negative intervals/budgets); dry_run previews the document/diff without writing; update replaces an existing asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object",
        required: ["project", "name"],
        properties: {
          project: { type: "string" },
          name: { type: "string", minLength: 1, description: "spec name (becomes the file name)" },
          dry_run: { type: "boolean", default: false },
          update: { type: "boolean", default: false },
          version: { type: "integer", default: 1 },
          cellSize: { type: "number", default: 16.0, description: "> 0" },
          fullRadius: { type: "number", default: 48.0, description: ">= 0" },
          falloffRadius: { type: "number", default: 320.0, description: "> fullRadius" },
          dayLengthSeconds: { type: "number", default: 240.0, description: "> 0" },
          daysPerSeason: { type: "integer", default: 30, description: ">= 1" },
          tiers: {
            type: "array",
            minItems: 1,
            items: {
              type: "object",
              required: ["name", "mode"],
              properties: {
                name: { type: "string", minLength: 1, description: "unique tier name" },
                mode: { type: "string", enum: [...SIMULATION_LOD_MODES] },
                minRelevance: { type: "number", description: "in [0, 1]; tiers must be sorted strictly descending" },
                updateInterval: { type: "number", default: 0, description: ">= 0" },
                sleepAfterIdle: { type: "number", default: 0, description: ">= 0" },
                maxRegions: { type: "integer", default: 0, description: ">= 0; 0 = unlimited" },
                aggregateInterval: { type: "number", default: 0, description: ">= 0" }
              },
              additionalProperties: false
            }
          }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_simulation_lod_specs",
      description: "List and validate every simulation LOD spec under Content/SimulationLod for a project (structural diagnostics mirroring SimulationLodSpec::load_from_json).",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "create_prefab",
      description: "Extract a reusable entity set (prefab) from an existing scene into Content/Prefabs/<name>.prefab — the entity documents are preserved with every component but their ids are STRIPPED (regenerated on instantiation). Entity ids not found in the scene are refused (all-or-nothing: nothing written). dry_run previews the extracted document without writing; update replaces the asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object", required: ["project", "scene", "name", "entity_ids"],
        properties: {
          project: { type: "string" }, scene: { type: "string" }, name: { type: "string" },
          entity_ids: { type: "array", items: { type: "string", format: "uuid" } },
          root_entity: { type: "string", description: "name of the prefab root (instantiation origin when an offset is applied)" },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "instantiate_prefab",
      description: "Insert a prefab (Content/Prefabs/<name>.prefab) into a target scene with FRESH UUIDs — internal Hierarchy.parent_id links are remapped to the new ids; links to entities OUTSIDE the prefab are preserved (the target scene must provide them). Optional offset shifts EVERY prefab entity's Transform position by [x, y, z] (scene transforms are global — the whole set moves together, spatial relations preserved). Append-only: existing entities are never rewritten; a missing prefab or scene is refused before any write.",
      inputSchema: {
        type: "object", required: ["project", "scene", "prefab"],
        properties: {
          project: { type: "string" }, scene: { type: "string" }, prefab: { type: "string" },
          offset: { type: "array", items: { type: "number" }, description: "[x, y, z] world offset applied to the prefab root (default [0, 0, 0])" },
          parent_id: { type: "string", format: "uuid", description: "target entity to parent the prefab root under (sets the root's Hierarchy.parent_id)" }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_prefabs",
      description: "List and validate every prefab under Content/Prefabs for a project (structural diagnostics: format/version/entities without ids, component field checks mirroring scene validation).",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "create_particle_asset",
      description: "Author a reusable particle emitter asset under Content/Particles/<name>.particle — the emitter fields are EXACTLY the public ParticleEmitter component schema (derived from COMPONENT_SCHEMAS, single source of truth). Unknown fields or wrong types are refused all-or-nothing (nothing written, diagnostics naming the field). dry_run previews the document; update replaces the asset and returns the previous document for rollback.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: true
      }
    },
    {
      name: "apply_particle_asset",
      description: "Apply a particle asset (Content/Particles/<name>.particle) to an entity's ParticleEmitter component in a scene — the component gets every emitter field from the asset (missing fields fall back to the component defaults). The entity must exist; the asset must validate; nothing is written otherwise.",
      inputSchema: {
        type: "object", required: ["project", "scene", "entity_id", "asset"],
        properties: {
          project: { type: "string" }, scene: { type: "string" }, entity_id: { type: "string", format: "uuid" },
          asset: { type: "string" }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_particle_assets",
      description: "List and validate every particle asset under Content/Particles for a project (structural diagnostics mirroring create_particle_asset validation).",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    // §4.4/§4.5 — rendering + voxel/world CONFIG assets (2026-08-28). Each
    // author tool mirrors the public C++ contract JSON surface (all-or-nothing)
    // and accepts dry_run/update/rollback like every other author tool.
    {
      name: "create_shader_asset",
      description: "Author a shader asset (Content/Shaders/<name>.json) mirroring IShaderCompiler.ShaderCompilerConfig: GLSL source, stage, target_env, opt_level, defines. Validates structure against the public contract; dry_run previews; update replaces and returns the previous document for rollback.",
      inputSchema: {
        type: "object", required: ["project", "name", "source"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          source: { type: "string" },
          stage: { type: "string", enum: [...SHADER_STAGES], default: "fragment" },
          target_env: { type: "string", default: "" },
          opt_level: { type: "integer", minimum: 0, maximum: 2, default: 0 },
          defines: { type: "array", items: { type: "string" }, default: [] },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_shader_assets",
      description: "List and validate every shader asset under Content/Shaders for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_render_graph",
      description: "Author a render graph asset (Content/RenderGraphs/<name>.json) mirroring IRenderGraph: resources (name/kind/byte_size/width/height/depth/transient/imported/initial_state), passes (name/queue/enabled/resources with access+state), dependencies (before/after). Validates unique names, resource references and acyclicity (topological).",
      inputSchema: {
        type: "object", required: ["project", "name", "resources", "passes"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          resources: { type: "array", items: { type: "object" } },
          passes: { type: "array", items: { type: "object" } },
          dependencies: { type: "array", items: { type: "object" }, default: [] },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_render_graphs",
      description: "List and validate every render graph asset under Content/RenderGraphs for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "create_light_asset",
      description: "Author a light asset (Content/Lights/<name>.json) mirroring the Light component schema + game_capabilities light types: type (directional/point/spot/area), color [r,g,b], intensity, range, cast_shadows.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          type: { type: "string", enum: [...LIGHT_TYPE_NAMES], default: "directional" },
          color: { type: "array", items: { type: "number" }, default: [1, 1, 1] },
          intensity: { type: "number", default: 1000 },
          range: { type: "number", default: 50 },
          cast_shadows: { type: "boolean", default: true },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_light_assets",
      description: "List and validate every light asset under Content/Lights for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_gi_config",
      description: "Author a global-illumination config (Content/GI/<name>.json) mirroring GiClipmapConfig (IGlobalIlluminationProvider) + DiffuseGiConfig (IDiffuseGlobalIllumination): cascade_count/resolution/probes_per_frame/base_spacing/cascade_scale/sun_refresh_angle_degrees/bounces/skylight/max_distance/intensity.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          cascade_count: { type: "integer", minimum: 1, maximum: 6, default: 6 },
          resolution: { type: "integer", minimum: 4, maximum: 32, default: 16 },
          probes_per_frame: { type: "integer", minimum: 1, default: 192 },
          base_spacing: { type: "number", default: 4.0 },
          cascade_scale: { type: "number", default: 4.0 },
          sun_refresh_angle_degrees: { type: "number", default: 2.0 },
          bounces: { type: "integer", minimum: 1, maximum: 8, default: 2 },
          skylight: { type: "array", items: { type: "number" }, default: [0.05, 0.07, 0.10] },
          max_distance: { type: "number", default: 128.0 },
          intensity: { type: "number", default: 1.0 },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_gi_configs",
      description: "List and validate every GI config under Content/GI for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_ocean_config",
      description: "Author an FFT ocean surface config (Content/Ocean/<name>.json) mirroring FftOceanConfig (IFftOceanSurface): size (power of two 16..1024), tile_size_meters, wind_speed, wind_dir_rad, choppiness, amplitude, seed.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          size: { type: "integer", minimum: 16, maximum: 1024, default: 64 },
          tile_size_meters: { type: "number", default: 256.0 },
          wind_speed: { type: "number", default: 18.0 },
          wind_dir_rad: { type: "number", default: 0.7 },
          choppiness: { type: "number", default: 1.2 },
          amplitude: { type: "number", default: 0.9 },
          seed: { type: "integer", default: 1 },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_ocean_configs",
      description: "List and validate every ocean config under Content/Ocean for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_post_processing",
      description: "Author a post-processing config (Content/PostProcess/<name>.json) mirroring ToneMappingConfig (IToneMapping) + CasConfig (ICasSharpening) + IRenderingPresets quality: operator/exposure/use_ev/ev100/white_point/sharpness/clamp_values/quality.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          operator: { type: "string", enum: [...TONE_OPERATORS], default: "aces" },
          exposure: { type: "number", default: 1.0 },
          use_ev: { type: "boolean", default: false },
          ev100: { type: "number", default: 0.0 },
          white_point: { type: "number", default: 11.2 },
          sharpness: { type: "number", default: 0.4 },
          clamp_values: { type: "boolean", default: true },
          quality: { type: "string", enum: [...QUALITY_LEVELS], default: "high" },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_post_processings",
      description: "List and validate every post-processing config under Content/PostProcess for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_fluid_simulation",
      description: "Author a fluid simulation config (Content/FluidSims/<name>.json) mirroring FluidConfig (IFluidSimulation): grid_size, cell_size, gravity, dt, solver_iterations, damping, viscosity, surface_tension.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          grid_size: { type: "integer", minimum: 1, default: 64 },
          cell_size: { type: "number", default: 1.0 },
          gravity: { type: "number", default: 9.81 },
          dt: { type: "number", default: 0.0166667 },
          solver_iterations: { type: "integer", minimum: 1, default: 4 },
          damping: { type: "number", default: 0.999 },
          viscosity: { type: "number", default: 0.001 },
          surface_tension: { type: "number", default: 0.0 },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_fluid_simulations",
      description: "List and validate every fluid simulation config under Content/FluidSims for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_world_asset",
      description: "Author a world asset (Content/Worlds/<name>.json) mirroring IWorldManager.WorldSpec: seed, rules_json (well-formed JSON), profile (world profile asset name in Content/Profiles), save_path, portals (from/to world anchors + yaw).",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          seed: { type: "integer", default: 0 },
          rules_json: { type: "string", default: "" },
          profile: { type: "string", default: "" },
          save_path: { type: "string", default: "" },
          portals: { type: "array", items: { type: "object" }, default: [] },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_world_assets",
      description: "List and validate every world asset under Content/Worlds for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_chunk_config",
      description: "Author a chunk streaming config (Content/Chunks/<name>.json) mirroring IVoxelStreaming budgets: chunk_budget, memory_budget_bytes, far_lod_percent, worker_threads.",
      inputSchema: {
        type: "object", required: ["project", "name"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          chunk_budget: { type: "integer", minimum: 0, default: 16 },
          memory_budget_bytes: { type: "integer", minimum: 0, default: 0 },
          far_lod_percent: { type: "integer", minimum: 0, maximum: 100, default: 0 },
          worker_threads: { type: "integer", minimum: 0, default: 0 },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_chunk_configs",
      description: "List and validate every chunk config under Content/Chunks for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_block_transaction",
      description: "Author a block transaction document (Content/Transactions/<name>.json) mirroring IVoxelWorld BlockEdit[] + TransactionLimits: edits [{ position: [x,y,z], block_id }], max_edits, max_box_volume. Validation computes the bounding-box volume and edit count against the limits (dry-run diff).",
      inputSchema: {
        type: "object", required: ["project", "name", "edits"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          edits: { type: "array", items: { type: "object" } },
          max_edits: { type: "integer", minimum: 0, default: 0 },
          max_box_volume: { type: "integer", minimum: 0, default: 0 },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_block_transactions",
      description: "List and validate every block transaction under Content/Transactions for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_block_entity",
      description: "Author a block entity definition (Content/BlockEntities/<name>.json) mirroring IVoxelBlockEntity: type_id (namespaced), data_version, script_id, components (type inventory/script/custom, version, blob).",
      inputSchema: {
        type: "object", required: ["project", "name", "type_id"],
        properties: {
          project: { type: "string" }, name: { type: "string" }, type_id: { type: "string" },
          data_version: { type: "integer", minimum: 1, default: 1 },
          script_id: { type: "string", default: "" },
          components: { type: "array", items: { type: "object" }, default: [] },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_block_entities",
      description: "List and validate every block entity definition under Content/BlockEntities for a project.",
      inputSchema: {
        type: "object", required: ["project"], properties: { project: { type: "string" } }, additionalProperties: false
      }
    },
    {
      name: "author_inventory",
      description: "Author an inventory asset (Content/Inventories/<name>.json) mirroring engine/registry/Inventory.hpp serialize_json: slots [{ item (namespaced), count, damage, data } | null], filters [{ slot, allow_items, allow_tags, allow_any }].",
      inputSchema: {
        type: "object", required: ["project", "name", "slots"],
        properties: {
          project: { type: "string" }, name: { type: "string" },
          slots: { type: "array", items: {} },
          filters: { type: "array", items: { type: "object" }, default: [] },
          dry_run: { type: "boolean", default: false }, update: { type: "boolean", default: false }
        },
        additionalProperties: false
      }
    },
    {
      name: "inspect_inventories",
      description: "List and validate every inventory asset under Content/Inventories for a project.",
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
    },
    {
      name: "run_batch",
      description: "Transactional command bus: apply N authoring operations (author_registry_asset, author_vehicle_asset, author_ability_asset, author_mission_asset, author_world_profile_asset, author_gait_asset, author_simulation_lod_spec, create_prefab, create_particle_asset) all-or-nothing — every operation is validated (dry_run) before anything is written; on any apply failure the already-applied operations are rolled back (updates restored, creates removed). dry_run: true validates the whole batch without writing.",
      inputSchema: {
        type: "object",
        required: ["project", "operations"],
        properties: {
          project: { type: "string" },
          operations: {
            type: "array",
            minItems: 1,
            items: {
              type: "object",
              required: ["tool", "args"],
              properties: {
                tool: { type: "string" },
                args: { type: "object" }
              },
              additionalProperties: false
            }
          },
          dry_run: { type: "boolean", default: false }
        },
        additionalProperties: false
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
    case "author_mission_asset": return authorMissionAsset(engineRoot, args);
    case "inspect_mission_assets": return inspectMissionAssets(engineRoot, args.project);
    case "author_world_profile_asset": return authorWorldProfileAsset(engineRoot, args);
    case "inspect_world_profile_assets": return inspectWorldProfileAssets(engineRoot, args.project);
    case "author_gait_asset": return authorGaitAsset(engineRoot, args);
    case "inspect_gait_assets": return inspectGaitAssets(engineRoot, args.project);
    case "author_simulation_lod_spec": return authorSimulationLodSpec(engineRoot, args);
    case "inspect_simulation_lod_specs": return inspectSimulationLodSpecs(engineRoot, args.project);
    case "create_prefab": return createPrefab(engineRoot, args);
    case "instantiate_prefab": return instantiatePrefab(engineRoot, args);
    case "inspect_prefabs": return inspectPrefabs(engineRoot, args.project);
    case "create_particle_asset": return createParticleAsset(engineRoot, args);
    case "apply_particle_asset": return applyParticleAsset(engineRoot, args);
    case "inspect_particle_assets": return inspectParticleAssets(engineRoot, args.project);
    // §4.4/§4.5 — rendering + voxel/world CONFIG assets (2026-08-28).
    case "create_shader_asset": return authorConfigAsset(engineRoot, args, "shader");
    case "inspect_shader_assets": return inspectConfigAssets(engineRoot, args.project, "shader");
    case "author_render_graph": return authorConfigAsset(engineRoot, args, "render_graph");
    case "inspect_render_graphs": return inspectConfigAssets(engineRoot, args.project, "render_graph");
    case "create_light_asset": return authorConfigAsset(engineRoot, args, "light");
    case "inspect_light_assets": return inspectConfigAssets(engineRoot, args.project, "light");
    case "author_gi_config": return authorConfigAsset(engineRoot, args, "gi");
    case "inspect_gi_configs": return inspectConfigAssets(engineRoot, args.project, "gi");
    case "author_ocean_config": return authorConfigAsset(engineRoot, args, "ocean");
    case "inspect_ocean_configs": return inspectConfigAssets(engineRoot, args.project, "ocean");
    case "author_post_processing": return authorConfigAsset(engineRoot, args, "post_process");
    case "inspect_post_processings": return inspectConfigAssets(engineRoot, args.project, "post_process");
    case "author_fluid_simulation": return authorConfigAsset(engineRoot, args, "fluid_sim");
    case "inspect_fluid_simulations": return inspectConfigAssets(engineRoot, args.project, "fluid_sim");
    case "author_world_asset": return authorConfigAsset(engineRoot, args, "world");
    case "inspect_world_assets": return inspectConfigAssets(engineRoot, args.project, "world");
    case "author_chunk_config": return authorConfigAsset(engineRoot, args, "chunk");
    case "inspect_chunk_configs": return inspectConfigAssets(engineRoot, args.project, "chunk");
    case "author_block_transaction": return authorConfigAsset(engineRoot, args, "transaction");
    case "inspect_block_transactions": return inspectConfigAssets(engineRoot, args.project, "transaction");
    case "author_block_entity": return authorConfigAsset(engineRoot, args, "block_entity");
    case "inspect_block_entities": return inspectConfigAssets(engineRoot, args.project, "block_entity");
    case "author_inventory": return authorConfigAsset(engineRoot, args, "inventory");
    case "inspect_inventories": return inspectConfigAssets(engineRoot, args.project, "inventory");
    case "validate_game_project": return validateProject(engineRoot, args.project);
    case "run_batch": return runBatch(engineRoot, args);
    default: return undefined;
  }
}

// FALTANTES item 5 / task_plan Agente 5 §4 item 2 — Command Bus transacional:
// run_batch executa N authoring operations com semântica all-or-nothing. O
// surface individual (author_*/create_prefab/create_particle_asset) já tem
// dry_run (valida sem escrever), update (substitui devolvendo o documento
// anterior) e rollback — o run_batch orquestra esses contratos:
//   Phase 1 (validate): TODAS as ops rodam com dry_run:true — qualquer recusa
//     aborta o batch inteiro sem escrever nada (validação é a fonte da
//     atomicidade: nada é escrito até tudo validar).
//   Phase 2 (apply): as ops rodam em ordem com update:true; cada resultado
//     grava {path, created|updated, previous} para undo.
//   Phase 3 (rollback): se QUALQUER op falhar no apply, as ops já aplicadas
//     são desfeitas em ordem reversa (updated -> re-escreve o previous
//     document; created -> remove o arquivo). O batch nunca deixa o projeto
//     pela metade.
const BATCH_TOOLS = new Set([
  "author_registry_asset", "author_vehicle_asset", "author_ability_asset",
  "author_mission_asset", "author_world_profile_asset", "author_gait_asset",
  "author_simulation_lod_spec", "create_prefab", "create_particle_asset"
]);

function runBatch(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const operations = Array.isArray(args.operations) ? args.operations : [];
  if (operations.length === 0) throw new Error("run_batch requires a non-empty 'operations' array");
  for (const [index, op] of operations.entries()) {
    if (!op || typeof op.tool !== "string" || !BATCH_TOOLS.has(op.tool)) {
      throw new Error(`run_batch operation[${index}] must be one of: ${[...BATCH_TOOLS].join(", ")}`);
    }
  }

  // Phase 1 — validate everything first (dry_run; nothing is written until
  // every operation passes its public-contract validation). Defensive: a tool
  // that throws during the preview (e.g. a path collision on disk) is caught
  // and returned as a structured refusal — the batch still writes nothing.
  for (const [index, op] of operations.entries()) {
    let preview;
    try {
      preview = callSemanticTool(engineRoot, op.tool, { ...op.args, project: project.project, dry_run: true });
    } catch (error) {
      return {
        refused: true,
        project: project.project,
        operation: index,
        tool: op.tool,
        diagnostics: [error instanceof Error ? error.message : String(error)],
        reason: `operation[${index}] (${op.tool}) failed during validation; nothing was written`
      };
    }
    if (preview && preview.refused) {
      return {
        refused: true,
        project: project.project,
        operation: index,
        tool: op.tool,
        diagnostics: preview.diagnostics ?? [],
        reason: `operation[${index}] (${op.tool}) failed validation; nothing was written`
      };
    }
  }
  if (args.dry_run) {
    return {
      dry_run: true,
      project: project.project,
      would_apply: operations.map((op) => ({ tool: op.tool, args: op.args })),
      validated: operations.length
    };
  }

  // Phase 2 — apply in order, capturing undo info per applied op. Natural
  // create semantics (a duplicate inside the batch fails and rolls the batch
  // back); the batch-level update flag opts into replacement (mirrors the
  // per-tool update contract).
  const allowUpdate = Boolean(args.update);
  const applied = [];
  for (const [index, op] of operations.entries()) {
    try {
      const result = callSemanticTool(engineRoot, op.tool, { ...op.args, project: project.project, ...(allowUpdate ? { update: true } : {}) });
      const file = result.path ? path.join(project.root, result.path) : null;
      applied.push({
        index,
        tool: op.tool,
        result,
        created: Boolean(result.created),
        file: file && fs.existsSync(file) ? file : null,
        previous: result.rollback?.document ?? null
      });
    } catch (error) {
      // Phase 3 — roll back every already-applied op, in reverse order.
      const rolledBack = [];
      for (const done of applied.reverse()) {
        try {
          if (done.file && done.previous !== null) {
            atomicWriteJson(done.file, done.previous);
            rolledBack.push({ tool: done.tool, file: done.file, action: "restored" });
          } else if (done.file && done.created) {
            fs.rmSync(done.file, { force: true });
            rolledBack.push({ tool: done.tool, file: done.file, action: "removed" });
          }
        } catch {
          rolledBack.push({ tool: done.tool, file: done.file, action: "rollback-failed" });
        }
      }
      return {
        committed: false,
        project: project.project,
        failed_at: index,
        tool: op.tool,
        error: error instanceof Error ? error.message : String(error),
        rolled_back: rolledBack,
        applied: applied.map((done) => ({ tool: done.tool, index: done.index }))
      };
    }
  }

  return {
    committed: true,
    project: project.project,
    applied: applied.map((done) => ({ tool: done.tool, index: done.index, path: done.result.path, created: done.created }))
  };
}

export function listProjects(engineRoot) {
  const root = path.join(engineRoot, "Projects");
  fs.mkdirSync(root, { recursive: true });
  const projects = fs.readdirSync(root, { withFileTypes: true })
    .filter((entry) => entry.isDirectory() && !entry.name.startsWith("."))
    .map((entry) => {
      const manifest = path.join(root, entry.name, "project.json");
      return { name: entry.name, managed: fs.existsSync(manifest), path: `Projects/${entry.name}` };
    });
  projects.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
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

export function inspectProject(engineRoot, projectName) {
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
    abilities: files(paths.abilities, ".json"),
    missions: files(paths.missions, ".json"),
    world_profiles: files(paths.profiles, ".json"),
    gaits: files(paths.animations, ".json"),
    simulation_lod_specs: files(paths.simulationLod, ".json"),
    prefabs: files(paths.prefabs, ".prefab"),
    particle_assets: files(paths.particles, ".particle"),
    shaders: files(paths.shaders, ".json"),
    render_graphs: files(paths.renderGraphs, ".json"),
    lights: files(paths.lights, ".json"),
    gi_configs: files(paths.gi, ".json"),
    ocean_configs: files(paths.ocean, ".json"),
    post_processings: files(paths.postProcess, ".json"),
    fluid_simulations: files(paths.fluidSims, ".json"),
    worlds: files(paths.worlds, ".json"),
    chunk_configs: files(paths.chunks, ".json"),
    block_transactions: files(paths.transactions, ".json"),
    block_entities: files(paths.blockEntities, ".json"),
    inventories: files(paths.inventories, ".json"),
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
    for (const field of SCRIPT_NODE_REQUIRED_FIELDS[node.kind] ?? []) {
      const value = node[field];
      const empty = value === undefined || (typeof value === "string" && value.length === 0);
      if (empty) throw new Error(`script node '${node.key}' (${node.kind}) requires a non-empty '${field}'`);
    }
    const id = uuid();
    keys.set(node.key, id);
    const result = { id, kind: node.kind };
    if (node.event !== undefined) result.event = String(node.event);
    if (node.variable !== undefined) result.variable = String(node.variable);
    if (node.literal !== undefined) {
      try { result.literal = scriptLiteralForKind(node.kind, node.literal); }
      catch (error) { throw new Error(`script node '${node.key}' (${node.kind}) ${error.message}`); }
    }
    return result;
  });
  const links = args.links.map((link) => {
    if (!keys.has(link.from) || !keys.has(link.to)) throw new Error(`script link references an unknown key: ${link.from} -> ${link.to}`);
    return { from: keys.get(link.from), to: keys.get(link.to) };
  });
  const document = { id: uuid(), name: scriptName, nodes, links };
  const directory = args.scene_companion ? project.scenes : project.scripts;
  const file = path.join(directory, `${scriptName}.script`);
  const existed = fs.existsSync(file);
  const previous = existed ? readJson(file) : null;
  atomicWriteJson(file, document);
  // Runtime honesty (main_game.cpp setupGameplay): the demo game/editor auto-load
  // ONLY Content/Scenes/Initial.script and Content/Initial.script (hardcoded).
  // scene_companion + name "Initial" matches that convention; anything else is a
  // library asset the runtime does not auto-load — say so instead of implying it runs.
  const runtimeLoads = args.scene_companion === true && scriptName === "Initial";
  const location = args.scene_companion ? "scene_companion" : "library";
  return {
    created: true,
    replaced: existed,
    ...(existed ? { previous } : {}),
    project: project.project,
    script: scriptName,
    path: path.relative(project.root, file).replaceAll(path.sep, "/"),
    location,
    runtime_loads: runtimeLoads,
    notice: runtimeLoads
      ? "runtime auto-loads this script (demo convention: Content/Scenes/Initial.script)"
      : "library asset — the demo runtime auto-loads only Content/Scenes/Initial.script and Content/Initial.script; author it as a scene companion named 'Initial' to wire it into the game",
    node_ids: Object.fromEntries(keys),
    nodes: nodes.length,
    links: links.length
  };
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

// ---- mission assets (FALTANTES item 23 — missões e diálogos) -----------------

function buildMissionObjective(objective, index) {
  const prefix = `objective ${index}`;
  const target = String(objective.target ?? "");
  return {
    id: String(objective.id ?? `${prefix}`),
    kind: String(objective.kind ?? "reach"),
    target,
    count: objective.count !== undefined ? Number(objective.count) : 1,
    x: objective.x !== undefined ? Number(objective.x) : 0,
    z: objective.z !== undefined ? Number(objective.z) : 0,
    radius: objective.radius !== undefined ? Number(objective.radius) : 0,
    conditions: buildMissionConditions(objective.conditions)
  };
}

function buildMissionConditions(conditions) {
  return (conditions ?? []).map((condition) => ({
    kind: String(condition.kind ?? "flag"),
    key: String(condition.key ?? ""),
    op: String(condition.op ?? ">="),
    value: condition.value !== undefined ? Number(condition.value) : 0,
    flagValue: condition.flagValue !== undefined ? Boolean(condition.flagValue) : true
  }));
}

function buildMissionDocument(kind, args) {
  if (kind !== "mission") throw new Error(`unsupported mission kind '${kind}'`);
  const document = {
    name: String(args.name),
    version: args.version !== undefined ? Number(args.version) : 1,
    objectives: (args.objectives ?? []).map(buildMissionObjective),
    dialogue: (args.dialogue ?? []).map((node, index) => ({
      id: String(node.id ?? `node${index}`),
      speaker: String(node.speaker ?? ""),
      text: String(node.text ?? ""),
      choices: (node.choices ?? []).map((choice) => ({
        text: String(choice.text ?? ""),
        next: String(choice.next ?? ""),
        conditions: buildMissionConditions(choice.conditions)
      }))
    })),
    unlockConditions: buildMissionConditions(args.unlockConditions),
    reward: {
      itemId: String(args.reward?.itemId ?? ""),
      count: args.reward?.count !== undefined ? Number(args.reward.count) : 0,
      xp: args.reward?.xp !== undefined ? Number(args.reward.xp) : 0,
      setFlag: String(args.reward?.setFlag ?? "")
    },
    repeatable: args.repeatable !== undefined ? Boolean(args.repeatable) : false
  };
  if (args.id !== undefined) document.id = String(args.id);
  return document;
}

// Structured validation mirroring the public C++ factory (MissionDefinition::
// load_from_json — all-or-nothing, never clamp or guess). Returns { valid, errors }.
function validateMissionCondition(condition, objectiveIds, errors, where) {
  const fail = (message) => errors.push(`${where}: ${message}`);
  if (!condition || typeof condition !== "object" || Array.isArray(condition)) return fail("must be an object");
  const kind = String(condition.kind ?? "");
  if (!MISSION_CONDITION_KINDS.has(kind)) return fail(`'kind' must be flag|counter|objectiveDone|attribute (got '${kind}')`);
  if (typeof condition.key !== "string" || !condition.key) return fail("'key' must not be empty");
  if (kind === "counter" || kind === "attribute") {
    // The C++ factory defaults 'op' to ">=" and 'value' to 0 before
    // validating; mirror those defaults exactly.
    const op = String(condition.op ?? ">=");
    if (!MISSION_OPS.has(op)) fail(`'op' must be ==|!=|>=|<=|>|< (got '${op}')`);
    if (!finiteNumber(condition.value ?? 0)) fail("'value' must be finite");
  }
  if (kind === "objectiveDone" && objectiveIds && !objectiveIds.has(condition.key)) {
    fail(`objectiveDone references unknown objective '${condition.key}'`);
  }
}

export function validateMissionDocument(kind, document) {
  const errors = [];
  const fail = (message) => errors.push(`mission asset ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["mission asset must be a JSON object"] };
  }
  if (kind !== "mission") return { valid: false, errors: [`unsupported mission kind '${kind}'`] };
  if (document.version !== undefined && document.version !== 1) fail("'version' must be 1");
  if (typeof document.name !== "string" || !document.name) fail("'name' is required");
  if (!Array.isArray(document.objectives) || document.objectives.length === 0) {
    fail("'objectives' must be a non-empty array");
  } else {
    const objectiveIds = new Set();
    document.objectives.forEach((objective, index) => {
      const where = `objective ${index}`;
      if (!objective || typeof objective !== "object" || Array.isArray(objective)) return fail(`${where} must be an object`);
      if (typeof objective.id !== "string" || !objective.id) return fail(`${where} 'id' must not be empty`);
      if (objectiveIds.has(objective.id)) return fail(`duplicate objective id '${objective.id}'`);
      objectiveIds.add(objective.id);
      // The C++ factory defaults kind->"collect", count->1, x/z/radius->0
      // before validating; mirror those defaults exactly.
      const kind = String(objective.kind ?? "collect");
      if (!MISSION_OBJECTIVE_KINDS.has(kind)) return fail(`${where} 'kind' must be reach|collect|kill|interact (got '${kind}')`);
      if (kind !== "reach" && (typeof objective.target !== "string" || !objective.target)) fail(`${where} needs a target`);
      if (!Number.isInteger(objective.count ?? 1) || (objective.count ?? 1) < 1) fail(`${where} 'count' must be an integer >= 1`);
      if (!finiteNumber(objective.x ?? 0) || !finiteNumber(objective.z ?? 0)) fail(`${where} position must be finite`);
      if (!finiteNumber(objective.radius ?? 0) || (objective.radius ?? 0) < 0) fail(`${where} 'radius' must be finite and >= 0`);
      if (objective.conditions !== undefined) {
        if (!Array.isArray(objective.conditions)) fail(`${where} 'conditions' must be an array`);
        else objective.conditions.forEach((condition) => validateMissionCondition(condition, objectiveIds, errors, `${where} condition`));
      }
    });
  }
  const nodeIds = new Set();
  if (document.dialogue !== undefined) {
    if (!Array.isArray(document.dialogue)) {
      fail("'dialogue' must be an array");
    } else {
      document.dialogue.forEach((node, index) => {
        const where = `dialogue node ${index}`;
        if (!node || typeof node !== "object" || Array.isArray(node)) return fail(`${where} must be an object`);
        if (typeof node.id !== "string" || !node.id) return fail(`${where} 'id' must not be empty`);
        if (nodeIds.has(node.id)) return fail(`duplicate dialogue node id '${node.id}'`);
        nodeIds.add(node.id);
        if (node.choices !== undefined) {
          if (!Array.isArray(node.choices)) return fail(`${where} 'choices' must be an array`);
          node.choices.forEach((choice, choiceIndex) => {
            const choiceWhere = `${where} choice ${choiceIndex}`;
            if (!choice || typeof choice !== "object" || Array.isArray(choice)) return fail(`${choiceWhere} must be an object`);
            if (typeof choice.text !== "string" || !choice.text) fail(`${choiceWhere} 'text' must not be empty`);
            if (typeof choice.next !== "string") fail(`${choiceWhere} 'next' must be a string`);
            if (choice.conditions !== undefined) {
              if (!Array.isArray(choice.conditions)) fail(`${choiceWhere} 'conditions' must be an array`);
              else choice.conditions.forEach((condition) => validateMissionCondition(condition, null, errors, `${choiceWhere} condition`));
            }
          });
        }
      });
    }
  }
  if (document.dialogue && document.dialogue.length > 0 && !nodeIds.has("start")) {
    fail("dialogue must declare a 'start' node");
  }
  if (document.dialogue && Array.isArray(document.dialogue)) {
    for (const node of document.dialogue) {
      for (const choice of node.choices ?? []) {
        if (choice.next && !nodeIds.has(choice.next)) fail(`dialogue choice 'next' references unknown node '${choice.next}'`);
      }
    }
  }
  if (document.unlockConditions !== undefined) {
    if (!Array.isArray(document.unlockConditions)) fail("'unlockConditions' must be an array");
    else document.unlockConditions.forEach((condition) => validateMissionCondition(condition, null, errors, "unlock condition"));
  }
  if (document.reward !== undefined) {
    if (!document.reward || typeof document.reward !== "object" || Array.isArray(document.reward)) fail("'reward' must be an object");
    else {
      // The C++ factory defaults reward count/xp to 0 before validating.
      if (!Number.isInteger(document.reward.count ?? 0) || (document.reward.count ?? 0) < 0) fail("reward 'count' must be an integer >= 0");
      if (!Number.isInteger(document.reward.xp ?? 0) || (document.reward.xp ?? 0) < 0) fail("reward 'xp' must be an integer >= 0");
    }
  }
  if (document.repeatable !== undefined && typeof document.repeatable !== "boolean") fail("'repeatable' must be a boolean");
  return { valid: errors.length === 0, errors };
}

function missionDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorMissionAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const kind = String(args.kind ?? "mission");
  if (!MISSION_KINDS.includes(kind)) throw new Error(`unsupported mission kind '${kind}' (supported: ${MISSION_KINDS.join(", ")})`);
  const name = assetName(args.name);
  const document = buildMissionDocument(kind, args);
  const validation = validateMissionDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: "mission asset fails public-contract validation; nothing was written"
    };
  }
  const file = path.join(project.missions, `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? missionDiff(previous, document) : null;
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
  if (previous && !args.update) throw new Error(`mission asset '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
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

// Reads every mission asset of a project with its structural diagnostics
// (mirrors readAbilityAssets).
function readMissionAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.missions)) return assets;
  for (const fileName of fs.readdirSync(project.missions).filter((file) => file.endsWith(".json")).sort()) {
    const file = path.join(project.missions, fileName);
    const baseName = fileName.replace(/\.json$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validateMissionDocument("mission", document).errors);
    } else if (document) {
      diagnostics.push("mission asset must be a JSON object");
    }
    assets.push({
      kind: "mission",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectMissionAssets(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readMissionAssets(project);
  return { project: project.project, mission_assets: assets, count: assets.length };
}

// ---- world profiles (FALTANTES item 23 "editar geração procedural") --------

function buildWorldProfileDocument(kind, args) {
  if (kind !== "world_profile") throw new Error(`unsupported world profile kind '${kind}'`);
  const document = { version: args.version !== undefined ? Number(args.version) : 1 };
  if (args.height !== undefined) {
    document.height = args.height;
    document.baseHeight = args.baseHeight !== undefined ? Number(args.baseHeight) : 0;
    document.amplitude = args.amplitude !== undefined ? Number(args.amplitude) : 1;
  }
  if (args.climate !== undefined) {
    const climate = {};
    for (const axis of WORLD_PROFILE_CLIMATE_AXES) {
      if (args.climate[axis] !== undefined) climate[axis] = args.climate[axis];
    }
    document.climate = climate;
  }
  if (args.biomes !== undefined) document.biomes = args.biomes;
  if (args.caves !== undefined) {
    document.caves = {};
    if (args.caves.density !== undefined) document.caves.density = args.caves.density;
    if (args.caves.scale !== undefined) document.caves.scale = Number(args.caves.scale);
    if (args.caves.offset !== undefined) document.caves.offset = Number(args.caves.offset);
  }
  if (args.ores !== undefined) {
    document.ores = {};
    if (args.ores.density !== undefined) document.ores.density = args.ores.density;
    if (args.ores.scale !== undefined) document.ores.scale = Number(args.ores.scale);
    if (args.ores.offset !== undefined) document.ores.offset = Number(args.ores.offset);
    if (args.ores.table !== undefined) document.ores.table = args.ores.table;
  }
  if (args.carver !== undefined) document.carver = args.carver;
  if (args.decorators !== undefined) document.decorators = args.decorators;
  if (args.structures !== undefined) document.structures = args.structures;
  return document;
}

// Top-level structural validation mirroring the public C++ factory
// (create_world_profile_from_json — version, section types, amplitude >= 0,
// finite scale/offset). The MCP never re-implements the subsystem parsers:
// every present section is validated all-or-nothing by its own parser on
// load. Returns { valid, errors }.
export function validateWorldProfileDocument(kind, document) {
  const errors = [];
  const fail = (message) => errors.push(`world profile ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["world profile must be a JSON object"] };
  }
  if (kind !== "world_profile") return { valid: false, errors: [`unsupported world profile kind '${kind}'`] };
  if (document.version !== 1) fail("'version' must be 1");
  if (document.height !== undefined) {
    if (!document.height || typeof document.height !== "object" || Array.isArray(document.height)) fail("'height' must be an object (noise graph JSON)");
    if (!Number.isInteger(document.baseHeight ?? 0)) fail("'baseHeight' must be an integer");
    if (!Number.isInteger(document.amplitude ?? 1) || (document.amplitude ?? 1) < 0) fail("'amplitude' must be an integer >= 0");
  }
  if (document.climate !== undefined) {
    if (!document.climate || typeof document.climate !== "object" || Array.isArray(document.climate)) {
      fail("'climate' must be an object");
    } else {
      for (const axis of WORLD_PROFILE_CLIMATE_AXES) {
        if (document.climate[axis] !== undefined &&
            (!document.climate[axis] || typeof document.climate[axis] !== "object" || Array.isArray(document.climate[axis]))) {
          fail(`'climate.${axis}' must be an object (noise graph JSON)`);
        }
      }
    }
  }
  for (const section of ["biomes", "carver", "decorators", "structures"]) {
    if (document[section] !== undefined &&
        (!document[section] || typeof document[section] !== "object" || Array.isArray(document[section]))) {
      fail(`'${section}' must be an object`);
    }
  }
  for (const section of ["caves", "ores"]) {
    if (document[section] === undefined) continue;
    if (!document[section] || typeof document[section] !== "object" || Array.isArray(document[section])) {
      fail(`'${section}' must be an object`);
      continue;
    }
    if (document[section].density !== undefined &&
        (!document[section].density || typeof document[section].density !== "object" || Array.isArray(document[section].density))) {
      fail(`'${section}.density' must be an object (noise graph JSON)`);
    }
    if (document[section].scale !== undefined && !finiteNumber(document[section].scale)) fail(`'${section}.scale' must be finite`);
    if (document[section].offset !== undefined && !finiteNumber(document[section].offset)) fail(`'${section}.offset' must be finite`);
  }
  return { valid: errors.length === 0, errors };
}

function worldProfileDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorWorldProfileAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const kind = String(args.kind ?? "world_profile");
  if (!WORLD_PROFILE_KINDS.includes(kind)) throw new Error(`unsupported world profile kind '${kind}' (supported: ${WORLD_PROFILE_KINDS.join(", ")})`);
  const name = assetName(args.name);
  const document = buildWorldProfileDocument(kind, args);
  const validation = validateWorldProfileDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: "world profile fails top-level public-contract validation; nothing was written"
    };
  }
  const file = path.join(project.profiles, `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? worldProfileDiff(previous, document) : null;
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
  if (previous && !args.update) throw new Error(`world profile '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
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

// Reads every world profile of a project with its top-level structural
// diagnostics (mirrors readMissionAssets).
function readWorldProfileAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.profiles)) return assets;
  for (const fileName of fs.readdirSync(project.profiles).filter((file) => file.endsWith(".json")).sort()) {
    const file = path.join(project.profiles, fileName);
    const baseName = fileName.replace(/\.json$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validateWorldProfileDocument("world_profile", document).errors);
    } else if (document) {
      diagnostics.push("world profile must be a JSON object");
    }
    assets.push({
      kind: "world_profile",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectWorldProfileAssets(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readWorldProfileAssets(project);
  return { project: project.project, world_profiles: assets, count: assets.length };
}

function buildGaitDocument(kind, args) {
  if (kind !== "gait") throw new Error(`unsupported gait kind '${kind}'`);
  const document = {
    name: String(args.name),
    version: args.version !== undefined ? Number(args.version) : 1,
    cycleDuration: args.cycleDuration !== undefined ? Number(args.cycleDuration) : 1.0,
    stanceFraction: args.stanceFraction !== undefined ? Number(args.stanceFraction) : 0.6,
    stepHeight: args.stepHeight !== undefined ? Number(args.stepHeight) : 0.25,
    maxStride: args.maxStride !== undefined ? Number(args.maxStride) : 0.5,
    legPhases: (args.legPhases ?? []).map((phase) => Number(phase)),
    legs: (args.legs ?? []).map((leg) => ({
      name: String(leg.name),
      hipOffset: (leg.hipOffset ?? [0, 0, 0]).map((v) => Number(v)),
      upperLength: leg.upperLength !== undefined ? Number(leg.upperLength) : 0.5,
      lowerLength: leg.lowerLength !== undefined ? Number(leg.lowerLength) : 0.5,
      restOffset: (leg.restOffset ?? [0, -1, 0]).map((v) => Number(v)),
      maxReach: leg.maxReach !== undefined ? Number(leg.maxReach) : 0.0,
      hipBone: leg.hipBone !== undefined ? Number(leg.hipBone) : -1,
      kneeBone: leg.kneeBone !== undefined ? Number(leg.kneeBone) : -1,
      footBone: leg.footBone !== undefined ? Number(leg.footBone) : -1
    }))
  };
  return document;
}

// Structured validation mirroring the public C++ factory (GaitAsset::
// load_from_json — all-or-nothing, never clamp or guess; the C++ applies
// documented defaults BEFORE validating, so this mirrors those defaults
// exactly). Returns { valid, errors }.
export function validateGaitDocument(kind, document) {
  const errors = [];
  const fail = (message) => errors.push(`gait asset ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["gait asset must be a JSON object"] };
  }
  if (kind !== "gait") return { valid: false, errors: [`unsupported gait kind '${kind}'`] };
  if (document.version !== undefined && document.version !== 1) fail("'version' must be 1");
  if (typeof document.name !== "string" || !document.name) fail("'name' is required");
  if (!finiteNumber(document.cycleDuration ?? 1.0) || (document.cycleDuration ?? 1.0) <= 0) {
    fail("'cycleDuration' must be finite and > 0");
  }
  if (!finiteNumber(document.stanceFraction ?? 0.6) || (document.stanceFraction ?? 0.6) <= 0 || (document.stanceFraction ?? 0.6) >= 1) {
    fail("'stanceFraction' must be finite and in (0, 1)");
  }
  if (!finiteNumber(document.stepHeight ?? 0.25) || (document.stepHeight ?? 0.25) < 0) {
    fail("'stepHeight' must be finite and >= 0");
  }
  if (!finiteNumber(document.maxStride ?? 0.5) || (document.maxStride ?? 0.5) <= 0) {
    fail("'maxStride' must be finite and > 0");
  }
  const phases = document.legPhases ?? [];
  if (!Array.isArray(document.legPhases)) fail("'legPhases' must be an array");
  if (!Array.isArray(document.legs) || document.legs.length === 0) {
    fail("'legs' must be a non-empty array");
    return { valid: errors.length === 0, errors };
  }
  if (!Array.isArray(document.legPhases) || phases.length !== document.legs.length) {
    fail(`'legPhases' size (${Array.isArray(document.legPhases) ? phases.length : "not an array"}) must match legs (${document.legs.length})`);
  } else {
    phases.forEach((phase, index) => {
      if (!finiteNumber(phase) || phase < 0 || phase >= 1) fail(`'legPhases[${index}]' must be in [0, 1)`);
    });
  }
  document.legs.forEach((leg, index) => {
    const where = `leg ${index}`;
    if (!leg || typeof leg !== "object" || Array.isArray(leg)) return fail(`${where} must be an object`);
    if (typeof leg.name !== "string" || !leg.name) return fail(`${where} 'name' must not be empty`);
    const hipOffset = leg.hipOffset ?? [0, 0, 0];
    if (!Array.isArray(hipOffset) || hipOffset.length !== 3 || !hipOffset.every(finiteNumber)) {
      fail(`${where} 'hipOffset' must be an array of 3 finite numbers`);
    }
    if (!finiteNumber(leg.upperLength ?? 0.5) || (leg.upperLength ?? 0.5) <= 0) fail(`${where} 'upperLength' must be finite and > 0`);
    if (!finiteNumber(leg.lowerLength ?? 0.5) || (leg.lowerLength ?? 0.5) <= 0) fail(`${where} 'lowerLength' must be finite and > 0`);
    const restOffset = leg.restOffset ?? [0, -1, 0];
    if (!Array.isArray(restOffset) || restOffset.length !== 3 || !restOffset.every(finiteNumber)) {
      fail(`${where} 'restOffset' must be an array of 3 finite numbers`);
    }
    if (!finiteNumber(leg.maxReach ?? 0.0) || (leg.maxReach ?? 0.0) < 0) fail(`${where} 'maxReach' must be finite and >= 0`);
    const bones = [leg.hipBone ?? -1, leg.kneeBone ?? -1, leg.footBone ?? -1].map(Number);
    if (!bones.every(Number.isInteger)) fail(`${where} bone indices must be integers`);
    const set = bones.filter((bone) => bone >= 0);
    if (new Set(set).size !== set.length) fail(`${where} bone indices must be distinct when set`);
  });
  return { valid: errors.length === 0, errors };
}

function gaitDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorGaitAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const kind = String(args.kind ?? "gait");
  if (!GAIT_KINDS.includes(kind)) throw new Error(`unsupported gait kind '${kind}' (supported: ${GAIT_KINDS.join(", ")})`);
  const name = assetName(args.name);
  const document = buildGaitDocument(kind, args);
  const validation = validateGaitDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: "gait fails public-contract validation; nothing was written"
    };
  }
  const file = path.join(project.animations, `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? gaitDiff(previous, document) : null;
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
  if (previous && !args.update) throw new Error(`gait '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
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

// Reads every gait asset of a project with its structural diagnostics
// (mirrors readWorldProfileAssets).
function readGaitAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.animations)) return assets;
  for (const fileName of fs.readdirSync(project.animations).filter((file) => file.endsWith(".json")).sort()) {
    const file = path.join(project.animations, fileName);
    const baseName = fileName.replace(/\.json$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validateGaitDocument("gait", document).errors);
    } else if (document) {
      diagnostics.push("gait asset must be a JSON object");
    }
    assets.push({
      kind: "gait",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectGaitAssets(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readGaitAssets(project);
  return { project: project.project, gaits: assets, count: assets.length };
}

function buildSimulationLodDocument(kind, args) {
  if (kind !== "simulation_lod") throw new Error(`unsupported simulation lod kind '${kind}'`);
  return {
    name: String(args.name),
    version: args.version !== undefined ? Number(args.version) : 1,
    cellSize: args.cellSize !== undefined ? Number(args.cellSize) : 16.0,
    fullRadius: args.fullRadius !== undefined ? Number(args.fullRadius) : 48.0,
    falloffRadius: args.falloffRadius !== undefined ? Number(args.falloffRadius) : 320.0,
    dayLengthSeconds: args.dayLengthSeconds !== undefined ? Number(args.dayLengthSeconds) : 240.0,
    daysPerSeason: args.daysPerSeason !== undefined ? Number(args.daysPerSeason) : 30,
    tiers: (args.tiers ?? []).map((tier) => ({
      name: String(tier.name),
      mode: String(tier.mode ?? "full"),
      minRelevance: tier.minRelevance !== undefined ? Number(tier.minRelevance) : 0.0,
      updateInterval: tier.updateInterval !== undefined ? Number(tier.updateInterval) : 0.0,
      sleepAfterIdle: tier.sleepAfterIdle !== undefined ? Number(tier.sleepAfterIdle) : 0.0,
      maxRegions: tier.maxRegions !== undefined ? Number(tier.maxRegions) : 0,
      aggregateInterval: tier.aggregateInterval !== undefined ? Number(tier.aggregateInterval) : 0.0
    }))
  };
}

// Structured validation mirroring the public C++ factory (SimulationLodSpec::
// load_from_json — all-or-nothing, never clamp or guess; the C++ applies
// documented defaults BEFORE validating, so this mirrors those defaults
// exactly). Returns { valid, errors }.
export function validateSimulationLodDocument(kind, document) {
  const errors = [];
  const fail = (message) => errors.push(`simulation lod spec ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["simulation lod spec must be a JSON object"] };
  }
  if (kind !== "simulation_lod") return { valid: false, errors: [`unsupported simulation lod kind '${kind}'`] };
  if (document.version !== undefined && document.version !== 1) fail("'version' must be 1");
  if (!finiteNumber(document.cellSize ?? 16.0) || (document.cellSize ?? 16.0) <= 0) fail("'cellSize' must be finite and > 0");
  if (!finiteNumber(document.fullRadius ?? 48.0) || (document.fullRadius ?? 48.0) < 0) fail("'fullRadius' must be finite and >= 0");
  if (!finiteNumber(document.falloffRadius ?? 320.0) || !((document.falloffRadius ?? 320.0) > (document.fullRadius ?? 48.0))) {
    fail("'falloffRadius' must be finite and > fullRadius");
  }
  if (!finiteNumber(document.dayLengthSeconds ?? 240.0) || (document.dayLengthSeconds ?? 240.0) <= 0) fail("'dayLengthSeconds' must be finite and > 0");
  if (!Number.isInteger(document.daysPerSeason ?? 30) || (document.daysPerSeason ?? 30) < 1) fail("'daysPerSeason' must be an integer >= 1");
  if (!Array.isArray(document.tiers) || document.tiers.length === 0) {
    fail("'tiers' must be a non-empty array");
    return { valid: errors.length === 0, errors };
  }
  const names = new Set();
  let previous = 2.0;
  document.tiers.forEach((tier, index) => {
    const where = `tier ${index}`;
    if (!tier || typeof tier !== "object" || Array.isArray(tier)) return fail(`${where} must be an object`);
    if (typeof tier.name !== "string" || !tier.name) return fail(`${where} 'name' must not be empty`);
    if (names.has(tier.name)) return fail(`duplicate tier name '${tier.name}'`);
    names.add(tier.name);
    const mode = String(tier.mode ?? "full");
    if (!SIMULATION_LOD_MODES.has(mode)) return fail(`${where} 'mode' must be full|coarse|aggregate|sleeping (got '${mode}')`);
    const minRelevance = Number(tier.minRelevance ?? 0.0);
    if (!finiteNumber(minRelevance) || minRelevance < 0 || minRelevance > 1) {
      return fail(`${where} 'minRelevance' must be in [0, 1]`);
    }
    if (!(minRelevance < previous)) return fail(`tiers must be sorted by minRelevance descending (tier '${tier.name}')`);
    previous = minRelevance;
    if (!finiteNumber(tier.updateInterval ?? 0.0) || (tier.updateInterval ?? 0.0) < 0) fail(`${where} 'updateInterval' must be finite and >= 0`);
    if (!finiteNumber(tier.sleepAfterIdle ?? 0.0) || (tier.sleepAfterIdle ?? 0.0) < 0) fail(`${where} 'sleepAfterIdle' must be finite and >= 0`);
    if (!Number.isInteger(tier.maxRegions ?? 0) || (tier.maxRegions ?? 0) < 0) fail(`${where} 'maxRegions' must be an integer >= 0`);
    if (!finiteNumber(tier.aggregateInterval ?? 0.0) || (tier.aggregateInterval ?? 0.0) < 0) fail(`${where} 'aggregateInterval' must be finite and >= 0`);
  });
  return { valid: errors.length === 0, errors };
}

function simulationLodDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function authorSimulationLodSpec(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const kind = String(args.kind ?? "simulation_lod");
  if (!SIMULATION_LOD_KINDS.includes(kind)) throw new Error(`unsupported simulation lod kind '${kind}' (supported: ${SIMULATION_LOD_KINDS.join(", ")})`);
  const name = assetName(args.name);
  const document = buildSimulationLodDocument(kind, args);
  const validation = validateSimulationLodDocument(kind, document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind,
      name,
      diagnostics: validation.errors,
      reason: "simulation LOD spec fails public-contract validation; nothing was written"
    };
  }
  const file = path.join(project.simulationLod, `${name}.json`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? simulationLodDiff(previous, document) : null;
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
  if (previous && !args.update) throw new Error(`simulation LOD spec '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
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

// Reads every simulation LOD spec of a project with its structural
// diagnostics (mirrors readGaitAssets).
function readSimulationLodAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.simulationLod)) return assets;
  for (const fileName of fs.readdirSync(project.simulationLod).filter((file) => file.endsWith(".json")).sort()) {
    const file = path.join(project.simulationLod, fileName);
    const baseName = fileName.replace(/\.json$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validateSimulationLodDocument("simulation_lod", document).errors);
    } else if (document) {
      diagnostics.push("simulation lod spec must be a JSON object");
    }
    assets.push({
      kind: "simulation_lod",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectSimulationLodSpecs(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readSimulationLodAssets(project);
  return { project: project.project, simulation_lod_specs: assets, count: assets.length };
}

// ---- prefab authoring (FALTANTES item 23 — "cenas, entidades, componentes
// e prefabs") ---------------------------------------------------------------

// Validates a prefab document: scene-shaped, version 1, entities WITHOUT ids
// (they are regenerated on instantiation), every component field known and
// matching the public component schemas. Returns { valid, errors }.
export function validatePrefabDocument(document) {
  const errors = [];
  const fail = (message) => errors.push(`prefab ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["prefab must be a JSON object"] };
  }
  if (document.format !== "VulkanEngine.Prefab") fail("'format' must be \"VulkanEngine.Prefab\"");
  if (document.version !== undefined && document.version !== 1) fail("'version' must be 1");
  if (!Array.isArray(document.entities)) {
    fail("'entities' must be an array");
    return { valid: errors.length === 0, errors };
  }
  const names = new Set();
  document.entities.forEach((entity, index) => {
    const where = `entity ${index}`;
    if (!entity || typeof entity !== "object" || Array.isArray(entity)) return fail(`${where} must be an object`);
    if (entity.id !== undefined) return fail(`${where} must NOT carry an 'id' (ids are regenerated on instantiation)`);
    if (typeof entity.name !== "string" || !entity.name) return fail(`${where} 'name' must not be empty`);
    if (names.has(entity.name)) return fail(`duplicate entity name '${entity.name}'`);
    names.add(entity.name);
    if (!entity.Transform) return fail(`${where} '${entity.name}' has no Transform`);
    for (const component of Object.keys(entity).filter((key) => !["name", "Transform"].includes(key))) {
      if (!COMPONENT_SCHEMAS[component]) return fail(`${where} has unknown component '${component}'`);
      const unknown = Object.keys(entity[component] ?? {}).filter((key) => !(key in COMPONENT_SCHEMAS[component]));
      if (unknown.length) return fail(`${where} ${component} has unknown fields ${unknown.join(", ")}`);
    }
  });
  if (errors.length === 0 && document.root_entity !== undefined && !names.has(document.root_entity)) {
    fail(`'root_entity' '${document.root_entity}' does not name any prefab entity`);
  }
  return { valid: errors.length === 0, errors };
}

function prefabDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

// Extracts a reusable entity set from an existing scene into
// Content/Prefabs/<name>.prefab (entity ids stripped — regenerated on
// instantiation). All-or-nothing: an entity id missing from the scene or a
// component failing the prefab contract refuses the WHOLE extraction.
function createPrefab(engineRoot, args) {
  const scene = requireScene(engineRoot, args.project, args.scene);
  const name = assetName(args.name);
  const requested = Array.isArray(args.entity_ids) ? args.entity_ids : [];
  if (requested.length === 0) throw new Error("entity_ids must name at least one entity");
  const byId = new Map(scene.document.entities.map((entity) => [entity.id, entity]));
  const missing = requested.filter((id) => !byId.has(id));
  if (missing.length > 0) {
    return {
      refused: true,
      project: scene.project.project,
      scene: scene.scene,
      name,
      diagnostics: [`entity id(s) not found in scene '${scene.scene}': ${missing.join(", ")}`],
      reason: "prefab extraction refused; nothing was written"
    };
  }
  // Remap INTERNAL Hierarchy.parent_id links (a parent that is part of the
  // selection) to the parent entity's NAME — entity ids are stripped from the
  // prefab, so names are the only stable reference. Links to entities OUTSIDE
  // the selection are preserved as-is (the instantiating scene must provide
  // them); the prefab contract refuses dangling internal links at validate.
  const selectedById = new Map(requested.map((id) => [id, byId.get(id)]));
  const document = {
    format: "VulkanEngine.Prefab",
    version: 1,
    name,
    source_scene: scene.scene,
    source_entity_ids: [...requested],
    entities: requested.map((id) => {
      const entity = byId.get(id);
      const stripped = Object.fromEntries(Object.entries(entity).filter(([key]) => key !== "id"));
      if (stripped.Hierarchy?.parent_id) {
        const parent = selectedById.get(stripped.Hierarchy.parent_id);
        if (parent) stripped.Hierarchy = { ...stripped.Hierarchy, parent_id: parent.name };
      }
      return stripped;
    })
  };
  if (args.root_entity !== undefined) {
    if (!document.entities.some((entity) => entity.name === args.root_entity)) {
      return {
        refused: true,
        project: scene.project.project,
        scene: scene.scene,
        name,
        diagnostics: [`root_entity '${args.root_entity}' does not name any selected entity`],
        reason: "prefab extraction refused; nothing was written"
      };
    }
    document.root_entity = String(args.root_entity);
  }
  const validation = validatePrefabDocument(document);
  if (!validation.valid) {
    return {
      refused: true,
      project: scene.project.project,
      scene: scene.scene,
      name,
      diagnostics: validation.errors,
      reason: "prefab fails the public prefab contract; nothing was written"
    };
  }
  const file = path.join(scene.project.prefabs, `${name}.prefab`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? prefabDiff(previous, document) : null;
  const relative = path.relative(scene.project.root, file).replaceAll(path.sep, "/");
  if (args.dry_run) {
    return {
      dry_run: true,
      would_write: relative,
      project: scene.project.project,
      scene: scene.scene,
      name,
      document,
      diagnostics: [],
      diff
    };
  }
  if (previous && !args.update) throw new Error(`prefab '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
  atomicWriteJson(file, document);
  return {
    created: !previous,
    updated: Boolean(previous),
    project: scene.project.project,
    scene: scene.scene,
    name,
    path: relative,
    sha256: sha256File(file),
    diagnostics: [],
    diff,
    rollback: previous ? { document: previous, hint: "re-author with update: true and this document to restore" } : undefined
  };
}

// Inserts a prefab into a target scene with FRESH UUIDs; internal
// Hierarchy.parent_id links are remapped to the new ids (links to entities
// outside the prefab are preserved). Optional offset shifts the prefab root's
// Transform position. Append-only: existing entities are never rewritten.
function instantiatePrefab(engineRoot, args) {
  const scene = requireScene(engineRoot, args.project, args.scene);
  const prefabName = assetName(args.prefab);
  const file = path.join(scene.project.prefabs, `${prefabName}.prefab`);
  if (!fs.existsSync(file)) throw new Error(`prefab '${prefabName}' does not exist in project '${scene.project.project}'`);
  const document = readJson(file);
  const validation = validatePrefabDocument(document);
  if (!validation.valid) {
    return {
      refused: true,
      project: scene.project.project,
      scene: scene.scene,
      prefab: prefabName,
      diagnostics: validation.errors,
      reason: "prefab is invalid; nothing was instantiated"
    };
  }
  const offset = Array.isArray(args.offset) && args.offset.length === 3 && args.offset.every(finiteNumber)
    ? args.offset.map(Number)
    : [0, 0, 0];
  const oldToNew = new Map();
  const ids = new Set(scene.document.entities.map((entity) => entity.id));
  const clones = document.entities.map((template) => {
    const freshId = uuid();
    oldToNew.set(template.name, freshId);
    return { ...template, id: freshId };
  });
  // Remap internal Hierarchy.parent_id links by the entity NAME captured
  // before the templates were cloned (names are unique inside a prefab).
  const templateById = new Map(document.entities.map((template) => [template.name, template]));
  for (const clone of clones) {
    const template = templateById.get(clone.name);
    if (template.Hierarchy?.parent_id) {
      const internal = templateById.get(template.Hierarchy.parent_id);
      if (internal) {
        clone.Hierarchy = { ...template.Hierarchy, parent_id: oldToNew.get(internal.name) };
      }
      // links to entities OUTSIDE the prefab are preserved as-is
    }
  }
  // Scene transforms are GLOBAL positions — the offset shifts EVERY prefab
  // entity so the whole set moves together (spatial relations preserved).
  // root_entity is the logical origin when the caller attaches a parent or
  // reasons about the prefab's placement, not an offset anchor.
  for (const clone of clones) {
    if (clone.Transform) {
      clone.Transform = {
        ...clone.Transform,
        px: Number(clone.Transform.px ?? 0) + offset[0],
        py: Number(clone.Transform.py ?? 0) + offset[1],
        pz: Number(clone.Transform.pz ?? 0) + offset[2]
      };
    }
  }
  // Optional external parent: attach the prefab root under a target entity.
  if (args.parent_id !== undefined) {
    if (!ids.has(args.parent_id)) {
      return {
        refused: true,
        project: scene.project.project,
        scene: scene.scene,
        prefab: prefabName,
        diagnostics: [`parent_id '${args.parent_id}' does not exist in scene '${scene.scene}'`],
        reason: "nothing was instantiated"
      };
    }
    const root = clones.find((clone) => clone.name === rootName);
    if (root) root.Hierarchy = { ...(root.Hierarchy ?? {}), parent_id: args.parent_id };
  }
  scene.document.entities.push(...clones);
  atomicWriteJson(scene.file, scene.document);
  return {
    instantiated: clones.length,
    project: scene.project.project,
    scene: scene.scene,
    prefab: prefabName,
    entity_ids: clones.map((clone) => clone.id),
    offset
  };
}

// Reads every prefab of a project with its structural diagnostics.
function readPrefabAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.prefabs)) return assets;
  for (const fileName of fs.readdirSync(project.prefabs).filter((file) => file.endsWith(".prefab")).sort()) {
    const file = path.join(project.prefabs, fileName);
    const baseName = fileName.replace(/\.prefab$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validatePrefabDocument(document).errors);
    } else if (document) {
      diagnostics.push("prefab must be a JSON object");
    }
    assets.push({
      kind: "prefab",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectPrefabs(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readPrefabAssets(project);
  return { project: project.project, prefabs: assets, count: assets.length };
}

// ---- particle asset authoring (FALTANTES item 23 — partículas) ---------------

// The emitter field names are the public ParticleEmitter component schema
// (single source of truth).
const PARTICLE_EMITTER_FIELDS = Object.keys(COMPONENT_SCHEMAS.ParticleEmitter);

// Validates a particle asset document: format/version/name plus every emitter
// field type-checked (numbers finite, booleans boolean); UNKNOWN fields are
// refused (never guessed). Returns { valid, errors }.
export function validateParticleDocument(document) {
  const errors = [];
  const fail = (message) => errors.push(`particle asset ${message}`);
  if (!document || typeof document !== "object" || Array.isArray(document)) {
    return { valid: false, errors: ["particle asset must be a JSON object"] };
  }
  if (document.format !== "VulkanEngine.Particle") fail("'format' must be \"VulkanEngine.Particle\"");
  if (document.version !== undefined && document.version !== 1) fail("'version' must be 1");
  if (typeof document.name !== "string" || !document.name) fail("'name' must not be empty");
  const unknown = Object.keys(document).filter((key) => !["name", "version", "format"].includes(key) && !PARTICLE_EMITTER_FIELDS.includes(key));
  for (const field of unknown) fail(`unknown field '${field}' (emitter fields are exactly the ParticleEmitter component schema)`);
  for (const field of PARTICLE_EMITTER_FIELDS) {
    if (document[field] === undefined) continue;
    if (typeof COMPONENT_SCHEMAS.ParticleEmitter[field] === "boolean") {
      if (typeof document[field] !== "boolean") fail(`'${field}' must be a boolean`);
    } else {
      if (typeof document[field] !== "number" || !Number.isFinite(document[field])) fail(`'${field}' must be a finite number`);
    }
  }
  return { valid: errors.length === 0, errors };
}

function particleDiff(previous, document) {
  const keys = new Set([...Object.keys(previous), ...Object.keys(document)]);
  const changed = [...keys].filter((key) => JSON.stringify(previous[key]) !== JSON.stringify(document[key])).sort();
  return { changed_fields: changed };
}

function buildParticleDocument(name, args) {
  const document = { format: "VulkanEngine.Particle", version: 1, name };
  for (const field of PARTICLE_EMITTER_FIELDS) {
    if (args[field] !== undefined) document[field] = args[field];
  }
  return document;
}

function createParticleAsset(engineRoot, args) {
  const project = requireProject(engineRoot, args.project);
  const name = assetName(args.name);
  // Never silently drop an argument the author passed: unknown keys are
  // refused all-or-nothing (the emitter fields are exactly the public
  // ParticleEmitter component schema).
  const unknownArgs = Object.keys(args).filter((key) => !["project", "name", "dry_run", "update"].includes(key) && !PARTICLE_EMITTER_FIELDS.includes(key));
  if (unknownArgs.length > 0) {
    return {
      refused: true,
      project: project.project,
      kind: "particle",
      name,
      diagnostics: unknownArgs.map((field) => `unknown field '${field}' (emitter fields are exactly the ParticleEmitter component schema)`),
      reason: "particle asset fails the public component schema; nothing was written"
    };
  }
  const document = buildParticleDocument(name, args);
  const validation = validateParticleDocument(document);
  if (!validation.valid) {
    return {
      refused: true,
      project: project.project,
      kind: "particle",
      name,
      diagnostics: validation.errors,
      reason: "particle asset fails the public component schema; nothing was written"
    };
  }
  const file = path.join(project.particles, `${name}.particle`);
  const previous = fs.existsSync(file) ? readJson(file) : null;
  const diff = previous ? particleDiff(previous, document) : null;
  const relative = path.relative(project.root, file).replaceAll(path.sep, "/");
  if (args.dry_run) {
    return {
      dry_run: true,
      would_write: relative,
      project: project.project,
      kind: "particle",
      name,
      document,
      diagnostics: [],
      diff
    };
  }
  if (previous && !args.update) throw new Error(`particle asset '${name}' already exists (pass update: true to replace, or use dry_run to preview the diff)`);
  atomicWriteJson(file, document);
  return {
    created: !previous,
    updated: Boolean(previous),
    project: project.project,
    kind: "particle",
    name,
    path: relative,
    sha256: sha256File(file),
    diagnostics: [],
    diff,
    rollback: previous ? { document: previous, hint: "re-author with update: true and this document to restore" } : undefined
  };
}

// Applies a particle asset to an entity's ParticleEmitter component through
// the SAME component validation the scene uses (setComponent) — unknown
// fields would be refused there too; missing fields fall back to component
// defaults.
function applyParticleAsset(engineRoot, args) {
  const scene = requireScene(engineRoot, args.project, args.scene);
  findEntity(scene, args.entity_id);
  const assetNameValue = assetName(args.asset);
  const file = path.join(scene.project.particles, `${assetNameValue}.particle`);
  if (!fs.existsSync(file)) throw new Error(`particle asset '${assetNameValue}' does not exist in project '${scene.project.project}'`);
  const document = readJson(file);
  const validation = validateParticleDocument(document);
  if (!validation.valid) {
    return {
      refused: true,
      project: scene.project.project,
      scene: scene.scene,
      entity_id: args.entity_id,
      asset: assetNameValue,
      diagnostics: validation.errors,
      reason: "particle asset is invalid; nothing was applied"
    };
  }
  const emitter = {};
  for (const field of PARTICLE_EMITTER_FIELDS) {
    if (document[field] !== undefined) emitter[field] = document[field];
  }
  const updated = setComponent(engineRoot, { project: args.project, scene: args.scene, entity_id: args.entity_id, component: "ParticleEmitter", values: emitter });
  return {
    applied: true,
    project: scene.project.project,
    scene: scene.scene,
    entity_id: args.entity_id,
    asset: assetNameValue,
    component: "ParticleEmitter",
    value: updated.value
  };
}

// Reads every particle asset of a project with its structural diagnostics.
function readParticleAssets(project) {
  const assets = [];
  if (!fs.existsSync(project.particles)) return assets;
  for (const fileName of fs.readdirSync(project.particles).filter((file) => file.endsWith(".particle")).sort()) {
    const file = path.join(project.particles, fileName);
    const baseName = fileName.replace(/\.particle$/, "");
    let document = null;
    const diagnostics = [];
    try {
      document = readJson(file);
    } catch (error) {
      diagnostics.push(`malformed JSON: ${error.message}`);
    }
    if (document && typeof document === "object" && !Array.isArray(document)) {
      diagnostics.push(...validateParticleDocument(document).errors);
    } else if (document) {
      diagnostics.push("particle asset must be a JSON object");
    }
    assets.push({
      kind: "particle",
      name: baseName,
      path: path.relative(project.root, file).replaceAll(path.sep, "/"),
      document,
      valid: diagnostics.length === 0,
      diagnostics
    });
  }
  return assets;
}

function inspectParticleAssets(engineRoot, projectName) {
  const project = requireProject(engineRoot, projectName);
  const assets = readParticleAssets(project);
  return { project: project.project, particle_assets: assets, count: assets.length };
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
        for (const node of script.nodes ?? []) {
          if (!SCRIPT_NODE_KINDS.includes(node.kind)) errors.push(`${fileName}: unsupported node kind '${node.kind}'`);
          for (const field of SCRIPT_NODE_REQUIRED_FIELDS[node.kind] ?? []) {
            const value = node[field];
            const empty = value === undefined || (typeof value === "string" && value.length === 0);
            if (empty) errors.push(`${fileName}: node ${node.id} (${node.kind}) requires a non-empty '${field}'`);
          }
          if (node.literal !== undefined && SCRIPT_LITERAL_KINDS[node.kind]) {
            try { scriptLiteralForKind(node.kind, node.literal); }
            catch (error) { errors.push(`${fileName}: node ${node.id} (${node.kind}) ${error.message}`); }
          }
        }
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

  // Mission assets (FALTANTES item 23 — missões e diálogos): structural
  // validation mirroring the public C++ MissionDefinition factory.
  const missionAssets = readMissionAssets(project);
  for (const asset of missionAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Missions/${asset.name}.json: ${diagnostic}`);
    }
  }

  // World profiles (FALTANTES item 23 — editar geração procedural): top-level
  // structural validation; sections validated by their own C++ parsers on load.
  const worldProfileAssets = readWorldProfileAssets(project);
  for (const asset of worldProfileAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Profiles/${asset.name}.json: ${diagnostic}`);
    }
  }

  // Gait assets (FALTANTES item 23 — animações): structural validation
  // mirroring the public C++ GaitAsset/LegChainAsset factories.
  const gaitAssets = readGaitAssets(project);
  for (const asset of gaitAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Animations/${asset.name}.json: ${diagnostic}`);
    }
  }

  // Simulation LOD specs (FALTANTES §20 — SimulationLodSpec): structural
  // validation mirroring SimulationLodSpec::load_from_json.
  const simulationLodAssets = readSimulationLodAssets(project);
  for (const asset of simulationLodAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/SimulationLod/${asset.name}.json: ${diagnostic}`);
    }
  }

  // Prefab assets (FALTANTES item 23): reusable entity sets — structural
  // validation mirroring the scene checks (components known, fields known).
  const prefabAssets = readPrefabAssets(project);
  for (const asset of prefabAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Prefabs/${asset.name}.prefab: ${diagnostic}`);
    }
  }

  // Particle assets (FALTANTES item 23 — partículas): reusable emitter
  // configurations mirroring the ParticleEmitter component schema.
  const particleAssets = readParticleAssets(project);
  for (const asset of particleAssets) {
    for (const diagnostic of asset.diagnostics) {
      errors.push(`Content/Particles/${asset.name}.particle: ${diagnostic}`);
    }
  }

  // §4.4/§4.5 — rendering + voxel/world CONFIG assets (2026-08-28): every
  // config kind validates against its public C++ contract mirror (structure +
  // ranges, all-or-nothing). A malformed config fails the project validation
  // exactly like a malformed registry asset.
  const configKindAssets = {};
  for (const kind of CONFIG_KINDS) {
    const assets = readConfigAssets(project, kind);
    configKindAssets[kind] = assets;
    for (const asset of assets) {
      for (const diagnostic of asset.diagnostics) {
        errors.push(`${asset.path}: ${diagnostic}`);
      }
    }
  }

  return {
    project: project.project,
    valid: errors.length === 0,
    errors,
    warnings,
    scenes: sceneFiles.length,
    registry_assets: registryAssets.length,
    vehicle_assets: vehicleAssets.length,
    ability_assets: abilityAssets.length,
    mission_assets: missionAssets.length,
    world_profiles: worldProfileAssets.length,
    gait_assets: gaitAssets.length,
    simulation_lod_specs: simulationLodAssets.length,
    prefabs: prefabAssets.length,
    particle_assets: particleAssets.length,
    shaders: configKindAssets.shader.length,
    render_graphs: configKindAssets.render_graph.length,
    lights: configKindAssets.light.length,
    gi_configs: configKindAssets.gi.length,
    ocean_configs: configKindAssets.ocean.length,
    post_processings: configKindAssets.post_process.length,
    fluid_simulations: configKindAssets.fluid_sim.length,
    worlds: configKindAssets.world.length,
    chunk_configs: configKindAssets.chunk.length,
    block_transactions: configKindAssets.transaction.length,
    block_entities: configKindAssets.block_entity.length,
    inventories: configKindAssets.inventory.length
  };
}
