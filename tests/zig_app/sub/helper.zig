#include "../macros.zpp.h"

const c = @cImport({
    @cDefine("EXTRA_BONUS", "100");
    @cInclude("math_lib.h");
});

ZIG_CONST_INT(helper_magic, c.MATH_MAGIC + c.BONUS_AMOUNT)
MAKE_GETTER(scaled_value, c.c_compute_weighted_sum(4, 6))

pub fn computeDot(x1: c_int, y1: c_int, x2: c_int, y2: c_int) c_int {
    const u = c.Vec2{ .x = x1, .y = y1 };
    const v = c.Vec2{ .x = x2, .y = y2 };
    return c.c_vec2_dot(u, v);
}
