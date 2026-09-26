#include "sema/shader_typecheck.hpp"

#include <algorithm>
#include <string>
#include <type_traits>
#include <variant>

namespace {

    using namespace folio;

    std::string typeName(ShaderType t) {
        switch (t) {
        case ShaderType::Float: return "float";
        case ShaderType::Bool: return "bool";
        case ShaderType::Vec2: return "vec2";
        case ShaderType::Vec3: return "vec3";
        case ShaderType::Vec4: return "vec4";
        case ShaderType::Texture: return "texture";
        }
        return "?";
    }

    // Grammar type-keyword ('float','int','bool','vec2','vec3','vec4','color','texture') -> ShaderType.
    std::optional<ShaderType> typeFromName(const std::string& name) {
        if (name == "float" || name == "int") return ShaderType::Float;
        if (name == "bool") return ShaderType::Bool;
        if (name == "vec2") return ShaderType::Vec2;
        if (name == "vec3") return ShaderType::Vec3;
        if (name == "vec4" || name == "color") return ShaderType::Vec4;
        if (name == "texture") return ShaderType::Texture;
        return std::nullopt;
    }

    ShaderType vecOfArity(int n) {
        switch (n) {
        case 2: return ShaderType::Vec2;
        case 3: return ShaderType::Vec3;
        default: return ShaderType::Vec4;
        }
    }

    // -1 for a type that can't appear as a vec-constructor argument (bool, texture).
    int arityOf(ShaderType t) {
        switch (t) {
        case ShaderType::Float: return 1;
        case ShaderType::Vec2: return 2;
        case ShaderType::Vec3: return 3;
        case ShaderType::Vec4: return 4;
        default: return -1;
        }
    }

    // IDENT lookup: a shader-local declared earlier in this body (positional,
    // like a node-body `statement`, see reference_resolver.cpp), else one of
    // the always-in-scope builtin variables (3.2), else unknown.
    std::optional<ShaderType> lookupIdentifier(const std::string& name, SourceSpan referenceSpan,
        const std::vector<Statement>& statements) {
        const Statement* best = nullptr;
        for (const Statement& s : statements) {
            if (s.name != name || !(s.span.start < referenceSpan.start)) continue;
            if (!best || s.span.start > best->span.start) best = &s;
        }
        if (best) return best->type ? typeFromName(*best->type) : std::nullopt;
        if (name == "pixel" || name == "node_size" || name == "uv") return ShaderType::Vec2;
        return std::nullopt;
    }

    // GLSL's constructor rule: a single scalar broadcasts
    // to fill every component; a single vector of >= the target arity
    // truncates; otherwise every argument's components must sum to exactly
    // the target arity.
    std::optional<ShaderType> checkVecConstructor(const std::vector<ShaderType>& args, int target, SourceSpan span,
        DiagnosticsEngine& engine) {
        if (args.size() == 1) {
            int a = arityOf(args[0]);
            if (a < 0) { engine.error(span, "vec constructor arguments must be numbers or vectors"); return std::nullopt; }
            if (a == 1 || a >= target) return vecOfArity(target);
            engine.error(span, "not enough components for this constructor");
            return std::nullopt;
        }
        int total = 0;
        for (ShaderType t : args) {
            int a = arityOf(t);
            if (a < 0) { engine.error(span, "vec constructor arguments must be numbers or vectors"); return std::nullopt; }
            total += a;
        }
        if (total != target) { engine.error(span, "wrong number of components for this constructor"); return std::nullopt; }
        return vecOfArity(target);
    }

