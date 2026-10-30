#pragma once
#include "app.h"

// ---------------------------------------------------------------------------
// Updating itself
//
// FastRoute is portable: a folder with the exe in it and the settings beside
// it, which is also what makes updating simple. The continuous build replaces
// one release, tagged "latest", on every push, and this looks at that release
// and unpacks it over the folder it is running from. Nothing is installed and
// nothing in the registry is touched.
//
// Settings are left alone by that, because they are not in the zip: the only
// things it contains are the exe and the documentation, so sessions.json and
// the rest survive being copied over.
//
// Every call that talks to the network blocks, and none of them may be made
// from the UI thread. ShowCheckForUpdates is the exception - it is the UI
// thread's entry point and starts its own.
// ---------------------------------------------------------------------------

struct UpdateInfo
{
    bool available = false;
    std::wstring version;      // what the release says it is
    std::wstring commit;       // and which commit it was built from
    std::wstring zipUrl;
    std::wstring notes;
    std::wstring error;        // empty unless the check itself failed
};

// What the server has. Blocking; call from a background thread.
UpdateInfo CheckForUpdates();

// Fetches it to a temporary file. The callback is given bytes so far and the
// total, which is zero when the server did not say, and returns false to give
// up. Blocking.
using UpdateProgressFn = std::function<bool(unsigned long long, unsigned long long)>;
bool DownloadUpdate(const std::wstring& url, UpdateProgressFn progress,
                    std::wstring* err);

// Unpacks the downloaded update over this copy and restarts it.
//
// `mayAsk` is whether a question may be put on screen: true for an update
// somebody asked for, false for one installing itself quietly. Either way
// nothing is changed until it is settled that FastRoute can actually exit,
// because the script that replaces the exe cannot be called back once it has
// started.
//
// False when it held off. The download is kept, so the next attempt has
// nothing to fetch again.
bool ApplyUpdate(bool mayAsk);

// The script that does it, as text. One batch file: wait for process `pid`
// (whose image is called `exeName`) to go, unpack `zip` into `tmp`, copy that
// over `dir`, tidy up, start `dir`\\`exeName` again and delete itself.
//
// Separated from ApplyUpdate because this is the part that cannot be tried
// out any other way. ApplyUpdate replaces the running program, so there is no
// watching it work; the script is a string, and a string can be read, and
// pointed somewhere harmless and actually run.
std::wstring UpdateScriptFor(unsigned long pid, const std::wstring& exeName,
                             const std::wstring& zip, const std::wstring& tmp,
                             const std::wstring& dir);

// The UI thread's way in. Checks on a thread of its own and then, on the UI
// thread, either offers the update or says there is none - except when silent,
// which says nothing unless there is something to say.
void ShowCheckForUpdates(HWND parent, bool silent);

// Starts and stops the timer that honours the update setting. Called when the
// program starts and whenever the settings change.
void StartUpdateChecks(HWND owner);
void StopUpdateChecks();

// Runs the check the settings ask for on startup, if they ask for one.
void CheckForUpdatesOnStartup(HWND owner);

// The two decisions, exposed so they can be checked without a network.
//
// ReleaseField pulls "**Version:** 0.6" and "**Commit:** abc" out of a release
// body, in the shapes the build workflow writes them. CompareVersions orders
// two version strings the way a person reads them, so 0.10 is newer than 0.6
// rather than older as a plain string comparison would have it. Negative when
// the first is older, zero when they match, positive when it is newer.
std::wstring ReleaseField(const std::wstring& body, const wchar_t* label);
int CompareVersions(const std::wstring& a, const std::wstring& b);

// What this build is, for the About dialog and for comparing against a
// release. The commit is empty when the build did not know its own.
std::wstring RunningVersion();
std::wstring RunningCommit();
