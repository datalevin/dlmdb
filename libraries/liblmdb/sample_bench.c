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

typedef enum SampleMode {
	SAMPLE_WARM = 0,
	SAMPLE_SCAN = 1,
	SAMPLE_BOTH = 2
} SampleMode;

static void
usage(const char *prog)
{
		fprintf(stderr,
		    "Usage: %s [--entries N] [--samples M] [--stride S] [--batch B]\n"
		    "          [--dups D] [--mode warm|scan|both] [--path DIR] [--mapsize BYTES]\n"
		    "Default: entries=10000000 samples=1000 stride=entries/samples batch=250000 dups=1\n",
		    prog);
}

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0 +
	       (end->tv_nsec - start->tv_nsec) / 1.0e6;
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
populate_counted_db(const char *path, size_t entries, size_t batch,
    size_t mapsize, size_t dupcount, MDB_env **env_out, MDB_dbi *dbi_out)
{
	if (!entries)
		return 0.0;

	prepare_dir(path);

	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi = 0;
	CHECK(mdb_env_create(&env), "mdb_env_create");
	CHECK(mdb_env_set_maxdbs(env, 4), "mdb_env_set_maxdbs");
	if (mapsize)
		CHECK(mdb_env_set_mapsize(env, mapsize), "mdb_env_set_mapsize");
	unsigned env_flags = MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK;
	CHECK(mdb_env_open(env, path, env_flags, 0664), "mdb_env_open");

	CHECK(mdb_txn_begin(env, NULL, 0, &txn), "db open txn");
	unsigned db_flags = MDB_CREATE | MDB_COUNTED;
	if (dupcount > 1)
		db_flags |= MDB_DUPSORT;
	CHECK(mdb_dbi_open(txn, "sample", db_flags, &dbi),
	    "mdb_dbi_open sample");
	CHECK(mdb_txn_commit(txn), "db open commit");

	struct timespec start = {0}, end = {0};
	clock_gettime(CLOCK_MONOTONIC, &start);

	size_t inserted = 0;
	while (inserted < entries) {
		size_t limit = inserted + batch;
		if (limit > entries)
			limit = entries;

		CHECK(mdb_txn_begin(env, NULL, 0, &txn), "populate txn begin");
			for (; inserted < limit; ++inserted) {
				char keybuf[32];
				char databuf[32];
				size_t key_index = dupcount > 1 ? (inserted / dupcount) : inserted;
				size_t dup_index = dupcount > 1 ? (inserted % dupcount) : 0;
				int klen = snprintf(keybuf, sizeof(keybuf), "%020zu", key_index);
				int dlen;
				if (dupcount > 1)
					dlen = snprintf(databuf, sizeof(databuf), "%016zu-%04zu",
					    key_index, dup_index);
				else
					dlen = snprintf(databuf, sizeof(databuf), "%016zu", inserted);
				if (klen < 0 || dlen < 0) {
					fprintf(stderr, "snprintf failed during population\n");
					exit(EXIT_FAILURE);
				}
				MDB_val key = {(size_t)klen, keybuf};
				MDB_val data = {(size_t)dlen, databuf};
				unsigned put_flags = (dupcount > 1) ? 0 : MDB_APPEND;
				CHECK(mdb_put(txn, dbi, &key, &data, put_flags),
				    "mdb_put populate");
			}
			CHECK(mdb_txn_commit(txn), "populate txn commit");
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	*env_out = env;
	*dbi_out = dbi;

	return elapsed_ms(&start, &end);
}

static double
run_sampling(MDB_env *env, MDB_dbi dbi, size_t entries,
    size_t samples, size_t stride, size_t *actual_out)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cur = NULL;
	MDB_val key = {0}, data = {0};
	struct timespec start = {0}, end = {0};
	uint64_t rank = 0;
	size_t performed = 0;
	int rc;

	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "sample txn begin");
	CHECK(mdb_cursor_open(txn, dbi, &cur), "sample cursor open");

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (; performed < samples; ++performed) {
		if (rank >= entries)
			break;

		rc = mdb_cursor_get_rank(cur, rank, &key, &data, 0);
		if (rc == MDB_NOTFOUND)
			break;
		CHECK(rc, "mdb_cursor_get_rank sample");

		rank += stride;
		if (rank >= entries && performed + 1 < samples)
			break;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	if (actual_out)
		*actual_out = performed;

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);

	return elapsed_ms(&start, &end);
}