    // Section 3.3's builtins, checked against the signatures exactly as
    // written there. Two things worth noting: `sin/cos/abs/floor/fract/pow`
    // are written there without a "T"/"vec2|vec3" annotation (unlike
    // mix/clamp/length/distance/dot), so unlike GLSL itself, this treats
    // them as float-only rather than generically vector-overloaded. And
    // `texture` has no literal or builtin that produces one, so `sample()`'s
    // first argument can currently never type-check, both read directly
    // off the spec as given, not filled in.
    std::optional<ShaderType> checkBuiltin(const std::string& callee, const std::vector<ShaderType>& args,
        SourceSpan span, DiagnosticsEngine& engine) {

        auto arity = [&](std::size_t n) {
            if (args.size() != n) {
                engine.error(span, "'" + callee + "' takes " + std::to_string(n) + " argument(s)");
                return false;
            }
            return true;
        };
        auto isVec2or3 = [](ShaderType t) { return t == ShaderType::Vec2 || t == ShaderType::Vec3; };
        auto isNumericLike = [](ShaderType t) {
            return t == ShaderType::Float || t == ShaderType::Vec2 || t == ShaderType::Vec3 || t == ShaderType::Vec4;
        };
        auto expect = [&](bool ok, const char* signature) -> std::optional<ShaderType> {
            if (!ok) engine.error(span, std::string(callee) + signature);
            return std::nullopt;
        };

        if (callee == "sdf_circle") {
            if (!arity(2)) return std::nullopt;
            if (args[0] != ShaderType::Vec2 || args[1] != ShaderType::Float)
                return expect(false, "(p: vec2, r: float)");
            return ShaderType::Float;
        }
        if (callee == "sdf_rect") {
            if (!arity(2)) return std::nullopt;
            if (args[0] != ShaderType::Vec2 || args[1] != ShaderType::Vec2)
                return expect(false, "(p: vec2, size: vec2)");
            return ShaderType::Float;
        }
        if (callee == "sdf_rounded_rect") {
            if (!arity(3)) return std::nullopt;
            if (args[0] != ShaderType::Vec2 || args[1] != ShaderType::Vec2 || args[2] != ShaderType::Float)
                return expect(false, "(p: vec2, size: vec2, radius: float)");
            return ShaderType::Float;
        }
        if (callee == "sdf_line") {
            if (!arity(4)) return std::nullopt;
            if (args[0] != ShaderType::Vec2 || args[1] != ShaderType::Vec2 || args[2] != ShaderType::Vec2 || args[3] != ShaderType::Float)
                return expect(false, "(p: vec2, a: vec2, b: vec2, width: float)");
            return ShaderType::Float;
        }
        if (callee == "mix") {
            if (!arity(3)) return std::nullopt;
            if (!isNumericLike(args[0]) || args[0] != args[1] || args[2] != ShaderType::Float)
                return expect(false, "(a, b: T, t: float), a and b must share the same numeric type");
            return args[0];
        }
        if (callee == "clamp") {
            if (!arity(3)) return std::nullopt;
            if (!isNumericLike(args[0]) || args[0] != args[1] || args[0] != args[2])
                return expect(false, "(x, lo, hi: T), all three must share the same numeric type");
            return args[0];
        }
        if (callee == "smoothstep") {
            if (!arity(3)) return std::nullopt;
            if (args[0] != ShaderType::Float || args[1] != ShaderType::Float || args[2] != ShaderType::Float)
                return expect(false, "(edge0, edge1, x: float)");
            return ShaderType::Float;
        }
        if (callee == "length") {
            if (!arity(1)) return std::nullopt;
            if (!isVec2or3(args[0])) return expect(false, "(v: vec2|vec3)");
            return ShaderType::Float;
        }
        if (callee == "distance" || callee == "dot") {
            if (!arity(2)) return std::nullopt;
            if (!isVec2or3(args[0]) || args[0] != args[1]) return expect(false, "(a, b: vec2|vec3), same type");
            return ShaderType::Float;
        }
        if (callee == "sin" || callee == "cos" || callee == "abs" || callee == "floor" || callee == "fract") {
            if (!arity(1)) return std::nullopt;
            if (args[0] != ShaderType::Float) return expect(false, "(x: float)");
            return ShaderType::Float;
        }
        if (callee == "pow") {
            if (!arity(2)) return std::nullopt;
            if (args[0] != ShaderType::Float || args[1] != ShaderType::Float) return expect(false, "(x, y: float)");
            return ShaderType::Float;
        }
        if (callee == "noise") {
            if (!arity(1)) return std::nullopt;
            if (args[0] != ShaderType::Vec2) return expect(false, "(p: vec2)");
            return ShaderType::Float;
        }
        if (callee == "fbm") {
            if (!arity(2)) return std::nullopt;
            if (args[0] != ShaderType::Vec2 || args[1] != ShaderType::Float) return expect(false, "(p: vec2, octaves: int)");
            return ShaderType::Float;
        }
        if (callee == "rgb") {
            if (!arity(3)) return std::nullopt;
            if (args[0] != ShaderType::Float || args[1] != ShaderType::Float || args[2] != ShaderType::Float)
                return expect(false, "(r, g, b: float)");
            return ShaderType::Vec3;
        }
        if (callee == "rgba") {
            if (!arity(4)) return std::nullopt;
            if (std::any_of(args.begin(), args.end(), [](ShaderType t) { return t != ShaderType::Float; }))
                return expect(false, "(r, g, b, a: float)");
            return ShaderType::Vec4;
        }
        if (callee == "hsl") {
            if (!arity(3)) return std::nullopt;
            if (args[0] != ShaderType::Float || args[1] != ShaderType::Float || args[2] != ShaderType::Float)
                return expect(false, "(h, s, l: float)");
            return ShaderType::Vec3;
        }
        if (callee == "sample") {
            if (!arity(2)) return std::nullopt;
            if (args[0] != ShaderType::Texture || args[1] != ShaderType::Vec2) return expect(false, "(tex: texture, uv: vec2)");
            return ShaderType::Vec4;
        }

        engine.error(span, "'" + callee + "' is not a shader builtin");
        return std::nullopt;
    }

}

