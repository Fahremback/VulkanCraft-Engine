#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const main = readFileSync('src/app/main_game.cpp', 'utf8');
assert.match(main, /engine\/core\/memory\/Allocator\.hpp/,
  'game entrypoint must include the public allocator integration');
assert.match(main, /vc::alloc::install_global\s*\(\s*\)/,
  'game entrypoint must install the configured allocator before engine construction');

console.log('Allocator product-consumption source gate passed.');
