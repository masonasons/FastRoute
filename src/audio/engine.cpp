#include "audio.h"
#include "net.h"
#include "util.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// Pipeline semantics
//
// The block list is walked top to bottom over a single bus:
//   * an input block  mixes its (volume scaled) capture into the bus
//   * an effect block transforms whatever is currently on the bus
//   * an output block renders the current bus contents to its endpoint
//
// That is what lets the three kinds interleave freely: putting an output above
// a second input means the first output only carries the blocks above it.
//
// The first output block is the master clock. Its renderer calls Render() from
// its own WASAPI event, so the whole graph runs at exactly the rate the sound
// card consumes and nothing is buffered between the mix and the endpoint.
// ---------------------------------------------------------------------------

static const size_t kNone = (size_t)-1;

static float PctToGain(int pct)
{
    if (pct < 0) pct = 0;
    return (float)pct / 100.0f;
}

// Two blocks share a key when they can share one live audio stream. Editing a
// block's volume or enabled flag keeps the key; retargeting it to a different
// device or application changes it, which is what forces a new stream.
std::wstring Engine::KeyFor(const Block& b)
{
    if (b.typeId == BT_INPUT_APP)
    {
        // A foreground block's identity is its mode, not a process: the whole
        // point is that the process changes underneath it.
        if (b.appSource != AS_NAMED) return FormatW(L"app|mode%d", b.appSource);
    {
        // Waiting is in the key because it decides which capture is built -
        // the rescanning one or the fixed single - and not merely what that
        // capture carries.
        std::wstring key = L"app|";
        for (const std::wstring& n : AppTargets(b)) key += n + L"|";
        return key + b.processName + (b.appWaitForStart ? L"|wait" : L"");
    }
    }
    if (b.typeId == BT_INPUT_DEVICE)
        return L"in|" + b.deviceId;
    if (b.typeId == BT_INPUT_STREAM)
    {
        // Everything here decides how the connection is made, so any of them
        // changing means a new one.
        return FormatW(L"url|%s|%d|%d|%d", b.url.c_str(), b.bufferMs,
                       b.reconnect ? 1 : 0, b.retrySeconds);
    }
    if (b.typeId == BT_INPUT_SYSTEM)
    {
        // The exclusions decide how the capture is built, not just what it
        // carries, so changing them has to build a new one.
        std::wstring key = L"sys|" + b.deviceId;
        for (const std::wstring& e : b.excludeApps) key += L"|" + e;
        return key;
    }
    if (b.typeId == BT_OUTPUT_DEVICE)
        return L"out|" + b.deviceId;
    if (b.typeId == BT_INPUT_ASIO)
        return FormatW(L"asioin|%s|%d|%d", b.deviceId.c_str(), b.asioInLeft,
                       b.asioInRight);
    if (b.typeId == BT_OUT_ASIO)
        return FormatW(L"asioout|%s|%d|%d", b.deviceId.c_str(), b.asioOutLeft,
                       b.asioOutRight);
    if (b.typeId == BT_INPUT_NET)
    {
        // The buffer depth sizes the ring, and the rest decides which socket
        // is opened and which group is joined, so all of it is identity.
        return FormatW(L"netin|%d|%s|%d|%s|%d|%u", b.netProtocol,
                       b.netAddress.c_str(), b.netPort,
                       b.netStreamName.c_str(), b.netBufferMs, b.netSourceId);
    }
    if (b.typeId == BT_OUT_NET)
    {
        // Likewise, with the codec and the packet length in it: both are built
        // into the encoder rather than pushed at it.
        // Everything, because all of it is built into the encoder or the
        // socket rather than pushed at them, so any change means a new node.
        return FormatW(L"netout|%d|%d|%s|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d|%d",
                       b.netProtocol, b.netMode, b.netAddress.c_str(),
                       b.netPort, b.netStreamName.c_str(), b.netCodec,
                       b.netPacketMs, b.netOpusKbps, b.netTtl,
                       b.netOpusLossPct, b.netOpusComplexity, b.netOpusSignal,
                       b.netOpusRateMode, b.netOpusDtx ? 1 : 0);
    }
    if (b.typeId == BT_OUT_RECORDER)
    {
        // Anything that changes where the audio lands has to start a new file,
        // so it all belongs in the key. Volume and enabled deliberately do not.
        // The mode is in it because it decides whether this block writes a
        // mixed file at all.
        return FormatW(L"rec|%s|%s|%d|%d|%d|%d|%d", b.folder.c_str(),
                       b.nameTemplate.c_str(), b.recFormat, b.bitrateKbps,
                       b.recMode, b.recRateHz, b.recBits);
    }
    return b.typeId;
}

