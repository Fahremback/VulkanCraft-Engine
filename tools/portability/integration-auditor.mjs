#!/usr/bin/env node
// integration-auditor.mjs — A5 Section A: full state matrix per capability.
//
// For every public contract/factory in src/engine/public it derives a real state
// from code evidence (NOT from comments or declared goals):
//   DECLARED      -> signature exists in a public header
//   IMPLEMENTED   -> a non-test .cpp/.hpp in src/engine provides a definition/call
//   LINKED        -> the owning object/library is referenced in CMake for a real exe
//   CONSUMED      -> a call site exists in src/app|editor|server|sdk/cooker
//   OBSERVABLE    -> the call feeds an update/render/serialize loop or public query
//   CERTIFIED     -> a gate/test proves runtime behavior (evidence recorded)
//
// It also reports violations required by Section A:
//   * factory without consumer outside sdk/tests
//   * integration publishing only a boolean/config instead of algorithm output
//   * parallel (duplicate) service tracks across app/editor/server
//   * asset/tool that writes files without a runtime loader of that schema
//   * stub/placeholder/not wired/headless-only/TODO comments on product paths
//
// Output is machine-readable JSON + a static HTML report. No value is hardcoded
// as "PASSED"; every cell is derived from the scanned evidence.
import { readdirSync, readFileSync, statSync, existsSync } from 'node:fs';
import { join, relative, basename } from 'node:path';

const root = process.cwd();
const PUBLIC = join(root, 'src', 'engine', 'public');
const outDir = join(root, 'out', 'artifacts', 'integration-audit');
const htmlOut = join(outDir, 'integration-report.html');
const jsonOut = join(outDir, 'integration-audit.json');

function walk(dir, extRe, acc = []) {
  if (!existsSync(dir)) return acc;
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    let st; try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p, extRe, acc);
    else if (extRe.test(e)) acc.push(p);
  }
  return acc;
}
const rel = (p) => relative(root, p).replace(/\\/g, '/');

// ---------------- public capabilities ----------------
const publicHeaders = walk(PUBLIC, /\.(hpp|h)$/);
const factoryRe = /(?:std::unique_ptr|std::shared_ptr)\s*<[A-Za-z0-9_:<>,\s]*\sI[A-Za-z0-9_]+[A-Za-z0-9_:<>,\s]*>\s+(create_[a-z0-9_]+)\s*\(/g;
// A public contract may be declared as `class I...` OR `struct I...` (e.g.
// IEditorCamera, IGizmoController, IPublishPipeline, ISceneHierarchy,
// ItemStack are structs). Matching only `class` was a false-negative source.
const contractRe = /(?:class|struct)\s+(I[A-Za-z0-9_]+)\b/g;

const caps = new Map(); // key -> { name, kind, headers[], factories[] }
const factoryOwner = new Map(); // factory name -> header

// A compatibility header that contains only comments/preprocessor directives
// and includes does not declare a public capability of its own. Treating the
// filename as a capability manufactured false rows for forwarding headers such
// as IPluginIsolationRuntime.hpp and IPluginManifestCodec.hpp after their
// declarations were consolidated into the canonical headers.
export function isForwardingPublicHeader(source) {
  const semanticBody = String(source)
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/\/\/[^\n]*/g, ' ')
    .replace(/^\s*#.*$/gm, ' ')
    .trim();
  return semanticBody.length === 0;
}

for (const h of publicHeaders) {
  const text = readFileSync(h, 'utf8');
  if (isForwardingPublicHeader(text)) continue;
  const hRel = rel(h);
  const origin = basename(h).replace(/\.(hpp|h)$/, '');
  const seeds = new Set([origin]);
  let m;
  contractRe.lastIndex = 0;
  while ((m = contractRe.exec(text)) !== null) seeds.add(m[1]);
  // Contract-like headers: derive capability name from file + contracts.
  const capKey = origin;
  let c = caps.get(capKey);
  if (!c) { c = { name: capKey, kind: 'contract', headers: [], factories: [], contractInterfaces: new Set() }; caps.set(capKey, c); }
  c.headers.push(hRel);
  contracts: while ((m = contractRe.exec(text)) !== null) c.contractInterfaces.add(m[1]);
  factoryRe.lastIndex = 0;
  while ((m = factoryRe.exec(text)) !== null) {
    const fn = m[1];
    c.factories.push(fn);
    factoryOwner.set(fn, hRel);
  }
}
// If a header declares a factory for an I* contract in the same header, link the
// factory as evidence of capability; de-dup.
for (const c of caps.values()) {
  c.contractInterfaces = [...c.contractInterfaces];
  c.factories = [...new Set(c.factories)].sort();
  c.contractCount = c.contractInterfaces.length;
}

// ---------------- files per zone ----------------
const srcFiles = walk(join(root, 'src'), /\.(hpp|h|cpp|cc|cxx)$/).filter((f) => !f.startsWith(join(root, 'src', 'engine', 'public')));
const testFiles = walk(join(root, 'tests'), /\.(cpp|hpp)$/);
const appFiles = walk(join(root, 'src', 'app'), /\.(cpp|hpp)$/).filter((f) => f.startsWith(join(root, 'src', 'app')));
const editorFiles = walk(join(root, 'src', 'editor'), /\.(cpp|hpp)$/);
const serverFiles = walk(join(root, 'src', 'server'), /\.(cpp|hpp)$/);
const sdkFiles = walk(join(root, 'src', 'engine', 'sdk'), /\.(cpp|hpp)$/);
const toolFiles = walk(join(root, 'tools'), /\.(mjs|cpp|bat|c)$/);

const zones = { src: srcFiles, test: testFiles, app: appFiles, editor: editorFiles, server: serverFiles, sdk: sdkFiles, tool: toolFiles };
const zoneIndex = new Map(); // file -> zone
for (const [z, files] of Object.entries(zones)) for (const f of files) zoneIndex.set(f, z);

const readText = (f) => { try { return readFileSync(f, 'utf8'); } catch { return ''; } };
const joinText = (files) => files.map(readText).join('\n');
const text = (p) => readText(p);
const escapeRe = (s) => String(s).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

export function detectConstantUnavailableFactories(source) {
  const stripped = String(source)
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/\/\/[^\n]*/g, ' ');
  const factories = [];
  const availabilityMethods = [];
  const factoryBodyRe = /\b(?:std::unique_ptr|std::shared_ptr)\s*<[^>{};]+>\s+(create_[a-z0-9_]+)\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{([^{}]*)\}/g;
  let m;
  while ((m = factoryBodyRe.exec(stripped)) !== null) {
    const body = m[2].trim().replace(/\s+/g, ' ');
    if (/^return\s+(?:nullptr|false|\{\})\s*;$/.test(body)) factories.push(m[1]);
  }
  const availableBodyRe = /\bbool\s+([A-Za-z_][A-Za-z0-9_:]*available[A-Za-z0-9_:]*)\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{([^{}]*)\}/g;
  while ((m = availableBodyRe.exec(stripped)) !== null) {
    const body = m[2].trim().replace(/\s+/g, ' ');
    if (/^return\s+false\s*;$/.test(body)) availabilityMethods.push(m[1]);
  }
  return {
    factories: [...new Set(factories)].sort(),
    availabilityMethods: [...new Set(availabilityMethods)].sort()
  };
}

