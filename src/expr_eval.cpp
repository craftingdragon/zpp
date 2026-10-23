#include "expr_eval.hpp"

#include <charconv>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace zpp {

namespace {

struct EvalValue {
    bool is_string = false;
    std::int64_t int_val = 0;
    std::string str_val{};

    static EvalValue from_int(std::int64_t v) {
        EvalValue ev;
        ev.is_string = false;
        ev.int_val = v;
        return ev;
    }

    static EvalValue from_str(std::string s) {
        EvalValue ev;
        ev.is_string = true;
        ev.int_val = 1;
        ev.str_val = std::move(s);
        return ev;
    }

    [[nodiscard]] bool truthy() const noexcept {
        return is_string ? !str_val.empty() : (int_val != 0);
    }
};

class Parser {
public:
    Parser(std::vector<Token> toks,
           const ExprEvaluator::IsDefinedFn& is_defined,
           std::string_view file,
           std::size_t line)
        : tokens_(std::move(toks)), is_defined_(is_defined), file_(file), line_(line) {}

    std::int64_t parse() {
        if (tokens_.empty()) {
            throw error("empty expression in #if/#elif directive");
        }
        EvalValue val = parse_ternary();
        if (pos_ < tokens_.size()) {
            throw error("unexpected token '" + tokens_[pos_].text + "' in #if/#elif expression");
        }
        return val.int_val;
    }

private:
    std::vector<Token> tokens_;
    const ExprEvaluator::IsDefinedFn& is_defined_;
    std::string file_;
    std::size_t line_;
    std::size_t pos_ = 0;

    [[nodiscard]] std::runtime_error error(const std::string& msg) const {
        std::ostringstream oss;
        std::size_t ln = (pos_ < tokens_.size()) ? tokens_[pos_].line : line_;
        oss << file_ << ":" << ln << ": error: " << msg;
        return std::runtime_error(oss.str());
    }

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= tokens_.size(); }

    [[nodiscard]] const Token& peek() const noexcept { return tokens_[pos_]; }

    bool match_punct(std::string_view p) noexcept {
        if (!at_end() && tokens_[pos_].is_punct(p)) {
            ++pos_;
            return true;
        }
        return false;
    }

    EvalValue parse_ternary() {
        EvalValue cond = parse_logical_or();
        if (match_punct("?")) {
            EvalValue then_val = parse_ternary();
            if (!match_punct(":")) {
                throw error("expected ':' in ternary expression");
            }
            EvalValue else_val = parse_ternary();
            return cond.truthy() ? then_val : else_val;
        }
        return cond;
    }

    EvalValue parse_logical_or() {
        EvalValue left = parse_logical_and();
        while (match_punct("||")) {
            EvalValue right = parse_logical_and();
            left = EvalValue::from_int((left.truthy() || right.truthy()) ? 1 : 0);
        }
        return left;
    }

    EvalValue parse_logical_and() {
        EvalValue left = parse_bitwise_or();
        while (match_punct("&&")) {
            EvalValue right = parse_bitwise_or();
            left = EvalValue::from_int((left.truthy() && right.truthy()) ? 1 : 0);
        }
        return left;
    }

    EvalValue parse_bitwise_or() {
        EvalValue left = parse_bitwise_xor();
        while (match_punct("|")) {
            EvalValue right = parse_bitwise_xor();
            left = EvalValue::from_int(left.int_val | right.int_val);
        }
        return left;
    }

    EvalValue parse_bitwise_xor() {
        EvalValue left = parse_bitwise_and();
        while (match_punct("^")) {
            EvalValue right = parse_bitwise_and();
            left = EvalValue::from_int(left.int_val ^ right.int_val);
        }
        return left;
    }

    EvalValue parse_bitwise_and() {
        EvalValue left = parse_equality();
        while (match_punct("&")) {
            EvalValue right = parse_equality();
            left = EvalValue::from_int(left.int_val & right.int_val);
        }
        return left;
    }

