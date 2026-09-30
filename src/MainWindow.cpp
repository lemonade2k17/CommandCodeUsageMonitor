// ---------------------------------------------------------------------------
//  MainWindow.cpp —— 主窗口的实现：界面构建、快照渲染、定时刷新与倒计时文案维护。
//
//  数据流：startInitialRefresh() → CommandCodeApi::fetchAll()（异步）
//          → snapshotReady 信号 → onSnapshotReady() → applySnapshot() 回填控件。
//  时间流：m_tickTimer 每秒触发 onTick()，既递减自动刷新倒计时，
//          也重算「5 小时 / 每周」两个限流窗口的「重置还剩 Xd Xh」文案。
//  百分比口径：统一为 pct = floor(used / cap × 100)，分母依次是 5 小时上限、每周上限、
//          套餐总额（PlanCatalog::totalCredits）；注意 credits.monthlyRemaining 是
//          「剩余」而不是「已用」，已用额度必须由「套餐总额 − 剩余」反推。
//  线程模型：全部逻辑运行在 Qt 主线程。CommandCodeApi 内部完成异步请求后通过信号回调，
//          因此 applySnapshot() 无需加锁；也正因为如此，界面绝不会被阻塞。
//  所有权：所有 new 出来的控件都显式挂在窗口或布局之下，由 Qt 对象树统一析构；
//          本类有意不写析构函数，避免手动 delete 与 Qt 自动析构重复释放。
//  样式约定：控件的 objectName 是唯一的选择器入口，改样式只改 buildUi() 末尾的样式表，
//          不在业务代码里零散地设置字号颜色（横幅配色是唯一的例外）。
//  注意：本文件不写任何配置，命令行覆盖仅存在于内存；所有控件由 Qt 父子机制析构。
//
//  桌面集成（通知区域 / 任务栏）子系统的约定：
//    · 本文件是"策略层"：只决定要不要显示、显示哪一项、数据无效时如何退化；
//      真正的 Win32 / COM 调用全部封装在 TrayController 与 TaskbarProgress 内部。
//    · 桌面集成配置是"运行期可变"的：用户每次在「设置」里保存后都必须重新走一遍
//      applyDesktopIntegrationSettings()，绝不允许只在启动时读一次。
//    · 「窗口能否消失」与「进程能否驻留」是同一件事的两面：任何一次策略变更都必须
//      保证"要么有托盘入口、要么窗口可见"，否则就是在制造关不掉的幽灵进程。
//    · 自检取证（--probe-ui）只报告事实、不下结论：程序认为"已经生效"与 shell
//      实际"看得见"是两回事，报告必须能让外部把这两者区分开。
//    · 任务栏调用在没有桌面会话（服务上下文、被策略禁用）时必然失败，因此失败路径
//      一律静默跳过并记录 HRESULT，绝不能冒泡成界面错误影响刷新主流程。
// ---------------------------------------------------------------------------

// 本类声明
#include "MainWindow.h"

#include "AppTheme.h"

// 持久化配置：API Key / base-url / 自动刷新间隔的唯一权威来源
#include "AppConfig.h"
// 用量抓取客户端：本文件只连接其信号，不直接触碰网络层
#include "CommandCodeApi.h"
// 分段进度条：套餐额度与三个限流窗口共用的自绘控件
#include "SegmentedBar.h"
// 设置对话框：以模态方式运行，保存成功回到本窗口后立即触发一次刷新
#include "SettingsDialog.h"
// 通知区域图标控制器：把缩略信息推到 Windows 托盘，并把菜单动作回传为信号
#include "TrayController.h"

// Qt 时间类型与控件；布局类统一来自 QtWidgets
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
// QTimeZone：把「下次刷新」目标时刻换算成北京时间展示（Asia/Shanghai 解析失败时有兜底）。
#include <QTimeZone>
#include <QVBoxLayout>
// std::floor：百分比必须向下取整，才能与官网展示口径完全一致
#include <cmath>

// ===========================================================================
//  本文件私有的辅助设施：不导出符号，仅供本翻译单元内的界面逻辑使用
// ===========================================================================
namespace
{

    /**
     * @brief 返回"北京时间"对应的时区对象。
     *
     * 状态栏的「下次刷新」时刻按北京时间展示：优先解析 IANA 名称 Asia/Shanghai，
     * 拿到完整的时区规则；解析失败（极少数精简系统缺时区数据）时退回固定
     * UTC+8 偏移——北京时间不实行夏令时，固定偏移在本场景下同样正确。
     *
     * @return QTimeZone，可直接用于 QDateTime::toTimeZone 的时区对象。
     * @note 结果以函数级静态变量缓存：QTimeZone 的构造需要查询系统时区数据库，
     *       而本函数处在每秒一次的心跳路径上，缓存可省去重复解析的开销。
     */
    QTimeZone beijingTimeZone()
    {
        // C++11 起函数级 static 初始化线程安全；本程序全部逻辑在主线程，更无竞争
        static const QTimeZone cached = []()
        {
            // IANA 名称由 Qt 在 Windows 上经系统时区机制解析；构造函数收 QByteArray 形式的时区 ID
            const QTimeZone iana(QStringLiteral("Asia/Shanghai").toLatin1());
            if (iana.isValid())
                return iana;
            // 兜底：固定偏移 +8 小时（28800 秒），语义即"东八区标准时间"
            return QTimeZone(8 * 3600);
        }();
        return cached;
    }

} // namespace

/**
 * @brief 构造主窗口：设定窗口属性，装配界面，并创建 API 客户端与两个定时器。
 *
 * 执行顺序刻意安排为「先建 API 客户端并连接信号 → buildUi() 造控件 → 最后启动心跳定时器」，
 * 这样 buildUi() 期间所有控件都已存在，即便信号被提前触发也不会访问到空指针。
 * 构造函数内不发任何网络请求，首次抓取由 main() 在窗口显示后调用 startInitialRefresh() 触发。
 *
 * @param[in] parent QWidget *，父窗口指针；默认 nullptr 表示作为独立顶层窗口显示，
 *                   传入非空时所有权交给父对象，由父对象负责析构。
 * @return 无（构造函数无返回值）。
 * @note CommandCodeApi、m_refreshTimer、m_tickTimer 均以 this 为父对象，随本窗口一起销毁，
 *       因此本类无需编写析构函数来手动 delete。
 * @note 托盘控制器 TrayController 同样挂在 this 之下，但它的「显示窗口 / 刷新 / 设置 /
 *       退出」四项动作全部以信号回到本类处理，托盘模块自身不持有任何窗口状态。
 * @note 构造末尾调用一次 applyDesktopIntegrationSettings()，把持久化的桌面集成配置
 *       落到运行时对象上；此时窗口尚未 show()，任务栏的 COM 绑定要等 showEvent()。
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // 窗口标题与产品名保持一致，方便任务栏与窗口管理器识别
    setWindowTitle(tr("Command Code 套餐用量"));
    // 默认尺寸贴合内容高度；再小就压不下三张卡片，故给出最小尺寸
    resize(1000, 500);
    // 最小尺寸略小于默认尺寸：允许用户适度缩小，但保证三张卡片不被压扁变形
    setMinimumSize(900, 440);

    // 网络客户端挂在 this 之下，窗口销毁时自动断开信号连接并释放
    m_api = new CommandCodeApi(this);
    // 信号槽使用默认的 AutoConnection：发送者与接收者同处主线程，回调会直接执行，
    // 不存在跨线程排队与数据竞争，因此本文件中的槽函数都不需要加锁。
    // 抓取成功：把完整快照交给 onSnapshotReady() 统一渲染
    connect(m_api, &CommandCodeApi::snapshotReady, this, &MainWindow::onSnapshotReady);
    // 抓取失败：交给 onFailed() 恢复按钮可用性并展示错误信息
    connect(m_api, &CommandCodeApi::failed, this, &MainWindow::onFailed);

    // 界面必须在信号连接之后、心跳启动之前建好，保证槽函数访问到的控件指针全部非空
    buildUi();

    // 自动刷新定时器：此处只创建与连接，是否启用及间隔由 refreshNow() 按配置决定
    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshNow);

    // 心跳与自动刷新是两个彼此独立的定时器：前者只驱动倒计时文案，后者才真正发请求；
    // 这样即便用户关掉了自动刷新，界面上的倒计时与重置提示依旧保持鲜活。
    // 心跳定时器固定 1 秒一拍，保证「下次自动刷新」与「重置还剩」文案每秒都在跳动
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, &MainWindow::onTick);
    // 立即启动：即使尚未配置 API Key，状态栏文案也需要随时间继续刷新
    m_tickTimer->start();

    // 通知区域图标：构造它只是创建对象与菜单，是否显示由下面的配置应用决定。
    // 菜单里的四项动作全部以信号转回本类，托盘模块自身不直接操作窗口。
    // 之所以放在 buildUi() 之后：唤出窗口的动作会调用 showNormal()，必须保证那时
    // 控件树已经完整，否则用户会看到一闪而过的半成品窗口。
    m_tray = new TrayController(this);
    connect(m_tray, &TrayController::showWindowRequested, this, [this]()
            {
        // 从托盘唤出窗口：先恢复常规显示，再提升到前台，避免被其它窗口压在下面
        // 三步缺一不可：showNormal() 负责从"隐藏 / 最小化"两种状态恢复，raise() 调整
        // Z 序，activateWindow() 再把键盘焦点抢过来；只做第一步时，Windows 常常只让
        // 任务栏图标闪一下，窗口依旧停在其它程序后面。
        // 这里不重新推送任务栏数据：用量本身没有变化，任务栏由
        // updateDesktopIndicators() 在每次抓取后统一维护，唤出动作不该引出第二份数据源。
        // 直接复用公开方法 restoreFromTray()，使"托盘双击唤出"与"第二个实例唤回"
        // 走完全相同的三步，避免两处实现将来各自漂移。
        restoreFromTray(); });
    // 刷新与设置直接连到已有的槽，而不额外包一层 lambda：托盘菜单与窗口顶部按钮
    // 走完全相同的代码路径，行为天然一致，也避免同一逻辑出现两处实现。
    connect(m_tray, &TrayController::refreshRequested, this, &MainWindow::refreshNow);
    connect(m_tray, &TrayController::settingsRequested, this, &MainWindow::openSettings);
    // 退出动作直接复用公开方法，与命令行 `--quit`、第二个实例的 QUIT 命令走同一条路径；
    // requestQuit() 内部的"先置 m_forceQuit 再 quit()"是退出不被自身关闭策略拦回的
    // 关键，这三处入口共用同一实现，避免将来只改一处导致退出行为不一致。
    connect(m_tray, &TrayController::quitRequested, this, &MainWindow::requestQuit);

    // 应用持久化的桌面集成配置（托盘开关、任务栏开关、关闭行为、展示指标）
    // 构造末尾统一应用一次，是"启动时读配置"的唯一入口：托盘显示、任务栏开关与
    // 关闭行为三者的初始状态都来自同一份已落盘的配置，而不是各自的硬编码默认值。
    applyDesktopIntegrationSettings();
}

/**
 * @brief 把桌面集成配置应用到运行时对象上。
 *
 * 三条独立规则：
 *   ① 托盘图标：仅在系统支持且用户开启时显示；
 *   ② 窗口关闭行为：只有「托盘可用 + 用户开启托盘 + 用户勾选关闭到托盘」三者同时
 *      成立，才把 setQuitOnLastWindowClosed 置为 false，其余情况一律保证窗口一关就退出；
 *   ③ 任务栏缩略信息：用户关闭时立刻清空进度与角标，避免残留旧值。
 *
 * @return 无。
 * @note 被构造函数与 openSettings() 各调用一次，是"设置改了要立刻生效"的唯一入口。
 * @note 三条规则之间存在依赖顺序：必须先确定托盘是否可用，才能判断"允许关窗后驻留"
 *       是否成立；不能把这两件事拆到两个函数里各自读配置，否则判定依据会不一致。
 * @note 本函数是幂等的：重复调用只会用同样的配置重算一遍，不累积任何状态，
 *       因此可以在每次保存设置后无条件调用。
 */
