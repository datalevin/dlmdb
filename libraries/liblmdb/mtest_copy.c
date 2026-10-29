/* Prepared copy snapshot, writer progress, and cleanup regressions.
 * Uses the engine directly to verify captured metadata and reader lifetime. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mdb.c"
#include <assert.h>
#include <time.h>

#define CHECK(call) do { int code = (call); if (code) { \
	fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #call, mdb_strerror(code)); abort(); \
} } while (0)

static void
path_join(char *out, size_t size, const char *parent, const char *name)
{
	int n = snprintf(out, size, "%s/%s", parent, name);
	assert(n > 0 && (size_t)n < size);
}

static void
clean_env(const char *dir)
{
	char path[512];
	path_join(path, sizeof(path), dir, "data.mdb");
	unlink(path);
	path_join(path, sizeof(path), dir, "lock.mdb");
	unlink(path);
	assert(!rmdir(dir));
}

static MDB_env *
open_env(const char *path, unsigned int flags, int readers)
{
	MDB_env *env;
	CHECK(mdb_env_create(&env));
	CHECK(mdb_env_set_mapsize(env, 64u << 20));
	CHECK(mdb_env_set_maxdbs(env, 4));
	CHECK(mdb_env_set_maxreaders(env, readers));
	CHECK(mdb_env_open(env, path, flags, 0600));
	return env;
}

static void
put(MDB_txn *txn, MDB_dbi dbi, const char *key, const char *value)
{
	MDB_val k = {strlen(key), (void *)key};
	MDB_val v = {strlen(value), (void *)value};
	CHECK(mdb_put(txn, dbi, &k, &v, 0));
}

static void
expect(MDB_txn *txn, MDB_dbi dbi, const char *key, const char *value)
{
	MDB_val k = {strlen(key), (void *)key}, v;
	int rc = mdb_get(txn, dbi, &k, &v);
	if (value) {
		assert(!rc && v.mv_size == strlen(value));
		assert(!memcmp(v.mv_data, value, v.mv_size));
	} else
		assert(rc == MDB_NOTFOUND);
}

typedef struct {
	MDB_env *env;
	MDB_dbi dbi;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int done;
} Writer;

static void *
write_after_prepare(void *arg)
{
	Writer *writer = arg;
	MDB_txn *txn;
	MDB_val key = {7, "deleted"};
	char value[16384], kbuf[40];
	int n, j;
	memset(value, 'x', sizeof(value)-1);
	value[sizeof(value)-1] = 0;
	for (n = 0; n < 32; n++) {
		CHECK(mdb_txn_begin(writer->env, NULL, 0, &txn));
		put(txn, writer->dbi, "before", "new");
		put(txn, writer->dbi, "after", "late");
		if (!n)
			CHECK(mdb_del(txn, writer->dbi, &key, NULL));
		/* Grow the file and recycle newer pages while the snapshot is held. */
		for (j = 0; j < 16; j++) {
			snprintf(kbuf, sizeof(kbuf), "churn-%d", j);
			value[0] = 'a' + (n % 26);
			put(txn, writer->dbi, kbuf, value);
		}
		CHECK(mdb_txn_commit(txn));
	}
	pthread_mutex_lock(&writer->mutex);
	writer->done = 1;
	pthread_cond_signal(&writer->cond);
	pthread_mutex_unlock(&writer->mutex);
	return NULL;
}

static void
run_writers(MDB_env *env, MDB_dbi dbi)
{
	Writer writer = {0};
	pthread_t thread;
	struct timespec deadline;
	writer.env = env;
	writer.dbi = dbi;
	CHECK(pthread_mutex_init(&writer.mutex, NULL));
	CHECK(pthread_cond_init(&writer.cond, NULL));
	CHECK(pthread_create(&thread, NULL, write_after_prepare, &writer));
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 15;
	pthread_mutex_lock(&writer.mutex);
	while (!writer.done) {
		int rc = pthread_cond_timedwait(&writer.cond, &writer.mutex, &deadline);
		assert(rc != ETIMEDOUT); /* Preparation must release the writer mutex. */
		assert(!rc);
	}
	pthread_mutex_unlock(&writer.mutex);
	CHECK(pthread_join(thread, NULL));
	CHECK(pthread_cond_destroy(&writer.cond));
	CHECK(pthread_mutex_destroy(&writer.mutex));
}

