#pragma once
#include <algorithm>
#include "RackEngine.h"
#include "Theme.h"
#include "HotPlugins.h"

static constexpr int kRackRowHeight = 56;

static int measureText(const juce::String& text, float height, bool bold = false)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText(juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain)),
                     text, 0.0f, 0.0f);
    return (int) std::ceil(ga.getBoundingBox(0, -1, true).getWidth());
}

//==============================================================================
// Vertical stereo level meter with dB ticks, peak hold and a clip light on top.
// `source(ch)` returns the peak since it was last asked (0 = left, 1 = right).
class LevelMeter : public juce::Component,
                   public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    LevelMeter(std::function<float(int)> peakSource, juce::String labelText, juce::String tip)
        : source(std::move(peakSource)), label(std::move(labelText))
    {
        setTooltip(tip + " Click to reset the clip light.");
        startTimerHz(30);
    }

    void mouseDown(const juce::MouseEvent&) override { clipped = false; repaint(); }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(label, area.removeFromBottom(20.0f), juce::Justification::centred);

        // Clip light
        auto led = area.removeFromTop(8.0f);
        g.setColour(clipped ? juce::Colour(0xffff5252) : Theme::control());
        g.fillRoundedRectangle(led.reduced(1.0f, 0.0f), 3.0f);
        area.removeFromTop(3.0f);

        const float gap = 3.0f;
        const float barW = (area.getWidth() - gap) / 2.0f;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = juce::Rectangle<float>(area.getX() + (float) ch * (barW + gap), area.getY(),
                                              barW, area.getHeight());
            g.setColour(Theme::control());
            g.fillRoundedRectangle(bar, barW * 0.5f);

            const float frac = toFraction(level[ch]);
            if (frac > 0.0f)
            {
                juce::ColourGradient grad(juce::Colour(0xffff5252), 0.0f, bar.getY(),
                                          juce::Colour(0xff3ddc84), 0.0f, bar.getBottom(), false);
                grad.addColour(0.22, Theme::warning());
                grad.addColour(0.40, juce::Colour(0xff3ddc84));
                g.setGradientFill(grad);
                const float h = juce::jmax(barW, bar.getHeight() * frac);
                g.fillRoundedRectangle(bar.withTop(bar.getBottom() - h), barW * 0.5f);
            }

            if (hold[ch] > 0.001f)
            {
                g.setColour(Theme::text().withAlpha(0.85f));
                g.fillRect(juce::Rectangle<float>(bar.getX(), bar.getBottom() - bar.getHeight() * toFraction(hold[ch]) - 1.0f,
                                                  barW, 2.0f));
            }
        }

        // dB ticks
        g.setColour(juce::Colours::white.withAlpha(0.14f));
        for (float db : { -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f })
        {
            const float y = area.getBottom() - area.getHeight() * dbToFraction(db);
            g.fillRect(juce::Rectangle<float>(area.getX(), y, area.getWidth(), 1.0f));
        }
    }

private:
    static float dbToFraction(float db) { return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f); }   // -60 .. +6 dB
    static float toFraction(float gain) { return dbToFraction(juce::Decibels::gainToDecibels(gain, -60.0f)); }

    void timerCallback() override
    {
        if (! isShowing())
            return;

        for (int ch = 0; ch < 2; ++ch)
        {
            const float raw = source(ch);
            if (raw >= 1.0f) clipped = true;

            level[ch] = juce::jmax(raw, level[ch] * 0.90f);

            if (level[ch] >= hold[ch]) { hold[ch] = level[ch]; holdTicks[ch] = 30; }
            else if (holdTicks[ch] > 0) --holdTicks[ch];
            else hold[ch] *= 0.93f;
        }
        repaint();
    }

    std::function<float(int)> source;
    juce::String label;
    float level[2] { 0.0f, 0.0f };
    float hold[2] { 0.0f, 0.0f };
    int holdTicks[2] { 0, 0 };
    bool clipped = false;
};

//==============================================================================
// One row in the rack list: grip + number + name + Edit / Bypass / Up / Down / Remove.
// Mouse clicks on the empty parts fall through to the list, so rows can be
// selected and dragged to reorder.
class SlotRow : public juce::Component
{
public:
    explicit SlotRow(RackEngine& e) : engine(e)
    {
        setInterceptsMouseClicks(false, true);

        edit.setButtonText("Edit");
        bypass.setButtonText("Bypass");
        bypass.setClickingTogglesState(true);
        up.setButtonText("^");
        down.setButtonText("v");
        remove.setButtonText("X");

        edit.setTooltip("Open this plugin's window");
        bypass.setTooltip("Switch this plugin off without removing it");
        up.setTooltip("Move up (earlier in the chain)");
        down.setTooltip("Move down (later in the chain)");
        remove.setTooltip("Remove from the rack");

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

            const int lat = s.node->getProcessor()->getLatencySamples();
            if (lat > 0)
                details += "  -  " + juce::String(lat) + " smp latency";

            bypass.setToggleState(s.node->isBypassed(), juce::dontSendNotification);
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const bool bypassed = bypass.getToggleState();
        const int textX = 66;
        const int textW = juce::jmax(40, edit.getX() - textX - 8);
        const float cy = (float) getHeight() * 0.5f;

        // Drag grip
        g.setColour(Theme::muted().withAlpha(0.6f));
        for (int ry = -1; ry <= 1; ++ry)
            for (int cx = 0; cx < 2; ++cx)
                g.fillEllipse(10.0f + (float) cx * 5.0f, cy + (float) ry * 7.0f - 1.5f, 3.0f, 3.0f);

        // Colour bar, tinted per plugin
        g.setColour(bypassed ? Theme::muted() : Theme::tint(name));
        g.fillRoundedRectangle(26.0f, 12.0f, 4.0f, (float) getHeight() - 24.0f, 2.0f);

        // Slot number
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(juce::String(row + 1).paddedLeft('0', 2), 34, 0, 26, getHeight(),
                   juce::Justification::centred);

        g.setColour(bypassed ? Theme::muted() : Theme::text());
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText(name, textX, 7, textW, getHeight() / 2 - 5, juce::Justification::centredLeft, true);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(details, textX, getHeight() / 2, textW, getHeight() / 2 - 7,
                   juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8, 12);
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
// A quick-grab chip for the hot plugins strip. Click to add it to the rack,
// drag it to drop it at an exact spot, right-click for pin/unpin.
class HotChip : public juce::Component,
                public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick, onMenu;

    HotChip(juce::String pluginId, juce::String pluginName, bool isPinned)
        : id(std::move(pluginId)), name(std::move(pluginName)), pinned(isPinned)
    {
        setTooltip("Click to add " + name + " to the rack. Drag it to choose where. Right-click for options.");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    static int widthFor(const juce::String& name) { return measureText(name, 13.0f) + 46; }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(hover ? Theme::control().brighter(0.18f) : Theme::control());
        g.fillRoundedRectangle(b, b.getHeight() * 0.5f);
        g.setColour(hover ? Theme::accent().withAlpha(0.8f) : Theme::outline());
        g.drawRoundedRectangle(b, b.getHeight() * 0.5f, 1.0f);

        const float cy = b.getCentreY();
        if (pinned)
        {
            juce::Path star;
            star.addStar({ 17.0f, cy }, 5, 3.0f, 6.5f);
            g.setColour(Theme::warning());
            g.fillPath(star);
        }
        else
        {
            g.setColour(Theme::tint(name));
            g.fillEllipse(13.0f, cy - 4.0f, 8.0f, 8.0f);
        }

        g.setColour(Theme::text());
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(name, 28, 0, getWidth() - 36, getHeight(), juce::Justification::centredLeft, true);
    }

    void mouseEnter(const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover = false; repaint(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && onMenu)
            onMenu();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown() || e.getDistanceFromDragStart() < 6)
            return;

        if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this))
            if (! c->isDragAndDropActive())
                c->startDragging("add:" + id, this);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && ! e.mouseWasDraggedSinceMouseDown()
             && contains(e.getPosition()) && onClick)
            onClick();
    }

private:
    juce::String id, name;
    bool pinned = false, hover = false;
};

