#include "cmdadapter.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

// keys whose string value is a client command template
static const QStringList CMD_KEYS =
{
    "cmd", "static cmd", "basic cmd", "show cmd", "path cmd", "divisor cmd", "clone cmd", "read"
};

// keys whose map values fill a flag placeholder in the template,
// e.g. "key type": {"A": "a", "B": "b"} fills "-<key type>"
static const QStringList FLAGMAP_KEYS =
{
    "card type", "key type", "known key type", "target key type", "t5555 flag", "t55x7 flag"
};

// Known short/long spellings of the same client option. Used to rewrite a
// template option that the probed client no longer accepts into a form it
// does accept. Long names are unambiguous per command, so only long-flag
// mismatches are rewritten automatically; bare short flags are only reported.
static QStringList equivalenceCandidates(const QString& flag)
{
    typedef QHash<QString, QStringList> EquivTable;
    static const EquivTable table =
    {
        {"blk",     {"--blk", "-b"}},
        {"key",     {"--key", "-k"}},
        {"data",    {"--data", "-d"}},
        {"sec",     {"--sec", "-s"}},
        {"uid",     {"--uid", "-u"}},
        {"atqa",    {"--atqa", "-a"}},
        {"sak",     {"--sak", "-s"}},
        {"file",    {"--file", "-f"}},
        {"keys",    {"--keys", "-k"}},
        {"buffer",  {"--buffer", "-1"}},
        {"divisor", {"--divisor", "-d", "--div"}},
        {"div",     {"--div", "-d", "--divisor"}},
        {"force",   {"--force"}},
        {"dump",    {"--dump"}},
        {"mini",    {"--mini"}},
        {"1k",      {"--1k"}},
        {"2k",      {"--2k"}},
        {"4k",      {"--4k"}},
        {"tblk",    {"--tblk"}},
        {"ta",      {"--ta"}},
        {"tb",      {"--tb"}},
        {"q5",      {"--q5"}},
        {"em",      {"--em"}},
        {"verbose", {"--verbose", "-v"}},
        {"help",    {"--help", "-h"}},
    };
    if(!flag.startsWith("--"))
        return QStringList();
    return table.value(flag.mid(2), QStringList());
}

CmdAdapter::CmdAdapter(QObject* parent) : QObject(parent)
{
    watchdog = new QTimer(this);
    watchdog->setSingleShot(true);
    watchdog->setInterval(90000);
    connect(watchdog, &QTimer::timeout, this, &CmdAdapter::onProbeTimeout);
}

bool CmdAdapter::startProbe(const QString& clientPath, const QStringList& clientEnv, const QString& workingDir, const QVariantMap& configRoot)
{
    if(probe != nullptr) // a previous probe is still running
    {
        probe->disconnect();
        probe->kill();
        probe->deleteLater();
        probe = nullptr;
    }
    if(!scriptPath.isEmpty())
    {
        QFile::remove(scriptPath);
        scriptPath.clear();
    }
    probeDone = false;
    probeOk = false;
    probeOutput.clear();
    version.clear();
    cmdFlags.clear();
    probedPath.clear();

    QStringList commands;
    collectCommands(configRoot, &commands);
    commands.removeAll(QString(""));
    if(commands.isEmpty() || !writeProbeScript(commands))
        return false;
    probedPath = clientPath;

    probe = new QProcess(this);
    probe->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    // don't flash a console window when probing at GUI startup
    probe->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args)
    {
        *static_cast<unsigned long*>(args->flags) |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    QStringList env = clientEnv;
    if(env.isEmpty())
        env = QProcessEnvironment::systemEnvironment().toStringList();
    augmentEnv(&env, QFileInfo(clientPath).absolutePath());
    probe->setEnvironment(env);
    if(!workingDir.isEmpty() && QDir(workingDir).exists())
        probe->setWorkingDirectory(workingDir);
    connect(probe, &QProcess::readyRead, this, &CmdAdapter::onProbeReadyRead);
    connect(probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &CmdAdapter::onProbeFinished);
    connect(probe, &QProcess::errorOccurred, this, &CmdAdapter::onProbeError);
    probe->start(clientPath, {"-f", "-s", scriptPath});
    watchdog->start();
    return true;
}

bool CmdAdapter::isProbeDone() const
{
    return probeDone;
}

