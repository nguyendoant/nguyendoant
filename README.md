# HL7 Low-Latency TCP/IP with TLS

A high-performance networking library for healthcare data exchange with HL7 v2.5 support.

## Features

- **Low-latency TCP/IP** - Optimized socket options for minimal latency
- **TLS/SSL encryption** - Secure data transmission with OpenSSL
- **HL7 v2.5 support** - Complete laboratory panel support
- **MLLP framing** - Proper HL7 message framing
- **Performance benchmarking** - Built-in latency and throughput tests

## Quick Start

```bash
# Run quick start script
./scripts/quick_start.sh

# Or manual steps:
make all
./hl7_app simulate
```

## Usage

```bash
./hl7_app simulate   # View HL7 message examples
./hl7_app server     # Start server
./hl7_app client     # Run benchmark
./hl7_app benchmark  # Full test suite
./hl7_app stress     # Multi-client test
```

## Building

### Prerequisites
- macOS with Xcode Command Line Tools
- Homebrew
- OpenSSL 3.x

### Build Commands
```bash
make all        # Build project
make clean      # Clean build files
make help       # Show all targets
```

### VS Code
1. Open project: `code .`
2. Build: Press `Cmd+Shift+B`
3. Debug: Press `F5`

## Project Structure

```
hl7-lowlatency-tcp/
├── include/          # Header files
│   └── hl7_tcp.hpp   # Main library
├── src/              # Source files
│   └── main.cpp      # Main application
├── scripts/          # Build scripts
├── certs/            # SSL certificates
├── docs/             # Documentation
└── .vscode/          # VS Code config
```

## Documentation

- [BUILD.md](docs/BUILD.md) - Detailed build instructions
- [API.md](docs/API.md) - API reference
- [EXAMPLES.md](docs/EXAMPLES.md) - Code examples

## Performance

Typical performance on macOS (localhost):
- **Latency**: 50-200 µs
- **Throughput**: 10,000-50,000 msg/s

## License

MIT License - See LICENSE file

## Author

Healthcare IT Team
Version 1.0.0
