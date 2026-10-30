/*
 * Minimal stand-in for <windows.h>, used only by the host-side test of
 * src/3rd party/luajit-2/src/xr_alloc.c (tests/cpp/luajit-alloc/run.sh).
 *
 * The engine ships LuaJIT on Windows only, so xr_alloc.c cannot be compiled on
 * a Linux dev box as-is. This header supplies just the declarations that file
 * touches; the test provides `ntavm` itself (a pool-backed NtAllocateVirtualMemory
 * look-alike), so the real pool code runs unmodified.
 */
#ifndef XR_HOST_FAKE_WINDOWS_H
#define XR_HOST_FAKE_WINDOWS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned long ULONG;
typedef unsigned long DWORD;
typedef int BOOL;

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_RELEASE 0x00008000
#define PAGE_READWRITE 0x04

#define GetLastError() 0
#define SetLastError(e) ((void)(e))

static inline int VirtualFree(void *addr, size_t size, DWORD type)
{
  (void)addr;
  (void)size;
  (void)type;
  return 1;
}

static inline void OutputDebugStringA(const char *s) { (void)s; }
#define OutputDebugString(s) OutputDebugStringA(s)

static inline HMODULE GetModuleHandleA(const char *name)
{
  (void)name;
  return NULL;
}

/*
 * The engine resolves NtAllocateVirtualMemory at runtime (lj_alloc.c / xr_alloc.c
 * call GetProcAddress). The test installs its pool-backed implementation here
 * before the first lj_alloc_create()/XR_INIT().
 */
extern void *xr_host_getprocaddr_result;

static inline void *GetProcAddress(HMODULE mod, const char *name)
{
  (void)mod;
  (void)name;
  return xr_host_getprocaddr_result;
}

#endif
