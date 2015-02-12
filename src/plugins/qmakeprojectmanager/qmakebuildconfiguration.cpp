/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company.  For licensing terms and
** conditions see http://www.qt.io/terms-conditions.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, The Qt Company gives you certain additional
** rights.  These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "qmakebuildconfiguration.h"

#include "qmakebuildinfo.h"
#include "qmakekitinformation.h"
#include "qmakeproject.h"
#include "qmakeprojectconfigwidget.h"
#include "qmakeprojectmanagerconstants.h"
#include "qmakenodes.h"
#include "qmakestep.h"
#include "makestep.h"

#include <coreplugin/documentmanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/mimedatabase.h>

#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectmacroexpander.h>
#include <projectexplorer/target.h>
#include <projectexplorer/toolchain.h>
#include <projectexplorer/toolchainmanager.h>
#include <qtsupport/qtkitinformation.h>
#include <qtsupport/qtversionmanager.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>
#include <android/androidmanager.h>

#include <QDebug>
#include <QInputDialog>

#include <limits>

using namespace ProjectExplorer;
using namespace QtSupport;
using namespace Utils;
using namespace QmakeProjectManager::Internal;

namespace QmakeProjectManager {

// --------------------------------------------------------------------
// Helpers:
// --------------------------------------------------------------------

QString QmakeBuildConfiguration::shadowBuildDirectory(const QString &proFilePath, const Kit *k, const QString &suffix)
{
    if (proFilePath.isEmpty())
        return QString();
    QFileInfo info(proFilePath);

    BaseQtVersion *version = QtKitInformation::qtVersion(k);
    if (version && !version->supportsShadowBuilds())
        return info.absolutePath();

    const QString projectName = QFileInfo(proFilePath).completeBaseName();
    ProjectMacroExpander expander(projectName, k, suffix);
    QString projectDir = Project::projectDirectory(FileName::fromString(proFilePath)).toString();
    QString buildPath = expander.expand(Core::DocumentManager::buildDirectory());
    return FileUtils::resolvePath(projectDir, buildPath);
}

static Utils::FileName defaultBuildDirectory(bool supportsShadowBuild,
                                             const QString &projectPath,
                                             const ProjectExplorer::Kit *k,
                                             const QString &suffix)
{
    if (supportsShadowBuild)
        return Utils::FileName::fromString(QmakeBuildConfiguration::shadowBuildDirectory(projectPath, k, suffix));
    return ProjectExplorer::Project::projectDirectory(Utils::FileName::fromString(projectPath));
}

const char QMAKE_BC_ID[] = "Qt4ProjectManager.Qt4BuildConfiguration";
const char USE_SHADOW_BUILD_KEY[] = "Qt4ProjectManager.Qt4BuildConfiguration.UseShadowBuild";
const char BUILD_CONFIGURATION_KEY[] = "Qt4ProjectManager.Qt4BuildConfiguration.BuildConfiguration";

enum { debug = 0 };

QmakeBuildConfiguration::QmakeBuildConfiguration(Target *target) :
    BuildConfiguration(target, Core::Id(QMAKE_BC_ID)),
    m_shadowBuild(true),
    m_isEnabled(false),
    m_qmakeBuildConfiguration(0),
    m_subNodeBuild(0),
    m_fileNodeBuild(0)
{
    ctor();
}

QmakeBuildConfiguration::QmakeBuildConfiguration(Target *target, Core::Id id) :
    BuildConfiguration(target, id),
    m_shadowBuild(true),
    m_isEnabled(false),
    m_qmakeBuildConfiguration(0),
    m_subNodeBuild(0),
    m_fileNodeBuild(0)
{
    ctor();
}

QmakeBuildConfiguration::QmakeBuildConfiguration(Target *target, QmakeBuildConfiguration *source) :
    BuildConfiguration(target, source),
    m_shadowBuild(source->m_shadowBuild),
    m_isEnabled(false),
    m_qmakeBuildConfiguration(source->m_qmakeBuildConfiguration),
    m_subNodeBuild(0), // temporary value, so not copied
    m_fileNodeBuild(0)
{
    cloneSteps(source);
    ctor();
}

QmakeBuildConfiguration::~QmakeBuildConfiguration()
{
}

QVariantMap QmakeBuildConfiguration::toMap() const
{
    QVariantMap map(BuildConfiguration::toMap());
    map.insert(QLatin1String(USE_SHADOW_BUILD_KEY), m_shadowBuild);
    map.insert(QLatin1String(BUILD_CONFIGURATION_KEY), int(m_qmakeBuildConfiguration));
    return map;
}

bool QmakeBuildConfiguration::fromMap(const QVariantMap &map)
{
    if (!BuildConfiguration::fromMap(map))
        return false;

    m_shadowBuild = map.value(QLatin1String(USE_SHADOW_BUILD_KEY), true).toBool();
    m_qmakeBuildConfiguration = BaseQtVersion::QmakeBuildConfigs(map.value(QLatin1String(BUILD_CONFIGURATION_KEY)).toInt());

    m_qtVersionSupportsShadowBuilds =  supportsShadowBuilds();

    m_lastKitState = LastKitState(target()->kit());

    connect(ProjectExplorer::ToolChainManager::instance(), SIGNAL(toolChainUpdated(ProjectExplorer::ToolChain*)),
            this, SLOT(toolChainUpdated(ProjectExplorer::ToolChain*)));
    connect(QtSupport::QtVersionManager::instance(), SIGNAL(qtVersionsChanged(QList<int>,QList<int>,QList<int>)),
            this, SLOT(qtVersionsChanged(QList<int>,QList<int>,QList<int>)));
    return true;
}

void QmakeBuildConfiguration::ctor()
{
    connect(this, SIGNAL(environmentChanged()),
            this, SLOT(emitProFileEvaluateNeeded()));
    connect(target(), SIGNAL(kitChanged()),
            this, SLOT(kitChanged()));
}

void QmakeBuildConfiguration::kitChanged()
{
    LastKitState newState = LastKitState(target()->kit());
    if (newState != m_lastKitState) {
        // This only checks if the ids have changed!
        // For that reason the QmakeBuildConfiguration is also connected
        // to the toolchain and qtversion managers
        emitProFileEvaluateNeeded();
        updateShadowBuild();
        m_lastKitState = newState;
    }
}

void QmakeBuildConfiguration::toolChainUpdated(ProjectExplorer::ToolChain *tc)
{
    if (ToolChainKitInformation::toolChain(target()->kit()) == tc)
        emitProFileEvaluateNeeded();
}

void QmakeBuildConfiguration::qtVersionsChanged(const QList<int> &,const QList<int> &, const QList<int> &changed)
{
    if (changed.contains(QtKitInformation::qtVersionId(target()->kit())))
        emitProFileEvaluateNeeded();
}

void QmakeBuildConfiguration::updateShadowBuild()
{
    // We also emit buildDirectoryChanged if the Qt version's supportShadowBuild changed
    bool currentShadowBuild = supportsShadowBuilds();
    if (currentShadowBuild != m_qtVersionSupportsShadowBuilds) {
        if (!currentShadowBuild)
            setBuildDirectory(target()->project()->projectDirectory());
        m_qtVersionSupportsShadowBuilds = currentShadowBuild;
    }
}

NamedWidget *QmakeBuildConfiguration::createConfigWidget()
{
    return new QmakeProjectConfigWidget(this);
}

QString QmakeBuildConfiguration::defaultShadowBuildDirectory() const
{
    // todo displayName isn't ideal
    return shadowBuildDirectory(target()->project()->projectFilePath().toString(),
                                target()->kit(), displayName());
}

bool QmakeBuildConfiguration::supportsShadowBuilds()
{
    BaseQtVersion *version = QtKitInformation::qtVersion(target()->kit());
    return !version || version->supportsShadowBuilds();
}

/// If only a sub tree should be build this function returns which sub node
/// should be build
/// \see QMakeBuildConfiguration::setSubNodeBuild
QmakeProFileNode *QmakeBuildConfiguration::subNodeBuild() const
{
    return m_subNodeBuild;
}

/// A sub node build on builds a sub node of the project
/// That is triggered by a right click in the project explorer tree
/// The sub node to be build is set via this function immediately before
/// calling BuildManager::buildProject( BuildConfiguration * )
/// and reset immediately afterwards
/// That is m_subNodesBuild is set only temporarly
void QmakeBuildConfiguration::setSubNodeBuild(QmakeProFileNode *node)
{
    m_subNodeBuild = node;
}

FileNode *QmakeBuildConfiguration::fileNodeBuild() const
{
    return m_fileNodeBuild;
}

void QmakeBuildConfiguration::setFileNodeBuild(FileNode *node)
{
    m_fileNodeBuild = node;
}

/// returns whether this is a shadow build configuration or not
/// note, even if shadowBuild() returns true, it might be using the
/// source directory as the shadow build directory, thus it
/// still is a in-source build
bool QmakeBuildConfiguration::isShadowBuild() const
{
    return buildDirectory() != target()->project()->projectDirectory();
}

void QmakeBuildConfiguration::setBuildDirectory(const FileName &directory)
{
    if (directory == buildDirectory())
        return;
    BuildConfiguration::setBuildDirectory(directory);
    QTC_CHECK(supportsShadowBuilds()
              || (!supportsShadowBuilds()
                  && buildDirectory() == target()->project()->projectDirectory()));
    emitProFileEvaluateNeeded();
}

QString QmakeBuildConfiguration::makefile() const
{
    return static_cast<QmakeProject *>(target()->project())->rootQmakeProjectNode()->makefile();
}

BaseQtVersion::QmakeBuildConfigs QmakeBuildConfiguration::qmakeBuildConfiguration() const
{
    return m_qmakeBuildConfiguration;
}

void QmakeBuildConfiguration::setQMakeBuildConfiguration(BaseQtVersion::QmakeBuildConfigs config)
{
    if (m_qmakeBuildConfiguration == config)
        return;
    m_qmakeBuildConfiguration = config;

    emit qmakeBuildConfigurationChanged();
    emitProFileEvaluateNeeded();
    emit buildTypeChanged();
}

void QmakeBuildConfiguration::emitProFileEvaluateNeeded()
{
    Target *t = target();
    Project *p = t->project();
    if (t->activeBuildConfiguration() == this && p->activeTarget() == t)
        static_cast<QmakeProject *>(p)->scheduleAsyncUpdate();
}

void QmakeBuildConfiguration::emitQMakeBuildConfigurationChanged()
{
    emit qmakeBuildConfigurationChanged();
}

QStringList QmakeBuildConfiguration::configCommandLineArguments() const
{
    QStringList result;
    BaseQtVersion *version = QtKitInformation::qtVersion(target()->kit());
    BaseQtVersion::QmakeBuildConfigs defaultBuildConfiguration =
            version ? version->defaultBuildConfig() : BaseQtVersion::QmakeBuildConfigs(BaseQtVersion::DebugBuild | BaseQtVersion::BuildAll);
    BaseQtVersion::QmakeBuildConfigs userBuildConfiguration = m_qmakeBuildConfiguration;
    if ((defaultBuildConfiguration & BaseQtVersion::BuildAll) && !(userBuildConfiguration & BaseQtVersion::BuildAll))
        result << QLatin1String("CONFIG-=debug_and_release");

    if (!(defaultBuildConfiguration & BaseQtVersion::BuildAll) && (userBuildConfiguration & BaseQtVersion::BuildAll))
        result << QLatin1String("CONFIG+=debug_and_release");
    if ((defaultBuildConfiguration & BaseQtVersion::DebugBuild) && !(userBuildConfiguration & BaseQtVersion::DebugBuild))
        result << QLatin1String("CONFIG+=release");
    if (!(defaultBuildConfiguration & BaseQtVersion::DebugBuild) && (userBuildConfiguration & BaseQtVersion::DebugBuild))
        result << QLatin1String("CONFIG+=debug");
    return result;
}

QMakeStep *QmakeBuildConfiguration::qmakeStep() const
{
    QMakeStep *qs = 0;
    BuildStepList *bsl = stepList(Core::Id(ProjectExplorer::Constants::BUILDSTEPS_BUILD));
    Q_ASSERT(bsl);
    for (int i = 0; i < bsl->count(); ++i)
        if ((qs = qobject_cast<QMakeStep *>(bsl->at(i))) != 0)
            return qs;
    return 0;
}

MakeStep *QmakeBuildConfiguration::makeStep() const
{
    MakeStep *ms = 0;
    BuildStepList *bsl = stepList(Core::Id(ProjectExplorer::Constants::BUILDSTEPS_BUILD));
    Q_ASSERT(bsl);
    for (int i = 0; i < bsl->count(); ++i)
        if ((ms = qobject_cast<MakeStep *>(bsl->at(i))) != 0)
            return ms;
    return 0;
}

// Returns true if both are equal.
QmakeBuildConfiguration::MakefileState QmakeBuildConfiguration::compareToImportFrom(const QString &makefile)
{
    QMakeStep *qs = qmakeStep();
    if (QFileInfo::exists(makefile) && qs) {
        FileName qmakePath = QtVersionManager::findQMakeBinaryFromMakefile(makefile);
        BaseQtVersion *version = QtKitInformation::qtVersion(target()->kit());
        if (!version)
            return MakefileForWrongProject;
        if (QtSupport::QtVersionManager::makefileIsFor(makefile, qs->project()->projectFilePath().toString())
                != QtSupport::QtVersionManager::SameProject) {
            if (debug) {
                qDebug() << "different profile used to generate the Makefile:"
                         << makefile << " expected profile:" << qs->project()->projectFilePath();
            }
            return MakefileIncompatible;
        }
        if (version->qmakeCommand() == qmakePath) {
            // same qtversion
            QPair<BaseQtVersion::QmakeBuildConfigs, QString> result =
                    QtVersionManager::scanMakeFile(makefile, version->defaultBuildConfig());
            if (qmakeBuildConfiguration() == result.first) {
                // The qmake Build Configuration are the same,
                // now compare arguments lists
                // we have to compare without the spec/platform cmd argument
                // and compare that on its own
                QString workingDirectory = QFileInfo(makefile).absolutePath();
                QStringList actualArgs;
                QString userArgs = qs->userArguments();
                // This copies the settings from userArgs to actualArgs (minus some we
                // are not interested in), splitting them up into individual strings:
                extractSpecFromArguments(&userArgs, workingDirectory, version, &actualArgs);
                actualArgs = qs->deducedArguments() + actualArgs + qs->deducedArgumentsAfter();
                FileName actualSpec = qs->mkspec();

                QString qmakeArgs = result.second;
                QStringList parsedArgs;
                FileName parsedSpec = extractSpecFromArguments(&qmakeArgs, workingDirectory, version, &parsedArgs);

                if (debug) {
                    qDebug() << "Actual args:" << actualArgs;
                    qDebug() << "Parsed args:" << parsedArgs;
                    qDebug() << "Actual spec:" << actualSpec.toString();
                    qDebug() << "Parsed spec:" << parsedSpec.toString();
                }

                // Comparing the sorted list is obviously wrong
                // Though haven written a more complete version
                // that managed had around 200 lines and yet faild
                // to be actually foolproof at all, I think it's
                // not feasible without actually taking the qmake
                // command line parsing code

                // Things, sorting gets wrong:
                // parameters to positional parameters matter
                //  e.g. -o -spec is different from -spec -o
                //       -o 1 -spec 2 is diffrent from -spec 1 -o 2
                // variable assignment order matters
                // variable assignment vs -after
                // -norecursive vs. recursive
                actualArgs.sort();
                parsedArgs.sort();
                if (actualArgs == parsedArgs) {
                    // Specs match exactly
                    if (actualSpec == parsedSpec)
                        return MakefileMatches;
                    // Actual spec is the default one
//                    qDebug() << "AS vs VS" << actualSpec << version->mkspec();
                    if ((actualSpec == version->mkspec() || actualSpec == FileName::fromLatin1("default"))
                        && (parsedSpec == version->mkspec() || parsedSpec == FileName::fromLatin1("default") || parsedSpec.isEmpty()))
                        return MakefileMatches;
                }
                return MakefileIncompatible;
            } else {
                if (debug)
                    qDebug() << "different qmake buildconfigurations buildconfiguration:"
                             << qmakeBuildConfiguration() << " Makefile:" << result.first;
                return MakefileIncompatible;
            }
        } else {
            if (debug)
                qDebug() << "different Qt versions, buildconfiguration:" << version->qmakeCommand().toString()
                         << " Makefile:"<< qmakePath.toString();
            return MakefileForWrongProject;
        }
    }
    return MakefileMissing;
}

bool QmakeBuildConfiguration::removeQMLInspectorFromArguments(QString *args)
{
    bool removedArgument = false;
    for (QtcProcess::ArgIterator ait(args); ait.next(); ) {
        const QString arg = ait.value();
        if (arg.contains(QLatin1String(Constants::QMAKEVAR_QMLJSDEBUGGER_PATH))
            || arg.contains(QLatin1String(Constants::QMAKEVAR_QUICK1_DEBUG))
            || arg.contains(QLatin1String(Constants::QMAKEVAR_QUICK2_DEBUG))) {
            ait.deleteArg();
            removedArgument = true;
        }
    }
    return removedArgument;
}

FileName QmakeBuildConfiguration::extractSpecFromArguments(QString *args,
                                                         const QString &directory, const BaseQtVersion *version,
                                                         QStringList *outArgs)
{
    FileName parsedSpec;

    bool ignoreNext = false;
    bool nextIsSpec = false;
    for (QtcProcess::ArgIterator ait(args); ait.next(); ) {
        if (ignoreNext) {
            ignoreNext = false;
            ait.deleteArg();
        } else if (nextIsSpec) {
            nextIsSpec = false;
            parsedSpec = FileName::fromUserInput(ait.value());
            ait.deleteArg();
        } else if (ait.value() == QLatin1String("-spec") || ait.value() == QLatin1String("-platform")) {
            nextIsSpec = true;
            ait.deleteArg();
        } else if (ait.value() == QLatin1String("-cache")) {
            // We ignore -cache, because qmake contained a bug that it didn't
            // mention the -cache in the Makefile.
            // That means changing the -cache option in the additional arguments
            // does not automatically rerun qmake. Alas, we could try more
            // intelligent matching for -cache, but i guess people rarely
            // do use that.
            ignoreNext = true;
            ait.deleteArg();
        } else if (outArgs && ait.isSimple()) {
            outArgs->append(ait.value());
        }
    }

    if (parsedSpec.isEmpty())
        return FileName();

    FileName baseMkspecDir = FileName::fromUserInput(
            version->qmakeProperty("QT_HOST_DATA") + QLatin1String("/mkspecs"));
    baseMkspecDir = Utils::FileName::fromString(baseMkspecDir.toFileInfo().canonicalFilePath());

    // if the path is relative it can be
    // relative to the working directory (as found in the Makefiles)
    // or relatively to the mkspec directory
    // if it is the former we need to get the canonical form
    // for the other one we don't need to do anything
    if (parsedSpec.toFileInfo().isRelative()) {
        if (QFileInfo::exists(directory + QLatin1Char('/') + parsedSpec.toString()))
            parsedSpec = FileName::fromUserInput(directory + QLatin1Char('/') + parsedSpec.toString());
        else
            parsedSpec = FileName::fromUserInput(baseMkspecDir.toString() + QLatin1Char('/') + parsedSpec.toString());
    }

    QFileInfo f2 = parsedSpec.toFileInfo();
    while (f2.isSymLink()) {
        parsedSpec = FileName::fromString(f2.symLinkTarget());
        f2.setFile(parsedSpec.toString());
    }

    if (parsedSpec.isChildOf(baseMkspecDir)) {
        parsedSpec = parsedSpec.relativeChildPath(baseMkspecDir);
    } else {
        FileName sourceMkSpecPath = FileName::fromString(version->sourcePath().toString()
                                                         + QLatin1String("/mkspecs"));
        if (parsedSpec.isChildOf(sourceMkSpecPath))
            parsedSpec = parsedSpec.relativeChildPath(sourceMkSpecPath);
    }
    return parsedSpec;
}

QStringList QmakeBuildConfiguration::extractDeducedArguments(QString *args)
{
    QStringList allPossibleDeducedArguments;
    allPossibleDeducedArguments << QLatin1String("CONFIG+=x86")
                                << QLatin1String("CONFIG+=x86_64")
                                << QLatin1String("CONFIG+=iphonesimulator")
                                << QLatin1String("CONFIG+=ppc")
                                << QLatin1String("CONFIG+=ppc64")
                                << QLatin1String("CONFIG+=iphoneos");
    QStringList result;
    for (QtcProcess::ArgIterator ait(args); ait.next(); ) {
        QString arg = ait.value();
        if (allPossibleDeducedArguments.contains(arg))  {
            result << arg;
            ait.deleteArg();
        }
    }
    return result;
}

QStringList QmakeBuildConfiguration::deduceArgumnetsForTargetAbi(const ProjectExplorer::Abi &targetAbi, const BaseQtVersion *version)
{
    QStringList arguments;
    if ((targetAbi.os() == ProjectExplorer::Abi::MacOS)
            && (targetAbi.binaryFormat() == ProjectExplorer::Abi::MachOFormat)) {
        if (targetAbi.architecture() == ProjectExplorer::Abi::X86Architecture) {
            if (targetAbi.wordWidth() == 32)
                arguments << QLatin1String("CONFIG+=x86");
            else if (targetAbi.wordWidth() == 64)
                arguments << QLatin1String("CONFIG+=x86_64");

            const char IOSQT[] = "Qt4ProjectManager.QtVersion.Ios";
            if (version && version->type() == QLatin1String(IOSQT)) // ugly, we can't distinguish between ios and mac toolchains
                arguments << QLatin1String("CONFIG+=iphonesimulator");
        } else if (targetAbi.architecture() == ProjectExplorer::Abi::PowerPCArchitecture) {
            if (targetAbi.wordWidth() == 32)
                arguments << QLatin1String("CONFIG+=ppc");
            else if (targetAbi.wordWidth() == 64)
                arguments << QLatin1String("CONFIG+=ppc64");
        } else if (targetAbi.architecture() == ProjectExplorer::Abi::ArmArchitecture) {
            arguments << QLatin1String("CONFIG+=iphoneos");
        }
    }
    return arguments;
}

bool QmakeBuildConfiguration::isEnabled() const
{
    return m_isEnabled;
}

QString QmakeBuildConfiguration::disabledReason() const
{
    if (!m_isEnabled)
        return tr("Parsing the .pro file");
    return QString();
}

void QmakeBuildConfiguration::setEnabled(bool enabled)
{
    if (m_isEnabled == enabled)
        return;
    m_isEnabled = enabled;
    emit enabledChanged();
}

/*!
  \class QmakeBuildConfigurationFactory
*/

QmakeBuildConfigurationFactory::QmakeBuildConfigurationFactory(QObject *parent) :
    IBuildConfigurationFactory(parent)
{
    update();

    connect(QtVersionManager::instance(),
            SIGNAL(qtVersionsChanged(QList<int>,QList<int>,QList<int>)),
            this, SLOT(update()));
}

QmakeBuildConfigurationFactory::~QmakeBuildConfigurationFactory()
{
}

void QmakeBuildConfigurationFactory::update()
{
    emit availableCreationIdsChanged();
}

bool QmakeBuildConfigurationFactory::canHandle(const Target *t) const
{
    if (!t->project()->supportsKit(t->kit()))
        return false;
    return qobject_cast<QmakeProject *>(t->project());
}

QmakeBuildInfo *QmakeBuildConfigurationFactory::createBuildInfo(const Kit *k,
                                                              const QString &projectPath,
                                                              BuildConfiguration::BuildType type) const
{
    QtSupport::BaseQtVersion *version = QtSupport::QtKitInformation::qtVersion(k);
    QmakeBuildInfo *info = new QmakeBuildInfo(this);
    QString suffix;
    if (type == BuildConfiguration::Release) {
        //: The name of the release build configuration created by default for a qmake project.
        info->displayName = tr("Release");
        //: Non-ASCII characters in directory suffix may cause build issues.
        suffix = tr("Release", "Shadow build directory suffix");
    } else {
        //: The name of the debug build configuration created by default for a qmake project.
        info->displayName = tr("Debug");
        //: Non-ASCII characters in directory suffix may cause build issues.
        suffix = tr("Debug", "Shadow build directory suffix");
    }
    info->typeName = tr("Build");
    // Leave info->buildDirectory unset;
    info->kitId = k->id();
    info->supportsShadowBuild = (version && version->supportsShadowBuilds());

    // check if this project is in the source directory:
    Utils::FileName projectFilePath = Utils::FileName::fromString(projectPath);
    if (version->isInSourceDirectory(projectFilePath)) {
        // assemble build directory
        QString projectDirectory = projectFilePath.toFileInfo().absolutePath();
        QDir qtSourceDir = QDir(version->sourcePath().toString());
        QString relativeProjectPath = qtSourceDir.relativeFilePath(projectDirectory);
        QString qtBuildDir = version->versionInfo().value(QStringLiteral("QT_INSTALL_PREFIX"));
        QString absoluteBuildPath = QDir::cleanPath(qtBuildDir + QLatin1Char('/') + relativeProjectPath);

        info->buildDirectory = Utils::FileName::fromString(absoluteBuildPath);
    } else {
        info->buildDirectory
                = defaultBuildDirectory(info->supportsShadowBuild, projectPath, k, suffix);
    }
    info->type = type;
    return info;
}

int QmakeBuildConfigurationFactory::priority(const Target *parent) const
{
    return canHandle(parent) ? 0 : -1;
}

QList<BuildInfo *> QmakeBuildConfigurationFactory::availableBuilds(const Target *parent) const
{
    QList<ProjectExplorer::BuildInfo *> result;
    QmakeBuildInfo *info = createBuildInfo(parent->kit(), parent->project()->projectFilePath().toString(),
                                           BuildConfiguration::Debug);
    info->displayName.clear(); // ask for a name
    info->buildDirectory.clear(); // This depends on the displayName
    result << info;

    return result;
}

int QmakeBuildConfigurationFactory::priority(const Kit *k, const QString &projectPath) const
{
    return (k && Core::MimeDatabase::findByFile(QFileInfo(projectPath))
            .matchesType(QLatin1String(Constants::PROFILE_MIMETYPE))) ? 0 : -1;
}

QList<BuildInfo *> QmakeBuildConfigurationFactory::availableSetups(const Kit *k, const QString &projectPath) const
{
    QList<ProjectExplorer::BuildInfo *> result;
    result << createBuildInfo(k, projectPath, ProjectExplorer::BuildConfiguration::Debug);
    result << createBuildInfo(k, projectPath, ProjectExplorer::BuildConfiguration::Release);
    return result;
}

BuildConfiguration *QmakeBuildConfigurationFactory::create(Target *parent, const BuildInfo *info) const
{
    QTC_ASSERT(info->factory() == this, return 0);
    QTC_ASSERT(info->kitId == parent->kit()->id(), return 0);
    QTC_ASSERT(!info->displayName.isEmpty(), return 0);

    const QmakeBuildInfo *qmakeInfo = static_cast<const QmakeBuildInfo *>(info);

    BaseQtVersion *version = QtSupport::QtKitInformation::qtVersion(parent->kit());
    QTC_ASSERT(version, return 0);

    BaseQtVersion::QmakeBuildConfigs config = version->defaultBuildConfig();
    if (qmakeInfo->type == BuildConfiguration::Release)
        config &= ~QtSupport::BaseQtVersion::DebugBuild;
    else
        config |= QtSupport::BaseQtVersion::DebugBuild;

    QmakeBuildConfiguration *bc = new QmakeBuildConfiguration(parent);
    bc->setDefaultDisplayName(info->displayName);
    bc->setDisplayName(info->displayName);

    BuildStepList *buildSteps = bc->stepList(Core::Id(ProjectExplorer::Constants::BUILDSTEPS_BUILD));
    BuildStepList *cleanSteps = bc->stepList(Core::Id(ProjectExplorer::Constants::BUILDSTEPS_CLEAN));
    Q_ASSERT(buildSteps);
    Q_ASSERT(cleanSteps);

    QMakeStep *qmakeStep = new QMakeStep(buildSteps);
    buildSteps->insertStep(0, qmakeStep);

    MakeStep *makeStep = new MakeStep(buildSteps);
    buildSteps->insertStep(1, makeStep);

    MakeStep *cleanStep = new MakeStep(cleanSteps);
    cleanStep->setClean(true);
    cleanStep->setUserArguments(QLatin1String("clean"));
    cleanSteps->insertStep(0, cleanStep);

    QString additionalArguments = qmakeInfo->additionalArguments;

    bool enableQmlDebugger
            = QmakeBuildConfiguration::removeQMLInspectorFromArguments(&additionalArguments);
    if (!additionalArguments.isEmpty())
        qmakeStep->setUserArguments(additionalArguments);
    if (!qmakeInfo->makefile.isEmpty())
        qmakeStep->setLinkQmlDebuggingLibrary(enableQmlDebugger);

    bc->setQMakeBuildConfiguration(config);

    Utils::FileName directory = qmakeInfo->buildDirectory;
    if (directory.isEmpty()) {
        directory = defaultBuildDirectory(qmakeInfo->supportsShadowBuild,
                                          parent->project()->projectFilePath().toString(),
                                          parent->kit(), info->displayName);
    }

    bc->setBuildDirectory(directory);
    return bc;
}

bool QmakeBuildConfigurationFactory::canClone(const Target *parent, BuildConfiguration *source) const
{
    return canHandle(parent) && qobject_cast<QmakeBuildConfiguration *>(source);
}

BuildConfiguration *QmakeBuildConfigurationFactory::clone(Target *parent, BuildConfiguration *source)
{
    if (!canClone(parent, source))
        return 0;
    QmakeBuildConfiguration *oldbc(static_cast<QmakeBuildConfiguration *>(source));
    return new QmakeBuildConfiguration(parent, oldbc);
}

bool QmakeBuildConfigurationFactory::canRestore(const Target *parent, const QVariantMap &map) const
{
    if (!canHandle(parent))
        return false;
    return ProjectExplorer::idFromMap(map) == QMAKE_BC_ID;
}

BuildConfiguration *QmakeBuildConfigurationFactory::restore(Target *parent, const QVariantMap &map)
{
    if (!canRestore(parent, map))
        return 0;
    QmakeBuildConfiguration *bc = new QmakeBuildConfiguration(parent);
    if (bc->fromMap(map))
        return bc;
    delete bc;
    return 0;
}

BuildConfiguration::BuildType QmakeBuildConfiguration::buildType() const
{
    if (qmakeBuildConfiguration() & BaseQtVersion::DebugBuild)
        return Debug;
    else
        return Release;
}

QmakeBuildConfiguration::LastKitState::LastKitState()
{

}

QmakeBuildConfiguration::LastKitState::LastKitState(Kit *k)
    : m_qtVersion(QtKitInformation::qtVersionId(k)),
      m_sysroot(SysRootKitInformation::sysRoot(k).toString()),
      m_mkspec(QmakeKitInformation::mkspec(k).toString())
{
    ToolChain *tc = ToolChainKitInformation::toolChain(k);
    m_toolchain = tc ? tc->id() : QString();
}

bool QmakeBuildConfiguration::LastKitState::operator ==(const LastKitState &other) const
{
    return m_qtVersion == other.m_qtVersion
            && m_toolchain == other.m_toolchain
            && m_sysroot == other.m_sysroot
            && m_mkspec == other.m_mkspec;
}

bool QmakeBuildConfiguration::LastKitState::operator !=(const LastKitState &other) const
{
    return !operator ==(other);
}

} // namespace QmakeProjectManager
