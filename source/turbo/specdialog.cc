#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TButton
#define Uses_TStaticText
#define Uses_TProgram
#define Uses_TDeskTop
#include <tvision/tv.h>

#include "specdialog.h"
#include "fieldinput.h"

namespace {

std::string trimmed(const char *s) noexcept
{
    std::string v = s ? s : "";
    size_t a = v.find_first_not_of(" \t");
    if (a == std::string::npos)
        return {};
    size_t b = v.find_last_not_of(" \t");
    return v.substr(a, b - a + 1);
}

} // namespace

bool executeNewSpecDialog(std::string &title, std::string &domain,
                          std::string &goal) noexcept
{
    auto *d = new TDialog(TRect(0, 0, 64, 12), "New Spec");
    d->options |= ofCentered;

    auto *titleLine = new FieldInputLine(TRect(12, 2, 61, 3), 128);
    d->insert(titleLine);
    d->insert(new TLabel(TRect(2, 2, 12, 3), "~T~itle:", titleLine));
    auto *domainLine = new FieldInputLine(TRect(12, 4, 61, 5), 64);
    d->insert(domainLine);
    d->insert(new TLabel(TRect(2, 4, 12, 5), "~D~omain:", domainLine));
    auto *goalLine = new FieldInputLine(TRect(12, 6, 61, 7), 256);
    d->insert(goalLine);
    d->insert(new TLabel(TRect(2, 6, 12, 7), "~G~oal:", goalLine));
    d->insert(new TStaticText(TRect(2, 8, 61, 9),
        "Created as specs/<kebab-cased-title>.md from the template."));

    d->insert(new TButton(TRect(38, 9, 48, 11), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(49, 9, 59, 11), "Cancel", cmCancel, bfNormal));

    seedInputLine(titleLine, title, 128);
    seedInputLine(domainLine, domain, 64);
    seedInputLine(goalLine, goal, 256);
    d->selectNext(False);

    bool ok = (TProgram::deskTop->execView(d) == cmOK);
    if (ok)
    {
        title = trimmed(titleLine->data);
        domain = trimmed(domainLine->data);
        goal = trimmed(goalLine->data);
    }
    TObject::destroy(d);
    return ok && !title.empty();
}
