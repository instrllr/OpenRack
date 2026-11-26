#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include "Sandbox.h"
#include "ChannelEq.h"
#include "MidiControl.h"
#include "RemotePlugin.h"

static constexpr int kMaxInputBuses = 64;                    // stereo inputs/outputs a DAW can patch
static constexpr int kMaxRacks = 64;                         // stereo racks, like SuperRack Performer
static constexpr int kGraphChannels = kMaxInputBuses * 2;    // 128 channels in and out
static constexpr int kNumSnapshots = 8;

//==============================================================================
// Gain stage with mute and peak metering. Used as each rack's fader, and (at
// unity) as its input meter. Two sets of peaks so two meters can watch it.
class GainProcessor : public juce::AudioProcessor
{
public:
    GainProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("In", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "Gain"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        b.applyGain(muted.load(std::memory_order_relaxed) ? 0.0f : gain.load(std::memory_order_relaxed));

        const int n = b.getNumSamples();
        if (b.getNumChannels() > 0)
        {
            const float m = b.getMagnitude(0, 0, n);
            raise(peakL, m);
            raise(listPeakL, m);
        }
        if (b.getNumChannels() > 1)
        {
            const float m = b.getMagnitude(1, 0, n);
            raise(peakR, m);
            raise(listPeakR, m);
        }
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    std::atomic<float> gain { 1.0f };
    std::atomic<bool> muted { false };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };               // read by the main meters
    std::atomic<float> listPeakL { 0.0f }, listPeakR { 0.0f };       // read by the rack list

private:
    static void raise(std::atomic<float>& a, float v)
    {
        if (v > a.load(std::memory_order_relaxed))
            a.store(v, std::memory_order_relaxed);
    }
};

//==============================================================================
// The global tempo the standalone app reports to every hosted plugin (tempo-synced delays, LFOs ...).
struct TempoPlayHead : juce::AudioPlayHead
{
    std::atomic<double> bpm { 120.0 };
    std::atomic<double> ppq { 0.0 };
    std::atomic<juce::int64> samples { 0 };
    std::atomic<double> rate { 44100.0 };

    void advance(int n, double sr)
    {
        rate.store(sr);
        ppq.store(ppq.load() + (double) n / sr * bpm.load() / 60.0);
        samples.store(samples.load() + n);
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        const double p = ppq.load();
        TimeSignature ts;
        ts.numerator = 4;
        ts.denominator = 4;

        info.setBpm(bpm.load());
        info.setTimeSignature(ts);
        info.setIsPlaying(true);
        info.setIsRecording(false);
        info.setIsLooping(false);
        info.setTimeInSamples(samples.load());
        info.setTimeInSeconds((double) samples.load() / juce::jmax(1.0, rate.load()));
        info.setPpqPosition(p);
        info.setPpqPositionOfLastBarStart(std::floor(p / 4.0) * 4.0);
        return info;
    }
};

