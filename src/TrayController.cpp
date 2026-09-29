// ---------------------------------------------------------------------------
//  TrayController.cpp —— 通知区域（系统托盘）图标控制器的实现
//
//  本实现只做三件事：
//    1. 建好托盘图标与右键菜单，并把用户动作统一转成信号抛给主窗口；
//    2. 把 TrayStatus 压缩成「一段百分比文字 + 一种底色」画进图标；
//    3. 维护悬停提示与菜单摘要行，让用户不打开主窗口也能看到关键数字。
//
//  阅读约定：凡是"看起来可以不这么写"的地方，都在紧邻的注释里写明为什么，
//  以便后续维护者不会因为"顺手简化"而破坏原有意图。
// ---------------------------------------------------------------------------

#include "TrayController.h"

#include <QAction>
#include <QApplication>
// QCoreApplication::applicationFilePath()：提升托盘图标时要与注册表里
// 外壳记录的 ExecutablePath 比对本程序 exe 的位置。
#include <QCoreApplication>
// QDir::toNativeSeparators()：注册表中的路径是反斜杠原生形态，比较前先转换。
#include <QDir>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPixmap>
// QSettings(NativeFormat)：读写 HKCU\Control Panel\NotifyIconSettings 下的
// 图标可见性（IsPromoted）配置，是把图标从 Win11 溢出区提进可见区的正规通道。
#include <QSettings>
#include <QSystemTrayIcon>
// QTimer::singleShot()：外壳尚未建立本程序条目时，安排一次延迟重试。
#include <QTimer>

// 匿名命名空间（anonymous namespace）：把下面的常量与配色限制在本翻译单元内。
// 这样做一是不污染全局符号表，二是防止其它源文件误用到"托盘专用"的画布尺寸。
namespace
{

    // 托盘图标画布边长（像素）。
    // 之所以用 64 而不是 16：Windows 会把位图缩放到 16/24/32 像素，画布越大、
    // 下采样时的采样点越多，文字边缘越平滑；直接用 16 像素画会明显锯齿。
    constexpr int trayCanvasSize = 64;

    // 与主界面保持一致的三档语义色：正常 / 接近上限 / 已超限。
    // 之所以在实现文件里再定义一份、而不是每处现算：配色属于"展示策略"，
    // 集中一处才能保证托盘图标、菜单与主界面三者的观感同步变化。
    const QColor colorOk(0x2E, 0xA0, 0x62);     // 绿：占用 < 60%
    const QColor colorWarn(0xE8, 0xA3, 0x3D);   // 琥珀：60% ≤ 占用 < 85%
    const QColor colorDanger(0xE0, 0x4B, 0x4B); // 红：占用 ≥ 85%

    // 无数据时的占位底色。
    // 刻意选中性灰而不是绿色：绿色会被用户读成"用量正常"，
    // 而此刻的真实语义是"还没有拿到数据"，二者必须区分开。
    const QColor colorIdle(0x8A, 0x8A, 0x8A);

} // namespace

/**
 * @brief 构造托盘控制器：创建托盘图标与右键菜单，装好全部信号接线。
 *
 * 构造阶段只"搭建"，不"显示"：QSystemTrayIcon 被创建出来但保持隐藏，
 * 是否真正出现在通知区域由后续的 setEnabled() 决定。这样调用方可以在
 * 读取用户配置之前就先构造本对象，等配置读出来再决定是否启用。
 *
 * 同时把菜单项与图标点击全部接成信号（而不是自己弹窗、自己开窗口），
 * 使本类不依赖任何窗口类，保持单向依赖：主窗口 → 托盘控制器。
 *
 * @param[in]     parent QObject*，父对象指针；传 nullptr 表示由调用方自行
 *                管理生命周期。传入后会挂到 Qt 对象树上，随父对象一起析构。
 * @return 无。
 * @note 必须在 QApplication 构造之后创建：QSystemTrayIcon 依赖 GUI 事件循环，
 *       在 QApplication 之前构造属于未定义行为。
 */
