// ---------------------------------------------------------------------------
//  SettingsDialog.cpp —— 设置对话框的实现：界面搭建、配置回填与三项动作的落地
//
//  本文件实现 SettingsDialog.h 声明的全部成员，职责可归为界面搭建（buildUi）、
//  配置回填（loadFromConfig）、配置读取（apiKey / baseUrl / refreshSeconds）
//  与用户动作（importFromCli / testConnection / accept）四类。
//
//  详细实现约定见下方「设计要点（补充说明）」注释块。
// ---------------------------------------------------------------------------

// 设计要点（补充说明）：
//   · 界面与存储解耦：所有落盘与读取都委托给 AppConfig，本文件不直接操作文件，
//     因此调整存储格式时无需改动界面代码；
//   · 「从 CLI 导入」是显式动作，只有用户点击对应按钮时才去读 auth.json，避免
//     程序启动阶段静默触碰磁盘上的凭据文件；
//   · 「测试连接」复用 CommandCodeApi，结果经 connectionTested 信号异步回到
//     onConnectionTested 槽，界面在此期间禁用按钮以防重复请求；
//   · 状态标签用"语义级别"着色：失败标 danger、成功标 ok、进行中清空级别以回到
//     主题默认色；具体色值由 AppTheme 按当前主题给出，界面代码不写死颜色。

// 本类的声明：同时引入 QDialog 基类、QUrl 返回值类型与四个控件的前置声明。
#include "SettingsDialog.h"

// 外观主题：设置对话框直接引用主题模块，使预览即时生效。
#include "AppTheme.h"
// AppConfig 提供配置读写、CLI 凭据导入与路径查询等静态接口，是本文件的数据层。
#include "AppConfig.h"
// CommandCodeApi 提供连通性测试能力，用于「测试连接」按钮背后的实际请求。
#include "CommandCodeApi.h"

// 对话框底部按钮区：提供标准的「保存 / 取消」按钮及自定义动作按钮槽位。
#include <QDialogButtonBox>
// 复选框：桌面集成区块的三个独立开关。
#include <QCheckBox>
// 下拉框：选择托盘图标与任务栏进度条展示哪一项指标。
#include <QComboBox>
// 分组框：把「桌面集成」相关的四项设置与连接配置在视觉上分开。
#include <QGroupBox>
// 用于把路径转换为当前平台的原生分隔符，提升提示文案的可读性。
#include <QDir>
// 与 JSON 解析相关的头文件由 AppConfig 内部使用，此处保留包含以维持原样。
#include <QFile>
// 表单布局：左侧自动对齐标签、右侧放置输入控件，符合桌面端设置页惯例。
#include <QFormLayout>
// JSON 文档 / 对象：供上层或后续扩展在对话框内直接解析响应时使用。
#include <QJsonDocument>
#include <QJsonObject>
// 水平布局：用于把输入框与其右侧的按钮排成一行。
#include <QHBoxLayout>
// 标签控件：分别用于常驻提示（m_hintLabel）与动态状态（m_statusLabel）。
#include <QLabel>
// 单行输入框：承载 API Key 与 API 地址两项文本配置。
#include <QLineEdit>
// 按钮控件：用于「测试连接」「从 CLI 导入」与「显示」三处交互。
#include <QPushButton>
// 微调框：承载自动刷新间隔（秒），并支持 0 值的特殊显示文案。
#include <QSpinBox>
// 垂直布局：对话框根布局，自上而下依次摆放表单、提示、状态与按钮区。
#include <QVBoxLayout>

// ===========================================================================
//  构造与信号连接
// ===========================================================================

/**
 * @brief 构造设置对话框：设置窗口属性、搭建界面、回填配置并接通测试信号。
 *
 * 执行顺序为：① 设定标题与最小宽度；② 搭建界面；③ 回填已保存的配置；
 * ④ 创建测试专用 API 对象并连接其 connectionTested 信号。
 * 其中 ② 必须先于 ③，否则回填时控件尚不存在；④ 放在构造末尾，但其连接必须在
 * 用户可能点击「测试连接」之前完成，构造期间用户无法交互，故天然满足。
 *
 * @param[in] parent QWidget *，父窗口指针；可为 nullptr，此时对话框为独立顶层
 *                   窗口，不继承父窗口的图标与居中位置（输入方向）。
 * @return 无。
 * @note 测试用 API 对象以 this 为父对象，随对话框一同析构；构造过程不读取
 *       ~/.commandcode/auth.json，API Key 仅来自已保存配置。
 */
SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    // 窗口标题；使用 tr() 包裹以便后续本地化。
    setWindowTitle(tr("设置"));
    // 设定最小宽度，保证较长的提示文案与「API Key + 三个按钮」行不会挤成两行。
    setMinimumWidth(520);
    // 先建界面：创建所有控件并接好按钮信号，后续回填才有对象可用。
    buildUi();
    // 再回填：把已保存的 API Key / 地址 / 刷新间隔灌入控件作为初值。
    loadFromConfig();

    // 测试专用 API 对象；以 this 为父对象，生命周期与对话框一致，无需手动 delete。
    m_testApi = new CommandCodeApi(this);
    // 把异步测试结果信号连到本类槽函数：请求返回后由槽负责更新提示与按钮状态。
    // 该连接必须在用户可能触发测试之前建立，否则测试结果将无处回填。
    connect(m_testApi, &CommandCodeApi::connectionTested,
            this, &SettingsDialog::onConnectionTested);
}

// ===========================================================================
//  界面搭建
// ===========================================================================

/**
 * @brief 构建对话框界面：根布局 → 表单 → 提示与状态标签 → 底部按钮区。
 *
 * 本函数只做「创建 + 排布 + 接线」，不读取也不写入任何配置，从而与
 * loadFromConfig() 的职责严格分离。界面结构自上而下为：
 *   · 表单区：API Key（输入框 + 导入按钮 + 显示开关）、API 地址、自动刷新间隔；
 *   · 提示区：说明密钥只存本机、不上传第三方（次要配色）；
 *   · 状态区：动态显示导入结果或连接测试结果（默认无颜色）；
 *   · 按钮区：标准的「保存 / 取消」，并在其中插入「测试连接」动作按钮。
 *
 * @return 无。
 * @note 所有按钮信号都在本函数内完成连接，因此本函数应在构造早期调用一次且仅
 *       调用一次；重复调用会造成控件与信号连接的重复创建。
 */
