#include "app.h"
#include "updater.h"
#include <map>
#include <shlobj.h>        // the file dialogs, for exporting and importing
#include <shobjidl.h>
#include <vector>

static const UINT_PTR kSchedulerTimer = 1;
static const UINT_PTR kRunningTimer = 2;

static HACCEL g_mainAccel = nullptr;

// Which of the two pages is showing. The lists sit on top of one another in
// the tab's area and take turns being visible, which is a great deal simpler
// than a child dialog per page and reads the same to a screen reader: the tab
// control, then the list that belongs to it.
enum { kTabSessions = 0, kTabSchedule = 1 };
static int g_tab = kTabSessions;

static void ShowTab(HWND dlg, int tab)
{
    g_tab = tab;
    const bool sessions = (tab == kTabSessions);
    HWND had = GetFocus();

    ShowWindow(GetDlgItem(dlg, IDC_SESSIONS_LABEL), sessions ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_SESSION_LIST), sessions ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_SCHED_LABEL), sessions ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(dlg, IDC_SCHED_LIST), sessions ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(dlg, IDC_MAIN_RUNNING_LABEL), sessions ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_MAIN_RUNNING), sessions ? SW_SHOW : SW_HIDE);

    // Changing tab leaves the focus where it was. Someone moving along the
    // tabs with the arrow keys is still moving along the tabs afterwards;
    // being dropped into the list means the next arrow press does something
    // else entirely, and there is no way back but Shift+Tab.
    //
    // The one exception is the focus being hidden out from under itself,
    // which would leave the dialog with no focus at all - then it goes to the
    // tab control, which is where the change came from.
    if (had && !IsWindowVisible(had)) SetFocus(GetDlgItem(dlg, IDC_MAIN_TABS));
}

// ---------------------------------------------------------------------------

static int SelectedIndex(HWND list)
{
    return (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void SelectIndex(HWND list, int i)
{
    int count = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    if (count == 0) return;
    if (i < 0) i = 0;
    if (i >= count) i = count - 1;

    LVITEMW it{};
    it.mask = LVIF_STATE;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    SendMessageW(list, LVM_ENSUREVISIBLE, (WPARAM)i, FALSE);
}

static void AddColumn(HWND list, int index, const wchar_t* title, int width)
{
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = width;
    col.iSubItem = index;
    SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)index, (LPARAM)&col);
}

static void SetItemText(HWND list, int item, int sub, const std::wstring& text)
{
    LVITEMW it{};
    it.iSubItem = sub;
    it.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)item, (LPARAM)&it);
}

// ---------------------------------------------------------------------------

void RefreshScheduleList()
{
    if (!g_mainDlg || !IsWindow(g_mainDlg)) return;
    HWND list = GetDlgItem(g_mainDlg, IDC_SCHED_LIST);

    int keep = SelectedIndex(list);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);

    for (size_t i = 0; i < g_store.events.size(); ++i)
    {
        const ScheduleEvent& e = g_store.events[i];
        Session* s = g_store.ById(e.sessionId);

        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        std::wstring who = s ? s->name : std::wstring(L"(deleted session)");
        it.pszText = const_cast<wchar_t*>(who.c_str());
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);

        SetItemText(list, (int)i, 1, ScheduleWhenText(e));
        SetItemText(list, (int)i, 2, ScheduleStopText(e));
        SetItemText(list, (int)i, 3, e.singleUse ? L"Once only" : L"");
        SetItemText(list, (int)i, 4, ScheduleNextText(e));
    }
    SelectIndex(list, keep < 0 ? 0 : keep);
}

// Every running session and how long it has been going, as a read only edit so
// it can be read a line at a time. The sessions list has a Status column, but
// a column cannot hold a clock that ticks.
static std::wstring RunningText()
{
    std::wstring out;
    int running = 0;
    for (const Session& s : g_store.sessions)
    {
        if (!SessionRunning(s.id)) continue;
        ++running;
        Engine::Status st;
        const bool got = SessionStatus(s.id, &st);
        if (!out.empty()) out += L"\r\n";
        out += got ? FormatW(L"%s - running for %s", s.name.c_str(),
                             DurationText(st.runSeconds).c_str())
                   : FormatW(L"%s - running", s.name.c_str());
        if (got && st.recFiles > 0)
            out += FormatW(L", recording %s into %d file%s",
                           DurationText(st.recSeconds).c_str(),
                           st.recFiles, st.recFiles == 1 ? L"" : L"s");
    }
    if (running == 0) return L"No sessions are running.";
    return out;
}