TrayController::TrayController(QObject *parent)
    : QObject(parent)
{
    // 无论系统是否支持托盘都创建对象：这样调用方只需 setEnabled()，
    // 不必在每处调用点判断"当前系统有没有通知区域"。
    // 由此换来的是"哪里都能安全调用"的统一语义，代价只是一个几十字节的对象。
    m_tray = new QSystemTrayIcon(this);
    // 传入 this 作为父对象：m_tray 会挂到 Qt 对象树上，析构时自动释放。
    m_menu = new QMenu();
    // 菜单刻意不设父对象：QMenu 若设了父窗口，在部分场景下会被当作窗口子控件
    // 参与布局；这里只把它当普通对象用，因此生命周期由本类手动管理（见析构函数）。

    // 菜单顶部放一行不可点击的摘要，让用户右键就能看到关键数字。
    // 该行是 QAction 而非 QLabel：QMenu 只接受 QAction，且这样能免去自绘。
    m_headerAction = m_menu->addAction(QStringLiteral("Command Code 用量"));
    // 置灰（禁用）表示"这是一条信息，不是命令"：若可点击，用户会以为点了有反应。
    m_headerAction->setEnabled(false);
    // 摘要行与动作组之间加分隔线，视觉上把"信息"和"命令"两类条目区分开。
    m_menu->addSeparator();

    // 四个动作按"使用频率 + 破坏性"排序：先显示、再刷新、再设置，最后才是退出。
    // 每个动作都带 &X 助记符，便于键盘用户在菜单里直接按字母触发。
    QAction *showAction = m_menu->addAction(QStringLiteral("显示主窗口(&O)"));
    QAction *refreshAction = m_menu->addAction(QStringLiteral("立即刷新(&R)"));
    QAction *settingsAction = m_menu->addAction(QStringLiteral("设置(&S)…"));
    // 退出前再插一条分隔线：它与上面三个"日常操作"性质不同，误点代价最高。
    m_menu->addSeparator();
    QAction *quitAction = m_menu->addAction(QStringLiteral("退出(&Q)"));

    // 菜单项一律转发成信号、不在本类里直接开窗口或退出进程：
    // 主窗口才知道该怎么响应（例如退出前保存配置），本类只负责"报告用户意图"。
    connect(showAction, &QAction::triggered, this, &TrayController::showWindowRequested);
    connect(refreshAction, &QAction::triggered, this, &TrayController::refreshRequested);
    connect(settingsAction, &QAction::triggered, this, &TrayController::settingsRequested);
    connect(quitAction, &QAction::triggered, this, &TrayController::quitRequested);

    // 把菜单挂到托盘图标上：右键图标时由 QSystemTrayIcon 负责弹出与定位。
    m_tray->setContextMenu(m_menu);

    // 双击/单击图标即唤出主窗口；这里用 lambda 而非槽函数，是为了避免在
    // 头文件里引入 QSystemTrayIcon::ActivationReason 这一平台相关枚举。
    // 匿名 lambda 只出现在本 .cpp 内，头文件因此无需包含 QSystemTrayIcon 的完整定义，
    // 减小了编译耦合（平台相关头文件不外泄）。
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason)
            {
                // 只响应"单击"与"双击"两种激活方式：
                // MiddleClick/Context 等其余取值在 Windows 上语义不稳定，不予处理，
                // 避免用户误触中键就弹出窗口。
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    emit showWindowRequested();
            });

    // 先给一个占位图标，避免 setEnabled(true) 的瞬间出现空白方框。
    // 缓存值同时初始化，使首次 setStatus() 一定能判定为"内容变了"从而触发真实绘制。
    m_lastDrawnText.clear();
    m_lastDrawnColor = colorIdle;
    // 长破折号"——"配合灰底，直观表达"暂无数据"，比空白或问号更友好。
    m_tray->setIcon(QIcon(makeTrayPixmap(QStringLiteral("—"), colorIdle, false)));
    // 悬停提示也同步生成一次，保证未收到任何数据时鼠标悬停也有可读文案。
    rebuildTooltip();
}

