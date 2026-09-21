#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QDebug>
#include <windows.h>
#include <QQmlContext>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>

#include "HeaderFile/FileScanner.h"
#include "HeaderFile/FileDatabase.h"
#include "HeaderFile/FileInteract.h"

static QFile* g_logFile = nullptr;
static QMutex g_logMutex;
static const qint64 kLogRotateBytes = 8 * 1024 * 1024;   // 8MB：超了把当前日志轮转成 scan.log.1

void logToFile(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    QMutexLocker locker(&g_logMutex);
    if (!g_logFile)
        return;
    QTextStream ts(g_logFile);
    ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz ") << msg << Qt::endl;
    g_logFile->flush();
    // 控制台必须用本地代码页（GBK）输出：UTF-8 字节会被 GBK 控制台解析成乱码
    fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
}

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN) && QT_VERSION_CHECK(5, 6, 0) <= QT_VERSION && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);

    // 日志文件放 exe 同级目录；超过阈值先轮转，只保留上一份（总量封顶 2×阈值）
    const QString logDir = QCoreApplication::applicationDirPath();
    if (QFile(logDir + "/scan.log").size() > kLogRotateBytes)
    {
        QFile::remove(logDir + "/scan.log.1");
        QFile::rename(logDir + "/scan.log", logDir + "/scan.log.1");
    }
    g_logFile = new QFile(logDir + "/scan.log");
    g_logFile->open(QIODevice::Append | QIODevice::Text);
    qInstallMessageHandler(logToFile);
    qWarning() << "[MAIN] 进程启动";

    FileInteract fileInteract;

    QQmlApplicationEngine engine;  

    engine.rootContext()->setContextProperty(
        "fileInteract",
        &fileInteract
    );

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/FileSearchFastly/main.qml")));
    if (engine.rootObjects().isEmpty())
        return -1;

    fileInteract.init();

    qWarning() << "[MAIN] 事件循环开始";
    const int rc = app.exec();
    qWarning() << "[MAIN] 事件循环退出 rc=" << rc;
    // 这里不能提前卸载日志处理器：fileInteract / engine 的析构发生在 return 处，
    // 提前卸载会让整条退出路径变成日志盲区，关窗卡住时无从定位
    return rc;
}
