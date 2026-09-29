// ---------------------------------------------------------------------------
//  CommandCodeApi.h —— Command Code 用量接口客户端
//
//  只依赖 QtNetwork，无第三方依赖。对应四个接口：
//    /alpha/whoami?limits=1           用户与组织
//    /alpha/billing/credits           额度余量 + 5h/周窗口限流
//    /alpha/billing/subscriptions     套餐与计费周期
//    /alpha/usage/summary?since=...   区间统计（次数/tokens/成本）
//
//  关键设计：
//    - 本类只负责取数与解析，不做任何界面呈现，结果一律通过信号回传。
//    - 四个接口分两阶段：whoami / credits / subscription 并行发出，
//      summary 需要等前三个返回后才能确定查询区间，因此延后发送。
//    - 单个接口失败不会中断整轮刷新，错误累积到 UsageSnapshot::errors。
//    - 所有回调都在本对象所属线程（通常是 GUI 线程）执行，可直接刷新界面。
// ---------------------------------------------------------------------------
#pragma once

#include "UsageTypes.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QUrl>

// 前置声明：该类型只以指针形式出现在成员函数签名里，此处无需完整定义
class QNetworkReply;

/**
 * @brief Command Code 用量 HTTP 接口的异步客户端。
 *
 * 封装账号信息（whoami）、额度（credits）、套餐（subscriptions）、
 * 用量统计（usage/summary）四个只读接口，统一负责：
 *   1. 拼装请求 URL 与鉴权头 Authorization: Bearer <apiKey>；
 *   2. 把 JSON 响应解析并填充到 UsageSnapshot；
 *   3. 通过 snapshotReady / connectionTested / failed 信号回传结果。
 *
 * @note 非线程安全：请始终在创建本对象的线程中调用其成员函数。
 * @note 网络请求由 QNetworkAccessManager 异步完成，任何成员函数都不会阻塞调用方。
 */
class CommandCodeApi : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造客户端，创建网络管理器并写入默认基地址。
     *
     * @param[in] parent QObject *，父对象指针，由父对象托管生命周期；可为 nullptr。
     * @return 无。
     * @note 构造完成后基地址为 https://api.commandcode.ai、API Key 为空，
     *       调用方需要随后通过 setApiKey() 注入密钥。
     */
    explicit CommandCodeApi(QObject *parent = nullptr);

    /**
     * @brief 设置接口基地址（协议 + 主机 [+ 端口]）。
     *
     * @param[in] url const QUrl &，新的基地址；只取其中 scheme/host/port，
     *                   具体路径由各接口在 start() 中自行拼接。
     * @return 无。
     * @note 用于自建代理或测试环境覆盖默认的 https://api.commandcode.ai。
     */
    void setBaseUrl(const QUrl &url);

    /**
     * @brief 读取当前生效的基地址。
     *
     * @return QUrl，当前基地址；未显式设置时即构造时写入的默认值。
     */
    QUrl baseUrl() const { return m_baseUrl; }

    /**
     * @brief 设置 API Key，首尾空白会被自动去除。
     *
     * @param[in] apiKey const QString &，Command Code 控制台签发的密钥；
     *                   传入空串等价于清除已保存的密钥。
     * @return 无。
     * @note 用户从剪贴板粘贴时常见带换行/空格，故此处统一 trimmed()。
     */
    void setApiKey(const QString &apiKey);

    /**
     * @brief 读取当前 API Key。
     *
     * @return QString，已保存的密钥原文（不含首尾空白）。
     */
    QString apiKey() const { return m_apiKey; }

    /**
     * @brief 判断是否已配置 API Key。
     *
     * @return bool，已配置返回 true；所有请求发出前都应先用它做前置校验。
     */
    bool hasApiKey() const { return !m_apiKey.isEmpty(); }

    // 完整刷新：whoami + credits + subscription，然后按周期起点拉 summary
    /**
     * @brief 发起一次完整刷新（两阶段请求）。
     *
     * 第一阶段并行请求 whoami / credits / subscription；三者全部返回（或失败）后
     * 进入第二阶段，按计费周期起点请求 usage/summary。最终结果通过 snapshotReady
     * 一次性回传；任何阶段的失败都只记录进 UsageSnapshot::errors，不中断整轮刷新。
     *
     * @return 无。
     * @note 未配置 API Key 时立即 emit failed() 并返回，不发任何网络请求。
     * @note 刷新途中重复调用会重置快照与待完成集合，上一轮在途结果将被丢弃。
     */
    void fetchAll();
    // 仅校验连通性与鉴权（用于"测试连接"）
    /**
     * @brief 仅校验连通性与鉴权，供界面上的「测试连接」使用。
     *
     * 只请求一次 whoami?limits=1，不填充 UsageSnapshot，也不触发 snapshotReady；
     * 结果改由 connectionTested(bool, QString) 回传。
     *
     * @return 无。
     * @note 未配置 API Key 时直接 emit connectionTested(false, ...) 并返回。
     */
    void testConnection();
    // 中途放弃当前刷新
    /**
     * @brief 中途放弃当前刷新，清空待完成请求集合。
     *
     * @return 无。
     * @note 已在途的 QNetworkReply 不会被强制中断，只是其回调因取消标志而
     *       不再推进状态机，也不会再发出 snapshotReady。
     */
    void cancel();

