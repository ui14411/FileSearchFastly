#include "HeaderFile/DriveScanner.h"

#include <QDir>
#include <QDebug>
#include <QDateTime>
#include <QThread>
#include <set>

// 方案B：USN 全量枚举需要 Win32 卷 API
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

// FILETIME → QDateTime（1601 epoch → 1970 epoch，100ns 间隔）
QDateTime FILETIME_to_QDateTime(const FILETIME& ft)
{
    ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (t == 0)
        return QDateTime();
    return QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(t / 10000) - 11644473600000LL);
}

// 构造函数已在头文件内联（带背压计数参数）

// 前向声明：解析函数定义在下方，resolveDirPath 先使用
static bool parseFileNameAttr(const BYTE* rec, DWORD recLen,
                              quint64& parentFrn, quint64& fileSize,
                              qint64& mtime, quint32& fileAttrs, QString& name);

// USN 卷枚举互斥定义
QMutex DriveScanner::s_usnMutex;

// 与全量一致的名字过滤：$ 开头、回收站、系统卷信息
static bool isSkipName(const QString& name)
{
    return name == QLatin1String("System Volume Information")
        || name == QLatin1String("$RECYCLE.BIN")
        || name.startsWith(QLatin1Char('$'));
}

// FILETIME(100ns 自 1601) → Unix 毫秒
static qint64 filetimeToMsec(qint64 ft)
{
    return ft / 10000 - 11644473600000LL;
}

// FRN 回溯目录完整路径（断链返回空串）
static QString resolveDirPath(HANDLE hVol, quint64 frn,
                              QHash<quint64, QString>& dirCache,
                              const QString& rootPath)
{
    auto it = dirCache.constFind(frn);
    if (it != dirCache.constEnd())
        return it.value();

    QVector<quint64> chain;              // 自底向上：需要解析名字的 FRN
    QHash<quint64, QString> chainNames;  // FRN → 目录名
    quint64 cur = frn;
    QByteArray recBuf(64 * 1024, Qt::Uninitialized);

    int guard = 0;
    while (!dirCache.contains(cur) && guard++ < 64)
    {
        chain.append(cur);
        LONGLONG inFrn = static_cast<LONGLONG>(cur & 0xFFFFFFFFFFFFULL);
        DWORD got = 0;
        if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_FILE_RECORD, &inFrn, sizeof(inFrn),
                             recBuf.data(), static_cast<DWORD>(recBuf.size()), &got, nullptr))
            return QString();           // 记录已删/不可读 → 链断
        if (got < 24)
            return QString();
        const BYTE* rec = reinterpret_cast<const BYTE*>(recBuf.data()) + 12;
        quint64 parent = 0, size = 0;
        qint64 mtime = 0;
        quint32 attrs = 0;
        QString name;
        if (!parseFileNameAttr(rec, got - 12, parent, size, mtime, attrs, name))
            return QString();
        chainNames.insert(cur, name);
        cur = parent & 0xFFFFFFFFFFFFULL;
    }
    if (!dirCache.contains(cur))
        return QString();               // 没回到已知根 → 断链

    QString p = dirCache.value(cur);
    if (p.endsWith(QLatin1Char(92)))
        p.chop(1);
    for (int i = chain.size() - 1; i >= 0; --i)
    {
        p += QLatin1Char(92) + chainNames.value(chain.at(i));
        dirCache.insert(chain.at(i), p);
    }
    return dirCache.value(frn);
}

void DriveScanner::startScanner()
{
    // 优先 USN 全量枚举（管理员秒级），失败自动降级目录遍历
    if (!scannerUsn())
    {
    // 降级：目录遍历（统一原生分隔符）
        dirs.push_back(QDir::toNativeSeparators(m_drive.absoluteFilePath()).toStdWString());
        scannerFile();
    }
    emit finished();
}

// NTFS USN 全量枚举（需管理员；失败返回 false 由调用方降级遍历）
// 启用备份特权：FSCTL_GET_NTFS_FILE_RECORD 必需
static bool enableBackupPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME,
                                    &tp.Privileges[0].Luid);
    DWORD e = 0;
    if (ok)
    {
        ok = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        e = GetLastError();          // ERROR_SUCCESS = 启用成功
    }
    CloseHandle(hToken);
    return ok && e == ERROR_SUCCESS;
}

