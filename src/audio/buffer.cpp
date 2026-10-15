#include "audio.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Buffer
//
// Keeps the last so many seconds of what passes through it, and lets you move
// backwards and forwards inside that while the session carries on running.
// Audio keeps arriving in real time and keeps leaving in real time, so being
// twelve seconds behind is a fixed offset rather than a debt that grows: the
// window slides, and the read point slides with it.
//
// At live the offset is zero and the block is a passthrough that costs nothing
// - the sample written this frame is the sample read this frame. That is worth
// stating because it is what makes the block safe to leave in a chain: it adds
// no latency at all until you actually ask it for some.
//
// Two details are not obvious:
//
//  * Moving the read point is crossfaded over a few milliseconds. Jumping a
//    ring buffer pointer joins two unrelated waveforms, and the join is a
//    click every single time. The fade is short enough not to be a effect and
//    long enough to remove the click.
//  * Saving does not interrupt anything. The audio thread only sets a flag;
//    the copy out of the ring is made by this block's own thread, in small
//    pieces, so the render callback never waits on a file or on 23 MB of
//    memcpy.
// ---------------------------------------------------------------------------

namespace {

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const int kFadeMs = 8;

// How much of the ring is copied out at a time while saving. Small enough that
// the audio thread never waits long for the lock, big enough that a minute of
// audio is not a million lock acquisitions.
const size_t kSaveChunkFrames = 4096;

class BufferFx : public Effect
{
public:
    ~BufferFx() override { StopWorker(); }

    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate > 0 ? rate : kDefaultRate;
        m_fade = (size_t)((long long)m_rate * kFadeMs / 1000);
        if (m_fade < 1) m_fade = 1;
        m_stop = false;
        m_thread = std::thread([this] { Worker(); });
        return true;
    }

    void Configure(const Block& b) override
    {
        m_step = (size_t)((long long)m_rate *
                          Clamp(b.bufStepSec, kBufStepMin, kBufStepMax));

        const size_t cap = (size_t)((long long)m_rate *
                                    Clamp(b.bufSeconds, kBufSecondsMin, kBufSecondsMax));
        if (cap != m_cap)
        {
            // Allocated here rather than in Process. Configure runs under the
            // same lock as the render, so this is the one safe moment. The
            // contents go with it: a buffer that has just changed length has
            // nothing trustworthy in it.
            std::lock_guard<std::mutex> g(m_ring);
            m_cap = cap;
            m_buf.assign(m_cap * kChannels, 0.0f);
            m_w = 0;
            m_filled = 0;
            m_offset = 0;
            m_want.store(0);
            m_fadeLeft = 0;
        }
    }

    void ConfigureRecording(const RecordSettings& rs) override
    {
        std::lock_guard<std::mutex> g(m_setLock);
        m_set = rs;
        // A buffer dump is one file. The gate and the splitting belong to a
        // recorder that is running continuously and mean nothing here.
        m_set.skipSilence = false;
        m_set.splitMode = 0;
    }

    // Live is offset zero, so nothing is delayed until something is asked for.
    int LatencyFrames() const override { return 0; }

    void DoAction(int action) override
    {
        switch (action)
        {
        case BA_REWIND:
        {
            // Clamped to what has actually been recorded rather than to the
            // capacity: rewinding into the part of the ring that has never
            // been written would be rewinding into silence.
            size_t want = m_want.load() + m_step;
            const size_t most = m_filled > 0 ? m_filled - 1 : 0;
            if (want > most) want = most;
            m_want.store(want);
            break;
        }
        case BA_FORWARD:
        {
            const size_t cur = m_want.load();
            m_want.store(cur > m_step ? cur - m_step : 0);
            break;
        }
        case BA_LIVE:
            m_want.store(0);
            break;
        case BA_SAVE:
            m_save.store(true);
            break;
        default:
            break;
        }
    }

    std::wstring StatusText() override
    {
        const size_t off = m_offset;
        if (m_saving.load())
            return L"Buffer: saving.";
        if (off == 0)
            return FormatW(L"Buffer: live, holding %s.",
                           DurationText((int)(m_filled / (size_t)m_rate)).c_str());
        return FormatW(L"Buffer: %s behind live, holding %s.",
                       DurationText((int)(off / (size_t)m_rate)).c_str(),
                       DurationText((int)(m_filled / (size_t)m_rate)).c_str());
    }

