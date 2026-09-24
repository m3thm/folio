#pragma once

#include "ast/ast.hpp"
#include "diagnostics/diagnostics_engine.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Resolves the IDENT half of an IDENT.<prop> reference (LANGUAGE-SPEC.md 2.1)
// to the NodeDecl it names, and answers what `parent.<prop>` means for a
// node. `self.`/`page.` need no lookup; only a bare IDENT does.

namespace folio {

    class SymbolTable {
    public:
        // `document` must outlive the returned SymbolTable.
        // Reports a diagnostic for two siblings in the same scope sharing a name.
        static SymbolTable build(const Document& document, DiagnosticsEngine& engine);

        // Searches `from`'s enclosing scope, then each ancestor scope up to
        // the page (sibling-or-ancestor-group, per 2.1). Returns nullptr if
        // unresolved; the caller reports the diagnostic.
        [[nodiscard]] const NodeDecl* resolve(const NodeDecl* from, const std::string& name) const;

        // Nearest enclosing group, or nullptr if `node` is top-level.
        [[nodiscard]] const NodeDecl* enclosingGroup(const NodeDecl* node) const;

    private:
        struct Scope {
            const NodeDecl* owner;  // group this scope belongs to; nullptr at the root
            const Scope* parent;
            std::unordered_map<std::string, const NodeDecl*> names;
        };

        std::vector<std::unique_ptr<Scope>> scopes; // heap-allocated so Scope* stays valid
        std::unordered_map<const NodeDecl*, const Scope*> declaringScope; // node -> its siblings' scope

        Scope* addScope(const NodeDecl* owner, const Scope* parent);
        void visitLevel(const std::vector<NodeDecl>& siblings, Scope* scope, DiagnosticsEngine& engine);
    };

}