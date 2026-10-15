#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Block taxonomy
// ---------------------------------------------------------------------------
enum class BlockKind { Input, Effect, Output };

struct BlockType
{
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* category;   // top-level node in the blocks library tree
    BlockKind      kind;
};

const std::vector<BlockType>& AllBlockTypes();

// ---------------------------------------------------------------------------
// The blocks library, as the tree shows it
//
// Built fresh rather than being a constant, because half of it is not: the
// external effects are whatever the last plugin scan found. A plugin item
// carries the plugin it stands for, so adding one needs no second lookup.
// ---------------------------------------------------------------------------
struct LibraryItem
{
    std::wstring typeId;
    std::wstring name;
    std::wstring category;
    BlockKind    kind = BlockKind::Effect;
    int          pluginIndex = -1;    // into AppSettings::plugins, or -1
};

// In the order the tree shows them: the four category names, then the items.
const std::vector<const wchar_t*>& LibraryCategories();
std::vector<LibraryItem> BuildLibrary();

// Fills in a new block from a library item, including which plugin it is.
struct Block;
void InitBlockFrom(Block& b, const LibraryItem& item);
const BlockType* FindBlockType(const std::wstring& id);

// Block type ids
#define BT_INPUT_APP    L"input.application"
#define BT_INPUT_DEVICE L"input.device"
#define BT_INPUT_SYSTEM L"input.system"
#define BT_INPUT_STREAM L"input.stream"
#define BT_INPUT_NET    L"input.net"
#define BT_INPUT_ASIO   L"input.asio"
#define BT_EFFECT_GAIN  L"effect.gain"
#define BT_FX_SPEEX     L"effect.denoise.speex"
#define BT_FX_RNNOISE   L"effect.denoise.rnnoise"
#define BT_FX_TONE      L"effect.tone"
#define BT_FX_EQ10      L"effect.eq10"
#define BT_FX_PARA      L"effect.para"
#define BT_FX_CHANNELS  L"effect.channels"
#define BT_FX_COMP      L"effect.compressor"
#define BT_FX_LIMIT     L"effect.limiter"
#define BT_FX_GATE      L"effect.gate"
#define BT_FX_NORM      L"effect.normalise"
#define BT_FX_PITCH     L"effect.pitch"
#define BT_FX_CLIPMON   L"effect.clipmonitor"
#define BT_FX_BUFFER    L"effect.buffer"
// One block type for every external plugin. Which plugin a given block is
// stands in its own fields, not in the type id: a type id is a compile time
// constant here and a plugin is whatever happens to be installed.
#define BT_FX_PLUGIN    L"effect.plugin"
#define BT_OUT_RECORDER L"output.recorder"
#define BT_OUT_NET      L"output.net"
#define BT_OUTPUT_DEVICE L"output.device"
#define BT_OUT_ASIO     L"output.asio"

// Which input blocks a Recorder has been told to give a file of their own, and
// what each of those files is called.
//
// Entry i is the name for block i's own track, or empty when block i does not
// get one - which is every block that is not an input, and every input with no
// recorder below it asking for tracks. Below, because a recorder only ever
// hears what is above it in the list.
//
// Whether that recorder is switched on is deliberately not part of it.
// Switching a recorder off pauses it: its own file stays open and resumes
// where it left off, so the tracks beside it have to do the same or they would
// be a different recording from the one they sit next to.
//
// The name is the block's, with a number added when two blocks of the same
// type are both unnamed: two files called "Application" would otherwise be
// told apart only by the order they happened to open in.
//
// Separate from the engine because it is a decision about a list of blocks and
// nothing else, so it can be checked without a graph or a sound card.
std::vector<std::wstring> TrackNamesFor(const std::vector<Block>& blocks);

// Which programs an application block captures, in the order they are listed.
// The list where it has one and the single name otherwise, so that one answer
// serves a session saved before a block could name several. Empty for a block
// pointed at the foreground, which works its own target out.
std::vector<std::wstring> AppTargets(const Block& b);

// What an application input block is pointed at. A foreground block needs no
// process name: it works out its own target, either once or continuously.
enum AppSource
{
    AS_NAMED = 0,        // the process named in the block
    AS_FOREGROUND = 1,   // whatever was in front when the block last started
    AS_FOLLOW = 2,       // whatever is in front, and it keeps up as that changes
};

