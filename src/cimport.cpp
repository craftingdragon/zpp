#include "cimport.hpp"
#include "lexer.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace zpp {

namespace {

struct ProcessOutput {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

ProcessOutput run_process(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        throw std::runtime_error("empty command passed to run_process");
    }
#if defined(__unix__) || defined(__APPLE__)
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0) {
        throw std::runtime_error("failed to create stdout pipe");
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        throw std::runtime_error("failed to create stderr pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        throw std::runtime_error("fork() failed");
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        std::vector<char*> c_args;
        c_args.reserve(argv.size() + 1);
        for (const auto& s : argv) {
            c_args.push_back(const_cast<char*>(s.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    ProcessOutput out;
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
        out.stdout_text.append(buf, static_cast<std::size_t>(n));
    }
    while ((n = read(stderr_pipe[0], buf, sizeof(buf))) > 0) {
        out.stderr_text.append(buf, static_cast<std::size_t>(n));
    }
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        out.exit_code = -1;
    } else if (WIFEXITED(status)) {
        out.exit_code = WEXITSTATUS(status);
    } else {
        out.exit_code = -1;
    }
    return out;
#else
    std::ostringstream cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) cmd << ' ';
        cmd << '"' << argv[i] << '"';
    }
    FILE* pipe_fp = popen(cmd.str().c_str(), "r");
    if (!pipe_fp) {
        throw std::runtime_error("popen() failed");
    }
    ProcessOutput out;
    char buf[4096];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), pipe_fp)) {
        out.stdout_text.append(buf, n);
    }
    out.exit_code = pclose(pipe_fp);
    return out;
#endif
}

std::string compute_hash_hex(std::string_view data) {
    // 128-bit dual FNV-1a hash for deterministic cache keys
    std::uint64_t h1 = 14695981039346656037ULL;
    std::uint64_t h2 = 1099511628211ULL ^ 0x9e3779b97f4a7c15ULL;
    for (unsigned char c : data) {
        h1 ^= c;
        h1 *= 1099511628211ULL;
        h2 ^= static_cast<unsigned char>(c ^ 0x5aU);
        h2 *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << h1 << std::setw(16) << h2;
    return oss.str();
}

std::string normalize_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

} // namespace

CImportProcessor::CImportProcessor(CImportOptions options) : options_(std::move(options)) {}

CImportResult CImportProcessor::process(std::string_view zig_source, std::string_view filename) {
    std::vector<Token> tokens = Lexer::tokenize(zig_source, filename, 1, 1);

    CImportResult result;
    std::string out;
    out.reserve(zig_source.size());
    std::set<std::filesystem::path> unique_c_deps;

    auto next_non_trivial = [&](std::size_t idx) -> std::size_t {
        while (idx < tokens.size() &&
               (tokens[idx].is(TokenKind::Whitespace) ||
                tokens[idx].is(TokenKind::Newline) ||
                tokens[idx].is(TokenKind::Comment))) {
            ++idx;
        }
        return idx;
    };

    std::size_t i = 0;
    while (i < tokens.size()) {
        if (!tokens[i].is_builtin("@cImport")) {
            out.append(tokens[i].text);
            ++i;
            continue;
        }

        std::size_t cimport_line = tokens[i].line;
        std::size_t open_paren = next_non_trivial(i + 1);
        if (open_paren >= tokens.size() || !tokens[open_paren].is_punct("(")) {
            std::ostringstream oss;
            oss << filename << ":" << cimport_line << ": error: expected '(' after @cImport";
            throw std::runtime_error(oss.str());
        }

        // Find matching ')' for @cImport(...)
        std::size_t cur = open_paren + 1;
        int paren_depth = 1;
        std::size_t newlines_spanned = 0;
        for (std::size_t k = i; k <= open_paren; ++k) {
            if (tokens[k].is(TokenKind::Newline)) {
                ++newlines_spanned;
            }
        }

        std::string inner_raw;
        while (cur < tokens.size() && paren_depth > 0) {
            if (tokens[cur].is(TokenKind::Newline)) {
                ++newlines_spanned;
            }
            if (tokens[cur].is_punct("(")) {
                ++paren_depth;
                inner_raw.append(tokens[cur].text);
            } else if (tokens[cur].is_punct(")")) {
                --paren_depth;
                if (paren_depth == 0) {
                    ++cur;
                    break;
                }
                inner_raw.append(tokens[cur].text);
            } else {
                inner_raw.append(tokens[cur].text);
            }
            ++cur;
        }

        if (paren_depth != 0) {
            std::ostringstream oss;
            oss << filename << ":" << cimport_line << ": error: unterminated @cImport(...) block";
            throw std::runtime_error(oss.str());
        }

        std::string header_content = generate_c_header_from_tokens(inner_raw, filename, cimport_line);
        std::vector<std::filesystem::path> block_deps;
        std::filesystem::path translated_zig_path =
            translate_c_header(header_content, block_deps, filename, cimport_line);

        result.generated_zig_files.push_back(translated_zig_path);
        for (const auto& d : block_deps) {
            unique_c_deps.insert(d);
        }

        // Compute relative path from output_file_path's parent directory to translated_zig_path
        std::filesystem::path import_path = translated_zig_path;
        if (!options_.output_file_path.empty()) {
            std::filesystem::path out_dir = std::filesystem::weakly_canonical(
                std::filesystem::absolute(options_.output_file_path).parent_path());
            std::filesystem::path target_canon = std::filesystem::weakly_canonical(
                std::filesystem::absolute(translated_zig_path));
            import_path = std::filesystem::proximate(target_canon, out_dir);
        }

        std::string rel_import = normalize_slashes(import_path.string());
        out.append("@import(");
        out.append(Lexer::quote_string(rel_import));
        out.push_back(')');
        out.append(newlines_spanned, '\n');

        i = cur;
    }

    result.transformed_source = std::move(out);
    result.c_header_dependencies.assign(unique_c_deps.begin(), unique_c_deps.end());
    return result;
}

