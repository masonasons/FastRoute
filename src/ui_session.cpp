#include "app.h"
#include <map>
#include <algorithm>

static std::map<int, HWND> g_windows;
static HACCEL g_sessionAccel = nullptr;
static bool g_syncingChecks = false;   // ignore our own check-state updates

// ---------------------------------------------------------------------------
// The block clipboard
//
// A block of its own rather than the Windows clipboard, and deliberately. A
// cut is destructive, so what comes back has to be exactly what went in - and
// a block carries plugin parameter values, ten EQ bands and four hotkeys,
// which a round trip through text could quietly lose a corner of. It is
// process wide, so a block cut in one session window pastes into another.
//
// Sharing a block with someone else is what exporting a session is for.
// ---------------------------------------------------------------------------
static Block g_clip;
static bool  g_clipHas = false;
// What it is called, for the status line: the block itself may be renamed or
// deleted after being copied, and saying "Microphone pasted" is the whole
// feedback a screen reader user gets.
static std::wstring ClipName()
{
    if (!g_clipHas) return std::wstring();
    return g_clip.label.empty() ? g_clip.TypeName() : g_clip.label;
}

// ---------------------------------------------------------------------------

HWND SessionWindowFor(int sessionId)
{
    auto it = g_windows.find(sessionId);
    return it == g_windows.end() ? nullptr : it->second;
}

HACCEL AccelForWindow(HWND hwnd)
{
    if (hwnd == g_mainDlg) return MainAccel();
    for (auto& kv : g_windows) if (kv.second == hwnd) return g_sessionAccel;
    return nullptr;
}

int SessionWindowIdFor(HWND hwnd)
{
    for (auto& kv : g_windows) if (kv.second == hwnd) return kv.first;
    return 0;
}

void SetSessionWindowsVisible(bool show)
{
    for (auto& kv : g_windows)
        ShowWindow(kv.second, show ? SW_SHOW : SW_HIDE);
}

void CloseAllSessionWindows()
{
    std::map<int, HWND> copy = g_windows;
    for (auto& kv : copy) DestroyWindow(kv.second);
    g_windows.clear();
}

// ---------------------------------------------------------------------------

static int SessionIdOf(HWND dlg)
{
    return (int)GetWindowLongPtrW(dlg, DWLP_USER);
}

static int SelectedBlock(HWND dlg)
{
    return (int)SendDlgItemMessageW(dlg, IDC_BLOCK_LIST, LVM_GETNEXTITEM,
                                    (WPARAM)-1, LVNI_SELECTED);
}

static void SelectBlock(HWND dlg, int i)
{
    HWND list = GetDlgItem(dlg, IDC_BLOCK_LIST);
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

static void AddCol(HWND list, int index, const wchar_t* title, int width)
{
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = width;
    col.iSubItem = index;
    SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)index, (LPARAM)&col);
}

static void SetSub(HWND list, int item, int sub, const std::wstring& text)
{
    LVITEMW it{};
    it.iSubItem = sub;
    it.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)item, (LPARAM)&it);
}

static const wchar_t* KindName(BlockKind k)
{
    switch (k)
    {
    case BlockKind::Input:  return L"Input";
    case BlockKind::Effect: return L"Effect";
    default:                return L"Output";
    }
}

// ---------------------------------------------------------------------------

// What the tree's lParam indexes into. Rebuilt with the tree, and read by the
// add command, so the two cannot disagree about what row 7 is.
static std::vector<LibraryItem> g_library;

// Whether a group was left shut last time. Collapsed rather than expanded is
// what is stored, so a group that did not exist when the setting was written
// arrives open.
static bool LibraryGroupShut(const wchar_t* cat)
{
    for (const std::wstring& c : g_store.settings.libraryCollapsed)
        if (c == cat) return true;
    return false;
}

// Building the tree expands groups itself, and that sends the same
// notification a person toggling one does. Reading the tree back halfway
// through building it would see only the groups inserted so far and conclude
// the rest are shut - which would then be saved over the real setting.
static bool g_buildingLibrary = false;

