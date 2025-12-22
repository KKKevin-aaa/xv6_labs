import time
import gdb

# Target: Capture 100 user-space samples
# Since user-space execution time is very short compared to kernel IO,
# this might take a while to collect, but the data will be pure.
TARGET_USER_SAMPLES = 100 
STEP_BATCH = 200 # Number of instructions to run per step

def get_pc_info():
    try:
        frame = gdb.selected_frame()
        pc = frame.pc()
        
        # Filter out Kernel addresses (High address space in xv6)
        if pc >= 0x80000000:
            return None, pc 
            
        # It is user space, try to resolve the function name
        block = gdb.block_for_pc(pc)
        if block and block.function:
            return block.function.name, pc
        
        # Fallback if symbol is missing
        return f"User_Addr(0x{pc:x})", pc
    except:
        return "Unknown", 0

samples = {}
gdb.execute("set pagination off")
gdb.execute("set confirm off")

print(f"Hunting for {TARGET_USER_SAMPLES} USER-SPACE samples...", flush=True)

user_count = 0
total_steps = 0

try:
    while user_count < TARGET_USER_SAMPLES:
        # 1. Sample the current PC
        func, pc = get_pc_info()
        
        # 2. Record only if it is a user-space function
        if func: 
            samples[func] = samples.get(func, 0) + 1
            user_count += 1
            # Print progress overwriting the same line
            print(f"Got User Sample! [{user_count}/{TARGET_USER_SAMPLES}] in {func:<20}", end="\r", flush=True)
        else:
            # If in kernel space, just print progress occasionally to show aliveness
            if total_steps % 10 == 0:
                print(f"Skipping Kernel... (Addr: 0x{pc:x})          ", end="\r", flush=True)

        # 3. Step the CPU forward by a batch of instructions
        gdb.execute(f"stepi {STEP_BATCH}", to_string=True)
        total_steps += 1

except KeyboardInterrupt:
    print("\nInterrupted by user.")
except gdb.error as e:
    print(f"\nGDB Error: {e}")

# --- Output Report ---
print(f"\n\nCaptured {user_count} user-space samples.")
print("-" * 40)

# Sort by frequency
sorted_samples = sorted(samples.items(), key=lambda item: item[1], reverse=True)

for func, hits in sorted_samples:
    percentage = (hits / user_count) * 100
    print(f"{func:<25} : {hits} samples ({percentage:.1f}%)")