// What a Recorder block writes. The mix is the bus where the block sits, which
// is what a recorder has always written. Tracks are a file per input block
// feeding it, tapped at that block's own level and before anything further
// down the list - which is what makes them separable, and also what makes them
// not the same audio as the mix.
enum
{
    RM_MIXED  = 0,
    RM_TRACKS = 1,
    RM_BOTH   = 2,
};

// Recorder silence gate: hold time meaning "never shut again".
static const int kHoldAlways = -1;
static const int kHoldMaxSec = 30;

// Compressor and limiter ranges. They are here rather than in the dialog
// because the effect clamps to them as well: a document edited by hand, or
// written by a later version, must not be able to hand the audio thread a
// ratio of zero or a lookahead of a second and a half.
static const int kCompThreshMin   = -60, kCompThreshMax  = 0;    // dBFS
static const int kCompRatioMin    = 1,   kCompRatioMax   = 20;   // n:1
static const int kCompAttackMin   = 1,   kCompAttackMax  = 200;  // ms
static const int kCompReleaseMin  = 10,  kCompReleaseMax = 2000; // ms
static const int kCompMakeupMax   = 24;                          // dB
static const int kLimCeilingMin   = -12, kLimCeilingMax  = 0;    // dBFS
static const int kLimReleaseMin   = 10,  kLimReleaseMax  = 1000; // ms
static const int kLimLookaheadMin = 0,   kLimLookaheadMax = 20;  // ms
static const int kGateThreshMin  = -80, kGateThreshMax  = 0;    // dBFS
static const int kGateAttackMin  = 1,   kGateAttackMax  = 100;  // ms
static const int kGateHoldMin    = 0,   kGateHoldMax    = 2000; // ms
static const int kGateReleaseMin = 10,  kGateReleaseMax = 2000; // ms
static const int kGateRangeMin   = 0,   kGateRangeMax   = 80;   // dB of cut
static const int kGateLookaheadMin = 0, kGateLookaheadMax = 50; // ms

// ---------------------------------------------------------------------------
// Networked audio
//
// Which protocol a net block speaks. FastRoute's own announces itself, so a
// receiver can list what it has heard; VBAN is VB-Audio's, which VoiceMeeter
// and a good many phones already speak, and is found by asking rather than by
// listening. Stored in a session file, so the order is fixed once written.
// ---------------------------------------------------------------------------
enum NetProtocol
{
    NP_FASTROUTE = 0,
    NP_VBAN = 1,
};
static const int kNetProtocolCount = 2;
const wchar_t* NetProtocolName(int p);          // "FastRoute"

// Whether a sender broadcasts to a group that anybody can join, or sends to
// one named host. Multicast is what "broadcasting" means and costs the sender
// the same however many are listening; unicast crosses subnets and VPNs, which
// multicast generally does not.
enum NetMode
{
    NM_MULTICAST = 0,
    NM_UNICAST = 1,
};
static const int kNetModeCount = 2;
const wchar_t* NetModeName(int m);              // "to a group, for anybody"

// A packet has to fit one datagram, so the useful range here is short: 5 ms of
// 16 bit stereo at 48 kHz is already 960 bytes. What a given codec settles on
// is worked out rather than set, and the dialog says which.
static const int kNetPacketMsMin = 1,  kNetPacketMsMax = 20;
// How much the receiver holds back before playing, which is its whole defence
// against a network that delivers unevenly. 60 ms is comfortable on wired
// Ethernet; Wi-Fi usually wants more.
static const int kNetBufferMsMin = 10, kNetBufferMsMax = 500;
static const int kNetOpusKbpsMin = 32, kNetOpusKbpsMax = 320;

// How much loss the encoder should protect against. Opus puts a low rate copy
// of each frame inside the next packet, so a lost one can be rebuilt from its
// successor instead of guessed at; the figure is what fraction of packets it
// should assume go missing, and it buys that protection with bitrate. Zero
// turns it off.
static const int kNetOpusLossMin = 0, kNetOpusLossMax = 50;
// 0 is cheapest and 10 is best. Opus's own default is 10, and even that is not
// expensive next to the rest of a session.
static const int kNetOpusComplexityMin = 0, kNetOpusComplexityMax = 10;