bool Engine::Start(const Session& s, std::wstring* err)
{
    Stop();
    if (!ApplySession(s, err)) { Stop(); return false; }
    m_startTick = GetTickCount64();
    m_running = true;
    return true;
}

bool Engine::ApplySession(const Session& s, std::wstring* err, bool* graphIntact)
{
    // Everything up to phase C is planning and fresh stream setup, so a
    // failure there leaves the running graph exactly as it was.
    if (graphIntact) *graphIntact = true;

    bool hasInput = false, hasOutput = false;
    for (const Block& b : s.blocks)
    {
        if (b.Kind() == BlockKind::Input)  hasInput = true;
        if (b.Kind() == BlockKind::Output) hasOutput = true;
    }
    if (!hasInput)  { if (err) *err = L"This session has no input blocks."; return false; }
    if (!hasOutput) { if (err) *err = L"This session has no output blocks."; return false; }

    // The pipeline runs at the master output endpoint's native rate, so that
    // endpoint needs no conversion and can be opened on the low latency path.
    // A master whose rate differs from the one already running means every
    // stream has to be rebuilt, so hand that case straight to Start().
    std::wstring masterDevice;
    bool masterIsAsio = false;
    for (const Block& b : s.blocks)
        if (b.Kind() == BlockKind::Output)
        {
            masterDevice = b.deviceId;
            masterIsAsio = (b.typeId == BT_OUT_ASIO);
            break;
        }

    int wantRate = kDefaultRate;
    if (masterIsAsio)
    {
        // The driver decides, and it is opened to ask. A driver that cannot
        // be asked leaves the default, and starting it will then say why.
        if (!AsioQueryRate(masterDevice, &wantRate)) wantRate = kDefaultRate;
    }
    else if (!QueryEndpointFormat(masterDevice, true, &wantRate, nullptr))
        wantRate = kDefaultRate;

    if (!m_nodes.empty() && wantRate != m_rate)
    {
        if (graphIntact) *graphIntact = false;
        Stop();
        return Start(s, err);
    }
    m_rate = wantRate;

    const size_t count = s.blocks.size();

    // ---- Phase A: plan ----------------------------------------------------
    // Match each new block against a live node with the same key. Nothing is
    // moved yet, so bailing out here leaves the running graph untouched.
    std::vector<size_t> reuse(count, kNone);
    std::vector<bool> taken(m_nodes.size(), false);

    for (size_t i = 0; i < count; ++i)
    {
        std::wstring key = KeyFor(s.blocks[i]);
        for (size_t j = 0; j < m_nodes.size(); ++j)
        {
            if (taken[j] || m_nodes[j]->key != key) continue;
            taken[j] = true;
            reuse[i] = j;
            break;
        }
    }

    // ---- Phase B: build and start what is genuinely new --------------------
    std::vector<std::unique_ptr<Node>> created(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (reuse[i] != kNone) continue;
        const Block& b = s.blocks[i];

        std::unique_ptr<Node> n(new Node());
        n->kind = b.Kind();
        n->typeId = b.typeId;
        n->key = KeyFor(b);
        n->deviceId = b.deviceId;

        if (b.typeId == BT_INPUT_APP)
        {
            if (!ProcessLoopbackAvailable())
            {
                if (err) *err = FormatW(
                    L"Capturing a single application needs Windows 10 version 2004 "
                    L"(build 19041) or later, and this is %s. Capture an input "
                    L"device instead, or route the application to one.",
                    WindowsVersionText().c_str());
                return false;
            }
            if (b.appSource != AS_NAMED)
            {
                n->capture = MakeForegroundCapture(b.appSource == AS_FOLLOW, m_rate);
                if (!n->capture->Start(err)) return false;
                created[i] = std::move(n);
                continue;
            }
            const std::vector<std::wstring> want = AppTargets(b);

            // Capturing ourselves is refused whichever program asks for it,
            // because the session would feed its own output back in. Checked
            // for every name, not just the first.
            for (const std::wstring& name : want)
            {
                const unsigned long p = FindProcessIdByName(name);
                if (p && WouldCaptureSelf(p))
                {
                    if (err) *err = FormatW(
                        L"Capturing %s would include FastRoute itself, because "
                        L"this process is inside that application's process "
                        L"tree - the session would capture its own output. Pick "
                        L"a different application.", name.c_str());
                    return false;
                }
            }

            // The rescanning capture takes a program into the mix when it
            // starts and drops it when it stops, so a block told to wait does
            // not need anything running at all - including a block naming one
            // program, which would otherwise take the fixed single capture
            // below and need a process id now.
            //
            // Nothing that starts later can be an ancestor of a process that
            // is already running, so the check above is the whole of it: what
            // joins the mix afterwards can only be a descendant or unrelated,
            // and Targets() leaves this process itself out.
            if (want.size() > 1 || b.appWaitForStart)
            {
                // One capture per program, rescanned. That is the point of a
                // block naming several: a games block where only one game is
                // ever running at a time is normal, so the ones that are not
                // running are not an error.
                if (!b.appWaitForStart)
                {
                    int running = 0;
                    for (const std::wstring& name : want)
                        if (FindProcessIdByName(name)) ++running;
                    if (running == 0)
                    {
                        if (err) *err = FormatW(
                            L"None of the %zu applications in this block is "
                            L"running, so there is nothing to capture yet. Tick "
                            L"\"Wait for them to start\" in the block to run "
                            L"anyway and pick them up as they launch.",
                            want.size());
                        return false;
                    }
                }
                n->capture = MakeMultiAppCapture(want, m_rate);
                if (!n->capture->Start(err)) return false;
            }
            else
            {
                const std::wstring only = want.empty() ? b.processName : want[0];
                unsigned long pid = FindProcessIdByName(only);
                if (pid == 0)
                {
                    if (err) *err = FormatW(
                        L"%s is not running, so it cannot be captured. Tick "
                        L"\"Wait for them to start\" in the block to run anyway "
                        L"and pick it up when it launches.", only.c_str());
                    return false;
                }
                n->capture = MakeProcessLoopback(pid, m_rate);
                if (!n->capture->Start(err)) return false;
            }
        }
        else if (b.typeId == BT_INPUT_DEVICE)
        {
            n->capture = MakeDeviceCapture(b.deviceId, m_rate);
            if (!n->capture->Start(err)) return false;
        }
        else if (b.typeId == BT_INPUT_STREAM)
        {
            if (b.url.empty())
            {
                if (err) *err = L"This stream block has no address. Press Enter "
                                L"on it to set one.";
                return false;
            }
            n->capture = MakeStreamCapture(b.url, b.bufferMs, b.reconnect,
                                           b.retrySeconds, m_rate);
            if (!n->capture->Start(err)) return false;
        }
        else if (b.typeId == BT_INPUT_SYSTEM)
        {
            // Endpoint loopback takes the endpoint's whole mix, this session's
            // own contribution included. Rendering back to the device being
            // captured is therefore a feedback loop, and the only honest thing
            // to do is refuse it: it does not go quiet, it builds.
            if (b.excludeApps.empty())
            {
                std::wstring capturing = b.deviceId.empty() ? DefaultEndpointId(true)
                                                            : b.deviceId;
                for (const Block& o : s.blocks)
                {
                    if (o.typeId != BT_OUTPUT_DEVICE) continue;
                    std::wstring to = o.deviceId.empty() ? DefaultEndpointId(true)
                                                         : o.deviceId;
                    if (to.empty() || to != capturing) continue;
                    if (err) *err = FormatW(
                        L"This session captures everything playing on %s and also "
                        L"plays back to it, which would feed its own output "
                        L"through itself and build until it is unusable. Send the "
                        L"output to a different device, or exclude an application "
                        L"so the system audio is captured per application instead.",
                        EndpointNameById(capturing, true).c_str());
                    return false;
                }
            }
            n->capture = MakeSystemCapture(b.deviceId, b.excludeApps, m_rate);
            if (!n->capture->Start(err)) return false;
        }
        else if (b.typeId == BT_INPUT_NET)
        {
            n->capture = MakeNetCapture(b, m_rate);
            if (!n->capture->Start(err)) return false;
        }
        else if (b.typeId == BT_OUT_NET)
        {
            n->net = MakeNetSink();
            if (!n->net->Start(b, m_rate, err)) return false;
        }
        else if (b.typeId == BT_OUTPUT_DEVICE)
        {
            n->renderer.reset(new DeviceRenderer());   // started once the master is known
        }
        else if (b.typeId == BT_INPUT_ASIO)
        {
            n->capture = MakeAsioCapture(b, m_rate);
            if (!n->capture->Start(err)) return false;
        }
        else if (b.typeId == BT_OUT_ASIO)
        {
            n->renderer = MakeAsioRenderer();          // started with the rest
            n->renderer->Configure(b);
        }
        else if (b.Kind() == BlockKind::Effect)
        {
            // Gain is applied by Render() directly, so it has no Effect object.
            n->fx = MakeEffectFor(b.typeId);
            if (n->fx && !n->fx->Start(m_rate, err)) return false;
        }
        else if (b.typeId == BT_OUT_RECORDER)
        {
            // Writing a track per block and no mix means this block has no
            // file of its own: the files hang off the input blocks instead.
            if (b.recMode != RM_TRACKS)
            {
                n->rec.reset(new RecordSink());
                if (!n->rec->Start(RecordSettingsFor(b, s.name), m_rate, err)) return false;
            }
        }
        created[i] = std::move(n);
    }

    // ---- Phase B2: a file per input block, where one is asked for ---------
    //
    // This is the one thing the key-based reuse above cannot work out for
    // itself, because it depends on blocks other than the one being built: an
    // input block's own track exists because of a *recorder* further down the
    // list. So it is settled here, once the arrangement is known, and the
    // sinks are opened before the lock is taken - opening one waits for the
    // file, and the audio thread must not.
    std::vector<std::unique_ptr<RecordSink>> newTracks(count);
    std::vector<std::wstring> wantTrack(count);
    std::vector<const Block*> askedBy(count, nullptr);
    {
        const std::vector<std::wstring> trackName = TrackNamesFor(s.blocks);
        for (size_t i = 0; i < count; ++i)
        {
            if (trackName[i].empty()) continue;

            // The recorder that asked - the first one below this block that
            // wants tracks, which is the same one TrackNamesFor found.
            const Block* asked = nullptr;
            for (size_t j = i + 1; j < count; ++j)
                if (s.blocks[j].typeId == BT_OUT_RECORDER &&
                    s.blocks[j].recMode != RM_MIXED)
                {
                    asked = &s.blocks[j];
                    break;
                }
            if (!asked) continue;
            askedBy[i] = asked;

            wantTrack[i] = KeyFor(*asked) + L"|" + trackName[i];

            Node* live = (reuse[i] != kNone) ? m_nodes[reuse[i]].get() : nullptr;
            if (live && live->trackRec && live->trackKey == wantTrack[i])
                continue;                       // already writing the right file

            std::unique_ptr<RecordSink> sink(new RecordSink());
            if (!sink->Start(TrackSettingsFor(*asked, s.name, trackName[i]),
                             m_rate, err))
                return false;
            newTracks[i] = std::move(sink);
        }
    }

    // ---- Phase C: work out who owns the clock -----------------------------
    if (graphIntact) *graphIntact = false;
    size_t newMaster = kNone;
    for (size_t i = 0; i < count; ++i)
    {
        Node* n = (reuse[i] != kNone) ? m_nodes[reuse[i]].get() : created[i].get();
        if (n->renderer) { newMaster = i; break; }
    }
    // A session whose only output is a recorder has nothing to clock it, so a
    // renderer is opened on the default device purely to drive the graph. It
    // plays silence: with no master index set, nothing writes to its buffer.
    const bool needClock = (newMaster == kNone);

    Node* oldMasterNode = (m_master < m_nodes.size()) ? m_nodes[m_master].get() : nullptr;
    Node* newMasterNode = needClock ? nullptr
                        : (reuse[newMaster] != kNone) ? m_nodes[reuse[newMaster]].get()
                                                      : created[newMaster].get();

    // The master's own thread is the one inside Render(), so it has to be
    // silenced before the graph is touched, and never while m_graph is held.
    if (oldMasterNode && oldMasterNode != newMasterNode)
        oldMasterNode->renderer->Stop();

    // ---- Phase D: bring the secondary outputs into line --------------------
    for (size_t i = 0; i < count; ++i)
    {
        if (!needClock && i == newMaster) continue;
        Node* n = (reuse[i] != kNone) ? m_nodes[reuse[i]].get() : created[i].get();
        if (!n->renderer) continue;

        // Newly added, or demoted from master: (re)start it in ring mode.
        if (reuse[i] == kNone || n == oldMasterNode)
            if (!n->renderer->Start(s.blocks[i].deviceId, m_rate, nullptr, err)) return false;
    }

    // ---- Phase E: swap the graph ------------------------------------------
    std::vector<std::unique_ptr<Node>> retired;
    std::vector<std::unique_ptr<RecordSink>> retiredSinks;
    {
        std::lock_guard<std::mutex> lock(m_graph);

        std::vector<std::unique_ptr<Node>> next(count);
        for (size_t i = 0; i < count; ++i)
        {
            next[i] = (reuse[i] != kNone) ? std::move(m_nodes[reuse[i]])
                                          : std::move(created[i]);
            next[i]->gain.store(PctToGain(s.blocks[i].volumePct));
            next[i]->enabled.store(s.blocks[i].enabled);
            next[i]->mix.store((float)s.blocks[i].mixPct / 100.0f);
            if (next[i]->fx)
            {
                next[i]->fx->Configure(s.blocks[i]);
                // The buffer saves files of its own, and wants the same
                // answer a Recorder block in this session would get - with
                // {block} filled in, since for a buffer there is exactly one
                // block the file is of and it is this one.
                RecordSettings rs = RecordSettingsFor(s.blocks[i], s.name);
                rs.blockName = s.blocks[i].TypeName();
                next[i]->fx->ConfigureRecording(rs);
            }
            if (next[i]->capture) next[i]->capture->Configure(s.blocks[i]);
            // The gate and the splitting are live: they do not decide which
            // file is open, so changing them must not start a new one.
            if (next[i]->rec) next[i]->rec->Configure(RecordSettingsFor(s.blocks[i], s.name));
            if (next[i]->net) next[i]->net->Configure(s.blocks[i]);

            // A track that is no longer wanted, or is wanted in a different
            // file, is moved out to be closed once the lock is released. A
            // new one is a pointer move, which is all this may cost here.
            if (next[i]->trackRec && next[i]->trackKey != wantTrack[i])
                retiredSinks.push_back(std::move(next[i]->trackRec));
            if (newTracks[i])
            {
                next[i]->trackRec = std::move(newTracks[i]);
                next[i]->trackKey = wantTrack[i];
            }
            else if (!next[i]->trackRec)
                next[i]->trackKey.clear();
            // A recorder switched off pauses every file it is writing, its
            // own and the tracks alike.
            next[i]->trackOn.store(askedBy[i] ? askedBy[i]->enabled : true);
        }
        for (std::unique_ptr<Node>& n : m_nodes)
            if (n) retired.push_back(std::move(n));

        m_nodes.swap(next);
        m_master = needClock ? kNone : newMaster;
    }

    // ---- Phase F: start the clock, then retire what is left ---------------
    if (needClock)
    {
        if (!m_clock)
        {
            m_resyncPending.store(true);
            m_clock.reset(new DeviceRenderer());
            DeviceRenderer::SourceFn src =
                [this](float* out, size_t frames) { Render(out, frames); };
            if (!m_clock->Start(L"", m_rate, src, err))
            {
                m_clock.reset();
                if (err && err->empty())
                    *err = L"A recording-only session still needs an output device to "
                           L"run its clock, and the default one could not be opened.";
                return false;
            }
        }
    }
    else if (newMasterNode != oldMasterNode)
    {
        // Opening a WASAPI client is not instant, and every capture started
        // above has been filling its ring the whole time. Left alone that
        // startup backlog would sit in the FIFO for the rest of the session as
        // pure latency. It is flushed on the first pull rather than here,
        // because the client below takes milliseconds more to open and the
        // rings would simply refill over that gap.
        m_resyncPending.store(true);

        // The incoming master may already be running as a secondary output,
        // which is what happens when the blocks are reordered so that a
        // different output comes first. Phase D deliberately leaves it alone,
        // so it is stopped here: a renderer cannot be handed a source while
        // its own thread is inside the old one.
        newMasterNode->renderer->Stop();

        DeviceRenderer::SourceFn src =
            [this](float* out, size_t frames) { Render(out, frames); };
        if (!newMasterNode->renderer->Start(s.blocks[newMaster].deviceId, m_rate, src, err))
            return false;
    }

    // A device output has taken over the clock, so the stand-in is no longer
    // needed. Stopping it here, after the swap, keeps exactly one clock live.
    if (!needClock && m_clock)
    {
        m_clock->Stop();
        m_clock.reset();
    }

    retired.clear();   // destructors stop the streams no longer in the graph
    // Out of the graph, so nothing is writing to them any more. Closing one
    // finalises an encoder, which is why it is not done under the lock.
    for (std::unique_ptr<RecordSink>& r : retiredSinks) if (r) r->Stop();
    retiredSinks.clear();
    return true;
}

