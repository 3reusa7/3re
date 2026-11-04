#include "pixel3d/correspond/solve.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

#include "pixel3d/common/union_find.hpp"

namespace pixel3d {
namespace {

constexpr int kMaxCoverSearch = 20;
constexpr size_t kMaxAlternatives = 8;
constexpr double kStrong = 0.9, kWeak = 0.6;

struct Block {
  int band = 0, fs = 0, ss = 0;
  std::vector<int> pieces;
  CandStatus status = CandStatus::Certain;
  std::string reason;
  bool present = false, absent = false, user_keep = false, user_remove_all = false;
  bool out = false, decided = false;
  double volume = 0;
  Mask2D::Box box{1 << 30, 1 << 30, -(1 << 30), -(1 << 30)};
};

bool runs_touch(const Run& a, const Run& b) { return a.a <= b.b && b.a <= a.b; }

const Strand& strand_of(const BandSet& bs, int band, ViewRole r, int strand) {
  return bs.view(band, r)->strands[strand];
}

// Does strand s (band b) touch strand t (band nb) across their shared boundary row?
bool strands_touch(const Band& b, const Strand& s, const Band& nb, const Strand& t) {
  if (nb.y0 == b.y1) return runs_touch(s.runs.back(), t.runs.front());
  if (nb.y1 == b.y0) return runs_touch(s.runs.front(), t.runs.back());
  return false;
}

// s and t touch each other and nothing else across the boundary.
bool one_to_one(const BandSet& bs, const Band& b, ViewRole r, int s, const Band& nb, int t) {
  const BandView* bv = bs.view(b.id, r);
  const BandView* nv = bs.view(nb.id, r);
  if (!bv || !nv || !strands_touch(b, bv->strands[s], nb, nv->strands[t])) return false;
  for (const Strand& o : nv->strands)
    if (o.id != t && strands_touch(b, bv->strands[s], nb, o)) return false;
  for (const Strand& o : bv->strands)
    if (o.id != s && strands_touch(nb, nv->strands[t], b, o)) return false;
  return true;
}

std::vector<const Band*> neighbours(const BandSet& bs, const Band& b) {
  std::vector<const Band*> out;
  for (const Band& nb : bs.bands)
    if (nb.y1 == b.y0 || nb.y0 == b.y1) out.push_back(&nb);
  return out;
}

// Family histogram of a strand's pixels in an aligned Y-view (outline excluded).
std::map<int, double> signature(const RegionMap& rm, const Strand& s, int H) {
  std::map<int, double> h;
  for (const Run& run : s.runs)
    for (int u = run.a; u < run.b; ++u) {
      const int v = H - 1 - run.y;
      if (u < 0 || v < 0 || u >= rm.w || v >= rm.h) continue;
      const int r = rm.at(u, v);
      if (r >= 0) h[rm.regions[r].family] += 1;
    }
  return h;
}

double cosine(const std::map<int, double>& a, const std::map<int, double>& b) {
  double ab = 0, aa = 0, bb = 0;
  for (const auto& [k, v] : a) {
    aa += v * v;
    if (auto it = b.find(k); it != b.end()) ab += v * it->second;
  }
  for (const auto& [k, v] : b) bb += v * v;
  return (aa == 0 || bb == 0) ? 0 : ab / std::sqrt(aa * bb);
}

class Solver {
 public:
  Solver(CandidateSet& cs, const BandSet& bands, const std::vector<CandidateEvidence>& ev,
         const std::map<ViewRole, RegionMap>& regions, const std::vector<UserDecision>& decisions)
      : cs_(cs), bands_(bands), ev_(ev), regions_(regions) {
    for (const UserDecision& d : decisions) (d.claimed ? claimed_ : d.keep ? keep_ : remove_).insert(d.candidate);
  }

  CorrespondenceResult run() {
    build_blocks();
    for (const Band& b : bands_.bands) solve_blocks(b);
    apply_block_status();
    for (const Band& b : bands_.bands) solve_pieces(b);
    apply_user_piece_decisions();
    apply_fill();
    volumes();
    build_parts();
    return std::move(res_);
  }

