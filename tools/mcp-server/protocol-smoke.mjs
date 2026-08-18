import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const smokeRelativePath = "tools/mcp-server/mcp-smoke-temp.txt";
const smokeAbsolutePath = path.join(directory, "mcp-smoke-temp.txt");
const smokeProject = `McpSmoke${Date.now()}`;
const smokeProjectPath = path.resolve(directory, "..", "..", "Projects", smokeProject);
const child = spawn(process.execPath, [path.join(directory, "server.mjs")], {
  cwd: directory,
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true
});

let buffer = "";
let stderr = "";
const pending = new Map();

child.stderr.setEncoding("utf8");
child.stderr.on("data", (chunk) => { stderr += chunk; });
child.stdout.setEncoding("utf8");
child.stdout.on("data", (chunk) => {
  buffer += chunk;
  while (buffer.includes("\n")) {
    const newline = buffer.indexOf("\n");
    const line = buffer.slice(0, newline).trim();
    buffer = buffer.slice(newline + 1);
    if (!line) continue;
    const message = JSON.parse(line);
    pending.get(message.id)?.(message);
    pending.delete(message.id);
  }
});

// Minimal JSON Schema (draft-07) validator covering the subset the exported
// registry schemas use (type/required/properties/enum/items/minItems/
// maxItems). The smoke uses it to prove the exported schemas (FALTANTES item
// 10) actually accept the emitted asset documents and reject the refused
// cases — no third-party dependency.
function validateJsonSchema(value, schema, pathStr = "$") {
  const errors = [];
  if (schema.enum !== undefined && !schema.enum.includes(value)) {
    errors.push(`${pathStr} must be one of ${JSON.stringify(schema.enum)} (got ${JSON.stringify(value)})`);
  }
  const type = schema.type;
  if (type === "object") {
    if (value === null || typeof value !== "object" || Array.isArray(value)) {
      errors.push(`${pathStr} must be an object`);
      return errors;
    }
    for (const requiredKey of schema.required ?? []) {
      if (value[requiredKey] === undefined) errors.push(`${pathStr} is missing required property '${requiredKey}'`);
    }
    for (const [key, subSchema] of Object.entries(schema.properties ?? {})) {
      if (value[key] !== undefined) errors.push(...validateJsonSchema(value[key], subSchema, `${pathStr}.${key}`));
    }
  } else if (type === "array") {
    if (!Array.isArray(value)) {
      errors.push(`${pathStr} must be an array`);
      return errors;
    }
    if (schema.minItems !== undefined && value.length < schema.minItems) errors.push(`${pathStr} must have at least ${schema.minItems} items`);
    if (schema.maxItems !== undefined && value.length > schema.maxItems) errors.push(`${pathStr} must have at most ${schema.maxItems} items`);
    if (schema.items) for (let i = 0; i < value.length; i++) errors.push(...validateJsonSchema(value[i], schema.items, `${pathStr}[${i}]`));
  } else if (type === "string") {
    if (typeof value !== "string") errors.push(`${pathStr} must be a string`);
  } else if (type === "boolean") {
    if (typeof value !== "boolean") errors.push(`${pathStr} must be a boolean`);
  } else if (type === "number") {
    if (typeof value !== "number") errors.push(`${pathStr} must be a number`);
  } else if (type === "integer") {
    if (!Number.isInteger(value)) errors.push(`${pathStr} must be an integer`);
  }
  return errors;
}

