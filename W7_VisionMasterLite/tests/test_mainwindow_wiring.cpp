// ============================================================
// W7 主窗口接线 回归测试(headless,offscreen)
// ============================================================
//
// 这是**本项目第一个把 mainwindow.cpp 拉进测试目标**的套件。
//
// ⚠ 2026-10-25 第六笔(P0-2):为什么需要它
// ============================================================
//
//   欠账 17 一直挂着「主窗口行为零测试覆盖」。
//   前四笔把「引擎语义 / 判定口径 / 显示规则 / 帧协议」搬出了主窗口,
//   剩下的确实只是**接线** ——
//   而「接线」以前是**一句注释**:「mainwindow.cpp 不在任何测试目标里」。
//
//   ⚠「没法测」和「没测」是两回事(见 notes/atomic/
//     at-cannot-be-tested-is-a-conclusion-not-a-reason.md)。
//   这一笔就是去把那句话**推翻**:
//     · mainwindow.cpp 链进 w7_app_core,和主程序**同一份实现**
//     · 用 QT_QPA_PLATFORM=offscreen 起一个真的 QApplication
//     · 走**和真程序一模一样的那条路**(invokeMethod 调 private slot,
//       不在测试里另写一份加载逻辑)
//
// ============================================================
// 这个套件测什么:「错了用户会不会自己发现」
// ============================================================
//
//   判据不是「哪些逻辑重要」,是「**用户能不能自己发现**」:
//
//     · **画面明显画错的** —— 用户自己看得见,不必测。
//       (节点画歪了、窗口尺寸不对、按钮没排好 —— 这些肉眼就发现。)
//     · **画面正常、但数字和画面对不上** —— 用户**发现不了**,必须测。
//       (表单显示新值、跑出来是旧值;显示的是另一张图;结论说 PASS 其实跑坏了)
//
//   上一笔(P0-1)改的 GUI 那一半,当时**一条断言都没有**:
//     · 算子没装 -> 参数面板切占位页(不再留着上一个节点的表单)
//     · 算子没装 -> 那一步失败、整批判 ERROR(而且日志不许说「跳过」)
//   那些改动全都落在本文件里。
//
// ============================================================
// 为什么不用 QtTest
// ============================================================
//
//   本项目 25 个套件都用**自己那套极简断言**(打印 [PASS]/[FAIL],
//   末行打 `SUMMARY: pass=N fail=0 RESULT=PASS`)。
//   ⚠ 原因不是「不熟悉 QtTest」,是**CI 数断言数靠那一行**:
//     ci.ps1 的 Step 3b 从 ctest 输出里正则抓 `pass=`。
//     换一套框架就得改 CI 的计数器 ——
//     而那个计数器刚加,还没有第二套实现去验证它。
//   离屏这一步用 QApplication + qputenv("QT_QPA_PLATFORM","offscreen"),
//   和别的套件一模一样。
//
// ============================================================
// 依赖面
// ============================================================
//
//   这个套件**很重**:整个 GUI + 全部算子 + OpenCV。
//   那是必然的 —— 主窗口本来就把整个 GUI 拖进来了。
//   换来的好处是:主窗口的接线**第一次**能测。
//
// ============================================================

#include <QApplication>
#include <QDebug>
#include <windows.h>   // SetConsoleOutputCP

#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPointF>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStringList>
#include <QUuid>

#include "mainwindow.h"
#include "pipeline_view.h"
#include "operator_node.h"

using namespace w7;

// ============================================================
// 极简断言(和别的套件同一套)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static QString g_case;

static void beginCase(const QString& name) {
    g_case = name;
    qInfo().noquote() << "\n=== " << name << " ===";
}

static void check(bool cond, const QString& what) {
    if (cond) {
        ++g_pass;
        qInfo().noquote() << "  [PASS]" << what;
    } else {
        ++g_fail;
        qWarning().noquote() << "  [FAIL]" << what << "  (case:" << g_case << ")";
    }
}

static void checkEq(int actual, int expected, const QString& what) {
    check(actual == expected,
          QString("%1 (期望 %2,实际 %3)").arg(what).arg(expected).arg(actual));
}