// ---------------------------------------------------------------------------

void Engine::Stop()
{
    // Silence the clock owner first, outside the lock: its thread is the one
    // that calls Render(), and joining it while holding m_graph would deadlock.
    if (m_master < m_nodes.size() && m_nodes[m_master] && m_nodes[m_master]->renderer)
        m_nodes[m_master]->renderer->Stop();
    if (m_clock) { m_clock->Stop(); m_clock.reset(); }

    std::vector<std::unique_ptr<Node>> retired;
    {
        std::lock_guard<std::mutex> lock(m_graph);
        retired.swap(m_nodes);
        m_master = kNone;
    }
    for (std::unique_ptr<Node>& n : retired)
    {
        if (!n) continue;
        if (n->rec) n->rec->Stop();          // flush and close the files
        if (n->trackRec) n->trackRec->Stop();
    }
    retired.clear();
    // The nodes are gone, so whatever was holding the ASIO driver has let go
    // of it; this closes it. A driver left open is one no other program can
    // use, and somebody who stops a session expects their interface back.
    AsioReleaseIfIdle();
    m_running = false;
}

bool Engine::PerformAction(int blockIndex, int action)
{
    std::lock_guard<std::mutex> lock(m_graph);
    if (blockIndex < 0 || (size_t)blockIndex >= m_nodes.size()) return false;
    Node* n = m_nodes[(size_t)blockIndex].get();
    if (!n || !n->fx) return false;
    n->fx->DoAction(action);
    return true;
}

