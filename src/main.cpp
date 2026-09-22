#include "ast/ast.hpp"
#include "ast/ast_printer.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include "lexer/lexer.hpp"
#include "lexer/token.hpp"
#include "parser/parser.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// folioc: the Folio compiler CLI.
//
// Pipeline so far: source -> lexer -> tokens -> parser -> AST.
// Semantic analysis, the scene graph, and the exporters don't exist yet, so
// for now a successful run prints a one-line summary, or with --ast the full
// canonical dump of the parsed tree.

namespace {

    constexpr std::string_view kUsage =
        "usage: folioc [--tokens] [--ast] <file.folio>\n"
        "\n"
        "  --tokens    also print the token stream before parsing\n"
        "  --ast       print the parsed AST (canonical dump) instead of the one-line summary\n"
        "  -h, --help  show this message\n";

    struct Options {
        std::string path;
        bool dumpTokens = false;
        bool dumpAst = false;
        bool showHelp = false;
    };

    // Returns false (after printing why) if the command line is malformed.
    bool parseArgs(int argc, char** argv, Options& options) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];

            if (arg == "--tokens") {
                options.dumpTokens = true;
            }
            else if (arg == "--ast") {
                options.dumpAst = true;
            }
            else if (arg == "-h" || arg == "--help") {
                options.showHelp = true;
            }
            else if (arg.size() > 1 && arg[0] == '-') {
                std::cerr << "folioc: unknown option '" << arg << "'\n";
                return false;
            }
            else if (!options.path.empty()) {
                std::cerr << "folioc: more than one input file given ('"
                    << options.path << "' and '" << arg << "')\n";
                return false;
            }
            else {
                options.path = std::string(arg);
            }
        }
        return true;
    }

    // Reads the whole file at `path` into a string. Returns false (and
    // leaves `out` untouched) if the file couldn't be opened.
    bool readFile(const std::string& path, std::string& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;

        std::ostringstream ss;
        ss << file.rdbuf();
        out = ss.str();
        return true;
    }

    void printTokens(const std::vector<folio::Token>& tokens) {
        for (const auto& token : tokens) {
            std::cout << "  " << folio::tokenKindName(token.kind);

            // End-of-input has no meaningful lexeme; skip the quotes for it.
            if (token.kind != folio::TokenKind::End) {
                std::cout << " '" << token.lexeme << "'";
            }

            std::cout << "  [" << token.span.start << ", " << token.span.end << ")\n";
        }
    }

    std::string describePageSize(const folio::PageDecl& page) {
        if (!page.size) return "no size";
        if (page.size->preset) return "size " + *page.size->preset;
        return "custom size";
    }

    void printSummary(const std::string& path, const folio::Document& document) {
        std::cout << path << ": page (" << describePageSize(document.page) << "), "
            << document.nodes.size() << " top-level node"
            << (document.nodes.size() == 1 ? "" : "s") << '\n';
    }

    // "2 errors, 1 warning" (omits zero counts)
    std::string countDiagnostics(const folio::DiagnosticsEngine& engine) {
        std::size_t errors = 0, warnings = 0;
        for (const auto& diag : engine.diagnostics()) {
            if (diag.severity == folio::Diagnostic::Severity::Error) ++errors;
            else if (diag.severity == folio::Diagnostic::Severity::Warning) ++warnings;
        }

        std::string result;
        if (errors > 0) {
            result += std::to_string(errors) + (errors == 1 ? " error" : " errors");
        }
        if (warnings > 0) {
            if (!result.empty()) result += ", ";
            result += std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
        }
        return result;
    }

}

int main(int argc, char** argv) {
    Options options;
    if (!parseArgs(argc, argv, options)) {
        std::cerr << kUsage;
        return 1;
    }
    if (options.showHelp) {
        std::cout << kUsage;
        return 0;
    }
    if (options.path.empty()) {
        std::cerr << "folioc: no input file\n" << kUsage;
        return 1;
    }

    std::string source;
    if (!readFile(options.path, source)) {
        std::cerr << "folioc: could not open file '" << options.path << "'\n";
        return 1;
    }

    // One engine for the whole run: lexer and parser report into it, so all
    // diagnostics come out together, in the order they were found.
    folio::DiagnosticsEngine engine;

    folio::Lexer lexer(source, engine);
    std::vector<folio::Token> tokens = lexer.tokenize();

    if (options.dumpTokens) {
        std::cout << options.path << ": " << tokens.size() << " tokens\n";
        printTokens(tokens);
        std::cout << '\n';
    }

    // The parser copes with lexer errors (Invalid tokens) and recovers, so run
    // it even if lexing reported problems: the person gets every error in one go.
    folio::Parser parser(std::move(tokens), engine);
    folio::Document document = parser.parse();

    if (!engine.diagnostics().empty()) {
        engine.printAll(std::cerr, source, options.path);
    }

    if (engine.hasErrors()) {
        std::cerr << "folioc: " << countDiagnostics(engine) << " in '" << options.path << "'\n";
        return 1;
    }

    if (options.dumpAst) {
        folio::printAst(std::cout, document);
    }
    else {
        printSummary(options.path, document);
    }
    return 0;
}