 private:
  const Atom& fatom(const Candidate& c) const { return bands_.view(c.band, ViewRole::Front)->atoms[c.front_atom]; }
  const Atom& satom(const Candidate& c) const { return bands_.view(c.band, ViewRole::Side)->atoms[c.side_atom]; }
  int block_of(int band, int fs, int ss) const {
    const auto it = index_.find({band, fs, ss});
    return it == index_.end() ? -1 : it->second;
  }

  void build_blocks() {
    for (Candidate& c : cs_.cands) {
      c.status = CandStatus::Certain;
      c.reason.clear();
      const auto k = std::make_tuple(c.band, fatom(c).strand, satom(c).strand);
      auto [it, ins] = index_.try_emplace(k, static_cast<int>(blocks_.size()));
      if (ins) {
        Block nb;
        nb.band = c.band;
        nb.fs = fatom(c).strand;
        nb.ss = satom(c).strand;
        blocks_.push_back(std::move(nb));
      }
      Block& b = blocks_[it->second];
      b.pieces.push_back(c.id);
      b.volume += c.volume;
      b.box.x0 = std::min(b.box.x0, c.footprint.x0), b.box.y0 = std::min(b.box.y0, c.footprint.y0);
      b.box.x1 = std::max(b.box.x1, c.footprint.x1), b.box.y1 = std::max(b.box.y1, c.footprint.y1);
    }
    for (Block& b : blocks_) {
      bool all_removed = true;
      for (int p : b.pieces) {
        b.present |= ev_[p].present();
        b.absent |= ev_[p].absent();
        b.user_keep |= keep_.contains(p);
        all_removed &= remove_.contains(p);
      }
      b.user_remove_all = all_removed;
      if (b.present) b.absent = false;
    }
  }

  std::vector<int> blocks_in(int band) const {
    std::vector<int> out;
    for (size_t i = 0; i < blocks_.size(); ++i)
      if (blocks_[i].band == band) out.push_back(static_cast<int>(i));
    return out;
  }

  std::vector<int> pieces_of(const std::vector<int>& blocks) const {
    std::vector<int> out;
    for (int b : blocks) out.insert(out.end(), blocks_[b].pieces.begin(), blocks_[b].pieces.end());
    return out;
  }

  // Minimal sets of blocks from `pool` covering the given strands (≤ kMaxAlternatives).
  std::vector<std::vector<int>> minimal_covers(const std::vector<int>& pool, const std::set<int>& need_f,
                                               const std::set<int>& need_s) const {
    std::vector<std::vector<int>> covers;
    if (need_f.empty() && need_s.empty()) return covers;
    const int n = static_cast<int>(pool.size());
    if (n > kMaxCoverSearch) return covers;
    for (int k = 1; k <= n && covers.empty(); ++k) {
      std::vector<int> pick(k);
      std::vector<bool> mask(n, false);
      std::fill(mask.begin(), mask.begin() + k, true);
      do {
        std::set<int> f, s;
        std::vector<int> chosen;
        for (int i = 0; i < n; ++i)
          if (mask[i]) {
            f.insert(blocks_[pool[i]].fs);
            s.insert(blocks_[pool[i]].ss);
            chosen.push_back(pool[i]);
          }
        if (std::includes(f.begin(), f.end(), need_f.begin(), need_f.end()) &&
            std::includes(s.begin(), s.end(), need_s.begin(), need_s.end())) {
          covers.push_back(chosen);
          if (covers.size() >= kMaxAlternatives) break;
        }
      } while (std::prev_permutation(mask.begin(), mask.end()));
    }
    return covers;
  }

