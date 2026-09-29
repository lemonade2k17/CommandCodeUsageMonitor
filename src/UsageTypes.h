// ---------------------------------------------------------------------------
//  UsageTypes.h —— 用量数据结构与套餐目录（纯头文件，无对应 .cpp）
//
//  所有结构体直接对应 https://api.commandcode.ai 的返回结构（2026-09 实测）：
//    GET /alpha/whoami?limits=1
//    GET /alpha/billing/credits
//    GET /alpha/billing/subscriptions
//    GET /alpha/usage/summary[?since=ISO8601]
//  每个结构体都带一个 valid 标志：解析失败时保持默认值且 valid 为 false，
//  调用方据此区分「字段没拿到」与「字段真的是 0」，避免界面显示误导性数据。
//  同一轮刷新可能只有部分接口成功，故用 UsageSnapshot 聚合四个结果与错误列表，
//  支持「部分失败也展示已成功的数据」。
//  结构体已注册元类型，可安全地放入 QVariant 或跨线程信号槽传递。
// ---------------------------------------------------------------------------
#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <cmath>

// 滑动窗口限流（5 小时 / 每周）
/**
 * @brief 单个滑动窗口的限流使用情况（对应 5 小时窗口与每周窗口）。
 *
 * 由 /alpha/billing/credits 返回的 windowLimits 子对象填充；
 * 两个窗口（fiveHour、weekly）共用本结构体，逻辑完全一致。
 */
struct WindowLimit
{
    // 该字段是否解析成功；为 false 时其余成员保持默认值，界面应显示占位符
    bool valid = false;         ///< 该字段是否解析成功
    // 窗口内已消耗的额度，单位与 cap 一致（由服务端口径决定）
    double used = 0.0;          ///< 已用额度
    // 窗口上限；服务端未下发时为 0，此时 percent() 约定返回 0
    double cap = 0.0;           ///< 窗口上限
    // 服务端直接给出的超限结论，比本地比较 used/cap 更权威（含浮点误差考量）
    bool exceeded = false;      ///< 是否已超限
    // 窗口重置时刻；服务端按 UTC 下发，此处已转换为本地时区供界面直接显示
    QDateTime resetAt;          ///< 重置时刻（本地时区）

    // 与官网一致：向下取整
    /**
     * @brief 计算窗口已用百分比（0~100 的整数）。
     *
     * 与官网展示口径一致：先做 used / cap 再做 floor 向下取整。
     * 之所以不用四舍五入，是为了避免 99.6% 显示成 100% 让人误以为已用尽。
     * cap 非正（服务端没给上限）时直接返回 0，防止除零并避免误报超限。
     *
     * @return int，向下取整后的已用百分比；cap <= 0 时返回 0。
     * @note 本函数只做展示计算，不改变任何状态；超过 100 时按实际值返回。
     */
    int percent() const
    {
        // cap <= 0 属于「上限未知」，直接返回 0，同时规避除零
        if (cap <= 0.0)
            return 0;
        return static_cast<int>(std::floor(used / cap * 100.0));
    }

    /**
     * @brief 计算窗口剩余可用额度。
     *
     * 用三元表达式把已超额的情况钳制为 0，保证返回值恒为非负，
     * 调用方无需再判断，界面也不会出现负数额度。
     *
     * @return double，cap - used（used 已达或超过 cap 时返回 0.0）。
     * @note 不修改任何成员，可在渲染循环中高频调用。
     */
    double remaining() const { return cap > used ? cap - used : 0.0; }
};

// GET /alpha/billing/credits
/**
 * @brief 额度与限流信息，对应 GET /alpha/billing/credits 的响应。
 *
 * 同时包含计费周期额度、加购/赠送额度以及两个滑动窗口的限流状态，
 * 是界面顶部「剩余额度」卡片的主要数据来源。
 */
struct CreditsInfo
{
    // 整体解析是否成功；为 false 时下列字段一律不可信，界面应提示加载失败
    bool valid = false;
    // 本计费周期剩余额度（注意：是"剩余"，不是"已用"）；界面不要再做减法
    double monthlyRemaining = 0.0;   ///< 本计费周期剩余额度（是"剩余"，不是"已用"）
    // 用户额外付费购买的额度，与周期额度分开累计
    double purchasedCredits = 0.0;   ///< 加购额度
    // 套餐赠送或活动发放的额度
    double freeCredits = 0.0;        ///< 赠送额度
    // 低于该值即触发预警，用于界面上色（如变红）
    double creditThreshold = 0.0;    ///< 预警阈值
    // 服务端给出的「已低于阈值」结论，避免客户端重复实现比较逻辑
    bool   belowThreshold = false;   ///< 是否已低于预警阈值
    // 对应响应里的 windowLimits.limited 字段
    bool   limited = false;          ///< windowLimits.limited
    // 5 小时滑动窗口的限流用量
    WindowLimit fiveHour;            ///< 5 小时滑动窗口
    // 每周滑动窗口的限流用量
    WindowLimit weekly;              ///< 每周滑动窗口
    // 是否具备沙箱（sandbox）功能的访问权限
    bool   sandboxAccess = false;    ///< 是否有沙箱访问权限
};

