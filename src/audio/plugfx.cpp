#include "audio.h"
#include "util.h"
#include "../plugins/plugins.h"

#include <algorithm>
#include <mutex>

// ---------------------------------------------------------------------------
// An external plugin as an effect block
//
// The plugin layer knows nothing about FastRoute's graph and the graph knows
// nothing about CLAP or VST3; this is the one file that knows both. All it
// really does is own an instance, hand it buffers, and push the block's stored
// parameter values into it.
//
// A plugin that will not load leaves the block silent-but-transparent rather
// than stopping the session. Someone whose reverb has been uninstalled still
// wants the rest of their routing to run, and the error log says what happened.
// ---------------------------------------------------------------------------

namespace {

class PluginFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate;
        return true;      // the plugin is loaded by Configure, which knows which
    }

    void Configure(const Block& b) override
    {
        const bool changed = !m_plugin ||
                             b.plugPath != m_path || b.plugUid != m_uid;
        if (changed)
        {
            m_plugin.reset();
            m_path = b.plugPath;
            m_uid = b.plugUid;

            if (m_path.empty()) return;

            PluginDesc d;
            d.format = b.plugFormat == 1 ? PlugFormat::Vst3 : PlugFormat::Clap;
            d.path = b.plugPath;
            d.uid = b.plugUid;
            d.name = b.plugName;

            std::wstring why;
            m_plugin = LoadPlugin(d, &why);
            if (!m_plugin)
            {
                LogError(FormatW(L"%s could not be loaded, so that block is doing "
                                 L"nothing. %s",
                                 b.plugName.empty() ? b.plugPath.c_str() : b.plugName.c_str(),
                                 why.c_str()));
                return;
            }
            if (!m_plugin->Start(m_rate, kMaxRenderBlock, &why))
            {
                LogError(FormatW(L"%s would not start, so that block is doing "
                                 L"nothing. %s", d.name.c_str(), why.c_str()));
                m_plugin.reset();
                return;
            }
        }

        if (!m_plugin) return;
        for (const auto& kv : b.plugParams) m_plugin->SetParam(kv.first, kv.second);
    }

    void Process(float* buf, size_t frames) override
    {
        if (!m_plugin) return;
        const float wet = m_wet.load();

        if (wet >= 1.0f)
        {
            m_plugin->Process(buf, frames);
            return;
        }
        // A plugin with latency cannot be crossfaded against the dry signal
        // without the two drifting apart, so mix is all or nothing here: it is
        // the bypass the engine uses when a block is switched off, and a
        // half-mixed plugin is not something the UI offers.
        if (wet <= 0.0f) return;
        m_plugin->Process(buf, frames);
    }

    int LatencyFrames() const override
    {
        return m_plugin ? m_plugin->LatencyFrames() : 0;
    }

    PluginInstance* Instance() { return m_plugin.get(); }

private:
    std::unique_ptr<PluginInstance> m_plugin;
    std::wstring m_path, m_uid;
    int m_rate = kDefaultRate;
};

}   // namespace

std::unique_ptr<Effect> MakePluginEffect()
{
    return std::unique_ptr<Effect>(new PluginFx());
}
