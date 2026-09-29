// ===========================================================================
//  TaskbarProgress.cpp —— Windows 任务栏按钮集成的实现
//
//  本文件实现 TaskbarProgress 的全部行为，逻辑上分三层：
//    1) 会话层：COM 的初始化/反初始化，以及 ITaskbarList3 的创建与 HrInit 握手；
//    2) 绑定层：把某个 QWidget 的原生窗口句柄（HWND，Handle of a WiNDow，窗口句柄）
//               登记为后续调用的作用目标；
//    3) 呈现层：进度条（TBPF_* 状态 + 数值）与角标（离屏绘制 → HICON → 覆盖图标）。
//
//  平台策略：凡是 Windows 专属实现都包在 Q_OS_WIN 内；非 Windows 平台保留同一组
//  函数签名的退化实现（返回 false 或 nullptr），调用方因此不必书写平台判断代码。
//
//  资源纪律：COM 接口、HICON、HBITMAP、HDC 四类系统句柄在本文件中都有唯一且明确的
//  释放点；反复刷新不会累积句柄，也不会泄漏 COM 引用计数——这是该类能够被每秒级
//  刷新循环安全调用的前提。
//
//  失败哲学：任何一步失败都不抛异常、不中断调用方，只把结果记录到
//  m_progressApplied / m_badgeApplied / m_badgeIconCreated / m_badgeHresult
//  四个探针字段上，供 `--probe-ui` 自检把「功能没生效」与「系统不支持」区分开。
// ===========================================================================

// 先包含自身头文件：TaskbarProgress.h 中前置声明了 ITaskbarList3 与 HWND/HICON，
// 把该声明放在系统头文件之前，可避免后续 windows.h 展开时出现重复定义冲突。
#include "TaskbarProgress.h"

// QFont：设置角标文字的字号（setPixelSize）与粗体。
#include <QFont>
// QFontMetrics：文字度量能力；当前实现按字符数估算字号，保留该包含以便后续做精确排版。
#include <QFontMetrics>
// QImage：把 QPixmap 转成可逐字节访问的 32 位 ARGB 缓冲，供 DIB 整体拷贝使用。
#include <QImage>
// QPainter：在离屏 QPixmap 上绘制圆形底色与居中文字。
#include <QPainter>
// QPixmap：角标画布（离屏绘制目标，稍后整体转成 HICON）。
#include <QPixmap>
// QVarLengthArray：读取令牌完整性级别时的栈上小缓冲，避免为一次探测做堆分配。
#include <QVarLengthArray>
// QWidget：调用 winId() 取原生窗口句柄需要完整类型定义，此处不能用前置声明替代。
#include <QWidget>

#ifdef Q_OS_WIN
// 只保留精简版 windows.h：不拖入 winsock 等无关头，缩短编译时间并降低符号冲突概率。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
// windows.h：HWND/HICON/HBITMAP/HDC、BITMAPV5HEADER、DIBSECTION 与图标相关 API；
// objbase.h：CoInitializeEx / CoCreateInstance / CoUninitialize / HRESULT 判定宏；
// shobjidl.h：ITaskbarList3 接口声明、TBPF_* 状态常量与 CLSCTX_INPROC_SERVER。
#  include <windows.h>
#  include <objbase.h>
#  include <shobjidl.h>
#endif

