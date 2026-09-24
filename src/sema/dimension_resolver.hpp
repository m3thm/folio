#pragma once

#include "ast/ast.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include "sema/symbol_table.hpp"

#include <array>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Implements LANGUAGE-SPEC.md 4.1 (percentage resolution) and 4.4 (defaults,
// derived and required properties) for the 8 properties a reference can name
// (2.1): x, y, width, height, rotation, opacity, z, radius.
//
// Doesn't evaluate anything. A group's own width may itself be a
// reference, so a child's `%` can't be resolved until that's evaluated.
// Only supplies what reference_resolver needs to do that during its own
// walk: the resolution basis, the `%` axis per property, unit/percent
// arithmetic, and a defined expr for every vertex (including defaulted and
// derived ones).

namespace folio {

    enum class PropertyKind { X, Y, Width, Height, Rotation, Opacity, Z, Radius };
    inline constexpr std::size_t kPropertyKindCount = 8;

    // Unit::None (bare number) is pt, the language default. Never call this
    // with Unit::Percent, percentages need a resolved basis value (see
    // resolvePercent), which isn't available until reference_resolver
    // evaluates that basis.
    double toPoints(double value, Unit unit);

    // What a property's `%` resolves against (4.1's table). Radius depends
    // on both axes at once (resolveRadiusPercent), not one or the other.
    enum class PercentAxis { Width, Height };
    std::vector<PercentAxis> percentAxesFor(PropertyKind kind); // empty if `%` isn't legal on `kind`

    double resolvePercent(double value, PercentAxis axis, double basisWidth, double basisHeight);
    double resolveRadiusPercent(double value, double basisWidth, double basisHeight);

    // How a (node, property) vertex gets its value once 4.4 is applied.
    struct ResolvedProperty {
        enum class Kind {
            Explicit,          // the author's own expr
            Default,           // synthesized: author omitted it (e.g. x defaults to 0)
            Derived,           // synthesized in terms of the node's own other properties (circle width = 2*self.radius)
            DerivedFromPoints, // path width/height: the bounding box of `points`; not an expr, evaluated once each point is
            Automatic,         // text/image: left to the renderer; no vertex, can't be referenced
            Errored,           // required-and-missing or illegally set; already diagnosed
        };
        Kind kind;
        const Expr* expr = nullptr; // set for Explicit/Default/Derived
    };

    class DimensionResolver {
    public:
        // `document` must outlive the result.
        static DimensionResolver build(const Document& document, DiagnosticsEngine& engine);

        // nullopt if `kind` doesn't apply to `node`'s type (e.g. Radius on a rect).
        [[nodiscard]] std::optional<ResolvedProperty> propertyOf(const NodeDecl* node, PropertyKind kind) const;

        // 4.1's resolution basis: the enclosing group, or nullptr for the page.
        static const NodeDecl* resolutionBasis(const NodeDecl* node, const SymbolTable& symbols);

    private:
        std::deque<Expr> synthesized; // owns every Default/Derived expr handed out via propertyOf; deque keeps pointers into it stable across push_back
        std::unordered_map<const NodeDecl*, std::array<std::optional<ResolvedProperty>, kPropertyKindCount>> resolved;

        const Expr* synth(Expr expr);
        void visitLevel(const std::vector<NodeDecl>& siblings, DiagnosticsEngine& engine);
        void visitNode(const NodeDecl& node, DiagnosticsEngine& engine);
        void checkPercentLegality(const NodeDecl& node, DiagnosticsEngine& engine);
    };

    // 2.1/4.1: `page.width`/`page.height` already mean the content area
    // (page size minus margin on all sides), there's no separate "raw
    // page size" a node can read. Always resolvable eagerly: unlike node
    // properties, page size/margin are always absolute (no `%`, no
    // references, see the `dimension` grammar rule), so there's no
    // dependency graph involved.
    struct PageContentBox {
        double width;
        double height;
    };
    std::optional<PageContentBox> resolvePageContentBox(const PageDecl& page, DiagnosticsEngine& engine);

    // LANGUAGE-SPEC.md names preset_size values (A4, Letter, ...) but never
    // gives their point dimensions, not sourced from the spec. These are
    // the standard portrait PDF/ISO 216 point sizes; confirm before relying
    // on them.
    std::optional<std::pair<double, double>> presetSizeInPoints(std::string_view preset);

}
