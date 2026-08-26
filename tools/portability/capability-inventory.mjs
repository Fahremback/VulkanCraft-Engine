#!/usr/bin/env node
// capability-inventory.mjs - automatic inventory of public capabilities
import fs from "node:fs";
import path from "node:path";

const PUBLIC = path.join(process.cwd(), "src", "engine", "public", "engine");
const domains = {};
function walk(dir) {
  if (!fs.existsSync(dir)) return;
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) { walk(p); continue; }
    if (!e.name.endsWith(".hpp")) continue;
    const rel = path.relative(PUBLIC, p).split(path.sep).join("/");
    const domain = rel.split("/")[0];
    const content = fs.readFileSync(p, "utf8");
    const contracts = (content.match(/class I[A-Z]w+/g) || []).map(s => s.slice(6));
    const factories = [];
    const re = /create_[a-z_]+/g;
    let m;
    while ((m = re.exec(content)) !== null) {
      if (content[m.index + m[0].length] === "(") factories.push(m[0]);
    }
    if (!domains[domain]) domains[domain] = { contracts: new Set(), factories: new Set(), headers: 0 };
    contracts.forEach(c => domains[domain].contracts.add(c));
    factories.forEach(f => domains[domain].factories.add(f));
    domains[domain].headers++;
  }
}
walk(PUBLIC);

console.log("# Capability Inventory (automatic)");
console.log("");
console.log("| Domain | Headers | Contracts | Factories |");
console.log("|--------|--------:|----------:|----------:|");
for (const [d, v] of Object.entries(domains).sort()) {
  console.log("| " + d + " | " + v.headers + " | " + v.contracts.size + " | " + v.factories.size + " |");
}
console.log("");
const totalHeaders = Object.values(domains).reduce((a, v) => a + v.headers, 0);
const totalContracts = Object.values(domains).reduce((a, v) => a + v.contracts.size, 0);
console.log("");
console.log("**TOTAL: " + totalHeaders + " headers, " + totalContracts + " contracts across " + Object.keys(domains).length + " domains.**");
