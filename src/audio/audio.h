#pragma once
#include <windows.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <deque>
#include <condition_variable>

#include "model.h"

// The pipeline always mixes in stereo float32. The sample rate is not fixed:
// it is taken from the master output endpoint's own mix format, so that
// endpoint needs no conversion at all and can run on the low latency path.
// Everything else is opened with AUTOCONVERTPCM and converted into this rate.
static const int kChannels     = 2;
static const int kDefaultRate  = 48000;

// The largest block the graph will ever be asked for in one call. The render
// size follows the master output endpoint's buffer, which is a thousand frames
// or so; this is generous enough that the chunking loop inside the plugin
// hosts effectively never runs, and small enough to be a trivial allocation.
static const int kMaxRenderBlock = 4096;

// Latency budget. The mixer is clocked by the master output endpoint, so a
// capture ring only ever holds the jitter between that clock and the capture
// device's own. The prime is the cushion we start with; the backlog ceiling is
// the hard limit, past which the oldest audio is dropped rather than letting
// latency creep upwards.
inline int RingFramesFor(int rate)   { return rate / 5;  }  // 200 ms capacity
inline int MaxBacklogFor(int rate)   { return rate / 20; }  // 50 ms hard ceiling
inline int TargetBacklogFor(int rate){ return rate / 50;  }  // 20 ms working point
inline int PrimeFramesFor(int rate)  { return rate / 50;  }  // 20 ms cushion

// ---------------------------------------------------------------------------
// Endpoint / process enumeration
// ---------------------------------------------------------------------------
struct AudioEndpoint
{
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct AudioProcess
{
    unsigned long pid = 0;
    std::wstring  exeName;      // "chrome.exe"
    std::wstring  displayName;  // session display name, if any
};

std::vector<AudioEndpoint> EnumEndpoints(bool render);

// Resolves an endpoint id ("" == system default) to a device. Caller releases.
HRESULT OpenEndpoint(const std::wstring& id, bool render, struct IMMDevice** out);
// The endpoint's shared-mode mix format, which is what it runs at natively.
bool QueryEndpointFormat(const std::wstring& id, bool render, int* rate, int* channels);
std::vector<AudioProcess>  EnumAudioProcesses();

// The same, but only what is playing to one endpoint ("" == system default).
//
// This is the difference between "the system audio, minus these applications"
// and "every application on the machine, minus these". A system audio block is
// pointed at a device, and an application playing to a different one is not
// part of that device's audio - it was never in the mix that endpoint loopback
// would have captured, and it should not appear just because the block also
// has an exclusion on it.
//
// The one thing this cannot narrow is an application playing to two endpoints
// at once. Process loopback takes everything that process renders, wherever it
// is going, so such an application arrives whole or not at all.
std::vector<AudioProcess>  EnumAudioProcessesOn(const std::wstring& endpointId);

// The whole process table, parents included. A system audio block needs it to
// work out which trees to capture, and keeping it separate from the capture
// means that decision can be tested with no audio hardware in sight.
struct ProcessEntry
{
    unsigned long pid = 0;
    unsigned long parent = 0;
    std::wstring  exeName;
};
std::vector<ProcessEntry> EnumProcessTree();

// Which process trees a system audio block should capture, given what is
// playing, the process table, the exe names the user excluded, and our own pid.
//
//  * a process whose own exe is excluded is left out, and so is everything
//    below it - excluding an application excludes its child processes, the
//    same way capturing one captures them;
//  * a process below another that is already being captured is left out, since
//    that capture takes the whole tree and would otherwise count it twice;
//  * this process, and anything above it, is never captured: that would feed
//    the session's own output straight back into it.
std::vector<unsigned long> SelectSystemCaptureTargets(
    const std::vector<AudioProcess>& audio,
    const std::vector<ProcessEntry>& tree,
    const std::vector<std::wstring>& exclude,
    unsigned long self);

// ---------------------------------------------------------------------------
// The foreground application
//
// Tracked as it changes rather than sampled on demand: by the time a session
// starts, FastRoute is very often what is in front, because the user just
// pressed Ctrl+R in its window. What an application block wants is the last
// application that was in front and could actually be captured.
// ---------------------------------------------------------------------------

// Installs the foreground hook. Called once, from the window that owns the
// message loop, since an out of context hook is delivered through it.
void ForegroundWatchStart();
void ForegroundWatchStop();
// Records `hwnd`'s application, if it is one. The shell's own surfaces - the
// Start menu, the task switcher, the desktop, the taskbar - are not, and are
// ignored so that the last real application stands.
void ForegroundNote(HWND hwnd);
void ForegroundSample();          // ForegroundNote(GetForegroundWindow())
// The application to capture, or false when there is none: nothing has been in
// front yet, or what was there has since exited.
bool ForegroundApp(unsigned long* pid, std::wstring* name);

// Whether a foreground window with this class, belonging to this executable,
// is an application a block should switch to. The shell's own surfaces are
// not: the Start menu, the task switcher, the desktop and the taskbar come to
// the front constantly, and treating them as the new target would mean losing
// the audio you were listening to every time you pressed a key. Separate from
// the window handles so the rule can be tested with no windows in sight.
bool ForegroundCandidate(const std::wstring& windowClass, const std::wstring& exeName);

// The programs with a window on screen, each with its window title as the
// display name. The shell's own surfaces are left out for the same reason the
// foreground tracking leaves them out, and a packaged application's window is
// credited to the application rather than to the frame host that owns it.
std::vector<AudioProcess> EnumWindowedApps();

// What an application block offers to capture, in the order it offers them.
//
// Three groups, because "which programs could this block capture" has three
// different answers and only one of them used to be asked. What is playing
// right now is the narrowest: a program that is running with nothing to hear
// yet has no audio session at all, and nor does one whose only sounds go
// through the system sounds session, which belongs to no process. So the
// programs with a window come next, and then the names the block already has
// that are neither - a program chosen yesterday should not fall off the list
// because it happens to be quiet today.
//
// `everything` adds the rest of the process table: the programs with no window
// and nothing to hear yet, which is how a program that ought to be in the list
// and is not can still be found without typing its name.
//
// FastRoute itself is never offered, nor is anything it was started from:
// capturing one would take this process's own output with it.
struct AppCandidate
{
    std::wstring exeName;
    std::wstring displayName;   // window title or audio session name, if any
    bool playing = false;       // has an audio session now
    bool running = false;       // in the process table at all
};
std::vector<AppCandidate> AppCandidates(const std::vector<std::wstring>& ticked,
                                        bool everything);

// The endpoint an empty id resolves to, so two blocks naming the same device
// different ways can still be compared.
std::wstring DefaultEndpointId(bool render);
unsigned long FindProcessIdByName(const std::wstring& exeName);
// True when capturing `pid`'s process tree would pull in this process's own
// render stream, which would feed a session's output back into itself.
bool WouldCaptureSelf(unsigned long pid);

// ---------------------------------------------------------------------------
// What this copy of Windows can actually do
//
// FastRoute is built against the current SDK but has to run on older releases,
// so anything newer than Windows 7 is resolved at run time and the features
// that depend on it are hidden rather than offered and then failing.
// ---------------------------------------------------------------------------

// Per-application capture needs ActivateAudioInterfaceAsync (Windows 8) with
// process loopback activation parameters (Windows 10 2004, build 19041).
bool ProcessLoopbackAvailable();
// False for a block whose underlying API this Windows does not have. Such a
// block is left out of the library tree entirely.
bool BlockTypeAvailable(const std::wstring& typeId);
// "Windows 10 (build 19041)", for messages that have to explain a shortfall.
std::wstring WindowsVersionText();
std::wstring  EndpointNameById(const std::wstring& id, bool render);

// ---------------------------------------------------------------------------
// Interleaved float32 ring buffer, single producer / single consumer.
// ---------------------------------------------------------------------------
// Interleaved float32 ring buffer, single producer / single consumer, with an
// asynchronous sample rate converter on the read side.
//
// A capture device never runs at exactly the rate the output consumes at. That
// is usually a fraction of a percent of clock drift, but a device can also
// simply be wrong about its rate - one endpoint here delivers 1.22x real time.
// Either way the fix is the same: read out at a continuously adjusted ratio
// instead of one-frame-in-one-frame-out, so the backlog stays put without any
// audio being discarded to keep it there.
class Ring
{
public:
    Ring(size_t frames, size_t maxBacklog, size_t target)
        : m_buf(frames * kChannels, 0.0f), m_frames(frames),
          m_maxBacklog(maxBacklog < frames ? maxBacklog : frames),
          m_target(target) {}