// ============================================================
// 小工具
// ============================================================

// 把一段工程 JSON 写到当前工作目录(build/test_work/<套件名>/),
// 返回路径。**不落源码树**。
static QString writeProject(const QString& fileName, const QJsonObject& obj) {
    const QString path = QDir::currentPath() + "/" + fileName;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.close();
    return path;
}

// 走**和真程序一模一样的那条路**加载工程:
// private slot 按名字 invoke,不在测试里另写一份加载逻辑。
static void loadProject(MainWindow& w, const QString& path) {
    QMetaObject::invokeMethod(&w, "loadProjectFrom", Q_ARG(QString, path));
}

// 选中一个节点 -> 画布发 selectionChanged -> 主窗口切参数面板。
//
// ⚠ 为什么**先清空再选**:
//   程序化调 `item->setSelected(true)` **不会**取消上一个选中
//   (取消选中是鼠标点击时 QGraphicsView 做的,SingleSelection 模式下
//   由视图接管 —— 程序化调用绕过了那一层)。
//   于是只调 setSelected 的话,场景里会**同时**有两个选中,
//   主窗口按"取第一个"挑节点 —— 挑到谁**取决于顺序**。
//   症状是测试**时绿时红**,而且红的时候报的错和真正的原因对不上。
//
//   所以这里先 clearSelection()(和鼠标点在空白处一样),
//   再选目标节点 —— 事件顺序就确定了。
static void selectNode(MainWindow& w, OperatorNode* node) {
    if (!node) return;
    auto* canvas = w.findChild<PipelineView*>();
    if (!canvas) return;
    canvas->scene()->clearSelection();
    QApplication::processEvents();
    node->setSelected(true);
    QApplication::processEvents();
}

// 在参数面板里找到「应用参数」那个按钮。
static QPushButton* findApplyButton(QWidget* form) {
    if (!form) return nullptr;
    const auto buttons = form->findChildren<QPushButton*>();
    for (auto* b : buttons) {
        if (b->text().contains("应用")) return b;
    }
    return buttons.isEmpty() ? nullptr : buttons.first();
}

// 参数面板**当前那一页**。
//
// ⚠ 为什么不在整窗里找控件:
//   工具栏上还有一个 QLineEdit(图像路径),它的内容也可能是 "images" ——
//   在整窗里按内容找,会**找到工具栏那个**,于是:
//     · 改的是工具栏的路径,不是表单的
//     · 按「应用参数」时 findApplyButton 找的是**工具栏的按钮**
//       (点下去可能是「打开文件对话框」—— 那会**卡住**整个测试)
//
//   所以:先定位参数面板,再在**当前页**里找。
static QWidget* currentParamPage(MainWindow& w) {
    auto* stack = w.findChild<QStackedWidget*>();
    return stack ? stack->currentWidget() : nullptr;
}

// 在参数面板当前页里按**当前值**找 QLineEdit。
static QLineEdit* findLineEditInParamPage(MainWindow& w, const QString& text) {
    QWidget* page = currentParamPage(w);
    if (!page) return nullptr;
    const auto edits = page->findChildren<QLineEdit*>();
    for (auto* e : edits) {
        if (e->text() == text) return e;
    }
    return nullptr;
}

// ============================================================
// 用例 1:主窗口能起来,默认流水线在
// ============================================================
//
// 这条是**前提**,不是结论 —— 后面每条都建立在它上面。
// 顺手钉住「默认流水线长什么样」:哪天它变了,这里会红。
//
static void test_window_comes_up() {
    beginCase("1. 主窗口能起来,默认流水线在");

    MainWindow w;
    const MainWindowWiring s = w.wiringSnapshot();

    checkEq(s.nodeCount, 5, "默认流水线有 5 个节点");
    checkEq(s.connectionCount, 3, "默认流水线有 3 条连线(QR 那条是旁路,没连)");
    checkEq(s.missingOperatorNodeCount, 0, "默认流水线里没有「未安装」的节点");
    check(s.logText.contains("已加载默认流水线"), "日志里说了加载了默认流水线");

    // 默认流水线是「起始状态」,不该被 Ctrl+Z 撤掉 -> 撤销栈是空的
    check(!s.undoEnabled, "默认流水线下 Undo 是灰的(起始状态不该被撤掉)");
    check(!s.redoEnabled, "默认流水线下 Redo 是灰的");
    check(!s.stopEnabled, "没在跑批时 Stop 是灰的");

    // ⚠ 没有节点被选中时,参数面板应该在**占位页**上,
    //   而且**不能显示任何一个节点的表单**。
    check(!s.paramShowsForm, "没选中节点时参数面板不是表单");
}