bool CmdAdapter::isProbeOk() const
{
    return probeOk;
}

QString CmdAdapter::clientVersion() const
{
    return version;
}

QString CmdAdapter::probedClientPath() const
{
    return probedPath;
}

void CmdAdapter::onProbeReadyRead()
{
    if(probe == nullptr)
        return;
    probeOutput.append(QString::fromLatin1(probe->readAll()));
}

void CmdAdapter::onProbeFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(exitCode)
    Q_UNUSED(status)
    if(probe == nullptr || probeDone)
        return;
    // drain whatever is left in the pipe
    probeOutput.append(QString::fromLatin1(probe->readAll()));
    parseProbeOutput();
    finishProbe(cmdFlags.size() >= 3,
                QString("client commands: %1 detected, version '%2'")
                .arg(cmdFlags.size())
                .arg(version.isEmpty() ? QString("unknown") : version));
}

void CmdAdapter::onProbeError(QProcess::ProcessError error)
{
    if(probe == nullptr || probeDone)
        return;
    if(error == QProcess::FailedToStart)
        finishProbe(false, "client could not be started for the command check");
}

void CmdAdapter::onProbeTimeout()
{
    if(probe == nullptr)
        return;
    probe->kill(); // onProbeFinished() will collect the partial output
}

void CmdAdapter::finishProbe(bool ok, const QString& summary)
{
    if(probeDone)
        return;
    watchdog->stop();
    probeDone = true;
    probeOk = ok;
    if(!scriptPath.isEmpty())
    {
        QFile::remove(scriptPath);
        scriptPath.clear();
    }
    qDebug() << "CmdAdapter:" << summary;
    emit probeFinished(probeOk, summary);
}

// ---- output parsing ----

void CmdAdapter::parseProbeOutput()
{
    // The client echoes every script line as "[...] pm3 --> <command>".
    // Split the output into per-command blocks on that marker.
    const QString marker = "pm3 --> ";
    QRegularExpression versionRe("Iceman/\\S+");
    QRegularExpressionMatch versionMatch = versionRe.match(probeOutput);
    if(versionMatch.hasMatch())
        version = versionMatch.captured();

    QStringList currentPath;
    QString currentBlock;
    const QStringList lines = probeOutput.split('\n');
    for(const QString& rawLine : lines)
    {
        int markerPos = rawLine.indexOf(marker);
        if(markerPos >= 0)
        {
            // flush the previous block
            if(!currentPath.isEmpty())
            {
                QStringList flags;
                if(parseHelpBlock(currentBlock, &flags) && !flags.isEmpty())
                    cmdFlags.insert(currentPath.join(' '), flags); // first successful variant(--help) wins
            }
            QString echoed = rawLine.mid(markerPos + marker.length()).trimmed();
            currentBlock.clear();
            currentPath.clear();
            if(echoed.endsWith("--help"))
                currentPath = echoed.left(echoed.length() - 7).trimmed().split(' ');
            else if(echoed.endsWith("-h"))
                currentPath = echoed.left(echoed.length() - 2).trimmed().split(' ');
            if(currentPath.join(' ').isEmpty())
                currentPath.clear();
        }
        else if(!currentPath.isEmpty())
            currentBlock += rawLine + '\n';
    }
    if(!currentPath.isEmpty())
    {
        QStringList flags;
        if(parseHelpBlock(currentBlock, &flags) && !flags.isEmpty())
            cmdFlags.insert(currentPath.join(' '), flags);
    }
}

