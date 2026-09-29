// ---------------------------------------------------------------------------
//  AppTheme.h —— 应用外观主题（浅色 / 深色 / 跟随系统）的统一入口
//
//  背景：早期版本在全局样式表与各处着色代码里写死了一批"为浅色背景挑选"的颜色
//  （例如 #C0392B 暗红、#2E7D32 暗绿、#8A5300 暗琥珀）。当系统主题切换为深色时，
//  这些深色文字落在深色背景上，对比度骤降，表现为"有一部分字看不清"。
//
//  本模块把"当前生效的外观"收敛为唯一事实来源：
//    · Palette 集中定义两套语义色，界面各处只引用语义名，不再出现写死的色值；
//    · 全局样式表（QSS）由 Palette 生成，只在本模块维护一份；
//    · 三档模式中的「跟随系统」通过 QStyleHints::colorScheme() 判定，并监听
//      colorSchemeChanged 信号，使系统切换主题时界面即时跟随。
//
//  设计约定：
//    · 所有接口不抛异常；指针参数一律在使用前判空；
//    · 只依赖 QtCore / QtGui / QtWidgets，不引入任何业务模块（高内聚低耦合）；
//    · 主题变更通过 Notifier::changed() 广播，自绘控件据此请求重绘。
// ---------------------------------------------------------------------------
#pragma once

#include "AppConfig.h"

#include <QColor>
#include <QObject>
#include <QString>

class QApplication;
class QPalette;
class QWidget;

namespace AppTheme
{
/**
 * @brief 一套语义调色板：界面的全部颜色都从这里取用。
 *
 * 字段按"用途"而非"色相"命名，因此替换主题时无需理解任何一处界面的具体语义；
 * 例如把 success 换成青色，所有"正常 / 成功"语义的位置会一起改变。
 */
struct Palette
{
    bool   dark = false;          ///< 是否为深色系，供需要做方向性判断的代码使用
    QColor window;                ///< 窗口底色
    QColor base;                  ///< 卡片 / 输入框 / 列表底色
    QColor alternateBase;         ///< 隔行底色
    QColor button;                ///< 按钮底色
    QColor text;                  ///< 主文本
    QColor mutedText;             ///< 次要文本（账号、状态、卡片标题、提示等）
    QColor disabledText;          ///< 禁用态文本
    QColor border;                ///< 卡片描边与分隔线
    QColor success;               ///< 正常 / 成功（绿）
    QColor warning;               ///< 告警（琥珀）
    QColor danger;                ///< 危险 / 失败（红）
    QColor highlight;             ///< 选中项底色
    QColor highlightedText;       ///< 选中项文字
    QColor bannerWarnBg;          ///< 警示横幅底色
    QColor bannerWarnFg;          ///< 警示横幅文字
    QColor bannerWarnBorder;      ///< 警示横幅描边
    QColor bannerOkBg;            ///< 成功横幅底色
    QColor bannerOkFg;            ///< 成功横幅文字
    QColor bannerOkBorder;        ///< 成功横幅描边
    QColor barIdle;               ///< 分段进度条熄灭格的颜色
};

/**
 * @brief 主题变更广播器：需要随主题重绘的自定义控件应连接 changed()。
 *
 * 之所以单独做一个只有信号的对象，而不是让使用者之间互相调用，是为了让
 * "谁改了主题"与"谁需要重绘"彻底解耦：设置对话框改主题时不需要知道主窗口、
 * 进度条或将来新增的任何控件。
 */
class Notifier : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造广播器；parent 通常为 nullptr（本对象是进程内单例）。
     * @param[in] parent QObject *，父对象指针；允许为 nullptr。
     * @return 无。
     */
    explicit Notifier(QObject *parent = nullptr);

    /**
     * @brief 广播一次主题变更。
     *
     * 由 setMode() 与系统配色变化处理器调用；做成公开成员函数而非直接 emit，
     * 是为了让"信号只能由本类发出"这一约束在代码层面可见。
     *
     * @return 无。
     */
    void notify();

signals:
    /// 生效主题发生变化时发出（用户切换模式，或系统配色变化导致跟随结果改变）
    void changed();
};

/**
 * @brief 取进程内唯一的主题广播器。
 * @return Notifier *，永不为 nullptr；所有权归模块，调用方不得释放。
 */
Notifier *notifier();