// 匿名命名空间：以下常量仅在本翻译单元可见，避免污染全局链接符号、防止与其它
// 同样手写 GUID 的模块发生重名冲突。
namespace {

#ifdef Q_OS_WIN
// 任务栏对象的 CLSID 与 ITaskbarList3 的 IID。
// 这里手写常量而不用 <shobjidl.h> 里的符号，是为了避开 MinGW 下 GUID 符号
// 需要额外链接 uuid 库、且不同版本头文件导出名不一致的问题。
//
// CLSID（Class IDentifier，类标识符）标识"要创建哪个 COM 组件"，
// IID（Interface IDentifier，接口标识符）标识"要拿该组件的哪个接口"；
// 两者都是 128 位值，此处拆成「4+2+2 字节整数 + 8 字节数组」的聚合初始化形式，
// 与 GUID 结构体的内存布局逐字段对应，因此无需任何运行时转换。
// 采用 const 常量而非宏，可获得类型检查并避免宏污染。
//
// clsidTaskbarList：{56FDF344-FD6D-11D0-958A-006097C9A090}
// 对应系统的"任务栏列表"组件，由 explorer.exe 的进程内服务器实现。
const GUID clsidTaskbarList = {0x56FDF344, 0xFD6D, 0x11D0,
                               {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
// iidTaskbarList3：{EA1AFB91-9E28-4B86-90E9-9E9F8A5EEFAF}
// 即 ITaskbarList3，Windows 7 引入，额外提供 SetProgressState/SetProgressValue/
// SetOverlayIcon；更早的 ITaskbarList/ITaskbarList2 不具备这些方法。
const GUID iidTaskbarList3 = {0xEA1AFB91, 0x9E28, 0x4B86,
                              {0x90, 0xE9, 0x9E, 0x9F, 0x8A, 0x5E, 0xEF, 0xAF}};
#endif

// 角标画布边长。Windows 在 100% 缩放下把角标显示为 16×16，
// 这里用 32×32 画再交给系统缩放，高 DPI 下会明显更清晰。
// 说明：画布越大，系统在低 DPI 下的缩小滤波越平滑，但也会增加每次刷新的
// 位图开销，因此取 2 倍基准尺寸作为清晰度与开销的折中。
constexpr int badgeCanvasSize = 32;

} // namespace

// ---------------------------------------------------------------------------
//  构造 / 析构
// ---------------------------------------------------------------------------

/**
 * @brief 构造对象；仅初始化成员默认值，不做任何可能失败的系统调用。
 *
 * 该构造函数被显式声明为 = default，因此成员初始化完全依赖头文件中的类内初始值
 * （m_taskbar = nullptr、各 bool 探针 = false 等）。之所以不在构造阶段调用
 * CoInitializeEx 或 CoCreateInstance：
 *   · 构造可能发生在没有桌面会话的环境（服务、CI、会话 0），此时任何 COM 调用
 *     都注定失败，构造函数却没有返回值可以表达失败；
 *   · 把会失败的动作推迟到 attachTo()/ensureTaskbar()，语义上更清晰，也让本类
 *     可以被自由地当作成员变量构造。
 *
 * @return 无。
 * @note 构造完成后 isAttached() 必为 false；要生效必须先 attachTo()。
 */
TaskbarProgress::TaskbarProgress() = default;

/**
 * @brief 析构：清除任务栏状态、释放角标 HICON、释放 COM 接口并配平 COM 初始化。
 *
 * 释放顺序有严格要求，共三步：
 *   1) 先调用 clearAll()，让任务栏把进度条与角标状态复位为"无"；
 *   2) 再 Release() 任务栏接口；
 *   3) 最后（且仅当 m_comReady 为真时）调用 CoUninitialize()。
 * 第 1 步必须在第 2 步之前：若先释放接口，任务栏仍可能持有 m_badge 句柄，
 * 而我们随后 DestroyIcon 会让任务栏引用一个已失效的图标，造成角标错乱甚至崩溃。
 *
 * @return 无。
 * @note Release() 与 CoUninitialize() 都是引用计数/线程级操作，因此本对象必须在
 *       创建它的那个线程上析构——本类全部方法都要求同一线程（通常为 GUI 线程）。
 * @note 若外部已提前销毁窗口（HWND 失效），clearAll() 内部的 isAttached() 判定
 *       存在局限：此时只保证本对象自身资源被释放，不保证任务栏状态被清干净。
 */
TaskbarProgress::~TaskbarProgress()
{
    // 先清状态再释放句柄：顺序反了会让任务栏继续持有一个已销毁的图标。
    clearAll();
#ifdef Q_OS_WIN
    // Release() 使 ITaskbarList3 的引用计数减一；这是 COM 对象的唯一释放方式，
    // 不能用 delete。置空是为了让对象即使被异常路径再次访问也不会二次释放。
    if (m_taskbar) {
        m_taskbar->Release();
        m_taskbar = nullptr;
    }
    // 只在"确实由本对象初始化过 COM"时才反初始化，避免破坏其它模块的引用计数。
    // m_comReady 仅在 CoInitializeEx 返回 S_OK/S_FALSE 时被置真——这两种返回
    // 都代表本次调用让线程的 COM 引用计数加一，必须由我们减回去。
    if (m_comReady) {
        CoUninitialize();
        m_comReady = false;
    }
#endif
}

// ---------------------------------------------------------------------------
//  能力查询与探测
// ---------------------------------------------------------------------------

/**
 * @brief 报告当前平台/系统是否具备任务栏进度与角标能力。
 *
 * 这是一个"编译期平台判定 + 运行期粗判"的静态函数：Windows 分支直接返回 true，
 * 因为 Qt 6 的 Q_OS_WIN 意味着 Windows 7 及以上（Qt 6 不支持更早系统），而
 * ITaskbarList3 自 Windows 7 起提供。它回答的是"这个二进制是否编入了该能力"，
 * 不回答"当前会话是否真的能用"——后者由 attachTo()/probe() 实际尝试后判定。
 *
 * @return bool，true 表示本平台编译了任务栏集成；非 Windows 平台恒为 false。
 * @note 调用方应先看 isSupported() 再看 isAttached()：前者为假时不必再尝试绑定。
 */
bool TaskbarProgress::isSupported()
{
#ifdef Q_OS_WIN
    return true;   // Windows 7 及以上均提供 ITaskbarList3；实际可用性由 attachTo() 判定
#else
    // 非 Windows 平台没有任务栏按钮概念，直接报告不支持，让调用方走降级分支。
    return false;
#endif
}

/**
 * @brief 查询当前是否已成功连接任务栏（即后续调用是否会真正生效）。
 *
 * 判定条件是两个成员同时非空：m_taskbar 说明 COM 接口已创建并通过 HrInit，
 * m_hwnd 说明目标窗口句柄已登记。两者缺一不可——只有接口没有窗口时，
 * SetProgressValue 没有作用对象；只有窗口没有接口时，根本没有可调用的方法。
 *
 * @return bool，true 表示可以安全调用 setProgress()/clearProgress()/setBadge() 等。
 * @note 本函数为 const 且不做任何系统调用，可在每次刷新时低成本反复查询。
 */
bool TaskbarProgress::isAttached() const
{
    return m_taskbar != nullptr && m_hwnd != nullptr;
}

/**
 * @brief 纯探测：不绑定窗口，仅验证任务栏 COM 对象能否创建并完成 HrInit。
 *
 * 供 `--selftest` / 桌面集成诊断使用。自检流程不需要（也不应该）创建主窗口，
 * 因此无法走 attachTo()；本函数用"临时对象 + ensureTaskbar()"的方式复用完全
 * 相同的创建路径，从而保证探测结论与真实使用结论一致。
 *
 * 关键点是"临时对象"这一写法：TaskbarProgress temporary 在栈上构造，函数返回时
 * 其析构函数自动执行 Release() 与 CoUninitialize()，所以无论成功还是失败，
 * 探测过程都不会给进程留下悬空的接口指针或多加一次的 COM 引用计数。
 * 若改为直接调用 ensureTaskbar() 而不构造对象，就必须在返回前手工写下完整的
 * 清理代码，任何提前 return 都会成为泄漏点。
 *
 * @return bool，true 表示 CoCreateInstance 与 HrInit 都成功。
 * @note 探测只覆盖"COM 侧可用性"，不涉及窗口；E_ACCESSDENIED 之类的呈现期错误
 *       不会在此暴露。
 * @note 非 Windows 平台直接返回 false，不引入任何条件编译以外的分支。
 */
bool TaskbarProgress::probe()
{
#ifdef Q_OS_WIN
    // 用临时对象探测：析构函数会自动 Release 接口并按引用计数反初始化 COM，
    // 因此探测过程不会给进程留下任何残留状态。
    TaskbarProgress temporary;
    return temporary.ensureTaskbar();
#else
    // 无 Windows 任务栏概念，探测结论恒为"不可用"。
    return false;
#endif
}

/**
 * @brief 读取当前进程的完整性级别（integrity level）。
 *
 * 之所以把这个能力放在本类：它是本项目里唯一的 Win32 互操作模块，而托盘与任务栏
 * 都直接受该属性影响——`SetOverlayIcon` 在进程完整性级别低于任务栏时会返回
 * E_ACCESSDENIED。把该值写进自检报告，"角标为什么没生效"就从推断变成事实：
 * `processIntegrity = Low` 配合 `taskbarBadgeHresult = 0x80070005` 即可判定
 * 是环境权限所致，而不是程序缺陷。
 *
 * 实现要点：用 GetCurrentProcessToken() 的伪句柄（值 -4）读取自身令牌，无需额外权限、
 * 也不产生需要关闭的句柄；完整性级别编码在 SID 的最后一个子权威里，其取值就是
 * SECURITY_MANDATORY_*_RID 常量。
 *
 * @return QString，形如 "Medium (0x2000)"；非 Windows 或读取失败时返回 "unknown"。
 * @note 只读操作，不修改任何令牌状态。
 */
QString TaskbarProgress::processIntegrityLevel()
{
#ifdef Q_OS_WIN
    const HANDLE processToken = reinterpret_cast<HANDLE>(static_cast<quintptr>(-4));

    QVarLengthArray<char, 256> buffer(256);
    DWORD length = static_cast<DWORD>(buffer.size());
    auto *label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer.data());

    if (!GetTokenInformation(processToken, TokenIntegrityLevel, label, length, &length)) {
        // 缓冲区不够时接口会回填所需长度，按该长度重试一次；仍失败说明读取受限
        // （极少见，例如被受限令牌拦截），如实返回 unknown 而不是猜一个值。
        buffer.resize(static_cast<int>(length));
        label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer.data());
        if (!GetTokenInformation(processToken, TokenIntegrityLevel, label, length, &length))
            return QStringLiteral("unknown");
    }

