/*
  ==============================================================================

    WaveformView.h

    Centre stage of the editor: drop zone + waveform + pipeline status
    overlay + ZONE EDITING.

    The zone defines the exact slice of audio the AI learns from:
      - start / end markers (drag the vertical lines) isolate and shorten
        the region fed to analysis
      - fade handles (dots at the top of each marker, drag horizontally)
        apply raised-cosine fades so the slice never pops or clicks
      - double-click resets the zone to the full length

    The view displays whatever buffer the processor says is active (full
    mix or a separated stem) via setAudio(); onZoneChanged fires on mouse
    release so the pipeline can re-learn the new slice.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../Core/AudioZone.h"

namespace axiom
{

//==============================================================================
class WaveformView final : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           private juce::ChangeListener
{
public:
    WaveformView();
    ~WaveformView() override;

    /** Fired on the message thread when the user picks or drops a file. */
    std::function<void (const juce::File&)> onFileSelected;

    /** Fired on mouse-release after any zone edit. */
    std::function<void (const AudioZone&)> onZoneChanged;

    /** Shows an in-memory buffer (active stem or full mix). */
    void setAudio (const juce::AudioBuffer<float>& buffer, double sampleRate,
                   const juce::String& displayName);

    /** Fallback for restored sessions where only the file path survives. */
    void setFileAudio (const juce::File& file);
    void clearAudio();

    void setZone (const AudioZone& z, bool notify = false);
    AudioZone getZone() const noexcept       { return zone; }

    /** Pipeline overlay. progress < 0 hides the bar. */
    void setStatus (const juce::String& headline, const juce::String& detail,
                    float progress);

    void openFilePicker();

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** Scroll = zoom around the cursor; horizontal scroll = pan. */
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Drag { none, zoneStart, zoneEnd, fadeIn, fadeOut };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    static bool hasSupportedExtension (const juce::String& path);

    juce::Rectangle<float> waveArea() const;
    float timeToX (double sec) const;
    double xToTime (float x) const;
    Drag zoneHitTest (juce::Point<float> pos) const;

    // --- View window (zoom) -------------------------------------------------
    double viewStartSec() const;
    double viewEndSec() const;
    bool   isZoomed() const;
    void   setViewRange (double startSec, double endSec);
    void   zoomAroundTime (double anchorSec, double factor);
    void   updateZoomButtonVisibility();

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache { 8 };
    juce::AudioThumbnail thumbnail { 512, formatManager, thumbnailCache };

    std::unique_ptr<juce::FileChooser> chooser;

    juce::String displayName;
    double       totalLengthSec = 0.0;

    // View window; viewEnd <= 0 means "show everything".
    double viewStart = 0.0, viewEnd = 0.0;
    juce::TextButton zoomInButton { "+" }, zoomOutButton { "-" }, zoomFitButton { "Fit" };

    AudioZone zone;
    Drag      dragMode = Drag::none;
    bool      zoneDirty = false;

    juce::String headline, detail;
    float        progress   = -1.0f;
    bool         dragActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace axiom
