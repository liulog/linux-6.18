# RISC-V `satp` 与 `sfence.vma` 语义详解

> 🤖 **本文档由 Minimax3 生成** · 基于 RISC-V Privileged Architecture Spec（Vol II）+ Linux 6.18 源码与维护者注释整理。
>
> **目的**：解答以下三个疑问：
> 1. `satp` 从 0 (Bare) 变成非零（开 MMU），**下一条指令立刻按新 satp 寻址吗**？（fetch / load / store）
> 2. `satp` 从非零（分页）变成 0 (Bare)，**下一条指令立刻按物理寻址吗**？还是会"惯性"继续走虚拟一段时间？
> 3. 常见模式 `csrw satp, x; sfence.vma` 中，`sfence.vma` 本身要被 fetch ——
>    既然上一条已经把 satp 切了，新 satp 下当前 PC 不一定能 fetch 到这条 sfence.vma，
>    它不会立刻崩吗？
>
> **声明**：因为离线环境无法 fetch RISC-V spec 网页，下面引用 spec 时我会标注 "（Priv Spec §X.X，意译）" 而非逐字。请以你手头的 spec 为准；Linux 源码注释（Palmer Dabbelt 等维护者写的）是与 spec 语义一致的实践参考，我会大量引用。

---

## 0. TL;DR — 四个问题的结论

| 问题 | 结论 | 关键依据 |
|---|---|---|
| Q1. `satp=0 → 非零`，下一条指令是否立即用新表？ | **是。** 紧跟 `csrw satp` 的那条指令的 fetch 就用新 satp 翻译；如果新表里没有当前 PC 对应的 VA，会立刻 instruction page fault。这正是 head.S 用 stvec trick 完成"PA→VA 跳转"的硬件前提。 | Priv Spec satp 一节："the new contents are observed by subsequent instructions"；head.S:96-106 stvec trick 全依赖此。 |
| Q2. `satp=非零 → 0`（Bare），下一条指令是否立即按 PA？ | **是。** 对称地立即生效。**实际上几乎不会被这样用**：若上一条 PC 是高地址 VA，下一条 PA fetch 会指向不存在的物理地址 → 立即崩。要安全做这种切换必须先保证下一条指令所在地址 VA==PA 可达（identity mapping 或低地址 PA）。 | Priv Spec satp 一节 (MODE 字段语义)；kexec/cpu_resume 等少数路径才会这么干，且都预先准备好 identity 映射。 |
| Q3. `csrw satp; sfence.vma` 那条 fence 怎么 fetch 成功？ | **取决于**："新 satp 下当前 PC 那一页的翻译是否仍合法"。三种情况之一即可：(a) 新表里这一页映射不变（VA→同一 PA）；(b) TLB 里还残留旧表对这一页的条目，且仍正确；(c) (a) 与 (b) 同时成立。Linux head.S 第二次写 satp 时显式靠 (a) —— 注释 "the first superpage is translated the same way"。 | head.S:116-120 注释；Priv Spec 关于"实现可以使用此前 satp 留下的 cached translation"。 |
| Q4. `satp=非零 → 另一个非零`，新表里没这条 VA 的映射，**但 TLB 里有旧 satp 留下的 stale 条目**，下一条 fetch 会因为 TLB 命中而"意外成功"吗？ | **可能。** Spec 允许实现使用未被 SFENCE.VMA 作废的 cached translation。**前提**是新旧 satp 用同一 ASID（或不开 ASID）：此时旧 TLB 项的 `(VA, ASID)` 标签和新查找匹配，可能命中给出旧映射。**这是 set_mm_noasid 切完 satp 必须 `local_flush_tlb_all()` 的根本原因**，注释直接写 "blindly nuke entire local TLB"。开 ASID 时新进程换 ASID，旧 ASID 的项不会被命中（带 G 的内核项跨 ASID 但全局稳定），因此 set_mm_asid 常规切换可以跳过 sfence。 | `context.c:200-205`；Priv Spec 关于 ASID-tagged TLB 与 SFENCE.VMA。详见 §6。 |

下面三个问题分别展开。

---

## 1. 30 秒心智模型（一句话记住）

> **`satp` 是开关，`sfence.vma` 是同步器，两者正交。**

