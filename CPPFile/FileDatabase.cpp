#include "HeaderFile/FileDatabase.h"

#include <QFileInfo>
#include <QSqlError>
#include <QThread>
#include <math.h>
#include <QCoreApplication>

//工具函数
namespace {
    //同步搜索（独立只读连接：WAL 并发读，不排 dbThread
    QList<FileInfo> runSearchSync(const QString& sql, const QList<QVariant>& binds)
    {
        QList<FileInfo> res;
        const QString connName = QStringLiteral("search_%1").arg(
            reinterpret_cast<quintptr>(QThread::currentThreadId()));
        {
            QSqlDatabase sdb = QSqlDatabase::addDatabase("QSQLITE", connName);
            sdb.setDatabaseName(QCoreApplication::applicationDirPath() + "/fileindex.db");
            sdb.setConnectOptions("QSQLITE_OPEN_READONLY");   // 只读：扫描写库可并发
            if (sdb.open())
            {
                QSqlQuery q(sdb);
                q.prepare(sql);
                for (const QVariant& v : binds)
                    q.addBindValue(v);
                if (q.exec())
                {
                    while (q.next())
                    {
                        FileInfo f;
                        f.name = q.value(0).toString();
                        f.path = q.value(1).toString();
                        f.suffix = q.value(2).toString();
                        f.size = q.value(3).toLongLong();
                        f.modifiedTime = QDateTime::fromSecsSinceEpoch(q.value(4).toLongLong());
                        f.isFolder = q.value(5).toInt() == 1;
                        f.id = q.value(6).toLongLong();
                        res.append(f);
                    }
                }
            }
            sdb.close();
        }
        QSqlDatabase::removeDatabase(connName);
        return res;
    }

    QString escapeLike(const QString& kw)
    {
        QString e = kw;
        e.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
            .replace(QLatin1Char('%'), QStringLiteral("\\%"))
            .replace(QLatin1Char('_'), QStringLiteral("\\_"));
        return e;
    }

    static QString orderDir(int sortType)
    {
        return (sortType % 2) ? QStringLiteral("DESC") : QStringLiteral("ASC");
    }

    static QString orderKeyExpr(int sortType)
    {
        return (sortType / 2 == 0) ? QStringLiteral("name COLLATE NOCASE")
            : (sortType / 2 == 1) ? QStringLiteral("size")
            : QStringLiteral("modifiedTime");
    }

    QVariant cursorKeyOf(const FileInfo& f, int sortType)
    {
        switch (sortType / 2) {
        case 0:  return f.name;                              // TEXT  ← 对应 name COLLATE NOCASE
        case 1:  return f.size;                              // INTEGER
        default: return f.modifiedTime.toSecsSinceEpoch();   // INTEGER（纪元秒，不是 QDateTime！）
        }
    }

    PagedResult searchPage(const QString& whereSql, const QList<QVariant>& whereBinds,
        int sortType, bool notIndexed,
        const QVariant& curKey, qint64 curId, int limit)
    {
        const QString keyExpr = orderKeyExpr(sortType);   // 可能是 "name COLLATE NOCASE"
        const QString dir = orderDir(sortType);       // "ASC" / "DESC"
        const QString cmp = (dir == QLatin1String("ASC")) ? QStringLiteral(">") : QStringLiteral("<");

        QString sql = QStringLiteral(
            "SELECT name, path, suffix, size, modifiedTime, fileType, id FROM files ");
        if (notIndexed)
            sql += QStringLiteral("NOT INDEXED ");        // 必须紧跟表名
        sql += QStringLiteral("WHERE ") + whereSql + QLatin1Char(' ');   // 兜底补空格

        QList<QVariant> binds = whereBinds;
        if (curKey.isValid()) {                           // 游标条件 = 「排在锚点之后」
            sql += QStringLiteral("AND (") + keyExpr + QLatin1Char(' ') + cmp + QStringLiteral(" ? ")
                + QStringLiteral("OR (") + keyExpr + QStringLiteral(" = ? AND id ") + cmp + QStringLiteral(" ?)) ");
            binds << curKey << curKey << curId;
        }

        sql += QStringLiteral("ORDER BY ") + keyExpr + QLatin1Char(' ') + dir
            + QStringLiteral(", id ") + dir
            + QStringLiteral(" LIMIT ") + QString::number(limit + 1);

        PagedResult r;
        r.rows = runSearchSync(sql, binds);
        r.hasMore = r.rows.size() > limit;
        if (r.hasMore)
            r.rows.removeLast();                          // 多要的那条只用来判 hasMore
        if (!r.rows.isEmpty()) {
            const FileInfo& last = r.rows.last();
            r.lastKey = cursorKeyOf(last, sortType);      // 给下一页用
            r.lastId = last.id;
        }
        else {
            r.hasMore = false;
        }
        return r;
    }

