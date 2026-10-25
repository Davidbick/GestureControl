"""Offline speech recognition using Moonshine and a Blue Yeti."""

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
        on_command: Callable[[str], None],
    ):
        self.loop = loop
        self.on_command = on_command

        self._model = None
        self._mic = None
        self._device_index = None

        # Prevent transcriptions from a previous Gemma session
        # from being delivered after the mode is disabled.
        self._generation = 0
        self._allowed = False

    def load(self):
        """Find the Yeti and load Moonshine into memory once."""

        devices = sd.query_devices()

        matches = [
            (index, device["name"])
            for index, device in enumerate(devices)
            if device["max_input_channels"] > 0
            and "yeti" in device["name"].lower()
        ]

        if not matches:
            raise RuntimeError(
                "Blue Yeti not found. Check USB connection. "
                "Run: python -m sounddevice"
            )

        # Prefer Windows' default input when it is a Yeti.
        default_index = sd.default.device[0]
        self._device_index = next(
            (index for index, _ in matches
             if index == default_index),
            matches[0][0],
        )

        print(f"[Speech] Yeti device: {self._device_index}")

        # Use the same Small Streaming model already tested.
        model_path, model_arch = get_model_for_language(
            "en", ModelArch.SMALL_STREAMING
        )

        # Keep the neural network loaded between BLE mode changes.
        self._model = Transcriber(model_path, model_arch)

        print("[Speech] Moonshine Small Streaming loaded")

    def allow(self) -> int:
        """Begin a new voice session."""
        self._generation += 1
        self._allowed = True
        return self._generation

    def disallow(self):
        """Immediately reject any pending speech results."""
        self._allowed = False
        self._generation += 1

    def _received_line(self, generation: int, text: str):
        """Moonshine can call this from its audio worker thread."""
        text = text.strip()

        if not text:
            return

        # Send completed transcription to the asyncio thread.
        try:
            self.loop.call_soon_threadsafe(
                self._deliver, generation, text
            )
        except RuntimeError:
            # Event loop has already shut down.
            pass

    def _deliver(self, generation: int, text: str):
        """Only accept speech from the currently active session."""
        if not self._allowed:
            return

        if generation != self._generation:
            return

        self.on_command(text)

    def start(self, generation: int):
        """Open Yeti microphone and begin recognition."""

        if self._model is None:
            raise RuntimeError("Call load() first")

        if self._mic is not None:
            raise RuntimeError("Microphone already active")

        # Fresh microphone/stream for each Gemma mode session.
        # The expensive model stays loaded and is reused.
        mic = (
            MicTranscriber()
            .use_transcriber(self._model)
            .device(self._device_index)
            .on_line(
                lambda line: self._received_line(
                    generation, line.text
                )
            )
        )

        try:
            mic.load()
            self._mic = mic
            mic.start()
        except Exception:
            self._mic = None
            mic.close()
            raise

    def stop(self):
        """Stop recognition and physically release the microphone."""

        mic = self._mic
        self._mic = None

        if mic is None:
            return

        try:
            mic.stop()
        finally:
            # Unlike stop(), close() also closes the audio device.
            mic.close()

    def close(self):
        """Release microphone and model resources on program exit."""
        self.stop()

        if self._model is not None:
            self._model.close()
            self._model = None
