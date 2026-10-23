#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zpp {

struct CImportOptions {
    std::string zig_executable = "zig";
    std::filesystem::path cimport_dir;
    std::filesystem::path source_file_dir;
    std::filesystem::path output_file_path;
    std::string target_triple;
    bool link_libc = true;
    std::vector<std::filesystem::path> include_dirs;
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::string> undefs;
    std::vector<std::string> extra_cflags;
};

struct CImportResult {
    std::string transformed_source;
    std::vector<std::filesystem::path> c_header_dependencies;
    std::vector<std::filesystem::path> generated_zig_files;
};

class CImportProcessor {
public:
    explicit CImportProcessor(CImportOptions options);

    // Scans preprocessed Zig source for @cImport({ ... }) expressions, generates synthetic C headers,
    // invokes `zig translate-c` (and `zig cc -M` for header dependencies), and rewrites @cImport(...)
    // into @import(".../cimport_<hash>.zig") while preserving exact line counts.
    CImportResult process(std::string_view zig_source, std::string_view filename = "<input>");

    // Writes a Ninja/Make compatible .d dependency file.
    static void write_depfile(const std::filesystem::path& depfile_path,
                              const std::filesystem::path& target_output,
                              const std::vector<std::filesystem::path>& dependencies);

    // Parses a Makefile-format .d file into a list of dependency paths.
    static std::vector<std::filesystem::path> parse_depfile(const std::filesystem::path& depfile_path);

private:
    CImportOptions options_;

    std::string generate_c_header_from_tokens(std::string_view block_source,
                                              std::string_view filename,
                                              std::size_t start_line) const;

    std::filesystem::path translate_c_header(const std::string& header_content,
                                             std::vector<std::filesystem::path>& out_deps,
                                             std::string_view filename,
                                             std::size_t line) const;
};

} // namespace zpp
