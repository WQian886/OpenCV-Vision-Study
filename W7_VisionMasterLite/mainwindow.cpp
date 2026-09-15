#include "mainwindow.h"
#include "operator_node.h"
#include "connection_line.h"

#include <QDockWidget>
#include <QToolBar>
#include <QStatusBar>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QTextEdit>
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
#include "operators/operator_factory.h"  // P1: for createOperatorByName

namespace w7 {

// ============================================================
// P1: Save/Load JSON 流水线 helpers
// JSON 格式:
//   { version, name, image_path, nodes:[{op, display, category, x, y, params}], edges:[{from, to}] }
//   节点用 op 名 + 出现顺序匹配 edges (FIFO)
// ============================================================

static QJsonDocument pipelineToJson(PipelineView* canvas, const QString& imagePath) {
    QJsonObject root;
    root["version"]    = "1.0";
    root["name"]       = "untitled";
    root["image_path"] = imagePath;

    // 序列化所有节点
    QJsonArray nodesJson;
    for (auto* n : canvas->nodes()) {
        QJsonObject nj;
        nj["op"]       = n->opName();
        nj["display"]  = n->displayName();
        nj["category"] = n->category();
        nj["x"]        = n->pos().x();
        nj["y"]        = n->pos().y();
        nj["params"]   = n->params();
        nodesJson.append(nj);
    }
    root["nodes"] = nodesJson;

    // 序列化所有边（用 op 名引用,避免节点指针失效）
    QJsonArray edgesJson;
    for (auto* c : canvas->connections()) {
        QJsonObject ej;
        ej["from"] = c->fromNode()->opName();
        ej["to"]   = c->toNode()->opName();
        edgesJson.append(ej);
    }
    root["edges"] = edgesJson;

    return QJsonDocument(root);
}

static bool pipelineFromJson(PipelineView* canvas, const QJsonDocument& doc,
                             std::function<void(const QString&)> logFn,
                             std::function<OperatorBase*(const QString&)> factoryFn,
                             QString& errOut) {
    if (!doc.isObject()) { errOut = "JSON 根不是对象"; return false; }
    QJsonObject root = doc.object();
    if (!root.contains("nodes") || !root["nodes"].isArray()) {
        errOut = "缺少 nodes 数组";
        return false;
    }

    canvas->clearPipeline();

    // 1. 按数组顺序重建节点,记录 op 名 → 节点指针的 FIFO 列表
    QHash<QString, QList<OperatorNode*>> opIndex;
    QJsonArray nodesJson = root["nodes"].toArray();
    int created = 0, skipped = 0;
    for (const auto& v : nodesJson) {
        QJsonObject nj = v.toObject();
        QString opName = nj["op"].toString();
        OperatorBase* op = factoryFn(opName);
        if (!op) {
            logFn(QString("[Load] 跳过未知算子: %1").arg(opName));
            ++skipped;
            continue;
        }
        // 用 JSON 里的 display/category（没有则用算子默认）
        QString display  = nj.value("display").toString(op->displayName());
        QString category = nj.value("category").toString(op->category());
        double x = nj.value("x").toDouble(100);
        double y = nj.value("y").toDouble(200);
        OperatorNode* node = canvas->addNode(opName, display, category, {x, y});
        // 恢复参数
        QJsonObject params = nj.value("params").toObject();
        if (!params.isEmpty()) node->setParams(params);
        opIndex[opName].append(node);
        delete op;  // factory new 出来的临时实例
        ++created;
    }

    // 2. 重建连线（FIFO 匹配）
    QJsonArray edgesJson = root.value("edges").toArray();
    int linked = 0, edgeSkip = 0;
    for (const auto& v : edgesJson) {
        QJsonObject ej = v.toObject();
        QString fromOp = ej["from"].toString();
        QString toOp   = ej["to"].toString();
        if (!opIndex.contains(fromOp) || opIndex[fromOp].isEmpty()) {
            logFn(QString("[Load] 边的起点缺失: %1").arg(fromOp));
            ++edgeSkip;
            continue;
        }
        if (!opIndex.contains(toOp) || opIndex[toOp].isEmpty()) {
            logFn(QString("[Load] 边的终点缺失: %1").arg(toOp));
            ++edgeSkip;
            continue;
        }
        OperatorNode* fromN = opIndex[fromOp].takeFirst();
        OperatorNode* toN   = opIndex[toOp].takeFirst();
        canvas->addConnection(fromN, toN);
        ++linked;
    }

    logFn(QString("[Load] 完成: 创建 %1 个节点(跳过 %2),连线 %3 条(跳过 %4)")
        .arg(created).arg(skipped).arg(linked).arg(edgeSkip));
    return true;
}

// ============================================================
// 算子工厂:见 operators/operator_factory.cpp 的 w7::createOperatorByName
// ============================================================
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Mini VisionMaster Lite — W7"));
    resize(1400, 900);

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
        registry.insert(op.name, m);
    }
    m_canvas->setOperatorRegistry(registry);

    // === S3: 接画布选中变化 + 参数面板应用按钮 ===
    connect(m_canvas, &w7::PipelineView::selectionChangedInScene,
            this, &MainWindow::onSceneSelectionChanged);

    loadDefaultPipeline();

    statusBar()->showMessage(tr("就绪。点击 ▶ Run 运行默认流水线 | 从左侧算子库拖到画布创建节点 | 点击节点切参数面板"));
}

