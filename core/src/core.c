#include "fvs.h"

#include <mysql.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fvs_ctx {
    MYSQL *db;
    char last_error[512];
};

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int failed;
} jsonw;

static void seterr(fvs_ctx *ctx, const char *fmt, ...) {
    if (!ctx) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(ctx->last_error, sizeof ctx->last_error, fmt, ap);
    va_end(ap);
}

static void set_mysql_err(fvs_ctx *ctx, const char *where) {
    seterr(ctx, "%s: (%u) %s", where, mysql_errno(ctx->db), mysql_error(ctx->db));
}

static int mysql_error_status(fvs_ctx *ctx, const char *where) {
    unsigned int err = mysql_errno(ctx->db);
    set_mysql_err(ctx, where);
    /* 1205 lock wait timeout and 1213 deadlock are explicitly retryable.
       Connection-loss errors remain FVS_ERR_DB because write outcome may be ambiguous. */
    if (err == 1205U || err == 1213U) return FVS_ERR_RETRY;
    return FVS_ERR_DB;
}

static unsigned int env_uint_clamped(const char *name, unsigned int def, unsigned int lo, unsigned int hi) {
    const char *raw = getenv(name);
    if (!raw || !*raw) return def;
    char *end = NULL;
    unsigned long v = strtoul(raw, &end, 10);
    if (!end || *end != '\0' || v < (unsigned long)lo || v > (unsigned long)hi) return def;
    return (unsigned int)v;
}

static int exec_sql(fvs_ctx *ctx, const char *sql) {
    if (mysql_query(ctx->db, sql) != 0) return mysql_error_status(ctx, "mysql_query");
    return FVS_OK;
}

static int tx_begin(fvs_ctx *ctx) { return exec_sql(ctx, "START TRANSACTION"); }
static int tx_commit(fvs_ctx *ctx) { return exec_sql(ctx, "COMMIT"); }
static void tx_rollback(fvs_ctx *ctx) { (void)mysql_query(ctx->db, "ROLLBACK"); }

static char *sql_escape(fvs_ctx *ctx, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    if (n > 1024U * 1024U) { seterr(ctx, "input too large"); return NULL; }
    char *out = malloc(n * 2U + 1U);
    if (!out) { seterr(ctx, "out of memory"); return NULL; }
    unsigned long written = mysql_real_escape_string(ctx->db, out, s, (unsigned long)n);
    out[written] = '\0';
    return out;
}

static int sqlf(fvs_ctx *ctx, char **out, const char *fmt, ...) {
    if (!out || !fmt) { seterr(ctx, "invalid SQL formatter arguments"); return FVS_ERR_INVALID; }
    *out = NULL;
    va_list ap;
    va_start(ap, fmt);
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) { va_end(ap); seterr(ctx, "format failure"); return FVS_ERR_INTERNAL; }
    char *buf = malloc((size_t)n + 1U);
    if (!buf) { va_end(ap); seterr(ctx, "out of memory"); return FVS_ERR_INTERNAL; }
    int n2 = vsnprintf(buf, (size_t)n + 1U, fmt, ap);
    va_end(ap);
    if (n2 != n) { free(buf); seterr(ctx, "format mismatch"); return FVS_ERR_INTERNAL; }
    *out = buf;
    return FVS_OK;
}


static int appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...) {
    if (!buf || !len || *len >= cap) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) return 0;
    *len += (size_t)n;
    return 1;
}

static void trim_ascii(char *s) {
    if (!s) return;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start != s) memmove(s, start, strlen(start) + 1U);
    size_t n = strlen(s);
    while (n > 0U && isspace((unsigned char)s[n - 1U])) s[--n] = '\0';
}

static int queryf(fvs_ctx *ctx, MYSQL_RES **res, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) { va_end(ap); return FVS_ERR_INTERNAL; }
    char *sql = malloc((size_t)n + 1U);
    if (!sql) { va_end(ap); return FVS_ERR_INTERNAL; }
    (void)vsnprintf(sql, (size_t)n + 1U, fmt, ap);
    va_end(ap);
    int rc = mysql_query(ctx->db, sql);
    free(sql);
    if (rc != 0) return mysql_error_status(ctx, "query");
    *res = mysql_store_result(ctx->db);
    if (!*res && mysql_field_count(ctx->db) != 0U) return mysql_error_status(ctx, "store_result");
    return FVS_OK;
}

static void jw_init(jsonw *w, char *buf, size_t cap) {
    w->buf = buf; w->cap = cap; w->len = 0; w->failed = 0;
    if (cap) buf[0] = '\0';
}

static void jw_puts(jsonw *w, const char *s) {
    if (w->failed) return;
    size_t n = strlen(s);
    if (w->len + n + 1U > w->cap) { w->failed = 1; return; }
    memcpy(w->buf + w->len, s, n + 1U); w->len += n;
}

static void jw_printf(jsonw *w, const char *fmt, ...) {
    if (w->failed) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap > w->len ? w->cap - w->len : 0U, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= (w->cap > w->len ? w->cap - w->len : 0U)) { w->failed = 1; return; }
    w->len += (size_t)n;
}

static void jw_string(jsonw *w, const char *s) {
    if (!s) { jw_puts(w, "null"); return; }
    size_t n = strlen(s);
    size_t tmpcap = n * 6U + 1U;
    char *tmp = malloc(tmpcap);
    if (!tmp) { w->failed = 1; return; }
    if (fvs_json_escape(s, tmp, tmpcap) != FVS_OK) { free(tmp); w->failed = 1; return; }
    jw_puts(w, "\""); jw_puts(w, tmp); jw_puts(w, "\"");
    free(tmp);
}

static int jw_result(fvs_ctx *ctx, jsonw *w) {
    if (w->failed) { seterr(ctx, "JSON output buffer too small"); return FVS_ERR_BUFFER; }
    return FVS_OK;
}

static int valid_id(const char *s) {
    if (!s || strlen(s) != 36U) return 0;
    for (size_t i = 0; i < 36U; ++i) {
        if (i == 8U || i == 13U || i == 18U || i == 23U) { if (s[i] != '-') return 0; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    }
    return 1;
}

static int valid_date(const char *s) {
    if (!s || strlen(s) != 10U || s[4] != '-' || s[7] != '-') return 0;
    const unsigned int pos[] = {0U,1U,2U,3U,5U,6U,8U,9U};
    for (size_t i = 0U; i < sizeof pos / sizeof pos[0]; ++i) {
        if (s[pos[i]] < '0' || s[pos[i]] > '9') return 0;
    }
    int year = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[5]-'0')*10 + (s[6]-'0');
    int day = (s[8]-'0')*10 + (s[9]-'0');
    if (year < 1000 || month < 1 || month > 12 || day < 1) return 0;
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int max_day = mdays[month-1];
    int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) max_day = 29;
    return day <= max_day;
}

static int nonempty_max(const char *s, size_t max_len) {
    return s && *s && strlen(s) <= max_len;
}

static int optional_max(const char *s, size_t max_len) {
    return !s || strlen(s) <= max_len;
}

static int auth_hash(const char *secret, char out[65]) {
    if (!secret || strlen(secret) < 32U || strlen(secret) > 256U) return FVS_ERR_AUTH;
    return fvs_sha256_hex(secret, out);
}

static int lock_cart(fvs_ctx *ctx, const char *cart_id, const char *secret_hash,
                     char status[24], unsigned long long *version) {
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res,
        "SELECT status,version FROM carts WHERE id='%s' AND secret_hash=UNHEX('%s') FOR UPDATE",
        cart_id, secret_hash);
    if (rc != FVS_OK) return rc;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); seterr(ctx, "cart not found or secret invalid"); return FVS_ERR_AUTH; }
    (void)snprintf(status, 24, "%s", row[0] ? row[0] : "");
    *version = row[1] ? strtoull(row[1], NULL, 10) : 0ULL;
    mysql_free_result(res);
    return FVS_OK;
}

static int currency_equal(const char *a, const char *b);

typedef struct {
    unsigned long item_count;
    int64_t subtotal_minor;
    int64_t addons_minor;
    int64_t discount_minor;
    int64_t total_minor;
    char currency[8];
    char promo_code[65];
    char referral_code[65];
    char source_channel[33];
    int code_valid;
} fvs_cart_price;

static int parse_money_i64(fvs_ctx *ctx, const char *s, int64_t *out) {
    if (!s || !out) return FVS_ERR_INVALID;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0' || v > (unsigned long long)LLONG_MAX) {
        seterr(ctx, "money overflow");
        return FVS_ERR_OVERFLOW;
    }
    *out = (int64_t)v;
    return FVS_OK;
}

static int commerce_code_capacity_available(fvs_ctx *ctx, const char *cart_id, const fvs_cart_price *p, int *out_available) {
    if (!ctx || !valid_id(cart_id) || !p || !out_available) return FVS_ERR_INVALID;
    *out_available = 1;
    const char *code = p->promo_code[0] ? p->promo_code : p->referral_code;
    if (!code || !*code) return FVS_OK;
    const char *table = p->promo_code[0] ? "commerce_promotions" : "commerce_referral_codes";
    const char *column = p->promo_code[0] ? "promo_code" : "referral_code";
    char *ec = sql_escape(ctx, code);
    if (!ec) return FVS_ERR_INTERNAL;
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res,
        "SELECT usage_limit,usage_count FROM %s WHERE code='%s' AND active=1 FOR UPDATE",
        table, ec);
    if (rc != FVS_OK) { free(ec); return rc; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); free(ec); *out_available = 0; return FVS_OK; }
    unsigned long long limit = row[0] ? strtoull(row[0], NULL, 10) : 0ULL;
    unsigned long long used = row[1] ? strtoull(row[1], NULL, 10) : 0ULL;
    mysql_free_result(res);
    if (limit == 0ULL) { free(ec); return FVS_OK; }
    rc = queryf(ctx, &res,
        "SELECT COUNT(*) FROM carts c WHERE c.id<>'%s' AND c.status='checkout' AND c.%s='%s' "
        "AND EXISTS(SELECT 1 FROM cart_items ci WHERE ci.cart_id=c.id AND ci.hold_expires_at>UTC_TIMESTAMP())",
        cart_id, column, ec);
    free(ec);
    if (rc != FVS_OK) return rc;
    row = mysql_fetch_row(res);
    unsigned long long reserved = row && row[0] ? strtoull(row[0], NULL, 10) : 0ULL;
    mysql_free_result(res);
    *out_available = used < limit && reserved < (limit - used);
    return FVS_OK;
}

static int cart_price_snapshot(fvs_ctx *ctx, const char *cart_id, fvs_cart_price *p) {
    if (!ctx || !valid_id(cart_id) || !p) return FVS_ERR_INVALID;
    memset(p, 0, sizeof *p);
    p->code_valid = 1;
    (void)snprintf(p->currency, sizeof p->currency, "%s", "MXN");

    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res,
        "SELECT COALESCE(promo_code,''),COALESCE(referral_code,''),COALESCE(source_channel,'') FROM carts WHERE id='%s'",
        cart_id);
    if (rc != FVS_OK) return rc;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return FVS_ERR_NOT_FOUND; }
    (void)snprintf(p->promo_code, sizeof p->promo_code, "%s", row[0] ? row[0] : "");
    (void)snprintf(p->referral_code, sizeof p->referral_code, "%s", row[1] ? row[1] : "");
    (void)snprintf(p->source_channel, sizeof p->source_channel, "%s", row[2] ? row[2] : "");
    mysql_free_result(res);

    rc = queryf(ctx, &res,
        "SELECT COUNT(*),COALESCE(SUM(price_minor),0),COUNT(DISTINCT currency),COALESCE(MAX(currency),'MXN') FROM cart_items WHERE cart_id='%s'",
        cart_id);
    if (rc != FVS_OK) return rc;
    row = mysql_fetch_row(res);
    p->item_count = row && row[0] ? strtoul(row[0], NULL, 10) : 0UL;
    unsigned long currency_count = row && row[2] ? strtoul(row[2], NULL, 10) : 0UL;
    if (!row || parse_money_i64(ctx, row[1] ? row[1] : "0", &p->subtotal_minor) != FVS_OK) {
        mysql_free_result(res);
        return FVS_ERR_OVERFLOW;
    }
    (void)snprintf(p->currency, sizeof p->currency, "%s", row[3] ? row[3] : "MXN");
    mysql_free_result(res);
    if (currency_count > 1UL) { seterr(ctx, "mixed-currency cart not allowed"); return FVS_ERR_CONFLICT; }

    rc = queryf(ctx, &res,
        "SELECT COALESCE(SUM(unit_price_minor*quantity),0),COUNT(DISTINCT currency),COALESCE(MAX(currency),'') FROM cart_addons WHERE cart_id='%s'",
        cart_id);
    if (rc != FVS_OK) return rc;
    row = mysql_fetch_row(res);
    unsigned long addon_currency_count = row && row[1] ? strtoul(row[1], NULL, 10) : 0UL;
    char addon_currency[8];
    (void)snprintf(addon_currency, sizeof addon_currency, "%s", row && row[2] ? row[2] : "");
    rc = parse_money_i64(ctx, row && row[0] ? row[0] : "0", &p->addons_minor);
    mysql_free_result(res);
    if (rc != FVS_OK) return rc;
    if (addon_currency_count > 1UL || (p->item_count > 0UL && addon_currency[0] && !currency_equal(addon_currency, p->currency))) {
        seterr(ctx, "mixed-currency add-ons not allowed");
        return FVS_ERR_CONFLICT;
    }
    if (p->item_count == 0UL && addon_currency[0]) (void)snprintf(p->currency, sizeof p->currency, "%s", addon_currency);

    if (p->promo_code[0]) {
        char *ec = sql_escape(ctx, p->promo_code);
        if (!ec) return FVS_ERR_INTERNAL;
        char *esc_source = sql_escape(ctx, p->source_channel);
        if (!esc_source) { free(ec); return FVS_ERR_INTERNAL; }
        rc = queryf(ctx, &res,
            "SELECT discount_type,discount_value,min_subtotal_minor,max_discount_minor,currency "
            "FROM commerce_promotions WHERE code='%s' AND active=1 "
            "AND (starts_at IS NULL OR starts_at<=UTC_TIMESTAMP()) AND (ends_at IS NULL OR ends_at>UTC_TIMESTAMP()) "
            "AND (usage_limit=0 OR usage_count<usage_limit) "
            "AND (channel_scope IS NULL OR JSON_LENGTH(channel_scope)=0 OR ('%s'<>'' AND JSON_CONTAINS(channel_scope,JSON_QUOTE('%s')))) LIMIT 1",
            ec, esc_source, esc_source);
        free(esc_source);
        free(ec);
        if (rc != FVS_OK) return rc;
        row = mysql_fetch_row(res);
        if (!row) {
            p->code_valid = 0;
        } else {
            int64_t value = 0, min_sub = 0, max_discount = 0;
            if (parse_money_i64(ctx, row[1] ? row[1] : "0", &value) != FVS_OK ||
                parse_money_i64(ctx, row[2] ? row[2] : "0", &min_sub) != FVS_OK ||
                parse_money_i64(ctx, row[3] ? row[3] : "0", &max_discount) != FVS_OK ||
                !currency_equal(row[4] ? row[4] : "", p->currency) || p->subtotal_minor < min_sub) {
                p->code_valid = 0;
            } else if (strcmp(row[0] ? row[0] : "", "percent_bps") == 0) {
                if (value < 0 || value > 10000) p->code_valid = 0;
                else {
                    int64_t q = p->subtotal_minor / 10000;
                    int64_t rem = p->subtotal_minor % 10000;
                    p->discount_minor = q * value + (rem * value) / 10000;
                }
            } else if (strcmp(row[0] ? row[0] : "", "fixed_minor") == 0) {
                p->discount_minor = value;
            } else p->code_valid = 0;
            if (p->discount_minor > p->subtotal_minor) p->discount_minor = p->subtotal_minor;
            if (max_discount > 0 && p->discount_minor > max_discount) p->discount_minor = max_discount;
        }
        mysql_free_result(res);
    } else if (p->referral_code[0]) {
        char *ec = sql_escape(ctx, p->referral_code);
        if (!ec) return FVS_ERR_INTERNAL;
        rc = queryf(ctx, &res,
            "SELECT invitee_discount_minor,currency FROM commerce_referral_codes WHERE code='%s' AND active=1 "
            "AND (starts_at IS NULL OR starts_at<=UTC_TIMESTAMP()) AND (ends_at IS NULL OR ends_at>UTC_TIMESTAMP()) "
            "AND (usage_limit=0 OR usage_count<usage_limit) LIMIT 1", ec);
        free(ec);
        if (rc != FVS_OK) return rc;
        row = mysql_fetch_row(res);
        if (!row) p->code_valid = 0;
        else {
            int64_t value = 0;
            if (parse_money_i64(ctx, row[0] ? row[0] : "0", &value) != FVS_OK || !currency_equal(row[1] ? row[1] : "", p->currency)) p->code_valid = 0;
            else p->discount_minor = value > p->subtotal_minor ? p->subtotal_minor : value;
        }
        mysql_free_result(res);
    }

    int64_t gross = 0;
    rc = fvs_add_i64_checked(p->subtotal_minor, p->addons_minor, &gross);
    if (rc != FVS_OK) { seterr(ctx, "cart total overflow"); return rc; }
    if (p->discount_minor < 0 || p->discount_minor > gross) { seterr(ctx, "invalid discount"); return FVS_ERR_STATE; }
    p->total_minor = gross - p->discount_minor;
    return FVS_OK;
}

static int render_cart(fvs_ctx *ctx, const char *cart_id, const char *secret_hash, char *out, size_t out_size) {
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res,
        "SELECT c.status,c.version,DATE_FORMAT(MIN(ci.hold_expires_at),'%%Y-%%m-%%dT%%H:%%i:%%sZ'),"
        "COALESCE(c.promo_code,''),COALESCE(c.referral_code,''),COALESCE(c.source_channel,''),COALESCE(c.social_link_token,'') "
        "FROM carts c LEFT JOIN cart_items ci ON ci.cart_id=c.id WHERE c.id='%s' AND c.secret_hash=UNHEX('%s') "
        "GROUP BY c.id,c.status,c.version,c.promo_code,c.referral_code,c.source_channel,c.social_link_token",
        cart_id, secret_hash);
    if (rc != FVS_OK) { return rc; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); seterr(ctx, "cart not found"); return FVS_ERR_AUTH; }
    char status[24], minexp[32], promo[65], referral[65], source[33], social_token[33];
    (void)snprintf(status, sizeof status, "%s", row[0] ? row[0] : "");
    unsigned long long version = row[1] ? strtoull(row[1], NULL, 10) : 0ULL;
    (void)snprintf(minexp, sizeof minexp, "%s", row[2] ? row[2] : "");
    (void)snprintf(promo, sizeof promo, "%s", row[3] ? row[3] : "");
    (void)snprintf(referral, sizeof referral, "%s", row[4] ? row[4] : "");
    (void)snprintf(source, sizeof source, "%s", row[5] ? row[5] : "");
    (void)snprintf(social_token, sizeof social_token, "%s", row[6] ? row[6] : "");
    mysql_free_result(res);

    fvs_cart_price price;
    rc = cart_price_snapshot(ctx, cart_id, &price);
    if (rc != FVS_OK) { return rc; }

    rc = queryf(ctx, &res,
        "SELECT ci.slot_id,ci.guests,ci.price_minor,ci.currency,DATE_FORMAT(ci.hold_expires_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),"
        "ci.resort_snapshot,ci.unit_code_snapshot,ci.unit_name_snapshot,DATE_FORMAT(ci.check_in_snapshot,'%%Y-%%m-%%d'),DATE_FORMAT(ci.check_out_snapshot,'%%Y-%%m-%%d'),"
        "ci.city_snapshot,ci.country_snapshot,ci.image_url_snapshot,ci.short_description_snapshot,ci.max_guests_snapshot,ci.week_number_snapshot,ci.booking_mode_snapshot,ci.float_group_snapshot "
        "FROM cart_items ci WHERE ci.cart_id='%s' ORDER BY ci.check_in_snapshot,ci.resort_snapshot,ci.unit_code_snapshot", cart_id);
    if (rc != FVS_OK) { return rc; }
    jsonw w; jw_init(&w, out, out_size);
    jw_puts(&w, "{\"cart_id\":"); jw_string(&w, cart_id);
    jw_puts(&w, ",\"status\":"); jw_string(&w, status);
    jw_printf(&w, ",\"version\":%llu,\"subtotal_minor\":%lld,\"addons_minor\":%lld,\"discount_minor\":%lld,\"total_minor\":%lld,\"currency\":",
              version, (long long)price.subtotal_minor, (long long)price.addons_minor, (long long)price.discount_minor, (long long)price.total_minor);
    jw_string(&w, price.currency);
    jw_puts(&w, ",\"promo_code\":"); promo[0] ? jw_string(&w,promo) : jw_puts(&w,"null");
    jw_puts(&w, ",\"referral_code\":"); referral[0] ? jw_string(&w,referral) : jw_puts(&w,"null");
    jw_printf(&w, ",\"code_valid\":%s", price.code_valid ? "true" : "false");
    jw_puts(&w, ",\"source_channel\":"); source[0] ? jw_string(&w,source) : jw_puts(&w,"null");
    jw_puts(&w, ",\"social_link_token\":"); social_token[0] ? jw_string(&w,social_token) : jw_puts(&w,"null");
    jw_puts(&w, ",\"hold_expires_at\":"); minexp[0] ? jw_string(&w,minexp) : jw_puts(&w,"null");
    jw_puts(&w, ",\"items\":[");
    int first = 1; MYSQL_ROW it;
    while ((it = mysql_fetch_row(res)) != NULL) {
        if (!first) { jw_puts(&w, ","); }
        first = 0;
        jw_puts(&w, "{\"slot_id\":"); jw_string(&w, it[0]);
        jw_printf(&w, ",\"guests\":%u,\"price_minor\":%lld,\"currency\":", it[1] ? (unsigned int)strtoul(it[1],NULL,10) : 0U, it[2] ? strtoll(it[2],NULL,10) : 0LL); jw_string(&w, it[3]);
        jw_puts(&w, ",\"hold_expires_at\":"); jw_string(&w, it[4]);
        jw_puts(&w, ",\"resort\":"); jw_string(&w, it[5]);
        jw_puts(&w, ",\"unit_code\":"); jw_string(&w, it[6]);
        jw_puts(&w, ",\"unit_name\":"); jw_string(&w, it[7]);
        jw_puts(&w, ",\"check_in\":"); jw_string(&w, it[8]);
        jw_puts(&w, ",\"check_out\":"); jw_string(&w, it[9]);
        jw_puts(&w, ",\"city\":"); jw_string(&w, it[10]);
        jw_puts(&w, ",\"country\":"); jw_string(&w, it[11]);
        jw_puts(&w, ",\"image_url\":"); jw_string(&w, it[12]);
        jw_puts(&w, ",\"short_description\":"); jw_string(&w, it[13]);
        jw_printf(&w, ",\"max_guests\":%u,\"week_number\":%u,\"booking_mode\":", it[14] ? (unsigned int)strtoul(it[14],NULL,10) : 0U, it[15] ? (unsigned int)strtoul(it[15],NULL,10) : 0U); jw_string(&w,it[16]);
        jw_puts(&w,",\"float_group\":"); jw_string(&w,it[17]); jw_puts(&w,"}");
    }
    mysql_free_result(res);
    jw_puts(&w, "],\"addons\":[");
    rc = queryf(ctx,&res,
        "SELECT ca.addon_id,ca.code_snapshot,ca.name_snapshot,a.description,a.category,ca.pricing_model_snapshot,ca.quantity,ca.unit_price_minor,ca.currency,a.icon "
        "FROM cart_addons ca JOIN commerce_addons a ON a.id=ca.addon_id WHERE ca.cart_id='%s' ORDER BY a.sort_order,a.name",cart_id);
    if (rc != FVS_OK) { return rc; }
    first = 1;
    while ((it=mysql_fetch_row(res)) != NULL) {
        if (!first) { jw_puts(&w,","); } first=0;
        jw_puts(&w,"{\"addon_id\":");jw_string(&w,it[0]);jw_puts(&w,",\"code\":");jw_string(&w,it[1]);jw_puts(&w,",\"name\":");jw_string(&w,it[2]);
        jw_puts(&w,",\"description\":");jw_string(&w,it[3]);jw_puts(&w,",\"category\":");jw_string(&w,it[4]);jw_puts(&w,",\"pricing_model\":");jw_string(&w,it[5]);
        jw_printf(&w,",\"quantity\":%u,\"unit_price_minor\":%lld,\"total_minor\":%lld,\"currency\":",it[6]?(unsigned int)strtoul(it[6],NULL,10):0U,it[7]?strtoll(it[7],NULL,10):0LL,(it[6]&&it[7])?strtoll(it[7],NULL,10)*(long long)strtoul(it[6],NULL,10):0LL);jw_string(&w,it[8]);
        jw_puts(&w,",\"icon\":");jw_string(&w,it[9]);jw_puts(&w,"}");
    }
    mysql_free_result(res);
    jw_puts(&w, "]}");
    return jw_result(ctx, &w);
}


static int currency_equal(const char *a, const char *b) {
    if (!a || !b || strlen(a) != 3U || strlen(b) != 3U) return 0;
    return toupper((unsigned char)a[0]) == toupper((unsigned char)b[0]) &&
           toupper((unsigned char)a[1]) == toupper((unsigned char)b[1]) &&
           toupper((unsigned char)a[2]) == toupper((unsigned char)b[2]);
}

const char *fvs_version(void) { return "6.0.0-c-core"; }
const char *fvs_last_error(fvs_ctx *ctx) { return ctx ? ctx->last_error : "no context"; }
const char *fvs_status_name(int status) {
    switch (status) {
        case FVS_OK: return "ok"; case FVS_ERR_INVALID: return "invalid"; case FVS_ERR_DB: return "db";
        case FVS_ERR_NOT_FOUND: return "not_found"; case FVS_ERR_CONFLICT: return "conflict"; case FVS_ERR_AUTH: return "auth";
        case FVS_ERR_EXPIRED: return "expired"; case FVS_ERR_STATE: return "state"; case FVS_ERR_OVERFLOW: return "overflow";
        case FVS_ERR_BUFFER: return "buffer"; case FVS_ERR_RETRY: return "retry"; default: return "internal";
    }
}

fvs_ctx *fvs_ctx_open(const char *host, unsigned int port, const char *user, const char *password, const char *database, unsigned int timeout) {
    if (!host || !user || !database) return NULL;
    fvs_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->db = mysql_init(NULL);
    if (!ctx->db) { free(ctx); return NULL; }
    const char *env_name = getenv("FVS_ENV");
    const char *allow_insecure = getenv("FVS_ALLOW_INSECURE_DB");
    const char *configured_ca = getenv("FVS_DB_SSL_CA");
    if (env_name && strcmp(env_name,"production") == 0 &&
        (!configured_ca || !*configured_ca) && (!allow_insecure || strcmp(allow_insecure,"1") != 0)) {
        mysql_close(ctx->db); free(ctx); return NULL;
    }
    (void)mysql_options(ctx->db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    unsigned int read_timeout = env_uint_clamped("FVS_DB_READ_TIMEOUT", 10U, 1U, 300U);
    unsigned int write_timeout = env_uint_clamped("FVS_DB_WRITE_TIMEOUT", 10U, 1U, 300U);
    (void)mysql_options(ctx->db, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
    (void)mysql_options(ctx->db, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
    const char *charset = "utf8mb4";
    (void)mysql_options(ctx->db, MYSQL_SET_CHARSET_NAME, charset);
    const char *ssl_ca = getenv("FVS_DB_SSL_CA");
    if (ssl_ca && *ssl_ca) {
        const char *ssl_cert = getenv("FVS_DB_SSL_CERT");
        const char *ssl_key = getenv("FVS_DB_SSL_KEY");
        if (mysql_ssl_set(ctx->db,
                          (ssl_key && *ssl_key) ? ssl_key : NULL,
                          (ssl_cert && *ssl_cert) ? ssl_cert : NULL,
                          ssl_ca, NULL, NULL) != 0) {
            mysql_close(ctx->db); free(ctx); return NULL;
        }
        my_bool yes = 1;
        if (mysql_options(ctx->db, MYSQL_OPT_SSL_ENFORCE, &yes) != 0 ||
            mysql_options(ctx->db, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &yes) != 0) {
            mysql_close(ctx->db); free(ctx); return NULL;
        }
    }
    if (!mysql_real_connect(ctx->db, host, user, password ? password : "", database, port, NULL, CLIENT_FOUND_ROWS)) {
        set_mysql_err(ctx, "connect"); mysql_close(ctx->db); free(ctx); return NULL;
    }
    unsigned int lock_wait = env_uint_clamped("FVS_DB_LOCK_WAIT_TIMEOUT", 5U, 1U, 120U);
    char lock_sql[96];
    int lock_n = snprintf(lock_sql, sizeof lock_sql, "SET SESSION innodb_lock_wait_timeout = %u", lock_wait);
    if (lock_n < 0 || (size_t)lock_n >= sizeof lock_sql ||
        exec_sql(ctx, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED") != FVS_OK ||
        exec_sql(ctx, "SET SESSION time_zone = '+00:00'") != FVS_OK ||
        exec_sql(ctx, "SET SESSION sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'") != FVS_OK ||
        exec_sql(ctx, lock_sql) != FVS_OK) {
        mysql_close(ctx->db); free(ctx); return NULL;
    }
    return ctx;
}

void fvs_ctx_close(fvs_ctx *ctx) { if (ctx) { if (ctx->db) mysql_close(ctx->db); free(ctx); } }
int fvs_ping(fvs_ctx *ctx) { if (!ctx || !ctx->db) return FVS_ERR_INVALID; if (mysql_ping(ctx->db) != 0) return mysql_error_status(ctx, "ping"); return FVS_OK; }

int fvs_inventory_search(fvs_ctx *ctx, const char *check_in, const char *check_out, unsigned int guests, char *out, size_t out_size) {
    if (!ctx || !valid_date(check_in) || !valid_date(check_out) || strcmp(check_in,check_out) >= 0 || guests == 0U || guests > 64U || !out) return FVS_ERR_INVALID;
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res,
        "SELECT id,resort,unit_code,unit_name,DATE_FORMAT(check_in,'%%Y-%%m-%%d'),DATE_FORMAT(check_out,'%%Y-%%m-%%d'),"
        "price_minor,currency,max_guests,city,country,image_url,short_description,season,week_number,booking_mode,float_group "
        "FROM inventory_slots WHERE active=1 AND is_booked=0 AND max_guests>=%u AND check_in>='%s' AND check_out<='%s' "
        "AND (held_by_cart_id IS NULL OR hold_expires_at<=UTC_TIMESTAMP()) ORDER BY check_in,price_minor LIMIT 100",
        guests, check_in, check_out);
    if (rc != FVS_OK) return rc;
    jsonw w; jw_init(&w, out, out_size); jw_puts(&w, "{\"items\":[");
    int first = 1; MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (!first) { jw_puts(&w, ","); }
        first = 0;
        jw_puts(&w, "{\"id\":"); jw_string(&w,row[0]); jw_puts(&w,",\"resort\":"); jw_string(&w,row[1]);
        jw_puts(&w,",\"unit_code\":"); jw_string(&w,row[2]); jw_puts(&w,",\"unit_name\":"); jw_string(&w,row[3]);
        jw_puts(&w,",\"check_in\":"); jw_string(&w,row[4]); jw_puts(&w,",\"check_out\":"); jw_string(&w,row[5]);
        jw_printf(&w,",\"price_minor\":%lld,\"currency\":",row[6]?strtoll(row[6],NULL,10):0LL); jw_string(&w,row[7]);
        jw_printf(&w,",\"max_guests\":%u,\"city\":",row[8]?(unsigned int)strtoul(row[8],NULL,10):0U); jw_string(&w,row[9]);
        jw_puts(&w,",\"country\":"); jw_string(&w,row[10]); jw_puts(&w,",\"image_url\":"); jw_string(&w,row[11]);
        jw_puts(&w,",\"short_description\":"); jw_string(&w,row[12]); jw_puts(&w,",\"season\":"); jw_string(&w,row[13]);
        jw_printf(&w,",\"week_number\":%u,\"booking_mode\":",row[14]?(unsigned int)strtoul(row[14],NULL,10):0U); jw_string(&w,row[15]);
        jw_puts(&w,",\"float_group\":"); jw_string(&w,row[16]); jw_puts(&w,"}");
    }
    mysql_free_result(res); jw_puts(&w, "]}"); return jw_result(ctx,&w);
}


static int discovery_write_facet(fvs_ctx *ctx, jsonw *w, const char *where_sql,
                                 const char *column, const char *json_key,
                                 unsigned int limit) {
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res,
        "SELECT %s,COUNT(*) FROM inventory_slots WHERE %s AND %s IS NOT NULL AND %s<>'' "
        "GROUP BY %s ORDER BY COUNT(*) DESC,%s LIMIT %u",
        column, where_sql, column, column, column, column, limit);
    if (rc != FVS_OK) return rc;
    jw_string(w, json_key); jw_puts(w, ":[");
    int first = 1; MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (!first) jw_puts(w, ",");
        first = 0;
        jw_puts(w, "{\"value\":"); jw_string(w, row[0]);
        jw_printf(w, ",\"count\":%llu}", row[1] ? strtoull(row[1], NULL, 10) : 0ULL);
    }
    mysql_free_result(res);
    jw_puts(w, "]");
    return jw_result(ctx, w);
}

