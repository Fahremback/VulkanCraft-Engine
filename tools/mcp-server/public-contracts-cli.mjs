#!/usr/bin/env node
import { callPublicRuntimeTool } from "./contract-runtime.mjs";
import path from "node:path";
const root = path.resolve(import.meta.dirname, "..", "..");
const [command, contract] = process.argv.slice(2);
if (command === "list") console.log(JSON.stringify(callPublicRuntimeTool(root, "public_contracts", {}), null, 2));
else if (command === "read" && contract) console.log(JSON.stringify(callPublicRuntimeTool(root, "read_public_contract", { contract }), null, 2));
else { console.error("usage: public-contracts-cli.mjs list|read <relative-contract>"); process.exitCode = 2; }
