import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const smokeRelativePath = "tools/mcp-server/mcp-smoke-temp.txt";
const smokeAbsolutePath = path.join(directory, "mcp-smoke-temp.txt");
const smokeProject = `McpSmoke${Date.now()}`;
const collator = new Intl.Collator("en", { sensitivity: "variant" });
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
  assert.equal(initialized.result.protocolVersion, "2025-03-26", "initialize negotiates the supported version");
  assert.ok(initialized.result.capabilities.prompts, "initialize advertises the prompts capability");

  // ---- semantic versioning: serverInfo exposes engine semver + ABI from the
  // single source of truth (version.hpp), never a duplicated literal.
  assert.match(initialized.result.serverInfo.engineVersion, /^\d+\.\d+\.\d+$/, "serverInfo.engineVersion is semver");
  assert.ok(initialized.result.serverInfo.engineAbi, "serverInfo.engineAbi is present");
  const version = await request("tools/call", { name: "engine_version", arguments: {} });
  const versionPayload = JSON.parse(version.result.content[0].text);
  assert.match(versionPayload.engine, /^\d+\.\d+\.\d+$/, "engine_version.engine is semver");
  assert.equal(versionPayload.engine, initialized.result.serverInfo.engineVersion, "tool and serverInfo agree");
  assert.equal(versionPayload.engine_abi, initialized.result.serverInfo.engineAbi, "ABI agrees between tool and serverInfo");
  assert.equal(versionPayload.protocol, "2025-03-26", "engine_version exposes the protocol version");

  // ---- handshake version negotiation: both published MCP spec versions ----
  // are accepted and echoed; unknown versions refused with the full list ----
  // (findings #234-mcp-multiversion).
  const olderVersion = await request("initialize", { protocolVersion: "2024-11-05", capabilities: {}, clientInfo: { name: "smoke", version: "1" } });
  assert.equal(olderVersion.result.protocolVersion, "2024-11-05", "2024-11-05 negotiated and echoed");
  assert.equal(olderVersion.result.serverInfo.name, "vulkancraft-engine");
  const badVersion = await request("initialize", { protocolVersion: "2099-01-01", capabilities: {}, clientInfo: { name: "smoke", version: "1" } });
  assert.equal(badVersion.error.code, -32602);
  assert.match(badVersion.error.message, /unsupported protocol version/);
  assert.deepEqual(badVersion.error.data.supportedProtocolVersions, ["2024-11-05", "2025-03-26"]);

  // ---- prompts: game-creation recipes (FALTANTES item 5) ----
  const promptsListed = await request("prompts/list");
  const promptNames = promptsListed.result.prompts.map((p) => p.name);
  for (const expected of ["create_game_project", "author_block", "author_item",
    "author_biome", "create_material", "add_entity", "author_ability",
    "author_mission", "author_vehicle"]) {
    assert.ok(promptNames.includes(expected), `prompts/list includes ${expected}`);
  }
  // Each prompt declares its arguments (at least the named template argument).
  const blockPrompt = promptsListed.result.prompts.find((p) => p.name === "author_block");
  assert.ok(blockPrompt.arguments.some((a) => a.name === "name" && a.required), "author_block requires a name argument");

  // prompts/get renders a grounded recipe referencing real tool calls, with
  // the argument interpolated into the message.
  const blockRecipe = await request("prompts/get", { name: "author_block", arguments: { name: "SmokeBrick" } });
  assert.ok(Array.isArray(blockRecipe.result.messages), "prompts/get returns messages");
  assert.equal(blockRecipe.result.messages[0].role, "user");
  assert.match(blockRecipe.result.messages[0].content.text, /author_registry_asset/);
  assert.match(blockRecipe.result.messages[0].content.text, /SmokeBrick/);
  assert.match(blockRecipe.result.messages[0].content.text, /block/);

  // Every listed prompt resolves (no dangling templates) and mentions a real tool.
  for (const p of promptsListed.result.prompts) {
    const got = await request("prompts/get", { name: p.name, arguments: { name: "Smoke" } });
    assert.equal(got.result.messages[0].role, "user", `${p.name} renders a user message`);
    assert.ok(got.result.messages[0].content.text.length > 0, `${p.name} has non-empty text`);
  }

  // §5 item 8: the plugin prompt is grounded in the REAL project-authoring
  // surface — create_game_project's plugins arg (the only honest plugin
  // registration today). create_system grounds on the source-maintenance
  // tools (read_file/apply_text_edits/start_build). create_ui grounds on the
  // public engine/ui contracts (IUiDoc — the versioned JSON UI-composition
  // document declared in the header as the data surface for MCP tooling)
  // authored via create_file/apply_text_edits into Content/UI/.
  const pluginPrompt = await request("prompts/get", { name: "create_plugin", arguments: { name: "MyMod" } });
  const pluginText = pluginPrompt.result.messages[0].content.text;
  assert.match(pluginText, /create_game_project/);
  assert.match(pluginText, /plugins: \["MyMod"\]/);
  assert.ok(promptsListed.result.prompts.some((p) => p.name === "create_plugin"), "create_plugin listed");

  // §5 item 8 ("sistema"): create_system grounds on the REAL source-maintenance
  // surface (engine_overview + read_file + apply_text_edits + start_build).
  const systemPrompt = await request("prompts/get", { name: "create_system", arguments: { name: "ExampleSystem" } });
  const systemText = systemPrompt.result.messages[0].content.text;
  assert.match(systemText, /apply_text_edits/);
  assert.match(systemText, /read_file/);
  assert.match(systemText, /start_build/);
  assert.match(systemText, /ExampleSystem/);
  assert.ok(promptsListed.result.prompts.some((p) => p.name === "create_system"), "create_system listed");

  // §5 item 8 ("UI"): create_ui grounds on the public engine/ui contracts —
  // IUiDoc is a versioned JSON document composed of layout/widgets/viewport/
  // confirmations, explicitly declared in the header as the data surface for
  // MCP tooling. The recipe reads the contract, then authors Content/UI/<name>.json
  // via create_file/apply_text_edits and compiles with build_game.
  const uiPrompt = await request("prompts/get", { name: "create_ui", arguments: { name: "MainMenu" } });
  const uiText = uiPrompt.result.messages[0].content.text;
  assert.match(uiText, /IUiDoc\.hpp/);
  assert.match(uiText, /create_file/);
  assert.match(uiText, /Content\/UI\/MainMenu\.json/);
  assert.match(uiText, /build_game/);
  assert.ok(promptsListed.result.prompts.some((p) => p.name === "create_ui"), "create_ui listed");

  // Unknown prompt is refused with a protocol error.
  const unknownPrompt = await request("prompts/get", { name: "does_not_exist" });
  assert.equal(unknownPrompt.error.code, -32602);
  assert.match(unknownPrompt.error.message, /unknown prompt/);

  // ---- resources: documentation surface covers docs + SDK manifest ----
  const resources = await request("resources/list");
  const resourceUris = resources.result.resources.map((r) => r.uri);
  assert.ok(resourceUris.includes("engine://readme"), "resources expose readme");
  assert.ok(resourceUris.includes("engine://sdk-manifest"), "resources expose the SDK manifest");
  assert.ok(resourceUris.includes("engine://pending-work"), "resources expose pending work");
  const manifestRead = await request("resources/read", { uri: "engine://sdk-manifest" });
  assert.match(manifestRead.result.contents[0].text, /SDK/);

  // §5 item 3: the dynamic metrics resource (engine://metrics) — generated on
  // read, no backing file. Shape is deterministic per instance; uptime and
  // subscription counts move with the process.
  assert.ok(resourceUris.includes("engine://metrics"), "resources expose live metrics");
  const metricsRead = await request("resources/read", { uri: "engine://metrics" });
  const metrics = JSON.parse(metricsRead.result.contents[0].text);
  assert.equal(metrics.server, "vulkancraft-engine");
  assert.equal(metrics.protocol_version, "2025-03-26");
  assert.ok(metrics.tools.total >= metrics.tools.semantic && metrics.tools.semantic > 0, "tool counts coherent");
  assert.ok(metrics.audit_ring.max >= metrics.audit_ring.entries, "audit ring bounded");
  assert.ok(metrics.uptime_seconds >= 0, "uptime present");

  // §5 item 3 ("tests"): the dynamic tests resource lists the REGISTERED ctest
  // tests of the current build tree via ctest -N (no execution — deterministic
  // per configured tree).
  assert.ok(resourceUris.includes("engine://tests"), "resources expose the tests list");
  const testsRead = await request("resources/read", { uri: "engine://tests" });
  const testsPayload = JSON.parse(testsRead.result.contents[0].text);
  assert.equal(testsPayload.build_configured, true, "build tree is configured");
  assert.ok(testsPayload.count > 0, "ctest -N lists registered tests");
  assert.ok(Array.isArray(testsPayload.tests) && testsPayload.tests.length === testsPayload.count, "tests array matches count");
  const sortedTests = [...testsPayload.tests].sort();
  assert.deepEqual(testsPayload.tests, sortedTests, "tests sorted deterministically");

  // §5 item 3: the dynamic projects resource (engine://projects) — same
  // enumeration as list_game_projects, generated on read, deterministic
  // (sorted by name, each entry {name, managed, path}).
  assert.ok(resourceUris.includes("engine://projects"), "resources expose live projects");
  const projectsRead = await request("resources/read", { uri: "engine://projects" });
  const projects = JSON.parse(projectsRead.result.contents[0].text);
  assert.ok(Array.isArray(projects.projects), "projects resource carries the list");
  const names = projects.projects.map((p) => p.name);
  assert.deepEqual(names, [...names].sort((a, b) => a < b ? -1 : a > b ? 1 : 0), "projects sorted by name");
  for (const p of projects.projects) {
    assert.equal(typeof p.name, "string") && assert.equal(typeof p.managed, "boolean") && assert.equal(typeof p.path, "string");
  }
  // Cross-check against the tool: same enumeration, same order.
  const projectsTool = await request("tools/call", { name: "list_game_projects", arguments: {} });
  const viaTool = JSON.parse(projectsTool.result.content[0].text);
  assert.deepEqual(projects, viaTool, "projects resource == list_game_projects (single source)");
  const unknownResource = await request("resources/read", { uri: "engine://nope" });
  assert.equal(unknownResource.error.code, -32002);

  const listed = await request("tools/list");
  assert.ok(listed.result.tools.some((tool) => tool.name === "apply_text_edits"));
  assert.ok(listed.result.tools.some((tool) => tool.name === "engine_overview"));
  assert.ok(listed.result.tools.some((tool) => tool.name === "create_game_project"));
  assert.ok(listed.result.tools.some((tool) => tool.name === "set_component"));

  // ---- long operations: async build jobs (start_build / build_status /
  // cancel_build / list_build_jobs). A real build races concurrent agents,
  // so the smoke verifies the deterministic surface: registration, refusals,
  // and the (empty) job list — never a live cmake spawn.
  for (const expected of ["start_build", "build_status", "cancel_build", "list_build_jobs"]) {
    assert.ok(listed.result.tools.some((tool) => tool.name === expected), `tool registered: ${expected}`);
  }
  const noJobs = await request("tools/call", { name: "list_build_jobs", arguments: {} });
  assert.deepEqual(JSON.parse(noJobs.result.content[0].text), { jobs: [] });
  const badBuildTarget = await request("tools/call", { name: "start_build", arguments: { exe: "not_a_target" } });
  assert.equal(badBuildTarget.result.isError, true);
  assert.match(badBuildTarget.result.content[0].text, /unknown target/);
  const unknownJob = await request("tools/call", { name: "build_status", arguments: { job_id: 1 } });
  assert.equal(unknownJob.result.isError, true);
  assert.match(unknownJob.result.content[0].text, /unknown build job/);
  const cancelUnknown = await request("tools/call", { name: "cancel_build", arguments: { job_id: 1 } });
  assert.equal(cancelUnknown.result.isError, true);
  assert.match(cancelUnknown.result.content[0].text, /unknown build job/);

  // §5 item 5 ("artefatos"): a full job lifecycle with a fast-FAILING config.
  // An invalid --config makes cmake error in <1s (MSB8013) WITHOUT compiling
  // anything — no race with concurrent agents, but the job transitions
  // running → failed and build_status/list expose the artifacts shape.
  const badConfigStart = await request("tools/call", { name: "start_build", arguments: { exe: "VulkanEngineGame", config: "NopeConfig" } });
  assert.equal(badConfigStart.result.isError, undefined, JSON.stringify(badConfigStart));
  const jobStart = JSON.parse(badConfigStart.result.content[0].text);
  assert.equal(jobStart.status, "running");
  assert.match(jobStart.log, /\.log$/);
  // Budget 60s: in a concurrent tree `cmake --build` first runs ZERO_CHECK,
  // which fully reconfigures when CMakeLists.txt changed (agents edit it
  // constantly) — a full rocksdb re-configure alone takes ~27s before the
  // fast failure (MSB8013/CMP0002) is even reached. No compilation ever
  // happens for an invalid --config, so a generous budget stays safe.
  let jobFinal = null;
  let polled = null;
  for (let attempt = 0; attempt < 120; attempt++) {
    await new Promise((resolve) => setTimeout(resolve, 500));
    const poll = await request("tools/call", { name: "build_status", arguments: { job_id: jobStart.job_id } });
    polled = JSON.parse(poll.result.content[0].text);
    if (polled.status !== "running") { jobFinal = polled; break; }
  }
  assert.ok(jobFinal, `job reached a terminal state (last: ${JSON.stringify(polled ?? null)})`);
  assert.equal(jobFinal.status, "failed", JSON.stringify(jobFinal));
  // The fast-fail reason varies in a concurrent tree: the invalid --config is
  // rejected either by MSBuild (MSB8013, exit 1) or by the ZERO_CHECK
  // regeneration step when a concurrent agent's CMakeLists edit breaks
  // configure (CMP0002, exit 127) or by a signal (exit null). The invariant
  // is the terminal "failed" state with no binaries — not the exact reason.
  assert.ok(jobFinal.exit_code === null || jobFinal.exit_code !== 0,
    `non-zero/null exit (got ${jobFinal.exit_code})`);
  // §5 item 5 ("progresso granular"): the coarse stage is derived from the
  // log tail (configure/compile/link/running) — the fast-fail config never
  // compiles, so the terminal job reports configure or running.
  assert.ok(["configure", "compile", "link", "running"].includes(jobFinal.stage),
    `stage is a known value (got ${jobFinal.stage})`);
  // The invalid config fails either at configure (CMP0002 during ZERO_CHECK
  // re-generation) or at compile (MSB8013 inside MSBuild) — never at link.
  assert.ok(jobFinal.stage !== "link", `fast-fail job never reaches link (got ${jobFinal.stage})`);
  // §5 item 5 ("progresso granular"): targets_built counts completed MSBuild
  // targets (`-> exe/dll/lib` lines) — the fast-fail job completes none.
  assert.equal(typeof jobFinal.targets_built, "number") && assert.ok(Number.isInteger(jobFinal.targets_built) && jobFinal.targets_built >= 0,
    `targets_built is a non-negative integer (got ${jobFinal.targets_built})`);
  assert.equal(jobFinal.targets_built, 0, `fast-fail job builds no targets (got ${jobFinal.targets_built})`);
  // §5 item 5 ("bytes exatos"): artifacts carry bytes_total (0 for a job that
  // built nothing) and each binary entry carries its real size when present.
  assert.deepEqual(jobFinal.artifacts, { log: jobFinal.log, binaries: [], bytes_total: 0 },
    "failed job exposes log artifact, no binaries, zero bytes");
  assert.match(jobFinal.tail, /MSB8013|error|CMP0002|failed/i, "log tail carries the failure");
  const jobsAfter = await request("tools/call", { name: "list_build_jobs", arguments: {} });
  const jobsList = JSON.parse(jobsAfter.result.content[0].text);
  assert.ok(jobsList.jobs.some((job) => job.job_id === jobStart.job_id && job.status === "failed"
    && job.artifacts && Array.isArray(job.artifacts.binaries)), "list_build_jobs exposes artifacts");
  assert.ok(jobsList.jobs.some((job) => job.job_id === jobStart.job_id && ["configure", "compile", "link", "running"].includes(job.stage)),
    "list_build_jobs exposes the stage");
  // cancel of the now-terminal job is reported, not re-cancelled.
  const cancelDone = await request("tools/call", { name: "cancel_build", arguments: { job_id: jobStart.job_id } });
  const cancelPayload = JSON.parse(cancelDone.result.content[0].text);
  assert.equal(cancelPayload.cancelled, false);

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

  // §8 item 4 ("plugins"): the plugin MANIFEST surface is testable today —
  // create_game_project(plugins:) must materialize both the project manifest
  // (project.json plugins array) and Config/Plugins.ini (one enabled=true per
  // plugin). The plugin RUNTIME does not exist yet (§3), so the manifest is
  // the honest extent of plugin coverage.
  const smokeManifest = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "project.json"), "utf8"));
  assert.deepEqual(smokeManifest.plugins, ["VoxelWorld", "Vehicles"], "project.json carries the plugin list");
  const pluginsIni = fs.readFileSync(path.join(smokeProjectPath, "Config", "Plugins.ini"), "utf8");
  assert.match(pluginsIni, /VoxelWorld=true/);
  assert.match(pluginsIni, /Vehicles=true/);

  // §4 item 3 "empacotar projetos": package_game deterministic surface
  // (unknown exe / missing project — the tool was proven e2e against a real
  // build, findings #186).
  const packageUnknownExe = await request("tools/call", { name: "package_game", arguments: { project: smokeProject, exe: "not_an_exe" } });
  assert.equal(packageUnknownExe.result.isError, true);
  assert.match(packageUnknownExe.result.content[0].text, /unknown exe/);
  const packageMissingProject = await request("tools/call", { name: "package_game", arguments: { project: "NopeNever" } });
  assert.equal(packageMissingProject.result.isError, true);
  assert.match(packageMissingProject.result.content[0].text, /does not exist/);

  // §5 item 3 ("asset/cena"): per-project dynamic resources via URI templates
  // — engine://projects/<name>[/assets|/scenes], grounded in the SAME
  // inspection as inspect_game_project (single source of truth). Runs after
  // create_game_project so smokeProject exists on disk.
  const templates = await request("resources/templates/list", {});
  const templateUris = templates.result.resourceTemplates.map((t) => t.uriTemplate);
  assert.ok(templateUris.includes("engine://projects/{name}"), "resources advertise the project template");
  assert.ok(templateUris.includes("engine://projects/{name}/assets"), "assets template advertised");
  assert.ok(templateUris.includes("engine://projects/{name}/scenes"), "scenes template advertised");
  const projectRead = await request("resources/read", { uri: `engine://projects/${smokeProject}` });
  const projectDetails = JSON.parse(projectRead.result.contents[0].text);
  assert.equal(projectDetails.project, smokeProject);
  assert.ok(Array.isArray(projectDetails.scenes) && Array.isArray(projectDetails.assets), "project resource carries scenes + assets arrays");
  const scenesRead = await request("resources/read", { uri: `engine://projects/${smokeProject}/scenes` });
  const scenesOnly = JSON.parse(scenesRead.result.contents[0].text);
  assert.ok(Array.isArray(scenesOnly.scenes) && scenesOnly.scenes.some((s) => s === "Initial.scene"), "scenes resource lists Initial.scene");
  const missingProject = await request("resources/read", { uri: `engine://projects/NoSuchProject_${Date.now()}` });
  assert.equal(missingProject.error.code, -32002, "unknown project resource is refused");

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
  const scriptPayload = JSON.parse(scriptResponse.result.content[0].text);
  assert.equal(scriptPayload.runtime_loads, true, "scene companion Initial.script is auto-loaded by the demo runtime");
  assert.equal(scriptPayload.location, "scene_companion");
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Scenes", "Initial.script")));

  const deadScriptResponse = await request("tools/call", {
    name: "create_visual_script",
    arguments: {
      project: smokeProject,
      name: "DeadGraph",
      nodes: [
        { key: "start", kind: "Event" }, // Event without event: dead entry (eventEntries[""] never dispatches)
        { key: "set", kind: "SetVariable", variable: "x" }
      ],
      links: [{ from: "start", to: "set" }]
    }
  });
  assert.ok(deadScriptResponse.result.isError, "Event node without event must be refused (dead graph)");
  assert.match(deadScriptResponse.result.content[0].text, /'start'/);
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Scripts", "DeadGraph.script")));

  const libraryScriptResponse = await request("tools/call", {
    name: "create_visual_script",
    arguments: {
      project: smokeProject,
      name: "LibraryHelper",
      nodes: [
        { key: "start", kind: "Event", event: "OnStart" },
        { key: "one", kind: "ConstantFloat", literal: 2.0 },
        { key: "set", kind: "SetVariable", variable: "helper" }
      ],
      links: [{ from: "start", to: "one" }, { from: "one", to: "set" }]
    }
  });
  assert.equal(libraryScriptResponse.result.isError, undefined);
  const libraryScriptPayload = JSON.parse(libraryScriptResponse.result.content[0].text);
  assert.equal(libraryScriptPayload.location, "library");
  assert.equal(libraryScriptPayload.runtime_loads, false, "Content/Scripts assets are library-only, never auto-loaded");
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Scripts", "LibraryHelper.script")));

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

  // FALTANTES item 22 sub-1/sub-2 — SDK contract gate: every public header
  // under src/engine/public must be self-contained (engine//std/vendor only,
  // canonical engine/<domain>/<Header>.hpp includes — never short names). A
  // header with an internal include or a short include breaks this gate.
  // (No exact header count is pinned here: in a concurrent tree the count is
  // pure churn — the freshness assertions below (inventory + matrix byte-equal
  // to a fresh generation) subsume it: an added/removed header or a broken
  // walk changes the regeneration and breaks the byte-equality until the
  // committed docs are regenerated.)
  const sdkCheck = spawnSync(process.execPath, [path.join(directory, "..", "sdk", "sdk-check.mjs")], { encoding: "utf8" });
  assert.equal(sdkCheck.status, 0, sdkCheck.stdout + sdkCheck.stderr);

  // §1 item 1 + §8 item 3: the COMMITTED API inventory (docs/SDK_API_INVENTORY.md)
  // must equal a fresh generation — it is a GENERATED artifact (single source:
  // the filesystem walk), never a manual list. Regenerate with:
  //   node tools/sdk/sdk-check.mjs --write-manifest docs/SDK_API_INVENTORY.md
  const inventoryPath = path.resolve(directory, "..", "..", "docs", "SDK_API_INVENTORY.md");
  const inventoryTemp = path.join(directory, "mcp-smoke-inventory.md");
  const inventoryGen = spawnSync(process.execPath, [path.join(directory, "..", "sdk", "sdk-check.mjs"), "--write-manifest", inventoryTemp], { encoding: "utf8" });
  assert.equal(inventoryGen.status, 0, inventoryGen.stderr);
  const committedInventory = fs.readFileSync(inventoryPath, "utf8");
  const freshInventory = fs.readFileSync(inventoryTemp, "utf8");
  assert.equal(freshInventory, committedInventory, "docs/SDK_API_INVENTORY.md is fresh (regenerate: sdk-check.mjs --write-manifest docs/SDK_API_INVENTORY.md)");

  // §1 item 5 ("namespaces"): the hard invariant from bugs.md B-219-2 — no
  // namespace-less public header — is now enforced by namespace-gate.mjs, and
  // the canonical map (docs/NAMESPACE_CANONICAL.md) must equal a fresh
  // generation (single source: the same filesystem walk). Regenerate with:
  //   node tools/sdk/namespace-gate.mjs --write docs/NAMESPACE_CANONICAL.md
  const nsGate = spawnSync(process.execPath, [path.join(directory, "..", "sdk", "namespace-gate.mjs")], { encoding: "utf8" });
  assert.equal(nsGate.status, 0, nsGate.stdout + nsGate.stderr);
  const nsPath = path.resolve(directory, "..", "..", "docs", "NAMESPACE_CANONICAL.md");
  const nsTemp = path.join(directory, "mcp-smoke-namespaces.md");
  const nsGen = spawnSync(process.execPath, [path.join(directory, "..", "sdk", "namespace-gate.mjs"), "--write", nsTemp], { encoding: "utf8" });
  assert.equal(nsGen.status, 0, nsGen.stderr);
  const committedNs = fs.readFileSync(nsPath, "utf8");
  const freshNs = fs.readFileSync(nsTemp, "utf8");
  assert.equal(freshNs, committedNs, "docs/NAMESPACE_CANONICAL.md is fresh (regenerate: namespace-gate.mjs --write docs/NAMESPACE_CANONICAL.md)");
  fs.rmSync(nsTemp, { force: true });
  fs.rmSync(inventoryTemp, { force: true });

  // §8 item 1: the COMMITTED capability matrix (docs/SDK_CAPABILITY_MATRIX.md)
  // must equal a fresh generation — it is GENERATED from semanticToolDefinitions()
  // + the filesystem walk (single sources, never a manual list). Regenerate with:
  //   node tools/sdk/capability-matrix.mjs docs/SDK_CAPABILITY_MATRIX.md
  const matrixPath = path.resolve(directory, "..", "..", "docs", "SDK_CAPABILITY_MATRIX.md");
  const matrixTemp = path.join(directory, "mcp-smoke-matrix.md");
  const matrixGen = spawnSync(process.execPath, [path.join(directory, "..", "sdk", "capability-matrix.mjs"), matrixTemp], { encoding: "utf8" });
  assert.equal(matrixGen.status, 0, matrixGen.stderr);
  const committedMatrix = fs.readFileSync(matrixPath, "utf8");
  const freshMatrix = fs.readFileSync(matrixTemp, "utf8");
  assert.equal(freshMatrix, committedMatrix, "docs/SDK_CAPABILITY_MATRIX.md is fresh (regenerate: capability-matrix.mjs docs/SDK_CAPABILITY_MATRIX.md)");
  fs.rmSync(matrixTemp, { force: true });

  // §6 item 7: the COMMITTED generated docs (REFERENCE_GENERATED.md +
  // COOKBOOK_GENERATED.md) must equal a fresh generation — same single
  // sources (filesystem walk + semanticToolDefinitions() + schema/registry +
  // error registry), never manual lists. Regenerate with:
  //   node tools/portability/gen-docs.mjs
  const genDocsDir = path.resolve(directory, "..", "portability");
  const refTemp = path.join(directory, "mcp-smoke-reference.md");
  const cookTemp = path.join(directory, "mcp-smoke-cookbook.md");
  const refPath = path.resolve(directory, "..", "..", "docs", "REFERENCE_GENERATED.md");
  const cookPath = path.resolve(directory, "..", "..", "docs", "COOKBOOK_GENERATED.md");
  // Regen into temp: gen-docs writes fixed paths, so copy the committed files
  // aside, regen in place, compare, and restore. Simpler: regen in place and
  // diff against the committed bytes read BEFORE regeneration.
  const committedRef = fs.readFileSync(refPath, "utf8");
  const committedCook = fs.readFileSync(cookPath, "utf8");
  const genDocs = spawnSync(process.execPath, [path.join(genDocsDir, "gen-docs.mjs")], { encoding: "utf8" });
  assert.equal(genDocs.status, 0, genDocs.stdout + genDocs.stderr);
  const freshRef = fs.readFileSync(refPath, "utf8");
  const freshCook = fs.readFileSync(cookPath, "utf8");
  assert.equal(freshRef, committedRef, "docs/REFERENCE_GENERATED.md is fresh (regenerate: gen-docs.mjs)");
  assert.equal(freshCook, committedCook, "docs/COOKBOOK_GENERATED.md is fresh (regenerate: gen-docs.mjs)");
  void refTemp; void cookTemp;

  // §1 item 6: the SDK package config template must carry per-config selection
  // (Debug archives staged in lib/Debug/, every other config in lib/) with a
  // backward-compatible fallback for single-config prefixes. This is the
  // MSVC runtime-compatibility contract for Debug/Release consumers.
  const configTemplate = fs.readFileSync(path.resolve(directory, "..", "..", "cmake", "vulkan_craft_sdk-config.cmake.in"), "utf8");
  assert.match(configTemplate, /lib\/Debug\/vc_sdk\.lib/, "config template must stage/select Debug archives under lib/Debug/");
  assert.match(configTemplate, /IMPORTED_CONFIGURATIONS \"DEBUG;RELEASE\"/, "config template must advertise DEBUG+RELEASE imported configs");
  assert.match(configTemplate, /IMPORTED_LOCATION_DEBUG/, "config template must map the Debug consumer config to the Debug archive");
  assert.match(configTemplate, /\$<\$<CONFIG:Debug>/s, "config template must select the Debug link set per consumer config");

  // The CLI exposes the same artifacts without a running server.
  const cliPath = path.join(directory, "registry-cli.mjs");
  const engineRoot = path.resolve(directory, "..", "..");
  const runCli = (args) => spawnSync(process.execPath, [cliPath, ...args], { encoding: "utf8" });
  const cliKinds = runCli(["kinds"]);
  assert.equal(cliKinds.status, 0, cliKinds.stderr);
  // 2 vehicle kinds (§17 item 12) + 1 ability kind (§19) + 1 mission kind
  // (item 23) + 1 world profile kind (item 23 — geração procedural) + 1 gait
  // kind (item 23 — animações) + 1 simulation LOD kind (§20) + 1 prefab kind
  // (item 23 — prefabs) + 1 particle kind (item 23 — partículas) + 12 config
  // kinds (§4.4/§4.5 — shader/render_graph/light/gi/ocean/post_process/
  // fluid_sim/world/chunk/transaction/block_entity/inventory).
  assert.equal(cliKinds.stdout.split(/\n/).filter(Boolean).length, schemaKinds.length + 21, cliKinds.stdout);
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

  // §8 item 3: the COMMITTED schemas (schema/registry/) must equal a fresh
  // regeneration from the single source (REGISTRY_FIELD_SCHEMAS) — no manual
  // lists, no drift. The fresh export (schemaOutDir) contains every kind the
  // CLI generates; the committed dir must match it file-for-file and
  // byte-for-byte. If an agent adds a field/kind, this fails until
  // `node tools/mcp-server/registry-cli.mjs export-schemas schema/registry`
  // is re-run (regenerated 2026-08-26, findings #182).
  const committedSchemaDir = path.resolve(directory, "..", "..", "schema", "registry");
  const freshKinds = fs.readdirSync(schemaOutDir)
    .filter((f) => f.endsWith(".json"))
    .map((f) => f.replace(/\.json$/, ""))
    .sort();
  const committedKinds = fs.readdirSync(committedSchemaDir)
    .filter((f) => f.endsWith(".json"))
    .map((f) => f.replace(/\.json$/, ""))
    .sort();
  assert.deepEqual(committedKinds, freshKinds, "schema/registry/ covers every kind the CLI generates");
  for (const kind of freshKinds) {
    const committed = fs.readFileSync(path.join(committedSchemaDir, `${kind}.json`), "utf8");
    const fresh = fs.readFileSync(path.join(schemaOutDir, `${kind}.json`), "utf8");
    assert.equal(fresh, committed, `schema/registry/${kind}.json is fresh (regenerate: registry-cli.mjs export-schemas schema/registry)`);
  }

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

  // ---- §4 item 2 Command Bus: run_batch (all-or-nothing transaction) ----
  {
    const batchOk = await request("tools/call", { name: "run_batch", arguments: {
      project: smokeProject,
      operations: [
        { tool: "author_registry_asset", args: { kind: "item", name: "batch_smoke_ingot" } },
        { tool: "author_registry_asset", args: { kind: "block", name: "BatchSmokeBlock", hardness: 3, drops: ["vulkancraft:batch_smoke_ingot"] } }
      ]
    } });
    const batchPayload = JSON.parse(batchOk.result.content[0].text);
    assert.equal(batchOk.result.isError, undefined, JSON.stringify(batchPayload));
    assert.equal(batchPayload.committed, true, "run_batch commits a valid batch");
    assert.equal(batchPayload.applied.length, 2, "run_batch applies every operation");

    // Atomic refusal: op0 valid, op1 invalid -> nothing written (op0 absent).
    const batchRefused = await request("tools/call", { name: "run_batch", arguments: {
      project: smokeProject,
      operations: [
        { tool: "author_registry_asset", args: { kind: "block", name: "BatchNever", hardness: 2 } },
        { tool: "author_registry_asset", args: { kind: "block", name: "BatchBad", collision_shape: "octagon" } }
      ]
    } });
    const refusedPayload = JSON.parse(batchRefused.result.content[0].text);
    assert.equal(refusedPayload.refused, true, "run_batch refuses when any op fails validation");
    assert.equal(refusedPayload.operation, 1, "refusal names the failing operation");
    assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "BatchNever.json")),
      "refused batch writes NOTHING (op0 rolled back before write)");

    // Rollback on apply failure: duplicate without update -> op0 removed.
    const batchRollback = await request("tools/call", { name: "run_batch", arguments: {
      project: smokeProject,
      operations: [
        { tool: "author_registry_asset", args: { kind: "block", name: "BatchRollback", hardness: 2 } },
        { tool: "author_registry_asset", args: { kind: "block", name: "BatchRollback", hardness: 9 } }
      ]
    } });
    const rollbackPayload = JSON.parse(batchRollback.result.content[0].text);
    assert.equal(rollbackPayload.committed, false, "duplicate inside a batch fails at apply");
    assert.equal(rollbackPayload.failed_at, 1, "apply failure names the operation");
    assert.ok(rollbackPayload.rolled_back.length >= 1, "apply failure rolls back prior ops");
    assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "BatchRollback.json")),
      "rolled-back create leaves no file");

    // Batch-level dry_run: validates but writes nothing.
    const batchDry = await request("tools/call", { name: "run_batch", arguments: {
      project: smokeProject,
      dry_run: true,
      operations: [
        { tool: "author_registry_asset", args: { kind: "block", name: "BatchDryOnly", hardness: 1 } }
      ]
    } });
    const dryPayload = JSON.parse(batchDry.result.content[0].text);
    assert.equal(dryPayload.dry_run, true, "run_batch dry_run validates without writing");
    assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Registry", "block", "BatchDryOnly.json")),
      "batch dry_run writes nothing");

    // Unknown tool inside a batch is refused up front (structured).
    const batchBadTool = await request("tools/call", { name: "run_batch", arguments: {
      project: smokeProject,
      operations: [ { tool: "nope", args: {} } ]
    } });
    assert.equal(batchBadTool.result.isError, true);
    assert.match(batchBadTool.result.content[0].text, /must be one of/);
    process.stdout.write("run_batch smoke (transaction/atomicity) passed\n");
  }

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

  // §19 — abilities data-driven: author an ability through the same MCP
  // surface, mirroring AbilityDefinition::load_from_json (all-or-nothing).
  const abilityAsset = await request("tools/call", {
    name: "author_ability_asset",
    arguments: {
      project: smokeProject, name: "Grav Shift",
      cooldown_seconds: 5,
      attributes: [{ name: "level", value: 3 }],
      tags: ["movement", "ultimate"],
      cost: { resource: "mana", amount: 25 },
      conditions: [
        { kind: "ownerTag", tag: "mage" },
        { kind: "ownerAttribute", attribute: "level", min_value: 2 },
        { kind: "distance", max_distance: 40 }
      ],
      targeting: { mode: "point", range: 40, radius: 3 },
      effects: [
        { type: "telekinesis", holdOffset: [0, 2, 0], grabForce: 300, durationSeconds: 4, cast_animation: "lift" },
        { type: "periodic", intervalSeconds: 0.5, ticks: 4, subEffect: { type: "damage", amount: 4 } },
        { type: "blockEdit", min: [-2, 0, -2], max: [2, 3, 2], blockId: 7, relative: true, sound_effect: "rumble" }
      ]
    }
  });
  assert.equal(abilityAsset.result.isError, undefined, JSON.stringify(abilityAsset));
  const abilityPayload = JSON.parse(abilityAsset.result.content[0].text);
  assert.equal(abilityPayload.created, true);
  assert.equal(abilityPayload.diagnostics.length, 0, JSON.stringify(abilityPayload));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Abilities", "Grav_Shift.json")));
  const abilityDocument = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "Abilities", "Grav_Shift.json"), "utf8"));
  assert.equal(abilityDocument.cooldownSeconds, 5);
  assert.equal(abilityDocument.effects.length, 3);
  assert.equal(abilityDocument.effects[1].subEffect.type, "damage");

  // The exported JSON Schema covers the ability kind and accepts the emitted document.
  assert.ok(capabilitiesPayload.ability_schemas, "game_capabilities exposes ability_schemas");
  assert.ok(capabilitiesPayload.ability_asset_kinds, "game_capabilities exposes ability_asset_kinds");
  const abilitySchema = capabilitiesPayload.ability_schemas.ability;
  assert.equal(abilitySchema.$schema, "http://json-schema.org/draft-07/schema#");
  assert.equal(abilitySchema.type, "object");
  assert.ok(abilitySchema.properties.effects, "ability schema declares effects");
  const abilitySchemaErrors = validateJsonSchema(abilityDocument, abilitySchema);
  assert.equal(abilitySchemaErrors.length, 0, abilitySchemaErrors.join("; "));

  // §19 all-or-nothing: an unknown effect type is refused and nothing is written.
  const badAbility = await request("tools/call", {
    name: "author_ability_asset",
    arguments: {
      project: smokeProject, name: "BrokenCast",
      effects: [{ type: "mindControl" }]
    }
  });
  assert.equal(badAbility.result.isError, undefined, JSON.stringify(badAbility));
  const badAbilityPayload = JSON.parse(badAbility.result.content[0].text);
  assert.equal(badAbilityPayload.refused, true, JSON.stringify(badAbilityPayload));
  assert.ok(badAbilityPayload.diagnostics.some((d) => String(d).includes("mindControl")), JSON.stringify(badAbilityPayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Abilities", "BrokenCast.json")));

  // Abilities are listed by inspect and counted by validate_game_project.
  const inspectAbilities = await request("tools/call", {
    name: "inspect_ability_assets",
    arguments: { project: smokeProject }
  });
  const inspectAbilitiesPayload = JSON.parse(inspectAbilities.result.content[0].text);
  assert.equal(inspectAbilitiesPayload.count, 1);
  assert.ok(inspectAbilitiesPayload.ability_assets.every((asset) => asset.valid), JSON.stringify(inspectAbilitiesPayload));
  const abilitiesValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const abilitiesValidationPayload = JSON.parse(abilitiesValidation.result.content[0].text);
  assert.equal(abilitiesValidationPayload.valid, true, JSON.stringify(abilitiesValidationPayload));
  assert.ok(abilitiesValidationPayload.ability_assets >= 1);

  // The CLI exposes the ability schema too.
  const cliAbilitySchema = runCli(["schema", "ability"]);
  assert.equal(cliAbilitySchema.status, 0, cliAbilitySchema.stderr);
  assert.ok(JSON.parse(cliAbilitySchema.stdout).properties.effects, "CLI ability schema declares effects");

  // Item 23 — missions and dialogues: author a mission through the same MCP
  // surface, mirroring MissionDefinition::load_from_json (all-or-nothing).
  const missionAsset = await request("tools/call", {
    name: "author_mission_asset",
    arguments: {
      project: smokeProject, name: "First Steps",
      objectives: [
        { id: "reach_hill", kind: "reach", x: 10, z: -5, radius: 3 },
        { id: "collect_wood", kind: "collect", target: "vulkancraft:oak_log", count: 2 }
      ],
      dialogue: [
        { id: "start", speaker: "Elder", text: "Welcome, adventurer.", choices: [
          { text: "Tell me the task.", next: "task" },
          { text: "Goodbye.", next: "" }
        ]},
        { id: "task", speaker: "Elder", text: "Gather wood." }
      ],
      unlockConditions: [{ kind: "flag", key: "met_elder" }],
      reward: { itemId: "vulkancraft:torch", count: 3, xp: 50, setFlag: "first_steps_done" },
      repeatable: false
    }
  });
  assert.equal(missionAsset.result.isError, undefined, JSON.stringify(missionAsset));
  const missionPayload = JSON.parse(missionAsset.result.content[0].text);
  assert.equal(missionPayload.created, true);
  assert.equal(missionPayload.diagnostics.length, 0, JSON.stringify(missionPayload));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Missions", "First_Steps.json")));
  const missionDocument = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "Missions", "First_Steps.json"), "utf8"));
  assert.equal(missionDocument.objectives.length, 2);
  assert.equal(missionDocument.dialogue.length, 2);
  assert.equal(missionDocument.dialogue[0].choices[0].next, "task");
  assert.equal(missionDocument.reward.itemId, "vulkancraft:torch");

  // The exported JSON Schema covers the mission kind and accepts the emitted document.
  assert.ok(capabilitiesPayload.mission_schemas, "game_capabilities exposes mission_schemas");
  assert.ok(capabilitiesPayload.mission_asset_kinds, "game_capabilities exposes mission_asset_kinds");
  const missionSchema = capabilitiesPayload.mission_schemas.mission;
  assert.equal(missionSchema.$schema, "http://json-schema.org/draft-07/schema#");
  assert.equal(missionSchema.type, "object");
  assert.ok(missionSchema.properties.objectives, "mission schema declares objectives");
  const missionSchemaErrors = validateJsonSchema(missionDocument, missionSchema);
  assert.equal(missionSchemaErrors.length, 0, missionSchemaErrors.join("; "));

  // Item 23 all-or-nothing: a dialogue without a 'start' node is refused.
  const badMission = await request("tools/call", {
    name: "author_mission_asset",
    arguments: {
      project: smokeProject, name: "BrokenQuest",
      objectives: [{ id: "o1", kind: "reach", x: 0, z: 0, radius: 1 }],
      dialogue: [{ id: "a", speaker: "X", text: "hi" }]
    }
  });
  assert.equal(badMission.result.isError, undefined, JSON.stringify(badMission));
  const badMissionPayload = JSON.parse(badMission.result.content[0].text);
  assert.equal(badMissionPayload.refused, true, JSON.stringify(badMissionPayload));
  assert.ok(badMissionPayload.diagnostics.some((d) => String(d).includes("start")), JSON.stringify(badMissionPayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Missions", "BrokenQuest.json")));

  // Missions are listed by inspect and counted by validate_game_project.
  const inspectMissions = await request("tools/call", {
    name: "inspect_mission_assets",
    arguments: { project: smokeProject }
  });
  const inspectMissionsPayload = JSON.parse(inspectMissions.result.content[0].text);
  assert.equal(inspectMissionsPayload.count, 1);
  assert.ok(inspectMissionsPayload.mission_assets.every((asset) => asset.valid), JSON.stringify(inspectMissionsPayload));
  const missionsValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const missionsValidationPayload = JSON.parse(missionsValidation.result.content[0].text);
  assert.equal(missionsValidationPayload.valid, true, JSON.stringify(missionsValidationPayload));
  assert.ok(missionsValidationPayload.mission_assets >= 1);

  // The CLI exposes the mission schema too.
  const cliMissionSchema = runCli(["schema", "mission"]);
  assert.equal(cliMissionSchema.status, 0, cliMissionSchema.stderr);
  assert.ok(JSON.parse(cliMissionSchema.stdout).properties.objectives, "CLI mission schema declares objectives");

  // Item 23 — geração procedural: author a world profile through the same MCP
  // surface, mirroring the public world-profile document
  // (create_world_profile_from_json, IWorldProfile). The MCP validates the
  // top-level structure; sections are validated by their own C++ parsers.
  const profileAsset = await request("tools/call", {
    name: "author_world_profile_asset",
    arguments: {
      project: smokeProject, name: "Test World",
      height: { version: 1, seed: 2026, root: 1, nodes: [
        { type: "perlin", params: [0.05, 0.0], sources: [] },
        { type: "fbm", params: [4, 0.5, 2.0, 0.0], sources: [0] }] },
      baseHeight: 131, amplitude: 4,
      climate: { temperature: { version: 1, seed: 1, root: 0, nodes: [{ type: "constant", params: [0.7], sources: [] }] } },
      biomes: { version: 1, biomes: [{ name: "meadow", engineBiomeIndex: 7 }] },
      caves: { density: { version: 1, seed: 2, root: 0, nodes: [{ type: "constant", params: [0.0], sources: [] }] } },
      ores: { density: { version: 1, seed: 3, root: 0, nodes: [{ type: "constant", params: [0.75], sources: [] }] }, table: { version: 1, rules: [{ blockId: 18, minDensity: 0.7, maxDensity: 0.8, minY: 10, maxY: 120 }] } },
      carver: { version: 1, fluidMaxY: 60, fluidBlockId: 12 },
      decorators: { version: 1, decorators: [{ type: "column", density: 1.0, params: [2, 2], blocks: [50] }] },
      structures: { version: 1, definitions: [], rules: [] }
    }
  });
  assert.equal(profileAsset.result.isError, undefined, JSON.stringify(profileAsset));
  const profilePayload = JSON.parse(profileAsset.result.content[0].text);
  assert.equal(profilePayload.created, true);
  assert.equal(profilePayload.diagnostics.length, 0, JSON.stringify(profilePayload));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Profiles", "Test_World.json")));
  const profileDocument = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "Profiles", "Test_World.json"), "utf8"));
  assert.equal(profileDocument.version, 1);
  assert.equal(profileDocument.height.nodes.length, 2);
  assert.equal(profileDocument.baseHeight, 131);

  // The exported JSON Schema covers the world profile kind and accepts the
  // emitted document.
  assert.ok(capabilitiesPayload.world_profile_schemas, "game_capabilities exposes world_profile_schemas");
  assert.ok(capabilitiesPayload.world_profile_asset_kinds, "game_capabilities exposes world_profile_asset_kinds");
  const profileSchema = capabilitiesPayload.world_profile_schemas.world_profile;
  assert.equal(profileSchema.$schema, "http://json-schema.org/draft-07/schema#");
  assert.equal(profileSchema.type, "object");
  assert.ok(profileSchema.properties.height, "world profile schema declares height");
  const profileSchemaErrors = validateJsonSchema(profileDocument, profileSchema);
  assert.equal(profileSchemaErrors.length, 0, profileSchemaErrors.join("; "));

  // Item 23 all-or-nothing: a negative amplitude is refused.
  const badProfile = await request("tools/call", {
    name: "author_world_profile_asset",
    arguments: {
      project: smokeProject, name: "BrokenProfile",
      height: { version: 1, seed: 1, root: 0, nodes: [{ type: "constant", params: [0.5], sources: [] }] },
      baseHeight: 0, amplitude: -3
    }
  });
  assert.equal(badProfile.result.isError, undefined, JSON.stringify(badProfile));
  const badProfilePayload = JSON.parse(badProfile.result.content[0].text);
  assert.equal(badProfilePayload.refused, true, JSON.stringify(badProfilePayload));
  assert.ok(badProfilePayload.diagnostics.some((d) => String(d).includes("amplitude")), JSON.stringify(badProfilePayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Profiles", "BrokenProfile.json")));

  // World profiles are listed by inspect and counted by validate_game_project.
  const inspectProfiles = await request("tools/call", {
    name: "inspect_world_profile_assets",
    arguments: { project: smokeProject }
  });
  const inspectProfilesPayload = JSON.parse(inspectProfiles.result.content[0].text);
  assert.equal(inspectProfilesPayload.count, 1);
  assert.ok(inspectProfilesPayload.world_profiles.every((asset) => asset.valid), JSON.stringify(inspectProfilesPayload));
  const profilesValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const profilesValidationPayload = JSON.parse(profilesValidation.result.content[0].text);
  assert.equal(profilesValidationPayload.valid, true, JSON.stringify(profilesValidationPayload));
  assert.ok(profilesValidationPayload.world_profiles >= 1);

  // The CLI exposes the world profile schema too.
  const cliProfileSchema = runCli(["schema", "world_profile"]);
  assert.equal(cliProfileSchema.status, 0, cliProfileSchema.stderr);
  assert.ok(JSON.parse(cliProfileSchema.stdout).properties.height, "CLI world profile schema declares height");

  // Item 23 — animações: author a gait asset through the same MCP surface,
  // mirroring GaitAsset::load_from_json (bit-exact %.9g round-trip,
  // all-or-nothing). Creature locomotion: cycle timing + per-leg phase
  // offsets + hip-anchored two-bone leg chains.
  const gaitAsset = await request("tools/call", {
    name: "author_gait_asset",
    arguments: {
      project: smokeProject, name: "Trot",
      cycleDuration: 0.8, stanceFraction: 0.55, stepHeight: 0.3, maxStride: 0.75,
      legPhases: [0, 0.5, 0.25, 0.75],
      legs: [
        { name: "front_left", hipOffset: [0.3, 0.8, 0.5], upperLength: 0.5, lowerLength: 0.6, restOffset: [0.3, -1, 0.5], hipBone: 1, kneeBone: 2, footBone: 3 },
        { name: "front_right", hipOffset: [0.3, 0.8, -0.5], upperLength: 0.6, lowerLength: 0.6, restOffset: [0.3, -1, -0.5], hipBone: 4, kneeBone: 5, footBone: 6 },
        { name: "rear_left", hipOffset: [-0.3, 0.8, 0.5], upperLength: 0.7, lowerLength: 0.6, restOffset: [-0.3, -1, 0.5], hipBone: 7, kneeBone: 8, footBone: 9 },
        { name: "rear_right", hipOffset: [-0.3, 0.8, -0.5], upperLength: 0.8, lowerLength: 0.6, restOffset: [-0.3, -1, -0.5], hipBone: 10, kneeBone: 11, footBone: 12 }
      ]
    }
  });
  assert.equal(gaitAsset.result.isError, undefined, JSON.stringify(gaitAsset));
  const gaitPayload = JSON.parse(gaitAsset.result.content[0].text);
  assert.equal(gaitPayload.created, true);
  assert.equal(gaitPayload.diagnostics.length, 0, JSON.stringify(gaitPayload));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "Animations", "Trot.json")));
  const gaitDocument = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "Animations", "Trot.json"), "utf8"));
  assert.equal(gaitDocument.version, 1);
  assert.equal(gaitDocument.legs.length, 4);
  assert.equal(gaitDocument.legPhases.length, 4);
  assert.equal(gaitDocument.legs[3].footBone, 12);

  // The exported JSON Schema covers the gait kind and accepts the emitted
  // document.
  assert.ok(capabilitiesPayload.gait_schemas, "game_capabilities exposes gait_schemas");
  assert.ok(capabilitiesPayload.gait_asset_kinds, "game_capabilities exposes gait_asset_kinds");
  const gaitSchema = capabilitiesPayload.gait_schemas.gait;
  assert.equal(gaitSchema.$schema, "http://json-schema.org/draft-07/schema#");
  assert.equal(gaitSchema.type, "object");
  assert.ok(gaitSchema.properties.legs, "gait schema declares legs");
  const gaitSchemaErrors = validateJsonSchema(gaitDocument, gaitSchema);
  assert.equal(gaitSchemaErrors.length, 0, gaitSchemaErrors.join("; "));

  // Item 23 all-or-nothing: a stanceFraction outside (0, 1) is refused.
  const badGait = await request("tools/call", {
    name: "author_gait_asset",
    arguments: {
      project: smokeProject, name: "BrokenGait",
      stanceFraction: 1.5,
      legs: [{ name: "l" }], legPhases: [0]
    }
  });
  assert.equal(badGait.result.isError, undefined, JSON.stringify(badGait));
  const badGaitPayload = JSON.parse(badGait.result.content[0].text);
  assert.equal(badGaitPayload.refused, true, JSON.stringify(badGaitPayload));
  assert.ok(badGaitPayload.diagnostics.some((d) => String(d).includes("stanceFraction")), JSON.stringify(badGaitPayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Animations", "BrokenGait.json")));

  // Gaits are listed by inspect and counted by validate_game_project.
  const inspectGaits = await request("tools/call", {
    name: "inspect_gait_assets",
    arguments: { project: smokeProject }
  });
  const inspectGaitsPayload = JSON.parse(inspectGaits.result.content[0].text);
  assert.equal(inspectGaitsPayload.count, 1);
  assert.ok(inspectGaitsPayload.gaits.every((asset) => asset.valid), JSON.stringify(inspectGaitsPayload));
  const gaitsValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const gaitsValidationPayload = JSON.parse(gaitsValidation.result.content[0].text);
  assert.equal(gaitsValidationPayload.valid, true, JSON.stringify(gaitsValidationPayload));
  assert.ok(gaitsValidationPayload.gait_assets >= 1);

  // The CLI exposes the gait schema too.
  const cliGaitSchema = runCli(["schema", "gait"]);
  assert.equal(cliGaitSchema.status, 0, cliGaitSchema.stderr);
  assert.ok(JSON.parse(cliGaitSchema.stdout).properties.legs, "CLI gait schema declares legs");

  // FALTANTES §20 — simulation LOD specs through the same MCP surface
  // (SimulationLodSpec::load_from_json; bit-exact %.9g round-trip).
  const simLodAuthor = await request("tools/call", {
    name: "author_simulation_lod_spec",
    arguments: {
      project: smokeProject, name: "WorldBudget",
      version: 1, cellSize: 16, fullRadius: 48, falloffRadius: 320,
      dayLengthSeconds: 240, daysPerSeason: 30,
      tiers: [
        { name: "full", mode: "full", minRelevance: 0.8, maxRegions: 128 },
        { name: "coarse", mode: "coarse", minRelevance: 0.4, updateInterval: 0.25, maxRegions: 512 },
        { name: "aggregate", mode: "aggregate", minRelevance: 0.1, aggregateInterval: 0.5, maxRegions: 2048 },
        { name: "sleeping", mode: "sleeping", minRelevance: 0.0, sleepAfterIdle: 30.0, maxRegions: 8192 }
      ]
    }
  });
  assert.equal(simLodAuthor.result.isError, undefined, JSON.stringify(simLodAuthor));
  const simLodPayload = JSON.parse(simLodAuthor.result.content[0].text);
  assert.equal(simLodPayload.created, true);
  assert.equal(simLodPayload.diagnostics.length, 0, JSON.stringify(simLodPayload));
  assert.ok(fs.existsSync(path.join(smokeProjectPath, "Content", "SimulationLod", "WorldBudget.json")));
  const simLodDocument = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "SimulationLod", "WorldBudget.json"), "utf8"));
  assert.equal(simLodDocument.version, 1);
  assert.equal(simLodDocument.tiers.length, 4);
  assert.equal(simLodDocument.tiers[0].name, "full");
  assert.equal(simLodDocument.tiers[3].mode, "sleeping");

  // The exported JSON Schema covers the simulation_lod kind and accepts the
  // emitted document.
  assert.ok(capabilitiesPayload.simulation_lod_schemas, "game_capabilities exposes simulation_lod_schemas");
  assert.ok(capabilitiesPayload.simulation_lod_asset_kinds, "game_capabilities exposes simulation_lod_asset_kinds");
  const simLodSchema = capabilitiesPayload.simulation_lod_schemas.simulation_lod;
  assert.equal(simLodSchema.$schema, "http://json-schema.org/draft-07/schema#");
  assert.ok(simLodSchema.properties.tiers, "simulation_lod schema declares tiers");
  const simLodSchemaErrors = validateJsonSchema(simLodDocument, simLodSchema);
  assert.equal(simLodSchemaErrors.length, 0, simLodSchemaErrors.join("; "));

  // All-or-nothing: tiers not sorted by minRelevance descending are refused.
  const badSimLod = await request("tools/call", {
    name: "author_simulation_lod_spec",
    arguments: {
      project: smokeProject, name: "BrokenBudget",
      cellSize: 16, fullRadius: 48, falloffRadius: 320,
      dayLengthSeconds: 240, daysPerSeason: 30,
      tiers: [
        { name: "coarse", mode: "coarse", minRelevance: 0.4 },
        { name: "full", mode: "full", minRelevance: 0.8 }
      ]
    }
  });
  assert.equal(badSimLod.result.isError, undefined, JSON.stringify(badSimLod));
  const badSimLodPayload = JSON.parse(badSimLod.result.content[0].text);
  assert.equal(badSimLodPayload.refused, true, JSON.stringify(badSimLodPayload));
  assert.ok(badSimLodPayload.diagnostics.some((d) => String(d).includes("minRelevance")), JSON.stringify(badSimLodPayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "SimulationLod", "BrokenBudget.json")));

  // Specs are listed by inspect and counted by validate_game_project.
  const inspectSimLods = await request("tools/call", {
    name: "inspect_simulation_lod_specs",
    arguments: { project: smokeProject }
  });
  const inspectSimLodsPayload = JSON.parse(inspectSimLods.result.content[0].text);
  assert.equal(inspectSimLodsPayload.count, 1);
  assert.ok(inspectSimLodsPayload.simulation_lod_specs.every((asset) => asset.valid), JSON.stringify(inspectSimLodsPayload));
  const simLodsValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const simLodsValidationPayload = JSON.parse(simLodsValidation.result.content[0].text);
  assert.equal(simLodsValidationPayload.valid, true, JSON.stringify(simLodsValidationPayload));
  assert.ok(simLodsValidationPayload.simulation_lod_specs >= 1);

  // The CLI exposes the simulation_lod schema and kind too.
  const cliSimLodSchema = runCli(["schema", "simulation_lod"]);
  assert.equal(cliSimLodSchema.status, 0, cliSimLodSchema.stderr);
  assert.ok(JSON.parse(cliSimLodSchema.stdout).properties.tiers, "CLI simulation_lod schema declares tiers");

  // FALTANTES item 23 — prefabs: extract a reusable entity set from a scene,
  // instantiate it into another scene with fresh UUIDs + Hierarchy remap.
  const prefabScene = await request("tools/call", {
    name: "create_scene", arguments: { project: smokeProject, name: "PrefabSource" }
  });
  assert.equal(prefabScene.result.isError, undefined, JSON.stringify(prefabScene));
  const prefabNpc = await request("tools/call", {
    name: "create_entity", arguments: {
      project: smokeProject, scene: "PrefabSource", name: "Villager",
      transform: { px: 5, py: 0, pz: 5 },
      components: { Rigidbody: { mass: 60 }, Audio: { clip: "villager_hello" } }
    }
  });
  const prefabNpcPayload = JSON.parse(prefabNpc.result.content[0].text);
  assert.equal(prefabNpc.result.isError, undefined, JSON.stringify(prefabNpc));
  const prefabNpcId = prefabNpcPayload.entity_id;
  const prefabTorch = await request("tools/call", {
    name: "create_entity", arguments: {
      project: smokeProject, scene: "PrefabSource", name: "Lantern",
      transform: { px: 5.5, py: 1.5, pz: 5 },
      components: { Light: { r: 1, g: 0.6, b: 0.3, intensity: 800 }, Hierarchy: { parent_id: prefabNpcId } }
    }
  });
  assert.equal(prefabTorch.result.isError, undefined, JSON.stringify(prefabTorch));
  const prefabCreate = await request("tools/call", {
    name: "create_prefab", arguments: {
      project: smokeProject, scene: "PrefabSource", name: "VillagerSet",
      entity_ids: [prefabNpcId, JSON.parse(prefabTorch.result.content[0].text).entity_id],
      root_entity: "Villager"
    }
  });
  assert.equal(prefabCreate.result.isError, undefined, JSON.stringify(prefabCreate));
  const prefabCreatePayload = JSON.parse(prefabCreate.result.content[0].text);
  assert.equal(prefabCreatePayload.created, true);
  assert.equal(prefabCreatePayload.path, "Content/Prefabs/VillagerSet.prefab");
  const prefabDoc = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "Prefabs", "VillagerSet.prefab"), "utf8"));
  assert.equal(prefabDoc.format, "VulkanEngine.Prefab");
  assert.equal(prefabDoc.entities.length, 2);
  assert.ok(prefabDoc.entities.every((entity) => entity.id === undefined), "prefab strips entity ids");
  assert.equal(prefabDoc.entities.find((entity) => entity.name === "Lantern").Hierarchy.parent_id, "Villager", "internal Hierarchy remapped to entity NAME");

  // The exported JSON Schema covers the prefab kind and accepts the emitted
  // document.
  assert.ok(capabilitiesPayload.prefab_schemas, "game_capabilities exposes prefab_schemas");
  assert.ok(capabilitiesPayload.prefab_asset_kinds, "game_capabilities exposes prefab_asset_kinds");
  const prefabSchema = capabilitiesPayload.prefab_schemas.prefab;
  assert.equal(prefabSchema.$schema, "http://json-schema.org/draft-07/schema#");
  assert.ok(prefabSchema.properties.entities, "prefab schema declares entities");
  const prefabSchemaErrors = validateJsonSchema(prefabDoc, prefabSchema);
  assert.equal(prefabSchemaErrors.length, 0, prefabSchemaErrors.join("; "));

  // All-or-nothing: extracting with an unknown entity id refuses everything.
  const badPrefab = await request("tools/call", {
    name: "create_prefab", arguments: {
      project: smokeProject, scene: "PrefabSource", name: "BrokenPrefab",
      entity_ids: [prefabNpcId, "00000000-0000-0000-0000-000000000000"]
    }
  });
  assert.equal(badPrefab.result.isError, undefined, JSON.stringify(badPrefab));
  const badPrefabPayload = JSON.parse(badPrefab.result.content[0].text);
  assert.equal(badPrefabPayload.refused, true, JSON.stringify(badPrefabPayload));
  assert.ok(badPrefabPayload.diagnostics[0].includes("00000000-0000-0000-0000-000000000000"), JSON.stringify(badPrefabPayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Prefabs", "BrokenPrefab.prefab")));

  // Instantiate into a target scene: fresh UUIDs + remap + offset.
  await request("tools/call", { name: "create_scene", arguments: { project: smokeProject, name: "Village" } });
  const prefabInst = await request("tools/call", {
    name: "instantiate_prefab", arguments: {
      project: smokeProject, scene: "Village", prefab: "VillagerSet", offset: [100, 0, 200]
    }
  });
  assert.equal(prefabInst.result.isError, undefined, JSON.stringify(prefabInst));
  const prefabInstPayload = JSON.parse(prefabInst.result.content[0].text);
  assert.equal(prefabInstPayload.instantiated, 2);
  const villageDoc = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "Scenes", "Village.scene"), "utf8"));
  const villagerClone = villageDoc.entities.find((entity) => entity.name === "Villager");
  const lanternClone = villageDoc.entities.find((entity) => entity.name === "Lantern");
  assert.ok(villagerClone && lanternClone, "both clones present");
  assert.equal(villagerClone.Transform.px, 105, "offset shifts the whole set (5+100)");
  assert.equal(lanternClone.Transform.px, 105.5, "relative offset preserved (5.5+100)");
  assert.equal(lanternClone.Hierarchy.parent_id, villagerClone.id, "internal Hierarchy remapped to fresh id");

  // Prefabs are listed by inspect, counted by validate_game_project, and the
  // CLI exposes the prefab schema/kind.
  const inspectPrefabs = await request("tools/call", {
    name: "inspect_prefabs", arguments: { project: smokeProject }
  });
  const inspectPrefabsPayload = JSON.parse(inspectPrefabs.result.content[0].text);
  assert.equal(inspectPrefabsPayload.count, 1);
  assert.ok(inspectPrefabsPayload.prefabs.every((asset) => asset.valid), JSON.stringify(inspectPrefabsPayload));
  const prefabsValidation = await request("tools/call", {
    name: "validate_game_project", arguments: { project: smokeProject }
  });
  const prefabsValidationPayload = JSON.parse(prefabsValidation.result.content[0].text);
  assert.equal(prefabsValidationPayload.valid, true, JSON.stringify(prefabsValidationPayload));
  assert.ok(prefabsValidationPayload.prefabs >= 1);
  const cliPrefabSchema = runCli(["schema", "prefab"]);
  assert.equal(cliPrefabSchema.status, 0, cliPrefabSchema.stderr);
  assert.ok(JSON.parse(cliPrefabSchema.stdout).properties.entities, "CLI prefab schema declares entities");

  // FALTANTES item 23 — partículas: author a reusable emitter asset and apply
  // it to an entity's ParticleEmitter component.
  const particleAuthor = await request("tools/call", {
    name: "create_particle_asset",
    arguments: {
      project: smokeProject, name: "Fire",
      rate: 40, speedMin: 1.5, speedMax: 3.5, lifeMin: 0.8, lifeMax: 2.0,
      sizeStart: 0.15, sizeEnd: 0, cr: 1, cg: 0.55, cb: 0.2, ca: 1,
      er: 1, eg: 0.5, eb: 0.1, ea: 0.5, ay: 0.8, drag: 0.02, emitting: true
    }
  });
  assert.equal(particleAuthor.result.isError, undefined, JSON.stringify(particleAuthor));
  const particleAuthorPayload = JSON.parse(particleAuthor.result.content[0].text);
  assert.equal(particleAuthorPayload.created, true);
  assert.equal(particleAuthorPayload.path, "Content/Particles/Fire.particle");
  const particleDoc = JSON.parse(fs.readFileSync(path.join(smokeProjectPath, "Content", "Particles", "Fire.particle"), "utf8"));
  assert.equal(particleDoc.format, "VulkanEngine.Particle");
  assert.equal(particleDoc.rate, 40);
  assert.equal(particleDoc.ay, 0.8);

  // The exported JSON Schema covers the particle kind and accepts the emitted
  // document.
  assert.ok(capabilitiesPayload.particle_schemas, "game_capabilities exposes particle_schemas");
  assert.ok(capabilitiesPayload.particle_asset_kinds, "game_capabilities exposes particle_asset_kinds");
  const particleSchema = capabilitiesPayload.particle_schemas.particle;
  assert.equal(particleSchema.$schema, "http://json-schema.org/draft-07/schema#");
  assert.ok(particleSchema.properties.rate, "particle schema declares emitter fields");
  const particleSchemaErrors = validateJsonSchema(particleDoc, particleSchema);
  assert.equal(particleSchemaErrors.length, 0, particleSchemaErrors.join("; "));

  // All-or-nothing: an unknown emitter field is refused (never silently
  // dropped).
  const badParticle = await request("tools/call", {
    name: "create_particle_asset", arguments: { project: smokeProject, name: "BadParticle", rate: 10, glorp: 5 }
  });
  assert.equal(badParticle.result.isError, undefined, JSON.stringify(badParticle));
  const badParticlePayload = JSON.parse(badParticle.result.content[0].text);
  assert.equal(badParticlePayload.refused, true, JSON.stringify(badParticlePayload));
  assert.ok(badParticlePayload.diagnostics.some((d) => String(d).includes("glorp")), JSON.stringify(badParticlePayload));
  assert.ok(!fs.existsSync(path.join(smokeProjectPath, "Content", "Particles", "BadParticle.particle")));

  // Apply the asset to an entity's ParticleEmitter component.
  const particleTarget = await request("tools/call", {
    name: "create_entity", arguments: {
      project: smokeProject, scene: "PrefabSource", name: "Campfire",
      transform: { px: 0, py: 1, pz: 0 }
    }
  });
  const particleTargetPayload = JSON.parse(particleTarget.result.content[0].text);
  const particleApply = await request("tools/call", {
    name: "apply_particle_asset", arguments: {
      project: smokeProject, scene: "PrefabSource", entity_id: particleTargetPayload.entity_id, asset: "Fire"
    }
  });
  assert.equal(particleApply.result.isError, undefined, JSON.stringify(particleApply));
  const particleApplyPayload = JSON.parse(particleApply.result.content[0].text);
  assert.equal(particleApplyPayload.applied, true);
  assert.equal(particleApplyPayload.value.rate, 40);

  // Particle assets are listed by inspect, counted by validate_game_project,
  // and the CLI exposes the particle schema/kind.
  const inspectParticles = await request("tools/call", {
    name: "inspect_particle_assets", arguments: { project: smokeProject }
  });
  const inspectParticlesPayload = JSON.parse(inspectParticles.result.content[0].text);
  assert.equal(inspectParticlesPayload.count, 1);
  assert.ok(inspectParticlesPayload.particle_assets.every((asset) => asset.valid), JSON.stringify(inspectParticlesPayload));
  const particlesValidation = await request("tools/call", {
    name: "validate_game_project", arguments: { project: smokeProject }
  });
  const particlesValidationPayload = JSON.parse(particlesValidation.result.content[0].text);
  assert.equal(particlesValidationPayload.valid, true, JSON.stringify(particlesValidationPayload));
  assert.ok(particlesValidationPayload.particle_assets >= 1);
  const cliParticleSchema = runCli(["schema", "particle"]);
  assert.equal(cliParticleSchema.status, 0, cliParticleSchema.stderr);
  assert.ok(JSON.parse(cliParticleSchema.stdout).properties.rate, "CLI particle schema declares emitter fields");

  // ---- §4.4/§4.5 — rendering + voxel/world CONFIG assets (2026-08-28) ----
  // Every new author tool mirrors a public C++ contract JSON surface
  // (all-or-nothing); this gate authors one document per kind, inspects it,
  // refuses the invalid cases, and counts every category in
  // validate_game_project + inspect_game_project.
  const configAuthor = async (name, args) => {
    const result = await request("tools/call", { name, arguments: args });
    assert.equal(result.result.isError, undefined, JSON.stringify(result));
    const payload = JSON.parse(result.result.content[0].text);
    assert.equal(payload.created, true, JSON.stringify(payload));
    assert.equal(payload.diagnostics.length, 0, JSON.stringify(payload));
    return payload;
  };
  await configAuthor("create_shader_asset", { project: smokeProject, name: "SmokeFrag", source: "#version 450\nvoid main() { gl_FragColor = vec4(1.0); }", stage: "fragment", opt_level: 2, defines: ["MAX_LIGHTS=8"] });
  await configAuthor("author_render_graph", {
    project: smokeProject, name: "SmokeRG",
    resources: [
      { name: "color", kind: "image", width: 1920, height: 1080 },
      { name: "depth", kind: "image", width: 1920, height: 1080 }
    ],
    passes: [
      { name: "Geometry", resources: [{ resource: "color", access: "write", state: "color_attachment" }] },
      { name: "Lighting", resources: [{ resource: "color", access: "read", state: "shader_read" }] }
    ],
    dependencies: [{ before: "Geometry", after: "Lighting" }]
  });
  await configAuthor("create_light_asset", { project: smokeProject, name: "SmokeSun", type: "directional", color: [1, 0.95, 0.85], intensity: 10000 });
  await configAuthor("author_gi_config", { project: smokeProject, name: "SmokeGI", cascade_count: 4, bounces: 3 });
  await configAuthor("author_ocean_config", { project: smokeProject, name: "SmokeOcean", size: 128, wind_speed: 22 });
  await configAuthor("author_post_processing", { project: smokeProject, name: "SmokePost", operator: "filmic", quality: "cinematic" });
  await configAuthor("author_fluid_simulation", { project: smokeProject, name: "SmokeFluid", grid_size: 128 });
  await configAuthor("author_world_asset", { project: smokeProject, name: "SmokeWorld", seed: 42, profile: "SmokeProfile", rules_json: "{\"day\":600}" });
  await configAuthor("author_chunk_config", { project: smokeProject, name: "SmokeChunk", chunk_budget: 24, memory_budget_bytes: 104857600 });
  await configAuthor("author_block_transaction", {
    project: smokeProject, name: "SmokeTx", max_edits: 10, max_box_volume: 27,
    edits: [
      { position: [10, 64, 10], block_id: 0 },
      { position: [11, 64, 10], block_id: 0 },
      { position: [12, 64, 10], block_id: 3 }
    ]
  });
  await configAuthor("author_block_entity", { project: smokeProject, name: "SmokeFurnace", type_id: "vulkancraft:furnace", components: [{ type: "inventory", version: 1, blob: "{}" }] });
  await configAuthor("author_inventory", {
    project: smokeProject, name: "SmokeChest",
    slots: [{ item: "vulkancraft:iron_ingot", count: 32 }, null],
    filters: [{ slot: 0, allow_items: ["vulkancraft:iron_ingot"] }]
  });

  // Refusal cases: render-graph cycle, ocean size not a power of two,
  // transaction exceeding max_box_volume, block entity with non-namespaced id.
  const configRefused = async (name, args) => {
    const result = await request("tools/call", { name, arguments: args });
    const payload = JSON.parse(result.result.content[0].text);
    assert.equal(payload.refused, true, JSON.stringify(payload));
    assert.ok(payload.diagnostics.length > 0, JSON.stringify(payload));
  };
  await configRefused("author_render_graph", {
    project: smokeProject, name: "SmokeBadRG",
    resources: [{ name: "r", kind: "buffer" }],
    passes: [{ name: "A", resources: [] }, { name: "B", resources: [] }],
    dependencies: [{ before: "A", after: "B" }, { before: "B", after: "A" }]
  });
  await configRefused("author_ocean_config", { project: smokeProject, name: "SmokeBadOcean", size: 100 });
  await configRefused("author_block_transaction", {
    project: smokeProject, name: "SmokeBadTx", max_box_volume: 4,
    edits: [{ position: [0, 0, 0], block_id: 1 }, { position: [10, 0, 0], block_id: 1 }]
  });
  await configRefused("author_block_entity", { project: smokeProject, name: "SmokeBadBE", type_id: "furnace" });

  // Every inspect_* lists the authored asset as valid.
  const configInspects = [
    ["inspect_shader_assets", "shaders"], ["inspect_render_graphs", "render_graphs"],
    ["inspect_light_assets", "lights"], ["inspect_gi_configs", "gi_configs"],
    ["inspect_ocean_configs", "ocean_configs"], ["inspect_post_processings", "post_processings"],
    ["inspect_fluid_simulations", "fluid_simulations"], ["inspect_world_assets", "worlds"],
    ["inspect_chunk_configs", "chunk_configs"], ["inspect_block_transactions", "block_transactions"],
    ["inspect_block_entities", "block_entities"], ["inspect_inventories", "inventories"]
  ];
  for (const [tool, key] of configInspects) {
    const result = await request("tools/call", { name: tool, arguments: { project: smokeProject } });
    const payload = JSON.parse(result.result.content[0].text);
    assert.equal(payload.count, 1, `${tool} count (${JSON.stringify(payload)})`);
    assert.ok(payload[key].every((asset) => asset.valid), `${tool} all valid`);
  }

  // CLI exposes the config schemas and validates documents (registry-cli).
  for (const kind of ["shader", "render_graph", "gi", "ocean", "post_process", "fluid_sim", "world", "chunk", "transaction", "block_entity", "inventory"]) {
    const schema = runCli(["schema", kind]);
    assert.equal(schema.status, 0, `${kind} schema: ${schema.stderr}`);
    assert.ok(JSON.parse(schema.stdout).properties.name, `${kind} schema declares name`);
  }
  const goodShaderDoc = path.join(directory, "mcp-smoke-good-shader.json");
  const badShaderDoc = path.join(directory, "mcp-smoke-bad-shader.json");
  fs.writeFileSync(goodShaderDoc, JSON.stringify({ version: 1, name: "CliShader", source: "#version 450\nvoid main(){}", stage: "compute" }));
  fs.writeFileSync(badShaderDoc, JSON.stringify({ version: 1, name: "CliShader", source: "", stage: "vertex" }));
  assert.equal(runCli(["validate", "shader", goodShaderDoc]).status, 0, "CLI validates a good shader doc");
  assert.equal(runCli(["validate", "shader", badShaderDoc]).status, 1, "CLI rejects an empty-source shader doc");

  // Item 23 final checkbox — "criar um jogo completo usando somente MCP + APIs
  // públicas": the accumulated smokeProject IS that game (registry content,
  // vehicle, ability, mission, world profile, gait, scene + entities +
  // components, material, audio event, physics material, visual script).
  // One validate_game_project must come out CLEAN with every category
  // counted together, and inspect_game_project lists the whole stack.
  const completeGameValidation = await request("tools/call", {
    name: "validate_game_project",
    arguments: { project: smokeProject }
  });
  const completeGame = JSON.parse(completeGameValidation.result.content[0].text);
  assert.equal(completeGame.valid, true, JSON.stringify(completeGame));
  assert.ok(completeGame.registry_assets >= 9, `registry_assets >= 9 (${completeGame.registry_assets})`);
  assert.ok(completeGame.vehicle_assets >= 2, `vehicle_assets >= 2 (${completeGame.vehicle_assets})`);
  assert.ok(completeGame.ability_assets >= 1, `ability_assets >= 1 (${completeGame.ability_assets})`);
  assert.ok(completeGame.mission_assets >= 1, `mission_assets >= 1 (${completeGame.mission_assets})`);
  assert.ok(completeGame.world_profiles >= 1, `world_profiles >= 1 (${completeGame.world_profiles})`);
  assert.ok(completeGame.gait_assets >= 1, `gait_assets >= 1 (${completeGame.gait_assets})`);
  assert.ok(completeGame.simulation_lod_specs >= 1, `simulation_lod_specs >= 1 (${completeGame.simulation_lod_specs})`);
  assert.ok(completeGame.prefabs >= 1, `prefabs >= 1 (${completeGame.prefabs})`);
  assert.ok(completeGame.particle_assets >= 1, `particle_assets >= 1 (${completeGame.particle_assets})`);
  assert.ok(completeGame.shaders >= 1, `shaders >= 1 (${completeGame.shaders})`);
  assert.ok(completeGame.render_graphs >= 1, `render_graphs >= 1 (${completeGame.render_graphs})`);
  assert.ok(completeGame.lights >= 1, `lights >= 1 (${completeGame.lights})`);
  assert.ok(completeGame.gi_configs >= 1, `gi_configs >= 1 (${completeGame.gi_configs})`);
  assert.ok(completeGame.ocean_configs >= 1, `ocean_configs >= 1 (${completeGame.ocean_configs})`);
  assert.ok(completeGame.post_processings >= 1, `post_processings >= 1 (${completeGame.post_processings})`);
  assert.ok(completeGame.fluid_simulations >= 1, `fluid_simulations >= 1 (${completeGame.fluid_simulations})`);
  assert.ok(completeGame.worlds >= 1, `worlds >= 1 (${completeGame.worlds})`);
  assert.ok(completeGame.chunk_configs >= 1, `chunk_configs >= 1 (${completeGame.chunk_configs})`);
  assert.ok(completeGame.block_transactions >= 1, `block_transactions >= 1 (${completeGame.block_transactions})`);
  assert.ok(completeGame.block_entities >= 1, `block_entities >= 1 (${completeGame.block_entities})`);
  assert.ok(completeGame.inventories >= 1, `inventories >= 1 (${completeGame.inventories})`);
  assert.ok(completeGame.scenes >= 1, `scenes >= 1 (${completeGame.scenes})`);
  const completeGameInspect = await request("tools/call", {
    name: "inspect_game_project",
    arguments: { project: smokeProject }
  });
  const completeGameInspectPayload = JSON.parse(completeGameInspect.result.content[0].text);
  assert.ok(completeGameInspectPayload.vehicles.length >= 2, "inspect lists vehicles");
  assert.ok(completeGameInspectPayload.abilities.length >= 1, "inspect lists abilities");
  assert.ok(completeGameInspectPayload.missions.length >= 1, "inspect lists missions");
  assert.ok(completeGameInspectPayload.world_profiles.length >= 1, "inspect lists world profiles");
  assert.ok(completeGameInspectPayload.gaits.length >= 1, "inspect lists gaits");
  assert.ok(completeGameInspectPayload.simulation_lod_specs.length >= 1, "inspect lists simulation LOD specs");
  assert.ok(completeGameInspectPayload.prefabs.length >= 1, "inspect lists prefabs");
  assert.ok(completeGameInspectPayload.particle_assets.length >= 1, "inspect lists particle assets");
  assert.ok(completeGameInspectPayload.shaders.length >= 1, "inspect lists shaders");
  assert.ok(completeGameInspectPayload.render_graphs.length >= 1, "inspect lists render graphs");
  assert.ok(completeGameInspectPayload.lights.length >= 1, "inspect lists lights");
  assert.ok(completeGameInspectPayload.gi_configs.length >= 1, "inspect lists GI configs");
  assert.ok(completeGameInspectPayload.ocean_configs.length >= 1, "inspect lists ocean configs");
  assert.ok(completeGameInspectPayload.post_processings.length >= 1, "inspect lists post processings");
  assert.ok(completeGameInspectPayload.fluid_simulations.length >= 1, "inspect lists fluid simulations");
  assert.ok(completeGameInspectPayload.worlds.length >= 1, "inspect lists worlds");
  assert.ok(completeGameInspectPayload.chunk_configs.length >= 1, "inspect lists chunk configs");
  assert.ok(completeGameInspectPayload.block_transactions.length >= 1, "inspect lists block transactions");
  assert.ok(completeGameInspectPayload.block_entities.length >= 1, "inspect lists block entities");
  assert.ok(completeGameInspectPayload.inventories.length >= 1, "inspect lists inventories");
  assert.ok(completeGameInspectPayload.scenes.length >= 1, "inspect lists scenes");
  assert.equal(completeGameInspectPayload.validation.valid, true, JSON.stringify(completeGameInspectPayload.validation));

  // ---- Control API (editor_*): drive the RUNNING editor via :8321 ----

  // All 69 control tools are registered (count revalidated — findings #112:
  // README "66" -> 69 control tools).
  const controlTools = listed.result.tools.filter((t) => t.name.startsWith("editor_"));
  assert.equal(controlTools.length, 92, `expected 92 editor_* tools, got ${controlTools.length}`);
  for (const expected of ["editor_status", "editor_play", "editor_step", "editor_add_entity",
    "editor_set_gizmo", "editor_set_snap", "editor_voxel_paint", "editor_run_self_test",
    "editor_spawn_character", "editor_layer_set", "editor_decal_add", "editor_hair_add",
    "editor_softbody_add", "editor_envprobe_capture", "editor_paint_mode",
    "editor_video_add_frame", "editor_gaussian_add", "editor_expression_add"]) {
    assert.ok(controlTools.some((t) => t.name === expected), `missing tool ${expected}`);
  }

  // Client-side validation refuses invalid enum values without touching HTTP.
  const badEntity = await request("tools/call", {
    name: "editor_add_entity", arguments: { type: "unicorn" }
  });
  assert.equal(badEntity.result.isError, true, JSON.stringify(badEntity));
  assert.match(badEntity.result.content[0].text, /one of/);

  // Live editor checks are skipped gracefully when the editor is not running,
  // so the smoke test still passes in CI/headless environments.
  const status = await request("tools/call", { name: "editor_status", arguments: {} });
  if (status.result.isError) {
    assert.match(status.result.content[0].text, /127.0.0.1:8321/);
    process.stdout.write("Control API smoke (skipped: editor not running)\n");
  } else {
    const payload = JSON.parse(status.result.content[0].text);
    assert.equal(payload.tool, "editor_status");
    assert.ok(payload.result.state, "editor_status returns live state");
    const play = await request("tools/call", { name: "editor_play", arguments: {} });
    assert.equal(play.result.isError, undefined, JSON.stringify(play));
    const pause = await request("tools/call", { name: "editor_pause", arguments: {} });
    assert.equal(pause.result.isError, undefined, JSON.stringify(pause));
    const step = await request("tools/call", { name: "editor_step", arguments: {} });
    assert.equal(step.result.isError, undefined, JSON.stringify(step));
    const stop = await request("tools/call", { name: "editor_stop", arguments: {} });
    assert.equal(stop.result.isError, undefined, JSON.stringify(stop));
    const gizmo = await request("tools/call", { name: "editor_set_gizmo", arguments: { mode: "move" } });
    assert.equal(gizmo.result.isError, undefined, JSON.stringify(gizmo));
    process.stdout.write("Control API smoke (live editor verified)\n");
  }

  // ---- limits + audit: audit_log records every tools/call with args keys,
  // error status, timestamp and result hash; rate limit is generous (not hit).
  assert.ok(listed.result.tools.some((tool) => tool.name === "audit_log"), "tool registered: audit_log");
  const audit = await request("tools/call", { name: "audit_log", arguments: { limit: 200 } });
  const auditPayload = JSON.parse(audit.result.content[0].text);
  assert.ok(auditPayload.entries.length > 0, "audit_log has entries from this smoke run");
  assert.ok(auditPayload.count > 0, "audit_log count > 0");
  assert.ok(auditPayload.total_recorded >= auditPayload.count, "total_recorded >= count");
  const first = auditPayload.entries[0];
  assert.ok(first.ts, "audit entry has timestamp");
  assert.ok(typeof first.tool === "string", "audit entry has tool name");
  assert.ok(Array.isArray(first.args), "audit entry args are keys (never values)");
  assert.equal(typeof first.is_error, "boolean", "audit entry has is_error");
  assert.match(first.result_sha256, /^[a-f0-9]{64}$/, "audit entry has result SHA-256");
  assert.ok(auditPayload.rate.per_second >= 1, "rate limit is configured");

  // ---- subscriptions/events: subscribe_events / unsubscribe_events /
  // list_event_topics. Firing a real build races concurrent agents, so the
  // smoke verifies the deterministic surface: registration, topic list,
  // subscribe/unsubscribe lifecycle, and refusals — never a live cmake spawn.
  for (const expected of ["subscribe_events", "unsubscribe_events", "list_event_topics"]) {
    assert.ok(listed.result.tools.some((tool) => tool.name === expected), `tool registered: ${expected}`);
  }
  const topics = await request("tools/call", { name: "list_event_topics", arguments: {} });
  assert.deepEqual(JSON.parse(topics.result.content[0].text), {
    topics: ["asset.changed", "build.status_changed", "game.status_changed", "profiler.sampled", "scene.changed", "test.status_changed"]
  });
  const sub = await request("tools/call", { name: "subscribe_events", arguments: { kinds: ["build.status_changed"] } });
  const subPayload = JSON.parse(sub.result.content[0].text);
  assert.ok(subPayload.subscription_id >= 1, "subscribe returns an id");
  assert.deepEqual(subPayload.kinds, ["build.status_changed"]);
  const unsub = await request("tools/call", { name: "unsubscribe_events", arguments: { subscription_id: subPayload.subscription_id } });
  const unsubPayload = JSON.parse(unsub.result.content[0].text);
  assert.equal(unsubPayload.removed, true, "unsubscribe removes the subscription");
  const unsubAgain = await request("tools/call", { name: "unsubscribe_events", arguments: { subscription_id: subPayload.subscription_id } });
  assert.equal(JSON.parse(unsubAgain.result.content[0].text).removed, false, "re-unsubscribe is idempotent (removed=false)");
  const badSub = await request("tools/call", { name: "subscribe_events", arguments: { kinds: ["nope"] } });
  assert.equal(badSub.result.isError, true);
  assert.match(badSub.result.content[0].text, /unknown event kind/);
  const emptySub = await request("tools/call", { name: "subscribe_events", arguments: { kinds: [] } });
  assert.equal(emptySub.result.isError, true);

  // ---- lifecycle: initialize -> shutdown -> exit terminates the server ----
  {
    const lifecycle = spawn(process.execPath, [path.join(directory, "server.mjs")], {
      cwd: directory,
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true
    });
    let lbuf = "";
    let lPending = null;
    lifecycle.stdout.setEncoding("utf8");
    lifecycle.stdout.on("data", (chunk) => {
      lbuf += chunk;
      let nl;
      while ((nl = lbuf.indexOf("\n")) >= 0) {
        const line = lbuf.slice(0, nl).trim();
        lbuf = lbuf.slice(nl + 1);
        if (!line) continue;
        const msg = JSON.parse(line);
        if (lPending) { lPending(msg); lPending = null; }
      }
    });
    const lRequest = (method, params = {}) => new Promise((resolve) => {
      lPending = resolve;
      lifecycle.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id: 1, method, params })}\n`);
    });
    const exited = new Promise((resolve) => lifecycle.on("exit", resolve));
    const init = await lRequest("initialize", { protocolVersion: "2025-03-26", capabilities: {} });
    assert.equal(init.result.serverInfo.name, "vulkancraft-engine");
    const shutdown = await lRequest("shutdown", {});
    assert.equal(shutdown.error, undefined, JSON.stringify(shutdown));
    // notifications/exit is a notification (no response); it must terminate the server.
    lifecycle.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", method: "notifications/exit" })}\n`);
    const code = await Promise.race([exited, new Promise((resolve) => setTimeout(() => resolve("timeout"), 5000))]);
    assert.equal(code, 0, `server exited cleanly after shutdown+exit (got ${code})`);
    lifecycle.stdin.end();
    lifecycle.kill();
    process.stdout.write("Lifecycle smoke (shutdown+exit) passed\n");
  }

  // ---- §5 item 1 ("retry em framing parcial"): Content-Length framing must
  // HOLD a partial frame (wait for the declared byte count — no premature
  // response, no crash) and dispatch only when the frame is complete. ----
  {
    const framed = spawn(process.execPath, [path.join(directory, "server.mjs")], {
      cwd: directory,
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true
    });
    let fbuf = "";
    let fPending = null;
    let fResponses = [];
    framed.stdout.setEncoding("utf8");
    framed.stdout.on("data", (chunk) => {
      fbuf += chunk;
      let nl;
      while ((nl = fbuf.indexOf("\n")) >= 0) {
        const line = fbuf.slice(0, nl).trim();
        fbuf = fbuf.slice(nl + 1);
        if (!line) continue;
        const msg = JSON.parse(line);
        fResponses.push(msg);
        if (fPending) { fPending(msg); fPending = null; }
      }
    });
    const fNext = () => new Promise((resolve) => { fPending = resolve; });
    const fExited = new Promise((resolve) => framed.on("exit", resolve));

    // 1. Partial Content-Length frame: header + HALF the JSON body. The server
    //    must not respond and must not crash — it waits for the remaining bytes.
    const partialBody = JSON.stringify({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-03-26", capabilities: {} } });
    const head = `Content-Length: ${Buffer.byteLength(partialBody, "utf8")}\r\n\r\n`;
    const cut = Math.floor(partialBody.length / 2);
    framed.stdin.write(head + partialBody.slice(0, cut));
    const premature = await Promise.race([fNext(), new Promise((resolve) => setTimeout(() => resolve("no-response"), 400))]);
    assert.equal(premature, "no-response", "partial Content-Length frame must be held (no premature response)");
    assert.equal(fResponses.length, 0, "no response before the frame completes");

    // 2. Complete the frame — the held request dispatches now.
    const completed = fNext();
    framed.stdin.write(partialBody.slice(cut));
    const initMsg = await Promise.race([completed, new Promise((resolve) => setTimeout(() => resolve("timeout"), 5000))]);
    assert.notEqual(initMsg, "timeout", "completed Content-Length frame dispatches");
    assert.equal(initMsg.result.serverInfo.name, "vulkancraft-engine", JSON.stringify(initMsg));

    // 3. A second full frame in the same connection works (the Content-Length
    //    path, not the newline fallback) — ping round-trips.
    const pingFrame = JSON.stringify({ jsonrpc: "2.0", id: 2, method: "tools/list", params: {} });
    const pingPromise = fNext();
    framed.stdin.write(`Content-Length: ${Buffer.byteLength(pingFrame, "utf8")}\r\n\r\n${pingFrame}`);
    const toolsMsg = await Promise.race([pingPromise, new Promise((resolve) => setTimeout(() => resolve("timeout"), 5000))]);
    assert.notEqual(toolsMsg, "timeout", "second full Content-Length frame dispatches");
    assert.ok(Array.isArray(toolsMsg.result.tools) && toolsMsg.result.tools.length > 0, "tools/list via Content-Length framing");

    // 4. Truncated frame at EOF: the server must not hang forever — it exits
    //    (stdin end), which is the documented behavior.
    framed.stdin.write(`Content-Length: 999999\r\n\r\n{"jsonrpc":"2.0",`);
    framed.stdin.end();
    const exitCode = await Promise.race([fExited, new Promise((resolve) => setTimeout(() => resolve("timeout"), 5000))]);
    assert.notEqual(exitCode, "timeout", "server exits (not hang) when stdin ends mid-frame");
    framed.kill();
    process.stdout.write("Framing smoke (Content-Length partial hold + dispatch + EOF) passed\n");
  }

  // ---- optional remote transport (FALTANTES item 5): HTTP + SSE + concurrent clients ----
  {
    const httpPort = 8323 + (Date.now() % 1000);
    const httpChild = spawn(process.execPath, [path.join(directory, "server.mjs"), "--http", "--port", String(httpPort)], {
      cwd: directory,
      stdio: ["ignore", "pipe", "pipe"],
      windowsHide: true
    });
    let httpStderr = "";
    httpChild.stderr.setEncoding("utf8");
    httpChild.stderr.on("data", (c) => { httpStderr += c; });

    const httpPost = (body) => new Promise((resolve, reject) => {
      const payload = JSON.stringify(body);
      const req = http.request(
        { host: "127.0.0.1", port: httpPort, path: "/mcp", method: "POST",
          headers: { "content-type": "application/json", "content-length": Buffer.byteLength(payload) } },
        (res) => {
          let data = "";
          res.setEncoding("utf8");
          res.on("data", (c) => { data += c; });
          res.on("end", () => { try { resolve(JSON.parse(data)); } catch (e) { reject(e); } });
        });
      req.on("error", reject);
      req.end(payload);
    });

    let ready = false;
    const deadline = Date.now() + 10000;
    while (!ready && Date.now() < deadline) {
      try {
        await httpPost({ jsonrpc: "2.0", id: 1, method: "ping", params: {} });
        ready = true;
      } catch {
        await new Promise((r) => setTimeout(r, 100));
      }
    }
    assert.ok(ready, `HTTP server did not become ready; stderr=${httpStderr}`);

    const init = await httpPost({ jsonrpc: "2.0", id: 2, method: "initialize", params: { protocolVersion: "2025-03-26", capabilities: {}, clientInfo: { name: "http-smoke", version: "1" } } });
    assert.equal(init.result.serverInfo.name, "vulkancraft-engine");
    assert.ok(init.result.capabilities.prompts, "HTTP initialize advertises capabilities");

    const badInit = await httpPost({ jsonrpc: "2.0", id: 3, method: "initialize", params: { protocolVersion: "2099-01-01", capabilities: {} } });
    assert.equal(badInit.error.code, -32602, "HTTP initialize refuses an unsupported protocol version");

    const pingBurst = await Promise.all(
      Array.from({ length: 20 }, (_, i) => httpPost({ jsonrpc: "2.0", id: 100 + i, method: "ping", params: {} }))
    );
    for (const r of pingBurst) assert.deepEqual(r.result, {}, "concurrent HTTP clients resolve independently");

    const sse = await new Promise((resolve, reject) => {
      const req = http.request({ host: "127.0.0.1", port: httpPort, path: "/events", method: "GET",
        headers: { accept: "text/event-stream" } }, (res) => resolve(res));
      req.on("error", reject);
      req.end();
    });
    assert.equal(sse.statusCode, 200, "SSE endpoint returns 200");
    let sseBuf = "";
    sse.setEncoding("utf8");
    sse.on("data", (c) => { sseBuf += c; });
    await new Promise((r) => setTimeout(r, 150));
    assert.match(sseBuf, /connected/, "SSE stream sends a connect ack");

    const sub = await httpPost({ jsonrpc: "2.0", id: 4, method: "tools/call", params: { name: "subscribe_events", arguments: { kinds: ["build.status_changed"] } } });
    assert.equal(sub.result.isError, undefined, "subscribe_events works over HTTP");

    sse.destroy();
    const exitInfo = new Promise((resolve) => httpChild.on("exit", (code, signal) => resolve({ code, signal })));
    httpChild.kill("SIGTERM");
    const stopResult = await Promise.race([exitInfo, new Promise((r) => setTimeout(() => r("timeout"), 5000))]);
    assert.notEqual(stopResult, "timeout", "HTTP server stops after SIGTERM");
    // POSIX runs the graceful handler (exit 0); on Windows child.kill is a forced
    // termination (signal SIGTERM, code null). Both stop the server.
    assert.ok(
      stopResult && (stopResult.code === 0 || stopResult.signal === "SIGTERM"),
      `HTTP server stopped after SIGTERM (got ${JSON.stringify(stopResult)})`
    );
    process.stdout.write("Remote transport smoke (HTTP + SSE + concurrency) passed\n");

    // ---- §5 item 7: optional token auth on the HTTP transport. Off by
    // default (the server above has no MCP_AUTH_TOKEN and served unauthenticated
    // — the deliberate local default); with MCP_AUTH_TOKEN set, POST /mcp and
    // GET /events require `Authorization: Bearer <token>` (HTTP 401 otherwise).
    {
      const authPort = 8423 + (Date.now() % 1000);
      const authChild = spawn(process.execPath, [path.join(directory, "server.mjs"), "--http", "--port", String(authPort)], {
        cwd: directory,
        stdio: ["ignore", "pipe", "pipe"],
        windowsHide: true,
        env: { ...process.env, MCP_AUTH_TOKEN: "secret-token" }
      });
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("auth server not ready")), 8000);
        authChild.stderr.setEncoding("utf8");
        authChild.stderr.on("data", (c) => { if (String(c).includes("ready")) { clearTimeout(timer); resolve(); } });
      });
      const authPost = (body, token) => new Promise((resolve, reject) => {
        const payload = JSON.stringify(body);
        const headers = { "content-type": "application/json", "content-length": Buffer.byteLength(payload) };
        if (token) headers.authorization = `Bearer ${token}`;
        const req = http.request({ host: "127.0.0.1", port: authPort, path: "/mcp", method: "POST", headers }, (res) => {
          let buf = "";
          res.setEncoding("utf8");
          res.on("data", (c) => { buf += c; });
          res.on("end", () => resolve({ status: res.statusCode, body: buf }));
        });
        req.on("error", reject);
        req.end(payload);
      });
      const denied = await authPost({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} });
      assert.equal(denied.status, 401, "token auth rejects requests without a token");
      assert.equal(JSON.parse(denied.body).error.code, -32001, "401 carries a JSON-RPC server error");
      const wrong = await authPost({ jsonrpc: "2.0", id: 2, method: "initialize", params: {} }, "nope");
      assert.equal(wrong.status, 401, "token auth rejects a wrong token");
      const ok = await authPost({ jsonrpc: "2.0", id: 3, method: "initialize", params: { protocolVersion: "2025-03-26", capabilities: {}, clientInfo: { name: "auth-smoke", version: "1" } } }, "secret-token");
      assert.equal(ok.status, 200, "token auth accepts the right token");
      assert.equal(JSON.parse(ok.body).result.serverInfo.name, "vulkancraft-engine");
      const sseDenied = await new Promise((resolve, reject) => {
        const req = http.request({ host: "127.0.0.1", port: authPort, path: "/events", method: "GET",
          headers: { accept: "text/event-stream" } }, (res) => resolve(res.statusCode));
        req.on("error", reject);
        req.end();
      });
      assert.equal(sseDenied, 401, "SSE endpoint requires the token when auth is enabled");
      authChild.kill("SIGTERM");
      process.stdout.write("Token auth smoke (MCP_AUTH_TOKEN) passed\n");
    }
  }

  // ---- Semantic Engine API CLI (§4/§8): the full 36-tool semantic surface
  // reachable dependency-free via the same factories the MCP server uses.
  {
    const tools = spawnSync(process.execPath, [path.join(directory, "semantic-cli.mjs"), "tools"], { encoding: "utf8" });
    assert.equal(tools.status, 0, tools.stderr);
    const toolLines = tools.stdout.trim().split(/\r?\n/);
    assert.ok(toolLines.length >= 36, `semantic CLI exposes the full tool surface (got ${toolLines.length})`);
    assert.ok(toolLines.some((l) => l.startsWith("create_game_project\t")), "tools lists create_game_project");
    assert.ok(toolLines.some((l) => l.startsWith("author_ability_asset\t")), "tools lists author_ability_asset");

    const schema = spawnSync(process.execPath, [path.join(directory, "semantic-cli.mjs"), "schema", "create_scene"], { encoding: "utf8" });
    assert.equal(schema.status, 0, schema.stderr);
    const sceneSchema = JSON.parse(schema.stdout);
    assert.deepEqual(sceneSchema.required, ["project", "name"], "schema exposes required args");

    const badSchema = spawnSync(process.execPath, [path.join(directory, "semantic-cli.mjs"), "schema", "nope"], { encoding: "utf8" });
    assert.equal(badSchema.status, 2, "unknown tool schema exits 2");
    assert.match(badSchema.stderr, /unknown semantic tool/);

    const badCall = spawnSync(process.execPath, [path.join(directory, "semantic-cli.mjs"), "call", "create_game_project", '{"name":"Bad/Name!"}'], { encoding: "utf8" });
    assert.equal(badCall.status, 2, "isError result exits 2");
    assert.match(badCall.stdout, /isError.*true/s);

    const unknown = spawnSync(process.execPath, [path.join(directory, "semantic-cli.mjs"), "call", "nope", "{}"], { encoding: "utf8" });
    assert.equal(unknown.status, 3, "unknown tool call exits 3");
    process.stdout.write("Semantic CLI smoke (37 tools, dependency-free) passed\n");
  }

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
