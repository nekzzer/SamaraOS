#!/usr/bin/env python3
# nm -n -S output on stdin -> ksyms.c (text symbols for the panic backtrace)
import sys
syms = []
last = -1
for l in sys.stdin:
    p = l.split()
    if len(p) != 4 or p[2] not in 'tTwW': continue
    a = int(p[0], 16)
    if a < 0xffffffff80000000 or a == last: continue
    last = a
    syms.append((a, int(p[1], 16), p[3]))
off = []
blob = bytearray()
for a, s, n in syms:
    off.append(len(blob))
    blob += n.encode() + b'\0'
print('const unsigned long ksym_n = %d;' % len(syms))
print('const unsigned long ksym_addr[] = {' + ','.join('0x%x' % s[0] for s in syms) + ',0};')
print('const unsigned int ksym_size[] = {' + ','.join(str(s[1]) for s in syms) + ',0};')
print('const unsigned int ksym_off[] = {' + ','.join(map(str, off)) + ',0};')
print('const char ksym_names[] = {' + ','.join(map(str, blob)) + ',0};')
