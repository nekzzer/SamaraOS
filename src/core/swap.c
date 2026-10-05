#include "core/swap.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/smp.h"
#include "core/vmm.h"
#include "drivers/ata.h"

typedef struct {
    bool on;
    int disk, prio;
    uint32_t pages, used, next;
    uint8_t* map;          /* refs per slot, 0xFF = header / bad */
} area_t;

static area_t ar[SWAP_AREAS];
static spin_t sl, iol;

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int swap_area_of(int disk) {
    for (int i = 0; i < SWAP_AREAS; i++) if (ar[i].on && ar[i].disk == disk) return i;
    return -1;
}

int swap_on(int disk, int prio) {
    static uint8_t hdr[4096] __attribute__((aligned(4096)));
    if (!ata_drive_present(disk)) return -19;
    if (swap_area_of(disk) >= 0) return -16;
    int a = -1;
    for (int i = 0; i < SWAP_AREAS; i++) if (!ar[i].on && !ar[i].map) { a = i; break; }
    if (a < 0) return -24;
    uint64_t f = spin_lock(&iol);
    int r = ata_read(disk, 0, 8, hdr);
    spin_unlock(&iol, f);
    if (r < 0) return -5;
    if (memcmp(hdr + 4096 - 10, "SWAPSPACE2", 10) || le32(hdr + 1024) != 1) return -22;
    uint32_t last = le32(hdr + 1028);
    uint64_t dpages = ata_drive_sectors(disk) / 8;
    if (last + 1 > dpages) last = dpages - 1;
    if (last < 10) return -22;
    area_t* s = &ar[a];
    s->map = kmalloc(last + 1);
    if (!s->map) return -12;
    memset(s->map, 0, last + 1);
    s->map[0] = 0xFF;
    uint32_t nbad = le32(hdr + 1032);
    for (uint32_t i = 0; i < nbad && i < 512; i++) {
        uint32_t b = le32(hdr + 1536 + i * 4);     // badpages[] after uuid, label and 117 words of padding
        if (b && b <= last) s->map[b] = 0xFF;
    }
    s->disk = disk; s->prio = prio; s->pages = last + 1; s->used = 0; s->next = 1;
    s->on = true;
    return 0;
}

int swap_off(int disk) {
    int a = swap_area_of(disk);
    if (a < 0) return -22;
    area_t* s = &ar[a];
    s->on = false;                // no new slots from now
    extern int vmm_swap_drain(int area);
    if (vmm_swap_drain(a) < 0) { s->on = true; return -12; }
    uint64_t f = spin_lock(&sl);
    uint8_t* m = s->map;
    s->map = NULL;
    spin_unlock(&sl, f);
    kfree(m);
    return 0;
}

int swap_active(void) {
    int n = 0;
    for (int i = 0; i < SWAP_AREAS; i++) n += ar[i].on;
    return n;
}

int swap_alloc(void) {
    int ret = -1;
    uint64_t f = spin_lock(&sl);
    // highest priority first, next fit so batches get slots in a row
    int best = -1;
    for (int i = 0; i < SWAP_AREAS; i++)
        if (ar[i].on && ar[i].used < ar[i].pages - 1 && (best < 0 || ar[i].prio > ar[best].prio)) best = i;
    if (best >= 0) {
        area_t* s = &ar[best];
        for (uint32_t n = 0; n < s->pages; n++) {
            uint32_t i = (s->next + n) % s->pages;
            if (s->map[i]) continue;
            s->map[i] = 1;
            s->used++;
            s->next = i + 1;
            ret = (best << 28) | i;
            break;
        }
    }
    spin_unlock(&sl, f);
    return ret;
}

void swap_dup(uint64_t slot) {
    uint64_t f = spin_lock(&sl);
    area_t* s = &ar[slot >> 28];
    uint32_t i = slot & 0xFFFFFFF;
    if (s->map && i < s->pages && s->map[i] && s->map[i] < 254) s->map[i]++;
    spin_unlock(&sl, f);
}

void swap_put(uint64_t slot) {
    uint64_t f = spin_lock(&sl);
    area_t* s = &ar[slot >> 28];
    uint32_t i = slot & 0xFFFFFFF;
    if (s->map && i < s->pages && s->map[i] && s->map[i] < 254 && --s->map[i] == 0) s->used--;
    spin_unlock(&sl, f);
}

int swap_write(uint64_t slot, int n, const void* buf) {
    area_t* s = &ar[slot >> 28];
    uint64_t f = spin_lock(&iol);
    int r = ata_write(s->disk, (uint32_t)(slot & 0xFFFFFFF) * 8, n * 8, buf);
    spin_unlock(&iol, f);
    return r;
}

int swap_read(uint64_t slot, void* buf) {
    area_t* s = &ar[slot >> 28];
    uint64_t f = spin_lock(&iol);
    int r = ata_read(s->disk, (uint32_t)(slot & 0xFFFFFFF) * 8, 8, buf);
    spin_unlock(&iol, f);
    return r;
}

uint64_t swap_total(void) {
    uint64_t n = 0;
    for (int i = 0; i < SWAP_AREAS; i++) if (ar[i].on) n += ar[i].pages - 1;
    return n;
}

uint64_t swap_free(void) {
    uint64_t n = 0;
    for (int i = 0; i < SWAP_AREAS; i++) if (ar[i].on) n += ar[i].pages - 1 - ar[i].used;
    return n;
}

int swap_info(int i, char* name, uint64_t* size, uint64_t* used, int* prio) {
    if (i < 0 || i >= SWAP_AREAS || !ar[i].on) return 0;
    strcpy(name, "/dev/");
    strcat(name, ata_drive_name(ar[i].disk));
    *size = (uint64_t)(ar[i].pages - 1) * 4;
    *used = (uint64_t)ar[i].used * 4;
    *prio = ar[i].prio;
    return 1;
}
