#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <new>
#include <thread>
#include <vector>

//==============================================================================
// Out-of-process plugin hosting.
//
// Each isolated plugin runs in its own OpenRack helper process (the same executable started
// with --host). The two processes share a small memory-mapped block that carries the audio
// (stereo, in place) plus a mailbox for commands (get / set state, open the editor...). A
// named pipe is used only to wake the helper once per audio block. If the helper crashes or
// hangs, OpenRack keeps running: the slot passes audio through untouched while the helper is
// restarted and its last known state is restored.
namespace remote
{
    struct Shared
    {
        static constexpr juce::uint32 kMagic = 0x4f524831;
        static constexpr int kMaxBlock = 8192;
        static constexpr int kChannels = 2;

        enum Command { cmdNone = 0, cmdGetState = 1, cmdSetState = 2, cmdOpenEditor = 3, cmdPrepare = 4, cmdQuit = 5 };

        std::atomic<juce::uint32> magic { 0 };
        std::atomic<juce::uint32> requestSeq { 0 }, doneSeq { 0 };
        std::atomic<juce::int32> numSamples { 0 };
        std::atomic<juce::int32> latency { 0 };
        std::atomic<juce::int32> status { 0 };          // 0 starting, 1 ready, 2 failed, 3 exiting
        std::atomic<juce::uint32> mainBeat { 0 };       // heartbeat from OpenRack, so an orphaned helper quits
        std::atomic<juce::int32> cmd { 0 };
        std::atomic<juce::int32> cmdArg { 0 }, cmdArg2 { 0 };
        std::atomic<juce::int32> cmdResult { 0 };
        char error[512] = {};
        alignas(64) float audio[kChannels][kMaxBlock];
    };

    //==========================================================================
    // The helper process side.
    class Worker : private juce::Timer
    {
    public:
        // args: shared file, pipe name, plugin description file, state file, sample rate, block size
        explicit Worker(const juce::StringArray& args)
            : map(juce::File(args[0]), juce::MemoryMappedFile::readWrite), stateFile(args[3])
        {
            if (args.size() < 6 || map.getData() == nullptr || map.getSize() < sizeof(Shared))
                return;

            sh = static_cast<Shared*>(map.getData());
            if (sh->magic.load() != Shared::kMagic)
                return;

            if (! pipe.openExisting(args[1]))
                { fail("Could not open the audio pipe"); return; }

            juce::PluginDescription d;
            auto xml = juce::parseXML(juce::File(args[2]));
            if (xml == nullptr || ! d.loadFromXml(*xml))
                { fail("Could not read the plugin description"); return; }

            sampleRate = args[4].getDoubleValue();
            blockSize = juce::jmax(16, args[5].getIntValue());

            juce::AudioPluginFormatManager formats;
            formats.addDefaultFormats();

            juce::String error;
            plugin = formats.createPluginInstance(d, sampleRate, blockSize, error);
            if (plugin == nullptr)
                { fail(error.isEmpty() ? "Could not load the plugin" : error); return; }

            // One stereo bus in and out; any side-chain or extra buses are switched off.
            using Set = juce::AudioChannelSet;
            auto layout = plugin->getBusesLayout();
            for (int i = 0; i < layout.inputBuses.size(); ++i)
                layout.inputBuses.getReference(i) = (i == 0) ? Set::stereo() : Set::disabled();
            for (int i = 0; i < layout.outputBuses.size(); ++i)
                layout.outputBuses.getReference(i) = (i == 0) ? Set::stereo() : Set::disabled();
            if (! plugin->setBusesLayout(layout))
                { fail(d.name + " does not support a plain stereo layout"); return; }

            plugin->prepareToPlay(sampleRate, blockSize);
            sh->latency.store(plugin->getLatencySamples());

            loop = std::make_unique<AudioLoop>(*this);
            loop->startThread(juce::Thread::Priority::highest);

            lastBeat = sh->mainBeat.load();
            lastBeatTime = juce::Time::getMillisecondCounter();
            running = true;
            sh->status.store(1);
            startTimerHz(100);
        }

        ~Worker() override
        {
            stopTimer();
            if (loop != nullptr)
                loop->stopThread(2000);
            window.reset();
            plugin.reset();
        }

