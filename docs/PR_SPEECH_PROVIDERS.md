# PR draft: Add Deepgram as an alternate speech provider

**Target repo:** https://github.com/contextablemark/esp32-agui  
**Branch (local):** `feat/speech-providers-deepgram`  
**Working tree:** `~/Desktop/esp32-agui`

## Summary

- **Add** Deepgram as an optional STT/TTS provider — Soniox stays the default and is unchanged as a feature.
- Add a thin provider facade (`speech_stt` / `speech_tts` + `speech_cfg`) over the existing Soniox clients.
- Implement **Deepgram** Listen v1 + Speak v1 WebSocket backends (16 kHz linear16, same mic/speaker path).
- Select provider at runtime from the **AMOLED-setup** captive portal.
- Existing devices (legacy `soniox_key` or no `speech_prov`) keep using Soniox.

## Test plan

- [ ] `idf.py build` on ESP-IDF 5.5.x / esp32s3
- [ ] Flash Waveshare ESP32-S3-Touch-AMOLED-1.8
- [ ] Portal: provider=Deepgram + Deepgram API key + Wi‑Fi + AG-UI URL → PTT → transcript → spoken reply
- [ ] Portal: switch to Soniox + Soniox key → same flow
- [ ] Barge-in (new PTT during TTS) still cancels speech

## Notes for maintainers

See [docs/speech-providers.md](../docs/speech-providers.md) for NVS keys and how to add a third provider.

### Suggested `gh` flow (after fork)

```bash
cd ~/Desktop/esp32-agui
git remote add fork git@github.com:<YOUR_USER>/esp32-agui.git   # if needed
git push -u fork feat/speech-providers-deepgram
gh pr create --repo contextablemark/esp32-agui \
  --title "Add Deepgram as an alternate speech provider (Soniox remains default)" \
  --body-file docs/PR_SPEECH_PROVIDERS.md
```
