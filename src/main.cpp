#include "app.h"
#include "updater.h"
#include "audio/asiodev.h"
#include <map>
#include <memory>

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

HINSTANCE g_inst = nullptr;
HWND      g_mainDlg = nullptr;

// ---------------------------------------------------------------------------
// Running engines, keyed by the stable session id
// ---------------------------------------------------------------------------

static std::map<int, std::unique_ptr<Engine>> g_engines;

bool SessionRunning(int sessionId)
{
    auto it = g_engines.find(sessionId);
    return it != g_engines.end() && it->second->Running();
}

static bool StartSessionInto(int sessionId, std::wstring* err)
{
    Session* s = g_store.ById(sessionId);
    if (!s)
    {
        if (err) *err = L"That session no longer exists.";
        return false;
    }

    StopSession(sessionId);

    std::unique_ptr<Engine> e(new Engine());
    if (!e->Start(*s, err)) return false;

    g_engines[sessionId] = std::move(e);
    NotifySessionStateChanged(sessionId);
    return true;
}

bool StartSession(HWND owner, int sessionId)
{
    std::wstring err;
    if (StartSessionInto(sessionId, &err)) return true;
    ErrorBox(owner, err.empty() ? L"The session could not be started." : err);
    return false;
}

bool StartSessionSilently(int sessionId, std::wstring* err)
{
    return StartSessionInto(sessionId, err);
}

void StopSession(int sessionId)
{
    auto it = g_engines.find(sessionId);
    if (it == g_engines.end()) return;
    it->second->Stop();
    g_engines.erase(it);
    NotifySessionStateChanged(sessionId);
}

void StopAllSessions()
{
    for (auto& kv : g_engines) kv.second->Stop();
    g_engines.clear();
}

bool SessionStatus(int sessionId, Engine::Status* st)
{
    auto it = g_engines.find(sessionId);
    if (it == g_engines.end()) return false;
    it->second->GetStatus(st);
    return true;
}

void ApplyLiveEditQuiet(int sessionId)
{
    auto it = g_engines.find(sessionId);
    Session* s = g_store.ById(sessionId);
    if (it == g_engines.end() || !s) return;

    std::wstring err;
    bool intact = true;
    if (it->second->ApplySession(*s, &err, &intact)) return;

    if (!intact) StopSession(sessionId);
    LogError(err.empty() ? L"A change could not be applied to a running session."
                         : err);
}

bool PerformBlockAction(int sessionId, int blockIndex, int action)
{
    auto it = g_engines.find(sessionId);
    if (it == g_engines.end()) return false;
    return it->second->PerformAction(blockIndex, action);
}

bool ApplyLiveEdit(HWND owner, int sessionId)
{
    auto it = g_engines.find(sessionId);
    Session* s = g_store.ById(sessionId);
    if (it == g_engines.end() || !s) return true;      // not running, nothing to push

    std::wstring err;
    bool intact = true;
    if (it->second->ApplySession(*s, &err, &intact)) return true;

    // If the rebuild failed before touching anything, the session is still
    // playing the previous graph; otherwise it is in no fit state to continue.
    if (!intact) StopSession(sessionId);
    ErrorBox(owner, err.empty() ? L"The change could not be applied." : err);
    return false;
}

void RunStartupSessions()
{
    for (const Session& s : g_store.sessions)
    {
        if (!s.autoRun) continue;

        std::unique_ptr<Engine> e(new Engine());
        std::wstring err;
        if (!e->Start(s, &err))
        {
            // No box: FastRoute can easily come up before the application a
            // session captures, and a dialog to dismiss at every login is the
            // wrong way to say so. It goes to the log, under Help.
            LogError(FormatW(L"%s is set to run at startup but did not: %s",
                             s.name.c_str(),
                             err.empty() ? L"it could not be started." : err.c_str()));
            continue;
        }
        g_engines[s.id] = std::move(e);
    }
    RefreshMainList();
    // These started without going through NotifySessionStateChanged, and a
    // block action's hotkey is only registered while its session runs - so
    // without this a session that runs at startup would have none of its
    // action keys until it was next stopped and started.
    if (g_mainDlg) RefreshItemHotkeys(g_mainDlg);
}