/**
 * @brief 析构：隐藏并释放托盘图标与右键菜单。
 *
 * 析构顺序很关键——先 hide() 再让对象树收拾残局：若在图标仍可见时直接销毁，
 * Windows 通知区域会残留一个"幽灵图标"，直到用户把鼠标划过去才消失。
 *
 * @return 无。
 * @note 本函数不抛异常，也不依赖事件循环，可在程序退出的任意时机调用。
 */
TrayController::~TrayController()
{
    // QSystemTrayIcon 的父对象是本对象，析构时会自动清理；菜单没有父对象，
    // 必须显式删除，否则会泄漏（QMenu 不参与 Qt 的对象树）。
    // 这里的 if 判断属于防御性写法：即使 m_tray 因异常路径为 nullptr，也不会解引用空指针。
    if (m_tray)
        m_tray->hide();
    // 手动 delete 后立刻置空，形成"指针要么有效、要么为空"的不变式，
    // 便于排错时一眼看出它已被释放。
    delete m_menu;
    m_menu = nullptr;
}

/**
 * @brief 查询当前系统/会话是否提供通知区域（托盘）。
 *
 * 这是一个"环境探测"接口，只转发给 Qt 的平台实现，不读任何成员状态，
 * 因此可以在构造之后、启用之前随时调用，用于决定设置项是否置灰。
 *
 * @return bool，true 表示可以显示托盘图标；false 表示当前平台（或在某些
 *         受限会话、远程桌面精简模式下）没有通知区域。
 */
bool TrayController::isSupported() const
{
    // 直接询问 Qt 而非自行拼接平台判断：不同 Windows 版本/会话的可用性差异
    // 由 Qt 统一处理，本类不重复实现这套探测逻辑。
    return QSystemTrayIcon::isSystemTrayAvailable();
}

/**
 * @brief 查询用户意图层面的"是否已启用托盘图标"。
 *
 * 记录的是本类自己的开关状态，而非图标的真实可见性；因此即使图标因为
 * 系统限制没有画出来，只要用户开过，本函数依旧返回 true。
 *
 * @return bool，true 表示用户已开启该功能。
 */
bool TrayController::isEnabled() const
{
    // 纯读取成员，不做任何平台探测：保证该函数足够轻量，
    // 可以被界面在每次重绘时无顾虑地调用。
    return m_enabled;
}

/**
 * @brief 询问底层托盘图标的真实可见状态。
 *
 * 与 isEnabled() 的区别在于：isEnabled() 表示"用户想要开"，
 * 本函数表示"Qt 认为它已经显示出来了"。自检模块据此区分
 * "设置已生效"与"系统真的把图标画出来了"这两种不同结论。
 *
 * @return bool，true 表示 QSystemTrayIcon 自认为可见。
 * @note 先判空再取状态：对象构造失败或已进入析构流程时也不会崩溃。
 */
bool TrayController::isVisible() const
{
    // 用短路求值一次性完成"指针非空 + 图标可见"两个判断，
    // 既避免了空指针解引用，也省去了一层嵌套 if。
    return m_tray && m_tray->isVisible();
}

/**
 * @brief 查询"关闭窗口时的气泡提示"是否已经弹过。
 *
 * 只读取内存标志位，不向系统查询气泡的真实显示结果：勿扰模式、通知权限都可能让
 * 气泡根本不出现，而这一点无法从 Qt 侧同步获知。正因如此，调用方应在**发起提示后
 * 立刻置位**（见 MainWindow::closeEvent()），而不是等一个并不存在的"已显示"回执。
 *
 * @return bool，true 表示本次运行已经**发起过**气泡请求（不代表系统真的把它显示出来了）。
 * @note 纯读取、无副作用，可在关闭流程中被反复调用。
 */
bool TrayController::isBubbleShown() const
{
    return m_bubbleShown;
}

/**
 * @brief 显示或隐藏托盘图标（对应设置里的开关）。
 *
 * 关闭时只隐藏、不销毁对象，因此再次开启无需重新接线，状态（上次数据）也得以保留。
 * 开启时会强制重绘一次当前状态，防止上一次关闭时留下的旧图标内容被复用。
 *
 * @param[in]     enabled bool，true 显示托盘图标，false 隐藏。
 * @return 无。
 * @note 在系统不支持托盘的机器上调用本函数是安全的：开启请求会被静默忽略。
 */
