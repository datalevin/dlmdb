/* mtest_inmem.c - true in-memory environment smoke test */
/*
 * Copyright 2011-2021 Howard Chu, Symas Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "dlmdb.h"

#define E(expr) CHECK((rc = (expr)) == MDB_SUCCESS, #expr)
#define CHECK(test, msg) ((test) ? (void)0 : ((void)fprintf(stderr, \
	"%s:%d: %s: %s\n", __FILE__, __LINE__, msg, mdb_strerror(rc)), abort()))

typedef struct ReaderCtx {
	MDB_env *env;
	MDB_dbi dbi;
	const char *expect;
	int rc;
	volatile int done;
} ReaderCtx;

static void *
reader_fn(void *arg)
{
	ReaderCtx *ctx = (ReaderCtx *)arg;
	MDB_txn *txn = NULL;
	MDB_val key, data;
	char k[] = "k";
	size_t expect_len = strlen(ctx->expect);

	key.mv_data = k;
	key.mv_size = sizeof(k) - 1;
	ctx->rc = mdb_txn_begin(ctx->env, NULL, MDB_RDONLY, &txn);
	if (ctx->rc) {
		ctx->done = 1;
		return NULL;
	}
	ctx->rc = mdb_get(txn, ctx->dbi, &key, &data);
	if (!ctx->rc && (data.mv_size != expect_len ||
		memcmp(data.mv_data, ctx->expect, expect_len) != 0))
		ctx->rc = EINVAL;
	usleep(20000);
	mdb_txn_abort(txn);
	ctx->done = 1;
	return NULL;
}

int
main(void)
{
	int rc;
	MDB_env *env;
	MDB_txn *txn;
	MDB_dbi dbi;
	MDB_val key, data, got;
	pthread_t t1, t2;
	ReaderCtx r1, r2;
	char k[] = "k";
	char v1[] = "value-1";
	char v2[] = "value-2";

	E(mdb_env_create(&env));
	E(mdb_env_set_mapsize(env, 1u << 20));
	E(mdb_env_open(env, NULL, MDB_INMEMORY, 0664));

	key.mv_data = k;
	key.mv_size = sizeof(k) - 1;
	data.mv_data = v1;
	data.mv_size = sizeof(v1) - 1;

	E(mdb_txn_begin(env, NULL, 0, &txn));
	E(mdb_dbi_open(txn, NULL, 0, &dbi));
	E(mdb_put(txn, dbi, &key, &data, 0));
	E(mdb_txn_commit(txn));

	E(mdb_txn_begin(env, NULL, 0, &txn));
	data.mv_data = v2;
	data.mv_size = sizeof(v2) - 1;
	E(mdb_put(txn, dbi, &key, &data, 0));

	r1.env = env;
	r1.dbi = dbi;
	r1.expect = v1;
	r1.rc = MDB_SUCCESS;
	r1.done = 0;
	r2 = r1;
	E(pthread_create(&t1, NULL, reader_fn, &r1));
	E(pthread_create(&t2, NULL, reader_fn, &r2));
	for (rc = 0; rc < 2000 && !(r1.done && r2.done); rc++)
		usleep(1000);
	CHECK(r1.done && r2.done, "readers did not finish while writer active");
	E(pthread_join(t1, NULL));
	E(pthread_join(t2, NULL));
	if (r1.rc != MDB_SUCCESS) {
		fprintf(stderr, "reader 1 rc=%d (%s)\n", r1.rc, mdb_strerror(r1.rc));
		abort();
	}
	if (r2.rc != MDB_SUCCESS) {
		fprintf(stderr, "reader 2 rc=%d (%s)\n", r2.rc, mdb_strerror(r2.rc));
		abort();
	}
	E(mdb_txn_commit(txn));

	E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
	E(mdb_get(txn, dbi, &key, &got));
	CHECK(got.mv_size == data.mv_size, "value size mismatch");
	CHECK(memcmp(got.mv_data, data.mv_data, data.mv_size) == 0,
		"value content mismatch");
	mdb_txn_abort(txn);

	mdb_dbi_close(env, dbi);
	mdb_env_close(env);
	printf("mtest_inmem: ok\n");
	return 0;
}
