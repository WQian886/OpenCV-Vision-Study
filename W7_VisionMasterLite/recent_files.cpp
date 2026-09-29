#include "recent_files.h"

#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace w7 {

RecentFiles::RecentFiles(const QString& settingsKey, int maxCount)
    : m_key(settingsKey), m_maxCount(qMax(1, maxCount)) {}

QString RecentFiles::normalize(const QString& path) {
    if (path.isEmpty()) return {};
    // 相对路径在"换个目录启动程序"之后就指到别的地方去了 ——
    // 这里统一转绝对。cleanPath 顺带把 "a/../b" 折叠掉,
    // 否则同一个文件会以两种写法各占一行。
    const QString abs = QFileInfo(path).absoluteFilePath();
    return QDir::cleanPath(abs);
}

QStringList RecentFiles::paths() const {
    QSettings s;
    return s.value(m_key).toStringList();
}

void RecentFiles::add(const QString& path) {
    const QString p = normalize(path);
    if (p.isEmpty()) return;

    QStringList list = paths();
    list.removeAll(p);            // 去重:重开同一个文件不该出现两条
    list.prepend(p);              // 最新的排最前

    while (list.size() > m_maxCount) list.removeLast();

    QSettings s;
    s.setValue(m_key, list);
}

int RecentFiles::pruneMissing() {
    QStringList list = paths();
    const int before = list.size();

    QStringList alive;
    for (const QString& p : list) {
        // 文件被删/被挪走的条目留着没用,点下去只会报错
        if (QFileInfo::exists(p)) alive.append(p);
    }

    if (alive.size() != before) {
        QSettings s;
        s.setValue(m_key, alive);
    }
    return before - alive.size();
}

void RecentFiles::clear() {
    QSettings s;
    s.remove(m_key);
}

} // namespace w7
