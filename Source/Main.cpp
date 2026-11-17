#include "MainComponent.h"

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
};

START_JUCE_APPLICATION(OpenRackApplication)
