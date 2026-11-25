#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "RackEngine.h"
#include "Theme.h"

//==============================================================================
// Parameter faders: pick any parameter of any plugin in the racks and move it with a fader
// (or a MIDI controller). Shown as a strip at the bottom of the rack screen.

//==============================================================================
// Choose the plugin (from the racks), then one of its parameters.
class ParamPicker : public juce::Component, private juce::ListBoxModel
{
public:
    ParamPicker(RackEngine& e, int faderIndex) : engine(e), index(faderIndex)
    {
        sources = engine.listParamSources();

        for (size_t i = 0; i < sources.size(); ++i)
            pluginBox.addItem(sources[i].label, (int) i + 1);
        pluginBox.onChange = [this] { refillParams(); };
        addAndMakeVisible(pluginBox);

        search.setTextToShowWhenEmpty("Search parameters...", Theme::muted());
        search.onTextChange = [this] { refillParams(); };
        addAndMakeVisible(search);

        list.setModel(this);
        list.setRowHeight(26);
        addAndMakeVisible(list);

        info.setColour(juce::Label::textColourId, Theme::muted());
        info.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(info);

        assign.setButtonText("Assign");
        assign.onClick = [this] { commit(); };
        cancel.setButtonText("Cancel");
        cancel.onClick = [this] { close(); };
        addAndMakeVisible(assign);
        addAndMakeVisible(cancel);

        // start on the plugin that is already assigned, or the first one
        int start = 0;
        const auto& pf = engine.getParamFaderInfo(index);
        for (size_t i = 0; i < sources.size(); ++i)
            if (pf.on && sources[i].rackId == pf.rackId && sources[i].slot == pf.slot)
                start = (int) i;
        if (! sources.empty())
            pluginBox.setSelectedId(start + 1, juce::sendNotification);
        else
            info.setText("Add a plugin to a rack first.", juce::dontSendNotification);

        setSize(520, 460);
    }

    void paint(juce::Graphics& g) override { g.fillAll(Theme::background()); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14);
        pluginBox.setBounds(r.removeFromTop(28));
        r.removeFromTop(8);
        search.setBounds(r.removeFromTop(28));
        r.removeFromTop(8);
        auto bottom = r.removeFromBottom(32);
        cancel.setBounds(bottom.removeFromRight(90));
        bottom.removeFromRight(8);
        assign.setBounds(bottom.removeFromRight(90));
        info.setBounds(bottom);
        r.removeFromBottom(8);
        list.setBounds(r);
    }

private:
    void refillParams()
    {
        shown.clear();
        const int s = pluginBox.getSelectedId() - 1;
        if (s < 0 || s >= (int) sources.size())
        {
            list.updateContent();
            return;
        }

        current = sources[(size_t) s];
        const auto words = juce::StringArray::fromTokens(search.getText().trim(), " ", "");
        auto* proc = engine.getProcessorFor(current.rackId, current.slot);
        if (proc != nullptr)
        {
            const auto& params = proc->getParameters();
            for (int i = 0; i < params.size(); ++i)
            {
                const auto name = params[i]->getName(64);
                bool ok = name.isNotEmpty();
                for (auto& w : words)
                    if (! name.containsIgnoreCase(w)) { ok = false; break; }
                if (ok) shown.push_back(i);
            }
            info.setText(params.isEmpty() ? "This plugin does not expose any parameters."
                                          : juce::String((int) shown.size()) + " of " + juce::String(params.size()) + " parameters",
                         juce::dontSendNotification);
        }
        else
            info.setText("This plugin runs in its own process, so its parameters are not available.",
                         juce::dontSendNotification);

        list.updateContent();
        list.repaint();
    }

    int getNumRows() override { return (int) shown.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) shown.size()) return;
        if (selected)
        {
            g.setColour(Theme::accent().withAlpha(0.3f));
            g.fillRoundedRectangle(2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 5.0f);
        }
        auto* proc = engine.getProcessorFor(current.rackId, current.slot);
        if (proc == nullptr) return;
        auto* p = proc->getParameters()[shown[(size_t) row]];
        g.setColour(Theme::text());
        g.setFont(juce::FontOptions(13.5f));
        g.drawText(p->getName(64), 10, 0, w / 2 + 40, h, juce::Justification::centredLeft, true);
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(p->getText(p->getValue(), 16) + " " + p->getLabel(), w / 2 + 50, 0, w - w / 2 - 60, h,
                   juce::Justification::centredRight, true);
    }

    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { commit(); }

    void commit()
    {
        const int row = list.getSelectedRow();
        if (row < 0 || row >= (int) shown.size())
        {
            info.setText("Select a parameter first.", juce::dontSendNotification);
            return;
        }
        engine.assignParamFader(index, current.rackId, current.slot, shown[(size_t) row]);
        close();
    }

    void close()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }

    RackEngine& engine;
    int index;
    std::vector<RackEngine::ParamSource> sources;
    RackEngine::ParamSource current;
    std::vector<int> shown;
    juce::ComboBox pluginBox;
    juce::TextEditor search;
    juce::ListBox list;
    juce::Label info;
    juce::TextButton assign, cancel;
};