//==============================================================================
// Shows one hosted plugin's own interface inside OpenRack's window, under a
// "Back to rack" bar. Used when running inside a DAW, so plugin windows can
// never end up hidden behind the DAW.
class InlineEditorHost : public juce::Component,
                         private juce::ComponentListener
{
public:
    std::function<void()> onBack;
    std::function<void(int, int)> onWantedSize;   // window size the editor needs

    InlineEditorHost()
    {
        backBtn.setButtonText("< Back to rack");
        backBtn.onClick = [this] { if (onBack) onBack(); };
        addAndMakeVisible(backBtn);

        title.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        title.setColour(juce::Label::textColourId, Theme::text());
        addAndMakeVisible(title);
    }

    ~InlineEditorHost() override { clear(); }

    void show(juce::AudioProcessor& p, const juce::String& name)
    {
        clear();

        juce::AudioProcessorEditor* ed = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (ed == nullptr)
            ed = new juce::GenericAudioProcessorEditor(p);

        editor.reset(ed);
        title.setText(name, juce::dontSendNotification);
        addAndMakeVisible(*editor);
        editor->addComponentListener(this);

        layoutEditor();
        notifyWantedSize();
    }

    void clear()
    {
        if (editor != nullptr)
        {
            editor->removeComponentListener(this);
            removeChildComponent(editor.get());
            editor.reset();
        }
    }

    void paint(juce::Graphics& g) override { g.fillAll(Theme::background()); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(16);
        auto top = r.removeFromTop(36);
        backBtn.setBounds(top.removeFromLeft(140));
        title.setBounds(top.reduced(14, 0));
        r.removeFromTop(12);
        area = r;
        layoutEditor();
    }

private:
    void layoutEditor()
    {
        if (editor != nullptr)
            editor->setTopLeftPosition(juce::jmax(0, area.getCentreX() - editor->getWidth() / 2), area.getY());
    }

    void notifyWantedSize()
    {
        if (editor != nullptr && onWantedSize)
            onWantedSize(editor->getWidth() + 32, editor->getHeight() + 32 + 36 + 12);
    }

    void componentMovedOrResized(juce::Component&, bool, bool wasResized) override
    {
        if (wasResized)
        {
            layoutEditor();
            notifyWantedSize();
        }
    }

    juce::TextButton backBtn;
    juce::Label title;
    juce::Rectangle<int> area;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

//==============================================================================
// One input bus of the plugin version: mute, solo, gain and a level meter.
class InputStrip : public juce::Component, private juce::Timer
{
public:
    static constexpr int kHeight = 38;

    InputStrip(RackEngine& e, int index) : engine(e), idx(index)
    {
        mute.setButtonText("M");
        solo.setButtonText("S");
        mute.setClickingTogglesState(true);
        solo.setClickingTogglesState(true);
        mute.setTooltip("Mute this input");
        solo.setTooltip("Solo this input");
        mute.setColour(juce::TextButton::buttonOnColourId, Theme::warning());
        solo.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3ddc84));
        mute.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        solo.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        addAndMakeVisible(mute);
        addAndMakeVisible(solo);

        gain.setSliderStyle(juce::Slider::LinearHorizontal);
        gain.setRange(-60.0, 12.0, 0.1);
        gain.setDoubleClickReturnValue(true, 0.0);
        gain.setTextValueSuffix(" dB");
        gain.setTextBoxStyle(juce::Slider::TextBoxRight, false, 68, 20);
        gain.setTooltip("Level this input is mixed into the rack at (double-click for 0 dB)");
        addAndMakeVisible(gain);

        mute.onClick = [this] { bus().mute.store(mute.getToggleState()); };
        solo.onClick = [this] { bus().solo.store(solo.getToggleState()); };
        gain.onValueChange = [this]
        {
            bus().gain.store(juce::Decibels::decibelsToGain((float) gain.getValue(), -60.0f));
        };

        sync();
        setAlpha(0.5f);
        startTimerHz(20);
    }

    // Pull the controls into line with the engine (after loading a rack or snapshot).
    void sync()
    {
        gain.setValue(juce::Decibels::gainToDecibels(bus().gain.load(), -60.0f), juce::dontSendNotification);
        mute.setToggleState(bus().mute.load(), juce::dontSendNotification);
        solo.setToggleState(bus().solo.load(), juce::dontSendNotification);
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(2.0f, 2.0f);
        g.setColour(Theme::row());
        g.fillRoundedRectangle(r, 6.0f);

        g.setColour(lastActive ? Theme::text() : Theme::muted());
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("IN " + juce::String(idx + 1), 14, 0, 50, getHeight(), juce::Justification::centredLeft);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(idx == 0 ? "main" : (lastActive ? "on" : "off"), 56, 0, 40, getHeight(),
                   juce::Justification::centredLeft);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = juce::Rectangle<float>((float) meterArea.getX(),
                                              (float) meterArea.getCentreY() - 6.0f + (float) ch * 8.0f,
                                              (float) meterArea.getWidth(), 5.0f);
            g.setColour(Theme::control());
            g.fillRoundedRectangle(bar, 2.5f);

            const float db = juce::Decibels::gainToDecibels(level[ch], -60.0f);
            const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f);
            if (frac > 0.0f)
            {
                g.setColour(db > -1.0f ? juce::Colour(0xffff5252) : db > -12.0f ? Theme::warning()
                                                                                : juce::Colour(0xff3ddc84));
                g.fillRoundedRectangle(bar.withWidth(juce::jmax(5.0f, bar.getWidth() * frac)), 2.5f);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(6, 6);
        r.removeFromLeft(92);
        mute.setBounds(r.removeFromLeft(28));
        r.removeFromLeft(4);
        solo.setBounds(r.removeFromLeft(28));
        r.removeFromLeft(8);
        meterArea = r.removeFromRight(juce::jmin(150, r.getWidth() / 3)).reduced(0, 0);
        r.removeFromRight(10);
        gain.setBounds(r);
    }

private:
    RackEngine::InputBus& bus() { return engine.inputBuses[(size_t) idx]; }

    void timerCallback() override
    {
        if (! isShowing())
            return;

        const bool on = bus().active.load();
        if (on != lastActive)
        {
            lastActive = on;
            setAlpha(on ? 1.0f : 0.5f);
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            const float raw = (ch == 0 ? bus().peakL : bus().peakR).exchange(0.0f);
            level[ch] = juce::jmax(raw, level[ch] * 0.88f);
        }
        repaint();
    }

    RackEngine& engine;
    const int idx;
    juce::TextButton mute, solo;
    juce::Slider gain;
    juce::Rectangle<int> meterArea;
    float level[2] { 0.0f, 0.0f };
    bool lastActive = false;
};

// Full-width view listing all 64 input buses (plugin version only).
class InputsPanel : public juce::Component
{
public:
    explicit InputsPanel(RackEngine& e)
    {
        for (int i = 0; i < kMaxInputBuses; ++i)
        {
            strips.push_back(std::make_unique<InputStrip>(e, i));
            content.addAndMakeVisible(*strips.back());
        }
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);
    }

    void sync() { for (auto& s : strips) s->sync(); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(Theme::background());
        auto p = getLocalBounds().toFloat();
        g.setColour(Theme::panel());
        g.fillRoundedRectangle(p, 10.0f);
        g.setColour(Theme::outline());
        g.drawRoundedRectangle(p.reduced(0.5f), 10.0f, 1.0f);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
        g.drawText("INPUT BUSES  -  " + juce::String(kMaxInputBuses) + " stereo inputs you can patch to racks",
                   16, 8, getWidth() - 32, 20, juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Inputs your DAW has switched on are lit; choose each rack's In / Out pair in the rack header. "
                   "FL Studio: send a mixer track to an OpenRack input. Logic Pro: input 2 is the side-chain.",
                   16, 28, getWidth() - 32, 18, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        viewport.setBounds(getLocalBounds().withTrimmedTop(54).reduced(8, 6));
        const int w = juce::jmax(100, viewport.getMaximumVisibleWidth());
        content.setSize(w, kMaxInputBuses * InputStrip::kHeight);
        for (size_t i = 0; i < strips.size(); ++i)
            strips[i]->setBounds(0, (int) i * InputStrip::kHeight, w, InputStrip::kHeight);
    }

private:
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<InputStrip>> strips;
};

// Snapshot button: left click recalls (or stores into an empty slot), right click opens a menu.
class SnapButton : public juce::TextButton
{
public:
    std::function<void()> onRightClick;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick) onRightClick();
            return;
        }
        juce::TextButton::mouseDown(e);
    }
};

// One row in the racks sidebar: number, name, patching summary, mute / solo and a level meter.
// Clicks on the empty parts fall through to the list, which selects the rack.
class RackRow : public juce::Component, private juce::Timer
{
public:
    static constexpr int kHeight = 48;

    explicit RackRow(RackEngine& e) : engine(e)
    {
        setInterceptsMouseClicks(false, true);

        mute.setButtonText("M");
        solo.setButtonText("S");
        mute.setClickingTogglesState(true);
        solo.setClickingTogglesState(true);
        mute.setTooltip("Mute this rack");
        solo.setTooltip("Solo this rack");
        mute.setColour(juce::TextButton::buttonOnColourId, Theme::warning());
        solo.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3ddc84));
        mute.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        solo.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        addAndMakeVisible(mute);
        addAndMakeVisible(solo);

        mute.onClick = [this] { engine.setRackMute(row, mute.getToggleState()); };
        solo.onClick = [this] { engine.setRackSolo(row, solo.getToggleState()); };
        startTimerHz(20);
    }

    static juce::String pairName(int pair)
    {
        return pair < 0 ? juce::String("-") : juce::String(pair * 2 + 1) + "-" + juce::String(pair * 2 + 2);
    }

    void setRow(int r)
    {
        row = r;
        name = engine.getRackName(r);
        const int n = engine.getRackNumSlots(r);
        details = "In " + pairName(engine.getRackInputPair(r)) + "  >  Out " + pairName(engine.getRackOutputPair(r))
                  + "   -   " + juce::String(n) + (n == 1 ? " plugin" : " plugins");
        mute.setToggleState(engine.isRackMuted(r), juce::dontSendNotification);
        solo.setToggleState(engine.isRackSoloed(r), juce::dontSendNotification);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(mute.getToggleState() ? Theme::muted() : Theme::tint(name));
        g.fillRoundedRectangle(8.0f, 10.0f, 4.0f, (float) getHeight() - 20.0f, 2.0f);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(juce::String(row + 1), 14, 0, 24, getHeight() - 8, juce::Justification::centred);

        const int textW = juce::jmax(40, mute.getX() - 44 - 6);
        g.setColour(mute.getToggleState() ? Theme::muted() : Theme::text());
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(name, 44, 5, textW, 18, juce::Justification::centredLeft, true);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(details, 44, 22, textW, 14, juce::Justification::centredLeft, true);

        // level meter along the bottom
        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = juce::Rectangle<float>(44.0f, (float) getHeight() - 11.0f + (float) ch * 4.0f,
                                              (float) textW, 3.0f);
            g.setColour(Theme::control());
            g.fillRoundedRectangle(bar, 1.5f);

            const float db = juce::Decibels::gainToDecibels(level[ch], -60.0f);
            const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f);
            if (frac > 0.0f)
            {
                g.setColour(db > -1.0f ? juce::Colour(0xffff5252) : db > -12.0f ? Theme::warning()
                                                                                : juce::Colour(0xff3ddc84));
                g.fillRoundedRectangle(bar.withWidth(juce::jmax(3.0f, bar.getWidth() * frac)), 1.5f);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(6, 10);
        r.removeFromBottom(4);
        solo.setBounds(r.removeFromRight(26));
        r.removeFromRight(3);
        mute.setBounds(r.removeFromRight(26));
    }

private:
    void timerCallback() override
    {
        if (! isShowing())
            return;

        for (int ch = 0; ch < 2; ++ch)
            level[ch] = juce::jmax(engine.takeRackPeak(row, ch), level[ch] * 0.88f);
        repaint();
    }

    RackEngine& engine;
    int row = 0;
    juce::String name, details;
    juce::TextButton mute, solo;
    float level[2] { 0.0f, 0.0f };
};

//==============================================================================
// The channel-strip EQ of the selected rack: a response curve with draggable
// bands (drag = frequency and gain, scroll wheel = Q, double-click = reset a
// band) and exact values underneath.
class EqPanel : public juce::Component
{
public:
    explicit EqPanel(RackEngine& e) : engine(e)
    {
        onBtn.setButtonText("EQ on");
        onBtn.setClickingTogglesState(true);
        onBtn.setColour(juce::TextButton::buttonOnColourId, Theme::accent());
        onBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        onBtn.setTooltip("Switch the whole channel EQ on or off");
        onBtn.onClick = [this]
        {
            if (auto* q = eq())
            {
                q->enabled = onBtn.getToggleState();
                q->touch();
                engine.markDirty();
                repaint();
            }
        };
        addAndMakeVisible(onBtn);

        resetBtn.setButtonText("Reset");
        resetBtn.setTooltip("Put every band back to flat");
        resetBtn.onClick = [this]
        {
            if (auto* q = eq())
            {
                q->setDefaults();
                engine.markDirty();
                syncAll();
            }
        };
        addAndMakeVisible(resetBtn);

        for (int b = 0; b < ChannelEq::kBands; ++b)
        {
            on[b].setButtonText("On");
            on[b].setClickingTogglesState(true);
            on[b].setColour(juce::TextButton::buttonOnColourId, bandColour(b));
            on[b].setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            on[b].onClick = [this, b] { applyFromControls(b); };
            addAndMakeVisible(on[b]);

            name[b].setText(ChannelEq::bandName(b), juce::dontSendNotification);
            name[b].setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
            name[b].setColour(juce::Label::textColourId, bandColour(b));
            addAndMakeVisible(name[b]);

            freq[b].setSliderStyle(juce::Slider::LinearBar);
            freq[b].setRange(20.0, 20000.0, 0.1);
            freq[b].setSkewFactorFromMidPoint(1000.0);
            freq[b].setTextValueSuffix(" Hz");
            freq[b].setTooltip("Frequency");

            gain[b].setSliderStyle(juce::Slider::LinearBar);
            gain[b].setRange(-18.0, 18.0, 0.1);
            gain[b].setDoubleClickReturnValue(true, 0.0);
            gain[b].setTextValueSuffix(" dB");
            gain[b].setTooltip("Gain (double-click for 0 dB)");

            q[b].setSliderStyle(juce::Slider::LinearBar);
            q[b].setRange(0.2, 10.0, 0.01);
            q[b].setSkewFactorFromMidPoint(1.0);
            q[b].setTextValueSuffix(" Q");
            q[b].setTooltip("Width: higher Q is narrower");

            for (auto* sl : { &freq[b], &gain[b], &q[b] })
            {
                sl->onValueChange = [this, b] { applyFromControls(b); };
                addAndMakeVisible(sl);
            }
        }
        gain[0].setVisible(false);      // the high-pass has no gain
    }

