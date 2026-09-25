# TODO SamaraOS

## Сделано

- [x] **1. `userland/samara/samara.h`**: header-only C API над syscall 500
  (окна, события, KEYS/MOUSE/TITLE, примитивы рисования, `sm_text` через ядро,
  `sm_ticks`/`sm_sleep_ms`, константы `SMK_*`, `SM_CH_*`, `SM_FONT_*`, цвета).
  Собирается и musl-gcc (`-static -no-pie`), и tcc.
- [x] **2. Модуль MicroPython `samara`**: `userland/samara/micropython/samara/`
  (`samara.c` + `micropython.mk`), подключён через `USER_C_MODULES` в
  `userland/build-micropython.sh`. `Window(w,h,scale,title)`, `clear/fill/pixel/
  rect/frame/line/circle/disc/text/present/ev/keys/key/mouse/title/buffer/close`,
  `rgb/ticks_ms/sleep_ms/text_width/font_height`, `EV_*`, `KEY_*`, `SC_*`,
  `FONT_*`, цвета. Буфер хранится в malloc и освобождается финализатором.
  `present()` после закрытия окна кидает `OSError(EPIPE)`.
- [x] **3. Иконки на рабочем столе** (`src/gui/wm.c`): Terminal, Browser, Music,
  Paint, Clock + `/usr/games` (`*.py` → `python …`, ELF → по пути). Пересканирование
  раз в 1,5 с. Клик выделяет, двойной клик (< 400 мс) или Enter запускает.
  Команды игр уходят в терминал через `wm_terminal_feed()`/`wm_terminal_submit()`
  (если терминал занят, запуска нет).
- [x] **4. Обои из BMP**: `/home/user`, `/mnt`, `/wallpaper.bmp`, 24/32 bpp,
  растягивание на весь экран с обрезкой, затемнение ~15%, флаг `bg_dirty`.
  Шелл-команда `wallpaper [файл|off]` (копирует файл на `/mnt`, если диск
  смонтирован).
- [x] **5. Демо-игры**: `tetris.c`, `breakout.py`, `hello.c`, `hello.py`. В sysroot
  лежат `/usr/include/samara.h`, `/usr/src/samara/`, `/usr/src/games/`,
  `/usr/games/{tetris,breakout.py}` (tetris заранее собран musl-gcc).
  `/usr/games` добавлен в PATH.
- [x] Пересобраны micropython, `sysroot.tar` и ядро.

## В работе

- [ ] **Зафиксить баг: краш десктопа при нажатии клавиши ESC.**

- [ ] **6. Тесты** (QEMU на копии `/tmp/disk-test.img`, монитор `/tmp/qt.sock`):
  - [x] двойной клик по иконке tetris: окно открывается, игра идёт, счёт в заголовке;
  - [ ] `python -c "import samara"` внутри ОС;
  - [ ] `python /usr/src/samara/hello.py`, breakout с иконки;
  - [ ] `tcc /usr/src/games/tetris.c -o /usr/bin/tetris && tetris`;
  - [ ] `wallpaper /mnt/wp.bmp`, `wallpaper off`;
  - [ ] закрытие окна крестиком → EPIPE → выход; Ctrl+C / kill при
    заблокированном `ev()`;
  - [ ] скриншоты `@screendump`.
- [ ] Закоммитить (автор: `-c user.name=nekzzer -c user.email=sauvageleslie1@gmail.com`).

## Прочее (не срочно)

- TLS (mbedtls) для micropython/браузера — https.
- Изменение размера окон за угол, двойной клик по заголовку = максимум.
- Русификация меню/приложений (шрифт уже умеет CP866).
- Paint: «Открыть» BMP.
- Файловый менеджер.
- Запуск игр с иконок в фоне (сейчас одна программа на терминал).

## Заметки окружения

- Пересборка: `userland/build-micropython.sh` → `userland/build-sysroot.sh` → `make`
  (tar вшит в ядро).
- Не запускать два QEMU на одном `disk.img` (можно испортить FAT на /mnt).
