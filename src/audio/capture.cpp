#include "audio.h"
#include "util.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <map>
#include <algorithm>

// ---------------------------------------------------------------------------
// Runtime feature detection
//
// ActivateAudioInterfaceAsync is exported by mmdevapi.dll only from Windows 8.
// Importing it the ordinary way puts it in the import table, and Windows 7
// then refuses to start the process at all - not a missing feature, a missing
// application. It is resolved by hand so the rest of FastRoute still runs.
// ---------------------------------------------------------------------------

typedef HRESULT (STDAPICALLTYPE* PFN_ActivateAudioInterfaceAsync)(
    LPCWSTR, REFIID, PROPVARIANT*,
    IActivateAudioInterfaceCompletionHandler*,
    IActivateAudioInterfaceAsyncOperation**);

static PFN_ActivateAudioInterfaceAsync ActivateAsyncFn()
{
    static PFN_ActivateAudioInterfaceAsync fn = []() -> PFN_ActivateAudioInterfaceAsync {
        // Left loaded: mmdevapi is in the process for the lifetime of any
        // audio work anyway.
        HMODULE m = LoadLibraryW(L"mmdevapi.dll");
        return m ? (PFN_ActivateAudioInterfaceAsync)
                       GetProcAddress(m, "ActivateAudioInterfaceAsync")
                 : nullptr;
    }();
    return fn;
}

static bool OsVersion(DWORD* major, DWORD* minor, DWORD* build)
{
    // GetVersionEx reports 6.2 to anything without a supportedOS manifest
    // entry, which would make every Windows 10 look like Windows 8. RtlGetVersion
    // is not shimmed and answers with the truth.
    typedef LONG (WINAPI* PFN_RtlGetVersion)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PFN_RtlGetVersion get = nt ? (PFN_RtlGetVersion)GetProcAddress(nt, "RtlGetVersion")
                               : nullptr;
    if (!get) return false;

    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (get(&vi) != 0) return false;
    if (major) *major = vi.dwMajorVersion;
    if (minor) *minor = vi.dwMinorVersion;
    if (build) *build = vi.dwBuildNumber;
    return true;
}

std::wstring WindowsVersionText()
{
    DWORD major = 0, minor = 0, build = 0;
    if (!OsVersion(&major, &minor, &build)) return L"this version of Windows";
    return FormatW(L"Windows %lu.%lu (build %lu)", major, minor, build);
}

bool ProcessLoopbackAvailable()
{
    static bool ok = []() {
        if (!ActivateAsyncFn()) return false;          // Windows 7: no export
        DWORD major = 0, build = 0;
        if (!OsVersion(&major, nullptr, &build)) return false;
        // Process loopback activation arrived in Windows 10 2004. Before that
        // the export exists but rejects these activation parameters.
        return major > 10 || (major == 10 && build >= 19041);
    }();
    return ok;
}

bool BlockTypeAvailable(const std::wstring& typeId)
{
    if (typeId == BT_INPUT_APP) return ProcessLoopbackAvailable();
    return true;   // stream blocks work without Media Foundation, on Ogg
}

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------

void Ring::Write(const float* src, size_t frames)
{
    std::lock_guard<std::mutex> g(m_lock);

    // Once per call, not once per frame.
    //
    // This counted a glitch for every frame it had to drop, so one burst
    // arriving after a stall - which is a single event, and the ordinary shape
    // of a network catching up - was counted in the hundreds. That mattered
    // for more than the tally: the drift correction latches on at five
    // glitches, so a single overflow of five frames turned the resampler on
    // for the life of the stream, after which the pitch rode the buffer depth.
    // Twelve seconds of a stalling link was reporting three and a half
    // thousand glitches for six stalls.
    bool dropped = false;

    for (size_t i = 0; i < frames; ++i)
    {
        // Past the backlog ceiling the oldest frame goes. This is an emergency
        // valve, not an operating mechanism: reaching it means audio is being
        // discarded, and the newest is the audio worth keeping.
        if (m_count >= m_maxBacklog)
        {
            dropped = true;
            while (m_count >= m_maxBacklog)
            {
                m_read = (m_read + 1) % m_frames;
                --m_count;
            }
        }
        float* dst = &m_buf[m_write * kChannels];
        for (int c = 0; c < kChannels; ++c) dst[c] = src[i * kChannels + c];
        m_write = (m_write + 1) % m_frames;
        ++m_count;
    }

    if (dropped) m_glitches.fetch_add(1);
    m_framesIn.fetch_add(frames);
}

void Ring::PrimeSilence(size_t frames)
{
    std::lock_guard<std::mutex> g(m_lock);
    for (size_t i = 0; i < frames && m_count < m_maxBacklog; ++i)
    {
        float* dst = &m_buf[m_write * kChannels];
        for (int c = 0; c < kChannels; ++c) dst[c] = 0.0f;
        m_write = (m_write + 1) % m_frames;
        ++m_count;
    }
}

