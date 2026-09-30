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
//
//  为什么把"配色"单独抽成一个模块（而不是写进主窗口的样式表）：
//    · 主题是跨控件的横切关注点：主窗口、设置对话框、进度条、托盘图标都要用同一套
//      语义色，若各自维护一份色值，改一处必然漏一处，最终表现为"深浅主题下同一个
//      百分比有两种颜色"；
//    · QPalette 与 QSS 必须成对下发：只改 QSS 的话，原生绘制的部分（标题栏、菜单、
//      滚动条、工具提示）仍是系统色，深色主题下会出现刺眼的浅色块；
//    · 语义色与具体色值解耦：控件只声明"我是 danger 级别"，由本模块翻译成当前主题
//      下的具体颜色，于是"跟随系统"切换时无需任何控件参与。
//
//  一次主题应用的完整时序（理解本文件的关键）：
//    ① 请求平台配色（setColorScheme / unsetColorScheme）→ 影响原生绘制部分；
//    ② 下发 QPalette → 影响所有按调色板取色的控件；
//    ③ 下发全局样式表 → 覆盖卡片/横幅/百分比等着色细节，并触发全量重新抛光；
//    ④ 广播 changed() → 需要按新配色重绘自绘控件（如分段进度条）的模块自行响应。
//    ②③ 必须晚于 ①：平台随后可能刷新调色板，先写的会被覆盖掉。
//
//  阅读建议：色值本身不需要记忆，重要的是每组颜色"服务于什么语义、在哪种底色上"。
// ---------------------------------------------------------------------------
#include "AppTheme.h"

// QApplication：设置应用级 QPalette 与全局样式表（主题最终落地的两个载体）。
#include <QApplication>
// QCoreApplication：在无 GUI 场景（纯自检）下取进程内应用对象并判断其真实类型。
#include <QCoreApplication>
// QGuiApplication：读取/写入平台配色提示（styleHints），用于"跟随系统"。
#include <QGuiApplication>
// QPalette：Qt 的颜色角色表，承载窗口、文本、按钮、禁用态等基础配色。
#include <QPalette>
// QStyle：动态属性变化后手工重新抛光控件，使 QSS 的 [level="..."] 选择器重新匹配。
#include <QStyle>
// QStyleHints：平台级外观提示（配色方案、是否深色）的唯一查询入口。
#include <QStyleHints>
// QWidget：setLevel() 的入参类型，需要完整定义才能取 style() 与动态属性。
#include <QWidget>

// ===========================================================================
//  ① 调色板：浅色与深色两套语义色
//
//  设计原则（两套调色板共用的取舍逻辑）：
//    · 层级用"明度差"表达：窗口底 → 卡片底 → 交替行，逐级抬亮（深色下）或压暗
//      （浅色下），用户才能一眼看出"哪块是卡片、哪块是背景"；
//    · 语义色按底色分别挑选：同一个"成功"在浅底上用深绿、在深底上用亮绿，
//      两者不是同一个色值的不同透明度，而是两个独立的色值；
//    · 边框比底色只深/亮一档：既画出边界，又不会把界面切成格子；
//    · 所有色值写死为十六进制常量而非主题推导：主题要"稳定可复现"，
//      任何运行时推导都会让同一版本在不同机器上呈现不同颜色。
// ===========================================================================

