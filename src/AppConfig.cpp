// ---------------------------------------------------------------------------
//  AppConfig.cpp —— 应用配置读写与 Command Code CLI 凭证导入的实现
//
//  配置项集中存放在单个 INI 文件中，键名统一为「组/键」形式：
//    api/key、api/baseUrl、ui/refreshSeconds。
//  每个函数自行构造 QSettings 并立即 sync()，不做进程内的长期缓存，
//  从而避免多窗口同时改配置时出现脏读。
//  默认基地址写在匿名命名空间的 defaultBaseUrl 常量里，只在读取时兜底，
//  不会主动写回配置文件，这样用户恢复默认值后文件里不会残留冗余项。
//  导入 CLI 凭证属于显式动作，本文件只读 ~/.commandcode/auth.json，
//  绝不改写它，也不代替用户落盘（是否保存由设置对话框决定）。
// ---------------------------------------------------------------------------
#include "AppConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
// QStringList：legacyConfigFilePaths() 的返回类型（旧配置候选路径列表）。
#include <QStringList>

// 匿名命名空间：仅本翻译单元可见的内部常量与工具函数
namespace {

// API 基地址的出厂默认值：只在配置缺失或为空时兜底，不写入配置文件
constexpr auto defaultBaseUrl = "https://api.commandcode.ai";

/**
 * @brief 构造指向本应用 INI 配置文件的 QSettings 对象。
 *
 * 每次调用都重新打开文件，保证读到磁盘上的最新值，而不是某个长期缓存；
 * 显式使用 IniFormat（而非平台原生格式），是为了让配置文件在 Windows 上
 * 也能用记事本直接查看和编辑，便于用户排障与手工恢复。
 *
 * @return QSettings，已绑定 settings.ini 的设置对象（按值返回，可安全移动）。
 * @note 不预先检查路径可写性；QSettings 写入失败时会静默忽略，调用方无法感知。
 */
QSettings settings()
{
    return QSettings(AppConfig::settingsFilePath(), QSettings::IniFormat);
}

} // namespace

namespace AppConfig {

/**
 * @brief 计算旧版本配置文件的候选路径（一次性迁移的数据来源）。
 *
 * 旧版本把配置放在 QStandardPaths::AppConfigLocation 下。该档位在 Qt 的
 * qstandardpaths_win.cpp 里随进程完整性级别变化：中完整性进程映射到 %LOCALAPPDATA%，
 * 低完整性进程映射到 %USERPROFILE%\AppData\LocalLow。迁移执行时进程通常是中完整性
 * （配置路径修复后 AppConfigLocation 只会解析出 %LOCALAPPDATA% 一条路），因此历史上
 * 真正落过盘的 LocalLow 路径必须按用户主目录直接拼出来，否则永远探测不到旧文件。
 *
 * @return QStringList，按优先级排列的旧配置文件候选路径；只拼路径，不检查存在性。
 * @note 候选 2 硬编码了组织名与应用名（均由 main() 设为 CommandCodeUsageMonitor），
 *       这两个值是本程序的固定常量；若将来改动其中任何一个，必须同步修改这里的拼接。
 */
QStringList legacyConfigFilePaths()
{
    QStringList paths;
    // 候选 1：当前进程视角的 AppConfigLocation（中完整性下即 %LOCALAPPDATA% 一支）。
    // 旧版本从未在正常双击场景往这里写过（那时进程也是低完整性），保留它是为了
    // 兼容"曾在中完整性环境运行过旧版本"的极端情况，探测成本只有一次 stat。
    const QString appConfigDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!appConfigDir.isEmpty())
        paths << appConfigDir + QStringLiteral("/settings.ini");
    // 候选 2：LocalLow 的固定位置。Qt 没有直接暴露 FOLDERID_LocalAppDataLow 的查询档位，
    // 而该目录在 Windows 上固定位于用户主目录之下，直接拼接即可；这正是旧版本在
    // 低完整性进程里实际写出过配置的地方，是本次迁移最主要的来源。
    paths << QDir::homePath()
                 + QStringLiteral("/AppData/LocalLow/CommandCodeUsageMonitor/CommandCodeUsageMonitor/settings.ini");
    return paths;
}

