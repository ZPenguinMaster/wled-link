"""WLED Link on the Windows desktop: the app window (the control page in a WebView2 window, like any other app)
and the tray icon. wledlink.py loads this only for those, so the link itself runs without the extra packages
(pywebview, pystray, Pillow)."""
from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import threading
import winreg
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
ICON = HERE / "app" / "icon.ico"
TITLE = "WLED Link"
APP_ID = "ZPenguinMaster.WLEDLink"  # the taskbar groups the window (and the Start menu entry) under this
WINDOW_MUTEX = "Local\\WLEDLink.Window"

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32


# ---------------------------------------------------------------------------------------------
# Windows helpers

def set_app_id():
    """Our own taskbar button and icon, instead of being filed under Python."""
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(ctypes.c_wchar_p(APP_ID))
    except (AttributeError, OSError):
        pass


def light_theme() -> bool:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as k:
            return winreg.QueryValueEx(k, "AppsUseLightTheme")[0] == 1
    except OSError:
        return False


def logon_session() -> str:
    """This Windows sign-in: it changes when the user signs out and in again (the token's AuthenticationId)."""
    class LUID(ctypes.Structure):
        _fields_ = [("LowPart", wintypes.DWORD), ("HighPart", wintypes.LONG)]

    class TOKEN_STATISTICS(ctypes.Structure):
        _fields_ = [("TokenId", LUID), ("AuthenticationId", LUID), ("ExpirationTime", ctypes.c_longlong),
                    ("TokenType", ctypes.c_int), ("ImpersonationLevel", ctypes.c_int), ("DynamicCharged", wintypes.DWORD),
                    ("DynamicAvailable", wintypes.DWORD), ("GroupCount", wintypes.DWORD),
                    ("PrivilegeCount", wintypes.DWORD), ("ModifiedId", LUID)]

    advapi32 = ctypes.windll.advapi32
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE  # a 64-bit pseudo-handle: the default int would mangle it
    advapi32.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    advapi32.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                             ctypes.POINTER(wintypes.DWORD)]
    token = wintypes.HANDLE()
    if not advapi32.OpenProcessToken(kernel32.GetCurrentProcess(), 0x0008, ctypes.byref(token)):  # TOKEN_QUERY
        return ""
    try:
        stats, size = TOKEN_STATISTICS(), wintypes.DWORD()
        if not advapi32.GetTokenInformation(token, 10, ctypes.byref(stats), ctypes.sizeof(stats), ctypes.byref(size)):
            return ""
        return f"{stats.AuthenticationId.HighPart:x}-{stats.AuthenticationId.LowPart:x}"
    finally:
        kernel32.CloseHandle(token)


def focus_existing_window() -> bool:
    hwnd = user32.FindWindowW(None, TITLE)
    if not hwnd:
        return False
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    return True


def style_title_bar(hwnd: int, light: bool):
    """The title bar in the app's own colours (Windows 11), so the window reads as one piece."""
    dwm = ctypes.windll.dwmapi

    def attr(key, value):
        v = ctypes.c_int(value)
        dwm.DwmSetWindowAttribute(wintypes.HWND(hwnd), key, ctypes.byref(v), ctypes.sizeof(v))

    attr(20, 0 if light else 1)                       # DWMWA_USE_IMMERSIVE_DARK_MODE
    attr(35, 0x00F7F2F2 if light else 0x000C0A0A)     # DWMWA_CAPTION_COLOR (0x00BBGGRR): the page background
    attr(36, 0x001F1D1D if light else 0x00F7F5F5)     # DWMWA_TEXT_COLOR


# ---------------------------------------------------------------------------------------------
# the app window

class _WindowApi:
    """What the page in the window may ask of it (window.pywebview.api)."""

    def __init__(self, script: Path):
        self._script = script

    def start_link(self):
        """WLED Link was quit while the window stayed open: start it again."""
        start_link_process(self._script)

    def close(self):
        import webview
        for w in list(webview.windows):
            w.destroy()


def start_link_process(script: Path):
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    subprocess.Popen([str(pythonw if pythonw.exists() else sys.executable), str(script), "run"], cwd=str(HERE),
                     close_fds=True, creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)


