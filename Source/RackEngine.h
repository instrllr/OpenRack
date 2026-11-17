#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <map>
#include <memory>
#include "Sandbox.h"

static constexpr int kMaxInputBuses = 64;     // stereo input buses a DAW can patch into OpenRack
static constexpr int kNumSnapshots = 8;

//==============================================================================
// Simple master gain stage at the end of the chain, with peak metering.
class GainProcessor : public juce::AudioProcessor
{
public:
    GainProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("In", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "Master"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        b.applyGain(gain.load());

        // Track output peaks for the level meter (read and reset by the UI).
        const int n = b.getNumSamples();
        if (b.getNumChannels() > 0) updatePeak(peakL, b.getMagnitude(0, 0, n));
        if (b.getNumChannels() > 1) updatePeak(peakR, b.getMagnitude(1, 0, n));
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
    std::atomic<float> peakL { 0.0f };
    std::atomic<float> peakR { 0.0f };

private:
    static void updatePeak(std::atomic<float>& a, float v)
    {
        if (v > a.load(std::memory_order_relaxed))
            a.store(v, std::memory_order_relaxed);
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
// The rack itself. Two modes:
//  - ownsAudioDevice = true : the standalone app, which opens its own audio device.
//  - ownsAudioDevice = false: the plugin, where the DAW calls prepare()/process().
class RackEngine
{
public:
    struct Slot
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::PluginDescription desc;
    };

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioDeviceManager deviceManager;     // only used when ownsAudioDevice
    PluginSafety safety { getDataDirectory() }; // block list, crash marker, pre-flight cache

    // One stereo input bus of the plugin version, with its own mixer strip.
    struct InputBus
    {
        std::atomic<float> gain { 1.0f };
        std::atomic<bool> mute { false }, solo { false }, active { false };
        std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    };
    std::array<InputBus, kMaxInputBuses> inputBuses;
    std::function<void()> onChanged;            // rack contents changed (message thread)
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

        graph.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        audioIn = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
        audioOut = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
        gainNode = graph.addNode(std::make_unique<GainProcessor>());
        inputNode = graph.addNode(std::make_unique<GainProcessor>());   // unity gain, only meters the input

        rebuildConnections();

        if (ownsDevice)
        {
            deviceManager.initialiseWithDefaultDevices(2, 2);
            player.setProcessor(&graph);
            deviceManager.addAudioCallback(&player);
        }
    }

    ~RackEngine()
    {
        aliveToken.reset();     // any plugin still loading will notice and give up
        windows.clear();

        if (ownsDevice)
        {
            deviceManager.removeAudioCallback(&player);
            player.setProcessor(nullptr);
        }
    }

    bool ownsAudioDevice() const { return ownsDevice; }

    //--------------------------------------------------------------------------
    // Plugin mode: called by the host.
    void prepare(double sr, int bs)
    {
        sampleRate = sr;
        blockSize = bs;
        graph.setPlayConfigDetails(2, 2, sr, bs);
        graph.prepareToPlay(sr, bs);
    }

    void release() { graph.releaseResources(); }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        graph.processBlock(buffer, midi);
    }

    double getSampleRate() const { return sampleRate; }
    int getBlockSize() const { return blockSize; }

    // Total latency of the active plugins in the chain, so the host can compensate.
    int getTotalLatency() const
    {
        int total = 0;
        for (auto& s : slots)
            if (! s.node->isBypassed())
                total += s.node->getProcessor()->getLatencySamples();
        return total;
    }

    double getTotalTailSeconds() const
    {
        double total = 0.0;
        for (auto& s : slots)
            total += s.node->getProcessor()->getTailLengthSeconds();
        return total;
    }

    //--------------------------------------------------------------------------
    // Plugin mode: mixes every enabled input bus the DAW has patched in (bus 0 is
    // the main input) into the stereo signal that feeds the rack.
    void applyInputMix(juce::AudioProcessor& host, juce::AudioBuffer<float>& buffer)
    {
        const int n = buffer.getNumSamples();
        const int numBuses = juce::jmin(host.getBusCount(true), kMaxInputBuses);

        bool anySolo = false;
        for (int i = 0; i < kMaxInputBuses; ++i)
        {
            auto& b = inputBuses[(size_t) i];
            const bool on = i < numBuses && host.getBus(true, i)->isEnabled();
            b.active.store(on, std::memory_order_relaxed);
            if (on && b.solo.load(std::memory_order_relaxed))
                anySolo = true;
        }

        for (int i = 0; i < numBuses; ++i)
        {
            auto& b = inputBuses[(size_t) i];
            if (! b.active.load(std::memory_order_relaxed))
                continue;

            auto bb = host.getBusBuffer(buffer, true, i);
            if (bb.getNumChannels() < 2 || n == 0)
                continue;

            raisePeak(b.peakL, bb.getMagnitude(0, 0, n));
            raisePeak(b.peakR, bb.getMagnitude(1, 0, n));

            const bool audible = ! b.mute.load() && (! anySolo || b.solo.load());
            const float g = audible ? b.gain.load() : 0.0f;

            if (i == 0)
            {
                bb.applyGain(g);
            }
            else if (g > 0.0f)
            {
                buffer.addFrom(0, 0, bb, 0, 0, n, g);
                buffer.addFrom(1, 0, bb, 1, 0, n, g);
            }
        }
    }