namespace
{
/**
 * @brief 构造浅色主题调色板。
 *
 * 底色为白与浅灰，文本为近黑；语义色取"深色版"（在浅底上足够醒目）。
 * 逐项赋值的顺序与 Palette 结构的字段顺序一致，便于与深色版对照阅读。
 *
 * @return AppTheme::Palette，浅色主题的完整调色板。
 * @note 纯函数，无副作用；返回按值传递，由调用方的静态对象持有。
 * @note 这里的色值只在此函数出现一次：界面层不允许再写任何字面颜色，
 *       否则深色主题下必然出现"某个控件没跟着变色"的漏网之鱼。
 */
AppTheme::Palette makeLightPalette()
{
    AppTheme::Palette p;
    // 主题自身标记：自绘控件（进度条）据此决定用深色还是浅色的熄灭格。
    p.dark            = false;
    // 窗口底：极浅灰。之所以不用纯白，是为了让白色卡片能从背景里"浮"出来。
    p.window          = QColor(0xF3, 0xF4, 0xF6);
    // 卡片 / 输入框：纯白，与窗口底形成最小但足够的明度差。
    p.base            = QColor(0xFF, 0xFF, 0xFF);
    // 交替行（列表隔行）：比底色略压暗，用于表格类控件的斑马纹。
    p.alternateBase   = QColor(0xF7, 0xF8, 0xFA);
    // 按钮底：比卡片底略深，让按钮默认就"看起来可以按"。
    p.button          = QColor(0xEC, 0xEE, 0xF1);
    // 主文本：近黑（非纯黑）。纯黑在 LCD 上笔画过重，近黑更耐看且对比度仍约 15:1。
    p.text            = QColor(0x1B, 0x1F, 0x24);
    // 次要文本（账号、状态、说明）：中灰，对比度约 6.3:1，仍高于 AA 的 4.5:1。
    p.mutedText       = QColor(0x5C, 0x64, 0x70);
    // 禁用文本：明显更浅，让"不可用"一眼可辨；不参与语义表达，故不追求对比度。
    p.disabledText    = QColor(0x9A, 0xA1, 0xAC);
    // 边框：比窗口底深一档的灰，负责勾出卡片轮廓而不抢内容。
    p.border          = QColor(0xD5, 0xD9, 0xE0);
    // 成功（占用率正常）：深绿，白底对比度约 4.9:1，刚好越过 AA 线。
    p.success         = QColor(0x1B, 0x7F, 0x45);
    // 警告（占用率偏高）：深琥珀，白底对比度约 5.0:1；比纯黄更易读。
    p.warning         = QColor(0x9A, 0x62, 0x09);
    // 危险（占用率超限）：沿用旧版警示红，白底约 5.4:1，与既有截图观感一致。
    p.danger          = QColor(0xC0, 0x39, 0x2B);
    // 选中底色：中蓝，与白字搭配用于选区、焦点高亮。
    p.highlight       = QColor(0x2F, 0x6F, 0xEB);
    // 选中文字：白。选中的是"蓝底"，因此白色在这里对比度最高。
    p.highlightedText = QColor(0xFF, 0xFF, 0xFF);
    // 警示横幅底：浅橙，沿用旧版配色，与 danger 的红色区分开（横幅是提示，不是错误）。
    p.bannerWarnBg    = QColor(0xFF, 0xF4, 0xE5);
    // 警示横幅文字：深棕，与浅橙底对比度充足。
    p.bannerWarnFg    = QColor(0x8A, 0x53, 0x00);
    // 警示横幅描边：浅琥珀，把横幅与卡片底色分开。
    p.bannerWarnBorder = QColor(0xF0, 0xC3, 0x6D);
    // 成功横幅底：浅绿，用于"API Key 已保存"这类一次性提示。
    p.bannerOkBg      = QColor(0xE8, 0xF5, 0xE9);
    // 成功横幅文字：深绿。
    p.bannerOkFg      = QColor(0x1B, 0x5E, 0x20);
    // 成功横幅描边：浅绿。
    p.bannerOkBorder  = QColor(0xA5, 0xD6, 0xA7);
    // 进度条熄灭格：浅灰。比窗口底略深，保证"未填充"的部分依然看得见轮廓。
    p.barIdle         = QColor(0xD9, 0xDD, 0xE3);
    return p;
}

/**
 * @brief 构造深色主题调色板。
 *
 * 底色为深灰而非纯黑：纯黑底会让卡片描边与阴影失去层次，深灰能在保持暗色观感的
 * 同时留出可辨识的结构；语义色全部换成"亮色版"，保证在深底上依然醒目。
 * 与浅色版逐行对应，便于对照审查"同一个角色在两套主题下分别是什么"。
 *
 * @return AppTheme::Palette，深色主题的完整调色板。
 * @note 纯函数，无副作用；语义色与浅色版是各自独立挑选的，不是简单取反。
 * @note 深色下所有文本 / 语义色的对比度同样按 ≥ 4.5:1 校核（注释里给出实测值），
 *       这是本项目"深色下看不清"问题的修复依据。
 */
AppTheme::Palette makeDarkPalette()
{
    AppTheme::Palette p;
    // 主题自身标记：自绘控件据此切换熄灭格与文字色。
    p.dark            = true;
    // 窗口底：近黑深灰。最暗的一层，负责"退到后面去"。
    p.window          = QColor(0x1B, 0x1D, 0x21);
    // 卡片底：比窗口略亮，形成层次；深色主题下"更亮 = 更靠前"。
    p.base            = QColor(0x23, 0x26, 0x2B);
    // 交替行：再亮一档，供斑马纹使用。
    p.alternateBase   = QColor(0x2A, 0x2E, 0x34);
    // 按钮底：与卡片底拉开一档，避免按钮"陷进"卡片里。
    p.button          = QColor(0x2C, 0x30, 0x36);
    // 主文本：浅灰白而非纯白，避免深底上的高亮溢出（halation），对比度约 13:1。
    p.text            = QColor(0xE8, 0xEA, 0xED);
    // 次要文本：中浅灰，对比度约 7.0:1，仍高于 AA 线。
    p.mutedText       = QColor(0xA6, 0xAD, 0xBB);
    // 禁用文本：进一步压暗，与可用文本形成明确区分。
    p.disabledText    = QColor(0x6B, 0x72, 0x80);
    // 边框：比卡片底亮一档的灰，深色下用"提亮"而不是压暗来勾轮廓。
    p.border          = QColor(0x3A, 0x3F, 0x47);
    // 成功：亮绿，深底对比度约 7.4:1 —— 与浅色版的深绿是两个独立色值。
    p.success         = QColor(0x4C, 0xC3, 0x8A);
    // 警告：亮琥珀，深底对比度约 8.0:1，深色下比橙色系更清晰。
    p.warning         = QColor(0xE3, 0xB3, 0x41);
    // 危险：亮红，深底对比度约 5.9:1；不能沿用浅色版的深红（会糊在深底里）。
    p.danger          = QColor(0xF8, 0x71, 0x71);
    // 选中底色：稍亮的蓝，保证在深底上依然可辨。
    p.highlight       = QColor(0x3B, 0x82, 0xF6);
    // 选中文字：白，与蓝色高亮底对比度最高。
    p.highlightedText = QColor(0xFF, 0xFF, 0xFF);
    // 警示横幅底：深琥珀底 + 亮琥珀字，保持"提示"语义但整体压暗。
    p.bannerWarnBg    = QColor(0x3A, 0x2C, 0x12);
    // 警示横幅文字：亮琥珀，与深底对比度充足。
    p.bannerWarnFg    = QColor(0xF5, 0xC9, 0x7A);
    // 警示横幅描边：中棕，勾出横幅边界。
    p.bannerWarnBorder = QColor(0x6B, 0x52, 0x27);
    // 成功横幅底：深绿底 + 亮绿字。
    p.bannerOkBg      = QColor(0x14, 0x30, 0x1F);
    // 成功横幅文字：亮绿。
    p.bannerOkFg      = QColor(0x7B, 0xD9, 0x9A);
    // 成功横幅描边：中绿。
    p.bannerOkBorder  = QColor(0x2C, 0x5B, 0x3D);
    // 进度条熄灭格：深灰，比卡片底略亮，保证未填充部分与卡片底可辨。
    p.barIdle         = QColor(0x3A, 0x3F, 0x47);
    return p;
}

/**
 * @brief 取浅色调色板（进程内只构造一次）。
 *
 * 之所以用函数内静态对象而不是全局对象：静态局部变量在首次调用时构造，
 * 早于 main() 的全局构造顺序问题被规避掉，也不需要额外的初始化函数。
 *
 * @return const AppTheme::Palette &，长期有效的只读引用。
 * @note 返回引用而非值：调用方只读访问，避免每次取色都复制一份调色板。
 */
const AppTheme::Palette &lightPalette()
{
    static const AppTheme::Palette kPalette = makeLightPalette();
    return kPalette;
}

/**
 * @brief 取深色调色板（进程内只构造一次）。
 * @return const AppTheme::Palette &，长期有效的只读引用。
 * @note 与 lightPalette() 完全对称：两者都只在首次调用时构造，且都返回只读引用。
 */
const AppTheme::Palette &darkPalette()
{
    static const AppTheme::Palette kPalette = makeDarkPalette();
    return kPalette;
}

/// 动态属性的键名：语义级别（"ok" / "warn" / "danger" / 空）
/// 之所以用动态属性而不是给控件加子类：配色是"附加信息"，动态属性不侵入控件类型，
/// 且 QSS 可以用 [level="..."] 直接选择，无需任何 C++ 侧的查询代码。
const char *const kLevelProperty = "level";

/// 重入保护：setColorScheme() 可能同步发出 colorSchemeChanged，避免无限递归
/// 时序：applyInternal → setColorScheme → (平台同步回调) colorSchemeChanged
///       → applyInternal → …若不设此闸门，栈会无限增长。
bool s_applying = false;

/// 命令行临时覆盖是否生效：true 表示忽略配置文件，一律使用 s_overrideMode
/// 与 AppConfig 中的持久化值分开存放：覆盖的语义是"只影响本次运行"，不能写盘。
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
 * @note 无 GUI 场景（如 --selftest 只有 QCoreApplication）下安全空转：取不到
 *       QApplication 时跳过样式下发，但仍会设置平台配色提示。
 */
void applyInternal()
{
    // 无 GUI 场景（例如纯自检）下不设置任何外观，直接返回。
    // 用 qobject_cast 判断真实类型：只有 QApplication 才支持 setPalette/setStyleSheet。
    QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    // 平台外观提示的只读句柄；拿不到（无 QGuiApplication）时后续步骤自动跳过。
    const QStyleHints *hintsRaw = QGuiApplication::styleHints();

    // 重入闸门：由平台配色回调再次进入时立即返回，保证栈深度恒为 1。
    if (s_applying)
        return;
    s_applying = true;

    // 第一步：把"希望的配色方案"告知平台主题。强制浅色 / 深色时使用 setColorScheme，
    // 跟随系统时用 unsetColorScheme() 交还给系统；这一步让原生绘制的控件（标题栏、
    // 菜单、滚动条）也呈现目标配色，而不是只有被样式表覆盖的部分变了色。
    if (hintsRaw != nullptr) {
        // styleHints() 返回 const 指针（其设计意图是"只读提示"），但 setColorScheme
        // 是官方提供的写入接口，因此这里必须去掉 const 才能调用。
        QStyleHints *hints = const_cast<QStyleHints *>(hintsRaw);
        // 取值走 AppTheme::mode() 而非 AppConfig::themeMode()：
        // 前者会把命令行临时覆盖考虑进来，保证 --theme 也能驱动平台配色。
        switch (AppTheme::mode()) {
        case AppConfig::ThemeMode::light:
            // 强制浅色：让原生控件（标题栏、菜单）也走浅色。
            hints->setColorScheme(Qt::ColorScheme::Light);
            break;
        case AppConfig::ThemeMode::dark:
            // 强制深色：同上，避免"应用是深色、标题栏还是浅色"的割裂感。
            hints->setColorScheme(Qt::ColorScheme::Dark);
            break;
        case AppConfig::ThemeMode::system:
        default:
            // 跟随系统：交还控制权，之后由系统的 colorSchemeChanged 驱动重新应用。
            hints->unsetColorScheme();
            break;
        }
    }

    // 第二步：下发我方调色板与全局样式表。样式表一旦变化，Qt 会自动重新抛光
    // 已存在的控件，因此动态属性（level）驱动的配色会立即换成新主题的颜色。
    if (app != nullptr) {
        // 调色板负责"基础色"（窗口、文本、按钮、禁用态），样式表负责"细节色"
        // （卡片、横幅、百分比）；两者缺一都会出现部分控件不跟着换主题。
        app->setPalette(AppTheme::buildPalette());
        app->setStyleSheet(AppTheme::styleSheet());
    }

    // 释放闸门：必须在所有可能抛出的步骤之后再置回，保证下一次调用能正常进入。
    s_applying = false;
}
} // namespace

