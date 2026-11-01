/*
 * Distribution over TLS (SW_NODE_TLS=1) — regression test for #40.
 *
 * One OS process, one node. The node connects to its own listener through a
 * recording TCP proxy, so the test sees exactly what crosses the wire:
 *
 *   T1  a message sent over the link is delivered;
 *   T2  the secret it carries never appears in the captured bytes (the link
 *       is encrypted);
 *   T3  replaying the captured client->server bytes on a fresh connection
 *       delivers nothing (cross-connection replay);
 *   T4  a plaintext frame with a valid SW_NODE_COOKIE MAC is refused (a TLS
 *       node never falls back to plaintext);
 *   T5  a peer with a different cookie fails the handshake: node_connect
 *       returns an error instead of succeeding into a dead link.
 *
 * Before TLS existed, T2 found the secret in the capture and T3/T4 delivered
 * a second and third copy of the message.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "swarmrt_node.h"
#include "swarmrt_lang.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

#define NODE_PORT  19261
#define PROXY_PORT 19262
#define SECRET     "TOP-SECRET-dist-payload-7f3a91"
#define COOKIE     "test-cookie-0123456789abcdef"

static int passed = 0, failed = 0;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  [PASS] %s\n", name); } \
                               else { failed++; printf("  [FAIL] %s\n", name); } } while (0)

/* ---- recording proxy: PROXY_PORT -> NODE_PORT ---- */

static uint8_t c2s[1 << 20], s2c[1 << 20];
static _Atomic size_t c2s_len = 0, s2c_len = 0;

static int tcp_connect_raw(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) { close(fd); return -1; }
    return fd;
}

static int g_proxy_listen = -1;

static void *proxy_main(void *arg) {
    (void)arg;
    int cli = accept(g_proxy_listen, NULL, NULL);
    if (cli < 0) return NULL;
    int srv = tcp_connect_raw(NODE_PORT);
    if (srv < 0) { close(cli); return NULL; }
    struct pollfd p[2] = { { cli, POLLIN, 0 }, { srv, POLLIN, 0 } };
    uint8_t buf[16384];
    for (;;) {
        if (poll(p, 2, 10000) <= 0) break;
        for (int i = 0; i < 2; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = recv(p[i].fd, buf, sizeof(buf), 0);
            if (n <= 0) goto done;
            uint8_t *rec = i == 0 ? c2s : s2c;
            _Atomic size_t *rl = i == 0 ? &c2s_len : &s2c_len;
            size_t at = atomic_load(rl);
            if (at + (size_t)n <= sizeof(c2s)) { memcpy(rec + at, buf, (size_t)n); atomic_store(rl, at + (size_t)n); }
            if (send(p[1 - i].fd, buf, (size_t)n, 0) != n) goto done;
        }
    }
done:
    close(cli);
    close(srv);
    return NULL;
}

static int start_proxy(void) {
    g_proxy_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_proxy_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(PROXY_PORT) };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    if (bind(g_proxy_listen, (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
        listen(g_proxy_listen, 4) != 0) return -1;
    pthread_t t;
    pthread_create(&t, NULL, proxy_main, NULL);
    pthread_detach(t);
    return 0;
}

static int contains(const uint8_t *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0) return 1;
    return 0;
}

/* ---- receiver: counts deliveries of SECRET ---- */

static _Atomic int g_delivered = 0;
static _Atomic int g_receiver_ready = 0;

static void receiver(void *arg) {
    (void)arg;
    sw_register("tls_target", sw_self());
    atomic_store(&g_receiver_ready, 1);
    for (;;) {
        uint64_t tag = 0;
        void *msg = sw_receive_any(8000, &tag);
        if (!msg && tag == 0) break;
        sw_val_t *v = (sw_val_t *)msg;
        if (v && v->type == SW_VAL_STRING && strcmp(v->v.str, SECRET) == 0)
            atomic_fetch_add(&g_delivered, 1);
    }
}

static _Atomic int g_send_rc = 99;

static void sender(void *arg) {
    (void)arg;
    uint8_t *buf = NULL;
    uint32_t len = 0;
    if (sw_marshal(sw_val_string(SECRET), &buf, &len) != 0) { atomic_store(&g_send_rc, -2); return; }
    atomic_store(&g_send_rc, sw_node_send("self@tls", "tls_target", SW_TAG_NONE, buf, len));
    free(buf);
}

static int wait_delivered(int want, int ms) {
    for (int i = 0; i < ms / 20 && atomic_load(&g_delivered) < want; i++) usleep(20000);
    return atomic_load(&g_delivered);
}

