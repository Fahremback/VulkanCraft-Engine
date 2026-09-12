#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = readFileSync('src/tools/BuildPipeline.cpp', 'utf8');
const section = (name, next) => {
  const start = source.indexOf(`bool BuildPipeline::${name}`);
  const end = next ? source.indexOf(`bool BuildPipeline::${next}`, start) : source.length;
  assert.ok(start >= 0 && end > start, `${name} definition missing`);
  return source.slice(start, end);
};

const shaders = section('stage_compile_shaders', 'stage_cook_assets');
assert.doesNotMatch(shaders, /Compiled 3 shader permutations|standard_pbr\.vert\.spv/,
  'shader stage must not fake compilation with a hardcoded manifest');
assert.match(shaders, /dev-shared/,
  'shader stage must consume the canonical shared shader output');
assert.match(shaders, /copy_file\s*\(/,
  'shader stage must stage real SPIR-V bytes');

const deps = section('stage_copy_dependencies', 'stage_generate_distributable');
assert.doesNotMatch(deps, /deps\.txt/,
  'dependency stage must copy runtime libraries, not write a text list');
assert.match(deps, /copy_file\s*\(/,
  'dependency stage must stage real runtime dependency files');

const dist = section('stage_generate_distributable', null);
assert.match(dist, /build_path\(\)\s*\/\s*"Content"/,
  'distributable must include cooked Content, not only the package manifest');
assert.match(dist, /content\.pkg|Package/,
  'distributable must retain package metadata alongside cooked content');

console.log('BuildPipeline packaging source gate passed.');