// ============================================================
// 用例 2:算子没装 -> 节点留在画布 + 标「未安装」+ 链不断
// ============================================================
//
// ⚠ 这是上一笔(P0-1)改的 GUI 那一半,当时**一条断言都没有**。
//
// 以前解析层把认不出来的算子**直接丢掉**:
//   节点没了 -> 边的端点缺失 -> 边也没了 -> 链断成两截
//   -> 下游拿空输出兜底 -> 数字正常 -> 判 PASS
//
// 「缺一个算子,伤的是它后面所有算子」。
//
static void test_missing_operator_stays_on_canvas() {
    beginCase("2. 算子没装:节点留在画布、标「未安装」、链不断");

    // 一个工程:源 -> 一个**没装**的算子 -> 汇
    // 用了**真 UUID**(MD5 派生)—— 假 UUID 会被当成旧格式,
    // 于是测试护的是磁盘上根本不存在的格式(绿,但骗人)。
    const QString srcId = "6f1a2b3c-4d5e-4f60-8a71-0b1c2d3e4f50";
    const QString badId = "7a2b3c4d-5e6f-4071-9b82-1c2d3e4f5061";
    const QString dstId = "8b3c4d5e-6f70-4182-ac93-2d3e4f506172";

    QJsonObject project;
    project["image_path"] = "images";
    project["name"] = "缺一个算子";
    project["nodes"] = QJsonArray{
        QJsonObject{{"op","image_source_dir"},{"display","源"},{"category","acquisition"},
                    {"x",100},{"y",200},{"uuid",srcId},
                    {"params", QJsonObject{{"dir_path","images"},{"pattern","*.png"}}}},
        QJsonObject{{"op","no_such_operator_v2"},{"display","没装的算子"},{"category","detection"},
                    {"x",400},{"y",200},{"uuid",badId}},
        QJsonObject{{"op","blob_count"},{"display","计数"},{"category","detection"},
                    {"x",700},{"y",200},{"uuid",dstId}},
    };
    project["edges"] = QJsonArray{
        QJsonObject{{"from",srcId},{"to",badId}},
        QJsonObject{{"from",badId},{"to",dstId}},
    };

    const QString path = writeProject("missing_operator.json", project);
    check(!path.isEmpty(), "工程文件写出来了");
    if (path.isEmpty()) return;

    MainWindow w;
    loadProject(w, path);
    const MainWindowWiring s = w.wiringSnapshot();

    checkEq(s.nodeCount, 3, "**三个节点都在画布上**(没装的算子没被丢掉)");
    checkEq(s.missingOperatorNodeCount, 1, "有一个节点标着「未安装」");
    checkEq(s.connectionCount, 2, "**两条连线都接上了**(链没断成两截)");

    // 那个没装的节点,画布上**得有名字**(不能是个空框)
    const auto& nodes = w.findChild<PipelineView*>()->nodes();
    OperatorNode* bad = nullptr;
    for (auto* n : nodes) {
        if (n->isOperatorMissing()) { bad = n; break; }
    }
    check(bad != nullptr, "找得到那个「未安装」的节点");
    if (bad) {
        check(!bad->displayName().isEmpty(), "「未安装」的节点在画布上有名字");
        check(bad->opName() == "no_such_operator_v2",
              "节点保留的是原来的算子名(没被换成别的东西)");
    }

    // ⚠ 而且**必须报出来** —— 不报就是静默失效
    check(s.logText.contains("未安装"), "日志里说了有算子没装");
}

