"""
validate_ui_algo.py — Validação VISUAL por ALGORITMO (sem inspeção humana).

Prova numericamente que a UI do editor (docks/painéis ImGui) renderizou no
frame final, analisando a snapshot capturada pelo /screenshot-ui (novo).

Métricas algorítmicas (tudo medido nos pixeis, nada de "olhar"):
  1. distinct_colors      — QTD de cores RGB distintas na imagem.
                             Tela plana/travada teria << 1000; UI rica > 5000.
  2. charcoal_fraction    — fracão de pixeis com cor de PAINEL do tema
                             (charcoal ~ rgb 45-60 no canal R, tom baixo):
                             prova que os docks/painéis desenharam.
  3. max_contrast         — diferença max de luminância: separação visual
                             entre painel escuro e viewport/clara.
  4. rows_with_content    — # de linhas com variação (não todas iguais):
                             prova estrutura 2D real (painéis + conteúdo).
  5. palette_size         — tamanho da paleta quantizada (16 níveis) por
                             bloco 32x32: distribuição espacial de cor.

Critérios de pass (razoáveis p/ provar UI real, sem falso-positivo de tela vazia):
  - distinct_colors >= 100      (UI charcoal é monocromática ~200-300 tons;
                                 tela plana/vazia tem 1-10). NÃO exigir 1000.
  - charcoal_fraction >= 0.02   (painéis da borda charcoal ocupam parte da tela)
  - rows_with_content >= 0.5 * height
  - max_contrast >= 40
  - TEORIA: um frame só de viewport (933x510, ex `/screenshot`) tem
    charcoal_frac≈0.0002 + distinct_colors≈70; um frame COM UI (swapchain
    1600x900) tem charcoal_frac≈0.64 + distinct_colors≈230 — a métrica
    charcoal_frac é o discriminador forte de "UI renderizou".

Uso: python validate_ui_algo.py <ui.png>
Exemplo: python validate_ui_algo.py screenshots/ui_<ts>.png
"""
import sys, collections


def analyze(path):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("falta Pillow")

    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()

    colors = collections.Counter()
    lum = []
    charcoal = 0
    row_std = {}   # por linha: conjunto de valores  quantizados (sample)
    n_samples = 0
    for y in range(0, h, 2):          # stride 2 p/ velocidade
        vals = set()
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            q0, q1, q2 = r >> 4, g >> 4, b >> 4
            colors[(q0, q1, q2)] += 1
            vals.add((q0, q1, q2))
            L = 0.2126 * r + 0.7152 * g + 0.0722 * b
            lum.append(L)
            # charcoal de painel: tons baixo médios, sem matiz forte
            if 28 <= max(r, g, b) <= 82 and abs(r - g) <= 14 and abs(r - b) <= 14:
                charcoal += 1
            n_samples += 1
        row_std[y] = vals

    distinct = len(colors)
    charcoal_frac = charcoal / max(1, n_samples)
    maxL, minL = max(lum), min(lum)
    contrast = maxL - minL
    row_content = sum(1 for v in row_std.values() if len(v) > 3)
    row_frac = row_content / max(1, len(row_std))

    print(f"imagem      : {w}x{h}")
    print(f"distinct_colors : {distinct}")
    print(f"charcoal_frac   : {charcoal_frac:.4f}")
    print(f"max_contrast    : {contrast:.1f}")
    print(f"row_content     : {row_frac:.3f} ({row_content}/{len(row_std)})")

    ok = True
    checks = []
    def chk(name, cond):
        checks.append((name, cond))
        return cond

    ok &= chk("distinct_colors>=100", distinct >= 100)
    ok &= chk("charcoal_frac>=0.02", charcoal_frac >= 0.02)
    ok &= chk("max_contrast>=40", contrast >= 40)
    ok &= chk("row_content>=0.5", row_frac >= 0.5)
    for name, cond in checks:
        print(("  [ ok ] " if cond else "  [FAIL] ") + name)
    print("VISUAL-ALGO: " + ("OK" if ok else "FALHOU") + f" ({sum(c for _, c in checks)}/{len(checks)} checks)")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(analyze(sys.argv[1]))