#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>

//==============================================================================
// Simple master gain stage at the end of the chain.
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
    juce::AudioDeviceManager deviceManager;
    std::function<void()> onChanged;

    RackEngine()
    {
        formatManager.addDefaultFormats();
        deviceManager.initialiseWithDefaultDevices(2, 2);

        graph.setPlayConfigDetails(2, 2, 44100.0, 512);
        audioIn = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
        audioOut = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
        gainNode = graph.addNode(std::make_unique<GainProcessor>());

        rebuildConnections();

        player.setProcessor(&graph);
        deviceManager.addAudioCallback(&player);
    }

    ~RackEngine()
    {
        windows.clear();
        deviceManager.removeAudioCallback(&player);
        player.setProcessor(nullptr);
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

    //--------------------------------------------------------------------------
    // Adds a plugin to the end of the chain. `done` gets an empty string on success.
    void addPlugin(const juce::PluginDescription& d,
                   std::function<void(const juce::String&)> done = {},
                   juce::MemoryBlock state = {},
                   bool bypassed = false)
    {
        double sr = 44100.0;
        int bs = 512;
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            sr = dev->getCurrentSampleRate();
            bs = dev->getCurrentBufferSizeSamples();
        }

        formatManager.createPluginInstanceAsync(
            d, sr, bs,
            [this, d, done, state, bypassed](std::unique_ptr<juce::AudioPluginInstance> inst,
                                             const juce::String& err)
            {
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

    void setBypassed(int i, bool b)
    {
        if (i >= 0 && i < getNumSlots())
            slots[(size_t) i].node->setBypassed(b);
    }

    void openEditor(int i)
    {
        if (i < 0 || i >= getNumSlots()) return;
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
    // Rack files: plugin descriptions + state + bypass + master gain.
    bool saveRack(const juce::File& file) const
    {
        juce::XmlElement root("Rack");
        root.setAttribute("gain", (double) getMasterGain());

        for (auto& s : slots)
        {
            auto* e = root.createNewChildElement("Slot");
            e->addChildElement(s.desc.createXml().release());
            e->setAttribute("bypass", s.node->isBypassed());

            juce::MemoryBlock mb;
            s.node->getProcessor()->getStateInformation(mb);
            e->setAttribute("state", mb.toBase64Encoding());
        }
        return root.writeTo(file);
    }

    void loadRack(const juce::File& file, std::function<void(const juce::String&)> done)
    {
        std::shared_ptr<juce::XmlElement> xml(juce::parseXML(file).release());
        if (xml == nullptr || ! xml->hasTagName("Rack"))
        {
            if (done) done("Not a valid rack file");
            return;
        }

        while (getNumSlots() > 0)
            removeSlot(0);

        setMasterGain((float) xml->getDoubleAttribute("gain", 1.0));
        loadNext(xml, 0, std::move(done));
    }

private:
    GainProcessor& gainProc() const { return *static_cast<GainProcessor*>(gainNode->getProcessor()); }

    void loadNext(std::shared_ptr<juce::XmlElement> xml, int idx,
                  std::function<void(const juce::String&)> done)
    {
        auto* e = xml->getChildElement(idx);
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
            loadNext(xml, idx + 1, done);
            return;
        }

        juce::MemoryBlock mb;
        mb.fromBase64Encoding(e->getStringAttribute("state"));

        addPlugin(d, [this, xml, idx, done](const juce::String&) { loadNext(xml, idx + 1, done); },
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

        auto prev = audioIn->nodeID;
        for (auto& s : slots)
        {
            link(prev, s.node->nodeID);
            prev = s.node->nodeID;
        }
        link(prev, gainNode->nodeID);
        link(gainNode->nodeID, audioOut->nodeID);
    }

    void notify() { if (onChanged) onChanged(); }

    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioProcessorGraph::Node::Ptr audioIn, audioOut, gainNode;
    std::vector<Slot> slots;
    std::map<juce::uint32, std::unique_ptr<PluginWindow>> windows;
};