/**
 * @brief 计算配置文件路径，并确保其父目录存在；必要时执行一次旧配置迁移。
 *
 * 使用 QStandardPaths::AppDataLocation（Roaming）而不是旧版本的 AppConfigLocation：
 * 在 Qt 的 Windows 实现里，前者对中、低完整性进程都映射到 FOLDERID_RoamingAppData
 * （%APPDATA%），路径不随进程完整性级别漂移；后者在低完整性进程里会漂移到
 * LocalAppDataLow，造成"同一个用户在不同启动方式下各有一份配置"的分裂局面。
 *
 * @return QString，settings.ini 的绝对路径；父目录已通过 mkpath 创建。
 * @note 迁移是一次性的：仅当新文件不存在且某个旧候选文件存在时整体复制一次；
 *       旧文件复制后保留不删（不静默销毁用户数据），且此后因新文件已存在而
 *       不再进入迁移分支，重复调用是安全的。
 * @note 迁移失败（如目标目录只读）时静默放弃：QSettings 会把新配置当空白处理，
 *       用户重新填写一遍即可恢复，不值得为此阻断启动或弹窗打扰。
 */
QString settingsFilePath()
{
    // Roaming 档位：中、低完整性进程指向同一目录，配置位置从此不再漂移
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // 空路径会让配置落到进程当前目录，不可预期，故必须回退到主目录
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.commandcode-usage");
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/settings.ini");

    // 一次性迁移：新配置尚不存在时，把旧位置（含 API Key）整体搬过来。
    // 逐个候选探测，第一个存在的旧文件生效；QFile::copy 在目标已存在时必然失败，
    // 外层的 exists 判断已排除该情况，这里无需再处理复制冲突。
    if (!QFile::exists(path)) {
        const QStringList legacyPaths = legacyConfigFilePaths();
        for (const QString &legacyPath : legacyPaths) {
            if (QFile::exists(legacyPath)) {
                // 复制结果（成功/失败）都不阻断：失败时按"无旧配置"继续运行
                QFile::copy(legacyPath, path);
                break;   // 只迁移找到的第一份旧配置，无论成败都终止探测
            }
        }
    }
    return path;
}

/**
 * @brief 读取 api/key 配置项。
 *
 * 供网络层在每次请求前取用。返回值统一做 trimmed()，与写入侧口径一致，
 * 消除用户粘贴时附带换行/空格造成的鉴权失败。
 *
 * @return QString，已去空白的 API Key；未配置时为空字符串。
 * @note 返回空串属于正常的「未配置」状态，不等于读取异常。
 */
QString apiKey()
{
    QSettings s = settings();
    return s.value(QStringLiteral("api/key")).toString().trimmed();
}

/**
 * @brief 写入 api/key 配置项并立即刷盘。
 *
 * 写入前 trimmed()，防止带空白的值被存入后与界面上显示的内容不一致；
 * 写入后 sync() 立即落盘，避免程序被强制结束导致用户刚填的 Key 丢失。
 *
 * @param[in]     key QString，要保存的 API Key；空串表示清空该配置项。
 * @return 无。
 * @note 不校验 Key 的格式，是否合法由服务端鉴权结果决定。
 */
void setApiKey(const QString &key)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("api/key"), key.trimmed());
    s.sync();
}

/**
 * @brief 读取并规范化 API 基地址。
 *
 * 处理顺序：取配置值（缺省用 defaultBaseUrl）→ 去空白 → 若为空再兜底一次 →
 * 循环去掉末尾 '/' → 构造 QUrl。这样无论配置缺失、为空还是写成
 * "https://host/"，调用方拿到的地址都能安全地与接口路径直接拼接。
 *
 * @return QUrl，结尾不带 '/' 的 API 基地址，保证非空。
 * @note 该函数是纯读取操作，不会把兜底出来的默认值写回配置文件。
 */
