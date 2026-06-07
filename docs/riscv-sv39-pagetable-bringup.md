# RISC-V (Sv39) 早期页表构建过程梳理

> 🤖 **本文档由 Minimax3 生成** · 基于 Linux 6.18 源码梳理，请结合源码交叉验证。
>
> 范围：从 `arch/riscv/kernel/head.S` 入口开始，直到 `start_kernel()` 调用 `rest_init()` 派生出 init 进程之前。
> 目标 ISA：`riscv64 + Sv39`（仅 3 级页表：PGD → PMD → PTE，`P4D`/`PUD` 折叠）。
> **本文只考虑 S-mode 内核**（即 SBI/OpenSBI 跑在 M-mode、Linux 跑在 S-mode 的常规组合），不考虑 `CONFIG_RISCV_M_MODE` 的 nommu / 单一 M-mode 场景。
> 内核版本：Linux 6.18（当前 worktree）。
>
> 关键源码：
> - `arch/riscv/kernel/head.S`
> - `arch/riscv/mm/init.c`
> - `arch/riscv/include/asm/pgtable.h`
> - `arch/riscv/include/asm/page.h`
> - `arch/riscv/include/asm/csr.h`
> - `arch/riscv/include/asm/fixmap.h`
> - `Documentation/arch/riscv/vm-layout.rst`

---

## 0. 背景与术语速查

### 0.1 satp 寄存器（Sv39 视角）

`csr.h:67-75` 给出 64bit 下的布局：

| 字段     | 位          | 含义                                                                 |
| -------- | ----------- | -------------------------------------------------------------------- |
| `MODE`   | `[63:60]`   | `0x0`=Bare（关 MMU）/ `0x8`=Sv39 / `0x9`=Sv48 / `0xA`=Sv57           |
| `ASID`   | `[59:44]`   | 地址空间 ID                                                          |
| `PPN`    | `[43:0]`    | 根页表（PGD）所在物理页帧号（PFN）= 物理地址 >> 12                  |

约定常量：
```c
#define SATP_MODE_39  _AC(0x8000000000000000, UL)   // Sv39
#define SATP_MODE_48  _AC(0x9000000000000000, UL)
#define SATP_MODE_57  _AC(0xa000000000000000, UL)
```

`MODE==0` 时即 **MMU off**（Bare 模式），地址直接以物理地址访问；本文 "MMU 关" / "MMU 开 + satp=X" 都按这个口径表述。

### 0.2 Sv39 三级页表

`arch/riscv/include/asm/pgtable-64.h`：

| 级别 | 移位 (SHIFT) | 映射粒度 | 备注                            |
| ---- | ------------ | -------- | ------------------------------- |
| PGD  | 30           | 1 GiB    | Sv39 时 `P4D_SHIFT_L3=PUD_SHIFT_L3=PGDIR_SHIFT_L3=30`，P4D/PUD 折叠 |
| PMD  | 21           | 2 MiB    | 内核 mapping 使用 2 MiB 大页    |
| PTE  | 12           | 4 KiB    | 常规页                          |

Sv39 = 39 位虚拟地址 = 512 GB 用户态 + 512 GB 内核态。

### 0.3 关键虚拟地址（Sv39，见 `Documentation/arch/riscv/vm-layout.rst`）

```
ffffffc4fea00000 - ffffffc4feffffff   6 MB   fixmap
ffffffc4ff000000 - ffffffc4ffffffff  16 MB   PCI io
ffffffc500000000 - ffffffc5ffffffff   4 GB   vmemmap
ffffffc600000000 - ffffffd5ffffffff  64 GB   vmalloc/ioremap
ffffffd600000000 - fffffff5ffffffff 128 GB   direct mapping (PAGE_OFFSET_L3)
ffffffff00000000 - ffffffff7fffffff   2 GB   modules, BPF
ffffffff80000000 - ffffffffffffffff   2 GB   kernel image (KERNEL_LINK_ADDR)
```

```c
#define PAGE_OFFSET_L3   _AC(0xffffffd600000000, UL)
#define KERNEL_LINK_ADDR (ADDRESS_SPACE_END - SZ_2G + 1)   // 0xffffffff80000000
```

### 0.4 几张关键页表（`arch/riscv/mm/init.c:369-496` 静态对象）

| 名称                  | 用途                                                                 | 段 / 生命周期             |
| --------------------- | -------------------------------------------------------------------- | ------------------------- |
| `early_pg_dir[]`      | 早期 PGD：暂时同时覆盖 fixmap + 内核镜像 (PMD 大页)，引导用         | `.init.data`，setup_vm_final 后释放 |
| `early_pmd[]`         | 早期 PMD                                                             | 同上                      |
| `early_pud[]`/`early_p4d[]` | 仅在 Sv48/Sv57 路径才用，Sv39 折叠不真正引用                  | 同上                      |
| `trampoline_pg_dir[]` | "跳板" PGD：只覆盖内核镜像所在 1 个 PGD 槽（PMD 大页）              | `.bss`，relocate 后失效   |
| `trampoline_pmd[]`    | trampoline 的 PMD                                                    | 同上                      |
| `fixmap_pte[]` / `fixmap_pmd[]` / (`fixmap_pud[]`/`fixmap_p4d[]`) | 固定映射区使用的中间页表，全程保留 | `__page_aligned_bss`     |
| `swapper_pg_dir[]`    | **最终** 内核 PGD（init_mm.pgd），所有用户进程共享内核空间的源       | `__page_aligned_bss`，长期 |

### 0.5 `pt_ops`：根据"现在 MMU 处于什么阶段"切换页表分配策略 (`init.c:1028-1080`)

```c
struct pt_alloc_ops pt_ops;
```

| 阶段                 | `pt_ops_set_early`        | `pt_ops_set_fixmap`       | `pt_ops_set_late`            |
| -------------------- | ------------------------- | ------------------------- | ---------------------------- |
| MMU 状态             | OFF                       | 已 ON（trampoline 或 early_pg_dir） | ON + swapper_pg_dir          |
| `alloc_pmd`          | 静态返回 `early_pmd` PA   | `memblock_phys_alloc()`   | `pagetable_alloc()`          |
| `get_pmd_virt`       | `pa == va` 直接强转       | 通过 `fixmap` 临时挂载读写 | `__va(pa)` 走线性映射         |

