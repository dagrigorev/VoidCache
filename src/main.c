/*
 * src/main.c  –  VoidCache smoke test & demo
 */
#define _POSIX_C_SOURCE 200809L
#include "voidcache.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("VoidCache – smoke test\n\n");

    /* Create: 256 MB soft cap, no WAL, 4096 slots/shard */
    vc_cache_t *c = vc_create(256ULL * 1024 * 1024, NULL, 4096);
    assert(c);

    /* SET */
    assert(vc_set(c, "greeting", 8, "hello, world", 12, 0) == VC_OK);
    assert(vc_set(c, "counter",  7, "\x00\x00\x00\x01", 4, 0) == VC_OK);
    assert(vc_set(c, "ttl_key",  7, "expires soon", 12, 2) == VC_OK);

    /* GET */
    const void *v; size_t vl;
    assert(vc_get(c, "greeting", 8, &v, &vl) == VC_OK);
    printf("greeting  = \"%.*s\"\n", (int)vl, (const char *)v);

    /* DEL */
    assert(vc_del(c, "counter", 7) == VC_OK);
    assert(vc_get(c, "counter", 7, NULL, NULL) == VC_ERR_NOTFND);
    printf("counter   deleted OK\n");

    /* Missing key */
    assert(vc_get(c, "nope", 4, NULL, NULL) == VC_ERR_NOTFND);
    printf("nope      not found  OK\n");

    /* Overwrite */
    assert(vc_set(c, "greeting", 8, "OVERWRITTEN", 11, 0) == VC_OK);
    assert(vc_get(c, "greeting", 8, &v, &vl) == VC_OK);
    printf("greeting  = \"%.*s\" (after overwrite)\n", (int)vl, (const char *)v);

    printf("\n");
    vc_stats_print(c);
    vc_destroy(c);
    printf("\nAll assertions passed.\n");
    return 0;
}