void MainWindow::applyDesktopIntegrationSettings()
{
    // 托盘是否真正可用：系统不支持托盘时，即便用户勾了开关也不能让它生效
    // 先算"托盘到底可不可用"，再看用户的意愿：系统根本不提供通知区域（精简版系统、
    // 被组策略禁用的会话）时，用户勾了什么都不能算数——否则后续所有依赖托盘可用性的
    // 判断都会连锁失真，最终演变成关掉窗口就再也找不到进程。
    const bool trayUsable = m_tray && m_tray->isSupported();
    const bool trayOn = trayUsable && AppConfig::trayEnabled();
    if (m_tray)
        m_tray->setEnabled(trayOn);
    // 把"用户开关"落在控制器上，而不是每次现读配置：控制器是运行期唯一的状态源，
    // closeEvent() 只问控制器开没开，因此不会出现"配置说开着、控制器说关着"的分裂。

    // 只有"能躲进托盘"时才允许窗口关闭后继续驻留；否则必须恢复"关窗即退出"，
    // 否则会出现用户关掉窗口、进程却还在后台且没有任何入口的幽灵进程。
    // 三个条件必须同时成立，缺一即视为"不能隐藏"：
    //   trayUsable = false → 藏起来就再也叫不出来，只能靠任务管理器结束进程；
    //   trayOn     = false → 用户自己关掉了托盘图标，隐藏窗口同样是死路一条；
    //   closeToTray= false → 用户明确要求"关闭即退出"，必须尊重这个选择。
    const bool canHideToTray = trayOn && AppConfig::closeToTray();
    QApplication::setQuitOnLastWindowClosed(!canHideToTray);
    // 每次都显式赋值（而不是只在允许隐藏时置 false）：setQuitOnLastWindowClosed 是
    // 进程级全局开关，上一次的取值会一直留着；若只在允许隐藏时写 false，用户随后
    // 关掉托盘开关时就没人再把它改回 true，进程又变成关不掉的幽灵。

    // 用户关闭任务栏开关时主动清理，而不是留着一个不再更新的旧进度条
    // 清理只处理"用户关掉了开关"这一种情况：其余情况交给
    // updateDesktopIndicators() 用最新数据覆盖，避免两个函数在这里互相打架。
    if (!AppConfig::taskbarBadgeEnabled())
        m_taskbar.clearAll();
}

/**
 * @brief 显示事件：窗口首次显示后把任务栏集成绑定到本窗口。
 *
 * 绑定放在 show() 之后是 Windows 的硬性要求——任务栏按钮必须先存在，
 * SetProgressValue / SetOverlayIcon 才会生效；因此这里用 m_taskbar.isAttached()
 * 做一次性惰性绑定，避免每次显示都重建 COM 对象。
 *
 * @param[in] event QShowEvent *，显示事件；先交给基类完成默认处理。
 * @return 无。
 * @note 绑定时机由 Windows 决定而不是由本类决定：任务栏按钮必须先存在，
 *       SetProgressValue / SetOverlayIcon 才会被接受，因此只能在 show() 之后绑定。
 * @note 本函数可能被多次调用（窗口隐藏后再唤出），幂等性由 m_taskbar.isAttached() 保证。
 */
void MainWindow::showEvent(QShowEvent *event)
{
    // 先交给基类：它会处理窗口激活、焦点分配与尺寸恢复等默认行为。绑定动作放在其后，
    // 既满足"任务栏按钮必须已存在"的前提，也避免基类逻辑被后续的提前返回跳过。
    QMainWindow::showEvent(event);
    // 惰性绑定，并且只绑一次：
    //   · Windows 的任务栏按钮在窗口首次 show() 之前并不存在，构造函数里绑定必然失败；
    //   · 每次显示都重新绑定会重复创建 COM 对象（CoCreateInstance + HrInit）并反复增加
    //     引用计数，用户频繁隐藏 / 唤出窗口时这部分资源会持续增长且无从释放；
    //   · 因此用 isAttached() 当哨兵：第一次显示时真正绑定，此后只做一次廉价的读取。
    if (!m_taskbar.isAttached())
        m_taskbar.attachTo(this);
    // 绑定失败（无桌面会话、任务栏被策略禁用）时不做任何补救动作：失败原因由
    // TaskbarProgress 记成 HRESULT，desktopProbeReport() 会如实报出来，
    // 界面本身照常工作——任务栏只是锦上添花，不该拖累刷新主流程。
}

/**
 * @brief 关闭事件：按配置决定"隐藏到托盘"还是"真正退出"。
 *
 * 判定条件与 applyDesktopIntegrationSettings() 保持一致（托盘可用 + 已开启 +
 * 用户勾选关闭到托盘），三者缺一就走默认退出路径，杜绝关不掉的进程。
 *
 * @param[in] event QCloseEvent *，关闭事件；隐藏时调用 ignore() 阻止关闭。
 * @return 无。
 * @note 托盘菜单的「退出」会先把 m_forceQuit 置真，从而直接走 accept() 分支。
 * @note 判定依据之一是控制器的 isEnabled() 而不是再读一次配置：控制器是本进程里
 *       "托盘此刻是否开着"的唯一状态源，重读配置等于引入两套可能不一致的依据。
 * @note 若不加条件地吞掉关闭事件，用户就会面对一个既关不掉、又没有任何入口的进程，
 *       只能去任务管理器结束它——这正是本函数刻意避免的"幽灵进程"。
 */
void MainWindow::closeEvent(QCloseEvent *event)
{
    // 三个条件是与关系，缺一即走默认的"接受关闭"：
    //   ① !m_forceQuit：优先级最高。用户在托盘菜单里选了「退出」，这是明确的终止意图，
    //      必须跳过隐藏分支，否则进程永远退不掉——也就是"关不掉的幽灵进程"；
    //   ② m_tray->isEnabled()：托盘此刻真的开着（已同时满足"系统支持 + 用户开启"），
    //      窗口藏起来之后用户有办法把它叫回来，而不是彻底失去入口；
    //   ③ closeToTray()：用户在设置里勾选过"关闭时最小化到托盘"，隐藏符合其预期。
    // 只要②或③不成立，窗口一旦 hide() 就会"既看不见、也召不回"，因此宁可按默认行为
    // 退出，也不去制造一个没有任何入口的后台进程。
    if (!m_forceQuit && m_tray && m_tray->isEnabled() && AppConfig::closeToTray())
    {
        // 先 hide() 再 ignore()：ignore() 只负责撤销本次关闭，把窗口藏起来这件事
        // 仍要显式调用 hide()；若只 ignore() 而不 hide()，窗口会原地不动，
        // 用户会以为"点了关闭却没反应"。
        hide();
        // 忽略本次关闭：窗口只是藏起来，进程继续在托盘里监控用量
        event->ignore();
        // 关闭时弹一次气泡提示，且**每次运行只弹一次**（标志位记在 TrayController 内）：
        //   · 为什么第一次要弹：勾选"关闭时最小化到托盘"的用户多半刚用上这个功能，
        //     此刻最需要知道"窗口没退出、点托盘图标能叫回来"，弹一次即完成告知；
        //   · 为什么只弹一次：窗口被关闭按钮截留、自己藏起来，这件事本身已足以说明
        //     "窗口没退出"；每次关闭都弹会变成对频繁开关窗口者的反复打扰；
        //   · 发出去就不管：QSystemTrayIcon::showMessage() 是"即发即忘"的——它不返回
        //     投递结果，本程序也无从探测勿扰模式 / 通知权限是否把气泡吃掉了。
        //     因此这里既不做显示结果检测，也不补弹：补弹一次比漏弹更烦人。
        //   · 作用域限本次运行：标志位只在内存里，程序重启后的第一次关闭会再提示一次
        //     （"装一次只提示一次"需要把该状态落进配置文件，目前刻意没这么做）。
        if (!m_tray->isBubbleShown())
        {
            // 标题用简短的「通知」：Windows 会在气泡上方自动显示应用名，标题再写产品名会重复。
            m_tray->TrayController::showMessage(tr("通知"),
                                                tr("窗口已隐藏到托盘，点击托盘图标可唤回"),
                                                false);
            // 无论系统是否真的把气泡画出来都立即置位：否则在"通知被系统关闭"的机器上，
            // 每次关闭都会重复走一遍这条分支，白做一次通知调用。
            m_tray->setBubbleShown(true);
        }
        // 走到这里：窗口已隐藏、关闭事件已撤销，进程继续留在托盘里监控用量。
        // 与上面气泡的关系：提示只在**首次**关闭时弹一次，之后静默隐藏；
        // "找不到托盘图标时怎么办"的两条退路（再运行一次唤回窗口 / 命令行 --quit）
        // 另见「设置 → 桌面集成」里的逃生说明与 README 的对应章节。
        return;
    }
    // 走到这里说明"隐藏到托盘"的前提不成立：托盘不可用、用户没开托盘、或没勾关闭到
    // 托盘。此时关窗即退出才符合直觉，也与 setQuitOnLastWindowClosed(true) 的语义一致。
    event->accept();
}

/**
 * @brief 把窗口从隐藏或最小化状态恢复到前台。
 *
 * 三步顺序不可颠倒：showNormal() 负责从"隐藏 / 最小化"两种状态恢复为常规窗口，
 * raise() 调整 Z 序，activateWindow() 再抢回键盘焦点。只做第一步时，Windows 常常
 * 只让任务栏图标闪一下，窗口仍停在其它程序后面。
 *
 * @return 无。
 * @note 本方法是"窗口唤回"的唯一实现：托盘双击、托盘单击、第二个实例的 SHOW 命令
 *       全部走它，避免三处各写一遍 showNormal/raise/activateWindow 而逐渐不一致。
 */
void MainWindow::restoreFromTray()
{
    showNormal();
    raise();
    activateWindow();
}

/**
 * @brief 请求退出进程，并且一定绕开"关闭到托盘"策略。
 *
 * 之所以必须先置 m_forceQuit：QApplication::quit() 会先关闭所有顶层窗口、触发
 * closeEvent()；若此时该标志仍为 false，closeEvent() 会按"关闭到托盘"把窗口藏起来，
 * 退出流程就被自己的关闭策略拦住——用户看到的是"点了退出毫无反应，程序还在后台"。
 *
 * @return 无。
 * @note 与托盘菜单的「退出」使用同一套两步动作（置标志 → quit），因此命令行
 *       `--quit`、第二个实例的 QUIT 命令、托盘菜单退出三者行为完全一致。
 */
void MainWindow::requestQuit()
{
    m_forceQuit = true;
    QApplication::quit();
}

/**
 * @brief 一次性搭建整个窗口的控件树与样式表。
 *
 * 构建顺序与视觉顺序一致：根布局 → 顶部标题栏 → 提示横幅 → 上排两张卡片
 * （套餐概览、本周期统计）→ 下排「用量限制」卡片（5 小时 / 每周 / 每月三块分段进度条）
 * → 底部错误区 → 全局样式表。各控件的 objectName 是样式表选择器的唯一依据，
 * 因此每建一个控件都紧跟一次 setObjectName，避免漏配导致样式整块失效。
 *
 * 布局伸缩意图：上排两张卡按 2 份、限流卡按 1 份分配多余高度，
 * 使窗口纵向拉高时三张卡片一起变高，而不是只把某一张撑成空壳。
 *
 * @return 无。
 * @note 只在构造函数中调用一次；重复调用会重复创建控件并造成悬空的旧指针。
 */
