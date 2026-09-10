#!/bin/bash

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║         Building HL7 Low-Latency TCP/IP Project               ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Check dependencies
if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo "❌ No C++ compiler found"
    echo "Install with: xcode-select --install"
    exit 1
fi

if ! brew list openssl@3 &> /dev/null 2>&1; then
    echo "❌ OpenSSL not found"
    echo "Install with: brew install openssl@3"
    exit 1
fi

# Build
make clean
make all

if [ $? -eq 0 ]; then
    echo ""
    echo "╔════════════════════════════════════════════════════════════════╗"
    echo "║                  ✓ Build Successful!                          ║"
    echo "╚════════════════════════════════════════════════════════════════╝"
    echo ""
    echo "Run examples:"
    echo "  ./hl7_app simulate   - View HL7 message examples"
    echo "  ./hl7_app server     - Start HL7 server"
    echo "  ./hl7_app client     - Run client benchmark"
    echo "  ./hl7_app benchmark  - Full benchmark suite"
    echo "  ./hl7_app stress     - Stress test"
    echo ""
else
    echo "❌ Build failed!"
    exit 1
fi
