#include "cimport.hpp"
#include "expr_eval.hpp"
#include "lexer.hpp"
#include "preprocessor.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool cond, const char* expr, int line) {
    if (!cond) {
        throw std::runtime_error(std::string("Test assertion failed at line ") +
                                 std::to_string(line) + ": " + expr);
    }
}

#define ZPP_CHECK(cond) require((cond), #cond, __LINE__)

void test_lexer_and_logical_lines() {
    std::string src =
        "#define MULTI_LINE(a, b) \\\n"
        "    ((a) + (b))\n"
        "const s = \\\\hello\\\n"
        "\\\\world\n"
        "const x = MULTI_LINE(1, 2);\n";

    auto lines = zpp::Lexer::split_logical_lines(src);
    ZPP_CHECK(lines.size() == 4);
    ZPP_CHECK(lines[0].is_directive);
    ZPP_CHECK(lines[0].directive_name == "define");
    ZPP_CHECK(lines[0].physical_line_count == 2);
    // Zig multiline raw string ending in '\' must NOT be merged with next line
    ZPP_CHECK(!lines[1].is_directive);
    ZPP_CHECK(lines[1].physical_line_count == 1);
    ZPP_CHECK(!lines[2].is_directive);
    ZPP_CHECK(lines[2].physical_line_count == 1);

    zpp::Preprocessor pp;
    auto res = pp.process_string(src, "test.zig");
    // Count physical lines in output (must match 5 input physical lines)
    std::size_t out_lines = 0;
    for (char c : res.output) {
        if (c == '\n') ++out_lines;
    }
    ZPP_CHECK(out_lines == 5);
    ZPP_CHECK(res.output.find("const x = ((1) + (2));") != std::string::npos);
}

void test_expr_evaluator() {
    zpp::Preprocessor pp;
    pp.define_object_macro("VER_MAJOR", "2");
    pp.define_object_macro("VER_MINOR", "5");
    pp.define_object_macro("OS_NAME", "\"linux\"");

    std::string src =
        "#if defined(VER_MAJOR) && (VER_MAJOR * 10 + VER_MINOR >= 25) && (0x10 == 16) && (0b101 == 5)\n"
        "const branch_a = true;\n"
        "#else\n"
        "const branch_a = false;\n"
        "#endif\n"
        "#if OS_NAME == \"linux\" && !defined(NON_EXISTENT) && (1 ? 42 : 0) == 42\n"
        "const branch_b = true;\n"
        "#endif\n";

    auto res = pp.process_string(src, "expr.zig");
    ZPP_CHECK(res.output.find("const branch_a = true;") != std::string::npos);
    ZPP_CHECK(res.output.find("const branch_a = false;") == std::string::npos);
    ZPP_CHECK(res.output.find("const branch_b = true;") != std::string::npos);
}

void test_macros_stringify_paste_variadic() {
    std::string src =
        "#define STR(x) #x\n"
        "#define PASTE(a, b) a ## _ ## b\n"
        "#define LOG_TUPLE(head, ...) .{ head __VA_OPT__(, __VA_ARGS__) }\n"
        "#define GNU_VA(head, ...) .{ head, ## __VA_ARGS__ }\n"
        "const s = STR(  hello   world  );\n"
        "const PASTE(my, var): i32 = 99;\n"
        "const t1 = LOG_TUPLE(1);\n"
        "const t2 = LOG_TUPLE(1, 2, 3);\n"
        "const g1 = GNU_VA(10);\n"
        "const g2 = GNU_VA(10, 20);\n";

    zpp::Preprocessor pp;
    auto res = pp.process_string(src, "macros.zig");
    ZPP_CHECK(res.output.find("const s = \"hello world\";") != std::string::npos);
    ZPP_CHECK(res.output.find("const my_var: i32 = 99;") != std::string::npos);
    ZPP_CHECK(res.output.find("const t1 = .{ 1  };") != std::string::npos);
    ZPP_CHECK(res.output.find("const t2 = .{ 1 , 2, 3 };") != std::string::npos);
    ZPP_CHECK(res.output.find("const g1 = .{ 10 };") != std::string::npos);
    ZPP_CHECK(res.output.find("const g2 = .{ 10, 20 };") != std::string::npos);
}

void test_multiline_macro_invocation_line_preservation() {
    std::string src =
        "#define ADD3(a, b, c) ((a) + (b) + (c))\n"
        "const x = ADD3(\n"
        "    10,\n"
        "    20,\n"
        "    30\n"
        ");\n"
        "const line_seven = __LINE__;\n";

    zpp::Preprocessor pp;
    auto res = pp.process_string(src, "multiline.zig");
    ZPP_CHECK(res.output.find("const line_seven = 7;") != std::string::npos);
}

void test_cimport_and_depfile() {
    auto tmp_dir = std::filesystem::temp_directory_path() / "zpp_unit_test_cimport";
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);

    auto hdr_path = tmp_dir / "sample.h";
    {
        std::ofstream ofs(hdr_path);
        ofs << "#pragma once\n"
            << "#define SAMPLE_CONST 777\n"
            << "int sample_fn(int x);\n";
    }

    std::string zig_src =
        "const c = @cImport({\n"
        "    @cDefine(\"CUSTOM_DEF\", \"123\");\n"
        "    @cInclude(\"sample.h\");\n"
        "});\n"
        "pub fn main() void {}\n";

    zpp::CImportOptions opts;
    opts.zig_executable = "zig";
    opts.cimport_dir = tmp_dir / "shadow" / "__zpp_cimports";
    opts.source_file_dir = tmp_dir;
    opts.output_file_path = tmp_dir / "shadow" / "main.zig";

    zpp::CImportProcessor proc(opts);
    auto res = proc.process(zig_src, (tmp_dir / "main.zig").string());

    ZPP_CHECK(res.transformed_source.find("@import(\"__zpp_cimports/cimport_") != std::string::npos);
    ZPP_CHECK(res.generated_zig_files.size() == 1);
    ZPP_CHECK(std::filesystem::exists(res.generated_zig_files[0]));

    // Verify dependency discovery found sample.h
    bool found_sample_h = false;
    for (const auto& d : res.c_header_dependencies) {
        if (d.filename() == "sample.h") {
            found_sample_h = true;
            break;
        }
    }
    ZPP_CHECK(found_sample_h);

    // Verify exact line count preservation across @cImport replacement
    std::size_t orig_newlines = 0;
    for (char c : zig_src) {
        if (c == '\n') ++orig_newlines;
    }
    std::size_t new_newlines = 0;
    for (char c : res.transformed_source) {
        if (c == '\n') ++new_newlines;
    }
    ZPP_CHECK(orig_newlines == new_newlines);

    std::filesystem::remove_all(tmp_dir);
}

} // namespace

int main() {
    test_lexer_and_logical_lines();
    test_expr_evaluator();
    test_macros_stringify_paste_variadic();
    test_multiline_macro_invocation_line_preservation();
    test_cimport_and_depfile();
    std::cout << "All zpp unit tests passed!\n";
    return 0;
}
