// ---------------------------------------------------------------------------
//  CommandCodeApi.cpp —— Command Code 用量接口客户端的实现（请求发送与 JSON 解析）
//
//  依赖：QtNetwork（QNetworkAccessManager 异步 GET）、QtJson（手工解析响应体）。
//
//  设计要点：
//    1. 请求分两阶段：whoami / credits / subscription 三个互不依赖的接口并行发出，
//       待它们全部回来后，才能从 subscription 得知计费周期起点，再发 summary。
//    2. 解析全部手工完成，以便精确控制容错粒度：某个字段缺失只影响对应结构体，
//       不会波及其它字段，更不会让整轮刷新失败。
//    3. 任何单个接口出错都只向 snapshot.errors 追加一条文案，
//       保证「部分成功」时界面仍能显示已经拿到的数据。
//    4. 所有网络回调都在本对象所属线程执行，可直接操作界面。
//
//  错误处理约定：
//    - 本地前置校验失败（例如未配置 API Key）走 failed() 信号，
//      属于调用方可以预期的用法问题，不会发起任何网络请求；
//    - 远端失败（网络错误、非 2xx 状态、响应体非法）一律写入 snapshot.errors，
//      保证四个接口中任意一个出错时，其余接口的数据依然能展示出来。
// ---------------------------------------------------------------------------
#include "CommandCodeApi.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimeZone>
#include <QUrlQuery>
#include <QVariant>

// 匿名命名空间：以下解析辅助函数仅在本翻译单元可见，避免污染全局符号表
namespace {

/**
 * @brief 把服务端返回的 ISO 8601 时间字符串解析为本地时区的 QDateTime。
 *
 * @param[in] text const QString &，形如 2026-09-01T00:00:00.000Z 的时间串；
 *                 允许为空串或非法串，此时返回无效 QDateTime。
 * @return QDateTime，本地时区时间；解析失败时返回默认构造对象（isValid() 为 false）。
 * @note 服务端统一返回 Z 结尾的 UTC 串；少数接口可能省略毫秒或时区，
 *       因此这里先按带毫秒格式解析，失败后再退回不带毫秒的格式。
 */
QDateTime parseIso(const QString &text)
{
    // 空串直接短路返回，避免走 QDateTime::fromString 的无效解析路径
    if (text.isEmpty())
        return {};
    // 首选带毫秒的格式：subscriptions 接口返回的周期时间戳精确到毫秒
    QDateTime dt = QDateTime::fromString(text, Qt::ISODateWithMs);
    // 兼容不带毫秒的变体：不同接口/不同服务端版本返回的时间精度并不一致
    if (!dt.isValid())
        dt = QDateTime::fromString(text, Qt::ISODate);
    // 两种格式都解析失败，说明该字段不是时间串，交由调用方按「无效」处理
    if (!dt.isValid())
        return {};
    // 带 Z / 偏移量的字符串 Qt 会自行识别；不带时区的按 UTC 处理
    if (!text.endsWith(QLatin1Char('Z')) && !text.contains(QLatin1Char('+')))
        dt.setTimeZone(QTimeZone::UTC);
    // 统一转成本地时区，界面层无需再做时区换算
    return dt.toLocalTime();
}

/**
 * @brief 把毫秒级 Unix 时间戳（epoch milliseconds）解析为本地时区的 QDateTime。
 *
 * @param[in] value const QJsonValue &，JSON 中的数值字段，例如 1767225600000；
 *                  非数值、0 或负数一律视为无效。
 * @return QDateTime，本地时区时间；输入非法时返回默认构造对象。
 * @note 窗口限流的 resetAt 字段是毫秒时间戳而非 ISO 字符串，
 *       这正是本函数与 parseIso() 并存的原因；解析后立即转本地时区，
 *       以免界面显示 UTC 时刻造成误解。
 */
QDateTime parseEpochMs(const QJsonValue &value)
{
    // 字段缺失时 toDouble() 会给出 0，故必须先判类型，避免把缺失当成 1970-01-01
    if (!value.isDouble())
        return {};
    const qint64 ms = static_cast<qint64>(value.toDouble());
    // 非正数没有任何现实意义（服务端用 0 / null 表示「无重置时间」）
    if (ms <= 0)
        return {};
    // 传入 QTimeZone::UTC 而不是默认时区：时间戳本身是绝对时刻，
    // 若让 Qt 按本地时区解释会凭空偏移若干小时
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toLocalTime();
}

/**
 * @brief 按键名从 JSON 对象中取值的语法糖。
 *
 * @param[in] obj const QJsonObject &，待检索的 JSON 对象。
 * @param[in] key const char *，键名，以 ASCII 字面量传入（内部按 Latin-1 处理）。
 * @return QJsonValue，命中的值；键不存在时返回 QJsonValue::Undefined。
 * @note 仅用于压缩重复的 obj.value(QLatin1String(...)) 写法，
 *       调用方仍需自行做类型转换与缺失判断。
 */
QJsonValue pick(const QJsonObject &obj, const char *key)
{
    // QLatin1String：键名恒为 ASCII，用 Latin-1 包装可省去每次构造 QString 的开销
    // 返回值可能是 Undefined（键不存在）或类型不符，调用方必须自行判类型后再取值
    return obj.value(QLatin1String(key));
}

// 匿名命名空间结束：上述辅助函数无法在其它翻译单元中被链接引用
} // namespace

