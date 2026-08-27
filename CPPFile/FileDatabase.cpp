#include "HeaderFile/FileDatabase.h"

#include <QFileInfo>
#include <QSqlError>
#include <QThread>
#include <math.h>
#include <QCoreApplication>

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
    db.exec("PRAGMA wal_autocheckpoint = 100000;"); // 400MB 才自动 checkpoint
    db.exec("PRAGMA journal_size_limit = 1073741824;"); // WAL 上限 1GB

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

void FileDatabase::searchFile(const QString& keyword, FileSearch type)
{
    switch (type) {
    case FileSearch::Suffix:
        searchFile_suffix(keyword);
        break;
    case FileSearch::File:
        searchFile_filename(keyword);
        break;
    case FileSearch::Folder:
        searchFile_folder(keyword);
        break;
    default:
        break;
    }
}

void FileDatabase::searchFolderContent(const QString& folderPath)
{
    QList<FileInfo> res;

    if (folderPath.isEmpty()) {
        emit sendFile_folderContent(res);
        return;
    }

    QSqlQuery searchQuery(db);

    // 直接子项：path = folderPath\xxx
    // 转义 LIKE 通配符后：前缀 + '%' 匹配子项；排除前缀 + '%' + '\' + '%'
    QString prefix = folderPath;
    if (!prefix.endsWith('\\') && !prefix.endsWith('/'))
        prefix += '\\';
    QString escPrefix = prefix;
    escPrefix.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    const QString childPattern = escPrefix + '%';
    // 排除更深层：% 通配 + 字面反斜杠(\\转义对) + % 通配 —— 原写成 "\%" 是字面百分号，排除失效
    const QString nestedPattern = escPrefix + "%\\\\%";

    searchQuery.prepare(
        R"(
            SELECT name, path, suffix, size, modifiedTime, fileType
            FROM files
            WHERE path LIKE ? ESCAPE '\' AND path NOT LIKE ? ESCAPE '\'
            ORDER BY modifiedTime DESC
        )"
    );
    searchQuery.addBindValue(childPattern);
    searchQuery.addBindValue(nestedPattern);

    if (!searchQuery.exec())
    {
        qWarning() << searchQuery.lastError();
        emit sendFile_folderContent(res);
        return;
    }

    while (searchQuery.next()) {
        FileInfo file;
        file.name = searchQuery.value(0).toString();
        file.path = searchQuery.value(1).toString();
        file.suffix = searchQuery.value(2).toString();
        file.size = searchQuery.value(3).toLongLong();
        file.modifiedTime = QDateTime::fromSecsSinceEpoch(searchQuery.value(4).toLongLong());
        file.isFolder = searchQuery.value(5).toInt() == 1;
        res.append(file);
    }

    emit sendFile_folderContent(res);
}

void FileDatabase::searchFile_suffix(const QString& keyword)
{
    QList<FileInfo> res;
    
    QSqlQuery searchQuery(db);

    searchQuery.prepare(
        R"(
            SELECT name, path, suffix, size, modifiedTime, fileType
            FROM files
            WHERE suffix = ?
           )"
    );
    searchQuery.addBindValue(keyword.toLower());

    if (!searchQuery.exec())
    {
        qWarning() << searchQuery.lastError();
        
        // 🐛 BUG: 搜索出错时不发射任何信号，UI 可能无响应
        // ✅ 修复建议: 无论成功失败，都应发射信号，UI 需要响应
        emit sendFile_suffix(QList<FileInfo>());
        return;
    }

    while (searchQuery.next()) {
        FileInfo file;

        file.name = searchQuery.value(0).toString();
        file.path = searchQuery.value(1).toString();
        file.suffix = searchQuery.value(2).toString();
        file.size = searchQuery.value(3).toLongLong();
        file.modifiedTime = QDateTime::fromSecsSinceEpoch(searchQuery.value(4).toLongLong());
        file.isFolder = searchQuery.value(5).toInt() == 1;

        res.append(file);
    }

    emit sendFile_suffix(res);
}

void FileDatabase::searchFile_filename(const QString& keyword)
{
    QList<FileInfo> res;

    QSqlQuery searchQuery(db);

    searchQuery.prepare(
        R"(
            SELECT name, path, suffix, size, modifiedTime, fileType
            FROM files
            WHERE name LIKE ? ESCAPE '\' AND fileType = 0
           )"
    );
    // 转义 LIKE 通配符，防止用户输入 % 或 _ 导致意外匹配
    QString escaped = keyword;
    escaped.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    searchQuery.addBindValue("%" + escaped + "%");

    if (!searchQuery.exec())
    {
        qWarning() << searchQuery.lastError();
        emit sendFile_filename(QList<FileInfo>());
        return;
    }

    while (searchQuery.next()) {
        FileInfo file;

        file.name = searchQuery.value(0).toString();
        file.path = searchQuery.value(1).toString();
        file.suffix = searchQuery.value(2).toString();
        file.size = searchQuery.value(3).toLongLong();
        file.modifiedTime = QDateTime::fromSecsSinceEpoch(searchQuery.value(4).toLongLong());
        file.isFolder = searchQuery.value(5).toInt() == 1;

        res.append(file);
    }

    emit sendFile_filename(res);
}

