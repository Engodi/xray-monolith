/*
 * Host-side test of the *combination* the engine actually runs on Windows:
 * dlmalloc (src/3rd party/luajit-2/src/lj_alloc.c) on top of the 512 MB pool
 * (xr_alloc.c), both taken from the tree unmodified.
 *
 * The crash in lj_str_new() was a live LuaJIT object overwritten by another
 * live object, i.e. the allocator handed out (or unmapped) memory twice. This
 * test makes that visible without a debugger: every live block carries a unique
 * byte pattern, so an alias shows up as a corrupted block. It also drives the
 * paths that call into the pool with unusual sizes: direct-mapped chunks
 * (>= 128 KB requests), realloc/shrink, and alloc_trim() after big frees.
 *
 * lj_alloc.c is compiled with LUAJIT_OS_WINDOWS so that its CALL_MMAP/CALL_MUNMAP
 * really go through XR_MMAP/XR_DESTROY instead of plain mmap().
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__linux__)
#error "host-side test: Linux only"
#endif

#include <sys/mman.h>
#include <unistd.h>

#include <windows.h>

#include "lj_alloc.h"
#include "xr_alloc.h"

typedef long (*PNTAVM)(HANDLE handle, void **addr, ULONG zbits, size_t *size,
                       ULONG alloctype, ULONG prot);

extern PNTAVM ntavm;
extern void *g_heap;

void *xr_host_getprocaddr_result;

#define POOL_CHUNK (64u * 1024u)
#define POOL_SIZE (512u * 1024u * 1024u)

static long host_ntavm(HANDLE handle, void **addr, ULONG zbits, size_t *size,
                       ULONG alloctype, ULONG prot)
{
  void *want = *addr != NULL ? *addr : (void *)(uintptr_t)0x30000000u;
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
  if ((uintptr_t)p + *size > 0x100000000ull) {
    munmap(p, *size);
    return -1;
  }
  *addr = p;
  return 0;
}

/* -- live blocks with a unique pattern ----------------------------------- */

#define MAX_BLOCKS 512
typedef struct Block {
  unsigned char *p;
  size_t size;
  unsigned char tag;
} Block;

static Block blocks[MAX_BLOCKS];
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

static void fill(Block *b)
{
  size_t i;
  for (i = 0; i < b->size; i++) b->p[i] = (unsigned char)(b->tag + (i & 0x3f));
}

static int verify(const Block *b)
{
  size_t i;
  for (i = 0; i < b->size; i++)
    if (b->p[i] != (unsigned char)(b->tag + (i & 0x3f))) return 0;
  return 1;
}

static int verify_all(void)
{
  int i, bad = 0;
  for (i = 0; i < MAX_BLOCKS; i++)
    if (blocks[i].p != NULL && !verify(&blocks[i])) {
      printf("  ..   block %d (%zu bytes at %p) was overwritten by another "
             "allocation\n", i, blocks[i].size, (void *)blocks[i].p);
      bad++;
    }
  return bad;
}

