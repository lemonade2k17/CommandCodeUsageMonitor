// ---------------------------------------------------------------------------
//  TrayController.h —— 通知区域（系统托盘）图标控制器
//
//  职责：把"套餐用量"压缩成一条缩略信息，显示在 Windows 通知区域：
//        · 图标本体：动态绘制的彩底 + 百分比文字（如 "15%"），颜色随占用率变化；
//        · 悬停提示：套餐名、剩余额度、所选指标的已用/上限与重置时间、最后更新时间；
//        · 右键菜单：显示主窗口 / 立即刷新 / 设置 / 退出。
//
//  设计要点：
//    1. 本类**不依赖**任何 Win32 API，纯 Qt 实现，因此可在关闭任务栏集成时单独使用；
//    2. 图标 pixmap 每次收到新数据才重绘（缓存上一次的文本与颜色），避免无谓重绘；
//    3. 所有对外动作都以信号形式抛出，主窗口负责接线，本类不直接操作窗口。
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>

class QSystemTrayIcon;
class QMenu;
class QAction;

// ---------------------------------------------------------------------------
//  TrayStatus —— 托盘图标所需的全部展示数据
//
//  由主窗口在每次刷新成功后填充；结构本身不理解接口字段，只承载"已经算好的"
//  展示值，从而让托盘模块与接口解析模块彻底解耦。
// ---------------------------------------------------------------------------
struct TrayStatus
{
    bool valid = false;     ///< 是否已有一次成功的数据；false 时图标显示占位态
    QString metricName;     ///< 所选指标名称，如「5 小时限额」「每周限额」「剩余额度」
    int metricPercent = 0;  ///< 所选指标的占用百分比（0~100），用于配色与进度条
    QString metricText;     ///< 绘制在图标上的缩略文字，如 "15%"（长度建议 ≤ 4 字符）
    QString planName;       ///< 套餐显示名，如「GOAT」
    double remaining = 0.0; ///< 本周期剩余额度
    double total = 0.0;     ///< 套餐总额度
    QString detailText;     ///< 所选指标的明细，如「已用 2.24 / 14　重置还剩 2h 39m」
    QString updatedText;    ///< 最后刷新时间文本，如 "14:35:02"
    QStringList errors;     ///< 上次刷新的错误列表，非空时提示里会附上
};

// ---------------------------------------------------------------------------
//  TrayController —— 通知区域图标的生命周期与内容更新
// ---------------------------------------------------------------------------
class TrayController : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造托盘控制器并尝试创建托盘图标。
     *
     * 构造时即创建 QSystemTrayIcon 与其菜单，但**不会立刻显示**；是否显示由
     * setEnabled() 决定。若当前系统不支持托盘（QSystemTrayIcon::isSystemTrayAvailable()
     * 为假），对象仍然可用，只是 isSupported() 返回 false、setEnabled() 不会生效，
     * 以便调用方统一处理而无需到处判空。
     *
     * @param[in] parent QObject*，父对象；传 nullptr 表示由调用方自行管理生命周期。
     * @return 无。
     * @note 必须在 QApplication 构造之后创建：QSystemTrayIcon 依赖 GUI 事件循环。
     */
    explicit TrayController(QObject *parent = nullptr);

    /** @brief 析构：隐藏并释放托盘图标与菜单。 @return 无。 */
    ~TrayController() override;

    /**
     * @brief 当前系统是否提供通知区域。
     * @return bool，true 表示可以显示托盘图标；false 表示该平台/会话不支持。
     */
    bool isSupported() const;

    /**
     * @brief 图标当前是否处于显示状态。
     * @return bool，true 表示托盘图标可见。
     */
    bool isEnabled() const;

    /**
     * @brief 询问 Qt 侧的"显示请求状态"。
     *
     * 注意语义边界：本函数返回的是 QSystemTrayIcon 自己的标志位，只表示
     * "程序已请求显示图标"，**并不代表 shell 真的把它画在了任务栏上**。
     * Windows 11 默认会把新图标收进溢出区（`^`），此时本函数依然返回 true。
     * 因此自检用它验证"程序侧动作已完成"，而"肉眼是否可见"只能靠截图确认。
     *
     * @return bool，true 表示 Qt 认为图标已处于显示状态。
     */
    bool isVisible() const;

    /**
     * @brief 询问"关闭窗口时的气泡提示"是否已经弹过。
     *
     * 主窗口的 closeEvent() 用它实现"每次运行只提示一次"：首次关闭并隐藏到托盘时
     * 弹一条气泡告知用户程序没退出，之后不再打扰。
     * 注意语义边界：本标志位**只存在于内存、不写入配置文件**，因此程序重启后的
     * 第一次关闭会重新提示一次；若将来需要"装一次只提示一次"，应把它挪进 AppConfig。
     *
     * @return bool，true 表示本次运行已经**发起过**气泡请求（不代表系统真的显示出来了）。
     * @note 与 isVisible() 无关：前者回答"图标可见吗"，本函数回答"气泡请求发过了吗"。
     */
    bool isBubbleShown() const;

    /**
     * @brief 显示或隐藏托盘图标。
     *
     * 用户可在设置里关闭该功能；关闭时会立即隐藏图标但保留对象，以便随时再次开启。
     * 若系统不支持托盘，本函数不做任何事（并可被调用方用 isSupported() 提前拦截）。
     *
     * @param[in] enabled bool，true 显示、false 隐藏。
     * @return 无。
     * @note 显示时会同时应用最近一次 setStatus() 的内容；尚无数据时显示占位图标。
     */
    void setEnabled(bool enabled);

    /**
     * @brief 记录"关闭窗口时的气泡请求"已经发出（置位后本次运行不再发）。
     *
     * 刻意做成独立的 setter，而不是让 showMessage() 自己置位：气泡"是否已经发过"
     * 属于调用方的业务语义（主窗口的关闭流程），不是通知机制本身的属性；
     * 将来同一套气泡用于别的场景时，不会被这里的"只发一次"规则绑住。
     *
     * @param[in] shown bool，true 表示已发出过；传 false 可重置该状态
     *                （为将来可能出现的"重新提示"入口预留）。
     * @return 无。
     * @note 只改内存标志位，不做任何系统调用，可在关闭流程中安全反复调用。
     * @note 语义是"已发起"而非"已显示"：showMessage() 即发即忘、拿不到投递结果，
     *       所以调用方必须在发起后立刻置位，而不是等待一个并不存在的回执。
     */
    void setBubbleShown(bool shown);

    /**
     * @brief 用最新数据刷新图标、悬停提示与菜单标题。
     *
     * 内部会缓存上一次绘制所用的「文字 + 颜色」，两者都没变时跳过重绘，
     * 因为托盘图标重绘在 Windows 上要走一趟 HICON 转换，属于相对昂贵的操作。
     *
     * @param[in] status TrayStatus，由主窗口算好的展示数据。
     * @return 无。
     * @note 本函数可在没有托盘的环境下安全调用（仅更新内部缓存）。
     */
    void setStatus(const TrayStatus &status);

    /**
     * @brief 弹出一条气泡通知（Windows 通知区域气泡）。
     *
     * @param[in] title QString，标题。
     * @param[in] message QString，正文。
     * @param[in] warning bool，true 用警告图标、false 用信息图标。
     * @return 无。
     * @note 系统可能按用户设置屏蔽气泡；本类不重试、不阻塞。
     */
    void showMessage(const QString &title, const QString &message, bool warning = false);

    /**
     * @brief 依据占用百分比返回语义色（绿 / 琥珀 / 红）。
     *
     * 公开为静态方法是为了让任务栏角标与托盘图标共用同一套配色规则：
     * 若两处各写一份阈值，将来调整告警档位时极易只改一处而造成视觉不一致。
     *
     * @param[in] percent int，占用百分比（0~100）。
     * @return QColor，< 60 绿、60~84 琥珀、≥ 85 红。
     */
    static QColor severityColor(int percent);

