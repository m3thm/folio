#pragma once

#include "token.hpp"
#include <string>
#include <vector>


namespace folio {

    class Lexer {
        public:
            explicit Lexer(std::string source);

            std::vector<Token> tokenize();

        private:
            std::string source;
            std::size_t position = 0;
            std::size_t line = 1;
            std::size_t column = 1;

            char peek(std::size_t offset = 0) const;
            char advance();
            bool match(char expected);
            bool isAtEnd() const;

            void skipWhitespaceAndComments();

            Token makeToken(TokenKind kind, std::string text, std::size_t startLine, std::size_t startCol);
            Token isNumber();
            Token isIdentOrKeyword();
            Token isString();
            Token isHexColor();
    };

} 
