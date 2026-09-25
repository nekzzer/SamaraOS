# Breakout for SamaraOS (MicroPython + samara).
# Left/right or the mouse moves the paddle, space launches, P pauses,
# R restarts, Esc quits.
import samara, random

W, H, S = 240, 180, 3
BW, BH, COLS, ROWS = 20, 8, 11, 6
PW, PH = 36, 4
COLORS = (samara.RED, samara.ORANGE, samara.YELLOW, samara.GREEN, samara.CYAN, samara.BLUE)
BG = samara.rgb(0x17, 0x19, 0x1D)
OX = (W - COLS * BW) // 2

w = samara.Window(W, H, scale=S, title="Breakout")

def reset(full):
    global bricks, px, bx, by, vx, vy, stuck, lives, score, level
    if full:
        lives, score, level = 3, 0, 1
    if full or not any(bricks):
        bricks = [1] * (COLS * ROWS)
    px, stuck = (W - PW) // 2, True
    bx, by, vx, vy = px + PW / 2, H - 20.0, 0.0, 0.0

bricks = []
reset(True)
paused = False
last_mouse = None
try:
    while True:
        e = w.ev()
        while e:
            t = e[0]
            if t == samara.EV_CLOSE:
                raise SystemExit
            if t == samara.EV_KEY:
                k = e[1]
                if k == samara.KEY_ESC:
                    raise SystemExit
                if k in (ord("p"), ord("P")):
                    paused = not paused
                if k in (ord("r"), ord("R")):
                    reset(True)
            if t == samara.EV_MOUSE_DOWN and stuck and lives > 0:
                stuck = False
            e = w.ev()

        if not paused and lives > 0:
            if w.key(samara.SC_LEFT):
                px -= 4
            if w.key(samara.SC_RIGHT):
                px += 4
            mx, my, _ = w.mouse()
            if 0 <= mx < W and 0 <= my < H and (mx, my) != last_mouse:
                if last_mouse is not None:
                    px = mx - PW // 2
                last_mouse = (mx, my)
            px = max(0, min(W - PW, px))
            if stuck:
                bx, by = px + PW / 2, H - 14.0
                if w.key(samara.SC_SPACE):
                    stuck = False
                    vx, vy = random.choice((-1.6, 1.6)), -2.2 - 0.2 * level
            else:
                bx += vx
                by += vy
                if bx < 2 or bx > W - 2:
                    vx = -vx
                    bx = max(2, min(W - 2, bx))
                if by < 2:
                    vy, by = -vy, 2
                if vy > 0 and H - 12 <= by <= H - 8 and px - 2 <= bx <= px + PW + 2:
                    vy = -abs(vy)
                    vx = (bx - (px + PW / 2)) / (PW / 2) * 2.6
                col, row = (int(bx) - OX) // BW, (int(by) - 20) // BH
                if 0 <= row < ROWS and 0 <= col < COLS and bricks[row * COLS + col]:
                    bricks[row * COLS + col] = 0
                    vy = -vy
                    score += 10 * (ROWS - row)
                    if not any(bricks):
                        level += 1
                        reset(False)
                if by > H:
                    lives -= 1
                    reset(False)

        w.clear(BG)
        for r in range(ROWS):
            for c in range(COLS):
                if bricks[r * COLS + c]:
                    w.rect(OX + c * BW + 1, 20 + r * BH + 1, BW - 2, BH - 2, COLORS[r])
        w.rect(px, H - 10, PW, PH, samara.PAPER)
        w.disc(int(bx), int(by), 2, samara.WHITE)
        w.text(4, 2, "SCORE %d" % score, samara.GRAY, samara.FONT_MONO)
        w.text(W - 8 * 9 - 4, 2, "LIVES %d" % lives, samara.GRAY, samara.FONT_MONO)
        if lives <= 0:
            w.text(W // 2 - 36, H // 2, "GAME OVER", samara.ORANGE, samara.FONT_MONO)
            w.text(W // 2 - 44, H // 2 + 18, "R = restart", samara.GRAY, samara.FONT_MONO)
        elif paused:
            w.text(W // 2 - 24, H // 2, "PAUSED", samara.ORANGE, samara.FONT_MONO)
        elif stuck:
            w.text(W // 2 - 52, H // 2 + 10, "SPACE = launch", samara.GRAY, samara.FONT_MONO)
        w.present()
        samara.sleep_ms(16)
except OSError:          # EPIPE: window closed under us
    pass
finally:
    w.close()
