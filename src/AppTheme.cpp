// ---------------------------------------------------------------------------
//  AppTheme.cpp —— 外观主题的实现：两套调色板、QPalette 构造与全局样式表
//
//  文件结构：
//    ① 两份调色板工厂函数（浅色 / 深色），色值只在此处出现；
//    ② QPalette 构造与样式表生成；
//    ③ 应用与广播逻辑（applyInternal / initialize / setMode）。
//
//  实现约定（补充说明）：
//    · 深色并非把浅色取反：深色背景上的语义色必须"变亮"才有足够对比度，
//      因此 success / warning / danger 在两套调色板中是两组独立取值的颜色；
//    · 所有色值按"对比度优先"挑选，语义色在各自底色上的对比度均不低于 4.5:1
//      （WCAG AA 正文标准），避免再次出现"看不清"的问题；
//    · 本文件不做任何 I/O，不持有窗口指针，可被界面层任意调用。
// ---------------------------------------------------------------------------
#include "AppTheme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>
#include <QWidget>

// ===========================================================================
//  ① 调色板：浅色与深色两套语义色
// ===========================================================================

namespace
{
/**
 * @brief 构造浅色主题调色板。
 *
 * 底色为白与浅灰，文本为近黑；语义色取"深色版"（在浅底上足够醒目）。
 *
 * @return AppTheme::Palette，浅色主题的完整调色板。
 * @note 纯函数，无副作用；返回按值传递，由调用方的静态对象持有。
 */
AppTheme::Palette makeLightPalette()
{
    AppTheme::Palette p;
    p.dark            = false;
    p.window          = QColor(0xF3, 0xF4, 0xF6);   // 窗口底：极浅灰
    p.base            = QColor(0xFF, 0xFF, 0xFF);   // 卡片 / 输入框：纯白
    p.alternateBase   = QColor(0xF7, 0xF8, 0xFA);
    p.button          = QColor(0xEC, 0xEE, 0xF1);
    p.text            = QColor(0x1B, 0x1F, 0x24);   // 近黑，对比度约 15:1
    p.mutedText       = QColor(0x5C, 0x64, 0x70);   // 次要文本，对比度约 6.3:1
    p.disabledText    = QColor(0x9A, 0xA1, 0xAC);
    p.border          = QColor(0xD5, 0xD9, 0xE0);
    p.success         = QColor(0x1B, 0x7F, 0x45);   // 深绿，白底对比度约 4.9:1
    p.warning         = QColor(0x9A, 0x62, 0x09);   // 深琥珀，白底对比度约 5.0:1
    p.danger          = QColor(0xC0, 0x39, 0x2B);   // 沿用旧版警示红，白底约 5.4:1
    p.highlight       = QColor(0x2F, 0x6F, 0xEB);
    p.highlightedText = QColor(0xFF, 0xFF, 0xFF);
    p.bannerWarnBg    = QColor(0xFF, 0xF4, 0xE5);   // 沿用旧版警示横幅配色
    p.bannerWarnFg    = QColor(0x8A, 0x53, 0x00);
    p.bannerWarnBorder = QColor(0xF0, 0xC3, 0x6D);
    p.bannerOkBg      = QColor(0xE8, 0xF5, 0xE9);   // 沿用旧版成功横幅配色
    p.bannerOkFg      = QColor(0x1B, 0x5E, 0x20);
    p.bannerOkBorder  = QColor(0xA5, 0xD6, 0xA7);
    p.barIdle         = QColor(0xD9, 0xDD, 0xE3);   // 熄灭格：浅灰
    return p;
}

/**
 * @brief 构造深色主题调色板。
 *
 * 底色为深灰而非纯黑：纯黑底会让卡片描边与阴影失去层次，深灰能在保持暗色观感的
 * 同时留出可辨识的结构；语义色全部换成"亮色版"，保证在深底上依然醒目。
 *
 * @return AppTheme::Palette，深色主题的完整调色板。
 * @note 纯函数，无副作用；语义色与浅色版是各自独立挑选的，不是简单取反。
 */
AppTheme::Palette makeDarkPalette()
{
    AppTheme::Palette p;
    p.dark            = true;
    p.window          = QColor(0x1B, 0x1D, 0x21);   // 窗口底：近黑深灰
    p.base            = QColor(0x23, 0x26, 0x2B);   // 卡片底：比窗口略亮，形成层次
    p.alternateBase   = QColor(0x2A, 0x2E, 0x34);
    p.button          = QColor(0x2C, 0x30, 0x36);
    p.text            = QColor(0xE8, 0xEA, 0xED);   // 浅灰白，对比度约 13:1
    p.mutedText       = QColor(0xA6, 0xAD, 0xBB);   // 次要文本，对比度约 7.0:1
    p.disabledText    = QColor(0x6B, 0x72, 0x80);
    p.border          = QColor(0x3A, 0x3F, 0x47);
    p.success         = QColor(0x4C, 0xC3, 0x8A);   // 亮绿，深底对比度约 7.4:1
    p.warning         = QColor(0xE3, 0xB3, 0x41);   // 亮琥珀，深底对比度约 8.0:1
    p.danger          = QColor(0xF8, 0x71, 0x71);   // 亮红，深底对比度约 5.9:1
    p.highlight       = QColor(0x3B, 0x82, 0xF6);
    p.highlightedText = QColor(0xFF, 0xFF, 0xFF);
    p.bannerWarnBg    = QColor(0x3A, 0x2C, 0x12);   // 深琥珀底 + 亮琥珀字
    p.bannerWarnFg    = QColor(0xF5, 0xC9, 0x7A);
    p.bannerWarnBorder = QColor(0x6B, 0x52, 0x27);
    p.bannerOkBg      = QColor(0x14, 0x30, 0x1F);   // 深绿底 + 亮绿字
    p.bannerOkFg      = QColor(0x7B, 0xD9, 0x9A);
    p.bannerOkBorder  = QColor(0x2C, 0x5B, 0x3D);
    p.barIdle         = QColor(0x3A, 0x3F, 0x47);   // 熄灭格：深灰，与卡片底可辨
    return p;
}

/**
 * @brief 取浅色调色板（进程内只构造一次）。
 * @return const AppTheme::Palette &，长期有效的只读引用。
 */
const AppTheme::Palette &lightPalette()
{
    static const AppTheme::Palette kPalette = makeLightPalette();
    return kPalette;
}

/**
 * @brief 取深色调色板（进程内只构造一次）。
 * @return const AppTheme::Palette &，长期有效的只读引用。
 */
const AppTheme::Palette &darkPalette()
{
    static const AppTheme::Palette kPalette = makeDarkPalette();
    return kPalette;
}

/// 动态属性的键名：语义级别（"ok" / "warn" / "danger" / 空）
const char *const kLevelProperty = "level";

/// 重入保护：setColorScheme() 可能同步发出 colorSchemeChanged，避免无限递归
bool s_applying = false;

/// 命令行临时覆盖是否生效：true 表示忽略配置文件，一律使用 s_overrideMode
bool s_hasOverride = false;

/// 命令行临时覆盖的目标模式（仅在 s_hasOverride 为 true 时有意义）
AppConfig::ThemeMode s_overrideMode = AppConfig::ThemeMode::system;

/**
 * @brief 把当前模式与调色板落到 QApplication 上（内部实现，不广播）。
 *
 * 处理顺序不可颠倒：先请求平台配色（保留原生控件观感），再下发我方 QPalette 与
 * 样式表——后两者必须在平台主题之后设置，否则会被平台随后刷新的调色板覆盖。
 *
 * @return 无。
 * @note 本函数只应由本文件的公开接口调用；重复进入时直接返回，防止信号递归。
 */
void applyInternal()
{
    // 无 GUI 场景（例如纯自检）下不设置任何外观，直接返回。
    QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    const QStyleHints *hintsRaw = QGuiApplication::styleHints();

    if (s_applying)
        return;
    s_applying = true;

    // 第一步：把"希望的配色方案"告知平台主题。强制浅色 / 深色时使用 setColorScheme，
    // 跟随系统时用 unsetColorScheme() 交还给系统；这一步让原生绘制的控件（标题栏、
    // 菜单、滚动条）也呈现目标配色，而不是只有被样式表覆盖的部分变了色。
    if (hintsRaw != nullptr) {
        QStyleHints *hints = const_cast<QStyleHints *>(hintsRaw);
        // 取值走 AppTheme::mode() 而非 AppConfig::themeMode()：
        // 前者会把命令行临时覆盖考虑进来，保证 --theme 也能驱动平台配色。
        switch (AppTheme::mode()) {
        case AppConfig::ThemeMode::light:
            hints->setColorScheme(Qt::ColorScheme::Light);
            break;
        case AppConfig::ThemeMode::dark:
            hints->setColorScheme(Qt::ColorScheme::Dark);
            break;
        case AppConfig::ThemeMode::system:
        default:
            hints->unsetColorScheme();
            break;
        }
    }

    // 第二步：下发我方调色板与全局样式表。样式表一旦变化，Qt 会自动重新抛光
    // 已存在的控件，因此动态属性（level）驱动的配色会立即换成新主题的颜色。
    if (app != nullptr) {
        app->setPalette(AppTheme::buildPalette());
        app->setStyleSheet(AppTheme::styleSheet());
    }

    s_applying = false;
}
} // namespace