static int discovery_where(fvs_ctx *ctx,
                           const char *query, const char *check_in, const char *check_out,
                           unsigned int guests, const char *country, const char *city,
                           const char *resort, const char *property_type, const char *season,
                           const char *booking_mode, const char *amenities_csv,
                           unsigned int min_bedrooms, double min_bathrooms, unsigned int min_rating_x100,
                           int64_t min_price_minor, int64_t max_price_minor,
                           double min_lat, double min_lng, double max_lat, double max_lng,
                           int use_bbox, char *where, size_t where_cap,
                           char **escaped_query_out) {
    if (!ctx || !where || where_cap < 256U || !escaped_query_out) return FVS_ERR_INVALID;
    *escaped_query_out = NULL;
    if ((check_in && *check_in && !valid_date(check_in)) ||
        (check_out && *check_out && !valid_date(check_out)) ||
        (check_in && *check_in && check_out && *check_out && strcmp(check_in, check_out) >= 0) ||
        guests == 0U || guests > 64U || min_bedrooms > 40U || min_bathrooms < 0.0 || min_bathrooms > 40.0 || min_rating_x100 > 500U ||
        min_price_minor < 0 || max_price_minor < 0 || (max_price_minor > 0 && min_price_minor > max_price_minor)) return FVS_ERR_INVALID;
    if (use_bbox && (min_lat < -90.0 || max_lat > 90.0 || min_lng < -180.0 || max_lng > 180.0 || min_lat > max_lat || min_lng > max_lng)) return FVS_ERR_INVALID;
    const char *strings[] = {query,country,city,resort,property_type,season,booking_mode};
    size_t maxes[] = {240U,120U,120U,160U,60U,40U,24U};
    for (size_t i=0;i<sizeof strings/sizeof strings[0];++i) if (strings[i] && strlen(strings[i]) > maxes[i]) return FVS_ERR_INVALID;
    if (amenities_csv && strlen(amenities_csv) > 1024U) return FVS_ERR_INVALID;

    char *eq = sql_escape(ctx, query ? query : "");
    char *eco = sql_escape(ctx, country ? country : "");
    char *eci = sql_escape(ctx, city ? city : "");
    char *er = sql_escape(ctx, resort ? resort : "");
    char *ept = sql_escape(ctx, property_type ? property_type : "");
    char *ese = sql_escape(ctx, season ? season : "");
    char *ebm = sql_escape(ctx, booking_mode ? booking_mode : "");
    if (!eq || !eco || !eci || !er || !ept || !ese || !ebm) { free(eq);free(eco);free(eci);free(er);free(ept);free(ese);free(ebm); return FVS_ERR_INTERNAL; }

    size_t len=0U;
    int ok=appendf(where,where_cap,&len,"active=1 AND is_booked=0 AND max_guests>=%u AND (held_by_cart_id IS NULL OR hold_expires_at<=UTC_TIMESTAMP())",guests);
    if (ok && check_in && *check_in) ok=appendf(where,where_cap,&len," AND check_in>='%s'",check_in);
    if (ok && check_out && *check_out) ok=appendf(where,where_cap,&len," AND check_out<='%s'",check_out);
    if (ok && *eco) ok=appendf(where,where_cap,&len," AND country='%s'",eco);
    if (ok && *eci) ok=appendf(where,where_cap,&len," AND city='%s'",eci);
    if (ok && *er) ok=appendf(where,where_cap,&len," AND resort='%s'",er);
    if (ok && *ept) ok=appendf(where,where_cap,&len," AND property_type='%s'",ept);
    if (ok && *ese) ok=appendf(where,where_cap,&len," AND season='%s'",ese);
    if (ok && *ebm) ok=appendf(where,where_cap,&len," AND booking_mode='%s'",ebm);
    if (ok && min_bedrooms > 0U) ok=appendf(where,where_cap,&len," AND bedrooms>=%u",min_bedrooms);
    if (ok && min_bathrooms > 0.0) ok=appendf(where,where_cap,&len," AND bathrooms>=%.1f",min_bathrooms);
    if (ok && min_rating_x100 > 0U) ok=appendf(where,where_cap,&len," AND rating_x100>=%u",min_rating_x100);
    if (ok && min_price_minor > 0) ok=appendf(where,where_cap,&len," AND price_minor>=%lld",(long long)min_price_minor);
    if (ok && max_price_minor > 0) ok=appendf(where,where_cap,&len," AND price_minor<=%lld",(long long)max_price_minor);
    if (ok && use_bbox) ok=appendf(where,where_cap,&len," AND latitude BETWEEN %.7f AND %.7f AND longitude BETWEEN %.7f AND %.7f",min_lat,max_lat,min_lng,max_lng);
    if (ok && *eq) ok=appendf(where,where_cap,&len," AND (MATCH(search_text) AGAINST ('%s' IN NATURAL LANGUAGE MODE) OR search_text LIKE '%%%s%%')",eq,eq);

    if (ok && amenities_csv && *amenities_csv) {
        char *copy = malloc(strlen(amenities_csv)+1U);
        if (!copy) ok=0;
        else {
            strcpy(copy,amenities_csv);
            unsigned int count=0U;
            for (char *tok=strtok(copy,","); tok && ok; tok=strtok(NULL,",")) {
                trim_ascii(tok); if (!*tok) continue; if (++count>12U || strlen(tok)>64U) { ok=0; break; }
                char *ea=sql_escape(ctx,tok); if(!ea){ok=0;break;}
                ok=appendf(where,where_cap,&len," AND JSON_CONTAINS(COALESCE(amenities_json,JSON_ARRAY()),JSON_QUOTE('%s'))",ea);
                free(ea);
            }
            free(copy);
        }
    }
    free(eco);free(eci);free(er);free(ept);free(ese);free(ebm);
    if (!ok) { free(eq); seterr(ctx,"search filter too large or invalid"); return FVS_ERR_INVALID; }
    *escaped_query_out=eq;
    return FVS_OK;
}

int fvs_inventory_search_v2(fvs_ctx *ctx,
                            const char *query, const char *check_in, const char *check_out,
                            unsigned int guests, const char *country, const char *city,
                            const char *resort, const char *property_type, const char *season,
                            const char *booking_mode, const char *amenities_csv,
                            unsigned int min_bedrooms, double min_bathrooms, unsigned int min_rating_x100,
                            int64_t min_price_minor, int64_t max_price_minor,
                            double min_lat, double min_lng, double max_lat, double max_lng,
                            int use_bbox, const char *sort, unsigned int limit, unsigned int offset,
                            char *out, size_t out_size) {
    if (!ctx || !out || !sort || strlen(sort)>24U) return FVS_ERR_INVALID;
    if (limit == 0U) limit = 60U;
    if (limit > 240U || offset > 10000U) return FVS_ERR_INVALID;
    char where[32768]; char *eq=NULL;
    int rc=discovery_where(ctx,query,check_in,check_out,guests,country,city,resort,property_type,season,booking_mode,amenities_csv,min_bedrooms,min_bathrooms,min_rating_x100,min_price_minor,max_price_minor,min_lat,min_lng,max_lat,max_lng,use_bbox,where,sizeof where,&eq);
    if(rc!=FVS_OK)return rc;
    const char *order="check_in ASC,price_minor ASC";
    if(strcmp(sort,"price_asc")==0)order="price_minor ASC,check_in ASC";
    else if(strcmp(sort,"price_desc")==0)order="price_minor DESC,check_in ASC";
    else if(strcmp(sort,"rating")==0)order="rating_x100 DESC,price_minor ASC";
    else if(strcmp(sort,"soonest")==0)order="check_in ASC,price_minor ASC";
    else if(strcmp(sort,"relevance")==0 && eq && *eq)order="relevance DESC,price_minor ASC";
    else if(strcmp(sort,"relevance")!=0 && strcmp(sort,"price_asc")!=0 && strcmp(sort,"price_desc")!=0 && strcmp(sort,"rating")!=0 && strcmp(sort,"soonest")!=0){free(eq);return FVS_ERR_INVALID;}

    MYSQL_RES *res=NULL;
    rc=queryf(ctx,&res,
        "SELECT id,resort,unit_code,unit_name,DATE_FORMAT(check_in,'%%Y-%%m-%%d'),DATE_FORMAT(check_out,'%%Y-%%m-%%d'),price_minor,currency,max_guests,city,country,image_url,short_description,season,week_number,booking_mode,float_group,latitude,longitude,property_type,bedrooms,bathrooms,rating_x100,COALESCE(amenities_json,JSON_ARRAY()),"
        "CASE WHEN '%s'='' THEN 0 ELSE (MATCH(search_text) AGAINST ('%s' IN NATURAL LANGUAGE MODE)*10 + IF(LOWER(resort)=LOWER('%s'),30,0)+IF(LOWER(city)=LOWER('%s'),20,0)+IF(resort LIKE '%s%%',8,0)+IF(city LIKE '%s%%',6,0)) END AS relevance "
        "FROM inventory_slots WHERE %s ORDER BY %s LIMIT %u OFFSET %u",eq?eq:"",eq?eq:"",eq?eq:"",eq?eq:"",eq?eq:"",eq?eq:"",where,order,limit,offset);
    if(rc!=FVS_OK){free(eq);return rc;}
    jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"items\":[");
    int first=1;MYSQL_ROW row;
    while((row=mysql_fetch_row(res))!=NULL){
        if (!first) jw_puts(&w, ",");
        first = 0;
        jw_puts(&w,"{\"id\":");jw_string(&w,row[0]);jw_puts(&w,",\"resort\":");jw_string(&w,row[1]);
        jw_puts(&w,",\"unit_code\":");jw_string(&w,row[2]);jw_puts(&w,",\"unit_name\":");jw_string(&w,row[3]);
        jw_puts(&w,",\"check_in\":");jw_string(&w,row[4]);jw_puts(&w,",\"check_out\":");jw_string(&w,row[5]);
        jw_printf(&w,",\"price_minor\":%lld,\"currency\":",row[6]?strtoll(row[6],NULL,10):0LL);jw_string(&w,row[7]);
        jw_printf(&w,",\"max_guests\":%u,\"city\":",row[8]?(unsigned int)strtoul(row[8],NULL,10):0U);jw_string(&w,row[9]);
        jw_puts(&w,",\"country\":");jw_string(&w,row[10]);jw_puts(&w,",\"image_url\":");jw_string(&w,row[11]);
        jw_puts(&w,",\"short_description\":");jw_string(&w,row[12]);jw_puts(&w,",\"season\":");jw_string(&w,row[13]);
        jw_printf(&w,",\"week_number\":%u,\"booking_mode\":",row[14]?(unsigned int)strtoul(row[14],NULL,10):0U);jw_string(&w,row[15]);jw_puts(&w,",\"float_group\":");jw_string(&w,row[16]);
        if(row[17]&&row[18])jw_printf(&w,",\"latitude\":%.6f,\"longitude\":%.6f",strtod(row[17],NULL),strtod(row[18],NULL));else jw_puts(&w,",\"latitude\":null,\"longitude\":null");
        jw_puts(&w,",\"property_type\":");jw_string(&w,row[19]);
        jw_printf(&w,",\"bedrooms\":%u,\"bathrooms\":%.1f,\"rating\":%.2f,\"amenities\":",row[20]?(unsigned int)strtoul(row[20],NULL,10):0U,row[21]?strtod(row[21],NULL):0.0,row[22]?strtod(row[22],NULL)/100.0:0.0);
        jw_puts(&w,row[23]?row[23]:"[]");jw_printf(&w,",\"relevance\":%.4f}",row[24]?strtod(row[24],NULL):0.0);
    }
    mysql_free_result(res);
    jw_puts(&w,"],\"total\":");
    rc=queryf(ctx,&res,"SELECT COUNT(*) FROM inventory_slots WHERE %s",where);if(rc!=FVS_OK){free(eq);return rc;}row=mysql_fetch_row(res);jw_printf(&w,"%llu",row&&row[0]?strtoull(row[0],NULL,10):0ULL);mysql_free_result(res);
    jw_puts(&w,",\"facets\":{");
    rc=discovery_write_facet(ctx,&w,where,"country","countries",24U);if(rc!=FVS_OK){free(eq);return rc;}jw_puts(&w,",");
    rc=discovery_write_facet(ctx,&w,where,"city","cities",36U);if(rc!=FVS_OK){free(eq);return rc;}jw_puts(&w,",");
    rc=discovery_write_facet(ctx,&w,where,"property_type","property_types",20U);if(rc!=FVS_OK){free(eq);return rc;}jw_puts(&w,",");
    rc=discovery_write_facet(ctx,&w,where,"season","seasons",12U);if(rc!=FVS_OK){free(eq);return rc;}jw_puts(&w,",");
    rc=discovery_write_facet(ctx,&w,where,"booking_mode","booking_modes",4U);if(rc!=FVS_OK){free(eq);return rc;}
    jw_puts(&w,"},\"limit\":");jw_printf(&w,"%u,\"offset\":%u}",limit,offset);
    free(eq);return jw_result(ctx,&w);
}

int fvs_inventory_suggest(fvs_ctx *ctx,const char *query,unsigned int limit,char *out,size_t out_size){
    if (!ctx || !query || !out || strlen(query) > 160U) return FVS_ERR_INVALID;
    if (limit == 0U) limit = 10U;
    if (limit > 30U) return FVS_ERR_INVALID;
    char *eq=sql_escape(ctx,query);if(!eq)return FVS_ERR_INTERNAL;
    MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,
        "SELECT kind,label,subtitle,weight FROM ("
        "SELECT 'country' kind,country label,'' subtitle,COUNT(*) weight FROM inventory_slots WHERE active=1 AND country<>'' AND country LIKE '%%%s%%' GROUP BY country "
        "UNION ALL SELECT 'city',city,country,COUNT(*) FROM inventory_slots WHERE active=1 AND city<>'' AND city LIKE '%%%s%%' GROUP BY city,country "
        "UNION ALL SELECT 'resort',resort,CONCAT_WS(', ',city,country),COUNT(*) FROM inventory_slots WHERE active=1 AND resort<>'' AND resort LIKE '%%%s%%' GROUP BY resort,city,country "
        "UNION ALL SELECT 'property',unit_name,resort,COUNT(*) FROM inventory_slots WHERE active=1 AND unit_name<>'' AND unit_name LIKE '%%%s%%' GROUP BY unit_name,resort"
        ") x ORDER BY (LOWER(label)=LOWER('%s')) DESC,(label LIKE '%s%%') DESC,weight DESC,label LIMIT %u",eq,eq,eq,eq,eq,eq,limit);
    free(eq);if(rc!=FVS_OK)return rc;
    jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"suggestions\":[");int first=1;MYSQL_ROW row;
    while((row=mysql_fetch_row(res))!=NULL){if(!first)jw_puts(&w,",");first=0;jw_puts(&w,"{\"type\":");jw_string(&w,row[0]);jw_puts(&w,",\"label\":");jw_string(&w,row[1]);jw_puts(&w,",\"subtitle\":");jw_string(&w,row[2]);jw_printf(&w,",\"weight\":%llu}",row[3]?strtoull(row[3],NULL,10):0ULL);}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);
}

int fvs_inventory_discovery_meta_upsert(fvs_ctx *ctx,const char *source_ref,int has_geo,double latitude,double longitude,const char *property_type,const char *amenities_json,unsigned int bedrooms,double bathrooms,unsigned int rating_x100){
    if(!ctx||!nonempty_max(source_ref,191U)||!optional_max(property_type,60U)||!optional_max(amenities_json,4096U)||bedrooms>40U||bathrooms<0.0||bathrooms>40.0||rating_x100>500U)return FVS_ERR_INVALID;
    if(has_geo&&(latitude < -90.0||latitude>90.0||longitude < -180.0||longitude>180.0))return FVS_ERR_INVALID;
    char *es=sql_escape(ctx,source_ref),*ept=sql_escape(ctx,property_type?property_type:""),*eam=sql_escape(ctx,(amenities_json&&*amenities_json)?amenities_json:"[]");if(!es||!ept||!eam){free(es);free(ept);free(eam);return FVS_ERR_INTERNAL;}
    char geo[96];if(has_geo)(void)snprintf(geo,sizeof geo,"latitude=%.7f,longitude=%.7f,",latitude,longitude);else(void)snprintf(geo,sizeof geo,"latitude=NULL,longitude=NULL,");
    char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE inventory_slots SET %s property_type=%s%s%s,amenities_json=JSON_EXTRACT('%s','$'),bedrooms=%u,bathrooms=%.1f,rating_x100=%u,updated_at=UTC_TIMESTAMP() WHERE source_ref='%s'",geo,*ept?"'":"NULL",*ept?ept:"",*ept?"'":"",eam,bedrooms,bathrooms,rating_x100,es);
    if (rc == FVS_OK) rc = exec_sql(ctx, sql);
    free(sql); free(es); free(ept); free(eam);
    return rc;
}

int fvs_inventory_upsert_v2(fvs_ctx *ctx, const char *source_ref, const char *resort,
                            const char *unit_code, const char *unit_name, const char *city,
                            const char *country, const char *check_in, const char *check_out,
                            unsigned int week_number, const char *booking_mode, const char *float_group,
                            unsigned int max_guests, int64_t price_minor, const char *currency,
                            const char *image_url, const char *short_description, const char *season,
                            int active, int *out_created) {
    int mode_ok = booking_mode && (strcmp(booking_mode,"fixed")==0 || strcmp(booking_mode,"floating")==0);
    if (!ctx || !nonempty_max(source_ref,191U) || !nonempty_max(resort,160U) ||
        !nonempty_max(unit_code,80U) || !nonempty_max(unit_name,160U) ||
        !optional_max(city,120U) || !optional_max(country,120U) ||
        !valid_date(check_in) || !valid_date(check_out) || strcmp(check_in,check_out) >= 0 ||
        week_number > 53U || !mode_ok || !optional_max(float_group,100U) ||
        (strcmp(booking_mode,"floating")==0 && (!float_group || !*float_group)) ||
        !optional_max(image_url,1024U) || !optional_max(short_description,500U) || !optional_max(season,40U) ||
        max_guests == 0U || max_guests > 64U || price_minor <= 0 || !fvs_currency_valid(currency) || !out_created) {
        return FVS_ERR_INVALID;
    }
    char *es = sql_escape(ctx, source_ref), *er = sql_escape(ctx, resort), *eu = sql_escape(ctx, unit_code);
    char *en = sql_escape(ctx, unit_name), *eci = sql_escape(ctx, city ? city : ""), *eco = sql_escape(ctx, country ? country : "");
    char *ebm = sql_escape(ctx, booking_mode), *efg = sql_escape(ctx, float_group ? float_group : "");
    char *ecur = sql_escape(ctx, currency), *eimg = sql_escape(ctx, image_url ? image_url : "");
    char *edesc = sql_escape(ctx, short_description ? short_description : ""), *eseason = sql_escape(ctx, season ? season : "");
    if (!es || !er || !eu || !en || !eci || !eco || !ebm || !efg || !ecur || !eimg || !edesc || !eseason) {
        free(es); free(er); free(eu); free(en); free(eci); free(eco); free(ebm); free(efg); free(ecur); free(eimg); free(edesc); free(eseason);
        return FVS_ERR_INTERNAL;
    }
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res, "SELECT id FROM inventory_slots WHERE source_ref='%s' LIMIT 1", es);
    if (rc != FVS_OK) goto done;
    *out_created = mysql_fetch_row(res) ? 0 : 1;
    mysql_free_result(res);
    char id[37]; rc = fvs_uuid_v4(id); if (rc != FVS_OK) goto done;
    char week_expr[16];
    if (week_number == 0U) (void)snprintf(week_expr,sizeof week_expr,"NULL");
    else (void)snprintf(week_expr,sizeof week_expr,"%u",week_number);
    char *sql = NULL;
    rc = sqlf(ctx, &sql,
        "INSERT INTO inventory_slots(id,resort,unit_code,unit_name,city,country,check_in,check_out,week_number,booking_mode,float_group,max_guests,price_minor,currency,image_url,short_description,season,source_ref,active,is_booked,created_at,updated_at) "
        "VALUES('%s','%s','%s','%s','%s','%s','%s','%s',%s,'%s',%s%s%s,%u,%lld,'%s','%s','%s','%s','%s',%d,0,UTC_TIMESTAMP(),UTC_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE resort=VALUES(resort),unit_code=VALUES(unit_code),unit_name=VALUES(unit_name),city=VALUES(city),country=VALUES(country),check_in=VALUES(check_in),check_out=VALUES(check_out),week_number=VALUES(week_number),booking_mode=VALUES(booking_mode),float_group=VALUES(float_group),max_guests=VALUES(max_guests),price_minor=VALUES(price_minor),currency=VALUES(currency),image_url=VALUES(image_url),short_description=VALUES(short_description),season=VALUES(season),active=VALUES(active),updated_at=UTC_TIMESTAMP()",
        id, er, eu, en, eci, eco, check_in, check_out, week_expr, ebm,
        efg[0]?"'":"",efg[0]?efg:"NULL",efg[0]?"'":"",
        max_guests, (long long)price_minor, ecur, eimg, edesc, eseason, es, active ? 1 : 0);
    if (rc == FVS_OK) rc = exec_sql(ctx, sql);
    free(sql);
done:
    free(es); free(er); free(eu); free(en); free(eci); free(eco); free(ebm); free(efg); free(ecur); free(eimg); free(edesc); free(eseason);
    return rc;
}

int fvs_inventory_upsert(fvs_ctx *ctx, const char *source_ref, const char *resort,
                         const char *unit_code, const char *unit_name, const char *city,
                         const char *country, const char *check_in, const char *check_out,
                         unsigned int max_guests, int64_t price_minor, const char *currency,
                         const char *image_url, const char *short_description, const char *season,
                         int active, int *out_created) {
    return fvs_inventory_upsert_v2(ctx,source_ref,resort,unit_code,unit_name,city,country,check_in,check_out,
                                   0U,"fixed",NULL,max_guests,price_minor,currency,image_url,short_description,season,active,out_created);
}

int fvs_cart_create(fvs_ctx *ctx, char *out_cart_id, size_t cart_id_size, char *out_secret, size_t secret_size) {
    if (!ctx || !out_cart_id || cart_id_size < 37U || !out_secret || secret_size < 65U) return FVS_ERR_INVALID;
    char id[37], secret[65], hash[65];
    int rc = fvs_uuid_v4(id); if (rc != FVS_OK) return rc;
    rc = fvs_random_hex(secret,sizeof secret,32U); if (rc != FVS_OK) return rc;
    rc = fvs_sha256_hex(secret,hash); if (rc != FVS_OK) return rc;
    char *sql = NULL; rc = sqlf(ctx,&sql,"INSERT INTO carts(id,secret_hash,status,version,created_at,updated_at) VALUES('%s',UNHEX('%s'),'open',1,UTC_TIMESTAMP(),UTC_TIMESTAMP())",id,hash);
    if (rc != FVS_OK) return rc;
    rc = exec_sql(ctx, sql);
    free(sql);
    if (rc != FVS_OK) return rc;
    memcpy(out_cart_id,id,37U); memcpy(out_secret,secret,65U); return FVS_OK;
}

int fvs_cart_get(fvs_ctx *ctx, const char *cart_id, const char *secret, char *out, size_t out_size) {
    if (!ctx || !valid_id(cart_id) || !secret || !out) return FVS_ERR_INVALID;
    char hash[65]; int rc = auth_hash(secret,hash); if (rc != FVS_OK) return rc;
    return render_cart(ctx,cart_id,hash,out,out_size);
}

int fvs_cart_add(fvs_ctx *ctx, const char *cart_id, const char *secret, const char *slot_id, unsigned int guests, unsigned int hold_seconds, char *out, size_t out_size) {
    if (!ctx || !valid_id(cart_id) || !valid_id(slot_id) || guests==0U || guests>64U || hold_seconds<60U || hold_seconds>3600U) return FVS_ERR_INVALID;
    char hash[65]; int rc=auth_hash(secret,hash); if(rc!=FVS_OK) return rc;
    rc=tx_begin(ctx); if(rc!=FVS_OK) return rc;
    char status[24]; unsigned long long version=0; rc=lock_cart(ctx,cart_id,hash,status,&version);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;} if(strcmp(status,"open")!=0){seterr(ctx,"cart not open");tx_rollback(ctx);return FVS_ERR_STATE;}
    MYSQL_RES *res=NULL; rc=queryf(ctx,&res,
        "SELECT is_booked,active,max_guests,held_by_cart_id,(hold_expires_at>UTC_TIMESTAMP()) "
        "FROM inventory_slots WHERE id='%s' FOR UPDATE",slot_id);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;} MYSQL_ROW row=mysql_fetch_row(res);
    if(!row){mysql_free_result(res);seterr(ctx,"inventory not found");tx_rollback(ctx);return FVS_ERR_NOT_FOUND;}
    int booked=row[0]?atoi(row[0]):0, active=row[1]?atoi(row[1]):0; unsigned int maxg=row[2]?(unsigned int)strtoul(row[2],NULL,10):0U;
    char owner[37];(void)snprintf(owner,sizeof owner,"%s",row[3]?row[3]:""); int hold_live=row[4]?atoi(row[4]):0; mysql_free_result(res);
    if(booked||!active||guests>maxg){seterr(ctx,"inventory unavailable");tx_rollback(ctx);return FVS_ERR_CONFLICT;}
    if(hold_live && owner[0] && strcmp(owner,cart_id)!=0){seterr(ctx,"inventory held by another cart");tx_rollback(ctx);return FVS_ERR_CONFLICT;}
    char *sql=NULL; rc=sqlf(ctx,&sql,"UPDATE inventory_slots SET held_by_cart_id='%s',hold_expires_at=TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id,hold_seconds,slot_id);
    if (rc == FVS_OK) { rc = exec_sql(ctx, sql); }
    free(sql);
    if (rc != FVS_OK) { tx_rollback(ctx); return rc; }
    rc=sqlf(ctx,&sql,
        "INSERT INTO cart_items(cart_id,slot_id,guests,price_minor,currency,hold_expires_at,"
        "resort_snapshot,unit_code_snapshot,unit_name_snapshot,check_in_snapshot,check_out_snapshot,city_snapshot,country_snapshot,"
        "image_url_snapshot,short_description_snapshot,max_guests_snapshot,week_number_snapshot,booking_mode_snapshot,float_group_snapshot,created_at,updated_at) "
        "SELECT '%s',s.id,%u,s.price_minor,s.currency,TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),"
        "s.resort,s.unit_code,s.unit_name,s.check_in,s.check_out,s.city,s.country,s.image_url,s.short_description,s.max_guests,s.week_number,s.booking_mode,s.float_group,UTC_TIMESTAMP(),UTC_TIMESTAMP() "
        "FROM inventory_slots s WHERE s.id='%s' "
        "ON DUPLICATE KEY UPDATE guests=VALUES(guests),price_minor=VALUES(price_minor),currency=VALUES(currency),"
        "hold_expires_at=VALUES(hold_expires_at),resort_snapshot=VALUES(resort_snapshot),unit_code_snapshot=VALUES(unit_code_snapshot),"
        "unit_name_snapshot=VALUES(unit_name_snapshot),check_in_snapshot=VALUES(check_in_snapshot),check_out_snapshot=VALUES(check_out_snapshot),"
        "city_snapshot=VALUES(city_snapshot),country_snapshot=VALUES(country_snapshot),image_url_snapshot=VALUES(image_url_snapshot),"
        "short_description_snapshot=VALUES(short_description_snapshot),max_guests_snapshot=VALUES(max_guests_snapshot),"
        "week_number_snapshot=VALUES(week_number_snapshot),booking_mode_snapshot=VALUES(booking_mode_snapshot),float_group_snapshot=VALUES(float_group_snapshot),updated_at=UTC_TIMESTAMP()",
        cart_id,guests,hold_seconds,slot_id);
    if (rc == FVS_OK) { rc = exec_sql(ctx, sql); }
    free(sql);
    if (rc != FVS_OK) { tx_rollback(ctx); return rc; }
    rc=queryf(ctx,&res,"SELECT COUNT(*),COALESCE(SUM(price_minor),0),COUNT(DISTINCT currency) FROM cart_items WHERE cart_id='%s'",cart_id);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;}
    row=mysql_fetch_row(res);
    unsigned long item_count=row&&row[0]?strtoul(row[0],NULL,10):0UL;
    char *sum_end=NULL;
    unsigned long long sum_u=row&&row[1]?strtoull(row[1],&sum_end,10):0ULL;
    unsigned long currencies=row&&row[2]?strtoul(row[2],NULL,10):0UL;
    int sum_bad=!row||!row[1]||!sum_end||*sum_end!='\0'||sum_u>(unsigned long long)LLONG_MAX;
    mysql_free_result(res);
    if(item_count>64UL){seterr(ctx,"cart item limit exceeded");tx_rollback(ctx);return FVS_ERR_CONFLICT;}
    if(sum_bad){seterr(ctx,"cart total overflow");tx_rollback(ctx);return FVS_ERR_OVERFLOW;}
    if(currencies>1UL){seterr(ctx,"mixed-currency cart not allowed");tx_rollback(ctx);return FVS_ERR_CONFLICT;}
    rc=sqlf(ctx,&sql,"UPDATE carts SET version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id); if(rc==FVS_OK) rc=exec_sql(ctx,sql); free(sql);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;} rc=tx_commit(ctx); if(rc!=FVS_OK)return rc;
    return render_cart(ctx,cart_id,hash,out,out_size);
}

