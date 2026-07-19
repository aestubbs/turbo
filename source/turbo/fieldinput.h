#ifndef TURBO_FIELDINPUT_H
#define TURBO_FIELDINPUT_H

#define Uses_TInputLine
#define Uses_TEvent
#define Uses_TKeys
#define Uses_TGroup
#include <tvision/tv.h>

#include <cstring>
#include <string>

// A plain TInputLine in this fork swallows Tab and Enter (its default key case
// turns them into spaces) before the dialog can use them, so neither focus
// movement nor "Enter = OK" works in a dialog with input fields. Route Tab to
// the focus chain and Enter to the dialog's default (OK) action.
//
// Shared by every dialog with input fields. Previously duplicated file-locally
// in builddialog.cc and gitdialog.cc, which only compiled because the two
// happened to fall into different unity batches; one shared definition removes
// that landmine.
struct FieldInputLine : public TInputLine
{
    FieldInputLine(const TRect &b, int maxLen) noexcept : TInputLine(b, maxLen) {}

    void handleEvent(TEvent &ev) override
    {
        if (ev.what == evKeyDown)
        {
            ushort key = ev.keyDown.keyCode;
            if (key == kbTab || key == kbShiftTab)
            {
                if (owner)
                    owner->selectNext(Boolean(key == kbShiftTab));
                clearEvent(ev);
                return;
            }
            if (key == kbEnter)
            {
                if (owner)
                    message(owner, evCommand, cmOK, nullptr);
                clearEvent(ev);
                return;
            }
        }
        TInputLine::handleEvent(ev);
    }
};

// Seed an input line from a std::string, respecting its capacity.
inline void seedInputLine(TInputLine *line, const std::string &val, int max) noexcept
{
    strncpy(line->data, val.c_str(), max - 1);
    line->data[max - 1] = '\0';
}

#endif // TURBO_FIELDINPUT_H
