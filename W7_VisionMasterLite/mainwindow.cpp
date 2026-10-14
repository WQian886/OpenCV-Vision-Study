#include "mainwindow.h"
#include "operator_node.h"
#include "connection_line.h"
#include "select_delete_plan.h"   // 批量删除的"计划"层(见头文件里的 use-after-free 说明)
#include "operator_library_groups.h"   // 算子库分组:不变式是"没有算子会消失"
#include "pipeline_serialize.h"   // 存盘/读盘(已搬出,无头测试直接用)

#include <QDockWidget>
#include <QToolBar>
#include <QStatusBar>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QApplication>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QPixmap>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <QScopeGuard>
#include <memory>
#include <QQueue>
#include <QHash>
#include <QFile>

// 算子头文件
#include "operators/operator_base.h"
#include "operators/param_form.h"
#include "operators/operator_factory.h"
#include "operators/color_segment.h"
#include "operators/blob_count.h"
#include "operators/count_check.h"
#include "operators/template_match.h"
#include "operators/qr_decode.h"
#include "operators/image_source_dir.h"
#include "operators/image_source_camera.h"
#include "operators/pcb_inspect.h"
#include "operators/pcb_defect_judge.h"
#include "operators/line_fit.h"
#include "operators/circle_fit.h"
#include "operators/caliper_dist.h"

// === M6 撤销/重做:具体 Command 子类 ===
#include "commands/node_commands.h"
#include "commands/edge_commands.h"

namespace w7 {

// ============================================================
// 算子工厂:见 operators/operator_factory.cpp 的 w7::createOperatorByName
// ============================================================
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Mini VisionMaster Lite — W7"));
    resize(1400, 900);

    // === 最近工程列表 ===
    // 必须在 setupToolbar 之前建好 —— 那里要读它来决定
    // "最近"按钮初始是否置灰
    m_recentFiles = new w7::RecentFiles(QStringLiteral("recent/sol_json"), 5);

    registerOperators();
    setupUi();
    setupToolbar();
    setupOperatorLibrary();
    setupParamPanel();
    setupResultPanel();

    // === S1: 把算子表注册到画布(供拖拽时查询) ===
    QHash<QString, w7::OpMeta> registry;
    for (const auto& op : m_operatorInfo) {
        w7::OpMeta m;
        m.name        = op.name;
        m.display     = op.display;
        m.category    = op.category;
        m.description = op.description;
        // === pin 类型:从算子自己问出来,免得两处各写一份表、改一处忘一处 ===
        if (w7::OperatorBase* o = w7::createOperatorByName(op.name)) {
            m.inType   = o->inputPinType();
            m.outType  = o->outputPinType();
            m.isSource = o->isSource();
            delete o;
        }
        registry.insert(op.name, m);
    }
    m_canvas->setOperatorRegistry(registry);

    // === S3: 接画布选中变化 + 参数面板应用按钮 ===
    connect(m_canvas, &w7::PipelineView::selectionChangedInScene,
            this, &MainWindow::onSceneSelectionChanged);

    // === P0: 接节点右键删除请求 → 统一走 PipelineView::removeNode ===
    connect(m_canvas, &w7::PipelineView::nodeDeleteRequested,
            this, &MainWindow::onNodeDeleteRequested);

    // === M6: 接节点拖动 → 生成 MoveNodeCmd ===
    connect(m_canvas, &w7::PipelineView::nodeMovedSignal,
            this, &MainWindow::onNodeMoved);

    // === 每步结果查看:节点右键 → 显示该步缓存的输出 ===
    connect(m_canvas, &w7::PipelineView::nodeViewResultRequested,
            this, &MainWindow::onViewStepResultRequested);

    // === M6: 画布的"增节点/连线"请求 → 包成 Command ===
    connect(m_canvas, &w7::PipelineView::nodeAddRequested,
            this, [this](const QString& op, const QString& disp,
                         const QString& cat, const QPointF& pos) {
        doAddNode(op, disp, cat, pos);
    });
    connect(m_canvas, &w7::PipelineView::connectionAddRequested,
            this, [this](w7::OperatorNode* from, w7::OperatorNode* to,
                         w7::OperatorNode::PortSide fromSide,
                         w7::OperatorNode::PortSide toSide) {
        doConnect(from, to, fromSide, toSide);
    });

    loadDefaultPipeline();

    statusBar()->showMessage(tr("就绪。点击 ▶ Run 运行默认流水线 | 从左侧算子库拖到画布创建节点 | 点击节点切参数面板"));
}

MainWindow::~MainWindow() {
    // === M6: Command 由栈持有,必须手动 delete(QStack 不析构元素) ===
    clearUndoRedoStacks();
    // OperatorInfo 是值类型,无需手动释放
    m_operatorInfo.clear();
}

void MainWindow::setupUi() {
    // 中央:流程图画布
    m_canvas = new PipelineView(this);
    setCentralWidget(m_canvas);
}

