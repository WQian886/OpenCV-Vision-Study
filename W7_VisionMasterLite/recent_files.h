#pragma once
// ============================================================
// RecentFiles — 最近打开的工程列表(存 QSettings)
//
// 单抽一个类出来,是因为这段逻辑有两个真会出事的点,而它们
// 只有在"程序从别的目录启动"时才暴露:
//
//   1. 存相对路径。第一次从工程根启动,存下 "demo.sol.json";
//      下次从 D:\ 启动,同一个字符串指向 D:\demo.sol.json —— 文件
//      "不见了"。所以 add() 前一律转绝对路径。
//   2. 不剔除已删除的条目。用户把工程挪走/删了,列表里还挂着,
//      点下去报"文件不存在",久而久之整个菜单都不敢用了。
//
// 两个方法都写成了不依赖 UI 的纯逻辑,可以直接单测。
// ============================================================

#include <QString>
#include <QStringList>

namespace w7 {

class RecentFiles {
public:
    // settingsKey 用来区分不同列表(比如"最近工程"和"最近图像")
    explicit RecentFiles(const QString& settingsKey, int maxCount = 5);

    // 读出来的路径已归一化(绝对路径)
    QStringList paths() const;

    // 加一条:已有的提到最前,重复的不产生第二条,超过上限丢最旧的
    void add(const QString& path);

    // 剔掉磁盘上已经不存在的条目,返回剔掉了几条
    int pruneMissing();

    void clear();

    // 相对 → 绝对,顺便把分隔符统一成 Qt 的 /
    // 空串和已经是绝对路径的原样返回
    static QString normalize(const QString& path);

    int maxCount() const { return m_maxCount; }

private:
    QString m_key;
    int     m_maxCount;
};

} // namespace w7