int fvs_cart_remove(fvs_ctx *ctx,const char *cart_id,const char *secret,const char *slot_id,char *out,size_t out_size){
    if (!ctx || !valid_id(cart_id) || !valid_id(slot_id) || !secret) return FVS_ERR_INVALID;
    char hash[65];
    int rc = auth_hash(secret, hash);
    if (rc != FVS_OK) return rc;
    rc=tx_begin(ctx);if(rc!=FVS_OK)return rc;char status[24];unsigned long long version=0;rc=lock_cart(ctx,cart_id,hash,status,&version);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;}if(strcmp(status,"open")!=0){seterr(ctx,"cart not open");tx_rollback(ctx);return FVS_ERR_STATE;}
    MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT id FROM inventory_slots WHERE id='%s' FOR UPDATE",slot_id);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}mysql_free_result(res);
    char *sql=NULL;rc=sqlf(ctx,&sql,"DELETE FROM cart_items WHERE cart_id='%s' AND slot_id='%s'",cart_id,slot_id);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);
    if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE inventory_slots SET held_by_cart_id=NULL,hold_expires_at=NULL,updated_at=UTC_TIMESTAMP() WHERE id='%s' AND held_by_cart_id='%s'",slot_id,cart_id);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);}
    if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE carts SET version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);}
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;}rc=tx_commit(ctx);if(rc!=FVS_OK)return rc;return render_cart(ctx,cart_id,hash,out,out_size);
}

static int attempt_json(fvs_ctx *ctx,const char *attempt_id,char *out,size_t out_size){
    MYSQL_RES *res=NULL;
    int rc=queryf(ctx,&res,
        "SELECT p.id,p.cart_id,p.cart_version,p.provider,p.status,p.amount_minor,p.subtotal_minor,p.addons_minor,p.discount_minor,"
        "COALESCE(p.promo_code,''),COALESCE(p.source_channel,''),p.currency,p.email,p.provider_checkout_id,p.provider_payment_id,"
        "p.client_secret,p.redirect_url,p.idempotency_key,o.id "
        "FROM payment_attempts p LEFT JOIN orders o ON o.payment_attempt_id=p.id WHERE p.id='%s'",attempt_id);
    if(rc!=FVS_OK)return rc;
    MYSQL_ROW r=mysql_fetch_row(res);
    if(!r){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}
    jsonw w;jw_init(&w,out,out_size);
    jw_puts(&w,"{\"attempt_id\":");jw_string(&w,r[0]);
    jw_puts(&w,",\"cart_id\":");jw_string(&w,r[1]);
    jw_printf(&w,",\"cart_version\":%llu,\"provider\":",r[2]?strtoull(r[2],NULL,10):0ULL);jw_string(&w,r[3]);
    jw_puts(&w,",\"status\":");jw_string(&w,r[4]);
    jw_printf(&w,",\"amount_minor\":%lld,\"subtotal_minor\":%lld,\"addons_minor\":%lld,\"discount_minor\":%lld,\"promo_code\":",
        r[5]?strtoll(r[5],NULL,10):0LL,r[6]?strtoll(r[6],NULL,10):0LL,r[7]?strtoll(r[7],NULL,10):0LL,r[8]?strtoll(r[8],NULL,10):0LL);
    r[9]&&r[9][0]?jw_string(&w,r[9]):jw_puts(&w,"null");
    jw_puts(&w,",\"source_channel\":");r[10]&&r[10][0]?jw_string(&w,r[10]):jw_puts(&w,"null");
    jw_puts(&w,",\"currency\":");jw_string(&w,r[11]);
    jw_puts(&w,",\"email\":");jw_string(&w,r[12]);
    jw_puts(&w,",\"provider_checkout_id\":");jw_string(&w,r[13]);
    jw_puts(&w,",\"provider_payment_id\":");jw_string(&w,r[14]);
    jw_puts(&w,",\"client_secret\":");jw_string(&w,r[15]);
    jw_puts(&w,",\"redirect_url\":");jw_string(&w,r[16]);
    jw_puts(&w,",\"idempotency_key\":");jw_string(&w,r[17]);
    jw_puts(&w,",\"order_id\":");jw_string(&w,r[18]);jw_puts(&w,"}");
    mysql_free_result(res);
    return jw_result(ctx,&w);
}

int fvs_checkout_begin(fvs_ctx *ctx,const char *cart_id,const char *secret,const char *provider,const char *email,const char *terms_version,unsigned int hold_seconds,char *out,size_t out_size){
    if(!ctx||!valid_id(cart_id)||!secret||!fvs_provider_valid(provider)||!fvs_email_valid(email)||!terms_version||strlen(terms_version)>64U||hold_seconds<300U||hold_seconds>3600U)return FVS_ERR_INVALID;
    char hash[65];
    int rc=auth_hash(secret,hash);
    if(rc!=FVS_OK)return rc;
    char *ep=sql_escape(ctx,provider),*ee=sql_escape(ctx,email),*et=sql_escape(ctx,terms_version);
    if(!ep||!ee||!et){free(ep);free(ee);free(et);return FVS_ERR_INTERNAL;}

    rc=tx_begin(ctx);
    if(rc!=FVS_OK)goto cleanup;
    char status[24];
    unsigned long long version=0ULL;
    rc=lock_cart(ctx,cart_id,hash,status,&version);
    if(rc!=FVS_OK){tx_rollback(ctx);goto cleanup;}

    if(strcmp(status,"checkout")==0){
        MYSQL_RES *er=NULL;
        rc=queryf(ctx,&er,"SELECT id FROM payment_attempts WHERE cart_id='%s' AND cart_version=%llu AND provider='%s' ORDER BY created_at DESC LIMIT 1",cart_id,version,ep);
        if(rc==FVS_OK){
            MYSQL_ROW rr=mysql_fetch_row(er);
            if(rr){
                char aid[37];
                (void)snprintf(aid,sizeof aid,"%s",rr[0]);
                mysql_free_result(er);
                rc=tx_commit(ctx);
                if(rc==FVS_OK)rc=attempt_json(ctx,aid,out,out_size);
                goto cleanup;
            }
            mysql_free_result(er);
        }
        tx_rollback(ctx);
        if(rc==FVS_OK){seterr(ctx,"checkout state without attempt");rc=FVS_ERR_STATE;}
        goto cleanup;
    }
    if(strcmp(status,"open")!=0){seterr(ctx,"cart is not checkoutable");tx_rollback(ctx);rc=FVS_ERR_STATE;goto cleanup;}
    if(version==ULLONG_MAX){seterr(ctx,"cart version overflow");tx_rollback(ctx);rc=FVS_ERR_OVERFLOW;goto cleanup;}

    /* Lock the inventory contract before calculating the payable total. If a quote/hold
       expired, refresh price and property snapshots from the current inventory row. */
    MYSQL_RES *res=NULL;
    rc=queryf(ctx,&res,
        "SELECT s.id,s.is_booked,s.active,s.held_by_cart_id,(s.hold_expires_at>UTC_TIMESTAMP()),s.max_guests,ci.guests,(ci.hold_expires_at>UTC_TIMESTAMP()) "
        "FROM cart_items ci JOIN inventory_slots s ON s.id=ci.slot_id WHERE ci.cart_id='%s' ORDER BY s.id FOR UPDATE",cart_id);
    if(rc!=FVS_OK){tx_rollback(ctx);goto cleanup;}
    MYSQL_ROW ir;
    unsigned long item_count=0UL;
    int invalid=0;
    int needs_requote=0;
    while((ir=mysql_fetch_row(res))!=NULL){
        ++item_count;
        int booked=ir[1]?atoi(ir[1]):0;
        int active=ir[2]?atoi(ir[2]):0;
        int live=ir[4]?atoi(ir[4]):0;
        unsigned int mg=ir[5]?(unsigned int)strtoul(ir[5],NULL,10):0U;
        unsigned int g=ir[6]?(unsigned int)strtoul(ir[6],NULL,10):0U;
        int item_live=ir[7]?atoi(ir[7]):0;
        if(booked||!active||g==0U||g>mg||(live&&ir[3]&&strcmp(ir[3],cart_id)!=0)){invalid=1;break;}
        if(!live||!item_live||!ir[3]||strcmp(ir[3],cart_id)!=0)needs_requote=1;
    }
    mysql_free_result(res);
    if(item_count==0UL||item_count>64UL){seterr(ctx,"cart empty or too large");tx_rollback(ctx);rc=FVS_ERR_STATE;goto cleanup;}
    if(invalid){seterr(ctx,"one or more cart items are no longer available");tx_rollback(ctx);rc=FVS_ERR_CONFLICT;goto cleanup;}

    char *sql=NULL;
    rc=sqlf(ctx,&sql,
        "UPDATE cart_items ci JOIN inventory_slots s ON s.id=ci.slot_id SET "
        "ci.price_minor=s.price_minor,ci.currency=s.currency,ci.resort_snapshot=s.resort,ci.unit_code_snapshot=s.unit_code,"
        "ci.unit_name_snapshot=s.unit_name,ci.check_in_snapshot=s.check_in,ci.check_out_snapshot=s.check_out,"
        "ci.city_snapshot=s.city,ci.country_snapshot=s.country,ci.image_url_snapshot=s.image_url,"
        "ci.short_description_snapshot=s.short_description,ci.max_guests_snapshot=s.max_guests,ci.week_number_snapshot=s.week_number,ci.booking_mode_snapshot=s.booking_mode,ci.float_group_snapshot=s.float_group,ci.updated_at=UTC_TIMESTAMP() "
        "WHERE ci.cart_id='%s' AND (ci.hold_expires_at<=UTC_TIMESTAMP() OR s.hold_expires_at IS NULL OR s.hold_expires_at<=UTC_TIMESTAMP() OR s.held_by_cart_id IS NULL OR s.held_by_cart_id<>'%s')",
        cart_id,cart_id);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);
    if(rc!=FVS_OK){tx_rollback(ctx);goto cleanup;}

    rc=sqlf(ctx,&sql,
        "UPDATE inventory_slots s JOIN cart_items ci ON ci.slot_id=s.id SET "
        "s.held_by_cart_id='%s',s.hold_expires_at=TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),s.updated_at=UTC_TIMESTAMP(),"
        "ci.hold_expires_at=TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),ci.updated_at=UTC_TIMESTAMP() WHERE ci.cart_id='%s'",
        cart_id,hold_seconds,hold_seconds,cart_id);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);
    if(rc!=FVS_OK){tx_rollback(ctx);goto cleanup;}

    fvs_cart_price price;
    rc = cart_price_snapshot(ctx, cart_id, &price);
    if (rc != FVS_OK) { tx_rollback(ctx); goto cleanup; }
    if (price.item_count != item_count || price.item_count == 0UL || price.item_count > 64UL ||
        price.total_minor <= 0 || !fvs_currency_valid(price.currency)) {
        seterr(ctx, "cart total invalid or inconsistent item count");
        tx_rollback(ctx);
        rc = FVS_ERR_STATE;
        goto cleanup;
    }
    if (!price.code_valid && (price.promo_code[0] || price.referral_code[0])) {
        rc = sqlf(ctx, &sql,
            "UPDATE carts SET promo_code=NULL,referral_code=NULL,version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",
            cart_id);
        if (rc == FVS_OK) rc = exec_sql(ctx, sql);
        free(sql); sql = NULL;
        if (rc != FVS_OK) { tx_rollback(ctx); goto cleanup; }
        rc = cart_price_snapshot(ctx, cart_id, &price);
        if (rc != FVS_OK) { tx_rollback(ctx); goto cleanup; }
        needs_requote = 1;
    }
    if (price.code_valid && (price.promo_code[0] || price.referral_code[0])) {
        int code_capacity = 1;
        rc = commerce_code_capacity_available(ctx, cart_id, &price, &code_capacity);
        if (rc != FVS_OK) { tx_rollback(ctx); goto cleanup; }
        if (!code_capacity) {
            rc = sqlf(ctx, &sql,
                "UPDATE carts SET promo_code=NULL,referral_code=NULL,version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",
                cart_id);
            if (rc == FVS_OK) rc = exec_sql(ctx, sql);
            free(sql); sql = NULL;
            if (rc != FVS_OK) { tx_rollback(ctx); goto cleanup; }
            rc = cart_price_snapshot(ctx, cart_id, &price);
            if (rc != FVS_OK) { tx_rollback(ctx); goto cleanup; }
            needs_requote = 1;
        }
    }
    if(needs_requote){
        char *vsql=NULL;
        rc=sqlf(ctx,&vsql,"UPDATE carts SET version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);
        if(rc==FVS_OK)rc=exec_sql(ctx,vsql);
        free(vsql);
        if(rc!=FVS_OK){tx_rollback(ctx);goto cleanup;}
        rc=tx_commit(ctx);
        if(rc==FVS_OK){
            jsonw w;jw_init(&w,out,out_size);
            jw_puts(&w,"{\"status\":\"quote_refreshed\",\"requires_confirmation\":true");
            jw_printf(&w,",\"subtotal_minor\":%lld,\"addons_minor\":%lld,\"discount_minor\":%lld,\"amount_minor\":%lld,\"currency\":",
                      (long long)price.subtotal_minor,(long long)price.addons_minor,(long long)price.discount_minor,(long long)price.total_minor);
            jw_string(&w,price.currency);
            jw_puts(&w,",\"promo_code\":");
            price.promo_code[0] ? jw_string(&w,price.promo_code) : jw_puts(&w,"null");
            jw_puts(&w,"}");
            rc=jw_result(ctx,&w);
        }
        goto cleanup;
    }
    unsigned long long checkout_version=version+1ULL;

    char attempt[37],idem[128];
    rc=fvs_uuid_v4(attempt);
    if(rc!=FVS_OK){tx_rollback(ctx);goto cleanup;}
    int idem_n=snprintf(idem,sizeof idem,"fvs:%s:v%llu:%s",cart_id,checkout_version,provider);
    if(idem_n<0||(size_t)idem_n>=sizeof idem){seterr(ctx,"idempotency key overflow");tx_rollback(ctx);rc=FVS_ERR_INTERNAL;goto cleanup;}
    rc=sqlf(ctx,&sql,
        "INSERT INTO payment_attempts(id,cart_id,cart_version,provider,status,amount_minor,subtotal_minor,addons_minor,discount_minor,promo_code,source_channel,currency,email,terms_version,idempotency_key,created_at,updated_at) "
        "VALUES('%s','%s',%llu,'%s','creating',%lld,%lld,%lld,%lld,NULLIF('%s',''),NULLIF('%s',''),'%s','%s','%s','%s',UTC_TIMESTAMP(),UTC_TIMESTAMP())",
        attempt,cart_id,checkout_version,ep,(long long)price.total_minor,(long long)price.subtotal_minor,(long long)price.addons_minor,
        (long long)price.discount_minor,price.promo_code,price.source_channel,price.currency,ee,et,idem);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);
    if(rc==FVS_OK){
        rc=sqlf(ctx,&sql,"UPDATE carts SET status='checkout',version=version+1,customer_email='%s',terms_version='%s',terms_accepted_at=UTC_TIMESTAMP(),updated_at=UTC_TIMESTAMP() WHERE id='%s'",ee,et,cart_id);
        if(rc==FVS_OK)rc=exec_sql(ctx,sql);
        free(sql);
    }
    if(rc!=FVS_OK){tx_rollback(ctx);goto cleanup;}
    rc=tx_commit(ctx);
    if(rc==FVS_OK)rc=attempt_json(ctx,attempt,out,out_size);
cleanup:
    free(ep);free(ee);free(et);return rc;
}

int fvs_checkout_attach_provider(fvs_ctx *ctx,const char *attempt_id,const char *checkout_id,const char *payment_id,const char *client_secret,const char *redirect_url){
    if (!ctx || !valid_id(attempt_id)) return FVS_ERR_INVALID;
    if ((checkout_id && strlen(checkout_id) > 255U) ||
        (payment_id && strlen(payment_id) > 255U) ||
        (client_secret && strlen(client_secret) > 512U) ||
        (redirect_url && strlen(redirect_url) > 2048U)) return FVS_ERR_INVALID;

    char *ec = checkout_id ? sql_escape(ctx,checkout_id) : NULL;
    char *ep = payment_id ? sql_escape(ctx,payment_id) : NULL;
    char *ecs = client_secret ? sql_escape(ctx,client_secret) : NULL;
    char *er = redirect_url ? sql_escape(ctx,redirect_url) : NULL;
    if ((checkout_id && !ec) || (payment_id && !ep) || (client_secret && !ecs) || (redirect_url && !er)) {
        free(ec); free(ep); free(ecs); free(er); return FVS_ERR_INTERNAL;
    }

    int rc = tx_begin(ctx);
    if (rc != FVS_OK) goto done;
    MYSQL_RES *res = NULL;
    rc = queryf(ctx,&res,
        "SELECT status,provider_checkout_id,provider_payment_id,client_secret,redirect_url FROM payment_attempts WHERE id='%s' FOR UPDATE",
        attempt_id);
    if (rc != FVS_OK) { tx_rollback(ctx); goto done; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); tx_rollback(ctx); rc = FVS_ERR_NOT_FOUND; goto done; }
    const char *status = row[0] ? row[0] : "";
    int attachable = strcmp(status,"creating") == 0 || strcmp(status,"pending") == 0;
    int mismatch =
        (checkout_id && row[1] && strcmp(checkout_id,row[1]) != 0) ||
        (payment_id && row[2] && strcmp(payment_id,row[2]) != 0) ||
        (client_secret && row[3] && strcmp(client_secret,row[3]) != 0) ||
        (redirect_url && row[4] && strcmp(redirect_url,row[4]) != 0);
    mysql_free_result(res);
    if (mismatch) { seterr(ctx,"provider object mismatch on resume"); tx_rollback(ctx); rc = FVS_ERR_CONFLICT; goto done; }
    if (!attachable) {
        if (strcmp(status,"paid") == 0) { rc = tx_commit(ctx); goto done; }
        seterr(ctx,"attempt not attachable"); tx_rollback(ctx); rc = FVS_ERR_STATE; goto done;
    }

    char *sql = NULL;
    rc = sqlf(ctx,&sql,
        "UPDATE payment_attempts SET provider_checkout_id=COALESCE(provider_checkout_id,%s%s%s),"
        "provider_payment_id=COALESCE(provider_payment_id,%s%s%s),"
        "client_secret=COALESCE(client_secret,%s%s%s),redirect_url=COALESCE(redirect_url,%s%s%s),"
        "status='pending',updated_at=UTC_TIMESTAMP() WHERE id='%s'",
        ec?"'":"",ec?ec:"NULL",ec?"'":"",
        ep?"'":"",ep?ep:"NULL",ep?"'":"",
        ecs?"'":"",ecs?ecs:"NULL",ecs?"'":"",
        er?"'":"",er?er:"NULL",er?"'":"",attempt_id);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql);
    if (rc == FVS_OK) rc = tx_commit(ctx); else tx_rollback(ctx);

done:
    free(ec); free(ep); free(ecs); free(er);
    return rc;
}

int fvs_checkout_get(fvs_ctx *ctx,const char *cart_id,const char *secret,char *out,size_t out_size){
    if (!ctx || !valid_id(cart_id) || !secret) return FVS_ERR_INVALID;
    char hash[65];
    int rc = auth_hash(secret,hash);
    if (rc != FVS_OK) return rc;
    MYSQL_RES *res = NULL;
    rc = queryf(ctx,&res,"SELECT id FROM carts WHERE id='%s' AND secret_hash=UNHEX('%s')",cart_id,hash);
    if (rc != FVS_OK) return rc;
    MYSQL_ROW r = mysql_fetch_row(res);
    if (!r) { mysql_free_result(res); return FVS_ERR_AUTH; }
    mysql_free_result(res);
    rc = queryf(ctx,&res,"SELECT id FROM payment_attempts WHERE cart_id='%s' ORDER BY created_at DESC LIMIT 1",cart_id);
    if (rc != FVS_OK) return rc;
    r = mysql_fetch_row(res);
    if (!r) { mysql_free_result(res); return FVS_ERR_NOT_FOUND; }
    char aid[37];
    (void)snprintf(aid,sizeof aid,"%s",r[0]);
    mysql_free_result(res);
    return attempt_json(ctx,aid,out,out_size);
}

