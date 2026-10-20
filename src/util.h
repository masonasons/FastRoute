#pragma once
#include <windows.h>
#include <string>
#include <vector>

std::wstring FormatW(const wchar_t* fmt, ...);
std::wstring AppDataDir();               // %APPDATA%\FastRoute, created on demand

// ---------------------------------------------------------------------------
// Where the document lives
//
// Beside the executable, always. FastRoute is portable: copy the folder to a
// memory stick and the sessions, the schedule and the settings go with it,
// rather than staying behind on whichever machine it was plugged into.
//
// The cost is that a copy put somewhere the user cannot write to - Program
// Files, most obviously - has nowhere to save. That is the trade, and saving
// says so plainly when it happens.
// ---------------------------------------------------------------------------
std::wstring ExeDir();       // the folder FastRoute.exe is in
std::wstring ConfigDir();    // where sessions.json is kept
std::wstring HrText(HRESULT hr);
void ErrorBox(HWND owner, const std::wstring& msg);

// Every error the application reports also lands here, so there is one place
// to go back to after a dialog has been dismissed. Help > Error Log shows it.
struct LogEntry
{
    std::wstring when;   // local time, "14:03:22"
    std::wstring text;
};
void LogError(const std::wstring& msg);
std::vector<LogEntry> ErrorLogEntries();
void ClearErrorLog();
void InfoBox(HWND owner, const std::wstring& msg);
std::wstring Trim(const std::wstring& s);
bool IEquals(const std::wstring& a, const std::wstring& b);

// True only for a folder that is there now. A path on a drive that is not
// plugged in is not an error in itself, so callers warn rather than refuse.
bool DirectoryExists(const std::wstring& path);

// Where a Recorder block with this folder setting actually writes. Blank means
// Music\FastRoute, which is the fallback the recorder has always used. The
// folder is only worked out here, never created: the recorder makes it when it
// opens a file, and merely asking where it is should not leave one behind.
//
// Always fully qualified, since a folder can be typed by hand as well as
// browsed for. SHCreateDirectoryExW refuses a relative path outright, so a
// relative folder was never actually created - and an error message that names
// the folder is a good deal more use when it says where the folder is.
std::wstring ResolveRecordFolder(const std::wstring& folder);

// "1:02:03", or "4:07" under an hour. Durations here are read out loud, and
// leading zeroes on an hour that has not happened are just noise.
std::wstring DurationText(int seconds);
// "1.4 MB", "812 KB". Same idea: the unit that makes the number small.
std::wstring SizeText(unsigned long long bytes);
