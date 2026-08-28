import fs from "node:fs";
import path from "node:path";

export function contractCatalog(engineRoot) {
  const root = path.resolve(engineRoot, "src", "engine", "public", "engine");
  if (!fs.existsSync(root) || !fs.statSync(root).isDirectory()) throw new Error("public SDK include tree is unavailable");
  const rootReal = fs.realpathSync(root);
  const contracts = [];
  const walk = (dir, prefix = "") => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const absolute = path.join(dir, entry.name);
      const real = fs.realpathSync(absolute);
      const relativeReal = path.relative(rootReal, real);
      if (relativeReal.startsWith("..") || path.isAbsolute(relativeReal)) continue;
      const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(absolute, relative);
      else if (/^I[A-Za-z0-9_]+\.hpp$/.test(entry.name)) contracts.push(`src/engine/public/engine/${relative}`);
    }
  };
  walk(root);
  return contracts.sort();
}
export function publicRuntimeTools() { return [
  {name:"public_contracts",description:"List every public SDK interface discovered from the public include tree.",inputSchema:{type:"object",properties:{},additionalProperties:false}},
  {name:"read_public_contract",description:"Read one discovered public SDK interface.",inputSchema:{type:"object",required:["contract"],properties:{contract:{type:"string"}},additionalProperties:false}}
]; }
export function callPublicRuntimeTool(engineRoot,name,args={}) {
  const contracts=contractCatalog(engineRoot);
  if(name==="public_contracts")return{contracts,count:contracts.length,source:"src/engine/public/engine",generated:true};
  if(name!=="read_public_contract")return undefined;
  if(typeof args.contract!=="string"||!contracts.includes(args.contract))throw new Error("unknown public contract");
  const root=path.resolve(engineRoot,"src","engine","public","engine");
  const file=path.resolve(engineRoot,args.contract);const real=fs.realpathSync(file);const rel=path.relative(fs.realpathSync(root),real);
  if(rel.startsWith("..")||path.isAbsolute(rel)||!/^I[A-Za-z0-9_]+\.hpp$/.test(path.basename(real)))throw new Error("contract is outside the public SDK tree");
  return{contract:args.contract,content:fs.readFileSync(real,"utf8")};
}
