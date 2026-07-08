#include "rfb_client.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/select.h>

#include "esp_log.h"
#include "mbedtls/des.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "rfb_client";

/* RFB message type constants (RFC 6143 §7) */
enum {
    SMSG_FRAMEBUFFER_UPDATE     = 0,
    SMSG_SET_COLOUR_MAP_ENTRIES = 1,
    SMSG_BELL                   = 2,
    SMSG_SERVER_CUT_TEXT        = 3,
};
enum {
    CMSG_SET_PIXEL_FORMAT        = 0,
    CMSG_SET_ENCODINGS            = 2,
    CMSG_FRAMEBUFFER_UPDATE_REQ   = 3,
    CMSG_KEY_EVENT                = 4,
    CMSG_POINTER_EVENT            = 5,
    CMSG_CLIENT_CUT_TEXT          = 6,
};
enum {
    ENC_RAW       = 0,
    ENC_COPY_RECT = 1,
};
enum {
    SEC_TYPE_INVALID  = 0,
    SEC_TYPE_NONE     = 1,
    SEC_TYPE_VNCAUTH  = 2,
    SEC_TYPE_VENCRYPT = 19,
};

struct rfb_client {
    int sock;
    rfb_callbacks_t cb;
    uint16_t fb_width, fb_height;
    volatile bool running;
    SemaphoreHandle_t send_lock;

    /* Set up on demand by start_tls() below, only when the server picks
     * a TLS-wrapped VeNCrypt subtype (X509Plain etc.). Once active, ALL
     * further reads/writes for the rest of the connection - not just the
     * auth exchange - go through this instead of the raw socket; that's
     * how VeNCrypt actually works (the TLS tunnel wraps the entire rest
     * of the RFB session, not just the login). */
    bool use_tls;
    mbedtls_net_context tls_net;
    mbedtls_entropy_context tls_entropy;
    mbedtls_ctr_drbg_context tls_ctr_drbg;
    mbedtls_ssl_config tls_conf;
    mbedtls_ssl_context tls_ssl;
};

/* --- small blocking I/O helpers ------------------------------------------ */

static esp_err_t read_full(rfb_client_t *c, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        int n;
        if (c->use_tls) {
            n = mbedtls_ssl_read(&c->tls_ssl, p + got, len - got);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) return ESP_FAIL; /* includes clean close (0) and any real error */
        } else {
            n = recv(c->sock, p + got, len - got, 0);
            if (n == 0) return ESP_ERR_INVALID_STATE; /* peer closed */
            if (n < 0) {
                if (errno == EINTR) continue;
                return ESP_FAIL;
            }
        }
        got += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t write_full(rfb_client_t *c, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        int n;
        if (c->use_tls) {
            n = mbedtls_ssl_write(&c->tls_ssl, p + sent, len - sent);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) return ESP_FAIL;
        } else {
            n = send(c->sock, p + sent, len - sent, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                return ESP_FAIL;
            }
        }
        sent += (size_t)n;
    }
    return ESP_OK;
}

static inline uint16_t rd_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr_u16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static inline void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* --- VNC "classic" DES authentication (RFC 6143 §7.2.2) ------------------
 * The password (max 8 bytes, zero-padded) is used as a DES key, but each
 * key byte has its bits reversed - a historical quirk baked into every
 * VNC implementation since the original AT&T source. mbedtls's DES wants
 * the key in normal bit order, so we do the reversal ourselves. */

