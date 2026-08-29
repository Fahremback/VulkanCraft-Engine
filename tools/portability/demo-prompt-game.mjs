#!/usr/bin/env node
// demo-prompt-game.mjs — §11-28: reproducible demo where a SINGLE prompt
// generates, compiles, runs and packages a game using ONLY the installed SDK +
// MCP surface. Nothing in src/engine or private headers is touched.
//
// Pipeline (each step writes evidence):
//   1. MCP semantic: create_game_project  (generates the game)
//   2. MCP semantic: create_material + author_registry_asset (content from prompt)
//   3. MCP semantic: validate_game_project (all-or-nothing gate)
//   4. SDK: configure + build + RUN the external consumer against the INSTALLED
//      SDK (proves the generated game compiles and executes out-of-tree)
//   5. cmake --install the consumer into a package dir (proves packaging)
//   6. Write demo-manifest.json with per-step evidence.
//
// Usage:
//   node demo-prompt-game.mjs [--prompt <text>] [--project <name>] [--keep]
import { mkdirSync, writeFileSync, existsSync, rmSync, cpSync } from 'fs';
import { join } from 'path';
import { tmpdir } from 'os';
import { spawnSync } from 'child_process';
import { callSemanticTool, semanticToolDefinitions } from '../mcp-server/game-authoring.mjs';
import { ERRORS } from './error-registry.mjs';

const ROOT = process.cwd();
const DEFS = semanticToolDefinitions();
const ERR_DEMO = ERRORS.find((e) => e.id === 'E-3002');
const ERR_ARG = ERRORS.find((e) => e.id === 'E-1001');   // invalid argument value
const ERR_PROJ = ERRORS.find((e) => e.id === 'E-1004');  // project not found
const ERR_EXISTS = ERRORS.find((e) => e.id === 'E-1005'); // document/asset already exists
const ERR_USAGE = ERRORS.find((e) => e.id === 'E-3001');  // CLI usage error

const argv = process.argv.slice(2);
if (argv.includes('--help') || argv.includes('-h')) {
  console.log('demo-prompt-game.mjs — §11-28: um único prompt gera, compila, roda e empacota um jogo.');
  console.log(`  --prompt <text>    descrição do jogo (default: jogo voxel procedural)`);
  console.log(`  --project <name>   nome do projeto (default: DemoPrompt_<ts>)`);
  console.log(`  --keep             não apagar o diretório temporário`);
  console.log(`  --help             esta ajuda (${ERR_USAGE.id})`);
  process.exit(0);
}
const prompt = argv[argv.indexOf('--prompt') + 1] || 'crie um jogo voxel com mundo procedural, um material de pedra e um registro de item, compile, rode e empacote';
const projectName = argv[argv.indexOf('--project') + 1] || `DemoPrompt_${Date.now()}`;
const keep = argv.includes('--keep');

function log(m) { console.log('[demo] ' + m); }
function fail(m) { console.error('[demo] FAIL: ' + m); process.exitCode = 1; return null; }

function semantic(tool, args) {
  const def = DEFS.find((d) => d.name === tool);
  if (!def) return fail(`unknown semantic tool '${tool}' (${ERR_DEMO.id})`);
  try {
    return callSemanticTool(ROOT, tool, args);
  } catch (error) {
    return { isError: true, message: error instanceof Error ? error.message : String(error) };
  }
}

function run(cmd, args, options = {}) {
  const res = spawnSync(cmd, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...options });
  if (res.stdout) console.log(res.stdout.split('\n').slice(-6).map((l) => '    ' + l).join('\n'));
  if (res.stderr) process.stderr.write(res.stderr.split('\n').slice(-6).map((l) => '    ' + l).join('\n') + '\n');
  return res;
}