// Reads the groups back out of the tree and remembers which are shut, so the
// state survives closing the window.
static void RememberLibraryGroups(HWND dlg)
{
    if (g_buildingLibrary) return;

    HWND tree = GetDlgItem(dlg, IDC_LIB_TREE);
    std::vector<std::wstring> shut;

    HTREEITEM h = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    while (h)
    {
        wchar_t text[128] = {};
        TVITEMW it{};
        it.mask = TVIF_TEXT | TVIF_STATE;
        it.stateMask = TVIS_EXPANDED;
        it.hItem = h;
        it.pszText = text;
        it.cchTextMax = (int)(sizeof(text) / sizeof(text[0])) - 1;
        if (SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&it) &&
            !(it.state & TVIS_EXPANDED))
            shut.push_back(text);
        h = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)h);
    }

    if (shut == g_store.settings.libraryCollapsed) return;
    g_store.settings.libraryCollapsed = shut;
    g_store.Save();
}

static void BuildLibraryTree(HWND dlg)
{
    g_buildingLibrary = true;

    HWND tree = GetDlgItem(dlg, IDC_LIB_TREE);
    SendMessageW(tree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);

    // The library is rebuilt every time, because half of it can change without
    // this window knowing: a plugin scan in Settings adds to the external
    // effects, and the tree has to pick that up the next time it is drawn.
    g_library = BuildLibrary();

    for (const wchar_t* cat : LibraryCategories())
    {
        TVINSERTSTRUCTW ins{};
        ins.hParent = TVI_ROOT;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM;
        ins.item.pszText = const_cast<wchar_t*>(cat);
        ins.item.lParam = -1;
        HTREEITEM parent = (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);

        int shown = 0;
        for (size_t i = 0; i < g_library.size(); ++i)
        {
            if (g_library[i].category != cat) continue;
            // A block whose API this Windows does not have is not shown at
            // all. Offering it and then refusing at run time would be worse.
            if (!BlockTypeAvailable(g_library[i].typeId)) continue;
            TVINSERTSTRUCTW leaf{};
            leaf.hParent = parent;
            leaf.hInsertAfter = TVI_LAST;
            leaf.item.mask = TVIF_TEXT | TVIF_PARAM;
            leaf.item.pszText = const_cast<wchar_t*>(g_library[i].name.c_str());
            leaf.item.lParam = (LPARAM)i;
            SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&leaf);
            ++shown;
        }

        // An empty group would read as a dead end. Saying why, and where to
        // go, is worth the one line it costs.
        if (shown == 0 && wcscmp(cat, L"External Effects") == 0)
        {
            TVINSERTSTRUCTW leaf{};
            leaf.hParent = parent;
            leaf.hInsertAfter = TVI_LAST;
            leaf.item.mask = TVIF_TEXT | TVIF_PARAM;
            leaf.item.pszText = const_cast<wchar_t*>(
                L"(no plugins found - add a folder in Settings and scan)");
            leaf.item.lParam = -1;
            SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&leaf);
        }
        if (!LibraryGroupShut(cat))
            SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)parent);
    }

    // Put the caret on the first group. Without this it lands wherever the
    // control happens to leave it, which is not the top and is not the same
    // thing twice - and for anybody reading the window aloud, the first thing
    // read should be the first thing in it.
    HTREEITEM first = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    if (first) SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)first);

    g_buildingLibrary = false;
}

static std::wstring SessionStateLine(int sessionId)
{
    Session* s = g_store.ById(sessionId);
    if (!s) return L"";

    size_t off = 0;
    for (const Block& b : s->blocks) if (!b.enabled) ++off;

    std::wstring line;
    Engine::Status st;
    if (SessionRunning(sessionId) && SessionStatus(sessionId, &st))
    {
        line = FormatW(L"Running at %d Hz, about %d ms latency (%d out, %d in%s). "
                       L"Press Ctrl+R to stop.",
                       st.rate, st.outMs + st.inMs + st.fxMs, st.outMs, st.inMs,
                       st.fxMs ? FormatW(L", %d effects", st.fxMs).c_str() : L"");
        // Only worth saying when an input is not actually running at the rate
        // it claims; ordinary clock drift sits far below this.
        if (st.inRatio > 1.002 || st.inRatio < 0.998)
            line += FormatW(L"  Input resampled %.3fx.", st.inRatio);
        if (st.glitches)
            line += FormatW(L"  %u dropouts.", st.glitches);
        // A block pointed at the foreground has no other way of saying what it
        // has hold of, and in follow mode that is the one thing worth knowing.
        if (!st.following.empty())
            line += FormatW(L"  Capturing %s.", st.following.c_str());
        // There is no level meter to look at, so a gate holding off has to be
        // said out loud, or a recorder skipping silence looks like a fault.
        if (st.recFiles)
            line += FormatW(L"  Recording%s, %d file%s so far.",
                            st.recSkipping ? L" paused below the silence threshold" : L"",
                            st.recFiles, st.recFiles == 1 ? L"" : L"s");
    }
    else
        line = L"Stopped. Press Ctrl+R to run.";

    if (off) line += FormatW(L"  %zu of %zu blocks disabled.", off, s->blocks.size());
    if (s->autoRun) line += L"  Runs at startup.";
    return line;
}

