#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TButton
#define Uses_TStaticText
#define Uses_TRadioButtons
#define Uses_TSItem
#define Uses_TListViewer
#define Uses_TScrollBar
#define Uses_TProgram
#define Uses_TDeskTop
#define Uses_TEvent
#define Uses_TKeys
#include <tvision/tv.h>

#include "builddialog.h"
#include "buildconfig.h"
#include "fieldinput.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kCmdMax = 512;

// Dialog-local commands for the additional-commands list buttons. Handled by
// BuildDialog while it is modal, so they don't need to be globally unique.
enum { cmAddExtra = 1390, cmEditExtra, cmRemoveExtra };

std::string trim(const char *s) noexcept
{
    std::string v = s ? s : "";
    size_t a = v.find_first_not_of(" \t");
    if (a == std::string::npos)
        return {};
    size_t b = v.find_last_not_of(" \t");
    return v.substr(a, b - a + 1);
}

void seedInput(TInputLine *line, const std::string &val, int max) noexcept
{
    strncpy(line->data, val.c_str(), max - 1);
    line->data[max - 1] = '\0';
}

// Small modal to add/edit one {name, command} entry.
bool editCommandDialog(const char *title, BuildCommand &bc) noexcept
{
    auto *d = new TDialog(TRect(0, 0, 60, 9), title);
    d->options |= ofCentered;

    auto *nameLine = new FieldInputLine(TRect(13, 2, 57, 3), 64);
    d->insert(nameLine);
    d->insert(new TLabel(TRect(2, 2, 13, 3), "~N~ame:", nameLine));
    auto *cmdLine = new FieldInputLine(TRect(13, 4, 57, 5), kCmdMax);
    d->insert(cmdLine);
    d->insert(new TLabel(TRect(2, 4, 13, 5), "~C~ommand:", cmdLine));

    d->insert(new TButton(TRect(34, 6, 44, 8), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(45, 6, 55, 8), "Cancel", cmCancel, bfNormal));

    seedInput(nameLine, bc.name, 64);
    seedInput(cmdLine, bc.command, kCmdMax);
    d->selectNext(False);

    bool ok = (TProgram::deskTop->execView(d) == cmOK);
    if (ok)
    {
        bc.name = trim(nameLine->data);
        bc.command = trim(cmdLine->data);
    }
    TObject::destroy(d);
    return ok && !bc.command.empty();
}

// Scrolling list of the additional commands (name + command).
struct CmdListView : public TListViewer
{
    std::vector<BuildCommand> *cmds;

    CmdListView(const TRect &b, TScrollBar *vsb, std::vector<BuildCommand> *c) noexcept :
        TListViewer(b, 1, nullptr, vsb), cmds(c)
    {
        setRange(c ? (short) c->size() : 0);
    }

    void getText(char *dst, short item, short maxLen) override
    {
        if (cmds && item >= 0 && item < (short) cmds->size())
        {
            const BuildCommand &bc = (*cmds)[item];
            std::string s = bc.name.empty() ? bc.command : (bc.name + ":  " + bc.command);
            strncpy(dst, s.c_str(), maxLen);
            dst[maxLen] = '\0';
        }
        else if (dst)
            dst[0] = '\0';
    }

    void refresh() noexcept
    {
        setRange(cmds ? (short) cmds->size() : 0);
        drawView();
    }
};

struct BuildDialog : public TDialog
{
    TInputLine *buildLine, *testLine, *runLine, *artifactLine;
    TRadioButtons *modeBox;

    BuildDialog(const BuildConfig &cfg) noexcept;
    void writeBack(BuildConfig &cfg) const noexcept;
};

BuildDialog::BuildDialog(const BuildConfig &cfg) noexcept :
    TWindowInit(&TDialog::initFrame),
    TDialog(TRect(0, 0, 72, 17), "Build Configuration")
{
    options |= ofCentered;

    insert(new TStaticText(TRect(2, 2, 70, 3),
        "Commands run from the project root. Saved to .turbo/config.json."));

    buildLine = new FieldInputLine(TRect(13, 4, 69, 5), kCmdMax);
    insert(buildLine);
    insert(new TLabel(TRect(2, 4, 13, 5), "~B~uild:", buildLine));
    testLine = new FieldInputLine(TRect(13, 5, 69, 6), kCmdMax);
    insert(testLine);
    insert(new TLabel(TRect(2, 5, 13, 6), "~T~est:", testLine));
    runLine = new FieldInputLine(TRect(13, 6, 69, 7), kCmdMax);
    insert(runLine);
    insert(new TLabel(TRect(2, 6, 13, 7), "~R~un:", runLine));
    artifactLine = new FieldInputLine(TRect(13, 7, 69, 8), kCmdMax);
    insert(artifactLine);
    insert(new TLabel(TRect(2, 7, 13, 8), "Art~i~fact:", artifactLine));

    modeBox = new TRadioButtons(TRect(13, 9, 69, 12),
        new TSItem("A~u~to (build if artifact is stale)",
        new TSItem("Always ~b~uild first",
        new TSItem("R~u~n only", nullptr))));
    insert(modeBox);
    insert(new TLabel(TRect(2, 9, 13, 10), "On Run:", modeBox));

    insert(new TStaticText(TRect(2, 12, 70, 13),
        "Tool processes are configured separately (Run > Tools)."));

    insert(new TButton(TRect(48, 14, 58, 16), "O~K~", cmOK, bfDefault));
    insert(new TButton(TRect(59, 14, 69, 16), "Cancel", cmCancel, bfNormal));

    // Seed from the current config.
    seedInput(buildLine, cfg.build, kCmdMax);
    seedInput(testLine, cfg.test, kCmdMax);
    seedInput(runLine, cfg.run, kCmdMax);
    seedInput(artifactLine, cfg.artifact, kCmdMax);
    ushort mode = (cfg.runMode == "build") ? 1 : (cfg.runMode == "run") ? 2 : 0;
    modeBox->setData(&mode);

    selectNext(False);
}