> 关键点：`pt_ops` 控制"分配 PTE/PMD 页时怎么拿到物理空间，以及怎么用 CPU 访问它"，因为 **MMU off → MMU on with limited mapping → MMU on with full linear map** 三阶段下，对页表页的可达方式完全不同。

---

## 1. 总览：satp 在整个 bring-up 过程中的状态机

```
SBI/M-mode firmware  ──▶  S-mode 入口 _start
                                │
                                │     satp = 0  (MMU OFF, Bare)
                                ▼
                  _start → _start_kernel ────────────────┐
                                │                        │
                                │ MMU OFF                │
                                │                        │
                       call setup_vm(dtb_pa)             │
                                │                        │
                                │ ★ 在 MMU OFF 状态构建: │
                                │   - fixmap PMD/PTE 中间页表
                                │   - trampoline_pg_dir：1 个 PGD 槽 → 内核 2MB
                                │   - early_pg_dir     ：内核全镜像 + fixmap
                                │   - fdt fixmap（FIX_FDT）
                                │                        │
                                ▼                        │
                  call relocate_enable_mmu(early_pg_dir) │
                                │                        │
                                │ ① satp = trampoline_pg_dir | SATP_MODE_39
                                │   ── 此时只有 kernel_map.virt_addr 对应的 2MB 映射；
                                │      只有它能 fetch；该 PMD 是 PA==VA 也 OK，
                                │      所以执行流可以无缝从物理跳到虚拟。
                                │
                                │ stvec 提前指向写完 satp 之后的虚拟地址 1: ；
                                │ csrw 之后下一条 fetch 即用新虚地址。
                                │
                                │ ② csrw satp, a2   (a2 = early_pg_dir | SATP_MODE_39)
                                │   sfence.vma
                                │   ── 切到 early_pg_dir：内核全 +  fixmap 全可达
                                │
                                ▼ ret 回到 head.S
                  call setup_trap_vector
                  call soc_early_init
                  tail start_kernel
                                │
                                │ MMU ON, satp = early_pg_dir | SATP_MODE_39
                                │
                                ▼
                  start_kernel() → setup_arch() → paging_init()
                                │
                                │     setup_bootmem()       ── memblock 初始化
                                │     setup_vm_final()
                                │       ├─ 构建 swapper_pg_dir 的 fixmap PGD entry
                                │       ├─ 构建 swapper_pg_dir 的 linear mapping
                                │       ├─ 64bit: 把内核镜像重新映射到 swapper
                                │       ├─ csrw satp, swapper_pg_dir | SATP_MODE_39 ←★
                                │       ├─ local_flush_tlb_all()
                                │       └─ pt_ops_set_late()
                                │
                                ▼
                  start_kernel() 余下流程 …… → rest_init() → kernel_init (PID 1)
```

各阶段构建工作所"处于的 satp"，下文细说。

---

## 2. 入口阶段：`head.S` _start → _start_kernel（satp == 0，MMU OFF）

入口符号链接顺序：OpenSBI（M-mode 固件）完成自身初始化后，以 **S-mode** 把控制权交给内核镜像的物理首地址 `_start`，即 `__HEAD` 段的第一条指令（`head.S:20-39`）。SBI 调用约定：`a0 = boot hartid`，`a1 = dtb_pa`：

```asm
SYM_CODE_START(_start)
    ...
    j _start_kernel       # 越过镜像头里的元数据
```

镜像头位于 `_start` 之后 8 字节起，记录内核 load offset、size、magic 等，给 bootloader 用。

`_start_kernel`（`head.S:203` 起）在 **satp == 0、MMU off** 状态下做以下事情（仅列 S-mode 路径）：

| 步骤 | 源码位置                            | 行为                                                                                                   |
| ---- | ----------------------------------- | ------------------------------------------------------------------------------------------------------ |
| 1    | `head.S:205-206`                    | 屏蔽中断 `csrw CSR_IE,0; csrw CSR_IP,0`                                                               |
| 2    | `head.S:236-238`                    | 打开 S-mode 访问 `time` CSR：`li t0,0x2; csrw CSR_SCOUNTEREN,t0`（让用户态/内核态读 `rdtime` 不再陷入 M-mode） |
| 3    | `head.S:242`                        | `load_global_pointer` 加载 `__global_pointer$`（PC-relative）                                          |
| 4    | `head.S:247-249`                    | 关闭 FPU/VECTOR：`csrc CSR_STATUS, SR_FS_VS`（默认不允许内核态浮点/向量，便于检出误用）              |
| 5    | `head.S:251-277`                    | "hart lottery"：通过 `amoadd.w` 让多个 hart 中只有一个进入主初始化路径；其余跳到 `.Lsecondary_start`，等待 boot CPU 把它们叫起来 |
| 6    | `head.S:291-301`                    | 清 BSS：`la a3,__bss_start; la a4,__bss_stop; ...; REG_S zero,(a3); ...`（这一步把 `swapper_pg_dir`、`trampoline_pg_dir`、`fixmap_p*d` 等 BSS 中的页表对象全清零）|
| 7    | `head.S:302-304`                    | `boot_cpu_hartid = a0`（SBI 通过 `a0` 把当前 hartid 传进来）                                          |
| 8    | `head.S:307-311`                    | 准备 C 环境：`tp = init_task`, `sp = init_thread_union + THREAD_SIZE - PT_SIZE_ON_STACK`              |
| 9    | `head.S:312-317`                    | `a0 = dtb_pa`（SBI 通过 `a1` 传 DTB 物理地址，这里转入 `a0` 给 `setup_vm`）                          |
| 10   | `head.S:318-321`                    | 设 trap vector 为 `.Lsecondary_park`（出错则死循环 wfi），然后 **`call setup_vm`**                    |

> 重点：到 `call setup_vm` 时，**`satp = 0`，MMU 处于 Bare 模式**，所有访问都是物理地址。因此 `setup_vm` 内编译要求 `cmodel=medany`（PC-relative）并不被 ftrace 插桩，原因写在 `init.c:931-947` 注释里。

---

## 3. `setup_vm()`：MMU OFF 状态下构建三套早期页表

文件 `arch/riscv/mm/init.c:1100-1272`。**全过程仍是 `satp=0`**（除了 §3.3 在 Sv57/Sv48 自探测时短暂写入又清除）。

