#!/usr/bin/env python3
"""
Detailed performance profiling for sandbox_fork test
Measures exact timing of different phases
"""
import subprocess
import time
import sys
import re
import os

def run_cmd_with_timing(cmd, desc, cwd=None):
    """Run command and return (success, elapsed_time, output)"""
    print(f"\n{'='*70}")
    print(f"[TIMING] {desc}")
    print(f"[CMD] {cmd}")
    print(f"{'='*70}")
    
    start = time.time()
    try:
        result = subprocess.run(
            cmd, 
            shell=True, 
            capture_output=True, 
            text=True, 
            timeout=50,
            cwd=cwd or os.getcwd()
        )
        elapsed = time.time() - start
        success = result.returncode == 0
        output = result.stdout + result.stderr
        
        print(f"[STATUS] {'✓ SUCCESS' if success else '✗ FAILED'}")
        print(f"[TIME] {elapsed:.3f}s")
        print(f"[EXIT CODE] {result.returncode}")
        
        return success, elapsed, output
    except subprocess.TimeoutExpired as e:
        elapsed = time.time() - start
        print(f"[STATUS] ✗ TIMEOUT after {elapsed:.1f}s")
        return False, elapsed, str(e)

def analyze_output(output):
    """Analyze test output for performance indicators"""
    print(f"\n{'='*70}")
    print("[OUTPUT ANALYSIS]")
    print(f"{'='*70}")
    
    lines = output.split('\n')
    
    # Count different message types
    perf_fork = sum(1 for l in lines if 'PERF: fork' in l)
    perf_exec = sum(1 for l in lines if 'PERF: exec' in l)
    syscall_restricted = sum(1 for l in lines if 'syscall' in l and 'restricted' in l)
    open_restricted = sum(1 for l in lines if 'open or exec is restricted' in l)
    in_freeproc = sum(1 for l in lines if 'in freeproc' in l)
    done_free = sum(1 for l in lines if 'Done free' in l)
    in_fork = sum(1 for l in lines if 'In fork now' in l)
    
    print(f"PERF: fork messages:           {perf_fork:6d}")
    print(f"PERF: exec messages:           {perf_exec:6d}")
    print(f"'syscall restricted' messages: {syscall_restricted:6d}")
    print(f"'open restricted' messages:    {open_restricted:6d}")
    print(f"'in freeproc' messages:        {in_freeproc:6d}")
    print(f"'Done free' messages:          {done_free:6d}")
    print(f"'In fork now' messages:        {in_fork:6d}")
    print(f"Total output lines:            {len(lines):6d}")
    
    # Extract PERF cycle counts
    fork_cycles = []
    exec_cycles = []
    
    for line in lines:
        if 'PERF: fork' in line:
            match = re.search(r'took (\d+) cycles', line)
            if match:
                fork_cycles.append(int(match.group(1)))
        elif 'PERF: exec' in line:
            match = re.search(r'took (\d+) cycles', line)
            if match:
                exec_cycles.append(int(match.group(1)))
    
    if fork_cycles:
        print(f"\n[FORK CYCLE ANALYSIS]")
        print(f"  Count: {len(fork_cycles)}")
        print(f"  Min:   {min(fork_cycles):,} cycles")
        print(f"  Max:   {max(fork_cycles):,} cycles")
        print(f"  Avg:   {sum(fork_cycles)//len(fork_cycles):,} cycles")
        print(f"  Total: {sum(fork_cycles):,} cycles")
        
        # Estimate time (assuming 1GHz CPU = 1 cycle/ns)
        total_ms = sum(fork_cycles) / 1_000_000
        print(f"  Estimated time: {total_ms:.1f}ms")
    
    if exec_cycles:
        print(f"\n[EXEC CYCLE ANALYSIS]")
        print(f"  Count: {len(exec_cycles)}")
        print(f"  Min:   {min(exec_cycles):,} cycles")
        print(f"  Max:   {max(exec_cycles):,} cycles")
        print(f"  Avg:   {sum(exec_cycles)//len(exec_cycles):,} cycles")
        print(f"  Total: {sum(exec_cycles):,} cycles")
        
        total_ms = sum(exec_cycles) / 1_000_000
        print(f"  Estimated time: {total_ms:.1f}ms")
    
    # Identify bottlenecks
    print(f"\n[BOTTLENECK ANALYSIS]")
    
    total_debug_msgs = perf_fork + perf_exec + syscall_restricted + open_restricted + \
                       in_freeproc + done_free + in_fork
    
    if total_debug_msgs > 100:
        print(f"⚠️  CRITICAL: {total_debug_msgs} debug messages printed!")
        print(f"   Each printf to UART in QEMU is ~1-10ms")
        print(f"   Estimated overhead: {total_debug_msgs * 2}ms - {total_debug_msgs * 10}ms")
        print(f"   This is likely THE MAIN BOTTLENECK!")
    
    if perf_fork > 10 or perf_exec > 10:
        print(f"⚠️  Performance measurement overhead detected")
        print(f"   PROC_TEST_TIME and EXEC_TEST_TIME add cycles to hot path")
    
    if len(lines) > 1000:
        print(f"⚠️  Excessive output: {len(lines)} lines")
        print(f"   Console I/O is slow in QEMU")