static void RefreshRunningField(HWND dlg)
{
    if (!dlg || !IsWindow(dlg)) return;
    // Only when it has changed, so a screen reader is not told the same thing
    // every time the timer comes round.
    const std::wstring want = RunningText();
    wchar_t had[1024] = { 0 };
    GetDlgItemTextW(dlg, IDC_MAIN_RUNNING, had, 1024);
    if (want != had) SetDlgItemTextW(dlg, IDC_MAIN_RUNNING, want.c_str());
}

void RefreshMainList()
{
    if (!g_mainDlg || !IsWindow(g_mainDlg)) return;
    HWND list = GetDlgItem(g_mainDlg, IDC_SESSION_LIST);

    int keep = SelectedIndex(list);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);

    TrayUpdateTip(g_mainDlg);

    for (size_t i = 0; i < g_store.sessions.size(); ++i)
    {
        const Session& s = g_store.sessions[i];

        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(s.name.c_str());
        it.lParam = (LPARAM)s.id;
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);

        SetItemText(list, (int)i, 1, FormatW(L"%zu", s.blocks.size()));
        SetItemText(list, (int)i, 2, SessionRunning(s.id) ? L"Running" : L"Stopped");
        SetItemText(list, (int)i, 3, s.autoRun ? L"At startup" : L"");
        SetItemText(list, (int)i, 4, s.hotkey.Set() ? HotkeyScopeText(s.hotkey) : L"");
    }
    SelectIndex(list, keep < 0 ? 0 : keep);
    RefreshRunningField(g_mainDlg);
}

static ScheduleEvent* SelectedEvent()
{
    if (!g_mainDlg) return nullptr;
    int i = SelectedIndex(GetDlgItem(g_mainDlg, IDC_SCHED_LIST));
    if (i < 0 || (size_t)i >= g_store.events.size()) return nullptr;
    return &g_store.events[(size_t)i];
}

static Session* SelectedSession()
{
    HWND list = GetDlgItem(g_mainDlg, IDC_SESSION_LIST);
    int i = SelectedIndex(list);
    if (i < 0 || (size_t)i >= g_store.sessions.size()) return nullptr;
    return &g_store.sessions[(size_t)i];
}

// A session that has just been added or renamed has moved: the list is in name
// order. Following it by id keeps the selection on the session the user was
// working with rather than on whatever landed in that row.
static void SelectSessionById(HWND list, int id)
{
    for (size_t i = 0; i < g_store.sessions.size(); ++i)
        if (g_store.sessions[i].id == id) { SelectIndex(list, (int)i); return; }
}

