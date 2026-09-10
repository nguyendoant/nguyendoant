#!/bin/bash

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║     HL7 Low-Latency TCP/IP - Quick Start for Mac             ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Step 1: Check dependencies
echo "Step 1: Checking dependencies..."
echo ""

if ! command -v brew &> /dev/null; then
    echo "❌ Homebrew not found"
    echo "Install: /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
    exit 1
fi
echo "  ✓ Homebrew found"

if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo "❌ C++ compiler not found"
    echo "Install: xcode-select --install"
    exit 1
fi
echo "  ✓ C++ compiler found"

if ! brew list openssl@3 &> /dev/null 2>&1; then
    echo "⚠️  OpenSSL not found. Installing..."
    brew install openssl@3
fi
echo "  ✓ OpenSSL found"

# Step 2: Generate certificates
echo ""
echo "Step 2: Generating SSL certificates..."
./scripts/generate_certs.sh > /dev/null 2>&1
echo "  ✓ Certificates generated"

# Step 3: Build
echo ""
echo "Step 3: Building project..."
make clean > /dev/null 2>&1
make all

if [ $? -ne 0 ]; then
    echo "❌ Build failed"
    exit 1
fi
echo "  ✓ Build successful"

# Step 4: Run demo
echo ""
echo "Step 4: Running HL7 message simulation..."
echo ""
./hl7_app simulate

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                   ✓ Setup Complete!                           ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Next steps:"
echo ""
echo "  1. Open in VS Code:"
echo "     code ."
echo ""
echo "  2. Build (in VS Code):"
echo "     Press Cmd+Shift+B"
echo ""
echo "  3. Run examples:"
echo "     ./hl7_app server      # Terminal 1"
echo "     ./hl7_app client      # Terminal 2"
echo ""
echo "  4. Debug (in VS Code):"
echo "     Press F5"
echo ""
