#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

//==============================================================================
// What the MIDI controller needs from the rack engine.
struct MidiHost
{
    virtual ~MidiHost() = default;
    virtual int midiNumRacks() const = 0;
    virtual float midiGetFader(int rack) const = 0;         // linear gain
    virtual void midiSetFader(int rack, float gain) = 0;
    virtual bool midiGetMute(int rack) const = 0;
    virtual void midiSetMute(int rack, bool on) = 0;
    virtual bool midiGetSolo(int rack) const = 0;
    virtual void midiSetSolo(int rack, bool on) = 0;
    virtual float midiGetMaster() const = 0;
    virtual void midiSetMaster(float gain) = 0;
    virtual bool midiHasSnapshot(int index) const = 0;
    virtual int midiCurrentSnapshot() const = 0;
    virtual void midiRecallSnapshot(int index) = 0;
};

//==============================================================================
// Hardware control: MIDI learn, strips (faders / mute / solo) with banks, master,
// snapshot buttons and LED / motor-fader feedback. Works with any controller that
// sends CC, notes or pitch-bend (AKAI MIDImix, Mackie Control style surfaces, ...).
class MidiController : private juce::MidiInputCallback,
                       private juce::AsyncUpdater,
                       private juce::Timer
{
public:
    enum class Kind { fader = 0, mute = 1, solo = 2, master = 3, bankDown = 4, bankUp = 5, snapshot = 6 };
    enum class SrcType { cc = 0, note = 1, pitchBend = 2 };

    struct Action
    {
        Kind kind = Kind::fader;
        int index = 0;      // strip number (0-based) or snapshot number
        bool operator==(const Action& o) const { return kind == o.kind && index == o.index; }
    };

    struct Source
    {
        SrcType type = SrcType::cc;
        int channel = 1;    // 1..16
        int number = 0;     // CC or note number (0 for pitch-bend)
    };

    struct Binding
    {
        Source src;
        Action act;
    };

    static constexpr int kMaxStrips = 64;

    MidiController(MidiHost& h, juce::File settingsFile) : host(h), file(std::move(settingsFile)) {}
    ~MidiController() override { stop(); }

    std::function<void()> onChanged;    // bindings / devices / learn state / last message changed (UI)
    std::function<void()> onApplied;    // a rack value was changed by MIDI (UI should resync, mark dirty)

    //--------------------------------------------------------------------------
    void start()
    {
        load();
        openInput();
        openOutput();
        startTimerHz(20);
    }

    void stop()
    {
        stopTimer();
        cancelPendingUpdate();
        input.reset();
        output.reset();
    }

    //--------------------------------------------------------------------------
    // Devices
    static juce::Array<juce::MidiDeviceInfo> availableInputs() { return juce::MidiInput::getAvailableDevices(); }
    static juce::Array<juce::MidiDeviceInfo> availableOutputs() { return juce::MidiOutput::getAvailableDevices(); }

    juce::String getInputName() const { return inputName; }
    juce::String getOutputName() const { return outputName; }
    bool isInputOpen() const { return input != nullptr; }
    bool isOutputOpen() const { return output != nullptr; }

    void setInput(const juce::MidiDeviceInfo& d)
    {
        inputId = d.identifier;
        inputName = d.name;
        openInput();
        save();
        changed();
    }

    void setOutput(const juce::MidiDeviceInfo& d)
    {
        outputId = d.identifier;
        outputName = d.name;
        openOutput();
        save();
        changed();
    }

    //--------------------------------------------------------------------------
    // Settings
    int getStripCount() const { return stripCount; }

    void setStripCount(int n)
    {
        stripCount = juce::jlimit(1, kMaxStrips, n);
        setBank(bank);
        resetPickup();
        invalidateFeedback();
        save();
        changed();
    }

    bool getPickup() const { return pickup; }
    bool getFaderFeedback() const { return faderFeedback; }
    bool getLedFeedback() const { return ledFeedback; }

    void setPickup(bool b) { pickup = b; resetPickup(); save(); changed(); }
    void setFaderFeedback(bool b) { faderFeedback = b; invalidateFeedback(); save(); changed(); }
    void setLedFeedback(bool b) { ledFeedback = b; invalidateFeedback(); save(); changed(); }

    //--------------------------------------------------------------------------
    // Banks: strip k of bank b controls rack b * stripCount + k.
    int getBank() const { return bank; }
    int getNumBanks() const { return juce::jmax(1, (host.midiNumRacks() + stripCount - 1) / stripCount); }
    int rackFor(int strip) const
    {
        const int r = bank * stripCount + strip;
        return r >= 0 && r < host.midiNumRacks() ? r : -1;
    }
    bool isRackInBank(int rack) const { return hasAnyBinding() && rack >= bank * stripCount && rack < (bank + 1) * stripCount; }
    bool hasAnyBinding() const { return ! bindings.empty(); }

    void setBank(int b)
    {
        const int newBank = juce::jlimit(0, getNumBanks() - 1, b);
        const bool moved = newBank != bank;
        bank = newBank;
        if (moved)
        {
            resetPickup();
            invalidateFeedback();
        }
        changed();
        if (moved && onApplied) onApplied();
    }

    //--------------------------------------------------------------------------
    // Bindings / learn
    const std::vector<Binding>& getBindings() const { return bindings; }

    const Binding* findBinding(const Action& a) const
    {
        for (auto& b : bindings)
            if (b.act == a) return &b;
        return nullptr;
    }

    static juce::String describe(const Source& s)
    {
        switch (s.type)
        {
            case SrcType::cc:        return "CC " + juce::String(s.number) + "  ch " + juce::String(s.channel);
            case SrcType::note:      return "Note " + juce::String(s.number) + "  ch " + juce::String(s.channel);
            case SrcType::pitchBend: return "Pitch-bend  ch " + juce::String(s.channel);
        }
        return {};
    }

    void startLearn(Kind kind, int count)
    {
        learnQueue.clear();
        learned.clear();
        for (int i = 0; i < count; ++i)
            learnQueue.push_back({ kind, i });
        totalToLearn = count;
        changed();
    }

    void startLearnOne(const Action& a)
    {
        learnQueue.clear();
        learned.clear();
        learnQueue.push_back(a);
        totalToLearn = 1;
        changed();
    }

    void stopLearn() { learnQueue.clear(); changed(); }
    bool isLearning() const { return ! learnQueue.empty(); }
    bool isLearning(const Action& a) const { return ! learnQueue.empty() && learnQueue.front() == a; }
    int learnDone() const { return totalToLearn - (int) learnQueue.size(); }
    int learnTotal() const { return totalToLearn; }

    void clearBinding(const Action& a)
    {
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                      [&](const Binding& b) { return b.act == a; }),
                       bindings.end());
        invalidateFeedback();
        save();
        changed();
    }

    void clearAll()
    {
        bindings.clear();
        learnQueue.clear();
        invalidateFeedback();
        save();
        changed();
    }

    // Mackie Control layout (also used by X-Touch, FaderPort 8/16 in MCU mode, ...):
    // pitch-bend faders on channels 1-8 (master on 9), notes for solo / mute, bank buttons.
    void applyMackiePreset()
    {
        bindings.clear();
        stripCount = 8;
        for (int i = 0; i < 8; ++i)
        {
            bindings.push_back({ { SrcType::pitchBend, i + 1, 0 }, { Kind::fader, i } });
            bindings.push_back({ { SrcType::note, 1, 8 + i }, { Kind::solo, i } });
            bindings.push_back({ { SrcType::note, 1, 16 + i }, { Kind::mute, i } });
        }
        bindings.push_back({ { SrcType::pitchBend, 9, 0 }, { Kind::master, 0 } });
        bindings.push_back({ { SrcType::note, 1, 46 }, { Kind::bankDown, 0 } });
        bindings.push_back({ { SrcType::note, 1, 47 }, { Kind::bankUp, 0 } });
        pickup = false;
        faderFeedback = true;
        ledFeedback = true;
        setBank(0);
        resetPickup();
        invalidateFeedback();
        save();
        changed();
    }

    juce::String getLastMessage() const { return lastMessage; }