### 3.1 计算 `kernel_map`（PA/VA 关系）

`init.c:1123-1163`：

```c
kernel_map.virt_addr = KERNEL_LINK_ADDR + kernel_map.virt_offset;   // 默认 0xffffffff80000000 + KASLR_off
kernel_map.phys_addr = (uintptr_t)(&_start);
kernel_map.size      = (uintptr_t)(&_end) - kernel_map.phys_addr;
kernel_map.va_kernel_pa_offset = kernel_map.virt_addr - kernel_map.phys_addr;
kernel_map.va_pa_offset        = 0UL;     // 64-bit：留到 setup_bootmem 再算 (因为还要 PAGE_OFFSET 对齐)
```

注意：**线性映射所用的 `va_pa_offset` 还没确定**，所以本阶段一切都只能用"内核镜像的 VA↔PA"关系（`va_kernel_pa_offset`），不能用 `__va()`/`__pa()` 走线性映射。

### 3.2 决定页表级数 / `satp_mode`

`init.c:49-60, 870-928`，仅 64-bit + !XIP 走 `set_satp_mode(dtb_pa)`：

- 默认假设 Sv57：`satp_mode = SATP_MODE_57; pgtable_l5_enabled = true`
- 从 cmdline (`no4lvl`/`no5lvl`) 和 DT 的 `mmu-type` 取最低值
- 否则用 **identity mapping 自探测**：临时建立 `set_satp_mode` 函数所在 2MB 的 1:1 映射，写入 `satp = early_pg_dir | SATP_MODE_57/48`，再读回看硬件是否接受；不行就降级到 48，再不行降到 39
- 探测结束后把 `early_pg_dir / early_p4d / early_pud / early_pmd` 全部 `memset(0)` 还原

> **Sv39 视角**：如果 `mmu-type = "riscv,sv39"` 或加了 `no4lvl`，`set_satp_mode` 在 880 行直接走两次 disable：
> ```c
> disable_pgtable_l5();   // pgtable_l5_enabled=false; satp_mode=SATP_MODE_48
> disable_pgtable_l4();   // pgtable_l4_enabled=false; satp_mode=SATP_MODE_39; PAGE_OFFSET=PAGE_OFFSET_L3
> return;                 // 不做硬件探测，也不写 satp
> ```
> 此时仍然 `satp=0`。

### 3.3 切换 `pt_ops` 到 early 模式

`init.c:1192` → `pt_ops_set_early()`：所有 `alloc_pmd` 都返回静态 `early_pmd` 的物理地址，`get_pmd_virt(pa)` 直接 `(void*)pa`。在 MMU off 下完全合理。

### 3.4 构建 fixmap 中间页表（不映射叶子页）

`init.c:1194-1208`，**MMU OFF**：

```c
/* PGD[fixmap] → fixmap 下一级页表 */
create_pgd_mapping(early_pg_dir, FIXADDR_START,
                   (uintptr_t)fixmap_pmd  /* Sv39: 下一级直接是 PMD */,
                   PGDIR_SIZE, PAGE_TABLE);

/* Sv39 折叠掉 P4D/PUD，只剩这一条 */
create_pmd_mapping(fixmap_pmd, FIXADDR_START,
                   (uintptr_t)fixmap_pte, PMD_SIZE, PAGE_TABLE);
```

注意：

- 只建立 **PGD→PMD→PTE 的指针链**，叶子 PTE 全部为 0（`fixmap_pte` 还在 BSS）。后续 `__set_fixmap()` 会把具体 PA 填进 `fixmap_pte[idx]`。
- `fixmap_pmd` 等指针都是 BSS 中静态数组的 **物理地址**（因为还在 MMU off 阶段）。

### 3.5 构建 trampoline 页表（只覆盖内核镜像那 2 MiB）

`init.c:1210-1228`：

```c
create_pgd_mapping(trampoline_pg_dir, kernel_map.virt_addr,
                   (uintptr_t)trampoline_pmd, PGDIR_SIZE, PAGE_TABLE);
create_pmd_mapping(trampoline_pmd, kernel_map.virt_addr,
                   kernel_map.phys_addr,           /* PA */
                   PMD_SIZE, PAGE_KERNEL_EXEC);   /* 2MB 大页 + R|W|X */
```

- 只填满 `trampoline_pg_dir` 中 **1 个 PGD 槽**（对应内核镜像所在 1 GiB 区间）；其它 511 个 PGD 槽留 0。
- 该 PGD 槽指向 `trampoline_pmd`，PMD 中放一个 **2 MiB 叶子 PMD 项**，把 `kernel_map.virt_addr` 映射到 `kernel_map.phys_addr`。
- ⚠️ **这是一条 VA → PA 的"高地址映射"，不是 identity mapping**：
  - 64-bit Sv39 下 `kernel_map.virt_addr ≈ 0xffffffff80000000`（KERNEL_LINK_ADDR + KASLR offset）
  - `kernel_map.phys_addr` = 内核镜像在 DRAM 的实际加载物理地址（如 `0x80200000`）
  - 显然 **VA ≠ PA**。Linux RISC-V 启动不使用经典的 identity mapping，而是用 §4.2 的 **stvec trick** 来跨越 MMU 开启这一瞬间。
- 名字"跳板"的真正含义：**让 CPU 在 MMU 打开之后能从 stvec 跳到真正在虚拟空间的下一条指令**，因此它只要保证"`1:` 标号所在那 2 MiB 在新 satp 下能取指"就够了。

### 3.6 构建 early_pg_dir：覆盖整个内核镜像

`init.c:1231-1236` → `create_kernel_page_table(early_pg_dir, true)`：

```c
for (va = kernel_map.virt_addr; va < end_va; va += PMD_SIZE)
    create_pgd_mapping(pgdir, va,
                       kernel_map.phys_addr + (va - kernel_map.virt_addr),
                       PMD_SIZE,
                       PAGE_KERNEL_EXEC);   /* early=true，所有段先按 RWX */
```

也就是 **内核镜像的每个 2 MiB**，都在 `early_pg_dir` 里以 2 MiB 大页映射一遍。由于 `alloc_pmd_early` 只返回 `early_pmd` 的物理地址，整个内核 mapping 共享同一 `early_pmd`（这就是为什么 `alloc_pmd_early` 里 `BUG_ON((va - kernel_map.virt_addr) >> PUD_SHIFT)` —— 不允许跨 1 GiB）。

