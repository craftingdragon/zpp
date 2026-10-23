#include <assert.h>
#include <stdio.h>

extern int zig_compute_total(int a, int b);

int main(void) {
    /* c_compute_weighted_sum(3, 7) = (3 + 7) * 3 = 30
     * + MATH_MAGIC (42)
     * + CUSTOM_ZIG_OFFSET (1000)
     * = 1072
     */
    int res = zig_compute_total(3, 7);
    assert(res == 1072);
    printf("c_calls_zig_test passed: %d\n", res);
    return 0;
}
