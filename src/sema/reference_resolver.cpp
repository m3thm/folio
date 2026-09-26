#include "sema/reference_resolver.hpp"
#include "sema/dependency_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace folio {

    //,-- Vertex: identity for one (node, property) / page-property / statement-local,--
    //
    // Kept out of the header (see reference_resolver.hpp) so callers only ever
    // see ReferenceResolver's query API, never the graph itself.

    struct Vertex {
        enum class Kind { NodeProperty, PageProperty, Local };
        Kind kind;
        const NodeDecl* node = nullptr;                   // NodeProperty, Local (owning node)
        PropertyKind property = PropertyKind::X;           // NodeProperty
        PageProperty pageProperty = PageProperty::Width;   // PageProperty
        const Statement* statement = nullptr;              // Local

        friend bool operator==(const Vertex&, const Vertex&) = default;
    };

    Vertex nodeVertex(const NodeDecl* node, PropertyKind property) {
        return Vertex{Vertex::Kind::NodeProperty, node, property, {}, nullptr};
    }
    Vertex pageVertex(PageProperty property) {
        return Vertex{Vertex::Kind::PageProperty, nullptr, {}, property, nullptr};
    }
    Vertex localVertex(const NodeDecl* node, const Statement* statement) {
        return Vertex{Vertex::Kind::Local, node, {}, {}, statement};
    }

}

template <>
struct std::hash<folio::Vertex> {
    std::size_t operator()(const folio::Vertex& v) const noexcept {
        std::size_t h = std::hash<int>{}(static_cast<int>(v.kind));
        auto mix = [&h](std::size_t x) { h ^= x + 0x9e3779b9 + (h << 6) + (h >> 2); };
        mix(std::hash<const void*>{}(v.node));
        mix(std::hash<int>{}(static_cast<int>(v.property)));
        mix(std::hash<int>{}(static_cast<int>(v.pageProperty)));
        mix(std::hash<const void*>{}(v.statement));
        return h;
    }
};

namespace {

    using namespace folio;

    std::string nodeDisplayName(const NodeDecl* node) {
        if (!node) return "page";
        return node->name ? *node->name : "<unnamed>";
    }

    std::string propertyKindName(PropertyKind kind) {
        switch (kind) {
        case PropertyKind::X: return "x";
        case PropertyKind::Y: return "y";
        case PropertyKind::Width: return "width";
        case PropertyKind::Height: return "height";
        case PropertyKind::Rotation: return "rotation";
        case PropertyKind::Opacity: return "opacity";
        case PropertyKind::Z: return "z";
        case PropertyKind::Radius: return "radius";
        }
        return "?";
    }

    std::string pagePropertyName(PageProperty property) {
        switch (property) {
        case PageProperty::Width: return "width";
        case PageProperty::Height: return "height";
        case PageProperty::Margin: return "margin";
        }
        return "?";
    }

    std::string describeVertex(const Vertex& v) {
        switch (v.kind) {
        case Vertex::Kind::PageProperty: return "page." + pagePropertyName(v.pageProperty);
        case Vertex::Kind::NodeProperty: return nodeDisplayName(v.node) + "." + propertyKindName(v.property);
        case Vertex::Kind::Local: return nodeDisplayName(v.node) + "." + (v.statement ? v.statement->name : "?");
        }
        return "?";
    }

    std::optional<PropertyKind> propertyKindFromName(const std::string& name) {
        if (name == "x") return PropertyKind::X;
        if (name == "y") return PropertyKind::Y;
        if (name == "width") return PropertyKind::Width;
        if (name == "height") return PropertyKind::Height;
        if (name == "rotation") return PropertyKind::Rotation;
        if (name == "opacity") return PropertyKind::Opacity;
        if (name == "z") return PropertyKind::Z;
        if (name == "radius") return PropertyKind::Radius;
        return std::nullopt;
    }

    std::optional<PageProperty> pagePropertyFromName(const std::string& name) {
        if (name == "width") return PageProperty::Width;
        if (name == "height") return PageProperty::Height;
        if (name == "margin") return PageProperty::Margin;
        return std::nullopt;
    }

    constexpr std::array<PropertyKind, 8> kAllPropertyKinds = {
        PropertyKind::X, PropertyKind::Y, PropertyKind::Width, PropertyKind::Height,
        PropertyKind::Rotation, PropertyKind::Opacity, PropertyKind::Z, PropertyKind::Radius,
    };

