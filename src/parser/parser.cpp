#include "parser/parser.hpp"
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

// Recursive-descent parser implementation.

namespace {

    using namespace folio;

    bool isNodeTypeKeyword(const std::string& text) {
        return text == "rect" || text == "circle" || text == "ellipse" ||
            text == "path" || text == "text" || text == "image" || text == "group";
    }

    bool isReservedKeyword(const std::string& text) {
        return text == "true" || text == "false" ||
            text == "self" || text == "parent" || text == "page" || text == "at";
    }

    // The AST holds one std::optional per property, so a repeated property silently
    // overwrites the earlier one and later stages could never tell. The parser is the
    // last place that can see both occurrences, so it reports the repeat.
    void reportDuplicateProperty(DiagnosticsEngine& engine, const Token& nameTok) {
        engine.error(nameTok.span, "property '" + nameTok.lexeme + "' is set more than once; each property can appear only once");
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

    // "#RGB" / "#RGBA" shorthand -> "RRGGBB" / "RRGGBBAA" (each digit doubled, like CSS).
    // Longer forms pass through unchanged, so a ColorExpr always holds 6 or 8 digits.
    std::string expandHexDigits(const std::string& digits) {
        if (digits.size() != 3 && digits.size() != 4) return digits;
        std::string out;
        for (char c : digits) { out += c; out += c; }
        return out;
    }

    double numberFromLexeme(const std::string& lexeme) {
        try {
            return std::stod(lexeme);
        }
        catch (...) {
            // Shouldn't happen: the lexer only emits well-formed NUMBER lexemes.
            // Fall back to 0 rather than let an exception escape the parser.
            return 0.0;
        }
    }

}

namespace folio {

    Parser::Parser(std::vector<Token> tokens, DiagnosticsEngine& engine)
        : tokens(std::move(tokens)), engine(engine) {
        exprPolicy.parseOperand = [](Parser& p) { return p.parsePrimary(); };
        shaderPolicy.parseOperand = [](Parser& p) { return p.parseShaderPostfix(); };
    }

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

    void Parser::synchronize()
    {
        // Callers invoke this right after reporting an error, sometimes
        // without having consumed anything. Unconditionally advancing past that token
        // first, guarantees this call always makes forward progress.
        if (isAtEnd()) return;
        advance();

        while (!isAtEnd()) {
            if (previous().kind == TokenKind::Semicolon) return;
            if (check(TokenKind::RBrace)) return; // let the caller consume the closing brace itself

            // An Ident immediately followed by ':' or '=' looks like the
            // start of a fresh prop/statement. Which is a reasonable place to
            // resume parsing after an error.
            if (check(TokenKind::Ident) &&
                (peek(1).kind == TokenKind::Colon || peek(1).kind == TokenKind::Eq)) {
                return;
            }

            advance();
        }
    }

    // top-level grammar

    Document Parser::parse() {
        Document doc;
        doc.page = parsePageDecl();

        while (!isAtEnd()) {
            if (checkKeyword("page")) {
                engine.error(peek().span, "unexpected second 'page' declaration; a document has exactly one");
                synchronize();
                continue;
            }
            if (!(check(TokenKind::Ident) && isNodeTypeKeyword(peek().lexeme))) {
                engine.error(peek().span,
                    "expected a node declaration ('rect', 'circle', 'ellipse', 'path', 'text', 'image', or 'group')");
                synchronize();
                continue;
            }
            doc.nodes.push_back(parseNodeDecl());
        }

        return doc;
    }

    PageDecl Parser::parsePageDecl() {
        PageDecl page;
        const Token& startTok = peek();

        if (!matchKeyword("page")) {
            engine.error(peek().span, "expected 'page' declaration at the start of the document");
            synchronize();
            page.span = SourceSpan{ startTok.span.start, previous().span.end };
            return page;
        }

        expect(TokenKind::LBrace, "expected '{' after 'page'");

        while (!isAtEnd() && !check(TokenKind::RBrace)) {
            parsePageProp(page);
        }

        const Token& closeBrace = expect(TokenKind::RBrace, "expected '}' to close 'page' block");
        page.span = SourceSpan{ startTok.span.start, closeBrace.span.end };
        return page;
    }

