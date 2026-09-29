// ---------------------------------------------------------------------------
//  SegmentedBar.cpp —— 分段式进度条控件的实现（状态收敛与自绘逻辑）
//
//  本文件实现 SegmentedBar.h 中声明的全部成员：三个状态 setter、两个尺寸提示
//  函数、一个颜色判定函数，以及唯一的绘制入口 paintEvent()。
// ---------------------------------------------------------------------------
#include "SegmentedBar.h"

#include <QPainter>
#include <QPaintEvent>

// 实现约定（补充说明）：
//   1. 所有 setter 都遵循「裁剪 → 判等 → 赋值 → update()」的顺序，既保证内部
//      状态始终合法，又避免重复赋值引发多余重绘；
//   2. 绘制所需的几何量（间隙、格宽、圆角半径）全部在 paintEvent() 内即时算，
//      不缓存为成员，避免窗口缩放后出现几何量失效或残影的问题；
//   3. 颜色不写死在绘制循环里，而是交由 fillColor() 统一判定，便于后续调整阈值；
//   4. 本文件只依赖 QtGui / QtWidgets，不引入任何业务模块，保持低耦合。

/**
 * @brief 构造分段式进度条控件，并设定其尺寸伸缩策略。
 *
 * 构造函数只做一件事：把尺寸策略设为「水平可扩展、垂直固定」，使控件在水平方向
 * 填满父布局给出的宽度，在垂直方向始终采用 sizeHint() 的高度。之所以在构造阶段
 * 就固定下来，是因为该策略在整个生命周期内都不会变化，无需重复设置。
 *
 * @param[in] parent QWidget *，父对象指针；可为 nullptr，此时控件无父对象，其
 *                   销毁时机由调用方负责（输入方向）。
 * @return 无。
 * @note 默认成员初始化已把百分比置 0、格数置 24，因此构造完成后控件立即可绘制，
 *       呈现为一整条全部熄灭的方格条。
 */