### 3.7 把 DTB 区域映射到 fixmap

`init.c:1238-1239` → `create_fdt_early_page_table(__fix_to_virt(FIX_FDT), dtb_pa)`，64-bit 走：

```c
create_pmd_mapping(fixmap_pmd, fix_fdt_va,
                   pa, PMD_SIZE, PAGE_KERNEL);
create_pmd_mapping(fixmap_pmd, fix_fdt_va + PMD_SIZE,
                   pa + PMD_SIZE, PMD_SIZE, PAGE_KERNEL);
dtb_early_va = (void *)fix_fdt_va + (dtb_pa & (PMD_SIZE - 1));
```

这两个 2 MiB 大页是 **直接写到 `fixmap_pmd` 中**（fixmap PMD 既被 early_pg_dir 也即将被 swapper_pg_dir 链接），所以**只要 fixmap PGD 项就位、谁都能看到 FDT**。

### 3.8 切换 `pt_ops` 到 fixmap 模式

`init.c:1271` → `pt_ops_set_fixmap()`：把 `alloc_pmd/get_pmd_virt` 改成 "memblock 分配 + 通过 FIX_PMD 临时映射访问"。这是为 **下一阶段 MMU 打开之后** 仍能动态分配/读写页表页做准备。

> 注意：此时还在 MMU off，但 `pt_ops_set_fixmap` 里把函数指针通过 `kernel_mapping_pa_to_va(...)` 转成了内核虚拟地址，意味着 **下次调用必须在 MMU 已开、且 kernel mapping 已生效** 之后。

### 3.9 setup_vm 完成时的状态

- `satp = 0`，MMU 还是 OFF
- 页表内存对象的状态：

| 对象              | 内容                                                                                                                       |
| ----------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `trampoline_pg_dir` | 只 1 个 PGD 槽指向 `trampoline_pmd`；后者 1 个 PMD 项 2MB 大页：`kernel_map.virt_addr → kernel_map.phys_addr` (RWX)         |
| `early_pg_dir`      | (a) 每 2 MiB 一条 PGD/PMD 项覆盖整个内核镜像 (RWX); (b) `FIXADDR_START` 对应的 PGD 项指向 `fixmap_pmd`                       |
| `fixmap_pmd`        | (a) 一条 PMD 项指向 `fixmap_pte`（叶子全 0）；(b) 两个 PMD 项 2MB 大页指向 FDT 物理区                                       |
| `fixmap_pte`        | 全 0                                                                                                                       |
| `swapper_pg_dir`    | 全 0（还没碰）                                                                                                             |

---

## 4. `relocate_enable_mmu`：在 MMU OFF → ON 的临界点切换 satp

`head.S:71-126`，从 `_start_kernel` 中 `call relocate_enable_mmu`（参数 `a0 = early_pg_dir 物理地址`）进入。

### 4.1 修正返回地址到虚拟空间

```asm
la a1, kernel_map
REG_L a1, KERNEL_MAP_VIRT_ADDR(a1)   # a1 = kernel_map.virt_addr
la a2, _start                        # a2 = _start 的物理地址
sub a1, a1, a2                       # a1 = va_kernel_pa_offset
add ra, ra, a1                       # 把返回地址 ra 改成虚拟地址
```

执行后，`ret` 时将跳到虚拟空间内对应的下一条指令。

### 4.2 把 stvec 指向 satp 之后那一条虚拟 PC

```asm
la a2, 1f                            # 物理地址
add a2, a2, a1                       # → 虚拟地址
csrw CSR_TVEC, a2
```

**整个 MMU 启用机制中最巧妙的一招**：

- 写 satp 之后，硬件用新 satp 把 PC 当 VA 翻译。在 64-bit Sv39 下，PC 当前是个低地址 PA（例如 `0x80200124`），不在 trampoline 的高地址映射范围 → 必然触发 instruction page fault
- trap handler = `stvec`。我们提前把 `stvec` 设成 `1:` 标号的 **VA**（如 `0xffffffff800001xx`），这是 trampoline 那条 2 MiB 大页里的合法 VA → CPU 跳过去后能正常翻译并取指
- 这是一次"借 trap 完成跨地址空间跳转"的手法，**等价于其他 ISA 中 identity-mapping 的作用，但路径完全不同**

（在 32-bit 或者 KERNEL_LINK_ADDR 设得很低的场景下，如果碰巧 `0x80200124` 这个数值同时也是 trampoline 里映射的某个 VA，就会 fall through 不触发 trap —— `head.S:95-100` 注释里说的 "or simply fall through if VA == PA" 就是这种情况。Sv39 + KERNEL_LINK_ADDR=`0xffffffff80000000` 永远走 trap 路径。）

### 4.3 算好但不写入：`a2 = early_pg_dir_PA >> 12 | satp_mode`

```asm
srl a2, a0, PAGE_SHIFT
la  a1, satp_mode
REG_L a1, 0(a1)
or  a2, a2, a1                       # a2 待用：早期 PGD 的 satp 值
```

### 4.4 ① 写入 trampoline_pg_dir：开启 MMU 并跳到虚拟空间

```asm
la a0, trampoline_pg_dir
srl a0, a0, PAGE_SHIFT
or  a0, a0, a1                       # a0 = trampoline | satp_mode
sfence.vma
csrw CSR_SATP, a0                    # ★★ MMU 开启，satp = trampoline | Sv39
.align 2
1:
```

写完这一瞬间是整篇文档最关键的一段，慢动作解析：

1. **PC 寄存器的"数值"不会因为写 satp 而改变**。写之前 `csrw` 这条指令本身位于某个 PA（例如 `0x80200120`），写完之后 PC 还是 `0x80200124`（下一条指令的 PA）。
2. MMU 此时已开（mode=Sv39），CPU 把 PC 当成 **虚拟地址** 来取下一条指令。它去查 `trampoline_pg_dir`：
   - `trampoline_pg_dir` 只在 `kernel_map.virt_addr ≈ 0xffffffff80000000` 那一支有项
   - 它根本没有 `0x80200124` 这种"低半区"的 VA → walk 失败 → **instruction page fault**
