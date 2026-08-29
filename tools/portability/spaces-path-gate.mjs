#!/usr/bin/env node
// spaces-path-gate.mjs — §11-25: SDK/MCP must work installed in a path WITH
// SPACES (a common Windows pitfall) and with a consumer that has spaces in its
// own path. Clone-clean / no-privilege remain blocked by infrastructure
// (network); this gate closes the "path with spaces" clause locally.
//
//   1. Install the SDK from the existing build into "<tmp>/SDK With Spaces".
//   2. Copy tools/external-project (consumer: only find_package CONFIG) into
//      "<tmp>/My Game Project" (also with spaces).
//   3. Configure + build + RUN the consumer against the spaced prefix.
//   4. Exit 0 = pass. The engine build must exist (build/).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { ERRORS } from './error-registry.mjs';

const GATE_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(GATE_DIR, '..', '..');
// Build dir overridable so the gate can run against the shared out-of-tree
// build (e.g. VC_BUILD_DIR=out/mission-gate). Default stays legacy in-tree build.
const BUILD_DIR = path.join(ENGINE_ROOT, process.env.VC_BUILD_DIR ?? 'build');
const ERR_INSTALL = ERRORS.find((e) => e.id === 'E-5001'); // SDK install/consume misconfig
const ERR_BUILD = ERRORS.find((e) => e.id === 'E-4001');   // build failed

function log(m) { process.stderr.write(`[spaces-path-gate] ${m}${os.EOL}`); }
function fail(m) { process.stderr.write(`[spaces-path-gate] FAIL: ${m}${os.EOL}`); process.exitCode = 1; }

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...options });
  if (result.stdout) process.stderr.write(result.stdout);
  if (result.stderr) process.stderr.write(result.stderr);
  return result;
}

function main() {
  if (!fs.existsSync(BUILD_DIR)) {
    fail(`build/ not found at ${BUILD_DIR} — build the engine first (${ERR_INSTALL.id})`);
    return;
  }
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'vc-spaces-path-'));
  const prefix = path.join(work, 'SDK With Spaces');
  const projectDir = path.join(work, 'My Game Project');

  log(`prefix with spaces: ${prefix}`);
  log(`consumer with spaces: ${projectDir}`);

  // 1. Install SDK into a spaced prefix
  log('cmake --install into spaced prefix');
  const install = run('cmake', ['--install', BUILD_DIR, '--config', 'Release', '--prefix', prefix]);
  if (install.status !== 0) {
    fail(`cmake --install failed (${ERR_INSTALL.id})`);
    return;
  }

  // 2. Copy consumer template into a spaced project dir
  const template = path.join(ENGINE_ROOT, 'tools', 'external-project');
  if (!fs.existsSync(template)) {
    fail(`consumer template not found at ${template}`);
    return;
  }
  fs.cpSync(template, projectDir, { recursive: true });
  log(`consumer copied to ${projectDir}`);

  // 3. Configure consumer against the spaced prefix
  const buildDir = path.join(projectDir, 'build');
  log('configure consumer (spaced paths)');
  const configure = run('cmake', ['-S', projectDir, '-B', buildDir, `-DCMAKE_PREFIX_PATH=${prefix}`]);
  if (configure.status !== 0) {
    fail(`consumer configure failed (${ERR_BUILD.id})`);
    return;
  }

  // 4. Build + run the consumer
  log('build consumer');
  const build = run('cmake', ['--build', buildDir, '--config', 'Release']);
  if (build.status !== 0) {
    fail(`consumer build failed (${ERR_BUILD.id})`);
    return;
  }

  log('run consumer');
  // Template target is 'consumer' (tools/external-project/CMakeLists.txt)
  const exePath = ['Release', 'Debug', ''].map((cfg) => path.join(buildDir, cfg, 'consumer.exe')).find((p) => fs.existsSync(p));
  if (!exePath) {
    fail('consumer executable not found after build');
    return;
  }
  const runRes = run(exePath, [], { timeout: 60000 });
  if (runRes.status !== 0) {
    fail(`consumer run failed (exit ${runRes.status})`);
    return;
  }
  log('consumer ran successfully from spaced path');

  // Cleanup
  try { fs.rmSync(work, { recursive: true, force: true }); } catch { /* ignore */ }
  log('SPACES-PATH GATE PASSED — SDK install + consumer work in paths with spaces.');
}

main();
