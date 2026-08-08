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
        Colon, Comma, Dot, Semicolon,
        ThinArrow,            // ->
        FatArrow,             // =>  (unused by the grammar today; kept distinct from ThinArrow so the parser isn't blocked if/when it's needed later)

        // operators
        Plus, Minus, Star, Slash, Percent,
        Lt, Gt, Le, Ge, Eq, EqEq, NotEq,
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