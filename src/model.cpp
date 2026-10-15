#include "model.h"
#include "util.h"
#include "json.h"
#include "audio/audio.h"
#include "audio/net.h"
#include <windows.h>
#include <stdio.h>
#include <algorithm>

SessionStore g_store;

// ---------------------------------------------------------------------------

const std::vector<BlockType>& AllBlockTypes()
{
    static const std::vector<BlockType> types = {
        { BT_INPUT_APP,     L"Application",     L"Input Blocks",  BlockKind::Input  },
        { BT_INPUT_DEVICE,  L"Input Device",    L"Input Blocks",  BlockKind::Input  },
        { BT_INPUT_SYSTEM,  L"System Audio",    L"Input Blocks",  BlockKind::Input  },
        { BT_INPUT_STREAM,  L"Stream URL",      L"Input Blocks",  BlockKind::Input  },
        { BT_INPUT_NET,     L"Net Receive",     L"Input Blocks",  BlockKind::Input  },
        { BT_INPUT_ASIO,    L"ASIO Input",      L"Input Blocks",  BlockKind::Input  },
        // The effects are listed alphabetically, which is the order the blocks
        // library tree shows them in. Keep a new one in its place.
        { BT_FX_EQ10,       L"10 Band EQ",      L"Built-in Effects", BlockKind::Effect },
        { BT_FX_PARA,       L"4 Band Parametric EQ", L"Built-in Effects", BlockKind::Effect },
        { BT_FX_TONE,       L"Bass and Treble", L"Built-in Effects", BlockKind::Effect },
        { BT_FX_CHANNELS,   L"Channel Mixer",   L"Built-in Effects", BlockKind::Effect },
        { BT_FX_COMP,       L"Compressor",      L"Built-in Effects", BlockKind::Effect },
        { BT_EFFECT_GAIN,   L"Gain",            L"Built-in Effects", BlockKind::Effect },
        { BT_FX_LIMIT,      L"Limiter",         L"Built-in Effects", BlockKind::Effect },
        { BT_FX_GATE,       L"Noise Gate",      L"Built-in Effects", BlockKind::Effect },
        { BT_FX_NORM,       L"Normaliser",      L"Built-in Effects", BlockKind::Effect },
        { BT_FX_PITCH,      L"Pitch Shifter",   L"Built-in Effects", BlockKind::Effect },
        { BT_FX_CLIPMON,    L"Clipping Monitor", L"Built-in Effects", BlockKind::Effect },
        { BT_FX_BUFFER,     L"Buffer",          L"Built-in Effects", BlockKind::Effect },
        { BT_FX_RNNOISE,    L"RNNoise Denoise", L"Built-in Effects", BlockKind::Effect },
        { BT_FX_SPEEX,      L"Speex Denoise",   L"Built-in Effects", BlockKind::Effect },
        // Stands for every external plugin. It is never shown in the library
        // under this name: the tree lists what the last scan actually found.
        { BT_FX_PLUGIN,     L"Plugin",          L"External Effects", BlockKind::Effect },
        { BT_OUTPUT_DEVICE, L"Output Device",   L"Output Blocks", BlockKind::Output },
        { BT_OUT_RECORDER,  L"Recorder",        L"Output Blocks", BlockKind::Output },
        { BT_OUT_NET,       L"Net Send",        L"Output Blocks", BlockKind::Output },
        { BT_OUT_ASIO,      L"ASIO Output",     L"Output Blocks", BlockKind::Output },
    };
    return types;
}

// One octave apart, the usual graphic equaliser layout. The top band is
// dropped by the filter design when it lands at or above Nyquist.
const wchar_t* ParaTypeName(int type)
{
    switch (type)
    {
    case PT_PEAK:       return L"Peaking";
    case PT_LOW_SHELF:  return L"Low shelf";
    case PT_HIGH_SHELF: return L"High shelf";
    case PT_LOW_PASS:   return L"Low pass";
    case PT_HIGH_PASS:  return L"High pass";
    default:            return L"Off";
    }
}

bool ParaTypeUsesGain(int type)
{
    return type == PT_PEAK || type == PT_LOW_SHELF || type == PT_HIGH_SHELF;
}

// "500 Hz" or "2.0 kHz", whichever reads better at that frequency.
std::wstring ParaHzText(int hz)
{
    if (hz >= 10000) return FormatW(L"%d kHz", hz / 1000);
    if (hz >= 1000)  return FormatW(L"%.1f kHz", (double)hz / 1000.0);
    return FormatW(L"%d Hz", hz);
}

std::wstring ParaBandText(const Block& b, int i)
{
    if (i < 0 || i >= kParaBands) return L"";
    const int type = b.paraType[i];
    if (type == PT_OFF) return FormatW(L"Band %d: off", i + 1);

    std::wstring out = FormatW(L"Band %d: %s at %s", i + 1, ParaTypeName(type),
                               ParaHzText(b.paraHz[i]).c_str());
    if (ParaTypeUsesGain(type)) out += FormatW(L", %+d dB", b.paraGainDb[i]);
    out += FormatW(L", Q %.1f", (double)b.paraQ[i] / 10.0);
    return out;
}

const int* EqBandHz()
{
    static const int hz[kEqBands] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    return hz;
}

std::wstring EqBandLabel(int band)
{
    if (band < 0 || band >= kEqBands) return L"";
    int hz = EqBandHz()[band];
    if (hz >= 1000) return FormatW(L"%d kHz", hz / 1000);
    return FormatW(L"%d Hz", hz);
}

std::wstring PanText(int pan)
{
    if (pan <= kChanPanMin) return L"hard left";
    if (pan >= kChanPanMax) return L"hard right";
    if (pan == 0) return L"centre";
    // The number is how far over it has been moved, not a signed coordinate.
    // "left 40" is a place; "-40" is a reading somebody has to decode, and
    // which half of the field it means is a convention rather than a word.
    return pan < 0 ? FormatW(L"left %d", -pan) : FormatW(L"right %d", pan);
}

const wchar_t* UpdateCheckName(int c)
{
    switch (c)
    {
    case UC_STARTUP: return L"when FastRoute starts";
    case UC_HOURLY:  return L"when it starts, and every hour";
    case UC_DAILY:   return L"when it starts, and every day";
    default:         return L"never";
    }
}

const wchar_t* NetProtocolName(int p)
{
    return (p == NP_VBAN) ? L"VBAN" : L"FastRoute";
}

const wchar_t* NetOpusSignalName(int s)
{
    switch (s)
    {
    case NOS_MUSIC: return L"music";
    case NOS_VOICE: return L"speech";
    default:        return L"work it out";
    }
}

const wchar_t* NetOpusRateModeName(int m)
{
    switch (m)
    {
    case NOR_CONSTRAINED: return L"variable, within one packet size";
    case NOR_CBR:         return L"constant";
    default:              return L"variable";
    }
}

const wchar_t* NetModeName(int m)
{
    return (m == NM_UNICAST) ? L"to one host" : L"to a group, for anybody";
}

// The three windows the shifter can work on, named by what they cost rather
// than by "high" and "low": the delay is the thing being chosen, and it is the
// thing somebody will notice.
const wchar_t* PitchQualityName(int q)
{
    switch (q)
    {
    case PQ_BALANCED:  return L"balanced, about 60 ms of delay";
    case PQ_LOW_DELAY: return L"low delay, about 30 ms";
    default:           return L"best, about 120 ms of delay";
    }
}

int PitchQualityMs(int q)
{
    switch (q)
    {
    case PQ_BALANCED:  return 60;
    case PQ_LOW_DELAY: return 30;
    default:           return 120;
    }
}

// A shift, said the way it would be said out loud. Semitones and cents are two
// controls but one number, and reading them back separately - "3 semitones,
// -50 cents" - leaves the listener to do the arithmetic to find out that it is
// two and a half semitones up.
std::wstring PitchText(int semitones, int cents)
{
    const int total = semitones * 100 + cents;
    if (total == 0) return L"no shift";

    const wchar_t* dir = total > 0 ? L"up" : L"down";
    const int mag = total < 0 ? -total : total;
    const int whole = mag / 100, part = mag % 100;

    if (part == 0)
    {
        if (whole == 12) return FormatW(L"%s an octave", dir);
        if (whole == 24) return FormatW(L"%s two octaves", dir);
        return FormatW(L"%s %d semitone%s", dir, whole, whole == 1 ? L"" : L"s");
    }
    if (whole == 0) return FormatW(L"%s %d cents", dir, part);
    return FormatW(L"%s %d semitone%s and %d cents", dir, whole,
                   whole == 1 ? L"" : L"s", part);
}

std::wstring PluginEntry::Label() const
{
    return FormatW(L"%s (%s)", name.c_str(), format == 1 ? L"VST3" : L"CLAP");
}

double AutoMakeupDb(double threshDb, double ratio)
{
    if (ratio <= 1.0 || threshDb >= 0.0) return 0.0;
    double db = (1.0 - 1.0 / ratio) * -threshDb;
    if (db > (double)kCompMakeupMax) db = (double)kCompMakeupMax;
    return db;
}

