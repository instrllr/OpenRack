#pragma once
#include "RackEngine.h"

//==============================================================================
// One row in the rack list: name + Edit / Bypass / Up / Down / Remove.
class SlotRow : public juce::Component
{
public:
    explicit SlotRow(RackEngine& e) : engine(e)
    {
        edit.setButtonText("Edit");
        bypass.setButtonText("Bypass");
        bypass.setClickingTogglesState(true);
        up.setButtonText("^");
        down.setButtonText("v");
        remove.setButtonText("X");

        for (juce::Button* b : { (juce::Button*) &edit, (juce::Button*) &bypass,
                                 (juce::Button*) &up, (juce::Button*) &down,
                                 (juce::Button*) &remove })
            addAndMakeVisible(b);

        edit.onClick = [this] { engine.openEditor(row); };
        bypass.onClick = [this] { engine.setBypassed(row, bypass.getToggleState()); repaint(); };
        up.onClick = [this] { engine.moveSlot(row, -1); };
        down.onClick = [this] { engine.moveSlot(row, +1); };
        remove.onClick = [this] { engine.removeSlot(row); };
    }

    void setRow(int r)
    {
        row = r;
        if (r < engine.getNumSlots())
        {
            auto& s = engine.getSlot(r);
            name = s.desc.name;
            maker = s.desc.manufacturerName;
            bypass.setToggleState(s.node->isBypassed(), juce::dontSendNotification);
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(bypass.getToggleState() ? juce::Colours::grey : juce::Colours::white);
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText(name, 8, 2, getWidth() - 260, getHeight() / 2, juce::Justification::centredLeft);
        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(maker, 8, getHeight() / 2, getWidth() - 260, getHeight() / 2 - 2,
                   juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(2);
        remove.setBounds(r.removeFromRight(30));
        down.setBounds(r.removeFromRight(30));
        up.setBounds(r.removeFromRight(30));
        bypass.setBounds(r.removeFromRight(70));
        edit.setBounds(r.removeFromRight(50));
    }

private:
    RackEngine& engine;
    int row = 0;
    juce::String name, maker;
    juce::TextButton edit, up, down, remove;
    juce::ToggleButton bypass;
};

//==============================================================================
class MainComponent : public juce::Component,
                      private juce::ChangeListener
{
public:
    MainComponent() : rackModel(*this), pluginModel(*this)
    {
        auto dataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("OpenRack");
        dataDir.createDirectory();
        pluginListFile = dataDir.getChildFile("plugins.xml");
        deadMansFile = dataDir.getChildFile("scan-crash.txt");

        if (auto xml = juce::parseXML(pluginListFile))
            engine.knownPlugins.recreateFromXml(*xml);
        engine.knownPlugins.addChangeListener(this);

        engine.onChanged = [this]
        {
            juce::Component::SafePointer<MainComponent> safe(this);
            juce::MessageManager::callAsync([safe]
            {
                if (safe != nullptr) safe->refresh();
            });
        };

        for (auto* b : { &scanBtn, &audioBtn, &saveBtn, &loadBtn })
            addAndMakeVisible(b);
        scanBtn.setButtonText("Scan plugins");
        audioBtn.setButtonText("Audio settings");
        saveBtn.setButtonText("Save rack");
        loadBtn.setButtonText("Load rack");

        scanBtn.onClick = [this] { showScanner(); };
        audioBtn.onClick = [this] { showAudioSettings(); };
        saveBtn.onClick = [this] { saveRack(); };
        loadBtn.onClick = [this] { loadRack(); };

        pluginList.setModel(&pluginModel);
        rackList.setModel(&rackModel);
        rackList.setRowHeight(44);
        pluginList.setRowHeight(24);
        addAndMakeVisible(pluginList);
        addAndMakeVisible(rackList);

        addAndMakeVisible(pluginTitle);
        addAndMakeVisible(rackTitle);
        pluginTitle.setText("Plugins (double-click to add)", juce::dontSendNotification);
        rackTitle.setText("Rack (signal flows top to bottom)", juce::dontSendNotification);

        gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        gainSlider.setRange(-60.0, 12.0, 0.1);
        gainSlider.setValue(0.0, juce::dontSendNotification);
        gainSlider.setTextValueSuffix(" dB");
        gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 22);
        gainSlider.onValueChange = [this]
        {
            engine.setMasterGain(juce::Decibels::decibelsToGain((float) gainSlider.getValue(), -60.0f));
        };
        addAndMakeVisible(gainSlider);
        addAndMakeVisible(gainLabel);
        gainLabel.setText("Master", juce::dontSendNotification);

        setSize(900, 560);
    }

    ~MainComponent() override
    {
        engine.onChanged = nullptr;
        engine.knownPlugins.removeChangeListener(this);
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff1e1f22)); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8);
        auto top = r.removeFromTop(30);
        for (auto* b : { &scanBtn, &audioBtn, &saveBtn, &loadBtn })
            b->setBounds(top.removeFromLeft(120).reduced(2));

        auto bottom = r.removeFromBottom(30);
        gainLabel.setBounds(bottom.removeFromLeft(60));
        gainSlider.setBounds(bottom);

        r.removeFromTop(6);
        auto left = r.removeFromLeft(r.getWidth() * 2 / 5);
        pluginTitle.setBounds(left.removeFromTop(22));
        pluginList.setBounds(left);
        r.removeFromLeft(8);
        rackTitle.setBounds(r.removeFromTop(22));
        rackList.setBounds(r);
    }

