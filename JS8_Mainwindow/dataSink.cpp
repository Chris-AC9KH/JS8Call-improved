

/** \file
 * @brief member function of the UI_Constructor class
 *  creates the waterfall spectrum
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_Mode/RxPeriodContinuity.h"

/**
 * Emulation of the Fortran 'flat1' subroutine.
 */
void flat1(float const *const savg, int const iz, int const nsmo,
           float *const slin) {
    constexpr int x_size = 8192;
    constexpr int nstep = 20;
    constexpr int nh = nstep / 2;

    // Define bounds for smoothing
    int const ia = nsmo / 2 + 1;
    int const ib = iz - nsmo / 2 - 1;

    std::vector<float> x(x_size, 0.0f);

    // Smooth savg using median percentiles

    auto const rank =
        std::clamp(static_cast<int>(std::round(0.5f * nsmo)), 0, nsmo - 1);

    for (int i = ia; i <= ib; i += nstep) {
        auto const data = &savg[i - nsmo / 2];
        auto temp = std::vector<float>(data, data + nsmo);

        std::nth_element(temp.begin(), temp.begin() + rank, temp.end());

        x[i] = temp[rank];

        std::fill(x.begin() + (i - nh), x.begin() + (i + nh), x[i]);
    }

    // Extend smoothed values to boundaries
    std::fill(x.begin(), x.begin() + ia, x[ia]);
    std::fill(x.begin() + ib + 1, x.begin() + iz, x[ib]);

    // Compute scaling factor
    float x0 = 0.001f * *std::max_element(x.begin() + iz / 10,
                                          x.begin() + (9 * iz) / 10);

    // Normalize savg to compute slin
    for (int i = 0; i < iz; ++i)
        slin[i] = savg[i] / (x[i] + x0);
}

/** Emulation of the Fortran 'smo' subroutine. However, doesn't copy the data
 * back from b to a; rather, a is input and, b is output. Since we invariably
 * call this twice, we can just swap the order of the arrays to achieve the
 * same result without the extra two copy operations.
 */
void smo(float const *const a, float *const b, int const npts, int const nadd) {
    auto const nh = nadd / 2;

    // Smooth the array
    for (int i = nh; i < npts - nh; ++i) {
        float sum = 0.0f;
        for (int j = -nh; j <= nh; ++j) {
            sum += a[i + j];
        }
        b[i] = sum;
    }

    // Set edges to zero
    for (int i = 0; i < nh; ++i)
        b[i] = 0.0f; // Zero out leading edge
    for (int i = npts - nh; i < npts; ++i)
        b[i] = 0.0f; // Zero out trailing edge
}

