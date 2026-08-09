# Folio — Project Architecture & Roadmap

This describes how the codebase should grow from here: what modules exist,
what each owns, how data flows between them, and — most importantly — how
to sequence the work so you have a usable tool (exporting real files) long
before the GPU live-preview renderer is finished.

---

## 1. Guiding principles

A few decisions shape everything below:

1. **One-way pipeline, matching the spec's own diagram.** Source → tokens →
   AST → resolved scene graph → {renderer, exporters}. Each stage only
   depends on the stage before it. No stage reaches back upstream (the
   renderer never touches the AST; the exporters never touch tokens).
2. **The scene graph is the seam.** Once semantic analysis produces a
   scene graph — plain data, no more percentages, no more `self.width`
   references, everything resolved to absolute numbers — the renderer and
   every exporter become independent consumers of the *same* read-only
   structure. This is the boundary that lets you build/test them in
   parallel and in either order.
3. **Two shader backends, not one, from a single typed IR.** A `shader{}`
   fill needs to run in two very different places: live on the GPU (fast,
   interactive) and baked to a raster image for PDF/SVG export (correct,
   one-shot). Building *one* shader compiler that targets both from day one
   is a trap — WGSL codegen is real compiler work, and if it's the only way
   to evaluate a shader, exporters are blocked on the renderer being done.
   Instead: compile `shader_expr` once to a small typed IR, then give that
   IR **two independent backends** — a naive CPU interpreter (pixel loop in
   plain C++, used by the exporters and by unit tests) and a WGSL codegen
   backend (used by the live renderer). This is the single most important
   structural decision in this doc — see §7.
4. **Diagnostics are a shared service, not a per-module afterthought.**
   Lexer, parser, and every sema pass report through one `DiagnosticEngine`
   so error output is consistent and testable from the start, instead of
   retrofitted later.
5. **AST nodes are closed, finite, and known up front** (the grammar says
   so explicitly). That means `std::variant` + `std::visit` is a better fit
   than a classic polymorphic class hierarchy with virtual dispatch — no
   vtables, exhaustiveness-checked visitors, and it matches how compact the
   grammar actually is. Details in §4.

---

## 2. Top-level directory structure

```
folio-project/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── docs/
│   ├── language-spec-phase1.md
│   └── ARCHITECTURE.md              (this file)
├── src/
│   ├── main.cpp
│   ├── diagnostics/
│   │   ├── diagnostic.hpp            Diagnostic, Severity, SourceSpan
│   │   └── diagnostic_engine.hpp/.cpp
│   ├── lexer/
│   │   ├── token.hpp                 (moved here from src/ — see §3)
│   │   ├── lexer.hpp
│   │   └── lexer.cpp
│   ├── ast/
│   │   ├── ast.hpp                   Document/Page/Node/Expr/Fill/Shader trees
│   │   └── ast_printer.hpp/.cpp      debug dump, drives parser golden tests
│   ├── parser/
│   │   ├── parser.hpp/.cpp           top-level recursive descent
│   │   └── expr_parser.hpp/.cpp      shared precedence-climbing parser
│   ├── sema/
│   │   ├── symbol_table.hpp/.cpp     node-name scoping for references
│   │   ├── dimension_resolver.hpp/.cpp   % / unit resolution (spec §4.1)
│   │   ├── reference_resolver.hpp/.cpp   dependency graph + cycles (§4.2)
│   │   ├── shader_typecheck.hpp/.cpp     shader_expr type checking
│   │   └── scene_builder.hpp/.cpp        AST + resolved values → scene graph
│   ├── scene/
│   │   └── scene_graph.hpp           Rect/Circle/Ellipse/Path/Text/Image/Group,
│   │                                 FillDescriptor
│   ├── shader/
│   │   ├── shader_ir.hpp             typed IR, post-typecheck
│   │   ├── cpu_interpreter.hpp/.cpp  IR → per-pixel evaluator (for export)
│   │   └── wgsl_codegen.hpp/.cpp     IR → WGSL text (for live preview)
│   ├── render/
│   │   ├── gpu_context.hpp/.cpp      device/surface abstraction
│   │   ├── scene_renderer.hpp/.cpp   scene graph → draw calls
│   │   └── window.hpp/.cpp           windowing + live preview loop
│   └── export/
│       ├── exporter.hpp              common Exporter interface
│       ├── raster_baker.hpp/.cpp     shared: shader fill → RGBA image
│       ├── pdf/
│       │   ├── pdf_exporter.hpp/.cpp
│       │   ├── pdf_writer.hpp/.cpp   low-level PDF object/xref writing
│       │   └── font_subsetter.hpp/.cpp
│       ├── svg/
│       │   └── svg_exporter.hpp/.cpp
│       └── png/
│           └── png_exporter.hpp/.cpp
├── tests/
│   ├── lexer_tests.cpp
│   ├── parser_tests.cpp
│   ├── sema_tests.cpp
│   ├── shader_interpreter_tests.cpp
│   ├── export_tests.cpp
│   └── golden/
│       ├── *.folio                   sample sources
│       └── *.expected.{svg,txt}      checked-in expected output
└── examples/
    └── *.folio                       hand-written showcase documents
```