void MainWindow::buildUi()
{
    // 中央部件是所有内容的容器；交给 setCentralWidget 之后由 QMainWindow 接管所有权
    auto *central = new QWidget;
    // 单独建 central 而不是直接往窗口上挂控件，便于整块设置样式表
    setCentralWidget(central);

    // 根布局纵向堆叠：标题栏 → 横幅 → 上排两卡 → 限流卡 → 错误区
    auto *root = new QVBoxLayout(central);
    // 四周留白 20/18/20/16，避免控件贴边；卡片间距 14 保证块与块之间有呼吸感
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(14);

    // ---------------- 顶部标题栏 ----------------
    // 标题栏用横向布局：左侧标题与账号，中间弹簧，右侧状态与两个按钮
    auto *header = new QHBoxLayout;
    // 标题同时出现在窗口标题栏与本行首，形成统一的品牌视觉锚点
    auto *title = new QLabel(tr("Command Code 套餐用量"));
    // objectName 供样式表命中放大加粗的标题样式
    title->setObjectName(QStringLiteral("appTitle"));
    header->addWidget(title);

    // 账号标签：内容在 applySnapshot() 中按「用户名 → 邮箱 → 用户 ID」的优先级回填
    m_account = new QLabel;
    m_account->setObjectName(QStringLiteral("account"));
    // 账号与标题之间补 12px，避免两段文字视觉上粘连
    header->addSpacing(12);
    header->addWidget(m_account);
    // 弹簧把后续控件推到右侧对齐
    header->addStretch(1);

    // 状态标签：在「正在刷新 / 下次自动刷新 / 最后更新」几种文案之间切换
    m_status = new QLabel(tr("尚未刷新"));
    m_status->setObjectName(QStringLiteral("status"));
    // 错误原因可能包含服务端返回的任意文本：显式声明纯文本格式，
    // 避免 QLabel 的富文本自动探测把尖括号内容当 HTML 渲染
    m_status->setTextFormat(Qt::PlainText);
    header->addWidget(m_status);

    // 刷新按钮：点击即手动触发一次抓取，抓取期间由 setBusy() 置灰防止重复请求
    m_refreshButton = new QPushButton(tr("刷新"));
    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::refreshNow);
    header->addWidget(m_refreshButton);

    // 设置按钮：以局部变量的形式创建，因为除了点击连接之外本类不再需要引用它
    auto *settingsButton = new QPushButton(tr("设置"));
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::openSettings);
    header->addWidget(settingsButton);
    // 标题栏作为一个整体加入根布局
    root->addLayout(header);

    // ---------------- 提示横幅 ----------------
    // 横幅默认隐藏，只有 showBanner() 被调用时才显示，避免窗口启动时出现空白条
    m_banner = new QLabel;
    m_banner->setObjectName(QStringLiteral("banner"));
    // 允许换行：提示文案较长时不会把窗口撑宽
    m_banner->setWordWrap(true);
    m_banner->hide();
    root->addWidget(m_banner);

    // ---------------- 第一行：套餐概览 + 本周期统计 ----------------
    // 上排两卡等宽并排，各自占比 1
    // 宽度均分、高度按 2:1 分配（见下方 addLayout 的第二个参数），
    // 让信息密度更高的上排比「只有三条进度条」的下排略高，视觉重量更平衡。
    auto *row = new QHBoxLayout;
    row->setSpacing(14);

    // planBody 是套餐卡的正文布局，由 createCard() 通过出参回传
    QVBoxLayout *planBody = nullptr;
    QFrame *planCard = createCard(tr("套餐概览"), &planBody);
    // 套餐名：大字号；初始为占位符「—」，抓取成功后替换为真实套餐名
    m_planName = new QLabel(QStringLiteral("—"));
    m_planName->setObjectName(QStringLiteral("bigValue"));
    // 订阅状态：小字灰色，展示 active 等状态以及是否周期末取消
    m_planStatus = new QLabel(QStringLiteral("—"));
    m_planStatus->setObjectName(QStringLiteral("muted"));
    // 计费周期：起止时间可能较长，开启换行避免撑破卡片
    m_planPeriod = new QLabel(QStringLiteral("—"));
    m_planPeriod->setObjectName(QStringLiteral("muted"));
    m_planPeriod->setWordWrap(true);
    // 额度主数值：显示「剩余 / 套餐总额 credits」
    m_creditsValue = new QLabel(QStringLiteral("—"));
    m_creditsValue->setObjectName(QStringLiteral("bigValue"));
    // 额度明细：已用额度与百分比，必要时追加加购 / 赠送额度
    m_creditsDetail = new QLabel(QStringLiteral("—"));
    m_creditsDetail->setObjectName(QStringLiteral("muted"));
    // 额度进度条：分母为套餐总额，与官网百分比口径一致
    m_creditsBar = new SegmentedBar;
    // 上下各加一个弹簧，使内容在卡片长高后仍保持垂直居中
    // 套餐卡的信息层级：套餐名（大字号）→ 状态与周期（灰色小字）→ 剩余额度（大字号）
    // → 进度条 → 明细，上下各一个弹簧把这五段整体压在卡片中央，避免贴顶。
    // 行对齐约束：右侧「本周期统计」卡正文与本卡逐行镜像（见 statsBody 处注释），
    // 调整本卡行序或行数时必须同步右侧，否则两卡正文总高不等，垂直居中后
    // 同行元素（套餐名 ↔ 运行次数、剩余额度 ↔ tokens）会上下错位。
    planBody->addStretch(1);
    planBody->addWidget(m_planName);
    planBody->addWidget(m_planStatus);
    planBody->addWidget(m_planPeriod);
    // 周期信息与额度之间补 6px，形成两个语义分组
    planBody->addSpacing(6);
    planBody->addWidget(m_creditsValue);
    planBody->addWidget(m_creditsBar);
    planBody->addWidget(m_creditsDetail);
    planBody->addStretch(1);
    // 两张卡各占 1 份宽度，保证等宽
    row->addWidget(planCard, 1);

    // statsBody 是本周期统计卡的正文布局
    QVBoxLayout *statsBody = nullptr;
    QFrame *statsCard = createCard(tr("本周期统计"), &statsBody);
    // 运行次数：大字号主指标
    m_runsValue = new QLabel(QStringLiteral("—"));
    m_runsValue->setObjectName(QStringLiteral("bigValue"));
    // 成功与失败次数：次要信息，小字灰色
    m_successValue = new QLabel(QStringLiteral("—"));
    m_successValue->setObjectName(QStringLiteral("muted"));
    // 成功率：独立灰字行，与左卡「计费周期」同位（两卡第一组逐行镜像）
    m_successRate = new QLabel(QStringLiteral("—"));
    m_successRate->setObjectName(QStringLiteral("muted"));
    // token 总量：大字号主指标，使用 K / M 缩写
    m_tokensValue = new QLabel(QStringLiteral("—"));
    m_tokensValue->setObjectName(QStringLiteral("bigValue"));
    // 输入 / 输出 token 分项
    m_tokensDetail = new QLabel(QStringLiteral("—"));
    m_tokensDetail->setObjectName(QStringLiteral("muted"));
    // 成本：已消耗 credits 与单次平均成本
    m_costValue = new QLabel(QStringLiteral("—"));
    m_costValue->setObjectName(QStringLiteral("muted"));
    // 同样上下留弹簧，保证两组指标在卡片内垂直居中。
    // 行序与左侧套餐卡逐行镜像：大字号 / 灰字 / 灰字 / 6px / 大字号 /
    // 进度条槽位 / 灰字。两卡正文条目一一对应后总高度恒等（与字体度量无关），
    // 垂直居中时上下弹簧平分相同的多余空间，两卡正文顶部因此落在同一水平线上，
    // 「GOAT ↔ runs」「credits ↔ tokens」「进度条 ↔ 已消耗」等同行元素不再错位。
    statsBody->addStretch(1);
    statsBody->addWidget(m_runsValue);
    statsBody->addWidget(m_successValue);
    statsBody->addWidget(m_successRate);
    // 运行次数组与 token 组之间留出分隔
    statsBody->addSpacing(6);
    statsBody->addWidget(m_tokensValue);
    // 已消耗行占据左侧进度条的同位槽：高度锁定为进度条 sizeHint 同源的高度，
    // 文字在其中垂直居中，该行因此与左卡进度条带同高同位（同一 y 轴）；
    // 行距待遇与真实控件一致（前后各一个行距），两卡第 5~7 行全部逐行对齐。
    // 高度取自 m_creditsBar，进度条规格调整时此处自动跟随。
    m_costValue->setFixedHeight(m_creditsBar->sizeHint().height());
    statsBody->addWidget(m_costValue);
    statsBody->addWidget(m_tokensDetail);
    statsBody->addStretch(1);
    row->addWidget(statsCard, 1);

    // 上排两张卡吸收 2/3 的多余高度，限流卡 1/3：窗口拉高时不会把限流卡撑成空壳
    root->addLayout(row, 2);

    // ---------------- 第二行：用量限制 ----------------
    // 限流卡占满整行宽度，内部再横向切成三块等宽区域（5 小时 / 每周 / 每月），
    // 三块共用同一个工厂 lambda，因此名称、百分比、进度条、重置文案四行的
    // 垂直位置在三者之间严格对齐，横向扫视时不会出现参差的错位感。
    QVBoxLayout *limitsBody = nullptr;
    QFrame *limitsCard = createCard(tr("用量限制"), &limitsBody);

    // 局部 lambda：生成一块「名称 + 百分比 + 进度条 + 重置文案」的限流区块。
    // 参数约定：name 为区块标题；barOut / percentOut / resetOut 为三个出参指针，
    // 分别回传进度条、百分比标签、重置文案标签，供 applySnapshot() 后续写入；
    // 返回值是可直接 addLayout 的纵向布局。三块限流区共用该工厂，保证版式完全一致：
    // 若要调整限流区的排版，只改这一处即可，不必在三个区块里重复修改。
    const auto makeLimitBlock = [](const QString &name, SegmentedBar **barOut,
                                   QLabel **percentOut, QLabel **resetOut) -> QVBoxLayout *
    {
        // 每个限流块自带纵向布局，内部行距 6px
        auto *box = new QVBoxLayout;
        box->setSpacing(6);
        box->addStretch(1); // 卡片随窗口长高后，整块垂直居中而不是堆在顶部
        // 标题行：左侧名称、右侧百分比，中间用弹簧撑开
        auto *head = new QHBoxLayout;
        auto *nameLabel = new QLabel(name);
        nameLabel->setObjectName(QStringLiteral("limitName"));
        *percentOut = new QLabel(QStringLiteral("—"));
        (*percentOut)->setObjectName(QStringLiteral("limitPercent"));
        head->addWidget(nameLabel);
        head->addStretch(1);
        head->addWidget(*percentOut);
        box->addLayout(head);

        // 分段进度条：具体百分比在 applySnapshot() 中通过 setPercent() 写入
        *barOut = new SegmentedBar;
        box->addWidget(*barOut);

        // 重置文案：默认占位符「—」；有效时会被改写为倒计时（5 小时 / 每周）
        // 或计费周期结束时间（每月）
        *resetOut = new QLabel(QStringLiteral("—"));
        (*resetOut)->setObjectName(QStringLiteral("muted"));
        box->addWidget(*resetOut);
        box->addStretch(1);
        return box;
    };

    // 三块限流区等宽并排，间距 24px 以体现分组感
    auto *limitsRow = new QHBoxLayout;
    limitsRow->setSpacing(24);
    limitsRow->addLayout(makeLimitBlock(tr("5 小时限额"), &m_fiveHourBar, &m_fiveHourPercent, &m_fiveHourReset), 1);
    limitsRow->addLayout(makeLimitBlock(tr("每周限额"), &m_weeklyBar, &m_weeklyPercent, &m_weeklyReset), 1);
    limitsRow->addLayout(makeLimitBlock(tr("每月限额"), &m_monthlyBar, &m_monthlyPercent, &m_monthlyReset), 1);
    limitsBody->addLayout(limitsRow);

    // 让限流卡吸收多余的纵向空间：窗口拉高时卡片跟着长高，不再留出裸露空白
    root->addWidget(limitsCard, 1);

    // ---------------- 错误信息 ----------------
    // 刻意**不设**底部错误区：错误文字会随行数增高，把上方卡片挤压变形
    // （用户实测：一次超时之后，套餐/统计两张卡片被压扁）。
    // 失败信息改由右上角状态栏承载：一行「查询失败：<原因>」就地给出结论，
    // 多接口的完整明细放进状态栏的悬停提示（tooltip），不占任何布局空间。
    // 参见 onFailed() / applySnapshot() / showFailure()。

    // 全局样式表不在此处下发：它由 AppTheme 依据当前生效主题生成，并挂在
    // QApplication 上（见 AppTheme::applyInternal()）。之所以不再挂到中央部件：
    //   ① 主题切换需要一次性重算所有控件的配色，样式表挂在应用上时 Qt 会自动
    //      重新抛光全部控件，动态属性（level）驱动的颜色也随之更新；
    //   ② 设置对话框等其它顶层窗口不在中央部件的子树里，挂在中央部件上覆盖不到。
    // 本函数只需保证各控件都起了样式表中登记的 objectName（见上文各处 setObjectName），
    // 具体色值一律由调色板给出，界面代码中不再出现任何写死的颜色。
}

/**
 * @brief 创建一张带标题的卡片容器，并把正文布局通过出参回传给调用方。
 *
 * 卡片统一命名为 "card" 以命中样式表中的圆角边框规则；纵向策略设为 Expanding，
 * 使卡片能随窗口一起长高，从而消除窗口底部裸露的空白区域。
 * 标题文本会先 toUpper() 再显示，以贴合卡片标题全大写的视觉规范。
 *
 * @param[in]  title QString，卡片标题文本，只用于显示。
 * @param[out] bodyOut QVBoxLayout **，回传卡片正文布局的指针；允许为 nullptr，
 *                     此时调用方拿不到正文布局（本项目始终传入非空指针）。
 * @return QFrame *，新建的卡片外框；加入父布局后由布局接管所有权。
 * @note 正文布局位于标题标签下方，拥有独立的 8px 间距，便于各卡片自行排布内容。
 * @note 标题与正文分成两层布局，是为了让标题始终紧贴卡片顶部，而正文可自行居中。
 */
