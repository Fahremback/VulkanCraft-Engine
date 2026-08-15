import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
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

let nextId = 1;
function request(method, params = {}) {
  const id = nextId++;
  const response = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timeout waiting for ${method}; stderr=${stderr}`)), 5000);
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

  const validationResponse = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const validation = JSON.parse(validationResponse.result.content[0].text);
  assert.equal(validation.valid, true, JSON.stringify(validation));

  process.stdout.write("MCP protocol smoke test passed\n");
} finally {
  fs.rmSync(smokeAbsolutePath, { force: true });
  fs.rmSync(smokeProjectPath, { recursive: true, force: true });
  child.stdin.end();
  child.kill();
}
