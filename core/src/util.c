#include "fvs.h"

#include <ctype.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

int fvs_uuid_v4(char out[37]) {
    unsigned char b[16];
    if (!out || RAND_bytes(b, (int)sizeof b) != 1) return FVS_ERR_INTERNAL;
    b[6] = (unsigned char)((b[6] & 0x0fU) | 0x40U);
    b[8] = (unsigned char)((b[8] & 0x3fU) | 0x80U);
    int n = snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return n == 36 ? FVS_OK : FVS_ERR_INTERNAL;
}

int fvs_random_hex(char *out, size_t out_size, size_t random_bytes) {
    static const char hex[] = "0123456789abcdef";
    if (!out || random_bytes == 0 || random_bytes > 1024 || out_size < random_bytes * 2U + 1U) return FVS_ERR_BUFFER;
    unsigned char buf[1024];
    if (RAND_bytes(buf, (int)random_bytes) != 1) return FVS_ERR_INTERNAL;
    for (size_t i = 0; i < random_bytes; ++i) {
        out[i * 2U] = hex[(buf[i] >> 4U) & 0x0fU];
        out[i * 2U + 1U] = hex[buf[i] & 0x0fU];
    }
    out[random_bytes * 2U] = '\0';
    return FVS_OK;
}

int fvs_sha256_hex(const char *text, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    if (!text || !out) return FVS_ERR_INVALID;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return FVS_ERR_INTERNAL;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, text, strlen(text)) == 1 &&
             EVP_DigestFinal_ex(ctx, digest, &dlen) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || dlen != 32U) return FVS_ERR_INTERNAL;
    for (unsigned int i = 0; i < dlen; ++i) {
        out[i * 2U] = hex[(digest[i] >> 4U) & 0x0fU];
        out[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    out[64] = '\0';
    return FVS_OK;
}

int fvs_secure_equals(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

int fvs_currency_valid(const char *currency) {
    if (!currency || strlen(currency) != 3U) return 0;
    return isalpha((unsigned char)currency[0]) && isalpha((unsigned char)currency[1]) && isalpha((unsigned char)currency[2]);
}

int fvs_email_valid(const char *email) {
    if (!email) return 0;
    size_t n = strlen(email);
    if (n < 5U || n > 254U) return 0;
    const char *at = strchr(email, '@');
    if (!at || at == email || strchr(at + 1, '@')) return 0;
    const char *dot = strrchr(at + 1, '.');
    if (!dot || dot == at + 1 || dot[1] == '\0') return 0;
    for (const unsigned char *p = (const unsigned char *)email; *p; ++p) {
        if (*p <= 31U || *p == 127U || isspace(*p)) return 0;
    }
    return 1;
}

int fvs_provider_valid(const char *provider) {
    return provider && (strcmp(provider, "stripe") == 0 || strcmp(provider, "mercadopago") == 0);
}

int fvs_add_i64_checked(int64_t a, int64_t b, int64_t *out) {
    if (!out) return FVS_ERR_INVALID;
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return FVS_ERR_OVERFLOW;
    *out = a + b;
    return FVS_OK;
}

int fvs_json_escape(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size == 0U) return FVS_ERR_INVALID;
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        const char *esc = NULL;
        char small[7] = {0};
        size_t need = 1;
        switch (*p) {
            case '"': esc = "\\\""; need = 2; break;
            case '\\': esc = "\\\\"; need = 2; break;
            case '\b': esc = "\\b"; need = 2; break;
            case '\f': esc = "\\f"; need = 2; break;
            case '\n': esc = "\\n"; need = 2; break;
            case '\r': esc = "\\r"; need = 2; break;
            case '\t': esc = "\\t"; need = 2; break;
            default:
                if (*p < 0x20U) {
                    (void)snprintf(small, sizeof small, "\\u%04x", *p);
                    esc = small; need = 6;
                }
                break;
        }
        if (w + need + 1U > out_size) return FVS_ERR_BUFFER;
        if (esc) { memcpy(out + w, esc, need); w += need; }
        else { out[w++] = (char)*p; }
    }
    out[w] = '\0';
    return FVS_OK;
}
