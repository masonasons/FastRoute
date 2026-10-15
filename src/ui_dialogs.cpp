#include "app.h"
#include "audio/net.h"
#include "updater.h"
#include "audio/netcodec.h"
#include "audio/asiodev.h"
#include "plugins/plugins.h"
#include <thread>
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <vector>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

// ---------------------------------------------------------------------------
// Shared control helpers
// ---------------------------------------------------------------------------

struct KeyData { ListKeys keys; };

static LRESULT CALLBACK KeySubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref)
{
    KeyData* kd = reinterpret_cast<KeyData*>(ref);

    switch (msg)
    {
    case WM_GETDLGCODE:
        // Claim Enter and Delete for ourselves; the dialog manager would
        // otherwise turn Enter into a default-button press.
        if (lp)
        {
            MSG* m = reinterpret_cast<MSG*>(lp);
            if (m->message == WM_KEYDOWN &&
                ((m->wParam == VK_RETURN && kd->keys.onEnter) ||
                 (m->wParam == VK_DELETE && kd->keys.onDelete)))
                return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTMESSAGE;
        }
        break;

    case WM_KEYDOWN:
        if (wp == VK_RETURN && kd->keys.onEnter)
        {
            PostMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(kd->keys.onEnter, 0), 0);
            return 0;
        }
        if (wp == VK_DELETE && kd->keys.onDelete)
        {
            PostMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(kd->keys.onDelete, 0), 0);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, KeySubclass, id);
        delete kd;
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void SubclassListKeys(HWND ctl, const ListKeys& keys)
{
    KeyData* kd = new KeyData();
    kd->keys = keys;
    SetWindowSubclass(ctl, KeySubclass, 1, reinterpret_cast<DWORD_PTR>(kd));
}

void SetupSlider(HWND dlg, int sliderId, int textId, int lo, int hi, int value,
                 const wchar_t* suffix)
{
    HWND s = GetDlgItem(dlg, sliderId);
    SendMessageW(s, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(s, TBM_SETTICFREQ, (hi - lo) / 6 + 1, 0);
    SendMessageW(s, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(s, TBM_SETPOS, TRUE, value);
    SetSliderReadout(dlg, sliderId, textId, FormatW(L"%d%s", value, suffix));
}

int ReadSlider(HWND dlg, int sliderId)
{
    return (int)SendMessageW(GetDlgItem(dlg, sliderId), TBM_GETPOS, 0, 0);
}

void UpdateSliderText(HWND dlg, int sliderId, int textId, const wchar_t* suffix)
{
    SetSliderReadout(dlg, sliderId, textId,
                     FormatW(L"%d%s", ReadSlider(dlg, sliderId), suffix));
}

void SetupVolumeSlider(HWND dlg, int value)
{
    SetupSlider(dlg, IDC_VOL_SLIDER, IDC_VOL_TEXT, 0, 150, value, L"%");
}

int ReadVolumeSlider(HWND dlg)
{
    return ReadSlider(dlg, IDC_VOL_SLIDER);
}

// The dB sliders run from -limit to +limit with 0 in the middle, so the
// readout carries an explicit sign: "0 dB" and "-0 dB" would otherwise be
// indistinguishable from "+3 dB" at a glance, and identical to a screen
// reader reading the row.
static void SetDbText(HWND dlg, int sliderId, int textId)
{
    SetSliderReadout(dlg, sliderId, textId,
                     FormatW(L"%+d dB", ReadSlider(dlg, sliderId)));
}

static void SetupDbSlider(HWND dlg, int sliderId, int textId, int limit, int value)
{
    HWND s = GetDlgItem(dlg, sliderId);
    // TBM_SETRANGE packs both ends into one LPARAM, which cannot carry a
    // negative minimum cleanly; these two take the value whole.
    SendMessageW(s, TBM_SETRANGEMIN, FALSE, -limit);
    SendMessageW(s, TBM_SETRANGEMAX, TRUE, limit);
    SendMessageW(s, TBM_SETTICFREQ, 3, 0);
    SendMessageW(s, TBM_SETPAGESIZE, 0, 3);
    if (value < -limit) value = -limit;
    if (value > limit) value = limit;
    SendMessageW(s, TBM_SETPOS, TRUE, value);
    SetDbText(dlg, sliderId, textId);
}

// A slider whose range is not symmetric about zero. TBM_SETRANGE packs both
// ends into one LPARAM, which cannot carry a negative minimum, so these go in
// one at a time - the same reason SetupDbSlider does it.
static void SetupRange(HWND dlg, int sliderId, int textId, int lo, int hi,
                       int value, const wchar_t* suffix, int page)
{
    HWND s = GetDlgItem(dlg, sliderId);
    SendMessageW(s, TBM_SETRANGEMIN, FALSE, lo);
    SendMessageW(s, TBM_SETRANGEMAX, TRUE, hi);
    SendMessageW(s, TBM_SETTICFREQ, (hi - lo) / 6 + 1, 0);
    SendMessageW(s, TBM_SETPAGESIZE, 0, page);
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    SendMessageW(s, TBM_SETPOS, TRUE, value);
    SetSliderReadout(dlg, sliderId, textId, FormatW(L"%d%s", value, suffix));
}

static void OnVolumeScroll(HWND dlg)
{
    UpdateSliderText(dlg, IDC_VOL_SLIDER, IDC_VOL_TEXT, L"%");
}

// ---------------------------------------------------------------------------
// Name prompt
// ---------------------------------------------------------------------------

struct NameCtx
{
    const wchar_t* caption;
    const wchar_t* label;
    std::wstring   value;
};

// ---------------------------------------------------------------------------
// OK, Apply and Cancel on a block's parameters
//
// Every parameter dialog collects its fields in one place, the IDOK branch,
// and Apply runs that same branch. What differs is only what happens at the
// end of it: OK closes, Apply pushes the block into the running session and
// stays where it is. There is deliberately no second copy of the reading that
// could drift out of step with the first.
//
// Applied changes stick. Cancel afterwards undoes what was done since the last
// Apply, not the Apply itself. That is what these three buttons mean in every
// other Windows dialog, and it is the only reading under which Apply is worth
// having: the point of it is to hear a change against live audio and keep the
// ones that were right.
//
// And it beeps. The change is meant to be heard, but a setting that was
// already where you wanted it makes no audible difference, and a button that
// might have done nothing is indistinguishable from one that is broken.
// ---------------------------------------------------------------------------

// The block a dialog is editing in place, and the copy its Cancel would put
// back. Only the parametric EQ works that way - everything else leaves the
// block alone until its OK branch runs - and it is set only for as long as
// that one modal dialog is up.
static Block* g_editLive = nullptr;
static Block* g_editSaved = nullptr;

void FinishBlockEdit(HWND dlg, int cmd)
{
    if (cmd != IDC_APPLY)
    {
        EndDialog(dlg, IDOK);
        return;
    }

    // What has been applied is no longer something Cancel can take back.
    if (g_editLive && g_editSaved) *g_editSaved = *g_editLive;

    if (ApplyBlockEdit(GetParent(dlg))) MessageBeep(MB_OK);
    SetFocus(GetDlgItem(dlg, IDC_APPLY));
}

static INT_PTR CALLBACK NameDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    NameCtx* ctx = reinterpret_cast<NameCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ctx = reinterpret_cast<NameCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetWindowTextW(dlg, ctx->caption);
        SetDlgItemTextW(dlg, IDC_NAME_LABEL, ctx->label);
        SetDlgItemTextW(dlg, IDC_NAME_EDIT, ctx->value.c_str());
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETSEL, 0, -1);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            wchar_t buf[256] = { 0 };
            GetDlgItemTextW(dlg, IDC_NAME_EDIT, buf, 256);
            std::wstring v = Trim(buf);
            if (v.empty())
            {
                ErrorBox(dlg, L"Please type a name.");
                SetFocus(GetDlgItem(dlg, IDC_NAME_EDIT));
                return TRUE;
            }
            ctx->value = v;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

bool PromptForName(HWND parent, const wchar_t* caption, const wchar_t* label,
                   std::wstring& name)
{
    NameCtx ctx{ caption, label, name };
    INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_NAME), parent,
                                NameDlgProc, reinterpret_cast<LPARAM>(&ctx));
    if (r != IDOK) return false;
    name = ctx.value;
    return true;
}

// ---------------------------------------------------------------------------
// Application input block
// ---------------------------------------------------------------------------

struct AppCtx
{
    Block* block;
    std::vector<AppCandidate> apps;     // one per row, in row order
};

// A row per program: what it is called, what the person using it calls it, and
// whether it is playing, merely running, or neither. The state is spelled out
// rather than left to the order of the rows, because the order of the rows is
// not something a screen reader conveys.
static void FillAppList(HWND dlg, AppCtx* ctx, const std::vector<std::wstring>& ticked)
{
    HWND list = GetDlgItem(dlg, IDC_APP_LIST);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
    ctx->apps = AppCandidates(ticked,
                              IsDlgButtonChecked(dlg, IDC_APP_ALL) == BST_CHECKED);

    for (size_t i = 0; i < ctx->apps.size(); ++i)
    {
        const AppCandidate& a = ctx->apps[i];
        std::wstring label = a.exeName;
        if (!a.displayName.empty() && !IEquals(a.displayName, a.exeName))
        {
            // A window title can be a whole sentence, and the part that says
            // which program this is comes at the start of it.
            std::wstring title = a.displayName;
            if (title.size() > 60) title = title.substr(0, 57) + L"...";
            label += L" (" + title + L")";
        }
        label += a.playing ? L" - playing audio"
               : a.running ? L" - running"
                           : L" - not running";

        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(label.c_str());
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);

        bool on = false;
        for (const std::wstring& t : ticked)
            if (IEquals(t, a.exeName)) on = true;
        ListView_SetCheckState(list, (int)i, on ? TRUE : FALSE);
    }
}

// The ticked names, in the order the rows are in.
static std::vector<std::wstring> ReadAppList(HWND dlg, AppCtx* ctx)
{
    HWND list = GetDlgItem(dlg, IDC_APP_LIST);
    std::vector<std::wstring> out;
    for (size_t i = 0; i < ctx->apps.size(); ++i)
        if (ListView_GetCheckState(list, (int)i))
            out.push_back(ctx->apps[i].exeName);
    return out;
}

// The application list only means anything when the block is pointed at a
// name. In the other two modes it is left unavailable rather than hidden, so
// tabbing past it still says the choice is there and why it is not in use.
static int AppModeOf(HWND dlg)
{
    if (IsDlgButtonChecked(dlg, IDC_APP_MODE_FOLLOW) == BST_CHECKED) return AS_FOLLOW;
    if (IsDlgButtonChecked(dlg, IDC_APP_MODE_FRONT) == BST_CHECKED) return AS_FOREGROUND;
    return AS_NAMED;
}

static void AppModeState(HWND dlg)
{
    const bool named = AppModeOf(dlg) == AS_NAMED;
    EnableWindow(GetDlgItem(dlg, IDC_APP_LIST), named);
    EnableWindow(GetDlgItem(dlg, IDC_APP_LABEL), named);
    EnableWindow(GetDlgItem(dlg, IDC_APP_ADD), named);
    EnableWindow(GetDlgItem(dlg, IDC_APP_ALL), named);
    EnableWindow(GetDlgItem(dlg, IDC_APP_WAIT), named);
    EnableWindow(GetDlgItem(dlg, IDC_BTN_REFRESH), named);
}

// How many are ticked, said plainly, because a list of checkboxes does not
// tell you its own total and more than one is the point of the thing.
static void AppCountNote(HWND dlg, AppCtx* ctx)
{
    const size_t n = ReadAppList(dlg, ctx).size();
    std::wstring note;
    if (AppModeOf(dlg) != AS_NAMED)
        note = L"This block goes by whatever is in front, so the list above is "
               L"not used. A block that goes by what is in front ignores the "
               L"Start menu, the task switcher and the desktop, and never "
               L"captures FastRoute itself.";
    else if (n == 0)
        note = L"Nothing ticked. The list is what is playing now, then what has "
               L"a window on screen. For a program with neither, tick Show "
               L"every running program; for one that is not started yet, use "
               L"Add Application and type its name.";
    else if (n == 1)
        note = IsDlgButtonChecked(dlg, IDC_APP_WAIT) == BST_CHECKED
            ? L"One application, captured with its child processes. The session "
              L"will run whether or not it is started yet, and will pick it up "
              L"when it launches."
            : L"One application, captured with its child processes.";
    else
        note = FormatW(L"%zu applications, each captured with its child "
                       L"processes and mixed together in this one block. One "
                       L"of them not running is not a problem: it joins the "
                       L"mix when it starts.%s", n,
                       IsDlgButtonChecked(dlg, IDC_APP_WAIT) == BST_CHECKED
                           ? L" Nor is none of them running, since this block "
                             L"is set to wait."
                           : L"");
    SetDlgItemTextW(dlg, IDC_APP_NOTE, note.c_str());
}

static INT_PTR CALLBACK AppDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    AppCtx* ctx = reinterpret_cast<AppCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        ctx = reinterpret_cast<AppCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        {
            HWND list = GetDlgItem(dlg, IDC_APP_LIST);
            SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                         LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES,
                         LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = const_cast<wchar_t*>(L"Application");
            col.cx = 260;
            SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        }
        FillAppList(dlg, ctx, AppTargets(*ctx->block));
        CheckDlgButton(dlg, IDC_APP_WAIT,
                       ctx->block->appWaitForStart ? BST_CHECKED : BST_UNCHECKED);
        CheckRadioButton(dlg, IDC_APP_MODE_NAMED, IDC_APP_MODE_FOLLOW,
                         ctx->block->appSource == AS_FOLLOW     ? IDC_APP_MODE_FOLLOW :
                         ctx->block->appSource == AS_FOREGROUND ? IDC_APP_MODE_FRONT
                                                                : IDC_APP_MODE_NAMED);
        AppModeState(dlg);
        AppCountNote(dlg, ctx);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, ctx->block->enabled ? BST_CHECKED : BST_UNCHECKED);
        SetupVolumeSlider(dlg, ctx->block->volumePct);
        return TRUE;

    case WM_HSCROLL:
        OnVolumeScroll(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_APP_MODE_NAMED:
        case IDC_APP_MODE_FRONT:
        case IDC_APP_MODE_FOLLOW:
            AppModeState(dlg);
            AppCountNote(dlg, ctx);
            return TRUE;

        case IDC_APP_ADD:
        {
            // For something that is not playing anything yet, which is the
            // ordinary case for a games block: none of them is running when
            // the session is being put together.
            std::wstring name;
            if (!PromptForName(dlg, L"Add Application",
                               L"Program &file name, such as chrome.exe:", name))
                return TRUE;
            std::vector<std::wstring> keep = ReadAppList(dlg, ctx);
            bool dup = false;
            for (const std::wstring& e : keep) if (IEquals(e, name)) dup = true;
            if (!dup) keep.push_back(name);
            FillAppList(dlg, ctx, keep);
            AppCountNote(dlg, ctx);
            SetFocus(GetDlgItem(dlg, IDC_APP_LIST));
            return TRUE;
        }

        case IDC_BTN_REFRESH:
            // The ticks are what is kept, not the highlighted row: refreshing
            // must not quietly drop a choice because its program stopped.
            FillAppList(dlg, ctx, ReadAppList(dlg, ctx));
            AppCountNote(dlg, ctx);
            SetFocus(GetDlgItem(dlg, IDC_APP_LIST));
            return TRUE;

        case IDC_APP_WAIT:
            // Only the note changes: what to wait for is the list, and the
            // list is the same either way.
            AppCountNote(dlg, ctx);
            return TRUE;

        case IDC_APP_ALL:
            // Focus stays on the checkbox that was just toggled. The list is
            // next along, and being moved there instead would make it hard to
            // tell what the checkbox had done.
            FillAppList(dlg, ctx, ReadAppList(dlg, ctx));
            AppCountNote(dlg, ctx);
            return TRUE;
        case IDC_APPLY:
        case IDOK:
        {
            const int mode = AppModeOf(dlg);
            const std::vector<std::wstring> picked = ReadAppList(dlg, ctx);

            // Only a block that goes by name needs any. The other two work it
            // out for themselves, and refusing them here would mean an empty
            // list stopping a block that never reads it.
            if (picked.empty() && mode == AS_NAMED)
            {
                ErrorBox(dlg, L"No application is ticked. Tick one or more in the list, or use "
                              L"Add Application to name one that is not playing anything yet. "
                              L"You can also point this block at whatever is in front instead, "
                              L"which needs no list at all.");
                SetFocus(GetDlgItem(dlg, IDC_APP_LIST));
                return TRUE;
            }

            // Kept whichever mode is chosen, so switching back to the list
            // finds what was picked before. processName stays as the first of
            // them: it is what a FastRoute without this feature would read.
            if (!picked.empty())
            {
                ctx->block->appNames = picked;
                ctx->block->processName = picked[0];
                ctx->block->processId = 0;
            }
            ctx->block->appSource   = mode;
            ctx->block->appWaitForStart =
                IsDlgButtonChecked(dlg, IDC_APP_WAIT) == BST_CHECKED;
            ctx->block->enabled     = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            ctx->block->volumePct   = ReadVolumeSlider(dlg);
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Input / output device block
// ---------------------------------------------------------------------------

struct DevCtx
{
    Block* block;
    bool   render;
    std::vector<AudioEndpoint> endpoints;
};

static void FillDeviceCombo(HWND dlg, DevCtx* ctx, const std::wstring& keepId)
{
    HWND cb = GetDlgItem(dlg, IDC_DEV_COMBO);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);

    ctx->endpoints = EnumEndpoints(ctx->render);

    int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"System default");
    SendMessageW(cb, CB_SETITEMDATA, idx, (LPARAM)-1);
    int select = idx;

    for (size_t i = 0; i < ctx->endpoints.size(); ++i)
    {
        std::wstring label = ctx->endpoints[i].name;
        if (ctx->endpoints[i].isDefault) label += L" (current default)";
        int j = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        SendMessageW(cb, CB_SETITEMDATA, j, (LPARAM)i);
        if (!keepId.empty() && ctx->endpoints[i].id == keepId) select = j;
    }
    SendMessageW(cb, CB_SETCURSEL, select, 0);
}

static INT_PTR CALLBACK DevDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    DevCtx* ctx = reinterpret_cast<DevCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        ctx = reinterpret_cast<DevCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetWindowTextW(dlg, ctx->render ? L"Output Device Block" : L"Input Device Block");
        SetDlgItemTextW(dlg, IDC_DEV_LABEL,
                        ctx->render ? L"Output &device:" : L"Input &device:");
        FillDeviceCombo(dlg, ctx, ctx->block->deviceId);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, ctx->block->enabled ? BST_CHECKED : BST_UNCHECKED);
        SetupVolumeSlider(dlg, ctx->block->volumePct);
        return TRUE;

    case WM_HSCROLL:
        OnVolumeScroll(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_BTN_REFRESH:
            FillDeviceCombo(dlg, ctx, ctx->block->deviceId);
            SetFocus(GetDlgItem(dlg, IDC_DEV_COMBO));
            return TRUE;

        case IDC_APPLY:
        case IDOK:
        {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_DEV_COMBO, CB_GETCURSEL, 0, 0);
            LRESULT d = (sel >= 0)
                ? SendDlgItemMessageW(dlg, IDC_DEV_COMBO, CB_GETITEMDATA, sel, 0) : -1;
            if (d < 0 || (size_t)d >= ctx->endpoints.size())
            {
                ctx->block->deviceId.clear();
                ctx->block->deviceName = L"System default";
            }
            else
            {
                ctx->block->deviceId   = ctx->endpoints[(size_t)d].id;
                ctx->block->deviceName = ctx->endpoints[(size_t)d].name;
            }
            ctx->block->enabled   = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            ctx->block->volumePct = ReadVolumeSlider(dlg);
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Stream URL block
// ---------------------------------------------------------------------------

static void SyncStreamFields(HWND dlg)
{
    bool on = IsDlgButtonChecked(dlg, IDC_STR_RECONNECT) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_STR_RETRY), on);
    EnableWindow(GetDlgItem(dlg, IDC_STR_RETRY_LABEL), on);
    UpdateSliderText(dlg, IDC_STR_BUFFER, IDC_STR_BUFFER_TEXT, L" ms");
}