void TrayController::setEnabled(bool enabled)
{
    // 系统不支持托盘时静默忽略：让「设置里勾了但系统没有托盘」这种组合
    // 退化为"功能不生效"，而不是抛错或崩溃。
    // 注意只拦"开启"：关闭请求永远放行，否则会对不支持的机器留下一个假的开状态。
    if (enabled && !isSupported())
        return;

    // 先落状态再干活：即使后面的 show()/hide() 出错，成员状态也与用户意图一致。
    m_enabled = enabled;
    if (enabled)
    {
        // 显示前强制重绘一次，防止上一次关闭时留下的旧内容被复用。
        // 清空缓存文字等价于"缓存失效"，从而让 setStatus() 内部的比较必然不相等。
        m_lastDrawnText.clear();
        // 传入最近一次的数据：用户可能是在收到若干次刷新之后才打开托盘的，
        // 这里必须立刻显示出最新数字，而不是等下一次刷新。
        setStatus(m_status);
        // 放到最后显示：此时图标位图与提示文本都已就绪，
        // 用户看到的第一帧就是完整内容，不会闪一下空白。
        m_tray->show();
        // 显示成功后立刻尝试提升到可见区（Windows 11 默认把新图标收进溢出区）：
        // 外壳的注册条目可能要等图标真正注册后才落盘，因此这里先试一次，
        // 失败再安排一次 2 秒后的重试；两次都拿不到条目就放弃，属预期内的降级，
        // 图标本身仍会在溢出区里可用，用户手动提升同样有效。
        if (!promoteToVisibleTrayArea())
        {
            // 重试挂在定时器上而不是循环等待：绝不阻塞创建托盘的调用方，
            // 2 秒是"外壳完成图标注册"的宽限量级，通常首次尝试就已成功。
            QTimer::singleShot(2000, this, [this]()
                               {
                // 重试前核对开关仍然开启：若用户在这 2 秒里关掉了托盘，
                // 就不应再为已经隐藏的图标做任何注册表改动。
                if (m_enabled)
                    promoteToVisibleTrayArea(); });
        }
    }
    else
    {
        // 只隐藏不销毁：保留对象与已注册的菜单，随时可以零成本再次开启。
        m_tray->hide();
    }
}

/**
 * @brief 记录"关闭窗口时的气泡提示"已弹过（置位后本次运行不再弹）。
 *
 * @param[in] shown bool，true 表示已提示过；传 false 可重置该状态。
 * @return 无。
 * @note 只改内存标志位、不落盘，因此程序重启后的第一次关闭会重新提示一次；
 *       若将来需要"装一次只提示一次"，应把该状态挪进 AppConfig。
 */
void TrayController::setBubbleShown(bool shown)
{
    m_bubbleShown = shown;
}

/**
 * @brief 尝试把托盘图标从 Windows 11 溢出区提升到任务栏可见区域（best-effort）。
 *
 * 实现分四步：① 取本程序 exe 的原生路径作为匹配依据；② 枚举
 * HKCU\Control Panel\NotifyIconSettings 的全部子项——子项名是外壳的私有哈希、
 * 无法反推，只能靠 ExecutablePath 值逐个比对；③ 找到匹配项后，仅当 IsPromoted
 * 还不是 1 时写入 1（幂等，避免每次启动都改注册表）；④ 先隐藏再显示图标，
 * 迫使外壳按新状态重新注册，用户无需重启程序即可看到图标出现在可见区。
 *
 * @return bool，true=条目存在且已处于/已被置为提升状态；false=条目尚不存在。
 * @note 全程不抛错、不弹窗：注册表不可写、条目缺失都属于"环境暂时不配合"，
 *       静默返回 false 即可；图标本身照常显示（哪怕在溢出区），功能没有损失。
 */
