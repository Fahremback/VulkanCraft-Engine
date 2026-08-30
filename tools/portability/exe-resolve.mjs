// exe-resolve.mjs — canonical build-tree exe resolution shared by the
// portability gates. Every gate used to hardcode the legacy in-source
// `build/Release/` tree; the single canonical tree is now out/dev-shared
// (single-config Ninja, binaries at root; multi-config generators put them in
// <tree>/<config>/). VC_BUILD_DIR overrides, same convention as server.mjs /
// build-shared.ps1 / fast-gate.mjs / ci-matrix.mjs.
import { existsSync } from 'node:fs';
import { join } from 'node:path';

export const BUILD_REL = process.env.VC_BUILD_DIR || 'out/dev-shared';

export function exeCandidates(root, name) {
  return [
    join(root, BUILD_REL, name + '.exe'),
    join(root, BUILD_REL, 'Release', name + '.exe'),
    join(root, BUILD_REL, 'RelWithDebInfo', name + '.exe'),
    join(root, BUILD_REL, 'Debug', name + '.exe')
  ];
}

export function resolveExe(root, name) {
  const candidates = exeCandidates(root, name);
  return candidates.find((p) => existsSync(p)) || candidates[0];
}
