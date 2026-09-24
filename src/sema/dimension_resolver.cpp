#include "sema/dimension_resolver.hpp"

#include <string>
#include <type_traits>
#include <variant>

namespace {

    using namespace folio;

    Expr makeNumber(double value, Unit unit, SourceSpan span) {
        return Expr{NumberExpr{value, unit}, span};
    }

    Expr makeSelfRef(std::string property, SourceSpan span) {
        return Expr{MemberRefExpr{RefBase::Self, "", std::move(property)}, span};
    }

    Expr makeBinary(BinaryOp op, Expr left, Expr right, SourceSpan span) {
        return Expr{BinaryExpr{op, Box<Expr>(std::move(left)), Box<Expr>(std::move(right))}, span};
    }

    // First NumberExpr using `%` found anywhere in `expr`'s tree, or nullptr.
    const Expr* containsPercent(const Expr& expr) {
        return std::visit([&](const auto& node) -> const Expr* {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, NumberExpr>) {
                return node.unit == Unit::Percent ? &expr : nullptr;
            }
            else if constexpr (std::is_same_v<T, SwizzleExpr>) {
                return containsPercent(*node.base);
            }
            else if constexpr (std::is_same_v<T, UnaryExpr>) {
                return containsPercent(*node.operand);
            }
            else if constexpr (std::is_same_v<T, BinaryExpr>) {
                if (const Expr* found = containsPercent(*node.left)) return found;
                return containsPercent(*node.right);
            }
            else if constexpr (std::is_same_v<T, TernaryExpr>) {
                if (const Expr* found = containsPercent(*node.condition)) return found;
                if (const Expr* found = containsPercent(*node.thenBranch)) return found;
                return containsPercent(*node.elseBranch);
            }
            else if constexpr (std::is_same_v<T, CallExpr>) {
                for (const Expr& arg : node.args) {
                    if (const Expr* found = containsPercent(arg)) return found;
                }
                return nullptr;
            }
            else {
                return nullptr; // BoolExpr, StringExpr, ColorExpr, IdentifierExpr, MemberRefExpr: no children
            }
        }, expr.node);
    }

    // `%` is legal only on x/y/width/height/radius (LANGUAGE-SPEC.md 2.1); every
    // other property using it is a semantic error, not just font_size/line_height.
    void checkOne(DiagnosticsEngine& engine, std::string_view propName, const Expr* expr, bool allowed) {
        if (!expr || allowed) return;
        if (const Expr* found = containsPercent(*expr)) {
            engine.error(found->span, "'%' is not permitted on property '" + std::string(propName) + "'");
        }
    }

}

namespace folio {

    double toPoints(double value, Unit unit) {
        switch (unit) {
        case Unit::None:    return value;
        case Unit::Pt:      return value;
        case Unit::Px:      return value * 0.75;
        case Unit::Mm:      return value * (72.0 / 25.4);
        case Unit::Cm:      return value * (72.0 / 2.54);
        case Unit::In:      return value * 72.0;
        case Unit::Percent: return value; // precondition violated -- see header
        }
        return value;
    }

    std::vector<PercentAxis> percentAxesFor(PropertyKind kind) {
        switch (kind) {
        case PropertyKind::X:
        case PropertyKind::Width:
            return {PercentAxis::Width};
        case PropertyKind::Y:
        case PropertyKind::Height:
            return {PercentAxis::Height};
        case PropertyKind::Radius:
            return {PercentAxis::Width, PercentAxis::Height};
        case PropertyKind::Rotation:
        case PropertyKind::Opacity:
        case PropertyKind::Z:
            return {};
        }
        return {};
    }

    double resolvePercent(double value, PercentAxis axis, double basisWidth, double basisHeight) {
        return value / 100.0 * (axis == PercentAxis::Width ? basisWidth : basisHeight);
    }

    double resolveRadiusPercent(double value, double basisWidth, double basisHeight) {
        double smaller = basisWidth < basisHeight ? basisWidth : basisHeight;
        return value / 100.0 * (smaller / 2.0);
    }

    const Expr* DimensionResolver::synth(Expr expr) {
        synthesized.push_back(std::move(expr));
        return &synthesized.back();
    }

