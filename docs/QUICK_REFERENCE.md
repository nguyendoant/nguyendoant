# Quick Reference Guide

## Building

```bash
# Build project
make all

# Clean
make clean

# Run specific target
make run-simulate
make run-server
make run-client
```

## VS Code Shortcuts

| Action | Shortcut |
|--------|----------|
| Build | `Cmd+Shift+B` |
| Debug | `F5` |
| Run Task | `Cmd+Shift+P` → "Tasks: Run Task" |
| Terminal | `` Ctrl+` `` |

## Common Commands

```bash
# View HL7 examples
./hl7_app simulate

# Start server (Terminal 1)
./hl7_app server

# Run client (Terminal 2)
./hl7_app client

# Full benchmark
./hl7_app benchmark

# Stress test
./hl7_app stress
```

## Troubleshooting

### OpenSSL Not Found
```bash
brew install openssl@3
```

### Port In Use
```bash
lsof -i :2575
kill -9 <PID>
```

### Build Errors
```bash
make clean
make all
```
