#pragma once
#include "diagnostics.hpp"
#include <vector>
#include <span>
#include <string_view>
#include <ostream>

namespace folio {
    class DiagnosticsEngine {
    public:
        // Core reporting method
        void report(Diagnostic::Severity severity, SourceSpan span, std::string message);

        void error(SourceSpan span, std::string message);
        void warning(SourceSpan span, std::string message);
        void note(SourceSpan span, std::string message);

        // State queries
        // Used [[nodiscard]] here in-case I forget to use the result of these functions when calling them.
        [[nodiscard]] bool hasErrors() const;
        [[nodiscard]] std::span<const Diagnostic> diagnostics() const;

        // Terminal output
        // `filename` is only used for display in the "file:line:col" prefix.
        void printAll(std::ostream& os, std::string_view sourceText, std::string_view filename) const;

    private:
        std::vector<Diagnostic> m_diagnostics;
        size_t m_errorCount = 0;
    };
}