void Engine::GetStatus(Status* st)
{
    if (!st) return;
    *st = Status();

    std::lock_guard<std::mutex> lock(m_graph);
    st->rate = m_rate;

    if (m_startTick)
        st->runSeconds = (int)((GetTickCount64() - m_startTick) / 1000);

    int fxFrames = 0;
    for (std::unique_ptr<Node>& n : m_nodes)
    {
        if (n->fx)
        {
            fxFrames += n->fx->LatencyFrames();
            std::wstring line = n->fx->StatusText();
            if (!line.empty()) st->blockStatus.push_back(line);
        }
        if (n->net)
        {
            std::wstring line = n->net->StatusText();
            if (!line.empty()) st->blockStatus.push_back(line);
        }
        if (n->capture && st->following.empty()) st->following = n->capture->TargetName();
        // With several recordings going, the one that has written most is the
        // one worth reporting: it is the recording somebody is waiting on. The
        // file count already adds them all up. A block's own track counts the
        // same as a recorder's mixed file - from the outside they are both
        // just files being written.
        RecordSink* sinks[2] = { n->rec.get(), n->trackRec.get() };
        for (RecordSink* r : sinks)
        {
            if (!r) continue;
            const unsigned long long f = r->RecordedFrames();
            const int secs = m_rate > 0 ? (int)(f / (unsigned long long)m_rate) : 0;
            if (secs > st->recSeconds) st->recSeconds = secs;
            const unsigned long long b = r->FileBytes();
            if (b > st->recBytes) st->recBytes = b;
        }
    }
    st->fxMs = (int)((long long)fxFrames * 1000 / m_rate);

    Renderer* clock = (m_master < m_nodes.size()) ? m_nodes[m_master]->renderer.get()
                                                        : m_clock.get();
    if (!clock) return;

    // What the output endpoint holds, and the deepest capture ring feeding it.
    size_t out = clock->BufferFrames();
    size_t worst = 0;
    unsigned packet = 0;
    for (std::unique_ptr<Node>& n : m_nodes)
        if (n->capture)
        {
            worst = (std::max)(worst, n->capture->Backlog());
            packet = (std::max)(packet, n->capture->PacketFrames());
        }
    st->inPacketFrames = (int)packet;

    for (std::unique_ptr<Node>& n : m_nodes)
    {
        RecordSink* sinks[2] = { n->rec.get(), n->trackRec.get() };
        for (RecordSink* r : sinks)
        {
            if (!r) continue;
            st->recFiles += r->Files();
            if (r->Skipping()) st->recSkipping = true;
        }
    }

    unsigned glitches = 0;
    for (std::unique_ptr<Node>& n : m_nodes)
        if (n->capture)
        {
            glitches += n->capture->Glitches();
            st->framesIn  += n->capture->FramesIn();
            st->framesOut += n->capture->FramesOut();
            double r = n->capture->Ratio();
            if (r > st->inRatio) st->inRatio = r;
        }
    st->glitches = glitches;

    st->outMs = (int)(out * 1000 / m_rate);
    st->inMs  = (int)(worst * 1000 / m_rate);
    st->outFrames = (int)out;
    st->periodFrames = (int)clock->PeriodFrames();
}

