import json
import os
import sys
import tkinter as tk
from tkinter import ttk, scrolledtext
import serial
import serial.tools.list_ports
import threading
import time
import re
import ctypes
from ctypes import wintypes

# ---------------------------------------------------------------------------
# Injecao de teclado/mouse via SendInput usando SCANCODE de hardware.
# Jogos como o Minecraft (GLFW/LWJGL) costumam IGNORAR teclas "virtuais" (o
# que o pyautogui.press envia) e so reconhecem scancodes reais. Por isso
# trocamos a injecao de teclas/cliques para SendInput.
# SendInput retorna o numero de eventos inseridos; 0 = o Windows BLOQUEOU
# (tipicamente UIPI: o jogo roda como administrador e este script nao).
# ---------------------------------------------------------------------------
_user32 = ctypes.windll.user32
ULONG_PTR = wintypes.WPARAM  # ponteiro-sized (8 bytes em Python 64-bit)

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_KEYUP = 0x0002
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

SC_SPACE = 0x39
SC_ESC = 0x01


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ULONG_PTR)]


class _MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", _KEYBDINPUT), ("mi", _MOUSEINPUT)]


class _INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]


def _send(*inputs):
    n = len(inputs)
    arr = (_INPUT * n)(*inputs)
    sent = _user32.SendInput(n, arr, ctypes.sizeof(_INPUT))
    return sent == n  # False => Windows bloqueou (provavel falta de admin)


def _key_input(scancode, keyup):
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if keyup else 0)
    return _INPUT(type=INPUT_KEYBOARD,
                  u=_INPUTUNION(ki=_KEYBDINPUT(0, scancode, flags, 0, 0)))


def _mouse_input(flags):
    return _INPUT(type=INPUT_MOUSE,
                  u=_INPUTUNION(mi=_MOUSEINPUT(0, 0, 0, flags, 0, 0)))


def tap_key(scancode, hold=0.05):
    """Pressiona e solta uma tecla (keydown -> segura -> keyup)."""
    ok = _send(_key_input(scancode, False))
    time.sleep(hold)
    ok = _send(_key_input(scancode, True)) and ok
    return ok


def left_click(hold=0.05):
    ok = _send(_mouse_input(MOUSEEVENTF_LEFTDOWN))
    time.sleep(hold)
    ok = _send(_mouse_input(MOUSEEVENTF_LEFTUP)) and ok
    return ok


# Movimento de cursor por SetCursorPos (livre da aceleracao de ponteiro do
# Windows, ao contrario do SendInput relativo) -> camera proporcional e suave.
class _POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


def move_cursor_rel(dx, dy):
    pt = _POINT()
    _user32.GetCursorPos(ctypes.byref(pt))
    _user32.SetCursorPos(pt.x + dx, pt.y + dy)


MOVER_HZ = 120
MOVER_PERIOD = 1.0 / MOVER_HZ

BAUDS = ["9600", "19200", "38400", "57600", "115200", "230400"]
DEFAULT_BAUD = "115200"

# Paleta (tema escuro, estilo VS Code).
BG = "#1e1e1e"
FG = "#d4d4d4"
BG_ENTRY = "#ffffff"
FG_ENTRY = "#1e1e1e"
BG_TERMINAL = "#121212"
BG_WINDOW = "#181818"
BG_BUTTON = "#3a3d41"
BG_BUTTON_CONNECT = "#2e7d32"
BG_BUTTON_DISCONNECT = "#7b2020"
ACCENT = "#64b5f6"
DOT_ON = "#66bb6a"
DOT_OFF = "#6e6e6e"
FONT_TERMINAL = ("Consolas", 10)
FONT_UI = ("Segoe UI", 9)
FONT_TITLE = ("Segoe UI", 12, "bold")

# MAC remoto embutido no hwid das COMs Bluetooth (Windows). Ex.:
#   BTHENUM\{...SPP UUID...}_LOCALMFG&0002\7&12A885FE&0&98D341FE1AB6_C00000000
# O padrao "&<12 hex>_" captura o MAC do dispositivo (98D341FE1AB6), sem casar
# com o UUID do SPP (que termina em '}' e nao em '_').
_BT_MAC_RE = re.compile(r"&([0-9A-Fa-f]{12})_")