void MainWindow::setupToolbar() {
    QToolBar* tb = addToolBar(tr("主工具栏"));
    tb->setMovable(true);

    QPushButton* runBtn = new QPushButton(tr("▶ Run"), this);
    // === Stop ===
    // 跑批之前只有上限没有退路,和没有上限没本质区别:
    // 2000 帧跑完要很久,用户总得能喊停。
    // 平时置灰,只在 Run 期间可用 —— 免得空按钮让人以为坏了。
    m_stopBtn = new QPushButton(tr("■ Stop"), this);
    m_stopBtn->setEnabled(false);
    QPushButton* loadBtn = new QPushButton(tr("Load"), this);
    QPushButton* saveBtn = new QPushButton(tr("Save"), this);
    tb->addWidget(runBtn);
    tb->addWidget(m_stopBtn);
    tb->addWidget(loadBtn);
    tb->addWidget(saveBtn);

    // === 最近工程 ===
    // 独立成一个下拉而不是复用 Load:流水线是"打开工程"的主入口,
    // 藏进二级菜单等于把主入口降级了。
    m_recentMenu = new QMenu(tr("最近工程"), this);
    // 菜单不放进菜单栏(这个项目没有),直接由 QPushButton 弹出
    m_recentBtn = new QPushButton(tr("最近 ▾"), this);
    m_recentBtn->setMenu(m_recentMenu);
    // 没内容时置灰,免得点开是个空菜单
    m_recentBtn->setEnabled(!m_recentFiles->paths().isEmpty());
    connect(m_recentMenu, &QMenu::aboutToShow,
            this, &MainWindow::rebuildRecentMenu);
    tb->addWidget(m_recentBtn);

    tb->addSeparator();
    tb->addWidget(new QLabel(tr(" 图像: "), this));
    m_imagePathEdit = new QLineEdit("images/baseline.png", this);
    m_imagePathEdit->setMinimumWidth(280);
    tb->addWidget(m_imagePathEdit);

    // === M6: Undo / Redo 按钮(用 QAction 而非 QPushButton,自带快捷键+启用态) ===
    m_undoAction = new QAction(tr("↶ Undo"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);        // Ctrl+Z
    m_undoAction->setShortcutContext(Qt::WindowShortcut); // 在参数面板里也生效
    m_undoAction->setEnabled(false);
    tb->addAction(m_undoAction);

    m_redoAction = new QAction(tr("↷ Redo"), this);
    m_redoAction->setShortcut(QKeySequence::Redo);        // Ctrl+Y (Win/Linux)
    m_redoAction->setShortcuts({QKeySequence(QKeySequence::Redo),
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    m_redoAction->setShortcutContext(Qt::WindowShortcut);
    m_redoAction->setEnabled(false);
    tb->addAction(m_redoAction);

    connect(runBtn,  &QPushButton::clicked, this, &MainWindow::onRunClicked);
    connect(m_stopBtn, &QPushButton::clicked, this, [this] {
        // 不在跑时点了(理论上按钮是灰的,但快捷键/程序化调用拦不住)
        if (!w7::requestCancel(&m_runState)) return;
        appendLog(tr("[Run] 收到停止请求,当前这一帧跑完就收工"));
        m_stopBtn->setEnabled(false);
    });
    connect(loadBtn, &QPushButton::clicked, this, &MainWindow::onLoadProject);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveProject);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::execUndo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::execRedo);
}

void MainWindow::registerOperators() {
    m_operatorInfo.append({"image_source_dir", tr("文件夹批量图像"), "acquisition",
                           tr("按通配符批量加载文件夹图像(对标 VM 文件夹图像源)")});
    m_operatorInfo.append({"image_source_camera", tr("工业相机实时流"), "acquisition",
                           tr("从海康 GigE 工业相机取一帧(对标 VM 相机采集;需 MVS SDK)")});
    m_operatorInfo.append({"image_source_video", tr("视频文件图像源"), "acquisition",
                           tr("从视频逐帧取图,Run 时按帧循环(对标 VM 视频图像源)")});
    m_operatorInfo.append({"color_segment",  tr("颜色分割"),   "detection",
                           tr("HSV 颜色范围提取")});
    m_operatorInfo.append({"blob_count",     tr("Blob 计数"),   "detection",
                           tr("连通域计数 + 面积过滤")});
    m_operatorInfo.append({"count_check",    tr("数量判定"),   "detection",
                           tr("区间判定 PASS/FAIL")});
    m_operatorInfo.append({"template_match", tr("模板匹配"),   "positioning",
                           tr("在大图找模板位置")});
    m_operatorInfo.append({"qr_decode",      tr("QR 解码"),    "identification",
                           tr("读 QR 码内容")});
    // === W6 深度联动:PCB 检测上流水线(定位/判定拆成两个算子) ===
    m_operatorInfo.append({"pcb_inspect", tr("PCB 元件定位"), "detection",
                           tr("定位 PCB 上的元件,支持真值坐标/自动轮廓(源自 W6)")});
    m_operatorInfo.append({"pcb_defect_judge", tr("PCB 缺陷判定"), "detection",
                           tr("判定 7 类 PCB 缺陷,阈值全可调(源自 W6)")});
    // === DL 算子:传统 CV 之外的第二条腿 ===
    m_operatorInfo.append({"dl_infer", tr("DL 推理 (ONNX)"), "dl",
                           tr("加载 ONNX 模型做目标检测(YOLOv5/v8);"
                              "需 ONNX Runtime,未装时算子仍可用只是 Run 会提示")});
    // === 测量类:补齐 VM 四大模块里最后一个(定位/识别/检测都有了) ===
    m_operatorInfo.append({"line_fit", tr("直线测量"), "measurement",
                           tr("给两个点,量出直线的长度和角度(0~180°)")});
    m_operatorInfo.append({"circle_fit", tr("圆形测量"), "measurement",
                           tr("在二值图里找出圆形目标,量圆心/半径/圆度")});
    m_operatorInfo.append({"caliper_dist", tr("卡尺距离"), "measurement",
                           tr("三点卡尺:AB 定基准,量 C 到 AB 的垂直距离")});
    m_operatorInfo.append({"point_point_dist", tr("两点距离"), "measurement",
                           tr("给两个点,量出它们的欧氏距离(像素)")});
    m_operatorInfo.append({"point_line_dist", tr("点到线距离"), "measurement",
                           tr("三点:AB 定基准,量 C 到 AB 的垂直距离")});
    m_operatorInfo.append({"angle", tr("两线夹角"), "measurement",
                           tr("给四点(AB/CD 各一条线),量两线之间夹角(0~90°)")});
}

void MainWindow::setupOperatorLibrary() {
    QDockWidget* dock = new QDockWidget(tr("算子库"), this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_opLib = new OpTreeWidget(dock);
    m_opLib->setHeaderLabel(tr("算子名"));
    m_opLib->setRootIsDecorated(true);
    // === S1: 启用拖拽 ===
    m_opLib->setDragEnabled(true);
    m_opLib->setDragDropMode(QAbstractItemView::DragOnly);
    m_opLib->setSelectionMode(QAbstractItemView::SingleSelection);

    // ============================================================
    // 按类别分组 —— 分组逻辑在 operator_library_groups.h,
    // 那里的不变式是"**没有算子会消失**"。
    //
    // 以前这里是硬编码 5 个 if/else,而 registerOperators() 里写了
    // category = "dl"。6 个分类只认 5 个 → parent 停在 nullptr
    // → `if (parent)` 不成立 → dl_infer 凭空消失,连日志都不打。
    //
    // 只补一个 `else if (dl)` 能压住这一次的症状,但形态没变:
    // **下一个新分类还会再犯同样的静默丢失。** 所以改成兜底建组。
    // ============================================================
    QList<OperatorInfoLite> lite;
    lite.reserve(m_operatorInfo.size());
    for (const auto& op : m_operatorInfo)
        lite.append({op.name, op.display, op.category});

    QList<QPair<QString, QString>> knownGroups = {
        {"acquisition",    tr("采集 Acquisition")},
        {"positioning",    tr("定位 Positioning")},
        {"measurement",    tr("测量 Measurement")},
        {"identification", tr("识别 Identification")},
        {"detection",      tr("检测 Detection")},
        {"dl",             tr("深度学习 Deep Learning")},
    };

    QStringList unknownCats;
    const auto groups = buildOperatorLibraryGroups(lite, knownGroups, &unknownCats);
    for (const auto& cat : unknownCats) {
        appendLog(tr("[算子库] 分类 '%1' 不在已知分组里,已现建一个 —— "
                     "以后在 knownGroups 里给它配个显示名").arg(cat));
    }

    // 每个算子的显示名/描述还要按名字对回去 —— 建组只关心分类归属
    QHash<QString, const OperatorInfo*> byName;
    for (const auto& op : m_operatorInfo) byName.insert(op.name, &op);

    for (const auto& g : groups) {
        if (g.operators.isEmpty()) continue;
        auto* groupItem = new QTreeWidgetItem(m_opLib, {g.title});
        for (const QString& opName : g.operators) {
            const OperatorInfo* op = byName.value(opName, nullptr);
            if (!op) continue;
            auto* item = new QTreeWidgetItem(groupItem, {op->display});
            item->setData(0, Qt::UserRole, op->name);
            item->setToolTip(0, op->description);
        }
    }
    m_opLib->expandAll();
    connect(m_opLib, &QTreeWidget::itemSelectionChanged,
            this, &MainWindow::onOperatorSelected);

    dock->setWidget(m_opLib);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
}

void MainWindow::setupParamPanel() {
    QDockWidget* dock = new QDockWidget(tr("参数面板"), this);
    m_paramPanel = new QStackedWidget(dock);

    // 默认占位页
    auto* placeholder = new QWidget;
    auto* phLayout = new QVBoxLayout(placeholder);
    phLayout->addWidget(new QLabel(tr("(选中一个节点查看参数)\n\n"
                                       "本 MVP 简化:\n"
                                       "所有算子参数在 Run 时用默认值。\n"
                                       "完整版会按算子类型动态生成表单。")));
    phLayout->addStretch();
    m_paramPanel->addWidget(placeholder);

    dock->setWidget(m_paramPanel);
    addDockWidget(Qt::RightDockWidgetArea, dock);
}

void MainWindow::setupResultPanel() {
    QDockWidget* dock = new QDockWidget(tr("结果 / 日志"), this);

    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);

    // === 每步结果查看:标题条 ===
    // 让人一眼知道现在显示的是「整条流水线的最终结果」还是
    // 「我刚点开的第 3 步」。二者长得不一样,不标出来会以为
    // 右键看到的那张图就是判定结果。
    m_resultTitle = new QLabel(tr("最终结果"));
    m_resultTitle->setStyleSheet("font-weight:bold; color:#444; padding:2px;");
    layout->addWidget(m_resultTitle);

    // 图像预览
    m_resultImg = new QLabel(tr("(运行后显示标注图)"));
    m_resultImg->setAlignment(Qt::AlignCenter);
    m_resultImg->setMinimumHeight(180);
    m_resultImg->setStyleSheet("background:#222; color:#ddd; border:1px solid #555;");
    layout->addWidget(m_resultImg);

    // === 每步结果查看:该步的完整输出字段 ===
    // 图像只说明"看起来对不对",数值才是判定的依据
    // (阈值、得分、框坐标、缺陷清单)。没有这块,看到图不对
    // 也无从判断是哪一步的哪个参数导致的。
    m_stepDetail = new QPlainTextEdit;
    m_stepDetail->setReadOnly(true);
    m_stepDetail->setMaximumHeight(110);
    m_stepDetail->setPlaceholderText(tr("(运行后,右键任意节点可查看该步输出)"));
    m_stepDetail->setStyleSheet(
        "background:#f7f7f7; color:#222; font-family:Consolas,monospace;"
        "border:1px solid #bbb;");
    layout->addWidget(m_stepDetail);

    // 日志
    //
    // 用 QPlainTextEdit 而不是 QTextEdit,这不是随手选的:
    // Run 的帧循环会写"帧数 x 节点数"行日志,而每写一行,
    // processEvents() 就把整个日志文档重排版+重画一次。
    // 实测(2026-10-08,2000x220 字符行,offscreen):
    //     QTextEdit      append 90 us/行,append+重绘 1545 us/行
    //     QPlainTextEdit append 90 us/行,append+重绘  785 us/行
    // 重绘才是大头(是 append 的十几倍),QPlainTextEdit 便宜一半。
    // QTextEdit 是富文本控件,为一个只读日志付出富文本的排版代价不值。
    //
    // 只读 + 封顶都收在 configureLogView() 里,好让无头测试能验到
    // **同一段代码** —— 理由见 run_policy.h 里那个函数的注释。
    m_logView = new QPlainTextEdit;
    w7::configureLogView(m_logView);
    m_logView->setStyleSheet("background:#1e1e1e; color:#ddd; font-family:Consolas,monospace;");
    layout->addWidget(m_logView, 1);

    dock->setWidget(container);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
}

void MainWindow::loadDefaultPipeline() {
    // MVP 默认流水线:读图 → HSV 分割 → Blob 计数 → 数量判定
    m_canvas->clearPipeline();
    QJsonObject params;

    QString imagePath = "images/baseline.png";
    if (QFile::exists("../W6_PCB_DefectProject/images/baseline.png")) {
        imagePath = "../W6_PCB_DefectProject/images/baseline.png";
    } else if (QFile::exists("../" + imagePath)) {
        imagePath = "../" + imagePath;
    }
    m_imagePathEdit->setText(imagePath);

    auto* n_src = m_canvas->addNode("image_source_dir", tr("文件夹批量图像"), "acquisition", {100, 200});
    QJsonObject srcParams;
    srcParams["dir_path"] = imagePath;   // 默认用 W6 的 images/baseline.png 所在目录
    srcParams["pattern"]  = "*.png";
    n_src->setParams(srcParams);

    auto* n1 = m_canvas->addNode("color_segment", tr("颜色分割"),  "detection",    {400, 200});
    auto* n2 = m_canvas->addNode("blob_count",    tr("Blob 计数"),  "detection",    {650, 200});
    auto* n3 = m_canvas->addNode("count_check",   tr("数量判定"),  "detection",    {900, 200});
    auto* n4 = m_canvas->addNode("qr_decode",     tr("QR 解码"),   "identification",{400, 380});

    // === S2: 用 ConnectionLine 创建贝塞尔连线 ===
    m_canvas->addConnection(n_src, n1);
    m_canvas->addConnection(n1, n2);
    m_canvas->addConnection(n2, n3);

    m_canvas->scene()->setSceneRect(m_canvas->scene()->itemsBoundingRect().adjusted(-50,-50,50,50));

    appendLog(tr("已加载默认流水线: 文件夹批量图像 → 颜色分割 → Blob 计数 → 数量判定 (旁路: QR 解码)"));
    appendLog(tr("提示:点击 ▶ Run 执行 / 从节点右侧端口拖到另一节点左侧端口可手动连线"));

    // === M6: 默认流水线是"起始状态",不该被 Ctrl+Z 撤掉 ===
    clearUndoRedoStacks();
}

// ============================================================
void MainWindow::onOperatorSelected() {
    auto* item = m_opLib->currentItem();
    if (!item) return;
    QString opName = item->data(0, Qt::UserRole).toString();
    if (opName.isEmpty()) return;  // 分组节点没 userRole

    appendLog(tr("[选中算子] %1 (%2)").arg(item->text(0), opName));
    // === S3: 选中算子库也展示默认参数面板(用于添加新节点前预览) ===
    w7::OperatorBase* op = w7::createOperatorByName(opName);
    if (!op) return;
    QJsonObject defaults = op->defaultParams();
    QWidget* widget = op->createParamWidget(m_paramPanel, defaults);
    delete op;
    if (!widget) return;

    if (m_currentParamWidget) {
        m_paramPanel->removeWidget(m_currentParamWidget);
        m_currentParamWidget->deleteLater();
    }
    m_paramPanel->addWidget(widget);
    m_currentParamWidget = widget;
    m_paramPanel->setCurrentWidget(widget);
    m_currentParamNode = nullptr;  // 当前显示的是算子库预览,不是某节点的参数

    // 接"应用参数"信号
    if (auto* form = qobject_cast<w7::ParamForm*>(widget)) {
        connect(form, &w7::ParamForm::valueChanged,
                this, &MainWindow::onParamApplied);
    }
}

// === S3: 画布选中变化 → 切换参数面板 ===
void MainWindow::onSceneSelectionChanged() {
    auto selected = m_canvas->scene()->selectedItems();
    qDebug() << "[SSC] ===> enter, m_inUpdate=" << m_inUpdate
             << "selected count=" << selected.size();
    OperatorNode* node = nullptr;
    for (auto* item : selected) {
        if (auto* n = dynamic_cast<OperatorNode*>(item)) {
            node = n;
            break;
        }
    }
    qDebug() << "[SSC] ===> calling UPN with node="
             << (node ? node->opName() : QStringLiteral("null"));
    updateParamPanelForNode(node);
    qDebug() << "[SSC] <=== return from UPN";
}

// === S3: 核心 - 用 node 切换参数面板 ===
void MainWindow::updateParamPanelForNode(w7::OperatorNode* node) {
    qDebug() << "[UPN] ===> enter, node="
             << (node ? node->opName() : QStringLiteral("null"))
             << "m_inUpdate=" << m_inUpdate
             << "old=" << m_currentParamWidget.data();

    // === 重入防护:防止 selectionChanged 在 widget swap 期间递归触发导致栈溢出 ===
    if (m_inUpdate) {
        qDebug() << "[UPN] ===> RE-ENTRY BLOCKED, return early";
        return;
    }
    m_inUpdate = true;
    auto resetGuard = qScopeGuard([this] {
        qDebug() << "[UPN] <=== guard fires (function exit)";
        m_inUpdate = false;
    });

    if (!node) {
        qDebug() << "[UPN] null node, set placeholder";
        m_paramPanel->setCurrentIndex(0);  // 切到 placeholder
        m_currentParamNode = nullptr;
        return;
    }
    qDebug() << "[UPN] step 1: setSelectedNode";
    m_selectedNode = node;

    qDebug() << "[UPN] step 2: createOperator for" << node->opName();
    w7::OperatorBase* op = w7::createOperatorByName(node->opName());
    if (!op) { qDebug() << "[UPN] op is null, return"; return; }

    // 用 node 已存的参数初始化表单(没有就用 default)
    qDebug() << "[UPN] step 3: getDefaultParams";
    QJsonObject initParams = node->params();
    if (initParams.isEmpty()) {
        initParams = op->defaultParams();
        node->setParams(initParams);
    }

    // === 修复 3: 调 setParams 让表单显示当前 node 的值 ===
    qDebug() << "[UPN] step 4: createParamWidget BEGIN";
    QWidget* widget = op->createParamWidget(m_paramPanel, initParams);
    qDebug() << "[UPN] step 4: createParamWidget END, widget=" << widget;
    delete op;
    if (!widget) { qDebug() << "[UPN] widget is null, return"; return; }

    // 切掉旧的(用 deleteLater,Qt 安全做法)
    // === S3 修复:先建新 widget 再拆旧 widget,避免 focus chain 切换时连环触发 ===
    // 旧 widget 暂存到局部变量(QPointer 防止被外部先销毁)
    qDebug() << "[UPN] step 6: capture oldWidget, current="
             << m_currentParamWidget.data();
    QPointer<QWidget> oldWidget = m_currentParamWidget;

    // 1) 先把新 widget 装进 stack 并设为当前 → focus 立刻转移到新 widget
    qDebug() << "[UPN] step 7: addWidget(new) BEGIN";
    m_paramPanel->addWidget(widget);
    qDebug() << "[UPN] step 7: addWidget END";
    qDebug() << "[UPN] step 8: setCurrentWidget BEGIN";
    m_paramPanel->setCurrentWidget(widget);
    qDebug() << "[UPN] step 8: setCurrentWidget END";
    qDebug() << "[UPN] step 9: update m_currentParamWidget";
    m_currentParamWidget = widget;
    m_currentParamNode = node;

    // === 修复 4: setParams 必须在 widget 已显示之后(QSpinBox::lineEdit 才被初始化) ===
    qDebug() << "[UPN] step 5: form->setParams BEGIN (AFTER show)";
    if (auto* form = qobject_cast<w7::ParamForm*>(widget)) {
        form->setParams(initParams);
    }
    qDebug() << "[UPN] step 5: form->setParams END";

    // 2) 再拆旧 widget → 此时 focus 已不在旧 widget 上,focusOut 不会再连环触发
    if (oldWidget) {
        qDebug() << "[UPN] step 10: hide old BEGIN, oldWidget=" << oldWidget.data();
        oldWidget->hide();
        qDebug() << "[UPN] step 10: hide old END";
        qDebug() << "[UPN] step 11: removeWidget old BEGIN";
        m_paramPanel->removeWidget(oldWidget);
        qDebug() << "[UPN] step 11: removeWidget old END";
        qDebug() << "[UPN] step 12: deleteLater old BEGIN";
        oldWidget->deleteLater();
        qDebug() << "[UPN] step 12: deleteLater old END";
    } else {
        qDebug() << "[UPN] step 10-12 skipped (no oldWidget, first time)";
    }

    // 用默认 AutoConnection(同线程 = DirectConnection,但更安全)
    if (auto* form = qobject_cast<w7::ParamForm*>(widget)) {
        qDebug() << "[UPN] step 13: connect form valueChanged";
        connect(form, &w7::ParamForm::valueChanged,
                this, [this](const QJsonObject& p) {
                    if (!m_currentParamNode) {
                        appendLog(tr("[参数应用] m_currentParamNode 已失效"));
                        return;
                    }
                    if (!m_currentParamWidget) {
                        appendLog(tr("[参数应用] 当前 widget 已析构"));
                        return;
                    }
                    m_currentParamNode->setParams(p);
                    // === 每步结果查看:参数一改,这一步的旧结果就不作数了 ===
                    // 不清的话用户调完阈值右键回看,看到的还是调参之前的
                    // output,还会以为"改了没生效"。宁可看不到,别给旧的。
                    const QUuid id = m_currentParamNode->uuid();
                    m_stepOutputs.remove(id);
                    m_stepLabels.remove(id);
                    m_currentParamNode->setHasCachedResult(false);
                    appendLog(tr("[参数应用] %1 → %2 个字段")
                        .arg(m_currentParamNode->opName())
                        .arg(p.size()));
                });
    }

    qDebug() << "[UPN] step 14: appendLog";
    appendLog(tr("[选中节点] %1 (%2)").arg(node->displayName(), node->opName()));
    qDebug() << "[UPN] <=== normal exit (about to hit scope guard)";
}

// === S3: 参数面板"应用"按钮(从算子库预览时) ===
void MainWindow::onParamApplied() {
    // 已在 lambda 里处理
}

void MainWindow::onLoadProject() {
    QString path = QFileDialog::getOpenFileName(this, tr("加载流水线"),
        lastUsedDir(), tr("Mini VM Pipeline (*.json *.sol.json)"));
    if (path.isEmpty()) return;
    loadProjectFrom(path);
}

// 最近工程菜单和 Load 按钮走的是同一条路 ——
// 两份加载逻辑各写一遍的话,以后加个"加载后自动 Run"之类的需求
// 必然只改一处,两处行为开始不一致。
void MainWindow::loadProjectFrom(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        appendLog(tr("[Load] ❌ 无法打开文件: %1").arg(f.errorString()));
        return;
    }
    QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError) {
        appendLog(tr("[Load] ❌ JSON 解析失败: %1 (offset %2)")
            .arg(err.errorString()).arg(err.offset));
        return;
    }

    QString loadErr;
    bool ok = pipelineFromJson(m_canvas, doc,
        [this](const QString& s){ appendLog(s); },
        [](const QString& name) -> w7::OpDefaults {
            // 这里 new 算子只为了问"认不认识这个名字"和它的显示名/分类,
            // 读完就扔 —— 所以套 unique_ptr,漏 delete 的写法不可能发生。
            std::unique_ptr<OperatorBase> op(w7::createOperatorByName(name));
            if (!op) return {};
            return {true, op->displayName(), op->category()};
        },
        loadErr);
    if (!ok) {
        appendLog(tr("[Load] ❌ 失败: %1").arg(loadErr));
        return;
    }

    // 恢复工具栏图路径（如果 JSON 有）
    QJsonObject root = doc.object();
    if (root.contains("image_path")) {
        QString imgPath = root["image_path"].toString();
        if (!imgPath.isEmpty()) m_imagePathEdit->setText(imgPath);
    }

    // 调整 sceneRect 适配新节点
    m_canvas->scene()->setSceneRect(
        m_canvas->scene()->itemsBoundingRect().adjusted(-50, -50, 50, 50));

    // === M6: 加载的是全新流水线,旧命令栈已无意义 ===
    // 不清的话 Ctrl+Z 会拿旧工程的 UUID 去当前画布找节点(找不到 → no-op),
    // 用户会看到"按了没反应"。清空并重置按钮。
    clearUndoRedoStacks();

    // === 加载成功才进"最近" ===
    // 失败的也记进去的话,菜单里会攒一串打不开的路径
    m_recentFiles->add(path);
    m_recentBtn->setEnabled(true);

    statusBar()->showMessage(tr("✅ 流水线已加载: %1").arg(path), 3000);
}