    void Process(float* buf, size_t frames) override
    {
        if (m_cap == 0) return;
        const float wet = m_wet.load();

        std::lock_guard<std::mutex> g(m_ring);

        // A move asked for since the last block starts a fade from where the
        // read point is now to where it has been asked to go.
        const size_t want = m_want.load();
        if (want != m_offset && m_fadeLeft == 0)
        {
            m_from = m_offset;
            m_offset = want;
            m_fadeLeft = m_fade;
        }

        for (size_t i = 0; i < frames; ++i)
        {
            const float inL = buf[i * kChannels + 0];
            const float inR = buf[i * kChannels + 1];

            m_buf[m_w * kChannels + 0] = inL;
            m_buf[m_w * kChannels + 1] = inR;
            m_w = (m_w + 1) % m_cap;
            if (m_filled < m_cap) ++m_filled;

            float l = 0.0f, r = 0.0f;
            ReadAt(m_offset, &l, &r);

            if (m_fadeLeft > 0)
            {
                // Linear over a handful of milliseconds. Both points advance
                // with the write pointer, so each side of the fade is itself
                // continuous and only the mix between them moves.
                float ol = 0.0f, orr = 0.0f;
                ReadAt(m_from, &ol, &orr);
                const float t = 1.0f - (float)m_fadeLeft / (float)m_fade;
                l = ol + (l - ol) * t;
                r = orr + (r - orr) * t;
                --m_fadeLeft;
            }

            buf[i * kChannels + 0] = wet * l + (1.0f - wet) * inL;
            buf[i * kChannels + 1] = wet * r + (1.0f - wet) * inR;
        }
    }

private:
    // `back` frames behind the newest sample. Zero is the sample just written.
    void ReadAt(size_t back, float* l, float* r) const
    {
        if (back >= m_filled) { *l = 0.0f; *r = 0.0f; return; }
        const size_t idx = (m_w + m_cap - 1 - back) % m_cap;
        *l = m_buf[idx * kChannels + 0];
        *r = m_buf[idx * kChannels + 1];
    }

    void StopWorker()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
    }

    void Worker()
    {
        while (!m_stop.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (m_stop.load()) break;
            if (!m_save.exchange(false)) continue;
            SaveNow();
        }
    }

    void SaveNow()
    {
        RecordSettings set;
        {
            std::lock_guard<std::mutex> g(m_setLock);
            set = m_set;
        }

        m_saving.store(true);
        std::wstring err;
        std::wstring path = ResolveRecordingPath(set.folder, set.nameTemplate,
                                                 set.sessionName, set.fmt);
        // The same resolving the recorder block does, so a buffer saved in a
        // format gets the rate and depth its dialog offered rather than the
        // session's.
        const RecordFormat rf = RecFileFormatFor(set.fmt, set.rateHz, set.bits,
                                                 set.kbps, m_rate);
        std::unique_ptr<Recorder> rec = MakeRecorder(set.fmt);
        if (!rec || !rec->Start(path, rf, &err))
        {
            LogError(err.empty() ? L"The buffer could not be saved." : err);
            m_saving.store(false);
            return;
        }

        // Oldest first. Copied a chunk at a time so the render callback waits
        // microseconds rather than for the whole buffer.
        std::vector<float> chunk(kSaveChunkFrames * kChannels);
        size_t written = 0;
        for (;;)
        {
            size_t got = 0;
            {
                std::lock_guard<std::mutex> g(m_ring);
                if (written >= m_filled) break;
                const size_t left = m_filled - written;
                got = left < kSaveChunkFrames ? left : kSaveChunkFrames;
                for (size_t i = 0; i < got; ++i)
                {
                    // `written` counts forward from the oldest sample, which
                    // is m_filled - 1 frames behind the newest.
                    const size_t back = m_filled - 1 - (written + i);
                    ReadAt(back, &chunk[i * kChannels + 0], &chunk[i * kChannels + 1]);
                }
            }
            if (got == 0) break;
            rec->Write(chunk.data(), got);
            written += got;
        }

        rec->Stop();
        m_saving.store(false);
    }

    int    m_rate = kDefaultRate;
    size_t m_cap = 0;                 // frames the ring holds
    size_t m_w = 0;                   // next slot to write
    size_t m_filled = 0;              // frames actually written, up to m_cap
    size_t m_step = 0;                // frames one skip moves
    std::vector<float> m_buf;
    mutable std::mutex m_ring;

    size_t m_offset = 0;              // frames behind live, render thread
    size_t m_from = 0;                // where a fade is coming from
    size_t m_fade = 1, m_fadeLeft = 0;
    std::atomic<size_t> m_want{ 0 };  // where an action has asked to be

    RecordSettings m_set;
    std::mutex m_setLock;
    std::atomic<bool> m_save{ false };
    std::atomic<bool> m_saving{ false };
    std::atomic<bool> m_stop{ false };
    std::thread m_thread;
};

}  // namespace

std::unique_ptr<Effect> MakeBuffer()
{
    return std::unique_ptr<Effect>(new BufferFx());
}