具体一点：

### 0.1 `sfence.vma` 做且仅做两件事

1. **内存序栅栏**：保证此指令之前对 in-memory PTE/PGD 等结构的 **store**，对此指令之后的**隐式翻译访问（PTW）** 可见。
   - 是 store→PTW 的栅栏，不是 store→普通 load 的栅栏（那是 `fence rw,rw`）
   - 只对当前 hart 已经可见的 store 生效；跨 hart 要靠 IPI + 远端各自执行 sfence.vma（这就是 `SBI RFENCE` 的活）
2. **TLB 失效**：按 `rs1` (vaddr) 和 `rs2` (asid) 作废 TLB 条目，四个变体：

   | `rs1` | `rs2` | 作用 | Global 项是否清掉 |
   |---|---|---|---|
   | `x0` | `x0` | 作废全部 | **会清** |
   | `vaddr` | `x0` | 作废涉及 vaddr 的所有 ASID 的项 | 涉及 vaddr 的全部 |
   | `x0` | `asid` | 作废指定 ASID 的所有项 | **不清** |
   | `vaddr` | `asid` | 最细粒度 | 不清（除非 vaddr 的项被标 G） |

   > Linux 中 `local_flush_tlb_all()` (`tlbflush.h:18-21`) 直接展开成 `sfence.vma`（即 x0,x0 变体），把 Global 项也清光；`local_flush_tlb_all_asid(asid)` (`tlbflush.h:23-29`) 走 `ASID` 变体，保留 G 项。

### 0.2 `satp` 写入即生效

- `csrw satp, X` 写完，**下一条指令立刻** 用新 satp 做 fetch / load / store 的翻译——不需要 sfence.vma 来"激活"。
- 唯一的"缓存惯性"：TLB 里可能残留**前一个 satp 时代**的条目，命中时给出旧映射。要保证之后只看到新映射，才需要 sfence.vma。
- 因此：
  - **Bare → 分页**：之前没用 TLB，没有残留，写完就干净（head.S 第一次写 satp 之后**不放** sfence.vma 正是这道理）
  - **分页 → 另一张分页表**：TLB 可能残留，**需要** sfence.vma 清干净（head.S 第二次写 satp 之后立刻 sfence）
  - **分页 → Bare**：写完即按 PA 寻址，下一条指令的 PC 数值必须本身就是合法 PA（少见，需提前布好 identity 映射）

---

## 2. spec 怎么说（语义摘要）

为方便后文引用，先把 RISC-V Privileged Spec Vol II 关于 satp 和 SFENCE.VMA 的几条规则浓缩成中文（**意译，非逐字**）：

### 1.1 `satp` 写入语义

- `satp` 是一个 CSR（控制状态寄存器），写它和写普通 CSR 一样，**没有阻塞流水线的"特殊副作用"**——程序顺序上，下一条指令开始执行时 satp 已是新值。
- 因此，**下一条指令的 implicit memory reference（包括 fetch、PTW、load/store 翻译）都按新 satp 走**。这是"in-program-order"语义。
- 但 spec 同时说：**实现可以**继续使用此前留在 address-translation cache（TLB）里的翻译条目，**不要求 csrw satp 自动清 TLB**。
- 所以 satp 写入"生效"的精确含义是：**新的 page-table walks 用新 satp**，但**已经 cache 的 TLB 命中可能继续给出旧映射**。
- 若新旧 satp 下同一 VA 的映射不同，必须显式 `sfence.vma` 才能保证之后看到新映射。

### 1.2 `SFENCE.VMA rs1, rs2` 的作用

- 它**不是**一条 "make satp effective" 指令。satp 写完早就生效。
- 它做的是：
  1. 把当前 hart 上所有"在它之前的 store 到 in-memory PTE / PGD 等结构"对**后续的 implicit page-table walk** 可见（store→walk 的内存序栅栏）；
  2. 把 address-translation cache（TLB）中可能 stale 的项**作废**。
- 参数：
  - `rs1 = x0, rs2 = x0`：作废全部（gVA 全部 ASID 全部）
  - `rs1 = vaddr, rs2 = x0`：作废涉及 vaddr 的所有 ASID 的项
  - `rs1 = x0, rs2 = asid`：作废指定 ASID 的所有项
  - `rs1 = vaddr, rs2 = asid`：最细粒度
