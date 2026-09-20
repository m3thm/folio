# Folio

Folio is a declarative programming language for authoring visual documents. i.e.,
posters, presentations, and diagrams, where every visual element, from a simple rectangle
to a fully custom per-pixel effect, is described declaratively and rendered through a unified
GPU pipeline.

Instead of writing separate tools for documents, magazines, presentations,
and motion graphics, Folio treats all of them as the same underlying idea: a
tree of nodes with position, size, and a some type of fill, which can be
anything from a flat color up to a hand-written shader.

Folio source files use the `.folio` extension.

## Project status

Folio is at an early stage: the front half of the pipeline is written, and
nothing renders or exports yet.

| Component                                   | State                                                                                                                                         |
| ------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| Diagnostics engine                          | Done                                                                                                                                          |
| Lexer                                       | Done                                                                                                                                          |
| AST                                         | Done                                                                                                                                          |
| Parser                                      | Done — accepts the full example in [`LANGUAGE-SPEC.md`](LANGUAGE-SPEC.md) section 5 with no errors                                            |
| `folioc` CLI                                | Partial — lexes and parses a file, reports diagnostics, and prints a short outline of the parsed nodes; a full AST dump needs the AST printer |
| AST printer, semantic analysis, scene graph | Not started                                                                                                                                   |
| Shader IR, CPU interpreter, WGSL codegen    | Not started                                                                                                                                   |
| SVG / PNG / PDF export                      | Not started                                                                                                                                   |
| Live GPU preview                            | Not started                                                                                                                                   |
| Tests                                       | Not started                                                                                                                                   |

The rest of this README describes the language as specified and the planned
pipeline; [`ARCHITECTURE.md`](ARCHITECTURE.md) has the module breakdown and
build order. `examples/` has two valid documents (`hello.folio`, a minimal one, and
`showcase.folio`, the complete example from `LANGUAGE-SPEC.md` section 5) and
`errors.folio`, a deliberately broken input for testing the lexer, parser, and
diagnostics.

## What the language covers (Phase 1)

This is the scope of the Phase 1 grammar, which the parser implements — see [`LANGUAGE-SPEC.md`](LANGUAGE-SPEC.md)
for the full EBNF specification.

- **Page setup** — page size (presets like `A4`/`Letter` or custom
  dimensions), margins, and background fill.
- **Shape nodes** — `rect`, `circle`, `ellipse`, `path` (arbitrary polygons,
  used for anything from checkmarks to hexagons to angled banner cuts).
- **Text nodes** — content, font, size, weight, alignment, line height.
  Text exported to PDF stays real, selectable/searchable text (not
  rasterized), as long as its fill is a solid color or gradient.
- **Image nodes** — importing existing images (`import("photo.jpg")`) and
  compositing them into the document, including clipping an image into an
  arbitrary shape (e.g. a photo filled into a hexagonal `path` comes out
  clipped to that hexagon, with no separate "mask" feature required).
- **Groups** — nesting nodes together for organizing repeated components
  (e.g. a numbered list item made of a badge, heading, and description).
- **Fills, in increasing order of power:**
  - solid colors (`#RRGGBB`, `rgb()`, `rgba()`, `hsl()`)
  - linear and radial gradients
  - image textures
  - fully custom **shaders** — a small, pure, side-effect-free expression
    language (signed-distance functions, noise, color math, texture
    sampling) that compiles down toward real GPU shader code, letting any
    node have an arbitrary per-pixel visual effect.
- **Strokes** — color, width, cap style, join style, on any shape.
- **Z-ordering** — explicit layering of overlapping nodes.

Because every fill is a shader under the hood (a solid color being the
simplest possible one), the same fill syntax works identically whether
you're coloring a rectangle, a hexagon, or text.

## Where the language is headed (later in development)

- Curved path segments (`curve()`, `qcurve()`, `arc()`) and SVG icon import,
  for organic/hand-drawn shapes.
- Layout containers (`stack`, `grid`, `flow`) instead of absolute
  positioning only.
- Time, `animate(...)`, and timelines — turning static documents into
  animated presentations and video.
- A reactive dependency graph for real-time property editing (drag a
  slider, see the canvas update instantly).
- User-defined reusable components/templates.
- 3D scenes composited as textures into 2D nodes.
- A math module (`solve`, `plot`, equation rendering) and diagram/flowchart
  layout.

## Planned output formats

None of these exist yet. Folio documents will render live to a window for preview/editing, and export to:

- **PDF** — with real embedded fonts and selectable text where possible
- **SVG**
- **PNG**

## Architecture

For a full deep-dive into the architecture of Folio, read [`ARCHITECTURE.md`](ARCHITECTURE.md).
Only the first two stages of this pipeline (lexer, parser) are implemented so far.

```
.folio source
   │  lexer
   ▼
tokens
   │  parser
   ▼
AST
   │  semantic analysis (type checking, unit resolution, import resolution)
   ▼
scene graph
   ├──► live preview renderer (GPU, via a cross-platform graphics
   │     abstraction; simple fills fast-path, shader{} fills compile
   │     through an intermediate shader representation to the native
   │     GPU API per platform)
   └──► exporters (PDF / SVG / PNG ), each walking the same scene
         graph and producing native output where possible, falling back
         to rasterization only for effects a given format can't express
         natively (e.g. a custom shader fill on text)
```

## Building

Requires CMake 3.25+ and a C++20 compiler. There are currently no third-party
dependencies (`vcpkg.json` is empty), so [vcpkg](https://github.com/microsoft/vcpkg)
is only needed if you use the bundled Windows presets, or once dependencies are
added (planned: freetype, harfbuzz, gtest).

```powershell
git clone https://github.com/m3thm/folio.git
cd folio
```

**Windows** — the CMake presets (Ninja generator + vcpkg toolchain, with the
`VCPKG_ROOT` environment variable set) are Windows-only. Open the folder in
Visual Studio (presets are auto-detected), or:

```powershell
cmake --preset debug
cmake --build out/build/debug
```

**Linux / macOS** — there are no presets for these platforms yet, so configure
manually (no vcpkg needed):

```bash
cmake -S . -B out/build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/debug
```

Run the compiler CLI:

```
./out/build/debug/folioc examples/hello.folio
```

`folioc` lexes and parses the file. On success it prints a short outline of the
page and its nodes and exits 0. On errors it prints diagnostics (with file,
line, and column) to stderr and exits 1. Pass `--tokens` to also dump the token
stream. Semantic analysis and export aren't implemented yet, so that is all it
does for now.