/**
 * @brief 构造客户端：建立网络管理器并写入默认基地址。
 *
 * @param[in] parent QObject *，父对象；交由 Qt 对象树托管生命周期，可为 nullptr。
 * @return 无。
 * @note 网络管理器以 this 为父对象，随本对象析构自动释放，无需手工 delete。
 * @note 此处只设置基地址，不注入 API Key——密钥属于敏感配置，由调用方显式传入。
 */
CommandCodeApi::CommandCodeApi(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    // 生产环境基地址；如需走代理或自建网关，调用方可用 setBaseUrl() 覆盖
    m_baseUrl = QUrl(QStringLiteral("https://api.commandcode.ai"));
    // 此处刻意不设置 API Key：密钥属于敏感配置，必须由调用方从安全来源注入
    // （例如 QSettings 或环境变量），避免把密钥硬编码进二进制。
}

/**
 * @brief 覆盖接口基地址。
 *
 * @param[in] url const QUrl &，新的基地址；只使用其 scheme/host/port，路径另行拼接。
 * @return 无。
 * @note 立即生效，仅影响之后发出的请求；已在途的请求仍使用旧地址。
 */
void CommandCodeApi::setBaseUrl(const QUrl &url)
{
    m_baseUrl = url;
}

/**
 * @brief 设置 API Key，写入前先去除首尾空白。
 *
 * @param[in] apiKey const QString &，Command Code 控制台签发的密钥；空串表示清除。
 * @return 无。
 * @note 用户从网页复制密钥时常常带上换行或空格，若不去除会直接导致 401。
 */
void CommandCodeApi::setApiKey(const QString &apiKey)
{
    m_apiKey = apiKey.trimmed();
}

/**
 * @brief 取消当前刷新：置位取消标志并清空在途集合。
 *
 * @return 无。
 * @note 不会真正断开已发出的连接（QNetworkAccessManager 不提供该能力），
 *       但回调会在 handleReply() 入口处因取消标志而不再推进状态机，
 *       因此既不会发射 snapshotReady，也不会触发第二阶段的 summary 请求。
 */
void CommandCodeApi::cancel()
{
    m_cancelled = true;
    // 清空后 finishIfComplete() 的「有待完成请求」判据不再成立，流程就此冻结
    m_pending.clear();
}

/**
 * @brief 把 HTTP 状态码与响应体翻译成一句面向用户的错误文案。
 *
 * @param[in] status int，HTTP 状态码；无有效状态码（如连接失败）时调用方传 0。
 * @param[in] body const QByteArray &，原始响应体，用于尝试提取服务端错误描述。
 * @param[in] path const QString &，请求路径，仅在无法提取描述时用于兜底提示。
 * @return QString，错误文案；优先级为：服务端 error.message（附 code）＞ 服务端
 *         message ＞ 401 专用提示 ＞ 「HTTP <状态码>：<路径>」。
 * @note 服务端存在两种错误结构，故此处依次尝试；解析失败不应再抛错，
 *       因为本函数本身就在错误处理路径上。
 */