        bool isOk() const { return running; }

    private:
        void fail(const juce::String& message)
        {
            std::strncpy(sh->error, message.toRawUTF8(), sizeof(sh->error) - 1);
            sh->status.store(2);
        }

        struct AudioLoop : juce::Thread
        {
            explicit AudioLoop(Worker& w) : juce::Thread("OpenRack host audio"), worker(w) {}

            void run() override
            {
                while (! threadShouldExit())
                {
                    char token = 0;
                    if (worker.pipe.read(&token, 1, 100) == 1)
                        worker.processOne();
                    else
                        juce::Thread::sleep(2);
                }
            }

            Worker& worker;
        };

        void processOne()
        {
            const juce::ScopedLock sl(processLock);
            const auto seq = sh->requestSeq.load(std::memory_order_acquire);
            const int n = sh->numSamples.load(std::memory_order_relaxed);

            if (plugin != nullptr && n > 0 && n <= Shared::kMaxBlock)
            {
                juce::ScopedNoDenormals noDenormals;
                float* channels[2] = { sh->audio[0], sh->audio[1] };
                juce::AudioBuffer<float> buffer(channels, 2, n);
                juce::MidiBuffer midi;
                plugin->processBlock(buffer, midi);

                const int lat = plugin->getLatencySamples();
                if (lat != sh->latency.load(std::memory_order_relaxed))
                    sh->latency.store(lat, std::memory_order_relaxed);
            }

            sh->doneSeq.store(seq, std::memory_order_release);
        }

        //----------------------------------------------------------------------
        struct EditorWindow : juce::DocumentWindow
        {
            EditorWindow(juce::AudioProcessor& p, std::function<void()> onCloseFn)
                : DocumentWindow(p.getName(), juce::Colours::darkgrey, DocumentWindow::closeButton),
                  onClose(std::move(onCloseFn))
            {
                juce::AudioProcessorEditor* ed = p.createEditorIfNeeded();
                if (ed == nullptr)
                    ed = new juce::GenericAudioProcessorEditor(p);
                setUsingNativeTitleBar(true);
                setContentOwned(ed, true);
                setResizable(ed->isResizable(), false);
                centreWithSize(getWidth(), getHeight());
                setVisible(true);
            }

            void closeButtonPressed() override { onClose(); }

            std::function<void()> onClose;
        };

        int execute(int command)
        {
            switch (command)
            {
                case Shared::cmdGetState:
                {
                    juce::MemoryBlock mb;
                    plugin->getStateInformation(mb);
                    return stateFile.replaceWithData(mb.getData(), mb.getSize()) ? 1 : -1;
                }
                case Shared::cmdSetState:
                {
                    juce::MemoryBlock mb;
                    if (! stateFile.loadFileAsData(mb))
                        return -1;
                    plugin->setStateInformation(mb.getData(), (int) mb.getSize());
                    return 1;
                }
                case Shared::cmdOpenEditor:
                {
                    if (window != nullptr)
                    {
                        window->toFront(true);
                        return 1;
                    }
                    if (! plugin->hasEditor())
                        return -1;
                    window = std::make_unique<EditorWindow>(*plugin, [this]
                    {
                        juce::MessageManager::callAsync([this] { window.reset(); });
                    });
                    return 1;
                }
                case Shared::cmdPrepare:
                {
                    const juce::ScopedLock sl(processLock);
                    blockSize = juce::jmax(16, sh->cmdArg.load());
                    sampleRate = (double) sh->cmdArg2.load();
                    plugin->releaseResources();
                    plugin->prepareToPlay(sampleRate, blockSize);
                    sh->latency.store(plugin->getLatencySamples());
                    return 1;
                }
                case Shared::cmdQuit:
                    quitNow();
                    return 1;
                default:
                    return -1;
            }
        }

        void quitNow()
        {
            stopTimer();
            sh->status.store(3);
            juce::JUCEApplicationBase::quit();
        }

