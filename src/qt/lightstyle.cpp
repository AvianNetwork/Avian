/*
###############################################################################
#                                                                             #
# The MIT License                                                             #
#                                                                             #
# Copyright (C) 2017 by Juergen Skrotzky (JorgenVikingGod@gmail.com)          #
#               >> https://github.com/Jorgen-VikingGod                        #
#                                                                             #
# Sources: https://github.com/Jorgen-VikingGod/Qt-Frameless-Window-DarkStyle  #
#                                                                             #
###############################################################################
*/

#include <QDebug>
#include <QRegularExpression>
#include <vector>
#include <memory>
#include <tinyformat.h>

#include "lightstyle.h"
#include "aviangui.h"

LightStyle::LightStyle():
  LightStyle(styleBase())
{ }

LightStyle::LightStyle(QStyle *style):
  QProxyStyle(style)
{ }

QStyle *LightStyle::styleBase(QStyle *style) const {
  static QStyle *base = !style ? QStyleFactory::create(QStringLiteral("Fusion")) : style;
  return base;
}

QStyle *LightStyle::baseStyle() const
{
  return styleBase();
}

void LightStyle::polish(QPalette &palette)
{
  // modify palette to light
  palette.setColor(QPalette::Window,QColor(202,202,202));
  palette.setColor(QPalette::WindowText,Qt::black);/*
###############################################################################
#                                                                             #
# The MIT License                                                             #
#                                                                             #
# Copyright (C) 2017 by Juergen Skrotzky (JorgenVikingGod@gmail.com)          #
#               >> https://github.com/Jorgen-VikingGod                        #
#                                                                             #
# Sources: https://github.com/Jorgen-VikingGod/Qt-Frameless-Window-DarkStyle  #
#                                                                             #
###############################################################################
*/
  palette.setColor(QPalette::Disabled,QPalette::WindowText,QColor(127,127,127));
  palette.setColor(QPalette::Base,QColor(213,213,213));
  palette.setColor(QPalette::AlternateBase,QColor(213,213,213));
  palette.setColor(QPalette::ToolTipBase,Qt::white);
  palette.setColor(QPalette::ToolTipText,QColor(202,202,202));
  palette.setColor(QPalette::Text,Qt::black);
  palette.setColor(QPalette::Disabled,QPalette::Text,QColor(127,127,127));
  palette.setColor(QPalette::Dark,QColor(220,220,220));
  palette.setColor(QPalette::Shadow,QColor(235,235,235));
  palette.setColor(QPalette::Button,QColor(202,202,202));
  palette.setColor(QPalette::ButtonText,Qt::white);
  palette.setColor(QPalette::Disabled,QPalette::ButtonText,QColor(127,127,127));
  palette.setColor(QPalette::BrightText,Qt::red);
  palette.setColor(QPalette::Link,QColor(43, 115, 127));
  palette.setColor(QPalette::Highlight,QColor(43, 115, 127));
  palette.setColor(QPalette::Disabled,QPalette::Highlight,QColor(175,175,175));
  palette.setColor(QPalette::HighlightedText,Qt::black);
  palette.setColor(QPalette::Disabled,QPalette::HighlightedText,QColor(127,127,127));
}

void LightStyle::polish(QApplication *app)
{
  if (!app) return;

  QString stylesheet = ""; 
  
  std::vector<QString> vecFiles;
  vecFiles.push_back(":/css/general");
  vecFiles.push_back(":/css/Light");

  for (const auto& file : vecFiles) {
    // loadstylesheet
    QFile qFile(file);
    if (qFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      std::string platformName = AvianGUI::DEFAULT_UIPLATFORM;

      // set stylesheet
      QString strStyle = QString::fromLatin1(qFile.readAll());

      // Process all <os=...></os> groups in the stylesheet first
      QRegularExpressionMatch osStyleMatch;
      QRegularExpression osStyleExp(
              "^"
              "(<os=(?:'|\").+(?:'|\")>)" // group 1
              "((?:.|\n)+?)"              // group 2
              "(</os>?)"                  // group 3
              "$");
      osStyleExp.setPatternOptions(QRegularExpression::MultilineOption);
      QRegularExpressionMatchIterator it = osStyleExp.globalMatch(strStyle);

      // For all <os=...></os> sections
      while (it.hasNext() && (osStyleMatch = it.next()).isValid()) {
        QStringList listMatches = osStyleMatch.capturedTexts();
        // Full match + 3 group matches
        if (listMatches.size() % 4) {
          throw std::runtime_error(strprintf("%s: Invalid <os=...></os> section in file Light.css", __func__));
        }

        for (int i = 0; i < listMatches.size(); i += 4) {
          if (!listMatches[i + 1].contains(QString::fromStdString(platformName))) {
            // If os is not supported for this styles
            // just remove the full match
            strStyle.replace(listMatches[i], "");
          } else {
            // If its supported remove the <os=...></os> tags
            strStyle.replace(listMatches[i + 1], "");
            strStyle.replace(listMatches[i + 3], "");
          }
        }
        stylesheet += strStyle;
      }
      qFile.close();
    }
  }
  app->setStyleSheet(stylesheet);
}