QFrame *MainWindow::createCard(const QString &title, QVBoxLayout **bodyOut)
{
    // 用 QFrame 而非 QWidget，是为了让样式表的 QFrame#card 选择器生效（背景与描边）
    // 三张卡片都经由此函数创建，因此圆角、内边距、标题字号只需维护一份；
    // 卡片内部的具体内容完全由调用方决定，本函数不关心任何业务语义。
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    card->setFrameShape(QFrame::NoFrame);
    // 纵向可扩展：与窗口一起长高，消除底部留白
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 卡片外壳布局：上方标题、下方正文
    auto *layout = new QVBoxLayout(card);
    // 内边距 18/14/18/16 让内容不贴边框，行距 10px 分隔标题与正文
    layout->setContentsMargins(18, 14, 18, 16);
    layout->setSpacing(10);

    // 标题小写转大写，靠样式表 #cardTitle 统一成小号灰字
    auto *titleLabel = new QLabel(title.toUpper());
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
    layout->addWidget(titleLabel);

    // 正文布局独立于卡片外壳，方便各卡片按自己的语义分组内容
    auto *body = new QVBoxLayout;
    body->setSpacing(8);
    layout->addLayout(body);

    // 仅当调用方需要正文布局时才回传，避免对空指针解引用
    if (bodyOut)
        *bodyOut = body;
    return card;
}

/**
 * @brief 记录命令行传入的临时凭证覆盖（--key / --base-url）。
 *
 * 覆盖值只写入内存成员，绝不落盘：这样「--key 只是本次运行的临时覆盖」这一语义
 * 由代码结构本身保证，而不是靠调用方自觉。用户一旦在「设置」中保存配置，
 * openSettings() 就会清空这两个成员，覆盖随之失效。
 *
 * @param[in] apiKey QString，命令行给出的 API Key；会先 trim 去掉首尾空白，
 *                   空串表示不覆盖（届时回退到 AppConfig 中的配置）。
 * @param[in] baseUrl QUrl，命令行给出的服务地址；无效或为空都表示不覆盖。
 * @return 无。
 * @note 必须在 startInitialRefresh() 之前调用，否则首次抓取读不到覆盖值。
 */
void MainWindow::setCredentialsOverride(const QString &apiKey, const QUrl &baseUrl)
{
    // 统一去掉首尾空白：命令行参数常因复制粘贴带上看不见的空格
    m_keyOverride = apiKey.trimmed();
    // base-url 原样保存；是否生效交由 effectiveBaseUrl() 判断有效性与非空
    m_baseUrlOverride = baseUrl;
}

/**
 * @brief 计算当前生效的 API Key。
 *
 * 优先级明确为「命令行覆盖 → 持久化配置」：覆盖值为空才算未覆盖，
 * 因此命令行无法用空串「清空」配置中的 Key，这是有意为之的行为。
 *
 * @return QString，可直接交给 CommandCodeApi 的 Key；两者都为空时返回空串，
 *         调用方（refreshNow / startInitialRefresh）据此判定「尚未配置」。
 * @note const 函数，只读取成员与配置，不产生任何副作用。
 */
QString MainWindow::effectiveApiKey() const
{
    // 覆盖值非空则优先使用，否则回退到设置里保存的 Key
    return m_keyOverride.isEmpty() ? AppConfig::apiKey() : m_keyOverride;
}

/**
 * @brief 计算当前生效的 base-url。
 *
 * 与 API Key 的判定不同，URL 需要同时满足「有效」与「非空」才算覆盖成功，
 * 因为默认构造的 QUrl 既无效也空，必须排除掉才能正确回退到持久化配置。
 *
 * @return QUrl，有效且非空的覆盖地址；否则返回 AppConfig::baseUrl()。
 * @note const 函数，不修改任何成员。
 */
QUrl MainWindow::effectiveBaseUrl() const
{
    // 覆盖地址必须有效且非空，二者缺一都视为「未覆盖」
    if (m_baseUrlOverride.isValid() && !m_baseUrlOverride.isEmpty())
        return m_baseUrlOverride;
    // 回退到设置中保存的地址
    return AppConfig::baseUrl();
}

/**
 * @brief 启动阶段的首次刷新入口：先灌入凭证，再决定是提示配置还是立即抓取。
 *
 * 之所以先 setApiKey / setBaseUrl 再判断 hasApiKey()，是因为「是否有 Key」的判定
 * 必须基于生效值（可能来自命令行覆盖）而不是配置文件，否则命令行启动会被误判为未配置。
 * 未配置时只显示常驻横幅并保持「等待配置」状态，不发起任何网络请求。
 *
 * @return 无。
 * @note 常驻横幅（sticky = true）不会因为没有错误而被隐藏，会一直提示到用户配置好为止。
 * @note 本函数只应在程序启动时调用一次；后续刷新走 refreshNow()。
 */
void MainWindow::startInitialRefresh()
{
    // 把生效凭证灌入客户端，供后续 fetchAll() 使用
    m_api->setApiKey(effectiveApiKey());
    m_api->setBaseUrl(effectiveBaseUrl());

    // 注意用客户端自身的是否有 Key 判断，因为它已经合并了命令行覆盖
    if (!m_api->hasApiKey())
    {
        showBanner(tr("尚未配置 API Key。点击右上角「设置」填写，或从本机 Command Code CLI 配置导入。"),
                   true, /*sticky=*/true);
        m_status->setText(tr("等待配置"));
        return;
    }
    // 凭证齐备，直接走一次正常的刷新流程
    refreshNow();
}

/**
 * @brief 打开「设置」对话框并在保存成功时立即刷新。
 *
 * 保存成功后先清空命令行覆盖，让本次运行切换到持久化配置，避免「设置里改了却仍被
 * 命令行值压住」的困惑；随后按最新配置重启自动刷新定时器，并同步倒计时秒数。
 * 若本次保存让程序从「无 Key」变为「有 Key」，则额外显示一次性横幅，
 * 并停掉自动刷新定时器再手动刷新一次，保证首屏数据尽快出现。
 *
 * @return 无。
 * @note SettingsDialog 以栈对象创建，exec() 返回即结束生命周期，无需手动释放。
 * @note 只有用户点击「确定」（Accepted）才应用改动，取消时保持现状。
 */
void MainWindow::openSettings()
{
    // 模态对话框：期间主窗口不可交互，避免用户在保存过程中触发刷新
    SettingsDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted)
    {
        // 用户在设置里保存过配置，命令行覆盖随之失效
        m_keyOverride.clear();
        m_baseUrlOverride = QUrl();

        // 记录保存前的状态，用于判断是否需要提示「API Key 已保存」
        bool hadKey = m_api->hasApiKey();
        // 覆盖已清空，此处读到的就是刚刚保存进配置文件的持久化值
        m_api->setApiKey(AppConfig::apiKey());
        m_api->setBaseUrl(AppConfig::baseUrl());
        const int seconds = AppConfig::refreshSeconds();
        // 间隔为正才启动定时器，否则视为用户关闭了自动刷新
        if (seconds > 0)
            m_refreshTimer->start(seconds * 1000);
        else
            m_refreshTimer->stop();
        // 倒计时与配置里的间隔保持同步，避免出现「刚设置完却显示旧秒数」
        m_secondsToRefresh = seconds > 0 ? seconds : 0;
        // 目标时刻与倒计时同源重算：若随后的 refreshNow() 正常发起，会以同一口径覆盖；
        // 这里先赋值是为了在"Key 为空、refreshNow() 提前返回"的路径上也不残留旧时刻
        m_nextRefreshAt = seconds > 0 ? QDateTime::currentDateTime().addSecs(seconds) : QDateTime();

        // 桌面集成配置（托盘开关、任务栏开关、关闭行为、展示指标）立即生效。
        // 必须在这里立刻重算：它同时决定 closeEvent() 的隐藏条件与任务栏的清理动作，
        // 若拖到下一次抓取才生效，用户就会看到"设置里明明关了托盘，行为却还是旧的"。
        // 这也是运行期唯一的重配置入口——配置只在设置对话框里被改写，别处不会动它。
        applyDesktopIntegrationSettings();
        // 用户可能刚刚关掉了托盘开关：若此时窗口正处于隐藏状态，必须主动显示出来，
        // 否则会出现"既没有托盘图标、也没有可见窗口"的第二个幽灵状态。
        // 典型路径：上次关窗时勾了"关闭到托盘"→ 窗口消失 → 用户只能从托盘菜单进设置，
        // 而一旦在这里把托盘开关也关掉，就再也没有任何入口能唤回窗口或结束进程了。
        // 用 show() 而不是 showNormal()：窗口本身是正常尺寸、只是被隐藏，
        // show() 不会改变几何信息，也就不会把用户调整过的窗口尺寸重置掉。
        if (!isVisible())
            show();

        // 首次配置成功的场景：给一次明确反馈，并立刻拉一次数据
        if (!hadKey && m_api->hasApiKey())
        {
            showBanner(tr("API Key 已保存，正在刷新…"), false);
            // 暂时停掉自动刷新，避免与下面的手动刷新在短时间内重复请求
            m_refreshTimer->stop();
            m_secondsToRefresh = 0;
            // 目标时刻同步作废：此刻的显示交给随后的 refreshNow() 重新排程后给出
            m_nextRefreshAt = QDateTime();
            refreshNow();
        }
        else
        {
            // 其余情况（改地址、改间隔）只需刷新一次即可看到新结果
            refreshNow();
        }
    }
}

/**
 * @brief 立即刷新：校验凭证、按最新配置重启自动刷新计时器，并发起一次异步抓取。
 *
 * 每次刷新都重新读取 AppConfig 并把生效凭证灌入客户端，使「设置」里刚改过的地址
 * 与间隔立即生效而不必重启程序。抓取前切到忙碌态禁用刷新按钮，结果由信号异步回来。
 *
 * @return 无。
 * @note 无可用 API Key 时直接返回并弹出常驻横幅，不会发出网络请求。
 * @note 本函数同时被「刷新」按钮、自动刷新定时器与 openSettings() 复用，
 *       因此内部必须自带凭证校验与计时器重置，不能假设调用方已做好准备。
 */
void MainWindow::refreshNow()
{
    // 防重入闸门：上一轮抓取尚未结束时（刷新按钮处于禁用态），自动刷新定时器
    // 到点仍会触发本函数。此时再 fetchAll() 会重置状态机，而在途请求的回包
    // 稍后到达时，会把**新一轮**的接口标签从在途集合里"偷走"，造成快照提前、
    // 重复收尾。因此忙碌期间到点的自动刷新直接忽略：在途轮次发起时已经重启过
    // 定时器，结束后下一次到点自然恢复正常刷新。
    // 注：openSettings() 保存后在忙碌期发起的刷新同样会被忽略一次，
    //     最迟在下一次自动刷新到点时生效（间隔 ≤ 用户设置的刷新秒数）。
    if (m_refreshButton && !m_refreshButton->isEnabled())
        return;

    // 无 Key 直接放弃：发出去的请求必然 401，徒增错误提示
    if (effectiveApiKey().isEmpty())
    {
        showBanner(tr("尚未配置 API Key。点击右上角「设置」填写，或从本机 Command Code CLI 配置导入。"),
                   true, /*sticky=*/true);
        return;
    }
    // 每次刷新都重新读配置，保证在设置里改的地址立即生效
    m_api->setApiKey(effectiveApiKey());
    m_api->setBaseUrl(effectiveBaseUrl());

    // 按当前配置重置自动刷新节拍：改了间隔无需重启程序
    // 每次刷新都重置计时器，等价于「以最后一次刷新为起点」重新计时，
    // 因此手动刷新会顺延自动刷新的时刻，不会出现刚点完就到点的尴尬。
    const int seconds = AppConfig::refreshSeconds();
    if (seconds > 0)
    {
        m_refreshTimer->start(seconds * 1000);
        m_secondsToRefresh = seconds;
        // 记录下一次刷新的目标时刻：以"本次刷新发起时"为基准，之后每秒心跳只读不写，
        // 因此状态栏显示的是一个稳定的目标时刻，不会随倒计时逐秒抖动；
        // 每次刷新发起都会走到这里，"刷新后时刻随之更新"由这一行天然保证
        m_nextRefreshAt = QDateTime::currentDateTime().addSecs(seconds);
    }
    else
    {
        // 间隔为 0 表示用户关闭了自动刷新
        m_refreshTimer->stop();
        m_secondsToRefresh = 0;
        // 同步作废目标时刻，避免状态栏残留一个不会再发生的旧时刻
        m_nextRefreshAt = QDateTime();
    }

    // 先置忙碌态再发请求，防止用户在等待期间连点刷新按钮
    setBusy(true);
    m_api->fetchAll();
}