    // Show the EQ of this rack.
    void attach(int rackIndex)
    {
        rack = rackIndex;
        syncAll();
    }

    void paint(juce::Graphics& g) override
    {
        auto p = getLocalBounds().toFloat();
        g.setColour(Theme::panel());
        g.fillRoundedRectangle(p, 10.0f);
        g.setColour(Theme::outline());
        g.drawRoundedRectangle(p.reduced(0.5f), 10.0f, 1.0f);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
        g.drawText("CHANNEL EQ  -  RACK " + juce::String(rack + 1) + "  " + engine.getRackName(rack).toUpperCase(),
                   100, 12, getWidth() - 190, 28, juce::Justification::centredLeft, true);

        // Graph background and grid
        g.setColour(Theme::background().darker(0.3f));
        g.fillRoundedRectangle(graph, 8.0f);

        g.setFont(juce::FontOptions(10.5f));
        for (float f : { 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f, 100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f,
                         700.0f, 800.0f, 900.0f, 1000.0f, 2000.0f, 3000.0f, 4000.0f, 5000.0f, 6000.0f, 7000.0f,
                         8000.0f, 9000.0f, 10000.0f })
        {
            const bool major = (f == 100.0f || f == 1000.0f || f == 10000.0f);
            g.setColour(juce::Colours::white.withAlpha(major ? 0.14f : 0.05f));
            g.fillRect(juce::Rectangle<float>(freqToX(f), graph.getY(), 1.0f, graph.getHeight()));
            if (major)
            {
                g.setColour(Theme::muted());
                g.drawText(f >= 1000.0f ? juce::String((int) (f / 1000.0f)) + "k" : juce::String((int) f),
                           (int) freqToX(f) + 3, (int) graph.getBottom() - 15, 30, 13, juce::Justification::left);
            }
        }
        for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
        {
            g.setColour(juce::Colours::white.withAlpha(db == 0.0f ? 0.22f : 0.07f));
            g.fillRect(juce::Rectangle<float>(graph.getX(), dbToY(db), graph.getWidth(), 1.0f));
            g.setColour(Theme::muted());
            g.drawText(juce::String((int) db), (int) graph.getX() + 4, (int) dbToY(db) - 12, 28, 12,
                       juce::Justification::left);
        }

        auto* q0 = eq();
        if (q0 == nullptr)
            return;

        // Combined response
        juce::Path curve;
        for (float x = 0.0f; x <= graph.getWidth(); x += 2.0f)
        {
            const double db = juce::jlimit(-18.0, 18.0, q0->totalResponseDb(xToFreq(graph.getX() + x)));
            const float y = dbToY((float) db);
            if (x == 0.0f) curve.startNewSubPath(graph.getX() + x, y);
            else           curve.lineTo(graph.getX() + x, y);
        }

        const bool enabled = q0->enabled.load();
        juce::Path fill(curve);
        fill.lineTo(graph.getRight(), dbToY(0.0f));
        fill.lineTo(graph.getX(), dbToY(0.0f));
        fill.closeSubPath();
        g.setColour(Theme::accent().withAlpha(enabled ? 0.16f : 0.05f));
        g.fillPath(fill);
        g.setColour(enabled ? Theme::accent() : Theme::muted());
        g.strokePath(curve, juce::PathStrokeType(2.0f));

        // Band handles
        for (int b = 0; b < ChannelEq::kBands; ++b)
        {
            const auto pt = handlePos(b);
            const bool bandOn = q0->bands[b].on.load();
            g.setColour(bandColour(b).withAlpha(bandOn ? 1.0f : 0.35f));
            g.fillEllipse(pt.x - 7.0f, pt.y - 7.0f, 14.0f, 14.0f);
            if (b == selected)
            {
                g.setColour(juce::Colours::white);
                g.drawEllipse(pt.x - 9.0f, pt.y - 9.0f, 18.0f, 18.0f, 1.5f);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        auto top = r.removeFromTop(28);
        onBtn.setBounds(top.removeFromLeft(80));
        resetBtn.setBounds(top.removeFromRight(70));
        r.removeFromTop(8);

        const int rowsH = juce::jlimit(120, 190, r.getHeight() / 2);
        graph = r.removeFromTop(juce::jmax(110, r.getHeight() - rowsH - 8)).toFloat();
        r.removeFromTop(8);

        const int rowH = juce::jlimit(24, 36, r.getHeight() / ChannelEq::kBands);
        for (int b = 0; b < ChannelEq::kBands; ++b)
        {
            auto row = r.removeFromTop(rowH).reduced(0, 2);
            on[b].setBounds(row.removeFromLeft(42));
            row.removeFromLeft(6);
            name[b].setBounds(row.removeFromLeft(52));
            const int w = row.getWidth();
            freq[b].setBounds(row.removeFromLeft(w * 40 / 100));
            row.removeFromLeft(6);
            if (b == 0)
            {
                q[b].setBounds(row.removeFromRight(juce::jmin(row.getWidth(), w * 28 / 100)));
            }
            else
            {
                gain[b].setBounds(row.removeFromLeft(w * 29 / 100));
                row.removeFromLeft(6);
                q[b].setBounds(row);
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging = -1;
        if (! graph.contains(e.position))
            return;

        const int b = nearestBand(e.position);
        selected = b;
        dragging = b;

        if (b >= 0)
            if (auto* q0 = eq())
                if (! q0->bands[b].on.load())
                {
                    q0->bands[b].on = true;
                    q0->touch();
                    engine.markDirty();
                    syncAll();
                }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        auto* q0 = eq();
        if (dragging < 0 || q0 == nullptr)
            return;

        const float x = juce::jlimit(graph.getX(), graph.getRight(), e.position.x);
        const float y = juce::jlimit(graph.getY(), graph.getBottom(), e.position.y);
        const auto& band = q0->bands[dragging];

        q0->setBand(dragging, xToFreq(x), dragging == 0 ? 0.0f : yToDb(y), band.q.load(), true);
        engine.markDirty();
        syncAll();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override { dragging = -1; }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        auto* q0 = eq();
        const int b = nearestBand(e.position);
        if (q0 == nullptr || b < 0)
            return;

        const auto& band = q0->bands[b];
        q0->setBand(b, band.freq.load(), 0.0f, band.q.load(), band.on.load());
        engine.markDirty();
        syncAll();
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        auto* q0 = eq();
        int b = nearestBand(e.position);
        if (b < 0) b = selected;
        if (q0 == nullptr || b < 0)
            return;

        selected = b;
        const auto& band = q0->bands[b];
        q0->setBand(b, band.freq.load(), band.gain.load(), band.q.load() * std::exp(wheel.deltaY * 0.8f),
                    band.on.load());
        engine.markDirty();
        syncAll();
        repaint();
    }

private:
    static juce::Colour bandColour(int b)
    {
        return juce::Colour::fromHSV(0.08f + 0.19f * (float) b, 0.62f, 0.95f, 1.0f);
    }

    ChannelEq* eq() { return engine.getEq(rack); }

    float freqToX(float f) const
    {
        return graph.getX() + graph.getWidth() * std::log(f / 20.0f) / std::log(1000.0f);
    }
    float xToFreq(float x) const
    {
        return 20.0f * std::pow(1000.0f, juce::jlimit(0.0f, 1.0f, (x - graph.getX()) / graph.getWidth()));
    }
    float dbToY(float db) const { return graph.getCentreY() - db / 18.0f * (graph.getHeight() * 0.5f); }
    float yToDb(float y) const { return (graph.getCentreY() - y) / (graph.getHeight() * 0.5f) * 18.0f; }

    juce::Point<float> handlePos(int b)
    {
        auto* q0 = eq();
        const float f = q0->bands[b].freq.load();
        const float gdb = b == 0 ? 0.0f : q0->bands[b].gain.load();
        return { freqToX(f), dbToY(gdb) };
    }

    int nearestBand(juce::Point<float> pos)
    {
        if (eq() == nullptr)
            return -1;

        int best = -1;
        float bestDist = 16.0f;
        for (int b = 0; b < ChannelEq::kBands; ++b)
        {
            const float d = handlePos(b).getDistanceFrom(pos);
            if (d < bestDist) { bestDist = d; best = b; }
        }
        return best;
    }

    void applyFromControls(int b)
    {
        if (auto* q0 = eq())
        {
            q0->setBand(b, (float) freq[b].getValue(), b == 0 ? 0.0f : (float) gain[b].getValue(),
                        (float) q[b].getValue(), on[b].getToggleState());
            engine.markDirty();
            repaint();
        }
    }

    void syncAll()
    {
        if (auto* q0 = eq())
        {
            onBtn.setToggleState(q0->enabled.load(), juce::dontSendNotification);
            for (int b = 0; b < ChannelEq::kBands; ++b)
            {
                on[b].setToggleState(q0->bands[b].on.load(), juce::dontSendNotification);
                freq[b].setValue(q0->bands[b].freq.load(), juce::dontSendNotification);
                gain[b].setValue(q0->bands[b].gain.load(), juce::dontSendNotification);
                q[b].setValue(q0->bands[b].q.load(), juce::dontSendNotification);
            }
        }
        repaint();
    }

    RackEngine& engine;
    int rack = 0;
    int selected = -1, dragging = -1;
    juce::Rectangle<float> graph;
    juce::TextButton onBtn, resetBtn;
    juce::TextButton on[ChannelEq::kBands];
    juce::Label name[ChannelEq::kBands];
    juce::Slider freq[ChannelEq::kBands], gain[ChannelEq::kBands], q[ChannelEq::kBands];
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::DragAndDropTarget,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    // Set by the plugin window so the editor view can ask the DAW for more room.
    std::function<void(int, int)> onRequestSize;

    explicit MainComponent(RackEngine& e)
        : engine(e), hot(getDataDir().getChildFile("hot.xml")), rackModel(*this), racksModel(*this), pluginModel(*this)
    {
        auto dataDir = getDataDir();
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
                if (safe != nullptr)
                {
                    safe->autosaveDirty = true;
                    safe->refresh();
                }
            });
        };

        for (auto* b : { &scanBtn, &audioBtn, &hotBtn, &saveBtn, &loadBtn, &bypassAllBtn, &clearBtn, &inputsBtn, &addRackBtn, &removeRackBtn, &eqBtn })
            addAndMakeVisible(b);
        scanBtn.setButtonText("Scan plugins");
        audioBtn.setButtonText("Audio settings");
        hotBtn.setButtonText("Hot plugins");
        saveBtn.setButtonText("Save rack");
        loadBtn.setButtonText("Load rack");
        bypassAllBtn.setButtonText("Bypass all");
        clearBtn.setButtonText("Clear rack");
        inputsBtn.setButtonText("Inputs");
        eqBtn.setButtonText("EQ");
        eqBtn.setTooltip("Open the built-in channel EQ of the selected rack");
        addRackBtn.setButtonText("Add rack");
        removeRackBtn.setButtonText("Remove rack");
        addRackBtn.setTooltip("Add another stereo rack (up to 64)");
        removeRackBtn.setTooltip("Remove the selected rack");
        inputsBtn.setTooltip("Mix the 64 input buses your DAW can send to OpenRack");

        scanBtn.setTooltip("Find the plugins installed on this computer");
        audioBtn.setTooltip("Choose the audio device, sample rate and buffer size");
        hotBtn.setTooltip("Quick-grab menu: your pinned and most-used plugins");
        saveBtn.setTooltip("Save this rack to a file");
        loadBtn.setTooltip("Load a rack from a file");
        bypassAllBtn.setTooltip("Switch every plugin in the rack off or on");
        clearBtn.setTooltip("Remove every plugin from the rack");

        scanBtn.onClick = [this] { showScanner(); };
        audioBtn.onClick = [this] { showAudioSettings(); };
        hotBtn.onClick = [this] { showHotMenu(); };
        saveBtn.onClick = [this] { saveRack(); };
        loadBtn.onClick = [this] { loadRack(); };
        bypassAllBtn.onClick = [this] { engine.setAllBypassed(! engine.areAllBypassed()); };
        clearBtn.onClick = [this] { confirmClear(); };
        inputsBtn.onClick = [this] { toggleInputs(); };
        eqBtn.onClick = [this] { toggleEq(); };
        engine.onDirty = [this] { autosaveDirty = true; };
        addRackBtn.onClick = [this]
        {
            if (engine.addRack() && activeGroup >= 0)
                engine.setRackGroup(engine.getNumRacks() - 1, (juce::uint32) activeGroup);
        };
        removeRackBtn.onClick = [this] { confirmRemoveRack(); };

        // Racks sidebar and patching
        tabViewport.setViewedComponent(&tabContent, false);
        tabViewport.setScrollBarsShown(false, true);
        tabViewport.setScrollBarThickness(6);
        addAndMakeVisible(tabViewport);

        racksList.setModel(&racksModel);
        racksList.setRowHeight(RackRow::kHeight);
        addAndMakeVisible(racksList);

        for (auto* l : { &inLabel, &outLabel })
        {
            l->setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            l->setColour(juce::Label::textColourId, Theme::muted());
            addAndMakeVisible(l);
        }
        inLabel.setText("IN", juce::dontSendNotification);
        outLabel.setText("OUT", juce::dontSendNotification);
        inCombo.setTooltip("Which input pair feeds this rack");
        outCombo.setTooltip("Which output pair this rack plays to");
        inCombo.onChange = [this]
        {
            engine.setRackInputPair(engine.getSelectedRack(), inCombo.getSelectedId() - 2);
        };
        outCombo.onChange = [this]
        {
            engine.setRackOutputPair(engine.getSelectedRack(), outCombo.getSelectedId() - 2);
        };
        addAndMakeVisible(inCombo);
        addAndMakeVisible(outCombo);

        // Snapshot buttons
        addAndMakeVisible(snapLabel);
        snapLabel.setText("SNAP", juce::dontSendNotification);
        snapLabel.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
        snapLabel.setColour(juce::Label::textColourId, Theme::muted());
        for (int i = 0; i < kNumSnapshots; ++i)
        {
            auto& b = snapBtns[(size_t) i];
            b.setButtonText(juce::String(i + 1));
            b.setColour(juce::TextButton::buttonOnColourId, Theme::accent());
            b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            b.setTooltip("Snapshot " + juce::String(i + 1)
                         + ": click to recall it (or store the rack here if empty). Right-click for more.");
            b.onClick = [this, i] { snapshotClicked(i); };
            b.onRightClick = [this, i] { showSnapshotMenu(i); };
            addAndMakeVisible(b);
        }

        // Plugin search
        searchBox.setTextToShowWhenEmpty("Search plugins by name, maker or type...", Theme::muted());
        searchBox.setFont(juce::Font(juce::FontOptions(15.0f)));
        searchBox.setIndents(34, 0);
        searchBox.setSelectAllWhenFocused(true);
        searchBox.onTextChange = [this] { refresh(); };
        searchBox.onEscapeKey = [this] { searchBox.setText("", true); };
        searchBox.onReturnKey = [this] { addFromSearch(); };
        addAndMakeVisible(searchBox);

        pluginList.setModel(&pluginModel);
        rackList.setModel(&rackModel);
        rackList.setRowHeight(kRackRowHeight);
        pluginList.setRowHeight(32);
        addAndMakeVisible(pluginList);
        addAndMakeVisible(rackList);

        for (auto* l : { &pluginTitle, &rackTitle, &racksTitle })
        {
            l->setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
            l->setColour(juce::Label::textColourId, Theme::muted());
            addAndMakeVisible(l);
        }
        for (auto* l : { &rackStripLabel, &masterLabel })
        {
            l->setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            l->setColour(juce::Label::textColourId, Theme::muted());
            l->setJustificationType(juce::Justification::centred);
            addAndMakeVisible(l);
        }
        masterLabel.setText("MASTER", juce::dontSendNotification);

        auto setupFader = [this](juce::Slider& f, const juce::String& tip)
        {
            f.setSliderStyle(juce::Slider::LinearVertical);
            f.setRange(-60.0, 12.0, 0.1);
            f.setValue(0.0, juce::dontSendNotification);
            f.setDoubleClickReturnValue(true, 0.0);
            f.setTextValueSuffix(" dB");
            f.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
            f.setTooltip(tip);
            addAndMakeVisible(f);
        };
        setupFader(rackFader, "Fader of the selected rack (double-click to reset to 0 dB)");
        setupFader(masterFader, "Master fader for every output (double-click to reset to 0 dB)");

        rackFader.onValueChange = [this]
        {
            engine.setFaderGain(juce::Decibels::decibelsToGain((float) rackFader.getValue(), -60.0f));
        };
        masterFader.onValueChange = [this]
        {
            engine.setMasterGain(juce::Decibels::decibelsToGain((float) masterFader.getValue(), -60.0f));
        };
        syncFaders();

        addAndMakeVisible(inMeter);
        addAndMakeVisible(meter);
        addAndMakeVisible(masterMeter);

        // As a plugin the DAW owns the audio device, so there is nothing to configure.
        audioBtn.setVisible(engine.ownsAudioDevice());
        inputsBtn.setVisible(! engine.ownsAudioDevice());
        addChildComponent(eqPanel);
        addChildComponent(inputsPanel);

        // Inline plugin editors (used when running inside a DAW).
        addChildComponent(editorHost);
        editorHost.onBack = [this] { closeInlineEditor(true); };
        editorHost.onWantedSize = [this](int w, int h)
        {
            if (onRequestSize)
                onRequestSize(juce::jmax(1180, w), juce::jmax(720, h));
        };
        if (! engine.ownsAudioDevice())
            engine.onOpenEditorRequested = [this](int i) { showInlineEditor(i); };
        engine.onBeforeSlotRemoved = [this](juce::uint32 uid)
        {
            if (inlineNode == uid)
                closeInlineEditor(false);
        };

        setSize(1180, 720);
        rebuildFilter();
        rebuildHotChips();
        refresh();
        timerCallback();
        startTimerHz(2);
        checkLastSession();
    }

    ~MainComponent() override
    {
        stopTimer();
        if (engine.ownsAudioDevice())
            lockFile.deleteFile();      // a clean exit: nothing to recover next time
        engine.onDirty = nullptr;
        engine.onOpenEditorRequested = nullptr;
        engine.onBeforeSlotRemoved = nullptr;
        editorHost.clear();
        engine.onChanged = nullptr;
        engine.knownPlugins.removeChangeListener(this);
    }

    void paint(juce::Graphics& g) override
    {
        const float w = (float) getWidth();

        // Background gradient
        g.setGradientFill(juce::ColourGradient(Theme::background().brighter(0.08f), 0.0f, 0.0f,
                                               Theme::background().darker(0.4f), 0.0f, (float) getHeight(),
                                               false));
        g.fillAll();

        // Title
        g.setColour(Theme::accent());
        g.fillRoundedRectangle(16.0f, 17.0f, 6.0f, 26.0f, 3.0f);
        g.setColour(Theme::text());
        g.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        g.drawText("OpenRack", 32, 12, 130, 36, juce::Justification::centredLeft);
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(13.0f));
        g.drawText("live plugin host", 166, 17, 160, 26, juce::Justification::centredLeft);

        // Glowing header line
        g.setGradientFill(juce::ColourGradient(Theme::accent().withAlpha(0.7f), 16.0f, 0.0f,
                                               Theme::accent().withAlpha(0.0f), w - 16.0f, 0.0f, false));
        g.fillRect(juce::Rectangle<float>(16.0f, 56.0f, w - 32.0f, 1.5f));

        // Hot plugins strip
        {
            auto s = hotStrip.toFloat();
            g.setColour(Theme::panel());
            g.fillRoundedRectangle(s, 10.0f);
            g.setColour(Theme::outline());
            g.drawRoundedRectangle(s.reduced(0.5f), 10.0f, 1.0f);

            juce::Path star;
            star.addStar({ s.getX() + 20.0f, s.getCentreY() }, 5, 3.2f, 7.0f);
            g.setColour(Theme::warning());
            g.fillPath(star);
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.drawText("HOT", (int) s.getX() + 30, hotStrip.getY(), 40, hotStrip.getHeight(),
                       juce::Justification::centredLeft);

            if (hotChips.empty())
            {
                g.setColour(Theme::muted());
                g.setFont(juce::FontOptions(12.5f));
                g.drawText("Click the star beside a plugin to pin it here. Your most-used plugins show up automatically.",
                           hotStrip.withTrimmedLeft(78).withTrimmedRight(10),
                           juce::Justification::centredLeft, true);
            }
        }

        // Panels behind the two lists and the master bar
        for (auto* panel : { &pluginPanel, &rackPanel, &racksPanel, &mixerPanel })
        {
            auto p = panel->toFloat();
            g.setGradientFill(juce::ColourGradient(Theme::panel().brighter(0.06f), 0.0f, p.getY(),
                                                   Theme::panel(), 0.0f, p.getBottom(), false));
            g.fillRoundedRectangle(p, 10.0f);
            g.setColour(Theme::outline());
            g.drawRoundedRectangle(p.reduced(0.5f), 10.0f, 1.0f);
        }

        // Divider between the rack strip and the master strip
        g.setColour(Theme::outline());
        g.fillRect(juce::Rectangle<float>((float) stripDividerX, (float) mixerPanel.getY() + 14.0f, 1.0f,
                                          (float) mixerPanel.getHeight() - 28.0f));

        // Empty-state hints
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(14.0f));

