#!/usr/bin/env node
// schema-key-fix.mjs — task A.4: the canonical schemas in schema/registry were
// written in snake_case but every real C++ runtime parser reads camelCase
// (verified against the source: ShaderCompiler.cpp, GlobalIllumination.cpp,
// FftOceanSurface.cpp, IFluidSimulation.hpp, IToneMapping.hpp, IVoxelStreaming.hpp,
// IWorldManager.hpp, Serializer.cpp). This rewrites the property keys to the
// exact camelCase the runtime consumes, so authored documents bind to real
// fields instead of being silently ignored.
//
//   node tools/portability/schema-key-fix.mjs [--check]
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const root = process.cwd();
const dir = join(root, 'schema', 'registry');

// snake -> camel mappings (each verified against a real C++ parser).
const MAPPINGS = {
  shader: { target_env: 'targetEnv', opt_level: 'optLevel' },
  gi: {
    cascade_count: 'cascadeCount', probes_per_frame: 'probesPerFrame',
    base_spacing: 'baseSpacing', cascade_scale: 'cascadeScale',
    sun_refresh_angle_degrees: 'sunRefreshAngleDegrees',
    max_distance: 'maxDistance'   // DiffuseGiConfig (IDiffuseGlobalIllumination.hpp)
  },
  ocean: {
    tile_size_meters: 'tileSizeMeters', wind_speed: 'windSpeed',
    wind_dir_rad: 'windDirRad'
  },
  fluid_sim: {
    grid_size: 'gridSize', cell_size: 'cellSize',
    solver_iterations: 'solverIterations', surface_tension: 'surfaceTension'
  },
  post_process: {
    use_ev: 'useEV', white_point: 'whitePoint', clamp_values: 'clampValues'
  },
  light: { cast_shadows: 'castShadows' },
  world: { rules_json: 'rulesJson', save_path: 'savePath' },
  chunk: {
    chunk_budget: 'chunkBudget', memory_budget_bytes: 'memoryBudgetBytes',
    far_lod_percent: 'farLodPercent', worker_threads: 'workerThreads'
  },
  transaction: { max_edits: 'maxEdits', max_box_volume: 'maxBoxVolume' },
  block_entity: {
    type_id: 'typeId', data_version: 'dataVersion', script_id: 'scriptId'
  }
};

const checkOnly = process.argv.includes('--check');
let changed = 0, errors = 0;
for (const file of readdirSync(dir).sort()) {
  if (!file.endsWith('.json')) continue;
  const kind = file.slice(0, -5);
  const map = MAPPINGS[kind];
  if (!map) continue; // no snake->camel keys to fix for this kind
  const p = join(dir, file);
  const doc = JSON.parse(readFileSync(p, 'utf8'));
  const props = doc.properties || {};
  let localChanged = 0;
  for (const [snake, camel] of Object.entries(map)) {
    if (Object.prototype.hasOwnProperty.call(props, snake)) {
      props[camel] = props[snake];
      delete props[snake];
      localChanged++;
    }
  }
  if (localChanged > 0) {
    // rewrite required[] entries that referenced a renamed key
    const renamed = new Map(Object.entries(map).filter(([s]) => !props[s]));
    if (Array.isArray(doc.required)) {
      doc.required = doc.required.map((r) => (renamed.get(r) ?? r));
    }
    if (!checkOnly) writeFileSync(p, JSON.stringify(doc, null, 2) + '\n');
    changed++;
    console.log(`${file}: renamed ${localChanged} keys to camelCase`);
  }
}
console.log(`\n${checkOnly ? '[check]' : '[fix]'} ${changed}/${MAPPINGS ? Object.keys(MAPPINGS).length : 0} schema kinds with snake_case -> camelCase corrections`);
process.exit(errors ? 1 : 0);