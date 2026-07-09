/*
  ==============================================================================

    PresetManager.h

    File-based preset system. Each preset is one `.axiom` JSON file in
    ~/Library/Application Support/Axiom/Presets (per-user, survives
    reinstalls) holding the full InstrumentPatch plus preset metadata
    (`presetName`, `favorite`, `createdAt`) at the top level — exported
    .axiom instrument files load as presets too, since patch_io ignores
    unknown fields both ways.

    The list is kept sorted favorites-first, then alphabetical. A handful of
    factory presets are seeded on first run so the browser is never empty.

    Message-thread only (owned by the processor; the audio thread receives
    patches through the usual SynthEngine double buffer).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <optional>
#include <vector>

#include "../Core/AppPaths.h"
#include "../Core/DdspTimbre.h"
#include "../Core/InstrumentPatch.h"

namespace axiom
{

class PresetManager
{
public:
    struct PresetInfo
    {
        juce::String name;
        juce::File   file;
        bool         favorite = false;
    };

    PresetManager();

    static juce::File getPresetDirectory();

    void rescan();
    const std::vector<PresetInfo>& getPresets() const noexcept   { return presets; }

    int getCurrentIndex() const noexcept    { return currentIndex; }
    juce::String getCurrentName() const;
    void clearCurrent() noexcept            { currentIndex = -1; }
    void setCurrentByName (const juce::String& name);

    /** A preset is the patch plus (optionally) the DDSP resynthesis frames
        captured from the source sample — `ddsp` stays default/invalid for
        presets saved without one. */
    struct LoadedPreset
    {
        InstrumentPatch patch;
        DdspTimbre      ddsp;
    };

    std::optional<LoadedPreset> loadPreset (int index);

    /** Saves (or overwrites) `name` and makes it current. Pass the current
        DDSP timbre so "save captures the sound as heard" includes the
        resynthesis layer (nullptr / invalid = none stored). */
    juce::Result savePreset (const juce::String& name, const InstrumentPatch& patch,
                             const DdspTimbre* ddsp = nullptr);

    void setFavorite (int index, bool shouldBeFavorite);
    bool deletePreset (int index);

private:
    void seedFactoryPresets();
    juce::Result writePresetFile (const juce::File& file, const juce::String& name,
                                  const InstrumentPatch& patch, bool favorite,
                                  const DdspTimbre* ddsp = nullptr);

    std::vector<PresetInfo> presets;
    int currentIndex = -1;
};

} // namespace axiom
