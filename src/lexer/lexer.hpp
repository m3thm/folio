#pragma once

#include "token.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include <string>
#include <vector>


namespace folio {

    class Lexer {
    public:
        explicit Lexer(std::string source, DiagnosticsEngine& engine);

        std::vector<Token> tokenize();

    private:
        std::string source;
        DiagnosticsEngine& engine;
        std::size_t position = 0;

        char peek(std::size_t offset = 0) const;
        char advance();
        bool match(char expected);
        bool isAtEnd() const;

        void skipWhitespaceAndComments();

        Token makeToken(TokenKind kind, std::string text, std::size_t start, std::size_t end);
        Token isNumber();
        Token isIdentOrKeyword();
        Token isString();
        Token isHexColor();
    };

} 
