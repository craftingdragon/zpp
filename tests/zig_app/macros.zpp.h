#pragma once

#define ZIG_CONST_INT(name, val) pub const name: i32 = (val);
#define MAKE_GETTER(suffix, expr) pub fn get_##suffix() i32 { return (expr); }
#define STRINGIFY_EXPR(x) #x
#define ADD_ALL(first, ...) ((first) __VA_OPT__(+ (__VA_ARGS__)))