3. 触发 trap，CPU 跳到 `stvec`。**而 stvec 在 §4.2 已经被预先设成了 `1:` 标号的虚拟地址**（约 `0xffffffff800001xx`），这个 VA 正好落在 trampoline 那 2 MiB 大页里，可以正常翻译成 `1:` 的 PA → 取指成功 → 从 `1:` 继续执行。
4. （对照：32-bit 或某些低虚地址布局下，如果碰巧 `0x80200124` 本身也是个有效 VA，trampoline 又把它映射了，那就直接 fall through 不触发 trap —— `head.S:95-100` 注释里说的 "or simply fall through if VA == PA" 就是这种情况。**Sv39 + KERNEL_LINK_ADDR 永远走 trap 路径**。）

> ⚠️ **不是 identity mapping！** 很多其他 ISA 在 MMU 启动时会临时建立一段 VA==PA 的 identity 段；RISC-V Linux **不这么干**，而是用上述 "stvec 兜底跳转" 的设计把 PC 一步切到高地址 VA。

写 satp 后到走到 `1:` 之间这段，**绝对不能做任何内存访问（含 stack/data）**，原因是：
- 当前 `sp`、`gp` 都是 MMU off 阶段算出来的 PA 值，trampoline 里没有对应映射 → 一旦访问就会再触发 load/store fault
- 所以 head.S 这段只允许 PC-relative 的 `la`、立即数 `li`、以及 CSR / 算术指令，**没有一条 load/store**

**这是 satp 的第一次写入**，从此之后所有 fetch 都走 Sv39 翻译。

### 4.5 ② 写入 early_pg_dir：扩展可见范围到完整内核 + fixmap

```asm
la a0, .Lsecondary_park
csrw CSR_TVEC, a0                    # 暂时把 stvec 改回兜底 park
load_global_pointer                  # 重新加载 gp（现在 PC-relative 算出来的是 VA）

csrw CSR_SATP, a2                    # ★★ 第二次：satp = early_pg_dir | Sv39
sfence.vma
ret
```

要点：

- 进到这里时 PC 已经在 VA 空间（trampoline 翻译过）。重载 `gp` 之前不能访问 stack，所以 head.S 这里仍然只用 PC-relative 指令。
- 写完 satp 后 `sfence.vma` 是确保新 PGD 生效；接着 `ret` 跳到 `ra`。`ra` 在 §4.1 已经被加上 `va_kernel_pa_offset` 改成 VA，所以 `ret` 返回到 head.S 中 `call relocate_enable_mmu` 后面那条指令的 **VA**。
- 切到 early_pg_dir 后能看到的：
  - 整个内核镜像（多 PGD 槽 × 多 PMD 项 × 2 MiB 大页 RWX）
  - `FIXADDR_START` 子树（fixmap_pmd 中预先放好的 FDT 两个 2 MiB 大页可访问；fixmap_pte 里的项暂时全 0，后续 `__set_fixmap` 才填）

> 为什么必须"先 trampoline 再 early"，不直接一步写 early_pg_dir？
>
> 1. **最小化第一次切换的不确定性**。trampoline 只有 1 个 PGD 槽 + 1 个 PMD 大页，是绝对最小集。即使 early_pg_dir 此刻被某个隐含 bug 写坏了别处，第一次切换也不依赖那些位置，能保证最少跳到 `1:` 是稳定的。
> 2. **TLB 状态干净**。两次切换之间各有 `sfence.vma`，让 trampoline 的 entry 不会被错误带入到 early_pg_dir 时代。`head.S:115-120` 注释强调："Fetching the fence is guaranteed to work because that first superpage is translated the same way" —— 这里依赖的是两表对 kernel 那 2 MiB 的映射**结果一致**（VA → 同一个 PA、同样 RWX），所以从 trampoline 切到 early 时，正在执行的指令所在地址依然有效。
> 3. **secondary CPU 复用**。`head.S:166-171` 的 `secondary_start_sbi` 会直接 `call relocate_enable_mmu(swapper_pg_dir)`，也走"trampoline → swapper"两步切换。把这套机制做成统一调用更省事。

从 `ret` 那一刻起，**a) PC 在 VA；b) ra 在 VA；c) 但 sp/tp/gp 中 sp、tp 还是 PA**！紧接着 head.S 的下一段会立刻重载（见 §4.6）。

### 4.6 返回到 `_start_kernel` 之后的余下流程

```asm
                                          # MMU ON, satp = early_pg_dir | Sv39
    call .Lsetup_trap_vector              # stvec = handle_exception (虚拟地址)
    la   tp, init_task                    # ★ 重新加载 tp/sp 为 VA
    la   sp, init_thread_union + THREAD_SIZE
    addi sp, sp, -PT_SIZE_ON_STACK
    scs_load_current
#ifdef CONFIG_KASAN
    call kasan_early_init                 # 还在 early_pg_dir 下做 KASAN 影子区准备
#endif
    call soc_early_init                   # 厂商 SoC hook
    tail start_kernel                     # ↓ 进入通用内核代码
```

接续 §4.5 末尾 "sp/tp 仍是 PA" 的伏笔：

- `call .Lsetup_trap_vector` 是 **leaf call**（`.Lsetup_trap_vector` 内没有 `call`，只有 `csrw`），RISC-V 的 `call` 只更新 `ra` 寄存器、不压栈，所以这一步并不访问 sp 指向的内存，安全。
- 紧接着 `la tp, init_task` 和 `la sp, init_thread_union + THREAD_SIZE` 都是 **PC-relative**：因为现在 PC 已经在 VA 空间，所算出的也是 VA。**这就是 sp / tp 从 PA 切换到 VA 的瞬间**。
- 之后 KASAN/SoC/start_kernel 的所有 C 调用、栈访问就都安全了。

**进入 `start_kernel()` 时**：`satp = early_pg_dir | SATP_MODE_39`，MMU 已开，sp/tp/gp/ra 全部已就位为 VA。

---

## 5. `start_kernel()` → `setup_arch()` → `paging_init()`：构建并切换到 `swapper_pg_dir`

通用入口在 `init/main.c:start_kernel()`。和页表相关的关键路径：

