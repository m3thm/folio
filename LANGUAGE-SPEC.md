# Document Language — Phase 1 Grammar Specification

Phase 1 scope: static 2D documents. Declarative only — no time, no animation,
no reactive dependency graph. Every node has a `fill`, and every `fill` is,
under the hood, a shader — from a solid color (the simplest possible shader)
up to a fully custom per-pixel expression.

---

## 1. Lexical Tokens

```
IDENT       := [a-zA-Z_][a-zA-Z0-9_]*        (* excludes any KEYWORD below *)
NUMBER      := [0-9]+ ('.' [0-9]+)?
STRING      := '"' (any char except '"', or escaped) * '"'
UNIT        := 'px' | 'pt' | 'mm' | 'cm' | 'in'
HEXCOLOR    := '#' ( [0-9a-fA-F]{3,4} | [0-9a-fA-F]{6} | [0-9a-fA-F]{8} )
             (* 3/4 digits are #RGB / #RGBA shorthand: each digit is doubled,
                so #F80 == #FF8800 and #F808 == #FF880088. *)

KEYWORD     := 'true' | 'false'
             | 'self' | 'parent' | 'page'
             | 'at'
             (* KEYWORD tokens are reserved: they can never be lexed as IDENT,
                so a node/variable named e.g. `self` or `true` is a lex error,
                not an ambiguity resolved later. *)

PUNCT       := '{' '}' '(' ')' '[' ']' ':' ',' '.' ';' '->' '=>'
OPERATOR    := '+' '-' '*' '/' '%' '<' '>' '<=' '>=' '=' '==' '!='
                '&&' '||' '!' '?' ':'
             (* '%' is a single lexeme used two ways, disambiguated by parser
                position, not by the lexer: immediately suffixing a NUMBER
                with no space in a `dimension` context it means "percent";
                elsewhere (e.g. inside a shader_expr) it's the modulo
                operator. See §4.1.
                '=' (assignment, used only by `statement`) is likewise its
                own lexeme, distinct from '==' and '=>' — a single '=' is
                not itself an `expr` operator. *)

COMMENT     := '//' (any char except newline)*
             | '/*' (any char) * '*/'

WHITESPACE  := (' ' | '\t' | '\n' | '\r')+   // not significant, discarded
```

---

## 2. Top-Level Grammar (EBNF)

```ebnf
document        := page_decl node_decl* ;

page_decl       := 'page' '{' page_prop* '}' ;

page_prop       := 'size' ':' page_size
                  | 'background' ':' fill_expr
                  | 'margin' ':' dimension
                  ;

page_size       := preset_size
                  | dimension ',' dimension        (* width, height *)
                  ;

preset_size     := 'A4' | 'A3' | 'A5' | 'Letter' | 'Legal' | 'Tabloid' ;

dimension       := NUMBER UNIT? ;                  (* default unit: pt; no % here —
                                                        page_prop values (size, margin)
                                                        are always absolute, see §4.1 *)

bool_literal    := 'true' | 'false' ;

node_decl       := node_type IDENT? '{' node_body '}' ;

node_type       := 'rect' | 'circle' | 'ellipse' | 'path'
                  | 'text' | 'image' | 'group'
                  ;

node_body       := (common_prop | shape_prop | statement)* ;

common_prop     := 'x' ':' expr
                  | 'y' ':' expr
                  | 'width' ':' expr
                  | 'height' ':' expr
                  | 'rotation' ':' expr
                  | 'opacity' ':' expr
                  | 'z' ':' expr
                  | 'fill' ':' fill_expr
                  | 'stroke' ':' stroke_expr
                  | 'visible' ':' bool_literal
                  ;

shape_prop      := circle_prop | text_prop | image_prop | path_prop | group_prop ;

circle_prop     := 'radius' ':' expr ;

text_prop       := 'content' ':' STRING
                  | 'font' ':' STRING
                  | 'font_size' ':' expr
                  | 'font_weight' ':' ('normal' | 'bold' | NUMBER)
                  | 'align' ':' ('left' | 'center' | 'right' | 'justify')
                  | 'line_height' ':' expr
                  ;

image_prop      := 'source' ':' import_expr
                  | 'fit' ':' ('cover' | 'contain' | 'stretch' | 'none')
                  ;

path_prop       := 'points' ':' point_list
                  | 'closed' ':' bool_literal
                  ;

point_list      := '[' point (',' point)* ']' ;
point            := '(' expr ',' expr ')' ;

group_prop      := node_decl* ;   (* groups nest other nodes directly *)

statement       := IDENT '=' expr ';'?
                  ;
    (* Declares a local variable, visible only:
         - within the same node_body,
         - in property/statement positions that occur AFTER it in source
           order (top to bottom, as written in the file).
       It is not visible to sibling nodes, child nodes, or the rest of the
       document, and a statement may not reference a variable declared
       later in the same body (no forward reference, no recursion). *)
```

