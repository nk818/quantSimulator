#pragma once

#include "TileManager.hpp"

#include <complex>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_set>

namespace qe {

using complex_t = std::complex<double>;

std::ostream& operator<<(std::ostream& os, const complex_t& z);

/// High-level simulator: tiled backing store + sparse evolution when support is small
/// (e.g. GHZ has only two non-zero basis amplitudes).
class Simulator {
 public:
  explicit Simulator(int num_qubits);
  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  void apply_gate(const std::string& name, int q);
  void apply_gate(const std::string& name, int control, int target);

  complex_t get_amplitude(std::uint64_t index);

  // Test helpers matching your external scripts.
  void export_active_tiles_for_test(const std::filesystem::path& out_dir);
  void export_final_state_bin_for_test(const std::filesystem::path& out_file);

  int num_qubits() const { return n_qubits_; }

 private:
  enum class GateKind { H, X };

  void initialize_zero_state();
  void apply_h(std::uint32_t q);
  void apply_x(std::uint32_t q);
  void apply_cnot(std::uint32_t control, std::uint32_t target);

  void apply_xor_style_1q(std::uint32_t q, GateKind kind);

  void apply_h_sparse(std::uint32_t q);
  void apply_x_sparse(std::uint32_t q);
  void apply_cnot_sparse(std::uint32_t control, std::uint32_t target);

  void apply_h_dense(std::uint32_t q);
  void apply_x_dense(std::uint32_t q);
  void apply_xor_style_1q_dense(std::uint32_t q, GateKind kind);
  void apply_cnot_dense(std::uint32_t control, std::uint32_t target);

  bool use_sparse() const;

  int n_qubits_;
  std::unique_ptr<TileManager> tiles_;
  std::unordered_set<std::uint64_t> sparse_support_;
  bool dense_mode_ = false;

  static constexpr std::uint64_t kSparseSupportCap = 1ull << 22;  // switch to dense above this
  static constexpr double kPruneAbs = 1e-12;
};

}  // namespace qe
