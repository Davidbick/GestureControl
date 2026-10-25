"""Assemble Moonshine lines into commands bracketed by wake/finish phrases."""

import re
import time
from collections.abc import Callable


# Accept only 'Hey Gesture' or 'Hey Control' (never 'Hi').
WAKE_RE = re.compile(
    r"^\s*(?:(?:uh|um|oh|okay|ok)\s*,?\s+)?"
    r"hey\s*,?\s+(?:gesture|control)\b[\s,:.!?-]*", re.I
)
# A trailing 'Gesture' or 'Control' completes the collection.
FINISH_RE = re.compile(
    r"(?:^|[\s,.!?])(?:gesture|control)\b"
    r"(?:[\s,]+(?:do\s+that|that's\s+all|execute|done))?"
    r"[\s.!?]*$",
    re.I,
)
CANCEL_RE = re.compile(
    r"^\s*(?:cancel\s*,?\s*(?:gesture|control)|(?:gesture|control)\s*,?\s*cancel)\s*[.!?]*$",
    re.I,
)

# Common no-content outputs, not a complete solution to ASR hallucinations.
NOISE = {
    "you", "uh", "um", "hmm", "[music]", "(music)", "...",
    "thank you for watching", "thanks for watching", "subscribe",
}


class CommandCollector:
    def __init__(
        self,
        on_complete: Callable[[str], None],
        silence_seconds: float = 6.0,
        wake_timeout_seconds: float = 15.0,
        max_session_seconds: float = 120.0,
        max_chars: int = 4000,
        clock: Callable[[], float] = time.monotonic,
    ):
        self.on_complete = on_complete
        self.silence_seconds = silence_seconds
        self.wake_timeout_seconds = wake_timeout_seconds
        self.max_session_seconds = max_session_seconds
        self.max_chars = max_chars
        self.clock = clock
        self._reset()

    def _reset(self) -> None:
        self.collecting = False
        self.parts: list[str] = []
        self.started_at = 0.0
        self.last_activity_at = 0.0
        self.last_partial = ""

    @staticmethod
    def _clean(value: str) -> str:
        return " ".join(value.strip().split())

    @staticmethod
    def _is_noise(value: str) -> bool:
        return value.strip().casefold().strip(".?!, ") in NOISE

    def _start(self) -> None:
        self._reset()
        self.collecting = True
        self.started_at = self.clock()
        self.last_activity_at = self.started_at
        print('[Voice] Wake detected. Say "Gesture" or "Control" to finish.')

    def on_partial(self, text: str) -> None:
        """Track activity; partial text is NOT added to the command."""
        if not self.collecting:
            return
        text = self._clean(text)
        if not text or self._is_noise(text):
            return
        if text != self.last_partial:
            self.last_partial = text
            self.last_activity_at = self.clock()

    def on_line(self, text: str) -> None:
        """Accept one finalized Moonshine segment (always on event-loop thread)."""
        text = self._clean(text)
        if not text or self._is_noise(text):
            return

        if not self.collecting:
            wake = WAKE_RE.match(text)
            if wake is None:
                return
            self._start()
            text = text[wake.end():].strip()
            if not text:
                return

        # Ignore repeated stand-alone wake phrases during collection.
        if WAKE_RE.fullmatch(text):
            return

        self.last_activity_at = self.clock()
        self.last_partial = ""

        if CANCEL_RE.fullmatch(text):
            print("[Voice] Command cancelled")
            self._reset()
            return

        finish = FINISH_RE.search(text)
        if finish:
            text = text[:finish.start()].strip(" \t.,!?;:")

        if text:
            # Drop consecutive identical lines that Moonshine sometimes repeats.
            if not self.parts or self.parts[-1].casefold() != text.casefold():
                if sum(map(len, self.parts)) + len(text) > self.max_chars:
                    print("[Voice] Command too long; discarding")
                    self._reset()
                    return
                self.parts.append(text)
                print(f"[Voice] + {text}")

        if finish:
            self._finish("completion phrase")

    def poll(self) -> None:
        """Called regularly from main loop to enforce timeouts."""
        if not self.collecting:
            return
        now = self.clock()
        if now - self.started_at >= self.max_session_seconds:
            self._finish("maximum duration")
        elif not self.parts:
            if now - self.last_activity_at >= self.wake_timeout_seconds:
                print("[Voice] Wake expired without a command")
                self._reset()
        elif now - self.last_activity_at >= self.silence_seconds:
            self._finish("inactivity timeout")

    def _finish(self, reason: str) -> None:
        command = "\n".join(self.parts).strip()
        self._reset()
        if not command:
            print("[Voice] Empty command; ignored")
            return
        print(f"[Voice] Command complete ({reason})")
        self.on_complete(command)
