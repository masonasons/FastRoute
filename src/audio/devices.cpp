#include "audio.h"
#include "util.h"

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <algorithm>

// ---------------------------------------------------------------------------

static std::wstring GetEndpointFriendlyName(IMMDevice* dev)
{
    std::wstring name;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props)
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            name = pv.pwszVal;
        PropVariantClear(&pv);
        props->Release();
    }
    return name;
}

std::vector<AudioEndpoint> EnumEndpoints(bool render)
{
    std::vector<AudioEndpoint> out;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en))) || !en)
        return out;

    EDataFlow flow = render ? eRender : eCapture;

    std::wstring defaultId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eMultimedia, &def)) && def)
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
        def->Release();
    }

    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)) && coll)
    {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;

            AudioEndpoint ep;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) { ep.id = id; CoTaskMemFree(id); }
            ep.name = GetEndpointFriendlyName(dev);
            ep.isDefault = (!ep.id.empty() && ep.id == defaultId);
            if (!ep.id.empty()) out.push_back(ep);

            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    return out;
}

HRESULT OpenEndpoint(const std::wstring& id, bool render, IMMDevice** out)
{
    *out = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&en));
    if (FAILED(hr)) return hr;

    if (id.empty())
        hr = en->GetDefaultAudioEndpoint(render ? eRender : eCapture, eMultimedia, out);
    else
        hr = en->GetDevice(id.c_str(), out);

    en->Release();
    return hr;
}

bool QueryEndpointFormat(const std::wstring& id, bool render, int* rate, int* channels)
{
    IMMDevice* dev = nullptr;
    if (FAILED(OpenEndpoint(id, render, &dev)) || !dev) return false;

    bool ok = false;
    IAudioClient* client = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                (void**)&client)) && client)
    {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix)
        {
            if (rate)     *rate = (int)mix->nSamplesPerSec;
            if (channels) *channels = (int)mix->nChannels;
            ok = true;
            CoTaskMemFree(mix);
        }
        client->Release();
    }
    dev->Release();
    return ok;
}

std::wstring EndpointNameById(const std::wstring& id, bool render)
{
    if (id.empty()) return L"System default";
    for (const AudioEndpoint& ep : EnumEndpoints(render))
        if (ep.id == id) return ep.name;
    return L"";
}

// ---------------------------------------------------------------------------

static std::wstring ExeNameForPid(DWORD pid)
{
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h)
    {
        wchar_t path[MAX_PATH] = { 0 };
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n))
            name = PathFindFileNameW(path);
        CloseHandle(h);
    }
    return name;
}

// The name of one pid, out of a snapshot already taken.
static std::wstring NameOfPid(const std::vector<ProcessEntry>& all, unsigned long pid)
{
    for (const ProcessEntry& e : all) if (e.pid == pid) return e.exeName;
    return std::wstring();
}

// Is any process above `pid` called `exeName`? That is what makes `pid` a
// helper inside somebody else's application rather than the application.
static bool HasAncestorNamed(const std::vector<ProcessEntry>& all,
                             unsigned long pid, const std::wstring& exeName)
{
    unsigned long cur = pid;
    // The same hop limit SelfAndAbove uses: a snapshot taken while processes
    // are starting and stopping can describe a loop that never existed, and
    // walking it forever would hang the session start.
    for (int hop = 0; hop < 64 && cur; ++hop)
    {
        unsigned long parent = 0;
        for (const ProcessEntry& e : all) if (e.pid == cur) { parent = e.parent; break; }
        if (!parent) return false;
        if (IEquals(NameOfPid(all, parent), exeName)) return true;
        cur = parent;
    }
    return false;
}

// Is `pid` the root of a tree, or somewhere inside it?
static bool InTreeOf(const std::vector<ProcessEntry>& all, unsigned long root,
                     unsigned long pid)
{
    if (root == pid) return true;
    unsigned long cur = pid;
    for (int hop = 0; hop < 64 && cur; ++hop)
    {
        unsigned long parent = 0;
        for (const ProcessEntry& e : all) if (e.pid == cur) { parent = e.parent; break; }
        if (!parent) return false;
        if (parent == root) return true;
        cur = parent;
    }
    return false;
}

