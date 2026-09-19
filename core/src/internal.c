#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fvs_i_seterr(fvs_ctx *ctx, const char *fmt, ...) {
    if (!ctx || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(ctx->last_error, sizeof ctx->last_error, fmt, ap);
    va_end(ap);
}

int fvs_i_db_status(fvs_ctx *ctx, const char *where) {
    if (!ctx || !ctx->db) return FVS_ERR_DB;
    const unsigned int err = mysql_errno(ctx->db);
    fvs_i_seterr(ctx, "%s: (%u) %s", where ? where : "mysql", err, mysql_error(ctx->db));
    return (err == 1205U || err == 1213U) ? FVS_ERR_RETRY : FVS_ERR_DB;
}

int fvs_i_exec(fvs_ctx *ctx, const char *sql) {
    if (!ctx || !ctx->db || !sql) return FVS_ERR_INVALID;
    return mysql_query(ctx->db, sql) == 0 ? FVS_OK : fvs_i_db_status(ctx, "mysql_query");
}

int fvs_i_queryf(fvs_ctx *ctx, MYSQL_RES **res, const char *fmt, ...) {
    if (!ctx || !ctx->db || !res || !fmt) return FVS_ERR_INVALID;
    *res = NULL;
    va_list ap;
    va_start(ap, fmt);
    va_list cp;
    va_copy(cp, ap);
    const int needed = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (needed < 0) {
        va_end(ap);
        fvs_i_seterr(ctx, "query format failure");
        return FVS_ERR_INTERNAL;
    }
    char *sql = malloc((size_t)needed + 1U);
    if (!sql) {
        va_end(ap);
        fvs_i_seterr(ctx, "out of memory");
        return FVS_ERR_INTERNAL;
    }
    const int written = vsnprintf(sql, (size_t)needed + 1U, fmt, ap);
    va_end(ap);
    if (written != needed) {
        free(sql);
        fvs_i_seterr(ctx, "query format mismatch");
        return FVS_ERR_INTERNAL;
    }
    const int query_rc = mysql_query(ctx->db, sql);
    free(sql);
    if (query_rc != 0) return fvs_i_db_status(ctx, "query");
    *res = mysql_store_result(ctx->db);
    if (!*res && mysql_field_count(ctx->db) != 0U) return fvs_i_db_status(ctx, "store_result");
    return FVS_OK;
}

char *fvs_i_escape(fvs_ctx *ctx, const char *text) {
    if (!ctx || !ctx->db || !text) return NULL;
    const size_t len = strlen(text);
    if (len > 1024U * 1024U) {
        fvs_i_seterr(ctx, "input too large");
        return NULL;
    }
    char *out = malloc(len * 2U + 1U);
    if (!out) {
        fvs_i_seterr(ctx, "out of memory");
        return NULL;
    }
    const unsigned long written = mysql_real_escape_string(ctx->db, out, text, (unsigned long)len);
    out[written] = '\0';
    return out;
}

int fvs_i_valid_uuid(const char *text) {
    if (!text || strlen(text) != 36U) return 0;
    for (size_t idx = 0U; idx < 36U; ++idx) {
        const unsigned char ch = (unsigned char)text[idx];
        if (idx == 8U || idx == 13U || idx == 18U || idx == 23U) {
            if (ch != (unsigned char)'-') return 0;
        } else if (!((ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
                     (ch >= (unsigned char)'a' && ch <= (unsigned char)'f') ||
                     (ch >= (unsigned char)'A' && ch <= (unsigned char)'F'))) {
            return 0;
        }
    }
    return 1;
}

void fvs_i_json_init(fvs_i_json *writer, char *buf, size_t cap) {
    if (!writer) return;
    writer->buf = buf;
    writer->cap = cap;
    writer->len = 0U;
    writer->failed = (!buf || cap == 0U) ? 1 : 0;
    if (!writer->failed) buf[0] = '\0';
}

void fvs_i_json_puts(fvs_i_json *writer, const char *text) {
    if (!writer || writer->failed || !text) return;
    const size_t len = strlen(text);
    if (writer->len + len + 1U > writer->cap) {
        writer->failed = 1;
        return;
    }
    memcpy(writer->buf + writer->len, text, len + 1U);
    writer->len += len;
}

void fvs_i_json_printf(fvs_i_json *writer, const char *fmt, ...) {
    if (!writer || writer->failed || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    const size_t room = writer->cap > writer->len ? writer->cap - writer->len : 0U;
    const int written = vsnprintf(writer->buf + writer->len, room, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= room) {
        writer->failed = 1;
        return;
    }
    writer->len += (size_t)written;
}

void fvs_i_json_string(fvs_i_json *writer, const char *text) {
    if (!writer || writer->failed) return;
    if (!text) {
        fvs_i_json_puts(writer, "null");
        return;
    }
    const size_t len = strlen(text);
    char *escaped = malloc(len * 6U + 1U);
    if (!escaped) {
        writer->failed = 1;
        return;
    }
    if (fvs_json_escape(text, escaped, len * 6U + 1U) != FVS_OK) {
        free(escaped);
        writer->failed = 1;
        return;
    }
    fvs_i_json_puts(writer, "\"");
    fvs_i_json_puts(writer, escaped);
    fvs_i_json_puts(writer, "\"");
    free(escaped);
}

int fvs_i_json_result(fvs_ctx *ctx, fvs_i_json *writer) {
    if (!writer || writer->failed) {
        fvs_i_seterr(ctx, "JSON output buffer too small");
        return FVS_ERR_BUFFER;
    }
    return FVS_OK;
}