QString CommandCodeApi::httpErrorMessage(int status, const QByteArray &body, const QString &path)
{
    // 错误描述可能藏在响应体里，因此先尽力解析；即便解析失败也不影响后面的
    // 状态码兜底文案，故此处不做任何错误上报。
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    // 只有「合法 JSON 且为对象」时才值得尝试提取服务端文案，否则直接走兜底分支
    if (err.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject obj = doc.object();
        // 形态一：{"success":false,"error":{"code":..,"message":..}}
        const QJsonObject errorObj = obj.value(QStringLiteral("error")).toObject();
        if (!errorObj.isEmpty()) {
            // code 是机器可读的错误码（如 invalid_api_key），message 是给人看的描述；
            // 两者一起返回，便于用户把错误码反馈给服务端排查
            const QString code = errorObj.value(QStringLiteral("code")).toString();
            const QString message = errorObj.value(QStringLiteral("message")).toString();
            // 有 message 才值得返回；code 缺失时用 HTTP 状态码占位，保证格式一致
            if (!message.isEmpty())
                return QStringLiteral("%1 (%2)").arg(message, code.isEmpty() ? QString::number(status) : code);
        }
        // 形态二：{"success":false,"status":404,"message":..}
        const QString message = obj.value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
            return message;
    }
    // 401 单独给出可操作的提示，这是用户最常遇到的错误（密钥粘贴错误或已撤销）
    // 两种已知的错误结构都没命中，说明响应体既不是预期 JSON 也不是业务错误
    // （例如被网关替换成了 HTML 错误页），此时只能依据状态码给出兜底文案
    if (status == 401)
        return QStringLiteral("API Key 无效或已失效（401）");
    // 最后兜底：至少让用户知道是哪个接口、什么状态码
    return QStringLiteral("HTTP %1：%2").arg(status).arg(path);
}

/**
 * @brief 向指定路径发起一次带鉴权头的异步 GET 请求，并把接口标识绑定到 reply 上。
 *
 * @param[in] tag const QString &，接口标识（whoami / credits / subscription /
 *                  summary / whoami-test），回包时据此决定填充哪部分数据。
 * @param[in] path const QString &，相对基地址的路径，可含查询串（?key=value）。
 * @return 无。
 * @note 请求是异步的，本函数立即返回；响应统一由 handleReply() 处理。
 * @note tag 以动态属性 "cc_tag" 存放在 reply 上，回包时无需额外的上下文映射表，
 *       也避免了在 lamdba 中捕获额外状态。
 */
void CommandCodeApi::start(const QString &tag, const QString &path)
{
    QUrl url = m_baseUrl;
    // 手工切分路径与查询串：setPath() 不会解析 '?'，直接整体传入会把问号当成路径字符
    const int q = path.indexOf(QLatin1Char('?'));
    url.setPath(q >= 0 ? path.left(q) : path);
    // 查询串必须交给 setQuery()，由 QUrl 负责百分号编码，避免 since 中的冒号等被误处理
    if (q >= 0)
        url.setQuery(path.mid(q + 1));

    // 四类头信息一次配齐：鉴权、请求体类型、期望响应类型、客户端标识
    QNetworkRequest request(url);
    // 鉴权头是本类唯一的认证手段；密钥在 setApiKey() 时已去除首尾空白
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + m_apiKey.toUtf8());
    request.setRawHeader("Content-Type", QByteArrayLiteral("application/json"));
    request.setRawHeader("Accept", QByteArrayLiteral("application/json"));
    // 自定义 User-Agent 便于服务端侧排查是哪个客户端在调用
    request.setRawHeader("User-Agent", QByteArrayLiteral("commandcode-usage-monitor/1.0"));
    // 20 秒传输超时：既覆盖慢网络，又不至于让界面「刷新中」状态无限期挂住
    request.setTransferTimeout(20000);
    // 只跟随「不降级」的重定向（https → http 会被拒绝），避免密钥明文外泄
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    // 真正发出请求：QNetworkAccessManager 内部异步执行，本函数立即返回，
    // 结果稍后通过 finished 信号回到 handleReply()
    QNetworkReply *reply = m_nam->get(request);
    // 把接口标识随 reply 一起传递，等价于给这次请求打上路由标签
    reply->setProperty("cc_tag", tag);
    /**
     * @brief 响应到达时的回调（lambda），把控制权交给 handleReply()。
     *
     * @param 无；所需上下文（this 与 reply）由闭包捕获。
     * @return 无。
     * @note 只捕获 this 与 reply 两个指针：reply 由 handleReply() 内部负责
     *       deleteLater()；this 的生命周期由调用方保证，若 this 先被销毁，
     *       QObject 会自动断开该连接，回调不会执行。
     */
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleReply(reply); });
}

