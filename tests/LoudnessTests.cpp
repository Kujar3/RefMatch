#include "TestSignals.h"

#include "core/Loudness.h"

#include <algorithm>
#include <limits>
#include <string>

// Tests for Loudness.h: K-weighting, EBU Tech 3341 (meter) and Tech 3342 (loudness range) cases.

namespace
{
using rmtest::Segment;

const double kRates[] = { 44100.0, 48000.0, 96000.0 };

// Feeds the same signal to both channels (stereo) or only the left one (right == nullptr).
rm::LoudnessMeter measure (const std::vector<float>& x, double sampleRate, bool stereo = true, int blockSize = 4096)
{
    rm::LoudnessMeter m;
    m.prepare (sampleRate);

    for (size_t pos = 0; pos < x.size(); pos += (size_t) blockSize)
    {
        const int n = (int) std::min ((size_t) blockSize, x.size() - pos);
        m.process (x.data() + pos, stereo ? x.data() + pos : nullptr, n);
    }

    return m;
}
} // namespace

//==============================================================================
TEST_CASE ("K-weighting coefficients at 48 kHz match the BS.1770 tables")
{
    rm::Biquad shelf, highPass;
    rm::designKWeighting (48000.0, shelf, highPass);

    CHECK_NEAR (shelf.b0, 1.53512485958697, 1.0e-6);
    CHECK_NEAR (shelf.b1, -2.69169618940638, 1.0e-6);
    CHECK_NEAR (shelf.b2, 1.19839281085285, 1.0e-6);
    CHECK_NEAR (shelf.a1, -1.69065929318241, 1.0e-6);
    CHECK_NEAR (shelf.a2, 0.73248077421585, 1.0e-6);

    CHECK_NEAR (highPass.b0, 1.0, 1.0e-12);
    CHECK_NEAR (highPass.b1, -2.0, 1.0e-12);
    CHECK_NEAR (highPass.b2, 1.0, 1.0e-12);
    CHECK_NEAR (highPass.a1, -1.99004745483398, 1.0e-6);
    CHECK_NEAR (highPass.a2, 0.99007225036621, 1.0e-6);
}

TEST_CASE ("K-weighting response: 0 dB at 997 Hz, shelf above 2 kHz, high-pass below 100 Hz")
{
    for (double sr : kRates)
    {
        CHECK_NEAR (rm::kWeightingResponseDb (997.0, sr), 0.0, 0.05);
        CHECK_RANGE (rm::kWeightingResponseDb (10000.0, sr) + 0.691, 3.6, 4.4);   // filter alone ~ +4 dB
        CHECK (rm::kWeightingResponseDb (20.0, sr) < -10.0f);
        CHECK (rm::kWeightingResponseDb (20.0, sr) > -20.0f);
    }

    rmtest::note ("K(10 kHz) @48k incl. -0.691", rm::kWeightingResponseDb (10000.0, 48000.0));
    rmtest::note ("K(20 Hz) @48k incl. -0.691", rm::kWeightingResponseDb (20.0, 48000.0));

    // Biquad::magnitudeAt agrees with a direct evaluation of the filter: a long sine through the filter.
    rm::Biquad shelf, highPass;
    rm::designKWeighting (48000.0, shelf, highPass);
    const auto x = rmtest::sine (5000.0, 0.0, 1.0, 48000.0);
    double peak = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double y = shelf.process (x[i]);
        if (i > x.size() / 2)
            peak = std::max (peak, std::abs (y));
    }
    CHECK_NEAR (peak, shelf.magnitudeAt (5000.0, 48000.0), 0.002);
}

//==============================================================================
TEST_CASE ("EBU 3341 cases 1+2: steady 1 kHz sines read M / S / I = level, at 44.1 / 48 / 96 kHz")
{
    for (double sr : kRates)
    {
        for (double level : { -23.0, -33.0 })
        {
            const auto m = measure (rmtest::sine (1000.0, level, 20.0, sr), sr);
            CHECK_NEAR (m.momentaryLufs(), level, 0.1);
            CHECK_NEAR (m.shortTermLufs(), level, 0.1);
            CHECK_NEAR (m.integratedLufs(), level, 0.1);
            CHECK_NEAR (m.secondsMeasured(), 20.0, 1.0e-9);
            CHECK_EQ (m.stepCount(), 200LL);

            if (sr == 48000.0)
                rmtest::note ("I @" + std::to_string ((int) level) + " dBFS", m.integratedLufs());
        }
    }
}