bool TrayController::promoteToVisibleTrayArea()
{
    // 匹配依据：本进程可执行文件的原生路径。外壳记录的 ExecutablePath 就是这个形态，
    // 但大小写与目录写法可能不一致，因此比较时统一转原生分隔符并忽略大小写。
    const QString selfPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    // 只打开通知区域图标的注册表节点：NativeFormat 让 QSettings 直接以该键为根，
    // childGroups() 返回的每个组名就是外壳为一个图标建立的子项（私有哈希）。
    QSettings notifyIcons(QStringLiteral("HKEY_CURRENT_USER\\Control Panel\\NotifyIconSettings"),
                          QSettings::NativeFormat);
    const QStringList groups = notifyIcons.childGroups();
    for (const QString &group : groups)
    {
        // 逐个子项读 ExecutablePath 并与本程序比对；其它程序的条目一律不碰，
        // 这是把"提升自己"限定在"自己的图标"上的唯一判据。
        const QString entryPath = notifyIcons.value(group + QStringLiteral("/ExecutablePath")).toString();
        if (entryPath.compare(selfPath, Qt::CaseInsensitive) != 0)
            continue;

        // 已是 1（用户自己开过，或之前提升过）就不必再动注册表，也不必闪一次图标。
        if (notifyIcons.value(group + QStringLiteral("/IsPromoted"), 0).toInt() == 1)
            return true;

        // 写入提升标记并立即刷盘：外壳按此值决定图标摆在可见区还是溢出区。
        notifyIcons.setValue(group + QStringLiteral("/IsPromoted"), 1);
        notifyIcons.sync();

        // 先隐藏再显示 = 对外壳执行一次 NIM_DELETE + NIM_ADD，让提升状态立即生效；
        // 显示前重置绘制缓存并重放当前数据，保证重新出现的图标内容不缺失。
        m_tray->hide();
        m_lastDrawnText.clear();
        setStatus(m_status);
        m_tray->show();
        return true;
    }
    // 走到这里说明外壳还没有为本程序建立条目：通常是图标刚注册、外壳尚未落盘，
    // 或注册根本没成功（如低完整性等环境限制）。返回 false 让调用方决定是否重试。
    return false;
}

/**
 * @brief 用最新数据刷新图标位图、菜单摘要行与悬停提示。
 *
 * 这是本类被调用最频繁的函数（每次数据刷新都会走一遍），因此做了两处省力：
 * 一是只有"文字或颜色真的变了"才重绘位图；二是文本类更新纯走字符串拼接。
 * 在没有数据时会退化到灰色占位图标，保证视觉上不会出现空白方块。
 *
 * @param[in]     status TrayStatus，由主窗口算好的展示数据；未取到数据时
 *                status.valid 为 false，此时忽略各文本字段并显示占位态。
 * @return 无。
 * @note 本函数可在没有托盘的环境下安全调用：此时只更新内部缓存与菜单文字，
 *       并不会产生任何平台调用，因此调用方不需要额外判空或判断平台。
 */
void TrayController::setStatus(const TrayStatus &status)
{
    // 无论后续是否重绘，都先把最新数据记下来：
    // setEnabled(true) 与 rebuildTooltip() 都依赖这份缓存，记早了才不会有陈旧值。
    m_status = status;

    // 文字为空时退回一个长破折号占位，避免图标上出现一片纯色、看不出含义。
    const QString text = status.metricText.isEmpty() ? QStringLiteral("—") : status.metricText;
    // 配色分两路：有数据时按占用率选语义色，没数据时统一用中性灰。
    // 这里不对 metricPercent 做范围校验，是因为它的取值来源（主窗口）已保证落在 0~100。
    const QColor color = status.valid ? severityColor(status.metricPercent) : colorIdle;

    // 只有文字或颜色真的变了才重绘：Windows 下每次 setIcon() 都要走一遍
    // QPixmap → HICON 的转换，属于相对昂贵的操作。
    // 用量刷新多以秒计，绝大多数刷新周期的文字与颜色是完全相同的，
    // 靠这一层比较即可把绝大部分重绘省掉，同时不影响正确性。
    if (text != m_lastDrawnText || color != m_lastDrawnColor)
    {
        // 先更新缓存再绘制：即使绘制过程中被再次进入，缓存也已经是新值，
        // 不会出现"画了新内容却仍记着旧内容"的不一致。
        m_lastDrawnText = text;
        m_lastDrawnColor = color;
        // 第三个参数 valid 决定底色是否使用传入色：无效数据强制走灰色分支，
        // 使"数据状态"与"配色"这两件事在绘制函数里也保持单一判据。
        m_tray->setIcon(QIcon(makeTrayPixmap(text, color, status.valid)));
    }

    // 菜单顶部摘要行与悬停提示都随数据变化，直接重算即可（纯字符串操作，开销极低）。
    // 与位图不同，这两处不设缓存：字符串比较本身的成本与直接重建相当，
    // 加缓存反而增加状态、得不偿失。
    m_headerAction->setText(status.valid
                                ? QStringLiteral("%1　%2 %3").arg(status.planName, status.metricName, status.metricText)
                                : QStringLiteral("尚未获取到数据"));
    rebuildTooltip();
}