private:
    //--------------------------------------------------------------------------
    struct RackModel : juce::ListBoxModel
    {
        explicit RackModel(MainComponent& o) : owner(o) {}
        int getNumRows() override { return owner.engine.getNumSlots(); }
        void paintListBoxItem(int, juce::Graphics& g, int w, int h, bool) override
        {
            g.setColour(juce::Colour(0xff2b2d31));
            g.fillRoundedRectangle(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f, 4.0f);
        }
        juce::Component* refreshComponentForRow(int row, bool, juce::Component* existing) override
        {
            if (row >= getNumRows())
            {
                delete existing;
                return nullptr;
            }
            auto* r = dynamic_cast<SlotRow*>(existing);
            if (r == nullptr)
            {
                delete existing;
                r = new SlotRow(owner.engine);
            }
            r->setRow(row);
            return r;
        }
        MainComponent& owner;
    };

    struct PluginModel : juce::ListBoxModel
    {
        explicit PluginModel(MainComponent& o) : owner(o) {}
        int getNumRows() override { return owner.engine.knownPlugins.getNumTypes(); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (selected) g.fillAll(juce::Colour(0xff3a4a6b));
            if (auto types = owner.engine.knownPlugins.getTypes(); row < types.size())
            {
                g.setColour(juce::Colours::white);
                g.setFont(14.0f);
                g.drawText(types[row].name + "  -  " + types[row].manufacturerName, 6, 0, w - 8, h,
                           juce::Justification::centredLeft);
            }
        }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
        {
            auto types = owner.engine.knownPlugins.getTypes();
            if (row < types.size())
                owner.engine.addPlugin(types[row], [this](const juce::String& err)
                {
                    if (err.isNotEmpty())
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                               "Couldn't load plugin", err);
                });
        }
        MainComponent& owner;
    };

    //--------------------------------------------------------------------------
    void refresh()
    {
        rackList.updateContent();
        rackList.repaint();
        pluginList.updateContent();
        pluginList.repaint();
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (auto xml = engine.knownPlugins.createXml())
            xml->writeTo(pluginListFile);
        refresh();
    }

    void showScanner()
    {
        auto* c = new juce::PluginListComponent(engine.formatManager, engine.knownPlugins,
                                                deadMansFile, nullptr);
        c->setSize(720, 500);

        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(c);
        o.dialogTitle = "Plugin scanner";
        o.componentToCentreAround = this;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    void showAudioSettings()
    {
        auto* c = new juce::AudioDeviceSelectorComponent(engine.deviceManager, 0, 2, 0, 2,
                                                         false, false, true, false);
        c->setSize(500, 450);

        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(c);
        o.dialogTitle = "Audio settings";
        o.componentToCentreAround = this;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void saveRack()
    {
        chooser = std::make_unique<juce::FileChooser>("Save rack", juce::File(), "*.orack");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [this](const juce::FileChooser& fc)
                             {
                                 auto f = fc.getResult();
                                 if (f != juce::File())
                                     engine.saveRack(f.withFileExtension("orack"));
                             });
    }

    void loadRack()
    {
        chooser = std::make_unique<juce::FileChooser>("Load rack", juce::File(), "*.orack");
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc)
                             {
                                 auto f = fc.getResult();
                                 if (f == juce::File()) return;
                                 engine.loadRack(f, [this](const juce::String& err)
                                 {
                                     gainSlider.setValue(
                                         juce::Decibels::gainToDecibels(engine.getMasterGain(), -60.0f),
                                         juce::dontSendNotification);
                                     if (err.isNotEmpty())
                                         juce::AlertWindow::showMessageBoxAsync(
                                             juce::MessageBoxIconType::WarningIcon, "Load rack", err);
                                 });
                             });
    }

    //--------------------------------------------------------------------------
    RackEngine engine;
    RackModel rackModel;
    PluginModel pluginModel;
    juce::ListBox pluginList, rackList;
    juce::TextButton scanBtn, audioBtn, saveBtn, loadBtn;
    juce::Slider gainSlider;
    juce::Label gainLabel, pluginTitle, rackTitle;
    juce::File pluginListFile, deadMansFile;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