// How far the drift correction is allowed to go, and how hard it pulls.
//
// A quarter of one percent is about four cents, which is not audible as pitch;
// the gain is scaled so that a depth error of one whole buffer asks for just
// about all of that and anything smaller asks for proportionally less. A
// correction that saturates would be a correction that oscillates.
static const double kMaxDriftRatio = 0.0025;
static const double kDepthGain = 0.0004;

// Catmull-Rom through p1..p2, with p0 and p3 as the surrounding points. At
// x == 0 it returns p1 exactly, so a ratio of 1.0 with no accumulated
// fraction is bit-for-bit passthrough.
static inline float Interp(float p0, float p1, float p2, float p3, double x)
{
    double a = 2.0 * p1;
    double b = p2 - p0;
    double c = 2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3;
    double d = 3.0 * (p1 - p2) + p3 - p0;
    return (float)(0.5 * (a + x * (b + x * (c + x * d))));
}

size_t Ring::Read(float* dst, size_t frames)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_depth.store(m_count);
    m_framesOut.fetch_add(frames);
    if (frames == 0) return 0;

    unsigned long long inNow = m_framesIn.load();
    unsigned long long arrived = inNow - m_lastIn;
    m_lastIn = inNow;

    // Truly idle means nothing in the ring and nothing arriving. A ring that
    // has run dry while audio is still coming in is not idle - it is a source
    // that stalled for a moment, which is what every wireless link does - and
    // leaving those reads out of the measurement below biased it upwards:
    // their output went uncounted while the audio that turned up immediately
    // afterwards was counted in. The measured ratio then climbed past the
    // threshold, the correction engaged, and the pitch rode the buffer depth
    // from then on. That was the wander.
    const bool idle = (m_count == 0 && arrived == 0);

    // Rate measurement.
    //
    // This has to be a ratio of totals over a window, never an average of
    // per-read ratios. Packets do not arrive on every read: some reads see
    // none, some see two. Averaging per-read ratios while discarding the empty
    // ones - which is what an "only learn when data arrived" guard does -
    // biases the estimate upwards by exactly the reads it threw away. The
    // converter then engages and pitch shifts the whole session by several
    // percent, wandering as the estimate wanders. Totals over the same span
    // cannot be biased that way.
    //
    // Idle stretches are skipped entirely, or a silent source would drag the
    // measured ratio towards zero.
    if (!idle)
    {
        m_measIn += arrived;
        m_measOut += frames;
        if (m_measOut >= (unsigned long long)m_target * 50)   // roughly a second
        {
            double measured = (double)m_measIn / (double)m_measOut;
            double prev = m_ratioEst.load();
            m_ratioEst.store(prev + 0.25 * (measured - prev));
            m_measIn = m_measOut = 0;
        }
    }

    const double est = m_ratioEst.load();
    const double want = (double)m_target + (double)frames * est;
    const double err = (double)m_count - want;

    // The converter stays latched off until this input has proved it needs
    // one, and once on it stays on for the life of the stream. A hiccup while
    // the graph is starting up is not proof of anything, so it takes several.
    //
    // At a ratio of exactly 1.0 with no accumulated fraction the interpolator
    // is bit-for-bit passthrough, so an input running at our rate is left
    // completely alone.
    if (!m_engaged && m_glitches.load() > 4 && (est < 0.99 || est > 1.01))
    {
        m_engaged = true;
        m_engagedFlag.store(true);
    }

    double ratio = 1.0;
    if (m_engaged)
    {
        ratio = est + kDepthGain * err / (double)frames;
        // What this corrects is two clocks differing, and real crystals differ
        // by tens of parts per million - a quarter of one percent is already
        // twenty times more authority than that needs, and is about four
        // cents, which is below hearing it as a pitch at all.
        //
        // It used to be allowed five percent, which is most of a semitone,
        // with a gain that let one packet's worth of depth move the pitch by
        // one and a half percent. On a packetised source the depth moves by a
        // packet all the time, so the correction rode it. The buffer is what
        // absorbs jitter; the resampler is only for drift, and it should be
        // too slow and too weak to be noticed doing it.
        if (ratio < 1.0 - kMaxDriftRatio) ratio = 1.0 - kMaxDriftRatio;
        if (ratio > 1.0 + kMaxDriftRatio) ratio = 1.0 + kMaxDriftRatio;
    }

    double pos = m_frac;
    size_t got = 0;
    bool starved = false;

    for (size_t i = 0; i < frames; ++i)
    {
        size_t i1 = (size_t)pos;
        if (i1 + 2 >= m_count)
        {
            // Ran dry. Mid-read that is a real discontinuity; an entirely
            // empty ring just means nothing is being played into it.
            if (!starved && !idle) { starved = true; m_glitches.fetch_add(1); }
            for (int c = 0; c < kChannels; ++c) dst[i * kChannels + c] = 0.0f;
            continue;
        }
        double x = pos - (double)i1;
        for (int c = 0; c < kChannels; ++c)
        {
            float p0 = (i1 == 0) ? m_hist[c] : *At(i1 - 1, c);
            dst[i * kChannels + c] =
                Interp(p0, *At(i1, c), *At(i1 + 1, c), *At(i1 + 2, c), x);
        }
        pos += ratio;
        ++got;
    }

    size_t consumed = (size_t)pos;
    if (consumed > m_count) consumed = m_count;
    if (consumed > 0)
    {
        for (int c = 0; c < kChannels; ++c) m_hist[c] = *At(consumed - 1, c);
        m_read = (m_read + consumed) % m_frames;
        m_count -= consumed;
    }
    m_frac = (starved || idle) ? 0.0 : pos - (double)consumed;
    return got;
}