// Which process a capture of a named program should be pointed at.
//
// Process loopback takes the target and its whole tree, so for a program that
// is several processes - which every Chromium application is, and that covers
// Discord, Chrome, Edge, Teams, Slack and most of the rest - the one to point
// at is the topmost one. Point at a child instead and you capture that child
// alone, its own tree being just itself, and most of those children never play
// a sound.
//
// Discord was the report, and it is worth writing down because it breaks both
// of the obvious answers at once. Six processes all called Discord.exe: the
// browser process on top, with a renderer, a GPU process, a network service, a
// crash handler and an audio service under it.
//
//   * "the first one found" picked a renderer, because the browser process had
//     the HIGHEST process id of the six - a snapshot is in no useful order, and
//     Chromium starts its children after itself.
//   * "whichever owns an audio session" picked the audio service, which is one
//     of two Discord processes holding a session, is torn down and restarted
//     under Chromium's own rules, and does not exist at all while Discord is
//     quiet - which is how a chat program usually is when you start a session.
//
// So: the root. Among the processes of this name, the ones with no ancestor of
// the same name are the real instances and the rest are helpers inside them.
// One root is the answer. Several means separate copies of the program really
// are running, and then the one with audio anywhere in its tree is the one
// meant - asked of the tree, not of the process, because the audio usually is
// coming out of a helper.
unsigned long SelectAppCaptureTarget(const std::vector<ProcessEntry>& tree,
                                     const std::vector<AudioProcess>& audio,
                                     const std::wstring& exeName)
{
    if (exeName.empty()) return 0;

    std::vector<unsigned long> mine;
    for (const ProcessEntry& e : tree)
        if (IEquals(e.exeName, exeName)) mine.push_back(e.pid);
    if (mine.empty()) return 0;

    std::vector<unsigned long> roots;
    for (unsigned long pid : mine)
        if (!HasAncestorNamed(tree, pid, exeName)) roots.push_back(pid);

    // Every one of them being inside another is not a shape Windows produces,
    // but a snapshot that disagrees with itself could describe it. Falling back
    // to the whole set keeps an answer rather than none at all.
    if (roots.empty()) roots = mine;

    if (roots.size() == 1) return roots[0];

    for (const AudioProcess& ap : audio)
        for (unsigned long root : roots)
            if (InTreeOf(tree, root, ap.pid)) return root;

    // Nothing playing anywhere, so there is nothing to choose on. The lowest
    // id is a stable answer rather than whichever the snapshot listed first.
    return *std::min_element(roots.begin(), roots.end());
}

unsigned long FindProcessIdByName(const std::wstring& exeName)
{
    if (exeName.empty()) return 0;
    return SelectAppCaptureTarget(EnumProcessTree(), EnumAudioProcesses(), exeName);
}

std::vector<ProcessEntry> EnumProcessTree()
{
    std::vector<ProcessEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
        do {
            ProcessEntry e;
            e.pid = pe.th32ProcessID;
            e.parent = pe.th32ParentProcessID;
            e.exeName = pe.szExeFile;
            out.push_back(e);
        } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return out;
}

std::wstring DefaultEndpointId(bool render)
{
    IMMDevice* dev = nullptr;
    if (FAILED(OpenEndpoint(L"", render, &dev)) || !dev) return L"";

    std::wstring id;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(dev->GetId(&raw)) && raw)
    {
        id = raw;
        CoTaskMemFree(raw);
    }
    dev->Release();
    return id;
}

bool WouldCaptureSelf(unsigned long pid)
{
    DWORD self = GetCurrentProcessId();
    if (pid == self) return true;

    // Walk our own ancestry: if the target is anywhere above us, a tree
    // capture takes in this process too, and the session captures its own
    // output back into itself.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    std::vector<std::pair<DWORD, DWORD>> parents;   // pid -> parent pid
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
        do { parents.push_back({ pe.th32ProcessID, pe.th32ParentProcessID }); }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);

    DWORD cur = self;
    for (int hop = 0; hop < 64 && cur != 0; ++hop)
    {
        DWORD parent = 0;
        for (const auto& e : parents) if (e.first == cur) { parent = e.second; break; }
        if (parent == 0) break;
        if (parent == pid) return true;
        cur = parent;
    }
    return false;
}

// The sessions on one endpoint, appended to `out`. `skipExpired` drops the
// sessions Windows keeps around after a process has stopped using the device -
// which is how an application that has moved to another endpoint leaves its
// mark on the one it left.
static void AppendSessionsOn(IMMDevice* dev, bool skipExpired,
                             std::vector<AudioProcess>& out)
{
    if (!dev) return;

    IAudioSessionManager2* mgr = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                             nullptr, (void**)&mgr)) || !mgr)
        return;

    IAudioSessionEnumerator* se = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se)
    {
        int count = 0;
        se->GetCount(&count);
        for (int i = 0; i < count; ++i)
        {
            IAudioSessionControl* ctl = nullptr;
            if (FAILED(se->GetSession(i, &ctl)) || !ctl) continue;

            // Inactive is kept and expired is not: a paused player is still on
            // this device and should come back when it resumes, whereas an
            // expired session is one the process has finished with.
            bool skip = false;
            if (skipExpired)
            {
                AudioSessionState st = AudioSessionStateInactive;
                if (SUCCEEDED(ctl->GetState(&st)) && st == AudioSessionStateExpired)
                    skip = true;
            }

            IAudioSessionControl2* ctl2 = nullptr;
            if (!skip && SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&ctl2))) && ctl2)
            {
                DWORD pid = 0;
                if (SUCCEEDED(ctl2->GetProcessId(&pid)) && pid != 0 &&
                    ctl2->IsSystemSoundsSession() != S_OK)
                {
                    AudioProcess ap;
                    ap.pid = pid;
                    ap.exeName = ExeNameForPid(pid);

                    LPWSTR disp = nullptr;
                    if (SUCCEEDED(ctl2->GetDisplayName(&disp)) && disp)
                    {
                        ap.displayName = disp;
                        CoTaskMemFree(disp);
                    }

                    bool dup = false;
                    for (const AudioProcess& e : out)
                        if (e.pid == ap.pid) dup = true;
                    if (!dup && !ap.exeName.empty()) out.push_back(ap);
                }
                ctl2->Release();
            }
            ctl->Release();
        }
        se->Release();
    }
    mgr->Release();
}