// 解析 $FILE_NAME（多条时优先长名，短名特征=含 ~）
static bool parseFileNameAttr(const BYTE* rec, DWORD recLen,
                              quint64& parentFrn, quint64& fileSize,
                              qint64& mtime, quint32& fileAttrs, QString& name)
{
    // FILE_RECORD_HEADER: "FILE" @0, 第一个属性偏移 @0x14
    if (recLen < 0x38 || memcmp(rec, "FILE", 4) != 0)
        return false;
    const USHORT attrOff = *reinterpret_cast<const USHORT*>(rec + 0x14);
    if (attrOff == 0 || attrOff >= recLen)
        return false;

    bool found = false;
    QString bestName;
    quint64 bestParent = 0, bestSize = 0;
    qint64 bestMtime = 0;
    quint32 bestAttrs = 0;

    const BYTE* p = rec + attrOff;
    const BYTE* end = rec + recLen;
    while (p + 8 <= end)
    {
        const DWORD type = *reinterpret_cast<const DWORD*>(p);
        const DWORD len = *reinterpret_cast<const DWORD*>(p + 4);
        if (len < 24 || p + len > end)
            break;
        if (type == 0xFFFFFFFF)   // 属性列表结束
            break;
        if (type == 0x30)         // $FILE_NAME
        {
            const BYTE nonResident = p[8];
            if (nonResident == 0 && len >= 0x18 + 0x42)
            {
                // resident 属性头：ValueLength@0x10、ValueOffset@0x14
                const DWORD dataLen = *reinterpret_cast<const DWORD*>(p + 0x10);
                const USHORT dataOff = *reinterpret_cast<const USHORT*>(p + 0x14);
                if (dataOff + dataLen <= len && dataLen >= 0x42)
                {
                    const BYTE* d = p + dataOff;
                    const quint64 parent = *reinterpret_cast<const quint64*>(d);
                    const quint64 size = *reinterpret_cast<const quint64*>(d + 0x30);
                    const qint64 mt = *reinterpret_cast<const qint64*>(d + 0x10);
                    const quint32 attrs = *reinterpret_cast<const quint32*>(d + 0x38);
                    const quint8 nameLen = d[0x40];
                    if (0x42 + static_cast<int>(nameLen) * 2 <= static_cast<int>(dataLen))
                    {
                        const QString nm = QString::fromWCharArray(
                            reinterpret_cast<const wchar_t*>(d + 0x42), nameLen);
                        const bool nmIsShort = nm.indexOf(QLatin1Char('~')) >= 0;
                        const bool bestIsShort = bestName.indexOf(QLatin1Char('~')) >= 0;
                        const bool better = !found
                            || (!nmIsShort && bestIsShort)   // 长名优于短名
                            || (nmIsShort == bestIsShort && nm.size() > bestName.size());
                        if (better)
                        {
                            bestName = nm;
                            bestParent = parent;
                            bestSize = size;
                            bestMtime = mt;
                            bestAttrs = attrs;
                            found = true;
                        }
                    }
                }
            }
            // 不 return：继续遍历，可能还有第二个 $FILE_NAME（长名/短名）
        }
        p += len;
    }
    if (found)
    {
        parentFrn = bestParent;
        fileSize = bestSize;
        mtime = bestMtime;
        fileAttrs = bestAttrs;
        name = bestName;
        return true;
    }
    return false;
}