// ============================================================
// 用例 3:算子没装 -> 参数面板切占位页,不许显示上一个节点的表单
// ============================================================
//
// ⚠ 这是「面板**不说谎**」那条底线。
//   以前面板上留着的是**上一个节点**的参数表单 ——
//   用户以为自己在看这个节点的参数,其实看的是别人的。
//
// ⚠ 这种谎比「什么都不显示」更难查:
//   面板上密密麻麻一堆旋钮,看着**很正常**。
//   这就是「用户自己发现不了」的那一类。
//
static void test_missing_operator_placeholder_not_someone_elses_form() {
    beginCase("3. 算子没装:参数面板切占位页,不许显示别人的表单");

    const QString srcId = "11111111-2222-4333-8444-555555555555";
    const QString badId = "66666666-7777-4888-8999-aaaaaaaaaaaa";

    QJsonObject project;
    project["image_path"] = "images";
    project["nodes"] = QJsonArray{
        QJsonObject{{"op","color_segment"},{"display","颜色分割"},{"category","detection"},
                    {"x",100},{"y",200},{"uuid",srcId}},
        QJsonObject{{"op","no_such_operator_v2"},{"display","没装的算子"},{"category","detection"},
                    {"x",400},{"y",200},{"uuid",badId}},
    };
    project["edges"] = QJsonArray{};

    const QString path = writeProject("missing_operator_panel.json", project);
    check(!path.isEmpty(), "工程文件写出来了");
    if (path.isEmpty()) return;

    MainWindow w;
    loadProject(w, path);

    auto* canvas = w.findChild<PipelineView*>();
    check(canvas != nullptr, "找得到画布");
    if (!canvas) return;

    // --- 先选中一个**认识的**节点 ---
    OperatorNode* known = nullptr;
    OperatorNode* bad = nullptr;
    for (auto* n : canvas->nodes()) {
        if (n->isOperatorMissing()) bad = n;
        else if (!known) known = n;
    }
    check(known != nullptr && bad != nullptr, "工程里有一个认识的节点和一个没装的节点");
    if (!known || !bad) return;

    selectNode(w, known);
    MainWindowWiring s1 = w.wiringSnapshot();
    check(s1.paramShowsForm, "选中**认识的**节点时,参数面板显示的是**它的表单**");
    checkEq(s1.paramPanelPages, 2, "参数面板有 2 页(占位页 + 这个节点的表单)");

    // --- 再选中那个**没装**的节点 ---
    selectNode(w, bad);
    MainWindowWiring s2 = w.wiringSnapshot();
    check(!s2.paramShowsForm,
          "选中**没装的**节点时,参数面板**不再显示上一个节点的表单**");
    // ⚠ 上一个表单必须**真的被拆掉** ——
    //   以前它只是「不显示」,却还留在堆栈里,成了孤儿:
    //   面板每选一次没装的节点就多一个死表单,越点越胖。
    checkEq(s2.paramPanelPages, 1,
            "切到占位页后,参数面板里**只剩占位页**(上一个表单被拆掉了)");
    check(s2.paramPlaceholderText.contains("no_such_operator_v2"),
          "占位页上点名了是哪个算子没装");
    check(s2.paramPlaceholderText.contains("未安装"),
          "占位页上说了「未安装」");
}

