#include "dlmdb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CHECK(rc, msg)                                                      \
	do {                                                                \
		if ((rc) != MDB_SUCCESS) {                                   \
			fprintf(stderr, "%s: %s\n", (msg), mdb_strerror(rc)); \
			exit(EXIT_FAILURE);                                   \
		}                                                             \
	} while (0)

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [--keys N] [--dups M] [--runs R] [--mapsize BYTES] "
	    "[--path DIR]\n"
	    "Defaults: keys=50000 dups=20 runs=5 mapsize=512MiB path=./dup_iter_bench_env\n",
	    prog);
}

static void
prepare_dir(const char *path)
{
	if (mkdir(path, 0775) && errno != EEXIST) {
		perror("mkdir benchmark env");
		exit(EXIT_FAILURE);
	}
	if (chmod(path, 0775) && errno != EPERM) {
		perror("chmod benchmark env");
	}
	char data_path[512];
	char lock_path[512];
	snprintf(data_path, sizeof(data_path), "%s/data.mdb", path);
	snprintf(lock_path, sizeof(lock_path), "%s/lock.mdb", path);
	unlink(data_path);
	unlink(lock_path);
}

static void
cleanup_dir(const char *path)
{
	char data_path[512];
	char lock_path[512];
	snprintf(data_path, sizeof(data_path), "%s/data.mdb", path);
	snprintf(lock_path, sizeof(lock_path), "%s/lock.mdb", path);
	unlink(data_path);
	unlink(lock_path);
	if (rmdir(path) && errno != ENOENT) {
		perror("rmdir benchmark env");
	}
}

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0 +
	       (end->tv_nsec - start->tv_nsec) / 1.0e6;
}

static void
populate_db(MDB_env *env, MDB_dbi dbi, size_t keys, size_t dups)
{
	MDB_txn *txn = NULL;
	size_t total = keys * dups;
	size_t inserted = 0;

	while (inserted < total) {
		CHECK(mdb_txn_begin(env, NULL, 0, &txn), "populate txn begin");
		for (size_t batch = 0; batch < 4096 && inserted < total; ++batch, ++inserted) {
			size_t key_index = inserted / dups;
			size_t dup_index = inserted % dups;
			char keybuf[32];
			char valbuf[64];
			int klen = snprintf(keybuf, sizeof(keybuf), "%020zu", key_index);
			int vlen = snprintf(valbuf, sizeof(valbuf), "%016zu-value-%04zu",
			    key_index, dup_index);
			if (klen < 0 || vlen < 0) {
				fprintf(stderr, "snprintf failed during population\n");
				exit(EXIT_FAILURE);
			}
			MDB_val key = {(size_t)klen, keybuf};
			MDB_val val = {(size_t)vlen, valbuf};
			CHECK(mdb_put(txn, dbi, &key, &val, 0), "mdb_put populate");
		}
		CHECK(mdb_txn_commit(txn), "populate txn commit");
	}
}

static double
bench_list_dup(MDB_env *env, MDB_dbi dbi, size_t runs, volatile size_t *sink)
{
	struct timespec start = {0}, end = {0};
	size_t checksum = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (size_t iter = 0; iter < runs; ++iter) {
		MDB_txn *txn = NULL;
		MDB_cursor *cur = NULL;
		MDB_val key = {0}, data = {0};
		CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "list_dup txn");
		CHECK(mdb_cursor_open(txn, dbi, &cur), "list_dup cursor");
		int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
		while (rc == MDB_SUCCESS) {
			const MDB_val *vals = NULL;
			mdb_size_t count = 0;
			CHECK(mdb_cursor_list_dup(cur, &vals, &count), "mdb_cursor_list_dup");
			for (mdb_size_t i = 0; i < count; ++i) {
				const unsigned char *p = (const unsigned char *)vals[i].mv_data;
				checksum += p[0];
			}
			rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_NODUP);
		}
		CHECK(rc == MDB_NOTFOUND ? MDB_SUCCESS : rc, "list dup iteration");
		mdb_cursor_close(cur);
		mdb_txn_abort(txn);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	*sink += checksum;
	return elapsed_ms(&start, &end);
}

