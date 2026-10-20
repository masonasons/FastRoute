#include "audio.h"
#include "resample.h"
#include "util.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>

// ---------------------------------------------------------------------------
// Paths and templates
// ---------------------------------------------------------------------------

const wchar_t* RecFormatExt(RecFormat fmt)
{
    switch (fmt)
    {
    case RecFormat::Flac: return L"flac";
    case RecFormat::Mp3:  return L"mp3";
    case RecFormat::Aac:  return L"m4a";   // the container the sink writer makes
    case RecFormat::Ogg:  return L"ogg";
    case RecFormat::Opus: return L"opus";
    default:              return L"wav";
    }
}

const wchar_t* RecFormatShortName(RecFormat fmt)
{
    switch (fmt)
    {
    case RecFormat::Flac: return L"FLAC";
    case RecFormat::Mp3:  return L"MP3";
    case RecFormat::Aac:  return L"AAC";
    case RecFormat::Ogg:  return L"Ogg Vorbis";
    case RecFormat::Opus: return L"Opus";
    default:              return L"WAV";
    }
}

bool RecFormatUsesBitrate(RecFormat fmt)
{
    return fmt == RecFormat::Mp3 || fmt == RecFormat::Aac ||
           fmt == RecFormat::Ogg || fmt == RecFormat::Opus;
}

const wchar_t* RecFormatName(RecFormat fmt)
{
    switch (fmt)
    {
    case RecFormat::Flac: return L"FLAC (lossless)";
    case RecFormat::Mp3:  return L"MP3 (lossy)";
    case RecFormat::Aac:  return L"AAC in an M4A file (lossy)";
    case RecFormat::Ogg:  return L"Ogg Vorbis (lossy)";
    case RecFormat::Opus: return L"Opus (lossy)";
    default:              return L"WAV (uncompressed)";
    }
}

// One piece of a path - a folder name or a file name, never both. Separators
// are among the characters taken out, because a piece holding one would be two
// pieces. Comes back empty when there was nothing usable in it, which is how a
// caller tells.
static std::wstring SanitisePiece(const std::wstring& in)
{
    std::wstring out;
    for (wchar_t c : in)
    {
        if (wcschr(L"\\/:*?\"<>|", c)) out += L'-';
        else if (c >= L' ')            out += c;
    }
    // Trailing dots and spaces are legal to type but not to create.
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    return out;
}

static std::wstring SanitiseName(const std::wstring& in)
{
    std::wstring out = SanitisePiece(in);
    return out.empty() ? std::wstring(L"recording") : out;
}

// A resolved template, split on either slash into the pieces of a relative
// path. Empty pieces are dropped, so a doubled separator or one on the end
// does not ask for a folder with no name.
//
// Each piece is cleaned on its own, which is what makes this safe to build a
// path out of: "." and ".." are thrown away rather than cleaned, the second
// one being the one that would write outside the folder the recorder was
// pointed at, and no colon survives to be read as a drive.
static std::vector<std::wstring> SplitTemplatePath(const std::wstring& in)
{
    std::vector<std::wstring> out;
    std::wstring piece;

    for (size_t i = 0; i <= in.size(); ++i)
    {
        // One past the end counts as a separator, so the last piece is taken
        // by the same code as the rest.
        const wchar_t c = (i < in.size()) ? in[i] : L'/';
        if (c != L'/' && c != L'\\')
        {
            piece += c;
            continue;
        }

        std::wstring one = piece;
        piece.clear();
        while (!one.empty() && one.front() == L' ') one.erase(0, 1);
        while (!one.empty() && one.back() == L' ') one.pop_back();
        if (one == L"." || one == L"..") continue;

        std::wstring clean = SanitisePiece(one);
        if (!clean.empty()) out.push_back(clean);
    }
    return out;
}

static void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to)
{
    if (from.empty()) return;
    size_t at = 0;
    while ((at = s.find(from, at)) != std::wstring::npos)
    {
        s.replace(at, from.size(), to);
        at += to.size();
    }
}