`token.hpp` moves under `lexer/` — it's lexer output vocabulary, and
nothing outside the lexer/parser boundary should need it directly (the AST
carries its own `SourceSpan`, copied out of tokens once, so downstream code
never holds a `Token`).

---

## 3. Module breakdown

### `diagnostics/`

Everything after this point — parser, every sema pass, eventually the
exporters when a font is missing or an image fails to load — needs to
report an error at a source location without immediately halting. Build
this now, once:

```cpp
struct Diagnostic {
    enum class Severity { Error, Warning, Note };
    Severity severity;
    SourceSpan span;
    std::string message;
};

class DiagnosticEngine {
public:
    void report(Diagnostic::Severity, SourceSpan, std::string message);
    bool hasErrors() const;
    std::span<const Diagnostic> diagnostics() const;
    void printAll(std::ostream&, std::string_view sourceText) const;
};
```

Pass a `DiagnosticEngine&` into the lexer, parser, and every sema pass
rather than having each throw exceptions or return `optional`. This lets
the parser recover from one bad node and keep parsing (report-and-continue
error recovery), which matters a lot for a language people will hand-edit.

### `ast/`

The AST is the direct, structural translation of the EBNF in
`language-spec-phase1.md` — nothing resolved yet, no computed values, just
"what did the person write." One header, `ast.hpp`, is enough for phase 1;
it's not that big a grammar.

Represent `Expr` as a closed variant, matching the grammar's own closed set
of forms:

```cpp
struct NumberLit  { double value; std::optional<std::string> unit; SourceSpan span; };
struct ColorLit   { /* hex or rgb()/rgba()/hsl() args */ SourceSpan span; };
struct Reference  { std::string base; std::string prop; SourceSpan span; }; // self.width, badge.x
struct VarRef     { std::string name; SourceSpan span; };                  // local `statement` var
struct BinaryOp   { char op; Box<Expr> lhs, rhs; SourceSpan span; };
struct UnaryOp    { char op; Box<Expr> operand; SourceSpan span; };
struct Ternary    { Box<Expr> cond, then, else_; SourceSpan span; };

using Expr = std::variant<NumberLit, ColorLit, Reference, VarRef,
                           BinaryOp, UnaryOp, Ternary>;
```