TEST_CASE ("EBU 3341 cases 3-5: level sequences gate to -23 LUFS integrated")
{
    for (double sr : kRates)
    {
        const auto case3 = rmtest::sineSequence (1000.0, { { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 } }, sr);
        const auto case4 = rmtest::sineSequence (1000.0, { { -72.0, 10.0 }, { -36.0, 10.0 }, { -23.0, 60.0 },
                                                           { -36.0, 10.0 }, { -72.0, 10.0 } }, sr);
        const auto case5 = rmtest::sineSequence (1000.0, { { -26.0, 20.0 }, { -20.0, 20.1 }, { -26.0, 20.0 } }, sr);

        const float i3 = measure (case3, sr).integratedLufs();
        const float i4 = measure (case4, sr).integratedLufs();
        const float i5 = measure (case5, sr).integratedLufs();

        CHECK_NEAR (i3, -23.0, 0.1);
        CHECK_NEAR (i4, -23.0, 0.1);
        CHECK_NEAR (i5, -23.0, 0.1);

        if (sr == 48000.0)
        {
            rmtest::note ("case 3 I", i3);
            rmtest::note ("case 4 I", i4);
            rmtest::note ("case 5 I", i5);
        }
    }
}

TEST_CASE ("Relative gate: -23 dBFS then -60 dBFS reads ~-23 LUFS integrated")
{
    for (double sr : kRates)
    {
        const auto x = rmtest::sineSequence (1000.0, { { -23.0, 20.0 }, { -60.0, 20.0 } }, sr);
        const auto m = measure (x, sr);
        CHECK_NEAR (m.integratedLufs(), -23.0, 0.1);

        // Momentary is not gated: it reports the quiet part as it is.
        CHECK_NEAR (m.momentaryLufs(), -60.0, 0.1);
        CHECK_NEAR (m.shortTermLufs(), -60.0, 0.1);
    }
}

TEST_CASE ("Mono input (right == nullptr) reads 3.01 LU below the same signal in both channels")
{
    const auto x = rmtest::sine (1000.0, -23.0, 10.0, 48000.0);
    const auto stereo = measure (x, 48000.0, true);
    const auto mono = measure (x, 48000.0, false);

    CHECK_NEAR (stereo.integratedLufs() - mono.integratedLufs(), 3.0103, 0.01);
    CHECK_NEAR (stereo.momentaryLufs() - mono.momentaryLufs(), 3.0103, 0.01);
    CHECK_NEAR (stereo.shortTermLufs() - mono.shortTermLufs(), 3.0103, 0.01);
}

TEST_CASE ("Silence and too little audio give kNoValue; LRA 0")
{
    const std::vector<float> zeros ((size_t) (5 * 48000), 0.0f);
    const auto m = measure (zeros, 48000.0);
    CHECK_EQ (m.momentaryLufs(), rm::kNoValue);
    CHECK_EQ (m.shortTermLufs(), rm::kNoValue);
    CHECK_EQ (m.integratedLufs(), rm::kNoValue);
    CHECK_EQ (m.loudnessRange(), 0.0f);

    // 350 ms: no momentary block yet; 2.9 s: no short-term window yet.
    const auto shortSine = measure (rmtest::sine (1000.0, -20.0, 0.35, 48000.0), 48000.0);
    CHECK_EQ (shortSine.momentaryLufs(), rm::kNoValue);
    CHECK_EQ (shortSine.integratedLufs(), rm::kNoValue);

    const auto almost3s = measure (rmtest::sine (1000.0, -20.0, 2.9, 48000.0), 48000.0);
    CHECK_NEAR (almost3s.momentaryLufs(), -20.0, 0.1);
    CHECK_EQ (almost3s.shortTermLufs(), rm::kNoValue);
    CHECK_NEAR (almost3s.integratedLufs(), -20.0, 0.1);

    // Below the absolute gate: momentary still reads, integrated does not.
    const auto quiet = measure (rmtest::sine (1000.0, -75.0, 5.0, 48000.0), 48000.0);
    CHECK_NEAR (quiet.momentaryLufs(), -75.0, 0.1);
    CHECK_EQ (quiet.integratedLufs(), rm::kNoValue);

    // Default-constructed and never fed.
    rm::LoudnessMeter fresh;
    fresh.prepare (48000.0);
    CHECK_EQ (fresh.integratedLufs(), rm::kNoValue);
    CHECK_EQ (fresh.secondsMeasured(), 0.0);
}

