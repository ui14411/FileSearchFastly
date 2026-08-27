#pragma once

#include <QObject>
#include <QDir>
#include <QCoreApplication>
#include <QList>
#include <QFileInfo>
#include <QThread>
#include <functional>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "publicHeader/FileInfo.h"

class DriveScanner;
class FileDatabase;

    // 移动盘变化监视（dirty 标记：运行期有变化则下次启动重建）
class DriveWatcher
{
public:
    ~DriveWatcher() { stop(); }

    void start(const QString& letter, std::function<void()> onChange)
    {
        m_stop = false;
        m_thread = std::thread([this, letter, onChange]() { loop(letter, onChange); });
    }

    void stop()
    {
        m_stop = true;
        if (m_h)
            CancelIoEx(m_h, nullptr);
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void loop(const QString& letter, std::function<void()> onChange)
    {
        const QString root = letter + QLatin1String(":\\");
        m_h = CreateFileW(
            reinterpret_cast<const wchar_t*>(root.utf16()),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (m_h == INVALID_HANDLE_VALUE)
        {
            m_h = nullptr;
            return;
        }
        QByteArray buf(64 * 1024, Qt::Uninitialized);
        for (;;)
        {
            DWORD got = 0;
            const BOOL ok = ReadDirectoryChangesW(
                m_h, buf.data(), static_cast<DWORD>(buf.size()), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &got, nullptr, nullptr);
            if (!ok || m_stop || got == 0)
                break;
            if (onChange)
                onChange();   // 注意：在 watcher 线程执行，回调内部需跨线程安全
        }
        CloseHandle(m_h);
        m_h = nullptr;
    }

    std::thread m_thread;
    HANDLE m_h = nullptr;
    volatile bool m_stop = false;
};

class FileScanner :public QObject
{
    Q_OBJECT
public:
    FileScanner(QObject* parent = nullptr,FileDatabase* database = nullptr);
    ~FileScanner();

public:
    void scannerFile();
    void sortFile(SortType key, QList<FileInfo>& filelist);

signals:
    void scanDriveStarted(const QString& letter, bool usnSupported);   // 每盘开始扫描
    void scanAllFinished();                                            // 全部盘扫描完成

private:
    QVector<QThread*> threads;
    FileDatabase* m_database = nullptr;
    QThread* m_dbThread = nullptr;
    QList<DriveWatcher*> m_watchers;   // 移动盘运行时监视（析构时停止）
};