std::vector<AudioProcess> EnumAudioProcesses()
{
    std::vector<AudioProcess> out;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en))) || !en)
        return out;

    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll)
    {
        UINT devCount = 0;
        coll->GetCount(&devCount);
        for (UINT d = 0; d < devCount; ++d)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(d, &dev)) || !dev) continue;
            AppendSessionsOn(dev, false, out);
            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    return out;
}

std::vector<AudioProcess> EnumAudioProcessesOn(const std::wstring& endpointId)
{
    std::vector<AudioProcess> out;
    IMMDevice* dev = nullptr;
    if (FAILED(OpenEndpoint(endpointId, true, &dev)) || !dev) return out;
    AppendSessionsOn(dev, true, out);
    dev->Release();
    return out;
}

// ---------------------------------------------------------------------------
// What an application block offers to capture
// ---------------------------------------------------------------------------

// This process and everything it was started from, worked out once from a
// snapshot that has already been taken. WouldCaptureSelf answers the same
// question, but it takes a snapshot of its own each time it is asked, and here
// it would be asked once per process on the machine.
static std::vector<unsigned long> SelfAndAbove(const std::vector<ProcessEntry>& all)
{
    std::vector<unsigned long> out;
    unsigned long cur = (unsigned long)GetCurrentProcessId();
    for (int hop = 0; hop < 64 && cur; ++hop)
    {
        out.push_back(cur);
        unsigned long parent = 0;
        for (const ProcessEntry& e : all) if (e.pid == cur) { parent = e.parent; break; }
        if (!parent) break;
        cur = parent;
    }
    return out;
}

// Each group is put in order within itself, so arrowing down a long list is
// predictable, while the groups stay in the order that puts the likeliest
// answer at the top.
static void SortRange(std::vector<AppCandidate>& v, size_t from)
{
    std::sort(v.begin() + (ptrdiff_t)from, v.end(),
              [](const AppCandidate& a, const AppCandidate& b)
              { return _wcsicmp(a.exeName.c_str(), b.exeName.c_str()) < 0; });
}

static bool Contains(const std::vector<unsigned long>& v, unsigned long pid)
{
    for (unsigned long x : v) if (x == pid) return true;
    return false;
}

// One row per program, so a second instance of something already listed adds
// what it knows rather than a duplicate row: the copy with a window supplies
// the title, the copy with an audio session supplies "playing".
static void AddCandidate(std::vector<AppCandidate>& out, const std::wstring& exe,
                         const std::wstring& display, bool playing, bool running)
{
    if (exe.empty()) return;
    for (AppCandidate& c : out)
        if (IEquals(c.exeName, exe))
        {
            c.playing = c.playing || playing;
            c.running = c.running || running;
            if (c.displayName.empty()) c.displayName = display;
            return;
        }

    AppCandidate c;
    c.exeName = exe;
    c.displayName = display;
    c.playing = playing;
    c.running = running;
    out.push_back(c);
}

std::vector<AppCandidate> AppCandidates(const std::vector<std::wstring>& ticked,
                                        bool everything)
{
    std::vector<AppCandidate> out;
    const std::vector<ProcessEntry> all = EnumProcessTree();
    const std::vector<unsigned long> ours = SelfAndAbove(all);

    // What is playing, which is what someone looking for a program they can
    // hear will look for first.
    for (const AudioProcess& p : EnumAudioProcesses())
    {
        if (Contains(ours, p.pid)) continue;
        AddCandidate(out, p.exeName, p.displayName, true, true);
    }
    SortRange(out, 0);

    // Then what is on screen. EnumWindowedApps has left us out already.
    size_t mark = out.size();
    for (const AudioProcess& p : EnumWindowedApps())
        AddCandidate(out, p.exeName, p.displayName, false, true);
    SortRange(out, mark);

    if (everything)
    {
        mark = out.size();
        for (const ProcessEntry& e : all)
        {
            if (Contains(ours, e.pid)) continue;
            AddCandidate(out, e.exeName, std::wstring(), false, true);
        }
        SortRange(out, mark);
    }

    // And last the block's own choices, whether or not they are anywhere to be
    // found. One that is running without a window or a sound is still running,
    // and saying so is the difference between "wait for it" and "check the
    // name".
    for (const std::wstring& t : ticked)
    {
        bool runs = false;
        for (const ProcessEntry& e : all) if (IEquals(e.exeName, t)) runs = true;
        AddCandidate(out, t, std::wstring(), false, runs);
    }

    return out;
}
