// ---------------------------------------------------------------------------
//  MainWindow.h —— 主窗口类 MainWindow 的接口声明（三张卡片的控件、两个定时器与刷新入口）。
//
//  设计：界面构建 buildUi()、数据落地 applySnapshot()、时间推进 onTick() 三条职责分离；
//        构造函数只组装界面与定时器、不发网络请求，首次抓取由 startInitialRefresh() 触发。
//  依赖：只引入 UsageTypes.h 与两个 Qt 值类型，CommandCodeApi / SegmentedBar 及各控件
//        一律前置声明，以降低头文件耦合、缩短编译时间。
//  注意：命令行覆盖（m_keyOverride / m_baseUrlOverride）只作用于本次运行；用户在「设置」
//        中保存成功后覆盖即失效，全部回退到 AppConfig 提供的持久化配置。
// ---------------------------------------------------------------------------
#pragma once

// 数据模型：UsageSnapshot（一次抓取的完整结果）及其子结构体 CreditsInfo / WindowLimit 等
#include "UsageTypes.h"

// Qt 主窗口基类，以及用于保存 --base-url 覆盖值的 QUrl
#include <QMainWindow>
#include <QUrl>

// 任务栏缩略信息：本类按值持有一个 TaskbarProgress（内部只有 COM 接口指针与 HWND，
// 拷贝已被显式禁止），因此这里需要完整定义而非前置声明。
#include "TaskbarProgress.h"

// ---------------- 前置声明 ----------------
// 以下类型在本头文件中只以指针形式出现，因此只需声明、无需包含它们的定义
class CommandCodeApi;
class TrayController;
class SegmentedBar;
class QLabel;
class QPushButton;
class QTimer;
class QFrame;
class QVBoxLayout;

/**
 * @brief 应用主窗口：把一次用量快照渲染成三张卡片，并驱动周期性自动刷新。
 *
 * 窗口自上而下依次为：顶部标题栏（标题、账号、状态、刷新与设置按钮）、提示横幅、
 * 上排「套餐概览 + 本周期统计」两张卡片、下排「用量限制」一张卡片、底部错误信息区。
 * 本类不直接发起 HTTP 请求，而是通过 CommandCodeApi 的信号回填界面，
 * 因此所有控件更新都发生在 Qt 主线程的事件循环中，无需额外加锁。
 *
 * @note 所有子控件均以本窗口为父对象，由 Qt 父子所有权机制自动析构，无需手动 delete。
 * @see 各成员函数的完整实现说明见 MainWindow.cpp 中的同名函数。
 */
class MainWindow : public QMainWindow
{
    // Qt 元对象宏：使本类具备信号槽、tr() 国际化与运行时类型信息能力
    Q_OBJECT

public:
    /**
     * @brief 构造主窗口：设定窗口属性、装配界面，并创建 API 客户端与两个定时器。
     *
     * 构造阶段只做「搭骨架」的工作，顺序为：设置标题与尺寸 → 创建 CommandCodeApi 并
     * 连接其信号 → 调用 buildUi() 建立全部控件 → 创建自动刷新定时器与每秒心跳定时器。
     * 构造函数内不发任何网络请求，以免界面尚未显示就阻塞事件循环。
     *
     * @param[in] parent QWidget *，父窗口指针；默认 nullptr 表示作为独立顶层窗口显示，
     *                   传入非空时所有权交给父对象，由父对象负责析构。
     * @return 无（构造函数无返回值）。
     * @note 自动刷新定时器 m_refreshTimer 在此只创建不启动，具体间隔由 refreshNow() 按配置启动。
     */
    explicit MainWindow(QWidget *parent = nullptr);