def run_window(url: str, state_dir: Path, script: Path) -> int:
    """Opens the WLED Link window, or brings the open one to the front. Returns when it's closed."""
    mutex = kernel32.CreateMutexW(None, False, WINDOW_MUTEX)
    if kernel32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS: one window is enough
        focus_existing_window()
        return 0
    try:
        import webview
    except ImportError:  # no pywebview: an app window from Edge instead
        edge = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft" / "Edge" / "Application" / "msedge.exe"
        subprocess.Popen([str(edge), f"--app={url}", f"--user-data-dir={state_dir / 'edge'}", "--window-size=1180,800"])
        return 0
    set_app_id()
    light = light_theme()
    geometry_file = state_dir / "window.json"
    try:
        g = json.loads(geometry_file.read_text())
    except (OSError, ValueError):
        g = {}
    window = webview.create_window(
        TITLE, url, width=g.get("width", 1180), height=g.get("height", 800), x=g.get("x"), y=g.get("y"),
        min_size=(380, 560), background_color="#f2f2f7" if light else "#0a0a0c", text_select=False,
        js_api=_WindowApi(script))

    def shown():
        try:
            style_title_bar(int(window.native.Handle.ToInt64()), light)
        except Exception:
            pass

    def closing():
        try:
            if window.width > 300 and window.height > 300:
                geometry_file.write_text(json.dumps({"width": window.width, "height": window.height, "x": window.x, "y": window.y}))
        except Exception:
            pass

    window.events.shown += shown
    window.events.closing += closing
    state_dir.mkdir(parents=True, exist_ok=True)
    webview.start(gui="edgechromium", private_mode=False, storage_path=str(state_dir / "webview"), icon=str(ICON))
    kernel32.CloseHandle(mutex)
    return 0


def open_window_process(script: Path):
    """Opens the window from the background copy (the tray icon): a process of its own, so closing it leaves the
    link running."""
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    exe = str(pythonw if pythonw.exists() else sys.executable)
    subprocess.Popen([exe, str(script), "app"], cwd=str(HERE), close_fds=True,
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)


# ---------------------------------------------------------------------------------------------
# the tray icon

class Tray:
    """The icon next to the clock: open the app, pick a mode, quit. Runs in a thread of its own; `act` runs
    an action (by name) in the link's event loop, `status` says what to show."""

    MODES = [("sync", "PC Sync"), ("neutral", "Neutral white"), ("warm", "Warm white"), ("cool", "Cool white")]

    def __init__(self, act, status, on_open, on_quit):
        self._act, self._status, self._open, self._quit = act, status, on_open, on_quit
        self._icon = None
        self._images = {}
        self._shown = None

    def start(self) -> bool:
        try:
            import pystray
            from PIL import Image
        except ImportError:
            return False
        self._images = {True: Image.open(HERE / "app" / "icon.png"), False: Image.open(HERE / "app" / "icon-off.png")}
        item, sep = pystray.MenuItem, pystray.Menu.SEPARATOR

        def mode_item(mode, text):
            return item(text, lambda: self._act(mode), checked=lambda _: self._status().get("mode") == mode, radio=True,
                        enabled=lambda _: self._status().get("connected", False))

        menu = pystray.Menu(
            item("Open WLED Link", lambda: self._open(), default=True),
            sep,
            *[mode_item(m, t) for m, t in self.MODES],
            item(lambda _: "Turn on" if self._status().get("mode") == "off" else "Turn off", lambda: self._act("power"),
                 enabled=lambda _: self._status().get("connected", False)),
            sep,
            item("Quit WLED Link", lambda: self._quit()),
        )
        self._icon = pystray.Icon("WLED Link", self._images[False], TITLE, menu)
        threading.Thread(target=self._icon.run, name="tray", daemon=True).start()
        return True

    def refresh(self):
        """Icon and tooltip for the current state (cheap when nothing changed)."""
        if not self._icon:
            return
        s = self._status()
        shown = (s.get("connected", False), s.get("text", ""))
        if shown == self._shown:
            return
        self._shown = shown
        try:
            self._icon.icon = self._images[shown[0]]
            self._icon.title = f"{TITLE} · {shown[1]}"[:127]
        except Exception:
            pass

    def stop(self):
        if self._icon:
            try:
                self._icon.stop()
            except Exception:
                pass
