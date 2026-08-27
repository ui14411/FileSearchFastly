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

// 诊断（2026-08）：所有 Qt 日志落盘 exe 同级 scan.log（控制台随进程消失，日志会丢）
static QFile* g_logFile = nullptr;
static QMutex g_logMutex;

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

    // 日志文件放 exe 同级目录（发布后跟随程序，不依赖 F 盘固定路径）
    g_logFile = new QFile(QCoreApplication::applicationDirPath() + "/scan.log");
    g_logFile->open(QIODevice::Append | QIODevice::Text);
    qInstallMessageHandler(logToFile);
    qWarning() << "[MAIN] 进程启动";

    FileInteract fileInteract;

    fileInteract.init();

    QQmlApplicationEngine engine;    
    
    engine.rootContext()->setContextProperty(
        "fileInteract",
        &fileInteract
    );

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/FileSearchFastly/main.qml")));
    if (engine.rootObjects().isEmpty())
        return -1;

    qWarning() << "[MAIN] 事件循环开始";
    const int rc = app.exec();
    qWarning() << "[MAIN] 事件循环退出 rc=" << rc;
    qInstallMessageHandler(nullptr);
    return rc;
}
