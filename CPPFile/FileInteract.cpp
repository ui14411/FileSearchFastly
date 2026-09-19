#include "HeaderFile/FileInteract.h"
#include "HeaderFile/FileDatabase.h"
#include "HeaderFile/FileScanner.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QThreadPool>
#include <algorithm>
#include <thread>
#include <windows.h>

// 检查当前进程是否管理员提权（诊断 USN 为什么失败）
static bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz);
    CloseHandle(token);
    return ok && elev.TokenIsElevated;
}

FileInteract::FileInteract(QObject* parent)
    : QObject(parent)
    , m_database(nullptr)
{
}

FileInteract::~FileInteract()
{
}

bool FileInteract::init()
{
    m_database = new FileDatabase(this);

    m_scanner = new FileScanner(this, m_database);

    // 扫描状态 → 标题提示（必须先 connect：scannerFile 循环里同步 emit 信号）
    connect(m_scanner, &FileScanner::scanDriveStarted,
            this, &FileInteract::onScanDriveStarted);
    connect(m_scanner, &FileScanner::scanAllFinished,
            this, &FileInteract::onScanAllFinished);

    m_scanner->scannerFile();   // 内部创建 dbThread + 队列初始化 + 启动扫描

    // 搜索走独立线程+独立只读连接（不排 dbThread 队列）
    connect(this, &FileInteract::pagedResultReady,
		this, &FileInteract::onPagedResult_all);

    // qDebug() << "FileInteract: 初始化成功";  // 调试用
    qWarning() << "[权限] 是否管理员提权:" << (isElevated() ? "是" : "否");

    return true;
}

// 在资源管理器中定位文件
void FileInteract::showInExplorer(const QString& path)
{
    if (path.isEmpty())
        return;
    const QStringList args = { "/select,", path };
    if (!QProcess::startDetached("explorer", args))
        qWarning() << "打开资源管理器失败:" << path;
}

// 搜索函数，后缀，文件夹，所有文件，处理文件不存在
void FileInteract::searchBySuffix(const QString& keyword, int sortType, const QString& drivePrefix)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    m_keyWord = keyword;
    m_sortType = sortType;
    m_drivePrefix = drivePrefix;
    const int thisSeq = m_seq.fetchAndAddOrdered(1) + 1;
    m_curKey = QVariant();
    QString kw = m_keyWord;
    QString dp = m_drivePrefix;
    QVariant curKey = m_curKey;
    qint64 curId = m_curId;
    int st = m_sortType;
    m_hasMore = false;
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(1);
        return p;
        }();

    pool->start([this,kw,st,dp,curKey,curId, thisSeq]() {
        if (m_seq != thisSeq)
            return;
        PagedResult res = m_database->searchFileSuffixSync(kw, st, curKey, curId, dp);
		QVariantList rows = convertToQVariantList(res.rows);
        if (m_seq != thisSeq)
            return;
        emit pagedResultReady(thisSeq, rows, res.hasMore, res.lastKey, res.lastId, false);
        });
}

void FileInteract::searchByFolder(const QString& keyword, int sortType, const QString& drivePrefix)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    if (keyword.isEmpty()) {
        emit searchFinished(0, "搜索关键词不能为空");
        return;
    }
    m_keyWord = keyword;
    m_sortType = sortType;
    m_drivePrefix = drivePrefix;
    const int thisSeq = m_seq.fetchAndAddOrdered(1) + 1;
    m_curKey = QVariant();
    QString kw = m_keyWord;
    QString dp = m_drivePrefix;
    QVariant curKey = m_curKey;
    qint64 curId = m_curId;
    int st = m_sortType;
    m_hasMore = false;

    static QThreadPool* pool = [] {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(1);
        return p;
        }();

    pool->start([this, kw,dp,curKey,curId,st, thisSeq]() {
        if (m_seq != thisSeq)
            return;
        PagedResult res = m_database->searchFileFolderSync(kw, st, curKey, curId, dp);
		QVariantList rows = convertToQVariantList(res.rows);
        if (m_seq != thisSeq)
            return;
		emit pagedResultReady(thisSeq, rows, res.hasMore, res.lastKey, res.lastId, false);
        });
}

