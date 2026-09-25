# Minimal `samara` demo: window, text, shapes, mouse.  python hello.py
import samara

w = samara.Window(360, 220, title="Привет, samara")
t0 = samara.ticks_ms()
clicks = 0
try:
    while True:
        e = w.ev()
        while e:
            if e[0] == samara.EV_CLOSE or (e[0] == samara.EV_KEY and e[1] == samara.KEY_ESC):
                raise SystemExit
            if e[0] == samara.EV_MOUSE_DOWN:
                clicks += 1
            e = w.ev()
        mx, my, btn = w.mouse()
        t = samara.ticks_ms() - t0
        w.clear(samara.PAPER)
        w.text(16, 12, "Hello from Python!  Привет!", samara.DARK, samara.FONT_BIG)
        w.text(16, 44, "t = %d ms   clicks = %d   mouse = %d,%d" % (t, clicks, mx, my),
               samara.GRAY, samara.FONT_SMALL)
        w.rect(16, 80, 80, 50, samara.ORANGE)
        w.frame(110, 80, 80, 50, samara.BLUE)
        w.disc(240, 105, 25, samara.GREEN)
        w.circle(310, 105, 25, samara.RED)
        w.line(16, 150, 16 + t // 8 % 328, 200, samara.MAGENTA)
        w.text(16, 200, "Esc / X closes", samara.DARK, samara.FONT_MONO)
        if mx >= 0 and my >= 0:
            w.disc(mx, my, 4, samara.RED)
        w.present()
        samara.sleep_ms(16)
except OSError:          # EPIPE: window closed under us
    pass
finally:
    w.close()