    const UCHAR subAuthorityCount = *GetSidSubAuthorityCount(label->Label.Sid);
    const DWORD rid = *GetSidSubAuthority(label->Label.Sid,
                                          static_cast<DWORD>(subAuthorityCount) - 1);

    // 从高到低比较，保证取值落在哪个区间就报哪一档；未达 Low 的统称 Untrusted。
    const char *name = "Untrusted";
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID)
        name = "System";
    else if (rid >= SECURITY_MANDATORY_HIGH_RID)
        name = "High";
    else if (rid >= SECURITY_MANDATORY_MEDIUM_RID)
        name = "Medium";
    else if (rid >= SECURITY_MANDATORY_LOW_RID)
        name = "Low";

    // 同时给出可读名与原始 RID：前者便于阅读，后者用于与 whoami /groups、
    // Process Explorer 等外部工具的输出逐位对齐，避免"名字相同、数值不同"的误判。
    return QStringLiteral("%1 (0x%2)").arg(QLatin1String(name)).arg(rid, 0, 16);
#else
    // 非 Windows 平台没有完整性级别概念。
    return QStringLiteral("unknown");
#endif
}

// ---------------------------------------------------------------------------
//  COM 连接与窗口绑定
// ---------------------------------------------------------------------------

/**
 * @brief 确保 ITaskbarList3 已创建并就绪（幂等）。
 *
 * 内部流程与失败语义：
 *   1) 若 m_taskbar 已存在，直接返回 true（幂等，避免重复创建对象）；
 *   2) 调用 CoInitializeEx 初始化 COM 套间，并按下述三种返回分别处理；
 *   3) CoCreateInstance 创建任务栏对象并取 ITaskbarList3 接口；
 *   4) 调用 HrInit() 完成与任务栏服务的握手，失败则释放接口并返回 false；
 *   5) 全部成功才把接口指针记入 m_taskbar。
 *
 * 三种 CoInitializeEx 返回的处理（这是本函数最需要小心的地方）：
 *   · S_OK    —— 本线程首次初始化 COM，引用计数由 0 变 1，必须由本对象配平
 *                CoUninitialize()，故置 m_comReady = true；
 *   · S_FALSE —— 本线程此前已被初始化过（通常是 Qt 自己做的），本次调用只是让
 *                引用计数再加一；既然"加过"就必须"减回去"，因此同样置
 *                m_comReady = true；
 *   · RPC_E_CHANGED_MODE —— 线程已用另一种套间模型（如 MTA，Multi-Threaded
 *                Apartment，多线程套间）初始化过，本次调用并未增加引用计数。
 *                此时若调用 CoUninitialize() 就会减掉别人加的计数，导致其它模块
 *                的 COM 提前失效，因此它绝不能置 m_comReady；同时它也不代表
 *                功能不可用——任务栏接口在 MTA 下同样能创建，所以不做失败处理。
 *
 * @return bool，true 表示 m_taskbar 可用；false 表示本次创建失败，对象保持未连接。
 * @note 必须在将调用 COM 接口的同一线程上执行（本类约定为 GUI 线程）。
 * @note 失败时不会留下半初始化状态：COINIT 被放弃、接口被 Release、m_comReady
 *       仅在前两种成功返回时置真，与析构逻辑严格配对。
 */