void FileDatabase::searchFile_folder(const QString& keyword)
{
    QList<FileInfo> res;

    QSqlQuery searchQuery(db);

    searchQuery.prepare(
        R"(
            SELECT name, path, suffix, size, modifiedTime, fileType
            FROM files
            WHERE name LIKE ? ESCAPE '\' AND fileType = 1
           )"
    );
    // 转义 LIKE 通配符，防止用户输入 % 或 _ 导致意外匹配
    QString escaped = keyword;
    escaped.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    searchQuery.addBindValue("%" + escaped + "%");

    if (!searchQuery.exec())
    {
        qWarning() << searchQuery.lastError();
        emit sendFile_folder(QList<FileInfo>());
        return;
    }

    while (searchQuery.next()) {
        FileInfo file;

        file.name = searchQuery.value(0).toString();
        file.path = searchQuery.value(1).toString();
        file.suffix = searchQuery.value(2).toString();
        file.size = searchQuery.value(3).toLongLong();
        file.modifiedTime = QDateTime::fromSecsSinceEpoch(searchQuery.value(4).toLongLong());
        file.isFolder = searchQuery.value(5).toInt() == 1;

        res.append(file);
    }

    emit sendFile_folder(res);
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

// 全量搜索：名字 LIKE，文件+文件夹全部返回（前端按模式过滤显示）
void FileDatabase::searchAll(const QString& keyword)
{
    QList<FileInfo> res;

    QSqlQuery q(db);
    q.prepare(
        R"(
            SELECT name, path, suffix, size, modifiedTime, fileType
            FROM files
            WHERE name LIKE ?
        )"
    );

    QString escaped = keyword;
    escaped.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    q.addBindValue("%" + escaped + "%");

    if (!q.exec())
    {
        emit sendFile_all(QList<FileInfo>());
        return;
    }

    while (q.next())
    {
        FileInfo file;
        file.name = q.value(0).toString();
        file.path = q.value(1).toString();
        file.suffix = q.value(2).toString();
        file.size = q.value(3).toLongLong();
        file.modifiedTime = QDateTime::fromSecsSinceEpoch(q.value(4).toLongLong());
        file.isFolder = q.value(5).toInt() == 1;
        res.append(file);
    }

    emit sendFile_all(res);
}

// 移动盘变化标记（运行时监视设置，下次启动据此决定是否重建）
bool FileDatabase::getDirty(const QString& drive, bool* hasMark)
{
    QSqlQuery q(db);
    q.prepare("SELECT value FROM metadata WHERE key = ?");
    q.addBindValue("dirty_" + drive);
    if (!q.exec() || !q.next())
    {
        if (hasMark) *hasMark = false;
        return false;
    }
    if (hasMark) *hasMark = true;
    return q.value(0).toString() == "1";
}

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
qint64 FileDatabase::getLastTraverseTime(const QString& drive)
{
    QSqlQuery q(db);
    q.prepare("SELECT value FROM metadata WHERE key = ?");
    q.addBindValue("lastTraverse_" + drive);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

void FileDatabase::setLastTraverseTime(const QString& drive, qint64 ts)
{
    QSqlQuery q(db);
    q.prepare("INSERT OR REPLACE INTO metadata(key, value) VALUES(?, ?)");
    q.addBindValue("lastTraverse_" + drive);
    q.addBindValue(QString::number(ts));
    if (!q.exec())
        qWarning() << "setLastTraverseTime 失败:" << q.lastError();
}

//同步搜索（独立只读连接：WAL 并发读，不排 dbThread 扫描队列
namespace {
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
}

QList<FileInfo> FileDatabase::searchFileSuffixSync(const QString& keyword)
{
    return runSearchSync(QStringLiteral(
        "SELECT name, path, suffix, size, modifiedTime, fileType FROM files WHERE suffix = ?"),
        { keyword.toLower() });
}

QList<FileInfo> FileDatabase::searchFileFilenameSync(const QString& keyword)
{
    return runSearchSync(QStringLiteral(
        "SELECT name, path, suffix, size, modifiedTime, fileType FROM files WHERE name LIKE ? ESCAPE '\\' AND fileType = 0"),
        { "%" + escapeLike(keyword) + "%" });
}

QList<FileInfo> FileDatabase::searchFileFolderSync(const QString& keyword)
{
    return runSearchSync(QStringLiteral(
        "SELECT name, path, suffix, size, modifiedTime, fileType FROM files WHERE name LIKE ? ESCAPE '\\' AND fileType = 1"),
        { "%" + escapeLike(keyword) + "%" });
}

QList<FileInfo> FileDatabase::searchFolderContentSync(const QString& folderPath)
{
    QString prefix = folderPath;
    if (!prefix.endsWith(QLatin1Char('\\')))
        prefix += QLatin1Char('\\');
    return runSearchSync(QStringLiteral(
        "SELECT name, path, suffix, size, modifiedTime, fileType FROM files WHERE path LIKE ? ESCAPE '\\'"),
        { escapeLike(prefix) + "%" });
}

QList<FileInfo> FileDatabase::searchAllSync(const QString& keyword)
{
    return runSearchSync(QStringLiteral(
        "SELECT name, path, suffix, size, modifiedTime, fileType FROM files WHERE name LIKE ? ESCAPE '\\'"),
        { "%" + escapeLike(keyword) + "%" });
}

