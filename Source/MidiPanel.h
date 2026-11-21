#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiControl.h"
#include "Theme.h"

//==============================================================================
// Full-size MIDI control view: devices, strips and banks, MIDI learn for every control.
class MidiPanel : public juce::Component, private juce::ListBoxModel
{
public:
    explicit MidiPanel(MidiController& c) : midi(c)
    {
        list.setModel(this);
        list.setRowHeight(34);
        addAndMakeVisible(list);

        for (auto* b : { &inBox, &outBox, &stripsBox })
            addAndMakeVisible(b);
        for (auto* b : { &pickupBtn, &faderFbBtn, &ledFbBtn })
        {
            b->setColour(juce::ToggleButton::textColourId, Theme::text());
            addAndMakeVisible(b);
        }
        for (auto* b : { &rescanBtn, &bankPrev, &bankNext, &mapFaders, &mapMutes, &mapSolos, &mapSnaps,
                         &mackieBtn, &clearBtn, &stopBtn })
            addAndMakeVisible(b);
        addAndMakeVisible(bankLabel);
        bankLabel.setJustificationType(juce::Justification::centred);

        pickupBtn.setButtonText("Soft takeover");
        pickupBtn.setTooltip("Faders only take over once they reach the current level, so a bank change never makes a level jump");
        faderFbBtn.setButtonText("Move motorized faders");
        faderFbBtn.setTooltip("Send fader positions back to the controller (motorized faders)");
        ledFbBtn.setButtonText("Light button LEDs");
        ledFbBtn.setTooltip("Send mute / solo / snapshot state back to the controller's button LEDs");
        rescanBtn.setButtonText("Rescan");
        bankPrev.setButtonText("<");
        bankNext.setButtonText(">");
        bankPrev.setTooltip("Previous bank of racks");
        bankNext.setTooltip("Next bank of racks");
        mapFaders.setButtonText("Learn faders 1-N");
        mapMutes.setButtonText("Learn mutes 1-N");
        mapSolos.setButtonText("Learn solos 1-N");
        mapSnaps.setButtonText("Learn snapshots");
        mackieBtn.setButtonText("Mackie Control preset");
        clearBtn.setButtonText("Clear all");
        stopBtn.setButtonText("Stop learning");
        mapFaders.setTooltip("Move fader 1, then 2, then 3... each one is assigned to the next strip");
        mapMutes.setTooltip("Press the mute button of strip 1, then 2, then 3...");
        mapSolos.setTooltip("Press the solo button of strip 1, then 2, then 3...");
        mapSnaps.setTooltip("Press 8 buttons to recall snapshots 1-8");
        mackieBtn.setTooltip("Standard layout for Mackie Control compatible surfaces (X-Touch, FaderPort in MCU mode, ...)");

        for (auto* b : { &mapFaders, &mapMutes, &mapSolos, &mapSnaps })
            b->setColour(juce::TextButton::buttonColourId, Theme::control());

        inBox.onChange = [this]
        {
            const auto devs = MidiController::availableInputs();
            const int i = inBox.getSelectedId() - 2;
            if (i >= 0 && i < devs.size()) midi.setInput(devs[i]);
            else                           midi.setInput({});
        };
        outBox.onChange = [this]
        {
            const auto devs = MidiController::availableOutputs();
            const int i = outBox.getSelectedId() - 2;
            if (i >= 0 && i < devs.size()) midi.setOutput(devs[i]);
            else                           midi.setOutput({});
        };

        for (int n : { 4, 8, 12, 16, 24, 32, 48, 64 })
            stripsBox.addItem(juce::String(n) + " strips", n);
        stripsBox.onChange = [this] { midi.setStripCount(stripsBox.getSelectedId()); };
        stripsBox.setTooltip("How many faders your controller has. Bigger boards: pick 16, 24, 32... and use the bank buttons for more racks");

        pickupBtn.onClick = [this] { midi.setPickup(pickupBtn.getToggleState()); };
        faderFbBtn.onClick = [this] { midi.setFaderFeedback(faderFbBtn.getToggleState()); };
        ledFbBtn.onClick = [this] { midi.setLedFeedback(ledFbBtn.getToggleState()); };
        rescanBtn.onClick = [this] { refreshDevices(); };
        bankPrev.onClick = [this] { midi.setBank(midi.getBank() - 1); };
        bankNext.onClick = [this] { midi.setBank(midi.getBank() + 1); };
        mapFaders.onClick = [this] { midi.startLearn(MidiController::Kind::fader, midi.getStripCount()); };
        mapMutes.onClick = [this] { midi.startLearn(MidiController::Kind::mute, midi.getStripCount()); };
        mapSolos.onClick = [this] { midi.startLearn(MidiController::Kind::solo, midi.getStripCount()); };
        mapSnaps.onClick = [this] { midi.startLearn(MidiController::Kind::snapshot, 8); };
        mackieBtn.onClick = [this] { midi.applyMackiePreset(); };
        clearBtn.onClick = [this] { midi.clearAll(); };
        stopBtn.onClick = [this] { midi.stopLearn(); };

        refreshDevices();
        refresh();
    }

