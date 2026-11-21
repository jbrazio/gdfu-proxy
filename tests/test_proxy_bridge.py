import unittest

from proxy_bridge import (
    ProxyBridge,
    ProxyCommand,
    ProxyError,
    ProxyState,
    RadioConfig,
)


class ProxyBridgeTests(unittest.TestCase):
    def test_target_discovery_cache_expires_after_ttl(self) -> None:
        """Catches stale targets remaining selectable after moving out of range."""
        proxy = ProxyBridge(ttl_seconds=5)
        proxy.handle_beacon(target_id=0x01020304, profile=1, rssi=-65, timestamp=100.0)

        self.assertEqual(len(proxy.get_discovered_targets(now=102.0)), 1)
        self.assertEqual(len(proxy.get_discovered_targets(now=106.0)), 0)

    def test_bounded_queue_signals_backpressure_when_full(self) -> None:
        """Catches fast BLE writes overwhelming slow RF link without backpressure."""
        proxy = ProxyBridge(queue_capacity=2)
        proxy.select_target(0x01020304)
        proxy.start_session(session_id=1)

        self.assertTrue(proxy.write_data(b"chunk1"))
        self.assertTrue(proxy.write_data(b"chunk2"))
        self.assertFalse(proxy.write_data(b"chunk3"))  # Queue full, reject

        proxy.confirm_rf_ack(length=len(b"chunk1"))
        self.assertTrue(proxy.write_data(b"chunk3"))  # Now has space

    def test_ble_disconnect_aborts_active_rf_session(self) -> None:
        """Catches zombie RF transfer continuing after app disconnects."""
        proxy = ProxyBridge()
        proxy.select_target(0x01020304)
        proxy.start_session(session_id=1)

        proxy.handle_ble_disconnect()
        self.assertEqual(proxy.state, ProxyState.IDLE)
        self.assertIsNone(proxy.active_session_id)


if __name__ == "__main__":
    unittest.main()
