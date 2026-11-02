#include "ui/toast.h"

#include "ui/theme.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>

Toaster *Toaster::s_instance = nullptr;

namespace {

class ToastWidget : public QWidget {
public:
    ToastWidget(const QString &text, bool error, const QString &action, std::function<void()> onAction, QWidget *parent)
        : QWidget(parent), m_error(error) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(error ? 30 : 18, 9, action.isEmpty() ? 18 : 8, 9);
        row->setSpacing(10);
        auto *label = new QLabel(text, this);
        label->setFont(Theme::uiFont(13));
        label->setMaximumWidth(520);
        label->setWordWrap(true);
        QPalette pal = label->palette();
        pal.setColor(QPalette::WindowText, Theme::t().canvas);
        label->setPalette(pal);
        row->addWidget(label);
        if (!action.isEmpty()) {
            auto *b = new QPushButton(action, this);
            b->setFlat(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setFont(Theme::uiFont(13, QFont::DemiBold));
            b->setStyleSheet(QStringLiteral("QPushButton{color:%1;border:none;padding:2px 10px;background:transparent;}")
                                 .arg(Theme::t().ivySoft.name()));
            connect(b, &QPushButton::clicked, this, [this, onAction] {
                if (onAction) onAction();
                close();
            });
            row->addWidget(b);
        }
        adjustSize();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, Theme::dark() ? 90 : 30));
        p.drawRoundedRect(r.adjusted(0, 2, 0, 1.5), Theme::radius, Theme::radius);
        p.setBrush(Theme::t().ink);
        p.drawRoundedRect(r, Theme::radius, Theme::radius);
        if (m_error) {
            p.setBrush(Theme::t().danger);
            p.drawEllipse(QPointF(16, r.center().y()), 3.5, 3.5);
        }
    }

private:
    bool m_error;
};

} // namespace

Toaster::Toaster(QWidget *host) : QObject(host), m_host(host) { host->installEventFilter(this); }

void Toaster::install(QWidget *host) {
    if (!s_instance) s_instance = new Toaster(host);
}

bool Toaster::eventFilter(QObject *o, QEvent *e) {
    if (o == m_host && e->type() == QEvent::Resize) layout();
    return false;
}

void Toaster::layout() {
    if (!m_host) return;
    m_toasts.removeAll(nullptr);
    int y = m_host->height() - 24;
    for (int i = int(m_toasts.size()) - 1; i >= 0; --i) {
        QWidget *t = m_toasts[i];
        y -= t->height();
        t->move((m_host->width() - t->width()) / 2, y);
        y -= 8;
    }
}

void Toaster::show(const QString &text, bool error, const QString &action, std::function<void()> onAction) {
    if (!s_instance || !s_instance->m_host) return;
    Toaster *self = s_instance;
    self->m_toasts.removeAll(nullptr);
    while (self->m_toasts.size() >= 4) {
        if (QWidget *old = self->m_toasts.takeFirst()) old->deleteLater();
    }
    auto *w = new ToastWidget(text, error, action, std::move(onAction), self->m_host);
    QPointer<QWidget> guard(w);
    self->m_toasts << guard;
    self->layout();
    w->show();
    w->raise();
    if (!Theme::reducedMotion()) {
        auto *fx = new QGraphicsOpacityEffect(w);
        w->setGraphicsEffect(fx);
        auto *a = new QPropertyAnimation(fx, "opacity", w);
        a->setDuration(220);
        a->setStartValue(0.0);
        a->setEndValue(1.0);
        a->setEasingCurve(Theme::easeOut());
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
    QTimer::singleShot(error ? 5000 : (action.isEmpty() ? 2600 : 5000), w, [w] {
        if (Theme::reducedMotion()) return w->deleteLater();
        auto *fx = new QGraphicsOpacityEffect(w);
        w->setGraphicsEffect(fx);
        auto *a = new QPropertyAnimation(fx, "opacity", w);
        a->setDuration(220);
        a->setStartValue(1.0);
        a->setEndValue(0.0);
        QObject::connect(a, &QPropertyAnimation::finished, w, &QObject::deleteLater);
        a->start(QAbstractAnimation::DeleteWhenStopped);
    });
    QObject::connect(w, &QObject::destroyed, self, [self] { QTimer::singleShot(0, self, [self] { self->layout(); }); });
}