void Engine::Render(float* out, size_t frames)
{
    std::lock_guard<std::mutex> lock(m_graph);

    const size_t samples = frames * kChannels;
    if (m_bus.size() < samples)
    {
        m_bus.resize(samples);
        m_tmp.resize(samples);
        m_out.resize(samples);
        m_trk.resize(samples);
    }
    std::fill(m_bus.begin(), m_bus.begin() + samples, 0.0f);
    std::fill(out, out + samples, 0.0f);

    // First pull of a freshly started graph: drop whatever the captures banked
    // while the output client was still opening.
    if (m_resyncPending.exchange(false))
        for (std::unique_ptr<Node>& n : m_nodes)
            if (n->capture) n->capture->Resync();

    for (size_t idx = 0; idx < m_nodes.size(); ++idx)
    {
        Node& n = *m_nodes[idx];
        const float g = n.gain.load();
        const bool  on = n.enabled.load();

        if (n.kind == BlockKind::Input && n.capture)
        {
            // Drain the capture even while bypassed, so re-enabling the block
            // resumes at real time instead of playing back a backlog.
            n.capture->Read(m_tmp.data(), frames);
            if (on)
                for (size_t i = 0; i < samples; ++i) m_bus[i] += m_tmp[i] * g;

            // This block's own file, when a recorder below it asked for one.
            // It is what the block contributes to the mix and nothing else:
            // its own level applied, nothing further down the list, and
            // silence rather than nothing while it is switched off - a track
            // that stopped being written would no longer line up with the
            // others, which is the whole reason for having them separate.
            if (n.trackRec && n.trackOn.load())
            {
                for (size_t i = 0; i < samples; ++i)
                {
                    float v = on ? m_tmp[i] * g : 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    else if (v < -1.0f) v = -1.0f;
                    m_trk[i] = v;
                }
                n.trackRec->Write(m_trk.data(), frames);
            }
        }
        else if (n.kind == BlockKind::Effect)
        {
            if (n.fx)
            {
                // A bypassed effect is run with the mix wound to dry rather
                // than skipped. Skipping would leave its frame pipeline full of
                // stale audio to spit out on re-enable; this way bypass is
                // sample accurate and switching is silent.
                n.fx->SetMix(on ? n.mix.load() : 0.0f);
                n.fx->Process(m_bus.data(), frames);
            }
            else if (on && n.typeId == BT_EFFECT_GAIN)
            {
                for (size_t i = 0; i < samples; ++i) m_bus[i] *= g;
            }
        }
        else if (n.kind == BlockKind::Output && n.rec)
        {
            if (on)
            {
                for (size_t i = 0; i < samples; ++i)
                {
                    float v = m_bus[i] * g;
                    if (v > 1.0f) v = 1.0f;
                    else if (v < -1.0f) v = -1.0f;
                    m_out[i] = v;
                }
                n.rec->Write(m_out.data(), frames);
            }
        }
        else if (n.kind == BlockKind::Output && n.net)
        {
            if (on)
            {
                for (size_t i = 0; i < samples; ++i)
                {
                    float v = m_bus[i] * g;
                    if (v > 1.0f) v = 1.0f;
                    else if (v < -1.0f) v = -1.0f;
                    m_out[i] = v;
                }
                n.net->Write(m_out.data(), frames);
            }
        }
        else if (n.kind == BlockKind::Output && n.renderer)
        {
            // A bypassed output is fed silence rather than skipped, so its
            // endpoint keeps ticking over instead of underrunning.
            float* dst = (idx == m_master) ? out : m_out.data();
            for (size_t i = 0; i < samples; ++i)
            {
                float v = on ? m_bus[i] * g : 0.0f;
                if (v > 1.0f) v = 1.0f;
                else if (v < -1.0f) v = -1.0f;
                dst[i] = v;
            }
            if (idx != m_master) n.renderer->Write(m_out.data(), frames);
        }
    }
}