const BlockType* FindBlockType(const std::wstring& id)
{
    for (const BlockType& t : AllBlockTypes())
        if (id == t.id) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------

BlockKind Block::Kind() const
{
    const BlockType* t = FindBlockType(typeId);
    return t ? t->kind : BlockKind::Effect;
}

std::wstring Block::TypeName() const
{
    if (!label.empty()) return label;
    // A plugin block is named after its plugin. "Plugin" in the list would be
    // true and useless, and a session with three of them unreadable.
    if (typeId == BT_FX_PLUGIN && !plugName.empty())
        return FormatW(L"%s (%s)", plugName.c_str(), plugFormat == 1 ? L"VST3" : L"CLAP");
    const BlockType* t = FindBlockType(typeId);
    return t ? t->name : typeId;
}

const std::vector<const wchar_t*>& LibraryCategories()
{
    static const std::vector<const wchar_t*> cats = {
        L"Input Blocks", L"Built-in Effects", L"External Effects", L"Output Blocks"
    };
    return cats;
}

std::vector<LibraryItem> BuildLibrary()
{
    std::vector<LibraryItem> out;
    for (const BlockType& t : AllBlockTypes())
    {
        // The plugin placeholder is replaced below by the real plugins, and
        // an empty External Effects group is better than one holding a block
        // that cannot be configured.
        if (std::wstring(t.id) == BT_FX_PLUGIN) continue;
        LibraryItem it;
        it.typeId = t.id;
        it.name = t.name;
        it.category = t.category;
        it.kind = t.kind;
        out.push_back(it);
    }

    for (size_t i = 0; i < g_store.settings.plugins.size(); ++i)
    {
        const PluginEntry& pe = g_store.settings.plugins[i];
        LibraryItem it;
        it.typeId = BT_FX_PLUGIN;
        it.name = pe.Label();
        it.category = L"External Effects";
        it.kind = BlockKind::Effect;
        it.pluginIndex = (int)i;
        out.push_back(it);
    }
    return out;
}

void InitBlockFrom(Block& b, const LibraryItem& item)
{
    b.typeId = item.typeId;
    if (item.typeId == BT_INPUT_DEVICE || item.typeId == BT_OUTPUT_DEVICE)
        b.deviceName = L"System default";

    // A new recorder starts in the folder set in Settings, if there is one.
    // It is a starting point rather than a binding: the block keeps its own
    // copy, so changing the default later leaves existing recorders alone.
    if (item.typeId == BT_OUT_RECORDER)
        b.folder = g_store.settings.recFolder;

    if (item.typeId == BT_FX_PLUGIN && item.pluginIndex >= 0 &&
        (size_t)item.pluginIndex < g_store.settings.plugins.size())
    {
        const PluginEntry& pe = g_store.settings.plugins[(size_t)item.pluginIndex];
        b.plugFormat = pe.format;
        b.plugPath = pe.path;
        b.plugUid = pe.uid;
        b.plugName = pe.name;
    }
}

std::vector<std::wstring> TrackNamesFor(const std::vector<Block>& blocks)
{
    std::vector<std::wstring> out(blocks.size());

    for (size_t i = 0; i < blocks.size(); ++i)
    {
        if (blocks[i].Kind() != BlockKind::Input) continue;

        bool asked = false;
        for (size_t j = i + 1; j < blocks.size() && !asked; ++j)
            asked = blocks[j].typeId == BT_OUT_RECORDER &&
                    blocks[j].recMode != RM_MIXED;
        if (!asked) continue;

        // Counted over every input, asked for or not, so that a block's file
        // keeps its name when a recorder above it stops wanting tracks.
        const std::wstring name = blocks[i].TypeName();
        int seen = 0;
        for (size_t j = 0; j < i; ++j)
            if (blocks[j].Kind() == BlockKind::Input &&
                blocks[j].TypeName() == name) ++seen;

        out[i] = seen ? FormatW(L"%s %d", name.c_str(), seen + 1) : name;
    }
    return out;
}

std::vector<std::wstring> AppTargets(const Block& b)
{
    if (b.typeId != BT_INPUT_APP || b.appSource != AS_NAMED)
        return std::vector<std::wstring>();
    if (!b.appNames.empty()) return b.appNames;
    if (!b.processName.empty()) return { b.processName };
    return std::vector<std::wstring>();
}

bool Block::IsConfigured() const
{
    if (typeId == BT_FX_PLUGIN)    return !plugPath.empty();
    if (typeId == BT_INPUT_APP)
        return appSource != AS_NAMED || !AppTargets(*this).empty();
    if (typeId == BT_INPUT_STREAM) return !url.empty();
    if (typeId == BT_OUT_RECORDER) return true;   // blank folder means the Music folder
    if (typeId == BT_INPUT_DEVICE || typeId == BT_OUTPUT_DEVICE ||
        typeId == BT_INPUT_SYSTEM) return true;   // "" == default device
    // There is no default ASIO driver to fall back on, so one has to be named.
    if (typeId == BT_INPUT_ASIO || typeId == BT_OUT_ASIO) return !deviceId.empty();
    return true;
}

std::wstring Block::ParamSummary() const
{
    if (typeId == BT_INPUT_APP)
    {
        if (appSource == AS_FOLLOW)
            return FormatW(L"whichever application is in front, following as that "
                           L"changes, volume %d%%", volumePct);
        if (appSource == AS_FOREGROUND)
            return FormatW(L"the application in front when this block starts, "
                           L"volume %d%%", volumePct);
        const std::vector<std::wstring> who = AppTargets(*this);
        if (who.empty())
            return FormatW(L"(not set), volume %d%%", volumePct);
        const wchar_t* wait = appWaitForStart ? L", waiting for them to start"
                                             : L"";
        if (who.size() == 1)
            return FormatW(L"%s and its child processes%s, volume %d%%",
                           who[0].c_str(), wait, volumePct);
        // Named in full rather than counted: "3 applications" would mean
        // opening the block to find out which three.
        std::wstring list;
        for (size_t i = 0; i < who.size(); ++i)
        {
            if (i) list += (i + 1 == who.size()) ? L" and " : L", ";
            list += who[i];
        }
        return FormatW(L"%s, each with its child processes%s, volume %d%%",
                       list.c_str(), wait, volumePct);
    }
    if (typeId == BT_INPUT_DEVICE || typeId == BT_OUTPUT_DEVICE)
    {
        std::wstring dev = deviceId.empty() ? std::wstring(L"System default")
                                            : (deviceName.empty() ? std::wstring(L"(unknown device)") : deviceName);
        return FormatW(L"%s, volume %d%%", dev.c_str(), volumePct);
    }
    if (typeId == BT_INPUT_STREAM)
    {
        std::wstring where = url.empty() ? std::wstring(L"(no address set)") : url;
        return FormatW(L"%s, %d ms buffer%s, volume %d%%", where.c_str(), bufferMs,
                       reconnect ? FormatW(L", reconnecting after %d s", retrySeconds).c_str()
                                 : L", no reconnect",
                       volumePct);
    }
    if (typeId == BT_INPUT_SYSTEM)
    {
        std::wstring dev = deviceId.empty() ? std::wstring(L"the default output")
                                            : (deviceName.empty() ? std::wstring(L"(unknown device)") : deviceName);
        std::wstring skip;
        for (const std::wstring& e : excludeApps)
        {
            skip += skip.empty() ? L", without " : L", ";
            skip += e;
        }
        return FormatW(L"everything on %s%s, volume %d%%",
                       dev.c_str(), skip.c_str(), volumePct);
    }
    if (typeId == BT_EFFECT_GAIN)
        return FormatW(L"gain %d%%", volumePct);
    if (typeId == BT_FX_SPEEX)
        return FormatW(L"suppress %d dB, mix %d%%", suppressDb, mixPct);
    if (typeId == BT_FX_RNNOISE)
        return FormatW(L"mix %d%%", mixPct);
    if (typeId == BT_FX_TONE)
        return FormatW(L"bass %+d dB, treble %+d dB", bassDb, trebleDb);
    if (typeId == BT_FX_EQ10)
    {
        std::wstring out;
        for (int i = 0; i < kEqBands; ++i)
        {
            if (eqDb[i] == 0) continue;
            if (!out.empty()) out += L", ";
            out += FormatW(L"%s %+d dB", EqBandLabel(i).c_str(), eqDb[i]);
        }
        return out.empty() ? std::wstring(L"flat") : out;
    }
    if (typeId == BT_FX_PARA)
    {
        std::wstring out;
        for (int i = 0; i < kParaBands; ++i)
        {
            if (paraType[i] == PT_OFF) continue;
            // A peak or a shelf at 0 dB is doing nothing, so it is not worth
            // a place in a one line summary. A pass filter always is.
            if (ParaTypeUsesGain(paraType[i]) && paraGainDb[i] == 0) continue;
            if (!out.empty()) out += L", ";
            out += FormatW(L"%s at %s", ParaTypeName(paraType[i]),
                           ParaHzText(paraHz[i]).c_str());
            if (ParaTypeUsesGain(paraType[i])) out += FormatW(L" %+d dB", paraGainDb[i]);
        }
        return out.empty() ? std::wstring(L"flat") : out;
    }
    if (typeId == BT_FX_CHANNELS)
    {
        std::wstring out = FormatW(L"width %d%%", widthPct);
        if (swapChannels) out += L", channels reversed";
        if (centreCancel) out += L", centre cancelled";
        // Only what has been moved. A summary that always spelled out both
        // channels would be four clauses of "0 dB, hard left" on the blocks
        // nobody has touched, which is where most of them will stay.
        for (int c = 0; c < kMixChannels; ++c)
        {
            const wchar_t* side = c == 0 ? L"left" : L"right";
            if (chanGainDb[c] != 0)
                out += FormatW(L", %s %+d dB", side, chanGainDb[c]);
            if (chanPan[c] != (c == 0 ? kChanPanMin : kChanPanMax))
                out += FormatW(L", %s to %s", side, PanText(chanPan[c]).c_str());
        }
        return out;
    }
    if (typeId == BT_FX_COMP)
    {
        std::wstring makeup = compAutoMakeup
            ? FormatW(L"automatic makeup (%+.0f dB)", AutoMakeupDb(compThreshDb, compRatio))
            : FormatW(L"makeup %+d dB", compMakeupDb);
        return FormatW(L"%d dB threshold, %d:1, attack %d ms, release %d ms, %s",
                       compThreshDb, compRatio, compAttackMs, compReleaseMs,
                       makeup.c_str());
    }
    if (typeId == BT_FX_GATE)
    {
        if (gateRangeDb == 0) return L"open, nothing is being cut";
        return FormatW(L"below %d dB, cut by %d dB; attack %d ms, hold %d ms, "
                       L"release %d ms, %s", gateThreshDb, gateRangeDb,
                       gateAttackMs, gateHoldMs, gateReleaseMs,
                       gateLookaheadMs > 0
                           ? FormatW(L"%d ms lookahead", gateLookaheadMs).c_str()
                           : L"no lookahead");
    }
    if (typeId == BT_INPUT_ASIO || typeId == BT_OUT_ASIO)
    {
        // The driver's name first, because that is which interface this is,
        // and then the pair of channels. Channels are numbered from one here
        // and from zero inside: every interface is labelled from one on its
        // front panel, and the front panel is what somebody is looking at.
        if (deviceId.empty())
            return L"no ASIO driver chosen yet; press Enter to pick one";
        const bool in = (typeId == BT_INPUT_ASIO);
        const int l = in ? asioInLeft : asioOutLeft;
        const int r = in ? asioInRight : asioOutRight;
        std::wstring chans = (l == r)
            ? FormatW(L"channel %d, the same on both sides", l + 1)
            : FormatW(L"channels %d and %d", l + 1, r + 1);
        return FormatW(L"%s, %s, volume %d%%", deviceId.c_str(), chans.c_str(),
                       volumePct);
    }
    if (typeId == BT_INPUT_NET)
    {
        // What somebody wants read back first is which stream this is, so the
        // name comes before the machinery. A block that has not been pointed
        // at anything says so rather than describing a buffer it is not using.
        std::wstring who = !netStreamName.empty() ? FormatW(L"\"%s\"", netStreamName.c_str())
                         : !netAddress.empty()    ? netAddress
                                                  : std::wstring();
        if (who.empty())
            return L"no sender chosen yet; press Enter to pick one";
        return FormatW(L"%s over %s, %d ms buffer", who.c_str(),
                       NetProtocolName(netProtocol), netBufferMs);
    }
    if (typeId == BT_OUT_NET)
    {
        // VBAN carries PCM and nothing else here, so saying the chosen codec
        // would be saying something untrue.
        const bool vban = (netProtocol == NP_VBAN);
        const std::wstring codec = vban ? std::wstring(L"16 bit PCM")
                                        : net::NetCodecName(netCodec);
        std::wstring where = (netMode == NM_UNICAST)
            ? (netAddress.empty() ? std::wstring(L"nowhere yet")
                                  : FormatW(L"to %s", netAddress.c_str()))
            : L"to a group";
        return FormatW(L"%s %s as \"%s\", %s", NetProtocolName(netProtocol),
                       where.c_str(),
                       netStreamName.empty() ? L"unnamed" : netStreamName.c_str(),
                       codec.c_str());
    }
    if (typeId == BT_FX_PITCH)
    {
        if (pitchSemitones == 0 && pitchCents == 0)
            return L"no shift, so nothing is delayed";
        return FormatW(L"%s, %s", PitchText(pitchSemitones, pitchCents).c_str(),
                       PitchQualityName(pitchQuality));
    }
    if (typeId == BT_FX_LIMIT)
    {
        return FormatW(L"ceiling %d dB, release %d ms, %s",
                       limCeilingDb, limReleaseMs,
                       limLookaheadMs > 0
                           ? FormatW(L"%d ms lookahead", limLookaheadMs).c_str()
                           : L"no lookahead");
    }
    if (typeId == BT_FX_NORM)
    {
        return FormatW(L"target %d dB, %s, up to %+d dB of boost, release %d ms",
                       normTargetDb,
                       normLookaheadMs > 0
                           ? FormatW(L"%d ms lookahead", normLookaheadMs).c_str()
                           : L"no lookahead",
                       normMaxGainDb, normReleaseMs);
    }
    if (typeId == BT_FX_CLIPMON)
    {
        return FormatW(L"beeps at %s, at most every %d second%s; the audio is "
                       L"not altered",
                       clipThreshDb == 0
                           ? L"full scale"
                           : FormatW(L"%d dB", clipThreshDb).c_str(),
                       clipIntervalS, clipIntervalS == 1 ? L"" : L"s");
    }
    if (typeId == BT_FX_BUFFER)
    {
        std::wstring out = FormatW(L"keeps %s, skips %d s at a time",
                                   DurationText(bufSeconds).c_str(), bufStepSec);
        int bound = 0;
        for (int a = 0; a < BA_COUNT; ++a) if (actionKey[a].Set()) ++bound;
        if (bound == 0) out += L"; no action hotkeys set";
        else out += FormatW(L"; %d action hotkey%s", bound, bound == 1 ? L"" : L"s");
        return out;
    }
    if (typeId == BT_FX_PLUGIN)
    {
        if (plugPath.empty()) return L"no plugin chosen";
        std::wstring who = plugName.empty() ? std::wstring(L"(unnamed)") : plugName;
        return FormatW(L"%s (%s), %zu settings stored", who.c_str(),
                       plugFormat == 1 ? L"VST3" : L"CLAP", plugParams.size());
    }
    if (typeId == BT_OUT_RECORDER)
    {
        int f = (recFormat >= 0 && recFormat < kRecFormatCount) ? recFormat : 0;
        std::wstring where = folder.empty() ? std::wstring(L"Music folder") : folder;
        std::wstring tmpl = nameTemplate.empty() ? std::wstring(L"{session} {date} {time}")
                                                 : nameTemplate;
        std::wstring bitrate = RecFormatUsesBitrate((RecFormat)f)
                             ? FormatW(L" %d kbps", bitrateKbps) : std::wstring();
        std::wstring extra;
        if (skipSilence && silenceHoldSec == kHoldAlways)
            extra += FormatW(L", waiting for sound above %d dB to start", silenceDb);
        else if (skipSilence)
            extra += FormatW(L", skipping silence below %d dB", silenceDb);
        if (splitMode == 1)
            extra += FormatW(L", new file every %d minutes", splitMinutes);
        else if (splitMode == 2)
            extra += FormatW(L", new file every %d MB", splitMb);
        // Only when it has been chosen. The ordinary case is the session's own
        // rate and the format's own depth, and saying so every time would bury
        // the settings that were actually touched.
        if (recRateHz > 0) extra += FormatW(L", at %d Hz", recRateHz);
        if (recBits == 32) extra += L", 32 bit float";
        else if (recBits > 0) extra += FormatW(L", %d bit", recBits);
        // Said first, because it decides how many files there are at all.
        std::wstring what;
        if (recMode == RM_TRACKS) what = L"a file per input block, ";
        else if (recMode == RM_BOTH) what = L"the mix and a file per input block, ";
        return FormatW(L"%s%s%s, %s, \"%s\", volume %d%%%s", what.c_str(),
                       RecFormatShortName((RecFormat)f), bitrate.c_str(),
                       where.c_str(), tmpl.c_str(), volumePct, extra.c_str());
    }
    return L"";
}

// ---------------------------------------------------------------------------

size_t PasteIndexFor(size_t blockCount, int selected, bool above)
{
    if (selected < 0 || (size_t)selected >= blockCount) return blockCount;
    return above ? (size_t)selected : (size_t)selected + 1;
}

int ClearClashingHotkeys(const Session& s, Block& b)
{
    // A key is taken if any block in the session answers to it, whether as
    // its own on-and-off key or as one of its action keys - the two share one
    // keyboard, so a clash between them is still a clash.
    auto taken = [&s](const Hotkey& h) {
        if (!h.Set()) return false;
        for (const Block& other : s.blocks)
        {
            if (other.hotkey.Set() && other.hotkey.mods == h.mods &&
                other.hotkey.vk == h.vk)
                return true;
            for (int a = 0; a < BA_COUNT; ++a)
                if (other.actionKey[a].Set() &&
                    other.actionKey[a].mods == h.mods &&
                    other.actionKey[a].vk == h.vk)
                    return true;
        }
        return false;
    };

    int cleared = 0;
    if (taken(b.hotkey)) { b.hotkey = Hotkey(); ++cleared; }
    for (int a = 0; a < BA_COUNT; ++a)
        if (taken(b.actionKey[a])) { b.actionKey[a] = Hotkey(); ++cleared; }
    return cleared;
}

// ---------------------------------------------------------------------------

static Hotkey ReadHotkey(const Json& owner)
{
    Hotkey h;
    const Json* j = owner.Find(L"hotkey");
    if (!j || !j->IsObject()) return h;
    h.enabled = j->AsBool(L"enabled", false);
    h.mods = j->AsInt(L"mods", 0);
    h.vk = j->AsInt(L"vk", 0);
    // Absent means global: that is what every key written before the choice
    // existed was, and it is how those documents have to keep reading.
    h.global = j->AsBool(L"global", true);
    return h;
}

static void WriteHotkey(Json& owner, const Hotkey& h)
{
    if (!h.Set()) return;      // nothing set is the default; do not write it
    Json j = Json::Obj();
    j.Set(L"enabled", Json::Bool(h.enabled));
    j.Set(L"mods", Json::Num(h.mods));
    j.Set(L"vk", Json::Num(h.vk));
    j.Set(L"global", Json::Bool(h.global));
    owner.Set(L"hotkey", std::move(j));
}

// The keys worth naming ourselves. GetKeyNameTextW knows most of these, but it
// answers in the keyboard layout's own words and abbreviates - "Left" for the
// arrow, which read aloud is ambiguous with the Left in Left Shift. These are
// the names that get read out, so they are spelled out.
struct NamedKey { int vk; const wchar_t* name; };
static const NamedKey kNamedKeys[] = {
    { VK_LEFT,      L"Left Arrow" },
    { VK_RIGHT,     L"Right Arrow" },
    { VK_UP,        L"Up Arrow" },
    { VK_DOWN,      L"Down Arrow" },
    { VK_HOME,      L"Home" },
    { VK_END,       L"End" },
    { VK_PRIOR,     L"Page Up" },
    { VK_NEXT,      L"Page Down" },
    { VK_INSERT,    L"Insert" },
    { VK_DELETE,    L"Delete" },
    { VK_BACK,      L"Backspace" },
    { VK_RETURN,    L"Enter" },
    { VK_SPACE,     L"Space" },
    { VK_TAB,       L"Tab" },
    { VK_ESCAPE,    L"Escape" },
    { VK_PAUSE,     L"Pause" },
    { VK_SNAPSHOT,  L"Print Screen" },
    { VK_SCROLL,    L"Scroll Lock" },
    { VK_CAPITAL,   L"Caps Lock" },
    { VK_NUMLOCK,   L"Num Lock" },
    { VK_APPS,      L"Applications" },
    { VK_MULTIPLY,  L"Num Star" },
    { VK_ADD,       L"Num Plus" },
    { VK_SUBTRACT,  L"Num Minus" },
    { VK_DECIMAL,   L"Num Decimal" },
    { VK_DIVIDE,    L"Num Slash" },
    { VK_OEM_1,      L"Semicolon" },
    { VK_OEM_PLUS,   L"Equals" },
    { VK_OEM_COMMA,  L"Comma" },
    { VK_OEM_MINUS,  L"Minus" },
    { VK_OEM_PERIOD, L"Full Stop" },
    { VK_OEM_2,      L"Slash" },
    { VK_OEM_3,      L"Backtick" },
    { VK_OEM_4,      L"Left Bracket" },
    { VK_OEM_5,      L"Backslash" },
    { VK_OEM_6,      L"Right Bracket" },
    { VK_OEM_7,      L"Quote" },
    { VK_BROWSER_BACK,       L"Browser Back" },
    { VK_BROWSER_FORWARD,    L"Browser Forward" },
    { VK_BROWSER_REFRESH,    L"Browser Refresh" },
    { VK_BROWSER_HOME,       L"Browser Home" },
    { VK_VOLUME_MUTE,        L"Volume Mute" },
    { VK_VOLUME_DOWN,        L"Volume Down" },
    { VK_VOLUME_UP,          L"Volume Up" },
    { VK_MEDIA_NEXT_TRACK,   L"Media Next" },
    { VK_MEDIA_PREV_TRACK,   L"Media Previous" },
    { VK_MEDIA_STOP,         L"Media Stop" },
    { VK_MEDIA_PLAY_PAUSE,   L"Media Play" },
};

std::wstring KeyName(int vk)
{
    if (vk == 0) return L"none";
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return FormatW(L"F%d", vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return FormatW(L"Num %d", vk - VK_NUMPAD0);

    for (const NamedKey& k : kNamedKeys)
        if (k.vk == vk) return k.name;

    // Anything else: ask Windows, which knows the layout. The scan code has to
    // be built by hand because there is no message here to take one from.
    const UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc)
    {
        wchar_t buf[64] = { 0 };
        if (GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0) return buf;
    }
    return FormatW(L"key %d", vk);
}

// Everything that is not a letter or a digit comes out, and what is left is
// lower case. That is what makes "Page Up", "page up", "PageUp" and "page-up"
// one name rather than four.
static std::wstring FoldKeyName(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s)
    {
        if (c >= L'A' && c <= L'Z') out += (wchar_t)(c - L'A' + L'a');
        else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) out += c;
    }
    return out;
}