// Parses one "<cmd> --help" block of the current client help format:
//
//     usage:
//         hf mf rdbl [-habv] --blk <dec> [-c <dec>] [-k <hex>]
//
//     options:
//         -h, --help                     This help
//         --blk <dec>                    block number
//         -a                             input key type is key A (def)
//         -k, --key <hex>                key, 6 hex bytes
//
// Returns false when the block doesn't follow this format(e.g. the
// "not available in this mode" notice or a parent help listing).
bool CmdAdapter::parseHelpBlock(const QString& block, QStringList* flagsOut)
{
    const QStringList lines = block.split('\n');
    bool hasUsage = false;
    bool inOptions = false;
    for(const QString& rawLine : lines)
    {
        QString line = rawLine.trimmed();
        if(line.isEmpty())
            continue;
        if(!hasUsage)
        {
            if(line.startsWith("usage", Qt::CaseInsensitive))
                hasUsage = true;
            continue;
        }
        if(!inOptions)
        {
            if(line.startsWith("options", Qt::CaseInsensitive))
                inOptions = true;
            continue;
        }
        if(line.startsWith("examples", Qt::CaseInsensitive))
            break;
        // option lines: "-h, --help <...>  description" / "--blk <dec>  description" / "-a  description"
        QRegularExpressionMatch m = QRegularExpression("^(-[^\\s,]+)(?:,\\s*(--[^\\s]+))?\\s*(.*)$").match(line);
        if(!m.hasMatch())
            continue;
        const QString first = m.captured(1);
        const QString second = m.captured(2);
        if(!flagsOut->contains(first))
            flagsOut->append(first);
        if(first.startsWith("--"))
            continue; // long-only option, already added
        if(!second.isEmpty() && !flagsOut->contains(second))
            flagsOut->append(second);
    }
    return hasUsage && inOptions;
}

// ---- config adaptation ----

QString CmdAdapter::adaptMap(QVariantMap& section, const QString& sectionName)
{
    if(!isProbeOk() || section.isEmpty())
        return QString();
    Stats stats;
    adaptEntryMap(section, &stats);
    const QString report = QString("%1: %2 command(s) verified, %3 option(s) fixed, %4 missing, %5 unknown")
                           .arg(sectionName)
                           .arg(stats.cmdsOk)
                           .arg(stats.optsFixed)
                           .arg(stats.optsMissing)
                           .arg(stats.cmdsUnknown);
    qDebug() << "CmdAdapter::adaptMap" << report;
    return report;
}

// Adapt one command entry(e.g. configMap["nested"]) or a section root.
// Maps that contain no "cmd"-like key are just recursed into.
void CmdAdapter::adaptEntryMap(QVariantMap& entry, Stats* stats)
{
    // collect the templates of this entry
    QStringList templKeys;
    for(const QString& key : CMD_KEYS)
    {
        if(entry.contains(key) && entry.value(key).type() == QVariant::String)
            templKeys.append(key);
    }

    if(!templKeys.isEmpty())
    {
        // placeholder name -> flag prefix taken from the templates,
        // e.g. "--<card type>" -> {"card type": "--"}, "-<key type>" -> {"key type": "-"}
        QMap<QString, QString> placeholderPrefix;
        QRegularExpression placeholderRe("(--?[^ <>]*)<([^>]+)>");
        for(const QString& key : templKeys)
        {
            QRegularExpressionMatchIterator it = placeholderRe.globalMatch(entry.value(key).toString());
            while(it.hasNext())
            {
                QRegularExpressionMatch m = it.next();
                if(!placeholderPrefix.contains(m.captured(2)))
                    placeholderPrefix.insert(m.captured(2), m.captured(1));
            }
        }
        const QString cmdPath = commandPathOf(entry.value(templKeys.first()).toString());
        QStringList flags = flagsFor(cmdPath);
        if(flags.isEmpty())
        {
            stats->cmdsUnknown++;
            qWarning() << "CmdAdapter: client does not report options for" << cmdPath;
        }
        else
        {
            for(const QString& key : templKeys)
            {
                QString cmd = entry.value(key).toString();
                adaptTemplate(&cmd, stats);
                entry[key] = cmd;
            }
            stats->cmdsOk++;
            // verify/fix the flag-valued submaps of this entry
            for(const QString& key : FLAGMAP_KEYS)
            {
                if(!entry.contains(key))
                    continue;
                if(entry.value(key).type() == QVariant::Map)
                {
                    QVariantMap flagMap = entry.value(key).toMap();
                    adaptFlagMap(flagMap, placeholderPrefix.value(key, ""), cmdPath, stats);
                    entry[key] = flagMap;
                }
                else if(entry.value(key).type() == QVariant::String)
                {
                    // single flag value, e.g. "t5555 flag": "--q5" fills "<type>"
                    const QString placeholder = (key == "t5555 flag" || key == "t55x7 flag")
                                                ? QString("type") : key;
                    QVariantMap single = {{"", entry.value(key).toString()}};
                    adaptFlagMap(single, placeholderPrefix.value(placeholder, ""), cmdPath, stats);
                    entry[key] = single.value("").toString();
                }
            }
            // raw sequences(e.g. Magic Card gen1 lock commands) are verified as-is
            if(entry.contains("sequence") && entry.value("sequence").type() == QVariant::StringList)
            {
                const QStringList sequence = entry.value("sequence").toStringList();
                for(const QString& item : sequence)
                {
                    const QString flag = item.section(' ', 0, 0);
                    if(!flag.startsWith('-') || flag.startsWith("--"))
                        continue;
                    bool allKnown = true;
                    for(int i = 1; i < flag.length(); i++) // bundled short flags, e.g. "-ak"
                    {
                        if(!flags.contains(QString('-') + flag.at(i)))
                        {
                            allKnown = false;
                            break;
                        }
                    }
                    if(allKnown)
                        stats->flagsChecked++;
                    else
                        stats->optsMissing++;
                }
            }
        }
    }

    // recurse into nested maps, skipping the flag maps handled above
    for(auto it = entry.begin(); it != entry.end(); it++)
    {
        if(it.value().type() != QVariant::Map || FLAGMAP_KEYS.contains(it.key()))
            continue;
        QVariantMap child = it.value().toMap();
        adaptEntryMap(child, stats);
        entry[it.key()] = child;
    }
}

