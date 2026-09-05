#ifndef CMDADAPTER_H
#define CMDADAPTER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

// Keeps the GUI in sync with the Proxmark3 client it is pointed at.
//
// At GUI startup it runs the client binary once in offline script mode
// ("<client> -f -s <script>"), where the script asks "--help"/"-h" for every
// command referenced by the JSON config. Each help block is parsed into the
// list of option spellings the client actually accepts.
//
// The collected option tables are then used by adaptMap() to verify the
// "cmd"-like templates of a config section and to rewrite option spellings
// that the current client no longer understands into an equivalent form
// (e.g. "--blk" -> "-b"), so config files shipped with older GUI releases
// keep working with newer clients. Anything that cannot be resolved safely
// is reported instead of guessed.
class CmdAdapter : public QObject
{
    Q_OBJECT
public:
    explicit CmdAdapter(QObject* parent = nullptr);

    // Launches one offline client process. clientEnv may be empty(inherit the
    // system environment); the "<exedir>/libs" layout of the Windows client
    // bundles is detected and added to PATH automatically.
    // Returns false when the probe cannot even be started(no script file).
    bool startProbe(const QString& clientPath, const QStringList& clientEnv, const QString& workingDir, const QVariantMap& configRoot);

    bool isProbeDone() const;
    bool isProbeOk() const;
    QString clientVersion() const;
    QString probedClientPath() const;

    // Rewrites the templates of one config section("mifare classic"/"lf"/"t55xx")
    // in place against the probed option tables. Returns a short report,
    // empty when no usable probe result exists yet.
    QString adaptMap(QVariantMap& section, const QString& sectionName);

signals:
    void probeFinished(bool ok, const QString& summary);

private slots:
    void onProbeReadyRead();
    void onProbeFinished(int exitCode, QProcess::ExitStatus status);
    void onProbeError(QProcess::ProcessError error);
    void onProbeTimeout();

private:
    struct Stats
    {
        int cmdsOk = 0;      // commands whose template matched the client
        int cmdsUnknown = 0; // commands the probe knows nothing about(left untouched)
        int optsFixed = 0;   // option spellings rewritten
        int optsMissing = 0; // options the client does not offer any more
        int flagsChecked = 0;// flag map entries verified(e.g. "key type" A/B)
    };

    QProcess* probe = nullptr;
    QTimer* watchdog;
    QString probeOutput;
    QString scriptPath;
    QString probedPath;
    bool probeDone = false;
    bool probeOk = false;
    QString version;
    // full command path(e.g. "hf mf rdbl") -> option spellings accepted by the client
    QHash<QString, QStringList> cmdFlags;

    void finishProbe(bool ok, const QString& summary);
    void parseProbeOutput();
    bool parseHelpBlock(const QString& block, QStringList* flagsOut);
    void adaptEntryMap(QVariantMap& entry, Stats* stats);
    void adaptTemplate(QString* cmd, Stats* stats);
    void adaptFlagMap(QVariantMap& flagMap, const QString& prefix, const QString& cmdPath, Stats* stats);
    QStringList flagsFor(const QString& cmdPath) const;
    bool writeProbeScript(const QStringList& commands);
};

#endif // CMDADAPTER_H