/**
 * @brief 弹出一条系统气泡通知。
 *
 * 仅在用户已开启托盘时才有意义：没有图标就没有"气泡的锚点"，
 * 因此未开启时直接返回，而不是转投其它提示通道。
 *
 * @param[in]     title QString，气泡标题。
 * @param[in]     message QString，气泡正文。
 * @param[in]     warning bool，true 用警告图标、false 用信息图标；默认 false。
 * @return 无。
 * @note 通知的显示与否最终由系统设置决定（勿扰模式、通知权限等），
 *       本函数不等待、不重试、不阻塞界面线程。
 */
void TrayController::showMessage(const QString &title, const QString &message, bool warning)
{
    // 未开启托盘时直接返回：既避免向系统提交无人接收的通知，
    // 也避免在没有图标的情况下产生一条"孤儿气泡"。
    if (!m_enabled)
        return; // 用户没开托盘时不打扰
    // 图标类型按调用方给出的严重级别二选一：本类不自行推断，
    // 以免把"信息"误升格为"警告"而制造焦虑。
    m_tray->showMessage(title, message,
                        warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information,
                        5000); // 5 秒后自动消失，和系统通知的常见时长保持一致
    // 采用超时自动消失而非等待用户点击：用量提示属于背景信息，
    // 不应要求用户做出任何"确认"动作。
}

/**
 * @brief 依据占用百分比返回语义色（绿 / 琥珀 / 红）。
 *
 * 阈值与主界面进度条完全一致，这是刻意的：同一个百分比若在托盘显示为琥珀、
 * 在界面显示为绿色，用户会怀疑数据出错。公开为静态函数是为了让任务栏角标
 * 与托盘图标共用同一套规则，杜绝"改了一处忘了另一处"的配色漂移。
 *
 * @param[in]     percent int，占用百分比（0~100）；超出该范围也不会异常，
 *                只会落入最低或最高档。
 * @return QColor，< 60 返回绿色、60~84 返回琥珀色、≥ 85 返回红色。
 */
QColor TrayController::severityColor(int percent)
{
    // 先判最高档：越界数值（如 120）会被正确归入"危险"，
    // 因为判断顺序保证它不可能落到前两个分支之外。
    if (percent >= 85)
        return colorDanger;
    // 再判中间档：85 以上的情况已在上面返回，这里只需处理 60~84。
    if (percent >= 60)
        return colorWarn;
    // 兜底档即"正常"：不需要再写 percent < 60 的比较，
    // 少一次判断、也少一处可能写错的边界。
    return colorOk;
}

/**
 * @brief 把缩略文字绘制成托盘图标位图（圆角底色 + 居中白字）。
 *
 * 采用"高分辨率画布 + 下采样"策略：先按 64×64 绘制，再由 Windows 缩放到
 * 实际使用的 16/24/32 像素，从而获得更平滑的文字边缘。字号随字符数分档，
 * 目的是让 "15%" 这类常见值尽量大，同时保证 "100%" 不会溢出圆角方块。
 *
 * @param[in]     text QString，要绘制的缩略文字，如 "15%"；空串时按 1 个字符处理。
 * @param[in]     background QColor，图标底色；仅当 valid 为 true 时生效。
 * @param[in]     valid bool，true 使用 background 配色，false 强制使用灰色占位底色。
 * @return QPixmap，可直接交给 QSystemTrayIcon::setIcon() 的位图。
 * @note 本函数为静态、无副作用：不读也不写任何成员，因此可在构造期间
 *       （成员尚未初始化完毕时）安全调用。
 */
