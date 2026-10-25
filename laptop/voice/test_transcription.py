
"""Test Blue Yeti + Moonshine without wake-word filtering."""

import asyncio
from offline_stt import OfflineSpeech

SHOW_PARTIAL = False  # Set True to see live transcription updates


async def main():
    loop = asyncio.get_running_loop()

    def on_line(text):
        print(f"\n[HEARD] {text}", flush=True)

    def on_partial(text):
        if SHOW_PARTIAL:
            print(f"[PARTIAL] {text}", flush=True)

    speech = OfflineSpeech(
        loop=loop,
        on_line=on_line,
        on_partial=on_partial,
    )

    try:
        await asyncio.to_thread(speech.load)
        await asyncio.to_thread(speech.start)

        print("\nMicrophone listening. Say anything!")
        print("Press Ctrl+C to stop.\n")

        while True:
            await asyncio.sleep(1)

    finally:
        await asyncio.to_thread(speech.close)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