    static QString prefixUpperBound(const QString& p)
    {
        if (p.isEmpty())
            return p;
        QString up = p;
        up.back() = QChar(up.back().unicode() + 1);
        return up;
    }
}

FileDatabase::FileDatabase(QObject* parent)
{
}

bool FileDatabase::initDatabase()
{
    db = QSqlDatabase::addDatabase("QSQLITE", "FileDatabaseConnection");

    db.setDatabaseName(QCoreApplication::applicationDirPath() + "/fileindex.db");

    if (!db.open())
    {
        qWarning() << "数据库打开失败\n";

        return false;
    }

    // 写入提速四件套
    db.exec("PRAGMA journal_mode = WAL;");        // WAL 模式：读写不互斥，写入更快
    db.exec("PRAGMA synchronous = NORMAL;");      // 降低 fsync 频率，大幅提速（掉电最多丢最后一批）
    db.exec("PRAGMA temp_store = MEMORY;");       // 临时表/排序放内存
    db.exec("PRAGMA cache_size = -65536;");       // 页缓存 64MB，减少磁盘读
    db.exec("PRAGMA wal_autocheckpoint = 4000;"); // 4MB 才自动 checkpoint
    db.exec("PRAGMA journal_size_limit = 67108864;"); // WAL 上限 64

    query = QSqlQuery(db);

    query.exec(
        R"(
            CREATE TABLE IF NOT EXISTS files
            (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT,
                path TEXT UNIQUE,
                suffix TEXT,
                size INTEGER,
                modifiedTime INTEGER,
                fileType INTEGER
            )
        )"
    );
    query.exec(
        R"(
        CREATE TABLE IF NOT EXISTS metadata
        (
            key TEXT PRIMARY KEY,
            value TEXT
        )
    )"
    );

    query.prepare(
        R"(
                    INSERT OR REPLACE INTO files
                    (
                        name,
                        path,
                        suffix,
                        size,
                        modifiedTime,
                        fileType
                    )
                    VALUES
                    (
                        :name,
                        :path,
                        :suffix,
                        :size,
                        :modifiedTime,
                        :fileType
                    )
        )"
    );

    return true;
}

