#pragma once
#include <algorithm>
#include "RackEngine.h"
#include "Theme.h"

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

        for (auto* b : { &edit, &bypass, &up, &down, &remove })
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
            details = s.desc.manufacturerName + "  -  " + s.desc.pluginFormatName;
            bypass.setToggleState(s.node->isBypassed(), juce::dontSendNotification);
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const bool bypassed = bypass.getToggleState();
        const int textX = 20;
        const int textW = juce::jmax(40, edit.getX() - textX - 8);

        g.setColour(bypassed ? Theme::muted() : Theme::accent());
        g.fillRoundedRectangle(8.0f, 12.0f, 4.0f, (float) getHeight() - 24.0f, 2.0f);

        g.setColour(bypassed ? Theme::muted() : Theme::text());
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText(name, textX, 6, textW, getHeight() / 2 - 4, juce::Justification::centredLeft, true);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(details, textX, getHeight() / 2, textW, getHeight() / 2 - 6,
                   juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8, 11);
        remove.setBounds(r.removeFromRight(28));
        r.removeFromRight(4);
        down.setBounds(r.removeFromRight(28));
        r.removeFromRight(4);
        up.setBounds(r.removeFromRight(28));
        r.removeFromRight(10);
        bypass.setBounds(r.removeFromRight(68));
        r.removeFromRight(4);
        edit.setBounds(r.removeFromRight(48));
    }

private:
    RackEngine& engine;
    int row = 0;
    juce::String name, details;
    juce::TextButton edit, bypass, up, down, remove;
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

        // Plugin search
        searchBox.setTextToShowWhenEmpty("Search plugins by name, maker or type...", Theme::muted());
        searchBox.setFont(juce::Font(juce::FontOptions(15.0f)));
        searchBox.setIndents(10, 0);
        searchBox.setSelectAllWhenFocused(true);
        searchBox.onTextChange = [this] { refresh(); };
        searchBox.onEscapeKey = [this] { searchBox.setText("", true); };
        searchBox.onReturnKey = [this] { addFromSearch(); };
        addAndMakeVisible(searchBox);

        pluginList.setModel(&pluginModel);
        rackList.setModel(&rackModel);
        rackList.setRowHeight(52);
        pluginList.setRowHeight(30);
        addAndMakeVisible(pluginList);
        addAndMakeVisible(rackList);

        for (auto* l : { &pluginTitle, &rackTitle, &gainLabel })
        {
            l->setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
            l->setColour(juce::Label::textColourId, Theme::muted());
            addAndMakeVisible(l);
        }
        rackTitle.setText("RACK  -  signal flows top to bottom", juce::dontSendNotification);
        gainLabel.setText("MASTER", juce::dontSendNotification);

        gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        gainSlider.setRange(-60.0, 12.0, 0.1);
        gainSlider.setValue(0.0, juce::dontSendNotification);
        gainSlider.setDoubleClickReturnValue(true, 0.0);
        gainSlider.setTextValueSuffix(" dB");
        gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 22);
        gainSlider.onValueChange = [this]
        {
            engine.setMasterGain(juce::Decibels::decibelsToGain((float) gainSlider.getValue(), -60.0f));
        };
        addAndMakeVisible(gainSlider);

        rebuildFilter();
        setSize(960, 600);
    }

    ~MainComponent() override
    {
        engine.onChanged = nullptr;
        engine.knownPlugins.removeChangeListener(this);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(Theme::background());

        // Title
        g.setColour(Theme::accent());
        g.fillRoundedRectangle(16.0f, 17.0f, 6.0f, 26.0f, 3.0f);
        g.setColour(Theme::text());
        g.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        g.drawText("OpenRack", 32, 12, 220, 36, juce::Justification::centredLeft);

        // Panels behind the two lists
        for (auto* panel : { &pluginPanel, &rackPanel })
        {
            g.setColour(Theme::panel());
            g.fillRoundedRectangle(panel->toFloat(), 10.0f);
            g.setColour(Theme::outline());
            g.drawRoundedRectangle(panel->toFloat().reduced(0.5f), 10.0f, 1.0f);
        }

        // Empty-state hints
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(14.0f));

        if (filtered.empty())
        {
            const bool noPlugins = engine.knownPlugins.getNumTypes() == 0;
            g.drawFittedText(noPlugins ? "No plugins yet.\nClick \"Scan plugins\" to find your VST3 plugins."
                                       : "No plugins match your search.",
                             pluginPanel.reduced(24), juce::Justification::centred, 3);
        }

        if (engine.getNumSlots() == 0)
            g.drawFittedText("The rack is empty.\nDouble-click a plugin on the left, "
                             "or press Enter in the search box, to add it.",
                             rackPanel.reduced(30), juce::Justification::centred, 4);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(16);

        auto top = r.removeFromTop(40);
        for (auto* b : { &loadBtn, &saveBtn, &audioBtn, &scanBtn })
            b->setBounds(top.removeFromRight(116).reduced(3, 3));

        r.removeFromTop(12);

        auto bottom = r.removeFromBottom(34);
        r.removeFromBottom(12);
        gainLabel.setBounds(bottom.removeFromLeft(70));
        gainSlider.setBounds(bottom);

        auto left = r.removeFromLeft(r.getWidth() * 2 / 5);
        r.removeFromLeft(16);

        pluginTitle.setBounds(left.removeFromTop(24));
        searchBox.setBounds(left.removeFromTop(32));
        left.removeFromTop(8);
        pluginPanel = left;
        pluginList.setBounds(left.reduced(6));

        rackTitle.setBounds(r.removeFromTop(24));
        r.removeFromTop(40);
        rackPanel = r;
        rackList.setBounds(r.reduced(6));
    }

