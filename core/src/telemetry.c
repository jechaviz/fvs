#include "internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int token_ok(const char *text, size_t max_len, int allow_empty) {
    if (!text) return allow_empty;
    const size_t len = strlen(text);
    if (len == 0U) return allow_empty;
    if (len > max_len) return 0;
    for (size_t idx = 0U; idx < len; ++idx) {
        const unsigned char ch = (unsigned char)text[idx];
        if (!(isalnum(ch) || ch == (unsigned char)'_' || ch == (unsigned char)'-' || ch == (unsigned char)'.')) return 0;
    }
    return 1;
}

static int optional_uuid_ok(const char *text) {
    return !text || !*text || fvs_i_valid_uuid(text);
}

static int source_ok(const char *source) {
    return source && (!strcmp(source, "server") || !strcmp(source, "provider") ||
                      !strcmp(source, "worker") || !strcmp(source, "browser"));
}

int fvs_commerce_event_record(
    fvs_ctx *ctx,
    const char *event_key,
    const char *event_type,
    const char *source,
    const char *visitor_id,
    const char *session_id,
    const char *cart_id,
    const char *attempt_id,
    const char *order_id,
    const char *slot_id,
    const char *campaign_id,
    const char *creative_id,
    const char *channel,
    const char *provider,
    const char *outcome,
    const char *reason_code,
    int64_t value_minor,
    const char *currency,
    const char *query_text,
    int result_count,
    const char *metadata_json
) {
    if (!ctx || !token_ok(event_type, 48U, 0) || !source_ok(source) || value_minor < 0 || result_count < -1) return FVS_ERR_INVALID;
    if (!optional_uuid_ok(visitor_id) || !optional_uuid_ok(session_id) || !optional_uuid_ok(cart_id) ||
        !optional_uuid_ok(attempt_id) || !optional_uuid_ok(order_id) || !optional_uuid_ok(slot_id) ||
        !optional_uuid_ok(campaign_id) || !optional_uuid_ok(creative_id)) return FVS_ERR_INVALID;
    if (!token_ok(channel, 32U, 1) || !token_ok(provider, 32U, 1) ||
        !token_ok(outcome, 48U, 1) || !token_ok(reason_code, 80U, 1)) return FVS_ERR_INVALID;
    if (currency && *currency && !fvs_currency_valid(currency)) return FVS_ERR_INVALID;
    if (query_text && strlen(query_text) > 300U) return FVS_ERR_INVALID;
    if (metadata_json && strlen(metadata_json) > 8192U) return FVS_ERR_INVALID;

    char generated_key[64];
    const char *effective_key = event_key;
    if (!effective_key || !*effective_key) {
        char uuid[37];
        if (fvs_uuid_v4(uuid) != FVS_OK) return FVS_ERR_INTERNAL;
        const int key_len = snprintf(generated_key, sizeof generated_key, "auto:%s", uuid);
        if (key_len < 0 || (size_t)key_len >= sizeof generated_key) return FVS_ERR_INTERNAL;
        effective_key = generated_key;
    }
    if (strlen(effective_key) > 191U) return FVS_ERR_INVALID;

    const char *raw_values[] = {
        effective_key, event_type, source, visitor_id ? visitor_id : "", session_id ? session_id : "",
        cart_id ? cart_id : "", attempt_id ? attempt_id : "", order_id ? order_id : "", slot_id ? slot_id : "",
        campaign_id ? campaign_id : "", creative_id ? creative_id : "", channel ? channel : "", provider ? provider : "",
        outcome ? outcome : "", reason_code ? reason_code : "", currency ? currency : "", query_text ? query_text : "",
        metadata_json ? metadata_json : ""
    };
    enum { VALUE_COUNT = 18 };
    char *escaped[VALUE_COUNT];
    memset(escaped, 0, sizeof escaped);
    for (size_t idx = 0U; idx < VALUE_COUNT; ++idx) {
        escaped[idx] = fvs_i_escape(ctx, raw_values[idx]);
        if (!escaped[idx]) {
            for (size_t free_idx = 0U; free_idx < VALUE_COUNT; ++free_idx) free(escaped[free_idx]);
            return FVS_ERR_INTERNAL;
        }
    }

    char uuid[37];
    if (fvs_uuid_v4(uuid) != FVS_OK) {
        for (size_t idx = 0U; idx < VALUE_COUNT; ++idx) free(escaped[idx]);
        return FVS_ERR_INTERNAL;
    }

    char result_sql[32];
    if (result_count < 0) {
        (void)snprintf(result_sql, sizeof result_sql, "NULL");
    } else {
        (void)snprintf(result_sql, sizeof result_sql, "%d", result_count);
    }
    const char *metadata_expr = (metadata_json && *metadata_json) ? "CAST('%s' AS JSON)" : "NULL";
    const int base_len = snprintf(NULL, 0,
        "INSERT INTO commerce_events(id,event_key,event_type,source,visitor_id,session_id,cart_id,attempt_id,order_id,slot_id,campaign_id,creative_id,channel,provider,outcome,reason_code,value_minor,currency,query_text,result_count,metadata_json,created_at) "
        "VALUES('%s','%s','%s','%s',NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),%lld,NULLIF('%s',''),NULLIF('%s',''),%s,",
        uuid, escaped[0], escaped[1], escaped[2], escaped[3], escaped[4], escaped[5], escaped[6], escaped[7], escaped[8],
        escaped[9], escaped[10], escaped[11], escaped[12], escaped[13], escaped[14], (long long)value_minor, escaped[15], escaped[16], result_sql);
    if (base_len < 0) {
        for (size_t idx = 0U; idx < VALUE_COUNT; ++idx) free(escaped[idx]);
        return FVS_ERR_INTERNAL;
    }
    const size_t extra = strlen(metadata_expr) + strlen(escaped[17]) + 128U;
    char *sql = malloc((size_t)base_len + extra + 1U);
    if (!sql) {
        for (size_t idx = 0U; idx < VALUE_COUNT; ++idx) free(escaped[idx]);
        return FVS_ERR_INTERNAL;
    }
    const int prefix_len = snprintf(sql, (size_t)base_len + extra + 1U,
        "INSERT INTO commerce_events(id,event_key,event_type,source,visitor_id,session_id,cart_id,attempt_id,order_id,slot_id,campaign_id,creative_id,channel,provider,outcome,reason_code,value_minor,currency,query_text,result_count,metadata_json,created_at) "
        "VALUES('%s','%s','%s','%s',NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),NULLIF('%s',''),%lld,NULLIF('%s',''),NULLIF('%s',''),%s,",
        uuid, escaped[0], escaped[1], escaped[2], escaped[3], escaped[4], escaped[5], escaped[6], escaped[7], escaped[8],
        escaped[9], escaped[10], escaped[11], escaped[12], escaped[13], escaped[14], (long long)value_minor, escaped[15], escaped[16], result_sql);
    if (prefix_len < 0) {
        free(sql);
        for (size_t idx = 0U; idx < VALUE_COUNT; ++idx) free(escaped[idx]);
        return FVS_ERR_INTERNAL;
    }
    size_t used = (size_t)prefix_len;
    const size_t cap = (size_t)base_len + extra + 1U;
    int append_len;
    if (metadata_json && *metadata_json) {
        append_len = snprintf(sql + used, cap - used, "CAST('%s' AS JSON),UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE event_key=VALUES(event_key)", escaped[17]);
    } else {
        append_len = snprintf(sql + used, cap - used, "NULL,UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE event_key=VALUES(event_key)");
    }
    for (size_t idx = 0U; idx < VALUE_COUNT; ++idx) free(escaped[idx]);
    if (append_len < 0 || (size_t)append_len >= cap - used) {
        free(sql);
        return FVS_ERR_INTERNAL;
    }
    const int rc = fvs_i_exec(ctx, sql);
    free(sql);
    return rc;
}

