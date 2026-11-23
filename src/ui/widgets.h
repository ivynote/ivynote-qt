// Small shared widgets.
#pragma once

#include <QFrame>
#include <QLabel>
#include <QSplitter>
#include <QToolButton>
#include <QVariantAnimation>

#include <functional>

// A flat icon button (32x32) that follows the theme; its hover fades in.
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
    QVariantAnimation m_hover; // 0..1
};

// A compact button in Ivynote's style, cornered like the native ones.
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
    QVariantAnimation m_hover;
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

// The surface the note sits on: flat, edge to edge.
class Paper : public QFrame {
    Q_OBJECT
public:
    explicit Paper(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
};

// A splitter whose handle is a one-pixel hairline in the theme's line color
// (with a wider grab area), the same on every platform style.
class Divider : public QSplitter {
    Q_OBJECT
public:
    explicit Divider(Qt::Orientation o, QWidget *parent = nullptr);

protected:
    QSplitterHandle *createHandle() override;
};

// Fades a widget in (the "rise" motion).
void rise(QWidget *w, int ms = 320);
// Fades a widget out, then calls done (hides it by default).
void sink(QWidget *w, int ms = 160, std::function<void()> done = {});
// Grows or shrinks a widget's maximum width or height to a target, for panels
// that slide open and closed. done runs at the end (also without motion).
void slide(QWidget *w, Qt::Orientation o, int from, int to, int ms = 200, std::function<void()> done = {});

QLabel *mutedLabel(const QString &text, QWidget *parent = nullptr, qreal px = 12);

// Formats a portable shortcut string (e.g. "Ctrl+Shift+D") for the current OS
// ("Ctrl+Shift+D" on Linux/Windows, "⇧⌘D" on macOS).
QString shortcutText(const QString &portable);
// Find & Replace shortcut: Cmd+Option+F on macOS (Cmd+H hides the app), Ctrl+H elsewhere.
QString replaceShortcutPortable();

// Text fields claim some keys for editing (Ctrl+K deletes to the line end on
// X11); this lets the window's shortcuts win for the keys the app uses.
void passAppShortcuts(QWidget *w);
