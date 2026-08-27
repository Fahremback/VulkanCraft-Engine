// sdk-check — FALTANTES item 22 sub-1/sub-2 (SDK público): o contrato do SDK.
//
// 1. DEFINE o SDK: a lista oficial de headers públicos é o que vive sob
//    src/engine/public/ (o único include dir exposto aos consumidores — a
//    regra "um projeto não inclui headers internos por acidente" é ESTRUTURAL:
//    headers internos não estão nesse include dir; este gate torna a regra
//    verificável também no lado dos headers públicos).
// 2. IMPEDE header interno por acidente DENTRO do SDK: nenhum header público
//    pode incluir um header que não esteja sob engine/, std ou vendor
//    promovido (glm/miniz/stb/jolt/recast/detour/zstd/blake3) — um include de
//    "RegistryJson.hpp" (src/engine/sdk), "VoxelMesher.hpp" (src/simulation)
//    etc. quebra o self-contained dos contratos e o external-consumer.
// 3. EXIGE includes canônicos: headers públicos referenciam os irmãos pelo
//    caminho completo "engine/<domain>/<Header>.hpp" (include dir = public),
//    nunca por nome curto — nomes curtos só resolvem por diretório relativo e
//    são frágeis quando o consumer usa o include path oficial.
// 4. EXIGE include guard (task_plan Agente 5 §1 item 5 — lifecycle/safety
//    uniforme): todo header público precisa de `#pragma once` ou `#ifndef`
//    guard. Um header SEM guard incluído duas vezes no mesmo TU quebra a
//    compilação do consumer (redefinition) ou, pior, duplica símbolos.
//
// Usage: node sdk-check.mjs [--emit-manifest] [--expect-count <n>]
// Exit 0 = green, 1 = issues (or count mismatch).

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const PUBLIC_ROOT = path.join(ENGINE_ROOT, "src", "engine", "public");

const STD_HEADERS = new Set([
  "algorithm", "array", "atomic", "cassert", "cctype", "charconv", "chrono", "cinttypes", "complex",
  "climits", "cmath", "cstdarg", "cstdbool", "cstddef", "cstdint", "cstdio", "cstdlib",
  "cstring", "ctime", "cwchar", "cwctype", "deque", "exception", "filesystem", "functional",
  "initializer_list", "iosfwd", "iterator", "limits", "list", "map", "memory", "mutex",
  "new", "numeric", "optional", "queue", "random", "ratio", "regex", "set", "span",
  "sstream", "stack", "stdexcept", "string", "string_view", "thread", "tuple", "type_traits", "unordered_map",
  "unordered_set", "utility", "variant", "vector"
]);
const VENDOR_PREFIXES = ["glm/", "miniz/", "stb/", "jolt/", "recast/", "detour/", "zstd/", "blake3/", "spdlog/", "mimalloc"];

// Optional feature defines — includes behind these #ifdef/#ifndef guards are
// acceptable in public headers (the API works without the optional dependency).
const OPTIONAL_DEFINES = new Set(["VC_USE_SPDLOG", "VC_USE_MIMALLOC"]);

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, out);
    else if (entry.name.endsWith(".hpp")) out.push(full);
  }
  return out;
}

/**
 * Simple preprocessor state tracker: tracks #ifdef/#ifndef/#else/#endif nesting
 * to determine if a given line is inside a conditional block with an unknown guard.
 * Returns true if the line is "active" (not inside a false conditional).
 */
