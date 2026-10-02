/* domjs in.html out.html base-url: parse a page, run its scripts against a
   tiny DOM (dom.js), write the resulting html. Used by the browser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "quickjs-libc.h"

/* promise callbacks: dom.js runs them after every script, timer and event */
static JSValue js_jobs(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSContext *c;
    for (int i = 0; i < 20000; i++)
        if (JS_ExecutePendingJob(JS_GetRuntime(ctx), &c) <= 0) break;
    return JS_UNDEFINED;
}
/* live pages stay around after the first render: the load alarm goes */
static JSValue js_alarm(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t s = 0;
    if (argc) JS_ToInt32(ctx, &s, argv[0]);
    alarm((unsigned)s);
    return JS_UNDEFINED;
}

extern const uint32_t dom_boot_size;
extern const uint8_t dom_boot[];

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: domjs in out base-url\n"); return 2; }
    alarm(20);                                   /* the browser falls back to the raw page */
    JSRuntime *rt = JS_NewRuntime();
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    JS_SetMaxStackSize(rt, 512 * 1024);
    js_std_add_helpers(ctx, argc, argv);
    js_init_module_std(ctx, "__std");
    js_init_module_os(ctx, "__os");
    const char *init = "import * as std from '__std'; import * as os from '__os';"
                       "globalThis.__std = std; globalThis.__os = os;";
    JSValue v = JS_Eval(ctx, init, strlen(init), "<init>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) { js_std_dump_error(ctx); return 1; }
    JS_FreeValue(ctx, v);
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__jobs", JS_NewCFunction(ctx, js_jobs, "__jobs", 0));
    JS_SetPropertyStr(ctx, g, "__alarm", JS_NewCFunction(ctx, js_alarm, "__alarm", 1));
    JS_FreeValue(ctx, g);
    js_std_eval_binary(ctx, dom_boot, dom_boot_size, 0);
    const char *go = "__dom_main()";
    v = JS_Eval(ctx, go, strlen(go), "<main>", 0);
    if (JS_IsException(v)) { js_std_dump_error(ctx); return 1; }
    js_std_loop(ctx);
    return 0;
}