QPixmap TrayController::makeTrayPixmap(const QString &text, const QColor &background, bool valid)
{
    // 用 64×64 画布而不是 16×16：画布越大，Windows 缩放到目标尺寸时的
    // 下采样采样点越多，文字边缘越平滑；直接在 16×16 上画会有明显锯齿与粘连。
    QPixmap pixmap(trayCanvasSize, trayCanvasSize);
    // 先填成透明：这样圆角之外的四个角是"镂空"的，
    // 托盘背景能透出来，图标看起来才像原生图标而不是小方块贴图。
    pixmap.fill(Qt::transparent);

    // 画笔作用域仅限本函数：painter 析构时自动 end()，
    // 保证返回的 pixmap 一定处于"绘制已提交"的状态。
    QPainter painter(&pixmap);
    // 开启抗锯齿让圆角边缘平滑；两个开关分别管图形与文字，
    // 文字单独开是因为部分平台下两者由不同的后端路径处理。
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // 圆角方块作为底色，留 2 像素边距，避免缩放后边缘被裁掉。
    // 边距取 2 是因为 64 → 16 是 1/4 缩放，2 像素恰好对应最终 0.5 像素的抗锯齿过渡带。
    const QRectF plate(2.0, 2.0, trayCanvasSize - 4.0, trayCanvasSize - 4.0);
    // 去掉描边：高分辨率下 1 像素描边缩放后会变成一圈灰晕，反而显脏。
    painter.setPen(Qt::NoPen);
    // 数据无效时无视调用方传入的颜色，统一用灰色：
    // 把"是否有效"这一个判据固化在绘制函数内部，调用方就不可能画出自相矛盾的图标。
    painter.setBrush(valid ? background : colorIdle);
    // 圆角半径 14 约为边长 60 的 23%，是"圆角方块"观感与可用面积之间的折中；
    // 再大就会挤压文字空间，再小则接近直角、失去辨识度。
    painter.drawRoundedRect(plate, 14.0, 14.0);

    // 字号随字符数自适应：3 个字符（如 "15%"）可用大字号，
    // 4~5 个字符（如 "100%"）必须缩小，否则会溢出圆角方块。
    // 先取 max(1, ...) 是因为空串长度为 0，若直接参与分档会落到异常分支。
    const int length = qMax(1, text.size());
    // 四档字号分别对应 1~2 字符、3 字符、4 字符、≥5 字符：先用固定档位保证
    // 常见取值之间的字号稳定，避免同一数值在不同刷新之间跳字号。
    int pixelSize = length <= 2 ? 44 : (length <= 3 ? 34 : (length <= 4 ? 26 : 20));

    // 基于 painter 当前字体复制一份再改，而不是新建 QFont：
    // 这样可以继承平台的默认字形与 hinting 设置，只覆盖需要的两项属性。
    QFont font = painter.font();
    font.setPixelSize(pixelSize);
    // 加粗：缩小到 16 像素后笔画会更细，加粗能显著提升小尺寸下的可读性。
    font.setBold(true);
    // 度量兜底：固定档位是按"数字较窄"估算的，个别组合（如 "4%" 的百分号很宽、
    // 剩余额度模式的两位数）实测仍会顶到甚至溢出圆角方块，白字露出底色反而看不清
    // （用户实测反馈）。因此用字体度量实测文字宽度，超宽就每级缩 2px 重测，
    // 直到放得下或到达可读下限——"绿底包住文字"最终由度量保证，
    // 档位只负责常规情形下的字号稳定。
    // 垂直方向无需兜底：档位上限 44px（≤2 字符档）的字形高远小于 60px 方块高，
    // 上下余量由档位上限自然保证。
    const int maxTextWidth = trayCanvasSize - 12; // 左右各留 6px 安全边距
    QFontMetrics metrics(font);
    while (metrics.horizontalAdvance(text) > maxTextWidth && pixelSize > 14)
    {
        pixelSize -= 2;
        font.setPixelSize(pixelSize);
        metrics = QFontMetrics(font);
    }
    painter.setFont(font);

    // 文字固定白色：三档底色（绿/琥珀/红）与灰色都足够深，
    // 白色在它们之上的对比度均满足可读性要求，无需按底色反算前景色。
    painter.setPen(QColor(0xFF, 0xFF, 0xFF));
    // 在底色矩形内居中绘制；用 plate 而非整个画布，
    // 是为了让视觉居中与"看起来的方块"一致，而不是与含边距的画布一致。
    painter.drawText(plate, Qt::AlignCenter, text);

    // 返回按引用计数管理的位图，调用方包进 QIcon 后即可交给系统，
    // 本函数不持有任何长期资源，不需要额外的释放约定。
    return pixmap;
}