  void solve_blocks(const Band& band) {
    const std::vector<int> bl = blocks_in(band.id);
    if (bl.empty()) return;
    std::set<int> fronts, sides;
    for (int b : bl) fronts.insert(blocks_[b].fs), sides.insert(blocks_[b].ss);
    // Blocks inside a user part's masks are out; the user part explains their strands.
    std::set<int> claimed_f, claimed_s;
    for (int b : bl) {
      Block& B = blocks_[b];
      bool all = !B.pieces.empty();
      for (int p : B.pieces) all &= claimed_.contains(p);
      if (!all) continue;
      B.out = true;
      B.reason = "user part mask";
      claimed_f.insert(B.fs);
      claimed_s.insert(B.ss);
    }
    for (int f : claimed_f) fronts.erase(f);
    for (int s : claimed_s) sides.erase(s);
    auto covered_without = [&](int extra) {
      std::set<int> f, s;
      for (int b : bl)
        if (!blocks_[b].out && b != extra) f.insert(blocks_[b].fs), s.insert(blocks_[b].ss);
      return std::includes(f.begin(), f.end(), fronts.begin(), fronts.end()) &&
             std::includes(s.begin(), s.end(), sides.begin(), sides.end());
    };
    // Removals: user requests first, then Absent evidence; never uncover a strand.
    for (int b : bl) {
      Block& B = blocks_[b];
      if (!B.user_remove_all || B.user_keep) continue;
      for (int p : B.pieces) handled_removals_.insert(p);
      if (covered_without(b)) B.out = true, B.reason = "user";
      else add_ambiguity("edit.uncovers_atom", band.id, B.pieces, {}, "removing it would leave a drawn strand unexplained");
    }
    for (int b : bl) {
      Block& B = blocks_[b];
      if (B.out || !B.absent || B.user_keep) continue;
      if (covered_without(b)) B.out = true, B.reason = "a Y-view shows another surface through its footprint";
      else add_ambiguity("evidence_conflict", band.id, B.pieces, {}, "Y-view says absent but the silhouettes need it");
    }
    // Present / user-kept / necessary blocks are certain.
    std::set<int> cov_f, cov_s;
    auto take = [&](int b, CandStatus st, std::string why) {
      Block& B = blocks_[b];
      B.status = st;
      B.reason = std::move(why);
      B.decided = true;
      cov_f.insert(B.fs);
      cov_s.insert(B.ss);
    };
    for (int b : bl)
      if (!blocks_[b].out && (blocks_[b].present || blocks_[b].user_keep))
        take(b, CandStatus::Certain, blocks_[b].user_keep ? "user" : "observed in a Y-view");
    for (int b : bl) {
      Block& B = blocks_[b];
      if (B.out || B.decided) continue;
      int f_users = 0, s_users = 0;
      for (int o : bl)
        if (!blocks_[o].out) f_users += blocks_[o].fs == B.fs ? 1 : 0, s_users += blocks_[o].ss == B.ss ? 1 : 0;
      if ((f_users == 1 && !claimed_f.contains(B.fs)) || (s_users == 1 && !claimed_s.contains(B.ss)))
        take(b, CandStatus::Certain, "only explanation of a drawn strand");
    }
    std::vector<int> open;
    for (int b : bl)
      if (!blocks_[b].out && !blocks_[b].decided) open.push_back(b);
    if (open.empty()) return;

    // Rule 1: 1-to-1 continuation of a kept block in a neighbouring band.
    for (int b : std::vector<int>(open)) {
      const Block& B = blocks_[b];
      for (const Band* nb : neighbours(bands_, band)) {
        for (int o : blocks_in(nb->id)) {
          const Block& O = blocks_[o];
          if (O.out) continue;
          if (one_to_one(bands_, band, ViewRole::Front, B.fs, *nb, O.fs) &&
              one_to_one(bands_, band, ViewRole::Side, B.ss, *nb, O.ss)) {
            take(b, CandStatus::InferredContinuation, "continues a kept part in the neighbouring band");
            break;
          }
        }
        if (blocks_[b].decided) break;
      }
    }
    std::erase_if(open, [&](int b) { return blocks_[b].decided; });
    if (open.empty()) return;
    std::set<int> need_f, need_s;
    for (int f : fronts)
      if (!cov_f.contains(f)) need_f.insert(f);
    for (int s : sides)
      if (!cov_s.contains(s)) need_s.insert(s);

    // Rule 2: palette signatures pin a unique assignment.
    if (palette_rule(band, open, need_f, need_s)) return;

    // Rule 3: symmetric 2×2 grid of similar strands (four-legged furniture).
    if (fronts.size() == 2 && sides.size() == 2 && bl.size() == 4 && grid_like(band)) {
      const auto covers = minimal_covers(open, need_f, need_s);
      double min_vol = 0;
      if (!covers.empty()) {
        min_vol = 1e300;
        for (const auto& c : covers) {
          double v = 0;
          for (int b : c) v += blocks_[b].volume;
          min_vol = std::min(min_vol, v);
        }
      }
      double open_vol = 0;
      for (int b : open) open_vol += blocks_[b].volume, take(b, CandStatus::InferredGrid, "2x2 grid prior (hidden members assumed)");
      res_.inferred_volume += std::max(0.0, open_vol - min_vol);
      return;
    }

    // Rule 4: ambiguous — keep the maximal solution, list the alternatives.
    std::vector<std::vector<int>> alternatives;
    for (const auto& c : minimal_covers(open, need_f, need_s)) alternatives.push_back(pieces_of(c));
    for (int b : open) take(b, CandStatus::Ambiguous, "the views do not determine which of these exist");
    add_ambiguity("ambiguous_correspondence", band.id, pieces_of(open), alternatives,
                  "several combinations of these parts explain the drawings equally well");
  }