static INT_PTR CALLBACK StreamDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        SetDlgItemTextW(dlg, IDC_STR_URL, b->url.c_str());
        // Which formats work is not the same on every machine, so the dialog
        // says what this one can do rather than making a general promise.
        SetDlgItemTextW(dlg, IDC_STR_NOTE,
            MediaFoundationAvailable()
                ? L"Ogg Vorbis and Opus are decoded by FastRoute; MP3, AAC and "
                  L"FLAC by Windows, which decides for itself which of those it "
                  L"has. An http or https address."
                : L"This edition of Windows has no Media Foundation, so only Ogg "
                  L"Vorbis and Opus streams will play. An http or https address.");

        SetupSlider(dlg, IDC_STR_BUFFER, IDC_STR_BUFFER_TEXT, 200, 10000,
                    b->bufferMs, L" ms");
        SendDlgItemMessageW(dlg, IDC_STR_BUFFER, TBM_SETPAGESIZE, 0, 500);
        SendDlgItemMessageW(dlg, IDC_STR_BUFFER, TBM_SETTICFREQ, 1000, 0);

        CheckDlgButton(dlg, IDC_STR_RECONNECT, b->reconnect ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dlg, IDC_STR_RETRY, (UINT)b->retrySeconds, FALSE);

        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        SetupVolumeSlider(dlg, b->volumePct);
        SyncStreamFields(dlg);
        return TRUE;

    case WM_HSCROLL:
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_STR_BUFFER)
            UpdateSliderText(dlg, IDC_STR_BUFFER, IDC_STR_BUFFER_TEXT, L" ms");
        else
            OnVolumeScroll(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_STR_RECONNECT:
            SyncStreamFields(dlg);
            return TRUE;

        case IDC_APPLY:
        case IDOK:
        {
            wchar_t url[2048] = { 0 };
            GetDlgItemTextW(dlg, IDC_STR_URL, url, 2048);
            std::wstring want = Trim(url);
            if (want.empty())
            {
                ErrorBox(dlg, L"Type the address of the stream, starting with "
                              L"http:// or https://.");
                SetFocus(GetDlgItem(dlg, IDC_STR_URL));
                return TRUE;
            }
            if (_wcsnicmp(want.c_str(), L"http://", 7) != 0 &&
                _wcsnicmp(want.c_str(), L"https://", 8) != 0)
            {
                ErrorBox(dlg, L"FastRoute opens http:// and https:// addresses. "
                              L"For a file already on this machine, use a Recorder "
                              L"block's folder instead, or a player application.");
                SetFocus(GetDlgItem(dlg, IDC_STR_URL));
                return TRUE;
            }

            BOOL got = FALSE;
            int retry = (int)GetDlgItemInt(dlg, IDC_STR_RETRY, &got, FALSE);
            if (IsDlgButtonChecked(dlg, IDC_STR_RECONNECT) == BST_CHECKED &&
                (!got || retry < 1 || retry > 3600))
            {
                ErrorBox(dlg, L"Type how long to wait before trying again, from "
                              L"1 to 3600 seconds.");
                SetFocus(GetDlgItem(dlg, IDC_STR_RETRY));
                return TRUE;
            }

            b->url = want;
            b->bufferMs = ReadSlider(dlg, IDC_STR_BUFFER);
            b->reconnect = IsDlgButtonChecked(dlg, IDC_STR_RECONNECT) == BST_CHECKED;
            if (got) b->retrySeconds = retry;
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            b->volumePct = ReadVolumeSlider(dlg);
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// System audio block
//
// The exclusions are a real list-view with checkboxes rather than a text box
// of names: Space toggles a row, the checked state is part of what a screen
// reader reads out, and the applications currently making sound are already
// in the list to be picked from.
// ---------------------------------------------------------------------------

struct SysCtx
{
    Block* block;
    std::vector<AudioEndpoint> endpoints;
    std::vector<std::wstring> names;    // one per row, in row order
};

static void FillExcludeList(HWND dlg, SysCtx* ctx, const std::vector<std::wstring>& checked)
{
    HWND list = GetDlgItem(dlg, IDC_SYS_EXCLUDE);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
    ctx->names.clear();

    // Everything playing right now, plus any name already excluded that is not
    // - an application you excluded yesterday should not vanish off the list
    // because it happens not to be running today.
    for (const AudioProcess& p : EnumAudioProcesses())
    {
        bool dup = false;
        for (const std::wstring& n : ctx->names) if (IEquals(n, p.exeName)) dup = true;
        if (!dup && !p.exeName.empty()) ctx->names.push_back(p.exeName);
    }
    for (const std::wstring& e : checked)
    {
        bool dup = false;
        for (const std::wstring& n : ctx->names) if (IEquals(n, e)) dup = true;
        if (!dup) ctx->names.push_back(e);
    }

    for (size_t i = 0; i < ctx->names.size(); ++i)
    {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(ctx->names[i].c_str());
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);

        bool on = false;
        for (const std::wstring& e : checked) if (IEquals(e, ctx->names[i])) on = true;
        ListView_SetCheckState(list, (int)i, on ? TRUE : FALSE);
    }
}

static std::vector<std::wstring> ReadExcludeList(HWND dlg, SysCtx* ctx)
{
    HWND list = GetDlgItem(dlg, IDC_SYS_EXCLUDE);
    std::vector<std::wstring> out;
    for (size_t i = 0; i < ctx->names.size(); ++i)
        if (ListView_GetCheckState(list, (int)i)) out.push_back(ctx->names[i]);
    return out;
}

static INT_PTR CALLBACK SystemDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SysCtx* ctx = reinterpret_cast<SysCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        ctx = reinterpret_cast<SysCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        HWND list = GetDlgItem(dlg, IDC_SYS_EXCLUDE);
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES,
                     LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(L"Application");
        col.cx = 260;
        SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);

        DevCtx dev{ ctx->block, true, {} };
        FillDeviceCombo(dlg, &dev, ctx->block->deviceId);
        ctx->endpoints = dev.endpoints;

        FillExcludeList(dlg, ctx, ctx->block->excludeApps);

        // Excluding anything at all changes how the capture is built, and that
        // way needs a newer Windows. Say so before it is tried, not after.
        if (ProcessLoopbackAvailable())
            SetDlgItemTextW(dlg, IDC_SYS_NOTE,
                            L"With nothing excluded the device is captured whole. "
                            L"Excluding an application captures the others "
                            L"individually instead - the ones playing to this "
                            L"device, not everything on the machine. An "
                            L"application playing to two devices at once still "
                            L"arrives whole.");
        else
        {
            SetDlgItemTextW(dlg, IDC_SYS_NOTE,
                            L"Excluding applications needs Windows 10 version 2004 "
                            L"or later. This device can still be captured whole.");
            EnableWindow(list, FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_SYS_ADD), FALSE);
        }

        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, ctx->block->enabled ? BST_CHECKED : BST_UNCHECKED);
        SetupVolumeSlider(dlg, ctx->block->volumePct);
        return TRUE;
    }

    case WM_HSCROLL:
        OnVolumeScroll(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_BTN_REFRESH:
        {
            // Keep what is ticked: refreshing is for picking up an application
            // that has started since, not for starting again.
            std::vector<std::wstring> keep = ReadExcludeList(dlg, ctx);
            DevCtx dev{ ctx->block, true, {} };
            FillDeviceCombo(dlg, &dev, ctx->block->deviceId);
            ctx->endpoints = dev.endpoints;
            FillExcludeList(dlg, ctx, keep);
            SetFocus(GetDlgItem(dlg, IDC_SYS_EXCLUDE));
            return TRUE;
        }

        case IDC_SYS_ADD:
        {
            std::wstring name;
            if (!PromptForName(dlg, L"Exclude Application",
                               L"Program &file name, such as chrome.exe:", name))
                return TRUE;
            std::vector<std::wstring> keep = ReadExcludeList(dlg, ctx);
            bool dup = false;
            for (const std::wstring& e : keep) if (IEquals(e, name)) dup = true;
            if (!dup) keep.push_back(name);
            FillExcludeList(dlg, ctx, keep);
            SetFocus(GetDlgItem(dlg, IDC_SYS_EXCLUDE));
            return TRUE;
        }

        case IDC_APPLY:
        case IDOK:
        {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_DEV_COMBO, CB_GETCURSEL, 0, 0);
            LRESULT d = (sel >= 0)
                ? SendDlgItemMessageW(dlg, IDC_DEV_COMBO, CB_GETITEMDATA, sel, 0) : -1;
            if (d < 0 || (size_t)d >= ctx->endpoints.size())
            {
                ctx->block->deviceId.clear();
                ctx->block->deviceName = L"System default";
            }
            else
            {
                ctx->block->deviceId   = ctx->endpoints[(size_t)d].id;
                ctx->block->deviceName = ctx->endpoints[(size_t)d].name;
            }
            ctx->block->excludeApps = ReadExcludeList(dlg, ctx);
            ctx->block->enabled   = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            ctx->block->volumePct = ReadVolumeSlider(dlg);
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Gain effect block
// ---------------------------------------------------------------------------

static INT_PTR CALLBACK GainDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        SetupVolumeSlider(dlg, b->volumePct);
        return TRUE;

    case WM_HSCROLL:
        OnVolumeScroll(dlg);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->enabled   = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            b->volumePct = ReadVolumeSlider(dlg);
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Denoise blocks
// ---------------------------------------------------------------------------

static INT_PTR CALLBACK DenoiseDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        const bool speex = (b->typeId == BT_FX_SPEEX);
        SetWindowTextW(dlg, speex ? L"Speex Denoise Block" : L"RNNoise Denoise Block");

        // RNNoise has no strength control of its own: the model decides how
        // much to remove, so only the mix is meaningful there.
        if (!speex)
        {
            ShowWindow(GetDlgItem(dlg, IDC_SUP_LABEL), SW_HIDE);
            ShowWindow(GetDlgItem(dlg, IDC_SUP_SLIDER), SW_HIDE);
            ShowWindow(GetDlgItem(dlg, IDC_SUP_TEXT), SW_HIDE);
            EnableWindow(GetDlgItem(dlg, IDC_SUP_SLIDER), FALSE);
        }
        else
        {
            SetupSlider(dlg, IDC_SUP_SLIDER, IDC_SUP_TEXT, 3, 45, b->suppressDb, L" dB");
        }
        SetupSlider(dlg, IDC_MIX_SLIDER, IDC_MIX_TEXT, 0, 100, b->mixPct, L"%");
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_HSCROLL:
    {
        HWND ctl = reinterpret_cast<HWND>(lp);
        if (ctl == GetDlgItem(dlg, IDC_SUP_SLIDER))
            UpdateSliderText(dlg, IDC_SUP_SLIDER, IDC_SUP_TEXT, L" dB");
        else if (ctl == GetDlgItem(dlg, IDC_MIX_SLIDER))
            UpdateSliderText(dlg, IDC_MIX_SLIDER, IDC_MIX_TEXT, L"%");
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            if (b->typeId == BT_FX_SPEEX) b->suppressDb = ReadSlider(dlg, IDC_SUP_SLIDER);
            b->mixPct  = ReadSlider(dlg, IDC_MIX_SLIDER);
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Recorder block
// ---------------------------------------------------------------------------

static bool PickFolder(HWND owner, std::wstring& path,
                       const wchar_t* title = L"Choose where recordings are saved")
{
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))) || !dlg)
        return false;

    bool ok = false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);

    if (!path.empty())
    {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                  IID_PPV_ARGS(&start))) && start)
        {
            dlg->SetFolder(start);
            start->Release();
        }
    }

    if (SUCCEEDED(dlg->Show(owner)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item)
        {
            LPWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
            {
                path = p;
                CoTaskMemFree(p);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}


// Which formats this copy of Windows can actually encode. A format Windows
// has no encoder for is left out of the list entirely rather than offered and
// then refused at run time, so the combo index is not the stored value.
struct FormatChoice { int value; const wchar_t* label; };


static std::vector<FormatChoice> AvailableFormats()
{
    std::vector<FormatChoice> out;
    for (int i = 0; i < kRecFormatCount; ++i)
    {
        RecFormat f = (RecFormat)i;
        if (RecFormatAvailable(f)) out.push_back({ i, RecFormatName(f) });
    }
    return out;
}

static void FillRateAndDepth(HWND dlg, const Block* b);
static void FillBitrate(HWND dlg, int want);
static int SelectedRecRate(HWND dlg);
static int SelectedRecBits(HWND dlg);
static int SelectedBitrate(HWND dlg);

static int SelectedFormat(HWND dlg)
{
    int sel = (int)SendDlgItemMessageW(dlg, IDC_REC_FORMAT, CB_GETCURSEL, 0, 0);
    if (sel < 0) return 0;
    return (int)SendDlgItemMessageW(dlg, IDC_REC_FORMAT, CB_GETITEMDATA, sel, 0);
}

// The top of the hold slider is not a duration, so it does not read as one.
static void SetHoldText(HWND dlg)
{
    int v = ReadSlider(dlg, IDC_REC_HOLD_SLIDER);
    SetSliderReadout(dlg, IDC_REC_HOLD_SLIDER, IDC_REC_HOLD_TEXT,
                     v > kHoldMaxSec ? std::wstring(L"always") : FormatW(L"%d s", v));
}

// The folder, template, format and bitrate fields. The recorder and the buffer
// ask exactly the same question about where a file goes and what it is, so
// they ask it with the same controls and the same code.
static void FillFileFields(HWND dlg, const Block* b, const wchar_t* tokenHelp)
{
    SetDlgItemTextW(dlg, IDC_REC_FOLDER, b->folder.c_str());
    SetDlgItemTextW(dlg, IDC_REC_TEMPLATE,
                    b->nameTemplate.empty() ? L"{session} {date} {time}"
                                            : b->nameTemplate.c_str());
    SetDlgItemTextW(dlg, IDC_REC_TOKENS, tokenHelp);

    HWND fc = GetDlgItem(dlg, IDC_REC_FORMAT);
    std::vector<FormatChoice> formats = AvailableFormats();
    int fsel = 0;
    for (size_t i = 0; i < formats.size(); ++i)
    {
        int at = (int)SendMessageW(fc, CB_ADDSTRING, 0, (LPARAM)formats[i].label);
        SendMessageW(fc, CB_SETITEMDATA, at, (LPARAM)formats[i].value);
        if (formats[i].value == b->recFormat) fsel = at;
    }
    SendMessageW(fc, CB_SETCURSEL, fsel, 0);

    FillRateAndDepth(dlg, b);
    FillBitrate(dlg, b->bitrateKbps);
}

// The sample rate and bit depth, which depend on the format: Windows' FLAC
// encoder starts at 44100 and its MP3 encoder stops at 48000, and the lossy
// formats have no bit depth at all. Refilled whenever the format changes, so
// what is in the list is always something the file can actually be written as
// rather than something that fails when you press run.
static void FillRateAndDepth(HWND dlg, const Block* b)
{
    const RecFormat fmt = (RecFormat)SelectedFormat(dlg);

    HWND rc = GetDlgItem(dlg, IDC_REC_RATE);
    SendMessageW(rc, CB_RESETCONTENT, 0, 0);
    int at = (int)SendMessageW(rc, CB_ADDSTRING, 0,
                               (LPARAM)L"The session's own rate");
    SendMessageW(rc, CB_SETITEMDATA, at, 0);
    int rsel = 0;

    const std::vector<int>& offered = RecFormatRates(fmt);
    // Empty means the format takes anything, so the choice is ours to make.
    static const int kAnyRates[] = { 8000, 11025, 16000, 22050, 24000, 32000,
                                     44100, 48000, 88200, 96000, 176400, 192000 };
    std::vector<int> rates = offered;
    if (rates.empty())
        rates.assign(kAnyRates, kAnyRates + sizeof(kAnyRates) / sizeof(kAnyRates[0]));

    for (int hz : rates)
    {
        at = (int)SendMessageW(rc, CB_ADDSTRING, 0,
                               (LPARAM)FormatW(L"%d Hz", hz).c_str());
        SendMessageW(rc, CB_SETITEMDATA, at, (LPARAM)hz);
        if (hz == b->recRateHz) rsel = at;
    }
    SendMessageW(rc, CB_SETCURSEL, rsel, 0);
    // One rate on offer is not a choice: Opus writes 48 kHz whatever it is
    // handed, and a combo that can only be set to what it already says would
    // be a control that does nothing.
    EnableWindow(rc, rates.size() > 1);
    EnableWindow(GetDlgItem(dlg, IDC_REC_RATE_LABEL), rates.size() > 1);

    HWND bc = GetDlgItem(dlg, IDC_REC_BITS);
    SendMessageW(bc, CB_RESETCONTENT, 0, 0);
    const std::vector<int>& depths = RecFormatDepths(fmt);
    at = (int)SendMessageW(bc, CB_ADDSTRING, 0, (LPARAM)L"16 bit, the default");
    SendMessageW(bc, CB_SETITEMDATA, at, 0);
    int bsel = 0;
    for (int bits : depths)
    {
        if (bits == 16) continue;      // already there, as the default
        at = (int)SendMessageW(bc, CB_ADDSTRING, 0,
                               (LPARAM)(bits == 32 ? L"32 bit float"
                                                   : FormatW(L"%d bit", bits).c_str()));
        SendMessageW(bc, CB_SETITEMDATA, at, (LPARAM)bits);
        if (bits == b->recBits) bsel = at;
    }
    SendMessageW(bc, CB_SETCURSEL, bsel, 0);
    EnableWindow(bc, !depths.empty());
    EnableWindow(GetDlgItem(dlg, IDC_REC_BITS_LABEL), !depths.empty());
}

// The bitrates the chosen format can be written at, at the chosen rate.
// libvorbis refuses 192 kbps at 22050 Hz, so for Ogg this list shortens as
// the rate comes down - and a bitrate that cannot be written must not be
// something the dialog let you pick.
static void FillBitrate(HWND dlg, int want)
{
    const RecFormat fmt = (RecFormat)SelectedFormat(dlg);
    const std::vector<int> kbps = RecFormatBitrates(fmt, SelectedRecRate(dlg));

    HWND kc = GetDlgItem(dlg, IDC_REC_KBPS);
    SendMessageW(kc, CB_RESETCONTENT, 0, 0);
    int sel = 0, bestDelta = -1;
    for (size_t i = 0; i < kbps.size(); ++i)
    {
        const int at = (int)SendMessageW(kc, CB_ADDSTRING, 0,
                                         (LPARAM)FormatW(L"%d kbps", kbps[i]).c_str());
        SendMessageW(kc, CB_SETITEMDATA, at, (LPARAM)kbps[i]);
        // Nearest to what the block asked for, so narrowing the list moves the
        // selection as little as it can rather than back to the start of it.
        const int delta = kbps[i] > want ? kbps[i] - want : want - kbps[i];
        if (bestDelta < 0 || delta < bestDelta) { bestDelta = delta; sel = at; }
    }
    SendMessageW(kc, CB_SETCURSEL, sel, 0);

    const bool lossy = !kbps.empty();
    EnableWindow(kc, lossy);
    EnableWindow(GetDlgItem(dlg, IDC_REC_KBPS_LABEL), lossy);
}

static int SelectedBitrate(HWND dlg)
{
    HWND kc = GetDlgItem(dlg, IDC_REC_KBPS);
    const int sel = (int)SendMessageW(kc, CB_GETCURSEL, 0, 0);
    if (sel < 0) return 192;
    return (int)SendMessageW(kc, CB_GETITEMDATA, sel, 0);
}

// Which rate and depth the two combos are on. Nothing is read from a combo
// that the format has disabled: it still holds whatever the last format put
// there, and storing that would quietly change a setting nobody touched.
static int SelectedRecRate(HWND dlg)
{
    HWND rc = GetDlgItem(dlg, IDC_REC_RATE);
    if (!IsWindowEnabled(rc)) return 0;
    const int sel = (int)SendMessageW(rc, CB_GETCURSEL, 0, 0);
    if (sel < 0) return 0;
    return (int)SendMessageW(rc, CB_GETITEMDATA, sel, 0);
}

static int SelectedRecBits(HWND dlg)
{
    HWND bc = GetDlgItem(dlg, IDC_REC_BITS);
    if (!IsWindowEnabled(bc)) return 0;
    const int sel = (int)SendMessageW(bc, CB_GETCURSEL, 0, 0);
    if (sel < 0) return 0;
    return (int)SendMessageW(bc, CB_GETITEMDATA, sel, 0);
}

static void ReadFileFields(HWND dlg, Block* b)
{
    wchar_t folder[MAX_PATH] = { 0 }, tmpl[256] = { 0 };
    GetDlgItemTextW(dlg, IDC_REC_FOLDER, folder, MAX_PATH);
    GetDlgItemTextW(dlg, IDC_REC_TEMPLATE, tmpl, 256);
    b->folder = Trim(folder);
    b->nameTemplate = Trim(tmpl);
    b->recFormat = SelectedFormat(dlg);

    // Left alone when the format has no bitrate, so switching a block to WAV
    // and back does not lose the bitrate it was set to.
    if (IsWindowEnabled(GetDlgItem(dlg, IDC_REC_KBPS)))
        b->bitrateKbps = SelectedBitrate(dlg);

    b->recRateHz = SelectedRecRate(dlg);
    b->recBits = SelectedRecBits(dlg);
}

// The bitrate only means anything for the formats that have one, and which
// ones those are depends on the rate as well for Ogg Vorbis.
static void SyncBitrateField(HWND dlg)
{
    FillBitrate(dlg, SelectedBitrate(dlg));
}

// Rebuilt from what is selected now rather than from the block, so a rate
// still on offer after the format changed stays chosen and one that is not
// falls back to the session's rather than to something silently different.
// The bitrate follows, because changing the rate can shorten its list.
static void RefillRateAndDepth(HWND dlg)
{
    Block probe;
    probe.recRateHz = SelectedRecRate(dlg);
    probe.recBits = SelectedRecBits(dlg);
    const int kbps = SelectedBitrate(dlg);
    FillRateAndDepth(dlg, &probe);
    FillBitrate(dlg, kbps);
}

// Which of the three the combo is on. Position is the value, which is why the
// RM_ constants are 0, 1 and 2 in that order.
static int SelectedRecMode(HWND dlg)
{
    const int m = (int)SendDlgItemMessageW(dlg, IDC_REC_MODE, CB_GETCURSEL, 0, 0);
    return (m >= RM_MIXED && m <= RM_BOTH) ? m : RM_MIXED;
}

// What the mode means, which is not obvious from three words in a list. The
// part worth knowing is that a track is not a slice of the mix: it is taken at
// the input block, so nothing below that block is in it.
static void SyncRecModeNote(HWND dlg)
{
    const int mode = SelectedRecMode(dlg);
    const wchar_t* note =
        mode == RM_MIXED
        ? L"One file, of the audio reaching this block - every input above it, "
          L"through every effect above it."
        : mode == RM_TRACKS
        ? L"A file per input block above this one, named after the block, taken "
          L"at that block's own volume and before any effect below it. No file "
          L"of the mix. Skipping silence and splitting by size are not applied "
          L"to tracks: they would cut each file in a different place, and "
          L"tracks are worth having because they line up."
        : L"The mix in one file, and a file per input block above this one as "
          L"well. The tracks are taken at each block's own volume, before any "
          L"effect below it, so they are not slices of the mixed file.";
    SetDlgItemTextW(dlg, IDC_REC_MODE_NOTE, note);
}

static void SyncRecorderFields(HWND dlg)
{
    SyncBitrateField(dlg);
    SyncRecModeNote(dlg);

    // The gate and splitting by size do not reach the tracks, so with no mixed
    // file to apply them to there is nothing for them to do.
    const bool anyMix = SelectedRecMode(dlg) != RM_TRACKS;
    EnableWindow(GetDlgItem(dlg, IDC_REC_GATE), anyMix);

    bool gate = anyMix && IsDlgButtonChecked(dlg, IDC_REC_GATE) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_REC_GATE_SLIDER), gate);
    EnableWindow(GetDlgItem(dlg, IDC_REC_GATE_TEXT), gate);
    EnableWindow(GetDlgItem(dlg, IDC_REC_GATE_LABEL), gate);
    EnableWindow(GetDlgItem(dlg, IDC_REC_HOLD_SLIDER), gate);
    EnableWindow(GetDlgItem(dlg, IDC_REC_HOLD_TEXT), gate);
    EnableWindow(GetDlgItem(dlg, IDC_REC_HOLD_LABEL), gate);

    int split = (int)SendDlgItemMessageW(dlg, IDC_REC_SPLIT, CB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dlg, IDC_REC_SPLIT_VALUE), split > 0);
    EnableWindow(GetDlgItem(dlg, IDC_REC_SPLIT_LABEL), split > 0);
    SetDlgItemTextW(dlg, IDC_REC_SPLIT_UNITS,
                    split == 1 ? L"minutes of recorded audio"
                  : split == 2 ? L"megabytes on disk"
                               : L"");
}