/**
 * @brief 初始化主题：应用当前配置，并挂接系统配色变化。
 *
 * 必须在 QCoreApplication 的组织名 / 应用名设定之后调用（主题模式存放于配置文件，
 * 而配置文件路径由组织名与应用名共同决定），且在创建任何窗口之前调用，
 * 否则界面会先按默认配色绘制一帧再被重绘成目标主题，出现可见闪烁。
 *
 * @param[in] app QApplication *，应用对象；允许为 nullptr，此时只挂接系统信号
 *                而不应用调色板（无 GUI 场景，例如未来的纯控制台用法）。
 * @return 无。
 * @note 本函数可重复调用：重复调用等价于按当前配置重新应用一次。
 */
void initialize(QApplication *app);

/**
 * @brief 切换主题模式并立即生效（同时写入配置文件）。
 *
 * 写入配置是刻意的：主题属于"改完就该记住"的偏好，若等用户点「保存」才落盘，
 * 那么"跟随系统 → 深色"这类临时预览会在取消后留下界面与配置不一致的错觉。
 * 调用方若需要"取消时还原"，应在退出前把原模式再设回来（设置对话框即如此）。
 *
 * @param[in] mode AppConfig::ThemeMode，目标模式（light / dark / system）。
 * @return 无。
 * @note 生效后会通过 notifier()->changed() 通知所有订阅者。
 */
void setMode(AppConfig::ThemeMode mode);

/**
 * @brief 设置「仅本次运行生效」的主题覆盖（不写入配置文件）。
 *
 * 用途与命令行里的 --key / --base-url 完全一致：临时指定本次运行的外观，
 * 既不污染用户的持久化配置，也让自动化脚本（例如逐主题截图对照）能够稳定地
 * 在指定主题下启动程序。
 *
 * @param[in] mode AppConfig::ThemeMode，本次运行要强制使用的模式。
 * @return 无。
 * @note 覆盖一经设置便持续到进程结束；若用户随后在设置对话框里显式切换主题，
 *       以用户操作为准（setMode() 会清除覆盖标记）。
 */
void overrideMode(AppConfig::ThemeMode mode);

/**
 * @brief 读取配置中的主题模式。
 * @return AppConfig::ThemeMode，与 AppConfig::themeMode() 完全一致。
 */
AppConfig::ThemeMode mode();

/**
 * @brief 判定「最终生效的」外观是否为深色。
 *
 * 强制浅色 / 强制深色直接返回对应结果；「跟随系统」则查询当前的颜色方案
 * （QStyleHints::colorScheme()），系统未提供该信息时按浅色处理。
 *
 * @return bool，true 表示当前应以深色调色板绘制。
 * @note 判定只读取"模式"与"系统配色"，不依赖窗口是否已创建，可在任意时刻调用。
 */
bool isDark();

/**
 * @brief 取当前生效的调色板。
 * @return const Palette &，按 isDark() 结果返回浅色或深色那套；引用长期有效。
 */
const Palette &palette();

/**
 * @brief 依据当前调色板构造 QPalette，供标准控件（按钮、下拉框、滚动条等）取色。
 *
 * 之所以除了样式表还要设置 QPalette：样式表只覆盖被选择器命中的控件，而按钮、
 * 文本框、菜单等由样式自己绘制的部件是从 QPalette 取色的；两者缺一都会出现
 * "一半深色一半浅色"的割裂外观。
 *
 * @return QPalette，包含常规态与禁用态的全部角色。
 * @note 返回值按值传递，调用方拿到的是独立副本。
 */
QPalette buildPalette();

/**
 * @brief 生成全局样式表（QSS）。
 *
 * 样式表由当前调色板逐项替换占位符得到，因此主题切换时只需重新下发一次样式表，
 * 已存在的控件会被 Qt 自动重新抛光（re-polish），无需逐个手动刷新。
 *
 * @return QString，可直接交给 QApplication::setStyleSheet() 的完整样式表。
 */
QString styleSheet();

/**
 * @brief 设置控件的语义级别，使其颜色由样式表按级别决定。
 *
 * 级别写入动态属性 "level"，取值约定："ok"、"warn"、"danger"，空串表示回到默认色。
 * 之所以用动态属性而不是每次 setStyleSheet()，是因为样式表的重新下发会统一重算
 * 所有选择器，主题切换时这些控件会自动换成新主题的颜色，不会残留旧色值。
 *
 * @param[in] widget QWidget *，目标控件；为 nullptr 时本函数直接返回。
 * @param[in] level QString，级别名；空串表示清除级别、使用默认颜色。
 * @return 无。
 * @note 级别未变化时不触发重绘，因此可在每次数据刷新时无脑调用。
 */
void setLevel(QWidget *widget, const QString &level);
} // namespace AppTheme