- 关键正交概念：**SFENCE.VMA 解决 TLB & store→walk 顺序，不解决 satp 何时生效**。

### 1.3 把上面两条放一起

> "csrw satp 这条指令完成后，下一条指令开始时 satp 是新值。新值会被下次 page-table walk 用到。
> 但 TLB 里可能还有旧 satp 时代留下的、对某些 VA 仍能命中的条目；这些条目命中给出的就是旧映射。
> 若你需要把这些 stale 条目彻底清掉，就发 SFENCE.VMA。"

记住这条，三个问题全都能机械推出来。

---

## 3. 三个问题逐个拆解（Q4 单独成节，见 §6）

### 2.1 Q1：`satp = 0 → 非零`，下一条指令立刻用新表吗？

**结论：是。**

#### 推理

- `satp` 写完后下一条指令开始执行；此时 MODE=Sv39（或别的分页模式）。
- 这条指令的 fetch 触发地址翻译。
- TLB 此刻是什么状态？——前一阶段 satp=0 时硬件在 Bare 模式下取指/访存，根本没有走 page table，**所以 TLB 里没有任何 entry**（最多有空条目或 reset 状态）。
- 因此下一条 fetch 一定 miss TLB → 硬件 walker 用**新 satp**走表。
- 走表结果是 hit 还是 fault？取决于"PC 这个数值"在新表里是否是合法 VA：
  - 如果新表里映射了"PC 对应的那一页 VA"，且物理页有指令 → 取指成功，正常往下走。
  - 如果新表里没有"PC 对应的 VA" → instruction page fault → 跳 `stvec`。

这就是 Linux head.S 的 **stvec trick**。`csrw satp, trampoline` 之前 PC 是某个低地址 PA（如 `0x80200124`），trampoline_pg_dir 里 **只在 `0xffffffff80...` 那一支有项**，所以 PC 数值 `0x80200124` 当 VA 翻译必然 fault。fault 跳 `stvec`，而 `stvec` 已经提前被设成 `1:` 标号的 **高地址 VA**（在 trampoline 的 2 MiB 大页里），跳过去能取指。

#### 一图概括

```
时钟周期 →

cycle N:    csrw satp, X     ← satp = X 在此 cycle 末已成定局
cycle N+1:  fetch PC          ← 用 satp = X 翻译 PC
            ├ TLB miss (Bare 阶段 TLB 是空的)
            └ walk page table X
                ├ hit  → 取指成功
                └ miss → page fault → 跳 stvec
```

#### 一个反直觉细节

很多人会想："新 satp 这时还没 sfence.vma，怎么就能用它走表？"

答案：spec 不需要 sfence.vma 才让新 satp"生效"，**只是不保证旧的 TLB 条目被清**。在 Q1 这种 Bare→分页的场景下根本没有旧 TLB 条目，所以"是否需要 sfence.vma"和"satp 是否生效"是两个独立问题。

---

### 2.2 Q2：`satp = 非零 → 0` (Bare)，下一条指令立刻按 PA 寻址吗？

**结论：是，立刻生效。但这种切换在 Linux 内核里几乎不会被孤立使用——必须先准备好"下一条指令的 PA 是合法可达的 RAM 地址"。**

#### 推理

完全对称于 Q1：

- `csrw satp, 0` 之后，下一条指令的 fetch 用 MODE=Bare 翻译，PC 数值被当作 **PA**。
- 如果当前 PC 是高地址 VA（比如 `0xffffffff80000abc`），把它当 PA 用，物理地址空间根本没这么大 → 必然失败，要么是 access fault，要么挂飞。
- 所以从分页模式回到 Bare 是危险操作，**必须先把 PC 切到一段 VA==PA（identity 映射）的代码里**，然后再做 csrw satp, 0；之后那条指令的 PA = 之前的 VA = 同一个物理位置，能正常 fetch。

#### Linux 中谁会这么干？

正常路径下没有。少数特殊场景：

