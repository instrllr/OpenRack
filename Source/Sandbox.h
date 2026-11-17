#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

//==============================================================================
// Protection against plugins that crash.
//
//  * Crash marker: before a plugin is loaded its id is written to disk; if
//    OpenRack (or the DAW) dies while it loads, the next start finds the stale
//    marker and blocks that plugin.
//  * Block list: blocked plugins are refused until the user unblocks them.
//  * Pre-flight check (standalone app): a new plugin is first loaded and run in
//    a separate OpenRack process. If that process crashes, hangs or fails, the
//    plugin is never loaded into the real rack.
class PluginSafety
{
public:
    explicit PluginSafety(juce::File dir) : folder(std::move(dir))
    {
        folder.createDirectory();
        blockFile = folder.getChildFile("blocked.txt");
        markerFile = folder.getChildFile("loading.txt");
        verifiedFile = folder.getChildFile("verified.txt");
        blocked.addLines(blockFile.loadFileAsString());
        blocked.removeEmptyStrings();
        verified.addLines(verifiedFile.loadFileAsString());
        verified.removeEmptyStrings();
    }

    //--------------------------------------------------------------------------
    bool isBlocked(const juce::String& id) const { return blocked.contains(id); }

    void block(const juce::String& id)
    {
        blocked.addIfNotAlreadyThere(id);
        blockFile.replaceWithText(blocked.joinIntoString("\n"));
    }

    void unblock(const juce::String& id)
    {
        blocked.removeString(id);
        blockFile.replaceWithText(blocked.joinIntoString("\n"));
    }

    //--------------------------------------------------------------------------
    void markLoading(const juce::String& id) { markerFile.replaceWithText(id); }
    void clearLoading() { markerFile.deleteFile(); }
    juce::File getMarkerFile() const { return markerFile; }

    // A marker older than a few seconds that is still there means a previous
    // session died while loading that plugin. Returns its id (and blocks it).
    juce::String takeStaleMarker()
    {
        if (! markerFile.existsAsFile())
            return {};

        const bool stale = (juce::Time::getCurrentTime() - markerFile.getLastModificationTime()).inSeconds() > 20.0;
        if (! stale)
            return {};

        auto id = markerFile.loadFileAsString().trim();
        markerFile.deleteFile();
        if (id.isNotEmpty())
            block(id);
        return id;
    }

    //--------------------------------------------------------------------------
    static juce::String keyFor(const juce::PluginDescription& d)
    {
        return d.createIdentifierString() + "|"
             + juce::String(juce::File(d.fileOrIdentifier).getLastModificationTime().toMilliseconds());
    }

    bool isVerified(const juce::PluginDescription& d) const { return verified.contains(keyFor(d)); }

    void setVerified(const juce::PluginDescription& d)
    {
        verified.addIfNotAlreadyThere(keyFor(d));
        verifiedFile.replaceWithText(verified.joinIntoString("\n"));
    }

    //--------------------------------------------------------------------------
    // Runs the pre-flight check in a child process without blocking the UI.
    // `result` is called on the message thread: ok == false means do not load.
    static void runPreflight(const juce::PluginDescription& d,
                             std::function<void(bool ok, const juce::String& message)> result)
    {
        const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto descFile = tmp.getNonexistentChildFile("openrack-preflight", ".xml", false);
        const auto resultFile = tmp.getNonexistentChildFile("openrack-preflight-result", ".txt", false);

        auto xml = d.createXml();
        if (xml == nullptr || ! exe.existsAsFile() || ! xml->writeTo(descFile))
        {
            result(true, {});       // cannot run the check here; carry on without it
            return;
        }

        const auto name = d.name;

        juce::Thread::launch([exe, descFile, resultFile, name, result]
        {
            bool ok = false;
            juce::String message;

            juce::ChildProcess child;
            if (! child.start(juce::StringArray { exe.getFullPathName(), "--preflight",
                                                  descFile.getFullPathName(), resultFile.getFullPathName() }))
            {
                ok = true;          // could not start the checker; do not block the plugin for that
            }
            else if (! child.waitForProcessToFinish(45000))
            {
                child.kill();
                message = name + " did not finish its safety check in 45 seconds.";
            }
            else
            {
                ok = resultFile.loadFileAsString().trim() == "OK";
                if (! ok)
                    message = name + " crashed or failed during a safety check, so it was not loaded.";
            }

            descFile.deleteFile();
            resultFile.deleteFile();

            juce::MessageManager::callAsync([result, ok, message] { result(ok, message); });
        });
    }

    // The child side: load the plugin, run some audio through it, open and close
    // its editor. Returns true if everything survived.
    static bool runPreflightChild(const juce::File& descFile, const juce::File& resultFile)
    {
        bool ok = false;

        {
            juce::PluginDescription d;
            auto xml = juce::parseXML(descFile);

            if (xml != nullptr && d.loadFromXml(*xml))
            {
                juce::AudioPluginFormatManager fm;
                fm.addDefaultFormats();

                juce::String error;
                if (auto inst = fm.createPluginInstance(d, 44100.0, 512, error))
                {
                    const int channels = juce::jmax(2, inst->getTotalNumInputChannels(),
                                                       inst->getTotalNumOutputChannels());
                    inst->setPlayConfigDetails(inst->getTotalNumInputChannels(),
                                               inst->getTotalNumOutputChannels(), 44100.0, 512);
                    inst->prepareToPlay(44100.0, 512);

                    juce::AudioBuffer<float> buffer(channels, 512);
                    juce::MidiBuffer midi;
                    for (int i = 0; i < 8; ++i)
                    {
                        buffer.clear();
                        inst->processBlock(buffer, midi);
                    }

                    if (inst->hasEditor())
                        delete inst->createEditorIfNeeded();

                    inst->releaseResources();
                    inst.reset();
                    ok = true;
                }
            }
        }

        resultFile.replaceWithText(ok ? "OK" : "FAIL");
        return ok;
    }

private:
    juce::File folder, blockFile, markerFile, verifiedFile;
    juce::StringArray blocked, verified;
};