// The times field, which is a read only edit rather than a label so a screen
// reader can walk it a line at a time. Written on the same timer as the status
// line, and left saying so when nothing is running: an empty box reads as a
// fault rather than as a session that is stopped.
static std::wstring SessionTimesText(int sessionId)
{
    Engine::Status st;
    if (!SessionRunning(sessionId) || !SessionStatus(sessionId, &st))
        return L"Not running.";

    std::wstring out = FormatW(L"Running for %s", DurationText(st.runSeconds).c_str());

    if (st.recFiles > 0)
    {
        // Recorded time is not running time: the silence gate can hold off for
        // most of a session, and the difference is the whole point of it.
        out += FormatW(L"\r\nRecorded %s into %d file%s",
                       DurationText(st.recSeconds).c_str(),
                       st.recFiles, st.recFiles == 1 ? L"" : L"s");
        if (st.recBytes > 0)
            out += FormatW(L"\r\nCurrent file %s", SizeText(st.recBytes).c_str());
        if (st.recSkipping)
            out += L"\r\nHolding off: the audio is below the silence threshold.";
    }

    // Anything a block wants to say for itself, one line each. A buffer that
    // has been rewound is the case this exists for: there is nothing else on
    // screen that would say so.
    for (const std::wstring& line : st.blockStatus)
        out += L"\r\n" + line;

    return out;
}

void RefreshSessionWindow(int sessionId)
{
    HWND dlg = SessionWindowFor(sessionId);
    if (!dlg) return;

    Session* s = g_store.ById(sessionId);
    if (!s) return;

    SetWindowTextW(dlg, (s->name + L" - FastRoute Session").c_str());

    HWND list = GetDlgItem(dlg, IDC_BLOCK_LIST);
    int keep = SelectedBlock(dlg);
    g_syncingChecks = true;
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);

    for (size_t i = 0; i < s->blocks.size(); ++i)
    {
        const Block& b = s->blocks[i];

        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        std::wstring name = b.TypeName();
        it.pszText = const_cast<wchar_t*>(name.c_str());
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);

        SetSub(list, (int)i, 1, KindName(b.Kind()));
        SetSub(list, (int)i, 2, b.ParamSummary());
        SetSub(list, (int)i, 3, b.hotkey.Set() ? HotkeyScopeText(b.hotkey) : L"");
        ListView_SetCheckState(list, (int)i, b.enabled ? TRUE : FALSE);
    }
    g_syncingChecks = false;
    SelectBlock(dlg, keep < 0 ? 0 : keep);

    SetDlgItemTextW(dlg, IDC_SESSION_STATE, SessionStateLine(sessionId).c_str());
    SetDlgItemTextW(dlg, IDC_SESSION_TIMES, SessionTimesText(sessionId).c_str());
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Every edit takes the same path: persist it, push it into the running graph
// if there is one, then redraw. Nothing here needs the session to be stopped.
static void CommitEdit(HWND dlg, int id)
{
    std::wstring err;
    if (!g_store.Save(&err)) ErrorBox(dlg, err);
    RefreshItemHotkeys(g_mainDlg);
    ApplyLiveEdit(dlg, id);
    RefreshSessionWindow(id);
    RefreshMainList();
}

// A name of the user's own for one block. Two Gain blocks in a session are
// otherwise told apart only by where they sit in the list, which is a poor way
// to find the one you meant. Clearing the name puts the block type back.
static void CmdRenameBlock(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    if (i < 0 || (size_t)i >= s->blocks.size()) return;
    Block& b = s->blocks[(size_t)i];

    // Starts from whatever it is called now, so renaming a block that has
    // never been named begins from its type rather than from nothing.
    std::wstring name = b.label.empty() ? b.TypeName() : b.label;
    if (!PromptForName(dlg, L"Rename Block",
                       L"Block &name (leave empty for the block type):", name))
        return;

    name = Trim(name);
    const BlockType* t = FindBlockType(b.typeId);
    // Typing the type's own name back in is the same as not naming it.
    if (t && name == t->name) name.clear();
    b.label = name;

    CommitEdit(dlg, id);
    SelectBlock(dlg, i);
    SetDlgItemTextW(dlg, IDC_SESSION_STATE,
                    b.label.empty()
                        ? FormatW(L"Block name cleared. It is a %s.", b.TypeName().c_str()).c_str()
                        : FormatW(L"Block renamed to %s.", b.label.c_str()).c_str());
}