int fvs_payment_confirm(fvs_ctx *ctx,const char *attempt_id,const char *provider,const char *provider_payment_id,int64_t amount_minor,const char *currency,uint64_t checkout_version,char *out,size_t out_size){
    if(!ctx||!valid_id(attempt_id)||!fvs_provider_valid(provider)||!provider_payment_id||!fvs_currency_valid(currency)||amount_minor<=0)return FVS_ERR_INVALID;
    char *epp=sql_escape(ctx,provider_payment_id);
    if(!epp)return FVS_ERR_INTERNAL;

    MYSQL_RES *res=NULL;
    int entitlement_exhausted=0;
    int rc=queryf(ctx,&res,"SELECT cart_id FROM payment_attempts WHERE id='%s'",attempt_id);
    if(rc!=FVS_OK)goto done;
    MYSQL_ROW r=mysql_fetch_row(res);
    if(!r){mysql_free_result(res);rc=FVS_ERR_NOT_FOUND;goto done;}
    char cart_id[37];
    (void)snprintf(cart_id,sizeof cart_id,"%s",r[0]);
    mysql_free_result(res);

    rc=tx_begin(ctx);
    if(rc!=FVS_OK)goto done;

    rc=queryf(ctx,&res,
        "SELECT status,version,COALESCE(source_channel,''),COALESCE(source_campaign_id,''),COALESCE(source_creative_id,''),"
        "COALESCE(social_link_token,''),COALESCE(promo_code,''),COALESCE(referral_code,'') FROM carts WHERE id='%s' FOR UPDATE",cart_id);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    r=mysql_fetch_row(res);
    if(!r){mysql_free_result(res);tx_rollback(ctx);rc=FVS_ERR_NOT_FOUND;goto done;}
    char cstatus[24],source_channel[33],source_campaign[37],source_creative[37],social_token[33],cart_promo[65],cart_referral[65];
    (void)snprintf(cstatus,sizeof cstatus,"%s",r[0]?r[0]:"");
    unsigned long long cversion=r[1]?strtoull(r[1],NULL,10):0ULL;
    (void)snprintf(source_channel,sizeof source_channel,"%s",r[2]?r[2]:"");
    (void)snprintf(source_campaign,sizeof source_campaign,"%s",r[3]?r[3]:"");
    (void)snprintf(source_creative,sizeof source_creative,"%s",r[4]?r[4]:"");
    (void)snprintf(social_token,sizeof social_token,"%s",r[5]?r[5]:"");
    (void)snprintf(cart_promo,sizeof cart_promo,"%s",r[6]?r[6]:"");
    (void)snprintf(cart_referral,sizeof cart_referral,"%s",r[7]?r[7]:"");
    mysql_free_result(res);

    rc=queryf(ctx,&res,
        "SELECT status,cart_version,provider,amount_minor,currency,provider_payment_id,provider_checkout_id,"
        "subtotal_minor,addons_minor,discount_minor,COALESCE(promo_code,''),COALESCE(source_channel,'') "
        "FROM payment_attempts WHERE id='%s' FOR UPDATE",attempt_id);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    r=mysql_fetch_row(res);
    if(!r){mysql_free_result(res);tx_rollback(ctx);rc=FVS_ERR_NOT_FOUND;goto done;}
    char astatus[24],aprov[24],acur[8],existing_pid[256],existing_checkout[256],attempt_promo[65],attempt_source[33];
    (void)snprintf(astatus,sizeof astatus,"%s",r[0]?r[0]:"");
    unsigned long long aversion=r[1]?strtoull(r[1],NULL,10):0ULL;
    (void)snprintf(aprov,sizeof aprov,"%s",r[2]?r[2]:"");
    long long expected=r[3]?strtoll(r[3],NULL,10):0LL;
    (void)snprintf(acur,sizeof acur,"%s",r[4]?r[4]:"");
    (void)snprintf(existing_pid,sizeof existing_pid,"%s",r[5]?r[5]:"");
    (void)snprintf(existing_checkout,sizeof existing_checkout,"%s",r[6]?r[6]:"");
    long long expected_subtotal=r[7]?strtoll(r[7],NULL,10):0LL;
    long long expected_addons=r[8]?strtoll(r[8],NULL,10):0LL;
    long long expected_discount=r[9]?strtoll(r[9],NULL,10):0LL;
    (void)snprintf(attempt_promo,sizeof attempt_promo,"%s",r[10]?r[10]:"");
    (void)snprintf(attempt_source,sizeof attempt_source,"%s",r[11]?r[11]:"");
    mysql_free_result(res);

    if(strcmp(astatus,"paid")==0){
        rc=tx_commit(ctx);
        if(rc==FVS_OK){
            rc=queryf(ctx,&res,"SELECT id,total_minor,discount_minor,currency FROM orders WHERE payment_attempt_id='%s'",attempt_id);
            if(rc==FVS_OK){
                r=mysql_fetch_row(res);
                if(r){
                    long long total=r[1]?strtoll(r[1],NULL,10):0LL;
                    long long discount=r[2]?strtoll(r[2],NULL,10):0LL;
                    const char *order_currency=r[3]?r[3]:"";
                    jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"status\":\"confirmed\",\"order_id\":");jw_string(&w,r[0]);jw_printf(&w,",\"total_minor\":%lld,\"discount_minor\":%lld,\"currency\":",total,discount);jw_string(&w,order_currency);jw_puts(&w,"}");rc=jw_result(ctx,&w);
                }else rc=FVS_ERR_STATE;
                mysql_free_result(res);
            }
        }
        goto done;
    }

    int state_ok=strcmp(astatus,"creating")==0||strcmp(astatus,"pending")==0;
    int mismatch=!state_ok||strcmp(cstatus,"checkout")!=0||cversion!=aversion||aversion!=checkout_version||strcmp(aprov,provider)!=0||
        expected!=amount_minor||!currency_equal(acur,currency)||!existing_pid[0]||strcmp(existing_pid,provider_payment_id)!=0||
        (strcmp(provider,"stripe")==0&&(!existing_checkout[0]||strcmp(existing_checkout,provider_payment_id)!=0));
    if(mismatch){
        char *sql=NULL;
        rc=sqlf(ctx,&sql,"UPDATE payment_attempts SET status='manual_review',provider_payment_id=COALESCE(provider_payment_id,'%s'),last_error='payment reconciliation mismatch',updated_at=UTC_TIMESTAMP() WHERE id='%s'",epp,attempt_id);
        if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
        if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE carts SET status='manual_review',updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
        if(rc==FVS_OK)rc=tx_commit(ctx);else tx_rollback(ctx);
        if(rc==FVS_OK){jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"status\":\"manual_review\"}");rc=jw_result(ctx,&w);}goto done;
    }

    fvs_cart_price price;
    rc=cart_price_snapshot(ctx,cart_id,&price);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    int price_mismatch=!price.code_valid||price.item_count==0UL||price.item_count>64UL||price.total_minor!=expected||
        price.subtotal_minor!=expected_subtotal||price.addons_minor!=expected_addons||price.discount_minor!=expected_discount||
        !currency_equal(price.currency,acur)||strcmp(price.promo_code,attempt_promo)!=0||strcmp(price.source_channel,attempt_source)!=0;
    if(price_mismatch){
        char *sql=NULL;
        rc=sqlf(ctx,&sql,"UPDATE payment_attempts SET status='manual_review',last_error='commerce quote reconciliation mismatch',updated_at=UTC_TIMESTAMP() WHERE id='%s'",attempt_id);
        if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
        if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE carts SET status='manual_review',updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
        if(rc==FVS_OK)rc=tx_commit(ctx);else tx_rollback(ctx);
        if(rc==FVS_OK){jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"status\":\"manual_review\"}");rc=jw_result(ctx,&w);}goto done;
    }

    rc=queryf(ctx,&res,"SELECT s.id,s.is_booked,s.active,s.held_by_cart_id,(s.hold_expires_at>UTC_TIMESTAMP()),s.max_guests,ci.guests FROM cart_items ci JOIN inventory_slots s ON s.id=ci.slot_id WHERE ci.cart_id='%s' ORDER BY s.id FOR UPDATE",cart_id);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    int inv_bad=0;MYSQL_ROW ir;
    while((ir=mysql_fetch_row(res))!=NULL){
        int booked=ir[1]?atoi(ir[1]):0,active=ir[2]?atoi(ir[2]):0,live=ir[4]?atoi(ir[4]):0;
        unsigned int maxg=ir[5]?(unsigned int)strtoul(ir[5],NULL,10):0U,g=ir[6]?(unsigned int)strtoul(ir[6],NULL,10):0U;
        if(booked||!active||g>maxg||(live&&ir[3]&&strcmp(ir[3],cart_id)!=0)){inv_bad=1;break;}
    }
    mysql_free_result(res);
    if(inv_bad){
        char *sql=NULL;
        rc=sqlf(ctx,&sql,"UPDATE payment_attempts SET status='manual_review',last_error='inventory conflict after payment',updated_at=UTC_TIMESTAMP() WHERE id='%s'",attempt_id);
        if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
        if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE carts SET status='manual_review',updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
        if(rc==FVS_OK)rc=tx_commit(ctx);else tx_rollback(ctx);
        if(rc==FVS_OK){jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"status\":\"manual_review\"}");rc=jw_result(ctx,&w);}goto done;
    }

    char order_id[37];
    rc=fvs_uuid_v4(order_id);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    char *sql=NULL;
    rc=sqlf(ctx,&sql,
        "INSERT INTO orders(id,cart_id,payment_attempt_id,status,total_minor,subtotal_minor,addons_minor,discount_minor,promo_code,source_channel,source_campaign_id,source_creative_id,currency,customer_email,created_at,updated_at) "
        "SELECT '%s',c.id,'%s','confirmed',p.amount_minor,p.subtotal_minor,p.addons_minor,p.discount_minor,p.promo_code,p.source_channel,c.source_campaign_id,c.source_creative_id,p.currency,p.email,UTC_TIMESTAMP(),UTC_TIMESTAMP() "
        "FROM carts c JOIN payment_attempts p ON p.cart_id=c.id WHERE c.id='%s' AND p.id='%s'",
        order_id,attempt_id,cart_id,attempt_id);
    if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
    if(rc==FVS_OK){rc=sqlf(ctx,&sql,"INSERT INTO order_items(id,order_id,slot_id,guests,price_minor,currency,resort,unit_code,unit_name,check_in,check_out,city,country,week_number,booking_mode,float_group,created_at) SELECT UUID(),'%s',ci.slot_id,ci.guests,ci.price_minor,ci.currency,ci.resort_snapshot,ci.unit_code_snapshot,ci.unit_name_snapshot,ci.check_in_snapshot,ci.check_out_snapshot,ci.city_snapshot,ci.country_snapshot,ci.week_number_snapshot,ci.booking_mode_snapshot,ci.float_group_snapshot,UTC_TIMESTAMP() FROM cart_items ci WHERE ci.cart_id='%s' ORDER BY ci.slot_id",order_id,cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
    if(rc==FVS_OK){rc=sqlf(ctx,&sql,"INSERT INTO order_addons(id,order_id,addon_id,code,name,quantity,unit_price_minor,total_minor,currency,created_at) SELECT UUID(),'%s',ca.addon_id,ca.code_snapshot,ca.name_snapshot,ca.quantity,ca.unit_price_minor,ca.unit_price_minor*ca.quantity,ca.currency,UTC_TIMESTAMP() FROM cart_addons ca WHERE ca.cart_id='%s'",order_id,cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
    if(rc==FVS_OK&&price.discount_minor>0&&(cart_promo[0]||cart_referral[0])){const char *used_code=cart_promo[0]?cart_promo:cart_referral;rc=sqlf(ctx,&sql,"INSERT INTO order_promotions(id,order_id,promo_code,discount_minor,currency,created_at) VALUES(UUID(),'%s','%s',%lld,'%s',UTC_TIMESTAMP())",order_id,used_code,(long long)price.discount_minor,price.currency);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
    if(rc==FVS_OK&&cart_promo[0]){
        rc=sqlf(ctx,&sql,"UPDATE commerce_promotions SET usage_count=usage_count+1,updated_at=UTC_TIMESTAMP() WHERE code='%s' AND active=1 AND (starts_at IS NULL OR starts_at<=UTC_TIMESTAMP()) AND (ends_at IS NULL OR ends_at>UTC_TIMESTAMP()) AND (usage_limit=0 OR usage_count<usage_limit)",cart_promo);
        if(rc==FVS_OK){
            rc=exec_sql(ctx,sql);
            if(rc==FVS_OK&&mysql_affected_rows(ctx->db)!=1ULL){seterr(ctx,"promotion usage exhausted");entitlement_exhausted=1;rc=FVS_ERR_CONFLICT;}
        }
        free(sql);
    }
    if(rc==FVS_OK&&cart_referral[0]){
        rc=sqlf(ctx,&sql,"UPDATE commerce_referral_codes SET usage_count=usage_count+1,updated_at=UTC_TIMESTAMP() WHERE code='%s' AND active=1 AND (starts_at IS NULL OR starts_at<=UTC_TIMESTAMP()) AND (ends_at IS NULL OR ends_at>UTC_TIMESTAMP()) AND (usage_limit=0 OR usage_count<usage_limit)",cart_referral);
        if(rc==FVS_OK){
            rc=exec_sql(ctx,sql);
            if(rc==FVS_OK&&mysql_affected_rows(ctx->db)!=1ULL){seterr(ctx,"referral usage exhausted");entitlement_exhausted=1;rc=FVS_ERR_CONFLICT;}
        }
        free(sql);
    }
    if(rc==FVS_OK){
        rc=sqlf(ctx,&sql,"INSERT INTO commerce_loyalty_accounts(id,email_hash,points_balance,lifetime_points,tier,created_at,updated_at) SELECT UUID(),UNHEX(SHA2(LOWER(TRIM(email)),256)),0,0,'explorer',UTC_TIMESTAMP(),UTC_TIMESTAMP() FROM payment_attempts WHERE id='%s' ON DUPLICATE KEY UPDATE updated_at=UTC_TIMESTAMP()",attempt_id);
        if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
    }
    if(rc==FVS_OK){
        char loyalty_ledger_id[37];
        if(fvs_uuid_v4(loyalty_ledger_id)!=FVS_OK){ rc=FVS_ERR_INTERNAL; }
        if(rc==FVS_OK){
            rc=sqlf(ctx,&sql,
                "INSERT IGNORE INTO commerce_loyalty_ledger(id,account_id,order_id,event_type,points_delta,detail_json,created_at) "
                "SELECT '%s',la.id,'%s','earn',FLOOR(FLOOR(p.amount_minor/lr.minor_per_point)*COALESCE(MAX(mp.points_multiplier_bps),10000)/10000),"
                "JSON_OBJECT('currency',p.currency,'amount_minor',p.amount_minor,'rule_minor_per_point',lr.minor_per_point),UTC_TIMESTAMP() "
                "FROM payment_attempts p JOIN commerce_loyalty_rules lr ON lr.currency=p.currency AND lr.active=1 "
                "JOIN commerce_loyalty_accounts la ON la.email_hash=UNHEX(SHA2(LOWER(TRIM(p.email)),256)) "
                "LEFT JOIN commerce_memberships m ON m.email_hash=la.email_hash AND m.status='active' AND m.starts_at<=UTC_TIMESTAMP() AND (m.ends_at IS NULL OR m.ends_at>UTC_TIMESTAMP()) "
                "LEFT JOIN commerce_membership_plans mp ON mp.id=m.plan_id AND mp.active=1 WHERE p.id='%s' "
                "GROUP BY la.id,p.amount_minor,p.currency,lr.minor_per_point HAVING FLOOR(FLOOR(p.amount_minor/lr.minor_per_point)*COALESCE(MAX(mp.points_multiplier_bps),10000)/10000)>0",
                loyalty_ledger_id,order_id,attempt_id);
            if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
            free(sql);
        }
        if(rc==FVS_OK){
            rc=sqlf(ctx,&sql,"UPDATE commerce_loyalty_accounts la JOIN commerce_loyalty_ledger l ON l.account_id=la.id SET la.points_balance=la.points_balance+l.points_delta,la.lifetime_points=la.lifetime_points+GREATEST(l.points_delta,0),la.tier=CASE WHEN la.lifetime_points+GREATEST(l.points_delta,0)>=500 THEN 'elite' WHEN la.lifetime_points+GREATEST(l.points_delta,0)>=100 THEN 'insider' ELSE la.tier END,la.updated_at=UTC_TIMESTAMP() WHERE l.id='%s'",loyalty_ledger_id);
            if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
            free(sql);
        }
    }
    if(rc==FVS_OK&&cart_referral[0]){
        rc=sqlf(ctx,&sql,"INSERT INTO commerce_loyalty_accounts(id,email_hash,points_balance,lifetime_points,tier,created_at,updated_at) SELECT UUID(),owner_email_hash,0,0,'explorer',UTC_TIMESTAMP(),UTC_TIMESTAMP() FROM commerce_referral_codes WHERE code='%s' AND owner_email_hash IS NOT NULL ON DUPLICATE KEY UPDATE updated_at=UTC_TIMESTAMP()",cart_referral);
        if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
        char referral_ledger_id[37];
        if(rc==FVS_OK&&fvs_uuid_v4(referral_ledger_id)!=FVS_OK){ rc=FVS_ERR_INTERNAL; }
        if(rc==FVS_OK){
            rc=sqlf(ctx,&sql,"INSERT IGNORE INTO commerce_loyalty_ledger(id,account_id,order_id,event_type,points_delta,detail_json,created_at) SELECT '%s',la.id,'%s','referral',rc.advocate_points,JSON_OBJECT('referral_code',rc.code),UTC_TIMESTAMP() FROM commerce_referral_codes rc JOIN commerce_loyalty_accounts la ON la.email_hash=rc.owner_email_hash WHERE rc.code='%s' AND rc.owner_email_hash IS NOT NULL AND rc.advocate_points>0",referral_ledger_id,order_id,cart_referral);
            if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
            free(sql);
        }
        if(rc==FVS_OK){
            rc=sqlf(ctx,&sql,"UPDATE commerce_loyalty_accounts la JOIN commerce_loyalty_ledger l ON l.account_id=la.id SET la.points_balance=la.points_balance+l.points_delta,la.lifetime_points=la.lifetime_points+GREATEST(l.points_delta,0),la.updated_at=UTC_TIMESTAMP() WHERE l.id='%s'",referral_ledger_id);
            if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
            free(sql);
        }
        if(rc==FVS_OK){
            rc=sqlf(ctx,&sql,"INSERT INTO commerce_referral_events(id,referral_code_id,order_id,event_type,value_minor,created_at) SELECT UUID(),id,'%s','booking',%lld,UTC_TIMESTAMP() FROM commerce_referral_codes WHERE code='%s'",order_id,expected,cart_referral);
            if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
            free(sql);
        }
    }
    if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE inventory_slots s JOIN cart_items ci ON ci.slot_id=s.id SET s.is_booked=1,s.held_by_cart_id=NULL,s.hold_expires_at=NULL,s.updated_at=UTC_TIMESTAMP() WHERE ci.cart_id='%s'",cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
    if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE payment_attempts SET status='paid',provider_payment_id='%s',paid_at=UTC_TIMESTAMP(),updated_at=UTC_TIMESTAMP() WHERE id='%s'",epp,attempt_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
    if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE carts SET status='paid',updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}
    if(rc==FVS_OK&&social_token[0]){
        rc=sqlf(ctx,&sql,"UPDATE social_sales_links SET bookings=bookings+1,updated_at=UTC_TIMESTAMP() WHERE token='%s'",social_token);
        if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
    }
    if(rc==FVS_OK&&social_token[0]){
        rc=sqlf(ctx,&sql,"INSERT INTO social_sales_link_revenue(link_id,currency,bookings,revenue_minor,updated_at) SELECT id,'%s',1,%lld,UTC_TIMESTAMP() FROM social_sales_links WHERE token='%s' ON DUPLICATE KEY UPDATE bookings=social_sales_link_revenue.bookings+1,revenue_minor=social_sales_link_revenue.revenue_minor+VALUES(revenue_minor),updated_at=UTC_TIMESTAMP()",acur,expected,social_token);
        if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
    }
    if(rc==FVS_OK&&source_campaign[0]&&source_channel[0]){
        char conversion_job[37];
        if(fvs_uuid_v4(conversion_job)!=FVS_OK){rc=FVS_ERR_INTERNAL;}
        if(rc==FVS_OK){
            rc=sqlf(ctx,&sql,
                "INSERT INTO marketing_jobs(id,campaign_id,creative_id,channel,action,payload_json,scheduled_at,status,attempts,next_attempt_at,created_at,updated_at) "
                "VALUES('%s','%s',NULLIF('%s',''),'%s','send_conversion',JSON_OBJECT('order_id','%s','value_minor',%lld,'currency','%s','payment_provider','%s'),UTC_TIMESTAMP(),'pending',0,UTC_TIMESTAMP(),UTC_TIMESTAMP(),UTC_TIMESTAMP())",
                conversion_job,source_campaign,source_creative,source_channel,order_id,expected,acur,provider);
            if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);
        }
    }
    if(rc==FVS_OK){char event_id[37];if(fvs_uuid_v4(event_id)!=FVS_OK)rc=FVS_ERR_INTERNAL;else{rc=sqlf(ctx,&sql,"INSERT INTO outbox_events(id,event_type,aggregate_id,payload_json,status,attempts,next_attempt_at,created_at,updated_at) VALUES('%s','order.confirmed','%s',JSON_OBJECT('order_id','%s'),'pending',0,UTC_TIMESTAMP(),UTC_TIMESTAMP(),UTC_TIMESTAMP())",event_id,order_id,order_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }
        free(sql);}}
    if(rc!=FVS_OK){
        tx_rollback(ctx);
        if(entitlement_exhausted){
            int mrc=tx_begin(ctx);
            char *msql=NULL;
            if(mrc==FVS_OK){
                mrc=sqlf(ctx,&msql,"UPDATE payment_attempts SET status='manual_review',last_error='promotion or referral entitlement exhausted after provider payment',updated_at=UTC_TIMESTAMP() WHERE id='%s'",attempt_id);
                if(mrc==FVS_OK)mrc=exec_sql(ctx,msql);
                free(msql);msql=NULL;
            }
            if(mrc==FVS_OK){
                mrc=sqlf(ctx,&msql,"UPDATE carts SET status='manual_review',updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);
                if(mrc==FVS_OK)mrc=exec_sql(ctx,msql);
                free(msql);
            }
            if(mrc==FVS_OK)mrc=tx_commit(ctx);else tx_rollback(ctx);
            if(mrc==FVS_OK){
                jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"status\":\"manual_review\",\"reason\":\"commerce_entitlement_exhausted\"}");rc=jw_result(ctx,&w);goto done;
            }
        }
        goto done;
    }
    rc=tx_commit(ctx);
    if(rc==FVS_OK){jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"status\":\"confirmed\",\"order_id\":");jw_string(&w,order_id);jw_printf(&w,",\"total_minor\":%lld,\"discount_minor\":%lld,\"currency\":",expected,expected_discount);jw_string(&w,acur);jw_puts(&w,"}");rc=jw_result(ctx,&w);}

done:
    free(epp);
    return rc;
}

int fvs_webhook_claim_v2(fvs_ctx *ctx,const char *provider,const char *event_id,const char *lease_owner,unsigned int lease_seconds,char *out_lease_token,size_t token_size,int *out_claim){
    if(!ctx||!fvs_provider_valid(provider)||!event_id||!lease_owner||strlen(event_id)>255U||strlen(lease_owner)>128U||lease_seconds<5U||lease_seconds>3600U||!out_lease_token||token_size<33U||!out_claim)return FVS_ERR_INVALID;
    out_lease_token[0]='\0';
    char *ep=sql_escape(ctx,provider),*ee=sql_escape(ctx,event_id),*el=sql_escape(ctx,lease_owner);
    if(!ep||!ee||!el){free(ep);free(ee);free(el);return FVS_ERR_INTERNAL;}
    char token[33];
    int rc=fvs_random_hex(token,sizeof token,16U);
    if(rc!=FVS_OK){free(ep);free(ee);free(el);return rc;}
    rc=tx_begin(ctx);
    if(rc!=FVS_OK)goto done;
    char row_id[37];
    rc=fvs_uuid_v4(row_id);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    char *sql=NULL;
    rc=sqlf(ctx,&sql,
        "INSERT IGNORE INTO webhook_events(id,provider,event_id,attempts,created_at,updated_at) VALUES('%s','%s','%s',0,UTC_TIMESTAMP(),UTC_TIMESTAMP())",
        row_id,ep,ee);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}

    MYSQL_RES *res=NULL;
    rc=queryf(ctx,&res,
        "SELECT processed_at,(lease_until>UTC_TIMESTAMP()) FROM webhook_events WHERE provider='%s' AND event_id='%s' FOR UPDATE",
        ep,ee);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    MYSQL_ROW r=mysql_fetch_row(res);
    if(!r){mysql_free_result(res);seterr(ctx,"webhook event row missing");tx_rollback(ctx);rc=FVS_ERR_DB;goto done;}
    int processed=r[0]!=NULL;
    int busy=r[1]?atoi(r[1]):0;
    mysql_free_result(res);
    if(processed){*out_claim=FVS_CLAIM_DONE;rc=tx_commit(ctx);goto done;}
    if(busy){*out_claim=FVS_CLAIM_BUSY;rc=tx_commit(ctx);goto done;}

    rc=sqlf(ctx,&sql,
        "UPDATE webhook_events SET lease_owner='%s',lease_token='%s',lease_until=TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),attempts=attempts+1,updated_at=UTC_TIMESTAMP() "
        "WHERE provider='%s' AND event_id='%s' AND processed_at IS NULL AND (lease_until IS NULL OR lease_until<=UTC_TIMESTAMP())",
        el,token,lease_seconds,ep,ee);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);
    if(rc!=FVS_OK){tx_rollback(ctx);goto done;}
    if(mysql_affected_rows(ctx->db)!=1ULL){*out_claim=FVS_CLAIM_BUSY;rc=tx_commit(ctx);goto done;}
    *out_claim=FVS_CLAIM_ACQUIRED;
    memcpy(out_lease_token,token,33U);
    rc=tx_commit(ctx);
done:
    free(ep);free(ee);free(el);return rc;
}

int fvs_webhook_complete_v2(fvs_ctx *ctx,const char *provider,const char *event_id,const char *lease_owner,const char *lease_token,int success,const char *last_error){
    if(!ctx||!fvs_provider_valid(provider)||!event_id||!lease_owner||!lease_token||strlen(lease_token)!=32U)return FVS_ERR_INVALID;
    char *ep=sql_escape(ctx,provider),*ee=sql_escape(ctx,event_id),*el=sql_escape(ctx,lease_owner),*et=sql_escape(ctx,lease_token);
    char *er=last_error?sql_escape(ctx,last_error):NULL;
    if(!ep||!ee||!el||!et||(last_error&&!er)){free(ep);free(ee);free(el);free(et);free(er);return FVS_ERR_INTERNAL;}
    char *sql=NULL;
    int rc;
    if(success)rc=sqlf(ctx,&sql,
        "UPDATE webhook_events SET processed_at=UTC_TIMESTAMP(),lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=NULL,updated_at=UTC_TIMESTAMP() "
        "WHERE provider='%s' AND event_id='%s' AND lease_owner='%s' AND lease_token='%s' AND processed_at IS NULL",
        ep,ee,el,et);
    else rc=sqlf(ctx,&sql,
        "UPDATE webhook_events SET lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=%s%s%s,updated_at=UTC_TIMESTAMP() "
        "WHERE provider='%s' AND event_id='%s' AND lease_owner='%s' AND lease_token='%s' AND processed_at IS NULL",
        er?"'":"",er?er:"NULL",er?"'":"",ep,ee,el,et);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);free(ep);free(ee);free(el);free(et);free(er);
    if(rc!=FVS_OK)return rc;
    if(mysql_affected_rows(ctx->db)==0ULL){seterr(ctx,"webhook lease lost");return FVS_ERR_CONFLICT;}
    return FVS_OK;
}

/* Backward-compatible ABI. New shims use v2 fencing tokens. */
int fvs_webhook_claim(fvs_ctx *ctx,const char *provider,const char *event_id,const char *lease_owner,unsigned int lease_seconds,int *out_claim){
    char token[33];
    return fvs_webhook_claim_v2(ctx,provider,event_id,lease_owner,lease_seconds,token,sizeof token,out_claim);
}

int fvs_webhook_complete(fvs_ctx *ctx,const char *provider,const char *event_id,const char *lease_owner,int success,const char *last_error){
    if(!ctx||!fvs_provider_valid(provider)||!event_id||!lease_owner)return FVS_ERR_INVALID;
    char *ep=sql_escape(ctx,provider),*ee=sql_escape(ctx,event_id),*el=sql_escape(ctx,lease_owner),*er=last_error?sql_escape(ctx,last_error):NULL;
    if(!ep||!ee||!el||(last_error&&!er)){free(ep);free(ee);free(el);free(er);return FVS_ERR_INTERNAL;}
    char *sql=NULL;
    int rc;
    if(success)rc=sqlf(ctx,&sql,"UPDATE webhook_events SET processed_at=UTC_TIMESTAMP(),lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=NULL,updated_at=UTC_TIMESTAMP() WHERE provider='%s' AND event_id='%s' AND lease_owner='%s' AND processed_at IS NULL",ep,ee,el);
    else rc=sqlf(ctx,&sql,"UPDATE webhook_events SET lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=%s%s%s,updated_at=UTC_TIMESTAMP() WHERE provider='%s' AND event_id='%s' AND lease_owner='%s' AND processed_at IS NULL",er?"'":"",er?er:"NULL",er?"'":"",ep,ee,el);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);free(ep);free(ee);free(el);free(er);
    if(rc!=FVS_OK)return rc;
    if(mysql_affected_rows(ctx->db)==0ULL){seterr(ctx,"webhook lease lost");return FVS_ERR_CONFLICT;}
    return FVS_OK;
}

int fvs_outbox_claim(fvs_ctx *ctx,const char *lease_owner,unsigned int lease_seconds,char *out,size_t out_size){
    if (!ctx || !lease_owner || strlen(lease_owner) > 128U || lease_seconds < 5U || lease_seconds > 3600U || !out) return FVS_ERR_INVALID;
    char *el = sql_escape(ctx,lease_owner);
    if (!el) return FVS_ERR_INTERNAL;
    char lease_token[33];
    int rc = fvs_random_hex(lease_token,sizeof lease_token,16U);
    if (rc != FVS_OK) { free(el); return rc; }
    int trc = tx_begin(ctx);
    if (trc != FVS_OK) { free(el); return trc; }
    MYSQL_RES *res = NULL;
    rc = queryf(ctx,&res,
        "SELECT id,event_type,aggregate_id,payload_json,attempts FROM outbox_events "
        "WHERE attempts < 8 AND ((status IN('pending','retry') AND next_attempt_at<=UTC_TIMESTAMP()) "
        "OR (status='processing' AND lease_until<=UTC_TIMESTAMP())) "
        "AND (lease_until IS NULL OR lease_until<=UTC_TIMESTAMP()) "
        "ORDER BY CASE WHEN event_type='order.confirmed' THEN 0 WHEN event_type='support.ai_requested' THEN 10 ELSE 5 END,created_at,id LIMIT 1 FOR UPDATE SKIP LOCKED");
    if (rc != FVS_OK) { tx_rollback(ctx); free(el); return rc; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        rc = tx_commit(ctx);
        free(el);
        if (rc != FVS_OK) return rc;
        if (out_size < 3U) return FVS_ERR_BUFFER;
        memcpy(out,"{}",3U);
        return FVS_OK;
    }
    char id[37],etype[81],agg[37],payload[8192];
    unsigned int attempts = row[4] ? (unsigned int)strtoul(row[4],NULL,10) : 0U;
    if (attempts >= 8U) {
        mysql_free_result(res); tx_rollback(ctx); free(el);
        seterr(ctx,"outbox retry limit reached"); return FVS_ERR_STATE;
    }
    unsigned int claim_attempt = attempts + 1U;
    (void)snprintf(id,sizeof id,"%s",row[0]);
    (void)snprintf(etype,sizeof etype,"%s",row[1] ? row[1] : "");
    (void)snprintf(agg,sizeof agg,"%s",row[2] ? row[2] : "");
    const char *payload_src = row[3] ? row[3] : "{}";
    if (strlen(payload_src) >= sizeof payload) {
        mysql_free_result(res); tx_rollback(ctx); free(el);
        seterr(ctx,"outbox payload exceeds core limit"); return FVS_ERR_BUFFER;
    }
    (void)snprintf(payload,sizeof payload,"%s",payload_src);
    mysql_free_result(res);

    char *sql = NULL;
    rc = sqlf(ctx,&sql,
        "UPDATE outbox_events SET lease_owner='%s',lease_token='%s',lease_until=TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),"
        "status='processing',attempts=attempts+1,updated_at=UTC_TIMESTAMP() WHERE id='%s' AND attempts < 8",
        el,lease_token,lease_seconds,id);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql);
    if (rc != FVS_OK) { tx_rollback(ctx); free(el); return rc; }
    rc = tx_commit(ctx);
    free(el);
    if (rc != FVS_OK) return rc;

    jsonw w; jw_init(&w,out,out_size);
    jw_puts(&w,"{\"event_id\":"); jw_string(&w,id);
    jw_puts(&w,",\"event_type\":"); jw_string(&w,etype);
    jw_puts(&w,",\"aggregate_id\":"); jw_string(&w,agg);
    jw_puts(&w,",\"lease_token\":"); jw_string(&w,lease_token);
    jw_printf(&w,",\"attempts\":%u,\"payload\":",claim_attempt);
    jw_puts(&w,payload); jw_puts(&w,"}");
    return jw_result(ctx,&w);
}

int fvs_outbox_ack(fvs_ctx *ctx,const char *event_id,const char *lease_owner,const char *lease_token){
    if (!ctx || !valid_id(event_id) || !lease_owner || !lease_token || strlen(lease_token) != 32U) return FVS_ERR_INVALID;
    char *el = sql_escape(ctx,lease_owner);
    char *et = sql_escape(ctx,lease_token);
    if (!el || !et) { free(el); free(et); return FVS_ERR_INTERNAL; }
    char *sql = NULL;
    int rc = sqlf(ctx,&sql,
        "UPDATE outbox_events SET status='done',processed_at=UTC_TIMESTAMP(),lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=NULL,updated_at=UTC_TIMESTAMP() "
        "WHERE id='%s' AND lease_owner='%s' AND lease_token='%s' AND status='processing'",
        event_id,el,et);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql); free(el); free(et);
    if (rc != FVS_OK) return rc;
    if (mysql_affected_rows(ctx->db) == 0ULL) { seterr(ctx,"outbox lease lost"); return FVS_ERR_CONFLICT; }
    return FVS_OK;
}

int fvs_outbox_nack(fvs_ctx *ctx,const char *event_id,const char *lease_owner,const char *lease_token,const char *error_message){
    if (!ctx || !valid_id(event_id) || !lease_owner || !lease_token || strlen(lease_token) != 32U) return FVS_ERR_INVALID;
    char *el = sql_escape(ctx,lease_owner);
    char *et = sql_escape(ctx,lease_token);
    char *ee = error_message ? sql_escape(ctx,error_message) : NULL;
    if (!el || !et || (error_message && !ee)) { free(el); free(et); free(ee); return FVS_ERR_INTERNAL; }
    char *sql = NULL;
    int rc = sqlf(ctx,&sql,
        "UPDATE outbox_events SET status=IF(attempts>=8,'dead','retry'),"
        "next_attempt_at=TIMESTAMPADD(SECOND,LEAST(3600,15*(1 << LEAST(GREATEST(attempts-1,0),8))),UTC_TIMESTAMP()),"
        "lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=%s%s%s,updated_at=UTC_TIMESTAMP() "
        "WHERE id='%s' AND lease_owner='%s' AND lease_token='%s' AND status='processing'",
        ee?"'":"",ee?ee:"NULL",ee?"'":"",event_id,el,et);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql); free(el); free(et); free(ee);
    if (rc != FVS_OK) return rc;
    if (mysql_affected_rows(ctx->db) == 0ULL) { seterr(ctx,"outbox lease lost"); return FVS_ERR_CONFLICT; }
    return FVS_OK;
}

int fvs_maintenance_sweep(fvs_ctx *ctx,unsigned int open_cart_ttl_seconds,unsigned int *out_expired_carts,unsigned int *out_released_holds){
    if(!ctx||!out_expired_carts||!out_released_holds||open_cart_ttl_seconds<3600U||open_cart_ttl_seconds>2592000U)return FVS_ERR_INVALID;
    *out_expired_carts=0U;*out_released_holds=0U;
    int rc=tx_begin(ctx);if(rc!=FVS_OK)return rc;
    char *sql=NULL;rc=sqlf(ctx,&sql,"UPDATE inventory_slots SET held_by_cart_id=NULL,hold_expires_at=NULL,updated_at=UTC_TIMESTAMP() WHERE is_booked=0 AND hold_expires_at IS NOT NULL AND hold_expires_at<=UTC_TIMESTAMP()");
    if(rc==FVS_OK){rc=exec_sql(ctx,sql);}
    free(sql);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;}
    unsigned long long released=mysql_affected_rows(ctx->db);if(released>4294967295ULL){tx_rollback(ctx);seterr(ctx,"released hold count overflow");return FVS_ERR_OVERFLOW;}
    rc=sqlf(ctx,&sql,"UPDATE carts c SET c.status='expired',c.updated_at=UTC_TIMESTAMP() WHERE c.status='open' AND c.updated_at<TIMESTAMPADD(SECOND,-%u,UTC_TIMESTAMP()) AND NOT EXISTS(SELECT 1 FROM inventory_slots s WHERE s.held_by_cart_id=c.id AND s.hold_expires_at>UTC_TIMESTAMP())",open_cart_ttl_seconds);
    if(rc==FVS_OK){rc=exec_sql(ctx,sql);}
    free(sql);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;}
    unsigned long long expired=mysql_affected_rows(ctx->db);if(expired>4294967295ULL){tx_rollback(ctx);seterr(ctx,"expired cart count overflow");return FVS_ERR_OVERFLOW;}
    rc=tx_commit(ctx);if(rc!=FVS_OK)return rc;
    *out_released_holds=(unsigned int)released;*out_expired_carts=(unsigned int)expired;return FVS_OK;
}

