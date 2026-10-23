#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace zpp {

enum class TokenKind {
    Whitespace,
    Newline,
    Comment,
    Identifier,
    Builtin,                // @ident (e.g. @cImport, @cInclude, @cDefine, @cUndef)
    RawIdentifier,          // @"..."
    StringLiteral,          // "..."
    MultilineStringLiteral, // \\...
    CharLiteral,            // '...'
    NumberLiteral,          // 123, 0x1a, 0b101, 0o77, etc.
    Punctuator,             // ##, #, ..., ==, !=, <=, >=, <<, >>, &&, ||, (, ), {, }, etc.
    Eof
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string text;
    std::string file;
    std::size_t line = 1;
    std::size_t column = 1;

    [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
    [[nodiscard]] bool is_punct(std::string_view p) const noexcept {
        return kind == TokenKind::Punctuator && text == p;
    }
    [[nodiscard]] bool is_ident(std::string_view id) const noexcept {
        return kind == TokenKind::Identifier && text == id;
    }
    [[nodiscard]] bool is_builtin(std::string_view b) const noexcept {
        return kind == TokenKind::Builtin && text == b;
    }
};

struct LogicalLine {
    bool is_directive = false;
    std::string directive_name;
    std::string directive_raw_body;
    std::string full_raw_text;
    std::size_t start_line = 1;
    std::size_t physical_line_count = 1;
};

class Lexer {
public:
    // Splits a source file into logical lines, merging backslash-continued (#...) directive lines
    // while preserving exact physical line counts for 1:1 line numbering.
    static std::vector<LogicalLine> split_logical_lines(std::string_view source);

    // Tokenizes an arbitrary slice of Zig or C-preprocessor text.
    static std::vector<Token> tokenize(std::string_view text,
                                       std::string_view filename = "<input>",
                                       std::size_t start_line = 1,
                                       std::size_t start_col = 1);

    // Unquotes a Zig/C string literal (e.g. "\"hello\\n\"" -> "hello\n").
    static std::string unquote_string(std::string_view literal);

    // Quotes a string into a valid Zig/C string literal.
    static std::string quote_string(std::string_view raw);
};

} // namespace zpp
