import process from "node:process";

const numericFields = [
  "tick", "entities", "pendingEvents", "queuedQueries", "mappedAudioEvents",
  "routedEvents", "simulationSeconds", "aiTick", "renderCards",
  "navigationRevision", "invalidNavigationTiles", "loadedNavigationTiles",
  "semanticKinds"
];
const booleanFields = ["navigationBound", "debugBound", "authoringBound", "fullyBound", "bindingsComplete", "runtimeWiringComplete"];

export function readGameplayDebugSnapshot(jsonText) {
  const value = JSON.parse(jsonText);
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("invalid gameplay debug snapshot");
  }
  for (const field of numericFields) {
    if (typeof value[field] !== "number" || !Number.isFinite(value[field]) || value[field] < 0) {
      throw new Error(`invalid gameplay debug field: ${field}`);
    }
  }
  for (const field of booleanFields) {
    if (typeof value[field] !== "boolean") throw new Error(`invalid gameplay debug field: ${field}`);
  }
  if (value.fullyBound !== (value.navigationBound && value.debugBound && value.authoringBound &&
      value.bindingsComplete && value.runtimeWiringComplete)) {
    throw new Error("inconsistent gameplay debug binding state");
  }
  return Object.freeze({ ...value });
}

if (process.argv[1] && process.argv[1].endsWith("gameplay-debug.mjs")) {
  let input = "";
  process.stdin.setEncoding("utf8");
  process.stdin.on("data", chunk => { input += chunk; });
  process.stdin.on("end", () => {
    try {
      const snapshot = readGameplayDebugSnapshot(input);
      process.stdout.write(JSON.stringify({
        ok: true,
        tick: snapshot.tick,
        fullyBound: snapshot.fullyBound,
        bindingsComplete: snapshot.bindingsComplete,
        runtimeWiringComplete: snapshot.runtimeWiringComplete,
        routedEvents: snapshot.routedEvents
      }) + "\n");
    } catch (error) {
      process.stderr.write(error.message + "\n");
      process.exitCode = 1;
    }
  });
}
