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

UNAME_S="$(uname -s)"
if [ "$UNAME_S" = "Darwin" ]; then
    # macOS: this project links against Homebrew's OpenSSL (see Makefile).
    if ! brew list openssl@3 &> /dev/null 2>&1; then
        echo "❌ OpenSSL not found"
        echo "Install with: brew install openssl@3"
        exit 1
    fi
else
    # Linux (e.g. inside the RHEL10 dev container): no Homebrew here, so
    # actually try compiling+linking against OpenSSL instead of checking
    # for a package manager that doesn't exist on this platform.
    OPENSSL_CHECK_BIN="$(mktemp /tmp/hl7_openssl_check.XXXXXX)"
    if ! echo '#include <openssl/ssl.h>
int main(){return 0;}' | g++ -x c++ - -o "$OPENSSL_CHECK_BIN" -lssl -lcrypto 2>/dev/null; then
        rm -f "$OPENSSL_CHECK_BIN"
        echo "❌ OpenSSL dev headers/libs not found"
        echo "Install with: sudo dnf install -y openssl-devel   (RHEL/UBI/Fedora)"
        echo "           or: sudo apt install -y libssl-dev     (Debian/Ubuntu)"
        exit 1
    fi
    rm -f "$OPENSSL_CHECK_BIN"
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
