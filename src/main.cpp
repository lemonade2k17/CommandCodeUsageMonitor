// ---------------------------------------------------------------------------
//  main.cpp —— Command Code 套餐用量查看器的程序入口
//
//  职责：解析命令行参数，决定「打开 GUI 主窗口」还是「跑一次无界面自检」。
//  普通启动：构造 QApplication 后创建 MainWindow 并显示；API Key 由用户在
//            「设置」中填写，默认留空，留空时界面可用但取不到真实用量。
//  自检模式：--selftest 下依次调用 whoami / credits / subscriptions / summary
//            四个接口，把结果打印到 stdout 与 --out 指定的文件后退出。
//  临时覆盖：--key / --base-url 只对本次运行生效，绝不写入配置文件。
//  退出码：0 = 自检全部成功；1 = 自检部分失败或 45 秒超时；2 = 没有 API Key。
//  平台差异：Windows 上本程序按 WIN32（GUI）子系统链接，从控制台启动时
//            stdout 可能拿不到（不会自动继承父控制台），故自检模式提供 --out
//            文件通道，并尝试附挂父进程控制台后再把标准流重定向过去。
// ---------------------------------------------------------------------------
#include "AppConfig.h"
#include "CommandCodeApi.h"
#include "MainWindow.h"
// 桌面集成诊断：自检里报告托盘与任务栏接口在当前系统上是否可用。
#include "TaskbarProgress.h"
#include "UsageTypes.h"

// Qt 头文件分组：先应用层，再命令行解析，再时间/路径/事件循环/文件/定时器。
// 之所以显式包含 QEventLoop 与 QTimer，是因为自检模式要自己跑一个局部事件循环。
#include "AppTheme.h"

#include <QApplication>
// QCommandLineOption / QCommandLineParser：命令行选项的定义与解析。
#include <QCommandLineOption>
#include <QCommandLineParser>
// QDateTime：把套餐周期与配额重置时间格式化成人可读的字符串。
#include <QDateTime>
// QDir：把配置文件路径转成 Windows 风格的反斜杠形式，方便直接复制粘贴。
#include <QDir>
// QEventLoop：自检模式必须自己驱动事件循环，否则网络请求的回调永远不会被派发。
#include <QEventLoop>
// QFile：--out 输出文件，以及探测配置文件是否存在。
#include <QFile>
// QLocalServer / QLocalSocket：单实例守护——第二个实例借此把 SHOW / QUIT 命令
// 发给已在运行的实例，是"窗口躲进托盘后找不回来"的逃生通道。
#include <QLocalServer>
#include <QLocalSocket>
// QSystemTrayIcon：仅用于查询"当前系统是否提供通知区域"，不创建任何图标对象。
#include <QSystemTrayIcon>
// QTextStream：把自检输出同时送到 stdout 与文件。
#include <QTextStream>
// QTimer：实现 45 秒总超时，以及 GUI 启动后延后发起首次刷新。
#include <QTimer>

// Windows 专有部分：AttachConsole / ATTACH_PARENT_PROCESS / freopen_s 的来源。
// 非 Windows 平台上这段整体被裁掉，因此相关调用必须用同名宏包住。
#ifdef Q_OS_WIN
#  include <windows.h>
#  include <cstdio>
#endif

