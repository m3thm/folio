#include "diagnostics_engine.hpp"
#include <algorithm>

namespace folio {
    void DiagnosticsEngine::report(Diagnostic::Severity severity, SourceSpan span, std::string message) {
        if (severity == Diagnostic::Severity::Error) {
            m_errorCount++;
        }
        m_diagnostics.push_back(Diagnostic{ severity, span, std::move(message) });
    }

    void DiagnosticsEngine::error(SourceSpan span, std::string message) {
        report(Diagnostic::Severity::Error, span, std::move(message));
    }

    void DiagnosticsEngine::warning(SourceSpan span, std::string message) {
        report(Diagnostic::Severity::Warning, span, std::move(message));
    }

    void DiagnosticsEngine::note(SourceSpan span, std::string message) {
        report(Diagnostic::Severity::Note, span, std::move(message));
    }

    bool DiagnosticsEngine::hasErrors() const {
        return m_errorCount > 0;
    }

    std::span<const Diagnostic> DiagnosticsEngine::diagnostics() const {
        return m_diagnostics;
    }

    // Helper struct for line resolution
    struct LineInfo {
        size_t lineNum;
        size_t colNum;
        std::string_view lineText;
    };

    // Computes the line and column number from a raw byte offset
    static LineInfo resolveLineInfo(std::string_view source, size_t offset) {
        size_t line = 1;
        size_t lineStart = 0;

        // Find the line number and the start of the current line
        for (size_t i = 0; i < offset && i < source.size(); ++i) {
            if (source[i] == '\n') {
                line++;
                lineStart = i + 1;
            }
        }

        // Find the end of the current line
        size_t lineEnd = lineStart;
        while (lineEnd < source.size() && source[lineEnd] != '\n' && source[lineEnd] != '\r') {
            lineEnd++;
        }

        size_t col = (offset >= lineStart) ? (offset - lineStart + 1) : 1;
        return { line, col, source.substr(lineStart, lineEnd - lineStart) };
    }

    void DiagnosticsEngine::printAll(std::ostream& os, std::string_view sourceText) const {
        // ANSI color codes
        constexpr const char* COLOR_RED = "\033[1;31m";
        constexpr const char* COLOR_YELLOW = "\033[1;33m";
        constexpr const char* COLOR_CYAN = "\033[1;36m";
        constexpr const char* COLOR_RESET = "\033[0m";
        constexpr const char* BOLD = "\033[1m";

        for (const auto& diag : m_diagnostics) {
            LineInfo info = resolveLineInfo(sourceText, diag.span.start);

            const char* colorCode = COLOR_RESET;
            const char* severityStr = "";

            switch (diag.severity) {
            case Diagnostic::Severity::Error:
                severityStr = "error";   colorCode = COLOR_RED;    break;
            case Diagnostic::Severity::Warning:
                severityStr = "warning"; colorCode = COLOR_YELLOW; break;
            case Diagnostic::Severity::Note:
                severityStr = "note";    colorCode = COLOR_CYAN;   break;
            }

            // 1. Print file/line/col and message
            os << BOLD << "file.src:" << info.lineNum << ":" << info.colNum << ": "
                << colorCode << severityStr << ": " << COLOR_RESET
                << BOLD << diag.message << COLOR_RESET << "\n";

            // 2. Print the source code line
            os << " " << info.lineText << "\n";

            // 3. Print the squiggly underline / caret
            os << " ";
            for (size_t i = 1; i < info.colNum; ++i) {
                // Handle tabs in source text so the caret aligns correctly
                if (i - 1 < info.lineText.size() && info.lineText[i - 1] == '\t') {
                    os << "\t";
                }
                else {
                    os << " ";
                }
            }

            os << colorCode << "^";
            size_t squigglyLength = diag.span.length() > 0 ? diag.span.length() - 1 : 0;
            for (size_t i = 0; i < squigglyLength; ++i) {
                os << "~";
            }
            os << COLOR_RESET << "\n\n";
        }
    }
}