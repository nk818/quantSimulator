# QuantumEngine (34-Qubit Tiled Simulator)

A C++ quantum statevector simulator designed to run large logical state spaces on limited RAM by using:

- a tiled virtual-memory layout,
- SSD-backed state storage,
- tile activity metadata (active vs empty),
- pruning of empty tiles,
- and a GHZ-focused test harness.

The current implementation is optimized to validate correctness on a 34-qubit GHZ circuit while keeping storage/compute practical.

## What this simulator does

For `n` qubits, a dense simulator needs `2^n` complex amplitudes. At `n=34`, that is huge in dense form.

This engine:

1. Splits the state into fixed-size tiles (`2^20` complex amplitudes per tile),
2. Keeps only a small cache in RAM,
3. Stores tile data in `simulation_state.bin`,
4. Tracks tile activity in `simulation_state.meta`,
5. Skips known-empty tiles,
6. Prunes tiles that become numerically zero (`|amp| < 1e-12` for whole tile).

## File overview

- `TileManager.hpp`
  - Core virtual memory grid.
  - `TileActivityMap`: bit-array for active/empty tiles.
  - `DiskController`: single-file tile IO (`simulation_state.bin`), optional `io_uring` on Linux.
  - Tile pruning and export helpers for tests.
- `Simulator.hpp` / `Simulator.cpp`
  - High-level simulator API (`H`, `X`, `CNOT`, amplitude reads).
  - Hybrid execution:
    - sparse-support path for circuits like GHZ (keeps support tiny),
    - dense tile-scan path with AVX-512 kernels for `H/X` where applicable.
- `main.cpp`
  - GHZ-34 correctness test (human-readable output).
- `qengine_test.cpp`
  - Test runner that exports `tile_*.bin` and `final_state.bin`.
- `run_ghz_storage_test.sh`
  - Bash SSD/pruning report script.
- `verify_math_accuracy.py`
  - NumPy probability-normalization checker.
- `Dockerfile`
  - Ubuntu 24.04 image with build dependencies.
- `.dockerignore`
  - Prevents large artifacts from bloating Docker build context.

## Build and run (native)

```bash
cd "/Users/nk/Documents/QuantumEngine"
g++ -O2 -std=c++20 -Wall -Wextra -o qengine main.cpp Simulator.cpp
./qengine
g++ -O2 -std=c++20 -Wall -Wextra -o qengine_test qengine_test.cpp Simulator.cpp
./qengine_test
```

## Docker build and run

```bash
cd "/Users/nk/Documents/QuantumEngine"
docker build -t quantumengine:local .
docker run --rm quantumengine:local
```

## Tests and why they work

### Test 1: GHZ state correctness (`qengine` / `qengine_test`)

Circuit:
1. `H(0)`
2. `CNOT(0,1), CNOT(1,2), ... CNOT(32,33)`

Mathematically, this prepares:
`(|00...0> + |11...1>) / sqrt(2)`

So only basis indices `0` and `(1<<34)-1` should be non-zero with amplitude `1/sqrt(2)`.

### Test 2: SSD/pruning behavior (`run_ghz_storage_test.sh`)

After GHZ preparation, only tile(s) that contain those two basis states should remain active.
The script reports tile count and storage usage. With pruning enabled, active count stays very small.

### Test 3: Numerical precision (`verify_math_accuracy.py`)

Loads `final_state.bin` as `np.complex128` and checks:
`sum(|amp|^2) ~= 1` with tolerance `1e-12`.

This validates probability conservation and catches kernel precision issues.

## Run the 3 tests quickly

```bash
cd "/Users/nk/Documents/QuantumEngine"
g++ -O2 -std=c++20 -Wall -Wextra -o qengine_test qengine_test.cpp Simulator.cpp
./qengine_test
bash ./run_ghz_storage_test.sh
python3 ./verify_math_accuracy.py
```

## Future testing possibilities

1. Random-circuit cross-checks against Qiskit/Cirq (small-to-medium qubits).
2. Deterministic gate unit tests for H/X/CNOT edge cases.
3. Stress tests with long-range CNOTs and deep entangling layers.
4. Performance benchmarks: cache hit rate, IO throughput, AVX vs scalar.
5. Noise-model validation (depolarizing/phase damping) with fidelity tracking.
6. CI regression suite running GHZ + norm + pruning checks on each push.

