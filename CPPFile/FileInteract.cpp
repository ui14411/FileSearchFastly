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

    connect(m_database, &FileDatabase::sendFile_suffix,
        this, &FileInteract::onDatabaseResult_suffix);

    connect(m_database, &FileDatabase::sendFile_filename,
        this, &FileInteract::onDatabaseResult_filename);

    connect(m_database, &FileDatabase::sendFile_folder,
        this, &FileInteract::onDatabaseResult_folder);

    connect(m_database, &FileDatabase::sendFile_folderContent,
        this, &FileInteract::onDatabaseResult_folderContent);

    connect(m_database, &FileDatabase::sendFile_all,
        this, &FileInteract::onDatabaseResult_all);

    // 搜索已改独立线程+独立只读连接（不排 dbThread 队列）——旧队列连接废弃

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

// 搜索函数，后缀，文件名，文件夹，
void FileInteract::searchBySuffix(const QString& suffix)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    // 独立线程 + 独立只读连接：扫描中也能秒搜（不排 dbThread 队列）
    std::thread([this, suffix]() {
        emit searchResultBySuffix(convertToQVariantList(m_database->searchFileSuffixSync(suffix)));
    }).detach();
}

void FileInteract::searchByFile(const QString& keyword)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    if (keyword.isEmpty()) {
        emit searchFinished(0, "搜索关键词不能为空");
        return;
    }
    std::thread([this, keyword]() {
        emit searchResultByFile(convertToQVariantList(m_database->searchFileFilenameSync(keyword)));
    }).detach();
}

void FileInteract::searchByFolder(const QString& keyword)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    if (keyword.isEmpty()) {
        emit searchFinished(0, "搜索关键词不能为空");
        return;
    }
    std::thread([this, keyword]() {
        emit searchResultByFolder(convertToQVariantList(m_database->searchFileFolderSync(keyword)));
    }).detach();
}

void FileInteract::searchAll(const QString& keyword)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    if (keyword.isEmpty()) {
        emit searchFinished(0, "搜索关键词不能为空");
        return;
    }
    std::thread([this, keyword]() {
        emit searchResultAll(convertToQVariantList(m_database->searchAllSync(keyword)));
    }).detach();
}

//输出目录中的内容
void FileInteract::searchByFolderContent(const QString& folderPath)
{
    if (!m_database) {
        emit searchFinished(0, "数据库未初始化");
        return;
    }
    // 独立线程 + 独立只读连接：扫描中也能秒搜（不排 dbThread 队列）
    std::thread([this, folderPath]() {
        emit searchResultByFolderContent(convertToQVariantList(m_database->searchFolderContentSync(folderPath)));
    }).detach();
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

QString FileInteract::formatSize(qint64 bytes)
{
    if (bytes < 0) return "未知";
    if (bytes < 1024) {
        return QString::number(bytes) + " B";
    }
    else if (bytes < 1024 * 1024) {
        return QString::number(bytes / 1024.0, 'f', 2) + " KB";
    }
    else if (bytes < 1024 * 1024 * 1024) {
        return QString::number(bytes / 1024.0 / 1024.0, 'f', 2) + " MB";
    }
    else {
        return QString::number(bytes / 1024.0 / 1024.0 / 1024.0, 'f', 2) + " GB";
    }
}

QString FileInteract::formatDateTime(const QDateTime& dateTime)
{
    if (!dateTime.isValid()) {
        return "未知";
    }
    return dateTime.toString("yyyy-MM-dd hh:mm:ss");
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

// 排序（在线程池执行，不卡 UI）：sortType 0-5 = 名称↑↓ / 大小↑↓ / 修改时间↑↓
// modifiedTime 是 "yyyy-MM-dd hh:mm:ss" 字符串，字典序 == 时间序
static QVariantList sortResultsImpl(const QVariantList& results, int sortType)
{
    QVector<QVariantMap> maps;
    maps.reserve(results.size());
    for (const QVariant& v : results)
        maps.append(v.toMap());

    auto byName = [](const QVariantMap& a, const QVariantMap& b) {
        return a.value("name").toString().localeAwareCompare(b.value("name").toString());
    };
    auto bySize = [](const QVariantMap& a, const QVariantMap& b) {
        const qint64 sa = a.value("size").toLongLong(), sb = b.value("size").toLongLong();
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    };
    auto byTime = [](const QVariantMap& a, const QVariantMap& b) {
        return a.value("modifiedTime").toString().compare(b.value("modifiedTime").toString());
    };

    auto comparator = [&](const QVariantMap& a, const QVariantMap& b) {
        int r = 0;
        switch (sortType) {
        case 0: r = byName(a, b); break;
        case 1: r = byName(b, a); break;
        case 2: r = bySize(a, b); break;
        case 3: r = bySize(b, a); break;
        case 4: r = byTime(a, b); break;
        default: r = byTime(b, a); break;   // 5: 修改时间↓
        }
        return r < 0;
    };
    std::sort(maps.begin(), maps.end(), comparator);

    QVariantList out;
    out.reserve(maps.size());
    for (QVariantMap& m : maps)
        out.append(m);
    return out;
}

void FileInteract::requestSort(const QVariantList& results, int sortType, int seq)
{
    // 线程池排序；完成信号跨线程队列回 QML（seq 用于丢弃过期结果）
    QThreadPool::globalInstance()->start([this, results, sortType, seq]() {
        const QVariantList sorted = sortResultsImpl(results, sortType);
        emit sortResultReady(sorted, seq);
    });
}

void FileInteract::onDatabaseResult_suffix(const QList<FileInfo>& files)
{
    // 转换放线程池：36000 条构建 QVariantMap 在主线程会卡 UI
    QThreadPool::globalInstance()->start([this, files]() {
        const QVariantList results = convertToQVariantList(files);
        emit searchResultBySuffix(results);
        emit searchFinished(files.size());
    });
}

void FileInteract::onDatabaseResult_filename(const QList<FileInfo>& files)
{
    QThreadPool::globalInstance()->start([this, files]() {
        const QVariantList results = convertToQVariantList(files);
        emit searchResultByFile(results);
        emit searchFinished(files.size());
    });
}

void FileInteract::onDatabaseResult_folder(const QList<FileInfo>& files)
{
    QThreadPool::globalInstance()->start([this, files]() {
        const QVariantList results = convertToQVariantList(files);
        emit searchResultByFolder(results);
        emit searchFinished(files.size());
    });
}

void FileInteract::onDatabaseResult_folderContent(const QList<FileInfo>& files)
{
    QThreadPool::globalInstance()->start([this, files]() {
        const QVariantList results = convertToQVariantList(files);
        emit searchResultByFolderContent(results);
        emit searchFinished(files.size());
    });
}

void FileInteract::onDatabaseResult_all(const QList<FileInfo>& files)
{
    QThreadPool::globalInstance()->start([this, files]() {
        const QVariantList results = convertToQVariantList(files);
        emit searchResultAll(results);
        emit searchFinished(files.size());
    });
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