// ===========================================================================
//  ② QPalette 与样式表
//
//  分工说明：
//    · QPalette 管"平台认识的颜色角色"（窗口底、文本、按钮、选中、禁用态、
//      三维边框组），这些角色由 Qt 样式与原生控件直接取用；
//    · 样式表管"本项目自定义的视觉细节"（卡片圆角/描边、横幅配色、百分比按档位
//      着色、工具提示底色），这些是 QPalette 表达不了的；
//    · 两者都从同一个 Palette 结构取色，因此不可能出现"调色板是深色、样式表是浅色"
//      的错配。
// ===========================================================================

namespace AppTheme
{
/**
 * @brief 依据当前主题构造 QPalette。
 *
 * 覆盖三类角色：常规态、三维边框组与中间色、禁用态。之所以连三维边框组都显式赋值，
 * 是因为原生样式在深色主题下仍可能按浅色语义绘制三维明暗（出现白色高光边）。
 *
 * @return QPalette，可直接交给 QApplication::setPalette() 的调色板。
 * @note 每次调用都重新构造：QPalette 是轻量值类型，避免缓存带来的"主题切换后
 *       部分控件还用旧调色板"的风险。
 * @note 本函数只读取 palette()（即当前主题），不做任何全局状态写入。
 */
QPalette buildPalette()
{
    // 当前主题的调色板：浅色或深色，由 mode()/isDark() 决定。
    const Palette &p = palette();
    // 默认构造的 QPalette 已带一组平台默认色，这里逐项覆盖需要改的角色。
    QPalette pal;

    // ---- 常规态 ----
    // 窗口底：所有非卡片区域的背景。
    pal.setColor(QPalette::Window, p.window);
    // 窗口上的文字：标题、普通标签。
    pal.setColor(QPalette::WindowText, p.text);
    // 卡片 / 输入框底：白色（浅色）或比窗口略亮（深色）。
    pal.setColor(QPalette::Base, p.base);
    // 交替行底：列表、表格的斑马纹用色。
    pal.setColor(QPalette::AlternateBase, p.alternateBase);
    // 控件内文字（输入框、列表项）。
    pal.setColor(QPalette::Text, p.text);
    // 按钮底：让按钮与卡片面区分开。
    pal.setColor(QPalette::Button, p.button);
    // 按钮文字：与正文同色，保持整体一致。
    pal.setColor(QPalette::ButtonText, p.text);
    // 高亮文本（如警示性文字）：用 danger 色，保证"刺眼"语义在两种主题下都成立。
    pal.setColor(QPalette::BrightText, p.danger);
    // 选中/焦点底色。
    pal.setColor(QPalette::Highlight, p.highlight);
    // 选中项文字：白字配蓝底，对比度最高。
    pal.setColor(QPalette::HighlightedText, p.highlightedText);
    // 工具提示底：跟随卡片底，深色主题下才不会出现浅底黑字的割裂感。
    pal.setColor(QPalette::ToolTipBase, p.base);
    // 工具提示文字。
    pal.setColor(QPalette::ToolTipText, p.text);
    // 占位提示文字（未输入时的灰字）用次要文本色，避免与真实输入内容混淆。
    pal.setColor(QPalette::PlaceholderText, p.mutedText);

    // ---- 三维边框组与中间色 ----
    // 这些角色供样式自己绘制凹陷/凸起边框时取用；一并赋值可避免原生样式在深色下
    // 仍按浅色的三维明暗绘制，出现"白色高光边"这类突兀细节。
    // Light：三维凸起的受光面，跟随窗口底（不再另设高光）。
    pal.setColor(QPalette::Light, p.window);
    // Midlight：介于受光面与中间色之间，用熄灭格色充当。
    pal.setColor(QPalette::Midlight, p.barIdle);
    // Mid：三维中间色，用次要文本色，保证在两种主题下都有足够明度差。
    pal.setColor(QPalette::Mid, p.mutedText);
    // Dark：三维阴影面，用边框色，负责勾出"凹陷"的观感。
    pal.setColor(QPalette::Dark, p.border);
    // Shadow：最深的阴影，与边框同色即可，避免深色主题下出现过重的黑边。
    pal.setColor(QPalette::Shadow, p.border);

    // ---- 禁用态 ----
    // 三处文本统一降到低对比度；若不设置，深色主题下禁用控件可能反而比可用控件更亮。
    // 窗口文字（标签、标题）禁用态。
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.disabledText);
    // 控件内文字（输入框内容）禁用态。
    pal.setColor(QPalette::Disabled, QPalette::Text, p.disabledText);
    // 按钮文字禁用态："刷新"按钮在抓取期间就是靠这个变灰。
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.disabledText);
    // 选中项文字禁用态：与其它禁用文本保持一致，避免出现"半灰半白"。
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, p.disabledText);

    return pal;
}

