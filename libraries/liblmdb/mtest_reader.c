#ifdef NDEBUG
#undef NDEBUG
#endif

/* Inspect reclamation boundaries while using real reader transactions. */
#include "mdb.c"
#include <assert.h>
#include <stdio.h>

static void put_marker(MDB_env *env, MDB_dbi dbi, char value) {
	MDB_txn *txn;
	MDB_val key = {6, "marker"}, data = {1, &value};
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_put(txn, dbi, &key, &data, 0) == 0);
	assert(mdb_txn_commit(txn) == 0);
}

static void reader_reclamation_test(unsigned flags) {
	char dir[] = "/tmp/dlmdb-reader-XXXXXX";
	char file[256];
	MDB_env *env;
	MDB_txn *txn, *reader, *child;
	MDB_cursor *cursor;
	MDB_dbi dbi;
	MDB_page *page;
	MDB_val key = {7, "payload"}, data;
	assert(mkdtemp(dir));
	assert(mdb_env_create(&env) == 0);
	assert(mdb_env_set_mapsize(env, 64UL * 1024 * 1024) == 0);
	assert(mdb_env_open(env, dir,
		flags | MDB_NOTLS | MDB_NOSYNC, 0600) == 0);
	size_t size = env->me_psize * 4;
	char *payload = malloc(size);
	assert(payload);
	memset(payload, 'p', size);
	data = (MDB_val){size, payload};
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi) == 0);
	assert(mdb_put(txn, dbi, &key, &data, 0) == 0);
	assert(mdb_txn_commit(txn) == 0);

	/* Keep the original overflow value alive across its deletion. */
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &reader) == 0);
	assert(mdb_get(reader, dbi, &key, &data) == 0);
	int pages = OVPAGES(size, env->me_psize);
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_del(txn, dbi, &key, NULL) == 0);
	assert(mdb_txn_commit(txn) == 0);
	put_marker(env, dbi, 'a');

	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
	pgno_t next = txn->mt_next_pgno;
	assert(mdb_page_new(cursor, P_OVERFLOW, pages, &page) == 0);
	assert(page->mp_pgno == next);
	assert(env->me_pgoldest == reader->mt_txnid);
	assert(env->me_pgoldest < txn->mt_txnid - 1);
	memset(METADATA(page), 'x', size);
	assert(mdb_get(reader, dbi, &key, &data) == 0);
	assert(data.mv_size == size && !memcmp(data.mv_data, payload, size));

	/* A boundary below the ceiling must refresh during the same writer
	 * after a reader leaves, making its old overflow pages reusable.
	 */
	mdb_txn_abort(reader);
	assert(mdb_page_new(cursor, P_OVERFLOW, pages, &page) == 0);
	assert(page->mp_pgno < next);
	assert(env->me_pgoldest == txn->mt_txnid - 1);
	memset(METADATA(page), 'y', size);

	/* New readers can only acquire the latest committed snapshot. */
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &reader) == 0);
	assert(reader->mt_txnid == env->me_pgoldest);
	for (unsigned i = 0; i < 4; ++i) {
		assert(mdb_page_new(cursor, P_OVERFLOW, pages, &page) == 0);
		assert(env->me_pgoldest == txn->mt_txnid - 1);
	}
	MDB_val marker = {6, "marker"};
	assert(mdb_get(reader, dbi, &marker, &data) == 0);
	assert(data.mv_size == 1 && *(char *)data.mv_data == 'a');
	mdb_txn_abort(reader);
	mdb_cursor_close(cursor);

	if (!(flags & MDB_WRITEMAP)) {
		assert(mdb_txn_begin(env, txn, 0, &child) == 0);
		assert(child->mt_txnid == txn->mt_txnid);
		assert(mdb_cursor_open(child, dbi, &cursor) == 0);
		assert(mdb_page_new(cursor, P_OVERFLOW, pages, &page) == 0);
		assert(env->me_pgoldest == child->mt_txnid - 1);
		mdb_cursor_close(cursor);
		mdb_txn_abort(child);
	}
	/* These direct allocations are deliberately not linked into the DB. */
	mdb_txn_abort(txn);

	/* A later writer has a higher ceiling. Do not reuse the previous
	 * writer's "fully advanced" decision while an older reader remains.
	 */
	put_marker(env, dbi, 'b');
	assert(mdb_txn_begin(env, NULL, MDB_RDONLY, &reader) == 0);
	put_marker(env, dbi, 'c');
	assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);
	assert(mdb_cursor_open(txn, dbi, &cursor) == 0);
	assert(mdb_page_new(cursor, P_OVERFLOW, pages * 4, &page) == 0);
	assert(env->me_pgoldest == reader->mt_txnid);
	assert(env->me_pgoldest < txn->mt_txnid - 1);
	assert(mdb_get(reader, dbi, &marker, &data) == 0);
	assert(data.mv_size == 1 && *(char *)data.mv_data == 'b');
	mdb_txn_abort(reader);
	assert(mdb_page_new(cursor, P_OVERFLOW, pages * 4, &page) == 0);
	assert(env->me_pgoldest == txn->mt_txnid - 1);
	mdb_cursor_close(cursor);
	mdb_txn_abort(txn);

	free(payload);
	mdb_env_close(env);
	snprintf(file, sizeof(file), "%s/data.mdb", dir);
	assert(unlink(file) == 0);
	snprintf(file, sizeof(file), "%s/lock.mdb", dir);
	assert(unlink(file) == 0);
	assert(rmdir(dir) == 0);
}

int main(void) {
	reader_reclamation_test(0);
	reader_reclamation_test(MDB_WRITEMAP);
	puts("reader reclamation: snapshots, refresh, nested and later writers passed");
	return 0;
}
