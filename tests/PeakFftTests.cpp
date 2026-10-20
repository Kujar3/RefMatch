#include "TestSignals.h"

#include "core/Fft.h"
#include "core/TruePeak.h"

#include <algorithm>
#include <complex>
#include <limits>

// Tests for TruePeak.h (true peak, crest factor) and Fft.h.

//==============================================================================
TEST_CASE ("True peak: fs/4 sine at 45 degrees reads ~0 dBTP while its samples peak at -3.01 dBFS")
{
    // Samples are +-0.7071 (sin (45 + k * 90 degrees)); the real waveform reaches 1.0 between them.
    const auto x = rmtest::sine (12000.0, 0.0, 1.0, 48000.0, rmtest::kPi / 4.0);

    rm::TruePeakMeter m;
    m.prepare (48000.0);
    m.process (x.data(), x.data(), (int) x.size());

    CHECK_NEAR (m.samplePeakDb(), -3.0103, 0.01);
    CHECK_RANGE (m.truePeakDb(), -0.4, 0.2);
    rmtest::note ("fs/4 sine true peak dBTP", m.truePeakDb());
}

TEST_CASE ("True peak: full-scale 997 Hz sine ~0 dBTP; mono; silence")
{
    const auto x = rmtest::sine (997.0, 0.0, 1.0, 48000.0);

    rm::TruePeakMeter m;
    m.prepare (48000.0);
    m.process (x.data(), nullptr, (int) x.size());
    CHECK_NEAR (m.truePeakDb(), 0.0, 0.1);
    CHECK_NEAR (m.samplePeakDb(), 0.0, 0.01);
    CHECK (m.truePeakLinear() >= m.samplePeakLinear());

    // -6 dBFS: scales linearly.
    const auto half = rmtest::sine (997.0, -6.0, 1.0, 48000.0);
    m.reset();
    m.process (half.data(), half.data(), (int) half.size());
    CHECK_NEAR (m.truePeakDb(), -6.0, 0.1);

    const std::vector<float> zeros (4800, 0.0f);
    m.reset();
    m.process (zeros.data(), zeros.data(), (int) zeros.size());
    CHECK_EQ (m.truePeakDb(), rm::kNoValue);
    CHECK_EQ (m.samplePeakDb(), rm::kNoValue);
    CHECK_EQ (m.truePeakLinear(), 0.0f);
}

TEST_CASE ("True peak: max over channels, block-size independent, NaN ignored")
{
    const auto loud = rmtest::sine (5000.0, -2.0, 0.5, 44100.0, 0.3);
    const auto soft = rmtest::sine (5000.0, -12.0, 0.5, 44100.0);

    rm::TruePeakMeter whole, small;
    whole.prepare (44100.0);
    small.prepare (44100.0);
    whole.process (soft.data(), loud.data(), (int) loud.size());

    for (size_t pos = 0; pos < loud.size(); pos += 37)
    {
        const int n = (int) std::min ((size_t) 37, loud.size() - pos);
        small.process (soft.data() + pos, loud.data() + pos, n);
    }

    CHECK_NEAR (whole.truePeakDb(), -2.0, 0.15);
    CHECK_EQ (whole.truePeakDb(), small.truePeakDb());
    CHECK_EQ (whole.samplePeakDb(), small.samplePeakDb());

    std::vector<float> withNan (soft);
    withNan[1000] = std::numeric_limits<float>::quiet_NaN();
    rm::TruePeakMeter n;
    n.process (withNan.data(), nullptr, (int) withNan.size());
    // The NaN counts as a 0 sample: a click in the sine, whose reconstruction peaks a little higher. Finite, not stuck.
    CHECK_RANGE (n.truePeakDb(), -12.15, -10.5);
}

TEST_CASE ("True peak: NaN / inf count as silence and do not hide nearby inter-sample peaks")
{
    // Codex's case: 16 zeros, NaN, then [+r, +r, -r, -r] twice (r = sqrt 0.5: an fs/4 tone at 45 degrees), zeros.
    const float r = std::sqrt (0.5f);
    std::vector<float> x (16, 0.0f);
    x.push_back (std::numeric_limits<float>::quiet_NaN());
    for (int rep = 0; rep < 2; ++rep)
        for (float v : { r, r, -r, -r })
            x.push_back (v);
    x.resize (x.size() + 32, 0.0f);

    rm::TruePeakMeter m;
    m.process (x.data(), nullptr, (int) x.size());

    std::vector<float> clean (x);
    clean[16] = 0.0f;
    rm::TruePeakMeter reference;
    reference.process (clean.data(), nullptr, (int) clean.size());

    CHECK_EQ (m.truePeakDb(), reference.truePeakDb());
    CHECK (m.truePeakDb() > -1.0f);

    x[20] = std::numeric_limits<float>::infinity();
    rm::TruePeakMeter withInf;
    withInf.process (x.data(), x.data(), (int) x.size());
    CHECK (std::isfinite (withInf.truePeakDb()));
    CHECK (withInf.truePeakDb() < 1.0f);
}

