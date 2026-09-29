// ---------------------------------------------------------------------------
//  SettingsDialog.h —— 设置对话框的声明：配置 API Key、API 地址与自动刷新间隔
//
//  本对话框是本应用唯一的配置入口，字段与持久化一一对应：API Key、API 地址、
//  自动刷新间隔。其中 API Key 默认留空，由用户自行填写（或点击「从 CLI 导入」
//  按钮，从 ~/.commandcode/auth.json 里显式读取），界面不会替用户猜测或预填。
// ---------------------------------------------------------------------------
#pragma once

// AppConfig::StatusMetric 作为按值返回的类型出现在本头文件的方法签名中，
// 因此必须包含完整定义，而不能仅做前置声明。
#include "AppConfig.h"

#include <QDialog>
#include <QUrl>

// 以下六个类仅以指针成员的形式出现，故使用前置声明（forward declaration）替代
// 头文件包含，以减少编译期依赖、缩短编译时间并降低模块间耦合。
class QLineEdit;
class QSpinBox;
class QLabel;
class QPushButton;
// 桌面集成区块新增的两种控件：复选框与下拉框。
class QCheckBox;
class QComboBox;

// 设计要点（补充说明）：
//    1. 对话框只负责「采集与校验用户输入」，读写配置的实际动作全部委托给
//       AppConfig 静态接口，做到界面与存储解耦；
//    2. 「从 CLI 导入」是显式动作——只有用户点击时才去读 auth.json，避免程序
//       启动阶段静默读取磁盘上的凭据文件；
//    3. 「测试连接」复用 CommandCodeApi 而非自行发请求，连接结果通过
//       connectionTested 信号回到槽函数，界面据此更新提示文案与颜色；
//    4. 点击保存（accept）时才把三个字段写回配置，取消则不做任何持久化。

/**
 * @brief 设置对话框：采集 API Key、API 地址与自动刷新间隔三项配置。
 *
 * 典型用法是先 exec() 模态弹出，返回 QDialog::Accepted 后由调用方依次读取
 * apiKey()、baseUrl()、refreshSeconds()；各 getter 只读控件当前文本，不做落盘。
 *
 * @note 对话框内部持有一个专供连通性测试的 CommandCodeApi 实例，以 this 为父对象，
 *       因此其生命周期与对话框绑定，无需调用方手动释放。
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief 构造设置对话框：先搭建界面、回填当前配置，再接通测试信号。
     *
     * 构造顺序不可随意调换——必须先 buildUi() 创建出各控件，loadFromConfig() 才有
     * 对象可回填；最后创建测试用 API 对象并连接 connectionTested 信号，保证任何
     * 时刻点击「测试连接」都能拿到结果回调。
     *
     * @param[in] parent QWidget *，父窗口指针；可为 nullptr，此时对话框无父窗口，
     *                   在部分平台上不会继承父窗口的图标与居中行为（输入方向）。
     * @return 无。
     * @note 构造过程中不读取磁盘上的凭据文件，API Key 仅从已保存配置回填。
     */
    explicit SettingsDialog(QWidget *parent = nullptr);

    /**
     * @brief 读取用户填写的 API Key。
     *
     * 返回值已做首尾空白裁剪；之所以在 getter 里裁剪而不是在保存时裁剪，是为了让
     * 「测试连接」与「保存」两条路径拿到完全一致的字符串，避免出现「测试通过、
     * 保存后却因隐藏空格而鉴权失败」这类不一致问题。
     *
     * @return QString，裁剪后的 API Key 文本；用户未填写时为空字符串。
     */
    QString apiKey() const;

    /**
     * @brief 读取用户填写的 API 地址，并归一化为可直接使用的 QUrl。
     *
     * 归一化规则有两条：① 文本为空时回退到默认地址，保证不会返回无效 URL；
     * ② 去掉末尾所有 '/'，避免与各接口路径拼接时出现双斜杠。
     *
     * @return QUrl，归一化后的 API 基础地址。
     * @note 本函数只做字符串层面的整理，不校验主机是否可达，连通性由
     *       testConnection() 负责。
     */
    QUrl baseUrl() const;

    /**
     * @brief 读取自动刷新间隔。
     *
     * 取值直接来自微调框当前值，0 表示「关闭自动刷新」，此时只能手动点击刷新；
     * 取值范围的约束由控件自身的 setRange() 保证，本函数不再重复校验。
     *
     * @return int，自动刷新间隔秒数，取值范围 0 ~ 3600。
     */
    int refreshSeconds() const;

    /**
     * @brief 读取「在通知区域（系统托盘）显示图标」开关。
     * @return bool，true 表示用户勾选开启。
     */
    bool trayEnabled() const;

    /**
     * @brief 读取「在任务栏按钮上显示进度与角标」开关。
     * @return bool，true 表示用户勾选开启。
     */
    bool taskbarBadgeEnabled() const;

    /**
     * @brief 读取「关闭窗口时最小化到托盘」开关。
     * @return bool，true 表示关闭窗口后隐藏到托盘而不是退出进程。
     */
    bool closeToTray() const;

    /**
     * @brief 读取缩略信息所选指标（托盘文字与任务栏进度都使用它）。
     * @return AppConfig::StatusMetric，下拉框当前选中项对应的枚举值。
     */
    AppConfig::StatusMetric statusMetric() const;

    /**
     * @brief 读取用户选择的界面主题模式。
     *
     * 与指标下拉框同一约定：控件里存的是枚举整数值，取不到时回退为
     * 「跟随系统」，与 AppConfig 的默认值保持一致。
     *
     * @return AppConfig::ThemeMode，下拉框当前选中项对应的枚举值。
     */
    AppConfig::ThemeMode themeMode() const;