static double
bench_next_dup(MDB_env *env, MDB_dbi dbi, size_t runs, volatile size_t *sink)
{
	struct timespec start = {0}, end = {0};
	size_t checksum = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (size_t iter = 0; iter < runs; ++iter) {
		MDB_txn *txn = NULL;
		MDB_cursor *cur = NULL;
		MDB_val key = {0}, data = {0};
		CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "next_dup txn");
		CHECK(mdb_cursor_open(txn, dbi, &cur), "next_dup cursor");
		int rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
		while (rc == MDB_SUCCESS) {
			CHECK(mdb_cursor_get(cur, &key, &data, MDB_FIRST_DUP), "first dup");
			do {
				const unsigned char *p = (const unsigned char *)data.mv_data;
				checksum += p[0];
				rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_DUP);
			} while (rc == MDB_SUCCESS);
			if (rc != MDB_NOTFOUND) {
				CHECK(rc, "next dup loop");
			}
			rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT_NODUP);
		}
		CHECK(rc == MDB_NOTFOUND ? MDB_SUCCESS : rc, "next dup iteration");
		mdb_cursor_close(cur);
		mdb_txn_abort(txn);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	*sink += checksum;
	return elapsed_ms(&start, &end);
}

int
main(int argc, char **argv)
{
	size_t keys = 50000;
	size_t dups = 20;
	size_t runs = 5;
	size_t mapsize = (size_t)512 << 20;
	const char *path = "./dup_iter_bench_env";

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
			keys = strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--dups") && i + 1 < argc) {
			dups = strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--runs") && i + 1 < argc) {
			runs = strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--mapsize") && i + 1 < argc) {
			mapsize = strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--path") && i + 1 < argc) {
			path = argv[++i];
		} else if (!strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		} else {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!keys || !dups || !runs) {
		fprintf(stderr, "keys, dups, and runs must be non-zero\n");
		return EXIT_FAILURE;
	}

	prepare_dir(path);

	MDB_env *env = NULL;
	MDB_dbi dbi = 0;
	MDB_txn *txn = NULL;

	CHECK(mdb_env_create(&env), "mdb_env_create");
	CHECK(mdb_env_set_maxdbs(env, 4), "mdb_env_set_maxdbs");
	if (mapsize)
		CHECK(mdb_env_set_mapsize(env, mapsize), "mdb_env_set_mapsize");
	unsigned env_flags = MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK;
	CHECK(mdb_env_open(env, path, env_flags, 0664), "mdb_env_open");

	CHECK(mdb_txn_begin(env, NULL, 0, &txn), "dbi open txn");
	unsigned db_flags = MDB_CREATE | MDB_DUPSORT | MDB_COUNTED | MDB_PREFIX_COMPRESSION;
	CHECK(mdb_dbi_open(txn, "bench", db_flags, &dbi), "mdb_dbi_open bench");
	CHECK(mdb_txn_commit(txn), "dbi open commit");

	populate_db(env, dbi, keys, dups);

	volatile size_t sink = 0;
	double list_ms = bench_list_dup(env, dbi, runs, &sink);
	double next_ms = bench_next_dup(env, dbi, runs, &sink);

	size_t total_keys = keys * runs;
	size_t total_values = keys * dups * runs;

	printf("Benchmark configuration: keys=%zu dups/key=%zu runs=%zu\n",
	    keys, dups, runs);
	printf("Total key visits: %zu, total duplicates read: %zu\n",
	    total_keys, total_values);
	printf("mdb_cursor_list_dup: %.3f ms (%.3f us/key, %.3f ns/value)\n",
	    list_ms, (list_ms * 1000.0) / total_keys,
	    (list_ms * 1.0e6) / total_values);
	printf("MDB_NEXT_DUP loop:   %.3f ms (%.3f us/key, %.3f ns/value)\n",
	    next_ms, (next_ms * 1000.0) / total_keys,
	    (next_ms * 1.0e6) / total_values);
	printf("Checksum sink: %zu (ignore, prevents DCE)\n", sink);

	mdb_env_close(env);
	cleanup_dir(path);
	return EXIT_SUCCESS;
}
