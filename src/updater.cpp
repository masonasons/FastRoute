#include "updater.h"
#include "version.h"
#include "json.h"
#include "util.h"

#include <winhttp.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Where the builds are
// ---------------------------------------------------------------------------
namespace {

const wchar_t* kHost = L"api.github.com";
const wchar_t* kPath = L"/repos/masonasons/FastRoute/releases/tags/latest";
// GitHub refuses a request without one, so this is not decoration.
const wchar_t* kAgent = L"FastRoute-Updater";

std::wstring TempFile(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

std::wstring UpdateZipPath() { return TempFile(L"FastRoute-update.zip"); }

// ExeDir is util.h's; only the file name is wanted here as well.
std::wstring ExeName()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? s : s.substr(slash + 1);
}

// --- a blocking HTTPS GET --------------------------------------------------
//
// Small and on its own rather than borrowing the stream block's HttpSource:
// that one is built to keep a connection open and hand back audio forever,
// and this wants a whole file and then to stop.
struct Http
{
    HINTERNET session = nullptr, conn = nullptr, req = nullptr;

    ~Http()
    {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }

    bool Open(const std::wstring& host, const std::wstring& path,
              bool json, std::wstring* err)
    {
        session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
        {
            if (err) *err = L"Could not start a network session.";
            return false;
        }
        // Long enough for a slow link, short enough that a dead one does not
        // leave a thread sitting there for the rest of the session.
        WinHttpSetTimeouts(session, 15000, 15000, 15000, 30000);

        conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!conn)
        {
            if (err) *err = FormatW(L"Could not reach %s.", host.c_str());
            return false;
        }

        req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
        if (!req)
        {
            if (err) *err = L"Could not form the request.";
            return false;
        }
        if (json)
            WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json",
                                     (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr))
        {
            if (err) *err = FormatW(L"%s did not answer.", host.c_str());
            return false;
        }

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200)
        {
            if (err) *err = (status == 404)
                ? std::wstring(L"There is no build published yet.")
                : FormatW(L"The server answered %u.", status);
            return false;
        }
        return true;
    }

    unsigned long long Length() const
    {
        DWORD len = 0, size = sizeof(len);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &len, &size,
                                 WINHTTP_NO_HEADER_INDEX))
            return 0;
        return len;
    }

    // Returns false only on a read error; running out of data is the end.
    bool Read(std::vector<char>& chunk, DWORD* got)
    {
        *got = 0;
        return WinHttpReadData(req, chunk.data(), (DWORD)chunk.size(), got) != FALSE;
    }
};

// --- reading the release ---------------------------------------------------

// Pulls "**Version:** 0.6" and "**Commit:** abc123" out of the release body.
// The continuous build writes both, in those shapes, for exactly this.
}  // namespace

std::wstring ReleaseField(const std::wstring& body, const wchar_t* label)
{
    const std::wstring tag = std::wstring(L"**") + label + L":**";
    size_t at = body.find(tag);
    if (at == std::wstring::npos) return std::wstring();
    at += tag.size();
    while (at < body.size() && (body[at] == L' ' || body[at] == L'\t')) ++at;
    size_t end = at;
    while (end < body.size() && body[end] != L'\r' && body[end] != L'\n' &&
           body[end] != L' ')
        ++end;
    return body.substr(at, end - at);
}

// "0.6" against "0.10" the way a person means it, rather than the way strcmp
// does - which would have 0.10 as older than 0.6.
int CompareVersions(const std::wstring& a, const std::wstring& b)
{
    size_t i = 0, j = 0;
    for (;;)
    {
        long x = 0, y = 0;
        bool hasX = false, hasY = false;
        while (i < a.size() && a[i] >= L'0' && a[i] <= L'9')
        { x = x * 10 + (a[i++] - L'0'); hasX = true; }
        while (j < b.size() && b[j] >= L'0' && b[j] <= L'9')
        { y = y * 10 + (b[j++] - L'0'); hasY = true; }

        if (!hasX && !hasY) return 0;
        if (x != y) return x < y ? -1 : 1;

        while (i < a.size() && (a[i] < L'0' || a[i] > L'9')) ++i;
        while (j < b.size() && (b[j] < L'0' || b[j] > L'9')) ++j;
        if (i >= a.size() && j >= b.size()) return 0;
    }
}

