#pragma once
#include "audio.h"
#include <speex/speex_resampler.h>
#include <vector>

// ---------------------------------------------------------------------------
// Rate conversion
//
// Audio arrives from outside the machine at whatever rate it was made at:
// Vorbis at whatever the encoder used, Opus always at 48 kHz, and a net stream
// at whatever the far end's output endpoint happens to run. None of those need
// be this session's rate, which is the master endpoint's own.
//
// speex_resampler is already in the build for the denoiser, so it does the
// conversion here too. Header only because it is small and because both the
// stream block and the net receive block want their own instance.
// ---------------------------------------------------------------------------
class Converter
{
public:
    Converter() {}
    ~Converter() { if (m_res) speex_resampler_destroy(m_res); }
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;

    // Idempotent: asking for the same pair twice keeps the state, so a stream
    // that reports its rate on every packet does not restart the filter on
    // every packet.
    // `quality` is speex's own 0 to 10. The default suits a live stream, where
    // the work happens between two audio callbacks. A recording converts on a
    // writer thread with a whole file's worth of slack, so it asks for more.
    bool Prepare(int rateIn, int rateOut, int quality = 5)
    {
        if (m_res && rateIn == m_in && rateOut == m_out && quality == m_quality)
            return true;
        if (m_res) { speex_resampler_destroy(m_res); m_res = nullptr; }
        m_in = rateIn;
        m_out = rateOut;
        m_quality = quality;
        if (rateIn == rateOut) return true;
        if (rateIn <= 0 || rateOut <= 0) return false;

        int errCode = 0;
        m_res = speex_resampler_init((spx_uint32_t)kChannels, (spx_uint32_t)rateIn,
                                     (spx_uint32_t)rateOut, quality, &errCode);
        return m_res != nullptr;
    }

    bool Engaged() const { return m_res != nullptr; }
    int RateIn() const { return m_in; }
    int RateOut() const { return m_out; }

    // `in` is interleaved stereo at rateIn; appends the result to `out`.
    void Run(const float* in, size_t frames, std::vector<float>& out)
    {
        if (!m_res)
        {
            out.insert(out.end(), in, in + frames * kChannels);
            return;
        }
        // Room for the ratio plus a little, since the resampler carries state
        // across calls and can hand back one more frame than the ratio says.
        size_t room = (size_t)((double)frames * m_out / m_in) + 64;
        size_t at = out.size();
        out.resize(at + room * kChannels);

        spx_uint32_t inLen = (spx_uint32_t)frames;
        spx_uint32_t outLen = (spx_uint32_t)room;
        speex_resampler_process_interleaved_float(m_res, in, &inLen,
                                                  out.data() + at, &outLen);
        out.resize(at + (size_t)outLen * kChannels);
    }

    // How many frames of delay the filter itself adds, for a block that has to
    // account for its own latency honestly.
    int LatencyFrames() const
    {
        if (!m_res) return 0;
        return (int)speex_resampler_get_output_latency(m_res);
    }

private:
    SpeexResamplerState* m_res = nullptr;
    int m_in = 0, m_out = 0;
    int m_quality = 5;
};
