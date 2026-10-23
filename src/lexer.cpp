#include "lexer.hpp"

#include <cctype>
#include <stdexcept>

namespace zpp {

namespace {

bool is_ident_start(char c) noexcept {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) || c == '_';
}

bool is_ident_continue(char c) noexcept {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

bool is_hex_digit(char c) noexcept {
    const auto uc = static_cast<unsigned char>(c);
    return std::isxdigit(uc) != 0;
}

// Strip trailing line comment (// ...) from a directive body while respecting string/char literals.
std::string strip_directive_comments(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        char c = input[i];
        if (c == '"' || c == '\'') {
            char quote = c;
            out.push_back(c);
            ++i;
            while (i < input.size()) {
                char sc = input[i];
                out.push_back(sc);
                ++i;
                if (sc == '\\' && i < input.size()) {
                    out.push_back(input[i]);
                    ++i;
                } else if (sc == quote) {
                    break;
                }
            }
        } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            // Line comment to end of physical line
            while (i < input.size() && input[i] != '\n') {
                ++i;
            }
        } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            out.push_back(' ');
            i += 2;
            while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) {
                ++i;
            }
            if (i + 1 < input.size()) {
                i += 2;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

} // namespace

std::vector<LogicalLine> Lexer::split_logical_lines(std::string_view source) {
    std::vector<LogicalLine> result;
    std::size_t pos = 0;
    std::size_t current_line = 1;

    while (pos < source.size()) {
        std::size_t line_start = pos;
        std::size_t start_line_num = current_line;

        // Check if this physical line starts with optional horizontal whitespace followed by '#'
        std::size_t scan = pos;
        while (scan < source.size() && (source[scan] == ' ' || source[scan] == '\t')) {
            ++scan;
        }

        bool is_dir = (scan < source.size() && source[scan] == '#' &&
                       !(scan + 1 < source.size() && source[scan + 1] == '#'));

        if (!is_dir) {
            // Normal Zig source line (do not merge backslash continuations; Zig uses \\ for multiline strings)
            while (pos < source.size() && source[pos] != '\n') {
                ++pos;
            }
            std::size_t line_end = pos;
            if (line_end > line_start && source[line_end - 1] == '\r') {
                --line_end;
            }
            if (pos < source.size() && source[pos] == '\n') {
                ++pos;
            }

            LogicalLine ll;
            ll.is_directive = false;
            ll.full_raw_text = std::string(source.substr(line_start, line_end - line_start));
            ll.start_line = start_line_num;
            ll.physical_line_count = 1;
            result.push_back(std::move(ll));
            ++current_line;
            continue;
        }

        // Preprocessor directive line: support '\' line continuations across physical lines
        std::string combined;
        std::size_t phys_lines = 0;
        while (true) {
            std::size_t p_start = pos;
            while (pos < source.size() && source[pos] != '\n') {
                ++pos;
            }
            std::size_t p_end = pos;
            if (p_end > p_start && source[p_end - 1] == '\r') {
                --p_end;
            }
            bool had_newline = false;
            if (pos < source.size() && source[pos] == '\n') {
                had_newline = true;
                ++pos;
            }
            ++phys_lines;
            ++current_line;

            std::string_view slice = source.substr(p_start, p_end - p_start);
            // Strip any trailing // comment before checking for backslash continuation?
            // Standard C preprocessor processes line splicing (backslash-newline) BEFORE comments.
            if (had_newline && !slice.empty() && slice.back() == '\\') {
                combined.append(slice.substr(0, slice.size() - 1));
                if (pos < source.size()) {
                    continue;
                }
            }
            combined.append(slice);
            break;
        }

        // Parse directive name and body from combined
        std::string cleaned = strip_directive_comments(combined);
        std::size_t d_pos = 0;
        while (d_pos < cleaned.size() && (cleaned[d_pos] == ' ' || cleaned[d_pos] == '\t')) {
            ++d_pos;
        }
        // Skip '#'
        if (d_pos < cleaned.size() && cleaned[d_pos] == '#') {
            ++d_pos;
        }
        while (d_pos < cleaned.size() && (cleaned[d_pos] == ' ' || cleaned[d_pos] == '\t')) {
            ++d_pos;
        }
        std::size_t name_start = d_pos;
        while (d_pos < cleaned.size() && is_ident_continue(cleaned[d_pos])) {
            ++d_pos;
        }
        std::string dir_name = cleaned.substr(name_start, d_pos - name_start);
        std::string dir_body = cleaned.substr(d_pos);

        LogicalLine ll;
        ll.is_directive = true;
        ll.directive_name = std::move(dir_name);
        ll.directive_raw_body = std::move(dir_body);
        ll.full_raw_text = std::move(combined);
        ll.start_line = start_line_num;
        ll.physical_line_count = phys_lines;
        result.push_back(std::move(ll));
    }

    return result;
}

std::vector<Token> Lexer::tokenize(std::string_view text,
                                   std::string_view filename,
                                   std::size_t start_line,
                                   std::size_t start_col) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    std::size_t line = start_line;
    std::size_t col = start_col;
    std::string file_str(filename);

