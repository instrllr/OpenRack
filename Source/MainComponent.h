#pragma once
#include <algorithm>
#include "RackEngine.h"
#include "Theme.h"
#include "HotPlugins.h"
#include "MidiPanel.h"
#include "PluginImages.h"
#include "PluginCategories.h"
#include "ParamFaders.h"
#if OPENRACK_SPLASH_FRAMES
 #include "SplashData.h"
#endif

static constexpr int kRackRowHeight = 72;

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
// The colour a rack is shown in: the one the user picked, or an automatic tint from its name.
inline juce::Colour rackColour(const RackEngine& e, int rack, const juce::String& name)
{
    const auto c = e.getRackColour(rack);
    return c != 0 ? juce::Colour(c) : Theme::tint(name);
}

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

        edit.setTooltip("Open this plugin inside OpenRack");
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
            imageId = s.desc.createIdentifierString();
            details = s.desc.manufacturerName + "  -  " + s.desc.pluginFormatName;

            const int lat = s.node->getProcessor()->getLatencySamples();
            if (lat > 0)
                details += "  -  " + juce::String(lat) + " smp latency";

            if (auto* rp = dynamic_cast<remote::RemotePlugin*>(s.node->getProcessor()))
                details += "  -  " + rp->getStatusText();

            bypass.setToggleState(s.node->isBypassed(), juce::dontSendNotification);
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const bool bypassed = bypass.getToggleState();
        const auto thumb = PluginImages::get(imageId);
        const bool narrow = getWidth() < 420;       // in the chain column beside an open plugin
        int textX = 66;
        if (thumb.isValid())
        {
            const float boxH = narrow ? 34.0f : (float) getHeight() - 16.0f;
            const float boxW = narrow ? 54.0f : juce::jmin(112.0f, boxH * 1.6f);
            const auto box = juce::Rectangle<float>(66.0f, 8.0f, boxW, boxH);
            g.setColour(Theme::control());
            g.fillRoundedRectangle(box, 5.0f);
            g.saveState();
            juce::Path clip;
            clip.addRoundedRectangle(box, 5.0f);
            g.reduceClipRegion(clip);
            g.setOpacity(bypassed ? 0.4f : 1.0f);
            g.drawImage(thumb, box, juce::RectanglePlacement::centred);
            g.restoreState();
            g.setColour(Theme::outline());
            g.drawRoundedRectangle(box, 5.0f, 1.0f);
            textX = 66 + (int) boxW + 12;
        }
        const int textW = narrow ? juce::jmax(40, getWidth() - textX - 8) : juce::jmax(40, edit.getX() - textX - 8);
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
        if (narrow)
            g.drawText(name, textX, 5, textW, 20, juce::Justification::centredLeft, true);
        else
            g.drawText(name, textX, 7, textW, getHeight() / 2 - 5, juce::Justification::centredLeft, true);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.0f));
        if (narrow)
            g.drawText(details, textX, 25, textW, 16, juce::Justification::centredLeft, true);
        else
            g.drawText(details, textX, getHeight() / 2, textW, getHeight() / 2 - 7,
                       juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        if (getWidth() < 420)
        {
            auto r = juce::Rectangle<int>(66, getHeight() - 28, getWidth() - 66 - 8, 22);
            remove.setBounds(r.removeFromRight(26));
            r.removeFromRight(3);
            down.setBounds(r.removeFromRight(26));
            r.removeFromRight(3);
            up.setBounds(r.removeFromRight(26));
            r.removeFromRight(8);
            bypass.setBounds(r.removeFromRight(62));
            r.removeFromRight(3);
            edit.setBounds(r.removeFromRight(46));
            return;
        }

        auto r = getLocalBounds().reduced(8, 0).withSizeKeepingCentre(getWidth() - 16, 30);
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
    juce::String name, details, imageId;
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
    std::function<void()> onDoesNotFit;            // the plugin is bigger than the room it has

    InlineEditorHost()
    {
        backBtn.setButtonText("Close editor");
        backBtn.onClick = [this] { if (onBack) onBack(); };
        addAndMakeVisible(backBtn);

        title.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        title.setColour(juce::Label::textColourId, Theme::text());
        addAndMakeVisible(title);

        viewport.setViewedComponent(&holder, false);
        viewport.setScrollBarsShown(true, true);        // a plugin bigger than the room scrolls; it always stays inside the client
        addAndMakeVisible(viewport);
    }

    ~InlineEditorHost() override { clear(); }

    std::function<void()> onImageChanged;

    void show(juce::AudioProcessor& p, const juce::String& name, const juce::String& imageId = {})
    {
        clear();
        imageKey = imageId;

        juce::AudioProcessorEditor* ed = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (ed == nullptr)
            ed = new juce::GenericAudioProcessorEditor(p);

        editor.reset(ed);
        title.setText(name, juce::dontSendNotification);
        holder.addAndMakeVisible(*editor);
        editor->addComponentListener(this);

        layoutEditor();

        // Once the editor has drawn itself, keep a picture of it for the rack row.
        juce::Component::SafePointer<InlineEditorHost> safe(this);
        juce::Timer::callAfterDelay(900, [safe] { if (safe != nullptr) safe->capture(); });
    }

    void capture()
    {
        if (editor == nullptr || imageKey.isEmpty() || editor->getWidth() < 40)
            return;
        const auto shot = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
        if (PluginImages::setCaptured(imageKey, shot) && onImageChanged)
            onImageChanged();
    }

    void clear()
    {
        capture();
        if (editor != nullptr)
        {
            editor->removeComponentListener(this);
            holder.removeChildComponent(editor.get());
            editor.reset();
        }
    }

    void paint(juce::Graphics& g) override
    {
        // A panel like the others in the window, with the editor sitting centred inside it.
        auto p = getLocalBounds().toFloat();
        g.setGradientFill(juce::ColourGradient(Theme::panel().brighter(0.06f), 0.0f, p.getY(),
                                               Theme::panel(), 0.0f, p.getBottom(), false));
        g.fillRoundedRectangle(p, 10.0f);
        g.setColour(Theme::outline());
        g.drawRoundedRectangle(p.reduced(0.5f), 10.0f, 1.0f);
    }

    // True when the plugin is bigger than the room it has been given.
    bool tooBig() const
    {
        return editor != nullptr && viewport.getWidth() > 0
               && (editor->getWidth() + 16 > viewport.getMaximumVisibleWidth()
                   || editor->getHeight() + 16 > viewport.getMaximumVisibleHeight());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10, 8);
        auto top = r.removeFromTop(30);
        backBtn.setBounds(top.removeFromRight(110));
        title.setBounds(top.reduced(4, 0));
        r.removeFromTop(6);
        viewport.setBounds(r);
        layoutEditor();
        if (tooBig() && onDoesNotFit)
            onDoesNotFit();
    }

private:
    juce::String imageKey;

    // The editor is centred, both ways, in whatever room there is; when there is not enough room
    // it scrolls.
    void layoutEditor()
    {
        const int vw = viewport.getMaximumVisibleWidth();
        const int vh = viewport.getMaximumVisibleHeight();
        const int ew = editor != nullptr ? editor->getWidth() : 0;
        const int eh = editor != nullptr ? editor->getHeight() : 0;
        const int hw = juce::jmax(vw, ew + 16);
        const int hh = juce::jmax(vh, eh + 16);
        if (holder.getWidth() != hw || holder.getHeight() != hh)
            holder.setSize(hw, hh);
        if (editor != nullptr)
            editor->setTopLeftPosition((hw - ew) / 2, (hh - eh) / 2);
    }

    void componentMovedOrResized(juce::Component&, bool, bool wasResized) override
    {
        if (wasResized)
        {
            layoutEditor();
            if (tooBig() && onDoesNotFit)
                onDoesNotFit();
        }
    }

    juce::TextButton backBtn;
    juce::Label title;
    juce::Viewport viewport;
    juce::Component holder;
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

// Patch bay: a crosspoint matrix in the style of a console patcher. Rows are the racks, columns are
// the stereo sockets (inputs or outputs). Click a crosspoint to patch, click it again to unpatch.
class PatchPanel : public juce::Component
{
public:
    static constexpr int kCellW = 25, kCellH = 25, kRowHeaderW = 170, kColHeaderH = 38;