// 匿名命名空间：本文件私有的自检辅助设施，不导出符号，
// 避免与其他翻译单元中的同名函数或变量在链接期发生冲突。
namespace {

// 全局（本文件私有）输出文件路径；为空表示「只写 stdout，不写文件」。
// 之所以用文件级变量而不是函数参数：logLine 会被自检流程在多个位置调用，
// 逐层透传路径会污染所有调用点，而自检是单线程顺序执行，无并发写入风险。
QString g_outFilePath;

/**
 * @brief 自检流程的统一输出函数：把一行文本同时写入 stdout 与可选的输出文件。
 *
 * 在 --selftest 分支中被反复调用，覆盖头部信息、四个接口的返回字段与最终结论。
 * 处理步骤：
 *   1) 用 QTextStream 把该行写向 stdout 并立即 flush，保证进程退出前输出不丢；
 *   2) 若 g_outFilePath 非空，则以「只写 + 追加」方式打开文件并写入该行；
 *   3) 每次调用都重新开关文件，牺牲一点性能换取「崩溃也不丢前面的日志」。
 *
 * 边界条件：stdout 无控制台时写入会静默失败（这正是 --out 存在的理由）；
 *           文件打不开时（例如目录不存在、被占用）静默跳过文件分支，不影响自检。
 *
 * @param[in] line QString，待输出的一整行文本（不含换行符，函数内部补 \n）。
 * @return 无。
 * @note 本函数只做输出，不改变自检状态；GUI 子系统下 stdout 可能不可见，
 *       排查问题时请以 --out 写出的文件为准。
 */
void logLine(const QString &line)
{
    // 每次调用都新建 QTextStream：不缓存文件/流状态，避免跨调用的生命周期问题。
    QTextStream stream(stdout);
    stream << line << "\n";
    // 立即刷新：自检可能在网络异常后立刻退出，缓冲区若未落盘就会丢掉关键错误。
    stream.flush();

    // 只有显式给了 --out 才走文件通道；未给时保持「纯控制台」的传统行为。
    if (!g_outFilePath.isEmpty()) {
        QFile file(g_outFilePath);
        // 追加模式打开：配合 main 中「先删除旧文件」的策略，实现本次运行独立成篇。
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            // 显式转 UTF-8，避免中文错误信息在 Windows 记事本里出现乱码。
            file.write(line.toUtf8());
            file.write("\n");
            file.close();
        }
    }
}

// Windows：把 GUI 子系统进程的 stdout/stderr 接到父进程控制台上。
// 非 Windows 平台没有该概念，整段条件编译掉；调用点因此也必须同步条件编译。
#ifdef Q_OS_WIN
/**
 * @brief 尝试附挂父进程控制台，并把 stdout / stderr 重新指向控制台设备。
 *
 * 调用时机：仅在 --selftest 分支、且位于 Windows 平台时，于 runSelfTest 之前调用。
 * 处理步骤：
 *   1) AttachConsole(ATTACH_PARENT_PROCESS) 尝试挂到启动本程序的父进程控制台；
 *   2) 挂接成功后用 freopen_s 把 stdout 与 stderr 重新绑定到 "CONOUT$"。
 *
 * 为什么需要它：本程序以 WIN32（GUI）子系统链接，进程启动时并没有控制台，
 * 标准流默认指向无效句柄，printf / QTextStream(stdout) 的输出会丢失；
 * 附挂并重定向之后，用户在 cmd/PowerShell 里就能直接看到自检结果。
 *
 * 边界条件：从资源管理器双击启动（没有父控制台）或父进程无控制台时，
 *           AttachConsole 会失败，此时函数什么都不做——这是预期行为，
 *           不是错误，用户仍可通过 --out 文件拿到结果。
 *
 * @return 无。
 * @note 仅在 Q_OS_WIN 下编译；调用失败不报错、不改变退出码，属「尽力而为」。
 */
void tryAttachParentConsole()
{
    // 只有挂上了父控制台，重定向到 CONOUT$ 才有意义；失败直接放弃。
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *dummy = nullptr;
        // freopen_s 的旧句柄出参用不上；重定向后 QTextStream(stdout) 才能落到控制台。
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
}
#endif

/**
 * @brief 执行一次无界面自检：调用 whoami/credits/subscriptions/summary 并打印结果。
 *
 * 调用时机：main 检测到 --selftest 之后、且已设置好 g_outFilePath 并附挂控制台时。
 * 处理步骤：
 *   1) 决定本次使用的 API Key 与 base URL（命令行覆盖优先，否则读配置）；
 *      Key 为空时立即以退出码 2 结束，不发起任何网络请求；
 *   2) 打印环境信息（baseUrl、key 长度、key 来源、配置文件路径与存在性、Qt 版本、静态/动态构建），
 *      这些字段用于快速判断「问题出在配置、网络还是链接方式」；
 *   3) 用局部 QEventLoop 等待 fetchAll() 的一次性结果：成功信号回填快照，
 *      失败信号回填错误文本，超时定时器兜底退出等待；
 *   4) 按固定格式逐行打印各字段，最后依据三个数据块的有效性给出结论与退出码。
 *
 * 关键约束：本函数不打日志文件、不改配置、不做重试；它只读取并展示。
 *
 * 边界条件：超时（45 秒）与网络失败走同一分支，都会返回 1；只要四个接口里
 *           有任何一块数据无效（credits / subscription / summary 任一 valid 为假），
 *           结论就是 SELFTEST PARTIAL 并返回 1；全部有效才返回 0。
 *
 * @param[in] apiKeyOverride QString，命令行 --key 的值；为空表示未覆盖，改用配置文件中的 Key。
 * @param[in] baseUrlOverride QString，命令行 --base-url 的值；为空表示未覆盖，改用配置文件中的地址。
 * @return int，退出码：0 = 全部成功；1 = 部分失败或超时；2 = 没有可用的 API Key。
 * @note 该函数会阻塞到网络返回或 45 秒超时为止；超时值是刻意给足余量的，
 *       因为首次 TLS 握手加四个接口串行请求在弱网下可能耗时数十秒。
 */