QUrl baseUrl()
{
    QSettings s = settings();
    QString raw = s.value(QStringLiteral("api/baseUrl"), QString::fromLatin1(defaultBaseUrl)).toString().trimmed();
    // 配置项存在但内容为纯空白时，同样需要回退，否则会得到空 URL
    if (raw.isEmpty())
        raw = QString::fromLatin1(defaultBaseUrl);
    // 用户常把地址写成带尾部斜杠的形式，这里统一裁掉以保证拼接结果规范
    while (raw.endsWith(QLatin1Char('/')))
        raw.chop(1);
    return QUrl(raw);
}

/**
 * @brief 规范化并保存 API 基地址。
 *
 * 与 baseUrl() 采用同一套归一化规则（去空白 + 去末尾 '/'），
 * 保证「读回来的」与「写进去的」在语义上等价，避免同一地址产生两种写法。
 *
 * @param[in]     url QUrl，待保存的基地址；传入无效/空 URL 时相当于写入空串，
 *                下次读取将回退到 defaultBaseUrl。
 * @return 无。
 * @note 不校验协议与主机名，合法性由后续网络请求自行判定。
 */
void setBaseUrl(const QUrl &url)
{
    QString raw = url.toString().trimmed();
    // 与读取侧保持一致：去掉尾部斜杠，防止同一地址出现两种形态
    while (raw.endsWith(QLatin1Char('/')))
        raw.chop(1);
    QSettings s = settings();
    s.setValue(QStringLiteral("api/baseUrl"), raw);
    s.sync();
}

/**
 * @brief 读取自动刷新间隔（秒）。
 *
 * 界面定时器据此设定轮询周期；键名 ui/refreshSeconds，缺省 60 秒。
 *
 * @return int，刷新间隔秒数；未配置时为 60；0 表示关闭自动刷新。
 * @note 写入侧已做下界钳制，正常流程不会读到负数。
 */
int refreshSeconds()
{
    QSettings s = settings();
    return s.value(QStringLiteral("ui/refreshSeconds"), 60).toInt();
}

/**
 * @brief 保存自动刷新间隔（秒）。
 *
 * 用 qMax(0, seconds) 把负数钳制为 0：负数对定时器没有意义，
 * 统一按「0 = 关闭自动刷新」处理，可让调用方省去入参校验。
 *
 * @param[in]     seconds int，刷新间隔秒数；负数会被钳制为 0。
 * @return 无。
 * @note 本函数不重启定时器，界面需在保存后自行重建计时逻辑。
 */
void setRefreshSeconds(int seconds)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("ui/refreshSeconds"), qMax(0, seconds));
    s.sync();
}

/**
 * @brief 返回 Command Code CLI 的鉴权文件路径。
 *
 * 与 CLI 自身读取的位置保持一致（主目录下的 .commandcode/auth.json）。
 * 单独抽成函数是为了让错误提示可以复用同一路径来源，避免两处硬编码不一致。
 *
 * @return QString，auth.json 的绝对路径；仅拼接字符串，不检查文件是否存在。
 * @note 路径使用正斜杠，展示给用户前建议经 QDir::toNativeSeparators 转换。
 */
QString cliAuthFilePath()
{
    return QDir::homePath() + QStringLiteral("/.commandcode/auth.json");
}

/**
 * @brief 从 ~/.commandcode/auth.json 解析并回传 API Key 与用户名。
 *
 * 调用时机：用户在设置对话框点击「从 Command Code CLI 导入」。
 * 处理步骤：解析路径 → 判断文件存在 → 以只读方式打开 → 一次性读全部内容
 * 并立即关闭 → 用 QJsonDocument 解析（同时校验顶层必须是 JSON 对象）→
 * 取 apiKey 字段并去空白 → 通过出参回写 Key 与 userName。
 * 任一环节失败都立刻返回 false，并把中文原因写入 *error，
 * 直接对应界面上要求用户复制粘贴 Key 的场景。
 *
 * @param[out]    apiKeyOut QString*，接收解析出的 API Key；可为 nullptr，
 *                表示调用方只关心成功与否。
 * @param[out]    userName QString*，接收 auth.json 的 userName，可能为空串；
 *                可为 nullptr。
 * @param[out]    error QString*，失败时写入面向用户的原因（文件缺失、
 *                无法读取、JSON 解析失败、缺少 apiKey 字段）；可为 nullptr。
 * @return bool，导入成功返回 true；文件不存在、打开失败、解析失败或
 *         apiKey 为空时返回 false。
 * @note 三个出参指针都会先判空再写入，因此允许调用方按需只传部分；
 *       本函数不落盘、不修改 CLI 文件，是否保存由调用方决定。
 */