/**
 * @brief 生成全局样式表（QSS），色值全部来自当前主题的调色板。
 *
 * 模板用 @名字 形式的占位符，最后逐项替换成十六进制色值。之所以不用 %1..%n：
 * 色值众多时 arg() 链一旦增删顺序就会整体错位，而命名替换不受顺序影响。
 *
 * @return QString，可直接交给 QApplication::setStyleSheet() 的样式表文本。
 * @note 选择器只依赖两类信息：控件的 objectName，以及语义级别动态属性 level。
 *       因此界面层新增控件时，只需给它起一个已在模板中登记的名字即可复用配色。
 * @note 本函数每次调用都重新拼装：样式表是纯字符串，重建成本远低于排查"切换主题后
 *       某个控件没变色"的时间。
 */
QString styleSheet()
{
    // 当前主题的调色板：模板里的每个占位符最终都映射到它上面的某个字段。
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

/* ---- 状态栏：查询失败时整体标红（成功 / 刷新中不设级别，用默认色） ---- */
QLabel#status[level="danger"] { color: @danger; }

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
    // 窗口底：@window 目前只在需要显式指定窗口背景时使用。
    qss.replace(QLatin1String("@window"), p.window.name());
    // 卡片 / 输入框底：模板中 @base 出现多次（卡片、横幅之外的容器、工具提示）。
    qss.replace(QLatin1String("@base"), p.base.name());
    // 交替行底：留给列表类控件扩展使用。
    qss.replace(QLatin1String("@altBase"), p.alternateBase.name());
    // 按钮底：供将来的自定义按钮复用。
    qss.replace(QLatin1String("@button"), p.button.name());
    // 主文本色：工具提示文字等需要显式指定前景色的场合。
    qss.replace(QLatin1String("@text"), p.text.name());
    // 次要文本色：账号、状态栏、卡片标题共用同一个灰。
    qss.replace(QLatin1String("@muted"), p.mutedText.name());
    // 禁用文本色：留给需要显式表达"不可用"的选择器。
    qss.replace(QLatin1String("@disabled"), p.disabledText.name());
    // 边框色：卡片描边与工具提示描边共用。
    qss.replace(QLatin1String("@border"), p.border.name());
    // 成功语义色：百分比正常档、设置对话框的成功提示。
    qss.replace(QLatin1String("@success"), p.success.name());
    // 警告语义色：百分比偏高档、设置对话框的逃生说明。
    qss.replace(QLatin1String("@warning"), p.warning.name());
    // 危险语义色：百分比超限档、状态栏失败提示。
    qss.replace(QLatin1String("@danger"), p.danger.name());
    // 选中底色 / 选中文字：供将来需要显式高亮的控件使用。
    qss.replace(QLatin1String("@selBg"), p.highlight.name());
    qss.replace(QLatin1String("@selFg"), p.highlightedText.name());
    // 警示横幅三件套（底 / 字 / 描边）：由 showBanner(level="warn") 命中。
    qss.replace(QLatin1String("@bwBg"), p.bannerWarnBg.name());
    qss.replace(QLatin1String("@bwFg"), p.bannerWarnFg.name());
    qss.replace(QLatin1String("@bwBd"), p.bannerWarnBorder.name());
    // 成功横幅三件套（底 / 字 / 描边）：由 showBanner(level="ok") 命中。
    qss.replace(QLatin1String("@boBg"), p.bannerOkBg.name());
    qss.replace(QLatin1String("@boFg"), p.bannerOkFg.name());
    qss.replace(QLatin1String("@boBd"), p.bannerOkBorder.name());
    // 进度条熄灭格：自绘控件直接取用调色板字段，此处保留占位符以便样式表扩展。
    qss.replace(QLatin1String("@barIdle"), p.barIdle.name());

    return qss;
}

