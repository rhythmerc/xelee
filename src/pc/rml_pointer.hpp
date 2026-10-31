/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pointing at a menu's rows with the mouse or the headset's laser.
 *
 * The menus follow Mousemove, not Mouseover: RmlUi hovers whatever the layout
 * moves under a resting pointer (Game Mode leaves one over the window), and
 * taking that as pointing pulled the cursor away from the pad (A on a stage
 * jumped to the last one, Down to the bottom row). A mouse's Mousemove is a
 * real move, so it points. The laser's comes every frame, as no hand rests,
 * so it points only on reaching another row: one resting on a row doesn't
 * undo the thumbstick. */
#pragma once

namespace pc::rml {

class Pointing {
public:
    // A Mousemove over `row` (-1 for none of them): whether that points at it.
    bool at(int row, bool laser) {
        const bool anew = !laser || row != row_;
        row_ = row;
        return anew;
    }
    // The laser left: back on the same row is pointing again.
    void reset() { row_ = -1; }

private:
    int row_ = -1;
};

} // namespace pc::rml
