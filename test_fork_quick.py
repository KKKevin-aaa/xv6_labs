#!/usr/bin/env python3
"""
Quick test to see if sandbox_fork works
"""
import subprocess
import time
import signal
import os

def test_sandbox_fork():
    # Start qemu
    print("Starting xv6...")
    proc = subprocess.Popen(
        ['make', 'qemu-gdb', 'CPUS=1'],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.PIPE,
        cwd='/home/kevin/labs/os_demo/xv6-labs-2025'
    )
    
    time.sleep(3)  # Wait for boot
    
    # Send commands
    print("Sending commands...")
    commands = b'sandbox 32768 - sh < exec.sh\necho DONE\n'
    proc.stdin.write(commands)
    proc.stdin.flush()
    
    # Wait and collect output
    start_time = time.time()
    output = b''
    timeout = 15
    
    while time.time() - start_time < timeout:
        chunk = proc.stdout.read(1024)
        if chunk:
            output += chunk
            print(chunk.decode('utf-8', errors='replace'), end='', flush=True)
            if b'DONE' in output:
                print(f"\n\n✓ Test passed in {time.time() - start_time:.1f}s")
                proc.terminate()
                return True
        time.sleep(0.1)
    
    print(f"\n\n✗ Test TIMEOUT after {timeout}s")
    proc.terminate()
    return False

if __name__ == '__main__':
    test_sandbox_fork()