```
start_kernel()
  └─ setup_arch(&command_line)        // arch/riscv/kernel/setup.c
        ├─ parse_dtb()
        ├─ misc_mem_init()  (注：实际位置随版本略有移动)
        └─ paging_init()              // arch/riscv/mm/init.c:1430
              ├─ setup_bootmem()      //  ⒈ 物理内存框架，确定 va_pa_offset
              └─ setup_vm_final()     //  ⒉ 建 swapper_pg_dir 并切换 satp
```

### 5.1 `setup_bootmem()` 阶段做的页表相关事

`init.c:226-326`，**MMU 状态：satp = early_pg_dir | Sv39**：

- 64-bit 第一次设定线性映射偏移：
  ```c
  phys_ram_base                = memblock_start_of_DRAM() & PMD_MASK;
  kernel_map.va_pa_offset      = PAGE_OFFSET - phys_ram_base;
  ```
  在此之前 `va_pa_offset == 0`，所以 `__va()/__pa()` 是不能用于线性映射的；之后才能用。
- 限制可用 RAM 不超过线性映射窗口大小 (`KERN_VIRT_SIZE = 128GB` for Sv39)。
- 预留内核镜像、initrd、reserved-memory、FDT 自身（如果非 builtin）。

### 5.2 `setup_vm_final()`：核心切换 (`init.c:1346-1385`)

仍处于 **satp = early_pg_dir | Sv39**。

```c
static void __init setup_vm_final(void)
{
    /* (a) 让 swapper_pg_dir 也指向同一个 fixmap 子树 */
    create_pgd_mapping(swapper_pg_dir, FIXADDR_START,
                       __pa_symbol(fixmap_pmd),   /* Sv39 时 fixmap_pgd_next = fixmap_pmd */
                       PGDIR_SIZE, PAGE_TABLE);

    /* (b) 建立全部物理内存的线性映射 */
    create_linear_mapping_page_table();           //  for_each_mem_range → create_pgd_mapping

    /* (c) 64-bit：把内核镜像按真正的 RWX 属性映射到 swapper */
    if (IS_ENABLED(CONFIG_64BIT))
        create_kernel_page_table(swapper_pg_dir, false);  // early=false 用 pgprot_from_va

#ifdef CONFIG_KASAN
    kasan_swapper_init();
#endif

    /* (d) 清掉 fixmap 中临时使用的 FIX_PTE/PMD/PUD/P4D */
    clear_fixmap(FIX_PTE);
    clear_fixmap(FIX_PMD);
    clear_fixmap(FIX_PUD);
    clear_fixmap(FIX_P4D);

    /* (e) ★ 切到正式根页表 */
    csr_write(CSR_SATP, PFN_DOWN(__pa_symbol(swapper_pg_dir)) | satp_mode);
    local_flush_tlb_all();

    /* (f) 以后用真正的 buddy allocator 分配页表页 */
    pt_ops_set_late();
}
```

逐步含义：

#### (a) fixmap 子树共享

只在 swapper 里写**一个 PGD 项**（指向 fixmap 子树物理地址）。由于 `fixmap_pmd/fixmap_pte` 是全程同一份 BSS 对象，**所有"早已通过 fixmap_pte 设过 fixmap"的映射（包括正在用的 earlycon、FDT 视图）自动在 swapper 里也可见。**

#### (b) 线性映射 `create_linear_mapping_page_table()` (`init.c:1290-1344`)

对每段 memblock 内存段，调 `create_linear_mapping_range(start, end, 0, NULL)`，再由 `create_pgd_mapping(swapper_pg_dir, va=__va(pa), pa, best_map_size, prot)` 写入：

- `best_map_size` 在 Sv39 下能用 1 GiB (PUD_SIZE，因为 `PUD_SHIFT==30`) 或 2 MiB；属性由 `pgprot_from_va()` 决定。
- `STRICT_KERNEL_RWX` 时 ktext / krodata 区被 `memblock_mark_nomap` 屏蔽，单独以更细粒度映射，避免被 1 GiB 大页"吃掉"。
- 这一阶段 `pt_ops` 是 `set_fixmap`，分配 PMD 用 `memblock_phys_alloc()`，访问该 PMD 时通过 `FIX_PMD` 临时挂载（`get_pmd_virt_fixmap`）。

#### (c) 重映射内核镜像

64-bit 模式下，**内核镜像不在线性映射区，而在 `KERNEL_LINK_ADDR` 那 2 GB**。`create_kernel_page_table(swapper_pg_dir, false)` 沿用与 (a) 同样的循环，但 `early=false` 走 `pgprot_from_va`，分别给出：

- text → `PAGE_KERNEL_READ_EXEC`
- rodata → `PAGE_KERNEL_READ`
- 其余 → `PAGE_KERNEL`

（注释 `init.c:790-805`，`mark_rodata_ro()` 在更晚还会把 rodata 真正改成只读。）

#### (d) `clear_fixmap` 收尾

构建 (b)/(c) 过程中频繁借用 `FIX_PTE/PMD/PUD/P4D` 临时挂载新分配的页表页，到这里全部清理。`fixmap_pte` 中这些临时项归零，避免之后误用。

#### (e) **第三次写 satp**：切到 swapper_pg_dir

```c
csr_write(CSR_SATP, PFN_DOWN(__pa_symbol(swapper_pg_dir)) | satp_mode);
local_flush_tlb_all();
```

- 因为 swapper 已经覆盖了内核镜像、线性映射、fixmap，所以当前 PC、当前 sp、当前 fixmap 视图都不会失效。
- `early_pg_dir` 和它链接的 `early_pmd / early_pud / early_p4d` 现在不再被 satp 引用，将随 `__init` 段被释放 (`__initdata`)；`trampoline_pg_dir/pmd/pud/p4d` 在 BSS 中保留，作为 secondary CPU 启动时的跳板（见 `head.S:166-171`，`secondary_start_sbi` 也走 `relocate_enable_mmu(swapper_pg_dir)`）。

#### (f) `pt_ops_set_late()`

从现在起 PTE/PMD 等通过 `pagetable_alloc()`（buddy + ptdesc 构造），读写通过 `__va(pa)` 走线性映射。前提就是线性映射 (b) 已经建好。

### 5.3 `setup_vm_final` 之后的状态