// ============================================================
// 用例 4:算子没装 -> 那一步失败、整批判 ERROR,日志不许说「跳过」
// ============================================================
//
// ⚠ 上一笔(P0-1)在 headless 那边立了规矩:
//   「算子没装」由**解析层**说了算,那一步**强制失败**,整批判 ERROR。
//
// ⚠ 而 GUI 侧当时是**另一份实现**,而且消息写的是「未注册,**跳过**」:
//   它**并没有跳过** —— 那一步返回了 false,引擎记成失败,整批判 ERROR。
//   用户读到「跳过」会以为后面照跑了,而结论是**整批作废**。
//
// 这一条钉住的是:两边**行为一致**,而且**消息不许骗人**。
//
static void test_missing_operator_run_is_error_not_skip() {
    beginCase("4. 算子没装:跑起来是 ERROR,日志不许说「跳过」");

    const QString badId = "99999999-aaaa-4bbb-8ccc-dddddddddddd";
    QJsonObject project;
    project["image_path"] = "images";
    project["nodes"] = QJsonArray{
        QJsonObject{{"op","no_such_operator_v2"},{"display","没装的算子"},{"category","detection"},
                    {"x",400},{"y",200},{"uuid",badId}},
    };
    project["edges"] = QJsonArray{};

    const QString path = writeProject("missing_operator_run.json", project);
    check(!path.isEmpty(), "工程文件写出来了");
    if (path.isEmpty()) return;

    MainWindow w;
    loadProject(w, path);

    auto* canvas = w.findChild<PipelineView*>();
    check(canvas != nullptr, "找得到画布");
    if (!canvas) return;

    // ⚠ 先加一个节点,让撤销栈**非空** ——
    //   否则「跑完 Undo 还是灰的」和「跑完 Undo 恢复了」根本分不出来。
    //   走的是**和用户从算子库拖一个节点进来一模一样的那条路**
    //   (画布发请求 -> 主窗口包成 Command -> 进撤销栈)。
    canvas->nodeAddRequested("blob_count", "计数", "detection", QPointF(1200, 600));
    QApplication::processEvents();
    const MainWindowWiring before = w.wiringSnapshot();
    check(before.undoEnabled, "加了一个节点之后 Undo 可用(撤销栈非空)");

    // 跑一次。走**和点 ▶ Run 一模一样的那条路**(private slot)。
    QMetaObject::invokeMethod(&w, "onRunClicked");
    QApplication::processEvents();

    const MainWindowWiring s = w.wiringSnapshot();
    check(s.logText.contains("这一步跑不了"),
          "日志里说的是「这一步跑不了」");
    check(!s.logText.contains("未注册,跳过"),
          "日志里**不许**出现「跳过」(它并没有跳过,那一步失败了)");
    check(s.logText.contains("最终结果: ERROR"),
          "整批判 ERROR(不是静默 PASS)");

    // ⚠ 跑完之后,**跑之前是什么样,跑完就该还是什么样** ——
    //   跑批期间禁掉 Undo/Redo 是为了防悬垂指针(拓扑序里是裸指针),
    //   跑完必须**恢复**,否则用户按 Ctrl+Z 没反应,
    //   而界面上看不出任何异常。
    //   少管一样就等于留一个「下次 Run 点不动/按钮一直是灰的」的坑。
    check(!s.stopEnabled, "跑完之后 Stop 恢复成灰的(没有卡在「正在跑」)");
    check(s.undoEnabled == before.undoEnabled,
          "跑完之后 Undo 恢复成**跑之前**的样子");
    check(s.redoEnabled == before.redoEnabled,
          "跑完之后 Redo 恢复成**跑之前**的样子");
}

// ============================================================
// 用例 5:改了参数 -> 节点参数真的更新了
// ============================================================
//
// ⚠ 「画面正常、但数字和画面对不上」的第一种:
//   表单上写着新值,跑出来却是旧值 ——
//   用户看得见表单变了,看不见数字为什么没变。
//
static void test_param_change_updates_node() {
    beginCase("5. 改了参数:节点上的参数真的更新了");

    const QString srcId = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    QJsonObject project;
    project["image_path"] = "images";
    project["nodes"] = QJsonArray{
        QJsonObject{{"op","image_source_dir"},{"display","源"},{"category","acquisition"},
                    {"x",100},{"y",200},{"uuid",srcId},
                    {"params", QJsonObject{{"dir_path","images"},{"pattern","*.png"}}}},
    };
    project["edges"] = QJsonArray{};

    const QString path = writeProject("param_change.json", project);
    check(!path.isEmpty(), "工程文件写出来了");
    if (path.isEmpty()) return;

    MainWindow w;
    loadProject(w, path);

    auto* canvas = w.findChild<PipelineView*>();
    if (!canvas || canvas->nodes().isEmpty()) { check(false, "画布上有节点"); return; }
    OperatorNode* node = canvas->nodes().first();

    selectNode(w, node);
    const MainWindowWiring s1 = w.wiringSnapshot();
    check(s1.paramShowsForm, "参数面板显示的是这个节点的表单");
    if (!s1.paramShowsForm) return;

    // 找到表单里的目录输入框,改成别的值,再点「应用参数」
    QLineEdit* dirEdit = findLineEditInParamPage(w, "images");
    check(dirEdit != nullptr, "参数表单里找得到目录输入框");
    if (!dirEdit) return;

    dirEdit->setText("images_changed");
    QPushButton* apply = findApplyButton(currentParamPage(w));
    check(apply != nullptr, "找得到「应用参数」按钮");
    if (!apply) return;

    apply->click();
    QApplication::processEvents();

    check(node->params().value("dir_path").toString() == "images_changed",
          "点「应用参数」之后,节点上的参数真的变了");
}

