#include "lexer/lexer.hpp"
#include "diagnostics/diagnostics.hpp"

namespace folio {
    Lexer::Lexer(std::string source, DiagnosticsEngine& engine)
        : source(std::move(source)), engine(engine) {}

    char Lexer::peek(std::size_t offset) const
    {
        std::size_t i = position + offset;
        if (i >= source.size()) return '\0';
        return source[i];
    }

    char Lexer::advance()
    {
        return source[position++];
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
                const std::size_t commentStart = position;
                advance(); advance();
                while (!isAtEnd() && !(peek() == '*' && peek(1) == '/')) advance();
                if (isAtEnd()) {
                    // Point at the opening '/*' only: the comment runs to end of file, and
                    // an underline that long would be noise.
                    engine.error(SourceSpan{ commentStart, commentStart + 2 }, "unterminated block comment");
                }
                else {
                    advance(); advance();
                }
            }
            else {
                break;
            }
        }
    }

    Token Lexer::makeToken(TokenKind kind, std::string text, std::size_t start, std::size_t end)
    {
        return Token{ kind, std::move(text), SourceSpan{start, end} };
    }

    Token Lexer::isNumber()
    {
        std::size_t start = position;
        std::string text;
        while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            text += advance(); // consume '.'
            while (std::isdigit(static_cast<unsigned char>(peek()))) text += advance();
        }
        return makeToken(TokenKind::Number, text, start, position);
    }

    Token Lexer::isIdentOrKeyword()
    {
        std::size_t start = position;
        std::string text;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') text += advance();
        // keywords (page, rect, fill, etc.) are NOT special-cased here.
        // The parser recognizes them by comparing Ident text. this keeps the
        // lexer dumb and the keyword list easy to extend later.
        return makeToken(TokenKind::Ident, text, start, position);
    }

    Token Lexer::isString()
    {
        std::size_t start = position;
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
            engine.error(SourceSpan{ start, position }, "unterminated string literal");
            return makeToken(TokenKind::Invalid, text, start, position);
        }
        advance(); // consume closing quote
        return makeToken(TokenKind::String, text, start, position);
    }

    Token Lexer::isHexColor()
    {
        std::size_t start = position;
        std::string text;
        text += advance(); // consume '#'
        while (std::isxdigit(static_cast<unsigned char>(peek()))) text += advance();

        // HEXCOLOR := '#' ([0-9a-fA-F]{3,4} | [0-9a-fA-F]{6,8}) — 3/4 are the
        // #RGB / #RGBA shorthand. Any other digit count is lexically invalid.
        std::size_t digitCount = text.size() - 1; // exclude the leading '#'
        if (digitCount != 3 && digitCount != 4 && digitCount != 6 && digitCount != 8) {
            engine.error(SourceSpan{ start, position }, "hex color must have 3, 4, 6, or 8 digits");
            return makeToken(TokenKind::Invalid, text, start, position);
        }
        return makeToken(TokenKind::HexColor, text, start, position);
    }

    std::vector<Token> Lexer::tokenize()
    {
        std::vector<Token> tokens;

        for (;;) {
            skipWhitespaceAndComments();
            if (isAtEnd()) break;

            std::size_t start = position;
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
                case '{': tokens.push_back(makeToken(TokenKind::LBrace, "{", start, position)); break;
                case '}': tokens.push_back(makeToken(TokenKind::RBrace, "}", start, position)); break;
                case '(': tokens.push_back(makeToken(TokenKind::LParen, "(", start, position)); break;
                case ')': tokens.push_back(makeToken(TokenKind::RParen, ")", start, position)); break;
                case '[': tokens.push_back(makeToken(TokenKind::LBracket, "[", start, position)); break;
                case ']': tokens.push_back(makeToken(TokenKind::RBracket, "]", start, position)); break;
                case ':': tokens.push_back(makeToken(TokenKind::Colon, ":", start, position)); break;
                case ',': tokens.push_back(makeToken(TokenKind::Comma, ",", start, position)); break;
                case '.': tokens.push_back(makeToken(TokenKind::Dot, ".", start, position)); break;
                case ';': tokens.push_back(makeToken(TokenKind::Semicolon, ";", start, position)); break;
                case '+': tokens.push_back(makeToken(TokenKind::Plus, "+", start, position)); break;
                case '*': tokens.push_back(makeToken(TokenKind::Star, "*", start, position)); break;
                case '/': tokens.push_back(makeToken(TokenKind::Slash, "/", start, position)); break;
                case '%': tokens.push_back(makeToken(TokenKind::Percent, "%", start, position)); break;
                case '?': tokens.push_back(makeToken(TokenKind::Question, "?", start, position)); break;
                case '@': tokens.push_back(makeToken(TokenKind::At, "@", start, position)); break;
                
                case '!':
                    tokens.push_back(match('=')
                        ? makeToken(TokenKind::NotEq, "!=", start, position)
                        : makeToken(TokenKind::Bang, "!", start, position));
                    break;
                case '=':
                    if (match('=')) tokens.push_back(makeToken(TokenKind::EqEq, "==", start, position));
                    else if (match('>')) tokens.push_back(makeToken(TokenKind::FatArrow, "=>", start, position));
                    else tokens.push_back(makeToken(TokenKind::Eq, "=", start, position));
                    break;
                case '<':
                    tokens.push_back(match('=')
                        ? makeToken(TokenKind::Le, "<=", start, position)
                        : makeToken(TokenKind::Lt, "<", start, position));
                    break;
                case '>':
                    tokens.push_back(match('=')
                        ? makeToken(TokenKind::Ge, ">=", start, position)
                        : makeToken(TokenKind::Gt, ">", start, position));
                    break;
                case '-':
                    tokens.push_back(match('>')
                        ? makeToken(TokenKind::ThinArrow, "->", start, position)
                        : makeToken(TokenKind::Minus, "-", start, position));
                    break;
                case '&':
                    if (match('&')) {
                        tokens.push_back(makeToken(TokenKind::AndAnd, "&&", start, position));
                    } else {
                        engine.error(SourceSpan{start, position}, "expected '&&', found '&'");
                        tokens.push_back(makeToken(TokenKind::Invalid, "&", start, position));
                    }
                    break;
                case '|':
                    if (match('|')) {
                        tokens.push_back(makeToken(TokenKind::OrOr, "||", start, position));
                    } else {
                        engine.error(SourceSpan{start, position}, "expected '||', found '|'");
                        tokens.push_back(makeToken(TokenKind::Invalid, "|", start, position));
                    }
                    break;
                default:
                    engine.error(SourceSpan{start, position}, "unexpected character '" + std::string(1, c) + "'");
                    tokens.push_back(makeToken(TokenKind::Invalid, std::string(1, c), start, position));
                    break;
            }
        }

        tokens.push_back(makeToken(TokenKind::End, "", position, position));
        return tokens;
    }
}