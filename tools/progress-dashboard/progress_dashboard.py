#!/usr/bin/env python3
"""Painel local de progresso dos seis agentes, sem dependências externas."""

from __future__ import annotations

import argparse
import json
import os
import re
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ENGINE_ROOT = Path(__file__).resolve().parents[2]
AGENTS_ROOT = ENGINE_ROOT / "agentes"
AGENT_LABELS = {
    "agente1_render_lumen": "Render e Lumen",
    "agente2_editor_ui": "Editor e UI",
    "agente3_voxel_world": "Voxel e Mundo",
    "agente4_gameplay_ai": "Gameplay e IA",
    "agente5_sdk_mcp": "SDK e MCP",
    "agente6_integracao": "Integração e Red Team",
}
DONE_RE = re.compile(r"^\s*-\s*\[[xX]\]", re.MULTILINE)
OPEN_RE = re.compile(r"^\s*-\s*\[(?:\s|~)\]", re.MULTILINE)
SUCCESSOR_MARKER = "<!-- AUTO-SUCCESSOR:AGENT6-GAMEPLAY-AI -->"
SUCCESSOR_TEMPLATE = AGENTS_ROOT / "agente6_integracao" / "successor_gameplay_ai.md"
def activate_agent6_successor() -> bool:
    """Anexa a missão sucessora uma única vez quando o lote atual chega a 100%."""
    plan = AGENTS_ROOT / "agente6_integracao" / "task_plan.md"
    if not plan.exists() or not SUCCESSOR_TEMPLATE.exists():
        return False
    current = plan.read_text(encoding="utf-8", errors="replace")
    if SUCCESSOR_MARKER in current:
        return False
    done = len(DONE_RE.findall(current))
    pending = len(OPEN_RE.findall(current))
    if done == 0 or pending != 0:
        return False

    lock = plan.with_suffix(".successor.lock")
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        return False
    try:
        os.close(descriptor)
        current = plan.read_text(encoding="utf-8", errors="replace")
        if SUCCESSOR_MARKER in current or len(OPEN_RE.findall(current)) != 0:
            return False
        successor = SUCCESSOR_TEMPLATE.read_text(encoding="utf-8", errors="replace").strip()
        with plan.open("a", encoding="utf-8", newline="\n") as output:
            output.write(f"\n\n{SUCCESSOR_MARKER}\n{successor}\n")
        return True
    finally:
        lock.unlink(missing_ok=True)


def snapshot() -> dict:
    activated = activate_agent6_successor()
    agents = []
    for directory, label in AGENT_LABELS.items():
        plan = AGENTS_ROOT / directory / "task_plan.md"
        text = plan.read_text(encoding="utf-8", errors="replace") if plan.exists() else ""
        done = len(DONE_RE.findall(text))
        pending = len(OPEN_RE.findall(text))
        total = done + pending
        percent = round((done / total * 100.0) if total else 0.0, 1)
        agents.append(
            {
                "id": directory,
                "name": label,
                "done": done,
                "pending": pending,
                "total": total,
                "percent": percent,
                "almost": 80.0 <= percent < 100.0,
                "complete": total > 0 and done == total,
                "updated": int(plan.stat().st_mtime) if plan.exists() else 0,
            }
        )

    done = sum(agent["done"] for agent in agents)
    total = sum(agent["total"] for agent in agents)
    return {
        "done": done,
        "pending": total - done,
        "total": total,
        "percent": round((done / total * 100.0) if total else 0.0, 1),
        "agents": agents,
        "timestamp": int(time.time()),
        "successor_activated": activated,
    }


