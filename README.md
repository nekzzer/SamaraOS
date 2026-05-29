# SamaraOS

Маленькая хобби-ОС на чистом C для x86. Ядро Multiboot 1, грузится через
`qemu-system-i386 -kernel`. Внутри: текстовый шелл с нормальным редактором,
многозадачность, и **настоящий графический рабочий стол через linear framebuffer**.

## Возможности

- Multiboot 1, без отдельного `.asm` (всё в `naked`/`interrupt` функциях C)
- Вытесняющая многозадачность через PIT IRQ + переключение контекста
- PS/2 клавиатура (Ctrl/Shift/Alt/Caps, стрелки, Home/End/Del/PgUp/PgDn)
- PS/2 мышь (3-байтовые пакеты)
- In-memory файловая система с деревом
- Шелл: `help clear echo pwd ls cd cat mkdir touch rm uptime mem ps mouse
  reboot nano neofetch desktop`. Стрелки/Home/End/Del + история (^/v).
- nano — нормальный редактор: курсор, прокрутка, ^S сохранить, ^X/^Q выйти
- **Графический рабочий стол**: 1024×768×32 через Bochs VBE (универсально
  работает в QEMU stdvga); fallback в VGA mode 13h (320×200×8)
  - PCI scan чтобы найти физический адрес фреймбуфера
  - Шрифт VGA BIOS вытаскивается из plane 2 при буте
  - Курсор-стрелка как нарисованный sprite поверх FB
  - Окна с заголовком, тенью, рамкой; живые часы
  - Esc — возврат в шелл (программирует CRTC обратно в текстовый режим 03h
    и заливает шрифт обратно в plane 2)

## Сборка / запуск

Тулчейн: `C:\cross\bin\i686-elf-gcc.exe`, `qemu-system-i386` из MSYS2 UCRT64.

```bash
cd /c/Users/omar/SamaraOS
mingw32-make            # собрать
mingw32-make run        # запустить в QEMU (`-vga std` обязательно для VBE)
```

Из QEMU выйти: Ctrl+A, X.

## Графический режим

В QEMU `-kernel` Multiboot framebuffer-флаг игнорируется, поэтому для перехода
в графику ОС сама программирует видеокарту через **Bochs VBE расширение**
(порты `0x01CE`/`0x01CF`):

1. Проверка `VBE_ID` (должен быть `0xB0Cx`)
2. `XRES=1024, YRES=768, BPP=32, ENABLE = 1 | LFB`
3. PCI configuration space (mech #1) сканируется на класс 0x03 (display);
   читается BAR0 — физический адрес LFB

В QEMU stdvga LFB живёт по `0xFD000000`, в i440fx-машине BIOS его сам туда
маппит. Без paging адрес доступен напрямую.

Если VBE недоступен (например, реальное железо без Bochs-расширения), идёт
fallback в **VGA mode 13h** (320×200×8) через прямое программирование
sequencer/CRTC/AC регистров. Палитра — 16-цветная курируемая.

## Структура

```
src/
  kernel.c       multiboot header, _start (naked), kmain
  types.h io.h   inline asm для портов и базовые типы
  string.{c,h}   memset/memcpy/memmove/str*/itoa
  vga.{c,h}      80x25 текст, цвета, vga_set_text_mode_3()
  gdt.{c,h}      GDT, плоские 4 GiB сегменты
  idt.{c,h}      IDT + установка gate
  pic.{c,h}      8259 PIC remap, EOI
  pit.{c,h}      100 Hz таймер
  keyboard.{c,h} PS/2 + Ctrl-letter -> ASCII control codes
  mouse.{c,h}    PS/2, переключатель текстового курсора, set_pos/range
  heap.{c,h}     first-fit kmalloc/kfree
  task.{c,h}     preemptive multitasking, 8 KB стек на задачу
  fs.{c,h}       in-memory дерево файлов
  font.{c,h}     извлечение и восстановление VGA BIOS шрифта (plane 2)
  multiboot.h    структура multiboot info
  gfx.{c,h}      framebuffer, primitives, VBE+PCI, mode13h fallback
  desktop.{c,h}  scene рендер + цикл с курсором мыши
  shell.{c,h}    readline (стрелки + история), команды
linker.ld        multiboot at 1 MiB, entry _start
Makefile         i686-elf-gcc + qemu-system-i386
```

## Window Manager (`wm.c/h`)

Команда `desktop` запускает WM:

- **Перетаскивание окон** — клик на title bar и тяни. Координаты обновляются и
  всё перерисовывается (полный redraw, без back-buffer'а — fast enough at 1024x768).
- **Закрытие окна** — красная кнопка X в правом верхнем углу.
- **Z-order**: клик по окну поднимает его наверх; новые окна сверху.
- **Start menu в стиле Windows**: кнопка "Start" слева снизу с 4-square логотипом,
  popup открывается над таскбаром. Пункты: Terminal, About, Welcome, System Info,
  Reboot, Shutdown, Exit Desktop. Hover-эффект, клик мимо закрывает меню.
- **Таскбар**: кнопки всех открытых окон, фокус подсвечен; часы в правом углу
  обновляются раз в секунду.
- **Esc** — выход из десктопа (если меню открыто, сначала закрывает меню).

### Типы окон

```c
typedef enum {
    WIN_INFO,      // статичный текст в окне
    WIN_TERMINAL,  // окно с шеллом (singleton — один на десктоп)
    WIN_APP,       // generic app с paint+key callbacks
} window_type_t;
```

### App API (для портирования DOOM и подобного)

```c
window_t* wm_open_app(int x, int y, int w, int h, const char* title,
                      void (*on_paint)(window_t* w),
                      void (*on_key)(window_t* w, char c),
                      void* user);
void wm_client_rect(window_t* w, int* x, int* y, int* cw, int* ch);
void wm_close(window_t* w);
```

`on_paint` вызывается каждый кадр — рисуй в client area через `gfx_pixel/rect/string`
или blitни свой back buffer прямо в фреймбуффер. `on_key` получает ASCII / спец-коды
(`K_LEFT`/`K_RIGHT`/`K_UP`/`K_DOWN`/`K_ESC`).

Пример встроен — команда `bounce` (доступна в терминале десктопа): открывает окно
с прыгающим мячом, реагирующим на стрелки.

### Как портировать DOOM (план)

1. Положить `doom.wad` в RAM-FS (или загрузить через ATA-драйвер позже).
2. Создать `doom_paint(window_t* w)` — вызывает `D_DoomFrame()` за один тик игры,
   потом блитит DOOM-овский 320×200×8 буфер в client area через `gfx_pixel`/blit32.
3. Создать `doom_key(window_t* w, char c)` — мапит наши `K_*` коды в DOOM keys.
4. Из shell вызвать `wm_open_app(..., doom_paint, doom_key, NULL)`.
5. Заменить DOOM-овские `malloc/free` на `kmalloc/kfree`, `printf` на `vga_printf`,
   `fopen/fread` на `fs_resolve/fs_read`.

Всё что DOOM-у нужно от ОС у нас уже есть: framebuffer, keyboard polling, heap,
timer, файлы, multitasking.

## Что добавить дальше

- ATA PIO драйвер для реального диска и WAD-файлов
- Звуковая карта (Sound Blaster 16 или AC97) для DOOM-овского звука
- Resize windows (углы для перетаскивания)
- Пользовательский режим (ring 3) и сисколлы
- ELF-загрузчик чтобы апы были отдельными бинарниками
- Сеть (RTL8139)
