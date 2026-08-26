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
//
// Usage: node sdk-check.mjs [--emit-manifest] [--expect-count <n>]
// Exit 0 = green, 1 = issues (or count mismatch).

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const PUBLIC_ROOT = path.join(ENGINE_ROOT, "src", "engine", "public");

const STD_HEADERS = new Set([
  "algorithm", "array", "atomic", "cassert", "cctype", "charconv", "chrono", "cinttypes",
  "climits", "cmath", "cstdarg", "cstdbool", "cstddef", "cstdint", "cstdio", "cstdlib",
  "cstring", "ctime", "cwchar", "cwctype", "deque", "exception", "filesystem", "functional",
  "initializer_list", "iosfwd", "iterator", "limits", "list", "map", "memory", "mutex",
  "new", "numeric", "optional", "queue", "random", "ratio", "regex", "set", "span",
  "sstream", "stack", "stdexcept", "string", "thread", "tuple", "type_traits", "unordered_map",
  "unordered_set", "utility", "variant", "vector"
]);
const VENDOR_PREFIXES = ["glm/", "miniz/", "stb/", "jolt/", "recast/", "detour/", "zstd/", "blake3/"];

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, out);
    else if (entry.name.endsWith(".hpp")) out.push(full);
  }
  return out;
}

const publicHeaders = walk(PUBLIC_ROOT).sort();
const issues = [];
const shortIncludes = [];

for (const file of publicHeaders) {
  const text = fs.readFileSync(file, "utf8");
  const re = /#include\s+["<]([^">]+)[">]/g;
  let match;
  while ((match = re.exec(text))) {
    const include = match[1];
    if (include.startsWith("engine/")) continue;
    if (STD_HEADERS.has(include)) continue;
    if (VENDOR_PREFIXES.some((prefix) => include.startsWith(prefix))) continue;
    // A header público incluindo algo que não é engine/ nem std nem vendor:
    // é um header INTERNO (sdk/, app/, simulation/, scene/, ...) vazando para
    // o contrato público — quebra o self-contained.
    issues.push(`${path.relative(ENGINE_ROOT, file).replaceAll(path.sep, "/")} includes non-public "${include}"`);
  }
  // Includes por nome curto (sem prefixo engine/): frágeis fora da árvore.
  const shortRe = /#include\s+"((?!engine\/)[^"]+\.hpp)"/g;
  while ((match = shortRe.exec(text))) {
    if (STD_HEADERS.has(match[1])) continue;
    if (VENDOR_PREFIXES.some((prefix) => match[1].startsWith(prefix))) continue;
    shortIncludes.push(`${path.relative(ENGINE_ROOT, file).replaceAll(path.sep, "/")} short include "${match[1]}" (use engine/<domain>/<Header>.hpp)`);
  }
}

const manifest = publicHeaders.map((file) => path.relative(PUBLIC_ROOT, file).replaceAll(path.sep, "/"));

const emitManifest = process.argv.includes("--emit-manifest");
const expectIndex = process.argv.indexOf("--expect-count");
const expectCount = expectIndex >= 0 ? Number(process.argv[expectIndex + 1]) : null;

if (emitManifest) {
  for (const entry of manifest) console.log(entry);
} else {
  console.log(`sdk-check: ${publicHeaders.length} public headers under src/engine/public`);
  console.log(`sdk-check: ${issues.length} non-public include(s), ${shortIncludes.length} short include(s)`);
  for (const issue of issues) console.log(`  FAIL ${issue}`);
  for (const issue of shortIncludes) console.log(`  FAIL ${issue}`);
  if (expectCount !== null && publicHeaders.length !== expectCount) {
    console.log(`  FAIL expected ${expectCount} public headers, found ${publicHeaders.length}`);
    process.exit(1);
  }
  if (issues.length > 0 || shortIncludes.length > 0) process.exit(1);
  console.log("sdk-check: OK — every public header is self-contained (engine//std/vendor only)");
}
