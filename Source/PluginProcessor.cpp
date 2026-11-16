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
    OpenRackProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          engine(false)
    {
        // Tell the host how much delay the plugins in the rack add.
        engine.onLatencyChanged = [this] { setLatencySamples(engine.getTotalLatency()); };
    }

    ~OpenRackProcessor() override { engine.onLatencyChanged = nullptr; }

    RackEngine& getEngine() { return engine; }

    const juce::String getName() const override { return "OpenRack"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        engine.prepare(sampleRate, samplesPerBlock);
    }

    void releaseResources() override { engine.release(); }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        engine.process(buffer, midi);
    }

    double getTailLengthSeconds() const override { return engine.getTotalTailSeconds(); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return new OpenRackEditor(*this); }
    bool hasEditor() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
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
    RackEngine engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenRackProcessor)
};

//==============================================================================
OpenRackEditor::OpenRackEditor(OpenRackProcessor& p) : AudioProcessorEditor(&p)
{
    setLookAndFeel(&laf.get());
    content = std::make_unique<MainComponent>(p.getEngine());
    addAndMakeVisible(*content);

    setResizable(true, true);
    setResizeLimits(800, 500, 2000, 1200);
    setSize(960, 620);
}

// Called by the plugin wrapper to create the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OpenRackProcessor();
}