    EvalValue parse_equality() {
        EvalValue left = parse_relational();
        while (!at_end()) {
            if (match_punct("==")) {
                EvalValue right = parse_relational();
                bool eq = (left.is_string && right.is_string)
                              ? (left.str_val == right.str_val)
                              : (left.int_val == right.int_val);
                left = EvalValue::from_int(eq ? 1 : 0);
            } else if (match_punct("!=")) {
                EvalValue right = parse_relational();
                bool neq = (left.is_string && right.is_string)
                               ? (left.str_val != right.str_val)
                               : (left.int_val != right.int_val);
                left = EvalValue::from_int(neq ? 1 : 0);
            } else {
                break;
            }
        }
        return left;
    }

    EvalValue parse_relational() {
        EvalValue left = parse_shift();
        while (!at_end()) {
            if (match_punct("<=")) {
                EvalValue right = parse_shift();
                left = EvalValue::from_int((left.int_val <= right.int_val) ? 1 : 0);
            } else if (match_punct(">=")) {
                EvalValue right = parse_shift();
                left = EvalValue::from_int((left.int_val >= right.int_val) ? 1 : 0);
            } else if (match_punct("<")) {
                EvalValue right = parse_shift();
                left = EvalValue::from_int((left.int_val < right.int_val) ? 1 : 0);
            } else if (match_punct(">")) {
                EvalValue right = parse_shift();
                left = EvalValue::from_int((left.int_val > right.int_val) ? 1 : 0);
            } else {
                break;
            }
        }
        return left;
    }

    EvalValue parse_shift() {
        EvalValue left = parse_additive();
        while (!at_end()) {
            if (match_punct("<<")) {
                EvalValue right = parse_additive();
                left = EvalValue::from_int(left.int_val << right.int_val);
            } else if (match_punct(">>")) {
                EvalValue right = parse_additive();
                left = EvalValue::from_int(left.int_val >> right.int_val);
            } else {
                break;
            }
        }
        return left;
    }

    EvalValue parse_additive() {
        EvalValue left = parse_multiplicative();
        while (!at_end()) {
            if (match_punct("+")) {
                EvalValue right = parse_multiplicative();
                left = EvalValue::from_int(left.int_val + right.int_val);
            } else if (match_punct("-")) {
                EvalValue right = parse_multiplicative();
                left = EvalValue::from_int(left.int_val - right.int_val);
            } else {
                break;
            }
        }
        return left;
    }

    EvalValue parse_multiplicative() {
        EvalValue left = parse_unary();
        while (!at_end()) {
            if (match_punct("*")) {
                EvalValue right = parse_unary();
                left = EvalValue::from_int(left.int_val * right.int_val);
            } else if (match_punct("/")) {
                EvalValue right = parse_unary();
                if (right.int_val == 0) {
                    throw error("division by zero in #if/#elif expression");
                }
                left = EvalValue::from_int(left.int_val / right.int_val);
            } else if (match_punct("%")) {
                EvalValue right = parse_unary();
                if (right.int_val == 0) {
                    throw error("modulo by zero in #if/#elif expression");
                }
                left = EvalValue::from_int(left.int_val % right.int_val);
            } else {
                break;
            }
        }
        return left;
    }

    EvalValue parse_unary() {
        if (match_punct("+")) {
            return parse_unary();
        }
        if (match_punct("-")) {
            EvalValue v = parse_unary();
            v.int_val = -v.int_val;
            return v;
        }
        if (match_punct("!")) {
            EvalValue v = parse_unary();
            return EvalValue::from_int(v.truthy() ? 0 : 1);
        }
        if (match_punct("~")) {
            EvalValue v = parse_unary();
            v.int_val = ~v.int_val;
            return v;
        }
        if (!at_end() && peek().is_ident("defined")) {
            ++pos_;
            bool has_paren = match_punct("(");
            if (at_end() || !peek().is(TokenKind::Identifier)) {
                throw error("expected macro identifier after 'defined'");
            }
            std::string macro_name = peek().text;
            ++pos_;
            if (has_paren && !match_punct(")")) {
                throw error("expected ')' after 'defined(" + macro_name + "'");
            }
            return EvalValue::from_int(is_defined_(macro_name) ? 1 : 0);
        }
        return parse_primary();
    }

