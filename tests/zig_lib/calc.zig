const c = @cImport({
    @cInclude("math_lib.h");
});

#ifdef CUSTOM_ZIG_OFFSET
const base_offset: c_int = CUSTOM_ZIG_OFFSET;
#else
const base_offset: c_int = 0;
#endif

pub export fn zig_compute_total(a: c_int, b: c_int) c_int {
    return c.c_compute_weighted_sum(a, b) + c.MATH_MAGIC + base_offset;
}