void SettingsDialog::buildUi()
{
    // 根布局直接以 this 为父，成为对话框的主布局；垂直排布各区块。
    auto *root = new QVBoxLayout(this);
    // 区块间距稍大，使表单、提示与按钮区在视觉上形成清晰的分组。
    root->setSpacing(12);
    // 四周留出统一内边距，避免控件紧贴窗口边缘。
    root->setContentsMargins(18, 18, 18, 18);

    // 表单布局负责「标签右对齐、控件占满右侧」的成对排布，最贴合设置页惯例。
    auto *form = new QFormLayout;
    // 行间距小于区块间距，让三行配置项在视觉上归属于同一组。
    form->setSpacing(10);
    // 标签右对齐并垂直居中，使长短不一的标签与输入框基线对齐、美观整齐。
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // ---- API Key ----
    // API Key 一行由「输入框 + 从 CLI 导入 + 显示」三个控件横向组成。
    auto *keyRow = new QHBoxLayout;
    m_keyEdit = new QLineEdit;
    // 默认以密码方式回显，避免密钥在屏幕或截图中被直接读出。
    m_keyEdit->setEchoMode(QLineEdit::Password);
    // 占位文案说明「留空即未配置」以及获取密钥的官方入口，降低用户疑惑。
    m_keyEdit->setPlaceholderText(tr("留空即未配置，在 https://commandcode.ai 的 API Keys 页面获取"));
    // 启用一键清空按钮，方便用户快速抹掉已保存的旧密钥。
    m_keyEdit->setClearButtonEnabled(true);
    // 伸缩因子给 1，使输入框占据本行剩余的全部宽度，按钮只占内容宽度。
    keyRow->addWidget(m_keyEdit, 1);

    // 「从 CLI 导入」按钮：把 Command Code CLI 已登录的密钥一键搬进本应用。
    m_importButton = new QPushButton(tr("从 CLI 导入"));
    // 悬停提示直接显示将要读取的具体路径，让「会读哪个文件」对用户透明。
    m_importButton->setToolTip(tr("读取 %1 中的 apiKey").arg(AppConfig::cliAuthFilePath()));
    // 连接点击信号：只有用户显式点击时才真正去读磁盘上的凭据文件。
    connect(m_importButton, &QPushButton::clicked, this, &SettingsDialog::importFromCli);
    keyRow->addWidget(m_importButton);

    // 「显示」按钮做成可切换（checkable）的开关，而非一次性动作。
    auto *showButton = new QPushButton(tr("显示"));
    showButton->setCheckable(true);
    // 切换回显模式：按下时明文显示，弹起时恢复密码式，方便核对密钥是否填错。
    connect(showButton, &QPushButton::toggled, this, [this](bool on) {
        // 直接在切换回调内改回显模式，无需额外成员变量记录当前可见状态。
        m_keyEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    keyRow->addWidget(showButton);

    // 把整行三控件作为一个整体放入表单的「API Key」行。
    form->addRow(tr("API Key"), keyRow);

    // ---- Base URL ----
    // API 地址输入框：默认留空，由占位文案提示官方默认地址。
    m_baseUrlEdit = new QLineEdit;
    // 占位文案给出默认地址，用户不填时 baseUrl() 会回退到同一地址。
    m_baseUrlEdit->setPlaceholderText(QStringLiteral("https://api.commandcode.ai"));
    form->addRow(tr("API 地址"), m_baseUrlEdit);

    // ---- 刷新间隔 ----
    // 自动刷新间隔（秒）：0 具有特殊语义，表示关闭自动刷新。
    m_refreshSpin = new QSpinBox;
    // 上限 3600 秒（1 小时），兼顾低频率轮询需求，同时避免过大数值无意义。
    m_refreshSpin->setRange(0, 3600);
    // 后缀让数值带上单位，用户无需从标签文字推断单位是秒还是毫秒。
    m_refreshSpin->setSuffix(tr(" 秒"));
    // 当数值为最小值 0 时，用「关闭自动刷新」替代枯燥的「0 秒」，语义更直观。
    m_refreshSpin->setSpecialValueText(tr("关闭自动刷新"));
    // 悬停提示补充说明 0 的后果：只能手动点击刷新按钮更新数据。
    m_refreshSpin->setToolTip(tr("0 表示不自动刷新，只能手动点「刷新」"));
    form->addRow(tr("自动刷新"), m_refreshSpin);

    // 表单整体作为一个区块加入根布局。
    root->addLayout(form);

    // ---- 外观：主题颜色选择（浅色 / 深色 / 跟随系统）----
    // 单独成组是因为它影响的是整个程序的观感，与连接配置、桌面集成在语义上无关；
    // 放在连接配置之后、桌面集成之前，符合"先能用、再好用、再锦上添花"的顺序。
    auto *appearanceBox = new QGroupBox(tr("外观"));
    auto *appearanceLayout = new QVBoxLayout(appearanceBox);
    appearanceLayout->setSpacing(8);

    m_themeCombo = new QComboBox;
    // userData 存枚举整数值，与指标下拉框保持同一约定，读取时无需做字符串映射。
    m_themeCombo->addItem(tr("浅色主题"), static_cast<int>(AppConfig::ThemeMode::light));
    m_themeCombo->addItem(tr("深色主题"), static_cast<int>(AppConfig::ThemeMode::dark));
    m_themeCombo->addItem(tr("跟随系统"), static_cast<int>(AppConfig::ThemeMode::system));
    m_themeCombo->setToolTip(tr("「跟随系统」会随 Windows 的浅色 / 深色设置自动切换"));
    auto *themeRow = new QHBoxLayout;
    themeRow->addWidget(new QLabel(tr("主题颜色")));
    themeRow->addWidget(m_themeCombo, 1);
    appearanceLayout->addLayout(themeRow);
    root->addWidget(appearanceBox);

    // ---- 桌面集成：托盘与任务栏缩略信息 ----
    // 单独成组是因为这四项都属于「把用量展示到窗口之外」的同一类设置，
    // 与上面的连接配置在语义上互不相干，分组后用户更容易找到。
    auto *desktopBox = new QGroupBox(tr("桌面集成（缩略信息）"));
    auto *desktopLayout = new QVBoxLayout(desktopBox);
    desktopLayout->setSpacing(8);

    // 指标选择：决定托盘图标里的数字与任务栏进度条展示哪一项限制。
    m_metricCombo = new QComboBox;
    // userData 直接存枚举的整数值，读取时无需再做字符串到枚举的映射。
    m_metricCombo->addItem(tr("5 小时限额占用率"), static_cast<int>(AppConfig::StatusMetric::fiveHour));
    m_metricCombo->addItem(tr("每周限额占用率"), static_cast<int>(AppConfig::StatusMetric::weekly));
    m_metricCombo->addItem(tr("每月额度占用率"), static_cast<int>(AppConfig::StatusMetric::monthly));
    m_metricCombo->addItem(tr("剩余额度数值"), static_cast<int>(AppConfig::StatusMetric::remaining));
    m_metricCombo->setToolTip(tr("托盘图标内的数字与任务栏进度条都按这一项计算"));
    auto *metricRow = new QHBoxLayout;
    metricRow->addWidget(new QLabel(tr("显示内容")));
    metricRow->addWidget(m_metricCombo, 1);
    desktopLayout->addLayout(metricRow);

    // 三个开关彼此独立：用户可以只开托盘、只开任务栏，或两者都开。
    m_trayCheck = new QCheckBox(tr("在通知区域（系统托盘）显示用量图标"));
    m_trayCheck->setToolTip(tr("图标内绘制所选指标的缩略数字，悬停查看完整概要"));
    desktopLayout->addWidget(m_trayCheck);

    m_taskbarCheck = new QCheckBox(tr("在任务栏按钮上显示进度与彩色角标"));
    m_taskbarCheck->setToolTip(tr("需要 Windows 任务栏支持；无桌面会话时自动失效"));
    desktopLayout->addWidget(m_taskbarCheck);

    m_closeToTrayCheck = new QCheckBox(tr("关闭窗口时最小化到托盘（不退出程序）"));
    m_closeToTrayCheck->setToolTip(tr("勾选后需从托盘菜单的「退出」结束程序"));
    desktopLayout->addWidget(m_closeToTrayCheck);

    // 逃生说明：这是本对话框里最重要的一段文字。Windows 11 会把新托盘图标收进溢出区，
    // 用户勾了"关闭到托盘"却找不到图标时，会以为程序卡死；这里提前把两条退路写清楚
    // （再运行一次本程序 / 命令行 --quit），把"关不掉的幽灵进程"变成可自救的状态。
    auto *escapeHint = new QLabel(tr("⚠ Windows 11 默认把新托盘图标收进任务栏溢出区（^）。"
                                     "若关闭窗口后找不到图标：① 再运行一次本程序即可唤回窗口；"
                                     "② 或在命令行执行 <程序> --quit 结束后台进程。"));
    escapeHint->setWordWrap(true);
    // 起 objectName 后颜色由全局样式表的 #escapeHint 规则给出（当前主题的告警色）：
    // 原来写死的 #8A5300 是"浅色底上的深琥珀"，在深色主题下几乎不可见。
    escapeHint->setObjectName(QStringLiteral("escapeHint"));
    desktopLayout->addWidget(escapeHint);

    root->addWidget(desktopBox);

    // 常驻提示：说明密钥的落盘位置与「不外传」的承诺，缓解用户对凭据的顾虑。
    m_hintLabel = new QLabel(tr("提示：API Key 只保存在本机 %1，不会上传到任何第三方。")
                                 .arg(QDir::toNativeSeparators(AppConfig::settingsFilePath())));
    // 允许自动换行，窗口变窄时提示不会撑大对话框宽度。
    m_hintLabel->setWordWrap(true);
    // 起 objectName 后由全局样式表的 #dialogHint 规则统一给色（当前主题的次要文本色），
    // 不再依赖 palette(mid)——该角色在深色主题下与卡片底色的对比度不足，
    // 正是"提示文字看不清"的来源之一。
    m_hintLabel->setObjectName(QStringLiteral("dialogHint"));
    root->addWidget(m_hintLabel);

    // 动态状态标签：显示导入结果、测试进行中提示或测试结果，初始为空。
    m_statusLabel = new QLabel;
    // 起 objectName 后，成功 / 失败的配色由样式表按 level 语义级别给出：
    // 深浅两套主题各有一组对比度足够的绿与红，界面代码不再写死色值。
    m_statusLabel->setObjectName(QStringLiteral("dialogStatus"));
    // 同样允许换行，因为错误信息（如网络异常描述）可能较长。
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    // 底部按钮区采用系统标准按钮盒，自动适配各平台的按钮顺序与命名习惯。
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    // 标准按钮的默认文字（Save/Cancel）来自 Qt 自带的翻译资源；本程序没有内嵌
    // 翻译文件，直接显示英文会与全中文界面格格不入，因此就地改写显示文本。
    // 只改显示文字，不改变按钮的 accept/reject 语义与信号连接。
    buttons->button(QDialogButtonBox::Save)->setText(tr("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    // 「测试连接」以 ActionRole 加入：它不参与接受 / 拒绝语义，仅作为附加动作。
    m_testButton = buttons->addButton(tr("测试连接"), QDialogButtonBox::ActionRole);
    // 点击「测试连接」→ 本地校验后发起连通性测试。
    connect(m_testButton, &QPushButton::clicked, this, &SettingsDialog::testConnection);
    // 保存按钮 → accept()：先落盘配置再关闭对话框并返回 Accepted。
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    // 取消按钮 → reject()：直接关闭并返回 Rejected，不做任何持久化。
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    root->addWidget(buttons);
}

// ===========================================================================
//  配置回填与读取
// ===========================================================================

/**
 * @brief 从已持久化的配置回填三个控件的初值。
 *
 * 逐项调用 AppConfig 的只读接口：API Key 直接取文本、API 地址转为字符串后填入、
 * 刷新间隔取整数值写入微调框。本函数只读配置、不写控件以外的任何状态。
 *
 * @return 无。
 * @note 必须在 buildUi() 之后调用，否则三个控件仍为空指针；API Key 默认为空，
 *       界面因此呈现「未配置」的占位提示，这是预期行为而非缺陷。
 */
void SettingsDialog::loadFromConfig()
{
    // 回填 API Key：未配置时为空字符串，占位文案会自动提示获取途径。
    m_keyEdit->setText(AppConfig::apiKey());
    // 回填 API 地址：转成字符串以便放入单行输入框。
    m_baseUrlEdit->setText(AppConfig::baseUrl().toString());
    // 回填自动刷新间隔：0 会触发微调框的特殊值文案「关闭自动刷新」。
    m_refreshSpin->setValue(AppConfig::refreshSeconds());

    // 回填桌面集成四项：开关状态与指标选择。全部走 AppConfig 的只读接口，
    // 因此"默认值"只在一处定义（AppConfig），界面无需重复一份默认逻辑。
    m_trayCheck->setChecked(AppConfig::trayEnabled());
    m_taskbarCheck->setChecked(AppConfig::taskbarBadgeEnabled());
    m_closeToTrayCheck->setChecked(AppConfig::closeToTray());

    const int metricValue = static_cast<int>(AppConfig::statusMetric());
    const int metricIndex = m_metricCombo->findData(metricValue);
    // findData 返回 -1 说明配置里的取值不在候选项内（例如手工改坏配置文件），
    // 此时退回第 0 项而不是留空，保证界面始终处于可用状态。
    m_metricCombo->setCurrentIndex(metricIndex >= 0 ? metricIndex : 0);

    // 回填外观主题。顺序上必须先选中、后连接信号：若先连接，初始化时的
    // setCurrentIndex() 会被当成一次用户操作，触发一次多余的落盘与重绘。
    const int themeValue = static_cast<int>(AppConfig::themeMode());
    const int themeIndex = m_themeCombo->findData(themeValue);
    // 回退项取最后一项（system）：它的语义就是"不做任何强制"，是非法值最安全的落点。
    m_themeCombo->setCurrentIndex(themeIndex >= 0 ? themeIndex : m_themeCombo->count() - 1);
    // 记录进入对话框时的模式，供 reject() 在用户点「取消」时还原外观与配置。
    m_initialThemeMode = AppConfig::themeMode();
    connect(m_themeCombo, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::onThemeModeChanged);
}

bool SettingsDialog::trayEnabled() const
{
    return m_trayCheck->isChecked();
}

bool SettingsDialog::taskbarBadgeEnabled() const
{
    return m_taskbarCheck->isChecked();
}

bool SettingsDialog::closeToTray() const
{
    return m_closeToTrayCheck->isChecked();
}

AppConfig::StatusMetric SettingsDialog::statusMetric() const
{
    // 控件里存的是枚举整数值；取不到时回退到 5 小时窗口，与 AppConfig 的默认值一致。
    const int value = m_metricCombo->currentData().toInt();
    return AppConfig::statusMetricFromKey(AppConfig::statusMetricKey(static_cast<AppConfig::StatusMetric>(value)),
                                          AppConfig::StatusMetric::fiveHour);
}

/**
 * @brief 读取用户填写的 API Key 文本。
 *
 * 返回值统一做首尾空白裁剪：用户从网页复制密钥时极易带上换行或空格，若不裁剪，
 * 请求头中的密钥会因隐藏字符而鉴权失败，且这类问题极难排查。
 *
 * @return QString，裁剪后的密钥文本；用户未填写时为空字符串。
 * @note 在 getter 而非保存处裁剪，可保证「测试连接」与「保存」使用同一份字符串。
 */
QString SettingsDialog::apiKey() const
{
    return m_keyEdit->text().trimmed();
}

/**
 * @brief 读取并归一化用户填写的 API 地址。
 *
 * 归一化分两步：① 先裁剪空白，若结果为空则回退到默认官方地址，保证返回值始终是
 * 一个合法可用的 URL；② 循环去掉末尾所有 '/'，因为各接口路径都以 '/' 开头，
 * 保留尾部斜杠会拼出「//」这种可能被部分网关拒绝的路径。
 *
 * @return QUrl，归一化后的 API 基础地址；输入为空时为默认官方地址。
 * @note 本函数只做字符串整理，不校验主机可达性；连通性判断由测试连接完成。
 */
QUrl SettingsDialog::baseUrl() const
{
    // 先裁剪空白：粘贴地址时同样可能带上不可见的前后空格。
    QString text = m_baseUrlEdit->text().trimmed();
    // 空输入回退到默认地址，避免返回空 QUrl 导致后续请求直接失败。
    if (text.isEmpty())
        text = QStringLiteral("https://api.commandcode.ai");
    // 循环而非单次 chop：用户可能粘贴了「https://host///」这类多个尾部斜杠。
    while (text.endsWith(QLatin1Char('/')))
        text.chop(1);
    // 用整理后的字符串构造 QUrl 返回；此处不再做 scheme 校验。
    return QUrl(text);
}

/**
 * @brief 读取自动刷新间隔秒数。
 *
 * 直接返回微调框当前值，不做范围校验——范围由 setRange(0, 3600) 在控件层面保证，
 * 在 getter 中重复校验只会增加冗余分支。0 表示关闭自动刷新。
 *
 * @return int，自动刷新间隔秒数，取值 0 ~ 3600；0 表示关闭自动刷新。
 */
int SettingsDialog::refreshSeconds() const
{
    return m_refreshSpin->value();
}

// ===========================================================================
//  用户动作：从 CLI 导入
// ===========================================================================

/**
 * @brief 槽函数：把 Command Code CLI 已登录的 API Key 导入到输入框。
 *
 * 该动作由用户点击「从 CLI 导入」显式触发，不做任何静默读取。处理步骤为：
 *   ① 准备三个出参变量（密钥、用户名、错误信息）；
 *   ② 调用 AppConfig::importFromCommandCodeCli() 读取 ~/.commandcode/auth.json；
 *   ③ 失败时以红色显示具体原因并提前返回，不改动输入框内容；
 *   ④ 成功时把密钥写入输入框，并以绿色提示结果（有用户名则一并展示）。
 *
 * 之所以让被调方通过出参回传用户名与错误，而不是让调用方再去解析一次 auth.json，
 * 是为了让文件只被读取一次，避免重复 I/O 以及两次解析之间文件变化带来的不一致。
 *
 * @return 无。
 * @note 本函数会读取磁盘上的凭据文件，仅在用户显式点击按钮时执行；导入成功后
 *       仍需用户点击「保存」才会把密钥真正写入本应用配置。
 */
void SettingsDialog::importFromCli()
{
    // 出参：导入成功后回传的 API Key。
    QString key;
    // 出参：CLI 中记录的用户名，可能为空（旧版本 CLI 未写入该字段）。
    QString userName;
    // 出参：失败时回传可读的错误原因，用于直接展示给用户。
    QString error;
    // 一次性完成读文件与解析：成功返回 true，失败返回 false 并填充 error。
    if (!AppConfig::importFromCommandCodeCli(&key, &userName, &error)) {
        // 失败标 danger 级别，与成功态的绿色形成明确区分；色值由样式表按主题给出。
        AppTheme::setLevel(m_statusLabel, QStringLiteral("danger"));
        // 展示底层给出的具体原因（文件不存在、格式非法等），便于用户自助排查。
        m_statusLabel->setText(tr("导入失败：%1").arg(error));
        // 失败时保持输入框原值不变，避免用户已填写的密钥被清空。
        return;
    }
    // 成功：把导入到的密钥填回输入框，用户可继续核对或直接保存。
    m_keyEdit->setText(key);
    // 成功标 ok 级别（绿）。
    AppTheme::setLevel(m_statusLabel, QStringLiteral("ok"));
    // 用户名非空时附带展示，便于用户确认导入的是哪个账号的凭据。
    m_statusLabel->setText(userName.isEmpty()
                               ? tr("已从 CLI 导入 API Key。")
                               : tr("已从 CLI 导入 API Key（用户：%1）。").arg(userName));
}

// ===========================================================================
//  用户动作：连通性测试
// ===========================================================================

/**
 * @brief 槽函数：校验输入后发起一次 API 连通性测试。
 *
 * 处理步骤：
 *   ① 前置校验——API Key 去空白后为空则红色提示并直接返回，不发无意义的请求；
 *   ② 禁用「测试连接」按钮，防止用户连点产生并发请求；
 *   ③ 清除状态标签样式（回到主题默认色）并显示「正在测试连接…」；
 *   ④ 把当前界面上的地址与密钥写入测试用 API 对象，然后发起请求。
 *
 * 校验用 m_keyEdit->text().trimmed() 而非 apiKey()，是为了与 apiKey() 的裁剪规则
 * 保持一致的判断口径：只含空白的输入同样视为「未填写」。
 *
 * @return 无。
 * @note 实际请求是异步的，结果经由 connectionTested 信号回到
 *       onConnectionTested()，在那里重新启用按钮并展示结果。
 */
void SettingsDialog::testConnection()
{
    // 只含空白字符的输入一律视为未填写，避免发出必然失败的请求。
    if (m_keyEdit->text().trimmed().isEmpty()) {
        // 校验失败标 danger 级别。
        AppTheme::setLevel(m_statusLabel, QStringLiteral("danger"));
        // 明确告诉用户先补哪一项，而不是笼统报「参数错误」。
        m_statusLabel->setText(tr("请先填写 API Key。"));
        // 提前返回，不进入请求流程，也不改动按钮可用状态。
        return;
    }
    // 请求发出前禁用按钮：既防连点，也向用户暗示「正在进行中」。
    m_testButton->setEnabled(false);
    // 清除语义级别，让「进行中」文案使用主题默认颜色，而非残留上一次的红 / 绿。
    AppTheme::setLevel(m_statusLabel, QString());
    // 提示用户请求已发出；省略号表示这是一个尚在进行中的状态。
    m_statusLabel->setText(tr("正在测试连接…"));

    // 把界面上的地址（已归一化）同步到测试对象，确保测试目标与保存目标一致。
    m_testApi->setBaseUrl(baseUrl());
    // 把界面上的密钥（已裁剪）同步到测试对象。
    m_testApi->setApiKey(apiKey());
    // 发起异步测试；返回结果稍后通过 connectionTested 信号回到本对话框。
    m_testApi->testConnection();
}

/**
 * @brief 槽函数：接收连通性测试结果并更新界面。
 *
 * 由 CommandCodeApi::connectionTested 信号触发，处理动作有两步：① 无论成败都重新
 * 启用「测试连接」按钮，使界面恢复可交互状态；② 按成功与否选择绿色 / 红色样式，
 * 并把服务端返回或本地生成的说明文案写入状态标签。
 *
 * @param[in] ok bool，测试是否成功；true 表示连通，false 表示失败或超时。
 * @param[in] message QString，展示给用户的说明文案，成功时为服务端信息，失败时
 *                    为具体错误原因。
 * @return 无。
 * @note 本函数必须挂接在测试请求发出之前，否则结果无人接收、按钮将一直处于禁用态。
 */
void SettingsDialog::onConnectionTested(bool ok, const QString &message)
{
    // 结果已到，恢复按钮可用，允许用户再次测试或直接保存。
    m_testButton->setEnabled(true);
    // 成功绿、失败红；只写语义级别，具体色值由样式表按当前主题给出。
    AppTheme::setLevel(m_statusLabel, ok ? QStringLiteral("ok") : QStringLiteral("danger"));
    // 直接把结果文案展示出来，不再自行拼接前缀，避免重复信息。
    m_statusLabel->setText(message);
}

// ===========================================================================
//  外观主题
// ===========================================================================

/**
 * @brief 读取用户选择的界面主题模式。
 *
 * 控件里存的是枚举整数值，经 themeModeKey() 与 themeModeFromKey() 往返一次后返回，
 * 这样"非法值如何回退"的规则只由 AppConfig 定义一处，界面不重复实现。
 *
 * @return AppConfig::ThemeMode，当前选中的模式；控件缺失或未选中时返回 system。
 * @note 与 statusMetric() 采用完全相同的取值风格，便于维护者举一反三。
 */
AppConfig::ThemeMode SettingsDialog::themeMode() const
{
    // 控件理论上一定存在，但显式判空可避免将来调整构建顺序时出现空指针解引用。
    if (m_themeCombo == nullptr || m_themeCombo->currentIndex() < 0)
        return AppConfig::ThemeMode::system;
    const int value = m_themeCombo->currentData().toInt();
    return AppConfig::themeModeFromKey(AppConfig::themeModeKey(static_cast<AppConfig::ThemeMode>(value)),
                                       AppConfig::ThemeMode::system);
}

/**
 * @brief 槽函数：用户切换主题下拉框时立即预览并生效。
 *
 * 主题是"所见即所得"的设置：若等到点「保存」才生效，用户在对话框里无法判断
 * 选中的到底是哪一档（尤其"跟随系统"在系统为浅色时与"浅色主题"看起来一样）。
 * 因此这里即时调用 AppTheme::setMode()——它同时写入配置并广播变更，
 * 主窗口、进度条等订阅者会立即按新主题重绘。
 *
 * @return 无。
 * @note 立即落盘意味着"取消"必须负责还原，该职责由 reject() 承担。
 */
void SettingsDialog::onThemeModeChanged()
{
    AppTheme::setMode(themeMode());
}

/**
 * @brief 覆写取消行为：还原进入对话框时的主题，再交给基类关闭。
 *
 * 由于主题在切换时已即时生效并落盘，用户按「取消」时若不还原，会出现
 * "点了取消、外观却变了、配置也被改了"的错觉。这里比较当前配置与进入时的模式，
 * 仅在确实被改动过时才回写，避免无谓的落盘与重绘。
 *
 * @return 无。
 * @note 点窗口关闭按钮与点「取消」走同一条路径，行为保持一致。
 */
void SettingsDialog::reject()
{
    if (AppConfig::themeMode() != m_initialThemeMode)
        AppTheme::setMode(m_initialThemeMode);
    QDialog::reject();
}

// ===========================================================================
//  用户动作：保存并关闭
// ===========================================================================

/**
 * @brief 覆写接受行为：先把当前配置写入持久化存储，再关闭对话框。
 *
 * 保存顺序为 API Key → API 地址 → 刷新间隔，三者都通过 getter 取值，因此
 * apiKey() 的空白裁剪与 baseUrl() 的尾部斜杠去除都会在落盘前生效，写入的始终是
 * 归一化后的值。三项写完后调用基类 accept() 关闭对话框并返回 Accepted。
 *
 * @return 无。
 * @note 本实现没有失败分支，配置写入后必定关闭；若将来需要「校验不通过则不关闭」
 *       的行为，应在此处提前 return，而不是改写基类语义。
 */
void SettingsDialog::accept()
{
    // 写入裁剪后的 API Key（用户可能在「从 CLI 导入」后直接保存）。
    AppConfig::setApiKey(apiKey());
    // 写入归一化后的 API 地址，保证后续请求拼接路径时不会出现双斜杠。
    AppConfig::setBaseUrl(baseUrl());
    // 写入自动刷新间隔；0 表示关闭自动刷新。
    AppConfig::setRefreshSeconds(refreshSeconds());
    // 写入外观主题。切换下拉框时已即时落盘，此处再写一次是为了覆盖"用户从未
    // 动过下拉框"的场景，使本函数对全部配置项保持"保存即完整落盘"的一致语义；
    // 重复写入同一个值无任何副作用。
    AppConfig::setThemeMode(themeMode());
    // 写入桌面集成配置：托盘开关、任务栏开关、关闭行为与缩略信息指标。
    AppConfig::setTrayEnabled(trayEnabled());
    AppConfig::setTaskbarBadgeEnabled(taskbarBadgeEnabled());
    AppConfig::setCloseToTray(closeToTray());
    AppConfig::setStatusMetric(statusMetric());
    // 走基类流程：关闭对话框并令 exec() 返回 QDialog::Accepted。
    QDialog::accept();
}
