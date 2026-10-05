#!/usr/bin/env python3
# python3 tools/prof.py serial.log [samara64.elf]  -- reads `cat /proc/samara/prof` output
import sys, subprocess, bisect, collections
log = open(sys.argv[1], errors='replace').read().splitlines()
elf = sys.argv[2] if len(sys.argv) > 2 else 'build/samara64.elf'
syms = []
for l in subprocess.check_output(['nm', '-n', elf], text=True).splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
sysn = {0:'read',1:'write',2:'open',3:'close',4:'stat',5:'fstat',7:'poll',8:'lseek',9:'mmap',10:'mprotect',11:'munmap',12:'brk',13:'rt_sigaction',
        17:'pread',20:'writev',21:'access',23:'select',24:'sched_yield',35:'nanosleep',39:'getpid',56:'clone',57:'fork',59:'execve',61:'wait4',
        72:'fcntl',78:'getdents',202:'futex',217:'getdents64',230:'clock_nanosleep',232:'epoll_wait',257:'openat',262:'newfstatat',281:'epoll_pwait',
        318:'getrandom',500:'uwin',45:'recvfrom',44:'sendto',47:'recvmsg',46:'sendmsg',16:'ioctl',18:'pwrite',19:'readv',87:'unlink',89:'readlink',
        90:'chmod',233:'epoll_ctl',291:'epoll_create1',425:'uring_setup',426:'uring_enter'}
hs = collections.Counter(); ks = collections.Counter(); us = {}; ys = []; cs = []; head = ''
for l in log:
    p = l.split()
    if not p: continue
    if p[0] == 'T' and len(p) > 8: head = l
    elif p[0] == 'C' and len(p) == 3: cs.append((p[1], int(p[2])))
    elif p[0] == 'Y' and len(p) == 4: ys.append((int(p[1]), int(p[2]), int(p[3])))
    elif p[0] == "U" and len(p) == 4: us[p[1]] = us.get(p[1], 0) + int(p[3])
    elif p[0] in ('K', 'H') and len(p) == 3:
        try: a = int(p[1], 16); i = bisect.bisect_right(addrs, a) - 1
        except ValueError: continue
        (hs if p[0] == 'H' else ks)[syms[i][1] if i >= 0 else '?'] += int(p[2])
print(head)
for k, v in cs: print('  %-14s %d' % (k, v))
tot = sum(ks.values()) or 1
print('kernel samples by symbol:')
for k, v in ks.most_common(25): print('  %5.1f%% %6d %s' % (100.0 * v / tot, v, k))
print('kernel without bkl_take:')
rest = tot - ks.get('bkl_take', 0) or 1
for k, v in ks.most_common(40):
    if k != 'bkl_take': print('  %5.1f%% %6d %s' % (100.0 * v / rest, v, k))
print('while holding bkl:')
ht = sum(hs.values()) or 1
for k, v in hs.most_common(20): print('  %5.1f%% %6d %s' % (100.0 * v / ht, v, k))
print('user samples by proc:')
for k, v in sorted(us.items(), key=lambda x: -x[1])[:12]: print('  %6d %s' % (v, k))
print('syscalls by time (Mcycles):')
for nr, n, c in sorted(ys, key=lambda x: -x[2])[:15]: print('  %-14s n=%-7d %8.1f Mcyc' % (sysn.get(nr, nr), n, c / 1e6))
