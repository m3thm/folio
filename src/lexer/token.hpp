#pragma once

#include <string>
#include <cstdint>
#include "diagnostics/diagnostics.hpp"

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
        At,

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
        SourceSpan span;
    };

    inline const char* tokenKindName(TokenKind kind) {
        switch (kind) {
            case TokenKind::Ident: return "Ident";
            case TokenKind::Number: return "Number";
            case TokenKind::String: return "String";
            case TokenKind::HexColor: return "HexColor";
            case TokenKind::LBrace: return "LBrace";
            case TokenKind::RBrace: return "RBrace";
            case TokenKind::LParen: return "LParen";
            case TokenKind::RParen: return "RParen";
            case TokenKind::LBracket: return "LBracket";
            case TokenKind::RBracket: return "RBracket";
            case TokenKind::Colon: return "Colon";
            case TokenKind::Comma: return "Comma";
            case TokenKind::Dot: return "Dot";
            case TokenKind::Semicolon: return "Semicolon";
            case TokenKind::ThinArrow: return "ThinArrow";
            case TokenKind::FatArrow: return "FatArrow";
            case TokenKind::Plus: return "Plus";
            case TokenKind::Minus: return "Minus";
            case TokenKind::Star: return "Star";
            case TokenKind::Slash: return "Slash";
            case TokenKind::Percent: return "Percent";
            case TokenKind::Lt: return "Lt";
            case TokenKind::Gt: return "Gt";
            case TokenKind::Le: return "Le";
            case TokenKind::Ge: return "Ge";
            case TokenKind::Eq: return "Eq";
            case TokenKind::EqEq: return "EqEq";
            case TokenKind::NotEq: return "NotEq";
            case TokenKind::AndAnd: return "AndAnd";
            case TokenKind::OrOr: return "OrOr";
            case TokenKind::Bang: return "Bang";
            case TokenKind::Question: return "Question";
            case TokenKind::End: return "End";
            case TokenKind::Invalid: return "Invalid";
        }
        return "Unknown";
    }
}