QString MainWindow::lastUsedDir() const {
    const QStringList list = m_recentFiles->paths();
    if (!list.isEmpty()) return QFileInfo(list.first()).absolutePath();
    // 没有历史时回到当前工程目录,总比打开"最近用过"里一堆系统目录强
    return QDir::currentPath();
}

void MainWindow::rebuildRecentMenu() {
    m_recentMenu->clear();

    // 先剔掉已经被删掉/挪走的条目,免得点下去只得到一句"文件不存在"
    const int dropped = m_recentFiles->pruneMissing();
    if (dropped > 0) {
        appendLog(tr("[最近] 已清理 %1 条不存在的路径").arg(dropped));
    }

    const QStringList list = m_recentFiles->paths();
    if (list.isEmpty()) {
        m_recentMenu->addAction(tr("(还没有打开过工程)"))->setEnabled(false);
        m_recentBtn->setEnabled(false);
        return;
    }

    for (const QString& p : list) {
        QAction* a = m_recentMenu->addAction(QFileInfo(p).fileName());
        // 菜单只显示文件名(不同目录下同名工程很多),
        // 完整路径放 tooltip —— 不然用户分不清是哪一条
        a->setToolTip(p);
        a->setStatusTip(p);
        // 捕获路径"值"而不是 action 指针:菜单每次弹出都重建,
        // 捕获指针会在下一次重建后指向已释放的 action
        connect(a, &QAction::triggered, this, [this, p] { loadProjectFrom(p); });
    }
    m_recentMenu->addSeparator();
    QAction* clr = m_recentMenu->addAction(tr("清空列表"));
    connect(clr, &QAction::triggered, this, [this] {
        m_recentFiles->clear();
        m_recentBtn->setEnabled(false);
        appendLog(tr("[最近] 列表已清空"));
    });
}