int fvs_commerce_funnel_dashboard(fvs_ctx *ctx, unsigned int days, char *out, size_t out_size) {
    if (!ctx || !out || days < 1U || days > 730U) return FVS_ERR_INVALID;
    MYSQL_RES *res = NULL;
    int rc = fvs_i_queryf(ctx, &res,
        "SELECT "
        "COALESCE(SUM(event_type='search'),0),"
        "COALESCE(SUM(event_type='search' AND result_count=0),0),"
        "COUNT(DISTINCT CASE WHEN event_type='cart_add' THEN cart_id END),"
        "COUNT(DISTINCT CASE WHEN event_type='checkout_start' AND outcome='started' THEN attempt_id END),"
        "COUNT(DISTINCT CASE WHEN event_type='booking' AND outcome='confirmed' THEN order_id END),"
        "COALESCE(SUM(event_type='payment' AND outcome IN ('provider_error','manual_review','failed')),0) "
        "FROM commerce_events WHERE created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())", days);
    if (rc != FVS_OK) return rc;
    MYSQL_ROW row = mysql_fetch_row(res);
    const unsigned long long searches = row && row[0] ? strtoull(row[0], NULL, 10) : 0ULL;
    const unsigned long long zero_results = row && row[1] ? strtoull(row[1], NULL, 10) : 0ULL;
    const unsigned long long carts = row && row[2] ? strtoull(row[2], NULL, 10) : 0ULL;
    const unsigned long long checkouts = row && row[3] ? strtoull(row[3], NULL, 10) : 0ULL;
    const unsigned long long bookings = row && row[4] ? strtoull(row[4], NULL, 10) : 0ULL;
    const unsigned long long payment_failures = row && row[5] ? strtoull(row[5], NULL, 10) : 0ULL;
    mysql_free_result(res);

    fvs_i_json writer;
    fvs_i_json_init(&writer, out, out_size);
    fvs_i_json_printf(&writer,
        "{\"days\":%u,\"searches\":%llu,\"zero_results\":%llu,\"zero_result_rate\":%.6f,"
        "\"carts\":%llu,\"checkouts\":%llu,\"bookings\":%llu,\"payment_failures\":%llu,"
        "\"search_to_cart\":%.6f,\"cart_to_checkout\":%.6f,\"checkout_to_booking\":%.6f,\"revenue_by_currency\":[",
        days, searches, zero_results, searches ? (double)zero_results / (double)searches : 0.0,
        carts, checkouts, bookings, payment_failures,
        searches ? (double)carts / (double)searches : 0.0,
        carts ? (double)checkouts / (double)carts : 0.0,
        checkouts ? (double)bookings / (double)checkouts : 0.0);

    rc = fvs_i_queryf(ctx, &res,
        "SELECT currency,COUNT(DISTINCT order_id),COALESCE(SUM(value_minor),0) "
        "FROM commerce_events WHERE event_type='booking' AND outcome='confirmed' AND currency IS NOT NULL "
        "AND created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP()) GROUP BY currency ORDER BY currency", days);
    if (rc != FVS_OK) return rc;
    int first = 1;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (!first) fvs_i_json_puts(&writer, ",");
        first = 0;
        fvs_i_json_puts(&writer, "{\"currency\":");
        fvs_i_json_string(&writer, row[0] ? row[0] : "");
        fvs_i_json_printf(&writer, ",\"bookings\":%llu,\"revenue_minor\":%llu}",
                          row[1] ? strtoull(row[1], NULL, 10) : 0ULL,
                          row[2] ? strtoull(row[2], NULL, 10) : 0ULL);
    }
    mysql_free_result(res);
    fvs_i_json_puts(&writer, "]}");
    return fvs_i_json_result(ctx, &writer);
}