    /**
     * @brief 启动阶段的首次刷新入口。
     *
     * 先把当前生效的 API Key 与 base-url 灌入 CommandCodeApi；若最终没有可用的 Key，
     * 则显示常驻横幅提示用户去「设置」中填写或从本机 Command Code CLI 配置导入，
     * 并保持「等待配置」状态；否则立即调用 refreshNow() 发起第一次抓取。
     *
     * @return 无。
     * @note 由 main() 在窗口 show() 之后调用，避免在构造期就触发网络请求。
     */
    void startInitialRefresh();

    /**
     * @brief 记录命令行传入的临时凭证覆盖（--key / --base-url）。
     *
     * 覆盖值只保存在内存成员 m_keyOverride / m_baseUrlOverride 中，不写入任何配置文件；
     * 用户在「设置」对话框里保存后，openSettings() 会清空这两个成员，覆盖随即失效。
     *
     * @param[in] apiKey QString，命令行给出的 API Key，内部会 trim 去掉首尾空白；空串表示不覆盖。
     * @param[in] baseUrl QUrl，命令行给出的服务地址；无效或为空都表示不覆盖。
     * @return 无。
     * @note 必须在 startInitialRefresh() 之前调用，否则首次抓取用不到覆盖值。
     */
    void setCredentialsOverride(const QString &apiKey, const QUrl &baseUrl);

    /**
     * @brief 输出桌面集成的运行时状态，供 `--probe-ui` 自检取证。
     *
     * 报告的内容刻意选择"可被外部核对的事实"，而不是简单的一句 OK：
     * 窗口是否可见、托盘是否被系统支持、用户意图与图标实际可见状态、
     * 任务栏 COM 是否已绑定、以及"关窗即退出"的当前取值。
     *
     * @return QString，多行 "键 = 值" 文本，每行一个事实。
     * @note 之所以设为 public：自检分支在 main() 中持有 MainWindow 栈对象，
     *       需要直接读取该报告，而非通过信号绕行。
     */
    QString desktopProbeReport() const;

    /**
     * @brief 把窗口从托盘/隐藏状态恢复到前台。
     *
     * 供两处调用：托盘图标的双击/单击，以及**第二个实例**通过本地套接字发来的
     * "SHOW" 命令。后者是关键逃生通道——Windows 11 会把新托盘图标收进溢出区，
     * 用户可能根本找不到图标；此时只要再运行一次本程序，就能把窗口唤回。
     *
     * @return 无。
     * @note showNormal() 必须在 raise() 之前：窗口处于最小化状态时直接 raise() 不会还原。
     */
    void restoreFromTray();

    /**
     * @brief 请求退出进程（绕过"关闭到托盘"策略）。
     *
     * 供命令行 `--quit` 与第二个实例的 "QUIT" 命令调用。会先置 m_forceQuit，
     * 使 closeEvent 直接放行，避免退出动作又被自身的隐藏策略拦回托盘。
     *
     * @return 无。
     * @note 这是"进程已在后台但找不到托盘图标"时的第二种逃生手段。
     */
    void requestQuit();

private slots:
    /**
     * @brief 立即刷新：校验凭证、按配置重启自动刷新计时器，并发起一次异步抓取。
     *
     * 每次刷新都会重新读取 AppConfig，使「设置」里刚改过的地址与间隔立即生效；
     * 随后把状态切到忙碌态并调用 CommandCodeApi::fetchAll()，结果经信号异步回到本类。
     *
     * @return 无。
     * @note 无可用 API Key 时直接返回并弹出常驻横幅，不会发出请求。
     */
    void refreshNow();

    /**
     * @brief 打开「设置」对话框（模态），保存成功后立即刷新并让命令行覆盖失效。
     * @return 无。
     * @note 对话框以栈对象方式创建，exec() 返回后即结束生命周期，无需手动释放。
     */
    void openSettings();

    /**
     * @brief 抓取成功槽：结束忙碌态并把快照交给 applySnapshot() 渲染。
     * @param[in] snapshot const UsageSnapshot &，本次抓取的完整结果。
     * @return 无。
     */
    void onSnapshotReady(const UsageSnapshot &snapshot);

