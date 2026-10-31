#include "dlmdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#define CHECK(call)                                                            \
	do {                                                                   \
		int rc_ = (call);                                              \
		if (rc_) {                                                     \
			fprintf(stderr, "%s: %s\n", #call, mdb_strerror(rc_)); \
			exit(1);                                               \
		}                                                              \
	} while (0)
static void
be32(unsigned char *p, unsigned n) {
	for (int i = 3; i >= 0; i--, n >>= 8)
		p[i] = (unsigned char)n;
}
static unsigned
aid(const MDB_val *v) {
	const unsigned char *p = v->mv_data;
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
	       ((unsigned)p[2] << 8) | p[3];
}
static double
now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
int
main(int argc, char **argv) {
	unsigned keys = argc > 1 ? (unsigned)atoi(argv[1]) : 100000;
	unsigned dups = argc > 2 ? (unsigned)atoi(argv[2]) : 20;
	unsigned width = argc > 3 ? (unsigned)atoi(argv[3]) : 96;
	unsigned selected = argc > 4 ? (unsigned)atoi(argv[4]) : 7;
	int compressed = argc > 5 ? atoi(argv[5]) : 1;
	if (!keys || !dups || !selected || selected > dups || width < 5 ||
	    width > 511)
		return 2;
	MDB_env *env;
	MDB_txn *txn;
	MDB_dbi dbi;
	CHECK(mdb_env_create(&env));
	CHECK(mdb_env_set_mapsize(env, (size_t)1024 * 1024 * 1024));
	CHECK(mdb_env_open(env, NULL, MDB_INMEMORY | MDB_NOTLS, 0600));
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK(mdb_dbi_open(txn, NULL,
			   MDB_DUPSORT | MDB_COUNTED |
			       (compressed ? MDB_PREFIX_COMPRESSION : 0),
			   &dbi));
	unsigned char kb[4], vb[511];
	for (unsigned e = 1; e <= keys; e++)
		for (unsigned a = 1; a <= dups; a++) {
			be32(kb, e);
			be32(vb, a);
			memset(vb + 4, 'a' + a % 26, width - 4);
			MDB_val k = {4, kb}, v = {width, vb};
			CHECK(mdb_put(txn, dbi, &k, &v, 0));
		}
	CHECK(mdb_txn_commit(txn));
	MDB_val *prefixes = calloc(selected, sizeof(MDB_val)),
		*values = calloc(selected, sizeof(MDB_val));
	unsigned char *bytes = calloc(selected, 4),
		      *present = calloc(selected, 1);
	unsigned *aids = calloc(selected, sizeof(unsigned));
	assert(prefixes && values && bytes && present && aids);
	for (unsigned i = 0; i < selected; i++) {
		aids[i] = (i + 1) * (dups + 1) / (selected + 1);
		be32(bytes + 4 * i, aids[i]);
		prefixes[i].mv_data = bytes + 4 * i;
		prefixes[i].mv_size = 4;
	}
	MDB_dup_projector *projector;
	CHECK(mdb_dup_projector_create(&projector, prefixes, selected));
	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	printf("keys=%u duplicates=%u width=%u selected=%u prefix=%d "
	       "map=1024MiB\n",
	       keys, dups, width, selected, compressed);
	uint64_t expected = 0;
	for (int pass = -1; pass < 8; pass++)
		for (int turn = 0; turn < 2; turn++) {
			int selective = turn ^ (pass & 1);
			MDB_cursor *cur;
			CHECK(mdb_cursor_open(txn, dbi, &cur));
			MDB_val k, v;
			uint64_t sum = 0, rows = 0;
			double start = now();
			int rc = mdb_cursor_get(cur, &k, selective ? NULL : &v,
						MDB_FIRST);
			while (rc == MDB_SUCCESS) {
				if (selective) {
					CHECK(mdb_cursor_project_dup(
					    cur, projector, values, present));
					for (unsigned i = 0; i < selected;
					     i++) {
						assert(present[i]);
						sum +=
						    values[i].mv_size +
						    ((unsigned char *)values[i]
							 .mv_data)
							[values[i].mv_size - 1];
					}
				} else {
					const MDB_val *all = NULL;
					mdb_size_t count = 0;
					int frc = mdb_cursor_list_dup(cur, &all,
								      &count);
					if (frc == MDB_NOTFOUND) {
						all = &v;
						count = 1;
					} else
						CHECK(frc);
					unsigned wanted = 0;
					for (mdb_size_t i = 0;
					     i < count && wanted < selected;
					     i++) {
						unsigned a = aid(&all[i]);
						if (a < aids[wanted])
							continue;
						assert(a == aids[wanted]);
						sum += all[i].mv_size +
						       ((unsigned char *)all[i]
							    .mv_data)
							   [all[i].mv_size - 1];
						wanted++;
					}
					assert(wanted == selected);
				}
				rows++;
				rc = mdb_cursor_get(cur, &k,
						    selective ? NULL : &v,
						    MDB_NEXT_NODUP);
			}
			assert(rc == MDB_NOTFOUND && rows == keys);
			double elapsed = now() - start;
			if (!expected)
				expected = sum;
			assert(sum == expected);
			printf(
			    "%s pass=%d ms=%.6f ns/entity=%.2f checksum=%llu\n",
			    selective ? "selective" : "full", pass, elapsed,
			    elapsed * 1e6 / keys, (unsigned long long)sum);
			fflush(stdout);
			mdb_cursor_close(cur);
		}
	mdb_txn_abort(txn);
	mdb_dup_projector_destroy(projector);
	mdb_env_close(env);
	free(prefixes);
	free(values);
	free(bytes);
	free(present);
	free(aids);
	return 0;
}
