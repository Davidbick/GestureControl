"""Monitor GestureControl Gemma mode over BLE.

"""

import asyncio
from collections.abc import Callable

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError


GEMMA_SERVICE_UUID = "89ca2ebf-7c18-4a33-9877-df55001abce2"
GEMMA_STATUS_UUID = "89ca2ebf-7c18-4a33-9877-df55001abce3"

SCAN_TIMEOUT_S = 5.0
RETRY_DELAY_S = 1.0


class GemmaModeMonitor:
    """Track whether a Gemma-mode device is connected."""

    def __init__(self, on_change: Callable[[bool], None] | None = None):
        self.active = asyncio.Event()
        self.on_change = on_change

    def _set_active(self, enabled: bool) -> None:
        # Only trigger callbacks when the state changes.
        if self.active.is_set() == enabled:
            return

        if enabled:
            self.active.set()
        else:
            self.active.clear()

        if self.on_change is not None:
            self.on_change(enabled)

    async def run(self) -> None:
        """Search, connect, detect disconnects, and reconnect."""

        try:
            while True:
                try:
                    # Search by service UUID, not the test Wio's name.
                    device = await BleakScanner.find_device_by_filter(
                        lambda _device, ad: GEMMA_SERVICE_UUID in (
                            uuid.lower() for uuid in ad.service_uuids
                        ),
                        timeout=SCAN_TIMEOUT_S,
                        service_uuids=[GEMMA_SERVICE_UUID],
                    )

                    if device is None:
                        continue

                    disconnected = asyncio.Event()
                    loop = asyncio.get_running_loop()

                    def on_disconnect(_client: BleakClient) -> None:
                        # Handle BLE disconnect safely from a callback.
                        loop.call_soon_threadsafe(self._set_active, False)
                        loop.call_soon_threadsafe(disconnected.set)

                    async with BleakClient(
                        device,
                        disconnected_callback=on_disconnect,
                        timeout=15.0,
                    ) as client:

                        # Verify that this is an active Gemma connection.
                        status = bytes(
                            await client.read_gatt_char(
                                GEMMA_STATUS_UUID
                            )
                        )

                        if status != b"\x01":
                            raise RuntimeError(
                                f"Unexpected Gemma status: {status.hex()}"
                            )

                        if not client.is_connected:
                            raise ConnectionError(
                                "Gemma device disconnected"
                            )

                        # Connected and validated: Gemma mode is ON.
                        self._set_active(True)

                        # Stay connected until Wio exits Gemma mode.
                        await disconnected.wait()

                except asyncio.CancelledError:
                    raise

                except (BleakError, OSError, RuntimeError) as exc:
                    print(f"[Gemma BLE] Connection issue: {exc}")

                finally:
                    # Always disable voice after losing the connection.
                    self._set_active(False)

                # Try discovering Gemma mode again.
                await asyncio.sleep(RETRY_DELAY_S)

        finally:
            self._set_active(False)


def print_mode(enabled: bool) -> None:
    if enabled:
        print("[Gemma BLE] GEMMA ON - voice gate enabled")
    else:
        print("[Gemma BLE] GEMMA OFF - voice gate disabled")


async def main() -> None:
    print("[Gemma BLE] Looking for GestureGemma service...")

    monitor = GemmaModeMonitor(on_change=print_mode)
    await monitor.run()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[Gemma BLE] Stopped")