    void Parser::parsePageProp(PageDecl& page) {
        const Token& nameTok = peek();

        if (nameTok.kind != TokenKind::Ident) {
            engine.error(nameTok.span, "expected a page property ('size', 'background', or 'margin')");
            synchronize();
            return;
        }

        if (nameTok.lexeme == "size") {
            if (page.size) reportDuplicateProperty(engine, nameTok);
            advance();
            expect(TokenKind::Colon, "expected ':' after 'size'");
            page.size = parsePageSize();
        }
        else if (nameTok.lexeme == "background") {
            if (page.background) reportDuplicateProperty(engine, nameTok);
            advance();
            expect(TokenKind::Colon, "expected ':' after 'background'");
            page.background = parseFillExpr();
        }
        else if (nameTok.lexeme == "margin") {
            if (page.margin) reportDuplicateProperty(engine, nameTok);
            advance();
            expect(TokenKind::Colon, "expected ':' after 'margin'");
            page.margin = parseDimension();
        }
        else {
            engine.error(nameTok.span, "unknown page property '" + nameTok.lexeme + "'");
            synchronize();
            return;
        }

        match(TokenKind::Semicolon); // tolerated but not required.
    }

    PageSize Parser::parsePageSize() {
        PageSize size;

        static constexpr std::string_view presets[] = { "A4", "A3", "A5", "Letter", "Legal", "Tabloid" };
        if (check(TokenKind::Ident)) {
            for (auto preset : presets) {
                if (peek().lexeme == preset) {
                    size.preset = std::string(preset);
                    advance();
                    return size;
                }
            }
        }

        size.width = parseDimension();
        expect(TokenKind::Comma, "expected ',' between page width and height");
        size.height = parseDimension();
        return size;
    }

    Expr Parser::parseDimension() {
        const Token& numberTok = expect(TokenKind::Number, "expected a number");
        SourceSpan span = numberTok.span;
        double value = numberFromLexeme(numberTok.lexeme);

        Unit unit = Unit::None;
        if (check(TokenKind::Percent) && peek().span.start == span.end) {
            // Spec 4.1: page size/margin resolve before any node exists, so a
            // percentage has nothing to be a percentage *of*. Say so, and eat
            // the '%' so it doesn't cascade into a second, confusing error.
            engine.error(peek().span, "percentages aren't allowed in a page 'size' or 'margin'; use an absolute unit (pt, px, mm, cm, in)");
            advance();
            span.end = previous().span.end;
        }
        else if (check(TokenKind::Ident)) {
            unit = unitFromText(peek().lexeme);
            if (unit != Unit::None) {
                advance();
                span.end = previous().span.end;
            }
        }

        return Expr{ NumberExpr{ value, unit }, span };
    }

    NodeDecl Parser::parseNodeDecl() {
        const Token& startTok = peek();
        NodeType type = parseNodeType();

        NodeDecl node;
        node.type = type;

        if (check(TokenKind::Ident)) {
            const Token& nameTok = peek();
            if (isReservedKeyword(nameTok.lexeme)) {
                engine.error(nameTok.span, "'" + nameTok.lexeme + "' is a reserved word and can't be used as a node name");
                advance(); // consume it but leave node.name unset, i.e. treat the node as unnamed
            }
            else {
                node.name = advance().lexeme;
            }
        }

        expect(TokenKind::LBrace, "expected '{' to start a node body");
        parseNodeBody(node);
        const Token& closeBrace = expect(TokenKind::RBrace, "expected '}' to close a node body");

        node.span = SourceSpan{ startTok.span.start, closeBrace.span.end };
        return node;
    }

    NodeType Parser::parseNodeType() {
        const Token& tok = peek();
        if (tok.kind == TokenKind::Ident && isNodeTypeKeyword(tok.lexeme)) {
            advance();
            return nodeTypeFromKeyword(tok.lexeme);
        }

        // Defensive only: every call site checks isNodeTypeKeyword() before
        // calling parseNodeDecl(), so this branch shouldn't be reached in
        // practice. It doesn't consume and the caller is responsible for
        // synchronizing.
        engine.error(tok.span,
            "expected a node type ('rect', 'circle', 'ellipse', 'path', 'text', 'image', or 'group')");
        return NodeType::Rect;
    }

