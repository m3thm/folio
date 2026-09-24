#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

// Generic directed graph with topological sort + cycle detection. Used by
// reference_resolver (LANGUAGE-SPEC.md 4.2) over (node, property) keys, but
// kept generic so it can be built and tested standalone.
//
// `Key` must be hashable (std::hash<Key>) and equality-comparable.

namespace folio {

    template <typename Key>
    class DependencyGraph {
    public:
        using VertexId = std::size_t;

        // Gets or creates the vertex for `key`.
        VertexId vertex(const Key& key) {
            auto [it, inserted] = keyToId.try_emplace(key, keys.size());
            if (inserted) {
                keys.push_back(key);
                adjacency.emplace_back();
            }
            return it->second;
        }

        // `from` depends on `to`: `to` must come before `from` in topoSort().
        void addEdge(const Key& from, const Key& to) {
            addEdge(vertex(from), vertex(to));
        }

        void addEdge(VertexId from, VertexId to) {
            adjacency[from].push_back(to);
        }

        [[nodiscard]] std::size_t size() const { return keys.size(); }

        [[nodiscard]] const Key& keyOf(VertexId id) const { return keys[id]; }

        // path.front() == path.back(): a closed loop, e.g. [badge.x, logo.x, badge.x].
        struct Cycle {
            std::vector<VertexId> path;
        };

        // Evaluation order (dependencies first), or the first cycle found.
        [[nodiscard]] std::variant<std::vector<VertexId>, Cycle> topoSort() const {
            enum class Color : std::uint8_t { White, Gray, Black };
            std::vector<Color> color(keys.size(), Color::White);
            std::vector<VertexId> recStack;
            std::vector<VertexId> order;
            order.reserve(keys.size());
            std::optional<Cycle> cycle;

            // Post-order DFS over "depends on" edges: a vertex is appended
            // only after everything it depends on, which is already the
            // correct evaluation order (no reversed graph needed).
            std::function<void(VertexId)> visit = [&](VertexId v) {
                if (cycle || color[v] != Color::White) return;
                color[v] = Color::Gray;
                recStack.push_back(v);

                for (VertexId dep : adjacency[v]) {
                    if (cycle) return;
                    if (color[dep] == Color::Gray) {
                        // Back edge: slice the cycle out of the current stack.
                        auto it = std::find(recStack.begin(), recStack.end(), dep);
                        std::vector<VertexId> path(it, recStack.end());
                        path.push_back(dep);
                        cycle = Cycle{ std::move(path) };
                        return;
                    }
                    if (color[dep] == Color::White) visit(dep);
                }

                if (!cycle) {
                    recStack.pop_back();
                    color[v] = Color::Black;
                    order.push_back(v);
                }
                };

            for (VertexId v = 0; v < keys.size(); ++v) {
                if (cycle) break;
                if (color[v] == Color::White) visit(v);
            }

            if (cycle) return *cycle;
            return order;
        }

    private:
        std::vector<Key> keys;
        std::unordered_map<Key, VertexId> keyToId;
        std::vector<std::vector<VertexId>> adjacency; // out-edges = things each vertex depends on
    };

}