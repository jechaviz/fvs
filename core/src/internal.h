#ifndef FVS_INTERNAL_H
#define FVS_INTERNAL_H

#include "fvs.h"
#include <mysql.h>
#include <stddef.h>

struct fvs_ctx {
    MYSQL *db;
    char last_error[512];
};

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int failed;
} fvs_i_json;

void fvs_i_seterr(fvs_ctx *ctx, const char *fmt, ...);
int fvs_i_db_status(fvs_ctx *ctx, const char *where);
int fvs_i_exec(fvs_ctx *ctx, const char *sql);
int fvs_i_queryf(fvs_ctx *ctx, MYSQL_RES **res, const char *fmt, ...);
char *fvs_i_escape(fvs_ctx *ctx, const char *text);
int fvs_i_valid_uuid(const char *text);

void fvs_i_json_init(fvs_i_json *writer, char *buf, size_t cap);
void fvs_i_json_puts(fvs_i_json *writer, const char *text);
void fvs_i_json_printf(fvs_i_json *writer, const char *fmt, ...);
void fvs_i_json_string(fvs_i_json *writer, const char *text);
int fvs_i_json_result(fvs_ctx *ctx, fvs_i_json *writer);

#endif