- **MMU ON，`satp = swapper_pg_dir | SATP_MODE_39`**，一直延续到 init 进程及其它进程切换。
- `init_mm.pgd = swapper_pg_dir`（在 `mm_alloc()` / `init_mm` 静态初始化里挂上）。
- 内核此后的所有页表（vmalloc、vmemmap 实际填充、ioremap、kmap_local…）都基于 swapper_pg_dir，并通过 `preallocate_pgd_pages_range`（`init.c:1487-1543`，由 `pgtable_cache_init()` 调用）预先把 vmalloc/modules/vmemmap/direct map 范围的中间页表分配好，**保证不同进程拷贝 PGD 时这些中间表能共享**。

---

## 6. 从 `paging_init` 到 init 进程之间还会更新页表的关键点

| 时序 | 函数                            | 与页表相关的事                                                                                  | satp                            |
| ---- | ------------------------------- | ----------------------------------------------------------------------------------------------- | ------------------------------- |
| 1    | `setup_arch` 余下                | `unflatten_device_tree`、`paging_init` 已完成                                                   | swapper                         |
| 2    | `mm_core_init()`                | `arch_mm_preinit()`、`mem_init()`→ buddy 上线；`kmem_cache_init()`                              | swapper                         |
| 3    | `mm_core_init()` 末              | `pgtable_cache_init()`：调 `preallocate_pgd_pages_range(VMALLOC_*, MODULES_*, VMEMMAP_*, ...)` 把对应区间的中间页表全分配，避免 fork/clone 后 PGD 出现 "缺中间表" 情形 | swapper                         |
| 4    | `late_time_init`/`init_IRQ`/... | 大多只用现成 fixmap / ioremap                                                                   | swapper                         |
| 5    | `mark_readonly()`               | `mark_rodata_ro()`：把 `__start_rodata..._data` 范围以及线性映射别名设为只读                    | swapper                         |
| 6    | `rest_init()` → `kernel_init`   | PID 1，开始走用户空间                                                                           | swapper（用户进程切换时改 satp）|

> `vmemmap` 区间的实际映射（`SPARSEMEM_VMEMMAP`）由 `misc_mem_init()` → `sparse_init()` → 各 section 的 `vmemmap_populate()` 完成，叶子是大页或常规页，分配走 buddy / memblock。`paging_init` 这里只准备 PGD 中间页面，但实际 vmemmap PFN 范围是在 sparse_init 时填进 swapper_pg_dir 的。

---

## 7. 一图流：satp 三段式时间线

```
时间 →

phys-bringup │ setup_vm() │ relocate_enable_mmu │ start_kernel..paging_init │ setup_vm_final │ rest_init→init
─────────────┼────────────┼─────────────────────┼────────────────────────────┼────────────────┼───────────────
 satp = 0    │  satp = 0  │ ① satp=tramp|Sv39   │ satp = early_pg_dir|Sv39   │ ③ satp=swapper │   ……  swapper
             │  MMU OFF   │ ② satp=early|Sv39   │ MMU ON                     │   |Sv39        │
             │  (Bare)    │  (MMU 真正打开)     │                            │                │

 主要构建对象  trampoline_pg_dir,                               (b) 线性映射 + 内核镜像入 swapper_pg_dir
              early_pg_dir,                                     (a) swapper 共享 fixmap
              fixmap_pmd,                                       (c) 清 FIX_PTE/PMD/PUD/P4D
              fdt 进 fixmap_pmd,                                pt_ops_set_late
              pt_ops_set_early/_fixmap
```

具体到 Sv39，三次写 satp 的值（PPN 用真实 BSS/data 符号的物理地址 >> 12 计算）：

| 序号 | 来源指令位置                              | satp 值                                                                                              | 此刻能访问哪些 VA                                                                                                                                                                                  |
| ---- | ----------------------------------------- | ---------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ①    | `head.S:105-106` `csrw CSR_SATP,a0`       | `SATP_MODE_39 \| PFN(trampoline_pg_dir)`                                                              | 仅 `kernel_map.virt_addr` 起的 2 MiB（且物理地址 = `kernel_map.phys_addr`）                                                                                                                         |
| ②    | `head.S:122` `csrw CSR_SATP,a2`           | `SATP_MODE_39 \| PFN(early_pg_dir)`                                                                  | 整个内核镜像（以 2 MiB 大页 RWX）；`FIXADDR_START` 子树（含已设的 FDT 2 MiB 大页），其余 fixmap_pte 中 PTE 暂为 0                                                                                  |
| ③    | `init.c:1381` `csr_write(CSR_SATP, ...)`  | `SATP_MODE_39 \| PFN(swapper_pg_dir)`                                                                | 整个内核虚拟空间（fixmap、PCI io、vmemmap、vmalloc、direct mapping、modules、kernel 镜像），其中线性映射用 1 GiB/2 MiB 大页，内核镜像段按真实 RWX 属性                                              |

---

## 8. 关键符号 / 函数索引（便于绘图、可视化对照）

### 8.1 静态页表对象

| 符号                 | 位置                                                                                                  |
| -------------------- | ----------------------------------------------------------------------------------------------------- |
| `swapper_pg_dir`     | `arch/riscv/mm/init.c:369`                                                                            |
| `trampoline_pg_dir`  | `arch/riscv/mm/init.c:370`                                                                            |
| `fixmap_pte`         | `arch/riscv/mm/init.c:371`                                                                            |
| `early_pg_dir`       | `arch/riscv/mm/init.c:373`                                                                            |
| `trampoline_pmd`     | `arch/riscv/mm/init.c:474`                                                                            |
| `fixmap_pmd`         | `arch/riscv/mm/init.c:475`                                                                            |
| `early_pmd`          | `arch/riscv/mm/init.c:476`                                                                            |
| `trampoline_p4d/pud` `fixmap_p4d/pud` `early_p4d/pud` | `arch/riscv/mm/init.c:484-496`（Sv39 折叠后不会被实际使用）                  |

### 8.2 函数（按调用顺序）