std::wstring ResolveRecordingPath(const std::wstring& folder,
                                  const std::wstring& nameTemplate,
                                  const std::wstring& sessionName,
                                  RecFormat fmt,
                                  const std::wstring& blockName,
                                  int blockIndex, int blockCount)
{
    SYSTEMTIME t;
    GetLocalTime(&t);

    std::wstring date = FormatW(L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    std::wstring time = FormatW(L"%02d-%02d-%02d", t.wHour, t.wMinute, t.wSecond);

    // Padded to the width the session needs rather than to a fixed two, so a
    // session of three blocks is "1. Mic" and not "01. Mic", while one of
    // twelve still sorts 09 before 10 instead of 10 before 9.
    std::wstring index;
    if (blockIndex > 0)
    {
        int width = 1;
        for (int n = blockCount; n >= 10; n /= 10) ++width;
        index = FormatW(L"%0*d", width, blockIndex);
    }

    // Each token's value is cleaned before it goes in, so a session or a block
    // whose name holds a slash cannot make a folder out of it. Only a
    // separator the user typed in the template itself does that.
    std::wstring name = nameTemplate.empty() ? std::wstring(L"{session} {date} {time}")
                                             : nameTemplate;
    ReplaceAll(name, L"{session}",  SanitisePiece(sessionName));
    ReplaceAll(name, L"{block}",    SanitisePiece(blockName));
    ReplaceAll(name, L"{index}",    index);
    // The name this was asked for by, kept working alongside the shorter one.
    ReplaceAll(name, L"{blocknumber}", index);
    ReplaceAll(name, L"{date}",     date);
    ReplaceAll(name, L"{time}",     time);
    ReplaceAll(name, L"{datetime}", date + L" " + time);

    // The template is a relative path rather than only a name: a slash or a
    // backslash in it makes a folder, so "{session}\\{block}" writes a folder
    // per session with a file per block inside it.
    //
    // Worked out in one place, because Ctrl+Shift+O has to open the same
    // folder this is about to write into.
    const std::wstring base = ResolveRecordFolder(folder);
    const wchar_t* ext = RecFormatExt(fmt);

    // A template that ends at a separator names folders and no file, which is
    // what "{session}\\{block}" comes to for a recorder's own mixed file: that
    // file is of no single block, so {block} is empty. It gets the name an
    // empty template would have given it, which puts the mix in the same
    // folder as the tracks rather than beside it.
    std::wstring tail = name;
    while (!tail.empty() && tail.back() == L' ') tail.pop_back();
    const bool endsAtSeparator = !tail.empty() &&
                                 (tail.back() == L'/' || tail.back() == L'\\');
    const std::wstring fallback = SanitiseName(SanitisePiece(sessionName) +
                                               L" " + date + L" " + time);

    // {n} is the collision escape hatch, substituted across the whole path so
    // that it works in a folder's name as well as a file's. Whether or not the
    // template uses it, never overwrite a file that is already there.
    std::wstring dir, path;
    for (int n = 1; n < 10000; ++n)
    {
        std::wstring candidate = name;
        const bool hasN = candidate.find(L"{n}") != std::wstring::npos;
        if (hasN) ReplaceAll(candidate, L"{n}", FormatW(L"%03d", n));

        std::vector<std::wstring> parts = SplitTemplatePath(candidate);

        std::wstring leaf;
        if (!endsAtSeparator && !parts.empty())
        {
            leaf = parts.back();
            parts.pop_back();
        }
        if (leaf.empty()) leaf = fallback;
        if (!hasN && n > 1) leaf += FormatW(L" (%d)", n);

        dir = base;
        for (const std::wstring& p : parts) dir += L"\\" + p;
        path = dir + L"\\" + leaf + L"." + ext;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }

    // After the name has settled, so the folders made are the ones being
    // written into rather than a set from a candidate that collided.
    const int made = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS &&
        made != ERROR_FILE_EXISTS && !DirectoryExists(dir))
    {
        // Said here rather than left to the recorder, which can only report
        // that it could not create a file - and a file it cannot create
        // because the folder is not there reads as a fault in the file name
        // rather than in the folder.
        LogError(FormatW(L"%s could not be created (error %d), so there is "
                         L"nowhere to record to.", dir.c_str(), made));
    }
    return path;
}

// ---------------------------------------------------------------------------
// Writer thread
//
// Encoding must never happen on the mixer's thread: the file system and the
// Media Foundation encoders both block. Audio is queued and a writer thread
// drains it. The queue is deliberately unbounded - dropping a recording to
// save a few megabytes of RAM would be the wrong trade.
// ---------------------------------------------------------------------------
class RecorderBase : public Recorder
{
public:
    void Write(const float* buf, size_t frames) override
    {
        if (!m_running) return;
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_queue.insert(m_queue.end(), buf, buf + frames * kChannels);
        }
        m_wake.notify_one();
    }

    void Stop() override
    {
        if (!m_running) return;
        m_running = false;
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
        Close();
    }

    std::wstring Path() const override { return m_path; }

