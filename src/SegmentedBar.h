// ---------------------------------------------------------------------------
//  SegmentedBar.h —— 声明分段式进度条控件 SegmentedBar 的状态接口与绘制入口
//
//  本控件视觉上对齐 Command Code 官网 "USAGE LIMITS" 的方格条：整条由固定数量
//  的方格组成，按百分比点亮靠前的若干格，颜色随占用率在绿 / 琥珀 / 红之间切换。
// ---------------------------------------------------------------------------
#pragma once

#include <QWidget>

// 设计要点（补充说明）：
//   1. 控件只持有展示状态（百分比、格数、告警阈值），不访问网络与磁盘，因此可
//      在任意界面中被安全复用；状态变更统一经 setter 收敛后再触发重绘；
//   2. 百分比与格数的合法性全部在 setter 内部裁剪，调用方无需预先校验入参；
//   3. 绘制逻辑集中在 paintEvent()，样式来源唯一，便于与官网视觉保持对齐；
//   4. 继承 QWidget 并使用 Q_OBJECT，以启用元对象系统（信号槽、tr() 等）。

/**
 * @brief 分段式进度条控件：把 0 ~ 100 的百分比渲染为一串点亮的方格。
 *
 * 该控件是纯展示型（View）组件，不持有业务数据，也不发起任何 I/O；外部每次
 * 拿到新的占用率后调用 setPercent() 即可，控件内部会完成裁剪、去重与重绘。
 *
 * @note 控件不接受直接写入内部成员，所有写入口都是 public 的 setXxx() 系列，
 *       以保证「状态变化 → update() 重绘」这一约定不会被绕过。
 */
class SegmentedBar : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造分段式进度条，并设定其在布局中的伸缩策略。
     *
     * 构造函数只做最基础的初始化：把尺寸策略设为「水平可扩展、垂直固定」，使其
     * 在水平方向占满可用宽度、在垂直方向保持 sizeHint() 给出的高度，从而在多行
     * 表单中不会被纵向拉伸变形。
     *
     * @param[in] parent QWidget *，父对象指针；可为 nullptr，此时控件无父对象，
     *                   其生命周期需由调用方或上层布局负责管理（输入方向）。
     * @return 无。
     * @note 构造期间不读取任何配置，初始百分比为 0，即全部方格处于熄灭态。
     */
    explicit SegmentedBar(QWidget *parent = nullptr);

    /**
     * @brief 设置当前占用率百分比，并触发一次重绘。
     *
     * 入参会被裁剪到 0 ~ 100 的闭区间；若裁剪后的值与当前值相同，则直接返回，
     * 不做无谓的 update()，以避免高频刷新时产生多余的重绘请求。
     *
     * @param[in] percent int，占用率百分比，合法区间 0 ~ 100；越界值按边界裁剪。
     * @return 无。
     * @note 传入 0 表示「全部熄灭」，传入任意大于 0 的值至少点亮 1 格，
     *       该规则由 paintEvent() 中的填充数量计算保证。
     */
    void setPercent(int percent);          // 0 ~ 100

    /**
     * @brief 读取当前占用率百分比。
     *
     * 该函数为简单 getter，不触发任何重绘或状态变更，可安全用于绘制回调内部。
     *
     * @return int，当前保存的百分比数值，取值恒在 0 ~ 100 闭区间内。
     */
    int  percent() const { return m_percent; }

    /**
     * @brief 设置方格总数，并触发一次重绘。
     *
     * 格数过少会让进度条看起来像几个色块，故内部用 qMax(4, ...) 兜底，保证视觉
     * 上仍是一条「分段」而非「分块」的条；上层若需与官网一致的观感，传 24 即可。
     *
     * @param[in] segments int，期望的方格数量；小于 4 时按 4 处理。
     * @return 无。
     * @note 修改格数不影响已保存的百分比，仅改变每格对应的百分比跨度。
     */
    void setSegments(int segments);        // 方格数量，默认 24

    /**
     * @brief 设置颜色切换所用的告警阈值。
     *
     * 两个阈值只影响 fillColor() 的分支判断，不参与填充格数的计算；阈值本身不做
     * 大小关系校验，调用方应保证 warn ≤ danger，否则较小者将永远不会生效。
     *
     * @param[in] warn int，转为琥珀色的起点百分比（默认为 60）。
     * @param[in] danger int，转为红色的起点百分比（默认为 85）。
     * @return 无。
     * @note 阈值变更后立即重绘，使颜色在新阈值下即时生效。
     */
    void setDangerThresholds(int warn, int danger);  // 默认 60 / 85

    /**
     * @brief 给出控件的推荐尺寸。
     *
     * 仅在控件使用 sizeHint 作为高度来源时被布局系统查询；返回的 360 × 22 是
     * 与官网方格条比例接近的经验值，宽度通常会被 Expanding 策略继续放大。
     *
     * @return QSize，推荐尺寸，宽 360、高 22（单位：逻辑像素）。
     * @note 本函数为 const，不得在其中修改任何成员，也不得调用 update()。
     */
    QSize sizeHint() const override;

    /**
     * @brief 给出控件可被压缩到的最小尺寸。
     *
     * 当外层窗口变窄或空间紧张时，布局系统会把控件压到该尺寸；再小则方格宽度
     * 趋近于 0，视觉上失去意义，因此给出 120 × 18 作为下限。
     *
     * @return QSize，最小尺寸，宽 120、高 18（单位：逻辑像素）。
     * @note 本函数为 const，仅返回常量，不产生副作用。
     */
    QSize minimumSizeHint() const override;

protected:
    /**
     * @brief 绘制整个分段条：先算几何，再决定点亮的格数，最后逐格上色。
     *
     * 处理步骤依次为：① 关闭抗锯齿以保证小方格的边缘锐利；② 按「格宽 + 间隙」
     * 计算每格的水平位置；③ 依据百分比换算应点亮的格数（大于 0 至少点亮 1 格）；
     * ④ 用填充色与熄灭色交替绘制圆角矩形。
     *
     * @param[in] event QPaintEvent *，Qt 传入的绘制事件；实现中不使用其内容，
     *                  因此参数未命名（输入方向）。
     * @return 无。
     * @note 绘制只依赖 width()、height() 与 palette()，不得在此发起耗时 I/O，
     *       否则会阻塞界面线程并造成窗口卡顿。
     */
    void paintEvent(QPaintEvent *event) override;

private:
    /**
     * @brief 依据占用率选择填充颜色。
     *
     * 判定顺序为「先危险、后告警、最后正常」，因此当 danger 小于 warn 时红色
     * 分支会被优先命中，保证高危状态不会被误判为正常。
     *
     * @param[in] percent int，用于比较的占用率百分比。
     * @return QColor，返回颜色：≥ danger 为红、≥ warn 为琥珀、其余为绿。
     * @note 本函数为 const 且无副作用，可在绘制循环中安全重复调用。
     */
    QColor fillColor(int percent) const;

    int m_percent = 0;      ///< 当前占用率百分比，恒被裁剪到 0 ~ 100。
    int m_segments = 24;    ///< 方格总数，与官网观感一致的默认值，最小为 4。
    int m_warn = 60;        ///< 转为琥珀色的百分比阈值。
    int m_danger = 85;      ///< 转为红色的百分比阈值。
};
