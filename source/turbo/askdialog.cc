#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TButton
#define Uses_TStaticText
#define Uses_TRadioButtons
#define Uses_TCheckBoxes
#define Uses_TSItem
#define Uses_TProgram
#define Uses_TDeskTop
#include <tvision/tv.h>

#include "askdialog.h"
#include "fieldinput.h"

#include <algorithm>

namespace {

// Dialog-local command for the Back button (only live while modal, like
// builddialog's cmAddExtra block).
enum { cmWizardBack = 1394 };

constexpr int kDlgW = 66;       // dialog width
constexpr int kTextMax = 256;   // free-text capacity
constexpr int kMaxOptions = 16; // TCheckBoxes' mask is 32-bit; stay well under

// One wizard page. Returns cmOK (Next/Finish), cmWizardBack, or cmCancel,
// and updates 'answer' from the page's controls on any forward/back move.
ushort runPage(const std::string &attribution, const AskQuestion &q,
               size_t index, size_t count, AskAnswer &answer) noexcept
{
    int optCount = (int) std::min<size_t>(q.options.size(), kMaxOptions);
    bool hasText = q.freeText || optCount == 0;

    // Rows: frame(1) + from(1) + prompt(3) + gap(1) + options + text(2?) +
    // buttons(2) + frame(1).
    int optRows = optCount;
    int textRows = hasText ? 2 : 0;
    int height = 1 + 1 + 3 + 1 + optRows + (optRows ? 1 : 0) + textRows + 3;

    std::string title = "Agent Question";
    if (count > 1)
        title += " (" + std::to_string(index + 1) + "/" +
                 std::to_string(count) + ")";
    auto *d = new TDialog(TRect(0, 0, kDlgW, height), title.c_str());
    d->options |= ofCentered;

    int y = 2;
    // Attribution first (FR12): agent-driven UI is never mistaken for turbo's.
    std::string from = "From: " + attribution;
    d->insert(new TStaticText(TRect(2, y, kDlgW - 2, y + 1), from.c_str()));
    y += 1;
    d->insert(new TStaticText(TRect(2, y, kDlgW - 2, y + 3), q.prompt.c_str()));
    y += 4;

    TCluster *cluster = nullptr;
    if (optCount)
    {
        // TSItem chains are built back to front.
        TSItem *items = nullptr;
        for (int i = optCount - 1; i >= 0; --i)
            items = new TSItem(q.options[i].c_str(), items);
        TRect cr(2, y, kDlgW - 2, y + optRows);
        if (q.multiSelect)
            cluster = new TCheckBoxes(cr, items);
        else
            cluster = new TRadioButtons(cr, items);
        d->insert(cluster);
        y += optRows + 1;
        // Restore a previous visit's selection (Back/Next keeps answers).
        ushort mark = 0;
        for (int i = 0; i < optCount; ++i)
            if (std::find(answer.selected.begin(), answer.selected.end(),
                          q.options[i]) != answer.selected.end())
                mark |= (ushort) (q.multiSelect ? (1u << i) : (ushort) i + 1);
        if (q.multiSelect)
            cluster->setData(&mark);
        else if (mark)
        {
            ushort idx = (ushort) (mark - 1);
            cluster->setData(&idx);
        }
    }

    FieldInputLine *textLine = nullptr;
    if (hasText)
    {
        textLine = new FieldInputLine(TRect(11, y, kDlgW - 2, y + 1), kTextMax);
        d->insert(textLine);
        d->insert(new TLabel(TRect(2, y, 11, y + 1), "~A~nswer:", textLine));
        seedInputLine(textLine, answer.text, kTextMax);
        y += 2;
    }

    const char *okLabel = index + 1 == count ? "~F~inish" : "~N~ext";
    int bx = kDlgW - 2;
    d->insert(new TButton(TRect(bx - 10, y, bx, y + 2), "Cancel", cmCancel,
                          bfNormal));
    bx -= 11;
    d->insert(new TButton(TRect(bx - 10, y, bx, y + 2), okLabel, cmOK,
                          bfDefault));
    bx -= 11;
    if (index > 0)
        d->insert(new TButton(TRect(bx - 10, y, bx, y + 2), "~B~ack",
                              cmWizardBack, bfNormal));
    d->selectNext(False);

    ushort res = TProgram::deskTop->execView(d);

    if (res != cmCancel)
    {
        answer.selected.clear();
        if (cluster)
        {
            ushort mark = 0;
            cluster->getData(&mark);
            if (q.multiSelect)
            {
                for (int i = 0; i < optCount; ++i)
                    if (mark & (1u << i))
                        answer.selected.push_back(q.options[i]);
            }
            else if (mark < (ushort) optCount)
                answer.selected.push_back(q.options[mark]);
        }
        if (textLine)
            answer.text = textLine->data;
        else
            answer.text.clear();
    }
    TObject::destroy(d);
    return res;
}

} // namespace

bool executeAskUserWizard(const std::string &attribution,
                          const std::vector<AskQuestion> &questions,
                          std::vector<AskAnswer> &answers) noexcept
{
    answers.assign(questions.size(), AskAnswer {});
    size_t i = 0;
    while (i < questions.size())
    {
        ushort res = runPage(attribution, questions[i], i, questions.size(),
                             answers[i]);
        if (res == cmCancel)
            return false;
        if (res == cmWizardBack)
        {
            if (i > 0)
                --i;
            continue;
        }
        ++i;
    }
    return true;
}
