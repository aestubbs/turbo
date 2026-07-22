#ifndef TURBO_SPECCOLORS_H
#define TURBO_SPECCOLORS_H

#include <tvision/tv.h>

#include <turbo/basicwindow.h> // WindowColorScheme

// The spec surface: one deep-violet identity shared by every window that shows
// spec material -- the editor for a file under specs/, the Spec Manager, and
// (from M3) the Spec Workbench container. D29 of specs/spec-workbench.md made
// frames part of the surface rather than leaving them Turbo blue; keeping the
// palette in one place is what stops the three windows drifting apart.
//
// Classic 16-colour mode has no purple, so everything falls back to BIOS
// magenta with white/yellow accents.
//
// Deliberately a .cc + header rather than an inline/static in a header: the
// scheme holds a function-local static, and duplicating that across unity
// batches is exactly the landmine D21(a) removed for FieldInputLine.

namespace turbo {

// The editor/pane text background for a spec surface. Returns BIOS magenta in
// classic 16-colour mode.
TColorDesired specSurfaceBg(bool active) noexcept;

// True when the active theme is the classic 16-colour (BIOS) one.
bool specSurfaceIsBios() noexcept;

// Window chrome (frame, icons, scrollbars) recoloured to the spec violet.
// Rebuilt from windowSchemeActive on each call so it tracks theme edits for
// the entries it does not override.
const WindowColorScheme &specPurpleScheme() noexcept;

// Accent used for headings and emphasis on the violet surface: gold, tying
// the purple into the classic blue/gold palette exactly as the frame icons do.
TColorDesired specAccent() noexcept;

// Body text on the violet surface.
TColorDesired specBodyFg() noexcept;

} // namespace turbo

#endif // TURBO_SPECCOLORS_H
