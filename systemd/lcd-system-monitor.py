#!/usr/bin/env python3
import asyncio
import logging
import psutil
import struct
import time
import sys
import signal
import socket

class SystemMonitorService:
    PHEADER = 0xFA
    PTYPE_CPU = 1
    PTYPE_TEMP = 2
    PTYPE_RAM = 3
    PTYPE_DISK = 4
    PTYPE_UPTIME = 5

    REFRESH_FPS = 20
    BROADCAST_PORT = 33333

    def __init__(self):
        self.udp_sock: socket.socket | None = None
        self.esp32_ip = None
        self.esp32_port = None
        self.running = False
        self.previousDelta = {"read_bytes": 0, "write_bytes": 0}
        self.logger = self._setup_logger()

        self._disk_last_time = 0.0
        self._disk_last_value = {"read_bytes": 0, "write_bytes": 0}
        self._seq = 0

    # --------------------------
    # Setup / Lifecycle
    # --------------------------
    def _setup_logger(self):
        logger = logging.getLogger("SystemMonitorService")
        logger.setLevel(logging.INFO)
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
        logger.addHandler(handler)
        return logger

    async def start(self):
        self.running = True
        self.logger.info("SystemMonitorService starting...")
        await self.run()

    def stop(self, *_):
        self.logger.info("SystemMonitorService stopping...")
        self.running = False
        if self.udp_sock:
            try:
                self.udp_sock.close()
            except Exception:
                pass
            self.udp_sock = None

    # --------------------------
    # Main Loop
    # --------------------------
    async def run(self):
        self.logger.info("SystemMonitorService started.")
        refresh_interval = 1 / self.REFRESH_FPS

        while self.running:
            try:
                await self.ensure_connected()
                if not self.udp_sock:
                    await asyncio.sleep(2)
                    continue

                # Collect system stats
                cpu_load = psutil.cpu_percent()
                per_core = self.get_usage_per_core()
                cpu_temp = self.get_cpu_temp()
                ram = self.get_ram_stats()
                disk = self.get_disk_stats()
                uptime = int(time.time() - psutil.boot_time())

                # Build and send all packets concurrently
                state_payload = self.pack_state(self._seq, cpu_load, per_core, cpu_temp, ram, disk, uptime)
                packet = self.build_packet(self.STATE_PACKET_TYPE(), state_payload)
                await self.send_packet(packet)
                self._seq = (self._seq + 1) & 0xFFFFFFFF

                await asyncio.sleep(refresh_interval)

            except Exception as e:
                self.logger.exception(f"Error in main loop: {e}")
                await asyncio.sleep(3)

        self.logger.info("SystemMonitorService stopped cleanly.")

    # --------------------------
    # Packet Building
    # --------------------------
    def calc_checksum(self, data: bytes) -> int:
        chk = 0
        for b in data:
            chk ^= b
        return chk

    def build_packet(self, packet_type: int, data: bytes) -> bytes:
        length = len(data) + 1  # Include checksum
        header = struct.pack("<BHB", self.PHEADER, length, packet_type)
        chk = self.calc_checksum(struct.pack("<H", length) + bytes([packet_type]) + data)
        return header + data + bytes([chk])

    def STATE_PACKET_TYPE(self) -> int:
        # Keep in sync with firmware PacketType::STATE = 6
        return 6

    # --------------------------
    # Data Packing
    # --------------------------
    def pack_state(self, seq, total, per_core, temp, ram, disk, uptime):
        buf = struct.pack("<I", int(seq))
        buf += struct.pack("<BB", int(total), len(per_core))
        buf += bytes(int(c) for c in per_core)
        buf += struct.pack("<B", int(temp))
        buf += struct.pack("<HH", ram["total"], ram["used"])
        buf += struct.pack("<QQ", int(disk["read_bytes"]), int(disk["write_bytes"]))
        buf += struct.pack("<I", int(uptime))
        return buf

    # --------------------------
    # System Info
    # --------------------------
    def get_usage_per_core(self):
        per_logical = psutil.cpu_percent(percpu=True)
        physical = psutil.cpu_count(logical=False) or len(per_logical)
        threads_per_core = max(len(per_logical) // max(physical, 1), 1)
        return [
            sum(per_logical[i * threads_per_core:(i + 1) * threads_per_core]) / threads_per_core
            for i in range(physical)
        ]

    def get_cpu_temp(self):
        temps = psutil.sensors_temperatures(fahrenheit=False)
        for _, entries in temps.items():
            for entry in entries:
                if "cpu" in entry.label.lower():
                    return int(entry.current)
        return 0

    def get_ram_stats(self):
        vm = psutil.virtual_memory()
        total = int(vm.total / (1024 * 1024))
        used = int(vm.used / (1024 * 1024))
        return {"total": total, "used": used}

    def get_disk_stats(self):
        now = time.time()
        counters = psutil.disk_io_counters()
        if self._disk_last_time == 0:
            self._disk_last_time = now
            if counters:
                self._disk_last_value = {"read_bytes": counters.read_bytes, "write_bytes": counters.write_bytes}
            return {"read_bytes": 0, "write_bytes": 0}

        elapsed = now - self._disk_last_time
        if elapsed >= 3.0 and counters:
            self.previousDelta = {
                "read_bytes": max((counters.read_bytes - self._disk_last_value.get("read_bytes", 0)) / elapsed, 0),
                "write_bytes": max((counters.write_bytes - self._disk_last_value.get("write_bytes", 0)) / elapsed, 0),
            }
            self._disk_last_time = now
            self._disk_last_value = {"read_bytes": counters.read_bytes, "write_bytes": counters.write_bytes}
        return self.previousDelta

    # --------------------------
    # Network Handling
    # --------------------------
    async def ensure_connected(self):
        """Ensure UDP socket exists and ESP32 data port discovered."""
        if self.udp_sock:
            return

        self.logger.info("Searching for ESP32...")

        while self.running and not self.udp_sock:
            result = await self.listen_for_esp32(timeout=10)
            self.esp32_ip, self.esp32_port = result if result else (None, None)
            if not self.esp32_ip:
                self.logger.info("No ESP32 found, retrying...")
                await asyncio.sleep(5)
                continue

            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.setblocking(False)
                self.udp_sock = s
                self.logger.info(f"UDP ready for {self.esp32_ip}:{self.esp32_port}")
            except Exception as e:
                self.logger.warning(f"Failed to prepare UDP socket: {e}")
                await asyncio.sleep(5)

    async def send_packet(self, data: bytes):
        if not self.udp_sock:
            return
        try:
            # fire-and-forget UDP send
            if not self.esp32_ip or not self.esp32_port:
                return
            self.udp_sock.sendto(data, (self.esp32_ip, int(self.esp32_port)))
        except Exception:
            self.logger.warning("UDP send failed; will rediscover.")
            if self.udp_sock:
                try:
                    self.udp_sock.close()
                except Exception:
                    pass
            self.udp_sock = None

    async def listen_for_esp32(self, timeout=10):
        """Wait for UDP broadcast: SYSMN_INFO <ip> <port>"""
        loop = asyncio.get_running_loop()
        self.logger.info("Listening for ESP32 broadcast...")

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", self.BROADCAST_PORT))
            s.setblocking(False)

            try:
                data, addr = await asyncio.wait_for(loop.sock_recvfrom(s, 1024), timeout)
                msg = data.decode(errors="ignore").strip("\x00")
                if msg.startswith("SYSMN_INFO"):
                    _, ip, port = msg.split()
                    self.logger.info(f"Discovered ESP32 at {ip}:{port}")
                    return ip, port
            except asyncio.TimeoutError:
                return None
        return None


# --------------------------
# Entrypoint
# --------------------------
async def main():
    service = SystemMonitorService()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, service.stop)
    await service.start()


if __name__ == "__main__":
    asyncio.run(main())
