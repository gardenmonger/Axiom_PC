#include "PresetBar.h"
#include "AxiomLookAndFeel.h"
#include "../Core/AppPaths.h"

namespace axiom
{

namespace
{
    const juce::String heartFull    = juce::String::fromUTF8 ("\xe2\x99\xa5");   // ♥
    const juce::String heartHollow  = juce::String::fromUTF8 ("\xe2\x99\xa1");   // ♡
    const juce::String arrowLeft    = juce::String::fromUTF8 ("\xe2\x80\xb9");   // ‹
    const juce::String arrowRight   = juce::String::fromUTF8 ("\xe2\x80\xba");   // ›

    enum MenuIds { idSave = 1, idDelete = 2, idOpenFolder = 3,
                   idUpdate = 4, idRevert = 5, idPresetBase = 1000 };
}

PresetBar::PresetBar()
{
    prevButton.setButtonText (arrowLeft);
    nextButton.setButtonText (arrowRight);
    addButton.setButtonText ("+");
    favButton.setButtonText (heartHollow);
    nameButton.setButtonText ("Init");

    prevButton.setTooltip ("Previous preset");
    nextButton.setTooltip ("Next preset");
    favButton.setTooltip ("Favorite this preset");
    addButton.setTooltip ("Save current sound as a preset");
    nameButton.setTooltip ("Browse presets");

    prevButton.onClick = [this] { if (onNavigate) onNavigate (-1); };
    nextButton.onClick = [this] { if (onNavigate) onNavigate (+1); };
    favButton.onClick  = [this] { if (onToggleFavorite) onToggleFavorite(); };
    addButton.onClick  = [this] { promptForName(); };
    nameButton.onClick = [this] { showMenu(); };

    for (auto* b : { &prevButton, &nextButton, &nameButton, &favButton, &addButton })
        addAndMakeVisible (b);
}

//==============================================================================
void PresetBar::refresh (const juce::StringArray& names, const juce::Array<bool>& favorites,
                         int currentIndex, const juce::String& displayText)
{
    presetNames     = names;
    presetFavorites = favorites;
    current         = currentIndex;
    suggestedName   = displayText;

    updateNameText();

    const bool hasCurrent = current >= 0 && current < presetFavorites.size();
    const bool isFav      = hasCurrent && presetFavorites[current];
    favButton.setButtonText (isFav ? heartFull : heartHollow);
    favButton.setColour (juce::TextButton::textColourOffId,
                         isFav ? palette::gold : palette::textDim);   // gold = premium state
    favButton.setEnabled (hasCurrent);

    prevButton.setEnabled (! presetNames.isEmpty());
    nextButton.setEnabled (! presetNames.isEmpty());
}

void PresetBar::setDirty (bool nowDirty)
{
    if (dirty == nowDirty)
        return;
    dirty = nowDirty;
    updateNameText();
}

void PresetBar::updateNameText()
{
    nameButton.setButtonText (dirty && current >= 0 ? suggestedName + " *" : suggestedName);
}

void PresetBar::resized()
{
    auto area = getLocalBounds();
    const int h = area.getHeight();

    prevButton.setBounds (area.removeFromLeft (h));
    area.removeFromLeft (4);
    addButton.setBounds (area.removeFromRight (h));
    area.removeFromRight (4);
    favButton.setBounds (area.removeFromRight (h));
    area.removeFromRight (4);
    nextButton.setBounds (area.removeFromRight (h));
    area.removeFromRight (4);
    nameButton.setBounds (area);
}

//==============================================================================
void PresetBar::showMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());

    bool anyFavorite = false;
    for (auto fav : presetFavorites)
        anyFavorite = anyFavorite || fav;

    if (anyFavorite)
    {
        menu.addSectionHeader ("FAVORITES");
        for (int i = 0; i < presetNames.size(); ++i)
            if (presetFavorites[i])
                menu.addItem (idPresetBase + i, heartFull + "  " + presetNames[i],
                              true, i == current);
        menu.addSeparator();
    }

    if (! presetNames.isEmpty())
    {
        menu.addSectionHeader ("ALL PRESETS");
        for (int i = 0; i < presetNames.size(); ++i)
            menu.addItem (idPresetBase + i, presetNames[i], true, i == current);
        menu.addSeparator();
    }

    if (dirty && current >= 0)
    {
        menu.addItem (idUpdate, "Update \"" + presetNames[current] + "\" with edits");
        menu.addItem (idRevert, "Revert to saved");
        menu.addSeparator();
    }

    menu.addItem (idSave, "Save preset");
    menu.addItem (idDelete, "Delete current preset", current >= 0);
    menu.addItem (idOpenFolder, "Show presets folder");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nameButton)
                            .withMinimumWidth (nameButton.getWidth()),
                        [this] (int result)
    {
        if (result >= idPresetBase)
        {
            if (onSelect) onSelect (result - idPresetBase);
        }
        else if (result == idSave)
        {
            promptForName();
        }
        else if (result == idUpdate)
        {
            if (onUpdateCurrent) onUpdateCurrent();
        }
        else if (result == idRevert)
        {
            if (onRevert) onRevert();
        }
        else if (result == idDelete && current >= 0)
        {
            const auto name = presetNames[current];
            juce::AlertWindow::showOkCancelBox (
                juce::MessageBoxIconType::QuestionIcon,
                "Delete Preset",
                "Delete \"" + name + "\"? This cannot be undone.",
                "Delete", "Cancel", this,
                juce::ModalCallbackFunction::create ([this] (int ok)
                {
                    if (ok == 1 && onDeleteCurrent)
                        onDeleteCurrent();
                }));
        }
        else if (result == idOpenFolder)
        {
            paths::presetDirectory().revealToUser();
        }
    });
}

void PresetBar::promptForName()
{
    auto* window = new juce::AlertWindow ("Save Preset",
                                          "Name this sound:",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", suggestedName.upToLastOccurrenceOf (" (unsaved)", false, false));
    window->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, window] (int result)
        {
            const auto name = window->getTextEditorContents ("name").trim();
            if (result == 1 && name.isNotEmpty() && onSave)
                onSave (name);
        }),
        true);   // delete the window when dismissed
}

} // namespace axiom