| 场景 | 何处 | 做法 |
|---|---|---|
| kexec 切到新内核 | `arch/riscv/kernel/machine_kexec*.c` + `purgatory/` | purgatory 代码运行在 identity 映射区，先 satp=0 再跳到新内核入口 |
| CPU hot-unplug 之后 hot-plug 回来 | `cpu_resume` 流程 | 通过 SBI 重新走 head.S 路径，等价于"从 Bare 重新开 MMU"，不是 Linux 自己把 satp 写 0 |
| 早期 `set_satp_mode` 自探测 | `init.c:910-913` | `csr_write(SATP, identity)` 试一个 mode，再 `csr_swap(SATP, 0)` 读回看硬件是否接受。这里之所以不崩是因为它**事先就建了 identity mapping**：`set_satp_mode_pmd` 整个 2 MiB 大页 VA==PA。 |

#### `set_satp_mode` 的细节（值得讲清楚，因为它是 Linux 里"短暂写 satp 又写 0"的唯一明显例子）

`arch/riscv/mm/init.c:887-928`：

```c
/* Handle the case where set_satp_mode straddles 2 PMDs */
create_pmd_mapping(early_pmd,
                   set_satp_mode_pmd, set_satp_mode_pmd,    /* VA == PA */
                   PMD_SIZE, PAGE_KERNEL_EXEC);
create_pmd_mapping(early_pmd,
                   set_satp_mode_pmd + PMD_SIZE,
                   set_satp_mode_pmd + PMD_SIZE,            /* VA == PA */
                   PMD_SIZE, PAGE_KERNEL_EXEC);
...
local_flush_tlb_all();
csr_write(CSR_SATP, identity_satp);   /* satp = early_pg_dir | TestMode */
hw_satp = csr_swap(CSR_SATP, 0ULL);   /* 读回 + 顺便把 satp 写回 0 */
local_flush_tlb_all();
```

注意它**真正写了两次 satp**：

1. `csr_write(SATP, identity_satp)`：把 MODE 设成待测的模式。下一条指令是 `csr_swap(SATP, 0)`，这条指令本身要 fetch。因为 set_satp_mode 函数代码所在的 2 MiB 已经预先建了 **VA==PA identity 映射**，所以新 satp 下当前 PC（=函数代码 PA）可达，取指成功。
2. `csr_swap(SATP, 0)`：读回新值（看硬件接没接受这个 mode），同时把 satp 写回 0。下一条指令是 `local_flush_tlb_all()`，PC 仍是函数代码 PA。MODE 变 Bare，PC 当 PA 寻址刚好就是它自己，仍可达。

**所以 set_satp_mode 是 Linux 在 64-bit 启动里真用了 identity mapping 的唯一地方**，目的就是为了能在"分页 ↔ Bare"之间安全跳。这也反过来佐证了 trampoline 那条**不是** identity mapping —— 二者的设计目标不一样。

---

### 2.3 Q3：`csrw satp; sfence.vma` 紧挨着，`sfence.vma` 不会 fetch 失败吗？

**结论：取决于"新 satp 下当前 PC 那一页的映射"。下面分情况列出，并对应到 Linux 代码。**

设刚刚写完 satp，PC 准备取下一条指令 `sfence.vma`。

#### 情况 A：新表 与 旧表 对当前 PC 那一页 **映射完全一致**

新表里 walk 成功，结果跟旧表一样 → fetch 成功。
**Linux 例**：`head.S` 第二次写 satp：

```asm
                                    # 此时 satp = trampoline
csrw CSR_SATP, a2                   # 切到 early_pg_dir
sfence.vma                          # ← 必须能 fetch
```

为什么 OK？看注释 `head.S:116-120`：

```
* Switch to kernel page tables.  A full fence is necessary in order to
* avoid using the trampoline translations, which are only correct for
* the first superpage.  Fetching the fence is guaranteed to work
* because that first superpage is translated the same way.
```

trampoline 和 early_pg_dir **都把内核镜像那 2 MiB 映射到同一个 PA，同样 RWX**（看 `init.c:1210-1228` 和 `1231-1236`），所以正在执行的指令所在的 VA 在两表中翻译结果相同，`sfence.vma` 这条指令的 fetch 必然成功。

