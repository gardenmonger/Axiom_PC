/*
  ==============================================================================

    AppPaths.h

    Per-user data locations, resolved per platform:

      macOS    ~/Library/Application Support/Axiom
      Windows  %APPDATA%\Axiom
      Linux    ~/.config/Axiom  (JUCE userApplicationDataDirectory)

    Note: JUCE's userApplicationDataDirectory is "~/Library" on macOS, so
    the "Application Support" segment must be added explicitly here — this
    helper is the single source of truth for it.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace axiom::paths
{
    inline juce::File appDataRoot()
    {
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        base = base.getChildFile ("Application Support");
       #endif
        return base.getChildFile ("Axiom");
    }

    inline juce::File modelDirectory()
    {
        auto dir = appDataRoot().getChildFile ("Models");
        dir.createDirectory();
        return dir;
    }

    inline juce::File presetDirectory()
    {
        auto dir = appDataRoot().getChildFile ("Presets");
        dir.createDirectory();
        return dir;
    }
}
