#include "audio/tts_engine.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>


TtsEngine g_tts;

TtsEngine::TtsEngine() {
    LightLock_Init(&synthLock_);
    LightEvent_Init(&synthEvent_, RESET_ONESHOT);
    memset(audioBufs_, 0, sizeof(audioBufs_));
    memset(waveBufs_,  0, sizeof(waveBufs_));
}

TtsEngine::~TtsEngine() { exit(); }

//-----------------------------------------------------------------------------
// init() — eSpeak NG + ndsp
//-----------------------------------------------------------------------------
bool TtsEngine::init(const std::string& voiceDataPath, const std::string& lang) {
#if !ENABLE_TTS
    // TTS stretch goal: eSpeak NG not compiled in — silently disable
    (void)voiceDataPath; (void)lang;
    return false;
#else
    // Initialise eSpeak NG with the provided voice data path
    int srate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS,
                                  BUF_SAMPLES, voiceDataPath.c_str(), 0);
    if (srate < 0) return false;

    espeak_SetSynthCallback(espkCallback);
    espeak_SetVoiceByName(lang.c_str());

    // ndsp setup
    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_MONO);
    ndspSetClippingMode(NDSP_CLIP_SOFT);
    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHANNEL, (float)SAMPLE_RATE);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_MONO_PCM16);

    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(CHANNEL, mix);

    // Allocate audio buffers in DSP-accessible linear memory
    for (int i = 0; i < NUM_BUFS; i++) {
        audioBufs_[i] = (int16_t*)linearAlloc(BUF_SAMPLES * sizeof(int16_t));
        if (!audioBufs_[i]) { exit(); return false; }
        memset(&waveBufs_[i], 0, sizeof(ndspWaveBuf));
        waveBufs_[i].data_vaddr = audioBufs_[i];
        waveBufs_[i].nsamples   = BUF_SAMPLES;
    }

    init_ = true;
    return true;
#endif
}

void TtsEngine::exit() {
    stop();
    if (synthThread_) { LightEvent_Signal(&synthEvent_); threadJoin(synthThread_, U64_MAX);
                        threadFree(synthThread_); synthThread_ = nullptr; }
#if ENABLE_TTS
    if (init_) { espeak_Terminate(); ndspExit(); }
#endif
    for (int i = 0; i < NUM_BUFS; i++) {
        if (audioBufs_[i]) { linearFree(audioBufs_[i]); audioBufs_[i] = nullptr; }
    }
    init_ = false;
}

//-----------------------------------------------------------------------------
// speak() — start synthesis from scratch
//-----------------------------------------------------------------------------
void TtsEngine::speak(const std::string& utf8Text, int wpm) {
    if (!init_) return;
    stop();
    text_      = utf8Text;
    wpm_       = wpm;
    stopReq_   = false;
    paused_    = false;
    speaking_  = true;

    s32 prio;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Pin synthesis to core 1 on New 3DS if available (APP_CPU_USAGE_30 set in main)
    synthThread_ = threadCreate(synthThreadFunc, this, 64 * 1024, prio + 1, -2, true);
}

void TtsEngine::pause()  { paused_ = true; }
void TtsEngine::resume() { paused_ = false; LightEvent_Signal(&synthEvent_); }
void TtsEngine::stop()   { stopReq_ = true; speaking_ = false; paused_ = false; }

//-----------------------------------------------------------------------------
// Synthesis thread
//-----------------------------------------------------------------------------
void TtsEngine::synthThreadFunc(void* arg) {
    reinterpret_cast<TtsEngine*>(arg)->synthesiseLoop();
}

void TtsEngine::synthesiseLoop() {
#if ENABLE_TTS
    // Set WPM via espeak parameter
    espeak_SetParameter(espeakRATE, wpm_, 0);

    // Split text into sentences for aligned pause/resume
    // Simple split at '.', '!', '?', '\n'
    std::string sentence;
    for (size_t i = 0; i <= text_.size(); i++) {
        char c = (i < text_.size()) ? text_[i] : '\0';
        bool endSent = (c == '.' || c == '!' || c == '?' || c == '\n' || c == '\0');
        if (c != '\0') sentence += c;
        if (endSent && !sentence.empty()) {
            if (stopReq_) break;
            while (paused_.load()) {
                LightEvent_Wait(&synthEvent_);
                LightEvent_Clear(&synthEvent_);
                if (stopReq_) goto done;
            }
            espeak_Synth(sentence.c_str(), sentence.size(), 0,
                         POS_CHARACTER, 0, espeakCHARS_UTF8, nullptr, nullptr);
            espeak_Synchronize();
            sentence.clear();
        }
    }
    done:
    if (!stopReq_ && onPageEnd) onPageEnd();
#endif
    speaking_ = false;
}

#if ENABLE_TTS
//-----------------------------------------------------------------------------
// eSpeak callback — called by eSpeak with synthesised PCM samples
//-----------------------------------------------------------------------------
int TtsEngine::espkCallback(short* wave, int numsamples, espeak_EVENT* events) {
    TtsEngine* self = &g_tts;
    if (!self->init_ || self->stopReq_) return 1;  // 1 = abort

    // Send PCM to ndsp buffer
    int buf = self->bufIdx_;
    while (self->waveBufs_[buf].status == NDSP_WBUF_QUEUED ||
           self->waveBufs_[buf].status == NDSP_WBUF_PLAYING)
        svcSleepThread(500000);  // 0.5ms wait

    int copy = std::min(numsamples, (int)self->BUF_SAMPLES);
    memcpy(self->audioBufs_[buf], wave, copy * sizeof(int16_t));
    DSP_FlushDataCache(self->audioBufs_[buf], copy * sizeof(int16_t));
    self->waveBufs_[buf].nsamples = copy;
    ndspChnWaveBufAdd(CHANNEL, &self->waveBufs_[buf]);
    self->bufIdx_ = 1 - buf;

    // Handle word position events for highlight overlay
    self->handleEspkEvents(events);
    return 0;
}

void TtsEngine::handleEspkEvents(espeak_EVENT* events) {
    for (espeak_EVENT* ev = events; ev && ev->type != espeakEVENT_LIST_TERMINATED; ev++) {
        if (ev->type == espeakEVENT_WORD) {
            // ev->text_position = byte offset of word start in original text
            // ev->length = number of characters
            LightLock_Lock(&synthLock_);
            currentWord_.charOffset = ev->text_position;
            currentWord_.length     = ev->length;
            LightLock_Unlock(&synthLock_);
        }
    }
}
#endif  // ENABLE_TTS