    //--------------------------------------------------------------------------
    // Snapshots: eight stored racks. If a snapshot has the same plugins in the
    // same order the switch is instant (only settings change); otherwise the
    // plugins are reloaded.
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

        std::vector<juce::XmlElement*> els;
        for (auto* e : xml->getChildWithTagNameIterator("Slot"))
            els.push_back(e);

        bool same = els.size() == slots.size();
        for (size_t k = 0; same && k < els.size(); ++k)
        {
            juce::PluginDescription d;
            auto* dx = els[k]->getFirstChildElement();
            same = dx != nullptr && d.loadFromXml(*dx)
                   && d.createIdentifierString() == slots[k].desc.createIdentifierString();
        }

        if (same)
        {
            for (size_t k = 0; k < els.size(); ++k)
            {
                juce::MemoryBlock mb;
                mb.fromBase64Encoding(els[k]->getStringAttribute("state"));
                if (mb.getSize() > 0)
                    slots[k].node->getProcessor()->setStateInformation(mb.getData(), (int) mb.getSize());
                slots[k].node->setBypassed(els[k]->getBoolAttribute("bypass"));
            }
            setMasterGain((float) xml->getDoubleAttribute("gain", 1.0));
            applyInputsXml(*xml);
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
    int getNumSlots() const { return (int) slots.size(); }
    const Slot& getSlot(int i) const { return slots[(size_t) i]; }

    float getMasterGain() const { return gainProc().gain.load(); }
    void setMasterGain(float g) { gainProc().gain.store(g); }

    // Peak output level since the last call (0..1+), for the level meter.
    float takeOutputPeak(int channel)
    {
        auto& p = (channel == 0) ? gainProc().peakL : gainProc().peakR;
        return p.exchange(0.0f);
    }

    // Same for the signal arriving at the rack.
    float takeInputPeak(int channel)
    {
        auto& g = *static_cast<GainProcessor*>(inputNode->getProcessor());
        return (channel == 0 ? g.peakL : g.peakR).exchange(0.0f);
    }

    //--------------------------------------------------------------------------
    // Adds a plugin to the end of the chain. `done` gets an empty string on success.
    void addPlugin(const juce::PluginDescription& d,
                   std::function<void(const juce::String&)> done = {},
                   juce::MemoryBlock state = {},
                   bool bypassed = false)
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

        const auto marker = safety.getMarkerFile();
        safety.markLoading(pluginId);

        std::weak_ptr<int> alive = aliveToken;

        formatManager.createPluginInstanceAsync(
            d, sr, bs,
            [this, alive, d, done, state, bypassed, marker](std::unique_ptr<juce::AudioPluginInstance> inst,
                                                            const juce::String& err)
            {
                marker.deleteFile();      // the plugin got through loading

                if (alive.expired())
                    return;     // the rack was destroyed while this plugin was loading

                if (inst == nullptr)
                {
                    if (done) done(err.isEmpty() ? "Could not load plugin" : err);
                    return;
                }

                if (state.getSize() > 0)
                    inst->setStateInformation(state.getData(), (int) state.getSize());

                auto node = graph.addNode(std::move(inst));
                node->setBypassed(bypassed);
                slots.push_back({ node, d });
                rebuildConnections();
                notify();
                if (done) done({});
            });
    }

    void removeSlot(int i)
    {
        if (i < 0 || i >= getNumSlots()) return;
        auto id = slots[(size_t) i].node->nodeID;
        if (onBeforeSlotRemoved) onBeforeSlotRemoved(id.uid);   // let the UI close its editor first
        windows.erase(id.uid);
        graph.removeNode(id);
        slots.erase(slots.begin() + i);
        rebuildConnections();
        notify();
    }

    void moveSlot(int i, int delta)
    {
        int j = i + delta;
        if (i < 0 || j < 0 || i >= getNumSlots() || j >= getNumSlots()) return;
        std::swap(slots[(size_t) i], slots[(size_t) j]);
        rebuildConnections();
        notify();
    }

    // Moves slot `from` so it sits before slot `insertBefore` (0..numSlots) - used by drag and drop.
    void moveSlotTo(int from, int insertBefore)
    {
        const int n = getNumSlots();
        if (from < 0 || from >= n || insertBefore < 0 || insertBefore > n) return;

        int to = insertBefore > from ? insertBefore - 1 : insertBefore;
        if (to == from) return;

        auto s = slots[(size_t) from];
        slots.erase(slots.begin() + from);
        slots.insert(slots.begin() + to, s);
        rebuildConnections();
        notify();
    }

    void setBypassed(int i, bool b)
    {
        if (i >= 0 && i < getNumSlots())
        {
            slots[(size_t) i].node->setBypassed(b);
            notify();
        }
    }

    void setAllBypassed(bool b)
    {
        for (auto& s : slots)
            s.node->setBypassed(b);
        notify();
    }