    explicit PatchPanel(RackEngine& e) : engine(e), grid(*this), rowHeader(*this), colHeader(*this)
    {
        view.setViewedComponent(&grid, false);
        view.setScrollBarsShown(true, true);
        view.onScroll = [this] { rowHeader.repaint(); colHeader.repaint(); };
        addAndMakeVisible(view);
        addAndMakeVisible(rowHeader);
        addAndMakeVisible(colHeader);

        for (auto* b : { &inBtn, &outBtn, &directBtn, &defaultBtn, &clearBtn })
            addAndMakeVisible(b);
        inBtn.setButtonText("Input patch");
        outBtn.setButtonText("Output patch");
        directBtn.setButtonText("Direct I/O");
        directBtn.setTooltip("Route an input socket straight to an output socket, without a rack");
        directBtn.setClickingTogglesState(false);
        directBtn.setColour(juce::TextButton::buttonOnColourId, Theme::warning());
        directBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1a1200));
        directBtn.onClick = [this] { setMode(2); };
        defaultBtn.setButtonText("Patch 1:1");
        clearBtn.setButtonText("Clear all");
        inBtn.setClickingTogglesState(false);
        outBtn.setClickingTogglesState(false);
        inBtn.setColour(juce::TextButton::buttonOnColourId, Theme::accent());
        outBtn.setColour(juce::TextButton::buttonOnColourId, Theme::accent());
        inBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        outBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        inBtn.onClick = [this] { setMode(0); };
        outBtn.onClick = [this] { setMode(1); };
        defaultBtn.setTooltip("Patch rack 1 to socket pair 1-2, rack 2 to 3-4, and so on");
        clearBtn.setTooltip("Remove every patch in this view");
        defaultBtn.onClick = [this] { patchStraight(); };
        clearBtn.onClick = [this] { clearAll(); };
        setMode(0);
    }

    void refresh()
    {
        // Always the full 64 x 64 stereo grid (128 x 128 channels).
        columns = kMaxInputBuses;
        grid.setSize(columns * kCellW, rowCount() * kCellH);
        grid.repaint();
        rowHeader.repaint();
        colHeader.repaint();
        repaint();
    }

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
        g.drawText(direct ? "DIRECT I/O  -  64 stereo inputs straight to 64 stereo outputs, no rack in between"
                          : outputs ? "OUTPUT PATCH  -  64 racks to 64 stereo outputs (128 channels)"
                                    : "INPUT PATCH  -  64 stereo inputs (128 channels) to 64 racks",
                   16, 8, juce::jmax(100, inBtn.getX() - 32), 20, juce::Justification::centredLeft, true);
        g.setFont(juce::FontOptions(12.0f));
        const int avail = availableCols();
        const juce::String note = direct
            ? juce::String("Rows are inputs, columns are outputs. Click to route, click again to remove. "
                           "Direct routes pass through the master fader but no rack. ")
            : juce::String("Click a crosspoint to patch, click again to remove. Clicking an empty rack row creates the rack. ");
        g.drawText(note + "Dimmed sockets are not on your device right now (" + juce::String(avail)
                       + " of 64 pairs available).",
                   descArea.withTrimmedLeft(8), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8, 6);
        auto top = r.removeFromTop(48);
        clearBtn.setBounds(top.removeFromRight(96).reduced(0, 8));
        top.removeFromRight(6);
        defaultBtn.setBounds(top.removeFromRight(96).reduced(0, 8));
        top.removeFromRight(20);
        directBtn.setBounds(top.removeFromRight(110).reduced(0, 8));
        top.removeFromRight(4);
        outBtn.setBounds(top.removeFromRight(110).reduced(0, 8));
        top.removeFromRight(4);
        inBtn.setBounds(top.removeFromRight(110).reduced(0, 8));

        descArea = r.removeFromTop(22);
        auto body = r.withTrimmedTop(2);
        colHeader.setBounds(body.getX() + kRowHeaderW, body.getY(), body.getWidth() - kRowHeaderW, kColHeaderH);
        rowHeader.setBounds(body.getX(), body.getY() + kColHeaderH, kRowHeaderW, body.getHeight() - kColHeaderH);
        view.setBounds(body.getX() + kRowHeaderW, body.getY() + kColHeaderH,
                       body.getWidth() - kRowHeaderW, body.getHeight() - kColHeaderH);
        refresh();
    }

private:
    int availableCols() const { return outputs || direct ? engine.getNumOutputPairs() : engine.getNumInputPairs(); }
    int rowCount() const { return direct ? kMaxInputBuses : kMaxRacks; }

    int patchOf(int rack) const { return outputs ? engine.getRackOutputPair(rack) : engine.getRackInputPair(rack); }

    void setPatch(int rack, int pair)
    {
        if (outputs) engine.setRackOutputPair(rack, pair);
        else         engine.setRackInputPair(rack, pair);
    }

    void setMode(int m)
    {
        outputs = (m == 1);
        direct = (m == 2);
        inBtn.setToggleState(m == 0, juce::dontSendNotification);
        outBtn.setToggleState(m == 1, juce::dontSendNotification);
        directBtn.setToggleState(m == 2, juce::dontSendNotification);
        defaultBtn.setTooltip(direct ? "Route input 1-2 to output 1-2, 3-4 to 3-4, and so on"
                                     : "Patch rack 1 to socket pair 1-2, rack 2 to 3-4, and so on");
        refresh();
    }

    void patchStraight()
    {
        if (direct)
        {
            const int n = juce::jmin(engine.getNumInputPairs(), engine.getNumOutputPairs());
            for (int i = 0; i < n; ++i)
                engine.setDirect(i, i, true);
            return;
        }

        const int cols = outputs ? engine.getNumOutputPairs() : engine.getNumInputPairs();
        for (int r = 0; r < engine.getNumRacks(); ++r)
            setPatch(r, r < cols ? r : -1);
    }

    void clearAll()
    {
        if (direct)
        {
            engine.clearDirect();
            return;
        }

        for (int r = 0; r < engine.getNumRacks(); ++r)
            setPatch(r, -1);
    }

    static juce::String pairName(int pair) { return juce::String(pair * 2 + 1) + "-" + juce::String(pair * 2 + 2); }

    //----------------------------------------------------------------------
    struct ScrollView : juce::Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged(const juce::Rectangle<int>&) override { if (onScroll) onScroll(); }
    };

    struct Grid : juce::Component
    {
        explicit Grid(PatchPanel& o) : owner(o) {}

        void paint(juce::Graphics& g) override
        {
            const int racks = owner.engine.getNumRacks();
            const int sel = owner.direct ? -1 : owner.engine.getSelectedRack();
            const auto clip = g.getClipBounds();
            const int avail = owner.availableCols();
            const int rowsN = owner.rowCount();
            const int availRows = owner.direct ? owner.engine.getNumInputPairs() : racks;
            const int r0 = juce::jmax(0, clip.getY() / kCellH), r1 = juce::jmin(rowsN - 1, clip.getBottom() / kCellH);
            const int c0 = juce::jmax(0, clip.getX() / kCellW), c1 = juce::jmin(owner.columns - 1, clip.getRight() / kCellW);
            const auto dot = owner.direct ? Theme::warning() : owner.outputs ? juce::Colour(0xff3ddc84) : Theme::accent();

            for (int r = r0; r <= r1; ++r)
            {
                const bool exists = r < availRows;
                const int patched = (exists && ! owner.direct) ? owner.patchOf(r) : -1;
                for (int c = c0; c <= c1; ++c)
                {
                    auto cell = juce::Rectangle<float>((float) (c * kCellW), (float) (r * kCellH),
                                                       (float) kCellW, (float) kCellH).reduced(1.0f);
                    const bool band = ((c / 4) % 2) == 0;         // groups of 4 pairs (8 channels)
                    auto base = r == sel ? Theme::row().brighter(0.08f) : (band ? Theme::row() : Theme::row().darker(0.2f));
                    if (! exists || c >= avail)
                        base = base.darker(0.35f);
                    g.setColour(base);
                    g.fillRoundedRectangle(cell, 4.0f);

                    if (r == hoverRow || c == hoverCol)
                    {
                        g.setColour(Theme::accent().withAlpha(0.12f));
                        g.fillRoundedRectangle(cell, 4.0f);
                    }

                    auto centre = cell.getCentre();
                    if (owner.direct ? owner.engine.hasDirect(r, c) : c == patched)
                    {
                        g.setColour(dot);
                        g.fillEllipse(centre.x - 7.0f, centre.y - 7.0f, 14.0f, 14.0f);
                    }
                    else if (r == hoverRow && c == hoverCol)
                    {
                        g.setColour(dot.withAlpha(0.35f));
                        g.fillEllipse(centre.x - 6.0f, centre.y - 6.0f, 12.0f, 12.0f);
                    }
                }
            }
        }

        void mouseMove(const juce::MouseEvent& e) override { hover(e.getPosition()); }
        void mouseDrag(const juce::MouseEvent& e) override { hover(e.getPosition()); }
        void mouseExit(const juce::MouseEvent&) override { hoverRow = hoverCol = -1; owner.refreshHeaders(); repaint(); }

        void mouseDown(const juce::MouseEvent& e) override
        {
            const int r = e.y / kCellH, c = e.x / kCellW;
            if (e.x < 0 || e.y < 0 || r >= owner.rowCount() || c >= owner.columns)
                return;
            if (owner.direct)
            {
                owner.engine.setDirect(r, c, ! owner.engine.hasDirect(r, c));
                return;
            }
            while (owner.engine.getNumRacks() <= r && owner.engine.addRack()) {}
            if (r >= owner.engine.getNumRacks())
                return;
            owner.engine.selectRack(r);
            owner.setPatch(r, owner.patchOf(r) == c ? -1 : c);
        }

        void hover(juce::Point<int> p)
        {
            const int r = p.y / kCellH, c = p.x / kCellW;
            const bool ok = p.x >= 0 && p.y >= 0 && r < owner.rowCount() && c < owner.columns;
            const int nr = ok ? r : -1, nc = ok ? c : -1;
            if (nr != hoverRow || nc != hoverCol)
            {
                hoverRow = nr;
                hoverCol = nc;
                owner.refreshHeaders();
                repaint();
            }
        }

        PatchPanel& owner;
        int hoverRow = -1, hoverCol = -1;
    };

    struct RowHeader : juce::Component
    {
        explicit RowHeader(PatchPanel& o) : owner(o) {}

        void paint(juce::Graphics& g) override
        {
            const int viewY = owner.view.getViewPositionY();
            const int sel = owner.engine.getSelectedRack();
            for (int r = 0; r < owner.rowCount(); ++r)
            {
                const bool exists = owner.direct ? r < owner.engine.getNumInputPairs()
                                                 : r < owner.engine.getNumRacks();
                const int y = r * kCellH - viewY;
                if (y + kCellH < 0 || y > getHeight()) continue;

                auto cell = juce::Rectangle<float>(0.0f, (float) y, (float) getWidth() - 4.0f, (float) kCellH).reduced(1.0f);
                g.setColour(r == sel ? Theme::row().brighter(0.12f) : Theme::row());
                g.fillRoundedRectangle(cell, 4.0f);
                if (r == owner.grid.hoverRow)
                {
                    g.setColour(Theme::accent().withAlpha(0.2f));
                    g.fillRoundedRectangle(cell, 4.0f);
                }

                if (owner.direct)
                {
                    g.setColour(exists ? Theme::muted() : Theme::muted().withAlpha(0.5f));
                    g.setFont(juce::FontOptions(11.5f));
                    g.drawText("IN  " + pairName(r), cell.withTrimmedLeft(10.0f).toNearestInt(),
                               juce::Justification::centredLeft, true);
                    continue;
                }

                if (! exists)
                {
                    g.setColour(Theme::muted().withAlpha(0.5f));
                    g.setFont(juce::FontOptions(11.0f));
                    g.drawText(juce::String(r + 1) + "  (empty)", cell.withTrimmedLeft(14.0f).toNearestInt(),
                               juce::Justification::centredLeft, true);
                    continue;
                }

                const auto name = owner.engine.getRackName(r);
                g.setColour(rackColour(owner.engine, r, name));
                g.fillRoundedRectangle(cell.getX() + 4.0f, cell.getY() + 5.0f, 4.0f, cell.getHeight() - 10.0f, 2.0f);
                g.setColour(r == sel ? Theme::text() : Theme::muted());
                g.setFont(juce::FontOptions(12.0f, r == sel ? juce::Font::bold : juce::Font::plain));
                g.drawText(juce::String(r + 1) + "  " + name, cell.withTrimmedLeft(14.0f).toNearestInt(),
                           juce::Justification::centredLeft, true);

                const int p = owner.patchOf(r);
                g.setColour(Theme::muted());
                g.setFont(juce::FontOptions(10.5f));
                g.drawText(p >= 0 ? pairName(p) : "-", cell.toNearestInt().withTrimmedRight(6),
                           juce::Justification::centredRight);
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            const int r = (e.y + owner.view.getViewPositionY()) / kCellH;
            if (! owner.direct && r >= 0 && r < owner.engine.getNumRacks())
                owner.engine.selectRack(r);
        }

        PatchPanel& owner;
    };

    struct ColHeader : juce::Component
    {
        explicit ColHeader(PatchPanel& o) : owner(o) {}

        void paint(juce::Graphics& g) override
        {
            const int viewX = owner.view.getViewPositionX();
            const int patched = (! owner.direct && owner.engine.getNumRacks() > 0) ? owner.patchOf(owner.engine.getSelectedRack()) : -1;
            for (int c = 0; c < owner.columns; ++c)
            {
                const int x = c * kCellW - viewX;
                if (x + kCellW < 0 || x > getWidth()) continue;

                auto cell = juce::Rectangle<float>((float) x, 0.0f, (float) kCellW, (float) getHeight() - 4.0f).reduced(1.0f, 0.0f);
                g.setColour(c == patched ? Theme::accent().withAlpha(0.35f)
                                         : (c < owner.availableCols() ? Theme::row() : Theme::row().darker(0.35f)));
                g.fillRoundedRectangle(cell, 4.0f);
                if (c == owner.grid.hoverCol)
                {
                    g.setColour(Theme::accent().withAlpha(0.2f));
                    g.fillRoundedRectangle(cell, 4.0f);
                }

                g.setColour(Theme::muted());
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText(juce::String(c * 2 + 1), cell.withHeight(cell.getHeight() * 0.5f).toNearestInt(),
                           juce::Justification::centred);
                g.drawText(juce::String(c * 2 + 2),
                           cell.withTrimmedTop(cell.getHeight() * 0.5f).toNearestInt(), juce::Justification::centred);
            }
        }

        PatchPanel& owner;
    };

    void refreshHeaders() { rowHeader.repaint(); colHeader.repaint(); }

    RackEngine& engine;
    juce::Rectangle<int> descArea;
    bool outputs = false, direct = false;
    int columns = 0;
    ScrollView view;
    Grid grid;
    RowHeader rowHeader;
    ColHeader colHeader;
    juce::TextButton inBtn, outBtn, directBtn, defaultBtn, clearBtn;
};