bool TaskbarProgress::ensureTaskbar()
{
#ifdef Q_OS_WIN
    // 幂等快速路径：接口已在手，无需重复初始化 COM，也无需重复创建对象。
    if (m_taskbar)
        return true;

    // COM 可能已被 Qt 初始化：RPC_E_CHANGED_MODE 表示"已按别的套间模型初始化过"，
    // 这种情况依然可以直接使用任务栏接口，因此不做失败处理。
    // COINIT_APARTMENTTHREADED 选用 STA（Single-Threaded Apartment，单线程套间）：
    // 任务栏集成属 UI 操作，与窗口消息循环同套间最稳妥；Qt 的 GUI 线程默认也是 STA，
    // 因此绝大多数情况下这里拿到的是 S_FALSE 而非 S_OK。
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // SUCCEEDED 同时覆盖 S_OK(0) 与 S_FALSE(1)；两者都代表"引用计数已加一"，
    // 必须由析构函数中的 CoUninitialize() 偿还，故统一置 m_comReady。
    if (SUCCEEDED(initResult))
        m_comReady = true;
    // 除 RPC_E_CHANGED_MODE 以外的失败（如 E_OUTOFMEMORY、CO_E_NOTINITIALIZED 环境
    // 问题）说明 COM 环境不可用，此时继续创建任务栏对象必然失败，直接返回可省一次调用。
    else if (initResult != RPC_E_CHANGED_MODE)
        return false;

    // 初始化为 nullptr：CoCreateInstance 失败时可能不改写该指针，先置空可让
    // 后面的 !taskbar 判定真正起到"防御未初始化"的作用。
    // CLSCTX_INPROC_SERVER 表示"只要进程内服务器"：任务栏对象由 explorer.exe
    // 通过进程内方式实现，不涉及跨进程代理，延迟最低。
    ITaskbarList3 *taskbar = nullptr;
    const HRESULT createResult = CoCreateInstance(clsidTaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                                  iidTaskbarList3, reinterpret_cast<void **>(&taskbar));
    // FAILED 覆盖所有错误码（< 0），并额外用 !taskbar 兜底"返回成功却没给出指针"的
    // 异常实现；此处直接返回，不触碰 m_comReady，避免为一次失败多算一次反初始化。
    if (FAILED(createResult) || !taskbar)
        return false;

    // HrInit() 是任务栏对象的初始化握手，失败说明当前会话没有可用的任务栏。
    // 典型失败场景：无桌面会话（会话 0/服务）、explorer.exe 尚未启动或已崩溃、
    // 远程会话未建立 Shell。失败语义是"这次连接不可用"，而不是永久不可用——
    // 调用方之后仍可再次 attachTo() 重试，因此这里只清理局部资源、不置任何阻塞标志。
    if (FAILED(taskbar->HrInit())) {
        // 握手失败必须先 Release：否则这个局部接口指针随函数返回而丢失，
        // 引用计数永远减不回去，任务栏对象将无法被卸载。
        taskbar->Release();
        return false;
    }

    // 走到这里才把所有权交给成员变量；此前所有失败路径都不会改写对象状态，
    // 保证 ensureTaskbar() 失败后 isAttached() 仍为 false。
    m_taskbar = taskbar;
    return true;
#else
    // 非 Windows 平台没有任务栏集成，恒判失败，调用方据此走降级路径。
    return false;
#endif
}

/**
 * @brief 把任务栏集成绑定到指定窗口的 HWND。
 *
 * 流程：校验入参 → 取窗口原生句柄 → 登记 m_hwnd → ensureTaskbar() → 复位角标阻塞标志。
 * 任一环节失败都会把 m_hwnd 还原为 nullptr，保证对象不会停留在"有窗口无接口"的
 * 半连接状态（否则 isAttached() 的语义就会被破坏）。
 *
 * 关于调用时机：winId() 本身会强制 Qt 创建原生窗口，所以在 show() 之前调用也能
 * 拿到非空句柄；但任务栏按钮是窗口在屏幕上真正显示之后才由系统创建的，过早调用
 * SetProgressValue/SetOverlayIcon 会作用在一个尚不存在的按钮上而静默无效。
 * 因此实际使用约定是：先 show()，再 attachTo()，然后才开始刷新进度与角标。
 *
 * @param[in] window QWidget*，目标窗口；传 nullptr 表示不绑定（等价于空操作式失败），
 *                   函数不会解绑已有窗口，只是返回 false。
 * @return bool，true 表示窗口句柄与任务栏接口都已就绪，后续 setProgress()/setBadge() 生效。
 * @note 同一对象重复调用会覆盖 m_hwnd，用于"窗口销毁重建后重新绑定"的场景。
 * @note 本函数不吞掉旧窗口的状态：若切换窗口，应先对旧窗口 clearAll()。
 */
bool TaskbarProgress::attachTo(QWidget *window)
{
    // 空指针校验放在条件编译之前：非 Windows 平台同样需要这层防御，
    // 且能避免在不同平台上出现两套不同的参数校验逻辑。
    if (!window)
        return false;

#ifdef Q_OS_WIN
    // winId() 会强制创建原生窗口句柄；此时窗口尚未 show() 也安全。
    // WId 在 Windows 平台就是 HWND，reinterpret_cast 不产生运行时代价；
    // 注意 winId() 是 Qt 中少数会"创建资源"的 const 语义函数。
    HWND handle = reinterpret_cast<HWND>(window->winId());
    // 理论上 winId() 不会失败，但句柄为 0 时后续所有调用都会无效，故显式拦截。
    if (!handle)
        return false;

    // 先登记再初始化：ensureTaskbar() 不依赖 m_hwnd，但这样写能让失败路径统一
    // 走下面的回滚分支，避免出现两处状态清理代码。
    m_hwnd = handle;
    if (!ensureTaskbar()) {
        // 回滚：留着非空 m_hwnd 会让 isAttached() 之外的地方误以为绑定成功，
        // 也会让 clearAll() 之类函数去做无意义的判断，故一并还原。
        m_hwnd = nullptr;
        return false;
    }
    // 重新绑定窗口（例如窗口被销毁重建）时给角标一次重新尝试的机会
    // 理由：上次的 E_ACCESSDENIED 可能是旧进程会话/旧窗口造成的，新窗口值得重试；
    // 若新环境仍然拒绝，setBadge() 会再次把该标志置真，代价仅为一次失败调用。
    m_badgeBlocked = false;
    return true;
#else
    // 非 Windows 平台的退化分支：显式吞掉参数以避免"未使用形参"警告（-Wunused-parameter）。
    Q_UNUSED(window);
    return false;
#endif
}

// ---------------------------------------------------------------------------
//  进度条
// ---------------------------------------------------------------------------

/**
 * @brief 设置任务栏按钮上的进度条百分比。
 *
 * 执行顺序与设计要点：
 *   1) 未连接任务栏（isAttached() 为假）时直接返回，不产生任何副作用；
 *   2) 负数表示"清除进度条"，转调 clearProgress()，让调用方用单一入口表达两种意图；
 *   3) 把入参夹到 0~100，防止上游计算溢出（如 -5% 之外的 120%）导致任务栏显示异常；
 *   4) 必须先 SetProgressState(TBPF_NORMAL) 再 SetProgressValue——因为若上一次的
 *      状态是 TBPF_NOPROGRESS（已隐藏），只更新数值并不会让进度条重新出现，
 *      系统会继续按"无进度"渲染；状态与数值是两套独立属性，必须都设置；
 *   5) 两个 HRESULT 都被保留下来合成 m_progressApplied，供 `--probe-ui` 自检读取。
 *
 * @param[in] percent int，占用百分比；取值范围会被夹到 0~100，< 0 表示清除进度条。
 * @return 无。
 * @note 两个 COM 调用的返回值只用于自检记录，不在此处中断流程——进度条属于
 *       锦上添花的呈现，失败不应影响主功能（用量统计与刷新）。
 * @note 必须与窗口同线程调用（STA 套间要求）。
 */