signals:
    /**
     * @brief 一轮完整刷新结束时发射，携带汇总结果。
     *
     * 发射时机：summary 也已返回（无论成功还是失败）之后，由 finishIfComplete() 发射。
     * 载荷含义：汇总四个接口的有效字段；某接口失败时其 valid 保持 false，
     *           具体错误文本见 snapshot.errors，fetchedAt 已填为当前本地时间。
     *
     * @param[out] snapshot const UsageSnapshot &，本轮刷新的汇总快照。
     */
    void snapshotReady(const UsageSnapshot &snapshot);

    /**
     * @brief 「测试连接」结束时发射。
     *
     * 发射时机：whoami-test 请求返回后；HTTP 状态码为 2xx 即视为成功。
     * 载荷含义：@p ok 为 true 表示鉴权通过；@p message 是可直接展示给用户的文案，
     *           成功时形如「鉴权成功：<userName>」，失败时为服务端错误或网络错误描述。
     *
     * @param[out] ok bool，连接与鉴权是否成功。
     * @param[out] message const QString &，面向用户的提示文本。
     */
    void connectionTested(bool ok, const QString &message);

    /**
     * @brief 刷新前的本地前置校验失败时发射。
     *
     * 发射时机：目前只有「尚未配置 API Key」一种情形，此时不会发起任何网络请求。
     *
     * @param[out] message const QString &，面向用户的失败原因文本。
     * @note 网络层与服务端错误不走本信号，而是记录进 UsageSnapshot::errors，
     *       以免单点失败导致整轮刷新无结果可用。
     */
    void failed(const QString &message);

private:
    /**
     * @brief 向 @p path 发起一次带鉴权头的 GET 请求，并把 @p tag 绑定到 reply 上。
     *
     * @param[in] tag const QString &，接口标识（whoami / credits / subscription /
     *                  summary / whoami-test），回包时凭此判断该填充哪部分数据。
     * @param[in] path const QString &，相对基地址的路径，可含 ?query。
     * @return 无。
     * @note tag 通过 QObject 动态属性 "cc_tag" 存放在 reply 上，
     *       使 handleReply() 无需额外的请求上下文表。
     */
    void start(const QString &tag, const QString &path);

    /**
     * @brief 统一的响应处理入口：判定成败、解析 JSON、推进状态机。
     *
     * @param[in] reply QNetworkReply *，已完成（finished 信号已发出）的响应对象；
     *                  本函数内部会调用其 deleteLater()，调用方不得再持有它。
     * @return 无。
     * @note 无论成功失败，末尾都会调用 maybeFetchSummary() 与 finishIfComplete()，
     *       以保证状态机在任何分支下都能继续推进。
     */
    void handleReply(QNetworkReply *reply);

    /**
     * @brief 在三个前置接口都回来之后，按需发起第二阶段的 summary 请求。
     *
     * @return 无。
     * @note 幂等：m_summaryStarted 为真、处于测试连接模式、或仍有请求在途时直接返回。
     * @note 有计费周期起点时以其为 since（并附带 orgId），否则退回服务端默认周期。
     */
    void maybeFetchSummary();

    /**
     * @brief 判断本轮刷新是否可以收尾；满足条件时补上时间戳并发射 snapshotReady。
     *
     * @return 无。
     * @note 收尾条件：非测试模式、未被取消、无在途请求且 summary 阶段已开始。
     *       这样即使个别接口失败，只要流程走完，界面也能拿到部分成功的数据。
     */
    void finishIfComplete();

    /**
     * @brief 把 HTTP 状态码与响应体翻译成一句可直接展示的错误文案。
     *
     * @param[in]  status int，HTTP 状态码；无有效状态码时调用方传 0。
     * @param[in]  body const QByteArray &，原始响应体，用于尝试提取服务端错误描述。
     * @param[in]  path const QString &，请求路径，仅在无法提取描述时用于兜底提示。
     * @return QString，错误文案；优先使用服务端 message/code，其次 401 专用提示，
     *         最后退化为「HTTP <状态码>：<路径>」。
     */
    static QString httpErrorMessage(int status, const QByteArray &body, const QString &path);

    // 网络管理器：以本对象为父对象，随本对象一起析构，无需手工释放
    QNetworkAccessManager *m_nam = nullptr;
    // 接口基地址，默认 https://api.commandcode.ai，可由 setBaseUrl() 覆盖
    QUrl    m_baseUrl;
    // 鉴权密钥，请求时拼成 "Bearer <key>"，已去除首尾空白
    QString m_apiKey;

    // 本轮刷新的结果累积区，随各接口回包逐步填充，最后整体发给界面
    UsageSnapshot m_snapshot;
    // 仍在途的接口 tag 集合；为空是进入第二阶段与收尾的必要条件
    QSet<QString> m_pending;
    // 第二阶段（summary）是否已经发出，防止重复触发
    bool m_summaryStarted = false;
    // summary 是否已拿到结果（含失败），仅用于状态记录
    bool m_summaryDone = false;
    // 是否处于「测试连接」模式：该模式下不填充快照、不发射 snapshotReady
    bool m_testMode = false;
    // 是否已被 cancel()：置真后回调不再推进状态机，也不发任何信号
    bool m_cancelled = false;
};
