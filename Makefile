# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O3 -pthread

# Detect OS + architecture and set OpenSSL paths accordingly.
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
    ifeq ($(UNAME_M),arm64)
        OPENSSL_PREFIX = /opt/homebrew/opt/openssl@3
    else
        OPENSSL_PREFIX = /usr/local/opt/openssl@3
    endif
    INCLUDES = -I./include -I$(OPENSSL_PREFIX)/include
    LDFLAGS = -L$(OPENSSL_PREFIX)/lib
else
    # Linux (incl. the RHEL10/UBI10 dev container): OpenSSL headers/libs
    # from openssl-devel land in standard system paths, no prefix needed.
    INCLUDES = -I./include
    LDFLAGS =
endif

LIBS = -lssl -lcrypto

# Source and output
SRC = src/main.cpp
TARGET = hl7_app

.PHONY: all clean run help

all: $(TARGET)

$(TARGET): $(SRC)
	@echo "Building HL7 Low-Latency TCP/IP..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) -o $(TARGET) $(LDFLAGS) $(LIBS)
	@echo "Build successful! Binary: ./$(TARGET)"

clean:
	@echo "Cleaning build artifacts..."
	rm -f $(TARGET)
	@echo "Clean complete"

run-simulate: $(TARGET)
	./$(TARGET) simulate

run-server: $(TARGET)
	./$(TARGET) server

run-client: $(TARGET)
	./$(TARGET) client

run-benchmark: $(TARGET)
	./$(TARGET) benchmark

run-stress: $(TARGET)
	./$(TARGET) stress

help:
	@echo "HL7 Low-Latency TCP/IP - Makefile Help"
	@echo ""
	@echo "Targets:"
	@echo "  make all           - Build the project"
	@echo "  make clean         - Remove build artifacts"
	@echo "  make run-simulate  - Run HL7 message simulation"
	@echo "  make run-server    - Start HL7 server"
	@echo "  make run-client    - Run client benchmark"
	@echo "  make run-benchmark - Run full benchmark"
	@echo "  make run-stress    - Run stress test"
	@echo "  make help          - Show this help"
