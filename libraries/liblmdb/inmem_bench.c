#include "lmdb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct bench_cfg {
	size_t entries;
	size_t value_size;
	size_t rounds;
	size_t mapsize;
	int no_lock;
} bench_cfg;

typedef struct bench_result {
	double write_ms;
	double read_ms;
	uint64_t checksum;
} bench_result;

#define CHECK_RC(rc, msg)                                                     \
	do {                                                                  \
		int _rc = (rc);                                               \
		if (_rc != MDB_SUCCESS) {                                     \
			fprintf(stderr, "%s failed: %s\n", (msg),             \
			    mdb_strerror(_rc));                                  \
			exit(EXIT_FAILURE);                                       \
		}                                                             \
	} while (0)

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0 +
	       (end->tv_nsec - start->tv_nsec) / 1.0e6;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [--entries N] [--value-size N] [--rounds N] [--mapsize BYTES] [--no-lock]\n"
	    "Defaults: entries=500000 value-size=64 rounds=3 mapsize=auto\n",
	    prog);
}

static uint64_t
parse_u64(const char *arg, const char *name)
{
	char *end = NULL;
	unsigned long long v = strtoull(arg, &end, 10);
	if (!arg[0] || (end && *end)) {
		fprintf(stderr, "Invalid %s: %s\n", name, arg);
		exit(EXIT_FAILURE);
	}
	return (uint64_t)v;
}

static void
parse_args(bench_cfg *cfg, int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--entries") && i + 1 < argc) {
			cfg->entries = (size_t)parse_u64(argv[++i], "entries");
		} else if (!strcmp(argv[i], "--value-size") && i + 1 < argc) {
			cfg->value_size = (size_t)parse_u64(argv[++i], "value-size");
		} else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) {
			cfg->rounds = (size_t)parse_u64(argv[++i], "rounds");
		} else if (!strcmp(argv[i], "--mapsize") && i + 1 < argc) {
			cfg->mapsize = (size_t)parse_u64(argv[++i], "mapsize");
		} else if (!strcmp(argv[i], "--no-lock")) {
			cfg->no_lock = 1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else {
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (!cfg->entries || !cfg->value_size || !cfg->rounds) {
		fprintf(stderr, "entries, value-size, and rounds must be > 0\n");
		exit(EXIT_FAILURE);
	}
	if (!cfg->mapsize) {
		size_t per_entry = cfg->value_size + 128;
		cfg->mapsize = cfg->entries * per_entry * 2;
		if (cfg->mapsize < (1u << 20))
			cfg->mapsize = (1u << 20);
	}
}

static void
make_temp_path(char *buf, size_t buflen)
{
	char tmpl[] = "inmem-bench-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) {
		perror("mkstemp");
		exit(EXIT_FAILURE);
	}
	close(fd);
	unlink(tmpl);
	snprintf(buf, buflen, "%s", tmpl);
}

static void
cleanup_file_env(const char *path)
{
	char lock_path[512];
	unlink(path);
	snprintf(lock_path, sizeof(lock_path), "%s-lock", path);
	unlink(lock_path);
}

static void
run_case(const bench_cfg *cfg, unsigned int env_flags, const char *path,
    bench_result *out)
{
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi = 0;
	MDB_val key, data;
	struct timespec t0, t1;
	uint8_t *vbuf = NULL;
	size_t i;

	vbuf = malloc(cfg->value_size);
	if (!vbuf) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	memset(vbuf, 0, cfg->value_size);

	CHECK_RC(mdb_env_create(&env), "mdb_env_create");
	CHECK_RC(mdb_env_set_maxdbs(env, 2), "mdb_env_set_maxdbs");
	CHECK_RC(mdb_env_set_maxreaders(env, 64), "mdb_env_set_maxreaders");
	CHECK_RC(mdb_env_set_mapsize(env, cfg->mapsize), "mdb_env_set_mapsize");
	CHECK_RC(mdb_env_open(env, path, env_flags, 0664), "mdb_env_open");

	CHECK_RC(mdb_txn_begin(env, NULL, 0, &txn), "mdb_txn_begin write");
	CHECK_RC(mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi), "mdb_dbi_open");

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < cfg->entries; i++) {
		uint64_t k = (uint64_t)i;
		vbuf[0] = (uint8_t)(i & 0xff);
		key.mv_data = &k;
		key.mv_size = sizeof(k);
		data.mv_data = vbuf;
		data.mv_size = cfg->value_size;
		CHECK_RC(mdb_put(txn, dbi, &key, &data, 0), "mdb_put");
	}
	CHECK_RC(mdb_txn_commit(txn), "mdb_txn_commit write");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	out->write_ms = elapsed_ms(&t0, &t1);

	CHECK_RC(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), "mdb_txn_begin read");
	out->checksum = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < cfg->entries; i++) {
		uint64_t k = (uint64_t)i;
		key.mv_data = &k;
		key.mv_size = sizeof(k);
		CHECK_RC(mdb_get(txn, dbi, &key, &data), "mdb_get");
		out->checksum += ((const uint8_t *)data.mv_data)[0];
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	out->read_ms = elapsed_ms(&t0, &t1);
	mdb_txn_abort(txn);

	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	free(vbuf);
}

static double
avg_ms(double total, size_t rounds)
{
	return total / (double)rounds;
}

int
main(int argc, char **argv)
{
	bench_cfg cfg = {
		.entries = 500000,
		.value_size = 64,
		.rounds = 3,
		.mapsize = 0,
		.no_lock = 0
	};
	bench_result file_res = {0}, inmem_res = {0};
	double file_write_total = 0.0, file_read_total = 0.0;
	double inmem_write_total = 0.0, inmem_read_total = 0.0;
	size_t r;

	parse_args(&cfg, argc, argv);

	for (r = 0; r < cfg.rounds; r++) {
		char path[512];
		unsigned int file_flags = MDB_NOSUBDIR;
		if (cfg.no_lock)
			file_flags |= MDB_NOLOCK;
		make_temp_path(path, sizeof(path));
		run_case(&cfg, file_flags, path, &file_res);
		cleanup_file_env(path);
		file_write_total += file_res.write_ms;
		file_read_total += file_res.read_ms;

		run_case(&cfg, MDB_INMEMORY, NULL, &inmem_res);
		inmem_write_total += inmem_res.write_ms;
		inmem_read_total += inmem_res.read_ms;
	}

	printf("entries=%zu value_size=%zu rounds=%zu mapsize=%zu\n",
	    cfg.entries, cfg.value_size, cfg.rounds, cfg.mapsize);
	printf("file-backed avg: write=%.2f ms read=%.2f ms checksum=%" PRIu64 "\n",
	    avg_ms(file_write_total, cfg.rounds),
	    avg_ms(file_read_total, cfg.rounds),
	    file_res.checksum);
	printf("in-memory  avg: write=%.2f ms read=%.2f ms checksum=%" PRIu64 "\n",
	    avg_ms(inmem_write_total, cfg.rounds),
	    avg_ms(inmem_read_total, cfg.rounds),
	    inmem_res.checksum);
	printf("speedup: write=%.2fx read=%.2fx\n",
	    avg_ms(file_write_total, cfg.rounds) / avg_ms(inmem_write_total, cfg.rounds),
	    avg_ms(file_read_total, cfg.rounds) / avg_ms(inmem_read_total, cfg.rounds));

	return 0;
}
