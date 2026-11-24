#pragma once
#include <juce_graphics/juce_graphics.h>
#include <juce_core/juce_core.h>
#include <map>

//==============================================================================
// Pictures of plugins, shown on their rows in the rack. They live in the OpenRack data folder
// (images/), keyed by plugin. "<key>.u.png" is a picture you chose; "<key>.png" is one captured from
// the plugin's editor, which never replaces one you chose.
namespace PluginImages
{
    inline juce::File folder()
    {
        auto d = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                     .getChildFile("OpenRack").getChildFile("images");
        d.createDirectory();
        return d;
    }

    inline juce::String keyFor(const juce::String& pluginId) { return juce::String::toHexString(pluginId.hashCode64()); }

    inline std::map<juce::String, juce::Image>& cache()
    {
        static std::map<juce::String, juce::Image> c;
        return c;
    }

    inline juce::Image downscale(const juce::Image& in, int maxW = 420, int maxH = 280)
    {
        if (! in.isValid()) return {};
        const float s = juce::jmin(1.0f, (float) maxW / (float) in.getWidth(), (float) maxH / (float) in.getHeight());
        if (s >= 1.0f) return in;
        return in.rescaled(juce::jmax(1, juce::roundToInt((float) in.getWidth() * s)),
                           juce::jmax(1, juce::roundToInt((float) in.getHeight() * s)),
                           juce::Graphics::highResamplingQuality);
    }

    inline void save(const juce::Image& img, const juce::File& f)
    {
        f.deleteFile();
        if (auto out = f.createOutputStream())
            juce::PNGImageFormat().writeImageToStream(img, *out);
    }

    // Nothing but one flat colour (or fully transparent): typical of a native plugin window that
    // cannot be copied, so it is not worth keeping.
    inline bool looksBlank(const juce::Image& img)
    {
        if (! img.isValid()) return true;
        juce::Colour first;
        bool haveFirst = false;
        for (int y = 2; y < img.getHeight() - 2; y += juce::jmax(1, img.getHeight() / 24))
            for (int x = 2; x < img.getWidth() - 2; x += juce::jmax(1, img.getWidth() / 24))
            {
                const auto c = img.getPixelAt(x, y);
                if (c.getAlpha() < 8) continue;
                if (! haveFirst) { first = c; haveFirst = true; continue; }
                if (std::abs((int) c.getRed() - (int) first.getRed()) > 6
                    || std::abs((int) c.getGreen() - (int) first.getGreen()) > 6
                    || std::abs((int) c.getBlue() - (int) first.getBlue()) > 6)
                    return false;
            }
        return true;
    }

    inline bool hasUserImage(const juce::String& pluginId)
    {
        return folder().getChildFile(keyFor(pluginId) + ".u.png").existsAsFile();
    }

    inline juce::Image get(const juce::String& pluginId)
    {
        if (pluginId.isEmpty()) return {};
        const auto key = keyFor(pluginId);
        auto& c = cache();
        auto it = c.find(key);
        if (it != c.end()) return it->second;

        juce::Image img;
        for (auto* suffix : { ".u.png", ".png" })
        {
            const auto f = folder().getChildFile(key + suffix);
            if (f.existsAsFile())
            {
                img = juce::ImageFileFormat::loadFrom(f);
                if (img.isValid()) break;
            }
        }
        c[key] = img;
        return img;
    }

    inline bool setFromFile(const juce::String& pluginId, const juce::File& file)
    {
        auto img = downscale(juce::ImageFileFormat::loadFrom(file));
        if (! img.isValid()) return false;
        const auto key = keyFor(pluginId);
        save(img, folder().getChildFile(key + ".u.png"));
        cache()[key] = img;
        return true;
    }

    // From the editor: stored only when there is something to see and no chosen picture exists.
    inline bool setCaptured(const juce::String& pluginId, const juce::Image& captured)
    {
        if (pluginId.isEmpty() || hasUserImage(pluginId) || looksBlank(captured)) return false;
        auto img = downscale(captured);
        const auto key = keyFor(pluginId);
        save(img, folder().getChildFile(key + ".png"));
        cache()[key] = img;
        return true;
    }

    inline void clear(const juce::String& pluginId)
    {
        const auto key = keyFor(pluginId);
        folder().getChildFile(key + ".u.png").deleteFile();
        folder().getChildFile(key + ".png").deleteFile();
        cache()[key] = juce::Image();
    }
}
