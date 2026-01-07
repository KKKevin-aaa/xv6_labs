import gdb

class TreePrinter(gdb.Command):
    """
    通用二叉树/红黑树可视化工具。
    用法: ptree <根指针> <左子节点名> <右子节点名> [可选:摘要成员名]
    
    示例: ptree root rb_left rb_right
    示例: ptree root left right val
    示例: ptree &my_tree->root node.left node.right key
    """
    def __init__(self):
        super(TreePrinter, self).__init__("ptree", gdb.COMMAND_USER)
        self.max_depth = 20  # 防止树太深或死循环
        self.visited = set() # 环检测

    def invoke(self, arg, from_tty):
        self.visited.clear()
        args = gdb.string_to_argv(arg)
        if len(args) < 3:
            print("Usage: ptree <ROOT_PTR> <LEFT_NAME> <RIGHT_NAME> [SUMMARY_MEMBER]")
            return

        # 1. 解析参数
        ptr_expr = args[0]
        left_field = args[1]
        right_field = args[2]
        summary_field = args[3] if len(args) > 3 else None

        # 2. 获取根节点
        try:
            root_val = gdb.parse_and_eval(ptr_expr)
        except Exception as e:
            print(f"Error parsing pointer: {e}")
            return

        if int(root_val) == 0:
            print("Tree is NULL.")
            return

        print(f"\n\033[1;36m=== Tree View: {ptr_expr} (L: {left_field}, R: {right_field}) ===\033[0m\n")
        
        # 3. 开始递归打印
        # 参数: 当前节点, 前缀字符串, 是否是最后一个子节点, 节点类型(Root/L/R)
        self._print_node(root_val, "", True, "Root", left_field, right_field, summary_field, 0)
        print("\n")

    def _print_node(self, curr, prefix, is_last, node_type, left_field, right_field, summary_field, depth):
        """
        递归打印核心逻辑
        prefix: 父级传递下来的缩进字符串
        is_last: 当前节点是否是父节点的最后一个子节点（决定了连接线形状）
        """
        addr = int(curr)
        
        # 1. 环检测与深度限制
        if addr in self.visited:
            print(f"{prefix}\033[31m└── [CYCLE DETECTED: 0x{addr:x}]\033[0m")
            return
        self.visited.add(addr)

        if depth > self.max_depth:
            print(f"{prefix}...")
            return

        # 2. 准备显示内容 (摘要 + 地址)
        summary_str = ""
        if summary_field:
            try:
                # 尝试获取摘要数据
                val = curr[summary_field]
                summary_str = f" \033[1;33m{summary_field}: {val}\033[0m"
            except:
                summary_str = " ?"
        
        # 节点标签颜色区分：Root(青), L(黄), R(紫)
        type_color = "\033[1;36m" if node_type == "Root" else ("\033[33m" if node_type == "L" else "\033[35m")
        node_label = f"{type_color}[{node_type}]\033[0m"

        # 3. 打印当前节点行
        # 根节点不需要前缀连接符
        if node_type == "Root":
            print(f"{node_label} @ \033[32m0x{addr:x}\033[0m{summary_str}")
            new_prefix = ""
        else:
            # 使用 Unicode 字符绘制树枝
            connector = "└── " if is_last else "├── "
            print(f"{prefix}{connector}{node_label} @ \033[32m0x{addr:x}\033[0m{summary_str}")
            # 更新下一层的前缀：如果我是最后一个，子节点就不需要继承我的竖线
            new_prefix = prefix + ("    " if is_last else "│   ")

        # 4. 获取左右子节点
        try:
            left_node = curr[left_field]
            right_node = curr[right_field]
        except Exception as e:
            print(f"{new_prefix}\033[31m[Error accessing children: {e}]\033[0m")
            return

        has_left = int(left_node) != 0
        has_right = int(right_node) != 0

        # 5. 递归处理子节点
        # 逻辑：如果有右节点，那么左节点就不是最后一个（需要画 ├──）；否则左节点是最后一个（画 └──）
        # 注意：这里先打印左，再打印右，符合常规视觉习惯
        
        if has_left:
            is_left_last = not has_right # 如果没有右节点，左节点就是最后一个
            self._print_node(left_node, new_prefix, is_left_last, "L", left_field, right_field, summary_field, depth + 1)
        
        if has_right:
            # 右节点永远是当前层级的最后一个（在二叉树语境下）
            self._print_node(right_node, new_prefix, True, "R", left_field, right_field, summary_field, depth + 1)

# 注册命令
TreePrinter()