// What the encoder should assume it is carrying. Opus tunes itself quite
// differently for speech and for music, and it guesses well, so automatic is
// the default and the other two are for when it guesses wrong.
enum NetOpusSignal { NOS_AUTO = 0, NOS_MUSIC = 1, NOS_VOICE = 2 };
static const int kNetOpusSignalCount = 3;
const wchar_t* NetOpusSignalName(int s);

// Variable bitrate spends bits where they are needed and is the right default
// for quality. Constant is for a link with a hard budget: it makes every
// packet the same size, which is easier on a congested network to predict even
// though it is worse per bit.
enum NetOpusRateMode { NOR_VBR = 0, NOR_CONSTRAINED = 1, NOR_CBR = 2 };
static const int kNetOpusRateModeCount = 3;
const wchar_t* NetOpusRateModeName(int m);
// Hops. One keeps a group on the local subnet, which is almost always what is
// wanted; zero keeps it inside this machine, which is useful for trying it out.
static const int kNetTtlMin = 0, kNetTtlMax = 16;

// A stream's name doubles as its identity, so two senders on one network want
// two different names.
static const int kNetNameMax = 31;

// Pitch shifter. Two octaves each way is as far as anything stays recognisable,
// and the cents are the same span again either side of a semitone so that any
// pitch in range can be reached from either direction.
static const int kPitchSemitonesMin = -24, kPitchSemitonesMax = 24;
static const int kPitchCentsMin = -100, kPitchCentsMax = 100;

// How long a window the shifter works on, which is also exactly what it costs
// in latency: 120, 60 and 30 ms. A shorter window resolves low frequencies
// more coarsely, so a bass line goes grainy long before a voice does.
enum PitchQuality
{
    PQ_BEST = 0,
    PQ_BALANCED = 1,
    PQ_LOW_DELAY = 2,
};
static const int kPitchQualityCount = 3;
const wchar_t* PitchQualityName(int q);      // "Best, about 120 ms of delay"
int PitchQualityMs(int q);                   // 120, 60, 30

// How far a shift is, said the way somebody would say it out loud.
std::wstring PitchText(int semitones, int cents);

// Normaliser. The target and the lookahead are the two controls that matter;
// the other two are what stop a normaliser being unusable. Without a ceiling
// on the gain it applies, a passage of near silence is worth 60 dB of boost
// and the room tone arrives like a jet engine.
static const int kNormTargetMin    = -40, kNormTargetMax    = 0;    // dBFS
static const int kNormLookaheadMin = 0,   kNormLookaheadMax = 200;  // ms
static const int kNormMaxGainMin   = 0,   kNormMaxGainMax   = 40;   // dB of boost
static const int kNormReleaseMin   = 50,  kNormReleaseMax   = 5000; // ms

// Clipping monitor. The threshold is how close to full scale counts, and 0 is
// full scale itself. The interval is how often it is willing to beep: a beep
// per buffer on a badly clipping signal would be a continuous tone.
static const int kClipThreshMin   = -12, kClipThreshMax   = 0;   // dBFS
static const int kClipIntervalMin = 1,   kClipIntervalMax = 30;  // seconds

// Channel mixer, per channel. Two channels because that is what the whole
// engine carries; kChannels itself belongs to the audio layer, which the
// document does not include.
static const int kMixChannels = 2;
// -60 dB is the bottom of the slider and is silence exactly, rather than the
// millionth of full scale the arithmetic would otherwise give: a control that
// says "as far down as it goes" should mean off.
static const int kChanGainMin = -60, kChanGainMax = 12;    // dB
static const int kChanPanMin  = -100, kChanPanMax = 100;   // hard left to hard right

// Buffer. The memory is the length times the rate times two channels times
// four bytes: a minute at 48 kHz is 23 MB, and the maximum is around two
// hundred, which is why there is a maximum at all.
static const int kBufSecondsMin = 10, kBufSecondsMax = 600;
static const int kBufStepMin    = 1,  kBufStepMax    = 120;   // seconds a skip moves