private:
    //--------------------------------------------------------------------------
    static float normToGain(float n)
    {
        n = juce::jlimit(0.0f, 1.0f, n);
        if (n < 0.002f) return 0.0f;
        return juce::Decibels::decibelsToGain(-60.0f + 72.0f * n, -60.0f);     // same -60..+12 dB range as the UI faders
    }

    static float gainToNorm(float g)
    {
        return juce::jlimit(0.0f, 1.0f, (juce::Decibels::gainToDecibels(g, -60.0f) + 60.0f) / 72.0f);
    }

    static int sourceKey(const Source& s) { return (int) s.type * 100000 + s.channel * 1000 + s.number; }
    static int actionKey(const Action& a) { return (int) a.kind * 1000 + a.index; }
    static bool sameSource(const Source& a, const Source& b)
    {
        return a.type == b.type && a.channel == b.channel && a.number == b.number;
    }

    void changed() { if (onChanged) onChanged(); }

    void resetPickup() { pickupState.clear(); }
    void invalidateFeedback() { sent.clear(); }

    //--------------------------------------------------------------------------
    void openInput()
    {
        input.reset();
        if (inputId.isEmpty()) return;

        auto id = inputId;
        auto devices = availableInputs();
        bool found = false;
        for (auto& d : devices)
            if (d.identifier == id) found = true;
        if (! found)
            for (auto& d : devices)
                if (d.name == inputName) { id = d.identifier; found = true; break; }
        if (! found) return;

        input = juce::MidiInput::openDevice(id, this);
        if (input != nullptr)
            input->start();
    }

    void openOutput()
    {
        output.reset();
        invalidateFeedback();
        if (outputId.isEmpty()) return;

        auto id = outputId;
        auto devices = availableOutputs();
        bool found = false;
        for (auto& d : devices)
            if (d.identifier == id) found = true;
        if (! found)
            for (auto& d : devices)
                if (d.name == outputName) { id = d.identifier; found = true; break; }
        if (! found) return;

        output = juce::MidiOutput::openDevice(id);
    }

    //--------------------------------------------------------------------------
    // MIDI thread: just queue the message for the message thread.
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override
    {
        if (! (m.isController() || m.isNoteOnOrOff() || m.isPitchWheel()))
            return;
        {
            const juce::ScopedLock sl(lock);
            if (queue.size() < 2048)
                queue.push_back(m);
        }
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        std::vector<juce::MidiMessage> batch;
        {
            const juce::ScopedLock sl(lock);
            batch.swap(queue);
        }

        bool applied = false;
        for (auto& m : batch)
            applied = handleMessage(m) || applied;

        changed();
        if (applied && onApplied)
            onApplied();
    }

    // Returns true if a rack value changed.
    bool handleMessage(const juce::MidiMessage& m)
    {
        Source s;
        float value = 0.0f;
        bool pressed = false;
        s.channel = m.getChannel();

        if (m.isController())
        {
            s.type = SrcType::cc;
            s.number = m.getControllerNumber();
            value = (float) m.getControllerValue() / 127.0f;
            const bool down = value >= 0.5f;
            pressed = down && ! ccDown[sourceKey(s)];
            ccDown[sourceKey(s)] = down;
        }
        else if (m.isNoteOn())
        {
            s.type = SrcType::note;
            s.number = m.getNoteNumber();
            value = m.getFloatVelocity();
            pressed = true;
        }
        else if (m.isNoteOff())
        {
            s.type = SrcType::note;
            s.number = m.getNoteNumber();
        }
        else if (m.isPitchWheel())
        {
            s.type = SrcType::pitchBend;
            s.number = 0;
            value = (float) m.getPitchWheelValue() / 16383.0f;
        }
        else
            return false;

        lastMessage = describe(s) + "   value " + juce::String(juce::roundToInt(value * 127.0f));

        if (! learnQueue.empty())
        {
            learnFrom(s, pressed);
            return false;
        }

        for (auto& b : bindings)
            if (sameSource(b.src, s))
                return dispatch(b.act, value, pressed);

        return false;
    }

    void learnFrom(const Source& s, bool pressed)
    {
        if (learned.count(sourceKey(s)) > 0)       // still the control we just assigned (a moving fader)
            return;

        const auto act = learnQueue.front();
        const bool analog = act.kind == Kind::fader || act.kind == Kind::master;
        const bool ok = analog ? (s.type == SrcType::cc || s.type == SrcType::pitchBend)
                               : ((s.type == SrcType::note && pressed) || (s.type == SrcType::cc && pressed));
        if (! ok)
            return;

        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                      [&](const Binding& b) { return sameSource(b.src, s) || b.act == act; }),
                       bindings.end());
        bindings.push_back({ s, act });
        learned.insert(sourceKey(s));
        learnQueue.pop_front();
        resetPickup();
        invalidateFeedback();
        save();
    }

    bool dispatch(const Action& a, float value, bool pressed)
    {
        switch (a.kind)
        {
            case Kind::fader:
            {
                const int rack = rackFor(a.index);
                if (rack < 0) return false;
                return analog(actionKey(a), value, host.midiGetFader(rack),
                              [this, rack](float g) { host.midiSetFader(rack, g); });
            }
            case Kind::master:
                return analog(actionKey(a), value, host.midiGetMaster(), [this](float g) { host.midiSetMaster(g); });

            case Kind::mute:
            {
                const int rack = rackFor(a.index);
                if (! pressed || rack < 0) return false;
                host.midiSetMute(rack, ! host.midiGetMute(rack));
                return true;
            }
            case Kind::solo:
            {
                const int rack = rackFor(a.index);
                if (! pressed || rack < 0) return false;
                host.midiSetSolo(rack, ! host.midiGetSolo(rack));
                return true;
            }
            case Kind::bankDown:  if (pressed) setBank(bank - 1); return false;
            case Kind::bankUp:    if (pressed) setBank(bank + 1); return false;
            case Kind::snapshot:
                if (pressed && host.midiHasSnapshot(a.index))
                {
                    host.midiRecallSnapshot(a.index);
                    resetPickup();
                    return true;
                }
                return false;
        }
        return false;
    }

    // Faders and knobs, with optional soft takeover (the physical control has to reach the
    // current value before it takes over, so a bank change does not make the level jump).
    bool analog(int key, float value, float currentGain, const std::function<void(float)>& apply)
    {
        auto& st = pickupState[key];
        if (pickup && ! st.engaged)
        {
            const float cur = gainToNorm(currentGain);
            const bool closeEnough = std::abs(value - cur) <= 0.02f;
            const bool crossed = st.last >= 0.0f && (st.last - cur) * (value - cur) <= 0.0f;
            st.last = value;
            if (! (closeEnough || crossed))
                return false;
        }
        st.engaged = true;
        st.last = value;
        lastInput[key] = juce::Time::getMillisecondCounter();
        apply(normToGain(value));
        return true;
    }

    //--------------------------------------------------------------------------
    // Feedback to the controller: LEDs and motorised faders.
    void timerCallback() override
    {
        if (output == nullptr || bindings.empty())
            return;

        const auto now = juce::Time::getMillisecondCounter();

        for (auto& b : bindings)
        {
            int value = -1;
            bool led = false;
            bool isLed = false;

            switch (b.act.kind)
            {
                case Kind::fader:
                case Kind::master:
                {
                    if (! faderFeedback) continue;
                    if (now - lastInput[actionKey(b.act)] < 500) continue;      // the user is touching it
                    float gain;
                    if (b.act.kind == Kind::master)
                        gain = host.midiGetMaster();
                    else
                    {
                        const int rack = rackFor(b.act.index);
                        if (rack < 0) continue;
                        gain = host.midiGetFader(rack);
                    }
                    const float n = gainToNorm(gain);
                    value = b.src.type == SrcType::pitchBend ? juce::roundToInt(n * 16383.0f)
                                                             : juce::roundToInt(n * 127.0f);
                    break;
                }
                case Kind::mute:
                case Kind::solo:
                {
                    if (! ledFeedback) continue;
                    const int rack = rackFor(b.act.index);
                    led = rack >= 0 && (b.act.kind == Kind::mute ? host.midiGetMute(rack) : host.midiGetSolo(rack));
                    isLed = true;
                    break;
                }
                case Kind::snapshot:
                    if (! ledFeedback) continue;
                    led = host.midiCurrentSnapshot() == b.act.index;
                    isLed = true;
                    break;
                case Kind::bankDown:
                    if (! ledFeedback) continue;
                    led = bank > 0;
                    isLed = true;
                    break;
                case Kind::bankUp:
                    if (! ledFeedback) continue;
                    led = bank < getNumBanks() - 1;
                    isLed = true;
                    break;
            }

            if (isLed)
                value = led ? 1 : 0;

            const int key = sourceKey(b.src);
            auto it = sent.find(key);
            if (it != sent.end() && it->second == value)
                continue;
            sent[key] = value;

            if (isLed)
            {
                if (b.src.type == SrcType::note)
                    output->sendMessageNow(led ? juce::MidiMessage::noteOn(b.src.channel, b.src.number, (juce::uint8) 127)
                                               : juce::MidiMessage::noteOff(b.src.channel, b.src.number));
                else if (b.src.type == SrcType::cc)
                    output->sendMessageNow(juce::MidiMessage::controllerEvent(b.src.channel, b.src.number, led ? 127 : 0));
            }
            else if (b.src.type == SrcType::pitchBend)
                output->sendMessageNow(juce::MidiMessage::pitchWheel(b.src.channel, value));
            else if (b.src.type == SrcType::cc)
                output->sendMessageNow(juce::MidiMessage::controllerEvent(b.src.channel, b.src.number, value));
        }
    }

    //--------------------------------------------------------------------------
    void save() const
    {
        juce::XmlElement root("Midi");
        root.setAttribute("in", inputId);
        root.setAttribute("inName", inputName);
        root.setAttribute("out", outputId);
        root.setAttribute("outName", outputName);
        root.setAttribute("strips", stripCount);
        root.setAttribute("pickup", pickup);
        root.setAttribute("faderFb", faderFeedback);
        root.setAttribute("ledFb", ledFeedback);
        for (auto& b : bindings)
        {
            auto* e = root.createNewChildElement("Bind");
            e->setAttribute("kind", (int) b.act.kind);
            e->setAttribute("idx", b.act.index);
            e->setAttribute("type", (int) b.src.type);
            e->setAttribute("ch", b.src.channel);
            e->setAttribute("num", b.src.number);
        }
        file.getParentDirectory().createDirectory();
        root.writeTo(file);
    }

    void load()
    {
        auto xml = juce::parseXML(file);
        if (xml == nullptr || ! xml->hasTagName("Midi"))
            return;

        inputId = xml->getStringAttribute("in");
        inputName = xml->getStringAttribute("inName");
        outputId = xml->getStringAttribute("out");
        outputName = xml->getStringAttribute("outName");
        stripCount = juce::jlimit(1, kMaxStrips, xml->getIntAttribute("strips", 8));
        pickup = xml->getBoolAttribute("pickup", true);
        faderFeedback = xml->getBoolAttribute("faderFb", false);
        ledFeedback = xml->getBoolAttribute("ledFb", true);

        bindings.clear();
        for (auto* e : xml->getChildWithTagNameIterator("Bind"))
        {
            Binding b;
            b.act.kind = (Kind) juce::jlimit(0, 6, e->getIntAttribute("kind"));
            b.act.index = e->getIntAttribute("idx");
            b.src.type = (SrcType) juce::jlimit(0, 2, e->getIntAttribute("type"));
            b.src.channel = juce::jlimit(1, 16, e->getIntAttribute("ch", 1));
            b.src.number = juce::jlimit(0, 127, e->getIntAttribute("num"));
            bindings.push_back(b);
        }
    }

    //--------------------------------------------------------------------------
    struct Pickup { bool engaged = false; float last = -1.0f; };

    MidiHost& host;
    juce::File file;
    juce::String inputId, inputName, outputId, outputName;
    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;

    int stripCount = 8;
    int bank = 0;
    bool pickup = true, faderFeedback = false, ledFeedback = true;

    std::vector<Binding> bindings;
    std::deque<Action> learnQueue;
    std::set<int> learned;
    int totalToLearn = 0;
    juce::String lastMessage;

    std::map<int, Pickup> pickupState;
    std::map<int, bool> ccDown;
    std::map<int, juce::uint32> lastInput;
    std::map<int, int> sent;

    juce::CriticalSection lock;
    std::vector<juce::MidiMessage> queue;
};
