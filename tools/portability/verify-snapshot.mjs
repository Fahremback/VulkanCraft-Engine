// Verify external/solutions/SNAPSHOT.md pinned commits match the actual
// clones in the catalog, and that every catalog entry has a license file.
// Node, no dependencies. Usage: node tools/portability/verify-snapshot.mjs
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const root = process.cwd();
const snapPath = path.join(root, "external", "solutions", "SNAPSHOT.md");
const snap = fs.readFileSync(snapPath, "utf8");

const rows = [];
for (const line of snap.split("\n")) {
  const m = line.match(/^\| `([^`]+)` \| `([0-9a-f]{40})` \|/);
  if (m) rows.push({ dir: m[1], commit: m[2] });
}

let ok = 0;
const problems = [];
for (const { dir, commit } of rows) {
  const dirPath = path.join(root, "external", "solutions", dir);
  if (!fs.existsSync(dirPath)) {
    problems.push(`${dir}: clone directory missing`);
    continue;
  }
  let head = null;
  try {
    head = execFileSync("git", ["-C", dirPath, "rev-parse", "HEAD"], {
      encoding: "utf8",
    }).trim();
  } catch {
    problems.push(`${dir}: not a git repo`);
    continue;
  }
  if (head !== commit) {
    problems.push(`${dir}: HEAD ${head} != pinned ${commit}`);
  } else {
    ok++;
  }
  // License carriers: LICENSE*, COPYING, and 3rdpartynotice (AMD/FidelityFX).
  const lic = fs
    .readdirSync(dirPath)
    .find((f) => /^(licen[cs]e|copying|3rdpartynotice)/i.test(f));
  if (!lic) problems.push(`${dir}: no license file found`);
}

console.log(`entries=${rows.length} commits-ok=${ok} problems=${problems.length}`);
for (const p of problems) console.log(`  - ${p}`);
process.exit(problems.length === 0 ? 0 : 1);
