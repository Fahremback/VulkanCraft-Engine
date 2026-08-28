#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const manifestPath = path.join(root, 'tools', 'portability', 'integration-manifest.mjs');
const required = [
  ['src/engine/public/engine/jobs/IJobSystem.hpp', 'public job contract'],
  ['src/engine/sdk/JobSystem.cpp', 'job implementation'],
  ['tests/JobSystemTests.cpp', 'job integration tests'],
  ['vcpkg.json', 'reproducible dependency manifest'],
  ['tools/portability/checklist-audit.mjs', 'checklist audit gate'],
  ['tools/portability/evidence-contract.mjs', 'evidence contract'],
  ['tools/portability/evidence-validate.mjs', 'evidence validator'],
  ['tools/portability/evidence-record.mjs', 'evidence record generator'],
  ['tools/portability/evidence-aggregate.mjs', 'evidence aggregate gate']
];
const cmake = fs.readFileSync(path.join(root, 'CMakeLists.txt'), 'utf8');
const failures = [];
if (!fs.existsSync(manifestPath)) failures.push('integration manifest is missing');
for (const [file, description] of required) {
  if (!fs.existsSync(path.join(root, file))) failures.push(`${description}: missing ${file}`);
}
const mustAppear = [
  ['JobSystem.cpp', 'JobSystem source is absent from build graph'],
  ['job_system_tests', 'job integration test target is absent from build graph'],
  ['gtest_discover_tests', 'GoogleTest discovery is not configured'],
  ['VC_USE_MIMALLOC', 'mimalloc option is not represented in CMake'],
  ['VC_USE_SPDLOG', 'spdlog option is not represented in CMake'],
  ['gtest_discover_tests', 'GoogleTest discovery is not configured']
];
for (const [needle, message] of mustAppear) if (!cmake.includes(needle)) failures.push(message);
if (!fs.existsSync(path.join(root, 'vcpkg.json'))) failures.push('vcpkg.json is missing');
if (failures.length) {
  console.error('integration-lint: FAIL');
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}
console.log(`integration-lint: PASS (${required.length} contracts and ${mustAppear.length} build invariants)`);
