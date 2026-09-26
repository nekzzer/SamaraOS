/* Minimal `openssl s_client` for SamaraOS, on top of mbedTLS.
 *
 *   openssl s_client [-quiet] -connect host:port [-servername name]
 *                    [-verify_quiet] [-insecure]
 *
 * Connects, does a TLS 1.2/1.3 handshake (certificates checked against
 * /etc/ssl/certs/ca-certificates.crt), then copies stdin -> TLS and
 * TLS -> stdout. That is exactly what busybox wget expects from it for
 * https:// URLs. SSL_INSECURE=1 in the environment (or -insecure) skips
 * certificate checks. */
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#define CA_FILE "/etc/ssl/certs/ca-certificates.crt"

static void die(const char *what, int err) {
    char buf[160];
    mbedtls_strerror(err, buf, sizeof buf);
    fprintf(stderr, "openssl: %s: %s (-0x%04x)\n", what, buf, (unsigned)-err);
    exit(1);
}

static int write_all(int fd, const unsigned char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *connect_to = NULL, *sni = NULL;
    int quiet = 0, insecure = getenv("SSL_INSECURE") != NULL;
    if (argc < 2 || strcmp(argv[1], "s_client") != 0) {
        fprintf(stderr, "usage: openssl s_client [-quiet] -connect host:port [-servername name] [-insecure]\n"
                        "(SamaraOS: only s_client is implemented, on mbedTLS)\n");
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-connect") && i + 1 < argc) connect_to = argv[++i];
        else if (!strcmp(argv[i], "-servername") && i + 1 < argc) sni = argv[++i];
        else if (!strcmp(argv[i], "-quiet")) quiet = 1;
        else if (!strcmp(argv[i], "-insecure")) insecure = 1;
    }
    if (!connect_to) { fprintf(stderr, "openssl: -connect host:port required\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);

    char host[256], port[16] = "443";
    snprintf(host, sizeof host, "%s", connect_to);
    char *colon = strrchr(host, ':');
    if (colon && !strchr(colon, ']')) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }
    if (host[0] == '[') { memmove(host, host + 1, strlen(host)); char *b = strchr(host, ']'); if (b) *b = 0; }
    if (!sni) sni = host;

    psa_crypto_init();
    mbedtls_net_context net;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_net_init(&net);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&ca);

    int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"samara", 6);
    if (r) die("rng", r);
    int have_ca = !insecure && mbedtls_x509_crt_parse_file(&ca, CA_FILE) >= 0 && ca.version;
    if (!insecure && !have_ca && !quiet)
        fprintf(stderr, "openssl: warning: no CA roots in %s, not verifying\n", CA_FILE);

    if ((r = mbedtls_net_connect(&net, host, port, MBEDTLS_NET_PROTO_TCP))) die("connect", r);
    if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT))) die("config", r);
    mbedtls_ssl_conf_authmode(&conf, have_ca ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    if (have_ca) mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if ((r = mbedtls_ssl_setup(&ssl, &conf))) die("setup", r);
    if ((r = mbedtls_ssl_set_hostname(&ssl, sni))) die("hostname", r);
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char vb[512];
            mbedtls_x509_crt_verify_info(vb, sizeof vb, "  ", mbedtls_ssl_get_verify_result(&ssl));
            fprintf(stderr, "openssl: certificate verification failed for %s:\n%s"
                            "(check the clock with `date`, or use SSL_INSECURE=1)\n", sni, vb);
            return 1;
        }
        die("handshake", r);
    }
    if (!quiet)
        fprintf(stderr, "openssl: connected %s:%s, %s, %s\n", host, port,
                mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));

    unsigned char buf[16384];
    int in_open = 1;
    for (;;) {
        /* Drain what mbedTLS already decrypted before sleeping in poll. */
        if (mbedtls_ssl_get_bytes_avail(&ssl) == 0) {
            struct pollfd p[2] = { { net.fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
            int np = in_open ? 2 : 1;
            if (poll(p, np, -1) < 0) { if (errno == EINTR) continue; break; }
            if (in_open && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t n = read(0, buf, sizeof buf);
                if (n <= 0) in_open = 0;              /* keep reading the reply */
                else {
                    size_t off = 0;
                    while (off < (size_t)n) {
                        r = mbedtls_ssl_write(&ssl, buf + off, (size_t)n - off);
                        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                        if (r < 0) die("write", r);
                        off += (size_t)r;
                    }
                }
            }
            if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        }
        r = mbedtls_ssl_read(&ssl, buf, sizeof buf);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) break;
        if (r == MBEDTLS_ERR_NET_CONN_RESET) break;
        if (r < 0) die("read", r);
        if (write_all(1, buf, (size_t)r) < 0) break;
    }
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&net);
    return 0;
}