/**
 * @brief 重建悬停提示文本（套餐、剩余额度、指标明细、更新时间与错误）。
 *
 * 提示文本按"信息重要度递减"排列：标题 → 套餐与剩余额度 → 当前指标 →
 * 明细 → 更新时间 → 错误。这样用户把鼠标移上去时，第一屏就能看到最关键的数字。
 * 各可选字段（明细、更新时间、错误）为空时不占行，避免出现空行。
 *
 * @return 无。
 * @note 每次数据刷新都会重建，属于纯字符串操作，开销远低于图标重绘，
 *       因此不做"内容未变则跳过"的缓存。
 */
void TrayController::rebuildTooltip()
{
    // 未开启托盘时直接返回：图标都不可见，提示文本没有任何展示机会，
    // 提前返回可以省掉后面一串字符串拼接。
    if (!m_enabled)
        return;

    // 无数据分支单独处理：此时除标题外没有可信字段可展示，
    // 用固定两行文案给出明确结论，好过拼接出一堆空白字段。
    if (!m_status.valid)
    {
        m_tray->setToolTip(QStringLiteral("Command Code 套餐用量\n尚未获取到数据"));
        return;
    }

    // 逐行累积再一次性提交，避免多次调用 setToolTip() 造成重复的重绘与刷新。
    QStringList lines;
    // 第一行固定为产品名：Windows 会在气泡/提示里加粗首行，正好当作标题使用。
    lines << QStringLiteral("Command Code 套餐用量");
    // 第二行给出套餐名与额度：这是用户最关心的两项，故放在最靠前的位置。
    // 剩余额度与总额度统一保留两位小数，保证同一屏内数值宽度稳定、不跳动。
    lines << QStringLiteral("套餐：%1　剩余 %2 / %3 credits")
                 .arg(m_status.planName)
                 .arg(m_status.remaining, 0, 'f', 2)
                 .arg(m_status.total, 0, 'f', 2);
    // 第三行是当前所选指标（名称与缩略文字），与图标上显示的内容一一对应。
    lines << QStringLiteral("%1：%2").arg(m_status.metricName, m_status.metricText);
    // 明细为可选字段：为空说明数据源没提供，此时整行略去而不是留一个空标题。
    if (!m_status.detailText.isEmpty())
        lines << m_status.detailText;
    // 更新时间同样可选：缺失时省略，以免显示成"最后更新："这种半截文案。
    if (!m_status.updatedText.isEmpty())
        lines << QStringLiteral("最后更新：%1").arg(m_status.updatedText);

    // 错误只在提示里"附一行"，不弹气泡，避免每次刷新都打扰用户。
    // 多个错误用中文分号连接成一行，既保留全部信息又不会把提示撑得太长。
    if (!m_status.errors.isEmpty())
        lines << QStringLiteral("⚠ %1").arg(m_status.errors.join(QStringLiteral("；")));

    // 用换行符拼接成多行文本一次性提交：Windows 的提示框按 \n 自动分行，
    // 结合 QLatin1Char 拼接可避免一次从 QString 到本地编码的转换。
    m_tray->setToolTip(lines.join(QLatin1Char('\n')));
}
