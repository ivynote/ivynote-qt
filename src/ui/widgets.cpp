#include "ui/widgets.h"

#include "ui/icons.h"
#include "ui/theme.h"

#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QStyleOption>

// --- IconButton ---

IconButton::IconButton(const QString &icon, const QString &tip, QWidget *parent) : QToolButton(parent), m_icon(icon) {
    setToolTip(tip);
    setAccessibleName(tip);
    setFixedSize(32, 32);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void IconButton::setIconName(const QString &icon) {
    m_icon = icon;
    update();
}

void IconButton::enterEvent(QEnterEvent *e) {
    m_hover = true;
    update();
    QToolButton::enterEvent(e);
}

void IconButton::leaveEvent(QEvent *e) {
    m_hover = false;
    update();
    QToolButton::leaveEvent(e);
}

void IconButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Tokens &k = Theme::t();
    const bool on = isDown() || isChecked();
    if (m_hover || on || hasFocus()) {
        QColor bg = Theme::dark() ? QColor(255, 255, 255, on ? 26 : 18) : QColor(0, 0, 0, on ? 20 : 13);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        // Pressed: shrink to 0.9 (guide 10.5).
        const qreal s = isDown() ? 0.9 : 1.0;
        QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        r = QRectF(r.center() - QPointF(r.width() * s / 2, r.height() * s / 2), r.size() * s);
        p.drawEllipse(r);
    }
    const QColor c = !isEnabled() ? k.faint : (m_hover || on) ? k.ink : k.muted;
    const int is = 18;
    p.drawPixmap((width() - is) / 2, (height() - is) / 2, icons::pixmap(m_icon, isChecked() ? k.ivy : c, is, devicePixelRatioF()));
}

// --- PillButton ---

PillButton::PillButton(const QString &text, Kind kind, QWidget *parent) : QToolButton(parent), m_kind(kind) {
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setFont(Theme::uiFont(13, QFont::Medium));
    connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

QSize PillButton::sizeHint() const {
    const QFontMetrics fm(font());
    return {fm.horizontalAdvance(text()) + 32, 36};
}

void PillButton::enterEvent(QEnterEvent *e) {
    m_hover = true;
    update();
    QToolButton::enterEvent(e);
}

void PillButton::leaveEvent(QEvent *e) {
    m_hover = false;
    update();
    QToolButton::leaveEvent(e);
}

void PillButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Tokens &k = Theme::t();
    QColor bg, fg;
    switch (m_kind) {
    case Primary: bg = m_hover ? k.ivyHover : k.ivy; fg = k.onIvy; break;
    case Secondary: bg = k.sunken; fg = k.ink; break;
    case Ghost: bg = m_hover ? k.sunken : QColor(Qt::transparent); fg = k.ink2; break;
    case Danger: bg = k.danger; fg = k.canvas; break;
    }
    if (!isEnabled()) {
        bg = Theme::alpha(bg, bg.alphaF() * 0.45);
        fg = Theme::alpha(fg, 0.7);
    } else if (m_hover && m_kind != Ghost && m_kind != Primary) {
        bg = m_kind == Secondary ? k.hover : bg.lighter(Theme::dark() ? 92 : 125);
    }
    const qreal s = isDown() ? 0.97 : 1.0;
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    r = QRectF(r.center() - QPointF(r.width() * s / 2, r.height() * s / 2), r.size() * s);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    if (hasFocus()) {
        p.setPen(QPen(Theme::alpha(k.ivy, 0.45), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(-1, -1, 1, 1), r.height() / 2 + 1, r.height() / 2 + 1);
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
    setAttribute(Qt::WA_TranslucentBackground, false);
    connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void Paper::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Tokens &k = Theme::t();
    p.fillRect(rect(), k.canvas);
    const QRectF r = QRectF(rect()).adjusted(6, 4, -6, -8);
    // A soft shadow from a few translucent layers (shadow-paper).
    const QColor shade = Theme::dark() ? QColor(0, 0, 0) : QColor(16, 24, 32);
    for (int i = 6; i >= 1; --i) {
        QColor c = shade;
        c.setAlphaF(float((Theme::dark() ? 0.07 : 0.012) * (7 - i)));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(r.adjusted(-i * 0.5 + 1, i * 0.6, i * 0.5 - 1, i * 1.2), 20 + i, 20 + i);
    }
    p.setPen(QPen(Theme::dark() ? QColor(255, 255, 255, 13) : QColor(16, 24, 32, 13), 0.8));
    p.setBrush(k.surface);
    p.drawRoundedRect(r, 20, 20);
}

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
    QObject::connect(a, &QPropertyAnimation::finished, w, [w] { w->setGraphicsEffect(nullptr); });
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
            if ((ctrl && (key == Qt::Key_K || key == Qt::Key_N || key == Qt::Key_Backslash || key == Qt::Key_Comma || key == Qt::Key_R)) ||
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
