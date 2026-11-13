#include "ui/widgets.h"

#include "ui/icons.h"
#include "ui/theme.h"

#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QStyleOption>

namespace {

// Hover is a short fade rather than a jump.
void setupHover(QVariantAnimation &a, QWidget *w) {
    a.setStartValue(0.0);
    a.setEndValue(1.0);
    a.setDuration(140);
    a.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&a, &QVariantAnimation::valueChanged, w, qOverload<>(&QWidget::update));
}

qreal hoverValue(const QVariantAnimation &a);

void hoverTo(QVariantAnimation &a, bool on) {
    if (Theme::reducedMotion()) {
        a.stop();
        a.setDirection(on ? QAbstractAnimation::Forward : QAbstractAnimation::Backward);
        a.setCurrentTime(on ? a.duration() : 0);
        return;
    }
    // Pick up from wherever the fade is now.
    const int at = a.state() == QAbstractAnimation::Running ? a.currentTime() : (hoverValue(a) > 0.5 ? a.duration() : 0);
    a.stop();
    a.setDirection(on ? QAbstractAnimation::Forward : QAbstractAnimation::Backward);
    a.start();
    a.setCurrentTime(at);
}

qreal hoverValue(const QVariantAnimation &a) {
    if (a.state() == QAbstractAnimation::Running) return a.currentValue().toReal();
    return a.direction() == QAbstractAnimation::Forward && a.currentTime() > 0 ? 1.0 : 0.0;
}

} // namespace

// --- IconButton ---

IconButton::IconButton(const QString &icon, const QString &tip, QWidget *parent) : QToolButton(parent), m_icon(icon) {
    setToolTip(tip);
    setAccessibleName(tip);
    setFixedSize(32, 32);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setupHover(m_hover, this);
    connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void IconButton::setIconName(const QString &icon) {
    m_icon = icon;
    update();
}

void IconButton::enterEvent(QEnterEvent *e) {
    hoverTo(m_hover, true);
    QToolButton::enterEvent(e);
}

void IconButton::leaveEvent(QEvent *e) {
    hoverTo(m_hover, false);
    QToolButton::leaveEvent(e);
}

void IconButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Tokens &k = Theme::t();
    const bool on = isDown() || isChecked();
    const qreal h = std::max(hoverValue(m_hover), hasFocus() ? 1.0 : 0.0);
    if (h > 0 || on) {
        const int a = on ? (Theme::dark() ? 30 : 22) : int((Theme::dark() ? 20 : 14) * h);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::dark() ? QColor(255, 255, 255, a) : QColor(0, 0, 0, a));
        p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), Theme::radius, Theme::radius);
    }
    QColor c = !isEnabled() ? k.faint : on ? k.ink : k.muted;
    if (isEnabled() && !on && h > 0) {
        // Blend muted toward ink with the hover.
        c = QColor::fromRgbF(c.redF() + (k.ink.redF() - c.redF()) * h, c.greenF() + (k.ink.greenF() - c.greenF()) * h,
                             c.blueF() + (k.ink.blueF() - c.blueF()) * h);
    }
    const int is = 18;
    p.drawPixmap((width() - is) / 2, (height() - is) / 2, icons::pixmap(m_icon, isChecked() ? k.ivy : c, is, devicePixelRatioF()));
}

// --- PillButton ---