std::string CImportProcessor::generate_c_header_from_tokens(std::string_view block_source,
                                                            std::string_view filename,
                                                            std::size_t start_line) const {
    std::vector<Token> toks = Lexer::tokenize(block_source, filename, start_line, 1);
    std::vector<Token> clean;
    clean.reserve(toks.size());
    for (const auto& t : toks) {
        if (!t.is(TokenKind::Whitespace) && !t.is(TokenKind::Newline) && !t.is(TokenKind::Comment)) {
            clean.push_back(t);
        }
    }

    auto make_err = [&](std::size_t idx, const std::string& msg) {
        std::size_t ln = (idx < clean.size()) ? clean[idx].line : start_line;
        std::ostringstream oss;
        oss << filename << ":" << ln << ": error: " << msg;
        return std::runtime_error(oss.str());
    };

    std::size_t pos = 0;
    // Strip optional outer '{' ... '}' block
    if (!clean.empty() && clean.front().is_punct("{") && clean.back().is_punct("}")) {
        clean.erase(clean.begin());
        clean.pop_back();
    }

    std::ostringstream header;
    header << "/* Auto-generated by zpp for @cImport */\n";

    while (pos < clean.size()) {
        if (clean[pos].is_punct(";")) {
            ++pos;
            continue;
        }

        const Token& dir_tok = clean[pos];
        if (dir_tok.is_builtin("@cInclude")) {
            ++pos;
            if (pos >= clean.size() || !clean[pos].is_punct("(")) {
                throw make_err(pos, "expected '(' after @cInclude");
            }
            ++pos;
            if (pos >= clean.size() || !clean[pos].is(TokenKind::StringLiteral)) {
                throw make_err(pos, "expected string literal inside @cInclude(...)");
            }
            std::string inc_file = Lexer::unquote_string(clean[pos].text);
            ++pos;
            if (pos >= clean.size() || !clean[pos].is_punct(")")) {
                throw make_err(pos, "expected ')' after @cInclude argument");
            }
            ++pos;
            if (pos < clean.size() && clean[pos].is_punct(";")) {
                ++pos;
            }
            header << "#include <" << inc_file << ">\n";
            continue;
        }

        if (dir_tok.is_builtin("@cDefine")) {
            ++pos;
            if (pos >= clean.size() || !clean[pos].is_punct("(")) {
                throw make_err(pos, "expected '(' after @cDefine");
            }
            ++pos;
            if (pos >= clean.size() || !clean[pos].is(TokenKind::StringLiteral)) {
                throw make_err(pos, "expected macro name string literal as first argument to @cDefine");
            }
            std::string macro_name = Lexer::unquote_string(clean[pos].text);
            ++pos;
            if (pos >= clean.size() || !clean[pos].is_punct(",")) {
                throw make_err(pos, "expected ',' after first argument to @cDefine");
            }
            ++pos;
            if (pos >= clean.size()) {
                throw make_err(pos, "expected value argument in @cDefine");
            }

            std::string macro_val;
            if (clean[pos].is_punct("{")) {
                // @cDefine("FOO", {})
                ++pos;
                if (pos >= clean.size() || !clean[pos].is_punct("}")) {
                    throw make_err(pos, "expected '}' in @cDefine empty block value");
                }
                ++pos;
            } else if (clean[pos].is_ident("void") && pos + 2 < clean.size() &&
                       clean[pos + 1].is_punct("{") && clean[pos + 2].is_punct("}")) {
                // @cDefine("FOO", void{})
                pos += 3;
            } else if (clean[pos].is(TokenKind::StringLiteral)) {
                macro_val = Lexer::unquote_string(clean[pos].text);
                ++pos;
            } else {
                // Collect tokens until matching ')'
                int depth = 0;
                while (pos < clean.size()) {
                    if (clean[pos].is_punct("(")) {
                        ++depth;
                    } else if (clean[pos].is_punct(")")) {
                        if (depth == 0) {
                            break;
                        }
                        --depth;
                    }
                    if (!macro_val.empty()) {
                        macro_val.push_back(' ');
                    }
                    macro_val.append(clean[pos].text);
                    ++pos;
                }
            }

            if (pos >= clean.size() || !clean[pos].is_punct(")")) {
                throw make_err(pos, "expected ')' closing @cDefine");
            }
            ++pos;
            if (pos < clean.size() && clean[pos].is_punct(";")) {
                ++pos;
            }

            if (macro_val.empty()) {
                header << "#define " << macro_name << "\n";
            } else {
                header << "#define " << macro_name << " " << macro_val << "\n";
            }
            continue;
        }

        if (dir_tok.is_builtin("@cUndef")) {
            ++pos;
            if (pos >= clean.size() || !clean[pos].is_punct("(")) {
                throw make_err(pos, "expected '(' after @cUndef");
            }
            ++pos;
            if (pos >= clean.size() || !clean[pos].is(TokenKind::StringLiteral)) {
                throw make_err(pos, "expected macro name string literal inside @cUndef(...)");
            }
            std::string macro_name = Lexer::unquote_string(clean[pos].text);
            ++pos;
            if (pos >= clean.size() || !clean[pos].is_punct(")")) {
                throw make_err(pos, "expected ')' after @cUndef argument");
            }
            ++pos;
            if (pos < clean.size() && clean[pos].is_punct(";")) {
                ++pos;
            }
            header << "#undef " << macro_name << "\n";
            continue;
        }

        throw make_err(pos, "unsupported statement '" + dir_tok.text +
                                "' inside @cImport (expected @cInclude, @cDefine, or @cUndef)");
    }

    return header.str();
}

