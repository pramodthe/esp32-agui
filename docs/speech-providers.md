# Speech providers (STT / TTS)

The firmware speaks to cloud STT and TTS through a thin facade so the rest of the
app (`main`, push-to-talk, AG-UI) stays provider-agnostic.

## Supported providers

| Provider | STT | TTS | Default |
|----------|-----|-----|---------|
| **Soniox** | Real-time WSS (`stt-rt-v5`) | Real-time WSS (`tts-rt-v1`) | Yes (fresh flash + legacy) |
| **Deepgram** | Listen v1 WSS (`nova-2`, linear16 @ 16 kHz) | Speak v1 WSS (Aura-2, linear16 @ 16 kHz) | Opt-in via portal |

Select the provider and paste that vendor’s API key in the **AMOLED-setup** captive portal.
Switching providers requires a new API key; the portal resets TTS voice to the new provider’s default.

`speech_provider_get()` caches the NVS value in RAM (invalidated on portal save). STT/TTS facades
tear down the previous backend’s mic / drain task before opening the other provider, so a
portal switch without reboot does not double-own the ES8311.

## NVS keys (`appcfg` namespace)

| Key | Meaning |
|-----|---------|
| `speech_prov` | `deepgram` or `soniox` |
| `speech_key` | API key for the active provider |
| `soniox_key` | Legacy Soniox-only key (still read as fallback when provider is Soniox) |
| `tts_voice` | Provider-specific voice / model id |

## Components

```
speech_cfg     resolve provider + key (incl. migration)
speech_stt     facade → deepgram_stt | soniox_client
speech_tts     facade → deepgram_tts | soniox_tts_client
deepgram_stt   Deepgram Listen v1
deepgram_tts   Deepgram Speak v1
soniox_*       unchanged original backends
```

## Adding a third provider

1. Implement `*_stt` / `*_tts` components with the same session / open-feed-finish APIs.
2. Extend `speech_provider_t` + portal dropdown + voice list.
3. Dispatch in `speech_stt.c` / `speech_tts.c`.
4. Document auth + endpoints here.

Do **not** call provider backends from `main` — always go through the facades.
