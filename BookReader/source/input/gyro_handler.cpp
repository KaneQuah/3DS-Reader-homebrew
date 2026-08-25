#include "input/gyro_handler.h"
#include <cmath>
#include <cstdio>

// libctru: gyroscope angular rate is in radians/s * 1000 (i.e. mrad/s)
// HIDUSER_EnableGyroscope() must be called before hidGyroRead().

void GyroHandler::init() {
    HIDUSER_EnableGyroscope();
    lastTick_      = svcGetSystemTick();
    rollDeg_       = 0.0f;
    debounceCount_ = 0;
    pendingFlip_   = false;
    orientation_   = Orientation::Normal;
    flipAnimFrames_ = 0;
    stillPolls_    = 0;
}

void GyroHandler::exit() {
    HIDUSER_DisableGyroscope();
}

// See the header for why orientation_ survives this and rollDeg_ does not.
void GyroHandler::resync() {
    lastTick_       = svcGetSystemTick();
    rollDeg_        = 0.0f;
    debounceCount_  = 0;
    pendingFlip_    = false;
    stillPolls_     = 0;
    flipAnimFrames_ = 0;
}

bool GyroHandler::update(bool orientationLock) {
    frameCounter_++;
    if (flipAnimFrames_ > 0) flipAnimFrames_--;

    // Rate-limit polling to 30 Hz
    if (frameCounter_ % POLL_EVERY_N_FRAMES != 0) return false;
    if (orientationLock) { debounceCount_ = 0; pendingFlip_ = false; return false; }

    // Measure elapsed time since last poll
    u64 now    = svcGetSystemTick();
    float dtS  = static_cast<float>(now - lastTick_) / (float)SYSCLOCK_ARM11;
    lastTick_  = now;
    dtS        = (dtS > 0.1f) ? 0.1f : dtS;  // clamp for safety

    // Read angular rate: gyro.x = pitch, gyro.y = roll, gyro.z = yaw
    angularRate rate;
    hidGyroRead(&rate);

    // Integrate roll (device rotated ACW 90° so user-visible roll is around Z axis;
    // libctru reports roll around Y when the 3DS is in its natural landscape position)
    // Convert mrad/s → deg/s: factor = 1000 * (180/π)^-1 ≈ 0.01745 rad per mrad → deg/s
    float rollRateDegsPerSec = static_cast<float>(rate.y) * (180.0f / (float)M_PI) / 1000.0f;

    // Below the noise/bias floor, treat the console as stationary rather than
    // integrating whatever small nonzero reading the sensor happens to give.
    // If it stays that way for STILL_RESET_POLLS, snap the integrator back to
    // zero — a real rotation would have crossed the flip threshold *during*
    // the motion, not shown up later while sitting still.
    if (std::fabs(rollRateDegsPerSec) < STILL_RATE_DEG_S) {
        if (++stillPolls_ >= STILL_RESET_POLLS) rollDeg_ = 0.0f;
    } else {
        stillPolls_ = 0;
        rollDeg_ += rollRateDegsPerSec * dtS;
    }

    // Normalise to [-180, 180]
    while (rollDeg_ >  180.0f) rollDeg_ -= 360.0f;
    while (rollDeg_ < -180.0f) rollDeg_ += 360.0f;

    bool overThreshold = (rollDeg_ > FLIP_THRESHOLD_DEG || rollDeg_ < -FLIP_THRESHOLD_DEG);

    if (overThreshold) {
        debounceCount_++;
    } else {
        debounceCount_ = 0;
        pendingFlip_   = false;
    }

    // Confirmed flip: threshold held for DEBOUNCE_FRAMES consecutive polls
    if (debounceCount_ >= DEBOUNCE_FRAMES && !pendingFlip_) {
        pendingFlip_  = true;
        orientation_  = (orientation_ == Orientation::Normal)
                        ? Orientation::Flipped
                        : Orientation::Normal;
        // Reset roll so we don't fire again immediately
        rollDeg_      = 0.0f;
        debounceCount_ = 0;
        flipAnimFrames_ = FLIP_ANIM_FRAMES;
        return true;   // caller should start fade animation
    }

    return false;
}