    void Write(const float* src, size_t frames);
    size_t Read(float* dst, size_t frames);   // resamples; zero-fills shortfall
    void PrimeSilence(size_t frames);
    size_t Count();
    // Depth seen at the last read, which is how old the audio handed out then
    // was. Sampling Count() from another thread just races the drain.
    size_t Depth() const { return m_depth.load(); }
    void Clear();

    // Every point at which the stream was not continuous. Non-zero is audible.
    unsigned Glitches() const { return m_glitches.load(); }
    // Measured input rate relative to the output, 1.0 when the clocks agree.
    // The ratio actually being applied; 1.0 while the converter is latched off.
    double Ratio() const { return m_engagedFlag.load() ? m_ratioEst.load() : 1.0; }
    unsigned long long FramesIn() const { return m_framesIn.load(); }
    unsigned long long FramesOut() const { return m_framesOut.load(); }

private:
    const float* At(size_t idx, int ch) const
    {
        return &m_buf[((m_read + idx) % m_frames) * kChannels + ch];
    }

    std::vector<float> m_buf;
    size_t m_frames;
    size_t m_maxBacklog;
    size_t m_target;
    size_t m_read = 0, m_write = 0, m_count = 0;

    // Resampler state.
    double m_frac = 0.0;                  // fractional read position
    bool   m_engaged = false;             // conversion actually needed
    std::atomic<bool> m_engagedFlag{ false };
    std::atomic<double> m_ratioEst{ 1.0 };// smoothed input frames per output frame
    unsigned long long m_lastIn = 0;
    // Measurement window: totals, not per-read means. See Ring::Read.
    unsigned long long m_measIn = 0, m_measOut = 0;
    float m_hist[kChannels] = { 0.0f };   // frame before the read position