static INT_PTR CALLBACK RecorderDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        FillFileFields(dlg, b,
                       L"Use {session}, {block}, {index}, {date}, {time}, "
                       L"{datetime} and {n}. {index} is the block's place in "
                       L"the pipeline. Leave the folder blank for your Music "
                       L"folder.");

        {
            HWND mc = GetDlgItem(dlg, IDC_REC_MODE);
            SendMessageW(mc, CB_ADDSTRING, 0, (LPARAM)L"The mix, in one file");
            SendMessageW(mc, CB_ADDSTRING, 0,
                         (LPARAM)L"Each input block, in a file of its own");
            SendMessageW(mc, CB_ADDSTRING, 0,
                         (LPARAM)L"Both: the mix, and each input block");
            SendMessageW(mc, CB_SETCURSEL,
                         (b->recMode >= RM_MIXED && b->recMode <= RM_BOTH)
                             ? b->recMode : RM_MIXED, 0);
        }

        CheckDlgButton(dlg, IDC_REC_GATE, b->skipSilence ? BST_CHECKED : BST_UNCHECKED);
        SetupDbSlider(dlg, IDC_REC_GATE_SLIDER, IDC_REC_GATE_TEXT, 80,
                      b->silenceDb < -80 ? -80 : (b->silenceDb > -20 ? -20 : b->silenceDb));
        // The gate only ever cuts, so the top of its range is -20 dB rather
        // than the symmetric one SetupDbSlider would give it.
        SendDlgItemMessageW(dlg, IDC_REC_GATE_SLIDER, TBM_SETRANGEMAX, TRUE, -20);
        // One past the seconds range is "always": the gate opens once and
        // never shuts, so the threshold only decides when to start.
        SetupSlider(dlg, IDC_REC_HOLD_SLIDER, IDC_REC_HOLD_TEXT, 0, kHoldMaxSec + 1,
                    b->silenceHoldSec == kHoldAlways ? kHoldMaxSec + 1
                                                     : b->silenceHoldSec, L" s");
        SetHoldText(dlg);

        HWND sc = GetDlgItem(dlg, IDC_REC_SPLIT);
        SendMessageW(sc, CB_ADDSTRING, 0, (LPARAM)L"Never");
        SendMessageW(sc, CB_ADDSTRING, 0, (LPARAM)L"After a number of minutes");
        SendMessageW(sc, CB_ADDSTRING, 0, (LPARAM)L"After a number of megabytes");
        SendMessageW(sc, CB_SETCURSEL,
                     (b->splitMode >= 0 && b->splitMode <= 2) ? b->splitMode : 0, 0);
        SetDlgItemInt(dlg, IDC_REC_SPLIT_VALUE,
                      (UINT)(b->splitMode == 2 ? b->splitMb : b->splitMinutes), FALSE);

        SetupVolumeSlider(dlg, b->volumePct);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        SyncRecorderFields(dlg);
        return TRUE;
    }

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_REC_GATE_SLIDER)
            SetDbText(dlg, IDC_REC_GATE_SLIDER, IDC_REC_GATE_TEXT);
        else if (id == IDC_REC_HOLD_SLIDER)
            SetHoldText(dlg);
        else
            UpdateSliderText(dlg, IDC_VOL_SLIDER, IDC_VOL_TEXT, L"%");
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_REC_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                SyncRecorderFields(dlg);
                RefillRateAndDepth(dlg);
            }
            return TRUE;

        // Changing the rate can shorten what bitrates are left, for Ogg.
        case IDC_REC_RATE:
            if (HIWORD(wp) == CBN_SELCHANGE) SyncBitrateField(dlg);
            return TRUE;

        case IDC_REC_SPLIT:
        case IDC_REC_MODE:
            if (HIWORD(wp) == CBN_SELCHANGE) SyncRecorderFields(dlg);
            return TRUE;

        case IDC_REC_GATE:
            SyncRecorderFields(dlg);
            return TRUE;

        case IDC_REC_BROWSE:
        {
            wchar_t cur[MAX_PATH] = { 0 };
            GetDlgItemTextW(dlg, IDC_REC_FOLDER, cur, MAX_PATH);
            std::wstring folder = cur;
            if (PickFolder(dlg, folder))
                SetDlgItemTextW(dlg, IDC_REC_FOLDER, folder.c_str());
            SetFocus(GetDlgItem(dlg, IDC_REC_FOLDER));
            return TRUE;
        }

        case IDC_APPLY:
        case IDOK:
        {
            ReadFileFields(dlg, b);

            b->recMode        = SelectedRecMode(dlg);
            b->skipSilence    = IsDlgButtonChecked(dlg, IDC_REC_GATE) == BST_CHECKED;
            b->silenceDb      = ReadSlider(dlg, IDC_REC_GATE_SLIDER);
            int hold = ReadSlider(dlg, IDC_REC_HOLD_SLIDER);
            b->silenceHoldSec = (hold > kHoldMaxSec) ? kHoldAlways : hold;

            int mode = (int)SendDlgItemMessageW(dlg, IDC_REC_SPLIT, CB_GETCURSEL, 0, 0);
            b->splitMode = (mode >= 0 && mode <= 2) ? mode : 0;
            if (b->splitMode)
            {
                BOOL got = FALSE;
                int v = (int)GetDlgItemInt(dlg, IDC_REC_SPLIT_VALUE, &got, FALSE);
                const int lo = 1;
                const int hi = (b->splitMode == 1) ? 1440 : 4000;
                if (!got || v < lo || v > hi)
                {
                    ErrorBox(dlg, FormatW(
                        L"Type how many %s each file should hold, from %d to %d.",
                        b->splitMode == 1 ? L"minutes" : L"megabytes", lo, hi));
                    SetFocus(GetDlgItem(dlg, IDC_REC_SPLIT_VALUE));
                    return TRUE;
                }
                if (b->splitMode == 1) b->splitMinutes = v;
                else                   b->splitMb = v;
            }

            b->volumePct = ReadVolumeSlider(dlg);
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}


// ---------------------------------------------------------------------------
// Tone, EQ and channel blocks
// ---------------------------------------------------------------------------

static INT_PTR CALLBACK ToneDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetupDbSlider(dlg, IDC_BASS_SLIDER, IDC_BASS_TEXT, 15, b->bassDb);
        SetupDbSlider(dlg, IDC_TREB_SLIDER, IDC_TREB_TEXT, 15, b->trebleDb);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_BASS_SLIDER) SetDbText(dlg, IDC_BASS_SLIDER, IDC_BASS_TEXT);
        else if (id == IDC_TREB_SLIDER) SetDbText(dlg, IDC_TREB_SLIDER, IDC_TREB_TEXT);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->bassDb   = ReadSlider(dlg, IDC_BASS_SLIDER);
            b->trebleDb = ReadSlider(dlg, IDC_TREB_SLIDER);
            b->enabled  = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK Eq10DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        for (int i = 0; i < kEqBands; ++i)
            SetupDbSlider(dlg, IDC_EQ_SLIDER_BASE + i, IDC_EQ_TEXT_BASE + i, 12, b->eqDb[i]);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
    {
        int band = GetDlgCtrlID(reinterpret_cast<HWND>(lp)) - IDC_EQ_SLIDER_BASE;
        if (band >= 0 && band < kEqBands)
            SetDbText(dlg, IDC_EQ_SLIDER_BASE + band, IDC_EQ_TEXT_BASE + band);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_EQ_RESET:
            for (int i = 0; i < kEqBands; ++i)
            {
                SendDlgItemMessageW(dlg, IDC_EQ_SLIDER_BASE + i, TBM_SETPOS, TRUE, 0);
                SetDbText(dlg, IDC_EQ_SLIDER_BASE + i, IDC_EQ_TEXT_BASE + i);
            }
            SetFocus(GetDlgItem(dlg, IDC_EQ_SLIDER_BASE));
            return TRUE;

        case IDC_APPLY:
        case IDOK:
            for (int i = 0; i < kEqBands; ++i)
                b->eqDb[i] = ReadSlider(dlg, IDC_EQ_SLIDER_BASE + i);
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// The two per-channel sliders, and the four ids that go with each channel.
// Written down once so that adding the right channel could not be a copy of
// the left channel with one of the four ids left behind.
struct ChanRow { int volSlider, volText, panSlider, panText; };
static const ChanRow kChanRows[kMixChannels] = {
    { IDC_CHAN_LVOL, IDC_CHAN_LVOL_TEXT, IDC_CHAN_LPAN, IDC_CHAN_LPAN_TEXT },
    { IDC_CHAN_RVOL, IDC_CHAN_RVOL_TEXT, IDC_CHAN_RPAN, IDC_CHAN_RPAN_TEXT },
};

// Pan gets its own text because a position is a place, not a number: the
// slider itself has to report -100 because that is where its thumb is, but
// nothing else in the dialog should make anybody translate that back into a
// side. PanText is the model's spelling, so the label beside the slider and
// the block's one-line summary say the same words.
static void UpdatePanText(HWND dlg, int sliderId, int textId)
{
    SetSliderReadout(dlg, sliderId, textId, PanText(ReadSlider(dlg, sliderId)));
}

static void FillChannelFields(HWND dlg, const Block* b)
{
    for (int c = 0; c < kMixChannels; ++c)
    {
        const ChanRow& row = kChanRows[c];
        SetupRange(dlg, row.volSlider, row.volText,
                   kChanGainMin, kChanGainMax, b->chanGainDb[c], L" dB", 6);
        SetupRange(dlg, row.panSlider, row.panText,
                   kChanPanMin, kChanPanMax, b->chanPan[c], L"", 10);
        UpdatePanText(dlg, row.panSlider, row.panText);
    }
}

static INT_PTR CALLBACK ChannelsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        FillChannelFields(dlg, b);
        SetupSlider(dlg, IDC_WIDTH_SLIDER, IDC_WIDTH_TEXT, 0, 200, b->widthPct, L"%");
        CheckDlgButton(dlg, IDC_CHAN_SWAP,
                       b->swapChannels ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_CHAN_CENTRE,
                       b->centreCancel ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
        // Which slider moved is in lParam. Asking every one of them for its
        // position and rewriting all five labels would work, but it also makes
        // a reader announce four labels that did not change on every arrow key.
        {
            const HWND moved = reinterpret_cast<HWND>(lp);
            for (int c = 0; c < kMixChannels; ++c)
            {
                const ChanRow& row = kChanRows[c];
                if (moved == GetDlgItem(dlg, row.volSlider))
                    UpdateSliderText(dlg, row.volSlider, row.volText, L" dB");
                else if (moved == GetDlgItem(dlg, row.panSlider))
                    UpdatePanText(dlg, row.panSlider, row.panText);
            }
            if (moved == GetDlgItem(dlg, IDC_WIDTH_SLIDER))
                UpdateSliderText(dlg, IDC_WIDTH_SLIDER, IDC_WIDTH_TEXT, L"%");
        }
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHAN_RESET)
        {
            // Back to the identity, which is the setting somebody who has got
            // lost in four sliders is trying to find their way back to.
            Block def;
            FillChannelFields(dlg, &def);
            SetFocus(GetDlgItem(dlg, IDC_CHAN_LVOL));
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            for (int c = 0; c < kMixChannels; ++c)
            {
                b->chanGainDb[c] = ReadSlider(dlg, kChanRows[c].volSlider);
                b->chanPan[c]    = ReadSlider(dlg, kChanRows[c].panSlider);
            }
            b->widthPct     = ReadSlider(dlg, IDC_WIDTH_SLIDER);
            b->swapChannels = IsDlgButtonChecked(dlg, IDC_CHAN_SWAP) == BST_CHECKED;
            b->centreCancel = IsDlgButtonChecked(dlg, IDC_CHAN_CENTRE) == BST_CHECKED;
            b->enabled      = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Four band parametric EQ
//
// One band at a time rather than sixteen controls at once. Each row of the
// list is a whole band written out - "Band 2: Peaking at 500 Hz, +3 dB, Q 1.0"
// - so a reader gets the setting from the row instead of having to visit four
// controls to find out what it says, and the four controls below edit whatever
// is selected.
// ---------------------------------------------------------------------------

static const int kPeqFreqSteps = 1000;

// A frequency slider has to be logarithmic. Half of what anyone listens to
// lives below 1 kHz, and a linear 20 Hz to 20 kHz slider puts all of it in the
// first twentieth of the travel.
static int PeqHzToPos(int hz)
{
    if (hz <= kParaHzMin) return 0;
    if (hz >= kParaHzMax) return kPeqFreqSteps;
    const double span = std::log((double)kParaHzMax / (double)kParaHzMin);
    return (int)(kPeqFreqSteps * std::log((double)hz / (double)kParaHzMin) / span + 0.5);
}

static int PeqPosToHz(int pos)
{
    if (pos <= 0) return kParaHzMin;
    if (pos >= kPeqFreqSteps) return kParaHzMax;
    const double span = std::log((double)kParaHzMax / (double)kParaHzMin);
    double hz = kParaHzMin * std::exp(span * (double)pos / (double)kPeqFreqSteps);
    // Rounded to something worth reading out. 4970 Hz is not a setting anyone
    // means, and a readout that says it is noise rather than precision.
    int step = 1;
    if (hz >= 10000) step = 100;
    else if (hz >= 1000) step = 50;
    else if (hz >= 100) step = 5;
    int v = (int)((hz / step) + 0.5) * step;
    if (v < kParaHzMin) v = kParaHzMin;
    if (v > kParaHzMax) v = kParaHzMax;
    return v;
}

static int PeqBand(HWND dlg)
{
    const int sel = (int)SendDlgItemMessageW(dlg, IDC_PEQ_LIST, LB_GETCURSEL, 0, 0);
    return (sel == LB_ERR || sel >= kParaBands) ? 0 : sel;
}

static void PeqFillList(HWND dlg, const Block* b, int select)
{
    HWND list = GetDlgItem(dlg, IDC_PEQ_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kParaBands; ++i)
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)ParaBandText(*b, i).c_str());
    SendMessageW(list, LB_SETCURSEL, (WPARAM)select, 0);
}

// Rewrites one row in place, keeping the selection: a list box row cannot be
// edited, only replaced.
static void PeqRefreshRow(HWND dlg, const Block* b, int i)
{
    HWND list = GetDlgItem(dlg, IDC_PEQ_LIST);
    SendMessageW(list, LB_DELETESTRING, (WPARAM)i, 0);
    SendMessageW(list, LB_INSERTSTRING, (WPARAM)i, (LPARAM)ParaBandText(*b, i).c_str());
    SendMessageW(list, LB_SETCURSEL, (WPARAM)i, 0);
}

// Shows the selected band in the four controls, and greys out what does not
// apply: a pass filter has no gain, and an off band has nothing at all.
static void PeqSyncControls(HWND dlg, const Block* b)
{
    const int i = PeqBand(dlg);
    const int type = b->paraType[i];
    const bool on = (type != PT_OFF);
    const bool gain = ParaTypeUsesGain(type);

    SendDlgItemMessageW(dlg, IDC_PEQ_TYPE, CB_SETCURSEL, (WPARAM)type, 0);

    SendDlgItemMessageW(dlg, IDC_PEQ_FREQ, TBM_SETRANGE, TRUE,
                        MAKELPARAM(0, kPeqFreqSteps));
    SendDlgItemMessageW(dlg, IDC_PEQ_FREQ, TBM_SETPAGESIZE, 0, kPeqFreqSteps / 20);
    SendDlgItemMessageW(dlg, IDC_PEQ_FREQ, TBM_SETPOS, TRUE, PeqHzToPos(b->paraHz[i]));
    SetSliderReadout(dlg, IDC_PEQ_FREQ, IDC_PEQ_FREQ_TEXT, ParaHzText(b->paraHz[i]));

    SetupRange(dlg, IDC_PEQ_GAIN, IDC_PEQ_GAIN_TEXT,
               kParaGainMin, kParaGainMax, b->paraGainDb[i], L" dB", 3);
    SetSliderReadout(dlg, IDC_PEQ_GAIN, IDC_PEQ_GAIN_TEXT,
                     FormatW(L"%+d dB", b->paraGainDb[i]));

    SendDlgItemMessageW(dlg, IDC_PEQ_Q, TBM_SETRANGE, TRUE,
                        MAKELPARAM(kParaQMin, kParaQMax));
    SendDlgItemMessageW(dlg, IDC_PEQ_Q, TBM_SETPAGESIZE, 0, 5);
    SendDlgItemMessageW(dlg, IDC_PEQ_Q, TBM_SETPOS, TRUE, b->paraQ[i]);
    SetSliderReadout(dlg, IDC_PEQ_Q, IDC_PEQ_Q_TEXT,
                     FormatW(L"%.1f", (double)b->paraQ[i] / 10.0));

    EnableWindow(GetDlgItem(dlg, IDC_PEQ_FREQ), on);
    EnableWindow(GetDlgItem(dlg, IDC_PEQ_FREQ_LABEL), on);
    EnableWindow(GetDlgItem(dlg, IDC_PEQ_Q), on);
    EnableWindow(GetDlgItem(dlg, IDC_PEQ_Q_LABEL), on);
    EnableWindow(GetDlgItem(dlg, IDC_PEQ_GAIN), on && gain);
    EnableWindow(GetDlgItem(dlg, IDC_PEQ_GAIN_LABEL), on && gain);
}

