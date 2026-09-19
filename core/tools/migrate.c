#include <dirent.h>
#include <errno.h>
#include <mysql.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ends_sql(const char *s) {
    size_t n = strlen(s);
    return n > 4U && strcmp(s + n - 4U, ".sql") == 0;
}

static int cmpstr(const void *a, const void *b) {
    const char *const *aa = a;
    const char *const *bb = b;
    return strcmp(*aa, *bb);
}

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1U;
    char *out = malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1U);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1U, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(b); return NULL; }
    b[r] = '\0';
    *len = r;
    return b;
}

static int sha256_hex(const unsigned char *data, size_t len, char out[65]) {
    unsigned char d[32];
    unsigned int dl = 0;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c) return 0;
    int ok = EVP_DigestInit_ex(c, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(c, data, len) == 1 &&
             EVP_DigestFinal_ex(c, d, &dl) == 1;
    EVP_MD_CTX_free(c);
    if (!ok || dl != 32U) return 0;
    static const char h[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 32U; ++i) {
        out[i * 2U] = h[d[i] >> 4U];
        out[i * 2U + 1U] = h[d[i] & 15U];
    }
    out[64] = '\0';
    return 1;
}

static int run_multi(MYSQL *db, const char *sql) {
    if (mysql_query(db, sql) != 0) return 0;
    int next = 0;
    do {
        MYSQL_RES *r = mysql_store_result(db);
        if (r) mysql_free_result(r);
        else if (mysql_field_count(db) != 0U) return 0;
        next = mysql_next_result(db);
    } while (next == 0);
    return next == -1;
}

static char *esc(MYSQL *db, const char *s) {
    size_t n = strlen(s);
    char *o = malloc(n * 2U + 1U);
    if (!o) return NULL;
    unsigned long w = mysql_real_escape_string(db, o, s, (unsigned long)n);
    o[w] = '\0';
    return o;
}

static void free_files(char **files, size_t n) {
    if (!files) return;
    for (size_t i = 0; i < n; ++i) free(files[i]);
    free(files);
}

static int acquire_migration_lock(MYSQL *db, unsigned int timeout_seconds) {
    char q[160];
    int n = snprintf(q, sizeof q, "SELECT GET_LOCK('fvs_schema_migrate',%u)", timeout_seconds);
    if (n < 0 || (size_t)n >= sizeof q || mysql_query(db, q) != 0) return 0;
    MYSQL_RES *res = mysql_store_result(db);
    if (!res) return 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    int ok = row && row[0] && strcmp(row[0], "1") == 0;
    mysql_free_result(res);
    return ok;
}