static void
snapshot_case(const char *root, unsigned int flags, unsigned int mode)
{
	char source[512], dest[512], legacy[512];
	MDB_env *env, *restored;
	MDB_txn *txn;
	MDB_dbi dbi, dup;
	MDB_copy *copy;
	struct stat st;
	mdb_size_t bytes;
	txnid_t id;
	path_join(source, sizeof(source), root, "source");
	path_join(dest, sizeof(dest), root, "prepared");
	path_join(legacy, sizeof(legacy), root, "legacy");
	assert(!mkdir(source, 0700) && !mkdir(dest, 0700) && !mkdir(legacy, 0700));
	env = open_env(source, flags, 8);
	CHECK(mdb_txn_begin(env, NULL, 0, &txn));
	CHECK(mdb_dbi_open(txn, "data", MDB_CREATE, &dbi));
	CHECK(mdb_dbi_open(txn, "duplicates", MDB_CREATE|MDB_DUPSORT, &dup));
	put(txn, dbi, "before", "old");
	put(txn, dbi, "deleted", "keep");
	put(txn, dup, "key", "first");
	put(txn, dup, "key", "second");
	CHECK(mdb_txn_commit(txn));
	CHECK(mdb_env_copy_prepare(env, dest, mode, &copy));
	assert((uintptr_t)copy->mc_meta % env->me_os_psize == 0);
	id = copy->mc_txn->mt_txnid;
	bytes = copy->mc_bytes;
	assert(!fstat(copy->mc_fd, &st) && !st.st_size);
	run_writers(env, dbi);
	assert(copy->mc_txn->mt_txnid == id);
	CHECK(mdb_env_copy_execute(copy));
	assert(copy->mc_txn->mt_txnid == id);
	assert(mdb_env_copy_execute(copy) == EINVAL);
	assert(!fstat(copy->mc_fd, &st));
	if (!mode)
		assert((mdb_size_t)st.st_size == bytes);
	CHECK(mdb_env_copy_close(copy));
	restored = open_env(dest, MDB_RDONLY, 8);
	CHECK(mdb_txn_begin(restored, NULL, MDB_RDONLY, &txn));
	CHECK(mdb_dbi_open(txn, "data", 0, &dbi));
	CHECK(mdb_dbi_open(txn, "duplicates", 0, &dup));
	expect(txn, dbi, "before", "old");
	expect(txn, dbi, "deleted", "keep");
	expect(txn, dbi, "after", NULL);
	expect(txn, dup, "key", "first");
	mdb_txn_abort(txn);
	mdb_env_close(restored);
	/* Legacy entry points compose the same phases and retain behavior. */
	CHECK(mdb_env_copy2(env, legacy, mode));
	restored = open_env(legacy, MDB_RDONLY, 8);
	CHECK(mdb_txn_begin(restored, NULL, MDB_RDONLY, &txn));
	CHECK(mdb_dbi_open(txn, "data", 0, &dbi));
	expect(txn, dbi, "before", "new");
	expect(txn, dbi, "deleted", NULL);
	expect(txn, dbi, "after", "late");
	mdb_txn_abort(txn);
	mdb_env_close(restored);
	mdb_env_close(env);
	clean_env(source);
	clean_env(dest);
	clean_env(legacy);
}

static void
cleanup_case(const char *root, unsigned int mode)
{
	char source[512], dest[512], path[512];
	MDB_env *env;
	MDB_copy *copy = (MDB_copy *)1;
	MDB_txn *readers[2];
	int n, fd;
	path_join(source, sizeof(source), root, "source");
	path_join(dest, sizeof(dest), root, "prepared");
	path_join(path, sizeof(path), dest, "data.mdb");
	assert(!mkdir(source, 0700) && !mkdir(dest, 0700));
	env = open_env(source, MDB_NOTLS, 2);
	assert(mdb_env_copy_prepare(env, dest, ~MDB_CP_COMPACT, &copy) == EINVAL);
	assert(!copy && access(path, F_OK) < 0);
	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &readers[0]));
	CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &readers[1]));
	assert(mdb_env_copy_prepare(env, dest, mode, &copy) == MDB_READERS_FULL);
	assert(!copy);
	mdb_txn_abort(readers[0]);
	mdb_txn_abort(readers[1]);
	assert(!unlink(path));
	/* Cancel without executing more times than there are reader slots. */
	for (n = 0; n < 32; n++) {
		CHECK(mdb_env_copy_prepare(env, dest, mode, &copy));
		CHECK(mdb_env_copy_close(copy));
		assert(!unlink(path));
	}
	CHECK(mdb_env_copy_prepare(env, dest, mode, &copy));
	fd = copy->mc_fd;
	assert(!close(fd));
	assert(mdb_env_copy_execute(copy));
	assert(mdb_env_copy_close(copy)); /* EBADF, but the reader still releases. */
	assert(!unlink(path));
	CHECK(mdb_env_copy_prepare(env, dest, mode, &copy));
	CHECK(mdb_env_copy_execute(copy));
	CHECK(mdb_env_copy_close(copy));
	/* Exclusive creation never replaces a completed copy. */
	assert(mdb_env_copy_prepare(env, dest, mode, &copy) == EEXIST && !copy);
	CHECK(mdb_env_copy_close(NULL));
	mdb_env_close(env);
	clean_env(source);
	clean_env(dest);
}

