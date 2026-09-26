#pragma once

#include "ast/ast.hpp"
#include "diagnostics/diagnostics_engine.hpp"

#include <optional>
#include <unordered_map>

// Type-checks the shader_expr sub-language (LANGUAGE-SPEC.md section 3) found
// inside one `shader { ... }` fill block (the ShaderFill alternative of
// Fill). Scoped exactly to that: solid_fill/gradient_fill/texture_fill's own
// `expr`/`import_expr` fields are ordinary expr, not shader_expr, and aren't
// this pass's job.
//
// `int` and `color` are surface syntax only, `int` has no lexically
// distinct literal from `float` (NumberExpr never preserves one), and
// `color` is a true alias for `vec4` (3.4), so neither gets its own
// ShaderType; both map onto Float/Vec4 respectively.
//
// Produces per-expression types (a typed AST via a side-table, since this
// pass predates `shader/` and its `shader::IR` isn't designed yet) rather
// than inventing that IR's concrete shape here.

namespace folio {

    enum class ShaderType { Float, Bool, Vec2, Vec3, Vec4, Texture };

    class ShaderTypeChecker {
    public:
        // `fillSpan` (the enclosing Fill's span) is only used if `return` is
        // missing entirely. `fill` must outlive the result.
        static ShaderTypeChecker build(const ShaderFill& fill, SourceSpan fillSpan, DiagnosticsEngine& engine);

        // The shader's overall type, always Vec4 if well-typed; nullopt if
        // it couldn't be determined (missing return, or a type error in it).
        [[nodiscard]] std::optional<ShaderType> returnType() const;

        // The type of any sub-expression walked during build(), keyed by its
        // address in `fill`. nullopt for one that failed to type-check.
        [[nodiscard]] std::optional<ShaderType> typeOf(const Expr* expr) const;

    private:
        ShaderTypeChecker() = default;
        std::optional<ShaderType> returnTypeValue;
        std::unordered_map<const Expr*, ShaderType> types;

        std::optional<ShaderType> check(const Expr& expr, const std::vector<Statement>& statements, DiagnosticsEngine& engine);
    };

}