protected:
    // Called by each subclass's Start once it knows what it has settled on -
    // which need not be what was asked for, since a format can refuse. After
    // this, m_rate is the rate going into the file and Encode is called at it.
    void SetFormat(int inRate, int fileRate, int bits)
    {
        m_rate = fileRate > 0 ? fileRate : inRate;
        m_bits = bits;
        // Quality 10 rather than the live default: this runs on the writer
        // thread, where the only thing waiting on it is the file.
        m_conv.Prepare(inRate > 0 ? inRate : m_rate, m_rate, 10);
    }

    void RunWriter()
    {
        m_running = true;
        m_thread = std::thread([this] {
            std::vector<float> chunk;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> g(m_lock);
                    m_wake.wait(g, [this] { return !m_queue.empty() || !m_running; });
                    if (m_queue.empty() && !m_running) break;
                    chunk.assign(m_queue.begin(), m_queue.end());
                    m_queue.clear();
                }
                if (chunk.empty()) continue;

                // Recording at a rate of its own is converted here rather
                // than anywhere nearer the audio clock. Latched off entirely
                // when the two rates agree, which is the ordinary case.
                if (!m_conv.Engaged())
                {
                    Encode(chunk.data(), chunk.size() / kChannels);
                    continue;
                }
                m_converted.clear();
                m_conv.Run(chunk.data(), chunk.size() / kChannels, m_converted);
                if (!m_converted.empty())
                    Encode(m_converted.data(), m_converted.size() / kChannels);
            }
        });
    }

    // Converts the interleaved float bus to interleaved 16 bit PCM.
    static void ToPcm16(const float* src, size_t frames, std::vector<short>& dst)
    {
        dst.resize(frames * kChannels);
        for (size_t i = 0; i < frames * kChannels; ++i)
        {
            float v = src[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            dst[i] = (short)lrintf(v * 32767.0f);
        }
    }

    // The same at 24 bit, three bytes per sample, little endian - which is a
    // byte stream rather than any integer type, so it is built as one.
    static void ToPcm24(const float* src, size_t frames,
                        std::vector<unsigned char>& dst)
    {
        dst.resize(frames * kChannels * 3);
        unsigned char* p = dst.data();
        for (size_t i = 0; i < frames * kChannels; ++i)
        {
            float v = src[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            const long q = lrintf(v * 8388607.0f);
            const unsigned long u = (unsigned long)q;
            *p++ = (unsigned char)(u & 0xFF);
            *p++ = (unsigned char)((u >> 8) & 0xFF);
            *p++ = (unsigned char)((u >> 16) & 0xFF);
        }
    }

    virtual void Encode(const float* buf, size_t frames) = 0;
    virtual void Close() = 0;

    std::wstring m_path;
    int m_rate = 48000;     // the rate going into the file, after conversion
    int m_bits = 16;

private:
    Converter m_conv;
    std::vector<float> m_converted;
    std::deque<float> m_queue;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
};

// ---------------------------------------------------------------------------
// WAV
//
// Written directly rather than through Media Foundation: it is a header and
// a stream of samples, and doing it here means the size fields can be patched
// on close even if the encoder path is unavailable.
// ---------------------------------------------------------------------------
class WavRecorder : public RecorderBase
{
public:
    ~WavRecorder() override { Stop(); }

    bool Start(const std::wstring& path, const RecordFormat& f,
               std::wstring* err) override
    {
        m_path = path;
        // WAV is the one format that takes anything: any rate, and 16 or 24
        // bit integer or 32 bit float. Nothing is clamped away here.
        SetFormat(f.inRate, f.rateHz, f.bits);

        if (_wfopen_s(&m_f, path.c_str(), L"wb") != 0 || !m_f)
        {
            if (err) *err = L"Could not create " + path;
            return false;
        }

        const int bytesPerSample = m_bits / 8;
        const int bytesPerFrame = kChannels * bytesPerSample;
        // 32 bit is float, which is a different format tag rather than a wider
        // integer, and float is what the bus already holds - so at 32 bit the
        // samples go to disk exactly as they arrived.
        const bool isFloat = (m_bits == 32);
        unsigned char hdr[44] = { 0 };
        memcpy(hdr + 0, "RIFF", 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        Put32(hdr + 16, 16);                       // fmt chunk size
        Put16(hdr + 20, isFloat ? 3 : 1);          // IEEE float, or PCM
        Put16(hdr + 22, (unsigned short)kChannels);
        Put32(hdr + 24, (unsigned)m_rate);
        Put32(hdr + 28, (unsigned)(m_rate * bytesPerFrame));
        Put16(hdr + 32, (unsigned short)bytesPerFrame);
        Put16(hdr + 34, (unsigned short)m_bits);
        memcpy(hdr + 36, "data", 4);
        fwrite(hdr, 1, sizeof(hdr), m_f);

        RunWriter();
        return true;
    }

protected:
    void Encode(const float* buf, size_t frames) override
    {
        if (!m_f) return;
        if (m_bits == 32)
        {
            // Clamped even so: a sample above full scale is legal in a float
            // file but is still a sample nothing can play back undistorted,
            // and every other depth here is clamped.
            m_flt.assign(buf, buf + frames * kChannels);
            for (float& v : m_flt)
            {
                if (v > 1.0f) v = 1.0f;
                else if (v < -1.0f) v = -1.0f;
            }
            fwrite(m_flt.data(), sizeof(float), m_flt.size(), m_f);
            m_bytes += m_flt.size() * sizeof(float);
        }
        else if (m_bits == 24)
        {
            ToPcm24(buf, frames, m_raw);
            fwrite(m_raw.data(), 1, m_raw.size(), m_f);
            m_bytes += m_raw.size();
        }
        else
        {
            ToPcm16(buf, frames, m_pcm);
            fwrite(m_pcm.data(), sizeof(short), m_pcm.size(), m_f);
            m_bytes += m_pcm.size() * sizeof(short);
        }
    }

    void Close() override
    {
        if (!m_f) return;
        // Patch the two sizes now that the length is known.
        unsigned char v[4];
        Put32(v, (unsigned)(36 + m_bytes));
        fseek(m_f, 4, SEEK_SET);
        fwrite(v, 1, 4, m_f);
        Put32(v, (unsigned)m_bytes);
        fseek(m_f, 40, SEEK_SET);
        fwrite(v, 1, 4, m_f);
        fclose(m_f);
        m_f = nullptr;
    }

private:
    static void Put16(unsigned char* p, unsigned short v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
    static void Put32(unsigned char* p, unsigned v)
    {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    }

    FILE* m_f = nullptr;
    size_t m_bytes = 0;
    std::vector<short> m_pcm;
    std::vector<unsigned char> m_raw;
    std::vector<float> m_flt;
};

// ---------------------------------------------------------------------------
// FLAC and MP3, through Media Foundation
//
// Windows ships both encoders (a FLAC encoder MFT and an MP3 ACM wrapper MFT),
// so neither format needs a third party library. The sink writer picks the
// file container from the extension.
// ---------------------------------------------------------------------------
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

class MfRecorder : public RecorderBase
{
public:
    explicit MfRecorder(RecFormat fmt) : m_fmt(fmt) {}
    ~MfRecorder() override { Stop(); }

    bool Start(const std::wstring& path, const RecordFormat& f,
               std::wstring* err) override
    {
        m_path = path;
        // Only FLAC has a sample width to choose. MP3 and AAC spend a bitrate
        // instead, and their input is 16 bit whatever the block asked for.
        const int bits = (m_fmt == RecFormat::Flac && f.bits == 24) ? 24 : 16;
        SetFormat(f.inRate, f.rateHz, bits);
        const int rate = m_rate;
        const int kbps = f.kbps;

        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) { Fail(err, L"Media Foundation could not start", hr); return false; }
        m_mfUp = true;

        const GUID sub = (m_fmt == RecFormat::Flac) ? MFAudioFormat_FLAC
                       : (m_fmt == RecFormat::Aac)  ? MFAudioFormat_AAC
                                                    : MFAudioFormat_MP3;
        IMFMediaType* out = PickOutputType(sub, rate, bits, kbps);
        if (!out)
        {
            // Worth naming the rates it does take, because the list differs
            // by format and by Windows release - a fixed sentence here was
            // guessing, and told anyone at 96 kHz to go and change their
            // output device when the recorder can simply be set to 48.
            std::wstring rates;
            for (int r : RecFormatRates(m_fmt))
                rates += (rates.empty() ? L"" : L", ") + FormatW(L"%d", r);
            if (err) *err = FormatW(
                L"Windows has no %s encoder here that accepts %d Hz stereo at "
                L"%d bit. Set this block's sample rate to one of these: %s.",
                RecFormatName(m_fmt), rate, bits,
                rates.empty() ? L"none at all" : rates.c_str());
            return false;
        }

        hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &m_writer);
        if (FAILED(hr)) { out->Release(); Fail(err, L"Could not create " + path, hr); return false; }

        hr = m_writer->AddStream(out, &m_stream);
        out->Release();
        if (FAILED(hr)) { Fail(err, L"The encoder rejected the output format", hr); return false; }

        IMFMediaType* in = nullptr;
        const UINT32 align = (UINT32)(kChannels * (bits / 8));
        if (SUCCEEDED(MFCreateMediaType(&in)) && in)
        {
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, (UINT32)bits);
            in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
            in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, align);
            in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)rate * align);
            in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            hr = m_writer->SetInputMediaType(m_stream, in, nullptr);
            in->Release();
        }
        if (FAILED(hr))
        {
            Fail(err, FormatW(L"The encoder rejected %d bit PCM input", bits), hr);
            return false;
        }

        hr = m_writer->BeginWriting();
        if (FAILED(hr)) { Fail(err, L"The encoder could not start", hr); return false; }

        RunWriter();
        return true;
    }