(`Box<T>` here is just a small `std::unique_ptr<T>` wrapper so a variant
can hold recursive members — `std::variant` can't directly contain itself.)

Do the same for `ShaderExpr` (its own closed variant — number, color,
vec-constructor, ident, builtin call, binary/unary/ternary/swizzle) and for
`FillExpr` (`SolidFill | GradientFill | TextureFill | ShaderFill`).
`NodeDecl`, `PageDecl`, and `Document` can be plain structs with
`std::vector<NodeDecl>` children — that part of the tree isn't a closed
value type in the same sense, it's genuinely just a list.

Write `ast_printer.hpp/.cpp` (a `std::visit`-based pretty-printer that
dumps the tree back to a canonical text form) as part of this module, not
as an afterthought — it's what your first parser tests will diff against.

### `parser/`

Split into two files because the grammar itself splits cleanly:

- **`parser.hpp/.cpp`** — the structural, recursive-descent part:
  `parseDocument`, `parsePage`, `parseNode`, `parseNodeBody`,
  `parseFillExpr`, `parseStrokeExpr`, `parsePointList`. Each function maps
  ~1:1 to a production in §2 of the spec — that direct correspondence is
  worth preserving even when it means a few small functions, because it's
  what makes the parser reviewable against the grammar doc.

- **`expr_parser.hpp/.cpp`** — a single **precedence-climbing** parser
  shared by both `expr` (spec §2.1) and `shader_expr` (spec §3.1). Look at
  the two grammars side by side: they're the same six-level precedence
  chain (ternary → || → && → equality → compare → add → mul → unary) with
  only the `primary` production differing. Don't hand-write six recursive
  functions twice. Instead write one generic climber parameterized by a
  small policy struct:

  ```cpp
  struct ExprPolicy {
      // parses a `primary_num` / `primary_expr` respectively — the only
      // point where the two grammars actually diverge
      std::function<Expr(Parser&)> parsePrimary;
  };

  Expr parseExprPrecedence(Parser&, const ExprPolicy&, int minPrecedence = 0);
  ```

  This also means when the grammar's arithmetic layer changes (it will —
  phase 2's math module is exactly the kind of thing that touches this),
  you fix it once.

- **Error recovery**: on a parse error inside a `node_body`, report the
  diagnostic and skip tokens until the next `}` or a recognized property
  keyword at the current brace depth, rather than aborting the whole parse.
  A `.folio` file with one bad node should still parse the other nine.

### `sema/`

This is the module doing the real work described in spec §4, as a
sequence of passes over the AST, each with a narrow job:

1. **`symbol_table.hpp/.cpp`** — walks the tree once, building one scope
   per `group`/page level, mapping each `IDENT` node name to its `NodeDecl`.
   This is what makes `badge.x` resolvable later — look up `badge` in the
   nearest enclosing scope chain.

2. **`dimension_resolver.hpp/.cpp`** — implements §4.1: resolves every
   `%` value against its node's resolution basis, top-down starting from
   the page content box. This *must* run before reference resolution,
   since references read already-resolved absolute values.

3. **`reference_resolver.hpp/.cpp`** — implements §4.2: builds a
   dependency graph over `(node, property)` pairs touched by `self./
   parent./page./IDENT.` references, topologically sorts it, and evaluates
   in that order. A cycle is a diagnostic with the full cycle path, not a
   crash — this is worth a dedicated small `DependencyGraph` type with its
   own unit tests, since cycle detection is exactly the kind of thing that
   looks right until it isn't (see §8, testing).

4. **`shader_typecheck.hpp/.cpp`** — walks each `shader_expr` tree,
   inferring/checking types against the builtin signatures in spec §3.3,
   validating swizzles (`.rgb` only valid on `vec3`/`vec4`, etc.), and
   producing the typed `shader::IR` that `shader/` consumes. This is a
   real little type checker — give it its own file rather than folding it
   into `scene_builder`.

5. **`scene_builder.hpp/.cpp`** — the final pass: given a fully-resolved
   AST, constructs the immutable `scene::Node` tree (§4 below). This is
   deliberately the *only* place that constructs scene graph nodes — no
   other code should call `scene::Rect{...}` directly.

Run these five passes in the order listed from a single
`sema::analyze(ast::Document&, DiagnosticEngine&) -> std::optional<scene::Document>`
entry point that the CLI (and later, tests) calls.

### `scene/`

Plain data, no behavior. This is intentional — it should be trivially
walkable by three completely different consumers (renderer, each
exporter) without any of them needing to understand the language grammar.

```cpp
struct FillDescriptor {
    struct Solid   { Color color; };
    struct Gradient{ /* stops, kind, angle/points or radius+center */ };
    struct Texture { ImageHandle image; };
    struct Shader  { shader::IR ir; };
    std::variant<Solid, Gradient, Texture, Shader> value;
};

struct NodeCommon { double x, y, width, height, rotation, opacity; int z; };

struct RectNode   { NodeCommon common; FillDescriptor fill; std::optional<Stroke> stroke; };
struct CircleNode { NodeCommon common; double radius; FillDescriptor fill; std::optional<Stroke> stroke; };
// EllipseNode, PathNode, TextNode, ImageNode similarly
struct GroupNode  { NodeCommon common; std::vector<Node> children; };

using Node = std::variant<RectNode, CircleNode, EllipseNode, PathNode,
                           TextNode, ImageNode, GroupNode>;

struct Document { PageInfo page; std::vector<Node> nodes; };
```

Everything here is an absolute number in absolute units (points). No
percentages, no references, no `expr` trees survive into this layer —
that's the whole point of sema having already run.

### `shader/` — the part worth designing carefully

Per principle 3 (§1), this module has one input (the typed IR from
`shader_typecheck`) and two independent output backends:

```cpp
namespace shader {
    struct IR { /* typed expression tree: BinOp, Call, Swizzle, VecCtor,
                    VarRef(pixel/node_size/uv), Literal — post-typecheck,
                    every node knows its type */ };
}
```

- **`cpu_interpreter.hpp/.cpp`** — `Color evaluate(const IR&, vec2 pixel, vec2 nodeSize)`.
  A plain recursive `std::visit` evaluator, called once per pixel by
  `raster_baker` when export needs to rasterize a shader fill. No GPU, no
  external dependencies, trivially unit-testable (feed it a known `pixel`,
  assert the output color). **Build this before the WGSL codegen** — it's
  a day of work, it unblocks all three exporters immediately, and it gives
  you a correctness reference to diff the GPU path against later ("does
  the WGSL-compiled shader on the GPU produce the same pixel as the CPU
  interpreter, for the same input?" is a great integration test once the
  renderer exists).
- **`wgsl_codegen.hpp/.cpp`** — `std::string compileToWGSL(const IR&)`.
  Pure text generation from the same IR, consumed only by `render/`. This
  is the piece that can genuinely wait.

### `render/`

- **`gpu_context.hpp/.cpp`** — thin wrapper around whatever GPU
  abstraction you pick (see §7 for the recommendation) — device creation,
  surface/swapchain, shader module compilation from the WGSL text
  `wgsl_codegen` produces.
- **`scene_renderer.hpp/.cpp`** — walks `scene::Document`, and for each
  node picks one of two paths: solid/gradient/texture fills go through a
  small fixed set of precompiled built-in shaders (the "fast path" the
  README describes); `Shader` fills get their WGSL compiled and cached per
  distinct shader source (memoize by IR hash — many nodes will reuse
  identical shaders).
- **`window.hpp/.cpp`** — GLFW (or SDL2) window + input, owns the render
  loop, re-invokes `scene_renderer` on file change for live preview.

### `export/`

```cpp
class Exporter {
public:
    virtual ~Exporter() = default;
    virtual bool exportTo(const scene::Document&, const std::filesystem::path&,
                           DiagnosticEngine&) = 0;
};
```

- **`raster_baker.hpp/.cpp`** — shared by any exporter that needs to turn
  a `Shader` (or, for SVG in phase 1, potentially a `Gradient` if you ever
  choose not to emit native SVG gradients) fill into pixels: allocates an
  RGBA buffer sized to the node, calls `shader::cpu_interpreter::evaluate`
  per pixel, hands back an in-memory image the PDF/SVG/PNG exporters can
  each embed however they need to (XObject, `<image>`, raw PNG). One
  implementation, three consumers.
- **`pdf/`** — the exporter that needs the most care, because "real
  embedded fonts, selectable text" (README) means you're doing font
  subsetting and low-level PDF object/xref writing, not just handing text
  strings to a generic PDF library that rasterizes everything. Recommend
  `freetype` for glyph/metric access and `harfbuzz` for shaping, with your
  own minimal `pdf_writer` for the object graph — most general-purpose C++
  PDF libraries fight you exactly where this project's core value
  proposition is (selectable text on shader-decorated pages).
- **`svg/`** — the easiest exporter: direct XML text generation, native
  `<linearGradient>`/`<radialGradient>` defs for gradient fills, and
  `raster_baker` + embedded base64 PNG for `shader{}` fills. Build this
  one **first** — it has no font-subsetting complexity and is the fastest
  path to "I can see real Folio output."
- **`png/`** — full-document rasterization: run every node through
  `raster_baker` (solid/gradient fills can also go through the same
  per-pixel path for PNG, or you can composite via a simple software
  rasterizer for shapes — either is fine since PNG has no "stay vector"
  requirement) and encode with `stb_image_write` or `libpng`.

---

## 4. Data ownership model

Three different tree representations exist across the pipeline, and each
should use the ownership model that fits its shape:

| Stage | Representation | Why |
|---|---|---|
| Tokens | `std::vector<Token>`, parser holds an index | flat, no ownership question |
| AST | `std::variant` for `Expr`/`ShaderExpr`/`FillExpr`; `std::vector<NodeDecl>` for structural nesting | closed value sets → variant; open-ended lists → vector |
| Scene graph | Same variant approach as AST, but immutable after `scene_builder` produces it | read-only, shared by renderer + N exporters — no mutation aliasing to worry about |

Avoid a classic `virtual` base-class AST (`class ExprNode { virtual ~ExprNode() = default; ... }`
with subclasses) — with a closed, spec-fixed grammar, `std::variant` +
`std::visit` gives you compiler-enforced exhaustiveness (add a new `Expr`
kind, every `visit` call site that doesn't handle it fails to compile) and
avoids heap-allocating every single leaf node.

---

## 5. Diagnostics-driven testing strategy

- **`tests/lexer_tests.cpp`** — you're already positioned for this (the
  `CMakeLists.txt` gtest scaffold is commented out and ready). Cover the
  cases from the last two patches explicitly: unterminated strings,
  hex-color length validation, `=` vs `==` vs `=>`.
- **`tests/parser_tests.cpp`** — golden-file based: `.folio` snippet in →
  `ast_printer` text dump out, diffed against a checked-in `.expected.txt`.
  Far less brittle than asserting on individual AST fields per test.
- **`tests/sema_tests.cpp`** — this is where the interesting bugs will
  actually live (percentage cascades through nested groups, reference
  cycles, `self.width`-before-`width`-is-set). Write these adversarially:
  a group whose `width` is itself a percentage of its own parent; a
  three-node reference cycle; a sibling reference to a node declared later
  in the file (should work, per spec §4.2) vs. a `statement` local used
  before its declaration (should fail, per spec §2).
- **`tests/shader_interpreter_tests.cpp`** — feed the CPU interpreter
  small `shader_expr` IRs directly (skip the parser) and assert exact
  pixel colors for known inputs. This is your correctness oracle for the
  WGSL path later.
- **`tests/export_tests.cpp`** — golden SVG output for a handful of
  `examples/*.folio` files is enough to catch regressions without needing
  pixel-diffing infrastructure yet; add PNG pixel-diffing once PNG export
  exists.

---

## 6. CMake target structure

Split the single `folio_core` library along the same dependency-weight
lines as the module breakdown, so tests and early CLI builds don't need to
link GPU/windowing/font libraries at all:

```
folio_frontend   (diagnostics, lexer, ast, parser, sema, scene)  — zero external deps
folio_shader     (shader IR, cpu_interpreter, wgsl_codegen)      — zero external deps
folio_export     (export/*)                                     — freetype, harfbuzz, stb/libpng
folio_render     (render/*)                                     — GPU backend, GLFW
folioc           CLI: links folio_frontend + folio_shader + folio_export (+ folio_render optionally)
```

This means `folio_frontend` + `folio_shader` + their tests build and run
with an **empty `vcpkg.json`**, exactly as today, for as long as possible
— you don't need font or GPU dependencies to validate the lexer, parser,
sema, or shader interpreter. Add `freetype`/`harfbuzz` to `vcpkg.json` only
once you start `export/pdf/`; add GPU/window deps only once you start
`render/`.

---

## 7. GPU backend recommendation (for when you get there)

The README's own pipeline diagram already describes a WebGPU-shaped model
("compiles through an intermediate shader representation to the native GPU
API per platform"). That maps directly onto **`wgpu-native`** (the
standalone C API around Firefox's `wgpu`) taking WGSL text and targeting
Vulkan/Metal/D3D12 per platform — no separate `naga` invocation needed
since `wgpu-native` consumes WGSL directly. It isn't in vcpkg as a normal
package; plan to vendor it via `FetchContent` pulling the prebuilt release
artifacts for each platform, or build it from source. `sokol_gfx` is a
lighter-weight fallback worth knowing about if `wgpu-native`'s packaging
becomes a build-system headache, at the cost of writing your own
WGSL→GLSL/HLSL/MSL translation instead of getting it for free.

This is deliberately the last thing on this list — nothing about the
parser, sema, shader interpreter, or SVG/PNG exporters needs this decision
made yet.

---

## 8. Suggested build order

The dependency graph in §6 is also the recommended sequence — each step
unlocks something runnable end-to-end:

1. **`diagnostics/`** — small, everything else leans on it immediately.
2. **`ast/`** (+ `ast_printer`) — no logic yet, just the tree shape.
3. **`parser/`** — `parser.cpp` + shared `expr_parser.cpp`. First
   milestone: `folioc` can parse a `.folio` file and pretty-print its AST.
4. **`sema/`** — all five passes. Second milestone: `folioc` can report
   "3 nodes, page 595x842pt, no errors" for a real file, with reference
   cycles and bad percentages caught and reported.
5. **`scene/`** — comes for free as `scene_builder`'s output type.
6. **`shader/cpu_interpreter`** — small, high-leverage, unblocks exporters.
7. **`export/svg/`** — third milestone, and arguably the most important
   one: **first real file Folio has ever produced.** Do this before the
   renderer.
8. **`export/png/`** — cheap once SVG + the interpreter exist.
9. **`export/pdf/`** — the big one; budget real time for font subsetting.
10. **`render/`** (`wgsl_codegen` + `gpu_context` + `scene_renderer` +
    `window`) — live preview. Everything above already works without it,
    so it's no longer blocking the rest of the tool.

By the end of step 7 you have a command-line compiler that turns `.folio`
source into a real SVG file — a genuinely useful, demoable tool — without
having touched a GPU API at all.