// Tick the racks that belong to a group. Changes apply straight away.
class GroupPicker : public juce::Component
{
public:
    GroupPicker(RackEngine& e, juce::uint32 groupId) : engine(e), gid(groupId)
    {
        title.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        addAndMakeVisible(title);
        hint.setColour(juce::Label::textColourId, Theme::muted());
        hint.setFont(juce::FontOptions(12.0f));
        hint.setText("Tick a rack to put it in this group. Racks that are in another group move over.",
                     juce::dontSendNotification);
        addAndMakeVisible(hint);

        for (int i = 0; i < e.getNumRacks(); ++i)
        {
            auto* b = boxes.add(new juce::ToggleButton());
            b->setColour(juce::ToggleButton::textColourId, Theme::text());
            b->onClick = [this, i] { engine.setRackGroup(i, boxes[i]->getToggleState() ? gid : 0); update(); };
            content.addAndMakeVisible(b);
        }

        viewport.setViewedComponent(&content, false);
        addAndMakeVisible(viewport);

        for (auto* b : { &allBtn, &noneBtn, &doneBtn })
            addAndMakeVisible(b);
        allBtn.setButtonText("Select all");
        noneBtn.setButtonText("Select none");
        doneBtn.setButtonText("Done");
        allBtn.onClick = [this] { setAll(true); };
        noneBtn.onClick = [this] { setAll(false); };
        doneBtn.onClick = [this]
        {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        };
        update();
        setSize(460, 520);
    }

    void paint(juce::Graphics& g) override { g.fillAll(Theme::background()); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14, 12);
        title.setBounds(r.removeFromTop(22));
        hint.setBounds(r.removeFromTop(20));
        r.removeFromTop(6);
        auto foot = r.removeFromBottom(34);
        r.removeFromBottom(8);
        doneBtn.setBounds(foot.removeFromRight(90));
        foot.removeFromRight(8);
        noneBtn.setBounds(foot.removeFromRight(100));
        foot.removeFromRight(8);
        allBtn.setBounds(foot.removeFromRight(100));

        viewport.setBounds(r);
        const int w = juce::jmax(100, viewport.getMaximumVisibleWidth());
        content.setSize(w, boxes.size() * 28);
        for (int i = 0; i < boxes.size(); ++i)
            boxes[i]->setBounds(4, i * 28, w - 8, 28);
    }

private:
    juce::String groupName(juce::uint32 id) const
    {
        for (int i = 0; i < engine.getNumGroups(); ++i)
            if (engine.getGroupId(i) == id) return engine.getGroupName(i);
        return {};
    }

    void update()
    {
        title.setText("Racks in \"" + groupName(gid) + "\"", juce::dontSendNotification);
        for (int i = 0; i < boxes.size(); ++i)
        {
            const auto g = engine.getRackGroup(i);
            auto text = juce::String(i + 1) + "   " + engine.getRackName(i);
            if (g != 0 && g != gid)
                text += "     (in " + groupName(g) + ")";
            boxes[i]->setButtonText(text);
            boxes[i]->setToggleState(g == gid, juce::dontSendNotification);
        }
    }

    void setAll(bool on)
    {
        for (int i = 0; i < boxes.size(); ++i)
            if (on || engine.getRackGroup(i) == gid)
                engine.setRackGroup(i, on ? gid : 0);
        update();
    }

    RackEngine& engine;
    juce::uint32 gid;
    juce::Label title, hint;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<juce::ToggleButton> boxes;
    juce::TextButton allBtn, noneBtn, doneBtn;
};

// Full-size view of the shows library: every saved show in a list, with actions on the right.
class ShowsPanel : public juce::Component, private juce::ListBoxModel
{
public:
    std::function<void(const juce::File&)> onOpen, onRename, onDelete;
    std::function<void()> onSave, onSaveAs, onImport, onExport, onReveal;

    explicit ShowsPanel(juce::File dir) : folder(std::move(dir))
    {
        list.setModel(this);
        list.setRowHeight(54);
        addAndMakeVisible(list);

        for (auto* b : { &openBtn, &saveBtn, &saveAsBtn, &renameBtn, &deleteBtn, &importBtn, &exportBtn, &folderBtn })
            addAndMakeVisible(b);

        openBtn.setButtonText("Open selected");
        saveBtn.setButtonText("Save current");
        saveAsBtn.setButtonText("Save as new...");
        renameBtn.setButtonText("Rename...");
        deleteBtn.setButtonText("Delete...");
        importBtn.setButtonText("Import file...");
        exportBtn.setButtonText("Export file...");
        folderBtn.setButtonText("Show folder");
        openBtn.setColour(juce::TextButton::buttonColourId, Theme::accent());
        openBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);

        openBtn.onClick = [this] { if (auto f = selected(); f != juce::File() && onOpen) onOpen(f); };
        saveBtn.onClick = [this] { if (onSave) onSave(); };
        saveAsBtn.onClick = [this] { if (onSaveAs) onSaveAs(); };
        renameBtn.onClick = [this] { if (auto f = selected(); f != juce::File() && onRename) onRename(f); };
        deleteBtn.onClick = [this] { if (auto f = selected(); f != juce::File() && onDelete) onDelete(f); };
        importBtn.onClick = [this] { if (onImport) onImport(); };
        exportBtn.onClick = [this] { if (onExport) onExport(); };
        folderBtn.onClick = [this] { if (onReveal) onReveal(); };

