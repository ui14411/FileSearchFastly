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
    // 诊断：退出路径逐步打点。关窗后进程残留时，scan.log 的最后一行即为卡住的位置。
    qWarning() << "[退出] ~FileScanner 开始 watchers=" << m_watchers.size()
               << " threads=" << threads.size()
               << " dbThread=" << (m_dbThread != nullptr);

    // 先停移动盘监视线程
    for (int i = 0; i < m_watchers.size(); ++i)
    {
        qWarning() << "[退出] watcher" << i << "stop() 进入";
        m_watchers.at(i)->stop();
        qWarning() << "[退出] watcher" << i << "stop() 返回";
        delete m_watchers.at(i);
        qWarning() << "[退出] watcher" << i << "已 delete";
    }
    m_watchers.clear();
    qWarning() << "[退出] watcher 全部处理完毕";

    // 先停扫描线程
    for (int i = 0; i < threads.size(); ++i) {
        QThread* thread = threads.at(i);
        if (thread && thread->isRunning()) {
            qWarning() << "[退出] 扫描线程" << i << "仍在运行，quit+wait";
            thread->quit();
            thread->wait();
            qWarning() << "[退出] 扫描线程" << i << "已退出";
        }
    }
    qWarning() << "[退出] 扫描线程清理完毕";

    // 再停数据库线程
    if (m_dbThread) {
        qWarning() << "[退出] dbThread quit";
        m_dbThread->quit();
        m_dbThread->wait();
        qWarning() << "[退出] dbThread wait 返回，delete";
        delete m_dbThread;
        m_dbThread = nullptr;
    }
    qWarning() << "[退出] ~FileScanner 完成";
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

    // 增量路径：首次全量已完成 只读 USN Journal 变更
    if (initialDone)
    {
        QAtomicInt* pending = new QAtomicInt(filelist.size());
        QAtomicInt* pendingBatches = new QAtomicInt(0);

        for (const auto& file : filelist)
        {
            const QString letter = file.absolutePath().left(1).toUpper();

            //  盘符更换检测：同一盘符换了卷（U 盘拔出换插) 旧数据+基线全清
            QString oldVolId;
            QMetaObject::invokeMethod(m_database, [&]() {
                oldVolId = m_database->getVolumeId(letter);
                }, Qt::BlockingQueuedConnection);
            const QString volId = getVolumeSerial(letter);
            if (!volId.isEmpty() && !oldVolId.isEmpty() && oldVolId != volId)
            {
                qWarning() << "[增量] 盘符" << letter << "卷已更换:" << oldVolId << "--" << volId << "，清空旧数据";
                QMetaObject::invokeMethod(m_database, [=]() {
                    m_database->clearDrive(letter);
                    }, Qt::QueuedConnection);
                oldVolId.clear();
            }
            if (!volId.isEmpty() && volId != oldVolId)
            {
                QMetaObject::invokeMethod(m_database, [=]() {
                    m_database->setVolumeId(letter, volId);
                    }, Qt::QueuedConnection);
            }
            auto wireScanner = [this, pendingBatches](DriveScanner* s, QThread* t)
                {
                    connect(s, &DriveScanner::sendFileinfo, m_database,
                        [this, pendingBatches](QList<FileInfo> files)
                        {
                            m_database->insertFile(files);
                            pendingBatches->fetchAndSubRelaxed(1);
                        });
                    connect(s, &DriveScanner::sendFileDelete, m_database,
                        [this](const QStringList& paths) { m_database->deleteFiles(paths); });
                    connect(s, &DriveScanner::sendRenamePrefix, m_database,
                        [this](const QString& o, const QString& n) { m_database->renamePrefix(o, n); });
                    connect(s, &DriveScanner::sendLastUsn, m_database,
                        [this](const QString& d, quint64 u) { m_database->setLastUsn(d, u); });
                    connect(s, &DriveScanner::finished, t, &QThread::quit);
                    connect(s, &DriveScanner::finished, s, &QObject::deleteLater);
                    connect(t, &QThread::finished, t, &QObject::deleteLater);
                };

            // 全量重建某盘：清幽灵 → USN 枚举(失败自动降级遍历) → 记录遍历时间
            // pending 传 nullptr = 不计入每盘完成计数（NTFS 兜底：该盘的增量扫描器已经减过一次）
            // pending 传实指针 = 计入（无日志盘：这是该盘唯一的扫描器，不减则计数永不归零）
            auto fullRebuild = [this, wireScanner, pendingBatches](const QString& letter, QAtomicInt* pending)
                {
                    qWarning() << "[增量]" << letter << "全量重建";
                    QMetaObject::invokeMethod(m_database, [=]() {
                        m_database->clearDrive(letter);
                        }, Qt::QueuedConnection);

                    QThread* t = new QThread();
                    threads.append(t);
                    DriveScanner* s = new DriveScanner(nullptr, QFileInfo(letter + ":\\"), pendingBatches);
                    s->moveToThread(t);
                    wireScanner(s, t);
                    connect(t, &QThread::started, s, &DriveScanner::startScanner);
                    connect(s, &DriveScanner::finished, m_database, [this, letter]() {
                        m_database->setDirty(letter, false);
                        m_database->setLastTraverseTime(letter,
                            QDateTime::currentSecsSinceEpoch());
                        });
                    if (pending)
                        connect(s, &DriveScanner::finished, m_database,
                            [this, pending]() {
                                if (pending->fetchAndSubOrdered(1) == 1)
                                {
                                    emit scanAllFinished();
                                    delete pending;
                                }
                            });
                    t->start();
                };

            // 分流：有 USN Journal（NTFS）→ 增量；无（FAT/exFAT 移动盘）→ 目录遍历重建 
            const bool hasJournal = driveHasUsnJournal(letter);
            emit scanDriveStarted(letter, hasJournal);

            if (hasJournal)
            {
                QThread* thread = new QThread();
                threads.append(thread);

                DriveScanner* drive = new DriveScanner(nullptr, file, pendingBatches,
                    lastUsns.value(letter));
                drive->moveToThread(thread);
                wireScanner(drive, thread);

                connect(thread, &QThread::started, drive, &DriveScanner::incrementalUsn);
                // 基线作废（日志绕回 / 被重建）→ 该盘全量重建一次；不计入 pending
                connect(drive, &DriveScanner::sendNeedFullRescan, this,
                    [fullRebuild](const QString& l) { fullRebuild(l, nullptr); });

                // 每盘一次的完成计数（兜底那次不重复计）
                connect(drive, &DriveScanner::finished, m_database,
                    [this, pending]()
                    {
                        if (pending->fetchAndSubOrdered(1) == 1)
                        {
                            emit scanAllFinished();   // 增量完成也通知 UI（隐藏扫描提示）
                            delete pending;
                        }
                    });

                thread->start();
            }
            else
            {
                // 移动盘（无 USN Journal）：FAT/exFAT 无日志，程序关闭期间的变更
                // 无法增量检测 → 每次启动全量遍历重建，保证数据完整（U 盘数据量小，遍历快）
                qWarning() << "[增量] " << letter << " 移动盘全量遍历重建";
                // 这是该盘唯一的扫描器 → 必须计入 pending，否则 scanAllFinished 永不触发
                fullRebuild(letter, pending);

                // 启动运行时监视
                DriveWatcher* w = new DriveWatcher();
                m_watchers.append(w);
                w->start(letter, [this, letter]() {
                    QMetaObject::invokeMethod(m_database, [this, letter]() {
                        m_database->setDirty(letter, true);
                        }, Qt::QueuedConnection);
                    });
            }

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