void FileInteract::searchAll(const QString& keyword, int sortType, const QString& drivePrefix)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    m_keyWord = keyword;
    m_sortType = sortType;
    m_drivePrefix = drivePrefix;
    if (m_keyWord.isEmpty()) {
        emit searchFinished(0, "搜索关键词不能为空");
        return;
    }
    const int thisSeq = m_seq.fetchAndAddOrdered(1) + 1;
	m_curKey = QVariant();
	QString kw = m_keyWord;
	QString dp = m_drivePrefix;
	QVariant curKey = m_curKey;
	qint64 curId = m_curId;
	int st = m_sortType;
    m_hasMore = false;

    static QThreadPool* pool = [] {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(1);
        return p;
        }();

    pool->start([this,thisSeq,kw,dp,curKey,st,curId]() {
        if (m_seq != thisSeq)
            return;
        PagedResult res = m_database->searchAllSync(kw,st,curKey,curId, dp);
        QVariantList rows = convertToQVariantList(res.rows);
        if (m_seq != thisSeq)
            return;
        emit pagedResultReady(thisSeq,rows,res.hasMore,res.lastKey,res.lastId, false);
        });
}

 void FileInteract::loadNextPage()
 {
     if (!m_hasMore || m_loading || !m_database)
         return;
	 m_loading = true;
	 const int thisSeq = m_seq.loadAcquire();

     const QString kw = m_keyWord;
     const int st = m_sortType;
     const QString dp = m_drivePrefix;
     const QVariant ck = m_curKey;
     const qint64 cid = m_curId;

     static QThreadPool* pool = [] {
         auto* p = new QThreadPool();
         p->setMaxThreadCount(1);
         return p;
         }();

     pool->start([this, thisSeq, kw, st, dp, ck, cid]() {
         if (m_seq != thisSeq)
             return;
         PagedResult res = m_database->searchAllSync(kw, st, ck, cid, dp);
         QVariantList rows = convertToQVariantList(res.rows);
         if (m_seq != thisSeq)
             return;
         emit pagedResultReady(thisSeq, rows, res.hasMore, res.lastKey, res.lastId, true);   // true = 追加
         });
 }

void FileInteract::onPagedResult_all(int seq, const QVariantList& rows, bool hasMore, const QVariant& lastKey, qint64 lastId,bool append)
{
    if (seq != m_seq.loadAcquire()) return;
    m_curKey = lastKey;          
    m_curId = lastId;
    m_hasMore = hasMore;
    m_loading = false;               
    emit searchResultAll(rows, hasMore,append);
}


//输出目录中的内容
void FileInteract::searchByFolderContent(const QString& folderPath)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    // 独立线程 + 独立只读连接：扫描中也能秒搜（不排 dbThread 队列）

    const int thisSeq = m_seq.fetchAndAddOrdered(1) + 1;
    
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(1);
        return p;
        }();

    pool->start([this,folderPath,thisSeq]() {
        if (m_seq != thisSeq)
            return;
        QVariantList res = convertToQVariantList(m_database->searchFolderContentSync(folderPath));
        if (m_seq != thisSeq)
            return;
        emit searchResultByFolderContent(res);
        });
}

QStringList FileInteract::getDrives() const
{
    QStringList list;
    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo& d : drives)
    {
        const QString letter = d.absolutePath().left(1).toUpper();
        if (!letter.isEmpty() && letter.at(0).isLetter())
            list << letter;
    }
    return list;
}

QVariantList FileInteract::convertToQVariantList(const QList<FileInfo>& files)
{
    QVariantList results;
    for (const auto& file : files) {
        QVariantMap map;
        map["name"] = file.name;
        map["path"] = file.path;
        // 无后缀文件在 db 里是 NULL，QString() 是 null 字符串 → QML 里变 null → toUpper 崩
        map["suffix"] = file.suffix.isNull() ? QStringLiteral("") : file.suffix;
        map["size"] = file.size;
        map["modifiedTime"] = file.modifiedTime.toString("yyyy-MM-dd hh:mm:ss");
        map["isFolder"] = file.isFolder;
        results.append(map);
    }
    return results;
}

// 扫描状态：按盘 USN 支持显示不同文本
void FileInteract::onScanDriveStarted(const QString& letter, bool usnSupported)
{
    const QString text = usnSupported
        ? QStringLiteral("正在扫描 %1 盘（USN 加速）…").arg(letter)
        : QStringLiteral("正在扫描 %1 盘（U盘，较慢）…").arg(letter);
    m_scanStatusText = text;   // 先存属性（QML 加载后绑定可读到）
    qWarning() << "[状态] 扫描提示:" << text;   // 诊断：确认信号链
    emit scanStatusChanged(text);
}

void FileInteract::onScanAllFinished()
{
    // 扫描完成：清空状态 → 标题旁提示隐藏
    m_scanStatusText.clear();
    emit scanStatusChanged(QString());
}