static INT_PTR CALLBACK ParaDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        for (int t = 0; t < PT_COUNT; ++t)
            SendDlgItemMessageW(dlg, IDC_PEQ_TYPE, CB_ADDSTRING, 0,
                                (LPARAM)ParaTypeName(t));

        PeqFillList(dlg, b, 0);
        PeqSyncControls(dlg, b);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_HSCROLL:
    {
        const int i = PeqBand(dlg);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_PEQ_FREQ)
        {
            b->paraHz[i] = PeqPosToHz(ReadSlider(dlg, IDC_PEQ_FREQ));
            SetSliderReadout(dlg, IDC_PEQ_FREQ, IDC_PEQ_FREQ_TEXT,
                             ParaHzText(b->paraHz[i]));
        }
        else if (id == IDC_PEQ_GAIN)
        {
            b->paraGainDb[i] = ReadSlider(dlg, IDC_PEQ_GAIN);
            SetSliderReadout(dlg, IDC_PEQ_GAIN, IDC_PEQ_GAIN_TEXT,
                             FormatW(L"%+d dB", b->paraGainDb[i]));
        }
        else if (id == IDC_PEQ_Q)
        {
            b->paraQ[i] = ReadSlider(dlg, IDC_PEQ_Q);
            SetSliderReadout(dlg, IDC_PEQ_Q, IDC_PEQ_Q_TEXT,
                             FormatW(L"%.1f", (double)b->paraQ[i] / 10.0));
        }
        else return TRUE;
        PeqRefreshRow(dlg, b, i);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_PEQ_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) PeqSyncControls(dlg, b);
            return TRUE;

        case IDC_PEQ_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                const int i = PeqBand(dlg);
                const int sel = (int)SendDlgItemMessageW(dlg, IDC_PEQ_TYPE,
                                                        CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) b->paraType[i] = sel;
                PeqSyncControls(dlg, b);
                PeqRefreshRow(dlg, b, i);
            }
            return TRUE;

        case IDC_PEQ_RESET:
        {
            Block fresh;
            for (int i = 0; i < kParaBands; ++i)
            {
                b->paraType[i]   = fresh.paraType[i];
                b->paraHz[i]     = fresh.paraHz[i];
                b->paraGainDb[i] = fresh.paraGainDb[i];
                b->paraQ[i]      = fresh.paraQ[i];
            }
            PeqFillList(dlg, b, 0);
            PeqSyncControls(dlg, b);
            SetFocus(GetDlgItem(dlg, IDC_PEQ_LIST));
            return TRUE;
        }

        case IDC_APPLY:
        case IDOK:
            // The bands were written as they were edited, so there is nothing
            // to collect here but the enabled flag.
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Noise gate
// ---------------------------------------------------------------------------

static INT_PTR CALLBACK GateDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetupRange(dlg, IDC_GATE_THRESH, IDC_GATE_THRESH_TEXT,
                   kGateThreshMin, kGateThreshMax, b->gateThreshDb, L" dB", 3);
        SetupRange(dlg, IDC_GATE_RANGE, IDC_GATE_RANGE_TEXT,
                   kGateRangeMin, kGateRangeMax, b->gateRangeDb, L" dB", 3);
        SetupRange(dlg, IDC_GATE_ATTACK, IDC_GATE_ATTACK_TEXT,
                   kGateAttackMin, kGateAttackMax, b->gateAttackMs, L" ms", 5);
        SetupRange(dlg, IDC_GATE_HOLD, IDC_GATE_HOLD_TEXT,
                   kGateHoldMin, kGateHoldMax, b->gateHoldMs, L" ms", 50);
        SetupRange(dlg, IDC_GATE_RELEASE, IDC_GATE_RELEASE_TEXT,
                   kGateReleaseMin, kGateReleaseMax, b->gateReleaseMs, L" ms", 50);
        SetupRange(dlg, IDC_GATE_LOOK, IDC_GATE_LOOK_TEXT,
                   kGateLookaheadMin, kGateLookaheadMax, b->gateLookaheadMs, L" ms", 5);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_GATE_THRESH)
            UpdateSliderText(dlg, IDC_GATE_THRESH, IDC_GATE_THRESH_TEXT, L" dB");
        else if (id == IDC_GATE_RANGE)
            UpdateSliderText(dlg, IDC_GATE_RANGE, IDC_GATE_RANGE_TEXT, L" dB");
        else if (id == IDC_GATE_ATTACK)
            UpdateSliderText(dlg, IDC_GATE_ATTACK, IDC_GATE_ATTACK_TEXT, L" ms");
        else if (id == IDC_GATE_HOLD)
            UpdateSliderText(dlg, IDC_GATE_HOLD, IDC_GATE_HOLD_TEXT, L" ms");
        else if (id == IDC_GATE_RELEASE)
            UpdateSliderText(dlg, IDC_GATE_RELEASE, IDC_GATE_RELEASE_TEXT, L" ms");
        else if (id == IDC_GATE_LOOK)
            UpdateSliderText(dlg, IDC_GATE_LOOK, IDC_GATE_LOOK_TEXT, L" ms");
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->gateThreshDb  = ReadSlider(dlg, IDC_GATE_THRESH);
            b->gateRangeDb   = ReadSlider(dlg, IDC_GATE_RANGE);
            b->gateAttackMs  = ReadSlider(dlg, IDC_GATE_ATTACK);
            b->gateHoldMs    = ReadSlider(dlg, IDC_GATE_HOLD);
            b->gateReleaseMs = ReadSlider(dlg, IDC_GATE_RELEASE);
            b->gateLookaheadMs = ReadSlider(dlg, IDC_GATE_LOOK);
            b->enabled       = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

// The makeup readout says the figure either way. On automatic the slider is
// unavailable rather than hidden, so tabbing past it still says what the
// setting is instead of the control simply not being there.
static void CompMakeupState(HWND dlg)
{
    const bool automatic = IsDlgButtonChecked(dlg, IDC_COMP_AUTO) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_COMP_MAKEUP), !automatic);
    EnableWindow(GetDlgItem(dlg, IDC_COMP_MAKEUP_LABEL), !automatic);

    if (automatic)
    {
        double db = AutoMakeupDb((double)ReadSlider(dlg, IDC_COMP_THRESH),
                                 (double)ReadSlider(dlg, IDC_COMP_RATIO));
        SetSliderReadout(dlg, IDC_COMP_MAKEUP, IDC_COMP_MAKEUP_TEXT,
                         FormatW(L"%+.0f dB, worked out", db));
    }
    else
    {
        SetSliderReadout(dlg, IDC_COMP_MAKEUP, IDC_COMP_MAKEUP_TEXT,
                         FormatW(L"%+d dB", ReadSlider(dlg, IDC_COMP_MAKEUP)));
    }
}

static INT_PTR CALLBACK CompDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetupRange(dlg, IDC_COMP_THRESH, IDC_COMP_THRESH_TEXT,
                   kCompThreshMin, kCompThreshMax, b->compThreshDb, L" dB", 3);
        SetupRange(dlg, IDC_COMP_RATIO, IDC_COMP_RATIO_TEXT,
                   kCompRatioMin, kCompRatioMax, b->compRatio, L":1", 1);
        SetupRange(dlg, IDC_COMP_ATTACK, IDC_COMP_ATTACK_TEXT,
                   kCompAttackMin, kCompAttackMax, b->compAttackMs, L" ms", 5);
        SetupRange(dlg, IDC_COMP_RELEASE, IDC_COMP_RELEASE_TEXT,
                   kCompReleaseMin, kCompReleaseMax, b->compReleaseMs, L" ms", 50);
        SetupRange(dlg, IDC_COMP_MAKEUP, IDC_COMP_MAKEUP_TEXT,
                   0, kCompMakeupMax, b->compMakeupDb, L" dB", 3);
        CheckDlgButton(dlg, IDC_COMP_AUTO,
                       b->compAutoMakeup ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        CompMakeupState(dlg);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_COMP_THRESH)
            UpdateSliderText(dlg, IDC_COMP_THRESH, IDC_COMP_THRESH_TEXT, L" dB");
        else if (id == IDC_COMP_RATIO)
            UpdateSliderText(dlg, IDC_COMP_RATIO, IDC_COMP_RATIO_TEXT, L":1");
        else if (id == IDC_COMP_ATTACK)
            UpdateSliderText(dlg, IDC_COMP_ATTACK, IDC_COMP_ATTACK_TEXT, L" ms");
        else if (id == IDC_COMP_RELEASE)
            UpdateSliderText(dlg, IDC_COMP_RELEASE, IDC_COMP_RELEASE_TEXT, L" ms");
        // Threshold and ratio are what the automatic figure is worked out
        // from, so it is refreshed on any of them.
        CompMakeupState(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_COMP_AUTO) { CompMakeupState(dlg); return TRUE; }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->compThreshDb   = ReadSlider(dlg, IDC_COMP_THRESH);
            b->compRatio      = ReadSlider(dlg, IDC_COMP_RATIO);
            b->compAttackMs   = ReadSlider(dlg, IDC_COMP_ATTACK);
            b->compReleaseMs  = ReadSlider(dlg, IDC_COMP_RELEASE);
            b->compMakeupDb   = ReadSlider(dlg, IDC_COMP_MAKEUP);
            b->compAutoMakeup = IsDlgButtonChecked(dlg, IDC_COMP_AUTO) == BST_CHECKED;
            b->enabled        = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Limiter
// ---------------------------------------------------------------------------

// The clipping threshold reads as "full scale" at 0 rather than "0 dB", which
// is the same number said in the way people actually mean it.
static std::wstring ClipThreshText(int db)
{
    return db == 0 ? std::wstring(L"full scale") : FormatW(L"%d dB", db);
}

// ---------------------------------------------------------------------------
// Action hotkeys
//
// The list is the whole dialog: one row per action the block has, each row
// carrying the action and the key that does it. A row reads as a pair - the
// action, then its key - so a screen reader gets both together rather than a
// column of names and a column of keys to hold in your head at once.
// ---------------------------------------------------------------------------

struct ActionsCtx
{
    Block* block;
    std::vector<int> actions;
    Hotkey keys[BA_COUNT];
};

static void FillActionList(HWND dlg, ActionsCtx* ctx)
{
    HWND list = GetDlgItem(dlg, IDC_ACT_LIST);
    int keep = (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);

    for (size_t i = 0; i < ctx->actions.size(); ++i)
    {
        const int a = ctx->actions[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        std::wstring name = BlockActionName(a);
        it.pszText = const_cast<wchar_t*>(name.c_str());
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);

        std::wstring key = ctx->keys[a].Set() ? HotkeyScopeText(ctx->keys[a])
                                              : std::wstring(L"none");
        LVITEMW sub{};
        sub.mask = LVIF_TEXT;
        sub.iItem = (int)i;
        sub.iSubItem = 1;
        sub.pszText = const_cast<wchar_t*>(key.c_str());
        SendMessageW(list, LVM_SETITEMW, 0, (LPARAM)&sub);
    }

    const int sel = (keep >= 0 && keep < (int)ctx->actions.size()) ? keep : 0;
    if (!ctx->actions.empty())
    {
        LVITEMW st{};
        st.mask = LVIF_STATE;
        st.state = LVIS_SELECTED | LVIS_FOCUSED;
        st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)sel, (LPARAM)&st);
    }
}

static int SelectedAction(HWND dlg, ActionsCtx* ctx)
{
    HWND list = GetDlgItem(dlg, IDC_ACT_LIST);
    int row = (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (row < 0 || row >= (int)ctx->actions.size()) return -1;
    return ctx->actions[(size_t)row];
}

static void SyncActionNote(HWND dlg, ActionsCtx* ctx)
{
    if (!ctx) return;
    const int a = SelectedAction(dlg, ctx);
    if (a < 0) { SetDlgItemTextW(dlg, IDC_ACT_NOTE, L""); return; }

    std::wstring note;
    if (ctx->keys[a].Set())
    {
        note = FormatW(L"%s will %s", HotkeyText(ctx->keys[a]).c_str(),
                       BlockActionDescription(a));
        if (!ctx->keys[a].global)
            note += L" It only works while a FastRoute window is in front.";
    }
    else
    {
        note = FormatW(L"No key is set, so nothing will %s",
                       BlockActionDescription(a));
    }
    SetDlgItemTextW(dlg, IDC_ACT_NOTE, note.c_str());
}

static INT_PTR CALLBACK ActionsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    ActionsCtx* ctx = reinterpret_cast<ActionsCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ctx = reinterpret_cast<ActionsCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        HWND list = GetDlgItem(dlg, IDC_ACT_LIST);
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                     LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(L"Action");
        col.cx = 110;
        SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.pszText = const_cast<wchar_t*>(L"Hotkey");
        col.cx = 155;
        col.iSubItem = 1;
        SendMessageW(list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

        ListKeys keys;
        keys.onEnter = IDC_ACT_SET;
        keys.onDelete = IDC_ACT_CLEAR;
        SubclassListKeys(list, keys);

        FillActionList(dlg, ctx);
        SyncActionNote(dlg, ctx);
        return TRUE;
    }

    case WM_NOTIFY:
    {
        LPNMHDR nm = reinterpret_cast<LPNMHDR>(lp);
        if (nm && nm->idFrom == IDC_ACT_LIST && nm->code == LVN_ITEMCHANGED)
            SyncActionNote(dlg, ctx);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_ACT_SET:
        {
            if (!ctx) return TRUE;
            const int a = SelectedAction(dlg, ctx);
            if (a < 0) return TRUE;
            Hotkey want = ctx->keys[a];
            if (EditHotkey(dlg, FormatW(L"%s on the %s block",
                                        BlockActionName(a),
                                        ctx->block->TypeName().c_str()),
                           want, HF_ACTION))
            {
                ctx->keys[a] = want;
                FillActionList(dlg, ctx);
                SyncActionNote(dlg, ctx);
            }
            SetFocus(GetDlgItem(dlg, IDC_ACT_LIST));
            return TRUE;
        }

        case IDC_ACT_CLEAR:
        {
            if (!ctx) return TRUE;
            const int a = SelectedAction(dlg, ctx);
            if (a < 0) return TRUE;
            ctx->keys[a] = Hotkey();
            FillActionList(dlg, ctx);
            SyncActionNote(dlg, ctx);
            SetFocus(GetDlgItem(dlg, IDC_ACT_LIST));
            return TRUE;
        }

        case IDOK:
            if (ctx)
                for (int a = 0; a < BA_COUNT; ++a) ctx->block->actionKey[a] = ctx->keys[a];
            EndDialog(dlg, IDOK);
            return TRUE;

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool EditBlockActions(HWND parent, Block& b)
{
    ActionsCtx ctx{ &b, BlockActionsFor(b.typeId), {} };
    if (ctx.actions.empty()) return false;
    for (int a = 0; a < BA_COUNT; ++a) ctx.keys[a] = b.actionKey[a];
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_ACTIONS), parent,
                           ActionsDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

// ---------------------------------------------------------------------------
// Buffer block
// ---------------------------------------------------------------------------

static void SyncBufferFields(HWND dlg, Block* b)
{
    SyncBitrateField(dlg);

    int bound = 0;
    for (int a = 0; a < BA_COUNT; ++a) if (b->actionKey[a].Set()) ++bound;
    SetDlgItemTextW(dlg, IDC_BUF_NOTE, bound
        ? FormatW(L"%d of 4 actions have a key.", bound).c_str()
        : L"No action has a key yet, so there is no way to rewind this block "
          L"while it is running.");
}

static INT_PTR CALLBACK BufferDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        SetupSlider(dlg, IDC_BUF_SECONDS, IDC_BUF_SECONDS_TEXT,
                    kBufSecondsMin, kBufSecondsMax, b->bufSeconds, L" s");
        SendDlgItemMessageW(dlg, IDC_BUF_SECONDS, TBM_SETPAGESIZE, 0, 30);
        SetSliderReadout(dlg, IDC_BUF_SECONDS, IDC_BUF_SECONDS_TEXT,
                         DurationText(b->bufSeconds));
        SetupRange(dlg, IDC_BUF_STEP, IDC_BUF_STEP_TEXT,
                   kBufStepMin, kBufStepMax, b->bufStepSec, L" s", 5);

        FillFileFields(dlg, b,
                       L"Use {session}, {block}, {index}, {date}, {time}, "
                       L"{datetime} and {n}. {index} is the block's place in "
                       L"the pipeline. Leave the folder blank for your Music "
                       L"folder.");
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        SyncBufferFields(dlg, b);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_BUF_SECONDS)
            SetSliderReadout(dlg, IDC_BUF_SECONDS, IDC_BUF_SECONDS_TEXT,
                             DurationText(ReadSlider(dlg, IDC_BUF_SECONDS)));
        else if (id == IDC_BUF_STEP)
            UpdateSliderText(dlg, IDC_BUF_STEP, IDC_BUF_STEP_TEXT, L" s");
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_REC_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                SyncBitrateField(dlg);
                RefillRateAndDepth(dlg);
            }
            return TRUE;

        case IDC_REC_RATE:
            if (HIWORD(wp) == CBN_SELCHANGE) SyncBitrateField(dlg);
            return TRUE;

        case IDC_REC_BROWSE:
        {
            wchar_t cur[MAX_PATH] = { 0 };
            GetDlgItemTextW(dlg, IDC_REC_FOLDER, cur, MAX_PATH);
            std::wstring folder = cur;
            if (PickFolder(dlg, folder, L"Choose where the buffer is saved"))
                SetDlgItemTextW(dlg, IDC_REC_FOLDER, folder.c_str());
            SetFocus(GetDlgItem(dlg, IDC_REC_FOLDER));
            return TRUE;
        }

        case IDC_BUF_ACTIONS:
            if (b && EditBlockActions(dlg, *b)) SyncBufferFields(dlg, b);
            SetFocus(GetDlgItem(dlg, IDC_BUF_ACTIONS));
            return TRUE;

        case IDC_APPLY:
        case IDOK:
            b->bufSeconds = ReadSlider(dlg, IDC_BUF_SECONDS);
            b->bufStepSec = ReadSlider(dlg, IDC_BUF_STEP);
            ReadFileFields(dlg, b);
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK NormDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetupRange(dlg, IDC_NORM_TARGET, IDC_NORM_TARGET_TEXT,
                   kNormTargetMin, kNormTargetMax, b->normTargetDb, L" dB", 2);
        SetupRange(dlg, IDC_NORM_LOOK, IDC_NORM_LOOK_TEXT,
                   kNormLookaheadMin, kNormLookaheadMax, b->normLookaheadMs, L" ms", 10);
        SetupRange(dlg, IDC_NORM_MAXGAIN, IDC_NORM_MAXGAIN_TEXT,
                   kNormMaxGainMin, kNormMaxGainMax, b->normMaxGainDb, L" dB", 2);
        SetupRange(dlg, IDC_NORM_RELEASE, IDC_NORM_RELEASE_TEXT,
                   kNormReleaseMin, kNormReleaseMax, b->normReleaseMs, L" ms", 250);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_NORM_TARGET)
            UpdateSliderText(dlg, IDC_NORM_TARGET, IDC_NORM_TARGET_TEXT, L" dB");
        else if (id == IDC_NORM_LOOK)
            UpdateSliderText(dlg, IDC_NORM_LOOK, IDC_NORM_LOOK_TEXT, L" ms");
        else if (id == IDC_NORM_MAXGAIN)
            UpdateSliderText(dlg, IDC_NORM_MAXGAIN, IDC_NORM_MAXGAIN_TEXT, L" dB");
        else if (id == IDC_NORM_RELEASE)
            UpdateSliderText(dlg, IDC_NORM_RELEASE, IDC_NORM_RELEASE_TEXT, L" ms");
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->normTargetDb    = ReadSlider(dlg, IDC_NORM_TARGET);
            b->normLookaheadMs = ReadSlider(dlg, IDC_NORM_LOOK);
            b->normMaxGainDb   = ReadSlider(dlg, IDC_NORM_MAXGAIN);
            b->normReleaseMs   = ReadSlider(dlg, IDC_NORM_RELEASE);
            b->enabled         = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK ClipMonDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetupRange(dlg, IDC_CLIP_THRESH, IDC_CLIP_THRESH_TEXT,
                   kClipThreshMin, kClipThreshMax, b->clipThreshDb, L" dB", 1);
        SetSliderReadout(dlg, IDC_CLIP_THRESH, IDC_CLIP_THRESH_TEXT,
                         ClipThreshText(b->clipThreshDb));
        SetupRange(dlg, IDC_CLIP_INTERVAL, IDC_CLIP_INTERVAL_TEXT,
                   kClipIntervalMin, kClipIntervalMax, b->clipIntervalS, L" s", 1);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_CLIP_THRESH)
            SetSliderReadout(dlg, IDC_CLIP_THRESH, IDC_CLIP_THRESH_TEXT,
                             ClipThreshText(ReadSlider(dlg, IDC_CLIP_THRESH)));
        else if (id == IDC_CLIP_INTERVAL)
            UpdateSliderText(dlg, IDC_CLIP_INTERVAL, IDC_CLIP_INTERVAL_TEXT, L" s");
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->clipThreshDb  = ReadSlider(dlg, IDC_CLIP_THRESH);
            b->clipIntervalS = ReadSlider(dlg, IDC_CLIP_INTERVAL);
            b->enabled       = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Net Receive
