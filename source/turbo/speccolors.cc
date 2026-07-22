#include "speccolors.h"

namespace turbo {

// Spec files (and every window that shows them) are a single deep-violet
// surface -- frame and text together, like the Lua windows' brown (D29
// revising D12) -- in two shades: brighter active, dimmer passive. Icons stay
// gold, tying the purple into the blue/gold palette.
namespace {
constexpr TColorDesired
    cSpecBgActive       = 0x2A1B4D, // active: deep violet (editor + frame)
    cSpecBgPassive      = 0x1F1838, // passive: darker, desaturated violet
    cSpecFrameFgActive  = 0xE6DFF5, // active frame text / box lines
    cSpecFrameFgPassive = 0xA79BC7, // passive frame text (dim lavender)
    cSpecIcon           = 0xE8C07D, // frame icons (gold), on the active violet
    cSpecBarTrough      = 0x1A1230, // scrollbar trough
    cSpecBarThumb       = 0x5B4691, // scrollbar slider
    cSpecBarArrows      = 0xCBB8F0, // scrollbar arrows
    cSpecBodyFg         = 0xD8D2EA; // body text on the violet surface

// BIOS fallbacks for classic 16-colour mode, which has no purple.
constexpr uchar
    biosMagenta = 0x5,
    biosWhite   = 0xF,
    biosYellow  = 0xE,
    biosGray    = 0x7;
} // namespace

bool specSurfaceIsBios() noexcept
{
    return ::getBack(windowSchemeActive[wndFrameActive]).isBIOS();
}

TColorDesired specSurfaceBg(bool active) noexcept
{
    if (specSurfaceIsBios())
        return TColorDesired(biosMagenta);
    return active ? cSpecBgActive : cSpecBgPassive;
}

TColorDesired specAccent() noexcept
{
    if (specSurfaceIsBios())
        return TColorDesired(biosYellow);
    return cSpecIcon;
}

TColorDesired specBodyFg() noexcept
{
    if (specSurfaceIsBios())
        return TColorDesired(biosWhite);
    return cSpecBodyFg;
}

const WindowColorScheme &specPurpleScheme() noexcept
{
    static WindowColorScheme purple;
    for (int i = 0; i < WindowPaletteItemCount; ++i)
        purple[i] = windowSchemeActive[i];
    if (specSurfaceIsBios())
    {
        ::setFore(purple[wndFramePassive], TColorDesired(biosGray));
        ::setBack(purple[wndFramePassive], TColorDesired(biosMagenta));
        ::setFore(purple[wndFrameActive], TColorDesired(biosWhite));
        ::setBack(purple[wndFrameActive], TColorDesired(biosMagenta));
        ::setFore(purple[wndFrameIcon], TColorDesired(biosYellow));
        ::setBack(purple[wndFrameIcon], TColorDesired(biosMagenta));
        return purple;
    }
    ::setFore(purple[wndFramePassive], cSpecFrameFgPassive);
    ::setBack(purple[wndFramePassive], cSpecBgPassive);
    ::setFore(purple[wndFrameActive], cSpecFrameFgActive);
    ::setBack(purple[wndFrameActive], cSpecBgActive);
    ::setFore(purple[wndFrameIcon], cSpecIcon);
    ::setBack(purple[wndFrameIcon], cSpecBgActive);
    ::setFore(purple[wndScrollBarPageArea], cSpecBarThumb);
    ::setBack(purple[wndScrollBarPageArea], cSpecBarTrough);
    ::setFore(purple[wndScrollBarControls], cSpecBarArrows);
    ::setBack(purple[wndScrollBarControls], cSpecBarTrough);
    return purple;
}

} // namespace turbo