// Verify/fix the option tokens of one template against the probed flag list.
// The template is tokenized on spaces; the command path itself, values and
// "<placeholders>" are left alone.
void CmdAdapter::adaptTemplate(QString* cmd, Stats* stats)
{
    const QString cmdPath = commandPathOf(*cmd);
    const QStringList flags = flagsFor(cmdPath);
    if(flags.isEmpty())
        return; // counted as unknown by the caller
    QStringList tokens = cmd->split(' ');
    for(int i = 0; i < tokens.length(); i++)
    {
        const QString token = tokens.at(i); // copy: the element may be rewritten below
        if(token.isEmpty() || token.contains('<') || !token.startsWith('-'))
            continue; // empty slot, value placeholder or positional value
        if(flags.contains(token))
            continue; // accepted as-is
        if(token.startsWith("--"))
        {
            // long options are unambiguous, try the known equivalent spellings
            bool fixed = false;
            for(const QString& candidate : equivalenceCandidates(token))
            {
                if(flags.contains(candidate))
                {
                    tokens[i] = candidate;
                    stats->optsFixed++;
                    qDebug() << "CmdAdapter:" << cmdPath << ":" << token << "->" << candidate;
                    fixed = true;
                    break;
                }
            }
            if(!fixed)
            {
                stats->optsMissing++;
                qWarning() << "CmdAdapter: option" << token << "of" << cmdPath << "not reported by the client";
            }
            continue;
        }
        // short flags are ambiguous between commands, they are reported but never rewritten
        if(token.length() > 2)
        {
            // bundled short flags, e.g. "-nsv": fine as long as every component exists
            bool allKnown = true;
            for(int c = 1; c < token.length(); c++)
            {
                if(!flags.contains(QString('-') + token.at(c)))
                    allKnown = false;
            }
            if(allKnown)
                continue;
        }
        stats->optsMissing++;
        qWarning() << "CmdAdapter: option" << token << "of" << cmdPath << "not reported by the client";
    }
    *cmd = tokens.join(' ');
}

// Verify/fix a submap that fills a flag placeholder, e.g.
// "key type": {"A": "a", "B": "b"} inside "-<key type>" or
// "t5555 flag": "--q5" inside "<type>".
void CmdAdapter::adaptFlagMap(QVariantMap& flagMap, const QString& prefix, const QString& cmdPath, Stats* stats)
{
    const QStringList flags = flagsFor(cmdPath);
    if(flags.isEmpty())
        return;
    for(auto it = flagMap.begin(); it != flagMap.end(); it++)
    {
        const QString value = it.value().toString();
        const QString effective = prefix + value;
        if(effective.isEmpty())
            continue; // e.g. "t55x7 flag": ""(no flag)
        if(flags.contains(effective))
        {
            stats->flagsChecked++;
            continue;
        }
        bool fixed = false;
        if(effective.startsWith("--"))
        {
            for(const QString& candidate : equivalenceCandidates(effective))
            {
                if(flags.contains(candidate))
                {
                    flagMap[it.key()] = candidate.mid(prefix.length());
                    stats->optsFixed++;
                    stats->flagsChecked++;
                    qDebug() << "CmdAdapter:" << cmdPath << ":" << effective << "->" << candidate;
                    fixed = true;
                    break;
                }
            }
        }
        if(!fixed)
        {
            stats->optsMissing++;
            qWarning() << "CmdAdapter: option" << effective << "of" << cmdPath << "not reported by the client";
        }
    }
}

