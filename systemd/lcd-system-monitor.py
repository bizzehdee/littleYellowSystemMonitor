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
        self.sock_writer: asyncio.StreamWriter | None = None
        self.esp32_ip = None
        self.esp32_port = None
        self.running = False
        self.previousDelta = {"read_bytes": 0, "write_bytes": 0}
        self.logger = self._setup_logger()

        self._disk_last_time = 0.0
        self._disk_last_value = {"read_bytes": 0, "write_bytes": 0}

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
        if self.sock_writer:
            self.sock_writer.close()

    # --------------------------
    # Main Loop
    # --------------------------
    async def run(self):
        self.logger.info("SystemMonitorService started.")
        refresh_interval = 1 / self.REFRESH_FPS

        while self.running:
            try:
                await self.ensure_connected()
                if not self.sock_writer:
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
                packets = [
                    self.build_packet(self.PTYPE_CPU, self.pack_cpu_load(cpu_load, per_core)),
                    self.build_packet(self.PTYPE_TEMP, self.pack_cpu_temp(cpu_temp)),
                    self.build_packet(self.PTYPE_RAM, self.pack_ram(ram)),
                    self.build_packet(self.PTYPE_DISK, self.pack_disk(disk)),
                    self.build_packet(self.PTYPE_UPTIME, self.pack_uptime(uptime)),
                ]

                for p in packets:
                    await self.send_packet(p)

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

    # --------------------------
    # Data Packing
    # --------------------------
    def pack_cpu_load(self, total, per_core):
        buf = struct.pack("<BB", int(total), len(per_core))
        buf += bytes(int(c) for c in per_core)
        return buf

    def pack_cpu_temp(self, temp):
        return struct.pack("<B", int(temp))

    def pack_ram(self, ram):
        return struct.pack("<HH", ram["total"], ram["used"])

    def pack_disk(self, disk):
        return struct.pack("<QQ", int(disk["read_bytes"]), int(disk["write_bytes"]))

    def pack_uptime(self, uptime):
        return struct.pack("<I", uptime)

    # --------------------------
    # System Info
    # --------------------------
    def get_usage_per_core(self):
        per_logical = psutil.cpu_percent(percpu=True)
        physical = psutil.cpu_count(logical=False)
        threads_per_core = len(per_logical) // physical
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
            self._disk_last_value = {"read_bytes": counters.read_bytes, "write_bytes": counters.write_bytes}
            return {"read_bytes": 0, "write_bytes": 0}

        elapsed = now - self._disk_last_time
        if elapsed >= 3.0:
            self.previousDelta = {
                "read_bytes": max((counters.read_bytes - self._disk_last_value["read_bytes"]) / elapsed, 0),
                "write_bytes": max((counters.write_bytes - self._disk_last_value["write_bytes"]) / elapsed, 0),
            }
            self._disk_last_time = now
            self._disk_last_value = {"read_bytes": counters.read_bytes, "write_bytes": counters.write_bytes}
        return self.previousDelta

    # --------------------------
    # Network Handling
    # --------------------------
    async def ensure_connected(self):
        """Keep TCP connection alive, rediscover if lost."""
        if self.sock_writer and not self.sock_writer.is_closing():
            return

        self.sock_writer = None
        self.logger.info("Searching for ESP32...")

        while self.running and not self.sock_writer:
            self.esp32_ip, self.esp32_port = await self.listen_for_esp32(timeout=10)
            if not self.esp32_ip:
                self.logger.info("No ESP32 found, retrying...")
                await asyncio.sleep(5)
                continue

            try:
                reader, writer = await asyncio.open_connection(self.esp32_ip, self.esp32_port)
                writer.transport.set_write_buffer_limits(0)
                self.sock_writer = writer
                self.logger.info(f"Connected to ESP32 at {self.esp32_ip}:{self.esp32_port}")
            except Exception as e:
                self.logger.warning(f"Failed to connect to ESP32: {e}")
                await asyncio.sleep(5)

    async def send_packet(self, data: bytes):
        if not self.sock_writer:
            return
        try:
            self.sock_writer.write(data)
            await self.sock_writer.drain()
        except Exception:
            self.logger.warning("Lost connection to ESP32.")
            if self.sock_writer:
                self.sock_writer.close()
            self.sock_writer = None

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