int fvs_order_get(fvs_ctx *ctx,const char *order_id,char *out,size_t out_size){
    if(!ctx||!valid_id(order_id)||!out){ return FVS_ERR_INVALID; }
    MYSQL_RES *res=NULL;
    int rc=queryf(ctx,&res,
        "SELECT status,total_minor,subtotal_minor,addons_minor,discount_minor,COALESCE(promo_code,''),COALESCE(source_channel,''),"
        "COALESCE(source_campaign_id,''),COALESCE(source_creative_id,''),currency,customer_email,DATE_FORMAT(created_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') "
        "FROM orders WHERE id='%s'",order_id);
    if(rc!=FVS_OK){ return rc; }
    MYSQL_ROW r=mysql_fetch_row(res);
    if(!r){ mysql_free_result(res); return FVS_ERR_NOT_FOUND; }
    char status[24],promo[65],source[33],campaign[37],creative[37],cur[8],email[256],created[32];
    long long total=r[1]?strtoll(r[1],NULL,10):0LL;
    long long subtotal=r[2]?strtoll(r[2],NULL,10):0LL;
    long long addons=r[3]?strtoll(r[3],NULL,10):0LL;
    long long discount=r[4]?strtoll(r[4],NULL,10):0LL;
    (void)snprintf(status,sizeof status,"%s",r[0]?r[0]:"");
    (void)snprintf(promo,sizeof promo,"%s",r[5]?r[5]:"");
    (void)snprintf(source,sizeof source,"%s",r[6]?r[6]:"");
    (void)snprintf(campaign,sizeof campaign,"%s",r[7]?r[7]:"");
    (void)snprintf(creative,sizeof creative,"%s",r[8]?r[8]:"");
    (void)snprintf(cur,sizeof cur,"%s",r[9]?r[9]:"");
    (void)snprintf(email,sizeof email,"%s",r[10]?r[10]:"");
    (void)snprintf(created,sizeof created,"%s",r[11]?r[11]:"");
    mysql_free_result(res);

    jsonw w;jw_init(&w,out,out_size);
    jw_puts(&w,"{\"order_id\":");jw_string(&w,order_id);
    jw_puts(&w,",\"status\":");jw_string(&w,status);
    jw_printf(&w,",\"total_minor\":%lld,\"subtotal_minor\":%lld,\"addons_minor\":%lld,\"discount_minor\":%lld,\"currency\":",total,subtotal,addons,discount);jw_string(&w,cur);
    jw_puts(&w,",\"promo_code\":");promo[0]?jw_string(&w,promo):jw_puts(&w,"null");
    jw_puts(&w,",\"source_channel\":");source[0]?jw_string(&w,source):jw_puts(&w,"null");
    jw_puts(&w,",\"source_campaign_id\":");campaign[0]?jw_string(&w,campaign):jw_puts(&w,"null");
    jw_puts(&w,",\"source_creative_id\":");creative[0]?jw_string(&w,creative):jw_puts(&w,"null");
    jw_puts(&w,",\"customer_email\":");jw_string(&w,email);
    jw_puts(&w,",\"created_at\":");jw_string(&w,created);
    jw_puts(&w,",\"items\":[");
    rc=queryf(ctx,&res,"SELECT slot_id,guests,price_minor,currency,resort,unit_code,unit_name,DATE_FORMAT(check_in,'%%Y-%%m-%%d'),DATE_FORMAT(check_out,'%%Y-%%m-%%d'),city,country,week_number,booking_mode,float_group FROM order_items WHERE order_id='%s' ORDER BY check_in,resort,unit_code",order_id);
    if(rc!=FVS_OK){ return rc; }
    int first=1;
    while((r=mysql_fetch_row(res))!=NULL){
        if(!first){ jw_puts(&w,","); } first=0;
        jw_puts(&w,"{\"slot_id\":");jw_string(&w,r[0]);
        jw_printf(&w,",\"guests\":%u,\"price_minor\":%lld,\"currency\":",r[1]?(unsigned int)strtoul(r[1],NULL,10):0U,r[2]?strtoll(r[2],NULL,10):0LL);jw_string(&w,r[3]);
        jw_puts(&w,",\"resort\":");jw_string(&w,r[4]);jw_puts(&w,",\"unit_code\":");jw_string(&w,r[5]);jw_puts(&w,",\"unit_name\":");jw_string(&w,r[6]);
        jw_puts(&w,",\"check_in\":");jw_string(&w,r[7]);jw_puts(&w,",\"check_out\":");jw_string(&w,r[8]);jw_puts(&w,",\"city\":");jw_string(&w,r[9]);jw_puts(&w,",\"country\":");jw_string(&w,r[10]);
        jw_printf(&w,",\"week_number\":%u,\"booking_mode\":",r[11]?(unsigned int)strtoul(r[11],NULL,10):0U);jw_string(&w,r[12]);jw_puts(&w,",\"float_group\":");jw_string(&w,r[13]);jw_puts(&w,"}");
    }
    mysql_free_result(res);
    jw_puts(&w,"],\"addons\":[");
    rc=queryf(ctx,&res,"SELECT code,name,quantity,unit_price_minor,total_minor,currency FROM order_addons WHERE order_id='%s' ORDER BY created_at,id",order_id);
    if(rc!=FVS_OK){ return rc; }
    first=1;
    while((r=mysql_fetch_row(res))!=NULL){
        if(!first){ jw_puts(&w,","); } first=0;
        jw_puts(&w,"{\"code\":");jw_string(&w,r[0]);jw_puts(&w,",\"name\":");jw_string(&w,r[1]);
        jw_printf(&w,",\"quantity\":%u,\"unit_price_minor\":%lld,\"total_minor\":%lld,\"currency\":",r[2]?(unsigned int)strtoul(r[2],NULL,10):0U,r[3]?strtoll(r[3],NULL,10):0LL,r[4]?strtoll(r[4],NULL,10):0LL);jw_string(&w,r[5]);jw_puts(&w,"}");
    }
    mysql_free_result(res);
    jw_puts(&w,"]}");
    return jw_result(ctx,&w);
}



static int scalar_ull(fvs_ctx *ctx, unsigned long long *out, const char *sql) {
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res, "%s", sql);
    if (rc != FVS_OK) return rc;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); seterr(ctx, "scalar query returned no row"); return FVS_ERR_INTERNAL; }
    *out = row[0] ? strtoull(row[0], NULL, 10) : 0ULL;
    mysql_free_result(res);
    return FVS_OK;
}

int fvs_schema_status(fvs_ctx *ctx, const char *required_migration, char *out, size_t out_size) {
    if (!ctx || !out || (required_migration && strlen(required_migration) > 128U)) return FVS_ERR_INVALID;
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx, &res,
        "SELECT COUNT(*),COALESCE(SUM(dirty),0),COALESCE(MAX(version),'') FROM schema_migrations");
    if (rc != FVS_OK) return rc;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); seterr(ctx, "schema_migrations unavailable"); return FVS_ERR_STATE; }
    unsigned long long count = row[0] ? strtoull(row[0], NULL, 10) : 0ULL;
    unsigned long long dirty = row[1] ? strtoull(row[1], NULL, 10) : 0ULL;
    char latest[129]; (void)snprintf(latest, sizeof latest, "%s", row[2] ? row[2] : "");
    mysql_free_result(res);
    unsigned long long required_present = required_migration && *required_migration ? 0ULL : 1ULL;
    if (required_migration && *required_migration) {
        char *er = sql_escape(ctx, required_migration);
        if (!er) return FVS_ERR_INTERNAL;
        char *sql = NULL;
        rc = sqlf(ctx, &sql, "SELECT COUNT(*) FROM schema_migrations WHERE version='%s' AND dirty=0", er);
        free(er);
        if (rc != FVS_OK) return rc;
        rc = scalar_ull(ctx, &required_present, sql);
        free(sql);
        if (rc != FVS_OK) return rc;
    }
    int ready = count > 0ULL && dirty == 0ULL && required_present == 1ULL;
    jsonw w; jw_init(&w, out, out_size);
    jw_printf(&w, "{\"ready\":%s,\"migration_count\":%llu,\"dirty_count\":%llu,\"latest\":", ready ? "true" : "false", count, dirty);
    jw_string(&w, latest);
    jw_puts(&w, ",\"required\":"); jw_string(&w, required_migration ? required_migration : "");
    jw_puts(&w, "}");
    return jw_result(ctx, &w);
}

int fvs_worker_heartbeat(fvs_ctx *ctx, const char *worker_name, const char *instance_id, const char *worker_version) {
    if (!ctx || !nonempty_max(worker_name,64U) || !nonempty_max(instance_id,128U) || !nonempty_max(worker_version,64U)) return FVS_ERR_INVALID;
    char *ew = sql_escape(ctx,worker_name), *ei = sql_escape(ctx,instance_id), *ev = sql_escape(ctx,worker_version);
    if (!ew || !ei || !ev) { free(ew); free(ei); free(ev); return FVS_ERR_INTERNAL; }
    char *sql = NULL;
    int rc = sqlf(ctx,&sql,
        "INSERT INTO ops_worker_heartbeats(worker_name,instance_id,worker_version,started_at,last_seen_at,stopped_at) "
        "VALUES('%s','%s','%s',UTC_TIMESTAMP(6),UTC_TIMESTAMP(6),NULL) "
        "ON DUPLICATE KEY UPDATE worker_version=VALUES(worker_version),last_seen_at=UTC_TIMESTAMP(6),stopped_at=NULL", ew,ei,ev);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql); free(ew); free(ei); free(ev);
    return rc;
}

int fvs_worker_goodbye(fvs_ctx *ctx, const char *worker_name, const char *instance_id) {
    if (!ctx || !nonempty_max(worker_name,64U) || !nonempty_max(instance_id,128U)) return FVS_ERR_INVALID;
    char *ew = sql_escape(ctx,worker_name), *ei = sql_escape(ctx,instance_id);
    if (!ew || !ei) { free(ew); free(ei); return FVS_ERR_INTERNAL; }
    char *sql = NULL;
    int rc = sqlf(ctx,&sql,
        "UPDATE ops_worker_heartbeats SET stopped_at=UTC_TIMESTAMP(6),last_seen_at=UTC_TIMESTAMP(6) "
        "WHERE worker_name='%s' AND instance_id='%s' AND stopped_at IS NULL", ew,ei);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql); free(ew); free(ei);
    return rc;
}

int fvs_ops_snapshot(fvs_ctx *ctx, unsigned int stale_worker_seconds, char *out, size_t out_size) {
    if (!ctx || !out || stale_worker_seconds < 10U || stale_worker_seconds > 86400U) return FVS_ERR_INVALID;
    unsigned long long cart_review=0ULL, payment_review=0ULL, dead=0ULL, retry_due=0ULL, processing_stale=0ULL;
    unsigned long long webhook_stale=0ULL, holds_5m=0ULL, workers=0ULL, workers_stale=0ULL;
    int rc = scalar_ull(ctx,&cart_review,"SELECT COUNT(*) FROM carts WHERE status='manual_review'"); if (rc != FVS_OK) return rc;
    rc = scalar_ull(ctx,&payment_review,"SELECT COUNT(*) FROM payment_attempts WHERE status='manual_review'"); if (rc != FVS_OK) return rc;
    rc = scalar_ull(ctx,&dead,"SELECT COUNT(*) FROM outbox_events WHERE status='dead'"); if (rc != FVS_OK) return rc;
    rc = scalar_ull(ctx,&retry_due,"SELECT COUNT(*) FROM outbox_events WHERE status='retry' AND next_attempt_at<=UTC_TIMESTAMP()"); if (rc != FVS_OK) return rc;
    rc = scalar_ull(ctx,&processing_stale,"SELECT COUNT(*) FROM outbox_events WHERE status='processing' AND lease_until<=UTC_TIMESTAMP()"); if (rc != FVS_OK) return rc;
    rc = scalar_ull(ctx,&webhook_stale,"SELECT COUNT(*) FROM webhook_events WHERE processed_at IS NULL AND lease_until IS NOT NULL AND lease_until<=UTC_TIMESTAMP()"); if (rc != FVS_OK) return rc;
    rc = scalar_ull(ctx,&holds_5m,"SELECT COUNT(*) FROM inventory_slots WHERE is_booked=0 AND held_by_cart_id IS NOT NULL AND hold_expires_at>UTC_TIMESTAMP() AND hold_expires_at<=TIMESTAMPADD(MINUTE,5,UTC_TIMESTAMP())"); if (rc != FVS_OK) return rc;
    rc = scalar_ull(ctx,&workers,"SELECT COUNT(*) FROM ops_worker_heartbeats WHERE stopped_at IS NULL"); if (rc != FVS_OK) return rc;
    char sql[256];
    int n = snprintf(sql,sizeof sql,"SELECT COUNT(*) FROM ops_worker_heartbeats WHERE stopped_at IS NULL AND last_seen_at<TIMESTAMPADD(SECOND,-%u,UTC_TIMESTAMP())",stale_worker_seconds);
    if (n < 0 || (size_t)n >= sizeof sql) return FVS_ERR_INTERNAL;
    rc = scalar_ull(ctx,&workers_stale,sql); if (rc != FVS_OK) return rc;
    const char *level = (dead > 0ULL || cart_review > 0ULL || payment_review > 0ULL || workers_stale > 0ULL) ? "degraded" : "ok";
    jsonw w; jw_init(&w,out,out_size);
    jw_puts(&w,"{\"status\":"); jw_string(&w,level);
    jw_printf(&w,",\"manual_review_carts\":%llu,\"manual_review_payments\":%llu,\"outbox_dead\":%llu,\"outbox_retry_due\":%llu,\"outbox_processing_stale\":%llu,\"webhook_stale\":%llu,\"holds_expiring_5m\":%llu,\"workers\":%llu,\"workers_stale\":%llu}",cart_review,payment_review,dead,retry_due,processing_stale,webhook_stale,holds_5m,workers,workers_stale);
    return jw_result(ctx,&w);
}

int fvs_manual_review_list(fvs_ctx *ctx, unsigned int limit, char *out, size_t out_size) {
    if (!ctx || !out || limit == 0U || limit > 500U) return FVS_ERR_INVALID;
    MYSQL_RES *res = NULL;
    int rc = queryf(ctx,&res,
        "SELECT id,cart_id,provider,COALESCE(provider_payment_id,''),amount_minor,currency,email,COALESCE(last_error,''),"
        "DATE_FORMAT(updated_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') "
        "FROM payment_attempts WHERE status='manual_review' ORDER BY updated_at DESC LIMIT %u", limit);
    if (rc != FVS_OK) return rc;
    jsonw w; jw_init(&w,out,out_size); jw_puts(&w,"{\"items\":[");
    MYSQL_ROW row; int first=1;
    while ((row=mysql_fetch_row(res))) {
        if (!first) jw_puts(&w,",");
        first = 0;
        jw_puts(&w,"{\"attempt_id\":"); jw_string(&w,row[0]);
        jw_puts(&w,",\"cart_id\":"); jw_string(&w,row[1]);
        jw_puts(&w,",\"provider\":"); jw_string(&w,row[2]);
        jw_puts(&w,",\"provider_payment_id\":"); jw_string(&w,row[3]);
        jw_printf(&w,",\"amount_minor\":%llu,\"currency\":",row[4]?strtoull(row[4],NULL,10):0ULL); jw_string(&w,row[5]);
        jw_puts(&w,",\"email\":"); jw_string(&w,row[6]);
        jw_puts(&w,",\"last_error\":"); jw_string(&w,row[7]);
        jw_puts(&w,",\"updated_at\":"); jw_string(&w,row[8]); jw_puts(&w,"}");
    }
    mysql_free_result(res); jw_puts(&w,"]}"); return jw_result(ctx,&w);
}

int fvs_ops_action_record(fvs_ctx *ctx, const char *actor, const char *action_type, const char *target_id) {
    if (!ctx || !nonempty_max(actor,128U) || !nonempty_max(action_type,80U) || !nonempty_max(target_id,191U)) return FVS_ERR_INVALID;
    char *ea=sql_escape(ctx,actor), *et=sql_escape(ctx,action_type), *ei=sql_escape(ctx,target_id);
    if (!ea || !et || !ei) { free(ea); free(et); free(ei); return FVS_ERR_INTERNAL; }
    char id[37]; int rc=fvs_uuid_v4(id); char *sql=NULL;
    if (rc==FVS_OK) rc=sqlf(ctx,&sql,"INSERT INTO ops_actions(id,actor,action_type,target_id,created_at) VALUES('%s','%s','%s','%s',UTC_TIMESTAMP(6))",id,ea,et,ei);
    if (rc==FVS_OK) rc=exec_sql(ctx,sql);
    free(sql); free(ea); free(et); free(ei); return rc;
}

int fvs_outbox_requeue_dead(fvs_ctx *ctx, const char *event_id, const char *actor) {
    if (!ctx || !valid_id(event_id) || !nonempty_max(actor,128U)) return FVS_ERR_INVALID;
    char *ea = sql_escape(ctx,actor);
    if (!ea) return FVS_ERR_INTERNAL;
    int rc = tx_begin(ctx);
    if (rc != FVS_OK) { free(ea); return rc; }
    MYSQL_RES *res = NULL;
    rc = queryf(ctx,&res,"SELECT status FROM outbox_events WHERE id='%s' FOR UPDATE",event_id);
    if (rc != FVS_OK) { tx_rollback(ctx); free(ea); return rc; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); tx_rollback(ctx); free(ea); return FVS_ERR_NOT_FOUND; }
    int is_dead = row[0] && strcmp(row[0],"dead") == 0;
    mysql_free_result(res);
    if (!is_dead) { tx_rollback(ctx); free(ea); seterr(ctx,"outbox event is not dead"); return FVS_ERR_STATE; }
    char *sql = NULL;
    rc = sqlf(ctx,&sql,"UPDATE outbox_events SET status='retry',attempts=0,next_attempt_at=UTC_TIMESTAMP(),lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=NULL,updated_at=UTC_TIMESTAMP() WHERE id='%s' AND status='dead'",event_id);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql);
    if (rc != FVS_OK) { tx_rollback(ctx); free(ea); return rc; }
    char action_id[37]; rc = fvs_uuid_v4(action_id);
    if (rc != FVS_OK) { tx_rollback(ctx); free(ea); return rc; }
    rc = sqlf(ctx,&sql,"INSERT INTO ops_actions(id,actor,action_type,target_id,created_at) VALUES('%s','%s','outbox.requeue','%s',UTC_TIMESTAMP(6))",action_id,ea,event_id);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql); free(ea);
    if (rc != FVS_OK) { tx_rollback(ctx); return rc; }
    return tx_commit(ctx);
}

/* ---- Enterprise support, growth and SEO extensions (4.x) ---------------- */
static int support_priority_valid(const char *s){return s && (!strcmp(s,"low")||!strcmp(s,"normal")||!strcmp(s,"high")||!strcmp(s,"urgent"));}
static int support_status_valid(const char *s){return s && (!strcmp(s,"offline")||!strcmp(s,"available")||!strcmp(s,"busy")||!strcmp(s,"away"));}
static int marketing_objective_valid(const char *s){return s && (!strcmp(s,"awareness")||!strcmp(s,"traffic")||!strcmp(s,"leads")||!strcmp(s,"bookings")||!strcmp(s,"revenue")||!strcmp(s,"retargeting"));}
static int marketing_mode_valid(const char *s){return s && (!strcmp(s,"manual")||!strcmp(s,"assist")||!strcmp(s,"guarded"));}
static int marketing_channel_valid(const char *s){return s && (!strcmp(s,"meta")||!strcmp(s,"instagram")||!strcmp(s,"linkedin")||!strcmp(s,"tiktok")||!strcmp(s,"x")||!strcmp(s,"google")||!strcmp(s,"email")||!strcmp(s,"webhook")||!strcmp(s,"pinterest")||!strcmp(s,"whatsapp")||!strcmp(s,"youtube"));}
static int marketing_action_valid(const char *s){return s && (!strcmp(s,"publish")||!strcmp(s,"pause")||!strcmp(s,"resume")||!strcmp(s,"sync_metrics")||!strcmp(s,"update_budget")||!strcmp(s,"sync_catalog")||!strcmp(s,"send_conversion")||!strcmp(s,"recover_abandonment")||!strcmp(s,"publish_collection"));}
static int marketing_event_valid(const char *s){return s && (!strcmp(s,"impression")||!strcmp(s,"click")||!strcmp(s,"landing")||!strcmp(s,"lead")||!strcmp(s,"cart")||!strcmp(s,"checkout")||!strcmp(s,"booking")||!strcmp(s,"revenue"));}
static int seo_page_type_valid(const char *s){return s && (!strcmp(s,"property")||!strcmp(s,"destination")||!strcmp(s,"collection")||!strcmp(s,"editorial"));}

static int support_render(fvs_ctx *ctx,const char *thread_id,const char *secret_hash,int trusted,char *out,size_t out_size){
    MYSQL_RES *res=NULL; int rc;
    if(trusted) rc=queryf(ctx,&res,"SELECT id,COALESCE(customer_email,''),subject,status,priority,channel,COALESCE(assigned_agent_id,''),ai_enabled,COALESCE(handoff_reason,''),DATE_FORMAT(last_message_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),COALESCE(cart_id,''),COALESCE(order_id,'') FROM support_threads WHERE id='%s'",thread_id);
    else rc=queryf(ctx,&res,"SELECT id,COALESCE(customer_email,''),subject,status,priority,channel,COALESCE(assigned_agent_id,''),ai_enabled,COALESCE(handoff_reason,''),DATE_FORMAT(last_message_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),COALESCE(cart_id,''),COALESCE(order_id,'') FROM support_threads WHERE id='%s' AND secret_hash=UNHEX('%s')",thread_id,secret_hash);
    if (rc != FVS_OK) return rc;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        seterr(ctx, "support thread not found");
        return trusted ? FVS_ERR_NOT_FOUND : FVS_ERR_AUTH;
    }
    char email[255],subject[241],status[32],priority[16],channel[16],agent[65],reason[501],last[32],cart[37],order[37]; int ai=row[7]?atoi(row[7]):0;
    (void)snprintf(email,sizeof email,"%s",row[1]);(void)snprintf(subject,sizeof subject,"%s",row[2]);(void)snprintf(status,sizeof status,"%s",row[3]);(void)snprintf(priority,sizeof priority,"%s",row[4]);(void)snprintf(channel,sizeof channel,"%s",row[5]);(void)snprintf(agent,sizeof agent,"%s",row[6]);(void)snprintf(reason,sizeof reason,"%s",row[8]);(void)snprintf(last,sizeof last,"%s",row[9]);(void)snprintf(cart,sizeof cart,"%s",row[10]);(void)snprintf(order,sizeof order,"%s",row[11]);mysql_free_result(res);
    rc=queryf(ctx,&res,"SELECT id,sender_type,COALESCE(sender_id,''),body,visibility,COALESCE(model_name,''),DATE_FORMAT(created_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') FROM support_messages WHERE thread_id='%s' ORDER BY created_at,id LIMIT 250",thread_id); if(rc!=FVS_OK)return rc;
    jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"thread_id\":");jw_string(&w,thread_id);jw_puts(&w,",\"email\":");jw_string(&w,email);jw_puts(&w,",\"subject\":");jw_string(&w,subject);jw_puts(&w,",\"status\":");jw_string(&w,status);jw_puts(&w,",\"priority\":");jw_string(&w,priority);jw_puts(&w,",\"channel\":");jw_string(&w,channel);jw_puts(&w,",\"assigned_agent_id\":");agent[0]?jw_string(&w,agent):jw_puts(&w,"null");jw_printf(&w,",\"ai_enabled\":%s",ai?"true":"false");jw_puts(&w,",\"handoff_reason\":");reason[0]?jw_string(&w,reason):jw_puts(&w,"null");jw_puts(&w,",\"last_message_at\":");jw_string(&w,last);jw_puts(&w,",\"cart_id\":");cart[0]?jw_string(&w,cart):jw_puts(&w,"null");jw_puts(&w,",\"order_id\":");order[0]?jw_string(&w,order):jw_puts(&w,"null");jw_puts(&w,",\"messages\":[");int first=1;MYSQL_ROW m;while((m=mysql_fetch_row(res))!=NULL){if(!first)jw_puts(&w,",");first=0;jw_puts(&w,"{\"id\":");jw_string(&w,m[0]);jw_puts(&w,",\"sender_type\":");jw_string(&w,m[1]);jw_puts(&w,",\"sender_id\":");m[2]&&*m[2]?jw_string(&w,m[2]):jw_puts(&w,"null");jw_puts(&w,",\"body\":");jw_string(&w,m[3]);jw_puts(&w,",\"visibility\":");jw_string(&w,m[4]);jw_puts(&w,",\"model_name\":");m[5]&&*m[5]?jw_string(&w,m[5]):jw_puts(&w,"null");jw_puts(&w,",\"created_at\":");jw_string(&w,m[6]);jw_puts(&w,"}");}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);
}

int fvs_support_thread_create(fvs_ctx *ctx,const char *email,const char *subject,const char *cart_id,const char *order_id,const char *priority,char *out_thread_id,size_t thread_id_size,char *out_secret,size_t secret_size){
    if (!ctx || !nonempty_max(subject,240U) || !support_priority_valid(priority) ||
        !out_thread_id || thread_id_size < 37U || !out_secret || secret_size < 65U) return FVS_ERR_INVALID;
    if (email && *email && !fvs_email_valid(email)) return FVS_ERR_INVALID;
    if (cart_id && *cart_id && !valid_id(cart_id)) return FVS_ERR_INVALID;
    if (order_id && *order_id && !valid_id(order_id)) return FVS_ERR_INVALID;
    char id[37],secret[65],hash[65];if(fvs_uuid_v4(id)!=FVS_OK||fvs_random_hex(secret,sizeof secret,32U)!=FVS_OK||fvs_sha256_hex(secret,hash)!=FVS_OK)return FVS_ERR_INTERNAL;char *ee=sql_escape(ctx,email?email:"");char *es=sql_escape(ctx,subject);if(!ee||!es){free(ee);free(es);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO support_threads(id,secret_hash,cart_id,order_id,customer_email,subject,status,priority,channel,ai_enabled,first_response_due_at,resolution_due_at,last_message_at,created_at,updated_at) VALUES('%s',UNHEX('%s'),%s%s%s,%s%s%s,%s%s%s,'%s','open','%s','web',1,TIMESTAMPADD(MINUTE,5,UTC_TIMESTAMP(6)),TIMESTAMPADD(HOUR,24,UTC_TIMESTAMP(6)),UTC_TIMESTAMP(6),UTC_TIMESTAMP(6),UTC_TIMESTAMP(6))",id,hash,cart_id&&*cart_id?"'":"",cart_id&&*cart_id?cart_id:"NULL",cart_id&&*cart_id?"'":"",order_id&&*order_id?"'":"",order_id&&*order_id?order_id:"NULL",order_id&&*order_id?"'":"",*ee?"'":"",*ee?ee:"NULL",*ee?"'":"",es,priority);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(ee);free(es);if(rc!=FVS_OK)return rc;(void)snprintf(out_thread_id,thread_id_size,"%s",id);(void)snprintf(out_secret,secret_size,"%s",secret);return FVS_OK;
}
int fvs_support_thread_get(fvs_ctx *ctx,const char *thread_id,const char *secret,char *out,size_t out_size){if(!ctx||!valid_id(thread_id)||!secret||!out)return FVS_ERR_INVALID;char hash[65];int rc=auth_hash(secret,hash);if(rc!=FVS_OK)return rc;return support_render(ctx,thread_id,hash,0,out,out_size);}
int fvs_support_thread_context(fvs_ctx *ctx,const char *thread_id,char *out,size_t out_size){if(!ctx||!valid_id(thread_id)||!out)return FVS_ERR_INVALID;return support_render(ctx,thread_id,NULL,1,out,out_size);}

int fvs_support_customer_message(fvs_ctx *ctx,const char *thread_id,const char *secret,const char *body,char *out,size_t out_size){
    if (!ctx || !valid_id(thread_id) || !secret || !nonempty_max(body,12000U) || !out) return FVS_ERR_INVALID;
    char hash[65];
    int rc = auth_hash(secret,hash);
    if (rc != FVS_OK) return rc;
    char *eb = sql_escape(ctx,body);
    if (!eb) return FVS_ERR_INTERNAL;
    rc = tx_begin(ctx);
    if (rc != FVS_OK) { free(eb); return rc; }
    MYSQL_RES *res = NULL;
    rc = queryf(ctx,&res,"SELECT status,ai_enabled FROM support_threads WHERE id='%s' AND secret_hash=UNHEX('%s') FOR UPDATE",thread_id,hash);
    if (rc != FVS_OK) { tx_rollback(ctx); free(eb); return rc; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); tx_rollback(ctx); free(eb); seterr(ctx,"support thread not found"); return FVS_ERR_AUTH; }
    if (row[0] && !strcmp(row[0],"closed")) { mysql_free_result(res); tx_rollback(ctx); free(eb); seterr(ctx,"support thread closed"); return FVS_ERR_STATE; }
    int ai = row[1] ? atoi(row[1]) : 0;
    mysql_free_result(res);
    char mid[37], eid[37];
    if (fvs_uuid_v4(mid) != FVS_OK || fvs_uuid_v4(eid) != FVS_OK) { tx_rollback(ctx); free(eb); return FVS_ERR_INTERNAL; }
    char *sql = NULL;
    rc = sqlf(ctx,&sql,"INSERT INTO support_messages(id,thread_id,sender_type,body,visibility,created_at) VALUES('%s','%s','customer','%s','public',UTC_TIMESTAMP(6))",mid,thread_id,eb);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql); sql = NULL;
    if (rc == FVS_OK) {
        rc = sqlf(ctx,&sql,"UPDATE support_threads SET status=IF(status='waiting_customer','in_progress',status),last_message_at=UTC_TIMESTAMP(6),updated_at=UTC_TIMESTAMP(6) WHERE id='%s'",thread_id);
        if (rc == FVS_OK) rc = exec_sql(ctx,sql);
        free(sql); sql = NULL;
    }
    if (rc == FVS_OK && ai) {
        rc = sqlf(ctx,&sql,"INSERT INTO outbox_events(id,event_type,aggregate_id,payload_json,status,attempts,next_attempt_at,created_at,updated_at) VALUES('%s','support.ai_requested','%s',JSON_OBJECT('thread_id','%s'),'pending',0,UTC_TIMESTAMP(6),UTC_TIMESTAMP(6),UTC_TIMESTAMP(6))",eid,thread_id,thread_id);
        if (rc == FVS_OK) rc = exec_sql(ctx,sql);
        free(sql);
    }
    free(eb);
    if (rc != FVS_OK) { tx_rollback(ctx); return rc; }
    rc = tx_commit(ctx);
    if (rc != FVS_OK) return rc;
    return support_render(ctx,thread_id,hash,0,out,out_size);
}

