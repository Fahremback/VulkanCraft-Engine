// control-api.mjs
//
// MCP tools that drive the RUNNING VulkanCraft editor through the loopback
// Control API (http://127.0.0.1:8321). Every tool mirrors one interactive
// feature of the editor UI (Play/PASSO, camera, scene, gizmos, assets, voxel,
// scripts, windows, theme, weather, terrain, graphics, dev). The HTTP layer is
// Node built-ins only — zero dependencies, same as server.mjs.
//
// Endpoint contract lives in engine/src/editor/EditorControlApi.cpp and the
// handler in EditorApplication::handle_control_command. Numbers use "/" as
// separator; text arguments use "?arg=" with URL-encoding.

import http from "node:http";

export const CONTROL_API_BASE = "http://127.0.0.1:8321";

// ---------------------------------------------------------------------------
// Enum tables (mirror the C++ handler exactly)
// ---------------------------------------------------------------------------

export const ENTITY_TYPES = Object.freeze([
  "empty", "cube", "camera", "sun", "point", "spot", "area", "particles",
  "audio", "rigidbody", "vehicle", "destructible", "navagent", "mission",
  "dialogue", "voxelworld"
]);

export const COMPONENT_TYPES = Object.freeze([
  "light", "camera", "mesh", "material", "rigidbody", "weapon", "vehicle",
  "ragdoll", "destructible", "navigation", "particle", "audio", "mission",
  "dialogue", "voxel"
]);

export const GIZMO_MODES = Object.freeze(["select", "move", "rotate", "scale"]);
export const GIZMO_SPACES = Object.freeze(["world", "local"]);
export const SNAP_STEPS = Object.freeze([0, 0.1, 0.5, 1, 2, 5]);
export const MESH_MODES = Object.freeze([0, 1]);
export const SELF_TESTS = Object.freeze([
  "render graph", "hdr", "material", "play/physics", "build"
]);
export const GRAPHICS_QUALITIES = Object.freeze([1, 2, 3, 4]);

// Windows toggled by /window/<name> (from EditorApplication.cpp).
export const WINDOW_NAMES = Object.freeze([
  "viewport", "scene", "inspector", "assets", "console", "dev", "guide",
  "name", "layers", "object", "light", "camera", "material", "sound",
  "rigidbody", "collider", "constraint", "softbody", "spring", "decal",
  "emitter", "hair", "spline", "forcefield", "envprobe", "weather",
  "animation-tools", "armature", "humanoid", "ik-tools", "expression",
  "terrain", "paint", "mesh", "importer", "video", "gaussian", "theme",
  "project-creator", "general", "graphics", "profiler"
]);

// Specialized editor tabs opened by /editor/<tab> (SpecializedEditorsPanel).
export const EDITOR_TABS = Object.freeze([
  "animation", "timeline", "retarget", "ik", "ragdoll", "weapon", "vehicle",
  "destruction", "particles", "audio", "navigation", "mission", "dialogue",
  "physics", "material", "render-graph"
]);

// ---------------------------------------------------------------------------
// HTTP helpers (Node built-ins only)
// ---------------------------------------------------------------------------

function httpRequest(method, path, timeoutMs = 15000) {
  return new Promise((resolve, reject) => {
    const url = new URL(path, CONTROL_API_BASE);
    const req = http.request(
      {
        hostname: url.hostname,
        port: url.port,
        path: `${url.pathname}${url.search}`,
        method,
        timeout: timeoutMs
      },
      (res) => {
        let body = "";
        res.setEncoding("utf8");
        res.on("data", (chunk) => { body += chunk; });
        res.on("end", () => {
          let parsed = null;
          try { parsed = JSON.parse(body); } catch { parsed = body; }
          resolve({ status: res.statusCode, body: parsed });
        });
      }
    );
    req.on("timeout", () => { req.destroy(new Error("Control API timeout")); });
    req.on("error", (error) => reject(error));
    req.end();
  });
}

function controlPost(endpoint) {
  return httpRequest("POST", endpoint);
}

function controlGet(endpoint) {
  return httpRequest("GET", endpoint);
}

