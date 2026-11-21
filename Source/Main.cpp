#include "MainComponent.h"
#include "RemotePlugin.h"
#include <cstdio>

// Self-test used by CI: loads a plugin through the out-of-process host, pushes audio through it,
// reads and writes its state, then kills the helper and checks that it comes back.
class HostSelfTest : private juce::Timer
{
public:
    explicit HostSelfTest(const juce::String& pluginPath)
    {
        juce::AudioPluginFormatManager formats;
        formats.addDefaultFormats();
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : formats.getFormats())
            if (f->fileMightContainThisPluginType(pluginPath))
                f->findAllTypesForFile(found, pluginPath);

        if (found.isEmpty())
        {
            finish(false, "no plugin found in " + pluginPath);
            return;
        }

        std::printf("HOSTTEST: launching %s in a helper process\n", found[0]->name.toRawUTF8());
        started = juce::Time::getMillisecondCounter();

        remote::RemotePlugin::launch(*found[0], 48000.0, 512,
            [this](std::unique_ptr<remote::RemotePlugin> p, const juce::String& err, bool)
            {
                if (p == nullptr)
                    return finish(false, "launch failed: " + err);
                plugin = std::move(p);
                plugin->prepareToPlay(48000.0, 512);
                phase = 1;
            });
        startTimer(50);
    }

private:
    void finish(bool ok, const juce::String& message)
    {
        stopTimer();
        std::printf("HOSTTEST: %s - %s\n", ok ? "PASS" : "FAIL", message.toRawUTF8());
        std::fflush(stdout);
        plugin.reset();
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->setApplicationReturnValue(ok ? 0 : 1);
        juce::JUCEApplicationBase::quit();
    }

    // Runs `blocks` blocks of a sine wave through the plugin. Returns the largest difference
    // between input and output (the OpenRack plugin is a plain pass-through by default).
    float runAudio(int blocks, int& misses)
    {
        const int before = plugin->getTotalMisses();
        float worst = 0.0f;
        double phaseAcc = 0.0;
        juce::AudioBuffer<float> buffer(2, 512), reference(2, 512);
        juce::MidiBuffer midi;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.5f * (float) std::sin(phaseAcc);
                phaseAcc += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0;
                buffer.setSample(0, i, v);
                buffer.setSample(1, i, -v);
            }
            reference.makeCopyOf(buffer);
            plugin->processBlock(buffer, midi);

            if (b > 4)      // skip the first blocks
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i)
                        worst = juce::jmax(worst, std::abs(buffer.getSample(c, i) - reference.getSample(c, i)));
            juce::Thread::sleep(1);
        }
        misses = plugin->getTotalMisses() - before;
        return worst;
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (now - started > 150000)
            return finish(false, "timed out in phase " + juce::String(phase));

        using State = remote::RemotePlugin::State;

        switch (phase)
        {
            case 0: break;      // waiting for the helper to load the plugin

            case 1:
            {
                int misses = 0;
                const float diff = runAudio(300, misses);
                std::printf("HOSTTEST: audio round trip: max difference %.6f, missed blocks %d\n", diff, misses);
                if (misses > 5)
                    return finish(false, "too many missed blocks: " + juce::String(misses) + " (" + plugin->debugInfo() + ")");
                if (diff > 0.01f)
                    return finish(false, "output does not match input");
                phase = 2;
                break;
            }
            case 2:
            {
                juce::MemoryBlock mb;
                plugin->getStateInformation(mb);
                std::printf("HOSTTEST: state size %d bytes\n", (int) mb.getSize());
                if (mb.getSize() == 0)
                    return finish(false, "no state returned");
                plugin->setStateInformation(mb.getData(), (int) mb.getSize());
                std::printf("HOSTTEST: killing the helper to simulate a crash\n");
                plugin->debugKillChild();
                phase = 3;
                phaseStart = now;
                break;
            }
            case 3:
                if (plugin->getState() == State::crashed || plugin->getState() == State::launching)
                {
                    std::printf("HOSTTEST: crash detected (%s)\n", plugin->getStatusText().toRawUTF8());
                    phase = 4;
                    phaseStart = now;
                }
                else if (now - phaseStart > 5000)
                    return finish(false, "crash was not detected");
                break;

            case 4:
                if (plugin->getState() == State::ready)
                {
                    int misses = 0;
                    const float diff = runAudio(100, misses);
                    std::printf("HOSTTEST: after restart: max difference %.6f, missed blocks %d\n", diff, misses);
                    if (diff > 0.01f)
                        return finish(false, "output wrong after restart");
                    return finish(true, "audio, state and crash recovery all work");
                }
                if (plugin->getState() == State::failed)
                    return finish(false, "helper was not restarted");
                break;
        }
    }

    std::unique_ptr<remote::RemotePlugin> plugin;
    int phase = 0;
    juce::uint32 started = 0, phaseStart = 0;
};

// The standalone app: owns a rack that opens its own audio device. The engine is
// a base class so it is built before, and destroyed after, the UI that uses it.
struct EngineHolder { RackEngine engine { true }; };

class StandaloneRack : private EngineHolder, public MainComponent
{
public:
    StandaloneRack() : MainComponent(EngineHolder::engine) {}
};

class OpenRackApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "OpenRack"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        // Plugin host helper: runs one plugin for the main app (out-of-process hosting).
        {
            const auto a = getCommandLineParameterArray();
            const int h = a.indexOf("--host");
            if (h >= 0)
            {
#if JUCE_MAC
                juce::Process::setDockIconVisible(false);
#endif
                juce::StringArray rest;
                for (int i = h + 1; i < a.size(); ++i)
                    rest.add(a[i]);
                hostWorker = std::make_unique<remote::Worker>(rest);
                if (! hostWorker->isOk())
                {
                    setApplicationReturnValue(1);
                    quit();
                }
                return;
            }

            const int t = a.indexOf("--hosttest");
            if (t >= 0 && a.size() > t + 1)
            {
                selfTest = std::make_unique<HostSelfTest>(a[t + 1]);
                return;
            }
        }

        // Safety check mode: a short-lived helper process that loads one plugin,
        // so that a plugin which crashes takes down this helper, not the app.
        const auto args = getCommandLineParameterArray();
        const int flag = args.indexOf("--preflight");
        if (flag >= 0 && args.size() > flag + 2)
        {
            const bool ok = PluginSafety::runPreflightChild(juce::File(args[flag + 1]), juce::File(args[flag + 2]));
            setApplicationReturnValue(ok ? 0 : 1);
            quit();
            return;
        }

        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        hostWorker.reset();
        selfTest.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }
    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name, Theme::background(), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new StandaloneRack(), true);
            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    OpenRackLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<remote::Worker> hostWorker;
    std::unique_ptr<HostSelfTest> selfTest;
};

START_JUCE_APPLICATION(OpenRackApplication)