// The spellings accepted for the keys that have more than one. The first name
// in kNamedKeys is what gets written back; these are what will also be read.
struct KeyAlias { const wchar_t* name; int vk; };
static const KeyAlias kKeyAliases[] = {
    { L"left",        VK_LEFT },
    { L"right",       VK_RIGHT },
    { L"up",          VK_UP },
    { L"down",        VK_DOWN },
    { L"pgup",        VK_PRIOR },
    { L"prior",       VK_PRIOR },
    { L"pgdn",        VK_NEXT },
    { L"pgdown",      VK_NEXT },
    { L"next",        VK_NEXT },
    { L"esc",         VK_ESCAPE },
    { L"del",         VK_DELETE },
    { L"ins",         VK_INSERT },
    { L"back",        VK_BACK },
    { L"bksp",        VK_BACK },
    { L"return",      VK_RETURN },
    { L"spacebar",    VK_SPACE },
    { L"caps",        VK_CAPITAL },
    { L"apps",        VK_APPS },
    { L"menu",        VK_APPS },
    { L"contextmenu", VK_APPS },
    { L"prtsc",       VK_SNAPSHOT },
    { L"printscr",    VK_SNAPSHOT },
    { L"break",       VK_PAUSE },
    { L"numstar",     VK_MULTIPLY },
    { L"numasterisk", VK_MULTIPLY },
    { L"multiply",    VK_MULTIPLY },
    { L"numplus",     VK_ADD },
    { L"add",         VK_ADD },
    { L"numminus",    VK_SUBTRACT },
    { L"subtract",    VK_SUBTRACT },
    { L"numslash",    VK_DIVIDE },
    { L"divide",      VK_DIVIDE },
    { L"decimal",     VK_DECIMAL },
    { L"period",      VK_OEM_PERIOD },
    { L"dot",         VK_OEM_PERIOD },
    { L"dash",        VK_OEM_MINUS },
    { L"grave",       VK_OEM_3 },
    { L"apostrophe",  VK_OEM_7 },
    { L"volup",       VK_VOLUME_UP },
    { L"voldown",     VK_VOLUME_DOWN },
    { L"volmute",     VK_VOLUME_MUTE },
    { L"mute",        VK_VOLUME_MUTE },
    { L"play",        VK_MEDIA_PLAY_PAUSE },
    { L"playpause",   VK_MEDIA_PLAY_PAUSE },
    { L"medianexttrack", VK_MEDIA_NEXT_TRACK },
    { L"mediaprev",      VK_MEDIA_PREV_TRACK },
    { L"mediaprevioustrack", VK_MEDIA_PREV_TRACK },
};

