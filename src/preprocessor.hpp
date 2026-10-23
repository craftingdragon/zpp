#pragma once

#include "lexer.hpp"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zpp {

struct MacroDef {
    std::string name;
    bool is_function_like = false;
    bool is_variadic = false;
    std::vector<std::string> params;
    std::vector<Token> body;
};

struct PreprocessOptions {
    std::vector<std::filesystem::path> include_dirs;
    std::vector<std::pair<std::string, std::string>> cli_defines;
    std::vector<std::string> cli_undefs;
};

struct PreprocessResult {
    std::string output;
    std::vector<std::filesystem::path> dependencies;
    std::vector<std::string> warnings;
};

class Preprocessor {
public:
    explicit Preprocessor(PreprocessOptions options = {});

    void define_object_macro(std::string name, std::string_view replacement_text);
    void undef_macro(std::string_view name);
    [[nodiscard]] bool is_defined(std::string_view name) const;
    [[nodiscard]] const std::unordered_map<std::string, MacroDef>& macros() const noexcept {
        return macros_;
    }

    PreprocessResult process_file(const std::filesystem::path& filepath);
    PreprocessResult process_string(std::string_view source,
                                    std::string_view filename = "<input>",
                                    const std::filesystem::path& base_dir = {});

    // Expands macros in an arbitrary token sequence (used by both normal lines and #if expressions).
    std::vector<Token> expand_tokens(const std::vector<Token>& tokens,
                                     std::unordered_set<std::string>& active_macros,
                                     std::string_view current_file,
                                     std::size_t current_line,
                                     bool in_preprocessor_expr = false);

private:
    struct CondFrame {
        bool parent_active = true;
        bool current_active = true;
        bool branch_taken = false;
        bool seen_else = false;
        std::size_t start_line = 1;
    };

    PreprocessOptions options_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::set<std::filesystem::path> pragma_once_files_;
    std::set<std::filesystem::path> dependencies_;
    std::vector<std::string> warnings_;
    std::size_t counter_ = 0;

    void process_stream(std::string_view source,
                        std::string_view filename,
                        const std::filesystem::path& base_dir,
                        std::string& out_text,
                        bool preserve_line_count);

    void handle_directive(const LogicalLine& ll,
                          std::string_view filename,
                          const std::filesystem::path& base_dir,
                          std::vector<CondFrame>& cond_stack,
                          std::string& out_text);

    void parse_and_register_define(std::string_view raw_body,
                                   std::string_view filename,
                                   std::size_t line);

    bool evaluate_if_expr(std::string_view expr_body,
                          std::string_view filename,
                          std::size_t line);

    std::filesystem::path resolve_include(std::string_view include_spec,
                                          bool is_system,
                                          const std::filesystem::path& base_dir,
                                          std::string_view filename,
                                          std::size_t line) const;

    std::vector<Token> substitute_function_macro(
        const MacroDef& def,
        const std::vector<std::vector<Token>>& args,
        std::unordered_set<std::string>& active_macros,
        std::string_view current_file,
        std::size_t current_line);

    static std::string tokens_to_string(const std::vector<Token>& tokens);
    static std::vector<Token> trim_whitespace_tokens(const std::vector<Token>& tokens);
};

} // namespace zpp