    /**
     * @brief 抓取失败槽：恢复刷新按钮可用性，并把错误信息显示在窗口底部的错误区。
     * @param[in] message QString，来自 CommandCodeApi 的错误描述文本。
     * @return 无。
     */
    void onFailed(const QString &message);

    /**
     * @brief 每秒心跳槽：递减自动刷新倒计时，并重算限流窗口的重置文案。
     * @return 无。
     * @note 由 m_tickTimer 以 1000 ms 固定间隔驱动，是界面上唯一的时间推进来源。
     */
    void onTick();

private:
    /**
     * @brief 创建一张带标题的卡片容器（QFrame），并把其正文布局回传给调用方。
     *
     * 卡片统一设置 objectName 为 "card" 以套用圆角边框样式，纵向设为 Expanding
     * 以便随窗口长高、消除底部留白；标题自动转大写以贴合卡片标题的视觉规范。
     *
     * @param[in]  title QString，卡片标题，函数内部会 toUpper() 后再显示。
     * @param[out] bodyOut QVBoxLayout **，用于回传卡片正文布局指针；可为 nullptr，
     *                     此时调用方拿不到正文布局（本项目中始终传非空）。
     * @return QFrame *，新建的卡片外框；父对象在 addWidget 时由布局接管，无需手动释放。
     * @note 正文布局在标题标签之下，拥有独立的间距设置，方便各卡片自行排布内容。
     */
    QFrame *createCard(const QString &title, QVBoxLayout **bodyOut);

    /**
     * @brief 一次性搭建整个窗口的控件树与样式表。
     *
     * 依次构建根布局、顶部标题栏、提示横幅、上排两张卡片（套餐概览 / 本周期统计）、
     * 下排「用量限制」卡片（5 小时、每周、每月三块分段进度条），最后套用全局样式表。
     *
     * @return 无。
     * @note 只在构造函数中调用一次；重复调用会重复创建控件并造成泄漏式堆积。
     */
    void buildUi();

    /**
     * @brief 把一次成功抓取的快照渲染到全部控件上（纯界面更新，不发网络请求）。
     *
     * 最绕的是额度口径：credits.monthlyRemaining 是「剩余」而非「已用」，
     * 因此已用额度优先由「套餐总额 − 剩余」反推，套餐未知时才退回 summary.totalCost。
     *
     * @param[in] snapshot const UsageSnapshot &，一次抓取的完整结果；各子结构体自带
     *                     valid 标志，无效字段按「—」或 0 处理，调用方无需预判。
     * @return 无。
     */
    void applySnapshot(const UsageSnapshot &snapshot);

    /**
     * @brief 切换忙碌态：抓取期间禁用刷新按钮，结束后恢复并刷新倒计时文案。
     * @param[in] busy bool，true 表示正在抓取（按钮置灰、状态显示「正在刷新…」）。
     * @return 无。
     */
    void setBusy(bool busy);

    /**
     * @brief 把状态栏切换为「查询失败：<原因>」，并持续显示到下一次刷新发起。
     *
     * 失败状态由 m_lastRefreshFailed / m_lastFailureReason / m_lastFailureDetails
     * 三者共同记录：updateCountdown() 每秒心跳都会调用本函数维持文案，
     * 否则失败结论会在一秒内被「下次刷新」倒计时覆盖。
     *
     * @param[in] reason QString，状态栏里展示的精简原因。
     * @param[in] details QString，悬停提示里的完整明细；空串时与 reason 相同。
     * @return 无。
     * @note 颜色走语义级别 danger；只改状态栏，不动任何数据控件。
     */
    void showFailure(const QString &reason, const QString &details);

    /**
     * @brief 清除失败状态：复位标志位、悬停提示与语义级别。
     * @return 无。
     * @note 在快照无错误（成功）时由 applySnapshot() 调用。
     */
    void clearFailure();

