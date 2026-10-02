#include "apps/browser.c"
/* host test entry */
int h_run(const char* html, int n, int w, int dump) {
    if (!resp) {
        resp = kmalloc_big(RESP_CAP); pool = kmalloc_big(POOL_CAP); url_pool = kmalloc_big(URLPOOL_CAP);
        runs = kmalloc_big(sizeof(run_t) * MAX_RUNS); nodes = kmalloc_big(sizeof(node_t) * MAX_NODES);
        clsh = kmalloc_big(sizeof(uint32_t) * MAX_CLS); comps = kmalloc_big(sizeof(comp_t) * MAX_COMPS);
        rules = kmalloc_big(sizeof(rule_t) * MAX_RULES); decls = kmalloc_big(sizeof(decl_t) * MAX_DECLS);
        aconds = kmalloc_big(sizeof(acond_t) * MAX_ACONDS); boxes = kmalloc_big(sizeof(bx_t) * MAX_BOXES);
    }
    if (n > RESP_CAP - 1) n = RESP_CAP - 1;
    memcpy(resp, html, n);
    resp_len = utf8_to_cp866(resp, n);
    plain_text = false;
    scpy(base_url, URL_MAX, "https://news.ycombinator.com/");
    view_box.h = 700;
    laid_w = w;
    layout(w);
    if (dump) {
        extern int printf(const char*, ...);
        printf("runs %d boxes %d nodes %d rules %d decls %d doc_h %d title '%s'\n", n_runs, n_boxes, n_nodes, n_rules, n_decls, doc_h, page_title);
        for (int i = 0; i < n_runs && i < dump; i++) {
            run_t* r = &runs[i];
            printf("R%d k%d x%d y%d w%d h%d f%d s%d fg%06x fl%d '%.*s'\n", i, r->kind, r->x, r->y, r->w, r->h, r->font, r->scale, r->fg, r->flags, r->kind == RK_TEXT ? r->len : 0, pool + r->off);
        }
        for (int i = 0; i < n_boxes && i < dump; i++) {
            bx_t* b = &boxes[i];
            printf("B%d x%d y%d w%d h%d bg%06x a%d bw%d,%d,%d,%d r_at%d\n", i, b->x, b->y, b->w, b->h, b->bg, b->bga, b->bw[0], b->bw[1], b->bw[2], b->bw[3], b->r_at);
        }
    }
    return n_runs;
}

/* whole document into the stub framebuffer, like br_paint does */
int h_paint(int fw) {
    view_box = (box_t){ 0, 0, fw, 20000 };
    scroll_y = 0;
    gfx_set_clip(0, 0, fw, 20000);
    clip_x = 0; clip_y = 0; clip_w = fw; clip_h = 20000;
    gfx_rect_fill(0, 0, fw, 20000, page_bg_set ? page_bg : COL_PAPER);
    int ox = (fw - laid_w) / 2, oy = 0;
    if (ox < 0) ox = 0;
    int cur = -2, bi = 0;
    for (int i = 0; i <= n_runs; i++) {
        for (; bi < n_boxes && boxes[bi].r_at <= i; bi++) {
            bx_t* b = &boxes[bi];
            if (b->clip != cur) { cur = b->clip; use_clip(cur, ox, oy); }
            draw_box(b, ox, oy);
        }
        if (i == n_runs) break;
        run_t* r = &runs[i];
        if (r->clip != cur) { cur = r->clip; use_clip(cur, ox, oy); }
        draw_run(i, ox, oy);
    }
    return doc_h;
}

/* which rules hit the element with this id (BT_ID=...) */
void h_why(const char* id) {
    extern int printf(const char*, ...);
    uint32_t h = hsh(id, (int)strlen(id));
    for (int n = 1; n < n_nodes; n++) {
        if (nodes[n].text || nodes[n].idh != h) continue;
        collect(n);
        for (int m = 0; m < n_mt; m++) {
            rule_t* r = &rules[mt[m]];
            for (int d = r->d0; d < r->d0 + r->nd; d++)
                printf("rule %d spec %d pel %d: %.*s: %.*s%s\n", mt[m], r->spec, r->pel, decls[d].nl, decls[d].n, decls[d].vl, decls[d].v, decls[d].imp ? " !imp" : "");
        }
    }
}
