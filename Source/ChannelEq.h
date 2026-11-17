#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>
#include <complex>

//==============================================================================
// The built-in channel-strip EQ every rack has, ahead of its plugin chain:
// a high-pass filter, a low shelf, two peaking bands and a high shelf.
// Parameters are atomics so the UI can change them while audio is running.
class ChannelEq : public juce::AudioProcessor
{
public:
    static constexpr int kBands = 5;

    enum class Type { highPass, lowShelf, peak, highShelf };

    struct Band
    {
        std::atomic<float> freq { 1000.0f }, gain { 0.0f }, q { 1.0f };
        std::atomic<bool> on { true };
        Type type = Type::peak;
    };

    Band bands[kBands];
    std::atomic<bool> enabled { true };

    ChannelEq()
        : AudioProcessor(BusesProperties()
                             .withInput("In", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
        bands[0].type = Type::highPass;
        bands[1].type = Type::lowShelf;
        bands[2].type = Type::peak;
        bands[3].type = Type::peak;
        bands[4].type = Type::highShelf;
        setDefaults();
    }

    static const char* bandName(int i)
    {
        static const char* names[kBands] = { "HPF", "Low", "Mid 1", "Mid 2", "High" };
        return names[i];
    }

    void setDefaults()
    {
        const float f[kBands] = { 80.0f, 120.0f, 400.0f, 2500.0f, 8000.0f };
        const float q[kBands] = { 0.71f, 0.71f, 1.0f, 1.0f, 0.71f };
        for (int i = 0; i < kBands; ++i)
        {
            bands[i].freq = f[i];
            bands[i].gain = 0.0f;
            bands[i].q = q[i];
            bands[i].on = (i != 0);         // the high-pass starts off
        }
        enabled = true;
        ++version;
    }

    void setBand(int i, float freq, float gainDb, float q, bool on)
    {
        auto& b = bands[i];
        b.freq = juce::jlimit(20.0f, 20000.0f, freq);
        b.gain = juce::jlimit(-18.0f, 18.0f, gainDb);
        b.q = juce::jlimit(0.2f, 10.0f, q);
        b.on = on;
        ++version;
    }

    void touch() { ++version; }

    // Does this band currently change the sound?
    bool isActive(int i) const
    {
        const auto& b = bands[i];
        return b.on.load() && (b.type == Type::highPass || std::abs(b.gain.load()) > 0.005f);
    }

    //--------------------------------------------------------------------------
    struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    static Coeffs makeCoeffs(Type type, double fs, double freq, double gainDb, double q)
    {
        freq = juce::jlimit(20.0, juce::jmin(20000.0, fs * 0.45), freq);
        const double w0 = juce::MathConstants<double>::twoPi * freq / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double sqA2 = 2.0 * std::sqrt(A) * alpha;

        double b0, b1, b2, a0, a1, a2;
        switch (type)
        {
            case Type::highPass:
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
                a0 = 1 + alpha;    a1 = -2 * cw;   a2 = 1 - alpha;
                break;
            case Type::lowShelf:
                b0 = A * ((A + 1) - (A - 1) * cw + sqA2);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - sqA2);
                a0 = (A + 1) + (A - 1) * cw + sqA2;
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - sqA2;
                break;
            case Type::highShelf:
                b0 = A * ((A + 1) + (A - 1) * cw + sqA2);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - sqA2);
                a0 = (A + 1) - (A - 1) * cw + sqA2;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - sqA2;
                break;
            case Type::peak:
            default:
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
                break;
        }

        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    static double magnitudeDb(const Coeffs& c, double fs, double f)
    {
        const double w = juce::MathConstants<double>::twoPi * f / fs;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
        const auto num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const auto den = 1.0 + c.a1 * z1 + c.a2 * z2;
        return 20.0 * std::log10(juce::jmax(1.0e-9, std::abs(num / den)));
    }

    // Response of one band, or of the whole EQ, at a frequency (for drawing).
    double bandResponseDb(int i, double f) const
    {
        if (! isActive(i)) return 0.0;
        const auto& b = bands[i];
        return magnitudeDb(makeCoeffs(b.type, fs.load(), b.freq.load(), b.gain.load(), b.q.load()), fs.load(), f);
    }

    double totalResponseDb(double f) const
    {
        if (! enabled.load()) return 0.0;
        double sum = 0.0;
        for (int i = 0; i < kBands; ++i)
            sum += bandResponseDb(i, f);
        return sum;
    }

    //--------------------------------------------------------------------------
    std::unique_ptr<juce::XmlElement> createXml() const
    {
        auto e = std::make_unique<juce::XmlElement>("Eq");
        e->setAttribute("on", enabled.load());
        for (int i = 0; i < kBands; ++i)
        {
            auto* b = e->createNewChildElement("Band");
            b->setAttribute("i", i);
            b->setAttribute("freq", (double) bands[i].freq.load());
            b->setAttribute("gain", (double) bands[i].gain.load());
            b->setAttribute("q", (double) bands[i].q.load());
            b->setAttribute("on", bands[i].on.load());
        }
        return e;
    }

    void loadXml(const juce::XmlElement& e)
    {
        setDefaults();
        enabled = e.getBoolAttribute("on", true);
        for (auto* b : e.getChildWithTagNameIterator("Band"))
        {
            const int i = b->getIntAttribute("i", -1);
            if (i < 0 || i >= kBands) continue;
            setBand(i, (float) b->getDoubleAttribute("freq", bands[i].freq.load()),
                    (float) b->getDoubleAttribute("gain", 0.0),
                    (float) b->getDoubleAttribute("q", bands[i].q.load()),
                    b->getBoolAttribute("on", true));
        }
        ++version;
    }

    //--------------------------------------------------------------------------
    const juce::String getName() const override { return "Channel EQ"; }

    void prepareToPlay(double sampleRate, int) override
    {
        fs = sampleRate;
        for (auto& ch : z)
            for (auto& band : ch)
                band[0] = band[1] = 0.0;
        rebuild();
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (! enabled.load(std::memory_order_relaxed))
            return;

        if (builtVersion != version.load(std::memory_order_relaxed))
            rebuild();

        const int numCh = juce::jmin(2, buffer.getNumChannels());
        const int n = buffer.getNumSamples();

        for (int b = 0; b < kBands; ++b)
        {
            if (! active[b]) continue;
            const auto& c = coeffs[b];

            for (int ch = 0; ch < numCh; ++ch)
            {
                float* d = buffer.getWritePointer(ch);
                double z1 = z[ch][b][0], z2 = z[ch][b][1];

                for (int i = 0; i < n; ++i)
                {
                    const double x = d[i];
                    const double y = c.b0 * x + z1;
                    z1 = c.b1 * x - c.a1 * y + z2;
                    z2 = c.b2 * x - c.a2 * y;
                    d[i] = (float) y;
                }

                z[ch][b][0] = z1;
                z[ch][b][1] = z2;
            }
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    void rebuild()
    {
        const unsigned int v = version.load();
        const double rate = fs.load();
        for (int i = 0; i < kBands; ++i)
        {
            active[i] = isActive(i);
            if (active[i])
                coeffs[i] = makeCoeffs(bands[i].type, rate, bands[i].freq.load(), bands[i].gain.load(),
                                       bands[i].q.load());
        }
        builtVersion = v;
    }

    std::atomic<double> fs { 44100.0 };
    std::atomic<unsigned int> version { 0 };
    unsigned int builtVersion = 0xffffffffu;
    bool active[kBands] = {};
    Coeffs coeffs[kBands];
    double z[2][kBands][2] = {};
};