static void CmdAddBlock(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    HWND tree = GetDlgItem(dlg, IDC_LIB_TREE);
    HTREEITEM sel = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0);
    if (!sel) return;

    TVITEMW it{};
    it.mask = TVIF_PARAM;
    it.hItem = sel;
    if (!SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&it)) return;
    if (it.lParam < 0)
    {
        // A category was selected; expand it instead of adding. Remembering
        // it is the notification handler's job, which catches the arrow keys
        // too.
        SendMessageW(tree, TVM_EXPAND, TVE_TOGGLE, (LPARAM)sel);
        return;
    }

    if ((size_t)it.lParam >= g_library.size()) return;
    const LibraryItem& t = g_library[(size_t)it.lParam];

    Block b;
    InitBlockFrom(b, t);

    s->blocks.push_back(b);
    CommitEdit(dlg, id);

    // Focus stays on the library tree so several blocks can be added in a row.
    SelectBlock(dlg, (int)s->blocks.size() - 1);
    SetDlgItemTextW(dlg, IDC_SESSION_STATE,
                    FormatW(L"Added %s. %zu blocks in this session.",
                            t.name.c_str(), s->blocks.size()).c_str());
}

static void CmdEditBlock(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    if (i < 0 || (size_t)i >= s->blocks.size()) return;

    if (!EditBlockParams(dlg, s->blocks[(size_t)i])) return;

    CommitEdit(dlg, id);
    SelectBlock(dlg, i);
}

// Apply, from inside a block's parameter dialog. The same commit closing it
// would do: saved, pushed into the running graph, and both windows brought up
// to date. The dialog is modal and sits on top of this one, so the list is
// rebuilt behind it and the selection put back, ready for when it does close.
bool ApplyBlockEdit(HWND dlg)
{
    const int id = SessionIdOf(dlg);
    if (!g_store.ById(id)) return false;

    const int sel = SelectedBlock(dlg);

    std::wstring err;
    bool ok = g_store.Save(&err);
    if (!ok) ErrorBox(dlg, err);
    RefreshItemHotkeys(g_mainDlg);
    if (!ApplyLiveEdit(dlg, id)) ok = false;
    RefreshSessionWindow(id);
    RefreshMainList();
    if (sel >= 0) SelectBlock(dlg, sel);
    return ok;
}

// Space in the list, or the Enabled menu item: bypass the block without
// removing it from the pipeline.
static void SetBlockEnabled(HWND dlg, int i, bool on)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s || i < 0 || (size_t)i >= s->blocks.size()) return;
    if (s->blocks[(size_t)i].enabled == on) return;

    s->blocks[(size_t)i].enabled = on;
    CommitEdit(dlg, id);
    SelectBlock(dlg, i);
}

static void CmdToggleBlock(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    int i = SelectedBlock(dlg);
    if (!s || i < 0 || (size_t)i >= s->blocks.size()) return;
    SetBlockEnabled(dlg, i, !s->blocks[(size_t)i].enabled);
}

// The hotkeys for this block's actions. Only some blocks have any, and the
// menu item is disabled for the rest rather than opening an empty dialog.
static void CmdBlockActions(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    if (i < 0 || (size_t)i >= s->blocks.size()) return;
    if (BlockActionsFor(s->blocks[(size_t)i].typeId).empty()) return;

    if (!EditBlockActions(dlg, s->blocks[(size_t)i])) return;
    CommitEdit(dlg, id);     // which also re-registers everything
    SelectBlock(dlg, i);
}

// A hotkey that switches this block on and off, from anywhere or from here.
static void CmdBlockHotkey(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    if (i < 0 || (size_t)i >= s->blocks.size()) return;

    Block& b = s->blocks[(size_t)i];
    Hotkey want = b.hotkey;
    if (!EditHotkey(dlg, FormatW(L"the %s block in %s",
                                 b.TypeName().c_str(), s->name.c_str()),
                    want, HF_BLOCK))
        return;

    b.hotkey = want;
    CommitEdit(dlg, id);     // which also re-registers everything
    SelectBlock(dlg, i);
}