void BuildDialog::writeBack(BuildConfig &cfg) const noexcept
{
    cfg.build = trim(buildLine->data);
    cfg.test = trim(testLine->data);
    cfg.run = trim(runLine->data);
    cfg.artifact = trim(artifactLine->data);
    ushort mode = 0;
    modeBox->getData(&mode);
    cfg.runMode = (mode == 1) ? "build" : (mode == 2) ? "run" : "auto";
}

// The tool-processes dialog: a scrolling list of {name, command} entries with
// Add/Edit/Remove. These are the long-running commands the user toggles on/off
// from the Run menu, each streaming into its own tab in the Output pane.
struct ToolsDialog : public TDialog
{
    CmdListView *list;
    std::vector<BuildCommand> tools; // working copy

    ToolsDialog(const std::vector<BuildCommand> &initial) noexcept;
    void handleEvent(TEvent &ev) override;
};

ToolsDialog::ToolsDialog(const std::vector<BuildCommand> &initial) noexcept :
    TWindowInit(&TDialog::initFrame),
    TDialog(TRect(0, 0, 72, 18), "Tool Processes"),
    tools(initial)
{
    options |= ofCentered;

    insert(new TStaticText(TRect(2, 2, 70, 4),
        "Long-running commands you start and stop from the Run menu (e.g. a dev "
        "server). Each streams its output to its own tab in the Output pane. "
        "Saved to .turbo/config.json; run from the project root."));

    auto *vsb = new TScrollBar(TRect(51, 5, 52, 15));
    insert(vsb);
    list = new CmdListView(TRect(2, 5, 51, 15), vsb, &tools);
    insert(list);
    insert(new TButton(TRect(53, 5, 69, 7), "~A~dd...", cmAddExtra, bfNormal));
    insert(new TButton(TRect(53, 7, 69, 9), "~E~dit...", cmEditExtra, bfNormal));
    insert(new TButton(TRect(53, 9, 69, 11), "Re~m~ove", cmRemoveExtra, bfNormal));

    insert(new TButton(TRect(48, 15, 58, 17), "O~K~", cmOK, bfDefault));
    insert(new TButton(TRect(59, 15, 69, 17), "Cancel", cmCancel, bfNormal));

    selectNext(False);
}

void ToolsDialog::handleEvent(TEvent &ev)
{
    if (ev.what == evCommand)
    {
        switch (ev.message.command)
        {
            case cmAddExtra:
            {
                BuildCommand bc;
                if (editCommandDialog("Add Tool", bc))
                {
                    tools.push_back(bc);
                    list->refresh();
                }
                clearEvent(ev);
                return;
            }
            case cmEditExtra:
            {
                int i = list->focused;
                if (i >= 0 && i < (int) tools.size())
                {
                    BuildCommand bc = tools[i];
                    if (editCommandDialog("Edit Tool", bc))
                    {
                        tools[i] = bc;
                        list->refresh();
                    }
                }
                clearEvent(ev);
                return;
            }
            case cmRemoveExtra:
            {
                int i = list->focused;
                if (i >= 0 && i < (int) tools.size())
                {
                    tools.erase(tools.begin() + i);
                    list->refresh();
                }
                clearEvent(ev);
                return;
            }
        }
    }
    TDialog::handleEvent(ev);
}

} // namespace

bool executeBuildDialog(BuildConfig &config) noexcept
{
    auto *d = new BuildDialog(config);
    bool ok = (TProgram::deskTop->execView(d) == cmOK);
    if (ok)
        d->writeBack(config);
    TObject::destroy(d);
    return ok;
}

bool executeToolsDialog(std::vector<BuildCommand> &tools) noexcept
{
    auto *d = new ToolsDialog(tools);
    bool ok = (TProgram::deskTop->execView(d) == cmOK);
    if (ok)
        tools = d->tools;
    TObject::destroy(d);
    return ok;
}