        void timerCallback() override
        {
            const auto now = juce::Time::getMillisecondCounter();
            const auto beat = sh->mainBeat.load();

            if (beat != lastBeat)
            {
                lastBeat = beat;
                lastBeatTime = now;
            }
            else if (now - lastBeatTime > 10000)        // OpenRack is gone
            {
                quitNow();
                return;
            }

            const int command = sh->cmd.load(std::memory_order_acquire);
            if (command != Shared::cmdNone)
            {
                const int result = execute(command);
                sh->cmdResult.store(result);
                sh->cmd.store(Shared::cmdNone, std::memory_order_release);
            }
        }

        juce::MemoryMappedFile map;
        juce::File stateFile;
        Shared* sh = nullptr;
        juce::NamedPipe pipe;
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        std::unique_ptr<EditorWindow> window;
        std::unique_ptr<AudioLoop> loop;
        juce::CriticalSection processLock;
        double sampleRate = 44100.0;
        int blockSize = 512;
        bool running = false;
        juce::uint32 lastBeat = 0, lastBeatTime = 0;
    };

    //==========================================================================
    // The OpenRack side: an AudioProcessor that stands in for the real plugin.
    class RemotePlugin : public juce::AudioProcessor, private juce::Timer
    {
    public:
        enum class State { launching, ready, crashed, failed };

        // cleanFailure: the helper reported a normal error (e.g. no stereo layout), so loading the
        // plugin in-process is still an option. false means the helper died while loading it.
        using Done = std::function<void(std::unique_ptr<RemotePlugin>, const juce::String& error, bool cleanFailure)>;

        // Starts the helper and calls `done` (message thread) when the plugin is loaded or has failed.
        static void launch(const juce::PluginDescription& d, double sr, int bs, Done done)
        {
            std::unique_ptr<RemotePlugin> p(new RemotePlugin(d, sr, bs));
            if (! p->startChild())
            {
                const auto e = p->errorText;
                done(nullptr, e, true);
                return;
            }
            new Pending(std::move(p), std::move(done));
        }

        ~RemotePlugin() override
        {
            stopTimer();
            {
                const juce::ScopedLock sl(ioLock);
                active.store(false);
            }

            if (sh != nullptr && child != nullptr && child->isRunning())
            {
                sh->cmd.store(Shared::cmdQuit, std::memory_order_release);
                child->waitForProcessToFinish(300);
            }
            teardownChild();
        }

        State getState() const { return state; }
        const juce::String& getError() const { return errorText; }
        bool isIsolated() const { return true; }
        int getTotalMisses() const { return totalMisses.load(); }

        juce::String getStatusText() const
        {
            switch (state)
            {
                case State::launching: return "starting separate process";
                case State::ready:     return "running in its own process";
                case State::crashed:   return "CRASHED - restarting";
                case State::failed:    return "CRASHED - not restarted (audio passes through)";
            }
            return {};
        }

        std::function<void(const juce::String&)> onEvent;      // crashes and restarts, for the status bar
        std::function<void()> onLatencyChanged;

        // For the self-test: kill the helper as if the plugin had crashed.
        void debugKillChild() { if (child != nullptr) child->kill(); }

        // A manual restart (after the automatic restarts gave up).
        void restart()
        {
            crashTimes.clear();
            beginRestart();
        }

        //----------------------------------------------------------------------
        const juce::String getName() const override { return desc.name; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        double getTailLengthSeconds() const override { return 0.0; }
        bool hasEditor() const override { return true; }
        juce::AudioProcessorEditor* createEditor() override { return new RemoteEditor(*this); }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void releaseResources() override {}

        void prepareToPlay(double sr, int bs) override
        {
            sampleRate = sr;
            const bool changed = (std::abs(sr - launchedRate) > 0.5 || bs != launchedBlock);
            launchedRate = sr;
            launchedBlock = bs;
            if (changed && state == State::ready)
                sendCommand(Shared::cmdPrepare, bs, (int) sr);
        }

        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            const juce::ScopedTryLock sl(ioLock);
            if (! sl.isLocked() || ! active.load(std::memory_order_acquire) || sh == nullptr)
                return;                                 // dry: the audio passes through unchanged

            const int total = buffer.getNumSamples();
            const int chans = juce::jmin(2, buffer.getNumChannels());
            if (chans == 0)
                return;

            for (int pos = 0; pos < total; pos += Shared::kMaxBlock)
                if (! processChunk(buffer, pos, juce::jmin(Shared::kMaxBlock, total - pos), chans))
                    break;
        }

