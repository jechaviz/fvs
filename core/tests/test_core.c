#include "fvs.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)

int main(void) {
    char u1[37], u2[37];
    CHECK(fvs_uuid_v4(u1) == FVS_OK);
    CHECK(fvs_uuid_v4(u2) == FVS_OK);
    CHECK(strlen(u1) == 36U);
    CHECK(strcmp(u1,u2) != 0);
    CHECK(u1[14] == '4');
    CHECK(u1[19] == '8' || u1[19] == '9' || u1[19] == 'a' || u1[19] == 'b');

    char secret[65], secret2[65];
    CHECK(fvs_random_hex(secret,sizeof secret,32U) == FVS_OK);
    CHECK(fvs_random_hex(secret2,sizeof secret2,32U) == FVS_OK);
    CHECK(strlen(secret) == 64U);
    CHECK(strcmp(secret,secret2) != 0);

    char h[65];
    CHECK(fvs_sha256_hex("abc",h) == FVS_OK);
    CHECK(strcmp(h,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    CHECK(fvs_secure_equals(h,h) == 1);
    CHECK(fvs_secure_equals(h,"x") == 0);

    CHECK(fvs_currency_valid("MXN") == 1);
    CHECK(fvs_currency_valid("US") == 0);
    CHECK(fvs_provider_valid("stripe") == 1);
    CHECK(fvs_provider_valid("mercadopago") == 1);
    CHECK(fvs_provider_valid("paypal") == 0);
    CHECK(fvs_email_valid("guest@example.com") == 1);
    CHECK(fvs_email_valid("not-an-email") == 0);

    int64_t out = 0;
    CHECK(fvs_add_i64_checked(10,20,&out) == FVS_OK && out == 30);
    CHECK(fvs_add_i64_checked(INT64_MAX,1,&out) == FVS_ERR_OVERFLOW);
    CHECK(fvs_add_i64_checked(INT64_MIN,-1,&out) == FVS_ERR_OVERFLOW);

    char escaped[128];
    CHECK(fvs_json_escape("a\"b\\c\n",escaped,sizeof escaped) == FVS_OK);
    CHECK(strcmp(escaped,"a\\\"b\\\\c\\n") == 0);

    CHECK(strcmp(fvs_status_name(FVS_ERR_RETRY), "retry") == 0);
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("PASS core unit tests (%s)\n", fvs_version());
    return 0;
}