    std::atomic<unsigned> m_glitches{ 0 };
    std::atomic<unsigned long long> m_framesIn{ 0 }, m_framesOut{ 0 };
    std::atomic<size_t> m_depth{ 0 };
    std::mutex m_lock;
};

// ---------------------------------------------------------------------------
// Capture sources
// ---------------------------------------------------------------------------
class CaptureSource
{
public:
    virtual ~CaptureSource() {}
    virtual bool Start(std::wstring* err) = 0;
    virtual void Stop() = 0;
    // Virtual because a system audio block with exclusions is several captures
    // mixed together, and has to answer for all of them.
    virtual size_t Read(float* dst, size_t frames) { return m_ring.Read(dst, frames); }
    virtual size_t Backlog() { return m_ring.Depth(); }
    virtual unsigned PacketFrames() const { return m_packetFrames.load(); }
    virtual unsigned Glitches() const { return m_ring.Glitches(); }
    virtual unsigned long long FramesIn() const { return m_ring.FramesIn(); }
    virtual unsigned long long FramesOut() const { return m_ring.FramesOut(); }
    virtual double Ratio() const { return m_ring.Ratio(); }

    void NotePacket(unsigned n) { if (n > m_packetFrames.load()) m_packetFrames.store(n); }

    // Drops whatever accumulated while the rest of the graph was still being
    // built, so the stream starts at the cushion instead of that backlog.
    virtual void Resync() { m_ring.Clear(); m_ring.PrimeSilence(m_prime); }

    // Pushes the block's own settings into a live capture, called from
    // ApplySession with the graph lock held - the same lock Read() takes. Only
    // a capture whose behaviour depends on more than its identity needs this;
    // for everything else a change of identity builds a new capture instead.
    virtual void Configure(const Block& b) { (void)b; }
    // What is actually being captured, when that is not a fixed part of the
    // block. Empty for every capture that simply is what it was set to.
    virtual std::wstring TargetName() const { return std::wstring(); }

protected:
    explicit CaptureSource(int rate)
        : m_rate(rate), m_prime((size_t)PrimeFramesFor(rate)),
          m_ring((size_t)RingFramesFor(rate),
                 (size_t)MaxBacklogFor(rate),
                 (size_t)TargetBacklogFor(rate)) {}

    // A network stream needs far more cushion than a sound card: it arrives in
    // bursts, over a link that stalls, from a clock nobody here controls.
    //
    // The buffer is the working depth. The ceiling is three times it and the
    // ring holds four, which leaves two whole buffers of headroom above the
    // working depth for a burst to land in. That matters because a link that
    // stops for a moment and then delivers everything it was holding is the
    // ordinary case, not the exceptional one: with only one buffer of headroom
    // the catch-up overflowed and the oldest audio was thrown away frame by
    // frame, which is heard as the stream breaking up. Latency that does creep
    // up after a stall is walked back down by the drift correction instead,
    // slowly enough not to be heard doing it.
    CaptureSource(int rate, int bufferMs)
        : m_rate(rate), m_prime((size_t)rate * (size_t)bufferMs / 1000),
          m_ring((size_t)rate * (size_t)bufferMs * 4 / 1000,
                 (size_t)rate * (size_t)bufferMs * 3 / 1000,
                 (size_t)rate * (size_t)bufferMs / 1000) {}

    int m_rate;
    size_t m_prime;
    std::atomic<unsigned> m_packetFrames{ 0 };
    Ring m_ring;
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    HANDLE m_event = nullptr;
};

std::unique_ptr<CaptureSource> MakeDeviceCapture(const std::wstring& endpointId, int rate);
std::unique_ptr<CaptureSource> MakeProcessLoopback(unsigned long pid, int rate);
// Everything playing on a render endpoint, captured from the endpoint itself.
// Works on every Windows FastRoute runs on, but cannot leave anything out.
std::unique_ptr<CaptureSource> MakeLoopbackCapture(const std::wstring& endpointId, int rate);
// A system audio block. With no exclusions this is endpoint loopback; with
// them it is one process loopback per application, mixed, because Windows has
// no way to ask an endpoint for everything except a given list.
std::unique_ptr<CaptureSource> MakeSystemCapture(const std::wstring& endpointId,
                                                 const std::vector<std::wstring>& exclude,
                                                 int rate);
// An Application block naming more than one program: one process loopback per
// program, each taking that program and its children, summed. The set is
// rescanned, so a program named here that is not running yet joins the mix
// when it starts and leaves it when it stops.
std::unique_ptr<CaptureSource> MakeMultiAppCapture(const std::vector<std::wstring>& names,
                                                   int rate);
// An application block pointed at the foreground rather than at a name. With
// `follow` it keeps up as the foreground changes; without it, it resolves once
// when it starts and again whenever the block is switched back on.
std::unique_ptr<CaptureSource> MakeForegroundCapture(bool follow, int rate);
// An internet audio stream, decoded by Media Foundation.
std::unique_ptr<CaptureSource> MakeStreamCapture(const std::wstring& url, int bufferMs,
                                                 bool reconnect, int retrySeconds,
                                                 int rate);
// A stream arriving over the local network, from another FastRoute or from
// anything that speaks VBAN. The wire formats and the discovery live in net.h;
// this is the end the graph sees.
std::unique_ptr<CaptureSource> MakeNetCapture(const Block& b, int rate);

// ---------------------------------------------------------------------------
// Net Send
//
// Shaped like Recorder rather than like a renderer: Render() hands it the bus
// and it clocks nothing, so a session whose only output is a Net Send block
// still needs a device to drive it, exactly as a recorder-only session does.
// ---------------------------------------------------------------------------
class NetSink
{
public:
    virtual ~NetSink() {}
    virtual bool Start(const Block& b, int rate, std::wstring* err) = 0;
    virtual void Write(const float* buf, size_t frames) = 0;
    virtual void Configure(const Block& b) = 0;
    virtual void Stop() = 0;
    // One line for the session window: where it is going, and how much of it.
    virtual std::wstring StatusText() = 0;
};

std::unique_ptr<NetSink> MakeNetSink();

// Whether Media Foundation is on this machine at all. It is delay loaded, so
// nothing may touch it until this says so.
bool MediaFoundationAvailable();

// ---------------------------------------------------------------------------
// How deep to hold a render endpoint
//
// The endpoint is refilled from an event that fires once per engine period,
// and it plays out a period's worth between two of those events. So what has
// to be standing in it when the event arrives is one period for the playing
// out, plus however long producing the next period actually takes.
//
// That second part is the whole question, and it is not one number. An input
// wired straight to an output costs almost nothing. A session with plugins in
// it costs most of the budget, because a plugin is arbitrary code running on
// the audio thread and the graph is not finished until it returns. One figure
// guessed for both gives you a session that either crackles or is needlessly
// late, so this measures instead: it keeps the worst production cost seen over
// the last second or two and asks for exactly that much headroom. Putting a
// plugin in deepens the buffer, taking it out gives the depth back, and a
// session without one keeps the latency it has always had.
//
// Kept apart from the WASAPI loop because the arithmetic is the part that was
// wrong, and arithmetic can be checked without a sound card.
// ---------------------------------------------------------------------------
class FillPolicy
{
public:
    void Begin(unsigned period, unsigned bufferFrames)
    {
        m_period = period;
        m_buffer = bufferFrames;
        m_floor  = period / 2;
        m_cur = m_prev = 0;
        m_ticks = 0;
        // An endpoint that reports no period at all gives no basis for holding
        // less than the whole buffer, so it gets the whole buffer and the
        // measurement is left out of it entirely.
        m_target = period ? Clamp(period + m_floor) : bufferFrames;
    }