// How much is in the ring right now, as opposed to Depth(), which is what the
// last read saw. A stream throttling itself against the mixer needs the live
// figure: it is filling the ring between reads, not at them.
size_t Ring::Count()
{
    std::lock_guard<std::mutex> g(m_lock);
    return m_count;
}

void Ring::Clear()
{
    std::lock_guard<std::mutex> g(m_lock);
    m_read = m_write = m_count = 0;
    m_depth.store(0);
    m_frac = 0.0;
    m_measIn = m_measOut = 0;
    m_engaged = false;
    m_engagedFlag.store(false);
    m_ratioEst.store(1.0);
    m_lastIn = 0;
    for (int c = 0; c < kChannels; ++c) m_hist[c] = 0.0f;
    // A deliberate flush is not a dropout, and the tallies restart with it.
    m_glitches.store(0);
    m_framesIn.store(0);
    m_framesOut.store(0);
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static void FillMixFormat(WAVEFORMATEX& wfx, int rate)
{
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = (WORD)kChannels;
    wfx.nSamplesPerSec  = (DWORD)rate;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = (WORD)(kChannels * 4);
    wfx.nAvgBytesPerSec = (DWORD)rate * wfx.nBlockAlign;
    wfx.cbSize          = 0;
}

// Pump a capture client until asked to stop.
static void CaptureLoop(IAudioCaptureClient* cap, IAudioClient* client,
                        HANDLE evt, Ring& ring, std::atomic<bool>& stop,
                        CaptureSource* owner)
{
    DWORD taskIndex = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    std::vector<float> silence(4096 * (size_t)kChannels, 0.0f);

    while (!stop.load())
    {
        DWORD wait = WaitForSingleObject(evt, 200);
        if (stop.load()) break;
        if (wait != WAIT_OBJECT_0) continue;   // timeout: the source is idle

        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (frames)
            {
                if (owner) owner->NotePacket(frames);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                {
                    if (silence.size() < (size_t)frames * kChannels)
                        silence.assign((size_t)frames * kChannels, 0.0f);
                    ring.Write(silence.data(), frames);
                }
                else
                {
                    ring.Write(reinterpret_cast<const float*>(data), frames);
                }
            }
            cap->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    if (mm) AvRevertMmThreadCharacteristics(mm);
}

// ---------------------------------------------------------------------------
// Capture from a physical input endpoint (microphone, line in, ...)
// ---------------------------------------------------------------------------

class DeviceCapture : public CaptureSource
{
public:
    DeviceCapture(std::wstring id, int rate)
        : CaptureSource(rate), m_id(std::move(id)) {}
    ~DeviceCapture() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stop = false;
        m_thread = std::thread([this] { ThreadMain(); });
        WaitForSingleObject(m_ready, 5000);
        if (!m_initOk)
        {
            if (err) *err = m_initErr.empty() ? L"Could not open the input device." : m_initErr;
            Stop();
            return false;
        }
        return true;
    }

    void Stop() override
    {
        m_stop = true;
        if (m_event) SetEvent(m_event);
        if (m_thread.joinable()) m_thread.join();
        if (m_event) { CloseHandle(m_event); m_event = nullptr; }
        if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
    }

private:
    void ThreadMain()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap = nullptr;

        HRESULT hr = OpenEndpoint(m_id, false, &dev);
        if (SUCCEEDED(hr))
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);

        // Deliberately NOT the IAudioClient3 minimum-period path here. On at
        // least one endpoint it hands back 1 ms packets at roughly 1.22x real
        // time, so the ring overruns and the stream is shredded. The plain
        // shared-mode path with AUTOCONVERTPCM asks Windows' own resampler for
        // our exact format, which pins the rate. The few milliseconds the
        // shorter period would have saved are not worth that risk.
        // Deliberately NOT the IAudioClient3 minimum-period path here. On at
        // least one endpoint it hands back 1 ms packets at about 1.22x real
        // time. The resampler now absorbs that without dropping a sample, but
        // resampling by 22% is a real quality cost for a couple of milliseconds
        // of period, so we ask Windows' own converter for our exact format and
        // let the resampler handle genuine drift instead.
        if (SUCCEEDED(hr))
        {
            WAVEFORMATEX wfx{};
            FillMixFormat(wfx, m_rate);
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    1000000 /* headroom, not latency: we drain
                                               on every event */,
                                    0, &wfx, nullptr);
        }

        if (SUCCEEDED(hr)) hr = client->SetEventHandle(m_event);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
        if (SUCCEEDED(hr)) hr = client->Start();

        m_initOk = SUCCEEDED(hr);
        if (!m_initOk) m_initErr = L"Input device could not be opened (" + HrText(hr) + L").";
        SetEvent(m_ready);

        if (m_initOk)
        {
            m_ring.PrimeSilence(PrimeFramesFor(m_rate));
            CaptureLoop(cap, client, m_event, m_ring, m_stop, this);
        }

        if (cap) cap->Release();
        if (client) client->Release();
        if (dev) dev->Release();
        CoUninitialize();
    }

    std::wstring m_id;
    HANDLE m_ready = nullptr;
    bool m_initOk = false;
    std::wstring m_initErr;
};

