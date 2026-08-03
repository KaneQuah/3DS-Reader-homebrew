#pragma once
#include <3ds.h>

//-----------------------------------------------------------------------------
// GyroHandler — polls gyroscope at 30 Hz and detects 180° flip events
//
// Design doc §6.4:
//   • Default orientation: 90° ACW (top = left, bottom = right)
//   • 180° flip confirmed when roll passes ±170° and held for 300 ms
//   • 300 ms debounce prevents accidental triggers
//   • Optional 4-frame (~67 ms) fade animation on confirmed flip
//   • All events ignored when orientation_lock is active
//-----------------------------------------------------------------------------
class GyroHandler {
public:
    // Orientation — which screen plays which page role
    enum class Orientation {
        Normal,   // top = left (verso), bottom = right (recto)
        Flipped,  // top = right (recto), bottom = left (verso)
    };

    void init();
    void exit();

    // Call once per game loop tick (60 Hz); internally rate-limits to 30 Hz
    // Returns true if orientation just flipped (fade animation should play)
    bool update(bool orientationLock);

    Orientation orientation() const { return orientation_; }
    bool        flipping()    const { return flipAnimFrames_ > 0; }
    int         flipFrame()   const { return flipAnimFrames_; }  // counts down 4→0

private:
    static constexpr int   POLL_EVERY_N_FRAMES = 2;   // 60Hz / 2 = 30Hz poll
    static constexpr float FLIP_THRESHOLD_DEG  = 170.0f;
    static constexpr int   DEBOUNCE_FRAMES      = 9;   // ~300 ms at 30 Hz
    static constexpr int   FLIP_ANIM_FRAMES     = 4;

    int         frameCounter_    = 0;
    int         debounceCount_   = 0;
    bool        pendingFlip_     = false;
    Orientation orientation_     = Orientation::Normal;
    int         flipAnimFrames_  = 0;

    // Integrated roll angle (degrees) — accumulated from angular rate
    float       rollDeg_         = 0.0f;
    u64         lastTick_        = 0;
};