        openBtn.setTooltip("Load the selected show (or double-click it)");
        saveBtn.setTooltip("Save the current rack setup over the show that is open");
        saveAsBtn.setTooltip("Save the current rack setup as a new show");
        importBtn.setTooltip("Copy a .oshow / .orack file from anywhere into the library");
        exportBtn.setTooltip("Write the current show to a file you can back up or share");
    }

    void setCurrent(const juce::String& name) { current = name; refresh(); }

    void refresh()
    {
        const auto keep = selected();
        files.clear();
        for (auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.oshow;*.orack"))
            files.add(f);

        struct ByName
        {
            static int compareElements(const juce::File& a, const juce::File& b)
            {
                return a.getFileNameWithoutExtension().compareNatural(b.getFileNameWithoutExtension());
            }
        };
        ByName sorter;
        files.sort(sorter);

        list.updateContent();
        const int idx = keep != juce::File() ? files.indexOf(keep) : -1;
        const int cur = idx >= 0 ? idx : indexOfName(current);
        if (cur >= 0) list.selectRow(cur);
        else          list.deselectAllRows();
        list.repaint();
        updateButtons();
    }

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
        g.drawText("SHOWS  -  " + juce::String(files.size()) + " saved", 16, 8, getWidth() - 32, 20,
                   juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(current.isNotEmpty() ? "Current show: " + current
                                        : "No show open yet - use Save as new... to keep this setup",
                   16, 28, getWidth() - 32, 18, juce::Justification::centredLeft, true);

        if (files.isEmpty())
        {
            g.setFont(juce::FontOptions(15.0f));
            g.drawText("Nothing saved yet. Click \"Save as new...\" to store your racks as a show.",
                       list.getBounds(), juce::Justification::centred, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedTop(54).reduced(12, 6);
        auto side = r.removeFromRight(170);
        r.removeFromRight(12);
        list.setBounds(r);

        int y = side.getY();
        auto place = [&](juce::Button& b, int gapAfter)
        {
            b.setBounds(side.getX(), y, side.getWidth(), 34);
            y += 34 + gapAfter;
        };
        place(openBtn, 16);
        place(saveBtn, 6);
        place(saveAsBtn, 16);
        place(renameBtn, 6);
        place(deleteBtn, 16);
        place(importBtn, 6);
        place(exportBtn, 6);
        place(folderBtn, 0);
    }

private:
    juce::File selected() const
    {
        const int row = list.getSelectedRow();
        return row >= 0 && row < files.size() ? files[row] : juce::File();
    }

    int indexOfName(const juce::String& name) const
    {
        for (int i = 0; i < files.size(); ++i)
            if (files[i].getFileNameWithoutExtension() == name) return i;
        return -1;
    }

    void updateButtons()
    {
        const bool has = selected() != juce::File();
        for (auto* b : { &openBtn, &renameBtn, &deleteBtn })
            b->setEnabled(has);
    }

    int getNumRows() override { return files.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool isSelected) override
    {
        if (row >= files.size()) return;
        const auto& f = files[row];
        const auto name = f.getFileNameWithoutExtension();
        const bool isCurrent = name == current;

        auto r = juce::Rectangle<float>(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f);
        g.setColour(isSelected ? Theme::row().brighter(0.1f) : Theme::row());
        g.fillRoundedRectangle(r, 8.0f);
        if (isSelected)
        {
            g.setColour(Theme::accent());
            g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.5f);
        }

        g.setColour(Theme::tint(name));
        g.fillRoundedRectangle(14.0f, (float) h * 0.5f - 12.0f, 5.0f, 24.0f, 2.5f);

        g.setColour(Theme::text());
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText(name, 30, 6, w - 220, 24, juce::Justification::centredLeft, true);

        g.setColour(Theme::muted());
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Saved " + f.getLastModificationTime().formatted("%d %b %Y, %H:%M")
                       + (f.hasFileExtension("orack") ? "   (old .orack format)" : ""),
                   30, 29, w - 220, 18, juce::Justification::centredLeft, true);

        if (isCurrent)
        {
            auto badge = juce::Rectangle<float>((float) w - 104.0f, (float) h * 0.5f - 11.0f, 88.0f, 22.0f);
            g.setColour(Theme::accent().withAlpha(0.25f));
            g.fillRoundedRectangle(badge, 11.0f);
            g.setColour(Theme::accent());
            g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
            g.drawText("OPEN", badge, juce::Justification::centred);
        }
    }

    void selectedRowsChanged(int) override { updateButtons(); }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (row >= 0 && row < files.size() && onOpen) onOpen(files[row]);
    }

    void deleteKeyPressed(int) override
    {
        if (auto f = selected(); f != juce::File() && onDelete) onDelete(f);
    }

    juce::File folder;
    juce::String current;
    juce::Array<juce::File> files;
    juce::ListBox list;
    juce::TextButton openBtn, saveBtn, saveAsBtn, renameBtn, deleteBtn, importBtn, exportBtn, folderBtn;
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
        colour = rackColour(engine, r, name);
        customColour = engine.getRackColour(r) != 0;
        const int n = engine.getRackNumSlots(r);
        details = "In " + pairName(engine.getRackInputPair(r)) + "  >  Out " + pairName(engine.getRackOutputPair(r))
                  + "   -   " + juce::String(n) + (n == 1 ? " plugin" : " plugins");
        mute.setToggleState(engine.isRackMuted(r), juce::dontSendNotification);
        solo.setToggleState(engine.isRackSoloed(r), juce::dontSendNotification);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        if (customColour)
        {
            g.setColour(colour.withAlpha(mute.getToggleState() ? 0.07f : 0.2f));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 8.0f);
        }
        g.setColour(mute.getToggleState() ? Theme::muted() : colour);
        g.fillRoundedRectangle(8.0f, 10.0f, customColour ? 6.0f : 4.0f, (float) getHeight() - 20.0f, 2.0f);

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
    juce::Colour colour = juce::Colours::grey;
    bool customColour = false;
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
//==============================================================================
// Start-up film: a short clip stored as a numbered set of JPEG frames and drawn by OpenRack itself
// (no system video player involved). It fades in, plays, fades out and then dissolves away to reveal
// the application.
class SplashOverlay : public juce::Component, private juce::Timer
{
public:
    SplashOverlay()
    {
        setInterceptsMouseClicks(true, false);   // block clicks while it is up
        start = juce::Time::getMillisecondCounterHiRes();
#if OPENRACK_SPLASH_FRAMES
        data = reinterpret_cast<const juce::uint8*>(SplashData::splash_frames);
        const auto size = (size_t) SplashData::splash_framesSize;
        if (size > 8)
        {
            memcpy(&count, data, 4);
            if (count > 0 && size >= 4u + 4u * ((size_t) count + 1u))
                offsets = reinterpret_cast<const juce::uint8*>(data + 4);
            else
                count = 0;
        }
#endif
        startTimerHz(60);
    }

    std::function<void()> onFinished;

    void paint(juce::Graphics& g) override
    {
        const float t = elapsed();
        const float filmLength = (float) count / kFps;
        const float cover = 1.0f - ease(juce::jlimit(0.0f, 1.0f, (t - filmLength) / kReveal));

        g.setColour(juce::Colour(0xff101010).withAlpha(cover));
        g.fillAll();

        if (count <= 0 || t >= filmLength)
            return;

        const int index = juce::jlimit(0, count - 1, (int) (t * kFps));
        if (index != shownIndex)
        {
            shownIndex = index;
            frame = decode(index);
        }
        if (! frame.isValid())
            return;

        const float fadeIn = ease(juce::jlimit(0.0f, 1.0f, t / 0.8f));
        const float fadeOut = 1.0f - ease(juce::jlimit(0.0f, 1.0f, (t - (filmLength - 1.0f)) / 1.0f));
        g.setOpacity(juce::jmin(fadeIn, fadeOut));
        g.drawImage(frame, getLocalBounds().toFloat(), juce::RectanglePlacement::centred);
    }

private:
    static constexpr float kFps = 30.0f;
    static constexpr float kReveal = 0.5f;      // seconds the dark cover takes to dissolve at the end