    unsigned Target() const { return m_target; }

    // How much to write now. This fills towards the target rather than to the
    // endpoint's capacity, because the rest of the WASAPI buffer is latency we
    // are choosing not to pay for.
    //
    // The cap used to be one period, which is exactly the amount that cannot
    // work: a period written against a period played out leaves the depth
    // where it was, so the depth was a fixed point. An endpoint that had once
    // run dry could never be filled again - it sat at one period with no slack
    // at all, every block after the first dropout was a race it lost, and one
    // hiccup became crackling for the rest of the session. Deepening the
    // target could not help either, because the target was a figure the writes
    // were never allowed to reach.
    //
    // A period and a half is the cap instead. It is more than a period, so the
    // depth climbs back by half a period a time and a dropout costs a few
    // milliseconds rather than the session; and it is only half a period more
    // than the steady state, so catching up asks the capture rings for a
    // little more than usual instead of swallowing one whole.
    unsigned WriteNow(unsigned padding) const
    {
        if (padding >= m_target || padding >= m_buffer) return 0;
        unsigned want = m_target - padding;
        const unsigned most = m_period + m_period / 2;
        if (most && want > most) want = most;
        const unsigned room = m_buffer - padding;
        return want < room ? want : room;
    }

    // One measured production, as frames at the session rate, already scaled
    // to what a period's worth of work would have cost.
    void NoteCost(unsigned costFrames)
    {
        if (costFrames > m_cur) m_cur = costFrames;
    }

    // The endpoint was found empty, which is a dropout that was heard. The
    // cost measurement cannot see every reason for one - being late to the
    // event at all does not show up as time spent producing - so this says
    // plainly that whatever the graph costs, it did not fit in the slack it
    // had, and asks for a quarter period more than it had.
    void NoteDry()
    {
        const unsigned slack = m_target > m_period ? m_target - m_period : 0;
        const unsigned want  = slack + (m_period ? m_period / 4 : 1);
        const unsigned cost  = want / 2;        // Tick doubles it again
        if (cost > m_cur) m_cur = cost;
    }

    // Once per wake. Rolls the measurement window on and walks the target
    // towards what the measurement is asking for.
    void Tick()
    {
        if (m_period == 0) return;      // nothing to measure against

        if (++m_ticks >= kWindowTicks)
        {
            m_prev = m_cur;
            m_cur = 0;
            m_ticks = 0;
        }

        const unsigned worst = m_cur > m_prev ? m_cur : m_prev;
        // Twice the worst seen, because the worst that has not happened yet is
        // worse than the worst that has, and a few milliseconds of headroom
        // costs less than one dropout.
        unsigned need = worst * 2;
        if (need < m_floor) need = m_floor;
        const unsigned want = Clamp(m_period + need);

        // Up in quarter periods and down in sixteenths: deepening is what
        // stops a dropout, and shallowing is only ever a convenience, so it
        // should not be the thing that causes one.
        if (want > m_target)
            m_target += Step(want - m_target, m_period / 4);
        else if (want < m_target)
            m_target -= Step(m_target - want, m_period / 16);
    }

private:
    static unsigned Step(unsigned by, unsigned most)
    {
        if (most == 0) return by;
        return by < most ? by : most;
    }

    unsigned Clamp(unsigned v) const
    {
        return (m_buffer && v > m_buffer) ? m_buffer : v;
    }

    // Roughly a second a bucket at any ordinary period, so the maximum is over
    // one to two seconds: long enough that a plugin's occasional expensive
    // block still counts, short enough that taking the plugin out gives the
    // latency back while you are still looking at the session window.
    static const unsigned kWindowTicks = 100;