  bool grid_like(const Band& band) const {
    const BandView* fv = bands_.view(band.id, ViewRole::Front);
    const BandView* sv = bands_.view(band.id, ViewRole::Side);
    if (fv->strands.size() != 2 || sv->strands.size() != 2) return false;
    auto width = [](const Strand& s) { return s.b - s.a; };
    // Two strands of (nearly) equal width are mirror images about their midpoint.
    return std::abs(width(fv->strands[0]) - width(fv->strands[1])) <= 1 &&
           std::abs(width(sv->strands[0]) - width(sv->strands[1])) <= 1;
  }

  bool palette_rule(const Band& band, const std::vector<int>& open, const std::set<int>& need_f,
                    const std::set<int>& need_s) {
    const auto fr = regions_.find(ViewRole::Front);
    const auto sr = regions_.find(ViewRole::Side);
    if (fr == regions_.end() || sr == regions_.end()) return false;
    std::vector<int> strong;
    std::map<int, int> strong_per_front;
    for (int b : open) {
      const Block& B = blocks_[b];
      const double c = cosine(signature(fr->second, strand_of(bands_, band.id, ViewRole::Front, B.fs), fr->second.h),
                              signature(sr->second, strand_of(bands_, band.id, ViewRole::Side, B.ss), sr->second.h));
      if (c >= kStrong) {
        strong.push_back(b);
        ++strong_per_front[B.fs];
      } else if (c > kWeak) {
        return false;  // no clear margin
      }
    }
    std::set<int> f, s;
    for (int b : strong) {
      if (strong_per_front[blocks_[b].fs] != 1) return false;  // not a unique assignment
      f.insert(blocks_[b].fs);
      s.insert(blocks_[b].ss);
    }
    if (!std::includes(f.begin(), f.end(), need_f.begin(), need_f.end()) ||
        !std::includes(s.begin(), s.end(), need_s.begin(), need_s.end()))
      return false;
    for (int b : open) {
      Block& B = blocks_[b];
      if (std::find(strong.begin(), strong.end(), b) != strong.end()) {
        B.status = CandStatus::CertainPalette;
        B.reason = "colours pair these drawn strands";
        B.decided = true;
      } else {
        B.out = true;
        B.reason = "colours contradict this pairing";
      }
    }
    return true;
  }

  void apply_block_status() {
    for (const Block& B : blocks_)
      for (int p : B.pieces) {
        cs_.cands[p].status = B.out ? CandStatus::Removed : B.status;
        cs_.cands[p].reason = B.reason;
      }
  }

  // Piece continues original (unsplit) strands of a neighbouring band whose block is kept.
  bool piece_continues(const Candidate& c) const {
    const Band& band = bands_.bands[c.band];
    const Atom& fa = fatom(c);
    const Atom& sa = satom(c);
    for (const Band* nb : neighbours(bands_, band)) {
      const BandView* nf = bands_.view(nb->id, ViewRole::Front);
      const BandView* ns = bands_.view(nb->id, ViewRole::Side);
      if (!nf || !ns) continue;
      for (const Atom& tf : nf->atoms) {
        if (tf.split || fa.a < tf.a || fa.b > tf.b) continue;
        for (const Atom& ts : ns->atoms) {
          if (ts.split || sa.a < ts.a || sa.b > ts.b) continue;
          const int ob = block_of(nb->id, tf.strand, ts.strand);
          if (ob >= 0 && !blocks_[ob].out) return true;
        }
      }
    }
    return false;
  }