int KeyFromName(const std::wstring& name)
{
    const std::wstring k = FoldKeyName(name);
    if (k.empty()) return 0;

    // A single letter or digit is itself.
    if (k.size() == 1)
    {
        const wchar_t c = k[0];
        if (c >= L'a' && c <= L'z') return (int)(c - L'a' + L'A');
        if (c >= L'0' && c <= L'9') return (int)c;
    }

    // F1 to F24.
    if (k.size() >= 2 && k[0] == L'f')
    {
        bool digits = true;
        int n = 0;
        for (size_t i = 1; i < k.size(); ++i)
        {
            if (k[i] < L'0' || k[i] > L'9') { digits = false; break; }
            n = n * 10 + (k[i] - L'0');
        }
        if (digits && n >= 1 && n <= 24) return VK_F1 + n - 1;
    }

    // "num7" and "numpad7", but not "num plus", which the alias table has.
    if (k.size() == 4 && k.compare(0, 3, L"num") == 0 &&
        k[3] >= L'0' && k[3] <= L'9')
        return VK_NUMPAD0 + (k[3] - L'0');
    if (k.size() == 7 && k.compare(0, 6, L"numpad") == 0 &&
        k[6] >= L'0' && k[6] <= L'9')
        return VK_NUMPAD0 + (k[6] - L'0');

    // Whatever KeyName would have written, so a filled field always reads back.
    for (const NamedKey& e : kNamedKeys)
        if (FoldKeyName(e.name) == k) return e.vk;

    for (const KeyAlias& a : kKeyAliases)
        if (FoldKeyName(a.name) == k) return a.vk;

    return 0;
}

std::wstring HotkeyText(const Hotkey& h)
{
    if (!h.enabled || h.vk == 0) return L"none";

    std::wstring out;
    if (h.mods & HK_CTRL)  out += L"Ctrl+";
    if (h.mods & HK_ALT)   out += L"Alt+";
    if (h.mods & HK_SHIFT) out += L"Shift+";
    if (h.mods & HK_WIN)   out += L"Windows+";
    out += KeyName(h.vk);
    return out;
}

const wchar_t* BlockActionName(int action)
{
    switch (action)
    {
    case BA_REWIND:  return L"Rewind";
    case BA_FORWARD: return L"Fast Forward";
    case BA_LIVE:    return L"Jump to Live";
    case BA_SAVE:    return L"Save Buffer";
    default:         return L"";
    }
}

const wchar_t* BlockActionDescription(int action)
{
    switch (action)
    {
    case BA_REWIND:
        return L"go back through what the buffer has kept, one step at a time.";
    case BA_FORWARD:
        return L"come forward again towards live, one step at a time.";
    case BA_LIVE:
        return L"jump straight back to live, however far back you are.";
    case BA_SAVE:
        return L"write everything the buffer is holding to a file, without "
               L"interrupting it.";
    default:
        return L"";
    }
}

