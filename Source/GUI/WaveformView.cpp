#include "WaveformView.h"
#include "AxiomLookAndFeel.h"

namespace axiom
{

namespace
{
    constexpr float markerGrabPx = 7.0f;
    constexpr float fadeHandleR  = 4.5f;
    constexpr double minZoneSec  = 0.05;
}

WaveformView::WaveformView()
{
    formatManager.registerBasicFormats();   // WAV, AIFF, FLAC, MP3/AAC (CoreAudio/Windows Media)
    thumbnail.addChangeListener (this);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);

    zoomInButton.setTooltip ("Zoom in (or scroll on the waveform)");
    zoomOutButton.setTooltip ("Zoom out");
    zoomFitButton.setTooltip ("Show the whole file");

    zoomInButton.onClick  = [this] { zoomAroundTime ((viewStartSec() + viewEndSec()) * 0.5, 0.55); };
    zoomOutButton.onClick = [this] { zoomAroundTime ((viewStartSec() + viewEndSec()) * 0.5, 1.0 / 0.55); };
    zoomFitButton.onClick = [this] { setViewRange (0.0, 0.0); };

    for (auto* b : { &zoomInButton, &zoomOutButton, &zoomFitButton })
    {
        addChildComponent (b);              // shown once audio is loaded
        b->setWantsKeyboardFocus (false);
    }
}

WaveformView::~WaveformView()
{
    thumbnail.removeChangeListener (this);
}

//==============================================================================
void WaveformView::setAudio (const juce::AudioBuffer<float>& buffer, double sampleRate,
                             const juce::String& name)
{
    displayName = name;
    if (buffer.getNumSamples() > 0 && sampleRate > 0.0)
    {
        totalLengthSec = buffer.getNumSamples() / sampleRate;
        thumbnail.reset (buffer.getNumChannels(), sampleRate, buffer.getNumSamples());
        thumbnail.addBlock (0, buffer, 0, buffer.getNumSamples());
    }
    else
    {
        totalLengthSec = 0.0;
        thumbnail.clear();
    }
    viewStart = viewEnd = 0.0;
    updateZoomButtonVisibility();
    repaint();
}

void WaveformView::setFileAudio (const juce::File& file)
{
    displayName = file.getFileName();
    totalLengthSec = 0.0;                      // filled once the thumbnail loads
    if (file.existsAsFile())
        thumbnail.setSource (new juce::FileInputSource (file));
    else
        thumbnail.clear();
    viewStart = viewEnd = 0.0;
    updateZoomButtonVisibility();
    repaint();
}

void WaveformView::clearAudio()
{
    displayName.clear();
    totalLengthSec = 0.0;
    thumbnail.clear();
    zone = {};
    viewStart = viewEnd = 0.0;
    updateZoomButtonVisibility();
    repaint();
}

void WaveformView::setZone (const AudioZone& z, bool notify)
{
    zone = z;
    repaint();
    if (notify && onZoneChanged)
        onZoneChanged (zone);
}

void WaveformView::setStatus (const juce::String& newHeadline, const juce::String& newDetail,
                              float newProgress)
{
    if (headline == newHeadline && detail == newDetail
        && std::abs (progress - newProgress) < 0.005f)
        return;

    headline = newHeadline;
    detail   = newDetail;
    progress = newProgress;
    repaint();
}

//==============================================================================
juce::Rectangle<float> WaveformView::waveArea() const
{
    return getLocalBounds().toFloat().reduced (18.0f, 12.0f)
               .withTrimmedTop (26.0f).withTrimmedBottom (40.0f);
}

//==============================================================================
// View window (zoom)
//==============================================================================
double WaveformView::viewStartSec() const
{
    return juce::jlimit (0.0, totalLengthSec, viewStart);
}

double WaveformView::viewEndSec() const
{
    return viewEnd > viewStartSec() + 1.0e-6 ? juce::jmin (viewEnd, totalLengthSec)
                                             : totalLengthSec;
}

bool WaveformView::isZoomed() const
{
    return viewEndSec() - viewStartSec() < totalLengthSec - 1.0e-3;
}