private:
    //--------------------------------------------------------------------------
    struct RackModel : juce::ListBoxModel
    {
        explicit RackModel(MainComponent& o) : owner(o) {}
        int getNumRows() override { return owner.engine.getNumSlots(); }
        void paintListBoxItem(int, juce::Graphics& g, int w, int h, bool) override
        {
            g.setColour(Theme::row());
            g.fillRoundedRectangle(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f, 8.0f);
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
        int getNumRows() override { return (int) owner.filtered.size(); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (row < 0 || row >= (int) owner.filtered.size())
                return;

            if (selected)
            {
                g.setColour(Theme::accent().withAlpha(0.28f));
                g.fillRoundedRectangle(2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 6.0f);
            }

            auto& d = owner.filtered[(size_t) row];
            auto area = juce::Rectangle<int>(0, 0, w, h).reduced(10, 0);

            g.setColour(Theme::text());
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(d.name, area.withTrimmedRight(area.getWidth() * 2 / 5),
                       juce::Justification::centredLeft, true);

            g.setColour(Theme::muted());
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(d.manufacturerName, area.removeFromRight(area.getWidth() * 2 / 5),
                       juce::Justification::centredRight, true);
        }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
        {
            owner.addFilteredPlugin(row);
        }
        MainComponent& owner;
    };

    //--------------------------------------------------------------------------
    // Rebuilds the visible plugin list from the search text: every word typed
    // must appear somewhere in the name, maker, category or format.
    void rebuildFilter()
    {
        filtered.clear();

        const auto words = juce::StringArray::fromTokens(searchBox.getText().trim(), " ", "");
        const auto types = engine.knownPlugins.getTypes();

        for (const auto& t : types)
        {
            const juce::String haystack = t.name + " " + t.manufacturerName + " "
                                        + t.category + " " + t.pluginFormatName;
            bool matches = true;
            for (const auto& w : words)
                if (! haystack.containsIgnoreCase(w)) { matches = false; break; }

            if (matches)
                filtered.push_back(t);
        }

        std::sort(filtered.begin(), filtered.end(),
                  [](const juce::PluginDescription& a, const juce::PluginDescription& b)
                  { return a.name.compareIgnoreCase(b.name) < 0; });

        const int total = (int) types.size();
        const int shown = (int) filtered.size();
        pluginTitle.setText(words.isEmpty() ? "PLUGINS  -  " + juce::String(total)
                                            : "PLUGINS  -  " + juce::String(shown) + " of " + juce::String(total),
                            juce::dontSendNotification);
    }

    void addFilteredPlugin(int row)
    {
        if (row < 0 || row >= (int) filtered.size())
            return;

        engine.addPlugin(filtered[(size_t) row], [](const juce::String& err)
        {
            if (err.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Couldn't load plugin", err);
        });
    }

    // Enter in the search box adds the selected result, or the first one.
    void addFromSearch()
    {
        int row = pluginList.getSelectedRow();
        addFilteredPlugin(row >= 0 ? row : 0);
    }

    void refresh()
    {
        rebuildFilter();
        rackList.updateContent();
        rackList.repaint();
        pluginList.updateContent();
        pluginList.repaint();
        repaint();
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
        o.dialogBackgroundColour = Theme::background();
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
        o.dialogBackgroundColour = Theme::background();
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
    std::vector<juce::PluginDescription> filtered;
    juce::ListBox pluginList, rackList;
    juce::TextEditor searchBox;
    juce::TextButton scanBtn, audioBtn, saveBtn, loadBtn;
    juce::Slider gainSlider;
    juce::Label gainLabel, pluginTitle, rackTitle;
    juce::Rectangle<int> pluginPanel, rackPanel;
    juce::File pluginListFile, deadMansFile;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
