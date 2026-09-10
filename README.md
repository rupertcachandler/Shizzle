# ndi-testgen for Linux - Debian Release

NDI test pattern generator for Linux systems— emits SMPTE colour bars, solid fills, animated patterns, and audio tones over NDI for testing receiver pipelines.

Single C file, one dependency: the NDI SDK.

## Build

```bash
# If NDI SDK is in /opt/ndi-monitor-v2:
make

# Or point to your SDK location:
make NDI_SDK=/path/to/ndi-sdk
```

## Usage

```bash
# SMPTE bars at 1080p30
ndi-testgen --pattern smpte75 --width 1920 --height 1080

# With audio
ndi-testgen --pattern smpte75 --audio

# 4K animated clock
ndi-testgen --pattern clock --width 3840 --height 2160

# Custom name and frame rate
ndi-testgen --pattern gradient --name "My Generator" --fps 60
```

## Patterns

| Pattern | Description |
|---------|-------------|
| `smpte75` | SMPTE 75% colour bars (standard) |
| `smpte100` | SMPTE 100% colour bars |
| `white` | Solid white |
| `black` | Solid black |
| `red` / `green` / `blue` | Solid colour fills |
| `gradient` | Horizontal luminance ramp |
| `clock` | Animated clock with second hand |
| `bars` | Animated moving bars |

## Options

```
--pattern PAT   Test pattern type (default: smpte75)
--width N       Frame width (default: 1920)
--height N      Frame height (default: 1080)
--fps N         Frames per second (default: 30)
--audio         Enable audio tone generation
--name NAME     NDI source name (default: hostname + " (Test Pattern)")
--connections N Max NDI connections (default: 16)
--bw LEVEL      NDI bandwidth: lowest/normal/highest (default: lowest)
--verbose       Verbose logging
```

## Requirements

- NDI SDK 6.x (headers + libndi.so.6)
- GCC or Clang with C11 support

## License

MIT