let nextId = 1;
function request(method, params = {}) {
  const id = nextId++;
  const response = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timeout waiting for ${method} (id=${id}); stderr=${stderr}`)), 25000);
    pending.set(id, (message) => { clearTimeout(timer); resolve(message); });
  });
  child.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
  return response;
}

try {
  const initialized = await request("initialize", { protocolVersion: "2025-03-26", capabilities: {}, clientInfo: { name: "smoke", version: "1" } });
  assert.equal(initialized.result.serverInfo.name, "vulkancraft-engine");

  const listed = await request("tools/list");
  assert.ok(listed.result.tools.some((tool) => tool.name === "apply_text_edits"));
  assert.ok(listed.result.tools.some((tool) => tool.name === "engine_overview"));
  assert.ok(listed.result.tools.some((tool) => tool.name === "create_game_project"));
  assert.ok(listed.result.tools.some((tool) => tool.name === "set_component"));

  const overview = await request("tools/call", { name: "engine_overview", arguments: {} });
  assert.equal(overview.result.isError, undefined);
  assert.match(overview.result.content[0].text, /VulkanEngineGame/);

  const read = await request("tools/call", { name: "read_file", arguments: { path: "README.md", start_line: 1, end_line: 8 } });
  const readPayload = JSON.parse(read.result.content[0].text);
  assert.match(readPayload.sha256, /^[a-f0-9]{64}$/);
  assert.equal(readPayload.start_line, 1);

  const search = await request("tools/call", { name: "search_code", arguments: { query: "add_executable", path: ".", glob: "CMakeLists.txt", max_results: 10 } });
  assert.equal(search.result.isError, undefined);

  const rejected = await request("tools/call", {
    name: "apply_text_edits",
    arguments: {
      path: "README.md",
      expected_sha256: "0".repeat(64),
      dry_run: true,
      edits: [{ old_text: "Vulkan", new_text: "Vulkan" }]
    }
  });
  assert.equal(rejected.result.isError, true);
  assert.match(rejected.result.content[0].text, /concurrent modification detected/);

  const created = await request("tools/call", {
    name: "create_file",
    arguments: { path: smokeRelativePath, content: "before\n" }
  });
  assert.equal(created.result.isError, undefined);
  const createdPayload = JSON.parse(created.result.content[0].text);
  assert.equal(fs.readFileSync(smokeAbsolutePath, "utf8"), "before\n");

  const edited = await request("tools/call", {
    name: "apply_text_edits",
    arguments: {
      path: smokeRelativePath,
      expected_sha256: createdPayload.sha256,
      edits: [{ old_text: "before", new_text: "after" }]
    }
  });
  assert.equal(edited.result.isError, undefined);
  assert.equal(fs.readFileSync(smokeAbsolutePath, "utf8"), "after\n");

  const gameProject = await request("tools/call", {
    name: "create_game_project",
    arguments: { name: smokeProject, plugins: ["VoxelWorld", "Vehicles"], starter_scene: true }
  });
  assert.equal(gameProject.result.isError, undefined);
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Scenes", "Initial.scene")));

  const entityResponse = await request("tools/call", {
    name: "create_entity",
    arguments: {
      project: smokeProject,
      scene: "Initial",
      name: "Player",
      transform: { py: 3 },
      components: { Rigidbody: { mass: 80 }, Weapon: { damage: 42 } }
    }
  });
  assert.equal(entityResponse.result.isError, undefined);
  const entity = JSON.parse(entityResponse.result.content[0].text);

  const componentResponse = await request("tools/call", {
    name: "set_component",
    arguments: {
      project: smokeProject,
      scene: "Initial",
      entity_id: entity.entity_id,
      component: "Audio",
      values: { clip: "Audio/step.ogg", spatial: true }
    }
  });
  assert.equal(componentResponse.result.isError, undefined);

  const scriptResponse = await request("tools/call", {
    name: "create_visual_script",
    arguments: {
      project: smokeProject,
      name: "Initial",
      scene_companion: true,
      nodes: [
        { key: "start", kind: "Event", event: "OnStart" },
        { key: "one", kind: "ConstantFloat", literal: 1.0 },
        { key: "set", kind: "SetVariable", variable: "initialized" }
      ],
      links: [{ from: "start", to: "one" }, { from: "one", to: "set" }]
    }
  });
  assert.equal(scriptResponse.result.isError, undefined);

  const materialResponse = await request("tools/call", {
    name: "create_material",
    arguments: { project: smokeProject, name: "Player Metal", albedo: { r: 0.2, g: 0.3, b: 0.4 }, roughness: 0.25, metallic: 0.9 }
  });
  assert.equal(materialResponse.result.isError, undefined);
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Materials", "Player_Metal.material")));

  const audioResponse = await request("tools/call", {
    name: "create_audio_event",
    arguments: { project: smokeProject, name: "Footstep", clip: "Assets/footstep.ogg", min_pitch: 0.92, max_pitch: 1.08, spatial: true }
  });
  assert.equal(audioResponse.result.isError, undefined);

  const physicsMaterialResponse = await request("tools/call", {
    name: "create_physics_material",
    arguments: { project: smokeProject, name: "Ice", friction: 0.03, restitution: 0.05, density: 920 }
  });
  assert.equal(physicsMaterialResponse.result.isError, undefined);

  const registryBlock = await request("tools/call", {
    name: "author_registry_asset",
    arguments: {
      project: smokeProject, kind: "block", name: "Titanium", hardness: 3.5, opaque: true,
      tags: ["metal"], drops: ["vulkancraft:titanium"],
      collision_shape: "cross", selection_shape: "none",
      face_top: [0.2, 0.8, 0.2], face_side: [0.5, 0.35, 0.2],
      occlusion: false, render_layer: 1,
      sound_place: "vulkancraft:titanium_place", sound_break: "vulkancraft:titanium_break",
      particle_break: "vulkancraft:metal_dust", tool: "pickaxe", tool_tier: 2,
      resistance: 30, friction: 0.4, bounciness: 0.02, density: 7.5,
      behavior: "vulkancraft:reinforced",
      states: [
        { name: "base", color: [0.5, 0.4, 0.3] },
        { name: "lit", color: [1.0, 0.6, 0.1], face_top: [0.9, 0.9, 0.9], light_emission: 0.8 }
      ],
      transitions: [
        { from: "", to: "lit", trigger: "ignite" },
        { from: "lit", to: "", trigger: "extinguish" }
      ]
    }
  });
  assert.equal(registryBlock.result.isError, undefined, JSON.stringify(registryBlock));
  const registryBlockPayload = JSON.parse(registryBlock.result.content[0].text);
  assert.equal(registryBlockPayload.created, true);
  assert.equal(registryBlockPayload.diagnostics.length, 0);
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "Titanium.json")));

  // §14 validation: out-of-range renderLayer is refused with a diagnostic.
  const badLayer = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "BadLayer", render_layer: 300, dry_run: true }
  });
  assert.equal(badLayer.result.isError, undefined, JSON.stringify(badLayer));
  const badLayerPayload = JSON.parse(badLayer.result.content[0].text);
  assert.equal(badLayerPayload.refused, true);
  assert.ok(badLayerPayload.diagnostics.some((d) => String(d).includes("renderLayer")));

  // §2 item 2: an explicit unknown collision shape is refused (strict enum).
  const badShape = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "BadShape", collision_shape: "octagon", dry_run: true }
  });
  assert.equal(badShape.result.isError, undefined, JSON.stringify(badShape));
  const badShapePayload = JSON.parse(badShape.result.content[0].text);
  assert.equal(badShapePayload.refused, true);
  assert.ok(badShapePayload.diagnostics.some((d) => String(d).includes("collisionShape")));

  const registryItem = await request("tools/call", {
    name: "author_registry_asset",
    arguments: {
      project: smokeProject, kind: "item", name: "titanium_ingot", max_stack: 16, tags: ["metal"],
      use_cooldown: 300, use_mode: "instant", equip_slot: "hand", attack_damage: 4.5, armor: 0,
      behavior: "vulkancraft:combat_use"
    }
  });
  assert.equal(registryItem.result.isError, undefined);

  // §2 item 9: the recipe input and the Titanium drop resolve to this item.
  const registryInputItem = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "item", name: "titanium", max_stack: 64, tags: ["metal"] }
  });
  assert.equal(registryInputItem.result.isError, undefined);

  // §2 item 9: the fluid drives an authored block, so cross-references resolve.
  // §2 item 7: the block also declares its own inline fluid binding (the
  // explicit FluidRegistry entry wins at the world table; the inline binding
  // is what a catalog-only fluid block uses with no separate asset).
  const registrySludgeBlock = await request("tools/call", {
    name: "author_registry_asset",
    arguments: {
      project: smokeProject, kind: "block", name: "sludge", hardness: 1, opaque: true,
      fluid: { viscosity: 0.8, density: 1.2, range: 4, damage_per_tick: 2, evaporation: false }
    }
  });
  assert.equal(registrySludgeBlock.result.isError, undefined);
  // The sludge block auto-drops "vulkancraft:sludge", so the item must exist
  // for the project's cross-references to be consistent.
  const registrySludgeItem = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "item", name: "sludge", max_stack: 64, tags: [] }
  });
  assert.equal(registrySludgeItem.result.isError, undefined);

  // §2 item 5 validation: a transition referencing an unknown state is refused.
  const badTransition = await request("tools/call", {
    name: "author_registry_asset",
    arguments: {
      project: smokeProject, kind: "block", name: "BadState",
      states: [{ name: "on" }],
      transitions: [{ from: "", to: "off", trigger: "toggle" }],
      dry_run: true
    }
  });
  assert.equal(badTransition.result.isError, undefined, JSON.stringify(badTransition));
  const badTransitionPayload = JSON.parse(badTransition.result.content[0].text);
  assert.equal(badTransitionPayload.refused, true);
  assert.ok(badTransitionPayload.diagnostics.some((d) => String(d).includes("to-state 'off'")));

  // §2 item 6 validation: an unnamespaced behavior reference is refused.
  const badBehavior = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "BadBehavior", behavior: "unnamespaced", dry_run: true }
  });
  assert.equal(badBehavior.result.isError, undefined, JSON.stringify(badBehavior));
  const badBehaviorPayload = JSON.parse(badBehavior.result.content[0].text);
  assert.equal(badBehaviorPayload.refused, true);
  assert.ok(badBehaviorPayload.diagnostics.some((d) => String(d).includes("behaviorId")));

  // §2 item 4 validation: an unknown tool class is refused with a diagnostic.
  const badTool = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "BadTool", tool: "drill", dry_run: true }
  });
  assert.equal(badTool.result.isError, undefined, JSON.stringify(badTool));
  const badToolPayload = JSON.parse(badTool.result.content[0].text);
  assert.equal(badToolPayload.refused, true);
  assert.ok(badToolPayload.diagnostics.some((d) => String(d).includes("tool' must be")));

  // §2 item 7 validation: an out-of-contract inline fluid range is refused.
  const badFluid = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "BadFluid", fluid: { range: 9 }, dry_run: true }
  });
  assert.equal(badFluid.result.isError, undefined, JSON.stringify(badFluid));
  const badFluidPayload = JSON.parse(badFluid.result.content[0].text);
  assert.equal(badFluidPayload.refused, true);
  assert.ok(badFluidPayload.diagnostics.some((d) => String(d).includes("fluid.range")));

  // §2 item 8 validation: an unknown equipSlot is refused with a diagnostic.
  const badItem = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "item", name: "BadItem", equip_slot: "wings", dry_run: true }
  });
  assert.equal(badItem.result.isError, undefined, JSON.stringify(badItem));
  const badItemPayload = JSON.parse(badItem.result.content[0].text);
  assert.equal(badItemPayload.refused, true);
  assert.ok(badItemPayload.diagnostics.some((d) => String(d).includes("equipSlot")));

  const registryFluid = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "fluid", name: "Sludge", block: "vulkancraft:sludge", viscosity: 0.8, damage_per_tick: 2 }
  });
  assert.equal(registryFluid.result.isError, undefined);

  const registryRecipe = await request("tools/call", {
    name: "author_registry_asset",
    arguments: {
      project: smokeProject, kind: "recipe", name: "TitaniumIngot", station: "vulkancraft:furnace",
      inputs: [{ item: "vulkancraft:titanium", count: 2 }],
      outputs: [{ item: "vulkancraft:titanium_ingot", count: 1 }]
    }
  });
  assert.equal(registryRecipe.result.isError, undefined);

  const registryBiome = await request("tools/call", {
    name: "author_registry_asset",
    arguments: {
      project: smokeProject, kind: "biome", name: "my_biomes",
      biomes: [{ name: "crystal_plains", engine_biome_index: 12, climate: { temperature: [0.2, 0.8], moisture: [0.3, 0.9] }, surface: [{ block_id: 3, min_depth: 0, max_depth: 3 }] }]
    }
  });
  assert.equal(registryBiome.result.isError, undefined);

  const registryStructure = await request("tools/call", {
    name: "author_registry_asset",
    arguments: {
      project: smokeProject, kind: "structure", name: "ruin",
      sample_width: 4, sample_height: 4,
      sample: [1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1],
      pattern_size: 2, profiles: [{ block_id: 1, layers: [1, 1, 2] }]
    }
  });
  assert.equal(registryStructure.result.isError, undefined);

  // Dry-run previews the diff without writing.
  const dryRun = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "Titanium", hardness: 9, dry_run: true }
  });
  const dryRunPayload = JSON.parse(dryRun.result.content[0].text);
  assert.equal(dryRunPayload.dry_run, true);
  assert.ok(dryRunPayload.diff.changed_fields.includes("hardness"));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "Titanium.json")));

  // Invalid assets are refused with structured diagnostics, nothing written.
  const refusedBuiltin = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "Bad", builtin_id: 3 }
  });
  const refusedBuiltinPayload = JSON.parse(refusedBuiltin.result.content[0].text);
  assert.equal(refusedBuiltinPayload.refused, true);
  assert.match(refusedBuiltinPayload.diagnostics.join(" "), /already used by the builtin table/);
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "Bad.json")));

  const refusedRecipe = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "recipe", name: "Broken", inputs: [{ item: "vulkancraft:titanium" }] }
  });
  const refusedRecipePayload = JSON.parse(refusedRecipe.result.content[0].text);
  assert.equal(refusedRecipePayload.refused, true);
  assert.match(refusedRecipePayload.diagnostics.join(" "), /at least one output/);

  const inspectRegistry = await request("tools/call", {
    name: "inspect_registry_assets",
    arguments: { project: smokeProject }
  });
  const inspectRegistryPayload = JSON.parse(inspectRegistry.result.content[0].text);
  assert.equal(inspectRegistryPayload.count, 9);
  assert.ok(inspectRegistryPayload.registry_assets.every((asset) => asset.valid));

  const validationResponse = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const validation = JSON.parse(validationResponse.result.content[0].text);
  assert.equal(validation.valid, true, JSON.stringify(validation));
  assert.ok(validation.registry_assets >= 9);

  // §2 item 9: a dangling drop makes the project invalid (never guessed).
  const danglingBlock = await request("tools/call", {
    name: "author_registry_asset",
    arguments: { project: smokeProject, kind: "block", name: "Dangling", hardness: 1, drops: ["vulkancraft:nope"] }
  });
  assert.equal(danglingBlock.result.isError, undefined);
  const danglingValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const danglingValidationPayload = JSON.parse(danglingValidation.result.content[0].text);
  assert.equal(danglingValidationPayload.valid, false, JSON.stringify(danglingValidationPayload));
  assert.ok(danglingValidationPayload.errors.some((error) => error.includes("vulkancraft:nope")));
  fs.rmSync(path.join(smokeProjectPath, "Content", "Registry", "block", "Dangling.json"), { force: true });
  const restoredValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  assert.equal(JSON.parse(restoredValidation.result.content[0].text).valid, true);

  // ---- FALTANTES item 10: complete schemas for editor/scripting/CLI/MCP ----

  // The MCP exposes the full JSON Schema (draft-07) per registry kind,
  // generated from the SAME REGISTRY_FIELD_SCHEMAS the author tool mirrors.
  const capabilities = await request("tools/call", { name: "game_capabilities", arguments: {} });
  const capabilitiesPayload = JSON.parse(capabilities.result.content[0].text);
  const schemaKinds = ["block", "item", "fluid", "recipe", "biome", "structure"];
  assert.ok(capabilitiesPayload.registry_schemas, "game_capabilities exposes registry_schemas");
  for (const kind of schemaKinds) {
    const schema = capabilitiesPayload.registry_schemas[kind];
    assert.ok(schema, `registry schema for '${kind}'`);
    assert.equal(schema.$schema, "http://json-schema.org/draft-07/schema#");
    assert.equal(schema.type, "object");
    assert.ok(schema.properties.version, `'${kind}' schema declares version`);
  }

  // Every emitted asset document passes its own exported schema.
  const emittedKinds = [
    ["block", "Titanium"], ["block", "sludge"],
    ["item", "titanium_ingot"], ["item", "titanium"], ["item", "sludge"],
    ["fluid", "Sludge"], ["recipe", "TitaniumIngot"],
    ["biome", "my_biomes"], ["structure", "ruin"]
  ];
  for (const [kind, name] of emittedKinds) {
    const file = path.join(smokeProjectPath, "Content", "Registry", kind, `${name}.json`);
    const document = JSON.parse(fs.readFileSync(file, "utf8"));
    const schemaErrors = validateJsonSchema(document, capabilitiesPayload.registry_schemas[kind]);
    assert.equal(schemaErrors.length, 0, `${kind}/${name}: ${schemaErrors.join("; ")}`);
  }

  // And the schema rejects the cases the mirror validation refuses.
  const badSchemaDoc = { version: 1, namespace: "vulkancraft", name: "X", collisionShape: "octagon" };
  const badSchemaErrors = validateJsonSchema(badSchemaDoc, capabilitiesPayload.registry_schemas.block);
  assert.ok(badSchemaErrors.some((error) => error.includes("octagon")), JSON.stringify(badSchemaErrors));
  const badItemSchemaDoc = { version: 1, namespace: "vulkancraft", name: "X", equipSlot: "wings" };
  const badItemSchemaErrors = validateJsonSchema(badItemSchemaDoc, capabilitiesPayload.registry_schemas.item);
  assert.ok(badItemSchemaErrors.some((error) => error.includes("wings")), JSON.stringify(badItemSchemaErrors));

  // The CLI exposes the same artifacts without a running server.
  const cliPath = path.join(directory, "registry-cli.mjs");
  const engineRoot = path.resolve(directory, "..", "..");
  const runCli = (args) => spawnSync(process.execPath, [cliPath, ...args], { encoding: "utf8" });
  const cliKinds = runCli(["kinds"]);
  assert.equal(cliKinds.status, 0, cliKinds.stderr);
  assert.equal(cliKinds.stdout.split(/\n/).filter(Boolean).length, schemaKinds.length + 2, cliKinds.stdout);
  const cliSchema = runCli(["schema", "block"]);
  assert.equal(cliSchema.status, 0, cliSchema.stderr);
  assert.deepEqual(JSON.parse(cliSchema.stdout).properties.collisionShape.enum, ["full", "cross", "none"]);
  // §17 item 12: the CLI exposes the vehicle-assembly schemas too.
  const cliVehicleSchema = runCli(["schema", "vehicle"]);
  assert.equal(cliVehicleSchema.status, 0, cliVehicleSchema.stderr);
  assert.deepEqual(JSON.parse(cliVehicleSchema.stdout).properties.kind.enum, ["wheeled", "motorcycle", "tracked"]);
  const cliBeamSchema = runCli(["schema", "beam"]);
  assert.equal(cliBeamSchema.status, 0, cliBeamSchema.stderr);
  assert.ok(JSON.parse(cliBeamSchema.stdout).properties.nodes, "beam schema declares nodes");

  const goodDoc = path.join(directory, "mcp-smoke-good-doc.json");
  const badDoc = path.join(directory, "mcp-smoke-bad-doc.json");
  const cliDoc = path.join(directory, "mcp-smoke-cli-doc.json");
  const schemaOutDir = path.join(directory, "mcp-smoke-schemas");
  fs.writeFileSync(goodDoc, JSON.stringify({ version: 1, namespace: "vulkancraft", name: "CliBlock", hardness: 2 }));
  fs.writeFileSync(badDoc, JSON.stringify({ version: 1, namespace: "vulkancraft", name: "CliBad", collisionShape: "octagon" }));
  fs.writeFileSync(cliDoc, JSON.stringify({ version: 1, namespace: "vulkancraft", name: "CliStone", hardness: 1.5, collisionShape: "cross" }));
  const cliValid = runCli(["validate", "block", goodDoc]);
  assert.equal(cliValid.status, 0, cliValid.stderr);
  const cliInvalid = runCli(["validate", "block", badDoc]);
  assert.equal(cliInvalid.status, 1, "invalid document must exit 1");
  const cliExport = runCli(["export-schemas", schemaOutDir]);
  assert.equal(cliExport.status, 0, cliExport.stderr);
  for (const kind of schemaKinds) assert.ok(fs.existsSync(path.join(schemaOutDir, `${kind}.json`)));

  // Author through the CLI: writes the asset via the same validation; a second
  // author without --update is refused; --dry-run writes nothing.
  const cliAuthor = runCli(["author", "--engine", engineRoot, "--project", smokeProject, "--kind", "block", "--name", "CliStone", "--file", cliDoc]);
  assert.equal(cliAuthor.status, 0, cliAuthor.stderr);
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "CliStone.json")));
  const cliAuthorAgain = runCli(["author", "--engine", engineRoot, "--project", smokeProject, "--kind", "block", "--name", "CliStone", "--file", cliDoc]);
  assert.equal(cliAuthorAgain.status, 1, "second author without --update must be refused");
  const cliDryRun = runCli(["author", "--engine", engineRoot, "--project", smokeProject, "--kind", "block", "--name", "CliDry", "--file", cliDoc, "--dry-run"]);
  assert.equal(cliDryRun.status, 0, cliDryRun.stderr);
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "CliDry.json")));

  // ---- FALTANTES §17 item 12: vehicle assembly authoring ----

  // A rigid wheeled VehicleAsset with seats and a fuel system. The document
  // must load unchanged through VehicleAsset::load_from_json (proved by
  // tests/VehicleAssetGateTests.cpp, which mirrors this exact payload).
  const vehicleAsset = await request("tools/call", {
    name: "author_vehicle_asset",
    arguments: {
      project: smokeProject, kind: "vehicle", name: "Pickup",
      vehicle_kind: "wheeled", position: [0, 0.5, 0], rotation: [0, 0, 0, 1],
      chassis: { shape: "box", half_extents: [1.1, 0.4, 0.65], mass: 1500, friction: 0.6, restitution: 0.05 },
      wheels: [
        { localPosition: [0.95, 0, 0.7], radius: 0.38, suspension_rest_length: 0.5, max_drive_force: 4200, max_steer_angle: 0.5, steering: true, driven: true },
        { localPosition: [-0.95, 0, 0.7], radius: 0.38, suspension_rest_length: 0.5, max_drive_force: 4200, max_steer_angle: 0.5, steering: true, driven: true },
        { localPosition: [0.95, 0, -0.7], radius: 0.38, suspension_rest_length: 0.5, max_drive_force: 4200, driven: true },
        { localPosition: [-0.95, 0, -0.7], radius: 0.38, suspension_rest_length: 0.5, max_drive_force: 4200, driven: true }
      ],
      drivetrain: { engine_max_torque: 1800, engine_min_rpm: 900, engine_max_rpm: 6500, differential_ratio: 3.7, gear_ratios: [3.2, 2.1, 1.5, 1.1, 0.85] },
      seats: [{ name: "driver", localPosition: [0.2, 0.5, 0.3], exitOffset: [0, 1.1, 1.2] }],
      power: {
        fuel: { capacity: 60, initial_level: 0.8, burn_per_second: 0.12, min_level_to_run: 0.05 },
        energy: { capacity: 0 },
        controls: { throttle_deadzone: 0.05, throttle_sensitivity: 1.2, steering_invert: false }
      }
    }
  });
  assert.equal(vehicleAsset.result.isError, undefined, JSON.stringify(vehicleAsset));
  const vehiclePayload = JSON.parse(vehicleAsset.result.content[0].text);
  assert.equal(vehiclePayload.created, true);
  assert.equal(vehiclePayload.diagnostics.length, 0, JSON.stringify(vehiclePayload));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Vehicles", "Pickup.json")));

  // A node/beam BeamGraphAsset — the soft-body chassis. Mirrored by
  // BeamGraphAsset::load_from_json in VehicleAssetGateTests.cpp.
  const beamAsset = await request("tools/call", {
    name: "author_vehicle_asset",
    arguments: {
      project: smokeProject, kind: "beam", name: "SoftCrawler",
      mass: 900,
      nodes: [
        { position: [1.2, 0, 0.8] }, { position: [1.2, 0, -0.8] },
        { position: [-1.2, 0, 0.8] }, { position: [-1.2, 0, -0.8] },
        { position: [0, 1, 0], fixed: true }
      ],
      beams: [
        { a: 0, b: 1, stiffness: 0.95 }, { a: 1, b: 3, stiffness: 0.9 },
        { a: 3, b: 2, stiffness: 0.95 }, { a: 2, b: 0, stiffness: 0.9 },
        { a: 0, b: 4 }, { a: 1, b: 4 }, { a: 2, b: 4 }, { a: 3, b: 4 }
      ],
      wheels: [{ node: 0, steering: true, driven: true, wheel: { radius: 0.4, suspension_rest_length: 0.5, max_drive_force: 3200 } }],
      solver: { substeps: 3, solver_iterations: 12, stiffness: 0.85, damping: 0.15, gravity: [0, -9.81, 0] }
    }
  });
  assert.equal(beamAsset.result.isError, undefined, JSON.stringify(beamAsset));
  const beamPayload = JSON.parse(beamAsset.result.content[0].text);
  assert.equal(beamPayload.created, true);
  assert.equal(beamPayload.diagnostics.length, 0, JSON.stringify(beamPayload));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Vehicles", "SoftCrawler.json")));

  // §17 item 12 validation: a wheel with radius <= 0 is refused (all-or-nothing).
  const badVehicle = await request("tools/call", {
    name: "author_vehicle_asset",
    arguments: {
      project: smokeProject, kind: "vehicle", name: "BrokenWheel",
      chassis: { shape: "box", half_extents: [1, 0.4, 0.6], mass: 1000 },
      wheels: [{ localPosition: [0, 0, 0], radius: 0 }],
      drivetrain: { engine_min_rpm: 1000, engine_max_rpm: 6000, differential_ratio: 3.42, gear_ratios: [2.66] }
    }
  });
  assert.equal(badVehicle.result.isError, undefined, JSON.stringify(badVehicle));
  const badVehiclePayload = JSON.parse(badVehicle.result.content[0].text);
  assert.equal(badVehiclePayload.refused, true, JSON.stringify(badVehiclePayload));
  assert.ok(badVehiclePayload.diagnostics.some((d) => String(d).includes("radius")), JSON.stringify(badVehiclePayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Vehicles", "BrokenWheel.json")));

  // A beam referencing an invalid node is refused.
  const badBeam = await request("tools/call", {
    name: "author_vehicle_asset",
    arguments: {
      project: smokeProject, kind: "beam", name: "BrokenBeam",
      nodes: [{ position: [0, 0, 0] }],
      beams: [{ a: 0, b: 7, stiffness: 0.9 }]
    }
  });
  assert.equal(badBeam.result.isError, undefined, JSON.stringify(badBeam));
  const badBeamPayload = JSON.parse(badBeam.result.content[0].text);
  assert.equal(badBeamPayload.refused, true, JSON.stringify(badBeamPayload));
  assert.ok(badBeamPayload.diagnostics.some((d) => String(d).includes("invalid node")), JSON.stringify(badBeamPayload));

  // The exported JSON Schema covers both vehicle kinds and the emitted
  // documents validate against it (editor/IDE/CI consumption).
  assert.ok(capabilitiesPayload.vehicle_schemas, "game_capabilities exposes vehicle_schemas");
  for (const kind of ["vehicle", "beam"]) {
    const schema = capabilitiesPayload.vehicle_schemas[kind];
    assert.ok(schema, `vehicle schema for '${kind}'`);
    assert.equal(schema.$schema, "http://json-schema.org/draft-07/schema#");
    assert.equal(schema.type, "object");
    assert.ok(schema.properties.version, `'${kind}' vehicle schema declares version`);
  }
  for (const [kind, name] of [["vehicle", "Pickup"], ["beam", "SoftCrawler"]]) {
    const file = path.join(smokeProjectPath, "Content", "Vehicles", `${name}.json`);
    const document = JSON.parse(fs.readFileSync(file, "utf8"));
    const schemaErrors = validateJsonSchema(document, capabilitiesPayload.vehicle_schemas[kind]);
    assert.equal(schemaErrors.length, 0, `${kind}/${name}: ${schemaErrors.join("; ")}`);
  }

  // The project validation covers vehicle assets and they are listed.
  const inspectVehicles = await request("tools/call", {
    name: "inspect_vehicle_assets",
    arguments: { project: smokeProject }
  });
  const inspectVehiclesPayload = JSON.parse(inspectVehicles.result.content[0].text);
  assert.equal(inspectVehiclesPayload.count, 2);
  assert.ok(inspectVehiclesPayload.vehicle_assets.every((asset) => asset.valid), JSON.stringify(inspectVehiclesPayload));

  // The CLI-authored CliStone auto-fills its drop to an unknown item (the
  // registry mirror flags it), so remove it before the final validation — the
  // vehicle assets themselves must validate clean.
  fs.rmSync(path.join(smokeProjectPath, "Content", "Registry", "block", "CliStone.json"), { force: true });
  const vehiclesValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const vehiclesValidationPayload = JSON.parse(vehiclesValidation.result.content[0].text);
  assert.equal(vehiclesValidationPayload.valid, true, JSON.stringify(vehiclesValidationPayload));
  assert.ok(vehiclesValidationPayload.vehicle_assets >= 2);

  process.stdout.write("MCP protocol smoke test passed\n");
} finally {
  for (const temp of ["mcp-smoke-good-doc.json", "mcp-smoke-bad-doc.json", "mcp-smoke-cli-doc.json"]) {
    fs.rmSync(path.join(directory, temp), { force: true });
  }
  fs.rmSync(path.join(directory, "mcp-smoke-schemas"), { recursive: true, force: true });
  fs.rmSync(smokeAbsolutePath, { force: true });
  fs.rmSync(smokeProjectPath, { recursive: true, force: true });
  child.stdin.end();
  child.kill();
}
