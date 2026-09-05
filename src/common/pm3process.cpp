#include "pm3process.h"
#include "cmdadapter.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTime>

// simple append-only debug log next to the executable, used to diagnose
// the client startup(env, exit codes) without a debugger attached
static void pm3ProcessLog(const QString& line)
{
    QFile log(QCoreApplication::applicationDirPath() + "/pm3gui_debug.log");
    if(log.open(QFile::Append | QFile::Text))
    {
        log.write(QString("[%1] %2\n").arg(QTime::currentTime().toString("hh:mm:ss.zzz"), line).toUtf8());
        log.close();
    }
}

PM3Process::PM3Process(QThread* thread, QObject* parent): QProcess(parent), serialPort(new QSerialPort(this))
{
    moveToThread(thread);
    setProcessChannelMode(PM3Process::MergedChannels);
    isRequiringOutput = false;
    requiredOutput = new QString();
    serialListener = new QTimer(); // if using new QTimer(this), the debug output will show "Cannot create children for a parent that is in a different thread."
    serialListener->moveToThread(this->thread());// I've tried many ways to creat a QTimer instance, but all of the instances are in the main thread(UI thread), so I have to move it manually
    serialListener->setInterval(1000);
    serialListener->setTimerType(Qt::VeryCoarseTimer);
    connect(serialListener, &QTimer::timeout, this, &PM3Process::onTimeout);
    connect(this, &PM3Process::readyRead, this, &PM3Process::onReadyRead);

    qRegisterMetaType<QProcess::ProcessError>("QProcess::ProcessError");
}

void PM3Process::connectPM3(const QString& path, const QStringList args)
{
    QString result;
    Util::ClientType clientType;
    setRequiringOutput(true);
	QRegularExpression osPattern("(os:\\s+|OS\\.+\\s+)");

    // stash for reconnect
    currPath = path;
    currArgs = args;

    // make the bundled Windows client layout("<exedir>/libs") work without a
    // user-configured env script: put the client's own DLL dirs(libgd,
    // libjansson, Qt plugins, ...) first on PATH. When an env script was
    // configured, setProcEnv() already ran(queued before this slot) and its
    // environment is kept as the base.
    QProcessEnvironment startEnv = processEnvironment();
    if(startEnv.isEmpty())
        startEnv = QProcessEnvironment::systemEnvironment();
    const QString clientDir = QFileInfo(path).absolutePath();
    CmdAdapter::augmentEnv(&startEnv, clientDir);
    CmdAdapter::ensureQtConf(clientDir);
    setProcessEnvironment(startEnv);
    pm3ProcessLog(QString("connect start: %1 %2, env entries: %3, HOME='%4', QT_QPA='%5'")
                  .arg(path, args.join(' '))
                  .arg(startEnv.toStringList().size())
                  .arg(startEnv.value("HOME"))
                  .arg(startEnv.value("QT_QPA_PLATFORM_PLUGIN_PATH")));

    // using "-f" option to make the client output flushed after every print.
    // single '\r' might appear. Don't use QProcess::Text there or '\r' is ignored.
    start(path, args, QProcess::Unbuffered | QProcess::ReadWrite);
    if(waitForStarted(10000))
    {
        waitForReadyRead(10000);
        setRequiringOutput(false);
        result = *requiredOutput;
        if(result.contains("[=]"))
        {
            clientType = Util::CLIENTTYPE_ICEMAN;
            setRequiringOutput(true);
            write("hw version\n");
            for(int i = 0; i < 50; i++)
            {
                waitForReadyRead(200);
                result += *requiredOutput;
                // if(result.contains("os: "))
                if(osPattern.match(result).hasMatch())
                    break;
            }
            setRequiringOutput(false);
        }
        else
        {
            clientType = Util::CLIENTTYPE_OFFICIAL;
        }
        // if(result.contains("os: ")) // make sure the PM3 is connected
		if(osPattern.match(result).hasMatch())
        {
            emit changeClientType(clientType);
            // result = result.mid(result.indexOf("os: "));
			QRegularExpressionMatch osMatch = osPattern.match(result);
			result = result.mid(osMatch.capturedStart());
            result = result.left(result.indexOf("\n"));
            // result = result.mid(4, result.indexOf(" ", 4) - 4);
			result = result.mid(osMatch.capturedLength(), result.indexOf(" ", osMatch.capturedLength()) - osMatch.capturedLength());
            emit PM3StatedChanged(true, result);
        }
        else
        {
            qDebug() << "unexpected output:" << (result.isEmpty() ? "(empty)" : result);
            emit HWConnectFailed();
            kill();
        }
    }

    setRequiringOutput(false);
}

void PM3Process::reconnectPM3()
{
    connectPM3(currPath, currArgs);
}

void PM3Process::setRequiringOutput(bool st)
{
    isRequiringOutput = st;
    if(isRequiringOutput)
        requiredOutput->clear();
}

bool PM3Process::waitForReadyRead(int msecs)
{
    return QProcess::waitForReadyRead(msecs);
}

void PM3Process::setSerialListener(const QString& name, bool state)
{
    if(state)
    {
        currPort = name;
        serialPort->setPortName(name);
        serialListener->start();
        qDebug() << serialListener->thread();
    }
    else
    {
        serialListener->stop();
        if(serialPort->isOpen())
            serialPort->close();
    }
}

void PM3Process::setSerialListener(bool state)
{
    setSerialListener(currPort, state);
}

void PM3Process::onTimeout()
{
    //when the proxmark3 client is unexpectedly terminated or the PM3 hardware
    //is removed, the port can be opened again; probe it like the old Qt5
    //QSerialPortInfo::isBusy() check(the client is supposed to use the target
    //serial port exclusively).
    //The probe will always succeed on Raspbian, in this case, check "Keep the
    //client active" in the Settings panel.
    if(serialPort->open(QIODevice::ReadWrite))
    {
        serialPort->close();
        killPM3();
    }
}

void PM3Process::testThread()
{
    qDebug() << "PM3:" << QThread::currentThread();
}

qint64 PM3Process::write(QString data)
{
    return QProcess::write(data.toLatin1());
}

void PM3Process::onReadyRead()
{
    QString out = readAll();
    if(isRequiringOutput)
        requiredOutput->append(out);
    if(out != "")
    {
//        qDebug() << "PM3Process::onReadyRead:" << out;
        emit newOutput(out);

    }
}

void PM3Process::setProcEnv(const QStringList* env)
{
//    qDebug() << "passed Env List" << *env;
    QProcessEnvironment procEnv;
    for(const QString& entry : *env)
    {
        int eq = entry.indexOf('=');
        if(eq > 0)
            procEnv.insert(entry.left(eq), entry.mid(eq + 1));
    }
    this->setProcessEnvironment(procEnv);
    //    qDebug() << "final Env List" << processEnvironment().toStringList();
}

void PM3Process::setWorkingDir(const QString& dir)
{
    // the working directory cannot be the default, or the client will failed to load the dll
    this->setWorkingDirectory(dir);
}

void PM3Process::killPM3()
{
    kill();
    emit PM3StatedChanged(false);
    setSerialListener(false);
}
