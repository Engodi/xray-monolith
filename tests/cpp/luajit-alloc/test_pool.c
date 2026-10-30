/*
 * Host-side test of the LuaJIT pool allocator that the engine installs on
 * Windows: src/3rd party/luajit-2/src/xr_alloc.c (512 MB reserved through
 * NtAllocateVirtualMemory, handed out to dlmalloc in 64 KB chunks).
 *
 * Why: the headless crash in lj_str_new() was a bucket of the string table
 * holding a GCstr field value - a 4-byte write that landed inside the live
 * interning table. Only one thing can do that to a live allocation: the pool
 * handing out (or un-mapping) memory that is still owned. This test drives the
 * real XR_MMAP/XR_DESTROY and asserts the pool never does that, with the
 * surrounding invariants (chunk alignment, inside the pool, request bounds).
 *
 * The pool is driven like dlmalloc drives it (DEFAULT_GRANULARITY = 128 KB
 * segments, direct-mapped regions, segment trim), plus the hostile inputs a
 * 64 KB-granular pool must survive: a zero/oversized request, a free of an
 * address the pool never handed out, a free that runs past the pool end.
 *
 * Compiled against stub/windows.h, so the shipped source is used unmodified.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#else
#error "host-side test: Linux only"
#endif

#include <windows.h>

#include "xr_alloc.h"

/* Same signature xr_alloc.c/lj_alloc.c use for NtAllocateVirtualMemory. */
typedef long (*PNTAVM)(HANDLE handle, void **addr, ULONG zbits, size_t *size,
                       ULONG alloctype, ULONG prot);
PNTAVM ntavm;

extern void *g_heap;

/* XR_EARLY_INIT() resolves NtAllocateVirtualMemory through GetProcAddress; the
** stub returns whatever the test installs here. */
void *xr_host_getprocaddr_result;

#define POOL_CHUNK (64u * 1024u)
#define POOL_CHUNKS 8192u
#define POOL_SIZE ((size_t)POOL_CHUNK * POOL_CHUNKS)
#define MFAIL ((void *)(~(size_t)0))

static char *pool_base;
static size_t pool_size;

/* -- pool-backed NtAllocateVirtualMemory --------------------------------- */