// ===========================================================================
//  ② QPalette 与样式表
// ===========================================================================

namespace AppTheme
{
QPalette buildPalette()
{
    const Palette &p = palette();
    QPalette pal;

    // ---- 常规态 ----
    pal.setColor(QPalette::Window, p.window);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.base);
    pal.setColor(QPalette::AlternateBase, p.alternateBase);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::Button, p.button);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::BrightText, p.danger);
    pal.setColor(QPalette::Highlight, p.highlight);
    pal.setColor(QPalette::HighlightedText, p.highlightedText);
    pal.setColor(QPalette::ToolTipBase, p.base);
    pal.setColor(QPalette::ToolTipText, p.text);
    // 占位提示文字（未输入时的灰字）用次要文本色，避免与真实输入内容混淆。
    pal.setColor(QPalette::PlaceholderText, p.mutedText);

    // ---- 三维边框组与中间色 ----
    // 这些角色供样式自己绘制凹陷/凸起边框时取用；一并赋值可避免原生样式在深色下
    // 仍按浅色的三维明暗绘制，出现"白色高光边"这类突兀细节。
    pal.setColor(QPalette::Light, p.window);
    pal.setColor(QPalette::Midlight, p.barIdle);
    pal.setColor(QPalette::Mid, p.mutedText);
    pal.setColor(QPalette::Dark, p.border);
    pal.setColor(QPalette::Shadow, p.border);

    // ---- 禁用态 ----
    // 三处文本统一降到低对比度；若不设置，深色主题下禁用控件可能反而比可用控件更亮。
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.disabledText);
    pal.setColor(QPalette::Disabled, QPalette::Text, p.disabledText);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.disabledText);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, p.disabledText);

    return pal;
}