// ============================================================
// 用例 6:改了参数 -> 这一步的旧结果**作废**
// ============================================================
//
// ⚠ 「画面正常、但数字和画面对不上」的第二种,而且更隐蔽:
//
//   用户调完阈值,右键回看这一步 —— 看到的是**调参之前**的结果,
//   画面看着完全正常(就是那张图),
//   数字却和当前参数对不上。
//
//   用户会以为「改了没生效」,或者更糟:以为程序算错了。
//
// 所以规则是:**参数一改,这一步的旧结果就不作数**(宁可看不到)。
// 三件事必须**一起**发生:
//   1. 节点参数更新
//   2. 这一步的每步缓存被清掉
//   3. 节点上「有可回看结果」的标记被清掉
//
// ⚠ 少了任何一件,症状都是「画面正常、数字对不上」。
//
static void test_param_change_invalidates_cached_result() {
    beginCase("6. 改了参数:这一步的旧结果作废");

    const QString srcId = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
    QJsonObject project;
    project["image_path"] = "images";
    project["nodes"] = QJsonArray{
        QJsonObject{{"op","image_source_dir"},{"display","源"},{"category","acquisition"},
                    {"x",100},{"y",200},{"uuid",srcId},
                    {"params", QJsonObject{{"dir_path","images"},{"pattern","*.png"}}}},
    };
    project["edges"] = QJsonArray{};

    const QString path = writeProject("param_invalidate.json", project);
    check(!path.isEmpty(), "工程文件写出来了");
    if (path.isEmpty()) return;

    MainWindow w;
    loadProject(w, path);

    auto* canvas = w.findChild<PipelineView*>();
    if (!canvas || canvas->nodes().isEmpty()) { check(false, "画布上有节点"); return; }
    OperatorNode* node = canvas->nodes().first();
    const QString nodeId = node->uuid().toString(QUuid::WithoutBraces);

    // --- 先跑一次,让缓存里有东西 ---
    QMetaObject::invokeMethod(&w, "onRunClicked");
    QApplication::processEvents();

    MainWindowWiring s1 = w.wiringSnapshot();
    check(s1.cachedNodeIds.contains(nodeId),
          "跑完之后,这一步的结果在缓存里");
    check(node->hasCachedResult(),
          "跑完之后,节点上标着「有可回看的结果」");

    // --- 改参数 ---
    selectNode(w, node);
    if (!w.wiringSnapshot().paramShowsForm) { check(false, "参数面板显示的是这个节点的表单"); return; }

    QLineEdit* dirEdit = findLineEditInParamPage(w, "images");
    check(dirEdit != nullptr, "参数表单里找得到目录输入框");
    if (!dirEdit) return;

    dirEdit->setText("images_changed");
    QPushButton* apply = findApplyButton(currentParamPage(w));
    if (!apply) { check(false, "找得到「应用参数」按钮"); return; }
    apply->click();
    QApplication::processEvents();

    // --- 三件事必须一起发生 ---
    MainWindowWiring s2 = w.wiringSnapshot();
    check(!s2.cachedNodeIds.contains(nodeId),
          "改完参数,这一步的每步缓存被清了");
    check(!node->hasCachedResult(),
          "改完参数,节点上「有可回看的结果」的标记被清了");
    check(node->params().value("dir_path").toString() == "images_changed",
          "改完参数,节点上的参数真的变了");
}