std::filesystem::path CImportProcessor::translate_c_header(
    const std::string& header_content,
    std::vector<std::filesystem::path>& out_deps,
    std::string_view filename,
    std::size_t line) const {
    std::filesystem::path out_dir = options_.cimport_dir;
    if (out_dir.empty()) {
        out_dir = std::filesystem::current_path() / "__zpp_cimports";
    }
    std::filesystem::create_directories(out_dir);

    // Build hash input from header content + flags + include dirs + target
    std::ostringstream hash_input;
    hash_input << header_content << "\n--target=" << options_.target_triple
               << "\n--libc=" << (options_.link_libc ? "1" : "0");
    if (!options_.source_file_dir.empty()) {
        hash_input << "\n-I" << options_.source_file_dir.string();
    }
    for (const auto& inc : options_.include_dirs) {
        hash_input << "\n-I" << inc.string();
    }
    for (const auto& [k, v] : options_.defines) {
        hash_input << "\n-D" << k << "=" << v;
    }
    for (const auto& u : options_.undefs) {
        hash_input << "\n-U" << u;
    }
    for (const auto& cf : options_.extra_cflags) {
        hash_input << "\n--cflag=" << cf;
    }

    std::string digest = compute_hash_hex(hash_input.str());
    std::string stem = "cimport_" + digest;
    std::filesystem::path header_path = out_dir / (stem + ".h");
    std::filesystem::path dep_path = out_dir / (stem + ".d");
    std::filesystem::path zig_out_path = out_dir / (stem + ".zig");

    // Write the synthetic C header if it doesn't exist or differs
    {
        bool write_hdr = true;
        if (std::filesystem::exists(header_path)) {
            std::ifstream ifs(header_path, std::ios::binary);
            std::ostringstream existing;
            existing << ifs.rdbuf();
            if (existing.str() == header_content) {
                write_hdr = false;
            }
        }
        if (write_hdr) {
            std::ofstream ofs(header_path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                throw std::runtime_error("failed to write synthetic C header: " + header_path.string());
            }
            ofs << header_content;
        }
    }

    // Build common include/define flags
    std::vector<std::string> common_c_flags;
    if (!options_.source_file_dir.empty()) {
        common_c_flags.push_back("-I" + options_.source_file_dir.string());
    }
    for (const auto& inc : options_.include_dirs) {
        common_c_flags.push_back("-I" + inc.string());
    }
    for (const auto& [k, v] : options_.defines) {
        if (v.empty()) {
            common_c_flags.push_back("-D" + k + "=1");
        } else {
            common_c_flags.push_back("-D" + k + "=" + v);
        }
    }
    for (const auto& u : options_.undefs) {
        common_c_flags.push_back("-U" + u);
    }

    // 1. Run `zig cc -M -MF <dep_path>` to extract C header dependencies
    {
        std::vector<std::string> cc_argv = {
            options_.zig_executable, "cc", "-M", "-MF", dep_path.string()
        };
        if (!options_.target_triple.empty()) {
            cc_argv.push_back("-target");
            cc_argv.push_back(options_.target_triple);
        }
        cc_argv.insert(cc_argv.end(), common_c_flags.begin(), common_c_flags.end());
        cc_argv.insert(cc_argv.end(), options_.extra_cflags.begin(), options_.extra_cflags.end());
        cc_argv.push_back(header_path.string());

        ProcessOutput cc_res = run_process(cc_argv);
        if (cc_res.exit_code == 0 && std::filesystem::exists(dep_path)) {
            auto parsed = parse_depfile(dep_path);
            for (const auto& p : parsed) {
                if (p != header_path) {
                    out_deps.push_back(p);
                }
            }
        }
    }

    // 2. Run `zig translate-c`
    std::vector<std::string> tc_argv = {options_.zig_executable, "translate-c"};
    if (options_.link_libc) {
        tc_argv.push_back("-lc");
    }
    if (!options_.target_triple.empty()) {
        tc_argv.push_back("-target");
        tc_argv.push_back(options_.target_triple);
    }
    tc_argv.insert(tc_argv.end(), common_c_flags.begin(), common_c_flags.end());
    if (!options_.extra_cflags.empty()) {
        tc_argv.push_back("-cflags");
        tc_argv.insert(tc_argv.end(), options_.extra_cflags.begin(), options_.extra_cflags.end());
        tc_argv.push_back("--");
    }
    tc_argv.push_back(header_path.string());

    ProcessOutput tc_res = run_process(tc_argv);
    if (tc_res.exit_code != 0) {
        std::ostringstream oss;
        oss << filename << ":" << line << ": error: zig translate-c failed (exit code "
            << tc_res.exit_code << "):\n"
            << tc_res.stderr_text;
        throw std::runtime_error(oss.str());
    }

    // Write translated Zig source atomically
#if defined(__unix__) || defined(__APPLE__)
    std::string pid_suffix = std::to_string(getpid());
#else
    std::string pid_suffix = "tmp";
#endif
    std::filesystem::path tmp_path = out_dir / (stem + ".zig.tmp." + pid_suffix);
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw std::runtime_error("failed to write translated C module: " + tmp_path.string());
        }
        ofs << tc_res.stdout_text;
    }
    std::filesystem::rename(tmp_path, zig_out_path);

    return zig_out_path;
}