bool importFromCommandCodeCli(QString *apiKeyOut, QString *userName, QString *error)
{
    const QString path = cliAuthFilePath();
    QFile f(path);
    // 文件缺失是最常见的情况（用户未装 CLI），单独给出去向明确的提示
    if (!f.exists()) {
        if (error)
            *error = QStringLiteral("未找到 CLI 鉴权文件：%1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取 %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    const QByteArray raw = f.readAll();
    // 内容已全部读入内存，尽早释放文件句柄，避免导入过程中继续占用该文件
    f.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    // 顶层必须是对象：数组或标量都取不到 apiKey 字段，按解析失败统一处理
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("auth.json 解析失败：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject obj = doc.object();
    const QString key = obj.value(QStringLiteral("apiKey")).toString().trimmed();
    // 空 Key 视为导入失败：写入配置也只会得到一个必然鉴权失败的占位值
    if (key.isEmpty()) {
        if (error)
            *error = QStringLiteral("auth.json 中没有 apiKey 字段");
        return false;
    }
    // 走到这里才写出参，保证失败路径不会留下半成品数据
    if (apiKeyOut)
        *apiKeyOut = key;
    // userName 允许缺失：它只用于界面展示，不影响鉴权本身
    if (userName)
        *userName = obj.value(QStringLiteral("userName")).toString();
    return true;
}

// ---------------------------------------------------------------------------
//  桌面集成（通知区域 / 任务栏）相关配置
//
//  设计约定：
//    · 键名前缀统一为 ui/，与既有的 ui/refreshSeconds 保持同一分组；
//    · 每个开关都有明确默认值，读取时用 QSettings 的第二参数传入默认值，
//      因此配置文件缺失或损坏时行为仍然确定，不需要额外的存在性判断。
//    · 读取函数一律只读不写：配置里没有该键时也不会顺手把默认值写回去，
//      这样升级后新增的开关不会因为"被读了一次"就在用户文件里留下冗余项，
//      用户回退旧版本时也不会看到一串无法识别的键。
//    · 四个键的默认值遵循同一条原则——"默认状态下不会吓到用户"：
//        ui/trayEnabled         bool，默认 true —— 图标摆在通知区域，看得见才敢用；
//        ui/taskbarBadgeEnabled bool，默认 true —— 进度条不占界面空间，纯增益；
//        ui/closeToTray         bool，默认 false —— 它改变进程的存活语义，默认不启用；
//        ui/statusMetric        QString，默认 "fiveHour" —— 最容易被触发的一档限制。
//      展示类开关默认开、改变进程生命周期的开关默认关，用户不必先去设置里做决定。
// ---------------------------------------------------------------------------

/**
 * @brief 读取「是否显示通知区域（系统托盘）图标」开关。
 *
 * 默认 true：本程序的使用方式是"挂着看用量"，托盘图标是它最常见的存在形态；
 * 若默认关闭，用户关掉窗口后程序就彻底消失，反而体会不到后台盯着额度的价值。
 * 默认值由 QSettings 的第二个参数给出，因此键缺失、文件损坏或从未写过时
 * 都会稳定返回 true，不需要额外判断配置项是否存在。
 *
 * @return bool，true 表示允许显示托盘图标，false 表示用户主动关掉了它。
 * @note 这里只表达"用户意愿"，不代表图标此刻真的可见；系统不支持通知区域时
 *       由界面层另做可用性判断并降级，本函数不做也不该做这种判断。
 */
bool trayEnabled()
{
    QSettings s = settings();
    // 默认开启：本项目的主要价值就是"不占地方地看着用量"，托盘是它的默认形态
    return s.value(QStringLiteral("ui/trayEnabled"), true).toBool();
}

/**
 * @brief 保存「是否显示通知区域图标」开关。
 * @param[in] enabled bool，true 表示开启托盘图标，false 表示关闭。
 * @return 无。
 * @note 立即 sync() 落盘：该开关决定"关掉窗口后进程是否还在"，属于不能被异常退出吞掉的配置。
 */
void setTrayEnabled(bool enabled)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("ui/trayEnabled"), enabled);
    s.sync();
}

/**
 * @brief 读取「是否在任务栏按钮上显示进度条与角标」开关。
 *
 * 默认 true：任务栏缩略信息不占任何窗口空间，属于"顺带多给一点信息"的纯增益，
 * 且它完全不参与进程生命周期，即便用户从不看它也不会有副作用，故默认给出。
 * 与托盘不同的地方在于：任务栏进度条在无桌面会话时本来就绑定不上，
 * 这里的 true 只是"允许尝试"，真正的可用性由 TaskbarProgress 在运行期判定。
 *
 * @return bool，true 表示允许在任务栏按钮上叠加进度与角标。
 * @note 键缺失或文件损坏时同样由第二个参数兜底返回 true，无需存在性判断。
 */
bool taskbarBadgeEnabled()
{
    QSettings s = settings();
    // 展示类开关默认开：多给信息不吓人，关掉才是用户的主动选择
    return s.value(QStringLiteral("ui/taskbarBadgeEnabled"), true).toBool();
}

/**
 * @brief 保存「任务栏缩略信息」开关。
 * @param[in] enabled bool，true 表示开启任务栏进度条与角标，false 表示关闭。
 * @return 无。
 * @note 关闭后界面层会主动清空已推送的进度与角标，避免旧值继续挂在任务栏上。
 */
void setTaskbarBadgeEnabled(bool enabled)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("ui/taskbarBadgeEnabled"), enabled);
    s.sync();
}

