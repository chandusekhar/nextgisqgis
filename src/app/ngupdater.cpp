/******************************************************************************
 * Project: NextGIS QGIS
 * Purpose: NextGIS QGIS Customization
 * Author:  Alexander Lisovenko, alexander.lisovenko@nextgis.com
 ******************************************************************************
 *   Copyright (c) 2017 NextGIS, <info@nextgis.com>
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#include "ngupdater.h"

#include "qgsmessagebar.h"
#include "qgsmessagebaritem.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QApplication>
#include <QProcess>
#include <QXmlStreamReader>
#include <QMessageBox>

NGQgisUpdater::NGQgisUpdater( QWidget* parent )
    : QObject( parent )
{
    mMaintainerProcess = new QProcess(this);
       
    connect(mMaintainerProcess, SIGNAL(started()), this, SLOT(maintainerStrated()) );
    connect(mMaintainerProcess, SIGNAL(error(QProcess::ProcessError)), this, SLOT(maintainerErrored(QProcess::ProcessError)));
    connect(mMaintainerProcess, SIGNAL(stateChanged(QProcess::ProcessState)), this, SLOT(maintainerStateChanged(QProcess::ProcessState)));
    connect(mMaintainerProcess, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(maintainerFinished(int, QProcess::ExitStatus)));
    connect(mMaintainerProcess, SIGNAL(readyReadStandardOutput()), this, SLOT(maintainerReadyReadStandardOutput()));
    connect(mMaintainerProcess, SIGNAL(readyReadStandardError()), this, SLOT(maintainerReadyReadStandardError()));
}

NGQgisUpdater::~NGQgisUpdater()
{
}

const QString NGQgisUpdater::updateProgrammPath()
{
  return QFileInfo(
    QApplication::instance()->applicationDirPath()
  ).absolutePath() + QDir::separator() + "nextgisupdater.exe";
}

void NGQgisUpdater::checkUpdates()
{
#ifdef Q_OS_WIN32
    if (mMaintainerProcess->state() != QProcess::NotRunning)
    {
        return;
    }

    const QString path = updateProgrammPath();
    QStringList args;
    args << "--checkupdates";


    mMaintainerProcess->start(path, args);
#endif
}

void NGQgisUpdater::maintainerStrated()
{
    this->updatesInfoGettingStarted();
}

void NGQgisUpdater::maintainerErrored(QProcess::ProcessError)
{
}

void NGQgisUpdater::maintainerStateChanged(QProcess::ProcessState)
{
}

void NGQgisUpdater::maintainerFinished(int code, QProcess::ExitStatus status)
{
    QProcess* prc = (QProcess*) sender();

    QByteArray data = prc->readAllStandardOutput();
    QXmlStreamReader xml(data);
    while (!xml.atEnd() && !xml.hasError())
    {
        QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartDocument)
            continue;
        if (token == QXmlStreamReader::StartElement)
        {
            if (xml.name() == "update")
            {
                if (!ignorePackages().contains(xml.attributes().value("name").toString(), Qt::CaseInsensitive))
                {
                    this->updatesInfoGettingFinished(true);
                    return;
                }
            }
        }
    }
    this->updatesInfoGettingFinished(false);
}

void NGQgisUpdater::maintainerReadyReadStandardOutput()
{
}

void NGQgisUpdater::maintainerReadyReadStandardError()
{
}

void NGQgisUpdater::startUpdate()
{
  QString program = updateProgrammPath();
  QStringList arguments;
  arguments << "--updater";
  
  int status = QProcess::execute(
    program,
    arguments
  );

  if(status == 0)
  {
    QMessageBox::information((QWidget*)parent(), tr("Updates installed"), tr("Please, restart QGIS."));
  }
}

const QStringList NGQgisUpdater::ignorePackages()
{
    QStringList packages;
    packages << "Qt5";
    packages << "Formbuilder";
    return packages;
}