#pragma once
#include <numeric>
#include <vector>

namespace pixel3d {

class UnionFind {
 public:
  explicit UnionFind(size_t n) : parent_(n) { std::iota(parent_.begin(), parent_.end(), 0); }
  int find(int x) {
    while (parent_[x] != x) x = parent_[x] = parent_[parent_[x]];
    return x;
  }
  void unite(int a, int b) { parent_[find(a)] = find(b); }
  // Dense component ids 0..count-1 in order of first appearance.
  std::vector<int> components(int* count = nullptr) {
    std::vector<int> id(parent_.size(), -1), out(parent_.size());
    int next = 0;
    for (size_t i = 0; i < parent_.size(); ++i) {
      const int r = find(static_cast<int>(i));
      if (id[r] < 0) id[r] = next++;
      out[i] = id[r];
    }
    if (count) *count = next;
    return out;
  }

 private:
  std::vector<int> parent_;
};

}  // namespace pixel3d