export function detectParallelPublicContracts(interfaceNames) {
  const names = [...new Set(interfaceNames.map(String))].sort();
  const nameSet = new Set(names);
  const suffixRe = /(Runtime|Codec|Legacy|V[0-9]+)$/;
  const grouped = new Map();
  for (const name of names) {
    const base = name.replace(suffixRe, '');
    if (base === name || !nameSet.has(base)) continue;
    const group = grouped.get(base) || new Set([base]);
    group.add(name);
    grouped.set(base, group);
  }
  return [...grouped.entries()]
    .map(([base, contracts]) => ({ base, contracts: [...contracts].sort() }))
    .sort((a, b) => a.base.localeCompare(b.base));
}

export function detectNotImplementedLines(source) {
  const findings = [];
  String(source).split('\n').forEach((line, index) => {
    if (!/not\s+implemented(?:\s+yet)?/i.test(line)) return;
    findings.push({ line: index + 1, snippet: line.trim() });
  });
  return findings;
}

export function textReferencesAnySymbol(source, symbols) {
  const haystack = String(source);
  return [...new Set((symbols || []).map(String).filter(Boolean))]
    .some((symbol) => new RegExp(`\\b${escapeRe(symbol)}\\b`).test(haystack));
}

// A public leaf factory may be intentionally owned by a product-consumed
// composition root instead of being called directly by an executable. Resolve
// that case only from structural evidence:
//   1. the leaf factory is actually called from an SDK implementation file;
//   2. that same file defines another public factory (the composition root);
//   3. the root factory is called directly by Game/Editor/Server; and
//   4. the leaf capability itself is already consumed by the product graph.
// Comments, same-header grouping, or mere SDK linkage are insufficient.
export function resolveTransitivelyOwnedFactories(factoryRows, capabilityRows,
                                                   sdkSources, productZoneText) {
  const rows = Array.isArray(factoryRows) ? factoryRows : [];
  const capabilities = Array.isArray(capabilityRows) ? capabilityRows : [];
  const sources = Array.isArray(sdkSources) ? sdkSources : [];
  const zones = productZoneText && typeof productZoneText === 'object'
    ? productZoneText : {};

  const stripComments = (source) => String(source)
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/\/\/[^\n]*/g, ' ');
  const cleanZones = Object.entries(zones)
    .map(([zone, source]) => [zone, stripComments(source)]);
  const capabilityByFactory = new Map();
  for (const capability of capabilities) {
    for (const factory of capability?.factories || []) {
      capabilityByFactory.set(factory, capability);
    }
  }
  const normalizePath = (value) => String(value ?? '').replace(/\\/g, '/');
  const capabilityForFactoryRow = (row) => {
    const explicit = capabilityByFactory.get(row.factory);
    if (explicit) return explicit;
    const rowHeaders = new Set((row.headers || []).map(normalizePath));
    if (rowHeaders.size === 0) return null;
    return capabilities.find((capability) =>
      (capability?.headers || []).some((header) => rowHeaders.has(normalizePath(header)))) || null;
  };

  const allFactoryNames = rows.map((row) => row?.factory).filter(Boolean);
  const directProductZones = (factory) => cleanZones
    .filter(([, source]) => new RegExp(`\\b${escapeRe(factory)}\\s*\\(`).test(source))
    .map(([zone]) => zone)
    .sort();
  const definesFactory = (source, factory) => new RegExp(
    `\\b(?:std::unique_ptr|std::shared_ptr)\\s*<[^;{}]+>\\s+${escapeRe(factory)}\\s*\\([^;{}]*\\)\\s*\\{`
  ).test(stripComments(source));

  const resolved = [];
  for (const row of rows) {
    if (!row?.factory || !['TEST-ONLY', 'SDK-INTERNAL'].includes(row.kind)) continue;
    if (!(Number(row.sdkSites) > 0)) continue;

    // The capability inventory is contract-centric, so some contract rows do
    // not carry their factory names even though the factory-consumption audit
    // reports the declaring header. Fall back to the shared public header.
    const capability = capabilityForFactoryRow(row);
    if (!capability?.state?.CONSUMED) continue;

    const leafCall = new RegExp(`\\b${escapeRe(row.factory)}\\s*\\(`);
    for (const sdk of sources) {
      const source = stripComments(sdk?.text ?? '');
      if (!leafCall.test(source)) continue;
      if (!textReferencesAnySymbol(source, capability.symbols || [capability.capability])) continue;

      const parents = [];
      const parentZones = new Set();
      for (const candidate of allFactoryNames) {
        if (candidate === row.factory || !definesFactory(source, candidate)) continue;
        const consumedZones = directProductZones(candidate);
        if (consumedZones.length === 0) continue;
        parents.push(candidate);
        for (const zone of consumedZones) parentZones.add(zone);
      }
      if (parents.length === 0) continue;

      resolved.push({
        factory: row.factory,
        capability: capability.capability,
        sdkSource: sdk?.path ?? '',
        compositionRoots: [...new Set(parents)].sort(),
        consumerZones: [...parentZones].sort()
      });
      break;
    }
  }
  return resolved.sort((a, b) => a.factory.localeCompare(b.factory));
}

export function propagatePublicConsumerZones(rows) {
  let changed = true;
  while (changed) {
    changed = false;
    for (const parent of rows) {
      if (!parent?.state?.CONSUMED || !parent.headerText) continue;
      for (const child of rows) {
        if (child === parent || !child?.symbols?.length) continue;
        if (!textReferencesAnySymbol(parent.headerText, child.symbols)) continue;

        const beforeConsumed = Boolean(child.state.CONSUMED);
        const beforeObservable = Boolean(child.state.OBSERVABLE);
        const zones = new Set(child.state.consumerZones || []);
        for (const zone of parent.state.consumerZones || []) zones.add(zone);
        child.state.consumerZones = [...zones].sort();
        child.state.CONSUMED = child.state.consumerZones.length > 0;
        child.state.OBSERVABLE = Boolean(child.state.OBSERVABLE || parent.state.OBSERVABLE);
        const evidence = new Set(child.state.consumerEvidence || []);
        evidence.add(`public-dependency:${parent.capability}`);
        child.state.consumerEvidence = [...evidence].sort();

        if (child.state.CONSUMED !== beforeConsumed ||
            child.state.OBSERVABLE !== beforeObservable) changed = true;
      }
    }
  }
  return rows;
}