PillButton::PillButton(const QString &text, Kind kind, QWidget *parent) : QToolButton(parent), m_kind(kind) {
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setFont(Theme::uiFont(13, QFont::Medium));
    setupHover(m_hover, this);
    connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

QSize PillButton::sizeHint() const {
    const QFontMetrics fm(font());
    return {fm.horizontalAdvance(text()) + 28, 30};
}

void PillButton::enterEvent(QEnterEvent *e) {
    hoverTo(m_hover, true);
    QToolButton::enterEvent(e);
}

void PillButton::leaveEvent(QEvent *e) {
    hoverTo(m_hover, false);
    QToolButton::leaveEvent(e);
}

void PillButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Tokens &k = Theme::t();
    const qreal h = hoverValue(m_hover);
    auto mix = [](const QColor &a, const QColor &b, qreal t) {
        return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                                a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
    };
    QColor bg, fg;
    QColor border = Qt::transparent;
    switch (m_kind) {
    case Primary: bg = mix(k.ivy, k.ivyHover, h); fg = k.onIvy; break;
    case Secondary: bg = mix(k.surface, k.hover, h); fg = k.ink; border = k.line.darker(Theme::dark() ? 70 : 108); break;
    case Ghost: bg = mix(Theme::alpha(k.sunken, 0), k.sunken, h); fg = k.ink2; break;
    case Danger: bg = mix(k.danger, k.danger.lighter(Theme::dark() ? 92 : 112), h); fg = k.canvas; break;
    }
    if (!isEnabled()) {
        bg = Theme::alpha(bg, bg.alphaF() * 0.45);
        fg = Theme::alpha(fg, 0.7);
    }
    if (isDown()) bg = bg.darker(Theme::dark() ? 90 : 106);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(border.alpha() ? QPen(border, 1) : Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, Theme::radius, Theme::radius);
    if (hasFocus()) {
        p.setPen(QPen(Theme::alpha(k.ivy, 0.55), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), Theme::radius - 1, Theme::radius - 1);
    }
    p.setPen(fg);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

// --- LeafWidget ---

LeafWidget::LeafWidget(int size, qreal opacity, QWidget *parent) : QWidget(parent), m_opacity(opacity) {
    setFixedSize(size, size);
    m_anim.setStartValue(0.0);
    m_anim.setEndValue(1.0);
    // Outline 1100 ms; veins from 760 ms, 620 ms each, 130 ms apart.
    m_anim.setDuration(760 + 4 * 130 + 620);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, qOverload<>(&QWidget::update));
    connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void LeafWidget::replay() {
    if (Theme::reducedMotion()) {
        m_anim.stop();
        m_anim.setCurrentTime(m_anim.duration());
        update();
        return;
    }
    m_anim.stop();
    m_anim.start();
}

void LeafWidget::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (!m_played) {
        m_played = true;
        replay();
    }
}

void LeafWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);
    p.scale(width() / 64.0, height() / 64.0);
    const qreal total = m_anim.duration();
    const qreal ms = m_anim.state() == QAbstractAnimation::Running ? m_anim.currentTime() : total;
    const QEasingCurve ease = Theme::easeOut();
    auto drawPart = [&](const QPainterPath &path, qreal width, qreal start, qreal dur) {
        const qreal t = std::clamp((ms - start) / dur, 0.0, 1.0);
        if (t <= 0) return;
        const qreal f = ease.valueForProgress(t);
        QPen pen(Theme::t().ivy, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        if (f < 1.0) {
            // Reveal the stroke along its length with a dash.
            const qreal len = path.length() / width;
            pen.setDashPattern({len * f, len});
        }
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    drawPart(icons::leafOutline(), 1.5, 0, 1100);
    const auto veins = icons::leafVeins();
    for (int i = 0; i < veins.size(); ++i) drawPart(veins[i], 1.0, 760 + i * 130, 620);
}

// --- Paper ---

Paper::Paper(QWidget *parent) : QFrame(parent) {
    connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void Paper::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Theme::t().surface);
}

// --- Divider ---

namespace {

class Hairline : public QSplitterHandle {
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        // The grab area is a few pixels wide; only the middle line shows.
        const Tokens &k = Theme::t();
        if (orientation() == Qt::Horizontal) {
            p.fillRect(rect(), k.canvas);
            p.fillRect(QRect(width() / 2, 0, 1, height()), k.line);
        } else {
            p.fillRect(rect(), k.surface);
            p.fillRect(QRect(0, height() / 2, width(), 1), k.line);
        }
    }
};

} // namespace

