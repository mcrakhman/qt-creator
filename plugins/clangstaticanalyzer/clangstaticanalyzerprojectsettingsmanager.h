/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd
** All rights reserved.
** For any questions to The Qt Company, please use contact form at http://www.qt.io/contact-us
**
** This file is part of the Qt Enterprise ClangStaticAnalyzer Add-on.
**
** Licensees holding valid Qt Enterprise licenses may use this file in
** accordance with the Qt Enterprise License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company.
**
** If you have questions regarding the use of this file, please use
** contact form at http://www.qt.io/contact-us
**
****************************************************************************/

#ifndef CLANGSTATICANALYZERPROJECTSETTINGSMANAGER_H
#define CLANGSTATICANALYZERPROJECTSETTINGSMANAGER_H

namespace ProjectExplorer { class Project; }

#include <QHash>
#include <QSharedPointer>

namespace ClangStaticAnalyzer {
namespace Internal {
class ProjectSettings;

class ProjectSettingsManager
{
public:
    ProjectSettingsManager();

    static ProjectSettings *getSettings(ProjectExplorer::Project *project);

private:
    static void handleProjectToBeRemoved(ProjectExplorer::Project *project);

    typedef QHash<ProjectExplorer::Project *, QSharedPointer<ProjectSettings>> SettingsMap;
    static SettingsMap m_settings;
};

} // namespace Internal
} // namespace ClangStaticAnalyzer

#endif // Include guard.