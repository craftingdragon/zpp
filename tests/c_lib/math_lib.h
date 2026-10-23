#ifndef ZPP_TEST_MATH_LIB_H
#define ZPP_TEST_MATH_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MATH_LIB_SCALE
#define MATH_LIB_SCALE 1
#endif

#ifdef EXTRA_BONUS
#define BONUS_AMOUNT EXTRA_BONUS
#else
#define BONUS_AMOUNT 0
#endif

#define MATH_MAGIC (40 + 2)

typedef struct Vec2 {
    int x;
    int y;
} Vec2;

int c_compute_weighted_sum(int a, int b);
int c_vec2_dot(Vec2 u, Vec2 v);

#ifdef __cplusplus
}
#endif

#endif /* ZPP_TEST_MATH_LIB_H */
