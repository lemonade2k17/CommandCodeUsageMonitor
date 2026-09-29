// ---------------------------------------------------------------------------
//  AppConfig.h —— 应用配置的读写接口（QSettings 持久化 + CLI 凭证导入）
//
//  API Key 默认**留空**，由用户在「设置」对话框里填写；
//  配置保存在 %APPDATA%/CommandCodeUsageMonitor/settings.ini（QSettings INI）。
//  另提供「从 Command Code CLI 导入」：显式点击后读取 ~/.commandcode/auth.json。
//
//  设计约定：
//    - 命名空间内全部是无状态自由函数，每次调用重新构造 QSettings，
//      不持有跨线程共享的可变状态，因此可被任意线程按需调用。
//    - 所有接口不抛异常：失败通过返回值或出参错误串表达，调用方必须检查。
//    - 仅依赖 QtCore（QString / QUrl），不引入任何界面类型。
// ---------------------------------------------------------------------------
#pragma once

#include <QString>
#include <QUrl>

// 配置访问入口：读写 settings.ini，并提供从 CLI 鉴权文件导入凭证的能力
namespace AppConfig
{
/**
 * @brief 返回配置文件的绝对路径，并确保其父目录存在。
 *
 * 路径取自 QStandardPaths::AppConfigLocation（Windows 下即
 * %APPDATA%/CommandCodeUsageMonitor）；若系统返回空串（少数受限环境），
 * 则退回用户主目录下的 .commandcode-usage。函数会顺带创建目录，
 * 因此凡是以本函数结果为目标的写入都不会因目录缺失而失败。
 *
 * @return QString，settings.ini 的绝对路径，其父目录已确保存在。
 * @note 本函数有文件系统副作用（创建目录），但不会创建文件本身。
 */
QString settingsFilePath();

/**
 * @brief 读取用户已保存的 API Key。
 *
 * 供网络请求层在每次发起调用前取用。读取结果会去除首尾空白，
 * 因为用户在设置对话框粘贴 Key 时经常带上换行或空格，若不去除会导致鉴权失败。
 *
 * @return QString，API Key；从未配置或被清空时返回空字符串（绝非 nullptr）。
 * @note 空返回值仅代表「尚未配置」，不代表读取出错，调用方需自行提示用户填写。
 */
QString apiKey();

/**
 * @brief 保存 API Key 到配置文件。
 *
 * 写入前先做 trimmed()，与 apiKey() 的读取口径保持一致，避免存进带空白的值
 * 导致后续字符串比对或请求头拼装出现隐蔽错误；写入后立即 sync() 刷盘，
 * 防止进程被强制结束而丢失配置。
 *
 * @param[in]     key QString，待保存的 API Key；传空字符串表示清空已有配置。
 * @return 无。
 * @note 本函数不做合法性校验，空值同样会被写入，是否允许由调用方决定。
 */
void    setApiKey(const QString &key);

/**
 * @brief 读取 API 基地址，缺省为 https://api.commandcode.ai。
 *
 * 返回值保证末尾不含 '/'：读取时会循环去掉多余的斜杠，因为用户在设置里
 * 填 "https://host/" 是常见写法，而拼接接口路径时多一个斜杠会得到双斜杠路径。
 * 配置缺失、为空或只剩空白时，回退到内置默认地址。
 *
 * @return QUrl，可用的 API 基地址（末尾无 '/'，保证非空）。
 * @note 若配置内容不是合法 URL，QUrl 会原样承载该字符串，合法性由使用方判定。
 */
QUrl    baseUrl();

/**
 * @brief 保存 API 基地址。
 *
 * 落盘前先去除首尾空白并循环去掉末尾 '/'，使存取两端归一化，
 * 避免出现 "https://host" 与 "https://host/" 被当成两个不同值的困扰。
 *
 * @param[in]     url QUrl，待保存的基地址；传空 URL 相当于写入空串，
 *                下次读取时会回退到内置默认值。
 * @return 无。
 * @note 仅做字符串归一化，不校验协议与主机名的有效性。
 */
void    setBaseUrl(const QUrl &url);

/**
 * @brief 读取自动刷新间隔（秒）。
 *
 * 界面定时器据此决定轮询周期；键名固定为 ui/refreshSeconds。
 *
 * @return int，刷新间隔秒数；未配置时返回 60，0 表示关闭自动刷新。
 * @note 读取侧不做负数钳制，写入侧已保证不小于 0，正常流程不会出现负值。
 */
int     refreshSeconds();

/**
 * @brief 保存自动刷新间隔（秒）。
 *
 * 写入前用 qMax(0, seconds) 钳制下界：负数对定时器无意义且会引发告警，
 * 统一按「0 = 关闭自动刷新」处理，使调用方无需自行过滤非法输入。
 *
 * @param[in]     seconds int，期望的刷新间隔秒数；0 表示关闭自动刷新，
 *                负数会被钳制为 0。
 * @return 无。
 * @note 本函数不重启任何定时器，界面需在保存后自行重建计时逻辑。
 */
void    setRefreshSeconds(int seconds);

/**
 * @brief 从 ~/.commandcode/auth.json 导入 apiKey（用户显式操作）。
 *
 * 典型调用时机是用户在「设置」里点击「从 Command Code CLI 导入」。
 * 处理步骤：定位鉴权文件 → 检查存在性与可读性 → 解析 JSON 对象属性 →
 * 取出 apiKey 字段并去除空白 → 通过出参回传 Key 与用户名。
 * 任一环节失败都会立即返回 false，并在 *error 写入面向用户的中文原因。
 *
 * @param[out]    apiKeyOut QString*，接收解析出的 API Key；可为 nullptr，
 *                此时不写出该值（调用方只关心成功与否时使用）。
 * @param[out]    userName QString*，接收 auth.json 中的 userName，
 *                可能为空字符串；可为 nullptr，此时不写出。
 * @param[out]    error QString*，失败时写入失败原因（文件缺失、无法读取、
 *                JSON 解析失败、缺少 apiKey 字段）；可为 nullptr。
 * @return bool，导入成功返回 true；文件不存在、不可读、解析失败或缺少
 *         apiKey 字段时返回 false。
 * @note 本函数只读取 CLI 文件，不会修改它；导入结果也不会自动落盘，
 *       是否保存由调用方（设置对话框）决定。
 */
bool    importFromCommandCodeCli(QString *apiKeyOut, QString *userName, QString *error);

/**
 * @brief 返回 Command Code CLI 的鉴权文件路径。
 *
 * 固定为当前用户主目录下的 .commandcode/auth.json，与 CLI 自身使用的
 * 位置保持一致；单独暴露此函数是为了让错误提示能显示具体路径。
 *
 * @return QString，auth.json 的绝对路径（仅拼接，不检查文件是否存在）。
 * @note 路径使用正斜杠拼接，展示给用户时应先经 QDir::toNativeSeparators 转换。
 */
QString cliAuthFilePath();

// ---------------------------------------------------------------------------
//  桌面集成（通知区域 / 任务栏）相关配置
// ---------------------------------------------------------------------------

/**
 * @brief 缩略信息要展示哪一项指标。
 *
 * 托盘图标内的数字与任务栏进度条都由该枚举决定；把它做成可配置项，
 * 是因为不同用户关心的限制不同（有人怕 5 小时窗口，有人只看月度额度）。
 */
enum class StatusMetric
{
    fiveHour = 0,   ///< 5 小时滑动窗口占用率
    weekly = 1,     ///< 每周滑动窗口占用率
    monthly = 2,    ///< 本计费周期额度占用率
    remaining = 3,  ///< 本周期剩余额度占总套餐额度的比例
};

/**
 * @brief 是否在通知区域（系统托盘）显示图标。默认为 true。
 * @return bool，true 表示用户开启了托盘图标。
 */
bool trayEnabled();

/**
 * @brief 保存「显示托盘图标」开关。
 * @param[in] enabled bool，true 开启；false 关闭。
 * @return 无。
 */
void setTrayEnabled(bool enabled);

/**
 * @brief 是否在任务栏按钮上显示进度条与角标。默认为 true。
 * @return bool，true 表示用户开启了任务栏缩略信息。
 */
bool taskbarBadgeEnabled();

/**
 * @brief 保存「任务栏缩略信息」开关。
 * @param[in] enabled bool，true 开启；false 关闭。
 * @return 无。
 */
void setTaskbarBadgeEnabled(bool enabled);

/**
 * @brief 关闭主窗口时是否最小化到托盘而不是退出。默认为 false。
 *
 * 默认关闭是有意为之：若默认最小化到托盘，用户点关闭后进程仍在后台运行，
 * 容易被误认为"程序退不掉"。
 *
 * @return bool，true 表示关闭窗口时隐藏到托盘。
 */
bool closeToTray();

/**
 * @brief 保存「关闭时最小化到托盘」开关。
 * @param[in] enabled bool，true 隐藏到托盘；false 直接退出。
 * @return 无。
 */
void setCloseToTray(bool enabled);

/**
 * @brief 读取缩略信息展示的指标。默认为 fiveHour。
 * @return StatusMetric，配置缺失或取值非法时回退为 fiveHour。
 */
StatusMetric statusMetric();

/**
 * @brief 保存缩略信息展示的指标。
 * @param[in] metric StatusMetric，目标指标。
 * @return 无。
 */
void setStatusMetric(StatusMetric metric);

/**
 * @brief 把指标枚举转成可持久化的英文键名。
 * @param[in] metric StatusMetric，待转换的指标。
 * @return QString，形如 "fiveHour"/"weekly"/"monthly"/"remaining"。
 */
QString statusMetricKey(StatusMetric metric);

/**
 * @brief 把持久化键名还原成指标枚举。
 * @param[in] key QString，键名；非法或为空时返回 fallback。
 * @param[in] fallback StatusMetric，无法识别时使用的默认值。
 * @return StatusMetric，还原结果。
 */
StatusMetric statusMetricFromKey(const QString &key, StatusMetric fallback);
} // namespace AppConfig
