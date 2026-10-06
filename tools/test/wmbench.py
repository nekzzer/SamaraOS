#!/usr/bin/env python3
# input for the desktop benchmark, straight into the qemu monitor (one socket, no sleep per command like mon.py)
# usage: wmbench.py <monitor socket> [shot.ppm]
import socket, sys, time, math

s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.settimeout(2)
try: s.recv(4096)
except Exception: pass

def cmd(c, wait=False):
    s.send((c + '\n').encode())
    if wait:
        time.sleep(0.5)
    try: s.recv(65536)
    except Exception: pass

W, H = 1600, 900
def mv(x, y): cmd('mouse_move %d %d' % (x * 32767 // W, y * 32767 // H))
def click(x, y):
    mv(x, y); time.sleep(0.05)
    cmd('mouse_button 1'); time.sleep(0.05); cmd('mouse_button 0'); time.sleep(0.2)

def key(k):
    cmd('sendkey ' + k)

def circle(sec, hz=60):
    t0 = time.time(); n = 0
    while time.time() - t0 < sec:
        a = n * 0.12
        mv(800 + 500 * math.cos(a), 450 + 300 * math.sin(a))
        n += 1
        time.sleep(1.0 / hz)

def typing(text, hz=12):
    for ch in text:
        key('spc' if ch == ' ' else ch)
        time.sleep(1.0 / hz)

shot = sys.argv[2] if len(sys.argv) > 2 else None
if shot:
    cmd('screendump ' + shot, True)

circle(8)                         # pointer over the whole desktop, over foot too
click(300, 200)                   # kernel terminal
typing('ls usr lib bin ' * 3)
click(1100, 500)                  # foot lands somewhere around here
typing('echo hello from foot ' * 3)
circle(4, 120)
time.sleep(1)