//
// The list is the point of this dialog. A sender announces itself, so nobody
// should have to know an address to listen to one: the list fills itself in
// while the dialog is open, and choosing a row fills in everything underneath
// it. The address and port are still there for a sender that does not
// announce - a VBAN host, or one across a subnet where multicast does not
// reach - but they are the fallback rather than the way in.
// ---------------------------------------------------------------------------

// The edit fields here hold addresses and names rather than paragraphs, so
// one small helper saves a buffer at every call site.
static std::wstring GetDlgText(HWND dlg, int id, int max = 256)
{
    std::vector<wchar_t> buf((size_t)max, 0);
    GetDlgItemTextW(dlg, id, buf.data(), max);
    return std::wstring(buf.data());
}

struct NetRecvCtx
{
    Block* block = nullptr;
    std::unique_ptr<net::Listener> listener;
    std::vector<net::Peer> shown;
    unsigned int chosenId = 0;
};

static const UINT_PTR kNetListTimer = 1;
static const UINT kNetListEveryMs = 700;

// Rebuilt rather than patched, because peers come and go. What has to survive
// a rebuild is the selection: a list that reshuffled under somebody reading it
// by arrow key would be unusable, which is why Listener::Peers sorts.
static void FillNetFound(HWND dlg, NetRecvCtx* ctx)
{
    HWND list = GetDlgItem(dlg, IDC_NR_FOUND);
    const int want = ctx->chosenId
        ? (int)ctx->chosenId
        : 0;

    // What is selected now, by identity rather than by row.
    unsigned int selectedId = 0;
    const int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && (size_t)sel < ctx->shown.size())
        selectedId = ctx->shown[(size_t)sel].streamId;
    if (!selectedId) selectedId = (unsigned int)want;

    std::vector<net::Peer> peers = ctx->listener ? ctx->listener->Peers()
                                                 : std::vector<net::Peer>();
    // Nothing changed, so nothing is touched. Rewriting an identical list
    // every second would move the caret and interrupt a reader mid-line.
    if (peers.size() == ctx->shown.size())
    {
        bool same = true;
        for (size_t i = 0; i < peers.size(); ++i)
            if (peers[i].streamId != ctx->shown[i].streamId ||
                peers[i].from != ctx->shown[i].from ||
                peers[i].codec != ctx->shown[i].codec ||
                peers[i].rate != ctx->shown[i].rate)
                same = false;
        if (same) return;
    }

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    ctx->shown = peers;

    int reselect = -1;
    for (size_t i = 0; i < peers.size(); ++i)
    {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)peers[i].Label().c_str());
        if (selectedId && peers[i].streamId == selectedId) reselect = (int)i;
    }
    if (peers.empty())
        SendMessageW(list, LB_ADDSTRING, 0,
                     (LPARAM)L"nothing has announced itself yet");
    else if (reselect >= 0)
        SendMessageW(list, LB_SETCURSEL, (WPARAM)reselect, 0);

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

static void NetRecvNote(HWND dlg)
{
    const int protocol = (int)SendDlgItemMessageW(dlg, IDC_NR_PROTOCOL,
                                                  CB_GETCURSEL, 0, 0);
    std::wstring note;
    if (protocol == NP_VBAN)
        note = L"VBAN senders are found by being asked rather than by "
               L"announcing themselves, so the list holds the hosts that "
               L"answered. One host can send several streams to the same port, "
               L"and they are told apart by name, so the stream name is what "
               L"decides which one this block takes. Leave it blank to take "
               L"whatever arrives.";
    else
        note = L"A FastRoute sender announces itself twice a second, so the "
               L"list fills itself in. Choosing a row fills in the rest. The "
               L"name is what is followed rather than the address: a sender "
               L"that is restarted is still the same stream.";
    note += L" Name the machine that is sending and this block asks it for "
            L"the audio, so no port has to be opened on this side. Leave it "
            L"blank for a sender on a group, which is joined instead. The "
            L"buffer is how unevenly the network is allowed to deliver before "
            L"it is audible; more of it is more delay.";
    SetDlgItemTextW(dlg, IDC_NR_NOTE, note.c_str());
}

// Choosing a row fills in everything under it, so that the address, the port
// and the name all say what was chosen rather than having to be copied across
// by hand.
static void TakeNetChoice(HWND dlg, NetRecvCtx* ctx)
{
    const int sel = (int)SendDlgItemMessageW(dlg, IDC_NR_FOUND, LB_GETCURSEL, 0, 0);
    if (sel < 0 || (size_t)sel >= ctx->shown.size()) return;
    const net::Peer& p = ctx->shown[(size_t)sel];

    ctx->chosenId = p.streamId;
    SetDlgItemTextW(dlg, IDC_NR_NAME, p.name.c_str());
    // A sender on a group is joined on that group; one that offered no group
    // can only be taken from wherever it was heard.
    SetDlgItemTextW(dlg, IDC_NR_ADDRESS,
                    net::AddrText(p.group ? p.group : p.from).c_str());
    SetDlgItemInt(dlg, IDC_NR_PORT, p.port ? p.port : net::kDataPort, FALSE);
}

static INT_PTR CALLBACK NetRecvDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    NetRecvCtx* ctx = reinterpret_cast<NetRecvCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        ctx = reinterpret_cast<NetRecvCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        Block* b = ctx->block;

        for (int p = 0; p < kNetProtocolCount; ++p)
            SendDlgItemMessageW(dlg, IDC_NR_PROTOCOL, CB_ADDSTRING, 0,
                                (LPARAM)NetProtocolName(p));
        int protocol = b->netProtocol;
        if (protocol < 0 || protocol >= kNetProtocolCount) protocol = NP_FASTROUTE;
        SendDlgItemMessageW(dlg, IDC_NR_PROTOCOL, CB_SETCURSEL, (WPARAM)protocol, 0);

        SetDlgItemTextW(dlg, IDC_NR_ADDRESS, b->netAddress.c_str());
        SetDlgItemTextW(dlg, IDC_NR_NAME, b->netStreamName.c_str());
        if (b->netPort > 0) SetDlgItemInt(dlg, IDC_NR_PORT, b->netPort, FALSE);
        else SetDlgItemTextW(dlg, IDC_NR_PORT, L"");

        SetupRange(dlg, IDC_NR_BUFFER, IDC_NR_BUFFER_TEXT,
                   kNetBufferMsMin, kNetBufferMsMax, b->netBufferMs, L" ms", 25);

        ctx->chosenId = b->netSourceId;
        ctx->listener.reset(new net::Listener());
        FillNetFound(dlg, ctx);
        SetTimer(dlg, kNetListTimer, kNetListEveryMs, nullptr);

        NetRecvNote(dlg);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED,
                       b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == kNetListTimer && ctx) FillNetFound(dlg, ctx);
        return TRUE;

    case WM_HSCROLL:
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_NR_BUFFER)
            UpdateSliderText(dlg, IDC_NR_BUFFER, IDC_NR_BUFFER_TEXT, L" ms");
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_NR_FOUND &&
            (HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK))
        {
            if (ctx) TakeNetChoice(dlg, ctx);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_NR_REFRESH)
        {
            if (ctx && ctx->listener)
            {
                ctx->listener->Poke();
                FillNetFound(dlg, ctx);
            }
            SetFocus(GetDlgItem(dlg, IDC_NR_FOUND));
            return TRUE;
        }
        if (LOWORD(wp) == IDC_NR_PROTOCOL && HIWORD(wp) == CBN_SELCHANGE)
        {
            NetRecvNote(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            Block* b = ctx->block;
            const int protocol = (int)SendDlgItemMessageW(dlg, IDC_NR_PROTOCOL,
                                                          CB_GETCURSEL, 0, 0);
            if (protocol >= 0) b->netProtocol = protocol;
            b->netAddress = GetDlgText(dlg, IDC_NR_ADDRESS);
            b->netStreamName = GetDlgText(dlg, IDC_NR_NAME);
            BOOL ok = FALSE;
            const UINT port = GetDlgItemInt(dlg, IDC_NR_PORT, &ok, FALSE);
            b->netPort = ok ? (int)port : 0;
            b->netBufferMs = ReadSlider(dlg, IDC_NR_BUFFER);
            b->netSourceId = ctx->chosenId;
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;

    case WM_DESTROY:
        KillTimer(dlg, kNetListTimer);
        // The listener owns a thread and a socket, and the dialog is the only
        // reason either exists.
        if (ctx) ctx->listener.reset();
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Opus, in more detail
//
// On a dialog of its own rather than six more rows on the Net Send block's,
// which already has eleven controls on it. Bitrate stays out there because it
// is the one anybody changes; these are the ones that are set once for a
// particular link and then left alone.
// ---------------------------------------------------------------------------

static void NetOpusNote(HWND dlg)
{
    const int loss = ReadSlider(dlg, IDC_NO_LOSS);
    const bool dtx = IsDlgButtonChecked(dlg, IDC_NO_DTX) == BST_CHECKED;

    std::wstring note;
    if (loss > 0)
        note = FormatW(L"Opus will put a low rate copy of each packet inside "
                       L"the next one, enough to survive %d%% of them going "
                       L"missing, and spend part of the bitrate on it. The "
                       L"receiver rebuilds a lost packet from its successor "
                       L"instead of guessing at it. ", loss);
    else
        note = L"With no protection, a lost packet is guessed at from the ones "
               L"before it, and none of the bitrate is spent on redundancy. "
               L"Right on a network that does not lose anything. ";

    if (dtx)
        note += L"Sending nothing during silence saves a great deal on a mostly "
                L"quiet microphone, but the silence then stops arriving rather "
                L"than arriving as silence, which anything measuring the far "
                L"end will notice.";
    else
        note += L"Complexity costs processor time on the sending machine only; "
                L"10 is Opus's own default and is not expensive next to the "
                L"rest of a session.";
    SetDlgItemTextW(dlg, IDC_NO_NOTE, note.c_str());
}

static INT_PTR CALLBACK NetOpusDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        SetupRange(dlg, IDC_NO_LOSS, IDC_NO_LOSS_TEXT,
                   kNetOpusLossMin, kNetOpusLossMax, b->netOpusLossPct, L"%", 5);
        SetupRange(dlg, IDC_NO_CPLX, IDC_NO_CPLX_TEXT,
                   kNetOpusComplexityMin, kNetOpusComplexityMax,
                   b->netOpusComplexity, L"", 1);

        for (int i = 0; i < kNetOpusSignalCount; ++i)
            SendDlgItemMessageW(dlg, IDC_NO_SIGNAL, CB_ADDSTRING, 0,
                                (LPARAM)NetOpusSignalName(i));
        int sig = b->netOpusSignal;
        if (sig < 0 || sig >= kNetOpusSignalCount) sig = NOS_AUTO;
        SendDlgItemMessageW(dlg, IDC_NO_SIGNAL, CB_SETCURSEL, (WPARAM)sig, 0);

        for (int i = 0; i < kNetOpusRateModeCount; ++i)
            SendDlgItemMessageW(dlg, IDC_NO_RATEMODE, CB_ADDSTRING, 0,
                                (LPARAM)NetOpusRateModeName(i));
        int mode = b->netOpusRateMode;
        if (mode < 0 || mode >= kNetOpusRateModeCount) mode = NOR_VBR;
        SendDlgItemMessageW(dlg, IDC_NO_RATEMODE, CB_SETCURSEL, (WPARAM)mode, 0);

        CheckDlgButton(dlg, IDC_NO_DTX,
                       b->netOpusDtx ? BST_CHECKED : BST_UNCHECKED);
        NetOpusNote(dlg);
        return TRUE;
    }

    case WM_HSCROLL:
    {
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_NO_LOSS)
            UpdateSliderText(dlg, IDC_NO_LOSS, IDC_NO_LOSS_TEXT, L"%");
        else if (id == IDC_NO_CPLX)
            UpdateSliderText(dlg, IDC_NO_CPLX, IDC_NO_CPLX_TEXT, L"");
        NetOpusNote(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_NO_DTX)
        {
            NetOpusNote(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK)
        {
            b->netOpusLossPct = ReadSlider(dlg, IDC_NO_LOSS);
            b->netOpusComplexity = ReadSlider(dlg, IDC_NO_CPLX);
            const int sig = (int)SendDlgItemMessageW(dlg, IDC_NO_SIGNAL,
                                                      CB_GETCURSEL, 0, 0);
            if (sig >= 0) b->netOpusSignal = sig;
            const int mode = (int)SendDlgItemMessageW(dlg, IDC_NO_RATEMODE,
                                                       CB_GETCURSEL, 0, 0);
            if (mode >= 0) b->netOpusRateMode = mode;
            b->netOpusDtx = IsDlgButtonChecked(dlg, IDC_NO_DTX) == BST_CHECKED;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Net Send
// ---------------------------------------------------------------------------

// Which controls mean anything depends on the other controls, so this is one
// function rather than scattered enabling: VBAN can only carry PCM, a bitrate
// is only Opus's business, and the note has to say what the settings actually
// come to in packets and kilobits.
static void SyncNetSendFields(HWND dlg)
{
    const int protocol = (int)SendDlgItemMessageW(dlg, IDC_NS_PROTOCOL,
                                                  CB_GETCURSEL, 0, 0);
    const int mode = (int)SendDlgItemMessageW(dlg, IDC_NS_MODE, CB_GETCURSEL, 0, 0);
    int codec = (int)SendDlgItemMessageW(dlg, IDC_NS_CODEC, CB_GETCURSEL, 0, 0);
    const bool vban = (protocol == NP_VBAN);
    if (vban)
    {
        // Not merely disabled: shown as what it will actually send, so the
        // dialog never reads back something untrue.
        codec = net::NC_PCM16;
        SendDlgItemMessageW(dlg, IDC_NS_CODEC, CB_SETCURSEL, (WPARAM)codec, 0);
    }

    const bool opus = (codec == net::NC_OPUS);
    // The hop limit is a multicast setting. Sending to a named host uses the
    // system's own TTL, so showing the control as live would be promising
    // something it does not do.
    const bool group = (mode == NM_MULTICAST);
    EnableWindow(GetDlgItem(dlg, IDC_NS_TTL), group);
    EnableWindow(GetDlgItem(dlg, IDC_NS_TTL_LABEL), group);
    EnableWindow(GetDlgItem(dlg, IDC_NS_TTL_TEXT), group);
    EnableWindow(GetDlgItem(dlg, IDC_NS_CODEC), !vban);
    EnableWindow(GetDlgItem(dlg, IDC_NS_CODEC_LABEL), !vban);
    EnableWindow(GetDlgItem(dlg, IDC_NS_KBPS), opus);
    EnableWindow(GetDlgItem(dlg, IDC_NS_KBPS_LABEL), opus);
    EnableWindow(GetDlgItem(dlg, IDC_NS_KBPS_TEXT), opus);
    EnableWindow(GetDlgItem(dlg, IDC_NS_OPUSMORE), opus);

    SetDlgItemTextW(dlg, IDC_NS_ADDRESS_LABEL,
                    mode == NM_UNICAST
                        ? L"A&ddress to push to, or blank to let receivers ask:"
                        : L"Group a&ddress, or blank for the usual one:");

    UpdateSliderText(dlg, IDC_NS_TTL, IDC_NS_TTL_TEXT, L"");
    UpdateSliderText(dlg, IDC_NS_KBPS, IDC_NS_KBPS_TEXT, L" kbps");

    // What a packet really holds. The asked-for length is a wish: a datagram
    // has to arrive whole, so a long packet of deep samples gets cut down, and
    // saying so here is better than leaving somebody to wonder why 10 ms
    // behaves like 7.
    const int askedMs = ReadSlider(dlg, IDC_NS_PACKET);
    const int rate = kDefaultRate;
    const int frames = net::FramesPerPacketFor(codec, rate, askedMs);
    const int realMs10 = frames * 10000 / rate;         // tenths of a ms
    SetSliderReadout(dlg, IDC_NS_PACKET, IDC_NS_PACKET_TEXT,
                     FormatW(L"%d ms asked, %d frames (%d.%d ms)", askedMs,
                             frames, realMs10 / 10, realMs10 % 10));

    const int kbps = net::NetCodecKbps(codec, rate,
                                       ReadSlider(dlg, IDC_NS_KBPS));
    std::wstring note = FormatW(
        L"At 48 kHz this is about %d kbps, or %d packets a second. ",
        kbps, frames > 0 ? rate / frames : 0);
    if (vban)
        note += L"VBAN carries PCM and two of VB-Audio's own codecs, neither of "
                L"which this can produce, so a VBAN stream goes out as 16 bit "
                L"PCM whatever the codec says. ";
    if (opus)
    {
        Block* ob = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));
        if (ob)
            note += FormatW(L"Opus is protecting against %d%% loss, at "
                            L"complexity %d, %s bitrate%s. ",
                            ob->netOpusLossPct, ob->netOpusComplexity,
                            NetOpusRateModeName(ob->netOpusRateMode),
                            ob->netOpusDtx ? L", silent when the input is" : L"");
    }
    if (kbps > 1000)
        note += L"That is a lot for a wireless link; Opus or FLAC would be "
                L"kinder to one. ";
    if (mode == NM_MULTICAST)
        note += L"A group costs the sender the same however many are "
                L"listening, but it is a local network mechanism: no internet "
                L"provider carries multicast, and a group address has to be "
                L"between 224.0.0.0 and 239.255.255.255. To reach a machine "
                L"somewhere else, send to one host instead.";
    else
        note += L"Leave the address blank and this sender waits on its port for "
                L"receivers to ask, which is the usual way: start this, then "
                L"name this machine in a Net Receive block on the other one. "
                L"That works across subnets, over a VPN and over the internet, "
                L"and needs no port opened on the receiving side, because the "
                L"asking is outbound from there. This machine's port does have "
                L"to be reachable. An address pushes at a receiver instead, for "
                L"one that cannot ask. Nothing here is encrypted - anything "
                L"that finds the port can listen, and anything that reaches it "
                L"will be taken as a sender.";
    SetDlgItemTextW(dlg, IDC_NS_NOTE, note.c_str());
}

static INT_PTR CALLBACK NetSendDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        for (int p = 0; p < kNetProtocolCount; ++p)
            SendDlgItemMessageW(dlg, IDC_NS_PROTOCOL, CB_ADDSTRING, 0,
                                (LPARAM)NetProtocolName(p));
        int protocol = b->netProtocol;
        if (protocol < 0 || protocol >= kNetProtocolCount) protocol = NP_FASTROUTE;
        SendDlgItemMessageW(dlg, IDC_NS_PROTOCOL, CB_SETCURSEL, (WPARAM)protocol, 0);

        for (int m = 0; m < kNetModeCount; ++m)
            SendDlgItemMessageW(dlg, IDC_NS_MODE, CB_ADDSTRING, 0,
                                (LPARAM)NetModeName(m));
        int mode = b->netMode;
        if (mode < 0 || mode >= kNetModeCount) mode = NM_MULTICAST;
        SendDlgItemMessageW(dlg, IDC_NS_MODE, CB_SETCURSEL, (WPARAM)mode, 0);

        for (int c = 0; c < net::kNetCodecCount; ++c)
            SendDlgItemMessageW(dlg, IDC_NS_CODEC, CB_ADDSTRING, 0,
                                (LPARAM)net::NetCodecName(c));
        int codec = b->netCodec;
        if (codec < 0 || codec >= net::kNetCodecCount) codec = net::NC_PCM16;
        SendDlgItemMessageW(dlg, IDC_NS_CODEC, CB_SETCURSEL, (WPARAM)codec, 0);

        SetDlgItemTextW(dlg, IDC_NS_ADDRESS, b->netAddress.c_str());
        SetDlgItemTextW(dlg, IDC_NS_NAME, b->netStreamName.c_str());
        if (b->netPort > 0) SetDlgItemInt(dlg, IDC_NS_PORT, b->netPort, FALSE);
        else SetDlgItemTextW(dlg, IDC_NS_PORT, L"");

        SetupRange(dlg, IDC_NS_PACKET, IDC_NS_PACKET_TEXT,
                   kNetPacketMsMin, kNetPacketMsMax, b->netPacketMs, L" ms", 2);
        SetupRange(dlg, IDC_NS_KBPS, IDC_NS_KBPS_TEXT,
                   kNetOpusKbpsMin, kNetOpusKbpsMax, b->netOpusKbps, L" kbps", 32);
        SetupRange(dlg, IDC_NS_TTL, IDC_NS_TTL_TEXT,
                   kNetTtlMin, kNetTtlMax, b->netTtl, L"", 1);

        SyncNetSendFields(dlg);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED,
                       b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_HSCROLL:
        SyncNetSendFields(dlg);
        return TRUE;

    case WM_COMMAND:
        if (HIWORD(wp) == CBN_SELCHANGE &&
            (LOWORD(wp) == IDC_NS_PROTOCOL || LOWORD(wp) == IDC_NS_MODE ||
             LOWORD(wp) == IDC_NS_CODEC))
        {
            SyncNetSendFields(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_NS_OPUSMORE)
        {
            DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_NET_OPUS), dlg,
                            NetOpusDlgProc, reinterpret_cast<LPARAM>(b));
            SyncNetSendFields(dlg);
            SetFocus(GetDlgItem(dlg, IDC_NS_OPUSMORE));
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            const int protocol = (int)SendDlgItemMessageW(dlg, IDC_NS_PROTOCOL,
                                                          CB_GETCURSEL, 0, 0);
            if (protocol >= 0) b->netProtocol = protocol;
            const int mode = (int)SendDlgItemMessageW(dlg, IDC_NS_MODE,
                                                      CB_GETCURSEL, 0, 0);
            if (mode >= 0) b->netMode = mode;
            const int codec = (int)SendDlgItemMessageW(dlg, IDC_NS_CODEC,
                                                       CB_GETCURSEL, 0, 0);
            if (codec >= 0) b->netCodec = codec;

            b->netAddress = GetDlgText(dlg, IDC_NS_ADDRESS);
            b->netStreamName = GetDlgText(dlg, IDC_NS_NAME);
            BOOL ok = FALSE;
            const UINT port = GetDlgItemInt(dlg, IDC_NS_PORT, &ok, FALSE);
            b->netPort = ok ? (int)port : 0;
            b->netPacketMs = ReadSlider(dlg, IDC_NS_PACKET);
            b->netOpusKbps = ReadSlider(dlg, IDC_NS_KBPS);
            b->netTtl = ReadSlider(dlg, IDC_NS_TTL);
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// The two sliders are one number, so the line under them says what the number
// actually comes to. Reading "3 semitones" and "-50 cents" back separately
// leaves whoever is listening to work out that it is two and a half semitones
// up, every time either slider moves.
static void UpdatePitchSummary(HWND dlg)
{
    const int semis = ReadSlider(dlg, IDC_PITCH_SEMIS);
    const int cents = ReadSlider(dlg, IDC_PITCH_CENTS);
    const int q = (int)SendDlgItemMessageW(dlg, IDC_PITCH_QUALITY, CB_GETCURSEL, 0, 0);

    std::wstring line = PitchText(semis, cents);
    if (semis != 0 || cents != 0)
        line += FormatW(L", delaying by about %d ms",
                        PitchQualityMs(q < 0 ? PQ_BEST : q));
    else
        line += L", so the block does nothing and delays nothing";
    SetDlgItemTextW(dlg, IDC_PITCH_SUMMARY, line.c_str());
}

static INT_PTR CALLBACK PitchDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetupRange(dlg, IDC_PITCH_SEMIS, IDC_PITCH_SEMIS_TEXT,
                   kPitchSemitonesMin, kPitchSemitonesMax, b->pitchSemitones,
                   L" semitones", 12);
        SetupRange(dlg, IDC_PITCH_CENTS, IDC_PITCH_CENTS_TEXT,
                   kPitchCentsMin, kPitchCentsMax, b->pitchCents, L" cents", 10);
        for (int q = 0; q < kPitchQualityCount; ++q)
            SendDlgItemMessageW(dlg, IDC_PITCH_QUALITY, CB_ADDSTRING, 0,
                                (LPARAM)PitchQualityName(q));
        {
            int q = b->pitchQuality;
            if (q < 0 || q >= kPitchQualityCount) q = PQ_BEST;
            SendDlgItemMessageW(dlg, IDC_PITCH_QUALITY, CB_SETCURSEL, (WPARAM)q, 0);
        }
        UpdatePitchSummary(dlg);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_PITCH_SEMIS)
            UpdateSliderText(dlg, IDC_PITCH_SEMIS, IDC_PITCH_SEMIS_TEXT, L" semitones");
        else if (id == IDC_PITCH_CENTS)
            UpdateSliderText(dlg, IDC_PITCH_CENTS, IDC_PITCH_CENTS_TEXT, L" cents");
        else return TRUE;
        UpdatePitchSummary(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PITCH_QUALITY && HIWORD(wp) == CBN_SELCHANGE)
        {
            UpdatePitchSummary(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->pitchSemitones = ReadSlider(dlg, IDC_PITCH_SEMIS);
            b->pitchCents     = ReadSlider(dlg, IDC_PITCH_CENTS);
            const int q = (int)SendDlgItemMessageW(dlg, IDC_PITCH_QUALITY,
                                                   CB_GETCURSEL, 0, 0);
            if (q >= 0) b->pitchQuality = q;
            b->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK LimitDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Block* b = reinterpret_cast<Block*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ReverseSliderKeys(dlg);
        b = reinterpret_cast<Block*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetupRange(dlg, IDC_LIM_CEIL, IDC_LIM_CEIL_TEXT,
                   kLimCeilingMin, kLimCeilingMax, b->limCeilingDb, L" dB", 1);
        SetupRange(dlg, IDC_LIM_RELEASE, IDC_LIM_RELEASE_TEXT,
                   kLimReleaseMin, kLimReleaseMax, b->limReleaseMs, L" ms", 25);
        SetupRange(dlg, IDC_LIM_LOOK, IDC_LIM_LOOK_TEXT,
                   kLimLookaheadMin, kLimLookaheadMax, b->limLookaheadMs, L" ms", 1);
        CheckDlgButton(dlg, IDC_BLOCK_ENABLED, b->enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IDC_LIM_CEIL)
            UpdateSliderText(dlg, IDC_LIM_CEIL, IDC_LIM_CEIL_TEXT, L" dB");
        else if (id == IDC_LIM_RELEASE)
            UpdateSliderText(dlg, IDC_LIM_RELEASE, IDC_LIM_RELEASE_TEXT, L" ms");
        else if (id == IDC_LIM_LOOK)
            UpdateSliderText(dlg, IDC_LIM_LOOK, IDC_LIM_LOOK_TEXT, L" ms");
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_APPLY)
        {
            b->limCeilingDb   = ReadSlider(dlg, IDC_LIM_CEIL);
            b->limReleaseMs   = ReadSlider(dlg, IDC_LIM_RELEASE);
            b->limLookaheadMs = ReadSlider(dlg, IDC_LIM_LOOK);
            b->enabled        = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// ASIO blocks
//
// One dialog for both directions, because the questions are the same and only
// the word changes: which driver, and which two of its channels.
//
// The channel lists cannot be filled without opening the driver, and opening
// a driver takes the audio hardware from whatever is using it. So nothing is
// opened when the dialog appears - only when somebody picks a driver, which
// is the moment they have asked for the question to be answered. Until then
// the lists offer plain numbers, which is also what they fall back to when
// the driver turns out to be busy.
// ---------------------------------------------------------------------------
struct AsioCtx
{
    Block* block;
    bool   input;
    std::vector<std::wstring> drivers;
    AsioFacts facts;
};

// Channels are numbered from one here and from zero inside. Every interface is
// labelled from one on its front panel, and the front panel is the thing
// somebody is looking at while they use this.
static void FillAsioChannels(HWND dlg, AsioCtx* ctx)
{
    const std::vector<std::wstring>& names =
        ctx->input ? ctx->facts.inNames : ctx->facts.outNames;

    const int wantL = ctx->input ? ctx->block->asioInLeft : ctx->block->asioOutLeft;
    const int wantR = ctx->input ? ctx->block->asioInRight : ctx->block->asioOutRight;

    for (int side = 0; side < 2; ++side)
    {
        const int id = side ? IDC_AS_RIGHT : IDC_AS_LEFT;
        const int want = side ? wantR : wantL;
        HWND cb = GetDlgItem(dlg, id);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);

        // With no driver opened there is nothing to say how many channels
        // exist, so a plain stereo pair is offered and a wider interface
        // fills the list properly the moment its driver is picked.
        const int count = names.empty() ? 2 : (int)names.size();
        for (int i = 0; i < count; ++i)
        {
            std::wstring label = names.empty()
                ? FormatW(L"Channel %d", i + 1)
                : FormatW(L"%d. %s", i + 1, names[(size_t)i].c_str());
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        }
        const int sel = (want >= 0 && want < count) ? want : (side ? 1 : 0);
        SendMessageW(cb, CB_SETCURSEL, sel < count ? sel : 0, 0);
    }
}

static std::wstring AsioDriverAt(AsioCtx* ctx, HWND dlg)
{
    const int sel = (int)SendDlgItemMessageW(dlg, IDC_AS_DRIVER, CB_GETCURSEL, 0, 0);
    if (sel <= 0) return std::wstring();          // item 0 is "(none chosen)"
    const size_t i = (size_t)(sel - 1);
    return (i < ctx->drivers.size()) ? ctx->drivers[i] : std::wstring();
}

// What the driver had to say, in a line somebody can read. This is the only
// place the rate and the buffer appear, and both matter: the rate because an
// ASIO driver will not be talked out of its own, and the buffer because it is
// the session's latency and is changed in the driver's panel, not here.
static void UpdateAsioNote(HWND dlg, AsioCtx* ctx)
{
    const std::wstring driver = AsioDriverAt(ctx, dlg);
    if (driver.empty())
    {
        SetDlgItemTextW(dlg, IDC_AS_NOTE,
            L"No driver chosen. Unlike a Windows audio device there is no "
            L"default ASIO driver to fall back on, so this block will not "
            L"start until one is picked.");
        EnableWindow(GetDlgItem(dlg, IDC_AS_PANEL), FALSE);
        return;
    }

    EnableWindow(GetDlgItem(dlg, IDC_AS_PANEL), TRUE);

    if (!ctx->facts.ok)
    {
        SetDlgItemTextW(dlg, IDC_AS_NOTE, ctx->facts.why.c_str());
        return;
    }

    const size_t have = ctx->input ? ctx->facts.inNames.size()
                                   : ctx->facts.outNames.size();
    if (have == 0)
    {
        SetDlgItemTextW(dlg, IDC_AS_NOTE, FormatW(
            L"%s has no %s, so there is nothing for this block to use. It has "
            L"%d inputs and %d outputs.", driver.c_str(),
            ctx->input ? L"inputs" : L"outputs",
            (int)ctx->facts.inNames.size(),
            (int)ctx->facts.outNames.size()).c_str());
        return;
    }

    const double ms = ctx->facts.rate > 0
        ? (double)ctx->facts.bufferFrames * 1000.0 / (double)ctx->facts.rate : 0.0;

    SetDlgItemTextW(dlg, IDC_AS_NOTE, FormatW(
        L"%s: %d inputs, %d outputs, running at %d Hz with a %d frame buffer, "
        L"which is %.1f ms. Both the buffer size and the sample rate belong to "
        L"the driver and are changed in its own control panel. A session takes "
        L"its rate from this driver when an ASIO output is its first output "
        L"block; otherwise the driver has to be set to the rate the session is "
        L"already using.",
        driver.c_str(), (int)ctx->facts.inNames.size(),
        (int)ctx->facts.outNames.size(), ctx->facts.rate,
        ctx->facts.bufferFrames, ms).c_str());
}

static void FillAsioDrivers(HWND dlg, AsioCtx* ctx, const std::wstring& keep)
{
    HWND cb = GetDlgItem(dlg, IDC_AS_DRIVER);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);

    // Reading the registry only. No driver is loaded by this.
    ctx->drivers = EnumAsioDrivers();

    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"(none chosen)");
    int select = 0;
    for (size_t i = 0; i < ctx->drivers.size(); ++i)
    {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)ctx->drivers[i].c_str());
        if (!keep.empty() && _wcsicmp(ctx->drivers[i].c_str(), keep.c_str()) == 0)
            select = (int)i + 1;
    }

    // A driver named in a saved session that is no longer installed is kept in
    // the list rather than silently dropped, because the block still refers to
    // it and somebody should be able to see what is missing.
    if (select == 0 && !keep.empty())
    {
        ctx->drivers.push_back(keep);
        SendMessageW(cb, CB_ADDSTRING, 0,
                     (LPARAM)FormatW(L"%s (not installed)", keep.c_str()).c_str());
        select = (int)ctx->drivers.size();
    }

    SendMessageW(cb, CB_SETCURSEL, select, 0);
}

