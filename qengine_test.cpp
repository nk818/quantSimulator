#include "Simulator.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
  namespace fs = std::filesystem;
  using qe::Simulator;

  // Reset previous outputs so the bash script sees only fresh results.
  if (fs::exists("final_state.bin")) fs::remove("final_state.bin");
  if (fs::exists(".quantumengine_state")) fs::remove_all(".quantumengine_state");

  // Remove prior exported tile files.
  for (const auto& entry : fs::directory_iterator(".")) {
    if (!entry.is_regular_file()) continue;
    const auto name = entry.path().filename().string();
    if (name.rfind("tile_", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".bin") {
      fs::remove(entry.path());
    }
  }

  Simulator sim(34);

  std::cout << "--- Starting 34-Qubit GHZ Test (qengine_test) ---" << std::endl;

  // GHZ circuit: H on qubit 0, then CNOT chain 0->1->2->...->33
  sim.apply_gate("H", 0);
  for (int i = 0; i < 33; i++) {
    sim.apply_gate("CNOT", i, i + 1);
  }

  // Optional: verify amplitude values for quick human sanity.
  const auto alpha = sim.get_amplitude(0);
  const auto beta = sim.get_amplitude((1ULL << 34) - 1);
  const double expected = 1.0 / std::sqrt(2.0);
  (void)expected;
  std::cout << "Amplitude |0...0>: " << alpha << std::endl;
  std::cout << "Amplitude |1...1>: " << beta << std::endl;

  // Export active tiles and a flattened complex128 view for your bash+NumPy scripts.
  sim.export_active_tiles_for_test(fs::path("."));
  sim.export_final_state_bin_for_test(fs::path("final_state.bin"));

  std::cout << "Export complete: tile_*.bin and final_state.bin written." << std::endl;
  return 0;
}