/* Cover empty/read-only sources and the caller-owned fd entry point. */
static void edge_case(const char *root, unsigned int mode, int empty)
{
	char source[512], dest[512], output[512], lock[512], single[512];
	MDB_env *env, *restored;
	MDB_txn *txn;
	MDB_dbi dbi;
	MDB_copy *copy;
	MDB_stat stats;
	int fd;
	path_join(source, sizeof(source), root, "source");
	path_join(dest, sizeof(dest), root, "prepared");
	path_join(output, sizeof(output), root, "fd-copy");
	path_join(single, sizeof(single), root, "single-copy");
	snprintf(lock, sizeof(lock), "%s-lock", output);
	assert(!mkdir(source, 0700) && !mkdir(dest, 0700));
	env = open_env(source, 0, 8);
	if (!empty) {
		CHECK(mdb_txn_begin(env, NULL, 0, &txn));
		CHECK(mdb_dbi_open(txn, NULL, 0, &dbi));
		put(txn, dbi, "before", "old");
		CHECK(mdb_txn_commit(txn));
	}
	mdb_env_close(env);
	env = open_env(source, MDB_RDONLY, 8);
	CHECK(mdb_env_copy_prepare(env, dest, mode, &copy));
	CHECK(mdb_env_copy_execute(copy));
	CHECK(mdb_env_copy_close(copy));
	fd = open(output, O_CREAT|O_EXCL|O_WRONLY, 0600);
	assert(fd >= 0);
	CHECK(mdb_env_copyfd2(env, fd, mode));
	assert(fcntl(fd, F_GETFD) >= 0); /* The supplied fd remains caller-owned. */
	assert(!close(fd));
	mdb_env_close(env);
	restored = open_env(output, MDB_RDONLY|MDB_NOSUBDIR, 8);
	CHECK(mdb_env_stat(restored, &stats));
	assert(stats.ms_entries == (empty ? 0u : 1u));
	if (!empty) {
		CHECK(mdb_txn_begin(restored, NULL, MDB_RDONLY, &txn));
		CHECK(mdb_dbi_open(txn, NULL, 0, &dbi));
		expect(txn, dbi, "before", "old");
		mdb_txn_abort(txn);
	}
	/* A NOSUBDIR source uses a file destination, as in the legacy API. */
	CHECK(mdb_env_copy_prepare(restored, single, mode, &copy));
	CHECK(mdb_env_copy_execute(copy));
	CHECK(mdb_env_copy_close(copy));
	mdb_env_close(restored);
	assert(!unlink(output) && !unlink(lock) && !unlink(single));
	clean_env(source);
	clean_env(dest);
}

int
main(void)
{
	char root[] = "/tmp/dtlv-prepared-copy-XXXXXX";
	unsigned int flags[] = {0, MDB_NOTLS, MDB_WRITEMAP, MDB_INMEMORY};
	unsigned int mode;
	size_t n;
	assert(mkdtemp(root));
	for (mode = 0; mode <= MDB_CP_COMPACT; mode += MDB_CP_COMPACT) {
		for (n = 0; n < sizeof(flags)/sizeof(flags[0]); n++)
			snapshot_case(root, flags[n], mode);
		cleanup_case(root, mode);
		edge_case(root, mode, 0);
		edge_case(root, mode, 1);
	}
	assert(!rmdir(root));
	puts("Prepared copy: fixed snapshots, writer progress, legacy copies and cleanup passed");
	return 0;
}
