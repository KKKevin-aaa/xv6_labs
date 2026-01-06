import gdb

class RBTreeViewer(gdb.Command):
    """
    红黑树专用可视化工具（rbview）。
    自动解析压缩的 parent_color 字段来显示颜色。
    
    用法: rbview <根指针> <左成员> <右成员> <ParentColor成员> [摘要成员]
    
    逻辑复刻:
      Color  = rb_parent_color & 1  (假设: 0=RED, 1=BLACK)
      Parent = rb_parent_color & ~3
      
    示例: rbview root rb_left rb_right rb_parent_color key
    """
    def __init__(self):
        super(RBTreeViewer, self).__init__("rbview", gdb.COMMAND_USER)
        self.max_depth = 25
        self.visited = set()

    def invoke(self, arg, from_tty):
        self.visited.clear()
        args = gdb.string_to_argv(arg)
        if len(args) < 4:
            print("Usage: rbview <ROOT> <LEFT> <RIGHT> <PARENT_COLOR_FIELD> [SUMMARY]")
            return

        # 参数解析
        ptr_expr = args[0]
        left_field = args[1]
        right_field = args[2]
        pc_field = args[3]     # 你的 rb_parent_color 字段
        summary_field = args[4] if len(args) > 4 else None

        try:
            root_val = gdb.parse_and_eval(ptr_expr)
        except Exception as e:
            print(f"Error parsing pointer: {e}")
            return

        if int(root_val) == 0:
            print("Tree is NULL.")
            return

        print(f"\n\033[1;36m=== RB Tree View: {ptr_expr} ===\033[0m")
        print(f"\033[36mFields: L->{left_field}, R->{right_field}, PC->{pc_field}\033[0m\n")
        
        # 开始递归
        self._print_node(root_val, "", True, "Root", 
                         left_field, right_field, pc_field, summary_field, 0)
        print("\n")

    def _print_node(self, curr, prefix, is_last, node_type, 
                    left_f, right_f, pc_f, sum_f, depth):
        
        addr = int(curr)
        if addr in self.visited:
            print(f"{prefix}\033[31m└── [CYCLE DETECTED: 0x{addr:x}]\033[0m")
            return
        self.visited.add(addr)

        if depth > self.max_depth:
            print(f"{prefix}...")
            return

        # =================================================
        # 核心逻辑：复刻 C 语言的 static inline int rb_color
        # =================================================
        is_red = False
        try:
            # 1. 读取 rb_parent_color 的值 (转为 Python int)
            pc_val = int(curr[pc_f])
            
            # 2. 这里的逻辑对应你的 C 代码：return rb->rb_parent_color & 1;
            # 通常约定：0 = RED, 1 = BLACK (如果你的定义相反，请修改这里: val & 1 == 1)
            color_bit = pc_val & 1
            is_red = (color_bit == 0)
            
        except Exception as e:
            # 如果读取失败，当作普通节点处理
            print(f"Error reading color: {e}")
            color_bit = -1

        # =================================================
        # 样式与输出
        # =================================================
        
        # 颜色配置
        if is_red:
            # 红色节点：高亮红字
            # \033[1;31m = Bold Red
            ansi_color = "\033[1;31m"
            color_label = "(RED)"
            # 整个节点的样式
            node_style = f"{ansi_color}" 
        else:
            # 黑色节点：为了在黑色终端看清，使用默认色或深灰，但标记为 (BLK)
            # \033[1;30m = Dark Gray (Bright Black), \033[0m = Reset/White
            ansi_color = "\033[0m" 
            color_label = "(BLK)"
            node_style = "\033[0m"

        # 摘要信息
        summary_str = ""
        if sum_f:
            try:
                # 尝试读取摘要
                val = curr[sum_f]
                # 把摘要也变成同样的颜色
                summary_str = f" {sum_f}:{val}"
            except: pass

        # 构建树形连接符
        connector = "└── " if is_last else "├── "
        
        # 节点类型标签 (L/R/Root)
        # Root用青色，L/R用默认色，避免和红黑树颜色混淆
        type_str = f"[{node_type}]"
        
        # 最终打印
        # 格式: 前缀 + 连接符 + [红黑颜色] (L/R) (RED/BLK) @ 地址 摘要 [重置]
        if node_type == "Root":
            # 根节点不需要连接符
            print(f"{node_style}{type_str} {color_label} @ 0x{addr:x}{summary_str}\033[0m")
            new_prefix = ""
        else:
            # 子节点
            print(f"{prefix}{connector}{node_style}{type_str} {color_label} @ 0x{addr:x}{summary_str}\033[0m")
            new_prefix = prefix + ("    " if is_last else "│   ")

        # =================================================
        # 递归子节点
        # =================================================
        try:
            left_node = curr[left_f]
            right_node = curr[right_f]
        except: return

        has_left = int(left_node) != 0
        has_right = int(right_node) != 0

        # 先左后右
        if has_left:
            # 如果有右节点，左节点就不是最后一个 (用 ├──)
            self._print_node(left_node, new_prefix, not has_right, "L", 
                             left_f, right_f, pc_f, sum_f, depth + 1)
        
        if has_right:
            # 右节点永远是这层最后一个 (用 └──)
            self._print_node(right_node, new_prefix, True, "R", 
                             left_f, right_f, pc_f, sum_f, depth + 1)

# 注册命令
RBTreeViewer()