// ---------------------------------------------------------------------------
// Per-process loopback capture (Windows 10 2004 and later)
// ---------------------------------------------------------------------------

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    ActivateHandler() { m_done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

    HRESULT Wait(DWORD ms, IAudioClient** out)
    {
        *out = nullptr;
        if (WaitForSingleObject(m_done, ms) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (FAILED(m_hr)) return m_hr;
        *out = m_client;
        m_client = nullptr;
        return S_OK;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
    {
        HRESULT hrActivate = S_OK;
        IUnknown* unk = nullptr;
        m_hr = op->GetActivateResult(&hrActivate, &unk);
        if (SUCCEEDED(m_hr)) m_hr = hrActivate;
        if (SUCCEEDED(m_hr) && unk) m_hr = unk->QueryInterface(IID_PPV_ARGS(&m_client));
        if (unk) unk->Release();
        SetEvent(m_done);
        return S_OK;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *ppv = static_cast<IAgileObject*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }

private:
    ~ActivateHandler()
    {
        if (m_client) m_client->Release();
        if (m_done) CloseHandle(m_done);
    }

    LONG m_ref = 1;
    HANDLE m_done = nullptr;
    HRESULT m_hr = E_FAIL;
    IAudioClient* m_client = nullptr;
};

class ProcessLoopback : public CaptureSource
{
public:
    ProcessLoopback(unsigned long pid, int rate)
        : CaptureSource(rate), m_pid(pid) {}
    ~ProcessLoopback() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stop = false;
        m_thread = std::thread([this] { ThreadMain(); });
        WaitForSingleObject(m_ready, 5000);
        if (!m_initOk)
        {
            if (err) *err = m_initErr;
            Stop();
            return false;
        }
        return true;
    }

    void Stop() override
    {
        m_stop = true;
        if (m_event) SetEvent(m_event);
        if (m_thread.joinable()) m_thread.join();
        if (m_event) { CloseHandle(m_event); m_event = nullptr; }
        if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
    }

private:
    void ThreadMain()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap = nullptr;

        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = m_pid;
        // INCLUDE_TARGET_PROCESS_TREE is the only mode that means "this
        // application". EXCLUDE_TARGET_PROCESS_TREE is not "without its
        // children" - it is the inverse capture, every process on the system
        // *except* that tree, which for a router that is also rendering means
        // capturing its own output straight back into the bus.
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        ActivateHandler* handler = new ActivateHandler();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;

        PFN_ActivateAudioInterfaceAsync activate = ActivateAsyncFn();
        HRESULT hr = activate
            ? activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                       __uuidof(IAudioClient), &pv, handler, &op)
            : E_NOTIMPL;
        if (SUCCEEDED(hr)) hr = handler->Wait(5000, &client);
        if (op) op->Release();
        handler->Release();

        if (SUCCEEDED(hr))
        {
            WAVEFORMATEX wfx{};
            FillMixFormat(wfx, m_rate);
            // Process loopback requires shared mode plus the loopback and event
            // flags; the requested format is honoured by the API directly.
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK |
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    1000000, 0, &wfx, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(m_event);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
        if (SUCCEEDED(hr)) hr = client->Start();

        m_initOk = SUCCEEDED(hr);
        if (!m_initOk)
            m_initErr = FormatW(L"Could not capture audio from process %lu (%s).",
                                m_pid, HrText(hr).c_str());
        SetEvent(m_ready);

        if (m_initOk)
        {
            m_ring.PrimeSilence(PrimeFramesFor(m_rate));
            CaptureLoop(cap, client, m_event, m_ring, m_stop, this);
        }

        if (cap) cap->Release();
        if (client) client->Release();
        CoUninitialize();
    }

    unsigned long m_pid;
    HANDLE m_ready = nullptr;
    bool m_initOk = false;
    std::wstring m_initErr;
};

std::unique_ptr<CaptureSource> MakeDeviceCapture(const std::wstring& endpointId, int rate)
{
    return std::unique_ptr<CaptureSource>(new DeviceCapture(endpointId, rate));
}

std::unique_ptr<CaptureSource> MakeProcessLoopback(unsigned long pid, int rate)
{
    return std::unique_ptr<CaptureSource>(new ProcessLoopback(pid, rate));
}