int main(void)
{
  void *msp;
  unsigned seed = 987654321u;
  unsigned char tag = 0;
  int i, k, live = 0;
  size_t total = 0, peak = 0;

  xr_host_getprocaddr_result = (void *)host_ntavm;

  printf("== dlmalloc on the XR pool ==\n");
  ntavm = host_ntavm;
  XR_INIT();
  expect(g_heap != NULL, "the pool was reserved");
  if (g_heap == NULL) return 1;
  msp = lj_alloc_create();
  expect(msp != NULL, "lj_alloc_create() gets its 128K initial segment");
  if (msp == NULL) return 1;

  /*
  ** dlmalloc's malloc_state is the second chunk of the pool, i.e. the first
  ** bytes the pool hands out - exactly where the headless crash log put the
  ** stray write (0x013E0000 = pool base). One bad 4-byte store there rewrites
  ** bins/top, and every later allocation can overlap a live object; that is how
  ** a GCstr ends up inside the string table. Nothing below may touch it again.
  */
  printf("== where dlmalloc's state lives ==\n");
  printf("  pool base %p, malloc_state at +%ld bytes\n", g_heap,
         (long)((char *)msp - (char *)g_heap));
  expect((char *)msp > (char *)g_heap && (char *)msp - (char *)g_heap < 64,
         "the allocator state sits at the pool base (one stray write poisons it)");

  printf("== random malloc/realloc/free on the real allocator ==\n");
  memset(blocks, 0, sizeof(blocks));
  for (k = 0; k < 40000; k++) {
    int slot = (int)((seed = seed * 1103515245u + 12345u) >> 16) % MAX_BLOCKS;
    size_t size;
    if (blocks[slot].p == NULL) {
      /* Mostly small objects, sometimes a direct-mapped (>128K) chunk. */
      size = ((seed >> 8) % 5u) == 0
                 ? (size_t)((seed >> 3) % 400u + 64u) * 1024u
                 : (size_t)((seed >> 5) % 900u) + 8u;
      blocks[slot].p = (unsigned char *)lj_alloc_f(msp, NULL, 0, size);
      if (blocks[slot].p == NULL) {
        printf("  FAIL lj_alloc_f(%zu) returned NULL\n", size);
        failures++;
        break;
      }
      blocks[slot].size = size;
      blocks[slot].tag = ++tag;
      fill(&blocks[slot]);
      live++;
      total += size;
      if (total > peak) peak = total;
      if ((k & 255) == 0) {
        int bad = verify_all();
        if (bad != 0) {
          printf("  FAIL %d live block(s) corrupted after %d operations\n", bad, k);
          failures++;
          break;
        }
      }
    } else if ((seed & 3u) == 0) {
      /* realloc: dlmalloc must preserve the prefix */
      size_t nsize = blocks[slot].size + (size_t)((seed >> 7) % 4096u) + 1u;
      size_t keep = blocks[slot].size;
      unsigned char *np = (unsigned char *)lj_alloc_f(
          msp, blocks[slot].p, blocks[slot].size, nsize);
      if (np == NULL) {
        printf("  FAIL realloc to %zu returned NULL\n", nsize);
        failures++;
        break;
      }
      blocks[slot].p = np;
      blocks[slot].size = nsize;
      total += nsize - keep;
      if (total > peak) peak = total;
      fill(&blocks[slot]); /* overwrite, we only need the new pattern */
    } else {
      size_t size = blocks[slot].size;
      if (!verify(&blocks[slot])) {
        printf("  FAIL block at %p was corrupted before free\n",
               (void *)blocks[slot].p);
        failures++;
        break;
      }
      lj_alloc_f(msp, blocks[slot].p, size, 0);
      blocks[slot].p = NULL;
      blocks[slot].size = 0;
      total -= size;
      live--;
    }
  }
  expect(failures == 0, "40000 operations without corrupting a live block");
  printf("  ..   peak live bytes %zu, live blocks %d\n", peak, live);

  printf("== trim: free big blocks, then allocate again ==\n");
  {
    void *big[8];
    size_t bigsize = 2u * 1024u * 1024u;
    int ok = 1;
    for (i = 0; i < 8; i++) {
      big[i] = lj_alloc_f(msp, NULL, 0, bigsize);
      if (big[i] == NULL) ok = 0;
      else memset(big[i], 0xAB, 256);
    }
    expect(ok, "eight 2 MB blocks are served (direct-mapped chunks)");
    for (i = 0; i < 8; i++)
      if (big[i] != NULL) lj_alloc_f(msp, big[i], bigsize, 0);
    /* big frees grow the top chunk, which triggers alloc_trim() ->
    ** CALL_MUNMAP(sp->base + newsize, extra) -> XR_DESTROY */
    {
      void *small[64];
      int smallok = 1;
      for (i = 0; i < 64; i++) {
        small[i] = lj_alloc_f(msp, NULL, 0, 200u + (size_t)i * 7u);
        if (small[i] == NULL) smallok = 0;
        else memset(small[i], (unsigned char)i, 200u + (size_t)i * 7u);
      }
      expect(smallok, "the allocator still works after trimming");
      for (i = 0; i < 64; i++)
        if (small[i] != NULL &&
            ((unsigned char *)small[i])[0] != (unsigned char)i) {
          printf("  FAIL small block %d was overwritten after trim\n", i);
          failures++;
        }
      for (i = 0; i < 64; i++)
        if (small[i] != NULL) lj_alloc_f(msp, small[i], 200u + (size_t)i * 7u, 0);
    }
  }

  printf("== every live block is still intact ==\n");
  expect(verify_all() == 0, "no block was overwritten by an alias");
  for (i = 0; i < MAX_BLOCKS; i++)
    if (blocks[i].p != NULL) lj_alloc_f(msp, blocks[i].p, blocks[i].size, 0);

  /* Second generation: a fresh state over the pool, like a level reload. */
  printf("== a new allocator instance after lj_alloc_destroy() ==\n");
  lj_alloc_destroy(msp);
  msp = lj_alloc_create();
  expect(msp != NULL, "the pool can be reused after destroy");
  if (msp != NULL) {
    void *p = lj_alloc_f(msp, NULL, 0, 1024);
    expect(p != NULL, "allocations work again");
    if (p != NULL) {
      memset(p, 0x5A, 1024);
      expect(((unsigned char *)p)[1023] == 0x5A, "the block is usable");
      lj_alloc_f(msp, p, 1024, 0);
    }
    lj_alloc_destroy(msp);
  }

  if (failures != 0) {
    printf("alloc+pool: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("alloc+pool: ok\n");
  return 0;
}