/* A plaintext frame that carries a valid cookie MAC. */
static void plaintext_frame(uint8_t **out, uint32_t *out_len) {
    uint8_t *payload = NULL;
    uint32_t plen = 0;
    sw_marshal(sw_val_string(SECRET), &payload, &plen);
    sw_remote_msg_t h;
    memset(&h, 0, sizeof(h));
    strcpy(h.from_node, "mallory@tls");
    strcpy(h.to_node, "self@tls");
    strcpy(h.to_name, "tls_target");
    h.payload_len = plen;
#ifdef __APPLE__
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, COOKIE, (CC_LONG)strlen(COOKIE));
    CC_SHA256_Update(&c, &h, (CC_LONG)sizeof(h));
    CC_SHA256_Update(&c, payload, plen);
    CC_SHA256_Final(h.mac, &c);
#else
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    unsigned int ol = 0;
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    EVP_DigestUpdate(c, COOKIE, strlen(COOKIE));
    EVP_DigestUpdate(c, &h, sizeof(h));
    EVP_DigestUpdate(c, payload, plen);
    EVP_DigestFinal_ex(c, h.mac, &ol);
    EVP_MD_CTX_free(c);
#endif
    uint32_t total = (uint32_t)sizeof(h) + plen;
    *out_len = 4 + total;
    *out = malloc(*out_len);
    uint32_t nl = htonl(total);
    memcpy(*out, &nl, 4);
    memcpy(*out + 4, &h, sizeof(h));
    memcpy(*out + 4 + sizeof(h), payload, plen);
    free(payload);
}

static void raw_send(const uint8_t *buf, size_t len) {
    int fd = tcp_connect_raw(NODE_PORT);
    if (fd < 0) return;
    (void)!send(fd, buf, len, 0);
    usleep(500000);
    close(fd);
}

static void *accept_and_hang_up(void *arg) {
    int fd = accept(*(int *)arg, NULL, NULL);
    if (fd >= 0) {
        uint8_t buf[4096];
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };   /* plaintext node: no hello comes */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)!recv(fd, buf, sizeof(buf), 0);   /* the ClientHello */
        close(fd);
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== distribution over TLS (SW_NODE_TLS=1) ===\n");
    setenv("SW_NODE_TLS", "1", 1);
    setenv("SW_NODE_COOKIE", COOKIE, 1);

    if (sw_init("node-tls-test", 4) != 0 || sw_io_init() != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    if (start_proxy() != 0) { fprintf(stderr, "proxy bind failed\n"); return 1; }
    if (sw_node_start("self@tls", NODE_PORT) != 0) {
        printf("  [FAIL] node_start with SW_NODE_TLS=1\n");
        return 1;
    }
    sw_spawn(receiver, NULL);
    for (int i = 0; i < 100 && !atomic_load(&g_receiver_ready); i++) usleep(10000);
    usleep(50000);

    int crc = sw_node_connect("self@tls", "127.0.0.1", PROXY_PORT);
    CHECK("node_connect over TLS", crc == 0);

    sw_spawn(sender, NULL);
    int got = wait_delivered(1, 3000);
    CHECK("T1 message delivered over the TLS link", atomic_load(&g_send_rc) == 0 && got == 1);

    usleep(200000);
    size_t cl = atomic_load(&c2s_len), sl = atomic_load(&s2c_len);
    printf("  captured %zu bytes client->server, %zu server->client\n", cl, sl);
    CHECK("T2 secret absent from the wire",
          cl > 0 && !contains(c2s, cl, SECRET) && !contains(s2c, sl, SECRET) &&
          !contains(c2s, cl, "tls_target"));

    /* T3: replay the whole recorded client stream on a new connection. */
    raw_send(c2s, cl);
    got = wait_delivered(2, 800);
    CHECK("T3 replayed stream delivers nothing", got == 1);

    /* T4: a plaintext frame with a correct cookie MAC. */
    uint8_t *pf = NULL;
    uint32_t pfl = 0;
    plaintext_frame(&pf, &pfl);
    raw_send(pf, pfl);
    free(pf);
    got = wait_delivered(2, 800);
    CHECK("T4 plaintext frame refused", got == 1);

    /* T5: a peer that will not complete the handshake (a plaintext node, or
     * one with another cookie, hangs up on our ClientHello) makes
     * node_connect fail instead of returning 'ok' for a dead link. The
     * different-cookie case end to end is in tests/sw/test_node_tls.sw. */
    int dumb = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(dumb, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(PROXY_PORT + 1) };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    bind(dumb, (struct sockaddr *)&sin, sizeof(sin));
    listen(dumb, 2);
    pthread_t closer;
    pthread_create(&closer, NULL, accept_and_hang_up, &dumb);
    printf("  (expect a handshake failure message below)\n");
    int brc = sw_node_connect("nobody@tls", "127.0.0.1", PROXY_PORT + 1);
    CHECK("T5 handshake failure is node_connect's error", brc != 0 && !sw_node_is_connected("nobody@tls"));
    pthread_join(closer, NULL);
    close(dumb);

    sw_node_stop();
    sw_io_shutdown();
    sw_shutdown(0);
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