export function validateStructuredClassification(row, entry, readEvidence) {
  const classification = String(entry?.classification ?? '').trim();
  const justification = String(entry?.justification ?? '').trim();
  const evidence = Array.isArray(entry?.evidence)
    ? entry.evidence.map(String).filter(Boolean)
    : [];
  if (!['internal', 'data-only'].includes(classification)) {
    return { valid: false, reason: 'unsupported-classification' };
  }
  if (justification.length < 20) {
    return { valid: false, reason: 'justification-too-short' };
  }
  if (evidence.length === 0 || typeof readEvidence !== 'function') {
    return { valid: false, reason: 'missing-evidence' };
  }

  let symbolHit = false;
  for (const evidencePath of evidence) {
    const content = readEvidence(evidencePath);
    if (typeof content !== 'string') {
      return { valid: false, reason: `evidence-unreadable:${evidencePath}` };
    }
    if (textReferencesAnySymbol(content, row?.symbols || [row?.capability])) {
      symbolHit = true;
    }
  }
  if (!symbolHit) {
    return { valid: false, reason: 'evidence-does-not-reference-capability' };
  }
  return {
    valid: true,
    classification,
    justification,
    evidence: [...new Set(evidence)].sort()
  };
}

// Hoisted zone joined-texts (shared by capability-state loop and parallel-track scan).
const ZONE_TEXT = {
  src: joinText(srcFiles),
  test: joinText(testFiles),
  app: joinText(appFiles),
  editor: joinText(editorFiles),
  server: joinText(serverFiles),
  sdk: joinText(sdkFiles),
  tool: joinText(toolFiles)
};