SegmentedBar::SegmentedBar(QWidget *parent)
    : QWidget(parent)
{
    // 水平方向允许拉伸以填满可用宽度，垂直方向锁定为 sizeHint() 的高度，
    // 这样在多行表单中进度条不会被纵向拉高，保持与官网一致的细条观感。
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

/**
 * @brief 设置当前占用率百分比，必要时触发重绘。
 *
 * 处理流程为：先用 qBound() 把入参夹到 0 ~ 100，再与当前值比较——只有真正发生
 * 变化时才赋值并调用 update()。判等这一步很关键：监控界面可能每秒刷新多次，
 * 若数值未变仍重绘，会白白消耗界面线程的绘制预算。
 *
 * @param[in] percent int，占用率百分比；小于 0 按 0 处理，大于 100 按 100 处理。
 * @return 无。
 * @note update() 只是投递重绘请求，真正的绘制发生在下一次事件循环的
 *       paintEvent() 中，因此本函数开销极小，可在高频刷新路径上安全调用。
 */
void SegmentedBar::setPercent(int percent)
{
    // 越界值统一裁剪到合法区间，避免后续换算填充格数时出现负数或溢出。
    const int clamped = qBound(0, percent, 100);
    // 数值未变化时直接返回，既省下一次 update()，也让重绘频率与数据变化对齐。
    if (clamped == m_percent)
        return;
    m_percent = clamped;
    update();
}

/**
 * @brief 设置方格总数，并触发重绘。
 *
 * 内部用 qMax(4, ...) 兜底：格数过少时进度条会退化成几个大色块，失去「分段条」
 * 的语义；下限 4 既保证仍能表达「分段」，又不会让间距挤压掉可用宽度。
 *
 * @param[in] segments int，期望的方格数量；小于 4 时按 4 处理。
 * @return 无。
 * @note 与 setPercent() 不同，这里不做判等，因为格数变更属于低频配置型操作，
 *       上层通常只在初始化或主题切换时调用一次。
 */
void SegmentedBar::setSegments(int segments)
{
    // 至少保留 4 格，确保任何配置下都能看出「分段」而非单一色块。
    m_segments = qMax(4, segments);
    update();
}

/**
 * @brief 设置绿 / 琥珀 / 红的切换阈值，并触发重绘。
 *
 * 两个阈值不参与填充格数的计算，只在 fillColor() 中决定使用哪种颜色；本函数
 * 不做 warn ≤ danger 的校验，调用方需自行保证顺序，否则较小的阈值会被覆盖。
 *
 * @param[in] warn int，占用率达到或超过该值时转为琥珀色（默认 60）。
 * @param[in] danger int，占用率达到或超过该值时转为红色（默认 85）。
 * @return 无。
 * @note 阈值变更后立即重绘，使配色在新阈值下即时生效；若阈值未变，本次重绘
 *       仍会执行，属于可接受的少量冗余，以换取实现上的简单直接。
 */
void SegmentedBar::setDangerThresholds(int warn, int danger)
{
    m_warn = warn;
    m_danger = danger;
    // 阈值只影响颜色，但颜色在绘制时才确定，因此必须请求一次重绘。
    update();
}

/**
 * @brief 返回控件的推荐尺寸。
 *
 * 宽 360、高 22 是与官网方格条比例接近的经验值：宽度通常会被 Expanding 策略
 * 继续放大，高度则会被布局系统原样采用，从而得到一条细长的方格条。
 *
 * @return QSize，推荐尺寸，宽 360、高 22（逻辑像素）。
 * @note 本函数为 const，禁止在其中修改成员或调用 update()；Qt 可能在任意时刻
 *       查询该值，因此实现必须是纯计算、无副作用的。
 */
QSize SegmentedBar::sizeHint() const
{
    return QSize(360, 22);
}

/**
 * @brief 返回控件可被压缩到的最小尺寸。
 *
 * 当外层窗口变窄或布局空间不足时，Qt 会退而使用该值；再小于 120 × 18 时每格
 * 宽度趋近于零，视觉上已无法辨识，因此以此作为下限。
 *
 * @return QSize，最小尺寸，宽 120、高 18（逻辑像素）。
 * @note 与 sizeHint() 一样必须是纯函数；两者共同决定控件在布局中的弹性区间。
 */
QSize SegmentedBar::minimumSizeHint() const
{
    return QSize(120, 18);
}

/**
 * @brief 依据占用率选择填充颜色。
 *
 * 判定顺序刻意写成「先危险、后告警、最后正常」：一旦 danger 被配置成小于 warn，
 * 红色分支也会优先命中，从而保证高危状态不会被误判成正常色。
 *
 * @param[in] percent int，用于比较的占用率百分比。
 * @return QColor，返回颜色：≥ danger 为红、≥ warn 为琥珀、其余为绿。
 * @note 本函数为 const 且无副作用，可在绘制循环中被反复调用；返回值按值传递，
 *       调用方拿到的是独立副本，修改它不会影响控件内部状态。
 */
QColor SegmentedBar::fillColor(int percent) const
{
    // 红色优先判定，保证最高危状态在任何阈值配置下都不会被降级显示。
    if (percent >= m_danger)
        return QColor(0xE0, 0x4B, 0x4B);   // 红
    // 次高优先级：越过告警阈值即转为琥珀色，提示用户留意配额消耗速度。
    if (percent >= m_warn)
        return QColor(0xE8, 0xA3, 0x3D);   // 琥珀
    // 兜底分支：未达任何阈值时使用绿色，表示配额充裕。
    return QColor(0x2E, 0xA0, 0x62);       // 绿
}

/**
 * @brief 绘制整个分段条：算几何 → 定格数 → 逐格上色。
 *
 * 完整步骤：
 *   ① 以本控件为画布创建 QPainter；
 *   ② 关闭抗锯齿，让小尺寸方格的四边保持锐利，与官网的像素级观感一致；
 *   ③ 计算间隙总量与单格宽度：总宽减去 (格数 - 1) 个间隙后均分给各格；
 *   ④ 依据百分比换算出应点亮的格数，其中「大于 0% 至少点亮 1 格」是产品规则，
 *      避免出现「明明有占用却整条熄灭」的误导性显示；
 *   ⑤ 循环绘制每一格：先算水平偏移，再以圆角矩形绘制，点亮格用活动色、
 *      其余格用由调色板派生的熄灭色。
 *
 * @param[in] event QPaintEvent *，Qt 派发的绘制事件；本实现不使用其内容，
 *                  故参数未命名（输入方向）。
 * @return 无。
 * @note 绘制全程只读，不修改任何成员，也不发起 I/O；所有几何量都是局部常量，
 *       因此窗口缩放时会自然重算，无需额外的 resizeEvent 处理。
 */
void SegmentedBar::paintEvent(QPaintEvent *)
{
    // 画布即本控件自身；QPainter 在析构时自动结束绘制，无需手动 end()。
    QPainter painter(this);
    // 关闭抗锯齿：方格尺寸很小，开启后边缘会发虚，反而失去官网那种硬朗的方格感。
    painter.setRenderHint(QPainter::Antialiasing, false);

    // 相邻方格之间的水平间隙（逻辑像素），固定为 2 以保持与官网一致的疏密比。
    const int gap = 2;
    // 用局部常量取一次格数，避免循环条件反复读取成员造成理解与维护上的干扰。
    const int count = m_segments;
    // 间隙总数为「格数 - 1」个：格与格之间才有间隙，首尾外侧不留间隙。
    const double totalGap = gap * (count - 1);
    // 单格宽度 = 可用宽度均分；用 double 计算以保留小数，避免整数除法截断误差累积。
    const double segWidth = (width() - totalGap) / static_cast<double>(count);
    // 圆角半径，取值很小，仅用来磨掉方格的尖角，视觉上依旧保持方形。
    const int radius = 2;

    // 与官网一致：只要大于 0% 就至少点亮 1 格
    // 先假定一格都不点亮；只有占用率严格大于 0 时才进入填充计算。
    int filled = 0;
    if (m_percent > 0) {
        // qMax(1, ...) 是产品规则的关键：哪怕只有 1% 占用，也必须点亮 1 格，
        // 否则用户会误以为完全没有消耗；floor 则让填充格数随百分比单调递增。
        filled = qMax(1, static_cast<int>(std::floor(m_percent / 100.0 * count)));
        // 浮点乘法在 100% 时理论上刚好等于 count，但仍用 qMin 兜底防止越界。
        filled = qMin(filled, count);
    }

    // 活动色由占用率决定，只计算一次供整轮循环复用，避免每格重复判定阈值。
    const QColor active = fillColor(m_percent);
    // 熄灭色取自调色板的中亮色并略微提亮，使其在深色 / 浅色主题下都能保持可见，
    // 而不是写死灰度值——这样控件能自动跟随系统或应用主题。
    const QColor idle = palette().color(QPalette::Midlight).lighter(118);

    // 逐格绘制：索引小于 filled 的格子使用活动色，其余使用熄灭色。
    for (int i = 0; i < count; ++i) {
        // 第 i 格的左边界 = 前面 i 个「格宽 + 间隙」之和。
        const double x = i * (segWidth + gap);
        // 每格顶天立地占满控件高度，因此纵向起点为 0、高度为 height()。
        QRectF rect(x, 0.0, segWidth, height());
        // 关闭描边，避免圆角矩形出现比填充更深的外框线。
        painter.setPen(Qt::NoPen);
        // 依据是否点亮选择画刷颜色；用三元表达式一次决定，逻辑一目了然。
        painter.setBrush(i < filled ? active : idle);
        // 使用 drawRoundedRect 而非 fillRect：小圆角能在保持方格感的同时消除
        // 生硬的直角，且与官网方格的圆角处理保持一致。
        painter.drawRoundedRect(rect, radius, radius);
    }
}
