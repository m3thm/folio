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

## What Folio can express currently

This is the scope of the current grammar — see [`LANGUAGE-SPEC.md`](LANGUAGE-SPEC.md)
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

## Output formats

Folio documents render live to a window for preview/editing, and export to:

- **PDF** — with real embedded fonts and selectable text where possible
- **SVG**
- **PNG**

## Architecture

For a full deep-dive into the architecture of Folio, read [`ARCHITECTURE.md`](ARCHITECTURE.md)

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

Requires CMake 3.25+, a C++20 compiler, and [vcpkg](https://github.com/microsoft/vcpkg)
(`VCPKG_ROOT` environment variable set).

```powershell
git clone <this repo>
cd folio-project
# open the folder in Visual Studio (CMake presets are auto-detected), or:
cmake --preset debug
cmake --build out/build/debug
```

Run the compiler CLI:

```powershell
./out/build/debug/folioc path/to/file.folio
```