// ============================================================
// 用例 7:纯层那份规则 —— GUI 和 headless 必须走**同一份**
// ============================================================
//
// ⚠ 这一条不是「测行为」,是「测**没有两份**」。
//
//   「算子没装 -> 那一步失败」这条规则,2026-10-25 当时有**两份实现**:
//     headless_runner.cpp :: guardedRunner
//     mainwindow.cpp     :: runPipeline 里内联的 runner
//   两份**行为一致、消息不一致** ——
//   属于最难发现的那一类:不比对两边就看不出差别。
//
//   判据用**源码守卫**:两个调用方都必须调那个**纯层入口**。
//   ⚠ 查之前**先剥掉注释** —— 注释里写一遍就把它满足了
//     (见 notes/atomic/at-the-guard-can-be-fed-by-a-comment.md)。
//
static void test_missing_operator_rule_has_one_home() {
    beginCase("7.「算子没装」这条规则只有一份");

    const QString pure = QStringLiteral("rejectMissingOperator");
    const QString headless = QStringLiteral("headless_runner.cpp");
    const QString gui = QStringLiteral("mainwindow.cpp");

    auto read = [](const QString& rel) -> QString {
        QFile f(QStringLiteral(W7_SOURCE_DIR) + "/" + rel);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(f.readAll());
    };

    // 剥掉注释:  // 和 /* */
    auto stripComments = [](QString s) -> QString {
        s.replace(QRegularExpression("/\\*[\\s\\S]*?\\*/"), QString());
        s.replace(QRegularExpression("//[^\\n]*"), QString());
        return s;
    };

    const QString h = stripComments(read(headless));
    const QString g = stripComments(read(gui));

    check(h.contains(pure), "headless 走的是**纯层那一份**");
    check(g.contains(pure), "GUI 走的是**纯层那一份**");

    // ⚠ 而且不许再内联一份自己的判据
    check(!g.contains("未注册,跳过"),
          "GUI 不许再自己写一份「未注册,跳过」的消息");
    check(!h.contains("未注册,跳过"),
          "headless 也不许有那句「跳过」");
}

// ============================================================
// 用例 8:判据是「值有没有被用上」,不是「名字在不在」
// ============================================================
//
// ⚠ 这一条是上一轮(M179)吃过的亏,现在钉住。
//
//   M179 那个变异把 `.arg(loadedGraph.unknownNodes)` 换成 `.arg(0)`,
//   而守卫查的是「源码里有没有出现某个名字」——
//   名字还在,**值已经没了**。
//
//   所以守卫必须查**调用**,不是查**名字**。
//
static void test_guard_checks_the_call_not_the_name() {
    beginCase("8. 守卫查的是「有没有调」,不是「名字在不在」");

    QFile f(QStringLiteral(W7_SOURCE_DIR) + "/mainwindow.cpp");
    check(f.open(QIODevice::ReadOnly), "读得到 mainwindow.cpp");
    if (!f.isOpen()) return;
    QString src = QString::fromUtf8(f.readAll());
    src.replace(QRegularExpression("/\\*[\\s\\S]*?\\*/"), QString());
    src.replace(QRegularExpression("//[^\\n]*"), QString());

    // 必须**调用**那个纯层入口:  w7::describeLoadAnomalies(
    check(src.contains("w7::describeLoadAnomalies("),
          "GUI **调用**了 w7::describeLoadAnomalies(...)");
}

// ============================================================
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");   // headless
    // Windows 控制台默认 GBK,中文断言信息会乱码 —— 强制 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 主窗口接线 回归测试(offscreen)";

    test_window_comes_up();
    test_missing_operator_stays_on_canvas();
    test_missing_operator_placeholder_not_someone_elses_form();
    test_missing_operator_run_is_error_not_skip();
    test_param_change_updates_node();
    test_param_change_invalidates_cached_result();
    test_missing_operator_rule_has_one_home();
    test_guard_checks_the_call_not_the_name();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    // ASCII 摘要:控制台编码不确定时也能一眼看出结果
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