namespace {

struct RecInfo { quint64 frn; quint64 parent; quint64 size; qint64 mtime; quint32 attrs; QString name; };

// 1) ENUM_USN_DATA 拿全量 FRN 列表（卷级游标只能枚举一次）
bool enumerateUsnFrns(HANDLE hVol, const USN_JOURNAL_DATA& journalData, QByteArray& buffer,
                      QVector<quint64>& frnList, qint64& totalRecs)
{
    MFT_ENUM_DATA_V1 med{};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;
    med.MinMajorVersion = 2;
    med.MaxMajorVersion = journalData.MaxSupportedMajorVersion;

    for (;;)
    {
        DWORD got = 0;
        bool okRead = false;
        for (int retry = 0; retry < 3; ++retry)
        {
            got = 0;
            okRead = DeviceIoControl(hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                                     buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr);
            if (okRead)
                break;
            DWORD e = GetLastError();
            if (e == 38 || e == 0)
                break;
            QThread::msleep(20);
        }
        if (!okRead)
            break;
        if (got <= sizeof(ULONGLONG))
            break;

        const BYTE* p = reinterpret_cast<const BYTE*>(buffer.data()) + sizeof(ULONGLONG);
        const BYTE* end = reinterpret_cast<const BYTE*>(buffer.data()) + got;
        while (p + 16 <= end)
        {
            const DWORD recLen = *reinterpret_cast<const DWORD*>(p);
            if (recLen < 16 || p + recLen > end)
                break;
            frnList.append(*reinterpret_cast<const quint64*>(p + 8));
            ++totalRecs;
            p += recLen;
        }
        med.StartFileReferenceNumber = *reinterpret_cast<ULONGLONG*>(buffer.data());
    }
    return totalRecs > 0;
}

// 2) 逐 FRN 读 MFT 记录（FSCTL_GET_NTFS_FILE_RECORD），解析 $FILE_NAME，填充目录表/文件表
void readMftRecords(HANDLE hVol, const QVector<quint64>& frnList, QByteArray& recBuf,
                    QHash<quint64, QPair<QString, quint64>>& dirTable, QVector<RecInfo>& files,
                    qint64& dirs, qint64& recFail, qint64& parseFail, qint64& gotSmall, qint64& diagShown,
                    const std::function<bool(const QString&)>& isSkipName)
{
    for (const quint64 frn : frnList)
    {
        // 只传低 48 位 MFT 记录号（高 16 位是序列号）
        LONGLONG inFrn = static_cast<LONGLONG>(frn & 0xFFFFFFFFFFFFULL);
        DWORD got = 0;
        if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_FILE_RECORD, &inFrn, sizeof(inFrn),
                             recBuf.data(), static_cast<DWORD>(recBuf.size()), &got, nullptr))
        {
            const DWORD e = GetLastError();
            if (recFail < 5)   // 只打前 5 条，别刷屏
                qWarning() << "[USN] GET_NTFS_FILE_RECORD 失败 frn=" << frn << "err=" << e;
            ++recFail;
            continue;
        }
        if (got < 24)
        {
            ++gotSmall;
            continue;
        }
        const BYTE* rec = reinterpret_cast<const BYTE*>(recBuf.data()) + 12;   // OUTPUT_BUFFER 头部 12 字节
        const DWORD recLen = got - 12;

        quint64 parent = 0, size = 0;
        qint64 mtime = 0;
        quint32 attrs = 0;
        QString name;
        if (!parseFileNameAttr(rec, recLen, parent, size, mtime, attrs, name))
        {
            ++parseFail;
            continue;
        }
        if (isSkipName(name))
            continue;

        if (diagShown < 3)   // 打印前 3 条成功解析样本，验证名字/父链正确
        {
            qWarning() << "[USN] 样本 frn=" << frn << "name=" << name
                       << "parent=" << parent
                       << "isDir=" << ((attrs & 0x10000000) != 0);
            ++diagShown;
        }

        const bool isDir = (attrs & 0x10000000) != 0;
        if (isDir)
        {
            dirTable.insert(frn & 0xFFFFFFFFFFFFULL,
                            QPair<QString, quint64>(name, parent & 0xFFFFFFFFFFFFULL));
            ++dirs;
        }
        else
        {
            RecInfo ri;
            ri.frn = frn; ri.parent = parent; ri.size = size;
            ri.mtime = mtime; ri.attrs = attrs; ri.name = name;
            files.append(ri);
        }
    }
}