static void SaveStore(HWND owner)
{
    std::wstring err;
    if (!g_store.Save(&err)) ErrorBox(owner, err);
    // The document decides which global hotkeys exist, so every change to it
    // rebuilds them. A binding then never points at a block that has moved.
    RefreshItemHotkeys(g_mainDlg);
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Export and import one session
//
// A session as a file somebody can send to somebody else, which is what makes
// "set this up for me" a thing one person can do for another. The file is the
// same JSON a saved session is, so nothing here describes a block a second
// time and nothing can fall behind the document format.
//
// The one thing that does not travel is which devices a session points at: an
// endpoint id is a GUID made on the machine the device was plugged into. The
// name is saved beside it, so an import looks for the name and adopts the id
// it finds here - and says plainly which blocks it could not place.
// ---------------------------------------------------------------------------

static const wchar_t* kSessionExt = L"fastroute";

// A file name that cannot fail to be created, out of a session name that can
// hold anything the user typed.
static std::wstring SafeFileName(const std::wstring& name)
{
    std::wstring out;
    for (wchar_t c : name)
    {
        if (wcschr(L"\\/:*?\"<>|", c)) out += L'-';
        else if (c >= L' ')            out += c;
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    return out.empty() ? std::wstring(L"session") : out;
}

// One file, through the same shell dialog the folder picker uses. `save`
// chooses between the two; `path` is in and out.
static bool PickSessionFile(HWND owner, bool save, std::wstring& path)
{
    IFileDialog* dlg = nullptr;
    const CLSID which = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(which, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))) || !dlg)
        return false;

    const COMDLG_FILTERSPEC filters[] = {
        { L"FastRoute session", L"*.fastroute" },
        { L"All files", L"*.*" },
    };
    dlg->SetFileTypes(2, filters);
    dlg->SetFileTypeIndex(1);
    dlg->SetDefaultExtension(kSessionExt);
    dlg->SetTitle(save ? L"Export the session to a file"
                       : L"Import a session from a file");

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM |
                    (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));

    if (save && !path.empty()) dlg->SetFileName(path.c_str());

    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item)
        {
            LPWSTR got = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &got)) && got)
            {
                path = got;
                CoTaskMemFree(got);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

void ExportSession(HWND owner, const Session& s)
{
    std::wstring path = SafeFileName(s.name) + L"." + kSessionExt;
    if (!PickSessionFile(owner, true, path)) return;

    const std::wstring text = SessionToText(s);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wt, ccs=UTF-8") != 0 || !f)
    {
        ErrorBox(owner, L"That file could not be written: " + path);
        return;
    }
    const bool wrote = fputws(text.c_str(), f) >= 0;
    fclose(f);
    if (!wrote)
    {
        ErrorBox(owner, L"That file could not be written: " + path);
        return;
    }

    // Said rather than left silent: a dialog that closes with nothing on
    // screen is a dialog you cannot tell worked.
    MessageBoxW(owner,
                FormatW(L"\"%s\" was exported to:\n\n%s\n\n"
                        L"It holds %zu block%s. Anyone with FastRoute can "
                        L"import it with Ctrl+I.",
                        s.name.c_str(), path.c_str(), s.blocks.size(),
                        s.blocks.size() == 1 ? L"" : L"s").c_str(),
                L"FastRoute", MB_OK | MB_ICONINFORMATION);
}

// Every endpoint on this machine, both directions, as id and name. The one
// place this needs a sound card, which is why RepointDevices takes the list
// rather than going and getting it.
static std::vector<std::pair<std::wstring, std::wstring>> EndpointsHere()
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (bool render : { true, false })
        for (const AudioEndpoint& e : EnumEndpoints(render))
            out.push_back({ e.id, e.name });
    return out;
}

int ImportSession(HWND owner)
{
    std::wstring path;
    if (!PickSessionFile(owner, false, path)) return 0;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt, ccs=UTF-8") != 0 || !f)
    {
        ErrorBox(owner, L"That file could not be read: " + path);
        return 0;
    }
    std::wstring text;
    wchar_t buf[4096];
    while (fgetws(buf, 4096, f)) text += buf;
    fclose(f);

    Session s;
    std::wstring err;
    if (!SessionFromText(text, s, &err))
    {
        ErrorBox(owner, err);
        return 0;
    }

    const ImportReport rep = RepointDevices(s, EndpointsHere());

    // A name already in use is not refused: two sessions may legitimately be
    // called the same thing, and renaming behind the user's back would be
    // worse than a duplicate they can see and rename.
    bool nameTaken = false;
    for (const Session& other : g_store.sessions)
        if (other.name == s.name) nameTaken = true;

    s.id = g_store.NewId();
    g_store.sessions.push_back(s);
    g_store.SortSessions();
    SaveStore(owner);
    RefreshMainList();
    SelectSessionById(GetDlgItem(g_mainDlg, IDC_SESSION_LIST), s.id);

    // What happened, in full. The devices are the part worth reading: a
    // session imported from another machine may be pointing at nothing.
    std::wstring said = FormatW(L"\"%s\" was imported, with %zu block%s.",
                                s.name.c_str(), s.blocks.size(),
                                s.blocks.size() == 1 ? L"" : L"s");
    if (rep.rematched == 1)
        said += L"\n\nOne block's device was found again by name and re-pointed "
                L"at this machine's copy of it.";
    else if (rep.rematched > 1)
        said += FormatW(L"\n\n%d blocks' devices were found again by name and "
                        L"re-pointed at this machine's copies of them.",
                        rep.rematched);
    if (!rep.unmatched.empty())
    {
        said += FormatW(L"\n\n%zu block%s pointing at a device this machine "
                        L"has not got, so %s need setting before the session "
                        L"will run:",
                        rep.unmatched.size(),
                        rep.unmatched.size() == 1 ? L" is" : L"s are",
                        rep.unmatched.size() == 1 ? L"it" : L"they");
        for (const std::wstring& n : rep.unmatched) said += L"\n  " + n;
    }
    if (nameTaken)
        said += L"\n\nAnother session is called that too. Press F2 to rename "
                L"this one.";

    MessageBoxW(owner, said.c_str(), L"FastRoute", MB_OK | MB_ICONINFORMATION);
    OpenSessionWindow(s.id);
    return s.id;
}


