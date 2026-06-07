# Linux 6.18 · k3-br-v1.0.y 定制分支

> Linux 内核源码树。本仓库基于 `k3-br-v1.0.y`，含 bianbu/k3 平台定制（PCIe port bus、AMD GPU 等）。
> 上游官方说明请见 [README](./README)。
>
> 🤖 本 README 及 `docs/` 下的文档与可视化播放器由 **Minimax3** 生成，请结合源码交叉验证。

---

## 📚 项目文档与可视化

| 主题 | 在线（GitHub Pages） | 仓库内文件 |
|---|---|---|
| RISC-V Sv39 早期页表构建过程<br>（`_start` → `init` 之前） | 🎬 [步进播放器](https://liulog.github.io/riscv-sv39-pagetable-bringup.html) · 📖 [文档](https://liulog.github.io/riscv-sv39-pagetable-bringup-doc.html) | [.md](docs/riscv-sv39-pagetable-bringup.md) · [.html](docs/riscv-sv39-pagetable-bringup.html) |

> 💡 **使用说明**
> - **在线版（推荐）**：上表中的 Pages 链接 GitHub 网页点击即可直接在浏览器渲染播放
> - **本地版**：clone 后双击 `docs/riscv-sv39-pagetable-bringup.html` 也能直接打开（零外部依赖，50 KB）
> - **键盘快捷键**：`←/→` 步进 · `Space` 自动播放 · `R` 重置 · `1-9,0` 跳转到对应步骤

---

## 🛠 构建 / 配置

```bash
# 使用本仓库 defconfig
make k3_bianbu_defconfig

# 编译（按需替换 ARCH / CROSS_COMPILE）
make -j$(nproc)
```

参见 `arch/riscv/configs/k3_bianbu_defconfig`。最近改动：

- `0ffac20d9` `k3_bianbu_defconfig: enable PCIe port bus and AMD GPU support`

---

## 📂 仓库结构（与上游一致的部分省略）

```
.
├── arch/riscv/configs/k3_bianbu_defconfig    # 本分支主 defconfig
├── docs/                                      # 项目自有文档与可视化
│   ├── riscv-sv39-pagetable-bringup.md
│   └── riscv-sv39-pagetable-bringup.html
├── README                                     # 上游 Linux 原始 README（不动）
└── README.md                                  # 本文件
```

---

## 🔗 上游 Linux 参考

- 上游官方文档：<https://www.kernel.org/doc/html/latest/>
- `Documentation/admin-guide/README.rst` — 阅读顺序起点
- `Documentation/process/changes.rst` — 构建运行依赖
- `Documentation/arch/riscv/vm-layout.rst` — 本分支可视化所用的 Sv39/48/57 虚拟内存布局参考
