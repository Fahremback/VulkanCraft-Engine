// Temp probe: capture the actual build job shape for --config NopeConfig
// (fast-failing cmake, MSB8013, no compile). Deleted after use.
import { spawn } from 'child_process';
const c = spawn('node', ['tools/mcp-server/server.mjs'], { cwd: process.cwd(), stdio: ['pipe', 'pipe', 'inherit'] });
let buf = '';
let seq = 0;
const pending = new Map();
c.stdout.on('data', (d) => {
  buf += d;
  let idx;
  while ((idx = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, idx).trim();
    buf = buf.slice(idx + 1);
    if (!line) continue;
    const m = JSON.parse(line);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  }
});
const send = (method, params = {}) => new Promise((res) => {
  const id = ++seq;
  pending.set(id, res);
  c.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
});
(async () => {
  await send('initialize', { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'probe', version: '0' } });
  await send('notifications/initialized', {});
  const start = await send('tools/call', { name: 'start_build', arguments: { exe: 'VulkanEngineGame', config: 'NopeConfig' } });
  const job = JSON.parse(start.result.content[0].text);
  console.log('START:', job.status, job.log);
  let final = null;
  for (let i = 0; i < 80; i++) {
    await new Promise((r) => setTimeout(r, 500));
    const poll = await send('tools/call', { name: 'build_status', arguments: { job_id: job.job_id } });
    const p = JSON.parse(poll.result.content[0].text);
    if (p.status !== 'running') { final = p; break; }
  }
  console.log('FINAL status=', final.status, 'exit=', final.exit_code, 'exitCodeHex=', '0x' + (final.exit_code >>> 0).toString(16));
  console.log('FINAL tail:', (final.tail || '').slice(0, 400));
  console.log('FINAL artifacts:', JSON.stringify(final.artifacts));
  c.stdin.end();
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(1); });