// ---------------------------------------------------------------------------
// Block actions
//
// An action is a moment rather than a setting. Every other thing a block can be
// told is a parameter: it is written into the document, and the running graph
// is rebuilt around it. "Rewind ten seconds" is not that - it is not a value
// the block has, it is something done to the block while it is playing, and it
// means nothing at all when the session is stopped.
//
// So actions take their own path. A hotkey bound to one reaches the live effect
// through the engine rather than through the document, and what it changes is
// never saved. What is saved is only the hotkey itself.
// ---------------------------------------------------------------------------
enum BlockActionId
{
    BA_REWIND  = 0,
    BA_FORWARD = 1,
    BA_LIVE    = 2,
    BA_SAVE    = 3,
    BA_COUNT   = 4,
};

// "Rewind", "Jump to Live". Shown in the action hotkeys dialog and read out.
const wchar_t* BlockActionName(int action);
// A sentence for the dialog, completing "<key> will ...".
const wchar_t* BlockActionDescription(int action);
// Which actions this block type has, in the order they should be listed. Empty
// for the blocks that have none, which is most of them.
std::vector<int> BlockActionsFor(const std::wstring& typeId);

// The makeup gain a full scale peak needs to come back out at full scale,
// which is what the compressor's automatic setting applies. The dialog shows
// the figure rather than leaving it a mystery.
double AutoMakeupDb(double threshDb, double ratio);

// ---------------------------------------------------------------------------
// Parametric equaliser
//
// Four bands, each of which is a filter in its own right: what shape it is,
// where it sits, how much, and how wide. That is the difference from the ten
// band graphic EQ next to it, whose bands are fixed and only have a level.
// ---------------------------------------------------------------------------
static const int kParaBands = 4;

enum ParaType
{
    PT_OFF = 0,
    PT_PEAK = 1,
    PT_LOW_SHELF = 2,
    PT_HIGH_SHELF = 3,
    PT_LOW_PASS = 4,
    PT_HIGH_PASS = 5,
    PT_COUNT = 6,
};

// "Peaking", "Low shelf". Used by the dialog and by the block summary.
const wchar_t* ParaTypeName(int type);
// Whether a shape does anything with the gain setting. A low or high pass
// does not: it removes what is past it whatever the gain says.
bool ParaTypeUsesGain(int type);
// "500 Hz" or "2.0 kHz", whichever reads better at that frequency.
std::wstring ParaHzText(int hz);
// One band as a line of text: "Band 2: Peaking at 500 Hz, +3 dB, Q 1.0".
// The dialog lists the bands with these, so a reader gets the whole band from
// the row rather than having to visit four controls to find out what it says.
struct Block;
std::wstring ParaBandText(const Block& b, int i);

static const int kParaHzMin   = 20,  kParaHzMax   = 20000;
static const int kParaGainMin = -18, kParaGainMax = 18;    // dB
// Q is kept in tenths, because a slider carries whole numbers and 0.7 is a
// setting people actually reach for.
static const int kParaQMin    = 1,   kParaQMax    = 100;   // 0.1 to 10.0

// Where a pan setting is: "hard left", "left 40", "centre", "right 100". Said
// the same way in the dialog and in the block's one-line summary, because a
// position written two ways is two things to learn.
std::wstring PanText(int pan);

// Graphic equaliser bands, one octave apart from 31 Hz to 16 kHz.
static const int kEqBands = 10;
const int* EqBandHz();                       // kEqBands centre frequencies
std::wstring EqBandLabel(int band);          // "31 Hz", "1 kHz", ...

// Our own modifier bits rather than Win32's MOD_*, so the model layer stays
// free of windows.h. Mapped at the point the hotkey is registered.
enum HotkeyMod { HK_CTRL = 1, HK_ALT = 2, HK_SHIFT = 4, HK_WIN = 8 };

// A key combination registered with Windows rather than with a window, so it
// works from whatever application happens to be in front.
struct Hotkey
{
    bool enabled = false;
    int  mods = 0;
    int  vk = 0;

