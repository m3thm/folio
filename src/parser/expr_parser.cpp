#include "parser/expr_parser.hpp"
#include "parser/parser.hpp"
#include <optional>
#include <utility>

namespace {

    using namespace folio;

    struct BinaryOpInfo {
        BinaryOp op;
        Precedence precedence;
    };

    // Token -> binary operator and its precedence level; nullopt if the
    // token isn't a binary operator.
    std::optional<BinaryOpInfo> binaryOpFor(TokenKind kind) {
        switch (kind) {
        case TokenKind::OrOr:    return BinaryOpInfo{ BinaryOp::Or,    Precedence::LogicOr };
        case TokenKind::AndAnd:  return BinaryOpInfo{ BinaryOp::And,   Precedence::LogicAnd };
        case TokenKind::EqEq:    return BinaryOpInfo{ BinaryOp::Eq,    Precedence::Equality };
        case TokenKind::NotEq:   return BinaryOpInfo{ BinaryOp::NotEq, Precedence::Equality };
        case TokenKind::Lt:      return BinaryOpInfo{ BinaryOp::Lt,    Precedence::Compare };
        case TokenKind::Gt:      return BinaryOpInfo{ BinaryOp::Gt,    Precedence::Compare };
        case TokenKind::Le:      return BinaryOpInfo{ BinaryOp::Le,    Precedence::Compare };
        case TokenKind::Ge:      return BinaryOpInfo{ BinaryOp::Ge,    Precedence::Compare };
        case TokenKind::Plus:    return BinaryOpInfo{ BinaryOp::Add,   Precedence::Additive };
        case TokenKind::Minus:   return BinaryOpInfo{ BinaryOp::Sub,   Precedence::Additive };
        case TokenKind::Star:    return BinaryOpInfo{ BinaryOp::Mul,   Precedence::Multiplicative };
        case TokenKind::Slash:   return BinaryOpInfo{ BinaryOp::Div,   Precedence::Multiplicative };
        case TokenKind::Percent: return BinaryOpInfo{ BinaryOp::Mod,   Precedence::Multiplicative };
        default:                 return std::nullopt;
        }
    }

    // The Expr constructors compute the span from the operands *before*
    // moving them into their Box.

    Expr makeBinary(BinaryOp op, Expr left, Expr right) {
        SourceSpan span{ left.span.start, right.span.end };
        return Expr{ BinaryExpr{ op, Box<Expr>(std::move(left)), Box<Expr>(std::move(right)) }, span };
    }

    Expr makeUnary(UnaryOp op, SourceSpan opSpan, Expr operand) {
        SourceSpan span{ opSpan.start, operand.span.end };
        return Expr{ UnaryExpr{ op, Box<Expr>(std::move(operand)) }, span };
    }

    Expr makeTernary(Expr condition, Expr thenBranch, Expr elseBranch) {
        SourceSpan span{ condition.span.start, elseBranch.span.end };
        return Expr{ TernaryExpr{ Box<Expr>(std::move(condition)),
                                  Box<Expr>(std::move(thenBranch)),
                                  Box<Expr>(std::move(elseBranch)) }, span };
    }

}

namespace folio {

    Expr parseExprPrecedence(Parser& p, const ExprPolicy& policy, int minPrecedence) {
        // unary := ('-' | '!')? operand     (one prefix operator, not recursive)
        std::optional<std::pair<UnaryOp, SourceSpan>> prefix;
        if (p.check(TokenKind::Minus) || p.check(TokenKind::Bang)) {
            const Token& opTok = p.advance();
            prefix = std::pair{ opTok.kind == TokenKind::Minus ? UnaryOp::Negate : UnaryOp::Not, opTok.span };
        }
        Expr left = policy.parseOperand(p);
        if (prefix) left = makeUnary(prefix->first, prefix->second, std::move(left));

        // Standard precedence climbing: keep folding operators that bind at
        // least as tightly as minPrecedence. The right-hand side is parsed one
        // level tighter, which makes every operator left-associative.
        for (;;) {
            std::optional<BinaryOpInfo> info = binaryOpFor(p.peek().kind);
            if (!info || static_cast<int>(info->precedence) < minPrecedence) break;

            p.advance();
            Expr right = parseExprPrecedence(p, policy, static_cast<int>(info->precedence) + 1);
            left = makeBinary(info->op, std::move(left), std::move(right));
        }

        // ternary := logic_or ('?' expr ':' expr)?   -- only at the top level
        if (minPrecedence <= static_cast<int>(Precedence::Ternary) && p.match(TokenKind::Question)) {
            Expr thenBranch = parseExprPrecedence(p, policy);
            p.expect(TokenKind::Colon, "expected ':' in ternary expression");
            Expr elseBranch = parseExprPrecedence(p, policy);
            return makeTernary(std::move(left), std::move(thenBranch), std::move(elseBranch));
        }

        return left;
    }

}
