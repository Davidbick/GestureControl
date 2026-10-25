"""Always-on offline speech recognition via Blue Yeti and Moonshine."""

import asyncio
from collections.abc import Callable

import sounddevice as sd
from moonshine_voice.download import get_model_for_language
from moonshine_voice.mic_transcriber import MicTranscriber
from moonshine_voice.transcriber import ModelArch, Transcriber


class OfflineSpeech:
    def __init__(
        self,
        loop: asyncio.AbstractEventLoop,
        on_line: Callable[[str], None],
        on_partial: Callable[[str], None],
    ):
        self.loop = loop
        self.on_line = on_line
        self.on_partial = on_partial
        self._model = None
        self._mic = None
        self._device_index = None
        self._running = False

    def load(self) -> None:
        devices = sd.query_devices()
        matches = [
            index for index, device in enumerate(devices)
            if device["max_input_channels"] > 0
            and "yeti" in device["name"].lower()
        ]
        if not matches:
            raise RuntimeError(
                "Blue Yeti not found. Check USB connection. "
                "Run: python -m sounddevice"
            )

        default_index = sd.default.device[0]
        self._device_index = (
            default_index if default_index in matches else matches[0]
        )
        print(f"[Speech] Yeti device: {self._device_index}")

        model_path, model_arch = get_model_for_language(
            "en", ModelArch.SMALL_STREAMING
        )
        self._model = Transcriber(model_path, model_arch)
        print("[Speech] Moonshine Small Streaming loaded")

    def _emit(self, callback: Callable[[str], None], text: str) -> None:
        if not self._running:
            return
        try:
            self.loop.call_soon_threadsafe(self._deliver, callback, text)
        except RuntimeError:
            pass  # Python loop already closed.

    def _deliver(self, callback: Callable[[str], None], text: str) -> None:
        if self._running:
            callback(text)

    def start(self) -> None:
        if self._model is None:
            raise RuntimeError("Call load() before start()")
        if self._mic is not None:
            raise RuntimeError("Microphone is already active")

        mic = (
            MicTranscriber()
            .use_transcriber(self._model)
            .device(self._device_index)
            .on_text(lambda text: self._emit(self.on_partial, text))
            .on_line(lambda line: self._emit(self.on_line, line.text))
        )
        try:
            mic.load()
            self._mic = mic
            self._running = True
            mic.start()
        except Exception:
            self._running = False
            self._mic = None
            mic.close()
            raise

    def stop(self) -> None:
        mic = self._mic
        self._mic = None
        self._running = False
        if mic is None:
            return
        try:
            mic.stop()
        finally:
            mic.close()

    def close(self) -> None:
        self.stop()
        if self._model is not None:
            self._model.close()
            self._model = None
