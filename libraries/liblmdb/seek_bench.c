/*
 * Benchmark for cursor forward seek optimization.
 *
 * Tests different seek patterns:
 * - Sequential: seek to key+1 each time
 * - Skip-N: seek to key+N each time (tests forward seek across pages)
 * - Random: seek to random keys (baseline comparison)
 *
 * Usage: ./seek_bench [num_entries] [value_size]
 *   num_entries: number of entries to insert (default: 500000)
 *   value_size: size of each value in bytes (default: 128)
 */

#include "dlmdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#define DB_PATH "./testdb_seek_bench"

#define CHECK(rc, msg) do { \
    if ((rc) != MDB_SUCCESS) { \
        fprintf(stderr, "%s: %s\n", (msg), mdb_strerror(rc)); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static double
now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Simple xorshift64 PRNG for reproducible random seeks */
static uint64_t rng_state = 0x123456789ABCDEF0ULL;

static uint64_t
xorshift64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static void
rng_seed(uint64_t seed)
{
    rng_state = seed ? seed : 0x123456789ABCDEF0ULL;
}

/*
 * Benchmark: sequential seeks (key + 1)
 * The cursor is already positioned, seek to next key.
 * This tests the common case of iterating with occasional re-seeks.
 */
static double
bench_sequential_seek(MDB_cursor *cursor, uint64_t num_keys, int iterations)
{
    MDB_val key, data;
    uint64_t k;
    int rc;
    double start, elapsed;

    start = now_ms();
    for (int iter = 0; iter < iterations; iter++) {
        /* Start from first key */
        k = 0;
        key.mv_size = sizeof(k);
        key.mv_data = &k;
        rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
            CHECK(rc, "sequential seek init");
        }

        /* Seek through all keys sequentially */
        for (uint64_t i = 1; i < num_keys; i++) {
            k = i;
            key.mv_size = sizeof(k);
            key.mv_data = &k;
            rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
            if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                CHECK(rc, "sequential seek");
            }
        }
    }
    elapsed = now_ms() - start;
    return elapsed / iterations;
}

/*
 * Benchmark: skip seeks (key + skip)
 * Seek forward by 'skip' keys each time.
 * This tests forward seek across multiple pages.
 */
static double
bench_skip_seek(MDB_cursor *cursor, uint64_t num_keys, int skip, int iterations)
{
    MDB_val key, data;
    uint64_t k;
    int rc;
    double start, elapsed;
    int seeks_per_iter = (int)(num_keys / skip);

    start = now_ms();
    for (int iter = 0; iter < iterations; iter++) {
        /* Start from first key */
        k = 0;
        key.mv_size = sizeof(k);
        key.mv_data = &k;
        rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
            CHECK(rc, "skip seek init");
        }

        /* Seek forward by skip each time */
        for (int i = 1; i < seeks_per_iter; i++) {
            k = (uint64_t)i * skip;
            key.mv_size = sizeof(k);
            key.mv_data = &k;
            rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
            if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                CHECK(rc, "skip seek");
            }
        }
    }
    elapsed = now_ms() - start;
    return elapsed / iterations;
}

/*
 * Benchmark: random seeks
 * Seek to random keys. This is the baseline where forward seek doesn't help.
 */
static double
bench_random_seek(MDB_cursor *cursor, uint64_t num_keys, int num_seeks, int iterations)
{
    MDB_val key, data;
    uint64_t k;
    int rc;
    double start, elapsed;

    start = now_ms();
    for (int iter = 0; iter < iterations; iter++) {
        rng_seed(42);  /* Reset RNG for reproducibility */
        for (int i = 0; i < num_seeks; i++) {
            k = xorshift64() % num_keys;
            key.mv_size = sizeof(k);
            key.mv_data = &k;
            rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
            if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                CHECK(rc, "random seek");
            }
        }
    }
    elapsed = now_ms() - start;
    return elapsed / iterations;
}

/*
 * Benchmark: forward random seeks (sorted random keys)
 * Generate random keys, sort them, then seek in order.
 * This tests forward seek with varying distances.
 */
static double
bench_forward_random_seek(MDB_cursor *cursor, uint64_t num_keys, int num_seeks, int iterations)
{
    MDB_val key, data;
    uint64_t k;
    int rc;
    double start, elapsed;

    /* Generate and sort random keys */
    uint64_t *keys = malloc(num_seeks * sizeof(uint64_t));
    if (!keys) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }

    rng_seed(42);
    for (int i = 0; i < num_seeks; i++) {
        keys[i] = xorshift64() % num_keys;
    }

    /* Simple insertion sort (good enough for benchmark) */
    for (int i = 1; i < num_seeks; i++) {
        uint64_t tmp = keys[i];
        int j = i - 1;
        while (j >= 0 && keys[j] > tmp) {
            keys[j + 1] = keys[j];
            j--;
        }
        keys[j + 1] = tmp;
    }

    start = now_ms();
    for (int iter = 0; iter < iterations; iter++) {
        /* Position at first key */
        k = keys[0];
        key.mv_size = sizeof(k);
        key.mv_data = &k;
        rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
            CHECK(rc, "forward random seek init");
        }

        /* Seek through sorted keys */
        for (int i = 1; i < num_seeks; i++) {
            k = keys[i];
            key.mv_size = sizeof(k);
            key.mv_data = &k;
            rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
            if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                CHECK(rc, "forward random seek");
            }
        }
    }
    elapsed = now_ms() - start;

    free(keys);
    return elapsed / iterations;
}