之后 `sfence.vma` 才真正把 trampoline 期间 cache 进 TLB 的可能 stale 条目扫掉，确保后续访问 fixmap、kasan 等其他区域时一定走 early_pg_dir。

#### 情况 B：新表 walk 这一页会失败（即新表里没这条映射），但 TLB 里还残留旧表的条目

按 spec "实现可以继续用 cache 里的旧翻译" → TLB 命中给出旧映射 → fetch 成功。
**Linux 例**：`set_mm_noasid` 进程切换（无 ASID）：

`arch/riscv/mm/context.c:200-205`：

```c
static void set_mm_noasid(struct mm_struct *mm)
{
    /* Switch the page table and blindly nuke entire local TLB */
    csr_write(CSR_SATP, virt_to_pfn(mm->pgd) | satp_mode);
    local_flush_tlb_all();                  /* = sfence.vma */
}
```

切到下一个进程的 `mm->pgd` 时：
- `mm->pgd` 内核空间部分（>= PAGE_OFFSET 的所有项）**所有进程的 PGD 是同一份**（init_mm 那份的拷贝），所以内核 VA 走新表也是同样结果（与情况 A 等价）；
- 用户空间部分在新表里通常不同；但当前 PC 是**内核 VA**，依旧落在共享的内核子树里，fetch 必然成功；
- 然后立刻 `local_flush_tlb_all()` 把所有 stale 项（特别是上个进程的用户空间映射）清干净。

所以这里其实又是情况 A 的变体：当前 PC 所在的内核映射在新旧表中相同。

#### 情况 C：新表对这一页有映射但跟旧表不同（少见，几乎不应该发生）

如果新表里同一个 VA 翻译到**不同**的 PA，TLB 又恰好被擦了，那 fetch 会指向新 PA。如果新 PA 上不是同样的指令字节，**会执行错的指令**或触发 illegal instruction。这是设计 bug，不应该出现。

#### 情况 D：新表对这一页没有映射，TLB 也没有该条目

fetch fault → 跳 stvec。
**Linux 例**：`head.S` **第一次**写 satp 那一瞬间正是这种：

```asm
sfence.vma                          # 注意：这一条在 csrw satp **之前**
csrw CSR_SATP, a0                   # satp = trampoline
.align 2
1:                                  # ← 取这里靠 stvec trap
```

这次 fence 在 satp 写**之前**。它跟 satp 生效无关，是另一个目的——`head.S:95-99` 注释解释得很清楚：

```
* Load trampoline page directory, which will cause us to trap to
* stvec if VA != PA, or simply fall through if VA == PA.  We need a
* full fence here because setup_vm() just wrote these PTEs and we need
* to ensure the new translations are in use.
```

意思是：`setup_vm()` 用普通 store 指令往 trampoline_pmd / early_pg_dir 里写了一堆 PTE 字节；这些 store 还在 store buffer / cache 里时，hardware walker 不一定能读到一致的值。`sfence.vma` 强制把这些 store 排到 walker 之前。
**所以这个 sfence.vma 是给"PTE 字节 store → walker read"加内存序栅栏的，不是为了让 satp 生效。**

---

## 4. 把 Q3 串成一个判定流程

> 写完 satp 的下一条指令能否成功 fetch？

```
              ┌─────────────────────────────────────┐
              │ satp 已切到新表，下一条 PC = P     │
              └────────────┬────────────────────────┘
                           │
              ┌────────────▼────────────┐
              │ 新表 walk P → 成功？     │
              └─────┬────────────┬──────┘
                    │            │
                  是│           否│
                    │            │
       ┌────────────▼─┐    ┌─────▼────────────────────┐
       │ 直接取指成功  │    │ TLB 中有 P 的 stale 条目?│
       └────────────┬─┘    └─────┬────────────┬───────┘
                    │            │            │
                    │          是│          否│
                    │            │            │
                    │   ┌────────▼─┐    ┌─────▼────────────┐
                    │   │用旧条目  │    │ instruction page │
                    │   │取指成功  │    │ fault → 跳 stvec │
                    │   │（注意:   │    │ (head.S Q1 路径) │
                    │   │ 不一定   │    │                  │
                    │   │ 是想要的)│    │                  │
                    │   └──────────┘    └──────────────────┘
                    │
              ┌─────▼──────────────────────────────┐
              │ 后续 sfence.vma 决定 stale 是否清掉 │
              └────────────────────────────────────┘
```

