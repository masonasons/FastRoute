#pragma once
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <string>
#include <map>

#include "resource.h"
#include "util.h"
#include "model.h"
#include "audio/audio.h"

extern HINSTANCE g_inst;
extern HWND      g_mainDlg;

// The notification area icon talks back through this, and the global hotkey
// arrives as WM_HOTKEY carrying this id.
#define WM_APP_TRAY (WM_APP + 1)
static const int kHotkeyId = 1;

// ---- running engines ------------------------------------------------------
bool StartSession(HWND owner, int sessionId);
void StopSession(int sessionId);
void StopAllSessions();
bool SessionRunning(int sessionId);
bool SessionStatus(int sessionId, Engine::Status* st);
// Pushes edits into a running session without a restart. No-op when stopped.
bool ApplyLiveEdit(HWND owner, int sessionId);
// The same, with nothing to report to: a global hotkey can fire while the
// window is hidden and the user is somewhere else entirely, and a modal box
// in front of that would be worse than a line in the error log.
void ApplyLiveEditQuiet(int sessionId);

// Tell a block in a running session to do something. Nothing is saved: an
// action is a moment, not a setting. False if the session is not running or
// that block has nothing to tell, which is what a hotkey pressed at the wrong
// time looks like.
bool PerformBlockAction(int sessionId, int blockIndex, int action);
bool StartSessionSilently(int sessionId, std::wstring* err);
// Starts every session marked "Run at Startup". A failure goes to the error
// log rather than a message box, so nothing blocks the way in.
void RunStartupSessions();

// ---- scheduler ------------------------------------------------------------
// Checked a few times a minute from the main window's timer.
void SchedulerTick(HWND owner);
void SchedulerForget(int eventId);
void ScheduleMarkCurrent(ScheduleEvent& e);
long long ScheduleNowMinutes();
long long ScheduleDueAt(const ScheduleEvent& e, const SYSTEMTIME& now);
std::wstring ScheduleNextText(const ScheduleEvent& e);

// ---- notification area and the global hotkey ------------------------------
// The message a second FastRoute broadcasts to ask the one already running to
// show itself. Registered rather than a WM_APP number so that it cannot mean
// something else in another program's window.
UINT ShowRequestMessage();

bool IsHiddenToTray();
void HideToTray(HWND owner);
void ShowFromTray(HWND owner);
void ToggleTray(HWND owner);
void TrayUpdateTip(HWND owner);
void TrayShutdown(HWND owner);
// Registers the hotkey in g_store.settings, replacing any previous one.
bool ApplyAppHotkey(HWND owner, std::wstring* err);
void UnregisterAppHotkey(HWND owner);
// Registers every session and block hotkey in the document, replacing the
// previous set. Called whenever anything is saved, so a binding never points
// at a block that has since moved or gone.
void RefreshItemHotkeys(HWND owner);
void ClearItemHotkeys(HWND owner);

// A key that only works while FastRoute is in front. Offered from the message
// loop before the dialog gets it, and true if it was one and has been acted on.
bool HandleWindowHotkey(const MSG& msg);
// True when the message was the tray's or the hotkey's and has been dealt with.
bool TrayHandleMessage(HWND owner, UINT msg, WPARAM wp, LPARAM lp);

// ---- windows --------------------------------------------------------------
INT_PTR CALLBACK MainDlgProc(HWND, UINT, WPARAM, LPARAM);
void OpenSessionWindow(int sessionId);
void CloseAllSessionWindows();
// Hides or restores every open session window alongside the main one.
void SetSessionWindowsVisible(bool show);
HWND SessionWindowFor(int sessionId);
// The session a window belongs to, or 0 if it is not one of ours.
int  SessionWindowIdFor(HWND hwnd);
void NotifySessionStateChanged(int sessionId);
void RefreshMainList();
void RefreshScheduleList();
void RefreshSessionWindow(int sessionId);
HACCEL AccelForWindow(HWND hwnd);
HACCEL MainAccel();

// ---- property dialogs -----------------------------------------------------
bool PromptForName(HWND parent, const wchar_t* caption, const wchar_t* label,
                   std::wstring& name);