// ---------------- capability state derivation ----------------
const ownerExe = (f) => {
  const r = rel(f);
  if (/^src\/editor\//.test(r)) return 'Editor';
  if (/^src\/server\//.test(r)) return 'Server';
  if (/^src\/app\//.test(r)) return 'Game';
  return null;
};

const capRows = [];
for (const c of caps.values()) {
  const { src, test, app, editor, server, sdk, tool } = ZONE_TEXT;

  const isImpl = (needle) => new RegExp(`\\b${escapeRe(needle)}\\b`).test(src);
  // A self-contained, header-only contract — e.g. the core Allocator adapter,
  // every function defined `inline` in the public header with real bodies — is
  // genuinely implemented even though no implementation TU references it.
  // Detect a function body `inline <ret> name(...) { ... }` (non-empty `{`) in
  // any owning public header.
  const isInlineHeaderOnly = (cand) =>
    c.headers.some((h) => {
      const t = text(h);
      if (!/\binline\b/.test(t)) return false;
      const body = t.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
      // An inline body is a sequence: `inline <...>(...){` ... `\n}` where the
      // opening brace appears after an inline declaration (not just `inline;`).
      return /\binline\b[^{;]*\{/.test(body);
    });
  // Every public symbol owned by a capability is valid evidence. Some headers
  // intentionally contain a result/data type before the actual I* service, so
  // looking only at the first regex hit under-counts real product consumers.
  const needles = [...new Set([
    ...c.factories,
    ...c.contractInterfaces,
    c.name
  ].filter(Boolean))];
  const needle = needles[0] || null;

  const declared = c.headers.length > 0;
  const implemented = needles.some((candidate) => isImpl(candidate)) || isInlineHeaderOnly(c.name);
  // CONSUMED: real call site in an executable zone (app/editor/server) or cooker.
  const consumedInExe = [app, editor, server].some((zone) =>
    textReferencesAnySymbol(zone, needles));

  const zonesHit = [];
  if (consumedInExe) {
    if (textReferencesAnySymbol(app, needles)) zonesHit.push('Game');
    if (textReferencesAnySymbol(editor, needles)) zonesHit.push('Editor');
    if (textReferencesAnySymbol(server, needles)) zonesHit.push('Server');
  }
  const sdkHit = textReferencesAnySymbol(sdk, needles);
  const testHit = textReferencesAnySymbol(test, needles);

  // OBSERVABLE: the consumer zone references it within update/render/serialize context.
  // We approximate by checking the consumer's file set is non-trivial and includes loop keywords.
  let observable = false;
  if (zonesHit.length) {
    const zoneText = zonesHit.map((z) => (z === 'Game' ? app : z === 'Editor' ? editor : server)).join('\n');
    observable = /update|render|tick|step|frame|draw|save|load|snapshot|advance|simulate/i.test(zoneText);
  }

  // LINKED: the owning header is compiled into an SDK object module that is
  // joined into VC_SDK_PUBLIC_OBJECTS (which every executable links), OR its
  // consumer zone is a real executable (app/editor/server) that links the SDK.
  // Derived from the CMake wiring, not from a comment. STATIC scan — no build.
  const cmakeText = text(join(root, 'CMakeLists.txt'));
  const linkSdk = /VC_SDK_PUBLIC_OBJECTS/.test(cmakeText);
  const hdrInSdk = c.headers.some((h) => /^src\/engine\/(sdk|public|engine)/.test(h));
  const linked = (linkSdk && hdrInSdk) || zonesHit.length > 0;

  const isSdkTestOnly = testHit && !consumedInExe;
  const isSdkInternal = sdkHit && !consumedInExe && !testHit;

  // CERTIFIED: certification is product-level evidence. A test/gate can certify
  // only a capability that is consumed by a real executable. SDK presence or a
  // test reference by itself can never certify an unconsumed public contract.
  let certified = false;
  let hasCertificationEvidence = false;
  if (needles.length) {
    const gateText = joinText(walk(join(root, 'tools', 'portability'), /\.(mjs|cpp)$/));
    hasCertificationEvidence = testHit || textReferencesAnySymbol(gateText, needles);
    certified = consumedInExe && hasCertificationEvidence;
  }

  capRows.push({
    capability: c.name,
    kind: c.kind,
    symbols: needles,
    headerText: c.headers.map((h) => text(join(root, h))).join('\n'),
    certificationEvidence: hasCertificationEvidence,
    contracts: c.contractInterfaces,
    factories: c.factories,
    headers: c.headers,
    state: {
      DECLARED: declared,
      IMPLEMENTED: implemented,
      LINKED: linked,
      CONSUMED: consumedInExe,
      OBSERVABLE: observable,
      CERTIFIED: certified,
      consumerZones: zonesHit,
      consumerEvidence: zonesHit.map((z) => `direct:${z}`),
      sdkTestOnly: isSdkTestOnly,
      sdkInternal: isSdkInternal
    }
  });
}

// Public services can expose other public services through their canonical
// interface (e.g. INetworkServer::authority() -> IAuthoritativeRpc). If the
// parent contract is consumed by a product executable, that child service is
// consumed transitively through the same public path. Keep the evidence chain
// in the report instead of requiring executable code to repeat the child type.
propagatePublicConsumerZones(capRows);
for (const row of capRows) {
  row.state.CERTIFIED = Boolean(row.state.CONSUMED && row.certificationEvidence);
  row.state.sdkTestOnly = Boolean(row.state.sdkTestOnly && !row.state.CONSUMED);
  row.state.sdkInternal = Boolean(row.state.sdkInternal && !row.state.CONSUMED);
}

// ---------------- violations ----------------
const violations = [];

// Structured exceptions are intentionally narrow. They do not mutate the
// derived state cells; they explain why a public header is data-only/internal
// and provide source files that mechanically prove that classification.
const classificationPath = join(root, 'manifests', 'integration-classifications.json');
let classificationDocument = { version: 1, capabilities: {} };
if (existsSync(classificationPath)) {
  try {
    classificationDocument = JSON.parse(readFileSync(classificationPath, 'utf8'));
  } catch (error) {
    violations.push({
      code: 'AUDIT-CLASSIFICATION-INVALID',
      file: rel(classificationPath),
      detail: `classification manifest is not valid JSON: ${error.message}`,
      severity: 'warn'
    });
  }
}

const classificationEntries = classificationDocument?.capabilities ?? {};
for (const [capability, entry] of Object.entries(classificationEntries)) {
  const row = capRows.find((candidate) => candidate.capability === capability);
  if (!row) {
    violations.push({
      code: 'AUDIT-CLASSIFICATION-INVALID',
      capability,
      detail: 'classification names a capability that is not present in the public inventory',
      severity: 'warn'
    });
    continue;
  }
  const validated = validateStructuredClassification(row, entry, (evidencePath) => {
    const absolute = join(root, evidencePath);
    if (!existsSync(absolute)) return null;
    return readText(absolute);
  });
  if (!validated.valid) {
    violations.push({
      code: 'AUDIT-CLASSIFICATION-INVALID',
      capability,
      detail: `structured classification rejected: ${validated.reason}`,
      severity: 'warn'
    });
    continue;
  }
  row.classification = validated;
  row.state.EXEMPT = true;
}

// 0) State invariants. Public capabilities are product claims. Negative state
// cells must be visible as violations instead of being hidden behind a green
// aggregate. Structured exceptions may be introduced later only as explicit
// machine-readable classifications; there is intentionally no comment-based
// suppression path here.
for (const row of capRows) {
  if (!row.state.DECLARED) continue;
  const exempt = row.state.EXEMPT === true;
  if (!row.state.IMPLEMENTED) {
    if (exempt) continue;
    violations.push({
      code: 'CAPABILITY-NOT-IMPLEMENTED',
      capability: row.capability,
      header: row.headers[0] || '',
      detail: `${row.capability} is publicly declared but no implementation evidence was derived`,
      severity: 'warn'
    });
  }
  if (!row.state.CONSUMED) {
    if (exempt) continue;
    violations.push({
      code: 'CAPABILITY-NOT-CONSUMED',
      capability: row.capability,
      header: row.headers[0] || '',
      detail: `${row.capability} has no consuming call site in Game, Editor or Server`,
      severity: 'warn'
    });
  }
  if (!row.state.OBSERVABLE) {
    if (exempt) continue;
    violations.push({
      code: 'CAPABILITY-NOT-OBSERVABLE',
      capability: row.capability,
      header: row.headers[0] || '',
      detail: `${row.capability} has no observable product-loop evidence`,
      severity: 'warn'
    });
  }
  if (!row.state.CERTIFIED) {
    if (exempt) continue;
    violations.push({
      code: 'CAPABILITY-NOT-CERTIFIED',
      capability: row.capability,
      header: row.headers[0] || '',
      detail: `${row.capability} lacks product-level certification evidence`,
      severity: 'warn'
    });
  }
}

const publicContractNames = [...caps.values()].flatMap((c) => c.contractInterfaces || []);
const publicContractDeclarationHeaders = new Map();
for (const h of publicHeaders) {
  const source = readText(h);
  contractRe.lastIndex = 0;
  let declaration;
  while ((declaration = contractRe.exec(source)) !== null) {
    const name = declaration[1];
    const headers = publicContractDeclarationHeaders.get(name) || new Set();
    headers.add(rel(h));
    publicContractDeclarationHeaders.set(name, headers);
  }
}
for (const duplicate of detectParallelPublicContracts(publicContractNames)) {
  // A suffix relationship alone is not a parallel public authority. Some
  // canonical APIs intentionally expose two different roles in the same
  // header (for example a per-plugin isolation object and its host runtime
  // manager). Only report the pair when the declarations actually come from
  // different public authority headers. Compatibility include-only headers do
  // not create a second declaration and therefore do not count here.
  const authorityHeaders = new Set();
  for (const contract of duplicate.contracts) {
    for (const header of publicContractDeclarationHeaders.get(contract) || []) {
      authorityHeaders.add(header);
    }
  }
  if (authorityHeaders.size <= 1) continue;
  violations.push({
    code: 'PUBLIC-CONTRACT-DUPLICATE',
    capability: duplicate.base,
    detail: `parallel public contracts share one authority: ${duplicate.contracts.join(', ')}`,
    contracts: duplicate.contracts,
    severity: 'warn'
  });
}

// 1) factory with no real consumer outside sdk/tests. We delegate the authoritative
// per-factory classification to factory-consumption-audit.mjs (single combined-regex
// pass over all files, call sites rather than declarations) and surface its TEST-ONLY
// and DECLARED-ONLY rows that lack an executable consumer here.
import { execFileSync } from 'node:child_process';
const auditPath = join(root, 'tools', 'portability', 'factory-consumption-audit.mjs');
let consumption = null;
try {
  const out = execFileSync(process.execPath, [auditPath, '--json'], { encoding: 'utf8', maxBuffer: 1e8 });
  consumption = JSON.parse(out);
} catch { /* consumption audit unavailable; fall back to in-tool heuristic */ }
const factoryCompositionResolution = consumption
  ? resolveTransitivelyOwnedFactories(
      consumption.rows || [],
      capRows,
      sdkFiles.map((file) => ({ path: rel(file), text: readText(file) })),
      { Game: ZONE_TEXT.app, Editor: ZONE_TEXT.editor, Server: ZONE_TEXT.server }
    )
  : [];
const transitivelyOwnedFactories = new Set(
  factoryCompositionResolution.map((entry) => entry.factory)
);
if (consumption) {
  for (const r of consumption.rows || []) {
    if (r.kind === 'TEST-ONLY' || r.kind === 'DECLARED-ONLY') {
      if (transitivelyOwnedFactories.has(r.factory)) continue;
      violations.push({ code: 'FACTORY-NO-CONSUMER', factory: r.factory, header: r.headers[0] || '',
        detail: `${r.kind}: ${r.factory} has no consuming call site outside sdk/tests`,
        severity: r.kind === 'DECLARED-ONLY' ? 'warn' : 'info' });
    }
  }
}

// 2) parallel service tracks: same service implemented in >1 executable zone.
// A PARALLEL-TRACK is a REAL duplicate track: an executable references the
// service keyword in product code but NONE of the canonical PUBLIC contract
// symbols of that domain (i.e. it ships its own parallel implementation).
// Executables that consume the SAME canonical public contract (e.g. the game,
// editor and server all call create_world_runtime/IWorldRuntime) are NOT a
// duplicate — that is the single shared track the plan requires. The per-domain
// verification (which exes reference which canonical symbol) is recorded in
// parallelTrackVerification in the derived JSON, so the check is derived from
// real call sites, never a hardcoded PASSED.
const stripLineComments = (s) => s.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
const serviceContracts = {
  world:     { symbols: ['IWorldRuntime', 'create_world_runtime'] },
  render:    { symbols: ['IRenderGraph', 'create_render_graph', 'IViewportRenderer', 'create_render_pass_metrics', 'create_render_provider_registry', 'create_rendering_debug_view'] },
  network:   { symbols: ['INetworkServer', 'create_network_server', 'INetworkGameClient', 'create_network_game_client', 'INetworkSession', 'create_network_session', 'ITransport', 'create_transport', 'create_network_transport_factory'] },
  gameplay:  { symbols: ['IGameplayRuntime', 'create_gameplay_runtime'] },
  physics:   { symbols: ['IPhysicsWorld', 'PhysicsRuntime', 'create_runtime_physics'] },
  animation: { symbols: ['IAnimCore', 'create_anim_core', 'IMotionMatchVendor', 'create_motion_match_vendor', 'IProceduralAnimationPipeline', 'create_procedural_animation', 'IAnimationTimelineEditor', 'create_animation_timeline_editor', 'IGaitPlanner', 'IFootPlacement'] },
  ai:        { symbols: ['INavigation', 'INavigationProvider', 'create_navigation_provider'] },
  voxel:     { symbols: ['IVoxelWorld', 'create_default_voxel_world', 'create_voxel_world'] },
  audio:     { symbols: ['IAudioMixer', 'create_audio_mixer', 'IAudioEventMapper', 'create_audio_event_mapper', 'ISpatialAudio', 'create_spatial_audio', 'IAudioCodec', 'create_opus_codec', 'IAdaptiveMusic'] },
  particle:  { symbols: ['IParticleSystem', 'create_particle_system', 'IParticleProvider', 'create_particle_provider'] }
};
const serviceZoneText = {
  app: stripLineComments(ZONE_TEXT.app),
  editor: stripLineComments(ZONE_TEXT.editor),
  server: stripLineComments(ZONE_TEXT.server)
};
const parallelTrackVerification = {};
for (const [kw, def] of Object.entries(serviceContracts)) {
  const exeSymbols = {};
  for (const [zone, name] of [['app', 'Game'], ['editor', 'Editor'], ['server', 'Server']]) {
    const hits = def.symbols.filter((s) => new RegExp(`\\b${escapeRe(s)}\\b`).test(serviceZoneText[zone]));
    if (hits.length) exeSymbols[name] = hits;
  }
  const consuming = Object.keys(exeSymbols);
  parallelTrackVerification[kw] = {
    canonicalContracts: def.symbols,
    consumers: consuming,
    symbols: exeSymbols
  };
  // A genuine duplicate track: >1 exe consumes the service AND at least one
  // exe ships the service keyword in code WITHOUT any canonical public symbol.
  const keywordOnly = [];
  for (const [zone, name] of [['app', 'Game'], ['editor', 'Editor'], ['server', 'Server']]) {
    if (!exeSymbols[name] && new RegExp(`\\b${kw}\\b`, 'i').test(serviceZoneText[zone])) keywordOnly.push(name);
  }
  if (consuming.length > 1 && keywordOnly.length > 0) {
    violations.push({ code: 'PARALLEL-TRACK', service: kw, detail: `${kw} consumed via canonical public contracts in ${consuming.join(' + ')} but ALSO present in ${keywordOnly.join(' + ')} without the public contract (duplicate track)`, severity: 'info' });
  }
}

// 3) asset/tool writes file without a runtime loader of that schema (heuristic:
// a generator in tools/ that writes a .json/.bin/.vcasset that no src runtime references)
for (const t of toolFiles) {
  const txt = readText(t);
  const writes = /writeFileSync|openSync.*w|createWriteStream|fs\.writeFile|\.emit\(.*asset/.test(txt);
  if (!writes) continue;
  const extMatch = txt.match(/\.(json|bin|vcasset|tmpl|scene|world)\[\s*['"]/g);
  if (!extMatch) continue;
  const exts = [...new Set(extMatch.map((m) => m.replace(/[\\['"]/g, '')))];
  const loaderable = exts.filter((e) => new RegExp(`\\.${e}`).test(src)).length;
  if (loaderable === 0) {
    violations.push({ code: 'ASSET-NO-LOADER', tool: rel(t), detail: `tool writes ${exts.join(', ')} but no src loader consumes that schema`, severity: 'warn' });
  }
}

// 4) stub/placeholder/not wired/headless-only/TODO on product (src/ non-test) paths
// Note: the '// not implemented' and 'return nullptr;//stub' markers contain a
// C++ line-comment '//', so the pattern is built via RegExp constructor to avoid
// breaking the JS regex literal lexing.
// Deliberately tight: bare 'placeholder' is too broad (MessageCatalog legitimately
// interpolates {n} placeholders, Qt exposes QtPaletteRole::PlaceholderText, and
// MaterialPipeline genuinely emits a _placeholder shader member). We flag concrete
// integration defects only: explicit stubs, unwired callbacks, headless-only seams,
// TODO(frontend-port)/not-implemented markers and impersonator flags.
const stubRe = new RegExp([
  '\\bstub\\b', 'gameplay stub', 'not\\s+wired', 'not_wired',
  'headless[-_ ]only', 'TODO\\s*\\(frontend[-_ ]?port', 'FIXME',
  'callback (?:não|nao|not) (?:ligado|wired)',
  'impersonat', 'x-fake', 'nullptr\\s*;\\s*/\\/\\s*stub',
  'produce the stub', 'executable stub', 'the chain is a gameplay stub'
].join('|'), 'i');
// Product paths only: game (src/app), editor (src/editor), server (src/server),
// and engine SDK/rendering/simulation (src/engine, minus public headers).
// src/tools is excluded: it is build/packaging tooling whose executables use
// BuildTools.cpp, and the BuildPipeline stub lives only in the EXCLUDE_FROM_ALL
// test target — off the shipped product path, so it is not a Section A product
// stub. This matches the documented intent 'stub/... on product paths'.
const productFiles = [
  ...appFiles, ...editorFiles, ...serverFiles,
  ...srcFiles.filter((f) => {
    const r = rel(f);
    return r.startsWith('src/engine/') && !r.startsWith('src/engine/public/');
  })
];
for (const f of productFiles) {
  for (const finding of detectNotImplementedLines(readText(f))) {
    violations.push({
      code: 'NOT-IMPLEMENTED-BRANCH',
      file: rel(f),
      line: finding.line,
      snippet: finding.snippet.slice(0, 120),
      detail: 'product path still contains an explicit not-implemented branch or marker',
      severity: 'warn'
    });
  }
}
for (const f of productFiles) {
  const constantUnavailable = detectConstantUnavailableFactories(readText(f));
  for (const factory of constantUnavailable.factories) {
    violations.push({
      code: 'CONSTANT-UNAVAILABLE',
      file: rel(f),
      factory,
      detail: `${factory} unconditionally returns an unavailable value`,
      severity: 'warn'
    });
  }
  for (const method of constantUnavailable.availabilityMethods) {
    violations.push({
      code: 'CONSTANT-UNAVAILABLE',
      file: rel(f),
      method,
      detail: `${method} unconditionally reports unavailable`,
      severity: 'warn'
    });
  }
}
for (const f of productFiles) {
  const lines = readText(f).split('\n');
  lines.forEach((line, i) => {
    if (!stubRe.test(line)) return;
    // Negation guard: a comment asserting the ABSENCE of a stub ("No ... stub",
    // "not ... stub", "without ... stub") is the anti-60% norm being documented,
    // not a stub marker. Only concrete stubs/not-wired/TODO markers flag.
    if (/\b(?:no|not|without|none)\b[^;\n]{0,80}\bstub\b/i.test(line)) return;
    violations.push({ code: 'STUB-COMMENT', file: rel(f), line: i + 1, snippet: line.trim().slice(0, 90), severity: 'warn' });
  });
}

// 5) integration publishing only a boolean/config instead of algorithm output:
// detect refresh_gpu_features-style `? 1.0f : 0.0f` boolean gates multiplied into a
// feature weight with no named buffer/descriptor output.
const srcText = joinText(srcFiles);
if (/refresh_gpu_features/.test(srcText)) {
  const flagGates = (srcText.match(/\? 1[.]0f : 0[.]0f/g) || []).length;
  const buffers = (srcText.match(/\b(createBuffer|VkBuffer|descriptorSet|writeDescriptor|binding)/g) || []).length;
  if (flagGates > 0 && buffers === 0) {
    violations.push({ code: 'FLAG-ONLY-INTEGRATION', detail: `refresh_gpu_features publishes ${flagGates} boolean gates with no real buffer/descriptor output`, severity: 'warn' });
  } else if (flagGates > buffers && buffers > 0) {
    violations.push({ code: 'FLAG-ONLY-INTEGRATION', detail: `refresh_gpu_features has ${flagGates} boolean gates vs ${buffers} real buffer/descriptor sites; verify weights come from algorithm output`, severity: 'info' });
  }
}

// 5b) A5 §I-109: networking capability declared but no socket-level traffic
// test exists — refuse a `DONE` claim backed only by API presence. Real
// traffic evidence = a test TU that opens/connects sockets AND sends/receives
// bytes (not a loopback-free unit mock). The engine ships network_server_tests
// / network_replication_tests / network_rpc_tests / multiplayer_stress_tests.
//
// CONTA 4 (rede/servidor): the dedicated server now uses the SINGLE public
// transport — the same create_network_server/INetworkServer/ITransport surface
// the game and editor consume — with NO parallel raw-socket track. When that
// public surface is consumed by an executable AND real socket-level traffic
// tests exist, those tests are the CERTIFICATION of the public transport (its
// end-to-end client<->server bytes), not a parallel track, so NETWORK-TRAFFIC
// is no longer emitted as a violation. The certification evidence (which tests
// open sockets and move bytes + which executables consume the public surface)
// is recorded in the derived JSON under networkTransportCertification instead.
let networkTransportCertification = { publicTransportConsumed: false, trafficTests: [] };
{
  const netDeclared = /(NetworkRuntime|create_network_session|INetworkSession|NetworkServer|GameNetworkingSockets)/i.test(srcText);
  if (netDeclared) {
    const testTextAll = joinText(testFiles);
    const trafficTests = testFiles.filter((f) => {
      const t = joinText([f]);
      return /(socket\(|bind\(|connect\(|listen\(|accept\()/i.test(t)
        && /(send\(|recv\(|sendto\(|recvfrom\(|write\s*\(|read\s*\(|\bwrite_bytes\b|\bread_bytes\b|Packet|Frame)/i.test(t);
    });
    // The dedicated server / game / editor consume the PUBLIC transport surface
    // (create_network_server, INetworkServer over ITransport, the game client).
    const publicRe = /create_network_server|create_network_game_client|create_network_transport_factory|INetworkServer|INetworkGameClient|ITransport|create_transport/i;
    const publicTransportConsumed =
      publicRe.test(ZONE_TEXT.server) || publicRe.test(ZONE_TEXT.app) || publicRe.test(ZONE_TEXT.editor);
    networkTransportCertification = {
      publicTransportConsumed,
      trafficTests: trafficTests.map((f) => rel(f)),
      consumers: {
        server: publicRe.test(ZONE_TEXT.server),
        game: publicRe.test(ZONE_TEXT.app),
        editor: publicRe.test(ZONE_TEXT.editor)
      }
    };
    if (trafficTests.length > 0 && !publicTransportConsumed) {
      // Socket-traffic tests exist but are NOT backed by the public transport —
      // this is a genuine PARALLEL raw-socket track the product must close.
      violations.push({
        code: 'NETWORK-TRAFFIC',
        factory: 'networking',
        header: 'engine/networking',
        detail: 'socket-level traffic tests present but NOT consumed by the public transport surface (parallel raw-socket track): ' + trafficTests.map((f) => rel(f)).join(', '),
        severity: 'info'
      });
    } else if (trafficTests.length === 0 && !publicTransportConsumed) {
      violations.push({
        code: 'NETWORK-NO-TRAFFIC',
        factory: 'networking',
        header: 'engine/networking',
        detail: 'networking capability declared in product source but no test opens a socket AND sends/receives bytes (real client<->server traffic)',
        severity: 'warn'
      });
    }
    // When publicTransportConsumed is true the traffic tests certify the public
    // transport (recorded above), so neither NETWORK-TRAFFIC nor
    // NETWORK-NO-TRAFFIC is a violation — the parallel track is gone.
  }
}

// 6) objects linked into executables but never referenced by the real flow:
// parse CMakeLists.txt for the object-module -> sources mapping and derive, from
// each real add_executable block, exactly which object modules it links via
// $<TARGET_OBJECTS:...> (both plain and $<$<BOOL:...>:...> conditional wrappers).
// For each such module, collect the source files and the anchor identifiers they
// define (contract types, factory names, public class names, basename stem). If
// NONE of a linked module's anchors appears anywhere in the executable flow
// (app/editor/server sources plus the exe mains) it is a dead link: the TU is
// pulled into the binary yet never reached by product code — it reached only a
// test/isolated target or is genuinely unreferenced product code. Derived
// statically from the CMake + source text (no build executed).
const cmakeForModules = text(join(root, 'CMakeLists.txt'));
const moduleRe2 = /\bvc_object_module\s*\(\s*([^\s#]+)([\s\S]*?)\)\s*\n/g;
const moduleTUs = new Map(); // module -> list of .cpp TUs
{
  let mm;
  moduleRe2.lastIndex = 0;
  while ((mm = moduleRe2.exec(cmakeForModules)) !== null) {
    // The body runs until the closing ')' then a newline. Strip comments so a
    // trailing '// ...' comment on a source line doesn't hide a later ')'. Then
    // pull every path that ends in a real C++ translation-unit extension.
    const bodyNoComment = mm[2].replace(/#[^\n]*(?=\n|$)/g, ' ');
    const srcs = [...bodyNoComment.matchAll(/src[\/\\][^\s#\)]+(?:\.cpp|\.c|\.cc|\.cxx)/g)]
      .map((x) => x[0].replace(/["'${}]/g, '').trim());
    if (srcs.length) moduleTUs.set(mm[1], srcs);
  }
}
// Real product executables = the add_executable blocks that wire the SDK.
const productExeNames = ['VulkanEngineGame', 'VulkanEngineEditor', 'VulkanEngineServer', 'VulkanEngineCooker', 'VulkanEnginePackageBuilder'];
const linkedModules = new Set();
// 1) modules referenced directly with $<TARGET_OBJECTS:...> inside each product
// executable's add_executable block.
for (const pname of productExeNames) {
  const blockStart = cmakeForModules.indexOf(`add_executable(${pname}`);
  if (blockStart < 0) continue;
  // The exe source block ends at the next add_dependencies/add_executable/install
  const blockEndRel = cmakeForModules.indexOf('add_dependencies', blockStart);
  const blockEndExe = cmakeForModules.indexOf('add_executable', blockStart + ('add_executable(' + pname).length);
  let blockEnd = Infinity;
  if (blockEndRel > 0) blockEnd = Math.min(blockEnd, blockEndRel);
  if (blockEndExe > 0) blockEnd = Math.min(blockEnd, blockEndExe);
  const block = cmakeForModules.slice(blockStart, blockEnd === Infinity ? blockStart + 5000 : blockEnd);
  for (const o of block.matchAll(/TARGET_OBJECTS:([A-Za-z0-9_]+)/g)) linkedModules.add(o[1]);
}
// 2) the ${VC_SDK_PUBLIC_OBJECTS} aggregate appears as a variable reference in
// the exe source list; expand it into its member object modules so every SDK
// module that ships in a product binary is audited for real-flow usage.
for (const m of cmakeForModules.matchAll(/set\s*\(\s*VC_SDK_PUBLIC_OBJECTS\s*([\s\S]*?)\)\s*\n/g)) {
  for (const t of m[1].match(/[^\s#]+/g) || []) {
    const mm = t.match(/TARGET_OBJECTS:([^>\s]+)/);
    if (mm) linkedModules.add(mm[1]);
  }
}
// A TU is on the real flow when a symbol it defines is referenced OUTSIDE that
// TU — from any other shipped src/ file. A module whose own TUs are the only
// place its anchors appear is unreferenced by the rest of the product (only a
// test/isolated target or ITSELF reached it) and is flagged.
// For each module we build once a Set of the product files that are NOT one of
// that module's TUs, concatenate their text, and check anchors inside only that
// excluded text — so a symbol shared with an external consumer still counts even
// when the module's own TU also carries the token (e.g. TerrainGenerator used by
// World.cpp counts even though TerrainGenerator.cpp names the class itself).
const srcTextPerFile = new Map(); // abs path -> text
for (const f of srcFiles) srcTextPerFile.set(f, readText(f));
const moduleExcludedText = new Map(); // module -> concatenated product src minus its TUs
const moduleAnchors = (tuRel) => {
  const anchors = new Set([basename(tuRel).replace(/\.[^.]+$/, '')]);
  const t = readText(tuRel);
  for (const m of t.matchAll(/(?:class|struct)\s+(I?[A-Z][A-Za-z0-9_]*)\b/g)) anchors.add(m[1]);
  for (const m of t.matchAll(/\b(create_[a-z0-9_]+)\s*\(/g)) anchors.add(m[1]);
  return [...anchors].filter((a) => !a.includes('$'));
};
for (const mod of [...linkedModules].sort()) {
  const tus = moduleTUs.get(mod) || [];
  if (!tus.length) continue;
  const absTus = new Set(tus.map((tu) => join(root, tu)).filter((p) => existsSync(p)));
  if (!moduleExcludedText.has(mod)) {
    moduleExcludedText.set(mod, [...srcTextPerFile.entries()]
      .filter(([f]) => !absTus.has(f))
      .map(([f, t]) => t).join('\n'));
  }
  const outsideText = moduleExcludedText.get(mod);
  const referenced = [];
  const missingTus = [];
  for (const tu of tus) {
    const absTu = join(root, tu);
    if (!existsSync(absTu)) { missingTus.push(tu); continue; }
    if (/\.(hpp|h|cc)$/.test(tu)) continue; // only C++ TUs define flow entry points
    const anchors = moduleAnchors(absTu);
    if (anchors.some((a) => new RegExp(`\\b${a}\\b`).test(outsideText))) referenced.push(tu);
  }
  if (referenced.length === 0 && missingTus.length === 0) {
    violations.push({ code: 'DEAD-LINKED-OBJECT', module: mod,
      detail: `${mod} is linked into product executables but none of its ${tus.length} object TUs is referenced outside its own source by the product flow (${tus.slice(0, 3).map(rel).join(', ')}${tus.length > 3 ? ', …' : ''})`,
      tuses: tus.map((t) => rel(t)), severity: 'warn' });
  }
}

// ---------------- derive aggregates without hardcoding PASSED ----------------
const derived = {
  generator: 'integration-auditor.mjs',
  generatedAt: new Date().toISOString(),
  base: rel(root),
  capabilityTotal: capRows.length,
  byState: {
    declared: capRows.filter((r) => r.state.DECLARED).length,
    implemented: capRows.filter((r) => r.state.IMPLEMENTED).length,
    linked: capRows.filter((r) => r.state.LINKED).length,
    consumed: capRows.filter((r) => r.state.CONSUMED).length,
    observable: capRows.filter((r) => r.state.OBSERVABLE).length,
    certified: capRows.filter((r) => r.state.CERTIFIED).length
  },
  consumersByExe: {
    game: capRows.filter((r) => r.state.consumerZones.includes('Game')).length,
    editor: capRows.filter((r) => r.state.consumerZones.includes('Editor')).length,
    server: capRows.filter((r) => r.state.consumerZones.includes('Server')).length
  },
  violationCount: violations.length,
  violations: violations.map((v) => ({ ...v, commitNeeds: 'owner-domain' })),
  // Factories reached only through a real product composition root are kept
  // explicit here so suppression of FACTORY-NO-CONSUMER remains inspectable.
  factoryCompositionResolution,
  // CONTA 6 (integração): per-domain proof that Game/Editor/Server consume the
  // SAME canonical public contract (not two headers) — derived from real call
  // sites, so zero PARALLEL-TRACK is a derived result, never a hardcode.
  parallelTrackVerification,
  // CONTA 4 (rede/servidor): positive evidence that the socket-level traffic
  // tests certify the SINGLE public transport (the dedicated server / game /
  // editor consume create_network_server/INetworkServer/ITransport), never a
  // parallel raw-socket track.
  networkTransportCertification,
  rows: capRows
};

// ---------------- write machine-readable ----
import { mkdirSync, writeFileSync } from 'node:fs';
mkdirSync(outDir, { recursive: true });
writeFileSync(jsonOut, JSON.stringify(derived, null, 2) + '\n');

// ---------------- HTML report (states derived, no hardcoded PASSED values) ----
const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const stateChip = (label, value) => `<span class="chip ${value ? 'on' : 'off'}">${label}</span>`;
function rowHtml(r) {
  return `<tr><td class="mono">${esc(r.capability)}</td>
    <td>${stateChip('D', r.state.DECLARED)}${stateChip('I', r.state.IMPLEMENTED)}${stateChip('L', r.state.LINKED)}${stateChip('C', r.state.CONSUMED)}${stateChip('O', r.state.OBSERVABLE)}${stateChip('R', r.state.CERTIFIED)}</td>
    <td class="mono">${esc(r.factories.join(', ') || '—')}</td>
    <td>${r.state.consumerZones.map(esc).join(', ') || '—'}</td></tr>`;
}
function violHtml(v) {
  return `<tr><td class="mono">${esc(v.code)}</td><td class="mono">${esc((v.capability || v.service || v.file || v.tool || '').slice(0, 60))}</td><td>${esc(v.detail)}</td><td>${esc(v.severity)}</td></tr>`;
}
const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Integration Audit</title>
<style>
 body{font-family:ui-monospace,Consolas,monospace;margin:2rem;background:#0f1115;color:#e6e6e6;}
 h1,h2{color:#fff} table{border-collapse:collapse;width:100%;margin:1rem 0;font-size:13px}
 th,td{border:1px solid #2a2d35;padding:6px 8px;text-align:left;vertical-align:top}
 th{background:#1a1d24} .mono{font-family:inherit} .chip{display:inline-block;width:18px;text-align:center;font-weight:700;border-radius:4px;margin-right:2px;padding:0 2px}
 .chip.on{background:#123524;color:#4ade80} .chip.off{background:#2a1b1b;color:#f87171}
 .warn{color:#fbbf24} .info{color:#60a5fa}
</style></head><body>
<h1>Integration Audit — capability state matrix</h1>
<p>Generated ${esc(derived.generatedAt)} from ${esc(derived.base)}.</p>
<p>States are derived from code evidence only (D=DECLARED, I=IMPLEMENTED, C=CONSUMED in exe, O=OBSERVABLE in loop). No value is hardcoded.</p>
<h2>Derived aggregates</h2>
<ul>
<li>Capabilities inventoried: <strong>${derived.capabilityTotal}</strong></li>
<li>DECLARED ${derived.byState.declared} · IMPLEMENTED ${derived.byState.implemented} · LINKED ${derived.byState.linked} · CONSUMED ${derived.byState.consumed} · OBSERVABLE ${derived.byState.observable} · CERTIFIED ${derived.byState.certified}</li>
<li>Consumers: Game ${derived.consumersByExe.game} · Editor ${derived.consumersByExe.editor} · Server ${derived.consumersByExe.server}</li>
<li>Violations detected: <strong>${derived.violationCount}</strong></li>
</ul>
<h2>Violations (owner must justify or close)</h2>
<table><tr><th>Code</th><th>Subject</th><th>Detail</th><th>Severity</th></tr>
${derived.violations.map(violHtml).join('\n') || '<tr><td colspan="4">none derived from scan</td></tr>'}
</table>
<h2>Capabilities</h2>
<table><tr><th>Capability</th><th>D · I · L · C · O · R</th><th>Factories</th><th>Consumers</th></tr>
${derived.rows.map(rowHtml).join('\n')}
</table>
</body></html>`;
writeFileSync(htmlOut, html);

// ---------------- console summary ----------------
console.log(`[integration-auditor] scanned ${srcFiles.length} src, ${appFiles.length} app, ${editorFiles.length} editor, ${serverFiles.length} server files`);
console.log(`[integration-auditor] capabilities: ${derived.capabilityTotal}`);
console.log(`[integration-auditor] DECLARED=${derived.byState.declared} IMPLEMENTED=${derived.byState.implemented} LINKED=${derived.byState.linked} CONSUMED=${derived.byState.consumed} OBSERVABLE=${derived.byState.observable} CERTIFIED=${derived.byState.certified}`);
console.log(`[integration-auditor] consumers Game=${derived.consumersByExe.game} Editor=${derived.consumersByExe.editor} Server=${derived.consumersByExe.server}`);
console.log(`[integration-auditor] violations=${derived.violationCount} (${derived.violations.filter((v) => v.severity === 'warn').length} warn, ${derived.violations.filter((v) => v.severity === 'info').length} info)`);
const top = new Map();
for (const v of derived.violations) top.set(v.code, (top.get(v.code) || 0) + 1);
console.log(`[integration-auditor] by code: ${[...top.entries()].map(([k, n]) => `${k}=${n}`).join(' ')}`);
console.log(`[integration-auditor] wrote ${jsonOut} and ${htmlOut}`);

// ---- --check gate contract (same shape as the other tools/portability gates) ----
//   node integration-auditor.mjs --check
// Exits non-zero (FAIL) when WARN-severity violations exceed the threshold.
// Info-severity violations (parallel-track hints, stub notices) are reported but
// do not fail the gate; they are owner-domain awareness, not blockers. The
// threshold is configurable via VC_INTEGRATION_WARN_LIMIT (default 0: any warn
// blocks). The gate is a static scan — no build is executed.
function gateMain() {
  if (!process.argv.includes('--check')) return;
  const warnCount = derived.violations.filter((v) => v.severity === 'warn').length;
  const flagGates = derived.violations.filter((v) => v.code === 'FLAG-ONLY-INTEGRATION').length;
  const limit = Number(process.env.VC_INTEGRATION_WARN_LIMIT ?? 0);
  const blockers = warnCount - (flagGates > 0 ? 0 : 0);
  const failThreshold = limit;
  if (derived.violations.some((v) => v.code === 'STUB-COMMENT' && v.severity === 'warn')) {
    // Stub comments on product paths are the one class that must always fail the
    // gate: they are the documented anti-pattern of Section A. If they are
    // legitimate (e.g. an off product path) the owner must annotate, not suppress.
  }
  const statesAreDerived = derived.rows.every((r) =>
    typeof r.state.DECLARED === 'boolean' &&
    typeof r.state.IMPLEMENTED === 'boolean' &&
    typeof r.state.CONSUMED === 'boolean');
  const htmlRefersToJson = existsSync(htmlOut) && htmlOut.includes('.html');
  // FAIL conditions:
  //   * state cells are not booleans (derivation broke)
  //   * warn-only STUB-COMMENT presence is a hard fail (anti-pattern on product)
  //   * warn violations exceed the configured threshold
  const stubWarns = derived.violations.filter((v) => v.severity === 'warn' && v.code === 'STUB-COMMENT').length;
  const hardBlockers = (stubWarns > 0 ? 1 : 0) + (statesAreDerived ? 0 : 1);
  if (process.argv.includes('--also-gate-no-build')) console.log('(auditor --check) no build executed by this gate');
  console.log(`[integration-auditor] --check: warn=${warnCount} stubWarn=${stubWarns} limit=${failThreshold} hardBlockers=${hardBlockers} statesDerived=${statesAreDerived}`);
  const fails = hardBlockers > 0 || (warnCount - stubWarns) > failThreshold;
  if (fails) {
    console.error('[integration-auditor] --check: FAIL (integration gaps/warnings above threshold)');
    process.exit(1);
  }
  console.log('[integration-auditor] --check: OK (within threshold; see report for owner-domain follow-ups)');
}
gateMain();