/**
 * @brief 读取「关闭主窗口时是否最小化到托盘而不是退出」开关。
 *
 * 默认 false：这是本组配置里唯一会改变进程存活语义的开关。若默认开启，
 * 用户点关闭后窗口消失、进程却仍在后台，既没有可见窗口也没有打开托盘的习惯时，
 * 就会得到"程序退不掉"的错觉，甚至只能去任务管理器结束进程。
 * 因此把"点关闭即退出"设为默认预期，让常驻后台成为用户主动选择的结果。
 *
 * @return bool，true 表示关闭窗口时隐藏到托盘，false 表示关闭即退出。
 * @note 该值只是用户的意愿；托盘不可用时界面层会忽略它（否则会造出关不掉的幽灵进程）。
 */
bool closeToTray()
{
    QSettings s = settings();
    // 默认关闭：让"点关闭即退出"成为默认预期，避免进程在后台悄悄常驻
    return s.value(QStringLiteral("ui/closeToTray"), false).toBool();
}

/**
 * @brief 保存「关闭时最小化到托盘」开关。
 * @param[in] enabled bool，true 表示关闭窗口时隐藏到托盘，false 表示直接退出。
 * @return 无。
 * @note 该开关只决定"关窗"行为，不影响托盘图标本身是否显示。
 */
void setCloseToTray(bool enabled)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("ui/closeToTray"), enabled);
    s.sync();
}

/**
 * @brief 把缩略信息指标枚举转换成写入配置文件的英文键名。
 *
 * 持久化刻意使用英文键名，而不是枚举的整数值，理由有三条：
 *   ① 跨版本稳定——枚举值的先后顺序可能因为将来插入新指标而整体位移，
 *      数字键会让旧配置"静默地指向另一个指标"；键名不会发生这种漂移；
 *   ② 可读可改——用户打开 settings.ini 能看懂 "weekly"，看不懂 "1"，
 *      排障时可以直接判断配置是否写坏，也敢手工修改；
 *   ③ 与语言解耦——键名不参与 tr() 翻译，切换界面语言不会让旧配置失效。
 * 因此落盘的一律是语义稳定的英文名，枚举只作为进程内的表达方式存在。
 *
 * @param[in] metric StatusMetric，待转换的指标枚举值。
 * @return QString，形如 "fiveHour" / "weekly" / "monthly" / "remaining" 的英文键名。
 * @note 返回值就是落盘内容，改动它等同于一次配置格式变更，必须同步更新
 *       statusMetricFromKey() 的识别表，否则用户已有的旧配置会退化成默认档位。
 */