/**
 * @brief 给控件打上语义级别，使其按当前主题变色。
 *
 * 语义级别是"颜色含义"而非"具体颜色"：调用方只说 danger（超限），由样式表决定
 * 它在浅色 / 深色下分别是什么红。级别未变化时直接返回，避免每轮刷新都重新抛光。
 *
 * @param[in] widget QWidget *，目标控件；允许为 nullptr（集中兜底，调用方免判空）。
 * @param[in] level const QString &，语义级别：`"ok"` / `"warn"` / `"danger"`，
 *                  传空串表示"清除级别、回到默认色"。
 * @return 无。
 * @note 动态属性变化不会自动触发样式重算，因此必须显式 unpolish + polish 一次。
 * @note 本函数是"配色随语义变化"的唯一入口：任何控件都不应再直接调用 setStyleSheet
 *       写死颜色，否则主题切换时它不会跟着变。
 */
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

    // 写入动态属性：QSS 里的 [level="..."] 选择器读的就是这个值。
    widget->setProperty(kLevelProperty, level);

    // 动态属性变化不会自动触发样式重算，必须显式 unpolish + polish 一次，
    // 否则 QSS 里的 [level="..."] 选择器不会重新匹配。
    if (QStyle *style = widget->style()) {
        // unpolish 让控件丢掉旧的样式缓存（包括旧的属性选择器匹配结果）。
        style->unpolish(widget);
        // polish 按当前属性值重新计算样式，颜色在这一步被换成新主题的色值。
        style->polish(widget);
    }
    // 主动请求重绘：抛光只更新样式参数，不保证立刻可见。
    widget->update();
}
} // namespace AppTheme

