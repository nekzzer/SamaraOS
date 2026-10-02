/* node for SamaraOS: quickjs engine + the node api written in js (node.js),
   plus a few socket natives so net/http can be done in js. Not real node. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "quickjs-libc.h"

extern const uint32_t node_boot_size;
extern const uint8_t node_boot[];

static JSValue n_socket(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    }
    return JS_NewInt32(ctx, fd < 0 ? -errno : fd);
}

static int get_addr(JSContext *ctx, JSValueConst ip, JSValueConst port, struct sockaddr_in *a) {
    const char *s = JS_ToCString(ctx, ip);
    int p = 0;
    JS_ToInt32(ctx, &p, port);
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(p);
    int ok = s && inet_pton(AF_INET, s, &a->sin_addr) == 1;
    JS_FreeCString(ctx, s);
    return ok ? 0 : -1;
}

static JSValue n_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd;
    struct sockaddr_in a;
    JS_ToInt32(ctx, &fd, argv[0]);
    if (get_addr(ctx, argv[1], argv[2], &a)) return JS_NewInt32(ctx, -EINVAL);
    return JS_NewInt32(ctx, bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ? -errno : 0);
}

static JSValue n_listen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd;
    JS_ToInt32(ctx, &fd, argv[0]);
    return JS_NewInt32(ctx, listen(fd, 16) < 0 ? -errno : 0);
}

static JSValue n_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd;
    struct sockaddr_in a;
    JS_ToInt32(ctx, &fd, argv[0]);
    if (get_addr(ctx, argv[1], argv[2], &a)) return JS_NewInt32(ctx, -EINVAL);
    return JS_NewInt32(ctx, connect(fd, (struct sockaddr *)&a, sizeof a) < 0 ? -errno : 0);
}

/* -> [fd, "ip", port] or negative errno */
static JSValue n_accept(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd;
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    JS_ToInt32(ctx, &fd, argv[0]);
    int c = accept(fd, (struct sockaddr *)&a, &l);
    if (c < 0) return JS_NewInt32(ctx, -errno);
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewInt32(ctx, c));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewString(ctx, inet_ntoa(a.sin_addr)));
    JS_SetPropertyUint32(ctx, r, 2, JS_NewInt32(ctx, ntohs(a.sin_port)));
    return r;
}

static JSValue n_nonblock(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd, on = 1;
    JS_ToInt32(ctx, &fd, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &on, argv[1]);
    int f = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
    return JS_UNDEFINED;
}

/* recv into an ArrayBuffer: (fd, ab, off, len) -> n, 0 = eof, negative errno */
static JSValue n_recv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd, off, len;
    size_t sz;
    JS_ToInt32(ctx, &fd, argv[0]);
    uint8_t *p = JS_GetArrayBuffer(ctx, &sz, argv[1]);
    JS_ToInt32(ctx, &off, argv[2]);
    JS_ToInt32(ctx, &len, argv[3]);
    if (!p || off < 0 || len < 0 || (size_t)(off + len) > sz) return JS_NewInt32(ctx, -EINVAL);
    int n = recv(fd, p + off, len, 0);
    return JS_NewInt32(ctx, n < 0 ? -errno : n);
}

static JSValue n_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd, off, len;
    size_t sz;
    JS_ToInt32(ctx, &fd, argv[0]);
    uint8_t *p = JS_GetArrayBuffer(ctx, &sz, argv[1]);
    JS_ToInt32(ctx, &off, argv[2]);
    JS_ToInt32(ctx, &len, argv[3]);
    if (!p || off < 0 || len < 0 || (size_t)(off + len) > sz) return JS_NewInt32(ctx, -EINVAL);
    int n = send(fd, p + off, len, 0);
    return JS_NewInt32(ctx, n < 0 ? -errno : n);
}

static JSValue n_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *h = JS_ToCString(ctx, argv[0]);
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int e = h ? getaddrinfo(h, NULL, &hints, &ai) : -1;
    JS_FreeCString(ctx, h);
    if (e) return JS_NULL;
    JSValue r = JS_NewString(ctx, inet_ntoa(((struct sockaddr_in *)ai->ai_addr)->sin_addr));
    freeaddrinfo(ai);
    return r;
}

static JSValue n_shutdown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fd;
    JS_ToInt32(ctx, &fd, argv[0]);
    shutdown(fd, SHUT_WR);
    return JS_UNDEFINED;
}

