
"""GestureControl voice system.

Runs Bluetooth in a dedicated thread and Moonshine separately.
This avoids Windows COM conflicts between BLE and audio libraries.
"""

import asyncio
import sys
import threading

from gemma_mode import GemmaModeMonitor
from offline_stt import OfflineSpeech


def run_ble_thread(main_loop, on_change, stop_event, on_error):
    """Run Bleak in its own thread with an independent event loop."""

    def report_change(enabled):
        main_loop.call_soon_threadsafe(on_change, enabled)

    async def ble_main():
        monitor = GemmaModeMonitor(on_change=report_change)
        task = asyncio.create_task(monitor.run())

        try:
            while not stop_event.is_set():
                if task.done():
                    await task
                await asyncio.sleep(0.1)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    ole32 = None
    com_initialized = False

    try:
        if sys.platform == "win32":
            import ctypes

            # Explicitly initialize this new thread as MTA.
            # COINIT_MULTITHREADED = 0.
            ole32 = ctypes.WinDLL("ole32")
            ole32.CoInitializeEx.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint,
            ]
            ole32.CoInitializeEx.restype = ctypes.c_long
            ole32.CoUninitialize.argtypes = []
            ole32.CoUninitialize.restype = None

            result = ole32.CoInitializeEx(None, 0)

            if result not in (0, 1):
                raise RuntimeError(
                    f"COM MTA initialization failed: "
                    f"0x{result & 0xFFFFFFFF:08X}"
                )

            com_initialized = True

        print("[BLE] Dedicated Bluetooth thread started")
        asyncio.run(ble_main())

    except Exception as exc:
        try:
            main_loop.call_soon_threadsafe(
                on_error, str(exc)
            )
        except RuntimeError:
            pass

    finally:
        if com_initialized:
            ole32.CoUninitialize()


async def main():
    loop = asyncio.get_running_loop()

    def handle_command(text):
        print(f"\n[VOICE COMMAND] {text}", flush=True)

    speech = OfflineSpeech(
        loop=loop,
        on_command=handle_command,
    )

    mode_changes = asyncio.Queue()
    stop_ble = threading.Event()

    ble_thread = None
    current_operation = None
    gemma_active = False

    async def run_blocking(function, *args):
        nonlocal current_operation

        operation = asyncio.create_task(
            asyncio.to_thread(function, *args)
        )
        current_operation = operation

        try:
            return await asyncio.shield(operation)
        finally:
            if operation.done() and current_operation is operation:
                current_operation = None

    def on_mode_change(enabled):
        nonlocal gemma_active

        if gemma_active == enabled:
            return

        gemma_active = enabled

        if enabled:
            print("[BLE] GEMMA ON")
        else:
            # Immediately reject pending transcription results.
            speech.disallow()
            print("[BLE] GEMMA OFF")

        mode_changes.put_nowait(enabled)

    def on_ble_error(message):
        print(f"[BLE] Worker failed: {message}")
        mode_changes.put_nowait(None)

    try:
        print("[System] Loading offline speech recognition...")
        await run_blocking(speech.load)

        print("[System] Moonshine ready")
        print("[System] Starting Bluetooth monitor...")

        # Unlike asyncio.to_thread(), this creates a fresh,
        # dedicated thread that we explicitly configure for MTA.
        ble_thread = threading.Thread(
            target=run_ble_thread,
            args=(
                loop,
                on_mode_change,
                stop_ble,
                on_ble_error,
            ),
            name="GestureControlBLE",
            daemon=True,
        )
        ble_thread.start()

        while True:
            enabled = await mode_changes.get()

            if enabled is None:
                raise RuntimeError("Bluetooth worker stopped")

            if enabled:
                if not gemma_active:
                    continue

                generation = speech.allow()

                try:
                    await run_blocking(
                        speech.start, generation
                    )

                    if gemma_active:
                        print("[Speech] Yeti listening")

                except Exception as exc:
                    speech.disallow()
                    print(f"[Speech] Start failed: {exc}")
                    continue

                if not gemma_active:
                    speech.disallow()
                    await run_blocking(speech.stop)

            else:
                await run_blocking(speech.stop)
                print("[Speech] Microphone closed")

    finally:
        speech.disallow()
        stop_ble.set()

        # Finish any ongoing microphone operation before cleanup.
        if current_operation is not None:
            try:
                await current_operation
            except Exception:
                pass

        if ble_thread is not None:
            await asyncio.to_thread(ble_thread.join)

        await asyncio.to_thread(speech.close)
        print("[System] Shutdown complete")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[System] Stopped")
