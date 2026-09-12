#!/usr/bin/env node
import assert from 'node:assert/strict';
import * as auditor from '../tools/portability/integration-auditor.mjs';

assert.equal(typeof auditor.detectConstantUnavailableFactories, 'function',
  'auditor must export detectConstantUnavailableFactories');
assert.equal(typeof auditor.detectParallelPublicContracts, 'function',
  'auditor must export detectParallelPublicContracts');
assert.equal(typeof auditor.detectNotImplementedLines, 'function',
  'auditor must export detectNotImplementedLines');
assert.equal(typeof auditor.textReferencesAnySymbol, 'function',
  'auditor must export textReferencesAnySymbol');
assert.equal(typeof auditor.propagatePublicConsumerZones, 'function',
  'auditor must export propagatePublicConsumerZones');
assert.equal(typeof auditor.isForwardingPublicHeader, 'function',
  'auditor must export isForwardingPublicHeader');
assert.equal(typeof auditor.resolveTransitivelyOwnedFactories, 'function',
  'auditor must export resolveTransitivelyOwnedFactories');

assert.equal(
  auditor.isForwardingPublicHeader(`#pragma once\n// compatibility include\n#include "engine/plugins/IPluginIsolation.hpp"\n`),
  true,
  'pure compatibility includes must not become standalone capabilities'
);
assert.equal(
  auditor.isForwardingPublicHeader(`#pragma once\nnamespace engine { struct IData { int value; }; }\n`),
  false,
  'headers with actual declarations must remain in the public inventory'
);

const unavailableSource = `
std::unique_ptr<IFoo> create_foo() {
  return nullptr;
}

std::unique_ptr<IBar> create_bar() {
  auto value = make_real_bar();
  return value;
}

bool RuntimeProvider::is_available() const noexcept {
  return false;
}
`;

const unavailable = auditor.detectConstantUnavailableFactories(unavailableSource);
assert.deepEqual(unavailable.factories, ['create_foo']);
assert.deepEqual(unavailable.availabilityMethods, ['RuntimeProvider::is_available']);

const duplicates = auditor.detectParallelPublicContracts([
  'IPluginIsolation',
  'IPluginIsolationManager',
  'IPluginManifest',
  'IPluginManifestCodec',
  'IWorldRuntime'
]);

assert.deepEqual(duplicates, [
  { base: 'IPluginManifest', contracts: ['IPluginManifest', 'IPluginManifestCodec'] }
]);

assert.deepEqual(
  auditor.detectNotImplementedLines('ok();\nerrorOut = "backend is not implemented yet";\nreturn false;'),
  [{ line: 2, snippet: 'errorOut = "backend is not implemented yet";' }]
);

assert.equal(
  auditor.textReferencesAnySymbol(
    'std::unique_ptr<engine::animation::IInertializer> animInertializer;',
    ['InertializerResult', 'IInertializer']
  ),
  true,
  'consumption must consider every contract symbol, not only the first one'
);
assert.equal(
  auditor.textReferencesAnySymbol('nothing relevant here', ['IFoo', 'IBar']),
  false
);

const transitiveRows = [
  {
    capability: 'IParent',
    symbols: ['IParent'],
    headerText: 'class IParent { virtual IChild& child() = 0; };',
    state: { CONSUMED: true, OBSERVABLE: true, consumerZones: ['Server'] }
  },
  {
    capability: 'IChild',
    symbols: ['IChild'],
    headerText: 'class IChild {};',
    state: { CONSUMED: false, OBSERVABLE: false, consumerZones: [] }
  }
];
auditor.propagatePublicConsumerZones(transitiveRows);
assert.equal(transitiveRows[1].state.CONSUMED, true);
assert.equal(transitiveRows[1].state.OBSERVABLE, true);
assert.deepEqual(transitiveRows[1].state.consumerZones, ['Server']);
assert.deepEqual(transitiveRows[1].state.consumerEvidence, ['public-dependency:IParent']);

const factoryComposition = auditor.resolveTransitivelyOwnedFactories(
  [
    { factory: 'create_child', kind: 'TEST-ONLY', sdkSites: 1 },
    { factory: 'create_root', kind: 'CONSUMED', sdkSites: 1 }
  ],
  [
    {
      capability: 'IChild',
      factories: ['create_child'],
      symbols: ['IChild'],
      state: { CONSUMED: true, consumerZones: ['Game'] }
    }
  ],
  [{
    path: 'src/engine/sdk/Root.cpp',
    text: `
      class Root { std::unique_ptr<IChild> child = create_child(error); };
      std::unique_ptr<IRoot> create_root() { return std::make_unique<Root>(); }
    `
  }],
  { Game: 'auto runtime = create_root();', Editor: '', Server: '' }
);
assert.deepEqual(factoryComposition, [{
  factory: 'create_child',
  capability: 'IChild',
  sdkSource: 'src/engine/sdk/Root.cpp',
  compositionRoots: ['create_root'],
  consumerZones: ['Game']
}]);

const deadFactoryComposition = auditor.resolveTransitivelyOwnedFactories(
  [
    { factory: 'create_child', kind: 'TEST-ONLY', sdkSites: 1 },
    { factory: 'create_root', kind: 'TEST-ONLY', sdkSites: 1 }
  ],
  [{
    capability: 'IChild',
    factories: ['create_child'],
    symbols: ['IChild'],
    state: { CONSUMED: true, consumerZones: ['Game'] }
  }],
  [{
    path: 'src/engine/sdk/Root.cpp',
    text: `
      class Root { std::unique_ptr<IChild> child = create_child(error); };
      std::unique_ptr<IRoot> create_root() { return std::make_unique<Root>(); }
    `
  }],
  { Game: '// create_root(); only a comment', Editor: '', Server: '' }
);
assert.deepEqual(deadFactoryComposition, [],
  'a leaf factory must stay unresolved when its owning root has no real product call');

console.log('Integration auditor detector policies passed.');
