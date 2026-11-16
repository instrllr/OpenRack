#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// Colour palette used across the app.
namespace Theme
{
    inline juce::Colour background() { return juce::Colour(0xff111318); }
    inline juce::Colour panel()      { return juce::Colour(0xff1a1d24); }
    inline juce::Colour row()        { return juce::Colour(0xff242833); }
    inline juce::Colour control()    { return juce::Colour(0xff2b303c); }
    inline juce::Colour outline()    { return juce::Colour(0xff333948); }
    inline juce::Colour text()       { return juce::Colour(0xffe8ebf2); }
    inline juce::Colour muted()      { return juce::Colour(0xff8a92a6); }
    inline juce::Colour accent()     { return juce::Colour(0xff4f8cff); }
    inline juce::Colour warning()    { return juce::Colour(0xffffa94d); }

    // A stable colour per plugin name, so a plugin looks the same everywhere.
    inline juce::Colour tint(const juce::String& s)
    {
        const float hue = (float) (s.hashCode() & 0xffff) / 65535.0f;
        return juce::Colour::fromHSV(hue, 0.55f, 0.95f, 1.0f);
    }
}

//==============================================================================
// Flat, rounded dark look for the whole app (also applies to dialogs).
class OpenRackLookAndFeel : public juce::LookAndFeel_V4
{
public:
    OpenRackLookAndFeel()
    {
        setColourScheme(juce::LookAndFeel_V4::ColourScheme(
            Theme::background(), Theme::panel(), Theme::panel(), Theme::outline(),
            Theme::text(), Theme::accent(), juce::Colours::white, Theme::accent(),
            Theme::text()));

        setColour(juce::TextButton::buttonColourId, Theme::control());
        setColour(juce::TextButton::buttonOnColourId, Theme::warning());
        setColour(juce::TextButton::textColourOffId, Theme::text());
        setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1a1200));

        setColour(juce::TextEditor::backgroundColourId, Theme::control());
        setColour(juce::TextEditor::outlineColourId, Theme::outline());
        setColour(juce::TextEditor::focusedOutlineColourId, Theme::accent());
        setColour(juce::TextEditor::textColourId, Theme::text());
        setColour(juce::CaretComponent::caretColourId, Theme::accent());

        setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);

        setColour(juce::Slider::trackColourId, Theme::accent());
        setColour(juce::Slider::backgroundColourId, Theme::control());
        setColour(juce::Slider::thumbColourId, Theme::text());
        setColour(juce::Slider::textBoxTextColourId, Theme::text());
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

        setColour(juce::Label::textColourId, Theme::text());

        // Controls used by the audio settings and plugin scanner dialogs
        setColour(juce::ResizableWindow::backgroundColourId, Theme::background());
        setColour(juce::ListBox::textColourId, Theme::text());
        setColour(juce::ComboBox::backgroundColourId, Theme::control());
        setColour(juce::ComboBox::textColourId, Theme::text());
        setColour(juce::ComboBox::outlineColourId, Theme::outline());
        setColour(juce::ComboBox::arrowColourId, Theme::muted());
        setColour(juce::PopupMenu::backgroundColourId, Theme::panel());
        setColour(juce::PopupMenu::textColourId, Theme::text());
        setColour(juce::PopupMenu::highlightedBackgroundColourId, Theme::accent());
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour(juce::ToggleButton::textColourId, Theme::text());
        setColour(juce::ToggleButton::tickColourId, Theme::accent());
        setColour(juce::ToggleButton::tickDisabledColourId, Theme::muted());
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);

        auto fill = backgroundColour;
        if (button.isEnabled())
        {
            if (shouldDrawButtonAsDown)             fill = fill.brighter(0.2f);
            else if (shouldDrawButtonAsHighlighted) fill = fill.brighter(0.1f);
        }
        else
        {
            fill = fill.withAlpha(0.5f);
        }

        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(Theme::outline());
        g.drawRoundedRectangle(bounds, 6.0f, 1.0f);
    }
};