    /**
     * @brief 显示顶部提示横幅，并设置其配色与是否常驻。
     *
     * 横幅分两类：常驻型（如未配置 API Key）与一次性提示（如「API Key 已保存」）；
     * 后者会在下一次成功抓取后被 applySnapshot() 自动隐藏。
     *
     * @param[in] text QString，横幅正文。
     * @param[in] warning bool，true 用橙色警示配色，false 用绿色成功配色。
     * @param[in] sticky bool，true 表示常驻（不随抓取成功自动消失），默认 false。
     * @return 无。
     * @note 该函数会写入成员 m_bannerSticky，是少数具有副作用的界面函数之一。
     */
    void showBanner(const QString &text, bool warning, bool sticky = false);

    /**
     * @brief 刷新状态栏上的「下次刷新」时刻文案（北京时间，每次刷新后随之更新）。
     * @return 无。
     * @note 刷新按钮处于禁用态（正在抓取）时直接返回，避免覆盖「正在刷新…」提示。
     */
    void updateCountdown();

    /**
     * @brief 计算当前生效的 API Key：命令行覆盖优先，否则回退到持久化配置。
     * @return QString，可直接交给 CommandCodeApi 使用的 Key；可能为空（表示尚未配置）。
     */
    QString effectiveApiKey() const;

    /**
     * @brief 计算当前生效的 base-url：命令行覆盖优先，否则回退到持久化配置。
     * @return QUrl，有效的服务地址；覆盖值无效或为空时返回 AppConfig::baseUrl()。
     */
    QUrl effectiveBaseUrl() const;

    /**
     * @brief 把重置时刻格式化为「重置还剩 Xd Xh / Xh Xm / Xm」样式的倒计时文案。
     * @param[in] resetAt const QDateTime &，限流窗口的重置时刻；无效时返回「重置时间未知」。
     * @return QString，面向用户的中文倒计时文案。
     * @note 静态函数，不依赖任何成员状态，便于在 lambda 中直接调用。
     */
    static QString formatCountdown(const QDateTime &resetAt);

    /**
     * @brief 把时间点格式化为「yyyy-MM-dd HH:mm」。
     * @param[in] dt const QDateTime &，待格式化的时间点；无效时返回占位符「—」。
     * @return QString，格式化后的时间文本。
     */
    static QString formatDateTime(const QDateTime &dt);

    /**
     * @brief 把 token 数量压缩成便于阅读的 K / M 单位文本。
     * @param[in] tokens qint64，原始 token 数。
     * @return QString，≥100 万显示两位小数的 M，≥1000 显示一位小数的 K，否则原样输出整数。
     */
    static QString formatTokens(qint64 tokens);

protected:
    /**
     * @brief 覆写关闭事件：按「关闭时最小化到托盘」的设置决定隐藏还是退出。
     *
     * 只有同时满足「托盘确实可用且已开启」与「用户勾选了关闭到托盘」两个条件时，
     * 才会隐藏窗口并忽略本次关闭；否则一律按默认行为退出。这一约束是刻意的：
     * 若在托盘不可用时吞掉关闭事件，用户将面对一个"关不掉且无入口"的幽灵进程。
     *
     * @param[in] event QCloseEvent *，关闭事件对象；隐藏时调用 ignore()。
     * @return 无。
     * @note 托盘菜单的「退出」会先置 m_forceQuit，从而绕过本函数的隐藏分支。
     */
    void closeEvent(QCloseEvent *event) override;

