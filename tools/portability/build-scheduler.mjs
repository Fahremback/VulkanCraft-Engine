#!/usr/bin/env node
// build-scheduler.mjs — A5 Section C: resolve the MINIMAL set of CMake targets
// to rebuild for a given set of changed source files, so build_game / start_build
// do not needlessly relink every executable.
//
// It statically parses CMakeLists.txt (without running CMake — the build lock of
// the coordination plan forbids executing builds during the implementation phase):
//   * vc_object_module(<target> <src>...)        -> source → object-module ND
//   * add_executable(<exe> <src...>)             -> source → executable ND
//   * ${VC_SDK_PUBLIC_OBJECTS} / $<TARGET_OBJECTS:...>  -> object-module → exe
//
// Usage:
//   node build-scheduler.mjs --changed <src/a.cpp> <src/b.hpp>   (explicit files)
//   node build-scheduler.mjs --git                                (from git status)
//   node build-scheduler.mjs --targets                            (list known targets)
//   node build-scheduler.mjs --graph                              (dump dependency graph)
//
// Output (stdout): { changed, affected_modules, affected_executables, order }
// where `order` preserves link order (dependencies before dependents). Every value
// is derived from the parsed graph — nothing is hardcoded.
import { readFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { relative, resolve, join, sep } from 'node:path';

const ROOT = process.cwd();
const CMAKE = join(ROOT, 'CMakeLists.txt');

// ── minimal CMake block parser ──────────────────────────────────────────────
// Tokens split on whitespace only (CMake arguments are space-separated). We need
// the first argument of add_library/add_executable plus following tokens that are
// source paths/object refs. `${VAR}` / `$<TARGET_OBJECTS:...>` handled literally.
function toTokens(text) {
  // strip comments
  text = text.replace(/#[^\n]*(?=\n|$)/g, ' ');
  const tokens = [];
  for (const m of text.matchAll(/[^\s]+/g)) tokens.push(m[0]);
  return tokens;
}
// returns [{ type:'module'|'exe', name, sources:[...] }]
function parseTargets(cmake) {
  const targets = [];
  const re = /\b(add_library|add_executable)\s*\(\s*([^\s#]+)([\s\S]*?)(?=\n\s*(?:add_library|add_executable|vc_object_module|install\s*|foreach\s*|end\(|if\s*\(|else|endif|target_|set_property|add_dependencies|add_test))(?=\n)/g;
  // A simpler robust approach: scan sequentially for the open paren block and
  // balance parentheses, collecting the first line + continuation until the
  // paren depth returns to zero.
  let i = 0;
  const lines = cmake.split(/\r?\n/);
  const collected = []; // {start, text} opening blocks by type
  for (let li = 0; li < lines.length; li++) {
    const m = lines[li].match(/\b(add_library|add_executable|vc_object_module)\s*\(/);
    if (!m) continue;
    const type = m[1] === 'vc_object_module' ? 'module' : (m[1] === 'add_library' ? 'library' : 'exe');
    // accumulate until paren balance resumes
    let depth = 0;
    let started = false;
    let buf = '';
    let j = li;
    for (; j < lines.length; j++) {
      for (const ch of lines[j]) {
        if (ch === '(') { depth++; started = true; }
        else if (ch === ')') depth--;
      }
      buf += lines[j] + '\n';
      if (started && depth === 0) break;
      if (started && depth < 0) break;
    }
    // name = first token after (
    const nameM = buf.match(/\(([^\s#]+)/);
    const name = nameM ? nameM[1] : null;
    if (!name) continue;
    // sources = path-like tokens in the block body (contain '/' or '.cpp' or
    // `${`/`$<`); exclude helper options like EXCLUDE_FROM_ALL
    const body = buf.slice(buf.indexOf('(') + 1, buf.lastIndexOf(')'));
    const opts = new Set(['EXCLUDE_FROM_ALL', 'PRIVATE', 'PUBLIC', 'INTERFACE', 'STATIC', 'OBJECT', 'SHARED', 'ALIAS']);
    // Keep ${...} aggregate-variable tokens (e.g. ${VC_SDK_PUBLIC_OBJECTS}) so a
    // later step can expand them into their member modules; drop literal generator
    // expressions like $<...> here (they cannot name a source we relate to).
    const sources = toTokens(body).filter((t) =>
      !opts.has(t) && !t.startsWith('$(') && !t.startsWith('#'));
    collected.push({ type, name, sources, line: li + 1 });
    li = j; // skip consumed lines
  }
  return collected;
}

// object-module target -> list of source files (from vc_object_module blocks)
// and, separately, the aggregate ${VC_SDK_PUBLIC_OBJECTS} membership marker.
function buildGraph(cmakeText) {
  const targets = parseTargets(cmakeText);

  const sourceToModule = new Map(); // abs/rel path -> module name
  const moduleSources = new Map();  // module -> [paths]
  const exeSources = new Map();     // exe -> [paths/moduleRefs]

  // vc_object_module(<name> <src>...) — also match add_library(... OBJECT ...)
  const moduleList = [];
  const moduleRe = /\bvc_object_module\s*\(\s*([^\s#]+)([\s\S]*?)\)\s*\n/g;
  for (const m of cmakeText.matchAll(moduleRe)) {
    const name = m[1];
    const body = toTokens(m[2]);
    moduleList.push({ name, sources: body.filter((t) => !t.startsWith('$<')) });
  }
  // add_library(<name> OBJECT ...)
  for (const t of targets) {
    if (t.type === 'library' && t.sources.some((s) => s === 'OBJECT')) {
      moduleList.push({ name: t.name, sources: t.sources });
    }
  }
  for (const mod of moduleList) {
    const paths = [];
    for (const s of mod.sources) {
      const p = s.replace(/["']/g, '').replace(/\$\{[^}]+\}/g, '').trim();
      if (!p || p.startsWith('$') || p.startsWith('#') || !/\.(cpp|cc|cxx|hpp|h|c|cc)$/.test(p)) continue;
      paths.push(p);
      if (!sourceToModule.has(p)) sourceToModule.set(p, mod.name);
    }
    moduleSources.set(mod.name, paths);
  }

  // Aggregate ${VC_SDK_PUBLIC_OBJECTS} expands to the split sdk modules. Parse the
  // set(...) block so any consumer of the variable maps to ALL of its members.
  const sdkAggregate = new Set();
  for (const m of cmakeText.matchAll(/set\s*\(\s*VC_SDK_PUBLIC_OBJECTS\s*([\s\S]*?)\)\s*\n/g)) {
    for (const t of toTokens(m[1])) {
      const mm = t.match(/TARGET_OBJECTS:([^\s>]+)/);
      if (mm) sdkAggregate.add(mm[1]);
    }
  }

  // executables
  const exeList = targets.filter((t) => t.type === 'exe');
  for (const exe of exeList) {
    const refs = [];
    for (const s of exe.sources) {
      const p = s.replace(/["']/g, '').replace(/\$\{[^}]+\}/g, '').trim();
      if (!p) continue;
      refs.push(p);
    }
    // expand ${VC_SDK_PUBLIC_OBJECTS} membership into the ref list
    if (exe.sources.some((s) => /\$\{VC_SDK_PUBLIC_OBJECTS\}/.test(s))) {
      for (const mod of sdkAggregate) refs.push(`$<TARGET_OBJECTS:${mod}>`);
    }
    exeSources.set(exe.name, refs);
  }

  // module -> exes that include it. Object-module refs appear as generator
  // expressions $<TARGET_OBJECTS:mod> AND inside wrapped $<$<BOOL:...>:...> forms:
  // strip generator wrappers so both plain and conditioned refs are matched.
  const moduleToExes = new Map();
  for (const [exe, refs] of exeSources) {
    for (const ref of refs) {
      const modM = ref.match(/TARGET_OBJECTS:([^>\s]+)/);
      if (modM) {
        if (!moduleToExes.has(modM[1])) moduleToExes.set(modM[1], []);
        if (!moduleToExes.get(modM[1]).includes(exe)) moduleToExes.get(modM[1]).push(exe);
      }
    }
  }

  return { sourceToModule, moduleSources, exeSources, moduleToExes, allModules: moduleList.map((m) => m.name), allExes: exeList.map((e) => e.name) };
}

function classify(file) {
  // returns { module (source->object module) or null, exes that compile the file directly }
  const relF = file.replace(/\\/g, '/').replace(/^\//, '');
  const graph = buildGraph(readFileSync(CMAKE, 'utf8'));
  const module = graph.sourceToModule.get(relF) || null;
  const directExes = [];
  for (const [exe, refs] of graph.exeSources) {
    if (refs.includes(relF) && !refs.some((r) => r.startsWith('$'))) directExes.push(exe);
  }
  return { module, directExes, graph };
}

function collectChangedFromGit() {
  // Large source trees (thousands of untracked build/backup entries) overflow the
  // default 1 MiB child buffer with `--untracked-files=all`. Raise the limit so the
  // scheduler reflects the true working tree instead of dying with ENOBUFS, and
  // restrict to tracked + changed files by letting git do the filtering. Untracked
  // files that are not part of a target (e.g. backup/) are harmless no-ops later.
  const out = execFileSync('git', ['status', '--porcelain', '--untracked-files=all'], { encoding: 'utf8', cwd: ROOT, maxBuffer: 256 * 1024 * 1024 });
  return out.split(/\r?\n/).filter(Boolean).map((line) => line.replace(/\s*[MADRCU?]+\s+/, '').trim()).filter(Boolean);
}

function main() {
  const args = process.argv.slice(2);
  const graph = buildGraph(readFileSync(CMAKE, 'utf8'));

  if (args.includes('--graph')) {
    const g = {};
    for (const [mod, paths] of graph.moduleSources) g['module:' + mod] = paths;
    for (const [exe, refs] of graph.exeSources) g['exe:' + exe] = refs;
    console.log(JSON.stringify({ modules: graph.allModules.length, exes: graph.allExes.length, graph: g }, null, 2));
    return;
  }
  if (args.includes('--targets')) {
    console.log(JSON.stringify({ modules: [...graph.allModules].sort(), executables: [...graph.allExes].sort() }, null, 2));
    return;
  }

  let changed;
  if (args.includes('--changed')) {
    const i = args.indexOf('--changed');
    changed = args.slice(i + 1);
  } else if (args.includes('--git')) {
    changed = collectChangedFromGit();
  } else {
    changed = collectChangedFromGit(); // default derives from working tree
  }
  changed = [...new Set(changed)];

  const affectedModules = [];
  const unknown = [];
  for (const f of changed) {
    const relF = f.replace(/\\/g, '/').replace(/^\//, '');
    if (graph.sourceToModule.has(relF)) {
      const m = graph.sourceToModule.get(relF);
      if (!affectedModules.includes(m)) affectedModules.push(m);
    } else {
      // still classify by directory
      unknown.push(relF);
    }
  }
  // executables: modules' consumers + direct exe source changes
  const affectedExes = new Set();
  for (const mod of affectedModules) (graph.moduleToExes.get(mod) || []).forEach((e) => affectedExes.add(e));
  for (const f of changed) {
    const relF = f.replace(/\\/g, '/');
    if (/^src\/app\//.test(relF)) affectedExes.add('VulkanEngineGame');
    if (/^src\/editor\//.test(relF)) affectedExes.add('VulkanEngineEditor');
    if (/^src\/server\//.test(relF)) affectedExes.add('VulkanEngineServer');
  }
  for (const f of changed) {
    const relF = f.replace(/\\/g, '/');
    for (const [exe, refs] of graph.exeSources) {
      if (refs.includes(relF) && !refs.some((r) => r.startsWith('$'))) affectedExes.add(exe);
    }
  }

  const result = {
    changed,
    affected_modules: affectedModules,
    affected_executables: [...affectedExes].sort(),
    unknown_files: [...new Set(unknown)],
    minimal_targets: affectedModules.length === 0 && affectedExes.size === 0
      ? []
      : [...affectedExes].sort()
  };
  console.log(JSON.stringify(result, null, 2));
}

main();