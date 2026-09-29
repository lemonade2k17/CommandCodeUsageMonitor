// ---------------------------------------------------------------------------
//  TaskbarProgress.h —— Windows 任务栏按钮上的缩略信息
//
//  职责：把用量百分比画到任务栏按钮上，包含两部分：
//        · 进度条：ITaskbarList3::SetProgressValue（任务栏按钮底部的填充条）；
//        · 角标：  ITaskbarList3::SetOverlayIcon（按钮右下角的彩色徽标，用于配色告警）。
//
//  为什么直接用 COM 而不用 Qt 封装：Qt 6 已移除 QtWinExtras，QWinTaskbarButton
//  不再可用；直接调用 ITaskbarList3 既没有额外依赖（ole32 属系统库），也不影响
//  静态链接。
//
//  设计要点：
//    1. 类本身不依赖 Qt Widgets 的窗口类型，只借用 HWND，便于单元化理解；
//    2. 所有调用都做失败兜底：在无桌面会话、任务栏不可用或系统不支持时，
//       isAttached() 返回 false，调用方据此跳过即可，不会抛异常；
//    3. HICON 的创建与释放严格配对，反复刷新不会累积 GDI 句柄。
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QString>

class QWidget;
class QPixmap;
struct ITaskbarList3;
typedef struct HICON__ *HICON;
typedef struct HWND__ *HWND;

// ---------------------------------------------------------------------------
//  TaskbarProgress —— 任务栏按钮进度条与角标
// ---------------------------------------------------------------------------
class TaskbarProgress
{
public:
    /**
     * @brief 构造对象；此时尚未连接任何窗口，也不访问任务栏。
     *
     * 构造函数不做 COM 初始化、不创建任务栏对象，把"可能失败"的动作全部推迟到
     * attachTo()，这样对象可以在任何时机安全地构造（包括没有桌面会话的场景）。
     *
     * @return 无。
     */
    TaskbarProgress();

    /**
     * @brief 析构：释放角标 HICON、清空任务栏状态并释放 COM 接口。
     * @return 无。
     * @note 会在窗口已销毁的情况下跳过状态清理，只释放自身持有的资源。
     */
    ~TaskbarProgress();

    // 明确禁止拷贝：内部持有 COM 接口指针与 HICON 句柄，拷贝会导致重复释放。
    TaskbarProgress(const TaskbarProgress &) = delete;
    TaskbarProgress &operator=(const TaskbarProgress &) = delete;

    /**
     * @brief 把任务栏集成绑定到指定窗口。
     *
     * 内部流程：取窗口的 HWND → 创建 ITaskbarList3 → 调用 HrInit()。
     * 任一步失败都会返回 false 并保持未连接状态，调用方可据此禁用该功能。
     *
     * @param[in] window QWidget*，目标窗口；传 nullptr 等价于 detach。
     * @return bool，true 表示已成功连接任务栏，后续 setProgress()/setBadge() 生效。
     * @note 只应在窗口首次显示后调用，Windows 要求任务栏按钮已存在。
     */
    bool attachTo(QWidget *window);

    /**
     * @brief 当前是否已成功连接任务栏。
     * @return bool，true 表示可以安全调用 setProgress()/setBadge()。
     */
    bool isAttached() const;

    /**
     * @brief 最近一次设置进度条是否成功。
     *
     * SetProgressValue / SetProgressState 返回 HRESULT，把它记录下来是为了让
     * `--probe-ui` 能给出"进度条确实被系统接受"这一客观事实，而不是只能靠肉眼看任务栏
     * （任务栏按钮在系统托盘化的布局下很难在截图里定位）。
     *
     * @return bool，true 表示最近一次 setProgress() 的两个调用都返回成功。
     */
    bool progressApplied() const { return m_progressApplied; }

    /**
     * @brief 最近一次设置角标是否成功。
     * @return bool，true 表示最近一次 setBadge() 成功把 HICON 交给了任务栏。
     */
    bool badgeApplied() const { return m_badgeApplied; }

    /**
     * @brief 最近一次角标流程中，HICON 是否创建成功。
     *
     * 与 badgeApplied() 配合可把失败定位到"图标没造出来"还是"系统不收"：
     * 前者是 GDI 侧问题（位图/图标创建），后者是任务栏 COM 侧问题。
     *
     * @return bool，true 表示 QPixmap → HICON 的转换成功。
     */
    bool badgeIconCreated() const { return m_badgeIconCreated; }

    /**
     * @brief 最近一次 SetOverlayIcon 的 HRESULT，供自检输出十六进制诊断码。
     * @return long，HRESULT 原始值；成功时为 0。
     */
    long badgeHresult() const { return static_cast<long>(m_badgeHresult); }

    /**
     * @brief 当前系统/会话是否支持任务栏进度与角标。
     * @return bool，false 表示无桌面会话或系统版本次于 Windows 7。
     */
    static bool isSupported();