protected:
    void Encode(const float* buf, size_t frames) override
    {
        if (!m_writer || frames == 0) return;
        const void* src = nullptr;
        DWORD bytes = 0;
        if (m_bits == 24)
        {
            ToPcm24(buf, frames, m_raw);
            src = m_raw.data();
            bytes = (DWORD)m_raw.size();
        }
        else
        {
            ToPcm16(buf, frames, m_pcm);
            src = m_pcm.data();
            bytes = (DWORD)(m_pcm.size() * sizeof(short));
        }

        IMFMediaBuffer* mb = nullptr;
        if (FAILED(MFCreateMemoryBuffer(bytes, &mb)) || !mb) return;

        BYTE* p = nullptr;
        DWORD maxLen = 0;
        if (SUCCEEDED(mb->Lock(&p, &maxLen, nullptr)))
        {
            memcpy(p, src, bytes);
            mb->Unlock();
            mb->SetCurrentLength(bytes);

            IMFSample* smp = nullptr;
            if (SUCCEEDED(MFCreateSample(&smp)) && smp)
            {
                smp->AddBuffer(mb);
                smp->SetSampleTime(m_time);
                LONGLONG dur = (LONGLONG)frames * 10000000 / m_rate;
                smp->SetSampleDuration(dur);
                m_time += dur;
                m_writer->WriteSample(m_stream, smp);
                smp->Release();
            }
        }
        mb->Release();
    }

    void Close() override
    {
        if (m_writer)
        {
            m_writer->Finalize();
            m_writer->Release();
            m_writer = nullptr;
        }
        if (m_mfUp) { MFShutdown(); m_mfUp = false; }
    }

private:
    void Fail(std::wstring* err, const std::wstring& what, HRESULT hr)
    {
        if (err) *err = what + L" (" + HrText(hr) + L").";
    }

    // Picks the encoder output type matching our rate, depth and channel
    // count. For FLAC the bit depth has to match the input type exactly, and
    // for MP3 we take whichever advertised bitrate is closest to the one
    // asked for.
    static IMFMediaType* PickOutputType(const GUID& sub, int rate, int bits, int kbps)
    {
        IMFCollection* col = nullptr;
        if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(sub, MFT_ENUM_FLAG_ALL,
                                                           nullptr, &col)) || !col)
            return nullptr;

        DWORD count = 0;
        col->GetElementCount(&count);

        const UINT32 wantBytes = (UINT32)(kbps * 1000 / 8);
        IMFMediaType* best = nullptr;
        UINT32 bestDelta = 0xFFFFFFFF;

        for (DWORD i = 0; i < count; ++i)
        {
            IUnknown* unk = nullptr;
            if (FAILED(col->GetElement(i, &unk)) || !unk) continue;

            IMFMediaType* t = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&t))) && t)
            {
                UINT32 sr = 0, ch = 0, typeBits = 0, bps = 0;
                t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &typeBits);
                t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);

                // A type reporting no depth at all is a lossy one, where the
                // field means nothing, so those are taken as they come.
                bool ok = (sr == (UINT32)rate) && (ch == (UINT32)kChannels) &&
                          (typeBits == 0 || typeBits == (UINT32)bits);
                if (ok)
                {
                    UINT32 d = (bps > wantBytes) ? bps - wantBytes : wantBytes - bps;
                    if (d < bestDelta)
                    {
                        bestDelta = d;
                        if (best) best->Release();
                        best = t;
                        best->AddRef();
                    }
                }
                t->Release();
            }
            unk->Release();
        }
        col->Release();
        return best;
    }

    RecFormat m_fmt;
    IMFSinkWriter* m_writer = nullptr;
    DWORD m_stream = 0;
    LONGLONG m_time = 0;
    bool m_mfUp = false;
    std::vector<short> m_pcm;
    std::vector<unsigned char> m_raw;
};

// Media Foundation is delay loaded, so nothing here touches it until this
// says the DLLs are there. Windows N and KN editions ship without them until
// the Media Feature Pack is installed, and a delay loaded import that cannot
// be resolved raises rather than returning an error.
bool MediaFoundationAvailable()
{
    static bool ok = LoadLibraryW(L"mfplat.dll") != nullptr &&
                     LoadLibraryW(L"mfreadwrite.dll") != nullptr &&
                     LoadLibraryW(L"mf.dll") != nullptr;
    return ok;
}