namespace folio {

    std::optional<ShaderType> ShaderTypeChecker::check(const Expr& expr, const std::vector<Statement>& statements,
        DiagnosticsEngine& engine) {

        auto result = std::visit([&](const auto& node) -> std::optional<ShaderType> {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, NumberExpr>) {
                // No lexically distinct int literal (see header); every NUMBER is float.
                return ShaderType::Float;
            }
            else if constexpr (std::is_same_v<T, ColorExpr>) {
                return ShaderType::Vec4; // HEXCOLOR: always `color` (3.4)
            }
            else if constexpr (std::is_same_v<T, IdentifierExpr>) {
                auto t = lookupIdentifier(node.name, expr.span, statements);
                if (!t) engine.error(expr.span, "unknown identifier '" + node.name + "'");
                return t;
            }
            else if constexpr (std::is_same_v<T, UnaryExpr>) {
                auto operand = check(*node.operand, statements, engine);
                if (!operand) return std::nullopt;
                if (node.op == UnaryOp::Not) {
                    if (*operand != ShaderType::Bool) { engine.error(expr.span, "'!' needs a bool"); return std::nullopt; }
                    return ShaderType::Bool;
                }
                if (*operand == ShaderType::Bool || *operand == ShaderType::Texture) {
                    engine.error(expr.span, "'-' needs a number or vector");
                    return std::nullopt;
                }
                return operand;
            }
            else if constexpr (std::is_same_v<T, BinaryExpr>) {
                auto l = check(*node.left, statements, engine);
                auto r = check(*node.right, statements, engine);
                if (!l || !r) return std::nullopt;

                switch (node.op) {
                case BinaryOp::And:
                case BinaryOp::Or:
                    if (*l != ShaderType::Bool || *r != ShaderType::Bool) {
                        engine.error(expr.span, "'&&'/'||' need bool operands");
                        return std::nullopt;
                    }
                    return ShaderType::Bool;
                case BinaryOp::Eq:
                case BinaryOp::NotEq:
                    if (*l != *r) { engine.error(expr.span, "can't compare different types"); return std::nullopt; }
                    return ShaderType::Bool;
                case BinaryOp::Lt:
                case BinaryOp::Gt:
                case BinaryOp::Le:
                case BinaryOp::Ge:
                    if (*l != ShaderType::Float || *r != ShaderType::Float) {
                        engine.error(expr.span, "'<'/'>'/'<='/'>=' need numbers");
                        return std::nullopt;
                    }
                    return ShaderType::Bool;
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Mod:
                    if (*l == ShaderType::Bool || *l == ShaderType::Texture || *r == ShaderType::Bool || *r == ShaderType::Texture) {
                        engine.error(expr.span, "arithmetic needs numbers or vectors");
                        return std::nullopt;
                    }
                    if (*l == *r) return *l;
                    if (*l == ShaderType::Float) return *r; // scalar broadcast
                    if (*r == ShaderType::Float) return *l;
                    engine.error(expr.span, "can't combine mismatched vector sizes");
                    return std::nullopt;
                }
                return std::nullopt;
            }
            else if constexpr (std::is_same_v<T, TernaryExpr>) {
                auto cond = check(*node.condition, statements, engine);
                auto thenT = check(*node.thenBranch, statements, engine);
                auto elseT = check(*node.elseBranch, statements, engine);
                if (cond && *cond != ShaderType::Bool) engine.error(node.condition->span, "ternary condition must be bool");
                if (thenT && elseT && *thenT != *elseT) {
                    engine.error(expr.span, "ternary branches have different types");
                    return std::nullopt;
                }
                return thenT;
            }
            else if constexpr (std::is_same_v<T, SwizzleExpr>) {
                auto baseType = check(*node.base, statements, engine);
                if (!baseType) return std::nullopt;
                int arity = 0;
                switch (*baseType) {
                case ShaderType::Vec2: arity = 2; break;
                case ShaderType::Vec3: arity = 3; break;
                case ShaderType::Vec4: arity = 4; break;
                default: engine.error(expr.span, "swizzle only valid on vec2/vec3/vec4"); return std::nullopt;
                }
                const std::string& mask = node.components;
                if (mask.empty() || mask.size() > 4) {
                    engine.error(expr.span, "swizzle must be 1 to 4 components");
                    return std::nullopt;
                }
                static const std::string kXyzw = "xyzw";
                static const std::string kRgba = "rgba";
                bool usesRgba = mask.find_first_of(kRgba) != std::string::npos;
                const std::string& set = usesRgba ? kRgba : kXyzw;
                for (char c : mask) {
                    std::size_t pos = set.find(c);
                    if (pos == std::string::npos || static_cast<int>(pos) >= arity) {
                        engine.error(expr.span, "'" + std::string(1, c) + "' isn't a valid component of this vector");
                        return std::nullopt;
                    }
                }
                switch (mask.size()) {
                case 1: return ShaderType::Float;
                case 2: return ShaderType::Vec2;
                case 3: return ShaderType::Vec3;
                default: return ShaderType::Vec4;
                }
            }
            else if constexpr (std::is_same_v<T, CallExpr>) {
                std::vector<std::optional<ShaderType>> argOpts;
                argOpts.reserve(node.args.size());
                for (const Expr& arg : node.args) argOpts.push_back(check(arg, statements, engine));
                if (std::any_of(argOpts.begin(), argOpts.end(), [](auto& t) { return !t.has_value(); })) return std::nullopt;

                std::vector<ShaderType> args;
                args.reserve(argOpts.size());
                for (auto& t : argOpts) args.push_back(*t);

                if (node.callee == "vec2") return checkVecConstructor(args, 2, expr.span, engine);
                if (node.callee == "vec3") return checkVecConstructor(args, 3, expr.span, engine);
                if (node.callee == "vec4") return checkVecConstructor(args, 4, expr.span, engine);
                return checkBuiltin(node.callee, args, expr.span, engine);
            }
            else {
                // BoolExpr, StringExpr, MemberRefExpr: none of these are
                // reachable from shader_expr's own grammar (3.1's primary_expr
                // lists neither bool_literal nor `reference`), defensive,
                // in case that ever changes.
                engine.error(expr.span, "not valid in a shader expression");
                return std::nullopt;
            }
        }, expr.node);