def find_hc06_port():
    """Acha a porta COM 'outgoing' do HC-06 sem depender do numero da porta.
    O Windows cria 2 COMs SPP: a outgoing (conecta no modulo) traz o MAC remoto
    no hwid; a incoming local tem MAC 000000000000. Retorna (device, mac) da
    primeira porta com MAC != zeros, ou (None, None)."""
    for p in serial.tools.list_ports.comports():
        hwid = p.hwid or ""
        if "BTHENUM" not in hwid.upper():
            continue
        m = _BT_MAC_RE.search(hwid)
        if m and m.group(1).upper() != "000000000000":
            return p.device, m.group(1).upper()
    return None, None


class App:
    def __init__(self, root):
        self.root = root
        # Caminho absoluto: ao rodar elevado (UAC), o processo inicia em system32;
        # ancorar no diretorio do script evita perder as portas salvas.
        self.settings_file = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "settings.json")

        # --- estado da conexao / controle ---
        self.serial = None
        self.running = False

        self._mouse_sync = False
        self._mouse_buf = bytearray()
        self._mouse_last = {0: 0, 1: 0}
        self._mouse_last_log = 0.0
        self._cmd_buf = bytearray()       # acumula comandos ASCII (JUMP\n, etc.)
        self._vel = [0, 0]                # velocidade-alvo do joystick (-127..127)
        self._mouse_accum = [0.0, 0.0]    # acumulador de subpixel do movedor
        self._sens = 20.0                 # sobrescrito pelo slider
        self._auto_enabled = True         # auto-connect ativo (desliga em disconnect manual)
        self._auto_warned = False         # evita spam de "procurando..." no log

        root.protocol("WM_DELETE_WINDOW", self.on_closing)
        root.title("Picareta — Monitor Bluetooth")
        root.configure(bg=BG_WINDOW)
        root.geometry("560x600")
        # minsize generoso o bastante para a barra inferior nunca ser cortada,
        # mesmo se a janela abrir minimizada/pequena.
        root.minsize(440, 380)

        self._apply_style()
        self._build_ui()

        self.refresh_ports()
        self._load_settings()
        self._update_status()
        self._auto_connect()  # tenta conectar sozinho ao HC-06 ao abrir

    # ----------------------------------------------------------------- UI ----
    def _build_ui(self):
        outer = tk.Frame(self.root, bg=BG, bd=0)
        outer.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # Cabecalho: titulo + indicador de status (ponto colorido).
        header = tk.Frame(outer, bg=BG)
        header.pack(fill=tk.X, pady=(2, 8))

        tk.Label(header, text="HC-06 · Bluetooth", bg=BG, fg=FG,
                 font=FONT_TITLE).pack(side=tk.LEFT)

        status = tk.Frame(header, bg=BG)
        status.pack(side=tk.RIGHT)
        self.status_dot = tk.Label(status, text="●", bg=BG, fg=DOT_OFF,
                                   font=("Segoe UI", 11))
        self.status_dot.pack(side=tk.LEFT, padx=(0, 4))
        self.status_text = tk.Label(status, text="desconectado", bg=BG, fg=FG,
                                    font=FONT_UI)
        self.status_text.pack(side=tk.LEFT)

        # Linha de conexao: porta, baud, conectar, limpar, atualizar.
        top = tk.Frame(outer, bg=BG)
        top.pack(fill=tk.X, pady=(0, 8))

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=DEFAULT_BAUD)

        self.port_cb = ttk.Combobox(top, textvariable=self.port_var,
                                    width=12, font=FONT_UI, state="readonly")
        self.port_cb.pack(side=tk.LEFT, padx=(0, 6), ipady=2)

        self.baud_cb = ttk.Combobox(top, textvariable=self.baud_var, values=BAUDS,
                                    width=8, font=FONT_UI, state="readonly")
        self.baud_cb.pack(side=tk.LEFT, padx=(0, 6), ipady=2)
        self.baud_cb.set(DEFAULT_BAUD)

        self.btn_connect = tk.Button(top, text="Conectar", bg=BG_BUTTON_CONNECT,
                                     fg="white", font=FONT_UI, relief=tk.FLAT,
                                     padx=10, pady=3, activebackground="#388e3c",
                                     activeforeground="white", cursor="hand2",
                                     command=self.toggle_connection)
        self.btn_connect.pack(side=tk.LEFT, padx=(0, 6))

        self.btn_clear = tk.Button(top, text="Limpar", bg=BG_BUTTON, fg="white",
                                   font=FONT_UI, relief=tk.FLAT, padx=10, pady=3,
                                   activebackground="#4a4d51", activeforeground="white",
                                   cursor="hand2", command=self.clear_terminal)
        self.btn_clear.pack(side=tk.LEFT, padx=(0, 6))

        self.btn_refresh = tk.Button(top, text="Atualizar portas", bg=BG_BUTTON,
                                     fg="white", font=FONT_UI, relief=tk.FLAT,
                                     padx=10, pady=3, activebackground="#4a4d51",
                                     activeforeground="white", cursor="hand2",
                                     command=self.refresh_ports)
        self.btn_refresh.pack(side=tk.LEFT)

        # Barra inferior (controle + sensibilidade). EMPACOTADA ANTES do terminal
        # com side=BOTTOM: assim ela reserva o espaco e o terminal (expand) e quem
        # encolhe quando a janela diminui -> a barra nunca e cortada.
        bottom = tk.Frame(outer, bg=BG)
        bottom.pack(side=tk.BOTTOM, fill=tk.X, pady=(8, 0))

        # Modo Controle ligado por padrao: ao conectar ja move o cursor / aciona
        # teclas a partir do stream do martelo.
        self.mouse_mode_var = tk.BooleanVar(value=True)
        self.chk_mouse = tk.Checkbutton(
            bottom, text="Controle", variable=self.mouse_mode_var,
            bg=BG, fg=ACCENT, selectcolor=BG_TERMINAL,
            activebackground=BG, activeforeground=ACCENT,
            font=FONT_UI, cursor="hand2"
        )
        self.chk_mouse.pack(side=tk.LEFT)

        # Sensibilidade da camera (px por unidade-de-joystick por segundo).
        # Ajustavel em tempo real -- o "feel" certo so a mao do usuario acha.
        tk.Label(bottom, text="Sens.", bg=BG, fg=ACCENT,
                 font=FONT_UI).pack(side=tk.LEFT, padx=(16, 4))
        self.sens_scale = tk.Scale(
            bottom, from_=2, to=80, resolution=1, orient=tk.HORIZONTAL,
            bg=BG, fg=ACCENT, troughcolor="#333333", highlightthickness=0,
            activebackground=ACCENT, sliderlength=16, length=160, font=FONT_UI,
            showvalue=True, command=lambda v: setattr(self, "_sens", float(v)))
        self.sens_scale.set(20)
        self.sens_scale.pack(side=tk.LEFT)

        # Terminal preenche o espaco restante entre 'top' e 'bottom'.
        self.terminal = scrolledtext.ScrolledText(outer, bg=BG_TERMINAL, fg=FG,
                                                   font=FONT_TERMINAL, relief=tk.FLAT,
                                                   wrap=tk.WORD, state=tk.DISABLED,
                                                   bd=0, padx=6, pady=6)
        self.terminal.pack(fill=tk.BOTH, expand=True)

        self.terminal.tag_config("info",    foreground="#4fc3f7")
        self.terminal.tag_config("error",   foreground="#ef5350")
        self.terminal.tag_config("sent",    foreground="#81c784")
        self.terminal.tag_config("default")

    def _apply_style(self):
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TCombobox",
                        fieldbackground=BG_ENTRY, background=BG_ENTRY,
                        foreground=FG_ENTRY, arrowcolor="#555555", bordercolor=BG_ENTRY,
                        selectbackground=BG_ENTRY, selectforeground=FG_ENTRY)
        style.map("TCombobox",
                  fieldbackground=[("readonly", BG_ENTRY), ("disabled", BG_ENTRY)],
                  foreground=[("readonly", FG_ENTRY), ("disabled", FG_ENTRY)],
                  selectbackground=[("readonly", BG_ENTRY)],
                  selectforeground=[("readonly", FG_ENTRY)])

    def _update_status(self):
        connected = bool(self.serial and self.serial.is_open)
        self.status_dot.config(fg=DOT_ON if connected else DOT_OFF)
        self.status_text.config(
            text=f"conectado · {self.port_var.get()}" if connected else "desconectado")

    # ------------------------------------------------------------ terminal ----
    def write_terminal(self, text, tag=None):
        text = text or ""
        effective_tag = tag or "default"

        def _write():
            self.terminal.config(state=tk.NORMAL)
            self.terminal.insert(tk.END, text + "\n", effective_tag)
            self.terminal.see(tk.END)
            self.terminal.config(state=tk.DISABLED)

        self.terminal.after(0, _write)

    def clear_terminal(self):
        self.terminal.config(state=tk.NORMAL)
        self.terminal.delete("1.0", tk.END)
        self.terminal.config(state=tk.DISABLED)

    # --------------------------------------------------------------- portas ---
    def refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    # ------------------------------------------------------------ conexao -----
    def toggle_connection(self):
        if self.serial and self.serial.is_open:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        port = self.port_var.get()
        if not port:
            self.write_terminal("[nenhuma porta selecionada]\n", tag="error")
            return
        baud = int(self.baud_var.get())
        try:
            self.serial = serial.Serial(port, baud, timeout=0.1)
            self.running = True
            self._auto_enabled = True   # conexao ativa -> manter auto-reconect
            self._auto_warned = False
            self._vel = [0, 0]
            self._mouse_accum = [0.0, 0.0]
            self.btn_connect.config(text="Desconectar", bg=BG_BUTTON_DISCONNECT,
                                    activebackground="#9a2a2a")
            self.write_terminal(f"[conectado em {port} @ {baud}]\n", tag="info")
            self._update_status()
            threading.Thread(target=self._read_loop, daemon=True).start()
            threading.Thread(target=self._mover_loop, daemon=True).start()
        except serial.SerialException as e:
            self.write_terminal(f"[erro: {e}]\n", tag="error")

    def disconnect(self):
        self.running = False
        self._auto_enabled = False  # disconnect manual -> nao reconecta sozinho
        self._mouse_sync = False
        self._mouse_buf = bytearray()
        self._cmd_buf = bytearray()
        self._vel = [0, 0]  # zera para o movedor parar o cursor
        if self.serial:
            self.serial.close()
            self.serial = None
        self.btn_connect.config(text="Conectar", bg=BG_BUTTON_CONNECT,
                                activebackground="#388e3c")
        self.write_terminal("[desconectado]\n", tag="info")
        self._update_status()

    def _auto_connect(self):
        """Procura e conecta ao HC-06 automaticamente (achando a porta pelo MAC
        no hwid) e re-tenta a cada 3 s ate conectar. Desligado por disconnect
        manual; rearmado por connect e por perda de conexao."""
        if not self._auto_enabled or (self.serial and self.serial.is_open):
            return
        self.refresh_ports()
        dev, mac = find_hc06_port()
        if dev:
            self.port_var.set(dev)
            self.write_terminal(f"[auto] HC-06 em {dev} (MAC {mac}) -> conectando", tag="info")
            self.connect()
        elif not self._auto_warned:
            self.write_terminal("[auto] procurando HC-06... ligue/pareie o modulo", tag="info")
            self._auto_warned = True
        if self._auto_enabled and not (self.serial and self.serial.is_open):
            self.root.after(3000, self._auto_connect)

    # -------------------------------------------------------- leitura serial --
    def _read_loop(self):
        while self.running and self.serial and self.serial.is_open:
            try:
                data = self.serial.read(256)
                if data:
                    if self.mouse_mode_var.get():
                        self._process_control_bytes(data)
                    else:
                        text = data.decode("utf-8", errors="replace")
                        self.write_terminal(text)
            except serial.SerialException:
                self.running = False
                try:                       # libera a porta antes de tentar religar
                    if self.serial:
                        self.serial.close()
                except Exception:
                    pass
                self.serial = None
                self.terminal.after(0, lambda: self.write_terminal("[conexao perdida]\n", tag="error"))
                self.terminal.after(0, lambda: self.btn_connect.config(
                    text="Conectar", bg=BG_BUTTON_CONNECT, activebackground="#388e3c"))
                self.terminal.after(0, self._update_status)
                # perda de conexao (nao foi disconnect manual) -> tenta religar
                self.terminal.after(3000, self._auto_connect)
                break

    def _process_control_bytes(self, data):
        """Demultiplexa o stream Bluetooth compartilhado:
          - 0xFF  -> inicio de quadro de mouse; os 3 bytes SEGUINTES sao
                     opacos (axis + valor 16b), podem ter qualquer valor;
          - demais bytes -> comando ASCII, acumulado ate '\\n'/'\\r'.
        A ordem importa: enquanto estamos lendo os 3 bytes do mouse, NAO os
        interpretamos como ASCII (um deles pode ser 0x0A, 0xFF, 'J'...)."""
        for b in data:
            if self._mouse_sync:
                self._mouse_buf.append(b)
                if len(self._mouse_buf) == 3:
                    self._apply_mouse(self._mouse_buf)
                    self._mouse_sync = False
                    self._mouse_buf = bytearray()
            elif b == 0xFF:
                self._mouse_sync = True
                self._mouse_buf = bytearray()
            elif b in (0x0A, 0x0D):  # \n ou \r -> fim de comando
                self._handle_command(self._cmd_buf.decode("utf-8", errors="replace"))
                self._cmd_buf = bytearray()
            else:
                self._cmd_buf.append(b)
                if len(self._cmd_buf) > 64:  # guarda contra lixo sem terminador
                    self._cmd_buf = bytearray()

        now = time.time()
        if now - self._mouse_last_log > 0.5:
            self.write_terminal(
                f"[mouse] X={self._mouse_last[0]:+4d} Y={self._mouse_last[1]:+4d}",
                tag="info"
            )
            self._mouse_last_log = now

    def _apply_mouse(self, buf):
        # Modelo de VELOCIDADE: o pacote NAO move o cursor na hora; ele apenas
        # atualiza a velocidade-alvo. Quem move e o _mover_loop, continuamente.
        axis = buf[0]
        value = int.from_bytes(buf[1:3], byteorder='little', signed=True)
        if axis in (0, 1):
            self._vel[axis] = value
            self._mouse_last[axis] = value

    def _mover_loop(self):
        """Move o cursor continuamente (~120 Hz) com base na velocidade-alvo,
        acumulando subpixels -> movimento suave, desacoplado da taxa de pacotes
        e proporcional a deflexao do joystick (sem aceleracao de ponteiro)."""
        last = time.perf_counter()
        while self.running:
            now = time.perf_counter()
            dt = now - last
            last = now

            if self.mouse_mode_var.get():
                vx, vy = self._vel[0], self._vel[1]
            else:
                vx, vy = 0, 0  # fora do modo Controle, nao mexe o cursor

            self._mouse_accum[0] += vx * self._sens * dt
            self._mouse_accum[1] += vy * self._sens * dt
            dx = int(self._mouse_accum[0])
            dy = int(self._mouse_accum[1])
            if dx or dy:
                self._mouse_accum[0] -= dx
                self._mouse_accum[1] -= dy
                move_cursor_rel(dx, dy)

            time.sleep(MOVER_PERIOD)

    def _handle_command(self, line):
        """Mapeia comandos ASCII do controle para teclas/cliques no jogo.
        Usa SendInput por scancode (nao pyautogui) para que jogos como o
        Minecraft reconhecam as teclas."""
        line = line.strip()
        if not line:
            return
        cmd, _, _arg = line.partition(":")
        cmd = cmd.upper()
        if cmd == "JUMP":
            ok = tap_key(SC_SPACE)
        elif cmd == "MINE":
            ok = left_click()
        elif cmd == "PAUSE":
            ok = tap_key(SC_ESC)
        else:
            # DIAG: mostra comandos desconhecidos (ex.: "AI:<gesto> <conf>" do
            # debug da IA) em vez de ignorar, para validar que o gesture_task
            # esta rodando e inferindo. Remover depois junto com o debug do firmware.
            self.write_terminal(f"[ia] {line}", tag="info")
            return

        if ok:
            self.write_terminal(f"[cmd] {line}", tag="sent")
        else:
            self.write_terminal(
                f"[cmd] {line} -> BLOQUEADO pelo Windows. "
                f"Rode este script COMO ADMINISTRADOR (o jogo provavelmente esta elevado).",
                tag="error")

    # ----------------------------------------------------------- settings -----
    def _load_settings(self):
        try:
            with open(self.settings_file, "r") as file:
                settings = json.load(file)
            self.port_var.set(settings.get("hc06_port", ""))
            self.baud_var.set(settings.get("hc06_baud", DEFAULT_BAUD))
        except (FileNotFoundError, json.JSONDecodeError):
            pass

    def on_closing(self):
        settings = {
            "hc06_port": self.port_var.get(),
            "hc06_baud": self.baud_var.get(),
        }
        with open(self.settings_file, "w") as file:
            json.dump(settings, file)
        self.root.destroy()


def _ensure_admin():
    """Relanca o script ELEVADO se ainda nao estiver como administrador.
    Necessario porque o Windows (UIPI) descarta injecao sintetica de teclado/
    mouse vinda de um processo de integridade mais baixa que o jogo. Dispara
    um prompt de UAC a cada execucao."""
    try:
        if ctypes.windll.shell32.IsUserAnAdmin():
            return
    except Exception:
        return  # nao-Windows ou API indisponivel: segue sem elevar
    params = " ".join(f'"{a}"' for a in sys.argv)
    workdir = os.path.dirname(os.path.abspath(__file__))  # evita iniciar em system32
    ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, params, workdir, 1)
    sys.exit(0)


if __name__ == "__main__":
    _ensure_admin()
    # Resolucao de 1 ms para o timer: sem isso, time.sleep(~8ms) do movedor
    # cai em ~15ms (~64 Hz). Com isso, o movedor roda perto dos 120 Hz -> mais suave.
    try:
        ctypes.windll.winmm.timeBeginPeriod(1)
    except Exception:
        pass
    root = tk.Tk()
    App(root)
    root.mainloop()
