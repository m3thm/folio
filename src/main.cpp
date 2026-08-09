#include "lexer/lexer.hpp"
#include "lexer/token.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

// TODO once the parser exists:
//   3. Parser parser(tokens); auto document = parser.parse();
//   4. do something with the parsed Document (print it, later feed a
//      scene-graph builder)

namespace {

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

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "folioc: no input file. usage: folioc <file.folio>\n";
        return 0;
    }

    const std::string path = argv[1];
    std::string source;
    if (!readFile(path, source)) {
        std::cerr << "folioc: could not open file '" << path << "'\n";
        return 1;
    }

    folio::DiagnosticsEngine engine;
    folio::Lexer lexer(source, engine);
    std::vector<folio::Token> tokens = lexer.tokenize();

    std::cout << "folioc: " << tokens.size() << " tokens from '" << path << "'\n";
    printTokens(tokens);

    if (engine.hasErrors()) {
        std::cout << "\n";
        engine.printAll(std::cout, source);
        return 1;
    }

    std::cout << "\nno diagnostics.\n";
    return 0;
}