PAGE = r"""<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>VulkanCraft — Progresso</title>
<style>
:root{color-scheme:dark;--bg:#080b12;--panel:#111725;--line:#263149;--text:#edf4ff;--muted:#8d9bb2;--green:#35dc87;--blue:#5d8cff;--yellow:#ffbf47}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 50% -20%,#1b2a49 0,#080b12 45%);font:15px Inter,Segoe UI,sans-serif;color:var(--text);min-height:100vh}
main{width:min(1100px,calc(100% - 32px));margin:auto;padding:42px 0}.top{display:flex;justify-content:space-between;align-items:end;gap:20px;margin-bottom:24px}
h1{font-size:28px;margin:0 0 6px}.muted{color:var(--muted)}.live{display:flex;align-items:center;gap:8px;color:var(--green);font-weight:700}.dot{width:9px;height:9px;border-radius:50%;background:var(--green);box-shadow:0 0 15px var(--green)}
.hero,.card{background:linear-gradient(145deg,rgba(22,30,47,.96),rgba(12,17,28,.96));border:1px solid var(--line);border-radius:18px;box-shadow:0 20px 60px #0005}
.hero{padding:26px;margin-bottom:18px}.hero-row{display:flex;align-items:end;justify-content:space-between}.big{font-size:54px;font-weight:800;letter-spacing:-3px}.count{font-size:17px;color:var(--muted);padding-bottom:8px}
.bar{height:13px;background:#070a10;border-radius:20px;overflow:hidden;margin-top:20px;border:1px solid #202a3d}.fill{height:100%;border-radius:inherit;background:linear-gradient(90deg,var(--blue),var(--green));transition:width .5s ease}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}.card{padding:20px}.card-head{display:flex;justify-content:space-between;align-items:center}.name{font-size:17px;font-weight:750}.pct{font-size:22px;font-weight:800}.mini{height:8px;margin:15px 0 12px}.stats{display:flex;justify-content:space-between;color:var(--muted);font-size:13px}.badge{color:var(--yellow);font-weight:800;font-size:12px;margin-left:8px}.complete{color:var(--green)}
.changed{animation:flash .8s ease}@keyframes flash{50%{border-color:var(--green);box-shadow:0 0 30px #35dc8733}}@media(max-width:700px){.grid{grid-template-columns:1fr}.big{font-size:42px}.top{align-items:start;flex-direction:column}}
</style></head>
<body><main><div class="top"><div><h1>VulkanCraft Engine</h1><div class="muted">Progresso dos seis agentes em tempo real</div></div><div class="live"><span class="dot"></span>MONITORANDO</div></div>
<section class="hero"><div class="hero-row"><div class="big" id="overall">0%</div><div class="count" id="count">Carregando…</div></div><div class="bar"><div class="fill" id="overall-bar"></div></div></section>
<section class="grid" id="agents"></section><p class="muted" id="updated"></p></main>
<script>
let previous={};
function card(a,index){const changed=previous[a.id]!==undefined&&previous[a.id]!==a.done;previous[a.id]=a.done;const badge=a.complete?'<span class="badge complete">CONCLUÍDO</span>':a.almost?'<span class="badge">QUASE</span>':'';return `<article class="card ${changed?'changed':''}"><div class="card-head"><div><span class="name">Agente ${index+1} — ${a.name}</span>${badge}</div><span class="pct">${a.percent.toFixed(1)}%</span></div><div class="bar mini"><div class="fill" style="width:${a.percent}%"></div></div><div class="stats"><span>${a.done} concluídas</span><span>${a.pending} pendentes</span><span>${a.total} total</span></div></article>`}
async function update(){try{const r=await fetch('/api/progress',{cache:'no-store'});const d=await r.json();document.querySelector('#overall').textContent=d.percent.toFixed(1)+'%';document.querySelector('#overall-bar').style.width=d.percent+'%';document.querySelector('#count').textContent=`${d.done} de ${d.total} tarefas • ${d.pending} pendentes`;document.querySelector('#agents').innerHTML=d.agents.map(card).join('');document.querySelector('#updated').textContent='Atualizado às '+new Date(d.timestamp*1000).toLocaleTimeString('pt-BR');}catch(e){document.querySelector('#updated').textContent='Reconectando…'}}
update();setInterval(update,1000);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path.startswith("/api/progress"):
            body = json.dumps(snapshot(), ensure_ascii=False).encode("utf-8")
            content_type = "application/json; charset=utf-8"
        elif self.path in ("/", "/index.html"):
            body = PAGE.encode("utf-8")
            content_type = "text/html; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        return


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765)
    parser.add_argument("--snapshot", action="store_true")
    args = parser.parse_args()
    if args.snapshot:
        print(json.dumps(snapshot(), ensure_ascii=False))
        return
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{args.port}"
    threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    print(f"Monitor ativo em {url} — pressione Ctrl+C para encerrar.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
