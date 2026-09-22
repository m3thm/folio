#include "ast/ast_printer.hpp"

#include <charconv>
#include <cstddef>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

// See ast_printer.hpp for the output format and the rules behind it.

namespace {

    using namespace folio;

    // Small formatting helpers

    // Shortest text that parses back to the same double: 40 -> "40", 0.5 -> "0.5".
    std::string formatNumber(double value) {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec != std::errc{}) return std::to_string(value); // can't happen: 64 chars fits any double
        return std::string(buffer, result.ptr);
    }

    std::string_view unitSuffix(Unit unit) {
        switch (unit) {
        case Unit::None:    return "";
        case Unit::Px:      return "px";
        case Unit::Pt:      return "pt";
        case Unit::Mm:      return "mm";
        case Unit::Cm:      return "cm";
        case Unit::In:      return "in";
        case Unit::Percent: return "%";
        }
        return "";
    }

    // Re-escapes a string the lexer already unescaped, so `\n` in the source
    // comes back as `\n` in the dump rather than as a literal line break.
    // (Control characters with no short escape are written as \xNN. That's
    // dump-only notation; the lexer doesn't read it back.)
    std::string quote(std::string_view text) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string out = "\"";
        for (const char c : text) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\x";
                    out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += hex[static_cast<unsigned char>(c) & 0xF];
                }
                else {
                    out += c;
                }
            }
        }
        out += '"';
        return out;
    }

    std::string_view binaryOpSymbol(BinaryOp op) {
        switch (op) {
        case BinaryOp::Add:   return "+";
        case BinaryOp::Sub:   return "-";
        case BinaryOp::Mul:   return "*";
        case BinaryOp::Div:   return "/";
        case BinaryOp::Mod:   return "%";
        case BinaryOp::Lt:    return "<";
        case BinaryOp::Gt:    return ">";
        case BinaryOp::Le:    return "<=";
        case BinaryOp::Ge:    return ">=";
        case BinaryOp::Eq:    return "==";
        case BinaryOp::NotEq: return "!=";
        case BinaryOp::And:   return "&&";
        case BinaryOp::Or:    return "||";
        }
        return "?";
    }

    std::string_view nodeTypeName(NodeType type) {
        switch (type) {
        case NodeType::Rect:    return "rect";
        case NodeType::Circle:  return "circle";
        case NodeType::Ellipse: return "ellipse";
        case NodeType::Path:    return "path";
        case NodeType::Text:    return "text";
        case NodeType::Image:   return "image";
        case NodeType::Group:   return "group";
        }
        return "?";
    }

    std::string_view textAlignName(TextAlign align) {
        switch (align) {
        case TextAlign::Left:    return "left";
        case TextAlign::Center:  return "center";
        case TextAlign::Right:   return "right";
        case TextAlign::Justify: return "justify";
        }
        return "?";
    }

    std::string_view imageFitName(ImageFit fit) {
        switch (fit) {
        case ImageFit::Cover:   return "cover";
        case ImageFit::Contain: return "contain";
        case ImageFit::Stretch: return "stretch";
        case ImageFit::None:    return "none";
        }
        return "?";
    }

    std::string_view capStyleName(CapStyle cap) {
        switch (cap) {
        case CapStyle::Butt:   return "butt";
        case CapStyle::Round:  return "round";
        case CapStyle::Square: return "square";
        }
        return "?";
    }

    std::string_view joinStyleName(JoinStyle join) {
        switch (join) {
        case JoinStyle::Miter: return "miter";
        case JoinStyle::Round: return "round";
        case JoinStyle::Bevel: return "bevel";
        }
        return "?";
    }

    // Expressions

    std::string formatExpr(const Expr& expr);

    // One overload per Expr alternative: std::visit won't compile if a new
    // alternative is added to Expr::Node without being handled here.
    struct ExprFormatter {
        std::string operator()(const NumberExpr& e) const {
            return formatNumber(e.value) + std::string(unitSuffix(e.unit));
        }
        std::string operator()(const BoolExpr& e) const {
            return e.value ? "true" : "false";
        }
        std::string operator()(const StringExpr& e) const {
            return quote(e.value);
        }
        std::string operator()(const ColorExpr& e) const {
            return "#" + e.hex;
        }
        std::string operator()(const IdentifierExpr& e) const {
            return e.name;
        }
        std::string operator()(const MemberRefExpr& e) const {
            switch (e.base) {
            case RefBase::Self:   return "self." + e.property;
            case RefBase::Parent: return "parent." + e.property;
            case RefBase::Page:   return "page." + e.property;
            case RefBase::Named:  return e.baseName + "." + e.property;
            }
            return e.property;
        }
        std::string operator()(const SwizzleExpr& e) const {
            return "(swizzle " + formatExpr(*e.base) + " " + e.components + ")";
        }
        std::string operator()(const UnaryExpr& e) const {
            return std::string("(") + (e.op == UnaryOp::Negate ? "neg " : "not ") + formatExpr(*e.operand) + ")";
        }
        std::string operator()(const BinaryExpr& e) const {
            return "(" + std::string(binaryOpSymbol(e.op)) + " " + formatExpr(*e.left) + " " + formatExpr(*e.right) + ")";
        }
        std::string operator()(const TernaryExpr& e) const {
            return "(? " + formatExpr(*e.condition) + " " + formatExpr(*e.thenBranch) + " " + formatExpr(*e.elseBranch) + ")";
        }
        std::string operator()(const CallExpr& e) const {
            std::string out = e.callee + "(";
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                if (i > 0) out += ", ";
                out += formatExpr(e.args[i]);
            }
            return out + ")";
        }
    };

    std::string formatExpr(const Expr& expr) {
        return std::visit(ExprFormatter{}, expr.node);
    }

    // After a parse error some optionals may be empty; "?" keeps the dump total.
    std::string formatOptionalExpr(const std::optional<Expr>& expr) {
        return expr ? formatExpr(*expr) : "?";
    }

    std::string formatPoint(const Expr& x, const Expr& y) {
        return "(" + formatExpr(x) + ", " + formatExpr(y) + ")";
    }

    std::string formatStops(const std::vector<GradientStop>& stops) {
        std::string out = "[";
        for (std::size_t i = 0; i < stops.size(); ++i) {
            if (i > 0) out += ", ";
            out += formatExpr(stops[i].color);
            if (stops[i].position) out += " @ " + formatExpr(*stops[i].position);
        }
        return out + "]";
    }

    // The printer: owns indentation and the line-oriented output.

    class Printer {
    public:
        explicit Printer(std::ostream& out) : out(out) {}

        void document(const Document& document) {
            line(0, "document");
            pageDecl(document.page, 1);
            for (const NodeDecl& node : document.nodes) nodeDecl(node, 1);
        }

        void line(int depth, std::string_view text) {
            out << std::string(static_cast<std::size_t>(depth) * 2, ' ') << text << '\n';
        }

        // `label: <fill>`; a shader is the one fill that needs several lines.
        void fillProperty(int depth, std::string_view label, const Fill& fill);

    private:
        std::ostream& out;

        void pageDecl(const PageDecl& page, int depth);
        void nodeDecl(const NodeDecl& node, int depth);
        void strokeProperty(const Stroke& stroke, int depth);

        void exprProperty(int depth, std::string_view label, const std::optional<Expr>& value) {
            if (value) line(depth, std::string(label) + ": " + formatExpr(*value));
        }
    };

    // One overload per Fill alternative, same exhaustiveness guarantee as ExprFormatter.
    struct FillPrinter {
        Printer& printer;
        int depth;
        std::string_view label;

        void emit(const std::string& value) const {
            printer.line(depth, std::string(label) + ": " + value);
        }

        void operator()(const SolidFill& f) const {
            emit("solid(" + formatExpr(f.color) + ")");
        }

        void operator()(const LinearGradientFill& f) const {
            std::string args;
            if (f.angle) {
                args = "angle: " + formatExpr(*f.angle);
            }
            else if (f.startX || f.startY || f.endX || f.endY) {
                args = "from: (" + formatOptionalExpr(f.startX) + ", " + formatOptionalExpr(f.startY) + "), " +
                       "to: (" + formatOptionalExpr(f.endX) + ", " + formatOptionalExpr(f.endY) + ")";
            }
            if (!args.empty()) args += ", ";
            emit("linear_gradient(" + args + "stops: " + formatStops(f.stops) + ")");
        }

        void operator()(const RadialGradientFill& f) const {
            std::string args;
            if (f.radius) args += "radius: " + formatExpr(*f.radius) + ", ";
            if (f.centerX || f.centerY) {
                args += "center: (" + formatOptionalExpr(f.centerX) + ", " + formatOptionalExpr(f.centerY) + "), ";
            }
            emit("radial_gradient(" + args + "stops: " + formatStops(f.stops) + ")");
        }

        void operator()(const TextureFill& f) const {
            emit("texture(import(" + quote(f.path) + "))");
        }

        void operator()(const ShaderFill& f) const {
            printer.line(depth, std::string(label) + ": shader");
            for (const Statement& statement : f.statements) {
                std::string text = statement.name;
                if (statement.type) text += ": " + *statement.type;
                text += " = " + formatExpr(statement.value);
                printer.line(depth + 1, text);
            }
            if (f.returnExpr) printer.line(depth + 1, "return " + formatExpr(*f.returnExpr));
        }
    };

    void Printer::fillProperty(int depth, std::string_view label, const Fill& fill) {
        std::visit(FillPrinter{ *this, depth, label }, fill.node);
    }

    void Printer::pageDecl(const PageDecl& page, int depth) {
        line(depth, "page");
        const int d = depth + 1;

        if (page.size) {
            const PageSize& size = *page.size;
            if (size.preset) {
                line(d, "size: " + *size.preset);
            }
            else {
                line(d, "size: " + formatOptionalExpr(size.width) + ", " + formatOptionalExpr(size.height));
            }
        }
        if (page.background) fillProperty(d, "background", *page.background);
        exprProperty(d, "margin", page.margin);
    }

    void Printer::strokeProperty(const Stroke& stroke, int depth) {
        if (stroke.isNone) {
            line(depth, "stroke: none");
            return;
        }
        line(depth, "stroke");
        const int d = depth + 1;
        if (stroke.color) fillProperty(d, "color", *stroke.color);
        exprProperty(d, "width", stroke.width);
        if (stroke.cap) line(d, "cap: " + std::string(capStyleName(*stroke.cap)));
        if (stroke.join) line(d, "join: " + std::string(joinStyleName(*stroke.join)));
    }

    // Property order here is the canonical order: change it and every golden
    // file changes with it. The AST doesn't remember source order, so this is
    // also the only order a dump can ever have.
    void Printer::nodeDecl(const NodeDecl& node, int depth) {
        std::string header(nodeTypeName(node.type));
        if (node.name) header += " " + *node.name;
        line(depth, header);
        const int d = depth + 1;

        // Properties every node has.
        const CommonProps& common = node.common;
        exprProperty(d, "x", common.x);
        exprProperty(d, "y", common.y);
        exprProperty(d, "width", common.width);
        exprProperty(d, "height", common.height);
        exprProperty(d, "rotation", common.rotation);
        exprProperty(d, "opacity", common.opacity);
        exprProperty(d, "z", common.z);
        if (common.fill) fillProperty(d, "fill", *common.fill);
        if (common.stroke) strokeProperty(*common.stroke, d);
        if (common.visible) line(d, std::string("visible: ") + (*common.visible ? "true" : "false"));

        // Properties belonging to one node type (only the matching struct is ever set).
        if (node.circleProps) {
            exprProperty(d, "radius", node.circleProps->radius);
        }
        if (node.textProps) {
            const TextProps& text = *node.textProps;
            if (text.content) line(d, "content: " + quote(*text.content));
            if (text.font) line(d, "font: " + quote(*text.font));
            exprProperty(d, "font_size", text.fontSize);
            if (text.fontWeightIsBold) {
                line(d, std::string("font_weight: ") + (*text.fontWeightIsBold ? "bold" : "normal"));
            }
            else {
                exprProperty(d, "font_weight", text.fontWeightNumber);
            }
            if (text.align) line(d, "align: " + std::string(textAlignName(*text.align)));
            exprProperty(d, "line_height", text.lineHeight);
        }
        if (node.imageProps) {
            const ImageProps& image = *node.imageProps;
            // The AST can't tell a missing `source` from `import("")`, so an empty path prints nothing.
            if (!image.sourcePath.empty()) line(d, "source: import(" + quote(image.sourcePath) + ")");
            if (image.fit) line(d, "fit: " + std::string(imageFitName(*image.fit)));
        }
        if (node.pathProps) {
            const PathProps& path = *node.pathProps;
            if (!path.points.empty()) {
                std::string points = "points: [";
                for (std::size_t i = 0; i < path.points.size(); ++i) {
                    if (i > 0) points += ", ";
                    points += formatPoint(path.points[i].x, path.points[i].y);
                }
                line(d, points + "]");
            }
            if (path.closed) line(d, std::string("closed: ") + (*path.closed ? "true" : "false"));
        }

        // `name = expr` local variables, in source order.
        for (const Statement& statement : node.statements) {
            line(d, "local " + statement.name + " = " + formatExpr(statement.value));
        }

        for (const NodeDecl& child : node.children) nodeDecl(child, d);
    }

}

namespace folio {

    void printAst(std::ostream& os, const Document& document) {
        Printer(os).document(document);
    }

    std::string printAst(const Document& document) {
        std::ostringstream os;
        printAst(os, document);
        return os.str();
    }

}