void CImportProcessor::write_depfile(const std::filesystem::path& depfile_path,
                                     const std::filesystem::path& target_output,
                                     const std::vector<std::filesystem::path>& dependencies) {
    if (depfile_path.has_parent_path()) {
        std::filesystem::create_directories(depfile_path.parent_path());
    }
    auto escape_path = [](const std::string& p) -> std::string {
        std::string out;
        out.reserve(p.size());
        for (char c : p) {
            if (c == ' ') {
                out.append("\\ ");
            } else {
                out.push_back(c);
            }
        }
        return out;
    };

    std::set<std::string> unique_deps;
    std::vector<std::string> ordered_deps;
    for (const auto& d : dependencies) {
        std::string s = d.string();
        if (unique_deps.insert(s).second) {
            ordered_deps.push_back(s);
        }
    }

    std::ofstream ofs(depfile_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("failed to open depfile for writing: " + depfile_path.string());
    }
    ofs << escape_path(target_output.string()) << ":";
    for (const auto& d : ordered_deps) {
        ofs << " \\\n  " << escape_path(d);
    }
    ofs << "\n";
}

std::vector<std::filesystem::path> CImportProcessor::parse_depfile(
    const std::filesystem::path& depfile_path) {
    std::ifstream ifs(depfile_path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string content = oss.str();

    // Find ':' after target name
    std::size_t colon = content.find(':');
    if (colon == std::string::npos) {
        return {};
    }

    std::vector<std::filesystem::path> deps;
    std::size_t i = colon + 1;
    std::string current;
    while (i < content.size()) {
        char c = content[i];
        if (c == '\\' && i + 1 < content.size() &&
            (content[i + 1] == '\n' || content[i + 1] == '\r')) {
            ++i;
            if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
                ++i;
            }
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < content.size() && content[i + 1] == ' ') {
            current.push_back(' ');
            i += 2;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!current.empty()) {
                deps.emplace_back(current);
                current.clear();
            }
            ++i;
            continue;
        }
        current.push_back(c);
        ++i;
    }
    if (!current.empty()) {
        deps.emplace_back(current);
    }
    return deps;
}

} // namespace zpp
