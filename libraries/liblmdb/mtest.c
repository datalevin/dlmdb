/* mtest.c - memory-mapped database tester/toy */
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
#include <time.h>
#include "dlmdb.h"

#define VPRINTF(...) do { if (verbose) printf(__VA_ARGS__); } while (0)

#define E(expr) CHECK((rc = (expr)) == MDB_SUCCESS, #expr)
#define RES(err, expr) ((rc = expr) == (err) || (CHECK(!rc, #expr), 0))
#define CHECK(test, msg) ((test) ? (void)0 : ((void)fprintf(stderr, \
	"%s:%d: %s: %s\n", __FILE__, __LINE__, msg, mdb_strerror(rc)), abort()))

static int verbose;

int main(int argc,char * argv[])
{
	int i = 0, j = 0, rc;
	MDB_env *env;
	MDB_dbi dbi;
	MDB_val key, data;
	MDB_txn *txn;
	MDB_stat mst;
	MDB_cursor *cursor, *cur2;
	MDB_cursor_op op;
	int count;
	int *values;
	char sval[32] = "";

	(void)argc;
	(void)argv;
	verbose = getenv("MTEST_VERBOSE") != NULL;
  printf("%s\n", mdb_version(NULL, NULL, NULL));

	srand(time(NULL));

	    count = (rand()%384) + 64;
	    values = (int *)malloc(count*sizeof(int));

	    for(i = 0;i<count;i++) {
			values[i] = rand()%1024;
	    }

		E(mdb_env_create(&env));
		E(mdb_env_set_maxreaders(env, 1));
		E(mdb_env_set_mapsize(env, 10485760));
		E(mdb_env_set_maxdbs(env, 16));
	E(mdb_env_open(env, "./testdb", MDB_NOLOCK /*|MDB_NOSYNC*/, 0664));

		E(mdb_txn_begin(env, NULL, 0, &txn));
		E(mdb_dbi_open(txn, NULL, 0, &dbi));

		key.mv_size = sizeof(int);
		key.mv_data = sval;

		printf("Adding %d values\n", count);
	    for (i=0;i<count;i++) {
			sprintf(sval, "%03x %d foo bar", values[i], values[i]);
			/* Set <data> in each iteration, since MDB_NOOVERWRITE may modify it */
			data.mv_size = sizeof(sval);
			data.mv_data = sval;
			if (RES(MDB_KEYEXIST, mdb_put(txn, dbi, &key, &data, MDB_NOOVERWRITE))) {
				j++;
				data.mv_size = sizeof(sval);
				data.mv_data = sval;
			}
	    }
		if (j) printf("%d duplicates skipped\n", j);
		E(mdb_txn_commit(txn));
		E(mdb_env_stat(env, &mst));

		E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
		E(mdb_cursor_open(txn, dbi, &cursor));
		while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0) {
			VPRINTF("key: %p %.*s, data: %p %.*s\n",
				key.mv_data,  (int) key.mv_size,  (char *) key.mv_data,
				data.mv_data, (int) data.mv_size, (char *) data.mv_data);
		}
		CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get");
		mdb_cursor_close(cursor);
		mdb_txn_abort(txn);

		j=0;
		key.mv_data = sval;
	    for (i= count - 1; i > -1; i-= (rand()%5)) {
			j++;
			txn=NULL;
			E(mdb_txn_begin(env, NULL, 0, &txn));
			sprintf(sval, "%03x ", values[i]);
			if (RES(MDB_NOTFOUND, mdb_del(txn, dbi, &key, NULL))) {
				j--;
				mdb_txn_abort(txn);
			} else {
				E(mdb_txn_commit(txn));
			}
	    }
	    free(values);
		printf("Deleted %d values\n", j);

		E(mdb_env_stat(env, &mst));
		E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
		E(mdb_cursor_open(txn, dbi, &cursor));
		printf("Cursor next\n");
		while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0) {
			VPRINTF("key: %.*s, data: %.*s\n",
				(int) key.mv_size,  (char *) key.mv_data,
				(int) data.mv_size, (char *) data.mv_data);
		}
		CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get");
		printf("Cursor last\n");
		E(mdb_cursor_get(cursor, &key, &data, MDB_LAST));
		VPRINTF("key: %.*s, data: %.*s\n",
			(int) key.mv_size,  (char *) key.mv_data,
			(int) data.mv_size, (char *) data.mv_data);
		printf("Cursor prev\n");
		while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_PREV)) == 0) {
			VPRINTF("key: %.*s, data: %.*s\n",
				(int) key.mv_size,  (char *) key.mv_data,
				(int) data.mv_size, (char *) data.mv_data);
		}
		CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get");
		printf("Cursor last/prev\n");
		E(mdb_cursor_get(cursor, &key, &data, MDB_LAST));
			VPRINTF("key: %.*s, data: %.*s\n",
				(int) key.mv_size,  (char *) key.mv_data,
				(int) data.mv_size, (char *) data.mv_data);
		E(mdb_cursor_get(cursor, &key, &data, MDB_PREV));
			VPRINTF("key: %.*s, data: %.*s\n",
				(int) key.mv_size,  (char *) key.mv_data,
				(int) data.mv_size, (char *) data.mv_data);

		mdb_cursor_close(cursor);
		mdb_txn_abort(txn);

		printf("Deleting with cursor\n");
		E(mdb_txn_begin(env, NULL, 0, &txn));
		E(mdb_cursor_open(txn, dbi, &cur2));
		for (i=0; i<50; i++) {
			if (RES(MDB_NOTFOUND, mdb_cursor_get(cur2, &key, &data, MDB_NEXT)))
				break;
			VPRINTF("key: %p %.*s, data: %p %.*s\n",
				key.mv_data,  (int) key.mv_size,  (char *) key.mv_data,
				data.mv_data, (int) data.mv_size, (char *) data.mv_data);
			E(mdb_del(txn, dbi, &key, NULL));
		}

		printf("Restarting cursor in txn\n");
		for (op=MDB_FIRST, i=0; i<=32; op=MDB_NEXT, i++) {
			if (RES(MDB_NOTFOUND, mdb_cursor_get(cur2, &key, &data, op)))
				break;
			VPRINTF("key: %p %.*s, data: %p %.*s\n",
				key.mv_data,  (int) key.mv_size,  (char *) key.mv_data,
				data.mv_data, (int) data.mv_size, (char *) data.mv_data);
		}
		mdb_cursor_close(cur2);
		E(mdb_txn_commit(txn));

		printf("Restarting cursor outside txn\n");
		E(mdb_txn_begin(env, NULL, 0, &txn));
		E(mdb_cursor_open(txn, dbi, &cursor));
		for (op=MDB_FIRST, i=0; i<=32; op=MDB_NEXT, i++) {
			if (RES(MDB_NOTFOUND, mdb_cursor_get(cursor, &key, &data, op)))
				break;
			VPRINTF("key: %p %.*s, data: %p %.*s\n",
				key.mv_data,  (int) key.mv_size,  (char *) key.mv_data,
				data.mv_data, (int) data.mv_size, (char *) data.mv_data);
		}
		mdb_cursor_close(cursor);
		mdb_txn_abort(txn);

		printf("Testing dupsort duplicate ordering\n");
		{
			MDB_dbi dup_dbi;
			MDB_cursor *dup_cursor;
			MDB_val dup_key, dup_data;
			char dupkey[] = "dupsort-key";
			static const char *dup_inputs[] = {
				"charlie", "delta", "charlie", "bravo",
        "alpha", "echo", "alpha", "alpha"
			};
			static const char *dup_expected[] = {
				"alpha", "bravo", "charlie", "delta", "echo"
			};
			enum {
				dup_input_count = sizeof(dup_inputs) / sizeof(dup_inputs[0]),
				dup_expected_count = sizeof(dup_expected) / sizeof(dup_expected[0])
			};
			int order[dup_input_count];
			int n, swap_idx, tmp;

			E(mdb_txn_begin(env, NULL, 0, &txn));
			E(mdb_dbi_open(txn, "dupsort", MDB_CREATE | MDB_DUPSORT, &dup_dbi));
			E(mdb_drop(txn, dup_dbi, 0));
			dup_key.mv_size = sizeof(dupkey) - 1;
			dup_key.mv_data = dupkey;
			for (n = 0; n < dup_input_count; n++)
				order[n] = n;
			for (n = dup_input_count - 1; n > 0; n--) {
				swap_idx = rand() % (n + 1);
				tmp = order[n];
				order[n] = order[swap_idx];
				order[swap_idx] = tmp;
			}
			for (n = 0; n < dup_input_count; n++) {
				dup_data.mv_data = (void *)dup_inputs[order[n]];
				dup_data.mv_size = strlen(dup_inputs[order[n]]);
				E(mdb_put(txn, dup_dbi, &dup_key, &dup_data, 0));
			}
			E(mdb_txn_commit(txn));

			E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn));
			E(mdb_cursor_open(txn, dup_dbi, &dup_cursor));
			E(mdb_cursor_get(dup_cursor, &dup_key, &dup_data, MDB_SET_KEY));
			{
				size_t dup_total = 0;
				E(mdb_cursor_count(dup_cursor, &dup_total));
				CHECK(dup_total == dup_expected_count, "mdb_cursor_count dupsort size");
			}
			for (n = 0; n < dup_expected_count; n++) {
				CHECK(dup_data.mv_size == strlen(dup_expected[n]) &&
					memcmp(dup_data.mv_data, dup_expected[n], dup_data.mv_size) == 0,
					"mdb_cursor_get dupsort ordering");
				if (n + 1 < dup_expected_count)
					E(mdb_cursor_get(dup_cursor, &dup_key, &dup_data, MDB_NEXT_DUP));
			}
			rc = mdb_cursor_get(dup_cursor, &dup_key, &dup_data, MDB_NEXT_DUP);
			CHECK(rc == MDB_NOTFOUND, "mdb_cursor_get dupsort end");
			mdb_cursor_close(dup_cursor);
			mdb_txn_abort(txn);

			mdb_dbi_close(env, dup_dbi);
		}

		mdb_dbi_close(env, dbi);
		mdb_env_close(env);

	return 0;
}
