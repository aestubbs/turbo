#ifndef TURBO_SPECDIALOG_H
#define TURBO_SPECDIALOG_H

#include <string>

// The "New Spec..." dialog (spec FR3): title (required; kebab-cased into the
// filename), domain and goal (optional; goal seeds the Objective section).
// Returns true on OK with a non-empty title.
bool executeNewSpecDialog(std::string &title, std::string &domain,
                          std::string &goal) noexcept;

#endif // TURBO_SPECDIALOG_H
