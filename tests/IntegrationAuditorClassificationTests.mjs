#!/usr/bin/env node
import assert from 'node:assert/strict';
import * as auditor from '../tools/portability/integration-auditor.mjs';

assert.equal(typeof auditor.validateStructuredClassification, 'function',
  'auditor must export validateStructuredClassification');

const row = {
  capability: 'IHashProvider',
  symbols: ['IHashProvider', 'create_blake3_hash_provider']
};

const good = auditor.validateStructuredClassification(
  row,
  {
    classification: 'internal',
    justification: 'Privately composed by the world persistence implementation.',
    evidence: ['src/engine/sdk/VoxelWorldFacade.cpp']
  },
  (path) => path.endsWith('VoxelWorldFacade.cpp')
    ? 'std::shared_ptr<engine::hashing::IHashProvider> hash_; create_blake3_hash_provider();'
    : null
);
assert.equal(good.valid, true);
assert.equal(good.classification, 'internal');

const badNoEvidence = auditor.validateStructuredClassification(
  row,
  {
    classification: 'internal',
    justification: 'A comment claims this is internal.',
    evidence: []
  },
  () => null
);
assert.equal(badNoEvidence.valid, false);

const badClass = auditor.validateStructuredClassification(
  row,
  {
    classification: 'external-sdk-only',
    justification: 'Unsupported exemption class.',
    evidence: ['x.cpp']
  },
  () => 'IHashProvider'
);
assert.equal(badClass.valid, false);

console.log('Integration auditor structured classification policy passed.');
