# High Availability and Memory Leak Detection

## Overview

This document describes the high availability features and memory leak detection tools added to ssh-chatter to ensure uninterrupted service operation.

## Problem Statement

The BBS system was experiencing crashes where both SSH and Telnet services would die after running for several hours and never restart, despite having restart logic in place. Additionally, there was no systematic way to detect memory leaks that could cause long-term stability issues.

## Solutions Implemented

### 1. Fixed Auto-Restart Logic

**Problem**: The main daemon loop would exit successfully (instead of restarting) when `host_serve` returned 0, even if the service crashed unexpectedly.

**Solution**: 
- Modified the main loop in `src/main.c` to check the `g_shutdown_flag` before exiting
- Added a `shutdown_flag` pointer to the `host_t` structure
- Updated all service loops (SSH listener, Telnet listener) to check the shutdown flag
- Now the service only exits on explicit SIGINT/SIGTERM signals, otherwise it always restarts

**Changed Files**:
- `src/main.c`: Main restart loop now checks `g_shutdown_flag`
- `include/ssh_chatter/host.h`: Added `shutdown_flag` field to `host_t` structure
- `src/host_parts/host_runtime.c`: Updated SSH and Telnet loops to check shutdown flag

### 2. Enhanced Systemd Service Configuration

**Problem**: The systemd service only restarted on explicit failures, not on unexpected exits.

**Solution**: Updated `example.service` with:
- `Restart=always` - Always restart the service regardless of exit status
- `RestartSec=3s` - Wait 3 seconds before restarting (prevents restart storm)
- `StartLimitBurst=10` - Allow up to 10 restarts in the interval
- `StartLimitIntervalSec=300` - 5-minute window for restart limits
- `WatchdogSec=60s` - Restart if service becomes unresponsive (future watchdog support)
- `TimeoutStartSec=30s` / `TimeoutStopSec=30s` - Reasonable timeouts

**Changed Files**:
- `example.service`: Enhanced with high-availability restart policies

### 3. Valgrind Memory Leak Detection

**Problem**: No systematic way to detect memory leaks, and false positives from libgc (Boehm Garbage Collector) would make manual valgrind runs difficult.

**Solution**: Added comprehensive valgrind support:

#### a. Valgrind Suppression File (`valgrind-libgc.supp`)
- Suppresses known false positives from libgc
- Suppresses reachable memory from system libraries (libssh, libcurl, libcrypto)
- Allows detection of real memory leaks in application code

#### b. Automated Valgrind Test Script (`scripts/run_valgrind_check.sh`)
Features:
- Automatically runs ssh-chatter under valgrind for a specified duration (default 30 seconds)
- Uses the suppression file to filter false positives
- Creates temporary SSH keys and MOTD for isolated testing
- Provides detailed analysis of memory leaks and errors
- Returns exit code 0 for pass, 1 for failure (CI/CD friendly)

Usage:
```bash
# Run with defaults (30 seconds)
./scripts/run_valgrind_check.sh

# Run for longer duration
./scripts/run_valgrind_check.sh --duration 300

# Quick check (less detailed)
./scripts/run_valgrind_check.sh --quick

# Show reachable memory (normally suppressed)
./scripts/run_valgrind_check.sh --show-reachable
```

**New Files**:
- `valgrind-libgc.supp`: Suppression file for libgc false positives
- `scripts/run_valgrind_check.sh`: Automated valgrind testing script

## How to Use

### For Production Deployment

1. **Update systemd service**:
   ```bash
   sudo cp example.service /etc/systemd/system/chatter.service
   sudo systemctl daemon-reload
   sudo systemctl restart chatter.service
   ```

2. **Monitor service health**:
   ```bash
   # Check service status
   sudo systemctl status chatter.service
   
   # View recent logs
   sudo journalctl -u chatter.service -n 100 -f
   
   # Check restart count
   sudo systemctl show chatter.service -p NRestarts
   ```

3. **Verify auto-restart is working**:
   ```bash
   # Kill the process (systemd will restart it)
   sudo systemctl kill -s KILL chatter.service
   
   # Wait a few seconds and check status
   sleep 5
   sudo systemctl status chatter.service
   ```

### For Memory Leak Testing

1. **Run valgrind check during development**:
   ```bash
   # Quick 30-second test
   ./scripts/run_valgrind_check.sh
   ```

2. **Run extended test**:
   ```bash
   # 5-minute test (300 seconds)
   ./scripts/run_valgrind_check.sh --duration 300
   ```