    /**
     * @brief 覆写显示事件：窗口首次显示后把任务栏集成绑定到本窗口的 HWND。
     *
     * Windows 的任务栏接口要求目标窗口的任务栏按钮已经存在，因此绑定动作
     * 必须放在 show() 之后；此处用一次性的惰性绑定，避免重复创建 COM 对象。
     *
     * @param[in] event QShowEvent *，显示事件对象，直接交给基类处理。
     * @return 无。
     */
    void showEvent(QShowEvent *event) override;

private:
    /**
     * @brief 缩略信息视图：把一次快照折算成托盘/任务栏要展示的那一项指标。
     *
     * 数值口径与主界面完全一致：占用率一律 floor(已用 / 上限 × 100)，分母按所选
     * 指标取 5 小时上限、每周上限或套餐总额；选择「剩余额度」时则以剩余额度为主。
     */
    struct MetricView
    {
        QString name;      ///< 指标名称，如「5 小时限额」
        bool    valid = false;   ///< 快照里是否含有该项数据
        int     percent = 0;     ///< 0~100 的占用率，用于进度条与配色
        QString text;      ///< 画进托盘图标/任务栏角标的缩略文字，如 "15%"
        QString detail;    ///< 悬停提示里的明细，如「已用 2.24 / 14　重置还剩 2h 39m」
    };

    /**
     * @brief 依据当前配置的指标，把快照折算成 MetricView。
     * @param[in] snapshot const UsageSnapshot &，最近一次成功抓取的数据。
     * @return MetricView，可直接喂给托盘与任务栏。
     * @note 本函数自行重算套餐总额与已用额度，与 applySnapshot() 存在少量重复计算；
     *       这样做的代价是几次浮点运算，换来的是"缩略信息口径"可被单独理解与测试。
     */
    MetricView metricView(const UsageSnapshot &snapshot) const;

    /**
     * @brief 把最新指标推送到托盘图标与任务栏（含开关判断）。
     * @param[in] snapshot const UsageSnapshot &，本次快照，用于补齐套餐名与剩余额度等上下文。
     * @param[in] metric const MetricView &，由 metricView() 折算出的展示数据。
     * @return 无。
     * @note 用户关闭任务栏开关或未连接任务栏时，会主动清理进度条与角标，
     *       而不是留着上一次的旧值。
     */
    void updateDesktopIndicators(const UsageSnapshot &snapshot, const MetricView &metric);

    /**
     * @brief 依据 AppConfig 的桌面集成配置，应用托盘开关、任务栏开关与退出行为。
     *
     * 关键点在于"是否允许关闭窗口后继续驻留"必须同时满足三个条件：托盘可用、
     * 用户开启托盘、用户开启关闭到托盘。只要有一条不满足，就恢复
     * setQuitOnLastWindowClosed(true)，保证窗口一关进程就退出。
     *
     * @return 无。
     * @note 在构造末尾与每次「设置」保存后各调用一次，保证运行期改动即时生效。
     */
    void applyDesktopIntegrationSettings();

    // ---------------- 核心依赖与刷新计时 ----------------
    // 网络客户端：本类只连接它的信号，绝不直接触碰 QNetworkAccessManager
    CommandCodeApi *m_api = nullptr;              ///< 用量抓取客户端，父对象为 this，随窗口析构
    QTimer *m_refreshTimer = nullptr;             ///< 自动刷新定时器，间隔取自 AppConfig::refreshSeconds()
    QTimer *m_tickTimer = nullptr;                ///< 每秒心跳定时器，驱动倒计时与重置文案
    int m_secondsToRefresh = 0;                   ///< 距下次自动刷新的剩余秒数，0 表示未启用
    QDateTime m_nextRefreshAt;                    ///< 下次自动刷新的目标时刻（本机时区存储，展示时换算北京时间）；无效表示尚未排程
    QString m_keyOverride;                        ///< 命令行 --key 的临时覆盖，空串表示不覆盖
    QUrl m_baseUrlOverride;                       ///< 命令行 --base-url 的临时覆盖，无效或空表示不覆盖

