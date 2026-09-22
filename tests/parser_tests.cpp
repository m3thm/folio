// Parser tests, mostly golden-file based (see ARCHITECTURE.md, "Testing").
//
// Every `.folio` file under tests/golden/, and every one under examples/, is run
// through the real pipeline (lexer -> parser). What comes out is compared with a
// checked-in `<name>.expected.txt`:
//
//   - parse succeeded  -> the canonical AST dump from printAst()
//   - parse reported errors -> the diagnostics text (colors stripped)
//
// so one harness covers both "the tree is right" and "the error message and its
// location are right". Asserting on a whole dump is far less brittle than
// asserting on individual AST fields, and a failure shows exactly what changed.
//
// Adding a case: drop a `name.folio` into tests/golden/, run the tests once with
// FOLIO_UPDATE_GOLDEN=1 to create `name.expected.txt`, then READ the generated
// file and check it against what the source should mean before committing it.
// A golden file you didn't review only proves the parser agrees with itself.
//
// Expected files for examples/foo.folio live in tests/golden/examples/foo.expected.txt.
//
// Regenerating after an intended change (e.g. to the dump format):
//   Linux/macOS:  FOLIO_UPDATE_GOLDEN=1 ctest --test-dir out/build/debug
//   PowerShell:   $env:FOLIO_UPDATE_GOLDEN=1; ctest --test-dir out/build/debug
// then review `git diff tests/golden`.

#include "ast/ast_printer.hpp"
#include "diagnostics/diagnostics_engine.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef FOLIO_SOURCE_DIR
#error "FOLIO_SOURCE_DIR must be defined by the build (see the folio_tests target in CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

namespace {

    // Running the pipeline

    struct ParseResult {
        std::string source;
        folio::DiagnosticsEngine engine;
        folio::Document document;
    };

    ParseResult parseSource(std::string source) {
        ParseResult result;
        result.source = std::move(source);
        folio::Lexer lexer(result.source, result.engine);
        std::vector<folio::Token> tokens = lexer.tokenize();
        folio::Parser parser(std::move(tokens), result.engine);
        result.document = parser.parse();
        return result;
    }

