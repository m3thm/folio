#include <iostream>

// TODO once lexer/parser exist:
//   1. read argv[1] as a .folio source file
//   2. Lexer lexer(source); auto tokens = lexer.tokenize();
//   3. Parser parser(tokens); auto document = parser.parse();
//   4. do something with the parsed Document (print it, later feed a
//      scene-graph builder)

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "folioc: no input file. usage: folioc <file.folio>\n";
        return 0;
    }

    std::cout << "folioc received: " << argv[1] << "\n";
    std::cout << "(lexer/parser not implemented yet)\n";
    return 0;
}
