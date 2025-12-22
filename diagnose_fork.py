#!/usr/bin/env python3
import subprocess
import sys
import time
import re

def main():
    print("Starting xv6-gdb...")
    proc = subprocess.Popen(
        ['make', 'qemu-gdb', 'CPUS=1'],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.PIPE,
        cwd='/home/kevin/labs/os_demo/xv6-labs-2025'
    )
    
    print("Waiting for init...")
    time.sleep(4)
    
    print("Sending command: sandbox 32768 - sh < exec.sh")
    proc.stdin.write(b'sandbox 32768 - sh < exec.sh\n')
    proc.stdin.flush()
    
    print("Waiting for command to complete...")
    time.sleep(5)
    
    print("Sending: echo DONE")
    proc.stdin.write(b'echo DONE\n')
    proc.stdin.flush()
    
    print("Reading output for 10 seconds...")
    start = time.time()
    output = []
    
    while time.time() - start < 10:
        line = proc.stdout.readline()
        if line:
            line_str = line.decode('utf-8', errors='replace').strip()
            output.append(line_str)
            print(f"  > {line_str}")
            if 'DONE' in line_str:
                print("\n✓ Found 'DONE'!")
                break
    
    proc.terminate()
    proc.wait(timeout=2)
    
    print("\n=== Analysis ===")
    hello_count = sum(1 for l in output if 'hello world' in l)
    cannot_count = sum(1 for l in output if 'cannot open' in l)
    done_count = sum(1 for l in output if 'DONE' in l)
    
    print(f"'hello world' appearances: {hello_count} (expected: 1)")
    print(f"'cannot open' appearances: {cannot_count} (expected: 1)")
    print(f"'DONE' appearances: {done_count} (expected: 1)")

if __name__ == '__main__':
    main()