// Whether Windows here can encode a format at all. Probed once: enumerating
// the encoder MFTs is not free, and the answer cannot change while we run.
bool RecFormatAvailable(RecFormat fmt)
{
    // Written here rather than by Windows, so these work everywhere.
    if (fmt == RecFormat::Wav || fmt == RecFormat::Ogg || fmt == RecFormat::Opus)
        return true;
    if (!MediaFoundationAvailable()) return false;

    static int cached[kRecFormatCount] = { 1, -1, -1, -1, 1, 1 };
    int idx = (int)fmt;
    if (cached[idx] >= 0) return cached[idx] != 0;

    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION)))
    {
        const GUID sub = (fmt == RecFormat::Flac) ? MFAudioFormat_FLAC
                       : (fmt == RecFormat::Aac)  ? MFAudioFormat_AAC
                                                  : MFAudioFormat_MP3;
        IMFCollection* col = nullptr;
        if (SUCCEEDED(MFTranscodeGetAudioOutputAvailableTypes(sub, MFT_ENUM_FLAG_ALL,
                                                              nullptr, &col)) && col)
        {
            DWORD count = 0;
            col->GetElementCount(&count);
            ok = count > 0;
            col->Release();
        }
        MFShutdown();
    }
    cached[idx] = ok ? 1 : 0;
    return ok;
}

std::unique_ptr<Recorder> MakeRecorder(RecFormat fmt)
{
    if (fmt == RecFormat::Wav)  return std::unique_ptr<Recorder>(new WavRecorder());
    if (fmt == RecFormat::Ogg)  return MakeVorbisRecorder();
    if (fmt == RecFormat::Opus) return MakeOpusRecorder();
    return std::unique_ptr<Recorder>(new MfRecorder(fmt));
}

// ---------------------------------------------------------------------------
// What each format can be written as
//
// Not a table of what the formats support in the abstract: a table of what
// this machine can actually do, because for FLAC, MP3 and AAC the encoder is
// Windows' own and its list differs by release. Asked once, since it cannot
// change while we run.
// ---------------------------------------------------------------------------

// The rates the encoder for `sub` advertises for stereo, ascending.
static std::vector<int> MfRatesFor(const GUID& sub, int* widestDepth)
{
    std::vector<int> out;
    if (widestDepth) *widestDepth = 16;
    if (!MediaFoundationAvailable()) return out;
    if (FAILED(MFStartup(MF_VERSION))) return out;

    IMFCollection* col = nullptr;
    if (SUCCEEDED(MFTranscodeGetAudioOutputAvailableTypes(sub, MFT_ENUM_FLAG_ALL,
                                                          nullptr, &col)) && col)
    {
        DWORD count = 0;
        col->GetElementCount(&count);
        for (DWORD i = 0; i < count; ++i)
        {
            IUnknown* unk = nullptr;
            if (FAILED(col->GetElement(i, &unk)) || !unk) continue;
            IMFMediaType* t = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&t))) && t)
            {
                UINT32 sr = 0, ch = 0, bits = 0;
                t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                if (ch == (UINT32)kChannels && sr > 0)
                {
                    if (std::find(out.begin(), out.end(), (int)sr) == out.end())
                        out.push_back((int)sr);
                    if (widestDepth && bits == 24) *widestDepth = 24;
                }
                t->Release();
            }
            unk->Release();
        }
        col->Release();
    }
    MFShutdown();
    std::sort(out.begin(), out.end());
    return out;
}

// Opus is a 48 kHz format: the encoder accepts other rates and resamples to
// 48 kHz regardless, and the original rate survives only as a number in the
// header. So the file's rate is not a choice, and saying so beats offering a
// setting that does nothing.
static const int kOpusFileRate = 48000;

// The bitrates a recorder offers. One list, in one place, because the dialog
// and the resolving below have to agree about what was on offer.
const int kRecBitrates[] = { 96, 128, 160, 192, 256, 320 };
const int kRecBitrateCount = (int)(sizeof(kRecBitrates) / sizeof(kRecBitrates[0]));

// Every rate a block may be set to, before a format narrows it.
static const int kCandidateRates[] = { 8000, 11025, 16000, 22050, 24000, 32000,
                                       44100, 48000, 88200, 96000, 176400, 192000 };

const std::vector<int>& RecFormatRates(RecFormat fmt)
{
    // Written once each, on first ask. Empty stands for "any rate at all",
    // which is true of the two formats FastRoute writes itself.
    static std::vector<int> cache[kRecFormatCount];
    static bool done[kRecFormatCount] = { false };

    const int idx = (fmt >= RecFormat::Wav && (int)fmt < kRecFormatCount) ? (int)fmt : 0;
    if (!done[idx])
    {
        switch ((RecFormat)idx)
        {
        case RecFormat::Flac:
            cache[idx] = MfRatesFor(MFAudioFormat_FLAC, nullptr);
            break;
        case RecFormat::Mp3:
            cache[idx] = MfRatesFor(MFAudioFormat_MP3, nullptr);
            break;
        case RecFormat::Aac:
            cache[idx] = MfRatesFor(MFAudioFormat_AAC, nullptr);
            break;
        case RecFormat::Opus:
            cache[idx] = { kOpusFileRate };
            break;
        case RecFormat::Ogg:
            // Asked of libvorbis, which refuses more than the rate alone
            // decides: a rate counts as usable only if some bitrate we offer
            // can actually be written at it.
            for (int r : kCandidateRates)
                for (int k = 0; k < kRecBitrateCount; ++k)
                    if (VorbisAccepts(r, kRecBitrates[k]))
                    {
                        cache[idx].push_back(r);
                        break;
                    }
            break;
        default:
            break;                     // WAV: any rate at all
        }
        done[idx] = true;
    }
    return cache[idx];
}

const std::vector<int>& RecFormatDepths(RecFormat fmt)
{
    static std::vector<int> wav = { 16, 24, 32 };
    static std::vector<int> flac;
    static bool flacDone = false;
    static std::vector<int> none;

    if (fmt == RecFormat::Wav) return wav;
    if (fmt == RecFormat::Flac)
    {
        if (!flacDone)
        {
            int widest = 16;
            MfRatesFor(MFAudioFormat_FLAC, &widest);
            flac.push_back(16);
            if (widest >= 24) flac.push_back(24);
            flacDone = true;
        }
        return flac;
    }
    return none;                       // the lossy formats have no depth
}