    // ---------------- 顶部提示与状态 ----------------
    QLabel *m_banner = nullptr;                   ///< 顶部提示横幅，常驻型与一次性提示共用
    bool m_bannerSticky = false;                  ///< true = 常驻提示（如未配置 API Key），false = 拿到数据后自动消失
    QLabel *m_status = nullptr;                   ///< 右上角状态文案：正在刷新 / 下次自动刷新 / 最后更新 / 查询失败
    QPushButton *m_refreshButton = nullptr;       ///< 「刷新」按钮，抓取期间被 setBusy() 置灰

    // 套餐卡
    QLabel *m_planName = nullptr;                 ///< 套餐名称（大字号），来自 PlanCatalog::displayName
    QLabel *m_planStatus = nullptr;               ///< 订阅状态，如 active，以及是否周期末取消
    QLabel *m_planPeriod = nullptr;               ///< 计费周期的起止时间区间，未知时显示「未知」
    QLabel *m_creditsValue = nullptr;             ///< 「剩余 / 套餐总额 credits」大字数值
    QLabel *m_creditsDetail = nullptr;            ///< 已用额度与百分比，必要时追加加购 / 赠送额度
    SegmentedBar *m_creditsBar = nullptr;         ///< 套餐额度分段进度条，分母为套餐总额

    // 限流卡
    QLabel *m_fiveHourPercent = nullptr;          ///< 5 小时窗口已用百分比文字，按档位着色
    QLabel *m_fiveHourReset = nullptr;            ///< 5 小时窗口重置倒计时（每秒重算）
    SegmentedBar *m_fiveHourBar = nullptr;        ///< 5 小时窗口分段进度条，分母为 5 小时上限
    QLabel *m_weeklyPercent = nullptr;            ///< 每周窗口已用百分比文字，按档位着色
    QLabel *m_weeklyReset = nullptr;              ///< 每周窗口重置倒计时（每秒重算）
    SegmentedBar *m_weeklyBar = nullptr;          ///< 每周窗口分段进度条，分母为每周上限
    QLabel *m_monthlyPercent = nullptr;           ///< 每月窗口已用百分比文字，与套餐额度同口径
    QLabel *m_monthlyReset = nullptr;             ///< 每月窗口的重置说明，直接展示计费周期结束时间
    SegmentedBar *m_monthlyBar = nullptr;         ///< 每月窗口分段进度条，分母为套餐总额

    // 统计卡
    QLabel *m_runsValue = nullptr;                ///< 本周期运行次数（大字号）
    QLabel *m_successValue = nullptr;             ///< 成功 / 失败次数
    QLabel *m_successRate = nullptr;              ///< 成功率（独立灰字行，与左卡「计费周期」同位）
    QLabel *m_tokensValue = nullptr;              ///< 本周期 token 总用量（大字号，K/M 缩写）
    QLabel *m_tokensDetail = nullptr;             ///< 输入与输出 token 的分项数值
    QLabel *m_costValue = nullptr;                ///< 已消耗 credits 与单次平均成本（与左卡进度条同位等高）

    // ---------------- 刷新失败状态（状态栏「查询失败」文案的数据源） ----------------
    bool m_lastRefreshFailed = false;             ///< 上一次刷新是否失败；失败时状态栏持续显示原因
    QString m_lastFailureReason;                  ///< 状态栏展示的精简原因（一行以内）
    QString m_lastFailureDetails;                 ///< 悬停提示展示的完整明细（多接口逐条）
    QLabel *m_account = nullptr;                  ///< 当前账号展示，优先用户名，其次邮箱、用户 ID

    // ---------------- 桌面集成（托盘 / 任务栏）----------------
    TrayController *m_tray = nullptr;             ///< 通知区域图标控制器，父对象为 this
    TaskbarProgress m_taskbar;                    ///< 任务栏进度条与角标，按值持有（不可拷贝）
    bool m_forceQuit = false;                     ///< true 表示用户明确要求退出，closeEvent 需放行
    MetricView m_lastMetric;                      ///< 最近一次折算出的指标，供自检报告读取
    bool m_hasMetric = false;                     ///< m_lastMetric 是否已被真实数据填充过
};
