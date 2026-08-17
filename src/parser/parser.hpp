#pragma once

#include "lexer/token.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Recursive-descent parser for Folio (.folio files).

namespace folio {

    struct ExprNode;
    struct FillNode;
    struct StrokeNode;
    struct NodeDecl;

    using ExprPtr = std::unique_ptr<ExprNode>;
    using FillPtr = std::unique_ptr<FillNode>;
    using StrokePtr = std::unique_ptr<StrokeNode>;
    using NodeDeclPtr = std::unique_ptr<NodeDecl>;

    enum class Unit { None, Px, Pt, Mm, Cm, In, Percent };

    /* Expressions (including normal expressions and shader expressions) */

    enum class ExprKind {
        Number,       // sized_number: NUMBER (UNIT | '%')?
        Bool,         // bool_literal: true | false
        String,       // STRING
        Color,        // HEXCOLOR form of color_literal
        Identifier,   // bare IDENT: statement-local var (expr) or shader local (shader_expr)
        MemberRef,    // reference: (self|parent|page|IDENT) '.' IDENT
        Swizzle,      // postfix_expr: primary_expr ('.' IDENT)*   (shader_expr only)
        Unary,        // unary_num / unary_expr
        Binary,       // add/mul/compare/equality/logic
        Ternary,      // '?' ':' 
        Call,         // rgb()/rgba()/hsl()/vec2()/vec3()/vec4()/import()/builtin_call
    };

    struct ExprNode {
        ExprKind kind;
        SourceSpan span;

        virtual ~ExprNode() = default;

    protected:
        ExprNode(ExprKind kind, SourceSpan span) : kind(kind), span(span) {}
    };

    struct NumberExpr : ExprNode {
        double value;
        Unit unit; // Unit::None => bare unitless scalar

        NumberExpr(double value, Unit unit, SourceSpan span)
            : ExprNode(ExprKind::Number, span), value(value), unit(unit) {}
    };

    struct BoolExpr : ExprNode {
        bool value;

        BoolExpr(bool value, SourceSpan span)
            : ExprNode(ExprKind::Bool, span), value(value) {}
    };

    struct StringExpr : ExprNode {
        std::string value;

        StringExpr(std::string value, SourceSpan span)
            : ExprNode(ExprKind::String, span), value(std::move(value)) {}
    };

    struct ColorExpr : ExprNode {
        std::string hex; // digits only, e.g. "FFAA00" or "FFAA0080" (no leading '#')

        ColorExpr(std::string hex, SourceSpan span)
            : ExprNode(ExprKind::Color, span), hex(std::move(hex)) {}
    };

    struct IdentifierExpr : ExprNode {
        std::string name;

        IdentifierExpr(std::string name, SourceSpan span)
            : ExprNode(ExprKind::Identifier, span), name(std::move(name)) {}
    };

    // reference := ('self' | 'parent' | 'page' | IDENT) '.' IDENT
    enum class RefBase { Self, Parent, Page, Named };

    struct MemberRefExpr : ExprNode {
        RefBase base;
        std::string baseName;  // only meaningful when base == RefBase::Named
        std::string property;  // the IDENT after '.'

        MemberRefExpr(RefBase base, std::string baseName, std::string property, SourceSpan span)
            : ExprNode(ExprKind::MemberRef, span), base(base),
            baseName(std::move(baseName)), property(std::move(property)) {}
    };

    // postfix_expr swizzle, e.g. `color.rgb`, `pixel.x`
    struct SwizzleExpr : ExprNode {
        ExprPtr base;
        std::string components; // e.g. "xyz", "rgb"

        SwizzleExpr(ExprPtr base, std::string components, SourceSpan span)
            : ExprNode(ExprKind::Swizzle, span), base(std::move(base)), components(std::move(components)) {}
    };

    enum class UnaryOp { Negate, Not };

    struct UnaryExpr : ExprNode {
        UnaryOp op;
        ExprPtr operand;

        UnaryExpr(UnaryOp op, ExprPtr operand, SourceSpan span)
            : ExprNode(ExprKind::Unary, span), op(op), operand(std::move(operand)) {}
    };

    enum class BinaryOp {
        Add, Sub, Mul, Div, Mod,
        Lt, Gt, Le, Ge, Eq, NotEq,
        And, Or,
    };

    struct BinaryExpr : ExprNode {
        BinaryOp op;
        ExprPtr left;
        ExprPtr right;

        BinaryExpr(BinaryOp op, ExprPtr left, ExprPtr right, SourceSpan span)
            : ExprNode(ExprKind::Binary, span), op(op), left(std::move(left)), right(std::move(right)) {}
    };

    struct TernaryExpr : ExprNode {
        ExprPtr condition;
        ExprPtr thenBranch;
        ExprPtr elseBranch;

        TernaryExpr(ExprPtr condition, ExprPtr thenBranch, ExprPtr elseBranch, SourceSpan span)
            : ExprNode(ExprKind::Ternary, span), condition(std::move(condition)),
            thenBranch(std::move(thenBranch)), elseBranch(std::move(elseBranch)) {}
    };

    // Covers rgb()/rgba()/hsl(), vec2()/vec3()/vec4(), import(), and any other built in shader function
    struct CallExpr : ExprNode {
        std::string callee;
        std::vector<ExprPtr> args;