static void CmdNew(HWND dlg)
{
    std::wstring name = FormatW(L"Session %zu", g_store.sessions.size() + 1);
    if (!PromptForName(dlg, L"New Session", L"Session &name:", name)) return;

    Session s;
    s.id = g_store.NewId();
    s.name = name;
    g_store.sessions.push_back(s);
    g_store.SortSessions();
    SaveStore(dlg);
    RefreshMainList();
    SelectSessionById(GetDlgItem(dlg, IDC_SESSION_LIST), s.id);
    OpenSessionWindow(s.id);
}

static void CmdRename(HWND dlg)
{
    Session* s = SelectedSession();
    if (!s) return;
    std::wstring name = s->name;
    if (!PromptForName(dlg, L"Rename Session", L"Session &name:", name)) return;
    const int id = s->id;
    s->name = name;
    // The new name may belong somewhere else in the list, and `s` points into
    // the vector being sorted, so nothing may touch it after this.
    g_store.SortSessions();
    SaveStore(dlg);
    RefreshMainList();
    SelectSessionById(GetDlgItem(dlg, IDC_SESSION_LIST), id);
}

static void CmdDelete(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_SESSION_LIST);
    int i = SelectedIndex(list);
    Session* s = SelectedSession();
    if (!s) return;

    std::wstring q = FormatW(L"Delete the session \"%s\"?", s->name.c_str());
    if (MessageBoxW(dlg, q.c_str(), L"FastRoute", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    int id = s->id;
    StopSession(id);
    HWND w = SessionWindowFor(id);
    if (w) DestroyWindow(w);

    g_store.sessions.erase(g_store.sessions.begin() + i);
    SaveStore(dlg);
    RefreshMainList();
    SelectIndex(list, i);
}

static void CmdOpen(HWND dlg)
{
    Session* s = SelectedSession();
    if (s) OpenSessionWindow(s->id);
}

static void CmdRun(HWND dlg)
{
    Session* s = SelectedSession();
    if (s) StartSession(dlg, s->id);
}

static void CmdStop(HWND dlg)
{
    Session* s = SelectedSession();
    if (s) StopSession(s->id);
}

// ---------------------------------------------------------------------------

static void CmdSchedNew(HWND dlg)
{
    if (g_store.sessions.empty())
    {
        ErrorBox(dlg, L"There are no sessions to schedule yet. Make one first "
                      L"with Ctrl+N.");
        return;
    }

    ScheduleEvent e;
    e.id = g_store.NewEventId();
    e.sessionId = g_store.sessions[0].id;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    e.hour = now.wHour;
    e.minute = now.wMinute;

    if (!EditScheduleEvent(dlg, e)) return;

    g_store.events.push_back(e);
    SaveStore(dlg);
    RefreshScheduleList();
    SelectIndex(GetDlgItem(dlg, IDC_SCHED_LIST), (int)g_store.events.size() - 1);
}

static void CmdSchedEdit(HWND dlg)
{
    ScheduleEvent* e = SelectedEvent();
    if (!e) return;

    ScheduleEvent want = *e;
    if (!EditScheduleEvent(dlg, want)) return;

    *e = want;
    // Any stop already owed belonged to the old settings.
    SchedulerForget(e->id);
    SaveStore(dlg);
    RefreshScheduleList();
}

static void CmdSchedDelete(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_SCHED_LIST);
    int i = SelectedIndex(list);
    ScheduleEvent* e = SelectedEvent();
    if (!e) return;

    Session* s = g_store.ById(e->sessionId);
    std::wstring q = FormatW(L"Delete the event that runs %s %s?",
                             s ? s->name.c_str() : L"a deleted session",
                             ScheduleWhenText(*e).c_str());
    if (MessageBoxW(dlg, q.c_str(), L"FastRoute", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    SchedulerForget(e->id);
    g_store.events.erase(g_store.events.begin() + i);
    SaveStore(dlg);
    RefreshScheduleList();
    SelectIndex(list, i);
}

static void CmdSchedToggle(HWND dlg)
{
    ScheduleEvent* e = SelectedEvent();
    if (!e) return;
    e->enabled = !e->enabled;
    if (!e->enabled) SchedulerForget(e->id);
    // Re-enabling should not make it think it slept through today.
    if (e->enabled) ScheduleMarkCurrent(*e);
    SaveStore(dlg);
    RefreshScheduleList();
}

// Marks the focused session to be started the next time FastRoute launches.
// It is a per-session flag rather than an application setting, so it lives in
// sessions.json alongside everything else about that session.
static void CmdAutoRun(HWND dlg)
{
    Session* s = SelectedSession();
    if (!s) return;
    s->autoRun = !s->autoRun;
    SaveStore(dlg);
    RefreshMainList();
}

// Ctrl+Shift+O. There is no block list in this window, so there is no selected
// block to prefer - the session's first recorder is the one that opens.
static void CmdOpenRecordFolder(HWND dlg)
{
    Session* s = SelectedSession();
    if (!s) return;
    OpenRecordFolder(dlg, s->id, -1);
}

// A hotkey that starts and stops this session, from anywhere or from here.
static void CmdSessionHotkey(HWND dlg)
{
    Session* s = SelectedSession();
    if (!s) return;

    Hotkey want = s->hotkey;
    if (!EditHotkey(dlg, FormatW(L"running the session %s", s->name.c_str()),
                    want, HF_SESSION))
        return;

    s->hotkey = want;
    SaveStore(dlg);          // which also re-registers everything
    RefreshMainList();
}

// Ctrl+R: run the focused session, or stop it if it is already running.
static void CmdToggle(HWND dlg)
{
    Session* s = SelectedSession();
    if (!s) return;
    if (SessionRunning(s->id)) StopSession(s->id);
    else                       StartSession(dlg, s->id);
}

// ---------------------------------------------------------------------------

INT_PTR CALLBACK MainDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    // A second FastRoute was launched and handed over to this one rather than
    // starting beside it. Not in the switch below because a registered message
    // has no number until it is asked for at run time.
    if (msg == ShowRequestMessage())
    {
        ShowFromTray(dlg);
        return TRUE;
    }

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // RefreshMainList() and the command handlers reach the list through
        // g_mainDlg, which wWinMain only assigns once CreateDialogParam returns.
        g_mainDlg = dlg;
        g_mainAccel = LoadAcceleratorsW(g_inst, MAKEINTRESOURCEW(IDR_MAIN_MENU));

        // A dialog does not pick up the application icon on its own, and the
        // tray needs one to show.
        HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPMAIN));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);

        // Failing to register is not fatal: FastRoute still runs, it just
        // cannot be brought back with a key. It goes to the error log.
        ApplyAppHotkey(dlg, nullptr);
        RefreshItemHotkeys(dlg);

        // Application blocks can be pointed at whatever is in front, which
        // means knowing what that was before the session started. The hook is
        // installed here because it reports through this window's message
        // loop, and it runs whether or not any block is using it: by the time
        // one is, the answer it needs is already in the past.
        ForegroundWatchStart();

        HWND list = GetDlgItem(dlg, IDC_SESSION_LIST);
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
        AddColumn(list, 0, L"Session", 230);
        AddColumn(list, 1, L"Blocks", 70);
        AddColumn(list, 2, L"Status", 90);
        AddColumn(list, 3, L"Startup", 70);
        AddColumn(list, 4, L"Hotkey", 110);

        ListKeys keys;
        keys.onEnter = IDM_SESSION_OPEN;
        keys.onDelete = IDM_SESSION_DELETE;
        SubclassListKeys(list, keys);

        HWND tabs = GetDlgItem(dlg, IDC_MAIN_TABS);
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(L"Sessions");
        SendMessageW(tabs, TCM_INSERTITEMW, 0, (LPARAM)&ti);
        ti.pszText = const_cast<wchar_t*>(L"Schedule");
        SendMessageW(tabs, TCM_INSERTITEMW, 1, (LPARAM)&ti);

        HWND sched = GetDlgItem(dlg, IDC_SCHED_LIST);
        SendMessageW(sched, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
        AddColumn(sched, 0, L"Session", 110);
        AddColumn(sched, 1, L"When", 140);
        AddColumn(sched, 2, L"Stop", 120);
        AddColumn(sched, 3, L"Repeat", 60);
        AddColumn(sched, 4, L"Next", 80);

        ListKeys schedKeys;
        schedKeys.onEnter = IDM_SCHED_EDIT;
        schedKeys.onDelete = IDM_SCHED_DELETE;
        SubclassListKeys(sched, schedKeys);

        ShowTab(dlg, kTabSessions);
        RefreshMainList();
        RefreshScheduleList();

        // Twice a minute: the scheduler works in whole minutes, so this is
        // enough to catch every one of them without being a busy loop.
        SetTimer(dlg, kSchedulerTimer, 20000, nullptr);
        // The running times need a second hand, which that is nowhere near.
        SetTimer(dlg, kRunningTimer, 1000, nullptr);

        SetFocus(list);
        return FALSE;   // focus set explicitly
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_SESSION_NEW:     CmdNew(dlg);    return TRUE;
        case IDM_SESSION_OPEN:    CmdOpen(dlg);   return TRUE;
        case IDM_SESSION_RUN:     CmdRun(dlg);    return TRUE;
        case IDM_SESSION_STOP:    CmdStop(dlg);   return TRUE;
        case IDM_SESSION_TOGGLE:  CmdToggle(dlg); return TRUE;
        case IDM_SESSION_AUTORUN: CmdAutoRun(dlg); return TRUE;
        case IDM_SESSION_HOTKEY:  CmdSessionHotkey(dlg); return TRUE;
        case IDM_SESSION_OPENDIR: CmdOpenRecordFolder(dlg); return TRUE;
        case IDM_SESSION_RENAME:  CmdRename(dlg); return TRUE;
        case IDM_SESSION_DELETE:  CmdDelete(dlg); return TRUE;
        case IDM_SESSION_IMPORT:  ImportSession(dlg); return TRUE;
        case IDM_SESSION_EXPORT:
        {
            Session* s = SelectedSession();
            if (s) ExportSession(dlg, *s);
            return TRUE;
        }
        case IDM_HELP_ERRORS:     ShowErrorLogDialog(dlg); return TRUE;
        // Asked for by hand, so it reports either way rather than only when
        // there is something to report.
        case IDM_HELP_UPDATE:     ShowCheckForUpdates(dlg, false); return TRUE;
        case IDM_APP_SETTINGS:    ShowSettingsDialog(dlg); return TRUE;
        case IDM_APP_HIDE:        HideToTray(dlg); return TRUE;
        case IDM_TRAY_SHOW:       ShowFromTray(dlg); return TRUE;
        case IDM_SCHED_NEW:       CmdSchedNew(dlg); return TRUE;
        case IDM_SCHED_EDIT:      CmdSchedEdit(dlg); return TRUE;
        case IDM_SCHED_DELETE:    CmdSchedDelete(dlg); return TRUE;
        case IDM_SCHED_TOGGLE:    CmdSchedToggle(dlg); return TRUE;
        case IDM_HELP_ABOUT:      ShowAboutDialog(dlg); return TRUE;
        case IDM_APP_EXIT:
            SendMessageW(dlg, WM_CLOSE, 0, 0);
            return TRUE;

        // Escape, which a dialog sends here as IDCANCEL, deliberately does
        // nothing. This is the main window rather than a dialog with
        // something to cancel, and Escape is a key people press to back out
        // of whatever they are in the middle of - pressed one window too
        // many, it was shutting FastRoute down and stopping every session with
        // it. Exiting is the File menu, Alt+F4, or the notification area.
        case IDCANCEL:
            return TRUE;
        }
        break;

    case WM_HOTKEY:
    case WM_APP_TRAY:
        if (TrayHandleMessage(dlg, msg, wp, lp)) return TRUE;
        break;

    case WM_INITMENUPOPUP:
    {
        // The menu bar carries the same toggle, so it has to show the same
        // state the context menu does.
        Session* s = SelectedSession();
        CheckMenuItem(reinterpret_cast<HMENU>(wp), IDM_SESSION_AUTORUN,
                      MF_BYCOMMAND | ((s && s->autoRun) ? MF_CHECKED : MF_UNCHECKED));
        ScheduleEvent* e = SelectedEvent();
        CheckMenuItem(reinterpret_cast<HMENU>(wp), IDM_SCHED_TOGGLE,
                      MF_BYCOMMAND | ((e && e->enabled) ? MF_CHECKED : MF_UNCHECKED));
        break;
    }

    case WM_TIMER:
        if (wp == kSchedulerTimer) SchedulerTick(dlg);
        else if (wp == kRunningTimer) RefreshRunningField(dlg);
        return TRUE;

    case WM_CONTEXTMENU:
    {
        HWND ctl = reinterpret_cast<HWND>(wp);
        if (ctl == GetDlgItem(dlg, IDC_SCHED_LIST))
        {
            bool has = SelectedEvent() != nullptr;
            std::map<UINT, bool> enabled;
            enabled[IDM_SCHED_EDIT]   = has;
            enabled[IDM_SCHED_TOGGLE] = has;
            enabled[IDM_SCHED_DELETE] = has;
            enabled[IDM_SCHED_NEW]    = true;

            std::map<UINT, bool> checked;
            ScheduleEvent* e = SelectedEvent();
            checked[IDM_SCHED_TOGGLE] = has && e->enabled;

            ShowContextMenu(dlg, IDR_SCHED_CTX, CtxMenuPoint(ctl, lp),
                            has ? IDM_SCHED_EDIT : IDM_SCHED_NEW, enabled, &checked);
            return TRUE;
        }
        if (ctl != GetDlgItem(dlg, IDC_SESSION_LIST)) break;

        Session* s = SelectedSession();
        bool has = (s != nullptr);
        bool running = has && SessionRunning(s->id);

        std::map<UINT, bool> enabled;
        enabled[IDM_SESSION_OPEN]    = has;
        enabled[IDM_SESSION_RUN]     = has && !running;
        enabled[IDM_SESSION_STOP]    = has && running;
        enabled[IDM_SESSION_AUTORUN] = has;
        enabled[IDM_SESSION_HOTKEY]  = has;
        enabled[IDM_SESSION_OPENDIR] = has;
        enabled[IDM_SESSION_RENAME]  = has;
        enabled[IDM_SESSION_DELETE]  = has;

        std::map<UINT, bool> checked;
        checked[IDM_SESSION_AUTORUN] = has && s->autoRun;

        ShowContextMenu(dlg, IDR_MAIN_CTX, CtxMenuPoint(ctl, lp),
                        has ? IDM_SESSION_OPEN : 0, enabled, &checked);
        return TRUE;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom == IDC_SESSION_LIST && hdr->code == NM_DBLCLK)
        {
            CmdOpen(dlg);
            return TRUE;
        }
        if (hdr->idFrom == IDC_SCHED_LIST && hdr->code == NM_DBLCLK)
        {
            CmdSchedEdit(dlg);
            return TRUE;
        }
        if (hdr->idFrom == IDC_MAIN_TABS && hdr->code == TCN_SELCHANGE)
        {
            int tab = (int)SendDlgItemMessageW(dlg, IDC_MAIN_TABS, TCM_GETCURSEL, 0, 0);
            ShowTab(dlg, tab);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        // Before anything is taken apart, since the answer may be no. Alt+F4
        // and the notification area's Exit both arrive here too, which is the
        // point: they are the two ways to exit by accident.
        if (!ConfirmExitWhileRunning(dlg)) return TRUE;

        KillTimer(dlg, kSchedulerTimer);
        KillTimer(dlg, kRunningTimer);
        // Closing exits. Hiding is Ctrl+H, the menu item, or the hotkey, and
        // nothing else - a window that vanished on Alt+F4 would be a window
        // people could not get rid of.
        ForegroundWatchStop();
        TrayShutdown(dlg);
        CloseAllSessionWindows();
        StopAllSessions();
        SaveStore(dlg);
        DestroyWindow(dlg);
        return TRUE;

    case WM_DESTROY:
        g_mainDlg = nullptr;
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

HACCEL MainAccel() { return g_mainAccel; }
