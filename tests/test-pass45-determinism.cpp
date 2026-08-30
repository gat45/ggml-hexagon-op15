// test-pass45-determinism.cpp
//
// PERMANENT regression test for the pass-4.5 topological reorder fix
// (ggml-backend.cpp: ggml_backend_sched_split_graph, pass 4.5).
//
// Verifies the critical invariant on which the memory planner depends:
//   the reorder must be DETERMINISTIC — for structurally identical graphs it
//   must always produce the same node order (reserve graph == compute graph),
//   otherwise gallocr reuses a memory plan built on a different order,
//   silently corrupting memory (see tests/test-alloc.cpp
//   test_adversarial_topology_change + ggml_gallocr_debug_mismatch_count).
//
// This mirrors the OLD (O(n^2)) and NEW (O(n+E)) pass-4.5 implementations
// verbatim and asserts they produce IDENTICAL orders across many graphs,
// including duplicate-source patterns.
//
// Build (host, no ggml dependency):
//   g++ -O2 -std=c++17 -o test-pass45-determinism test-pass45-determinism.cpp
//   ./test-pass45-determinism

#include <cstdio>
#include <cstdint>
#include <vector>
#include <queue>
#include <unordered_map>
#include <random>
#include <algorithm>

struct Node {
    int backend;
    intptr_t ptr;          // identity "ggml_tensor*" (== index here)
    std::vector<intptr_t> src_ptrs;
};

// ---- OLD O(n^2) pass-4.5 (verbatim logic from commit 859926e7) ----
static std::vector<int> pass45_old(const std::vector<Node> & nodes, int n_backends) {
    const int n = (int) nodes.size();
    std::vector<int> in_degree(n, 0);
    std::vector<std::vector<int>> deps(n);
    for (int i = 0; i < n; i++) {
        for (intptr_t sp : nodes[i].src_ptrs) {
            int k = -1;
            for (int kk = 0; kk < n; kk++) {
                if (nodes[kk].ptr == sp) { k = kk; break; }
            }
            if (k < 0) continue;
            deps[i].push_back(k);
            in_degree[i]++;
        }
    }
    auto cmp = [&](int a, int b) {
        int ba = nodes[a].backend, bb = nodes[b].backend;
        bool a_is_gpu = (ba < n_backends - 1);
        bool b_is_gpu = (bb < n_backends - 1);
        if (a_is_gpu != b_is_gpu) return b_is_gpu;
        return in_degree[a] > in_degree[b];
    };
    std::priority_queue<int, std::vector<int>, decltype(cmp)> pq(cmp);
    std::vector<bool> visited(n, false);
    for (int i = 0; i < n; i++) if (in_degree[i] == 0) pq.push(i);
    std::vector<int> result;
    while (!pq.empty()) {
        int idx = pq.top(); pq.pop();
        if (visited[idx]) continue;
        visited[idx] = true;
        result.push_back(idx);
        for (int i = 0; i < n; i++) {
            if (visited[i]) continue;
            for (int k : deps[i]) {
                if (k == idx) {
                    in_degree[i]--;
                    if (in_degree[i] == 0) pq.push(i);
                    break;
                }
            }
        }
    }
    for (int i = 0; i < n; i++) if (!visited[i]) result.push_back(i);
    return result;
}

