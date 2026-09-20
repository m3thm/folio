#pragma once
#include <cstddef>
#include <string>

namespace folio {

    // Represents a contiguous range of source code using byte offsets.
    struct SourceSpan {
        std::size_t start;
        std::size_t end;

        // Helper to get the length of the span
        [[nodiscard]] std::size_t length() const {
            return (end > start) ? (end - start) : 0;
        }
    };

    struct Diagnostic {
        enum class Severity {
            Error,
            Warning,
            Note
        };

        Severity severity;
        SourceSpan span;
        std::string message;
    };

}