#pragma once

#include "ast/ast.hpp"
#include <functional>

// Shared precedence-climbing parser for both `expr` (spec 2.1) and
// `shader_expr` (spec 3.1).
//
// The two grammars have the same precedence chain
//     ternary -> || -> && -> equality -> compare -> add -> mul -> unary
// and differ only at the bottom, in what an operand is. So the chain is
// written once, here, and each grammar supplies its own operand parser via
// an ExprPolicy.

namespace folio {

    class Parser;

    struct ExprPolicy {
        // Parses whatever sits under the unary operators:
        //   expr:        primary_num
        //   shader_expr: postfix_expr (primary_expr followed by swizzles)
        // The only point where the two grammars actually diverge.
        std::function<Expr(Parser&)> parseOperand;
    };

    // Precedence levels, loosest to tightest. Every binary level is left-associative.
    // A minPrecedence of 0 (the default) parses a whole expression, including a
    // trailing `cond ? a : b`; higher values are used internally for right-hand
    // operands, which is why '?' only ever appears at the top of an expression.
    enum class Precedence : int {
        Ternary = 0,
        LogicOr = 1,
        LogicAnd = 2,
        Equality = 3,
        Compare = 4,
        Additive = 5,
        Multiplicative = 6,
    };

    Expr parseExprPrecedence(Parser& parser, const ExprPolicy& policy,
                             int minPrecedence = static_cast<int>(Precedence::Ternary));

}