    unsigned m_period = 0, m_buffer = 0, m_floor = 0;
    unsigned m_target = 0;
    unsigned m_cur = 0, m_prev = 0;    // two bucket decaying maximum
    unsigned m_ticks = 0;
};

// ---------------------------------------------------------------------------
// Renderer
//
// A renderer given a SourceFn pulls audio directly from it on its own WASAPI
// event - that renderer is the master clock for the whole session, and nothing
// is buffered between the mix and the endpoint. Any further renderer runs in
// ring mode and is fed by the master.
// ---------------------------------------------------------------------------
// What every output has in common, so that the engine can own the clock
// without caring which of them owns it. A WASAPI endpoint and an ASIO driver
// are told apart only where they genuinely differ.
class Renderer
{
public:
    using SourceFn = std::function<void(float*, size_t)>;

    virtual ~Renderer() {}
    // `target` is an endpoint id for a device renderer and a driver name for
    // an ASIO one. A non-null `source` makes this renderer the master clock;
    // null puts it in ring mode, fed by whoever is.
    virtual bool Start(const std::wstring& target, int rate, SourceFn source,
                       std::wstring* err) = 0;
    virtual void Stop() = 0;
    virtual void Write(const float* src, size_t frames) = 0;
    virtual unsigned BufferFrames() const = 0;
    virtual unsigned PeriodFrames() const = 0;
    // Anything that is the block's business rather than the endpoint's -
    // which two channels of a sixteen channel interface, for instance. Called
    // before Start. A device renderer has nothing of the sort.
    virtual void Configure(const Block& b) { (void)b; }
};

class DeviceRenderer : public Renderer
{
public:
    ~DeviceRenderer() { Stop(); }
    bool Start(const std::wstring& endpointId, int rate, SourceFn source,
               std::wstring* err) override;
    void Stop() override;
    void Write(const float* src, size_t frames) override { m_ring.Write(src, frames); }
    unsigned BufferFrames() const override { return m_bufferFrames.load(); }
    unsigned PeriodFrames() const override { return m_periodFrames.load(); }

private:
    void ThreadMain();
    std::atomic<unsigned> m_bufferFrames{ 0 };
    std::atomic<unsigned> m_periodFrames{ 0 };
    int m_rate = kDefaultRate;
    Ring m_ring{ (size_t)RingFramesFor(kDefaultRate),
                 (size_t)MaxBacklogFor(kDefaultRate),
                 (size_t)TargetBacklogFor(kDefaultRate) };
    SourceFn m_source;
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::wstring m_endpointId;
    HANDLE m_ready = nullptr;
    bool   m_initOk = false;
    std::wstring m_initErr;
};

// ---------------------------------------------------------------------------
// ASIO
//
// Both ends of one driver. They are two views of a single callback rather than
// two independent things, so whichever starts first opens the device and the
// other joins it; the details are in asioio.cpp.
//
// An ASIO block can be mixed freely with WASAPI ones. Whichever output comes
// first owns the clock and the rest run in ring mode, exactly as two WASAPI
// outputs already do - so ASIO in to WASAPI out, and the other way about,
// both work, and only the pairing of an ASIO input with an ASIO output on the
// same driver avoids a ring altogether.
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> MakeAsioRenderer();
std::unique_ptr<CaptureSource> MakeAsioCapture(const Block& b, int rate);

// What rate the named driver is running at, for a session that ASIO is
// clocking. Opens the driver, so this is not idle curiosity: the engine calls
// it when it is about to use the thing anyway.
bool AsioQueryRate(const std::wstring& driver, int* rate);

// Everything ASIO is holding is let go. Called when a session stops, because
// a driver left open is a driver no other program can use.
void AsioReleaseIfIdle();

// What a driver actually has, for the dialog that offers its channels. There
// is no way to learn this without opening the driver, which is why a dialog
// asks only when somebody picks one, and why a refusal comes back as prose
// rather than as nothing: "in use by another program" is the answer somebody
// needs to see.
struct AsioFacts
{
    bool ok = false;
    std::wstring why;              // when it is not ok
    int rate = 0;
    int bufferFrames = 0;
    std::vector<std::wstring> inNames, outNames;
};
void AsioQueryDriver(const std::wstring& driver, AsioFacts* out);

// The driver's own settings window, where buffer size and clock source live.
void AsioShowControlPanel(const std::wstring& driver, HWND parent);

// ---------------------------------------------------------------------------
// Effects
//
// Effects work in place on the interleaved stereo bus. The denoisers are
// frame based and mono, so they buffer up to a frame and run one instance per
// channel, which costs one frame of latency.
// ---------------------------------------------------------------------------
class Effect
{
public:
    virtual ~Effect() {}
    virtual bool Start(int rate, std::wstring* err) = 0;
    virtual void Process(float* buf, size_t frames) = 0;
    virtual int  LatencyFrames() const { return 0; }
    virtual void SetMix(float wet) { m_wet = wet; }
    // Pushes the block's own parameters into a live effect. Called from
    // ApplySession with the graph lock held, which is the same lock Render()
    // takes, so an implementation may recompute filter coefficients in place.
    virtual void Configure(const Block& b) { (void)b; }

    // Effects that write files of their own are handed the same settings a
    // Recorder block would get, session name included. Only the buffer wants
    // it; everything else ignores it.
    virtual void ConfigureRecording(const struct RecordSettings& rs) { (void)rs; }

    // Do something, now, to a running effect. Unlike Configure this is not a
    // setting being pushed in - it is a moment, and nothing about it is saved.
    // Called from the UI thread with the graph lock held, so it must be cheap:
    // set a flag, move a pointer, and let the effect's own threads do the work.
    virtual void DoAction(int action) { (void)action; }

