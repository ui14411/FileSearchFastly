#include "HeaderFile/FileScanner.h"

#include <qDebug>
#include <QThread>
#include <QTime>
#include <QDateTime>
#include <QAtomicInt>
#include <QSqlQuery>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include "HeaderFile/DriveScanner.h"
#include "HeaderFile/FileDatabase.h"

// 卷序列号
static QString getVolumeSerial(const QString& letter)
{
    const QString root = letter + QLatin1String(":\\");
    DWORD serial = 0;
    if (!GetVolumeInformationW(
            reinterpret_cast<const wchar_t*>(root.utf16()),
            nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
        return QString();
    return QString::number(serial, 16).toUpper();
}

// 探测该盘是否有 USN Journal：NTFS（含 NTFS U 盘）有；FAT/exFAT 移动盘没有
static bool driveHasUsnJournal(const QString& letter)
{
    const QString volPath = QString(2, QChar(92)) + QLatin1String(".") + QChar(92) + letter + QLatin1Char(':');
    HANDLE hVol = CreateFileW(
        reinterpret_cast<const wchar_t*>(volPath.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE)
        return false;
    USN_JOURNAL_DATA jd{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                                    &jd, sizeof(jd), &bytes, nullptr);
    CloseHandle(hVol);
    return ok;
}

FileScanner::FileScanner(QObject* parent, FileDatabase* database)
    :QObject(parent),m_database(database)
{
}

FileScanner::~FileScanner()
{
    // 先停移动盘监视线程
    for (DriveWatcher* w : m_watchers)
    {
        w->stop();
        delete w;
    }
    m_watchers.clear();

    // 先停扫描线程
    for (auto thread : threads) {
        if (thread->isRunning()) {
            thread->quit();
            thread->wait();
        }
    }
    // 再停数据库线程
    if (m_dbThread) {
        m_dbThread->quit();
        m_dbThread->wait();
        delete m_dbThread;
        m_dbThread = nullptr;
    }
}

void FileScanner::scannerFile()
{
    if (!m_database)
    {
        qWarning() << "FileScanner: database为空";
        return;
    }

    if (!m_dbThread)
    {
        m_dbThread = new QThread(this);
        m_database->moveToThread(m_dbThread);
        connect(m_dbThread, &QThread::finished,
                m_database, &QObject::deleteLater);
        m_dbThread->start();
        bool initOk = false;
        QMetaObject::invokeMethod(m_database, [this, &initOk]() {
            initOk = m_database->initDatabase();
        }, Qt::BlockingQueuedConnection);
        if (!initOk)
            qWarning() << "数据库初始化失败";
    }

    QFileInfoList filelist = QDir::drives();

    // 在 dbThread 同步读状态
    bool initialDone = false;
    QHash<QString, quint64> lastUsns;
    QMetaObject::invokeMethod(m_database, [&]() {
        initialDone = m_database->isInitialScanFinished();
        for (const auto& file : filelist)
        {
            const QString letter = file.absolutePath().left(1).toUpper();
            lastUsns.insert(letter, m_database->getLastUsn(letter));
        }
    }, Qt::BlockingQueuedConnection);

    // 增量路径：首次全量已完成 → 只读 USN Journal 变更（Everything 秒开模式）
    if (initialDone)
    {
        QAtomicInt* pending = new QAtomicInt(filelist.size());
        QAtomicInt* pendingBatches = new QAtomicInt(0);

        for (const auto& file : filelist)
        {
            QThread* thread = new QThread();
            threads.append(thread);

            const QString letter = file.absolutePath().left(1).toUpper();

            //  盘符更换检测：同一盘符换了卷（U 盘拔出换插）→ 旧数据+基线全清
            QString oldVolId;
            QMetaObject::invokeMethod(m_database, [&]() {
                oldVolId = m_database->getVolumeId(letter);
            }, Qt::BlockingQueuedConnection);
            const QString volId = getVolumeSerial(letter);
            if (!volId.isEmpty() && !oldVolId.isEmpty() && oldVolId != volId)
            {
                qWarning() << "[增量] 盘符" << letter << "卷已更换:" << oldVolId << "→" << volId << "，清空旧数据";
                QMetaObject::invokeMethod(m_database, [&]() {
                    m_database->clearDrive(letter);
                }, Qt::BlockingQueuedConnection);
                oldVolId.clear();
            }
            if (!volId.isEmpty() && volId != oldVolId)
            {
                QMetaObject::invokeMethod(m_database, [&]() {
                    m_database->setVolumeId(letter, volId);
                }, Qt::BlockingQueuedConnection);
            }

            // 分流：有 USN Journal（NTFS）→ 增量；无（FAT/exFAT 移动盘）→ 目录遍历重建 
            const bool hasJournal = driveHasUsnJournal(letter);
            emit scanDriveStarted(letter, hasJournal);
            DriveScanner* drive = hasJournal
                ? new DriveScanner(nullptr, file, pendingBatches, lastUsns.value(letter))
                : new DriveScanner(nullptr, file, pendingBatches);
            drive->moveToThread(thread);

            if (hasJournal)
            {
                connect(thread, &QThread::started, drive, &DriveScanner::incrementalUsn);
            }
            else
            {
                // 移动盘（无 USN Journal）：FAT/exFAT 无日志，程序关闭期间的变更
                // 无法增量检测 → 每次启动全量遍历重建，保证数据完整（U 盘数据量小，遍历快）
                qWarning() << "[增量] " << letter << " 移动盘全量遍历重建";
                // 清幽灵
                QMetaObject::invokeMethod(m_database, [&]() {
                    m_database->clearDrive(letter);
                }, Qt::BlockingQueuedConnection);
                // startScanner 内部 QUERY 失败 → 自动降级 ScannerFile 目录遍历
                connect(thread, &QThread::started, drive, &DriveScanner::startScanner);
                // 遍历完成后：清 dirty + 记录遍历时间
                connect(drive, &DriveScanner::finished, m_database,
                        [this, letter]() {
                            m_database->setDirty(letter, false);
                            m_database->setLastTraverseTime(letter,
                                QDateTime::currentSecsSinceEpoch());
                        });
            }

            // 启动运行时监视
            if (!hasJournal)
            {
                DriveWatcher* w = new DriveWatcher();
                m_watchers.append(w);
                w->start(letter, [this, letter]() {
                    QMetaObject::invokeMethod(m_database, [this, letter]() {
                        m_database->setDirty(letter, true);
                    }, Qt::QueuedConnection);
                });
            }

            connect(drive, &DriveScanner::sendFileinfo, m_database,
                    [this, pendingBatches](QList<FileInfo> files)
                    {
                        m_database->insertFile(files);
                        pendingBatches->fetchAndSubRelaxed(1);
                    });
            connect(drive, &DriveScanner::sendFileDelete, m_database,
                    [this](const QStringList& paths)
                    {
                        m_database->deleteFiles(paths);
                    });
            connect(drive, &DriveScanner::sendRenamePrefix, m_database,
                    [this](const QString& oldPrefix, const QString& newPrefix)
                    {
                        m_database->renamePrefix(oldPrefix, newPrefix);
                    });
            connect(drive, &DriveScanner::sendLastUsn, m_database,
                    [this](const QString& dr, quint64 usn)
                    {
                        m_database->setLastUsn(dr, usn);
                    });

            connect(drive, &DriveScanner::finished, thread, &QThread::quit);
            connect(drive, &DriveScanner::finished, drive, &QObject::deleteLater);
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);

            connect(drive, &DriveScanner::finished, m_database,
                    [this, pending]()
                    {
                        if (pending->fetchAndSubOrdered(1) == 1)
                        {
                            // qDebug() << "增量扫描完成";  // 调试用
                            emit scanAllFinished();   // 增量完成也通知 UI（隐藏扫描提示）
                            delete pending;
                        }
                    });

            thread->start();
        }
        return;
    }

    // 全量路径：首次扫描（USN 秒级，失败降级目录遍历
    QAtomicInt* pending = new QAtomicInt(filelist.size());
    // 背压共享计数
    QAtomicInt* pendingBatches = new QAtomicInt(0);

    // qDebug() << "开始扫描";  // 调试用

    for (const auto& file : filelist)
    {
        QThread* thread = new QThread();
        threads.append(thread);

        const QString letter = file.absolutePath().left(1).toUpper();
        // 全量扫描也发盘状态（USN 支持 → 加速；U盘 → 较慢）
        emit scanDriveStarted(letter, driveHasUsnJournal(letter));

        DriveScanner* drive =
            new DriveScanner(nullptr, file, pendingBatches);

        drive->moveToThread(thread);

        connect(
            thread,
            &QThread::started,
            drive,
            &DriveScanner::startScanner
        );

        connect(
            drive,
            &DriveScanner::sendFileinfo,
            m_database,
            [this, pendingBatches](QList<FileInfo> files)
            {
                m_database->insertFile(files);
                // 背压：本批已消费，通知扫描线程可以继续
                pendingBatches->fetchAndSubRelaxed(1);
            }
        );

        connect(
            drive,
            &DriveScanner::finished,
            thread,
            &QThread::quit
        );

        connect(
            drive,
            &DriveScanner::finished,
            drive,
            &QObject::deleteLater
        );

        connect(
            thread,
            &QThread::finished,
            thread,
            &QObject::deleteLater
        );

        connect(
            drive,
            &DriveScanner::finished,
            m_database,
            [this, pending]()
            {
                if (pending->fetchAndSubOrdered(1) == 1)
                {
                    m_database->setInitialScanFinished();

                    // qDebug() << "所有磁盘扫描完成";  // 调试用
                    emit scanAllFinished();
                    // qDebug() << "初始索引建立完成";  // 调试用
                    delete pending;
                }
            }
        );

        thread->start();
    }
}

void FileScanner::sortFile(SortType key,QList<FileInfo>& filelist)
{
    switch (key) {
    case SortType::NameAsc:
        std::sort(filelist.begin(), filelist.end(), [](const FileInfo& a, const FileInfo& b) {
            return a.name.toLower() < b.name.toLower();
            });
        break;
    case SortType::NameDesc:
        std::sort(filelist.begin(), filelist.end(), [](const FileInfo& a, const FileInfo& b) {
            return a.name.toLower() > b.name.toLower();
            });
        break;
    case SortType::TimeAsc:
        std::sort(filelist.begin(), filelist.end(), [](const FileInfo& a, const FileInfo& b) {
            return a.modifiedTime.toSecsSinceEpoch() < b.modifiedTime.toSecsSinceEpoch();
            });
        break;
    case SortType::TimeDesc:
        std::sort(filelist.begin(), filelist.end(), [](const FileInfo& a, const FileInfo& b) {
            return a.modifiedTime.toSecsSinceEpoch() > b.modifiedTime.toSecsSinceEpoch();
            });
        break;
    case SortType::SizeAsc:
        std::sort(filelist.begin(), filelist.end(), [](const FileInfo& a, const FileInfo& b) {
            return a.size < b.size;
            });
        break;
    case SortType::SizeDesc:
        std::sort(filelist.begin(), filelist.end(), [](const FileInfo& a, const FileInfo& b) {
            return a.size > b.size;
            });
        break;
    default:
        break;
    }
}