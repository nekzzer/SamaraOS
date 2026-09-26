#!/usr/bin/env python3
"""Pack the musl.cc native i686 GCC (C only) + GNU make into build/gcc.tar,
a ustar archive the kernel unpacks at boot as a multiboot module
(qemu -initrd build/gcc.tar). Everything lands under /opt/gcc:

  /opt/gcc/bin            gcc cc cpp as ld ar ranlib nm objcopy objdump
                          readelf strip size strings addr2line make
  /opt/gcc/libexec/...    cc1 collect2 lto-wrapper
  /opt/gcc/lib            musl libc.a + crt*.o, libgcc (lib/gcc/...)
  /opt/gcc/usr/include    musl headers (gcc looks in <prefix>/usr/include;
                          the upstream tree has usr -> . which the ramfs
                          cannot represent)
  /opt/gcc/i686-linux-musl/bin   as/ld/... (gcc calls these by path)

The ramfs has no links: identical files are stored once and hard-linked
(tar type '1'); the kernel shares their bytes.

A replacement specs file makes `-static -no-pie` the default, since
SamaraOS has no dynamic loader (and this gcc otherwise builds PIE).
"""
import hashlib, io, os, subprocess, sys, tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TC = os.path.join(ROOT, "toolchain/gcc-native/i686-linux-musl-native")
MAKE = os.path.join(ROOT, "toolchain/make-4.4.1/make")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/gcc.tar")
VER = "11.2.1"
GCCLIB = f"lib/gcc/i686-linux-musl/{VER}"
LIBEXEC = f"libexec/gcc/i686-linux-musl/{VER}"

BIN = ["gcc", "cpp", "as", "ld", "ld.bfd", "ar", "ranlib", "nm", "objcopy",
       "objdump", "readelf", "strip", "size", "strings", "addr2line", "c++filt"]
LIBEXEC_FILES = ["cc1", "collect2", "lto-wrapper"]
LIB_FILES = ["crt1.o", "crti.o", "crtn.o", "Scrt1.o", "rcrt1.o", "libc.a", "libm.a",
             "libpthread.a", "librt.a", "libdl.a", "libutil.a", "libxnet.a",
             "libresolv.a", "libcrypt.a", "libatomic.a", "libssp.a",
             "libssp_nonshared.a"]


def specs_text():
    gcc = os.path.join(TC, "bin/gcc")
    s = subprocess.run([gcc, "-dumpspecs"], capture_output=True, text=True, check=True).stdout
    old = "*self_spec:\n\n\n"
    assert old in s, "unexpected specs layout"
    return s.replace(old, "*self_spec:\n%{!shared:%{!static-pie:%{!pie:-static -no-pie}}}\n\n")


def main():
    entries = []                                  # (arcname, src path | bytes, mode)

    def add(arc, src, mode=None):
        entries.append((arc, src, mode))

    for b in BIN:
        add(f"opt/gcc/bin/{b}", os.path.join(TC, "bin", b))
    add("opt/gcc/bin/cc", os.path.join(TC, "bin/gcc"))
    add("opt/gcc/bin/make", MAKE)
    for b in ["as", "ld", "ld.bfd", "ar", "ranlib", "nm", "objcopy", "objdump", "readelf", "strip"]:
        p = os.path.join(TC, "i686-linux-musl/bin", b)
        if os.path.exists(p):
            add(f"opt/gcc/i686-linux-musl/bin/{b}", p)
    for f in LIBEXEC_FILES:
        add(f"opt/gcc/{LIBEXEC}/{f}", os.path.join(TC, LIBEXEC, f))
    for f in LIB_FILES:
        p = os.path.join(TC, "lib", f)
        if os.path.exists(p):
            add(f"opt/gcc/lib/{f}", p)
    gl = os.path.join(TC, GCCLIB)
    for f in sorted(os.listdir(gl)):
        p = os.path.join(gl, f)
        if os.path.isfile(p) and (f.endswith(".o") or f in ("libgcc.a", "libgcc_eh.a", "libgcov.a")):
            add(f"opt/gcc/{GCCLIB}/{f}", p)
    for sub in ["include", "include-fixed"]:
        base = os.path.join(gl, sub)
        for dp, dn, fn in os.walk(base):
            for f in fn:
                p = os.path.join(dp, f)
                add(f"opt/gcc/{GCCLIB}/{sub}/" + os.path.relpath(p, base), p)
    add(f"opt/gcc/{GCCLIB}/specs", specs_text().encode(), 0o644)
    inc = os.path.join(TC, "include")
    for dp, dn, fn in os.walk(inc):
        dn[:] = [d for d in dn if d != "c++"]      # C only
        for f in fn:
            p = os.path.join(dp, f)
            if os.path.islink(p):
                p = os.path.realpath(p)
            add("opt/gcc/usr/include/" + os.path.relpath(os.path.join(dp, f), inc), p)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    seen = {}                                      # content hash -> first arcname
    dirs = set()
    total = stored = 0
    with tarfile.open(OUT, "w", format=tarfile.USTAR_FORMAT) as tar:
        def mkdirs(arc):
            parts = arc.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                d = "/".join(parts[:i])
                if d not in dirs:
                    dirs.add(d)
                    ti = tarfile.TarInfo(d)
                    ti.type = tarfile.DIRTYPE
                    ti.mode = 0o755
                    tar.addfile(ti)

        for arc, src, mode in entries:
            data = src if isinstance(src, bytes) else open(src, "rb").read()
            if mode is None:
                mode = os.stat(src).st_mode & 0o777
            mkdirs(arc)
            h = hashlib.sha1(data).hexdigest()
            total += len(data)
            ti = tarfile.TarInfo(arc)
            ti.mode = mode
            if h in seen and len(data) > 4096:
                ti.type = tarfile.LNKTYPE
                ti.linkname = seen[h]
                tar.addfile(ti)
                continue
            seen.setdefault(h, arc)
            ti.size = len(data)
            stored += len(data)
            tar.addfile(ti, io.BytesIO(data))
    print(f"{OUT}: {len(entries)} files, {stored >> 20} MiB stored ({total >> 20} MiB unpacked)")


if __name__ == "__main__":
    main()