    void DimensionResolver::checkPercentLegality(const NodeDecl& node, DiagnosticsEngine& engine) {
        auto opt = [](const std::optional<Expr>& e) { return e ? &*e : nullptr; };

        checkOne(engine, "x", opt(node.common.x), true);
        checkOne(engine, "y", opt(node.common.y), true);
        checkOne(engine, "width", opt(node.common.width), true);
        checkOne(engine, "height", opt(node.common.height), true);
        checkOne(engine, "rotation", opt(node.common.rotation), false);
        checkOne(engine, "opacity", opt(node.common.opacity), false);
        checkOne(engine, "z", opt(node.common.z), false);

        if (node.circleProps) checkOne(engine, "radius", opt(node.circleProps->radius), true);

        if (node.textProps) {
            checkOne(engine, "font_size", opt(node.textProps->fontSize), false);
            checkOne(engine, "line_height", opt(node.textProps->lineHeight), false);
        }

        if (node.common.stroke && !node.common.stroke->isNone) {
            checkOne(engine, "stroke width", opt(node.common.stroke->width), false);
        }

        if (node.pathProps) {
            for (const PathPoint& point : node.pathProps->points) {
                checkOne(engine, "path point", &point.x, false);
                checkOne(engine, "path point", &point.y, false);
            }
        }
    }

    void DimensionResolver::visitNode(const NodeDecl& node, DiagnosticsEngine& engine) {
        checkPercentLegality(node, engine);

        auto& slots = resolved[&node];

        auto setExplicitOr = [&](PropertyKind kind, const std::optional<Expr>& value, Expr defaultExpr) {
            std::size_t idx = static_cast<std::size_t>(kind);
            slots[idx] = value
                ? ResolvedProperty{ResolvedProperty::Kind::Explicit, &*value}
                : ResolvedProperty{ResolvedProperty::Kind::Default, synth(std::move(defaultExpr))};
        };

        setExplicitOr(PropertyKind::X, node.common.x, makeNumber(0, Unit::None, node.span));
        setExplicitOr(PropertyKind::Y, node.common.y, makeNumber(0, Unit::None, node.span));
        setExplicitOr(PropertyKind::Rotation, node.common.rotation, makeNumber(0, Unit::None, node.span));
        setExplicitOr(PropertyKind::Opacity, node.common.opacity, makeNumber(1, Unit::None, node.span));
        setExplicitOr(PropertyKind::Z, node.common.z, makeNumber(0, Unit::None, node.span));

        // width/height (+ radius for circle): node-type specific, LANGUAGE-SPEC.md 4.4's size table.
        switch (node.type) {
        case NodeType::Rect:
        case NodeType::Ellipse: {
            auto require = [&](PropertyKind kind, const std::optional<Expr>& value, const char* name) {
                std::size_t idx = static_cast<std::size_t>(kind);
                if (value) {
                    slots[idx] = ResolvedProperty{ResolvedProperty::Kind::Explicit, &*value};
                }
                else {
                    engine.error(node.span, std::string("'") + name + "' is required on this node");
                    slots[idx] = ResolvedProperty{ResolvedProperty::Kind::Errored, nullptr};
                }
            };
            require(PropertyKind::Width, node.common.width, "width");
            require(PropertyKind::Height, node.common.height, "height");
            break;
        }
        case NodeType::Group: {
            setExplicitOr(PropertyKind::Width, node.common.width, makeNumber(100, Unit::Percent, node.span));
            setExplicitOr(PropertyKind::Height, node.common.height, makeNumber(100, Unit::Percent, node.span));
            break;
        }
        case NodeType::Circle: {
            const std::optional<Expr>* radius = node.circleProps ? &node.circleProps->radius : nullptr;
            bool hasRadius = radius && radius->has_value();
            std::size_t radiusIdx = static_cast<std::size_t>(PropertyKind::Radius);
            if (hasRadius) {
                slots[radiusIdx] = ResolvedProperty{ResolvedProperty::Kind::Explicit, &**radius};
            }
            else {
                engine.error(node.span, "'radius' is required on a circle node");
                slots[radiusIdx] = ResolvedProperty{ResolvedProperty::Kind::Errored, nullptr};
            }

            auto deriveFromRadius = [&](PropertyKind kind, const std::optional<Expr>& value, const char* name) {
                std::size_t idx = static_cast<std::size_t>(kind);
                if (value) {
                    engine.error(value->span, std::string("'") + name + "' can't be set on a circle; use 'radius'");
                    slots[idx] = ResolvedProperty{ResolvedProperty::Kind::Errored, nullptr};
                }
                else {
                    Expr derived = makeBinary(BinaryOp::Mul, makeNumber(2, Unit::None, node.span),
                        makeSelfRef("radius", node.span), node.span);
                    slots[idx] = ResolvedProperty{ResolvedProperty::Kind::Derived, synth(std::move(derived))};
                }
            };
            deriveFromRadius(PropertyKind::Width, node.common.width, "width");
            deriveFromRadius(PropertyKind::Height, node.common.height, "height");
            break;
        }
        case NodeType::Path: {
            bool hasPoints = node.pathProps && !node.pathProps->points.empty();
            if (!hasPoints) engine.error(node.span, "'points' is required on a path node");

            auto deriveFromPoints = [&](PropertyKind kind, const std::optional<Expr>& value, const char* name) {
                std::size_t idx = static_cast<std::size_t>(kind);
                if (value) {
                    engine.error(value->span, std::string("'") + name + "' can't be set on a path; it's derived from 'points'");
                    slots[idx] = ResolvedProperty{ResolvedProperty::Kind::Errored, nullptr};
                }
                else if (!hasPoints) {
                    slots[idx] = ResolvedProperty{ResolvedProperty::Kind::Errored, nullptr};
                }
                else {
                    slots[idx] = ResolvedProperty{ResolvedProperty::Kind::DerivedFromPoints, nullptr};
                }
            };
            deriveFromPoints(PropertyKind::Width, node.common.width, "width");
            deriveFromPoints(PropertyKind::Height, node.common.height, "height");
            break;
        }
        case NodeType::Text:
        case NodeType::Image: {
            auto explicitOrAutomatic = [&](PropertyKind kind, const std::optional<Expr>& value) {
                std::size_t idx = static_cast<std::size_t>(kind);
                slots[idx] = value
                    ? ResolvedProperty{ResolvedProperty::Kind::Explicit, &*value}
                    : ResolvedProperty{ResolvedProperty::Kind::Automatic, nullptr};
            };
            explicitOrAutomatic(PropertyKind::Width, node.common.width);
            explicitOrAutomatic(PropertyKind::Height, node.common.height);
            break;
        }
        }
    }