std::vector<int> RecFormatBitrates(RecFormat fmt, int rateHz)
{
    std::vector<int> out;
    if (!RecFormatUsesBitrate(fmt)) return out;      // WAV and FLAC have none

    for (int i = 0; i < kRecBitrateCount; ++i)
    {
        // Only Vorbis ties the two together. Opus takes anything in this
        // range, and MP3 and AAC take the nearest their encoder advertises
        // rather than refusing, so for those the rate does not narrow it.
        if (fmt == RecFormat::Ogg && rateHz > 0 &&
            !VorbisAccepts(rateHz, kRecBitrates[i]))
            continue;
        out.push_back(kRecBitrates[i]);
    }
    return out;
}

RecordFormat RecFileFormatFor(RecFormat fmt, int rateHz, int bits, int kbps,
                              int sessionRate)
{
    RecordFormat f;
    f.inRate = sessionRate > 0 ? sessionRate : kDefaultRate;
    f.kbps = kbps;

    // Rate. Zero means the session's own, which is also where an unusable
    // choice lands: a rate the format cannot take is no better than no choice
    // at all, and the recorder will say so by name when it refuses.
    const std::vector<int>& rates = RecFormatRates(fmt);
    f.rateHz = (rateHz > 0) ? rateHz : f.inRate;
    if (!rates.empty() &&
        std::find(rates.begin(), rates.end(), f.rateHz) == rates.end())
    {
        // Opus has exactly one, so this lands on it rather than on a rate it
        // would silently convert away from anyway.
        f.rateHz = (rates.size() == 1) ? rates.front()
                 : (std::find(rates.begin(), rates.end(), f.inRate) != rates.end()
                        ? f.inRate : f.rateHz);
    }

    // Depth. Zero means the format's own default, which is 16: that is what
    // every format here was written at before this was a choice.
    const std::vector<int>& depths = RecFormatDepths(fmt);
    f.bits = (bits == 16 || bits == 24 || bits == 32) ? bits : 16;
    if (depths.empty())
        f.bits = 16;                   // lossy: the field means nothing
    else if (std::find(depths.begin(), depths.end(), f.bits) == depths.end())
        f.bits = 16;

    // Bitrate, last, because for Vorbis it depends on the rate settled on
    // just above. A document can hold a pair the encoder will not take - the
    // rate was changed and the bitrate left where it was, or the two were
    // written by a build whose libvorbis took more - and refusing to record
    // over it would be the wrong answer when MP3 has always just taken the
    // nearest it could get.
    const std::vector<int> rates2 = RecFormatBitrates(fmt, f.rateHz);
    if (!rates2.empty() &&
        std::find(rates2.begin(), rates2.end(), f.kbps) == rates2.end())
    {
        int best = rates2.front();
        for (int k : rates2)
            if (abs(k - f.kbps) < abs(best - f.kbps)) best = k;
        f.kbps = best;
    }
    return f;
}

// ---------------------------------------------------------------------------
// Recorder block output: silence gate and file splitting
// ---------------------------------------------------------------------------

RecordSettings RecordSettingsFor(const Block& b, const std::wstring& sessionName,
                                 int blockIndex, int blockCount)
{
    RecordSettings s;
    s.folder = b.folder;
    s.nameTemplate = b.nameTemplate;
    s.sessionName = sessionName;
    s.blockIndex = blockIndex;
    s.blockCount = blockCount;
    // Against kRecFormatCount, not against 2. It was 2 when WAV, FLAC and MP3
    // were the only formats, and it stayed 2 when AAC, Ogg Vorbis and Opus
    // were added - so choosing one of those three silently recorded a WAV
    // file, under whatever name the template gave it.
    s.fmt = (RecFormat)((b.recFormat >= 0 && b.recFormat < kRecFormatCount)
                            ? b.recFormat : 0);
    s.kbps = b.bitrateKbps;
    s.rateHz = b.recRateHz;
    s.bits = b.recBits;
    s.skipSilence = b.skipSilence;
    s.silenceDb = b.silenceDb;
    s.holdSeconds = b.silenceHoldSec;
    s.splitMode = b.splitMode;
    s.splitMinutes = b.splitMinutes;
    s.splitMb = b.splitMb;
    return s;
}

// One input block's own file, from the recorder block that asked for it.
//
// Two of the recorder's settings are deliberately not passed on, and for the
// same reason: separate tracks are worth having because they line up, and both
// of those settings would cut each file at a different moment.
//
//  * the silence gate decides per file, so a track that went quiet would come
//    out shorter than the ones that did not;
//  * splitting by size depends on how compressible each track turned out to
//    be, so the files would break in different places. Splitting by time is
//    the same number of frames for everyone, so that one is kept.
RecordSettings TrackSettingsFor(const Block& recorderBlock,
                                const std::wstring& sessionName,
                                const std::wstring& blockName,
                                int blockIndex, int blockCount)
{
    RecordSettings s = RecordSettingsFor(recorderBlock, sessionName,
                                         blockIndex, blockCount);
    s.blockName = blockName;
    s.skipSilence = false;
    if (s.splitMode == 2) s.splitMode = 0;

    // Every track needs a name of its own. A template that does not say which
    // block the file is of gets told, because otherwise all of them resolve to
    // the same name and the collision escape hatch numbers them in whatever
    // order they happened to open. A template naming the block's number
    // instead is already distinct, so that counts.
    if (s.nameTemplate.find(L"{block}") == std::wstring::npos &&
        s.nameTemplate.find(L"{index}") == std::wstring::npos &&
        s.nameTemplate.find(L"{blocknumber}") == std::wstring::npos)
    {
        if (s.nameTemplate.empty()) s.nameTemplate = L"{session} {date} {time}";
        s.nameTemplate += L" - {block}";
    }
    return s;
}

