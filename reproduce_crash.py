import subprocess
import time
import sys

def run_test():
    # Start make qemu
    # We use -nographic to ensure output goes to stdout
    proc = subprocess.Popen(['make', 'qemu'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=False)

    try:
        # Wait for boot
        time.sleep(5)
        
        # Sequence: ec 1111
        input_seq = b'ec 1111'
        proc.stdin.write(input_seq)
        proc.stdin.flush()
        time.sleep(0.5)

        # Move left 5 times: \033[D
        left_arrow = b'\x1b[D'
        for _ in range(5):
            proc.stdin.write(left_arrow)
            proc.stdin.flush()
            time.sleep(0.1)

        # Insert 0
        proc.stdin.write(b'0')
        proc.stdin.flush()
        time.sleep(0.5)

        # Press Left Arrow again (Trigger crash?)
        proc.stdin.write(left_arrow)
        proc.stdin.flush()
        time.sleep(1)

        # Read output to see if we got a trap/panic
        # We read non-blocking or just close and read remainder
        proc.terminate()
        stdout, stderr = proc.communicate()
        
        print("STDOUT:", stdout.decode('utf-8', errors='ignore'))
        print("STDERR:", stderr.decode('utf-8', errors='ignore'))

    except Exception as e:
        print(f"Error: {e}")
        proc.kill()

if __name__ == "__main__":
    run_test()