QStringList CmdAdapter::flagsFor(const QString& cmdPath) const
{
    if(cmdPath.isEmpty())
        return QStringList();
    return cmdFlags.value(cmdPath);
}

// "hf mf rdbl --blk <block> -k <key>" -> "hf mf rdbl"
// Leading tokens are the command path; it ends at the first option,
// placeholder or value token.
QString CmdAdapter::commandPathOf(const QString& templ)
{
    QStringList path;
#if (QT_VERSION <= QT_VERSION_CHECK(5, 14, 0))
    const QStringList tokens = templ.split(' ', QString::SkipEmptyParts);
#else
    const QStringList tokens = templ.split(' ', Qt::SkipEmptyParts);
#endif
    for(const QString& token : tokens)
    {
        if(token.startsWith('-') || token.contains('<'))
            break;
        path.append(token);
    }
    return path.join(' ');
}

// Collect every command path referenced by a config tree.
void CmdAdapter::collectCommands(const QVariantMap& map, QStringList* out)
{
    for(auto it = map.begin(); it != map.end(); it++)
    {
        if(it.value().type() == QVariant::String && CMD_KEYS.contains(it.key()))
        {
            const QString path = commandPathOf(it.value().toString());
            if(!path.isEmpty() && !out->contains(path))
                out->append(path);
        }
        else if(it.value().type() == QVariant::Map)
            collectCommands(it.value().toMap(), out);
    }
}

// Mirror what the bundled Windows client's setup.bat does("<exedir>/libs"
// layout): put the DLL dirs first on PATH and point Qt at its platform
// plugin, so the probe works even without a user-configured env script.
// The dirs must come first, otherwise DLLs from unrelated Qt installations
// on the system PATH shadow the bundled ones.
void CmdAdapter::augmentEnv(QStringList* env, const QString& exeDir)
{
    if(exeDir.isEmpty() || !QFileInfo::exists(exeDir + "/libs"))
        return;
#ifdef Q_OS_WIN
    const QChar sep = ';';
#else
    const QChar sep = ':';
#endif
    const QString libDir = exeDir + "/libs";
    auto prependVar = [env, sep](const char* name, const QStringList& values)
    {
        const QString prefix = QString(name) + '=';
        for(QString& entry : *env)
        {
            if(entry.startsWith(prefix, Qt::CaseInsensitive))
            {
                const QString current = entry.mid(prefix.length());
                QStringList merged = values;
                for(const QString& part : current.split(sep, Qt::SkipEmptyParts))
                {
                    if(!merged.contains(part, Qt::CaseInsensitive))
                        merged.append(part);
                }
                entry = prefix + merged.join(sep);
                return;
            }
        }
        env->append(prefix + values.join(sep));
    };
    prependVar("PATH", {exeDir, libDir, libDir + "/shell"});
    prependVar("QT_QPA_PLATFORM_PLUGIN_PATH", {libDir});
    prependVar("QT_PLUGIN_PATH", {libDir});
}

// One script file passed to "<client> -s": "--help" first, "-h" as the
// fallback for older clients that predate the "--help" convention.
bool CmdAdapter::writeProbeScript(const QStringList& commands)
{
    scriptPath = QDir::temp().absoluteFilePath(QString("pm3gui_cmdprobe_%1.cmd").arg(QCoreApplication::applicationPid()));
    QFile script(scriptPath);
    if(!script.open(QFile::WriteOnly | QFile::Text))
    {
        scriptPath.clear();
        return false;
    }
    script.write("hw version\n");
    for(const QString& command : commands)
    {
        script.write(QString("%1 --help\n").arg(command).toLatin1());
        script.write(QString("%1 -h\n").arg(command).toLatin1());
    }
    script.write("quit\n");
    script.close();
    return true;
}
