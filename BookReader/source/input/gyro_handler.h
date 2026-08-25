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

    /// Re-synchronise after the app was suspended or the console slept.
    ///
    /// No gyro samples arrive while suspended, and lastTick_ is left pointing
    /// at whenever the app last ran — so the first poll after waking would
    /// compute a nonsense dt and integrate it. This zeroes the roll
    /// integrator, clears the flip debounce, and restamps the clock.
    ///
    /// orientation_ is deliberately PRESERVED. This class derives orientation
    /// by integrating angular rate, so it has no way to sense how the console
    /// is being held after a gap in sampling; keeping the last confirmed
    /// screen assignment is strictly better than guessing a new one.
    void resync();

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

    // rollDeg_ is a pure integration of angular rate with no absolute
    // reference (no accelerometer correction), so any sensor bias — a few
    // tenths of a deg/s is normal for this hardware — accumulates forever
    // while the console just sits there being read. Given enough real time
    // (a several-minute reading session, "a few page turns" apart) that drift
    // alone can cross FLIP_THRESHOLD_DEG with the device never having
    // physically moved, silently swapping which screen plays which page.
    // STILL_* filters that out: sustained near-zero angular rate means the
    // console is at rest, so any rollDeg_ accumulated during that rest is
    // provably drift, not motion, and gets zeroed before it can trip the
    // flip debounce. A genuine 180° turn keeps the rate well above the dead
    // zone for its whole duration and is unaffected.
    static constexpr float STILL_RATE_DEG_S    = 2.0f;
    static constexpr int   STILL_RESET_POLLS   = 60;  // ~2 s at 30 Hz

    int         frameCounter_    = 0;
    int         debounceCount_   = 0;
    bool        pendingFlip_     = false;
    Orientation orientation_     = Orientation::Normal;
    int         flipAnimFrames_  = 0;

    // Integrated roll angle (degrees) — accumulated from angular rate
    float       rollDeg_         = 0.0f;
    u64         lastTick_        = 0;
    int         stillPolls_      = 0;
};