void MainWindow::onSaveProject() {
    QString path = QFileDialog::getSaveFileName(this, tr("保存流水线"),
        QDir(lastUsedDir()).filePath("untitled.sol.json"),
        tr("Mini VM Pipeline (*.sol.json)"));
    if (path.isEmpty()) return;

    QJsonDocument doc = pipelineToJson(m_canvas, m_imagePathEdit->text());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(tr("[Save] ❌ 无法写入: %1").arg(f.errorString()));
        return;
    }
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();

    int nodeCount = m_canvas->nodes().size();
    int edgeCount = m_canvas->connections().size();
    appendLog(tr("[Save] ✅ 已保存 %1 个节点 + %2 条连线到 %3")
        .arg(nodeCount).arg(edgeCount).arg(path));
    statusBar()->showMessage(tr("✅ 已保存: %1").arg(path), 3000);

    // 存过的工程自然算"最近打开过" —— 不管这次是 Save 还是 Save As
    m_recentFiles->add(path);
    m_recentBtn->setEnabled(true);
}

// ============================================================
// P0: Kahn 拓扑排序
// ============================================================
// P0: Kahn 拓扑排序（替代硬编码顺序）
// 返回拓扑序节点列表;若有环,hasCycle=true 且返回空
// ============================================================
static QList<OperatorNode*> topoSort(PipelineView* canvas, bool* hasCycle) {
    *hasCycle = false;
    QList<OperatorNode*> order;
    const auto& nodes = canvas->nodes();
    const auto& conns = canvas->connections();
    if (nodes.isEmpty()) return order;

    // 1. 建图 + 入度统计
    QHash<OperatorNode*, QList<OperatorNode*>> graph;
    QHash<OperatorNode*, int> inDegree;
    for (auto* n : nodes) {
        graph[n] = QList<OperatorNode*>();
        inDegree[n] = 0;
    }
    for (auto* c : conns) {
        OperatorNode* from = c->fromNode();
        OperatorNode* to   = c->toNode();
        if (!graph.contains(from) || !inDegree.contains(to)) continue;
        graph[from].append(to);
        inDegree[to]++;
    }

    // 2. 找所有入度 0 的节点入队
    QQueue<OperatorNode*> q;
    for (auto* n : nodes) {
        if (inDegree.value(n, 0) == 0) q.enqueue(n);
    }

    // 3. BFS 出队
    while (!q.isEmpty()) {
        OperatorNode* n = q.dequeue();
        order.append(n);
        for (OperatorNode* m : graph.value(n)) {
            int d = inDegree[m] - 1;
            inDegree[m] = d;
            if (d == 0) q.enqueue(m);
        }
    }

    // 4. 环检测
    if (order.size() != nodes.size()) {
        *hasCycle = true;
        return {};
    }
    return order;
}