MainWindow::~MainWindow() {
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
    QPushButton* loadBtn = new QPushButton(tr("Load"), this);
    QPushButton* saveBtn = new QPushButton(tr("Save"), this);
    tb->addWidget(runBtn);
    tb->addWidget(loadBtn);
    tb->addWidget(saveBtn);

    tb->addSeparator();
    tb->addWidget(new QLabel(tr(" 图像: "), this));
    m_imagePathEdit = new QLineEdit("images/baseline.png", this);
    m_imagePathEdit->setMinimumWidth(280);
    tb->addWidget(m_imagePathEdit);

    connect(runBtn,  &QPushButton::clicked, this, &MainWindow::onRunClicked);
    connect(loadBtn, &QPushButton::clicked, this, &MainWindow::onLoadProject);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveProject);
}

void MainWindow::registerOperators() {
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

    // 按类别分组
    QTreeWidgetItem* positioning    = new QTreeWidgetItem(m_opLib, {tr("定位 Positioning")});
    QTreeWidgetItem* measurement    = new QTreeWidgetItem(m_opLib, {tr("测量 Measurement")});
    QTreeWidgetItem* identification = new QTreeWidgetItem(m_opLib, {tr("识别 Identification")});
    QTreeWidgetItem* detection      = new QTreeWidgetItem(m_opLib, {tr("检测 Detection")});

    for (const auto& op : m_operatorInfo) {
        QTreeWidgetItem* parent = nullptr;
        if      (op.category == "positioning")    parent = positioning;
        else if (op.category == "measurement")    parent = measurement;
        else if (op.category == "identification") parent = identification;
        else if (op.category == "detection")      parent = detection;
        if (parent) {
            auto* item = new QTreeWidgetItem(parent, {op.display});
            item->setData(0, Qt::UserRole, op.name);
            item->setToolTip(0, op.description);
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

    // 图像预览
    m_resultImg = new QLabel(tr("(运行后显示标注图)"));
    m_resultImg->setAlignment(Qt::AlignCenter);
    m_resultImg->setMinimumHeight(180);
    m_resultImg->setStyleSheet("background:#222; color:#ddd; border:1px solid #555;");
    layout->addWidget(m_resultImg);

    // 日志
    m_logView = new QTextEdit;
    m_logView->setReadOnly(true);
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

    auto* n1 = m_canvas->addNode("color_segment", tr("颜色分割"),  "detection",    {150, 200});
    auto* n2 = m_canvas->addNode("blob_count",    tr("Blob 计数"),  "detection",    {400, 200});
    auto* n3 = m_canvas->addNode("count_check",   tr("数量判定"),  "detection",    {650, 200});
    auto* n4 = m_canvas->addNode("qr_decode",     tr("QR 解码"),   "identification",{150, 380});

    // === S2: 用 ConnectionLine 创建贝塞尔连线 ===
    m_canvas->addConnection(n1, n2);
    m_canvas->addConnection(n2, n3);

    m_canvas->scene()->setSceneRect(m_canvas->scene()->itemsBoundingRect().adjusted(-50,-50,50,50));

    appendLog(tr("已加载默认流水线: 颜色分割 → Blob 计数 → 数量判定 (旁路: QR 解码)"));
    appendLog(tr("提示:点击 ▶ Run 执行 / 从节点右侧端口拖到另一节点左侧端口可手动连线"));
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
        "", tr("Mini VM Pipeline (*.json *.sol.json)"));
    if (path.isEmpty()) return;

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
        [](const QString& name) -> OperatorBase* {
            return w7::createOperatorByName(name);
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

    statusBar()->showMessage(tr("✅ 流水线已加载: %1").arg(path), 3000);
}

void MainWindow::onSaveProject() {
    QString path = QFileDialog::getSaveFileName(this, tr("保存流水线"),
        "untitled.sol.json", tr("Mini VM Pipeline (*.sol.json)"));
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
    QString imagePath = m_imagePathEdit->text();
    if (imagePath.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("请先在工具栏输入图像路径"));
        return;
    }

    appendLog("========================================");
    appendLog(tr("[Run] 开始执行  图像: %1").arg(imagePath));

    // 边界:空画布
    if (m_canvas->nodes().isEmpty()) {
        appendLog(tr("[Run] 画布为空,请先从左侧算子库拖拽节点"));
        statusBar()->showMessage(tr("⚠️ 画布为空"), 3000);
        return;
    }

    // 拓扑排序
    bool hasCycle = false;
    QList<OperatorNode*> order = topoSort(m_canvas, &hasCycle);
    if (hasCycle) {
        appendLog(tr("[Run] ❌ 检测到环路!无法执行（%1 个节点未参与拓扑序）")
            .arg(m_canvas->nodes().size() - order.size()));
        statusBar()->showMessage(tr("❌ 环路"), 5000);
        return;
    }
    appendLog(tr("[Run] 拓扑序: %1 个节点,按图执行")
        .arg(order.size()));

    // 每个节点的输出缓存
    QHash<OperatorNode*, QJsonObject> nodeOutputs;
    OperatorNode* finalNode = nullptr;
    QJsonObject finalResult;

    for (OperatorNode* n : order) {
        m_canvas->highlightNode(n->opName(), true);
        QApplication::processEvents();

        // === 构造 inputs:起点 image_path + 所有前驱节点 outputs 合并 ===
        QJsonObject inputs;
        inputs["image_path"] = imagePath;
        for (const auto* c : m_canvas->connections()) {
            if (c->toNode() == n && nodeOutputs.contains(c->fromNode())) {
                const QJsonObject& upOut = nodeOutputs[c->fromNode()];
                for (auto it = upOut.begin(); it != upOut.end(); ++it) {
                    inputs[it.key()] = it.value();   // 简单合并:后写覆盖
                }
            }
        }

        OperatorBase* op = w7::createOperatorByName(n->opName());
        if (!op) {
            appendLog(tr("  ❌ 算子 %1 未注册,跳过").arg(n->opName()));
            m_canvas->highlightNode(n->opName(), false);
            continue;
        }

        // 用节点的参数(优先 node 自己存的,fallback 到默认)
        QJsonObject params = n->params();
        if (params.isEmpty()) params = op->defaultParams();

        QJsonObject out = op->run(params, inputs);
        nodeOutputs[n] = out;
        finalNode = n;
        if (out.contains("result")) finalResult = out;

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
    }

    // === 最终结果判定(优先 finalResult,否则取最后节点的 output) ===
    if (!finalResult.contains("result") && finalNode) {
        finalResult = nodeOutputs.value(finalNode);
    }
    QString result = finalResult.value("result").toString();
    if (result.isEmpty()) result = "UNKNOWN";
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

    // === P3: 根据最后节点 output 决定显示什么 ===
    // 优先级: mask (color_segment) > 模板框 (template_match) > 大字 (count_check) > 原图
    QPixmap pix;
    bool rendered = false;

    // 1) mask 图（color_segment 输出）
    QString maskPath = finalResult.value("mask_path").toString();
    if (!maskPath.isEmpty() && QFile::exists(maskPath)) {
        pix.load(maskPath);
        if (!pix.isNull()) {
            m_resultImg->setPixmap(pix.scaled(m_resultImg->width(),
                                              m_resultImg->height(),
                                              Qt::KeepAspectRatio));
            appendLog(QString("[结果图] mask: %1").arg(maskPath));
            rendered = true;
        }
    }

    // 2) 模板位置红框 (template_match 输出)
    if (!rendered && finalResult.contains("position_x")
                 && finalResult.contains("template_w")) {
        QImage srcImg(imagePath);
        if (!srcImg.isNull()) {
            QPainter painter(&srcImg);
            painter.setPen(QPen(QColor(0, 0, 255), 3));  // 红框
            int x = finalResult.value("position_x").toInt();
            int y = finalResult.value("position_y").toInt();
            int w = finalResult.value("template_w").toInt();
            int h = finalResult.value("template_h").toInt();
            painter.drawRect(x, y, w, h);
            double score = finalResult.value("score").toDouble();
            painter.setPen(Qt::red);
            painter.setFont(QFont("Arial", 14, QFont::Bold));
            painter.drawText(x, qMax(0, y - 8),
                QString("score=%1").arg(score, 0, 'f', 3));
            painter.end();
            pix = QPixmap::fromImage(srcImg);
            m_resultImg->setPixmap(pix.scaled(m_resultImg->width(),
                                              m_resultImg->height(),
                                              Qt::KeepAspectRatio));
            appendLog(QString("[结果图] 模板匹配红框: (%1,%2) %3x%4 score=%5")
                .arg(x).arg(y).arg(w).arg(h).arg(score, 0, 'f', 3));
            rendered = true;
        }
    }

    // 3) PASS/FAIL 大字叠加
    if (result == "PASS" || result == "FAIL") {
        QImage srcImg(imagePath);
        if (!srcImg.isNull()) {
            QPainter painter(&srcImg);
            QColor c = (result == "PASS") ? QColor(0, 200, 0) : QColor(255, 0, 0);
            painter.setPen(QPen(c, 6));
            painter.setFont(QFont("Arial", 120, QFont::Bold));
            painter.drawText(50, 200, result);
            painter.setFont(QFont("Arial", 28));
            painter.drawText(50, 250, finalResult.value("description").toString());
            painter.end();
            pix = QPixmap::fromImage(srcImg);
            m_resultImg->setPixmap(pix.scaled(m_resultImg->width(),
                                              m_resultImg->height(),
                                              Qt::KeepAspectRatio));
            if (!rendered) {
                appendLog(QString("[结果图] %1 大字").arg(result));
                rendered = true;
            }
        }
    }

    // 4) fallback 原图
    if (!rendered) {
        pix.load(imagePath);
        if (!pix.isNull()) {
            m_resultImg->setPixmap(pix.scaled(m_resultImg->width(),
                                              m_resultImg->height(),
                                              Qt::KeepAspectRatio));
        }
    }

    appendLog("[Run] 完成");
    appendLog("========================================");
}

QJsonObject MainWindow::runOperatorByName(const QString&, const QJsonObject&, const QJsonObject&) {
    return {};
}

void MainWindow::appendLog(const QString& msg) {
    QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logView->append(QString("[%1] %2").arg(ts, msg));
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
    QSet<OperatorNode*>   nodesToDelete;
    QSet<ConnectionLine*> connsToDelete;

    // 第一遍:分类 selected
    for (auto* item : selected) {
        if (auto* line = dynamic_cast<ConnectionLine*>(item)) {
            connsToDelete.insert(line);
        } else if (auto* node = dynamic_cast<OperatorNode*>(item)) {
            nodesToDelete.insert(node);
        }
    }

    // 第二遍:把"涉及待删节点"的连线也加入
    for (auto* line : m_canvas->connections()) {
        if (nodesToDelete.contains(line->fromNode()) ||
            nodesToDelete.contains(line->toNode())) {
            connsToDelete.insert(line);
        }
    }

    int delNodes = nodesToDelete.size();
    int delConns = connsToDelete.size();
    if (delNodes == 0 && delConns == 0) {
        appendLog(tr("[Delete] 未选中任何有效项"));
        return;
    }

    // 第三遍:从 scene 和容器移除(不 delete)
    QGraphicsScene* sc = m_canvas->scene();
    for (auto* line : connsToDelete) {
        sc->removeItem(line);
    }
    for (auto* node : nodesToDelete) {
        sc->removeItem(node);
    }

    // 第四遍:统一 delete(此时 scene 已不再引用)
    for (auto* line : connsToDelete) delete line;
    for (auto* node : nodesToDelete) delete node;

    // 第五遍:从 PipelineView 内部 list 清掉
    for (auto* line : connsToDelete) {
        m_canvas->connectionsRef().removeAll(line);
    }
    for (auto* node : nodesToDelete) {
        m_canvas->nodesRef().removeAll(node);
    }

    appendLog(tr("[Delete] 删除 %1 个节点,%2 条连线").arg(delNodes).arg(delConns));
}

} // namespace w7
