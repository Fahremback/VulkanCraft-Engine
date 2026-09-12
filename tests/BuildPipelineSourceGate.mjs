#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = readFileSync('src/tools/BuildPipeline.cpp', 'utf8');
const start = source.indexOf('bool BuildPipeline::stage_build_executable');
const end = source.indexOf('bool BuildPipeline::stage_copy_dependencies', start);
assert.ok(start >= 0 && end > start, 'stage_build_executable definition missing');
const body = source.slice(start, end);

assert.doesNotMatch(body, /executable stub|VC executable stub/i,
  'BuildPipeline must never manufacture a text .exe stub');
assert.match(body, /VulkanEngineGame(?:\.exe)?/,
  'BuildPipeline must stage the canonical VulkanEngineGame binary');
assert.match(body, /copy_file\s*\(/,
  'BuildPipeline must copy native executable bytes into the project distributable');
assert.match(body, /is_regular_file|exists\s*\(/,
  'BuildPipeline must reject a missing canonical native executable');

console.log('BuildPipeline native executable source gate passed.');
