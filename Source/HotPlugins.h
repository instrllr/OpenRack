#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <map>

//==============================================================================
// Remembers which plugins the user pinned and how often each one was added,
// so the "hot plugins" strip and menu can offer them for one-click access.
// Plugins are identified by PluginDescription::createIdentifierString().
class HotPlugins
{
public:
    explicit HotPlugins(juce::File f) : file(std::move(f)) { reload(); }

    void reload()
    {
        pinned.clear();
        counts.clear();

        if (auto xml = juce::parseXML(file))
        {
            for (auto* e : xml->getChildWithTagNameIterator("Pin"))
                pinned.add(e->getStringAttribute("id"));

            for (auto* e : xml->getChildWithTagNameIterator("Use"))
                counts[e->getStringAttribute("id")] = e->getIntAttribute("n");
        }
    }

    bool isPinned(const juce::String& id) const { return pinned.contains(id); }

    void togglePin(const juce::String& id)
    {
        if (pinned.contains(id)) pinned.removeString(id);
        else                     pinned.add(id);
        save();
    }

    void recordUse(const juce::String& id)
    {
        ++counts[id];
        save();
    }

    void forget(const juce::String& id)
    {
        pinned.removeString(id);
        counts.erase(id);
        save();
    }

    void clearUsage()
    {
        counts.clear();
        save();
    }

    int getUseCount(const juce::String& id) const
    {
        auto it = counts.find(id);
        return it == counts.end() ? 0 : it->second;
    }

    const juce::StringArray& getPinned() const { return pinned; }

    // The most-used plugin ids (not already pinned), busiest first.
    juce::StringArray getMostUsed(int maxCount) const
    {
        std::vector<std::pair<juce::String, int>> all(counts.begin(), counts.end());
        std::stable_sort(all.begin(), all.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });

        juce::StringArray out;
        for (auto& p : all)
        {
            if (out.size() >= maxCount) break;
            if (p.second > 0 && ! pinned.contains(p.first))
                out.add(p.first);
        }
        return out;
    }

    bool hasAnyUsage() const { return ! counts.empty(); }

private:
    void save() const
    {
        juce::XmlElement root("Hot");
        for (auto& id : pinned)
            root.createNewChildElement("Pin")->setAttribute("id", id);
        for (auto& kv : counts)
        {
            auto* e = root.createNewChildElement("Use");
            e->setAttribute("id", kv.first);
            e->setAttribute("n", kv.second);
        }
        file.getParentDirectory().createDirectory();
        root.writeTo(file);
    }

    juce::File file;
    juce::StringArray pinned;
    std::map<juce::String, int> counts;
};