        void getStateInformation(juce::MemoryBlock& dest) override
        {
            if (state == State::ready && sendCommand(Shared::cmdGetState))
            {
                juce::MemoryBlock mb;
                if (stateFile.loadFileAsData(mb))
                    cachedState = mb;
            }
            dest = cachedState;         // after a crash this is the last state that was seen
        }

        void setStateInformation(const void* data, int size) override
        {
            cachedState.replaceAll(data, (size_t) size);
            if (state == State::ready)
                pushState();
        }

        void openRemoteEditor()
        {
            if (state == State::ready)
                sendCommand(Shared::cmdOpenEditor);
        }

    private:
        //----------------------------------------------------------------------
        struct RemoteEditor : juce::AudioProcessorEditor
        {
            explicit RemoteEditor(RemotePlugin& p) : juce::AudioProcessorEditor(p), owner(p)
            {
                addAndMakeVisible(label);
                label.setJustificationType(juce::Justification::centred);
                label.setText(owner.desc.name + " runs in its own process.\nIts window opens separately.",
                              juce::dontSendNotification);
                addAndMakeVisible(button);
                button.setButtonText("Show plugin window");
                button.onClick = [this] { owner.openRemoteEditor(); };
                setSize(380, 130);
                owner.openRemoteEditor();
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced(12);
                button.setBounds(r.removeFromBottom(32));
                label.setBounds(r);
            }

            RemotePlugin& owner;
            juce::Label label;
            juce::TextButton button;
        };

        // Waits for a freshly launched helper, then hands the plugin over.
        struct Pending : private juce::Timer
        {
            Pending(std::unique_ptr<RemotePlugin> pl, Done d) : plugin(std::move(pl)), done(std::move(d))
            {
                startTimer(30);
            }

            void timerCallback() override
            {
                const auto st = plugin->getState();
                if (st == State::launching)
                    return;

                stopTimer();
                auto cb = std::move(done);
                auto p = std::move(plugin);
                const bool ok = st == State::ready;
                const auto err = p->getError();
                const bool clean = p->cleanFailure;
                delete this;

                if (ok) cb(std::move(p), {}, true);
                else    cb(nullptr, err, clean);
            }

            std::unique_ptr<RemotePlugin> plugin;
            Done done;
        };