static INT_PTR CALLBACK AsioDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    AsioCtx* ctx = reinterpret_cast<AsioCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        ctx = reinterpret_cast<AsioCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetWindowTextW(dlg, ctx->input ? L"ASIO Input Block" : L"ASIO Output Block");
        SetDlgItemTextW(dlg, IDC_AS_LEFT_LABEL,
                        ctx->input ? L"&Left input channel:" : L"&Left output channel:");
        SetDlgItemTextW(dlg, IDC_AS_RIGHT_LABEL,
                        ctx->input ? L"&Right input channel:" : L"&Right output channel:");

        FillAsioDrivers(dlg, ctx, ctx->block->deviceId);
        // The driver named in the block is asked about straight away, because
        // it is already this block's driver: nothing is being taken that the
        // block was not going to take anyway.
        if (!ctx->block->deviceId.empty())
            AsioQueryDriver(ctx->block->deviceId, &ctx->facts);
        FillAsioChannels(dlg, ctx);
        UpdateAsioNote(dlg, ctx);

        CheckDlgButton(dlg, IDC_BLOCK_ENABLED,
                       ctx->block->enabled ? BST_CHECKED : BST_UNCHECKED);
        SetupVolumeSlider(dlg, ctx->block->volumePct);
        return TRUE;
    }

    case WM_HSCROLL:
        OnVolumeScroll(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_AS_DRIVER:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                const std::wstring driver = AsioDriverAt(ctx, dlg);
                ctx->facts = AsioFacts();
                if (!driver.empty()) AsioQueryDriver(driver, &ctx->facts);
                // A different interface has different channels, so the
                // choices start again rather than keeping numbers that meant
                // something on the last one.
                if (ctx->input) { ctx->block->asioInLeft = 0; ctx->block->asioInRight = 1; }
                else            { ctx->block->asioOutLeft = 0; ctx->block->asioOutRight = 1; }
                FillAsioChannels(dlg, ctx);
                UpdateAsioNote(dlg, ctx);
            }
            return TRUE;

        case IDC_AS_REFRESH:
            FillAsioDrivers(dlg, ctx, AsioDriverAt(ctx, dlg));
            UpdateAsioNote(dlg, ctx);
            SetFocus(GetDlgItem(dlg, IDC_AS_DRIVER));
            return TRUE;

        case IDC_AS_PANEL:
        {
            const std::wstring driver = AsioDriverAt(ctx, dlg);
            if (driver.empty()) return TRUE;
            AsioShowControlPanel(driver, dlg);
            // The buffer size or the rate may be different now, so what the
            // note says is asked again rather than left as it was.
            AsioQueryDriver(driver, &ctx->facts);
            UpdateAsioNote(dlg, ctx);
            return TRUE;
        }

        case IDC_APPLY:
        case IDOK:
        {
            const std::wstring driver = AsioDriverAt(ctx, dlg);
            ctx->block->deviceId = driver;
            ctx->block->deviceName = driver;   // an ASIO driver's name is its id

            const int l = (int)SendDlgItemMessageW(dlg, IDC_AS_LEFT, CB_GETCURSEL, 0, 0);
            const int r = (int)SendDlgItemMessageW(dlg, IDC_AS_RIGHT, CB_GETCURSEL, 0, 0);
            if (ctx->input)
            {
                ctx->block->asioInLeft = (l >= 0) ? l : 0;
                ctx->block->asioInRight = (r >= 0) ? r : 1;
            }
            else
            {
                ctx->block->asioOutLeft = (l >= 0) ? l : 0;
                ctx->block->asioOutRight = (r >= 0) ? r : 1;
            }

            ctx->block->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            ctx->block->volumePct = ReadVolumeSlider(dlg);

            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

bool EditBlockParams(HWND parent, Block& b)
{
    if (b.typeId == BT_INPUT_APP)
    {
        AppCtx ctx{ &b, {} };
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_APP), parent,
                               AppDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
    }
    if (b.typeId == BT_INPUT_STREAM)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_STREAM), parent,
                               StreamDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_INPUT_SYSTEM)
    {
        SysCtx ctx{ &b, {}, {} };
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_SYSTEM), parent,
                               SystemDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
    }
    if (b.typeId == BT_INPUT_ASIO || b.typeId == BT_OUT_ASIO)
    {
        AsioCtx ctx{ &b, b.typeId == BT_INPUT_ASIO, {}, {} };
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_ASIO), parent,
                               AsioDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
    }
    if (b.typeId == BT_INPUT_DEVICE || b.typeId == BT_OUTPUT_DEVICE)
    {
        DevCtx ctx{ &b, b.typeId == BT_OUTPUT_DEVICE, {} };
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_DEVICE), parent,
                               DevDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
    }
    if (b.typeId == BT_EFFECT_GAIN)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_GAIN), parent,
                               GainDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_SPEEX || b.typeId == BT_FX_RNNOISE)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_DENOISE), parent,
                               DenoiseDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_TONE)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_TONE), parent,
                               ToneDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_PARA)
    {
        // Edited in place, so a Cancel has to put the block back as it was -
        // as far back as the last Apply, and no further.
        Block copy = b;
        g_editLive = &b;
        g_editSaved = &copy;
        const bool ok = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_PARA),
                                        parent, ParaDlgProc,
                                        reinterpret_cast<LPARAM>(&b)) == IDOK;
        g_editLive = nullptr;
        g_editSaved = nullptr;
        if (ok) return true;
        b = copy;
        return false;
    }
    if (b.typeId == BT_FX_EQ10)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_EQ10), parent,
                               Eq10DlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_CHANNELS)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_CHANNELS), parent,
                               ChannelsDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_COMP)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_COMP), parent,
                               CompDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_GATE)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_GATE), parent,
                               GateDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_LIMIT)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_LIMIT), parent,
                               LimitDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_PITCH)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_PITCH), parent,
                               PitchDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_INPUT_NET)
    {
        // The context carries the discovery listener, which lives exactly as
        // long as the dialog: a thread and a socket are too much to hold open
        // for a window nobody has got in front of them.
        NetRecvCtx ctx;
        ctx.block = &b;
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_NETRECV), parent,
                               NetRecvDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
    }
    if (b.typeId == BT_OUT_NET)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_NETSEND), parent,
                               NetSendDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_NORM)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_NORM), parent,
                               NormDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_BUFFER)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_BUFFER), parent,
                               BufferDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_CLIPMON)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_CLIPMON), parent,
                               ClipMonDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    if (b.typeId == BT_FX_PLUGIN)
    {
        return EditPluginBlock(parent, b);
    }
    if (b.typeId == BT_OUT_RECORDER)
    {
        return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_RECORDER), parent,
                               RecorderDlgProc, reinterpret_cast<LPARAM>(&b)) == IDOK;
    }
    InfoBox(parent, L"This block has no parameters.");
    return false;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Error log