QString styleSheet()
{
    const Palette &p = palette();

    // 样式表模板使用 @名字 形式的占位符，逐项替换为十六进制色值。
    // 用命名占位符而非 %1..%n：色值众多时，arg() 链一旦增删顺序就会整体错位，
    // 而命名替换不会因为位置变化而出错（占位符之间不存在前缀包含关系）。
    QString qss = QStringLiteral(R"QSS(
/* ---- 卡片：底色、描边与圆角 ---- */
QFrame#card {
    background-color: @base;
    border: 1px solid @border;
    border-radius: 10px;
}

/* ---- 字号与字重（颜色由调色板统一给出，此处不再写死） ---- */
QLabel#appTitle { font-size: 17px; font-weight: 600; }
QLabel#bigValue { font-size: 24px; font-weight: 600; }
QLabel#limitName { font-weight: 600; }
QLabel#limitPercent { font-weight: 600; }
QLabel#cardTitle { font-size: 11px; font-weight: 600; }

/* ---- 次要文本：账号、右上角状态、卡片说明等 ---- */
QLabel#account { color: @muted; }
QLabel#status  { color: @muted; }
QLabel#muted   { color: @muted; }
QLabel#cardTitle { color: @muted; }

/* ---- 错误汇总 ---- */
QLabel#errors { color: @danger; }

/* ---- 顶部横幅：圆角与内边距常驻，底色随语义级别变化 ---- */
QLabel#banner { padding: 8px 12px; border-radius: 6px; }
QLabel#banner[level="warn"] {
    background-color: @bwBg; color: @bwFg; border: 1px solid @bwBd;
}
QLabel#banner[level="ok"] {
    background-color: @boBg; color: @boFg; border: 1px solid @boBd;
}

/* ---- 百分比文字：按占用率档位着色 ---- */
QLabel#limitPercent[level="ok"]     { color: @success; }
QLabel#limitPercent[level="warn"]   { color: @warning; }
QLabel#limitPercent[level="danger"] { color: @danger; }

/* ---- 设置对话框：常驻提示、逃生说明与状态标签 ---- */
QLabel#dialogHint { color: @muted; }
QLabel#escapeHint { color: @warning; }
QLabel#dialogStatus[level="ok"]     { color: @success; }
QLabel#dialogStatus[level="danger"] { color: @danger; }

