// ===========================================================================
//  ACPI 模块 —— 只实现「真关机」所需的最小子集
//  ===========================================================================
//  链路：
//    RSDP("RSD PTR ") → RSDT/XSDT → FADT("FACP")
//      → 取 PM1a_CNT / PM1b_CNT 端口
//      → 从 DSDT 的 \_S5 包里解析 SLP_TYPa / SLP_TYPb
//      → outw(PM1a_CNT, SLP_TYPx << 10 | SLP_EN)
//
//  为什么要它：
//    以前的 shutdown 只是打印"可以关闭电源了"然后 hlt，
//    虚拟机和真机都**不断电**，还得手动关。
//
//  【微内核分工】
//    机制（写端口断电）在内核 —— 端口 I/O 是特权指令，用户态执行会 #GP。
//    策略（什么时候关机、要不要先提示用户）在用户态电源服务。
// ===========================================================================
#include <kernel/acpi.hpp>
#include <kernel/io.h>
#include <kernel/printf.hpp>
#include <kernel/vmm.hpp>
#include <kernel/log.hpp>

namespace acpi {

// ---------------------------------------------------------------------------
//  表结构（只需要我们真正会读的那几个字段）
// ---------------------------------------------------------------------------
struct [[gnu::packed]] Rsdp {
    char     signature[8];
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_addr;      // ACPI 1.0：32 位 RSDT 物理地址
    // ACPI 2.0 起后面还有 length / xsdt_addr(64) / ext_checksum / reserved，
    // 但我们优先用 RSDT（32 位），不需要读那么远。
};

struct [[gnu::packed]] SdtHeader {
    char     signature[4];
    u32      length;
    u8       revision;
    u8       checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    u32      oem_revision;
    u32      creator_id;
    u32      creator_revision;
};

// ---------------------------------------------------------------------------
//  高半区偏移：物理地址 + 这个偏移 = 内核虚拟地址
//    阶段 1 建了 0~4GB 恒等映射（在低半），同时又把内核映射到 -2GB 处。
//    内核代码里访问物理内存一律走高半区。
// ---------------------------------------------------------------------------
static constexpr u64 HH = 0xFFFFFFFF80000000ull;

// 找到的关机信息
static bool  g_available    = false;
static u32   g_pm1a_cnt     = 0;
static u32   g_pm1b_cnt     = 0;
static u32   g_slp_typa     = 0;
static u32   g_slp_typb     = 0;
static bool  g_s5_found     = false;

// ---------------------------------------------------------------------------
//  探测过程留痕，供"关机失败"时上屏诊断
//  -------------------------------------------------------------------------
//  用户在 VirtualBox 上**看不到串口**，只能看屏幕。
//  以前 ACPI 失败只在串口打一条日志，用户那边完全没信息，
//  只能反馈一句"[ACPI] 不可用"，无法定位。
//  现在把每一步的结果记下来，shutdown 失败时直接打到屏幕上。
// ---------------------------------------------------------------------------
static bool  g_d_rsdp = false;
static bool  g_d_rsdt = false;
static bool  g_d_xsdt = false;
static bool  g_d_fadt = false;
static bool  g_d_scan = false;      // 是否靠暴力扫描找到的 FADT
static bool  g_d_s5   = false;
static u32   g_d_pm1a = 0;
static u64   g_d_rsdt_phys = 0;    // 诊断：RSDT/XSDT 实际物理地址
static u64   g_d_xsdt_phys = 0;

// SLP_EN：写 1 到 bit13 触发休眠/关机状态转换
static constexpr u16 SLP_EN = 1 << 13;

// ---------------------------------------------------------------------------
//  校验和：ACPI 表的 checksum 字段要满足"整个表按字节相加 == 0"
// ---------------------------------------------------------------------------
static bool checksum_ok(const void* p, u64 len)
{
    const u8* b = reinterpret_cast<const u8*>(p);
    u8 sum = 0;
    for (u64 i = 0; i < len; ++i) sum += b[i];
    return sum == 0;
}

static bool sig_eq(const char* a, const char* b, int n)
{
    for (int i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------
//  扫描 RSDP
//  -------------------------------------------------------------------------
//  RSDP 只可能出现在这两处（都在 1MB 以下）：
//    1) EBDA 的前 1KB（EBDA 段地址在 0x40E）
//    2) 0xE0000 ~ 0xFFFFF
//  且必须 16 字节对齐。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  确保一段物理内存在高半区**可访问**
//  -------------------------------------------------------------------------
//  ACPI 表由固件放在物理内存里，位置由 BIOS 决定，
//  可能落在内核页表尚未映射的区域（VirtualBox 上尤其常见）。
//
//  原来直接 (HH + phys) 解引用，若该虚拟地址没映射就会 #PF；
//  即使不崩，读到的也可能是垃圾 —— 表现为"未找到 FADT"。
//
//  现在：访问前先检查，缺了就补映射。
// ---------------------------------------------------------------------------
// ===========================================================================
//  【致命 bug 修复】ACPI 访问不能再写 (HH + phys)！
//  ---------------------------------------------------------------------------
//  HH = 0xFFFFFFFF80000000，以前的写法一律是 `HH + phys` 解引用。
//  这在 **phys >= 2GB 时会发生 64 位溢出回绕**：
//      phys=0x7FFE0000 → 0xFFFFFFFFFFFE0000   ok
//      phys=0xDFEE0000 → 0x000000005FEE0000   回绕到低地址！
//
//  物理内存 4GB 以上的机器（用户实测 **4810 MB**），
//  VirtualBox 把 ACPI 表放在**高端内存**（接近 4GB 处），
//  于是所有表访问都踩到回绕后的错误地址：
//      RSDP 能找到（在 BIOS 低位区，不溢出）
//      RSDT/XSDT 的**地址值**也读到了（只是个数字）
//      但顺着地址去遍历表时全部落空 → **FADT=未找到**
//
//  这正是用户截图里那三行诊断的成因。
//
//  修法：改用**独立的映射窗口** —— 把物理页映射到固定内核虚拟地址，
//  用窗口地址访问，与 phys 大小无关，永远不会溢出。
// ===========================================================================
static constexpr u64 ACPI_WIN       = 0xFFFFFFFFC0000000ull;
static constexpr u64 ACPI_WIN_PAGES = 256;                  // 1 MB 窗口

static bool ensure_mapped(u64 phys, u64 len)
{
    if (phys == 0 || len == 0) return false;

    const u64 PAGE = 0x1000ull;
    u64 start = phys & ~(PAGE - 1);
    u64 end   = (phys + len + PAGE - 1) & ~(PAGE - 1);
    if (end <= start) return false;

    u64 pages = (end - start) / PAGE;
    if (pages > ACPI_WIN_PAGES) pages = ACPI_WIN_PAGES;

    for (u64 i = 0; i < pages; ++i) {
        u64 pp = start + i * PAGE;
        u64 vv = ACPI_WIN + i * PAGE;
        // 强制重映射：窗口可能还指向上一次用过的物理页。
        // 先 unmap（内部会 flush TLB）再 map，保证读到的是新数据。
        // ACPI 只在启动时访问，这点开销完全无所谓。
        if (vmm::is_mapped(vv)) vmm::unmap_page(vv);
        if (!vmm::map_page(vv, pp, vmm::FLAGS_KERNEL)) return false;
    }
    return true;
}

// 取 phys 处数据的窗口指针。
//
// ⚠️ 窗口是**共享**的：返回的指针只在**下一次 fetch 之前**有效。
//    所以调用方要么立刻用完，要么先把需要的数据复制到局部变量。
static const u8* fetch(u64 phys, u64 len)
{
    if (!ensure_mapped(phys, len)) return nullptr;
    return reinterpret_cast<const u8*>(ACPI_WIN + (phys & 0xFFFull));
}

// RSDP v2 起，偏移 24 处是 XSDT 的 64 位物理地址
static u64 rsdp_xsdt(const Rsdp* rsdp)
{
    if (rsdp->revision < 2) return 0;
    u64 v = 0;
    const u8* b = reinterpret_cast<const u8*>(rsdp);
    __builtin_memcpy(&v, b + 24, 8);
    return v;
}

static const Rsdp* find_rsdp()
{
    // --- 先试 EBDA ---
    const volatile u16* bda =
        reinterpret_cast<const volatile u16*>(fetch(0x40E, 2));
    if (bda == nullptr) return nullptr;
    u16 ebda_seg = *bda;
    u64 ebda_phys = static_cast<u64>(ebda_seg) << 4;

    auto scan = [](u64 base, u64 len) -> const Rsdp* {
        // ⚠️ 访问前必须确保已映射。
        //  ACPI 表位置由 BIOS 决定，可能落在内核页表未映射的区域。
        //  原来直接 deref，VirtualBox 上可能读到垃圾甚至 #PF。
        if (!ensure_mapped(base, len)) return nullptr;

        const u8* win = reinterpret_cast<const u8*>(ACPI_WIN + (base & 0xFFFull));
        for (u64 off = 0; off < len; off += 16) {
            const Rsdp* r = reinterpret_cast<const Rsdp*>(win + off);
            if (sig_eq(r->signature, "RSD PTR ", 8)) {
                return r;
            }
        }
        return nullptr;
    };

    if (ebda_phys != 0 && ebda_phys < 0x100000) {
        if (const Rsdp* r = scan(ebda_phys, 1024)) return r;
    }
    return scan(0xE0000, 0x20000);
}

// ---------------------------------------------------------------------------
//  在 RSDT 里按签名找表
// ---------------------------------------------------------------------------
static const SdtHeader* find_table(u64 rsdt_phys, u64 xsdt_phys, const char* sig)
{
    // ⚠️ 本函数全程用共享窗口 fetch，
    //    必须遵守"先把需要的数复制出来，再做下一次 fetch"的规则。

    // --- 先查 RSDT（32 位条目） ---
    if (rsdt_phys != 0 && rsdt_phys < 0x100000000ull) {
        const SdtHeader* rsdt = reinterpret_cast<const SdtHeader*>(
            fetch(rsdt_phys, sizeof(SdtHeader)));

        if (rsdt != nullptr
            && sig_eq(rsdt->signature, "RSDT", 4)
            && rsdt->length >= sizeof(SdtHeader) && rsdt->length < 0x100000) {

            u32 rlen = rsdt->length;
            rsdt = reinterpret_cast<const SdtHeader*>(fetch(rsdt_phys, rlen));

            if (rsdt != nullptr) {
                u32 count = (rlen - sizeof(SdtHeader)) / 4;
                if (count > 64) count = 64;
                const u32* entries = reinterpret_cast<const u32*>(
                    reinterpret_cast<const u8*>(rsdt) + sizeof(SdtHeader));

                // 先把条目地址复制出来 —— 下一次 fetch 会覆盖窗口
                u64 ents[64];
                u32 n = 0;
                for (u32 i = 0; i < count; ++i) {
                    u64 p = entries[i];
                    if (p == 0 || p >= 0x100000000ull) continue;
                    ents[n++] = p;
                }

                for (u32 i = 0; i < n; ++i) {
                    const SdtHeader* h = reinterpret_cast<const SdtHeader*>(
                        fetch(ents[i], sizeof(SdtHeader)));
                    if (h == nullptr) continue;
                    if (h->length < sizeof(SdtHeader) || h->length > 0x100000) continue;
                    if (sig_eq(h->signature, sig, 4)) {
                        return reinterpret_cast<const SdtHeader*>(fetch(ents[i], h->length));
                    }
                }
            }
        }
    }

    // --- RSDT 没找到：再查 XSDT（64 位条目） ---
    //    VirtualBox 等现代固件主要用 XSDT。
    if (xsdt_phys != 0 && xsdt_phys < 0x100000000ull) {
        const SdtHeader* xsdt = reinterpret_cast<const SdtHeader*>(
            fetch(xsdt_phys, sizeof(SdtHeader)));

        if (xsdt != nullptr
            && sig_eq(xsdt->signature, "XSDT", 4)
            && xsdt->length >= sizeof(SdtHeader) + 8 && xsdt->length < 0x100000) {

            u32 xlen = xsdt->length;
            xsdt = reinterpret_cast<const SdtHeader*>(fetch(xsdt_phys, xlen));

            if (xsdt != nullptr) {
                u32 count = (xlen - sizeof(SdtHeader)) / 8;
                if (count > 64) count = 64;
                const u64* entries = reinterpret_cast<const u64*>(
                    reinterpret_cast<const u8*>(xsdt) + sizeof(SdtHeader));

                u64 ents[64];
                u32 n = 0;
                for (u32 i = 0; i < count; ++i) {
                    u64 p = entries[i];
                    if (p == 0 || p >= 0x100000000ull) continue;
                    ents[n++] = p;
                }

                for (u32 i = 0; i < n; ++i) {
                    const SdtHeader* h = reinterpret_cast<const SdtHeader*>(
                        fetch(ents[i], sizeof(SdtHeader)));
                    if (h == nullptr) continue;
                    if (h->length < sizeof(SdtHeader) || h->length > 0x100000) continue;
                    if (sig_eq(h->signature, sig, 4)) {
                        return reinterpret_cast<const SdtHeader*>(fetch(ents[i], h->length));
                    }
                }
            }
        }
    }

    return nullptr;
}

static const SdtHeader* scan_facp()
{
    const u64 SCAN_START = 0xE0000ull;
    const u64 SCAN_END   = 0x100000ull;

    // 整段一次映射进窗口（128 KB，窗口够用），再逐 16 字节扫签名
    if (!ensure_mapped(SCAN_START, SCAN_END - SCAN_START)) return nullptr;
    const u8* win = reinterpret_cast<const u8*>(ACPI_WIN + (SCAN_START & 0xFFFull));

    for (u64 off = 0; off < SCAN_END - SCAN_START; off += 16) {
        const SdtHeader* h = reinterpret_cast<const SdtHeader*>(win + off);
        if (!sig_eq(h->signature, "FACP", 4)) continue;
        if (h->length < 64 + 8 || h->length > 0x100000) continue;
        return h;
    }
    return nullptr;
}

static bool parse_s5(const SdtHeader* dsdt)
{
    if (dsdt == nullptr) return false;

    const u8* p = reinterpret_cast<const u8*>(dsdt);
    u32 len = dsdt->length;
    if (len < sizeof(SdtHeader)) return false;

    for (u32 i = 1; i + 4 < len; ++i) {
        // 找 "_S5_"，且**前一个字节必须是 NameOp (0x08)**。
        //
        //   AML 里 \_S5 的定义形如：
        //     0x08(NameOp) '_S5_' 0x12(PackageOp) ...
        //   DSDT 里还可能出现别的 "_S5_" 字符串（比如在注释或别的 scope 中），
        //   不加这个约束就会误匹配 —— 实测就解析出了 SLP_TYP=0 这种无效值。
        if (!(p[i] == '_' && p[i + 1] == 'S' && p[i + 2] == '5'
              && p[i + 3] == '_')) {
            continue;
        }
        // 【bug 修复】原来只接受 0x08，漏掉了带根前缀的写法。
        //   AML 里定义在根作用域的名字，编码是：
        //     08(NameOp) 5C(RootChar) '_S5_' 12(PackageOp) ...
        //   即 "_S5_" 的前一字节是 0x5C（反斜杠），不是 0x08。
        //   硬要求 0x08 会把这种正确写法全跳过 ——
        //   这正是 S5 一直解析失败、只能回退硬编码的原因。
        u8 prev = p[i - 1];
        if (prev != 0x08 && prev != 0x5C) continue;
        // 若是 0x5C，要求再前一字节是 NameOp，避免误匹配
        if (prev == 0x5C && i >= 2 && p[i - 2] != 0x08) continue;

        // 往后 32 字节内找 PackageOp (0x12)
        for (u32 j = i + 4; j < i + 36 && j < len; ++j) {
            if (p[j] != 0x12) continue;

            // 0x12 后面是 PkgLength（1~4 字节，高 2 位是"还有几字节"）
            u32 k = j + 1;
            if (k >= len) break;

            u8 lead = p[k];
            u32 extra = lead >> 6;      // 0~3，表示后续还有几个字节
            k += 1 + extra;             // 跳过 PkgLength 本身

            // 下一个字节应该是 NumElements
            if (k >= len) break;
            u32 numelem = p[k];
            (void)numelem;
            ++k;

            // 取第一个元素（SLP_TYPa）
            if (k >= len) break;
            u32 typa;
            if (p[k] == 0x0A && k + 1 < len) {        // BytePrefix: 1 字节
                typa = p[k + 1];
                k += 2;
            } else if (p[k] == 0x0B && k + 2 < len) { // WordPrefix: 2 字节
                typa = static_cast<u32>(p[k + 1]) | (static_cast<u32>(p[k + 2]) << 8);
                k += 3;
            } else if (p[k] == 0x0C && k + 4 < len) { // DWordPrefix: 4 字节
                typa = static_cast<u32>(p[k + 1]) | (static_cast<u32>(p[k + 2]) << 8)
                     | (static_cast<u32>(p[k + 3]) << 16) | (static_cast<u32>(p[k + 4]) << 24);
                k += 5;
            } else if (p[k] <= 0x09) {                // ZeroOp..NineOp
                typa = p[k];
                k += 1;
            } else {
                continue;   // 不认识，换个位置再找
            }

            // 取第二个元素（SLP_TYPb）
            u32 typb = typa;                          // 没有就复用 typa
            if (k < len) {
                if (p[k] == 0x0A && k + 1 < len) {
                    typb = p[k + 1];
                } else if (p[k] == 0x0B && k + 2 < len) {
                    typb = static_cast<u32>(p[k + 1]) | (static_cast<u32>(p[k + 2]) << 8);
                } else if (p[k] == 0x0C && k + 4 < len) {
                    typb = static_cast<u32>(p[k + 1]) | (static_cast<u32>(p[k + 2]) << 8)
                         | (static_cast<u32>(p[k + 3]) << 16) | (static_cast<u32>(p[k + 4]) << 24);
                } else if (p[k] <= 0x09) {
                    typb = p[k];
                }
            }

            // ---------------------------------------------------------
            //  【重大 bug 修复】原来这里拒绝 typa==0，理由是想防误匹配。
            //  但 **QEMU 的 \_S5 就是 {0x00, 0x00}**！
            //  （QEMU 的 acpi_pm1_cnt_write 里 sus_typ==0 才触发关机）
            //  于是每次都"解析失败"→ 回退硬编码 5 → QEMU 根本不关机，
            //  表现为"已写端口但机器未断电"。
            //
            //  0 是完全合法的休眠类型，不能因为它看起来像"没解析出来"
            //  就拒绝。误匹配已经由 NameOp/PackageOp 的形状约束挡住了。
            // ---------------------------------------------------------

            g_slp_typa = typa;
            g_slp_typb = typb;
            g_s5_found = true;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
//  init：定位所有关机需要的信息
// ---------------------------------------------------------------------------
bool init()
{
    const Rsdp* rsdp = find_rsdp();
    if (rsdp == nullptr) {
        // ⚠️ 不能就此放弃！
        //  FADT 本身就在 BIOS 区域（0xE0000~0x100000），
        //  可以**直接暴力搜签名**找到，不依赖 RSDP 索引。
        //  原来这里直接 return false，等于放弃了最后一条路。
        kprintf_serial("[ACPI] 未找到 RSDP，改用暴力扫描 FACP\n");
    } else {
        g_d_rsdp = true;
    }

    // RSDP v0 的 checksum 覆盖前 20 字节
    if (!checksum_ok(rsdp, 20)) {
        kprintf_serial("[ACPI] RSDP 校验失败，继续尝试\n");
    }

    u64 rsdt_phys = (rsdp != nullptr) ? rsdp->rsdt_addr : 0;
    u64 xsdt_phys = (rsdp != nullptr) ? rsdp_xsdt(rsdp) : 0;
    g_d_rsdt = (rsdt_phys != 0 && rsdt_phys < 0x100000000ull);
    g_d_xsdt = (xsdt_phys != 0 && xsdt_phys < 0x100000000ull);
    g_d_rsdt_phys = rsdt_phys;
    g_d_xsdt_phys = xsdt_phys;

    // 至少要有 RSDT 或 XSDT 其中一个
    if ((rsdt_phys == 0 || rsdt_phys >= 0x100000000ull)
        && (xsdt_phys == 0 || xsdt_phys >= 0x100000000ull)) {
        // 同样不放弃 —— 后面还有暴力扫描兜底
        kprintf_serial("[ACPI] RSDT/XSDT 地址均无效：rsdt=0x%llx xsdt=0x%llx\n",
                       rsdt_phys, xsdt_phys);
    }

    // --- 找 FADT（RSDT 优先，回退 XSDT） ---
    const SdtHeader* fadt = find_table(rsdt_phys, xsdt_phys, "FACP");
    if (fadt == nullptr) {
        // 索引表路线失败 → 暴力扫描兜底
        kprintf_serial("[ACPI] RSDT/XSDT 均未索引到 FACP，改用暴力扫描\n");
        fadt = scan_facp();
        g_d_scan = (fadt != nullptr);
    }
    g_d_fadt = (fadt != nullptr);
    if (fadt == nullptr) {
        kprintf_serial("[ACPI] 未找到 FADT（RSDT=0x%llx XSDT=0x%llx，暴力扫描也无）\n",
                       rsdt_phys, xsdt_phys);
        return false;
    }

    // FADT 偏移（ACPI 1.0/2.0 前半部分布局一致）：
    //   +64  PM1a_CNT_BLK (u32)
    //   +68  PM1b_CNT_BLK (u32)
    //   +40  DSDT         (u32)
    const u8* f = reinterpret_cast<const u8*>(fadt);

    g_pm1a_cnt = *reinterpret_cast<const u32*>(f + 64);
    g_pm1b_cnt = *reinterpret_cast<const u32*>(f + 68);
    g_d_pm1a = g_pm1a_cnt;

    // --- 找 DSDT 并解析 S5 ---
    u32 dsdt_phys = *reinterpret_cast<const u32*>(f + 40);
    bool s5_ok = false;

    if (dsdt_phys != 0 && dsdt_phys < 0x100000000ull) {
        const SdtHeader* dsdt = reinterpret_cast<const SdtHeader*>(
            fetch(dsdt_phys, sizeof(SdtHeader)));
        if (dsdt != nullptr
            && sig_eq(dsdt->signature, "DSDT", 4)
            && dsdt->length >= sizeof(SdtHeader) && dsdt->length < 0x100000) {
            u32 dlen = dsdt->length;
            // 整表重新映射后再解析（窗口可能被上一次 fetch 覆盖）
            dsdt = reinterpret_cast<const SdtHeader*>(fetch(dsdt_phys, dlen));
            if (dsdt != nullptr) s5_ok = parse_s5(dsdt);
        }
    }

    // --- 回退：候选值列表 ---
    //  解析 AML 失败时不能就此放弃关机。
    //  各家固件的 SLP_TYP 不同（QEMU=0，多数真机/ VirtualBox=5，也有 3），
    //  所以准备一串候选，shutdown() 会依次尝试。
    if (!s5_ok) {
        g_slp_typa = 0xFFFFFFFFu;      // 表示"未解析，走候选列表"
        g_slp_typb = 0xFFFFFFFFu;
        kprintf_serial("[ACPI] S5 解析失败，关机时依次尝试候选 SLP_TYP\n");
    }

    if (g_pm1a_cnt == 0) {
        kprintf_serial("[ACPI] PM1a_CNT 端口为 0，关机将回退到 halt\n");
        return false;
    }

    g_available = true;
    kprintf_serial("[ACPI] 就绪 PM1a_CNT=0x%x PM1b_CNT=0x%x SLP_TYPa=0x%x SLP_TYPb=0x%x%s\n",
                   g_pm1a_cnt, g_pm1b_cnt, g_slp_typa, g_slp_typb,
                   s5_ok ? "" : " (未解析,走候选)");
    return true;
}

bool available()
{
    return g_available;
}

// ---------------------------------------------------------------------------
//  shutdown：写 PM1x_CNT 触发 S5（软关机）状态
//  -------------------------------------------------------------------------
//  写入格式：bit10~12 = SLP_TYP，bit13 = SLP_EN
//  先写 PM1b 再写 PM1a（顺序不影响，但都写更保险）。
// ---------------------------------------------------------------------------
bool shutdown()
{
    if (!g_available) return false;

    // 关中断，避免断电过程中被打断
    asm volatile("cli");

    LOG_ACPI2(LogOp::AcpiShutdown, g_pm1a_cnt,
              static_cast<u64>(g_slp_typa), g_pm1b_cnt);

    auto try_write = [](u32 typ) {
        u16 val = static_cast<u16>(((typ & 7) << 10) | SLP_EN);
        if (g_pm1b_cnt != 0) {
            outw(static_cast<u16>(g_pm1b_cnt), val);
        }
        outw(static_cast<u16>(g_pm1a_cnt), val);
        // 给硬件一点反应时间。
        // 不能太长：要试好几组值，每组都长等会让关机明显变慢。
        for (volatile int i = 0; i < 200000; ++i) { }
    };

    // ---------------------------------------------------------------
    //  【重要修复】无条件遍历**所有**候选值
    // ---------------------------------------------------------------
    //  以前：S5 解析成功就**只试解析出来的那一个值**，不再试其他；
    //        只有解析失败才走候选列表。
    //
    //  问题：AML 是复杂的字节码，我们只做了简化解析，
    //  很可能"解析成功"但拿到的是**错的值**（误匹配到别的对象）。
    //  此时就抱着一个错误的值写端口，再也不试别的 —— 必然失败。
    //  用户 VirtualBox 实测"没断电"，很可能就是这么来的。
    //
    //  现在：解析值优先试，之后把 0/5/3 都试一遍（去重）。
    //  关机会执行一次，多试几组值没有性能代价，但成功率大得多。
    // ---------------------------------------------------------------
    u32 list[8];
    int n = 0;

    if (g_slp_typa != 0xFFFFFFFFu && g_slp_typa <= 7) {
        list[n++] = g_slp_typa;
    }
    static constexpr u32 candidates[] = { 5, 0, 3, 7, 1 };
    for (int i = 0; i < static_cast<int>(sizeof(candidates) / sizeof(candidates[0])); ++i) {
        bool dup = false;
        for (int j = 0; j < n; ++j) {
            if (list[j] == candidates[i]) { dup = true; break; }
        }
        if (!dup && n < 8) list[n++] = candidates[i];
    }

    for (int i = 0; i < n; ++i) {
        try_write(list[i]);
    }

    // 正常不会走到这里 —— 已经断电了。
    // 走到这说明没成功，交给调用方（停机）。
    return true;
}

// 把探测过程打到屏幕（用户可见），用于"为什么 ACPI 没断成电"的现场诊断
void report_diag()
{
    kprintf("  [ACPI 诊断] RSDP=%s RSDT=%s XSDT=%s\n",
            g_d_rsdp ? "找到" : "未找到",
            g_d_rsdt ? "有" : "无",
            g_d_xsdt ? "有" : "无");
    kprintf("  [ACPI 诊断] FADT=%s%s\n",
            g_d_fadt ? "找到" : "未找到",
            g_d_scan ? "（靠暴力扫描）" : "");
    kprintf("  [ACPI 诊断] PM1a_CNT=0x%x  S5=%s\n",
            g_d_pm1a, g_d_s5 ? "解析成功" : "解析失败");
    kprintf("  [ACPI 诊断] RSDT地址=0x%x XSDT地址=0x%x\n",
            static_cast<u32>(g_d_rsdt_phys & 0xFFFFFFFFull),
            static_cast<u32>(g_d_xsdt_phys & 0xFFFFFFFFull));
}

u32 pm1a_cnt_port() { return g_pm1a_cnt; }
u32 slp_typa()      { return g_slp_typa; }
u32 slp_typb()      { return g_slp_typb; }

}   // namespace acpi