std::vector<int> BlockActionsFor(const std::wstring& typeId)
{
    if (typeId == BT_FX_BUFFER)
        return { BA_REWIND, BA_FORWARD, BA_LIVE, BA_SAVE };
    return {};
}

std::wstring HotkeyScopeText(const Hotkey& h)
{
    if (!h.enabled || h.vk == 0) return L"none";
    return h.global ? HotkeyText(h) : HotkeyText(h) + L" (this window)";
}

// ---------------------------------------------------------------------------

static const wchar_t* kDayNames[7] = { L"Sun", L"Mon", L"Tue", L"Wed",
                                       L"Thu", L"Fri", L"Sat" };

std::wstring ScheduleWhenText(const ScheduleEvent& e)
{
    std::wstring at = FormatW(L"%02d:%02d", e.hour, e.minute);

    if (e.repeat == SR_DAILY) return L"Every day at " + at;
    if (e.repeat == SR_WEEKDAYS)
    {
        std::wstring days;
        for (int d = 0; d < 7; ++d)
            if (e.weekdays & (1 << d))
            {
                if (!days.empty()) days += L", ";
                days += kDayNames[d];
            }
        if (days.empty()) return L"On no days, at " + at;
        return days + L" at " + at;
    }
    return FormatW(L"On %04d-%02d-%02d at %s", e.year, e.month, e.day, at.c_str());
}

std::wstring ScheduleStopText(const ScheduleEvent& e)
{
    if (e.stopMode == SS_AT_TIME)
        return FormatW(L"Stops at %02d:%02d", e.stopHour, e.stopMinute);
    if (e.stopMode == SS_AFTER)
        return FormatW(L"Stops after %d minutes", e.afterMinutes);
    return L"Runs until stopped";
}

ScheduleEvent* SessionStore::EventById(int id)
{
    for (ScheduleEvent& e : events) if (e.id == id) return &e;
    return nullptr;
}

int SessionStore::NewEventId()
{
    int max = 0;
    for (const ScheduleEvent& e : events) if (e.id > max) max = e.id;
    return max + 1;
}

// ---------------------------------------------------------------------------
// Persistence: sessions.json, beside the executable. See ConfigDir.
// ---------------------------------------------------------------------------

std::wstring SessionStore::Path() const
{
    std::wstring dir = ConfigDir();
    if (dir.empty()) return L"";
    return dir + L"\\sessions.json";
}

// Where earlier versions kept it. Read from once, if there is nothing beside
// the executable yet, so that turning portable does not look like every
// session having been deleted. It is never written to again, and it is left
// alone rather than removed: a copy of the document that is one save out of
// date is worth more than the tidiness of deleting it.
std::wstring SessionStore::LegacyPath() const
{
    std::wstring dir = AppDataDir();
    if (dir.empty()) return L"";
    return dir + L"\\sessions.json";
}

void SessionStore::SortSessions()
{
    // CompareStringW rather than a plain wcsicmp: these are names people wrote,
    // so "Ausgabe" has to sort where a reader expects it to and not where its
    // code points happen to fall.
    std::stable_sort(sessions.begin(), sessions.end(),
        [](const Session& a, const Session& b)
        {
            int r = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                                   a.name.c_str(), -1, b.name.c_str(), -1);
            return r == CSTR_LESS_THAN;
        });
}

int SessionStore::NewId()
{
    int max = 0;
    for (const Session& s : sessions) if (s.id > max) max = s.id;
    return max + 1;
}

Session* SessionStore::ById(int id)
{
    for (Session& s : sessions) if (s.id == id) return &s;
    return nullptr;
}

// ---------------------------------------------------------------------------
// One session, to and from JSON
//
// Lifted out of SessionStore::Load and ::Save, which had both inline, so
// that exporting and importing one session is the same code the whole
// document goes through rather than a second description of a session that
// could fall behind it.
// ---------------------------------------------------------------------------

// False for an object that is not a session at all. A block of a type this
// build does not know is skipped, as it always was.
static bool ReadSession(const Json& js, Session& s)
{
    if (!js.IsObject()) return false;

    s.id = js.AsInt(L"id", 0);
    s.name = js.AsString(L"name");
    s.autoRun = js.AsBool(L"autoRun", false);
    s.hotkey = ReadHotkey(js);

    const Json* blocks = js.Find(L"blocks");
    if (blocks && blocks->IsArray())
    {
        for (const Json& jb : blocks->Items())
        {
            if (!jb.IsObject()) continue;
            Block b;
            b.typeId      = jb.AsString(L"type");
            if (!FindBlockType(b.typeId)) continue;   // unknown block type
            b.deviceId    = jb.AsString(L"deviceId");
            b.deviceName  = jb.AsString(L"deviceName");
            b.appSource   = jb.AsInt(L"appSource", AS_NAMED);
            b.processName = jb.AsString(L"process");
            b.appNames.clear();
            if (const Json* names = jb.Find(L"appNames"))
                if (names->IsArray())
                    for (const Json& e : names->Items())
                    {
                        std::wstring n = e.AsText();
                        if (!n.empty()) b.appNames.push_back(n);
                    }
            b.appWaitForStart = jb.AsBool(L"appWait", false);
            b.url          = jb.AsString(L"url");
            b.bufferMs     = jb.AsInt(L"bufferMs", 1000);
            b.reconnect    = jb.AsBool(L"reconnect", true);
            b.retrySeconds = jb.AsInt(L"retrySeconds", 5);
            const Json* ex = jb.Find(L"excludeApps");
            if (ex && ex->IsArray())
                for (const Json& e : ex->Items())
                {
                    std::wstring name = e.AsText();
                    if (!name.empty()) b.excludeApps.push_back(name);
                }
            b.volumePct   = jb.AsInt(L"volume", 100);
            b.mixPct      = jb.AsInt(L"mix", 100);
            b.suppressDb  = jb.AsInt(L"suppressDb", 20);
            b.bassDb      = jb.AsInt(L"bassDb", 0);
            b.trebleDb    = jb.AsInt(L"trebleDb", 0);
            const Json* eq = jb.Find(L"eq");
            if (eq && eq->IsArray())
            {
                const std::vector<Json>& bands = eq->Items();
                for (int i = 0; i < kEqBands && (size_t)i < bands.size(); ++i)
                    b.eqDb[i] = (int)bands[(size_t)i].AsNumber(0.0);
            }
            b.compThreshDb   = jb.AsInt(L"compThreshDb", -20);
            b.compRatio      = jb.AsInt(L"compRatio", 4);
            b.compAttackMs   = jb.AsInt(L"compAttackMs", 10);
            b.compReleaseMs  = jb.AsInt(L"compReleaseMs", 100);
            b.compMakeupDb   = jb.AsInt(L"compMakeupDb", 0);
            b.compAutoMakeup = jb.AsBool(L"compAutoMakeup", true);
            // Nothing written before this block existed has these, and
            // zero is the setting where the block does nothing at all,
            // which is the right thing for a session that never had one.
            // Both net blocks, read together: they are two ends of
            // one link and they store the same answers.
            b.netProtocol   = jb.AsInt(L"netProtocol", NP_FASTROUTE);
            b.netMode       = jb.AsInt(L"netMode", NM_MULTICAST);
            b.netAddress    = jb.AsString(L"netAddress");
            b.netPort       = jb.AsInt(L"netPort", 0);
            b.netStreamName = jb.AsString(L"netStreamName");
            b.netCodec      = jb.AsInt(L"netCodec", 0);
            b.netPacketMs   = jb.AsInt(L"netPacketMs", 5);
            b.netOpusKbps   = jb.AsInt(L"netOpusKbps", 128);
            b.netOpusLossPct    = jb.AsInt(L"netOpusLossPct", 5);
            b.netOpusComplexity = jb.AsInt(L"netOpusComplexity", 10);
            b.netOpusSignal     = jb.AsInt(L"netOpusSignal", NOS_AUTO);
            b.netOpusRateMode   = jb.AsInt(L"netOpusRateMode", NOR_VBR);
            b.netOpusDtx        = jb.AsBool(L"netOpusDtx", false);
            b.netTtl        = jb.AsInt(L"netTtl", 1);
            b.netBufferMs   = jb.AsInt(L"netBufferMs", 60);
            // AsNumberAt rather than AsInt: a stream id is a full 32 bit
            // hash, and one above two billion read as an int comes back
            // negative, which would lose the sender somebody chose.
            b.netSourceId   = (unsigned int)jb.AsNumberAt(L"netSourceId", 0.0);
            b.asioInLeft    = jb.AsInt(L"asioInLeft", 0);
            b.asioInRight   = jb.AsInt(L"asioInRight", 1);
            b.asioOutLeft   = jb.AsInt(L"asioOutLeft", 0);
            b.asioOutRight  = jb.AsInt(L"asioOutRight", 1);
            b.pitchSemitones = jb.AsInt(L"pitchSemitones", 0);
            b.pitchCents     = jb.AsInt(L"pitchCents", 0);
            b.pitchQuality   = jb.AsInt(L"pitchQuality", PQ_BEST);
            b.limCeilingDb   = jb.AsInt(L"limCeilingDb", -1);
            b.limReleaseMs   = jb.AsInt(L"limReleaseMs", 50);
            b.limLookaheadMs = jb.AsInt(L"limLookaheadMs", 5);
            b.gateThreshDb  = jb.AsInt(L"gateThreshDb", -40);
            b.gateAttackMs  = jb.AsInt(L"gateAttackMs", 5);
            b.gateHoldMs    = jb.AsInt(L"gateHoldMs", 200);
            b.gateReleaseMs = jb.AsInt(L"gateReleaseMs", 150);
            b.gateRangeDb   = jb.AsInt(L"gateRangeDb", 60);
            // Nothing written before 0.3 has this, and those sessions were
            // built and listened to with no lookahead and no latency here.
            // A new gate block starts at the struct's own default instead,
            // which is where the control is actually worth having.
            b.gateLookaheadMs = jb.AsInt(L"gateLookaheadMs", 0);
            b.normTargetDb    = jb.AsInt(L"normTargetDb", -3);
            b.normLookaheadMs = jb.AsInt(L"normLookaheadMs", 50);
            b.normMaxGainDb   = jb.AsInt(L"normMaxGainDb", 20);
            b.normReleaseMs   = jb.AsInt(L"normReleaseMs", 1000);
            b.clipThreshDb    = jb.AsInt(L"clipThreshDb", 0);
            b.clipIntervalS   = jb.AsInt(L"clipIntervalS", 2);
            b.bufSeconds      = jb.AsInt(L"bufSeconds", 60);
            b.bufStepSec      = jb.AsInt(L"bufStepSec", 10);

            const Json* acts = jb.Find(L"actions");
            if (acts && acts->IsArray())
                for (const Json& ae : acts->Items())
                {
                    if (!ae.IsObject()) continue;
                    const int id = ae.AsInt(L"id", -1);
                    if (id < 0 || id >= BA_COUNT) continue;
                    Hotkey h;
                    h.enabled = ae.AsBool(L"enabled", false);
                    h.mods    = ae.AsInt(L"mods", 0);
                    h.vk      = ae.AsInt(L"vk", 0);
                    h.global  = ae.AsBool(L"global", true);
                    b.actionKey[id] = h;
                }
            b.label         = jb.AsString(L"label");
            b.plugFormat  = jb.AsInt(L"plugFormat", 0);
            b.plugPath    = jb.AsString(L"plugPath");
            b.plugUid     = jb.AsString(L"plugUid");
            b.plugName    = jb.AsString(L"plugName");
            const Json* pp = jb.Find(L"plugParams");
            if (pp && pp->IsArray())
                for (const Json& e : pp->Items())
                {
                    if (!e.IsObject()) continue;
                    b.plugParams.push_back({
                        (unsigned long long)e.AsNumberAt(L"id", 0.0),
                        e.AsNumberAt(L"v", 0.0) });
                }
            const Json* para = jb.Find(L"para");
            if (para && para->IsArray())
            {
                const std::vector<Json>& bands = para->Items();
                for (int i = 0; i < kParaBands && (size_t)i < bands.size(); ++i)
                {
                    const Json& pb = bands[(size_t)i];
                    if (!pb.IsObject()) continue;
                    b.paraType[i]   = pb.AsInt(L"type", PT_PEAK);
                    b.paraHz[i]     = pb.AsInt(L"hz", b.paraHz[i]);
                    b.paraGainDb[i] = pb.AsInt(L"db", 0);
                    b.paraQ[i]      = pb.AsInt(L"q", 10);
                }
            }
            b.widthPct     = jb.AsInt(L"width", 100);
            b.swapChannels = jb.AsBool(L"swapChannels", false);
            b.centreCancel = jb.AsBool(L"centreCancel", false);
            // Absent in anything written before 0.3, and the defaults are
            // the identity, so those documents keep sounding as they did.
            b.chanGainDb[0] = jb.AsInt(L"leftGainDb", 0);
            b.chanGainDb[1] = jb.AsInt(L"rightGainDb", 0);
            b.chanPan[0]    = jb.AsInt(L"leftPan", kChanPanMin);
            b.chanPan[1]    = jb.AsInt(L"rightPan", kChanPanMax);
            b.folder       = jb.AsString(L"folder");
            b.nameTemplate = jb.AsString(L"nameTemplate");
            b.recFormat    = jb.AsInt(L"format", 0);
            // "mp3Kbps" is what older documents call it, from when MP3
            // was the only format it applied to.
            b.bitrateKbps  = jb.AsInt(L"kbps", jb.AsInt(L"mp3Kbps", 192));
            b.skipSilence    = jb.AsBool(L"skipSilence", false);
            b.silenceDb      = jb.AsInt(L"silenceDb", -50);
            b.silenceHoldSec = jb.AsInt(L"silenceHold", 2);
            b.splitMode      = jb.AsInt(L"splitMode", 0);
            b.splitMinutes   = jb.AsInt(L"splitMinutes", 30);
            b.splitMb        = jb.AsInt(L"splitMb", 100);
            b.recMode        = jb.AsInt(L"recMode", RM_MIXED);
            if (b.recMode < RM_MIXED || b.recMode > RM_BOTH)
                b.recMode = RM_MIXED;
            b.recRateHz      = jb.AsInt(L"recRate", 0);
            b.recBits        = jb.AsInt(L"recBits", 0);
            if (b.recRateHz < 0) b.recRateHz = 0;
            if (b.recBits != 16 && b.recBits != 24 && b.recBits != 32)
                b.recBits = 0;
            b.enabled     = jb.AsBool(L"enabled", true);
            b.hotkey      = ReadHotkey(jb);
            s.blocks.push_back(b);
        }
    }
    return true;
}