void FileDatabase::insertFile(const QList<FileInfo>& info)
{
    if (info.isEmpty())
        return;

    if (!db.isOpen())
    {
        qWarning() << "数据库没有打开";
        return;
    }

    if (!db.transaction())
    {
        qWarning() << "开启事务失败:" << db.lastError();
        return;
    }

    // 每次创建一个新的 QSqlQuery
    QSqlQuery batchQuery(db);

    if (!batchQuery.prepare(
        R"(
            INSERT OR REPLACE INTO files
            (
                name,
                path,
                suffix,
                size,
                modifiedTime,
                fileType
            )
            VALUES
            (
                :name,
                :path,
                :suffix,
                :size,
                :modifiedTime,
                :fileType
            )
        )"))
    {
        qWarning() << "prepare失败:"
            << batchQuery.lastError();

        db.rollback();
        return;
    }

    QVariantList names;
    QVariantList paths;
    QVariantList suffixes;
    QVariantList sizes;
    QVariantList modifiedTimes;
    QVariantList fileTypes;

    names.reserve(info.size());
    paths.reserve(info.size());
    suffixes.reserve(info.size());
    sizes.reserve(info.size());
    modifiedTimes.reserve(info.size());
    fileTypes.reserve(info.size());

    for (const auto& file : info)
    {
        QString normalizedPath = file.path;
        if (normalizedPath.size() >= 2 && normalizedPath[1] == QChar(':')) {
            normalizedPath[0] = normalizedPath[0].toUpper();
        }
        names.append(file.name);
        paths.append(normalizedPath);
        suffixes.append(file.suffix);
        sizes.append(file.size);
        modifiedTimes.append(
            file.modifiedTime.toSecsSinceEpoch()
        );
        fileTypes.append(file.isFolder ? 1 : 0);
    }

    batchQuery.bindValue(":name", names);
    batchQuery.bindValue(":path", paths);
    batchQuery.bindValue(":suffix", suffixes);
    batchQuery.bindValue(":size", sizes);
    batchQuery.bindValue(":modifiedTime", modifiedTimes);
    batchQuery.bindValue(":fileType", fileTypes);

    //执行批量插入
    if (!batchQuery.execBatch())
    {
        qWarning() << "批量插入失败:"
            << batchQuery.lastError();

        db.rollback();
        return;
    }

    if (!db.commit())
    {
        qWarning() << "数据库提交失败:"
            << db.lastError();

        db.rollback();
        return;
    }
}

bool FileDatabase::isInitialScanFinished()
{
    QSqlQuery query(db);

    query.prepare(
        "SELECT value FROM metadata WHERE key = 'initial_scan'"
    );

    if (!query.exec()) {
        qWarning() << query.lastError();
        return false;
    }

    if (!query.next()) {
        return false;
    }

    return query.value(0).toString() == "1";
}

void FileDatabase::setInitialScanFinished()
{
    QSqlQuery idxQuery(db);
    idxQuery.exec("CREATE INDEX IF NOT EXISTS idx_suffix ON files(suffix);");
    idxQuery.exec("CREATE INDEX IF NOT EXISTS idx_name ON files(name);");
    idxQuery.exec("CREATE INDEX IF NOT EXISTS idx_path ON files(path);");
	idxQuery.exec("CREATE INDEX IF NOT EXISTS idx_modifiedTime ON files(modifiedTime);");

    QSqlQuery query(db);

    query.prepare(
        R"(
            INSERT OR REPLACE INTO metadata(key, value)
            VALUES('initial_scan', '1')
        )"
    );

    if (!query.exec()) {
        qWarning() << "保存初始扫描状态失败:"
            << query.lastError();
    }

    // 完成后在这里一次性合并 WAL 到主库，避免残留大 WAL
    QSqlQuery cpQuery(db);
    cpQuery.exec("PRAGMA wal_checkpoint(TRUNCATE);");
}

// 增量扫描
quint64 FileDatabase::getLastUsn(const QString& drive)
{
    QSqlQuery q(db);
    q.prepare("SELECT value FROM metadata WHERE key = ?");
    q.addBindValue("last_usn_" + drive);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toULongLong();
}

void FileDatabase::setLastUsn(const QString& drive, quint64 usn)
{
    QSqlQuery q(db);
    q.prepare("INSERT OR REPLACE INTO metadata(key, value) VALUES(?, ?)");
    q.addBindValue("last_usn_" + drive);
    q.addBindValue(QString::number(usn));
    if (!q.exec())
        qWarning() << "setLastUsn 失败:" << q.lastError();
}

