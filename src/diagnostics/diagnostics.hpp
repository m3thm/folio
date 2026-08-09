#pragma once
#include <string>

// Represents a contiguous range of source code using byte offsets.
struct SourceSpan {
    size_t start;
    size_t end;

    // Helper to get the length of the span
    [[nodiscard]] size_t length() const {
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