// ===========================================================================
//  ③ 应用与广播
//
//  广播的意义：本模块只能驱动"Qt 自己能着色"的部分。自绘控件（分段进度条）与
//  Win32 侧呈现（托盘图标位图、任务栏角标）需要自己重画，因此用 Notifier 通知它们
//  "配色变了，请按新主题重绘"。这也是本项目在深色主题下仍能保证托盘图标配色一致的
//  原因：托盘图标由主窗口在收到 changed() 后重新生成。
// ===========================================================================

namespace AppTheme
{
/**
 * @brief 构造广播器；只为承载 changed() 信号，无任何附加状态。
 * @param[in] parent QObject *，父对象；默认 nullptr 表示由静态实例自行管理生命周期。
 * @return 无。
 */
Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
}

/**
 * @brief 对外发出 changed() 信号（供本模块内部与测试调用）。
 * @return 无。
 * @note 单独包一层而不是让调用方 emit：notify() 是普通函数，可被非 QObject 上下文
 *       调用，也便于将来在广播前后插入日志而无需改所有调用点。
 */
void Notifier::notify()
{
    emit changed();
}

/**
 * @brief 取全局唯一的广播器实例。
 * @return Notifier *，进程内长期有效，永不返回 nullptr。
 * @note 函数内静态对象：首次使用时构造，随进程结束析构，不引入退出顺序问题；
 *       界面层可放心在任意时刻连接它的信号。
 */