    std::string stripAnsi(const std::string& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
                i += 2;
                while (i < text.size() && text[i] != 'm') ++i; // skip to the end of the SGR sequence
                continue;
            }
            out += text[i];
        }
        return out;
    }

    // Git may check text files out with CRLF on Windows; goldens compare on LF only.
    std::string normalizeNewlines(std::string text) {
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    }

    // What a golden file holds for this parse (see the header comment).
    std::string render(const fs::path& sourcePath, const ParseResult& result) {
        std::string out;
        if (!result.engine.diagnostics().empty()) {
            std::ostringstream text;
            result.engine.printAll(text, result.source, sourcePath.filename().string());
            out += stripAnsi(text.str());
        }
        if (!result.engine.hasErrors()) {
            out += folio::printAst(result.document);
        }
        return normalizeNewlines(std::move(out));
    }

    // Files

    std::optional<std::string> readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }

    bool writeFile(const fs::path& path, const std::string& contents) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << contents;
        return static_cast<bool>(out);
    }

    bool updateRequested() {
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t length = 0;
        const bool requested = _dupenv_s(&value, &length, "FOLIO_UPDATE_GOLDEN") == 0 &&
            value != nullptr && value[0] != '\0' && value[0] != '0';
        std::free(value);
        return requested;
#else
        const char* value = std::getenv("FOLIO_UPDATE_GOLDEN");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
#endif
    }

    // Golden cases

    struct GoldenCase {
        std::string name;   // gtest-safe test name
        fs::path source;    // the .folio input
        fs::path expected;  // the checked-in expected output
    };

    void PrintTo(const GoldenCase& golden, std::ostream* os) { *os << golden.name; }

    fs::path goldenDir() { return fs::path(FOLIO_SOURCE_DIR) / "tests" / "golden"; }
    fs::path examplesDir() { return fs::path(FOLIO_SOURCE_DIR) / "examples"; }

    std::string sanitizeForTestName(std::string name) {
        for (char& c : name) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!ok) c = '_';
        }
        return name;
    }

    std::vector<GoldenCase> collectGoldenCases() {
        std::vector<GoldenCase> cases;

        const auto collect = [&](const fs::path& sourceDir, const fs::path& expectedDir, const std::string& prefix) {
            std::error_code ec;
            if (!fs::is_directory(sourceDir, ec)) return;
            for (const fs::directory_entry& entry : fs::directory_iterator(sourceDir, ec)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".folio") continue;
                const std::string stem = entry.path().stem().string();
                cases.push_back(GoldenCase{ prefix + sanitizeForTestName(stem), entry.path(), expectedDir / (stem + ".expected.txt") });
            }
        };

        collect(goldenDir(), goldenDir(), "");
        collect(examplesDir(), goldenDir() / "examples", "example_");

        std::sort(cases.begin(), cases.end(), [](const GoldenCase& a, const GoldenCase& b) { return a.name < b.name; });
        return cases;
    }

    std::vector<std::string> splitLines(const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        return lines;
    }

    // A failure message that says where the outputs diverge, then shows the
    // whole actual output so it can be copied or regenerated.
    std::string describeMismatch(const GoldenCase& golden, const std::string& expected, const std::string& actual) {
        const std::vector<std::string> expectedLines = splitLines(expected);
        const std::vector<std::string> actualLines = splitLines(actual);

        std::size_t line = 0;
        while (line < expectedLines.size() && line < actualLines.size() && expectedLines[line] == actualLines[line]) ++line;

        std::ostringstream message;
        message << "output differs from " << golden.expected.generic_string() << "\n"
                << "first difference at line " << (line + 1) << ":\n"
                << "  expected: " << (line < expectedLines.size() ? expectedLines[line] : std::string("<end of file>")) << "\n"
                << "  actual:   " << (line < actualLines.size() ? actualLines[line] : std::string("<end of file>")) << "\n\n"
                << "----- actual output -----\n" << actual << "----- end of actual output -----\n"
                << "If this change is intended, regenerate with FOLIO_UPDATE_GOLDEN=1 and review the diff.";
        return message.str();
    }

    // The golden-file test

    class GoldenTest : public ::testing::TestWithParam<GoldenCase> {};

    TEST_P(GoldenTest, OutputMatchesExpectedFile) {
        const GoldenCase& golden = GetParam();

        const std::optional<std::string> source = readFile(golden.source);
        ASSERT_TRUE(source.has_value()) << "cannot read " << golden.source.generic_string();

        const ParseResult result = parseSource(*source);

        // The printer has to cope with whatever the parser leaves behind after
        // errors (half-filled nodes), not just with clean trees.
        EXPECT_NO_THROW({ (void)folio::printAst(result.document); });

        const std::string actual = render(golden.source, result);

        if (updateRequested()) {
            ASSERT_TRUE(writeFile(golden.expected, actual)) << "cannot write " << golden.expected.generic_string();
            SUCCEED() << "updated " << golden.expected.generic_string();
            return;
        }

        const std::optional<std::string> expected = readFile(golden.expected);
        if (!expected) {
            FAIL() << "missing " << golden.expected.generic_string()
                   << "\nCreate it with FOLIO_UPDATE_GOLDEN=1, then review it before committing.\n"
                   << "----- actual output -----\n" << actual << "----- end of actual output -----";
        }

        if (normalizeNewlines(*expected) != actual) {
            FAIL() << describeMismatch(golden, normalizeNewlines(*expected), actual);
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Fixtures, GoldenTest, ::testing::ValuesIn(collectGoldenCases()),
        [](const ::testing::TestParamInfo<GoldenCase>& info) { return info.param.name; });

    // Guards on the fixtures themselves

    TEST(GoldenFixtures, DirectoriesAreFoundAndNotEmpty) {
        // Without this, a wrong FOLIO_SOURCE_DIR would just produce zero golden
        // tests and everything would look green.
        EXPECT_TRUE(fs::is_directory(goldenDir())) << goldenDir().generic_string();
        EXPECT_FALSE(collectGoldenCases().empty());
    }

    TEST(GoldenFixtures, NoExpectedFileIsOrphaned) {
        // A renamed or deleted .folio would otherwise leave a stale expected
        // file behind that nothing checks any more.
        std::set<std::string> known;
        for (const GoldenCase& golden : collectGoldenCases()) known.insert(golden.expected.lexically_normal().generic_string());

        for (const fs::path& dir : { goldenDir(), goldenDir() / "examples" }) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
                const std::string name = entry.path().filename().string();
                if (!entry.is_regular_file() || name.size() < 13 || name.substr(name.size() - 13) != ".expected.txt") continue;
                EXPECT_TRUE(known.count(entry.path().lexically_normal().generic_string()) == 1)
                    << name << " has no matching .folio source";
            }
        }
    }

    // Properties of the printer that a golden file can't express

    TEST(AstPrinter, PropertyOrderInSourceDoesNotChangeTheDump) {
        // The dump is canonical: written in any order, the same node prints the same.
        const ParseResult a = parseSource("page { size: A4 }\nrect r { x: 1 y: 2 width: 3 height: 4 fill: #FFF opacity: 0.5 }");
        const ParseResult b = parseSource("page { size: A4 }\nrect r { opacity: 0.5 fill: #FFF height: 4 width: 3 y: 2 x: 1 }");
        ASSERT_FALSE(a.engine.hasErrors());
        ASSERT_FALSE(b.engine.hasErrors());
        EXPECT_EQ(folio::printAst(a.document), folio::printAst(b.document));
    }

    TEST(AstPrinter, LocalVariablesKeepSourceOrder) {
        // Unlike properties, `name = expr` statements are order-sensitive, so their order is real data.
        const ParseResult result = parseSource("page { size: A4 }\nrect r { second = 1 first = 2 }");
        ASSERT_FALSE(result.engine.hasErrors());
        const std::string dump = folio::printAst(result.document);
        ASSERT_NE(dump.find("local second"), std::string::npos);
        ASSERT_NE(dump.find("local first"), std::string::npos);
        EXPECT_LT(dump.find("local second"), dump.find("local first"));
    }

    TEST(AstPrinter, StreamAndStringOverloadsAgree) {
        const ParseResult result = parseSource("page { size: A4 }\nrect r { x: 1 + 2 }");
        std::ostringstream stream;
        folio::printAst(stream, result.document);
        EXPECT_EQ(stream.str(), folio::printAst(result.document));
    }

    TEST(AstPrinter, OutputEndsWithNewlineAndStartsWithDocument) {
        const ParseResult result = parseSource("page { size: A4 }");
        const std::string dump = folio::printAst(result.document);
        EXPECT_EQ(dump, "document\n  page\n    size: A4\n");
    }

}