void MainWindow::onRunClicked() {
    // === 重入防护(第一道) ===
    // 这一句必须在最前面,连参数校验都不能做:Run 里的 processEvents()
    // 会把用户的第二次点击派发进来,重入一层就等于两个帧循环叠着跑,
    // 共享同一个画布和日志。实测这类叠跑的表现就是"窗口不动、没有弹窗"。
    if (!w7::beginRun(&m_runState)) {
        appendLog(tr("[Run] 已经在跑了,忽略这次点击(或点 Stop 中止)"));
        return;
    }
    // 从这里到函数结束都必须把状态还原,否则一次早退就把程序
    // 永久锁在"正在跑"状态,之后再也点不动 Run。
    // 所以用一个 RAII 守卫,而不是在每个 return 前手写一遍 ——
    // 手写的话以后再加一个 early return 就漏一处,而漏了的表现是
    // "程序莫名其妙再也不响应 Run",极难查。
    //
    // 守卫要管三样:运行标志、Stop 按钮、Undo/Redo 的启用态。
    // 少管一样就等于留一个"下次 Run 点不动/按钮一直是灰的"的坑。
    struct RunGuard {
        MainWindow* w;
        bool undoWas, redoWas;
        explicit RunGuard(MainWindow* win)
            : w(win),
              undoWas(win->m_undoAction->isEnabled()),
              redoWas(win->m_redoAction->isEnabled()) {
            w->m_stopBtn->setEnabled(true);
            // 跑批期间禁掉 Undo/Redo:拓扑序里是节点裸指针,
            // 中途 Ctrl+Z 删掉节点,循环下一轮就会拿已释放的内存去写。
            w->m_undoAction->setEnabled(false);
            w->m_redoAction->setEnabled(false);
        }
        ~RunGuard() {
            w7::endRun(&w->m_runState);
            w->m_stopBtn->setEnabled(false);
            w->m_undoAction->setEnabled(undoWas);
            w->m_redoAction->setEnabled(redoWas);
        }
    } runGuard(this);

    QString fallbackPath = m_imagePathEdit->text();
    if (fallbackPath.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("请先在工具栏输入图像路径或拖入 image_source 节点"));
        return;
    }

    appendLog("========================================");
    appendLog(tr("[Run] 开始执行  fallback 图路径: %1").arg(fallbackPath));

    // 边界:空画布
    if (m_canvas->nodes().isEmpty()) {
        appendLog(tr("[Run] 画布为空,请先从左侧算子库拖拽节点"));
        statusBar()->showMessage(tr("⚠️ 画布为空"), 3000);
        return;
    }

    // 拓扑排序
    bool hasCycle = false;
    const QList<OperatorNode*> rawOrder = topoSort(m_canvas, &hasCycle);
    if (hasCycle) {
        appendLog(tr("[Run] ❌ 检测到环路!无法执行（%1 个节点未参与拓扑序）")
            .arg(m_canvas->nodes().size() - rawOrder.size()));
        statusBar()->showMessage(tr("❌ 环路"), 5000);
        return;
    }
    // 立刻把裸指针换成 QPointer。
    // 帧循环是长循环,而 Run 期间虽然禁了 Undo/Redo,画布本身还是活的
    // (右键删除、拖节点、Load 工程都还能改场景)。QPointer 会在对象
    // 被销毁时自动置空,于是"节点中途没了"从 use-after-free(堆损坏 →
    // 随机卡死)变成一次干净的跳过。
    // 换成 QPointer 之后每轮开头那次 null 检查就是必需的了,别当冗余删掉。
    QList<QPointer<OperatorNode>> order;
    order.reserve(rawOrder.size());
    for (OperatorNode* n : rawOrder) order.append(QPointer<OperatorNode>(n));

    appendLog(tr("[Run] 拓扑序: %1 个节点,按图执行")
        .arg(order.size()));

    // === 帧循环 ===
    // 图像源可能有多帧(文件夹批量 / 视频)。两者本质是同一件事:
    // 「一个源产出 N 张图,流水线对每张跑一遍」。
    // 先跑第 0 帧拿到 frame_count,再决定要不要继续 ——
    // 所以循环上界是动态的,不是先算好再跑。
    // 引擎通过 inputs["frame_index"] 告诉源"这轮要第几帧"。
    QHash<OperatorNode*, QJsonObject> nodeOutputs;   // 每个节点的输出缓存(本帧内传递用)
    QPointer<OperatorNode> finalNode;               // 同样不用裸指针,理由见上面 order 那段
    QJsonObject finalResult;                        // 保留最后一帧的结果供显示
    int totalFrames = 1;
    int passCount = 0, failCount = 0, frameDone = 0;
    bool cancelled = false;

    // 距上次抽帧的时长,用来给 processEvents 限频。
    // 这个计时器是这次卡死修复的核心之一 —— 以前每个节点无条件
    // processEvents 一次,而 processEvents 会把挂起的重绘真的画出去,
    // 于是"每写一行日志就重排版+重画整个不断变大的日志文档"。
    // 实测单行代价 1545 us(QTextEdit),日志行数 = 帧数 x 节点数,
    // 且单行代价随行数上涨 —— 2000 张 x 8 节点就是几万次整篇重绘,
    // 表现就是"跑着跑着卡死",而且全程不弹任何框。
    QElapsedTimer uiPumpTimer;
    uiPumpTimer.start();
    // 起始值取**负**的间隔,表示"上一次抽帧发生在无限久以前",
    // 于是第 0 帧的第一次检查就会抽帧,用户马上看到节点被点亮。
    //
    // 这里原来写成 +kUiPumpIntervalMs,想着是"预热成够久了",
    // 但下一次算的是 nowMs - lastPumpMs = 2 - 100 = -98 < 100,
    // 于是第 0 帧**不会**抽帧 —— 注释和算式是反的。
    // 是 run_policy 的测试用例 2 抓出来的:那条断言要求第 0 帧就该抽。
    qint64 lastPumpMs = -kUiPumpIntervalMs;

    // === 清掉上一次的每步缓存和状态点 ===
    // 不清的话:用户改了参数没重跑,右键看到的是旧结果,
    // 还会以为"我明明调了阈值怎么没用" —— 比看不到更误导。
    clearStepCache();
    for (const auto& n : order) {
        if (n) n->setStepStatus(OperatorNode::StepStatus::None);
    }

    for (int frame = 0; frame < totalFrames; ++frame) {
        // === 取消检查 ===
        // 放在帧边界而不是节点边界:Stop 的语义是"跑完这一帧就收工",
        // 在节点中间硬停会让这一帧的输出半新半旧,汇总数字反而对不上。
        if (m_runState == w7::RunState::Cancelling) {
            cancelled = true;
            appendLog(tr("[Run] 🛑 已中止,完成 %1 帧").arg(frameDone));
            break;
        }

        nodeOutputs.clear();
        finalNode = nullptr;
        QJsonObject frameResult;
        appendLog(tr("──────── 第 %1/%2 帧 ────────")
                  .arg(frame + 1).arg(qMax(totalFrames, frame + 1)));

        for (int step = 0; step < order.size(); ++step) {
        // 节点可能已经不在了(跑批期间右键删了/拖了/Load 了)。
        // QPointer 在这里自动变 null,跳过就行 —— 换成裸指针就是
        // 往已释放内存上写,堆一坏,表现是随机的卡死或闪退。
        if (!order[step]) {
            appendLog(tr("  ⚠️ 第 %1 步的节点在运行中被删除,跳过").arg(step + 1));
            continue;
        }
        OperatorNode* n = order[step].data();
        m_canvas->highlightNode(n->opName(), true);

        // === UI 抽帧限频 ===
        // 按时间限频,不是每节点都刷。再快也最多 10 次/秒。
        // 副作用是好的:一帧之内的多次日志写入会合并成一次重绘,
        // 而不是每个节点重画一遍整个日志。
        const qint64 nowMs = uiPumpTimer.elapsed();
        if (w7::shouldPumpUi(nowMs - lastPumpMs)) {
            lastPumpMs = nowMs;
            // 让用户看得见在动 —— "看不出在动"和"卡死了"在体感上是一回事。
            statusBar()->showMessage(tr("运行中… 第 %1/%2 帧")
                .arg(frame + 1).arg(qMax(totalFrames, frame + 1)));
            QApplication::processEvents();
        }

        // === 构造 inputs:优先合并所有前驱节点 outputs(后写覆盖) ===
        // === B 方案:不再硬塞 image_path;若上游没产 image_path 才 fallback 工具栏 ===
        QJsonObject inputs;
        for (const auto* c : m_canvas->connections()) {
            if (c->toNode() == n && nodeOutputs.contains(c->fromNode())) {
                const QJsonObject& upOut = nodeOutputs[c->fromNode()];
                for (auto it = upOut.begin(); it != upOut.end(); ++it) {
                    inputs[it.key()] = it.value();   // 简单合并:后写覆盖
                }
            }
        }
        // 兜底:若上游没产出 image_path,fallback 到工具栏
        if (!inputs.contains("image_path")) {
            inputs["image_path"] = fallbackPath;
        }
        // 帧索引:源算子据此取第几帧/第几张图,下游算子忽略这个键
        inputs["frame_index"] = frame;
        // 节点身份:算子据此生成**实例唯一**的中间文件名。
        // Run 引擎只传字符串路径,所以算子之间靠"写文件 → 下游 imread"
        // 传数据;文件名写死的话,一条流水线里两个同类型算子会互相覆盖,
        // 下游读到的是别人的结果 —— 不崩不报错,数就是错的。
        // (color_segment 的 mask 就是这么错的。)
        inputs["node_uuid"] = n->uuid().toString(QUuid::WithoutBraces);

        OperatorBase* op = w7::createOperatorByName(n->opName());
        if (!op) {
            appendLog(tr("  ❌ 算子 %1 未注册,跳过").arg(n->opName()));
            m_canvas->highlightNode(n->opName(), false);
            n->setStepStatus(OperatorNode::StepStatus::Error);
            continue;
        }

        // 用节点的参数(优先 node 自己存的,fallback 到默认)
        QJsonObject params = n->params();
        if (params.isEmpty()) params = op->defaultParams();

        QJsonObject out = op->run(params, inputs);
        nodeOutputs[n] = out;
        finalNode = n;
        if (out.contains("result")) frameResult = out;

        // ============================================================
        // === 每步结果查看:把这一步的输出留下 ===
        //
        // 只留最后一帧。多帧时中间帧的结果没有保留价值 ——
        // 用户右键要回答的是"这条流水线现在为什么这么判",
        // 而末帧就是流水线当前的产出;m_stepCacheFrame 记着来自第几帧。
        // ============================================================
        if (frame == totalFrames - 1 || totalFrames == 1) {
            m_stepOutputs[n->uuid()] = out;
            m_stepLabels[n->uuid()]  = tr("第 %1 步 · %2")
                                        .arg(step + 1).arg(n->displayName());
            n->setHasCachedResult(true);
        }

        // 画布状态点:有 error 是橙,result 是 FAIL/OK 是红/绿,
        // 中间算子(分割/计数)没有 result 字段,只要没 error 就是绿的
        // —— 它们确实跑成功了,只是这一级不做判定。
        n->setStepStatus(OperatorNode::statusFromOutput(out));

        // 日志输出关键字段(沿用旧格式)
        QString summary;
        for (auto it = out.begin(); it != out.end(); ++it) {
            if (it.key() == "error") {
                appendLog(tr("  ❌ %1: %2").arg(n->opName(), it.value().toString()));
            } else {
                summary += QString("%1=%2 ").arg(it.key()).arg(it.value().toVariant().toString());
            }
        }
        if (!summary.isEmpty()) {
            appendLog(tr("  ✅ %1 → %2").arg(n->opName(), summary.trimmed()));
        }

        delete op;
        m_canvas->highlightNode(n->opName(), false);
        }   // ← 节点循环结束

        // ---------- 本帧收尾 ----------
        if (!frameResult.contains("result") && finalNode) {
            frameResult = nodeOutputs.value(finalNode);
        }
        const QString r = frameResult.value("result").toString();
        if (r == "PASS")      ++passCount;
        else if (r == "FAIL") ++failCount;
        ++frameDone;

        finalResult = frameResult;   // 留给后面的结果图显示

        // 首帧跑完才知道一共有多少帧 —— 循环上界因此是动态的。
        //
        // clampFrameCount 是这次卡死修复的入口闸门:
        // 以前这里直接把源报的 frame_count 当循环上界,而文件夹源报的
        // 是目录里的文件数。指向一个大目录就是几万轮同步跑,而且日志
        // 行数 = 帧数 x 节点数,每行都要重画一次日志 ——
        // 这条链就是用户报的"跑着跑着卡死"。
        //
        // 截断了必须在日志里说清楚,不然用户会以为全跑完了。
        if (frame == 0) {
            for (auto it = nodeOutputs.constBegin(); it != nodeOutputs.constEnd(); ++it) {
                if (it.value().contains("frame_count")) {
                    const int reported = it.value().value("frame_count").toInt(1);
                    bool truncated = false;
                    totalFrames = w7::clampFrameCount(reported, &truncated);
                    if (truncated) {
                        appendLog(tr("⚠️ 一共有 %1 帧,超过单次上限 %2 帧,只跑前 %2 帧")
                            .arg(reported).arg(w7::kMaxRunFrames));
                        appendLog(tr("   要跑全部就分批,或者把上限调高(kMaxRunFrames)"));
                    }
                    break;
                }
            }
            statusBar()->showMessage(tr("共 %1 帧").arg(totalFrames), 3000);
        }
    }   // ← 帧循环结束

    // 记下缓存来自第几帧,右键回看时标题里要能说清
    m_stepCacheFrame = frameDone;

    // === 多帧汇总 ===
    // 只看最后一帧的 PASS/FAIL 是不够的:抽检 30 帧里 1 帧有问题,
    // 末帧正常的话单看末帧就是漏检。产线的判废看的是整体,不是抽一张。
    //
    // 中止的批次**不能**报"全批 OK":用户 Stop 掉一个 2000 帧的批次,
    // 恰好前 3 帧都 PASS,报"全批 3 帧判 OK"看起来像整批合格,
    // 而剩下 1997 帧根本没跑 —— 这是个会误导判废的假结论。
    // 所以中止时明说"跑了一部分,结论不完整"。
    if (totalFrames > 1) {
        appendLog("----------------------------------------");
        appendLog(tr("📊 批量汇总: 共 %1 帧 — PASS %2 / FAIL %3")
            .arg(frameDone).arg(passCount).arg(failCount));
        if (cancelled) {
            appendLog(tr("🛑 本批被手动中止:只跑完 %1/%2 帧,**这不是整批结论**")
                .arg(frameDone).arg(totalFrames));
            statusBar()->showMessage(tr("🛑 已中止(%1/%2 帧)")
                .arg(frameDone).arg(totalFrames), 5000);
        } else if (failCount > 0) {
            appendLog(tr("⚠️ 有 %1 帧判废,整批判 NG").arg(failCount));
            statusBar()->showMessage(tr("❌ 批量 FAIL (%1/%2 帧)")
                .arg(failCount).arg(frameDone), 5000);
        } else {
            appendLog(tr("✅ 全批 %1 帧判 OK").arg(frameDone));
            statusBar()->showMessage(tr("✅ 批量 PASS (%1 帧)").arg(frameDone), 5000);
        }
    }

    // === 最终结果判定(优先 finalResult,否则取最后节点的 output) ===
    // 多帧时上面已经报过批量汇总了,这里只补一句"末帧是什么",
    // 不再刷一遍 PASS/FAIL —— 两处都报会让人以为程序判了两次。
    if (!finalResult.contains("result") && finalNode) {
        finalResult = nodeOutputs.value(finalNode);
    }
    QString result = finalResult.value("result").toString();
    if (result.isEmpty()) result = "UNKNOWN";
    if (totalFrames <= 1) {
        appendLog("----------------------------------------");
        if (result == "PASS") {
            appendLog(tr("🎉 最终结果: PASS (%1)").arg(
                finalResult.value("description").toString()));
            statusBar()->showMessage(tr("✅ PASS"), 5000);
        } else if (result == "FAIL") {
            appendLog(tr("❌ 最终结果: FAIL (%1)").arg(
                finalResult.value("description").toString()));
            statusBar()->showMessage(tr("❌ FAIL"), 5000);
        } else {
            appendLog(tr("ℹ️  最终结果: %1").arg(result));
        }
    } else {
        appendLog(tr("ℹ️  末帧(%1)结果: %2").arg(frameDone).arg(result));
    }

    // === P3: 根据最后节点 output 决定显示什么 ===
    // 真正的挑选逻辑抽成了 renderOutputToResultPanel ——
    // 「查看该步输出」要显示的是同一套东西,两份各写一份必然跑偏。
    renderOutputToResultPanel(finalResult, fallbackPath, tr("最终结果"));

    appendLog("[Run] 完成");
    appendLog("========================================");
}