void TaskbarProgress::setProgress(int percent)
{
    // 未连接时静默返回：调用方无需先判断 isAttached()，降低使用门槛。
    if (!isAttached())
        return;
    // 负数语义 = 清除；集中在此转换可以让上游只维护"< 0 表示无效/隐藏"这一条约定。
    if (percent < 0) {
        clearProgress();
        return;
    }
#ifdef Q_OS_WIN
    // 夹取到合法区间：TBPF_NORMAL 下任务栏按 completed/total 计算比例，
    // 传入越界值虽不崩溃，但会得到"进度条卡在满格"这类难排查的显示问题。
    const int clamped = qBound(0, percent, 100);
    // 先置为 NORMAL 再给值：若上次是 NOPROGRESS 状态，只设置数值不会让进度条重新出现。
    // TBPF_NORMAL 表示"正常绿色进度"；状态与数值分离是 Win32 任务栏 API 的设计，
    // 因此这里刻意不做"状态是否变化"的判断，每次刷新都显式置一次，代价极低。
    const HRESULT stateResult = m_taskbar->SetProgressState(m_hwnd, TBPF_NORMAL);
    // SetProgressValue 的签名要求 ULONGLONG，故对已夹取的 int 做显式转换；
    // 分母用 100 与百分比语义直接对应，避免额外换算引入取整误差。
    const HRESULT valueResult = m_taskbar->SetProgressValue(m_hwnd, static_cast<ULONGLONG>(clamped), 100ULL);
    // 记录 HRESULT 供自检报告：只有两个调用都成功，才认为"进度条真的显示出来了"
    // 之所以用"与"而不是看最后一个返回值：状态成功但数值失败时，任务栏会显示
    // 一个没有具体比例的进度条，仍然是无效呈现，必须判为失败。
    m_progressApplied = SUCCEEDED(stateResult) && SUCCEEDED(valueResult);
#else
    // 非 Windows 平台：同样的参数校验（夹取发生在 Windows 分支，此处不需要），
    // 仅吞掉形参以满足 -Wunused-parameter 检查。
    Q_UNUSED(percent);
#endif
}

/**
 * @brief 清除任务栏按钮上的进度条。
 *
 * 通过 SetProgressState(TBPF_NOPROGRESS) 把状态置为"无进度"。
 * 必须使用状态调用而不是 SetProgressValue(hwnd, 0, 100)：后者只会把进度条画成
 * 0% 的空槽，任务栏按钮上仍会保留一条进度条轮廓，与"隐藏进度条"的预期不符；
 * TBPF_NOPROGRESS 才是真正让进度条消失的唯一方式。
 *
 * @return 无。
 * @note 本函数不修改 m_progressApplied——清除操作本身不构成"呈现成功"的证据，
 *       自检报告里保留的是最近一次 setProgress() 的结果。
 * @note 未连接任务栏时直接返回，可安全地在窗口销毁阶段反复调用。
 */
void TaskbarProgress::clearProgress()
{
    // 未连接时无可清理，直接返回；这里不做 HWND 有效性校验（判空成本高于收益，
    // 且窗口已销毁属于调用方的生命周期管理问题）。
    if (!isAttached())
        return;
#ifdef Q_OS_WIN
    // 只置状态、不动数值：数值会在下次 setProgress() 时与状态一起被覆盖，
    // 少一次 COM 调用可以让"隐藏到托盘"这条高频路径更快完成。
    m_taskbar->SetProgressState(m_hwnd, TBPF_NOPROGRESS);
#endif
}

// ---------------------------------------------------------------------------
//  角标（覆盖图标）
// ---------------------------------------------------------------------------

/**
 * @brief 在任务栏按钮右下角显示彩底文字的角标（overlay icon）。
 *
 * 完整流程：
 *   1) 未连接或已被系统拒绝过角标（m_badgeBlocked）时直接返回；
 *   2) 在 32×32 的透明 QPixmap 上画一个实心圆，颜色由 valid 决定（无效数据用灰色）；
 *   3) 依据文字长度选择字号并居中绘制白字；
 *   4) createHIcon() 把 QPixmap 转成 HICON，失败则不调用 COM（避免交出空句柄）；
 *   5) SetOverlayIcon() 把新句柄交给任务栏，记录 HRESULT 与两个探针字段；
 *   6) 只有在新句柄已经被系统接收之后，才 DestroyIcon() 释放旧句柄，最后接管新句柄。
 *
 * 边界条件：
 *   · text 为空时绘制破折号"—"，保证角标不会变成纯色圆点而不传递信息；
 *   · 文字长度 > 4 时字号降到 10px，避免 "100.0%" 这类长文本溢出圆形；
 *   · valid 为 false 时使用灰色底，与"真实配色"形成可区分的视觉信号。
 *
 * @param[in] text QString，角标文字（建议 ≤ 4 字符，如 "15%"）；空串按"—"处理。
 * @param[in] color QColor，角标底色，通常由用量档位决定（绿/黄/红）。
 * @param[in] valid bool，数据是否有效；false 时使用灰色底色表示"未知"。
 * @return 无。
 * @note SetOverlayIcon 要求窗口的任务栏按钮已存在（即窗口已 show()）；
 *       同时要求进程完整性级别不低于任务栏，否则返回 E_ACCESSDENIED。
 * @note 每次调用都会新建 HICON 并释放上一个，因此不会累积 GDI 句柄。
 */
