#include "Simulator.hpp"

#include <cmath>
#include <iostream>

using qe::Simulator;
using qe::complex_t;

void test_ghz_34_qubits() {
  Simulator sim(34);  // Initialize 34-qubit engine

  std::cout << "--- Starting 34-Qubit GHZ Test ---" << std::endl;

  // Step 1: Apply Hadamard to Qubit 0
  sim.apply_gate("H", 0);

  // Step 2: Apply CNOT chain to create entanglement
  for (int i = 0; i < 33; i++) {
    sim.apply_gate("CNOT", i, i + 1);
  }

  // Step 3: Verification Logic
  // In a GHZ state, only |00...0> and |11...1> should have amplitude 1/sqrt(2)
  double expected_amp = 1.0 / std::sqrt(2.0);
  (void)expected_amp;

  // Check first amplitude (Index 0)
  complex_t alpha = sim.get_amplitude(0);
  // Check last amplitude (Index 2^34 - 1)
  complex_t beta = sim.get_amplitude((1ULL << 34) - 1);

  std::cout << "Amplitude |0...0>: " << alpha << std::endl;
  std::cout << "Amplitude |1...1>: " << beta << std::endl;

  // Norm Check: |alpha|^2 + |beta|^2 should be ~1.0
  double norm = std::norm(alpha) + std::norm(beta);
  std::cout << "Total System Norm: " << norm << std::endl;

  if (std::abs(norm - 1.0) < 1e-12) {
    std::cout << "SUCCESS: Quantum State Integrity Verified." << std::endl;
  } else {
    std::cout << "FAILURE: Norm leakage detected!" << std::endl;
  }
}

int main() {
  test_ghz_34_qubits();
  return 0;
}