    static std::int64_t parse_number_literal(std::string_view raw) {
        std::string cleaned;
        cleaned.reserve(raw.size());
        for (char c : raw) {
            if (c != '_') {
                cleaned.push_back(c);
            }
        }
        // Strip trailing integer suffixes (u, U, l, L, z, Z)
        while (!cleaned.empty()) {
            char b = cleaned.back();
            if (b == 'u' || b == 'U' || b == 'l' || b == 'L') {
                cleaned.pop_back();
            } else {
                break;
            }
        }
        int base = 10;
        std::string_view num_view = cleaned;
        if (num_view.size() >= 2 && num_view[0] == '0') {
            if (num_view[1] == 'x' || num_view[1] == 'X') {
                base = 16;
                num_view.remove_prefix(2);
            } else if (num_view[1] == 'b' || num_view[1] == 'B') {
                base = 2;
                num_view.remove_prefix(2);
            } else if (num_view[1] == 'o' || num_view[1] == 'O') {
                base = 8;
                num_view.remove_prefix(2);
            } else {
                base = 8;
            }
        }
        std::uint64_t uval = 0;
        auto [ptr, ec] = std::from_chars(num_view.data(), num_view.data() + num_view.size(), uval, base);
        if (ec != std::errc{}) {
            return 0;
        }
        (void)ptr;
        return static_cast<std::int64_t>(uval);
    }

    EvalValue parse_primary() {
        if (at_end()) {
            throw error("unexpected end of #if/#elif expression");
        }
        if (match_punct("(")) {
            EvalValue val = parse_ternary();
            if (!match_punct(")")) {
                throw error("expected ')' in #if/#elif expression");
            }
            return val;
        }
        const Token& tok = peek();
        if (tok.is(TokenKind::NumberLiteral)) {
            ++pos_;
            return EvalValue::from_int(parse_number_literal(tok.text));
        }
        if (tok.is(TokenKind::CharLiteral)) {
            ++pos_;
            std::string unq = Lexer::unquote_string(
                "\"" + tok.text.substr(1, tok.text.size() >= 2 ? tok.text.size() - 2 : 0) + "\"");
            std::int64_t cval = unq.empty() ? 0 : static_cast<unsigned char>(unq[0]);
            return EvalValue::from_int(cval);
        }
        if (tok.is(TokenKind::StringLiteral)) {
            ++pos_;
            return EvalValue::from_str(Lexer::unquote_string(tok.text));
        }
        if (tok.is(TokenKind::Identifier)) {
            ++pos_;
            if (tok.text == "true") {
                return EvalValue::from_int(1);
            }
            if (tok.text == "false") {
                return EvalValue::from_int(0);
            }
            // Standard C preprocessor rule: any remaining unexpanded identifier evaluates to 0
            return EvalValue::from_int(0);
        }
        throw error("unexpected token '" + tok.text + "' in #if/#elif expression");
    }
};

} // namespace

std::int64_t ExprEvaluator::evaluate(const std::vector<Token>& tokens,
                                     const IsDefinedFn& is_defined,
                                     std::string_view filename,
                                     std::size_t line) {
    std::vector<Token> filtered;
    filtered.reserve(tokens.size());
    for (const auto& t : tokens) {
        if (!t.is(TokenKind::Whitespace) && !t.is(TokenKind::Comment) && !t.is(TokenKind::Newline)) {
            filtered.push_back(t);
        }
    }
    Parser parser(std::move(filtered), is_defined, filename, line);
    return parser.parse();
}

} // namespace zpp