---

## 2.1 Expressions, Dimensions, and References

The grammar above uses `expr` for every property value (`x`, `y`, `width`,
`radius`, `font_size`, point coordinates, and so on) without ever defining
it. This section defines it.

```ebnf
expr            := ternary_num_expr ;

ternary_num_expr := logic_or_num ('?' expr ':' expr)? ;

logic_or_num    := logic_and_num ('||' logic_and_num)* ;
logic_and_num   := equality_num  ('&&' equality_num)* ;
equality_num    := compare_num   (('==' | '!=') compare_num)* ;
compare_num     := add_num       (('<' | '>' | '<=' | '>=') add_num)* ;
add_num         := mul_num       (('+' | '-') mul_num)* ;
mul_num         := unary_num     (('*' | '/' | '%') unary_num)* ;
unary_num       := ('-' | '!')? primary_num ;

primary_num     := sized_number
                  | color_literal          (* only valid where a fill_expr/
                                               color context expects it *)
                  | reference
                  | IDENT                  (* reference to a local variable
                                               declared by `statement` *)
                  | '(' expr ')'
                  ;

sized_number    := NUMBER (UNIT | '%')? ;
                  (* bare NUMBER (no suffix) is a unitless scalar — legal for
                     opacity, rotation (degrees), font_weight, line_height,
                     z, and as an operand in arithmetic. NUMBER with UNIT is
                     an absolute dimension. NUMBER with '%' is a percentage,
                     legal only for x/y/width/height/radius/font_size — see
                     §4.1 for what it's a percentage OF. Mixing a UNIT/percent
                     value with a bare scalar via + or - across incompatible
                     kinds (e.g. `50% + 3` outside of width/height context)
                     is a semantic-analysis error, not a parse error. *)

reference       := ('self' | 'parent' | 'page' | IDENT) '.' IDENT ;
                  (* 'self.<prop>'   — this node's own resolved property
                                       (e.g. self.width inside a `height:`
                                       expression for an aspect-ratio lock)
                     'parent.<prop>' — the immediately enclosing group's
                                       resolved property, or the page's
                                       content box if there is no enclosing
                                       group
                     'page.<prop>'   — page.width / page.height (the content
                                       area, i.e. page size minus margins) /
                                       page.margin
                     'IDENT.<prop>'  — a sibling or ancestor node's resolved
                                       property, looked up by the IDENT name
                                       given after its node_type in
                                       node_decl (e.g. `badge.x`). IDENT
                                       must name a node in scope: a sibling
                                       within the same group/page, or an
                                       ancestor group. Referencing a node's
                                       own descendant, or a node later in
                                       document order that isn't yet
                                       resolved, is a semantic-analysis
                                       error (no forward/cyclic references —
                                       see §4.2). *)
```

`<prop>` after a `.` is restricted to the resolved numeric properties of a
node: `x`, `y`, `width`, `height`, `rotation`, `opacity`, `z`, and — for
`circle` nodes — `radius`. It cannot reach into a fill, stroke, or text
content.

---

## 3. Fill / Shader Sublanguage

This is the part that compiles down toward shader code. A `fill:` value can
be one of four increasingly powerful forms. The compiler picks the simplest
form that matches, which matters directly for PDF export (solid/gradient
fills stay as native, selectable vector/text fills; `shader(...)` fills
get rasterized on export).

