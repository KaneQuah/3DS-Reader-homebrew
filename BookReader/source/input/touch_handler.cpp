#include "input/touch_handler.h"
#include <cstdlib>

TouchGesture TouchHandler::update(const touchPosition& cur, const touchPosition& prev,
                                   bool touched, bool released) {
    longPress_ = false;

    if (touched && !holding_) {
        // Finger just went down
        holding_    = true;
        startX_     = cur.px;
        startY_     = cur.py;
        holdFrames_ = 0;
        maxDeltaX_  = 0;
    }

    if (holding_) {
        holdFrames_++;
        // Deliberately NOT sampled on the release frame: see the comment in
        // the release branch below. Sampling (0,0) there drove maxDeltaX_ up
        // to the full start offset, which is >= TAP_MAX_MOVE_PX for all but a
        // sliver of the screen — so the tap test below could essentially never
        // succeed, and taps were reported as swipes or as nothing.
        if (!released) {
            int dx = std::abs((int)cur.px - startX_);
            if (dx > maxDeltaX_) maxDeltaX_ = dx;
        }

        // Long-press detection (fire once)
        if (holdFrames_ == LONG_PRESS_FRAMES && maxDeltaX_ < TAP_MAX_MOVE_PX)
            longPress_ = true;
    }

    if (released && holding_) {
        holding_        = false;
        // `prev`, not `cur`. On the frame the finger lifts there is no contact
        // to report, so hidTouchRead() yields (0,0) — measuring the gesture
        // against that made every release look like a swipe toward the origin:
        // deltaX = -startX_. A tap in the upper-right of the page therefore
        // registered as SwipeLeft (always "next page"), and TapLeft/TapRight
        // never fired at all. `prev` is the last position the touchscreen
        // actually reported while in contact, which is what the gesture has to
        // be measured against. (This parameter was accepted and then never
        // read — the fix was already sitting in the signature.)
        int deltaX      = (int)prev.px - startX_;
        int absDeltaX   = std::abs(deltaX);
        int absDeltaY   = std::abs((int)prev.py - startY_);

        // Swipe: significant horizontal movement dominates
        if (absDeltaX >= SWIPE_THRESHOLD_PX && absDeltaX > absDeltaY) {
            return (deltaX < 0) ? TouchGesture::SwipeLeft : TouchGesture::SwipeRight;
        }

        // Tap: little movement, short duration
        if (maxDeltaX_ < TAP_MAX_MOVE_PX && holdFrames_ < TAP_MAX_FRAMES) {
            // startX_ is portrait X, 0..239 across the page width.
            //
            // The CALLER owns the conversion from the hardware's landscape
            // touch coordinates (px 0..319, py 0..239) into portrait space —
            // see ReaderView::update(). This class must never be handed a raw
            // touchPosition straight out of hidTouchRead(), or the split below
            // lands on the wrong axis at the wrong midpoint, which is exactly
            // the bug that made taps and swipes unreliable on hardware.
            return (startX_ < BOT_MID_X) ? TouchGesture::TapLeft : TouchGesture::TapRight;
        }
    }

    return TouchGesture::None;
}