static void CmdRemoveBlock(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    if (i < 0 || (size_t)i >= s->blocks.size()) return;

    s->blocks.erase(s->blocks.begin() + i);
    CommitEdit(dlg, id);
    SelectBlock(dlg, i);
}

// Alt+Up / Alt+Down on the block list: nudge the focused block's volume
// without opening its parameters. Ctrl is already taken by reordering, so Alt
// is the free modifier, and the step is coarse enough to be worth a keystroke.
static void CmdNudgeVolume(HWND dlg, int delta)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    if (i < 0 || (size_t)i >= s->blocks.size()) return;

    Block& b = s->blocks[(size_t)i];
    // Not every block has a volume: an effect's strength is its own control,
    // and saying so is better than silently doing nothing.
    if (b.Kind() == BlockKind::Effect && b.typeId != BT_EFFECT_GAIN)
    {
        SetDlgItemTextW(dlg, IDC_SESSION_STATE,
                        FormatW(L"%s has no volume. Press Enter to edit it.",
                                b.TypeName().c_str()).c_str());
        return;
    }

    int v = b.volumePct + delta;
    if (v < 0) v = 0;
    if (v > 150) v = 150;
    if (v == b.volumePct)
    {
        SetDlgItemTextW(dlg, IDC_SESSION_STATE,
                        FormatW(L"%s volume %d%%, already at the %s.",
                                b.TypeName().c_str(), v,
                                delta > 0 ? L"maximum" : L"minimum").c_str());
        return;
    }
    b.volumePct = v;

    CommitEdit(dlg, id);
    SelectBlock(dlg, i);
    // CommitEdit has just redrawn the status line with the session state, so
    // the reading goes on last. This is the line a screen reader announces.
    SetDlgItemTextW(dlg, IDC_SESSION_STATE,
                    FormatW(L"%s volume %d%%.", b.TypeName().c_str(), v).c_str());
}

static void CmdMoveBlock(HWND dlg, int delta)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    int j = i + delta;
    if (i < 0 || (size_t)i >= s->blocks.size()) return;
    if (j < 0 || (size_t)j >= s->blocks.size()) return;

    std::swap(s->blocks[(size_t)i], s->blocks[(size_t)j]);
    CommitEdit(dlg, id);
    SetFocus(GetDlgItem(dlg, IDC_BLOCK_LIST));
    SelectBlock(dlg, j);
}

// Spoken, because there is nothing on screen that would say a block had been
// copied. Written last so it is not overwritten by the session state line.
static void SayInStatus(HWND dlg, const std::wstring& text)
{
    SetDlgItemTextW(dlg, IDC_SESSION_STATE, text.c_str());
}

static std::wstring NameOf(const Block& b)
{
    return b.label.empty() ? b.TypeName() : b.label;
}

// Ctrl+C, Ctrl+X and Ctrl+V are accelerators, so they are taken before any
// control in the window sees them - including the times field, which is a
// read-only edit precisely so that it can be read and copied from. So a
// clipboard key only means a block when the block list has the focus; anywhere
// else it is handed on as the ordinary editing command it looks like.
//
// Only for the key, not for the menu item beside it: choosing Copy from the
// Block menu means the block whatever has the focus, while pressing Ctrl+C
// means whatever the focus is on. WM_COMMAND tells them apart - an
// accelerator sends 1 in the high word and a menu sends 0.
//
// Returns true when it has been dealt with elsewhere and there is nothing for
// the block commands to do.
static bool PassClipboardKeyOn(HWND dlg, WPARAM wp, UINT msg)
{
    if (HIWORD(wp) != 1) return false;              // the menu item
    HWND focus = GetFocus();
    if (focus == GetDlgItem(dlg, IDC_BLOCK_LIST)) return false;
    if (focus && IsChild(dlg, focus)) SendMessageW(focus, msg, 0, 0);
    return true;
}

static void CmdCopyBlock(HWND dlg, bool cut)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    int i = SelectedBlock(dlg);
    if (i < 0 || (size_t)i >= s->blocks.size())
    {
        SayInStatus(dlg, L"No block is selected.");
        return;
    }

    g_clip = s->blocks[(size_t)i];
    g_clipHas = true;
    const std::wstring name = NameOf(g_clip);

    if (!cut)
    {
        SayInStatus(dlg, FormatW(L"%s copied. Ctrl+V pastes it below the "
                                 L"selected block, Ctrl+Shift+V above it.",
                                 name.c_str()));
        return;
    }

    s->blocks.erase(s->blocks.begin() + i);
    CommitEdit(dlg, id);
    SelectBlock(dlg, i);
    SayInStatus(dlg, FormatW(L"%s cut. Ctrl+V pastes it below the selected "
                             L"block, Ctrl+Shift+V above it.", name.c_str()));
}

