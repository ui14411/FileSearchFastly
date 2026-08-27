#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QDateTime>
#include <QProcess>

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
    Q_INVOKABLE void searchBySuffix(const QString& suffix);
    Q_INVOKABLE void showInExplorer(const QString& path);   // 在资源管理器中定位文件
    QString scanStatusText() const { return m_scanStatusText; }   // Q_PROPERTY READ
    Q_INVOKABLE void searchByFile(const QString& keyword);
    Q_INVOKABLE void searchByFolder(const QString& keyword);
    Q_INVOKABLE void searchByFolderContent(const QString& folderPath);   // 列目录（直接子项）
    Q_INVOKABLE void searchAll(const QString& keyword);   // 全量搜索：文件+文件夹，前端过滤模式
    Q_INVOKABLE QStringList getDrives() const;   // 当前所有盘符（UI 动态生成"全部/C盘/D盘"过滤按钮）
    // 异步排序（QtConcurrent 线程池，不卡 UI）；seq 由 QML 自增，用于丢弃过期结果
    Q_INVOKABLE void requestSort(const QVariantList& results, int sortType, int seq);
    Q_INVOKABLE static QString formatSize(qint64 bytes);
    Q_INVOKABLE static QString formatDateTime(const QDateTime& dateTime);

signals:
    void searchResultBySuffix(const QVariantList& results);
    void searchResultByFile(const QVariantList& results);
    void searchResultByFolder(const QVariantList& results);
    void searchResultByFolderContent(const QVariantList& results);
    void searchResultAll(const QVariantList& results);
    void sortResultReady(const QVariantList& sorted, int seq);
    void searchFinished(int count, const QString& error = QString());
    void scanStatusChanged(const QString& text);   // 扫描状态栏文本
    // 跨线程搜索请求：database 在专用线程时走队列连接
    void requestSearchBySuffix(const QString& keyword);
    void requestSearchByFile(const QString& keyword);
    void requestSearchByFolder(const QString& keyword);
    void requestSearchByFolderContent(const QString& folderPath);
    void requestSearchAll(const QString& keyword);

private:
    QVariantList convertToQVariantList(const QList<FileInfo>& files);

private:
    QString m_scanStatusText;   // 当前扫描状态文本（Q_PROPERTY 备份）

private slots:
    void onScanDriveStarted(const QString& letter, bool usnSupported);
    void onScanAllFinished();
    void onDatabaseResult_suffix(const QList<FileInfo>& files);
    void onDatabaseResult_filename(const QList<FileInfo>& files);
    void onDatabaseResult_folder(const QList<FileInfo>& files);
    void onDatabaseResult_folderContent(const QList<FileInfo>& files);
    void onDatabaseResult_all(const QList<FileInfo>& files);

private:
    FileDatabase* m_database;
    FileScanner* m_scanner;
};