/* calc — tiny expression calculator for SamaraOS.
 *
 *   calc "2+2*(3-1)"     one-shot
 *   calc                 interactive; empty line or "q" quits
 *
 * Operators: + - * / % ^ (power), unary -, parentheses.
 * Functions: sqrt sin cos tan abs. Constants: pi. "ans" = previous result.
 *
 * Build on SamaraOS (busybox sh):  tcc calc.c -o calc     (musl has no libm)
 * Build on the host (static i686): i686-linux-musl-gcc -static -no-pie -Os -s -o calc calc.c -lm
 */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *p;
static int err;
static double ans;

static double expr(void);

static void ws(void) { while (isspace((unsigned char)*p)) p++; }

static double primary(void) {
    ws();
    if (*p == '(') {
        p++;
        double v = expr();
        ws();
        if (*p != ')') { err = 1; return 0; }
        p++;
        return v;
    }
    if (isdigit((unsigned char)*p) || *p == '.') {
        char *e;
        double v = strtod(p, &e);
        p = e;
        return v;
    }
    if (isalpha((unsigned char)*p)) {
        char name[8];
        int n = 0;
        while (isalpha((unsigned char)*p)) {
            if (n < 7) name[n++] = *p;
            p++;
        }
        name[n] = 0;
        if (!strcmp(name, "pi")) return M_PI;
        if (!strcmp(name, "ans")) return ans;
        double a = primary();
        if (!strcmp(name, "sqrt")) { if (a < 0) err = 1; return sqrt(a); }
        if (!strcmp(name, "sin")) return sin(a);
        if (!strcmp(name, "cos")) return cos(a);
        if (!strcmp(name, "tan")) return tan(a);
        if (!strcmp(name, "abs")) return fabs(a);
    }
    err = 1;
    return 0;
}

static double unary(void) {
    ws();
    if (*p == '-') { p++; return -unary(); }
    if (*p == '+') { p++; return unary(); }
    return primary();
}

static double power(void) {          /* right-associative */
    double b = unary();
    ws();
    if (*p == '^') { p++; return pow(b, power()); }
    return b;
}

static double term(void) {
    double v = power();
    for (;;) {
        ws();
        if (*p == '*') { p++; v *= power(); }
        else if (*p == '/') {
            p++;
            double d = power();
            if (d == 0) { err = 2; return 0; }
            v /= d;
        } else if (*p == '%') {
            p++;
            double d = power();
            if (d == 0) { err = 2; return 0; }
            v = fmod(v, d);
        } else return v;
    }
}

static double expr(void) {
    double v = term();
    for (;;) {
        ws();
        if (*p == '+') { p++; v += term(); }
        else if (*p == '-') { p++; v -= term(); }
        else return v;
    }
}

static int eval(const char *s) {
    p = s;
    err = 0;
    double v = expr();
    ws();
    if (!err && *p) err = 1;
    if (err == 2) { puts("error: division by zero"); return 1; }
    if (err) { puts("error: syntax"); return 1; }
    ans = v;
    printf("%.10g\n", v);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        char buf[256] = "";
        for (int i = 1; i < argc; i++) {
            strncat(buf, argv[i], sizeof buf - strlen(buf) - 1);
            strncat(buf, " ", sizeof buf - strlen(buf) - 1);
        }
        return eval(buf);
    }
    puts("SamaraOS calc — empty line or q to quit");
    char line[256];
    for (;;) {
        fputs("> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || !strcmp(line, "q")) break;
        eval(line);
    }
    return 0;
}