    bool areAllBypassed() const
    {
        if (slots.empty()) return false;
        for (auto& s : slots)
            if (! s.node->isBypassed()) return false;
        return true;
    }

    void clearRack()
    {
        while (getNumSlots() > 0)
            removeSlot(0);
    }

    // Inserts a plugin at a given position instead of the end (done after it has loaded).
    void addPluginAt(const juce::PluginDescription& d, int insertAt,
                     std::function<void(const juce::String&)> done = {})
    {
        const int before = getNumSlots();
        addPlugin(d, [this, insertAt, before, done](const juce::String& err)
        {
            if (err.isEmpty() && insertAt >= 0 && insertAt < before && getNumSlots() > 0)
                moveSlotTo(getNumSlots() - 1, insertAt);
            if (done) done(err);
        });
    }

    void openEditor(int i)
    {
        if (i < 0 || i >= getNumSlots()) return;

        // Inside a DAW the editor is shown within OpenRack's own window instead.
        if (onOpenEditorRequested)
        {
            onOpenEditorRequested(i);
            return;
        }

        auto node = slots[(size_t) i].node;
        auto uid = node->nodeID.uid;

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
    // Rack state: plugin descriptions + state + bypass + master gain.
    // The rack as it is right now (plugins, settings, bypass, master gain, input mixer).
    std::unique_ptr<juce::XmlElement> createCoreXml() const
    {
        auto root = std::make_unique<juce::XmlElement>("Rack");
        root->setAttribute("gain", (double) getMasterGain());

        for (auto& s : slots)
        {
            auto* e = root->createNewChildElement("Slot");
            e->addChildElement(s.desc.createXml().release());
            e->setAttribute("bypass", s.node->isBypassed());

            juce::MemoryBlock mb;
            s.node->getProcessor()->getStateInformation(mb);
            e->setAttribute("state", mb.toBase64Encoding());
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

    // Everything: the current rack plus all stored snapshots (this is what a DAW project saves).
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
        if (xml == nullptr || ! xml->hasTagName("Rack"))
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
                    snapshots[(size_t) i].reset(new juce::XmlElement(*e->getFirstChildElement()));
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
    GainProcessor& gainProc() const { return *static_cast<GainProcessor*>(gainNode->getProcessor()); }

    // Replaces the plugins with those in a <Rack> element (snapshots are left alone).
    void loadCore(std::shared_ptr<juce::XmlElement> xml, std::function<void(const juce::String&)> done)
    {
        while (getNumSlots() > 0)
            removeSlot(0);

        setMasterGain((float) xml->getDoubleAttribute("gain", 1.0));
        applyInputsXml(*xml);
        loadNext(xml, xml->getFirstChildElement(), std::move(done));
    }

    void applyInputsXml(const juce::XmlElement& rack)
    {
        for (auto& b : inputBuses)
        {
            b.gain.store(1.0f);
            b.mute.store(false);
            b.solo.store(false);
        }

        if (auto* inputs = rack.getChildByName("Inputs"))
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

    void loadNext(std::shared_ptr<juce::XmlElement> xml, juce::XmlElement* e,
                  std::function<void(const juce::String&)> done)
    {
        while (e != nullptr && ! e->hasTagName("Slot"))
            e = e->getNextElement();

        if (e == nullptr)
        {
            notify();
            if (done) done({});
            return;
        }

        juce::PluginDescription d;
        auto* descXml = e->getFirstChildElement();
        if (descXml == nullptr || ! d.loadFromXml(*descXml))
        {
            loadNext(xml, e->getNextElement(), done);
            return;
        }

        juce::MemoryBlock mb;
        mb.fromBase64Encoding(e->getStringAttribute("state"));

        addPlugin(d, [this, xml, e, done](const juce::String&) { loadNext(xml, e->getNextElement(), done); },
                  mb, e->getBoolAttribute("bypass"));
    }

    void rebuildConnections()
    {
        for (auto& c : graph.getConnections())
            graph.removeConnection(c);

        auto link = [this](juce::AudioProcessorGraph::NodeID a, juce::AudioProcessorGraph::NodeID b)
        {
            for (int ch = 0; ch < 2; ++ch)
                graph.addConnection({ { a, ch }, { b, ch } });
        };

        link(audioIn->nodeID, inputNode->nodeID);
        auto prev = inputNode->nodeID;
        for (auto& s : slots)
        {
            link(prev, s.node->nodeID);
            prev = s.node->nodeID;
        }
        link(prev, gainNode->nodeID);
        link(gainNode->nodeID, audioOut->nodeID);
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
    std::shared_ptr<int> aliveToken = std::make_shared<int>(0);

    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioProcessorGraph::Node::Ptr audioIn, audioOut, gainNode, inputNode;
    std::vector<Slot> slots;
    std::array<std::shared_ptr<juce::XmlElement>, kNumSnapshots> snapshots;
    int currentSnapshot = -1;
    juce::String suspectAtStartup;
    std::map<juce::uint32, std::unique_ptr<PluginWindow>> windows;
};