  void solve_pieces(const Band& band) {
    for (const ViewRole role : {ViewRole::Front, ViewRole::Side}) {
      const BandView* bv = bands_.view(band.id, role);
      if (!bv) continue;
      for (const Atom& atom : bv->atoms) {
        if (!atom.split) continue;
        std::vector<int> pieces;
        std::set<int> owners;
        for (const Candidate& c : cs_.cands) {
          if (c.band != band.id || !is_kept(c.status)) continue;
          if ((role == ViewRole::Front ? c.front_atom : c.side_atom) != atom.id) continue;
          pieces.push_back(c.id);
          owners.insert(block_of(c.band, fatom(c).strand, satom(c).strand));
        }
        if (owners.size() < 2) continue;  // pieces of one block are fill
        std::vector<int> unsupported;
        bool any_supported = false;
        for (int p : pieces) {
          Candidate& c = cs_.cands[p];
          if (keep_.contains(p) || ev_[p].present()) {
            any_supported = true;
          } else if (piece_continues(c)) {
            any_supported = true;
            if (status_rank(c.status) < status_rank(CandStatus::InferredContinuation)) {
              c.status = CandStatus::InferredContinuation;
              c.reason = "continues a kept part through this band";
            }
          } else {
            unsupported.push_back(p);
          }
        }
        // An unsupported piece that alone explains its other atom stays.
        std::erase_if(unsupported, [&](int p) { return sole_coverer_of_other_atom(p, role); });
        if (unsupported.empty()) continue;
        for (int p : unsupported) {
          cs_.cands[p].status = CandStatus::Ambiguous;
          cs_.cands[p].reason = "its depth is not fixed by any view";
        }
        std::vector<std::vector<int>> alternatives;
        if (!any_supported)
          for (int p : unsupported)
            if (alternatives.size() < kMaxAlternatives) alternatives.push_back({p});
        add_ambiguity("ambiguous_correspondence", band.id, unsupported, alternatives,
                      any_supported ? "extra pieces that no view requires" : "the drawings do not show at which depth this piece is");
      }
    }
  }

  bool sole_coverer_of_other_atom(int p, ViewRole role) const {
    const Candidate& c = cs_.cands[p];
    const int other = role == ViewRole::Front ? c.side_atom : c.front_atom;
    for (const Candidate& o : cs_.cands) {
      if (o.id == p || o.band != c.band || !is_kept(o.status)) continue;
      if ((role == ViewRole::Front ? o.side_atom : o.front_atom) == other) return false;
    }
    return true;
  }

  void apply_user_piece_decisions() {
    for (int p : claimed_) {
      if (p < 0 || p >= static_cast<int>(cs_.cands.size())) continue;
      cs_.cands[p].status = CandStatus::Removed;
      cs_.cands[p].reason = "user part mask";
    }
    for (int p : keep_) {
      if (p < 0 || p >= static_cast<int>(cs_.cands.size())) continue;
      cs_.cands[p].status = CandStatus::Certain;
      cs_.cands[p].reason = "user";
    }
    for (int p : remove_) {
      if (p < 0 || p >= static_cast<int>(cs_.cands.size()) || keep_.contains(p) || handled_removals_.contains(p)) continue;
      Candidate& c = cs_.cands[p];
      if (!is_kept(c.status)) continue;  // already removed at block level
      bool f_ok = false, s_ok = false;
      for (const Candidate& o : cs_.cands) {
        if (o.id == p || o.band != c.band || !is_kept(o.status)) continue;
        f_ok |= o.front_atom == c.front_atom;
        s_ok |= o.side_atom == c.side_atom;
      }
      if (f_ok && s_ok) {
        c.status = CandStatus::Removed;
        c.reason = "user";
      } else {
        add_ambiguity("edit.uncovers_atom", c.band, {p}, {}, "removing it would leave a drawn strand unexplained");
      }
    }
    // Ambiguities whose pieces the user has fully decided are resolved — but only
    // if every decision took effect (a refused removal leaves the piece ambiguous).
    std::erase_if(res_.ambiguities, [&](const Ambiguity& a) {
      if (a.kind != "ambiguous_correspondence") return false;
      for (int p : a.candidates) {
        if (!keep_.contains(p) && !remove_.contains(p) && !claimed_.contains(p)) return false;
        if (p >= 0 && p < static_cast<int>(cs_.cands.size()) && cs_.cands[p].status == CandStatus::Ambiguous)
          return false;
      }
      return true;
    });
  }

