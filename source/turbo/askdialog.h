#ifndef TURBO_ASKDIALOG_H
#define TURBO_ASKDIALOG_H

#include "asktypes.h"

// The ask_user wizard (specs/spec-workbench.md FR11/FR12): one page per
// question with Back/Next/Finish and Cancel, clearly attributed to the
// asking agent so agent-driven UI is never mistaken for the IDE's own.
// Fills 'answers' (one per question) and returns true on Finish; returns
// false when the user cancels — the caller must report that explicitly
// (never as empty answers). Answers survive Back/Next navigation.
bool executeAskUserWizard(const std::string &attribution,
                          const std::vector<AskQuestion> &questions,
                          std::vector<AskAnswer> &answers) noexcept;

#endif // TURBO_ASKDIALOG_H