TEST_CASE ("Crest factor: sine 3.01 dB, square 0 dB, silence kNoValue")
{
    const auto s = rmtest::sine (1000.0, -6.0, 1.0, 48000.0);
    CHECK_NEAR (rm::crestFactorDb (s.data(), s.data(), (long long) s.size()), 3.0103, 0.05);
    CHECK_NEAR (rm::crestFactorDb (s.data(), nullptr, (long long) s.size()), 3.0103, 0.05);

    std::vector<float> square (48000);
    for (size_t i = 0; i < square.size(); ++i)
        square[i] = (i / 24) % 2 == 0 ? 0.5f : -0.5f;
    CHECK_NEAR (rm::crestFactorDb (square.data(), square.data(), (long long) square.size()), 0.0, 0.05);

    const std::vector<float> zeros (1000, 0.0f);
    CHECK_EQ (rm::crestFactorDb (zeros.data(), zeros.data(), 1000), rm::kNoValue);
    CHECK_EQ (rm::crestFactorDb (nullptr, nullptr, 1000), rm::kNoValue);
    CHECK_EQ (rm::crestFactorDb (s.data(), nullptr, 0), rm::kNoValue);
}

//==============================================================================
namespace
{
std::vector<std::complex<double>> naiveDft (const std::vector<std::complex<float>>& x)
{
    const size_t n = x.size();
    std::vector<std::complex<double>> out (n);

    for (size_t k = 0; k < n; ++k)
    {
        std::complex<double> sum;
        for (size_t t = 0; t < n; ++t)
        {
            const double angle = -2.0 * rmtest::kPi * (double) ((k * t) % n) / (double) n;
            sum += std::complex<double> (x[t]) * std::polar (1.0, angle);
        }
        out[k] = sum;
    }

    return out;
}
} // namespace

TEST_CASE ("FFT matches a naive DFT for sizes 8 .. 4096")
{
    rmtest::Lcg lcg (99);

    for (int order = 3; order <= 12; ++order)
    {
        const rm::Fft fft (order);
        REQUIRE (fft.size() == 1 << order);
        CHECK_EQ (fft.order(), order);

        std::vector<std::complex<float>> x ((size_t) fft.size());
        for (auto& v : x)
            v = { lcg.next(), lcg.next() };

        const auto expected = naiveDft (x);
        auto actual = x;
        fft.forward (actual.data());

        double maxError = 0.0, maxMagnitude = 0.0;
        for (size_t k = 0; k < x.size(); ++k)
        {
            maxError = std::max (maxError, std::abs (std::complex<double> (actual[k]) - expected[k]));
            maxMagnitude = std::max (maxMagnitude, std::abs (expected[k]));
        }

        CHECK (maxError < 1.0e-5 * maxMagnitude * order);
    }
}

TEST_CASE ("FFT two-for-one: L real + R imaginary separates into both spectra")
{
    const rm::Fft fft (10);
    const int n = fft.size();
    rmtest::Lcg lcg (7);

    std::vector<std::complex<float>> z ((size_t) n), xl ((size_t) n), xr ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const float l = lcg.next(), r = lcg.next();
        z[(size_t) i] = { l, r };
        xl[(size_t) i] = { l, 0.0f };
        xr[(size_t) i] = { r, 0.0f };
    }

    fft.forward (z.data());
    fft.forward (xl.data());
    fft.forward (xr.data());

    double maxError = 0.0;
    for (int k = 0; k <= n / 2; ++k)
    {
        const auto zk = z[(size_t) k];
        const auto zc = std::conj (z[(size_t) ((n - k) & (n - 1))]);
        const auto l = 0.5f * (zk + zc);
        const auto r = (zk - zc) * std::complex<float> (0.0f, -0.5f);   // (zk - zc) / 2i

        maxError = std::max (maxError, (double) std::abs (l - xl[(size_t) k]));
        maxError = std::max (maxError, (double) std::abs (r - xr[(size_t) k]));
    }

    CHECK (maxError < 1.0e-3);   // spectra have magnitudes ~ sqrt (n / 3) ~ 18
}

TEST_CASE ("FFT of a single bin sine is a pair of N/2 peaks")
{
    const rm::Fft fft (8);
    const int n = fft.size();
    std::vector<std::complex<float>> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = { (float) std::cos (2.0 * rmtest::kPi * 5.0 * i / n), 0.0f };

    fft.forward (x.data());
    CHECK_NEAR (std::abs (x[5]), n / 2.0, 1.0e-3);
    CHECK_NEAR (std::abs (x[(size_t) (n - 5)]), n / 2.0, 1.0e-3);
    CHECK_NEAR (std::abs (x[6]), 0.0, 1.0e-3);
}