static Json WriteSession(const Session& s)
{
    Json js = Json::Obj();
    js.Set(L"id", Json::Num(s.id));
    js.Set(L"name", Json::Str(s.name));
    js.Set(L"autoRun", Json::Bool(s.autoRun));
    WriteHotkey(js, s.hotkey);

    Json blocks = Json::Arr();
    for (const Block& b : s.blocks)
    {
        Json jb = Json::Obj();
        jb.Set(L"type", Json::Str(b.typeId));
        if (b.typeId == BT_INPUT_APP)
        {
            jb.Set(L"appSource", Json::Num(b.appSource));
            // Kept even for a foreground block: switching the mode back
            // should find the application that was chosen before.
            jb.Set(L"process", Json::Str(b.processName));
            if (b.appNames.size() > 1)
            {
                Json names = Json::Arr();
                for (const std::wstring& n : b.appNames)
                    names.Push(Json::Str(n));
                jb.Set(L"appNames", std::move(names));
            }
            if (b.appWaitForStart) jb.Set(L"appWait", Json::Bool(true));
        }
        if (b.typeId == BT_INPUT_STREAM)
        {
            jb.Set(L"url", Json::Str(b.url));
            jb.Set(L"bufferMs", Json::Num(b.bufferMs));
            jb.Set(L"reconnect", Json::Bool(b.reconnect));
            jb.Set(L"retrySeconds", Json::Num(b.retrySeconds));
        }
        if (b.typeId == BT_INPUT_NET || b.typeId == BT_OUT_NET)
        {
            jb.Set(L"netProtocol", Json::Num(b.netProtocol));
            jb.Set(L"netAddress", Json::Str(b.netAddress));
            jb.Set(L"netPort", Json::Num(b.netPort));
            jb.Set(L"netStreamName", Json::Str(b.netStreamName));
        }
        if (b.typeId == BT_INPUT_NET)
        {
            jb.Set(L"netBufferMs", Json::Num(b.netBufferMs));
            jb.Set(L"netSourceId", Json::Num((double)b.netSourceId));
        }
        if (b.typeId == BT_OUT_NET)
        {
            jb.Set(L"netMode", Json::Num(b.netMode));
            jb.Set(L"netCodec", Json::Num(b.netCodec));
            jb.Set(L"netPacketMs", Json::Num(b.netPacketMs));
            jb.Set(L"netOpusKbps", Json::Num(b.netOpusKbps));
            jb.Set(L"netOpusLossPct", Json::Num(b.netOpusLossPct));
            jb.Set(L"netOpusComplexity", Json::Num(b.netOpusComplexity));
            jb.Set(L"netOpusSignal", Json::Num(b.netOpusSignal));
            jb.Set(L"netOpusRateMode", Json::Num(b.netOpusRateMode));
            jb.Set(L"netOpusDtx", Json::Bool(b.netOpusDtx));
            jb.Set(L"netTtl", Json::Num(b.netTtl));
        }
        if (b.typeId == BT_INPUT_DEVICE || b.typeId == BT_OUTPUT_DEVICE ||
            b.typeId == BT_INPUT_SYSTEM || b.typeId == BT_INPUT_ASIO ||
            b.typeId == BT_OUT_ASIO)
        {
            jb.Set(L"deviceId", Json::Str(b.deviceId));
            jb.Set(L"deviceName", Json::Str(b.deviceName));
        }
        if (b.typeId == BT_INPUT_ASIO)
        {
            jb.Set(L"asioInLeft", Json::Num(b.asioInLeft));
            jb.Set(L"asioInRight", Json::Num(b.asioInRight));
        }
        if (b.typeId == BT_OUT_ASIO)
        {
            jb.Set(L"asioOutLeft", Json::Num(b.asioOutLeft));
            jb.Set(L"asioOutRight", Json::Num(b.asioOutRight));
        }
        if (b.typeId == BT_INPUT_SYSTEM && !b.excludeApps.empty())
        {
            Json ex = Json::Arr();
            for (const std::wstring& e : b.excludeApps) ex.Push(Json::Str(e));
            jb.Set(L"excludeApps", std::move(ex));
        }
        if (b.typeId == BT_FX_SPEEX || b.typeId == BT_FX_RNNOISE)
        {
            jb.Set(L"mix", Json::Num(b.mixPct));
            if (b.typeId == BT_FX_SPEEX) jb.Set(L"suppressDb", Json::Num(b.suppressDb));
        }
        if (b.typeId == BT_FX_TONE)
        {
            jb.Set(L"bassDb", Json::Num(b.bassDb));
            jb.Set(L"trebleDb", Json::Num(b.trebleDb));
        }
        if (b.typeId == BT_FX_EQ10)
        {
            Json eq = Json::Arr();
            for (int i = 0; i < kEqBands; ++i) eq.Push(Json::Num(b.eqDb[i]));
            jb.Set(L"eq", std::move(eq));
        }
        if (b.typeId == BT_FX_PARA)
        {
            Json para = Json::Arr();
            for (int i = 0; i < kParaBands; ++i)
            {
                Json pb = Json::Obj();
                pb.Set(L"type", Json::Num(b.paraType[i]));
                pb.Set(L"hz", Json::Num(b.paraHz[i]));
                pb.Set(L"db", Json::Num(b.paraGainDb[i]));
                pb.Set(L"q", Json::Num(b.paraQ[i]));
                para.Push(std::move(pb));
            }
            jb.Set(L"para", std::move(para));
        }
        if (b.typeId == BT_FX_CHANNELS)
        {
            jb.Set(L"width", Json::Num(b.widthPct));
            jb.Set(L"swapChannels", Json::Bool(b.swapChannels));
            jb.Set(L"centreCancel", Json::Bool(b.centreCancel));
            jb.Set(L"leftGainDb",  Json::Num(b.chanGainDb[0]));
            jb.Set(L"rightGainDb", Json::Num(b.chanGainDb[1]));
            jb.Set(L"leftPan",     Json::Num(b.chanPan[0]));
            jb.Set(L"rightPan",    Json::Num(b.chanPan[1]));
        }
        if (b.typeId == BT_FX_COMP)
        {
            jb.Set(L"compThreshDb", Json::Num(b.compThreshDb));
            jb.Set(L"compRatio", Json::Num(b.compRatio));
            jb.Set(L"compAttackMs", Json::Num(b.compAttackMs));
            jb.Set(L"compReleaseMs", Json::Num(b.compReleaseMs));
            jb.Set(L"compAutoMakeup", Json::Bool(b.compAutoMakeup));
            if (!b.compAutoMakeup) jb.Set(L"compMakeupDb", Json::Num(b.compMakeupDb));
        }
        if (b.typeId == BT_FX_GATE)
        {
            jb.Set(L"gateThreshDb", Json::Num(b.gateThreshDb));
            jb.Set(L"gateAttackMs", Json::Num(b.gateAttackMs));
            jb.Set(L"gateHoldMs", Json::Num(b.gateHoldMs));
            jb.Set(L"gateReleaseMs", Json::Num(b.gateReleaseMs));
            jb.Set(L"gateRangeDb", Json::Num(b.gateRangeDb));
            jb.Set(L"gateLookaheadMs", Json::Num(b.gateLookaheadMs));
        }
        if (b.typeId == BT_FX_PITCH)
        {
            jb.Set(L"pitchSemitones", Json::Num(b.pitchSemitones));
            jb.Set(L"pitchCents", Json::Num(b.pitchCents));
            jb.Set(L"pitchQuality", Json::Num(b.pitchQuality));
        }
        if (b.typeId == BT_FX_LIMIT)
        {
            jb.Set(L"limCeilingDb", Json::Num(b.limCeilingDb));
            jb.Set(L"limReleaseMs", Json::Num(b.limReleaseMs));
            jb.Set(L"limLookaheadMs", Json::Num(b.limLookaheadMs));
        }
        if (b.typeId == BT_FX_NORM)
        {
            jb.Set(L"normTargetDb", Json::Num(b.normTargetDb));
            jb.Set(L"normLookaheadMs", Json::Num(b.normLookaheadMs));
            jb.Set(L"normMaxGainDb", Json::Num(b.normMaxGainDb));
            jb.Set(L"normReleaseMs", Json::Num(b.normReleaseMs));
        }
        if (b.typeId == BT_FX_CLIPMON)
        {
            jb.Set(L"clipThreshDb", Json::Num(b.clipThreshDb));
            jb.Set(L"clipIntervalS", Json::Num(b.clipIntervalS));
        }
        if (b.typeId == BT_FX_BUFFER)
        {
            jb.Set(L"bufSeconds", Json::Num(b.bufSeconds));
            jb.Set(L"bufStepSec", Json::Num(b.bufStepSec));
        }

        // Only the keys that are actually set, so a block with no action
        // hotkeys writes nothing rather than four empty objects.
        {
            Json acts = Json::Arr();
            for (int a = 0; a < BA_COUNT; ++a)
            {
                if (!b.actionKey[a].Set()) continue;
                Json ae = Json::Obj();
                ae.Set(L"id", Json::Num(a));
                ae.Set(L"enabled", Json::Bool(b.actionKey[a].enabled));
                ae.Set(L"mods", Json::Num(b.actionKey[a].mods));
                ae.Set(L"vk", Json::Num(b.actionKey[a].vk));
                ae.Set(L"global", Json::Bool(b.actionKey[a].global));
                acts.Push(std::move(ae));
            }
            if (!acts.Items().empty()) jb.Set(L"actions", std::move(acts));
        }

        if (b.typeId == BT_FX_PLUGIN)
        {
            jb.Set(L"plugFormat", Json::Num(b.plugFormat));
            jb.Set(L"plugPath", Json::Str(b.plugPath));
            jb.Set(L"plugUid", Json::Str(b.plugUid));
            jb.Set(L"plugName", Json::Str(b.plugName));
            Json ps = Json::Arr();
            for (const auto& kv : b.plugParams)
            {
                Json one = Json::Obj();
                one.Set(L"id", Json::Num((double)kv.first));
                one.Set(L"v", Json::Num(kv.second));
                ps.Push(std::move(one));
            }
            jb.Set(L"plugParams", std::move(ps));
        }
        if (b.typeId == BT_OUT_RECORDER)
        {
            jb.Set(L"folder", Json::Str(b.folder));
            jb.Set(L"nameTemplate", Json::Str(b.nameTemplate));
            jb.Set(L"format", Json::Num(b.recFormat));
            if (RecFormatUsesBitrate((RecFormat)b.recFormat))
                jb.Set(L"kbps", Json::Num(b.bitrateKbps));
            jb.Set(L"skipSilence", Json::Bool(b.skipSilence));
            if (b.skipSilence)
            {
                jb.Set(L"silenceDb", Json::Num(b.silenceDb));
                jb.Set(L"silenceHold", Json::Num(b.silenceHoldSec));
            }
            jb.Set(L"splitMode", Json::Num(b.splitMode));
            if (b.splitMode == 1) jb.Set(L"splitMinutes", Json::Num(b.splitMinutes));
            if (b.splitMode == 2) jb.Set(L"splitMb", Json::Num(b.splitMb));
            // Only when it is not the mix alone, so a document a older
            // FastRoute reads is unchanged until the feature is used - and
            // that older one then writes the mix, which is what it did.
            if (b.recMode != RM_MIXED) jb.Set(L"recMode", Json::Num(b.recMode));
            // Same again: absent means the session's rate and the format's
            // own depth, which is what recording did before these existed.
            if (b.recRateHz > 0) jb.Set(L"recRate", Json::Num(b.recRateHz));
            if (b.recBits > 0) jb.Set(L"recBits", Json::Num(b.recBits));
        }
        if (!b.label.empty()) jb.Set(L"label", Json::Str(b.label));
        jb.Set(L"volume", Json::Num(b.volumePct));
        jb.Set(L"enabled", Json::Bool(b.enabled));
        WriteHotkey(jb, b.hotkey);
        blocks.Push(std::move(jb));
    }
    js.Set(L"blocks", std::move(blocks));
    return js;
}

