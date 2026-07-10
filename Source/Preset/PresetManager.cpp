#include "PresetManager.h"

namespace axiom
{

//==============================================================================
PresetManager::PresetManager()
{
    seedFactoryPresets();
    rescan();
}

juce::File PresetManager::getPresetDirectory()
{
    return paths::presetDirectory();
}

//==============================================================================
void PresetManager::rescan()
{
    const auto currentFile = (currentIndex >= 0 && currentIndex < (int) presets.size())
                                 ? presets[(size_t) currentIndex].file : juce::File();

    presets.clear();
    for (auto& f : getPresetDirectory().findChildFiles (juce::File::findFiles, false, "*.axiom"))
    {
        const auto parsed = juce::JSON::parse (f.loadFileAsString());
        if (! parsed.isObject())
            continue;

        PresetInfo info;
        info.file     = f;
        info.name     = parsed.getProperty ("presetName", f.getFileNameWithoutExtension()).toString();
        info.favorite = (bool) parsed.getProperty ("favorite", false);
        presets.push_back (info);
    }

    // Favorites first, then case-insensitive alphabetical.
    std::sort (presets.begin(), presets.end(), [] (const PresetInfo& a, const PresetInfo& b)
    {
        if (a.favorite != b.favorite)
            return a.favorite;
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    currentIndex = -1;
    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].file == currentFile)
            currentIndex = i;
}

juce::String PresetManager::getCurrentName() const
{
    return currentIndex >= 0 && currentIndex < (int) presets.size()
               ? presets[(size_t) currentIndex].name : juce::String();
}

void PresetManager::setCurrentByName (const juce::String& name)
{
    currentIndex = -1;
    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].name == name)
            currentIndex = i;
}

//==============================================================================
std::optional<PresetManager::LoadedPreset> PresetManager::loadPreset (int index)
{
    if (index < 0 || index >= (int) presets.size())
        return std::nullopt;

    const auto parsed = juce::JSON::parse (presets[(size_t) index].file.loadFileAsString());
    if (! parsed.isObject())
        return std::nullopt;

    LoadedPreset preset;
    preset.patch = patch_io::fromVar (parsed);

    // Optional DDSP / SK-1 layers; absent (or corrupt) blocks stay invalid,
    // which the engine treats as "that layer unavailable".
    if (auto ddsp = DdspTimbre::fromBase64 (parsed.getProperty ("ddspTimbre", {}).toString()))
        preset.ddsp = std::move (*ddsp);
    if (auto sk = SamplerSource::fromBase64 (parsed.getProperty ("samplerSource", {}).toString()))
        preset.sampler = std::move (*sk);

    currentIndex = index;
    return preset;
}

juce::Result PresetManager::savePreset (const juce::String& name, const InstrumentPatch& patch,
                                        const DdspTimbre* ddsp, const SamplerSource* sampler)
{
    const auto trimmed = name.trim();
    if (trimmed.isEmpty())
        return juce::Result::fail ("Preset name is empty.");

    const auto file = getPresetDirectory()
                          .getChildFile (juce::File::createLegalFileName (trimmed) + ".axiom");

    // Preserve the favorite flag when overwriting an existing preset.
    bool favorite = false;
    if (file.existsAsFile())
        favorite = (bool) juce::JSON::parse (file.loadFileAsString()).getProperty ("favorite", false);

    if (auto result = writePresetFile (file, trimmed, patch, favorite, ddsp, sampler); result.failed())
        return result;

    rescan();
    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].file == file)
            currentIndex = i;

    return juce::Result::ok();
}

void PresetManager::setFavorite (int index, bool shouldBeFavorite)
{
    if (index < 0 || index >= (int) presets.size())
        return;

    auto& info = presets[(size_t) index];
    auto parsed = juce::JSON::parse (info.file.loadFileAsString());
    if (auto* obj = parsed.getDynamicObject())
    {
        obj->setProperty ("favorite", shouldBeFavorite);
        info.file.replaceWithText (juce::JSON::toString (parsed, false));
    }

    const auto keepFile = info.file;
    rescan();
    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].file == keepFile)
            currentIndex = i;
}

bool PresetManager::deletePreset (int index)
{
    if (index < 0 || index >= (int) presets.size())
        return false;

    const bool ok = presets[(size_t) index].file.deleteFile();
    currentIndex = -1;
    rescan();
    return ok;
}