    auto make_tok = [&](TokenKind kind, std::string_view slice, std::size_t l, std::size_t c) {
        tokens.push_back(Token{
            .kind = kind,
            .text = std::string(slice),
            .file = file_str,
            .line = l,
            .column = c,
        });
    };

    while (i < text.size()) {
        std::size_t tok_line = line;
        std::size_t tok_col = col;
        char c = text[i];

        // Newline
        if (c == '\r' || c == '\n') {
            std::size_t start = i;
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                i += 2;
            } else {
                ++i;
            }
            make_tok(TokenKind::Newline, "\n", tok_line, tok_col);
            (void)start;
            ++line;
            col = 1;
            continue;
        }

        // Horizontal whitespace
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
            std::size_t start = i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\v' || text[i] == '\f')) {
                ++i;
                ++col;
            }
            make_tok(TokenKind::Whitespace, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Line comment //...
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            std::size_t start = i;
            while (i < text.size() && text[i] != '\n' && text[i] != '\r') {
                ++i;
                ++col;
            }
            make_tok(TokenKind::Comment, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Block comment /* ... */ (supported for C preprocessor compatibility)
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            std::size_t start = i;
            i += 2;
            col += 2;
            while (i < text.size() && !(text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/')) {
                if (text[i] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
                ++i;
            }
            if (i + 1 < text.size()) {
                i += 2;
                col += 2;
            }
            make_tok(TokenKind::Comment, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Zig multiline string literal \\...
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == '\\') {
            std::size_t start = i;
            while (i < text.size() && text[i] != '\n' && text[i] != '\r') {
                ++i;
                ++col;
            }
            make_tok(TokenKind::MultilineStringLiteral, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Zig raw identifier @"..." or builtin @ident
        if (c == '@') {
            if (i + 1 < text.size() && text[i + 1] == '"') {
                std::size_t start = i;
                i += 2;
                col += 2;
                while (i < text.size()) {
                    char sc = text[i];
                    ++i;
                    ++col;
                    if (sc == '\\' && i < text.size()) {
                        ++i;
                        ++col;
                    } else if (sc == '"') {
                        break;
                    }
                }
                make_tok(TokenKind::RawIdentifier, text.substr(start, i - start), tok_line, tok_col);
                continue;
            }
            if (i + 1 < text.size() && is_ident_start(text[i + 1])) {
                std::size_t start = i;
                ++i;
                ++col;
                while (i < text.size() && is_ident_continue(text[i])) {
                    ++i;
                    ++col;
                }
                make_tok(TokenKind::Builtin, text.substr(start, i - start), tok_line, tok_col);
                continue;
            }
        }

        // String literal "..."
        if (c == '"') {
            std::size_t start = i;
            ++i;
            ++col;
            while (i < text.size()) {
                char sc = text[i];
                ++i;
                ++col;
                if (sc == '\\' && i < text.size()) {
                    ++i;
                    ++col;
                } else if (sc == '"') {
                    break;
                }
            }
            make_tok(TokenKind::StringLiteral, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Character literal '...'
        if (c == '\'') {
            std::size_t start = i;
            ++i;
            ++col;
            while (i < text.size()) {
                char sc = text[i];
                ++i;
                ++col;
                if (sc == '\\' && i < text.size()) {
                    ++i;
                    ++col;
                } else if (sc == '\'') {
                    break;
                }
            }
            make_tok(TokenKind::CharLiteral, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Identifier
        if (is_ident_start(c)) {
            std::size_t start = i;
            while (i < text.size() && is_ident_continue(text[i])) {
                ++i;
                ++col;
            }
            make_tok(TokenKind::Identifier, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Number literal
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t start = i;
            if (c == '0' && i + 1 < text.size() &&
                (text[i + 1] == 'x' || text[i + 1] == 'X' ||
                 text[i + 1] == 'b' || text[i + 1] == 'B' ||
                 text[i + 1] == 'o' || text[i + 1] == 'O')) {
                bool is_hex = (text[i + 1] == 'x' || text[i + 1] == 'X');
                i += 2;
                col += 2;
                while (i < text.size() &&
                       (is_ident_continue(text[i]) || (is_hex && is_hex_digit(text[i])))) {
                    ++i;
                    ++col;
                }
            } else {
                while (i < text.size()) {
                    char nc = text[i];
                    if (std::isalnum(static_cast<unsigned char>(nc)) != 0 || nc == '_') {
                        if ((nc == 'e' || nc == 'E' || nc == 'p' || nc == 'P') &&
                            i + 1 < text.size() && (text[i + 1] == '+' || text[i + 1] == '-')) {
                            i += 2;
                            col += 2;
                            continue;
                        }
                        ++i;
                        ++col;
                    } else if (nc == '.' && i + 1 < text.size() &&
                               std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0) {
                        ++i;
                        ++col;
                    } else {
                        break;
                    }
                }
            }
            make_tok(TokenKind::NumberLiteral, text.substr(start, i - start), tok_line, tok_col);
            continue;
        }

        // Multi-character punctuators
        std::string_view rem = text.substr(i);
        static constexpr std::string_view multi_puncts[] = {
            "...", "<<=", ">>=", "+|=", "-|=", "*|=", "+%=", "-%=", "*%=",
            "##", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>",
            "->", "=>", "++", "**", ".?", ".*", "..",
            "+|", "-|", "*|", "+%", "-%", "*%",
            "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="
        };
        bool matched_multi = false;
        for (std::string_view mp : multi_puncts) {
            if (rem.starts_with(mp)) {
                make_tok(TokenKind::Punctuator, mp, tok_line, tok_col);
                i += mp.size();
                col += mp.size();
                matched_multi = true;
                break;
            }
        }
        if (matched_multi) {
            continue;
        }

        // Single-character punctuator
        make_tok(TokenKind::Punctuator, text.substr(i, 1), tok_line, tok_col);
        ++i;
        ++col;
    }

    return tokens;
}

std::string Lexer::unquote_string(std::string_view literal) {
    if (literal.size() >= 2 &&
        ((literal.front() == '"' && literal.back() == '"') ||
         (literal.front() == '<' && literal.back() == '>'))) {
        bool is_angle = (literal.front() == '<');
        std::string_view inner = literal.substr(1, literal.size() - 2);
        if (is_angle) {
            return std::string(inner);
        }
        std::string out;
        out.reserve(inner.size());
        for (std::size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] == '\\' && i + 1 < inner.size()) {
                ++i;
                switch (inner[i]) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    case '0': out.push_back('\0'); break;
                    default: out.push_back(inner[i]); break;
                }
            } else {
                out.push_back(inner[i]);
            }
        }
        return out;
    }
    return std::string(literal);
}

std::string Lexer::quote_string(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    for (char c : raw) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '"': out.append("\\\""); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

} // namespace zpp
