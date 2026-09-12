#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = readFileSync('src/engine/audio/AudioRuntime.cpp', 'utf8');
const start = source.indexOf('void AttenuationCurve::set_points');
const end = source.indexOf('void GainEffect::set_gain', start);
assert.ok(start >= 0 && end > start, 'attenuation implementation block missing');
const block = source.slice(start, end);

assert.doesNotMatch(block, /Placeholder for custom curve|case AttenuationModel::Custom:\s*return 1\.0f/s,
  'custom attenuation must evaluate authored points');
assert.match(block, /normalizedDistance/,
  'custom attenuation must operate in normalized distance space');
assert.match(block, /lower_bound|upper_bound|for\s*\(/,
  'custom attenuation must locate the surrounding authored points');
assert.match(block, /std::clamp|glm::clamp|std::min/,
  'custom attenuation must clamp authored/evaluated values');

console.log('Audio custom attenuation source gate passed.');