//==============================================================================
juce::Result PresetManager::writePresetFile (const juce::File& file, const juce::String& name,
                                             const InstrumentPatch& patch, bool favorite,
                                             const DdspTimbre* ddsp, const SamplerSource* sampler)
{
    auto v = patch_io::toVar (patch);
    if (auto* obj = v.getDynamicObject())
    {
        obj->setProperty ("presetName", name);
        obj->setProperty ("favorite", favorite);
        obj->setProperty ("createdAt", juce::Time::getCurrentTime().toISO8601 (true));

        // DDSP frames and the SK-1 sample ride along as base64 so the preset
        // reproduces those layers; old builds simply ignore the extra keys.
        if (ddsp != nullptr && ddsp->isValid())
            obj->setProperty ("ddspTimbre", ddsp->toBase64());
        if (sampler != nullptr && sampler->isValid())
            obj->setProperty ("samplerSource", sampler->toBase64());
    }

    if (! file.replaceWithText (juce::JSON::toString (v, false)))
        return juce::Result::fail ("Could not write " + file.getFullPathName());
    return juce::Result::ok();
}

//==============================================================================
void PresetManager::seedFactoryPresets()
{
    auto dir = getPresetDirectory();
    if (! dir.findChildFiles (juce::File::findFiles, false, "*.axiom").isEmpty())
        return;

    auto make = [this] (const juce::String& name, const InstrumentPatch& p)
    {
        writePresetFile (getPresetDirectory()
                             .getChildFile (juce::File::createLegalFileName (name) + ".axiom"),
                         name, p, false);
    };

    {
        InstrumentPatch p;                       // Init Saw
        p.oscs[0] = { true, OscType::Saw, 0.9f, 0.0f, 0, 0.5f, 0.0f };
        p.filter.cutoffHz = 9000.0f;
        make ("Init Saw", p);
    }
    {
        InstrumentPatch p;                       // Supersaw Lead
        p.oscs[0] = { true, OscType::Supersaw, 0.9f, 18.0f, 0, 0.5f, 0.8f };
        p.filter.cutoffHz = 6500.0f;
        p.filter.resonance = 0.18f;
        p.ampEnv = { 0.004f, 0.25f, 0.85f, 0.25f };
        p.fx.chorusMix = 0.25f;
        p.fx.stereoWidth = 1.6f;
        p.character = { 0.8f, 0.3f, 0.6f, 0.3f };
        make ("Supersaw Lead", p);
    }
    {
        InstrumentPatch p;                       // Deep Sub
        p.oscs[0] = { true, OscType::Sine, 0.95f, 0.0f, 0, 0.5f, 0.0f };
        p.oscs[1] = { true, OscType::Sine, 0.35f, 0.0f, -1, 0.5f, 0.0f };
        p.filter.cutoffHz = 900.0f;
        p.ampEnv = { 0.005f, 0.3f, 0.9f, 0.18f };
        p.fx.satDrive = 0.15f;
        p.character = { 0.1f, 0.95f, 0.05f, 0.05f };
        make ("Deep Sub", p);
    }
    {
        InstrumentPatch p;                       // Glass Pluck
        p.oscs[0] = { true, OscType::Pulse, 0.8f, 0.0f, 0, 0.28f, 0.0f };
        p.oscs[1] = { true, OscType::Sine, 0.3f, 0.0f, 1, 0.5f, 0.0f };
        p.filter.cutoffHz = 1200.0f;
        p.filter.resonance = 0.3f;
        p.filter.envAmountSt = 30.0f;
        p.ampEnv    = { 0.002f, 0.35f, 0.0f, 0.4f };
        p.filterEnv = { 0.001f, 0.16f, 0.0f, 0.3f };
        p.fx.reverbMix = 0.22f;
        p.character = { 0.7f, 0.4f, 0.2f, 0.4f };
        make ("Glass Pluck", p);
    }
    {
        InstrumentPatch p;                       // Warm Pad
        p.oscs[0] = { true, OscType::Saw, 0.6f, -7.0f, 0, 0.5f, 0.0f };
        p.oscs[1] = { true, OscType::Saw, 0.6f,  7.0f, 0, 0.5f, 0.0f };
        p.filter.cutoffHz = 2200.0f;
        p.ampEnv = { 0.7f, 0.5f, 0.85f, 1.4f };
        p.lfo = { 0.4f, 0.0f, 2.5f };
        p.fx.chorusMix = 0.35f;
        p.fx.reverbMix = 0.3f;
        p.fx.reverbSize = 0.7f;
        p.fx.stereoWidth = 1.4f;
        p.character = { 0.4f, 0.8f, 0.5f, 0.3f };
        make ("Warm Pad", p);
    }
}

} // namespace axiom
