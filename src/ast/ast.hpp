#pragma once

#include "diagnostics/diagnostics.hpp"
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The Folio AST: a direct structural translation of the grammar in
// LANGUAGE-SPEC.md. Nothing is resolved here ("what did the person write").
//
// Nothing in this header depends on the lexer: spans are copied out of tokens
// once, so downstream code (sema, exporters) never has to hold a Token.

namespace folio {

    // Owning, non-null-by-construction heap box. std::variant can't contain
    // itself directly, so recursive AST members (a BinaryExpr's operands, etc.)
    // go through this. Move-only on purpose; moved-from boxes are empty.
    template <typename T>
    class Box {
    public:
        explicit Box(T&& value) : ptr(std::make_unique<T>(std::move(value))) {}

        Box(Box&&) noexcept = default;
        Box& operator=(Box&&) noexcept = default;
        Box(const Box&) = delete;
        Box& operator=(const Box&) = delete;

        T& operator*() { return *ptr; }
        const T& operator*() const { return *ptr; }
        T* operator->() { return ptr.get(); }
        const T* operator->() const { return ptr.get(); }

    private:
        std::unique_ptr<T> ptr;
    };

    enum class Unit { None, Px, Pt, Mm, Cm, In, Percent };

    /* Expressions (including normal expressions and shader expressions)
     *
     * Expr is a closed sum type: a std::variant of every expression form the
     * grammar allows, plus the source span shared by all of them. Consume it
     * with std::visit; adding a new alternative to Expr::Node makes every
     * visitor that doesn't handle it fail to compile.
     */

    struct Expr;

    // sized_number: NUMBER (UNIT | '%')?
    struct NumberExpr {
        double value;
        Unit unit; // Unit::None => bare unitless scalar
    };

    // bool_literal: true | false
    struct BoolExpr {
        bool value;
    };

    // STRING
    struct StringExpr {
        std::string value;
    };

    // HEXCOLOR form of color_literal
    struct ColorExpr {
        std::string hex; // digits only, always 6 or 8 (no leading '#'); #RGB/#RGBA shorthand is expanded by the parser
    };

    // bare IDENT: statement-local var (expr) or shader local (shader_expr)
    struct IdentifierExpr {
        std::string name;
    };

    // reference := ('self' | 'parent' | 'page' | IDENT) '.' IDENT
    enum class RefBase { Self, Parent, Page, Named };

    struct MemberRefExpr {
        RefBase base;
        std::string baseName;  // only meaningful when base == RefBase::Named
        std::string property;  // the IDENT after '.'
    };

    // postfix_expr swizzle, e.g. `color.rgb`, `pixel.x` (shader_expr only)
    struct SwizzleExpr {
        Box<Expr> base;
        std::string components; // e.g. "xyz", "rgb"
    };

    enum class UnaryOp { Negate, Not };

    // unary_num / unary_expr
    struct UnaryExpr {
        UnaryOp op;
        Box<Expr> operand;
    };

    enum class BinaryOp {
        Add, Sub, Mul, Div, Mod,
        Lt, Gt, Le, Ge, Eq, NotEq,
        And, Or,
    };

    // add/mul/compare/equality/logic
    struct BinaryExpr {
        BinaryOp op;
        Box<Expr> left;
        Box<Expr> right;
    };

    // condition '?' thenBranch ':' elseBranch
    struct TernaryExpr {
        Box<Expr> condition;
        Box<Expr> thenBranch;
        Box<Expr> elseBranch;
    };

    // Covers rgb()/rgba()/hsl(), vec2()/vec3()/vec4(), and any other built in shader function
    struct CallExpr {
        std::string callee;
        std::vector<Expr> args; // std::vector may hold an incomplete type, so no Box needed here
    };

    struct Expr {
        using Node = std::variant<
            NumberExpr, BoolExpr, StringExpr, ColorExpr, IdentifierExpr,
            MemberRefExpr, SwizzleExpr, UnaryExpr, BinaryExpr, TernaryExpr, CallExpr>;

        Node node;
        SourceSpan span;
    };

    // statement: IDENT '=' expr ';'? 
    // this is gonna be reused for shader_stmt (IDENT ':' type '=' shader_expr ';')
    // It carries a type annotation, `type` is nullopt for a plain node-body statement and set for a shader_stmt.
    struct Statement {
        std::string name;
        std::optional<std::string> type;
        Expr value;
        SourceSpan span;
    };

    /* Fill / stroke sublanguage
     *
     * Same idea as Expr: Fill is a closed variant over the fill forms.
     */

    // solid_fill := color_literal
    struct SolidFill {
        Expr color; // ColorExpr, or a rgb()/rgba()/hsl() CallExpr
    };

    // stop := color_literal ('@' expr)?
    struct GradientStop {
        Expr color;
        std::optional<Expr> position; // nullopt if the '@' position was omitted
    };