/**
 * @brief 发起一次完整刷新（两阶段请求），结果经 snapshotReady 一次性回传。
 *
 * @return 无。
 * @note 未配置 API Key 时立即 emit failed() 并返回，不会发出任何网络请求。
 * @note 重复调用会重置快照与在途集合，上一轮的散包结果将被忽略（tag 已不在集合中）。
 * @note 终态由 finishIfComplete() 判定：summary 阶段也已完成后才发射信号。
 */
void CommandCodeApi::fetchAll()
{
    // 前置校验：没有密钥就必然 401，直接在本地拦下，省去一次无意义的往返
    if (!hasApiKey()) {
        emit failed(QStringLiteral("尚未配置 API Key，请先在「设置」中填写。"));
        return;
    }
    // 复位取消标志：允许 cancel() 之后重新刷新
    m_cancelled = false;
    // 清空上一轮快照，避免旧数据与新数据混在一起造成「幽灵数据」
    m_snapshot = UsageSnapshot{};
    m_summaryStarted = false;
    m_summaryDone = false;
    m_pending.clear();

    // 第一阶段：登记三个互不依赖的接口；三者全部出列后才进入第二阶段
    m_pending << QStringLiteral("whoami")
              << QStringLiteral("credits")
              << QStringLiteral("subscription");

    // 三个请求一次性全部发出：它们彼此没有依赖关系，并行可把总耗时压到最慢的一个
    start(QStringLiteral("whoami"), QStringLiteral("/alpha/whoami?limits=1"));
    start(QStringLiteral("credits"), QStringLiteral("/alpha/billing/credits"));
    start(QStringLiteral("subscription"), QStringLiteral("/alpha/billing/subscriptions"));
}

/**
 * @brief 仅测试连通性与鉴权，不改动快照，也不触发 snapshotReady。
 *
 * @return 无。
 * @note 未配置 API Key 时直接 emit connectionTested(false, ...) 并返回，
 *       与 fetchAll() 的区别在于：此处的失败是可预期的用户操作结果，不必走 failed()。
 * @note 测试模式下的回包由 handleReply() 中的 whoami-test 分支独立处理，
 *       不会污染正式刷新的状态机。
 */
void CommandCodeApi::testConnection()
{
    if (!hasApiKey()) {
        emit connectionTested(false, QStringLiteral("尚未配置 API Key。"));
        return;
    }
    // 置位测试模式：finishIfComplete() 会据此跳过，避免「测试连接」误发 snapshotReady
    m_testMode = true;
    // 复用 whoami 接口做探测：它最轻量，且能顺带验证密钥是否仍然有效。
    // tag 用 whoami-test 而非 whoami，以便在 handleReply() 中走独立的测试分支。
    start(QStringLiteral("whoami-test"), QStringLiteral("/alpha/whoami?limits=1"));
}

/**
 * @brief 在三个前置接口全部返回后，按需发起第二阶段的 summary 请求。
 *
 * @return 无。
 * @note 三个提前返回条件：已经发过 summary、当前是测试连接模式、仍有请求在途。
 *       其中「仍有请求在途」正是两阶段流程的关键闸门。
 * @note 查询区间优先取订阅的计费周期起点；取不到则退回服务端默认周期，
 *       保证即使 subscriptions 接口失败，用量统计仍然能拿到数据。
 */