    void Parser::parseNodeBody(NodeDecl& node) {
        std::unordered_set<std::string> seenProperties; // for duplicate detection; per node body
        while (!isAtEnd() && !check(TokenKind::RBrace)) {
            // group_prop: groups nest other nodes directly.
            if (node.type == NodeType::Group && check(TokenKind::Ident) && isNodeTypeKeyword(peek().lexeme)) {
                node.children.push_back(parseNodeDecl());
                continue;
            }

            // statement := IDENT '=' expr ';'?
            if (check(TokenKind::Ident) && peek(1).kind == TokenKind::Eq) {
                node.statements.push_back(parseStatement());
                continue;
            }

            // common_prop / shape_prop, both shaped `IDENT ':' ...`
            if (check(TokenKind::Ident) && peek(1).kind == TokenKind::Colon) {
                const Token& propName = peek();
                if (parseCommonProp(node, propName) || parseShapeProp(node, propName)) {
                    if (!seenProperties.insert(propName.lexeme).second) {
                        reportDuplicateProperty(engine, propName);
                    }
                    continue;
                }

                engine.error(propName.span, "unknown property '" + propName.lexeme + "' for this node type");
                synchronize();
                continue;
            }

            engine.error(peek().span,
                "expected a property, a local variable assignment, or (inside a group) a nested node declaration");
            synchronize();
        }
    }