  void apply_fill() {
    for (const Block& B : blocks_) {
      if (B.out || B.present || B.status != CandStatus::Certain || B.reason == "user") continue;
      if (B.box.width() < 4 || B.box.height() < 4) continue;
      for (int p : B.pieces) {
        Candidate& c = cs_.cands[p];
        if (c.status != CandStatus::Certain || keep_.contains(p)) continue;
        c.status = CandStatus::InferredFill;
        c.reason = "solid interior assumed (not visible in any view)";
        const double fw = B.box.width(), fd = B.box.height();
        res_.inferred_volume += c.volume * std::max(0.0, (fw - 2) * (fd - 2)) / (fw * fd);
      }
    }
  }

  void volumes() {
    for (const Candidate& c : cs_.cands) {
      if (c.status == CandStatus::InferredContinuation) res_.inferred_volume += c.volume;
      if (c.status == CandStatus::Ambiguous) res_.ambiguous_volume += c.volume;
    }
  }

  void build_parts() {
    UnionFind uf(blocks_.size());
    for (size_t i = 0; i < blocks_.size(); ++i) {
      const Block& A = blocks_[i];
      if (A.out) continue;
      for (const Band* nb : neighbours(bands_, bands_.bands[A.band]))
        for (int j : blocks_in(nb->id)) {
          const Block& B = blocks_[j];
          if (!B.out && one_to_one(bands_, bands_.bands[A.band], ViewRole::Front, A.fs, *nb, B.fs) &&
              one_to_one(bands_, bands_.bands[A.band], ViewRole::Side, A.ss, *nb, B.ss))
            uf.unite(static_cast<int>(i), j);
        }
    }
    res_.part_of_candidate.assign(cs_.cands.size(), -1);
    std::map<int, int> part_id;
    for (size_t i = 0; i < blocks_.size(); ++i) {
      if (blocks_[i].out) continue;
      const int root = uf.find(static_cast<int>(i));
      auto [it, ins] = part_id.try_emplace(root, static_cast<int>(res_.parts.size()));
      if (ins) {
        Part3D p;
        p.id = it->second;
        p.label = "part_" + std::to_string(p.id);
        res_.parts.push_back(p);
      }
      Part3D& part = res_.parts[it->second];
      for (int c : blocks_[i].pieces) {
        if (!is_kept(cs_.cands[c].status)) continue;
        part.candidates.push_back(c);
        res_.part_of_candidate[c] = part.id;
        if (status_rank(cs_.cands[c].status) > status_rank(part.status)) part.status = cs_.cands[c].status;
      }
    }
    std::erase_if(res_.parts, [](const Part3D& p) { return p.candidates.empty(); });
    for (size_t i = 0; i < res_.parts.size(); ++i) {
      res_.parts[i].id = static_cast<int>(i);
      res_.parts[i].label = "part_" + std::to_string(i);
      for (int c : res_.parts[i].candidates) res_.part_of_candidate[c] = static_cast<int>(i);
    }
  }

  void add_ambiguity(std::string kind, int band, std::vector<int> cands, std::vector<std::vector<int>> alts,
                     std::string text) {
    res_.ambiguities.push_back(Ambiguity{std::move(kind), band, std::move(cands), std::move(alts), std::move(text)});
  }

  CandidateSet& cs_;
  const BandSet& bands_;
  const std::vector<CandidateEvidence>& ev_;
  const std::map<ViewRole, RegionMap>& regions_;
  std::set<int> keep_, remove_, claimed_, handled_removals_;
  std::vector<Block> blocks_;
  std::map<std::tuple<int, int, int>, int> index_;
  CorrespondenceResult res_;
};

}  // namespace

int status_rank(CandStatus s) {
  switch (s) {
    case CandStatus::Removed: return -1;
    case CandStatus::Certain: return 0;
    case CandStatus::CertainPalette: return 1;
    case CandStatus::InferredContinuation: return 2;
    case CandStatus::InferredGrid: return 3;
    case CandStatus::InferredFill: return 4;
    case CandStatus::Ambiguous: return 5;
  }
  return 0;
}

CorrespondenceResult solve_correspondence(CandidateSet& cands, const BandSet& bands,
                                          const std::vector<CandidateEvidence>& evidence,
                                          const std::map<ViewRole, RegionMap>& regions,
                                          const std::vector<UserDecision>& decisions) {
  return Solver(cands, bands, evidence, regions, decisions).run();
}

}  // namespace pixel3d
