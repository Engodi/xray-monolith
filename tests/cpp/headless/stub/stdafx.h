#ifndef HEADLESS_TEST_STDAFX_STUB
#define HEADLESS_TEST_STDAFX_STUB
// Minimal host stub for xrCore/headless.cpp (F0-04).
//
// The engine sources cannot be compiled on Linux (MSVC/VS2022 only, see
// F0-01), but the headless launch-flag parser is plain C++ with a tiny
// dependency surface: this stub provides exactly that surface, so the parser
// logic is verified locally before the Windows build ever runs.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

typedef char* LPSTR;
typedef const char* LPCSTR;
typedef unsigned int u32;

inline char* xr_strcpy(char* destination, size_t size, const char* source) {
    if (!size) {
        return destination;
    }
    std::strncpy(destination, source, size - 1);
    destination[size - 1] = 0;
    return destination;
}

inline int xr_sprintf(char* destination, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(destination, size, format, args);
    va_end(args);
    return written;
}

inline void Msg(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

inline void Sleep(unsigned milliseconds) { (void)milliseconds; }

inline int _stricmp(const char* left, const char* right) { return ::strcasecmp(left, right); }

#endif  // HEADLESS_TEST_STDAFX_STUB
