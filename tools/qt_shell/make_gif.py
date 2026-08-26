#!/usr/bin/env python
"""make_gif.py — gera o GIF hero do viewport (§C do plano Agente 2).

Sobe a câmera em passos de yaw via POST /turn/{delta}, captura um frame por
passo via POST /screenshot?path=..., e monta um GIF com PIL.

Uso: python make_gif.py [--frames 24] [--out screenshots/hero_viewport.gif]
"""
import argparse
import json
import os
import subprocess
import sys
import time

BASE = "http://127.0.0.1:8321"
# Script em <engine>/tools/qt_shell/make_gif.py -> raiz da engine é 3 níveis acima.
ENGINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def post(path):
    r = subprocess.run(["curl", "-s", "-m", "15", "-X", "POST", BASE + path],
                       capture_output=True, text=True)
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--out", default="screenshots/hero_viewport.gif")
    args = ap.parse_args()

    frames = []
    tmp = os.path.join(ENGINE, "screenshots", "_gif_tmp")
    os.makedirs(tmp, exist_ok=True)

    # Health check: editor no ar e fora do launcher.
    r = subprocess.run(["curl", "-s", "-m", "5", BASE + "/state"],
                       capture_output=True, text=True)
    try:
        st = json.loads(r.stdout)
    except Exception:
        print("editor fora do ar (sem /state)")
        sys.exit(1)
    print(f"editor ok: state={st.get('state')} fps={st.get('fps')}")

    step = 360 // args.frames
    paths = []
    for i in range(args.frames):
        # Gira a câmera e captura um frame. O comando /turn espera DOIS números
        # (yaw pitch); mandar 1 só falha silenciosamente e congela a câmera.
        post(f"/turn/{step}/0")
        time.sleep(0.35)
        p = os.path.join(tmp, f"frame_{i:03d}.png")
        post(f"/screenshot?path=screenshots/_gif_tmp/frame_{i:03d}.png")
        if os.path.exists(p):
            paths.append(p)
            print(f"  frame {i}: {os.path.getsize(p)} bytes")
        else:
            print(f"  frame {i}: MISSING")
    post("/turn/0")  # para de girar

    if len(paths) < 4:
        print("frames insuficientes")
        sys.exit(1)

    from PIL import Image
    imgs = [Image.open(p).convert("RGB") for p in paths]
    out = os.path.join(ENGINE, args.out)
    imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=120,
                 loop=0, optimize=False)
    print(f"GIF gravado: {out} ({os.path.getsize(out)} bytes, "
          f"{len(imgs)} frames)")


if __name__ == "__main__":
    main()
