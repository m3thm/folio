// Sema tests: direct unit tests, not golden-file based (see ARCHITECTURE.md,
// "Testing"). Unlike the parser, there's no printer for resolved sema output
// yet, so these assert on individual results the way lexer_tests.cpp does.
//
// Three independent things are covered so far:
//   - DependencyGraph: built and tested with plain string keys, entirely
//     independent of the AST (see dependency_graph.hpp for why).
//   - SymbolTable and DimensionResolver: built from real .folio source
//     through the actual lexer -> parser pipeline, since their whole job is
//     answering questions about a real Document's shape.

#include "ast/ast.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/dependency_graph.hpp"
#include "sema/dimension_resolver.hpp"
#include "sema/reference_resolver.hpp"
#include "sema/shader_typecheck.hpp"
#include "sema/symbol_table.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

namespace {

    using namespace folio;

    // ---- DependencyGraph -------------------------------------------------

    using Graph = DependencyGraph<std::string>;

    // Position of `key` in a successful topoSort() order; ADD_FAILURE and
    // returns an out-of-range index if `key` isn't in `order` at all, so a
    // missing vertex shows up as a normal test failure instead of a crash.
    std::size_t positionOf(const Graph& graph, const std::vector<Graph::VertexId>& order, const std::string& key) {
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (graph.keyOf(order[i]) == key) return i;
        }
        ADD_FAILURE() << "'" << key << "' missing from topoSort() order";
        return order.size();
    }

    TEST(DependencyGraph, IsolatedVertexWithNoEdgesStillAppears) {
        Graph graph;
        graph.vertex("A");

        auto result = graph.topoSort();
        ASSERT_TRUE(std::holds_alternative<std::vector<Graph::VertexId>>(result));
        EXPECT_EQ(std::get<std::vector<Graph::VertexId>>(result).size(), 1u);
    }

    TEST(DependencyGraph, LinearChainOrdersDependenciesFirst) {
        // A depends on B depends on C.
        Graph graph;
        graph.addEdge("A", "B");
        graph.addEdge("B", "C");

        auto result = graph.topoSort();
        ASSERT_TRUE(std::holds_alternative<std::vector<Graph::VertexId>>(result));
        const auto& order = std::get<std::vector<Graph::VertexId>>(result);
        ASSERT_EQ(order.size(), 3u);

        EXPECT_LT(positionOf(graph, order, "C"), positionOf(graph, order, "B"));
        EXPECT_LT(positionOf(graph, order, "B"), positionOf(graph, order, "A"));
    }

    TEST(DependencyGraph, DiamondOrdersSharedDependencyOnceBeforeBoth) {
        // A depends on B and C; B and C both depend on D.
        Graph graph;
        graph.addEdge("A", "B");
        graph.addEdge("A", "C");
        graph.addEdge("B", "D");
        graph.addEdge("C", "D");

        auto result = graph.topoSort();
        ASSERT_TRUE(std::holds_alternative<std::vector<Graph::VertexId>>(result));
        const auto& order = std::get<std::vector<Graph::VertexId>>(result);
        ASSERT_EQ(order.size(), 4u);

        std::size_t posD = positionOf(graph, order, "D");
        std::size_t posB = positionOf(graph, order, "B");
        std::size_t posC = positionOf(graph, order, "C");
        std::size_t posA = positionOf(graph, order, "A");
        EXPECT_LT(posD, posB);
        EXPECT_LT(posD, posC);
        EXPECT_LT(posB, posA);
        EXPECT_LT(posC, posA);
    }

    TEST(DependencyGraph, RepeatedVertexCallsDedupToOneId) {
        Graph graph;
        Graph::VertexId first = graph.vertex("A");
        Graph::VertexId second = graph.vertex("A");
        EXPECT_EQ(first, second);
        EXPECT_EQ(graph.size(), 1u);
    }

    TEST(DependencyGraph, SelfCycleIsDetected) {
        // The degenerate case from LANGUAGE-SPEC.md 4.2: `width: self.width`.
        Graph graph;
        graph.addEdge("width", "width");

        auto result = graph.topoSort();
        ASSERT_TRUE(std::holds_alternative<Graph::Cycle>(result));
        const auto& cycle = std::get<Graph::Cycle>(result).path;

        ASSERT_EQ(cycle.size(), 2u);
        EXPECT_EQ(graph.keyOf(cycle.front()), "width");
        EXPECT_EQ(graph.keyOf(cycle.back()), "width");
    }

    TEST(DependencyGraph, TwoVertexCycleIsDetectedWithFullPath) {
        // badge.x depends on logo.x, which depends back on badge.x.
        Graph graph;
        graph.addEdge("badge.x", "logo.x");
        graph.addEdge("logo.x", "badge.x");

        auto result = graph.topoSort();
        ASSERT_TRUE(std::holds_alternative<Graph::Cycle>(result));
        const auto& cycle = std::get<Graph::Cycle>(result).path;

        ASSERT_EQ(cycle.size(), 3u);
        EXPECT_EQ(graph.keyOf(cycle.front()), graph.keyOf(cycle.back()));
        // Every consecutive pair in the path must be a real edge, whichever
        // vertex DFS happened to start from.
        for (std::size_t i = 0; i + 1 < cycle.size(); ++i) {
            const std::string& from = graph.keyOf(cycle[i]);
            const std::string& to = graph.keyOf(cycle[i + 1]);
            EXPECT_TRUE((from == "badge.x" && to == "logo.x") || (from == "logo.x" && to == "badge.x"))
                << "unexpected edge " << from << " -> " << to << " in reported cycle";
        }
    }

    TEST(DependencyGraph, CycleDoesNotHideUnrelatedVertices) {
        // A cycle among B/C shouldn't stop A (which depends on nothing) or
        // D (which the cycle depends on) from having been valid vertices;
        // topoSort() just has to report the cycle rather than crash or
        // silently drop part of the graph.
        Graph graph;
        graph.vertex("A");
        graph.addEdge("B", "C");
        graph.addEdge("C", "B");
        graph.addEdge("B", "D");

        auto result = graph.topoSort();
        ASSERT_TRUE(std::holds_alternative<Graph::Cycle>(result));
        EXPECT_EQ(graph.size(), 4u); // A, B, C, D were all registered before the cycle was found
    }

    // ---- SymbolTable -------------------------------------------------

    struct ParseResult {
        std::string source;
        DiagnosticsEngine engine;
        Document document;
    };

    ParseResult parseSource(std::string source) {
        ParseResult result;
        result.source = std::move(source);
        Lexer lexer(result.source, result.engine);
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(std::move(tokens), result.engine);
        result.document = parser.parse();
        return result;
    }

    const NodeDecl* findByName(const std::vector<NodeDecl>& nodes, const std::string& name) {
        for (const NodeDecl& node : nodes) {
            if (node.name == name) return &node;
            if (const NodeDecl* found = findByName(node.children, name)) return found;
        }
        return nullptr;
    }

    TEST(SymbolTable, SiblingResolvesToEarlierSibling) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect badge { }
            rect logo { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());

        const NodeDecl* logo = findByName(parsed.document.nodes, "logo");
        const NodeDecl* badge = findByName(parsed.document.nodes, "badge");
        ASSERT_NE(logo, nullptr);
        ASSERT_NE(badge, nullptr);

        EXPECT_EQ(table.resolve(logo, "badge"), badge);
    }

    TEST(SymbolTable, SiblingResolvesToLaterSiblingRegardlessOfSourceOrder) {
        // LANGUAGE-SPEC.md 2.1: "a node may reference a sibling declared
        // later in the file."
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect logo { }
            rect badge { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);

        const NodeDecl* logo = findByName(parsed.document.nodes, "logo");
        const NodeDecl* badge = findByName(parsed.document.nodes, "badge");
        EXPECT_EQ(table.resolve(logo, "badge"), badge);
    }

    TEST(SymbolTable, ChildResolvesAncestorGroupSibling) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect header { }
            group footer {
                rect inner { }
            }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);

        const NodeDecl* inner = findByName(parsed.document.nodes, "inner");
        const NodeDecl* header = findByName(parsed.document.nodes, "header");
        ASSERT_NE(inner, nullptr);
        ASSERT_NE(header, nullptr);

        EXPECT_EQ(table.resolve(inner, "header"), header);
    }

    TEST(SymbolTable, SiblingCannotResolveNodesDescendant) {
        // "Referencing a node's own descendant is a semantic-analysis
        // error" (2.1) — resolve() reports that as simply "not found", the
        // same as any other unknown name; only ancestor scopes are chained.
        auto parsed = parseSource(R"(
            page { size: A4 }
            group footer {
                rect inner { }
            }
            rect header { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);

        const NodeDecl* header = findByName(parsed.document.nodes, "header");
        ASSERT_NE(header, nullptr);

        EXPECT_EQ(table.resolve(header, "inner"), nullptr);
    }

    TEST(SymbolTable, NearestScopeShadowsOuterScope) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect logo { }
            group footer {
                rect logo { }
                rect sibling { }
            }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors()); // same name, but two different scopes: not a duplicate

        const NodeDecl* sibling = findByName(parsed.document.nodes, "sibling");
        const NodeDecl* footer = findByName(parsed.document.nodes, "footer");
        ASSERT_NE(sibling, nullptr);
        ASSERT_NE(footer, nullptr);
        const NodeDecl* innerLogo = findByName(footer->children, "logo");
        ASSERT_NE(innerLogo, nullptr);

        // "sibling" is declared inside footer, so "logo" must resolve to
        // footer's own child, not the top-level one.
        EXPECT_EQ(table.resolve(sibling, "logo"), innerLogo);
    }

    TEST(SymbolTable, EnclosingGroupIsNullForTopLevelNode) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect header { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);

        const NodeDecl* header = findByName(parsed.document.nodes, "header");
        ASSERT_NE(header, nullptr);
        EXPECT_EQ(table.enclosingGroup(header), nullptr);
    }

    TEST(SymbolTable, EnclosingGroupIsImmediateParentNotGrandparent) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            group outer {
                group inner {
                    rect leaf { }
                }
            }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);

        const NodeDecl* leaf = findByName(parsed.document.nodes, "leaf");
        const NodeDecl* inner = findByName(parsed.document.nodes, "inner");
        const NodeDecl* outer = findByName(parsed.document.nodes, "outer");
        ASSERT_NE(leaf, nullptr);
        ASSERT_NE(inner, nullptr);
        ASSERT_NE(outer, nullptr);

        EXPECT_EQ(table.enclosingGroup(leaf), inner);
        EXPECT_EQ(table.enclosingGroup(inner), outer);
        EXPECT_EQ(table.enclosingGroup(outer), nullptr);
    }

    TEST(SymbolTable, DuplicateNameInSameScopeIsDiagnosed) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect badge { }
            circle badge { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable::build(parsed.document, semaEngine);

        EXPECT_TRUE(semaEngine.hasErrors());
        ASSERT_EQ(semaEngine.diagnostics().size(), 1u);
        EXPECT_NE(semaEngine.diagnostics()[0].message.find("badge"), std::string::npos);
    }

    TEST(SymbolTable, UnnamedNodeIsUnreachableButDoesNotError) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect { }
            rect named { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable table = SymbolTable::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());

        const NodeDecl* named = findByName(parsed.document.nodes, "named");
        ASSERT_NE(named, nullptr);
        EXPECT_EQ(table.resolve(named, "doesnotexist"), nullptr);
    }

    // ---- DimensionResolver -------------------------------------------------

    const NumberExpr* asNumber(const Expr* expr) {
        return expr ? std::get_if<NumberExpr>(&expr->node) : nullptr;
    }

    TEST(DimensionResolver, ToPointsConversions) {
        EXPECT_DOUBLE_EQ(toPoints(10, Unit::None), 10.0);
        EXPECT_DOUBLE_EQ(toPoints(10, Unit::Pt), 10.0);
        EXPECT_DOUBLE_EQ(toPoints(10, Unit::Px), 7.5);
        EXPECT_DOUBLE_EQ(toPoints(1, Unit::In), 72.0);
        EXPECT_NEAR(toPoints(1, Unit::Cm), 28.346, 0.001);
        EXPECT_NEAR(toPoints(1, Unit::Mm), 2.835, 0.001);
    }

    TEST(DimensionResolver, PercentAxes) {
        EXPECT_EQ(percentAxesFor(PropertyKind::X), std::vector{ PercentAxis::Width });
        EXPECT_EQ(percentAxesFor(PropertyKind::Width), std::vector{ PercentAxis::Width });
        EXPECT_EQ(percentAxesFor(PropertyKind::Y), std::vector{ PercentAxis::Height });
        EXPECT_EQ(percentAxesFor(PropertyKind::Height), std::vector{ PercentAxis::Height });
        EXPECT_EQ(percentAxesFor(PropertyKind::Radius), (std::vector{ PercentAxis::Width, PercentAxis::Height }));
        EXPECT_TRUE(percentAxesFor(PropertyKind::Rotation).empty());
        EXPECT_TRUE(percentAxesFor(PropertyKind::Opacity).empty());
        EXPECT_TRUE(percentAxesFor(PropertyKind::Z).empty());
    }

    TEST(DimensionResolver, PercentArithmetic) {
        EXPECT_DOUBLE_EQ(resolvePercent(50, PercentAxis::Width, 200.0, 100.0), 100.0);
        EXPECT_DOUBLE_EQ(resolvePercent(50, PercentAxis::Height, 200.0, 100.0), 50.0);
        // radius: half the smaller basis dimension.
        EXPECT_DOUBLE_EQ(resolveRadiusPercent(100, 200.0, 100.0), 50.0);
    }

    TEST(DimensionResolver, ExplicitRectSizeResolves) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect box { width: 10pt height: 20pt }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());

        const NodeDecl* box = findByName(parsed.document.nodes, "box");
        auto width = dims.propertyOf(box, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::Explicit);
    }

    TEST(DimensionResolver, MissingRectSizeIsErrored) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect box { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_TRUE(semaEngine.hasErrors());

        const NodeDecl* box = findByName(parsed.document.nodes, "box");
        auto width = dims.propertyOf(box, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::Errored);
    }

    TEST(DimensionResolver, GroupDefaultsTo100Percent) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            group box { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());

        const NodeDecl* box = findByName(parsed.document.nodes, "box");
        auto width = dims.propertyOf(box, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::Default);
        const NumberExpr* number = asNumber(width->expr);
        ASSERT_NE(number, nullptr);
        EXPECT_DOUBLE_EQ(number->value, 100.0);
        EXPECT_EQ(number->unit, Unit::Percent);
    }

    TEST(DimensionResolver, CircleSizeIsDerivedFromRadius) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            circle badge { radius: 10pt }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());

        const NodeDecl* badge = findByName(parsed.document.nodes, "badge");
        auto radius = dims.propertyOf(badge, PropertyKind::Radius);
        ASSERT_TRUE(radius.has_value());
        EXPECT_EQ(radius->kind, ResolvedProperty::Kind::Explicit);

        auto width = dims.propertyOf(badge, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::Derived);
        ASSERT_NE(width->expr, nullptr);
        const auto* binary = std::get_if<BinaryExpr>(&width->expr->node);
        ASSERT_NE(binary, nullptr);
        EXPECT_EQ(binary->op, BinaryOp::Mul);
    }

    TEST(DimensionResolver, MissingCircleRadiusIsErrored) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            circle badge { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_TRUE(semaEngine.hasErrors());

        const NodeDecl* badge = findByName(parsed.document.nodes, "badge");
        auto radius = dims.propertyOf(badge, PropertyKind::Radius);
        ASSERT_TRUE(radius.has_value());
        EXPECT_EQ(radius->kind, ResolvedProperty::Kind::Errored);
    }

    TEST(DimensionResolver, ExplicitWidthOnCircleIsErrored) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            circle badge { radius: 10pt width: 5pt }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_TRUE(semaEngine.hasErrors());

        const NodeDecl* badge = findByName(parsed.document.nodes, "badge");
        auto width = dims.propertyOf(badge, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::Errored);
    }

    TEST(DimensionResolver, RadiusDoesNotApplyToRect) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect box { width: 10pt height: 10pt }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);

        const NodeDecl* box = findByName(parsed.document.nodes, "box");
        EXPECT_FALSE(dims.propertyOf(box, PropertyKind::Radius).has_value());
    }

    TEST(DimensionResolver, PathSizeIsDerivedFromPoints) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            path shape { points: [(0pt, 0pt), (10pt, 10pt)] }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());

        const NodeDecl* shape = findByName(parsed.document.nodes, "shape");
        auto width = dims.propertyOf(shape, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::DerivedFromPoints);
    }

    TEST(DimensionResolver, MissingPathPointsIsErrored) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            path shape { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_TRUE(semaEngine.hasErrors());

        const NodeDecl* shape = findByName(parsed.document.nodes, "shape");
        auto width = dims.propertyOf(shape, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::Errored);
    }

    TEST(DimensionResolver, TextSizeIsAutomaticWhenOmitted) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            text label { content: "hi" }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());

        const NodeDecl* label = findByName(parsed.document.nodes, "label");
        auto width = dims.propertyOf(label, PropertyKind::Width);
        ASSERT_TRUE(width.has_value());
        EXPECT_EQ(width->kind, ResolvedProperty::Kind::Automatic);
    }

    TEST(DimensionResolver, UniversalPropertiesDefaultWhenOmitted) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect box { width: 10pt height: 10pt }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);

        const NodeDecl* box = findByName(parsed.document.nodes, "box");
        auto opacity = dims.propertyOf(box, PropertyKind::Opacity);
        ASSERT_TRUE(opacity.has_value());
        EXPECT_EQ(opacity->kind, ResolvedProperty::Kind::Default);
        const NumberExpr* number = asNumber(opacity->expr);
        ASSERT_NE(number, nullptr);
        EXPECT_DOUBLE_EQ(number->value, 1.0);

        auto x = dims.propertyOf(box, PropertyKind::X);
        ASSERT_TRUE(x.has_value());
        EXPECT_EQ(asNumber(x->expr)->value, 0.0);
    }

    TEST(DimensionResolver, PercentIllegalOnRotationIsDiagnosed) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect box { width: 10pt height: 10pt rotation: 50% }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_TRUE(semaEngine.hasErrors());
    }

    TEST(DimensionResolver, PercentIllegalOnFontSizeIsDiagnosed) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            text label { content: "hi" font_size: 50% }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_TRUE(semaEngine.hasErrors());
    }

    TEST(DimensionResolver, PercentIllegalNestedInsideArithmeticIsDiagnosed) {
        // `%` disallowed on `z`, even buried inside an arithmetic expression.
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect box { width: 10pt height: 10pt z: 1 + 5% }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_TRUE(semaEngine.hasErrors());
    }

    TEST(DimensionResolver, PercentOnWidthIsLegal) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            group outer {
                rect box { width: 50% height: 10pt }
            }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        DimensionResolver::build(parsed.document, semaEngine);
        EXPECT_FALSE(semaEngine.hasErrors());
    }

    TEST(DimensionResolver, ResolutionBasisIsEnclosingGroupOrNull) {
        auto parsed = parseSource(R"(
            page { size: A4 }
            rect top { width: 10pt height: 10pt }
            group outer {
                rect inner { width: 10pt height: 10pt }
            }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        SymbolTable symbols = SymbolTable::build(parsed.document, semaEngine);

        const NodeDecl* top = findByName(parsed.document.nodes, "top");
        const NodeDecl* inner = findByName(parsed.document.nodes, "inner");
        const NodeDecl* outer = findByName(parsed.document.nodes, "outer");

        EXPECT_EQ(DimensionResolver::resolutionBasis(top, symbols), nullptr);
        EXPECT_EQ(DimensionResolver::resolutionBasis(inner, symbols), outer);
    }

    TEST(DimensionResolver, PresetPageSizeIsKnown) {
        auto a4 = presetSizeInPoints("A4");
        ASSERT_TRUE(a4.has_value());
        EXPECT_NEAR(a4->first, 595.28, 0.01);
        EXPECT_NEAR(a4->second, 841.89, 0.01);

        EXPECT_FALSE(presetSizeInPoints("NotASize").has_value());
    }

    TEST(DimensionResolver, PageContentBoxSubtractsMarginOnAllSides) {
        auto parsed = parseSource(R"(
            page { size: 200pt, 100pt margin: 10pt }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        auto box = resolvePageContentBox(parsed.document.page, semaEngine);
        ASSERT_TRUE(box.has_value());
        EXPECT_DOUBLE_EQ(box->width, 180.0);
        EXPECT_DOUBLE_EQ(box->height, 80.0);
        EXPECT_FALSE(semaEngine.hasErrors());
    }

    TEST(DimensionResolver, PageContentBoxUsesPresetSize) {
        auto parsed = parseSource(R"(
            page { size: A4 }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        auto box = resolvePageContentBox(parsed.document.page, semaEngine);
        ASSERT_TRUE(box.has_value());
        EXPECT_NEAR(box->width, 595.28, 0.01);
        EXPECT_NEAR(box->height, 841.89, 0.01);
    }

    TEST(DimensionResolver, MissingPageSizeIsErrored) {
        auto parsed = parseSource(R"(
            page { }
        )");
        ASSERT_FALSE(parsed.engine.hasErrors());

        DiagnosticsEngine semaEngine;
        auto box = resolvePageContentBox(parsed.document.page, semaEngine);
        EXPECT_FALSE(box.has_value());
        EXPECT_TRUE(semaEngine.hasErrors());
    }

    // ---- ReferenceResolver -------------------------------------------------

    struct ResolvedDoc {
        ParseResult parsed;
        DiagnosticsEngine semaEngine;
        SymbolTable symbols;
        DimensionResolver dims;
        std::optional<ReferenceResolver> refs;
    };

    ResolvedDoc resolveSource(std::string source) {
        ParseResult parsed = parseSource(std::move(source));
        DiagnosticsEngine semaEngine;
        SymbolTable symbols = SymbolTable::build(parsed.document, semaEngine);
        DimensionResolver dims = DimensionResolver::build(parsed.document, semaEngine);
        std::optional<ReferenceResolver> refs = ReferenceResolver::build(parsed.document, symbols, dims, semaEngine);
        return ResolvedDoc{ std::move(parsed), std::move(semaEngine), std::move(symbols), std::move(dims), std::move(refs) };
    }

    TEST(ReferenceResolver, ExplicitValuesResolve) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box { width: 10pt height: 20pt }
        )");
        ASSERT_FALSE(doc.parsed.engine.hasErrors());
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Width), 10.0);
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Height), 20.0);
    }

    TEST(ReferenceResolver, SelfReferenceInAnotherProperty) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box { width: 10pt height: self.width * 2 }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Height), 20.0);
    }

    TEST(ReferenceResolver, SiblingReferenceForwardInSourceOrder) {
        // LANGUAGE-SPEC.md 4.2: source order doesn't matter -- `a` references
        // `badge`, declared later in the file.
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect a { width: badge.width height: 10pt }
            rect badge { width: 15pt height: 10pt }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* a = findByName(doc.parsed.document.nodes, "a");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(a, PropertyKind::Width), 15.0);
    }

    TEST(ReferenceResolver, AncestorGroupReferenceViaParent) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            group outer {
                width: 200pt height: 100pt
                rect inner { width: parent.width * 0.5 height: 10pt }
            }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* inner = findByName(doc.parsed.document.nodes, "inner");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(inner, PropertyKind::Width), 100.0);
    }

    TEST(ReferenceResolver, PageReferenceUsesContentBoxNotRawSize) {
        auto doc = resolveSource(R"(
            page { size: 300pt, 200pt margin: 10pt }
            rect box { width: page.width height: page.height }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Width), 280.0);
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Height), 180.0);
        EXPECT_DOUBLE_EQ(*doc.refs->pageValueOf(PageProperty::Margin), 10.0);
    }

    TEST(ReferenceResolver, PercentAgainstGroupBasis) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            group outer {
                width: 200pt height: 100pt
                rect inner { width: 50% height: 50% }
            }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* inner = findByName(doc.parsed.document.nodes, "inner");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(inner, PropertyKind::Width), 100.0);
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(inner, PropertyKind::Height), 50.0);
    }

    TEST(ReferenceResolver, PercentAgainstPageBasisAtTopLevel) {
        auto doc = resolveSource(R"(
            page { size: 300pt, 200pt }
            rect box { width: 50% height: 50% }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Width), 150.0);
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Height), 100.0);
    }

    TEST(ReferenceResolver, StatementLocalUsedInProperty) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box {
                half = 20pt;
                width: half height: half * 2
            }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Width), 20.0);
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Height), 40.0);
    }

    TEST(ReferenceResolver, StatementLocalForwardReferenceIsUnknown) {
        // Section 2's note under `statement`: no forward reference within a body.
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box {
                width: half
                half = 20pt;
                height: 10pt
            }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_TRUE(doc.semaEngine.hasErrors());
    }

    TEST(ReferenceResolver, CircleWidthHeightDerivedFromRadius) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            circle badge { radius: 10pt }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* badge = findByName(doc.parsed.document.nodes, "badge");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(badge, PropertyKind::Width), 20.0);
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(badge, PropertyKind::Height), 20.0);
    }

    TEST(ReferenceResolver, PathSizeDerivedFromPointsBoundingBox) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            path shape { points: [(0pt, 0pt), (10pt, 30pt), (40pt, 5pt)] }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* shape = findByName(doc.parsed.document.nodes, "shape");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(shape, PropertyKind::Width), 40.0);
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(shape, PropertyKind::Height), 30.0);
    }

    TEST(ReferenceResolver, SelfCycleFailsWithDiagnostic) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box { width: self.width height: 10pt }
        )");
        EXPECT_FALSE(doc.refs.has_value());
        EXPECT_TRUE(doc.semaEngine.hasErrors());
    }

    TEST(ReferenceResolver, TwoNodeCycleFailsWithDiagnostic) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect a { width: b.width height: 10pt }
            rect b { width: a.width height: 10pt }
        )");
        EXPECT_FALSE(doc.refs.has_value());
        EXPECT_TRUE(doc.semaEngine.hasErrors());
    }

    TEST(ReferenceResolver, UnknownSiblingNameIsDiagnosedButDoesNotFailTheWholeDocument) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box { width: nonexistent.width height: 10pt }
        )");
        ASSERT_TRUE(doc.refs.has_value()); // not a cycle -- resolution still completes
        EXPECT_TRUE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Width), 0.0);
    }

    TEST(ReferenceResolver, ReferencingAutomaticSizeIsDiagnosed) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            text label { content: "hi" }
            rect box { width: label.width height: 10pt }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_TRUE(doc.semaEngine.hasErrors());
    }

    TEST(ReferenceResolver, ArithmeticOperatorsEvaluateCorrectly) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box { width: (2 + 3) * 4 - 1 height: 10pt }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Width), 19.0);
    }

    TEST(ReferenceResolver, TernaryPicksBranchByComparison) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box { width: (5 > 3) ? 10pt : 20pt height: 10pt }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_FALSE(doc.semaEngine.hasErrors());

        const NodeDecl* box = findByName(doc.parsed.document.nodes, "box");
        EXPECT_DOUBLE_EQ(*doc.refs->valueOf(box, PropertyKind::Width), 10.0);
    }

    TEST(ReferenceResolver, DivisionByZeroIsDiagnosed) {
        auto doc = resolveSource(R"(
            page { size: A4 }
            rect box { width: 10pt / 0 height: 10pt }
        )");
        ASSERT_TRUE(doc.refs.has_value());
        EXPECT_TRUE(doc.semaEngine.hasErrors());
    }

    // ---- ShaderTypeChecker -------------------------------------------------

    const ShaderFill& shaderFillOf(const NodeDecl& node) {
        return std::get<ShaderFill>(node.common.fill->node);
    }

    struct CheckedShader {
        ParseResult parsed;
        DiagnosticsEngine engine;
        ShaderTypeChecker checker;
    };

    CheckedShader checkShader(std::string source) {
        ParseResult parsed = parseSource(std::move(source));
        const NodeDecl* node = findByName(parsed.document.nodes, "box");
        DiagnosticsEngine engine;
        ShaderTypeChecker checker = ShaderTypeChecker::build(shaderFillOf(*node), node->common.fill->span, engine);
        return CheckedShader{ std::move(parsed), std::move(engine), std::move(checker) };
    }

    TEST(ShaderTypeChecker, FullExampleFromSpecTypeChecksCleanly) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    d: float = sdf_rect(pixel - node_size * 0.5, node_size * 0.5);
                    base: vec3 = mix(rgb(30, 30, 60), rgb(60, 30, 90), uv.x);
                    glow: float = smoothstep(0.0, 40.0, -d);
                    return rgba(base.r, base.g, base.b, glow);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors());
        ASSERT_TRUE(result.checker.returnType().has_value());
        EXPECT_EQ(*result.checker.returnType(), ShaderType::Vec4);
    }

    TEST(ShaderTypeChecker, MissingReturnIsDiagnosed) {
        // The parser itself already reports "expected 'return'" and recovers
        // with returnExpr left nullopt (see ast.hpp's ShaderFill comment);
        // this confirms build() handles that gracefully too rather than
        // crashing, and reports its own diagnostic independently.
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader { d: float = 1.0; }
            }
        )");
        EXPECT_TRUE(result.engine.hasErrors());
        EXPECT_FALSE(result.checker.returnType().has_value());
    }

    TEST(ShaderTypeChecker, ReturnMustBeColor) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader { return 1.0; }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
        EXPECT_FALSE(result.checker.returnType().has_value());
    }

    TEST(ShaderTypeChecker, HexColorLiteralIsVec4) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader { return #FF0000FF; }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors());
        ASSERT_TRUE(result.checker.returnType().has_value());
        EXPECT_EQ(*result.checker.returnType(), ShaderType::Vec4);
    }

    TEST(ShaderTypeChecker, DeclaredTypeMismatchIsDiagnosed) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    v: vec2 = 1.0;
                    return rgba(v.x, 0, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, ForwardReferenceToShaderLocalIsUnknown) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    a: float = b;
                    b: float = 1.0;
                    return rgba(a, 0, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, BuiltinVariablesAreUsableDirectly) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader { return rgba(uv.x, uv.y, pixel.x, node_size.x); }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, RgbaCallRejectsAVectorArgument) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    c: vec3 = rgb(1, 0, 0);
                    return rgba(c, 0, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors()); // rgba()'s first slot needs a float, not a vec3
    }

    TEST(ShaderTypeChecker, SwizzleComponentOutOfRangeIsDiagnosed) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    c: vec3 = rgb(1, 0, 0);
                    return rgba(c.r, c.g, c.a, 1.0);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors()); // 'a' isn't a valid component of a vec3
    }

    TEST(ShaderTypeChecker, MixingSwizzleSetsIsDiagnosed) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    c: vec4 = rgb(1, 0, 0).rgbr;
                    return rgba(c.xg, 0, 0, 1.0);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, VecConstructorBroadcastsSingleScalar) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    v: vec3 = vec3(1.0);
                    return rgba(v.x, v.y, v.z, 1.0);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors()); // vec3(1.0) broadcasts to vec3, matching the declared type
    }

    TEST(ShaderTypeChecker, VecConstructorSumsComponents) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    c: vec3 = rgb(1, 0, 0);
                    v: vec4 = vec4(c, 1.0);
                    return v;
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, VecConstructorWrongComponentCountIsDiagnosed) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    v: vec2 = vec2(1.0, 2.0, 3.0);
                    return rgba(v.x, v.y, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, MixRequiresMatchingTypes) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    v: vec3 = mix(rgb(1, 0, 0), 1.0, 0.5);
                    return rgba(v.x, v.y, v.z, 1.0);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, LengthRejectsVec4) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    l: float = length(rgba(1, 0, 0, 1));
                    return rgba(l, 0, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, TrigFunctionsAreFloatOnly) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    s: float = sin(uv.x);
                    return rgba(s, 0, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, UnknownBuiltinIsDiagnosed) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    v: float = not_a_real_function(1.0);
                    return rgba(v, 0, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, TernaryBranchesMustMatch) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    v: float = (uv.x > 0.5) ? 1.0 : rgb(1, 0, 0);
                    return rgba(v, 0, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, ComparisonProducesBoolUsableAsTernaryCondition) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader { return (uv.x > 0.5) ? rgba(1, 1, 1, 1) : rgba(0, 0, 0, 1); }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, ScalarVectorArithmeticBroadcasts) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader {
                    p: vec2 = node_size * 0.5;
                    return rgba(p.x, p.y, 0, 1);
                }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(ShaderTypeChecker, UnknownIdentifierIsDiagnosed) {
        auto result = checkShader(R"(
            page { size: A4 }
            rect box {
                width: 10pt height: 10pt
                fill: shader { return rgba(not_declared, 0, 0, 1); }
            }
        )");
        ASSERT_FALSE(result.parsed.engine.hasErrors());
        EXPECT_TRUE(result.engine.hasErrors());
    }

}