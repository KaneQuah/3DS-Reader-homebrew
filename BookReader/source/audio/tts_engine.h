#pragma once
// audio/tts_engine.h — Text-to-Speech stretch goal (design doc §15)
//
// Engine: eSpeak NG (formant synthesiser, GPL v3)
// Audio output: libctru ndsp (Nintendo DSP service)
//
// Pipeline:
//   Text → sentence segmentation → eSpeak NG synthesis (PCM 16-bit mono 22050 Hz)
//   → ndsp double-buffers → speaker output
//   eSpeak word-position callback → word highlight overlay
//
// Threading: synthesis runs on a background thread (libctru threadCreate).
//   On New 3DS, background thread is pinned to core 1 via APP_CPU_USAGE_30.

// Build flag: ENABLE_TTS=0 compiles this class to no-op stubs so the project
// links without eSpeak NG (which is not available as a devkitPro portlib).
#ifndef ENABLE_TTS
#define ENABLE_TTS 0
#endif

#include <3ds.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

#if ENABLE_TTS
#include <espeak-ng/speak_lib.h>
#endif

struct WordHighlight {
    int lineIndex;   // line in the current page
    int charOffset;  // byte offset within the line's text
    int length;      // length in bytes
};

class TtsEngine {
public:
    static constexpr int  SAMPLE_RATE   = 22050;
    static constexpr int  BUF_SAMPLES   = 2048;   // ndsp buffer size
    static constexpr int  NUM_BUFS      = 2;       // double-buffering
    static constexpr int  CHANNEL       = 0;
    static constexpr int  PRE_SYNTH_SEC = 30;      // pre-synthesis buffer ahead

    TtsEngine();
    ~TtsEngine();

    // Initialise eSpeak NG and ndsp. Returns false if unavailable.
    bool init(const std::string& voiceDataPath, const std::string& lang = "en");
    void exit();

    // Start speaking text from the beginning. Stops any current speech.
    void speak(const std::string& utf8Text, int wpm = 150);

    // Pause at the current word boundary
    void pause();

    // Resume from where paused
    void resume();

    // Stop completely and clear buffer
    void stop();

    bool isSpeaking() const { return speaking_.load(); }
    bool isPaused()   const { return paused_.load(); }

    // Current word highlight (updated by eSpeak callback)
    WordHighlight currentWord() const { return currentWord_; }

    // Auto-advance: call when TTS reaches end of page
    std::function<void()> onPageEnd;

private:
    bool  init_    = false;
    std::atomic<bool> speaking_{ false };
    std::atomic<bool> paused_  { false };
    std::atomic<bool> stopReq_ { false };

    std::string text_;
    int         wpm_ = 150;

    // ndsp
    ndspWaveBuf waveBufs_[NUM_BUFS];
    int16_t*    audioBufs_[NUM_BUFS];
    int         bufIdx_   = 0;

    // Synthesis thread
    Thread      synthThread_    = nullptr;
    LightLock   synthLock_;
    LightEvent  synthEvent_;

    WordHighlight currentWord_{};

    static void synthThreadFunc(void* arg);
    void        synthesiseLoop();

#if ENABLE_TTS
    // eSpeak NG callback (static, calls instance method)
    static int espkCallback(short* wave, int numsamples, espeak_EVENT* events);
    void       handleEspkEvents(espeak_EVENT* events);
#endif
};

extern TtsEngine g_tts;
