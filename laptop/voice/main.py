
"""GestureControl voice + FunctionGemma demo.

Commands:
    python laptop/voice/main.py --text "Swipe up to move forward"
    python laptop/voice/main.py
    python laptop/voice/main.py --apply
"""
import sys
import argparse
import asyncio
import json
from pathlib import Path

from command_collector import CommandCollector
from gesture_pipeline import DEFAULT_MODEL, GesturePipeline
from ble_config import execute_batch


async def execute_batch_on_mta(batch):
    """Run BLE in a dedicated Windows MTA thread."""
    import ctypes
    from concurrent.futures import ThreadPoolExecutor

    def ble_worker():
        ole32 = ctypes.windll.ole32

        # COINIT_MULTITHREADED = 0
        hr = ole32.CoInitializeEx(None, 0)

        if hr not in (0, 1):
            raise OSError(
                f"Failed to initialize MTA: 0x{hr & 0xFFFFFFFF:08X}"
            )

        try:
            return asyncio.run(execute_batch(batch))
        finally:
            ole32.CoUninitialize()

    with ThreadPoolExecutor(
        max_workers=1,
        thread_name_prefix="ble-mta",
    ) as executor:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(executor, ble_worker)


async def process_command(engine, command, apply):
    print(
        '\n[Voice] Complete request:',
        command,
        flush=True,
    )

    try:
        batch = await asyncio.to_thread(
            engine.prepare,
            command,
        )
    except Exception as exc:
        print(
            f'[GestureControl] NOT applied: {exc}',
            flush=True,
        )
        return

    print('[GestureControl] Validated configuration:')

    print(
        json.dumps(batch, indent=2),
        flush=True,
    )

    if not apply:
        print(
            '[GestureControl] DRY RUN: no BLE changes. '
            'Use --apply to enable.',
            flush=True,
        )
        return

    if sys.platform == "win32":
        from bleak.backends.winrt.util import uninitialize_sta
    
    result = await execute_batch_on_mta(batch)

    print(
        '[BLE] ' + json.dumps(result, indent=2),
        flush=True,
    )

    if not result.get('success'):
        print(
            '[BLE] Configuration not fully verified; '
            'inspect receiver.',
            flush=True,
        )


async def main():
    parser = argparse.ArgumentParser(
        description='GestureControl live voice demo'
    )

    parser.add_argument(
        '--model',
        type=Path,
        default=DEFAULT_MODEL,
    )

    parser.add_argument(
        '--text',
        help='Test a written command instead of the microphone',
    )

    parser.add_argument(
        '--apply',
        action='store_true',
        help='Apply validated configuration over BLE',
    )

    args = parser.parse_args()

    engine = await asyncio.to_thread(
        GesturePipeline,
        args.model,
    )

    if args.text is not None:
        await process_command(
            engine,
            args.text,
            args.apply,
        )
        return

    # Load Moonshine only when microphone mode is requested.
    from offline_stt import OfflineSpeech

    loop = asyncio.get_running_loop()
    active = set()

    async def handle(command):
        try:
            await process_command(
                engine,
                command,
                args.apply,
            )
        finally:
            active.clear()

    def command_ready(command):
        if active:
            print(
                '[Voice] Still processing previous request; ignored',
                flush=True,
            )
            return

        task = loop.create_task(handle(command))
        active.add(task)

        print(
            '[Voice] Processing captured request...',
            flush=True,
        )

    collector = CommandCollector(
        on_complete=command_ready,
    )

    speech = OfflineSpeech(
        loop=loop,
        on_line=collector.on_line,
        on_partial=collector.on_partial,
    )

    try:
        print(
            '[System] Loading offline speech recognition...',
            flush=True,
        )

        await asyncio.to_thread(speech.load)
        await asyncio.to_thread(speech.start)

        print(
            '[Voice] Say "Hey Gesture", give your mappings, '
            'then say "Gesture" to finish',
            flush=True,
        )

        print(
            '[BLE] ' + (
                'ENABLED'
                if args.apply
                else 'DRY RUN'
            ),
            flush=True,
        )

        while True:
            await asyncio.sleep(0.2)
            collector.poll()

    finally:
        await asyncio.to_thread(speech.close)

        if active:
            await asyncio.gather(
                *active,
                return_exceptions=True,
            )


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print('\n[System] Stopped')