        if (filtered.empty())
        {
            const bool noPlugins = totalEffects == 0;
            g.drawFittedText(noPlugins ? "No effect plugins yet.\nClick \"Scan plugins\" to find your VST3 effects,\n"
                                         "or drop a .vst3 file onto this window."
                                       : "No plugins match your search.",
                             pluginPanel.reduced(24), juce::Justification::centred, 4);
        }

        if (engine.getNumSlots() == 0)
            g.drawFittedText("The rack is empty.\nDrag a plugin here, double-click one on the left,\n"
                             "or press Enter in the search box.",
                             rackList.getBounds().reduced(24), juce::Justification::centred, 4);

        // Status bar
        const bool hasDevice = ! engine.ownsAudioDevice()
                                || engine.deviceManager.getCurrentAudioDevice() != nullptr;
        g.setColour(hasDevice ? juce::Colour(0xff3ddc84) : Theme::warning());
        g.fillEllipse((float) statusBar.getX() + 2.0f, (float) statusBar.getCentreY() - 3.0f, 7.0f, 7.0f);
        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(statusText, statusBar.withTrimmedLeft(18), juce::Justification::centredLeft, true);
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        if (editorHost.isVisible() || inputsPanel.isVisible())
            return;

        // Magnifier icon inside the search box
        auto b = searchBox.getBounds().toFloat();
        const float cx = b.getX() + 17.0f;
        const float cy = b.getCentreY() - 1.0f;