3. **Connect to test server while it's running**:
   ```bash
   # In another terminal
   ssh -p 2222 testuser@localhost
   ```

4. **Interpret results**:
   - ✅ **PASSED**: No critical memory issues
   - ⚠️ **WARNING**: Memory errors detected (investigate)
   - ❌ **FAILED**: Definite memory leaks found (must fix)

### For Continuous Integration

Add to your CI pipeline:
```bash
#!/bin/bash
set -e

# Build the project
make clean && make

# Run valgrind memory leak check
./scripts/run_valgrind_check.sh --duration 60

# If valgrind passes, proceed with other tests
echo "Memory leak check passed!"
```

## Technical Details

### Restart Flow

1. **Main Loop** (`src/main.c`):
   - Checks `g_shutdown_flag` before each iteration
   - Only exits on explicit shutdown signal
   - Clears restart backoff after 10 seconds of stable operation
   - Uses exponential backoff: 1s, 1s, 1s, 1s, 1s, 5s, 5s, 5s, 5s, 5s, 30s...

2. **SSH Listener Loop** (`host_runtime.inc`):
   - Outer loop: Recreates ssh_bind on fatal socket errors
   - Inner loop: Accepts SSH connections
   - Both loops check `shutdown_flag` to exit gracefully on SIGTERM

3. **Telnet Listener Thread** (`host_runtime.inc`):
   - Runs in separate thread
   - Checks `shutdown_flag` and `telnet.stop` flag
   - Automatically restarts socket on fatal errors
   - Same exponential backoff as main loop

### Memory Management with libgc

The project uses Boehm GC (`libgc`) which:
- Automatically collects unreachable memory
- Marks some memory as "reachable" even at exit (this is normal)
- Can cause false positive leak reports in valgrind

Our suppression file handles these false positives while still detecting real leaks in application code.

## Monitoring and Alerts

### Recommended Monitoring Setup

1. **System-level monitoring**:
   ```bash
   # Monitor restart count
   watch -n 5 'systemctl show chatter.service -p NRestarts'
   
   # Monitor memory usage
   watch -n 5 'ps aux | grep ssh-chatter'
   ```

2. **Log monitoring**:
   ```bash
   # Alert on restart messages
   sudo journalctl -u chatter.service -f | grep -i "restart\|fatal\|error"
   ```

3. **External monitoring** (recommended):
   - Monitor port 2222 with a health check every minute
   - Alert if service is unreachable for >3 minutes
   - Track restart frequency - alert if >5 restarts/hour

### Expected Behavior

- **Normal operation**: Service runs continuously with 0 restarts
- **Network issues**: Temporary connection errors logged, but service continues
- **Socket errors**: Listener restarts automatically within 1-30 seconds
- **Process crash**: Systemd restarts the entire process within 3 seconds
- **Manual restart**: Service stops gracefully and restarts cleanly

### Troubleshooting

1. **Service won't start**:
   ```bash
   sudo journalctl -u chatter.service -n 100
   # Check for missing host keys, permission issues, or port conflicts
   ```

2. **Frequent restarts**:
   ```bash
   # Check for memory leaks
   ./scripts/run_valgrind_check.sh --duration 300
   
   # Check for resource exhaustion
   sudo systemctl status chatter.service
   dmesg | tail -50
   ```

3. **Service stops responding but doesn't restart**:
   - This should not happen with WatchdogSec configured
   - Check systemd journal for watchdog messages
   - Consider implementing sd_notify() for better watchdog support

## Future Improvements

1. **Implement systemd watchdog notifications**:
   - Add `sd_notify()` calls to signal health to systemd
   - Allows systemd to detect deadlocks and restart automatically

2. **Add health check endpoint**:
   - Expose a simple TCP port that responds to health checks
   - External monitoring can verify service is truly operational

3. **Metrics and telemetry**:
   - Export metrics about connection count, restart frequency
   - Track memory usage over time

4. **Automatic memory leak detection in CI**:
   - Run valgrind check on every commit
   - Fail build if leaks are detected

## References

- Main daemon loop: `src/main.c` lines 286-430
- SSH listener: `src/host_parts/host_runtime.c` lines 4339-4868
- Telnet listener: `src/host_parts/host_runtime.c` lines 2462-2712
- Systemd service: `example.service`
- Valgrind suppression: `valgrind-libgc.supp`
- Test script: `scripts/run_valgrind_check.sh`