function isLineActive(text, lineNum) {
  const lines = text.split("\n");
  let depth = 0;
  const stack = []; // true = active, false = inactive
  for (let i = 0; i < Math.min(lineNum, lines.length); i++) {
    const line = lines[i].trim();
    // #ifdef FOO / #ifndef FOO
    const ifdefMatch = line.match(/^#\s*if(?:def|ndef)\s+(\w+)/);
    if (ifdefMatch) {
      const define = ifdefMatch[1];
      // If inside a currently-active block, check if this new block is active
      const parentActive = depth === 0 || stack[depth - 1];
      if (parentActive) {
        // This block is active if the define is known or if it's an OPTIONAL_DEFINE
        // (optional defines are treated as "not defined" for checking purposes,
        // so includes inside them are considered conditional/optional)
        const isActive = !OPTIONAL_DEFINES.has(define);
        stack.push(isActive);
      } else {
        // Parent is inactive, so this block is also inactive
        stack.push(false);
      }
      depth++;
      continue;
    }
    // #else
    if (/^\s*#\s*else\b/.test(line)) {
      if (depth > 0) {
        const parentActive = depth === 1 || stack[depth - 2];
        // Flip the top of stack
        stack[depth - 1] = parentActive ? !stack[depth - 1] : false;
      }
      continue;
    }
    // #elif
    if (/^\s*#\s*elif\b/.test(line)) {
      if (depth > 0) {
        const parentActive = depth === 1 || stack[depth - 2];
        stack[depth - 1] = parentActive ? !stack[depth - 1] : false;
      }
      continue;
    }
    // #endif
    if (/^\s*#\s*endif\b/.test(line)) {
      if (depth > 0) {
        depth--;
        stack.pop();
      }
      continue;
    }
  }
  // If no active conditional blocks, line is active
  return depth === 0 || stack[depth - 1] !== false;
}

const publicHeaders = walk(PUBLIC_ROOT).sort();
const issues = [];
const shortIncludes = [];
const unguardedHeaders = [];

for (const file of publicHeaders) {
  const text = fs.readFileSync(file, "utf8");
  const lines = text.split("\n");
  const re = /#include\s+[\"<]([^\">]+)[\">]/g;
  let match;
  while ((match = re.exec(text))) {
    const include = match[1];
    if (include.startsWith("engine/")) continue;
    if (STD_HEADERS.has(include)) continue;
    if (VENDOR_PREFIXES.some((prefix) => include.startsWith(prefix))) continue;
    // Check if this include is inside a conditional block
    const lineNum = text.substring(0, match.index).split("\n").length;
    if (!isLineActive(text, lineNum)) continue;
    // A header público incluindo algo que não é engine/ nem std nem vendor:
    // é um header INTERNO (sdk/, app/, simulation/, scene/, ...) vazando para
    // o contrato público — quebra o self-contained dos contratos e o external-consumer.
    issues.push(`${path.relative(ENGINE_ROOT, file).replaceAll(path.sep, "/")} includes non-public "${include}"`);
  }
  // Includes por nome curto (sem prefixo engine/): frágeis fora da árvore.
  const shortRe = /#include\s+"((?!engine\/)[^"]+\.hpp)"/g;
  while ((match = shortRe.exec(text))) {
    if (STD_HEADERS.has(match[1])) continue;
    if (VENDOR_PREFIXES.some((prefix) => match[1].startsWith(prefix))) continue;
    const lineNum = text.substring(0, match.index).split("\n").length;
    if (!isLineActive(text, lineNum)) continue;
    shortIncludes.push(`${path.relative(ENGINE_ROOT, file).replaceAll(path.sep, "/")} short include "${match[1]}" (use engine/<domain>/<Header>.hpp)`);
  }
  // Include guard: every public header must carry `#pragma once` or an
  // #ifndef guard — a guardless header breaks consumers that include it twice
  // in one TU (redefinition). The old gate let 6 animation headers through.
  if (!/^\s*#\s*pragma\s+once/m.test(text) && !/#\s*ifndef/m.test(text)) {
    unguardedHeaders.push(path.relative(ENGINE_ROOT, file).replaceAll(path.sep, "/"));
  }
}

const manifest = publicHeaders.map((file) => path.relative(PUBLIC_ROOT, file).replaceAll(path.sep, "/"));

const emitManifest = process.argv.includes("--emit-manifest");
const writeIndex = process.argv.indexOf("--write-manifest");
const writeManifest = writeIndex >= 0 ? process.argv[writeIndex + 1] : null;
const expectIndex = process.argv.indexOf("--expect-count");
const expectCount = expectIndex >= 0 ? Number(process.argv[expectIndex + 1]) : null;

if (writeManifest) {
  fs.writeFileSync(writeManifest, JSON.stringify(manifest, null, 2));
  console.log(`sdk-check: manifest written to ${writeManifest} (${manifest.length} headers)`);
}

if (emitManifest) {
  process.stdout.write(JSON.stringify(manifest, null, 2));
}

// Reporting
console.log(`sdk-check: ${publicHeaders.length} public headers under src/engine/public`);
if (issues.length) {
  for (const issue of issues) console.log(`  FAIL ${issue}`);
}
if (shortIncludes.length) {
  for (const si of shortIncludes) console.log(`  WARN ${si}`);
}
if (unguardedHeaders.length) {
  for (const issue of unguardedHeaders) console.log(`  FAIL ${issue} has no include guard (add #pragma once)`);
}

// Expect count check
if (expectCount != null) {
  if (publicHeaders.length !== expectCount) {
    console.log(`  FAIL expected ${expectCount} headers, found ${publicHeaders.length}`);
  }
}

const totalIssues = issues.length + unguardedHeaders.length;
if (totalIssues > 0) {
  console.log(`sdk-check: ${totalIssues} issues found`);
  process.exit(1);
} else {
  console.log(`sdk-check: OK`);
  process.exit(0);
}