private slots:
    /**
     * @brief 槽函数：响应用户点击「从 CLI 导入」按钮。
     *
     * 只有用户显式点击时才会去读取 ~/.commandcode/auth.json，符合「不静默触碰
     * 磁盘凭据」的约定；导入结果（成功或失败原因）通过状态标签反馈给用户。
     *
     * @return 无。
     * @note 该槽挂在按钮的 clicked 信号上，导入失败的提示为红色、成功为绿色。
     */
    void importFromCli();

    /**
     * @brief 槽函数：响应用户点击「测试连接」按钮。
     *
     * 先做本地前置校验（API Key 非空），校验不过直接给出红色提示并不发请求；
     * 通过后才把当前界面上的地址与密钥写入测试用 API 对象并发起请求。
     *
     * @return 无。
     * @note 请求期间会禁用测试按钮，防止用户连点造成并发请求。
     */
    void testConnection();

    /**
     * @brief 槽函数：接收测试用 API 对象的连接测试结果。
     *
     * 由 CommandCodeApi::connectionTested 信号触发；无论成功与否都会重新启用测试
     * 按钮，并把结果文案与配色写入状态标签。
     *
     * @param[in] ok bool，测试是否成功；true 表示连通，false 表示失败或超时。
     * @param[in] message QString，展示给用户的提示文案，成功时为服务端信息，
     *                    失败时为具体错误原因。
     * @return 无。
     * @note 该槽必须在测试请求发出前完成连接，否则结果无法回填到界面。
     */
    void onConnectionTested(bool ok, const QString &message);

    /**
     * @brief 覆写接受（保存）行为：先把配置落盘，再走基类默认流程关闭对话框。
     *
     * 保存动作严格依赖三个 getter，因此 apiKey() 的裁剪、baseUrl() 的归一化都会
     * 在写入前生效；落盘完成后再调用 QDialog::accept() 关闭并返回 Accepted。
     *
     * @return 无。
     * @note 若后续需要在保存前做拦截校验，应在此处提前 return 而不调用基类实现；
     *       当前实现无失败分支，故总是关闭对话框。
     */
    void accept() override;

    /**
     * @brief 覆写取消行为：先还原进入本对话框时的主题，再走基类默认流程关闭。
     *
     * 主题是"选中即生效"的（见 onThemeModeChanged()），因此用户按「取消」时必须
     * 把外观与配置一并还原，否则会出现"点了取消、界面却变了"的错觉。
     *
     * @return 无。
     * @note 关闭窗口（点 ×）同样会走到这里，行为与「取消」一致。
     */
    void reject() override;

    /**
     * @brief 槽函数：响应用户切换主题下拉框，立即预览并生效。
     *
     * 主题属于"所见即所得"的设置：若等到点「保存」才生效，用户无法判断选中的
     * 是哪一档。因此这里即时调用 AppTheme::setMode()，它同时写配置并广播变更。
     *
     * @return 无。
     * @note 该槽在 loadFromConfig() 中回填完成后才连接，避免初始化时的
     *       setCurrentIndex() 被误当作一次用户操作而触发多余落盘。
     */
    void onThemeModeChanged();

private:
    /**
     * @brief 构建对话框界面：布局、各输入控件、提示标签与底部按钮区。
     *
     * 只负责创建与排布控件，不读取也不写入任何配置，从而与 loadFromConfig() 的
     * 数据回填职责严格分离，便于单独复用或测试界面构建过程。
     *
     * @return 无。
     * @note 界面构建期间会连接按钮信号与对应槽函数，因此本函数应在构造早期调用。
     */
    void buildUi();

    /**
     * @brief 从已持久化的配置回填各控件初值。
     *
     * 依次回填 API Key、API 地址与刷新间隔；必须在 buildUi() 之后调用，否则控件
     * 尚未创建会导致空指针解引用。
     *
     * @return 无。
     * @note API Key 默认为空字符串，界面据此呈现「未配置」状态，属于预期行为。
     */
    void loadFromConfig();

    QLineEdit   *m_keyEdit = nullptr;       ///< API Key 输入框，回显模式为密码式。
    QLineEdit   *m_baseUrlEdit = nullptr;   ///< API 地址输入框，可直接编辑。
    QSpinBox    *m_refreshSpin = nullptr;   ///< 自动刷新间隔（秒），0 表示关闭。
    QLabel      *m_hintLabel = nullptr;     ///< 常驻提示文案，说明密钥只存本机。
    QLabel      *m_statusLabel = nullptr;   ///< 动态状态标签，显示导入 / 测试结果。
    QPushButton *m_testButton = nullptr;    ///< 「测试连接」按钮，请求期间被禁用。
    QPushButton *m_importButton = nullptr;  ///< 「从 CLI 导入」按钮，点击才读凭据。

    // ---- 桌面集成（通知区域 / 任务栏）----
    QCheckBox   *m_trayCheck = nullptr;       ///< 「在通知区域显示图标」复选框。
    QCheckBox   *m_taskbarCheck = nullptr;    ///< 「在任务栏按钮上显示进度与角标」复选框。
    QCheckBox   *m_closeToTrayCheck = nullptr;///< 「关闭窗口时最小化到托盘」复选框。
    QComboBox   *m_metricCombo = nullptr;     ///< 「缩略信息显示内容」下拉框。

    // ---- 外观（主题） ----
    QComboBox   *m_themeCombo = nullptr;      ///< 「主题颜色」下拉框，三档选择。
    /// 进入对话框时的主题模式；供 reject() 在「取消」时还原外观与配置。
    AppConfig::ThemeMode m_initialThemeMode = AppConfig::ThemeMode::system;

    class CommandCodeApi *m_testApi = nullptr;  ///< 测试专用 API 对象，父对象为 this。
};