    // One line about what the effect is doing that its parameters do not say -
    // how far behind live a buffer is, say. Empty when there is nothing to add.
    // Called with the graph lock held.
    virtual std::wstring StatusText() { return std::wstring(); }

protected:
    std::atomic<float> m_wet{ 1.0f };
};

std::unique_ptr<Effect> MakeSpeexDenoise();
std::unique_ptr<Effect> MakeRnnDenoise();
std::unique_ptr<Effect> MakeToneControl();   // bass and treble shelves
std::unique_ptr<Effect> MakeEqualiser();     // kEqBands peaking filters
std::unique_ptr<Effect> MakeParametricEq();  // kParaBands filters of any shape
std::unique_ptr<Effect> MakeChannelMixer();  // per channel volume and pan,
                                            // width, reverse, centre cancel
std::unique_ptr<Effect> MakeCompressor();    // threshold, ratio, attack, release
std::unique_ptr<Effect> MakeNormaliser();    // a running target level, with lookahead
// Watches for clipping and beeps out of band. Alters nothing it is given.
std::unique_ptr<Effect> MakeClipMonitor();
// Keeps the last so many seconds, and can be rewound through them.
std::unique_ptr<Effect> MakeBuffer();
std::unique_ptr<Effect> MakeLimiter();       // ceiling, with lookahead
std::unique_ptr<Effect> MakeNoiseGate();     // shuts below a threshold
std::unique_ptr<Effect> MakePitchShift();   // moves the pitch, not the speed
std::unique_ptr<Effect> MakePluginEffect();  // an external CLAP or VST3 plugin

// Builds the effect for a block type, or nullptr if it is not an effect that
// needs one (the gain block is applied by the engine directly).
std::unique_ptr<Effect> MakeEffectFor(const std::wstring& typeId);

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------
// The stored value, so the order is fixed once written. Wav, Flac and Mp3
// were there first; the rest were added when the Ogg libraries arrived for the
// stream block and could serve in this direction too.
enum class RecFormat { Wav = 0, Flac = 1, Mp3 = 2, Aac = 3, Ogg = 4, Opus = 5 };
static const int kRecFormatCount = 6;
const wchar_t* RecFormatName(RecFormat fmt);       // "MP3 (lossy)"
const wchar_t* RecFormatShortName(RecFormat fmt);  // "MP3"
// Whether the bitrate setting means anything for this format. WAV and FLAC
// are what they are; the rest are told how many bits a second to spend.
bool RecFormatUsesBitrate(RecFormat fmt);

// What one file is actually written as, after the block's choices and the
// format's own limits have both been applied. Write() is always called at
// `inRate` - the session's rate - and the recorder converts to `rateHz` on its
// own writer thread, so nothing of this reaches the audio clock.
struct RecordFormat
{
    int inRate = kDefaultRate;   // the rate Write() is called at
    int rateHz = kDefaultRate;   // what goes into the file
    int bits   = 16;             // 16, 24, or 32 for float; ignored if lossy
    int kbps   = 192;
};

class Recorder
{
public:
    virtual ~Recorder() {}
    // `path` is the fully resolved output file, extension included.
    virtual bool Start(const std::wstring& path, const RecordFormat& f,
                       std::wstring* err) = 0;
    virtual void Write(const float* buf, size_t frames) = 0;
    virtual void Stop() = 0;
    virtual std::wstring Path() const = 0;
};

// The rates a format can be written at here, ascending. Empty means any rate,
// which is the case for the two formats FastRoute writes itself and can size
// freely. For the three Windows encodes this is what its encoder actually
// offers, asked once and remembered - and it is not the same list for each:
// FLAC starts at 44100 and MP3 stops at 48000.
const std::vector<int>& RecFormatRates(RecFormat fmt);
// The bit depths worth offering. Empty for a lossy format, which has none:
// what it spends per second is the bitrate, not a sample width.
const std::vector<int>& RecFormatDepths(RecFormat fmt);
// The bitrates worth offering for a format at a rate, ascending. Empty when
// the format has no bitrate at all.
//
// The rate is a parameter because for Ogg Vorbis the two are not independent:
// libvorbis refuses 192 kbps at 22050 Hz outright. MP3 and AAC never refuse -
// they take the nearest their encoder advertises - so for those this is the
// whole list whatever the rate.
std::vector<int> RecFormatBitrates(RecFormat fmt, int rateHz);
// Whether libvorbis will set up for this pair at all. Lives with the Vorbis
// encoder, in oggout.cpp, and is asked by the two lists above.
bool VorbisAccepts(int rate, int kbps);
// The bitrates a recorder block offers before any format has had its say.
extern const int kRecBitrates[];
extern const int kRecBitrateCount;
// A block's rate and depth, resolved against the format's limits and the rate
// the session is running at. Zero for either asks for the default. This is the
// one place that decides, so the recorder that writes the file, the accounting
// that works out how large it is, and the dialog that offers the choices all
// agree about what was asked for.
RecordFormat RecFileFormatFor(RecFormat fmt, int rateHz, int bits, int kbps,
                              int sessionRate);

std::unique_ptr<Recorder> MakeRecorder(RecFormat fmt);
std::unique_ptr<Recorder> MakeVorbisRecorder();
std::unique_ptr<Recorder> MakeOpusRecorder();
// True when Windows can actually encode this format here. Windows 10 ships a
// FLAC encoder MFT and an MP3 one; older releases ship neither, so the format
// is dropped from the recorder block rather than failing when you press run.
bool RecFormatAvailable(RecFormat fmt);
// Expands {session}, {block}, {index}, {date}, {time}, {datetime} and {n} and
// appends the extension for `fmt`, avoiding a name that already exists.
// {block} comes out empty when there is no one block the file is of, which is
// the ordinary case: a recorder's mixed file is of the whole bus.
//
// {index} is `blockIndex`, which is a block's place in the session counted
// from one, zero-padded to the width `blockCount` needs - so a session of
// nine blocks or fewer numbers them 1 to 9 and one of twelve numbers them 01
// to 12, and either way the files sort into pipeline order. Zero leaves it
// empty, like an absent {block}.
//
// A slash or a backslash in the template makes a folder: "{session}\\{block}"
// writes a folder per session with a file per block inside it, and the folders
// are created. A token's own value cannot do that - those are cleaned first -
// and ".." is dropped, so nothing is written outside `folder`.
std::wstring ResolveRecordingPath(const std::wstring& folder,
                                  const std::wstring& nameTemplate,
                                  const std::wstring& sessionName,
                                  RecFormat fmt,
                                  const std::wstring& blockName = std::wstring(),
                                  int blockIndex = 0, int blockCount = 0);
const wchar_t* RecFormatExt(RecFormat fmt);

// Everything a recorder block is set to. The first four decide which file is
// being written and so cannot change without starting a new one; the rest are
// applied to a running recording through RecordSink::Configure.
struct RecordSettings
{
    std::wstring folder;
    std::wstring nameTemplate;
    std::wstring sessionName;
    // Which block this file is of, for {block}. Empty for a recorder's own
    // mixed file: there is no one block it is of.
    std::wstring blockName;
    // That block's place in the session, counted from one, and how many
    // blocks there are - which together are {index}. Zero for neither known.
    int  blockIndex = 0;
    int  blockCount = 0;
    RecFormat fmt = RecFormat::Wav;
    int  kbps = 192;
    int  rateHz = 0;            // 0 == the rate the session is running at
    int  bits = 0;              // 0 == the format's own default