TEST_CASE ("reset() clears every reading")
{
    const auto loud = rmtest::sine (1000.0, -10.0, 6.0, 48000.0);
    const auto soft = rmtest::sine (1000.0, -30.0, 6.0, 48000.0);

    rm::LoudnessMeter m;
    m.prepare (48000.0);
    m.process (loud.data(), loud.data(), (int) loud.size());
    CHECK_NEAR (m.integratedLufs(), -10.0, 0.1);

    m.reset();
    CHECK_EQ (m.integratedLufs(), rm::kNoValue);
    CHECK_EQ (m.momentaryLufs(), rm::kNoValue);
    CHECK_EQ (m.shortTermLufs(), rm::kNoValue);
    CHECK_EQ (m.loudnessRange(), 0.0f);
    CHECK_EQ (m.stepCount(), 0LL);
    CHECK_EQ (m.secondsMeasured(), 0.0);

    m.process (soft.data(), soft.data(), (int) soft.size());
    CHECK_NEAR (m.integratedLufs(), -30.0, 0.1);
    CHECK_NEAR (m.shortTermLufs(), -30.0, 0.1);
    CHECK_NEAR (m.loudnessRange(), 0.0, 0.1);
}

TEST_CASE ("Block sizes 1, 64 and 4096 give bit-identical readings")
{
    // Noise with level changes, so every reading (incl. LRA) is non-trivial.
    auto x = rmtest::concat (rmtest::whiteNoise (0.3, 4.0, 48000.0, 1),
                             rmtest::whiteNoise (0.05, 4.0, 48000.0, 2));
    x = rmtest::concat (x, rmtest::whiteNoise (0.6, 3.3, 48000.0, 3));
    auto y = rmtest::whiteNoise (0.2, (double) x.size() / 48000.0, 48000.0, 4);

    float ref[4] = {};
    bool first = true;

    for (int block : { 1, 64, 4096, 999 })
    {
        rm::LoudnessMeter m;
        m.prepare (48000.0);

        for (size_t pos = 0; pos < x.size(); pos += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, x.size() - pos);
            m.process (x.data() + pos, y.data() + pos, n);
        }

        const float r[4] = { m.momentaryLufs(), m.shortTermLufs(), m.integratedLufs(), m.loudnessRange() };
        CHECK (rm::hasValue (r[2]));

        if (first)
            std::copy (r, r + 4, ref);
        else
            for (int i = 0; i < 4; ++i)
                CHECK_EQ (r[i], ref[i]);

        first = false;
    }
}

TEST_CASE ("measureIntegratedLufs matches the meter")
{
    const auto x = rmtest::sineSequence (1000.0, { { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 } }, 48000.0);
    CHECK_EQ (rm::measureIntegratedLufs (x.data(), x.data(), (long long) x.size(), 48000.0),
              measure (x, 48000.0).integratedLufs());
    CHECK_EQ (rm::measureIntegratedLufs (nullptr, nullptr, 100, 48000.0), rm::kNoValue);
}