**Linux head.S 第二次写 satp** 走的是 "新表 walk 成功"这条——靠的是 trampoline 和 early 对内核 2 MiB 大页映射一致。
**Linux head.S 第一次写 satp** 走的是 "新表 walk 失败 + 无 TLB stale → 跳 stvec"——这才是 stvec trick。

---

## 5. Linux 代码里所有写 satp 的位置（一览）

| 文件:行 | 写 satp 的目的 | satp 旧值 → 新值 | sfence.vma 时机 |
|---|---|---|---|
| `head.S:106` | bring-up 第一跳 (stvec trick) | 0 → trampoline\|Sv39 | **之前**：给 setup_vm 的 PTE store 排序<br>**之后** Linux 没立刻 sfence —— 因为 stvec trap 后下一段代码不依赖 stale TLB |
| `head.S:122` | bring-up 切到 early_pg_dir | trampoline → early\|Sv39 | **之后** sfence.vma 清 trampoline 期间 cache 的 TLB 条目 |
| `init.c:1381` `setup_vm_final` | bring-up 切到 swapper | early → swapper\|Sv39 | **之后** `local_flush_tlb_all()`（= sfence.vma） |
| `init.c:911-912` `set_satp_mode` | 探测硬件支持的最大 SATP_MODE | 0 → identity\|TestMode → 0 | 前后各 `local_flush_tlb_all()`；靠 identity 2MB 大页 cover 函数代码 |
| `context.c:192-197` `set_mm_asid` | 进程上下文切换（ASID 开启） | swapper or prev\|ASID_a → next\|ASID_b | 仅在 `need_flush_tlb` 时 `local_flush_tlb_all()`；常规情况靠 ASID 让 TLB 项自动隔离 |
| `context.c:203-204` `set_mm_noasid` | 进程上下文切换（无 ASID） | prev → next | **之后**立即 `local_flush_tlb_all()`（注释明说 "blindly nuke entire local TLB"） |
| `suspend.c:85` `cpu_resume` | 唤醒后恢复 satp | （恢复前的快照） | 由上下文外围保证 |

观察规律：

1. **bring-up 路径**两次写 satp 之间不放 sfence.vma，因为这两次的"当前 PC 那一页映射"被设计成一致；
2. **进程切换**靠 ASID 时常常省 sfence（旧映射会被 ASID 隔离），不开 ASID 时必须 sfence；
3. `csrw satp` **之前**的 sfence.vma 通常是为了排序之前的 PTE store；**之后**的 sfence.vma 通常是为了清 stale TLB。

---

## 6. Q4：`satp` 非零 → 非零，stale TLB 能不能让访问"意外成功"？

**结论：可能，且这正是 ASID-less 切换必须显式 sfence 的根本原因。**

### 6.1 场景复述

设：

- 切换前 `satp = OLD`（某进程或某早期表）
- 切换后 `satp = NEW`，**NEW 表里某 VA `P` 没有映射**
- 中间没有 SFENCE.VMA
- 紧接着 CPU 要 fetch 或访问地址 `P`

按 spec：
- 走 NEW 表 walk → fail（NEW 里没这条映射）
- TLB 查找：若 OLD 时期 `P` 被走过表 → TLB 里有条目 → 按 `(VA, ASID)` 标签匹配查找请求 → **若标签匹配，TLB hit 返回旧映射 → 访问成功！**

也就是说，**切了 satp 却像没切**。Spec 原话精神：实现可以使用任何在最近一次相关 SFENCE.VMA 之前曾对该 VA 合法的 cached translation。

### 6.2 决定是否真发生的两个因素

**因素 A：ASID 标签匹配**

TLB 条目的标签包含 `(VA, ASID)`（带 G 标志的项忽略 ASID）。新查找用的 ASID 来自**新 satp.ASID**。