// The marker that says this is one of ours rather than any other JSON file.
static const wchar_t* kExportKind = L"fastroute-session";

std::wstring SessionToText(const Session& s)
{
    Json doc = Json::Obj();
    doc.Set(L"kind", Json::Str(kExportKind));
    doc.Set(L"version", Json::Num(1));
    // The id is the store's, not the session's: two people's documents number
    // their sessions independently, so carrying one over would only collide.
    Session copy = s;
    copy.id = 0;
    doc.Set(L"session", WriteSession(copy));
    return doc.Serialise();
}

bool SessionFromText(const std::wstring& text, Session& out, std::wstring* err)
{
    Json doc;
    if (!Json::Parse(text, doc) || !doc.IsObject())
    {
        if (err) *err = L"That file is not a FastRoute session: it is not even "
                        L"the right kind of file.";
        return false;
    }
    if (doc.AsString(L"kind") != kExportKind)
    {
        if (err) *err = L"That file is not a FastRoute session. A session "
                        L"exported by FastRoute says so in its first line.";
        return false;
    }
    const Json* js = doc.Find(L"session");
    if (!js || !ReadSession(*js, out))
    {
        if (err) *err = L"That file says it is a FastRoute session, but the "
                        L"session in it could not be read.";
        return false;
    }
    out.id = 0;                       // the store gives it one
    if (out.name.empty()) out.name = L"Imported session";
    // A session whose every block was of a type this build does not know
    // would import as an empty one, which is worth saying rather than
    // leaving someone to wonder where the blocks went.
    if (out.blocks.empty())
    {
        if (err) *err = L"That session has no blocks this version of FastRoute "
                        L"knows about. It may have been exported by a newer one.";
        return false;
    }
    return true;
}

