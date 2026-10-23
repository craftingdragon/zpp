#include "preprocessor.hpp"
#include "expr_eval.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace zpp {

namespace {

std::string read_file_to_string(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::in | std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("failed to open file: " + p.string());
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::string normalize_whitespace_for_stringify(const std::vector<Token>& tokens) {
    std::vector<Token> trimmed = tokens;
    while (!trimmed.empty() &&
           (trimmed.front().is(TokenKind::Whitespace) || trimmed.front().is(TokenKind::Newline))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() &&
           (trimmed.back().is(TokenKind::Whitespace) || trimmed.back().is(TokenKind::Newline))) {
        trimmed.pop_back();
    }
    std::string out;
    bool last_was_space = false;
    for (const auto& t : trimmed) {
        if (t.is(TokenKind::Whitespace) || t.is(TokenKind::Newline) || t.is(TokenKind::Comment)) {
            if (!last_was_space) {
                out.push_back(' ');
                last_was_space = true;
            }
        } else {
            out.append(t.text);
            last_was_space = false;
        }
    }
    return out;
}

} // namespace

Preprocessor::Preprocessor(PreprocessOptions options) : options_(std::move(options)) {
    define_object_macro("__ZPP__", "1");
    for (const auto& [name, val] : options_.cli_defines) {
        define_object_macro(name, val.empty() ? "1" : val);
    }
    for (const auto& name : options_.cli_undefs) {
        undef_macro(name);
    }
}

void Preprocessor::define_object_macro(std::string name, std::string_view replacement_text) {
    MacroDef def;
    def.name = name;
    def.is_function_like = false;
    def.is_variadic = false;
    def.body = trim_whitespace_tokens(Lexer::tokenize(replacement_text, "<define>", 1, 1));
    macros_[std::move(name)] = std::move(def);
}

void Preprocessor::undef_macro(std::string_view name) {
    macros_.erase(std::string(name));
}

bool Preprocessor::is_defined(std::string_view name) const {
    if (name == "__FILE__" || name == "__LINE__" || name == "__COUNTER__") {
        return true;
    }
    return macros_.find(std::string(name)) != macros_.end();
}

PreprocessResult Preprocessor::process_file(const std::filesystem::path& filepath) {
    std::filesystem::path abs_path = std::filesystem::weakly_canonical(filepath);
    dependencies_.insert(abs_path);
    std::string content = read_file_to_string(abs_path);

    std::string out;
    out.reserve(content.size());
    process_stream(content, abs_path.string(), abs_path.parent_path(), out, true);

    PreprocessResult res;
    res.output = std::move(out);
    res.dependencies.assign(dependencies_.begin(), dependencies_.end());
    res.warnings = warnings_;
    return res;
}

PreprocessResult Preprocessor::process_string(std::string_view source,
                                              std::string_view filename,
                                              const std::filesystem::path& base_dir) {
    std::string out;
    out.reserve(source.size());
    process_stream(source, filename, base_dir, out, true);

    PreprocessResult res;
    res.output = std::move(out);
    res.dependencies.assign(dependencies_.begin(), dependencies_.end());
    res.warnings = warnings_;
    return res;
}

void Preprocessor::process_stream(std::string_view source,
                                  std::string_view filename,
                                  const std::filesystem::path& base_dir,
                                  std::string& out_text,
                                  bool preserve_line_count) {
    std::vector<LogicalLine> lines = Lexer::split_logical_lines(source);
    std::vector<CondFrame> cond_stack;

    auto is_currently_active = [&]() noexcept {
        return cond_stack.empty() || cond_stack.back().current_active;
    };

    auto emit_blank_lines = [&](std::size_t count) {
        if (preserve_line_count) {
            out_text.append(count, '\n');
        }
    };

    for (std::size_t idx = 0; idx < lines.size(); ++idx) {
        const LogicalLine& ll = lines[idx];

        if (ll.is_directive) {
            handle_directive(ll, filename, base_dir, cond_stack, out_text);
            emit_blank_lines(ll.physical_line_count);
            continue;
        }

        if (!is_currently_active()) {
            emit_blank_lines(ll.physical_line_count);
            continue;
        }

        // Check if this active line starts an unclosed function-like macro invocation that spans lines
        std::string combined_line = ll.full_raw_text;
        std::size_t extra_merged_lines = 0;

        auto needs_more_lines_for_macro = [&](std::string_view text_so_far) -> bool {
            std::vector<Token> toks = Lexer::tokenize(text_so_far, filename, ll.start_line, 1);
            for (std::size_t i = 0; i < toks.size(); ++i) {
                if (toks[i].is(TokenKind::Identifier)) {
                    auto it = macros_.find(toks[i].text);
                    if (it != macros_.end() && it->second.is_function_like) {
                        std::size_t j = i + 1;
                        while (j < toks.size() &&
                               (toks[j].is(TokenKind::Whitespace) ||
                                toks[j].is(TokenKind::Comment) ||
                                toks[j].is(TokenKind::Newline))) {
                            ++j;
                        }
                        if (j < toks.size() && toks[j].is_punct("(")) {
                            int depth = 0;
                            for (std::size_t k = j; k < toks.size(); ++k) {
                                if (toks[k].is_punct("(")) {
                                    ++depth;
                                } else if (toks[k].is_punct(")")) {
                                    --depth;
                                    if (depth == 0) {
                                        i = k;
                                        break;
                                    }
                                }
                            }
                            if (depth > 0) {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        };

        while (needs_more_lines_for_macro(combined_line) &&
               idx + 1 < lines.size() &&
               !lines[idx + 1].is_directive) {
            ++idx;
            combined_line.push_back('\n');
            combined_line.append(lines[idx].full_raw_text);
            extra_merged_lines += lines[idx].physical_line_count;
        }

        std::vector<Token> raw_tokens = Lexer::tokenize(combined_line, filename, ll.start_line, 1);
        std::unordered_set<std::string> active_macros;
        std::vector<Token> expanded = expand_tokens(
            raw_tokens, active_macros, filename, ll.start_line, false);

        std::string rendered = tokens_to_string(expanded);
        // Count how many newlines are already inside rendered (if any) so total newlines match
        std::size_t newlines_in_rendered = 0;
        for (char c : rendered) {
            if (c == '\n') {
                ++newlines_in_rendered;
            }
        }
        out_text.append(rendered);
        if (preserve_line_count) {
            std::size_t expected_newlines = ll.physical_line_count + extra_merged_lines;
            if (newlines_in_rendered < expected_newlines) {
                out_text.append(expected_newlines - newlines_in_rendered, '\n');
            }
        } else {
            out_text.push_back('\n');
        }
    }

    if (!cond_stack.empty()) {
        std::ostringstream oss;
        oss << filename << ":" << cond_stack.back().start_line
            << ": error: unterminated conditional directive (#if/#ifdef/#ifndef without #endif)";
        throw std::runtime_error(oss.str());
    }
}

void Preprocessor::handle_directive(const LogicalLine& ll,
                                    std::string_view filename,
                                    const std::filesystem::path& base_dir,
                                    std::vector<CondFrame>& cond_stack,
                                    std::string& out_text) {
    const std::string& dir = ll.directive_name;
    if (dir.empty()) {
        return; // Null directive '#'
    }

    auto make_err = [&](const std::string& msg) {
        std::ostringstream oss;
        oss << filename << ":" << ll.start_line << ": error: " << msg;
        return std::runtime_error(oss.str());
    };

    bool parent_active = cond_stack.empty() || cond_stack.back().current_active;

    // Conditional directives must be tracked even when currently inactive
    if (dir == "ifdef" || dir == "ifndef") {
        CondFrame frame;
        frame.parent_active = parent_active;
        frame.start_line = ll.start_line;
        if (!parent_active) {
            frame.current_active = false;
            frame.branch_taken = true;
        } else {
            std::vector<Token> toks = trim_whitespace_tokens(
                Lexer::tokenize(ll.directive_raw_body, filename, ll.start_line, 1));
            if (toks.empty() || !toks[0].is(TokenKind::Identifier)) {
                throw make_err("expected macro identifier after #" + dir);
            }
            bool def = is_defined(toks[0].text);
            bool cond = (dir == "ifdef") ? def : !def;
            frame.current_active = cond;
            frame.branch_taken = cond;
        }
        cond_stack.push_back(frame);
        return;
    }

    if (dir == "if") {
        CondFrame frame;
        frame.parent_active = parent_active;
        frame.start_line = ll.start_line;
        if (!parent_active) {
            frame.current_active = false;
            frame.branch_taken = true;
        } else {
            bool cond = evaluate_if_expr(ll.directive_raw_body, filename, ll.start_line);
            frame.current_active = cond;
            frame.branch_taken = cond;
        }
        cond_stack.push_back(frame);
        return;
    }

    if (dir == "elif") {
        if (cond_stack.empty()) {
            throw make_err("#elif without matching #if/#ifdef/#ifndef");
        }
        CondFrame& frame = cond_stack.back();
        if (frame.seen_else) {
            throw make_err("#elif after #else");
        }
        if (!frame.parent_active || frame.branch_taken) {
            frame.current_active = false;
        } else {
            bool cond = evaluate_if_expr(ll.directive_raw_body, filename, ll.start_line);
            frame.current_active = cond;
            frame.branch_taken = cond;
        }
        return;
    }

    if (dir == "else") {
        if (cond_stack.empty()) {
            throw make_err("#else without matching #if/#ifdef/#ifndef");
        }
        CondFrame& frame = cond_stack.back();
        if (frame.seen_else) {
            throw make_err("duplicate #else in conditional block");
        }
        frame.seen_else = true;
        frame.current_active = frame.parent_active && !frame.branch_taken;
        frame.branch_taken = true;
        return;
    }

    if (dir == "endif") {
        if (cond_stack.empty()) {
            throw make_err("#endif without matching #if/#ifdef/#ifndef");
        }
        cond_stack.pop_back();
        return;
    }

    // If we are inside an inactive conditional block, ignore all other directives
    if (!parent_active) {
        return;
    }

    if (dir == "define") {
        parse_and_register_define(ll.directive_raw_body, filename, ll.start_line);
        return;
    }

    if (dir == "undef") {
        std::vector<Token> toks = trim_whitespace_tokens(
            Lexer::tokenize(ll.directive_raw_body, filename, ll.start_line, 1));
        if (toks.empty() || !toks[0].is(TokenKind::Identifier)) {
            throw make_err("expected macro identifier after #undef");
        }
        undef_macro(toks[0].text);
        return;
    }

    if (dir == "include") {
        std::vector<Token> raw_toks = Lexer::tokenize(ll.directive_raw_body, filename, ll.start_line, 1);
        std::unordered_set<std::string> active;
        std::vector<Token> toks = trim_whitespace_tokens(
            expand_tokens(raw_toks, active, filename, ll.start_line, false));
        if (toks.empty()) {
            throw make_err("expected file path after #include");
        }
        std::string include_spec;
        bool is_system = false;
        if (toks[0].is(TokenKind::StringLiteral)) {
            include_spec = Lexer::unquote_string(toks[0].text);
            is_system = false;
        } else if (toks[0].is_punct("<")) {
            std::string combined;
            for (std::size_t i = 1; i < toks.size(); ++i) {
                if (toks[i].is_punct(">")) {
                    break;
                }
                combined.append(toks[i].text);
            }
            include_spec = combined;
            is_system = true;
        } else {
            throw make_err("invalid #include syntax");
        }

        std::filesystem::path inc_path = resolve_include(
            include_spec, is_system, base_dir, filename, ll.start_line);
        std::filesystem::path canon_inc = std::filesystem::weakly_canonical(inc_path);
        dependencies_.insert(canon_inc);

        if (pragma_once_files_.find(canon_inc) != pragma_once_files_.end()) {
            return;
        }

        std::string inc_content = read_file_to_string(canon_inc);
        // Process included file inline without adding extra line-preservation padding to parent line
        std::string inc_out;
        process_stream(inc_content, canon_inc.string(), canon_inc.parent_path(), inc_out, false);
        // Strip trailing newlines from inc_out so it doesn't shift the parent file's line numbering
        // unless the included file emits actual code
        while (!inc_out.empty() && (inc_out.back() == '\n' || inc_out.back() == '\r')) {
            inc_out.pop_back();
        }
        if (!inc_out.empty()) {
            // Collapse multi-line included code onto a single line if possible so parent line numbers stay 1:1
            for (char& ch : inc_out) {
                if (ch == '\n' || ch == '\r') {
                    ch = ' ';
                }
            }
            out_text.append(inc_out);
        }
        return;
    }

    if (dir == "pragma") {
        std::vector<Token> toks = trim_whitespace_tokens(
            Lexer::tokenize(ll.directive_raw_body, filename, ll.start_line, 1));
        if (!toks.empty() && toks[0].is_ident("once")) {
            if (!filename.empty() && filename.front() != '<') {
                pragma_once_files_.insert(std::filesystem::weakly_canonical(filename));
            }
        }
        return;
    }

    if (dir == "error") {
        std::string msg(ll.directive_raw_body);
        std::size_t first = msg.find_first_not_of(" \t");
        if (first != std::string::npos) {
            msg = msg.substr(first);
        }
        throw make_err("#error " + msg);
    }

    if (dir == "warning") {
        std::string msg(ll.directive_raw_body);
        std::size_t first = msg.find_first_not_of(" \t");
        if (first != std::string::npos) {
            msg = msg.substr(first);
        }
        std::ostringstream oss;
        oss << filename << ":" << ll.start_line << ": warning: #warning " << msg;
        warnings_.push_back(oss.str());
        return;
    }

    throw make_err("unknown preprocessor directive '#" + dir + "'");
}

void Preprocessor::parse_and_register_define(std::string_view raw_body,
                                             std::string_view filename,
                                             std::size_t line) {
    std::vector<Token> toks = Lexer::tokenize(raw_body, filename, line, 1);
    std::size_t idx = 0;
    while (idx < toks.size() && toks[idx].is(TokenKind::Whitespace)) {
        ++idx;
    }
    if (idx >= toks.size() || !toks[idx].is(TokenKind::Identifier)) {
        std::ostringstream oss;
        oss << filename << ":" << line << ": error: expected macro name after #define";
        throw std::runtime_error(oss.str());
    }

    MacroDef def;
    def.name = toks[idx].text;
    ++idx;

    // Function-like macro iff '(' immediately follows macro name with NO whitespace
    if (idx < toks.size() && toks[idx].is_punct("(")) {
        def.is_function_like = true;
        ++idx; // consume '('

        while (idx < toks.size()) {
            while (idx < toks.size() && toks[idx].is(TokenKind::Whitespace)) {
                ++idx;
            }
            if (idx < toks.size() && toks[idx].is_punct(")")) {
                ++idx;
                break;
            }
            if (idx < toks.size() && toks[idx].is_punct("...")) {
                def.is_variadic = true;
                ++idx;
                while (idx < toks.size() && toks[idx].is(TokenKind::Whitespace)) {
                    ++idx;
                }
                if (idx >= toks.size() || !toks[idx].is_punct(")")) {
                    std::ostringstream oss;
                    oss << filename << ":" << line << ": error: '...' must be the last parameter in #"
                        << "define " << def.name;
                    throw std::runtime_error(oss.str());
                }
                ++idx;
                break;
            }
            if (idx >= toks.size() || !toks[idx].is(TokenKind::Identifier)) {
                std::ostringstream oss;
                oss << filename << ":" << line << ": error: expected parameter name in #define "
                    << def.name;
                throw std::runtime_error(oss.str());
            }
            def.params.push_back(toks[idx].text);
            ++idx;
            while (idx < toks.size() && toks[idx].is(TokenKind::Whitespace)) {
                ++idx;
            }
            if (idx < toks.size() && toks[idx].is_punct(",")) {
                ++idx;
                continue;
            }
            if (idx < toks.size() && toks[idx].is_punct(")")) {
                ++idx;
                break;
            }
            std::ostringstream oss;
            oss << filename << ":" << line << ": error: expected ',' or ')' in parameter list of #define "
                << def.name;
            throw std::runtime_error(oss.str());
        }
    }

    std::vector<Token> body_toks(toks.begin() + static_cast<std::ptrdiff_t>(idx), toks.end());
    def.body = trim_whitespace_tokens(body_toks);
    macros_[def.name] = std::move(def);
}

bool Preprocessor::evaluate_if_expr(std::string_view expr_body,
                                    std::string_view filename,
                                    std::size_t line) {
    std::vector<Token> raw_toks = Lexer::tokenize(expr_body, filename, line, 1);
    std::unordered_set<std::string> active_macros;
    std::vector<Token> expanded = expand_tokens(raw_toks, active_macros, filename, line, true);

    std::int64_t val = ExprEvaluator::evaluate(
        expanded,
        [this](std::string_view name) { return is_defined(name); },
        filename,
        line);
    return val != 0;
}

std::filesystem::path Preprocessor::resolve_include(std::string_view include_spec,
                                                    bool is_system,
                                                    const std::filesystem::path& base_dir,
                                                    std::string_view filename,
                                                    std::size_t line) const {
    std::filesystem::path rel(include_spec);
    if (rel.is_absolute() && std::filesystem::exists(rel)) {
        return rel;
    }
    if (!is_system && !base_dir.empty()) {
        std::filesystem::path cand = base_dir / rel;
        if (std::filesystem::exists(cand)) {
            return cand;
        }
    }
    for (const auto& dir : options_.include_dirs) {
        std::filesystem::path cand = dir / rel;
        if (std::filesystem::exists(cand)) {
            return cand;
        }
    }
    if (is_system && !base_dir.empty()) {
        std::filesystem::path cand = base_dir / rel;
        if (std::filesystem::exists(cand)) {
            return cand;
        }
    }
    std::ostringstream oss;
    oss << filename << ":" << line << ": error:included file not found: '" << include_spec << "'";
    throw std::runtime_error(oss.str());
}

std::vector<Token> Preprocessor::expand_tokens(const std::vector<Token>& tokens,
                                               std::unordered_set<std::string>& active_macros,
                                               std::string_view current_file,
                                               std::size_t current_line,
                                               bool in_preprocessor_expr) {
    std::vector<Token> out;
    out.reserve(tokens.size());

    std::size_t i = 0;
    while (i < tokens.size()) {
        const Token& tok = tokens[i];

        // In #if/#elif expressions, do not expand the identifier inside `defined(X)` or `defined X`
        if (in_preprocessor_expr && tok.is_ident("defined")) {
            out.push_back(tok);
            ++i;
            while (i < tokens.size() && tokens[i].is(TokenKind::Whitespace)) {
                out.push_back(tokens[i]);
                ++i;
            }
            if (i < tokens.size() && tokens[i].is_punct("(")) {
                out.push_back(tokens[i]);
                ++i;
                while (i < tokens.size() && tokens[i].is(TokenKind::Whitespace)) {
                    out.push_back(tokens[i]);
                    ++i;
                }
                if (i < tokens.size()) {
                    out.push_back(tokens[i]);
                    ++i;
                }
                while (i < tokens.size() && tokens[i].is(TokenKind::Whitespace)) {
                    out.push_back(tokens[i]);
                    ++i;
                }
                if (i < tokens.size() && tokens[i].is_punct(")")) {
                    out.push_back(tokens[i]);
                    ++i;
                }
            } else if (i < tokens.size()) {
                out.push_back(tokens[i]);
                ++i;
            }
            continue;
        }

        if (!tok.is(TokenKind::Identifier)) {
            out.push_back(tok);
            ++i;
            continue;
        }

        // Built-in dynamic macros
        if (tok.text == "__FILE__") {
            out.push_back(Token{
                .kind = TokenKind::StringLiteral,
                .text = Lexer::quote_string(current_file),
                .file = std::string(current_file),
                .line = tok.line,
                .column = tok.column,
            });
            ++i;
            continue;
        }
        if (tok.text == "__LINE__") {
            out.push_back(Token{
                .kind = TokenKind::NumberLiteral,
                .text = std::to_string(tok.line ? tok.line : current_line),
                .file = std::string(current_file),
                .line = tok.line,
                .column = tok.column,
            });
            ++i;
            continue;
        }
        if (tok.text == "__COUNTER__") {
            out.push_back(Token{
                .kind = TokenKind::NumberLiteral,
                .text = std::to_string(counter_++),
                .file = std::string(current_file),
                .line = tok.line,
                .column = tok.column,
            });
            ++i;
            continue;
        }

        auto it = macros_.find(tok.text);
        if (it == macros_.end() || active_macros.find(tok.text) != active_macros.end()) {
            out.push_back(tok);
            ++i;
            continue;
        }

        const MacroDef& def = it->second;
        if (!def.is_function_like) {
            // In Zig code, do not expand an object-like macro when it is a field/decl access
            // or enum literal immediately following '.' (e.g. `c.MATH_LIB_SCALE` from @cImport).
            if (!in_preprocessor_expr && !out.empty() && out.back().is_punct(".")) {
                out.push_back(tok);
                ++i;
                continue;
            }
            std::vector<Token> sub = substitute_function_macro(
                def, {}, active_macros, current_file, tok.line);
            if (in_preprocessor_expr) {
                active_macros.insert(def.name);
                sub = expand_tokens(sub, active_macros, current_file, tok.line, true);
                active_macros.erase(def.name);
            }
            out.insert(out.end(), sub.begin(), sub.end());
            ++i;
            continue;
        }

        // Function-like macro: look ahead past whitespace/comments/newlines for '('
        std::size_t look = i + 1;
        while (look < tokens.size() &&
               (tokens[look].is(TokenKind::Whitespace) ||
                tokens[look].is(TokenKind::Comment) ||
                tokens[look].is(TokenKind::Newline))) {
            ++look;
        }
        if (look >= tokens.size() || !tokens[look].is_punct("(")) {
            // Function-like macro name not followed by '(' is not expanded
            out.push_back(tok);
            ++i;
            continue;
        }

        // Parse macro arguments between '(' and matching ')'
        std::size_t arg_pos = look + 1;
        int paren_depth = 1;
        std::vector<std::vector<Token>> args;
        std::vector<Token> current_arg;

        while (arg_pos < tokens.size() && paren_depth > 0) {
            const Token& atok = tokens[arg_pos];
            if (atok.is_punct("(")) {
                ++paren_depth;
                current_arg.push_back(atok);
            } else if (atok.is_punct(")")) {
                --paren_depth;
                if (paren_depth == 0) {
                    args.push_back(trim_whitespace_tokens(current_arg));
                    ++arg_pos;
                    break;
                }
                current_arg.push_back(atok);
            } else if (atok.is_punct(",") && paren_depth == 1) {
                args.push_back(trim_whitespace_tokens(current_arg));
                current_arg.clear();
            } else {
                current_arg.push_back(atok);
            }
            ++arg_pos;
        }

        if (paren_depth != 0) {
            std::ostringstream oss;
            oss << current_file << ":" << tok.line
                << ": error: unterminated argument list invoking macro '" << def.name << "'";
            throw std::runtime_error(oss.str());
        }

        // Handle 0-argument invocation FOO() where args has 1 empty element
        if (def.params.empty() && !def.is_variadic && args.size() == 1 && args[0].empty()) {
            args.clear();
        }

        if ((!def.is_variadic && args.size() != def.params.size()) ||
            (def.is_variadic && args.size() < def.params.size())) {
            // Allow variadic macro to omit trailing variadic argument when using GNU/C23 style
            if (!(def.is_variadic && args.size() == 1 && args[0].empty() && def.params.empty())) {
                std::ostringstream oss;
                oss << current_file << ":" << tok.line << ": error: macro '" << def.name
                    << "' expects " << (def.is_variadic ? "at least " : "") << def.params.size()
                    << " argument(s), got " << args.size();
                throw std::runtime_error(oss.str());
            }
        }

        std::vector<Token> sub = substitute_function_macro(
            def, args, active_macros, current_file, tok.line);
        if (in_preprocessor_expr) {
            sub = expand_tokens(sub, active_macros, current_file, tok.line, true);
        }
        out.insert(out.end(), sub.begin(), sub.end());
        i = arg_pos;
    }

    return out;
}

std::vector<Token> Preprocessor::substitute_function_macro(
    const MacroDef& def,
    const std::vector<std::vector<Token>>& args,
    std::unordered_set<std::string>& active_macros,
    std::string_view current_file,
    std::size_t current_line) {
    std::unordered_map<std::string, std::vector<Token>> raw_param_map;
    std::unordered_map<std::string, std::vector<Token>> expanded_param_map;

    for (std::size_t p = 0; p < def.params.size() && p < args.size(); ++p) {
        raw_param_map[def.params[p]] = args[p];
        expanded_param_map[def.params[p]] =
            expand_tokens(args[p], active_macros, current_file, current_line, false);
    }

    std::vector<Token> va_raw;
    if (def.is_variadic && args.size() > def.params.size()) {
        for (std::size_t v = def.params.size(); v < args.size(); ++v) {
            if (args.size() == 1 && def.params.empty() && args[0].empty()) {
                break;
            }
            if (v > def.params.size()) {
                va_raw.push_back(Token{
                    .kind = TokenKind::Punctuator,
                    .text = ",",
                    .file = std::string(current_file),
                    .line = current_line,
                    .column = 1,
                });
                va_raw.push_back(Token{
                    .kind = TokenKind::Whitespace,
                    .text = " ",
                    .file = std::string(current_file),
                    .line = current_line,
                    .column = 1,
                });
            }
            va_raw.insert(va_raw.end(), args[v].begin(), args[v].end());
        }
    }
    raw_param_map["__VA_ARGS__"] = va_raw;
    expanded_param_map["__VA_ARGS__"] =
        expand_tokens(va_raw, active_macros, current_file, current_line, false);

    auto get_raw_arg_string = [&](const std::string& name) -> std::string {
        auto it = raw_param_map.find(name);
        if (it == raw_param_map.end()) {
            return {};
        }
        return normalize_whitespace_for_stringify(it->second);
    };

    // Helper to find next non-whitespace token index in def.body
    const auto& body = def.body;
    auto next_non_ws = [&](std::size_t start) -> std::size_t {
        std::size_t k = start;
        while (k < body.size() && body[k].is(TokenKind::Whitespace)) {
            ++k;
        }
        return k;
    };

    std::vector<Token> replaced;
    std::size_t i = 0;
    while (i < body.size()) {
        const Token& tok = body[i];

        // Handle __VA_OPT__(...)
        if (tok.is_ident("__VA_OPT__")) {
            std::size_t lp = next_non_ws(i + 1);
            if (lp < body.size() && body[lp].is_punct("(")) {
                std::size_t cur = lp + 1;
                int depth = 1;
                std::vector<Token> inner;
                while (cur < body.size() && depth > 0) {
                    if (body[cur].is_punct("(")) {
                        ++depth;
                        inner.push_back(body[cur]);
                    } else if (body[cur].is_punct(")")) {
                        --depth;
                        if (depth == 0) {
                            ++cur;
                            break;
                        }
                        inner.push_back(body[cur]);
                    } else {
                        inner.push_back(body[cur]);
                    }
                    ++cur;
                }
                if (!va_raw.empty()) {
                    MacroDef temp_def = def;
                    temp_def.body = inner;
                    auto inner_sub = substitute_function_macro(
                        temp_def, args, active_macros, current_file, current_line);
                    replaced.insert(replaced.end(), inner_sub.begin(), inner_sub.end());
                }
                i = cur;
                continue;
            }
        }

        // Handle GNU `, ## __VA_ARGS__` comma elision
        if (tok.is_punct(",")) {
            std::size_t hash_idx = next_non_ws(i + 1);
            if (hash_idx < body.size() && body[hash_idx].is_punct("##")) {
                std::size_t va_idx = next_non_ws(hash_idx + 1);
                if (va_idx < body.size() && body[va_idx].is_ident("__VA_ARGS__")) {
                    if (!va_raw.empty()) {
                        replaced.push_back(tok);
                        replaced.push_back(Token{
                            .kind = TokenKind::Whitespace,
                            .text = " ",
                            .file = std::string(current_file),
                            .line = current_line,
                            .column = tok.column,
                        });
                        const auto& exp_va = expanded_param_map["__VA_ARGS__"];
                        replaced.insert(replaced.end(), exp_va.begin(), exp_va.end());
                    }
                    i = va_idx + 1;
                    continue;
                }
            }
        }

        // Handle stringification `#param`
        if (tok.is_punct("#")) {
            std::size_t param_idx = next_non_ws(i + 1);
            if (param_idx < body.size() && body[param_idx].is(TokenKind::Identifier) &&
                raw_param_map.find(body[param_idx].text) != raw_param_map.end()) {
                std::string str_val = Lexer::quote_string(get_raw_arg_string(body[param_idx].text));
                replaced.push_back(Token{
                    .kind = TokenKind::StringLiteral,
                    .text = std::move(str_val),
                    .file = std::string(current_file),
                    .line = current_line,
                    .column = tok.column,
                });
                i = param_idx + 1;
                continue;
            }
        }

        // Check if this token is the left operand of token pasting `left ## right`
        std::size_t maybe_paste = next_non_ws(i + 1);
        if (!tok.is(TokenKind::Whitespace) && maybe_paste < body.size() &&
            body[maybe_paste].is_punct("##")) {
            auto token_raw_text = [&](const Token& t) -> std::string {
                if (t.is(TokenKind::Identifier)) {
                    auto it = raw_param_map.find(t.text);
                    if (it != raw_param_map.end()) {
                        return tokens_to_string(it->second);
                    }
                }
                return t.text;
            };

            std::string pasted = token_raw_text(tok);
            std::size_t cur = maybe_paste;
            while (cur < body.size() && body[cur].is_punct("##")) {
                std::size_t rhs_idx = next_non_ws(cur + 1);
                if (rhs_idx >= body.size()) {
                    break;
                }
                pasted.append(token_raw_text(body[rhs_idx]));
                i = rhs_idx;
                cur = next_non_ws(rhs_idx + 1);
            }
            std::vector<Token> pasted_toks =
                Lexer::tokenize(pasted, current_file, current_line, tok.column);
            replaced.insert(replaced.end(), pasted_toks.begin(), pasted_toks.end());
            ++i;
            continue;
        }

        // Normal parameter replacement
        if (tok.is(TokenKind::Identifier)) {
            auto it = expanded_param_map.find(tok.text);
            if (it != expanded_param_map.end()) {
                replaced.insert(replaced.end(), it->second.begin(), it->second.end());
                ++i;
                continue;
            }
        }

        replaced.push_back(tok);
        ++i;
    }

    // Also handle object-like `##` pasting if any remained
    active_macros.insert(def.name);
    std::vector<Token> final_tokens =
        expand_tokens(replaced, active_macros, current_file, current_line, false);
    active_macros.erase(def.name);
    return final_tokens;
}

std::string Preprocessor::tokens_to_string(const std::vector<Token>& tokens) {
    std::string out;
    for (const auto& t : tokens) {
        out.append(t.text);
    }
    return out;
}

std::vector<Token> Preprocessor::trim_whitespace_tokens(const std::vector<Token>& tokens) {
    std::size_t start = 0;
    while (start < tokens.size() &&
           (tokens[start].is(TokenKind::Whitespace) || tokens[start].is(TokenKind::Newline))) {
        ++start;
    }
    std::size_t end = tokens.size();
    while (end > start &&
           (tokens[end - 1].is(TokenKind::Whitespace) || tokens[end - 1].is(TokenKind::Newline))) {
        --end;
    }
    return std::vector<Token>(tokens.begin() + static_cast<std::ptrdiff_t>(start),
                              tokens.begin() + static_cast<std::ptrdiff_t>(end));
}

} // namespace zpp
