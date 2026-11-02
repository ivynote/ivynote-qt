// Colors, fonts and motion (guide 10). The native style draws the controls;
// these tokens give it Ivynote's warm neutrals and the ivy accent.
#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QObject>

struct Tokens {
    QColor canvas, surface, sunken, ink, ink2, muted, faint, line, hover;
    QColor ivy, ivyHover, ivySoft, ivyInk;
    QColor warn, warnInk, warnSoft, danger, dangerSoft, ok, codeBg;
    QColor onIvy; // text on an ivy background
};

class Theme : public QObject {
    Q_OBJECT
public:
    enum Mode { System, Light, Dark };

    static Theme *instance();
    static const Tokens &t() { return instance()->m_tokens; }
    static bool dark() { return instance()->m_dark; }

    void setMode(Mode m);
    Mode mode() const { return m_mode; }
    void apply();

    static QFont uiFont(qreal px, int weight = QFont::Normal);
    static QFont serif(qreal px, int weight = QFont::DemiBold);
    static QFont mono(qreal px);
    static QColor tagColor(const QString &name, const QString &color);
    static QColor alpha(const QColor &c, qreal a);
    static bool reducedMotion();
    static void setReducedMotion(bool on);
    static QEasingCurve easeOut();   // 1 - (1-t)^4
    static QEasingCurve spring();    // slight overshoot
    // One corner radius for everything drawn by hand. Native controls on
    // Breeze, Fusion and Windows sit between square and ~4 px; larger, softer
    // corners next to them look out of place.
    static constexpr qreal radius = 4;

signals:
    void changed();

private:
    Theme();
    bool systemDark() const;

    Mode m_mode = System;
    bool m_dark = false;
    Tokens m_tokens;
};
