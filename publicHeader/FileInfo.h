#pragma once

#include <QString>
#include <QDateTime>

struct FileInfo
{
    QString name;
    QString path;
    QString suffix;
    qint64 size;
    QDateTime modifiedTime;
    bool isFolder;
};

enum class FileType {
	File = 0,
	Folder = 1
};

enum class FileSearch {
	Suffix = 0,
	File = 1,
	Folder = 2
};

enum class SortType
{
    NameAsc = 0,
    NameDesc = 1,

    SizeAsc = 2,
    SizeDesc = 3,

    TimeAsc = 4,
    TimeDesc = 5
};