    static float ease(float x) { return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }
    float elapsed() const { return (float) ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0); }

    juce::Image decode(int index) const
    {
#if OPENRACK_SPLASH_FRAMES
        juce::uint32 from = 0, to = 0;
        memcpy(&from, offsets + 4 * index, 4);
        memcpy(&to, offsets + 4 * (index + 1), 4);
        if (to > from && (size_t) to <= (size_t) SplashData::splash_framesSize)
            return juce::ImageFileFormat::loadFrom(data + from, (size_t) (to - from));
#else
        juce::ignoreUnused(index);
#endif
        return {};
    }

    void timerCallback() override
    {
        const float end = (float) count / kFps + kReveal;
        if (elapsed() >= end)
        {
            stopTimer();
            setVisible(false);
            if (onFinished)
                juce::MessageManager::callAsync(onFinished);
            return;
        }
        repaint();
    }

    int count = 0;
    int shownIndex = -1;
    const juce::uint8* data = nullptr;
    const juce::uint8* offsets = nullptr;
    juce::Image frame;
    double start = 0.0;
};

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

        for (auto* b : { &scanBtn, &audioBtn, &hotBtn, &loadBtn, &bypassAllBtn, &clearBtn, &inputsBtn, &addRackBtn, &removeRackBtn, &eqBtn, &patchBtn, &groupsBtn, &midiBtn })
            addAndMakeVisible(b);
        scanBtn.setButtonText("Scan plugins");
        audioBtn.setButtonText("Audio settings");
        hotBtn.setButtonText("Hot plugins");
        loadBtn.setButtonText("Shows");
        bypassAllBtn.setButtonText("Bypass all");
        clearBtn.setButtonText("Clear rack");
        inputsBtn.setButtonText("Inputs");
        groupsBtn.setButtonText("Add racks to group...");
        groupsBtn.setTooltip("Choose which existing racks belong to the open group tab");
        groupsBtn.onClick = [this] { groupsButtonClicked(); };
        midiBtn.setButtonText("MIDI");
        midiBtn.setTooltip("Hardware control: MIDI learn, faders, mute / solo, banks and LED feedback");
        midiBtn.onClick = [this] { toggleMidi(); };
        patchBtn.setButtonText("Patch");
        patchBtn.setTooltip("Patch bay: connect input sockets to racks and racks to output sockets");
        patchBtn.onClick = [this] { togglePatch(); };
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
        loadBtn.setTooltip("Your saved shows: open, save, rename, delete, import and export");
        bypassAllBtn.setTooltip("Switch every plugin in the rack off or on");
        clearBtn.setTooltip("Remove every plugin from the rack");

        scanBtn.onClick = [this] { showScanner(); };
        audioBtn.onClick = [this] { showAudioSettings(); };
        hotBtn.onClick = [this] { showHotMenu(); };
        loadBtn.onClick = [this] { toggleShows(); };
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
        tabViewport.setScrollBarsShown(true, false);
        tabViewport.setScrollBarThickness(6);
        addAndMakeVisible(tabViewport);

        racksList.setModel(&racksModel);
        racksList.setMultipleSelectionEnabled(true);
        racksList.setTooltip("Ctrl/Cmd-click or Shift-click to select several racks, then right-click to group or color them all");
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

        categoryBox.addItem("All types", 1);
        for (int i = 0; i < PluginCategories::all().size(); ++i)
            categoryBox.addItem(PluginCategories::all()[i], 2 + i);
        categoryBox.setSelectedId(1, juce::dontSendNotification);
        categoryBox.setTooltip("Show only one type of plugin. Right-click a plugin to change its type.");
        categoryBox.onChange = [this]
        {
            const int id = categoryBox.getSelectedId();
            categoryFilter = id >= 2 ? PluginCategories::all()[id - 2] : juce::String();
            refresh();
        };
        addAndMakeVisible(categoryBox);

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
        addChildComponent(showsPanel);
        addChildComponent(patchPanel);
        addChildComponent(midiPanel);

        isolateBtn.setButtonText("Isolate plugins");
        isolateBtn.setColour(juce::ToggleButton::textColourId, Theme::text());
        isolateBtn.setToggleState(engine.getIsolatePlugins(), juce::dontSendNotification);
        isolateBtn.setTooltip("Run each newly loaded plugin in its own process, so a crashing plugin cannot take "
                              "OpenRack down (it restarts automatically). Experimental. Standalone app only.");
        isolateBtn.onClick = [this] { engine.setIsolatePlugins(isolateBtn.getToggleState()); };
        isolateBtn.setVisible(engine.ownsAudioDevice());
        addChildComponent(isolateBtn);
        engine.onMessage = [this](const juce::String& m)
        {
            flashText = m;
            flashUntil = juce::Time::getMillisecondCounter() + 9000;
        };
        addAndMakeVisible(midiLed);
        addAndMakeVisible(paramBar);
        engine.onParamFadersChanged = [this] { paramBar.refresh(); };

        if (engine.ownsAudioDevice())
        {
            splash = std::make_unique<SplashOverlay>();
            splash->onFinished = [this] { splash.reset(); };
            addAndMakeVisible(*splash);
            splash->toFront(false);
        }
        midiLed.onClick = [this] { if (! midiPanel.isVisible()) toggleMidi(); };

        tempoLabel.setText("BPM", juce::dontSendNotification);
        tempoLabel.setColour(juce::Label::textColourId, Theme::muted());
        tempoLabel.setJustificationType(juce::Justification::centred);
        tempoLabel.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        tempoLabel.setBorderSize(juce::BorderSize<int>(0));
        tempoSlider.setSliderStyle(juce::Slider::IncDecButtons);
        tempoSlider.setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Horizontal);
        tempoSlider.setRange(tempo::kMin, tempo::kMax, 1.0);
        tempoSlider.setNumDecimalPlacesToDisplay(0);
        tempoSlider.setDoubleClickReturnValue(true, 120.0);
        tempoSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 70, 24);
        tempoSlider.setTooltip("Global tempo (max 500 BPM). Hosted plugins see this as the host tempo. Double-click resets to 120");
        tempoSlider.setValue(engine.getTempo(), juce::dontSendNotification);
        tempoSlider.onValueChange = [this] { engine.setTempo(tempoSlider.getValue()); };
        tapBtn.setButtonText("TAP");
        tapBtn.setTooltip("Tap the tempo");
        tapBtn.onClick = [this] { engine.tapTempo(); };
        if (engine.ownsAudioDevice())
            for (auto* c : std::initializer_list<juce::Component*> { &tempoLabel, &tempoSlider, &tapBtn })
                addAndMakeVisible(c);
        engine.onTempoChanged = [this] { tempoSlider.setValue(engine.getTempo(), juce::dontSendNotification); };

        engine.midi.onChanged = [this] { midiPanel.refresh(); };
        engine.midi.onApplied = [this]
        {
            tempoSlider.setValue(engine.getTempo(), juce::dontSendNotification);
            syncFaders();
            racksList.repaint();
            engine.markDirty();
        };
        showsPanel.onOpen = [this](const juce::File& f) { openShow(f, true); };
        showsPanel.onSave = [this] { saveCurrentShow(); };
        showsPanel.onSaveAs = [this] { saveShowAs(); };
        showsPanel.onRename = [this](const juce::File& f) { renameShow(f); };
        showsPanel.onDelete = [this](const juce::File& f) { deleteShow(f); };
        showsPanel.onImport = [this] { importShow(); };
        showsPanel.onExport = [this] { exportShow(); };
        showsPanel.onReveal = [] { getShowsDir().revealToUser(); };

        // Inline plugin editors (used when running inside a DAW).
        addChildComponent(editorHost);
        editorHost.onBack = [this] { closeInlineEditor(true); };
        editorHost.onImageChanged = [this] { rackList.repaint(); };
        editorHost.onDoesNotFit = [this]
        {
            juce::Component::SafePointer<MainComponent> safe(this);
            juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->ensureEditorFits(); });
        };
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
        engine.onMessage = nullptr;
        engine.midi.onChanged = nullptr;
        engine.midi.onApplied = nullptr;
        engine.onTempoChanged = nullptr;
        engine.onParamFadersChanged = nullptr;
        engine.knownPlugins.removeChangeListener(this);
    }

    // Double-click the status bar to reset the dropout counter and peak CPU.
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (statusBar.contains(e.getPosition()))
            engine.resetDropouts();
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
        g.drawText(statusText, statusBar.withTrimmedLeft(18).withTrimmedRight(80), juce::Justification::centredLeft, true);
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        if (splash != nullptr || editorHost.isVisible() || inputsPanel.isVisible() || showsPanel.isVisible() || patchPanel.isVisible() || midiPanel.isVisible())
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
        if (splash != nullptr)
            splash->setBounds(getLocalBounds());

        const bool editing = editorHost.isVisible();
        auto r = getLocalBounds().reduced(16);

        auto top = r.removeFromTop(40);
        for (auto* b : { &loadBtn, &patchBtn, &midiBtn, &hotBtn, &inputsBtn, &audioBtn, &scanBtn })
            if (b->isVisible())
                b->setBounds(top.removeFromRight(116).reduced(3, 3));

        if (engine.ownsAudioDevice())
        {
            auto t = top.removeFromRight(214);
            tapBtn.setBounds(t.removeFromRight(56).reduced(2, 4));
            t.removeFromRight(4);
            auto sl = t.removeFromRight(150);
            tempoLabel.setBounds(sl.getX(), sl.getY() + 1, 70, 12);   // "BPM" sits above the value
            tempoSlider.setBounds(sl.withTrimmedTop(14).withTrimmedBottom(2));
        }

        r.removeFromTop(16);

        hotStrip = r.removeFromTop(36);
        r.removeFromTop(12);

        statusBar = r.removeFromBottom(20);
        midiLed.setBounds(statusBar.getRight() - 70, statusBar.getY(), 70, statusBar.getHeight());
        r.removeFromBottom(8);

        midiPanel.setBounds(hotStrip.getX(), hotStrip.getY(), hotStrip.getWidth(),
                            r.getBottom() - hotStrip.getY());
        patchPanel.setBounds(hotStrip.getX(), hotStrip.getY(), hotStrip.getWidth(),
                             r.getBottom() - hotStrip.getY());
        showsPanel.setBounds(hotStrip.getX(), hotStrip.getY(), hotStrip.getWidth(),
                             r.getBottom() - hotStrip.getY());
        inputsPanel.setBounds(hotStrip.getX(), hotStrip.getY(), hotStrip.getWidth(),
                              r.getBottom() - hotStrip.getY());

        // The rack tabs step aside while a plugin is open, to leave more room for it.
        tabViewport.setVisible(! editing);
        if (! editing)
        {
            tabViewport.setBounds(r.removeFromLeft(108));
            r.removeFromLeft(12);
            layoutTabs();
        }

        const bool compact = editing && editorCompact;     // racks column steps aside too, for a big plugin
        juce::Rectangle<int> racksCol;
        if (! compact)
        {
            racksCol = r.removeFromRight(editing ? juce::jlimit(180, 210, r.getWidth() / 6)
                                                 : juce::jlimit(200, 260, r.getWidth() / 4));
            r.removeFromRight(16);
        }
        for (auto* c : std::initializer_list<juce::Component*> { &racksTitle, &addRackBtn, &removeRackBtn, &groupsBtn, &racksList,
                                                                  &rackStripLabel, &masterLabel, &inMeter, &meter, &rackFader,
                                                                  &masterMeter, &masterFader })
            c->setVisible(! compact);

        // While a plugin is open, the plugin library gives way to the rack's chain (like a hardware
        // rack: the chain on one side, the open plugin in the middle).
        juce::Rectangle<int> chainCol;
        if (editing)
        {
            chainCol = r.removeFromLeft(juce::jlimit(250, 300, r.getWidth() / 5));
            r.removeFromLeft(16);
            pluginPanel = {};
        }
        else
        {
            auto left = r.removeFromLeft(r.getWidth() * 2 / 5);
            r.removeFromLeft(16);

            {
                auto titleRow = left.removeFromTop(24);
                isolateBtn.setBounds(titleRow.removeFromRight(180));
                pluginTitle.setBounds(titleRow);
            }
            searchBox.setBounds(left.removeFromTop(32));
            left.removeFromTop(6);
            categoryBox.setBounds(left.removeFromTop(28));
            left.removeFromTop(8);
            pluginPanel = left;
            pluginList.setBounds(left.reduced(6));
        }
        pluginTitle.setVisible(! editing);
        isolateBtn.setVisible(! editing && engine.ownsAudioDevice());
        searchBox.setVisible(! editing);
        categoryBox.setVisible(! editing);
        pluginList.setVisible(! editing);

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
        juce::Rectangle<int> chainArea;
        if (editing)
        {
            rackPanel = chainCol;
            chainArea = chainCol.reduced(10, 8);
            auto editArea = r;
            if (editArea.getHeight() > 340)
            {
                paramBar.setBounds(editArea.removeFromBottom(ParamFaderBar::kHeight));
                editArea.removeFromBottom(6);
                paramBar.setVisible(true);
            }
            else
                paramBar.setVisible(false);
            editorHost.setBounds(editArea);
        }
        else
        {
            rackPanel = r;
            chainArea = r.reduced(10, 8);
            if (chainArea.getHeight() > 300)
            {
                paramBar.setBounds(chainArea.removeFromBottom(ParamFaderBar::kHeight));
                chainArea.removeFromBottom(6);
                paramBar.setVisible(true);
            }
            else
                paramBar.setVisible(false);
        }
        eqPanel.setBounds(rackPanel);
        auto patchRow = chainArea.removeFromTop(28);
        inLabel.setBounds(patchRow.removeFromLeft(24));
        inCombo.setBounds(patchRow.removeFromLeft((patchRow.getWidth() - 40) / 2));
        patchRow.removeFromLeft(10);
        outLabel.setBounds(patchRow.removeFromLeft(30));
        outCombo.setBounds(patchRow);
        chainArea.removeFromTop(6);
        rackList.setBounds(chainArea);

        if (compact)
        {
            racksPanel = {};
            mixerPanel = {};
            layoutHotChips();
            return;
        }

        racksTitle.setBounds(racksCol.removeFromTop(24));
        auto rackBtns = racksCol.removeFromTop(32);
        addRackBtn.setBounds(rackBtns.removeFromLeft(rackBtns.getWidth() / 2).reduced(0, 2).withTrimmedRight(3));
        removeRackBtn.setBounds(rackBtns.reduced(0, 2).withTrimmedLeft(3));
        groupsBtn.setBounds(racksCol.removeFromTop(32).reduced(0, 2));
        racksCol.removeFromTop(6);

        // Mixer strips (vertical faders and meters) under the racks list
        mixerPanel = racksCol.removeFromBottom(juce::jmin(260, racksCol.getHeight() / 2));
        racksCol.removeFromBottom(12);
        racksPanel = racksCol;
        racksList.setBounds(racksCol.reduced(6));

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
        if (inputsPanel.isVisible() || eqPanel.isVisible() || showsPanel.isVisible() || patchPanel.isVisible() || midiPanel.isVisible())
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
        if (editorHost.isVisible() || inputsPanel.isVisible() || eqPanel.isVisible() || showsPanel.isVisible() || patchPanel.isVisible() || midiPanel.isVisible())
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
        void listBoxItemClicked(int row, const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && row >= 0 && row < getNumRows())
                owner.showPluginImageMenu(row);
        }
        MainComponent& owner;
    };

    // The racks sidebar shows the racks of the active tab; `row` is a position in that list.
    struct RacksModel : juce::ListBoxModel
    {
        explicit RacksModel(MainComponent& o) : owner(o) {}
        int getNumRows() override { return (int) owner.visibleRacks.size(); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            auto r = juce::Rectangle<float>(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f);
            g.setColour(selected ? Theme::row().brighter(0.1f) : Theme::row());
            g.fillRoundedRectangle(r, 8.0f);
            if (selected)
            {
                g.setColour(Theme::accent());
                g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.5f);
            }

            // a thin bar marks the racks the hardware controller currently controls (its active bank)
            if (row >= 0 && row < (int) owner.visibleRacks.size()
                && owner.engine.midi.isRackInBank(owner.visibleRacks[(size_t) row]))
            {
                g.setColour(juce::Colour(0xff3ddc84));
                g.fillRoundedRectangle(3.0f, 10.0f, 3.0f, (float) h - 20.0f, 1.5f);
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
            g.drawText(PluginCategories::of(d) + "  -  " + d.manufacturerName, area.removeFromRight(area.getWidth() * 2 / 5),
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

            const auto cat = PluginCategories::of(t);
            if (categoryFilter.isNotEmpty() && cat != categoryFilter)
                continue;

            const juce::String haystack = t.name + " " + t.manufacturerName + " "
                                        + t.category + " " + t.pluginFormatName + " " + cat;
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
        pluginTitle.setText(words.isEmpty() && categoryFilter.isEmpty() ? "PLUGINS  -  " + juce::String(total)
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
        if (splash != nullptr)
            splash->toFront(false);          // chips are created after the splash: keep it on top
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

        const auto desc = engine.knownPlugins.getTypeForIdentifierString(id);
        if (desc != nullptr)
        {
            const auto current = PluginCategories::of(*desc);
            juce::PopupMenu cats;
            cats.addItem(100, "Automatic", true, ! PluginCategories::isOverridden(*desc));
            cats.addSeparator();
            for (int i = 0; i < PluginCategories::all().size(); ++i)
                cats.addItem(101 + i, PluginCategories::all()[i], true,
                             PluginCategories::isOverridden(*desc) && current == PluginCategories::all()[i]);
            m.addSeparator();
            m.addSubMenu("Type: " + current, cats);
        }

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
            else if (r >= 100 && r <= 100 + PluginCategories::all().size())
            {
                if (auto d = safe->engine.knownPlugins.getTypeForIdentifierString(id))
                {
                    PluginCategories::store().cache.erase(id.toStdString());
                    PluginCategories::setOverride(*d, r == 100 ? juce::String() : PluginCategories::all()[r - 101]);
                    safe->refresh();
                }
            }
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

    void toggleMidi()
    {
        const bool show = ! midiPanel.isVisible();
        if (show)
        {
            if (editorHost.isVisible())
                closeInlineEditor(true);
            if (inputsPanel.isVisible()) toggleInputs();
            if (showsPanel.isVisible()) toggleShows();
            if (patchPanel.isVisible()) togglePatch();
            midiPanel.refreshDevices();
            midiPanel.refresh();
        }
        midiPanel.setVisible(show);
        if (show)
            midiPanel.toFront(false);
        midiBtn.setButtonText(show ? "Rack" : "MIDI");
        repaint();
    }

    void togglePatch()
    {
        const bool show = ! patchPanel.isVisible();
        if (show)
        {
            if (editorHost.isVisible())
                closeInlineEditor(true);
            if (inputsPanel.isVisible())
                toggleInputs();
            if (showsPanel.isVisible())
                toggleShows();
            if (midiPanel.isVisible())
                toggleMidi();
            patchPanel.refresh();
        }
        patchPanel.setVisible(show);
        if (show)
            patchPanel.toFront(false);
        patchBtn.setButtonText(show ? "Rack" : "Patch");
        repaint();
    }

    void toggleShows()
    {
        const bool show = ! showsPanel.isVisible();
        if (show)
        {
            if (patchPanel.isVisible())
                togglePatch();
            if (midiPanel.isVisible())
                toggleMidi();
            if (editorHost.isVisible())
                closeInlineEditor(true);
            if (inputsPanel.isVisible())
                toggleInputs();
            showsPanel.setCurrent(currentShow);
        }
        showsPanel.setVisible(show);
        if (show)
            showsPanel.toFront(false);
        updateShowTitle();
        repaint();
    }

    void toggleInputs()
    {
        const bool show = ! inputsPanel.isVisible();
        if (show)
        {
            if (patchPanel.isVisible())
                togglePatch();
            if (midiPanel.isVisible())
                toggleMidi();
            if (showsPanel.isVisible())
                toggleShows();
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
    void newGroup(int moveRack, std::function<void(juce::uint32)> andThen = {})
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        promptText("New group", "Name for the new group (it becomes a tab):", {},
                   [safe, moveRack, andThen](const juce::String& name)
                   {
                       if (safe == nullptr) return;
                       const auto id = safe->engine.addGroup(name);
                       if (moveRack >= 0)
                           safe->engine.setRackGroup(moveRack, id);
                       safe->activeGroup = (int) id;
                       safe->refresh();
                       if (andThen) andThen(id);
                   });
    }

    void showGroupPicker(juce::uint32 id)
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(new GroupPicker(engine, id));
        o.dialogTitle = "Add racks to group";
        o.dialogBackgroundColour = Theme::background();
        o.componentToCentreAround = this;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    // The "Groups..." button: pick racks for the open tab (or choose / create a group first).
    void groupsButtonClicked()
    {
        if (activeGroup >= 0)
        {
            showGroupPicker((juce::uint32) activeGroup);
            return;
        }

        juce::PopupMenu m;
        for (int i = 0; i < engine.getNumGroups(); ++i)
            m.addItem(100 + i, "Add racks to \"" + engine.getGroupName(i) + "\"...");
        if (engine.getNumGroups() > 0)
            m.addSeparator();
        m.addItem(1, "New group...");

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(groupsBtn), [safe](int r)
        {
            if (safe == nullptr || r <= 0) return;
            if (r == 1)
                safe->newGroup(-1, [safe](juce::uint32 id) { if (safe != nullptr) safe->showGroupPicker(id); });
            else if (r >= 100 && r - 100 < safe->engine.getNumGroups())
                safe->showGroupPicker(safe->engine.getGroupId(r - 100));
        });
    }

    void showGroupMenu(juce::uint32 id)
    {
        juce::PopupMenu m;
        m.addItem(3, "Add racks to this group...");
        m.addItem(1, "Rename group...");
        m.addItem(2, "Delete group (racks are kept)");

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe, id](int r)
        {
            if (safe == nullptr) return;
            if (r == 3)
                safe->showGroupPicker(id);
            else if (r == 1)
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

    struct ColourChoice { const char* name; juce::uint32 argb; };

    static std::vector<ColourChoice> colourChoices()
    {
        return { { "Automatic", 0 },           { "Red", 0xffff5c5c },    { "Orange", 0xffff9a3d },
                 { "Yellow", 0xffffd43b },     { "Green", 0xff3ddc84 },  { "Teal", 0xff22c7b8 },
                 { "Blue", 0xff4f8cff },       { "Purple", 0xff9b6bff }, { "Pink", 0xffff6bb5 },
                 { "White", 0xffe8ebf2 } };
    }

    // The racks currently selected in the list (as rack indices).
    std::vector<int> selectedRacks() const
    {
        std::vector<int> out;
        const auto rows = racksList.getSelectedRows();
        for (int i = 0; i < rows.size(); ++i)
        {
            const int row = rows[i];
            if (row >= 0 && row < (int) visibleRacks.size())
                out.push_back(visibleRacks[(size_t) row]);
        }
        return out;
    }

    // Right-click menu. If the clicked rack is part of a multi-selection the choices apply to all of them.
    void showRackMenu(int rack)
    {
        auto targets = selectedRacks();
        if (std::find(targets.begin(), targets.end(), rack) == targets.end())
            targets = { rack };
        const bool many = targets.size() > 1;

        juce::PopupMenu m;
        if (many)
            m.addSectionHeader(juce::String((int) targets.size()) + " racks selected");
        else
            m.addItem(1, "Rename rack...");

        juce::PopupMenu groupsMenu;
        const auto current = engine.getRackGroup(rack);
        groupsMenu.addItem(2, "No group", true, ! many && current == 0);
        for (int i = 0; i < engine.getNumGroups(); ++i)
            groupsMenu.addItem(100 + i, engine.getGroupName(i), true, ! many && engine.getGroupId(i) == current);
        groupsMenu.addSeparator();
        groupsMenu.addItem(3, "New group...");
        m.addSubMenu(many ? "Move all to group" : "Move to group", groupsMenu);

        const auto swatches = colourChoices();
        const auto currentColour = engine.getRackColour(rack);
        juce::PopupMenu colourMenu, groupColourMenu;
        for (int i = 0; i < (int) swatches.size(); ++i)
        {
            const auto& sw = swatches[(size_t) i];
            const auto text = juce::String(juce::CharPointer_UTF8("\xe2\x96\xa0  ")) + sw.name;
            if (sw.argb == 0)
            {
                colourMenu.addItem(200 + i, "Automatic", true, ! many && currentColour == 0);
                groupColourMenu.addItem(300 + i, "Automatic");
            }
            else
            {
                colourMenu.addColouredItem(200 + i, text, juce::Colour(sw.argb), true,
                                           ! many && currentColour == sw.argb);
                groupColourMenu.addColouredItem(300 + i, text, juce::Colour(sw.argb));
            }
        }
        m.addSubMenu(many ? "Color all" : "Color", colourMenu);
        if (! many && current != 0)
            m.addSubMenu("Color whole group", groupColourMenu);

        juce::Component::SafePointer<MainComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe, rack, current, targets](int r)
        {
            if (safe == nullptr || r <= 0) return;
            auto& eng = safe->engine;
            const auto choices = colourChoices();

            if (r >= 200 && r < 200 + (int) choices.size())
            {
                for (int t : targets) eng.setRackColour(t, choices[(size_t) (r - 200)].argb);
                return;
            }
            if (r >= 300 && r < 300 + (int) choices.size())
            {
                for (int i = 0; i < eng.getNumRacks(); ++i)
                    if (eng.getRackGroup(i) == current)
                        eng.setRackColour(i, choices[(size_t) (r - 300)].argb);
                return;
            }
            if (r == 1)
                safe->renameRack(rack);
            else if (r == 2)
                for (int t : targets) eng.setRackGroup(t, 0);
            else if (r == 3)
                safe->newGroup(-1, [safe, targets](juce::uint32 id)
                {
                    if (safe == nullptr) return;
                    for (int t : targets) safe->engine.setRackGroup(t, id);
                });
            else if (r >= 100 && r - 100 < eng.getNumGroups())
                for (int t : targets) eng.setRackGroup(t, eng.getGroupId(r - 100));
        });
    }

    // One tab per group plus "All" and "+" (rebuilt only when the list of groups changes).
    void rebuildTabs()
    {
        juce::String sig;
        for (int i = 0; i < engine.getNumGroups(); ++i)
            sig += juce::String(engine.getGroupId(i)) + ":" + engine.getGroupName(i) + "|";

        if (! tabsBuilt || sig != tabSignature)
        {
            tabsBuilt = true;
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

            layoutTabs();
        }

        for (int i = 0; i < tabButtons.size(); ++i)
            tabButtons[i]->setToggleState(tabIds[(size_t) i] == activeGroup, juce::dontSendNotification);
    }

    // Vertical stack of tabs, anchored to the bottom-left of the window.
    void layoutTabs()
    {
        const int w = juce::jmax(40, tabViewport.getWidth() - 8);
        const int h = 30, gap = 4;
        const int total = tabButtons.size() * (h + gap);
        const int contentH = juce::jmax(total, tabViewport.getHeight());
        tabContent.setSize(w + 8, contentH);

        int y = contentH - total;
        for (auto* b : tabButtons)
        {
            b->setBounds(0, y, w, h);
            y += h + gap;
        }
        tabViewport.setViewPosition(0, contentH);       // keep the bottom (newest tabs) in view
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
        tempoSlider.setValue(engine.getTempo(), juce::dontSendNotification);
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
        rackStripLabel.setColour(juce::Label::textColourId,
                                 engine.getRackColour(sel) != 0 ? juce::Colour(engine.getRackColour(sel)) : Theme::text());
        rackStripLabel.setTooltip("Rack " + juce::String(sel + 1) + ": " + engine.getRackName(sel));

        const auto keepSelected = selectedRacks();
        rebuildVisibleRacks();
        rebuildTabs();
        racksTitle.setText(activeGroup < 0 ? "RACKS  -  " + juce::String(engine.getNumRacks())
                                           : "RACKS  -  " + juce::String((int) visibleRacks.size()) + " of "
                                                 + juce::String(engine.getNumRacks()),
                           juce::dontSendNotification);
        racksList.updateContent();
        const auto it = std::find(visibleRacks.begin(), visibleRacks.end(), sel);
        if (keepSelected.size() > 1 && std::find(keepSelected.begin(), keepSelected.end(), sel) != keepSelected.end())
        {
            // keep a multi-selection alive across changes (e.g. after moving racks to a group)
            juce::SparseSet<int> rows;
            for (size_t row = 0; row < visibleRacks.size(); ++row)
                if (std::find(keepSelected.begin(), keepSelected.end(), visibleRacks[row]) != keepSelected.end())
                    rows.addRange({ (int) row, (int) row + 1 });
            racksList.setSelectedRows(rows, juce::dontSendNotification);
        }
        else if (it != visibleRacks.end())
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
        if (patchPanel.isVisible())
            patchPanel.refresh();

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
        if (splash != nullptr)
            splash->toFront(false);
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
                + "      CPU " + juce::String(cpu) + "% (peak " + juce::String(engine.getPeakLoadPercent()) + "%)"
                + "      Dropouts " + juce::String(engine.getDropouts());
        }
        else if (engine.isAudioDeviceLost())
        {
            s = "Audio device disconnected - waiting for it to come back (or open Audio settings)";
        }
        else
        {
            s = "No audio device - open Audio settings";
        }

        engine.pollDevices();

        if (checking > 0)
            s = "Checking " + checkingName + " in a separate process before loading it...";

        if (flashUntil != 0 && (juce::int32) (juce::Time::getMillisecondCounter() - flashUntil) < 0)
            s = flashText;

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

        auto& slot = engine.getSlot(row);
        inlineNode = slot.node->nodeID.uid;
        editorHost.setVisible(true);
        resized();                                  // makes room: the chain moves left, the plugin goes in the middle
        editorHost.show(*slot.node->getProcessor(), slot.desc.name, slot.desc.createIdentifierString());
        resized();
        ensureEditorFits();
        repaint();
    }

    int inlineRowIndex() const
    {
        for (int i = 0; i < engine.getNumSlots(); ++i)
            if (engine.getSlot(i).node->nodeID.uid == inlineNode)
                return i;
        return -1;
    }

    // A plugin always stays inside the client. If it is too big for the room, the racks column steps
    // aside to give it more; if it is still too big, the panel scrolls.
    void ensureEditorFits()
    {
        if (! editorHost.isVisible() || ! editorHost.tooBig())
            return;

        if (! editorCompact)
        {
            editorCompact = true;
            resized();
        }
    }

    // restoreSize: put the window back to its size from before the editor opened.
    void showPluginImageMenu(int row)
    {
        if (row < 0 || row >= engine.getNumSlots())
            return;
        const auto id = engine.getSlot(row).desc.createIdentifierString();
        const auto pname = engine.getSlot(row).desc.name;

        juce::PopupMenu m;
        m.addSectionHeader(pname);
        m.addItem(1, "Set plugin image...");
        m.addItem(2, "Remove plugin image", PluginImages::get(id).isValid());
        m.showMenuAsync({}, [this, id, pname](int r)
        {
            if (r == 1)
            {
                imageChooser = std::make_unique<juce::FileChooser>("Choose an image for " + pname, juce::File(),
                                                                   "*.png;*.jpg;*.jpeg;*.gif");
                imageChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                          [this, id](const juce::FileChooser& fc)
                                          {
                                              const auto f = fc.getResult();
                                              if (f.existsAsFile())
                                              {
                                                  if (! PluginImages::setFromFile(id, f) && engine.onMessage)
                                                      engine.onMessage("Could not read that image.");
                                                  rackList.repaint();
                                              }
                                          });
            }
            else if (r == 2)
            {
                PluginImages::clear(id);
                rackList.repaint();
            }
        });
    }

    void closeInlineEditor(bool restoreSize)
    {
        juce::ignoreUnused(restoreSize);
        editorHost.clear();
        editorHost.setVisible(false);
        inlineNode = 0;
        editorCompact = false;
        resized();
        repaint();
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

    //--------------------------------------------------------------------------
    // Shows library: .oshow files kept in the app's data folder.
    static juce::File getShowsDir()
    {
        auto d = getDataDir().getChildFile("Shows");
        d.createDirectory();
        return d;
    }

    static juce::Array<juce::File> listShows()
    {
        juce::Array<juce::File> out;
        for (auto& f : getShowsDir().findChildFiles(juce::File::findFiles, false, "*.oshow;*.orack"))
            out.add(f);
        struct ByName
        {
            static int compareElements(const juce::File& a, const juce::File& b)
            {
                return a.getFileNameWithoutExtension().compareNatural(b.getFileNameWithoutExtension());
            }
        };
        ByName sorter;
        out.sort(sorter);
        return out;
    }

    void updateShowTitle()
    {
        rackTitle.setTooltip(currentShow.isNotEmpty() ? "Show: " + currentShow : juce::String());
        loadBtn.setButtonText(showsPanel.isVisible() ? "Rack" : "Shows");
        if (showsPanel.isVisible())
            showsPanel.setCurrent(currentShow);
    }

    void saveShowTo(const juce::File& f, bool inLibrary)
    {
        if (engine.saveRack(f))
        {
            if (inLibrary)
            {
                currentShow = f.getFileNameWithoutExtension();
                updateShowTitle();
            }
            showsPanel.refresh();
        }
        else
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Save show",
                                                   "Could not write " + f.getFullPathName());
    }

    void saveShowAs()
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        promptText("Save show", "Name for this show:", currentShow, [safe](const juce::String& typed)
        {
            if (safe == nullptr) return;
            const auto name = juce::File::createLegalFileName(typed.trim());
            if (name.isEmpty()) return;

            const auto f = getShowsDir().getChildFile(name + ".oshow");
            if (! f.existsAsFile())
            {
                safe->saveShowTo(f, true);
                return;
            }

            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle("Replace show?")
                    .withMessage("A show called \"" + name + "\" already exists. Replace it?")
                    .withButton("Replace").withButton("Cancel"),
                [safe, f](int r)
                {
                    if (r == 1 && safe != nullptr) safe->saveShowTo(f, true);
                });
        });
    }

    void openShow(const juce::File& f, bool inLibrary)
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        engine.loadRack(f, [safe, f, inLibrary](const juce::String& err)
        {
            if (safe == nullptr) return;
            safe->syncFaders();
            if (err.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Open show", err);
                return;
            }
            safe->currentShow = inLibrary ? f.getFileNameWithoutExtension() : juce::String();
            if (safe->showsPanel.isVisible())
                safe->toggleShows();
            else
                safe->updateShowTitle();
        });
    }

    void saveCurrentShow()
    {
        if (currentShow.isNotEmpty())
            saveShowTo(getShowsDir().getChildFile(currentShow + ".oshow"), true);
        else
            saveShowAs();
    }

    void renameShow(const juce::File& f)
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        promptText("Rename show", "New name:", f.getFileNameWithoutExtension(),
                   [safe, f](const juce::String& typed)
                   {
                       if (safe == nullptr) return;
                       const auto name = juce::File::createLegalFileName(typed.trim());
                       if (name.isEmpty()) return;
                       const auto dest = f.getParentDirectory().getChildFile(name + ".oshow");
                       if (dest != f && dest.existsAsFile())
                       {
                           juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                  "Rename show", "A show with that name already exists.");
                           return;
                       }
                       if (f.moveFileTo(dest))
                       {
                           if (safe->currentShow == f.getFileNameWithoutExtension())
                               safe->currentShow = name;
                           safe->updateShowTitle();
                           safe->showsPanel.setCurrent(safe->currentShow);
                       }
                   });
    }

    void deleteShow(const juce::File& f)
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Delete show?")
                .withMessage("Delete \"" + f.getFileNameWithoutExtension() + "\" permanently?")
                .withButton("Delete").withButton("Cancel"),
            [safe, f](int b)
            {
                if (b != 1 || safe == nullptr) return;
                f.deleteFile();
                if (safe->currentShow == f.getFileNameWithoutExtension())
                    safe->currentShow = {};
                safe->updateShowTitle();
                safe->showsPanel.setCurrent(safe->currentShow);
            });
    }

    void importShow()
    {
        chooser = std::make_unique<juce::FileChooser>("Import show", juce::File(), "*.oshow;*.orack");
        juce::Component::SafePointer<MainComponent> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe](const juce::FileChooser& fc)
                             {
                                 auto f = fc.getResult();
                                 if (safe == nullptr || f == juce::File()) return;
                                 auto dest = getShowsDir().getNonexistentChildFile(f.getFileNameWithoutExtension(),
                                                                                   ".oshow", false);
                                 f.copyFileTo(dest);
                                 safe->showsPanel.refresh();
                             });
    }

    void exportShow()
    {
        chooser = std::make_unique<juce::FileChooser>("Export show", juce::File(), "*.oshow");
        juce::Component::SafePointer<MainComponent> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe](const juce::FileChooser& fc)
                             {
                                 auto f = fc.getResult();
                                 if (safe != nullptr && f != juce::File())
                                     safe->saveShowTo(f.withFileExtension("oshow"), false);
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
    juce::String currentShow;
    bool tabsBuilt = false;
    juce::TextEditor searchBox;
    juce::ComboBox categoryBox;
    juce::String categoryFilter;
    juce::TextButton scanBtn, audioBtn, hotBtn, loadBtn, bypassAllBtn, clearBtn, inputsBtn, addRackBtn, removeRackBtn, eqBtn, patchBtn, groupsBtn, midiBtn;
    std::array<SnapButton, kNumSnapshots> snapBtns;
    juce::Label snapLabel;
    EqPanel eqPanel { engine };
    InputsPanel inputsPanel { engine };
    ShowsPanel showsPanel { getShowsDir() };
    PatchPanel patchPanel { engine };
    MidiPanel midiPanel { engine.midi };
    MidiActivityLed midiLed { engine.midi };
    ParamFaderBar paramBar { engine };
    std::unique_ptr<SplashOverlay> splash;
    juce::Slider tempoSlider;
    juce::Label tempoLabel;
    juce::TextButton tapBtn;
    int checking = 0;
    juce::String checkingName;
    juce::File lockFile = getDataDir().getChildFile("session.lock");
    juce::File autosaveFile = getDataDir().getChildFile("autosave.oshow");
    bool autosaveArmed = false, autosaveDirty = false;
    juce::Slider rackFader, masterFader;
    int stripDividerX = 0;
    juce::Label rackStripLabel, masterLabel, pluginTitle, rackTitle, racksTitle, inLabel, outLabel;
    juce::ComboBox inCombo, outCombo;
    juce::String patchSignature;
    juce::Rectangle<int> pluginPanel, rackPanel, racksPanel, mixerPanel, statusBar, hotStrip;
    juce::String statusText;
    juce::String flashText;
    juce::uint32 flashUntil = 0;
    juce::ToggleButton isolateBtn;
    std::vector<std::unique_ptr<HotChip>> hotChips;
    juce::String hotSignature;
    int dropIndex = -1;
    bool fileDropActive = false;
    InlineEditorHost editorHost;
    std::unique_ptr<juce::FileChooser> imageChooser;
    juce::uint32 inlineNode = 0;
    bool editorCompact = false;
    juce::File pluginListFile, deadMansFile;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltipWindow { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
