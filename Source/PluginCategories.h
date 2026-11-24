#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <unordered_map>

//==============================================================================
// Sorts plugins into types (Reverb, Delay, Distortion ...). The type comes from what the plugin
// reports about itself (VST3 sub-categories) where it says something useful, otherwise from words in
// its name. Anything can be overridden by hand; overrides are kept in categories.xml.
namespace PluginCategories
{
    inline const juce::StringArray& all()
    {
        static const juce::StringArray a { "Reverb", "Delay", "Distortion", "Filter", "EQ", "Dynamics",
                                           "Pitch", "Modulation", "Utility", "Other" };
        return a;
    }

    struct Store
    {
        std::unordered_map<std::string, juce::String> overrides;
        std::unordered_map<std::string, juce::String> cache;
        juce::File file;
        bool loaded = false;
    };

    inline Store& store()
    {
        static Store s;
        if (! s.loaded)
        {
            s.loaded = true;
            s.file = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("OpenRack").getChildFile("categories.xml");
            if (auto xml = juce::XmlDocument::parse(s.file))
                for (auto* c : xml->getChildWithTagNameIterator("C"))
                    s.overrides[c->getStringAttribute("id").toStdString()] = c->getStringAttribute("cat");
        }
        return s;
    }

    inline bool hasAny(const juce::String& text, std::initializer_list<const char*> words)
    {
        for (auto* w : words)
            if (text.containsIgnoreCase(w)) return true;
        return false;
    }

    inline juce::String guess(const juce::PluginDescription& d)
    {
        // 1) what the plugin says about itself ("Fx|Reverb", "Fx|Dynamics|Compressor" ...)
        const auto c = d.category;
        if (c.isNotEmpty())
        {
            if (hasAny(c, { "reverb", "spatial" }))                   return "Reverb";
            if (hasAny(c, { "delay", "echo" }))                       return "Delay";
            if (hasAny(c, { "distort", "overdrive", "saturat" }))     return "Distortion";
            if (hasAny(c, { "filter" }))                              return "Filter";
            if (hasAny(c, { "eq" }) && ! hasAny(c, { "sequ" }))       return "EQ";
            if (hasAny(c, { "dynamic", "compress", "limit", "gate", "expand" })) return "Dynamics";
            if (hasAny(c, { "pitch" }))                               return "Pitch";
            if (hasAny(c, { "modulat", "chorus", "flang", "phas" }))  return "Modulation";
            if (hasAny(c, { "tools", "analyz", "analys", "utility", "meter", "mastering" })) return "Utility";
        }

        // 2) words in the name
        const auto n = d.name;
        if (hasAny(n, { "reverb", "verb", "hall", "plate", "chamber", "spring", "shimmer", "room" })) return "Reverb";
        if (hasAny(n, { "delay", "echo", "slapback", "dub " }))                                       return "Delay";
        if (hasAny(n, { "compress", "limit", "gate", "expander", "de-ess", "deess", "dynamics", "transient",
                        "leveler", "leveller", "maximi", "1176", "la-2a", "la2a", "ssl bus", "clipper" })) return "Dynamics";
        if (hasAny(n, { "distort", "overdrive", "fuzz", "saturat", "crunch", "bitcrush", "drive", "tube",
                        "amp", "tape", "warm" }))                                                     return "Distortion";
        if (hasAny(n, { "filter", "wah", "lowpass", "highpass", "vocoder", "formant" }))              return "Filter";
        if (hasAny(n, { "pitch", "tune", "harmon", "octave", "shifter", "melodyne", "autotune", "auto-tune" })) return "Pitch";
        if (hasAny(n, { "chorus", "flang", "phase", "phaser", "tremolo", "vibrato", "rotary", "ensemble", "mod" })) return "Modulation";
        if (n.containsWholeWordIgnoreCase("eq") || hasAny(n, { "equal", "pultec", "parametric", "channel strip" })) return "EQ";
        if (hasAny(n, { "meter", "analy", "gain", "utility", "trim", "width", "imager", "pan", "stereo", "tuner",
                        "mono", "phase" }))                                                           return "Utility";

        return "Other";
    }

    inline juce::String of(const juce::PluginDescription& d)
    {
        auto& s = store();
        const auto id = d.createIdentifierString().toStdString();

        if (auto o = s.overrides.find(id); o != s.overrides.end())
            return o->second;
        if (auto c = s.cache.find(id); c != s.cache.end())
            return c->second;
        return s.cache[id] = guess(d);
    }

    inline bool isOverridden(const juce::PluginDescription& d)
    {
        return store().overrides.count(d.createIdentifierString().toStdString()) != 0;
    }

    // An empty category removes the override (back to automatic).
    inline void setOverride(const juce::PluginDescription& d, const juce::String& category)
    {
        auto& s = store();
        const auto id = d.createIdentifierString().toStdString();
        if (category.isEmpty()) s.overrides.erase(id);
        else                    s.overrides[id] = category;

        juce::XmlElement root("Categories");
        for (auto& [k, v] : s.overrides)
        {
            auto* e = root.createNewChildElement("C");
            e->setAttribute("id", juce::String(k));
            e->setAttribute("cat", v);
        }
        s.file.getParentDirectory().createDirectory();
        root.writeTo(s.file);
    }
}