int fvs_support_ai_message(fvs_ctx *ctx,const char *thread_id,const char *body,const char *model,unsigned int input_tokens,unsigned int output_tokens,char *out,size_t out_size){
    if (!ctx || !valid_id(thread_id) || !nonempty_max(body,12000U) || !optional_max(model,128U) || !out) return FVS_ERR_INVALID;
    char *eb = sql_escape(ctx,body);
    char *em = sql_escape(ctx,model ? model : "");
    if (!eb || !em) { free(eb); free(em); return FVS_ERR_INTERNAL; }
    int rc = tx_begin(ctx);
    if (rc != FVS_OK) { free(eb); free(em); return rc; }
    MYSQL_RES *res = NULL;
    rc = queryf(ctx,&res,"SELECT ai_enabled,status FROM support_threads WHERE id='%s' FOR UPDATE",thread_id);
    if (rc != FVS_OK) { tx_rollback(ctx); free(eb); free(em); return rc; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); tx_rollback(ctx); free(eb); free(em); return FVS_ERR_NOT_FOUND; }
    int allowed = row[0] && atoi(row[0]) == 1 && row[1] && strcmp(row[1],"waiting_human") && strcmp(row[1],"in_progress") && strcmp(row[1],"closed");
    mysql_free_result(res);
    if (!allowed) { tx_rollback(ctx); free(eb); free(em); seterr(ctx,"AI support disabled or human owns thread"); return FVS_ERR_CONFLICT; }
    char mid[37];
    if (fvs_uuid_v4(mid) != FVS_OK) { tx_rollback(ctx); free(eb); free(em); return FVS_ERR_INTERNAL; }
    char *sql = NULL;
    rc = sqlf(ctx,&sql,"INSERT INTO support_messages(id,thread_id,sender_type,sender_id,body,visibility,model_name,input_tokens,output_tokens,created_at) VALUES('%s','%s','ai','support-ai','%s','public',%s%s%s,%u,%u,UTC_TIMESTAMP(6))",mid,thread_id,eb,*em?"'":"",*em?em:"NULL",*em?"'":"",input_tokens,output_tokens);
    if (rc == FVS_OK) rc = exec_sql(ctx,sql);
    free(sql); sql = NULL;
    if (rc == FVS_OK) {
        rc = sqlf(ctx,&sql,"UPDATE support_threads SET status='waiting_customer',last_message_at=UTC_TIMESTAMP(6),updated_at=UTC_TIMESTAMP(6) WHERE id='%s'",thread_id);
        if (rc == FVS_OK) rc = exec_sql(ctx,sql);
        free(sql);
    }
    free(eb); free(em);
    if (rc != FVS_OK) { tx_rollback(ctx); return rc; }
    rc = tx_commit(ctx);
    if (rc != FVS_OK) return rc;
    return support_render(ctx,thread_id,NULL,1,out,out_size);
}
int fvs_support_handoff(fvs_ctx *ctx,const char *thread_id,const char *reason){if(!ctx||!valid_id(thread_id)||!nonempty_max(reason,500U))return FVS_ERR_INVALID;char *er=sql_escape(ctx,reason);if(!er)return FVS_ERR_INTERNAL;char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE support_threads SET status='waiting_human',ai_enabled=0,handoff_reason='%s',updated_at=UTC_TIMESTAMP(6) WHERE id='%s' AND status NOT IN('resolved','closed')",er,thread_id);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(er);if(rc!=FVS_OK)return rc;if(mysql_affected_rows(ctx->db)==0ULL)return FVS_ERR_NOT_FOUND;return FVS_OK;}
int fvs_support_agent_heartbeat(fvs_ctx *ctx,const char *agent_id,const char *name,const char *status,unsigned int max_active){if(!ctx||!nonempty_max(agent_id,64U)||!nonempty_max(name,160U)||!support_status_valid(status)||max_active<1U||max_active>100U)return FVS_ERR_INVALID;char *ei=sql_escape(ctx,agent_id),*en=sql_escape(ctx,name);if(!ei||!en){free(ei);free(en);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO support_agents(id,display_name,status,max_active,last_seen_at,created_at,updated_at) VALUES('%s','%s','%s',%u,UTC_TIMESTAMP(6),UTC_TIMESTAMP(6),UTC_TIMESTAMP(6)) ON DUPLICATE KEY UPDATE display_name=VALUES(display_name),status=VALUES(status),max_active=VALUES(max_active),last_seen_at=UTC_TIMESTAMP(6),updated_at=UTC_TIMESTAMP(6)",ei,en,status,max_active);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(ei);free(en);return rc;}
int fvs_support_queue_list(fvs_ctx *ctx,unsigned int limit,char *out,size_t out_size){if(!ctx||!out||limit<1U||limit>500U)return FVS_ERR_INVALID;MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT id,COALESCE(customer_email,''),subject,status,priority,COALESCE(assigned_agent_id,''),DATE_FORMAT(last_message_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),GREATEST(0,TIMESTAMPDIFF(SECOND,UTC_TIMESTAMP(6),first_response_due_at)) FROM support_threads WHERE status IN('open','waiting_human','in_progress','waiting_customer') ORDER BY FIELD(priority,'urgent','high','normal','low'),last_message_at LIMIT %u",limit);if(rc!=FVS_OK)return rc;jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"threads\":[");int first=1;MYSQL_ROW r;while((r=mysql_fetch_row(res))!=NULL){if(!first)jw_puts(&w,",");first=0;jw_puts(&w,"{\"thread_id\":");jw_string(&w,r[0]);jw_puts(&w,",\"email\":");jw_string(&w,r[1]);jw_puts(&w,",\"subject\":");jw_string(&w,r[2]);jw_puts(&w,",\"status\":");jw_string(&w,r[3]);jw_puts(&w,",\"priority\":");jw_string(&w,r[4]);jw_puts(&w,",\"assigned_agent_id\":");r[5]&&*r[5]?jw_string(&w,r[5]):jw_puts(&w,"null");jw_puts(&w,",\"last_message_at\":");jw_string(&w,r[6]);jw_printf(&w,",\"first_response_seconds_remaining\":%lld}",r[7]?strtoll(r[7],NULL,10):0LL);}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);}
int fvs_support_agent_claim(fvs_ctx *ctx,const char *thread_id,const char *agent_id){if(!ctx||!valid_id(thread_id)||!nonempty_max(agent_id,64U))return FVS_ERR_INVALID;char *ea=sql_escape(ctx,agent_id);if(!ea)return FVS_ERR_INTERNAL;char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE support_threads SET assigned_agent_id='%s',status='in_progress',ai_enabled=0,updated_at=UTC_TIMESTAMP(6) WHERE id='%s' AND status IN('open','waiting_human','waiting_customer','in_progress') AND (assigned_agent_id IS NULL OR assigned_agent_id='%s')",ea,thread_id,ea);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);if(rc==FVS_OK&&mysql_affected_rows(ctx->db)==0ULL){seterr(ctx,"support thread already assigned or unavailable");rc=FVS_ERR_CONFLICT;}free(ea);return rc;}
int fvs_support_agent_message(fvs_ctx *ctx,const char *thread_id,const char *agent_id,const char *body,int internal_only,char *out,size_t out_size){if(!ctx||!valid_id(thread_id)||!nonempty_max(agent_id,64U)||!nonempty_max(body,12000U)||!out)return FVS_ERR_INVALID;char *ea=sql_escape(ctx,agent_id),*eb=sql_escape(ctx,body);if(!ea||!eb){free(ea);free(eb);return FVS_ERR_INTERNAL;}char mid[37];if(fvs_uuid_v4(mid)!=FVS_OK){free(ea);free(eb);return FVS_ERR_INTERNAL;}int rc=tx_begin(ctx);if(rc!=FVS_OK){free(ea);free(eb);return rc;}char *sql=NULL;rc=sqlf(ctx,&sql,"INSERT INTO support_messages(id,thread_id,sender_type,sender_id,body,visibility,created_at) SELECT '%s',id,'agent','%s','%s','%s',UTC_TIMESTAMP(6) FROM support_threads WHERE id='%s' AND assigned_agent_id='%s' AND status='in_progress'",mid,ea,eb,internal_only?"internal":"public",thread_id,ea);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);if(rc==FVS_OK&&mysql_affected_rows(ctx->db)==0ULL){tx_rollback(ctx);free(ea);free(eb);seterr(ctx,"agent does not own thread");return FVS_ERR_AUTH;}if(rc==FVS_OK&&!internal_only){rc=sqlf(ctx,&sql,"UPDATE support_threads SET status='waiting_customer',last_message_at=UTC_TIMESTAMP(6),updated_at=UTC_TIMESTAMP(6) WHERE id='%s' AND assigned_agent_id='%s'",thread_id,ea);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);}free(ea);free(eb);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}rc=tx_commit(ctx);if(rc!=FVS_OK)return rc;return support_render(ctx,thread_id,NULL,1,out,out_size);}
int fvs_support_thread_resolve(fvs_ctx *ctx,const char *thread_id,const char *agent_id){if(!ctx||!valid_id(thread_id)||!nonempty_max(agent_id,64U))return FVS_ERR_INVALID;char *ea=sql_escape(ctx,agent_id);if(!ea)return FVS_ERR_INTERNAL;char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE support_threads SET status='resolved',resolved_at=UTC_TIMESTAMP(6),updated_at=UTC_TIMESTAMP(6) WHERE id='%s' AND assigned_agent_id='%s'",thread_id,ea);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(ea);if(rc==FVS_OK&&mysql_affected_rows(ctx->db)==0ULL)return FVS_ERR_AUTH;return rc;}

int fvs_marketing_campaign_create(fvs_ctx *ctx,const char *name,const char *objective,const char *mode,int64_t budget,int64_t daily,const char *currency,const char *utm,const char *actor,char *out,size_t out_size){if(!ctx||!nonempty_max(name,180U)||!marketing_objective_valid(objective)||!marketing_mode_valid(mode)||budget<0||daily<0||!fvs_currency_valid(currency)||!nonempty_max(utm,160U)||!nonempty_max(actor,128U)||!out)return FVS_ERR_INVALID;char id[37];if(fvs_uuid_v4(id)!=FVS_OK)return FVS_ERR_INTERNAL;char *en=sql_escape(ctx,name),*eu=sql_escape(ctx,utm),*ea=sql_escape(ctx,actor);if(!en||!eu||!ea){free(en);free(eu);free(ea);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO marketing_campaigns(id,name,objective,status,automation_mode,budget_minor,daily_budget_minor,max_daily_spend_minor,currency,utm_campaign,approval_required,created_by,created_at,updated_at) VALUES('%s','%s','%s','draft','%s',%lld,%lld,%lld,'%s','%s',1,'%s',UTC_TIMESTAMP(),UTC_TIMESTAMP())",id,en,objective,mode,(long long)budget,(long long)daily,(long long)daily,currency,eu,ea);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(en);free(eu);free(ea);if(rc!=FVS_OK)return rc;jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"campaign_id\":");jw_string(&w,id);jw_puts(&w,",\"status\":\"draft\"}");return jw_result(ctx,&w);}
int fvs_marketing_campaign_list(fvs_ctx *ctx,unsigned int limit,char *out,size_t out_size){if(!ctx||!out||limit<1U||limit>500U)return FVS_ERR_INVALID;MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT id,name,objective,status,automation_mode,budget_minor,daily_budget_minor,currency,utm_campaign,COALESCE(approved_by,''),DATE_FORMAT(created_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') FROM marketing_campaigns ORDER BY created_at DESC LIMIT %u",limit);if(rc!=FVS_OK)return rc;jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"campaigns\":[");int first=1;MYSQL_ROW r;while((r=mysql_fetch_row(res))!=NULL){if(!first)jw_puts(&w,",");first=0;jw_puts(&w,"{\"id\":");jw_string(&w,r[0]);jw_puts(&w,",\"name\":");jw_string(&w,r[1]);jw_puts(&w,",\"objective\":");jw_string(&w,r[2]);jw_puts(&w,",\"status\":");jw_string(&w,r[3]);jw_puts(&w,",\"automation_mode\":");jw_string(&w,r[4]);jw_printf(&w,",\"budget_minor\":%lld,\"daily_budget_minor\":%lld,\"currency\":",r[5]?strtoll(r[5],NULL,10):0LL,r[6]?strtoll(r[6],NULL,10):0LL);jw_string(&w,r[7]);jw_puts(&w,",\"utm_campaign\":");jw_string(&w,r[8]);jw_puts(&w,",\"approved_by\":");r[9]&&*r[9]?jw_string(&w,r[9]):jw_puts(&w,"null");jw_puts(&w,",\"created_at\":");jw_string(&w,r[10]);jw_puts(&w,"}");}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);}
int fvs_marketing_campaign_approve(fvs_ctx *ctx,const char *campaign_id,const char *actor){if(!ctx||!valid_id(campaign_id)||!nonempty_max(actor,128U))return FVS_ERR_INVALID;char *ea=sql_escape(ctx,actor);if(!ea)return FVS_ERR_INTERNAL;char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE marketing_campaigns SET status='approved',approved_by='%s',approved_at=UTC_TIMESTAMP(),updated_at=UTC_TIMESTAMP() WHERE id='%s' AND status IN('draft','pending_approval','paused')",ea,campaign_id);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(ea);if(rc==FVS_OK&&mysql_affected_rows(ctx->db)==0ULL)return FVS_ERR_CONFLICT;return rc;}
int fvs_marketing_campaign_configure(fvs_ctx *ctx,
                                             const char *campaign_id,
                                             int64_t max_daily_spend_minor,
                                             unsigned int frequency_cap_7d,
                                             unsigned int target_roas_bps,
                                             int64_t stop_loss_minor,
                                             const char *audience_json,
                                             const char *geo_json,
                                             const char *placements_json,
                                             const char *optimization_rules_json,
                                             const char *experiment_json,
                                             const char *actor) {
    if (!ctx || !valid_id(campaign_id) || max_daily_spend_minor < 0 || frequency_cap_7d > 1000U ||
        target_roas_bps > 1000000U || stop_loss_minor < 0 || !optional_max(audience_json,30000U) ||
        !optional_max(geo_json,30000U) || !optional_max(placements_json,30000U) ||
        !optional_max(optimization_rules_json,30000U) || !optional_max(experiment_json,30000U) ||
        !nonempty_max(actor,128U)) return FVS_ERR_INVALID;
    const char *raw[5] = {audience_json,geo_json,placements_json,optimization_rules_json,experiment_json};
    char *e[5] = {0};
    for (int i=0;i<5;i++) {
        e[i] = sql_escape(ctx, raw[i] && *raw[i] ? raw[i] : "{}");
        if (!e[i]) { for (int j=0;j<=i;j++) free(e[j]); return FVS_ERR_INTERNAL; }
    }
    char *ea=sql_escape(ctx,actor);
    if(!ea){for(int i=0;i<5;i++)free(e[i]);return FVS_ERR_INTERNAL;}
    int rc=tx_begin(ctx);
    if(rc!=FVS_OK){for(int i=0;i<5;i++)free(e[i]);free(ea);return rc;}
    MYSQL_RES *res=NULL;
    rc=queryf(ctx,&res,"SELECT daily_budget_minor,status FROM marketing_campaigns WHERE id='%s' FOR UPDATE",campaign_id);
    if(rc!=FVS_OK){tx_rollback(ctx);for(int i=0;i<5;i++)free(e[i]);free(ea);return rc;}
    MYSQL_ROW row=mysql_fetch_row(res);
    if(!row){mysql_free_result(res);tx_rollback(ctx);for(int i=0;i<5;i++)free(e[i]);free(ea);return FVS_ERR_NOT_FOUND;}
    unsigned long long previous=row[0]?strtoull(row[0],NULL,10):0ULL;
    const char *status=row[1]?row[1]:"";
    if(!strcmp(status,"active")){mysql_free_result(res);tx_rollback(ctx);for(int i=0;i<5;i++)free(e[i]);free(ea);seterr(ctx,"pause active campaign before changing guardrails");return FVS_ERR_STATE;}
    mysql_free_result(res);
    char *sql=NULL;
    rc=sqlf(ctx,&sql,
        "UPDATE marketing_campaigns SET max_daily_spend_minor=%lld,frequency_cap_7d=%u,target_roas_bps=%u,stop_loss_minor=%lld,"
        "audience_json=CAST('%s' AS JSON),geo_json=CAST('%s' AS JSON),placements_json=CAST('%s' AS JSON),"
        "optimization_rules_json=CAST('%s' AS JSON),experiment_json=CAST('%s' AS JSON),updated_at=UTC_TIMESTAMP() WHERE id='%s'",
        (long long)max_daily_spend_minor,frequency_cap_7d,target_roas_bps,(long long)stop_loss_minor,e[0],e[1],e[2],e[3],e[4],campaign_id);
    if(rc==FVS_OK)rc=exec_sql(ctx,sql);
    free(sql);sql=NULL;
    if(rc==FVS_OK){
        char bid[37];
        if(fvs_uuid_v4(bid)!=FVS_OK)rc=FVS_ERR_INTERNAL;
        else {
            rc=sqlf(ctx,&sql,"INSERT INTO marketing_budget_events(id,campaign_id,actor,action,previous_daily_budget_minor,new_daily_budget_minor,detail_json,created_at) VALUES('%s','%s','%s','configure',%llu,%lld,JSON_OBJECT('max_daily_spend_minor',%lld,'frequency_cap_7d',%u,'target_roas_bps',%u,'stop_loss_minor',%lld),UTC_TIMESTAMP())",bid,campaign_id,ea,previous,previous,(long long)max_daily_spend_minor,frequency_cap_7d,target_roas_bps,(long long)stop_loss_minor);
            if(rc==FVS_OK)rc=exec_sql(ctx,sql);
            free(sql);
        }
    }
    for(int i=0;i<5;i++) { free(e[i]); }
    free(ea);
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;}
    return tx_commit(ctx);
}

int fvs_marketing_campaign_pause(fvs_ctx *ctx,const char *campaign_id,const char *actor){
    if(!ctx||!valid_id(campaign_id)||!nonempty_max(actor,128U))return FVS_ERR_INVALID;
    char *ea=sql_escape(ctx,actor);if(!ea)return FVS_ERR_INTERNAL;
    int rc=tx_begin(ctx);if(rc!=FVS_OK){free(ea);return rc;}
    MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT daily_budget_minor,status FROM marketing_campaigns WHERE id='%s' FOR UPDATE",campaign_id);
    if(rc!=FVS_OK){tx_rollback(ctx);free(ea);return rc;}
    MYSQL_ROW row=mysql_fetch_row(res);if(!row){mysql_free_result(res);tx_rollback(ctx);free(ea);return FVS_ERR_NOT_FOUND;}
    unsigned long long daily=row[0]?strtoull(row[0],NULL,10):0ULL;const char *status=row[1]?row[1]:"";
    if(!strcmp(status,"completed")||!strcmp(status,"cancelled")){mysql_free_result(res);tx_rollback(ctx);free(ea);return FVS_ERR_STATE;}
    mysql_free_result(res);
    char *sql=NULL;rc=sqlf(ctx,&sql,"UPDATE marketing_campaigns SET status='paused',updated_at=UTC_TIMESTAMP() WHERE id='%s'",campaign_id);
    if(rc==FVS_OK) { rc=exec_sql(ctx,sql); }
    free(sql);
    sql=NULL;
    if(rc==FVS_OK){char bid[37];if(fvs_uuid_v4(bid)!=FVS_OK)rc=FVS_ERR_INTERNAL;else{rc=sqlf(ctx,&sql,"INSERT INTO marketing_budget_events(id,campaign_id,actor,action,previous_daily_budget_minor,new_daily_budget_minor,detail_json,created_at) VALUES('%s','%s','%s','pause',%llu,%llu,JSON_OBJECT('reason','operator_pause'),UTC_TIMESTAMP())",bid,campaign_id,ea,daily,daily);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);}}
    free(ea);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}return tx_commit(ctx);
}

int fvs_marketing_creative_add(fvs_ctx *ctx,const char *campaign_id,const char *channel,const char *variant,const char *headline,const char *body,const char *cta,const char *landing,const char *image,int ai,const char *actor,char *out,size_t out_size){if(!ctx||!valid_id(campaign_id)||!marketing_channel_valid(channel)||!nonempty_max(variant,64U)||!nonempty_max(headline,255U)||!nonempty_max(body,12000U)||!optional_max(cta,80U)||!nonempty_max(landing,1500U)||!optional_max(image,1500U)||!nonempty_max(actor,128U)||!out)return FVS_ERR_INVALID;char id[37];if(fvs_uuid_v4(id)!=FVS_OK)return FVS_ERR_INTERNAL;char *ev=sql_escape(ctx,variant),*eh=sql_escape(ctx,headline),*eb=sql_escape(ctx,body),*ec=sql_escape(ctx,cta?cta:""),*el=sql_escape(ctx,landing),*ei=sql_escape(ctx,image?image:"");if(!ev||!eh||!eb||!ec||!el||!ei){free(ev);free(eh);free(eb);free(ec);free(el);free(ei);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO marketing_creatives(id,campaign_id,channel,variant_key,headline,body,cta,landing_url,image_url,status,ai_generated,created_at,updated_at) VALUES('%s','%s','%s','%s','%s','%s',%s%s%s,'%s',%s%s%s,'pending_approval',%d,UTC_TIMESTAMP(),UTC_TIMESTAMP())",id,campaign_id,channel,ev,eh,eb,*ec?"'":"",*ec?ec:"NULL",*ec?"'":"",el,*ei?"'":"",*ei?ei:"NULL",*ei?"'":"",ai?1:0);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(ev);free(eh);free(eb);free(ec);free(el);free(ei);if(rc!=FVS_OK)return rc;rc=fvs_ops_action_record(ctx,actor,"marketing_creative_created",id);if(rc!=FVS_OK)return rc;jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"creative_id\":");jw_string(&w,id);jw_puts(&w,",\"status\":\"pending_approval\"}");return jw_result(ctx,&w);}
int fvs_marketing_creative_approve(fvs_ctx *ctx,const char *creative_id,const char *actor){if(!ctx||!valid_id(creative_id)||!nonempty_max(actor,128U))return FVS_ERR_INVALID;char *ea=sql_escape(ctx,actor);if(!ea)return FVS_ERR_INTERNAL;char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE marketing_creatives SET status='approved',approved_by='%s',approved_at=UTC_TIMESTAMP(),updated_at=UTC_TIMESTAMP() WHERE id='%s' AND status='pending_approval'",ea,creative_id);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(ea);if(rc==FVS_OK&&mysql_affected_rows(ctx->db)==0ULL)return FVS_ERR_CONFLICT;return rc;}
int fvs_marketing_job_schedule(fvs_ctx *ctx,const char *campaign_id,const char *creative_id,const char *channel,const char *action,const char *scheduled_at,const char *payload,const char *actor,char *out,size_t out_size){if(!ctx||!valid_id(campaign_id)||(creative_id&&*creative_id&&!valid_id(creative_id))||!marketing_channel_valid(channel)||!marketing_action_valid(action)||!nonempty_max(scheduled_at,32U)||!optional_max(payload,12000U)||!nonempty_max(actor,128U)||!out)return FVS_ERR_INVALID;char id[37];if(fvs_uuid_v4(id)!=FVS_OK)return FVS_ERR_INTERNAL;char *ep=sql_escape(ctx,payload?payload:"{}");if(!ep)return FVS_ERR_INTERNAL;char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO marketing_jobs(id,campaign_id,creative_id,channel,action,payload_json,scheduled_at,status,attempts,next_attempt_at,created_at,updated_at) SELECT '%s','%s',%s%s%s,'%s','%s',CAST('%s' AS JSON),'%s','pending',0,'%s',UTC_TIMESTAMP(),UTC_TIMESTAMP() FROM marketing_campaigns c WHERE c.id='%s' AND c.status IN('approved','scheduled','active') AND (%s OR EXISTS(SELECT 1 FROM marketing_creatives mc WHERE mc.id='%s' AND mc.campaign_id=c.id AND mc.status='approved'))",id,campaign_id,creative_id&&*creative_id?"'":"",creative_id&&*creative_id?creative_id:"NULL",creative_id&&*creative_id?"'":"",channel,action,ep,scheduled_at,scheduled_at,campaign_id,creative_id&&*creative_id?"0":"1",creative_id&&*creative_id?creative_id:"");if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(ep);if(rc!=FVS_OK)return rc;if(mysql_affected_rows(ctx->db)==0ULL){seterr(ctx,"campaign or creative not approved");return FVS_ERR_CONFLICT;}rc=fvs_ops_action_record(ctx,actor,"marketing_job_scheduled",id);if(rc!=FVS_OK)return rc;jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"job_id\":");jw_string(&w,id);jw_puts(&w,",\"status\":\"pending\"}");return jw_result(ctx,&w);}
int fvs_marketing_job_claim(fvs_ctx *ctx,const char *owner,unsigned int lease_seconds,char *out,size_t out_size){if(!ctx||!nonempty_max(owner,128U)||lease_seconds<5U||lease_seconds>3600U||!out)return FVS_ERR_INVALID;char *eo=sql_escape(ctx,owner);if(!eo)return FVS_ERR_INTERNAL;char token[33];if(fvs_random_hex(token,sizeof token,16U)!=FVS_OK){free(eo);return FVS_ERR_INTERNAL;}int rc=tx_begin(ctx);if(rc!=FVS_OK){free(eo);return rc;}MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT j.id,j.campaign_id,COALESCE(j.creative_id,''),j.channel,j.action,j.payload_json,j.attempts,COALESCE(c.name,''),COALESCE(mc.headline,''),COALESCE(mc.body,''),COALESCE(mc.cta,''),COALESCE(mc.landing_url,''),COALESCE(mc.image_url,''),c.max_daily_spend_minor,c.frequency_cap_7d,c.target_roas_bps,c.stop_loss_minor FROM marketing_jobs j JOIN marketing_campaigns c ON c.id=j.campaign_id LEFT JOIN marketing_creatives mc ON mc.id=j.creative_id WHERE j.attempts<8 AND j.scheduled_at<=UTC_TIMESTAMP() AND ((j.status IN('pending','retry') AND j.next_attempt_at<=UTC_TIMESTAMP()) OR (j.status='processing' AND j.lease_until<=UTC_TIMESTAMP())) AND (j.lease_until IS NULL OR j.lease_until<=UTC_TIMESTAMP()) AND (j.action IN('pause','sync_metrics','send_conversion') OR (c.status IN('approved','scheduled','active') AND (c.daily_budget_minor=0 OR COALESCE((SELECT SUM(md.spend_minor) FROM marketing_metrics_daily md WHERE md.campaign_id=c.id AND md.metric_date=UTC_DATE()),0)<c.daily_budget_minor) AND (c.max_daily_spend_minor=0 OR COALESCE((SELECT SUM(md.spend_minor) FROM marketing_metrics_daily md WHERE md.campaign_id=c.id AND md.metric_date=UTC_DATE()),0)<c.max_daily_spend_minor) AND (c.budget_minor=0 OR COALESCE((SELECT SUM(ma.spend_minor) FROM marketing_metrics_daily ma WHERE ma.campaign_id=c.id),0)<c.budget_minor) AND (c.stop_loss_minor=0 OR GREATEST(COALESCE((SELECT SUM(ms.spend_minor) FROM marketing_metrics_daily ms WHERE ms.campaign_id=c.id),0)-COALESCE((SELECT SUM(mr.revenue_minor) FROM marketing_metrics_daily mr WHERE mr.campaign_id=c.id),0),0)<c.stop_loss_minor) AND (j.action<>'update_budget' OR c.target_roas_bps=0 OR COALESCE((SELECT SUM(mt.spend_minor) FROM marketing_metrics_daily mt WHERE mt.campaign_id=c.id),0)=0 OR ((CAST(COALESCE((SELECT SUM(mv.revenue_minor) FROM marketing_metrics_daily mv WHERE mv.campaign_id=c.id),0) AS DECIMAL(30,6))/NULLIF(CAST(COALESCE((SELECT SUM(mx.spend_minor) FROM marketing_metrics_daily mx WHERE mx.campaign_id=c.id),0) AS DECIMAL(30,6)),0))*10000)>=c.target_roas_bps))) ORDER BY j.scheduled_at,j.id LIMIT 1 FOR UPDATE SKIP LOCKED");if(rc!=FVS_OK){tx_rollback(ctx);free(eo);return rc;}MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);rc=tx_commit(ctx);free(eo);if(rc!=FVS_OK)return rc;if(out_size<3U)return FVS_ERR_BUFFER;memcpy(out,"{}",3U);return FVS_OK;}char id[37],camp[37],creative[37],channel[16],action[24],payload[12001],name[181],headline[256],body[12001],cta[81],landing[1501],image[1501],maxdaily[32],freqcap[16],targetroas[16],stoploss[32];(void)snprintf(id,sizeof id,"%s",r[0]);(void)snprintf(camp,sizeof camp,"%s",r[1]);(void)snprintf(creative,sizeof creative,"%s",r[2]);(void)snprintf(channel,sizeof channel,"%s",r[3]);(void)snprintf(action,sizeof action,"%s",r[4]);(void)snprintf(payload,sizeof payload,"%s",r[5]?r[5]:"{}");unsigned int attempts=r[6]?(unsigned int)strtoul(r[6],NULL,10):0U;(void)snprintf(name,sizeof name,"%s",r[7]);(void)snprintf(headline,sizeof headline,"%s",r[8]);(void)snprintf(body,sizeof body,"%s",r[9]);(void)snprintf(cta,sizeof cta,"%s",r[10]);(void)snprintf(landing,sizeof landing,"%s",r[11]);(void)snprintf(image,sizeof image,"%s",r[12]);(void)snprintf(maxdaily,sizeof maxdaily,"%s",r[13]?r[13]:"0");(void)snprintf(freqcap,sizeof freqcap,"%s",r[14]?r[14]:"0");(void)snprintf(targetroas,sizeof targetroas,"%s",r[15]?r[15]:"0");(void)snprintf(stoploss,sizeof stoploss,"%s",r[16]?r[16]:"0");mysql_free_result(res);char *sql=NULL;rc=sqlf(ctx,&sql,"UPDATE marketing_jobs SET status='processing',attempts=attempts+1,lease_owner='%s',lease_token='%s',lease_until=TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),updated_at=UTC_TIMESTAMP() WHERE id='%s'",eo,token,lease_seconds,id);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);if(rc!=FVS_OK){tx_rollback(ctx);free(eo);return rc;}rc=tx_commit(ctx);free(eo);if(rc!=FVS_OK)return rc;jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"job_id\":");jw_string(&w,id);jw_puts(&w,",\"campaign_id\":");jw_string(&w,camp);jw_puts(&w,",\"creative_id\":");creative[0]?jw_string(&w,creative):jw_puts(&w,"null");jw_puts(&w,",\"channel\":");jw_string(&w,channel);jw_puts(&w,",\"action\":");jw_string(&w,action);jw_puts(&w,",\"lease_token\":");jw_string(&w,token);jw_printf(&w,",\"attempts\":%u,\"campaign_name\":",attempts+1U);jw_string(&w,name);jw_puts(&w,",\"headline\":");jw_string(&w,headline);jw_puts(&w,",\"body\":");jw_string(&w,body);jw_puts(&w,",\"cta\":");jw_string(&w,cta);jw_puts(&w,",\"landing_url\":");jw_string(&w,landing);jw_puts(&w,",\"image_url\":");jw_string(&w,image);jw_puts(&w,",\"payload\":");jw_puts(&w,payload);jw_printf(&w,",\"guardrails\":{\"max_daily_spend_minor\":%s,\"frequency_cap_7d\":%s,\"target_roas_bps\":%s,\"stop_loss_minor\":%s}}",maxdaily,freqcap,targetroas,stoploss);return jw_result(ctx,&w);}
int fvs_marketing_job_ack(fvs_ctx *ctx,const char *id,const char *owner,const char *token,const char *provider_ref){if(!ctx||!valid_id(id)||!nonempty_max(owner,128U)||!nonempty_max(token,32U)||!optional_max(provider_ref,191U))return FVS_ERR_INVALID;char *eo=sql_escape(ctx,owner),*et=sql_escape(ctx,token),*ep=sql_escape(ctx,provider_ref?provider_ref:"");if(!eo||!et||!ep){free(eo);free(et);free(ep);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE marketing_jobs SET status='done',processed_at=UTC_TIMESTAMP(),provider_ref=%s%s%s,lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=NULL,updated_at=UTC_TIMESTAMP() WHERE id='%s' AND lease_owner='%s' AND lease_token='%s' AND status='processing'",*ep?"'":"",*ep?ep:"NULL",*ep?"'":"",id,eo,et);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(eo);free(et);free(ep);if(rc==FVS_OK&&mysql_affected_rows(ctx->db)==0ULL)return FVS_ERR_CONFLICT;return rc;}
int fvs_marketing_job_nack(fvs_ctx *ctx,const char *id,const char *owner,const char *token,const char *error){if(!ctx||!valid_id(id)||!nonempty_max(owner,128U)||!nonempty_max(token,32U)||!optional_max(error,1000U))return FVS_ERR_INVALID;char *eo=sql_escape(ctx,owner),*et=sql_escape(ctx,token),*ee=sql_escape(ctx,error?error:"");if(!eo||!et||!ee){free(eo);free(et);free(ee);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"UPDATE marketing_jobs SET status=IF(attempts>=8,'dead','retry'),next_attempt_at=TIMESTAMPADD(SECOND,LEAST(3600,15*(1 << LEAST(GREATEST(attempts-1,0),8))),UTC_TIMESTAMP()),lease_owner=NULL,lease_token=NULL,lease_until=NULL,last_error=%s%s%s,updated_at=UTC_TIMESTAMP() WHERE id='%s' AND lease_owner='%s' AND lease_token='%s' AND status='processing'",*ee?"'":"",*ee?ee:"NULL",*ee?"'":"",id,eo,et);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(eo);free(et);free(ee);if(rc==FVS_OK&&mysql_affected_rows(ctx->db)==0ULL)return FVS_ERR_CONFLICT;return rc;}
int fvs_marketing_metric_upsert(fvs_ctx *ctx,const char *campaign_id,const char *channel,const char *date,uint64_t impressions,uint64_t clicks,int64_t spend,uint64_t leads,uint64_t bookings,int64_t revenue,const char *currency){if(!ctx||!valid_id(campaign_id)||!marketing_channel_valid(channel)||!valid_date(date)||spend<0||revenue<0||!fvs_currency_valid(currency))return FVS_ERR_INVALID;char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO marketing_metrics_daily(campaign_id,channel,metric_date,impressions,clicks,spend_minor,leads,bookings,revenue_minor,currency,updated_at) VALUES('%s','%s','%s',%llu,%llu,%lld,%llu,%llu,%lld,'%s',UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE impressions=VALUES(impressions),clicks=VALUES(clicks),spend_minor=VALUES(spend_minor),leads=VALUES(leads),bookings=VALUES(bookings),revenue_minor=VALUES(revenue_minor),currency=VALUES(currency),updated_at=UTC_TIMESTAMP()",campaign_id,channel,date,(unsigned long long)impressions,(unsigned long long)clicks,(long long)spend,(unsigned long long)leads,(unsigned long long)bookings,(long long)revenue,currency);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);return rc;}
int fvs_marketing_attribution_record(fvs_ctx *ctx,const char *campaign_id,const char *channel,const char *creative_id,const char *visitor_id,const char *session_id,const char *order_id,const char *event_type,int64_t value,const char *currency,const char *utm_source,const char *utm_medium,const char *utm_campaign,const char *utm_content,const char *referrer){if(!ctx||!marketing_event_valid(event_type)||value<0)return FVS_ERR_INVALID;if(campaign_id&&*campaign_id&&!valid_id(campaign_id))return FVS_ERR_INVALID;if(creative_id&&*creative_id&&!valid_id(creative_id))return FVS_ERR_INVALID;if(visitor_id&&*visitor_id&&!valid_id(visitor_id))return FVS_ERR_INVALID;if(session_id&&*session_id&&!valid_id(session_id))return FVS_ERR_INVALID;if(order_id&&*order_id&&!valid_id(order_id))return FVS_ERR_INVALID;if(currency&&*currency&&!fvs_currency_valid(currency))return FVS_ERR_INVALID;char id[37];if(fvs_uuid_v4(id)!=FVS_OK)return FVS_ERR_INTERNAL;const char *vals[6]={utm_source,utm_medium,utm_campaign,utm_content,referrer,channel};char *e[6]={0};for(int i=0;i<6;i++){e[i]=sql_escape(ctx,vals[i]?vals[i]:"");if(!e[i]){for(int j=0;j<=i;j++)free(e[j]);return FVS_ERR_INTERNAL;}}char *sql=NULL;int rc=FVS_OK;
    char *cid=sql_escape(ctx,campaign_id?campaign_id:""),*cr=sql_escape(ctx,creative_id?creative_id:""),*vi=sql_escape(ctx,visitor_id?visitor_id:""),*si=sql_escape(ctx,session_id?session_id:""),*oi=sql_escape(ctx,order_id?order_id:""),*cu=sql_escape(ctx,currency?currency:"");if(!cid||!cr||!vi||!si||!oi||!cu){free(cid);free(cr);free(vi);free(si);free(oi);free(cu);for(int i=0;i<6;i++)free(e[i]);return FVS_ERR_INTERNAL;}
    rc=sqlf(ctx,&sql,"INSERT INTO marketing_attribution_events(id,campaign_id,channel,creative_id,visitor_id,session_id,order_id,event_type,value_minor,currency,utm_source,utm_medium,utm_campaign,utm_content,referrer,created_at) VALUES('%s',NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),'%s',%lld,NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),UTC_TIMESTAMP())",id,cid,e[5],cr,vi,si,oi,event_type,(long long)value,cu,e[0],e[1],e[2],e[3],e[4]);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(cid);free(cr);free(vi);free(si);free(oi);free(cu);for(int i=0;i<6;i++)free(e[i]);return rc;}
int fvs_marketing_dashboard(fvs_ctx *ctx,unsigned int days,char *out,size_t out_size){if(!ctx||!out||days<1U||days>730U)return FVS_ERR_INVALID;MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT COALESCE(SUM(impressions),0),COALESCE(SUM(clicks),0),COALESCE(SUM(spend_minor),0),COALESCE(SUM(leads),0),COALESCE(SUM(bookings),0),COALESCE(SUM(revenue_minor),0) FROM marketing_metrics_daily WHERE metric_date>=DATE(TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP()))",days);if(rc!=FVS_OK)return rc;MYSQL_ROW r=mysql_fetch_row(res);unsigned long long impressions=r&&r[0]?strtoull(r[0],NULL,10):0ULL,clicks=r&&r[1]?strtoull(r[1],NULL,10):0ULL,spend=r&&r[2]?strtoull(r[2],NULL,10):0ULL,leads=r&&r[3]?strtoull(r[3],NULL,10):0ULL,bookings=r&&r[4]?strtoull(r[4],NULL,10):0ULL,revenue=r&&r[5]?strtoull(r[5],NULL,10):0ULL;mysql_free_result(res);jsonw w;jw_init(&w,out,out_size);jw_printf(&w,"{\"days\":%u,\"impressions\":%llu,\"clicks\":%llu,\"spend_minor\":%llu,\"leads\":%llu,\"bookings\":%llu,\"revenue_minor\":%llu,\"ctr\":%.6f,\"roas\":%.6f}",days,impressions,clicks,spend,leads,bookings,revenue,impressions?((double)clicks/(double)impressions):0.0,spend?((double)revenue/(double)spend):0.0);return jw_result(ctx,&w);}

/* ---- Commerce Experience and social commerce extensions (6.x) ----------- */
static int commerce_discount_type_valid(const char *s) {
    return s && (!strcmp(s,"percent_bps") || !strcmp(s,"fixed_minor"));
}
static int social_channel_valid(const char *s) {
    return s && (!strcmp(s,"meta") || !strcmp(s,"instagram") || !strcmp(s,"tiktok") ||
                 !strcmp(s,"pinterest") || !strcmp(s,"whatsapp") || !strcmp(s,"youtube") ||
                 !strcmp(s,"x") || !strcmp(s,"other"));
}
static int code_token_valid(const char *s, size_t max_len) {
    if (!s || !*s || strlen(s) > max_len) return 0;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p) {
        if (!(isalnum(*p) || *p=='_' || *p=='-')) return 0;
    }
    return 1;
}

int fvs_commerce_home(fvs_ctx *ctx,char *out,size_t out_size) {
    if (!ctx || !out) { return FVS_ERR_INVALID; }
    jsonw w; jw_init(&w,out,out_size); jw_puts(&w,"{\"collections\":[");
    MYSQL_RES *res=NULL; int rc=queryf(ctx,&res,
        "SELECT id,slug,name,COALESCE(subtitle,''),COALESCE(hero_image_url,''),COALESCE(badge,''),"
        "COALESCE(filter_json,JSON_OBJECT()),COALESCE(merchandising_json,JSON_OBJECT()) "
        "FROM commerce_collections WHERE active=1 ORDER BY sort_order,name LIMIT 30");
    if (rc!=FVS_OK) { return rc; }
    int first=1; MYSQL_ROW r;
    while((r=mysql_fetch_row(res))!=NULL){
        if(!first){ jw_puts(&w,","); } first=0;
        jw_puts(&w,"{\"id\":");jw_string(&w,r[0]);jw_puts(&w,",\"slug\":");jw_string(&w,r[1]);jw_puts(&w,",\"name\":");jw_string(&w,r[2]);
        jw_puts(&w,",\"subtitle\":");jw_string(&w,r[3]);jw_puts(&w,",\"hero_image_url\":");jw_string(&w,r[4]);jw_puts(&w,",\"badge\":");jw_string(&w,r[5]);
        jw_puts(&w,",\"filter\":");jw_puts(&w,r[6]?r[6]:"{}");jw_puts(&w,",\"merchandising\":");jw_puts(&w,r[7]?r[7]:"{}");jw_puts(&w,"}");
    }
    mysql_free_result(res);
    jw_puts(&w,"],\"addons\":[");
    rc=queryf(ctx,&res,
        "SELECT code,name,COALESCE(description,''),category,pricing_model,price_minor,currency,COALESCE(icon,''),COALESCE(metadata_json,JSON_OBJECT()) "
        "FROM commerce_addons WHERE active=1 ORDER BY sort_order,name LIMIT 100");
    if(rc!=FVS_OK){ return rc; } first=1;
    while((r=mysql_fetch_row(res))!=NULL){
        if(!first){ jw_puts(&w,","); }first=0;jw_puts(&w,"{\"code\":");jw_string(&w,r[0]);jw_puts(&w,",\"name\":");jw_string(&w,r[1]);
        jw_puts(&w,",\"description\":");jw_string(&w,r[2]);jw_puts(&w,",\"category\":");jw_string(&w,r[3]);jw_puts(&w,",\"pricing_model\":");jw_string(&w,r[4]);
        jw_printf(&w,",\"price_minor\":%lld,\"currency\":",r[5]?strtoll(r[5],NULL,10):0LL);jw_string(&w,r[6]);jw_puts(&w,",\"icon\":");jw_string(&w,r[7]);jw_puts(&w,",\"metadata\":");jw_puts(&w,r[8]?r[8]:"{}");jw_puts(&w,"}");
    }
    mysql_free_result(res);
    jw_puts(&w,"],\"membership_plans\":[");
    rc=queryf(ctx,&res,
        "SELECT code,name,COALESCE(description,''),billing_period,fee_minor,currency,booking_discount_bps,points_multiplier_bps,COALESCE(benefits_json,JSON_ARRAY()) "
        "FROM commerce_membership_plans WHERE active=1 ORDER BY sort_order,name LIMIT 30");
    if(rc!=FVS_OK){ return rc; }first=1;
    while((r=mysql_fetch_row(res))!=NULL){
        if(!first){ jw_puts(&w,","); }first=0;jw_puts(&w,"{\"code\":");jw_string(&w,r[0]);jw_puts(&w,",\"name\":");jw_string(&w,r[1]);jw_puts(&w,",\"description\":");jw_string(&w,r[2]);jw_puts(&w,",\"billing_period\":");jw_string(&w,r[3]);
        jw_printf(&w,",\"fee_minor\":%lld,\"currency\":",r[4]?strtoll(r[4],NULL,10):0LL);jw_string(&w,r[5]);jw_printf(&w,",\"booking_discount_bps\":%u,\"points_multiplier_bps\":%u,\"benefits\":",r[6]?(unsigned int)strtoul(r[6],NULL,10):0U,r[7]?(unsigned int)strtoul(r[7],NULL,10):10000U);jw_puts(&w,r[8]?r[8]:"[]");jw_puts(&w,"}");
    }
    mysql_free_result(res);
    jw_puts(&w,"],\"promotions\":[");
    rc=queryf(ctx,&res,
        "SELECT code,name,discount_type,discount_value,min_subtotal_minor,max_discount_minor,currency,COALESCE(channel_scope,JSON_ARRAY()),DATE_FORMAT(ends_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') "
        "FROM commerce_promotions WHERE active=1 AND (starts_at IS NULL OR starts_at<=UTC_TIMESTAMP()) AND (ends_at IS NULL OR ends_at>UTC_TIMESTAMP()) AND (usage_limit=0 OR usage_count<usage_limit) ORDER BY updated_at DESC LIMIT 20");
    if(rc!=FVS_OK){ return rc; }first=1;
    while((r=mysql_fetch_row(res))!=NULL){
        if(!first){ jw_puts(&w,","); }first=0;jw_puts(&w,"{\"code\":");jw_string(&w,r[0]);jw_puts(&w,",\"name\":");jw_string(&w,r[1]);jw_puts(&w,",\"discount_type\":");jw_string(&w,r[2]);
        jw_printf(&w,",\"discount_value\":%lld,\"min_subtotal_minor\":%lld,\"max_discount_minor\":%lld,\"currency\":",r[3]?strtoll(r[3],NULL,10):0LL,r[4]?strtoll(r[4],NULL,10):0LL,r[5]?strtoll(r[5],NULL,10):0LL);jw_string(&w,r[6]);jw_puts(&w,",\"channel_scope\":");jw_puts(&w,r[7]?r[7]:"[]");jw_puts(&w,",\"ends_at\":");r[8]?jw_string(&w,r[8]):jw_puts(&w,"null");jw_puts(&w,"}");
    }
    mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);
}

int fvs_commerce_collection_get(fvs_ctx *ctx,const char *slug,char *out,size_t out_size){
    if(!ctx||!code_token_valid(slug,120U)||!out){ return FVS_ERR_INVALID; }char *es=sql_escape(ctx,slug);if(!es){ return FVS_ERR_INTERNAL; }
    MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT id,name,COALESCE(subtitle,''),COALESCE(hero_image_url,''),COALESCE(badge,''),COALESCE(filter_json,JSON_OBJECT()),COALESCE(merchandising_json,JSON_OBJECT()) FROM commerce_collections WHERE slug='%s' AND active=1",es);free(es);if(rc!=FVS_OK){ return rc; }MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}
    char id[37],name[181],subtitle[301],hero[1501],badge[65];(void)snprintf(id,sizeof id,"%s",r[0]);(void)snprintf(name,sizeof name,"%s",r[1]);(void)snprintf(subtitle,sizeof subtitle,"%s",r[2]);(void)snprintf(hero,sizeof hero,"%s",r[3]);(void)snprintf(badge,sizeof badge,"%s",r[4]);const char *filter_src=r[5]?r[5]:"{}";const char *merch_src=r[6]?r[6]:"{}";size_t filter_len=strlen(filter_src),merch_len=strlen(merch_src);char *filter=malloc(filter_len+1U),*merch=malloc(merch_len+1U);if(filter){memcpy(filter,filter_src,filter_len+1U);}if(merch){memcpy(merch,merch_src,merch_len+1U);}mysql_free_result(res);if(!filter||!merch){free(filter);free(merch);return FVS_ERR_INTERNAL;}
    rc=queryf(ctx,&res,"SELECT s.id,s.resort,s.unit_name,s.city,s.country,DATE_FORMAT(s.check_in,'%%Y-%%m-%%d'),DATE_FORMAT(s.check_out,'%%Y-%%m-%%d'),s.price_minor,s.currency,s.image_url,COALESCE(d.rating_x100,0),ci.rank_score,COALESCE(ci.badge,'') FROM commerce_collection_items ci JOIN inventory_slots s ON s.id=ci.slot_id LEFT JOIN inventory_discovery d ON d.slot_id=s.id WHERE ci.collection_id='%s' AND s.active=1 AND s.is_booked=0 AND (s.held_by_cart_id IS NULL OR s.hold_expires_at<=UTC_TIMESTAMP()) ORDER BY ci.rank_score DESC,s.price_minor LIMIT 50",id);if(rc!=FVS_OK){free(filter);free(merch);return rc;}
    jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"id\":");jw_string(&w,id);jw_puts(&w,",\"slug\":");jw_string(&w,slug);jw_puts(&w,",\"name\":");jw_string(&w,name);jw_puts(&w,",\"subtitle\":");jw_string(&w,subtitle);jw_puts(&w,",\"hero_image_url\":");jw_string(&w,hero);jw_puts(&w,",\"badge\":");jw_string(&w,badge);jw_puts(&w,",\"filter\":");jw_puts(&w,filter);jw_puts(&w,",\"merchandising\":");jw_puts(&w,merch);jw_puts(&w,",\"items\":[");free(filter);free(merch);int first=1;while((r=mysql_fetch_row(res))!=NULL){if(!first){ jw_puts(&w,","); }first=0;jw_puts(&w,"{\"id\":");jw_string(&w,r[0]);jw_puts(&w,",\"resort\":");jw_string(&w,r[1]);jw_puts(&w,",\"unit_name\":");jw_string(&w,r[2]);jw_puts(&w,",\"city\":");jw_string(&w,r[3]);jw_puts(&w,",\"country\":");jw_string(&w,r[4]);jw_puts(&w,",\"check_in\":");jw_string(&w,r[5]);jw_puts(&w,",\"check_out\":");jw_string(&w,r[6]);jw_printf(&w,",\"price_minor\":%lld,\"currency\":",r[7]?strtoll(r[7],NULL,10):0LL);jw_string(&w,r[8]);jw_puts(&w,",\"image_url\":");jw_string(&w,r[9]);jw_printf(&w,",\"rating\":%.2f,\"rank_score\":%d,\"badge\":",r[10]?strtod(r[10],NULL)/100.0:0.0,r[11]?atoi(r[11]):0);jw_string(&w,r[12]);jw_puts(&w,"}");}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);
}