QString FileDatabase::getVolumeId(const QString& drive)
{
    QSqlQuery q(db);
    q.prepare("SELECT value FROM metadata WHERE key = ?");
    q.addBindValue("vol_" + drive);
    if (!q.exec() || !q.next())
        return QString();
    return q.value(0).toString();
}

void FileDatabase::setVolumeId(const QString& drive, const QString& volId)
{
    QSqlQuery q(db);
    q.prepare("INSERT OR REPLACE INTO metadata(key, value) VALUES(?, ?)");
    q.addBindValue("vol_" + drive);
    q.addBindValue(volId);
    if (!q.exec())
        qWarning() << "setVolumeId 失败:" << q.lastError();
}

// 清空某盘全部数据（移动盘遍历重建前 / 盘符换了盘时）：
// 删除该盘所有条目 + 增量基线 + 卷标识，下次全量/遍历重建
void FileDatabase::clearDrive(const QString& letter)
{
    if (!db.isOpen())
        return;
    const QString upLetter = letter.toUpper();   // 盘符归一化（防小写盘符）
    db.transaction();
    // GLOB 通配：反斜杠是字面量无转义问题，前缀匹配盘根（路径统一反斜杠）
    QSqlQuery q(db);
    // substr 前缀匹配盘根（无通配符转义歧义）：C:\ 开头全部删除
    // substr(path,1,N)：N = 盘符长度 + 2（如 "C" + ":\" = 3 字符）
    q.prepare("DELETE FROM files WHERE substr(path, 1, ?) = ?");
    q.addBindValue(upLetter.length() + 2);
    q.addBindValue(upLetter + QLatin1String(":\\"));
    if (!q.exec())
        qWarning() << "clearDrive files 失败:" << q.lastError();
    QSqlQuery m(db);
    m.prepare("DELETE FROM metadata WHERE key IN (?, ?)");
    m.addBindValue("last_usn_" + letter);
    m.addBindValue("vol_" + letter);
    if (!m.exec())
        qWarning() << "clearDrive metadata 失败:" << m.lastError();
    db.commit();
}

void FileDatabase::deleteFiles(const QStringList& paths)
{
    if (paths.isEmpty())
        return;
    QStringList up = paths;   // 盘符归一化（防小写盘符路径匹配失败）
    for (QString& p : up) {
        if (p.size() >= 2 && p.at(1) == QLatin1Char(':'))
            p[0] = p.at(0).toUpper();
    }
    if (!db.isOpen())
        return;
    db.transaction();
    QSqlQuery q(db);
    q.prepare("DELETE FROM files WHERE path = ?");
    for (const QString& p : up)
    {
        q.addBindValue(p);
        if (!q.exec())
        {
            qWarning() << "deleteFiles 失败:" << q.lastError();
            break;
        }
    }
    db.commit();
}

void FileDatabase::renamePrefix(const QString& oldPrefix, const QString& newPrefix)
{
    if (oldPrefix.isEmpty() || newPrefix.isEmpty())
        return;
    // 盘符归一化（防小写盘符路径匹配失败）
    QString oldP = oldPrefix, newP = newPrefix;
    if (oldP.size() >= 2 && oldP.at(1) == QLatin1Char(':')) oldP[0] = oldP.at(0).toUpper();
    if (newP.size() >= 2 && newP.at(1) == QLatin1Char(':')) newP[0] = newP.at(0).toUpper();
    if (!db.isOpen())
        return;
    db.transaction();
    QSqlQuery q(db);
    // 转义 LIKE 通配符；子项模式：旧前缀 + 字面反斜杠(\\转义对) + % 通配
    QString escOld = oldP;
    escOld.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    q.prepare(R"(
            UPDATE files SET path = ? || substr(path, ?)
            WHERE path = ? OR path LIKE ? ESCAPE '\'
        )");
    q.addBindValue(newP);
    q.addBindValue(QString::number(oldP.size() + 1));   // 去掉旧前缀及其尾斜杠
    q.addBindValue(oldP);                                // 目录自身
    q.addBindValue(escOld + "\\\\" + '%');                    // 直接子项
    if (!q.exec())
        qWarning() << "renamePrefix 失败:" << q.lastError();
    db.commit();
}