int runSelfTest(const QString &apiKeyOverride, const QString &baseUrlOverride)
{
    // 命令行覆盖优先于配置文件：便于在不污染用户配置的前提下验证另一套凭据。
    const QString key = apiKeyOverride.isEmpty() ? AppConfig::apiKey() : apiKeyOverride;
    // 没有 Key 就没有必要联网：直接给出明确提示并以专用退出码 2 结束，
    // 让脚本能区分「环境没配好」与「网络/服务端故障」这两类完全不同的失败。
    if (key.isEmpty()) {
        logLine(QStringLiteral("SELFTEST FAIL: 没有 API Key（--key 未提供，配置里也为空）"));
        return 2;
    }

    // 局部构造 API 对象：生命周期覆盖整个自检过程，退出函数时一并析构。
    CommandCodeApi api;
    // 注入本次使用的 Key（可能是临时覆盖值，不落盘）。
    api.setApiKey(key);
    // 地址同样遵循「命令行覆盖优先」；命令行给的是字符串，此处转成 QUrl。
    api.setBaseUrl(baseUrlOverride.isEmpty() ? AppConfig::baseUrl() : QUrl(baseUrlOverride));

    // 以下若干行是「环境快照」，出问题时先看这几行即可定位范围：
    // 实际请求地址是否正确（baseUrl）。
    logLine(QStringLiteral("SELFTEST: baseUrl = %1").arg(api.baseUrl().toString()));
    // Key 只打印长度而不打印内容，既够排查「是否读到值」，又避免凭据泄漏到日志。
    logLine(QStringLiteral("SELFTEST: keyLength = %1").arg(key.size()));
    // Key 来源（配置文件 / 命令行 --key）：确认覆盖是否按预期生效。
    logLine(QStringLiteral("SELFTEST: keySource = %1")
                .arg(apiKeyOverride.isEmpty() ? QStringLiteral("配置文件")
                                              : QStringLiteral("命令行 --key")));
    // 配置文件路径与是否存在：排查「配置写在别处」或「首次运行尚无配置」的经典问题。
    // 路径转成 Windows 原生分隔符，方便用户直接复制到资源管理器。
    logLine(QStringLiteral("SELFTEST: configFile = %1 (exists=%2)")
                .arg(QDir::toNativeSeparators(AppConfig::settingsFilePath()),
                     QFile::exists(AppConfig::settingsFilePath()) ? QStringLiteral("yes")
                                                                  : QStringLiteral("no")));
    // Qt 运行时版本：用于确认加载的是预期的那套 Qt 库。
    logLine(QStringLiteral("SELFTEST: Qt = %1").arg(QString::fromLatin1(qVersion())));
    // 桌面集成能力诊断：自检不创建主窗口，因此这里只报告"系统是否具备能力"，
    // 而不是"本进程是否已经显示成功"——后者需要真实窗口，属于 GUI 路径的职责。
    // 把这两项写进自检，是为了把"功能没生效"与"系统不支持"区分开，避免误判。
    logLine(QStringLiteral("SELFTEST: trayAvailable = %1")
                .arg(QSystemTrayIcon::isSystemTrayAvailable() ? QStringLiteral("yes")
                                                              : QStringLiteral("no")));
    logLine(QStringLiteral("SELFTEST: taskbarComAvailable = %1")
                .arg(TaskbarProgress::probe() ? QStringLiteral("yes") : QStringLiteral("no")));
    // 构建方式：静态链接（QT_STATIC）时依赖齐全，动态构建则需保证 DLL 随行，
    // 「本地能跑、拷到别的机器就不行」多半栽在这里。
#ifdef QT_STATIC
    logLine(QStringLiteral("SELFTEST: build = STATIC (QT_STATIC)"));
#else
    logLine(QStringLiteral("SELFTEST: build = SHARED"));
#endif

    // result 由信号回调写入、循环退出后读取，顺序执行故无需额外加锁。
    UsageSnapshot result;
    // got 标记「是否收到了成功快照」，用于区分成功与失败/超时两条路径。
    bool got = false;
    // error 保存失败信号携带的文本，超时情况下保持为空以区分两种失败原因。
    QString error;

    // 局部事件循环：自检是同步流程，必须自己「转」事件循环才能收到网络层的异步回调。
    QEventLoop loop;
    // 成功回调：保存快照、置位标志并退出等待（quit 只会结束这次局部等待）。
    QObject::connect(&api, &CommandCodeApi::snapshotReady, [&](const UsageSnapshot &snapshot) {
        result = snapshot;
        got = true;
        loop.quit();
    });
    // 失败回调：记录错误文本后同样退出等待，由后续分支统一判定退出码。
    QObject::connect(&api, &CommandCodeApi::failed, [&](const QString &message) {
        error = message;
        loop.quit();
    });
    // 45 秒总超时：四个接口串行请求，弱网下需要充足余量；
    // 没有这条兜底，网络半死不活时自检会永久挂起，自动化脚本将无法收敛。
    QTimer::singleShot(45000, &loop, &QEventLoop::quit);

    // 一次性发起全部取数请求（whoami / credits / subscriptions / summary）。
    api.fetchAll();
    // 阻塞在此，直到成功、失败或超时三者之一触发 quit。
    loop.exec();

    // 未拿到成功快照：要么是接口报错，要么是超时（error 为空即为超时）。
    if (!got) {
        logLine(QStringLiteral("SELFTEST FAIL: %1")
                    .arg(error.isEmpty() ? QStringLiteral("超时") : error));
        return 1;
    }

    // 下面三个引用只是给快照字段起短名，便于后续格式化语句阅读，不产生拷贝。
    const CreditsInfo &c = result.credits;
    const SubscriptionInfo &s = result.subscription;
    const UsageSummary &u = result.summary;
    // 套餐总额度（按 planId 从套餐目录查出）；后续用它把「剩余」换算成已用百分比。
    const double planTotal = PlanCatalog::totalCredits(s.planId);

    // whoami 字段：valid 为假说明该接口失败，此时用占位符避免输出半截数据。
    logLine(QStringLiteral("whoami:  %1 (%2)")
                .arg(result.whoami.valid ? result.whoami.userName : QStringLiteral("失败"),
                     result.whoami.valid ? result.whoami.email : QStringLiteral("-")));
    // plan 字段：套餐标识 / 订阅状态 / 该套餐总额度；缺失时统一用 "-" 占位。
    logLine(QStringLiteral("plan:    %1 / %2 / total=%3 credits")
                .arg(s.planId.isEmpty() ? QStringLiteral("-") : s.planId,
                     s.status.isEmpty() ? QStringLiteral("-") : s.status)
                .arg(planTotal, 0, 'f', 0));
    // period 字段：当前计费周期起止时间，用于核对「用量为何在某天清零」。
    logLine(QStringLiteral("period:  %1  ->  %2")
                .arg(s.currentPeriodStart.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                     s.currentPeriodEnd.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    // credits 字段：月额度剩余、已用（总额度减剩余）与百分比；
    // planTotal 为 0（套餐未知）时百分比按 0 处理，避免除零得到 inf/nan。
    logLine(QStringLiteral("credits: remaining=%1  used=%2  pct=%3%")
                .arg(c.monthlyRemaining, 0, 'f', 4)
                .arg(planTotal - c.monthlyRemaining, 0, 'f', 4)
                .arg(planTotal > 0 ? (planTotal - c.monthlyRemaining) / planTotal * 100.0 : 0.0, 0, 'f', 2));
    // 5h 字段：五小时滚动窗口的已用/上限/百分比与下次重置时间。
    logLine(QStringLiteral("5h:      used=%1 cap=%2 pct=%3% reset=%4")
                .arg(c.fiveHour.used, 0, 'f', 4).arg(c.fiveHour.cap, 0, 'f', 0)
                .arg(c.fiveHour.percent())
                .arg(c.fiveHour.resetAt.toString(QStringLiteral("MM-dd HH:mm"))));
    // weekly 字段：周滚动窗口的同组指标。
    logLine(QStringLiteral("weekly:  used=%1 cap=%2 pct=%3% reset=%4")
                .arg(c.weekly.used, 0, 'f', 4).arg(c.weekly.cap, 0, 'f', 0)
                .arg(c.weekly.percent())
                .arg(c.weekly.resetAt.toString(QStringLiteral("MM-dd HH:mm"))));
    // summary 字段：会话次数、总 token 数（拆成输入/输出）、累计费用。
    logLine(QStringLiteral("summary: runs=%1  tokens=%2 (in=%3 out=%4)  cost=%5")
                .arg(u.totalCount).arg(u.tokensTotal).arg(u.tokensIn).arg(u.tokensOut)
                .arg(u.totalCost, 0, 'f', 6));
    // 聚合错误：即使整体成功也可能有单个接口失败，这里把明细一并打出来便于定位。
    if (!result.errors.isEmpty())
        logLine(QStringLiteral("errors:  %1").arg(result.errors.join(QStringLiteral(" | "))));

    // 判定标准：三块核心数据（额度、订阅、汇总）全部有效才算整体成功。
    const bool ok = result.credits.valid && result.subscription.valid && result.summary.valid;
    logLine(ok ? QStringLiteral("SELFTEST OK") : QStringLiteral("SELFTEST PARTIAL"));
    // 结论与退出码保持一致，脚本可直接以退出码判定，无需解析文本。
    return ok ? 0 : 1;
}

} // namespace

/**
 * @brief 程序入口：解析命令行，并分流到「GUI 主窗口」或「无界面自检」两条路径。
 *
 * 调用时机：进程启动时由 C 运行时调用，整个程序只执行一次。
 * 处理步骤：
 *   1) 构造 QApplication（GUI 必需；即便自检模式也复用它，以便网络模块正常工作）；
 *   2) 设置组织名/应用名/版本号——这决定了 QSettings 配置文件的落盘位置，
 *      因此必须在任何读取配置之前完成；
 *   3) 注册并解析命令行选项：--selftest、--key、--base-url、--out，
 *      外加 Qt 自带的 --help 与 --version；
 *   4) 若给了 --selftest：准备输出文件、附挂控制台，交由 runSelfTest 执行并用其返回值退出；
 *   5) 否则进入 GUI 分支：应用临时的 Key/地址覆盖，显示主窗口，
 *      并用 QTimer::singleShot(0, ...) 把首次网络刷新推迟到窗口完成首帧绘制之后。
 *
 * 命令行选项速查：
 *   --selftest          无界面自检：调用接口并打印结果后退出；
 *   --key <key>         本次运行临时使用的 API Key（不写入配置）；
 *   --base-url <url>    本次运行临时使用的 API 地址（不写入配置）；
 *   --out <file>        把自检输出同时写入指定文件（覆盖同名旧文件）；
 *   --theme <mode>      本次运行临时使用的外观主题（light/dark/system，不写入配置）；
 *   --help / --version  由 Qt 自动提供，打印用法或版本后直接退出。
 *
 * @param[in] argc int，命令行参数个数（含程序自身路径），由 C 运行时传入。
 * @param[in] argv char*[]，命令行参数数组；argv[0] 为可执行文件路径，其余为参数。
 * @return int，进程退出码：
 *         0  = GUI 正常退出，或自检四个接口全部成功；
 *         1  = 自检部分失败或 45 秒超时；
 *         2  = 自检时没有可用的 API Key。
 * @note GUI 分支返回的是事件循环的退出码（app.exec()）；只有自检分支才会
 *       返回上面定义的 1 与 2。--key/--base-url 始终是「一次性覆盖」语义。
 */
int main(int argc, char *argv[])
{
    // 先建 QApplication：后续的 QSettings、网络与界面对象都依赖它存在。
    QApplication app(argc, argv);
    // 组织名/应用名决定配置文件（QSettings）的存放目录，必须在读配置之前设定，
    // 否则 AppConfig 会读到另一处路径，导致「明明填了 Key 却提示没有」。
    QCoreApplication::setOrganizationName(QStringLiteral("CommandCodeUsageMonitor"));
    QCoreApplication::setApplicationName(QStringLiteral("CommandCodeUsageMonitor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // 应用外观主题（浅色 / 深色 / 跟随系统）。
    // 位置有两处讲究：① 必须在组织名 / 应用名之后——主题模式存在配置文件里，
    // 而配置文件路径由这两个名字共同决定；② 必须在创建任何窗口之前——否则界面
    // 会先按默认配色绘制一帧再被重绘成目标主题，用户能看到一次闪烁。
    AppTheme::initialize(&app);

    // 命令行解析器：负责识别选项、给出 --help/--version 文本并在出错时终止进程。
    QCommandLineParser parser;
    // 描述信息会出现在 --help 输出中，顺带标明构建所用的 Qt 版本。
    parser.setApplicationDescription(
        QStringLiteral("Command Code 套餐用量查看器（Qt 6.11.2 构建）"));
    // 注册 Qt 内置的 --help 与 --version，二者命中时会自行打印并退出进程。
    parser.addHelpOption();
    parser.addVersionOption();

    // --selftest：无界面自检开关，不带参数；命中后不创建 MainWindow。
    QCommandLineOption selfTestOption(QStringLiteral("selftest"),
                                      QStringLiteral("无界面自检：调用接口并打印结果后退出"));
    // --key：临时 API Key，带值；仅本次运行生效，不写入配置文件。
    QCommandLineOption keyOption(QStringLiteral("key"),
                                 QStringLiteral("临时指定 API Key（仅本次运行，不写入配置）"),
                                 QStringLiteral("key"));
    // --base-url：临时 API 地址，带值；用途是在自检时指向测试/备用端点。
    QCommandLineOption baseUrlOption(QStringLiteral("base-url"),
                                     QStringLiteral("临时指定 API 地址"),
                                     QStringLiteral("url"));
    // --out：把自检结果同时写入文件，带值；GUI 子系统下 stdout 不可见时的可靠通道。
    QCommandLineOption outOption(QStringLiteral("out"),
                                 QStringLiteral("把自检结果同时写入指定文件"),
                                 QStringLiteral("file"));
    // --probe-ui：界面化的桌面集成自检，带值（等待秒数，默认 8 秒）。
    // 存在的意义是把"托盘/任务栏到底有没有生效"变成可落盘的客观事实，
    // 而不是只能靠肉眼看任务栏——Windows 11 默认会把新图标收进溢出区。
    QCommandLineOption probeOption(QStringLiteral("probe-ui"),
                                   QStringLiteral("启动界面并在等待若干秒后输出桌面集成状态后退出"),
                                   QStringLiteral("seconds"),
                                   QStringLiteral("8"));
    // --quit：向已在运行的实例发送退出请求（开关型，不带值）。
    // 存在的意义是"进程藏在后台、又找不到托盘图标"时的命令行逃生通道——
    // 用户不必去任务管理器里结束进程。
    QCommandLineOption quitOption(QStringLiteral("quit"),
                                  QStringLiteral("请求已在运行的实例退出（无实例时直接返回）"));
    // --theme：本次运行临时使用的外观主题，与 --key / --base-url 同一语义
    // （只影响本次运行、不写入配置）。存在的意义有两个：
    //   ① 让脚本能在指定主题下启动程序做截图对照，验证深浅两套配色；
    //   ② 用户只想"看一眼深色效果"时不必改动自己的持久化设置。
    QCommandLineOption themeOption(QStringLiteral("theme"),
                                   QStringLiteral("临时指定外观主题（light/dark/system，仅本次运行；"
                                                  "未识别的取值按 system 处理）"),
                                   QStringLiteral("mode"));
    // 逐个注册：addOption 之后 parser 才认识它们，缺失选项一律不算错误。
    parser.addOption(selfTestOption);
    parser.addOption(keyOption);
    parser.addOption(baseUrlOption);
    parser.addOption(outOption);
    parser.addOption(probeOption);
    parser.addOption(quitOption);
    parser.addOption(themeOption);
    // 执行解析：遇到未知选项会打印错误并退出进程，因此后面的代码无需再做校验。
    parser.process(app);

    // 外观主题的命令行覆盖：必须在创建任何窗口之前应用，否则会先按配置里的主题
    // 绘制一帧再切换，出现可见闪烁。取值非法时回退为 system 并给出告警，
    // 而不是直接终止进程——外观参数写错不该让程序起不来。
    if (parser.isSet(themeOption)) {
        const QString requested = parser.value(themeOption).trimmed().toLower();
        if (requested != QLatin1String("light") && requested != QLatin1String("dark")
            && requested != QLatin1String("system")) {
            qWarning("未知的 --theme 取值 \"%s\"，已回退为 system。", qPrintable(requested));
        }
        AppTheme::overrideMode(AppConfig::themeModeFromKey(requested, AppConfig::ThemeMode::system));
    }

    // 自检分支：全程无界面，结束后直接以自检结果的退出码结束进程。
    if (parser.isSet(selfTestOption)) {
        // 记录输出文件路径；空字符串代表「只输出到 stdout」。
        g_outFilePath = parser.value(outOption);
        // 先删除旧文件：避免上一次自检的内容被追加进来，导致本次结果难以辨认；
        // 删除失败（不存在或占用）不影响后续的追加写入。
        if (!g_outFilePath.isEmpty())
            QFile::remove(g_outFilePath);
        // Windows 上先附挂父控制台，让 GUI 子系统进程的自检输出也能在命令行里看到。
#ifdef Q_OS_WIN
        tryAttachParentConsole();
#endif
        // 用 --key / --base-url 的值（可能为空）驱动自检，并把其退出码作为进程退出码。
        return runSelfTest(parser.value(keyOption), parser.value(baseUrlOption));
    }

    // GUI 分支：创建主窗口（此时尚未发起网络请求，界面可先呈现）。

    // ---------------- 单实例守护（关键逃生通道）----------------
    // 背景：用户勾选"关闭窗口时最小化到托盘"后，点 X 只会隐藏窗口；而 Windows 11
    // 默认把新托盘图标收进溢出区，用户很可能根本找不到图标，于是陷入"窗口没了、
    // 进程还在、又无从下手"的死局。单实例机制的解法是：再运行一次本程序即可唤回窗口。
    //   · 已在运行的实例 → 本实例把 SHOW（或 QUIT）命令发过去后立即退出；
    //   · 没有实例在运行 → 本实例正常启动，并监听后续命令。
    // 服务器名带上用户名：多用户会话（快速切换用户）下互不串扰。
    //
    // 为什么用本地套接字而不是"查找同名进程再发窗口消息"：
    //   · 进程枚举 + 窗口消息依赖窗口标题或类名匹配，改标题就会失效；
    //   · 套接字由内核保证互斥，能顺带解决"如何判定已有实例"这件事本身；
    //   · 两台机器/两个用户各用各的套接字名，天然隔离。
    const QString serverName = QStringLiteral("CommandCodeUsageMonitor-%1")
                                   .arg(qEnvironmentVariable("USERNAME", QStringLiteral("default")));
    // --probe-ui 是刻意要开独立窗口的自检路径，必须跳过单实例检查，
    // 否则有实例在跑时自检会直接退出、拿不到任何报告。
    const bool probeMode = parser.isSet(probeOption);
    if (!probeMode) {
        QLocalSocket probe;
        probe.connectToServer(serverName);
        // 500 毫秒是"够用且不拖慢启动"的折中：本机套接字连接要么瞬间成功，
        // 要么说明没有实例在监听；超时时间再长只会让正常启动变慢。
        if (probe.waitForConnected(500)) {
            // 已有实例：把意图（唤回窗口 / 请求退出）交给它，本进程不再创建界面。
            // 命令末尾带换行，便于对端将来扩展为多行协议时仍能按行解析。
            const QByteArray command = parser.isSet(quitOption) ? QByteArrayLiteral("QUIT\n")
                                                                : QByteArrayLiteral("SHOW\n");
            probe.write(command);
            // flush + 等待写出完成：若直接 disconnect，命令可能还留在发送缓冲区里，
            // 于是"再运行一次却没唤回窗口"。
            probe.flush();
            probe.waitForBytesWritten(500);
            probe.disconnectFromServer();
            // 本实例的使命已达成，直接以成功退出，不进入事件循环、不创建窗口。
            return 0;
        }
        // --quit 但没有实例在运行：视为"已经达成退出意图"，直接成功返回。
        // 这样脚本里可以无脑先 --quit 再启动，不必判断是否真的有实例。
        if (parser.isSet(quitOption))
            return 0;
    }

    MainWindow window;
    // 命令行覆盖：仅本次运行生效，不写配置文件
    // 只有在用户显式给出至少一个覆盖选项时才调用，避免无谓地覆盖已保存的凭据。
    if (parser.isSet(keyOption) || parser.isSet(baseUrlOption)) {
        // 未给出的那一项传空 QUrl / 空字符串，交由 setCredentialsOverride 自行取舍。
        window.setCredentialsOverride(parser.value(keyOption),
                                      parser.isSet(baseUrlOption) ? QUrl(parser.value(baseUrlOption))
                                                                  : QUrl());
    }
    // 显示主窗口（非模态，随后立即进入事件循环）。
    window.show();
    // 让窗口先绘制出来，再发起网络请求
    // 用 0 毫秒延时把刷新排到事件队列的下一轮：这样界面能先完成首帧绘制，
    // 网络请求的等待过程不会表现为「窗口迟迟不出现」的假死现象。
    QTimer::singleShot(0, &window, &MainWindow::startInitialRefresh);

    // 启动本地命令服务：接收"第二个实例"发来的 SHOW / QUIT。
    // 监听放在窗口创建之后，保证收到命令时窗口对象一定已经存在。
    QLocalServer commandServer;
    if (!probeMode) {
        // 上次异常退出（如被任务管理器结束）可能残留套接字，先清掉再监听，
        // 否则 listen() 会因"地址已占用"失败，逃生通道就失效了。
        // 这一步是"被强杀之后仍可恢复正常"的关键：残留套接字在实际使用中很常见。
        QLocalServer::removeServer(serverName);
        if (commandServer.listen(serverName)) {
            // 每来一个新实例就产生一次 newConnection；命令很短（SHOW / QUIT），
            // 单次读写即可处理完，无需维护连接状态机。
            QObject::connect(&commandServer, &QLocalServer::newConnection, &window, [&commandServer, &window]() {
                QLocalSocket *connection = commandServer.nextPendingConnection();
                if (!connection)
                    return;
                QObject::connect(connection, &QLocalSocket::readyRead, &window, [connection, &window]() {
                    const QByteArray command = connection->readAll().trimmed();
                    // 只认两种命令，其余一律按"唤回窗口"处理：宁可多显示一次窗口，
                    // 也不要让用户面对一个无法召回的进程。
                    if (command == QByteArrayLiteral("QUIT"))
                        window.requestQuit();
                    else
                        window.restoreFromTray();
                    connection->disconnectFromServer();
                });
                // 连接对象没有父对象，用完必须自行销毁，否则每来一个实例就泄漏一个 socket。
                // 挂在 disconnected 上而不是 readyRead 里 deleteLater：保证"对端异常断开"
                // 这条路径同样能释放资源。
                QObject::connect(connection, &QLocalSocket::disconnected,
                                 connection, &QLocalSocket::deleteLater);
            });
        }
        // listen 失败不阻止程序启动：单实例只是便利功能，不该成为启动前提。
        // 此时用户仍可通过任务管理器结束进程，功能本身不受影响。
    }

    // 界面化自检分支：等待指定秒数让首轮抓取完成，把桌面集成状态落盘后自动退出。
    // 之所以要"显示真实窗口"再取证，是因为托盘图标的可见性与任务栏绑定都依赖
    // 真实窗口句柄，无界面路径测不出这两个最关键的事实。
    if (parser.isSet(probeOption)) {
        g_outFilePath = parser.value(outOption);
        if (!g_outFilePath.isEmpty())
            QFile::remove(g_outFilePath);
#ifdef Q_OS_WIN
        tryAttachParentConsole();
#endif
        // 等待秒数带下限保护：小于 1 秒几乎不可能完成一次网络往返，取证会失真。
        const int waitSeconds = qMax(1, parser.value(probeOption).toInt());
        QTimer::singleShot(waitSeconds * 1000, &window, [&window]() {
            logLine(QStringLiteral("UI-PROBE: 桌面集成运行时状态"));
            const QStringList report = window.desktopProbeReport().split(QLatin1Char('\n'));
            for (const QString &line : report)
                logLine(QStringLiteral("UI-PROBE: %1").arg(line));
            logLine(QStringLiteral("UI-PROBE END"));
            QApplication::quit();
        });
    }

    // 进入主事件循环，直到用户关闭窗口；其返回值即进程退出码（正常为 0）。
    return app.exec();
}
