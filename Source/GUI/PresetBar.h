/*
  ==============================================================================

    PresetBar.h

    Header preset strip:   ‹  [ Preset Name ▾ ]  ›  ♥  +

      ‹ / ›   step through presets (wraps)
      name    dropdown browser: Favorites section, All Presets, then
              Save / Delete / Open-folder actions
      ♥       toggle favorite on the current preset
      +       save the current patch as a new preset (name prompt)

    Dumb view: the editor injects state via refresh() and receives intent
    through the std::function callbacks — no processor coupling in GUI/.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace axiom
{

class PresetBar final : public juce::Component
{
public:
    PresetBar();

    std::function<void (int)>          onSelect;          // preset index
    std::function<void (int)>          onNavigate;        // -1 / +1
    std::function<void()>              onToggleFavorite;  // current preset
    std::function<void (juce::String)> onSave;            // new preset name
    std::function<void()>              onDeleteCurrent;
    std::function<void()>              onUpdateCurrent;   // overwrite with knob edits
    std::function<void()>              onRevert;          // discard knob edits

    void refresh (const juce::StringArray& names, const juce::Array<bool>& favorites,
                  int currentIndex, const juce::String& displayText);

    /** Marks the current preset as edited (name shows an asterisk; the menu
        gains Update / Revert). Polled by the editor as knobs move. */
    void setDirty (bool nowDirty);

    void resized() override;

private:
    void showMenu();
    void promptForName();

    juce::TextButton prevButton, nextButton, nameButton, favButton, addButton;

    void updateNameText();

    juce::StringArray  presetNames;
    juce::Array<bool>  presetFavorites;
    int                current = -1;
    bool               dirty = false;
    juce::String       suggestedName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};

} // namespace axiom