Notifier *notifier()
{
    // 函数内静态对象：首次使用时构造，随进程结束析构，不引入退出顺序问题。
    static Notifier kNotifier;
    return &kNotifier;
}

/**
 * @brief 取当前生效的主题模式（含命令行临时覆盖）。
 * @return AppConfig::ThemeMode，当前主题模式。
 * @note 命令行覆盖优先于配置文件：它的语义就是"本次运行按我说的来"，
 *       因此在 s_hasOverride 为真时完全不读配置。
 */
AppConfig::ThemeMode mode()
{
    // 命令行覆盖优先于配置文件：它的语义就是"本次运行按我说的来"。
    return s_hasOverride ? s_overrideMode : AppConfig::themeMode();
}

/**
 * @brief 判断当前是否应使用深色配色。
 * @return bool，true 表示深色。
 * @note 强制模式直接给出结论，不依赖平台是否真的响应了 setColorScheme()：若平台
 *       忽略了请求，colorScheme() 仍会返回系统值，此时按系统值倒推会得到与用户
 *       选择相反的结果，因此必须先按模式分支。
 * @note 「系统未提供配色信息」时按浅色处理：浅色是"看不清"风险更低的一侧。
 */
bool isDark()
{
    // 强制模式直接给出结论，不依赖平台是否真的响应了 setColorScheme()：
    // 若平台忽略了请求，colorScheme() 仍会返回系统值，此时按系统值倒推会得到
    // 与用户选择相反的结果，因此这里必须先按模式分支。
    switch (mode()) {
    case AppConfig::ThemeMode::light:
        // 用户明确要求浅色：不查询系统，直接返回。
        return false;
    case AppConfig::ThemeMode::dark:
        // 用户明确要求深色：同上。
        return true;
    case AppConfig::ThemeMode::system:
    default:
        // 跟随系统：落到下面按平台配色提示判断。
        break;
    }

    // 查询平台配色提示；Qt 只在真正知道系统配色时才返回 Dark。
    const QStyleHints *hints = QGuiApplication::styleHints();
    if (hints != nullptr && hints->colorScheme() == Qt::ColorScheme::Dark)
        return true;
    // 系统未提供配色信息（Unknown）或查询失败时按浅色处理：
    // 浅色是"看不清"风险更低的一侧，宁可少一次深色适配，也不要把浅色文字放在深底上。
    return false;
}

/**
 * @brief 取当前主题的调色板。
 * @return const Palette &，浅色或深色调色板（只读引用，进程内长期有效）。
 * @note 这是全工程取色的唯一入口：界面层不允许再出现字面颜色值。
 */
