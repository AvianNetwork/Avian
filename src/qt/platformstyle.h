// Copyright (c) 2015-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORMSTYLE_H
#define BITCOIN_QT_PLATFORMSTYLE_H

#include <QIcon>
#include <QPixmap>
#include <QString>

/** AVN: Global dark mode flag */
extern bool darkModeEnabled;

/* Coin network-specific GUI style information */
class PlatformStyle
{
public:
    /** Get style associated with provided platform name, or 0 if not known */
    static const PlatformStyle *instantiate(const QString &platformId);

    const QString &getName() const { return name; }

    bool getImagesOnButtons() const { return imagesOnButtons; }
    bool getUseExtraSpacing() const { return useExtraSpacing; }

    QColor TextColor() const;
    QColor SingleColor() const;

    /** Colorize an image (given filename) with the icon color */
    QImage SingleColorImage(const QString& filename) const;

    /** Colorize an icon (given filename) with the icon color */
    QIcon SingleColorIcon(const QString& filename) const;

    /** Colorize an icon (given filename) with a specific color */
    QIcon SingleColorIcon(const QString& filename, const QColor& colorbase) const;

    /** Colorize an icon (given object) with the icon color */
    QIcon SingleColorIcon(const QIcon& icon) const;

    /** Colorize an icon (given object) with the text color */
    QIcon TextColorIcon(const QIcon& icon) const;

    /** AVN: Color accessors for theming */
    QColor WidgetBackGroundColor() const;
    QColor Avian_2B737F() const;
    QColor Avian_18A7B7() const;
    QColor Avian_19827B() const;
    QColor Avian_34E2D6() const;
    QColor ToolBarSelectedTextColor() const;
    QColor ToolBarNotSelectedTextColor() const;
    QColor MainBackGroundColor() const;
    QColor TopWidgetBackGroundColor() const;
    QColor SendEntriesBackGroundColor() const;
    QColor ShadowColor() const;
    QColor LightBlueColor() const;
    QColor DarkBlueColor() const;
    QColor LightOrangeColor() const;
    QColor DarkOrangeColor() const;
    QColor AssetTxColor() const;

    /** AVN: Create icon with on/off states for toolbar */
    QIcon SingleColorIconOnOff(const QString& filenameOn, const QString& filenameOff) const;

private:
    PlatformStyle(const QString &name, bool imagesOnButtons, bool colorizeIcons, bool useExtraSpacing);

    QString name;
    bool imagesOnButtons;
    bool colorizeIcons;
    bool useExtraSpacing;
};

#endif // BITCOIN_QT_PLATFORMSTYLE_H

