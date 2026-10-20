#pragma once

#include <complex>
#include <vector>

// Small radix-2 complex FFT (iterative, in place). Twiddles and bit-reversal table are computed in the constructor;
// forward() does not allocate.
namespace rm
{
class Fft
{
public:
    explicit Fft (int order);   // size = 2^order, order 1..20

    int size() const noexcept { return n; }
    int order() const noexcept { return log2n; }

    // In-place forward transform, X[k] = sum_n x[n] e^{-2 pi i k n / N} (no scaling). data must hold size() values.
    void forward (std::complex<float>* data) const noexcept;

private:
    int n = 0, log2n = 0;
    std::vector<std::complex<float>> twiddles;   // e^{-2 pi i k / N}, k < N/2
    std::vector<int> bitReversed;
};

} // namespace rm