    bool isLengthKind(PropertyKind kind) {
        return kind != PropertyKind::Rotation && kind != PropertyKind::Opacity && kind != PropertyKind::Z;
    }

    // Whether a bare/absolute number in this vertex's expr means points (2.1:
    // x/y/width/height/radius) or a raw scalar (rotation/opacity/z).
    enum class NumericKind { Length, Plain };

    // 2.1: `self.`/`parent.`/`page.`/`IDENT.<prop>`. Scoping (sibling-or-ancestor,
    // never a descendant) is enforced by SymbolTable::resolve itself; a name it
    // can't find and a name that's out of scope both report the same "unknown
    // node" diagnostic here (see symbol_table.hpp).
    std::optional<Vertex> resolveMemberRef(const MemberRefExpr& ref, const NodeDecl* self, SourceSpan span,
        const SymbolTable& symbols, const DimensionResolver& dims, DiagnosticsEngine& engine) {

        bool isPage = false;
        const NodeDecl* target = nullptr;
        switch (ref.base) {
        case RefBase::Self:
            target = self;
            break;
        case RefBase::Parent:
            target = DimensionResolver::resolutionBasis(self, symbols);
            isPage = (target == nullptr);
            break;
        case RefBase::Page:
            isPage = true;
            break;
        case RefBase::Named:
            target = symbols.resolve(self, ref.baseName);
            if (!target) {
                engine.error(span, "unknown node '" + ref.baseName + "'");
                return std::nullopt;
            }
            break;
        }

        if (isPage) {
            auto pageProp = pagePropertyFromName(ref.property);
            if (!pageProp) {
                engine.error(span, "'page' has no property '" + ref.property + "'");
                return std::nullopt;
            }
            return pageVertex(*pageProp);
        }

        auto prop = propertyKindFromName(ref.property);
        if (!prop) {
            engine.error(span, "'" + ref.property + "' is not a resolved property that can be referenced");
            return std::nullopt;
        }
        if (*prop == PropertyKind::Radius && target->type != NodeType::Circle) {
            engine.error(span, "'radius' is only defined on circle nodes");
            return std::nullopt;
        }

        auto resolved = dims.propertyOf(target, *prop);
        if (resolved && resolved->kind == ResolvedProperty::Kind::Automatic) {
            engine.error(span, "can't reference '" + ref.property + "': its size is automatic (4.4)");
            return std::nullopt;
        }
        return nodeVertex(target, *prop);
    }

    // Bare IDENT: a `statement` local (2.1, "IDENT ... reference to a local
    // variable declared by statement"). Visible only to positions *after* its
    // declaration in the same node body (section 2's note under `statement`);
    // enforced here by comparing source spans, since the AST doesn't otherwise
    // preserve how properties and statements interleave.
    std::optional<Vertex> resolveIdentifier(const IdentifierExpr& ident, const NodeDecl* self, SourceSpan span,
        DiagnosticsEngine& engine) {
        const Statement* best = nullptr;
        for (const Statement& s : self->statements) {
            if (s.name != ident.name || !(s.span.start < span.start)) continue;
            if (!best || s.span.start > best->span.start) best = &s;
        }
        if (!best) {
            engine.error(span, "unknown local variable '" + ident.name + "'");
            return std::nullopt;
        }
        return localVertex(self, best);
    }