    // Global keys are taken from the whole system with RegisterHotKey, so they
    // work with FastRoute out of sight - and are gone from every other program
    // for as long as FastRoute is running, which is a real cost for a common
    // combination. A window key is only read when a FastRoute window is in
    // front, so it costs nothing elsewhere and cannot clash with anything.
    //
    // True by default because that is what these keys were before the choice
    // existed, and a document written by an older build must not change
    // meaning when it is read by this one.
    bool global = true;

    bool Set() const { return enabled && mods != 0 && vk != 0; }
};

// What one key is called: "R", "Left Arrow", "F9", "Num 7", "Page Up". Named
// rather than looked up in a table of allowed keys, because a hotkey field you
// type into can be handed any key on the keyboard - including the ones no list
// of defaults would have thought to offer.
std::wstring KeyName(int vk);

// The key that goes by this name, or 0 for one that does not. Forgiving about
// how it is written: case, spaces and the word "arrow" are all optional, so
// "Left", "left arrow" and "LeftArrow" are the same key. KeyName's own output
// always reads back, so what a field is filled with is always something it
// will accept again.
int KeyFromName(const std::wstring& name);

// Spells a hotkey the way it is written on a menu, e.g. "Ctrl+Alt+R".
std::wstring HotkeyText(const Hotkey& h);
// The same, with the scope named when it is not global: "Ctrl+R (this window)".
// This is what the lists show, because two keys that read alike but work in
// different places are otherwise impossible to tell apart.
std::wstring HotkeyScopeText(const Hotkey& h);

// ---------------------------------------------------------------------------
// A block instance inside a session
// ---------------------------------------------------------------------------
struct Block
{
    std::wstring typeId;

    // input.device / output.device
    std::wstring deviceId;      // MMDevice endpoint id ("" == system default)
    std::wstring deviceName;    // cached friendly name for display

    // input.application: which application this block captures.
    int appSource = AS_NAMED;
    std::wstring processName;   // durable key, e.g. "chrome.exe"
    unsigned long processId = 0;// last seen pid; re-resolved by name at run time
    // More than one program in one block, so that a session can have a block
    // per group of things rather than per program: every screen reader in one,
    // every game in another. Empty means the single processName above, which
    // is what a session saved before this existed has. AppTargets is the only
    // thing that should decide between the two.
    std::vector<std::wstring> appNames;
    // Whether the block may start with none of them running. A session that
    // records a game is set up before the game is launched, and the capture
    // already picks a program up when it starts, so refusing to run was the
    // only thing in the way.
    bool appWaitForStart = false;

    // input.stream
    std::wstring url;           // http or https, an audio stream or a file
    int  bufferMs = 1000;       // network jitter needs far more than a device
    bool reconnect = true;
    int  retrySeconds = 5;

    // input.system: applications kept out of the system audio, by exe name.
    // Empty means everything, which is also the cheaper capture.
    std::vector<std::wstring> excludeApps;

    // denoise blocks
    int mixPct = 100;           // dry/wet, 0..100
    int suppressDb = 20;        // Speex only, shown positive, applied negative

    // effect.tone: shelving filters, in dB
    int bassDb = 0;             // -15..+15, low shelf
    int trebleDb = 0;           // -15..+15, high shelf

    // effect.eq10: one peaking filter per band, in dB
    int eqDb[kEqBands] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // -12..+12

    // effect.para: four bands, each a filter of its own. The defaults are four
    // peaks spread across the range at 0 dB, which is flat: one slider move
    // does something, rather than needing a band switched on first.
    int paraType[kParaBands]   = { PT_PEAK, PT_PEAK, PT_PEAK, PT_PEAK };
    int paraHz[kParaBands]     = { 100, 500, 2000, 8000 };
    int paraGainDb[kParaBands] = { 0, 0, 0, 0 };
    int paraQ[kParaBands]      = { 10, 10, 10, 10 };         // tenths, so 1.0

    // effect.channels
    int  widthPct = 100;        // 0 mono, 100 unchanged, up to 200
    bool swapChannels = false;
    bool centreCancel = false;

