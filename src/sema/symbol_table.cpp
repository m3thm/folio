#include "sema/symbol_table.hpp"

namespace folio {

    SymbolTable::Scope* SymbolTable::addScope(const NodeDecl* owner, const Scope* parent) {
        scopes.push_back(std::make_unique<Scope>(Scope{ owner, parent, {} }));
        return scopes.back().get();
    }

    void SymbolTable::visitLevel(const std::vector<NodeDecl>& siblings, Scope* scope, DiagnosticsEngine& engine) {
        // Register all siblings before recursing, so source order never matters.
        for (const NodeDecl& node : siblings) {
            declaringScope.emplace(&node, scope);
            if (!node.name) continue;

            auto [it, inserted] = scope->names.emplace(*node.name, &node);
            if (!inserted) {
                engine.error(node.span,
                    "'" + *node.name + "' is already used as a node name in this scope");
            }
        }

        for (const NodeDecl& node : siblings) {
            if (node.type != NodeType::Group) continue;
            Scope* childScope = addScope(&node, scope);
            visitLevel(node.children, childScope, engine);
        }
    }

    SymbolTable SymbolTable::build(const Document& document, DiagnosticsEngine& engine) {
        SymbolTable table;
        Scope* root = table.addScope(/*owner=*/nullptr, /*parent=*/nullptr);
        table.visitLevel(document.nodes, root, engine);
        return table;
    }

    const NodeDecl* SymbolTable::resolve(const NodeDecl* from, const std::string& name) const {
        auto it = declaringScope.find(from);
        if (it == declaringScope.end()) return nullptr;

        for (const Scope* scope = it->second; scope != nullptr; scope = scope->parent) {
            auto found = scope->names.find(name);
            if (found != scope->names.end()) return found->second;
        }
        return nullptr;
    }

    const NodeDecl* SymbolTable::enclosingGroup(const NodeDecl* node) const {
        auto it = declaringScope.find(node);
        if (it == declaringScope.end()) return nullptr;
        return it->second->owner;
    }

}