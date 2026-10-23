#include "cimport.hpp"
#include "lexer.hpp"
#include "preprocessor.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage(std::ostream& os, std::string_view prog) {
    os << "Usage: " << prog << " [options] <input.zig>\n\n"
       << "Standalone Zig Preprocessor (C-style #directives, macros, and @cImport lowering)\n\n"
       << "Options:\n"
       << "  -o, --output <file>           Write preprocessed Zig output to <file> (default: stdout)\n"
       << "  -I, --include-dir <dir>       Add directory to #include and @cInclude search path\n"
       << "  -D<macro>[=<val>]             Define macro (passed to both preprocessor and @cImport)\n"
       << "  -U<macro>                     Undefine macro\n"
       << "  --cimport-dir <dir>           Directory for generated @cImport files\n"
       << "  --depfile <file.d>            Write Ninja/Make dependency file\n"
       << "  --output-dep-file <file.d>    Alias for --depfile (Qt moc compatible)\n"
       << "  --zig, --zig-executable <exe> Path to zig executable (default: zig)\n"
       << "  --target <triple>             Target triple for zig translate-c\n"
       << "  --no-libc                     Do not pass -lc to zig translate-c\n"
       << "  --no-cimport                  Disable @cImport lowering (macros/directives only)\n"
       << "  --cflag <flag>                Extra C compiler flag for zig translate-c\n"
       << "  @<file>                       Read command-line arguments from response file\n"
       << "  -v, --version                 Print version\n"
       << "  -h, --help                    Print this help message\n";
}

std::vector<std::string> split_cmake_list(std::string_view arg) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= arg.size()) {
        std::size_t semi = arg.find(';', start);
        std::string_view piece = (semi == std::string_view::npos)
                                     ? arg.substr(start)
                                     : arg.substr(start, semi - start);
        if (!piece.empty()) {
            items.emplace_back(piece);
        }
        if (semi == std::string_view::npos) {
            break;
        }
        start = semi + 1;
    }
    return items;
}

void parse_define_item(std::string_view def_str,
                       std::vector<std::pair<std::string, std::string>>& out_defines) {
    if (def_str.empty()) {
        return;
    }
    std::size_t eq = def_str.find('=');
    if (eq == std::string_view::npos) {
        out_defines.emplace_back(std::string(def_str), "1");
    } else {
        out_defines.emplace_back(std::string(def_str.substr(0, eq)),
                                 std::string(def_str.substr(eq + 1)));
    }
}