// 3) 回溯路径：文件 + 目录构建 FileInfo，批量回调发出（emitBatch 由调用方提供）
void buildFileInfos(const QHash<quint64, QPair<QString, quint64>>& dirTable,
                    const QVector<RecInfo>& files, const QString& rootPath,
                    const std::function<void(QList<FileInfo>&)>& emitBatch,
                    bool& anyEnumerated, qint64& pass2Recs)
{
    constexpr int kBatchSize = 5000;   // 调小批量：dbThread 单连接串行，搜索最多等几十 ms
    QList<FileInfo> batch;
    batch.reserve(kBatchSize);
    auto flush = [&]() {
        if (!batch.isEmpty()) {
            emitBatch(batch);
            batch.clear();
        }
    };

    QHash<quint64, QString> pathCache;
    pathCache.insert(5, rootPath);
    qint64 pathShown = 0;   // 已打印回溯成功样本数（诊断用）

    for (const RecInfo& ri : files)
    {
        // 路径缓存：父目录已缓存则 O(1)，否则回溯并逐级缓存链上目录
        QString fullPath;
        const quint64 parentKey = ri.parent & 0xFFFFFFFFFFFFULL;
        auto pcIt = pathCache.constFind(parentKey);
        if (pcIt != pathCache.constEnd())
        {
            fullPath = pcIt.value();
            if (fullPath.endsWith(QLatin1Char(92)))
                fullPath.chop(1);
        }
        else
        {
            QStringList dirNames;
            QVector<quint64> dirFrns;
            quint64 cur = parentKey;
            int guard = 0;
            while (!pathCache.contains(cur) && guard++ < 64)
            {
                auto it = dirTable.constFind(cur);
                if (it == dirTable.constEnd())
                    break;
                dirNames.append(it->first);
                dirFrns.append(cur);
                cur = it->second;
            }
            if (!pathCache.contains(cur))
                continue;   // 父链断裂，跳过

            QString dirPath = pathCache.value(cur);
            if (dirPath.endsWith(QLatin1Char(92)))
                dirPath.chop(1);
            for (int i = dirFrns.size() - 1; i >= 0; --i)
            {
                dirPath += QLatin1Char(92) + dirNames.at(i);
                pathCache.insert(dirFrns.at(i), dirPath);
            }
            fullPath = dirPath;
        }
        fullPath += QLatin1Char(92) + ri.name;

        FileInfo file;
        file.name = ri.name;
        file.path = fullPath;
        file.isFolder = false;

        if (pathShown < 2)
        {
            qWarning() << "[USN] 路径样本=" << fullPath;
            ++pathShown;
        }
        file.modifiedTime = QDateTime::fromSecsSinceEpoch(
            ri.mtime / 10000000LL - 11644473600LL);
        const int dot = ri.name.lastIndexOf(QLatin1Char('.'));
        file.suffix = (dot > 0) ? ri.name.mid(dot + 1).toLower() : QString();
        file.size = static_cast<qint64>(ri.size);

        batch.append(file);
        ++pass2Recs;
        anyEnumerated = true;
        if (batch.size() >= kBatchSize)
            flush();
    }

    // 目录入库：dirTable 里的目录也回溯完整路径并发送 isFolder=true
    int dirSent = 0;
    for (auto dit = dirTable.constBegin(); dit != dirTable.constEnd(); ++dit)
    {
        const QString dirName = dit.value().first;
        if (dirName.isEmpty())
            continue;   // 根目录（名字空）不入库
        QStringList parts;
        quint64 cur = dit.value().second;
        int guard = 0;
        while (!pathCache.contains(cur) && guard < 64)
        {
            auto it2 = dirTable.constFind(cur);
            if (it2 == dirTable.constEnd())
                break;
            parts.prepend(it2.value().first);
            cur = it2.value().second;
            ++guard;
        }
        if (!pathCache.contains(cur))
            continue;   // 父链断裂（罕见），跳过
        QString dirPath = pathCache.value(cur);
        if (dirPath.endsWith(QLatin1Char(92)))
            dirPath.chop(1);
        for (int i = parts.size() - 1; i >= 0; --i)
            dirPath += QLatin1Char(92) + parts.at(i);

        FileInfo d;
        d.name = dirName;
        d.path = dirPath + QLatin1Char(92) + dirName;
        d.isFolder = true;
        // 用 GetFileAttributesExW 取目录时间（QFileInfo 每次构造=一次 stat，几十万目录是性能杀手）
        {
            WIN32_FILE_ATTRIBUTE_DATA ad{};
            if (GetFileAttributesExW(d.path.toStdWString().c_str(),
                                     GetFileExInfoStandard, &ad))
                d.modifiedTime = FILETIME_to_QDateTime(ad.ftLastWriteTime);
            else
                d.modifiedTime = QDateTime::fromSecsSinceEpoch(0);
        }
        d.suffix = QString();
        d.size = 0;
        batch.append(d);
        ++dirSent;
        if (batch.size() >= kBatchSize)
            flush();
    }
    qWarning() << "[USN] 目录入库: " << dirSent;
    flush();
}

} // namespace