        CallExpr(std::string callee, std::vector<ExprPtr> args, SourceSpan span)
            : ExprNode(ExprKind::Call, span), callee(std::move(callee)), args(std::move(args)) {}
    };

    // statement: IDENT '=' expr ';'? 
    // this is gonna be reused for shader_stmt (IDENT ':' type '=' shader_expr ';')
    // It carries a type annotation, `type` is nullopt for a plain node-body statement and set for a shader_stmt.
    struct Statement {
        std::string name;
        std::optional<std::string> type;
        ExprPtr value;
        SourceSpan span;
    };

    /* Page and Node declarations */

    // page_size := preset_size | dimension ',' dimension
    struct PageSize {
        std::optional<std::string> preset; // "A4"/"Letter"/etc or nullopt if size is explicitly set.
        ExprPtr width;                     // set when preset is nullopt
        ExprPtr height;                    // set when preset is nullopt
    };

    // page_decl := 'page' '{' page_prop* '}'
    struct PageDecl {
        std::optional<PageSize> size;
        FillPtr background;
        ExprPtr margin;
        SourceSpan span;
    };

    enum class NodeType { Rect, Circle, Ellipse, Path, Text, Image, Group };

    // properties shared by every node type
    struct CommonProps {
        ExprPtr x, y, width, height, rotation, opacity, z;
        FillPtr fill;
        StrokePtr stroke;
        std::optional<bool> visible;
    };

    enum class TextAlign { Left, Center, Right, Justify };

    struct TextProps {
        std::optional<std::string> content;
        std::optional<std::string> font;
        ExprPtr fontSize;
        ExprPtr fontWeightNumber;              // the NUMBER form of font_weight
        std::optional<bool> fontWeightIsBold;
        std::optional<TextAlign> align;
        ExprPtr lineHeight;
    };

    enum class ImageFit { Cover, Contain, Stretch, None };

    struct ImageProps {
        std::string sourcePath; // import_expr's STRING contents
        std::optional<ImageFit> fit;
    };

    // point := '(' expr ',' expr ')'
    struct PathPoint {
        ExprPtr x;
        ExprPtr y;
    };

    struct PathProps {
        std::vector<PathPoint> points; // point_list
        std::optional<bool> closed;
    };

    struct CircleProps {
        ExprPtr radius;
    };

    // node_decl := node_type IDENT? '{' node_body '}'
    struct NodeDecl {
        NodeType type;
        std::optional<std::string> name;
        SourceSpan span;

        CommonProps common;

        // Only the member matching `type` is ever populated.
        std::optional<CircleProps> circleProps;
        std::optional<TextProps> textProps;
        std::optional<ImageProps> imageProps;
        std::optional<PathProps> pathProps;

        // group_prop: groups nest other nodes directly
        std::vector<NodeDeclPtr> children;

        std::vector<Statement> statements;
    };

    // document := page_decl node_decl*
    struct Document {
        PageDecl page;
        std::vector<NodeDeclPtr> nodes;
    };

    class Parser {
    public:
        Parser(std::vector<Token> tokens, DiagnosticsEngine& engine);

        // document := page_decl node_decl* ;
        Document parse();

    private:
        std::vector<Token> tokens;
        DiagnosticsEngine& engine;
        std::size_t position = 0;

        // token stream helper functions 
        const Token& peek(std::size_t offset = 0) const;
        const Token& previous() const;
        const Token& advance();
        bool check(TokenKind kind) const;
        bool checkKeyword(std::string_view keyword) const; 
        bool match(TokenKind kind);
        bool matchKeyword(std::string_view keyword);
        const Token& expect(TokenKind kind, std::string_view message);
        bool isAtEnd() const;

        // for error recovery.
        // advance until a likely statement or prop boundary (';', '}') 
        // or a recognised prop/keyword starts after a parse error.
        void synchronize();

        /* Basic grammer parsing functions */

        PageDecl parsePageDecl();
        void parsePageProp(PageDecl& page);            
        PageSize parsePageSize();                       
        ExprPtr parseDimension();                         

        NodeDeclPtr parseNodeDecl();                       
        NodeType parseNodeType();                           
        void parseNodeBody(NodeDecl& node);                  
        bool parseCommonProp(NodeDecl& node, const Token& propName);
        bool parseShapeProp(NodeDecl& node, const Token& propName);   
        void parseCircleProp(NodeDecl& node, const Token& propName);
        void parseTextProp(NodeDecl& node, const Token& propName);
        void parseImageProp(NodeDecl& node, const Token& propName);
        void parsePathProp(NodeDecl& node, const Token& propName);
        std::vector<PathPoint> parsePointList();             
        PathPoint parsePoint();                                
        Statement parseStatement();                             

        // expression parsing 
        ExprPtr parseExpr();            
        ExprPtr parseTernary();         
        ExprPtr parseLogicOr();         
        ExprPtr parseLogicAnd();        
        ExprPtr parseEquality();        
        ExprPtr parseCompare();         
        ExprPtr parseAdd();             
        ExprPtr parseMul();             
        ExprPtr parseUnary();           
        ExprPtr parsePrimary();         
        ExprPtr parseSizedNumber();     
        ExprPtr parseReference();       

    };
} 
