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

## In-memory vs file-backed benchmark

`inmem_bench` runs the same write/read workload on file-backed and in-memory
environments and reports average timings and speedup.

Example run:

```
./inmem_bench --entries 200000 --value-size 64 --rounds 3 --no-lock
entries=200000 value_size=64 rounds=3 mapsize=76800000
file-backed avg: write=95.74 ms read=70.47 ms checksum=25493856
in-memory  avg: write=69.08 ms read=66.47 ms checksum=25493856
speedup: write=1.39x read=1.06x
```

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

## Counted DB Benchmark

`count_bench` exercises both naive cursor scans and the counted APIs. With
100,000 entries, 200 sampled queries, a span of 10,000 keys, and 20 duplicates
per key:

```
./count_bench --entries 100000 --queries 200 --span 10000 --dups 20 --shuffle
Benchmark with 100000 entries, 200 queries, span 10000
Insert order: shuffled

== Plain DB Inserts ==
  plain:   54.28 ms (0.54 us/op)
  counted: 55.31 ms (0.55 us/op)
  overhead: 1.03 ms (1.89%)

== Range Count (keys) ==
  naive cursor: 30.01 ms (150.06 us/op)
  counted API:  0.18 ms (0.88 us/op)
  speedup: 170.52x

== Rank Lookup ==
  naive (sampled 128): 70.69 ms (552.30 us/op)
  cursor API:        0.09 ms (0.43 us/op)
  mdb_get_rank:      0.05 ms (0.23 us/op)
  speedup: 1269.67x

== Dupsort Inserts ==
  plain:   960.70 ms (0.48 us/op)
  counted: 979.46 ms (0.49 us/op)
  overhead: 18.75 ms (1.95%)

== Dupsort Range Count ==
  dup/key: 20
  cursor (mdb_cursor_count): 74.21 ms (371.04 us/op)
  counted API:              0.23 ms (1.15 us/op)
  speedup: 324.05x
```

In short: counted metadata adds negligible write-time overhead while delivering
two to three orders of magnitude acceleration for range counts and rank lookups.

### Sparse sampling

`sample_bench` focuses on the sampling workload: reading a tiny subset of
entries (e.g. 1000 ranks out of 10000000) without materializing the skipped
span. It builds a counted database (plain or dupsort) and compares two
strategies:

* **Warm sequential** – repeated `mdb_cursor_get_rank` calls using the cached
  cursor state that the counted tree exposes. This is the optimized path.
* **Stride scan (MDB_NEXT)** – a baseline that seeds the cursor once and then
  advances via `MDB_NEXT` for every skipped record, i.e. a traditional cursor walk.

Example runs on a single SSD-backed workstation (stride of 10000, 1000 samples):

```
./sample_bench --entries 10000000 --samples 1000 --batch 250000 --mode both \
               --path ./bench_sample_plain
Preparing 10000000 entries (batch 250000, stride 10000, samples 1000, dups 1)
Population: 1789.28 ms (5.59 M entries/sec)
Warm sequential: samples=1000 total=1.71 ms avg=1.71 us/op
Stride scan (MDB_NEXT): samples=1000 total=92.36 ms avg=92.36 us/op

./sample_bench --entries 10000000 --samples 1000 --batch 250000 --mode both \
               --dups 8 --path ./bench_sample_dups
Preparing 10000000 entries (batch 250000, stride 10000, samples 1000, dups 8)
Population: 4444.42 ms (2.25 M entries/sec)
Warm sequential: samples=1000 total=1.55 ms avg=1.55 us/op
Stride scan (MDB_NEXT): samples=1000 total=153.60 ms avg=153.60 us/op
```

For both plain and dupsort databases, the counted cursor path is roughly two
orders of magnitude faster than a sequential scan when sampling sparsely.

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

`compress_bench` measures workloads with and without prefix compression. With
1,000,000 entries, 64-byte values, 16-byte shared prefixes, and
duplicate-heavy traffic:

