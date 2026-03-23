#ifndef AVIAN_QT_VERTICALLABEL_H
#define AVIAN_QT_VERTICALLABEL_H

#include <QLabel>

class VerticalLabel : public QLabel
{
    Q_OBJECT

public:
    explicit VerticalLabel(QWidget *parent = nullptr);
    explicit VerticalLabel(const QString& text, QWidget *parent = nullptr);
    ~VerticalLabel();

protected:
    void paintEvent(QPaintEvent*) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
};

#endif // AVIAN_QT_VERTICALLABEL_H