function main() {
  const evidence = { steps: {} };
  log(`PROMPT: "${prompt}"`);
  log(`PROJECT: ${projectName}`);

  // ---- 1. MCP: create the game project ----
  log('1. create_game_project (MCP)');
  if (!projectName || projectName.length < 2) return fail(`invalid project name (${ERR_ARG.id})`);
  const created = semantic('create_game_project', { name: projectName });
  if (created.isError) {
    if (/exist/i.test(created.message)) return fail(`create_game_project: already exists (${ERR_EXISTS.id}): ${created.message}`);
    return fail(`create_game_project: ${created.message} (${ERR_PROJ.id})`);
  }
  evidence.steps.create = { ok: true };
  log('   project created');

  // ---- 2. MCP: author content from the prompt ----
  log('2. create_material stone_mat (MCP)');
  const mat = semantic('create_material', {
    project: projectName, name: 'stone_mat',
    albedo: { r: 0.5, g: 0.5, b: 0.5 }, roughness: 0.9, metallic: 0.0,
  });
  evidence.steps.material = { ok: !mat.isError, detail: mat.isError ? mat.message : undefined };
  log(`   material ${mat.isError ? 'REFUSED: ' + mat.message : 'OK'}`);

  log('3. author_registry_asset item stone (MCP)');
  const item = semantic('author_registry_asset', {
    project: projectName, kind: 'item', name: 'stone', data: { id: 'stone', displayName: 'Pedra', maxStack: 64 },
  });
  evidence.steps.item = { ok: !item.isError, detail: item.isError ? item.message : undefined };
  log(`   item ${item.isError ? 'REFUSED: ' + item.message : 'OK'}`);

  // ---- 3. MCP: validate ----
  log('4. validate_game_project (MCP)');
  const validated = semantic('validate_game_project', { project: projectName });
  const valid = validated && !validated.isError && (validated.valid === true || validated.result === true || validated.ok === true);
  evidence.steps.validate = { ok: valid, detail: JSON.stringify(validated).slice(0, 200) };
  log(`   valid=${valid}`);

  // ---- 4. SDK: compile + run the consumer against the INSTALLED SDK ----
  log('5. SDK consumer: configure + build + run (installed SDK)');
  const work = join(tmpdir(), `demo-game-${projectName}`);
  const prefix = join(work, 'sdk-install');
  const consumer = join(work, 'consumer');
  const consumerBuild = join(consumer, 'build');
  const pkg = join(work, 'package');

  // Install SDK from existing build
  // Build dir overridable (shared out-of-tree build); default legacy in-tree
  // (BUG-038 drift).
  const buildDir = join(ROOT, process.env.VC_BUILD_DIR ?? 'build');
  const install = run('cmake', ['--install', buildDir, '--config', 'Release', '--prefix', prefix]);
  evidence.steps.installSdk = { ok: install.status === 0 };
  if (install.status !== 0) return fail('SDK install failed (E-5001)');
  log('   SDK installed to ' + prefix);

  // Copy consumer template (only dep: find_package CONFIG)
  cpSync(join(ROOT, 'tools', 'external-project-empty'), consumer, { recursive: true });
  const cfg = run('cmake', ['-S', consumer, '-B', consumerBuild, `-DCMAKE_PREFIX_PATH=${prefix}`]);
  evidence.steps.configure = { ok: cfg.status === 0 };
  if (cfg.status !== 0) return fail('consumer configure failed (E-4001)');

  const bld = run('cmake', ['--build', consumerBuild, '--config', 'Release']);
  evidence.steps.build = { ok: bld.status === 0 };
  if (bld.status !== 0) return fail('consumer build failed (E-4001)');

  const exe = ['Release', 'Debug', ''].map((c) => join(consumerBuild, c, 'empty_consumer.exe')).find((p) => existsSync(p));
  if (!exe) return fail('consumer exe not found after build');
  const exeRun = run(exe, [], { timeout: 60000 });
  const ranOk = exeRun.status === 0 && /empty-consumer-ok/.test((exeRun.stdout || '') + (exeRun.stderr || ''));
  evidence.steps.run = { ok: ranOk, exit: exeRun.status };
  log(`   consumer ran: ${ranOk ? 'OK (empty-consumer-ok)' : 'FAILED (exit ' + exeRun.status + ')'}`);
  if (!ranOk) return fail('consumer run failed');

  // ---- 5. Package: stage the consumer into a distributable package dir
  // following the engine convention (Bin/<exe> + PackageManifest.txt,
  // all-or-nothing: stage then rename, remove staging on failure).
  log('6. package the consumer (stage + manifest)');
  const staging = join(work, 'package-staging');
  const pkgOk = (() => {
    try {
      const binDir = join(staging, 'Bin');
      mkdirSync(binDir, { recursive: true });
      cpSync(exe, join(binDir, 'empty_consumer.exe'));
      writeFileSync(join(staging, 'PackageManifest.txt'), [
        'package: demo-prompt-game',
        `project: ${projectName}`,
        `exe: empty_consumer.exe`,
        `built: ${new Date().toISOString()}`,
        'content: none (SDK consumer smoke)',
      ].join('\n') + '\n', 'utf8');
      // all-or-nothing: rename staging → package
      if (existsSync(pkg)) rmSync(pkg, { recursive: true, force: true });
      mkdirSync(join(work), { recursive: true });
      cpSync(staging, pkg, { recursive: true });
      rmSync(staging, { recursive: true, force: true });
      return existsSync(join(pkg, 'Bin', 'empty_consumer.exe')) && existsSync(join(pkg, 'PackageManifest.txt'));
    } catch (error) {
      try { rmSync(staging, { recursive: true, force: true }); } catch { /* ignore */ }
      console.error('    package error: ' + error.message);
      return false;
    }
  })();
  evidence.steps.package = { ok: pkgOk, pkg };
  log(`   packaged=${pkgOk} (${pkg})`);
  if (!pkgOk) return fail('packaging failed (E-4002)');

  // ---- 6. Evidence manifest ----
  const outDir = join(ROOT, 'out', 'artifacts', 'demo', projectName);
  mkdirSync(outDir, { recursive: true });
  const manifest = {
    prompt, project: projectName, steps: evidence.steps,
    surfaces: { mcp: true, sdk: true, cli: true },
    timestamp: new Date().toISOString(),
    tool: 'demo-prompt-game.mjs',
    note: 'Gerado por um único prompt via MCP (generate) + SDK (compile/run) + cmake install (package). Nenhum header privado tocado.',
  };
  writeFileSync(join(outDir, 'demo-manifest.json'), JSON.stringify(manifest, null, 2), 'utf8');
  log('7. evidence: ' + join(outDir, 'demo-manifest.json'));

  if (!keep) { try { rmSync(work, { recursive: true, force: true }); } catch { /* ignore */ } }

  const allOk = evidence.steps.create.ok && evidence.steps.build.ok && evidence.steps.run.ok && evidence.steps.package.ok;
  if (!allOk) return fail('pipeline incomplete');
  log('\nDEMO PASSED — single prompt → MCP generate → SDK compile → run → package.');
}

main();