    void refreshDevices()
    {
        auto fill = [](juce::ComboBox& box, const juce::Array<juce::MidiDeviceInfo>& devs, const juce::String& current)
        {
            box.clear(juce::dontSendNotification);
            box.addItem("None", 1);
            int sel = 1;
            for (int i = 0; i < devs.size(); ++i)
            {
                box.addItem(devs[i].name, 2 + i);
                if (devs[i].name == current) sel = 2 + i;
            }
            box.setSelectedId(sel, juce::dontSendNotification);
        };
        fill(inBox, MidiController::availableInputs(), midi.getInputName());
        fill(outBox, MidiController::availableOutputs(), midi.getOutputName());
    }

    // Called whenever the controller's state changes.
    void refresh()
    {
        stripsBox.setSelectedId(midi.getStripCount(), juce::dontSendNotification);
        if (stripsBox.getSelectedId() == 0)
            stripsBox.setText(juce::String(midi.getStripCount()) + " strips", juce::dontSendNotification);
        pickupBtn.setToggleState(midi.getPickup(), juce::dontSendNotification);
        faderFbBtn.setToggleState(midi.getFaderFeedback(), juce::dontSendNotification);
        ledFbBtn.setToggleState(midi.getLedFeedback(), juce::dontSendNotification);

        const int first = midi.getBank() * midi.getStripCount() + 1;
        bankLabel.setText("Bank " + juce::String(midi.getBank() + 1) + " of " + juce::String(midi.getNumBanks())
                              + "  (racks " + juce::String(first) + "-" + juce::String(first + midi.getStripCount() - 1) + ")",
                          juce::dontSendNotification);
        bankPrev.setEnabled(midi.getBank() > 0);
        bankNext.setEnabled(midi.getBank() < midi.getNumBanks() - 1);
        stopBtn.setEnabled(midi.isLearning());

        rebuildRows();
        list.updateContent();
        list.repaint();
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
        g.drawText("MIDI CONTROL  -  faders, mute / solo, master, banks and snapshots from a hardware controller",
                   16, 8, getWidth() - 32, 20, juce::Justification::centredLeft);

        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Input", inBox.getX(), inBox.getY() - 17, 80, 16, juce::Justification::centredLeft);
        g.drawText("Output (LEDs / motor faders)", outBox.getX(), outBox.getY() - 17, 200, 16, juce::Justification::centredLeft);
        g.drawText("Strips", stripsBox.getX(), stripsBox.getY() - 17, 80, 16, juce::Justification::centredLeft);

        // status line
        juce::String status;
        if (midi.isLearning())
            status = "LEARNING " + juce::String(midi.learnDone() + 1) + " of " + juce::String(midi.learnTotal())
                     + " - move or press the control on your device now";
        else
            status = midi.isInputOpen() ? "Last message: " + (midi.getLastMessage().isEmpty() ? juce::String("(none yet)")
                                                                                             : midi.getLastMessage())
                                        : "Choose your controller as the MIDI input to begin";
        g.setColour(midi.isLearning() ? Theme::accent() : Theme::muted());
        g.setFont(juce::FontOptions(13.0f, midi.isLearning() ? juce::Font::bold : juce::Font::plain));
        g.drawText(status, statusArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14, 8);
        r.removeFromTop(26);

        auto row1 = r.removeFromTop(48).withTrimmedTop(18);
        inBox.setBounds(row1.removeFromLeft(260));
        row1.removeFromLeft(10);
        outBox.setBounds(row1.removeFromLeft(260));
        row1.removeFromLeft(10);
        rescanBtn.setBounds(row1.removeFromLeft(80));
        row1.removeFromLeft(20);
        stripsBox.setBounds(row1.removeFromLeft(120));

        r.removeFromTop(6);
        auto row2 = r.removeFromTop(30);
        pickupBtn.setBounds(row2.removeFromLeft(150));
        faderFbBtn.setBounds(row2.removeFromLeft(190));
        ledFbBtn.setBounds(row2.removeFromLeft(160));
        auto bank = row2.removeFromRight(330);
        bankPrev.setBounds(bank.removeFromLeft(34));
        bankNext.setBounds(bank.removeFromRight(34));
        bankLabel.setBounds(bank);

        r.removeFromTop(4);
        statusArea = r.removeFromTop(24);
        r.removeFromTop(4);

        auto side = r.removeFromRight(190);
        r.removeFromRight(12);
        list.setBounds(r);

        int y = side.getY();
        auto place = [&](juce::Component& c, int gap)
        {
            c.setBounds(side.getX(), y, side.getWidth(), 32);
            y += 32 + gap;
        };
        place(mapFaders, 6);
        place(mapMutes, 6);
        place(mapSolos, 6);
        place(mapSnaps, 18);
        place(mackieBtn, 18);
        place(stopBtn, 6);
        place(clearBtn, 0);
    }

private:
    struct Row
    {
        bool header = false;
        juce::String text;
        MidiController::Action act;
    };