        RemotePlugin(const juce::PluginDescription& d, double sr, int bs)
            : AudioProcessor(BusesProperties()
                                 .withInput("In", juce::AudioChannelSet::stereo(), true)
                                 .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
              desc(d), launchedRate(sr), launchedBlock(bs), sampleRate(sr)
        {
        }

        //----------------------------------------------------------------------
        static void spinPause() { std::this_thread::yield(); }

        // Audio thread. ioLock is held.
        bool processChunk(juce::AudioBuffer<float>& buffer, int pos, int n, int chans)
        {
            // The helper is still busy with an earlier block: pass this one through.
            if (sh->doneSeq.load(std::memory_order_acquire) != sh->requestSeq.load(std::memory_order_relaxed))
                return miss();

            for (int c = 0; c < Shared::kChannels; ++c)
                std::memcpy(sh->audio[c], buffer.getReadPointer(juce::jmin(c, chans - 1)) + pos,
                            (size_t) n * sizeof(float));

            sh->numSamples.store(n, std::memory_order_relaxed);
            const auto seq = sh->requestSeq.load(std::memory_order_relaxed) + 1;
            sh->requestSeq.store(seq, std::memory_order_release);

            const char token = 1;
            if (pipe == nullptr || pipe->write(&token, 1, 0) != 1)
            {
                pipeFailed.store(true);
                return miss();
            }

            const auto ticksPerSecond = (double) juce::Time::getHighResolutionTicksPerSecond();
            const auto budget = (juce::int64) (ticksPerSecond * (double) n / juce::jmax(8000.0, sampleRate) * 0.85);
            const auto deadline = juce::Time::getHighResolutionTicks() + juce::jmax<juce::int64>(budget, (juce::int64) (ticksPerSecond * 0.001));

            while (sh->doneSeq.load(std::memory_order_acquire) != seq)
            {
                if (juce::Time::getHighResolutionTicks() > deadline)
                    return miss();
                spinPause();
            }

            for (int c = 0; c < chans; ++c)
                std::memcpy(buffer.getWritePointer(c) + pos, sh->audio[c], (size_t) n * sizeof(float));

            consecutiveMisses = 0;
            return true;
        }

        bool miss()
        {
            totalMisses.fetch_add(1, std::memory_order_relaxed);
            if (++consecutiveMisses > 400)
                hung.store(true);
            return false;
        }

        //----------------------------------------------------------------------
        // Message thread. Runs a command in the helper and waits for it.
        bool sendCommand(int command, int a = 0, int b = 0, int timeoutMs = 15000)
        {
            if (sh == nullptr || child == nullptr)
                return false;

            sh->cmdArg.store(a);
            sh->cmdArg2.store(b);
            sh->cmdResult.store(0);
            sh->cmd.store(command, std::memory_order_release);

            const auto start = juce::Time::getMillisecondCounter();
            while (sh->cmd.load(std::memory_order_acquire) != Shared::cmdNone)
            {
                if (juce::Time::getMillisecondCounter() - start > (juce::uint32) timeoutMs || ! child->isRunning())
                    return false;
                juce::Thread::sleep(2);
            }
            return sh->cmdResult.load() == 1;
        }

        void pushState()
        {
            if (cachedState.getSize() == 0)
                return;
            if (stateFile.replaceWithData(cachedState.getData(), cachedState.getSize()))
                sendCommand(Shared::cmdSetState);
        }

        //----------------------------------------------------------------------
        bool startChild()
        {
            teardownChild();

            const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory);
            const auto base = "openrack-host-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())
                              + "-" + juce::String(restartCounter++);
            sharedFile = tmp.getChildFile(base + ".shm");
            descFile = tmp.getChildFile(base + ".desc.xml");
            stateFile = tmp.getChildFile(base + ".state");

            {
                juce::FileOutputStream out(sharedFile);
                if (out.failedToOpen())
                    return failStart("Could not create the shared memory file");
                juce::MemoryBlock zeros(sizeof(Shared), true);
                out.write(zeros.getData(), zeros.getSize());
                out.flush();
            }

            auto newMap = std::make_unique<juce::MemoryMappedFile>(sharedFile, juce::MemoryMappedFile::readWrite);
            if (newMap->getData() == nullptr || newMap->getSize() < sizeof(Shared))
                return failStart("Could not map the shared memory file");

            auto* newSh = new (newMap->getData()) Shared();
            newSh->magic.store(Shared::kMagic);
            newSh->mainBeat.store(1);

            auto newPipe = std::make_unique<juce::NamedPipe>();
            if (! newPipe->createNewPipe(base, true))
                return failStart("Could not create the audio pipe");

            if (auto xml = desc.createXml())
                xml->writeTo(descFile);

            const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
            auto proc = std::make_unique<juce::ChildProcess>();
            if (! proc->start(juce::StringArray { exe.getFullPathName(), "--host", sharedFile.getFullPathName(), base,
                                                  descFile.getFullPathName(), stateFile.getFullPathName(),
                                                  juce::String(launchedRate), juce::String(launchedBlock) }, 0))
                return failStart("Could not start the plugin host process");

            {
                const juce::ScopedLock sl(ioLock);
                map = std::move(newMap);
                sh = newSh;
                pipe = std::move(newPipe);
                child = std::move(proc);
                pipeFailed.store(false);
                hung.store(false);
                consecutiveMisses = 0;
            }

            state = State::launching;
            launchStart = juce::Time::getMillisecondCounter();
            startTimerHz(20);
            return true;
        }

        bool failStart(const juce::String& message)
        {
            errorText = message;
            state = State::failed;
            return false;
        }