// ---------------------------------------------------------------------------
// Endpoint loopback: everything playing on a render device
//
// The oldest and simplest way to capture system audio, and the only one that
// works below Windows 10 2004. It takes the endpoint's whole mix, which means
// it cannot leave an application out - and it takes FastRoute's own output
// too, so a session must not render to the endpoint it is capturing.
// ---------------------------------------------------------------------------
class LoopbackCapture : public CaptureSource
{
public:
    LoopbackCapture(std::wstring id, int rate)
        : CaptureSource(rate), m_id(std::move(id)) {}
    ~LoopbackCapture() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stop = false;
        m_thread = std::thread([this] { ThreadMain(); });
        WaitForSingleObject(m_ready, 5000);
        if (!m_initOk)
        {
            if (err) *err = m_initErr.empty() ? L"Could not capture the output device."
                                              : m_initErr;
            Stop();
            return false;
        }
        return true;
    }

    void Stop() override
    {
        m_stop = true;
        if (m_event) SetEvent(m_event);
        if (m_thread.joinable()) m_thread.join();
        if (m_event) { CloseHandle(m_event); m_event = nullptr; }
        if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
    }

private:
    void ThreadMain()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap = nullptr;

        HRESULT hr = OpenEndpoint(m_id, true, &dev);
        if (SUCCEEDED(hr))
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);

        if (SUCCEEDED(hr))
        {
            WAVEFORMATEX wfx{};
            FillMixFormat(wfx, m_rate);
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK |
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    1000000, 0, &wfx, nullptr);
            if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
                m_initErr = FormatW(
                    L"This output device will not hand back its audio at %d Hz "
                    L"stereo. Set it to that rate in Windows sound settings, or "
                    L"make it this session's first output block so the session "
                    L"runs at its rate.", m_rate);
        }

        if (SUCCEEDED(hr)) hr = client->SetEventHandle(m_event);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
        if (SUCCEEDED(hr)) hr = client->Start();

        m_initOk = SUCCEEDED(hr);
        if (!m_initOk && m_initErr.empty())
            m_initErr = L"The output device could not be captured (" + HrText(hr) + L").";
        SetEvent(m_ready);

        if (m_initOk)
        {
            m_ring.PrimeSilence(PrimeFramesFor(m_rate));
            // A loopback endpoint delivers nothing at all while the device is
            // idle rather than delivering silence, which the ring's own idle
            // handling already copes with.
            CaptureLoop(cap, client, m_event, m_ring, m_stop, this);
        }

        if (cap) cap->Release();
        if (client) client->Release();
        if (dev) dev->Release();
        CoUninitialize();
    }

    std::wstring m_id;
    HANDLE m_ready = nullptr;
    bool m_initOk = false;
    std::wstring m_initErr;
};

// ---------------------------------------------------------------------------
// Which trees a system audio block captures
// ---------------------------------------------------------------------------

std::vector<unsigned long> SelectSystemCaptureTargets(
    const std::vector<AudioProcess>& audio,
    const std::vector<ProcessEntry>& tree,
    const std::vector<std::wstring>& exclude,
    unsigned long self)
{
    std::map<unsigned long, const ProcessEntry*> byPid;
    for (const ProcessEntry& e : tree) byPid[e.pid] = &e;

    auto excluded = [&](unsigned long pid) {
        auto it = byPid.find(pid);
        if (it == byPid.end()) return false;
        for (const std::wstring& x : exclude)
            if (IEquals(it->second->exeName, x)) return true;
        return false;
    };

    // True when this process or anything above it is excluded. Excluding an
    // application excludes its children, so a tree under an excluded process
    // is out even if its own name is not on the list.
    auto blocked = [&](unsigned long pid) {
        unsigned long cur = pid;
        for (int hop = 0; hop < 64 && cur != 0; ++hop)
        {
            if (excluded(cur)) return true;
            auto it = byPid.find(cur);
            if (it == byPid.end()) break;
            cur = it->second->parent;
        }
        return false;
    };

    // Everything above us, plus us. Capturing any of those takes in this
    // process, and the session would capture its own output.
    std::vector<unsigned long> ours;
    {
        unsigned long cur = self;
        for (int hop = 0; hop < 64 && cur != 0; ++hop)
        {
            ours.push_back(cur);
            auto it = byPid.find(cur);
            if (it == byPid.end()) break;
            cur = it->second->parent;
        }
    }
    auto isOurs = [&](unsigned long pid) {
        for (unsigned long p : ours) if (p == pid) return true;
        return false;
    };

    // The processes that are candidates in their own right.
    std::vector<unsigned long> playing;
    for (const AudioProcess& a : audio)
    {
        if (a.pid == 0 || isOurs(a.pid) || blocked(a.pid)) continue;
        bool dup = false;
        for (unsigned long p : playing) if (p == a.pid) dup = true;
        if (!dup) playing.push_back(a.pid);
    }

    // Drop anything already inside another candidate's tree: a process
    // loopback capture takes the whole tree, so keeping both counts it twice.
    std::vector<unsigned long> out;
    for (unsigned long pid : playing)
    {
        bool covered = false;
        auto it = byPid.find(pid);
        unsigned long cur = (it == byPid.end()) ? 0 : it->second->parent;
        for (int hop = 0; hop < 64 && cur != 0 && !covered; ++hop)
        {
            for (unsigned long p : playing) if (p == cur) covered = true;
            auto pit = byPid.find(cur);
            if (pit == byPid.end()) break;
            cur = pit->second->parent;
        }
        if (!covered) out.push_back(pid);
    }
    return out;
}