    void rebuildRows()
    {
        using K = MidiController::Kind;
        rows.clear();
        auto header = [this](const juce::String& t) { rows.push_back({ true, t, {} }); };
        auto add = [this](K k, int i) { rows.push_back({ false, {}, { k, i } }); };

        header("MASTER AND BANKS");
        add(K::master, 0);
        add(K::bankDown, 0);
        add(K::bankUp, 0);

        const int n = midi.getStripCount();
        header("FADERS");
        for (int i = 0; i < n; ++i) add(K::fader, i);
        header("MUTE BUTTONS");
        for (int i = 0; i < n; ++i) add(K::mute, i);
        header("SOLO BUTTONS");
        for (int i = 0; i < n; ++i) add(K::solo, i);
        header("SNAPSHOT BUTTONS");
        for (int i = 0; i < 8; ++i) add(K::snapshot, i);
    }

    juce::String labelFor(const MidiController::Action& a) const
    {
        using K = MidiController::Kind;
        switch (a.kind)
        {
            case K::master:   return "Master fader";
            case K::bankDown: return "Bank down  (previous racks)";
            case K::bankUp:   return "Bank up  (next racks)";
            case K::snapshot: return "Snapshot " + juce::String(a.index + 1);
            default: break;
        }
        const auto what = a.kind == K::fader ? "Fader " : a.kind == K::mute ? "Mute " : "Solo ";
        const int rack = midi.getBank() * midi.getStripCount() + a.index + 1;
        return juce::String(what) + juce::String(a.index + 1) + "   >  rack " + juce::String(rack);
    }

    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool) override
    {
        if (row >= (int) rows.size()) return;
        const auto& r = rows[(size_t) row];

        if (r.header)
        {
            g.setColour(Theme::muted());
            g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
            g.drawText(r.text, 6, h - 20, w - 12, 18, juce::Justification::centredLeft);
            return;
        }

        const bool learning = midi.isLearning(r.act);
        auto card = juce::Rectangle<float>(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f);
        g.setColour(learning ? Theme::accent().withAlpha(0.22f) : Theme::row());
        g.fillRoundedRectangle(card, 7.0f);
        if (learning)
        {
            g.setColour(Theme::accent());
            g.drawRoundedRectangle(card.reduced(0.5f), 7.0f, 1.5f);
        }

        g.setColour(Theme::text());
        g.setFont(juce::FontOptions(13.5f));
        g.drawText(labelFor(r.act), 14, 0, w / 2 - 30, h, juce::Justification::centredLeft, true);

        const auto* b = midi.findBinding(r.act);
        g.setFont(juce::FontOptions(13.0f, learning ? juce::Font::bold : juce::Font::plain));
        g.setColour(learning ? Theme::accent() : (b != nullptr ? Theme::text() : Theme::muted()));
        g.drawText(learning ? "Waiting for a control..." : (b != nullptr ? MidiController::describe(b->src) : juce::String("not assigned")),
                   w / 2 - 10, 0, w / 2 - 190, h, juce::Justification::centredLeft, true);

        auto drawBtn = [&](juce::Rectangle<int> rc, const juce::String& t, bool on)
        {
            g.setColour(on ? Theme::control() : Theme::control().withAlpha(0.4f));
            g.fillRoundedRectangle(rc.toFloat(), 5.0f);
            g.setColour(on ? Theme::text() : Theme::muted().withAlpha(0.6f));
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(t, rc, juce::Justification::centred);
        };
        drawBtn(learnRect(w, h), learning ? "Cancel" : "Learn", true);
        drawBtn(clearRect(w, h), "Clear", b != nullptr);
    }

    static juce::Rectangle<int> learnRect(int w, int h) { return { w - 168, 5, 76, h - 10 }; }
    static juce::Rectangle<int> clearRect(int w, int h) { return { w - 86, 5, 76, h - 10 }; }

    void listBoxItemClicked(int row, const juce::MouseEvent& e) override
    {
        if (row < 0 || row >= (int) rows.size() || rows[(size_t) row].header) return;
        const auto act = rows[(size_t) row].act;
        const int w = list.getVisibleRowWidth(), h = list.getRowHeight();

        if (learnRect(w, h).contains(e.getPosition()))
        {
            if (midi.isLearning(act)) midi.stopLearn();
            else                      midi.startLearnOne(act);
        }
        else if (clearRect(w, h).contains(e.getPosition()))
            midi.clearBinding(act);
    }

    MidiController& midi;
    std::vector<Row> rows;
    juce::Rectangle<int> statusArea;
    juce::ListBox list;
    juce::ComboBox inBox, outBox, stripsBox;
    juce::ToggleButton pickupBtn, faderFbBtn, ledFbBtn;
    juce::Label bankLabel;
    juce::TextButton rescanBtn, bankPrev, bankNext, mapFaders, mapMutes, mapSolos, mapSnaps, mackieBtn, clearBtn, stopBtn;
};