/**
 * @brief 切换忙碌态：抓取期间禁用刷新按钮，结束后恢复并刷新倒计时文案。
 *
 * 忙碌时状态栏固定显示「正在刷新…」；结束时调用 updateCountdown() 立即恢复
 * 「下次自动刷新：N 秒」文案，而不必等下一次心跳（最多 1 秒的视觉空档）。
 *
 * @param[in] busy bool，true 表示正在抓取（按钮置灰），false 表示抓取结束。
 * @return 无。
 * @note 按钮的可用性同时被 updateCountdown() 当作「是否忙碌」的判据使用，
 *       因此本函数是「忙碌状态」这一事实的唯一写入点，避免出现两套状态源。
 */
void MainWindow::setBusy(bool busy)
{
    // 取反：忙碌时禁用（false），空闲时启用（true）
    m_refreshButton->setEnabled(!busy);
    if (busy)
    {
        m_status->setText(tr("正在刷新…"));
    }
    else
    {
        // 抓取刚结束，立即把状态栏从「正在刷新…」切回倒计时
        updateCountdown();
    }
}

/**
 * @brief 每秒心跳：递减自动刷新倒计时，并重算限流窗口的重置倒计时文案。
 *
 * 两件事共用同一拍：一是自动刷新倒计时归零时提前返回（紧接着 refreshNow() 会把
 * 状态改成「正在刷新…」，此处无需再写倒计时）；二是只要 5 小时窗口带有有效重置时间，
 * 就顺带刷新 5 小时与每周两处「重置还剩 Xd Xh」文案，让用户看到秒级变化。
 *
 * @return 无。
 * @note 用控件动态属性 hasReset 作为「是否已拿到有效重置时间」的轻量标记，
 *       避免为倒计时再引入额外的成员变量。
 * @note 每分钟限流没有独立的重置时刻，其文案就是计费周期结束时间，故不参与重算。
 */
void MainWindow::onTick()
{
    // 仅在自动刷新启用时（秒数 > 0）做递减
    if (m_secondsToRefresh > 0)
    {
        --m_secondsToRefresh;
        // 归零说明定时器已触发刷新，此时以状态栏倒计时为准，直接返回即可
        if (m_secondsToRefresh <= 0)
        {
            updateCountdown();
            return;
        }
    }
    // 倒计时之外，重置时间文案也要随时间跳动
    updateCountdown();
    // 以 5 小时窗口的 hasReset 作为整组是否有效的判据：两个窗口通常同时有数据
    if (m_fiveHourReset && m_fiveHourReset->property("hasReset").toBool())
    {
        // 重置时刻与用量后缀都由 applySnapshot() 写入动态属性，这里只按当前时间重算
        // 倒计时部分，再把同一个后缀拼回去。两者必须一起写：只写倒计时会让刷新时
        // 刚显示的用量在 1 秒后消失，形成"每次刷新闪一下用量"的观感。
        const auto refreshResetText = [](QLabel *label) {
            const QString countdown = MainWindow::formatCountdown(label->property("resetAt").toDateTime());
            label->setText(countdown + label->property("detailSuffix").toString());
        };
        refreshResetText(m_fiveHourReset);
        refreshResetText(m_weeklyReset);
    }
}

/**
 * @brief 刷新状态栏上的「下次刷新」时刻文案（北京时间）。
 *
 * 展示的是排程好的目标时刻而不是逐秒递减的倒计时：目标时刻在每次刷新发起时
 * 一次性写入 m_nextRefreshAt，之后心跳只读不写，因此文案在两次刷新之间保持稳定，
 * 并在每次刷新后自动推进到下一次的时刻。刷新按钮处于禁用态说明正在抓取，
 * 此时状态栏的「正在刷新…」优先级更高，直接返回、不做覆盖。
 * 未排程（间隔为 0 或尚未刷新过）时同样不写任何文案，
 * 状态栏保留上一次的「最后更新 HH:mm:ss」，保证信息不会被无意义地抹掉。
 *
 * @return 无。
 * @note 用按钮可用性代替额外的忙碌标志位，避免重复状态导致不同步。
 * @note 时刻一律换算成北京时间展示（系统时区不是东八区时也会正确换算），
 *       跨天时补出日期，避免"凌晨看到次日时刻却不带日期"的歧义。
 */
void MainWindow::updateCountdown()
{
    // 按钮被禁用 = 正在抓取，此时不覆盖「正在刷新…」
    if (m_refreshButton->isEnabled() == false)
        return;
    // 失败状态优先于倒计时：一次失败必须持续显示到下一次刷新发起，
    // 否则下一秒就会被「下次刷新」倒计时覆盖，用户来不及看到失败结论与原因
    if (m_lastRefreshFailed)
    {
        showFailure(m_lastFailureReason, m_lastFailureDetails);
        return;
    }
    // 仅在已排程下一次自动刷新时展示（间隔为 0 或尚未排程则维持现状）
    if (m_secondsToRefresh > 0 && m_nextRefreshAt.isValid())
    {
        // 先把目标时刻换算到北京时区，再决定格式与文案
        const QTimeZone beijing = beijingTimeZone();
        const QDateTime beijingTime = m_nextRefreshAt.toTimeZone(beijing);
        // 「是否同一天」也按北京时间的日历判断：显示口径必须与判断口径一致，
        // 否则系统时区与东八区跨日不同步时会出现"明明跨天却不带日期"的文案
        const bool sameBeijingDay = beijingTime.date() == QDateTime::currentDateTime(beijing).date();
        // 同一天只给时刻；跨天补日期，便于隔天回看时仍能对上
        const QString text = sameBeijingDay
                                 ? beijingTime.toString(QStringLiteral("HH:mm:ss"))
                                 : beijingTime.toString(QStringLiteral("MM-dd HH:mm:ss"));
        m_status->setText(tr("下次刷新：%1（北京时间）").arg(text));
    }
}

/**
 * @brief 显示顶部提示横幅，并设置其配色与是否常驻。
 *
 * 横幅分两类：常驻型（如未配置 API Key，sticky = true，不会自动消失）与
 * 一次性提示（如「API Key 已保存」，sticky = false，下一次成功抓取后由
 * applySnapshot() 自动隐藏）。配色不走全局样式表，而是按 warning 就地覆盖，
 * 以免在样式表里为两种状态各写一条选择器。
 *
 * @param[in] text QString，横幅正文。
 * @param[in] warning bool，true 用橙色警示配色，false 用绿色成功配色。
 * @param[in] sticky bool，true 表示常驻提示；默认 false。
 * @return 无。
 * @note 会写入成员 m_bannerSticky，是少数具有副作用的界面函数。
 */
void MainWindow::showBanner(const QString &text, bool warning, bool sticky)
{
    // 记录常驻标志，供 applySnapshot() 决定是否自动隐藏
    m_bannerSticky = sticky;
    m_banner->setText(text);
    // 配色交由样式表按语义级别决定：level=warn 走警示配色、level=ok 走成功配色，
    // 两套色值都来自当前主题调色板。这样做的关键收益是"主题切换后横幅自动换色"——
    // 若仍用就地 setStyleSheet() 写死色值，切到深色主题时横幅会残留浅色主题的配色，
    // 出现"浅色横幅上的深色字"或反之的错配。
    AppTheme::setLevel(m_banner, warning ? QStringLiteral("warn") : QStringLiteral("ok"));
    // 样式与文本就绪后再显示，避免出现一次无样式的闪烁
    m_banner->show();
}

/**
 * @brief 把 token 数量压缩成便于阅读的 K / M 单位文本。
 *
 * 阈值与小数位经过取舍：百万级保留两位小数（1.23M）足以表达量级差异，
 * 千级保留一位小数（12.3K），千以下直接输出整数，避免出现「523.0」这类噪声。
 *
 * @param[in] tokens qint64，原始 token 数；负数按原样输出（上游不会给出负数）。
 * @return QString，格式化后的文本。
 * @note 静态函数，不访问任何成员，便于在其它上下文复用。
 * @note 使用整数除法会丢掉小数，故这里显式除以浮点数常量。
 */
QString MainWindow::formatTokens(qint64 tokens)
{
    // 百万级：除以浮点数并保留两位小数，前缀 "%1" 由 arg 填入
    if (tokens >= 1000000)
        return QStringLiteral("%1M").arg(tokens / 1000000.0, 0, 'f', 2);
    // 千级：保留一位小数
    if (tokens >= 1000)
        return QStringLiteral("%1K").arg(tokens / 1000.0, 0, 'f', 1);
    // 千以下：直接输出整数文本
    return QString::number(tokens);
}

/**
 * @brief 把时间点格式化为「yyyy-MM-dd HH:mm」。
 *
 * 计费周期的起止时间只精确到分钟即可，因此不显示秒；
 * 无效时间统一以占位符「—」呈现，避免界面出现空白或 "Invalid" 字样。
 *
 * @param[in] dt const QDateTime &，待格式化的时间点。
 * @return QString，格式化后的时间文本；dt 无效时返回「—」。
 * @note 静态函数，与窗口状态无关，可安全地在任意位置调用。
 */
