// Toasts: bottom-centered capsules, ink on canvas, at most four at once;
// 2.6 s by default, 5 s for errors, optionally with one action (guide 11.4).
#pragma once

#include <QPointer>
#include <QWidget>
#include <functional>

class Toaster : public QObject {
    Q_OBJECT
public:
    static void install(QWidget *host);
    static void show(const QString &text, bool error = false, const QString &action = {}, std::function<void()> onAction = {});

private:
    explicit Toaster(QWidget *host);
    bool eventFilter(QObject *o, QEvent *e) override;
    void layout();

    QPointer<QWidget> m_host;
    QList<QPointer<QWidget>> m_toasts;
    static Toaster *s_instance;
};
