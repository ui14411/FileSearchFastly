#pragma once
#include <QObject>
#include <QMutex>
#include <QFileInfo>
#include <QList>
#include <QStringList>
#include <QHash>
#include <QVector>
#include <string>
#include <vector>
#include <QDateTime>
#include <windows.h>

#include "publicHeader/FileInfo.h"

class DriveScanner :public QObject {
	Q_OBJECT
public:
	DriveScanner(QObject* parent = nullptr, QFileInfo drive = QFileInfo(""), QAtomicInt* pendingBatches = nullptr, quint64 lastUsn = 0)
		:QObject(parent),m_drive(drive),m_pendingBatches(pendingBatches),m_lastUsn(lastUsn)
	{
	}

public:
	void startScanner();
	// 增量扫描：从上次 Usn 游标读 USN Journal（README），处理新增/删除/改名
	void incrementalUsn();

private:
	// 目录遍历（降级路径：非管理员 / 非 NTFS 时使用）
	void scannerFile();
	// NTFS USN 全量枚举（方案B：管理员权限时秒级建索引，Everything 同思路）
	bool scannerUsn();
private:
	static qint64 combineFileSize(DWORD sizeLow, DWORD sizeHigh)
	{
		return (static_cast<qint64>(sizeHigh) << 32) | sizeLow;
	}

	QDateTime FILETIME_to_QDateTime(const FILETIME& ft)
	{
		ULARGE_INTEGER uli;
		uli.LowPart = ft.dwLowDateTime;
		uli.HighPart = ft.dwHighDateTime;

		// FILETIME(1601 起算 100ns) 转 unix 毫秒需减 11644473600000
		const qint64 ms = uli.QuadPart / 10000 - 11644473600000LL;

		return QDateTime::fromMSecsSinceEpoch(ms);
	}

signals:
	void finished();
	void sendFileinfo(const QList<FileInfo>& file);
	// 增量：删除路径 / 目录改名前缀迁移 / 更新 USN 基线
	void sendFileDelete(const QStringList& paths);
	void sendRenamePrefix(const QString& oldPrefix, const QString& newPrefix);
	void sendLastUsn(const QString& drive, quint64 usn);

private:
	QFileInfo m_drive;
	std::vector<std::wstring> dirs;

	// USN 枚举互斥（多线程并行会失败，串行化后稳定）
	static QMutex s_usnMutex;

	// 背压：dbThread 积压计数（FileScanner 共享），防止信号队列无限堆积
	QAtomicInt* m_pendingBatches = nullptr;

	// 增量基线：上次扫描的 USN 游标（0 = 尚无基线，本次只建立基线）
	quint64 m_lastUsn = 0;
};
