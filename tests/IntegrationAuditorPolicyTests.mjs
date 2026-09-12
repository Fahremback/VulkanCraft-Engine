#!/usr/bin/env node
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const root = process.cwd();

execFileSync(process.execPath, ['tools/portability/integration-auditor.mjs'], {
  cwd: root,
  stdio: 'pipe',
  maxBuffer: 128 * 1024 * 1024
});

const report = JSON.parse(readFileSync(
  join(root, 'out', 'artifacts', 'integration-audit', 'integration-audit.json'),
  'utf8'
));

const violationKeys = new Set(
  report.violations.map((v) => `${v.code}:${v.capability ?? ''}`)
);

for (const row of report.rows) {
  if (!row.state.DECLARED) continue;
  const exempt = row.state.EXEMPT === true;

  if (exempt) {
    assert.equal(
      row.classification?.valid,
      true,
      `EXEMPT capability must carry a validated structured classification (${row.capability})`
    );
  }

  if (!row.state.IMPLEMENTED && !exempt) {
    assert.ok(
      violationKeys.has(`CAPABILITY-NOT-IMPLEMENTED:${row.capability}`),
      `missing CAPABILITY-NOT-IMPLEMENTED violation for ${row.capability}`
    );
  }

  if (!row.state.CONSUMED) {
    assert.equal(
      row.state.CERTIFIED,
      false,
      `CERTIFIED must be false when CONSUMED is false (${row.capability})`
    );
    if (!exempt) {
      assert.ok(
        violationKeys.has(`CAPABILITY-NOT-CONSUMED:${row.capability}`),
        `missing CAPABILITY-NOT-CONSUMED violation for ${row.capability}`
      );
    }
  }

  if (row.state.CONSUMED && !row.state.OBSERVABLE && !exempt) {
    assert.ok(
      violationKeys.has(`CAPABILITY-NOT-OBSERVABLE:${row.capability}`),
      `missing CAPABILITY-NOT-OBSERVABLE violation for ${row.capability}`
    );
  }
}

console.log(`Integration auditor policy invariants passed for ${report.rows.length} capabilities.`);
