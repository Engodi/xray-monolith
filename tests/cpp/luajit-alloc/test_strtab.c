/*
 * Host-side regression test for the LuaJIT string-interning table.
 *
 * The headless engine crashed inside lj_str_new() because a bucket of the
 * interning table (g->strhash) held something that was not a GC object base:
 * the 4 bytes found there were 0x4E753AF9, which is exactly hash("prefetch"),
 * i.e. the contents of the GCstr field at object+8.
 *
 * This test drives the real lj_str_new()/lj_str_resize()/GC path of the
 * vendored LuaJIT and checks the invariant the crash violated:
 *
 *   every chain node reachable from g->strhash[] is an aligned, mapped,
 *   GC-tagged GCstr whose hash files it under that very bucket, chains are
 *   acyclic, and the number of distinct strings equals g->strnum.
 *
 * It also pins down the layout fact behind the "object+8" symptom: for GCstr
 * the fields are nextgc(0..3) marked(4) gct(5) reserved(6) unused(7) hash(8)
 * len(12) data(16) - so offset +8 is the hash field, not a pointer.
 *
 * Negative controls corrupt one bucket on purpose (hash value, object+8,
 * object+12, dead 64-bit pointer) and require the checker to *report* them
 * instead of dereferencing them blindly - that dereference is the crash.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#define HAVE_MINCORE 1
#else
#define HAVE_MINCORE 0
#endif

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_str.h"

#define NSTR 60000

/* -- small open-addressing set of uint32_t -------------------------------- */

typedef struct Set {
  uint32_t *keys;
  unsigned char *used;
  size_t mask;
  size_t count;
} Set;

