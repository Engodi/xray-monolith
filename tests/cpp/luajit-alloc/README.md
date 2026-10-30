# LuaJIT memory layer — host checks (`luajit-alloc`)

Host-side (Linux/gcc) checks for the two halves of the LuaJIT memory layer the
engine ships on Windows. No MSVC and no GPU needed; the shipped sources are
compiled unmodified.

```
tests/cpp/luajit-alloc/run.sh
```

## Why this exists

The headless engine crashed inside `lj_str_new()` (RVA `0x1347B0`, interning
`"prefetch"`): a bucket of the interning table `g->strhash[]` held
`0x4E753AF9` instead of a `GCRef`. That value is not arbitrary — it is exactly
`hash("prefetch")`, and for a `GCstr` the hash sits at **object + 8**:

```
GCstr:  nextgc(0..3) marked(4) gct(5) reserved(6) unused(7) hash(8) len(12) data(16)
```

So the bucket was overwritten by `s->hash = h` of a string object whose payload
address was `bucket - 8`: a live `GCstr` was allocated *inside* the live
interning table. Nothing in LuaJIT writes `object + 8` into a `GCRef`
(`GCRef` is a 4-byte struct, `setgcref` stores `&gc->gch`, and the table is
written in exactly two places, `lj_str.c:88` and `lj_str.c:158`, both with an
object base). What can produce that shape is the memory underneath: the pool
handing out — or releasing — memory that was still owned, after dlmalloc's
state (which lives at the pool base, `pool_base + 16`) was damaged by a foreign
write.

## What is checked

| file | what it drives | invariant |
| --- | --- | --- |
| `test_strtab.c` | real `lj_str_new()` / `lj_str_resize()` / GC from the vendored tree (60k strings, resize, churn) | every chain node reachable from `g->strhash[]` is an aligned, mapped, `~LJ_TSTR`-tagged object base filed under that very bucket, chains are acyclic, distinct strings == `g->strnum`; `object+8`, `object+12`, a bare hash or a dead pointer are *reported*, never dereferenced |
| `test_pool.c` | real `xr_alloc.c` (512 MB `NtAllocateVirtualMemory` pool, 64 KB chunks) | never hands out or releases live memory: distinct regions, in-pool, 64 KB aligned, refusals for size 0/`SIZE_MAX`/oversized, no-op for a free of an address the pool never handed out or that runs past the pool end, 6000 random map/free ops without an alias |
| `test_alloc_pool.c` | real dlmalloc (`lj_alloc.c` built with `LUAJIT_OS_WINDOWS`, so `CALL_MMAP`/`CALL_MUNMAP` go through `XR_MMAP`/`XR_DESTROY`) on top of that pool | 40 000 malloc/realloc/free ops, direct-mapped (>128 KB) chunks, `alloc_trim()` after big frees and a pool reuse after `lj_alloc_destroy()` — every live block carries a unique byte pattern, so an alias shows up as a corrupted block |

`stub/windows.h` supplies the handful of declarations `xr_alloc.c` needs and
lets the test provide the pool-backed `NtAllocateVirtualMemory` (both
`lj_alloc.c` and `xr_alloc.c` resolve it through `GetProcAddress`).

## Regression pinned by these tests

`XR_MMAP`/`XR_DESTROY` used to convert sizes with `(x + 65535) / 65536` in
`int` and addresses with `(ptr - g_heap) / CHUNK_SIZE` rounded *down*, then mark
`CHUNKS_FROM_SIZE(size)` chunks starting there (rounded *up*). Consequences
fixed in `xr_alloc.c`:

* `XR_MMAP(0)` (and any size that wrapped the chunk count to 0) returned
  `g_firstFreeChunk` **without marking it** — the same live region could be
  handed out again and again.
* `XR_DESTROY` on an address the pool never handed out (an unaligned or stale
  pointer — e.g. a truncated 64-bit address) marked the *chunk containing* it
  free, i.e. resurrected live memory.
* `XR_DESTROY` past the pool end indexed `g_heapMap` out of bounds; overwriting
  the `'\0'` terminator made `find_free()` walk past the map and return
  addresses **outside the pool** (`0x1fff0000` for a pool at `0x20000000` in the
  host test).
* `find_free()` stopped on the terminator byte instead of the map size.

All four are reproduced by `test_pool.c` (they fail on the previous revision)
and are now refused/clamped: only chunks covered by the request *entirely* are
released, and requests that cannot be served unambiguously fail with `MFAIL`.

Note for the record: stock dlmalloc requests are all 64 KB aligned, so the old
code is self-consistent on the happy path — `test_alloc_pool.c` passes on both
revisions. The guard is about the *unhappy* path, which is what a 64-bit engine
hitting a 32-bit pseudo-pointer VM produces.