// ---------------------------------------------------------------------------
// System audio
//
// With nothing excluded this is one endpoint loopback and nothing more. With
// exclusions it has to be built the other way up: Windows can capture one
// application, or everything, or everything except one process tree - but not
// everything except a list. So each application that is not excluded gets its
// own capture and they are mixed, which also means FastRoute is never in the
// mix and there is no feedback path at all.
//
// The cost is that an application is picked up when the scan next runs rather
// than the instant it starts playing.
// ---------------------------------------------------------------------------
static const int kScanIntervalMs = 500;

// Several process loopbacks mixed into one capture, with the set of them
// changing as processes start and stop.
//
// Two blocks are this shape. A System Audio block with exclusions is one
// capture per application it is allowed to have, because Windows has no way to
// ask an endpoint for everything except a given list. An Application block
// naming more than one program is one capture per program, each taking that
// program whole process tree and all. What differs between them is only which
// processes to want, which is Targets below; opening them off the audio
// thread, summing them and dropping the ones that have gone is the same job.
class MixedCapture : public CaptureSource
{
public:
    // A System Audio block: everything playing to one endpoint, less the
    // applications named.
    MixedCapture(std::wstring endpointId, std::vector<std::wstring> exclude, int rate)
        : CaptureSource(rate), m_endpointId(std::move(endpointId)),
          m_exclude(std::move(exclude)) {}

    // An Application block naming several programs.
    MixedCapture(std::vector<std::wstring> include, int rate)
        : CaptureSource(rate), m_include(std::move(include)), m_byName(true) {}

    ~MixedCapture() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        if (!m_byName && m_exclude.empty())
        {
            m_single = MakeLoopbackCapture(m_endpointId, m_rate);
            return m_single->Start(err);
        }

        if (!ProcessLoopbackAvailable())
        {
            if (err) *err = m_byName
                ? FormatW(
                    L"Capturing an application needs Windows 10 version 2004 "
                    L"(build 19041) or later, and this is %s.",
                    WindowsVersionText().c_str())
                : FormatW(
                    L"Leaving applications out of the system audio needs Windows 10 "
                    L"version 2004 (build 19041) or later, and this is %s. Remove the "
                    L"exclusions to capture the output device as a whole.",
                    WindowsVersionText().c_str());
            return false;
        }

        m_stop = false;
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Rescan();                       // so the first pull already has audio
        m_thread = std::thread([this] { ScanLoop(); });
        return true;
    }

    void Stop() override
    {
        m_stop = true;
        if (m_wake) SetEvent(m_wake);
        if (m_thread.joinable()) m_thread.join();
        if (m_wake) { CloseHandle(m_wake); m_wake = nullptr; }

        std::vector<Child> dying;
        {
            std::lock_guard<std::mutex> g(m_childLock);
            dying.swap(m_children);
        }
        dying.clear();
        m_single.reset();
    }

    size_t Read(float* dst, size_t frames) override
    {
        if (m_single) return m_single->Read(dst, frames);

        const size_t samples = frames * kChannels;
        std::fill(dst, dst + samples, 0.0f);

        std::lock_guard<std::mutex> g(m_childLock);
        if (m_children.empty()) return frames;
        if (m_mix.size() < samples) m_mix.resize(samples);

        for (Child& c : m_children)
        {
            c.cap->Read(m_mix.data(), frames);
            for (size_t i = 0; i < samples; ++i) dst[i] += m_mix[i];
        }
        return frames;
    }

    size_t Backlog() override
    {
        if (m_single) return m_single->Backlog();
        std::lock_guard<std::mutex> g(m_childLock);
        size_t worst = 0;
        for (Child& c : m_children) worst = (std::max)(worst, c.cap->Backlog());
        return worst;
    }

    unsigned PacketFrames() const override
    {
        if (m_single) return m_single->PacketFrames();
        std::lock_guard<std::mutex> g(m_childLock);
        unsigned worst = 0;
        for (const Child& c : m_children) worst = (std::max)(worst, c.cap->PacketFrames());
        return worst;
    }

    unsigned Glitches() const override
    {
        if (m_single) return m_single->Glitches();
        std::lock_guard<std::mutex> g(m_childLock);
        unsigned n = 0;
        for (const Child& c : m_children) n += c.cap->Glitches();
        return n;
    }

    unsigned long long FramesIn() const override
    {
        if (m_single) return m_single->FramesIn();
        std::lock_guard<std::mutex> g(m_childLock);
        unsigned long long n = 0;
        for (const Child& c : m_children) n += c.cap->FramesIn();
        return n;
    }

    unsigned long long FramesOut() const override
    {
        if (m_single) return m_single->FramesOut();
        std::lock_guard<std::mutex> g(m_childLock);
        unsigned long long n = 0;
        for (const Child& c : m_children) n += c.cap->FramesOut();
        return n;
    }

    double Ratio() const override
    {
        if (m_single) return m_single->Ratio();
        std::lock_guard<std::mutex> g(m_childLock);
        double worst = 1.0;
        for (const Child& c : m_children)
            if (c.cap->Ratio() > worst) worst = c.cap->Ratio();
        return worst;
    }

    void Resync() override
    {
        if (m_single) { m_single->Resync(); return; }
        std::lock_guard<std::mutex> g(m_childLock);
        for (Child& c : m_children) c.cap->Resync();
    }