// The gate measures over short blocks rather than per sample, so a single
// sample either way cannot open or shut it.
static const int kMeterMs = 20;
// Held back while the gate is shut and flushed when it opens, so the attack
// that opened it is in the file rather than clipped off the front of it.
static const int kPreRollMs = 300;
// RIFF stores its sizes in 32 bits, so a WAV cannot exceed 4 GB. This is the
// ceiling a WAV is rotated at whatever the split setting says, because the
// alternative is a file that silently stops being readable at the end.
static const unsigned long long kWavCeilingBytes = 4000ull * 1000ull * 1000ull;

bool RecordSink::Start(const RecordSettings& s, int rate, std::wstring* err)
{
    m_set = s;
    m_rate = rate;
    m_open = !s.skipSilence;   // with no gate the file starts immediately
    m_running = true;

    // The first file is opened on the sink thread like every later one, so
    // that every Media Foundation call for this recording happens on the one
    // thread. Start() waits for the answer so it can still be reported.
    m_thread = std::thread([this] { Worker(); });

    std::unique_lock<std::mutex> g(m_openLock);
    m_openCv.wait(g, [this] { return m_openDone; });
    if (!m_openOk)
    {
        g.unlock();
        m_running = false;
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
        if (err) *err = m_openErr;
        return false;
    }
    return true;
}

void RecordSink::Write(const float* buf, size_t frames)
{
    if (!m_running) return;
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_queue.insert(m_queue.end(), buf, buf + frames * kChannels);
    }
    m_wake.notify_one();
}

void RecordSink::Stop()
{
    if (!m_running.exchange(false)) return;
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void RecordSink::Configure(const RecordSettings& s)
{
    std::lock_guard<std::mutex> g(m_setLock);
    m_set = s;
}

RecordSettings RecordSink::Snapshot()
{
    std::lock_guard<std::mutex> g(m_setLock);
    return m_set;
}

std::wstring RecordSink::Path()
{
    std::lock_guard<std::mutex> g(m_setLock);
    return m_file ? m_file->Path() : std::wstring();
}

void RecordSink::Worker()
{
    // Media Foundation's sink writer wants COM up on the calling thread, and
    // this thread is the only one that ever touches it.
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::wstring err;
    bool ok = Open(&err);
    {
        std::lock_guard<std::mutex> g(m_openLock);
        m_openDone = true;
        m_openOk = ok;
        m_openErr = err;
    }
    m_openCv.notify_all();

    if (ok)
    {
        std::vector<float> chunk;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> g(m_lock);
                m_wake.wait(g, [this] { return !m_queue.empty() || !m_running; });
                if (m_queue.empty() && !m_running) break;
                chunk.assign(m_queue.begin(), m_queue.end());
                m_queue.clear();
            }
            if (!chunk.empty()) Consume(chunk.data(), chunk.size() / kChannels);
        }

        // Whatever is left in the metering block goes out if the gate is open;
        // a partial block is not worth measuring.
        if (m_open && !m_acc.empty())
            Emit(m_acc.data(), m_acc.size() / kChannels);
        CloseFile();
    }

    if (SUCCEEDED(hrCom)) CoUninitialize();
}

bool RecordSink::Open(std::wstring* err)
{
    RecordSettings s = Snapshot();

    if (!RecFormatAvailable(s.fmt))
    {
        if (err) *err = FormatW(
            L"%s cannot encode %s, so this block cannot record in that format. "
            L"Choose WAV, Ogg Vorbis or Opus, which FastRoute writes itself.",
            WindowsVersionText().c_str(), RecFormatName(s.fmt));
        return false;
    }

    std::wstring path = ResolveRecordingPath(s.folder, s.nameTemplate, s.sessionName,
                                            s.fmt, s.blockName,
                                            s.blockIndex, s.blockCount);

    const RecordFormat rf = RecFileFormatFor(s.fmt, s.rateHz, s.bits, s.kbps, m_rate);
    std::unique_ptr<Recorder> r = MakeRecorder(s.fmt);
    if (!r || !r->Start(path, rf, err)) return false;

    // How a WAV's size is worked out from its frame count, which is the one
    // thing the splitting cannot ask the file system for without lagging.
    m_fileRate = rf.rateHz;
    m_fileBytesPerFrame = kChannels * (rf.bits / 8);

    m_file = std::move(r);
    m_fileFrames = 0;
    m_sinceSizeCheck = 0;
    m_files.fetch_add(1);
    return true;
}

void RecordSink::CloseFile()
{
    if (!m_file) return;
    m_file->Stop();
    m_file.reset();
}

bool RecordSink::Rotate()
{
    CloseFile();
    std::wstring err;
    if (Open(&err))
        return true;

    m_dead = true;
    LogError(L"The recording could not be continued in a new file, so it has "
             L"stopped: " + (err.empty() ? std::wstring(L"the file could not be created.") : err));
    return false;
}

