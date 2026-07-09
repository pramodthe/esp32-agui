// soniox_tts_client — spoken AG-UI replies via Soniox real-time TTS (wss://tts-rt.soniox.com).
//
// P-a (this version): BATCH. soniox_tts_speak(text) opens the TTS WSS AFTER the AG-UI run has
// finished (so only one TLS session is open at a time — "sequential TLS"), sends the whole reply +
// text_end, receives base64 pcm_s16le @16k/mono, plays it through a caller-provided sink, and closes.
// Proves the protocol/auth, the audio format (mono vs stereo-as-mono), the speaker write path, and
// that the TLS coexists on this RAM-tight build. Streaming + barge-in are P-b / P-c.
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// PCM sink: raw 16-bit/16k/mono samples to play. Provided by main (which owns the ES8311 OUT / s_spk),
// so there is a single speaker owner (the beep + low-power codec-close never race a second handle).
typedef void (*tts_pcm_sink_t)(const void *pcm, size_t bytes);

// One-time: register the playback sink + spin up the drain task. Call once at boot.
esp_err_t soniox_tts_init(tts_pcm_sink_t sink);

// Speak `text` and BLOCK until playback finishes (or error/timeout). Opens the TTS WSS, sends the
// text, plays the returned PCM, closes. Soniox key comes from NVS (app_cfg). Serialized internally.
// Equivalent to open()+feed(text)+finish()+wait_drained(); used as the P-a batch fallback.
esp_err_t soniox_tts_speak(const char *text);

// --- P-b streaming: speak the reply as it arrives ---------------------------------
// Lifecycle, all called from ONE task (the run task) in order, EXCEPT cancel() (any task):
//   open() -> feed(chunk)* -> finish() -> wait_drained().
// open() BLOCKS through the handshake (so it briefly stalls the SSE read that calls it — lossless) and,
// on success, holds an internal lock until wait_drained() releases it. On failure it returns an error
// with nothing left open, so the caller can fall back to soniox_tts_speak() (sequential batch).
// wait_drained() MUST be called once per successful open() (cancel() alone does not release the lock).
esp_err_t soniox_tts_open(void);                  // open WSS + send config; ESP_OK once OPEN
esp_err_t soniox_tts_feed(const char *text);      // send a text chunk (text_end:false); no-op if not OPEN
esp_err_t soniox_tts_finish(void);                // send text_end:true once for the whole turn
esp_err_t soniox_tts_wait_drained(uint32_t timeout_ms);  // block until drained/error/cancel, then close

// PTT barge-in: cancel an in-flight stream from any task (e.g. a button cb during playback). Returns
// immediately; audio stops within ~1 drain chunk plus the codec DMA tail; wait_drained() then returns.
// No-op when nothing is streaming. The run task still owns the teardown via wait_drained().
void soniox_tts_cancel(void);

#ifdef __cplusplus
}
#endif
