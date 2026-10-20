#include "util.h"
#include <objbase.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <mutex>
#include <vector>

std::wstring FormatW(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    va_start(ap, fmt);
    vswprintf(&out[0], (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

std::wstring AppDataDir()
{
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)))
    {
        dir = base;
        CoTaskMemFree(base);
        dir += L"\\FastRoute";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

// ---------------------------------------------------------------------------

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring out = path;
    size_t slash = out.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"";
    return out.substr(0, slash);
}

std::wstring ConfigDir()
{
    std::wstring dir = ExeDir();
    // Only if there is genuinely no answer - which would mean the module path
    // could not be read at all - is there anywhere else to go.
    if (dir.empty()) return AppDataDir();
    return dir;
}

std::wstring DurationText(int seconds)
{
    if (seconds < 0) seconds = 0;
    const int h = seconds / 3600;
    const int m = (seconds / 60) % 60;
    const int s = seconds % 60;
    if (h > 0) return FormatW(L"%d:%02d:%02d", h, m, s);
    return FormatW(L"%d:%02d", m, s);
}

std::wstring SizeText(unsigned long long bytes)
{
    if (bytes >= 1024ull * 1024ull * 1024ull)
        return FormatW(L"%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    if (bytes >= 1024ull * 1024ull)
        return FormatW(L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    if (bytes >= 1024ull)
        return FormatW(L"%llu KB", bytes / 1024ull);
    return FormatW(L"%llu bytes", bytes);
}

std::wstring HrText(HRESULT hr)
{
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, (DWORD)hr, 0, (wchar_t*)&msg, 0, nullptr);
    std::wstring out = FormatW(L"0x%08X", (unsigned)hr);
    if (n && msg)
    {
        std::wstring m = Trim(msg);
        if (!m.empty()) out += L": " + m;
    }
    if (msg) LocalFree(msg);
    return out;
}

// ---------------------------------------------------------------------------
// Error log
//
// Errors are shown once and then gone, which is no use when one arrives while
// you are reading something else, or when a session fails to start before you
// have even reached the window. Everything ErrorBox reports is kept here too.
// ---------------------------------------------------------------------------

static std::mutex g_logLock;
static std::vector<LogEntry> g_log;
static const size_t kMaxLogEntries = 200;

void LogError(const std::wstring& msg)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);

    std::lock_guard<std::mutex> lock(g_logLock);
    LogEntry e;
    e.when = FormatW(L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    e.text = msg;
    g_log.push_back(e);
    // Bounded, so an error storm cannot grow without limit. The oldest go
    // first: the recent ones are what you came looking for.
    if (g_log.size() > kMaxLogEntries)
        g_log.erase(g_log.begin(), g_log.begin() + (g_log.size() - kMaxLogEntries));
}

std::vector<LogEntry> ErrorLogEntries()
{
    std::lock_guard<std::mutex> lock(g_logLock);
    return g_log;
}

void ClearErrorLog()
{
    std::lock_guard<std::mutex> lock(g_logLock);
    g_log.clear();
}

void ErrorBox(HWND owner, const std::wstring& msg)
{
    LogError(msg);
    MessageBoxW(owner, msg.c_str(), L"FastRoute", MB_OK | MB_ICONERROR);
}

void InfoBox(HWND owner, const std::wstring& msg)
{
    MessageBoxW(owner, msg.c_str(), L"FastRoute", MB_OK | MB_ICONINFORMATION);
}

std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool IEquals(const std::wstring& a, const std::wstring& b)
{
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring ResolveRecordFolder(const std::wstring& folder)
{
    std::wstring dir = folder;
    if (dir.empty())
    {
        wchar_t* music = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &music)) && music)
        {
            dir = music;
            CoTaskMemFree(music);
            dir += L"\\FastRoute";
        }
    }
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    if (dir.empty()) return dir;        // nowhere, rather than here

    // Fully qualified, whatever was typed. A folder can be entered by hand as
    // well as browsed for, and "Recordings" is a perfectly reasonable thing to
    // type into a program that keeps everything beside its executable.
    //
    // It has to be made absolute here because SHCreateDirectoryExW - which is
    // what actually creates the folder - refuses a relative path outright with
    // ERROR_BAD_PATHNAME, including ".\\like\\this". Nothing noticed for a long
    // time, because a relative folder that already existed worked fine and the
    // one that was never created was the one nobody had asked for yet. A
    // template that asks for a subfolder asks for a new folder every time, so
    // it turned a silent no-op into a recording that would not start.
    //
    // Resolved against the working directory, which is what matters: that is
    // where fopen will look for the same relative path, so the folder created
    // and the folder written into are the same folder.
    const DWORD need = GetFullPathNameW(dir.c_str(), 0, nullptr, nullptr);
    if (need == 0) return dir;          // no better answer than what we have

    std::vector<wchar_t> full(need + 1, 0);
    const DWORD got = GetFullPathNameW(dir.c_str(), need + 1, full.data(), nullptr);
    if (got == 0 || got > need) return dir;

    std::wstring out(full.data(), got);
    if (!out.empty() && out.back() == L'\\') out.pop_back();
    return out;
}

bool DirectoryExists(const std::wstring& path)
{
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
