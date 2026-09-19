#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QDateTime>
#include <QProcess>
#include <QAtomicInt>

#include "publicHeader/FileInfo.h"

class FileDatabase;
class FileScanner;

class FileInteract : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString scanStatusText READ scanStatusText NOTIFY scanStatusChanged)

public:
    explicit FileInteract(QObject* parent = nullptr);
    ~FileInteract();

    Q_INVOKABLE bool init();
    Q_INVOKABLE void searchBySuffix(const QString& keyword, int sortType, const QString& drivePrefix);
    Q_INVOKABLE void showInExplorer(const QString& path);   // 在资源管理器中定位文件
    Q_INVOKABLE void searchByFolder(const QString& keyword, int sortType, const QString& drivePrefix);
    Q_INVOKABLE void searchByFolderContent(const QString& folderPath);   // 列目录（直接子项）
    Q_INVOKABLE void searchAll(const QString& keyword, int sortType, const QString& drivePrefix);   // 全量搜索：文件+文件夹，前端过滤模式
    Q_INVOKABLE void loadNextPage();
    Q_INVOKABLE QStringList getDrives() const;   // 当前所有盘符（UI 动态生成"全部/C盘/D盘"过滤按钮）
public:
    QString scanStatusText() const { return m_scanStatusText; }   // Q_PROPERTY READ

signals:
    void searchResultByFolderContent(const QVariantList& results);
    void searchResultAll(const QVariantList& results,const bool hasMore,bool append);
    void searchFinished(int count, const QString& error = QString());
    void scanStatusChanged(const QString& text);   // 扫描状态栏文本
	void pagedResultReady(int seq, const QVariantList& rows, bool hasMore, const QVariant& lastKey, qint64 lastId,bool append);

private:
    QVariantList convertToQVariantList(const QList<FileInfo>& files);

private:
    QString m_scanStatusText;   // 当前扫描状态文本（Q_PROPERTY 备份）

private slots:
    void onScanDriveStarted(const QString& letter, bool usnSupported);
    void onScanAllFinished();
    void onPagedResult_all(int seq, const QVariantList& rows,bool hasMore, const QVariant& lastKey, qint64 lastId,bool append);

private:
    FileDatabase* m_database;
    FileScanner* m_scanner;
    QAtomicInt m_seq = { 0 };

    
    QVariant m_curKey;
    qint64 m_curId = 0;
    int m_sortType;
    QString m_drivePrefix;
    QString m_keyWord;
    bool m_hasMore = false;
    bool m_loading = false;
};