    // gradient_fill (linear_gradient form). angle_or_points is either a
    // single angle expr, or an explicit '(' x, y ')' '->' '(' x, y ')'. 
    // exactly one of {angle} or {startX/startY/endX/endY} is populated.
    struct LinearGradientFill {
        std::optional<Expr> angle;
        std::optional<Expr> startX, startY, endX, endY;
        std::vector<GradientStop> stops;
    };

    // gradient_fill (radial_gradient form). radial_spec is either just a
    // radius, or `radius 'at' '(' x, y ')'`, centerX/centerY are nullopt
    // when no explicit center was given (defaults to the node's own center).
    struct RadialGradientFill {
        std::optional<Expr> radius;
        std::optional<Expr> centerX, centerY;
        std::vector<GradientStop> stops;
    };

    // texture_fill := 'texture' '(' import_expr ')'
    struct TextureFill {
        std::string path; // import_expr's STRING contents
    };

    // shader_fill := 'shader' '{' shader_body '}'
    // shader_body := shader_stmt* 'return' shader_expr ';'
    struct ShaderFill {
        std::vector<Statement> statements;  // shader_stmt*  (IDENT ':' type '=' shader_expr ';')
        std::optional<Expr> returnExpr;     // 'return' shader_expr ';' (nullopt if it was missing)
    };

    struct Fill {
        using Node = std::variant<
            SolidFill, LinearGradientFill, RadialGradientFill, TextureFill, ShaderFill>;

        Node node;
        SourceSpan span;
    };

    enum class CapStyle { Butt, Round, Square };
    enum class JoinStyle { Miter, Round, Bevel };

    // stroke_expr := 'none' | '{' 'color' ':' fill_expr ',' 'width' ':' expr (...) '}'
    struct Stroke {
        bool isNone = false;
        std::optional<Fill> color;
        std::optional<Expr> width;
        std::optional<CapStyle> cap;
        std::optional<JoinStyle> join;
        SourceSpan span{};
    };

    /* Page and Node declarations */

    // page_size := preset_size | dimension ',' dimension
    struct PageSize {
        std::optional<std::string> preset; // "A4"/"Letter"/etc or nullopt if size is explicitly set.
        std::optional<Expr> width;         // set when preset is nullopt
        std::optional<Expr> height;        // set when preset is nullopt
    };

    // page_decl := 'page' '{' page_prop* '}'
    struct PageDecl {
        std::optional<PageSize> size;
        std::optional<Fill> background;
        std::optional<Expr> margin;
        SourceSpan span{};
    };

    enum class NodeType { Rect, Circle, Ellipse, Path, Text, Image, Group };

    // properties shared by every node type
    struct CommonProps {
        std::optional<Expr> x, y, width, height, rotation, opacity, z;
        std::optional<Fill> fill;
        std::optional<Stroke> stroke;
        std::optional<bool> visible;
    };

    enum class TextAlign { Left, Center, Right, Justify };

    struct TextProps {
        std::optional<std::string> content;
        std::optional<std::string> font;
        std::optional<Expr> fontSize;
        std::optional<Expr> fontWeightNumber;   // the NUMBER form of font_weight
        std::optional<bool> fontWeightIsBold;
        std::optional<TextAlign> align;
        std::optional<Expr> lineHeight;
    };

    enum class ImageFit { Cover, Contain, Stretch, None };

    struct ImageProps {
        std::string sourcePath; // import_expr's STRING contents
        std::optional<ImageFit> fit;
    };

    // point := '(' expr ',' expr ')'
    struct PathPoint {
        Expr x;
        Expr y;
    };

    struct PathProps {
        std::vector<PathPoint> points; // point_list
        std::optional<bool> closed;
    };

    struct CircleProps {
        std::optional<Expr> radius;
    };

    // node_decl := node_type IDENT? '{' node_body '}'
    // Not a closed value type, just a list of children, so a plain struct
    // with a std::vector<NodeDecl> (legal for an incomplete type) is enough.
    struct NodeDecl {
        NodeType type = NodeType::Rect;
        std::optional<std::string> name;
        SourceSpan span{};

        CommonProps common;

        // Only the member matching `type` is ever populated.
        std::optional<CircleProps> circleProps;
        std::optional<TextProps> textProps;
        std::optional<ImageProps> imageProps;
        std::optional<PathProps> pathProps;

        // group_prop: groups nest other nodes directly
        std::vector<NodeDecl> children;

        std::vector<Statement> statements;
    };

    // document := page_decl node_decl*
    struct Document {
        PageDecl page;
        std::vector<NodeDecl> nodes;
    };

    // std::vector reallocation falls back to *copying* when a move constructor
    // isn't noexcept, and these types are move-only (Box). Fail here, loudly,
    // if a future member ever breaks that.
    static_assert(std::is_nothrow_move_constructible_v<Expr>);
    static_assert(std::is_nothrow_move_constructible_v<Fill>);
    static_assert(std::is_nothrow_move_constructible_v<NodeDecl>);

}
