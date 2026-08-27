#!/usr/bin/env node
// otel-gate.mjs — §7 gate de utilização do opentelemetry-cpp vendido.
// Builda as libs MSVC (sdk trace/logs/metrics/common/resource/version +
// exporters otlp file/client/log/metric + proto/protobuf/absl/utf8) numa
// árvore de path curto (C:\\oteltmp — o populate do FetchContent estoura o
// limite do Windows na árvore profunda do repo) e compila+roda
// tools/portability/otel-probe.cpp: pipeline COMPLETO trace + log + métrica
// pelo OTLP **file** exporter (sem rede) com verificação do JSON gerado.
// Exit 0 = runtime de observabilidade vendido é utilizável SEM wiring da
// engine. Mesmo padrão dos gates immer/sqlite/libsodium/curl/tuf/luau/sentry.
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const SRC = join(ROOT, 'external', 'solutions', 'opentelemetry-cpp');
const TMP = 'C:\\oteltmp';

// 1. Ensure the short-path build tree exists (copy vendored source once).
if (!existsSync(join(TMP, 'CMakeLists.txt'))) {
  const cp = spawnSync('xcopy', ['/E', '/I', '/Q', '/Y', SRC + '\\', TMP + '\\'], {
    encoding: 'utf8',
    shell: true,
  });
  if (cp.status !== 0) {
    console.error('[otel-gate] FAIL: source copy error');
    console.error(cp.stdout + cp.stderr);
    process.exit(1);
  }
}

// 2. Configure the otel tree once (short path avoids MAX_PATH).
const CFG = join(TMP, 'build-gate');
if (!existsSync(join(CFG, 'CMakeCache.txt'))) {
  const cfg = spawnSync(
    'cmake',
    [
      '-S', TMP,
      '-B', CFG,
      '-G', 'Visual Studio 17 2022',
      '-A', 'x64',
      '-DWITH_OTLP_GRPC=OFF',
      '-DWITH_OTLP_HTTP=OFF',
      '-DWITH_OTLP_FILE=ON',
      '-DWITH_EXAMPLES=OFF',
      '-DWITH_BENCHMARK=OFF',
      '-DWITH_ZIPKIN=OFF',
      '-DWITH_PROMETHEUS=OFF',
      '-DWITH_ELASTICSEARCH=OFF',
      '-DWITH_OPENTRACING=OFF',
      '-DWITH_ETW=OFF',
      '-DWITH_ABSEIL=OFF',
      '-DBUILD_TESTING=OFF',
      '-DWITH_FUNC_TESTS=OFF',
    ],
    { encoding: 'utf8', cwd: TMP }
  );
  if (cfg.status !== 0) {
    console.error('[otel-gate] FAIL: configure error');
    console.error(cfg.stdout + cfg.stderr);
    process.exit(1);
  }
}

// 3. Build libs + probe under the VS18 environment (v145 toolset — the one
//    CMake resolves; BuildTools' MSBuild lacks v145).
const build = spawnSync(join(TMP, 'build-otel-libs.bat'), {
  encoding: 'utf8',
  shell: true,
  cwd: TMP,
});
process.stdout.write(build.stdout);
if (build.status !== 0) {
  console.error('[otel-gate] FAIL: lib build');
  process.stderr.write(build.stderr);
  process.exit(1);
}
const run = spawnSync(join(TMP, 'build-probe-2step.bat'), {
  encoding: 'utf8',
  shell: true,
  cwd: TMP,
});
if (run.stdout) process.stdout.write(run.stdout);
if (run.status !== 0) {
  console.error('[otel-gate] FAIL: probe build/link');
  if (run.stderr) process.stderr.write(run.stderr);
  process.exit(1);
}
const exe = spawnSync(join(TMP, 'otel-probe.exe'), { encoding: 'utf8', cwd: TMP, timeout: 30000 });
if (exe.stdout) process.stdout.write(exe.stdout);
if (exe.status !== 0) {
  console.error('[otel-gate] FAIL: probe run (status=' + exe.status + ')');
  if (exe.stderr) process.stderr.write(exe.stderr);
  process.exit(1);
}
console.log('[otel-gate] PASS — vendored opentelemetry-cpp usable (OTLP file exporter: trace+log+metric JSON verified)');
process.exit(0);