const Palette &palette()
{
    // 深色与浅色互为二选一；两个调色板都是函数内静态对象，构造一次后长期复用。
    return isDark() ? darkPalette() : lightPalette();
}

/**
 * @brief 初始化主题模块：挂接系统配色变化并应用一次当前主题。
 *
 * 调用时机：main() 中创建 QApplication 之后、创建主窗口之前，保证窗口首帧就是
 * 正确主题，避免"先浅色闪一下再变深色"。
 *
 * @param[in] app QApplication *，应用对象；本模块内部改用 QCoreApplication::instance()
 *                取应用对象（以支持在任意位置调用），该参数仅用于语义完整性。
 * @return 无。
 * @note 系统配色变化的回调里同时做两件事：重新应用（applyInternal）与广播（notify），
 *       前者更新 Qt 侧配色，后者让自绘控件重画。
 * @note 重复调用是安全的：applyInternal 幂等，信号连接由 Qt 自身去重（同一对象+槽）。
 */
void initialize(QApplication *app)
{
    // 挂接系统配色变化：仅在"跟随系统"模式下需要重新应用，但为了兼容平台主题
    // 在系统切换后自行刷新调色板的情况，这里统一重应用一次（代价极低）。
    if (QStyleHints *hints = QGuiApplication::styleHints()) {
        QObject::connect(hints, &QStyleHints::colorSchemeChanged, notifier(),
                         [](Qt::ColorScheme) {
                             // 先落地：把新配色写进 QPalette 与样式表。
                             applyInternal();
                             // 再广播：让自绘控件与 Win32 侧呈现按新配色重画。
                             notifier()->notify();
                         });
    }

    // 启动即应用一次：保证窗口出现时就是正确主题。
    applyInternal();

    // 传入的 app 仅用于语义完整性（本模块内部通过 QCoreApplication::instance()
    // 取应用对象，以支持在任意位置调用），此处显式标记为已使用。
    Q_UNUSED(app)

    // 广播一次：初始化发生在窗口创建之前，因此这里的广播主要服务于"先建主题、
    // 后建窗口"的调用顺序下仍需按主题初值重绘的自绘控件（幂等，重复无害）。
    notifier()->notify();
}

/**
 * @brief 切换主题模式并立即生效（用户显式选择入口）。
 *
 * 顺序刻意设计为"先落盘、再应用、后广播"：即便应用过程中出现任何异常分支，
 * 磁盘上的配置也已经与用户选择一致，下次启动不会回到旧主题。
 *
 * @param[in] mode AppConfig::ThemeMode，目标模式（light / dark / system）。
 * @return 无。
 * @note 本函数会清除命令行覆盖标记：用户显式选择优先于 --theme 这类一次性覆盖，
 *       否则用户改完设置后行为仍被命令行压着，属于"改了没反应"的典型困惑。
 */
void setMode(AppConfig::ThemeMode mode)
{
    // 用户显式选择优先于命令行覆盖：清掉覆盖标记，让配置文件重新成为唯一事实来源。
    s_hasOverride = false;
    // 先落盘再应用：即使随后应用过程中出现任何异常分支，配置也已经与用户选择一致。
    AppConfig::setThemeMode(mode);
    // 把新主题落到 QPalette 与全局样式表上。
    applyInternal();
    // 通知自绘控件与托盘/任务栏按新配色重绘。
    notifier()->notify();
}

/**
 * @brief 按命令行参数临时覆盖主题模式（只影响本次运行）。
 *
 * 与 setMode() 的关键差别：本函数**不写配置文件**，因为命令行覆盖的语义就是
 * "只影响本次运行"；同时它把覆盖标记置真，使 mode() 忽略持久化配置。
 *
 * @param[in] mode AppConfig::ThemeMode，本次运行要使用的模式。
 * @return 无。
 * @note 任何一次"用户在设置里保存主题"都会清除该覆盖，回到配置文件驱动。
 */
void overrideMode(AppConfig::ThemeMode mode)
{
    // 打开覆盖开关：mode() 之后会优先返回 s_overrideMode。
    s_hasOverride = true;
    // 记录目标模式；此值不落盘。
    s_overrideMode = mode;
    // 不写配置文件：覆盖的语义就是"只影响本次运行"。
    applyInternal();
    // 广播：自绘控件按临时主题重绘（退出程序后自然失效）。
    notifier()->notify();
}
} // namespace AppTheme
