"""Screenshots of the game windows, so a run folder shows what each player actually saw.

Uses PrintWindow with PW_RENDERFULLCONTENT, which also captures DirectX windows that are covered
by other windows. Never raises: a missing or minimised window just means no picture.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from pathlib import Path

PW_RENDERFULLCONTENT = 2
SHOT_EVERY_S = 60

# Work in real screen pixels (the laptop runs at 125 % scaling), so window sizes and positions
# match what the game itself uses.
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except (AttributeError, OSError):
    pass

_u32 = ctypes.WinDLL("user32", use_last_error=True)
_g32 = ctypes.WinDLL("gdi32", use_last_error=True)
_u32.GetWindowDC.restype = wintypes.HDC
_u32.GetWindowDC.argtypes = [wintypes.HWND]
_u32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
_u32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
_u32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
_u32.IsWindowVisible.argtypes = [wintypes.HWND]
_u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, wintypes.UINT]
SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE = 0x1, 0x4, 0x10
SPI_GETWORKAREA = 0x30

# The mod's own text console belongs to the game process too; it is not what the player sees.
_NOT_GAME = {"ConsoleWindowClass", "PseudoConsoleWindow"}
_g32.CreateCompatibleDC.restype = wintypes.HDC
_g32.CreateCompatibleDC.argtypes = [wintypes.HDC]
_g32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
_g32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
_g32.SelectObject.restype = wintypes.HGDIOBJ
_g32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
_g32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
_g32.DeleteDC.argtypes = [wintypes.HDC]
_g32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                           ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD),
                ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
                ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG),
                ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


def _windows(pid: int) -> tuple[int | None, int | None]:
    """(game window, text console) of the process: the largest visible top-level window of each kind."""
    game: list[tuple[int, int]] = []
    console: list[tuple[int, int]] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        owner = wintypes.DWORD()
        _u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _u32.IsWindowVisible(hwnd):
            cls = ctypes.create_unicode_buffer(256)
            _u32.GetClassNameW(hwnd, cls, 256)
            r = wintypes.RECT()
            if _u32.GetWindowRect(hwnd, ctypes.byref(r)):
                area = (r.right - r.left) * (r.bottom - r.top)
                if area > 0:
                    (console if cls.value in _NOT_GAME else game).append((area, hwnd))
        return True

    _u32.EnumWindows(each, 0)
    return (max(game)[1] if game else None), (max(console)[1] if console else None)


def _main_window(pid: int) -> int | None:
    """The window the player sees (never the mod's text console)."""
    return _windows(pid)[0]


def arrange(pids: dict[str, int]) -> None:
    """Give every process its own column of the screen: game window at the top, its text console
    below it, so screenshots and screen recordings show each player clearly. Never raises."""
    try:
        work = wintypes.RECT()
        if not _u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(work), 0) or not pids:
            return
        col_w = (work.right - work.left) // len(pids)
        height = work.bottom - work.top
        for i, pid in enumerate(pids.values()):
            game, console = _windows(pid)
            x, top = work.left + i * col_w, work.top
            if game:
                r = wintypes.RECT()
                _u32.GetWindowRect(game, ctypes.byref(r))
                if (r.left, r.top) != (x, top):
                    _u32.SetWindowPos(game, None, x, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
                top += r.bottom - r.top
            if console and height - (top - work.top) >= 150:
                r = wintypes.RECT()
                _u32.GetWindowRect(console, ctypes.byref(r))
                want = (x, top, x + col_w, work.bottom)
                if (r.left, r.top, r.right, r.bottom) != want:
                    _u32.SetWindowPos(console, None, x, top, col_w, work.bottom - top,
                                      SWP_NOZORDER | SWP_NOACTIVATE)
    except Exception:
        pass


def capture(pid: int, path: Path) -> bool:
    """Save the process's main window as a JPEG. False when there is nothing to capture."""
    try:
        from PIL import Image
        hwnd = _main_window(pid)
        if not hwnd:
            return False
        r = wintypes.RECT()
        _u32.GetWindowRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
        if w <= 0 or h <= 0:
            return False
        wdc = _u32.GetWindowDC(hwnd)
        mdc = _g32.CreateCompatibleDC(wdc)
        bmp = _g32.CreateCompatibleBitmap(wdc, w, h)
        old = _g32.SelectObject(mdc, bmp)
        try:
            if not _u32.PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT):
                return False
            bi = _BITMAPINFOHEADER(ctypes.sizeof(_BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
            buf = ctypes.create_string_buffer(w * h * 4)
            if not _g32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0):
                return False
            img = Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)
            path.parent.mkdir(parents=True, exist_ok=True)
            img.save(path, "JPEG", quality=70)
            return True
        finally:
            _g32.SelectObject(mdc, old)
            _g32.DeleteObject(bmp)
            _g32.DeleteDC(mdc)
            _u32.ReleaseDC(hwnd, wdc)
    except Exception:
        return False


def capture_all(pids: dict[str, int], run_dir: Path, label: str) -> list[str]:
    """One picture per game window: <run_dir>/screens/<label>-<name>.jpg. Returns the names saved."""
    return [name for name, pid in pids.items()
            if capture(pid, run_dir / "screens" / f"{label}-{name}.jpg")]
