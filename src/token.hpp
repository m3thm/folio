#pragma once

#include <string>
#include <cstdint>

namespace folio {

    // Token kinds
    enum class TokenKind {
        Ident,
        Number,
        String,
        HexColor,

        // punctuation
        LBrace, RBrace,     // { }
        LParen, RParen,     // ( )
        LBracket, RBracket, // [ ]
        Colon, Comma, Dot,
        Arrow,               // ->

        // operators
        Plus, Minus, Star, Slash, Percent,
        Lt, Gt, Le, Ge, EqEq, NotEq,
        AndAnd, OrOr, Bang, Question,

        End,     // end of input
        Invalid  // lexer error marker
    };

    struct Token {
        TokenKind kind;
        std::string lexeme;
        std::size_t line;
        std::size_t column;
    };

} 