```ebnf
fill_expr       := solid_fill
                  | gradient_fill
                  | texture_fill
                  | shader_fill
                  ;

(* ---- Form 1: solid color ---- *)
solid_fill      := color_literal ;

color_literal   := HEXCOLOR
                  | 'rgb' '(' expr ',' expr ',' expr ')'
                  | 'rgba' '(' expr ',' expr ',' expr ',' expr ')'
                  | 'hsl' '(' expr ',' expr ',' expr ')'
                  ;

(* ---- Form 2: gradient ---- *)
gradient_fill   := 'linear_gradient' '(' angle_or_points ',' stop_list ')'
                  | 'radial_gradient' '(' radial_spec ',' stop_list ')'
                  ;

angle_or_points := expr                                  (* angle in degrees *)
                  | '(' expr ',' expr ')' '->' '(' expr ',' expr ')'  (* start->end points *)
                  ;

radial_spec     := expr                                  (* radius; centered on
                                                              the node's own
                                                              bounding box *)
                  | expr 'at' '(' expr ',' expr ')'       (* radius, then an
                                                              explicit center in
                                                              the node's local
                                                              coordinate space *)
                  ;

stop_list       := stop (',' stop)* ;
stop             := color_literal ('@' expr)? ;           (* optional position 0..1 *)

(* ---- Form 3: image texture ---- *)
texture_fill    := 'texture' '(' import_expr ')' ;

import_expr     := 'import' '(' STRING ')' ;

(* ---- Form 4: full custom shader ---- *)
shader_fill     := 'shader' '{' shader_body '}' ;

shader_body     := shader_stmt* 'return' shader_expr ';' ;

shader_stmt     := IDENT ':' type '=' shader_expr ';'     (* local binding *)
                  ;

stroke_expr     := 'none'
                  | '{' 'color' ':' fill_expr ',' 'width' ':' expr
                    (',' 'cap' ':' cap_style)?
                    (',' 'join' ':' join_style)? '}'
                  ;

cap_style       := 'butt' | 'round' | 'square' ;
join_style      := 'miter' | 'round' | 'bevel' ;
```

### 3.1 Shader expression language

This is the restricted, **pure, side-effect-free** sub-language used inside
`shader { ... }` blocks. It is intentionally small and closed — every
construct here maps directly onto an operation available in GLSL/HLSL/WGSL,
which is what makes automatic translation to real shader code mechanical.

```ebnf
shader_expr     := ternary_expr ;

ternary_expr    := logic_or_expr ('?' shader_expr ':' shader_expr)? ;

logic_or_expr   := logic_and_expr ('||' logic_and_expr)* ;
logic_and_expr  := equality_expr ('&&' equality_expr)* ;
equality_expr   := compare_expr (('==' | '!=') compare_expr)* ;
compare_expr    := add_expr (('<' | '>' | '<=' | '>=') add_expr)* ;
add_expr        := mul_expr (('+' | '-') mul_expr)* ;
mul_expr        := unary_expr (('*' | '/' | '%') unary_expr)* ;
unary_expr      := ('-' | '!')? postfix_expr ;

postfix_expr    := primary_expr ('.' IDENT)*        (* swizzle: .x .xy .rgb etc *)
                  ;

primary_expr    := NUMBER
                  | color_literal
                  | vec_constructor
                  | IDENT
                  | builtin_call
                  | '(' shader_expr ')'
                  ;

vec_constructor := ('vec2' | 'vec3' | 'vec4') '(' expr_list ')' ;

builtin_call    := builtin_fn '(' expr_list ')' ;

expr_list       := shader_expr (',' shader_expr)* ;
```

### 3.2 Built-in shader variables (implicitly in scope inside `shader { }`)

```
pixel        : vec2    -- current fragment position in local node space, 0..1
node_size    : vec2    -- width/height of the node in local units
uv           : vec2    -- normalized texture coordinate, alias-equivalent to pixel
```

*(Note: `time` is intentionally NOT available in phase 1 — no animation yet.
Its later addition in phase 2 is exactly the trigger for needing the
CPU/GPU classification pass discussed earlier.)*

