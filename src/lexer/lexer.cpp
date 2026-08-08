#include "lexer/lexer.hpp"

namespace folio {
    Lexer::Lexer(std::string source) : source(std::move(source)) {}

    char Lexer::peek(std::size_t offset) const
    {
        std::size_t i = position + offset;
        if (i >= source.size()) return '\0';
        return source[i];
    }

    char Lexer::advance()
    {
        char c = source[position++];
        if (c == '\n') {
            line++;
            column = 1;
        }
        else {
            column++;
        }
        return c;
    }

    bool Lexer::isAtEnd() const
    {
        return position >= source.size();
    }

    bool Lexer::match(char expected)
    {
        if (isAtEnd() || peek() != expected) return false;
        advance();
        return true;
    }

    void Lexer::skipWhitespaceAndComments()
    {
        for (;;) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            }
            else if (c == '/' && peek(1) == '/') {
                while (!isAtEnd() && peek() != '\n') advance();
            }
            else if (c == '/' && peek(1) == '*') {
                advance(); advance();
                while (!isAtEnd() && !(peek() == '*' && peek(1) == '/')) advance();
                if (!isAtEnd()) { advance(); advance(); }
            }
            else {
                break;
            }
        }
    }

    Token Lexer::makeToken(TokenKind kind, std::string text, std::size_t startLine, std::size_t startCol)
    {
        return Token{ kind, std::move(text), startLine, startCol };
    }

    Token Lexer::isNumber()
    {
        std::size_t startLine = line, startCol = column;
        std::string text;
        while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            text += advance(); // consume '.'
            while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
        }
        return makeToken(TokenKind::Number, text, startLine, startCol);
    }

    Token Lexer::isIdentOrKeyword()
    {
        std::size_t startLine = line, startCol = column;
        std::string text;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') text += advance();
        // keywords (page, rect, fill, etc.) are NOT special-cased here.
        // The parser recognizes them by comparing Ident text. this keeps the
        // lexer dumb and the keyword list easy to extend later.
        return makeToken(TokenKind::Ident, text, startLine, startCol);
    }

    Token Lexer::isString()
    {
        std::size_t startLine = line, startCol = column;
        std::string text;
        advance(); // consume opening quote
        while (!isAtEnd() && peek() != '"') {
            char c = advance();
            if (c == '\\' && !isAtEnd()) {
                char esc = advance();
                switch (esc) {
                case 'n': text += '\n'; break;
                case 't': text += '\t'; break;
                case '"': text += '"'; break;
                case '\\': text += '\\'; break;
                default: text += esc; break;
                }
            }
            else {
                text += c;
            }
        }
        if (isAtEnd()) {
            // Hit end-of-input before a closing quote was found. This is a
            // lexical error, not a valid (if oddly-terminated) string — flag
            // it so the parser doesn't mistake it for well-formed input.
            return makeToken(TokenKind::Invalid, text, startLine, startCol);
        }
        advance(); // consume closing quote
        return makeToken(TokenKind::String, text, startLine, startCol);
    }

    Token Lexer::isHexColor()
    {
        std::size_t startLine = line, startCol = column;
        std::string text;
        text += advance(); // consume '#'
        while (std::isxdigit(static_cast<unsigned char>(peek()))) text += advance();

        // HEXCOLOR := '#' [0-9a-fA-F]{6,8} — anything else (too short, too
        // long, or a digit count that isn't 6 or 8) is lexically invalid.
        std::size_t digitCount = text.size() - 1; // exclude the leading '#'
        if (digitCount != 6 && digitCount != 8) {
            return makeToken(TokenKind::Invalid, text, startLine, startCol);
        }
        return makeToken(TokenKind::HexColor, text, startLine, startCol);
    }

    std::vector<Token> Lexer::tokenize()
    {
        std::vector<Token> tokens;

        for (;;) {
            skipWhitespaceAndComments();
            if (isAtEnd()) break;

            std::size_t startLine = line, startCol = column;
            char c = peek();

            if (std::isdigit(static_cast<unsigned char>(c))) {
                tokens.push_back(isNumber());
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                tokens.push_back(isIdentOrKeyword());
                continue;
            }
            if (c == '"') {
                tokens.push_back(isString());
                continue;
            }
            if (c == '#') {
                tokens.push_back(isHexColor());
                continue;
            }

            advance(); // consume the punctuation/operator char
            switch (c) {
            case '{': tokens.push_back(makeToken(TokenKind::LBrace, "{", startLine, startCol)); break;
            case '}': tokens.push_back(makeToken(TokenKind::RBrace, "}", startLine, startCol)); break;
            case '(': tokens.push_back(makeToken(TokenKind::LParen, "(", startLine, startCol)); break;
            case ')': tokens.push_back(makeToken(TokenKind::RParen, ")", startLine, startCol)); break;
            case '[': tokens.push_back(makeToken(TokenKind::LBracket, "[", startLine, startCol)); break;
            case ']': tokens.push_back(makeToken(TokenKind::RBracket, "]", startLine, startCol)); break;
            case ':': tokens.push_back(makeToken(TokenKind::Colon, ":", startLine, startCol)); break;
            case ',': tokens.push_back(makeToken(TokenKind::Comma, ",", startLine, startCol)); break;
            case '.': tokens.push_back(makeToken(TokenKind::Dot, ".", startLine, startCol)); break;
            case ';': tokens.push_back(makeToken(TokenKind::Semicolon, ";", startLine, startCol)); break;
            case '+': tokens.push_back(makeToken(TokenKind::Plus, "+", startLine, startCol)); break;
            case '*': tokens.push_back(makeToken(TokenKind::Star, "*", startLine, startCol)); break;
            case '/': tokens.push_back(makeToken(TokenKind::Slash, "/", startLine, startCol)); break;
            case '%': tokens.push_back(makeToken(TokenKind::Percent, "%", startLine, startCol)); break;
            case '?': tokens.push_back(makeToken(TokenKind::Question, "?", startLine, startCol)); break;
            case '!':
                tokens.push_back(match('=')
                    ? makeToken(TokenKind::NotEq, "!=", startLine, startCol)
                    : makeToken(TokenKind::Bang, "!", startLine, startCol));
                break;
            case '=':
                if (match('=')) tokens.push_back(makeToken(TokenKind::EqEq, "==", startLine, startCol));
                else if (match('>')) tokens.push_back(makeToken(TokenKind::FatArrow, "=>", startLine, startCol));
                else tokens.push_back(makeToken(TokenKind::Eq, "=", startLine, startCol));
                break;
            case '<':
                tokens.push_back(match('=')
                    ? makeToken(TokenKind::Le, "<=", startLine, startCol)
                    : makeToken(TokenKind::Lt, "<", startLine, startCol));
                break;
            case '>':
                tokens.push_back(match('=')
                    ? makeToken(TokenKind::Ge, ">=", startLine, startCol)
                    : makeToken(TokenKind::Gt, ">", startLine, startCol));
                break;
            case '-':
                tokens.push_back(match('>')
                    ? makeToken(TokenKind::ThinArrow, "->", startLine, startCol)
                    : makeToken(TokenKind::Minus, "-", startLine, startCol));
                break;
            case '&':
                tokens.push_back(match('&')
                    ? makeToken(TokenKind::AndAnd, "&&", startLine, startCol)
                    : makeToken(TokenKind::Invalid, "&", startLine, startCol));
                break;
            case '|':
                tokens.push_back(match('|')
                    ? makeToken(TokenKind::OrOr, "||", startLine, startCol)
                    : makeToken(TokenKind::Invalid, "|", startLine, startCol));
                break;
            default:
                tokens.push_back(makeToken(TokenKind::Invalid, std::string(1, c), startLine, startCol));
                break;
            }
        }

        tokens.push_back(makeToken(TokenKind::End, "", line, column));
        return tokens;
    }
}