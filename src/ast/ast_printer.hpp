#pragma once

#include "ast/ast.hpp"
#include <iosfwd>
#include <string>

// Canonical text dump of a parsed Document. This is what `folioc --ast` prints
// and what the parser golden tests (tests/golden/*.expected.txt) diff against.
//
// Design rules, so that a diff of two dumps always means a real change:
//
//  - It dumps the tree, not the source. Expressions are printed as S-expressions
//    with every operator applied explicitly, so precedence and associativity
//    are visible: `1 + 2 * 3` prints as `(+ 1 (* 2 3))`. The grouping
//    parentheses the person wrote are not in the AST, so they never appear.
//  - It is deterministic and canonical. Properties always come out in one fixed
//    order (common properties, then the node's own, then local variables, then
//    children) no matter what order they were written in. Spans are not
//    printed, so reformatting the source doesn't change the dump.
//  - It is total. Every AST alternative is handled by a std::visit visitor, so
//    adding a new kind of Expr or Fill fails to compile until it is printed
//    here. It also tolerates the half-filled tree the parser leaves behind
//    after an error, so it can be used to debug those too.
//
// Format (2 spaces of indent per level):
//
//   document
//     page
//       size: A4                          (or `size: 800pt, 600pt`)
//       background: solid(#F5F5FA)
//       margin: 40pt
//     rect header                         (`rect` alone if unnamed)
//       x: 0
//       width: 100%
//       fill: linear_gradient(angle: 180, stops: [#FFFFFF @ 0, #EEEEF5 @ 1])
//       stroke                            (or `stroke: none`)
//         color: solid(#000000)
//         width: 2pt
//       local w = (* 2 3)                 (a `name = expr` statement in the body)
//       ...children of a group, indented one more level...
//
// Expressions:
//
//   numbers      50%   40pt   1.5        (shortest text that round-trips)
//   colors       #FF8800                 (digits as stored; #RGB is already expanded)
//   strings      "a\"b\n"                (re-escaped)
//   references   self.width   badge.x   page.height
//   operators    (+ a b)  (- a b)  (* a b)  (/ a b)  (% a b)
//                (< a b)  (> a b)  (<= a b)  (>= a b)  (== a b)  (!= a b)
//                (&& a b)  (|| a b)
//                (neg a)  (not a)  (? cond then else)
//   swizzle      (swizzle base xyz)      (shader expressions only)
//   calls        rgb(30, 30, 60)   mix(a, b, t)   vec2(1, 2)
//
// `%` after a number with no space is the percent unit (`50%`); `(% a b)` is the
// modulo operator. The two never look alike in a dump.

namespace folio {

    // Writes the canonical dump of `document` to `os`. The output always ends
    // with a newline.
    void printAst(std::ostream& os, const Document& document);

    // Same, returned as a string.
    [[nodiscard]] std::string printAst(const Document& document);

}
