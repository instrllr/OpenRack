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
