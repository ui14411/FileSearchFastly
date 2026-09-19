#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QList>
#include <QStringList>
#include <QMutex>
#include <QSqlQuery>
#include <QVariant> 

#include "publicHeader/FileInfo.h"

class FileDatabase :public QObject {
	Q_OBJECT
public:
	FileDatabase(QObject* parent = nullptr);

public:
	Q_INVOKABLE bool initDatabase();
	bool isInitialScanFinished();
	void setInitialScanFinished();

public slots:
	void insertFile(const QList<FileInfo>& info);
    // 同步搜索（独立只读连接——不排 dbThread 扫描队列，扫描中可并发读）
    PagedResult searchFileSuffixSync(const QString& keyword, int sortType, const QVariant& curKey, qint64 curId, const QString& drivePrefix);
    PagedResult searchFileFolderSync(const QString& keyword, int sortType, const QVariant& curKey, qint64 curId, const QString& drivePrefix);
    QList<FileInfo> searchFolderContentSync(const QString& folderPath);
    PagedResult searchAllSync(const QString& keyword, int sortType,const QVariant& curKey, qint64 curId,const QString& drivePrefix = QString());
    void setLastUsn(const QString& drive, quint64 usn);        // 增量基线（journal 游标）
    void deleteFiles(const QStringList& paths);                // 增量删除
    void renamePrefix(const QString& oldPrefix, const QString& newPrefix);  // 目录改名前缀迁移

public:
    quint64 getLastUsn(const QString& drive);   // 主线程读（moveToThread 之前安全）
    QString getVolumeId(const QString& drive);  // 卷序列号标识（盘符更换检测）
    void setVolumeId(const QString& drive, const QString& volId);
    void clearDrive(const QString& letter);     // 删该盘全部条目 + 基线 + 卷标识
    void setDirty(const QString& drive, bool dirty);
    void setLastTraverseTime(const QString& drive, qint64 secs);

private:
	QSqlDatabase db;
	QSqlQuery query;
    static constexpr int kPageSize = 500;
};