    // Per incoming channel, applied before the width and centre controls: how
    // loud it is, and where in the stereo field it lands. The defaults leave
    // each channel at full level on its own side, which is the identity, so a
    // session saved before these existed loads unchanged.
    int  chanGainDb[kMixChannels] = { 0, 0 };
    int  chanPan[kMixChannels]    = { kChanPanMin, kChanPanMax };

    // effect.compressor
    int  compThreshDb = -20;    // dBFS the reduction starts at
    int  compRatio = 4;         // n:1; 1 is no compression at all
    int  compAttackMs = 10;
    int  compReleaseMs = 100;
    int  compMakeupDb = 0;      // ignored while compAutoMakeup is set
    bool compAutoMakeup = true;

    // effect.gate
    int  gateThreshDb = -40;    // dBFS below which the gate shuts
    int  gateAttackMs = 5;      // how fast it opens
    int  gateHoldMs = 200;      // how long it stays open after the sound stops
    int  gateReleaseMs = 150;   // how fast it shuts once the hold is up
    int  gateRangeDb = 60;      // how far down it goes; 0 is no gate at all
    // How far ahead the detector looks, and the block's latency. The gate
    // starts opening this long before the sound that opened it comes out, so
    // a lookahead at least as long as the attack means the word arrives with
    // the gate already open instead of with its first consonant shaved off.
    int  gateLookaheadMs = 5;

    // input.net and output.net
    //
    // Shared between the two because they are two ends of one link and the
    // questions are the same: which protocol, where, called what. The rest
    // belongs to one direction or the other, and is marked.
    int  netProtocol = NP_FASTROUTE;
    int  netMode = NM_MULTICAST;        // sending only
    std::wstring netAddress;            // group or host; empty is the default
    int  netPort = 0;                   // 0 is the protocol's own default
    std::wstring netStreamName;         // what it is called on the network
    int  netCodec = 0;                  // sending only; a net::NetCodec
    int  netPacketMs = 5;               // sending only
    int  netOpusKbps = 128;             // sending only
    // Opus, in more detail. All sending only, and all ignored by every other
    // codec. The defaults are Opus's own except the loss figure, which is set
    // low rather than off: a little protection costs little and a network that
    // never loses a packet is rarer than one that does.
    int  netOpusLossPct = 5;
    int  netOpusComplexity = 10;
    int  netOpusSignal = NOS_AUTO;
    int  netOpusRateMode = NOR_VBR;
    bool netOpusDtx = false;
    int  netTtl = 1;                    // sending only
    int  netBufferMs = 60;              // receiving only
    // The sender last chosen from the discovered list. The name is what is
    // really being remembered: a sender that restarts comes back with a new
    // id, and somebody who picked "Desk mic" yesterday means the thing called
    // Desk mic today.
    unsigned int netSourceId = 0;       // receiving only

    // input.asio and output.asio
    //
    // Which driver is in deviceId, as the name it registered under: an ASIO
    // driver has no endpoint id and its name is the only durable handle on it.
    // What is here is the pair of channels the stereo bus maps to, and they
    // are per direction because an interface's third and fourth inputs have
    // nothing to do with its third and fourth outputs.
    int asioInLeft = 0,  asioInRight = 1;
    int asioOutLeft = 0, asioOutRight = 1;

    // effect.pitch
    //
    // Whole semitones and the hundredths between them, kept apart because they
    // are two different questions: which note, and how far off it. Nothing is
    // built and nothing is delayed while both are zero.
    int  pitchSemitones = 0;
    int  pitchCents = 0;
    int  pitchQuality = PQ_BEST;

    // effect.limiter
    int  limCeilingDb = -1;     // dBFS nothing is allowed above
    int  limReleaseMs = 50;
    int  limLookaheadMs = 5;    // 0 makes it a clipper, and costs no latency

    // effect.normalise
    int  normTargetDb = -3;     // dBFS the loudest part is brought to
    int  normLookaheadMs = 50;  // how far ahead it looks, and its latency
    int  normMaxGainDb = 20;    // the most boost it will ever apply
    int  normReleaseMs = 1000;  // how fast the gain is allowed to come back up

    // effect.clipmonitor
    int  clipThreshDb = 0;      // dBFS at or above which it counts as clipping
    int  clipIntervalS = 2;     // the least time between two beeps

