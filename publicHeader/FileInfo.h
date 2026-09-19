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
    int id;
};

struct PagedResult {
    QList<FileInfo> rows;
    bool            hasMore = false;
    QVariant        lastKey;
    qint64          lastId = 0;
};