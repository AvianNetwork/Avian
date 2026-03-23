// Copyright (c) 2015-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platformstyle.h>

#include <qt/guiconstants.h>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPalette>

/** AVN: Dark mode state */
bool darkModeEnabled = false;

static const struct {
    const char *platformId;
    /** Show images on push buttons */
    const bool imagesOnButtons;
    /** Colorize single-color icons */
    const bool colorizeIcons;
    /** Extra padding/spacing in transactionview */
    const bool useExtraSpacing;
} platform_styles[] = {
    {"macosx", false, true, true},
    {"windows", false, true, false},
    /* Other: linux, unix, ... */
    {"other", false, true, false}
};

namespace {
/* Local functions for colorizing single-color images */

void MakeSingleColorImage(QImage& img, const QColor& colorbase)
{
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int x = img.width(); x--; )
    {
        for (int y = img.height(); y--; )
        {
            const QRgb rgb = img.pixel(x, y);
            img.setPixel(x, y, qRgba(colorbase.red(), colorbase.green(), colorbase.blue(), qAlpha(rgb)));
        }
    }
}

QIcon ColorizeIcon(const QIcon& ico, const QColor& colorbase)
{
    QIcon new_ico;
    for (const QSize& sz : ico.availableSizes())
    {
        QImage img(ico.pixmap(sz).toImage());
        MakeSingleColorImage(img, colorbase);
        new_ico.addPixmap(QPixmap::fromImage(img));
    }
    return new_ico;
}

QImage ColorizeImage(const QString& filename, const QColor& colorbase)
{
    QImage img(filename);
    MakeSingleColorImage(img, colorbase);
    return img;
}

QIcon ColorizeIcon(const QString& filename, const QColor& colorbase)
{
    return QIcon(QPixmap::fromImage(ColorizeImage(filename, colorbase)));
}

}


PlatformStyle::PlatformStyle(const QString &_name, bool _imagesOnButtons, bool _colorizeIcons, bool _useExtraSpacing):
    name(_name),
    imagesOnButtons(_imagesOnButtons),
    colorizeIcons(_colorizeIcons),
    useExtraSpacing(_useExtraSpacing)
{
}

QColor PlatformStyle::TextColor() const
{
    if (darkModeEnabled)
        return COLOR_TOOLBAR_SELECTED_TEXT_DARK_MODE;
    return STRING_LABEL_COLOR;
}

QColor PlatformStyle::SingleColor() const
{
    return COLOR_AVIAN_34E2D6;
}

QImage PlatformStyle::SingleColorImage(const QString& filename) const
{
    if (!colorizeIcons)
        return QImage(filename);
    return ColorizeImage(filename, SingleColor());
}

QIcon PlatformStyle::SingleColorIcon(const QString& filename) const
{
    if (!colorizeIcons)
        return QIcon(filename);
    return ColorizeIcon(filename, SingleColor());
}

QIcon PlatformStyle::SingleColorIcon(const QIcon& icon) const
{
    if (!colorizeIcons)
        return icon;
    return ColorizeIcon(icon, SingleColor());
}

QIcon PlatformStyle::TextColorIcon(const QIcon& icon) const
{
    return ColorizeIcon(icon, TextColor());
}

QIcon PlatformStyle::SingleColorIcon(const QString& filename, const QColor& colorbase) const
{
    return ColorizeIcon(filename, colorbase);
}

QIcon PlatformStyle::SingleColorIconOnOff(const QString& filenameOn, const QString& filenameOff) const
{
    QIcon icon;
    icon.addPixmap(QPixmap(filenameOn), QIcon::Normal, QIcon::On);
    icon.addPixmap(QPixmap(filenameOff), QIcon::Normal, QIcon::Off);
    return icon;
}

QColor PlatformStyle::ToolBarSelectedTextColor() const
{
    return COLOR_TOOLBAR_SELECTED_TEXT;
}

QColor PlatformStyle::ToolBarNotSelectedTextColor() const
{
    if (darkModeEnabled)
        return COLOR_TOOLBAR_NOT_SELECTED_TEXT_DARK_MODE;
    return COLOR_TOOLBAR_NOT_SELECTED_TEXT;
}

QColor PlatformStyle::MainBackGroundColor() const
{
    if (darkModeEnabled)
        return COLOR_WIDGET_BACKGROUND_DARK;
    return COLOR_BACKGROUND_LIGHT;
}

QColor PlatformStyle::TopWidgetBackGroundColor() const
{
    if (darkModeEnabled)
        return COLOR_PRICING_WIDGET;
    return COLOR_BACKGROUND_LIGHT;
}

QColor PlatformStyle::WidgetBackGroundColor() const
{
    if (darkModeEnabled)
        return COLOR_WIDGET_BACKGROUND_DARK;
    return COLOR_WIDGET_BACKGROUND;
}

QColor PlatformStyle::SendEntriesBackGroundColor() const
{
    if (darkModeEnabled)
        return COLOR_SENDENTRIES_BACKGROUND_DARK;
    return COLOR_SENDENTRIES_BACKGROUND;
}

QColor PlatformStyle::ShadowColor() const
{
    if (darkModeEnabled)
        return COLOR_SHADOW_DARK;
    return COLOR_SHADOW_LIGHT;
}

QColor PlatformStyle::LightBlueColor() const
{
    if (darkModeEnabled)
        return COLOR_LIGHT_BLUE_DARK;
    return COLOR_LIGHT_BLUE;
}

QColor PlatformStyle::DarkBlueColor() const
{
    if (darkModeEnabled)
        return COLOR_DARK_BLUE_DARK;
    return COLOR_DARK_BLUE;
}

QColor PlatformStyle::LightOrangeColor() const
{
    return COLOR_LIGHT_ORANGE;
}

QColor PlatformStyle::DarkOrangeColor() const
{
    return COLOR_DARK_ORANGE;
}

QColor PlatformStyle::Avian_18A7B7() const
{
    return COLOR_AVIAN_18A7B7;
}

QColor PlatformStyle::Avian_19827B() const
{
    return COLOR_AVIAN_19827B;
}

QColor PlatformStyle::Avian_2B737F() const
{
    return COLOR_AVIAN_2B737F;
}

QColor PlatformStyle::Avian_34E2D6() const
{
    return COLOR_AVIAN_34E2D6;
}

QColor PlatformStyle::AssetTxColor() const
{
    if (darkModeEnabled)
        return COLOR_LIGHT_BLUE;
    return COLOR_DARK_BLUE;
}

const PlatformStyle *PlatformStyle::instantiate(const QString &platformId)
{
    for (const auto& platform_style : platform_styles) {
        if (platformId == platform_style.platformId) {
            return new PlatformStyle(
                    platform_style.platformId,
                    platform_style.imagesOnButtons,
                    platform_style.colorizeIcons,
                    platform_style.useExtraSpacing);
        }
    }
    return nullptr;
}