void TaskbarProgress::setBadge(const QString &text, const QColor &color, bool valid)
{
    // 未连接任务栏时静默返回：角标是可选增强，不应让调用方承担判空负担。
    if (!isAttached())
        return;
    // 系统已经明确拒绝过角标（典型场景：进程完整性级别低于任务栏，
    // SetOverlayIcon 返回 E_ACCESSDENIED）时不再重试：每次刷新都调用一个必然
    // 失败的 COM 接口没有意义，还会拖慢刷新；进度条不受影响，功能自动降级。
    if (m_badgeBlocked)
        return;
#ifdef Q_OS_WIN
    // 离屏画布：先画在 QPixmap 上再整体转成 HICON，避免直接操作 GDI 绘图 API 的
    // 状态管理（GDI 需要手工保存/恢复 DC 属性，Qt 的绘制栈更不易出错）。
    QPixmap canvas(badgeCanvasSize, badgeCanvasSize);
    // 透明填充是必须的：QBitmap/QPixmap 默认内容未初始化，不填充会出现花屏噪点；
    // 同时 32 位 DIB 的 Alpha 通道靠这一步建立"角标之外全透明"的语义。
    canvas.fill(Qt::transparent);

    // QPainter 作用于栈上，作用域结束或显式 end() 都会结束绘制状态。
    QPainter painter(&canvas);
    // 圆形边缘必须抗锯齿，否则 16×16 显示时会出现明显的阶梯状毛边。
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 文字抗锯齿单独开关：小字号下它比图形抗锯齿更影响可读性。
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    // 不要描边：角标本身只有 16 像素，描边会吃掉有限的笔画宽度。
    painter.setPen(Qt::NoPen);
    // valid 决定底色：无效数据用中灰（0x8A8A8A）表示"未知"，避免与绿/黄/红语义混淆。
    painter.setBrush(valid ? color : QColor(0x8A, 0x8A, 0x8A));
    // 画满整个画布并做成圆形：任务栏角标位置很小，留白会进一步压缩可读面积。
    // 内缩 0.5 像素并把直径减 1：让抗锯齿的圆形轮廓落在像素格内部，边缘更干净。
    painter.drawEllipse(QRectF(0.5, 0.5, badgeCanvasSize - 1.0, badgeCanvasSize - 1.0));

    // 空文本退化为破折号：保证任何调用都能产生可辨识的角标，而不是一个纯色圆点。
    const QString shown = text.isEmpty() ? QStringLiteral("—") : text;
    // 字符数越多字号越小，保证 "100%" 也不会溢出圆形。
    // qMax(1, ...) 兜住空串（理论上已被 shown 的破折号排除），使后续比较总是有定义。
    const int length = qMax(1, shown.size());
    // 从painter 取出当前字体再改属性：保证字形族与系统 UI 字体一致，
    // 只覆盖像素字号与粗细，避免自造 QFont 造成的字形差异。
    QFont font = painter.font();
    // 四级字号阶梯（≤2 字符 20px、3 字符 16px、4 字符 12px、更长 10px）：
    // 用空间换可读性，让 "99%" 尽量大、"100.0%" 也仍能完整落在圆内。
    font.setPixelSize(length <= 2 ? 20 : (length <= 3 ? 16 : (length <= 4 ? 12 : 10)));
    // 粗体：小尺寸下细笔画在缩放后几乎不可见，加粗能显著提升辨识度。
    font.setBold(true);
    painter.setFont(font);
    // 白字配深色底：无论底是三档配色中的哪一档，白字都有足够对比度。
    painter.setPen(Qt::white);
    // 在整块画布矩形内居中绘制文字，与圆心天然重合；AlignCenter 同时做水平与垂直居中。
    painter.drawText(canvas.rect(), Qt::AlignCenter, shown);
    // 显式结束绘制：虽然析构也会结束，但这里要立刻把 QPixmap 交给 createHIcon()，
    // 提早 end() 可以确保所有绘制指令已经落到位图上（并尽早释放绘制资源）。
    painter.end();

    // 转成 HICON；失败时返回 nullptr，调用方按失败路径处理。
    HICON fresh = createHIcon(canvas);
    // 先记录"图标是否造出来"这一层事实，与"系统是否接收"分开记录，
    // 便于自检把 GDI 侧故障与任务栏 COM 侧故障区分开。
    m_badgeIconCreated = (fresh != nullptr);
    if (!fresh) {
        // 图标没造出来就不调用 COM：避免把一个空句柄交给任务栏
        // （空句柄会让系统清除已有角标，表现为"角标莫名消失"）；
        // 同时把 applied 置假并直接返回，保留旧 m_badge 不动——旧角标虽然是过期
        // 数据，但比"什么都没有"更接近真实状态，且避免句柄被误释放。
        m_badgeApplied = false;
        return;
    }

    // 先交出新句柄再由本对象释放旧句柄：顺序反了会出现短暂的"无角标"闪烁。
    // 原因：任务栏收到新句柄只是替换引用，不会同步取走旧句柄的所有权；
    // 若先 DestroyIcon(m_badge) 再调用 SetOverlayIcon，在两次调用之间任务栏
    // 仍指向已销毁的图标，任务栏线程重绘时会读到失效句柄。
    // 第三个参数是辅助功能（屏幕阅读器）用的描述文本，写死为固定英文短句即可。
    const HRESULT badgeResult = m_taskbar->SetOverlayIcon(m_hwnd, fresh, L"Command Code usage");
    // 以 long 形式保留原始 HRESULT：自检报告会以十六进制打印它，
    // 让人能直接对照 MSDN 错误码定位问题（如 0x80070005 = E_ACCESSDENIED）。
    m_badgeHresult = static_cast<long>(badgeResult);
    m_badgeApplied = SUCCEEDED(badgeResult);
    // 访问被拒说明当前进程无权设置角标（最常见的是完整性级别低于任务栏），
    // 记下来供自检报告，并从此不再重试。
    // E_ACCESSDENIED（0x80070005）的根因是 Windows 的 UIPI/UIPI 类完整性检查：
    // 若本进程以普通权限运行而任务栏由更高完整性级别的进程持有，跨进程调用会被拒绝；
    // 这是环境性、持续性的限制，不会因为重试而改变，故置 m_badgeBlocked 停止尝试，
    // 避免每秒一次的刷新都白跑一趟 COM（并可能反复触发系统安全检查）。
    if (badgeResult == E_ACCESSDENIED)
        m_badgeBlocked = true;
    // 此时新句柄已交给系统（成功），或已被系统拒绝（失败）——
    // 两种情况下 m_badge 引用的旧句柄都不再被任务栏需要，可以安全销毁。
    // 系统接收后需要继续持有的是 fresh，而不是 m_badge。
    if (m_badge)
        DestroyIcon(m_badge);
    // 接管新句柄的所有权，供下次替换或析构时释放。
    m_badge = fresh;
#else
    // 非 Windows 平台：三个形参全部显式吞掉，避免 -Wunused-parameter 警告，
    // 同时保持与 Windows 分支一致的函数签名与返回语义（无返回值）。
    Q_UNUSED(text);
    Q_UNUSED(color);
    Q_UNUSED(valid);
#endif
}