/* run source as a global script under a given file name (stack traces, import()) */
static JSValue n_evalas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len;
    const char *src = JS_ToCStringLen(ctx, &len, argv[0]);
    const char *fn = JS_ToCString(ctx, argv[1]);
    if (!src || !fn) return JS_EXCEPTION;
    JSValue r = JS_Eval(ctx, src, len, fn, JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, fn);
    return r;
}

static const JSCFunctionListEntry net_funcs[] = {
    JS_CFUNC_DEF("evalAs", 2, n_evalas),
    JS_CFUNC_DEF("socket", 0, n_socket),
    JS_CFUNC_DEF("bind", 3, n_bind),
    JS_CFUNC_DEF("listen", 1, n_listen),
    JS_CFUNC_DEF("connect", 3, n_connect),
    JS_CFUNC_DEF("accept", 1, n_accept),
    JS_CFUNC_DEF("nonblock", 2, n_nonblock),
    JS_CFUNC_DEF("recv", 4, n_recv),
    JS_CFUNC_DEF("send", 4, n_send),
    JS_CFUNC_DEF("resolve", 1, n_resolve),
    JS_CFUNC_DEF("shutdown", 1, n_shutdown),
};

static int net_init(JSContext *ctx, JSModuleDef *m) {
    return JS_SetModuleExportList(ctx, m, net_funcs, sizeof(net_funcs) / sizeof(net_funcs[0]));
}

static JSModuleDef *init_net_module(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, net_init);
    if (m) JS_AddModuleExportList(ctx, m, net_funcs, sizeof(net_funcs) / sizeof(net_funcs[0]));
    return m;
}

/* import fs from 'fs' / 'node:fs': js side builds the module source */
static JSModuleDef *esm_loader(JSContext *ctx, const char *name, void *opaque) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, g, "__node_esm_source");
    JSValue arg = JS_NewString(ctx, name);
    JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
    if (JS_IsString(r)) {
        size_t len;
        const char *src = JS_ToCStringLen(ctx, &len, r);
        JSValue m = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        JS_FreeCString(ctx, src);
        JS_FreeValue(ctx, r);
        if (JS_IsException(m)) return NULL;
        JSModuleDef *md = JS_VALUE_GET_PTR(m);
        JS_FreeValue(ctx, m);
        return md;
    }
    JS_FreeValue(ctx, r);
    return js_module_loader(ctx, name, opaque);
}

static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, JS_BOOL handled, void *opaque) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, g, "__node_rejection");
    if (JS_IsFunction(ctx, f)) {
        JSValue a[3] = { promise, reason, JS_NewBool(ctx, handled) };
        JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 3, a);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
}

static void die(JSContext *ctx) {
    js_std_dump_error(ctx);
    exit(1);
}

int main(int argc, char **argv) {
    JSRuntime *rt = JS_NewRuntime();
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    JS_SetMaxStackSize(rt, 1024 * 1024);
    JS_SetHostPromiseRejectionTracker(rt, rejection, NULL);
    JS_SetModuleLoaderFunc(rt, NULL, esm_loader, NULL);
    js_std_add_helpers(ctx, argc, argv);
    js_init_module_std(ctx, "__std");
    js_init_module_os(ctx, "__os");
    init_net_module(ctx, "__net");

    const char *init =
        "import * as std from '__std'; import * as os from '__os'; import * as net from '__net';"
        "globalThis.__std = std; globalThis.__os = os; globalThis.__net = net;";
    JSValue v = JS_Eval(ctx, init, strlen(init), "<init>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) die(ctx);
    JS_FreeValue(ctx, v);

    js_std_eval_binary(ctx, node_boot, node_boot_size, 0);
    const char *go = "__node_main()";
    v = JS_Eval(ctx, go, strlen(go), "<main>", 0);
    if (JS_IsException(v)) die(ctx);
    JS_FreeValue(ctx, v);

    js_std_loop(ctx);
    const char *be = "__node_before_exit()";
    v = JS_Eval(ctx, be, strlen(be), "<beforeExit>", 0);
    if (JS_IsException(v)) die(ctx);
    JS_FreeValue(ctx, v);
    js_std_loop(ctx);

    const char *end = "__node_exit()";
    v = JS_Eval(ctx, end, strlen(end), "<exit>", 0);
    int code = 0;
    if (JS_IsException(v)) die(ctx);
    JS_ToInt32(ctx, &code, v);
    JS_FreeValue(ctx, v);
    fflush(stdout);
    _exit(code);
}
