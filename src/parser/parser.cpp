#include "parser/parser.hpp"
#include <string_view>
#include <utility>

// Recursive-descent parser implementation.

namespace {

    using namespace folio;

    bool isNodeTypeKeyword(const std::string& text) {
        return text == "rect" || text == "circle" || text == "ellipse" ||
            text == "path" || text == "text" || text == "image" || text == "group";
    }

    NodeType nodeTypeFromKeyword(const std::string& text) {
        if (text == "rect")    return NodeType::Rect;
        if (text == "circle")  return NodeType::Circle;
        if (text == "ellipse") return NodeType::Ellipse;
        if (text == "path")    return NodeType::Path;
        if (text == "text")    return NodeType::Text;
        if (text == "image")   return NodeType::Image;
        return NodeType::Group; // only reached when isNodeTypeKeyword(text) was already true.
    }

    // Unit suffix recognized directly after a number (px/pt/mm/cm/in).
    Unit unitFromText(const std::string& text) {
        if (text == "px") return Unit::Px;
        if (text == "pt") return Unit::Pt;
        if (text == "mm") return Unit::Mm;
        if (text == "cm") return Unit::Cm;
        if (text == "in") return Unit::In;
        return Unit::None;
    }

}

namespace folio {

    Parser::Parser(std::vector<Token> tokens, DiagnosticsEngine& engine)
        : tokens(std::move(tokens)), engine(engine) {}

    // token stream helpers

    const Token& Parser::peek(std::size_t offset) const {
        std::size_t i = position + offset;
        if (i >= tokens.size()) return tokens.back(); // tokens always ends with a TokenKind::End.
        return tokens[i];
    }

    const Token& Parser::previous() const {
        return tokens[position - 1];
    }

    const Token& Parser::advance() {
        if (!isAtEnd()) position++;
        return previous();
    }

    bool Parser::check(TokenKind kind) const {
        return peek().kind == kind;
    }

    bool Parser::checkKeyword(std::string_view keyword) const {
        return peek().kind == TokenKind::Ident && peek().lexeme == keyword;
    }

    bool Parser::match(TokenKind kind) {
        if (!check(kind)) return false;
        advance();
        return true;
    }

    bool Parser::matchKeyword(std::string_view keyword) {
        if (!checkKeyword(keyword)) return false;
        advance();
        return true;
    }

    const Token& Parser::expect(TokenKind kind, std::string_view message) {
        if (check(kind)) return advance();
        engine.error(peek().span, std::string(message) + " (found '" + peek().lexeme + "')");
        return peek(); 
    }

    bool Parser::isAtEnd() const {
        return peek().kind == TokenKind::End;
    }

} 
