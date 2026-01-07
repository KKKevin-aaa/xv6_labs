#!/usr/bin/env python3
"""
分析 xv6 内存分配器的行为模式
为什么 kalloc/kfree 的地址看起来"无序"？
"""

import re
import sys
from collections import defaultdict

def parse_address(addr_str):
    """将地址字符串转换为整数"""
    if addr_str == '(nil)' or addr_str == '0x0':
        return None
    return int(addr_str, 16)

def analyze_memory_pattern(log_file):
    """分析内存分配模式"""
    
    operations = []  # 记录所有操作
    freelist_states = []  # freelist 状态变化
    page_lifecycle = defaultdict(list)  # 每个页面的生命周期
    
    with open(log_file, 'r') as f:
        for line in f:
            line = line.strip()
            
            # 解析 KALLOC
            kalloc_match = re.search(r'\[KALLOC\] pa=(0x[0-9a-f]+) -> pid=(\d+)\((\w+)\), old_head=(0x[0-9a-f]+), next=(0x[0-9a-f]+)', line)
            if kalloc_match:
                pa = parse_address(kalloc_match.group(1))
                pid = int(kalloc_match.group(2))
                name = kalloc_match.group(3)
                old_head = parse_address(kalloc_match.group(4))
                next_addr = parse_address(kalloc_match.group(5))
                
                operations.append({
                    'type': 'kalloc',
                    'pa': pa,
                    'pid': pid,
                    'name': name,
                    'old_head': old_head,
                    'next': next_addr
                })
                page_lifecycle[pa].append(('alloc', pid, name, len(operations)))
                continue
            
            # 解析新 head
            new_head_match = re.search(r'new_head=(0x[0-9a-f]+)', line)
            if new_head_match and operations:
                new_head = parse_address(new_head_match.group(1))
                operations[-1]['new_head'] = new_head
                freelist_states.append(new_head)
                continue
            
            # 解析 KFREE
            kfree_match = re.search(r'\[KFREE\] pa=(0x[0-9a-f]+) by pid=(\d+)\((\w+)\), old_head=(0x[0-9a-f]+)', line)
            if kfree_match:
                pa = parse_address(kfree_match.group(1))
                pid = int(kfree_match.group(2))
                name = kfree_match.group(3)
                old_head = parse_address(kfree_match.group(4))
                
                operations.append({
                    'type': 'kfree',
                    'pa': pa,
                    'pid': pid,
                    'name': name,
                    'old_head': old_head
                })
                page_lifecycle[pa].append(('free', pid, name, len(operations)))
    
    print("=" * 80)
    print("内存分配器行为分析")
    print("=" * 80)
    
    # 1. 分析地址分布
    print("\n【1. 地址分布分析】")
    kalloc_addrs = [op['pa'] for op in operations if op['type'] == 'kalloc']
    kfree_addrs = [op['pa'] for op in operations if op['type'] == 'kfree']
    
    if kalloc_addrs:
        print(f"KALLOC 地址范围: 0x{min(kalloc_addrs):x} - 0x{max(kalloc_addrs):x}")
        print(f"KALLOC 总数: {len(kalloc_addrs)}")
    
    if kfree_addrs:
        print(f"KFREE  地址范围: 0x{min(kfree_addrs):x} - 0x{max(kfree_addrs):x}")
        print(f"KFREE  总数: {len(kfree_addrs)}")
    
    # 2. 分析 freelist 的 LIFO 特性
    print("\n【2. LIFO (后进先出) 特性验证】")
    lifo_violations = 0
    recent_frees = []  # 最近释放的页面
    
    for i, op in enumerate(operations):
        if op['type'] == 'kfree':
            recent_frees.insert(0, op['pa'])  # 插入到头部
            if len(recent_frees) > 10:
                recent_frees.pop()
        elif op['type'] == 'kalloc':
            if recent_frees and op['pa'] not in recent_frees[:5]:
                lifo_violations += 1
            if op['pa'] in recent_frees:
                idx = recent_frees.index(op['pa'])
                recent_frees.remove(op['pa'])
                if idx == 0:
                    print(f"  ✓ 完美 LIFO: 分配了刚释放的页面 0x{op['pa']:x}")
    
    print(f"LIFO 违背次数: {lifo_violations} / {len(kalloc_addrs)}")
    print(f"LIFO 符合率: {(1 - lifo_violations/max(len(kalloc_addrs), 1)) * 100:.1f}%")
    
    # 3. 分析地址"跳跃"
    print("\n【3. 地址跳跃分析（为什么看起来无序）】")
    print("连续操作的地址差异：")
    
    jumps = []
    for i in range(1, min(20, len(operations))):
        if operations[i-1]['pa'] and operations[i]['pa']:
            diff = abs(operations[i]['pa'] - operations[i-1]['pa'])
            jumps.append(diff)
            op_type = f"{operations[i-1]['type']} -> {operations[i]['type']}"
            print(f"  {op_type:20s}: 0x{operations[i-1]['pa']:x} -> 0x{operations[i]['pa']:x}, "
                  f"diff={diff//4096:4d} 页")
    
    if jumps:
        avg_jump = sum(jumps) / len(jumps)
        print(f"\n平均地址跳跃: {avg_jump/4096:.1f} 页 (0x{int(avg_jump):x} 字节)")
    
    # 4. 分析重用模式
    print("\n【4. 页面重用模式】")
    reused_pages = {}
    for pa, lifecycle in page_lifecycle.items():
        if len(lifecycle) > 2:  # 分配-释放-再分配
            reused_pages[pa] = lifecycle
    
    print(f"被重用的页面数: {len(reused_pages)}")
    if reused_pages:
        print("示例页面生命周期：")
        for pa, lifecycle in list(reused_pages.items())[:5]:
            print(f"  页面 0x{pa:x}:")
            for action, pid, name, step in lifecycle:
                print(f"    [{step:4d}] {action:5s} by pid={pid}({name})")
    
    # 5. 解释"无序"的原因
    print("\n【5. 为什么地址看起来无序？】")
    print("""
原因分析：
1. LIFO 栈结构：freelist 是后进先出（LIFO）
   - kfree(A) -> freelist = A -> ...
   - kfree(B) -> freelist = B -> A -> ...
   - kalloc() 返回 B（最后释放的）
   - kalloc() 返回 A
   
2. 多进程交错操作：
   - init 进程分配页面
   - shell 进程分配页面
   - 释放顺序与分配顺序不同
   
3. 页表结构的分配模式：
   - 分配多级页表时，先分配 L2，再 L1，再 L0
   - 释放时可能是递归释放，顺序不同
   
4. 初始化顺序：
   - kinit() 时，从高地址到低地址遍历
   - 导致 freelist 初始就是"倒序"
   
结论：这不是"无序"，而是 LIFO 栈的自然行为！
""")
    
    # 6. freelist 状态追踪
    if freelist_states:
        print("\n【6. Freelist 头部地址变化（前20次）】")
        for i, head in enumerate(freelist_states[:20]):
            if head:
                print(f"  [{i:3d}] freelist head = 0x{head:x}")
        
        # 检查是否有重复
        unique_heads = len(set(freelist_states))
        print(f"\n唯一的 freelist 头部地址数: {unique_heads}")
        print(f"总的状态变化次数: {len(freelist_states)}")
        print(f"地址重复率: {(1 - unique_heads/len(freelist_states)) * 100:.1f}%")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("用法: python3 analyze_memory.py <log_file>")
        print("示例: python3 analyze_memory.py xv6.out")
        sys.exit(1)
    
    analyze_memory_pattern(sys.argv[1])
