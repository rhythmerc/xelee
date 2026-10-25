/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The RmlUi Options pages' rows (settings_page.hpp). */
#include "settings_page.hpp"

#include "menu_screen.h"
#include "launcher.h"
#include "pc.h"

#include <aurora/rmlui.hpp>
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" void lbAudioAx_80024030(int);

namespace pc::settings {
namespace {

enum { SFX_BACK = 0, SFX_CONFIRM = 1, SFX_MOVE = 2 };

// The layout, in px, scaled to fit the panel frame's inside, as Controls'.
constexpr float kScreenW = 1120.f, kScreenH = 660.f;
constexpr float kRowW = 900.f, kRowGap = 8.f, kGroupH = 38.f;
constexpr float kRowX = (kScreenW - kRowW) / 2;
constexpr float kRowMaxH = 64.f, kRowMinH = 40.f, kCompactH = 56.f;
constexpr float kPad = 22.f;           // a row's inside, each side
constexpr float kStepPitch = 21.f, kStepW = 16.f;
constexpr float kReadoutW = 72.f;      // a bar's percent
constexpr float kChipW = 96.f;         // a switch's Off and On
constexpr float kChoiceW = 330.f;      // a choice's value between its arrows
constexpr float kResetW = 260.f, kResetH = 42.f;
constexpr float kAskW = 640.f, kAskH = 250.f; // the restart question's box

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

int attr_of(Rml::Element* e, const char* name) {
    for (; e != nullptr; e = e->GetParentNode())
        if (const Rml::Variant* v = e->GetAttribute(name))
            return std::atoi(v->Get<Rml::String>().c_str());
    return -1;
}

} // namespace

Page::Page(const char* document, std::function<std::vector<Row>()> rows)
    : documentName_(document), makeRows_(std::move(rows)) {}

bool Page::load() {
    if (document_ != nullptr)
        return true;
    auto* context = aurora::rmlui::get_context();
    if (context == nullptr)
        return false;
    const std::string dir = pc_resources_dir();
    for (const char* face : {"menu.ttf", "menu-bold.ttf"})
        Rml::LoadFontFace(dir + face);
    document_ = context->LoadDocument(dir + documentName_);
    if (document_ == nullptr) {
        pc_log_line("options page: could not load %s", documentName_);
        return false;
    }
    document_->AddEventListener(Rml::EventId::Click, this);
    document_->AddEventListener(Rml::EventId::Mouseover, this);
    return true;
}

bool Page::live(int row) const {
    const Row& r = rows_[row];
    return !r.available || r.available();
}

void Page::build() {
    rows_ = makeRows_();
    const int n = static_cast<int>(rows_.size()) - 1; // the last is the bottom button
    int groups = 0;
    for (int i = 0; i < n; i++)
        groups += rows_[i].group != nullptr;
    const float top = 8.f, bottom = kScreenH - kResetH - 14.f - 16.f;
    const float headings = groups > 0 ? groups * kGroupH - 8.f + (groups - 1) * 12.f : 0.f;
    const float rowH = std::clamp((bottom - top - headings) / std::max(n, 1) - kRowGap, kRowMinH, kRowMaxH);
    const bool compact = rowH < kCompactH;

    std::string rml;
    float y = top;
    for (int i = 0; i < n; i++) {
        const Row& r = rows_[i];
        const std::string id = std::to_string(i);
        if (r.group != nullptr) {
            if (i > 0)
                y += 12.f;
            rml += "<div class=\"group\" style=\"left: " + fmt(kRowX) + "px; top: " + fmt(y) + "px; width: " +
                   fmt(kRowW) + "px;\"><div class=\"rule\"></div><div class=\"label\">" + r.group + "</div></div>";
            y += kGroupH - 8.f;
        }
        const float mid = rowH / 2;
        rml += "<div id=\"row-" + id + "\" class=\"row" + std::string(compact ? " compact" : "") + "\" data-row=\"" +
               id + "\" style=\"left: " + fmt(kRowX) + "px; top: " + fmt(y) + "px; width: " + fmt(kRowW) +
               "px; height: " + fmt(rowH) + "px;\"><span class=\"name\" style=\"top: " +
               fmt(mid - (compact ? 15.f : 18.f)) + "px;\">" + r.name + "</span>";
        const float right = kRowW - kPad;
        if (r.kind == Kind::Bar) {
            const float barW = r.count * kStepPitch;
            const float barX = right - kReadoutW - 16.f - barW;
            rml += "<div class=\"bar\" style=\"left: " + fmt(barX) + "px; top: " + fmt(mid - 10.f) +
                   "px; width: " + fmt(barW) + "px;\">";
            for (int k = 0; k < r.count; k++)
                rml += "<div id=\"step-" + id + "-" + std::to_string(k) + "\" class=\"step\" data-step=\"" +
                       std::to_string(k) + "\" style=\"left: " + fmt(k * kStepPitch) + "px; width: " + fmt(kStepW) +
                       "px;\"></div>";
            rml += "</div><span id=\"readout-" + id + "\" class=\"readout\" style=\"left: " +
                   fmt(right - kReadoutW) + "px; top: " + fmt(mid - 15.f) + "px; width: " + fmt(kReadoutW) +
                   "px;\"></span>";
        } else if (r.kind == Kind::Switch) {
            const float onX = right - kChipW, offX = onX - kChipW - 8.f;
            rml += "<span id=\"off-" + id + "\" class=\"chip\" style=\"left: " + fmt(offX) + "px; top: " +
                   fmt(mid - 16.f) + "px; width: " + fmt(kChipW) + "px;\">Off</span>";
            rml += "<span id=\"on-" + id + "\" class=\"chip\" style=\"left: " + fmt(onX) + "px; top: " +
                   fmt(mid - 16.f) + "px; width: " + fmt(kChipW) + "px;\">On</span>";
        } else if (r.kind == Kind::Choice) {
            const float x = right - kChoiceW;
            rml += "<span class=\"arrow\" data-dir=\"-1\" style=\"left: " + fmt(x) + "px; top: " + fmt(mid - 17.f) +
                   "px;\">\xe2\x80\xb9</span>";
            rml += "<span id=\"value-" + id + "\" class=\"value\" style=\"left: " + fmt(x + 28.f) + "px; top: " +
                   fmt(mid - 14.f) + "px; width: " + fmt(kChoiceW - 56.f) + "px;\"></span>";
            rml += "<span class=\"arrow\" data-dir=\"1\" style=\"left: " + fmt(right - 20.f) + "px; top: " +
                   fmt(mid - 17.f) + "px;\">\xe2\x80\xba</span>";
        } else {
            rml += "<span id=\"value-" + id + "\" class=\"chip action\" style=\"left: " +
                   fmt(right - kChipW - 40.f) + "px; top: " + fmt(mid - 16.f) + "px; width: " +
                   fmt(kChipW + 40.f) + "px;\"></span>";
        }
        rml += "<span id=\"note-" + id + "\" class=\"note\" style=\"top: " + fmt(mid - 9.f) + "px;\"></span></div>";
        y += rowH + kRowGap;
    }
    rml += "<button id=\"reset\" data-row=\"" + std::to_string(n) + "\" style=\"left: " +
           fmt((kScreenW - kResetW) / 2) + "px; top: " + fmt(kScreenH - kResetH - 14.f) + "px;\">" + rows_[n].name +
           "</button>";
    rml += "<div id=\"ask\"><div id=\"ask-box\" style=\"left: " + fmt((kScreenW - kAskW) / 2) + "px; top: " +
           fmt((kScreenH - kAskH) / 2) + "px; width: " + fmt(kAskW) + "px; height: " + fmt(kAskH) +
           "px;\"><span id=\"ask-title\">Restart now?</span><span id=\"ask-text\"></span>"
           "<button id=\"ask-0\" class=\"ask\" data-ask=\"0\" style=\"left: 70px;\">Restart now</button>"
           "<button id=\"ask-1\" class=\"ask\" data-ask=\"1\" style=\"left: 340px;\">Later</button></div></div>";
    document_->GetElementById("board")->SetInnerRML(rml);
    cursor_ = resume_ >= 0 && resume_ <= n ? resume_ : 0;
    resume_ = -1;
    refresh();
}

void Page::refresh() {
    const int n = static_cast<int>(rows_.size()) - 1;
    for (int i = 0; i < n; i++) {
        const Row& r = rows_[i];
        const std::string id = std::to_string(i);
        auto* box = document_->GetElementById("row-" + id);
        if (box == nullptr)
            continue;
        box->SetClass("cursor", i == cursor_);
        box->SetClass("unavailable", !live(i));
        const int v = r.get ? r.get() : 0;
        if (r.kind == Kind::Bar) {
            for (int k = 0; k < r.count; k++)
                if (auto* e = document_->GetElementById("step-" + id + "-" + std::to_string(k)))
                    e->SetClass("on", k < v);
            if (auto* e = document_->GetElementById("readout-" + id))
                e->SetInnerRML(r.label ? r.label(v) : std::to_string(v));
        } else if (r.kind == Kind::Switch) {
            if (auto* e = document_->GetElementById("on-" + id))
                e->SetClass("on", v != 0);
            if (auto* e = document_->GetElementById("off-" + id))
                e->SetClass("on", v == 0);
        } else if (auto* e = document_->GetElementById("value-" + id)) {
            e->SetInnerRML(Rml::StringUtilities::EncodeRml(r.label ? r.label(v) : std::to_string(v)));
        }
        if (auto* e = document_->GetElementById("note-" + id)) {
            const char* note = r.note ? r.note() : nullptr;
            if ((note == nullptr || *note == '\0') && r.pending && r.pending())
                note = "AFTER RESTART";
            e->SetInnerRML(note != nullptr ? note : "");
            // Beside the value: left of a switch's chips, a bar or a choice.
            const float right = kRowW - kPad;
            const float valueW = r.kind == Kind::Switch ? 2 * kChipW + 8.f
                                 : r.kind == Kind::Bar  ? r.count * kStepPitch + 16.f + kReadoutW
                                 : r.kind == Kind::Choice ? kChoiceW
                                                          : kChipW + 40.f;
            e->SetProperty("left", fmt(right - valueW - 16.f - 250.f) + "px");
            e->SetProperty("width", "250px");
        }
    }
    if (auto* e = document_->GetElementById("reset"))
        e->SetClass("cursor", cursor_ == n && !asking_);
    if (auto* e = document_->GetElementById("row-" + std::to_string(cursor_)))
        e->SetClass("cursor", !asking_);
    if (auto* e = document_->GetElementById("ask"))
        e->SetClass("open", asking_);
    if (!asking_)
        return;
    // "SpaceWarp and Foveated rendering (FFR) change when Xelee restarts."
    std::vector<const char*> names;
    for (int i = 0; i < n; i++)
        if (rows_[i].pending && rows_[i].pending())
            names.push_back(rows_[i].name);
    std::string text;
    for (size_t i = 0; i < names.size(); i++)
        text += std::string(i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
    text += names.size() == 1 ? " changes" : " change";
    text += " when Xelee restarts.";
    if (auto* e = document_->GetElementById("ask-text"))
        e->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
    for (int k = 0; k < 2; k++)
        if (auto* e = document_->GetElementById("ask-" + std::to_string(k)))
            e->SetClass("cursor", k == askCursor_);
}

// The rows' changes that wait for a restart, as "row=value;" ("" for none).
std::string Page::waiting() const {
    std::string key;
    for (size_t i = 0; i < rows_.size(); i++)
        if (rows_[i].pending && rows_[i].pending())
            key += std::to_string(i) + "=" + std::to_string(rows_[i].get ? rows_[i].get() : 0) + ";";
    return key;
}

void Page::ask(bool open) {
    asking_ = open;
    askCursor_ = 0;
    refresh();
}

// 0 Restart now, 1 Later (which goes back).
void Page::answer(int choice) {
    if (choice == 0) {
        restarting_ = true;
        lbAudioAx_80024030(SFX_CONFIRM);
        pc_restart();
        return;
    }
    putOff_ = waiting();
    ask(false);
    go_ = PC_SCREEN_GO_BACK;
}

int Page::leaving() {
    const int where = go_;
    go_ = PC_SCREEN_STAY;
    return where;
}

void Page::changed() {
    refresh();
    lbAudioAx_80024030(SFX_MOVE); // after the change: a volume's heard at its new level
}

void Page::show(bool show) {
    if (!show) {
        if (shown_) {
            shown_ = false;
            document_->Hide();
        }
        return;
    }
    if (shown_ || !load())
        return;
    build();
    shown_ = true;
    asking_ = false;
    go_ = PC_SCREEN_STAY;
    presence_ = 0.f;
    placedPresence_ = -1.f;
    document_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    layout();
}

void Page::move(int dir) {
    const int n = static_cast<int>(rows_.size());
    cursor_ = (cursor_ + (dir < 0 ? n - 1 : 1)) % n;
    hoverStep_ = -1;
    lbAudioAx_80024030(SFX_MOVE);
    refresh();
}

// Left (-1) or right (+1) on the cursor's row.
void Page::change(int dir) {
    const Row& r = rows_[cursor_];
    if (!live(cursor_) || !r.get || !r.set)
        return;
    const int v = r.get();
    switch (r.kind) {
    case Kind::Bar: {
        const int to = std::clamp(v + dir, 0, r.count);
        if (to == v)
            return;
        r.set(to);
        break;
    }
    case Kind::Switch:
        r.set(v != 0 ? 0 : 1); // either arrow flips it
        break;
    case Kind::Choice:
        r.set((v + dir + r.count) % r.count);
        break;
    case Kind::Action:
        return;
    }
    changed();
}

void Page::set_step(int row, int step) {
    const Row& r = rows_[row];
    if (r.kind != Kind::Bar || !live(row) || !r.set)
        return;
    r.set(step + 1);
    changed();
}

void Page::select() {
    const Row& r = rows_[cursor_];
    if (!live(cursor_) || !r.set)
        return;
    switch (r.kind) {
    case Kind::Action:
        r.set(0);
        refresh();
        lbAudioAx_80024030(SFX_CONFIRM);
        break;
    case Kind::Switch:
    case Kind::Choice:
        change(1);
        break;
    case Kind::Bar:
        if (pointerWas_ && hoverStep_ >= 0)
            set_step(cursor_, hoverStep_); // a laser's click on a step
        break;
    }
}

bool Page::input(int what) {
    if (!shown_ || presence_ < 1.f)
        return false;
    if (restarting_)
        return true;
    if (asking_) {
        switch (what) {
        case PC_SCREEN_LEFT:
        case PC_SCREEN_RIGHT:
            askCursor_ ^= 1;
            lbAudioAx_80024030(SFX_MOVE);
            refresh();
            return true;
        case PC_SCREEN_CONFIRM:
            answer(askCursor_);
            return true;
        case PC_SCREEN_BACK:
            // Later, and back at once.
            putOff_ = waiting();
            ask(false);
            return false;
        default:
            return true;
        }
    }
    if (what == PC_SCREEN_BACK) {
        const std::string key = waiting();
        if (key.empty() || key == putOff_ || !pc_restart_available())
            return false; // the page goes back
        lbAudioAx_80024030(SFX_CONFIRM);
        ask(true);
        return true;
    }
    switch (what) {
    case PC_SCREEN_UP:
        move(-1);
        return true;
    case PC_SCREEN_DOWN:
        move(1);
        return true;
    case PC_SCREEN_LEFT:
        change(-1);
        return true;
    case PC_SCREEN_RIGHT:
        change(1);
        return true;
    case PC_SCREEN_CONFIRM:
        select();
        return true;
    default:
        return false;
    }
}

const char* Page::hint() {
    if (!shown_ || rows_.empty())
        return nullptr;
    if (asking_ || restarting_)
        return askCursor_ == 0 ? "Xelee closes and starts again with the new settings."
                               : "Keep playing as you are. The changes wait for the next start.";
    const Row& r = rows_[cursor_];
    if (r.hint_now) {
        if (const char* h = r.hint_now())
            return h;
    }
    return r.hint;
}

void Page::layout() {
    auto* context = aurora::rmlui::get_context();
    auto* screen = document_->GetElementById("screen");
    if (context == nullptr || screen == nullptr)
        return;
    const Rml::Vector2i d = context->GetDimensions();
    dims_ = d;
    if (d == placedDims_ && presence_ == placedPresence_)
        return;
    placedDims_ = d;
    placedPresence_ = presence_;
    // The frame's inside, measured on the Erase Data frames, as Controls'.
    const float x0 = 0.123f * d.x, y0 = 0.170f * d.y;
    const float w = 0.755f * d.x, h = 0.672f * d.y;
    const float p = std::clamp(presence_, 0.f, 1.f);
    const float ease = 1.f - (1.f - p) * (1.f - p);
    const float scale = std::min(w / kScreenW, h / kScreenH);
    screen->SetProperty("transform", "scale(" + fmt(scale) + ")");
    screen->SetProperty("left", fmt(x0 + (w - kScreenW * scale) / 2) + "px");
    screen->SetProperty("top", fmt(y0 + (h - kScreenH * scale) / 2 + (1.f - ease) * 10.f * scale) + "px");
    screen->SetProperty("opacity", fmt(ease));
}

void Page::ProcessEvent(Rml::Event& event) {
    if (!shown_ || presence_ < 1.f)
        return;
    Rml::Element* target = event.GetTargetElement();
    if (asking_ || restarting_) {
        const int choice = attr_of(target, "data-ask");
        if (choice < 0 || restarting_)
            return;
        if (event.GetId() == Rml::EventId::Mouseover && choice != askCursor_) {
            askCursor_ = choice;
            lbAudioAx_80024030(SFX_MOVE);
            refresh();
        } else if (event.GetId() == Rml::EventId::Click && !pointerWas_) {
            answer(choice); // a laser's click arrives as A
        }
        return;
    }
    const int row = attr_of(target, "data-row");
    const int step = attr_of(target, "data-step");
    switch (event.GetId()) {
    case Rml::EventId::Mouseover:
        if (row >= 0 && row != cursor_) {
            cursor_ = row;
            lbAudioAx_80024030(SFX_MOVE);
            refresh();
        }
        hoverStep_ = row == cursor_ ? step : -1;
        break;
    case Rml::EventId::Click:
        // A laser's click arrives as the menus' A too (xr_pointer.c).
        if (pointerWas_ || row < 0)
            break;
        cursor_ = row;
        if (step >= 0) {
            set_step(row, step);
        } else if (const int dir = attr_of(target, "data-dir"); dir == 1 || dir == -1) {
            change(dir);
        } else {
            select();
        }
        break;
    default:
        break;
    }
}

void Page::update() {
    if (!shown_)
        return;
    layout();
    bool ticked = false;
    for (const Row& r : rows_)
        ticked |= r.tick && r.tick();
    if (ticked)
        refresh();
#ifdef AURORA_ENABLE_OPENXR
    // The lasers: the mouse on the screen.
    float px = 0.f, py = 0.f;
    bool down = false;
    auto* context = aurora::rmlui::get_context();
    if (context != nullptr && aurora_xr_screen_pointer(&px, &py, &down)) {
        context->ProcessMouseMove(static_cast<int>(px * dims_.x), static_cast<int>(py * dims_.y), 0);
        if (down && !pointerPressed_)
            context->ProcessMouseButtonDown(0, 0);
        else if (!down && pointerPressed_)
            context->ProcessMouseButtonUp(0, 0);
        pointerPressed_ = down;
        pointerWas_ = true;
    } else if (pointerWas_ && context != nullptr) {
        if (pointerPressed_)
            context->ProcessMouseButtonUp(0, 0);
        context->ProcessMouseLeave();
        pointerWas_ = pointerPressed_ = false;
    }
#endif
}

} // namespace pc::settings
