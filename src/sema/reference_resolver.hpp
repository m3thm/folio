#pragma once

#include "ast/ast.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include "sema/dimension_resolver.hpp"
#include "sema/symbol_table.hpp"

#include <memory>
#include <optional>

// Implements LANGUAGE-SPEC.md 4.2: builds one dependency graph over
// (node, property) pairs -- plus page.width/height/margin and statement
// locals, which need vertices too even though they aren't all externally
// referenceable -- with an edge for every self./parent./page./IDENT.
// reference, every `%` (via dimension_resolver), and every statement local
// used. Topologically sorts it and evaluates each vertex in that order.
//
// A cycle is the one graph-level error (4.2); it's reported once, with the
// full cycle path, and resolution fails (build() returns nullopt). Bad
// references (unknown name, descendant, wrong property, automatic size) are
// reported per-reference and don't stop the rest of resolution.

namespace folio {

    enum class PageProperty { Width, Height, Margin };

    class ReferenceResolver {
    public:
        // `document`, `symbols`, and `dims` must outlive the result.
        static std::optional<ReferenceResolver> build(const Document& document, const SymbolTable& symbols,
            const DimensionResolver& dims, DiagnosticsEngine& engine);

        ReferenceResolver(ReferenceResolver&&) noexcept;
        ReferenceResolver& operator=(ReferenceResolver&&) noexcept;
        ~ReferenceResolver();

        // The resolved value: points for x/y/width/height/radius, a raw
        // scalar for rotation/opacity/z. nullopt if `kind` doesn't apply to
        // `node`, or its value is Automatic or Errored (dimension_resolver
        // already diagnosed those; this just has nothing to report).
        [[nodiscard]] std::optional<double> valueOf(const NodeDecl* node, PropertyKind kind) const;

        // page.width / page.height (page's content box) / page.margin, in points.
        [[nodiscard]] std::optional<double> pageValueOf(PageProperty property) const;

    private:
        ReferenceResolver();
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