// GET /alpha/billing/subscriptions
/**
 * @brief 订阅信息，对应 GET /alpha/billing/subscriptions 的响应。
 *
 * 用于确定当前套餐（进而查 PlanCatalog 得到总额度与显示名）
 * 以及本计费周期的起止时间。
 */
struct SubscriptionInfo
{
    // 整体解析是否成功；为 false 时套餐名应显示为「未知」，不要沿用旧值
    bool valid = false;
    // 套餐标识，是 PlanCatalog 查表的键；未知值会导致总额度回退到 0
    QString planId;                  ///< 例：individual-goat
    // 订阅状态原文，交给界面映射为中文或颜色提示
    QString status;                  ///< active / trialing / past_due ...
    // 当前计费周期起始时刻
    QDateTime currentPeriodStart;    ///< 当前计费周期开始时刻
    // 当前计费周期结束时刻，通常与额度重置时刻一致
    QDateTime currentPeriodEnd;      ///< 当前计费周期结束时刻
    // 用户是否已选择在周期末取消：为 true 时界面应提示即将到期
    bool cancelAtPeriodEnd = false;  ///< 是否在周期末取消订阅
};

// GET /alpha/usage/summary
/**
 * @brief 使用量汇总，对应 GET /alpha/usage/summary 的响应。
 *
 * 覆盖任务次数、成功率、花费与 token 用量等统计维度，
 * 其中 monthlyCredits 表示本周期已用额度，与 CreditsInfo 的剩余额度口径相反，
 * 展示时务必区分，避免把「已用」当成「剩余」而给出错误结论。
 */
struct UsageSummary
{
    // 整体解析是否成功；为 false 时统计数字保持 0，界面应显示占位符
    bool valid = false;
    // 指定周期内的任务总数，是各类计数的分母
    int    totalCount = 0;           ///< 任务总数
    // 正常完成的任务数
    int    completedCount = 0;       ///< 成功完成的任务数
    // 失败的任务数；与成功数之和通常小于总数（存在进行中等中间态）
    int    failedCount = 0;          ///< 失败的任务数
    // 成功率，直接取服务端结果而不在本地重算，保持与官网口径一致
    double successRate = 0.0;        ///< 成功率（服务端口径）
    // 周期内总花费金额
    double totalCost = 0.0;          ///< 总花费
    // 单次平均花费
    double averageCost = 0.0;        ///< 平均花费
    // 输入 token 数（提示词侧）
    qint64 tokensIn = 0;             ///< 输入 token 数
    // 输出 token 数（模型回复侧）
    qint64 tokensOut = 0;            ///< 输出 token 数
    // 输入与输出之和，同样以服务端下发值为准
    qint64 tokensTotal = 0;          ///< token 总数
    // 周期内已消耗的额度
    double credits = 0.0;            ///< 已消耗额度
    // 本周期已用的套餐额度（与 CreditsInfo 的「剩余」口径相反，勿混用）
    double monthlyCredits = 0.0;     ///< 本周期已用套餐额度（是"已用"，不是"剩余"）
    // 已用的赠送额度
    double freeCredits = 0.0;        ///< 已用赠送额度
    // 已用的加购额度
    double purchasedCredits = 0.0;   ///< 已用加购额度
    // 统计周期的口径说明（如按月、按计费周期），仅用于展示
    QString periodBasis;             ///< 统计周期口径说明
};

// GET /alpha/whoami?limits=1
/**
 * @brief 当前登录账号信息，对应 GET /alpha/whoami?limits=1 的响应。
 *
 * 只用于界面展示与归属判断，不参与任何额度计算。
 */
struct WhoAmI
{
    // 整体解析是否成功；为 false 时界面应显示「未登录」或上次的缓存值
    bool valid = false;
    // 服务端用户唯一标识
    QString userId;                  ///< 用户 ID
    // 登录用户名（可能来自 CLI 的 auth.json）
    QString userName;                ///< 登录用户名
    // 便于阅读的展示名，界面优先使用；为空时回退到 userName
    QString displayName;             ///< 展示名
    // 账号邮箱
    QString email;                   ///< 邮箱
    // 所属组织 ID；个人账号为空，界面据此判断是否显示组织名
    QString orgId;                   ///< 组织 ID（个人账号为空）
};

// 一次完整刷新的结果
/**
 * @brief 单次刷新四个接口后的聚合结果。
 *
 * 每个子结构体各自带 valid 标志，允许「部分接口成功、部分失败」；
 * errors 收集失败接口的错误文案，界面可据此提示但仍展示已成功的数据。
 */
