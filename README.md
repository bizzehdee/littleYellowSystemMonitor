# Little Yellow System Monitor

![Mockup of the LCD UI showing everything at max usage](yellow-lcd-mockup.png "Mockup")

A project I decided to make because I bought one of these cheap AliExpress LCD screens and didnt know what else I should do with it.

A real-time system monitoring solution with **zero latency buildup** and no backpressure. The ESP32 device shows CPU load (overall and per-core), CPU temperature, RAM usage, disk I/O rates, and system uptime on a 320x240 TFT display with smooth gradient bars and live updating graphs at 20 FPS.

### Key Features

- **UDP-based communication** for minimal latency and live updates
- **Sequence number tracking** to automatically drop old/delayed packets
- **WiFi configuration** via access point mode with web interface
- **Auto-discovery** using UDP broadcast beacons
- **20 FPS refresh rate** for smooth, responsive UI
- **Per-core CPU visualization** with vertical bars
- **Gradient progress bars** for CPU and temperature metrics
- **Automatic scaling** for disk I/O display (KB/s, MB/s, GB/s)

## What it does

On the first run, the ESP32 creates a Wifi AP that you connect to (with your computer, phone or laptop), which asks you for your Wifi SSID and password.

Once the Wifi details are competed, they are saved to persistant storage on the ESP32, and it reboots into connected mode.

It then starts to broadcast its availability on the network via UDP beacons every 5 seconds.

The service sits in the background and listens for the broadcast, and when it sees a broadcast, it automatically connects to the ESP32.

Once connected, the service starts sending system stats as a single consolidated UDP packet per frame (20 FPS) to the ESP32 for it to process and display. Old/delayed packets are automatically dropped using sequence numbers, ensuring the display always shows the most recent data with zero latency buildup.

The ESP32 will go to the timeout/info screen after 10 seconds of inactivity.

## How to use it

### The service

The service is currently a systemd service for linux only, and all you need to do is open a terminal, cd to the systemd directory of this project, and run `sudo make install`. No configuration needed.

#### Manual Installation

1. **Install Dependencies**
   ```bash
   pip install psutil
   ```

2. **Run Service**
   ```bash
   python3 systemd/lcd-system-monitor.py
   ```

   The service will:
   - Listen for ESP32 broadcast beacons on port 33333
   - Automatically connect when an ESP32 is discovered
   - Start streaming metrics at 20 FPS

### The firmware

Requires [PlatformIO](https://platformio.org/) for VSCode.

1. **Install PlatformIO** (if not already installed)
   ```bash
   pip install platformio
   ```

2. **Configure TFT Display** (if needed)
   Edit `firmware/src/_User_Setup.h` to match your display configuration (pins, driver, etc.)

3. **Build and Upload**
   
   Should be able to just load up the firmware directory in PlatformIO and build/install the firmware directly to your ESP32.
   
   Or from command line:
   ```bash
   cd firmware
   platformio run --target upload
   ```

4. **First-Time WiFi Configuration**
   - The ESP32 will create an access point named `ESP32_Config_AP`
   - Connect to it and navigate to `192.168.4.1`
   - Enter your WiFi SSID and password
   - Device will restart and connect to your network

## Architecture

### Hardware Requirements

- ESP32 development board (tested on ESP32 Dev Module)
- 320x240 TFT display (TFT_eSPI compatible)
- Backlight control on GPIO 21

### Network Protocol

#### Discovery (Broadcast)
The ESP32 sends UDP broadcast beacons every 5 seconds:
```
Port: 33333
Message: "SYSMN_INFO <ip> <port>"
```

The Python service listens for these beacons and extracts the ESP32's IP and data port.

#### Data Transmission (Unicast)
The Python service sends STATE packets via UDP to the ESP32's data port (3333).

#### Packet Format

All packets use a common framing structure:
```
Header: 0xFA | Length (LE uint16) | Type (uint8)
Payload: <type-specific data>
Checksum: XOR of (length bytes + type byte + all payload bytes except checksum)
```

**STATE Packet (Type 6)** - Single UDP datagram containing all metrics:
```
seq:        uint32  - Monotonic sequence number
cpu_total:  uint8   - Overall CPU load (0-100%)
core_count: uint8   - Number of physical cores (1-64)
core_loads: uint8[] - Per-core load percentages
cpu_temp:   uint8   - CPU temperature in Celsius
ram_total:  uint16  - Total RAM in MB
ram_used:   uint16  - Used RAM in MB
disk_read:  uint64  - Disk read bytes per second
disk_write: uint64  - Disk write bytes per second
uptime:     uint32  - System uptime in seconds
```

#### Sequence Number Behavior
- Python increments sequence number with each frame (wraps at 2^32)
- ESP32 drops any packet with `seq <= lastSequence`
- Ensures only the most recent data is displayed
- Prevents latency buildup and backpressure

## Troubleshooting

### ESP32 shows "Waiting for data..."
- Ensure Python service is running
- Check that both devices are on the same network
- Verify UDP port 33333 and 3333 are not blocked by firewall
- Check service logs for discovery messages

### Display shows stale/frozen data
- Service may have stopped or lost connection
- Check network stability
- Restart Python service
- ESP32 will auto-switch to Info mode after 10s timeout

### Build errors
- Verify TFT_eSPI library is installed
- Check `_User_Setup.h` display configuration
- Ensure PlatformIO platform is up to date

## Performance

- **ESP32 Memory Usage**: 14.1% RAM (46KB), 63.8% Flash (835KB)
- **Network Bandwidth**: ~40-60 packets/second (STATE frames + occasional broadcasts)
- **Packet Size**: ~40-60 bytes depending on core count
- **Display Latency**: <50ms from metric collection to screen update

## Configuration

Key constants in `firmware/include/constants.h`:

- `MAX_PAYLOAD_SIZE`: Maximum packet payload (default: 64 bytes)
- `BROADCAST_PORT`: UDP discovery port (default: 33333)
- `TCP_PORT`: UDP data port (default: 3333) - legacy name, now UDP
- `targetRefreshRate`: Display refresh rate (default: 20 FPS)
- `screenWidth`, `screenHeight`: Display dimensions (320x240)

For systems with >32 cores, increase `MAX_PAYLOAD_SIZE` to 128 bytes.

## TODO

* Support more platforms (rp2040? + other screens)
* Support Linux OpenRC
* Support Windows

## Links to get your own cheap yellow ESP32

* [Little Yellow LCD 1](https://s.click.aliexpress.com/e/_c43pB65l)
* [Little Yellow LCD 2](https://s.click.aliexpress.com/e/_c3FNQDVZ)
* [Little Yellow LCD 3](https://s.click.aliexpress.com/e/_c3QTqTht)