static double
run_stride_scan(MDB_env *env, MDB_dbi dbi, size_t entries,
    size_t samples, size_t stride, size_t *actual_out)
{
	MDB_txn *txn = NULL;
	MDB_cursor *cur = NULL;
	MDB_val key = {0}, data = {0};
	struct timespec start = {0}, end = {0};
	size_t performed = 0;

	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "scan txn begin");
	CHECK(mdb_cursor_open(txn, dbi, &cur), "scan cursor open");

	clock_gettime(CLOCK_MONOTONIC, &start);
	CHECK(mdb_cursor_get_rank(cur, 0, &key, &data, 0), "scan initial rank");
	performed = 1;

	while (performed < samples) {
		size_t steps = stride;
		while (steps--) {
			int rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
			if (rc == MDB_NOTFOUND) {
				goto done;
			}
			CHECK(rc, "scan MDB_NEXT");
		}
		performed++;
	}

done:
	clock_gettime(CLOCK_MONOTONIC, &end);
	if (actual_out)
		*actual_out = performed;
	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	return elapsed_ms(&start, &end);
}

int
main(int argc, char **argv)
{
	const char *path = "./bench_sample";
	size_t entries = 10000000;
	size_t samples = 1000;
	size_t stride = 0;
	size_t batch = 250000;
	size_t mapsize = (size_t)1 << 34; /* 16 GiB default */
	SampleMode mode = SAMPLE_BOTH;
	size_t dupcount = 1;

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--entries") && i + 1 < argc) {
			entries = (size_t)strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--samples") && i + 1 < argc) {
			samples = (size_t)strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--stride") && i + 1 < argc) {
			stride = (size_t)strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--batch") && i + 1 < argc) {
			batch = (size_t)strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--path") && i + 1 < argc) {
			path = argv[++i];
		} else if (!strcmp(argv[i], "--mapsize") && i + 1 < argc) {
			mapsize = (size_t)strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--dups") && i + 1 < argc) {
			dupcount = (size_t)strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			const char *m = argv[++i];
			if (!strcmp(m, "warm"))
				mode = SAMPLE_WARM;
			else if (!strcmp(m, "scan"))
				mode = SAMPLE_SCAN;
			else if (!strcmp(m, "both"))
				mode = SAMPLE_BOTH;
			else {
				fprintf(stderr, "Unknown mode '%s'\n", m);
				return EXIT_FAILURE;
			}
		} else {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!entries) {
		fprintf(stderr, "entries must be > 0\n");
		return EXIT_FAILURE;
	}
	if (!dupcount) {
		fprintf(stderr, "dups must be >= 1\n");
		return EXIT_FAILURE;
	}
	if (!samples) {
		fprintf(stderr, "samples must be > 0\n");
		return EXIT_FAILURE;
	}
	if (!batch)
		batch = 100000;
	if (!stride)
		stride = entries / samples ? entries / samples : 1;
	if (stride == 0)
		stride = 1;

	printf("Preparing %zu entries (batch %zu, stride %zu, samples %zu, dups %zu)\n",
	    entries, batch, stride, samples, dupcount);
	fflush(stdout);

	MDB_env *env = NULL;
	MDB_dbi dbi = 0;
	double populate_ms =
	    populate_counted_db(path, entries, batch, mapsize, dupcount, &env, &dbi);
	printf("Population: %.2f ms (%.2f entries/sec)\n",
	    populate_ms,
	    populate_ms > 0 ? (entries / (populate_ms / 1000.0)) : 0.0);
	fflush(stdout);

	if (!env) {
		fprintf(stderr, "environment setup failed\n");
		return EXIT_FAILURE;
	}

	if (mode == SAMPLE_WARM || mode == SAMPLE_BOTH) {
		size_t ops = 0;
		double ms = run_sampling(env, dbi, entries, samples, stride, &ops);
		double us_per = (ops ? (ms * 1000.0) / ops : 0.0);
		printf("Warm sequential: samples=%zu total=%.2f ms avg=%.2f us/op\n",
		    ops, ms, us_per);
	}

	if (mode == SAMPLE_SCAN || mode == SAMPLE_BOTH) {
		size_t ops = 0;
		double ms = run_stride_scan(env, dbi, entries, samples, stride, &ops);
		double us_per = (ops ? (ms * 1000.0) / ops : 0.0);
		printf("Stride scan (MDB_NEXT): samples=%zu total=%.2f ms avg=%.2f us/op\n",
		    ops, ms, us_per);
	}

	mdb_env_close(env);
	cleanup_dir(path);

	return EXIT_SUCCESS;
}