// Runs the gate over `frames` and passes whatever survives to Emit.
void RecordSink::Consume(const float* buf, size_t frames)
{
    if (m_dead) return;

    RecordSettings s = Snapshot();

    if (!s.skipSilence)
    {
        // The gate is off, so anything held back from when it was on has to go
        // out first, in order, before this block.
        if (!m_open)
        {
            m_open = true;
            m_quietFrames = 0;
        }
        if (!m_acc.empty())
        {
            std::vector<float> pending;
            pending.swap(m_acc);
            Emit(pending.data(), pending.size() / kChannels);
        }
        if (!m_preRoll.empty())
        {
            std::vector<float> pre(m_preRoll.begin(), m_preRoll.end());
            m_preRoll.clear();
            Emit(pre.data(), pre.size() / kChannels);
        }
        m_skipping.store(false);
        Emit(buf, frames);
        return;
    }

    const size_t block = (size_t)(m_rate * kMeterMs / 1000) * kChannels;
    const size_t preRollMax = (size_t)(m_rate * kPreRollMs / 1000) * kChannels;
    const size_t holdFrames = (size_t)(s.holdSeconds > 0 ? s.holdSeconds : 0) *
                              (size_t)m_rate;
    // The threshold is an amplitude once it is off the dial.
    const float threshold = (float)pow(10.0, (double)s.silenceDb / 20.0);

    m_acc.insert(m_acc.end(), buf, buf + frames * kChannels);

    size_t at = 0;
    while (m_acc.size() - at >= block)
    {
        const float* p = m_acc.data() + at;

        double sum = 0.0;
        for (size_t i = 0; i < block; ++i) sum += (double)p[i] * (double)p[i];
        const float rms = (float)sqrt(sum / (double)block);

        if (rms >= threshold)
        {
            if (!m_open)
            {
                // Opening: the pre-roll is the run-up to this block, so it
                // goes into the file ahead of it.
                m_open = true;
                if (!m_preRoll.empty())
                {
                    std::vector<float> pre(m_preRoll.begin(), m_preRoll.end());
                    m_preRoll.clear();
                    Emit(pre.data(), pre.size() / kChannels);
                }
            }
            m_quietFrames = 0;
        }
        else if (m_open && s.holdSeconds != kHoldAlways)
        {
            // A hold of "always" means the gate only ever opens: the threshold
            // decides when the recording starts and nothing after that.
            m_quietFrames += block / kChannels;
            if (m_quietFrames >= holdFrames) m_open = false;
        }

        if (m_open)
        {
            Emit(p, block / kChannels);
        }
        else
        {
            m_preRoll.insert(m_preRoll.end(), p, p + block);
            while (m_preRoll.size() > preRollMax)
                m_preRoll.erase(m_preRoll.begin(),
                                m_preRoll.begin() + (m_preRoll.size() - preRollMax));
        }
        at += block;
        if (m_dead) break;
    }

    if (at) m_acc.erase(m_acc.begin(), m_acc.begin() + at);
    m_skipping.store(!m_open);
}

// How big the current file is. A WAV is 16 bit PCM at a known rate, so its
// size follows from the frame count exactly and with no lag - which matters,
// because the file on disk trails whatever the writer thread has flushed. An
// encoded file can only be measured, so that one is asked of the file system.
unsigned long long RecordSink::CurrentBytes(const RecordSettings& s)
{
    unsigned long long bytes = 0;
    if (s.fmt == RecFormat::Wav)
    {
        bytes = 44ull + FileFrames() * (unsigned long long)m_fileBytesPerFrame;
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (m_file && GetFileAttributesExW(m_file->Path().c_str(),
                                           GetFileExInfoStandard, &fad))
            bytes = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    // Published for the status line as a side effect of working it out, so
    // nothing has to hit the file system twice for the same number.
    m_bytes.store(bytes);
    return bytes;
}

// Rotates if the file has had enough bytes. False means recording has stopped.
bool RecordSink::CheckSize(const RecordSettings& s)
{
    const bool wantSize = (s.splitMode == 2);
    const bool wav = (s.fmt == RecFormat::Wav);
    if (!wantSize && !wav) return true;

    unsigned long long bytes = CurrentBytes(s);

    if (wantSize)
    {
        unsigned long long limit =
            (unsigned long long)(s.splitMb > 0 ? s.splitMb : 1) * 1024ull * 1024ull;
        if (bytes >= limit) return Rotate();
    }
    if (wav && bytes >= kWavCeilingBytes)
    {
        LogError(L"A WAV recording reached 4 GB, which is as large as the format "
                 L"can describe, so it has been continued in a new file.");
        return Rotate();
    }
    return true;
}

// Writes to the current file, splitting it where the settings say to.
void RecordSink::Emit(const float* buf, size_t frames)
{
    // Written in slices rather than whole buffers. A split boundary has to be
    // honoured wherever it falls, and audio does not always arrive in real
    // time - a recording catching up after a stall can hand over minutes at
    // once, and a whole-buffer write would sail straight past the limit.
    const size_t slice = (size_t)(m_rate / 4);

    while (frames > 0)
    {
        if (!m_file || m_dead) return;

        const RecordSettings s = Snapshot();
        const unsigned long long frameLimit = (s.splitMode == 1)
            ? (unsigned long long)(s.splitMinutes > 0 ? s.splitMinutes : 1) * 60ull *
              (unsigned long long)m_rate
            : 0;

        if (frameLimit && m_fileFrames >= frameLimit)
        {
            if (!Rotate()) return;
            continue;
        }

        size_t take = frames;
        if (frameLimit)
        {
            // A file asked to hold ten minutes holds ten minutes, not ten
            // minutes and however much the last buffer happened to carry.
            unsigned long long room = frameLimit - m_fileFrames;
            if (room < (unsigned long long)take) take = (size_t)room;
        }
        if (take > slice) take = slice;

        m_file->Write(buf, take);
        m_fileFrames += take;
        m_sinceSizeCheck += take;
        m_totalFrames.fetch_add(take);
        // WAV size follows from the frame count, so it can be kept current
        // here for nothing. The encoded formats are only measured when the
        // splitting asks, which is often enough for a status line.
        if (Snapshot().fmt == RecFormat::Wav)
            m_bytes.store(44ull + FileFrames() *
                                      (unsigned long long)m_fileBytesPerFrame);
        buf += take * kChannels;
        frames -= take;

        if (frameLimit && m_fileFrames >= frameLimit)
        {
            if (!Rotate()) return;
            continue;
        }

        // An encoded file has to be measured on disk, so that is throttled;
        // a WAV is arithmetic and can be checked every slice.
        if (s.fmt == RecFormat::Wav || m_sinceSizeCheck >= (size_t)m_rate)
        {
            m_sinceSizeCheck = 0;
            if (!CheckSize(s)) return;
        }
    }
}
