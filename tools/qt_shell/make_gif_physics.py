#!/usr/bin/env python
"""make_gif_physics.py — GIF de física (§C): cubos com rigidbody caem em play
mode; captura frames via /screenshot e monta GIF com PIL.

Uso: python make_gif_physics.py [--frames 18] [--out screenshots/physics_fall.gif]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=18)
    ap.add_argument("--out", default="screenshots/physics_fall.gif")
    args = ap.parse_args()

    try:
        st = json.loads(get("/state"))
    except Exception:
        print("editor fora do ar (sem /state)")
        sys.exit(1)
    print(f"editor ok: state={st.get('state')} fps={st.get('fps')}")

    # Abre a cena autosave (tem terreno renderizável + luz) e adiciona cubos
    # com rigidbody acima do chão. new-scene criaria cena vazia (frame 2KB).
    post("/open-scene?path=assets/scenes/autosave.scene")
    time.sleep(3)
    ids = []
    for i, y in enumerate((2.0, 3.5, 5.0)):
        post("/add-entity/cube")
        time.sleep(0.6)
        h = json.loads(get("/hierarchy"))
        hs = h.get("hierarchy", h) if isinstance(h, dict) else h
        if not hs:
            continue
        uid = hs[-1]["id"]
        ids.append(uid)
        post(f"/set-transform/{uid}/0/{y}/0")
        post(f"/add-component/{uid}/rigidbody")
        time.sleep(0.4)
    print(f"cubos com rigidbody: {len(ids)}")

    # Play mode: a física começa a valer (cubos caem).
    post("/play")
    time.sleep(1.0)
    os.makedirs(TMP, exist_ok=True)
    paths = []
    for i in range(args.frames):
        p = os.path.join(TMP, f"phys_{i:03d}.png")
        post(f"/screenshot?path=screenshots/_gif_tmp/phys_{i:03d}.png")
        time.sleep(0.28)
        if os.path.exists(p):
            paths.append(p)
    post("/stop")

    if len(paths) < 4:
        print("frames insuficientes")
        sys.exit(1)

    from PIL import Image
    imgs = [Image.open(p).convert("RGB") for p in paths]
    out = os.path.join(ENGINE, args.out)
    imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=140,
                 loop=0, optimize=False)
    print(f"GIF gravado: {out} ({os.path.getsize(out)} bytes, {len(imgs)} frames)")


if __name__ == "__main__":
    main()