/* ---- 悬停提示：深色主题下默认的浅底黑字会与整体观感割裂 ---- */
QToolTip {
    background-color: @base;
    color: @text;
    border: 1px solid @border;
}
)QSS");

    // 逐项替换占位符；replace 全部替换，模板中同一占位符可复用多次。
    qss.replace(QLatin1String("@window"), p.window.name());
    qss.replace(QLatin1String("@base"), p.base.name());
    qss.replace(QLatin1String("@altBase"), p.alternateBase.name());
    qss.replace(QLatin1String("@button"), p.button.name());
    qss.replace(QLatin1String("@text"), p.text.name());
    qss.replace(QLatin1String("@muted"), p.mutedText.name());
    qss.replace(QLatin1String("@disabled"), p.disabledText.name());
    qss.replace(QLatin1String("@border"), p.border.name());
    qss.replace(QLatin1String("@success"), p.success.name());
    qss.replace(QLatin1String("@warning"), p.warning.name());
    qss.replace(QLatin1String("@danger"), p.danger.name());
    qss.replace(QLatin1String("@selBg"), p.highlight.name());
    qss.replace(QLatin1String("@selFg"), p.highlightedText.name());
    qss.replace(QLatin1String("@bwBg"), p.bannerWarnBg.name());
    qss.replace(QLatin1String("@bwFg"), p.bannerWarnFg.name());
    qss.replace(QLatin1String("@bwBd"), p.bannerWarnBorder.name());
    qss.replace(QLatin1String("@boBg"), p.bannerOkBg.name());
    qss.replace(QLatin1String("@boFg"), p.bannerOkFg.name());
    qss.replace(QLatin1String("@boBd"), p.bannerOkBorder.name());
    qss.replace(QLatin1String("@barIdle"), p.barIdle.name());

    return qss;
}

void setLevel(QWidget *widget, const QString &level)
{
    // 调用方可能传入空指针（例如某个可选控件未创建），此处集中兜底，
    // 使业务代码不必到处写判空。
    if (widget == nullptr)
        return;

    // 级别未变化时直接返回：监控界面每次刷新都会重算配色，
    // 若每轮都强制重新抛光，会造成无谓的整控件重绘。
    if (widget->property(kLevelProperty).toString() == level)
        return;

    widget->setProperty(kLevelProperty, level);

    // 动态属性变化不会自动触发样式重算，必须显式 unpolish + polish 一次，
    // 否则 QSS 里的 [level="..."] 选择器不会重新匹配。
    if (QStyle *style = widget->style()) {
        style->unpolish(widget);
        style->polish(widget);
    }
    widget->update();
}
} // namespace AppTheme

// ===========================================================================
//  ③ 应用与广播
// ===========================================================================

namespace AppTheme
{
Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
}

void Notifier::notify()
{
    emit changed();
}

Notifier *notifier()
{
    // 函数内静态对象：首次使用时构造，随进程结束析构，不引入退出顺序问题。
    static Notifier kNotifier;
    return &kNotifier;
}

AppConfig::ThemeMode mode()
{
    // 命令行覆盖优先于配置文件：它的语义就是"本次运行按我说的来"。
    return s_hasOverride ? s_overrideMode : AppConfig::themeMode();
}

bool isDark()
{
    // 强制模式直接给出结论，不依赖平台是否真的响应了 setColorScheme()：
    // 若平台忽略了请求，colorScheme() 仍会返回系统值，此时按系统值倒推会得到
    // 与用户选择相反的结果，因此这里必须先按模式分支。
    switch (mode()) {
    case AppConfig::ThemeMode::light:
        return false;
    case AppConfig::ThemeMode::dark:
        return true;
    case AppConfig::ThemeMode::system:
    default:
        break;
    }

    const QStyleHints *hints = QGuiApplication::styleHints();
    if (hints != nullptr && hints->colorScheme() == Qt::ColorScheme::Dark)
        return true;
    // 系统未提供配色信息（Unknown）或查询失败时按浅色处理：
    // 浅色是"看不清"风险更低的一侧，宁可少一次深色适配，也不要把浅色文字放在深底上。
    return false;
}

const Palette &palette()
{
    return isDark() ? darkPalette() : lightPalette();
}

void initialize(QApplication *app)
{
    // 挂接系统配色变化：仅在"跟随系统"模式下需要重新应用，但为了兼容平台主题
    // 在系统切换后自行刷新调色板的情况，这里统一重应用一次（代价极低）。
    if (QStyleHints *hints = QGuiApplication::styleHints()) {
        QObject::connect(hints, &QStyleHints::colorSchemeChanged, notifier(),
                         [](Qt::ColorScheme) {
                             applyInternal();
                             notifier()->notify();
                         });
    }

    applyInternal();

    // 传入的 app 仅用于语义完整性（本模块内部通过 QCoreApplication::instance()
    // 取应用对象，以支持在任意位置调用），此处显式标记为已使用。
    Q_UNUSED(app)

    notifier()->notify();
}

void setMode(AppConfig::ThemeMode mode)
{
    // 用户显式选择优先于命令行覆盖：清掉覆盖标记，让配置文件重新成为唯一事实来源。
    s_hasOverride = false;
    // 先落盘再应用：即使随后应用过程中出现任何异常分支，配置也已经与用户选择一致。
    AppConfig::setThemeMode(mode);
    applyInternal();
    notifier()->notify();
}

void overrideMode(AppConfig::ThemeMode mode)
{
    s_hasOverride = true;
    s_overrideMode = mode;
    // 不写配置文件：覆盖的语义就是"只影响本次运行"。
    applyInternal();
    notifier()->notify();
}
} // namespace AppTheme
