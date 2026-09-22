// Lexer tests (see ARCHITECTURE.md, "Diagnostics-driven testing strategy").
//
// Unlike the parser tests, these are direct: small inline snippets in, an exact
// token sequence (and/or diagnostics) out. A golden file would be overkill for
// single tokens, and asserting on the real Token/Diagnostic types (rather than
// printed text) is what makes the "3-digit hex is fine, 5-digit isn't" kind of
// off-by-one bug fail loudly with the actual token, not a fuzzy text diff.
//
// A few behaviors here look like bugs on first read but are exactly what the
// lexer is supposed to do today; each such case has a comment explaining why.

#include "diagnostics/diagnostics_engine.hpp"
#include "lexer/lexer.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

    using namespace folio;

    // Running the lexer

    struct LexResult {
        std::string source;
        DiagnosticsEngine engine;
        std::vector<Token> tokens;
    };

    LexResult lex(std::string source) {
        LexResult result;
        result.source = std::move(source);
        Lexer lexer(result.source, result.engine); // Lexer keeps its own copy of the source.
        result.tokens = lexer.tokenize();
        return result;
    }

    // The kinds of every token except the trailing End marker, which every
    // tokenize() call produces and which every test below checks separately
    // (see AlwaysEndsWithEndToken) rather than repeating it in every sequence.
    std::vector<TokenKind> kindsWithoutEnd(const std::vector<Token>& tokens) {
        std::vector<TokenKind> kinds;
        for (const Token& token : tokens) {
            if (token.kind != TokenKind::End) kinds.push_back(token.kind);
        }
        return kinds;
    }

    // A short, greppable text form for failure messages: "Number 'a' Plus 'b'".
    std::string describe(const std::vector<Token>& tokens) {
        std::ostringstream out;
        for (const Token& token : tokens) {
            out << tokenKindName(token.kind) << " '" << token.lexeme << "' ";
        }
        return out.str();
    }

    ::testing::AssertionResult tokenIs(const Token& token, TokenKind kind, const std::string& lexeme) {
        if (token.kind != kind) {
            return ::testing::AssertionFailure() << "expected kind " << tokenKindName(kind)
                << ", got " << tokenKindName(token.kind) << " ('" << token.lexeme << "')";
        }
        if (token.lexeme != lexeme) {
            return ::testing::AssertionFailure() << "expected lexeme '" << lexeme << "', got '" << token.lexeme << "'";
        }
        return ::testing::AssertionSuccess();
    }

    // Numbers
    // Grammar: NUMBER := DIGIT+ ('.' DIGIT+)? — note the digit is required on
    // *both* sides of the dot. A unit or '%' suffix (px, pt, %, ...) is not part
    // of the Number token: it lexes as a separate Ident/Percent token that the
    // parser glues back on. That split, and what it implies for a dot with no
    // digit on one side, is exactly what this group pins down.

    TEST(LexerNumbers, Integer) {
        const LexResult result = lex("42");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "42"));
        EXPECT_EQ(result.tokens[0].span.start, 0u);
        EXPECT_EQ(result.tokens[0].span.end, 2u);
    }

    TEST(LexerNumbers, Decimal) {
        const LexResult result = lex("12.25");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "12.25"));
    }

    TEST(LexerNumbers, LeadingZero) {
        // Not special-cased (no octal-style meaning) — "007" is just the digits "007".
        const LexResult result = lex("007");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "007"));
    }

    TEST(LexerNumbers, TrailingDotWithNoDigitIsNotConsumed) {
        // "5." : the '.' needs a digit *after* it (per the grammar above) to be part
        // of the number, so "5." is Number "5" then a separate Dot, not one token.
        const LexResult result = lex("5.");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "5"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Dot, "."));
        EXPECT_FALSE(result.engine.hasErrors()); // this is a valid two-token split, not an error
    }

    TEST(LexerNumbers, LeadingDotWithNoDigitBeforeIsNotANumber) {
        // ".5" : a leading '.' isn't itself a digit, so isNumber() is never entered;
        // this lexes as Dot then Number "5" -- *not* a 0.5 literal. (The parser's
        // grammar has no rule that reassembles this into 0.5; write "0.5".)
        const LexResult result = lex(".5");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Dot, "."));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Number, "5"));
    }

    TEST(LexerNumbers, TwoDotsIsNumberDotNumber) {
        // "1.2.3" has only one '.' per number, so this is Number "1.2", Dot, Number "3" --
        // not a lexical error; whether it's meaningful is for the parser/sema to decide.
        const LexResult result = lex("1.2.3");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 3u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "1.2"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Dot, "."));
        EXPECT_TRUE(tokenIs(result.tokens[2], TokenKind::Number, "3"));
    }

    TEST(LexerNumbers, UnitSuffixIsASeparateIdentToken) {
        // "40pt" is Number "40" immediately followed by Ident "pt", with no space
        // and no gap in their spans; the parser is what turns this pair into a
        // dimension. The lexer itself doesn't know what a unit is.
        const LexResult result = lex("40pt");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "40"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "pt"));
        EXPECT_EQ(result.tokens[0].span.end, result.tokens[1].span.start); // adjacent, no gap
    }

    TEST(LexerNumbers, PercentSuffixIsASeparateToken) {
        // Same idea for "50%": Number "50" then a standalone Percent token.
        const LexResult result = lex("50%");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "50"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Percent, "%"));
    }

    // Identifiers
    // Grammar: IDENT := (ALPHA | '_') (ALPHA | DIGIT | '_')* . The lexer does not
    // special-case any keyword (see the comment in isIdentOrKeyword): "true",
    // "self", "rect", etc. all come out as plain Ident tokens, and it's the
    // parser's job to recognize their text.

    TEST(LexerIdentifiers, PlainWord) {
        const LexResult result = lex("banner");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "banner"));
    }

    TEST(LexerIdentifiers, LeadingUnderscoreAndInternalDigits) {
        const LexResult result = lex("_foo bar_2 __x9");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 3u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "_foo"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "bar_2"));
        EXPECT_TRUE(tokenIs(result.tokens[2], TokenKind::Ident, "__x9"));
    }

    TEST(LexerIdentifiers, KeywordsAreOrdinaryIdents) {
        // "true", "rect", and "self" are meaningful to the parser, not the lexer.
        const LexResult result = lex("true rect self");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        EXPECT_EQ(kinds, (std::vector<TokenKind>{ TokenKind::Ident, TokenKind::Ident, TokenKind::Ident }));
    }

    TEST(LexerIdentifiers, DigitsDoNotStartAnIdentifier) {
        // "9x" is Number "9" then Ident "x" -- a digit-led word is two tokens,
        // not one, since IDENT can't start with a digit.
        const LexResult result = lex("9x");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Number, "9"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "x"));
    }

    // Strings

    TEST(LexerStrings, Plain) {
        const LexResult result = lex("\"hello\"");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::String, "hello"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerStrings, Empty) {
        const LexResult result = lex("\"\"");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::String, ""));
    }

    TEST(LexerStrings, RecognizedEscapes) {
        // \n \t \" \\ are decoded; the token's lexeme holds the *decoded* text
        // (a real newline/tab byte), not the two source characters.
        const LexResult result = lex(R"("a\nb\tc\"d\\e")");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::String, "a\nb\tc\"d\\e"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerStrings, UnrecognizedEscapeKeepsTheLetterAndDropsTheBackslash) {
        // \q is not one of the four recognized escapes; the `default` case in
        // isString() drops the backslash and keeps the letter, so `\q` -> `q`, not
        // a lexical error and not a literal backslash-q. If this is ever meant to
        // be a diagnostic instead, this is the test that should start failing.
        const LexResult result = lex(R"("a\qb")");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::String, "aqb"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerStrings, Unterminated) {
        const LexResult result = lex("\"no closing quote");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_EQ(result.tokens[0].kind, TokenKind::Invalid);
        ASSERT_TRUE(result.engine.hasErrors());
        ASSERT_EQ(result.engine.diagnostics().size(), 1u);
        const Diagnostic& diag = result.engine.diagnostics()[0];
        EXPECT_EQ(diag.severity, Diagnostic::Severity::Error);
        EXPECT_EQ(diag.message, "unterminated string literal");
        EXPECT_EQ(diag.span.start, 0u); // points at the opening quote
        EXPECT_EQ(diag.span.end, result.source.size()); // ... through end of file
    }

    TEST(LexerStrings, UnterminatedStopsTokenizingAfterIt) {
        // Nothing after an unterminated string is lexed as further tokens: the
        // whole rest of the source was already consumed looking for the closing
        // quote, so tokenize() has nothing left to do but emit End.
        const LexResult result = lex("\"unterminated\nrect a { x: 1 }");
        EXPECT_EQ(kindsWithoutEnd(result.tokens).size(), 1u); // just the Invalid string token
        EXPECT_EQ(result.tokens.back().kind, TokenKind::End);
    }

    // Hex colors
    // Grammar: HEXCOLOR := '#' ([0-9a-fA-F]{3,4} | [0-9a-fA-F]{6,8}). The lexer
    // enforces only the *digit count* (3, 4, 6, or 8); it does not distinguish
    // "#RGB" from "#RRGGBB" beyond that, and expanding the short forms is the
    // parser's job (see the comment in isHexColor and ARCHITECTURE.md's note that
    // #RGB/#RGBA expansion belongs to the parser tests, not these).

    TEST(LexerHexColor, ValidLengths) {
        for (const std::string valid : { "#FFF", "#FFFF", "#FFFFFF", "#FFFFFFFF" }) {
            const LexResult result = lex(valid);
            ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u) << valid;
            EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::HexColor, valid)) << valid;
            EXPECT_FALSE(result.engine.hasErrors()) << valid;
        }
    }

    TEST(LexerHexColor, MixedCaseDigitsAreKeptAsWritten) {
        // The lexer doesn't normalize case; #ff8800 stays lowercase in the token
        // (case normalization, if any, is a later stage's job, not lexing).
        const LexResult result = lex("#ff8800");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::HexColor, "#ff8800"));
    }

    TEST(LexerHexColor, InvalidLengthsAreRejected) {
        // 1, 2, 5, 7, and 9+ digits are all invalid -- only 3/4/6/8 are legal.
        for (const std::string invalid : { "#F", "#FF", "#FFFFF", "#FFFFFFF", "#FFFFFFFFF" }) {
            const LexResult result = lex(invalid);
            ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u) << invalid;
            EXPECT_EQ(result.tokens[0].kind, TokenKind::Invalid) << invalid;
            ASSERT_TRUE(result.engine.hasErrors()) << invalid;
            ASSERT_EQ(result.engine.diagnostics().size(), 1u) << invalid;
            EXPECT_EQ(result.engine.diagnostics()[0].message, "hex color must have 3, 4, 6, or 8 digits") << invalid;
        }
    }

    TEST(LexerHexColor, NonHexDigitsAfterHashAreNotConsumed) {
        // "#GGG" : 'G' isn't a hex digit, so the '#' itself collects zero digits
        // (an error: 0 is not in {3,4,6,8}) and is its own Invalid token; "GGG"
        // then lexes separately as a plain Ident, not as part of the color.
        const LexResult result = lex("#GGG");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Invalid, "#"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "GGG"));
        ASSERT_TRUE(result.engine.hasErrors());
        EXPECT_EQ(result.engine.diagnostics()[0].message, "hex color must have 3, 4, 6, or 8 digits");
    }

    TEST(LexerHexColor, HashWithNothingAfterIt) {
        const LexResult result = lex("#");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Invalid, "#"));
        ASSERT_TRUE(result.engine.hasErrors());
    }

    // Comments
    // Both comment forms are consumed in skipWhitespaceAndComments and never
    // produce a token at all -- not even an implicit separator -- so a comment
    // right between two other tokens simply disappears from the token stream.

    TEST(LexerComments, LineCommentIsSkipped) {
        const LexResult result = lex("a // trailing comment\nb");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "a"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "b"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerComments, LineCommentAtEndOfFileWithNoTrailingNewline) {
        // The '\n' the line-comment loop looks for doesn't have to exist: running
        // off the end of the source is a normal, error-free way for it to stop too.
        const LexResult result = lex("a // comment with no newline after it");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "a"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerComments, BlockCommentIsSkipped) {
        const LexResult result = lex("a /* skip me */ b");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "a"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "b"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerComments, BlockCommentCanSpanMultipleLines) {
        const LexResult result = lex("a /* line one\n   line two */ b");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerComments, BlockCommentsDoNotNest) {
        // The scanner stops at the *first* "*/", so "/* outer /* inner */ still
        // outer text */" closes after "inner */" -- the trailing "still outer
        // text */" is ordinary source, not comment. Confirm it lexes as tokens.
        const LexResult result = lex("/* outer /* inner */ still outer text */");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        // "still outer text" = 3 idents; the final "*/" is punctuation, not comment.
        ASSERT_EQ(kinds.size(), 5u) << describe(result.tokens); // still, outer, text, Star, Slash
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "still"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "outer"));
        EXPECT_TRUE(tokenIs(result.tokens[2], TokenKind::Ident, "text"));
        EXPECT_TRUE(tokenIs(result.tokens[3], TokenKind::Star, "*"));
        EXPECT_TRUE(tokenIs(result.tokens[4], TokenKind::Slash, "/"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerComments, SlashStarSlashDoesNotCloseItself) {
        // "/*/" : after consuming the opening "/*", the very next character is
        // '/', not '*', so the "peek()=='*' && peek(1)=='/'" close check does not
        // match on it -- the third '/' is ordinary comment *content*, and the
        // comment keeps scanning for a real "*/" that never comes.
        const LexResult result = lex("/*/  rect a {}");
        EXPECT_TRUE(kindsWithoutEnd(result.tokens).empty()) << describe(result.tokens); // whole file swallowed
        ASSERT_TRUE(result.engine.hasErrors());
        ASSERT_EQ(result.engine.diagnostics().size(), 1u);
        EXPECT_EQ(result.engine.diagnostics()[0].message, "unterminated block comment");
    }

    TEST(LexerComments, Unterminated) {
        const LexResult result = lex("rect a {} /* this never ends");
        // Reported at the *opening* "/*", not at end of file, and the span is
        // exactly those two characters (see the comment in skipWhitespaceAndComments).
        ASSERT_TRUE(result.engine.hasErrors());
        ASSERT_EQ(result.engine.diagnostics().size(), 1u);
        const Diagnostic& diag = result.engine.diagnostics()[0];
        EXPECT_EQ(diag.message, "unterminated block comment");
        const std::size_t commentStart = result.source.find("/*");
        EXPECT_EQ(diag.span.start, commentStart);
        EXPECT_EQ(diag.span.end, commentStart + 2);
        // Tokens before the comment are still there -- only what's inside (and after) it is lost.
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        EXPECT_EQ(kinds, (std::vector<TokenKind>{ TokenKind::Ident, TokenKind::Ident, TokenKind::LBrace, TokenKind::RBrace }));
    }

    TEST(LexerComments, ClosedCommentRightBeforeOneThatIsnt) {
        // A properly closed comment must not affect where the *next* one's error
        // is reported: the diagnostic should point at the second "/*", not the first.
        const LexResult result = lex("/* ok */ /* not closed");
        ASSERT_TRUE(result.engine.hasErrors());
        ASSERT_EQ(result.engine.diagnostics().size(), 1u);
        const std::size_t secondCommentStart = result.source.find("/*", 1);
        EXPECT_EQ(result.engine.diagnostics()[0].span.start, secondCommentStart);
        EXPECT_TRUE(kindsWithoutEnd(result.tokens).empty());
    }

    // '&' / '|' : only && and || exist in the grammar; a lone one is an error,
    // distinct from "unexpected character" (it names what was expected).

    TEST(LexerAndOr, DoubleAmpersand) {
        const LexResult result = lex("&&");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::AndAnd, "&&"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerAndOr, DoublePipe) {
        const LexResult result = lex("||");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::OrOr, "||"));
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerAndOr, LoneAmpersandIsAnError) {
        const LexResult result = lex("a & b");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 3u) << describe(result.tokens);
        EXPECT_EQ(kinds[1], TokenKind::Invalid);
        EXPECT_EQ(result.tokens[1].lexeme, "&");
        ASSERT_TRUE(result.engine.hasErrors());
        ASSERT_EQ(result.engine.diagnostics().size(), 1u);
        EXPECT_EQ(result.engine.diagnostics()[0].message, "expected '&&', found '&'");
    }

    TEST(LexerAndOr, LonePipeIsAnError) {
        const LexResult result = lex("a | b");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 3u) << describe(result.tokens);
        EXPECT_EQ(kinds[1], TokenKind::Invalid);
        ASSERT_TRUE(result.engine.hasErrors());
        EXPECT_EQ(result.engine.diagnostics()[0].message, "expected '||', found '|'");
    }

    TEST(LexerAndOr, LoneAmpersandStillLetsLexingContinue) {
        // Unlike an unterminated string/comment, a bad '&' or '|' doesn't swallow
        // the rest of the file -- tokenizing resumes right after it.
        const LexResult result = lex("a & b & c");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        EXPECT_EQ(kinds, (std::vector<TokenKind>{
            TokenKind::Ident, TokenKind::Invalid, TokenKind::Ident, TokenKind::Invalid, TokenKind::Ident }));
        EXPECT_EQ(result.engine.diagnostics().size(), 2u);
    }

    // '=' vs '==' vs '=>' : three different tokens sharing a prefix; the lexer
    // must maximally-munch ("==" before settling for "=", "=>" checked too).

    TEST(LexerEquals, SingleEquals) {
        const LexResult result = lex("=");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Eq, "="));
    }

    TEST(LexerEquals, DoubleEquals) {
        const LexResult result = lex("==");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::EqEq, "=="));
    }

    TEST(LexerEquals, FatArrow) {
        // '=>' is unused by the grammar today (see the comment on TokenKind::FatArrow
        // in token.hpp) but the lexer still needs to produce it as its own token
        // rather than as Eq followed by Gt.
        const LexResult result = lex("=>");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::FatArrow, "=>"));
    }

    TEST(LexerEquals, AllThreeInSequenceAreTellApart) {
        const LexResult result = lex("= == =>");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 3u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Eq, "="));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::EqEq, "=="));
        EXPECT_TRUE(tokenIs(result.tokens[2], TokenKind::FatArrow, "=>"));
    }

    TEST(LexerEquals, EqualsImmediatelyFollowedByGreaterThanWithNoSpace) {
        // Nothing here depends on whitespace between tokens -- "a=>b" must still
        // split as Ident, FatArrow, Ident, not e.g. Eq + Gt swallowing the boundary.
        const LexResult result = lex("a=>b");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 3u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "a"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::FatArrow, "=>"));
        EXPECT_TRUE(tokenIs(result.tokens[2], TokenKind::Ident, "b"));
    }

    // The rest of punctuation and the two-character operators not covered above
    // (->, !=, <=, >=), plus the single-character forms they're built from.

    TEST(LexerPunctuation, EverySingleCharacterToken) {
        const LexResult result = lex("{ } ( ) [ ] : , . ; + - * / % ? @ ! < >");
        const std::vector<TokenKind> expected = {
            TokenKind::LBrace, TokenKind::RBrace, TokenKind::LParen, TokenKind::RParen,
            TokenKind::LBracket, TokenKind::RBracket, TokenKind::Colon, TokenKind::Comma,
            TokenKind::Dot, TokenKind::Semicolon, TokenKind::Plus, TokenKind::Minus,
            TokenKind::Star, TokenKind::Slash, TokenKind::Percent, TokenKind::Question,
            TokenKind::At, TokenKind::Bang, TokenKind::Lt, TokenKind::Gt,
        };
        EXPECT_EQ(kindsWithoutEnd(result.tokens), expected) << describe(result.tokens);
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerPunctuation, TwoCharacterOperators) {
        const LexResult result = lex("-> != <= >=");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 4u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::ThinArrow, "->"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::NotEq, "!="));
        EXPECT_TRUE(tokenIs(result.tokens[2], TokenKind::Le, "<="));
        EXPECT_TRUE(tokenIs(result.tokens[3], TokenKind::Ge, ">="));
    }

    TEST(LexerPunctuation, MinusIsNotConfusedWithThinArrow) {
        const LexResult result = lex("- ->");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Minus, "-"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::ThinArrow, "->"));
    }

    TEST(LexerPunctuation, BangIsNotConfusedWithNotEq) {
        const LexResult result = lex("! !=");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Bang, "!"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::NotEq, "!="));
    }

    // Unexpected characters — anything not covered above.

    TEST(LexerErrors, UnexpectedCharacter) {
        const LexResult result = lex("$");
        ASSERT_EQ(kindsWithoutEnd(result.tokens).size(), 1u);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Invalid, "$"));
        ASSERT_TRUE(result.engine.hasErrors());
        ASSERT_EQ(result.engine.diagnostics().size(), 1u);
        EXPECT_EQ(result.engine.diagnostics()[0].message, "unexpected character '$'");
    }

    TEST(LexerErrors, UnexpectedCharacterDoesNotStopLexing) {
        const LexResult result = lex("a $ b");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        EXPECT_EQ(kinds, (std::vector<TokenKind>{ TokenKind::Ident, TokenKind::Invalid, TokenKind::Ident }));
        EXPECT_EQ(result.engine.diagnostics().size(), 1u);
    }

    TEST(LexerErrors, MultipleIndependentErrorsAreAllReported) {
        // examples/errors.folio (see the parser goldens) exercises this in a full
        // document; this is the minimal version: three unrelated lexical errors in
        // one source all show up, in source order, as three separate diagnostics.
        const LexResult result = lex("& | $");
        ASSERT_EQ(result.engine.diagnostics().size(), 3u);
        EXPECT_EQ(result.engine.diagnostics()[0].message, "expected '&&', found '&'");
        EXPECT_EQ(result.engine.diagnostics()[1].message, "expected '||', found '|'");
        EXPECT_EQ(result.engine.diagnostics()[2].message, "unexpected character '$'");
        EXPECT_LT(result.engine.diagnostics()[0].span.start, result.engine.diagnostics()[1].span.start);
        EXPECT_LT(result.engine.diagnostics()[1].span.start, result.engine.diagnostics()[2].span.start);
    }

    // Whitespace, line endings, and the End token

    TEST(LexerWhitespace, SpacesTabsAndNewlinesAreAllSkipped) {
        const LexResult result = lex("a \t\n\r\n b");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "a"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "b"));
    }

    TEST(LexerWhitespace, CrLfLineEndingsWork) {
        // A file checked out with CRLF endings must lex the same as one with LF:
        // '\r' is skipped as whitespace like any other, so a line comment still
        // ends at the '\n' and doesn't swallow the next line.
        const LexResult result = lex("a // comment\r\nb");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        ASSERT_EQ(kinds.size(), 2u) << describe(result.tokens);
        EXPECT_TRUE(tokenIs(result.tokens[0], TokenKind::Ident, "a"));
        EXPECT_TRUE(tokenIs(result.tokens[1], TokenKind::Ident, "b"));
    }

    TEST(LexerEndToken, EmptySourceIsJustEnd) {
        const LexResult result = lex("");
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].kind, TokenKind::End);
        EXPECT_EQ(result.tokens[0].lexeme, "");
        EXPECT_EQ(result.tokens[0].span.start, 0u);
        EXPECT_EQ(result.tokens[0].span.end, 0u);
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerEndToken, WhitespaceAndCommentsOnlyIsJustEnd) {
        const LexResult result = lex("  \n // just a comment\n /* and a block */  ");
        ASSERT_EQ(result.tokens.size(), 1u);
        EXPECT_EQ(result.tokens[0].kind, TokenKind::End);
        EXPECT_FALSE(result.engine.hasErrors());
    }

    TEST(LexerEndToken, AlwaysPresentAndZeroLengthAtSourceEnd) {
        const LexResult result = lex("rect a {}");
        ASSERT_FALSE(result.tokens.empty());
        const Token& end = result.tokens.back();
        EXPECT_EQ(end.kind, TokenKind::End);
        EXPECT_EQ(end.lexeme, "");
        EXPECT_EQ(end.span.start, result.source.size());
        EXPECT_EQ(end.span.end, result.source.size());
    }

    // A realistic small snippet, as a sanity check that all the pieces above
    // combine the way a real .folio fragment needs them to. (Whole documents are
    // covered end-to-end by the parser goldens in tests/golden/; this is
    // deliberately small enough to read as a single token-by-token assertion.)

    TEST(LexerIntegration, SmallRealisticSnippet) {
        const LexResult result = lex("rect a { x: 1 + 2, fill: #FFF }");
        const std::vector<TokenKind> kinds = kindsWithoutEnd(result.tokens);
        const std::vector<TokenKind> expected = {
            TokenKind::Ident, TokenKind::Ident, TokenKind::LBrace,           // rect a {
            TokenKind::Ident, TokenKind::Colon, TokenKind::Number,           // x : 1
            TokenKind::Plus, TokenKind::Number, TokenKind::Comma,            // + 2 ,
            TokenKind::Ident, TokenKind::Colon, TokenKind::HexColor,         // fill : #FFF
            TokenKind::RBrace,                                              // }
        };
        EXPECT_EQ(kinds, expected) << describe(result.tokens);
        EXPECT_FALSE(result.engine.hasErrors());
    }

}
