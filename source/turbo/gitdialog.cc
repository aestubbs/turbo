#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TRadioButtons
#define Uses_TListBox
#define Uses_TStringCollection
#define Uses_TSItem
#define Uses_TStaticText
#define Uses_TScrollBar
#define Uses_TProgram
#define Uses_TDeskTop
#define Uses_TEvent
#define Uses_TKeys
#define Uses_MsgBox
#include <tvision/tv.h>

#include "gitdialog.h"
#include "gitmanager.h"
#include "fieldinput.h"

#include <string>
#include <vector>

namespace {

// TCheckBoxes stores its state in a 32-bit mask, so we can show at most 32 files
// in the staging cluster. Larger change sets are capped (with a note); the user
// can stage the rest from the command line or in batches.
constexpr int kMaxFiles = 32;

const char *badge(GitFileState s)
{
    switch (s)
    {
        case GitFileState::Modified:   return "M ";
        case GitFileState::Added:      return "A ";
        case GitFileState::Deleted:    return "D ";
        case GitFileState::Renamed:    return "R ";
        case GitFileState::Untracked:  return "? ";
        case GitFileState::Conflicted: return "U ";
        default:                       return "  ";
    }
}

std::string relPath(const std::string &root, const std::string &abs)
{
    if (abs.size() > root.size() + 1 && abs.compare(0, root.size(), root) == 0)
        return abs.substr(root.size() + 1);
    return abs;
}

} // namespace

bool executeGitCommitDialog(
    GitManager &git,
    std::function<bool(const std::string &message)> beforeCommit,
    std::function<void(bool ok, const std::string &output)> afterCommit) noexcept
{
    const GitRepoStatus &st = git.currentStatus();

    // Collect changed files (absolute paths) with their status.
    std::vector<std::string> paths;       // absolute
    std::vector<bool> initiallyStaged;
    for (auto &kv : st.files)
    {
        paths.push_back(kv.first);
        initiallyStaged.push_back(kv.second.staged);
        if ((int) paths.size() >= kMaxFiles)
            break;
    }

    if (paths.empty())
    {
        messageBox("No changes to commit.", mfInformation | mfOKButton);
        return false;
    }

    int rows = (int) paths.size();
    int height = 5 + rows + 5; // note + checklist + gap + message + buttons + border
    if (height > 22) height = 22;
    auto *d = new TDialog(TRect(0, 0, 70, height), "Commit");
    d->options |= ofCentered;

    d->insert(new TStaticText(TRect(2, 2, 68, 3),
        "Check the files to include, enter a message, then Commit."));

    // Staging checklist (one TSItem per file, built back-to-front).
    TSItem *items = nullptr;
    for (int i = rows - 1; i >= 0; --i)
    {
        std::string label = std::string(badge(st.files.at(paths[i]).state)) +
                            relPath(st.root, paths[i]);
        items = new TSItem(label, items);
    }
    int listH = rows;
    if (listH > height - 9) listH = height - 9;
    auto *boxes = new TCheckBoxes(TRect(3, 4, 67, 4 + listH), items);
    d->insert(boxes);

    int msgY = 4 + listH + 1;
    d->insert(new TLabel(TRect(2, msgY, 20, msgY + 1), "~M~essage:", nullptr));
    auto *msg = new TInputLine(TRect(2, msgY + 1, 67, msgY + 2), 512);
    d->insert(msg);

    int by = height - 3;
    d->insert(new TButton(TRect(44, by, 56, by + 2), "~C~ommit", cmOK, bfDefault));
    d->insert(new TButton(TRect(57, by, 67, by + 2), "Cancel", cmCancel, bfNormal));

    // Pre-check the files that are already staged.
    uint32_t mark = 0;
    for (int i = 0; i < rows; ++i)
        if (initiallyStaged[i])
            mark |= (1u << i);
    boxes->setData(&mark);

    d->selectNext(False);

    bool committed = false;
    if (TProgram::deskTop->execView(d) == cmOK)
    {
        uint32_t chosen = 0;
        boxes->getData(&chosen);

        std::string message = msg->data;
        // Trim trailing whitespace.
        while (!message.empty() && (message.back() == ' ' || message.back() == '\t'))
            message.pop_back();

        std::vector<std::string> toStage, toUnstage;
        for (int i = 0; i < rows; ++i)
        {
            bool want = (chosen >> i) & 1u;
            if (want && !initiallyStaged[i])
                toStage.push_back(paths[i]);
            else if (!want && initiallyStaged[i])
                toUnstage.push_back(paths[i]);
        }

        if (message.empty())
            messageBox("Commit message is empty.", mfError | mfOKButton);
        else if (chosen == 0)
            messageBox("No files selected to commit.", mfError | mfOKButton);
        else if (beforeCommit && !beforeCommit(message))
            ; // a beforeCommit hook vetoed the commit; leave committed = false
        else
        {
            // Reconcile the index to match the checkboxes, then commit. These
            // run on the git worker thread in order; the commit reports result.
            if (!toUnstage.empty())
                git.unstage(toUnstage);
            if (!toStage.empty())
                git.stage(toStage);
            git.commit(message, [afterCommit] (int code, const std::string &output) {
                if (code != 0)
                {
                    std::string m = "git commit failed:\n" +
                        (output.empty() ? std::string("(see terminal)")
                                        : output.substr(0, 400));
                    messageBox(m.c_str(), mfError | mfOKButton);
                }
                if (afterCommit)
                    afterCommit(code == 0, output);
            });
            committed = true;
        }
    }

    TObject::destroy(d);
    return committed;
}

