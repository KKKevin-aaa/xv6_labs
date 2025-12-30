import gdb

class LinkedListPrinter(gdb.Command):
    """
    通用链表可视化工具。
    用法: plist <头指针> <next成员名> [可选:用于摘要显示的成员名]
    示例: plist global_mm.mmap vm_next vm_start
    示例: plist free_list next_page inuse_count
    """
    def __init__(self):
        super(LinkedListPrinter, self).__init__("plist", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        args = gdb.string_to_argv(arg)
        if len(args) < 2:
            print("Usage: plist <HEAD_PTR> <NEXT_MEMBER_NAME> [SUMMARY_MEMBER]")
            return

        # 1. get argument
        ptr_expr = args[0]      # 例如: global_mm.mmap
        next_field = args[1]    # 例如: vm_next
        summary_field = args[2] if len(args) > 2 else None # 例如: vm_start

        # 2. Parsing header_ptr
        try:
            val = gdb.parse_and_eval(ptr_expr)
        except Exception as e:
            print(f"Error parsing pointer: {e}")
            return

        if str(val) == "0x0" or int(val) == 0:
            print("List is NULL.")
            return

        # 3. starting loop
        curr = val
        idx = 0
        visited = set() # 环检测

        print(f"\n\033[1;36m=== Walking List: {ptr_expr} (via ->{next_field}) ===\033[0m\n")

        while curr != 0:
            addr = int(curr)
            
            # 环检测 (防死循环)
            if addr in visited:
                print("      \033[1;31m||\033[0m")
                print(f"      \033[1;31m==> CYCLE DETECTED! Back to 0x{addr:x}\033[0m")
                break
            visited.add(addr)
            if idx > 50: # 安全限制
                print("      ...")
                print("      (Stopped after 50 nodes)")
                break

            # gettng the summary
            summary_str = ""
            if summary_field:
                try:
                    # 尝试读取摘要字段
                    field_val = curr[summary_field]
                    summary_str = f" | {summary_field}: {field_val}"
                except:
                    summary_str = f" | {summary_field}: ?"

            print(f"\033[1;33m[{idx}]\033[0m @ \033[32m0x{addr:x}\033[0m{summary_str}")
            try:
                # 关键点：利用Python的反射能力，通过字符串名字访问结构体成员
                next_val = curr[next_field]
            except Exception as e:
                print(f"Error: Structure has no member named '{next_field}'")
                break

            curr = next_val
            idx += 1

            # 打印连接线
            if curr != 0:
                print(" |")
                print(" V")
            else:
                print(" |")
                print(" X (NULL)")
        
        print("\n")

# 注册命令
LinkedListPrinter()