    void DimensionResolver::visitLevel(const std::vector<NodeDecl>& siblings, DiagnosticsEngine& engine) {
        for (const NodeDecl& node : siblings) {
            visitNode(node, engine);
            if (node.type == NodeType::Group) visitLevel(node.children, engine);
        }
    }

    DimensionResolver DimensionResolver::build(const Document& document, DiagnosticsEngine& engine) {
        DimensionResolver resolver;
        resolver.visitLevel(document.nodes, engine);
        return resolver;
    }

    std::optional<ResolvedProperty> DimensionResolver::propertyOf(const NodeDecl* node, PropertyKind kind) const {
        auto it = resolved.find(node);
        if (it == resolved.end()) return std::nullopt;
        return it->second[static_cast<std::size_t>(kind)];
    }

    const NodeDecl* DimensionResolver::resolutionBasis(const NodeDecl* node, const SymbolTable& symbols) {
        return symbols.enclosingGroup(node);
    }

    std::optional<PageContentBox> resolvePageContentBox(const PageDecl& page, DiagnosticsEngine& engine) {
        if (!page.size) {
            engine.error(page.span, "page requires a 'size'");
            return std::nullopt;
        }

        double width = 0.0, height = 0.0;
        if (page.size->preset) {
            auto preset = presetSizeInPoints(*page.size->preset);
            if (!preset) {
                engine.error(page.span, "unknown page size preset '" + *page.size->preset + "'");
                return std::nullopt;
            }
            width = preset->first;
            height = preset->second;
        }
        else {
            // parseDimension always yields a plain NumberExpr (parser.cpp); defensive fallback if that ever changes.
            const auto* w = page.size->width ? std::get_if<NumberExpr>(&page.size->width->node) : nullptr;
            const auto* h = page.size->height ? std::get_if<NumberExpr>(&page.size->height->node) : nullptr;
            if (!w || !h) {
                engine.error(page.span, "page size must be a plain dimension");
                return std::nullopt;
            }
            width = toPoints(w->value, w->unit);
            height = toPoints(h->value, h->unit);
        }

        double margin = 0.0; // LANGUAGE-SPEC.md states no default; 0 (no margin) is the natural one
        if (page.margin) {
            const auto* m = std::get_if<NumberExpr>(&page.margin->node);
            if (!m) {
                engine.error(page.span, "margin must be a plain dimension");
                return std::nullopt;
            }
            margin = toPoints(m->value, m->unit);
        }

        return PageContentBox{width - 2 * margin, height - 2 * margin};
    }

    std::optional<std::pair<double, double>> presetSizeInPoints(std::string_view preset) {
        // Standard portrait PDF/ISO 216 point sizes -- LANGUAGE-SPEC.md names
        // these presets but never defines them numerically (see header).
        static const std::unordered_map<std::string_view, std::pair<double, double>> table = {
            {"A3", {841.89, 1190.55}},
            {"A4", {595.28, 841.89}},
            {"A5", {419.53, 595.28}},
            {"Letter", {612.0, 792.0}},
            {"Legal", {612.0, 1008.0}},
            {"Tabloid", {792.0, 1224.0}},
        };
        auto it = table.find(preset);
        return it != table.end() ? std::optional(it->second) : std::nullopt;
    }

}