    /**
     * @brief 纯探测：不绑定窗口，仅验证任务栏 COM 对象能否创建并初始化。
     *
     * 供 `--selftest` 的桌面集成诊断使用：自检过程不创建主窗口，无法走 attachTo()，
     * 但仍需客观报告"这台机器上任务栏接口是否可用"，以便把"功能没生效"与
     * "系统不支持"区分开。
     *
     * @return bool，true 表示 CoCreateInstance + HrInit 成功。
     * @note 内部创建的临时对象会在返回前完成 COM 反初始化，不留下引用计数。
     */
    static bool probe();

    /**
     * @brief 读取当前进程的完整性级别（integrity level）。
     *
     * 放在本类中是因为它是本项目里唯一的 Win32 互操作模块；托盘与任务栏都直接
     * 受该属性影响——`SetOverlayIcon` 在进程完整性级别低于任务栏时会返回
     * E_ACCESSDENIED。把该值写进自检报告，可以让"角标为什么没生效"从推断变成事实：
     * 报告里的 `processIntegrity = Low` 配合 `taskbarBadgeHresult = 0x80070005`，
     * 就足以定位是环境权限而不是程序缺陷。
     *
     * @return QString，形如 "Medium (0x2000)"；非 Windows 或读取失败时返回 "unknown"。
     * @note 只读取当前进程自身的令牌，不需要额外权限，也不会修改任何状态。
     */
    static QString processIntegrityLevel();

    /**
     * @brief 设置任务栏按钮进度条。
     *
     * 百分比会被夹到 0~100；传负数表示清除进度条（等价于 clearProgress()）。
     * 未连接任务栏时本函数直接返回，不产生副作用。
     *
     * @param[in] percent int，占用百分比；< 0 表示清除。
     * @return 无。
     * @note 进度条用 TBPF_NORMAL 状态；不在此处切换红/黄状态，因为配色由角标承担，
     *       两者分工能避免"进度条颜色被系统主题覆盖"造成的信息丢失。
     */
    void setProgress(int percent);

    /**
     * @brief 清除任务栏进度条。
     * @return 无。
     */
    void clearProgress();

    /**
     * @brief 在任务栏按钮右下角显示彩色角标。
     *
     * 角标内容为彩底 + 简短文字（如 "15%"）。文字超过 4 个字符时自动缩小字号；
     * 无效数据（valid=false）时使用灰色底以示"未知"。
     *
     * @param[in] text QString，角标文字，建议 ≤ 4 个字符。
     * @param[in] color QColor，角标底色。
     * @param[in] valid bool，false 时用灰色占位。
     * @return 无。
     * @note 每次调用都会创建新的 HICON 并释放上一个，避免 GDI 句柄泄漏。
     */
    void setBadge(const QString &text, const QColor &color, bool valid);

    /**
     * @brief 清除任务栏角标。
     * @return 无。
     */
    void clearBadge();

    /**
     * @brief 一次性清除进度条与角标（窗口隐藏到托盘时调用）。
     * @return 无。
     * @note 窗口隐藏后任务栏按钮本身会消失，但状态残留在任务栏对象里，
     *       下次显示窗口可能瞬间出现旧值，因此隐藏前应主动清理。
     */
    void clearAll();

private:
    /**
     * @brief 把 Qt 位图转换成 Windows HICON。
     *
     * 实现方式是构造一个 32 位自顶向下的 DIB（Device Independent Bitmap，设备无关位图），
     * 把 QImage 的 BGRA 像素直接拷进去，再用 CreateIconIndirect() 包成 HICON。
     *
     * @param[in] pixmap QPixmap，源位图；内部会统一转成 QImage::Format_ARGB32。
     * @return HICON，成功返回句柄（调用方负责 DestroyIcon），失败返回 nullptr。
     * @note 使用 32 位带 Alpha 的 DIB 时，掩码位图不会被使用，但仍需提供以满足 API 要求。
     */
    static HICON createHIcon(const QPixmap &pixmap);

    /**
     * @brief 确保 ITaskbarList3 已创建并就绪。
     * @return bool，true 表示接口可用。
     * @note 会对 RPC_E_CHANGED_MODE 做容忍处理：Qt 可能已用其它套间模型初始化过 COM，
     *       这种情况不影响任务栏接口的使用。
     */
    bool ensureTaskbar();

    ITaskbarList3 *m_taskbar = nullptr;  ///< 任务栏 COM 接口（未连接时为 nullptr）
    HWND           m_hwnd = nullptr;     ///< 绑定的窗口句柄
    HICON          m_badge = nullptr;    ///< 当前角标句柄，用于替换时释放
    bool           m_comReady = false;   ///< COM 是否已初始化（用于决定析构时是否反初始化）
    bool           m_progressApplied = false;  ///< 最近一次进度条设置是否被系统接受
    bool           m_badgeApplied = false;     ///< 最近一次角标设置是否被系统接受
    bool           m_badgeIconCreated = false; ///< 最近一次角标流程中 HICON 是否创建成功
    long           m_badgeHresult = 0;         ///< 最近一次 SetOverlayIcon 的 HRESULT
    bool           m_badgeBlocked = false;     ///< 系统拒绝过角标后置真，避免每次刷新都重试
};
