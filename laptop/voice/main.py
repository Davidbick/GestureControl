"""Always-on wake-word command capture (FunctionGemma integration comes next)."""

import asyncio

from command_collector import CommandCollector
from offline_stt import OfflineSpeech


async def main() -> None:
    loop = asyncio.get_running_loop()

    def command_ready(command: str) -> None:
        print("\n========== COMPLETE COMMAND ==========")
        print(command)
        print("======================================\n", flush=True)
        # The next integration step will pass this complete string
        # to FunctionGemma. For now nothing is executed.
        print('[Voice] Waiting for "Hey Gesture" or "Hey Control"', flush=True)

    collector = CommandCollector(on_complete=command_ready)
    speech = OfflineSpeech(
        loop=loop,
        on_line=collector.on_line,
        on_partial=collector.on_partial,
    )

    try:
        print("[System] Loading offline speech recognition...")
        await asyncio.to_thread(speech.load)
        await asyncio.to_thread(speech.start)
        print("[Speech] Yeti listening continuously")
        print('[Voice] Waiting for "Hey Gesture" or "Hey Control"')

        while True:
            await asyncio.sleep(0.2)
            collector.poll()
    finally:
        await asyncio.to_thread(speech.close)
        print("[System] Shutdown complete")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[System] Stopped")
