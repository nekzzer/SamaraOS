# TODO SamaraOS

Продолжение работы после коммитов `f645b60` (userland/шрифты/Paint) и
`199acfd` (ядро API пользовательских окон). Ниже всё, что осталось,
в порядке зависимости. Ядровая часть оконного API **уже готова и собрана** —
юзерленд её может использовать без правок ядра.

---

## 1. Заголовок `userland/samara/samara.h` (C / TCC) — НЕ СДЕЛАНО

Header-only библиотека для программ на C, которые собираются TCC внутри ОС
или musl-gcc на хосте. Основа — syscall 500 (`SYS_SAMARA`).

Ядровый интерфейс (см. `src/gui/uwin.h`, реализация `src/gui/uwin.c`):

| op | ecx | edx | esi | результат |
|----|-----|-----|-----|-----------|
| 1 OPEN | `sm_open_t*` `{w,h,scale,flags,title}` | – | – | handle ≥0 / -errno; блокируется до появления окна; `-ENODEV` вне десктопа |
| 2 PRESENT | handle | user-указатель на пиксели XRGB | – | 0 / `-EPIPE` (окно закрыли крестиком) |
| 3 EVENT | handle | `sm_event_t*` `{type,a,b,c}` | timeout ms, -1 = ждать | 1/0/-EINTR |
| 4 CLOSE | handle | – | – | 0 |
| 5 TEXT | `sm_text_t*` `{buf,bw,bh,x,y,font,color,str}` | – | – | x пера; `buf==0` → только ширина |
| 6 KEYS | handle | `uint8_t[32]` out | – | биты зажатых клавиш (0, если окно не в фокусе) |
| 7 MOUSE | handle | `int[3]` out x,y,кнопки | – | 1 если курсор внутри |
| 8 TITLE | handle | char* | – | 0 |
| 9 FONT_H | font (в ebx!) | – | – | высота строки |

- Типы событий: `SM_EV_KEY` (a = символ CP866, спец-клавиши 0x81+ как
  `K_UP`… из `drivers/keyboard.h`), `SM_EV_MOUSE_DOWN/UP` (a=x, b=y, c=кнопка),
  `SM_EV_MOUSE_MOVE`, `SM_EV_CLOSE`.
- Координаты мыши уже поделены на scale. Scale 1..8, окна ≤ 8 штук на систему
  (`UWIN_MAX`), буфер ≤ 1600×1000.
- Кодировки: ядро само превращает UTF-8 кириллицу в CP866 внутри `ustr()`.
- Шрифты для TEXT: 0..4 = Golos (UIF_REG/MED/SMALL/BIG/HUGE, см. `gfx/uifont.h`),
  16 = `UIF_MONO` 8x16 (терминальный).

Содержимое samara.h:
- `static long sm_call(op,a,b,c){ return syscall(SYS_SAMARA,op,a,b,c); }`
  (нужны `<sys/syscall.h>`, `<unistd.h>`, `<stdint.h>`, `<malloc.h>`);
- структура `SmWin {int w,h,scale; uint32_t *pix;}` — **буфер живёт в
  программе**, ядро хранит свою копию только на время отрисовки;
- `sm_open/sm_present/sm_event/sm_close/sm_keys/sm_mouse/sm_title/sm_font_h`;
- рисование в свой буфер: `sm_pixel/sm_hline/sm_vline/sm_rect/sm_frame/
  sm_line (Брезенхэм)/sm_circle/sm_disc/sm_clear`;
- `sm_text()` через op 5 (текст рисует ядро своим шрифтом — растеризатор
  тащить в юзерленд не нужно);
- `sm_ticks()` через `clock_gettime(CLOCK_MONOTONIC)`, `sm_sleep_ms()` через
  `nanosleep`;
- константы: `SM_RGB(r,g,b)`, цвета, клавиши `SMK_UP=103…SMK_LEFT=106,
  SMK_A=30` (Linux keycodes — они же в байтах из KEYS), `SM_EV_*`,
  `SM_FONT_REG…SM_FONT_MONO=16`.
- Пример использования: `userland/samara/hello.c`.

## 2. Модуль MicroPython `samara` — НЕ СДЕЛАНО

Файлы: `userland/samara/micropython/samara.c` + `samara_mod.mak`:

```make
SAMARA_MOD_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SRC_USER_MOD += $(SAMARA_MOD_DIR)samara.c
```

Сборка в `userland/build-micropython.sh` — добавить к make unix-порта
`USER_C_MODULES=$(abspath .../samara_mod.mak)` (репозиторий уже в
`toolchain/micropython`, подмодуль lib/micropython-lib инициализирован,
mpy-cross собран). Если unix-порт не подхватит USER_C_MODULES — fallback:
скопировать samara.c в `ports/unix/` и дописать его в SRC_C через sed.

API модуля (обёртка над теми же op; буфер хранить в объекте окна,
`m_new(uint32_t, w*h)`):
- `samara.Window(w, h, scale=1, title=...)` → объект с handle;
- методы: `clear/pixel/rect/frame/line/circle/disc` (чистый C по буферу,
  без syscall), `text(x,y,s,color,font=REG)` через op 5, `present()` (op 2,
  после закрытия окна кидает `OSError(EPIPE)`), `ev(block=False)` → tuple
  `(type,a,b,c)`/None, `keys()` → bytes(32), `mouse()` → (x,y,btn),
  `title(s)`, `close()`;
- константы `EV_*`, `KEY_*` (символы CP866: стрелки 0x81+, буквы = коды
  CP866), `FONT_REG…FONT_MONO`, `rgb(r,g,b)`, `WHITE…`;
- `samara.ticks_ms()` = `mp_hal_ticks_ms()`, `samara.sleep_ms()` =
  `mp_hal_delay_ms()`;