### 3.3 Built-in shader functions

```
-- distance fields (return signed distance; negative = inside)
sdf_circle(p: vec2, r: float) -> float
sdf_rect(p: vec2, size: vec2) -> float
sdf_rounded_rect(p: vec2, size: vec2, radius: float) -> float
sdf_line(p: vec2, a: vec2, b: vec2, width: float) -> float

-- math
mix(a, b: T, t: float) -> T
clamp(x, lo, hi: T) -> T
smoothstep(edge0, edge1: float, x: float) -> float
length(v: vec2|vec3) -> float
distance(a, b: vec2|vec3) -> float
dot(a, b: vec2|vec3) -> float
sin(x) / cos(x) / abs(x) / floor(x) / fract(x) / pow(x, y) -> float

-- noise (deterministic, pure)
noise(p: vec2) -> float
fbm(p: vec2, octaves: int) -> float          -- fractal brownian motion

-- color
rgb(r, g, b: float) -> vec3
rgba(r, g, b, a: float) -> vec4
hsl(h, s, l: float) -> vec3

-- sampling
sample(tex: texture, uv: vec2) -> vec4
```

### 3.4 Types available in the shader sublanguage

```
float, int, bool
vec2, vec3, vec4
color            -- alias for vec4, constructible from rgb()/rgba()/hex
texture          -- opaque handle, only usable via sample()
```

---

## 4. Semantic Resolution Rules

Grammar alone doesn't pin down what `100%` or `parent.width` *mean* — that's
resolved in the semantic-analysis pass, after parsing and before the scene
graph is built. This section is normative for that pass.

### 4.1 Percentage resolution

A `%` value on a property is always resolved against the corresponding
axis/property of the node's **resolution basis**, defined as:

| Property           | Resolves against (`%` of...)                          |
|---------------------|--------------------------------------------------------|
| `width`             | resolution basis's `width`                              |
| `height`             | resolution basis's `height`                             |
| `x`                  | resolution basis's `width`                               |
| `y`                  | resolution basis's `height`                              |
| `radius`             | the smaller of resolution basis's `width`/`height`, ÷ 2  |
| `font_size`, `line_height` | `%` is not permitted — parse-time restriction to absolute/unitless only |

Where **resolution basis** is:

- the enclosing `group` node's resolved `width`/`height`, for any node
  declared directly inside that group;
- the page's **content box** (page size minus margins on all sides) for any
  node declared directly at the top level (a sibling of no enclosing group).

Percentages never resolve against the node's *own* prior value (no
self-referential `%`, which is why `page_prop` values like `margin` and
`size` — resolved before any node exists — reject `%` outright at the
grammar level; see the `dimension` rule in §2).

Resolution proceeds top-down: a group's own `width`/`height` must be fully
resolved (to an absolute unit) before its children's percentages can be
computed. A group whose own `width`/`height` is *itself* a percentage is
resolved against its own basis first, recursively, up to the page content
box, which is always absolute. This gives a strict top-down dependency
order with no cycles possible through nesting alone (cycles are still
possible through `reference` expressions — see §4.2).

### 4.2 Reference resolution

`self.`, `parent.`, `page.`, and `IDENT.` references (§2.1) are resolved in
the same pass, after percentages, using each node's already-resolved
absolute values:

- References form a dependency graph over resolved node properties. This
  graph must be acyclic; a cycle (e.g. `badge.x` depending on `logo.x`,
  which depends back on `badge.x`) is a semantic-analysis error, reported
  with the full cycle path.
- `IDENT.<prop>` may only name a node that is a sibling (within the same
  group or page) or an ancestor group of the referencing node — never a
  descendant. This is a scoping rule, not just a style guideline: it keeps
  the dependency graph shallow and rules out most accidental cycles by
  construction.
- Document order does **not** constrain references — a node may reference
  a sibling declared later in the file. Only the acyclicity check in the
  bullet above governs validity, not source position. (This differs from
  `statement` locals inside a single node body, which *are* strictly
  ordered — see the note under `statement` in §2.)