```
./compress_bench -n 1000000 -r 500000 -v 64 -p 16 -m 4096  -U 500000 -X 500000 -D 20
=== plain (plain, unique) ===
Entries: 1000000, Value bytes: 64, Prefix bytes: 16
Insert: 892.050 ms (0.892 us/op, 1121013 op/s over 1000000 ops)
Update: 429.478 ms (0.859 us/op, 1164204 op/s over 500000 ops)
Delete: 664.230 ms (1.328 us/key, 752751 key/s over 500000 keys)
Reinsert: 506.389 ms (1.013 us/key, 987383 key/s over 500000 keys)
Random Read (cold): 385.790 ms (0.772 us/op, 1296042 op/s over 500000 ops)
Random Read (warm): 376.617 ms (0.753 us/op, 1327609 op/s over 500000 ops)
Range Scan (cold): 6.989 ms (0.027 us/key, 36628988 key/s over 256000 keys)
Range Scan (warm): 4.172 ms (0.016 us/key, 61361457 key/s over 256000 keys)
Files: data 440.25 MiB, lock 0.00 B (total 440.25 MiB)
Map: 440.25 MiB used / 4.00 GiB configured
Tree: depth=3, pages(branch=33, leaf=9348, overflow=0), page size=16384

=== prefix (prefix, unique) ===
Entries: 1000000, Value bytes: 64, Prefix bytes: 16
Insert: 901.957 ms (0.902 us/op, 1108700 op/s over 1000000 ops)
Update: 378.016 ms (0.756 us/op, 1322695 op/s over 500000 ops)
Delete: 764.350 ms (1.529 us/key, 654151 key/s over 500000 keys)
Reinsert: 504.187 ms (1.008 us/key, 991696 key/s over 500000 keys)
Random Read (cold): 364.227 ms (0.728 us/op, 1372770 op/s over 500000 ops)
Random Read (warm): 332.968 ms (0.666 us/op, 1501646 op/s over 500000 ops)
Range Scan (cold): 6.535 ms (0.026 us/key, 39173680 key/s over 256000 keys)
Range Scan (warm): 4.461 ms (0.017 us/key, 57386236 key/s over 256000 keys)
Files: data 331.89 MiB, lock 0.00 B (total 331.89 MiB)
Map: 331.89 MiB used / 4.00 GiB configured
Tree: depth=3, pages(branch=33, leaf=7040, overflow=0), page size=16384

=== plain-dups (plain, dupsort) ===
Entries: 1000000, Value bytes: 64, Prefix bytes: 16, Duplicates/key target: 20 (~50000 unique keys)
Insert: 1221.752 ms (1.222 us/op, 818497 op/s over 1000000 ops)
Update: 970.830 ms (1.942 us/op, 515023 op/s over 500000 ops)
Delete: 612.807 ms (1.226 us/key, 815918 key/s over 500000 keys)
Reinsert: 706.770 ms (1.414 us/key, 707444 key/s over 500000 keys)
Random Read (cold): 384.830 ms (0.770 us/op, 1299275 op/s over 500000 ops)
Random Read (warm): 373.916 ms (0.748 us/op, 1337199 op/s over 500000 ops)
Range Scan (cold): 6.538 ms (0.026 us/key, 39155705 key/s over 256000 keys)
Range Scan (warm): 4.442 ms (0.017 us/key, 57631697 key/s over 256000 keys)
Files: data 339.59 MiB, lock 0.00 B (total 339.59 MiB)
Map: 339.59 MiB used / 4.00 GiB configured
Tree: depth=3, pages(branch=33, leaf=7206, overflow=0), page size=16384

=== prefix-dups (prefix, dupsort) ===
Entries: 1000000, Value bytes: 64, Prefix bytes: 16, Duplicates/key target: 20 (~50000 unique keys)
Insert: 1025.675 ms (1.026 us/op, 974968 op/s over 1000000 ops)
Update: 762.434 ms (1.525 us/op, 655794 op/s over 500000 ops)
Delete: 361.905 ms (0.724 us/key, 1381578 key/s over 500000 keys)
Reinsert: 476.998 ms (0.954 us/key, 1048222 key/s over 500000 keys)
Random Read (cold): 308.160 ms (0.616 us/op, 1622534 op/s over 500000 ops)
Random Read (warm): 297.377 ms (0.595 us/op, 1681367 op/s over 500000 ops)
Range Scan (cold): 5.128 ms (0.020 us/key, 49921997 key/s over 256000 keys)
Range Scan (warm): 4.360 ms (0.017 us/key, 58715596 key/s over 256000 keys)
Files: data 77.44 MiB, lock 0.00 B (total 77.44 MiB)
Map: 77.44 MiB used / 4.00 GiB configured
Tree: depth=3, pages(branch=9, leaf=1679, overflow=0), page size=16384

--- Relative to plain ---
Insert time: 1.011x (892.050 ms -> 901.957 ms)
Update time: 0.880x (429.478 ms -> 378.016 ms)
Delete time: 1.151x (664.230 ms -> 764.350 ms)
Reinsert time: 0.996x (506.389 ms -> 504.187 ms)
Random read (warm): 0.884x
Random read (cold): 0.944x
Range scan (warm): 1.069x
Data size: 0.754x (461635584 -> 348012544 bytes)
Map used: 0.754x (440.25 MiB -> 331.89 MiB)
Leaf pages: 0.753x (9348 -> 7040)

--- Relative to plain-dups ---
Insert time: 0.840x (1221.752 ms -> 1025.675 ms)
Update time: 0.785x (970.830 ms -> 762.434 ms)
Delete time: 0.591x (612.807 ms -> 361.905 ms)
Reinsert time: 0.675x (706.770 ms -> 476.998 ms)
Random read (warm): 0.795x
Random read (cold): 0.801x
Range scan (warm): 0.982x
Data size: 0.228x (356089856 -> 81199104 bytes)
Map used: 0.228x (339.59 MiB -> 77.44 MiB)
Leaf pages: 0.233x (7206 -> 1679)

```

For this highly redundant data set, prefix compression therefore shrinks on-disk
footprint by ~25 % for regular database and ~75 % for DUPSORT database, while
keeping read/write throughput on par with the uncompressed baseline. In fact,
compressed performance is generally slightly better than the uncompressed cases
in DUPSORT workloads. As Datalevin's triple storage uses this format, we did
more optimizations.

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
`mdb_cursor_list_dup(cursor, &vals, &count)` materializes all inline duplicates
into the cursor’s leaf cache and returns a read-only array of `MDB_val`
structures. When the duplicates were promoted to a sub-database (very large
dupsets) the call falls back to `MDB_INCOMPATIBLE`, signalling the caller to use
the classic loop. Prefix-compressed counted databases benefit automatically
because the decoded-leaf cache is already hot.

`dup_iter_bench` compares the fast path with the old per-duplicate loop:

```
cd libraries/liblmdb
make dup_iter_bench
./dup_iter_bench --keys 20000 --dups 20 --runs 5
Benchmark configuration: keys=20000 dups/key=20 runs=5
Total key visits: 100000, total duplicates read: 2000000
mdb_cursor_list_dup: 19.270 ms (0.193 us/key, 9.635 ns/value)
MDB_NEXT_DUP loop:   34.006 ms (0.340 us/key, 17.003 ns/value)
```

By keeping duplicate iteration inside the cache and avoiding repeated cursor
jumps, `mdb_cursor_list_dup` cuts the inner-loop cost roughly in half for common
key/dups iteration.

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