//
// A read only multiline edit rather than a list: an error is often a paragraph,
// and an edit control wraps it, lets it be selected and copied, and lets a
// screen reader walk it by line, word or character like any other text.
// ---------------------------------------------------------------------------

static std::wstring ErrorLogText()
{
    std::vector<LogEntry> entries = ErrorLogEntries();
    if (entries.empty())
        return L"No errors have been reported since FastRoute started.";

    std::wstring out;
    for (const LogEntry& e : entries)
    {
        if (!out.empty()) out += L"\r\n\r\n";
        out += e.when + L"  " + e.text;
    }
    return out;
}

static void FillErrorLog(HWND dlg)
{
    std::wstring text = ErrorLogText();
    SetDlgItemTextW(dlg, IDC_LOG_TEXT, text.c_str());
    // Show the end, which is where the newest entry is.
    SendDlgItemMessageW(dlg, IDC_LOG_TEXT, EM_SETSEL, (WPARAM)text.size(), (LPARAM)text.size());
    SendDlgItemMessageW(dlg, IDC_LOG_TEXT, EM_SCROLLCARET, 0, 0);

    bool any = !ErrorLogEntries().empty();
    EnableWindow(GetDlgItem(dlg, IDC_LOG_COPY), any);
    EnableWindow(GetDlgItem(dlg, IDC_LOG_CLEAR), any);
}

static void CopyToClipboard(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();

    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem)
    {
        void* p = GlobalLock(mem);
        if (p)
        {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            // The clipboard owns the block from here, so it is not freed.
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        }
        else GlobalFree(mem);
    }
    CloseClipboard();
}

static INT_PTR CALLBACK ErrorLogDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg)
    {
    case WM_INITDIALOG:
        FillErrorLog(dlg);
        SetFocus(GetDlgItem(dlg, IDC_LOG_TEXT));
        return FALSE;   // focus set explicitly

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_LOG_COPY:
            CopyToClipboard(dlg, ErrorLogText());
            SetFocus(GetDlgItem(dlg, IDC_LOG_TEXT));
            return TRUE;

        case IDC_LOG_CLEAR:
            ClearErrorLog();
            FillErrorLog(dlg);
            SetFocus(GetDlgItem(dlg, IDC_LOG_TEXT));
            return TRUE;

        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowErrorLogDialog(HWND parent)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ERRORLOG), parent, ErrorLogDlgProc, 0);
}

// ---------------------------------------------------------------------------
// Settings
//
// The hotkey is four checkboxes and a list rather than the standard hotkey
// control. A hotkey control wants you to press the combination and then shows
// it back as drawn text, which is exactly the arrangement a screen reader has
// the most trouble with. Checkboxes and a named key read correctly, and there
// is no moment where you have to press a key to find out what it does.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The key field
//
// The name of the key, typed. It used to be a list of the keys somebody
// thought to offer - letters, digits and function keys - which quietly ruled
// out every arrow, Home and End, the numeric keypad and the media keys, all of
// them perfectly good hotkeys and several of them the obvious choice for a
// transport control. A name has no list to be left off.
//
// Typing rather than pressing is deliberate. A field that captured keystrokes
// would have to swallow Tab to let you bind it, and a field you cannot leave
// without a mouse is not one this application is going to ship. It would also
// be unable to accept any combination the shell takes first, which is most of
// the Windows key ones.
//
// KeyFromName is forgiving about case, spaces and the word "arrow", and
// KeyName's own output always reads back, so a field that was filled from the
// document is always holding something it will accept again.
// ---------------------------------------------------------------------------

static std::wstring TypedKeyText(HWND dlg)
{
    wchar_t buf[64] = { 0 };
    GetDlgItemTextW(dlg, IDC_HK_KEY, buf, 64);
    return Trim(buf);
}

// Said on the way out, because a name that was not understood leaves the key
// unset and a hotkey that quietly does nothing is the worst of the outcomes.
// False means the dialog should stay open.
static bool CheckTypedKey(HWND dlg)
{
    const std::wstring typed = TypedKeyText(dlg);
    if (!typed.empty() && KeyFromName(typed) != 0) return true;

    ErrorBox(dlg, typed.empty()
        ? std::wstring(L"Type the name of a key - a letter or a digit, F1 to "
                       L"F24, Left Arrow, Home, Page Up, Num 7, or a media key "
                       L"such as Media Play.")
        : FormatW(L"There is no key called %s.\r\n\r\nTry a letter or a digit, "
                  L"F1 to F24, Left Arrow, Home, Page Up, Num 7, or a media key "
                  L"such as Media Play.", typed.c_str()));
    SetFocus(GetDlgItem(dlg, IDC_HK_KEY));
    return false;
}

// Reads the dialog back, so the same code decides what the note says and what
// OK stores.
static Hotkey ReadHotkeyFields(HWND dlg)
{
    Hotkey h;
    h.enabled = IsDlgButtonChecked(dlg, IDC_HK_ENABLED) == BST_CHECKED;
    h.mods = 0;
    if (IsDlgButtonChecked(dlg, IDC_HK_CTRL)  == BST_CHECKED) h.mods |= HK_CTRL;
    if (IsDlgButtonChecked(dlg, IDC_HK_ALT)   == BST_CHECKED) h.mods |= HK_ALT;
    if (IsDlgButtonChecked(dlg, IDC_HK_SHIFT) == BST_CHECKED) h.mods |= HK_SHIFT;
    if (IsDlgButtonChecked(dlg, IDC_HK_WIN)   == BST_CHECKED) h.mods |= HK_WIN;

    h.vk = KeyFromName(TypedKeyText(dlg));

    // Settings has no scope radios: its show and hide key has to be global,
    // because when the window is gone there is nothing to press a key in. A
    // missing control reads as unchecked, which lands on global, so this is
    // right for both dialogs without asking which one it is in.
    h.global = IsDlgButtonChecked(dlg, IDC_HK_SCOPE_WINDOW) != BST_CHECKED;
    return h;
}

static void FillHotkeyFields(HWND dlg, const Hotkey& h)
{
    CheckDlgButton(dlg, IDC_HK_ENABLED, h.enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_HK_CTRL,  (h.mods & HK_CTRL)  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_HK_ALT,   (h.mods & HK_ALT)   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_HK_SHIFT, (h.mods & HK_SHIFT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_HK_WIN,   (h.mods & HK_WIN)   ? BST_CHECKED : BST_UNCHECKED);

    // Written back in the spelling this field will read again, so that opening
    // a hotkey and pressing OK without touching anything cannot change it.
    SetDlgItemTextW(dlg, IDC_HK_KEY, h.vk ? KeyName(h.vk).c_str() : L"");

    CheckDlgButton(dlg, IDC_HK_SCOPE_GLOBAL, h.global ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_HK_SCOPE_WINDOW, h.global ? BST_UNCHECKED : BST_CHECKED);
}

// Enables the fields the checkbox governs and writes the running commentary
// underneath. `does` completes "<hotkey> will ...". `doesWindow` is the same
// sentence for a
// key that only works here; nullptr means this dialog offers no choice of
// scope, which is the Settings case.
static void SyncHotkeyFields(HWND dlg, const wchar_t* does, const wchar_t* whenOff,
                             const wchar_t* doesWindow = nullptr)
{
    bool on = IsDlgButtonChecked(dlg, IDC_HK_ENABLED) == BST_CHECKED;
    const int ids[] = { IDC_HK_CTRL, IDC_HK_ALT, IDC_HK_SHIFT, IDC_HK_WIN,
                        IDC_HK_KEY, IDC_HK_SCOPE_GLOBAL, IDC_HK_SCOPE_WINDOW };
    for (int id : ids)
    {
        HWND c = GetDlgItem(dlg, id);
        if (c) EnableWindow(c, on);
    }

    Hotkey h = ReadHotkeyFields(dlg);
    const std::wstring typed = TypedKeyText(dlg);

    std::wstring note;
    if (!on)
        note = whenOff;
    else if (typed.empty())
        note = L"Type the name of a key: a letter or a digit, F1 to F24, Left "
               L"Arrow, Right Arrow, Up Arrow, Down Arrow, Home, End, Page Up, "
               L"Page Down, Insert, Delete, Space, Backspace, Num 0 to Num 9, "
               L"or a media key such as Media Play or Volume Up.";
    else if (h.vk == 0)
        note = FormatW(L"There is no key called %s. Try a letter or a digit, "
                       L"F1 to F24, Left Arrow, Home, Page Up, Num 7, or a "
                       L"media key such as Media Play.", typed.c_str());
    else if (h.mods == 0)
        note = L"Choose at least one of Ctrl, Alt, Shift or Windows. A hotkey "
               L"with no modifier would take that key away from every other "
               L"program on the machine.";
    else if (!h.global && doesWindow)
        note = FormatW(L"%s %s", HotkeyText(h).c_str(), doesWindow);
    else
        note = FormatW(L"%s %s", HotkeyText(h).c_str(), does);
    SetDlgItemTextW(dlg, IDC_HK_NOTE, note.c_str());
}

static void SyncSettingsFields(HWND dlg)
{
    SyncHotkeyFields(dlg,
        L"will show FastRoute when it is hidden, and hide it when it is not, "
        L"from whatever you are using at the time.",
        L"With no global hotkey, FastRoute can only be brought back from its "
        L"icon in the notification area.");
}

// ---------------------------------------------------------------------------
// Plugin folders
//
// Scanning means loading every module in these folders far enough to ask what
// is inside, which is other people's code and takes as long as it takes. It
// runs on its own thread so the dialog stays answerable, and reports back with
// a message rather than touching any control from off the UI thread.
// ---------------------------------------------------------------------------

static const UINT WM_SCAN_DONE = WM_APP + 11;

struct ScanResult
{
    std::vector<PluginEntry> found;
    size_t folders = 0;
};

static void FillPluginPaths(HWND dlg, const std::vector<std::wstring>& paths)
{
    HWND list = GetDlgItem(dlg, IDC_PLUG_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const std::wstring& p : paths)
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)p.c_str());
    if (!paths.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
}

static std::vector<std::wstring> ReadPluginPaths(HWND dlg)
{
    std::vector<std::wstring> out;
    HWND list = GetDlgItem(dlg, IDC_PLUG_LIST);
    const int n = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i)
    {
        const int len = (int)SendMessageW(list, LB_GETTEXTLEN, i, 0);
        if (len <= 0) continue;
        std::wstring s((size_t)len, L'\0');
        SendMessageW(list, LB_GETTEXT, i, (LPARAM)&s[0]);
        out.push_back(s);
    }
    return out;
}

static void SetPluginFoundText(HWND dlg, const std::wstring& text)
{
    SetDlgItemTextW(dlg, IDC_PLUG_FOUND, text.c_str());
}

static void EnablePluginButtons(HWND dlg, bool on)
{
    EnableWindow(GetDlgItem(dlg, IDC_PLUG_ADD), on);
    EnableWindow(GetDlgItem(dlg, IDC_PLUG_REMOVE), on);
    EnableWindow(GetDlgItem(dlg, IDC_PLUG_SCAN), on);
    EnableWindow(GetDlgItem(dlg, IDOK), on);
}

static void StartPluginScan(HWND dlg)
{
    std::vector<std::wstring> paths = ReadPluginPaths(dlg);
    if (paths.empty())
    {
        SetPluginFoundText(dlg, L"There are no folders to scan. Add one first.");
        SetFocus(GetDlgItem(dlg, IDC_PLUG_ADD));
        return;
    }

    EnablePluginButtons(dlg, false);
    SetPluginFoundText(dlg, FormatW(L"Scanning %zu folder%s...", paths.size(),
                                    paths.size() == 1 ? L"" : L"s"));

    std::thread([dlg, paths]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ScanResult* r = new ScanResult();
        r->folders = paths.size();
        for (const std::wstring& p : paths)
        {
            for (const PluginDesc& d : ScanPluginFolder(p))
            {
                // The same plugin can sit in two folders that both got added.
                bool known = false;
                for (const PluginEntry& e : r->found)
                    if (e.uid == d.uid && IEquals(e.path, d.path)) known = true;
                if (known) continue;

                PluginEntry e;
                e.format = d.format == PlugFormat::Vst3 ? 1 : 0;
                e.path = d.path;
                e.uid = d.uid;
                e.name = d.name;
                e.vendor = d.vendor;
                r->found.push_back(e);
            }
        }
        std::sort(r->found.begin(), r->found.end(),
            [](const PluginEntry& a, const PluginEntry& b)
            {
                int c = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                                       a.name.c_str(), -1, b.name.c_str(), -1);
                if (c != CSTR_EQUAL) return c == CSTR_LESS_THAN;
                return a.format < b.format;
            });
        CoUninitialize();
        PostMessageW(dlg, WM_SCAN_DONE, 0, (LPARAM)r);
    }).detach();
}

static INT_PTR CALLBACK SettingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        FillHotkeyFields(dlg, g_store.settings.showHide);
        SyncSettingsFields(dlg);
        SetDlgItemTextW(dlg, IDC_SET_REC_FOLDER, g_store.settings.recFolder.c_str());

        for (int i = 0; i < kUpdateCheckCount; ++i)
            SendDlgItemMessageW(dlg, IDC_SET_UPDATE, CB_ADDSTRING, 0,
                                (LPARAM)UpdateCheckName(i));
        {
            int when = g_store.settings.updateCheck;
            if (when < 0 || when >= kUpdateCheckCount) when = UC_NEVER;
            SendDlgItemMessageW(dlg, IDC_SET_UPDATE, CB_SETCURSEL, (WPARAM)when, 0);
            CheckDlgButton(dlg, IDC_SET_UPDATE_SILENT,
                           g_store.settings.updateSilent ? BST_CHECKED
                                                         : BST_UNCHECKED);
            // Nothing to be quiet about when it is never going to look.
            EnableWindow(GetDlgItem(dlg, IDC_SET_UPDATE_SILENT), when != UC_NEVER);
        }

        // An empty list is not helpful when the answer is the same two folders
        // on almost every machine, so those are offered to begin with.
        std::vector<std::wstring> paths = g_store.settings.pluginPaths;
        bool offered = false;
        if (paths.empty()) { paths = DefaultPluginFolders(); offered = !paths.empty(); }
        FillPluginPaths(dlg, paths);

        const size_t n = g_store.settings.plugins.size();
        SetPluginFoundText(dlg,
            offered ? L"These are where plugins are usually installed. Choose Scan "
                      L"to see what is in them."
            : n ? FormatW(L"%zu plugin%s from the last scan.", n, n == 1 ? L"" : L"s")
                : std::wstring(L"Nothing has been scanned yet."));
        return TRUE;
    }

    case WM_SCAN_DONE:
    {
        std::unique_ptr<ScanResult> r(reinterpret_cast<ScanResult*>(lp));
        g_store.settings.plugins = r->found;
        EnablePluginButtons(dlg, true);
        SetPluginFoundText(dlg, r->found.empty()
            ? FormatW(L"No CLAP or VST3 effects were found in %zu folder%s.",
                      r->folders, r->folders == 1 ? L"" : L"s")
            : FormatW(L"%zu plugin%s found. They are in the blocks library under "
                      L"External Effects.",
                      r->found.size(), r->found.size() == 1 ? L"" : L"s"));
        SetFocus(GetDlgItem(dlg, IDC_PLUG_LIST));
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_HK_ENABLED:
        case IDC_HK_CTRL:
        case IDC_HK_ALT:
        case IDC_HK_SHIFT:
        case IDC_HK_WIN:
            SyncSettingsFields(dlg);
            return TRUE;

        case IDC_HK_KEY:
            if (HIWORD(wp) == EN_CHANGE) SyncSettingsFields(dlg);
            return TRUE;

        // The checkbox follows the interval, so that "never, quietly"
        // cannot be set.
        case IDC_SET_UPDATE:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                const int when = (int)SendDlgItemMessageW(dlg, IDC_SET_UPDATE,
                                                          CB_GETCURSEL, 0, 0);
                EnableWindow(GetDlgItem(dlg, IDC_SET_UPDATE_SILENT),
                             when != UC_NEVER);
            }
            return TRUE;

        case IDC_PLUG_ADD:
        {
            std::wstring folder;
            if (PickFolder(dlg, folder, L"Choose a folder to search for plugins") &&
                !folder.empty())
            {
                HWND list = GetDlgItem(dlg, IDC_PLUG_LIST);
                if (SendMessageW(list, LB_FINDSTRINGEXACT, (WPARAM)-1,
                                 (LPARAM)folder.c_str()) == LB_ERR)
                {
                    int at = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)folder.c_str());
                    SendMessageW(list, LB_SETCURSEL, (WPARAM)at, 0);
                }
                SetPluginFoundText(dlg, L"Choose Scan to look in it.");
            }
            SetFocus(GetDlgItem(dlg, IDC_PLUG_LIST));
            return TRUE;
        }

        case IDC_PLUG_REMOVE:
        {
            HWND list = GetDlgItem(dlg, IDC_PLUG_LIST);
            int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) return TRUE;
            SendMessageW(list, LB_DELETESTRING, (WPARAM)sel, 0);
            const int left = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
            if (left > 0) SendMessageW(list, LB_SETCURSEL, (WPARAM)(sel < left ? sel : left - 1), 0);
            SetPluginFoundText(dlg, L"Choose Scan to bring the plugin list up to date.");
            SetFocus(list);
            return TRUE;
        }

        case IDC_PLUG_SCAN:
            StartPluginScan(dlg);
            return TRUE;

        case IDC_SET_REC_BROWSE:
        {
            wchar_t cur[MAX_PATH] = { 0 };
            GetDlgItemTextW(dlg, IDC_SET_REC_FOLDER, cur, MAX_PATH);
            std::wstring folder = cur;
            if (PickFolder(dlg, folder,
                           L"Choose where new Recorder blocks save to"))
                SetDlgItemTextW(dlg, IDC_SET_REC_FOLDER, folder.c_str());
            SetFocus(GetDlgItem(dlg, IDC_SET_REC_FOLDER));
            return TRUE;
        }

        case IDOK:
        {
            Hotkey want = ReadHotkeyFields(dlg);
            if (want.enabled && !CheckTypedKey(dlg)) return TRUE;
            if (want.enabled && want.mods == 0)
            {
                ErrorBox(dlg, L"Choose at least one of Ctrl, Alt, Shift or Windows "
                              L"for the global hotkey.");
                SetFocus(GetDlgItem(dlg, IDC_HK_CTRL));
                return TRUE;
            }

            wchar_t recFolder[MAX_PATH] = { 0 };
            GetDlgItemTextW(dlg, IDC_SET_REC_FOLDER, recFolder, MAX_PATH);
            std::wstring rec = Trim(recFolder);

            // Said now rather than when a recording fails to start, which is
            // both later and further from the thing that caused it. It is a
            // warning and not a refusal: a folder on a drive that is not
            // plugged in at the moment is a legitimate thing to want.
            if (!rec.empty() && !DirectoryExists(rec))
            {
                if (MessageBoxW(dlg,
                        FormatW(L"%s does not exist at the moment.\r\n\r\n"
                                L"New Recorder blocks will still be set to it. "
                                L"Keep it anyway?", rec.c_str()).c_str(),
                        L"FastRoute", MB_YESNO | MB_ICONQUESTION) != IDYES)
                {
                    SetFocus(GetDlgItem(dlg, IDC_SET_REC_FOLDER));
                    return TRUE;
                }
            }

            AppSettings previous = g_store.settings;
            {
                const int when = (int)SendDlgItemMessageW(dlg, IDC_SET_UPDATE,
                                                          CB_GETCURSEL, 0, 0);
                if (when >= 0) g_store.settings.updateCheck = when;
                g_store.settings.updateSilent =
                    IsDlgButtonChecked(dlg, IDC_SET_UPDATE_SILENT) == BST_CHECKED;
            }
            g_store.settings.showHide = want;
            g_store.settings.pluginPaths = ReadPluginPaths(dlg);
            g_store.settings.recFolder = rec;

            // Try it before keeping it: a hotkey another program already owns
            // would otherwise be saved and silently do nothing.
            std::wstring err;
            if (!ApplyAppHotkey(g_mainDlg, &err))
            {
                g_store.settings = previous;
                ApplyAppHotkey(g_mainDlg, nullptr);
                ErrorBox(dlg, err);
                SetFocus(GetDlgItem(dlg, IDC_HK_KEY));
                return TRUE;
            }

            std::wstring saveErr;
            if (!g_store.Save(&saveErr)) ErrorBox(dlg, saveErr);
            EndDialog(dlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowSettingsDialog(HWND parent)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), parent, SettingsDlgProc, 0);
}