static long host_ntavm(HANDLE handle, void **addr, ULONG zbits, size_t *size,
                       ULONG alloctype, ULONG prot)
{
  void *want = *addr != NULL ? *addr : (void *)(uintptr_t)0x20000000u;
  void *p;
  (void)handle;
  (void)zbits;
  (void)alloctype;
  (void)prot;
  if (*size == 0) return -1;
  p = mmap(want, *size, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED) {
    p = mmap(NULL, *size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return -1;
  }
  if ((uintptr_t)p + *size > 0x100000000ull) { /* the VM needs < 4 GB */
    munmap(p, *size);
    return -1;
  }
  *addr = p;
  return 0;
}

/* -- model of what the caller (dlmalloc) still owns ----------------------- */

typedef struct Range {
  uintptr_t lo, hi;
} Range;

#define MAX_LIVE 8192
static Range live[MAX_LIVE];
static int nlive;

static int overlaps(uintptr_t lo, uintptr_t hi)
{
  int i;
  for (i = 0; i < nlive; i++)
    if (lo < live[i].hi && live[i].lo < hi) return 1;
  return 0;
}

/* Remove [lo,hi) from the owned ranges (a caller-visible free). */
static void model_free(uintptr_t lo, uintptr_t hi)
{
  Range out[MAX_LIVE];
  int n = 0, i;
  for (i = 0; i < nlive; i++) {
    uintptr_t l = live[i].lo, h = live[i].hi;
    if (h <= lo || hi <= l) {
      out[n++] = live[i];
      continue;
    }
    if (l < lo) { out[n].lo = l; out[n].hi = lo; n++; }
    if (hi < h) { out[n].lo = hi; out[n].hi = h; n++; }
  }
  memcpy(live, out, (size_t)n * sizeof(Range));
  nlive = n;
}

/* -- helpers ------------------------------------------------------------- */

static int failures;

static void expect(int cond, const char *what)
{
  if (cond) {
    printf("  ok   %s\n", what);
  } else {
    printf("  FAIL %s\n", what);
    failures++;
  }
}

static int in_pool(const void *p, size_t size)
{
  uintptr_t a = (uintptr_t)p;
  return p != NULL && p != MFAIL && a >= (uintptr_t)pool_base &&
         a + size >= a && a + size <= (uintptr_t)pool_base + POOL_SIZE;
}

/* Map like dlmalloc would and check the returned region; 1 on success. */
static void *map_checked(size_t size, const char *what)
{
  void *p = XR_MMAP(size);
  size_t span = ((size + POOL_CHUNK - 1) / POOL_CHUNK) * POOL_CHUNK;
  if (p == NULL || p == MFAIL) return NULL;
  if (!in_pool(p, span)) {
    printf("  FAIL %s: returned %p, outside the pool\n", what, p);
    failures++;
    return NULL;
  }
  if ((uintptr_t)p % POOL_CHUNK != 0) {
    printf("  FAIL %s: returned %p, not 64K aligned\n", what, p);
    failures++;
    return NULL;
  }
  if (overlaps((uintptr_t)p, (uintptr_t)p + span)) {
    printf("  FAIL %s: returned %p, overlapping a live allocation\n", what, p);
    failures++;
    return NULL;
  }
  if (nlive >= MAX_LIVE) {
    printf("  FAIL model full\n");
    failures++;
    return NULL;
  }
  live[nlive].lo = (uintptr_t)p;
  live[nlive].hi = (uintptr_t)p + span;
  nlive++;
  return p;
}

static void free_checked(void *p, size_t size)
{
  if (p != NULL && p != MFAIL) {
    model_free((uintptr_t)p, (uintptr_t)p + size);
    XR_DESTROY(p, size);
  }
}

/* -- tests --------------------------------------------------------------- */

int main(void)
{
  void *a, *b;
  int i;

  xr_host_getprocaddr_result = (void *)host_ntavm;
  ntavm = host_ntavm;
  XR_INIT();

  printf("== pool setup ==\n");
  pool_base = (char *)g_heap;
  pool_size = POOL_SIZE;
  expect(pool_base != NULL, "XR_INIT() reserved the pool");
  if (pool_base == NULL) {
    printf("pool: cannot continue without a pool\n");
    return 1;
  }
  expect((uintptr_t)pool_base % POOL_CHUNK == 0, "pool base is 64K aligned");
  expect((uintptr_t)pool_base + POOL_SIZE <= 0x100000000ull,
         "pool fits below 4 GB (GCRef is 32-bit)");

  printf("== dlmalloc-like usage (128K segments, direct regions, trim) ==\n");
  {
    void *seg[8];
    for (i = 0; i < 8; i++) seg[i] = map_checked(128 * 1024, "segment");
    expect(nlive == 8, "eight 128K segments are distinct");
    free_checked(seg[2], 128 * 1024);
    free_checked(seg[3], 128 * 1024);
    a = map_checked(256 * 1024, "merged reuse");
    expect(a == seg[2], "a freed run is reused by the next segment");
    free_checked(a, 256 * 1024);
    for (i = 0; i < 8; i++) free_checked(seg[i], 128 * 1024);
    /* alloc_trim(): dlmalloc releases the tail of a segment and keeps the head */
    a = map_checked(512 * 1024, "big segment");
    if (a != NULL) {
      model_free((uintptr_t)a + 128 * 1024, (uintptr_t)a + 512 * 1024);
      XR_DESTROY((char *)a + 128 * 1024, 384 * 1024);
      b = map_checked(384 * 1024, "trim tail");
      expect(b == (char *)a + 128 * 1024, "a trimmed segment tail is reusable");
      free_checked(b, 384 * 1024);
      free_checked(a, 128 * 1024);
    }
  }

  printf("== a request of an impossible size must fail, not alias ==\n");
  a = XR_MMAP(64 * 1024);
  expect(a != NULL && a != MFAIL, "baseline 64K allocation works");
  b = XR_MMAP(0);
  expect(b == NULL || b == MFAIL, "XR_MMAP(0) is refused");
  b = XR_MMAP(~(size_t)0);
  expect(b == NULL || b == MFAIL, "XR_MMAP(SIZE_MAX) is refused (no wraparound)");
  b = XR_MMAP(POOL_SIZE + POOL_CHUNK);
  expect(b == NULL || b == MFAIL, "XR_MMAP(> pool) is refused");
  if (b != NULL && b != MFAIL) {
    printf("  ..   got %p for an oversized request\n", b);
    model_free((uintptr_t)b, (uintptr_t)b + 64 * 1024);
    XR_DESTROY(b, 64 * 1024);
  }
  /* Whatever the refusals did, the pool must still hand out memory that is not
  ** the allocation we are holding: a refused request must not mark anything. */
  {
    void *c = map_checked(64 * 1024, "after refusals");
    expect(c != NULL, "the pool still works after refused requests");
    expect(c != a, "a refused request did not alias the live allocation");
    free_checked(c, 64 * 1024);
  }
  free_checked(a, 64 * 1024);

  printf("== a free of memory the pool never handed out must be a no-op ==\n");
  a = map_checked(128 * 1024, "victim");
  if (a != NULL) {
    /* dlmalloc never does this, but a corrupted caller (or a stale pointer
    ** from a truncated 64-bit address) can; freeing a piece in the middle of a
    ** live region must never resurrect the chunk it sits in. */
    model_free((uintptr_t)a + 128, (uintptr_t)a + 128 + 64 * 1024);
    XR_DESTROY((char *)a + 128, 64 * 1024);
    b = map_checked(64 * 1024, "after foreign free");
    expect(b != NULL && b != a, "a foreign free did not hand out live memory");
    if (b != NULL && b != a) free_checked(b, 64 * 1024);
    free_checked(a, 128 * 1024);
  }

  printf("== frees that run past the pool must not corrupt the pool map ==\n");
  {
    int live_before = nlive;
    XR_DESTROY(pool_base + POOL_SIZE, POOL_CHUNK);          /* one past the end */
    XR_DESTROY(pool_base + POOL_SIZE + 1024 * 1024, POOL_CHUNK);
    XR_DESTROY(pool_base - POOL_CHUNK, POOL_CHUNK);         /* before the pool */
    XR_DESTROY(pool_base, POOL_SIZE + POOL_CHUNK);          /* oversized */
    expect(nlive == live_before, "out-of-pool frees hurt nothing we hold");
    a = map_checked(64 * 1024, "after oob frees");
    expect(a != NULL, "the pool still hands out in-pool memory");
    if (a != NULL) free_checked(a, 64 * 1024);
  }

  printf("== the whole pool can be handed out and returned ==\n");
  {
    static void *all[POOL_CHUNKS];
    int got = 0, ok = 1;
    for (i = 0; i < (int)POOL_CHUNKS; i++) {
      all[i] = map_checked(POOL_CHUNK, "fill");
      if (all[i] == NULL) { ok = 0; break; }
      got++;
    }
    expect(ok && got == (int)POOL_CHUNKS, "all 8192 chunks are usable");
    expect(XR_MMAP(POOL_CHUNK) == NULL || XR_MMAP(POOL_CHUNK) == MFAIL,
           "an exhausted pool refuses further requests");
    for (i = 0; i < got; i++) free_checked(all[i], POOL_CHUNK);
  }
  expect(nlive == 0, "everything was returned");

  printf("== fuzz: random sizes and frees, never an alias ==\n");
  {
    static void *h[256];
    static size_t hs[256];
    unsigned seed = 1234567u;
    int k, aliases = 0;
    for (k = 0; k < 256; k++) { h[k] = NULL; hs[k] = 0; }
    for (k = 0; k < 6000; k++) {
      int slot = (int)((seed = seed * 1103515245u + 12345u) >> 16) % 256;
      if (h[slot] == NULL) {
        size_t size = (size_t)(((seed >> 8) % 40u) + 1u) * POOL_CHUNK;
        int before = failures;
        h[slot] = map_checked(size, "fuzz");
        if (h[slot] != NULL) hs[slot] = size;
        if (failures != before) { aliases++; break; }
        if (h[slot] == NULL) { hs[slot] = 0; continue; }
      } else {
        free_checked(h[slot], hs[slot]);
        h[slot] = NULL;
        hs[slot] = 0;
      }
    }
    for (k = 0; k < 256; k++)
      if (h[k] != NULL) free_checked(h[k], hs[k]);
    expect(aliases == 0, "6000 random map/free operations without an alias");
    expect(nlive == 0, "fuzz left no dangling region behind");
  }

  if (failures != 0) {
    printf("pool: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("pool: ok\n");
  return 0;
}