static void CmdPasteBlock(HWND dlg, bool above)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    if (!g_clipHas)
    {
        SayInStatus(dlg, L"Nothing has been copied. Select a block and press "
                         L"Ctrl+C or Ctrl+X first.");
        return;
    }

    Block b = g_clip;
    const int cleared = ClearClashingHotkeys(*s, b);
    const std::wstring name = NameOf(b);

    // With nothing selected a paste goes on the end, which is where a block
    // added from the library goes too.
    const size_t at = PasteIndexFor(s->blocks.size(), SelectedBlock(dlg), above);

    s->blocks.insert(s->blocks.begin() + at, b);
    CommitEdit(dlg, id);
    SetFocus(GetDlgItem(dlg, IDC_BLOCK_LIST));
    SelectBlock(dlg, (int)at);

    std::wstring said = FormatW(L"%s pasted at position %zu of %zu.",
                                name.c_str(), at + 1, s->blocks.size());
    if (cleared == 1)
        said += L"  Its hotkey was cleared, because this session already uses it.";
    else if (cleared > 1)
        said += FormatW(L"  %d of its hotkeys were cleared, because this "
                        L"session already uses them.", cleared);
    SayInStatus(dlg, said);
}

static void CmdRun(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;

    for (const Block& b : s->blocks)
    {
        if (!b.IsConfigured())
        {
            ErrorBox(dlg, FormatW(L"The %s block has no application selected. "
                                  L"Press Enter on it to set its parameters.",
                                  b.TypeName().c_str()));
            return;
        }
    }
    StartSession(dlg, id);
}

// The same per-session flag the main window exposes, so it can be set from
// wherever the session is being worked on.
static void CmdAutoRun(HWND dlg)
{
    int id = SessionIdOf(dlg);
    Session* s = g_store.ById(id);
    if (!s) return;
    s->autoRun = !s->autoRun;
    std::wstring err;
    if (!g_store.Save(&err)) ErrorBox(dlg, err);
    RefreshSessionWindow(id);
    RefreshMainList();
}

// Ctrl+R: run this session, or stop it if it is already running.
static void CmdToggle(HWND dlg)
{
    int id = SessionIdOf(dlg);
    if (SessionRunning(id)) StopSession(id);
    else                    CmdRun(dlg);
}

// ---------------------------------------------------------------------------

