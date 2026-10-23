#pragma once

#include "lexer.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zpp {

class ExprEvaluator {
public:
    using IsDefinedFn = std::function<bool(std::string_view)>;

    // Evaluates a C-preprocessor constant expression from a token stream.
    // Assumes macros (other than `defined(...)` targets if raw tokens are passed) have been expanded,
    // or evaluates `defined(X)` / `defined X` directly via `is_defined`.
    static std::int64_t evaluate(const std::vector<Token>& tokens,
                                 const IsDefinedFn& is_defined,
                                 std::string_view filename = "<expr>",
                                 std::size_t line = 1);
};

} // namespace zpp