    // Walks `expr`, registering an edge from `owner` to every reference, `%`,
    // and statement local it uses. Structural only, no values are read, so
    // this can run before anything is evaluated (a group's own width may
    // itself be a reference; see dimension_resolver.hpp). Remembers each
    // reference's resolved target in `refTarget` so evaluate() (below) doesn't
    // need to re-resolve it, and re-diagnose it a second time.
    void collectEdges(const Expr& expr, Vertex owner, const NodeDecl* owningNode,
        const std::vector<PercentAxis>& percentAxes, bool diagnoseIllegalPercent,
        DependencyGraph<Vertex>& graph, const SymbolTable& symbols, const DimensionResolver& dims,
        DiagnosticsEngine& engine, std::unordered_map<const Expr*, Vertex>& refTarget) {

        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, NumberExpr>) {
                if (node.unit != Unit::Percent) return;
                if (percentAxes.empty()) {
                    // Illegal here; dimension_resolver already diagnosed this for
                    // ordinary node properties (see checkPercentLegality), only
                    // statement locals reach this pass undiagnosed.
                    if (diagnoseIllegalPercent) engine.error(expr.span, "'%' is not permitted here");
                    return;
                }
                const NodeDecl* basis = DimensionResolver::resolutionBasis(owningNode, symbols);
                for (PercentAxis axis : percentAxes) {
                    Vertex target = axis == PercentAxis::Width
                        ? (basis ? nodeVertex(basis, PropertyKind::Width) : pageVertex(PageProperty::Width))
                        : (basis ? nodeVertex(basis, PropertyKind::Height) : pageVertex(PageProperty::Height));
                    graph.addEdge(owner, target);
                }
            }
            else if constexpr (std::is_same_v<T, MemberRefExpr>) {
                if (auto target = resolveMemberRef(node, owningNode, expr.span, symbols, dims, engine)) {
                    refTarget[&expr] = *target;
                    graph.addEdge(owner, *target);
                }
            }
            else if constexpr (std::is_same_v<T, IdentifierExpr>) {
                if (auto target = resolveIdentifier(node, owningNode, expr.span, engine)) {
                    refTarget[&expr] = *target;
                    graph.addEdge(owner, *target);
                }
            }
            else if constexpr (std::is_same_v<T, UnaryExpr>) {
                collectEdges(*node.operand, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
            }
            else if constexpr (std::is_same_v<T, BinaryExpr>) {
                collectEdges(*node.left, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
                collectEdges(*node.right, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
            }
            else if constexpr (std::is_same_v<T, TernaryExpr>) {
                collectEdges(*node.condition, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
                collectEdges(*node.thenBranch, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
                collectEdges(*node.elseBranch, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
            }
            else if constexpr (std::is_same_v<T, SwizzleExpr>) {
                collectEdges(*node.base, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
            }
            else if constexpr (std::is_same_v<T, CallExpr>) {
                for (const Expr& arg : node.args)
                    collectEdges(arg, owner, owningNode, percentAxes, diagnoseIllegalPercent, graph, symbols, dims, engine, refTarget);
            }
            // BoolExpr/StringExpr/ColorExpr: leaves; not valid in a numeric
            // property at all, but that's diagnosed by evaluate(), not here.
        }, expr.node);
    }

    // Evaluates `expr` to a double, given every dependency's value already in
    // `values` (guaranteed by evaluating in topological order). `kind` says
    // whether a bare/absolute number means points or a raw scalar (2.1);
    // `percentAxes` is the same set collectEdges was given for this vertex.
    double evaluate(const Expr& expr, const NodeDecl* owningNode, NumericKind kind,
        const std::vector<PercentAxis>& percentAxes, const SymbolTable& symbols,
        const std::unordered_map<Vertex, double>& values, const std::unordered_map<const Expr*, Vertex>& refTarget,
        DiagnosticsEngine& engine) {

        auto lookup = [&](const Vertex& v) {
            auto it = values.find(v);
            return it != values.end() ? it->second : 0.0; // already diagnosed upstream (Errored/cycle); propagate quietly
        };

        return std::visit([&](const auto& node) -> double {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, NumberExpr>) {
                if (node.unit == Unit::Percent) {
                    if (percentAxes.empty()) return node.value; // illegal; already diagnosed, just don't crash
                    const NodeDecl* basis = DimensionResolver::resolutionBasis(owningNode, symbols);
                    double basisWidth = lookup(basis ? nodeVertex(basis, PropertyKind::Width) : pageVertex(PageProperty::Width));
                    double basisHeight = lookup(basis ? nodeVertex(basis, PropertyKind::Height) : pageVertex(PageProperty::Height));
                    if (percentAxes.size() == 2) return resolveRadiusPercent(node.value, basisWidth, basisHeight);
                    return resolvePercent(node.value, percentAxes.front(), basisWidth, basisHeight);
                }
                return kind == NumericKind::Length ? toPoints(node.value, node.unit) : node.value;
            }
            else if constexpr (std::is_same_v<T, UnaryExpr>) {
                double v = evaluate(*node.operand, owningNode, kind, percentAxes, symbols, values, refTarget, engine);
                return node.op == UnaryOp::Negate ? -v : (v == 0.0 ? 1.0 : 0.0);
            }
            else if constexpr (std::is_same_v<T, BinaryExpr>) {
                double l = evaluate(*node.left, owningNode, kind, percentAxes, symbols, values, refTarget, engine);
                double r = evaluate(*node.right, owningNode, kind, percentAxes, symbols, values, refTarget, engine);
                switch (node.op) {
                case BinaryOp::Add: return l + r;
                case BinaryOp::Sub: return l - r;
                case BinaryOp::Mul: return l * r;
                case BinaryOp::Div:
                    if (r == 0.0) { engine.error(expr.span, "division by zero"); return 0.0; }
                    return l / r;
                case BinaryOp::Mod:
                    if (r == 0.0) { engine.error(expr.span, "division by zero"); return 0.0; }
                    return std::fmod(l, r);
                case BinaryOp::Lt: return l < r ? 1.0 : 0.0;
                case BinaryOp::Gt: return l > r ? 1.0 : 0.0;
                case BinaryOp::Le: return l <= r ? 1.0 : 0.0;
                case BinaryOp::Ge: return l >= r ? 1.0 : 0.0;
                case BinaryOp::Eq: return l == r ? 1.0 : 0.0;
                case BinaryOp::NotEq: return l != r ? 1.0 : 0.0;
                case BinaryOp::And: return (l != 0.0 && r != 0.0) ? 1.0 : 0.0;
                case BinaryOp::Or: return (l != 0.0 || r != 0.0) ? 1.0 : 0.0;
                }
                return 0.0;
            }
            else if constexpr (std::is_same_v<T, TernaryExpr>) {
                double cond = evaluate(*node.condition, owningNode, kind, percentAxes, symbols, values, refTarget, engine);
                return cond != 0.0
                    ? evaluate(*node.thenBranch, owningNode, kind, percentAxes, symbols, values, refTarget, engine)
                    : evaluate(*node.elseBranch, owningNode, kind, percentAxes, symbols, values, refTarget, engine);
            }
            else if constexpr (std::is_same_v<T, MemberRefExpr> || std::is_same_v<T, IdentifierExpr>) {
                auto it = refTarget.find(&expr);
                return it != refTarget.end() ? lookup(it->second) : 0.0; // unresolved; already diagnosed in collectEdges
            }
            else {
                // BoolExpr, StringExpr, ColorExpr, SwizzleExpr, CallExpr: none of
                // these are reachable from `expr`'s own grammar (2.1's primary_num
                // doesn't list them), defensive, in case that ever changes.
                engine.error(expr.span, "expected a number");
                return 0.0;
            }
        }, expr.node);
    }

}

namespace folio {

    struct ReferenceResolver::Impl {
        DependencyGraph<Vertex> graph;
        std::unordered_map<Vertex, double> values;
        std::unordered_map<Vertex, const Expr*> pendingExpr;
        std::unordered_map<Vertex, const NodeDecl*> pathVertices; // DerivedFromPoints width/height -> owning path node
        std::unordered_map<const Expr*, Vertex> refTarget;

        void collectNode(const NodeDecl& node, const SymbolTable& symbols, const DimensionResolver& dims,
            DiagnosticsEngine& engine) {

            for (const Statement& s : node.statements) {
                Vertex v = localVertex(&node, &s);
                graph.vertex(v);
                pendingExpr[v] = &s.value;
                collectEdges(s.value, v, &node, {}, /*diagnoseIllegalPercent=*/true, graph, symbols, dims, engine, refTarget);
            }

            for (PropertyKind kind : kAllPropertyKinds) {
                auto resolved = dims.propertyOf(&node, kind);
                if (!resolved) continue;

                Vertex v = nodeVertex(&node, kind);
                graph.vertex(v);

                switch (resolved->kind) {
                case ResolvedProperty::Kind::Explicit:
                case ResolvedProperty::Kind::Default:
                case ResolvedProperty::Kind::Derived: {
                    pendingExpr[v] = resolved->expr;
                    collectEdges(*resolved->expr, v, &node, percentAxesFor(kind), /*diagnoseIllegalPercent=*/false,
                        graph, symbols, dims, engine, refTarget);
                    break;
                }
                case ResolvedProperty::Kind::DerivedFromPoints:
                    pathVertices[v] = &node;
                    for (const PathPoint& point : node.pathProps->points) {
                        collectEdges(point.x, v, &node, {}, false, graph, symbols, dims, engine, refTarget);
                        collectEdges(point.y, v, &node, {}, false, graph, symbols, dims, engine, refTarget);
                    }
                    break;
                case ResolvedProperty::Kind::Automatic:
                case ResolvedProperty::Kind::Errored:
                    break; // no expr, no edges: referencing Automatic is diagnosed in resolveMemberRef; Errored was already diagnosed by dimension_resolver
                }
            }

            if (node.type == NodeType::Group)
                for (const NodeDecl& child : node.children) collectNode(child, symbols, dims, engine);
        }

        double evaluateBoundingBoxAxis(const NodeDecl& path, bool widthAxis, const SymbolTable& symbols,
            DiagnosticsEngine& engine) {
            double lo = std::numeric_limits<double>::infinity();
            double hi = -std::numeric_limits<double>::infinity();
            for (const PathPoint& point : path.pathProps->points) {
                const Expr& coord = widthAxis ? point.x : point.y;
                double v = evaluate(coord, &path, NumericKind::Length, {}, symbols, values, refTarget, engine);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            return hi - lo;
        }
    };

    ReferenceResolver::ReferenceResolver() : impl(std::make_unique<Impl>()) {}
    ReferenceResolver::ReferenceResolver(ReferenceResolver&&) noexcept = default;
    ReferenceResolver& ReferenceResolver::operator=(ReferenceResolver&&) noexcept = default;
    ReferenceResolver::~ReferenceResolver() = default;

    std::optional<ReferenceResolver> ReferenceResolver::build(const Document& document, const SymbolTable& symbols,
        const DimensionResolver& dims, DiagnosticsEngine& engine) {

        ReferenceResolver resolver;
        Impl& impl = *resolver.impl;

        // Page vertices: always-absolute roots (4.1), resolved eagerly, no graph needed.
        impl.graph.vertex(pageVertex(PageProperty::Width));
        impl.graph.vertex(pageVertex(PageProperty::Height));
        impl.graph.vertex(pageVertex(PageProperty::Margin));

        auto pageBox = resolvePageContentBox(document.page, engine);
        impl.values[pageVertex(PageProperty::Width)] = pageBox ? pageBox->width : 0.0;
        impl.values[pageVertex(PageProperty::Height)] = pageBox ? pageBox->height : 0.0;
        double margin = 0.0;
        if (document.page.margin) {
            if (const auto* m = std::get_if<NumberExpr>(&document.page.margin->node)) margin = toPoints(m->value, m->unit);
        }
        impl.values[pageVertex(PageProperty::Margin)] = margin;

        for (const NodeDecl& node : document.nodes) impl.collectNode(node, symbols, dims, engine);

        auto sorted = impl.graph.topoSort();
        if (auto* cycle = std::get_if<DependencyGraph<Vertex>::Cycle>(&sorted)) {
            std::string path;
            for (std::size_t i = 0; i < cycle->path.size(); ++i) {
                if (i) path += " -> ";
                path += describeVertex(impl.graph.keyOf(cycle->path[i]));
            }
            engine.error(SourceSpan{}, "circular dependency: " + path);
            return std::nullopt;
        }

        for (auto id : std::get<std::vector<DependencyGraph<Vertex>::VertexId>>(sorted)) {
            const Vertex& v = impl.graph.keyOf(id);
            if (v.kind == Vertex::Kind::PageProperty) continue; // already seeded

            if (auto pathIt = impl.pathVertices.find(v); pathIt != impl.pathVertices.end()) {
                impl.values[v] = impl.evaluateBoundingBoxAxis(*pathIt->second, v.property == PropertyKind::Width, symbols, engine);
                continue;
            }

            auto exprIt = impl.pendingExpr.find(v);
            if (exprIt == impl.pendingExpr.end()) continue; // Automatic/Errored: no value to compute

            NumericKind kind = v.kind == Vertex::Kind::NodeProperty && !isLengthKind(v.property)
                ? NumericKind::Plain : NumericKind::Length;
            std::vector<PercentAxis> axes = v.kind == Vertex::Kind::NodeProperty ? percentAxesFor(v.property) : std::vector<PercentAxis>{};
            impl.values[v] = evaluate(*exprIt->second, v.node, kind, axes, symbols, impl.values, impl.refTarget, engine);
        }

        return resolver;
    }

    std::optional<double> ReferenceResolver::valueOf(const NodeDecl* node, PropertyKind kind) const {
        auto it = impl->values.find(nodeVertex(node, kind));
        return it != impl->values.end() ? std::optional(it->second) : std::nullopt;
    }

    std::optional<double> ReferenceResolver::pageValueOf(PageProperty property) const {
        auto it = impl->values.find(pageVertex(property));
        return it != impl->values.end() ? std::optional(it->second) : std::nullopt;
    }

}