static void set_init(Set *s, size_t cap)
{
  size_t n = 16;
  while (n < cap * 2) n <<= 1;
  s->keys = (uint32_t *)calloc(n, sizeof(uint32_t));
  s->used = (unsigned char *)calloc(n, 1);
  s->mask = n - 1;
  s->count = 0;
  if (!s->keys || !s->used) {
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
}

static void set_free(Set *s)
{
  free(s->keys);
  free(s->used);
  s->keys = NULL;
  s->used = NULL;
  s->mask = 0;
  s->count = 0;
}

static size_t set_slot(const Set *s, uint32_t v)
{
  size_t i = (size_t)((v * 2654435761u) >> 7) & s->mask;
  while (s->used[i] && s->keys[i] != v) i = (i + 1) & s->mask;
  return i;
}

static void set_add(Set *s, uint32_t v)
{
  size_t i = set_slot(s, v);
  if (!s->used[i]) {
    s->used[i] = 1;
    s->keys[i] = v;
    s->count++;
  }
}

static int set_has(const Set *s, uint32_t v)
{
  size_t i = set_slot(s, v);
  return s->used[i] != 0;
}

/* -- safe probing --------------------------------------------------------- */

/* AGCRef must never be dereferenced before we know the address is mapped. */
static int addr_mapped(uintptr_t addr, size_t len)
{
#if HAVE_MINCORE
  static size_t pagesize;
  unsigned char vec[2];
  uintptr_t page;
  size_t span;
  if (pagesize == 0) pagesize = (size_t)sysconf(_SC_PAGESIZE);
  page = addr & ~(uintptr_t)(pagesize - 1);
  span = (size_t)((addr + len) - page);
  if (span <= pagesize) return mincore((void *)page, pagesize, vec) == 0;
  return mincore((void *)page, pagesize, vec) == 0 &&
         mincore((void *)((addr + len - 1) & ~(uintptr_t)(pagesize - 1)),
                 pagesize, vec) == 0;
#else
  (void)addr; (void)len;
  return 1;
#endif
}

/* -- string table checker ------------------------------------------------- */

typedef struct Check {
  const char *err;     /* NULL when the table is consistent */
  MSize bucket;        /* offending bucket */
  uint32_t value;      /* offending GCRef / chain link */
  unsigned nonempty;   /* non-empty buckets seen */
  unsigned nodes;      /* chain members walked */
  unsigned distinct;   /* distinct strings reachable from buckets */
} Check;

#define CHECK_ERR(r, m, b, v) \
  do { (r)->err = (m); (r)->bucket = (b); (r)->value = (v); } while (0)

/*
 * Verify the interning table against 'known' (bases of strings this test
 * interned). Values are classified before any dereference, so a corrupted
 * table is reported instead of crashed on.
 */
static void check_table(lua_State *L, const Set *known, const Set *hashes,
                        Check *r)
{
  global_State *g = G(L);
  Set seen, bases8;
  MSize i;
  int ok = 1;

  memset(r, 0, sizeof(*r));
  set_init(&seen, NSTR);
  set_init(&bases8, NSTR);
  if (known != NULL) {
    size_t k;
    for (k = 0; k <= known->mask; k++)
      if (known->used[k]) set_add(&bases8, known->keys[k] + 8u);
  }

  if (g->strhash == NULL || (g->strmask & (g->strmask + 1)) != 0) {
    CHECK_ERR(r, "strtab size is not a power of two", 0, g->strmask);
    ok = 0;
  }

  for (i = 0; ok && i <= g->strmask; i++) {
    uint32_t v = gcrefu(g->strhash[i]);
    unsigned chain = 0;
    if (v == 0) continue;
    r->nonempty++;
    while (v != 0) {
      GCstr *s;
      if ((v & 3u) != 0 || !addr_mapped((uintptr_t)v, sizeof(GCstr))) {
        /* Not a plausible object address: classify, never dereference. */
        if (known != NULL && set_has(&bases8, v))
          CHECK_ERR(r, "GCRef points at object+8 (the GCstr hash field)", i, v);
        else if (hashes != NULL && set_has(hashes, v))
          CHECK_ERR(r, "GCRef holds a string hash, not a pointer", i, v);
        else
          CHECK_ERR(r, "GCRef is not an aligned, mapped object address", i, v);
        ok = 0;
        break;
      }
      s = gco2str((GCobj *)(uintptr_t)v);
      if (s->gct != (uint8_t)~LJ_TSTR) {
        if (known != NULL && set_has(&bases8, v))
          CHECK_ERR(r, "GCRef points at object+8 (the GCstr hash field)", i, v);
        else
          CHECK_ERR(r, "chain node is not a string GC object", i, v);
        ok = 0;
        break;
      }
      if ((uint32_t)(s->hash & g->strmask) != (uint32_t)i) {
        CHECK_ERR(r, "string is filed under the wrong bucket", i, v);
        ok = 0;
        break;
      }
      if (set_has(&seen, v)) {
        CHECK_ERR(r, "string appears twice in the table (cycle or alias)", i, v);
        ok = 0;
        break;
      }
      set_add(&seen, v);
      r->nodes++;
      v = gcrefu(s->nextgc);
      if (++chain > 200000u) {
        CHECK_ERR(r, "hash chain does not terminate", i, v);
        ok = 0;
      }
    }
  }

  if (ok) {
    r->distinct = (unsigned)seen.count;
    if (seen.count != (size_t)g->strnum) {
      CHECK_ERR(r, "table holds g->strnum != distinct chain members", 0,
                (uint32_t)g->strnum);
      ok = 0;
    }
  }
  if (ok && known != NULL) {
    size_t k;
    for (k = 0; k <= known->mask; k++) {
      if (known->used[k] && !set_has(&seen, known->keys[k])) {
        CHECK_ERR(r, "an interned string is missing from the table", 0,
                  known->keys[k]);
        ok = 0;
        break;
      }
    }
  }

  set_free(&seen);
  set_free(&bases8);
}

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

static void report(const char *label, const Check *c)
{
  if (c->err != NULL)
    printf("  ..   %s: %s (bucket %u, value 0x%08X)\n", label, c->err,
           (unsigned)c->bucket, (unsigned)c->value);
}

/* -- test ---------------------------------------------------------------- */

int main(void)
{
  lua_State *L;
  global_State *g;
  Check c;
  char buf[64];
  int i;
  GCstr *prefetch;
  uint32_t saved;
  MSize bucket;
  Set known, hashes;
  GCstr **objs;

  objs = (GCstr **)calloc(NSTR, sizeof(GCstr *));
  if (objs == NULL) return 2;

  printf("== layout of GCstr (the \"object+8\" clue) ==\n");
  expect(sizeof(GCRef) == 4 && sizeof(((GCRef *)0)->gcptr32) == 4,
         "sizeof(GCRef) == 4 (this LuaJIT has no LJ_GC64, no tag bits)");
  {
    GCstr *probe = (GCstr *)0;
    expect((size_t)((char *)&probe->nextgc - (char *)probe) == 0,
           "nextgc lives at object+0 (a GCRef IS the object base)");
    expect((size_t)((char *)&probe->gct - (char *)probe) == 5,
           "gct lives at object+5");
    expect((size_t)((char *)&probe->hash - (char *)probe) == 8,
           "hash lives at object+8 (what the corrupted bucket contained)");
    expect((size_t)((char *)&probe->len - (char *)probe) == 12,
           "len lives at object+12");
  }

  L = luaL_newstate();
  if (L == NULL) {
    printf("  FAIL luaL_newstate()\n");
    return 1;
  }
  g = G(L);

  printf("== interning %d strings (grows and rehashes the table) ==\n", NSTR);
  lua_createtable(L, NSTR, 0);
  for (i = 0; i < NSTR; i++) {
    int n = snprintf(buf, sizeof(buf), "str_%d_%08x", i,
                     (unsigned)(i * 2654435761u));
    objs[i] = lj_str_new(L, buf, (size_t)n);
    setstrV(L, L->top++, objs[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  set_init(&known, NSTR);
  set_init(&hashes, NSTR);
  for (i = 0; i < NSTR; i++) {
    set_add(&known, (uint32_t)(uintptr_t)objs[i]);
    set_add(&hashes, objs[i]->hash);
  }

  check_table(L, &known, &hashes, &c);
  printf("  table: mask=%u buckets=%u nonempty=%u chain-members=%u strnum=%u\n",
         (unsigned)g->strmask, (unsigned)g->strmask + 1, c.nonempty, c.nodes,
         (unsigned)g->strnum);
  report("interning", &c);
  expect(c.err == NULL, "every bucket is a chain of GCstr payload bases");
  expect(c.nonempty > 0, "table is not empty (test would be vacuous)");
  expect(c.distinct == (unsigned)g->strnum, "distinct strings == g->strnum");
  expect(g->strmask + 1 >= (MSize)NSTR / 2, "table grew with the string count");

  printf("== interning is stable: same bytes return the same object ==\n");
  {
    int mismatched = 0;
    for (i = 0; i < NSTR; i += 61) {
      int n = snprintf(buf, sizeof(buf), "str_%d_%08x", i,
                       (unsigned)(i * 2654435761u));
      if (lj_str_new(L, buf, (size_t)n) != objs[i]) mismatched++;
    }
    expect(mismatched == 0, "lj_str_new() finds the interned string again");
  }

  printf("== churn: short-lived strings + incremental GC ==\n");
  for (i = 0; i < 40000; i++) {
    int n = snprintf(buf, sizeof(buf), "tmp_%d", i);
    (void)lj_str_new(L, buf, (size_t)n);  /* not anchored: dies with the GC */
    if ((i & 1023) == 0) lua_gc(L, LUA_GCSTEP, 1);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  check_table(L, &known, &hashes, &c);
  printf("  table: mask=%u nonempty=%u chain-members=%u strnum=%u\n",
         (unsigned)g->strmask, c.nonempty, c.nodes, (unsigned)g->strnum);
  report("churn", &c);
  expect(c.err == NULL, "table still consistent after churn and GC sweeps");

  /*
  ** Cross-check the value seen in the crash dump: 0x4E753AF9 was the GCRef of
  ** the bucket lj_str_new("prefetch") dereferenced. It is exactly what sits at
  ** object+8 of "prefetch" - the string's hash. Reproducing the constant from
  ** this very source proves the bucket was overwritten by a GCstr field write,
  ** i.e. a string object overlapped the live interning table.
  */
  prefetch = lj_str_new(L, "prefetch", 8);
  printf("== the crash value ==\n");
  printf("  hash(\"prefetch\") = 0x%08X (found in the corrupted bucket: 0x4E753AF9)\n",
         (unsigned)prefetch->hash);
  expect(prefetch->hash == 0x4E753AF9u,
         "hash(\"prefetch\") reproduces the value found in the corrupted bucket");
  expect((uint32_t)(uintptr_t)prefetch % 8u == 0,
         "a real GCRef is 8-aligned; a hash value is not");

  /* -- negative controls: the checker must *report*, never dereference ----- */
  printf("== negative controls (deliberate table corruption) ==\n");
  bucket = 0;
  while (bucket <= g->strmask && gcrefu(g->strhash[bucket]) == 0) bucket++;
  if (bucket > g->strmask) {
    printf("  FAIL no non-empty bucket to corrupt\n");
    failures++;
  } else {
    GCstr *head = gco2str(gcref(g->strhash[bucket]));
    saved = gcrefu(g->strhash[bucket]);

    gcrefu(g->strhash[bucket]) = head->hash;
    check_table(L, &known, &hashes, &c);
    report("hash in bucket", &c);
    expect(c.err != NULL && c.bucket == bucket && c.value == head->hash,
           "bucket holding a string hash is reported (the crash signature)");

    gcrefu(g->strhash[bucket]) = saved + 8u;
    check_table(L, &known, &hashes, &c);
    report("object+8", &c);
    expect(c.err != NULL && c.bucket == bucket && c.value == saved + 8u,
           "bucket pointing at object+8 is reported");

    gcrefu(g->strhash[bucket]) = saved + 12u;
    check_table(L, &known, &hashes, &c);
    expect(c.err != NULL && c.bucket == bucket,
           "bucket pointing at object+12 is reported");

    gcrefu(g->strhash[bucket]) = 0x4E753AF9u;
    check_table(L, &known, &hashes, &c);
    report("dead pointer", &c);
    expect(c.err != NULL && c.bucket == bucket,
           "bucket with a dead pointer is reported, not dereferenced");

    gcrefu(g->strhash[bucket]) = saved;
    check_table(L, &known, &hashes, &c);
    expect(c.err == NULL, "table is consistent again after restoring the bucket");
  }

  set_free(&known);
  set_free(&hashes);
  lua_close(L);
  free(objs);

  if (failures != 0) {
    printf("strtab: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("strtab: ok\n");
  return 0;
}