struct UsageSnapshot
{
    // 额度与限流信息（含两个滑动窗口）
    CreditsInfo      credits;        ///< 额度与限流信息
    // 订阅与套餐信息
    SubscriptionInfo subscription;   ///< 订阅信息
    // 使用量汇总统计
    UsageSummary     summary;        ///< 使用量汇总
    // 账号信息
    WhoAmI           whoami;         ///< 账号信息
    // 各接口的错误信息（部分失败也会回传成功的数据）
    QStringList      errors;         ///< 各接口的错误信息
    // 本次刷新的完成时刻，用于界面显示「最后更新时间」
    QDateTime        fetchedAt;      ///< 本次刷新完成时刻

    /**
     * @brief 判断本次刷新是否至少拿到了一份有效数据。
     *
     * 界面用它决定展示数据卡片还是「加载失败」空态：只要四个接口中有任意
     * 一个成功，就仍然值得渲染局部数据，而不是整屏报错。
     *
     * @return bool，四个子结构体中至少一个 valid 时返回 true，否则返回 false。
     * @note 只看 valid 标志，不关心 errors 是否为空（可能既有成功也有失败）。
     */
    bool hasAnything() const
    {
        return credits.valid || subscription.valid || summary.valid || whoami.valid;
    }
};

// 注册为 Qt 元类型，使上述结构体能放进 QVariant 并经信号槽跨线程传递
Q_DECLARE_METATYPE(WindowLimit)
Q_DECLARE_METATYPE(CreditsInfo)
Q_DECLARE_METATYPE(SubscriptionInfo)
Q_DECLARE_METATYPE(UsageSummary)
Q_DECLARE_METATYPE(WhoAmI)
Q_DECLARE_METATYPE(UsageSnapshot)

// ---------------------------------------------------------------------------
//  套餐目录：总额与显示名来自 Command Code CLI 内置映射表（v1.64.0）
// ---------------------------------------------------------------------------
/**
 * @brief 套餐标识到额度总额、显示名的静态映射表。
 *
 * 数据来源为 Command Code CLI 内置映射表（v1.64.0），此处以硬编码形式复刻，
 * 避免为一张小表引入额外的配置文件或网络请求。
 * 表内不含任何状态，全部函数为 inline，可被多个翻译单元安全包含。
 */
namespace PlanCatalog
{
/**
 * @brief 查询套餐对应的额度总额。
 *
 * 遍历固定映射表：命中即返回对应总额；未命中说明是新增或未知套餐，
 * 返回 0 并由调用方退回使用 summary.totalCost 兜底，
 * 而不是编造一个默认额度误导用户。
 *
 * @param[in]     planId QString，套餐标识（如 individual-goat），大小写敏感。
 * @return double，该套餐的额度总额；未知套餐返回 0.0。
 * @note 返回 0 不代表「免费套餐」，仅表示目录中查无此值，调用方须兜底。
 */
inline double totalCredits(const QString &planId)
{
    if (planId == QLatin1String("individual-go"))       return 10.0;
    if (planId == QLatin1String("individual-goat"))     return 70.0;
    if (planId == QLatin1String("individual-pro"))      return 30.0;
    if (planId == QLatin1String("individual-pro-v1"))   return 80.0;
    if (planId == QLatin1String("individual-provider")) return 15.0;
    if (planId == QLatin1String("individual-max"))      return 150.0;
    if (planId == QLatin1String("individual-ultra"))    return 300.0;
    if (planId == QLatin1String("teams-pro"))           return 40.0;
    return 0.0;   // 未知套餐：调用方应退回用 summary.totalCost 兜底
}

/**
 * @brief 查询套餐在界面上显示的名称。
 *
 * 与 totalCredits() 共用同一套标识，但展示名做了归并：
 * individual-pro 与 individual-pro-v1 都显示为 "Pro"，
 * 使同一档套餐的不同版本在界面上不会出现两种叫法。
 *
 * @param[in]     planId QString，套餐标识；未知值会被原样回显。
 * @return QString，套餐显示名；planId 为空时返回「未知套餐」，
 *         否则返回未识别的 planId 原文（便于用户反馈问题）。
 * @note 不返回空字符串，界面可直接绑定，无需再做空值判断。
 */
inline QString displayName(const QString &planId)
{
    if (planId == QLatin1String("individual-go"))       return QStringLiteral("Go");
    if (planId == QLatin1String("individual-goat"))     return QStringLiteral("GOAT");
    if (planId == QLatin1String("individual-pro"))      return QStringLiteral("Pro");
    if (planId == QLatin1String("individual-pro-v1"))   return QStringLiteral("Pro");
    if (planId == QLatin1String("individual-provider")) return QStringLiteral("Provider");
    if (planId == QLatin1String("individual-max"))      return QStringLiteral("Max");
    if (planId == QLatin1String("individual-ultra"))    return QStringLiteral("Ultra");
    if (planId == QLatin1String("teams-pro"))           return QStringLiteral("Teams Pro");
    return planId.isEmpty() ? QStringLiteral("未知套餐") : planId;
}
} // namespace PlanCatalog