// ---------------------------------------------------------------------------
// One instance at a time
//
// Two FastRoutes on one machine fight over everything that matters. Both read
// sessions.json when they start and both write it when they exit, so whichever
// exits last wins and the other's edits are gone. A global hotkey can only be
// registered once, so the second copy's silently do nothing. And two copies of
// one session open the same endpoints twice, which for an application block
// means capturing the same audio twice into two different places.
//
// So the second one hands over rather than refusing: it asks the copy already
// running to show itself and exits without a word. That is what somebody who
// has just launched FastRoute again wanted anyway - the usual reason for doing
// it is that the window was hidden to the notification area.
// ---------------------------------------------------------------------------

UINT ShowRequestMessage()
{
    // Registered once per process and the same number in every process that
    // asks for this string, which is what makes it safe to broadcast.
    static const UINT m = RegisterWindowMessageW(L"FastRoute.ShowWindow");
    return m;
}

static HANDLE g_soleInstance = nullptr;

// True when this process is the one that should carry on.
static bool ClaimSoleInstance()
{
    // Local rather than Global, so the scope is one logged-in user. Two people
    // signed into the same machine have their own settings and their own audio
    // devices, and neither is the other's second instance.
    g_soleInstance = CreateMutexW(nullptr, TRUE, L"Local\\FastRoute.SingleInstance");
    if (g_soleInstance && GetLastError() != ERROR_ALREADY_EXISTS) return true;

    if (g_soleInstance) { CloseHandle(g_soleInstance); g_soleInstance = nullptr; }

    // The running copy is about to call SetForegroundWindow, which Windows
    // only grants to the process already in front - which, for this moment, is
    // this one. Handing that right over is what this call is for.
    AllowSetForegroundWindow(ASFW_ANY);

    // Broadcast rather than sent to a window looked up by name: the main
    // window is a dialog and has no class of its own to find it by, and a
    // broadcast reaches it whether it is hidden or not.
    PostMessageW(HWND_BROADCAST, ShowRequestMessage(), 0, 0);
    return false;
}

// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;

    // Before anything is loaded or opened, and before COM: there is nothing to
    // undo if this copy is not the one that carries on.
    if (!ClaimSoleInstance()) return 0;

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCom))
    {
        ErrorBox(nullptr, L"COM could not be initialised (" + HrText(hrCom) + L").");
        return 1;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES |
                ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_store.Load();

    g_mainDlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr,
                                   MainDlgProc, 0);
    if (!g_mainDlg)
    {
        ErrorBox(nullptr, L"The main window could not be created.");
        CoUninitialize();
        return 1;
    }
    // Hidden from the start, when the settings ask for it: the window is never
    // shown at all rather than shown and then taken away, which would flash on
    // screen and, worse, take the focus from whatever the user was doing.
    //
    // There is a way back even if the hotkey is off and the notification area
    // icon will not go in: launching FastRoute again asks the copy already
    // running to show itself rather than starting beside it.
    if (g_store.settings.startHidden) HideToTray(g_mainDlg);
    else                              ShowWindow(g_mainDlg, SW_SHOW);

    // An ASIO driver is handed a window when it starts, and some of them use
    // it. Set before any session can open one.
    SetAsioHostWindow(g_mainDlg);

    RunStartupSessions();

    // Looking for a new build, if the settings ask for one. Both of these are
    // no-ops when they are set to never, and both do their waiting on threads
    // of their own.
    CheckForUpdatesOnStartup(g_mainDlg);
    StartUpdateChecks(g_mainDlg);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        // Window scoped hotkeys go first: they are only read while one of our
        // own windows is in front, and the dialog manager would otherwise
        // swallow the key before anything had a chance to look at it.
        if (GetActiveWindow() && HandleWindowHotkey(msg)) continue;

        HWND active = GetActiveWindow();
        if (active)
        {
            HACCEL accel = AccelForWindow(active);
            if (accel && TranslateAcceleratorW(active, accel, &msg)) continue;
            if (IsDialogMessageW(active, &msg)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopUpdateChecks();
    StopAllSessions();
    CoUninitialize();
    // Last, so that a FastRoute started while this one is shutting down cannot
    // come up and read sessions.json while WM_CLOSE is still writing it.
    if (g_soleInstance) { CloseHandle(g_soleInstance); g_soleInstance = nullptr; }
    return (int)msg.wParam;
}

void NotifySessionStateChanged(int sessionId)
{
    RefreshMainList();
    RefreshSessionWindow(sessionId);
    // A block action's hotkey is only registered while its session runs, so
    // the table has to be rebuilt when that changes. Guarded on the window
    // because the registrations are owned by it: with no owner there is
    // nothing to unregister them from, and clearing the table would leave
    // them held with nothing pointing at them.
    if (g_mainDlg) RefreshItemHotkeys(g_mainDlg);
}
