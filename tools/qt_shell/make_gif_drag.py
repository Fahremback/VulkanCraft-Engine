#!/usr/bin/env python
"""make_gif_drag.py — GIF de drag-drop (§C): simula o arrasto de gizmo move
(movendo a posição de um cubo via set-transform, o mesmo PATCH que o gizmo
aplica) e captura frames por passo via /screenshot.

Uso: python make_gif_drag.py [--frames 16] [--out screenshots/drag_move.gif]
"""
import argparse
import json
import os
import subprocess
import sys
import time

BASE = "http://127.0.0.1:8321"
ENGINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TMP = os.path.join(ENGINE, "screenshots", "_gif_tmp")


def post(path, timeout=15):
    return subprocess.run(["curl", "-s", "-m", str(timeout), "-X", "POST",
                           BASE + path], capture_output=True, text=True).stdout


def get(path):
    return subprocess.run(["curl", "-s", "-m", "5", BASE + path],
                          capture_output=True, text=True).stdout


def entity_ids():
    h = json.loads(get("/hierarchy"))
    rows = h.get("hierarchy", h) if isinstance(h, dict) else h
    return [r["id"] for r in rows] if rows else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--out", default="screenshots/drag_move.gif")
    args = ap.parse_args()

    try:
        st = json.loads(get("/state"))
    except Exception:
        print("editor fora do ar (sem /state)")
        sys.exit(1)
    print(f"editor ok: state={st.get('state')} fps={st.get('fps')}")

    # Cena com terreno (autosave) + um cubo "arrastável" sem física.
    post("/open-scene?path=assets/scenes/autosave.scene")
    time.sleep(3)
    post("/add-entity/cube")
    time.sleep(1.5)
    ids = entity_ids()
    if not ids:
        print("sem entidades")
        sys.exit(1)
    cube = ids[-1]
    # Modo gizmo move (visualmente correto) e gizmo world.
    post("/gizmo/move")
    post("/gizmo-space/world")
    print(f"cubo para arrastar: {cube}")

    # Arrasto: gizmo translate de x=-3 -> x=+3 (passos lineares = drag real).
    os.makedirs(TMP, exist_ok=True)
    paths = []
    for i in range(args.frames):
        t = i / max(args.frames - 1, 1)
        x = -3.0 + 6.0 * t
        post(f"/set-transform/{cube}/100 {x:.3f} 0.8 0 0 0 0 1 1 1")
        time.sleep(0.35)
        p = os.path.join(TMP, f"drag_{i:03d}.png")
        post(f"/screenshot?path=screenshots/_gif_tmp/drag_{i:03d}.png")
        if os.path.exists(p):
            paths.append(p)
    post("/gizmo/select")

    if len(paths) < 4:
        print("frames insuficientes")
        sys.exit(1)

    from PIL import Image
    imgs = [Image.open(p).convert("RGB") for p in paths]
    out = os.path.join(ENGINE, args.out)
    imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=130,
                 loop=0, optimize=False)
    print(f"GIF gravado: {out} ({os.path.getsize(out)} bytes, {len(imgs)} frames)")


if __name__ == "__main__":
    main()
