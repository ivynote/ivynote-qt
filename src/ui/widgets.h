// Small shared widgets.
#pragma once

#include <QFrame>
#include <QLabel>
#include <QToolButton>
#include <QVariantAnimation>

// A flat, round icon button (32x32) that follows the theme.
class IconButton : public QToolButton {
    Q_OBJECT
public:
    IconButton(const QString &icon, const QString &tip, QWidget *parent = nullptr);
    void setIconName(const QString &icon);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QString m_icon;
    bool m_hover = false;
};

// Pill-shaped primary/secondary button in Ivynote's style.
class PillButton : public QToolButton {
    Q_OBJECT
public:
    enum Kind { Primary, Secondary, Ghost, Danger };
    PillButton(const QString &text, Kind kind = Secondary, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    Kind m_kind;
    bool m_hover = false;
};

// The ivy leaf, drawn stroke by stroke on first show (guide 10.7).
class LeafWidget : public QWidget {
    Q_OBJECT
public:
    LeafWidget(int size, qreal opacity = 1.0, QWidget *parent = nullptr);
    void replay();

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;

private:
    QVariantAnimation m_anim;
    qreal m_opacity;
    bool m_played = false;
};

// The paper the note sits on: rounded surface with a soft shadow.
class Paper : public QFrame {
    Q_OBJECT
public:
    explicit Paper(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
};

// Fades a widget in and lifts it a few pixels (the "rise" motion).
void rise(QWidget *w, int ms = 320);

QLabel *mutedLabel(const QString &text, QWidget *parent = nullptr, qreal px = 12);

// Text fields claim some keys for editing (Ctrl+K deletes to the line end on
// X11); this lets the window's shortcuts win for the keys the app uses.
void passAppShortcuts(QWidget *w);
