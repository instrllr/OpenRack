#include "MainComponent.h"

class OpenRackProcessor;

//==============================================================================
// The plugin's window: the same interface as the standalone app.
class OpenRackEditor : public juce::AudioProcessorEditor
{
public:
    explicit OpenRackEditor(OpenRackProcessor& p);
    ~OpenRackEditor() override { setLookAndFeel(nullptr); }

    void resized() override { content->setBounds(getLocalBounds()); }

private:
    juce::SharedResourcePointer<OpenRackLookAndFeel> laf;
    std::unique_ptr<MainComponent> content;
};

//==============================================================================
// The plugin itself: a stereo effect that runs the rack on the host's audio.
class OpenRackProcessor : public juce::AudioProcessor
{
public:
    // VST3 (FL Studio, Ableton): the main input plus up to 63 more stereo inputs a
    // mixer track can be sent to. AU (Logic Pro) only offers one extra input (the
    // side-chain), so it exposes just two.
    static BusesProperties makeBuses()
    {
        const bool isAU = juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_AudioUnit;
        const int numInputs = isAU ? 2 : kMaxInputBuses;

        auto b = BusesProperties()
                     .withInput("Input 1", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true);

        for (int i = 2; i <= numInputs; ++i)
            b = b.withInput("Input " + juce::String(i), juce::AudioChannelSet::stereo(), false);
        return b;
    }

    OpenRackProcessor() : AudioProcessor(makeBuses()), engine(false)
    {
        // Tell the host how much delay the plugins in the rack add.
        engine.onLatencyChanged = [this] { setLatencySamples(engine.getTotalLatency()); };
    }

    ~OpenRackProcessor() override { aliveToken.reset(); engine.onLatencyChanged = nullptr; }

    RackEngine& getEngine() { return engine; }

    const juce::String getName() const override { return "OpenRack"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        engine.prepare(sampleRate, samplesPerBlock);
    }

    void releaseResources() override { engine.release(); }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo()
            || layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
            return false;

        // Every extra input is either switched off or stereo.
        for (const auto& in : layouts.inputBuses)
            if (! in.isDisabled() && in != juce::AudioChannelSet::stereo())
                return false;

        return true;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;

        // Mix the patched-in input buses, then run the rack on the stereo result.
        engine.applyInputMix(*this, buffer);

        juce::AudioBuffer<float> stereo(buffer.getArrayOfWritePointers(), 2, buffer.getNumSamples());
        engine.process(stereo, midi);
    }

    double getTailLengthSeconds() const override { return engine.getTotalTailSeconds(); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return new OpenRackEditor(*this); }
    bool hasEditor() const override { return true; }

    // The eight snapshots are the plugin's programs, so a DAW can automate or
    // recall them from its own program-change controls.
    int getNumPrograms() override { return kNumSnapshots; }
    int getCurrentProgram() override { return juce::jmax(0, engine.getCurrentSnapshot()); }

    void setCurrentProgram(int i) override
    {
        if (i == engine.getCurrentSnapshot() || ! engine.hasSnapshot(i))
            return;

        std::weak_ptr<int> alive = aliveToken;
        juce::MessageManager::callAsync([this, alive, i]
        {
            if (! alive.expired())
                engine.recallSnapshot(i);
        });
    }

    const juce::String getProgramName(int i) override
    {
        return "Snapshot " + juce::String(i + 1) + (engine.hasSnapshot(i) ? "" : " (empty)");
    }

    void changeProgramName(int, const juce::String&) override {}

    // The whole rack (plugins, their settings, bypass, master gain) is saved
    // inside the DAW project.
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        if (auto xml = engine.createRackXml())
            copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        if (auto xml = getXmlFromBinary(data, sizeInBytes))
        {
            std::shared_ptr<juce::XmlElement> shared(xml.release());
            engine.loadRackXml(shared, nullptr);
        }
    }

private:
    std::shared_ptr<int> aliveToken = std::make_shared<int>(0);
    RackEngine engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenRackProcessor)
};

//==============================================================================
OpenRackEditor::OpenRackEditor(OpenRackProcessor& p) : AudioProcessorEditor(&p)
{
    setLookAndFeel(&laf.get());
    content = std::make_unique<MainComponent>(p.getEngine());
    addAndMakeVisible(*content);

    // Plugin editors shown inside OpenRack can be large, so allow generous sizes.
    content->onRequestSize = [this](int w, int h) { setSize(w, h); };

    setResizable(true, true);
    setResizeLimits(800, 500, 4000, 3000);
    setSize(1020, 700);
}

// Called by the plugin wrapper to create the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OpenRackProcessor();
}