- `self.<prop>` may reference a property resolved earlier in the same
  node's evaluation (e.g. `height: self.width * 0.5` is valid because
  `width` is evaluated first per the property's position in `common_prop`
  ordering conventions); `self.<prop>` referencing a property not yet
  computed for that node is an error.

### 4.3 Booleans

`bool_literal` (`true` / `false`) is the only legal value for `visible` and
`closed`. Unlike numeric properties, boolean properties do not accept
`expr` — no computed/conditional visibility in phase 1.

---

## 5. Full Example

```
page {
  size: A4
  background: linear_gradient(180, #FFFFFF @ 0, #EEEEF5 @ 1)
  margin: 40pt
}

rect header {
  x: 0
  y: 0
  width: 100%
  height: 120pt
  fill: shader {
    d: float = sdf_rect(pixel - node_size * 0.5, node_size * 0.5);
    base: vec3 = mix(rgb(30, 30, 60), rgb(60, 30, 90), uv.x);
    glow: float = smoothstep(0.0, 40.0, -d);
    return rgba(base.r, base.g, base.b, glow);
  }
}

text title {
  x: 40pt
  y: 30pt
  content: "Quarterly Report"
  font: "Inter"
  font_size: 32
  font_weight: bold
  fill: #FFFFFF          // solid — stays selectable text on PDF export
}

circle badge {
  x: 480pt
  y: 300pt
  radius: 50pt
  // radius, centered on the node itself (default) — equivalent to
  // `radial_gradient(50 at (50pt, 50pt), ...)` since the circle is 100x100
  fill: radial_gradient(50, #FFD700 @ 0, #FF8C00 @ 1)
  stroke: { color: #000000, width: 2pt, join: round }
}

image logo {
  x: 40pt
  y: badge.y                // reference: align vertically with `badge`
  width: 120pt
  height: self.width        // reference: force a 1:1 aspect ratio
  source: import("logo.png")
  fit: contain
}

group footer {
  x: 0
  y: page.height - 40pt     // reference: pin to the bottom of the content box

  rect {
    width: 100%              // 100% of `footer`'s width (the enclosing group)
    height: 40pt
    fill: #111111
  }

  text {
    x: 20pt
    y: 8pt
    content: "Page 1"
    fill: #FFFFFF
  }
}
```

---

## 6. Compiler pipeline for this grammar

```
source.lang
   │  lexer
   ▼
tokens
   │  parser (recursive descent, grammar above)
   ▼
AST  (page_decl, node_decl*, nested fill_expr / shader_expr trees)
   │  semantic pass: type check, resolve imports, resolve % / unit dimensions,
   │  resolve self./parent./page./IDENT. references (topological order,
   │  cycle detection per §4.2)
   ▼
Scene Graph  (concrete Nodes: Rect, Circle, Text, Image, Group,
              each holding a resolved FillDescriptor)
   │
   ├─► FillDescriptor classification:
   │      solid / gradient / texture → native fill data (color, stops, image)
   │      shader { ... }             → shader_expr subtree kept as-is
   │
   ├─► Screen renderer:
   │      solid/gradient/texture → fast-path GPU fill
   │      shader block           → shader_expr compiled to WGSL → naga → native shader
   │
   └─► Exporters:
          PDF: text nodes → native text objects + embedded font (selectable)
               solid/gradient fills → native PDF fill/shading operators
               shader fills → rasterize node to image, embed as XObject
          SVG: solid/gradient → native SVG fill/gradient defs
               shader fills → rasterize to embedded PNG, or (future) inline
               SVG filter approximation
          PNG: full rasterization of final composited scene
```

---

## 7. What's deliberately deferred to later phases

- `time`, `animate(...)`, timelines, video/frame export
- Reactive dependency graph / dirty-flag recomputation
- Layout containers (`stack`, `grid`, `flow`) — phase 1 is absolute
  positioning only
- User-defined functions / reusable components
- 3D nodes, `render_to_texture`
- Math module (`solve`, `plot`, equation rendering)
- Diagrams / flowcharts / graph layout

Each of these plugs into the same AST → scene graph → renderer/exporter
architecture without requiring a rewrite of phase 1.
