const std = @import("std");
const helper = @import("sub/helper.zig");

#include "macros.zpp.h"
#include "macros.zpp.h" // Second include is skipped via #pragma once

const c = @cImport({
    @cDefine("EXTRA_BONUS", "100");
    @cDefine("TEMP_UNDEF_ME", "999");
    @cUndef("TEMP_UNDEF_ME");
#ifdef USE_STDIO_IN_CIMPORT
    @cInclude("stdio.h");
#endif
    @cInclude("math_lib.h");
});

#if defined(APP_FEATURE_LEVEL) && APP_FEATURE_LEVEL == 2
#define FEATURE_MULTIPLIER 10
#elif defined(APP_FEATURE_LEVEL) && APP_FEATURE_LEVEL == 1
#define FEATURE_MULTIPLIER 5
#else
#define FEATURE_MULTIPLIER 1
#endif

pub fn main() !void {
    // 1. Verify @cImport constants & C function calls linked via CMake target_link_libraries
    try std.testing.expectEqual(@as(i32, 142), helper.helper_magic);
    // (4 + 6) * MATH_LIB_SCALE(3) = 30
    try std.testing.expectEqual(@as(i32, 30), helper.get_scaled_value());
    // Dot product of (2, 3) and (4, 5) = 8 + 15 = 23
    try std.testing.expectEqual(@as(c_int, 23), helper.computeDot(2, 3, 4, 5));

    // 2. Verify CMake target_compile_definitions propagation into #if / #elif
    const mult: i32 = FEATURE_MULTIPLIER;
    try std.testing.expectEqual(@as(i32, 10), mult);

    // 3. Verify # stringification and __VA_OPT__ variadic macros
    const expr_str = STRINGIFY_EXPR(a + b * c);
    try std.testing.expectEqualStrings("a + b * c", expr_str);
    try std.testing.expectEqual(@as(i32, 7), ADD_ALL(7));
    try std.testing.expectEqual(@as(i32, 19), ADD_ALL(7, 12));

    // 4. Verify @cUndef removed TEMP_UNDEF_ME from translated C module
    try std.testing.expect(!@hasDecl(c, "TEMP_UNDEF_ME"));
    try std.testing.expectEqual(@as(c_int, 3), c.MATH_LIB_SCALE);
}
