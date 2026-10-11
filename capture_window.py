import ctypes, time
from ctypes import wintypes

u = ctypes.windll.user32
g = ctypes.windll.gdi32

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

from PIL import Image


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
        ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", ctypes.c_long), ("biYPelsPerMeter", ctypes.c_long),
        ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


h = u.FindWindowW("HWMonWndClass", None)
print("hwnd =", h)
if not h:
    raise SystemExit("window not found")

u.ShowWindow(h, 5)          # SW_SHOW
u.SetWindowPos(h, -1, 0, 0, 0, 0, 0x0001 | 0x0002)   # TOPMOST, NOMOVE|NOSIZE
u.SetWindowPos(h, -2, 0, 0, 0, 0, 0x0001 | 0x0002)   # NOTOPMOST
time.sleep(0.8)

r = wintypes.RECT()
u.GetWindowRect(h, ctypes.byref(r))
w = r.right - r.left
ht = r.bottom - r.top
print("window size =", w, ht, "visible =", bool(u.IsWindowVisible(h)), "iconic =", bool(u.IsIconic(h)))

hdcScreen = u.GetDC(0)
hdcMem = g.CreateCompatibleDC(hdcScreen)

bmi = BITMAPINFO()
bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
bmi.bmiHeader.biWidth = w
bmi.bmiHeader.biHeight = -ht
bmi.bmiHeader.biPlanes = 1
bmi.bmiHeader.biBitCount = 32
bmi.bmiHeader.biCompression = 0

ppv = ctypes.c_void_p()
hbmp = g.CreateDIBSection(hdcScreen, ctypes.byref(bmi), 0, ctypes.byref(ppv), None, 0)
print("dib ok =", bool(hbmp), "ptr =", ppv.value)

g.SelectObject(hdcMem, hbmp)
res = u.PrintWindow(h, hdcMem, 2)     # PW_RENDERFULLCONTENT
print("PrintWindow =", res)

buf = ctypes.string_at(ppv, w * ht * 4)
im = Image.frombuffer("RGBA", (w, ht), buf, "raw", "BGRA", 0, 1)
im = im.convert("RGB")
out = r"D:\My_programs\AI_workspace\temp\HardwareMonitor\_shot2.png"
im.save(out)
print("saved", out, im.size)

g.DeleteObject(hbmp)
g.DeleteDC(hdcMem)
u.ReleaseDC(0, hdcScreen)