    bool skipSilence = false;
    int  silenceDb = -50;       // dBFS
    int  holdSeconds = 2;

    int  splitMode = 0;         // 0 never, 1 minutes, 2 megabytes
    int  splitMinutes = 30;
    int  splitMb = 100;
};

RecordSettings RecordSettingsFor(const Block& b, const std::wstring& sessionName,
                                 int blockIndex = 0, int blockCount = 0);
// The same, for one input block's own file, from the recorder block that asked
// for it. Separate tracks are worth having because they line up, so the two
// settings that would cut each file at a different moment - the silence gate,
// and splitting by size - are not passed on. See TrackSettingsFor.
// `blockIndex` is the input block's own place in the session, not the
// recorder's: the file is of that block, so that is what {index} must mean.
RecordSettings TrackSettingsFor(const Block& recorderBlock,
                                const std::wstring& sessionName,
                                const std::wstring& blockName,
                                int blockIndex = 0, int blockCount = 0);

// ---------------------------------------------------------------------------
// One recorder block's output: the file being written, plus the silence gate
// and the file splitting wrapped around it.
//
// The mixer thread only ever queues into this. Everything else - metering,
// gating, and opening and closing files - happens on the sink's own thread,
// because closing a file finalises an encoder and opening one hits the disk,
// and neither belongs anywhere near the audio clock.
// ---------------------------------------------------------------------------
class RecordSink
{
public:
    ~RecordSink() { Stop(); }

    bool Start(const RecordSettings& s, int rate, std::wstring* err);
    void Write(const float* buf, size_t frames);   // mixer thread: queues only
    void Stop();
    // Changes the gate and the splitting under a running recording.
    void Configure(const RecordSettings& s);

    std::wstring Path();
    int  Files() const { return m_files.load(); }
    // How much audio has actually been written, across every file. Not the
    // same as how long the session has been running: the silence gate can
    // hold off for most of it.
    unsigned long long RecordedFrames() const { return m_totalFrames.load(); }
    // How large the file being written is now. Read from the sink's own
    // accounting for WAV and from the file system for the encoded formats,
    // which is the same thing the splitting already relies on.
    unsigned long long FileBytes() { return m_bytes.load(); }
    // True while the gate is shut, so the UI can say why nothing is being
    // written - there is no level meter to look at.
    bool Skipping() const { return m_skipping.load(); }

private:
    void Worker();
    bool Open(std::wstring* err);      // sink thread only
    void CloseFile();                  // sink thread only
    bool Rotate();                     // sink thread only
    void Consume(const float* buf, size_t frames);
    void Emit(const float* buf, size_t frames);
    bool CheckSize(const RecordSettings& s);
    unsigned long long CurrentBytes(const RecordSettings& s);
    RecordSettings Snapshot();
    // m_fileFrames is counted as the audio arrives, at the session's rate.
    // How many frames that is in the file is a different number whenever the
    // block records at a rate of its own.
    unsigned long long FileFrames() const
    {
        if (m_fileRate == m_rate || m_rate <= 0) return m_fileFrames;
        return (unsigned long long)((double)m_fileFrames * m_fileRate / m_rate);
    }

    RecordSettings m_set;
    std::mutex m_setLock;