void WaveformView::setViewRange (double startSec, double endSec)
{
    constexpr double minSpan = 0.02;                 // 20 ms floor: sample-fine edits

    double span = endSec - startSec;
    if (span <= 0.0 || span >= totalLengthSec - 1.0e-6)
    {
        viewStart = viewEnd = 0.0;                   // full view
    }
    else
    {
        span      = juce::jmax (minSpan, span);
        viewStart = juce::jlimit (0.0, totalLengthSec - span, startSec);
        viewEnd   = viewStart + span;
    }
    repaint();
}

void WaveformView::zoomAroundTime (double anchorSec, double factor)
{
    if (totalLengthSec <= 0.0)
        return;

    const double vs   = viewStartSec();
    const double span = viewEndSec() - vs;
    const double newSpan = span * factor;
    const double frac = span > 0.0 ? (anchorSec - vs) / span : 0.5;

    setViewRange (anchorSec - frac * newSpan, anchorSec + (1.0 - frac) * newSpan);
}

void WaveformView::updateZoomButtonVisibility()
{
    const bool show = totalLengthSec > 0.0;
    zoomInButton.setVisible (show);
    zoomOutButton.setVisible (show);
    zoomFitButton.setVisible (show);
}

void WaveformView::resized()
{
    auto row = getLocalBounds().reduced (16, 10).removeFromTop (20);
    zoomFitButton.setBounds (row.removeFromRight (36));
    row.removeFromRight (4);
    zoomOutButton.setBounds (row.removeFromRight (22));
    row.removeFromRight (4);
    zoomInButton.setBounds (row.removeFromRight (22));
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (totalLengthSec <= 0.0)
        return;

    if (std::abs (wheel.deltaY) > 1.0e-4f)
    {
        // Zoom around the cursor; a typical wheel notch ~= 20% zoom.
        zoomAroundTime (xToTime (e.position.x),
                        std::pow (2.0, (double) -wheel.deltaY * 3.0));
    }
    else if (std::abs (wheel.deltaX) > 1.0e-4f)
    {
        const double span = viewEndSec() - viewStartSec();
        setViewRange (viewStartSec() - wheel.deltaX * span,
                      viewEndSec()   - wheel.deltaX * span);
    }
}

//==============================================================================
float WaveformView::timeToX (double sec) const
{
    const auto area = waveArea();
    const double vs = viewStartSec();
    const double span = viewEndSec() - vs;
    return span > 0.0
               ? area.getX() + (float) ((sec - vs) / span) * area.getWidth()
               : area.getX();
}

double WaveformView::xToTime (float x) const
{
    const auto area = waveArea();
    const double vs = viewStartSec();
    const double span = viewEndSec() - vs;
    return area.getWidth() > 0.0f
               ? juce::jlimit (vs, viewEndSec(),
                               vs + (double) ((x - area.getX()) / area.getWidth()) * span)
               : vs;
}

WaveformView::Drag WaveformView::zoneHitTest (juce::Point<float> pos) const
{
    if (totalLengthSec <= 0.0 || ! waveArea().expanded (10.0f).contains (pos))
        return Drag::none;

    const auto area = waveArea();
    const float xStart = timeToX (zone.startSec);
    const float xEnd   = timeToX (zone.effectiveEnd (totalLengthSec));

    // Fade handles live at the top of the wave area, offset by fade length.
    const juce::Point<float> fadeInHandle  { timeToX (zone.startSec + zone.fadeInSec),
                                             area.getY() + 6.0f };
    const juce::Point<float> fadeOutHandle { timeToX (zone.effectiveEnd (totalLengthSec) - zone.fadeOutSec),
                                             area.getY() + 6.0f };

    if (pos.getDistanceFrom (fadeInHandle)  < fadeHandleR + 4.0f) return Drag::fadeIn;
    if (pos.getDistanceFrom (fadeOutHandle) < fadeHandleR + 4.0f) return Drag::fadeOut;
    if (std::abs (pos.x - xStart) < markerGrabPx)                 return Drag::zoneStart;
    if (std::abs (pos.x - xEnd)   < markerGrabPx)                 return Drag::zoneEnd;
    return Drag::none;
}