/**
 * @brief 清除任务栏按钮上的角标，并释放本对象持有的角标句柄。
 *
 * 实现要点：先以 nullptr 句柄调用 SetOverlayIcon()，这是 MSDN 规定的"移除角标"
 * 方式（并非传入一个空白图标）；随后再释放本对象缓存的 m_badge。
 * 顺序不能颠倒：若先 DestroyIcon 再调用 SetOverlayIcon(nullptr,...)，在两次调用
 * 之间任务栏仍引用着已销毁的图标（与 setBadge 中的闪烁问题同源）。
 *
 * @return 无。
 * @note 本函数不清除 m_badgeBlocked / m_badgeApplied / m_badgeHresult：
 *       它们记录的是"上一次设置尝试"的历史事实，清除动作不改变该事实。
 * @note 未连接任务栏时直接返回，避免用失效 HWND 调用 COM。
 */
void TaskbarProgress::clearBadge()
{
    // 与 clearProgress() 同构的前置判断，保证两个清除接口行为一致。
    if (!isAttached())
        return;
#ifdef Q_OS_WIN
    // 传 nullptr 才是"移除角标"的正式写法；描述文本传空串，因为此时已无覆盖图标。
    m_taskbar->SetOverlayIcon(m_hwnd, nullptr, L"");
    // 系统已放弃对该图标的引用，本对象必须释放自己的那一份，否则每次 clearBadge()
    // 都会泄漏一个 HICON，最终耗尽进程的 GDI 句柄配额（默认上限 10000）。
    if (m_badge) {
        DestroyIcon(m_badge);
        m_badge = nullptr;
    }
#endif
}

/**
 * @brief 一次性清除进度条与角标。
 *
 * 主要调用场景是"窗口隐藏到系统托盘"：任务栏按钮本身会随窗口隐藏而消失，但状态
 * 残留在任务栏对象内部，下次显示窗口时可能瞬间闪出旧数值，因此在隐藏前主动清理。
 * 析构函数也复用它，作为统一的"恢复任务栏按钮为无状态"入口。
 *
 * @return 无。
 * @note 内部两个清除函数都各自做了 isAttached() 判断，因此本函数在未连接状态下
 *       调用也是安全的（等价于空操作）。
 * @note 不释放 COM 接口本身——清除状态与断开连接是两件事，后者只在析构时发生。
 */
void TaskbarProgress::clearAll()
{
    // 先清进度条再清角标：进度条是"持续可见"的强提示，优先让它消失；
    // 两者互不依赖，顺序仅影响极短暂中间态下的观感。
    clearProgress();
    clearBadge();
}

// ---------------------------------------------------------------------------
//  QPixmap → HICON 转换
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN
/**
 * @brief 把 Qt 位图转换成 Windows HICON（图标句柄）。
 *
 * 步骤与关键约定：
 *   1) 统一转成 QImage::Format_ARGB32（非预乘、每像素 32 位、字节序 BGRA），
 *      与 32 位 DIB（Device Independent Bitmap，设备无关位图）的内存布局一致；
 *   2) 组装 BITMAPV5HEADER 描述目标 DIB：尺寸、1 个调色板平面、32 位色、
 *      BI_BITFIELDS 压缩方式，并给出 R/G/B/A 四个通道掩码；
 *   3) 取屏幕 DC 作为 CreateDIBSection 的兼容参考，创建可写的颜色位图并拿到像素指针；
 *   4) 把 QImage 的像素整体 memcpy 进 DIB（32 位下 bytesPerLine 恰为 width*4，无跨行填充问题）；
 *   5) 创建一个 1bpp 单色掩码位图（32 位带 Alpha 时系统不使用它，但 ICONINFO 要求非空）；
 *   6) CreateIconIndirect() 生成 HICON，随后立即释放两个临时位图。
 *
 * 边界条件与失败路径：QImage 为空、GetDC 失败、CreateDIBSection 失败、bits 为
 * nullptr 时一律返回 nullptr；其中 CreateDIBSection 分配失败时若句柄已创建，会
 * 先 DeleteObject 再返回，避免在失败路径上泄漏 GDI 对象。
 *
 * @param[in] pixmap QPixmap，源位图；内部会转成 QImage::Format_ARGB32，不做尺寸校验。
 * @return HICON，成功返回图标句柄（**调用方负责 DestroyIcon**）；失败返回 nullptr。
 * @note 使用 32 位带 Alpha 的 DIB 时掩码位图不会被使用，但仍需提供以满足 API 要求。
 * @note 位图内容在 CreateIconIndirect() 时已被复制，因此函数返回后两个 HBITMAP
 *       即可销毁；泄漏它们会让每次角标刷新都永久占用一块 GDI 内存。
 */