int
main(int argc, char *argv[])
{
    MDB_env *env;
    MDB_txn *txn;
    MDB_dbi dbi;
    MDB_cursor *cursor;
    MDB_val key, data;
    int rc;
    uint64_t num_entries = 500000;
    size_t value_size = 128;
    char *value_buf;
    double elapsed;

    if (argc > 1) {
        num_entries = strtoull(argv[1], NULL, 10);
    }
    if (argc > 2) {
        value_size = strtoul(argv[2], NULL, 10);
    }

    printf("Forward Seek Benchmark\n");
    printf("======================\n");
    printf("Entries: %" PRIu64 "\n", num_entries);
    printf("Value size: %zu bytes\n\n", value_size);

    /* Create value buffer */
    value_buf = malloc(value_size);
    if (!value_buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    memset(value_buf, 'x', value_size);

    /* Clean up old test db */
    system("rm -rf " DB_PATH);
    mkdir(DB_PATH, 0755);

    /* Create environment */
    CHECK(mdb_env_create(&env), "mdb_env_create");
    CHECK(mdb_env_set_mapsize(env, (size_t)num_entries * (value_size + 32) * 2), "mdb_env_set_mapsize");
    CHECK(mdb_env_open(env, DB_PATH, MDB_NOSYNC, 0664), "mdb_env_open");

    /* Populate database */
    printf("Populating database...\n");
    CHECK(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin");
    CHECK(mdb_dbi_open(txn, NULL, MDB_CREATE | MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi), "mdb_dbi_open");

    double pop_start = now_ms();
    for (uint64_t i = 0; i < num_entries; i++) {
        key.mv_size = sizeof(i);
        key.mv_data = &i;
        data.mv_size = value_size;
        data.mv_data = value_buf;
        rc = mdb_put(txn, dbi, &key, &data, 0);
        CHECK(rc, "mdb_put");
    }
    CHECK(mdb_txn_commit(txn), "mdb_txn_commit");
    printf("Population time: %.1f ms\n\n", now_ms() - pop_start);

    /* Get database stats */
    CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "mdb_txn_begin");
    MDB_stat stat;
    CHECK(mdb_stat(txn, dbi, &stat), "mdb_stat");
    printf("Database stats:\n");
    printf("  Tree depth: %u\n", stat.ms_depth);
    printf("  Branch pages: %zu\n", stat.ms_branch_pages);
    printf("  Leaf pages: %zu\n", stat.ms_leaf_pages);
    printf("  Entries: %zu\n\n", stat.ms_entries);

    /* Open cursor for benchmarks */
    CHECK(mdb_cursor_open(txn, dbi, &cursor), "mdb_cursor_open");

    printf("Running benchmarks...\n\n");

    /* Sequential seek benchmark */
    int seq_iters = 3;
    printf("Sequential seek (key+1):\n");
    elapsed = bench_sequential_seek(cursor, num_entries > 100000 ? 100000 : num_entries, seq_iters);
    printf("  Time: %.1f ms (%" PRIu64 " seeks)\n\n",
           elapsed, num_entries > 100000 ? 100000ULL : num_entries);

    /* Skip seek benchmarks */
    int skip_iters = 10;
    int skips[] = {10, 50, 100, 500, 1000};
    printf("Skip seek (key+N):\n");
    for (size_t i = 0; i < sizeof(skips)/sizeof(skips[0]); i++) {
        int skip = skips[i];
        elapsed = bench_skip_seek(cursor, num_entries, skip, skip_iters);
        int num_seeks = (int)(num_entries / skip);
        printf("  Skip %4d: %6.1f ms (%d seeks, %.2f us/seek)\n",
               skip, elapsed, num_seeks, elapsed * 1000.0 / num_seeks);
    }
    printf("\n");

    /* Random seek benchmark (baseline) */
    int rand_seeks = 10000;
    int rand_iters = 5;
    printf("Random seek (%d seeks):\n", rand_seeks);
    elapsed = bench_random_seek(cursor, num_entries, rand_seeks, rand_iters);
    printf("  Time: %.1f ms (%.2f us/seek)\n\n", elapsed, elapsed * 1000.0 / rand_seeks);

    /* Forward random seek benchmark */
    printf("Forward random seek (%d sorted seeks):\n", rand_seeks);
    elapsed = bench_forward_random_seek(cursor, num_entries, rand_seeks, rand_iters);
    printf("  Time: %.1f ms (%.2f us/seek)\n\n", elapsed, elapsed * 1000.0 / rand_seeks);

    /* Cleanup */
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    mdb_dbi_close(env, dbi);
    mdb_env_close(env);

    /*
     * ============================================
     * DUPSORT Database Benchmark
     * ============================================
     */
    printf("=====================================\n");
    printf("DUPSORT Database Benchmark\n");
    printf("=====================================\n\n");

    /* Clean up old test db */
    system("rm -rf " DB_PATH "_dup");
    mkdir(DB_PATH "_dup", 0755);

    /* Create environment for DUPSORT */
    CHECK(mdb_env_create(&env), "mdb_env_create dup");
    CHECK(mdb_env_set_mapsize(env, (size_t)num_entries * (value_size + 32) * 2), "mdb_env_set_mapsize dup");
    CHECK(mdb_env_open(env, DB_PATH "_dup", MDB_NOSYNC, 0664), "mdb_env_open dup");

    /* Populate DUPSORT database: same number of keys, use larger dup values */
    uint64_t num_keys = num_entries;        /* e.g., 500000 keys */
    uint64_t num_dups = 1;                  /* 1 dup per key */

    printf("Populating DUPSORT database...\n");
    printf("  Keys: %" PRIu64 ", Dups per key: %" PRIu64 "\n", num_keys, num_dups);

    CHECK(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin dup");
    CHECK(mdb_dbi_open(txn, NULL, MDB_CREATE | MDB_DUPSORT | MDB_COUNTED | MDB_PREFIX_COMPRESSION, &dbi), "mdb_dbi_open dup");

    pop_start = now_ms();
    for (uint64_t k = 0; k < num_keys; k++) {
        key.mv_size = sizeof(k);
        key.mv_data = &k;
        for (uint64_t d = 0; d < num_dups; d++) {
            /* Use same size values as regular DB for fair comparison */
            data.mv_size = value_size;
            data.mv_data = value_buf;
            rc = mdb_put(txn, dbi, &key, &data, 0);
            CHECK(rc, "mdb_put dup");
        }
    }
    CHECK(mdb_txn_commit(txn), "mdb_txn_commit dup");
    printf("  Population time: %.1f ms\n\n", now_ms() - pop_start);

    /* Get database stats */
    CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "mdb_txn_begin dup");
    CHECK(mdb_stat(txn, dbi, &stat), "mdb_stat dup");
    printf("Database stats:\n");
    printf("  Tree depth: %u\n", stat.ms_depth);
    printf("  Branch pages: %zu\n", stat.ms_branch_pages);
    printf("  Leaf pages: %zu\n", stat.ms_leaf_pages);
    printf("  Entries: %zu\n\n", stat.ms_entries);

    CHECK(mdb_cursor_open(txn, dbi, &cursor), "mdb_cursor_open dup");

    printf("Running DUPSORT benchmarks...\n\n");

    /* DUPSORT: Sequential key seek */
    printf("Sequential key seek (key+1):\n");
    {
        double start = now_ms();
        int iters = 10;
        for (int iter = 0; iter < iters; iter++) {
            for (uint64_t k = 0; k < num_keys; k++) {
                key.mv_size = sizeof(k);
                key.mv_data = &k;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    CHECK(rc, "dup sequential seek");
                }
            }
        }
        elapsed = (now_ms() - start) / iters;
        printf("  Time: %.2f ms (%" PRIu64 " seeks, %.2f us/seek)\n\n",
               elapsed, num_keys, elapsed * 1000.0 / num_keys);
    }

    /* DUPSORT: Skip key seek */
    printf("Skip key seek (key+10):\n");
    {
        double start = now_ms();
        int iters = 50;
        int skip = 10;
        int seeks_per_iter = (int)(num_keys / skip);
        for (int iter = 0; iter < iters; iter++) {
            for (int i = 0; i < seeks_per_iter; i++) {
                uint64_t k = (uint64_t)i * skip;
                key.mv_size = sizeof(k);
                key.mv_data = &k;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    CHECK(rc, "dup skip seek");
                }
            }
        }
        elapsed = (now_ms() - start) / iters;
        printf("  Time: %.2f ms (%d seeks, %.2f us/seek)\n\n",
               elapsed, seeks_per_iter, elapsed * 1000.0 / seeks_per_iter);
    }

    /* DUPSORT: MDB_GET_BOTH - sequential seek key+data */
    printf("MDB_GET_BOTH sequential (key+1):\n");
    {
        double start = now_ms();
        int iters = 10;
        for (int iter = 0; iter < iters; iter++) {
            for (uint64_t k = 0; k < num_keys; k++) {
                key.mv_size = sizeof(k);
                key.mv_data = &k;
                data.mv_size = value_size;
                data.mv_data = value_buf;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    CHECK(rc, "dup get_both");
                }
            }
        }
        elapsed = (now_ms() - start) / iters;
        printf("  Time: %.2f ms (%" PRIu64 " seeks, %.2f us/seek)\n\n",
               elapsed, num_keys, elapsed * 1000.0 / num_keys);
    }

    /* DUPSORT: MDB_GET_BOTH - skip seek key+data */
    printf("MDB_GET_BOTH skip 10 (key+10):\n");
    {
        double start = now_ms();
        int iters = 50;
        int skip = 10;
        int seeks_per_iter = (int)(num_keys / skip);
        for (int iter = 0; iter < iters; iter++) {
            for (int i = 0; i < seeks_per_iter; i++) {
                uint64_t k = (uint64_t)i * skip;
                key.mv_size = sizeof(k);
                key.mv_data = &k;
                data.mv_size = value_size;
                data.mv_data = value_buf;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    CHECK(rc, "dup get_both skip");
                }
            }
        }
        elapsed = (now_ms() - start) / iters;
        printf("  Time: %.2f ms (%d seeks, %.2f us/seek)\n\n",
               elapsed, seeks_per_iter, elapsed * 1000.0 / seeks_per_iter);
    }

    /* DUPSORT: MDB_GET_BOTH_RANGE - seek key + dup range */
    printf("MDB_GET_BOTH_RANGE (seek key + dup range):\n");
    {
        double start = now_ms();
        int iters = 10;
        for (int iter = 0; iter < iters; iter++) {
            for (uint64_t k = 0; k < num_keys; k++) {
                key.mv_size = sizeof(k);
                key.mv_data = &k;
                data.mv_size = value_size;
                data.mv_data = value_buf;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH_RANGE);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    CHECK(rc, "dup get_both_range");
                }
            }
        }
        elapsed = (now_ms() - start) / iters;
        printf("  Time: %.2f ms (%" PRIu64 " seeks, %.2f us/seek)\n\n",
               elapsed, num_keys, elapsed * 1000.0 / num_keys);
    }

    /* DUPSORT: MDB_GET_BOTH_RANGE - skip seek key+data */
    printf("MDB_GET_BOTH_RANGE skip 10 (key+10):\n");
    {
        double start = now_ms();
        int iters = 50;
        int skip = 10;
        uint64_t seeks_per_iter = num_keys / skip;
        for (int iter = 0; iter < iters; iter++) {
            for (uint64_t i = 0; i < seeks_per_iter; i++) {
                uint64_t k = i * skip;
                key.mv_size = sizeof(k);
                key.mv_data = &k;
                data.mv_size = value_size;
                data.mv_data = value_buf;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_GET_BOTH_RANGE);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    CHECK(rc, "dup get_both_range skip");
                }
            }
        }
        elapsed = (now_ms() - start) / iters;
        printf("  Time: %.2f ms (%" PRIu64 " seeks, %.2f us/seek)\n\n",
               elapsed, seeks_per_iter, elapsed * 1000.0 / seeks_per_iter);
    }

    /* DUPSORT: Forward random key seek (sorted) */
    printf("Forward random key seek (%d sorted seeks):\n", rand_seeks);
    {
        uint64_t *keys_arr = malloc(rand_seeks * sizeof(uint64_t));
        rng_seed(42);
        for (int i = 0; i < rand_seeks; i++) {
            keys_arr[i] = xorshift64() % num_keys;
        }
        /* Sort */
        for (int i = 1; i < rand_seeks; i++) {
            uint64_t tmp = keys_arr[i];
            int j = i - 1;
            while (j >= 0 && keys_arr[j] > tmp) {
                keys_arr[j + 1] = keys_arr[j];
                j--;
            }
            keys_arr[j + 1] = tmp;
        }

        double start = now_ms();
        int iters = 5;
        for (int iter = 0; iter < iters; iter++) {
            uint64_t k = keys_arr[0];
            key.mv_size = sizeof(k);
            key.mv_data = &k;
            mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);

            for (int i = 1; i < rand_seeks; i++) {
                k = keys_arr[i];
                key.mv_size = sizeof(k);
                key.mv_data = &k;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    CHECK(rc, "dup forward random");
                }
            }
        }
        elapsed = (now_ms() - start) / iters;
        printf("  Time: %.2f ms (%.2f us/seek)\n\n", elapsed, elapsed * 1000.0 / rand_seeks);
        free(keys_arr);
    }

    /* Cleanup */
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    mdb_dbi_close(env, dbi);
    mdb_env_close(env);
    free(value_buf);

    printf("Done.\n");
    return 0;
}