// 移动盘变化标记（运行时监视设置，下次启动据此决定是否重建）
void FileDatabase::setDirty(const QString& drive, bool dirty)
{
    QSqlQuery q(db);
    q.prepare("INSERT OR REPLACE INTO metadata(key, value) VALUES(?, ?)");
    q.addBindValue("dirty_" + drive);
    q.addBindValue(dirty ? "1" : "0");
    if (!q.exec())
        qWarning() << "setDirty 失败:" << q.lastError();
}

// 上次目录遍历时间戳（秒，用于移动盘增量判断）
void FileDatabase::setLastTraverseTime(const QString& drive, qint64 ts)
{
    QSqlQuery q(db);
    q.prepare("INSERT OR REPLACE INTO metadata(key, value) VALUES(?, ?)");
    q.addBindValue("lastTraverse_" + drive);
    q.addBindValue(QString::number(ts));
    if (!q.exec())
        qWarning() << "setLastTraverseTime 失败:" << q.lastError();
}

PagedResult FileDatabase::searchFileSuffixSync(const QString& keyword, int sortType, const QVariant& curKey, qint64 curId, const QString& drivePrefix)
{
    QString suf = keyword.toLower();
    if (drivePrefix.isEmpty()) {
        return searchPage(QStringLiteral("suffix = ?"),
            { suf }, sortType, true, curKey, curId, kPageSize);
    }
    return searchPage(QStringLiteral("suffix = ? AND path >= ? AND path < ? "),
        { suf, drivePrefix, prefixUpperBound(drivePrefix) },
        sortType, true, curKey, curId, kPageSize);
}

PagedResult FileDatabase::searchFileFolderSync(const QString& keyword, int sortType, const QVariant& curKey, qint64 curId, const QString& drivePrefix)
{
    const QString kw = QStringLiteral("%") + escapeLike(keyword) + QStringLiteral("%");
    if (drivePrefix.isEmpty()) {
        return searchPage(QStringLiteral("name LIKE ? ESCAPE '\\' AND fileType = 1"), { kw }, 
            sortType, true, curKey, curId, kPageSize);
    }
    return searchPage(QStringLiteral("name LIKE ? ESCAPE '\\' AND fileType = 1 AND path >= ? AND path < ? "),
        { kw, drivePrefix, prefixUpperBound(drivePrefix) },
		sortType, true, curKey, curId, kPageSize);
}

QList<FileInfo> FileDatabase::searchFolderContentSync(const QString& folderPath)
{
    QString prefix = folderPath;
    if (!prefix.endsWith(QLatin1Char('\\')))
        prefix += QLatin1Char('\\');
    return runSearchSync(QStringLiteral(
        "SELECT name, path, suffix, size, modifiedTime, fileType FROM files "
        "WHERE path >= ? AND path < ? AND path NOT LIKE ? ESCAPE '\\'"),
        { prefix, prefixUpperBound(prefix), escapeLike(prefix) + QStringLiteral("%\\\\%") });
}

PagedResult FileDatabase::searchAllSync(const QString& keyword, int sortType, const QVariant& curKey, qint64 curId, const QString& drivePrefix)
{
    const QString kw = QStringLiteral("%") + escapeLike(keyword) + QStringLiteral("%");
    if (drivePrefix.isEmpty()) {
        return searchPage(QStringLiteral("name LIKE ? ESCAPE '\\' "),
            { kw }, sortType, true, curKey, curId, kPageSize);
    }
    return searchPage(QStringLiteral("name LIKE ? ESCAPE '\\' AND path >= ? AND path < ? "),
        { kw, drivePrefix, prefixUpperBound(drivePrefix) },
        sortType, true, curKey, curId, kPageSize);
}