    int m_rate = kDefaultRate;         // the session's rate: what arrives
    // What the file is written as, which need not be what arrives. Kept here
    // because a WAV's size follows from its own frame count and sample width
    // exactly, and that is what the splitting relies on.
    int m_fileRate = kDefaultRate;
    int m_fileBytesPerFrame = kChannels * 2;
    std::unique_ptr<Recorder> m_file;
    std::atomic<int> m_files{ 0 };
    std::atomic<bool> m_skipping{ false };
    bool m_dead = false;               // a rotation failed; stop writing

    // Gate state, sink thread only.
    std::vector<float> m_acc;          // partial metering block
    std::deque<float> m_preRoll;       // audio held back while the gate is shut
    bool   m_open = true;
    size_t m_quietFrames = 0;

    std::atomic<unsigned long long> m_totalFrames{ 0 };
    std::atomic<unsigned long long> m_bytes{ 0 };

    // Split accounting, sink thread only.
    unsigned long long m_fileFrames = 0;
    size_t m_sinceSizeCheck = 0;

    std::deque<float> m_queue;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };

    // Handshake for the first file, so Start() can report why it failed.
    std::mutex m_openLock;
    std::condition_variable m_openCv;
    bool m_openDone = false, m_openOk = false;
    std::wstring m_openErr;
};

// ---------------------------------------------------------------------------
// Engine: one running session.
// ---------------------------------------------------------------------------
class Engine
{
public:
    ~Engine() { Stop(); }

    bool Start(const Session& s, std::wstring* err);
    void Stop();
    bool Running() const { return m_running; }

    // Rebuilds the graph in place against `s` while audio keeps flowing.
    // Blocks whose identity is unchanged keep their live capture or render
    // stream, so reordering, retargeting, adding and removing all happen
    // without a restart.
    // `graphIntact` reports whether a failure left the running graph untouched.
    bool ApplySession(const Session& s, std::wstring* err, bool* graphIntact = nullptr);

    // Tell one block's effect to do something, now. False if that block is not
    // in the running graph or has no effect to tell - a session that has been
    // edited since the hotkey was registered, most likely.
    bool PerformAction(int blockIndex, int action);

    // Runs the whole block list for `frames`. Called on the master renderer's
    // thread only.
    void Render(float* out, size_t frames);

    struct Status
    {
        int  outMs = 0;        // audio queued at the master output endpoint
        int  inMs = 0;         // deepest capture ring backlog
        int  rate = kDefaultRate;
        int  outFrames = 0;    // master endpoint buffer, in frames
        int  periodFrames = 0; // master endpoint engine period, in frames
        int  fxMs = 0;         // latency added by frame based effects
        int  inPacketFrames = 0; // largest capture packet seen, in frames
        int  recFiles = 0;       // files written by the recorder blocks so far
        // How long this session has been running, and how much of that has
        // ended up in a file. There is no clock on screen otherwise.
        int  runSeconds = 0;
        int  recSeconds = 0;
        unsigned long long recBytes = 0;
        // What a foreground application block has hold of at the moment. There
        // is no other way to find out, and in follow mode it is the one thing
        // worth knowing.
        std::wstring following;
        bool recSkipping = false;// a recorder is holding off, below its threshold
        unsigned glitches = 0;   // discontinuities since the session started
        unsigned long long framesIn = 0, framesOut = 0;
        double inRatio = 1.0;  // worst capture rate ratio seen
        // A line per block that has something to say beyond its parameters,
        // already named. The buffer uses it to say how far back it is.
        std::vector<std::wstring> blockStatus;
    };
    void GetStatus(Status* st);

private:
    struct Node
    {
        BlockKind kind = BlockKind::Effect;
        std::wstring typeId;
        std::wstring key;                 // identity used to reuse live streams
        std::wstring deviceId;
        std::atomic<float> gain{ 1.0f };
        std::atomic<bool> enabled{ true };
        std::atomic<float> mix{ 1.0f };   // effect dry/wet
        std::unique_ptr<CaptureSource> capture;
        std::unique_ptr<Renderer> renderer;
        std::unique_ptr<Effect> fx;
        std::unique_ptr<RecordSink> rec;
        std::unique_ptr<NetSink> net;
        // An input block's own file, when a Recorder below it in the list is
        // writing a track per block. It hangs off the input rather than off
        // the recorder because the input is the thing whose identity is
        // stable: adding a block above a running recorder must not restart
        // the recorder's own file. The key is the recorder's, so changing the
        // folder or the format starts the tracks again too.
        std::unique_ptr<RecordSink> trackRec;
        std::wstring trackKey;
        // Whether the recorder that asked for this track is switched on. Not
        // the same question as this block's own enabled flag: an input can be
        // out of the mix while the recording runs, and the recording can be
        // paused while the input plays on.
        std::atomic<bool> trackOn{ true };
    };

    static std::wstring KeyFor(const Block& b);

    std::vector<std::unique_ptr<Node>> m_nodes;
    size_t m_master = (size_t)-1;        // index of the clock-owning output
    // A session that only records has no output block to clock it, so one is
    // opened on the default device purely to drive the graph. It renders
    // silence: nothing in the block list writes to it.
    std::unique_ptr<DeviceRenderer> m_clock;
    int m_rate = kDefaultRate;           // taken from the master endpoint
    std::mutex m_graph;                  // guards m_nodes / m_master vs Render()
    std::vector<float> m_bus, m_tmp, m_out;
    std::vector<float> m_trk;            // one input's own audio, for its track
    // When Start() succeeded, for the running time. A tick count rather than a
    // wall clock: it cannot go backwards when the clock is put right.
    unsigned long long m_startTick = 0;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_resyncPending{ false };
};
