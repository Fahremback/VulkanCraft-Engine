#!/usr/bin/env python3
"""Coordenador visual para renovar sessões encerradas em terminais mapeados."""

from __future__ import annotations

import ctypes
import difflib
import json
import queue
import threading
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Callable
from ctypes import wintypes

import numpy as np
import pyautogui
import tkinter as tk
from PIL import ImageGrab
from rapidocr_onnxruntime import RapidOCR
from tkinter import messagebox, simpledialog, ttk


APP_DIR = Path(__file__).resolve().parent
CONFIG_PATH = APP_DIR / "coordinator_config.json"
pyautogui.FAILSAFE = True
pyautogui.PAUSE = 0.08


def enable_dpi_awareness() -> None:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def virtual_screen() -> tuple[int, int, int, int]:
    user32 = ctypes.windll.user32
    return (
        user32.GetSystemMetrics(76),
        user32.GetSystemMetrics(77),
        user32.GetSystemMetrics(78),
        user32.GetSystemMetrics(79),
    )


@dataclass
class WatchArea:
    name: str
    left: int
    top: int
    right: int
    bottom: int
    enabled: bool = True
    armed: bool = True
    misses: int = 0
    triggers: int = 0
    attempts: int = 0
    last_trigger: float = 0.0

    @property
    def bbox(self) -> tuple[int, int, int, int]:
        return self.left, self.top, self.right, self.bottom


class RegionSelector(tk.Toplevel):
    def __init__(self, parent: tk.Tk, title: str, callback: Callable[[tuple[int, int, int, int]], None]):
        super().__init__(parent)
        self.callback = callback
        self.origin_x, self.origin_y, width, height = virtual_screen()
        self.geometry(f"{width}x{height}{self.origin_x:+d}{self.origin_y:+d}")
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.attributes("-alpha", 0.28)
        self.configure(bg="#050914")
        self.canvas = tk.Canvas(self, bg="#050914", highlightthickness=0, cursor="crosshair")
        self.canvas.pack(fill="both", expand=True)
        self.canvas.create_text(
            width // 2,
            36,
            text=f"{title}  •  arraste para selecionar  •  ESC cancela",
            fill="white",
            font=("Segoe UI", 16, "bold"),
        )
        self.start: tuple[int, int] | None = None
        self.rectangle: int | None = None
        self.canvas.bind("<ButtonPress-1>", self._press)
        self.canvas.bind("<B1-Motion>", self._drag)
        self.canvas.bind("<ButtonRelease-1>", self._release)
        self.bind("<Escape>", lambda _event: self._cancel())
        self.focus_force()
        self.grab_set()

    def _press(self, event: tk.Event) -> None:
        self.start = (event.x, event.y)
        self.rectangle = self.canvas.create_rectangle(event.x, event.y, event.x, event.y, outline="#35dc87", width=4)

    def _drag(self, event: tk.Event) -> None:
        if self.start and self.rectangle:
            self.canvas.coords(self.rectangle, self.start[0], self.start[1], event.x, event.y)

    def _release(self, event: tk.Event) -> None:
        if not self.start:
            return
        x1, y1 = self.start
        x2, y2 = event.x, event.y
        left, right = sorted((x1 + self.origin_x, x2 + self.origin_x))
        top, bottom = sorted((y1 + self.origin_y, y2 + self.origin_y))
        if right - left < 20 or bottom - top < 12:
            messagebox.showwarning("Seleção pequena", "Selecione uma área maior.", parent=self)
            self._cancel()
            return
        self.grab_release()
        self.destroy()
        self.callback((left, top, right, bottom))

    def _cancel(self) -> None:
        self.grab_release()
        self.destroy()
        self.master.deiconify()


class CoordinatorApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("VulkanCraft — Coordenador de Sessões")
        self.root.geometry("860x590")
        self.root.minsize(760, 500)
        self.root.configure(bg="#0b101b")
        self.areas: list[WatchArea] = []
        self.phrase = "Press Enter to continue in a new session"
        self.ocr: RapidOCR | None = None
        self.running = False
        self.worker: threading.Thread | None = None
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.threshold = tk.DoubleVar(value=0.78)
        self.interval = tk.DoubleVar(value=0.50)
        self.cooldown = tk.DoubleVar(value=8.0)
        self.focus_delay = tk.DoubleVar(value=1.0)
        self.minimize_while_running = tk.BooleanVar(value=True)
        self.scan_threshold = 0.78
        self.scan_interval = 0.50
        self.scan_cooldown = 8.0
        self.scan_focus_delay = 1.0
        self.status = tk.StringVar(value="Parado")
        self._load()
        self._build_ui()
        self._refresh_table()
        self.root.after(100, self._drain_events)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    def _build_ui(self) -> None:
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Treeview", background="#111827", fieldbackground="#111827", foreground="#e8eef9", rowheight=30)
        style.configure("Treeview.Heading", background="#1d2738", foreground="#e8eef9", relief="flat")
        style.map("Treeview", background=[("selected", "#315fd6")])

        header = tk.Frame(self.root, bg="#111827", padx=20, pady=16)
        header.pack(fill="x")
        tk.Label(header, text="Coordenador de Sessões", bg="#111827", fg="white", font=("Segoe UI", 19, "bold")).pack(side="left")
        tk.Label(header, textvariable=self.status, bg="#111827", fg="#35dc87", font=("Segoe UI", 11, "bold")).pack(side="right")

        actions = tk.Frame(self.root, bg="#0b101b", padx=16, pady=14)
        actions.pack(fill="x")
        self.start_button = tk.Button(actions, text="▶ INICIAR", command=self.toggle, bg="#25b86f", fg="white", activebackground="#1d955a", relief="flat", padx=18, pady=9, font=("Segoe UI", 10, "bold"))
        self.start_button.pack(side="left", padx=(0, 8))
        tk.Button(actions, text="Definir frase", command=self.calibrate, bg="#315fd6", fg="white", relief="flat", padx=14, pady=9).pack(side="left", padx=4)
        tk.Button(actions, text="+ Adicionar área", command=self.add_area, bg="#242f43", fg="white", relief="flat", padx=14, pady=9).pack(side="left", padx=4)
        tk.Button(actions, text="Remover", command=self.remove_selected, bg="#642d3a", fg="white", relief="flat", padx=14, pady=9).pack(side="left", padx=4)

        table_frame = tk.Frame(self.root, bg="#0b101b", padx=16)
        table_frame.pack(fill="both", expand=True)
        self.table = ttk.Treeview(table_frame, columns=("name", "region", "state", "confidence", "triggers"), show="headings", selectmode="browse")
        for column, text, width in (("name", "Área", 190), ("region", "Região", 230), ("state", "Estado", 120), ("confidence", "Confiança", 110), ("triggers", "Enters", 70)):
            self.table.heading(column, text=text)
            self.table.column(column, width=width, anchor="center" if column != "name" else "w")
        self.table.pack(fill="both", expand=True)
        self.table.bind("<Double-1>", self.toggle_selected)

        settings = tk.LabelFrame(self.root, text=" Ajustes ", bg="#0b101b", fg="#9eacc3", padx=12, pady=10)
        settings.pack(fill="x", padx=16, pady=12)
        self._setting(settings, "Similaridade mínima", self.threshold, 0.55, 0.99, 0.01, 0)
        self._setting(settings, "Intervalo (s)", self.interval, 0.25, 3.0, 0.05, 1)
        self._setting(settings, "Proteção entre Enters (s)", self.cooldown, 2.0, 60.0, 1.0, 2)
        self._setting(settings, "Espera após clique (s)", self.focus_delay, 0.5, 3.0, 0.1, 3)
        tk.Checkbutton(
            settings,
            text="Minimizar enquanto monitora",
            variable=self.minimize_while_running,
            bg="#0b101b",
            fg="#d6deeb",
            activebackground="#0b101b",
            activeforeground="white",
            selectcolor="#151d2b",
        ).grid(row=1, column=0, columnspan=4, sticky="w", padx=12, pady=(12, 0))
        help_text = "1) Digite a frase que deve ser detectada.  2) Adicione cada terminal selecionando a região onde o aviso aparece.  3) Inicie.  Duplo clique ativa/desativa uma área."
        tk.Label(self.root, text=help_text, bg="#0b101b", fg="#7f8da5", wraplength=820, justify="left", padx=16, pady=8).pack(fill="x")

    def _setting(self, parent: tk.Widget, label: str, variable: tk.DoubleVar, start: float, end: float, step: float, column: int) -> None:
        frame = tk.Frame(parent, bg="#0b101b")
        frame.grid(row=0, column=column, padx=12, sticky="ew")
        tk.Label(frame, text=label, bg="#0b101b", fg="#d6deeb").pack(anchor="w")
        tk.Spinbox(frame, from_=start, to=end, increment=step, textvariable=variable, width=10, bg="#151d2b", fg="white", buttonbackground="#273249", relief="flat").pack(anchor="w", pady=(4, 0))
        parent.grid_columnconfigure(column, weight=1)

    def _select_region(self, title: str, callback: Callable[[tuple[int, int, int, int]], None]) -> None:
        self.root.withdraw()
        self.root.after(350, lambda: RegionSelector(self.root, title, callback))

    def calibrate(self) -> None:
        if self.running:
            self.toggle()
        phrase = simpledialog.askstring(
            "Frase a monitorar",
            "Digite a frase que fará o coordenador clicar e pressionar Enter:",
            initialvalue=self.phrase,
            parent=self.root,
        )
        if phrase and phrase.strip():
            self.phrase = phrase.strip()
            self.status.set(f"Frase: {self.phrase}")
            self._save()

    def add_area(self) -> None:
        if self.running:
            self.toggle()

        def selected(bbox: tuple[int, int, int, int]) -> None:
            self.root.deiconify()
            name = simpledialog.askstring("Nome da área", "Nome deste terminal:", initialvalue=f"Agente {len(self.areas) + 1}", parent=self.root)
            if name:
                self.areas.append(WatchArea(name=name, left=bbox[0], top=bbox[1], right=bbox[2], bottom=bbox[3]))
                self._save()
                self._refresh_table()

        self._select_region("Selecione a área do terminal que deve ser monitorada", selected)

    def remove_selected(self) -> None:
        selected = self.table.selection()
        if not selected:
            return
        index = int(selected[0])
        del self.areas[index]
        self._save()
        self._refresh_table()

    def toggle_selected(self, _event: tk.Event | None = None) -> None:
        selected = self.table.selection()
        if not selected:
            return
        area = self.areas[int(selected[0])]
        area.enabled = not area.enabled
        area.armed = area.enabled
        self._save()
        self._refresh_table()

    def toggle(self) -> None:
        if self.running:
            self.running = False
            self.status.set("Pausando…")
            self.start_button.configure(text="▶ INICIAR", bg="#25b86f")
            return
        if not self.phrase.strip():
            messagebox.showwarning("Frase necessária", "Defina a frase antes de iniciar.")
            return
        if not any(area.enabled for area in self.areas):
            messagebox.showwarning("Sem áreas", "Adicione pelo menos uma área de monitoramento.")
            return
        self.running = True
        self.scan_threshold = float(self.threshold.get())
        self.scan_interval = float(self.interval.get())
        self.scan_cooldown = float(self.cooldown.get())
        self.scan_focus_delay = float(self.focus_delay.get())
        if self.ocr is None:
            self.status.set("Carregando leitor de texto…")
            self.root.update_idletasks()
            self.ocr = RapidOCR()
        self.status.set("MONITORANDO")
        self.start_button.configure(text="■ PARAR", bg="#d34854")
        self.worker = threading.Thread(target=self._monitor, daemon=True)
        self.worker.start()
        if self.minimize_while_running.get():
            self.root.after(300, self.root.iconify)

    def _monitor(self) -> None:
        while self.running:
            threshold = self.scan_threshold
            now = time.time()
            for index, area in enumerate(self.areas):
                if not self.running or not area.enabled:
                    continue
                try:
                    screen = np.array(ImageGrab.grab(bbox=area.bbox, all_screens=True))
                    result, _timings = self.ocr(screen) if self.ocr else (None, None)
                    recognized = " ".join(str(line[1]) for line in (result or []))
                    confidence = self._text_similarity(self.phrase, recognized)
                    detected = confidence >= threshold
                    if detected and now - area.last_trigger >= self.scan_cooldown:
                        click_x = (area.left + area.right) // 2
                        click_y = (area.top + area.bottom) // 2
                        self._focus_click_enter(click_x, click_y, self.scan_focus_delay)
                        area.armed = False
                        area.misses = 0
                        area.triggers += 1
                        area.attempts += 1
                        area.last_trigger = now
                        self.events.put(("trigger", (index, confidence, area.attempts)))
                    elif not detected:
                        area.misses += 1
                        if area.misses >= 3:
                            area.armed = True
                            area.attempts = 0
                        self.events.put(("scan", (index, confidence, "Armado" if area.armed else "Aguardando sumir")))
                    else:
                        area.misses = 0
                        self.events.put(("scan", (index, confidence, "Detectado" if area.armed else "Enter enviado")))
                except pyautogui.FailSafeException:
                    self.running = False
                    self.events.put(("stopped", "Parado pelo failsafe: mouse no canto superior esquerdo"))
                    break
                except Exception as error:
                    self.events.put(("scan", (index, 0.0, f"Erro: {error}")))
            time.sleep(max(0.1, self.scan_interval))
        self.events.put(("stopped", "Parado"))

    @staticmethod
    def _focus_click_enter(x: int, y: int, delay_after_click: float = 1.0) -> None:
        """Foca a janela sob o ponto e envia Enter após o foco estabilizar."""
        user32 = ctypes.windll.user32
        user32.WindowFromPoint.argtypes = [wintypes.POINT]
        user32.WindowFromPoint.restype = wintypes.HWND
        user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
        user32.GetAncestor.restype = wintypes.HWND
        user32.SetForegroundWindow.argtypes = [wintypes.HWND]
        user32.BringWindowToTop.argtypes = [wintypes.HWND]
        point = wintypes.POINT(x, y)
        window = user32.WindowFromPoint(point)
        root_window = user32.GetAncestor(window, 2) if window else 0  # GA_ROOT
        if root_window:
            user32.ShowWindow(root_window, 9)  # SW_RESTORE
            pyautogui.keyDown("alt")
            try:
                user32.SetForegroundWindow(root_window)
                user32.BringWindowToTop(root_window)
            finally:
                pyautogui.keyUp("alt")
        time.sleep(0.25)
        pyautogui.click(x, y)
        time.sleep(max(0.5, delay_after_click))
        pyautogui.press("enter")
        time.sleep(0.20)

    @staticmethod
    def _text_similarity(expected: str, recognized: str) -> float:
        normalize = lambda value: "".join(character for character in value.casefold() if character.isalnum())
        needle = normalize(expected)
        haystack = normalize(recognized)
        if not needle or not haystack:
            return 0.0
        if needle in haystack:
            return 1.0
        minimum = max(1, int(len(needle) * 0.72))
        maximum = min(len(haystack), int(len(needle) * 1.28) + 1)
        best = 0.0
        for size in range(minimum, maximum + 1):
            for start in range(0, len(haystack) - size + 1):
                best = max(best, difflib.SequenceMatcher(None, needle, haystack[start : start + size]).ratio())
                if best >= 0.99:
                    return best
        return best

    def _drain_events(self) -> None:
        changed = False
        while True:
            try:
                kind, payload = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "trigger":
                index, confidence, attempt = payload
                self.status.set(f"Enter enviado: {self.areas[index].name} • tentativa {attempt} ({confidence:.0%})")
                self._save()
                changed = True
            elif kind == "scan":
                index, confidence, state = payload
                if index < len(self.areas):
                    self.table.set(str(index), "confidence", f"{confidence:.0%}")
                    self.table.set(str(index), "state", state)
                    self.table.set(str(index), "triggers", str(self.areas[index].triggers))
            elif kind == "stopped":
                self.running = False
                self.status.set(str(payload))
                self.start_button.configure(text="▶ INICIAR", bg="#25b86f")
                self.root.deiconify()
        if changed:
            self._refresh_table()
        self.root.after(100, self._drain_events)

    def _refresh_table(self) -> None:
        selected = self.table.selection()
        self.table.delete(*self.table.get_children())
        for index, area in enumerate(self.areas):
            state = "Armado" if area.enabled else "Desativado"
            region = f"{area.left},{area.top} → {area.right},{area.bottom}"
            self.table.insert("", "end", iid=str(index), values=(area.name, region, state, "—", area.triggers))
        if selected and self.table.exists(selected[0]):
            self.table.selection_set(selected[0])

    def _load(self) -> None:
        if not CONFIG_PATH.exists():
            return
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            self.phrase = str(data.get("phrase", self.phrase))
            self.threshold.set(float(data.get("threshold", 0.78)))
            self.interval.set(float(data.get("interval", 0.50)))
            self.cooldown.set(float(data.get("cooldown", 8.0)))
            self.focus_delay.set(float(data.get("focus_delay", 1.0)))
            self.minimize_while_running.set(bool(data.get("minimize_while_running", True)))
            for item in data.get("areas", []):
                runtime_fields = {"armed": True, "misses": 0, "last_trigger": 0.0}
                self.areas.append(WatchArea(**{**item, **runtime_fields}))
        except Exception:
            self.areas = []

    def _save(self) -> None:
        fields = ("name", "left", "top", "right", "bottom", "enabled", "triggers")
        data = {
            "phrase": self.phrase,
            "threshold": float(self.threshold.get()),
            "interval": float(self.interval.get()),
            "cooldown": float(self.cooldown.get()),
            "focus_delay": float(self.focus_delay.get()),
            "minimize_while_running": bool(self.minimize_while_running.get()),
            "areas": [{key: asdict(area)[key] for key in fields} for area in self.areas],
        }
        CONFIG_PATH.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")

    def _close(self) -> None:
        self.running = False
        self._save()
        self.root.destroy()


def main() -> None:
    enable_dpi_awareness()
    root = tk.Tk()
    CoordinatorApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
