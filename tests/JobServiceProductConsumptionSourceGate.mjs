#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const header = readFileSync('src/app/WorldProcgen.hpp', 'utf8');
const source = readFileSync('src/app/WorldProcgen.cpp', 'utf8');

assert.match(header, /engine\/procgen\/IJobService\.hpp/,
  'WorldProcgen must consume the public job-service contract');
assert.match(source, /create_job_service\s*\(/,
  'WorldProcgen must instantiate the public job service');
assert.match(source, /->start\s*\(/,
  'WorldProcgen init must start an observable job');
assert.match(source, /->update\s*\(/,
  'WorldProcgen init must publish progress');
assert.match(source, /->fail\s*\(/,
  'WorldProcgen init must mark failed initialization');
assert.match(source, /->complete\s*\(/,
  'WorldProcgen init must complete the job on success');
assert.match(source, /->poll\s*\(|->list\s*\(/,
  'WorldProcgen summary must observe the job state');

console.log('IJobService product-consumption source gate passed.');