TEST_CASE ("Short-term readings follow the signal in 100 ms steps")
{
    rm::LoudnessMeter m;
    m.prepare (44100.0);
    CHECK_EQ (m.getSamplesPerStep(), 4410);

    const auto x = rmtest::sineSequence (1000.0, { { -20.0, 5.0 }, { -30.0, 5.0 } }, 44100.0);
    m.process (x.data(), x.data(), (int) x.size());
    CHECK_EQ (m.stepCount(), 100LL);
    CHECK_NEAR (m.shortTermLufs(), -30.0, 0.1);   // the last 3 s are all -30
    CHECK_NEAR (m.momentaryLufs(), -30.0, 0.1);
}

TEST_CASE ("Step boundaries follow the sample clock at rates not divisible by 10")
{
    rm::LoudnessMeter m;
    m.prepare (11025.0);

    long long fed = 0;
    for (int step = 0; step < 1000; ++step)
    {
        const int n = m.samplesToNextStep();
        CHECK (n == 1102 || n == 1103);
        const std::vector<float> chunk ((size_t) n, 0.25f);
        m.process (chunk.data(), chunk.data(), n);
        fed += n;
    }

    CHECK_EQ (m.stepCount(), 1000LL);
    CHECK_EQ (fed, 1102500LL);   // exactly 100 s

    const auto x = rmtest::sine (1000.0, -23.0, 20.0, 11025.0);
    const auto s = measure (x, 11025.0);
    CHECK_NEAR (s.integratedLufs(), -23.0, 0.1);
    CHECK_NEAR (s.momentaryLufs(), -23.0, 0.1);
}

TEST_CASE ("Very hot float input (+24 LUFS) still measures")
{
    const auto x = rmtest::sine (1000.0, 24.08, 10.0, 48000.0);   // peak amplitude 16
    const auto m = measure (x, 48000.0);
    CHECK_NEAR (m.momentaryLufs(), 24.08, 0.1);
    CHECK (rm::hasValue (m.integratedLufs()));
    CHECK_NEAR (m.integratedLufs(), 24.08, 0.1);

    // +12 / +18 dBFS sections: the range is still measured (above 0 dBFS but inside the histogram).
    const auto range = measure (rmtest::sineSequence (1000.0, { { 12.0, 20.0 }, { 18.0, 20.0 } }, 48000.0), 48000.0);
    CHECK_NEAR (range.loudnessRange(), 6.0, 1.0);
}

TEST_CASE ("A NaN burst does not poison the meter")
{
    auto x = rmtest::sine (1000.0, -20.0, 10.0, 48000.0);
    for (size_t i = 100000; i < 100100; ++i)
        x[i] = std::numeric_limits<float>::quiet_NaN();

    const auto m = measure (x, 48000.0);
    CHECK_NEAR (m.integratedLufs(), -20.0, 0.2);
    CHECK_NEAR (m.momentaryLufs(), -20.0, 0.1);
}

//==============================================================================
TEST_CASE ("EBU 3342 loudness range cases")
{
    for (double sr : { 44100.0, 48000.0 })
    {
        const auto lra = [sr] (std::initializer_list<Segment> s)
        {
            return measure (rmtest::sineSequence (1000.0, s, sr), sr).loudnessRange();
        };

        const float lra1 = lra ({ { -20.0, 20.0 }, { -30.0, 20.0 } });
        const float lra2 = lra ({ { -20.0, 20.0 }, { -15.0, 20.0 } });
        const float lra3 = lra ({ { -40.0, 20.0 }, { -20.0, 20.0 } });
        const float lra4 = lra ({ { -50.0, 20.0 }, { -35.0, 20.0 }, { -20.0, 20.0 }, { -35.0, 20.0 }, { -50.0, 20.0 } });

        CHECK_NEAR (lra1, 10.0, 1.0);
        CHECK_NEAR (lra2, 5.0, 1.0);
        CHECK_NEAR (lra3, 20.0, 1.0);
        CHECK_NEAR (lra4, 15.0, 1.0);

        if (sr == 48000.0)
        {
            rmtest::note ("LRA case 1 (expect 10)", lra1);
            rmtest::note ("LRA case 2 (expect 5)", lra2);
            rmtest::note ("LRA case 3 (expect 20)", lra3);
            rmtest::note ("LRA case 4 (expect 15)", lra4);
        }
    }
}
