"""Send relative mouse input: rel.py dx dy [click] — moves in small steps so DirectInput sees them."""
import ctypes
import sys
import time

MOVE, LDOWN, LUP = 0x0001, 0x0002, 0x0004


class MI(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long), ("data", ctypes.c_ulong),
                ("flags", ctypes.c_ulong), ("time", ctypes.c_ulong), ("extra", ctypes.c_size_t)]


class INP(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("mi", MI), ("pad", ctypes.c_byte * 8)]


def send(dx, dy, flags):
    i = INP(0, MI(dx, dy, 0, flags, 0, 0))
    ctypes.windll.user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INP))


dx, dy = int(sys.argv[1]), int(sys.argv[2])
steps = max(1, max(abs(dx), abs(dy)) // 20)
for k in range(steps):
    send(dx // steps, dy // steps, MOVE)
    time.sleep(0.01)
send(dx - (dx // steps) * steps, dy - (dy // steps) * steps, MOVE)
if len(sys.argv) > 3:
    time.sleep(0.2)
    send(0, 0, LDOWN)
    time.sleep(0.1)
    send(0, 0, LUP)