void CommandCodeApi::maybeFetchSummary()
{
    // 幂等保护：summary 只允许发送一次，重复调用直接返回
    if (m_summaryStarted || m_testMode)
        return;
    // 闸门：必须等 whoami/credits/subscription 全部出列，否则周期起点可能还没到
    if (!m_pending.isEmpty())
        return;

    // 先置位再发请求：若回包极快（本地缓存/代理命中）导致重入，
    // 这一标志能确保 summary 不会被重复发出
    m_summaryStarted = true;
    QString path = QStringLiteral("/alpha/usage/summary");

    // 有计费周期起点就按周期查，否则由服务端按默认周期返回
    if (m_snapshot.subscription.valid && m_snapshot.subscription.currentPeriodStart.isValid()) {
        // 必须转成 UTC 再格式化：服务端只接受 Z 结尾的绝对时刻，
        // 若直接输出本地时间会被当成 UTC 解析，导致区间整体偏移时区差
        const QString since = m_snapshot.subscription.currentPeriodStart
                                  .toUTC()
                                  .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("since"), since);
        // 组织账号需带 orgId 才能查到组织维度的用量，个人账号该字段为空
        if (!m_snapshot.whoami.orgId.isEmpty())
            query.addQueryItem(QStringLiteral("orgId"), m_snapshot.whoami.orgId);
        // FullyEncoded：since 里的冒号与加号必须编码，否则服务端解析会截断参数
        path += QStringLiteral("?") + query.toString(QUrl::FullyEncoded);
    } else if (!m_snapshot.whoami.orgId.isEmpty()) {
        // 没有周期信息时至少带上 orgId，让服务端按默认周期返回组织用量
        path += QStringLiteral("?orgId=") + m_snapshot.whoami.orgId;
    }

    // 登记后才 start，确保回包时 m_pending 非空、不会被提前判定为「已收尾」
    m_pending << QStringLiteral("summary");
    start(QStringLiteral("summary"), path);
}

/**
 * @brief 判断本轮刷新是否可以收尾；满足条件时补时间戳并发射 snapshotReady。
 *
 * @return 无。
 * @note 之所以把收尾条件设为「summary 阶段已开始」而非「m_summaryDone 为真」，
 *       是为了让 summary 即使失败也能正常收尾——失败同样是一次终结，
 *       错误已记入 snapshot.errors，界面照样能拿到前三个接口的数据。
 * @note 取消或测试模式下不发射信号，保证界面不会收到意料之外的刷新结果。
 */
void CommandCodeApi::finishIfComplete()
{
    // 测试连接与已取消的流程都不产生正式快照
    if (m_testMode || m_cancelled)
        return;
    // 仍有请求在途：继续等，避免发出「半成品」快照
    if (!m_pending.isEmpty())
        return;
    // summary 尚未开始说明还停在第一阶段，此时收尾会漏掉用量统计
    if (!m_summaryStarted)
        return;

    // 时间戳在收尾这一刻才盖，表示这是本轮数据的实际新鲜度
    m_snapshot.fetchedAt = QDateTime::currentDateTime();
    // 按值发出：接收方可能在自己的槽函数里长期持有这份快照，
    // 而 m_snapshot 会在下一次 fetchAll() 时被整体重置
    emit snapshotReady(m_snapshot);
}

/**
 * @brief 统一的响应处理入口：判定成败、解析 JSON、推进两阶段状态机。
 *
 * @param[in] reply QNetworkReply *，已完成（finished 已发出）的响应对象；
 *                  所有权在本函数内终结——内部会调用 deleteLater()，
 *                  调用方不得再持有或访问它。
 * @return 无。
 * @note 所有分支（成功、HTTP 失败、JSON 非法、测试连接）都会在返回前推进状态机，
 *       因此不会出现「某个接口既不成功也不失败」导致流程永久挂起的情况。
 * @note 本函数可能在一次刷新中被并发调用多次（各接口的 finished 信号先后到达），
 *       但由于 Qt 事件循环串行执行槽函数，不存在数据竞争。
 */