signals:
    /** @brief 用户在托盘菜单里选择「显示主窗口」或双击图标。 */
    void showWindowRequested();
    /** @brief 用户在托盘菜单里选择「立即刷新」。 */
    void refreshRequested();
    /** @brief 用户在托盘菜单里选择「设置」。 */
    void settingsRequested();
    /** @brief 用户在托盘菜单里选择「退出」。 */
    void quitRequested();

private:
    /**
     * @brief 把缩略文字绘制成托盘图标位图。
     *
     * 画布固定 64×64，文字自适应字号（字符越多字号越小），这样 Windows 缩放到
     * 16/24/32 像素时有足够采样密度，视觉上比直接用 16×16 清晰。
     *
     * @param[in] text QString，要绘制的缩略文字，如 "15%"。
     * @param[in] background QColor，图标底色。
     * @param[in] valid bool，false 时绘制灰底问号占位图。
     * @return QPixmap，可直接交给 QSystemTrayIcon 的位图。
     */
    static QPixmap makeTrayPixmap(const QString &text, const QColor &background, bool valid);

    /**
     * @brief 尝试把托盘图标从 Windows 11 溢出区提升到任务栏可见区域。
     *
     * Windows 11 默认把新程序的托盘图标收进溢出区（任务栏右下角的 ^ 弹层），
     * 用户不主动设置就"看不见"。系统把每个已注册图标的可见性记在注册表
     * HKCU\Control Panel\NotifyIconSettings\<私有哈希> 下，其中 IsPromoted=1
     * 等价于系统设置里"在任务栏显示"开关打开。本函数枚举该键的子项，找到
     * ExecutablePath 与本程序 exe 一致的条目后把 IsPromoted 写为 1，
     * 并通过"先隐藏再显示"让外壳重新注册图标、立即应用新的可见性。
     *
     * @return bool，true 表示找到本程序条目且已处于/已被置为提升状态；
     *         false 表示外壳尚未为本程序建立条目（图标尚未注册成功），本次未做任何修改。
     * @note 属于 best-effort：条目由外壳在图标注册成功后创建，时机不受本进程控制，
     *       找不到条目不算错误，调用方不应据此改变托盘的启用状态。
     * @note 只写与本程序 exe 路径匹配的那个条目，绝不触碰其它程序的可见性设置。
     */
    bool promoteToVisibleTrayArea();

    /** @brief 重建悬停提示文本（套餐、剩余额度、指标明细、更新时间与错误）。 @return 无。 */
    void rebuildTooltip();

    QSystemTrayIcon *m_tray = nullptr; ///< 托盘图标本体（构造时创建，生命周期随本对象）
    QMenu *m_menu = nullptr;           ///< 右键菜单
    QAction *m_headerAction = nullptr; ///< 菜单顶部不可点击的摘要行
    TrayStatus m_status;               ///< 最近一次数据
    QString m_lastDrawnText;           ///< 上次绘制用的文字，用于跳过重复重绘
    QColor m_lastDrawnColor;           ///< 上次绘制用的底色，用于跳过重复重绘
    bool m_enabled = false;            ///< 用户是否开启了托盘图标
    bool m_bubbleShown = false;        ///< 是否已**发起过**关闭气泡（不代表系统真的显示出来；每次运行只弹一次，不持久化）
};