static INT_PTR CALLBACK SessionDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);   // session id

        HWND list = GetDlgItem(dlg, IDC_BLOCK_LIST);
        // Real list-view checkboxes: Space toggles them, and screen readers
        // announce the checked state as part of the row.
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES,
                     LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        AddCol(list, 0, L"Block", 100);
        AddCol(list, 1, L"Kind", 55);
        AddCol(list, 2, L"Parameters", 230);
        AddCol(list, 3, L"Hotkey", 100);

        ListKeys listKeys;
        listKeys.onEnter = IDM_BLOCK_EDIT;
        listKeys.onDelete = IDM_BLOCK_REMOVE;
        SubclassListKeys(list, listKeys);

        BuildLibraryTree(dlg);
        ListKeys treeKeys;
        treeKeys.onEnter = IDM_BLOCK_ADD;
        SubclassListKeys(GetDlgItem(dlg, IDC_LIB_TREE), treeKeys);

        RefreshSessionWindow((int)lp);
        SetTimer(dlg, 1, 750, nullptr);
        SetFocus(list);
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_BLOCK_ADD:    CmdAddBlock(dlg);    return TRUE;
        case IDM_BLOCK_EDIT:   CmdEditBlock(dlg);   return TRUE;
        case IDM_BLOCK_REMOVE: CmdRemoveBlock(dlg); return TRUE;
        case IDM_BLOCK_TOGGLE: CmdToggleBlock(dlg); return TRUE;
        case IDM_BLOCK_UP:     CmdMoveBlock(dlg, -1); return TRUE;
        case IDM_BLOCK_DOWN:   CmdMoveBlock(dlg, +1); return TRUE;
        case IDM_BLOCK_CUT:
            if (!PassClipboardKeyOn(dlg, wp, WM_CUT)) CmdCopyBlock(dlg, true);
            return TRUE;
        case IDM_BLOCK_COPY:
            if (!PassClipboardKeyOn(dlg, wp, WM_COPY)) CmdCopyBlock(dlg, false);
            return TRUE;
        case IDM_BLOCK_PASTE:
            if (!PassClipboardKeyOn(dlg, wp, WM_PASTE)) CmdPasteBlock(dlg, false);
            return TRUE;
        case IDM_BLOCK_PASTE_ABOVE:
            if (!PassClipboardKeyOn(dlg, wp, WM_PASTE)) CmdPasteBlock(dlg, true);
            return TRUE;
        case IDM_BLOCK_VOL_UP:   CmdNudgeVolume(dlg, +5); return TRUE;
        case IDM_BLOCK_VOL_DOWN: CmdNudgeVolume(dlg, -5); return TRUE;
        case IDM_BLOCK_HOTKEY: CmdBlockHotkey(dlg); return TRUE;
        case IDM_BLOCK_RENAME: CmdRenameBlock(dlg); return TRUE;
        case IDM_BLOCK_ACTIONS: CmdBlockActions(dlg); return TRUE;

        case IDM_SESS_RUN:     CmdRun(dlg);         return TRUE;
        case IDM_SESS_STOP:    StopSession(SessionIdOf(dlg)); return TRUE;
        case IDM_SESS_TOGGLE:  CmdToggle(dlg);      return TRUE;
        case IDM_SESSION_AUTORUN: CmdAutoRun(dlg);   return TRUE;
        case IDM_SESSION_EXPORT:
        {
            // The session this window is showing, not whatever the main
            // window happens to have selected.
            Session* s = g_store.ById(SessionIdOf(dlg));
            if (s) ExportSession(dlg, *s);
            return TRUE;
        }
        case IDM_SESSION_OPENDIR:
            // The selected block, so that a session with several recorders
            // opens the one being looked at rather than always the first.
            OpenRecordFolder(dlg, SessionIdOf(dlg), SelectedBlock(dlg));
            return TRUE;

        case IDM_APP_HIDE:
            HideToTray(g_mainDlg);
            return TRUE;

        case IDM_APP_SETTINGS:
            ShowSettingsDialog(dlg);
            return TRUE;

        case IDM_SESS_CLOSE:
        case IDCANCEL:
            SendMessageW(dlg, WM_CLOSE, 0, 0);
            return TRUE;
        }
        break;

    case WM_INITMENUPOPUP:
    {
        Session* s = g_store.ById(SessionIdOf(dlg));
        CheckMenuItem(reinterpret_cast<HMENU>(wp), IDM_SESSION_AUTORUN,
                      MF_BYCOMMAND | ((s && s->autoRun) ? MF_CHECKED : MF_UNCHECKED));
        break;
    }

    case WM_CONTEXTMENU:
    {
        HWND ctl = reinterpret_cast<HWND>(wp);
        HWND list = GetDlgItem(dlg, IDC_BLOCK_LIST);
        HWND tree = GetDlgItem(dlg, IDC_LIB_TREE);
        int  id = SessionIdOf(dlg);
        Session* s = g_store.ById(id);
        if (!s) break;

        bool running = SessionRunning(id);
        std::map<UINT, bool> enabled;
        enabled[IDM_SESS_RUN]  = !running;
        enabled[IDM_SESS_STOP] = running;
        enabled[IDM_SESSION_AUTORUN] = true;

        std::map<UINT, bool> checked;
        checked[IDM_SESSION_AUTORUN] = s->autoRun;

        if (ctl == list)
        {
            int i = SelectedBlock(dlg);
            bool has = i >= 0 && (size_t)i < s->blocks.size();
            enabled[IDM_BLOCK_EDIT]   = has;
            enabled[IDM_BLOCK_TOGGLE] = has;
            enabled[IDM_BLOCK_REMOVE] = has;
            enabled[IDM_BLOCK_UP]     = has && i > 0;
            enabled[IDM_BLOCK_DOWN]   = has && (size_t)(i + 1) < s->blocks.size();
            enabled[IDM_BLOCK_VOL_UP]   = has;
            enabled[IDM_BLOCK_VOL_DOWN] = has;
            enabled[IDM_BLOCK_CUT]      = has;
            enabled[IDM_BLOCK_COPY]     = has;
            // Pasting needs something copied, but not a selection: with none
            // it goes on the end.
            enabled[IDM_BLOCK_PASTE]       = g_clipHas;
            enabled[IDM_BLOCK_PASTE_ABOVE] = g_clipHas;
            enabled[IDM_BLOCK_HOTKEY]   = has;
            enabled[IDM_BLOCK_RENAME]   = has;
            // Most blocks have no actions, so the item is there but dead
            // rather than appearing and disappearing as the selection moves.
            enabled[IDM_BLOCK_ACTIONS]  =
                has && !BlockActionsFor(s->blocks[(size_t)i].typeId).empty();
            ShowContextMenu(dlg, IDR_BLOCK_CTX, CtxMenuPoint(ctl, lp),
                            has ? IDM_BLOCK_EDIT : 0, enabled, &checked);
            return TRUE;
        }
        if (ctl == tree)
        {
            enabled[IDM_BLOCK_ADD] = true;
            ShowContextMenu(dlg, IDR_LIB_CTX, CtxMenuPoint(ctl, lp),
                            IDM_BLOCK_ADD, enabled, &checked);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom == IDC_BLOCK_LIST && hdr->code == LVN_ITEMCHANGED && !g_syncingChecks)
        {
            NMLISTVIEW* lv = reinterpret_cast<NMLISTVIEW*>(lp);
            UINT was = (lv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
            UINT now = (lv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
            // State image 0 means "no checkbox yet", i.e. the row is being
            // inserted rather than toggled by the user.
            if (was != 0 && now != 0 && was != now)
            {
                SetBlockEnabled(dlg, lv->iItem, now == 2);
                return TRUE;
            }
            break;
        }
        if (hdr->idFrom == IDC_BLOCK_LIST && hdr->code == NM_DBLCLK)
        {
            CmdEditBlock(dlg);
            return TRUE;
        }
        // A group opened or shut, however it was done. The arrow keys are
        // the way a keyboard gets at this and they do not come through the
        // Enter handler, so the notification is what to listen to.
        if (hdr->idFrom == IDC_LIB_TREE && hdr->code == TVN_ITEMEXPANDEDW)
        {
            RememberLibraryGroups(dlg);
            return 0;
        }
        if (hdr->idFrom == IDC_LIB_TREE && hdr->code == NM_DBLCLK)
        {
            CmdAddBlock(dlg);
            return TRUE;
        }
        break;
    }

    case WM_TIMER:
    {
        // Refresh the latency reading, but leave any one-off message (such as
        // "Added ...") on screen until the next real state change.
        wchar_t cur[256] = { 0 };
        GetDlgItemTextW(dlg, IDC_SESSION_STATE, cur, 256);
        if (wcsncmp(cur, L"Running", 7) == 0 || wcsncmp(cur, L"Stopped", 7) == 0)
            SetDlgItemTextW(dlg, IDC_SESSION_STATE,
                            SessionStateLine(SessionIdOf(dlg)).c_str());

        // The times always refresh: a clock that stopped because a one off
        // message is on screen would be worse than useless. Only written when
        // it has actually changed, so nothing is announced twice a second for
        // saying the same thing.
        {
            const std::wstring want = SessionTimesText(SessionIdOf(dlg));
            wchar_t had[256] = { 0 };
            GetDlgItemTextW(dlg, IDC_SESSION_TIMES, had, 256);
            if (want != had) SetDlgItemTextW(dlg, IDC_SESSION_TIMES, want.c_str());
        }
        return TRUE;
    }

    case WM_CLOSE:
        DestroyWindow(dlg);
        return TRUE;

    case WM_DESTROY:
    {
        KillTimer(dlg, 1);
        int id = SessionIdOf(dlg);
        g_windows.erase(id);
        std::wstring err;
        if (!g_store.Save(&err)) ErrorBox(nullptr, err);
        RefreshMainList();
        return TRUE;
    }
    }
    return FALSE;
}

void OpenSessionWindow(int sessionId)
{
    HWND existing = SessionWindowFor(sessionId);
    if (existing)
    {
        SetForegroundWindow(existing);
        SetFocus(GetDlgItem(existing, IDC_BLOCK_LIST));
        return;
    }
    if (!g_store.ById(sessionId)) return;

    if (!g_sessionAccel)
        g_sessionAccel = LoadAcceleratorsW(g_inst, MAKEINTRESOURCEW(IDR_SESSION_MENU));

    HWND dlg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SESSION), g_mainDlg,
                                  SessionDlgProc, (LPARAM)sessionId);
    if (!dlg)
    {
        ErrorBox(g_mainDlg, L"The session window could not be created.");
        return;
    }
    g_windows[sessionId] = dlg;
    RefreshSessionWindow(sessionId);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
}