int fvs_commerce_recommendations(fvs_ctx *ctx,const char *slot_id,unsigned int limit,char *out,size_t out_size){
    if(!ctx||!valid_id(slot_id)||limit<1U||limit>30U||!out){ return FVS_ERR_INVALID; }MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT COALESCE(city,''),COALESCE(d.property_type,'') FROM inventory_slots s LEFT JOIN inventory_discovery d ON d.slot_id=s.id WHERE s.id='%s'",slot_id);if(rc!=FVS_OK){ return rc; }MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}char city[121],ptype[81];(void)snprintf(city,sizeof city,"%s",r[0]);(void)snprintf(ptype,sizeof ptype,"%s",r[1]);mysql_free_result(res);char *ec=sql_escape(ctx,city),*ep=sql_escape(ctx,ptype);if(!ec||!ep){free(ec);free(ep);return FVS_ERR_INTERNAL;}
    rc=queryf(ctx,&res,"SELECT s.id,s.resort,s.unit_name,s.city,s.country,DATE_FORMAT(s.check_in,'%%Y-%%m-%%d'),DATE_FORMAT(s.check_out,'%%Y-%%m-%%d'),s.price_minor,s.currency,s.image_url,COALESCE(d.rating_x100,0),COALESCE(d.property_type,'') FROM inventory_slots s LEFT JOIN inventory_discovery d ON d.slot_id=s.id WHERE s.id<>'%s' AND s.active=1 AND s.is_booked=0 AND (s.held_by_cart_id IS NULL OR s.hold_expires_at<=UTC_TIMESTAMP()) AND ((s.city='%s' AND '%s'<>'') OR (d.property_type='%s' AND '%s'<>'') OR 1=1) ORDER BY (s.city='%s') DESC,(d.property_type='%s') DESC,COALESCE(d.rating_x100,0) DESC,s.price_minor LIMIT %u",slot_id,ec,ec,ep,ep,ec,ep,limit);free(ec);free(ep);if(rc!=FVS_OK){ return rc; }jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"items\":[");int first=1;while((r=mysql_fetch_row(res))!=NULL){if(!first){ jw_puts(&w,","); }first=0;jw_puts(&w,"{\"id\":");jw_string(&w,r[0]);jw_puts(&w,",\"resort\":");jw_string(&w,r[1]);jw_puts(&w,",\"unit_name\":");jw_string(&w,r[2]);jw_puts(&w,",\"city\":");jw_string(&w,r[3]);jw_puts(&w,",\"country\":");jw_string(&w,r[4]);jw_puts(&w,",\"check_in\":");jw_string(&w,r[5]);jw_puts(&w,",\"check_out\":");jw_string(&w,r[6]);jw_printf(&w,",\"price_minor\":%lld,\"currency\":",r[7]?strtoll(r[7],NULL,10):0LL);jw_string(&w,r[8]);jw_puts(&w,",\"image_url\":");jw_string(&w,r[9]);jw_printf(&w,",\"rating\":%.2f,\"property_type\":",r[10]?strtod(r[10],NULL)/100.0:0.0);jw_string(&w,r[11]);jw_puts(&w,"}");}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);
}

int fvs_cart_apply_code(fvs_ctx *ctx,const char *cart_id,const char *secret,const char *code,char *out,size_t out_size){
    if(!ctx||!valid_id(cart_id)||!secret||!out||!optional_max(code,64U)){ return FVS_ERR_INVALID; }char hash[65];int rc=auth_hash(secret,hash);if(rc!=FVS_OK){ return rc; }rc=tx_begin(ctx);if(rc!=FVS_OK){ return rc; }char status[24];unsigned long long version=0;rc=lock_cart(ctx,cart_id,hash,status,&version);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}if(strcmp(status,"open")!=0){tx_rollback(ctx);return FVS_ERR_STATE;}char *sql=NULL;
    if(!code||!*code){rc=sqlf(ctx,&sql,"UPDATE carts SET promo_code=NULL,referral_code=NULL,version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);}else{
        if(!code_token_valid(code,64U)){tx_rollback(ctx);return FVS_ERR_INVALID;}char *ec=sql_escape(ctx,code);if(!ec){tx_rollback(ctx);return FVS_ERR_INTERNAL;}MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT 'promo' FROM commerce_promotions p JOIN carts c ON c.id='%s' WHERE p.code='%s' AND p.active=1 AND (p.starts_at IS NULL OR p.starts_at<=UTC_TIMESTAMP()) AND (p.ends_at IS NULL OR p.ends_at>UTC_TIMESTAMP()) AND (p.usage_limit=0 OR p.usage_count<p.usage_limit) AND (p.channel_scope IS NULL OR JSON_LENGTH(p.channel_scope)=0 OR (COALESCE(c.source_channel,'')<>'' AND JSON_CONTAINS(p.channel_scope,JSON_QUOTE(c.source_channel)))) UNION ALL SELECT 'referral' FROM commerce_referral_codes WHERE code='%s' AND active=1 AND (starts_at IS NULL OR starts_at<=UTC_TIMESTAMP()) AND (ends_at IS NULL OR ends_at>UTC_TIMESTAMP()) AND (usage_limit=0 OR usage_count<usage_limit) LIMIT 1",cart_id,ec,ec);if(rc!=FVS_OK){free(ec);tx_rollback(ctx);return rc;}MYSQL_ROW row=mysql_fetch_row(res);char kind[16];(void)snprintf(kind,sizeof kind,"%s",row&&row[0]?row[0]:"");mysql_free_result(res);if(!kind[0]){free(ec);seterr(ctx,"code unavailable");tx_rollback(ctx);return FVS_ERR_NOT_FOUND;}if(strcmp(kind,"promo")==0){ rc=sqlf(ctx,&sql,"UPDATE carts SET promo_code='%s',referral_code=NULL,version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",ec,cart_id); }else rc=sqlf(ctx,&sql,"UPDATE carts SET referral_code='%s',promo_code=NULL,version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",ec,cart_id);free(ec);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);
    }
    if(rc!=FVS_OK){tx_rollback(ctx);return rc;}rc=tx_commit(ctx);if(rc!=FVS_OK){ return rc; }return render_cart(ctx,cart_id,hash,out,out_size);
}

int fvs_cart_addon_set(fvs_ctx *ctx,const char *cart_id,const char *secret,const char *addon_code,unsigned int quantity,char *out,size_t out_size){
    if(!ctx||!valid_id(cart_id)||!secret||!code_token_valid(addon_code,64U)||quantity>10U||!out){ return FVS_ERR_INVALID; }char hash[65];int rc=auth_hash(secret,hash);if(rc!=FVS_OK){ return rc; }rc=tx_begin(ctx);if(rc!=FVS_OK){ return rc; }char status[24];unsigned long long version=0;rc=lock_cart(ctx,cart_id,hash,status,&version);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}if(strcmp(status,"open")!=0){tx_rollback(ctx);return FVS_ERR_STATE;}char *ec=sql_escape(ctx,addon_code);if(!ec){tx_rollback(ctx);return FVS_ERR_INTERNAL;}char *sql=NULL;
    if(quantity==0U){rc=sqlf(ctx,&sql,"DELETE ca FROM cart_addons ca JOIN commerce_addons a ON a.id=ca.addon_id WHERE ca.cart_id='%s' AND a.code='%s'",cart_id,ec);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);}else{
        MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT id,name,pricing_model,price_minor,currency FROM commerce_addons WHERE code='%s' AND active=1 FOR SHARE",ec);if(rc!=FVS_OK){free(ec);tx_rollback(ctx);return rc;}MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);free(ec);tx_rollback(ctx);return FVS_ERR_NOT_FOUND;}char aid[37],name[181],model[16],currency[8];(void)snprintf(aid,sizeof aid,"%s",r[0]);(void)snprintf(name,sizeof name,"%s",r[1]);(void)snprintf(model,sizeof model,"%s",r[2]);int64_t unit=0;rc=parse_money_i64(ctx,r[3]?r[3]:"0",&unit);(void)snprintf(currency,sizeof currency,"%s",r[4]?r[4]:"");mysql_free_result(res);if(rc!=FVS_OK){free(ec);tx_rollback(ctx);return rc;}if(unit<0||unit>LLONG_MAX/(int64_t)quantity){free(ec);tx_rollback(ctx);return FVS_ERR_OVERFLOW;}rc=queryf(ctx,&res,"SELECT COUNT(*),COUNT(DISTINCT currency),COALESCE(MAX(currency),'') FROM cart_items WHERE cart_id='%s'",cart_id);if(rc!=FVS_OK){free(ec);tx_rollback(ctx);return rc;}r=mysql_fetch_row(res);unsigned long items=r&&r[0]?strtoul(r[0],NULL,10):0UL;unsigned long currencies=r&&r[1]?strtoul(r[1],NULL,10):0UL;char cartcur[8];(void)snprintf(cartcur,sizeof cartcur,"%s",r&&r[2]?r[2]:"");mysql_free_result(res);if(currencies>1UL||(items&& !currency_equal(cartcur,currency))){free(ec);tx_rollback(ctx);seterr(ctx,"add-on currency mismatch");return FVS_ERR_CONFLICT;}char *en=sql_escape(ctx,name);if(!en){free(ec);tx_rollback(ctx);return FVS_ERR_INTERNAL;}rc=sqlf(ctx,&sql,"INSERT INTO cart_addons(cart_id,addon_id,code_snapshot,quantity,unit_price_minor,currency,name_snapshot,pricing_model_snapshot,created_at,updated_at) VALUES('%s','%s','%s',%u,%lld,'%s','%s','%s',UTC_TIMESTAMP(),UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE code_snapshot=VALUES(code_snapshot),quantity=VALUES(quantity),unit_price_minor=VALUES(unit_price_minor),currency=VALUES(currency),name_snapshot=VALUES(name_snapshot),pricing_model_snapshot=VALUES(pricing_model_snapshot),updated_at=UTC_TIMESTAMP()",cart_id,aid,ec,quantity,(long long)unit,currency,en,model);free(en);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);
    }
    free(ec);if(rc==FVS_OK){rc=sqlf(ctx,&sql,"UPDATE carts SET version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);}if(rc!=FVS_OK){tx_rollback(ctx);return rc;}rc=tx_commit(ctx);if(rc!=FVS_OK){ return rc; }return render_cart(ctx,cart_id,hash,out,out_size);
}

int fvs_cart_set_origin(fvs_ctx *ctx,const char *cart_id,const char *secret,const char *channel,const char *campaign_id,const char *creative_id,const char *social_token){
    if(!ctx||!valid_id(cart_id)||!secret||!optional_max(channel,32U)||(campaign_id&&*campaign_id&&!valid_id(campaign_id))||(creative_id&&*creative_id&&!valid_id(creative_id))||!optional_max(social_token,32U)){ return FVS_ERR_INVALID; }char hash[65];int rc=auth_hash(secret,hash);if(rc!=FVS_OK){ return rc; }rc=tx_begin(ctx);if(rc!=FVS_OK){ return rc; }char status[24];unsigned long long version=0;rc=lock_cart(ctx,cart_id,hash,status,&version);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}if(strcmp(status,"open")!=0){tx_rollback(ctx);return FVS_ERR_STATE;}
    char previous_token[33]="";MYSQL_RES *origin_res=NULL;rc=queryf(ctx,&origin_res,"SELECT COALESCE(social_link_token,'') FROM carts WHERE id='%s'",cart_id);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}MYSQL_ROW origin_row=mysql_fetch_row(origin_res);if(origin_row&&origin_row[0]){ (void)snprintf(previous_token,sizeof previous_token,"%s",origin_row[0]); }mysql_free_result(origin_res);
    char ch[33]="",camp[37]="",creative[37]="",token[33]="",promo[65]="";if(channel){ (void)snprintf(ch,sizeof ch,"%s",channel); }if(campaign_id){ (void)snprintf(camp,sizeof camp,"%s",campaign_id); }if(creative_id){ (void)snprintf(creative,sizeof creative,"%s",creative_id); }if(social_token){ (void)snprintf(token,sizeof token,"%s",social_token); }
    if(token[0]){if(strlen(token)!=32U){tx_rollback(ctx);return FVS_ERR_INVALID;}char *et=sql_escape(ctx,token);if(!et){tx_rollback(ctx);return FVS_ERR_INTERNAL;}MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT channel,COALESCE(campaign_id,''),COALESCE(creative_id,''),COALESCE(promo_code,'') FROM social_sales_links WHERE token='%s' AND active=1 AND (expires_at IS NULL OR expires_at>UTC_TIMESTAMP())",et);free(et);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);tx_rollback(ctx);return FVS_ERR_NOT_FOUND;}(void)snprintf(ch,sizeof ch,"%s",r[0]);(void)snprintf(camp,sizeof camp,"%s",r[1]);(void)snprintf(creative,sizeof creative,"%s",r[2]);(void)snprintf(promo,sizeof promo,"%s",r[3]);mysql_free_result(res);}
    if(ch[0]&&!social_channel_valid(ch)){tx_rollback(ctx);return FVS_ERR_INVALID;}char *ech=sql_escape(ctx,ch),*ecamp=sql_escape(ctx,camp),*ecreative=sql_escape(ctx,creative),*etok=sql_escape(ctx,token),*ep=sql_escape(ctx,promo);if(!ech||!ecamp||!ecreative||!etok||!ep){free(ech);free(ecamp);free(ecreative);free(etok);free(ep);tx_rollback(ctx);return FVS_ERR_INTERNAL;}char *sql=NULL;rc=sqlf(ctx,&sql,"UPDATE carts SET source_channel=NULLIF('%s',''),source_campaign_id=NULLIF('%s',''),source_creative_id=NULLIF('%s',''),social_link_token=NULLIF('%s',''),promo_code=IF('%s'='',promo_code,'%s'),version=version+1,updated_at=UTC_TIMESTAMP() WHERE id='%s'",ech,ecamp,ecreative,etok,ep,ep,cart_id);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);free(ech);free(ecamp);free(ecreative);free(etok);free(ep);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}if(token[0]&&strcmp(previous_token,token)!=0){char *et=sql_escape(ctx,token);if(!et){tx_rollback(ctx);return FVS_ERR_INTERNAL;}rc=sqlf(ctx,&sql,"UPDATE social_sales_links SET carts=carts+1,updated_at=UTC_TIMESTAMP() WHERE token='%s'",et);free(et);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}}return tx_commit(ctx);
}