- регистрация: `MP_REGISTER_MODULE(MP_QSTR_samara, mp_module_samara);`
- строку в text давать как `mp_obj_str_get_data` (UTF-8 — ядро сконвертирует).

Потом: пересобрать micropython, обновить бинарник в sysroot
(`build-sysroot.sh`), пересобрать `sysroot.tar` **и ядро** (tar вшит в
samara.elf как .o).

## 3. Иконки на рабочем столе — НЕ СДЕЛАНО (`src/gui/wm.c`)

- Слева колонка плиток ~72x84: Terminal, Browser, Music, Paint, Clock +
  содержимое `/usr/games` (`*.py` → запуск `micropython <файл>`, ELF по
  magic `\x7FELF` — запуск по имени; читается через `fs_readdir`/`fs_read`,
  как в ls шелла).
- Рисование: после бликa `bg_cache` в `paint_region()` (строка ~763),
  до отрисовки окон. Иконка: скруглённый квадрат с глифом (примитивы, как в
  Paint) + подпись `UIF_SMALL` снизу.
- Клики: в `on_press()` ветка `wi < 0` (мимо окон) — hit-тест по плиткам:
  одиночный клик = выделение, повторный по той же плитке < 400 мс
  (`pit_uptime_ms()`) = запуск.
- Запуск встроенных — переиспользовать `menu_run(action)`. Для /usr/games:
  добавить `wm_terminal_feed(const char* line)` — терминал живёт прямо в
  wm.c (WIN_TERMINAL, поля term_x/term_y): если терминала нет — открыть,
  вписать строку в буфер строки ввода и отправить `'\n'` тем же путём, что
  и `on_key` терминала. Внимание: если в шелле уже запущена задача, команду
  лучше просто не вводить (или открыть второй терминал).

## 4. Обои из BMP — НЕ СДЕЛАНО (`src/gui/wm.c build_background()`)

- Перед градиентом: попробовать `/home/user/wallpaper.bmp`, `/mnt/wallpaper.bmp`,
  `/wallpaper.bmp`. Парсер: 24/32 bpp, BI_RGB (compression==0), строки
  снизу-вверх (высота>0) или сверху-вниз (высота<0), BGR → XRGB; пиксели
  читать по указателю из заголовка (смещение 10), W=18, H=22, bpp=28,
  compression=30.
- Cover-масштаб до WxH экрана ближайшим соседом + затемнение на ~15%
  (чтобы иконки и wordmark читались). Не удалось прочитать — оставить
  текущий градиент.
- Обновление: флаг `bg_dirty` + `damage_rect` на весь экран; пересборка
  кэша в `collect_damage`/`paint_region`.
- Шелл-команда `wallpaper [файл|off]`: копирует файл в
  `/home/user/wallpaper.bmp` (fs_open/fs_read/fs_create/fs_write, как в
  `src/apps/paint.c save_bmp()`) и дёргает `wm_invalidate_wallpaper()`;
  `off` — удалить файл. Без аргументов — применить текущий.

## 5. Демо-игры — НЕ СДЕЛАНО

- `userland/samara/tetris.c` (~150 строк): поле 10x20, клетка 16px, scale 2
  → окно 320x440; стрелки (KEYS, автоповтор руками по `sm_ticks()`), up —
  поворот, gravity по тикам, удаление линий, счёт в заголовке (TITLE).
- `userland/samara/breakout.py` (~90 строк): ракетка по keys()/мыши, мяч,
  кирпичи, жизни; цикл `while True: … present(); sleep_ms(16)`.
- `userland/samara/hello.c` и `hello.py` — минимальные примеры API.
- В sysroot: `/usr/include/samara.h`, `/usr/src/samara/hello.{c,py}`,
  `/usr/src/games/{tetris.c,breakout.py}`, `/usr/games/breakout.py`.
  Сборка tetris **внутри ОС**: `tcc tetris.c -o /usr/bin/tetris` (флаги
  посмотреть в `userland/build-*.sh`, как линкуют с musl). Проверка на хосте:
  `i686-linux-musl-gcc -static -no-pie` — тот же заголовок должен работать.

## 6. Тесты (после 1–5)

В `/tmp/qsend.py` с `QSOCK=/tmp/qt.sock` (окно пользователя не трогать!):
1. `python -c "import samara"` — модуль грузится.
2. `python /usr/src/samara/hello.py` — окно открывается, рисует.
3. `tcc /usr/src/games/tetris.c -o /usr/bin/tetris && tetris` — играется.
4. Двойной клик по иконкам, `wallpaper`, `wallpaper off`.
5. Закрытие окна крестиком → `present()` даёт EPIPE, программа выходит;
   `kill %1` / Ctrl+C при блокированном `ev()` — процесс не висит.
6. Скриншоты: `@screendump`.

## 7. Прочее (из обсуждения, не срочно)

- TLS (mbedtls) для micropython/браузера — https.
- Изменение размера окон за угол, двойной клик по заголовку = максимум.
- Русификация меню/приложений (шрифт уже умеет CP866).
- Paint: «Открыть» BMP.
- Файловый менеджер.

## Заметки окружения

- Запущены ДВА QEMU: PID 17458 (старая сборка) и 19524 (новая). Оба
  подключены к одному `disk.img`, у нового locking=off — **закрой старое
  окно**, иначе можно испортить FAT на /mnt.
- Пересборка sysroot: `userland/build-sysroot.sh` → `userland/sysroot.tar`,
  затем `make` (tar вшит в ядро).
- Автор для git: `-c user.name=nekzzer -c user.email=sauvageleslie1@gmail.com`
  (глобальный конфиг не настроен; config не менять).
