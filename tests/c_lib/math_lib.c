#include "math_lib.h"

int c_compute_weighted_sum(int a, int b) {
    return (a + b) * MATH_LIB_SCALE;
}

int c_vec2_dot(Vec2 u, Vec2 v) {
    return (u.x * v.x) + (u.y * v.y);
}