bool EditBlockParams(HWND parent, Block& b);

// Apply, on a block parameter dialog: everything closing it would do except
// the closing. `sessionDlg` is the window the dialog was opened over. False
// means the change did not take, and something has already been said about it.
bool ApplyBlockEdit(HWND sessionDlg);

// What a block parameter dialog does at the end of its OK branch. `cmd` is the
// button that got it there: IDOK closes, IDC_APPLY commits and stays. Every
// such dialog routes both through the one branch, so there is never a second
// copy of the reading to drift out of step with the first.
void FinishBlockEdit(HWND dlg, int cmd);
// An external plugin's parameters, and its own editor. Its own file, because
// it is the only dialog that has to load somebody else's code to draw itself.
bool EditPluginBlock(HWND parent, Block& b);
// Which of the three things a hotkey is for. The fields are the same for all
// of them; what differs is the sentence underneath, and an action key really
// does behave differently - it is only registered while its session runs.
enum HotkeyFor
{
    HF_SESSION = 0,     // starts and stops the session
    HF_BLOCK   = 1,     // switches one block on and off
    HF_ACTION  = 2,     // does something to a running block
};
// `what` completes "Hotkey for ..." and is read out with the dialog.
bool EditHotkey(HWND parent, const std::wstring& what, Hotkey& hk, int forWhat);

// Ctrl+Shift+O: show the folder a session's Recorder block writes into.
// `preferBlock` is the block the user has selected, or -1 when there is no
// block list to have selected one in - the main window's case.
void OpenRecordFolder(HWND owner, int sessionId, int preferBlock);
// The hotkeys for one block's actions. False when the block has none, or
// when the dialog was cancelled.
bool EditBlockActions(HWND parent, Block& b);
bool EditScheduleEvent(HWND parent, ScheduleEvent& e);

// Export and import one session as a file of its own. Both ask for the file
// and report what happened; export takes the session to write, and import
// adds whatever it read to the store and returns its id, or 0.
void ExportSession(HWND owner, const Session& s);
int  ImportSession(HWND owner);
void ShowAboutDialog(HWND parent);
void ShowErrorLogDialog(HWND parent);
void ShowSettingsDialog(HWND parent);

// Asks before exiting while sessions are running, since exiting stops them.
// True to go ahead. Answers true without asking when nothing is running, or
// when the settings say not to.
bool ConfirmExitWhileRunning(HWND owner);

// ---------------------------------------------------------------------------
// Undo, shared by the two windows that have an Edit menu
// ---------------------------------------------------------------------------

// Sets the Undo and Redo items to say what they will actually do - "Undo
// removing Microphone" - and greys them when there is nothing to do. The menu
// saying so before the key is pressed is what keeps a stack of removals from
// being a surprise, since the last removal is not always the last edit.
void NameUndoItems(HMENU menu, const UndoStack& stack, int undoId, int redoId);

// Ctrl+Z belongs to a focused edit control when the focus is not on the list,
// the same way Ctrl+C does. True when it has been dealt with there and there
// is nothing for the undo command to do.
bool PassUndoKeyOn(HWND dlg, WPARAM wp, int listId);

// Everything remembered about removals from one session's block list, dropped.
// The session has gone, so putting a block back into it is not something that
// can be done.
void ForgetBlockUndo(int sessionId);

// ---- small helpers --------------------------------------------------------
struct ListKeys
{
    UINT onEnter = 0;
    UINT onDelete = 0;
};
void SubclassListKeys(HWND ctl, const ListKeys& keys);
void ReverseSliderKeys(HWND dlg);
void SetSliderValue(HWND slider, const std::wstring& text);
void SetSliderReadout(HWND dlg, int sliderId, int textId,
                      const std::wstring& text);
void SetupVolumeSlider(HWND dlg, int value);
POINT CtxMenuPoint(HWND ctl, LPARAM lp);
void  ShowContextMenu(HWND owner, int resId, POINT pt, UINT defaultCmd,
                      const std::map<UINT, bool>& enabled,
                      const std::map<UINT, bool>* checked = nullptr);
int  ReadVolumeSlider(HWND dlg);
