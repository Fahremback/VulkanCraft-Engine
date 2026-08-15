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
    physicsMaterials: path.join(root, "Content", "PhysicsMaterials")
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
      , "author materials", "author audio events", "author physics materials"
    ],
    components: COMPONENT_SCHEMAS,
    light_types: { Directional: 0, Point: 1, Spot: 2, Area: 3 },
    script_node_kinds: SCRIPT_NODE_KINDS,
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
      name: "validate_game_project",
      description: "Validate the portable project, scenes, entity UUIDs, components, hierarchy, scripts, and asset metadata without compiling the engine.",
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

  for (const directory of [paths.content, paths.scenes, paths.scripts, paths.assets, paths.materials, paths.audioEvents, paths.physicsMaterials, path.join(paths.root, "Config"), path.join(paths.root, "Intermediate"), path.join(paths.root, "Build")]) {
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

  return { project: project.project, valid: errors.length === 0, errors, warnings, scenes: sceneFiles.length };
}
