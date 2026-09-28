/* White-box path coverage for the prefix rebalance regressions. Run the
 * production cases with test-only counters at each rebalance decision.
 */
#define MDB_REBALANCE_COVERAGE 1
#include "mdb.c"

/* White-box page-size emulation for testing supported 4/16/32 KiB layouts
 * on one host. Only this test build can access the environment internals.
 */
static unsigned int rebalance_test_pagesize;

static int
rebalance_test_env_create(MDB_env **env)
{
	int rc = mdb_env_create(env);
	if (!rc && rebalance_test_pagesize)
		(*env)->me_os_psize = rebalance_test_pagesize;
	return rc;
}

#define mdb_env_create rebalance_test_env_create
#define main mtest_prefix_full_main
#include "mtest_prefix.c"
#undef main
#undef mdb_env_create

int
main(int argc, char **argv)
{
	static const char *names[MDB_RB_PATHS] = {
		"move from left", "move from right", "merge into left",
		"merge into right", "skip oversized left move",
		"skip oversized right move", "skip oversized left merge",
		"skip oversized right merge"
	};
	unsigned int merge_left_before =
	    mdb_rebalance_paths[MDB_RB_SKIP_MERGE_LEFT];
	unsigned int merge_right_before =
	    mdb_rebalance_paths[MDB_RB_SKIP_MERGE_RIGHT];
	unsigned int move_left_before;
	unsigned int move_right_before;
	unsigned int borrowed_left_before;
	unsigned int borrowed_right_before;
	unsigned int trunk_delete_splits_before;
	if (argc != 1) {
		if (argc != 3 || strcmp(argv[1], "--page-size") ||
		    (strcmp(argv[2], "4096") && strcmp(argv[2], "16384") &&
		    strcmp(argv[2], "32768"))) {
			fprintf(stderr, "usage: %s [--page-size 4096|16384|32768]\n",
			    argv[0]);
			return EXIT_FAILURE;
		}
		rebalance_test_pagesize = (unsigned int)strtoul(argv[2], NULL, 10);
	}

	test_prefix_merge_capacity_regression();
	if (mdb_rebalance_paths[MDB_RB_SKIP_MERGE_LEFT] == merge_left_before ||
	    mdb_rebalance_paths[MDB_RB_SKIP_MERGE_RIGHT] == merge_right_before) {
		fprintf(stderr, "prefix merge fixture: oversized merge skips left=%u right=%u\n",
		    mdb_rebalance_paths[MDB_RB_SKIP_MERGE_LEFT] - merge_left_before,
		    mdb_rebalance_paths[MDB_RB_SKIP_MERGE_RIGHT] - merge_right_before);
		return EXIT_FAILURE;
	}
	move_left_before = mdb_rebalance_paths[MDB_RB_SKIP_MOVE_LEFT];
	test_prefix_borrow_capacity_regression();
	if (mdb_rebalance_paths[MDB_RB_SKIP_MOVE_LEFT] == move_left_before) {
		fprintf(stderr, "prefix borrow fixture: oversized left move skips=%u\n",
		    mdb_rebalance_paths[MDB_RB_SKIP_MOVE_LEFT] - move_left_before);
		return EXIT_FAILURE;
	}
	move_right_before = mdb_rebalance_paths[MDB_RB_SKIP_MOVE_RIGHT];
	test_prefix_reverse_borrow_capacity_regression();
	if (mdb_rebalance_paths[MDB_RB_SKIP_MOVE_RIGHT] == move_right_before) {
		fprintf(stderr, "reverse borrow fixture: oversized right move skips=%u\n",
		    mdb_rebalance_paths[MDB_RB_SKIP_MOVE_RIGHT] - move_right_before);
		return EXIT_FAILURE;
	}
	trunk_delete_splits_before = mdb_prefix_trunk_delete_splits;
	test_prefix_reverse_trunk_delete_overflow_regression();
	if (mdb_prefix_trunk_delete_splits == trunk_delete_splits_before) {
		fprintf(stderr, "reverse trunk delete fixture missed split/retry\n");
		return EXIT_FAILURE;
	}
	test_prefix_reverse_root_delete_overflow_regression();
	test_prefix_reverse_root_delete_dupsort_regression();
	test_prefix_reverse_parent_delete_split_regression();
	test_prefix_dupsort_inline_trunk_delete_growth();
	borrowed_left_before = mdb_rebalance_paths[MDB_RB_MOVE_LEFT];
	test_prefix_overflow_rebalance_regression();
	if (mdb_rebalance_paths[MDB_RB_MOVE_LEFT] == borrowed_left_before) {
		fprintf(stderr, "descending overflow fixture missed left borrow\n");
		return EXIT_FAILURE;
	}
	borrowed_right_before = mdb_rebalance_paths[MDB_RB_MOVE_RIGHT];
	test_prefix_overflow_ascending_rebalance();
	if (mdb_rebalance_paths[MDB_RB_MOVE_RIGHT] == borrowed_right_before) {
		fprintf(stderr, "ascending overflow fixture missed right borrow\n");
		return EXIT_FAILURE;
	}

	for (unsigned int path = 0; path < MDB_RB_PATHS; ++path) {
		if (!mdb_rebalance_paths[path]) {
			fprintf(stderr, "prefix rebalance path not covered: %s\n",
			    names[path]);
			return EXIT_FAILURE;
		}
		printf("  %-27s %u\n", names[path], mdb_rebalance_paths[path]);
	}
	printf("mtest_prefix_rebalance: all eight leaf paths covered\n");
	return EXIT_SUCCESS;
}