| 场景 | 新旧 satp ASID | TLB 标签匹配？ | 后果 |
|---|---|---|---|
| 不开 ASID（`use_asid_allocator=false`） | 旧 0，新 0 | **匹配** | 旧的所有非 G 项都可能被命中 |
| 开 ASID，切到新进程（rollover 之外） | ASID_a → ASID_b | **不匹配**（除带 G 的项） | 用户态映射自动隔离，内核态（G 项）跨进程不变 |
| 开 ASID，ASID 复用（rollover） | ASID_x（旧含义）→ ASID_x（新含义） | **匹配** | 必须显式 flush，否则用旧映射 |

**因素 B：硬件 TLB 的替换/查询策略**

哪怕标签匹配，TLB 容量有限，旧条目可能已被新表的项挤掉。所以"是否命中"是个**实现相关的概率事件**——这正是 spec 不会让你"靠它救程序"的原因，你必须假设 worst case。

### 6.3 Linux 中的实证：`set_mm_noasid` vs `set_mm_asid`

**ASID-less 路径** (`arch/riscv/mm/context.c:200-205`)：

```c
static void set_mm_noasid(struct mm_struct *mm)
{
    /* Switch the page table and blindly nuke entire local TLB */
    csr_write(CSR_SATP, virt_to_pfn(mm->pgd) | satp_mode);
    local_flush_tlb_all();        // ← 必须！原因就是 §6.1 的 stale 命中
}
```

注释 "blindly nuke entire local TLB" 写得很直白：因为不开 ASID 时无法靠标签隔离，**只能粗暴清光**。
否则上一个进程的用户态映射会被新进程"意外继承"——既是安全漏洞（看到别的进程的数据），也是正确性 bug（写到别的进程的页）。

**ASID 路径** (`arch/riscv/mm/context.c:144-198` 的 `set_mm_asid`)：

```c
switch_mm_fast:
    csr_write(CSR_SATP, virt_to_pfn(mm->pgd) |
              (cntx2asid(cntx) << SATP_ASID_SHIFT) |
              satp_mode);

    if (need_flush_tlb)
        local_flush_tlb_all();
```

常规情况 `need_flush_tlb=false`，**没有 sfence.vma**——为什么这次安全？因为切换前后 ASID 不同，旧 ASID 标签的项与新查找标签不匹配，硬件自动隔离。
只有 ASID rollover（这个 ASID 编号被复用给新进程）时 `need_flush_tlb=true`，必须 flush。这是 ASID 机制的核心价值：把"必须 sfence 的频率"从"每次进程切换"降到"每轮 ASID 用完一次"。

### 6.4 反过来：head.S 第二次写 satp 为什么不用怕 stale "意外成功"？

trampoline → early 切换时，恰好两表对当前 PC 那一页**映射完全相同**：

- 走 NEW (early) 表：成功，结果 PA=kernel_phys
- TLB 命中 OLD (trampoline) 的 stale 条目：结果 PA=kernel_phys

**两条路径结果一致**。所以这次"是否使用 stale"无所谓，怎么走都对。注释 "the first superpage is translated the same way" 正是此意。

之后那条 `sfence.vma` 才是用来清掉 trampoline 期间可能 cache 进的**其他 VA**（比如如果 walker 预取过 fixmap 路径，虽然 trampoline 没填）的 stale 条目，确保后续 fixmap 等访问一定 walk early 表。

### 6.5 教训：编写涉及 satp 切换的代码时的 checklist

1. **新旧 satp 是否会出现"新表无映射 + 旧 TLB 仍可能命中"的 VA？**
   - 如果新进程的用户态 VA 和旧进程不同、又同 ASID → ✅ 是 → 必须 sfence
   - 如果开 ASID 且未 rollover → ❌ 不会被命中 → 可以省 sfence

2. **当前 PC 那一页在新表 walk 结果与旧表是否一致？**
   - 一致（head.S 那种）→ 切换瞬间无论靠 walk 还是 stale 都正确
   - 不一致 → 必须保证 sfence 在切到该 PC 之前已经清掉 stale

3. **是否依赖 stale 命中"救场"？**
   - **永远不要**。哪怕硬件如此，spec 不保证、未来 CPU 也可能不命中。
   - "意外成功"只是 bug 的另一种表现，比 page fault 更难调试。

---

## 7. 反过来回答"假如不放 sfence.vma 会怎样"

