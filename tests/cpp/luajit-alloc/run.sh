#!/usr/bin/env bash
# Host-side checks for the LuaJIT memory layer (no MSVC needed).
#
# The engine builds LuaJIT only on Windows, but both halves of its memory layer
# are portable C and can be exercised on a Linux dev box:
#
#   test_strtab      - real lj_str_new()/lj_str_resize()/GC from the vendored
#                      tree over dlmalloc: every bucket of the interning table
#                      must hold a GCstr payload base, never object+8 and never
#                      a bare hash (that is the shape of the headless crash).
#   test_pool        - real xr_alloc.c (512 MB NtAllocateVirtualMemory pool,
#                      64 KB chunks) driven like dlmalloc drives it, plus the
#                      hostile inputs it must survive: zero/oversized request,
#                      free of an address it never handed out, free past the
#                      pool end. It must never hand out or release live memory.
#   test_alloc_pool  - real dlmalloc (lj_alloc.c, compiled with
#                      LUAJIT_OS_WINDOWS so CALL_MMAP/CALL_MUNMAP go through
#                      XR_MMAP/XR_DESTROY) on top of that pool, with a unique
#                      byte pattern in every live block so an alias shows up as
#                      a corrupted block instead of a silent heap corruption.
#
# xr_alloc.c includes <windows.h>; ./stub/windows.h supplies the few
# declarations it touches and lets the test provide the pool-backed
# NtAllocateVirtualMemory, so the shipped source is compiled unmodified.
#
# Usage: tests/cpp/luajit-alloc/run.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
luajit="$root/src/3rd party/luajit-2"
ljsrc="$luajit/src"

for f in "$ljsrc/lj_str.c" "$ljsrc/lj_alloc.c" "$ljsrc/xr_alloc.c"; do
    if [[ ! -f "$f" ]]; then
        echo "missing $f" >&2
        exit 2
    fi
done

cc="${CC:-gcc}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# -- 1. host build of the vendored LuaJIT (mirror of msvcbuild.bat) ---------
# The tree ships without the generated VM headers (they are build artifacts),
# so they are produced in a scratch copy, never in the vendored directory.
echo "== building LuaJIT for the host from $ljsrc =="
cp -r "$ljsrc" "$work/lj"
src="$work/lj"
ALL_LIB="lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c"

"$cc" -O2 -I"$src" "$src/host/minilua.c" -o "$work/minilua" -lm
"$work/minilua" "$luajit/dynasm/dynasm.lua" -LN -D POSIX -D JIT -D FFI -D P64 \
    -o "$src/host/buildvm_arch.h" "$src/vm_x86.dasc"
"$cc" -O2 -I"$src" -I"$luajit/dynasm" \
    "$src/host/buildvm.c" "$src/host/buildvm_asm.c" "$src/host/buildvm_lib.c" \
    "$src/host/buildvm_fold.c" "$src/host/buildvm_peobj.c" \
    -o "$work/buildvm" -lm
(cd "$src" && "$work/buildvm" -m elfasm -o lj_vm.S \
    && "$work/buildvm" -m bcdef -o lj_bcdef.h $ALL_LIB \
    && "$work/buildvm" -m ffdef -o lj_ffdef.h $ALL_LIB \
    && "$work/buildvm" -m libdef -o lj_libdef.h $ALL_LIB \
    && "$work/buildvm" -m recdef -o lj_recdef.h $ALL_LIB \
    && "$work/buildvm" -m vmdef -o jit/vmdef.lua $ALL_LIB \
    && "$work/buildvm" -m folddef -o lj_folddef.h lj_opt_fold.c)
"$cc" -O1 -g -w -I"$src" -c "$src/ljamalg.c" -o "$work/ljamalg.o"
"$cc" -O1 -g -w -I"$src" -c "$src/lj_vm.S" -o "$work/lj_vm.o"
ar rcs "$work/libluajit.a" "$work/ljamalg.o" "$work/lj_vm.o"

# -- 2. string interning table ---------------------------------------------
echo "== test_strtab =="
"$cc" -O1 -g -Wall -Wextra -Werror -I"$src" \
    "$here/test_strtab.c" "$work/libluajit.a" -o "$work/test_strtab" -lm -ldl
"$work/test_strtab"

# -- 3. the 512 MB pool on its own -----------------------------------------
echo "== test_pool =="
"$cc" -O1 -g -w -I"$here/stub" -I"$ljsrc" -c "$ljsrc/xr_alloc.c" -o "$work/xr_alloc.o"
"$cc" -O1 -g -Wall -Wextra -Werror -I"$here/stub" -I"$ljsrc" \
    "$here/test_pool.c" "$work/xr_alloc.o" -o "$work/test_pool"
"$work/test_pool"

# -- 4. dlmalloc on the pool (the Windows allocation path) ------------------
echo "== test_alloc_pool =="
"$cc" -O1 -g -w -DLUAJIT_OS=1 -I"$here/stub" -I"$ljsrc" -c "$ljsrc/lj_alloc.c" \
    -o "$work/lj_alloc_win.o"
"$cc" -O1 -g -Wall -Wextra -Werror -I"$here/stub" -I"$ljsrc" \
    "$here/test_alloc_pool.c" "$work/lj_alloc_win.o" "$work/xr_alloc.o" \
    -o "$work/test_alloc_pool"
"$work/test_alloc_pool"

echo "luajit-alloc: all checks passed"