// ---- NEW O(n+E) pass-4.5 (the applied fix) ----
static std::vector<int> pass45_new(const std::vector<Node> & nodes, int n_backends) {
    const int n = (int) nodes.size();
    std::unordered_map<intptr_t, int> node_index;
    node_index.reserve(n);
    for (int i = 0; i < n; i++) node_index[nodes[i].ptr] = i;
    std::vector<int> in_degree(n, 0);
    std::vector<std::vector<int>> deps(n);
    std::vector<std::vector<int>> dependents(n);
    for (int i = 0; i < n; i++) {
        for (intptr_t sp : nodes[i].src_ptrs) {
            auto it = node_index.find(sp);
            if (it == node_index.end()) continue;
            const int k = it->second;
            bool already = false;
            for (int d : deps[i]) {
                if (d == k) { already = true; break; }
            }
            deps[i].push_back(k);
            in_degree[i]++;
            if (!already) dependents[k].push_back(i);
        }
    }
    auto cmp = [&](int a, int b) {
        int ba = nodes[a].backend, bb = nodes[b].backend;
        bool a_is_gpu = (ba < n_backends - 1);
        bool b_is_gpu = (bb < n_backends - 1);
        if (a_is_gpu != b_is_gpu) return b_is_gpu;
        return in_degree[a] > in_degree[b];
    };
    std::priority_queue<int, std::vector<int>, decltype(cmp)> pq(cmp);
    std::vector<bool> visited(n, false);
    for (int i = 0; i < n; i++) if (in_degree[i] == 0) pq.push(i);
    std::vector<int> result;
    while (!pq.empty()) {
        int idx = pq.top(); pq.pop();
        if (visited[idx]) continue;
        visited[idx] = true;
        result.push_back(idx);
        for (int i : dependents[idx]) {
            if (visited[i]) continue;
            in_degree[i]--;
            if (in_degree[i] == 0) pq.push(i);
        }
    }
    for (int i = 0; i < n; i++) if (!visited[i]) result.push_back(i);
    return result;
}

static std::vector<Node> build_pp_like(int n_layers, int ops_per_layer, int n_backends,
                                       unsigned seed, int dup_mode) {
    std::mt19937 rng(seed);
    const int n = n_layers * ops_per_layer;
    std::vector<Node> nodes(n);
    for (int i = 0; i < n; i++) {
        nodes[i].backend = (rng() % 5 == 0) ? (n_backends - 1) : 0;
        nodes[i].ptr = i;
    }
    for (int l = 1; l < n_layers; l++) {
        for (int o = 0; o < ops_per_layer; o++) {
            int i = l * ops_per_layer + o;
            int prev = (l - 1) * ops_per_layer;
            int nd = 1 + (int) (rng() % 3);
            for (int d = 0; d < nd; d++) {
                int k = prev + (int) (rng() % ops_per_layer);
                nodes[i].src_ptrs.push_back(k);
                if (dup_mode == 1 && rng() % 4 == 0) {
                    nodes[i].src_ptrs.push_back(k);
                } else if (dup_mode == 2 && rng() % 4 == 0) {
                    int j = prev + (int) (rng() % ops_per_layer);
                    nodes[i].src_ptrs.push_back(j);
                    nodes[i].src_ptrs.push_back(k);
                }
            }
            if (o > 0 && rng() % 3 == 0) nodes[i].src_ptrs.push_back(i - 1);
        }
    }
    return nodes;
}

static bool valid_topo(const std::vector<Node> & nodes, const std::vector<int> & order) {
    std::vector<int> pos(nodes.size(), -1);
    for (int i = 0; i < (int) order.size(); i++) pos[order[i]] = i;
    for (int i = 0; i < (int) nodes.size(); i++) {
        if (pos[i] < 0) return false;
        for (intptr_t sp : nodes[i].src_ptrs) {
            int k = (int) sp;
            if (pos[k] < 0 || pos[k] > pos[i]) return false;
        }
    }
    return true;
}

int main() {
    const int n_backends = 2;
    int fails = 0;
    for (int dup_mode = 0; dup_mode <= 2; dup_mode++) {
        for (int seed = 0; seed < 150; seed++) {
            auto nodes = build_pp_like(40 + seed % 20, 20, n_backends, seed * 3 + dup_mode, dup_mode);
            auto old = pass45_old(nodes, n_backends);
            auto now = pass45_new(nodes, n_backends);
            if (old != now) {
                printf("FAIL: order differs dup_mode=%d seed=%d\n", dup_mode, seed);
                fails++;
                goto end;
            }
            if (!valid_topo(nodes, now)) {
                printf("FAIL: invalid topology dup_mode=%d seed=%d\n", dup_mode, seed);
                fails++;
                goto end;
            }
        }
    }
    printf("PASS: old == new on 450 graphs (dup modes 0/1/2), topology valid\n");
end:
    printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", fails);
    return fails ? 1 : 0;
}