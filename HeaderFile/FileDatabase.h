#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QList>
#include <QStringList>
#include <QMutex>
#include <QSqlQuery>

#include "publicHeader/FileInfo.h"

class QFileInfo;

class FileDatabase :public QObject {
	Q_OBJECT
public:
	FileDatabase(QObject* parent = nullptr);

public:
	Q_INVOKABLE bool initDatabase();
	void searchFile(const QString& keyword, FileSearch type);//统一的搜索接口
	bool isInitialScanFinished();
	void setInitialScanFinished();

public slots:
	void insertFile(const QList<FileInfo>& info);
    void searchFile_suffix(const QString& keyword);
    void searchFile_filename(const QString& keyword);
    void searchFile_folder(const QString& keyword);
    void searchFolderContent(const QString& folderPath);   // 列目录：直接子项，按修改时间倒序
    void searchAll(const QString& keyword);                // 名字 LIKE，文件+文件夹全部返回（前端过滤模式用）
    // 同步搜索（独立只读连接——不排 dbThread 扫描队列，扫描中可并发读）
    QList<FileInfo> searchFileSuffixSync(const QString& keyword);
    QList<FileInfo> searchFileFilenameSync(const QString& keyword);
    QList<FileInfo> searchFileFolderSync(const QString& keyword);
    QList<FileInfo> searchFolderContentSync(const QString& folderPath);
    QList<FileInfo> searchAllSync(const QString& keyword);
    void setLastUsn(const QString& drive, quint64 usn);        // 增量基线（journal 游标）
    void deleteFiles(const QStringList& paths);                // 增量删除
    void renamePrefix(const QString& oldPrefix, const QString& newPrefix);  // 目录改名前缀迁移

public:
    quint64 getLastUsn(const QString& drive);   // 主线程读（moveToThread 之前安全）
    QString getVolumeId(const QString& drive);  // 卷序列号标识（盘符更换检测）
    void setVolumeId(const QString& drive, const QString& volId);
    void clearDrive(const QString& letter);     // 删该盘全部条目 + 基线 + 卷标识
    bool getDirty(const QString& drive, bool* hasMark = nullptr);   // 移动盘变化标记（运行时监视）
    void setDirty(const QString& drive, bool dirty);
    qint64 getLastTraverseTime(const QString& drive);  // 上次目录遍历时间戳（秒）
    void setLastTraverseTime(const QString& drive, qint64 secs);

signals:
    void sendFile_suffix(const QList<FileInfo>& file);
    void sendFile_filename(const QList<FileInfo>& file);
    void sendFile_folder(const QList<FileInfo>& file);
    void sendFile_folderContent(const QList<FileInfo>& file);
    void sendFile_all(const QList<FileInfo>& file);

private:
	QSqlDatabase db;
	QSqlQuery query;
};