QString MainWindow::formatDateTime(const QDateTime &dt)
{
    // 无效时间不能走 toString，否则会得到无意义的空串
    if (!dt.isValid())
        return QStringLiteral("—");
    // 固定格式：年月日 + 时分，与计费周期的展示粒度一致
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

/**
 * @brief 把重置时刻格式化为「重置还剩 Xd Xh / Xh Xm / Xm」形式的中文倒计时。
 *
 * 精度随距离自动降级：超过一天只到「天 + 小时」，超过一小时到「小时 + 分钟」，
 * 一分钟内只显示分钟，既保证信息量又不让文案过长把卡片撑坏。
 * 已过期（secs ≤ 0）时返回「即将重置」，而不是负数倒计时。
 *
 * @param[in] resetAt const QDateTime &，限流窗口的重置时刻。
 * @return QString，面向用户的倒计时文案；resetAt 无效时返回「重置时间未知」。
 * @note 静态函数：不依赖成员状态，因此可直接在 buildUi 的 lambda 内部调用。
 * @note 「重置时间未知」这一返回值会被 applyLimit 用作是否需要兜底文案的判断依据，
 *       修改该文案时需同步检查调用方。
 */
QString MainWindow::formatCountdown(const QDateTime &resetAt)
{
    // 上游未给出重置时间：由调用方决定是否用兜底文案覆盖
    if (!resetAt.isValid())
        return QStringLiteral("重置时间未知");
    // 用当前本地时间做差，得到距离重置的秒数
    const qint64 secs = QDateTime::currentDateTime().secsTo(resetAt);
    // 已到期或已过期：显示「即将重置」，服务端很快会返回新的窗口
    if (secs <= 0)
        return QStringLiteral("即将重置");
    // 拆成天、小时、分钟三段；86400 = 一天的秒数，3600 = 一小时的秒数
    // 只做整数运算并以“向下取整”的方式展示，因此文案读数只会比真实剩余时间略短，
    // 不会出现「显示还剩 1h 却已经重置」这种令人困惑的乐观读数。
    const qint64 days = secs / 86400;
    const qint64 hours = (secs % 86400) / 3600;
    const qint64 minutes = (secs % 3600) / 60;
    // 超过一天：精确到小时即可，分钟数对用户决策没有价值
    if (days > 0)
        return QStringLiteral("重置还剩 %1d %2h").arg(days).arg(hours);
    // 一小时内：精确到分钟
    if (hours > 0)
        return QStringLiteral("重置还剩 %1h %2m").arg(hours).arg(minutes);
    // 最后一分钟内：只显示分钟
    return QStringLiteral("重置还剩 %1m").arg(minutes);
}

/**
 * @brief 把状态栏切换为「查询失败」并记录失败原因。
 *
 * 失败状态必须**持续显示**：如果只设置一次文本，下一秒的心跳 updateCountdown()
 * 就会用「下次刷新」倒计时把它覆盖掉，用户根本来不及看到失败结论；
 * 因此这里同时记录标志位与原因，由 updateCountdown() 在每次重绘时维持该文案。
 *
 * @param[in] reason QString，展示在状态栏里的失败原因（宜精简，一行以内）。
 * @param[in] details QString，悬停提示里的完整明细（多接口逐条）；空串时用 reason。
 * @return 无。
 * @note 颜色走语义级别 danger，由 AppTheme 按当前主题给出（深浅两套各自保证对比度）。
 * @note 只影响状态栏，不改动任何数据控件；旧数据保留策略见 onFailed()。
 */
void MainWindow::showFailure(const QString &reason, const QString &details)
{
    // 幂等保护：updateCountdown() 每秒都会以同一对参数调用本函数，
    // 内容未变化时直接返回，避免每秒重复 setText/setToolTip 造成无谓重绘
    const QString detailsNorm = details.isEmpty() ? reason : details;
    if (m_lastRefreshFailed && m_lastFailureReason == reason && m_lastFailureDetails == detailsNorm)
        return;

    m_lastRefreshFailed = true;
    m_lastFailureReason = reason;
    m_lastFailureDetails = detailsNorm;
    // 状态栏是一行 QLabel，没有省略号机制：原因过长会把顶栏撑宽。
    // 展示文本按 340 逻辑像素做省略号截断，完整原因仍在悬停提示里，不丢信息；
    // updateCountdown() 每秒重绘时走同一截断，显示稳定一致。
    const QFontMetrics metrics = m_status->fontMetrics();
    const QString shown = metrics.elidedText(tr("查询失败：%1").arg(reason),
                                             Qt::ElideRight, 340);
    m_status->setText(shown);
    // 完整明细（多接口逐条）放悬停提示：既保留全部信息，又不占一像素布局空间
    m_status->setToolTip(m_lastFailureDetails);
    AppTheme::setLevel(m_status, QStringLiteral("danger"));
}

/**
 * @brief 清除失败状态：状态栏回到常规文案，悬停提示与语义级别一并复位。
 * @return 无。
 * @note 只在快照无错误（成功或部分数据全部可用）时由 applySnapshot() 调用。
 */
void MainWindow::clearFailure()
{
    m_lastRefreshFailed = false;
    m_lastFailureReason.clear();
    m_lastFailureDetails.clear();
    m_status->setToolTip(QString());
    AppTheme::setLevel(m_status, QString());
}

/**
 * @brief 抓取失败槽：恢复按钮可用性，并把状态栏切换为「查询失败：<原因>」（红色）。
 *
 * 已有数据不清空——保留上一次成功的数据比清空更有用，用户仍能看到旧的用量，
 * 同时通过状态栏明确知道本次抓取失败了；失败原因全文放悬停提示。
 *
 * @param[in] message QString，来自 CommandCodeApi 的错误描述文本。
 * @return 无。
 * @note 失败文案会持续显示，直到下一次刷新发起（变回「正在刷新…」）
 *       或下一次成功抓取（变回「最后更新 HH:mm:ss」）。
 */
void MainWindow::onFailed(const QString &message)
{
    // 恢复刷新按钮可用性，否则一次失败会让界面永久卡在忙碌态
    setBusy(false);
    // 状态栏就地给出「查询失败 + 原因」；此前失败后状态栏立刻切回
    // 「下次刷新」倒计时，用户完全看不出这一轮失败了。
    showFailure(message, message);
    // 数据保留策略：不清空上一次成功的数据。一次 20 秒超时就把整套数字清空，
    // 会让用户在等待重试的窗口里失去全部参考信息；失败结论由状态栏明确给出，
    // 不会与旧数据混淆。若希望失败即清空，把这里换成 applySnapshot() 的占位逻辑即可。
}

/**
 * @brief 抓取成功槽：结束忙碌态并把快照交给 applySnapshot() 渲染。
 *
 * 拆成两个函数是为了让「状态切换」与「界面渲染」各自单一职责；
 * 空快照与有内容的快照走同一条路径，不会出现状态不同步。
 *
 * @param[in] snapshot const UsageSnapshot &，本次抓取的完整结果。
 * @return 无。
 * @note 必须先 setBusy(false) 再渲染：渲染过程中若再次点击刷新，
 *       忙碌态判断才不会因为残留状态而误判。
 */
void MainWindow::onSnapshotReady(const UsageSnapshot &snapshot)
{
    // 先恢复按钮与状态栏，保证即便渲染过程中出现异常也不会卡在忙碌态
    setBusy(false);
    applySnapshot(snapshot);
}

/**
 * @brief 把一次成功抓取的快照渲染到全部控件上（纯界面更新，不发网络请求）。
 *
 * 处理顺序与界面自上而下的视觉顺序一致：账号 → 套餐概览 → 限流窗口 → 本周期统计
 * → 错误与状态。其中最绕的是额度口径：
 *   - credits.monthlyRemaining 的语义是「剩余」而不是「已用」；
 *   - 因此已用额度优先用「套餐总额 − 剩余」反推，套餐总额未知时退回 summary.totalCost；
 *   - 套餐总额与额度都未知时，用「剩余 + 已用」凑出一个自洽的分母；
 *   - 全都拿不到时把分母退化为已用值，避免出现除零。
 * 百分比统一 floor 向下取整（pct = floor(used / cap × 100)），与官网展示保持一致。
 *
 * @param[in] snapshot const UsageSnapshot &，一次抓取的完整结果；各子结构体自带 valid
 *                     标志，无效字段按「—」或 0 处理，调用方无需预判。
 * @return 无。
 * @note 除通过 showBanner 之外不修改任何成员状态；所有写入都发生在控件属性上。
 * @note 各分区彼此独立：某一分区数据无效时只跳过该分区，不影响其余卡片，
 *       这样部分接口失败时界面仍能呈现尽可能多的有效信息。
 */
void MainWindow::applySnapshot(const UsageSnapshot &snapshot)
{
    // ---------------- 账号 ----------------
    // 约定：快照中每个子结构体都自带 valid 标志，只有 valid 为 true 才认为其字段可信；
    // 无效数据一律以「—」或 0 呈现，既不猜测也不清空上一次的展示结果。
    // 只有 whoami 有效时才更新账号标签，避免失败请求把已有账号信息抹掉
    if (snapshot.whoami.valid)
    {
        QString who = snapshot.whoami.userName;
        // 用户名优先，其次邮箱，最后用户 ID：越靠前越容易被人识别
        if (who.isEmpty())
            who = snapshot.whoami.email;
        if (who.isEmpty())
            who = snapshot.whoami.userId;
        // 三者都为空则清空标签，而不是留下一个孤零零的「账号：」前缀
        m_account->setText(who.isEmpty() ? QString() : tr("账号：%1").arg(who));
    }

    // ---------------- 套餐 ----------------
    // 三个子结构体分别承载订阅、额度与用量统计，此处取引用避免反复拷贝
    const SubscriptionInfo &sub = snapshot.subscription;
    const CreditsInfo &credits = snapshot.credits;
    const UsageSummary &summary = snapshot.summary;

    const QString planId = sub.planId;
    // 套餐总额来自 PlanCatalog；未知套餐返回 0，后续以此判定是否需要兜底
    double planTotal = PlanCatalog::totalCredits(planId);
    // 注意：monthlyRemaining 是「剩余」而不是「已用」，命名容易误读
    const double remaining = credits.monthlyRemaining;

    // 已用额度：优先用「套餐总额 − 剩余」，未知套餐时退回 summary.totalCost
    double used = summary.totalCost;
    if (planTotal > 0.0)
        used = qMax(0.0, planTotal - remaining);
    if (planTotal <= 0.0 && credits.valid)
        planTotal = remaining + used; // 兜底：至少能显示一个自洽的进度
    if (planTotal <= 0.0)
        planTotal = used;

    m_planName->setText(PlanCatalog::displayName(planId));
    // 订阅无效时统一显示「未知」；有效时附带「周期末取消」的补充说明
    m_planStatus->setText(sub.valid
                              ? tr("状态：%1%2").arg(sub.status, sub.cancelAtPeriodEnd ? tr("（周期末取消）") : QString())
                              : tr("状态：未知"));
    // 起止时间都有效才展示完整周期，否则退化为「未知」
    if (sub.currentPeriodStart.isValid() && sub.currentPeriodEnd.isValid())
    {
        m_planPeriod->setText(tr("计费周期：%1 → %2")
                                  .arg(formatDateTime(sub.currentPeriodStart),
                                       formatDateTime(sub.currentPeriodEnd)));
    }
    else
    {
        m_planPeriod->setText(tr("计费周期：未知"));
    }

    // 主数值按「剩余 / 套餐总额」展示：用户最关心的是还剩多少
    m_creditsValue->setText(tr("%1 / %2 credits").arg(remaining, 0, 'f', 2).arg(planTotal, 0, 'f', 2));
    // 分母保护：理论上 planTotal 已非 0，此处再判一次以防极端数据出现除零
    const int creditsPercent = planTotal > 0.0
                                   ? static_cast<int>(std::floor(used / planTotal * 100.0))
                                   : 0;
    m_creditsBar->setPercent(creditsPercent);
    // 明细行展示已用额度（四位小数以体现小额消耗）与同一口径的百分比
    QString detail = tr("本周期已用 %1 credits（%2%）").arg(used, 0, 'f', 4).arg(creditsPercent);
    // 只有确实存在加购或赠送额度时才追加说明，避免常态下文案冗长
    if (credits.purchasedCredits > 0.0 || credits.freeCredits > 0.0)
    {
        detail += tr("　加购 %1　赠送 %2")
                      .arg(credits.purchasedCredits, 0, 'f', 2)
                      .arg(credits.freeCredits, 0, 'f', 2);
    }
    m_creditsDetail->setText(detail);

    // ---------------- 限流窗口 ----------------
    // 局部 lambda：把一条限流记录渲染成「进度条 + 百分比 + 重置文案」三件套。
    // 参数约定：limit 为待渲染的窗口数据；bar / percentLabel / resetLabel 为对应的三个控件；
    // fallbackReset 是当重置时间未知时使用的兜底文案（空串表示不兜底）。
    // 5 小时与每周两块共用它，保证两条限流区永远同构；每月块分母不同，另行处理。
    const auto applyLimit = [](const WindowLimit &limit, SegmentedBar *bar, QLabel *percentLabel,
                               QLabel *resetLabel, const QString &fallbackReset)
    {
        // 数据无效：进度条归零、文字统一置为占位符「—」
        if (!limit.valid)
        {
            bar->setPercent(0);
            percentLabel->setText(QStringLiteral("—"));
            resetLabel->setText(QStringLiteral("—"));
            // 清掉 hasReset 标记，心跳逻辑据此跳过对本窗口的倒计时重算
            resetLabel->setProperty("hasReset", false);
            return;
        }
        // 百分比由数据模型按 floor(used / cap × 100) 统一给出，此处不再改口径
        const int percent = limit.percent();
        bar->setPercent(percent);
        percentLabel->setText(QStringLiteral("%1%").arg(percent));
        // 三档配色：≥85% 红、≥60% 黄、其余绿，让高占用一眼可见。
        // 这里只写"语义级别"，具体色值由样式表按当前主题给出：深色主题用的是
        // 一组亮色版的语义色，避免深红 / 深绿落在深色卡片上导致看不清。
        AppTheme::setLevel(percentLabel, percent >= 85   ? QStringLiteral("danger")
                                         : percent >= 60 ? QStringLiteral("warn")
                                                         : QStringLiteral("ok"));
        // 写入动态属性：hasReset 供心跳判断，resetAt 供心跳重算倒计时文案
        resetLabel->setProperty("hasReset", true);
        resetLabel->setProperty("resetAt", limit.resetAt);
        const QString text = MainWindow::formatCountdown(limit.resetAt);
        // 用量后缀一律展示「（已用 / 上限）」的数值本身，不再冠以「已用」字样：
        // 括号里的两个数含义自明（官网同款排版），前缀反而拉长文案；
        // 近期未使用（used 为 0）时也按同一格式展示「（0 / 上限）」，
        // 让三块限流区的括号排版在任何状态下都逐字对齐。
        // 括号前不留分隔符，与套餐卡的「本周期已用 x credits（y%）」保持同一排版：
        // 全角空格会撑出明显空隙，与卡片内其它行不协调。
        // 后缀单独存进动态属性供每秒心跳复用：心跳只重算倒计时本身，如果不把后缀
        // 一起拼回去，刷新时刚写入的用量会在 1 秒后被抹掉，表现为"每次刷新闪一下用量"。
        const QString detail = QStringLiteral("（%1 / %2）")
                                   .arg(limit.used, 0, 'f', 2)
                                   .arg(limit.cap, 0, 'f', 0);
        resetLabel->setProperty("detailSuffix", detail);
        resetLabel->setText(text + detail);
        // 上游未给出重置时间时，用调用方提供的兜底文案替换整行。此时除了清掉后缀属性，
        // 还必须关掉心跳重算：兜底文案由调用方给定，心跳既无法重算也无法把它拼回来，
        // 不关就会像用量后缀那样在 1 秒后被覆盖（同一类闪烁）。
        if (!text.isEmpty() && text.startsWith(QStringLiteral("重置时间未知")) && !fallbackReset.isEmpty())
        {
            resetLabel->setProperty("detailSuffix", QString());
            resetLabel->setProperty("hasReset", false);
            resetLabel->setText(fallbackReset);
        }
    };

    // 5 小时与每周窗口：分母分别是各自的上限，走统一的限流渲染逻辑
    // 兜底文案传空串：这两个窗口在上游总是带有明确的重置时刻，
    // 真拿不到时显示「重置时间未知」比编造一个时间更诚实。
    applyLimit(credits.fiveHour, m_fiveHourBar, m_fiveHourPercent, m_fiveHourReset, QString());
    applyLimit(credits.weekly, m_weeklyBar, m_weeklyPercent, m_weeklyReset, QString());

    // 每月限额：用套餐总额做分母
    // 与套餐额度卡共用同一个百分比，保证两处数字永远一致、不会互相打架
    m_monthlyBar->setPercent(creditsPercent);
    m_monthlyPercent->setText(QStringLiteral("%1%").arg(creditsPercent));
    // 着色规则与上面两个窗口保持一致，避免同一页面出现两套阈值；
    // 同样只写语义级别，由样式表按当前主题决定具体色值。
    AppTheme::setLevel(m_monthlyPercent, creditsPercent >= 85   ? QStringLiteral("danger")
                                         : creditsPercent >= 60 ? QStringLiteral("warn")
                                                                : QStringLiteral("ok"));
    // 每月窗口的「重置」就是计费周期结束，因此直接展示周期结束时间而非倒计时；
    // 用量后缀与上两块保持同一格式「（已用 / 上限）」，三块排版逐字对齐
    if (sub.currentPeriodEnd.isValid())
    {
        m_monthlyReset->setText(tr("周期结束：%1").arg(formatDateTime(sub.currentPeriodEnd))
                                + QStringLiteral("（%1 / %2）")
                                      .arg(used, 0, 'f', 2)
                                      .arg(planTotal, 0, 'f', 2));
    }
    else
    {
        m_monthlyReset->setText(QStringLiteral("—"));
    }

    // ---------------- 统计 ----------------
    // 统计无效时保留上一次的数值，避免一轮失败把三张卡片全部清空
    // 五条统计文案共用同一个 if 分支，保证「要么整组一起更新、要么整组保持原样」，
    // 不会出现次数是新值而 token 还是旧值这种自相矛盾的中间态。
    if (summary.valid)
    {
        m_runsValue->setText(tr("%1 runs").arg(summary.totalCount));
        // 成功率由模型算好（保留一位小数），此处只负责排版；
        // 次数与成功率拆成两行灰字，与左卡「状态 / 计费周期」逐行镜像对齐
        m_successValue->setText(tr("成功 %1 / 失败 %2")
                                    .arg(summary.completedCount)
                                    .arg(summary.failedCount));
        m_successRate->setText(tr("成功率 %1%").arg(summary.successRate, 0, 'f', 1));
        m_tokensValue->setText(tr("%1 tokens").arg(formatTokens(summary.tokensTotal)));
        // 输入与输出分项同样走 K / M 缩写，保持与总量一致的阅读体验
        m_tokensDetail->setText(tr("输入 %1　输出 %2")
                                    .arg(formatTokens(summary.tokensIn), formatTokens(summary.tokensOut)));
        // 成本保留四位小数：单次成本常常小于 0.01，位数太少会全被抹成 0
        m_costValue->setText(tr("已消耗 %1 credits　平均 %2 / 次")
                                 .arg(summary.totalCost, 0, 'f', 4)
                                 .arg(summary.averageCost, 0, 'f', 4));
    }

    // ---------------- 错误与状态 ----------------
    // 本次抓取没有任何接口报错
    // 只有「无错误」才隐藏横幅，且仅限非常驻类型：常驻提示代表用户尚未完成的动作，
    // 不能因为一次成功抓取就悄悄消失，否则用户会以为已经配置好了。
    if (snapshot.errors.isEmpty())
    {
        // 成功即清除失败状态：状态栏回到「最后更新 / 下次刷新」的正常文案
        clearFailure();
        // 只隐藏一次性提示；常驻横幅（如未配置 API Key）必须继续留在界面上
        if (m_banner->isVisible() && !m_bannerSticky)
            m_banner->hide();
        // 时间戳只到秒：用户关心的是「数据有多新」，日期部分没有意义
        m_status->setText(tr("最后更新 %1").arg(snapshot.fetchedAt.toString(QStringLiteral("HH:mm:ss"))));
    }
    else
    {
        // 部分接口失败：状态栏就地给出「查询失败 + 首条原因」，多接口明细放悬停提示。
        // **不再**写入底部错误区——那块区域会随错误行数增高，把上方卡片挤压变形
        // （用户实测：一次超时后套餐/统计两张卡片被压扁）。
        // 数据照常渲染：失败接口的数值保持「—」，成功接口的数值仍然更新。
        const QString reason = snapshot.errors.first()
                                 + (snapshot.errors.size() > 1
                                        ? tr(" 等 %1 项").arg(snapshot.errors.size())
                                        : QString());
        showFailure(reason, snapshot.errors.join(QLatin1Char('\n')));
    }

    // 刷新结束后立即恢复倒计时文案，不必等下一次心跳；
    // 失败路径由 updateCountdown() 内的失败分支维持「查询失败」文案，不会被覆盖
    updateCountdown();

    // ---------------- 桌面集成（托盘 / 任务栏）----------------
    // 放在最后统一推送：一是此时所有数值都已算好，二是即便推送失败也不影响主界面。
    updateDesktopIndicators(snapshot, metricView(snapshot));
}

/**
 * @brief 依据当前配置的指标，把一次快照折算成托盘/任务栏要展示的那一项。
 *
 * 数值口径与 applySnapshot() 严格一致（占用率一律 floor(已用 / 上限 × 100)），
 * 之所以在此处重算一遍套餐总额与已用额度，是为了让"缩略信息"成为一个自洽的
 * 只读视图，可脱离主界面单独理解与验证；代价仅为几次浮点运算。
 *
 * @param[in] snapshot const UsageSnapshot &，最近一次成功抓取的数据。
 * @return MetricView，含指标名、占用率、缩略文字与明细文案。
 * @note text 会直接画进 16~32 像素的图标，因此刻意保持 ≤ 4 个字符：图标在任务栏与
 *       通知区域都会被系统按小尺寸缩放，字符一多就糊成一团；托盘悬停提示有整行空间，
 *       更长的说明交给 detail 承载。
 * @note 四档指标的差异只有两处，看懂这两处就能读懂整个 switch：
 *       ① 分母不同——前两档取各自窗口上限，后两档取套餐总额；
 *       ② 方向不同——前三档的 percent 是"已用占比"（越大越危险），
 *          第四档「剩余额度」是"剩余占比"（越大概率越安全）。
 * @note 选择「剩余额度」时 text 直接给出余额数字而不是百分比，正是为了不让这个相反的
 *       方向被误读成"已经用掉了 69%"；用户看到的是"还能用 69"。
 */
MainWindow::MetricView MainWindow::metricView(const UsageSnapshot &snapshot) const
{
    // 先按"无数据"初始化：任何分支若漏了赋值，调用方拿到的也是安全的空值
    // （valid=false → 任务栏清空、托盘显示占位），而不是未初始化的随机内容。
    MetricView view;

    // 与主界面一致的兜底链：套餐总额已知时用「总额 − 剩余」，未知时退回 summary.totalCost
    const double planTotal = PlanCatalog::totalCredits(snapshot.subscription.planId);
    const double remaining = snapshot.credits.monthlyRemaining;
    double used = snapshot.summary.totalCost;
    if (planTotal > 0.0)
        used = qMax(0.0, planTotal - remaining);
    double total = planTotal;
    if (total <= 0.0)
        total = remaining + used;

    // 统一的百分比换算：分母为 0 时返回 0，避免除零产生 inf/NaN 传到界面上
    // "套餐总额未知"是合法状态（新套餐、接口部分失败），因此这里只退化数值，
    // 不抛错也不返回负数，界面在这种状态下仍要能正常显示。
    const auto percentOf = [](double part, double whole) -> int
    {
        if (whole <= 0.0)
            return 0;
        return static_cast<int>(std::floor(part / whole * 100.0));
    };

    // 每次折算都重读一次配置：用户在设置里改完档位，下一次刷新就会换到新档，
    // 不需要重启程序，也不需要额外的信号来通知这里。
    switch (AppConfig::statusMetric())
    {
    case AppConfig::StatusMetric::fiveHour:
    {
        // 第 1 档：5 小时滑动窗口。分母是窗口上限、分子是窗口内已用量，
        // percent() 由数据模型按 floor(已用 / 上限 × 100) 给出，方向是"越大越紧张"。
        // 这一档最适合刚跑完几个大任务的用户：短期是否会被限流可以立刻看出来。
        const WindowLimit &limit = snapshot.credits.fiveHour;
        view.name = tr("5 小时限额");
        view.valid = limit.valid;
        view.percent = limit.percent();
        // text 只放"N%"：图标里 3 个以上字符就会互相挤压，百分比已足够表达占用程度。
        view.text = QStringLiteral("%1%").arg(view.percent);
        view.detail = tr("%1 / %2　%3")
                          .arg(limit.used, 0, 'f', 2)
                          .arg(limit.cap, 0, 'f', 0)
                          .arg(formatCountdown(limit.resetAt));
        break;
    }
    case AppConfig::StatusMetric::weekly:
    {
        // 第 2 档：每周滑动窗口，语义与 5 小时档同构，只是窗口更长、上限更大。
        // 保留独立分支而不与上一档合并，是因为 valid 与 percent 取自不同的子结构体，
        // 强行合并只能把窗口对象当参数传进来，反而更难看懂。
        const WindowLimit &limit = snapshot.credits.weekly;
        view.name = tr("每周限额");
        view.valid = limit.valid;
        view.percent = limit.percent();
        view.text = QStringLiteral("%1%").arg(view.percent);
        view.detail = tr("%1 / %2　%3")
                          .arg(limit.used, 0, 'f', 2)
                          .arg(limit.cap, 0, 'f', 0)
                          .arg(formatCountdown(limit.resetAt));
        break;
    }
    case AppConfig::StatusMetric::monthly:
    {
        // 第 3 档：本计费周期额度（套餐总额口径）。这里必须重算一遍 planTotal / used，
        // 才能保证缩略信息与套餐卡给出同一个数字；反推链同样沿用主界面的
        // "总额 − 剩余"，总额未知时退回 summary.totalCost。
        view.name = tr("每月额度");
        // 额度或统计任一有效即可展示，避免只有一个接口成功时缩略信息整个空白
        // 之所以放宽到"任一有效"：这一档的数值同时来自额度与统计两个接口，
        // 只有一个成功时给出近似值，也比整块缩略信息空着更有用。
        view.valid = snapshot.credits.valid || snapshot.summary.valid;
        view.percent = percentOf(used, total);
        view.text = QStringLiteral("%1%").arg(view.percent);
        view.detail = tr("%1 / %2 credits　周期结束 %3")
                          .arg(used, 0, 'f', 2)
                          .arg(total, 0, 'f', 2)
                          .arg(formatDateTime(snapshot.subscription.currentPeriodEnd));
        break;
    }
    case AppConfig::StatusMetric::remaining:
    {
        // 第 4 档：剩余额度，四档里唯一"方向相反"的一档——percent 表示还剩多少比例，
        // 数值越大越安全；前三档表示用了多少，数值越大越危险。
        view.name = tr("剩余额度");
        view.valid = snapshot.credits.valid;
        // 「剩余」这一档的百分比表示"还剩多少比例"，与前三档的"用了多少"方向相反，
        // 因此文字直接给余额数字（如 69），而不是容易误读的百分比。
        view.percent = percentOf(remaining, total);
        // 四舍五入到整数再转文本，同样是为了把 text 控制在 4 个字符以内：
        // 图标里放不下小数，余额的个位变化已足够提示。
        view.text = QString::number(qRound(remaining));
        // 阅读提示：这一档的 percent 只用于决定进度条长度，而任务栏角标的配色同样按
        // percent 分档（越大越警示），与"剩余越多越安全"的方向正好相反；
        // 因此判读这一档时必须结合 detail 的文字与指标名，不能只看颜色。
        view.detail = tr("剩余 %1 / %2 credits")
                          .arg(remaining, 0, 'f', 2)
                          .arg(total, 0, 'f', 2);
        break;
    }
    }
    return view;
}

/**
 * @brief 把指标推送到托盘图标与任务栏按钮。
 *
 * 托盘始终接收完整上下文（套餐名、剩余额度、更新时间、错误），因为它的悬停提示
 * 有足够空间；任务栏只拿得到进度百分比与一个角标文字，因此只推送最必要的两项。
 *
 * @param[in] snapshot const UsageSnapshot &，用于补齐套餐名与剩余额度等上下文。
 * @param[in] metric const MetricView &，本次要展示的指标。
 * @return 无。
 * @note 任务栏未连接（例如无桌面会话）时静默跳过，不影响托盘与主界面。
 * @note 托盘与任务栏的更新彼此独立：任务栏被用户关掉时托盘照常刷新，反之亦然；
 *       任何一侧失败都不应影响另一侧，更不该影响主界面。
 * @note 本函数是"推送"这一职责的唯一入口：所有对外的桌面呈现都从这里出去，
 *       因此自检报告只要看这里推送过什么，就能判断界面数据与呈现是否一致。
 */
void MainWindow::updateDesktopIndicators(const UsageSnapshot &snapshot, const MetricView &metric)
{
    // 记录最近一次指标：--probe-ui 自检需要据此报告"实际推送了什么"
    m_lastMetric = metric;
    m_hasMetric = true;
    // 先存后推：即便下面的推送全部失败（例如任务栏绑定不上），自检报告仍能回答
    // "本程序算出了什么"，从而把故障范围锁定在推送环节，而不是数据环节。

    // ---------------- 通知区域 ----------------
    if (m_tray)
    {
        // 托盘拿到的是完整上下文而不是单一数字：它的悬停提示有整行文字的空间，
        // 用户不打开主窗口也能判断"用量正常"还是"这个套餐不对劲"。
        // 这里逐字段搬运而不是直接把 MetricView 递下去：托盘模块无需知道指标是怎么
        // 算出来的，依赖方向保持"界面 → 托盘"单向，托盘也不会反过来去读配置。
        TrayStatus status;
        status.valid = metric.valid;
        status.metricName = metric.name;
        status.metricPercent = metric.percent;
        status.metricText = metric.text;
        status.planName = PlanCatalog::displayName(snapshot.subscription.planId);
        status.remaining = snapshot.credits.monthlyRemaining;
        status.total = PlanCatalog::totalCredits(snapshot.subscription.planId);
        // 未知套餐时用「剩余 + 已用」兜底，保证提示里的分母不会退化成 0
        // 若不兜底，提示里会出现"剩余 12.00 / 0.00"这种自相矛盾的文案，
        // 用户会直接怀疑数据算错了，而不是意识到只是套餐名不在目录里。
        if (status.total <= 0.0)
            status.total = status.remaining + snapshot.summary.totalCost;
        status.detailText = metric.detail;
        status.updatedText = snapshot.fetchedAt.toString(QStringLiteral("HH:mm:ss"));
        status.errors = snapshot.errors;
        m_tray->setStatus(status);
    }

    // ---------------- 任务栏 ----------------
    // 用户关闭开关时主动清理，避免留下一个不再更新的旧进度条
    // 停在半路的旧进度比"没有进度"更容易误导：用户会以为程序仍在更新、只是用量没变，
    // 因此这里选择主动清空，而不是把最后一轮的数字继续留在任务栏上。
    if (!AppConfig::taskbarBadgeEnabled())
    {
        m_taskbar.clearAll();
        return;
    }
    // 未连接任务栏（无桌面会话或系统不支持）时静默跳过，不影响其它功能
    // 除了这一步之外不再做别的错误处理：任务栏是锦上添花的展示渠道，它的失败既不该
    // 冒泡成界面错误，也不该打断托盘与主界面的更新流程。
    if (!m_taskbar.isAttached())
        return;

    // 数据无效时传 -1，让进度条回到"无进度"而不是错误地显示 0%：
    //   · TaskbarProgress 把负数解释为"清除进度条"（内部转调 clearProgress()），
    //     任务栏回到"没有进度可报"的状态，观感上就是"暂时没有数据"；
    //   · 0 则是一个完全合法的读数——"确实一点都没用"，它会把进度条画成空的 0% 槽。
    // 两者对用户的意义截然不同：0 会让人以为用量真的是零，而实际情况可能只是这一轮
    // 抓取失败或该档数据缺失。宁可显示"没有进度"，也不能用 0 冒充一个读数。
    m_taskbar.setProgress(metric.valid ? metric.percent : -1);
    // 角标同样受 valid 控制：数据无效时传入 false，由 TaskbarProgress 清除已有角标，
    // 避免上一轮的数字继续挂在任务栏上冒充新数据；配色取自同一个 percent，
    // 因此进度条与角标的档位观感始终一致。
    m_taskbar.setBadge(metric.text, TrayController::severityColor(metric.percent), metric.valid);
}

/**
 * @brief 生成桌面集成的运行时状态报告，供 `--probe-ui` 自检取证。
 *
 * 只报告可被外部核对的事实，不做主观判断：例如同时给出"用户意图（trayEnabled）"
 * 与"系统实际可见状态（trayVisible）"，二者不一致时一眼就能看出是 shell 没显示，
 * 而不是程序没生效。
 *
 * @return QString，多行 "键 = 值" 文本，每行一个事实。
 * @note 之所以只报事实、不报一句 "OK"：桌面集成分三层——配置（用户意图）、对象状态
 *       （程序认为的现状）与 shell 的实际表现，三者不一致时一句 OK 会把问题整个盖住，
 *       逐项事实才能让人一眼看出是哪一层断了。
 * @note 字段顺序固定、每行一个 "键 = 值"，便于脚本逐行解析；某个值取不到时也要
 *       写出字段名加占位文本，而不是把整行删掉，否则解析方无法区分"字段不存在"
 *       与"字段存在但暂时为空"这两种情况。
 */
QString MainWindow::desktopProbeReport() const
{
    QStringList lines;
    // windowVisible 与后面的 trayVisible 必须成对阅读：两者同时为 no 时，用户既看不到
    // 窗口、也找不到托盘图标，这正是最需要被立刻发现的"幽灵状态"，单看一行发现不了。
    lines << QStringLiteral("windowVisible = %1").arg(isVisible() ? QStringLiteral("yes") : QStringLiteral("no"));
    // traySupported 来自系统能力查询，trayEnabled 来自本程序是否"当作开着"：
    // 两者分开报告才能区分"系统没有通知区域"与"用户把开关关了"这两种情况——
    // 前者的处置只能是放弃托盘，后者打开设置即可，混成一项就无从判断。
    lines << QStringLiteral("traySupported = %1")
                 .arg(m_tray && m_tray->isSupported() ? QStringLiteral("yes") : QStringLiteral("no"));
    lines << QStringLiteral("trayEnabled = %1")
                 .arg(m_tray && m_tray->isEnabled() ? QStringLiteral("yes") : QStringLiteral("no"));
    // trayEnabled 与 trayVisible 又是一对：前者是意图、后者是结果。
    // 出现"enabled=yes 却 visible=no"时，问题在 shell 侧（图标被折叠进溢出区、
    // 或被系统设置隐藏），而不是本程序没有调用显示接口。
    lines << QStringLiteral("trayVisible = %1")
                 .arg(m_tray && m_tray->isVisible() ? QStringLiteral("yes") : QStringLiteral("no"));
    // 任务栏这条链路很长：COM 绑定 → 造图标 → 提交给 shell，任何一步失败，
    // 用户看到的都是同一个现象"任务栏上没有角标"，因此必须把中间态逐项暴露出来。
    lines << QStringLiteral("taskbarAttached = %1")
                 .arg(m_taskbar.isAttached() ? QStringLiteral("yes") : QStringLiteral("no"));
    // 这两项来自 COM 调用的 HRESULT：yes 表示任务栏确实接受了进度与角标，
    // 属于"功能真的生效"的直接证据，而不是仅仅"代码执行过"。
    lines << QStringLiteral("taskbarProgressApplied = %1")
                 .arg(m_taskbar.progressApplied() ? QStringLiteral("yes") : QStringLiteral("no"));
    lines << QStringLiteral("taskbarBadgeApplied = %1")
                 .arg(m_taskbar.badgeApplied() ? QStringLiteral("yes") : QStringLiteral("no"));
    // 角标失败时把中间态一并报出：iconCreated=no 说明失败发生在"造图标"这一步
    // （GDI 侧没能产出 HICON，通常是窗口句柄或绘制上下文拿不到）；iconCreated=yes 而
    // applied=no 则说明图标造出来了、任务栏却拒绝了该句柄（句柄失效或调用时机不对）。
    // 两者的排查方向完全不同，因此必须分别报告，而不是只报一个最终结论。
    lines << QStringLiteral("taskbarBadgeIconCreated = %1")
                 .arg(m_taskbar.badgeIconCreated() ? QStringLiteral("yes") : QStringLiteral("no"));
    // HRESULT 补零打印成 8 位十六进制（如 0x80070005），便于直接对照 Windows 错误码：
    // 0x00000000 成功、0x80070005 拒绝访问（Access denied）、0x80004005 未指定错误、
    // 0x80040154 类未注册（COM 组件不可用）。补零到 8 位只是为了让多行输出纵向对齐，
    // 便于把两次运行的结果直接比对。
    lines << QStringLiteral("taskbarBadgeHresult = 0x%1")
                 .arg(static_cast<quint32>(m_taskbar.badgeHresult()), 8, 16, QLatin1Char('0'));
    // 进程完整性级别：与上面的 HRESULT 组成完整因果链——此处为 Low 且 HRESULT 是
    // 0x80070005（E_ACCESSDENIED）时，即可判定"角标没生效"是环境权限所致，
    // 而不是程序缺陷；若此处为 Medium 却仍被拒，则说明原因另有其它，需要继续排查。
    // 构建方式标识：静态版与共享版的 probe 报告字段完全相同，落盘后无法分辨来源；
    // 用与 main.cpp 自检一致的 QT_STATIC 判据写明本进程是哪份产物，让证据自证。
#ifdef QT_STATIC
    lines << QStringLiteral("build = static");
#else
    lines << QStringLiteral("build = shared");
#endif
    lines << QStringLiteral("processIntegrity = %1").arg(TaskbarProgress::processIntegrityLevel());
    // 北京时区解析结果：本静态构建未启用 ICU，IANA 名称走 Qt 的注册表映射，
    // "解析成功"还是"落到固定偏移兜底"从外部不可见，这里把事实直接写进报告。
    // id 为 Asia/Shanghai 说明走的是完整时区规则；为 UTC+08:00 形态说明走了兜底，
    // 两者对"北京时间"的当前显示都是正确的，但排查环境问题时必须能区分。
    const QTimeZone beijing = beijingTimeZone();
    lines << QStringLiteral("beijingTz = %1 (valid=%2, offset=%3)")
                 .arg(QString::fromLatin1(beijing.id()))
                 .arg(beijing.isValid() ? QStringLiteral("yes") : QStringLiteral("no"))
                 .arg(beijing.offsetFromUtc(QDateTime::currentDateTime()));
    lines << QStringLiteral("quitOnLastWindowClosed = %1")
                 .arg(QApplication::quitOnLastWindowClosed() ? QStringLiteral("yes") : QStringLiteral("no"));
    lines << QStringLiteral("closeToTray = %1")
                 .arg(AppConfig::closeToTray() ? QStringLiteral("yes") : QStringLiteral("no"));
    // metricConfigured 报的是持久化用的英文键名，而不是枚举的整数值：
    // 键名跨版本稳定、人能直接读懂，出现在自检输出里比一个数字有用得多。
    lines << QStringLiteral("metricConfigured = %1")
                 .arg(AppConfig::statusMetricKey(AppConfig::statusMetric()));
    if (m_hasMetric)
    {
        lines << QStringLiteral("metricName = %1").arg(m_lastMetric.name);
        lines << QStringLiteral("metricText = %1").arg(m_lastMetric.text);
        lines << QStringLiteral("metricPercent = %1").arg(m_lastMetric.percent);
        lines << QStringLiteral("metricValid = %1")
                     .arg(m_lastMetric.valid ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    else
    {
        // 尚未拿到快照时如实标注，避免把"没数据"误读成"指标为空"
        // 这里仍然写出 metricName 一行并注明原因，而不是整块省略：读报告的人（或
        // 解析脚本）需要能区分"程序还没拿到数据"与"报告被截断 / 根本没生成"。
        lines << QStringLiteral("metricName = (尚未取得数据)");
    }
    return lines.join(QLatin1Char('\n'));
}
