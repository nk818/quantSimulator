import numpy as np


def verify_math_accuracy(cpp_output_file: str) -> None:
    data = np.fromfile(cpp_output_file, dtype=np.complex128)
    prob = np.sum(np.abs(data) ** 2)
    print(f"Total Probability: {prob}")
    if np.isclose(prob, 1.0, atol=1e-12):
        print("Numerical Precision: PASS")
    else:
        print("Numerical Precision: FAIL (Check your SIMD kernels)")


if __name__ == "__main__":
    verify_math_accuracy("final_state.bin")