//==============================================================================
// The parameter fader: one fader that follows the last parameter you touched in any plugin's
// editor (or the one you chose by hand while locked).
class ParamFaderBar : public juce::Component, private juce::Timer
{
public:
    static constexpr int kHeight = 78;

    explicit ParamFaderBar(RackEngine& e) : engine(e)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(0.0, 1.0, 0.0);
        slider.onValueChange = [this]
        {
            if (! syncing)
                engine.setParamFaderValue(0, (float) slider.getValue());
        };
        addAndMakeVisible(slider);

        lockBtn.setButtonText("Lock");
        lockBtn.setColour(juce::ToggleButton::textColourId, Theme::text());
        lockBtn.setTooltip("Locked: the fader stays on its parameter. Unlocked: it jumps to whichever "
                           "plugin parameter you touch next.");
        lockBtn.onClick = [this] { engine.setParamFaderLocked(lockBtn.getToggleState()); };
        addAndMakeVisible(lockBtn);

        chooseBtn.setButtonText("Choose...");
        chooseBtn.setTooltip("Pick a parameter from a list (this locks the fader to it)");
        chooseBtn.onClick = [this] { openPicker(); };
        addAndMakeVisible(chooseBtn);

        startTimerHz(15);
        refresh();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText("PARAMETER FADER  -  follows the last plugin parameter you touch", 4, 0, getWidth() - 8, 18,
                   juce::Justification::centredLeft);

        auto row = getLocalBounds().withTrimmedTop(22).reduced(2);
        g.setColour(Theme::row());
        g.fillRoundedRectangle(row.toFloat(), 8.0f);

        const auto& pf = engine.getParamFaderInfo(0);
        auto text = row.reduced(12, 8).removeFromLeft(nameWidth);
        g.setColour(pf.on ? Theme::text() : Theme::muted());
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(pf.on ? pf.name : "Nothing selected yet", text.removeFromTop(text.getHeight() / 2),
                   juce::Justification::centredLeft, true);
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(11.5f));
        g.drawText(pf.on ? pf.plugin : "Open a plugin and move one of its controls",
                   text, juce::Justification::centredLeft, true);

        g.setColour(pf.on ? Theme::text() : Theme::muted());
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(pf.on ? valueText : "-", valueArea, juce::Justification::centred, true);
    }

    void resized() override
    {
        auto row = getLocalBounds().withTrimmedTop(22).reduced(2).reduced(12, 8);
        nameWidth = juce::jlimit(120, 260, row.getWidth() / 4);
        row.removeFromLeft(nameWidth + 10);
        chooseBtn.setBounds(row.removeFromRight(88).reduced(0, 4));
        row.removeFromRight(8);
        lockBtn.setBounds(row.removeFromRight(64));
        row.removeFromRight(8);
        valueArea = row.removeFromRight(96);
        slider.setBounds(row);
    }

    void refresh()
    {
        lockBtn.setToggleState(engine.isParamFaderLocked(), juce::dontSendNotification);
        syncNow();
        repaint();
    }

private:
    void syncNow()
    {
        const float v = engine.getParamFaderValue(0);
        const bool usable = v >= 0.0f;
        slider.setEnabled(usable);
        slider.setAlpha(usable ? 1.0f : 0.35f);

        juce::String text;
        if (usable)
        {
            if (! slider.isMouseButtonDown())
            {
                syncing = true;
                slider.setValue(v, juce::dontSendNotification);
                syncing = false;
            }
            text = engine.getParamFaderText(0);
        }
        else if (engine.getParamFaderInfo(0).on)
            text = "plugin not found";

        if (text != valueText)
        {
            valueText = text;
            repaint();
        }
    }

    void timerCallback() override
    {
        if (isShowing())
            syncNow();
    }

    void openPicker()
    {
        auto* picker = new ParamPicker(engine, 0);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(picker);
        o.dialogTitle = "Parameter fader";
        o.dialogBackgroundColour = Theme::background();
        o.componentToCentreAround = this;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    RackEngine& engine;
    juce::Slider slider;
    juce::ToggleButton lockBtn;
    juce::TextButton chooseBtn;
    juce::String valueText;
    juce::Rectangle<int> valueArea;
    int nameWidth = 180;
    bool syncing = false;
};