unsigned short executeBranchSwitchDialog(const char *branch) noexcept
{
    auto *d = new TDialog(TRect(0, 0, 60, 13), "Switch Branch");
    d->options |= ofCentered;

    std::string b = branch ? branch : "";
    std::string msg =
        "Switching to '" + b + "', but you have uncommitted changes.\n\n"
        "Stash & Switch: set them aside and re-apply on '" + b + "'.\n"
        "Force: discard them.\n"
        "Cancel: stay on the current branch.";
    d->insert(new TStaticText(TRect(3, 2, 57, 9), msg));

    int by = 10;
    d->insert(new TButton(TRect(3, by, 23, by + 2), "~S~tash & Switch", cmYes, bfDefault));
    d->insert(new TButton(TRect(24, by, 35, by + 2), "~F~orce", cmNo, bfNormal));
    d->insert(new TButton(TRect(45, by, 57, by + 2), "Cancel", cmCancel, bfNormal));

    unsigned short res = TProgram::deskTop->execView(d);
    TObject::destroy(d);
    return res;
}

bool executeNewBranchDialog(std::string &name) noexcept
{
    auto *d = new TDialog(TRect(0, 0, 56, 10), "New Branch");
    d->options |= ofCentered;

    d->insert(new TStaticText(TRect(2, 2, 54, 4),
        "Create a branch at the current commit and switch to "
        "it. Uncommitted changes come along."));

    auto *input = new FieldInputLine(TRect(9, 5, 54, 6), 128);
    d->insert(new TLabel(TRect(2, 5, 8, 6), "~N~ame:", input));
    d->insert(input);

    int by = 7;
    d->insert(new TButton(TRect(31, by, 43, by + 2), "C~r~eate", cmOK, bfDefault));
    d->insert(new TButton(TRect(44, by, 54, by + 2), "Cancel", cmCancel, bfNormal));

    d->selectNext(False);

    bool ok = false;
    if (TProgram::deskTop->execView(d) == cmOK)
    {
        std::string n = input->data;
        size_t a = n.find_first_not_of(" \t");
        size_t b = n.find_last_not_of(" \t");
        n = (a == std::string::npos) ? std::string() : n.substr(a, b - a + 1);
        if (n.empty())
            messageBox("Branch name is empty.", mfError | mfOKButton);
        else
        {
            name = n;
            ok = true;
        }
    }
    TObject::destroy(d);
    return ok;
}

bool executeMergeDialog(GitManager &git, std::string &branch, int &favor) noexcept
{
    const GitRepoStatus &st = git.currentStatus();
    std::vector<std::string> others;
    for (auto &b : st.branches)
        if (b != st.branch)
            others.push_back(b);
    if (others.empty())
    {
        messageBox("No other branches to merge.", mfInformation | mfOKButton);
        return false;
    }

    auto *d = new TDialog(TRect(0, 0, 58, 18), "Merge");
    d->options |= ofCentered;

    std::string head = st.branch.empty() ? std::string("the current branch")
                                         : ("'" + st.branch + "'");
    d->insert(new TStaticText(TRect(2, 1, 56, 2),
        ("Merge a branch into " + head + ":").c_str()));

    // Branch list (TStringCollection sorts the names; read the selection back by
    // index from the collection so display order and result stay in sync).
    auto *vsb = new TScrollBar(TRect(54, 3, 55, 9));
    d->insert(vsb);
    auto *coll = new TStringCollection((ccIndex) others.size(), 8);
    for (auto &b : others)
        coll->insert(newStr(b.c_str()));
    auto *list = new TListBox(TRect(3, 3, 54, 9), 1, vsb);
    list->newList(coll);
    d->insert(list);

    d->insert(new TLabel(TRect(2, 10, 22, 11), "Conflict ~s~trategy:", nullptr));
    auto *strat = new TRadioButtons(TRect(3, 11, 56, 14),
        new TSItem("~D~efault (resolve manually)",
        new TSItem("Favor ~o~urs (-X ours)",
        new TSItem("Favor ~t~heirs (-X theirs)", nullptr))));
    d->insert(strat);

    int by = 15;
    d->insert(new TButton(TRect(32, by, 44, by + 2), "~M~erge", cmOK, bfDefault));
    d->insert(new TButton(TRect(45, by, 56, by + 2), "Cancel", cmCancel, bfNormal));

    d->selectNext(False);

    bool ok = false;
    if (TProgram::deskTop->execView(d) == cmOK)
    {
        short sel = list->focused;
        if (sel >= 0 && sel < (short) coll->getCount())
        {
            branch = (const char *) coll->at(sel);
            ushort s = 0;
            strat->getData(&s);
            favor = (int) s;
            ok = true;
        }
    }
    TObject::destroy(d);     // destroys the listbox (which does not own the list)
    TObject::destroy(coll);  // free the strings ourselves
    return ok;
}