HICON TaskbarProgress::createHIcon(const QPixmap &pixmap)
{
    // 统一成非预乘的 32 位 ARGB：DIB 里的字节序是 BGRA，与之正好对应。
    // 必须显式转换：QPixmap 内部格式随平台/绘制内容而变（可能是 RGB32、带预乘的
    // ARGB32_Premultiplied 等），直接取 constBits() 会得到错误的通道顺序或步长。
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    // 空图像说明上游传入了未初始化的 QPixmap；继续下去会创建 0×0 的 DIB 并
    // 让 CreateIconIndirect 失败，不如在此提前返回，失败原因更明确。
    if (image.isNull())
        return nullptr;

    // 零初始化整个结构体：BITMAPV5HEADER 末尾还有 bV5CSType / bV5Endpoints 等字段，
    // 本实现不使用它们，但必须为 0 才能让系统按默认 sRGB 语义解释颜色。
    BITMAPV5HEADER header = {};
    // bV5Size 必须填写结构体真实大小，系统据此判断调用方使用的是哪一版 BITMAP 头
    // （V4 与 V5 的差别由该字段区分）。
    header.bV5Size = sizeof(BITMAPV5HEADER);
    // 宽高直接取图像尺寸；角标场景下恒为 badgeCanvasSize（32）。
    header.bV5Width = image.width();
    header.bV5Height = -image.height();   // 负高度 = 自顶向下，与 QImage 的行序一致
    // 负高度是 Win32 的约定：正数表示自底向上存储，负数表示自顶向下。
    // QImage 的行序是自顶向下（第一行在最低地址），因此必须用负高度，
    // 否则图标会垂直翻转。
    // 平面数固定为 1：现代 DIB 不使用多平面。
    header.bV5Planes = 1;
    // 32 位色：每像素 4 字节，依次是 B、G、R、A，与 Format_ARGB32 的内存布局一一对应。
    header.bV5BitCount = 32;
    // BI_BITFIELDS 表示"颜色分量位置由下面的掩码指定"，这是 32 位带 Alpha 的标准写法；
    // 若用 BI_RGB，Alpha 通道不会被系统识别，角标会变成不透明的黑底方块。
    header.bV5Compression = BI_BITFIELDS;
    // 四个通道掩码告诉系统每个颜色分量占哪几位（此处即标准 BGRA 排列）：
    // 红 0x00FF0000、绿 0x0000FF00、蓝 0x000000FF、Alpha 0xFF000000。
    // 掩码必须与 QImage::Format_ARGB32 的实际字节序严格对应，否则会出现红蓝互换。
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    // 取屏幕 DC 仅用作"兼容参考设备"：DIB 是设备无关的，DC 只提供色彩管理上下文。
    // 传 nullptr 表示使用当前屏幕，多显示器下无需区分。
    HDC screen = GetDC(nullptr);
    // 极少数情况下（如会话正在注销）取不到 DC，此时无法创建 DIB，安全返回 nullptr。
    if (!screen)
        return nullptr;

    // bits 由 CreateDIBSection 回填，指向可直接写入的像素内存；
    // DIB_RGB_COLORS 表示颜色表中是字面 RGB 值（与 DIB_PAL_COLORS 相对）。
    void *bits = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO *>(&header),
                                           DIB_RGB_COLORS, &bits, nullptr, 0);
    // DC 用完立即归还：GDI 的 DC 是稀缺资源，进程级配额有限，
    // 在可能提前返回的路径上更应尽早释放。
    ReleaseDC(nullptr, screen);
    if (!colorBitmap || !bits) {
        // 失败清理：CreateDIBSection 可能只成功了一半（拿到句柄但没给出像素指针），
        // 这种半成品句柄同样必须 DeleteObject，否则每次失败都会泄漏一个位图对象。
        if (colorBitmap)
            DeleteObject(colorBitmap);
        return nullptr;
    }

    // 直接按行拷贝，避免逐像素循环；QImage 每行可能带 4 字节对齐填充，
    // 但 32 位格式下 bytesPerLine 恰好等于 width*4，可直接整体拷贝。
    // sizeInBytes() 给出的是整个缓冲（含各行填充）的字节数，对 32 位格式而言
    // 与 DIB 的连续内存布局完全一致，因此一次 memcpy 即完成全部像素的搬运。
    const int bytes = image.sizeInBytes();
    memcpy(bits, image.constBits(), static_cast<size_t>(bytes));

    // 32 位带 Alpha 的图标不使用掩码，但 CreateIconIndirect() 要求提供一个。
    // 这里创建 1bpp（1 位/像素）单色位图作为占位；若为 nullptr，CreateIconIndirect
    // 会直接失败，因此即便"用不到"也必须给一个合法句柄。
    HBITMAP maskBitmap = CreateBitmap(image.width(), image.height(), 1, 1, nullptr);

    // ICONINFO 描述图标的构成，同样需要零初始化（未列出的字段如 xHotspot/yHotspot
    // 对图标无意义，但保持为 0 才能让系统走"图标"而非"光标"的解析路径）。
    ICONINFO info = {};
    // fIcon = TRUE 声明这是一个图标（ICON）而不是光标（CURSOR），
    // 区别在于后者会使用热点坐标并改变系统对句柄的解释。
    info.fIcon = TRUE;
    // 颜色位图：提供图标的外观与 Alpha 通道。
    info.hbmColor = colorBitmap;
    // 掩码位图：32 位带 Alpha 时被忽略，但 API 要求非空。
    info.hbmMask = maskBitmap;

    // 生成图标：系统会把两个位图的内容复制进新建的图标对象。
    HICON icon = CreateIconIndirect(&info);

    // 位图已被 CreateIconIndirect 复制，两个临时位图必须立即释放，否则每次刷新都会泄漏。
    // 这是本函数最重要的资源约定：colorBitmap 与 maskBitmap 的所有权始终在本函数，
    // 无论 CreateIconIndirect 成功还是失败都要 DeleteObject。
    DeleteObject(colorBitmap);
    // maskBitmap 可能因资源不足而为 nullptr，DeleteObject(nullptr) 虽为无操作，
    // 但显式判空可以避免依赖 API 的容错行为。
    if (maskBitmap)
        DeleteObject(maskBitmap);
    // 返回图标句柄；失败时为 nullptr，由调用方（setBadge）决定是否调用 COM。
    return icon;
}
#else
/**
 * @brief 非 Windows 平台的 createHIcon 退化实现。
 *
 * 该平台没有 HICON 这一概念（TaskbarProgress.h 中 HICON 也只是不完整类型的前置
 * 声明），因此无需、也无法做任何转换。保留同名函数的意义在于：让两个平台上的
 * 函数签名、可见性与调用点完全一致，setBadge() 之外不再需要额外的平台判断；
 * 而 setBadge() 的非 Windows 分支本身也不会调用本函数。
 *
 * @param[in] pixmap QPixmap，源位图；此实现不使用该参数。
 * @return HICON，恒为 nullptr，表示"本平台永远无法创建图标句柄"。
 * @note 返回 nullptr 与 Windows 分支的失败返回值语义一致，调用方无需区分平台。
 */
HICON TaskbarProgress::createHIcon(const QPixmap &pixmap)
{
    // 显式吞掉形参，避免 -Wunused-parameter 警告；这也是"本平台不使用该参数"
    // 这一事实的代码级说明。
    Q_UNUSED(pixmap);
    // 恒返回空句柄：调用方（若将来有跨平台调用）据此走"无角标"降级路径。
    return nullptr;
}
#endif