static uint8_t reverse_bits(uint8_t b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

static void vnc_auth_response(const char *password, const uint8_t challenge[16], uint8_t response[16])
{
    uint8_t key[8] = {0};
    size_t plen = password ? strlen(password) : 0;
    if (plen > 8) plen = 8;
    for (size_t i = 0; i < plen; i++) key[i] = reverse_bits((uint8_t)password[i]);
    for (size_t i = plen; i < 8; i++) key[i] = reverse_bits(0);

    mbedtls_des_context des;
    mbedtls_des_init(&des);
    mbedtls_des_setkey_enc(&des, key);
    /* Two independent ECB blocks - NOT chained (this is not CBC). */
    mbedtls_des_crypt_ecb(&des, challenge, response);
    mbedtls_des_crypt_ecb(&des, challenge + 8, response + 8);
    mbedtls_des_free(&des);
}

/* --- TLS wrapping for VeNCrypt's X509-family subtypes ---------------------
 * Wraps the already-connected raw socket in a TLS session using mbedtls,
 * bridging to the existing fd via mbedtls_net_context (just a thin
 * wrapper around an fd - we don't use mbedtls_net_connect() since we
 * already did our own connect() with a bounded timeout). Once this
 * succeeds, read_full()/write_full() route through it automatically for
 * the rest of the connection's lifetime. */

static esp_err_t start_tls(rfb_client_t *c)
{
    mbedtls_net_init(&c->tls_net);
    c->tls_net.fd = c->sock;

    mbedtls_ssl_init(&c->tls_ssl);
    mbedtls_ssl_config_init(&c->tls_conf);
    mbedtls_entropy_init(&c->tls_entropy);
    mbedtls_ctr_drbg_init(&c->tls_ctr_drbg);

    int ret = mbedtls_ctr_drbg_seed(&c->tls_ctr_drbg, mbedtls_entropy_func, &c->tls_entropy, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS RNG seed failed: -0x%04x", (unsigned)-ret);
        return ESP_FAIL;
    }

    ret = mbedtls_ssl_config_defaults(&c->tls_conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS config defaults failed: -0x%04x", (unsigned)-ret);
        return ESP_FAIL;
    }

    /* VeNCrypt's X509* subtypes almost always present a self-signed
     * certificate with no real CA chain behind it - the threat model
     * here is encrypting the link on a private/LAN network, not PKI-
     * based server identity verification. Deliberately not validating
     * the chain matches how other VNC clients handle this in practice. */
    mbedtls_ssl_conf_authmode(&c->tls_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->tls_conf, mbedtls_ctr_drbg_random, &c->tls_ctr_drbg);

    ret = mbedtls_ssl_setup(&c->tls_ssl, &c->tls_conf);
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS setup failed: -0x%04x", (unsigned)-ret);
        return ESP_FAIL;
    }

    mbedtls_ssl_set_bio(&c->tls_ssl, &c->tls_net, mbedtls_net_send, mbedtls_net_recv, NULL);

    do {
        ret = mbedtls_ssl_handshake(&c->tls_ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        char errbuf[80];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        ESP_LOGE(TAG, "TLS handshake failed: %s", errbuf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TLS established for VeNCrypt (cipher: %s)",
             mbedtls_ssl_get_ciphersuite(&c->tls_ssl));
    c->use_tls = true;
    return ESP_OK;
}

static void stop_tls(rfb_client_t *c)
{
    if (!c->use_tls) return;
    mbedtls_ssl_close_notify(&c->tls_ssl);
    mbedtls_ssl_free(&c->tls_ssl);
    mbedtls_ssl_config_free(&c->tls_conf);
    mbedtls_ctr_drbg_free(&c->tls_ctr_drbg);
    mbedtls_entropy_free(&c->tls_entropy);
    c->use_tls = false;
}

/* --- VeNCrypt authentication (security type 19) --------------------------
 * A small sub-negotiation: version handshake, then the server offers a
 * list of "subtype" auth methods (u32 IDs). We support two:
 *  - 256 "Plain": send username+password in cleartext directly.
 *  - 262 "X509Plain": establish TLS first (see start_tls() above), then
 *    send username+password the same way, but now over that TLS session.
 * Other subtypes (TLSNone, TLSPlain, X509None, and the SASL variants) aren't implemented. */
#define VENCRYPT_SUBTYPE_PLAIN      256
#define VENCRYPT_SUBTYPE_X509_PLAIN 262

static esp_err_t negotiate_vencrypt(rfb_client_t *c, const char *username, const char *password)
{
    uint8_t server_ver[2];
    if (read_full(c, server_ver, 2) != ESP_OK) return ESP_FAIL;

    uint8_t our_ver[2] = {0, 2}; /* VeNCrypt 0.2 - the version almost every server supports */
    if (write_full(c, our_ver, 2) != ESP_OK) return ESP_FAIL;

    uint8_t ack;
    if (read_full(c, &ack, 1) != ESP_OK) return ESP_FAIL;
    if (ack != 0) {
        ESP_LOGE(TAG, "Server rejected VeNCrypt version 0.2");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t n;
    if (read_full(c, &n, 1) != ESP_OK) return ESP_FAIL;
    if (n == 0 || n > 16) {
        ESP_LOGE(TAG, "VeNCrypt offered an unreasonable subtype count (%d)", n);
        return ESP_FAIL;
    }
    uint8_t subtypes_raw[16 * 4];
    if (read_full(c, subtypes_raw, (size_t)n * 4) != ESP_OK) return ESP_FAIL;

    bool have_plain = false, have_x509plain = false;
    for (int i = 0; i < n; i++) {
        uint32_t st = rd_u32(subtypes_raw + i * 4);
        if (st == VENCRYPT_SUBTYPE_PLAIN) have_plain = true;
        if (st == VENCRYPT_SUBTYPE_X509_PLAIN) have_x509plain = true;
        /* Standard VeNCrypt subtype IDs (256-264); logged by number with
         * a name where recognized, so a real-world "not supported" case
         * tells us exactly what to implement next instead of just "no". */
        const char *name = "unknown";
        switch (st) {
            case 256: name = "Plain"; break;
            case 257: name = "TLSNone"; break;
            case 258: name = "TLSVnc"; break;
            case 259: name = "TLSPlain"; break;
            case 260: name = "X509None"; break;
            case 261: name = "X509Vnc"; break;
            case 262: name = "X509Plain"; break;
            case 263: name = "TLSSASL"; break;
            case 264: name = "X509SASL"; break;
            default: break;
        }
        ESP_LOGI(TAG, "Server offers VeNCrypt subtype %" PRIu32 " (%s)", st, name);
    }

    uint32_t chosen_subtype;
    if (have_plain) {
        chosen_subtype = VENCRYPT_SUBTYPE_PLAIN;
    } else if (have_x509plain) {
        chosen_subtype = VENCRYPT_SUBTYPE_X509_PLAIN;
    } else {
        ESP_LOGE(TAG, "Server's VeNCrypt subtypes don't include Plain or X509Plain - "
                      "other TLS-wrapped subtypes aren't supported here");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t chosen[4];
    wr_u32(chosen, chosen_subtype);
    if (write_full(c, chosen, 4) != ESP_OK) return ESP_FAIL;

    if (chosen_subtype == VENCRYPT_SUBTYPE_X509_PLAIN) {
        esp_err_t tls_err = start_tls(c);
        if (tls_err != ESP_OK) return tls_err;
        /* From here on, write_full()/read_full() transparently go
         * through the TLS session (c->use_tls is now true) - the
         * username/password send below, and everything for the rest of
         * this connection, is already covered. */
    }

    uint32_t ulen = username ? strlen(username) : 0;
    uint32_t plen = password ? strlen(password) : 0;
    uint8_t lenbuf[4];

    wr_u32(lenbuf, ulen);
    if (write_full(c, lenbuf, 4) != ESP_OK) return ESP_FAIL;
    if (ulen > 0 && write_full(c, username, ulen) != ESP_OK) return ESP_FAIL;

    wr_u32(lenbuf, plen);
    if (write_full(c, lenbuf, 4) != ESP_OK) return ESP_FAIL;
    if (plen > 0 && write_full(c, password, plen) != ESP_OK) return ESP_FAIL;

    return ESP_OK;
}

/* --- handshake ------------------------------------------------------------ */

static esp_err_t do_handshake(rfb_client_t *c, const char *username, const char *password)
{
    uint8_t buf[24];

    /* ProtocolVersion: "RFB xxx.yyy\n" (12 bytes) */
    if (read_full(c, buf, 12) != ESP_OK) return ESP_FAIL;
    buf[12] = 0;
    ESP_LOGI(TAG, "Server protocol: %s", buf);

    int major = 0, minor = 0;
    sscanf((char *)buf, "RFB %d.%3d", &major, &minor);
    bool is_38 = (major > 3) || (major == 3 && minor >= 8);
    bool is_37_plus = (major > 3) || (major == 3 && minor >= 7);

    /* Echo back the same (or a capped) version. We only implement up to 3.8. */
    const char *reply = is_38 ? "RFB 003.008\n" : (is_37_plus ? "RFB 003.007\n" : "RFB 003.003\n");
    if (write_full(c, reply, 12) != ESP_OK) return ESP_FAIL;

    uint8_t chosen_sec = SEC_TYPE_INVALID;

    if (is_37_plus) {
        /* Server sends: u8 number-of-types, then that many u8 types. */
        uint8_t n;
        if (read_full(c, &n, 1) != ESP_OK) return ESP_FAIL;
        if (n == 0) {
            /* Followed by a reason string (u32 length + text). */
            uint8_t lenb[4];
            if (read_full(c, lenb, 4) == ESP_OK) {
                uint32_t rl = rd_u32(lenb);
                uint8_t tmp[64];
                while (rl > 0) {
                    uint32_t chunk = rl > sizeof(tmp) ? sizeof(tmp) : rl;
                    if (read_full(c, tmp, chunk) != ESP_OK) break;
                    rl -= chunk;
                }
            }
            ESP_LOGE(TAG, "Server rejected connection before security negotiation");
            return ESP_FAIL;
        }
        uint8_t types[32];
        if (n > sizeof(types)) n = sizeof(types);
        if (read_full(c, types, n) != ESP_OK) return ESP_FAIL;

        bool has_none = false, has_vncauth = false, has_vencrypt = false;
        for (int i = 0; i < n; i++) {
            if (types[i] == SEC_TYPE_NONE) has_none = true;
            if (types[i] == SEC_TYPE_VNCAUTH) has_vncauth = true;
            if (types[i] == SEC_TYPE_VENCRYPT) has_vencrypt = true;
        }
        /* Prefer None (no auth needed) > VeNCrypt Plain (supports a
         * username, and doesn't preclude an empty one) > classic VNC
         * Auth (password only). */
        chosen_sec = has_none ? SEC_TYPE_NONE
                     : has_vencrypt ? SEC_TYPE_VENCRYPT
                     : has_vncauth ? SEC_TYPE_VNCAUTH
                     : SEC_TYPE_INVALID;
        if (chosen_sec == SEC_TYPE_INVALID) {
            ESP_LOGE(TAG, "No supported security type offered (only None/VeNCrypt-Plain/VNCAuth handled)");
            return ESP_ERR_NOT_SUPPORTED;
        }
        uint8_t choice = chosen_sec;
        if (write_full(c, &choice, 1) != ESP_OK) return ESP_FAIL;
    } else {
        /* 3.3: server unilaterally picks and sends a u32 security-type. */
        uint8_t secb[4];
        if (read_full(c, secb, 4) != ESP_OK) return ESP_FAIL;
        chosen_sec = (uint8_t)rd_u32(secb);
        if (chosen_sec != SEC_TYPE_NONE && chosen_sec != SEC_TYPE_VNCAUTH) {
            ESP_LOGE(TAG, "Unsupported 3.3 security-type %d", chosen_sec);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    if (chosen_sec == SEC_TYPE_VNCAUTH) {
        uint8_t challenge[16], response[16];
        if (read_full(c, challenge, 16) != ESP_OK) return ESP_FAIL;
        vnc_auth_response(password, challenge, response);
        if (write_full(c, response, 16) != ESP_OK) return ESP_FAIL;
    } else if (chosen_sec == SEC_TYPE_VENCRYPT) {
        esp_err_t err = negotiate_vencrypt(c, username, password);
        if (err != ESP_OK) return err;
    }

    /* SecurityResult: always present in 3.8; in 3.3/3.7 only after
     * VNCAuth or VeNCrypt (both are genuine auth attempts the server
     * needs to accept/reject; plain "None" has nothing to check). */
    if (is_38 || chosen_sec == SEC_TYPE_VNCAUTH || chosen_sec == SEC_TYPE_VENCRYPT) {
        uint8_t resb[4];
        if (read_full(c, resb, 4) != ESP_OK) return ESP_FAIL;
        uint32_t result = rd_u32(resb);
        if (result != 0) {
            if (is_38) {
                uint8_t lenb[4];
                if (read_full(c, lenb, 4) == ESP_OK) {
                    uint32_t rl = rd_u32(lenb);
                    uint8_t tmp[128];
                    while (rl > 0) {
                        uint32_t chunk = rl > sizeof(tmp) ? sizeof(tmp) : rl;
                        if (read_full(c, tmp, chunk) != ESP_OK) break;
                        rl -= chunk;
                    }
                }
            }
            ESP_LOGE(TAG, "Authentication failed");
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* ClientInit: u8 shared-flag (1 = allow other viewers to stay connected). */
    uint8_t shared = 1;
    if (write_full(c, &shared, 1) != ESP_OK) return ESP_FAIL;

    /* ServerInit: u16 width, u16 height, PIXEL_FORMAT(16), u32 name-len, name. */
    uint8_t si[24];
    if (read_full(c, si, 24) != ESP_OK) return ESP_FAIL;
    c->fb_width = rd_u16(si);
    c->fb_height = rd_u16(si + 2);
    uint32_t name_len = rd_u32(si + 20);

    char *name = malloc(name_len + 1);
    if (name && name_len > 0) {
        if (read_full(c, name, name_len) != ESP_OK) { free(name); return ESP_FAIL; }
    } else if (name_len > 0) {
        /* malloc failed - drain the bytes so the stream stays in sync. */
        uint8_t tmp[64];
        uint32_t rl = name_len;
        while (rl > 0) {
            uint32_t chunk = rl > sizeof(tmp) ? sizeof(tmp) : rl;
            if (read_full(c, tmp, chunk) != ESP_OK) break;
            rl -= chunk;
        }
    }
    if (name) name[name_len] = 0;

    ESP_LOGI(TAG, "Desktop '%s' is %ux%u", name ? name : "?", c->fb_width, c->fb_height);

    /* SetPixelFormat: request RGB565 explicitly so we never have to deal
     * with the server's native depth (often 32bpp) or byte order. */
    {
        uint8_t msg[20] = {0};
        msg[0] = CMSG_SET_PIXEL_FORMAT;
        uint8_t *pf = msg + 4;
        pf[0] = 16;  /* bits-per-pixel */
        pf[1] = 16;  /* depth */
        pf[2] = 0;   /* big-endian-flag */
        pf[3] = 1;   /* true-colour-flag */
        wr_u16(pf + 4, 31);  /* red-max   */
        wr_u16(pf + 6, 63);  /* green-max */
        wr_u16(pf + 8, 31);  /* blue-max  */
        pf[10] = 11; /* red-shift   */
        pf[11] = 5;  /* green-shift */
        pf[12] = 0;  /* blue-shift  */
        if (write_full(c, msg, sizeof(msg)) != ESP_OK) return ESP_FAIL;
    }

    /* SetEncodings: Raw + CopyRect. */
    {
        uint8_t msg[4 + 2 * 4];
        msg[0] = CMSG_SET_ENCODINGS;
        msg[1] = 0;
        wr_u16(msg + 2, 2);
        wr_u32(msg + 4, ENC_RAW);
        wr_u32(msg + 8, ENC_COPY_RECT);
        if (write_full(c, msg, sizeof(msg)) != ESP_OK) return ESP_FAIL;
    }

    if (c->cb.on_connected) c->cb.on_connected(c->fb_width, c->fb_height, name ? name : "", c->cb.ctx);
    free(name);

    /* Kick off the first (full) update. */
    uint8_t req[10];
    req[0] = CMSG_FRAMEBUFFER_UPDATE_REQ;
    req[1] = 0; /* incremental = 0 -> full frame */
    wr_u16(req + 2, 0);
    wr_u16(req + 4, 0);
    wr_u16(req + 6, c->fb_width);
    wr_u16(req + 8, c->fb_height);
    return write_full(c, req, sizeof(req));
}

/* --- FramebufferUpdate handling ------------------------------------------- */

static esp_err_t handle_framebuffer_update(rfb_client_t *c)
{
    uint8_t hdr[3];
    if (read_full(c, hdr, 3) != ESP_OK) return ESP_FAIL; /* padding + u16 n-rects */
    uint16_t n_rects = rd_u16(hdr + 1);

    /* A generous scratch buffer for raw rows. We read/draw one rectangle
     * at a time in chunks so we never need width*height*2 bytes at once. */
    static uint8_t line_buf[1024 * 2]; /* one full 1024px-wide RGB565 row */

    for (uint16_t i = 0; i < n_rects; i++) {
        uint8_t rb[12];
        if (read_full(c, rb, 12) != ESP_OK) return ESP_FAIL;
        uint16_t x = rd_u16(rb), y = rd_u16(rb + 2), w = rd_u16(rb + 4), h = rd_u16(rb + 6);
        int32_t enc = (int32_t)rd_u32(rb + 8);

        if (enc == ENC_RAW) {
            for (uint16_t row = 0; row < h; row++) {
                size_t row_bytes = (size_t)w * 2;
                /* w should never exceed our scratch row width in practice
                 * (server was told our framebuffer is <=1024 wide via
                 * FramebufferUpdateRequest), but guard anyway. */
                size_t remaining = row_bytes;
                uint8_t *dst = line_buf;
                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(line_buf) ? sizeof(line_buf) : remaining;
                    if (read_full(c, dst, chunk) != ESP_OK) return ESP_FAIL;
                    remaining -= chunk;
                }
                if (c->cb.on_raw_rect) {
                    c->cb.on_raw_rect(x, (uint16_t)(y + row), w, 1, line_buf, c->cb.ctx);
                }
            }
        } else if (enc == ENC_COPY_RECT) {
            uint8_t src[4];
            if (read_full(c, src, 4) != ESP_OK) return ESP_FAIL;
            uint16_t sx = rd_u16(src), sy = rd_u16(src + 2);
            if (c->cb.on_copy_rect) c->cb.on_copy_rect(x, y, w, h, sx, sy, c->cb.ctx);
        } else {
            ESP_LOGW(TAG, "Unsupported encoding %ld, connection will desync - disconnecting", (long)enc);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_OK;
}

static esp_err_t discard_bytes(rfb_client_t *c, uint32_t n)
{
    uint8_t tmp[64];
    while (n > 0) {
        uint32_t chunk = n > sizeof(tmp) ? sizeof(tmp) : n;
        if (read_full(c, tmp, chunk) != ESP_OK) return ESP_FAIL;
        n -= chunk;
    }
    return ESP_OK;
}

esp_err_t rfb_client_run(rfb_client_t *c)
{
    c->running = true;
    esp_err_t err = ESP_OK;

    while (c->running) {
        uint8_t msg_type;
        esp_err_t rd_err = read_full(c, &msg_type, 1);
        if (rd_err != ESP_OK) { err = rd_err; break; }

        switch (msg_type) {
        case SMSG_FRAMEBUFFER_UPDATE:
            err = handle_framebuffer_update(c);
            if (err != ESP_OK) goto out;
            /* Ask for the next incremental update right away - this is
             * what keeps the stream flowing (RFB is receiver-paced). */
            {
                uint8_t req[10];
                req[0] = CMSG_FRAMEBUFFER_UPDATE_REQ;
                req[1] = 1; /* incremental */
                wr_u16(req + 2, 0);
                wr_u16(req + 4, 0);
                wr_u16(req + 6, c->fb_width);
                wr_u16(req + 8, c->fb_height);
                if (write_full(c, req, sizeof(req)) != ESP_OK) { err = ESP_FAIL; goto out; }
            }
            break;

        case SMSG_SET_COLOUR_MAP_ENTRIES: {
            uint8_t hdr[5];
            if (read_full(c, hdr, 5) != ESP_OK) { err = ESP_FAIL; goto out; }
            uint16_t count = rd_u16(hdr + 3);
            if (discard_bytes(c, (uint32_t)count * 6) != ESP_OK) { err = ESP_FAIL; goto out; }
            break;
        }

        case SMSG_BELL:
            /* No payload. Wire this up to a beep/LED if you want. */
            break;

        case SMSG_SERVER_CUT_TEXT: {
            uint8_t hdr[7];
            if (read_full(c, hdr, 7) != ESP_OK) { err = ESP_FAIL; goto out; }
            uint32_t len = rd_u32(hdr + 3);
            if (discard_bytes(c, len) != ESP_OK) { err = ESP_FAIL; goto out; }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown server message type %d - disconnecting", msg_type);
            err = ESP_ERR_NOT_SUPPORTED;
            goto out;
        }
    }

out:
    c->running = false;
    if (c->cb.on_disconnected) c->cb.on_disconnected(err, c->cb.ctx);
    return err;
}

/* --- public API ------------------------------------------------------------ */

rfb_client_t *rfb_client_create(const rfb_callbacks_t *callbacks)
{
    rfb_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->sock = -1;
    if (callbacks) c->cb = *callbacks;
    c->send_lock = xSemaphoreCreateMutex();
    return c;
}

void rfb_client_destroy(rfb_client_t *c)
{
    if (!c) return;
    stop_tls(c);
    if (c->sock >= 0) close(c->sock);
    if (c->send_lock) vSemaphoreDelete(c->send_lock);
    free(c);
}

esp_err_t rfb_client_connect(rfb_client_t *c, const char *host, uint16_t port,
                              const char *username, const char *password, uint32_t connect_timeout_ms)
{
    stop_tls(c); /* clear any TLS state left over from a previous attempt */

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        ESP_LOGE(TAG, "getaddrinfo(%s) failed: %d", host, gai);
        return ESP_FAIL;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return ESP_FAIL; }

    /* Non-blocking connect + select(), so an unreachable host or a wrong
     * IP that just silently drops packets fails within connect_timeout_ms
     * instead of hanging on the kernel's much longer default TCP timeout -
     * important since this is what the setup dialog is bounded by. */
    int orig_flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, orig_flags | O_NONBLOCK);

    int rc = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc != 0 && errno != EINPROGRESS) {
        ESP_LOGE(TAG, "connect() to %s:%u failed immediately: errno %d", host, port, errno);
        close(sock);
        return ESP_FAIL;
    }
    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {
            .tv_sec = connect_timeout_ms / 1000,
            .tv_usec = (connect_timeout_ms % 1000) * 1000,
        };
        int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            ESP_LOGE(TAG, "connect() to %s:%u timed out after %ums", host, port, (unsigned)connect_timeout_ms);
            close(sock);
            return ESP_ERR_TIMEOUT;
        }
        int so_err = 0;
        socklen_t so_len = sizeof(so_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
        if (so_err != 0) {
            ESP_LOGE(TAG, "connect() to %s:%u failed: errno %d", host, port, so_err);
            close(sock);
            return ESP_FAIL;
        }
    }

    /* Back to blocking mode, with a bounded timeout on the handshake reads
     * and writes too - a host that accepts the TCP connection but never
     * speaks RFB shouldn't hang the dialog either. */
    fcntl(sock, F_SETFL, orig_flags);
    struct timeval rw_tv = {
        .tv_sec = connect_timeout_ms / 1000,
        .tv_usec = (connect_timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rw_tv, sizeof(rw_tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &rw_tv, sizeof(rw_tv));

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    c->sock = sock;
    esp_err_t err = do_handshake(c, username, password);
    if (err != ESP_OK) {
        stop_tls(c);
        close(sock);
        c->sock = -1;
        return err;
    }

    /* Handshake is done - lift the timeout for the steady-state receive
     * loop. FramebufferUpdates can legitimately be minutes apart if the
     * remote screen is idle; that's not a dead connection. */
    struct timeval no_tv = {0, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &no_tv, sizeof(no_tv));

    return ESP_OK;
}

void rfb_client_disconnect(rfb_client_t *c)
{
    c->running = false;
    stop_tls(c); /* close_notify needs to write, so do this before the socket goes away */
    if (c->sock >= 0) {
        shutdown(c->sock, SHUT_RDWR);
        close(c->sock);
        c->sock = -1;
    }
}

esp_err_t rfb_client_send_pointer(rfb_client_t *c, uint16_t x, uint16_t y, uint8_t button_mask)
{
    if (c->sock < 0) return ESP_ERR_INVALID_STATE;
    uint8_t msg[6];
    msg[0] = CMSG_POINTER_EVENT;
    msg[1] = button_mask;
    wr_u16(msg + 2, x);
    wr_u16(msg + 4, y);

    xSemaphoreTake(c->send_lock, portMAX_DELAY);
    esp_err_t err = write_full(c, msg, sizeof(msg));
    xSemaphoreGive(c->send_lock);
    return err;
}

esp_err_t rfb_client_send_key(rfb_client_t *c, uint32_t keysym, bool down)
{
    if (c->sock < 0) return ESP_ERR_INVALID_STATE;
    uint8_t msg[8] = {0};
    msg[0] = CMSG_KEY_EVENT;
    msg[1] = down ? 1 : 0;
    wr_u32(msg + 4, keysym);

    xSemaphoreTake(c->send_lock, portMAX_DELAY);
    esp_err_t err = write_full(c, msg, sizeof(msg));
    xSemaphoreGive(c->send_lock);
    return err;
}

esp_err_t rfb_client_request_full_update(rfb_client_t *c)
{
    if (c->sock < 0) return ESP_ERR_INVALID_STATE;
    uint8_t req[10];
    req[0] = CMSG_FRAMEBUFFER_UPDATE_REQ;
    req[1] = 0;
    wr_u16(req + 2, 0);
    wr_u16(req + 4, 0);
    wr_u16(req + 6, c->fb_width);
    wr_u16(req + 8, c->fb_height);

    xSemaphoreTake(c->send_lock, portMAX_DELAY);
    esp_err_t err = write_full(c, req, sizeof(req));
    xSemaphoreGive(c->send_lock);
    return err;
}