void expand_response_files(int argc, char** argv, std::vector<std::string>& out_args) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.size() > 1 && arg.front() == '@') {
            std::filesystem::path rsp_path(arg.substr(1));
            std::ifstream ifs(rsp_path);
            if (!ifs) {
                throw std::runtime_error("failed to open response file: " + rsp_path.string());
            }
            std::string line;
            while (std::getline(ifs, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    out_args.push_back(line);
                }
            }
        } else {
            out_args.emplace_back(arg);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
        expand_response_files(argc, argv, args);

        zpp::PreprocessOptions pp_opts;
        zpp::CImportOptions cimport_opts;
        std::filesystem::path input_path;
        std::filesystem::path output_path;
        std::filesystem::path depfile_path;
        bool enable_cimport = true;

        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args[i];

            if (arg == "-h" || arg == "--help") {
                print_usage(std::cout, argv[0]);
                return 0;
            }
            if (arg == "-v" || arg == "--version") {
                std::cout << "zpp 0.1.0\n";
                return 0;
            }
            if (arg == "-o" || arg == "--output") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after " + arg);
                }
                output_path = args[++i];
                continue;
            }
            if (arg.starts_with("-o=")) {
                output_path = arg.substr(3);
                continue;
            }
            if (arg == "--depfile" || arg == "--output-dep-file") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after " + arg);
                }
                depfile_path = args[++i];
                continue;
            }
            if (arg.starts_with("--depfile=")) {
                depfile_path = arg.substr(10);
                continue;
            }
            if (arg.starts_with("--output-dep-file=")) {
                depfile_path = arg.substr(18);
                continue;
            }
            if (arg == "--cimport-dir") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after --cimport-dir");
                }
                cimport_opts.cimport_dir = args[++i];
                continue;
            }
            if (arg.starts_with("--cimport-dir=")) {
                cimport_opts.cimport_dir = arg.substr(14);
                continue;
            }
            if (arg == "--zig" || arg == "--zig-executable") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after " + arg);
                }
                cimport_opts.zig_executable = args[++i];
                continue;
            }
            if (arg.starts_with("--zig=")) {
                cimport_opts.zig_executable = arg.substr(6);
                continue;
            }
            if (arg.starts_with("--zig-executable=")) {
                cimport_opts.zig_executable = arg.substr(17);
                continue;
            }
            if (arg == "--target") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after --target");
                }
                cimport_opts.target_triple = args[++i];
                continue;
            }
            if (arg.starts_with("--target=")) {
                cimport_opts.target_triple = arg.substr(9);
                continue;
            }
            if (arg == "--no-libc") {
                cimport_opts.link_libc = false;
                continue;
            }
            if (arg == "--no-cimport") {
                enable_cimport = false;
                continue;
            }
            if (arg == "--cflag") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after --cflag");
                }
                for (const auto& item : split_cmake_list(args[++i])) {
                    cimport_opts.extra_cflags.push_back(item);
                }
                continue;
            }
            if (arg.starts_with("--cflag=")) {
                for (const auto& item : split_cmake_list(arg.substr(8))) {
                    cimport_opts.extra_cflags.push_back(item);
                }
                continue;
            }
            if (arg == "-I" || arg == "--include-dir") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after " + arg);
                }
                for (const auto& dir : split_cmake_list(args[++i])) {
                    pp_opts.include_dirs.emplace_back(dir);
                    cimport_opts.include_dirs.emplace_back(dir);
                }
                continue;
            }
            if (arg.starts_with("--include-dir=")) {
                for (const auto& dir : split_cmake_list( std::string_view(arg).substr(14))) {
                    pp_opts.include_dirs.emplace_back(dir);
                    cimport_opts.include_dirs.emplace_back(dir);
                }
                continue;
            }
            if (arg.starts_with("-I")) {
                for (const auto& dir : split_cmake_list(std::string_view(arg).substr(2))) {
                    pp_opts.include_dirs.emplace_back(dir);
                    cimport_opts.include_dirs.emplace_back(dir);
                }
                continue;
            }
            if (arg == "-D" || arg == "--define") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after " + arg);
                }
                for (const auto& d : split_cmake_list(args[++i])) {
                    parse_define_item(d, pp_opts.cli_defines);
                    parse_define_item(d, cimport_opts.defines);
                }
                continue;
            }
            if (arg.starts_with("--define=")) {
                for (const auto& d : split_cmake_list(std::string_view(arg).substr(9))) {
                    parse_define_item(d, pp_opts.cli_defines);
                    parse_define_item(d, cimport_opts.defines);
                }
                continue;
            }
            if (arg.starts_with("-D")) {
                for (const auto& d : split_cmake_list(std::string_view(arg).substr(2))) {
                    parse_define_item(d, pp_opts.cli_defines);
                    parse_define_item(d, cimport_opts.defines);
                }
                continue;
            }
            if (arg == "-U" || arg == "--undef") {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("missing argument after " + arg);
                }
                for (const auto& u : split_cmake_list(args[++i])) {
                    pp_opts.cli_undefs.push_back(u);
                    cimport_opts.undefs.push_back(u);
                }
                continue;
            }
            if (arg.starts_with("--undef=")) {
                for (const auto& u : split_cmake_list(std::string_view(arg).substr(8))) {
                    pp_opts.cli_undefs.push_back(u);
                    cimport_opts.undefs.push_back(u);
                }
                continue;
            }
            if (arg.starts_with("-U")) {
                for (const auto& u : split_cmake_list(std::string_view(arg).substr(2))) {
                    pp_opts.cli_undefs.push_back(u);
                    cimport_opts.undefs.push_back(u);
                }
                continue;
            }
            if (arg.starts_with("-")) {
                throw std::runtime_error("unrecognized option: " + arg);
            }
            if (!input_path.empty()) {
                throw std::runtime_error("multiple input files specified: '" + input_path.string() +
                                         "' and '" + arg + "'");
            }
            input_path = arg;
        }

        if (input_path.empty()) {
            print_usage(std::cerr, argv[0]);
            return 1;
        }

        // Phase 1: Run C-style directive & macro preprocessor
        zpp::Preprocessor preprocessor(pp_opts);
        zpp::PreprocessResult pp_res = preprocessor.process_file(input_path);
        for (const auto& warn : pp_res.warnings) {
            std::cerr << warn << "\n";
        }

        std::string final_zig_source = std::move(pp_res.output);
        std::vector<std::filesystem::path> all_deps = std::move(pp_res.dependencies);

        // Phase 2: Lower @cImport(...) blocks via zig translate-c
        if (enable_cimport && final_zig_source.find("@cImport") != std::string::npos) {
            cimport_opts.source_file_dir =
                std::filesystem::weakly_canonical(std::filesystem::absolute(input_path)).parent_path();
            cimport_opts.output_file_path = output_path;
            if (cimport_opts.cimport_dir.empty()) {
                if (!output_path.empty()) {
                    cimport_opts.cimport_dir =
                        std::filesystem::absolute(output_path).parent_path() / "__zpp_cimports";
                } else {
                    cimport_opts.cimport_dir =
                        std::filesystem::current_path() / "__zpp_cimports";
                }
            }

            zpp::CImportProcessor cimport_proc(cimport_opts);
            zpp::CImportResult ci_res =
                cimport_proc.process(final_zig_source, input_path.string());
            final_zig_source = std::move(ci_res.transformed_source);
            all_deps.insert(all_deps.end(),
                            ci_res.c_header_dependencies.begin(),
                            ci_res.c_header_dependencies.end());
        }

        // Write output file or stdout
        if (output_path.empty()) {
            std::cout << final_zig_source;
        } else {
            if (output_path.has_parent_path()) {
                std::filesystem::create_directories(output_path.parent_path());
            }
            std::ofstream ofs(output_path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                throw std::runtime_error("failed to open output file for writing: " +
                                         output_path.string());
            }
            ofs << final_zig_source;
        }

        // Write depfile if requested
        if (!depfile_path.empty()) {
            std::filesystem::path target_for_dep =
                output_path.empty() ? std::filesystem::path("<stdout>") : output_path;
            zpp::CImportProcessor::write_depfile(depfile_path, target_for_dep, all_deps);
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "zpp: " << ex.what() << "\n";
        return 1;
    }
}