bool DriveScanner::scannerUsn()
{
    const QString rootPath = QDir::toNativeSeparators(m_drive.absolutePath());
    if (rootPath.isEmpty() || rootPath.size() < 2 || rootPath.at(1) != QLatin1Char(':'))
        return false;
    if (!rootPath.endsWith(QLatin1Char(92)))
        return false;
    const QChar driveLetter = rootPath.at(0);

    QMutexLocker locker(&s_usnMutex);

    // 分阶段计时
    QElapsedTimer timer;
    timer.start();

    // 关键：启用备份特权（不启用则 GET_NTFS_FILE_RECORD 全部 err=5）
    qWarning() << "[USN] SeBackupPrivilege 启用:" << enableBackupPrivilege();

    const QString volPath = QString(2, QChar(92)) + QLatin1String(".") + QChar(92) + driveLetter + QLatin1Char(':');
    HANDLE hVol = CreateFileW(
        reinterpret_cast<const wchar_t*>(volPath.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE)
    {
        qWarning() << "[USN] 打开卷失败" << volPath << "err=" << GetLastError();
        return false;
    }

    USN_JOURNAL_DATA journalData{};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                              &journalData, sizeof(journalData), &bytes, nullptr);
    if (!ok)
    {
        qWarning() << "[USN] FSCTL_QUERY_USN_JOURNAL 失败 err=" << GetLastError() << "volPath=" << volPath;
        CloseHandle(hVol);
        return false;
    }

    constexpr DWORD BUF_SIZE = 1024 * 1024;
    QByteArray buffer(BUF_SIZE, Qt::Uninitialized);

    // 目录表：FRN → (名字, 父FRN)；根目录 FRN=5
    QHash<quint64, QPair<QString, quint64>> dirTable;
    dirTable.insert(5, QPair<QString, quint64>(QString(), 0));
    qint64 totalRecs = 0;

    auto isSkipName = [](const QString& name) -> bool {
        return name == QLatin1String("System Volume Information")
            || name == QLatin1String("$RECYCLE.BIN")
            || name.startsWith(QLatin1Char('$'));
    };

    // 1) ENUM 拿全量 FRN 列表（诊断实测：记录含 FRN @8，卷级游标只能枚举一次）
    QVector<quint64> frnList;
    frnList.reserve(3000000);
    enumerateUsnFrns(hVol, journalData, buffer, frnList, totalRecs);
    qWarning() << "[USN] ENUM 完成: 记录=" << totalRecs << "耗时=" << timer.elapsed() << "ms";

    // 2) 逐 FRN 读 MFT 记录（FSCTL_GET_NTFS_FILE_RECORD，诊断实测可用）
    QByteArray recBuf(64 * 1024, Qt::Uninitialized);
    QVector<RecInfo> files;   // 非目录记录
    files.reserve(2000000);
    qint64 dirs = 0;
    qint64 recFail = 0;    // 读记录失败统计（诊断用）
    qint64 parseFail = 0;  // IOCTL 成功但 $FILE_NAME 解析失败（诊断用）
    qint64 gotSmall = 0;   // got < 24（诊断用）
    qint64 diagShown = 0;  // 已打印成功样本数
    readMftRecords(hVol, frnList, recBuf, dirTable, files, dirs,
                   recFail, parseFail, gotSmall, diagShown, isSkipName);
    qWarning() << "[USN] FSCTL 完成: 目录=" << dirs << "文件=" << files.size()
               << "读记录失败=" << recFail << "解析失败=" << parseFail
               << "got<24=" << gotSmall << "耗时=" << timer.elapsed() << "ms";

    // 3) 回溯输出（目录表完整）：文件 + 目录构建 FileInfo 批量发送
    auto emitBatch = [this](QList<FileInfo>& batch)
    {
        if (m_pendingBatches)
        {
            while (m_pendingBatches->loadAcquire() >= 30)
                QThread::msleep(2);
            m_pendingBatches->fetchAndAddRelaxed(1);
        }
        emit sendFileinfo(batch);
    };
    bool anyEnumerated = false;
    qint64 pass2Recs = 0;
    buildFileInfos(dirTable, files, rootPath, emitBatch, anyEnumerated, pass2Recs);
    qWarning() << "[USN] 回溯完成: 已入库=" << pass2Recs << "耗时=" << timer.elapsed() << "ms";

    CloseHandle(hVol);
    qWarning() << "[USN] 完成: anyEnumerated=" << anyEnumerated << "总耗时=" << timer.elapsed() << "ms";
    if (anyEnumerated)
    {
        // 增量基线：全量完成时刻的 journal 游标，下次启动从这里读变更
        emit sendLastUsn(QString(driveLetter), journalData.NextUsn);
    }
    return anyEnumerated;
}