//==============================================================================
void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    dragMode  = zoneHitTest (e.position);
    zoneDirty = false;
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == Drag::none || totalLengthSec <= 0.0)
        return;

    const double t   = xToTime (e.position.x);
    const double end = zone.effectiveEnd (totalLengthSec);

    switch (dragMode)
    {
        case Drag::zoneStart:
            zone.startSec = juce::jlimit (0.0, end - minZoneSec, t);
            break;
        case Drag::zoneEnd:
            zone.endSec = juce::jlimit (zone.startSec + minZoneSec, totalLengthSec, t);
            break;
        case Drag::fadeIn:
            zone.fadeInSec = juce::jlimit (0.0, juce::jmin (2.0, (end - zone.startSec) * 0.5),
                                           t - zone.startSec);
            break;
        case Drag::fadeOut:
            zone.fadeOutSec = juce::jlimit (0.0, juce::jmin (2.0, (end - zone.startSec) * 0.5),
                                            end - t);
            break;
        case Drag::none: break;
    }
    zoneDirty = true;
    repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (dragMode != Drag::none)
    {
        dragMode = Drag::none;
        if (zoneDirty && onZoneChanged)
            onZoneChanged (zone);
        return;
    }
    // Click-to-import only while empty — once audio is loaded, stray clicks
    // during zone/zoom work must not open the file dialog.
    if (e.mouseWasClicked() && totalLengthSec <= 0.0)
        openFilePicker();
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (totalLengthSec > 0.0)
    {
        zone = {};
        repaint();
        if (onZoneChanged)
            onZoneChanged (zone);
    }
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    switch (zoneHitTest (e.position))
    {
        case Drag::zoneStart:
        case Drag::zoneEnd:
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Drag::fadeIn:
        case Drag::fadeOut:
            setMouseCursor (juce::MouseCursor::DraggingHandCursor); break;
        case Drag::none:
            setMouseCursor (juce::MouseCursor::PointingHandCursor); break;
    }
}