// ============================================================
// === 每步结果查看 ===
// ============================================================

void MainWindow::clearStepCache() {
    m_stepOutputs.clear();
    m_stepLabels.clear();
    m_stepCacheFrame = 0;
}

void MainWindow::onViewStepResultRequested(OperatorNode* node) {
    if (!node) return;
    const QUuid id = node->uuid();
    if (!m_stepOutputs.contains(id)) {
        // 理论上右键菜单已经置灰了,这里再兜一次 ——
        // 菜单和信号之间隔着事件循环,中间状态可能已经变了
        appendLog(tr("[每步] %1 没有缓存结果,请先 Run").arg(node->displayName()));
        return;
    }
    QString title = m_stepLabels.value(id, node->displayName());
    // 多帧时必须标出这是第几帧的结果,否则用户会以为是第一帧
    if (m_stepCacheFrame > 1) {
        title += tr("  (第 %1/%2 帧)").arg(m_stepCacheFrame);
    }
    renderOutputToResultPanel(m_stepOutputs.value(id), m_imagePathEdit->text(), title);
    appendLog(tr("[每步] 查看 %1").arg(title));
}

// 把 output 渲染到结果区:挑一张最能说明问题的图 + 写全部字段
bool MainWindow::renderOutputToResultPanel(const QJsonObject& out,
                                           const QString& fallbackPath,
                                           const QString& title) {
    m_resultTitle->setText(title);
    showStepDetail(title, out);

    // 算子自己报错了 —— 结果图没有意义,直接把错误摆出来。
    // (以前这种情况会掉到最后的 fallback 分支,显示一张原图,
    //  看上去"图出来了、挺正常",但其实这一步压根没跑成。)
    if (out.contains("error")) {
        m_resultImg->setPixmap(QPixmap());
        m_resultImg->setText(tr("⚠️ 该步执行失败\n\n%1").arg(out.value("error").toString()));
        return false;
    }

    QPixmap pix;
    bool rendered = false;

    // 优先级说明 —— 这里写的是代码的实际行为,不是理想行为:
    //   1) mask_path        color_segment 的分割图
    //   2) 模板框            template_match
    //   3) image_path + PASS/FAIL 大字
    // 判定结果有大字图时会盖掉前两张,这是有意的:
    // 结果面板首先回答"判没判废",其次才是"为什么"。
    // 以前这段是"先画 mask,再无条件用大字图盖掉",而日志只在
    // 没画过时记一行 —— 于是日志写着 mask、屏幕上却是大字图,对不上。

    // 1) mask 图
    const QString maskPath = out.value("mask_path").toString();
    if (!maskPath.isEmpty() && QFile::exists(maskPath)) {
        pix.load(maskPath);
        if (!pix.isNull()) rendered = true;
    }

    // 2) 模板位置红框
    if (!rendered && out.contains("position_x") && out.contains("template_w")) {
        QString imgPath = out.value("image_path").toString();
        if (imgPath.isEmpty()) imgPath = fallbackPath;
        QImage srcImg(imgPath);
        if (!srcImg.isNull()) {
            QPainter painter(&srcImg);
            painter.setPen(QPen(QColor(0, 0, 255), 3));  // 蓝框
            int x = out.value("position_x").toInt();
            int y = out.value("position_y").toInt();
            int w = out.value("template_w").toInt();
            int h = out.value("template_h").toInt();
            painter.drawRect(x, y, w, h);
            double score = out.value("score").toDouble();
            painter.setPen(Qt::red);
            painter.setFont(QFont("Arial", 14, QFont::Bold));
            painter.drawText(x, qMax(0, y - 8),
                QString("score=%1").arg(score, 0, 'f', 3));
            painter.end();
            pix = QPixmap::fromImage(srcImg);
            rendered = true;
        }
    }

    // 3) 原图 + PASS/FAIL 大字
    const QString verdict = out.value("result").toString();
    if (verdict == "PASS" || verdict == "FAIL") {
        QString imgPath = out.value("image_path").toString();
        if (imgPath.isEmpty()) imgPath = fallbackPath;
        QImage srcImg(imgPath);
        if (!srcImg.isNull()) {
            QPainter painter(&srcImg);
            QColor c = (verdict == "PASS") ? QColor(0, 200, 0) : QColor(255, 0, 0);
            painter.setPen(QPen(c, 6));
            painter.setFont(QFont("Arial", 120, QFont::Bold));
            painter.drawText(50, 200, verdict);
            painter.setFont(QFont("Arial", 28));
            painter.drawText(50, 250, out.value("description").toString());
            painter.end();
            pix = QPixmap::fromImage(srcImg);
            rendered = true;
        }
    }

    // 4) fallback 原图(源算子 / 中间算子大多走这里)
    if (!rendered) {
        QString imgPath = out.value("image_path").toString();
        if (imgPath.isEmpty()) imgPath = fallbackPath;
        pix.load(imgPath);
        rendered = !pix.isNull();
        if (!rendered) {
            // 这一步没有图可显示(纯数值算子),但字段照常显示在详情框里,
            // 不要用占位文字把图区盖住 —— 数值才是这类算子的全部产出
            m_resultImg->setPixmap(QPixmap());
            m_resultImg->setText(tr("(该步不产生图像,见下方输出字段)"));
        }
    }

    if (rendered) {
        m_resultImg->setText(QString());
        m_resultImg->setPixmap(pix.scaled(m_resultImg->width(),
                                          m_resultImg->height(),
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
    }
    return rendered;
}

void MainWindow::showStepDetail(const QString& title, const QJsonObject& out) {
    if (out.isEmpty()) {
        m_stepDetail->setPlainText(tr("(无输出)"));
        return;
    }
    QStringList lines;
    lines << tr("── %1 ──").arg(title);
    // 按 key 排序:输出字段是 QJsonObject,本身无序,
    // 不排序的话每次显示的行顺序都可能不一样,肉眼对不上。
    for (auto it = out.constBegin(); it != out.constEnd(); ++it) {
        QString v = it.value().toVariant().toString();
        // 长文本(比如缺陷清单 JSON)换行显示,挤成一行横向拉不到底
        if (v.size() > 80) {
            lines << QString("%1 =").arg(it.key());
            lines << QString(v);
        } else {
            lines << QString("%1 = %2").arg(it.key(), v);
        }
    }
    m_stepDetail->setPlainText(lines.join('\n'));
}

QJsonObject MainWindow::runOperatorByName(const QString&, const QJsonObject&, const QJsonObject&) {
    return {};
}

void MainWindow::appendLog(const QString& msg) {
    QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logView->appendPlainText(QString("[%1] %2").arg(ts, msg));
}

// ============================================================
// === M6 撤销/重做 ===
// ============================================================

void MainWindow::execCommand(Command* cmd) {
    if (!cmd) return;
    // === 第二道重入/悬垂防护 ===
    // 禁用 Undo/Redo 按钮只挡住了快捷键那一路。画布本身在 Run 期间
    // 仍然是活的:右键删节点、拖节点、拉连线都会走到这里。
    // 帧循环正拿着这些节点的指针在跑,中途删掉就是 use-after-free,
    // 堆一坏的表现是"随机卡死/必须杀进程",而且和触发它的操作对不上号,
    // 极难查。所以在真正改场景之前就统一拦掉。
    if (w7::isRunning(m_runState)) {
        appendLog(tr("[Cmd] 正在运行中,已忽略这次改动(先点 Stop)"));
        delete cmd;
        return;
    }
    cmd->exec();                                  // 执行
    m_undoStack.push(cmd);                        // 压入 undoStack
    m_redoStack.clear();                          // 新操作清空 redoStack
    appendLog(tr("[Cmd] %1 (undoStack=%2)")
        .arg(cmd->desc()).arg(m_undoStack.size()));
    updateUndoRedoActions();
}

void MainWindow::execUndo() {
    if (m_undoStack.isEmpty()) {
        appendLog(tr("[Undo] 已是最初状态,无法再撤销"));
        return;
    }
    // undo 会改节点位置 → 触发 nodeMoved 信号。
    // 打标志让那批事件不落进命令栈,否则一次 Ctrl+Z 会连带压回一堆 MoveNodeCmd。
    m_applyingHistory = true;
    Command* cmd = m_undoStack.pop();
    cmd->undo();
    m_redoStack.push(cmd);
    m_applyingHistory = false;
    appendLog(tr("[Undo] %1 (undoStack=%2, redoStack=%3)")
        .arg(cmd->desc()).arg(m_undoStack.size()).arg(m_redoStack.size()));
    updateUndoRedoActions();
}

void MainWindow::execRedo() {
    if (m_redoStack.isEmpty()) {
        appendLog(tr("[Redo] 已是最末状态,无法再重做"));
        return;
    }
    m_applyingHistory = true;
    Command* cmd = m_redoStack.pop();
    cmd->redo();
    m_undoStack.push(cmd);
    m_applyingHistory = false;
    appendLog(tr("[Redo] %1 (undoStack=%2, redoStack=%3)")
        .arg(cmd->desc()).arg(m_undoStack.size()).arg(m_redoStack.size()));
    updateUndoRedoActions();
}

void MainWindow::clearUndoRedoStacks() {
    // Command 由栈持有(delete),栈本身是 QStack 不析构元素 → 必须手动清
    qDeleteAll(m_undoStack);
    m_undoStack.clear();
    qDeleteAll(m_redoStack);
    m_redoStack.clear();
    updateUndoRedoActions();
}

void MainWindow::updateUndoRedoActions() {
    if (m_undoAction) m_undoAction->setEnabled(!m_undoStack.isEmpty());
    if (m_redoAction) m_redoAction->setEnabled(!m_redoStack.isEmpty());
}

// ============================================================
// === M6: 各操作入口(统一在这里包 Command) ===
// ============================================================

void MainWindow::doAddNode(const QString& opName, const QString& display,
                           const QString& category, const QPointF& pos) {
    execCommand(new w7::AddNodeCmd(m_canvas, opName, display, category, pos));
}

void MainWindow::doDeleteNode(w7::OperatorNode* node) {
    if (!node) return;
    // 先解绑参数面板,否则 m_currentParamNode 会指向已删节点
    QString desc = QString("%1(%2)").arg(node->displayName(), node->opName());
    if (m_currentParamNode == node) {
        m_currentParamNode = nullptr;
        m_currentParamWidget = nullptr;
        m_paramPanel->setCurrentIndex(0);   // 切回 placeholder
    }
    if (m_selectedNode == node) m_selectedNode = nullptr;

    execCommand(new w7::DeleteNodeCmd(m_canvas, node));
    appendLog(tr("[删除] %1").arg(desc));
}

void MainWindow::doMoveNode(w7::OperatorNode* node, const QPointF& oldPos,
                            const QPointF& newPos) {
    if (!node) return;
    if (m_applyingHistory) return;   // undo/redo 自身引起的位置变化,不记录
    execCommand(new w7::MoveNodeCmd(m_canvas, node->uuid(), oldPos, newPos));
}

void MainWindow::doConnect(w7::OperatorNode* from, w7::OperatorNode* to,
                           w7::OperatorNode::PortSide fromSide,
                           w7::OperatorNode::PortSide toSide) {
    if (!from || !to) return;

    // ============================================================
    // === pin 类型检查 ===
    //
    // 只拦"用户主动连线"这一条路。加载旧工程 / 撤销重做走的是
    // PipelineView::addConnection,那边不查 —— 那些是恢复既有状态,
    // 不是在做新决策。旧工程里存着当年合法的连法,不该因为今天
    // 加了类型规则就打不开。
    // ============================================================
    w7::OperatorBase* fromOp = w7::createOperatorByName(from->opName());
    w7::OperatorBase* toOp   = w7::createOperatorByName(to->opName());
    if (fromOp && toOp) {
        const QString why = w7::checkPinCompatibility(
            fromOp->outputPinType(), toOp->inputPinType(),
            toOp->isSource(),
            from->displayName(), to->displayName(),
            from == to);
        if (!why.isEmpty()) {
            QMessageBox::warning(this, tr("不能连线"), why);
            appendLog(tr("[连线] ❌ 拒绝 %1 → %2: %3")
                .arg(from->opName(), to->opName(),
                     why.split('\n').value(0)));
            delete fromOp;
            delete toOp;
            return;
        }
    }
    delete fromOp;
    delete toOp;

    // === M7: AddEdgeCmd 携带 fromSide/toSide ===
    execCommand(new w7::AddEdgeCmd(m_canvas, from->uuid(), to->uuid(),
                                   fromSide, toSide));
}

// ============================================================
// === M6: 画布信号转发 ===
// ============================================================

void MainWindow::onNodeMoved(w7::OperatorNode* node, const QPointF& oldPos,
                             const QPointF& newPos) {
    doMoveNode(node, oldPos, newPos);
}

// ============================================================
// === P0: 节点右键删除请求 ===
// ============================================================
// 节点自己不能 delete(PipelineView::m_nodes 仍持有该指针 → 悬垂指针
// → 右键删完立刻 Run 会段错误)。这里统一走 PipelineView::removeNode,
// 它已经实现了正确的顺序:收集关联线 → 从容器和 scene 移除 → 统一 delete。
void MainWindow::onNodeDeleteRequested(w7::OperatorNode* node) {
    // === UUID 必须在删除【之前】取 ===
    // doDeleteNode 走 DeleteNodeCmd → PipelineView::removeNode,
    // 里面最后一行就是 delete node。node 从这一刻起是悬垂指针,
    // 后面再读 node->uuid() 是 use-after-free。
    //
    // 原来的写法是 `if (node) m_stepOutputs.remove(node->uuid());` ——
    // if 只挡住了 nullptr,挡不住"已释放"。这条路径是右键删节点,
    // 是最常走的一条删除入口。
    if (!node) return;
    const QUuid id = node->uuid();   // ← 先取值

    // === M6: 走 Command 栈,这样右键删也能 Ctrl+Z 撤销 ===
    doDeleteNode(node);              // ← node 在这里没了

    // === 每步结果查看:顺手把这一步的缓存摘掉 ===
    // 不摘也不会有人点到(uuid 唯一,菜单走的是活节点),
    // 但留着就是个永远查不到的条目,工程反复删了几十次会攒不少。
    m_stepOutputs.remove(id);
}

// ============================================================
// === 修复 1+2: Delete 键删除选中项(安全版) ===
// ============================================================
void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        deleteSelectedItems();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::deleteSelectedItems() {
    if (!m_canvas || !m_canvas->scene()) return;

    QList<QGraphicsItem*> selected = m_canvas->scene()->selectedItems();
    if (selected.isEmpty()) {
        appendLog(tr("[Delete] 未选中任何项(先点击要删除的节点/连线)"));
        return;
    }

    // === 修复 1: 收集 + 统一删除(避免循环里 delete 触发回调) ===
    QList<OperatorNode*>   nodesToDelete;
    QList<ConnectionLine*> connsToDelete;

    // 第一遍:分类 selected
    for (auto* item : selected) {
        if (auto* line = dynamic_cast<ConnectionLine*>(item)) {
            connsToDelete.append(line);
        } else if (auto* node = dynamic_cast<OperatorNode*>(item)) {
            nodesToDelete.append(node);
        }
    }

    // 第二遍:把"涉及待删节点"的连线也加入
    // (它们最终由 removeNode 连带删除,不走单独的 RemoveEdgeCmd)
    for (auto* line : m_canvas->connections()) {
        if (!line) continue;
        if (nodesToDelete.contains(line->fromNode()) ||
            nodesToDelete.contains(line->toNode())) {
            if (!connsToDelete.contains(line)) connsToDelete.append(line);
        }
    }

    // ============================================================
    // === 关键:计划必须在任何 delete 之前算完 ===
    //
    // doDeleteNode → PipelineView::removeNode,最后一行是 `delete node`,
    // 它的所有连线在同一次调用里被 qDeleteAll 掉。
    //
    // 原来的代码是"先删节点,再遍历连线",第二遍读 line->fromNode()
    // 读的是**已经释放的对象** —— 货真价实的 use-after-free。
    // 平时不崩只是因为 Release 下堆内存不填毒,还留着旧指针值;
    // 等那块内存被复用,现场就没了。
    //
    // planSelectDelete 里把指针全读成 QUuid,执行阶段一个裸指针都不碰。
    // ============================================================
    const w7::SelectDeletePlan plan =
        w7::planSelectDelete(nodesToDelete, connsToDelete, m_canvas->connections());

    const int delNodes = plan.nodes.size();
    const int delConns = plan.totalEdges();
    if (plan.isEmpty()) {
        appendLog(tr("[Delete] 未选中任何有效项"));
        return;
    }

    // === M6: 走 Command 栈,Delete 键删除也能 Ctrl+Z 撤销 ===
    // 顺序:先删节点(DeleteNodeCmd 自己会连带删线并记录,供 undo 重建),
    // 再删"没沾任何待删节点"的独立连线。
    // 按 UUID 回查节点,而不是直接用遍历时拿到的裸指针 ——
    // 前一个节点被删时不会影响后一个的查找,而裸指针列表
    // 里混着已经失效的地址。
    for (const QUuid& id : plan.nodes) {
        if (OperatorNode* n = m_canvas->findNodeByUuid(id)) doDeleteNode(n);
    }
    for (const auto& e : plan.edges) {
        execCommand(new w7::RemoveEdgeCmd(m_canvas, e.from, e.to, e.fromSide, e.toSide));
    }
    appendLog(tr("[Delete] 删除 %1 个节点,%2 条连线").arg(delNodes).arg(delConns));
}

} // namespace w7