// 增量扫描：从上次 Usn 游标读 USN Journal（需管理员，无基线时跳过）
void DriveScanner::incrementalUsn()
{
    const QString rootPath = QDir::toNativeSeparators(m_drive.absolutePath());
    if (rootPath.isEmpty() || rootPath.size() < 2 || rootPath.at(1) != QLatin1Char(':'))
    {
        emit finished();
        return;
    }
    if (!rootPath.endsWith(QLatin1Char(92)))
    {
        emit finished();
        return;
    }
    const QChar driveLetter = rootPath.at(0);

    QMutexLocker locker(&s_usnMutex);

    const QString volPath = QString(2, QChar(92)) + QLatin1String(".") + QChar(92) + driveLetter + QLatin1Char(':');
    HANDLE hVol = CreateFileW(
        reinterpret_cast<const wchar_t*>(volPath.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE)
    {
        emit finished();
        return;
    }

    USN_JOURNAL_DATA jd{};
    DWORD bytes = 0;
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                         &jd, sizeof(jd), &bytes, nullptr))
    {
        // U 盘等无 journal → 无增量，跳过
        CloseHandle(hVol);
        emit finished();
        return;
    }

    if (m_lastUsn == 0)
    {
        // 无基线（降级遍历后首次启动等）：只建立基线，不处理历史
        qWarning() << "[USN] 增量: " << driveLetter << " 无基线, 建立基线 NextUsn=" << jd.NextUsn;
        emit sendLastUsn(QString(driveLetter), jd.NextUsn);
        CloseHandle(hVol);
        emit finished();
        return;
    }

    qWarning() << "[USN] 增量开始: " << driveLetter << " from Usn=" << m_lastUsn;

    constexpr DWORD BUF_SIZE = 1024 * 1024;
    QByteArray buffer(BUF_SIZE, Qt::Uninitialized);

    READ_USN_JOURNAL_DATA rj{};
    rj.StartUsn = m_lastUsn;
    rj.ReasonMask = 0xFFFFFFFF;
    rj.UsnJournalID = jd.UsnJournalID;
    rj.MinMajorVersion = 2;
    rj.MaxMajorVersion = 2;   // 强制 V2 标准记录布局

    QHash<quint64, QString> dirCache;   // FRN(低48位) → 目录完整路径
    dirCache.insert(5, rootPath);
    QHash<quint64, QString> pendingRenames;   // FRN → 旧路径（RENAME_OLD 缓存，等 NEW 配对）

    QList<FileInfo> upserts;
    QStringList deletes;
    QList<QPair<QString, QString>> renames;   // (旧前缀, 新前缀)
    qint64 recCount = 0;
    qint64 upsertCount = 0, deleteCount = 0, renameCount = 0;

    auto flushAll = [&]()
    {
        if (!deletes.isEmpty())
        {
            emit sendFileDelete(deletes);
            deletes.clear();
        }
        for (const auto& rn : renames)
            emit sendRenamePrefix(rn.first, rn.second);
        renames.clear();
        if (!upserts.isEmpty())
        {
            if (m_pendingBatches)
            {
                while (m_pendingBatches->loadAcquire() >= 30)
                    QThread::msleep(2);
                m_pendingBatches->fetchAndAddRelaxed(1);
            }
            emit sendFileinfo(upserts);
            upserts.clear();
        }
    };

    for (;;)
    {
        DWORD got = 0;
        if (!DeviceIoControl(hVol, FSCTL_READ_USN_JOURNAL, &rj, sizeof(rj),
                             buffer.data(), BUF_SIZE, &got, nullptr))
        {
            const DWORD e = GetLastError();
            qWarning() << "[USN] 增量 READ 失败 err=" << e;
            break;   // 保守：不推进基线，下次重读
        }
        if (got <= sizeof(USN))
            break;   // 读完

        const USN nextUsn = *reinterpret_cast<const USN*>(buffer.data());
        const BYTE* p = reinterpret_cast<const BYTE*>(buffer.data()) + sizeof(USN);
        const BYTE* end = reinterpret_cast<const BYTE*>(buffer.data()) + got;

        while (p + sizeof(USN_RECORD_V2) <= end)
        {
            const USN_RECORD_V2* rec = reinterpret_cast<const USN_RECORD_V2*>(p);
            if (rec->RecordLength < sizeof(USN_RECORD_V2) || p + rec->RecordLength > end)
                break;
            if (rec->MajorVersion == 2 && rec->FileNameLength > 0)
            {
                const QString name = QString::fromWCharArray(
                    reinterpret_cast<const wchar_t*>(p + rec->FileNameOffset),
                    rec->FileNameLength / 2);
                if (!isSkipName(name))
                {
                    const quint64 frn = rec->FileReferenceNumber & 0xFFFFFFFFFFFFULL;
                    const quint64 parent = rec->ParentFileReferenceNumber & 0xFFFFFFFFFFFFULL;
                    const bool isDir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    const DWORD reason = rec->Reason;

                    const QString dirPath = resolveDirPath(hVol, parent, dirCache, rootPath);
                    if (dirPath.isEmpty())
                    {
                        p += rec->RecordLength;
                        continue;   // 父链断（父目录已被删/不可读）
                    }
                    const QString fullPath = dirPath + QLatin1Char(92) + name;

                    const bool isDelete  = (reason & USN_REASON_FILE_DELETE) != 0;
                    const bool isRenOld  = (reason & USN_REASON_RENAME_OLD_NAME) != 0;
                    const bool isRenNew  = (reason & USN_REASON_RENAME_NEW_NAME) != 0;

                    if (isDelete)
                    {
                        deletes.append(fullPath);
                        ++deleteCount;
                    }
                    else if (isRenOld)
                    {
                        // 目录改名：不删旧路径（renamePrefix 会迁移整棵子树）；
                        // 先删会导致子树记录消失、renamePrefix 落空（数据丢失）
                        pendingRenames.insert(frn, fullPath);
                        if (!isDir)
                        {
                            deletes.append(fullPath);   // 文件改名：删旧插新
                            ++deleteCount;
                        }
                    }
                    else if (isRenNew)
                    {
                        auto it = pendingRenames.constFind(frn);
                        if (it != pendingRenames.constEnd())
                        {
                            if (isDir)
                            {
                                // 目录改名：旧前缀 → 新前缀（整棵子树迁移）
                                renames.append(qMakePair(it.value(), fullPath));
                                ++renameCount;
                            }
                            else
                            {
                                deletes.append(it.value());   // 文件改名：删旧插新
                                ++deleteCount;
                                FileInfo f;
                                f.name = name;
                                f.path = fullPath;
                                f.isFolder = false;
                                f.modifiedTime = QDateTime::fromMSecsSinceEpoch(
                                    filetimeToMsec(rec->TimeStamp.QuadPart));
                                const int dot = name.lastIndexOf(QLatin1Char('.'));
                                f.suffix = (dot > 0) ? name.mid(dot + 1).toLower() : QString();
                                // 大小：README 无 size 字段，现查
                                WIN32_FIND_DATAW fd{};
                                HANDLE hf = FindFirstFileW(
                                    reinterpret_cast<const wchar_t*>(fullPath.utf16()), &fd);
                                if (hf == INVALID_HANDLE_VALUE)
                                    f.size = 0;
                                else
                                {
                                    f.size = combineFileSize(fd.nFileSizeLow, fd.nFileSizeHigh);
                                    FindClose(hf);
                                }
                                upserts.append(f);
                            }
                            pendingRenames.erase(it);
                        }
                        else
                        {
                            // 无 OLD 配对（journal 覆盖）：只插新
                            FileInfo f;
                            f.name = name;
                            f.path = fullPath;
                            f.isFolder = isDir;
                            f.modifiedTime = QDateTime::fromMSecsSinceEpoch(
                                filetimeToMsec(rec->TimeStamp.QuadPart));
                            const int dot = name.lastIndexOf(QLatin1Char('.'));
                            f.suffix = (dot > 0) ? name.mid(dot + 1).toLower() : QString();
                            f.size = 0;
                            upserts.append(f);
                            ++upsertCount;
                        }
                    }
                    else
                    {
                        // 新增/修改（CREATE/DATA_*/CLOSE 等）：upsert，现查大小
                        FileInfo f;
                        f.name = name;
                        f.path = fullPath;
                        f.isFolder = isDir;
                        f.modifiedTime = QDateTime::fromMSecsSinceEpoch(
                            filetimeToMsec(rec->TimeStamp.QuadPart));
                        const int dot = name.lastIndexOf(QLatin1Char('.'));
                        f.suffix = (dot > 0) ? name.mid(dot + 1).toLower() : QString();
                        WIN32_FIND_DATAW fd{};
                        HANDLE hf = FindFirstFileW(
                            reinterpret_cast<const wchar_t*>(fullPath.utf16()), &fd);
                        if (hf == INVALID_HANDLE_VALUE)
                        {
                            deletes.append(fullPath);   // 处理时已不存在 → 删
                            ++deleteCount;
                            p += rec->RecordLength;
                            ++recCount;
                            continue;
                        }
                        f.size = combineFileSize(fd.nFileSizeLow, fd.nFileSizeHigh);
                        FindClose(hf);
                        upserts.append(f);
                        ++upsertCount;
                    }
                    ++recCount;
                }
            }
            p += rec->RecordLength;
        }
        rj.StartUsn = nextUsn;
        m_lastUsn = nextUsn;
        if (upserts.size() >= 5000 || deletes.size() >= 5000)
            flushAll();
    }

    flushAll();
    qWarning() << "[USN] 增量完成: " << driveLetter << " 记录=" << recCount
               << " upsert=" << upsertCount << " delete=" << deleteCount
               << " rename=" << renameCount << " 新Usn=" << m_lastUsn;
    emit sendLastUsn(QString(driveLetter), m_lastUsn);
    CloseHandle(hVol);
    emit finished();
}