//==============================================================================
void WaveformView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    AxiomLookAndFeel::drawPanel (g, bounds, metrics::radiusCard);

    auto content = bounds.reduced (16.0f, 12.0f);

    if (dragActive)
    {
        g.setColour (palette::accent.withAlpha (0.10f));
        g.fillRoundedRectangle (bounds, metrics::radiusCard);
    }

    if (thumbnail.getTotalLength() > 0.0 && totalLengthSec > 0.0)
    {
        const auto area = waveArea();

        // Neural Blue waveform with a soft glow pass beneath the crisp one.
        g.setColour (palette::accent.withAlpha (0.18f));
        thumbnail.drawChannels (g, area.toNearestInt(), viewStartSec(), viewEndSec(), 1.0f);
        g.setColour (palette::accent.withAlpha (0.80f));
        thumbnail.drawChannels (g, area.toNearestInt(), viewStartSec(), viewEndSec(), 0.92f);

        // Zone overlay is drawn in view coordinates — clip so markers that
        // sit outside the zoomed window never smear across the panel.
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());

        const double end   = zone.effectiveEnd (totalLengthSec);
        const float xStart = timeToX (zone.startSec);
        const float xEnd   = timeToX (end);

        // Dim everything outside the zone.
        g.setColour (palette::background.withAlpha (0.72f));
        if (xStart > area.getX())
            g.fillRect (juce::Rectangle<float> (area.getX(), area.getY(),
                                                xStart - area.getX(), area.getHeight()));
        if (xEnd < area.getRight())
            g.fillRect (juce::Rectangle<float> (xEnd, area.getY(),
                                                area.getRight() - xEnd, area.getHeight()));

        // Fade curves (raised cosine) drawn inside the zone edges.
        auto drawFade = [&] (double fromSec, double lenSec, bool fadeIn)
        {
            if (lenSec <= 0.0005) return;
            juce::Path p;
            const int steps = 24;
            for (int i = 0; i <= steps; ++i)
            {
                const double frac = (double) i / steps;
                const double t    = fadeIn ? fromSec + frac * lenSec
                                           : fromSec - frac * lenSec;
                const float gain  = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) frac);
                const float y     = area.getBottom() - gain * area.getHeight();
                const float x     = timeToX (t);
                if (i == 0) p.startNewSubPath (x, area.getBottom());
                p.lineTo (x, y);
            }
            g.setColour (palette::accentAlt.withAlpha (0.85f));
            g.strokePath (p, juce::PathStrokeType (1.4f));
        };
        drawFade (zone.startSec, zone.fadeInSec, true);
        drawFade (end, zone.fadeOutSec, false);

        // Zone markers.
        g.setColour (palette::accent);
        g.fillRect (juce::Rectangle<float> (xStart - 1.0f, area.getY(), 2.0f, area.getHeight()));
        g.fillRect (juce::Rectangle<float> (xEnd - 1.0f,   area.getY(), 2.0f, area.getHeight()));

        // Marker caps + fade handles.
        auto cap = [&g] (float x, float y, juce::Colour c)
        {
            g.setColour (c);
            g.fillEllipse (x - fadeHandleR, y - fadeHandleR, fadeHandleR * 2, fadeHandleR * 2);
        };
        cap (xStart, area.getY(), palette::accent);
        cap (xEnd,   area.getY(), palette::accent);
        cap (timeToX (zone.startSec + zone.fadeInSec), area.getY() + 6.0f, palette::accentAlt);
        cap (timeToX (end - zone.fadeOutSec),          area.getY() + 6.0f, palette::accentAlt);

        g.restoreState();

        // Header line: name + zone + zoom info.
        g.setColour (palette::textDim);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        juce::String info = displayName + "   \xc2\xb7   "
                          + juce::String (totalLengthSec, 2) + " s";
        if (zone.startSec > 0.001 || zone.endSec > 0.0)
            info << "   \xc2\xb7   zone " << juce::String (zone.startSec, 2) << " - "
                 << juce::String (end, 2) << " s";
        if (isZoomed())
            info << "   \xc2\xb7   view " << juce::String (viewStartSec(), 2) << " - "
                 << juce::String (viewEndSec(), 2) << " s";
        g.drawText (info, content.removeFromTop (20.0f), juce::Justification::topLeft);
    }
    else
    {
        g.setColour (palette::textDim);
        g.setFont (juce::Font (juce::FontOptions (16.0f)));
        g.drawText ("Drop a sample or click to import",
                    content, juce::Justification::centred);
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawText ("WAV  AIFF  FLAC  MP3  a song to separate, or an isolated note",
                    content.translated (0.0f, 26.0f), juce::Justification::centred);
    }

    // Status overlay (bottom strip).
    if (headline.isNotEmpty())
    {
        auto strip = bounds.removeFromBottom (34.0f).reduced (14.0f, 4.0f);

        g.setColour (palette::text);
        g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
        g.drawText (headline, strip.removeFromLeft (strip.getWidth() * 0.5f),
                    juce::Justification::centredLeft);

        if (detail.isNotEmpty())
        {
            g.setColour (palette::textDim);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText (detail, strip, juce::Justification::centredRight);
        }

        if (progress >= 0.0f)
        {
            auto bar = bounds.removeFromBottom (4.0f).reduced (14.0f, 0.0f);
            g.setColour (palette::outline);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (palette::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress)), 2.0f);
        }
    }
}

//==============================================================================
bool WaveformView::hasSupportedExtension (const juce::String& path)
{
    return path.endsWithIgnoreCase (".wav") || path.endsWithIgnoreCase (".aif")
        || path.endsWithIgnoreCase (".aiff") || path.endsWithIgnoreCase (".flac")
        || path.endsWithIgnoreCase (".mp3") || path.endsWithIgnoreCase (".m4a");
}

bool WaveformView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (hasSupportedExtension (f))
            return true;
    return false;
}

void WaveformView::fileDragEnter (const juce::StringArray&, int, int)
{
    dragActive = true;
    repaint();
}

void WaveformView::fileDragExit (const juce::StringArray&)
{
    dragActive = false;
    repaint();
}

void WaveformView::filesDropped (const juce::StringArray& files, int, int)
{
    dragActive = false;
    repaint();
    for (auto& f : files)
    {
        if (hasSupportedExtension (f))
        {
            if (onFileSelected)
                onFileSelected (juce::File (f));
            break;
        }
    }
}

void WaveformView::openFilePicker()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Import a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file.existsAsFile() && onFileSelected)
                                  onFileSelected (file);
                          });
}

void WaveformView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // File-sourced thumbnails load asynchronously; adopt the length once known.
    if (totalLengthSec <= 0.0 && thumbnail.getTotalLength() > 0.0)
        totalLengthSec = thumbnail.getTotalLength();
    repaint();
}

} // namespace axiom
