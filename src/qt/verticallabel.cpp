#include "verticallabel.h"

#include <QPainter>
#include <QPainterPath>

VerticalLabel::VerticalLabel(QWidget* parent)
    : QLabel(parent)
{
}

VerticalLabel::VerticalLabel(const QString& text, QWidget* parent)
    : QLabel(text, parent)
{
}

VerticalLabel::~VerticalLabel()
{
}

void VerticalLabel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setPen(Qt::white);
    painter.setBrush(Qt::Dense1Pattern);
    painter.translate(width() / 2, height());
    painter.rotate(270);

    painter.drawText(0, 0, text());
}

QSize VerticalLabel::minimumSizeHint() const
{
    QSize s = QLabel::minimumSizeHint();
    return QSize(s.height(), s.width());
}

QSize VerticalLabel::sizeHint() const
{
    QSize s = QLabel::sizeHint();
    return QSize(s.height(), s.width());
}