        void teardownChild()
        {
            {
                const juce::ScopedLock sl(ioLock);
                active.store(false);
                if (child != nullptr)
                {
                    child->kill();
                    child->waitForProcessToFinish(500);
                }
                child.reset();
                pipe.reset();
                sh = nullptr;
                map.reset();
            }
            sharedFile.deleteFile();
            descFile.deleteFile();
            stateFile.deleteFile();
        }

        void beginRestart()
        {
            restarting = true;
            if (! startChild())
            {
                state = State::failed;
                if (onEvent) onEvent(desc.name + " could not be restarted.");
            }
        }

        void handleCrash()
        {
            active.store(false);
            state = State::crashed;
            const auto now = juce::Time::getMillisecondCounter();
            crashTimes.push_back(now);
            crashTimes.erase(std::remove_if(crashTimes.begin(), crashTimes.end(),
                                            [now](juce::uint32 t) { return now - t > 60000; }),
                             crashTimes.end());

            // Keep the most recent state we can still get (the helper is gone, so this is the cached copy).
            if (crashTimes.size() <= 3)
            {
                restartAt = now + 500;
                if (onEvent)
                    onEvent(desc.name + " stopped working. OpenRack is still running; restarting the plugin...");
            }
            else
            {
                state = State::failed;
                if (onEvent)
                    onEvent(desc.name + " keeps crashing. It is bypassed now - remove it or restart it from the rack.");
            }
            teardownChild();
        }

        void timerCallback() override
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (sh != nullptr)
                sh->mainBeat.fetch_add(1, std::memory_order_relaxed);

            switch (state)
            {
                case State::launching:
                {
                    const int st = sh != nullptr ? sh->status.load() : 2;
                    if (st == 1)
                    {
                        setLatencySamples(sh->latency.load());
                        state = State::ready;
                        active.store(true, std::memory_order_release);
                        if (restarting)
                        {
                            pushState();
                            restarting = false;
                            if (onEvent) onEvent(desc.name + " is running again.");
                            if (onLatencyChanged) onLatencyChanged();
                        }
                    }
                    else if (st == 2)
                    {
                        cleanFailure = true;
                        failStartKeepState(sh != nullptr ? juce::String(sh->error) : juce::String("The plugin host failed"));
                    }
                    else if (child == nullptr || ! child->isRunning())
                        failStartKeepState("The plugin host exited while loading " + desc.name);
                    else if (now - launchStart > 120000)
                        failStartKeepState(desc.name + " took too long to load");
                    break;
                }
                case State::ready:
                {
                    if (child == nullptr || ! child->isRunning() || hung.load() || pipeFailed.load())
                        handleCrash();
                    else if (sh != nullptr && sh->latency.load() != getLatencySamples())
                    {
                        setLatencySamples(sh->latency.load());
                        if (onLatencyChanged) onLatencyChanged();
                    }
                    break;
                }
                case State::crashed:
                    if ((juce::int32) (now - restartAt) >= 0)
                        beginRestart();
                    break;
                case State::failed:
                    break;
            }
        }

        void failStartKeepState(const juce::String& message)
        {
            errorText = message;
            const bool wasRestart = restarting;
            teardownChild();
            if (wasRestart)
            {
                // A restart that fails counts as another crash.
                restarting = false;
                handleCrash();
            }
            else
                state = State::failed;
        }

        //----------------------------------------------------------------------
        juce::PluginDescription desc;
        double launchedRate;
        int launchedBlock;
        double sampleRate;

        juce::CriticalSection ioLock;
        std::unique_ptr<juce::MemoryMappedFile> map;
        Shared* sh = nullptr;
        std::unique_ptr<juce::NamedPipe> pipe;
        std::unique_ptr<juce::ChildProcess> child;
        juce::File sharedFile, descFile, stateFile;

        State state = State::launching;
        std::atomic<bool> active { false }, hung { false }, pipeFailed { false };
        std::atomic<int> totalMisses { 0 };
        int consecutiveMisses = 0;
        juce::String errorText;
        juce::MemoryBlock cachedState;
        std::vector<juce::uint32> crashTimes;
        juce::uint32 launchStart = 0, restartAt = 0;
        bool restarting = false;
        bool cleanFailure = false;
        int restartCounter = 0;
    };
}