private:
    struct Child
    {
        unsigned long pid = 0;
        std::unique_ptr<CaptureSource> cap;
    };

    void ScanLoop()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!m_stop.load())
        {
            if (WaitForSingleObject(m_wake, kScanIntervalMs) == WAIT_OBJECT_0) break;
            if (m_stop.load()) break;
            Rescan();
        }
        CoUninitialize();
    }

    void Rescan()
    {
        std::vector<unsigned long> want = Targets();

        std::vector<unsigned long> have;
        {
            std::lock_guard<std::mutex> g(m_childLock);
            for (const Child& c : m_children) have.push_back(c.pid);
        }

        // Opening a capture client takes milliseconds, so it happens with no
        // lock held: Read() runs on the audio clock and must not wait for it.
        std::vector<Child> added;
        for (unsigned long pid : want)
        {
            bool known = false;
            for (unsigned long h : have) if (h == pid) known = true;
            if (known) continue;

            Child c;
            c.pid = pid;
            c.cap = MakeProcessLoopback(pid, m_rate);
            std::wstring err;
            if (c.cap && c.cap->Start(&err)) added.push_back(std::move(c));
            // A process that refuses to be captured is not worth a message on
            // every scan; it simply is not in the mix.
        }

        std::vector<Child> dying;
        {
            std::lock_guard<std::mutex> g(m_childLock);
            for (size_t i = m_children.size(); i-- > 0; )
            {
                bool keep = false;
                for (unsigned long w : want) if (w == m_children[i].pid) keep = true;
                if (keep) continue;
                dying.push_back(std::move(m_children[i]));
                m_children.erase(m_children.begin() + (ptrdiff_t)i);
            }
            for (Child& c : added) m_children.push_back(std::move(c));
        }
        dying.clear();   // joins their threads, outside the lock
    }

    // Which processes to be capturing right now. Runs on the scan thread with
    // no lock held, because working it out means enumerating.
    std::vector<unsigned long> Targets()
    {
        if (!m_byName)
            // Only what is playing to this block own endpoint. Taking every
            // audio process on the machine would pull in applications on other
            // output devices, which were never part of this device mix.
            return SelectSystemCaptureTargets(
                EnumAudioProcessesOn(m_endpointId), EnumProcessTree(), m_exclude,
                GetCurrentProcessId());

        // One per named program, and each capture takes that program whole
        // process tree, so a program whose audio comes out of a helper of its
        // own is covered by naming the program.
        std::vector<unsigned long> want;
        for (const std::wstring& name : m_include)
        {
            const unsigned long pid = FindProcessIdByName(name);
            if (!pid) continue;                      // not running; maybe later
            if (pid == GetCurrentProcessId()) continue;
            bool dup = false;
            for (unsigned long w : want) if (w == pid) dup = true;
            if (!dup) want.push_back(pid);
        }
        return want;
    }

    std::wstring m_endpointId;
    std::vector<std::wstring> m_exclude;
    std::vector<std::wstring> m_include;
    bool m_byName = false;
    std::unique_ptr<CaptureSource> m_single;   // the no-exclusions path

    std::vector<Child> m_children;
    mutable std::mutex m_childLock;
    std::vector<float> m_mix;
    HANDLE m_wake = nullptr;
};

std::unique_ptr<CaptureSource> MakeLoopbackCapture(const std::wstring& endpointId, int rate)
{
    return std::unique_ptr<CaptureSource>(new LoopbackCapture(endpointId, rate));
}

std::unique_ptr<CaptureSource> MakeSystemCapture(const std::wstring& endpointId,
                                                 const std::vector<std::wstring>& exclude,
                                                 int rate)
{
    return std::unique_ptr<CaptureSource>(new MixedCapture(endpointId, exclude, rate));
}

std::unique_ptr<CaptureSource> MakeMultiAppCapture(const std::vector<std::wstring>& names,
                                                   int rate)
{
    return std::unique_ptr<CaptureSource>(new MixedCapture(names, rate));
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------


bool DeviceRenderer::Start(const std::wstring& endpointId, int rate, SourceFn source,
                           std::wstring* err)
{
    // A renderer that is already running is being restarted, not started. That
    // happens when a secondary output is promoted to master - reordering the
    // blocks is enough to do it - and it has to be stopped first: assigning to
    // a joinable std::thread is std::terminate(), which takes the process out
    // on the spot with nothing logged and no dialog. Stop() is idempotent, so
    // this costs a bool test on the ordinary path.
    if (m_thread.joinable()) Stop();

    m_endpointId = endpointId;
    m_rate = rate;
    m_source = std::move(source);
    m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_stop = false;
    m_thread = std::thread([this] { ThreadMain(); });
    WaitForSingleObject(m_ready, 5000);
    if (!m_initOk)
    {
        if (err) *err = m_initErr.empty() ? L"Could not open the output device." : m_initErr;
        Stop();
        return false;
    }
    return true;
}

void DeviceRenderer::Stop()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
    m_source = nullptr;
}

