#include "Simulator.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <immintrin.h>
#include <stdexcept>

namespace qe {

std::ostream& operator<<(std::ostream& os, const complex_t& z) {
  return os << "(" << z.real() << ", " << z.imag() << ")";
}

Simulator::Simulator(int num_qubits) : n_qubits_(num_qubits) {
  if (n_qubits_ <= 0 || n_qubits_ > 62) {
    throw std::invalid_argument("num_qubits must be in [1, 62]");
  }
  TileManager::Options opts;
  opts.backing_dir = std::filesystem::current_path() / ".quantumengine_state";
  opts.tile_size = 1ull << 20;
  opts.max_cached_tiles = 8;
  opts.fsync_on_flush = false;
  tiles_ = std::make_unique<TileManager>(static_cast<std::uint32_t>(n_qubits_), opts);
  initialize_zero_state();
}

bool Simulator::use_sparse() const { return !dense_mode_ && sparse_support_.size() <= kSparseSupportCap; }

void Simulator::initialize_zero_state() {
  sparse_support_ = {0};
  dense_mode_ = false;
  tiles_->set_amplitude(0, Complex{1.0, 0.0});
  tiles_->flush();
  tiles_->prune_cached_tiles(kPruneAbs);
}

void Simulator::apply_gate(const std::string& name, int q) {
  if (name == "H") {
    apply_h(static_cast<std::uint32_t>(q));
  } else if (name == "X") {
    apply_x(static_cast<std::uint32_t>(q));
  } else {
    throw std::invalid_argument("Unknown 1-qubit gate: " + name);
  }
}

void Simulator::apply_gate(const std::string& name, int control, int target) {
  if (name == "CNOT") {
    apply_cnot(static_cast<std::uint32_t>(control), static_cast<std::uint32_t>(target));
  } else {
    throw std::invalid_argument("Unknown 2-qubit gate: " + name);
  }
}

complex_t Simulator::get_amplitude(std::uint64_t index) {
  const Complex a = tiles_->get_amplitude(index);
  return complex_t{a.re, a.im};
}

void Simulator::export_active_tiles_for_test(const std::filesystem::path& out_dir) {
  tiles_->flush();
  tiles_->export_active_tiles_as_files(out_dir);
}

void Simulator::export_final_state_bin_for_test(const std::filesystem::path& out_file) {
  tiles_->flush();
  tiles_->export_active_tiles_to_flat_bin(out_file);
}

void Simulator::apply_h(std::uint32_t q) { apply_xor_style_1q(q, GateKind::H); }
void Simulator::apply_x(std::uint32_t q) { apply_xor_style_1q(q, GateKind::X); }

void Simulator::apply_xor_style_1q(std::uint32_t q, GateKind kind) {
  if (q >= static_cast<std::uint32_t>(n_qubits_)) throw std::out_of_range("qubit out of range");
  if (use_sparse()) {
    if (kind == GateKind::H) {
      apply_h_sparse(q);
    } else {
      apply_x_sparse(q);
    }
    return;
  }
  if (kind == GateKind::H) {
    apply_h_dense(q);
  } else {
    apply_x_dense(q);
  }
}

void Simulator::apply_h_sparse(std::uint32_t q) {
  const std::uint64_t bit = 1ull << q;
  std::unordered_set<std::uint64_t> bases;
  bases.reserve(sparse_support_.size() * 2 + 1);
  for (const auto idx : sparse_support_) {
    bases.insert(idx & ~bit);
  }
  constexpr double kEps2 = 1e-30;
  const double s = 1.0 / std::sqrt(2.0);
  std::unordered_set<std::uint64_t> next;
  next.reserve(bases.size() * 2 + 1);

  for (const auto base : bases) {
    const std::uint64_t i = base;
    const std::uint64_t j = base | bit;
    Complex v0 = tiles_->get_amplitude(i);
    Complex v1 = tiles_->get_amplitude(j);
    const double re0 = v0.re, im0 = v0.im;
    const double re1 = v1.re, im1 = v1.im;
    const Complex r0{(re0 + re1) * s, (im0 + im1) * s};
    const Complex r1{(re0 - re1) * s, (im0 - im1) * s};
    tiles_->set_amplitude(i, r0);
    tiles_->set_amplitude(j, r1);
    if (r0.re * r0.re + r0.im * r0.im > kEps2) next.insert(i);
    if (r1.re * r1.re + r1.im * r1.im > kEps2) next.insert(j);
  }
  sparse_support_.swap(next);
  tiles_->flush();
  tiles_->prune_cached_tiles(kPruneAbs);
}

void Simulator::apply_x_sparse(std::uint32_t q) {
  const std::uint64_t bit = 1ull << q;
  std::unordered_set<std::uint64_t> bases;
  bases.reserve(sparse_support_.size() * 2 + 1);
  for (const auto idx : sparse_support_) {
    bases.insert(idx & ~bit);
  }
  constexpr double kEps2 = 1e-30;
  std::unordered_set<std::uint64_t> next;
  next.reserve(bases.size() * 2 + 1);

  for (const auto base : bases) {
    const std::uint64_t i = base;
    const std::uint64_t j = base | bit;
    Complex v0 = tiles_->get_amplitude(i);
    Complex v1 = tiles_->get_amplitude(j);
    tiles_->set_amplitude(i, v1);
    tiles_->set_amplitude(j, v0);
    if (v1.re * v1.re + v1.im * v1.im > kEps2) next.insert(i);
    if (v0.re * v0.re + v0.im * v0.im > kEps2) next.insert(j);
  }
  sparse_support_.swap(next);
  tiles_->flush();
  tiles_->prune_cached_tiles(kPruneAbs);
}

void Simulator::apply_cnot(std::uint32_t control, std::uint32_t target) {
  if (control >= static_cast<std::uint32_t>(n_qubits_) || target >= static_cast<std::uint32_t>(n_qubits_)) {
    throw std::out_of_range("qubit out of range");
  }
  if (control == target) throw std::invalid_argument("control == target");
  if (use_sparse()) {
    apply_cnot_sparse(control, target);
  } else {
    apply_cnot_dense(control, target);
  }
}

void Simulator::apply_cnot_sparse(std::uint32_t control, std::uint32_t target) {
  const std::uint64_t cbit = 1ull << control;
  const std::uint64_t tbit = 1ull << target;

  std::unordered_set<std::uint64_t> candidates;
  candidates.reserve(sparse_support_.size() * 2 + 1);
  for (const auto idx : sparse_support_) {
    candidates.insert(idx);
    if ((idx & cbit) != 0) candidates.insert(idx ^ tbit);
  }

  std::unordered_set<std::uint64_t> seen_lo;
  for (const auto idx : sparse_support_) {
    if ((idx & cbit) == 0) continue;
    const std::uint64_t j = idx ^ tbit;
    const std::uint64_t lo = (idx < j) ? idx : j;
    if (seen_lo.count(lo)) continue;
    seen_lo.insert(lo);
    Complex vi = tiles_->get_amplitude(idx);
    Complex vj = tiles_->get_amplitude(j);
    tiles_->set_amplitude(idx, vj);
    tiles_->set_amplitude(j, vi);
  }

  constexpr double kEps2 = 1e-30;
  std::unordered_set<std::uint64_t> next;
  for (const auto i : candidates) {
    Complex a = tiles_->get_amplitude(i);
    if (a.re * a.re + a.im * a.im > kEps2) next.insert(i);
  }
  sparse_support_.swap(next);
  tiles_->flush();
  tiles_->prune_cached_tiles(kPruneAbs);
}

// --- Dense paths (tile scan + SIMD for H/X) ---

void Simulator::apply_h_dense(std::uint32_t q) { apply_xor_style_1q_dense(q, GateKind::H); }
void Simulator::apply_x_dense(std::uint32_t q) { apply_xor_style_1q_dense(q, GateKind::X); }

void Simulator::apply_xor_style_1q_dense(std::uint32_t q, GateKind kind) {
  TileManager& tiles = *tiles_;
  const std::uint64_t tile_size = tiles.tile_size();
  const uint32_t tile_log = static_cast<uint32_t>(std::countr_zero(tile_size));
  const double s = 1.0 / std::sqrt(2.0);
  auto& activity = tiles.activity_map();

  if (q < tile_log) {
    const std::uint64_t qmask = 1ull << q;
    for (std::uint64_t ti = 0; ti < tiles.num_tiles(); ++ti) {
      if (!activity.is_active(ti)) continue;
      Tile& t = tiles.get_tile(ti);
      bool changed = false;
      for (std::uint64_t blockStart = 0; blockStart < tile_size; blockStart += (qmask << 1)) {
        std::uint64_t k = 0;
#if defined(__AVX512F__)
        if (qmask >= 4) {
          for (; k + 3 < qmask; k += 4) {
            const std::size_t base = static_cast<std::size_t>(blockStart + k);
            const std::size_t partner = static_cast<std::size_t>(base + qmask);
            double* baseD = reinterpret_cast<double*>(&t.data[base]);
            double* partD = reinterpret_cast<double*>(&t.data[partner]);
            __m512d z0 = _mm512_loadu_pd(baseD);
            __m512d z1 = _mm512_loadu_pd(partD);
            if (kind == GateKind::X) {
              _mm512_storeu_pd(baseD, z1);
              _mm512_storeu_pd(partD, z0);
            } else {
              __m512d sum = _mm512_add_pd(z0, z1);
              __m512d diff = _mm512_sub_pd(z0, z1);
              __m512d sc = _mm512_set1_pd(s);
              _mm512_storeu_pd(baseD, _mm512_mul_pd(sum, sc));
              _mm512_storeu_pd(partD, _mm512_mul_pd(diff, sc));
            }
            changed = true;
          }
        }
#endif
        for (; k < qmask; ++k) {
          const std::uint64_t off0 = blockStart + k;
          const std::uint64_t off1 = off0 + qmask;
          Complex& v0 = t.data[off0];
          Complex& v1 = t.data[off1];
          if (kind == GateKind::X) {
            std::swap(v0, v1);
          } else {
            const double re0 = v0.re, im0 = v0.im;
            const double re1 = v1.re, im1 = v1.im;
            v0.re = (re0 + re1) * s;
            v0.im = (im0 + im1) * s;
            v1.re = (re0 - re1) * s;
            v1.im = (im0 - im1) * s;
          }
          changed = true;
        }
      }
      if (changed) t.dirty = true;
    }
    tiles.flush();
    tiles.prune_cached_tiles(kPruneAbs);
    return;
  }

  const std::uint64_t k = 1ull << (q - tile_log);
  for (std::uint64_t ti = 0; ti < tiles.num_tiles(); ++ti) {
    if (!activity.is_active(ti) && !activity.is_active(ti ^ k)) continue;
    if ((ti & k) != 0) continue;
    const std::uint64_t tj = ti ^ k;
    Tile& t0 = tiles.get_tile(ti);
    Tile& t1 = tiles.get_tile(tj);
    bool changed = false;
#if defined(__AVX512F__)
    std::uint64_t off = 0;
    if (tile_size >= 4) {
      for (; off + 3 < tile_size; off += 4) {
        double* d0 = reinterpret_cast<double*>(&t0.data[off]);
        double* d1 = reinterpret_cast<double*>(&t1.data[off]);
        __m512d z0 = _mm512_loadu_pd(d0);
        __m512d z1 = _mm512_loadu_pd(d1);
        if (kind == GateKind::X) {
          _mm512_storeu_pd(d0, z1);
          _mm512_storeu_pd(d1, z0);
        } else {
          __m512d sum = _mm512_add_pd(z0, z1);
          __m512d diff = _mm512_sub_pd(z0, z1);
          __m512d sc = _mm512_set1_pd(s);
          _mm512_storeu_pd(d0, _mm512_mul_pd(sum, sc));
          _mm512_storeu_pd(d1, _mm512_mul_pd(diff, sc));
        }
        changed = true;
      }
    } else {
      off = 0;
    }
#else
    std::uint64_t off = 0;
#endif
    for (; off < tile_size; ++off) {
      Complex& v0 = t0.data[off];
      Complex& v1 = t1.data[off];
      if (kind == GateKind::X) {
        std::swap(v0, v1);
      } else {
        const double re0 = v0.re, im0 = v0.im;
        const double re1 = v1.re, im1 = v1.im;
        v0.re = (re0 + re1) * s;
        v0.im = (im0 + im1) * s;
        v1.re = (re0 - re1) * s;
        v1.im = (im0 - im1) * s;
      }
      changed = true;
    }
    if (changed) {
      t0.dirty = true;
      t1.dirty = true;
    }
  }
  tiles.flush();
  tiles.prune_cached_tiles(kPruneAbs);
}

void Simulator::apply_cnot_dense(std::uint32_t control, std::uint32_t target) {
  TileManager& tiles = *tiles_;
  const std::uint64_t tile_size = tiles.tile_size();
  const uint32_t tile_log = static_cast<uint32_t>(std::countr_zero(tile_size));

  if (target < tile_log && control < tile_log) {
    auto& activity = tiles.activity_map();
    const std::uint64_t cbit_in_tile = 1ull << control;
    const std::uint64_t tbit_in_tile = 1ull << target;
    for (std::uint64_t ti = 0; ti < tiles.num_tiles(); ++ti) {
      if (!activity.is_active(ti)) continue;
      Tile& t = tiles.get_tile(ti);
      bool changed = false;
      for (std::uint64_t off = 0; off < tile_size; ++off) {
        const bool c1 = (off & cbit_in_tile) != 0;
        const bool t0 = (off & tbit_in_tile) == 0;
        if (!c1 || !t0) continue;
        const std::uint64_t off2 = off | tbit_in_tile;
        std::swap(t.data[off], t.data[off2]);
        changed = true;
      }
      if (changed) t.dirty = true;
    }
    tiles.flush();
    tiles.prune_cached_tiles(kPruneAbs);
    return;
  }

  auto& activity = tiles.activity_map();
  const std::uint64_t cbit = 1ull << control;
  const std::uint64_t tbit = 1ull << target;

  for (std::uint64_t ti = 0; ti < tiles.num_tiles(); ++ti) {
    const bool ti_active = activity.is_active(ti);
    const std::uint64_t tj = ti ^ ((target >= tile_log) ? (1ull << (target - tile_log)) : 0ull);
    const bool tj_active = activity.is_active(tj);
    if (!ti_active && !tj_active) continue;

    Tile& t0 = tiles.get_tile(ti);
    Tile* t1_ptr = nullptr;
    if (target >= tile_log) t1_ptr = &tiles.get_tile(tj);
    Tile& t1 = t1_ptr ? *t1_ptr : t0;
    bool changed = false;

    for (std::uint64_t off = 0; off < tile_size; ++off) {
      const std::uint64_t idx = ti * tile_size + off;
      const bool c1 = (idx & cbit) != 0;
      const bool t0cond = (idx & tbit) == 0;
      if (!c1 || !t0cond) continue;
      const std::uint64_t idx2 = idx ^ tbit;
      const std::uint64_t tj2 = idx2 / tile_size;
      const std::uint64_t off2 = idx2 % tile_size;
      if (tj2 == ti) {
        std::swap(t0.data[off], t0.data[off2]);
      } else {
        std::swap(t0.data[off], t1.data[off2]);
      }
      changed = true;
    }
    if (changed) {
      t0.dirty = true;
      if (target >= tile_log) t1.dirty = true;
    }
  }
  tiles.flush();
  tiles.prune_cached_tiles(kPruneAbs);
}

}  // namespace qe