//==============================================================================
// Floating window that hosts a plugin's editor.
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow(juce::AudioProcessor& p, std::function<void()> onCloseFn)
        : DocumentWindow(p.getName(), juce::Colours::darkgrey, DocumentWindow::closeButton),
          onClose(std::move(onCloseFn))
    {
        juce::AudioProcessorEditor* ed = p.createEditorIfNeeded();
        if (ed == nullptr)
            ed = new juce::GenericAudioProcessorEditor(p);

        setUsingNativeTitleBar(true);
        setContentOwned(ed, true);
        setResizable(ed->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { onClose(); }

private:
    std::function<void()> onClose;
};

//==============================================================================
// The engine: up to 64 stereo racks, each with its own plugin chain and fader,
// patched from any stereo input pair to any stereo output pair of a 128-channel
// graph. Two modes:
//  - ownsAudioDevice = true : the standalone app; the engine opens the audio
//    interface and input/output pairs are the interface's channels.
//  - ownsAudioDevice = false: the plugin; pairs are the DAW's input/output buses.
//
// The "selected rack" is the one the plugin list, chain view and fader act on.
class RackEngine : private MidiHost
{
public:
    // Notices which parameter of a plugin you grab in its editor (for the parameter fader).
    struct ParamWatcher : juce::AudioProcessorParameter::Listener
    {
        ParamWatcher(juce::AudioProcessorGraph::Node::Ptr n, std::function<void(juce::AudioProcessor*, int)> cb)
            : node(std::move(n)), callback(std::move(cb))
        {
            for (auto* p : node->getProcessor()->getParameters())
                p->addListener(this);
        }

        ~ParamWatcher() override
        {
            for (auto* p : node->getProcessor()->getParameters())
                p->removeListener(this);
        }

        void parameterValueChanged(int, float) override {}
        void parameterGestureChanged(int index, bool starting) override
        {
            if (starting && callback)
                callback(node->getProcessor(), index);
        }

        juce::AudioProcessorGraph::Node::Ptr node;      // keeps the plugin alive until we have unregistered
        std::function<void(juce::AudioProcessor*, int)> callback;
    };

    struct Slot
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::PluginDescription desc;
        std::shared_ptr<ParamWatcher> watcher;
    };

    // One stereo input bus of the plugin version, with its own mixer strip.
    struct InputBus
    {
        std::atomic<float> gain { 1.0f };
        std::atomic<bool> mute { false }, solo { false }, active { false };
        std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    };

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioDeviceManager deviceManager;     // only used when ownsAudioDevice
    PluginSafety safety { getDataDirectory() }; // block list, crash marker, pre-flight cache
    std::array<InputBus, kMaxInputBuses> inputBuses;

    std::function<void()> onChanged;            // racks or chains changed (message thread)
    std::function<void(const juce::String&)> onMessage;     // status messages (plugin crashed / restarted ...)

    //--------------------------------------------------------------------------
    // Global tempo (standalone app): what hosted plugins see as the host tempo. Can be set from
    // the toolbar, tapped, followed from MIDI clock or driven by a MIDI controller.
    std::function<void()> onTempoChanged;

    double getTempo() const { return tempoHead.bpm.load(); }

    void setTempo(double bpm)
    {
        bpm = std::round(juce::jlimit(tempo::kMin, tempo::kMax, bpm));   // whole BPM only
        if (std::abs(bpm - tempoHead.bpm.load()) < 1.0e-6)
            return;
        tempoHead.bpm.store(bpm);
        markDirty();
        if (onTempoChanged)
            onTempoChanged();
    }

    void tapTempo()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (! taps.empty() && now - taps.back() > 2000.0)
            taps.clear();
        taps.push_back(now);
        while (taps.size() > 6)
            taps.erase(taps.begin());
        if (taps.size() >= 2)
            setTempo(60000.0 / ((taps.back() - taps.front()) / (double) (taps.size() - 1)));
    }

    //--------------------------------------------------------------------------
    // Parameter faders: a fader that moves one parameter of one plugin in a rack.
    static constexpr int kNumParamFaders = 1;

    struct ParamFader
    {
        bool on = false;
        juce::uint32 rackId = 0;
        int slot = -1, param = -1;
        juce::String plugin, name;
    };

    struct ParamSource
    {
        juce::uint32 rackId = 0;
        int slot = 0;
        juce::String label;
    };

    std::function<void()> onParamFadersChanged;

    // The fader follows the last parameter you touch in any plugin's editor, unless it is locked
    // (choosing a parameter by hand locks it; unlock to follow again).
    bool isParamFaderLocked() const { return paramLocked; }
    void setParamFaderLocked(bool on)
    {
        paramLocked = on;
        markDirty();
        if (onParamFadersChanged) onParamFadersChanged();
    }

    void onParamTouched(juce::AudioProcessor* p, int paramIndex)
    {
        if (paramLocked)
            return;
        for (auto& r : racks)
            for (size_t k = 0; k < r->slots.size(); ++k)
                if (r->slots[k].node->getProcessor() == p)
                {
                    const auto& pf = paramFaders[0];
                    if (! (pf.on && pf.rackId == r->id && pf.slot == (int) k && pf.param == paramIndex))
                        assignParamFader(0, r->id, (int) k, paramIndex, false);
                    return;
                }
    }

    const ParamFader& getParamFaderInfo(int i) const { return paramFaders[(size_t) juce::jlimit(0, kNumParamFaders - 1, i)]; }

    // Every plugin in every rack, for the parameter picker.
    std::vector<ParamSource> listParamSources() const
    {
        std::vector<ParamSource> out;
        for (auto& r : racks)
            for (size_t k = 0; k < r->slots.size(); ++k)
                out.push_back({ r->id, (int) k, r->name + "  -  " + juce::String((int) k + 1) + ": " + r->slots[k].desc.name });
        return out;
    }

    // Null for a plugin that runs in its own helper process (its parameters live over there).
    juce::AudioProcessor* getProcessorFor(juce::uint32 rackId, int slot) const
    {
        for (auto& r : racks)
            if (r->id == rackId && slot >= 0 && slot < (int) r->slots.size())
            {
                auto* p = r->slots[(size_t) slot].node->getProcessor();
                return dynamic_cast<remote::RemotePlugin*>(p) != nullptr ? nullptr : p;
            }
        return nullptr;
    }

    juce::AudioProcessorParameter* resolveParamFader(int i) const
    {
        if (i < 0 || i >= kNumParamFaders) return nullptr;
        const auto& pf = paramFaders[(size_t) i];
        if (! pf.on) return nullptr;

        for (auto& r : racks)
        {
            if (r->id != pf.rackId) continue;

            int idx = pf.slot;
            if (idx < 0 || idx >= (int) r->slots.size() || r->slots[(size_t) idx].desc.name != pf.plugin)
            {
                idx = -1;       // the chain was reordered: find the plugin again by name
                for (size_t k = 0; k < r->slots.size(); ++k)
                    if (r->slots[k].desc.name == pf.plugin) { idx = (int) k; break; }
            }
            if (idx < 0) return nullptr;

            auto* proc = getProcessorFor(r->id, idx);
            if (proc == nullptr) return nullptr;
            const auto& params = proc->getParameters();
            return pf.param < params.size() ? params[pf.param] : nullptr;
        }
        return nullptr;
    }

    float getParamFaderValue(int i) const
    {
        auto* p = resolveParamFader(i);
        return p != nullptr ? p->getValue() : -1.0f;
    }

    juce::String getParamFaderText(int i) const
    {
        auto* p = resolveParamFader(i);
        if (p == nullptr) return {};
        return (p->getText(p->getValue(), 12) + " " + p->getLabel()).trim();
    }

    void setParamFaderValue(int i, float v)
    {
        if (auto* p = resolveParamFader(i))
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, v));
    }

    void assignParamFader(int i, juce::uint32 rackId, int slot, int param, bool lock = true)
    {
        auto* proc = getProcessorFor(rackId, slot);
        if (i < 0 || i >= kNumParamFaders || proc == nullptr || param < 0 || param >= proc->getParameters().size())
            return;

        auto& pf = paramFaders[(size_t) i];
        pf.on = true;
        pf.rackId = rackId;
        pf.slot = slot;
        pf.param = param;
        for (auto& r : racks)
            if (r->id == rackId) pf.plugin = r->slots[(size_t) slot].desc.name;
        pf.name = proc->getParameters()[param]->getName(40);
        if (lock)
            paramLocked = true;

        markDirty();
        if (onParamFadersChanged) onParamFadersChanged();
    }

    void clearParamFader(int i)
    {
        if (i < 0 || i >= kNumParamFaders) return;
        paramFaders[(size_t) i] = ParamFader();
        markDirty();
        if (onParamFadersChanged) onParamFadersChanged();
    }

    //--------------------------------------------------------------------------
    // Audio dropout monitoring (standalone app). A dropout is counted when a callback takes longer
    // than the time its block represents, when the gap between two callbacks is much longer than
    // one block, or when the driver itself reports an xrun.
    int getDropouts() const { return dropouts.load(); }
    int getPeakLoadPercent() const { return (int) std::lround(peakLoad.load() * 100.0f); }
    void resetDropouts() { dropouts.store(0); peakLoad.store(0.0f); lastXruns = -1; }

    // Message thread, every ~0.5 s. Announces new dropouts and brings a lost audio / MIDI device back.
    void pollDevices()
    {
        if (! ownsDevice)
            return;

        const int d = dropouts.load();
        if (d > shownDropouts)
        {
            if (onMessage)
                onMessage("Audio dropout! (" + juce::String(d) + " so far) - raise the buffer size or lighten the racks");
            shownDropouts = d;
        }
        else if (d < shownDropouts)
            shownDropouts = d;

        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            const int x = dev->getXRunCount();
            if (x >= 0)
            {
                if (lastXruns >= 0 && x > lastXruns)
                    dropouts.fetch_add(x - lastXruns);
                lastXruns = x;
            }
            wantedSetup = deviceManager.getAudioDeviceSetup();
            deviceLost = false;
            missingTicks = 0;
        }
        else if (wantedSetup.outputDeviceName.isNotEmpty() || wantedSetup.inputDeviceName.isNotEmpty())
        {
            if (! deviceLost)
            {
                deviceLost = true;
                if (onMessage)
                    onMessage("Audio device disconnected - waiting for it to come back");
            }

            if (++missingTicks % 4 == 0)   // about every 2 s
                tryReconnectAudio();
        }

        if (++midiTicks % 4 == 0)
        {
            const auto m = midi.pollReconnect();
            if (m.isNotEmpty() && onMessage)
                onMessage(m);
        }
    }

    bool isAudioDeviceLost() const { return deviceLost; }

    // Out-of-process hosting (standalone app): run newly loaded plugins in their own helper process.
    bool getIsolatePlugins() const { return isolatePlugins.load(); }

    void setIsolatePlugins(bool on)
    {
        isolatePlugins.store(on);
        auto f = getDataDirectory().getChildFile("isolate.flag");
        f.getParentDirectory().createDirectory();
        f.replaceWithText(on ? "1" : "0");
    }
    MidiController midi { *this, getDataDirectory().getChildFile("midi.xml") };   // hardware control surfaces

    std::function<void()> onDirty;              // a setting changed that should be saved (light-weight)
    std::function<void()> onLatencyChanged;     // total latency may have changed
    std::function<void(int)> onOpenEditorRequested;           // plugin mode: show a slot's editor inline
    std::function<void(juce::uint32)> onBeforeSlotRemoved;    // a hosted plugin is about to be deleted

    static juce::File getDataDirectory()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("OpenRack");
    }

    explicit RackEngine(bool ownsAudioDevice = true) : ownsDevice(ownsAudioDevice)
    {
        formatManager.addDefaultFormats();
        suspectAtStartup = safety.takeStaleMarker();

        graph.setPlayConfigDetails(kGraphChannels, kGraphChannels, sampleRate, blockSize);
        audioIn = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
        audioOut = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

        createRack("Rack 1", 0, 0);
        rebuildConnections();
        work.setSize(kGraphChannels, blockSize);

        if (ownsDevice)
        {
            numInputPairs = 0;
            numOutputPairs = 0;
            deviceCallback = std::make_unique<DeviceCallback>(*this);
            deviceManager.initialise(kGraphChannels, kGraphChannels, nullptr, true);
            deviceManager.addAudioCallback(deviceCallback.get());
        }

        if (ownsDevice)
            graph.setPlayHead(&tempoHead);

        if (ownsDevice)
            isolatePlugins.store(getDataDirectory().getChildFile("isolate.flag").loadFileAsString().trim() == "1");

        // Open the saved MIDI devices once the message loop is running.
        std::weak_ptr<int> alive = aliveToken;
        juce::MessageManager::callAsync([this, alive] { if (alive.lock() != nullptr) midi.start(); });
    }

    ~RackEngine()
    {
        midi.stop();
        aliveToken.reset();     // any plugin still loading will notice and give up
        windows.clear();

        if (ownsDevice)
            deviceManager.removeAudioCallback(deviceCallback.get());
    }

    bool ownsAudioDevice() const { return ownsDevice; }

    //--------------------------------------------------------------------------
    // Plugin mode: called by the host.
    void prepare(double sr, int bs)
    {
        sampleRate = sr;
        blockSize = bs;
        prepareGraph(sr, bs);
    }

    void release() { graph.releaseResources(); }

    double getSampleRate() const { return sampleRate; }
    int getBlockSize() const { return blockSize; }

    // How many stereo input / output pairs can currently be patched.
    int getNumInputPairs() const { return numInputPairs.load(); }
    int getNumOutputPairs() const { return numOutputPairs.load(); }

    //--------------------------------------------------------------------------
    // Runs one block through all racks. `in` / `out` are per-channel pointers (any
    // may be null). `gains`, if given, is a per-input-channel gain (the input mixer).
    void processMulti(const float* const* in, int numIn, float* const* out, int numOut,
                      int numSamples, juce::MidiBuffer& midi, const float* gains)
    {
        const int cap = juce::jmax(1, cap_);

        for (int start = 0; start < numSamples; start += cap)
        {
            const int c = juce::jmin(cap, numSamples - start);

            work.setSize(kGraphChannels, c, false, false, true);
            work.clear();

            for (int ch = 0; ch < juce::jmin(numIn, kGraphChannels); ++ch)
                if (in[ch] != nullptr)
                {
                    work.copyFrom(ch, 0, in[ch] + start, c);
                    if (gains != nullptr && gains[ch] != 1.0f)
                        work.applyGain(ch, 0, c, gains[ch]);
                }

            graph.processBlock(work, midi);
            if (ownsDevice)
                tempoHead.advance(c, sampleRate);

            // Master: one gain for every output, with its own meter (even channels = left, odd = right).
            const float mg = masterGain.load(std::memory_order_relaxed);
            float peakL = 0.0f, peakR = 0.0f;
            for (int ch = 0; ch < kGraphChannels; ++ch)
            {
                if (mg != 1.0f)
                    work.applyGain(ch, 0, c, mg);
                const float m = work.getMagnitude(ch, 0, c);
                (ch & 1 ? peakR : peakL) = juce::jmax(ch & 1 ? peakR : peakL, m);
            }
            raisePeak(masterPeakL, peakL);
            raisePeak(masterPeakR, peakR);

            for (int ch = 0; ch < numOut; ++ch)
                if (out[ch] != nullptr)
                {
                    if (ch < kGraphChannels)
                        juce::FloatVectorOperations::copy(out[ch] + start, work.getReadPointer(ch), c);
                    else
                        juce::FloatVectorOperations::clear(out[ch] + start, c);
                }
        }
    }

    // Plugin mode: mixes the host's input buses (gain / mute / solo per bus) and
    // runs the racks, writing every enabled output bus.
    void processHostBuffer(juce::AudioProcessor& host, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        const int n = buffer.getNumSamples();
        const float* inPtrs[kGraphChannels] = {};
        float* outPtrs[kGraphChannels] = {};
        float gains[kGraphChannels];
        for (auto& g : gains) g = 1.0f;

        const int numIns = juce::jmin(host.getBusCount(true), kMaxInputBuses);
        const int numOuts = juce::jmin(host.getBusCount(false), kMaxInputBuses);

        bool anySolo = false;
        for (int i = 0; i < kMaxInputBuses; ++i)
        {
            auto& b = inputBuses[(size_t) i];
            const bool on = i < numIns && host.getBus(true, i)->isEnabled();
            b.active.store(on, std::memory_order_relaxed);
            if (on && b.solo.load(std::memory_order_relaxed))
                anySolo = true;
        }

        for (int i = 0; i < numIns; ++i)
        {
            auto& b = inputBuses[(size_t) i];
            if (! b.active.load(std::memory_order_relaxed))
                continue;

            auto bb = host.getBusBuffer(buffer, true, i);
            const bool audible = ! b.mute.load() && (! anySolo || b.solo.load());
            const float g = audible ? b.gain.load() : 0.0f;

            for (int c = 0; c < juce::jmin(2, bb.getNumChannels()); ++c)
            {
                inPtrs[2 * i + c] = bb.getReadPointer(c);
                gains[2 * i + c] = g;
            }

            if (n > 0 && bb.getNumChannels() >= 2)
            {
                raisePeak(b.peakL, bb.getMagnitude(0, 0, n));
                raisePeak(b.peakR, bb.getMagnitude(1, 0, n));
            }
        }

        for (int j = 0; j < numOuts; ++j)
        {
            if (! host.getBus(false, j)->isEnabled())
                continue;
            auto bb = host.getBusBuffer(buffer, false, j);
            for (int c = 0; c < juce::jmin(2, bb.getNumChannels()); ++c)
                outPtrs[2 * j + c] = bb.getWritePointer(c);
        }

        // Inputs are copied before outputs are written, so shared channels are safe.
        processMulti(inPtrs, kGraphChannels, outPtrs, kGraphChannels, n, midi, gains);
    }

    // Longest chain latency of any rack, so the host can compensate.
    int getTotalLatency() const
    {
        int worst = 0;
        for (auto& r : racks)
        {
            int total = 0;
            for (auto& s : r->slots)
                if (! s.node->isBypassed())
                    total += s.node->getProcessor()->getLatencySamples();
            worst = juce::jmax(worst, total);
        }
        return worst;
    }

    double getTotalTailSeconds() const
    {
        double worst = 0.0;
        for (auto& r : racks)
        {
            double total = 0.0;
            for (auto& s : r->slots)
                total += s.node->getProcessor()->getTailLengthSeconds();
            worst = juce::jmax(worst, total);
        }
        return worst;
    }

    //--------------------------------------------------------------------------
    // Racks
    int getNumRacks() const { return (int) racks.size(); }
    int getSelectedRack() const { return selected; }

    // The built-in channel-strip EQ of a rack (null if there is no such rack).
    ChannelEq* getEq(int i) { return valid(i) ? &eqOf(*racks[(size_t) i]) : nullptr; }
    void markDirty() { if (onDirty) onDirty(); }

    void selectRack(int i)
    {
        if (i < 0 || i >= getNumRacks() || i == selected) return;
        selected = i;
        if (onChanged) onChanged();
    }

    bool addRack()
    {
        if (getNumRacks() >= kMaxRacks) return false;
        const int idx = getNumRacks();
        createRack("Rack " + juce::String(idx + 1),
                   idx < numInputPairs.load() ? idx : -1,
                   idx < numOutputPairs.load() ? idx : -1);
        selected = idx;
        rebuildConnections();
        applyMuteSolo();
        notify();
        return true;
    }

    void removeRack(int i)
    {
        if (getNumRacks() <= 1 || i < 0 || i >= getNumRacks()) return;
        destroyRack(i);
        if (selected >= getNumRacks()) selected = getNumRacks() - 1;
        else if (i < selected) --selected;
        rebuildConnections();
        applyMuteSolo();
        notify();
    }

    //--------------------------------------------------------------------------
    // Groups: named tabs that racks can be filed under (group id 0 = no group).
    int getNumGroups() const { return (int) groups.size(); }
    juce::uint32 getGroupId(int i) const { return groups[(size_t) i].id; }
    juce::String getGroupName(int i) const { return groups[(size_t) i].name; }

    juce::uint32 addGroup(const juce::String& name)
    {
        const auto id = nextGroupId++;
        groups.push_back({ id, name.trim().isEmpty() ? "Group " + juce::String(id) : name.trim() });
        notify();
        return id;
    }

    void renameGroup(juce::uint32 id, const juce::String& name)
    {
        for (auto& g : groups)
            if (g.id == id && name.trim().isNotEmpty())
                g.name = name.trim();
        notify();
    }

    // Deleting a group keeps its racks; they just become ungrouped.
    void removeGroup(juce::uint32 id)
    {
        for (auto& r : racks)
            if (r->groupId == id) r->groupId = 0;
        groups.erase(std::remove_if(groups.begin(), groups.end(), [id](const Group& g) { return g.id == id; }),
                     groups.end());
        notify();
    }

    //--------------------------------------------------------------------------
    // Direct routes: an input pair straight to an output pair, without a rack
    // (e.g. passing a SoundGrid channel through). Several inputs to one output are summed.
    bool hasDirect(int inPair, int outPair) const
    {
        return inPair >= 0 && inPair < kMaxInputBuses && outPair >= 0 && outPair < kMaxInputBuses
               && direct[inPair][outPair];
    }

    void setDirect(int inPair, int outPair, bool on)
    {
        if (inPair < 0 || inPair >= kMaxInputBuses || outPair < 0 || outPair >= kMaxInputBuses) return;
        direct[inPair][outPair] = on;
        rebuildConnections();
        notify();
    }

    void clearDirect()
    {
        for (auto& row : direct)
            for (auto& v : row) v = false;
        rebuildConnections();
        notify();
    }

    // Colour used to colour-code a rack (ARGB; 0 = automatic).
    juce::uint32 getRackColour(int i) const { return valid(i) ? racks[(size_t) i]->colour : 0; }

    void setRackColour(int i, juce::uint32 argb)
    {
        if (! valid(i)) return;
        racks[(size_t) i]->colour = argb;
        notify();
    }

    juce::uint32 getRackGroup(int i) const { return valid(i) ? racks[(size_t) i]->groupId : 0; }

    void setRackGroup(int i, juce::uint32 groupId)
    {
        if (! valid(i)) return;
        racks[(size_t) i]->groupId = groupExists(groupId) ? groupId : 0;
        notify();
    }

    juce::String getRackName(int i) const { return valid(i) ? racks[(size_t) i]->name : juce::String(); }
    int getRackNumSlots(int i) const { return valid(i) ? (int) racks[(size_t) i]->slots.size() : 0; }
    int getRackInputPair(int i) const { return valid(i) ? racks[(size_t) i]->inPair : -1; }
    int getRackOutputPair(int i) const { return valid(i) ? racks[(size_t) i]->outPair : -1; }
    bool isRackMuted(int i) const { return valid(i) && racks[(size_t) i]->mute; }
    bool isRackSoloed(int i) const { return valid(i) && racks[(size_t) i]->solo; }
    float getRackGain(int i) const { return valid(i) ? faderOf(*racks[(size_t) i]).gain.load() : 1.0f; }

    void renameRack(int i, const juce::String& name)
    {
        if (! valid(i)) return;
        racks[(size_t) i]->name = name.trim().isEmpty() ? "Rack " + juce::String(i + 1) : name.trim();
        notify();
    }

    void setRackInputPair(int i, int pair)
    {
        if (! valid(i)) return;
        racks[(size_t) i]->inPair = (pair >= 0 && pair < kMaxInputBuses) ? pair : -1;
        rebuildConnections();
        notify();
    }

    void setRackOutputPair(int i, int pair)
    {
        if (! valid(i)) return;
        racks[(size_t) i]->outPair = (pair >= 0 && pair < kMaxInputBuses) ? pair : -1;
        rebuildConnections();
        notify();
    }

    void setRackGain(int i, float g) { if (valid(i)) faderOf(*racks[(size_t) i]).gain.store(g); }

    void setRackMute(int i, bool b)
    {
        if (! valid(i)) return;
        racks[(size_t) i]->mute = b;
        applyMuteSolo();
        if (onChanged) onChanged();
    }

    void setRackSolo(int i, bool b)
    {
        if (! valid(i)) return;
        racks[(size_t) i]->solo = b;
        applyMuteSolo();
        if (onChanged) onChanged();
    }

    // Level of a rack's output since the last call, for the rack list.
    float takeRackPeak(int i, int channel)
    {
        if (! valid(i)) return 0.0f;
        auto& f = faderOf(*racks[(size_t) i]);
        return (channel == 0 ? f.listPeakL : f.listPeakR).exchange(0.0f);
    }

    //--------------------------------------------------------------------------
    // The selected rack: its fader and meters
    float getFaderGain() const { return getRackGain(selected); }
    void setFaderGain(float g) { setRackGain(selected, g); }

    // The master: one fader over every output, with its own meter.
    float getMasterGain() const { return masterGain.load(); }
    void setMasterGain(float g) { masterGain.store(g); }

    float takeMasterPeak(int channel)
    {
        return (channel == 0 ? masterPeakL : masterPeakR).exchange(0.0f);
    }

    float takeOutputPeak(int channel)
    {
        auto& f = faderOf(*racks[(size_t) selected]);
        return (channel == 0 ? f.peakL : f.peakR).exchange(0.0f);
    }

    float takeInputPeak(int channel)
    {
        auto& g = *static_cast<GainProcessor*>(racks[(size_t) selected]->inNode->getProcessor());
        return (channel == 0 ? g.peakL : g.peakR).exchange(0.0f);
    }

    //--------------------------------------------------------------------------
    // The selected rack's plugin chain
    int getNumSlots() const { return (int) cur().slots.size(); }
    const Slot& getSlot(int i) const { return cur().slots[(size_t) i]; }

    // Adds a plugin to the selected rack. `done` gets an empty string on success.
    void addPlugin(const juce::PluginDescription& d,
                   std::function<void(const juce::String&)> done = {},
                   juce::MemoryBlock state = {},
                   bool bypassed = false)
    {
        addPluginTo(cur().id, d, std::move(done), std::move(state), bypassed, -1);
    }

    // Same, but placed at a given position in the chain.
    void addPluginAt(const juce::PluginDescription& d, int insertAt,
                     std::function<void(const juce::String&)> done = {})
    {
        addPluginTo(cur().id, d, std::move(done), {}, false, insertAt);
    }

    void removeSlot(int i)
    {
        auto& r = cur();
        if (i < 0 || i >= (int) r.slots.size()) return;
        removeSlotFrom(r, i);
        rebuildConnections();
        notify();
    }

    void moveSlot(int i, int delta)
    {
        auto& r = cur();
        int j = i + delta;
        if (i < 0 || j < 0 || i >= (int) r.slots.size() || j >= (int) r.slots.size()) return;
        std::swap(r.slots[(size_t) i], r.slots[(size_t) j]);
        rebuildConnections();
        notify();
    }

    // Moves slot `from` so it sits before slot `insertBefore` (0..numSlots) - used by drag and drop.
    void moveSlotTo(int from, int insertBefore)
    {
        auto& r = cur();
        const int n = (int) r.slots.size();
        if (from < 0 || from >= n || insertBefore < 0 || insertBefore > n) return;

        int to = insertBefore > from ? insertBefore - 1 : insertBefore;
        if (to == from) return;

        auto s = r.slots[(size_t) from];
        r.slots.erase(r.slots.begin() + from);
        r.slots.insert(r.slots.begin() + to, s);
        rebuildConnections();
        notify();
    }

    void setBypassed(int i, bool b)
    {
        if (i >= 0 && i < getNumSlots())
        {
            cur().slots[(size_t) i].node->setBypassed(b);
            notify();
        }
    }

    void setAllBypassed(bool b)
    {
        for (auto& s : cur().slots)
            s.node->setBypassed(b);
        notify();
    }

    bool areAllBypassed() const
    {
        if (cur().slots.empty()) return false;
        for (auto& s : cur().slots)
            if (! s.node->isBypassed()) return false;
        return true;
    }

    // Empties the selected rack.
    void clearRack()
    {
        auto& r = cur();
        while (! r.slots.empty())
            removeSlotFrom(r, 0);
        rebuildConnections();
        notify();
    }

    void openEditor(int i)
    {
        if (i < 0 || i >= getNumSlots()) return;

        auto node = cur().slots[(size_t) i].node;
        auto uid = node->nodeID.uid;

        // From now on, grabbing a control in this plugin's editor selects it for the parameter fader.
        // (Attached when an editor is opened rather than when a plugin loads, so loading stays untouched.)
        if (cur().slots[(size_t) i].watcher == nullptr && dynamic_cast<remote::RemotePlugin*>(node->getProcessor()) == nullptr)
        {
            std::weak_ptr<int> alive = aliveToken;
            cur().slots[(size_t) i].watcher = std::make_shared<ParamWatcher>(node, [this, alive](juce::AudioProcessor* p, int idx)
            {
                juce::MessageManager::callAsync([this, alive, p, idx]
                {
                    if (! alive.expired())
                        onParamTouched(p, idx);
                });
            });
        }

        auto* rp = dynamic_cast<remote::RemotePlugin*>(node->getProcessor());
        if (rp != nullptr && rp->getState() == remote::RemotePlugin::State::failed)
        {
            rp->restart();      // it gave up after repeated crashes; try once more on request
            return;
        }

        // The editor is shown inside OpenRack's own window. (A plugin running in a separate helper
        // process has its window in that process, so that one stays a floating window.)
        if (onOpenEditorRequested && rp == nullptr)
        {
            onOpenEditorRequested(i);
            return;
        }

        if (windows.count(uid) != 0)
        {
            windows[uid]->toFront(true);
            return;
        }

        windows[uid] = std::make_unique<PluginWindow>(*node->getProcessor(), [this, uid]
        {
            juce::MessageManager::callAsync([this, uid] { windows.erase(uid); });
        });
    }

    //--------------------------------------------------------------------------
    // Snapshots: eight stored sessions (all racks, patching, faders). If a snapshot
    // has the same plugins in the same racks the switch is instant (only settings
    // change); otherwise the plugins are reloaded.
    bool hasSnapshot(int i) const { return i >= 0 && i < kNumSnapshots && snapshots[(size_t) i] != nullptr; }
    int getCurrentSnapshot() const { return currentSnapshot; }

    void storeSnapshot(int i)
    {
        if (i < 0 || i >= kNumSnapshots) return;
        snapshots[(size_t) i].reset(createCoreXml().release());
        currentSnapshot = i;
        notify();
    }

    void clearSnapshot(int i)
    {
        if (! hasSnapshot(i)) return;
        snapshots[(size_t) i].reset();
        if (currentSnapshot == i) currentSnapshot = -1;
        notify();
    }

    void recallSnapshot(int i, std::function<void(const juce::String&)> done = {})
    {
        if (! hasSnapshot(i))
        {
            if (done) done("That snapshot is empty.");
            return;
        }

        auto xml = snapshots[(size_t) i];
        currentSnapshot = i;

        // Same racks with the same plugins in the same order? Then only settings change.
        std::vector<juce::XmlElement*> rackEls;
        for (auto* e : xml->getChildWithTagNameIterator("Rack"))
            rackEls.push_back(e);

        bool same = rackEls.size() == racks.size();
        for (size_t k = 0; same && k < rackEls.size(); ++k)
        {
            std::vector<juce::XmlElement*> slotEls;
            for (auto* s : rackEls[k]->getChildWithTagNameIterator("Slot"))
                slotEls.push_back(s);

            same = slotEls.size() == racks[k]->slots.size();
            for (size_t m = 0; same && m < slotEls.size(); ++m)
            {
                juce::PluginDescription d;
                auto* dx = slotEls[m]->getFirstChildElement();
                same = dx != nullptr && d.loadFromXml(*dx)
                       && d.createIdentifierString() == racks[k]->slots[m].desc.createIdentifierString();
            }
        }

        if (same)
        {
            applyGroupsXml(*xml);
            applyParamFadersXml(*xml);
            for (size_t k = 0; k < rackEls.size(); ++k)
            {
                auto& r = *racks[k];
                auto* re = rackEls[k];
                applyRackSettings(r, *re);

                size_t m = 0;
                for (auto* se : re->getChildWithTagNameIterator("Slot"))
                {
                    juce::MemoryBlock mb;
                    mb.fromBase64Encoding(se->getStringAttribute("state"));
                    if (mb.getSize() > 0)
                        r.slots[m].node->getProcessor()->setStateInformation(mb.getData(), (int) mb.getSize());
                    r.slots[m].node->setBypassed(se->getBoolAttribute("bypass"));
                    ++m;
                }
            }
            applyInputsXml(*xml);
            masterGain.store((float) xml->getDoubleAttribute("master", 1.0));
            if (xml->hasAttribute("tempo"))
                setTempo(xml->getDoubleAttribute("tempo", 120.0));
            applyMuteSolo();
            rebuildConnections();
            notify();
            if (done) done({});
            return;
        }

        loadCore(xml, std::move(done));
    }

    // The plugin that was loading when the last session crashed (it is now blocked).
    juce::String takeSuspectAtStartup()
    {
        auto s = suspectAtStartup;
        suspectAtStartup.clear();
        return s;
    }

    //--------------------------------------------------------------------------
    // Session state: every rack (plugins, settings, bypass, patching, fader), the
    // input mixer and the snapshots.
    std::unique_ptr<juce::XmlElement> createCoreXml() const
    {
        auto root = std::make_unique<juce::XmlElement>("Session");
        root->setAttribute("sel", selected);
        root->setAttribute("master", (double) masterGain.load());
        root->setAttribute("tempo", getTempo());

        auto* de = root->createNewChildElement("Direct");
        for (int i = 0; i < kMaxInputBuses; ++i)
            for (int o = 0; o < kMaxInputBuses; ++o)
                if (direct[i][o])
                {
                    auto* e = de->createNewChildElement("Route");
                    e->setAttribute("in", i);
                    e->setAttribute("out", o);
                }

        auto* pe = root->createNewChildElement("Params");
        pe->setAttribute("lock", paramLocked);
        for (int i = 0; i < kNumParamFaders; ++i)
        {
            const auto& pf = paramFaders[(size_t) i];
            if (! pf.on) continue;
            for (size_t k = 0; k < racks.size(); ++k)
                if (racks[k]->id == pf.rackId)
                {
                    auto* e = pe->createNewChildElement("P");
                    e->setAttribute("i", i);
                    e->setAttribute("rack", (int) k);        // position, since rack ids are not kept between sessions
                    e->setAttribute("slot", pf.slot);
                    e->setAttribute("param", pf.param);
                    e->setAttribute("plugin", pf.plugin);
                    e->setAttribute("name", pf.name);
                }
        }

        auto* ge = root->createNewChildElement("Groups");
        for (auto& g : groups)
        {
            auto* e = ge->createNewChildElement("Group");
            e->setAttribute("id", (int) g.id);
            e->setAttribute("name", g.name);
        }

        for (auto& r : racks)
        {
            auto* re = root->createNewChildElement("Rack");
            re->setAttribute("name", r->name);
            re->setAttribute("group", (int) r->groupId);
            if (r->colour != 0)
                re->setAttribute("color", juce::String::toHexString((int) r->colour));
            re->setAttribute("in", r->inPair);
            re->setAttribute("out", r->outPair);
            re->setAttribute("gain", (double) faderOf(*r).gain.load());
            re->setAttribute("mute", r->mute);
            re->setAttribute("solo", r->solo);
            re->addChildElement(eqOf(*r).createXml().release());

            for (auto& s : r->slots)
            {
                auto* e = re->createNewChildElement("Slot");
                e->addChildElement(s.desc.createXml().release());
                e->setAttribute("bypass", s.node->isBypassed());

                juce::MemoryBlock mb;
                s.node->getProcessor()->getStateInformation(mb);
                e->setAttribute("state", mb.toBase64Encoding());
            }
        }

        auto* inputs = root->createNewChildElement("Inputs");
        for (int i = 0; i < kMaxInputBuses; ++i)
        {
            auto& b = inputBuses[(size_t) i];
            if (b.gain.load() == 1.0f && ! b.mute.load() && ! b.solo.load())
                continue;
            auto* e = inputs->createNewChildElement("Bus");
            e->setAttribute("i", i);
            e->setAttribute("gain", (double) b.gain.load());
            e->setAttribute("mute", b.mute.load());
            e->setAttribute("solo", b.solo.load());
        }
        return root;
    }

    // Everything, including stored snapshots (this is what a DAW project saves).
    std::unique_ptr<juce::XmlElement> createRackXml() const
    {
        auto root = createCoreXml();
        root->setAttribute("snap", currentSnapshot);

        auto* snaps = root->createNewChildElement("Snapshots");
        for (int i = 0; i < kNumSnapshots; ++i)
            if (snapshots[(size_t) i] != nullptr)
            {
                auto* e = snaps->createNewChildElement("Snap");
                e->setAttribute("i", i);
                e->addChildElement(new juce::XmlElement(*snapshots[(size_t) i]));
            }
        return root;
    }

    bool saveRack(const juce::File& file) const
    {
        auto xml = createRackXml();
        return xml->writeTo(file);
    }

    void loadRackXml(std::shared_ptr<juce::XmlElement> xml,
                     std::function<void(const juce::String&)> done)
    {
        if (xml == nullptr || ! (xml->hasTagName("Session") || xml->hasTagName("Rack")))
        {
            if (done) done("Not a valid rack file");
            return;
        }

        for (auto& snap : snapshots)
            snap.reset();

        if (auto* snaps = xml->getChildByName("Snapshots"))
            for (auto* e : snaps->getChildWithTagNameIterator("Snap"))
            {
                const int i = e->getIntAttribute("i", -1);
                if (i >= 0 && i < kNumSnapshots && e->getFirstChildElement() != nullptr)
                    snapshots[(size_t) i] = toSession(*e->getFirstChildElement());
            }

        currentSnapshot = hasSnapshot(xml->getIntAttribute("snap", -1)) ? xml->getIntAttribute("snap", -1) : -1;
        loadCore(xml, std::move(done));
    }

    void loadRack(const juce::File& file, std::function<void(const juce::String&)> done)
    {
        std::shared_ptr<juce::XmlElement> xml(juce::parseXML(file).release());
        loadRackXml(xml, std::move(done));
    }