QString statusMetricKey(StatusMetric metric)
{
    switch (metric) {
    case StatusMetric::fiveHour:
        return QStringLiteral("fiveHour");
    case StatusMetric::weekly:
        return QStringLiteral("weekly");
    case StatusMetric::monthly:
        return QStringLiteral("monthly");
    case StatusMetric::remaining:
        return QStringLiteral("remaining");
    }
    // 枚举被扩展却忘记补分支时的兜底：返回最保守的 5 小时窗口
    // 之所以选 fiveHour 而不是别的档位：
    //   · 它是窗口最短、最容易被打满的一档，宁可让用户早一点看到紧张的数字，
    //     也不要用一个看起来"很宽松"的档位掩盖即将触发的限流；
    //   · 它同时是 statusMetric() 的出厂默认值，兜底与默认保持一致，
    //     用户不会在"配置缺失"与"新增了未识别档位"两种情况下看到不同行为；
    //   · 该分支只在编译期新增枚举成员而忘记同步本函数时才可能到达，
    //     属于纯防御性代码，但返回一个合法键名总好过返回空串污染配置文件。
    return QStringLiteral("fiveHour");
}

/**
 * @brief 把配置文件中读到的键名还原成缩略信息指标枚举。
 *
 * 逐项字符串比较而不是查表，是为了让"非法值"与"缺失值"走同一条回退路径：
 * 任何未识别的字符串（手工改坏配置文件、从更新版本回退导致键名未知、
 * 或键名大小写被改错）都会返回调用方给出的 fallback，
 * 而不会返回一个默认构造的枚举值让调用方拿到意料之外的档位。
 *
 * @param[in] key QString，从 ui/statusMetric 读到的键名；可能为空或未识别。
 * @param[in] fallback StatusMetric，无法识别时使用的兜底指标，由调用方指定。
 * @return StatusMetric，识别成功时返回对应枚举；否则原样返回 fallback。
 * @note 把 fallback 交给调用方传入而不是在这里写死，是为了让"当前默认档位"
 *       只在 statusMetric() 一处定义，将来调整默认值时不至于漏改这里。
 */
StatusMetric statusMetricFromKey(const QString &key, StatusMetric fallback)
{
    if (key == QLatin1String("fiveHour"))
        return StatusMetric::fiveHour;
    if (key == QLatin1String("weekly"))
        return StatusMetric::weekly;
    if (key == QLatin1String("monthly"))
        return StatusMetric::monthly;
    if (key == QLatin1String("remaining"))
        return StatusMetric::remaining;
    // 手工改坏配置文件、或从更新版本回退时都会走到这里
    // 返回 fallback 而不是抛错或断言：配置属于外部输入，永远可能被人工编辑，
    // 一个未识别的字符串不应该让整个程序起不来，只应该安静地退回默认档位。
    return fallback;
}

/**
 * @brief 读取缩略信息要展示的指标档位。默认为 fiveHour。
 *
 * 默认选 fiveHour：它是四档里窗口最短、最容易被跑满的一档，也是用户最先
 * 被迫关心的一档；先展示最紧张的限制，比先展示月额度更能及时提醒用户。
 * 实现上分两层兜底：先由 QSettings 的第二参数保证"键缺失"时拿到 fiveHour，
 * 再交给 statusMetricFromKey() 处理"键存在但内容非法"的情况，
 * 因此无论配置文件是空的、被改坏的还是来自未来的版本，返回值都合法。
 *
 * @return StatusMetric，当前生效的指标档位；配置缺失或非法时返回 fiveHour。
 * @note 每次调用都重新打开配置文件，因此用户在设置里改完指标后，
 *       下一次刷新即可生效，不需要重启程序。
 */
StatusMetric statusMetric()
{
    QSettings s = settings();
    const QString key = s.value(QStringLiteral("ui/statusMetric"),
                                QStringLiteral("fiveHour")).toString();
    return statusMetricFromKey(key, StatusMetric::fiveHour);
}

