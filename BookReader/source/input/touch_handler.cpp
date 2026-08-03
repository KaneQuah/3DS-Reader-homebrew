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
        int dx = std::abs(cur.px - startX_);
        if (dx > maxDeltaX_) maxDeltaX_ = dx;

        // Long-press detection (fire once)
        if (holdFrames_ == LONG_PRESS_FRAMES && maxDeltaX_ < TAP_MAX_MOVE_PX)
            longPress_ = true;
    }

    if (released && holding_) {
        holding_        = false;
        int deltaX      = cur.px - startX_;
        int absDeltaX   = std::abs(deltaX);
        int absDeltaY   = std::abs(cur.py - startY_);

        // Swipe: significant horizontal movement dominates
        if (absDeltaX >= SWIPE_THRESHOLD_PX && absDeltaX > absDeltaY) {
            return (deltaX < 0) ? TouchGesture::SwipeLeft : TouchGesture::SwipeRight;
        }

        // Tap: little movement, short duration
        if (maxDeltaX_ < TAP_MAX_MOVE_PX && holdFrames_ < TAP_MAX_FRAMES) {
            // startX_ is in portrait coordinates: 0..239 across the screen width
            return (startX_ < BOT_MID_X) ? TouchGesture::TapLeft : TouchGesture::TapRight;
        }
    }

    return TouchGesture::None;
}