| 漏放位置 | 可能后果 |
|---|---|
| 写 satp 前漏放（PTE 写完没排序） | 硬件 walker 看到部分写好的 PTE，地址翻译错误，page fault 或访问到错的 PA |
| 写 satp 后漏放（TLB 没清） | 后续访问命中旧映射，看起来"切了表却没切"，难调试 |
| 既不需要 PTE 排序、TLB 也没旧条目 | OK，可以不放（如 Q1 路径） |
| 切表前后当前 PC 那一页映射不同 | 即使加 sfence.vma 也救不了——sfence.vma 本身就 fetch 失败。这是设计 bug。 |

---

## 8. 常见误解辨析

> **"csrw satp 之后必须 sfence.vma 才能让新表生效"** ❌
> 不准确。spec 保证 csrw 写完，下一条指令开始时 satp 已是新值；sfence.vma 是为了 **清 TLB stale**，不是为了 **激活 satp**。

> **"trampoline_pg_dir 是 identity mapping，所以 VA==PA 让取指成功"** ❌
> 不是。trampoline 映射的是 VA `0xffffffff80...` → PA `0x80...`，VA≠PA。RISC-V Linux 不靠 identity，而靠 stvec trick。

> **"sfence.vma 是用来确保下一条 fetch 用新 satp"** ❌
> 不是。下一条 fetch 自动用新 satp。sfence.vma 是给 PTE store 和 TLB 排序的。

> **"两条 csrw satp 之间必须有 sfence.vma"** ❌
> 不必。前提是中间这段代码所在页的映射在两个 satp 下保持一致，就可以省（head.S 就是这么做的）。

> **"ASID 切换不需要 sfence.vma"** ⭕（条件成立时）
> 当 ASID 实现且新 ASID 没被复用过时确实不需要。set_mm_asid 中只有 `need_flush_tlb` 时才 flush。

---

## 9. 把所有事实摆在一起的总图

```
                  csrw satp, NEW
                  ─────────────────
                      ↓
   ┌──────────────────────────────────────┐
   │ 立刻生效（按 spec）:                  │
   │   - 下一条 fetch 用 NEW 走 page table │
   │   - 下一条 load/store 用 NEW 翻译     │
   │ 但允许使用 cache 里 OLD 时代 TLB 条目 │
   └──────────────────┬───────────────────┘
                      ↓
   下一条指令 fetch  → ┬→ walk 新表成功
                      │     └─→ 继续执行
                      ├→ walk 新表失败
                      │   ├ TLB 残留旧条目命中 → 继续执行（用旧映射）
                      │   └ TLB 没命中 → instruction page fault → 跳 stvec
                      └→ 命中旧 TLB 条目（不 walk 新表）
                          └─→ 继续执行（用旧映射，可能 stale）

       想清掉残留旧 TLB → 显式 SFENCE.VMA
       想保证之前 PTE store 对 walker 可见 → 在 csrw satp 之前 SFENCE.VMA
```

---

## 10. 参考

- **RISC-V Privileged Architecture Spec, Vol II**（最新版可在 <https://riscv.org/technical/specifications/> 下载）：
  - "Supervisor Address Translation and Protection (satp) Register" — satp 字段与写入语义
  - "Supervisor Memory-Management Fence Instruction (SFENCE.VMA)" — TLB 与 PTE 排序
  - "Translation Process" — walker / TLB / 何时 fault
- **Linux 6.18 源码**（本仓库）：
  - `arch/riscv/kernel/head.S:71-126` — stvec trick + 两次 csrw satp
  - `arch/riscv/mm/init.c:870-928` — set_satp_mode 用 identity mapping 探测硬件
  - `arch/riscv/mm/init.c:1381` — setup_vm_final 切到 swapper
  - `arch/riscv/mm/context.c:144-225` — set_mm_asid / set_mm_noasid（进程切换）
  - `arch/riscv/include/asm/tlbflush.h:18-28` — `local_flush_tlb_all` 直接展开成 `sfence.vma`
- 配套文档：[RISC-V Sv39 早期页表构建过程](riscv-sv39-pagetable-bringup.md) · [步进播放器](riscv-sv39-pagetable-bringup.html)
