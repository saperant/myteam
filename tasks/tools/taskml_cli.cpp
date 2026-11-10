// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
//
// taskml - command line front-end for the taskml library.

#include <taskml/taskml.hpp>

#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <iterator>
#include <sstream>
#include <string>

namespace {

void usage(std::ostream& os) {
    os << "usage: taskml <command> [--markdown|--taskml] [file]\n"
          "\n"
          "Reads from stdin when no file (or '-') is given. Files ending in .md or\n"
          ".markdown are read and written as Markdown plans, others as TaskML;\n"
          "--markdown / --taskml override that (and set it for stdin).\n"
          "\n"
          "commands:\n"
          "  check       validate a plan (strict + semantic checks)\n"
          "  json        convert a plan to JSON\n"
          "  fmt         re-emit a plan in canonical form, in its own dialect\n"
          "  md          convert a plan to Markdown\n"
          "  taskml      convert a plan to TaskML\n"
          "  from-json   convert JSON to a plan (TaskML unless --markdown)\n"
          "  version     print library and spec versions\n";
}

bool read_input(const char* path, std::string& out) {
    if (!path || std::strcmp(path, "-") == 0) {
        out.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "taskml: cannot open " << path << "\n";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

int report(const std::vector<taskml::Diagnostic>& diags, const std::string& src) {
    int errors = 0;
    for (const auto& d : diags) {
        std::cerr << d.format(src) << "\n";
        if (d.severity == taskml::Severity::Error) ++errors;
    }
    return errors;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(std::cerr);
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage(std::cout);
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::cout << "taskml " << taskml::library_version() << " (TaskML spec "
                  << taskml::spec_version() << ")\n";
        return 0;
    }

    const char* path = nullptr;
    std::optional<taskml::Dialect> forced;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--markdown") forced = taskml::Dialect::Markdown;
        else if (a == "--taskml") forced = taskml::Dialect::TaskML;
        else if (!path) path = argv[i];
        else {
            usage(std::cerr);
            return 2;
        }
    }
    const taskml::Dialect dialect =
        forced ? *forced : (path ? taskml::dialect_for_path(path) : taskml::Dialect::TaskML);
    taskml::ParseOptions popts;
    popts.dialect = dialect;
    auto emit = [](const taskml::Document& d, taskml::Dialect out) {
        taskml::SerializeOptions so;
        so.dialect = out;
        return taskml::to_taskml(d, so);
    };

    std::string input;
    if (!read_input(path, input)) return 2;

    try {
        if (cmd == "check") {
            auto diags = taskml::validate(input, dialect);
            int errors = report(diags, input);
            if (diags.empty()) std::cerr << "ok\n";
            return errors ? 1 : 0;
        }
        if (cmd == "json" || cmd == "fmt" || cmd == "md" || cmd == "taskml") {
            auto result = taskml::parse(input, popts);
            report(result.errors, input);
            report(result.warnings, input);
            if (cmd == "json")
                std::cout << taskml::to_json(result.document) << "\n";
            else if (cmd == "fmt")
                std::cout << emit(result.document, dialect);
            else
                std::cout << emit(result.document,
                                  cmd == "md" ? taskml::Dialect::Markdown : taskml::Dialect::TaskML);
            return result.ok() ? 0 : 1;
        }
        if (cmd == "from-json") {
            std::cout << emit(taskml::from_json(input), forced.value_or(taskml::Dialect::TaskML));
            return 0;
        }
    } catch (const taskml::Error& e) {
        std::cerr << "taskml: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "taskml: unknown command '" << cmd << "'\n";
    usage(std::cerr);
    return 2;
}
