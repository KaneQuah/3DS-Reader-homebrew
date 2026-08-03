#pragma once
#include <3ds.h>

//-----------------------------------------------------------------------------
// TouchHandler — swipe and tap gesture recognition on the touch screen
//
// Design doc §3.3:
//   Swipe right→left  : next page spread
//   Swipe left→right  : previous page spread
//   Tap right half    : next page spread
//   Tap left half     : previous page spread
//
// Bottom screen in portrait is 240×320 px; the midpoint is x=120.
//-----------------------------------------------------------------------------
enum class TouchGesture {
    None,
    TapLeft,
    TapRight,
    SwipeLeft,    // right→left → next
    SwipeRight,   // left→right → prev
};

class TouchHandler {
public:
    // Call once per frame with the current and previous touch position
    TouchGesture update(const touchPosition& cur, const touchPosition& prev,
                        bool touched, bool released);

private:
    static constexpr int SWIPE_THRESHOLD_PX = 30;   // min horizontal delta for a swipe
    static constexpr int TAP_MAX_MOVE_PX    = 10;   // max movement to qualify as a tap
    static constexpr int TAP_MAX_FRAMES     = 30;   // max duration for a tap (~500 ms)
    static constexpr int BOT_MID_X          = 120;  // portrait midpoint on 240-px-wide bottom

    bool  holding_      = false;
    int   startX_       = 0;
    int   startY_       = 0;
    int   holdFrames_   = 0;
    int   maxDeltaX_    = 0;  // track max horizontal movement during hold

    // Long-press (for file browser context menu)
public:
    static constexpr int LONG_PRESS_FRAMES = 30; // ~500 ms at 60 fps
    bool  longPress() const { return longPress_; }

private:
    bool  longPress_ = false;
};