| 顺序 | 函数                       | 文件:行                          | satp / MMU 状态                                  |
| ---- | -------------------------- | -------------------------------- | ------------------------------------------------ |
| 1    | `_start`                   | `head.S:21`                      | 0 / OFF                                          |
| 2    | `_start_kernel`            | `head.S:203`                     | 0 / OFF                                          |
| 3    | `setup_vm`                 | `init.c:1100`                    | 0 / OFF                                          |
| 3.1  | `set_satp_mode`            | `init.c:870`（仅 64bit+!XIP）    | 0 / OFF；自探测时短暂写又清                       |
| 3.2  | `pt_ops_set_early`         | `init.c:1028`                    | 0 / OFF                                          |
| 3.3  | `create_pgd_mapping` 等     | `init.c:728-752`                  | 0 / OFF                                          |
| 3.4  | `create_fdt_early_page_table` | `init.c:990`                  | 0 / OFF                                          |
| 3.5  | `pt_ops_set_fixmap`        | `init.c:1050`                    | 0 / OFF                                          |
| 4    | `relocate_enable_mmu`      | `head.S:73`                      | OFF → trampoline → early                          |
| 5    | `kasan_early_init`         | `mm/kasan_init.c`                | early_pg_dir / ON                                |
| 6    | `soc_early_init`           | `arch/riscv/kernel/soc.c`        | early_pg_dir / ON                                |
| 7    | `start_kernel`             | `init/main.c`                    | early_pg_dir / ON                                |
| 8    | `setup_arch`               | `arch/riscv/kernel/setup.c`      | early_pg_dir / ON                                |
| 9    | `paging_init`              | `init.c:1430`                    | early_pg_dir / ON                                |
| 9.1  | `setup_bootmem`            | `init.c:226`                     | early_pg_dir / ON；末尾设 `va_pa_offset`         |
| 9.2  | `setup_vm_final`           | `init.c:1346`                    | early → **swapper**                              |
| 9.3  | `pt_ops_set_late`          | `init.c:1068`                    | swapper / ON                                     |
| 10   | `mm_core_init` / `pgtable_cache_init` | `mm/init-mm.c` 等           | swapper / ON                                     |
| 11   | `rest_init → kernel_init`  | `init/main.c`                    | swapper / ON                                     |

### 8.3 satp 状态变量

| 符号                  | 文件:行           | 默认值（无 XIP，64bit）       |
| --------------------- | ----------------- | ---------------------------- |
| `satp_mode`           | `init.c:49`       | 启动默认 `SATP_MODE_57`，被 `set_satp_mode` 调整 |
| `pgtable_l5_enabled`  | `init.c:56`       | 启动默认 `true`              |
| `pgtable_l4_enabled`  | `init.c:57`       | 启动默认 `true`              |

> 对 Sv39：`disable_pgtable_l5()` + `disable_pgtable_l4()` 会把这三个量分别置为 `SATP_MODE_39 / false / false`，从此 `PGDIR_SHIFT/PMD_SHIFT/...` 全部走 L3 分支。

---

## 9. 一些常见疑问（Sv39 视角）

1. **trampoline_pg_dir 是 identity mapping 吗？为什么需要它？**
   - **不是** identity mapping。它把 `VA = kernel_map.virt_addr ≈ 0xffffffff80000000` 映射到 `PA = kernel_map.phys_addr`（如 `0x80200000`），VA ≠ PA。
   - Linux RISC-V 启动**不使用** identity mapping 那一招，而是用 §4.2 的 **stvec trick**："写 satp 必然触发 instruction fault → 跳到 stvec 预设的虚地址" 来完成从 PA 到 VA 的切换。
   - 需要 trampoline 而不直接写 early_pg_dir 的原因（`head.S:95-120` 注释）：
     (a) trampoline 是最小集（1 个 PGD 槽 + 1 个 PMD 大页），第一次切换不依赖 early_pg_dir 中其它任何位置，最稳；
     (b) 两次切换中间的 `sfence.vma` 让 TLB 状态干净，避免旧映射污染新表；
     (c) secondary CPU 启动也复用同一套机制（`head.S:166-171`，最终目标 swapper_pg_dir），调用 `relocate_enable_mmu` 一致。

2. **`early_pg_dir` 跟 `swapper_pg_dir` 的最大差别？**
   - `early_pg_dir`：只覆盖内核镜像（大页 RWX）+ fixmap 子树（仅 PGD 项+ FDT 大页）。
   - `swapper_pg_dir`：所有线性映射 + 内核镜像（按 RWX 区分）+ fixmap 子树（共享同一 fixmap_pmd）+ KASAN（如果开）+ 后续 vmalloc/vmemmap 的中间表。

3. **fixmap 是怎么实现"提前把 FDT 放进来"的？**
   - 在 setup_vm 阶段直接把 FDT 物理地址用 2 个 2MB 大页**写进 `fixmap_pmd`**，所以无论是 early_pg_dir 还是后来的 swapper_pg_dir，只要 PGD 中 `FIXADDR_START` 槽指向 `fixmap_pmd`，FDT 就一直可见。

4. **`swapper_pg_dir` 是何时初始化的？**
   - 静态在 BSS（清 0）；`setup_vm_final` 才开始往里写表项。在切换 satp 到它之前**不会有任何使用者**。

5. **三次写 satp 之后 TLB 怎么处理？**
   - ① ② 写 satp 前 ② 之间各有 `sfence.vma`/`local_flush_tlb_all()`（详见 `head.S:105,123`）。
   - ③ 之后 `local_flush_tlb_all()`（`init.c:1382`）。

---

## 10. 后续可视化建议

建议基于本文件做以下图：

- **时序泳道图**：横轴时间，纵轴三个泳道 (HW satp / pt_ops / 关键代码位置)，标注每个 milestone 的 PA/VA 状态。
- **页表树状图**：分别画 `trampoline_pg_dir`、`early_pg_dir`、`swapper_pg_dir` 三棵树的 PGD/PMD/PTE 链接关系，叶子项写明 PA→VA 与 RWX 属性。
- **VA 空间地图叠加**：以 vm-layout.rst 的 Sv39 表为底图，逐阶段染色"现在哪段 VA 已经可达"。
- **`pt_ops` 状态机**：三态切换图（early / fixmap / late），节点上标 alloc/get 用什么物理来源、用什么虚拟视图。

---

## 11. 参考

- Linux 6.18 源码（本仓库），重点：`arch/riscv/kernel/head.S`、`arch/riscv/mm/init.c`、`arch/riscv/include/asm/{page,pgtable,csr,fixmap,pgtable-64}.h`
- `Documentation/arch/riscv/vm-layout.rst`
- RISC-V Privileged Architecture Spec §4.4 (Sv39/Sv48/Sv57)
- OpenSBI 文档（关于 hart 启动握手，与本文衔接的部分）