// Builds the friendly "command" string the editor logs (mirror handle_control_command).
function commandString(endpoint) {
  return endpoint.replace(/^\//, "").replaceAll("/", " ").trim();
}

// Wrap every tool result with the endpoint + command so agents can re-run it
// with curl and keep a paper trail.
function wrapped(name, endpoint, result, extra = {}) {
  return {
    tool: name,
    endpoint: `${CONTROL_API_BASE}${endpoint}`,
    command: commandString(endpoint),
    ...extra,
    result
  };
}

async function callEditor(name, endpoint, extra = {}) {
  try {
    const res = await controlPost(endpoint);
    if (res.status !== 200) {
      throw new Error(`Control API returned HTTP ${res.status} for ${endpoint}`);
    }
    return wrapped(name, endpoint, res.body, extra);
  } catch (error) {
    if (error.code === "ECONNREFUSED") {
      throw new Error(
        "Editor not reachable on 127.0.0.1:8321 — start VulkanEngineEditor first. " +
        "Use run_game (exe=VulkanEngineEditor, seconds>=15) or launch the editor manually."
      );
    }
    throw error;
  }
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

function pickNumber(value, fallback, { min = -Infinity, max = Infinity } = {}) {
  const n = Number(value ?? fallback);
  if (!Number.isFinite(n) || n < min || n > max) {
    throw new Error(`expected a number in [${min}, ${max}], got '${value}'`);
  }
  return n;
}

function requireUuid(value, label = "uuid") {
  const text = String(value ?? "").trim();
  if (!/^[0-9a-fA-F-]{8,64}$/.test(text)) {
    throw new Error(`${label} must be a UUID, got '${value}'`);
  }
  return text;
}

function requireEnum(value, allowed, label) {
  const text = String(value ?? "").trim();
  if (!allowed.includes(text)) {
    throw new Error(`${label} must be one of: ${allowed.join(", ")} — got '${value}'`);
  }
  return text;
}

function requireText(value, label, { min = 1, max = 512 } = {}) {
  const text = String(value ?? "");
  if (text.length < min || text.length > max) {
    throw new Error(`${label} must have ${min}..${max} characters, got '${value}'`);
  }
  return text;
}

function queryEscape(text) {
  return encodeURIComponent(text).replace(/%20/g, "%20");
}

// ---------------------------------------------------------------------------
// Tool definitions
// ---------------------------------------------------------------------------

export function controlApiToolDefinitions() {
  return [
    {
      name: "editor_status",
      description: "Live state of the running editor (GET /state): play state, fps, entity count, camera, selected entity, gizmo mode, snap, terrain, script state, last self-test result. Also probes /health. Use first to learn what the editor is doing.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_play",
      description: "Start Play In Editor (runs physics, weapons, vehicles, scripts…).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_pause",
      description: "Pause a running play session (Play -> Pause).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_resume",
      description: "Resume a paused play session (Pause -> Play).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_step",
      description: "Advance exactly one game frame while paused (the PASSO feature). Requires the editor to be in Pause state first.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_stop",
      description: "Stop play/simulate and return to Edit mode.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_simulate",
      description: "Enter Simulate mode (physics without gameplay) from Edit.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_camera_zoom",
      description: "Dolly the editor camera (positive zooms in).",
      inputSchema: {
        type: "object",
        required: ["amount"],
        properties: { amount: { type: "number", description: "Zoom amount (e.g. 0.15); positive = closer" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_camera_move",
      description: "Pan the orbit target along the camera's local axes: forward/right/up.",
      inputSchema: {
        type: "object",
        properties: {
          forward: { type: "number", default: 0, description: "Forward (+) / backward (-)" },
          right: { type: "number", default: 0, description: "Right (+) / left (-)" },
          up: { type: "number", default: 0, description: "Up (+) / down (-)" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_camera_turn",
      description: "Rotate the camera by yaw/pitch degrees.",
      inputSchema: {
        type: "object",
        properties: {
          yaw: { type: "number", default: 0, description: "Degrees to yaw" },
          pitch: { type: "number", default: 0, description: "Degrees to pitch (clamped to -89..89)" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_camera_focus",
      description: "Point the orbit target at a world position.",
      inputSchema: {
        type: "object",
        properties: {
          x: { type: "number", default: 0 },
          y: { type: "number", default: 0 },
          z: { type: "number", default: 0 }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_new_scene",
      description: "Reset the scene to the default (camera + light).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_open_scene",
      description: "Load a .scene file by absolute path.",
      inputSchema: {
        type: "object",
        required: ["path"],
        properties: { path: { type: "string", description: "Absolute path to a .scene file" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_save_scene",
      description: "Save the current scene. If no path is active, saves to assets/scenes/api_<timestamp>.scene (never opens a dialog).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_add_entity",
      description: "Create an entity of a given type in the current scene and select it.",
      inputSchema: {
        type: "object",
        required: ["type"],
        properties: {
          type: { type: "string", enum: ENTITY_TYPES, description: "empty, cube, camera, sun, point, spot, area, particles, audio, rigidbody, vehicle, destructible, navagent, mission, dialogue, voxelworld" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_select_entity",
      description: "Select an entity by UUID or by name substring.",
      inputSchema: {
        type: "object",
        properties: {
          uuid: { type: "string", description: "Entity UUID" },
          name: { type: "string", description: "Name substring (alternative to uuid)" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_delete_entity",
      description: "Delete an entity by UUID.",
      inputSchema: {
        type: "object",
        required: ["uuid"],
        properties: { uuid: { type: "string" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_rename_entity",
      description: "Rename an entity by UUID.",
      inputSchema: {
        type: "object",
        required: ["uuid", "name"],
        properties: {
          uuid: { type: "string" },
          name: { type: "string", description: "New entity name" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_set_transform",
      description: "Set position (and optionally rotation/scale) of an entity by UUID. Values in degrees for rotation.",
      inputSchema: {
        type: "object",
        required: ["uuid"],
        properties: {
          uuid: { type: "string" },
          x: { type: "number", default: 0 },
          y: { type: "number", default: 0 },
          z: { type: "number", default: 0 },
          rot_x: { type: "number", description: "Rotation X in degrees" },
          rot_y: { type: "number", description: "Rotation Y in degrees" },
          rot_z: { type: "number", description: "Rotation Z in degrees" },
          scale_x: { type: "number", default: 1 },
          scale_y: { type: "number", default: 1 },
          scale_z: { type: "number", default: 1 }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_add_component",
      description: "Add a component to an entity by UUID.",
      inputSchema: {
        type: "object",
        required: ["uuid", "type"],
        properties: {
          uuid: { type: "string" },
          type: { type: "string", enum: COMPONENT_TYPES, description: "light, camera, mesh, material, rigidbody, weapon, vehicle, ragdoll, destructible, navigation, particle, audio, mission, dialogue, voxel" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_set_gizmo",
      description: "Change the viewport gizmo tool. 'select' hides the gizmo (picking only).",
      inputSchema: {
        type: "object",
        required: ["mode"],
        properties: { mode: { type: "string", enum: GIZMO_MODES } },
        additionalProperties: false
      }
    },
    {
      name: "editor_set_gizmo_space",
      description: "World or Local gizmo space (Local rotates the drag axes with the entity).",
      inputSchema: {
        type: "object",
        required: ["space"],
        properties: { space: { type: "string", enum: GIZMO_SPACES } },
        additionalProperties: false
      }
    },
    {
      name: "editor_set_snap",
      description: "Snap step used while holding Ctrl when dragging the gizmo (0 disables).",
      inputSchema: {
        type: "object",
        required: ["step"],
        properties: { step: { type: "number", enum: SNAP_STEPS, description: "0, 0.1, 0.5, 1, 2 or 5" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_import_asset",
      description: "Import and cook an asset file by absolute path (texture/mesh/audio…).",
      inputSchema: {
        type: "object",
        required: ["path"],
        properties: { path: { type: "string", description: "Absolute path to the source asset" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_create_block_model",
      description: "Create a Minecraft-style block model asset (.vblock) from a square POT texture (8-256px) by texture UUID.",
      inputSchema: {
        type: "object",
        required: ["texture_uuid"],
        properties: { texture_uuid: { type: "string", description: "UUID of the source texture asset" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_spawn_block",
      description: "Spawn a textured block cube entity in the scene, in front of the camera, from a block asset UUID.",
      inputSchema: {
        type: "object",
        required: ["block_uuid"],
        properties: { block_uuid: { type: "string", description: "UUID of the block asset (.vblock)" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_duplicate_asset",
      description: "Duplicate an asset by UUID.",
      inputSchema: {
        type: "object",
        required: ["uuid"],
        properties: { uuid: { type: "string" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_delete_asset",
      description: "Delete an asset by UUID.",
      inputSchema: {
        type: "object",
        required: ["uuid"],
        properties: { uuid: { type: "string" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_reimport_asset",
      description: "Re-cook an asset with its current import settings.",
      inputSchema: {
        type: "object",
        required: ["uuid"],
        properties: { uuid: { type: "string" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_voxel_generate",
      description: "Regenerate the voxel volume of an entity with a seed and sea level.",
      inputSchema: {
        type: "object",
        required: ["uuid"],
        properties: {
          uuid: { type: "string", description: "Voxel volume entity UUID" },
          seed: { type: "integer", default: 1337 },
          sea_level: { type: "number", default: 24 }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_voxel_paint",
      description: "Set or remove a single voxel at a grid coordinate.",
      inputSchema: {
        type: "object",
        required: ["uuid", "x", "y", "z"],
        properties: {
          uuid: { type: "string", description: "Voxel volume entity UUID" },
          x: { type: "integer" }, y: { type: "integer" }, z: { type: "integer" },
          type: { type: "integer", default: 2, description: "Block type id (1 = terrain, 2 = stone, …)" },
          mode: { type: "integer", default: 0, enum: [0, 1], description: "0 = place, 1 = remove" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_voxel_clear",
      description: "Empty the voxel volume of an entity.",
      inputSchema: {
        type: "object",
        required: ["uuid"],
        properties: { uuid: { type: "string" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_script_event",
      description: "Fire an event into the running script VM (e.g. OnStart).",
      inputSchema: {
        type: "object",
        required: ["name"],
        properties: { name: { type: "string", description: "Event name" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_script_pause",
      description: "Pause the script VM (debugger).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_script_continue",
      description: "Resume the script VM (debugger).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_script_step",
      description: "Single-step the script VM (debugger).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_toggle_window",
      description: "Toggle an editor window/panel.",
      inputSchema: {
        type: "object",
        required: ["window"],
        properties: { window: { type: "string", enum: WINDOW_NAMES, description: "One of the 42 dockable windows" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_open_specialized_editor",
      description: "Open a specialized editor tab (Animation, Material, Render Graph…).",
      inputSchema: {
        type: "object",
        required: ["tab"],
        properties: { tab: { type: "string", enum: EDITOR_TABS, description: "16 specialized editors" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_set_theme",
      description: "Set theme colors (background rgb + panel rgb), 0..1 each.",
      inputSchema: {
        type: "object",
        properties: {
          bg_r: { type: "number", default: 0.05 }, bg_g: { type: "number", default: 0.08 }, bg_b: { type: "number", default: 0.12 },
          panel_r: { type: "number", default: 0.15 }, panel_g: { type: "number", default: 0.15 }, panel_b: { type: "number", default: 0.18 }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_set_weather",
      description: "Set weather: sun color rgb, fog density, fog start, sky exposure, rain amount.",
      inputSchema: {
        type: "object",
        properties: {
          sun_r: { type: "number", default: 0.9 }, sun_g: { type: "number", default: 0.7 }, sun_b: { type: "number", default: 0.5 },
          fog_density: { type: "number", default: 0.01 },
          fog_start: { type: "number", default: 20 },
          sky_exposure: { type: "number", default: 1.2 },
          rain: { type: "number", default: 0.3 }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_generate_terrain",
      description: "Generate a terrain mesh in the scene.",
      inputSchema: {
        type: "object",
        properties: {
          scale: { type: "number", default: 1, description: "Terrain scale" },
          octaves: { type: "integer", default: 4 },
          amount: { type: "number", default: 0.5 },
          falloff: { type: "number", default: 0.4 }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_set_graphics",
      description: "Apply graphics settings: vsync and shadow quality (1-4).",
      inputSchema: {
        type: "object",
        properties: {
          vsync: { type: "boolean", default: true },
          quality: { type: "integer", default: 2, enum: GRAPHICS_QUALITIES, description: "Shadow quality 1..4" }
        },
        additionalProperties: false
      }
    },
    {
      name: "editor_save_settings",
      description: "Persist editor settings to settings.json.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_create_project",
      description: "Create a new game project (enters the editor with an empty project).",
      inputSchema: {
        type: "object",
        required: ["name"],
        properties: { name: { type: "string", description: "Project name" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_apply_mesh",
      description: "Recalculate mesh normals on the selected mesh: 0 = smooth, 1 = invert.",
      inputSchema: {
        type: "object",
        properties: { mode: { type: "integer", default: 0, enum: MESH_MODES } },
        additionalProperties: false
      }
    },
    {
      name: "editor_run_self_test",
      description: "Run a headless self-test in a second editor instance and report PASS/FAIL (120s timeout).",
      inputSchema: {
        type: "object",
        required: ["which"],
        properties: { which: { type: "integer", default: 0, enum: [0, 1, 2, 3, 4], description: "0 = render graph, 1 = HDR, 2 = material, 3 = play/physics, 4 = build" } },
        additionalProperties: false
      }
    },
    {
      name: "editor_package_assets",
      description: "Package cooked assets to Intermediate/Package (standalone, no build).",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    },
    {
      name: "editor_hot_reload",
      description: "Force the asset hot-reload watcher to check and reimport changed sources now.",
      inputSchema: { type: "object", properties: {}, additionalProperties: false }
    }
  ];
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

export async function callControlApiTool(name, args = {}) {
  switch (name) {
    // ---- Lifecycle ---------------------------------------------------------
    case "editor_status": {
      try {
        const state = await controlGet("/state");
        let health = null;
        try { health = await controlGet("/health"); } catch { /* optional */ }
        return wrapped("editor_status", "/state", state.body, health ? { health: health.body } : {});
      } catch (error) {
        if (error.code === "ECONNREFUSED") {
          throw new Error(
            "Editor not reachable on 127.0.0.1:8321 — start VulkanEngineEditor first " +
            "(run_game exe=VulkanEngineEditor, or launch it manually)."
          );
        }
        throw error;
      }
    }
    case "editor_play": return callEditor(name, "/play");
    case "editor_pause": return callEditor(name, "/pause");
    case "editor_resume": return callEditor(name, "/resume");
    case "editor_step": return callEditor(name, "/step");
    case "editor_stop": return callEditor(name, "/stop");
    case "editor_simulate": return callEditor(name, "/simulate");

    // ---- Camera ------------------------------------------------------------
    case "editor_camera_zoom": {
      const amount = pickNumber(args.amount, 0.15, { min: -0.9, max: 0.9 });
      return callEditor(name, `/zoom/${amount}`);
    }
    case "editor_camera_move": {
      const fx = pickNumber(args.forward, 0);
      const ry = pickNumber(args.right, 0);
      const uz = pickNumber(args.up, 0);
      return callEditor(name, `/move/${fx}/${ry}/${uz}`);
    }
    case "editor_camera_turn": {
      const yaw = pickNumber(args.yaw, 0);
      const pitch = pickNumber(args.pitch, 0, { min: -89, max: 89 });
      return callEditor(name, `/turn/${yaw}/${pitch}`);
    }
    case "editor_camera_focus": {
      const x = pickNumber(args.x, 0);
      const y = pickNumber(args.y, 0);
      const z = pickNumber(args.z, 0);
      return callEditor(name, `/focus/${x}/${y}/${z}`);
    }

    // ---- Scene -------------------------------------------------------------
    case "editor_new_scene": return callEditor(name, "/new-scene");
    case "editor_open_scene": {
      const p = requireText(args.path, "path");
      return callEditor(name, `/open-scene?path=${queryEscape(p)}`);
    }
    case "editor_save_scene": return callEditor(name, "/save-scene");
    case "editor_add_entity": {
      const type = requireEnum(args.type, ENTITY_TYPES, "type");
      return callEditor(name, `/add-entity/${type}`);
    }
    case "editor_select_entity": {
      if (args.uuid) {
        const uuid = requireUuid(args.uuid, "uuid");
        return callEditor(name, `/select/${uuid}`);
      }
      if (args.name) {
        return callEditor(name, `/select?name=${queryEscape(requireText(args.name, "name"))}`);
      }
      throw new Error("provide either 'uuid' or 'name'");
    }
    case "editor_delete_entity": {
      const uuid = requireUuid(args.uuid);
      return callEditor(name, `/delete-entity/${uuid}`);
    }
    case "editor_rename_entity": {
      const uuid = requireUuid(args.uuid);
      const name = requireText(args.name, "name", { max: 64 });
      return callEditor(name, `/rename-entity/${uuid}?name=${queryEscape(name)}`);
    }
    case "editor_set_transform": {
      const uuid = requireUuid(args.uuid);
      const p = (v, d) => pickNumber(v, d);
      const hasRot = args.rot_x !== undefined || args.rot_y !== undefined || args.rot_z !== undefined;
      const hasScale = args.scale_x !== undefined || args.scale_y !== undefined || args.scale_z !== undefined;
      let endpoint = `/set-transform/${uuid}/${p(args.x, 0)}/${p(args.y, 0)}/${p(args.z, 0)}`;
      if (hasRot) endpoint += `/${p(args.rot_x, 0)}/${p(args.rot_y, 0)}/${p(args.rot_z, 0)}`;
      if (hasScale) endpoint += `/${p(args.scale_x, 1)}/${p(args.scale_y, 1)}/${p(args.scale_z, 1)}`;
      return callEditor(name, endpoint);
    }
    case "editor_add_component": {
      const uuid = requireUuid(args.uuid);
      const type = requireEnum(args.type, COMPONENT_TYPES, "type");
      return callEditor(name, `/add-component/${uuid}/${type}`);
    }

    // ---- Gizmos ------------------------------------------------------------
    case "editor_set_gizmo": {
      const mode = requireEnum(args.mode, GIZMO_MODES, "mode");
      return callEditor(name, `/gizmo/${mode}`);
    }
    case "editor_set_gizmo_space": {
      const space = requireEnum(args.space, GIZMO_SPACES, "space");
      return callEditor(name, `/gizmo-space/${space}`);
    }
    case "editor_set_snap": {
      const step = pickNumber(args.step, 0.5, { min: 0, max: 100 });
      return callEditor(name, `/snap/${step}`);
    }

    // ---- Assets ------------------------------------------------------------
    case "editor_import_asset": {
      const p = requireText(args.path, "path");
      return callEditor(name, `/import?path=${queryEscape(p)}`);
    }
    case "editor_create_block_model": {
      const uuid = requireUuid(args.texture_uuid, "texture_uuid");
      return callEditor(name, `/block-model/${uuid}`);
    }
    case "editor_spawn_block": {
      const uuid = requireUuid(args.block_uuid, "block_uuid");
      return callEditor(name, `/spawn-block/${uuid}`);
    }
    case "editor_duplicate_asset": {
      const uuid = requireUuid(args.uuid);
      return callEditor(name, `/asset-duplicate/${uuid}`);
    }
    case "editor_delete_asset": {
      const uuid = requireUuid(args.uuid);
      return callEditor(name, `/asset-delete/${uuid}`);
    }
    case "editor_reimport_asset": {
      const uuid = requireUuid(args.uuid);
      return callEditor(name, `/reimport/${uuid}`);
    }

    // ---- Voxel -------------------------------------------------------------
    case "editor_voxel_generate": {
      const uuid = requireUuid(args.uuid);
      const seed = Math.trunc(pickNumber(args.seed, 1337, { min: 0, max: 2147483647 }));
      const sea = pickNumber(args.sea_level, 24);
      return callEditor(name, `/voxel-generate/${uuid}/${seed}/${sea}`);
    }
    case "editor_voxel_paint": {
      const uuid = requireUuid(args.uuid);
      const x = Math.trunc(pickNumber(args.x, 0));
      const y = Math.trunc(pickNumber(args.y, 0));
      const z = Math.trunc(pickNumber(args.z, 0));
      const type = Math.trunc(pickNumber(args.type, 2, { min: 0, max: 65535 }));
      const mode = Math.trunc(pickNumber(args.mode, 0, { min: 0, max: 1 }));
      return callEditor(name, `/voxel-paint/${uuid}/${x}/${y}/${z}/${type}/${mode}`);
    }
    case "editor_voxel_clear": {
      const uuid = requireUuid(args.uuid);
      return callEditor(name, `/voxel-clear/${uuid}`);
    }

    // ---- Scripts -----------------------------------------------------------
    case "editor_script_event": {
      const name = requireText(args.name, "name");
      return callEditor(name, `/script-event?name=${queryEscape(name)}`);
    }
    case "editor_script_pause": return callEditor(name, "/script-pause");
    case "editor_script_continue": return callEditor(name, "/script-continue");
    case "editor_script_step": return callEditor(name, "/script-step");

    // ---- Windows / editors / theme / weather -------------------------------
    case "editor_toggle_window": {
      const w = requireEnum(args.window, WINDOW_NAMES, "window");
      return callEditor(name, `/window/${w}`);
    }
    case "editor_open_specialized_editor": {
      const tab = requireEnum(args.tab, EDITOR_TABS, "tab");
      return callEditor(name, `/editor/${tab}`);
    }
    case "editor_set_theme": {
      const f = (v, d) => pickNumber(v, d, { min: 0, max: 1 });
      return callEditor(
        name,
        `/theme/${f(args.bg_r, 0.05)}/${f(args.bg_g, 0.08)}/${f(args.bg_b, 0.12)}` +
        `/${f(args.panel_r, 0.15)}/${f(args.panel_g, 0.15)}/${f(args.panel_b, 0.18)}`
      );
    }
    case "editor_set_weather": {
      const f = (v, d) => pickNumber(v, d);
      return callEditor(
        name,
        `/weather/${f(args.sun_r, 0.9)}/${f(args.sun_g, 0.7)}/${f(args.sun_b, 0.5)}` +
        `/${f(args.fog_density, 0.01)}/${f(args.fog_start, 20)}/${f(args.sky_exposure, 1.2)}/${f(args.rain, 0.3)}`
      );
    }

    // ---- Terrain / graphics / settings / project / mesh / dev --------------
    case "editor_generate_terrain": {
      const scale = pickNumber(args.scale, 1, { min: 0.01, max: 1000 });
      const octaves = Math.trunc(pickNumber(args.octaves, 4, { min: 1, max: 8 }));
      const amount = pickNumber(args.amount, 0.5);
      const falloff = pickNumber(args.falloff, 0.4);
      return callEditor(name, `/terrain/${scale}/${octaves}/${amount}/${falloff}`);
    }
    case "editor_set_graphics": {
      const vsync = args.vsync === false ? 0 : 1;
      const quality = Math.trunc(pickNumber(args.quality, 2, { min: 1, max: 4 }));
      return callEditor(name, `/graphics/${vsync}/${quality}`);
    }
    case "editor_save_settings": return callEditor(name, "/save-settings");
    case "editor_create_project": {
      const name = requireText(args.name, "name", { max: 64 });
      return callEditor(name, `/project/${name}`);
    }
    case "editor_apply_mesh": {
      const mode = Math.trunc(pickNumber(args.mode, 0, { min: 0, max: 1 }));
      return callEditor(name, `/mesh/${mode}`);
    }
    case "editor_run_self_test": {
      const which = Math.trunc(pickNumber(args.which, 0, { min: 0, max: 4 }));
      const label = SELF_TESTS[which];
      const res = await callEditor(name, `/selftest/${which}`, { test: label });
      return res;
    }
    case "editor_package_assets": return callEditor(name, "/package");
    case "editor_hot_reload": return callEditor(name, "/hot-reload");

    default:
      return undefined;
  }
}
