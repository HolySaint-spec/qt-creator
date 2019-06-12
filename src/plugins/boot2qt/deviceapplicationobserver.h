/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include <projectexplorer/devicesupport/idevice.h>

#include <QStringList>

namespace ProjectExplorer { class ApplicationLauncher; }

namespace Qdb {
namespace Internal {

class Command {
public:
    Command(const QString &bin, const QStringList &args = QStringList())
        : binary(bin), arguments(args) {}

    QString binary;
    QStringList arguments;
};

class DeviceApplicationObserver : public QObject
{
    Q_OBJECT
public:
    explicit DeviceApplicationObserver(QObject *parent = 0);

    // Once all commands have finished, this object will delete itself.
    void start(const ProjectExplorer::IDevice::ConstPtr &device, const QList<Command> &commands);

private:
    void handleStdout(const QString &data);
    void handleStderr(const QString &data);
    void handleError(const QString &message);
    void handleFinished(bool success);
    void runNext();

    QString m_stdout;
    QString m_stderr;
    QList<Command> m_commandsToRun;
    ProjectExplorer::ApplicationLauncher * const m_appRunner;
    ProjectExplorer::IDevice::ConstPtr m_device;
    QString m_error;
};

} // namespace Internal
} // namespace Qdb