Divider::Divider(Qt::Orientation o, QWidget *parent) : QSplitter(o, parent) {
    setHandleWidth(1);
    connect(Theme::instance(), &Theme::changed, this, [this] {
        for (int i = 0; i < count(); ++i) handle(i)->update();
    });
}

QSplitterHandle *Divider::createHandle() { return new Hairline(orientation(), this); }

// --- helpers ---

void rise(QWidget *w, int ms) {
    if (Theme::reducedMotion() || !w) return;
    auto *fx = new QGraphicsOpacityEffect(w);
    w->setGraphicsEffect(fx);
    auto *a = new QPropertyAnimation(fx, "opacity", w);
    a->setDuration(ms);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setEasingCurve(Theme::easeOut());
    QPointer<QGraphicsOpacityEffect> guard(fx);
    QObject::connect(a, &QPropertyAnimation::finished, w, [w, guard] {
        if (guard && w->graphicsEffect() == guard) w->setGraphicsEffect(nullptr);
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void sink(QWidget *w, int ms, std::function<void()> done) {
    if (!w) return;
    if (!done) done = [w] { w->hide(); };
    if (Theme::reducedMotion() || !w->isVisible()) return done();
    auto *fx = new QGraphicsOpacityEffect(w);
    w->setGraphicsEffect(fx);
    auto *a = new QPropertyAnimation(fx, "opacity", w);
    a->setDuration(ms);
    a->setStartValue(1.0);
    a->setEndValue(0.0);
    a->setEasingCurve(QEasingCurve::InCubic);
    QPointer<QGraphicsOpacityEffect> guard(fx);
    QObject::connect(a, &QPropertyAnimation::finished, w, [w, guard, done] {
        done();
        if (guard && w->graphicsEffect() == guard) w->setGraphicsEffect(nullptr);
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void slide(QWidget *w, Qt::Orientation o, int from, int to, int ms, std::function<void()> done) {
    if (!w) return;
    const QByteArray prop = o == Qt::Horizontal ? "maximumWidth" : "maximumHeight";
    // A running slide on the same widget gives way to the new one.
    for (auto *old : w->findChildren<QPropertyAnimation *>(QString(), Qt::FindDirectChildrenOnly))
        if (old->propertyName() == prop && old->targetObject() == w) old->stop();
    if (Theme::reducedMotion()) {
        w->setProperty(prop, QWIDGETSIZE_MAX);
        if (done) done();
        return;
    }
    auto *a = new QPropertyAnimation(w, prop, w);
    a->setDuration(ms);
    a->setStartValue(from);
    a->setEndValue(to);
    a->setEasingCurve(Theme::easeOut());
    QObject::connect(a, &QPropertyAnimation::finished, w, [w, prop, done] {
        w->setProperty(prop, QWIDGETSIZE_MAX);
        if (done) done();
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

namespace {

class ShortcutPass : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() == QEvent::ShortcutOverride) {
            auto *k = static_cast<QKeyEvent *>(e);
            const auto mods = k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
            const int key = k->key();
            const bool ctrl = mods == Qt::ControlModifier;
            const bool ctrlShift = mods == (Qt::ControlModifier | Qt::ShiftModifier);
            if ((ctrl && (key == Qt::Key_F || key == Qt::Key_K || key == Qt::Key_N || key == Qt::Key_Backslash || key == Qt::Key_Comma || key == Qt::Key_R)) ||
                (ctrlShift && key == Qt::Key_P)) {
                e->ignore();
                return true;
            }
        }
        return QObject::eventFilter(o, e);
    }
};

} // namespace

void passAppShortcuts(QWidget *w) { w->installEventFilter(new ShortcutPass(w)); }

QLabel *mutedLabel(const QString &text, QWidget *parent, qreal px) {
    auto *l = new QLabel(text, parent);
    l->setFont(Theme::uiFont(px));
    auto apply = [l] {
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, Theme::t().muted);
        l->setPalette(pal);
    };
    apply();
    QObject::connect(Theme::instance(), &Theme::changed, l, apply);
    return l;
}
