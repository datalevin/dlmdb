# DLMDB

This is the backing key-value (KV) storage engine of
[Datalevin](https://github.com/juji-io/datalevin) database: a simple, fast and
versatile Datalog database. Based on a fork of the esteemed
[LMDB](https://www.symas.com/mdb), this KV engine supports these additional
features:

* Order statistics.
  - Random access by rank, i.e. getting ith item efficiently.
  - Rank lookup for existing keys (and specific duplicates).
  - Efficient sampling.
  - Efficient range count.
* Prefix compression.
* DUPSORT iteration optimization.
* More robust interrupt handling.
* True in-memory mode (`MDB_INMEMORY`).
* Various performance optimizations.

These features are critical for Datalevin's high performance: the order
statistics facilitate query planning; prefix compression couples
well with triple storage; and the dedicated optimization speeds up the most
common index scan routines.

## In-memory mode

When persistence is not required, open an environment with
`MDB_INMEMORY`:

```c
MDB_env *env = NULL;
int rc = mdb_env_create(&env);
if (rc == MDB_SUCCESS)
    rc = mdb_env_set_mapsize(env, 1ull << 30);
if (rc == MDB_SUCCESS)
    rc = mdb_env_open(env, NULL, MDB_INMEMORY, 0664);
```

Properties:

* No data file or lock file is created.
* Data lives only in process memory and is lost on `mdb_env_close`.
* Concurrent readers are supported (single-writer semantics are still enforced).

### In-memory vs file-backed benchmark

`inmem_bench` runs the same write/read workload on file-backed and in-memory
environments and reports average timings and speedup.

```sh
./inmem_bench --entries 200000 --value-size 64 --rounds 3 --no-lock
```

Results refreshed on 2026-09-20, using an Apple M3 Pro with 36 GiB RAM,
16 KiB pages, and Apple Clang 21.0.0 with the Makefile's `-O2 -g` flags.
The refreshed benchmark tables use medians of three complete invocations,
retaining each harness's internal rounds. Speedups compare the two paths within
the current build.

| Operation | File-backed | In-memory | Speedup |
|---|---:|---:|---:|
| Write, including commit | 79.37 ms | 68.36 ms | 1.16x |
| Read | 67.79 ms | 66.08 ms | 1.03x |

Each invocation averages three rounds with a 76,800,000-byte map. Both modes
returned checksum 25,493,856. The file-backed write includes its durable commit.

## Order-statistics API

DLMDB extends LMDB's B+tree with optional per-branch cardinalities when a
database is opened using `MDB_COUNTED` flag. Once enabled, the following
functions are supported and they all run in O(log n) time:

### Getting the element at a rank

`mdb_get_rank` and `mdb_cursor_get_rank` : pass a zero-based rank to position
the cursor or to copy the key/data pair out of place.

```c
MDB_val key = {0}, data = {0};
int rc = mdb_get_rank(txn, dbi, rank, &key, &data);
if (rc == MDB_SUCCESS) {
    /* key/data point to the ith entry in sorted order */
}
```

`mdb_cursor_get_rank` keeps the state of previous call, so successive calls continue
search from the current position, which speeds up sparse sampling.

### Finding the rank of an existing key

`mdb_get_key_rank` and `mdb_cursor_key_rank` provide the inverse operation. They
return the zero-based rank of a key/value pair. These helpers work for both
plain and dupsort DBIs opened with `MDB_COUNTED`: passing a NULL `data` on a
dupsort DB returns the rank of the first duplicate for that key, while passing
`data` counts through the duplicate run up to (and including) that value.

```c
MDB_val key = {strlen("alpha"), "alpha"};
uint64_t rank = 0;
/* Plain database: `data` parameter may be NULL */
int rc = mdb_get_key_rank(txn, plain_counted_dbi, &key, NULL, &rank);

/* DUPSORT database: */
MDB_val dup = {strlen("payload-005"), "payload-005"};
rc = mdb_get_key_rank(txn, dupsort_counted_dbi, &key, &dup, &rank);
/* rank now includes all duplicates that precede payload-005 */
```

`mdb_cursor_key_rank` mirrors the cursor-oriented routines for callers that do
not want to leave the cursor's current position.

## Range Counting API

Counting records without walking the entire tree is more efficient than counting
while materializing the data. DLMDB exposes four helpers that are backed by the
same counted-branch metadata used for the rank APIs:

* `mdb_count_all(txn, dbi, flags, &total)` – Returns the total number of
  key/value pairs in a counted database. Works for both plain and dupsort DBIs.
* `mdb_count_range(txn, dbi, &low, &high, flags, &total)` – Counts entries with
  keys between two bounds. Flags let you toggle inclusive/exclusive endpoints.
* `mdb_range_count_keys(txn, dbi, &key_low, &key_high, flags, &total)` – Counts
  only distinct keys between optional bounds. On dupsort DBIs this ignores the
  number of duplicate values and accepts `MDB_COUNT_{LOWER,UPPER}_INCL` flags to
  control whether each boundary participates.
* `mdb_range_count_values(txn, dbi, &key_low, &key_high, key_flags, &total)` –
  Specialised for dupsort data: it counts individual values across a key range,
  honouring duplicate ordering.

All four execute in logarithmic time by traversing the B+tree once to the
relevant boundary nodes and aggregating the precomputed subtree counts stored on
each branch page.

```c
MDB_val low = {strlen("acct-0500"), "acct-0500"};
MDB_val high = {strlen("acct-0599"), "acct-0599"};
uint64_t total = 0;
int rc = mdb_count_range(txn, dbi, &low, &high, MDB_RANGE_INCLUDE_LOWER, &total);
```

### Counted DB Benchmark

`count_bench` exercises both naive cursor scans and the counted APIs. With
100,000 entries, 200 sampled queries, a span of 10,000 keys, and 20 duplicates
per key:

```sh
./count_bench --entries 100000 --queries 200 --span 10000 --dups 20 --shuffle
```

Median results, 2026-09-20. Both layouts use prefix compression; uncounted
means that only `MDB_COUNTED` is absent.

| Insert workload | Uncounted | Counted | Elapsed change |
|---|---:|---:|---:|
| 100,000 unique keys | 56.29 ms | 56.73 ms | +0.8% |
| 2,000,000 DUPSORT values | 1,025.92 ms | 1,040.85 ms | +1.5% |

| Query | Cursor scan (µs/query) | Counted API (µs/query) | Speedup |
|---|---:|---:|---:|
| Range count, unique keys | 171.49 | 0.94 | 182.43x |
| Rank lookup, cursor API | 599.89 | 0.57 | 1,051.18x |
| Range count, DUPSORT | 536.04 | 1.52 | 346.95x |

Query speedups are medians of the harness's reported ratios. The naive rank
lookup samples 128 queries; per-query times normalize it against the 200
counted queries. Direct `mdb_get_rank` measured 0.26 µs/query. In this workload,
counted metadata adds little insertion cost while avoiding long cursor scans.

### Sparse sampling

`sample_bench` focuses on the sampling workload: reading a tiny subset of
entries (e.g. 1000 ranks out of 10000000) without materializing the skipped
span. It builds a counted database (plain or dupsort) and compares two
strategies:

* **Warm sequential** – repeated `mdb_cursor_get_rank` calls using the cached
  cursor state that the counted tree exposes. This is the optimized path.
* **Stride scan (MDB_NEXT)** – a baseline that seeds the cursor once and then
  advances via `MDB_NEXT` for every skipped record, i.e. a traditional cursor walk.

The documented workload uses a stride of 10,000 and 1,000 samples:

```sh
./sample_bench --entries 10000000 --samples 1000 --batch 250000 --mode both \
               --path ./bench_sample_plain
./sample_bench --entries 10000000 --samples 1000 --batch 250000 --mode both \
               --dups 8 --path ./bench_sample_dups
```

Median results, 2026-09-20; sampling times cover all 1,000 samples:

| Layout | Population | Stride scan | Counted cursor | Sampling speedup |
|---|---:|---:|---:|---:|
| Unique keys | 1,910.68 ms | 103.40 ms | 1.81 ms | 57.13x |
| Eight duplicates/key | 4,681.41 ms | 162.35 ms | 1.61 ms | 100.84x |

Both use the default 16 GiB map and
`MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK`. The counted cursor skips the
intervening records; these speedups apply to sparse sampling at this stride.

## Prefix Compression

Enabling `MDB_PREFIX_COMPRESSION` flag on a database stores keys using shared
prefixes within each leaf page, reducing page fan-out and disk footprint. For
DUPSORT databases, a higher ratio of compression can be achieved because values
are also compressed.

To use prefix compression:

```c
MDB_dbi dbi;
CHECK(mdb_dbi_open(txn, "db", MDB_CREATE | MDB_PREFIX_COMPRESSION, &dbi));
```

Once enabled, read paths continue to expose fully reconstructed keys via the
cursor API; the optimization is completely internal to the engine.

### Prefix compression performance

`compress_bench` compares counted databases with and without prefix
compression. This workload uses 1,000,000 entries, 64-byte values, 16-byte
shared prefixes, 500,000 reads, 500,000 updates, and 500,000 deletes, with
20 duplicates per key in the DUPSORT variants:

```sh
./compress_bench -n 1000000 -r 500000 -v 64 -p 16 -m 4096 \
    -U 500000 -X 500000 -D 20
```

Median results, 2026-09-20, in milliseconds. Speedup is uncompressed time
divided by compressed time; values below 1 mean compression took longer.
All variants use a 4 GiB map, `MDB_NOLOCK`, and default durability. Operation
timers exclude the final commit. Range scans visit up to 256 keys across
1,000 ranges. “Cold” means a reopened environment; the OS cache is not purged.

Unique keys:

| Operation | Uncompressed | Prefix | Speedup |
|---|---:|---:|---:|
| Insert | 973.102 | 1,136.970 | 0.856x |
| Update | 489.617 | 479.469 | 1.021x |
| Delete | 720.147 | 886.444 | 0.812x |
| Reinsert | 564.517 | 608.555 | 0.928x |
| Random Read (cold) | 438.773 | 415.230 | 1.057x |
| Random Read (warm) | 438.868 | 405.480 | 1.082x |
| Range Scan (cold) | 7.240 | 7.102 | 1.019x |
| Range Scan (warm) | 5.141 | 5.393 | 0.953x |

DUPSORT:

| Operation | Uncompressed | Prefix | Speedup |
|---|---:|---:|---:|
| Insert | 1,371.212 | 1,234.082 | 1.111x |
| Update | 1,045.679 | 941.480 | 1.111x |
| Delete | 691.383 | 497.317 | 1.390x |
| Reinsert | 749.160 | 557.608 | 1.344x |
| Random Read (cold) | 439.207 | 366.538 | 1.198x |
| Random Read (warm) | 437.319 | 363.841 | 1.202x |
| Range Scan (cold) | 6.792 | 5.444 | 1.248x |
| Range Scan (warm) | 4.978 | 4.694 | 1.061x |

| Layout | Uncompressed data file | Prefix data file | Reduction |
|---|---:|---:|---:|
| Unique keys | 440.25 MiB | 331.89 MiB | 24.6% |
| DUPSORT | 339.59 MiB | 77.44 MiB | 77.2% |

Compression saves substantial space on these redundant keys and values.
DUPSORT operations are 1.06–1.39x faster in these medians. Unique-key inserts,
deletes, and reinserts are slower, while updates and random reads benefit;
the space saving does not imply that every operation becomes faster.

## LEAF2 read-side specialization

LEAF2 format stores fixed size values in DUPSORT, used in Datalog AVE index.

`mdb_cursor_list_dup()` returns every duplicate for the cursor's current key.
Inline LEAF2 duplicates already map directly to their containing page. When a
larger `MDB_DUPFIXED` set has been promoted to a nested B-tree, the read path
now preserves the nested cursor position and collects the tree page by page:

1. Position the nested cursor at its first value and use `MDB_GET_MULTIPLE` to
   obtain the complete current LEAF2 page.
2. Build the returned `MDB_val` descriptors with fixed-width pointer arithmetic;
   the duplicate bytes remain in their memory-mapped pages and are not copied.
3. Use `MDB_NEXT_MULTIPLE` only after consuming a page, reducing cursor movement
   from once per value to once per LEAF2 page.
4. Validate each page's type, width, byte count, and final entry count, then
   restore the caller's original duplicate position.

The descriptor fill remains O(values), but B-tree traversal is O(pages). The
same API and cursor-position contract apply whether the caller starts on the
first, middle, or last duplicate. Normal prefix-compressed DUPSORT trees keep
the existing scalar decode-and-copy path because their values are variable
width and may use cursor-owned decode buffers.

### DUPFIXED LEAF2 Benchmark

`leaf2_bench` compares fixed-width LEAF2 duplicate pages with normal
prefix-compressed duplicate trees. Both variants use `MDB_DUPSORT`,
`MDB_COUNTED`, and `MDB_PREFIX_COMPRESSION`; the only database flag difference
is `MDB_DUPFIXED`. They receive identical 16-byte keys, ordered 8-byte values,
transaction batches, and scalar `MDB_APPENDDUP` calls.

The benchmark uses independent fresh databases for append, interior insert, and
exact-value delete workloads. It also measures validated scalar scans, random
`MDB_GET_BOTH` lookups, `mdb_cursor_list_dup` scans, LEAF2 `MDB_GET_MULTIPLE`
scans, and a supplemental `MDB_MULTIPLE` write ceiling. Setup and full
validation are outside the mutation timers. Fresh A/B order alternates between
rounds, and reported speedups are the median of the per-round prefix time
divided by LEAF2 time.

Build and run the default workload with:

```
make -C libraries/liblmdb leaf2_bench
cd libraries/liblmdb
./leaf2_bench
```

The default workload uses 256 keys with 4096 duplicates each (1,048,576 total
values), four fresh A/B rounds, three repeated read passes, 100,000 exact
lookups, 65,536-value transactions, a 1 GiB map, and
`MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK`. The OS page cache is not purged.

Results refreshed on 2026-09-20 with the hardware and compiler described
above. The table takes the median of three complete default-workload runs,
including the harness's per-round paired speedups:

| Operation | Prefix DUPSORT | DUPFIXED LEAF2 | Paired result |
| --- | ---: | ---: | ---: |
| Scalar append cursor puts | 160.877 ms | 80.326 ms | 2.025x speedup |
| Interior cursor inserts | 398.703 ms | 240.701 ms | 1.668x speedup |
| Exact-value `mdb_del` | 1,587.701 ms | 372.727 ms | 4.265x speedup |
| Repeated scalar scan | 16.182 ms | 9.418 ms | 1.720x speedup |
| Repeated `mdb_cursor_list_dup` | 12.206 ms | 0.698 ms | 17.559x speedup |
| Random `MDB_GET_BOTH` | 113.732 ms | 37.442 ms | 2.993x speedup |
| Append high-water footprint | 20.23 bytes/value | 16.23 bytes/value | 0.802x size |

The LEAF2 `MDB_GET_MULTIPLE` scan consumed all values in 768 chunks in
0.434 ms. Repeated `mdb_cursor_list_dup` took 0.698 ms, with a 17.559x paired
speedup over prefix-compressed duplicate trees.

The supplemental `MDB_MULTIPLE` write run issued 256 calls of 4096 values.
Its put loop took **3.042 ms**, reaching **344.643 million values/second**,
with a **25.461x paired speedup** over scalar LEAF2 append. Including commits,
the complete bulk load took 4.508 ms. This uses a different write API and is
reported separately from the layout comparison above. The footprint is the
environment page high-water mark, not a compacted or live-page size.

## Cursor Reuse Benchmark

`cursor_reuse_bench` measures repeated cursor puts within and across leaves.
It runs ordinary counted, prefix-compressed counted, and prefix-compressed
DUPSORT/DUPFIXED trees, with forward, backward, random, and three same-leaf
patterns. Every case compares **before** (whole-leaf search, root fallback
across leaves) with **after** (bounded ordinary leaf search plus retained
ancestors) in one invocation. Both sides reuse the current leaf when it
contains the target. The new ordinary leaf search probes the adjacent key
first, then searches only the interval allowed by earlier comparisons.

```sh
make -C libraries/liblmdb cursor_reuse_bench
./libraries/liblmdb/cursor_reuse_bench
./libraries/liblmdb/cursor_reuse_bench --mode prefix --pattern forward \
    --entries 300000 --ops 200000 --stride 257 --map-mb 2048 --runs 5
./libraries/liblmdb/cursor_reuse_bench --mode plain --pattern same-leaf \
    --ops 2000000 --runs 8
```

Each case creates a temporary database with 256-byte keys and 64-byte values,
warms a write transaction, then times puts through one retained cursor. The
DUPSORT case puts an existing value again; other cases replace values. Final
values and entry counts are verified outside the timer, and temporary files
are removed. Five pairs run by default, alternating which side goes first.
The summary shows median before/after times, elapsed-time change, speedup,
tree depth, and page size. `--verbose` prints each individual sample. Trees
need at least three levels to retain an ancestor below the root.
`same-leaf`, `same-leaf-backward`, and `same-leaf-random` visit the first
eight keys in forward, backward, and fixed-seed random order, respectively.

The benchmark uses `MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK`; it measures warm
cursor positioning and writes, excluding commit and durability costs. A
private switch compiled only into this benchmark selects the two search paths;
ordinary library builds contain no switch.

Results measured on 2026-09-20 on an Apple M3 Pro with 36 GiB RAM, 16 KiB
pages, and Apple Clang 21.0.0 using `-O2 -g`. The default workload uses
300,000 entries, 200,000 puts per case, a stride of 257 for forward/backward
patterns, and a 2 GiB map. Times are medians of five alternating before/after
pairs; speedup is before divided by after.

| Mode | Pattern | Before (ms) | After (ms) | Speedup |
|---|---|---:|---:|---:|
| Ordinary counted | Forward | 218.161 | 199.072 | 1.096x |
| Ordinary counted | Backward | 210.046 | 190.386 | 1.103x |
| Ordinary counted | Random | 232.177 | 233.615 | 0.994x |
| Ordinary counted | Same leaf: forward | 23.167 | 11.940 | 1.940x |
| Ordinary counted | Same leaf: backward | 22.989 | 13.939 | 1.649x |
| Ordinary counted | Same leaf: random | 22.095 | 19.174 | 1.152x |
| Prefix counted | Forward | 206.541 | 186.347 | 1.108x |
| Prefix counted | Backward | 195.544 | 173.869 | 1.125x |
| Prefix counted | Random | 204.406 | 209.076 | 0.978x |
| Prefix counted | Same leaf: forward | 43.097 | 43.032 | 1.002x |
| Prefix counted | Same leaf: backward | 42.854 | 42.586 | 1.006x |
| Prefix counted | Same leaf: random | 39.861 | 40.508 | 0.984x |
| Prefix counted DUPSORT | Forward | 247.722 | 240.095 | 1.032x |
| Prefix counted DUPSORT | Backward | 216.579 | 203.252 | 1.066x |
| Prefix counted DUPSORT | Random | 240.940 | 235.085 | 1.025x |
| Prefix counted DUPSORT | Same leaf: forward | 68.686 | 67.536 | 1.017x |
| Prefix counted DUPSORT | Same leaf: backward | 55.889 | 55.962 | 0.999x |
| Prefix counted DUPSORT | Same leaf: random | 57.276 | 57.727 | 0.992x |

Longer ordinary counted same-leaf runs, with **2,000,000 puts and eight
alternating pairs**, confirm the improvement:

| Same-leaf pattern | Before (ms) | After (ms) | Speedup |
|---|---:|---:|---:|
| Forward | 228.428 | 115.412 | 1.979x |
| Backward | 228.704 | 136.472 | 1.676x |
| Random | 213.298 | 188.676 | 1.130x |

Cross-leaf forward/backward cases show 1.03–1.13x speedups. Across-tree random
times ranged from 2.4% faster to 2.3% slower. All 180 default samples and 48
longer same-leaf samples verified final values and counts, and each pair
produced matching tree shapes. Timings on this active machine vary with cache
and scheduling conditions.

## Startup Benchmark

`startup_bench` measures isolated database reopen cost across four variants:
plain, counted, prefix-compressed, and counted+prefix-compressed. Each run
builds comparable databases, then times:

* `startup_only` = `mdb_env_open` + `mdb_txn_begin` + `mdb_dbi_open`
* `startup_with_probe` = `startup_only` + first `mdb_get`

Example runs:

```
cd libraries/liblmdb
make startup_bench
./startup_bench -n 2000000 -r 30 -w 5
./startup_bench -n 2000000 -r 100 -w 10 -N
```

Observed results on a local workstation with 2,000,000 entries and 64-byte
values:

```
default locking
  plain           startup_only=0.056 ms   startup_with_probe=0.061 ms
  counted         startup_only=0.054 ms   startup_with_probe=0.059 ms
  prefix          startup_only=0.055 ms   startup_with_probe=0.059 ms
  counted+prefix  startup_only=0.063 ms   startup_with_probe=0.068 ms

MDB_NOLOCK
  plain           startup_only=0.036 ms   startup_with_probe=0.041 ms
  counted         startup_only=0.039 ms   startup_with_probe=0.045 ms
  prefix          startup_only=0.034 ms   startup_with_probe=0.038 ms
  counted+prefix  startup_only=0.034 ms   startup_with_probe=0.039 ms
```

In this harness, neither `MDB_COUNTED` nor `MDB_PREFIX_COMPRESSION` adds
meaningful startup overhead. The measured differences are only a few
microseconds and are dominated by constant reopen costs. Prefix compression
does, however, reduce the data file size substantially, which can help other
workloads.

This benchmark is intentionally narrow: it measures LMDB reopen cost, not full
application startup. If an application still shows slower startup, the next
step is to benchmark its actual open pattern, DBI count, and first-query path.

## Dupsort Bulk Iteration

Most Datalevin Datalog query workloads first seek to a key and then read every
duplicate in that set (analog to reading a row in row based RDBMS). Walking
`MDB_NEXT_DUP` for each value is cache-unfriendly, so LMDB now exposes
`mdb_cursor_list_dup()`: once a cursor is positioned on a key,
`mdb_cursor_list_dup(cursor, &vals, &count)` returns a read-only array of
`MDB_val` structures for all duplicates, including sets promoted to nested
B-trees. Promoted `MDB_DUPFIXED` sets use the
[LEAF2 page specialization](#leaf2-read-side-specialization) described above.
Normal prefix-compressed DUPSORT trees retain the scalar decode-and-copy
fallback, using cursor-owned storage so values remain valid after traversal.

`dup_iter_bench` compares the fast path with the per-duplicate loop:

```sh
cd libraries/liblmdb
make dup_iter_bench
./dup_iter_bench --keys 20000 --dups 20 --runs 5
```

Median results, 2026-09-20. Each invocation visits 100,000 keys and reads
2,000,000 duplicate values across five passes, using a 512 MiB map and
`MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOLOCK`.

| Method | Total time | Time/value |
|---|---:|---:|
| `MDB_NEXT_DUP` loop | 34.045 ms | 17.023 ns |
| `mdb_cursor_list_dup` | 21.052 ms | 10.526 ns |

For this workload, `mdb_cursor_list_dup` takes 38% less iteration time,
a 1.62x speedup. These are variable-width prefix-compressed duplicates;
fixed-width LEAF2 sets have the separate page-based results above.

## Interrupt handling

Long-running reads can be interrupted safely by setting an environment flag.
Call `mdb_env_set_interrupt(env, 1)` from a signal handler (e.g. SIGINT), and
in-flight operations will return `EINTR` and mark the active transaction as
failed. Abort/reset the transaction and clear the flag with
`mdb_env_set_interrupt(env, 0)` before retrying work.

```c
static volatile sig_atomic_t got_sigint;
static MDB_env *sig_env;

static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
    if (sig_env)
        mdb_env_set_interrupt(sig_env, 1);
}
```