        g.setColour(Theme::muted());
        g.drawEllipse(cx - 5.5f, cy - 5.5f, 11.0f, 11.0f, 1.7f);
        g.drawLine(cx + 3.5f, cy + 3.5f, cx + 8.0f, cy + 8.0f, 1.9f);

        // Drop feedback on the rack
        if (dropIndex >= 0 || fileDropActive)
        {
            g.setColour(Theme::accent().withAlpha(0.14f));
            g.fillRoundedRectangle(rackPanel.toFloat(), 10.0f);
            g.setColour(Theme::accent());
            g.drawRoundedRectangle(rackPanel.toFloat().reduced(1.0f), 10.0f, 2.0f);

            if (dropIndex >= 0)
            {
                const int viewY = rackList.getViewport() != nullptr ? rackList.getViewport()->getViewPositionY() : 0;
                float ly = (float) (rackList.getY() + dropIndex * kRackRowHeight - viewY);
                ly = juce::jlimit((float) rackList.getY() + 1.0f, (float) rackList.getBottom() - 2.0f, ly);

                g.setColour(Theme::accent());
                g.fillRoundedRectangle((float) rackList.getX() + 8.0f, ly - 2.0f,
                                       (float) rackList.getWidth() - 16.0f, 4.0f, 2.0f);
                g.fillEllipse((float) rackList.getX() + 2.0f, ly - 5.0f, 10.0f, 10.0f);
                g.fillEllipse((float) rackList.getRight() - 12.0f, ly - 5.0f, 10.0f, 10.0f);
            }
        }
    }

    void resized() override
    {
        editorHost.setBounds(getLocalBounds());

        auto r = getLocalBounds().reduced(16);

        auto top = r.removeFromTop(40);
        for (auto* b : { &loadBtn, &saveBtn, &hotBtn, &inputsBtn, &audioBtn, &scanBtn })
            if (b->isVisible())
                b->setBounds(top.removeFromRight(116).reduced(3, 3));

        r.removeFromTop(16);

        hotStrip = r.removeFromTop(36);
        r.removeFromTop(12);

        statusBar = r.removeFromBottom(20);
        r.removeFromBottom(8);

        inputsPanel.setBounds(hotStrip.getX(), hotStrip.getY(), hotStrip.getWidth(),
                              r.getBottom() - hotStrip.getY());

        auto racksCol = r.removeFromRight(juce::jlimit(200, 260, r.getWidth() / 4));
        r.removeFromRight(16);

        auto left = r.removeFromLeft(r.getWidth() * 2 / 5);
        r.removeFromLeft(16);

        pluginTitle.setBounds(left.removeFromTop(24));
        searchBox.setBounds(left.removeFromTop(32));
        left.removeFromTop(8);
        pluginPanel = left;
        pluginList.setBounds(left.reduced(6));

        rackTitle.setBounds(r.removeFromTop(24));
        auto actions = r.removeFromTop(32);
        bypassAllBtn.setBounds(actions.removeFromLeft(100).reduced(0, 2));
        actions.removeFromLeft(6);
        clearBtn.setBounds(actions.removeFromLeft(90).reduced(0, 2));
        actions.removeFromLeft(6);
        eqBtn.setBounds(actions.removeFromLeft(44).reduced(0, 2));
        actions.removeFromLeft(10);
        snapLabel.setBounds(actions.removeFromLeft(40));
        const int snapW = juce::jlimit(16, 32, actions.getWidth() / kNumSnapshots - 3);
        for (auto& b : snapBtns)
        {
            b.setBounds(actions.removeFromLeft(snapW).reduced(0, 2));
            actions.removeFromLeft(3);
        }
        r.removeFromTop(8);
        rackPanel = r;
        eqPanel.setBounds(r);

        auto chainArea = r.reduced(10, 8);
        auto patchRow = chainArea.removeFromTop(28);
        inLabel.setBounds(patchRow.removeFromLeft(24));
        inCombo.setBounds(patchRow.removeFromLeft((patchRow.getWidth() - 40) / 2));
        patchRow.removeFromLeft(10);
        outLabel.setBounds(patchRow.removeFromLeft(30));
        outCombo.setBounds(patchRow);
        chainArea.removeFromTop(6);
        rackList.setBounds(chainArea);

        racksTitle.setBounds(racksCol.removeFromTop(24));
        auto rackBtns = racksCol.removeFromTop(32);
        addRackBtn.setBounds(rackBtns.removeFromLeft(rackBtns.getWidth() / 2).reduced(0, 2).withTrimmedRight(3));
        removeRackBtn.setBounds(rackBtns.reduced(0, 2).withTrimmedLeft(3));
        racksCol.removeFromTop(8);

        // Mixer strips (vertical faders and meters) under the racks list
        mixerPanel = racksCol.removeFromBottom(juce::jmin(260, racksCol.getHeight() / 2));
        racksCol.removeFromBottom(12);
        racksPanel = racksCol;
        auto racksInner = racksCol.reduced(6);
        tabViewport.setBounds(racksInner.removeFromTop(36));
        racksInner.removeFromTop(2);
        racksList.setBounds(racksInner);

        auto strips = mixerPanel.reduced(10, 8);
        auto rackStrip = strips.removeFromLeft(strips.getWidth() * 58 / 100);
        strips.removeFromLeft(10);
        auto masterStrip = strips;
        stripDividerX = masterStrip.getX() - 5;

        rackStripLabel.setBounds(rackStrip.removeFromTop(18));
        masterLabel.setBounds(masterStrip.removeFromTop(18));
        rackStrip.removeFromTop(4);
        masterStrip.removeFromTop(4);

        const int meterW = 26;
        inMeter.setBounds(rackStrip.removeFromLeft(meterW));
        meter.setBounds(rackStrip.removeFromRight(meterW));
        rackFader.setBounds(rackStrip.reduced(4, 0));

        masterMeter.setBounds(masterStrip.removeFromRight(meterW));
        masterFader.setBounds(masterStrip.reduced(4, 0));

        layoutHotChips();
    }

    //--------------------------------------------------------------------------
    // Drag and drop: plugins (from the list or hot strip) and rack rows
    bool isInterestedInDragSource(const SourceDetails& d) override
    {
        if (editorHost.isVisible() || inputsPanel.isVisible() || eqPanel.isVisible())
            return false;
        const auto s = d.description.toString();
        return s.startsWith("add:") || s.startsWith("move:");
    }

    void itemDragEnter(const SourceDetails& d) override { updateDrop(d); }
    void itemDragMove(const SourceDetails& d) override { updateDrop(d); }
    void itemDragExit(const SourceDetails&) override { clearDrop(); }

    void itemDropped(const SourceDetails& d) override
    {
        const int idx = dropIndexAt(d.localPosition);
        clearDrop();
        if (idx < 0)
            return;

        const auto s = d.description.toString();
        if (s.startsWith("add:"))
        {
            if (auto desc = engine.knownPlugins.getTypeForIdentifierString(s.fromFirstOccurrenceOf("add:", false, false)))
                addPluginDesc(*desc, idx);
        }
        else if (s.startsWith("move:"))
        {
            engine.moveSlotTo(s.fromFirstOccurrenceOf("move:", false, false).getIntValue(), idx);
        }
    }

    // Dropping plugin files (.vst3 / .component) onto the window scans and adds them.
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        if (editorHost.isVisible() || inputsPanel.isVisible() || eqPanel.isVisible())
            return false;
        for (auto& f : files)
            if (isPluginFile(f)) return true;
        return false;
    }

    void fileDragEnter(const juce::StringArray&, int, int) override { fileDropActive = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { fileDropActive = false; repaint(); }

    void filesDropped(const juce::StringArray& files, int, int) override
    {
        fileDropActive = false;
        repaint();
        for (auto& f : files)
            if (isPluginFile(f))
                addPluginFile(f);
    }

private:
    //--------------------------------------------------------------------------
    static juce::File getDataDir()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("OpenRack");
    }

    static bool isPluginFile(const juce::String& f)
    {
        return f.endsWithIgnoreCase(".vst3") || f.endsWithIgnoreCase(".component");
    }

    struct RackModel : juce::ListBoxModel
    {
        explicit RackModel(MainComponent& o) : owner(o) {}
        int getNumRows() override { return owner.engine.getNumSlots(); }
        void paintListBoxItem(int, juce::Graphics& g, int w, int h, bool selected) override
        {
            auto r = juce::Rectangle<float>(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f);
            g.setColour(selected ? Theme::row().brighter(0.08f) : Theme::row());
            g.fillRoundedRectangle(r, 8.0f);
            if (selected)
            {
                g.setColour(Theme::accent().withAlpha(0.8f));
                g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.5f);
            }
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
        juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override
        {
            if (rows.size() > 0)
                return "move:" + juce::String(rows[0]);
            return {};
        }
        MainComponent& owner;
    };

    // The racks sidebar shows the racks of the active tab; `row` is a position in that list.
    struct RacksModel : juce::ListBoxModel
    {
        explicit RacksModel(MainComponent& o) : owner(o) {}
        int getNumRows() override { return (int) owner.visibleRacks.size(); }
        void paintListBoxItem(int, juce::Graphics& g, int w, int h, bool selected) override
        {
            auto r = juce::Rectangle<float>(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f);
            g.setColour(selected ? Theme::row().brighter(0.1f) : Theme::row());
            g.fillRoundedRectangle(r, 8.0f);
            if (selected)
            {
                g.setColour(Theme::accent());
                g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.5f);
            }
        }
        juce::Component* refreshComponentForRow(int row, bool, juce::Component* existing) override
        {
            if (row >= getNumRows())
            {
                delete existing;
                return nullptr;
            }
            auto* r = dynamic_cast<RackRow*>(existing);
            if (r == nullptr)
            {
                delete existing;
                r = new RackRow(owner.engine);
            }
            r->setRow(owner.visibleRacks[(size_t) row]);
            return r;
        }
        void selectedRowsChanged(int lastRow) override
        {
            if (lastRow >= 0 && lastRow < getNumRows())
                owner.engine.selectRack(owner.visibleRacks[(size_t) lastRow]);
        }
        void listBoxItemClicked(int row, const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && row >= 0 && row < getNumRows())
                owner.showRackMenu(owner.visibleRacks[(size_t) row]);
        }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
        {
            if (row >= 0 && row < getNumRows())
                owner.renameRack(owner.visibleRacks[(size_t) row]);
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

            // Pin star (click to pin as a hot plugin)
            const bool pinned = owner.hot.isPinned(d.createIdentifierString());
            juce::Path star;
            star.addStar({ (float) area.getX() + 8.0f, (float) h * 0.5f }, 5, 3.3f, 7.2f);
            if (pinned)
            {
                g.setColour(Theme::warning());
                g.fillPath(star);
            }
            else
            {
                g.setColour(Theme::muted().withAlpha(0.7f));
                g.strokePath(star, juce::PathStrokeType(1.1f));
            }
            area.removeFromLeft(24);

            // Colour dot matching the plugin's colour in the rack
            g.setColour(Theme::tint(d.name));
            g.fillEllipse((float) area.getX(), (float) h * 0.5f - 3.5f, 7.0f, 7.0f);
            area.removeFromLeft(16);

            g.setColour(Theme::text());
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(d.name, area.withTrimmedRight(area.getWidth() * 2 / 5),
                       juce::Justification::centredLeft, true);

            g.setColour(Theme::muted());
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(d.manufacturerName, area.removeFromRight(area.getWidth() * 2 / 5),
                       juce::Justification::centredRight, true);
        }
        void listBoxItemClicked(int row, const juce::MouseEvent& e) override
        {
            if (row < 0 || row >= (int) owner.filtered.size())
                return;

            const auto id = owner.filtered[(size_t) row].createIdentifierString();
            if (e.mods.isPopupMenu())
            {
                owner.showHotItemMenu(id);
            }
            else if (e.x < 30)
            {
                owner.hot.togglePin(id);
                owner.afterHotChanged();
            }
        }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent& e) override
        {
            if (e.x >= 30)
                owner.addFilteredPlugin(row);
        }
        juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override
        {
            if (rows.size() > 0 && rows[0] >= 0 && rows[0] < (int) owner.filtered.size())
                return "add:" + owner.filtered[(size_t) rows[0]].createIdentifierString();
            return {};
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

        totalEffects = 0;

        for (const auto& t : types)
        {
            // Effects only: virtual instruments (synths, samplers) are left out.
            if (t.isInstrument)
                continue;

            ++totalEffects;

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

        const int total = totalEffects;
        const int shown = (int) filtered.size();
        pluginTitle.setText(words.isEmpty() ? "PLUGINS  -  " + juce::String(total)
                                            : "PLUGINS  -  " + juce::String(shown) + " of " + juce::String(total),
                            juce::dontSendNotification);
    }

    //--------------------------------------------------------------------------
    // Adds a plugin, optionally at a specific position, and counts it as "used".
    void addPluginDesc(const juce::PluginDescription& d, int insertAt = -1)
    {
        // Standalone app: a plugin seen for the first time is run in a separate
        // process first, so one that crashes cannot take OpenRack down with it.
        if (engine.ownsAudioDevice() && ! engine.safety.isBlocked(d.createIdentifierString())
            && ! engine.safety.isVerified(d))
        {
            juce::Component::SafePointer<MainComponent> safe(this);
            ++checking;
            checkingName = d.name;
            timerCallback();

            PluginSafety::runPreflight(d, [safe, d, insertAt](bool ok, const juce::String& message)
            {
                if (safe == nullptr)
                    return;

                --safe->checking;
                safe->timerCallback();

                if (! ok)
                {
                    safe->engine.safety.block(d.createIdentifierString());
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Plugin blocked",
                        message + "\n\nRight-click it in the list and choose \"Unblock plugin\" to try again.");
                    return;
                }

                safe->engine.safety.setVerified(d);
                safe->addPluginNow(d, insertAt);
            });
            return;
        }

        addPluginNow(d, insertAt);
    }

    void addPluginNow(const juce::PluginDescription& d, int insertAt)
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        const auto id = d.createIdentifierString();

        auto done = [safe, id](const juce::String& err)
        {
            if (err.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Couldn't load plugin", err);
                return;
            }
            if (safe != nullptr)
            {
                safe->hot.recordUse(id);
                juce::MessageManager::callAsync([safe]
                {
                    if (safe != nullptr) safe->rebuildHotChips();
                });
            }
        };

        if (insertAt >= 0 && insertAt < engine.getNumSlots())
            engine.addPluginAt(d, insertAt, done);
        else
            engine.addPlugin(d, done);
    }

    void addFilteredPlugin(int row)
    {
        if (row >= 0 && row < (int) filtered.size())
            addPluginDesc(filtered[(size_t) row]);
    }

    // Enter in the search box adds the selected result, or the first one.
    void addFromSearch()
    {
        int row = pluginList.getSelectedRow();
        addFilteredPlugin(row >= 0 ? row : 0);
    }

    // A .vst3 / .component dropped onto the window: scan it, then add any effects in it.
    void addPluginFile(const juce::String& path)
    {
        std::vector<juce::PluginDescription> candidates;

        for (int i = 0; i < engine.formatManager.getNumFormats(); ++i)
        {
            auto* format = engine.formatManager.getFormat(i);
            if (format == nullptr || ! format->fileMightContainThisPluginType(path))
                continue;

            juce::OwnedArray<juce::PluginDescription> found;
            engine.knownPlugins.scanAndAddFile(path, true, found, *format);

            for (auto* d : found)
                candidates.push_back(*d);
        }

        if (candidates.empty())
        {
            // Already in the list: look it up there.
            const juce::File file(path);
            for (const auto& t : engine.knownPlugins.getTypes())
                if (juce::File(t.fileOrIdentifier) == file || t.name == file.getFileNameWithoutExtension())
                    candidates.push_back(t);
        }

        int effects = 0, instruments = 0;
        for (auto& d : candidates)
        {
            if (d.isInstrument) { ++instruments; continue; }
            ++effects;
            addPluginDesc(d);
        }

        if (effects == 0)
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon, "Drop a plugin",
                instruments > 0 ? juce::File(path).getFileName() + " is an instrument. OpenRack only hosts effects."
                                : "Couldn't find a plugin in " + juce::File(path).getFileName() + ".");
    }

    //--------------------------------------------------------------------------
    // Drop feedback
    int dropIndexAt(juce::Point<int> p) const
    {
        if (! rackPanel.contains(p))
            return -1;

        const auto lp = rackList.getLocalPoint(this, p);
        const int idx = rackList.getInsertionIndexForPosition(lp.x, lp.y);
        return idx < 0 ? engine.getNumSlots() : juce::jmin(idx, engine.getNumSlots());
    }

    void updateDrop(const SourceDetails& d)
    {
        const int idx = dropIndexAt(d.localPosition);
        if (idx != dropIndex)
        {
            dropIndex = idx;
            repaint();
        }
    }

    void clearDrop()
    {
        if (dropIndex != -1)
        {
            dropIndex = -1;
            repaint();
        }
    }

    //--------------------------------------------------------------------------
    // Hot plugins: pinned + most used, as chips under the toolbar and in a menu.
    void rebuildHotChips()
    {
        struct Item { juce::String id, name; bool pinned; };
        std::vector<Item> items;

        auto push = [&](const juce::String& id, bool pinned)
        {
            if (auto d = engine.knownPlugins.getTypeForIdentifierString(id))
                if (! d->isInstrument)
                    items.push_back({ id, d->name, pinned });
        };

        for (auto& id : hot.getPinned())      push(id, true);
        for (auto& id : hot.getMostUsed(8))   push(id, false);

        juce::String sig;
        for (auto& it : items)
            sig += it.id + (it.pinned ? "*" : "") + "|";

        if (sig == hotSignature)
            return;
        hotSignature = sig;

        hotChips.clear();
        for (auto& it : items)
        {
            auto chip = std::make_unique<HotChip>(it.id, it.name, it.pinned);
            chip->setSize(HotChip::widthFor(it.name), 26);

            const auto id = it.id;
            chip->onClick = [this, id]
            {
                if (auto d = engine.knownPlugins.getTypeForIdentifierString(id))
                    addPluginDesc(*d);
            };
            chip->onMenu = [this, id] { showHotItemMenu(id); };

            addAndMakeVisible(*chip);
            hotChips.push_back(std::move(chip));
        }

        layoutHotChips();
        repaint(hotStrip);
    }

    void layoutHotChips()
    {
        int x = hotStrip.getX() + 78;
        const int right = hotStrip.getRight() - 10;
        const int y = hotStrip.getCentreY() - 13;

        for (auto& c : hotChips)
        {
            const int w = c->getWidth();
            c->setTopLeftPosition(x, y);
            c->setVisible(x + w <= right);
            x += w + 8;
        }
    }

    void afterHotChanged()
    {
        rebuildHotChips();
        pluginList.repaint();
    }

    void showHotItemMenu(const juce::String& id)
    {
        const bool pinned = hot.isPinned(id);

        juce::PopupMenu m;
        m.addItem(1, "Add to rack");
        m.addItem(2, pinned ? "Unpin from hot plugins" : "Pin to hot plugins");
        m.addItem(3, "Remove from hot plugins", pinned || hot.getUseCount(id) > 0);
        if (engine.safety.isBlocked(id))
        {
            m.addSeparator();
            m.addItem(4, "Unblock plugin");
        }

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe, id](int r)
        {
            if (safe == nullptr)
                return;

            if (r == 1)
            {
                if (auto d = safe->engine.knownPlugins.getTypeForIdentifierString(id))
                    safe->addPluginDesc(*d);
            }
            else if (r == 2) { safe->hot.togglePin(id); safe->afterHotChanged(); }
            else if (r == 3) { safe->hot.forget(id);    safe->afterHotChanged(); }
            else if (r == 4) { safe->engine.safety.unblock(id); }
        });
    }

    void showHotMenu()
    {
        juce::PopupMenu m;
        auto ids = std::make_shared<std::vector<juce::String>>();

        auto addEntry = [&](const juce::String& id, const juce::String& suffix)
        {
            if (auto d = engine.knownPlugins.getTypeForIdentifierString(id))
            {
                if (d->isInstrument)
                    return;
                ids->push_back(id);
                m.addItem((int) ids->size(), d->name + suffix);
            }
        };

        m.addSectionHeader("Pinned");
        for (auto& id : hot.getPinned())
            addEntry(id, {});
        if (ids->empty())
            m.addItem(5000, "Click the star beside a plugin to pin it", false);

        const auto used = hot.getMostUsed(8);
        if (! used.isEmpty())
        {
            m.addSeparator();
            m.addSectionHeader("Most used");
            for (auto& id : used)
                addEntry(id, "     " + juce::String(hot.getUseCount(id)) + "x");
        }

        m.addSeparator();
        m.addItem(10000, "Clear usage history", hot.hasAnyUsage());

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&hotBtn), [safe, ids](int r)
        {
            if (safe == nullptr || r <= 0)
                return;

            if (r == 10000)
            {
                safe->hot.clearUsage();
                safe->afterHotChanged();
            }
            else if (r <= (int) ids->size())
            {
                if (auto d = safe->engine.knownPlugins.getTypeForIdentifierString((*ids)[(size_t) r - 1]))
                    safe->addPluginDesc(*d);
            }
        });
    }

    void toggleEq()
    {
        const bool show = ! eqPanel.isVisible();
        if (show)
        {
            if (editorHost.isVisible())
                closeInlineEditor(true);
            eqPanel.attach(engine.getSelectedRack());
        }
        eqPanel.setVisible(show);
        eqBtn.setToggleState(show, juce::dontSendNotification);
        eqBtn.setColour(juce::TextButton::buttonOnColourId, Theme::accent());
        repaint();
    }

    void toggleInputs()
    {
        const bool show = ! inputsPanel.isVisible();
        if (show)
        {
            if (editorHost.isVisible())
                closeInlineEditor(true);
            inputsPanel.sync();
        }
        inputsPanel.setVisible(show);
        if (show)
            inputsPanel.toFront(false);
        inputsBtn.setButtonText(show ? "Rack" : "Inputs");
        repaint();
    }

    void snapshotClicked(int i)
    {
        if (engine.hasSnapshot(i))
            recallSnapshot(i);
        else
            engine.storeSnapshot(i);
    }

    void recallSnapshot(int i)
    {
        engine.recallSnapshot(i, [](const juce::String& err)
        {
            if (err.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Snapshot", err);
        });
    }

    void showSnapshotMenu(int i)
    {
        juce::PopupMenu m;
        m.addItem(1, "Store current rack in snapshot " + juce::String(i + 1));
        m.addItem(2, "Recall snapshot " + juce::String(i + 1), engine.hasSnapshot(i));
        m.addItem(3, "Clear snapshot " + juce::String(i + 1), engine.hasSnapshot(i));

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe, i](int r)
        {
            if (safe == nullptr) return;
            if (r == 1)      safe->engine.storeSnapshot(i);
            else if (r == 2) safe->recallSnapshot(i);
            else if (r == 3) safe->engine.clearSnapshot(i);
        });
    }

    // Standalone app: offer to restore the last rack if OpenRack did not exit cleanly,
    // and say so if a plugin was blocked because it crashed while loading.
    void checkLastSession()
    {
        juce::String notice;
        const auto suspect = engine.takeSuspectAtStartup();
        if (suspect.isNotEmpty())
        {
            juce::String name = suspect.fromFirstOccurrenceOf("-", false, false)
                                       .upToLastOccurrenceOf("-", false, false)
                                       .upToLastOccurrenceOf("-", false, false);
            if (auto d = engine.knownPlugins.getTypeForIdentifierString(suspect))
                name = d->name;
            notice = name + " was loading when the last session ended unexpectedly, so it has been blocked. "
                            "You can unblock it by right-clicking it in the plugin list.";
        }

        bool canRestore = false;
        if (engine.ownsAudioDevice())
        {
            canRestore = lockFile.existsAsFile() && autosaveFile.existsAsFile();
            lockFile.replaceWithText("running");
            autosaveArmed = ! canRestore;
        }

        if (notice.isEmpty() && ! canRestore)
            return;

        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe, notice, canRestore]
        {
            if (safe == nullptr)
                return;

            if (! canRestore)
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Plugin blocked", notice);
                return;
            }

            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions::makeOptionsOkCancel(
                    juce::MessageBoxIconType::WarningIcon, "OpenRack closed unexpectedly",
                    (notice.isNotEmpty() ? notice + "\n\n" : juce::String()) + "Restore your last rack?",
                    "Restore", "Start empty", safe.getComponent()),
                [safe](int result)
                {
                    if (safe == nullptr)
                        return;

                    if (result == 1)
                        safe->engine.loadRack(safe->autosaveFile, [safe](const juce::String&)
                        {
                            if (safe != nullptr) safe->autosaveArmed = true;
                        });
                    else
                        safe->autosaveArmed = true;
                });
        });
    }

    void syncFaders()
    {
        rackFader.setValue(juce::Decibels::gainToDecibels(engine.getFaderGain(), -60.0f), juce::dontSendNotification);
        masterFader.setValue(juce::Decibels::gainToDecibels(engine.getMasterGain(), -60.0f), juce::dontSendNotification);
    }

    // Fill the In / Out selectors for the selected rack (only when something changed,
    // so an open menu is not rebuilt under the user).
    void updatePatchCombos()
    {
        const int sel = engine.getSelectedRack();
        const int curIn = engine.getRackInputPair(sel);
        const int curOut = engine.getRackOutputPair(sel);
        const int nIn = engine.getNumInputPairs();
        const int nOut = engine.getNumOutputPairs();

        const auto sig = juce::String(sel) + "/" + juce::String(curIn) + "/" + juce::String(curOut)
                         + "/" + juce::String(nIn) + "/" + juce::String(nOut);
        if (sig == patchSignature)
            return;
        patchSignature = sig;

        auto fill = [](juce::ComboBox& box, int count, int current)
        {
            box.clear(juce::dontSendNotification);
            box.addItem("None", 1);
            for (int p = 0; p < juce::jmax(count, current + 1); ++p)
                box.addItem(RackRow::pairName(p), 2 + p);
            box.setSelectedId(current < 0 ? 1 : 2 + current, juce::dontSendNotification);
        };

        fill(inCombo, nIn, curIn);
        fill(outCombo, nOut, curOut);
    }

    // A small "type a name" dialog.
    void promptText(const juce::String& title, const juce::String& message, const juce::String& initial,
                    std::function<void(const juce::String&)> onOk)
    {
        auto* w = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor("name", initial);
        w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        juce::Component::SafePointer<juce::AlertWindow> window(w);
        juce::Component::SafePointer<MainComponent> safe(this);
        w->enterModalState(true,
                           juce::ModalCallbackFunction::create([safe, window, onOk](int result)
                           {
                               if (result == 1 && safe != nullptr && window != nullptr)
                                   onOk(window->getTextEditorContents("name"));
                           }),
                           true);
    }

    void renameRack(int rack)
    {
        if (rack < 0 || rack >= engine.getNumRacks())
            return;

        juce::Component::SafePointer<MainComponent> safe(this);
        promptText("Rename rack", "Name for this rack:", engine.getRackName(rack),
                   [safe, rack](const juce::String& name)
                   {
                       if (safe != nullptr) safe->engine.renameRack(rack, name);
                   });
    }

    //--------------------------------------------------------------------------
    // Groups (tabs above the racks list)
    void newGroup(int moveRack)
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        promptText("New group", "Name for the new group (it becomes a tab):", {},
                   [safe, moveRack](const juce::String& name)
                   {
                       if (safe == nullptr) return;
                       const auto id = safe->engine.addGroup(name);
                       if (moveRack >= 0)
                           safe->engine.setRackGroup(moveRack, id);
                       safe->activeGroup = (int) id;
                       safe->refresh();
                   });
    }

    void showGroupMenu(juce::uint32 id)
    {
        juce::PopupMenu m;
        m.addItem(1, "Rename group...");
        m.addItem(2, "Delete group (racks are kept)");

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe, id](int r)
        {
            if (safe == nullptr) return;
            if (r == 1)
            {
                juce::String current;
                for (int i = 0; i < safe->engine.getNumGroups(); ++i)
                    if (safe->engine.getGroupId(i) == id) current = safe->engine.getGroupName(i);
                safe->promptText("Rename group", "Name for this group:", current,
                                 [safe, id](const juce::String& name)
                                 {
                                     if (safe != nullptr) safe->engine.renameGroup(id, name);
                                 });
            }
            else if (r == 2)
            {
                safe->engine.removeGroup(id);
                if (safe->activeGroup == (int) id) safe->activeGroup = -1;
                safe->refresh();
            }
        });
    }

    void showRackMenu(int rack)
    {
        juce::PopupMenu m;
        m.addItem(1, "Rename rack...");

        juce::PopupMenu groupsMenu;
        const auto current = engine.getRackGroup(rack);
        groupsMenu.addItem(2, "No group", true, current == 0);
        for (int i = 0; i < engine.getNumGroups(); ++i)
            groupsMenu.addItem(100 + i, engine.getGroupName(i), true, engine.getGroupId(i) == current);
        groupsMenu.addSeparator();
        groupsMenu.addItem(3, "New group...");
        m.addSubMenu("Move to group", groupsMenu);

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe, rack](int r)
        {
            if (safe == nullptr || r <= 0) return;
            if (r == 1)      safe->renameRack(rack);
            else if (r == 2) { safe->engine.setRackGroup(rack, 0); }
            else if (r == 3) safe->newGroup(rack);
            else if (r >= 100 && r - 100 < safe->engine.getNumGroups())
                safe->engine.setRackGroup(rack, safe->engine.getGroupId(r - 100));
        });
    }

    // One tab per group plus "All" and "+" (rebuilt only when the list of groups changes).
    void rebuildTabs()
    {
        juce::String sig;
        for (int i = 0; i < engine.getNumGroups(); ++i)
            sig += juce::String(engine.getGroupId(i)) + ":" + engine.getGroupName(i) + "|";

        if (sig != tabSignature)
        {
            tabSignature = sig;
            tabButtons.clear();

            auto add = [this](const juce::String& text, int groupId, bool isNew)
            {
                auto* b = tabButtons.add(new SnapButton());
                b->setButtonText(text);
                b->setColour(juce::TextButton::buttonOnColourId, Theme::accent());
                b->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
                b->setSize(measureText(text, 13.0f) + 28, 26);

                if (isNew)
                {
                    b->setTooltip("Add a group (a new tab)");
                    b->onClick = [this] { newGroup(-1); };
                }
                else
                {
                    b->setTooltip(groupId < 0 ? "Show every rack"
                                              : "Racks in " + text + ". Right-click to rename or delete.");
                    b->onClick = [this, groupId] { activeGroup = groupId; refresh(); };
                    if (groupId >= 0)
                        b->onRightClick = [this, groupId] { showGroupMenu((juce::uint32) groupId); };
                }
                tabContent.addAndMakeVisible(b);
                tabIds.push_back(isNew ? -2 : groupId);
            };

            tabIds.clear();
            add("All", -1, false);
            for (int i = 0; i < engine.getNumGroups(); ++i)
                add(engine.getGroupName(i), (int) engine.getGroupId(i), false);
            add("+", -1, true);

            int x = 0;
            for (auto* b : tabButtons)
            {
                b->setTopLeftPosition(x, 0);
                x += b->getWidth() + 4;
            }
            tabContent.setSize(juce::jmax(x, 1), 28);
        }

        for (int i = 0; i < tabButtons.size(); ++i)
            tabButtons[i]->setToggleState(tabIds[(size_t) i] == activeGroup, juce::dontSendNotification);
    }

    void rebuildVisibleRacks()
    {
        // A deleted group falls back to showing everything.
        bool found = activeGroup < 0;
        for (int i = 0; i < engine.getNumGroups(); ++i)
            found = found || (int) engine.getGroupId(i) == activeGroup;
        if (! found)
            activeGroup = -1;

        visibleRacks.clear();
        for (int i = 0; i < engine.getNumRacks(); ++i)
            if (activeGroup < 0 || (int) engine.getRackGroup(i) == activeGroup)
                visibleRacks.push_back(i);
    }

    void confirmRemoveRack()
    {
        const int sel = engine.getSelectedRack();
        if (engine.getNumRacks() <= 1)
            return;

        if (engine.getRackNumSlots(sel) == 0)
        {
            engine.removeRack(sel);
            return;
        }

        juce::Component::SafePointer<MainComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOkCancel(
                juce::MessageBoxIconType::QuestionIcon, "Remove rack",
                "Remove \"" + engine.getRackName(sel) + "\" and its " + juce::String(engine.getRackNumSlots(sel))
                    + " plugins?",
                "Remove", "Cancel", this),
            [safe, sel](int result)
            {
                if (result == 1 && safe != nullptr)
                    safe->engine.removeRack(sel);
            });
    }

    void confirmClear()
    {
        if (engine.getNumSlots() == 0)
            return;

        juce::Component::SafePointer<MainComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOkCancel(
                juce::MessageBoxIconType::QuestionIcon, "Clear rack",
                "Remove all " + juce::String(engine.getNumSlots()) + " plugins from the rack?",
                "Clear", "Cancel", this),
            [safe](int result)
            {
                if (result == 1 && safe != nullptr)
                    safe->engine.clearRack();
            });
    }

    //--------------------------------------------------------------------------
    void refresh()
    {
        // Keep the faders in step with a rack loaded by the host or from a file.
        syncFaders();
        rebuildFilter();
        rackList.updateContent();
        rackList.repaint();
        pluginList.updateContent();
        pluginList.repaint();

        const int n = engine.getNumSlots();
        const int lat = engine.getTotalLatency();
        const int sel = engine.getSelectedRack();
        auto title = "RACK " + juce::String(sel + 1) + "  -  " + engine.getRackName(sel).toUpperCase()
                     + "  -  " + juce::String(n) + (n == 1 ? " plugin" : " plugins");
        if (lat > 0)
            title += "  -  latency " + juce::String(lat) + " smp";
        rackTitle.setText(title, juce::dontSendNotification);

        rackStripLabel.setText(engine.getRackName(sel), juce::dontSendNotification);
        rackStripLabel.setTooltip("Rack " + juce::String(sel + 1) + ": " + engine.getRackName(sel));

        rebuildVisibleRacks();
        rebuildTabs();
        racksTitle.setText(activeGroup < 0 ? "RACKS  -  " + juce::String(engine.getNumRacks())
                                           : "RACKS  -  " + juce::String((int) visibleRacks.size()) + " of "
                                                 + juce::String(engine.getNumRacks()),
                           juce::dontSendNotification);
        racksList.updateContent();
        const auto it = std::find(visibleRacks.begin(), visibleRacks.end(), sel);
        if (it != visibleRacks.end())
            racksList.selectRow((int) (it - visibleRacks.begin()), true, true);
        else
            racksList.deselectAllRows();
        racksList.repaint();
        removeRackBtn.setEnabled(engine.getNumRacks() > 1);
        addRackBtn.setEnabled(engine.getNumRacks() < kMaxRacks);
        updatePatchCombos();
        if (eqPanel.isVisible())
            eqPanel.attach(sel);

        for (int i = 0; i < kNumSnapshots; ++i)
        {
            auto& b = snapBtns[(size_t) i];
            b.setToggleState(engine.getCurrentSnapshot() == i, juce::dontSendNotification);
            b.setColour(juce::TextButton::textColourOffId, engine.hasSnapshot(i) ? Theme::text() : Theme::muted());
        }
        if (inputsPanel.isVisible())
            inputsPanel.sync();

        bypassAllBtn.setButtonText(engine.areAllBypassed() ? "Enable all" : "Bypass all");
        bypassAllBtn.setEnabled(n > 0);
        clearBtn.setEnabled(n > 0);

        rebuildHotChips();
        repaint();
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (auto xml = engine.knownPlugins.createXml())
            xml->writeTo(pluginListFile);
        refresh();
    }

    // Twice a second: refresh the audio device / CPU line in the status bar.
    void timerCallback() override
    {
        juce::String s;
        if (! engine.ownsAudioDevice())
        {
            s = "Running inside your DAW"
                "      " + juce::String((int) engine.getSampleRate()) + " Hz"
                "      " + juce::String(engine.getBlockSize()) + " samples"
                "      rack latency " + juce::String(engine.getTotalLatency()) + " samples";
        }
        else if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
        {
            const double sr = dev->getCurrentSampleRate();
            const int bs = dev->getCurrentBufferSizeSamples();
            const double ms = sr > 0.0 ? 1000.0 * (double) bs / sr : 0.0;
            const int cpu = juce::roundToInt(engine.deviceManager.getCpuUsage() * 100.0);

            s = dev->getTypeName() + "  -  " + dev->getName()
                + "      " + juce::String((int) sr) + " Hz"
                + "      " + juce::String(bs) + " samples (" + juce::String(ms, 1) + " ms)"
                + "      CPU " + juce::String(cpu) + "%";
        }
        else
        {
            s = "No audio device - open Audio settings";
        }

        if (checking > 0)
            s = "Checking " + checkingName + " in a separate process before loading it...";

        if (autosaveArmed && autosaveDirty && engine.ownsAudioDevice())
        {
            autosaveDirty = false;
            engine.saveRack(autosaveFile);
        }

        if (s != statusText)
        {
            statusText = s;
            repaint(statusBar);
        }
    }

    void showScanner()
    {
        auto* c = new Themed<juce::PluginListComponent>(engine.formatManager, engine.knownPlugins,
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

    void showInlineEditor(int row)
    {
        if (row < 0 || row >= engine.getNumSlots())
            return;

        if (! editorHost.isVisible())
        {
            preEditorW = getWidth();
            preEditorH = getHeight();
        }

        auto& slot = engine.getSlot(row);
        inlineNode = slot.node->nodeID.uid;
        editorHost.setBounds(getLocalBounds());
        editorHost.show(*slot.node->getProcessor(), slot.desc.name);
        editorHost.setVisible(true);
        editorHost.toFront(false);
    }

    // restoreSize: put the window back to its size from before the editor opened.
    void closeInlineEditor(bool restoreSize)
    {
        const bool wasOpen = editorHost.isVisible();
        editorHost.clear();
        editorHost.setVisible(false);
        inlineNode = 0;
        repaint();

        if (wasOpen && restoreSize && onRequestSize)
            onRequestSize(preEditorW, preEditorH);
    }

    void showAudioSettings()
    {
        auto* c = new Themed<juce::AudioDeviceSelectorComponent>(engine.deviceManager, 0, 2, 0, 2,
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
                                     syncFaders();
                                     if (err.isNotEmpty())
                                         juce::AlertWindow::showMessageBoxAsync(
                                             juce::MessageBoxIconType::WarningIcon, "Load rack", err);
                                 });
                             });
    }

    //--------------------------------------------------------------------------
    RackEngine& engine;
    HotPlugins hot;
    LevelMeter inMeter { [this](int ch) { return engine.takeInputPeak(ch); }, "IN",
                         "Level arriving at the selected rack." };
    LevelMeter meter { [this](int ch) { return engine.takeOutputPeak(ch); }, "OUT",
                       "Level leaving the selected rack (after its fader)." };
    LevelMeter masterMeter { [this](int ch) { return engine.takeMasterPeak(ch); }, "OUT",
                             "Level of all outputs after the master fader." };
    RackModel rackModel;
    RacksModel racksModel;
    PluginModel pluginModel;
    std::vector<juce::PluginDescription> filtered;
    int totalEffects = 0;
    juce::ListBox pluginList, rackList, racksList;
    std::vector<int> visibleRacks;                      // racks shown in the active tab
    int activeGroup = -1;                               // -1 = "All"
    juce::Viewport tabViewport;
    juce::Component tabContent;
    juce::OwnedArray<SnapButton> tabButtons;
    std::vector<int> tabIds;
    juce::String tabSignature;
    juce::TextEditor searchBox;
    juce::TextButton scanBtn, audioBtn, hotBtn, saveBtn, loadBtn, bypassAllBtn, clearBtn, inputsBtn, addRackBtn, removeRackBtn, eqBtn;
    std::array<SnapButton, kNumSnapshots> snapBtns;
    juce::Label snapLabel;
    EqPanel eqPanel { engine };
    InputsPanel inputsPanel { engine };
    int checking = 0;
    juce::String checkingName;
    juce::File lockFile = getDataDir().getChildFile("session.lock");
    juce::File autosaveFile = getDataDir().getChildFile("autosave.orack");
    bool autosaveArmed = false, autosaveDirty = false;
    juce::Slider rackFader, masterFader;
    int stripDividerX = 0;
    juce::Label rackStripLabel, masterLabel, pluginTitle, rackTitle, racksTitle, inLabel, outLabel;
    juce::ComboBox inCombo, outCombo;
    juce::String patchSignature;
    juce::Rectangle<int> pluginPanel, rackPanel, racksPanel, mixerPanel, statusBar, hotStrip;
    juce::String statusText;
    std::vector<std::unique_ptr<HotChip>> hotChips;
    juce::String hotSignature;
    int dropIndex = -1;
    bool fileDropActive = false;
    InlineEditorHost editorHost;
    juce::uint32 inlineNode = 0;
    int preEditorW = 1180, preEditorH = 720;
    juce::File pluginListFile, deadMansFile;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltipWindow { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