std::wstring RunningVersion() { return FASTROUTE_VERSION_W; }

std::wstring RunningCommit()
{
    const std::string c = FASTROUTE_BUILD_COMMIT;
    return std::wstring(c.begin(), c.end());
}

// ---------------------------------------------------------------------------
// Asking what is out there
// ---------------------------------------------------------------------------
UpdateInfo CheckForUpdates()
{
    UpdateInfo info;

    Http http;
    if (!http.Open(kHost, kPath, true, &info.error)) return info;

    std::string body;
    std::vector<char> chunk(16 * 1024);
    for (;;)
    {
        DWORD got = 0;
        if (!http.Read(chunk, &got)) break;
        if (got == 0) break;
        body.append(chunk.data(), got);
        // A release document is a few kilobytes. Far past that means
        // something other than what was asked for.
        if (body.size() > 4u * 1024u * 1024u) break;
    }
    if (body.empty())
    {
        info.error = L"The server sent nothing back.";
        return info;
    }

    const int need = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(),
                                         nullptr, 0);
    std::wstring text((size_t)(need > 0 ? need : 0), L'\0');
    if (need > 0)
        MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), &text[0], need);

    Json doc;
    if (!Json::Parse(text, doc))
    {
        info.error = L"The server's answer could not be read.";
        return info;
    }

    const std::wstring notes = doc.AsString(L"body");
    info.notes = notes;
    info.version = ReleaseField(notes, L"Version");
    info.commit = ReleaseField(notes, L"Commit");

    // The portable zip, which is the only asset there is. Chosen by shape
    // rather than by position so that adding another one later cannot make
    // this pick up the wrong thing.
    const Json* assets = doc.Find(L"assets");
    if (assets && assets->IsArray())
    {
        for (const Json& a : assets->Items())
        {
            const std::wstring name = a.AsString(L"name");
            if (name.size() < 4) continue;
            if (name.substr(name.size() - 4) != L".zip") continue;
            if (name.find(L"macOS") != std::wstring::npos) continue;
            info.zipUrl = a.AsString(L"browser_download_url");
            break;
        }
    }
    if (info.zipUrl.empty())
    {
        info.error = L"That build has nothing in it to download.";
        return info;
    }

    // Which is newer. A commit is exact, and between releases it is the only
    // thing that distinguishes two builds of the same version - so it is used
    // when both ends have one, and the version is the fallback.
    const std::wstring mine = RunningCommit();
    if (!mine.empty() && !info.commit.empty())
    {
        // The release carries the full hash and the build a short one, so the
        // comparison is by prefix, in whichever direction is shorter.
        const size_t n = mine.size() < info.commit.size() ? mine.size()
                                                          : info.commit.size();
        info.available = (_wcsnicmp(mine.c_str(), info.commit.c_str(), n) != 0);
    }
    else if (!info.version.empty())
    {
        info.available = CompareVersions(RunningVersion(), info.version) < 0;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Fetching it
// ---------------------------------------------------------------------------
bool DownloadUpdate(const std::wstring& url, UpdateProgressFn progress,
                    std::wstring* err)
{
    // The asset lives on a different host from the API, and the URL redirects
    // again from there, so the host and path are taken from the URL rather
    // than assumed. WinHttpCrackUrl does the splitting.
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &parts))
    {
        if (err) *err = L"That download address could not be read.";
        return false;
    }

    Http http;
    if (!http.Open(host, path, false, err)) return false;

    const std::wstring to = UpdateZipPath();
    HANDLE file = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (err) *err = FormatW(L"%s could not be written.", to.c_str());
        return false;
    }

    const unsigned long long total = http.Length();
    unsigned long long done = 0;
    std::vector<char> chunk(64 * 1024);
    bool ok = true;

    for (;;)
    {
        DWORD got = 0;
        if (!http.Read(chunk, &got)) { ok = false; break; }
        if (got == 0) break;

        DWORD written = 0;
        if (!WriteFile(file, chunk.data(), got, &written, nullptr) ||
            written != got)
        {
            if (err) *err = L"The download could not be written to disk.";
            ok = false;
            break;
        }
        done += got;
        if (progress && !progress(done, total))
        {
            // Asked to stop. Not a failure, but there is nothing usable.
            ok = false;
            break;
        }
    }

    CloseHandle(file);
    if (!ok)
    {
        DeleteFileW(to.c_str());
        if (err && err->empty()) *err = L"The download did not finish.";
        return false;
    }
    if (total > 0 && done != total)
    {
        DeleteFileW(to.c_str());
        if (err) *err = L"The download was cut short.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Putting it in place
//
// The exe cannot overwrite itself while it is running, so a batch file does it
// once FastRoute has gone: wait, unpack, copy over, tidy up, start the new one
// and delete itself. Nothing in the zip is a settings file, so sessions.json
// and the rest are left where they are.
// ---------------------------------------------------------------------------
std::wstring UpdateScriptFor(unsigned long pid, const std::wstring& exeName,
                             const std::wstring& zip, const std::wstring& tmp,
                             const std::wstring& dir)
{
    // It waits for the process to actually go rather than guessing at how long
    // it takes. It used to pause for two seconds, and that is not a promise
    // anybody can keep: what takes the time is stopping the sessions, and
    // closing a recording means finalising an encoder, which on a long file
    // easily outlasts any fixed pause.
    //
    // Getting it wrong does not fail cleanly either. An exe cannot be
    // overwritten while it is running, so xcopy skips that one file and
    // replaces everything around it - a half-updated install, the new support
    // files beside the old program, and nothing said about it. Then the script
    // deletes the download and starts FastRoute, which finds the old copy
    // still running and hands straight back to it.
    //
    // The wait is on the process id and the image name together. The id alone
    // would be matched against every column tasklist prints, and a memory
    // figure that happens to read like a process id would keep it waiting.
    //
    // Two minutes is the limit, after which it gives up having changed
    // nothing: :giveup skips every destructive line and falls into the one
    // that deletes the script. The download is left where it is, so the next
    // attempt has nothing to fetch again.
    //
    // The restart is guarded on the file being there. `start` on a file that
    // is not puts up an error box, and this runs hidden - so it would be an
    // error nobody can see and nobody can dismiss, with the script waiting on
    // it for good and leaving itself behind in the folder. Found by running
    // the thing: the harness pointed it at an exe that did not exist and a
    // hidden cmd sat there until it was killed.
    return FormatW(
        L"@echo off\r\n"
        L"set /a tries=0\r\n"
        L":wait\r\n"
        L"tasklist /nh /fi \"PID eq %lu\" 2>nul | find /i \"%s\" >nul\r\n"
        L"if errorlevel 1 goto go\r\n"
        L"set /a tries+=1\r\n"
        L"if %%tries%% geq 120 goto giveup\r\n"
        L"timeout /t 1 /nobreak > nul\r\n"
        L"goto wait\r\n"
        L":go\r\n"
        L"powershell -NoProfile -Command \"Expand-Archive -LiteralPath '%s' "
        L"-DestinationPath '%s' -Force\"\r\n"
        L"xcopy /s /y /q \"%s\\*\" \"%s\\\"\r\n"
        L"rmdir /s /q \"%s\"\r\n"
        L"del \"%s\"\r\n"
        L"if exist \"%s\\%s\" start \"\" \"%s\\%s\"\r\n"
        L":giveup\r\n"
        L"del \"%%~f0\"\r\n",
        pid, exeName.c_str(),
        zip.c_str(), tmp.c_str(),
        tmp.c_str(), dir.c_str(),
        tmp.c_str(),
        zip.c_str(),
        dir.c_str(), exeName.c_str(),
        dir.c_str(), exeName.c_str());
}

bool ApplyUpdate(bool mayAsk)
{
    const std::wstring zip = UpdateZipPath();
    if (GetFileAttributesW(zip.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        ErrorBox(g_mainDlg, L"The downloaded update has gone missing, so "
                            L"nothing has been changed.");
        return false;
    }

    // Settled before anything is written, because the script below cannot be
    // called back once it has started. It waits two seconds and then replaces
    // the exe whether or not this process has gone - so asking afterwards, as
    // this used to, meant the question and the overwrite racing each other.
    // Answer no, or take three seconds to read it, and the update went in
    // around a still-running FastRoute: every file replaced except the one
    // that was locked, which is the exe, and the download deleted on the way
    // out. A half-replaced install and nothing to say so.
    //
    // Asked of the main window rather than worked out here. The engine table
    // belongs to that thread and a dialog needs its window, and this runs on a
    // worker thread - the check and the download both block.
    if (g_mainDlg &&
        SendMessageW(g_mainDlg, WM_APP_MAY_EXIT, mayAsk ? 1 : 0, 0) == 0)
        return false;

    const std::wstring dir = ExeDir();
    const std::wstring bat = dir + L"\\update.bat";
    const std::wstring tmp = dir + L"\\update_temp";

    HANDLE f = CreateFileW(bat.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        ErrorBox(g_mainDlg, FormatW(L"%s could not be written, so the update "
                                    L"has not been applied.", bat.c_str()));
        return false;
    }

    const std::wstring script = UpdateScriptFor(
        (unsigned long)GetCurrentProcessId(), ExeName(), zip, tmp, dir);

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
    std::vector<char> utf8((size_t)(bytes > 0 ? bytes : 1), 0);
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1, utf8.data(), bytes,
                            nullptr, nullptr);
    DWORD written = 0;
    // Less one, so the terminating zero does not go into the file.
    WriteFile(f, utf8.data(), (DWORD)(utf8.size() - 1), &written, nullptr);
    CloseHandle(f);

    const HINSTANCE r = ShellExecuteW(nullptr, L"open", bat.c_str(), nullptr,
                                      dir.c_str(), SW_HIDE);
    if (reinterpret_cast<intptr_t>(r) <= 32)
    {
        ErrorBox(g_mainDlg, L"The update script could not be started, so "
                            L"nothing has been changed.");
        return false;
    }

    // And now go, so that the exe can be replaced. The non-zero wParam says
    // the exit has already been agreed to, so the window does not ask about
    // the running sessions a second time.
    PostMessageW(g_mainDlg, WM_CLOSE, 1, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Asking, telling, and doing it on a timer
//
// All of this happens on threads of its own. The check and the download both
// block, and a message box put up from a worker thread is modal only to that
// thread - so an hourly check that finds something does not freeze the window
// somebody is working in while it waits to be answered.
// ---------------------------------------------------------------------------
namespace {

// One check at a time. Two overlapping ones would download the same build
// twice and then race to unpack it.
std::atomic<bool> g_busy{ false };

struct Scheduler
{
    std::mutex lock;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false;
    HWND owner = nullptr;
};
Scheduler g_sched;

// There is no progress window. The zip is about a megabyte, so one would be on
// screen for less time than it takes to read it; what matters more is that
// nothing is put in front of somebody who did not ask for it.
bool DownloadAndApply(HWND parent, const UpdateInfo& info, bool silent)
{
    std::wstring err;
    if (!DownloadUpdate(info.zipUrl, nullptr, &err))
    {
        if (!silent)
            ErrorBox(parent, err.empty()
                ? std::wstring(L"The update could not be downloaded.") : err);
        else
            LogError(FormatW(L"An update to %s could not be downloaded: %s",
                             info.version.c_str(),
                             err.empty() ? L"no reason given." : err.c_str()));
        return false;
    }
    // Quietly means quietly: an update nobody asked for does not get to put a
    // dialog in front of somebody, so when it cannot go ahead it says so in
    // the log and waits for the next check. The download is kept.
    if (!ApplyUpdate(!silent))
    {
        if (silent)
            LogError(FormatW(
                L"An update to %s was downloaded but not installed, because a "
                L"session is running and installing it would stop it. It will "
                L"be installed at the next check once nothing is running, or "
                L"now from Help > Check for Updates.",
                info.version.c_str()));
        return false;
    }
    return true;
}

void RunCheck(HWND parent, bool silent)
{
    const UpdateInfo info = CheckForUpdates();

    if (!info.error.empty())
    {
        // A failed check is not worth a box when nobody asked: a laptop that
        // was asleep, or a network that is not there yet, is the usual cause.
        if (silent) LogError(L"Checking for an update failed: " + info.error);
        else        ErrorBox(parent, info.error);
        g_busy.store(false);
        return;
    }

    if (!info.available)
    {
        if (!silent)
            MessageBoxW(parent,
                        FormatW(L"FastRoute %s is the latest build.",
                                RunningVersion().c_str()).c_str(),
                        L"No update", MB_OK | MB_ICONINFORMATION);
        g_busy.store(false);
        return;
    }

    // Silent means get on with it. The program restarts itself afterwards.
    if (silent)
    {
        DownloadAndApply(parent, info, true);
        g_busy.store(false);
        return;
    }

    std::wstring what = info.version.empty()
        ? std::wstring(L"A newer build")
        : FormatW(L"FastRoute %s", info.version.c_str());
    if (!info.commit.empty() && !info.version.empty())
        what += FormatW(L" (%s)", info.commit.substr(0, 7).c_str());

    const std::wstring ask = FormatW(
        L"%s is available. This copy is %s.\n\n"
        L"Downloading it takes a moment, after which FastRoute closes, "
        L"replaces itself and starts again. Your sessions and settings are "
        L"left alone.\n\nDownload and install it now?",
        what.c_str(), RunningVersion().c_str());

    if (MessageBoxW(parent, ask.c_str(), L"Update available",
                    MB_YESNO | MB_ICONQUESTION) == IDYES)
        DownloadAndApply(parent, info, false);

    g_busy.store(false);
}

void Start(HWND parent, bool silent)
{
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    std::thread([parent, silent] { RunCheck(parent, silent); }).detach();
}

int IntervalMinutes(int setting)
{
    switch (setting)
    {
    case UC_HOURLY: return 60;
    case UC_DAILY:  return 60 * 24;
    default:        return 0;      // never, or startup only
    }
}

void SchedulerThread()
{
    for (;;)
    {
        int minutes = 0;
        HWND owner = nullptr;
        {
            std::unique_lock<std::mutex> g(g_sched.lock);
            if (g_sched.stop) return;
            owner = g_sched.owner;
            minutes = IntervalMinutes(g_store.settings.updateCheck);
            if (minutes <= 0)
            {
                // Nothing to do, but the thread stays so that turning the
                // setting on does not need it restarted.
                g_sched.wake.wait_for(g, std::chrono::minutes(5));
                continue;
            }
            g_sched.wake.wait_for(g, std::chrono::minutes(minutes));
            if (g_sched.stop) return;
            // The setting may have changed while this was asleep.
            if (IntervalMinutes(g_store.settings.updateCheck) <= 0) continue;
        }
        Start(owner, g_store.settings.updateSilent);
    }
}

}  // namespace

void ShowCheckForUpdates(HWND parent, bool silent)
{
    Start(parent ? parent : g_mainDlg, silent);
}

void CheckForUpdatesOnStartup(HWND owner)
{
    // Startup covers the periodic settings too: somebody who asked for hourly
    // wants one when the program opens as well, not fifty-nine minutes later.
    if (g_store.settings.updateCheck == UC_NEVER) return;
    Start(owner, g_store.settings.updateSilent);
}

void StartUpdateChecks(HWND owner)
{
    std::lock_guard<std::mutex> g(g_sched.lock);
    g_sched.owner = owner;
    if (g_sched.thread.joinable()) return;
    g_sched.stop = false;
    g_sched.thread = std::thread(SchedulerThread);
}

void StopUpdateChecks()
{
    {
        std::lock_guard<std::mutex> g(g_sched.lock);
        g_sched.stop = true;
    }
    g_sched.wake.notify_all();
    if (g_sched.thread.joinable()) g_sched.thread.join();
}