void DriveScanner::scannerFile()
{
    // /测试速度/ 开始计时
    QElapsedTimer timer;
    timer.start();

    std::set<std::wstring> visited;
    QList<FileInfo> list;
    const int& maxSize = 5000;
    // 必须 reserve（resize 撑大 size 会每文件 emit 整批空数据 → 内存爆炸）
    list.reserve(maxSize);

    while (!dirs.empty()) {
        
        std::wstring current = dirs.back();
        dirs.pop_back();

        if (visited.find(current) != visited.end()) {
            continue; 
        }
        visited.insert(current);

        std::wstring path = current + L"\\*.*";
        WIN32_FIND_DATAW data{};
        HANDLE hfind = FindFirstFileW(path.c_str(), &data);

        if (hfind == INVALID_HANDLE_VALUE) {
            qWarning() << "无法打开目录:" << QString::fromStdWString(current);
            continue;
        }

        do 
        {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }

            // 过滤系统目录（与 USN 路径一致）：$ 开头、回收站、系统卷信息
            {
                const QString nm = QString::fromWCharArray(data.cFileName);
                if (nm == QLatin1String("System Volume Information") ||
                    nm == QLatin1String("$RECYCLE.BIN") ||
                    nm.startsWith(QLatin1Char('$')))
                    continue;
            }

            // 过滤隐藏/系统文件（与 USN 路径、QDirIterator 默认行为一致）
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                continue;

            // 拼接路径：current 已带尾反斜杠（盘根 Z:\）时不再重复加
            std::wstring fullpath = current;
            if (fullpath.empty() || fullpath.back() != L'\\')
                fullpath += L'\\';
            fullpath += data.cFileName;   // 必须拼上文件名，否则 path 全是目录路径（UNIQUE 冲突互相覆盖 + 子目录被 visited 跳过）
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                QString name = QString::fromStdWString(data.cFileName);
                FileInfo file;

                file.name = name;
                file.size = combineFileSize(data.nFileSizeLow, data.nFileSizeHigh);
                file.suffix = "";
                file.path = QString::fromStdWString(fullpath);
                file.isFolder = 1;
                file.modifiedTime = FILETIME_to_QDateTime(data.ftLastWriteTime);

                list.append(file); 

                dirs.push_back(fullpath); 
            }
            else {
                QString name = QString::fromStdWString(data.cFileName);
                int idx = name.lastIndexOf(".");

                FileInfo file;
                file.name = name;
                file.size = combineFileSize(data.nFileSizeLow, data.nFileSizeHigh);
                file.suffix = idx == -1 ? "" : name.right(name.length() - idx - 1);
                file.path = QString::fromStdWString(fullpath);
                file.isFolder = 0;
                file.modifiedTime = FILETIME_to_QDateTime(data.ftLastWriteTime);

                list.append(file);
            }
            if (list.size() >= maxSize) {
                // 背压：积压超过阈值时暂停（同 USN 路径；30 批 ≈ 225MB）
                if (m_pendingBatches)
                {
                    while (m_pendingBatches->loadAcquire() >= 30)
                        QThread::msleep(2);
                    m_pendingBatches->fetchAndAddRelaxed(1);
                }
                emit sendFileinfo(list);
                list.clear();
                list.reserve(maxSize);   // 同上：reserve 不改变 size
            }

        } while (FindNextFileW(hfind, &data));
        FindClose(hfind);
        if (!list.isEmpty()) {
            // 背压：同上
            if (m_pendingBatches)
            {
                while (m_pendingBatches->loadAcquire() >= 30)
                    QThread::msleep(2);
                m_pendingBatches->fetchAndAddRelaxed(1);
            }
            emit sendFileinfo(list);
            list.clear();
        }
    }
}
