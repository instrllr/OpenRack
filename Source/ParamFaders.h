#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "RackEngine.h"
#include "Theme.h"

//==============================================================================
// Parameter faders: pick any parameter of any plugin in the racks and move it with a fader
// (or a MIDI controller). Shown as a strip at the bottom of the rack screen.

// One fader of the strip.
class ParamFaderStrip : public juce::Component, private juce::Timer
{
public:
    std::function<void(int)> onAssign;      // open the parameter picker for this fader

    ParamFaderStrip(RackEngine& e, int idx) : engine(e), index(idx)
    {
        slider.setSliderStyle(juce::Slider::LinearVertical);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(0.0, 1.0, 0.0);
        slider.setDoubleClickReturnValue(false, 0.0);
        slider.onValueChange = [this]
        {
            if (! syncing)
                engine.setParamFaderValue(index, (float) slider.getValue());
        };
        addAndMakeVisible(slider);

        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        startTimerHz(15);
        syncNow();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(3.0f);
        g.setColour(Theme::row());
        g.fillRoundedRectangle(b, 8.0f);

        const auto& pf = engine.getParamFaderInfo(index);
        const bool on = pf.on;
        g.setColour(on ? Theme::accent().withAlpha(0.7f) : Theme::outline());
        g.drawRoundedRectangle(b, 8.0f, 1.0f);

        auto r = getLocalBounds().reduced(8, 6);
        g.setColour(on ? Theme::text() : Theme::muted());
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
        g.drawText(on ? pf.name : "Fader " + juce::String(index + 1), r.removeFromTop(16),
                   juce::Justification::centred, true);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(10.5f));
        g.drawText(on ? pf.plugin : "click to assign", r.removeFromTop(14), juce::Justification::centred, true);

        g.setColour(on ? Theme::text() : Theme::muted());
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(on ? valueText : "-", r.removeFromBottom(16), juce::Justification::centred, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8, 6);
        r.removeFromTop(34);
        r.removeFromBottom(18);
        slider.setBounds(r.withSizeKeepingCentre(36, r.getHeight()));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (slider.getBounds().contains(e.getPosition()))
            return;
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem(1, "Assign parameter...");
            m.addItem(2, "Clear", engine.getParamFaderInfo(index).on);
            m.showMenuAsync({}, [this](int r)
            {
                if (r == 1 && onAssign) onAssign(index);
                if (r == 2) engine.clearParamFader(index);
            });
        }
        else if (onAssign)
            onAssign(index);
    }

    void syncNow()
    {
        const float v = engine.getParamFaderValue(index);
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
            text = engine.getParamFaderText(index);
        }
        else if (engine.getParamFaderInfo(index).on)
            text = "plugin not found";

        if (text != valueText)
        {
            valueText = text;
            repaint();
        }
    }

private:
    void timerCallback() override
    {
        if (isShowing())
            syncNow();
    }

    RackEngine& engine;
    int index;
    juce::Slider slider;
    juce::String valueText;
    bool syncing = false;
};

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
// The strip: a heading and 8 faders (scrolls sideways when there is not enough room).
class ParamFaderBar : public juce::Component
{
public:
    static constexpr int kHeight = 176;

    explicit ParamFaderBar(RackEngine& e) : engine(e)
    {
        for (int i = 0; i < RackEngine::kNumParamFaders; ++i)
        {
            auto* s = new ParamFaderStrip(engine, i);
            s->onAssign = [this](int idx) { openPicker(idx); };
            strips.add(s);
            content.addAndMakeVisible(s);
        }
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(false, true);
        addAndMakeVisible(viewport);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText("PARAMETER FADERS  -  click one to choose a plugin parameter", 4, 0, getWidth() - 8, 18,
                   juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop(20);
        viewport.setBounds(r);

        const int n = strips.size();
        const int w = juce::jmax(92, (viewport.getMaximumVisibleWidth()) / n);
        const int h = viewport.getMaximumVisibleHeight();
        content.setSize(w * n, h);
        for (int i = 0; i < n; ++i)
            strips[i]->setBounds(i * w, 0, w, h);
    }

    void refresh() { for (auto* s : strips) s->syncNow(); repaint(); }

private:
    void openPicker(int index)
    {
        auto* picker = new ParamPicker(engine, index);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(picker);
        o.dialogTitle = "Parameter fader " + juce::String(index + 1);
        o.dialogBackgroundColour = Theme::background();
        o.componentToCentreAround = this;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    RackEngine& engine;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<ParamFaderStrip> strips;
};