void DeviceRenderer::ThreadMain()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 bufferFrames = 0;

    HRESULT hr = OpenEndpoint(m_endpointId, true, &dev);
    if (SUCCEEDED(hr))
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);

    // Deliberately the plain shared-mode path, not IAudioClient3's minimum
    // engine period. Asking for the minimum gains nothing on real hardware -
    // the engine periodicity is already held at the default by whatever else
    // is playing, so the minimum comes back as the default anyway - but on a
    // virtual endpoint with no other users it is granted literally: a 1 ms
    // period behind a 2.2 ms buffer. That runs the whole graph, resamplers and
    // effects included, every millisecond with no headroom, and it underruns
    // constantly. The plain path returns the same 22 ms buffer on every
    // endpoint on this machine, virtual or not.
    if (SUCCEEDED(hr))
    {
        WAVEFORMATEX wfx{};
        FillMixFormat(wfx, m_rate);
        // Buffer duration 0 asks for the engine's own default sizing.
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                0, 0, &wfx, nullptr);
    }

    if (SUCCEEDED(hr)) hr = client->SetEventHandle(evt);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) m_bufferFrames.store(bufferFrames);
    if (SUCCEEDED(hr))
    {
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        if (SUCCEEDED(client->GetDevicePeriod(&defPeriod, &minPeriod)))
            m_periodFrames.store((unsigned)(defPeriod * m_rate / 10000000));
    }
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));

    // WASAPI hands out a buffer noticeably larger than one engine period
    // (1056 frames against a 480 frame period is typical). Keeping it brimming
    // would make every one of those frames latency, so FillPolicy holds only
    // as much of it as this graph has actually been measured to need, starting
    // at the period and a half that used to be the fixed figure.
    const UINT32 period = m_periodFrames.load();
    FillPolicy fill;
    fill.Begin(period, bufferFrames);
    const UINT32 target = fill.Target();
    m_bufferFrames.store(target);

    // Prime with exactly that much silence so the first event does not glitch.
    if (SUCCEEDED(hr))
    {
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(target, &data)))
            render->ReleaseBuffer(target, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (SUCCEEDED(hr)) hr = client->Start();

    m_initOk = SUCCEEDED(hr);
    if (!m_initOk && m_initErr.empty())
        m_initErr = L"Output device could not be opened (" + HrText(hr) + L").";
    SetEvent(m_ready);

    if (m_initOk)
    {
        DWORD taskIndex = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        bool firstEvent = true;

        // For costing the graph. A frequency of zero would mean no timer at
        // all, in which case the policy simply never hears a cost and holds
        // its floor, which is what this did before any of it was measured.
        LARGE_INTEGER qpf{};
        if (!QueryPerformanceFrequency(&qpf)) qpf.QuadPart = 0;

        while (!m_stop.load())
        {
            if (WaitForSingleObject(evt, 200) != WAIT_OBJECT_0) continue;
            if (m_stop.load()) break;

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;

            // The endpoint ran dry before we got here: a dropout somebody
            // heard. Told to the policy directly, because not every reason for
            // one shows up as time spent producing - being late to the event
            // at all does not - and because it is the only evidence that the
            // measurement has missed something.
            if (padding == 0 && !firstEvent) fill.NoteDry();
            firstEvent = false;

            fill.Tick();
            const UINT32 want = fill.WriteNow(padding);
            m_bufferFrames.store(fill.Target());
            if (want == 0) continue;                  // already at the target

            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(want, &data))) break;

            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);

            float* dst = reinterpret_cast<float*>(data);
            if (m_source) m_source(dst, want);           // master clock
            else          m_ring.Read(dst, want);

            QueryPerformanceCounter(&t1);
            render->ReleaseBuffer(want, 0);

            // What that cost, in frames at this rate, scaled to what a whole
            // period would have cost. A write much smaller than a period is
            // mostly fixed overhead and would scale up into nonsense, so it is
            // left out rather than guessed at.
            if (period && qpf.QuadPart && want >= period / 2)
            {
                const double secs = (double)(t1.QuadPart - t0.QuadPart) /
                                    (double)qpf.QuadPart;
                const double cost = secs * (double)m_rate *
                                    (double)period / (double)want;
                if (cost > 0.0)
                    fill.NoteCost(cost < 1.0e9 ? (unsigned)cost : (unsigned)1.0e9);
            }
        }

        client->Stop();
        if (mm) AvRevertMmThreadCharacteristics(mm);
    }

    if (render) render->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (evt) CloseHandle(evt);
    CoUninitialize();
}