    bool Parser::parseCommonProp(NodeDecl& node, const Token& propName) {
        const std::string& name = propName.lexeme;

        if (name == "x") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'x'");
            node.common.x = parseExpr();
        }
        else if (name == "y") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'y'");
            node.common.y = parseExpr();
        }
        else if (name == "width") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'width'");
            node.common.width = parseExpr();
        }
        else if (name == "height") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'height'");
            node.common.height = parseExpr();
        }
        else if (name == "rotation") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'rotation'");
            node.common.rotation = parseExpr();
        }
        else if (name == "opacity") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'opacity'");
            node.common.opacity = parseExpr();
        }
        else if (name == "z") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'z'");
            node.common.z = parseExpr();
        }
        else if (name == "fill") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'fill'");
            node.common.fill = parseFillExpr();
        }
        else if (name == "stroke") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'stroke'");
            node.common.stroke = parseStrokeExpr();
        }
        else if (name == "visible") {
            advance(); 
            expect(TokenKind::Colon, "expected ':' after 'visible'");
            if (matchKeyword("true")) node.common.visible = true;
            else if (matchKeyword("false")) node.common.visible = false;
            else engine.error(peek().span, "expected 'true' or 'false' for 'visible'");
        }
        else {
            return false;
        }
        return true;
    }

    bool Parser::parseShapeProp(NodeDecl& node, const Token& propName) {
        const std::string& name = propName.lexeme;

        switch (node.type) {
        case NodeType::Circle:
            if (name == "radius") { parseCircleProp(node, propName); return true; }
            return false;

        case NodeType::Text:
            if (name == "content" || name == "font" || name == "font_size" ||
                name == "font_weight" || name == "align" || name == "line_height") {
                parseTextProp(node, propName);
                return true;
            }
            return false;

        case NodeType::Image:
            if (name == "source" || name == "fit") {
                parseImageProp(node, propName);
                return true;
            }
            return false;

        case NodeType::Path:
            if (name == "points" || name == "closed") {
                parsePathProp(node, propName);
                return true;
            }
            return false;

        case NodeType::Rect:
        case NodeType::Ellipse:
        case NodeType::Group:
            return false; // no shape-specific properties for these types
        }
        return false;
    }

    void Parser::parseCircleProp(NodeDecl& node, const Token& propName) {
        (void)propName; // Its always "radius" and parseShapeProp only dispatches here for that case
        advance();
        expect(TokenKind::Colon, "expected ':' after 'radius'");
        if (!node.circleProps) node.circleProps = CircleProps{};
        node.circleProps->radius = parseExpr();
    }

    void Parser::parseTextProp(NodeDecl& node, const Token& propName) {
        const std::string name = propName.lexeme;
        advance();
        expect(TokenKind::Colon, "expected ':' after '" + name + "'");

        if (!node.textProps) node.textProps = TextProps{};
        TextProps& text = *node.textProps;

        if (name == "content") {
            text.content = expect(TokenKind::String, "expected a string for 'content'").lexeme;
        }
        else if (name == "font") {
            text.font = expect(TokenKind::String, "expected a string for 'font'").lexeme;
        }
        else if (name == "font_size") {
            text.fontSize = parseExpr();
        }
        else if (name == "font_weight") {
            if (matchKeyword("normal")) text.fontWeightIsBold = false;
            else if (matchKeyword("bold")) text.fontWeightIsBold = true;
            else text.fontWeightNumber = parseExpr();
        }
        else if (name == "align") {
            if (matchKeyword("left")) text.align = TextAlign::Left;
            else if (matchKeyword("center")) text.align = TextAlign::Center;
            else if (matchKeyword("right")) text.align = TextAlign::Right;
            else if (matchKeyword("justify")) text.align = TextAlign::Justify;
            else engine.error(peek().span, "expected one of 'left'/'center'/'right'/'justify' for 'align'");
        }
        else if (name == "line_height") {
            text.lineHeight = parseExpr();
        }
    }

    void Parser::parseImageProp(NodeDecl& node, const Token& propName) {
        const std::string name = propName.lexeme;
        advance();
        expect(TokenKind::Colon, "expected ':' after '" + name + "'");

        if (!node.imageProps) node.imageProps = ImageProps{};
        ImageProps& image = *node.imageProps;

        if (name == "source") {
            image.sourcePath = parseImportExpr();
        }
        else if (name == "fit") {
            if (matchKeyword("cover")) image.fit = ImageFit::Cover;
            else if (matchKeyword("contain")) image.fit = ImageFit::Contain;
            else if (matchKeyword("stretch")) image.fit = ImageFit::Stretch;
            else if (matchKeyword("none")) image.fit = ImageFit::None;
            else engine.error(peek().span, "expected one of 'cover'/'contain'/'stretch'/'none' for 'fit'");
        }
    }

    void Parser::parsePathProp(NodeDecl& node, const Token& propName) {
        const std::string name = propName.lexeme;
        advance();
        expect(TokenKind::Colon, "expected ':' after '" + name + "'");

        if (!node.pathProps) node.pathProps = PathProps{};
        PathProps& path = *node.pathProps;

        if (name == "points") {
            path.points = parsePointList();
        }
        else if (name == "closed") {
            if (matchKeyword("true")) path.closed = true;
            else if (matchKeyword("false")) path.closed = false;
            else engine.error(peek().span, "expected 'true' or 'false' for 'closed'");
        }
    }

    std::vector<PathPoint> Parser::parsePointList() {
        std::vector<PathPoint> points;
        expect(TokenKind::LBracket, "expected '[' to start a point list");

        if (!check(TokenKind::RBracket)) {
            points.push_back(parsePoint());
            while (match(TokenKind::Comma)) {
                points.push_back(parsePoint());
            }
        }

        expect(TokenKind::RBracket, "expected ']' to close a point list");
        return points;
    }

    PathPoint Parser::parsePoint() {
        expect(TokenKind::LParen, "expected '(' to start a point");
        Expr x = parseExpr();
        expect(TokenKind::Comma, "expected ',' between point coordinates");
        Expr y = parseExpr();
        expect(TokenKind::RParen, "expected ')' to close a point");
        return PathPoint{ std::move(x), std::move(y) };
    }

    Statement Parser::parseStatement() {
        const Token& nameTok = expect(TokenKind::Ident, "expected an identifier");
        if (isReservedKeyword(nameTok.lexeme)) {
            engine.error(nameTok.span, "'" + nameTok.lexeme + "' is a reserved word and can't be used as a variable name");
        }

        expect(TokenKind::Eq, "expected '=' in a local variable assignment");
        Expr value = parseExpr();
        match(TokenKind::Semicolon); // optional, per the grammar

        return Statement{ nameTok.lexeme, std::nullopt, std::move(value),
                          SourceSpan{ nameTok.span.start, previous().span.end } };
    }

    // Expressions

    // expr := ternary_num_expr. The whole precedence chain (ternary, ||, &&,
    // equality, compare, add, mul, unary) lives in parseExprPrecedence().
    Expr Parser::parseExpr() { return parseExprPrecedence(*this, exprPolicy); }

    // primary_num := sized_number | color_literal | reference | IDENT | '(' expr ')'
    Expr Parser::parsePrimary() {
        const Token& tok = peek();

        if (tok.kind == TokenKind::Number) {
            return parseSizedNumber();
        }
        if (tok.kind == TokenKind::HexColor) {
            return parseColorLiteral();
        }
        if (checkKeyword("rgb") || checkKeyword("rgba") || checkKeyword("hsl")) {
            return parseColorLiteral();
        }
        if (checkKeyword("true")) {
            advance();
            return Expr{ BoolExpr{ true }, previous().span };
        }
        if (checkKeyword("false")) {
            advance();
            return Expr{ BoolExpr{ false }, previous().span };
        }
        if (checkKeyword("self") || checkKeyword("parent") || checkKeyword("page")) {
            return parseReference();
        }
        if (tok.kind == TokenKind::String) {
            // Not part of primary_num in the grammar, but accepting it here
            // (rather than falling through to the generic error) lets a
            // stray string still produce a real AST node.
            advance();
            return Expr{ StringExpr{ tok.lexeme }, tok.span };
        }
        if (tok.kind == TokenKind::Ident) {
            if (peek(1).kind == TokenKind::Dot) {
                return parseReference(); // IDENT '.' IDENT  (reference to another node's property)
            }
            advance();
            return Expr{ IdentifierExpr{ tok.lexeme }, tok.span }; // bare local-variable reference
        }
        if (match(TokenKind::LParen)) {
            Expr inner = parseExpr();
            expect(TokenKind::RParen, "expected ')' to close a parenthesized expression");
            return inner; // the grouping isn't kept in the tree, see the design note in parser.hpp
        }

        engine.error(tok.span, "expected an expression");
        advance(); // guarantee forward progress even on totally malformed input
        return Expr{ NumberExpr{ 0.0, Unit::None }, tok.span };
    }

    // sized_number := NUMBER (UNIT | '%')?
    // A '%' is a percent unit only when it touches the number (`50%`). With
    // whitespace (`5 % 6`) it's the modulo operator and is left for parseMul().
    Expr Parser::parseSizedNumber() {
        const Token& numberTok = expect(TokenKind::Number, "expected a number");
        SourceSpan span = numberTok.span;
        double value = numberFromLexeme(numberTok.lexeme);

        Unit unit = Unit::None;
        if (check(TokenKind::Percent) && peek().span.start == span.end) {
            advance();
            unit = Unit::Percent;
            span.end = previous().span.end;
        }
        else if (check(TokenKind::Ident)) {
            unit = unitFromText(peek().lexeme);
            if (unit != Unit::None) {
                advance();
                span.end = previous().span.end;
            }
        }

        return Expr{ NumberExpr{ value, unit }, span };
    }

    // shader primary_expr := NUMBER   (no unit, no percent: `%` is always modulo here)
    Expr Parser::parseShaderNumber() {
        const Token& numberTok = expect(TokenKind::Number, "expected a number");
        return Expr{ NumberExpr{ numberFromLexeme(numberTok.lexeme), Unit::None }, numberTok.span };
    }

    // reference := ('self' | 'parent' | 'page' | IDENT) '.' IDENT
    Expr Parser::parseReference() {
        const Token& baseTok = advance(); // caller has already confirmed this is a valid base

        RefBase base;
        std::string baseName;
        if (baseTok.lexeme == "self") base = RefBase::Self;
        else if (baseTok.lexeme == "parent") base = RefBase::Parent;
        else if (baseTok.lexeme == "page") base = RefBase::Page;
        else { base = RefBase::Named; baseName = baseTok.lexeme; }

        expect(TokenKind::Dot, "expected '.' in a reference");
        const Token& propTok = expect(TokenKind::Ident, "expected a property name after '.'");

        SourceSpan span{ baseTok.span.start, propTok.span.end };
        return Expr{ MemberRefExpr{ base, std::move(baseName), propTok.lexeme }, span };
    }

    // Fill / stroke sublanguage

    Fill Parser::parseFillExpr()
    {
        if (checkKeyword("linear_gradient")) return parseGradientFill(false);
        if (checkKeyword("radial_gradient")) return parseGradientFill(true);
        if (checkKeyword("texture")) return parseTextureFill();
        if (checkKeyword("shader")) return parseShaderFill();

        const Token& startTok = peek();
        Expr color = parseColorLiteral(); // solid_fill := color_literal
        SourceSpan span{ startTok.span.start, previous().span.end };
        return Fill{ SolidFill{ std::move(color) }, span };
    }

    Expr Parser::parseColorLiteral(bool insideShader)
    {
        const Token& tok = peek();

        if (tok.kind == TokenKind::HexColor) {
            advance();
            return Expr{ ColorExpr{ expandHexDigits(tok.lexeme.substr(1)) }, tok.span }; // strip leading '#', expand #RGB[A]
        }

        if (checkKeyword("rgb") || checkKeyword("rgba") || checkKeyword("hsl")) {
            const Token& nameTok = advance();
            std::size_t expectedArgs = (nameTok.lexeme == "rgba") ? 4 : 3;

            expect(TokenKind::LParen, "expected '(' after '" + nameTok.lexeme + "'");
            std::vector<Expr> args;
            if (!check(TokenKind::RParen)) {
                args.push_back(insideShader ? parseShaderExpr() : parseExpr());
                while (match(TokenKind::Comma)) {
                    args.push_back(insideShader ? parseShaderExpr() : parseExpr());
                }
            }
            const Token& closeParen = expect(TokenKind::RParen, "expected ')' to close '" + nameTok.lexeme + "(...)'");

            if (args.size() != expectedArgs) {
                engine.error(SourceSpan{ nameTok.span.start, closeParen.span.end },
                    "'" + nameTok.lexeme + "' expects " + std::to_string(expectedArgs) +
                    " arguments, found " + std::to_string(args.size()));
            }

            SourceSpan span{ nameTok.span.start, closeParen.span.end };
            return Expr{ CallExpr{ nameTok.lexeme, std::move(args) }, span };
        }

        engine.error(tok.span, "expected a color (a hex color, or rgb()/rgba()/hsl())");
        advance();
        return Expr{ ColorExpr{ "000000" }, tok.span };
    }

    Fill Parser::parseGradientFill(bool isRadial)
    {
        const Token& startTok = advance(); // 'linear_gradient' or 'radial_gradient'
        expect(TokenKind::LParen, "expected '(' after '" + startTok.lexeme + "'");

        if (!isRadial) {
            // angle_or_points := expr | '(' expr ',' expr ')' '->' '(' expr ',' expr ')'
            LinearGradientFill fill;

            if (check(TokenKind::LParen)) {
                PathPoint start = parsePoint();
                fill.startX = std::move(start.x);
                fill.startY = std::move(start.y);
                expect(TokenKind::ThinArrow, "expected '->' between gradient start and end points");
                PathPoint end = parsePoint();
                fill.endX = std::move(end.x);
                fill.endY = std::move(end.y);
            }
            else {
                fill.angle = parseExpr();
            }

            expect(TokenKind::Comma, "expected ',' before the gradient stop list");
            fill.stops = parseStopList();
            const Token& closeParen = expect(TokenKind::RParen, "expected ')' to close 'linear_gradient(...)'");
            return Fill{ std::move(fill), SourceSpan{ startTok.span.start, closeParen.span.end } };
        }

        // radial_spec := expr | expr 'at' '(' expr ',' expr ')'
        RadialGradientFill fill;
        fill.radius = parseExpr();
        if (matchKeyword("at")) {
            expect(TokenKind::LParen, "expected '(' after 'at'");
            fill.centerX = parseExpr();
            expect(TokenKind::Comma, "expected ',' between the center point's coordinates");
            fill.centerY = parseExpr();
            expect(TokenKind::RParen, "expected ')' to close the center point");
        }
        expect(TokenKind::Comma, "expected ',' before the gradient stop list");
        fill.stops = parseStopList();
        const Token& closeParen = expect(TokenKind::RParen, "expected ')' to close 'radial_gradient(...)'");
        return Fill{ std::move(fill), SourceSpan{ startTok.span.start, closeParen.span.end } };
    }

    std::vector<GradientStop> Parser::parseStopList()
    {
        std::vector<GradientStop> stops;
        stops.push_back(parseStop());
        while (match(TokenKind::Comma)) {
            stops.push_back(parseStop());
        }
        return stops;
    }

    GradientStop Parser::parseStop()
    {
        Expr color = parseColorLiteral();
        std::optional<Expr> position;
        if (match(TokenKind::At)) {
            position = parseExpr();
        }
        return GradientStop{ std::move(color), std::move(position) };
    }

    Fill Parser::parseTextureFill()
    {
        const Token& startTok = advance(); // 'texture'
        expect(TokenKind::LParen, "expected '(' after 'texture'");
        std::string path = parseImportExpr();
        const Token& closeParen = expect(TokenKind::RParen, "expected ')' to close 'texture(...)'");
        SourceSpan span{ startTok.span.start, closeParen.span.end };
        return Fill{ TextureFill{ std::move(path) }, span };
    }

    Fill Parser::parseShaderFill()
    {
        const Token& startTok = advance(); // 'shader'
        expect(TokenKind::LBrace, "expected '{' after 'shader'");

        ShaderFill fill;

        while (!isAtEnd() && !checkKeyword("return") && !check(TokenKind::RBrace)) {
            fill.statements.push_back(parseShaderStatement());
        }

        if (!matchKeyword("return")) {
            engine.error(peek().span, "expected 'return' in a shader body");
        }
        else {
            fill.returnExpr = parseShaderExpr();
            expect(TokenKind::Semicolon, "expected ';' after the shader's 'return' expression");
        }

        const Token& closeBrace = expect(TokenKind::RBrace, "expected '}' to close 'shader { ... }'");
        return Fill{ std::move(fill), SourceSpan{ startTok.span.start, closeBrace.span.end } };
    }

    Statement Parser::parseShaderStatement()
    {
        const Token& nameTok = expect(TokenKind::Ident, "expected a local variable name");
        if (isReservedKeyword(nameTok.lexeme)) {
            engine.error(nameTok.span, "'" + nameTok.lexeme + "' is a reserved word and can't be used as a variable name");
        }

        expect(TokenKind::Colon, "expected ':' after the variable name (e.g. 'd: float = ...')");
        const Token& typeTok = expect(TokenKind::Ident,
            "expected a type (float, int, bool, vec2, vec3, vec4, color, or texture)");

        expect(TokenKind::Eq, "expected '=' after the type");
        Expr value = parseShaderExpr();
        expect(TokenKind::Semicolon, "expected ';' after a shader local variable declaration");

        return Statement{ nameTok.lexeme, typeTok.lexeme, std::move(value),
                          SourceSpan{ nameTok.span.start, previous().span.end } };
    }

    std::string Parser::parseImportExpr()
    {
        if (!matchKeyword("import")) {
            engine.error(peek().span, "expected 'import(...)'");
            return "";
        }
        expect(TokenKind::LParen, "expected '(' after 'import'");
        const Token& pathTok = expect(TokenKind::String, "expected a string path inside 'import(...)'");
        expect(TokenKind::RParen, "expected ')' to close 'import(...)'");
        return pathTok.lexeme;
    }

    Stroke Parser::parseStrokeExpr()
    {
        Stroke stroke;
        const Token& startTok = peek();

        if (matchKeyword("none")) {
            stroke.isNone = true;
            stroke.span = previous().span;
            return stroke;
        }

        expect(TokenKind::LBrace, "expected '{' or 'none' for a stroke");

        if (!matchKeyword("color")) {
            engine.error(peek().span, "expected 'color' as the first field of a stroke");
        }
        expect(TokenKind::Colon, "expected ':' after 'color'");
        stroke.color = parseFillExpr();

        expect(TokenKind::Comma, "expected ',' after the stroke's 'color' field");
        if (!matchKeyword("width")) {
            engine.error(peek().span, "expected 'width' as the second field of a stroke");
        }
        expect(TokenKind::Colon, "expected ':' after 'width'");
        stroke.width = parseExpr();

        if (match(TokenKind::Comma)) {
            if (matchKeyword("cap")) {
                expect(TokenKind::Colon, "expected ':' after 'cap'");
                if (matchKeyword("butt")) stroke.cap = CapStyle::Butt;
                else if (matchKeyword("round")) stroke.cap = CapStyle::Round;
                else if (matchKeyword("square")) stroke.cap = CapStyle::Square;
                else engine.error(peek().span, "expected 'butt', 'round', or 'square' for 'cap'");

                if (match(TokenKind::Comma) && matchKeyword("join")) {
                    expect(TokenKind::Colon, "expected ':' after 'join'");
                    if (matchKeyword("miter")) stroke.join = JoinStyle::Miter;
                    else if (matchKeyword("round")) stroke.join = JoinStyle::Round;
                    else if (matchKeyword("bevel")) stroke.join = JoinStyle::Bevel;
                    else engine.error(peek().span, "expected 'miter', 'round', or 'bevel' for 'join'");
                }
            }
            else if (matchKeyword("join")) {
                expect(TokenKind::Colon, "expected ':' after 'join'");
                if (matchKeyword("miter")) stroke.join = JoinStyle::Miter;
                else if (matchKeyword("round")) stroke.join = JoinStyle::Round;
                else if (matchKeyword("bevel")) stroke.join = JoinStyle::Bevel;
                else engine.error(peek().span, "expected 'miter', 'round', or 'bevel' for 'join'");
            }
        }

        const Token& closeBrace = expect(TokenKind::RBrace, "expected '}' to close a stroke");
        stroke.span = SourceSpan{ startTok.span.start, closeBrace.span.end };
        return stroke;
    }

    // Shader expression language

    // shader_expr: same precedence chain as expr, over postfix_expr operands.
    Expr Parser::parseShaderExpr() { return parseExprPrecedence(*this, shaderPolicy); }

    // postfix_expr := primary_expr ('.' IDENT)*   (swizzle: .x .xy .rgb etc)
    Expr Parser::parseShaderPostfix() {
        Expr expr = parseShaderPrimary();
        while (check(TokenKind::Dot)) {
            advance();
            const Token& compTok = expect(TokenKind::Ident, "expected a swizzle (e.g. '.x', '.rgb') after '.'");
            SourceSpan span{ expr.span.start, compTok.span.end };
            expr = Expr{ SwizzleExpr{ Box<Expr>(std::move(expr)), compTok.lexeme }, span };
        }
        return expr;
    }

    // primary_expr := NUMBER | color_literal | vec_constructor | IDENT | builtin_call | '(' shader_expr ')'
    Expr Parser::parseShaderPrimary() {
        const Token& tok = peek();

        if (tok.kind == TokenKind::Number) {
            return parseShaderNumber();
        }
        if (tok.kind == TokenKind::HexColor) {
            return parseColorLiteral(/*insideShader=*/true);
        }
        if (checkKeyword("rgb") || checkKeyword("rgba") || checkKeyword("hsl")) {
            return parseColorLiteral(/*insideShader=*/true);
        }
        if (checkKeyword("vec2") || checkKeyword("vec3") || checkKeyword("vec4")) {
            return parseVecConstructor();
        }
        if (tok.kind == TokenKind::Ident && peek(1).kind == TokenKind::LParen) {
            // Any other IDENT directly followed by '(' is a builtin_call
            // (sdf_rect, mix, clamp, smoothstep, length, distance, dot,
            // sin, cos, abs, floor, fract, pow, noise, fbm, sample, ...).
            // Per-builtin arg-count/type checking is deferred for now.
            return parseBuiltinCall();
        }
        if (tok.kind == TokenKind::Ident) {
            advance();
            return Expr{ IdentifierExpr{ tok.lexeme }, tok.span }; // shader local, or a builtin var (pixel/uv/node_size)
        }
        if (match(TokenKind::LParen)) {
            Expr inner = parseShaderExpr();
            expect(TokenKind::RParen, "expected ')' to close a parenthesized expression");
            return inner;
        }

        engine.error(tok.span, "expected an expression");
        advance();
        return Expr{ NumberExpr{ 0.0, Unit::None }, tok.span };
    }

    // vec_constructor := ('vec2' | 'vec3' | 'vec4') '(' expr_list ')'
    Expr Parser::parseVecConstructor() {
        const Token& nameTok = advance(); // 'vec2'/'vec3'/'vec4'
        expect(TokenKind::LParen, "expected '(' after '" + nameTok.lexeme + "'");
        std::vector<Expr> args = parseExprList();
        const Token& closeParen = expect(TokenKind::RParen, "expected ')' to close '" + nameTok.lexeme + "(...)'");
        SourceSpan span{ nameTok.span.start, closeParen.span.end };
        return Expr{ CallExpr{ nameTok.lexeme, std::move(args) }, span };
    }

    // builtin_call := builtin_fn '(' expr_list ')'
    Expr Parser::parseBuiltinCall() {
        const Token& nameTok = expect(TokenKind::Ident, "expected a function name");
        expect(TokenKind::LParen, "expected '(' after '" + nameTok.lexeme + "'");
        std::vector<Expr> args = parseExprList();
        const Token& closeParen = expect(TokenKind::RParen, "expected ')' to close '" + nameTok.lexeme + "(...)'");
        SourceSpan span{ nameTok.span.start, closeParen.span.end };
        return Expr{ CallExpr{ nameTok.lexeme, std::move(args) }, span };
    }

    // expr_list := shader_expr (',' shader_expr)*
    std::vector<Expr> Parser::parseExprList() {
        std::vector<Expr> args;
        if (!check(TokenKind::RParen)) {
            args.push_back(parseShaderExpr());
            while (match(TokenKind::Comma)) {
                args.push_back(parseShaderExpr());
            }
        }
        return args;
    }

}