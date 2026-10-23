#!/usr/bin/env python3
"""Статический гейт MSVC-совместимости Lua-экспорта coop.

Причина гейта: `src/xrGame/coop/coop_*_script.cpp` и `coop_core_runtime.cpp`
собираются на хосте только вместе с настоящим движком (MSVC/VS2022, F0-01),
поэтому две поломки, которые g++/clang++ не видят, доехали до CI:

1. `catch (luabind::error&)` — в release-сборке движка `xrCore.h` определяет
   `LUABIND_NO_EXCEPTIONS`, и тогда `luabind::error` в `error.hpp` вообще не
   объявляется (MSVC: C2039/C2061/C2310/C2065). Ловить `luabind::error` можно
   только под охраной `#ifndef LUABIND_NO_EXCEPTIONS`.
2. Обёртка с именем из Lua C API (`lua_status`) — `&lua_status` превращается в
   неразрешённый набор перегрузок (наша функция + `int lua_status(lua_State*)`
   из `lua.h`), `luabind::def` не выводит `F` (MSVC: C2672/C7732/C2679,
   каскад на `module(...)[...]`).

Обе проверки — по настоящим заголовкам движка (`3rd party/luabind/error.hpp`,
`3rd party/luajit-2/src/*.h`, `3rd party/lua-extensions`), поэтому гейт
пересчитывается при обновлении luabind/LuaJIT, а не кодирует список имён.

Usage: tests/cpp/coop/check_msvc_compat.py [--root <engine-root>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

COOP_DIR = Path("src/xrGame/coop")
LUABIND_DIR = Path("src/3rd party/luabind")
LUA_HEADER_DIRS = (Path("src/3rd party/luajit-2/src"), Path("src/3rd party/lua-extensions"))
EXCEPTIONS_MACROS = ("LUABIND_NO_EXCEPTIONS", "XRAY_EXCEPTIONS")

# `luabind::def("name", &wrapper, ...)` — the wrapper is the 2nd argument.
DEF_CALL = re.compile(r"\bdef\s*\(\s*\"[^\"]*\"\s*,\s*&?\s*(lua_[A-Za-z0-9_]+)")
# Free function definition at namespace scope: `type name(` starting at column 0.
# Member calls start with `object.` and never match: the type part has no `.`.
FREE_FUNCTION = re.compile(r"^(?:::)?[A-Za-z_][\w:<>*&\s]*?\b([A-Za-z_]\w*)\s*\(", re.MULTILINE)
COMMENT_LINE = re.compile(r"^\s*(?://|/\*|\*)")
LUABIND_ERROR = re.compile(r"\bluabind::error\b")
GUARD = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def fail(errors: list[str], path: Path, line: int, message: str) -> None:
    errors.append(f"{path}:{line}: {message}")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def lua_c_api_names(root: Path) -> set[str]:
    """Имена функций/макросов, которые объявляет Lua C API движка."""
    names: set[str] = set()
    for directory in LUA_HEADER_DIRS:
        for header in sorted((root / directory).glob("*.h")):
            text = read(header)
            # `int lua_status(lua_State*)` and LuaJIT's parenthesised
            # `int (lua_status) (lua_State*)` declaration style both count.
            names.update(re.findall(r"\b(lua[A-Za-z0-9_]*)\s*\(", text))
            names.update(re.findall(r"\(\s*(lua[A-Za-z0-9_]*)\s*\)", text))
            names.update(re.findall(r"^\s*#\s*define\s+(lua[A-Za-z0-9_]*)", text, re.MULTILINE))
    return names


def luabind_error_needs_guard(root: Path, errors: list[str]) -> None:
    """`luabind::error` в этой версии существует только без LUABIND_NO_EXCEPTIONS."""
    header = root / LUABIND_DIR / "error.hpp"
    if not header.is_file():
        errors.append(f"{header}: not found (luabind layout changed)")
        return

    guarded = False
    stack: list[str] = []
    for line in read(header).splitlines():
        match = GUARD.match(line)
        if match:
            directive, rest = match.group(1), match.group(2)
            if directive in ("if", "ifdef", "ifndef"):
                stack.append(rest.strip())
            elif directive in ("else", "elif") and stack:
                stack[-1] = rest.strip()
            elif directive == "endif" and stack:
                stack.pop()
            continue
        if re.search(r"\bclass\s+error\b", line):
            guarded = any("LUABIND_NO_EXCEPTIONS" in condition for condition in stack)
            break

    if not guarded:
        fail(
            errors,
            header,
            1,
            "class error is not declared under `#ifndef LUABIND_NO_EXCEPTIONS`: "
            "review the luabind::error guard rule in check_msvc_compat.py",
        )


def functions_under_no_exceptions(root: Path, path: Path, text: str) -> list[tuple[int, str]]:
    """`luabind::error` вне охраны `LUABIND_NO_EXCEPTIONS`/`XRAY_EXCEPTIONS`."""
    found: list[tuple[int, str]] = []
    stack: list[str | None] = []
    for number, line in enumerate(text.splitlines(), start=1):
        match = GUARD.match(line)
        if match:
            directive, rest = match.group(1), match.group(2)
            if directive in ("if", "ifdef", "ifndef"):
                stack.append(rest.strip())
            elif directive in ("else", "elif"):
                if stack:
                    stack[-1] = rest.strip()
            elif directive == "endif":
                if stack:
                    stack.pop()
            continue
        if COMMENT_LINE.match(line):
            continue
        if LUABIND_ERROR.search(line) and not any(
            macro in (condition or "") for condition in stack for macro in EXCEPTIONS_MACROS
        ):
            found.append((number, line.strip()))
    return found


def check_cpp_files(root: Path, errors: list[str]) -> None:
    api = lua_c_api_names(root)
    coop = root / COOP_DIR
    if not coop.is_dir():
        errors.append(f"{coop}: not found")
        return

    for path in sorted(coop.glob("*.cpp")):
        text = read(path)
        rel = path.relative_to(root)

        # 1) luabind::error only under an exceptions guard.
        for number, line in functions_under_no_exceptions(root, path, text):
            fail(
                errors,
                rel,
                number,
                f"`{line}` compiles `luabind::error`, which does not exist when "
                "`LUABIND_NO_EXCEPTIONS` is defined (release build): guard it with "
                "`#ifndef LUABIND_NO_EXCEPTIONS`",
            )

        # 2) no wrapper shadowed by the Lua C API.
        defined = {match.group(1) for match in FREE_FUNCTION.finditer(text)}
        for match in DEF_CALL.finditer(text):
            name = match.group(1)
            line = text.count("\n", 0, match.start()) + 1
            if name not in defined:
                fail(errors, rel, line, f"`def(..., &{name})` binds an undeclared wrapper")
            elif name in api:
                fail(
                    errors,
                    rel,
                    line,
                    f"wrapper `{name}` collides with the Lua C API (`lua.h`), so "
                    f"`&{name}` is an unresolved overload set for `luabind::def` "
                    "(C2672 on MSVC); rename the wrapper",
                )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[3],
        help="engine checkout root (default: this script's repository)",
    )
    args = parser.parse_args()
    root: Path = args.root.resolve()

    errors: list[str] = []
    luabind_error_needs_guard(root, errors)
    check_cpp_files(root, errors)

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"{len(errors)} MSVC compatibility problem(s)", file=sys.stderr)
        return 1

    print("msvc-compat: ok (luabind::error guarded, no Lua C API shadowing)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