/**
 * @brief 保存缩略信息展示的指标档位。
 * @param[in] metric StatusMetric，目标档位；落盘时经 statusMetricKey() 转为英文键名。
 * @return 无。
 * @note 立即 sync() 落盘，使"改完设置 → 下一次刷新立即换档"这条链路可靠。
 */
void setStatusMetric(StatusMetric metric)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("ui/statusMetric"), statusMetricKey(metric));
    s.sync();
}

// ===========================================================================
//  外观主题
// ===========================================================================

/**
 * @brief 把主题模式转成可持久化的英文键名。
 *
 * 用英文键名而不是枚举整数值落盘，是为了让 settings.ini 手工可读、可改；
 * 同时也避免将来枚举成员顺序调整时把用户的选择映射到另一档。
 *
 * @param[in] mode ThemeMode，待转换的主题模式。
 * @return QString，形如 "light" / "dark" / "system"。
 * @note 枚举被扩展却漏补分支时返回 "system"：这是三档中最保守的一档，
 *       比静默写成 "light" 更不容易让用户看到意料之外的外观。
 */
QString themeModeKey(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::light:
        return QStringLiteral("light");
    case ThemeMode::dark:
        return QStringLiteral("dark");
    case ThemeMode::system:
        return QStringLiteral("system");
    }
    // 纯防御性分支：只有编译期新增枚举成员而忘记同步本函数时才可能到达。
    return QStringLiteral("system");
}

/**
 * @brief 把配置文件中读到的键名还原成主题模式。
 *
 * 逐项字符串比较，使"非法值"与"缺失值"走同一条回退路径：任何未识别的字符串
 * （手工改坏配置、从更新版本回退、大小写被改错）都返回调用方给出的 fallback。
 *
 * @param[in] key QString，从 ui/theme 读到的键名；可能为空或未识别。
 * @param[in] fallback ThemeMode，无法识别时使用的兜底模式，由调用方指定。
 * @return ThemeMode，识别成功时返回对应模式，否则原样返回 fallback。
 * @note 把 fallback 交给调用方传入，是为了让"当前默认模式"只在 themeMode() 一处
 *       定义，将来调整默认值时不必同步修改这里。
 */
ThemeMode themeModeFromKey(const QString &key, ThemeMode fallback)
{
    if (key == QLatin1String("light"))
        return ThemeMode::light;
    if (key == QLatin1String("dark"))
        return ThemeMode::dark;
    if (key == QLatin1String("system"))
        return ThemeMode::system;
    // 配置属于外部输入，永远可能被人工编辑；未识别的值只应安静地退回默认档，
    // 而不应让程序起不来或弹错。
    return fallback;
}

/**
 * @brief 读取界面主题模式。默认为 system（跟随系统）。
 *
 * 默认跟随系统是有意为之：绝大多数用户希望程序与系统设置保持一致，
 * 只有明确需要固定观感的用户才会去显式选择浅色或深色。
 * 实现上分两层兜底：QSettings 的第二参数处理"键缺失"，themeModeFromKey()
 * 处理"键存在但内容非法"，因此返回值在任何配置状态下都合法。
 *
 * @return ThemeMode，当前生效的主题模式；缺失或非法时返回 system。
 * @note 只读取"用户选了哪一档"，不涉及"系统当前是深是浅"，故本文件无需 GUI 依赖。
 */
ThemeMode themeMode()
{
    QSettings s = settings();
    const QString key = s.value(QStringLiteral("ui/theme"), QStringLiteral("system")).toString();
    return themeModeFromKey(key, ThemeMode::system);
}

/**
 * @brief 保存界面主题模式。
 * @param[in] mode ThemeMode，目标模式；落盘时经 themeModeKey() 转为英文键名。
 * @return 无。
 * @note 立即 sync() 落盘：主题是"改完就该记住"的偏好，不应因进程被强制结束而丢失。
 */
void setThemeMode(ThemeMode mode)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("ui/theme"), themeModeKey(mode));
    s.sync();
}

} // namespace AppConfig