    // effect.buffer. Saving reuses the recorder's folder, template, format and
    // bitrate fields, because it is the same question and answering it twice
    // in two different places would be the wrong kind of thrift.
    int  bufSeconds = 60;       // how much is kept
    int  bufStepSec = 10;       // how far one rewind or fast forward moves

    // A hotkey per action, for the blocks that have actions. Unset for the
    // rest, which costs four small structs and no thought.
    Hotkey actionKey[BA_COUNT];

    // effect.plugin: which external plugin, and what it is set to.
    //
    // The path and the identifier together are what finds it again. The
    // identifier alone is not enough - two copies of the same plugin can be
    // installed - and the path alone is not either, because one module often
    // holds several plugins.
    int          plugFormat = 0;   // 0 CLAP, 1 VST3, matching PlugFormat
    std::wstring plugPath;
    std::wstring plugUid;
    std::wstring plugName;         // remembered so a missing plugin can be named
    // Parameter values, by the plugin's own identifier. Written on the way out
    // and pushed back in when the block starts. A plugin whose state is more
    // than its parameters - a sampler with a file loaded, say - will not come
    // back complete from this, and that is a known limit rather than a bug.
    std::vector<std::pair<unsigned long long, double>> plugParams;

    // output.recorder
    std::wstring folder;        // "" == Music\FastRoute
    std::wstring nameTemplate;  // supports {session} {block} {index} {date}
                                // {time} {datetime} {n}
    int recFormat = 0;          // 0 wav, 1 flac, 2 mp3
    int bitrateKbps = 192;
    int recMode = RM_MIXED;     // the mix, a file per input block, or both
    // What the file is written as, as opposed to what the session runs at.
    // Zero for either means "whatever recording would have used anyway", so
    // a document written before these existed reads exactly as it did.
    //
    // The session's rate is the first output block's, which is not always a
    // rate the chosen format can even be written at - Windows' FLAC encoder
    // starts at 44100, so a session running at 32000 could not record FLAC at
    // all. Choosing the file's rate here is what makes that possible, as well
    // as being worth having in its own right.
    int recRateHz = 0;          // 0 == the session's own rate
    int recBits = 0;            // 0 == the format's default; 16, 24 or 32

    // output.recorder: silence gate. Audio quieter than the threshold is not
    // written at all, so the file skips it rather than storing it.
    bool skipSilence = false;
    int  silenceDb = -50;       // dBFS, -80 to -20
    // How long to keep recording after it goes quiet, 0 to 30 seconds, or
    // kHoldAlways to never shut the gate again once it has opened - which
    // turns the threshold into a "wait for sound before starting" control.
    int  silenceHoldSec = 2;

    // output.recorder: start a new file every so often
    int  splitMode = 0;         // 0 never, 1 after minutes, 2 after megabytes
    int  splitMinutes = 30;     // 1 to 1440
    int  splitMb = 100;         // 1 to 4000

    // all blocks
    // A name of the user's own, shown instead of the block type. Two Gain
    // blocks in one session are otherwise told apart only by their position.
    std::wstring label;
    int  volumePct = 100;       // 0..150
    bool enabled = true;        // bypassed when false, without leaving the list
    Hotkey hotkey;              // toggles `enabled` from anywhere

    BlockKind    Kind() const;
    std::wstring TypeName() const;
    std::wstring ParamSummary() const;
    bool         IsConfigured() const;
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
struct Session
{
    int id = 0;                 // stable across reordering/deletion
    std::wstring name;
    bool autoRun = false;       // started when FastRoute launches
    Hotkey hotkey;              // starts and stops this session from anywhere
    std::vector<Block> blocks;  // pipeline order, kinds may interleave freely
};

// ---------------------------------------------------------------------------
// Application settings
// ---------------------------------------------------------------------------

// One plugin, as the document remembers it between scans. This mirrors
// PluginDesc, which lives in the plugin layer: the model must not depend on
// the plugin headers, and the blocks library has to be able to list what was
// found without loading anything.
struct PluginEntry
{
    int          format = 0;       // 0 CLAP, 1 VST3
    std::wstring path;
    std::wstring uid;
    std::wstring name;
    std::wstring vendor;