//-------------------------------------------------------------- dataSink()
void UI_Constructor::dataSink(qint64 frames) {
    constexpr int NMAX = JS8_NTMAX * 12000;
    constexpr int nfft3 = 16384;
    constexpr std::array nch = {1, 2, 4, 9, 18, 36, 72};

    // symspec global vars
    static int ja = 0;
    static int k0 = 999999999;
    static WF::SPlot ssum = {};
    static WF::SPlot s = {};
    // Waterfall FFT history must span the decoder's UTC minute rollover.
    static std::array<std::int16_t, nfft3> previousFftTail = {};
    static bool havePreviousTail = false;

    int k(frames);
    if (k0 == 999999999) {
        m_ihsym = int((float)frames / (float)JS8_NSPS) * 2;
        ja = k;
        k0 = k;
    }

    // qCDebug(mainwindow_js8) << "k" << k << "k0" << k0 << "delta" << k-k0;

    // Get power, spectrum, and ihsym
    int const nsmo = m_wideGraph->smoothYellow() - 1;

    // make sure the ssum global is reset every period cycle
    static int lastCycle = -1;
    int const cycle = JS8::Submode::computeCycleForDecode(m_nSubMode, k);

    if (cycle != lastCycle) {
        qCDebug(decoder_js8) << "period loop, resetting ssum";
        ssum.fill(0.0f);
    }

    lastCycle = cycle;

    // cap ihsym based on the period max
    m_ihsym = m_ihsym % (m_TRperiod * JS8_RX_SAMPLE_RATE / JS8_NSPS * 2);

    int const jstep = JS8_NSPS / 2;

    if (k >= 2048 && k <= NMAX) {
        if (k < k0) {
            // The previous minute's last samples are still in d2 here.
            // Copy them before clearing d2 for the new decoder period.
            if (k0 > 0 && k0 <= NMAX) {
                const auto count = std::min(k0, nfft3);
                previousFftTail.fill(0);
                std::copy_n(std::begin(dec_data.d2) + k0 - count, count,
                            previousFftTail.end() - count);
                havePreviousTail = true;
            }
            // Start a new data block
            ja = 0;
            ssum.fill(0.0f);
            m_ihsym = 0;
            std::fill(std::begin(dec_data.d2) + k, std::end(dec_data.d2), 0);
        }

        float gain = pow(10.0f, 0.1f * m_inGain);
        float sq = 0.0f;
        float pxmax = 0.0f;

        for (int i = k0; i < k; ++i) {
            float x1 = dec_data.d2[i];
            pxmax = std::max(pxmax, fabs(x1));
            sq += x1 * x1;
        }

        m_px = sq > 0.0f ? 10.0f * log10(sq / (k - k0)) : 0.0f;
        m_pxmax = pxmax > 0.0f ? 20.0f * log10(pxmax) : 0.0f;

        k0 = k;
        ja += jstep;

        /** Perform real to complex FFT; note that the Fortran code performed
         * this operation in the four2a subroutine, which contains a cache
         * of plans. However, since the FFTW3 library itself caches, this
         * resulted in double caching, so as an experiment we're allowing
         * the library to handle caching.
         *
         * Note that the FFTW library requires all calls but for the plan
         * execution, i.e., fftwf_execute(), to be serialized. The library
         * doesn't require prescriptive serialization, anything will do so
         * long as the result is one thread at a time.
         *
         * Providing room for an extra complex value, i.e., a pair of floats,
         * real and imaginary parts, allows us to use the same buffer for the
         * FFT input and output.
         *
         * While the memory for the FFT can come from anywhere, if we ask the
         * library for it, it'll guarantee that it's aligned for use of SIMD
         * instructions, which will in turn allow it to use them.
         */

        fftwf_complex *fftw_complex;
        float *fftw_real;
        fftwf_plan fftw_plan;
        {
            std::lock_guard<std::mutex> lock(fftw_mutex);

            fftw_complex = fftwf_alloc_complex(nfft3 / 2 + 1);

            if (!fftw_complex) {
                throw std::runtime_error("Failed to allocate FFT data");
            }

            fftw_real = reinterpret_cast<float *>(fftw_complex);
            fftw_plan = fftwf_plan_dft_r2c_1d(nfft3, fftw_real, fftw_complex,
                                              FFTW_ESTIMATE_PATIENT);

            if (!fftw_plan) {
                fftwf_free(fftw_complex);
                throw std::runtime_error("Failed to create FFT plan");
            }
        }

        // Build the FFT window with continuous history across UTC rollover.
        // Unavailable samples at startup are explicitly zero-filled.
        RxPeriodContinuity::fillFftInput(
            fftw_real, nfft3, std::begin(dec_data.d2),
            static_cast<std::size_t>(k), static_cast<std::size_t>(ja),
            previousFftTail.data(), havePreviousTail);

        ++m_ihsym;

        fftwf_execute(fftw_plan);

        // Process the resulting spectrum.

        m_df3 = 12000.0f / nfft3;

        auto const iz = std::min(JS8_NSMAX, static_cast<int>(5000.0f / m_df3));
        auto const cx = reinterpret_cast<std::complex<float> *>(fftw_complex);
        auto const fac = std::pow(1.0f / nfft3, 2.0f);

        for (int i = 0; i < iz; ++i) {
            auto const sx = fac * std::norm(cx[i]);
            ssum[i] += sx;
            s[i] = 1000.0f * gain * sx;
        }

        // We're now done with the plan and the FFT storage. Even though
        // the plan is being 'destroyed' here, the library will cache it
        // at its discretion. As above, the calls must be serialized.

        {
            std::lock_guard<std::mutex> lock(fftw_mutex);
            fftwf_destroy_plan(fftw_plan);
            fftwf_free(fftw_complex);
        }

        // Update average spectra.

        for (int i = 0; i < iz; ++i)
            specData.savg[i] = ssum[i] / m_ihsym;

        if (m_ihsym % 10 == 0) {
            auto const mode4 = nch[nsmo];
            auto const nsmo = 4 * std::min(10 * mode4, 150);

            flat1(specData.savg, iz, nsmo, specData.slin);

            if (mode4 >= 2) {
                std::array<float, JS8_NSMAX> tmp;

                smo(specData.slin, tmp.data(), iz, mode4);
                smo(tmp.data(), specData.slin, iz, mode4);
            }

            std::fill(std::begin(specData.slin),
                      std::begin(specData.slin) + 250, 0.0f);

            auto const ia = static_cast<int>(500.0 / m_df3);
            auto const ib = static_cast<int>(2700.0 / m_df3);
            auto const smin = *std::min_element(std::begin(specData.slin) + ia,
                                                std::begin(specData.slin) + ib);
            auto const smax = *std::max_element(std::begin(specData.slin),
                                                std::begin(specData.slin) + iz);
            auto const scale = (smax > smin) ? 50.0f / (smax - smin) : 0.0f;

            for (auto &val : specData.slin)
                val = std::max(0.0f, scale * (val - smin));
        }
    } else if (k < 2048)
        m_ihsym = 0;

    // make sure ja is equal to k so if we jump ahead in the buffer, everything
    // resolves correctly
    ja = k;

    if (m_ihsym <= 0)
        return;

    if (ui)
        ui->signal_meter_widget->setValue(m_px, m_pxmax); // Update thermometer

    if (m_monitoring)
        m_wideGraph->dataSink(s, m_df3);

    decode(k);
}