def main():
    cwd = '/home/kevin/labs/os_demo/xv6-labs-2025'
    
    print("=" * 80)
    print("DETAILED FORK TEST PERFORMANCE PROFILING")
    print("=" * 80)
    
    # Step 1: Ensure clean build
    print("\n[STEP 1/4] Clean build check")
    subprocess.run(['pkill', '-9', 'qemu-system-riscv64'], 
                   capture_output=True)
    time.sleep(0.5)
    
    success, build_time, _ = run_cmd_with_timing(
        "make qemu 2>&1 | head -5",
        "Check if kernel needs rebuild",
        cwd
    )
    
    # Step 2: Run the test with detailed timing
    print("\n[STEP 2/4] Run sandbox_fork test")
    success, test_time, output = run_cmd_with_timing(
        "timeout 50 python3 grade-lab-syscall sandbox_fork 2>&1",
        "Execute sandbox_fork test",
        cwd
    )
    
    # Step 3: Analyze output
    print("\n[STEP 3/4] Analyze test output")
    analyze_output(output)
    
    # Step 4: Check current debug flags
    print(f"\n{'='*70}")
    print("[STEP 4/4] Check debug flags in source code")
    print(f"{'='*70}")
    
    # Check exec.c
    with open(f'{cwd}/kernel/exec.c', 'r') as f:
        exec_lines = f.readlines()
        for i, line in enumerate(exec_lines[:15], 1):
            if 'EXEC_DEBUG' in line or 'EXEC_TEST_TIME' in line:
                status = "ACTIVE" if not line.strip().startswith('//') else "DISABLED"
                print(f"exec.c:{i:3d} [{status:8s}] {line.rstrip()}")
    
    # Check proc.c
    with open(f'{cwd}/kernel/proc.c', 'r') as f:
        proc_lines = f.readlines()
        for i, line in enumerate(proc_lines[:15], 1):
            if 'PROC_DEBUG' in line or 'PROC_TEST_TIME' in line:
                status = "ACTIVE" if not line.strip().startswith('//') else "DISABLED"
                print(f"proc.c:{i:3d} [{status:8s}] {line.rstrip()}")
    
    # Final summary
    print(f"\n{'='*80}")
    print("SUMMARY & RECOMMENDATIONS")
    print(f"{'='*80}")
    print(f"Total test time: {test_time:.2f}s")
    
    if test_time > 30:
        print(f"\n❌ TEST TOO SLOW: {test_time:.1f}s > 30s timeout")
        print(f"\nRecommended fixes (in priority order):")
        print(f"1. DISABLE EXEC_TEST_TIME and PROC_TEST_TIME")
        print(f"   - Comment out '#define EXEC_TEST_TIME' in exec.c line 10")
        print(f"   - Comment out '#define PROC_TEST_TIME' in proc.c line 9")
        print(f"   - These printf calls are IN THE HOT PATH")
        print(f"   - Even with 10000 cycle threshold, they slow everything down")
        print(f"")
        print(f"2. Remove remaining debug prints:")
        print(f"   - 'in freeproc' (proc.c line 145)")
        print(f"   - 'Done free' (proc.c line 149)")
        print(f"   - 'In fork now' (proc.c line 276)")
        print(f"")
        print(f"3. Verify syscall.c has no excessive prints")
    else:
        print(f"✓ Test completed within acceptable time")
    
    print(f"{'='*80}")

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        sys.exit(1)