// ---------------------------------------------------------------------------
// One session's or one block's hotkey
//
// The same fields as Settings, because it is the same question. What differs
// is the sentence above them, which is the only thing telling you which of
// possibly several hotkeys you are looking at.
// ---------------------------------------------------------------------------

struct HotkeyCtx
{
    std::wstring what;
    Hotkey* hk;
    int forWhat = HF_BLOCK;
};

// The running commentary under the fields. The three kinds do three different
// things, and an action key is also the only one whose registration comes and
// goes, so saying "switches it on and off" for all of them would be wrong
// about two of them.
static void SyncItemHotkeyFields(HWND dlg, int forWhat)
{
    if (forWhat == HF_SESSION)
    {
        SyncHotkeyFields(dlg,
            L"will start the session from whatever you are using at the time, "
                L"and stop it again when it is running, with a high tone for "
                L"started and a low one for stopped. It is taken from every "
                L"other program for as long as FastRoute is running.",
            L"This has no hotkey, so the session can only be started from "
                L"FastRoute's own window.",
            L"will start and stop the session while a FastRoute window is in "
                L"front, with a high tone for started and a low one for "
                L"stopped. Every other program keeps that key.");
        return;
    }

    if (forWhat == HF_ACTION)
    {
        SyncHotkeyFields(dlg,
            L"will do it while the session is running, with a high tone if it "
                L"was done and a low one if there was nothing to do it to. "
                L"Because there is nothing to act on while the session is "
                L"stopped, the key is only taken from other programs while "
                L"that session runs - so the same key can do the same thing in "
                L"every session you have, as long as one of them is running at "
                L"a time.",
            L"This has no hotkey, so it can only be done from FastRoute's own "
                L"window.",
            L"will do it while a FastRoute window is in front and the session "
                L"is running, with a high tone if it was done and a low one if "
                L"there was nothing to do it to. Every other program keeps that "
                L"key, and so does every session of yours that is not running.");
        return;
    }

    SyncHotkeyFields(dlg,
        L"will switch it on and off from whatever you are using at the time, "
            L"with a high tone for on and a low one for off. It is taken from "
            L"every other program for as long as FastRoute is running, whether "
            L"or not the session is.",
        L"This has no hotkey, so it can only be changed from FastRoute's own "
            L"window.",
        L"will switch it on and off while a FastRoute window is in front, with "
            L"a high tone for on and a low one for off. Every other program "
            L"keeps that key.");
}

static INT_PTR CALLBACK HotkeyDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    HotkeyCtx* ctx = reinterpret_cast<HotkeyCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ctx = reinterpret_cast<HotkeyCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetDlgItemTextW(dlg, IDC_HK_WHAT,
                        FormatW(L"Hotkey for %s.", ctx->what.c_str()).c_str());
        FillHotkeyFields(dlg, *ctx->hk);
        SyncItemHotkeyFields(dlg, ctx->forWhat);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_HK_ENABLED:
        case IDC_HK_CTRL:
        case IDC_HK_ALT:
        case IDC_HK_SHIFT:
        case IDC_HK_WIN:
        case IDC_HK_SCOPE_GLOBAL:
        case IDC_HK_SCOPE_WINDOW:
        case IDC_HK_KEY:
            if (ctx && (LOWORD(wp) != IDC_HK_KEY || HIWORD(wp) == EN_CHANGE))
                SyncItemHotkeyFields(dlg, ctx->forWhat);
            return TRUE;

        case IDOK:
        {
            Hotkey want = ReadHotkeyFields(dlg);
            if (want.enabled && !CheckTypedKey(dlg)) return TRUE;
            if (want.enabled && want.mods == 0)
            {
                ErrorBox(dlg, L"Choose at least one of Ctrl, Alt, Shift or Windows "
                              L"for the hotkey.");
                SetFocus(GetDlgItem(dlg, IDC_HK_CTRL));
                return TRUE;
            }
            *ctx->hk = want;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool EditHotkey(HWND parent, const std::wstring& what, Hotkey& hk, int forWhat)
{
    HotkeyCtx ctx{ what, &hk, forWhat };
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_HOTKEY), parent,
                           HotkeyDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}


static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        std::wstring text =
            L"FastRoute\r\n\r\n"
            L"Per-session audio routing: input blocks feed effects blocks, "
            L"which feed output blocks.\r\n\r\n"
            L"Sessions are stored in:\r\n" + g_store.Path();
        SetDlgItemTextW(dlg, IDC_ABOUT_TEXT, text.c_str());
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
        {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Ctrl+Shift+O: the recording folder
//
// A recorder's folder is a setting several dialogs deep, and the whole point of
// recording is the files at the end of it. This opens the folder in Explorer
// from either window, without going looking for the block first.
// ---------------------------------------------------------------------------

void OpenRecordFolder(HWND owner, int sessionId, int preferBlock)
{
    Session* sn = g_store.ById(sessionId);
    if (!sn) return;

    std::vector<int> recorders;
    for (size_t i = 0; i < sn->blocks.size(); ++i)
        if (sn->blocks[i].typeId == BT_OUT_RECORDER) recorders.push_back((int)i);

    if (recorders.empty())
    {
        ErrorBox(owner, FormatW(
            L"%s has no Recorder block, so it is not recording anywhere. Add one "
            L"from the blocks library to give it somewhere to write.",
            sn->name.c_str()));
        return;
    }

    // The selected block wins if it is itself a recorder: with several in one
    // session, the one under the cursor is the one being asked about.
    int which = recorders[0];
    for (int i : recorders) if (i == preferBlock) which = i;

    const Block& b = sn->blocks[(size_t)which];
    std::wstring dir = ResolveRecordFolder(b.folder);
    if (dir.empty())
    {
        ErrorBox(owner, L"The recording folder could not be worked out.");
        return;
    }

    if (!DirectoryExists(dir))
    {
        // Not created until something is actually written, so this is the
        // ordinary state of a recorder that has never run rather than a fault.
        // It is not created here: asking where a folder is should not make one.
        ErrorBox(owner, FormatW(
            L"%s does not exist yet.\r\n\r\nIt is made when the recorder first "
            L"writes a file, so this is what a session that has not recorded "
            L"anything looks like.", dir.c_str()));
        return;
    }

    HINSTANCE r = ShellExecuteW(owner, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute reports failure as a value of 32 or less, in a type that has
    // nothing to do with instance handles.
    if ((INT_PTR)r <= 32)
        ErrorBox(owner, FormatW(L"%s could not be opened.", dir.c_str()));
}

void ShowAboutDialog(HWND parent)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ABOUT), parent, AboutDlgProc, 0);
}

// ---------------------------------------------------------------------------
// A scheduled event
//
// The two ways of saying when are radio buttons rather than a mode combo: the
// second one ("in twenty minutes") is not a variant of a time of day, it is a
// different question, and reading them one after the other says so.
// ---------------------------------------------------------------------------

static bool ReadClock(HWND dlg, int hourId, int minId, int* hour, int* minute)
{
    BOOL gotH = FALSE, gotM = FALSE;
    int h = (int)GetDlgItemInt(dlg, hourId, &gotH, FALSE);
    int m = (int)GetDlgItemInt(dlg, minId, &gotM, FALSE);
    if (!gotH || !gotM || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *hour = h;
    *minute = m;
    return true;
}

static void SyncScheduleFields(HWND dlg)
{
    const bool atTime = IsDlgButtonChecked(dlg, IDC_SCH_MODE_TIME) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_SCH_HOUR), atTime);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_MIN), atTime);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_TIMELABEL), atTime);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_INMIN), !atTime);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_INLABEL), !atTime);

    // Starting a set number of minutes from now happens once, by definition.
    EnableWindow(GetDlgItem(dlg, IDC_SCH_REPEAT), atTime);
    int repeat = atTime
        ? (int)SendDlgItemMessageW(dlg, IDC_SCH_REPEAT, CB_GETCURSEL, 0, 0)
        : SR_ONCE;
    const bool days = (repeat == SR_WEEKDAYS);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_DOWLABEL), days);
    for (int d = 0; d < 7; ++d) EnableWindow(GetDlgItem(dlg, IDC_SCH_DOW + d), days);

    int stop = (int)SendDlgItemMessageW(dlg, IDC_SCH_STOP, CB_GETCURSEL, 0, 0);
    const bool stopTime = (stop == SS_AT_TIME), stopAfter = (stop == SS_AFTER);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_STOPHOUR), stopTime);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_STOPMIN), stopTime);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_STOPLABEL), stopTime);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_AFTER), stopAfter);
    EnableWindow(GetDlgItem(dlg, IDC_SCH_AFTERLABEL), stopAfter);
}

struct SchedCtx
{
    ScheduleEvent* ev;
    std::vector<int> sessionIds;
};

static INT_PTR CALLBACK ScheduleDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SchedCtx* ctx = reinterpret_cast<SchedCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ctx = reinterpret_cast<SchedCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        ScheduleEvent* e = ctx->ev;

        HWND sc = GetDlgItem(dlg, IDC_SCH_SESSION);
        int sel = 0;
        for (size_t i = 0; i < g_store.sessions.size(); ++i)
        {
            SendMessageW(sc, CB_ADDSTRING, 0, (LPARAM)g_store.sessions[i].name.c_str());
            ctx->sessionIds.push_back(g_store.sessions[i].id);
            if (g_store.sessions[i].id == e->sessionId) sel = (int)i;
        }
        SendMessageW(sc, CB_SETCURSEL, sel, 0);

        CheckRadioButton(dlg, IDC_SCH_MODE_TIME, IDC_SCH_MODE_IN, IDC_SCH_MODE_TIME);
        SetDlgItemInt(dlg, IDC_SCH_HOUR, (UINT)e->hour, FALSE);
        SetDlgItemInt(dlg, IDC_SCH_MIN, (UINT)e->minute, FALSE);
        SetDlgItemInt(dlg, IDC_SCH_INMIN, 60, FALSE);

        HWND rc = GetDlgItem(dlg, IDC_SCH_REPEAT);
        SendMessageW(rc, CB_ADDSTRING, 0, (LPARAM)L"Once");
        SendMessageW(rc, CB_ADDSTRING, 0, (LPARAM)L"Every day");
        SendMessageW(rc, CB_ADDSTRING, 0, (LPARAM)L"On chosen days of the week");
        SendMessageW(rc, CB_SETCURSEL,
                     (e->repeat >= 0 && e->repeat <= 2) ? e->repeat : 0, 0);
        for (int d = 0; d < 7; ++d)
            CheckDlgButton(dlg, IDC_SCH_DOW + d,
                           (e->weekdays & (1 << d)) ? BST_CHECKED : BST_UNCHECKED);

        HWND tc = GetDlgItem(dlg, IDC_SCH_STOP);
        SendMessageW(tc, CB_ADDSTRING, 0, (LPARAM)L"Leave it running");
        SendMessageW(tc, CB_ADDSTRING, 0, (LPARAM)L"At a time of day");
        SendMessageW(tc, CB_ADDSTRING, 0, (LPARAM)L"After a number of minutes");
        SendMessageW(tc, CB_SETCURSEL,
                     (e->stopMode >= 0 && e->stopMode <= 2) ? e->stopMode : 0, 0);
        SetDlgItemInt(dlg, IDC_SCH_STOPHOUR, (UINT)e->stopHour, FALSE);
        SetDlgItemInt(dlg, IDC_SCH_STOPMIN, (UINT)e->stopMinute, FALSE);
        SetDlgItemInt(dlg, IDC_SCH_AFTER, (UINT)e->afterMinutes, FALSE);

        CheckDlgButton(dlg, IDC_SCH_SINGLE, e->singleUse ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_SCH_ENABLED, e->enabled ? BST_CHECKED : BST_UNCHECKED);
        SyncScheduleFields(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_SCH_MODE_TIME:
        case IDC_SCH_MODE_IN:
            SyncScheduleFields(dlg);
            return TRUE;

        case IDC_SCH_REPEAT:
        case IDC_SCH_STOP:
            if (HIWORD(wp) == CBN_SELCHANGE) SyncScheduleFields(dlg);
            return TRUE;

        case IDOK:
        {
            ScheduleEvent want = *ctx->ev;

            int sel = (int)SendDlgItemMessageW(dlg, IDC_SCH_SESSION, CB_GETCURSEL, 0, 0);
            if (sel < 0 || (size_t)sel >= ctx->sessionIds.size())
            {
                ErrorBox(dlg, L"Choose the session this event should run. If the "
                              L"list is empty, make a session first.");
                SetFocus(GetDlgItem(dlg, IDC_SCH_SESSION));
                return TRUE;
            }
            want.sessionId = ctx->sessionIds[(size_t)sel];

            const bool atTime = IsDlgButtonChecked(dlg, IDC_SCH_MODE_TIME) == BST_CHECKED;
            if (atTime)
            {
                if (!ReadClock(dlg, IDC_SCH_HOUR, IDC_SCH_MIN, &want.hour, &want.minute))
                {
                    ErrorBox(dlg, L"Type the hour as 0 to 23 and the minute as 0 to 59.");
                    SetFocus(GetDlgItem(dlg, IDC_SCH_HOUR));
                    return TRUE;
                }
                want.repeat = (int)SendDlgItemMessageW(dlg, IDC_SCH_REPEAT,
                                                       CB_GETCURSEL, 0, 0);
                if (want.repeat < 0) want.repeat = SR_ONCE;

                want.weekdays = 0;
                for (int d = 0; d < 7; ++d)
                    if (IsDlgButtonChecked(dlg, IDC_SCH_DOW + d) == BST_CHECKED)
                        want.weekdays |= (1 << d);
                if (want.repeat == SR_WEEKDAYS && want.weekdays == 0)
                {
                    ErrorBox(dlg, L"Tick at least one day of the week, or choose a "
                                  L"different kind of repeat.");
                    SetFocus(GetDlgItem(dlg, IDC_SCH_DOW));
                    return TRUE;
                }

                // A one-off at a time of day means the next time that time
                // comes round, which is today if it has not been yet.
                if (want.repeat == SR_ONCE)
                {
                    SYSTEMTIME now{};
                    GetLocalTime(&now);
                    if (now.wHour > want.hour ||
                        (now.wHour == want.hour && now.wMinute >= want.minute))
                    {
                        // Tomorrow, worked out by the calendar rather than by
                        // adding one to the day and hoping it is a real date.
                        FILETIME ft{};
                        SystemTimeToFileTime(&now, &ft);
                        ULARGE_INTEGER u;
                        u.LowPart = ft.dwLowDateTime;
                        u.HighPart = ft.dwHighDateTime;
                        u.QuadPart += 24ull * 60ull * 60ull * 10000000ull;
                        ft.dwLowDateTime = u.LowPart;
                        ft.dwHighDateTime = u.HighPart;
                        FileTimeToSystemTime(&ft, &now);
                    }
                    want.year = now.wYear;
                    want.month = now.wMonth;
                    want.day = now.wDay;
                }
            }
            else
            {
                BOOL got = FALSE;
                int mins = (int)GetDlgItemInt(dlg, IDC_SCH_INMIN, &got, FALSE);
                if (!got || mins < 1 || mins > 60 * 24 * 30)
                {
                    ErrorBox(dlg, L"Type how many minutes from now it should start, "
                                  L"from 1 upwards.");
                    SetFocus(GetDlgItem(dlg, IDC_SCH_INMIN));
                    return TRUE;
                }
                // Turned into a real date and time now, so it means the same
                // thing tomorrow as it does while the dialog is open.
                FILETIME ft{};
                SYSTEMTIME now{};
                GetLocalTime(&now);
                SystemTimeToFileTime(&now, &ft);
                ULARGE_INTEGER u;
                u.LowPart = ft.dwLowDateTime;
                u.HighPart = ft.dwHighDateTime;
                u.QuadPart += (unsigned long long)mins * 60ull * 10000000ull;
                ft.dwLowDateTime = u.LowPart;
                ft.dwHighDateTime = u.HighPart;
                SYSTEMTIME when{};
                FileTimeToSystemTime(&ft, &when);

                want.repeat = SR_ONCE;
                want.year = when.wYear;
                want.month = when.wMonth;
                want.day = when.wDay;
                want.hour = when.wHour;
                want.minute = when.wMinute;
            }

            want.stopMode = (int)SendDlgItemMessageW(dlg, IDC_SCH_STOP, CB_GETCURSEL, 0, 0);
            if (want.stopMode < 0) want.stopMode = SS_NONE;
            if (want.stopMode == SS_AT_TIME &&
                !ReadClock(dlg, IDC_SCH_STOPHOUR, IDC_SCH_STOPMIN,
                           &want.stopHour, &want.stopMinute))
            {
                ErrorBox(dlg, L"Type the stopping hour as 0 to 23 and the minute "
                              L"as 0 to 59.");
                SetFocus(GetDlgItem(dlg, IDC_SCH_STOPHOUR));
                return TRUE;
            }
            if (want.stopMode == SS_AFTER)
            {
                BOOL got = FALSE;
                int mins = (int)GetDlgItemInt(dlg, IDC_SCH_AFTER, &got, FALSE);
                if (!got || mins < 1 || mins > 60 * 24 * 30)
                {
                    ErrorBox(dlg, L"Type how many minutes it should run for, from "
                                  L"1 upwards.");
                    SetFocus(GetDlgItem(dlg, IDC_SCH_AFTER));
                    return TRUE;
                }
                want.afterMinutes = mins;
            }

            want.singleUse = IsDlgButtonChecked(dlg, IDC_SCH_SINGLE) == BST_CHECKED;
            want.enabled = IsDlgButtonChecked(dlg, IDC_SCH_ENABLED) == BST_CHECKED;

            // An event just written has not missed anything, whatever the
            // clock says about today's occurrence.
            ScheduleMarkCurrent(want);

            *ctx->ev = want;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool EditScheduleEvent(HWND parent, ScheduleEvent& e)
{
    SchedCtx ctx{ &e, {} };
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SCHEDULE), parent,
                           ScheduleDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

// ---------------------------------------------------------------------------
// Context menus
// ---------------------------------------------------------------------------

POINT CtxMenuPoint(HWND ctl, LPARAM lp)
{
    POINT pt;
    pt.x = (int)(short)LOWORD(lp);
    pt.y = (int)(short)HIWORD(lp);
    if (pt.x != -1 || pt.y != -1) return pt;   // real mouse position

    // Keyboard invocation (Shift+F10 or the Applications key): anchor the menu
    // on the focused item so it appears where the user is reading.
    RECT r{};
    bool got = false;
    wchar_t cls[64] = { 0 };
    GetClassNameW(ctl, cls, 64);

    if (_wcsicmp(cls, WC_LISTVIEW) == 0)
    {
        int i = (int)SendMessageW(ctl, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
        if (i >= 0)
        {
            r.left = LVIR_BOUNDS;
            got = SendMessageW(ctl, LVM_GETITEMRECT, (WPARAM)i, (LPARAM)&r) != 0;
        }
    }
    else if (_wcsicmp(cls, WC_TREEVIEW) == 0)
    {
        HTREEITEM h = (HTREEITEM)SendMessageW(ctl, TVM_GETNEXTITEM, TVGN_CARET, 0);
        if (h)
        {
            *reinterpret_cast<HTREEITEM*>(&r) = h;
            got = SendMessageW(ctl, TVM_GETITEMRECT, TRUE, (LPARAM)&r) != 0;
        }
    }
    if (!got) GetClientRect(ctl, &r);

    pt.x = r.left + 8;
    pt.y = r.bottom;
    MapWindowPoints(ctl, nullptr, &pt, 1);
    return pt;
}

void ShowContextMenu(HWND owner, int resId, POINT pt, UINT defaultCmd,
                     const std::map<UINT, bool>& enabled,
                     const std::map<UINT, bool>* checked)
{
    HMENU bar = LoadMenuW(g_inst, MAKEINTRESOURCEW(resId));
    if (!bar) return;
    HMENU popup = GetSubMenu(bar, 0);
    if (popup)
    {
        for (const auto& kv : enabled)
            EnableMenuItem(popup, kv.first,
                           MF_BYCOMMAND | (kv.second ? MF_ENABLED : MF_GRAYED));
        // Check marks are how a screen reader learns that an item is a toggle
        // and which way it currently sits.
        if (checked)
            for (const auto& kv : *checked)
                CheckMenuItem(popup, kv.first,
                              MF_BYCOMMAND | (kv.second ? MF_CHECKED : MF_UNCHECKED));
        if (defaultCmd) SetMenuDefaultItem(popup, defaultCmd, FALSE);

        TrackPopupMenuEx(popup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                         pt.x, pt.y, owner, nullptr);
    }
    DestroyMenu(bar);
}
