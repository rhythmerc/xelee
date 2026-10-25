/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The RmlUi Options pages' rows (Audio, Graphics; audio_menu.h,
 * graphics_menu.h): a column of rows in Controls' gold line work, under
 * group headings, with Restore defaults at the bottom. Drawn into the native
 * Options page's frame (src/melee/mn/mnfight.c, which owns the frame, the
 * tab, the hint box and the input) over the running menu.
 *
 * A row is a bar (steps, left and right move it, a click sets a step), a
 * switch (Off and On), a choice (its values cycled by left, right and A) or
 * an action (A runs it). Up and down move between rows, wrapping. A row can
 * be unavailable (dimmed, inert) and carry a small note beside its value.
 * A page gives its rows as callbacks on its settings, so every change is
 * applied by the page as it happens.
 *
 * A row that applies only at launch says so while its value differs from
 * the one the game started with (its `pending`), and B on a page with such a
 * change asks first: Restart now (pc_restart) or Later, which goes back. It
 * asks again only once the waiting changes are different ones.
 */
#pragma once

#include <RmlUi/Core.h>

#include <functional>
#include <string>
#include <vector>

namespace pc::settings {

enum class Kind { Bar, Switch, Choice, Action };

struct Row {
    const char* group = nullptr; // a heading above this row
    const char* name = "";
    const char* hint = "";
    Kind kind = Kind::Switch;
    int count = 2;                         // a bar's steps; a choice's values
    std::function<int()> get;              // a bar's step, a switch's 0/1, a choice's index
    std::function<void(int)> set;          // an action's is called with 0
    std::function<std::string(int)> label; // a choice's value, a bar's readout
    std::function<bool()> available;       // unset: always
    std::function<const char*()> note;     // beside the value; unset or "" for none
    std::function<const char*()> hint_now; // in place of `hint` when it returns one
    std::function<bool()> tick;            // each frame while shown; true when it changed
    std::function<bool()> pending;         // applies at launch, and differs from the launch's
};

class Page : public Rml::EventListener {
public:
    // `document` is the RML in the resources folder; `rows` gives the rows at
    // each showing (the last, an action, is the bottom button).
    Page(const char* document, std::function<std::vector<Row>()> rows);

    void show(bool show);
    void presence(float presence) { presence_ = presence; }
    // PC_FIGHT_MENU_* (fight_menu.h); false when there was nothing to do.
    bool input(int what);
    const char* hint();
    void update();
    // Where the page asks to go, PC_OPTIONS_* (fight_menu.h), once: back
    // after the restart question's Later, or an action's (go).
    int leaving();
    // The next showing opens on the row that went (back from where it went).
    void go(int where) {
        go_ = where;
        resume_ = cursor_;
    }

    void ProcessEvent(Rml::Event& event) override;

private:
    bool load();
    void build();
    void refresh();
    void layout();
    void move(int dir);
    void change(int dir);
    void select();
    void set_step(int row, int step);
    bool live(int row) const;
    void changed();
    std::string waiting() const;
    void ask(bool open);
    void answer(int choice);

    const char* documentName_;
    std::function<std::vector<Row>()> makeRows_;
    Rml::ElementDocument* document_ = nullptr;
    std::vector<Row> rows_;
    bool shown_ = false;
    int cursor_ = 0;
    int hoverStep_ = -1;
    float presence_ = 0.f;
    Rml::Vector2i dims_{0, 0};
    float placedPresence_ = -1.f;
    Rml::Vector2i placedDims_{0, 0};
    bool pointerWas_ = false, pointerPressed_ = false;
    // The restart question: open, its cursor (0 Restart now, 1 Later), the
    // waiting changes last put off (waiting()), the answer.
    bool asking_ = false;
    int askCursor_ = 0;
    std::string putOff_;
    int go_ = 0, resume_ = -1;
    bool restarting_ = false;
};

} // namespace pc::settings