static void release_migration_lock(MYSQL *db) {
    if (mysql_query(db, "SELECT RELEASE_LOCK('fvs_schema_migrate')") != 0) return;
    MYSQL_RES *res = mysql_store_result(db);
    if (res) mysql_free_result(res);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "migrations";
    const char *host = getenv("FVS_MIGRATE_DB_HOST"); if (!host || !*host) host = getenv("FVS_DB_HOST");
    const char *user = getenv("FVS_MIGRATE_DB_USER"); if (!user || !*user) user = getenv("FVS_DB_USER");
    const char *pass = getenv("FVS_MIGRATE_DB_PASSWORD"); if (!pass) pass = getenv("FVS_DB_PASSWORD");
    const char *name = getenv("FVS_MIGRATE_DB_NAME"); if (!name || !*name) name = getenv("FVS_DB_NAME");
    const char *port_s = getenv("FVS_MIGRATE_DB_PORT"); if (!port_s || !*port_s) port_s = getenv("FVS_DB_PORT");
    if (!host) host = "127.0.0.1";
    if (!user) user = "fvs";
    if (!pass) pass = "";
    if (!name) name = "fvs";
    unsigned int port = port_s ? (unsigned int)strtoul(port_s, NULL, 10) : 3306U;

    MYSQL *db = mysql_init(NULL);
    if (!db) { fprintf(stderr, "mysql_init failed\n"); return 2; }
    const char *env_name = getenv("FVS_ENV");
    const char *allow_insecure = getenv("FVS_ALLOW_INSECURE_DB");
    const char *configured_ca = getenv("FVS_DB_SSL_CA");
    if (env_name && strcmp(env_name,"production") == 0 &&
        (!configured_ca || !*configured_ca) && (!allow_insecure || strcmp(allow_insecure,"1") != 0)) {
        fprintf(stderr, "production requires FVS_DB_SSL_CA unless FVS_ALLOW_INSECURE_DB=1\n");
        mysql_close(db);
        return 2;
    }
    const char *timeout_s = getenv("FVS_DB_CONNECT_TIMEOUT");
    unsigned int timeout = timeout_s ? (unsigned int)strtoul(timeout_s, NULL, 10) : 5U;
    if (timeout == 0U || timeout > 120U) timeout = 5U;
    (void)mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    const char *read_s = getenv("FVS_DB_READ_TIMEOUT");
    const char *write_s = getenv("FVS_DB_WRITE_TIMEOUT");
    unsigned int read_timeout = read_s ? (unsigned int)strtoul(read_s,NULL,10) : 30U;
    unsigned int write_timeout = write_s ? (unsigned int)strtoul(write_s,NULL,10) : 30U;
    if (read_timeout == 0U || read_timeout > 300U) read_timeout = 30U;
    if (write_timeout == 0U || write_timeout > 300U) write_timeout = 30U;
    (void)mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
    (void)mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
    const char *charset = "utf8mb4";
    (void)mysql_options(db, MYSQL_SET_CHARSET_NAME, charset);
    const char *ssl_ca = getenv("FVS_DB_SSL_CA");
    if (ssl_ca && *ssl_ca) {
        const char *ssl_cert = getenv("FVS_DB_SSL_CERT");
        const char *ssl_key = getenv("FVS_DB_SSL_KEY");
        if (mysql_ssl_set(db,
                          (ssl_key && *ssl_key) ? ssl_key : NULL,
                          (ssl_cert && *ssl_cert) ? ssl_cert : NULL,
                          ssl_ca, NULL, NULL) != 0) {
            fprintf(stderr, "mysql SSL setup failed\n");
            mysql_close(db);
            return 2;
        }
        my_bool yes = 1;
        if (mysql_options(db, MYSQL_OPT_SSL_ENFORCE, &yes) != 0 ||
            mysql_options(db, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &yes) != 0) {
            fprintf(stderr, "mysql SSL verification setup failed\n");
            mysql_close(db);
            return 2;
        }
    }
    if (!mysql_real_connect(db, host, user, pass, name, port, NULL, CLIENT_MULTI_STATEMENTS)) {
        fprintf(stderr, "connect: %s\n", mysql_error(db));
        mysql_close(db);
        return 2;
    }
    const char *lock_s = getenv("FVS_DB_LOCK_WAIT_TIMEOUT");
    unsigned int lock_wait = lock_s ? (unsigned int)strtoul(lock_s,NULL,10) : 30U;
    if (lock_wait == 0U || lock_wait > 120U) lock_wait = 30U;
    char session_sql[512];
    int session_n = snprintf(session_sql,sizeof session_sql,
        "SET SESSION time_zone='+00:00'; SET SESSION sql_mode='STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'; SET SESSION innodb_lock_wait_timeout=%u",lock_wait);
    if (session_n < 0 || (size_t)session_n >= sizeof session_sql || !run_multi(db, session_sql)) {
        fprintf(stderr, "session setup: %s\n", mysql_error(db));
        mysql_close(db);
        return 2;
    }

    const char *migration_lock_s = getenv("FVS_MIGRATION_LOCK_TIMEOUT");
    unsigned int migration_lock_timeout = migration_lock_s ? (unsigned int)strtoul(migration_lock_s,NULL,10) : 30U;
    if (migration_lock_timeout == 0U || migration_lock_timeout > 600U) migration_lock_timeout = 30U;
    if (!acquire_migration_lock(db, migration_lock_timeout)) {
        fprintf(stderr, "could not acquire migration advisory lock within %u seconds\n", migration_lock_timeout);
        mysql_close(db);
        return 5;
    }

    const char *bootstrap =
        "CREATE TABLE IF NOT EXISTS schema_migrations("
        "version VARCHAR(128) PRIMARY KEY,checksum CHAR(64) NOT NULL,"
        "dirty TINYINT(1) NOT NULL DEFAULT 0,"
        "applied_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (!run_multi(db, bootstrap)) {
        fprintf(stderr, "bootstrap: %s\n", mysql_error(db));
        mysql_close(db);
        return 2;
    }

    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "opendir %s: %s\n", dir, strerror(errno));
        mysql_close(db);
        return 2;
    }

    size_t cap = 16U, n = 0U;
    char **files = calloc(cap, sizeof *files);
    if (!files) { closedir(d); mysql_close(db); return 2; }
    struct dirent *de = NULL;
    while ((de = readdir(d)) != NULL) {
        if (!ends_sql(de->d_name)) continue;
        if (n == cap) {
            cap *= 2U;
            char **tmp = realloc(files, cap * sizeof *files);
            if (!tmp) { free_files(files, n); closedir(d); mysql_close(db); return 2; }
            files = tmp;
        }
        files[n] = dupstr(de->d_name);
        if (!files[n]) { free_files(files, n); closedir(d); mysql_close(db); return 2; }
        ++n;
    }
    closedir(d);
    qsort(files, n, sizeof *files, cmpstr);

    for (size_t i = 0; i < n; ++i) {
        char path[4096];
        int pn = snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        if (pn < 0 || (size_t)pn >= sizeof path) { free_files(files, n); mysql_close(db); return 2; }
        size_t len = 0U;
        char *sql = slurp(path, &len);
        if (!sql) { fprintf(stderr, "read %s failed\n", path); free_files(files, n); mysql_close(db); return 2; }
        char sum[65];
        if (!sha256_hex((const unsigned char *)sql, len, sum)) {
            fprintf(stderr, "hash failed\n"); free(sql); free_files(files, n); mysql_close(db); return 2;
        }
        char *ver = esc(db, files[i]);
        if (!ver) { free(sql); free_files(files, n); mysql_close(db); return 2; }
        char q[1024];
        int qn = snprintf(q, sizeof q, "SELECT checksum,dirty FROM schema_migrations WHERE version='%s'", ver);
        if (qn < 0 || (size_t)qn >= sizeof q || mysql_query(db, q) != 0) {
            fprintf(stderr, "query: %s\n", mysql_error(db)); free(ver); free(sql); free_files(files, n); mysql_close(db); return 2;
        }
        MYSQL_RES *res = mysql_store_result(db);
        MYSQL_ROW row = res ? mysql_fetch_row(res) : NULL;
        if (row) {
            int dirty = row[1] ? atoi(row[1]) : 0;
            int mismatch = strcmp(row[0] ? row[0] : "", sum) != 0;
            if (dirty || mismatch) {
                fprintf(stderr, "migration %s is %s\n", files[i], dirty ? "dirty" : "checksum-mismatched");
                if (res) mysql_free_result(res);
                free(ver); free(sql); free_files(files, n); mysql_close(db); return 3;
            }
            printf("skip %s\n", files[i]);
            if (res) mysql_free_result(res);
            free(ver); free(sql);
            continue;
        }
        if (res) mysql_free_result(res);

        qn = snprintf(q, sizeof q,
            "INSERT INTO schema_migrations(version,checksum,dirty,applied_at) VALUES('%s','%s',1,UTC_TIMESTAMP(6))",
            ver, sum);
        if (qn < 0 || (size_t)qn >= sizeof q || mysql_query(db, q) != 0) {
            fprintf(stderr, "mark dirty: %s\n", mysql_error(db)); free(ver); free(sql); free_files(files, n); mysql_close(db); return 2;
        }
        printf("apply %s\n", files[i]);
        if (!run_multi(db, sql)) {
            fprintf(stderr, "migration %s failed: %s\n", files[i], mysql_error(db));
            free(ver); free(sql); free_files(files, n); mysql_close(db); return 4;
        }
        qn = snprintf(q, sizeof q,
            "UPDATE schema_migrations SET dirty=0,checksum='%s',applied_at=UTC_TIMESTAMP(6) WHERE version='%s'", sum, ver);
        if (qn < 0 || (size_t)qn >= sizeof q || mysql_query(db, q) != 0) {
            fprintf(stderr, "mark clean: %s\n", mysql_error(db)); free(ver); free(sql); free_files(files, n); mysql_close(db); return 2;
        }
        free(ver);
        free(sql);
    }

    free_files(files, n);
    release_migration_lock(db);
    mysql_close(db);
    printf("migrations OK\n");
    return 0;
}
