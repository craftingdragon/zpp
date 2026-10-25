# zpp — Standalone Zig Preprocessor & Qt `moc`-Style CMake Shadow Build

`zpp` is a zero-dependency C++20 preprocessor for Zig that runs before compilation and integrates with CMake using a Qt `moc`-style out-of-source shadow build (`<target>_zpp_autogen`).

It is completely independent of the Zig compiler codebase and restores removed/legacy workflows such as inline `@cImport({ ... })` blocks while adding full C-style preprocessor directives and macros (`#define`, `#ifdef`, `#if`, `#include`, stringification `#`, token pasting `##`, variadic `__VA_ARGS__` / `__VA_OPT__`) to Zig source files.

---

## Features

- **`@cImport` Lowering for Modern Zig (0.14+ / 0.17+)**:
  - Parses `@cImport({ @cInclude("..."); @cDefine("...", "..."); @cUndef("..."); })` blocks (including conditional `#ifdef` / `#if` directives nested inside `@cImport`).
  - Emits deterministic synthetic headers in `<target>_zpp_autogen/shadow/__zpp_cimports/cimport_<hash>.h`.
  - Invokes `zig translate-c` to generate `cimport_<hash>.zig` and rewrites `@cImport({ ... })` into a relative `@import("...")` inside the shadow source tree.
  - Extracts full transitive C header dependencies via `zig cc -M` into Ninja/Make `.d` depfiles so modifying any C header automatically triggers re-preprocessing.
- **Full C-Style Macro & Directive Engine**:
  - `#define` / `#undef` — object-like macros, function-like macros (including multi-line invocations), variadic macros (`...`, `__VA_ARGS__`, `__VA_OPT__(...)`, and GNU `, ## __VA_ARGS__`), stringification (`#param`), and token pasting (`a ## b`).
  - `#if`, `#elif`, `#else`, `#endif`, `#ifdef`, `#ifndef` — full constant expression evaluator supporting `defined(X)`, `defined X`, integer literals (`0x`, `0b`, `0o`, decimal), char literals, string equality (`"linux" == OS`), bitwise, logical, arithmetic, and ternary `? :` operators.
  - `#include "..."` / `#include <...>` and `#pragma once` for sharing macro definitions across `.zig` files.
  - `#error` and `#warning` diagnostics, plus built-in dynamic macros (`__FILE__`, `__LINE__`, `__COUNTER__`, `__ZPP__`).
- **1:1 Source Line Preservation**:
  - Preserves exact physical line numbers between original `.zig` files and generated shadow `.zig` files so Zig compiler diagnostics point to the exact original line numbers.
- **Qt `moc`-Style CMake Integration (`cmake/ZigPreprocessor.cmake`)**:
  - Automatically creates `<target>_zpp_autogen/shadow/` per target so relative `@import("sub/module.zig")` works naturally across multi-file Zig projects.
  - Automatically forwards CMake `target_include_directories()`, `target_compile_definitions()`, and linked C target properties (`target_link_libraries()`) to both `zpp` and `zig translate-c` via CMake generator expressions.

---

## Quick Start

### Build & Run Tests

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### System-Wide Installation

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

### Arch Linux Installation (`PKGBUILD`)

```bash
makepkg -si
```

Once installed system-wide, any CMake project can consume `zpp` directly via `find_package`:

```cmake
find_package(zpp REQUIRED)

zpp_add_executable(my_zig_app
    ROOT src/main.zig
)
```

---

## CMake Usage (`cmake/ZigPreprocessor.cmake`)

### 1. Executable Target (`zpp_add_executable`)

```cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ZigPreprocessor)

add_library(math_lib STATIC src/math_lib.c)
target_include_directories(math_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(math_lib PUBLIC MATH_LIB_SCALE=3)

zpp_add_executable(my_zig_app
    ROOT src/main.zig
    SOURCES
        src/main.zig
        src/sub/helper.zig
)

# CMake include directories and compile definitions from math_lib and my_zig_app
# automatically propagate to zpp (#if / #ifdef) and @cImport (zig translate-c):
target_link_libraries(my_zig_app PRIVATE math_lib)
target_compile_definitions(my_zig_app PRIVATE APP_FEATURE_LEVEL=2)
```

### 2. Static / Shared Library Target (`zpp_add_library`)

```cmake
zpp_add_library(zig_calc_lib STATIC
    ROOT src/calc.zig
    LINK_LIBRARIES math_lib
    COMPILE_DEFINITIONS CUSTOM_ZIG_OFFSET=1000
)

add_executable(c_host_app src/caller.c)
target_link_libraries(c_host_app PRIVATE zig_calc_lib)
```

### 3. Attach to an Existing CMake Target (`zpp_target_sources`)

```cmake
add_executable(mixed_app src/legacy.c)
zpp_target_sources(mixed_app
    ROOT src/main.zig
    SOURCES src/main.zig src/utils.zig
)
```

### 4. Shadow `zig build` Project (`zpp_add_zig_build`)

```cmake
zpp_add_zig_build(my_zig_project
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    BUILD_ZIG "${CMAKE_CURRENT_SOURCE_DIR}/build.zig"
    SOURCES
        src/main.zig
        src/lib.zig
    RESOURCES
        src/assets/message.txt
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
    COMPILE_DEFINITIONS "ENABLE_FAST_PATH=1"
)
```

`build.zig.zon` is copied into the shadow project automatically when present.
List other non-Zig files needed by `build.zig` or `@embedFile` in `RESOURCES`.
Resource paths are relative to `SOURCE_DIR` (or absolute paths inside it), and
their directory layout is preserved. Changes to these files rerun `zig build`.
For example, `src/main.zig` can use `@embedFile("assets/message.txt")` with the
resource path shown above.

---

## Example Zig Source with Macros & `@cImport`

```zig
const std = @import("std");
const helper = @import("sub/helper.zig");

#include "macros.zpp.h"

const c = @cImport({
    @cDefine("EXTRA_BONUS", "100");
#ifdef USE_STDIO_IN_CIMPORT
    @cInclude("stdio.h");
#endif
    @cInclude("math_lib.h");
});

#if defined(APP_FEATURE_LEVEL) && APP_FEATURE_LEVEL >= 2
#define FEATURE_MULTIPLIER 10
#else
#define FEATURE_MULTIPLIER 1
#endif

pub fn main() !void {
    const val = c.c_compute_weighted_sum(4, 6) * FEATURE_MULTIPLIER;
    std.debug.print("Result = {d}\n", .{val});
}
```

---

## Direct CLI Usage

```text
Usage: zpp [options] <input.zig>

Options:
  -o, --output <file>           Write preprocessed Zig output to <file> (default: stdout)
  -I, --include-dir <dir>       Add directory to #include and @cInclude search path
  -D<macro>[=<val>]             Define macro (passed to both preprocessor and @cImport)
  -U<macro>                     Undefine macro
  --cimport-dir <dir>           Directory for generated @cImport files
  --depfile <file.d>            Write Ninja/Make dependency file
  --output-dep-file <file.d>    Alias for --depfile (Qt moc compatible)
  --zig, --zig-executable <exe> Path to zig executable (default: zig)
  --target <triple>             Target triple for zig translate-c
  --no-libc                     Do not pass -lc to zig translate-c
  --no-cimport                  Disable @cImport lowering (macros/directives only)
  --cflag <flag>                Extra C compiler flag for zig translate-c
  @<file>                       Read command-line arguments from response file
  -v, --version                 Print version
  -h, --help                    Print this help message
```