ImportReport RepointDevices(Session& s,
                            const std::vector<std::pair<std::wstring,
                                                        std::wstring>>& present)
{
    ImportReport r;
    for (Block& b : s.blocks)
    {
        // Only the blocks that name a Windows endpoint. An ASIO block names a
        // driver, which is a name already and needs no translating; a block
        // with no device named is asking for the system default, which is
        // what it will get here too.
        if (b.typeId != BT_INPUT_DEVICE && b.typeId != BT_OUTPUT_DEVICE &&
            b.typeId != BT_INPUT_SYSTEM)
            continue;
        if (b.deviceId.empty()) continue;

        bool here = false;
        for (const auto& d : present)
            if (d.first == b.deviceId) { here = true; break; }
        if (here) continue;             // the same machine, or the same device

        // Not here under that id, so look for the name it was saved under.
        // An endpoint id is a GUID of the machine it was made on; the name is
        // the only thing that travels.
        const std::wstring* found = nullptr;
        for (const auto& d : present)
            if (!b.deviceName.empty() && d.second == b.deviceName)
            {
                found = &d.first;
                break;
            }
        if (found)
        {
            b.deviceId = *found;
            ++r.rematched;
            continue;
        }
        r.unmatched.push_back(b.deviceName.empty()
                                  ? b.TypeName()
                                  : b.deviceName);
    }
    return r;
}

void SessionStore::Load()
{
    sessions.clear();
    events.clear();
    settings = AppSettings();
    std::wstring path = Path();
    if (path.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt, ccs=UTF-8") != 0 || !f)
    {
        // Nothing beside the executable. An earlier version kept it per user,
        // so that is read instead and written back out portably on the next
        // save - otherwise moving to a portable build would look exactly like
        // every session having been deleted.
        std::wstring legacy = LegacyPath();
        if (legacy.empty() || legacy == path) return;
        if (_wfopen_s(&f, legacy.c_str(), L"rt, ccs=UTF-8") != 0 || !f) return;
    }

    std::wstring text;
    wchar_t buf[4096];
    while (fgetws(buf, 4096, f)) text += buf;
    fclose(f);

    Json doc;
    if (!Json::Parse(text, doc) || !doc.IsObject()) return;

    const Json* set = doc.Find(L"settings");
    if (set && set->IsObject())
    {
        settings.showHide.enabled = set->AsBool(L"hotkeyEnabled", true);
        settings.showHide.mods    = set->AsInt(L"hotkeyMods", HK_CTRL | HK_ALT);
        settings.showHide.vk      = set->AsInt(L"hotkeyVk", 'R');

        settings.recFolder = set->AsString(L"recFolder");

        const Json* shut = set->Find(L"libraryCollapsed");
        if (shut && shut->IsArray())
            for (const Json& e : shut->Items())
            {
                std::wstring c = e.AsText();
                if (!c.empty()) settings.libraryCollapsed.push_back(c);
            }
        settings.updateCheck = set->AsInt(L"updateCheck", UC_NEVER);
        settings.updateSilent = set->AsBool(L"updateSilent", false);

        const Json* paths = set->Find(L"pluginPaths");
        if (paths && paths->IsArray())
            for (const Json& e : paths->Items())
            {
                std::wstring p = e.AsText();
                if (!p.empty()) settings.pluginPaths.push_back(p);
            }

        const Json* plugs = set->Find(L"plugins");
        if (plugs && plugs->IsArray())
            for (const Json& e : plugs->Items())
            {
                if (!e.IsObject()) continue;
                PluginEntry pe;
                pe.format = e.AsInt(L"format", 0);
                pe.path   = e.AsString(L"path");
                pe.uid    = e.AsString(L"uid");
                pe.name   = e.AsString(L"name");
                pe.vendor = e.AsString(L"vendor");
                if (!pe.path.empty() && !pe.name.empty()) settings.plugins.push_back(pe);
            }
    }

    const Json* arr = doc.Find(L"sessions");
    if (!arr || !arr->IsArray()) return;

    for (const Json& js : arr->Items())
    {
        Session s;
        if (ReadSession(js, s)) sessions.push_back(s);
    }
    SortSessions();

    const Json* evs = doc.Find(L"events");
    if (evs && evs->IsArray())
    {
        for (const Json& je : evs->Items())
        {
            if (!je.IsObject()) continue;
            ScheduleEvent e;
            e.id           = je.AsInt(L"id", 0);
            e.sessionId    = je.AsInt(L"session", 0);
            e.enabled      = je.AsBool(L"enabled", true);
            e.repeat       = je.AsInt(L"repeat", SR_ONCE);
            e.weekdays     = je.AsInt(L"weekdays", 0);
            e.year         = je.AsInt(L"year", 0);
            e.month        = je.AsInt(L"month", 0);
            e.day          = je.AsInt(L"day", 0);
            e.hour         = je.AsInt(L"hour", 0);
            e.minute       = je.AsInt(L"minute", 0);
            e.stopMode     = je.AsInt(L"stopMode", SS_NONE);
            e.stopHour     = je.AsInt(L"stopHour", 0);
            e.stopMinute   = je.AsInt(L"stopMinute", 0);
            e.afterMinutes = je.AsInt(L"afterMinutes", 60);
            e.singleUse    = je.AsBool(L"singleUse", false);
            e.lastFired    = (long long)je.AsNumberAt(L"lastFired", 0.0);
            events.push_back(e);
        }
    }

    for (size_t i = 0; i < events.size(); ++i)
    {
        if (events[i].id != 0)
        {
            bool dup = false;
            for (size_t j = 0; j < i; ++j) if (events[j].id == events[i].id) dup = true;
            if (!dup) continue;
        }
        events[i].id = NewEventId();
    }

    // Repair missing or duplicated ids.
    for (size_t i = 0; i < sessions.size(); ++i)
    {
        if (sessions[i].id != 0)
        {
            bool dup = false;
            for (size_t j = 0; j < i; ++j) if (sessions[j].id == sessions[i].id) dup = true;
            if (!dup) continue;
        }
        sessions[i].id = NewId();
    }
}

bool SessionStore::Save(std::wstring* err) const
{
    std::wstring path = Path();
    if (path.empty())
    {
        if (err) *err = L"Could not locate the application data folder.";
        return false;
    }

    Json doc = Json::Obj();
    doc.Set(L"version", Json::Num(1));

    Json set = Json::Obj();
    set.Set(L"hotkeyEnabled", Json::Bool(settings.showHide.enabled));
    set.Set(L"hotkeyMods", Json::Num(settings.showHide.mods));
    set.Set(L"hotkeyVk", Json::Num(settings.showHide.vk));

    set.Set(L"recFolder", Json::Str(settings.recFolder));
    set.Set(L"updateCheck", Json::Num(settings.updateCheck));
    set.Set(L"updateSilent", Json::Bool(settings.updateSilent));

    Json paths = Json::Arr();
    for (const std::wstring& p : settings.pluginPaths) paths.Push(Json::Str(p));
    set.Set(L"pluginPaths", std::move(paths));

    Json shut = Json::Arr();
    for (const std::wstring& c : settings.libraryCollapsed) shut.Push(Json::Str(c));
    set.Set(L"libraryCollapsed", std::move(shut));

    Json plugs = Json::Arr();
    for (const PluginEntry& pe : settings.plugins)
    {
        Json one = Json::Obj();
        one.Set(L"format", Json::Num(pe.format));
        one.Set(L"path", Json::Str(pe.path));
        one.Set(L"uid", Json::Str(pe.uid));
        one.Set(L"name", Json::Str(pe.name));
        one.Set(L"vendor", Json::Str(pe.vendor));
        plugs.Push(std::move(one));
    }
    set.Set(L"plugins", std::move(plugs));

    doc.Set(L"settings", std::move(set));

    Json arr = Json::Arr();
    for (const Session& s : sessions) arr.Push(WriteSession(s));
    doc.Set(L"sessions", std::move(arr));

    Json evs = Json::Arr();
    for (const ScheduleEvent& e : events)
    {
        Json je = Json::Obj();
        je.Set(L"id", Json::Num(e.id));
        je.Set(L"session", Json::Num(e.sessionId));
        je.Set(L"enabled", Json::Bool(e.enabled));
        je.Set(L"repeat", Json::Num(e.repeat));
        if (e.repeat == SR_WEEKDAYS) je.Set(L"weekdays", Json::Num(e.weekdays));
        if (e.repeat == SR_ONCE)
        {
            je.Set(L"year", Json::Num(e.year));
            je.Set(L"month", Json::Num(e.month));
            je.Set(L"day", Json::Num(e.day));
        }
        je.Set(L"hour", Json::Num(e.hour));
        je.Set(L"minute", Json::Num(e.minute));
        je.Set(L"stopMode", Json::Num(e.stopMode));
        if (e.stopMode == SS_AT_TIME)
        {
            je.Set(L"stopHour", Json::Num(e.stopHour));
            je.Set(L"stopMinute", Json::Num(e.stopMinute));
        }
        if (e.stopMode == SS_AFTER) je.Set(L"afterMinutes", Json::Num(e.afterMinutes));
        je.Set(L"singleUse", Json::Bool(e.singleUse));
        je.Set(L"lastFired", Json::Num((double)e.lastFired));
        evs.Push(std::move(je));
    }
    doc.Set(L"events", std::move(evs));

    // Write to a temporary file and swap it in, so a crash mid-write cannot
    // leave a truncated session document behind.
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wt, ccs=UTF-8") != 0 || !f)
    {
        if (err) *err = L"Could not write " + tmp;
        return false;
    }
    std::wstring text = doc.Serialise();
    fputws(text.c_str(), f);
    fputwc(L'\n', f);
    fclose(f);

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        if (err) *err = L"Could not replace " + path;
        return false;
    }
    return true;
}