void CommandCodeApi::handleReply(QNetworkReply *reply)
{
    // 取出 start() 时绑定的接口标识，决定后续走哪条解析分支
    const QString tag = reply->property("cc_tag").toString();
    // 一次性读完响应体；readAll() 之后 reply 的缓冲区即被清空
    const QByteArray body = reply->readAll();
    // 连接层错误（如拒绝连接、超时）没有 HTTP 状态码，此时 attribute 无效，统一记为 0
    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = statusVar.isValid() ? statusVar.toInt() : 0;
    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorText = reply->errorString();
    // 先排定延迟释放：离开本函数回到事件循环后 reply 即被回收，避免内存泄漏
    reply->deleteLater();

    // 判定成功需要两个条件同时成立：传输层无错误 + HTTP 状态码为 2xx。
    // 只看 error() 并不充分——部分 4xx/5xx 并不会产生 NetworkError。
    const bool ok = (netError == QNetworkReply::NoError) && status >= 200 && status < 300;

    // ---- 测试连接：独立分支 ----
    if (tag == QLatin1String("whoami-test")) {
        // 无论成败都要退出测试模式，否则后续正式刷新会被 finishIfComplete() 静默跳过
        m_testMode = false;
        if (ok) {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
            // 默认文案：HTTP 通了但响应体解析不出用户名，仍算鉴权成功
            QString who = QStringLiteral("已连接");
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject user = doc.object().value(QStringLiteral("user")).toObject();
                const QString name = user.value(QStringLiteral("userName")).toString();
                // 带上用户名能让用户确认「连上的是自己的账号」而不是别人的
                if (!name.isEmpty())
                    who = QStringLiteral("鉴权成功：%1").arg(name);
            }
            emit connectionTested(true, who);
        } else {
            // 优先展示服务端的业务错误（如 401 的具体原因），
            // 传输层出错（netError != NoError）时才退回 Qt 的网络错误描述
            emit connectionTested(false,
                                  netError == QNetworkReply::NoError
                                      ? httpErrorMessage(status, body, QStringLiteral("/alpha/whoami"))
                                      : netErrorText);
        }
        // 测试分支到此结束：不触碰快照、不触发第二阶段
        return;
    }

    // ---- 正常刷新：把错误塞进 snapshot，尽量保留部分成功的数据 ----
    // 把内部 tag 翻译成中文标签，仅用于拼装用户可读的错误文案
    const QString label = tag == QLatin1String("whoami")       ? QStringLiteral("用户信息")
                          : tag == QLatin1String("credits")    ? QStringLiteral("额度")
                          : tag == QLatin1String("subscription") ? QStringLiteral("套餐")
                                                                 : QStringLiteral("用量统计");
    if (!ok) {
        // 关键设计：错误写进 snapshot.errors 而不是直接 emit failed()。
        // 这样四个接口中任意一个失败，其余三个的数据依然能正常展示。
        m_snapshot.errors << QStringLiteral("%1：%2").arg(
            label, netError == QNetworkReply::NoError
                       ? httpErrorMessage(status, body, QStringLiteral("/") + tag)
                       : netErrorText);
        // 失败也必须出列：m_pending 是收尾与发起 summary 的唯一闸门，
        // 漏掉这一步会让整轮刷新永远停在「进行中」
        m_pending.remove(tag);
        // summary 失败也算「第二阶段已完成」，据此避免流程卡死
        if (tag == QLatin1String("summary"))
            m_summaryDone = true;
        // 即使本接口失败也要尝试推进：可能正好是最后一个在途请求
        maybeFetchSummary();
        finishIfComplete();
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    // HTTP 200 但响应体不是合法 JSON 对象（如网关返回 HTML 错误页）也要如实上报
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        m_snapshot.errors << QStringLiteral("%1：返回体不是合法 JSON").arg(label);
        // 与 HTTP 失败分支同理：必须出列，否则后续阶段无法推进
        m_pending.remove(tag);
        if (tag == QLatin1String("summary"))
            m_summaryDone = true;
        maybeFetchSummary();
        finishIfComplete();
        return;
    }

    // 走到这里说明响应体是合法的 JSON 对象，可按 tag 分派到各自的解析分支。
    // 注意：四个分支只写自己那部分字段，彼此互不干扰。
    const QJsonObject root = doc.object();

    // 按 tag 分派：四个接口的响应结构差异较大，各自独立解析最直观也最易维护
    if (tag == QLatin1String("whoami")) {
        // 用户对象与组织对象是两个并列子对象；个人账号的 org 通常为空对象
        const QJsonObject user = root.value(QStringLiteral("user")).toObject();
        const QJsonObject org = root.value(QStringLiteral("org")).toObject();
        // 以 user 是否为空判定本次 whoami 是否有效（valid 供界面决定是否展示账号区）
        m_snapshot.whoami.valid = !user.isEmpty();
        // id：账号唯一标识，仅用于内部区分，不直接展示
        m_snapshot.whoami.userId = user.value(QStringLiteral("id")).toString();
        // userName：登录名，测试连接成功提示中展示的就是它
        m_snapshot.whoami.userName = user.value(QStringLiteral("userName")).toString();
        // name：显示用昵称，可能为空，界面需自行退回 userName
        m_snapshot.whoami.displayName = user.value(QStringLiteral("name")).toString();
        // email：账号邮箱，界面用于展示当前登录身份
        m_snapshot.whoami.email = user.value(QStringLiteral("email")).toString();
        // orgId：组织标识；个人账号为空，非空时会被带进 summary 查询参数
        m_snapshot.whoami.orgId = org.value(QStringLiteral("id")).toString();
    } else if (tag == QLatin1String("credits")) {
        // 额度接口的响应由两部分组成：credits（绝对余量）与 windowLimits（滑动窗口）
        const QJsonObject credits = root.value(QStringLiteral("credits")).toObject();
        const QJsonObject limits = root.value(QStringLiteral("windowLimits")).toObject();
        CreditsInfo &info = m_snapshot.credits;
        // 两部分任一非空即视为有效：某些账号只有窗口限流而没有额度信息
        info.valid = !credits.isEmpty() || !limits.isEmpty();
        // monthlyCredits：本计费周期的剩余额度（是「剩余」而非「已用」，勿反向解读）
        info.monthlyRemaining = credits.value(QStringLiteral("monthlyCredits")).toDouble();
        // purchasedCredits：额外加购的额度
        info.purchasedCredits = credits.value(QStringLiteral("purchasedCredits")).toDouble();
        // freeCredits：赠送额度
        info.freeCredits = credits.value(QStringLiteral("freeCredits")).toDouble();
        // creditThreshold：预警阈值，余量低于它时服务端会把 belowThreshold 置真
        info.creditThreshold = credits.value(QStringLiteral("creditThreshold")).toDouble();
        // belowThreshold：是否已低于预警阈值，界面据此显示警示色
        info.belowThreshold = credits.value(QStringLiteral("belowThreshold")).toBool();
        // limited：账号当前是否处于限流状态，与具体窗口是否超限是两个层次的信息
        info.limited = limits.value(QStringLiteral("limited")).toBool();
        // sandboxAccess：顶层字段（不在 credits 内），表示是否可用沙箱环境
        info.sandboxAccess = root.value(QStringLiteral("sandboxAccess")).toBool();

        // ---- 局部工具：把一个窗口对象（fiveHour / weekly）解析成 WindowLimit ----
        // 之所以放在 credits 分支内部：它只服务于该分支，放进匿名命名空间反而
        // 会引入一个只被单点使用的全局符号。
        /**
         * @brief 把 windowLimits 下的单个窗口对象解析为 WindowLimit。
         *
         * @param[in] obj const QJsonObject &，形如 {"used":..,"cap":..,"exceeded":..,"resetAt":..}
         *                  的窗口对象；空对象表示该窗口未被服务端返回。
         * @return WindowLimit，解析结果；obj 为空时返回 valid == false 的默认对象。
         * @note 只有 fiveHour 与 weekly 两种取值形态，故用同一个 lambda 复用解析逻辑，
         *       避免两段几乎相同的代码各自演化出差异。
         */
        const auto readWindow = [](const QJsonObject &obj) {
            WindowLimit w;
            // 服务端可能不下发某个窗口（例如未开通对应限制），此时保持 valid == false
            if (obj.isEmpty())
                return w;
            w.valid = true;
            // used：窗口内已消耗额度
            w.used = obj.value(QStringLiteral("used")).toDouble();
            // cap：窗口上限；used/cap 即官网展示的百分比
            w.cap = obj.value(QStringLiteral("cap")).toDouble();
            // exceeded：服务端判定的超限标志，不依赖客户端自行比较 used 与 cap
            w.exceeded = obj.value(QStringLiteral("exceeded")).toBool();
            // resetAt 是毫秒时间戳（不是 ISO 字符串），故走 parseEpochMs 专用解析
            w.resetAt = parseEpochMs(obj.value(QStringLiteral("resetAt")));
            return w;
        };
        // 5 小时滑动窗口：短周期限流
        info.fiveHour = readWindow(limits.value(QStringLiteral("fiveHour")).toObject());
        // 周滑动窗口：长周期限流
        info.weekly = readWindow(limits.value(QStringLiteral("weekly")).toObject());
    } else if (tag == QLatin1String("subscription")) {
        // 订阅信息被包在 data 子对象里，而不是像其它接口那样平铺在顶层
        const QJsonObject data = root.value(QStringLiteral("data")).toObject();
        SubscriptionInfo &info = m_snapshot.subscription;
        // data 为空通常表示账号已无有效订阅（如免费额度用尽/订阅到期），
        // 此时只把 valid 置假，交由界面提示，而不是当作请求错误处理
        info.valid = !data.isEmpty();
        // planId：套餐标识（如 individual-goat），界面可经 PlanCatalog 映射成显示名
        info.planId = data.value(QStringLiteral("planId")).toString();
        // status：订阅状态（active / trialing / past_due ...），用于提示续费异常
        info.status = data.value(QStringLiteral("status")).toString();
        // cancelAtPeriodEnd：是否已安排到期不续订，界面据此提示「将于期末失效」
        info.cancelAtPeriodEnd = data.value(QStringLiteral("cancelAtPeriodEnd")).toBool();
        // currentPeriodStart：本计费周期起点，是第二阶段 summary 的 since 取值来源
        info.currentPeriodStart = parseIso(data.value(QStringLiteral("currentPeriodStart")).toString());
        // currentPeriodEnd：本计费周期终点，界面用于展示周期范围与剩余天数
        info.currentPeriodEnd = parseIso(data.value(QStringLiteral("currentPeriodEnd")).toString());
    } else if (tag == QLatin1String("summary")) {
        // 用量统计字段最多；服务端保证请求成功即返回完整结构，故直接置 valid
        // 用量统计字段最多，且全部由服务端计算；客户端只做搬运，
        // 任何二次换算（如自行求成功率）都可能与服务端口径不一致
        UsageSummary &info = m_snapshot.summary;
        info.valid = true;
        // totalCount：区间内的任务总数
        info.totalCount = root.value(QStringLiteral("totalCount")).toInt();
        // completedCount：成功完成的任务数
        info.completedCount = root.value(QStringLiteral("completedCount")).toInt();
        // failedCount：失败的任务数
        info.failedCount = root.value(QStringLiteral("failedCount")).toInt();
        // successRate：服务端计算的成功率，客户端不再重复计算以免口径不一致
        info.successRate = root.value(QStringLiteral("successRate")).toDouble();
        // totalCost：区间内消耗的总成本
        info.totalCost = root.value(QStringLiteral("totalCost")).toDouble();
        // averageCost：单次任务的平均成本
        info.averageCost = root.value(QStringLiteral("averageCost")).toDouble();
        // tokens 系列在 JSON 中是浮点数，而模型侧一定是整数，故先 toDouble() 再窄化。
        // 直接 toInt() 在数值超过 int 范围（大 token 量）时会溢出，必须用 qint64。
        info.tokensIn = static_cast<qint64>(root.value(QStringLiteral("totalTokensIn")).toDouble());
        // tokensOut：输出 token 数，与输入分开累计，便于展示输入/输出占比
        info.tokensOut = static_cast<qint64>(root.value(QStringLiteral("totalTokensOut")).toDouble());
        // tokensTotal：服务端给出的合计值，保留它以免与自行相加的结果对不上
        info.tokensTotal = static_cast<qint64>(root.value(QStringLiteral("totalTokens")).toDouble());
        // credits：区间内消耗的总额度
        info.credits = root.value(QStringLiteral("totalCredits")).toDouble();
        // monthlyCredits：其中来自月订阅额度的部分
        info.monthlyCredits = root.value(QStringLiteral("totalMonthlyCredits")).toDouble();
        // freeCredits：其中来自赠送额度的部分
        info.freeCredits = root.value(QStringLiteral("totalFreeCredits")).toDouble();
        // purchasedCredits：其中来自加购额度的部分
        info.purchasedCredits = root.value(QStringLiteral("totalPurchasedCredits")).toDouble();
        // periodBasis：服务端实际采用的统计基准（便于核对 since 是否被采纳）
        info.periodBasis = root.value(QStringLiteral("periodBasis")).toString();
        // 标记第二阶段已产出结果；收尾判定本身以 m_pending 为准，此处仅作状态记录
        m_summaryDone = true;
    }

    // 本接口出列；若它是第一阶段的最后一个，下面这次调用就会触发 summary 请求。
    // 顺序上必须先 remove 再调用，否则 maybeFetchSummary() 仍会看到自己在途而拒绝发车。
    m_pending.remove(tag);
    maybeFetchSummary();
    // 若 summary 刚刚完成，这里会立即发射 snapshotReady
    finishIfComplete();
}