        if (result) types[&expr] = *result;
        return result;
    }

    ShaderTypeChecker ShaderTypeChecker::build(const ShaderFill& fill, SourceSpan fillSpan, DiagnosticsEngine& engine) {
        ShaderTypeChecker checker;

        for (const Statement& s : fill.statements) {
            auto valueType = checker.check(s.value, fill.statements, engine);
            std::optional<ShaderType> declaredType = s.type ? typeFromName(*s.type) : std::nullopt;
            if (s.type && !declaredType) {
                engine.error(s.span, "'" + *s.type + "' is not a shader type");
            }
            else if (declaredType && valueType && *declaredType != *valueType) {
                engine.error(s.value.span, "'" + s.name + "' is declared '" + typeName(*declaredType) +
                    "' but its value is '" + typeName(*valueType) + "'");
            }
        }

        if (!fill.returnExpr) {
            engine.error(fillSpan, "shader requires a 'return' expression");
            return checker;
        }

        auto returnType = checker.check(*fill.returnExpr, fill.statements, engine);
        if (returnType && *returnType != ShaderType::Vec4) {
            engine.error(fill.returnExpr->span, "shader must return a color (vec4), got '" + typeName(*returnType) + "'");
        }
        else {
            checker.returnTypeValue = returnType;
        }
        return checker;
    }

    std::optional<ShaderType> ShaderTypeChecker::returnType() const {
        return returnTypeValue;
    }

    std::optional<ShaderType> ShaderTypeChecker::typeOf(const Expr* expr) const {
        auto it = types.find(expr);
        return it != types.end() ? std::optional(it->second) : std::nullopt;
    }

}