int fvs_commerce_promotion_upsert(fvs_ctx *ctx,const char *code,const char *name,const char *discount_type,int64_t discount_value,int64_t min_subtotal,int64_t max_discount,const char *currency,const char *starts_at,const char *ends_at,uint64_t usage_limit,int active,const char *channel_scope_json,const char *actor){
    if(!ctx||!code_token_valid(code,64U)||!nonempty_max(name,180U)||!commerce_discount_type_valid(discount_type)||discount_value<0||min_subtotal<0||max_discount<0||!fvs_currency_valid(currency)||!optional_max(starts_at,32U)||!optional_max(ends_at,32U)||!optional_max(channel_scope_json,2000U)||!nonempty_max(actor,128U)){ return FVS_ERR_INVALID; }if(!strcmp(discount_type,"percent_bps")&&discount_value>10000){ return FVS_ERR_INVALID; }char id[37];if(fvs_uuid_v4(id)!=FVS_OK){ return FVS_ERR_INTERNAL; }char *ec=sql_escape(ctx,code),*en=sql_escape(ctx,name),*es=sql_escape(ctx,starts_at?starts_at:""),*ee=sql_escape(ctx,ends_at?ends_at:""),*esc_scope=sql_escape(ctx,(channel_scope_json&&*channel_scope_json)?channel_scope_json:"[]"),*ea=sql_escape(ctx,actor);if(!ec||!en||!es||!ee||!esc_scope||!ea){free(ec);free(en);free(es);free(ee);free(esc_scope);free(ea);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO commerce_promotions(id,code,name,discount_type,discount_value,min_subtotal_minor,max_discount_minor,currency,starts_at,ends_at,usage_limit,channel_scope,active,created_by,created_at,updated_at) VALUES('%s','%s','%s','%s',%lld,%lld,%lld,'%s',NULLIF('%s',''),NULLIF('%s',''),%llu,CAST('%s' AS JSON),%d,'%s',UTC_TIMESTAMP(),UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE name=VALUES(name),discount_type=VALUES(discount_type),discount_value=VALUES(discount_value),min_subtotal_minor=VALUES(min_subtotal_minor),max_discount_minor=VALUES(max_discount_minor),currency=VALUES(currency),starts_at=VALUES(starts_at),ends_at=VALUES(ends_at),usage_limit=VALUES(usage_limit),channel_scope=VALUES(channel_scope),active=VALUES(active),updated_at=UTC_TIMESTAMP()",id,ec,en,discount_type,(long long)discount_value,(long long)min_subtotal,(long long)max_discount,currency,es,ee,(unsigned long long)usage_limit,esc_scope,active?1:0,ea);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);free(ec);free(en);free(es);free(ee);free(esc_scope);free(ea);if(rc!=FVS_OK){ return rc; }return fvs_ops_action_record(ctx,actor,"commerce_promotion_upsert",code);
}

int fvs_commerce_collection_upsert(fvs_ctx *ctx,const char *slug,const char *name,const char *subtitle,const char *hero,const char *badge,const char *filter_json,const char *merch_json,int active,int sort_order,const char *actor){
    if(!ctx||!code_token_valid(slug,120U)||!nonempty_max(name,180U)||!optional_max(subtitle,300U)||!optional_max(hero,1500U)||!optional_max(badge,64U)||!optional_max(filter_json,12000U)||!optional_max(merch_json,12000U)||!nonempty_max(actor,128U)){ return FVS_ERR_INVALID; }char id[37];if(fvs_uuid_v4(id)!=FVS_OK){ return FVS_ERR_INTERNAL; }const char *vals[7]={slug,name,subtitle?subtitle:"",hero?hero:"",badge?badge:"",filter_json?filter_json:"{}",merch_json?merch_json:"{}"};char *e[7]={0};for(int i=0;i<7;i++){e[i]=sql_escape(ctx,vals[i]);if(!e[i]){for(int j=0;j<=i;j++)free(e[j]);return FVS_ERR_INTERNAL;}}char *ea=sql_escape(ctx,actor);if(!ea){for(int i=0;i<7;i++)free(e[i]);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO commerce_collections(id,slug,name,subtitle,hero_image_url,badge,filter_json,merchandising_json,active,sort_order,created_by,created_at,updated_at) VALUES('%s','%s','%s',NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),CAST('%s' AS JSON),CAST('%s' AS JSON),%d,%d,'%s',UTC_TIMESTAMP(),UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE name=VALUES(name),subtitle=VALUES(subtitle),hero_image_url=VALUES(hero_image_url),badge=VALUES(badge),filter_json=VALUES(filter_json),merchandising_json=VALUES(merchandising_json),active=VALUES(active),sort_order=VALUES(sort_order),updated_at=UTC_TIMESTAMP()",id,e[0],e[1],e[2],e[3],e[4],e[5],e[6],active?1:0,sort_order,ea);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);for(int i=0;i<7;i++)free(e[i]);free(ea);if(rc!=FVS_OK){ return rc; }return fvs_ops_action_record(ctx,actor,"commerce_collection_upsert",slug);
}

int fvs_social_catalog_feed(fvs_ctx *ctx,const char *channel,unsigned int limit,unsigned int offset,char *out,size_t out_size){
    if(!ctx||!social_channel_valid(channel)||!strcmp(channel,"other")||limit<1U||limit>1000U||offset>1000000U||!out){ return FVS_ERR_INVALID; }MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT s.id,s.resort,s.unit_name,COALESCE(s.short_description,''),s.price_minor,s.currency,COALESCE(s.image_url,''),COALESCE(s.city,''),COALESCE(s.country,''),DATE_FORMAT(s.check_in,'%%Y-%%m-%%d'),DATE_FORMAT(s.check_out,'%%Y-%%m-%%d'),s.max_guests,COALESCE(d.property_type,''),COALESCE(d.rating_x100,0) FROM inventory_slots s LEFT JOIN inventory_discovery d ON d.slot_id=s.id WHERE s.active=1 AND s.is_booked=0 AND (s.held_by_cart_id IS NULL OR s.hold_expires_at<=UTC_TIMESTAMP()) ORDER BY s.updated_at DESC LIMIT %u OFFSET %u",limit,offset);if(rc!=FVS_OK){ return rc; }jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"channel\":");jw_string(&w,channel);jw_puts(&w,",\"items\":[");int first=1;MYSQL_ROW r;while((r=mysql_fetch_row(res))!=NULL){if(!first){ jw_puts(&w,","); }first=0;jw_puts(&w,"{\"id\":");jw_string(&w,r[0]);jw_puts(&w,",\"title\":");char title[350];(void)snprintf(title,sizeof title,"%s · %s",r[1]?r[1]:"",r[2]?r[2]:"");jw_string(&w,title);jw_puts(&w,",\"description\":");jw_string(&w,r[3]);jw_printf(&w,",\"price_minor\":%lld,\"currency\":",r[4]?strtoll(r[4],NULL,10):0LL);jw_string(&w,r[5]);jw_puts(&w,",\"image_url\":");jw_string(&w,r[6]);jw_puts(&w,",\"city\":");jw_string(&w,r[7]);jw_puts(&w,",\"country\":");jw_string(&w,r[8]);jw_puts(&w,",\"check_in\":");jw_string(&w,r[9]);jw_puts(&w,",\"check_out\":");jw_string(&w,r[10]);jw_printf(&w,",\"max_guests\":%u,\"property_type\":",r[11]?(unsigned int)strtoul(r[11],NULL,10):0U);jw_string(&w,r[12]);jw_printf(&w,",\"rating\":%.2f,\"availability\":\"in stock\",\"link_path\":",r[13]?strtod(r[13],NULL)/100.0:0.0);char link[512];(void)snprintf(link,sizeof link,"/?slot=%s&utm_source=%s&utm_medium=social_catalog",r[0],channel);jw_string(&w,link);jw_puts(&w,"}");}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);
}

int fvs_social_sales_link_create(fvs_ctx *ctx,const char *channel,const char *slot_id,const char *collection_slug,const char *campaign_id,const char *creative_id,const char *promo_code,const char *actor,char *out,size_t out_size){
    if(!ctx||!social_channel_valid(channel)||!strcmp(channel,"other")||(slot_id&&*slot_id&&!valid_id(slot_id))||!optional_max(collection_slug,120U)||(campaign_id&&*campaign_id&&!valid_id(campaign_id))||(creative_id&&*creative_id&&!valid_id(creative_id))||!optional_max(promo_code,64U)||!nonempty_max(actor,128U)||!out){ return FVS_ERR_INVALID; }if((!slot_id||!*slot_id)&&(!collection_slug||!*collection_slug)){ return FVS_ERR_INVALID; }char collection_id[37]="";if(collection_slug&&*collection_slug){if(!code_token_valid(collection_slug,120U)){ return FVS_ERR_INVALID; }char *es=sql_escape(ctx,collection_slug);if(!es){ return FVS_ERR_INTERNAL; }MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT id FROM commerce_collections WHERE slug='%s' AND active=1",es);free(es);if(rc!=FVS_OK){ return rc; }MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}(void)snprintf(collection_id,sizeof collection_id,"%s",r[0]);mysql_free_result(res);}char id[37],token[33];int rc=fvs_uuid_v4(id);if(rc==FVS_OK){ rc=fvs_random_hex(token,sizeof token,16U); }if(rc!=FVS_OK){ return rc; }char utm_campaign[161]="",utm_content[161]="";if(campaign_id&&*campaign_id){MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT utm_campaign FROM marketing_campaigns WHERE id='%s'",campaign_id);if(rc!=FVS_OK){ return rc; }MYSQL_ROW r=mysql_fetch_row(res);if(r){ (void)snprintf(utm_campaign,sizeof utm_campaign,"%s",r[0]?r[0]:""); }mysql_free_result(res);}if(creative_id&&*creative_id){MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT variant_key FROM marketing_creatives WHERE id='%s'",creative_id);if(rc!=FVS_OK){ return rc; }MYSQL_ROW r=mysql_fetch_row(res);if(r){ (void)snprintf(utm_content,sizeof utm_content,"%s",r[0]?r[0]:""); }mysql_free_result(res);}char *ep=sql_escape(ctx,promo_code?promo_code:""),*ea=sql_escape(ctx,actor),*euc=sql_escape(ctx,utm_campaign),*eut=sql_escape(ctx,utm_content);if(!ep||!ea||!euc||!eut){free(ep);free(ea);free(euc);free(eut);return FVS_ERR_INTERNAL;}char *sql=NULL;rc=sqlf(ctx,&sql,"INSERT INTO social_sales_links(id,token,channel,slot_id,collection_id,campaign_id,creative_id,promo_code,utm_source,utm_medium,utm_campaign,utm_content,active,created_by,created_at,updated_at) VALUES('%s','%s','%s',NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),'%s','social',NULLIF('%s',''),NULLIF('%s',''),1,'%s',UTC_TIMESTAMP(),UTC_TIMESTAMP())",id,token,channel,slot_id?slot_id:"",collection_id,campaign_id?campaign_id:"",creative_id?creative_id:"",ep,channel,euc,eut,ea);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);free(ep);free(ea);free(euc);free(eut);if(rc!=FVS_OK){ return rc; }jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"id\":");jw_string(&w,id);jw_puts(&w,",\"token\":");jw_string(&w,token);jw_puts(&w,",\"path\":");char path[80];(void)snprintf(path,sizeof path,"/s/%s",token);jw_string(&w,path);jw_puts(&w,"}");return jw_result(ctx,&w);
}

int fvs_social_sales_link_resolve(fvs_ctx *ctx,const char *token,char *out,size_t out_size){
    if(!ctx||!token||strlen(token)!=32U||!out){ return FVS_ERR_INVALID; }for(const unsigned char *p=(const unsigned char*)token;*p;++p)if(!isxdigit(*p)){ return FVS_ERR_INVALID; }char *et=sql_escape(ctx,token);if(!et){ return FVS_ERR_INTERNAL; }int rc=tx_begin(ctx);if(rc!=FVS_OK){free(et);return rc;}MYSQL_RES *res=NULL;rc=queryf(ctx,&res,"SELECT l.channel,COALESCE(l.slot_id,''),COALESCE(c.slug,''),COALESCE(l.campaign_id,''),COALESCE(l.creative_id,''),COALESCE(l.promo_code,''),COALESCE(l.utm_campaign,''),COALESCE(l.utm_content,'') FROM social_sales_links l LEFT JOIN commerce_collections c ON c.id=l.collection_id WHERE l.token='%s' AND l.active=1 AND (l.expires_at IS NULL OR l.expires_at>UTC_TIMESTAMP()) FOR UPDATE",et);if(rc!=FVS_OK){free(et);tx_rollback(ctx);return rc;}MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);free(et);tx_rollback(ctx);return FVS_ERR_NOT_FOUND;}char channel[33],slot[37],slug[121],camp[37],creative[37],promo[65],utmc[161],utmcontent[161];(void)snprintf(channel,sizeof channel,"%s",r[0]);(void)snprintf(slot,sizeof slot,"%s",r[1]);(void)snprintf(slug,sizeof slug,"%s",r[2]);(void)snprintf(camp,sizeof camp,"%s",r[3]);(void)snprintf(creative,sizeof creative,"%s",r[4]);(void)snprintf(promo,sizeof promo,"%s",r[5]);(void)snprintf(utmc,sizeof utmc,"%s",r[6]);(void)snprintf(utmcontent,sizeof utmcontent,"%s",r[7]);mysql_free_result(res);char *sql=NULL;rc=sqlf(ctx,&sql,"UPDATE social_sales_links SET clicks=clicks+1,updated_at=UTC_TIMESTAMP() WHERE token='%s'",et);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);free(et);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}rc=tx_commit(ctx);if(rc!=FVS_OK){ return rc; }char path[1400];const char *base=slot[0]?"/?slot=":"/?collection=";const char *target=slot[0]?slot:slug;int n=snprintf(path,sizeof path,"%s%s&fvs_social=%s&utm_source=%s&utm_medium=social%s%s%s%s%s%s%s%s%s%s",base,target,token,channel,utmc[0]?"&utm_campaign=":"",utmc,utmcontent[0]?"&utm_content=":"",utmcontent,promo[0]?"&promo=":"",promo,camp[0]?"&campaign_id=":"",camp,creative[0]?"&creative_id=":"",creative);if(n<0||(size_t)n>=sizeof path){ return FVS_ERR_BUFFER; }jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"channel\":");jw_string(&w,channel);jw_puts(&w,",\"path\":");jw_string(&w,path);jw_puts(&w,",\"promo_code\":");promo[0]?jw_string(&w,promo):jw_puts(&w,"null");jw_puts(&w,"}");return jw_result(ctx,&w);
}

int fvs_social_lead_capture(fvs_ctx *ctx,const char *channel,const char *provider_lead_id,const char *contact_json,const char *message,const char *campaign_id,const char *creative_id,char *out,size_t out_size){
    if(!ctx||!social_channel_valid(channel)||!nonempty_max(provider_lead_id,191U)||!optional_max(contact_json,12000U)||!nonempty_max(message,4000U)||(campaign_id&&*campaign_id&&!valid_id(campaign_id))||(creative_id&&*creative_id&&!valid_id(creative_id))||!out){ return FVS_ERR_INVALID; }char lead[37],thread[37],secret[65],hash[65],mid[37],eid[37];int rc=fvs_uuid_v4(lead);if(rc==FVS_OK){ rc=fvs_uuid_v4(thread); }if(rc==FVS_OK){ rc=fvs_uuid_v4(mid); }if(rc==FVS_OK){ rc=fvs_uuid_v4(eid); }if(rc==FVS_OK){ rc=fvs_random_hex(secret,sizeof secret,32U); }if(rc==FVS_OK){ rc=fvs_sha256_hex(secret,hash); }if(rc!=FVS_OK){ return rc; }char *eprov=sql_escape(ctx,provider_lead_id),*econtact=sql_escape(ctx,contact_json?contact_json:"{}"),*em=sql_escape(ctx,message);if(!eprov||!econtact||!em){free(eprov);free(econtact);free(em);return FVS_ERR_INTERNAL;}rc=tx_begin(ctx);if(rc!=FVS_OK){free(eprov);free(econtact);free(em);return rc;}char *sql=NULL;const char *support_channel=!strcmp(channel,"whatsapp")?"whatsapp":"social";rc=sqlf(ctx,&sql,"INSERT INTO support_threads(id,secret_hash,customer_email,subject,status,priority,channel,ai_enabled,first_response_due_at,resolution_due_at,last_message_at,created_at,updated_at) VALUES('%s',UNHEX('%s'),NULLIF(JSON_UNQUOTE(JSON_EXTRACT(CAST('%s' AS JSON),'$.email')),''),'Lead social · %s','open','normal','%s',1,TIMESTAMPADD(MINUTE,5,UTC_TIMESTAMP()),TIMESTAMPADD(HOUR,24,UTC_TIMESTAMP()),UTC_TIMESTAMP(),UTC_TIMESTAMP(),UTC_TIMESTAMP())",thread,hash,econtact,channel,support_channel);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);if(rc==FVS_OK){rc=sqlf(ctx,&sql,"INSERT INTO support_messages(id,thread_id,sender_type,sender_id,body,visibility,created_at) VALUES('%s','%s','customer','%s','%s','public',UTC_TIMESTAMP())",mid,thread,channel,em);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);}if(rc==FVS_OK){rc=sqlf(ctx,&sql,"INSERT INTO social_leads(id,channel,provider_lead_id,campaign_id,creative_id,support_thread_id,contact_json,message_text,status,created_at,updated_at) VALUES('%s','%s','%s',NULLIF('%s',''),NULLIF('%s',''),'%s',CAST('%s' AS JSON),'%s','in_support',UTC_TIMESTAMP(),UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE updated_at=UTC_TIMESTAMP()",lead,channel,eprov,campaign_id?campaign_id:"",creative_id?creative_id:"",thread,econtact,em);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);}if(rc==FVS_OK){rc=sqlf(ctx,&sql,"INSERT INTO outbox_events(id,event_type,aggregate_id,payload_json,status,attempts,next_attempt_at,created_at,updated_at) VALUES('%s','support.message','%s',JSON_OBJECT('thread_id','%s'),'pending',0,UTC_TIMESTAMP(),UTC_TIMESTAMP(),UTC_TIMESTAMP())",eid,thread,thread);if(rc==FVS_OK){ rc=exec_sql(ctx,sql); }free(sql);}free(eprov);free(econtact);free(em);if(rc!=FVS_OK){tx_rollback(ctx);return rc;}rc=tx_commit(ctx);if(rc!=FVS_OK){ return rc; }jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"lead_id\":");jw_string(&w,lead);jw_puts(&w,",\"thread_id\":");jw_string(&w,thread);jw_puts(&w,",\"status\":\"in_support\"}");return jw_result(ctx,&w);
}


int fvs_seo_rebuild_inventory(fvs_ctx *ctx,const char *actor,unsigned int *out_upserted){if(!ctx||!nonempty_max(actor,128U)||!out_upserted)return FVS_ERR_INVALID;const char *sql="INSERT INTO seo_pages(id,inventory_slot_id,slug,locale,page_type,title,meta_description,h1,body_text,canonical_path,og_image_url,indexable,priority,changefreq,published_at,updated_at) SELECT UUID(),s.id,CONCAT('vacation-rental-',LEFT(s.id,8)),'es-MX','property',LEFT(CONCAT(s.resort,' · ',s.unit_name,' | FVS'),180),LEFT(CONCAT('Reserva ',s.unit_name,' en ',s.resort,IF(s.city<>'',CONCAT(' en ',s.city),''),'. Fechas, disponibilidad y precio en FVS.'),320),LEFT(CONCAT(s.resort,' · ',s.unit_name),220),s.short_description,CONCAT('/stay/vacation-rental-',LEFT(s.id,8)),s.image_url,1,0.80,'daily',UTC_TIMESTAMP(),UTC_TIMESTAMP() FROM inventory_slots s WHERE s.active=1 ON DUPLICATE KEY UPDATE title=VALUES(title),meta_description=VALUES(meta_description),h1=VALUES(h1),body_text=VALUES(body_text),og_image_url=VALUES(og_image_url),indexable=1,updated_at=UTC_TIMESTAMP()";int rc=exec_sql(ctx,sql);if(rc!=FVS_OK)return rc;unsigned long long n=mysql_affected_rows(ctx->db);if(n>4294967295ULL)return FVS_ERR_OVERFLOW;*out_upserted=(unsigned int)n;return fvs_ops_action_record(ctx,actor,"seo_rebuild_inventory","inventory");}
int fvs_seo_page_upsert(fvs_ctx *ctx,const char *slot,const char *slug,const char *locale,const char *page_type,const char *title,const char *meta,const char *h1,const char *body,const char *faq,const char *canonical,const char *og,int indexable,const char *actor){if(!ctx||(slot&&*slot&&!valid_id(slot))||!nonempty_max(slug,220U)||!nonempty_max(locale,16U)||!seo_page_type_valid(page_type)||!nonempty_max(title,180U)||!nonempty_max(meta,320U)||!nonempty_max(h1,220U)||!optional_max(body,200000U)||!optional_max(faq,30000U)||!nonempty_max(canonical,500U)||!optional_max(og,1500U)||!nonempty_max(actor,128U))return FVS_ERR_INVALID;char *es=sql_escape(ctx,slug),*el=sql_escape(ctx,locale),*et=sql_escape(ctx,title),*em=sql_escape(ctx,meta),*eh=sql_escape(ctx,h1),*eb=sql_escape(ctx,body?body:""),*ef=sql_escape(ctx,faq?faq:"[]"),*ec=sql_escape(ctx,canonical),*eo=sql_escape(ctx,og?og:"");if(!es||!el||!et||!em||!eh||!eb||!ef||!ec||!eo){free(es);free(el);free(et);free(em);free(eh);free(eb);free(ef);free(ec);free(eo);return FVS_ERR_INTERNAL;}char id[37];if(fvs_uuid_v4(id)!=FVS_OK){free(es);free(el);free(et);free(em);free(eh);free(eb);free(ef);free(ec);free(eo);return FVS_ERR_INTERNAL;}char *sql=NULL;int rc=sqlf(ctx,&sql,"INSERT INTO seo_pages(id,inventory_slot_id,slug,locale,page_type,title,meta_description,h1,body_text,faq_json,canonical_path,og_image_url,indexable,published_at,updated_at) VALUES('%s',NULLIF('%s',''),'%s','%s','%s','%s','%s','%s',NULLIF('%s',''),CAST('%s' AS JSON),'%s',NULLIF('%s',''),%d,UTC_TIMESTAMP(),UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE inventory_slot_id=VALUES(inventory_slot_id),page_type=VALUES(page_type),title=VALUES(title),meta_description=VALUES(meta_description),h1=VALUES(h1),body_text=VALUES(body_text),faq_json=VALUES(faq_json),canonical_path=VALUES(canonical_path),og_image_url=VALUES(og_image_url),indexable=VALUES(indexable),updated_at=UTC_TIMESTAMP()",id,slot?slot:"",es,el,page_type,et,em,eh,eb,ef,ec,eo,indexable?1:0);if(rc==FVS_OK)rc=exec_sql(ctx,sql);free(sql);free(es);free(el);free(et);free(em);free(eh);free(eb);free(ef);free(ec);free(eo);if(rc!=FVS_OK)return rc;return fvs_ops_action_record(ctx,actor,"seo_page_upsert",slug);}
int fvs_seo_page_get(fvs_ctx *ctx,const char *slug,const char *locale,char *out,size_t out_size){if(!ctx||!nonempty_max(slug,220U)||!nonempty_max(locale,16U)||!out)return FVS_ERR_INVALID;char *es=sql_escape(ctx,slug),*el=sql_escape(ctx,locale);if(!es||!el){free(es);free(el);return FVS_ERR_INTERNAL;}MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT p.id,p.slug,p.locale,p.page_type,p.title,p.meta_description,p.h1,COALESCE(p.body_text,''),COALESCE(p.faq_json,JSON_ARRAY()),p.canonical_path,COALESCE(p.og_image_url,''),p.indexable,DATE_FORMAT(p.updated_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),COALESCE(s.id,''),COALESCE(s.resort,''),COALESCE(s.unit_name,''),COALESCE(s.city,''),COALESCE(s.country,''),COALESCE(DATE_FORMAT(s.check_in,'%%Y-%%m-%%d'),''),COALESCE(DATE_FORMAT(s.check_out,'%%Y-%%m-%%d'),''),COALESCE(s.price_minor,0),COALESCE(s.currency,'MXN'),COALESCE(s.max_guests,0),COALESCE(s.image_url,'') FROM seo_pages p LEFT JOIN inventory_slots s ON s.id=p.inventory_slot_id WHERE p.slug='%s' AND p.locale='%s' AND p.indexable=1",es,el);free(es);free(el);if(rc!=FVS_OK)return rc;MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}jsonw w;jw_init(&w,out,out_size);const char *keys[]={"id","slug","locale","page_type","title","meta_description","h1","body_text"};for(size_t i=0;i<8U;i++){jw_puts(&w,i?",\"":"{\"");jw_puts(&w,keys[i]);jw_puts(&w,"\":");jw_string(&w,r[i]);}jw_puts(&w,",\"faq\":");jw_puts(&w,r[8]?r[8]:"[]");jw_puts(&w,",\"canonical_path\":");jw_string(&w,r[9]);jw_puts(&w,",\"og_image_url\":");jw_string(&w,r[10]);jw_printf(&w,",\"indexable\":%s,\"updated_at\":",r[11]&&atoi(r[11])?"true":"false");jw_string(&w,r[12]);jw_puts(&w,",\"property\":{");jw_puts(&w,"\"slot_id\":");jw_string(&w,r[13]);jw_puts(&w,",\"resort\":");jw_string(&w,r[14]);jw_puts(&w,",\"unit_name\":");jw_string(&w,r[15]);jw_puts(&w,",\"city\":");jw_string(&w,r[16]);jw_puts(&w,",\"country\":");jw_string(&w,r[17]);jw_puts(&w,",\"check_in\":");jw_string(&w,r[18]);jw_puts(&w,",\"check_out\":");jw_string(&w,r[19]);jw_printf(&w,",\"price_minor\":%lld,\"currency\":",r[20]?strtoll(r[20],NULL,10):0LL);jw_string(&w,r[21]);jw_printf(&w,",\"max_guests\":%u,\"image_url\":",r[22]?(unsigned int)strtoul(r[22],NULL,10):0U);jw_string(&w,r[23]);jw_puts(&w,"}}");mysql_free_result(res);return jw_result(ctx,&w);}
int fvs_seo_pages_list(fvs_ctx *ctx,unsigned int limit,char *out,size_t out_size){if(!ctx||!out||limit<1U||limit>50000U)return FVS_ERR_INVALID;MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT slug,locale,canonical_path,changefreq,priority,DATE_FORMAT(updated_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') FROM seo_pages WHERE indexable=1 AND published_at IS NOT NULL ORDER BY updated_at DESC LIMIT %u",limit);if(rc!=FVS_OK)return rc;jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"pages\":[");int first=1;MYSQL_ROW r;while((r=mysql_fetch_row(res))!=NULL){if(!first)jw_puts(&w,",");first=0;jw_puts(&w,"{\"slug\":");jw_string(&w,r[0]);jw_puts(&w,",\"locale\":");jw_string(&w,r[1]);jw_puts(&w,",\"canonical_path\":");jw_string(&w,r[2]);jw_puts(&w,",\"changefreq\":");jw_string(&w,r[3]);jw_printf(&w,",\"priority\":%.2f,\"updated_at\":",r[4]?strtod(r[4],NULL):0.7);jw_string(&w,r[5]);jw_puts(&w,"}");}mysql_free_result(res);jw_puts(&w,"]}");return jw_result(ctx,&w);}
int fvs_seo_redirect_get(fvs_ctx *ctx,const char *source,char *out,size_t out_size){if(!ctx||!nonempty_max(source,500U)||!out)return FVS_ERR_INVALID;char *es=sql_escape(ctx,source);if(!es)return FVS_ERR_INTERNAL;MYSQL_RES *res=NULL;int rc=queryf(ctx,&res,"SELECT target_path,status_code FROM seo_redirects WHERE source_path='%s' AND active=1",es);free(es);if(rc!=FVS_OK)return rc;MYSQL_ROW r=mysql_fetch_row(res);if(!r){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}jsonw w;jw_init(&w,out,out_size);jw_puts(&w,"{\"target_path\":");jw_string(&w,r[0]);jw_printf(&w,",\"status_code\":%u}",r[1]?(unsigned int)strtoul(r[1],NULL,10):301U);mysql_free_result(res);return jw_result(ctx,&w);}
