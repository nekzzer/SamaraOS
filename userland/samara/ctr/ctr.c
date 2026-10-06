// ctr: tiny container runner. ctr run <image> cmd args...
// image is unpacked once into /var/lib/ctr/<name> with skopeo (or an alpine minirootfs tarball if there is no skopeo)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define ROOT "/var/lib/ctr"

static int sh(const char* fmt, const char* a, const char* b) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), fmt, a, b);
    return system(cmd);
}

static int fetch(const char* name, const char* dir) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/.dl-%s", ROOT, name);
    sh("rm -rf %s; mkdir -p %s", tmp, tmp);
    mkdir(dir, 0755);
    if (!sh("skopeo copy docker://%s dir:%s >/dev/null", name, tmp)) {
        // layers in manifest order, each one is a gzip tar named by its digest
        char mf[300], buf[65536];
        snprintf(mf, sizeof(mf), "%s/manifest.json", tmp);
        FILE* f = fopen(mf, "r");
        if (!f) return -1;
        int n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        char* p = strstr(buf, "\"layers\"");
        while (p && (p = strstr(p, "\"digest\""))) {
            char* d = strstr(p, "sha256:");
            if (!d) break;
            d += 7;
            char hex[80];
            int k = 0;
            while (k < 64 && d[k] != '"') { hex[k] = d[k]; k++; }
            hex[k] = 0;
            char path[400];
            snprintf(path, sizeof(path), "%s/%s", tmp, hex);
            sh("tar xzf %s -C %s 2>/dev/null", path, dir);
            p = d;
        }
    } else {
        // no skopeo / no registry: alpine minirootfs from the cdn
        if (sh("wget -q -O %s/rootfs.tgz https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz", tmp, "")) return -1;
        sh("tar xzf %s/rootfs.tgz -C %s 2>/dev/null", tmp, dir);
    }
    sh("rm -rf %s", tmp, "");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4 || strcmp(argv[1], "run")) {
        fprintf(stderr, "usage: ctr run <image> cmd [args]\n");
        return 1;
    }
    const char* name = argv[2];
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/%s", ROOT, name);
    sh("mkdir -p %s%s", ROOT, "");
    struct stat st;
    char probe[320];
    snprintf(probe, sizeof(probe), "%s/etc", dir);
    if (stat(probe, &st) && fetch(name, dir)) { fprintf(stderr, "ctr: fetch failed\n"); return 1; }

    if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET) < 0) { perror("unshare"); return 1; }
    int pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid) {
        int s;
        waitpid(pid, &s, 0);
        return WIFEXITED(s) ? WEXITSTATUS(s) : 128 + WTERMSIG(s);
    }

    // we are pid 1 in the new ns
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (mount(dir, dir, NULL, MS_BIND | MS_REC, NULL) < 0) { perror("bind"); _exit(1); }
    chdir(dir);
    mkdir("oldroot", 0755);
    if (syscall(SYS_pivot_root, ".", "oldroot") < 0) { perror("pivot_root"); _exit(1); }
    chdir("/");
    umount2("/oldroot", MNT_DETACH);
    rmdir("/oldroot");
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);
    mkdir("/tmp", 01777);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    sethostname("ctr", 3);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", "/root", 1);
    execvp(argv[3], argv + 3);
    perror(argv[3]);
    _exit(127);
}