    std::wstring Label() const;    // "TAL Reverb 4 (VST3)"
};

// How often FastRoute looks for a new build of itself. Stored in the settings
// file, so the order is fixed once written.
enum UpdateCheck
{
    UC_NEVER = 0,
    UC_STARTUP = 1,
    UC_HOURLY = 2,
    UC_DAILY = 3,
};
static const int kUpdateCheckCount = 4;
const wchar_t* UpdateCheckName(int c);

struct AppSettings
{
    // Shows FastRoute again once it is hidden. It has to be global: when the
    // window is gone there is nothing to press a key in.
    Hotkey showHide = { true, HK_CTRL | HK_ALT, 'R' };

    // Folders to look in for CLAP and VST3 plugins, and what the last scan
    // found in them. The results are kept so that starting FastRoute does not
    // mean loading every plugin on the machine to find out their names.
    std::vector<std::wstring> pluginPaths;
    std::vector<PluginEntry>  plugins;

    // Where a newly added Recorder block writes to. A recorder with no folder
    // of its own falls back to Music\FastRoute, which is fine as a last
    // resort but a poor place to keep going back to if the recordings belong
    // somewhere else. Empty means no default, and the fallback stands.
    std::wstring recFolder;

    // Looking for a new build of FastRoute. Off by default: a program that
    // phones home without being asked should be asked first, and the Settings
    // dialog is where to ask.
    int  updateCheck = UC_NEVER;
    // Whether a check that finds something gets on with it. Without this a
    // scheduled check asks before downloading, which is the right default -
    // an update that restarts the program in the middle of a recording would
    // be a poor surprise.
    bool updateSilent = false;

    // Which groups in the blocks library are shut. The collapsed ones rather
    // than the expanded ones, so that an empty list means everything open,
    // which is how it behaved before this was remembered at all - and so that
    // a group added in a later version arrives open rather than hidden.
    std::vector<std::wstring> libraryCollapsed;
};

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

enum SchedRepeat { SR_ONCE = 0, SR_DAILY = 1, SR_WEEKDAYS = 2 };
enum SchedStop   { SS_NONE = 0, SS_AT_TIME = 1, SS_AFTER = 2 };

// One entry in the schedule: start a session at a time, and possibly stop it
// again either at another time or after a while.
struct ScheduleEvent
{
    int id = 0;
    int sessionId = 0;
    bool enabled = true;

    int repeat = SR_ONCE;
    int weekdays = 0;                   // bit 0 Sunday, through bit 6 Saturday
    int year = 0, month = 0, day = 0;   // the date, for a one-off
    int hour = 0, minute = 0;

    int stopMode = SS_NONE;
    int stopHour = 0, stopMinute = 0;
    int afterMinutes = 60;

    // Deleted once it has run, for a reminder rather than a routine.
    bool singleUse = false;

    // When it last started something, in minutes since 1601 - the same clock
    // the scheduler compares against, so an event cannot fire twice for one
    // occurrence. Zero means it never has.
    long long lastFired = 0;
};

// "Every day at 09:00", "Mon, Wed, Fri at 18:30", "On 2026-08-24 at 14:00".
std::wstring ScheduleWhenText(const ScheduleEvent& e);
// "Runs until stopped", "Stops at 10:00", "Stops after 90 minutes".
std::wstring ScheduleStopText(const ScheduleEvent& e);

class SessionStore
{
public:
    std::vector<Session> sessions;
    std::vector<ScheduleEvent> events;
    // Kept in the same document: there is one file, and settings are part of
    // what FastRoute is set up to do.
    AppSettings settings;

    void Load();
    bool Save(std::wstring* err = nullptr) const;
    std::wstring Path() const;
    // Where versions before the portable one kept it. Read from once, when
    // there is nothing beside the executable yet.
    std::wstring LegacyPath() const;

    // Sessions are kept in name order. Every command in the main window works
    // by index into this vector, so sorting it is what puts the list in order
    // without the list and the document being able to disagree about which
    // row is which.
    void SortSessions();

    Session* ById(int id);
    int      NewId();

    ScheduleEvent* EventById(int id);
    int            NewEventId();
};

extern SessionStore g_store;