private:
    // MidiHost: what the MIDI controller drives.
    int midiNumRacks() const override { return getNumRacks(); }
    float midiGetFader(int r) const override { return getRackGain(r); }
    void midiSetFader(int r, float g) override { setRackGain(r, g); }
    bool midiGetMute(int r) const override { return isRackMuted(r); }
    void midiSetMute(int r, bool on) override { setRackMute(r, on); }
    bool midiGetSolo(int r) const override { return isRackSoloed(r); }
    void midiSetSolo(int r, bool on) override { setRackSolo(r, on); }
    float midiGetMaster() const override { return getMasterGain(); }
    void midiSetMaster(float g) override { setMasterGain(g); }
    bool midiHasSnapshot(int i) const override { return hasSnapshot(i); }
    int midiCurrentSnapshot() const override { return getCurrentSnapshot(); }
    void midiRecallSnapshot(int i) override { recallSnapshot(i); }
    float midiGetParam(int i) const override { return getParamFaderValue(i); }
    void midiSetParam(int i, float v) override { setParamFaderValue(i, v); }
    double midiGetTempo() const override { return getTempo(); }
    void midiSetTempo(double bpm) override { setTempo(bpm); }
    void midiTapTempo() override { tapTempo(); }

    struct Rack
    {
        juce::uint32 id = 0;
        juce::String name;
        juce::uint32 groupId = 0;
        juce::uint32 colour = 0;
        int inPair = -1, outPair = -1;
        bool mute = false, solo = false;
        juce::AudioProcessorGraph::Node::Ptr inNode, eqNode, faderNode;
        std::vector<Slot> slots;
    };

    struct Group
    {
        juce::uint32 id;
        juce::String name;
    };

    struct PendingSlot
    {
        juce::uint32 rackId;
        juce::XmlElement* element;
    };

    // The standalone app's audio callback: device channels in, racks, device channels out.
    struct DeviceCallback : juce::AudioIODeviceCallback
    {
        explicit DeviceCallback(RackEngine& e) : engine(e) {}

        void audioDeviceAboutToStart(juce::AudioIODevice* d) override { engine.deviceStarting(d); }

        void audioDeviceStopped() override
        {
            engine.numInputPairs = 0;
            engine.numOutputPairs = 0;
            lastStart = 0.0;
            warm = 0;
        }

        void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out,
                                              int numOut, int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            midi.clear();
            engine.processMulti(in, numIn, out, numOut, numSamples, midi, nullptr);
            const double t1 = juce::Time::getMillisecondCounterHiRes();

            const double budget = 1000.0 * (double) numSamples / juce::jmax(1.0, engine.sampleRate);
            const float load = (float) ((t1 - t0) / budget);
            if (load > engine.peakLoad.load())
                engine.peakLoad.store(load);

            bool bad = load > 1.0f;
            if (lastStart > 0.0 && t0 - lastStart > budget * 1.8 + 2.0)
                bad = true;                         // the driver called us late: a block was missed
            if (bad && warm > 50)                   // ignore start-up hiccups
                engine.dropouts.fetch_add(1);
            lastStart = t0;
            if (warm <= 50) ++warm;
        }

        RackEngine& engine;
        juce::MidiBuffer midi;
        double lastStart = 0.0;
        int warm = 0;
    };

    //--------------------------------------------------------------------------
    bool groupExists(juce::uint32 id) const
    {
        for (auto& g : groups)
            if (g.id == id) return true;
        return false;
    }

    void applyParamFadersXml(const juce::XmlElement& session)
    {
        for (auto& pf : paramFaders)
            pf = ParamFader();

        paramLocked = false;
        if (auto* pe = session.getChildByName("Params"))
        {
            paramLocked = pe->getBoolAttribute("lock", false);
            for (auto* e : pe->getChildWithTagNameIterator("P"))
            {
                const int i = e->getIntAttribute("i", -1);
                const int rackPos = e->getIntAttribute("rack", -1);
                if (i < 0 || i >= kNumParamFaders || rackPos < 0 || rackPos >= (int) racks.size())
                    continue;
                auto& pf = paramFaders[(size_t) i];
                pf.on = true;
                pf.rackId = racks[(size_t) rackPos]->id;
                pf.slot = e->getIntAttribute("slot", -1);
                pf.param = e->getIntAttribute("param", -1);
                pf.plugin = e->getStringAttribute("plugin");
                pf.name = e->getStringAttribute("name");
            }
        }

        if (onParamFadersChanged)
            onParamFadersChanged();
    }

    void applyGroupsXml(const juce::XmlElement& session)
    {
        for (auto& row : direct)
            for (auto& v : row) v = false;
        if (auto* de = session.getChildByName("Direct"))
            for (auto* e : de->getChildWithTagNameIterator("Route"))
            {
                const int i = e->getIntAttribute("in", -1), o = e->getIntAttribute("out", -1);
                if (i >= 0 && i < kMaxInputBuses && o >= 0 && o < kMaxInputBuses)
                    direct[i][o] = true;
            }

        groups.clear();
        nextGroupId = 1;
        if (auto* ge = session.getChildByName("Groups"))
            for (auto* e : ge->getChildWithTagNameIterator("Group"))
            {
                const auto id = (juce::uint32) e->getIntAttribute("id", 0);
                if (id == 0 || groupExists(id)) continue;
                groups.push_back({ id, e->getStringAttribute("name", "Group") });
                nextGroupId = juce::jmax(nextGroupId, id + 1);
            }
    }

    bool valid(int i) const { return i >= 0 && i < (int) racks.size(); }
    Rack& cur() { return *racks[(size_t) juce::jlimit(0, (int) racks.size() - 1, selected)]; }
    const Rack& cur() const { return *racks[(size_t) juce::jlimit(0, (int) racks.size() - 1, selected)]; }

    static ChannelEq& eqOf(const Rack& r) { return *static_cast<ChannelEq*>(r.eqNode->getProcessor()); }
    static GainProcessor& faderOf(const Rack& r) { return *static_cast<GainProcessor*>(r.faderNode->getProcessor()); }

    Rack* findRack(juce::uint32 id)
    {
        for (auto& r : racks)
            if (r->id == id) return r.get();
        return nullptr;
    }

    int createRack(const juce::String& name, int inPair, int outPair)
    {
        auto r = std::make_unique<Rack>();
        r->id = nextRackId++;
        r->name = name;
        r->inPair = inPair;
        r->outPair = outPair;
        r->inNode = graph.addNode(std::make_unique<GainProcessor>());
        r->eqNode = graph.addNode(std::make_unique<ChannelEq>());
        r->faderNode = graph.addNode(std::make_unique<GainProcessor>());
        racks.push_back(std::move(r));
        return (int) racks.size() - 1;
    }

    void removeSlotFrom(Rack& r, int i)
    {
        auto id = r.slots[(size_t) i].node->nodeID;
        if (onBeforeSlotRemoved) onBeforeSlotRemoved(id.uid);   // let the UI close its editor first
        windows.erase(id.uid);
        graph.removeNode(id);
        r.slots.erase(r.slots.begin() + i);
    }

    void destroyRack(int i)
    {
        auto& r = *racks[(size_t) i];
        while (! r.slots.empty())
            removeSlotFrom(r, 0);
        graph.removeNode(r.inNode->nodeID);
        graph.removeNode(r.eqNode->nodeID);
        graph.removeNode(r.faderNode->nodeID);
        racks.erase(racks.begin() + i);
    }

    void applyMuteSolo()
    {
        bool anySolo = false;
        for (auto& r : racks)
            anySolo = anySolo || r->solo;
        for (auto& r : racks)
            faderOf(*r).muted.store(r->mute || (anySolo && ! r->solo));
    }

    void applyRackSettings(Rack& r, const juce::XmlElement& e)
    {
        r.name = e.getStringAttribute("name", r.name);
        const auto gid = (juce::uint32) e.getIntAttribute("group", 0);
        r.groupId = groupExists(gid) ? gid : 0;
        r.colour = (juce::uint32) e.getStringAttribute("color").getHexValue32();
        r.inPair = e.getIntAttribute("in", -1);
        r.outPair = e.getIntAttribute("out", -1);
        r.mute = e.getBoolAttribute("mute");
        r.solo = e.getBoolAttribute("solo");
        faderOf(r).gain.store((float) e.getDoubleAttribute("gain", 1.0));

        if (auto* ex = e.getChildByName("Eq"))
            eqOf(r).loadXml(*ex);
        else
            eqOf(r).setDefaults();
    }

    void prepareGraph(double sr, int bs)
    {
        cap_ = juce::jmax(1, bs);
        work.setSize(kGraphChannels, cap_);
        graph.setPlayConfigDetails(kGraphChannels, kGraphChannels, sr, bs);
        graph.prepareToPlay(sr, bs);
    }

    void deviceStarting(juce::AudioIODevice* d)
    {
        sampleRate = d->getCurrentSampleRate();
        blockSize = d->getCurrentBufferSizeSamples();
        prepareGraph(sampleRate, blockSize);

        numInputPairs = (d->getActiveInputChannels().countNumberOfSetBits() + 1) / 2;
        numOutputPairs = (d->getActiveOutputChannels().countNumberOfSetBits() + 1) / 2;

        std::weak_ptr<int> alive = aliveToken;
        juce::MessageManager::callAsync([this, alive]
        {
            if (! alive.expired() && onChanged)
                onChanged();
        });
    }

    //--------------------------------------------------------------------------
    // Loads a plugin into the rack with the given id (the rack may be gone by the
    // time the plugin has loaded, in which case it is dropped).
    void addPluginTo(juce::uint32 rackId, const juce::PluginDescription& d,
                     std::function<void(const juce::String&)> done,
                     juce::MemoryBlock state, bool bypassed, int insertAt)
    {
        double sr = sampleRate;
        int bs = blockSize;
        if (ownsDevice)
        {
            if (auto* dev = deviceManager.getCurrentAudioDevice())
            {
                sr = dev->getCurrentSampleRate();
                bs = dev->getCurrentBufferSizeSamples();
            }
        }

        const auto pluginId = d.createIdentifierString();
        if (safety.isBlocked(pluginId))
        {
            if (done)
                done(d.name + " is blocked because it crashed earlier. Right-click it in the list "
                              "and choose \"Unblock plugin\" to try again.");
            return;
        }

        std::weak_ptr<int> alive = aliveToken;

        // Puts a loaded plugin (in-process or in a helper process) into the rack.
        auto finish = [this, alive, d, done, state, bypassed, rackId, insertAt]
                      (std::unique_ptr<juce::AudioProcessor> proc)
        {
            if (alive.expired())
                return;     // the engine was destroyed while this plugin was loading

            auto* rack = findRack(rackId);
            if (rack == nullptr)
            {
                if (done) done("That rack was removed while the plugin was loading.");
                return;
            }

            if (state.getSize() > 0)
                proc->setStateInformation(state.getData(), (int) state.getSize());

            auto node = graph.addNode(std::move(proc));
            node->setBypassed(bypassed);

            const int pos = (insertAt >= 0 && insertAt < (int) rack->slots.size())
                                ? insertAt : (int) rack->slots.size();
            Slot slot;
            slot.node = node;
            slot.desc = d;
            rack->slots.insert(rack->slots.begin() + pos, slot);

            rebuildConnections();
            notify();
            if (done) done({});
        };

        auto loadInProcess = [this, alive, d, done, sr, bs, pluginId, finish]
        {
            const auto marker = safety.getMarkerFile();
            safety.markLoading(pluginId);

            formatManager.createPluginInstanceAsync(
                d, sr, bs,
                [alive, done, marker, finish](std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
                {
                    marker.deleteFile();      // the plugin got through loading

                    if (alive.expired())
                        return;

                    if (inst == nullptr)
                    {
                        if (done) done(err.isEmpty() ? "Could not load plugin" : err);
                        return;
                    }
                    finish(std::move(inst));
                });
        };

        if (! (ownsDevice && isolatePlugins.load()))
        {
            loadInProcess();
            return;
        }

        // Out-of-process: the plugin loads in a helper process, so a crash cannot take OpenRack down.
        remote::RemotePlugin::launch(
            d, sr, bs,
            [this, alive, d, done, pluginId, finish, loadInProcess]
            (std::unique_ptr<remote::RemotePlugin> p, const juce::String& err, bool cleanFailure)
            {
                if (alive.expired())
                    return;

                if (p == nullptr)
                {
                    if (cleanFailure)
                    {
                        if (onMessage)
                            onMessage(d.name + " cannot run in its own process (" + err + "). Loading it normally.");
                        loadInProcess();
                    }
                    else
                    {
                        safety.block(pluginId);
                        if (done)
                            done(d.name + " crashed while loading and has been blocked. It did not affect OpenRack.");
                    }
                    return;
                }

                p->onEvent = [this, alive](const juce::String& m)
                {
                    if (alive.expired()) return;
                    if (onMessage) onMessage(m);
                    notify();
                };
                p->onLatencyChanged = [this, alive] { if (! alive.expired()) notifyLatency(); };
                finish(std::move(p));
            });
    }

    //--------------------------------------------------------------------------
    // Replaces everything with the racks in a session (snapshots are left alone).
    void loadCore(std::shared_ptr<juce::XmlElement> xml, std::function<void(const juce::String&)> done)
    {
        auto session = toSession(*xml);
        const unsigned int generation = ++loadGeneration;

        while (! racks.empty())
            destroyRack(0);

        applyGroupsXml(*session);
        applyParamFadersXml(*session);

        auto pending = std::make_shared<std::vector<PendingSlot>>();

        for (auto* re : session->getChildWithTagNameIterator("Rack"))
        {
            if (getNumRacks() >= kMaxRacks) break;
            const int idx = createRack(re->getStringAttribute("name", "Rack"), -1, -1);
            applyRackSettings(*racks[(size_t) idx], *re);
            for (auto* se : re->getChildWithTagNameIterator("Slot"))
                pending->push_back({ racks[(size_t) idx]->id, se });
        }

        if (racks.empty())
            createRack("Rack 1", 0, 0);

        selected = juce::jlimit(0, getNumRacks() - 1, session->getIntAttribute("sel", 0));
        applyInputsXml(*session);
        masterGain.store((float) session->getDoubleAttribute("master", 1.0));
        if (session->hasAttribute("tempo"))
            setTempo(session->getDoubleAttribute("tempo", 120.0));
        applyMuteSolo();
        rebuildConnections();
        notify();

        loadNext(session, pending, 0, generation, std::move(done));
    }

    void loadNext(std::shared_ptr<juce::XmlElement> session, std::shared_ptr<std::vector<PendingSlot>> pending,
                  size_t idx, unsigned int generation, std::function<void(const juce::String&)> done)
    {
        if (generation != loadGeneration)
            return;     // another rack was loaded meanwhile

        if (idx >= pending->size())
        {
            notify();
            if (done) done({});
            return;
        }

        auto* e = (*pending)[idx].element;
        const auto rackId = (*pending)[idx].rackId;

        juce::PluginDescription d;
        auto* descXml = e->getFirstChildElement();
        if (descXml == nullptr || ! d.loadFromXml(*descXml))
        {
            loadNext(session, pending, idx + 1, generation, done);
            return;
        }

        juce::MemoryBlock mb;
        mb.fromBase64Encoding(e->getStringAttribute("state"));

        addPluginTo(rackId, d,
                    [this, session, pending, idx, generation, done](const juce::String&)
                    { loadNext(session, pending, idx + 1, generation, done); },
                    mb, e->getBoolAttribute("bypass"), -1);
    }

    // Accepts both the current <Session> format and the older single-<Rack> format.
    static std::shared_ptr<juce::XmlElement> toSession(const juce::XmlElement& x)
    {
        if (x.hasTagName("Session"))
            return std::make_shared<juce::XmlElement>(x);

        auto s = std::make_shared<juce::XmlElement>("Session");
        auto* r = s->createNewChildElement("Rack");
        r->setAttribute("name", "Rack 1");
        r->setAttribute("in", 0);
        r->setAttribute("out", 0);
        r->setAttribute("gain", x.getDoubleAttribute("gain", 1.0));

        for (auto* e : x.getChildWithTagNameIterator("Slot"))
            r->addChildElement(new juce::XmlElement(*e));
        if (auto* in = x.getChildByName("Inputs"))
            s->addChildElement(new juce::XmlElement(*in));
        return s;
    }

    void applyInputsXml(const juce::XmlElement& session)
    {
        for (auto& b : inputBuses)
        {
            b.gain.store(1.0f);
            b.mute.store(false);
            b.solo.store(false);
        }

        if (auto* inputs = session.getChildByName("Inputs"))
            for (auto* e : inputs->getChildWithTagNameIterator("Bus"))
            {
                const int i = e->getIntAttribute("i", -1);
                if (i < 0 || i >= kMaxInputBuses) continue;
                auto& b = inputBuses[(size_t) i];
                b.gain.store((float) e->getDoubleAttribute("gain", 1.0));
                b.mute.store(e->getBoolAttribute("mute"));
                b.solo.store(e->getBoolAttribute("solo"));
            }
    }

    static void raisePeak(std::atomic<float>& a, float v)
    {
        if (v > a.load(std::memory_order_relaxed))
            a.store(v, std::memory_order_relaxed);
    }

    // input pair -> rack input meter -> plugins -> rack fader -> output pair
    void rebuildConnections()
    {
        for (auto& c : graph.getConnections())
            graph.removeConnection(c);

        auto link = [this](juce::AudioProcessorGraph::NodeID a, int ach,
                           juce::AudioProcessorGraph::NodeID b, int bch)
        {
            graph.addConnection({ { a, ach }, { b, bch } });
        };

        for (auto& r : racks)
        {
            if (r->inPair >= 0 && r->inPair < kMaxInputBuses)
                for (int c = 0; c < 2; ++c)
                    link(audioIn->nodeID, r->inPair * 2 + c, r->inNode->nodeID, c);

            // input meter -> channel EQ -> plugin chain
            for (int c = 0; c < 2; ++c)
                link(r->inNode->nodeID, c, r->eqNode->nodeID, c);

            auto prev = r->eqNode->nodeID;
            for (auto& s : r->slots)
            {
                for (int c = 0; c < 2; ++c)
                    link(prev, c, s.node->nodeID, c);
                prev = s.node->nodeID;
            }

            for (int c = 0; c < 2; ++c)
                link(prev, c, r->faderNode->nodeID, c);

            if (r->outPair >= 0 && r->outPair < kMaxInputBuses)
                for (int c = 0; c < 2; ++c)
                    link(r->faderNode->nodeID, c, audioOut->nodeID, r->outPair * 2 + c);
        }

        for (int i = 0; i < kMaxInputBuses; ++i)
            for (int o = 0; o < kMaxInputBuses; ++o)
                if (direct[i][o])
                    for (int c = 0; c < 2; ++c)
                        link(audioIn->nodeID, i * 2 + c, audioOut->nodeID, o * 2 + c);
    }

    void notify()
    {
        if (onChanged) onChanged();
        notifyLatency();
    }

    void notifyLatency() { if (onLatencyChanged) onLatencyChanged(); }

    const bool ownsDevice;
    double sampleRate = 44100.0;
    int blockSize = 512;
    int cap_ = 512;
    std::atomic<float> masterGain { 1.0f }, masterPeakL { 0.0f }, masterPeakR { 0.0f };
    std::atomic<int> numInputPairs { kMaxInputBuses }, numOutputPairs { kMaxInputBuses };
    std::shared_ptr<int> aliveToken = std::make_shared<int>(0);
    std::atomic<bool> isolatePlugins { false };
    TempoPlayHead tempoHead;
    ParamFader paramFaders[kNumParamFaders];
    bool paramLocked = false;
    std::atomic<int> dropouts { 0 };
    std::atomic<float> peakLoad { 0.0f };
    int shownDropouts = 0, lastXruns = -1, missingTicks = 0, midiTicks = 0;
    bool deviceLost = false;
    juce::AudioDeviceManager::AudioDeviceSetup wantedSetup;

    void tryReconnectAudio()
    {
        auto& type = *deviceManager.getCurrentDeviceTypeObject();
        type.scanForDevices();
        const auto outs = type.getDeviceNames(false);
        const auto ins = type.getDeviceNames(true);
        const bool outOk = wantedSetup.outputDeviceName.isEmpty() || outs.contains(wantedSetup.outputDeviceName);
        const bool inOk = wantedSetup.inputDeviceName.isEmpty() || ins.contains(wantedSetup.inputDeviceName);
        if (! (outOk && inOk))
            return;

        const auto err = deviceManager.setAudioDeviceSetup(wantedSetup, true);
        if (err.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr)
        {
            deviceLost = false;
            lastXruns = -1;
            if (onMessage)
                onMessage("Audio device reconnected");
        }
    }
    std::vector<double> taps;

    juce::AudioProcessorGraph graph;
    juce::AudioProcessorGraph::Node::Ptr audioIn, audioOut;
    juce::AudioBuffer<float> work;
    std::unique_ptr<DeviceCallback> deviceCallback;

    std::vector<std::unique_ptr<Rack>> racks;
    std::vector<Group> groups;
    bool direct[kMaxInputBuses][kMaxInputBuses] = {};
    juce::uint32 nextGroupId = 1;
    int selected = 0;
    juce::uint32 nextRackId = 1;
    unsigned int loadGeneration = 0;

    std::array<std::shared_ptr<juce::XmlElement>, kNumSnapshots> snapshots;
    int currentSnapshot = -1;
    juce::String suspectAtStartup;
    std::map<juce::uint32, std::unique_ptr<PluginWindow>> windows;
};
