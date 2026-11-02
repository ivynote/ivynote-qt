#include "ui/theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// Tokens from the web client's app.css: neutral whites and charcoals with
// ivy as the accent.
Tokens lightTokens() {
    return {QColor(247, 248, 249), QColor(255, 255, 255), QColor(241, 243, 245), QColor(28, 31, 34), QColor(70, 75, 80),
            QColor(112, 118, 124), QColor(160, 166, 172), QColor(230, 233, 236), QColor(234, 237, 240),
            QColor(40, 122, 82), QColor(31, 104, 69), QColor(229, 243, 235), QColor(27, 92, 60),
            QColor(190, 134, 48), QColor(110, 78, 26), QColor(252, 244, 228), QColor(176, 62, 54), QColor(252, 235, 233),
            QColor(74, 142, 104), QColor(244, 246, 248), QColor(255, 255, 255)};
}

Tokens darkTokens() {
    return {QColor(16, 18, 20), QColor(25, 27, 30), QColor(34, 37, 41), QColor(232, 235, 238), QColor(190, 195, 200),
            QColor(138, 144, 150), QColor(96, 101, 107), QColor(44, 48, 52), QColor(38, 41, 45),
            QColor(86, 180, 130), QColor(110, 196, 150), QColor(28, 50, 39), QColor(150, 224, 182),
            QColor(214, 170, 98), QColor(232, 205, 158), QColor(48, 40, 26), QColor(226, 128, 116), QColor(58, 33, 31),
            QColor(124, 190, 150), QColor(31, 34, 38), QColor(10, 28, 18)};
}

// Family stacks from the guide, narrowed to what the platform has.
QStringList available(const QStringList &wanted) {
    const QStringList have = QFontDatabase::families();
    QStringList out;
    for (const QString &f : wanted)
        if (have.contains(f, Qt::CaseInsensitive)) out << f;
    return out;
}

} // namespace

Theme *Theme::instance() {
    static Theme *t = new Theme;
    return t;
}

Theme::Theme() {
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_mode == System) apply();
    });
}

bool Theme::systemDark() const {
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) return true;
    if (scheme == Qt::ColorScheme::Light) return false;
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

void Theme::setMode(Mode m) {
    m_mode = m;
    apply();
}

void Theme::apply() {
    // Ask the platform style for the matching scheme, then lay our colors on.
    auto *hints = QGuiApplication::styleHints();
    if (m_mode == System) hints->unsetColorScheme();
    else hints->setColorScheme(m_mode == Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    m_dark = m_mode == Dark || (m_mode == System && systemDark());
    m_tokens = m_dark ? darkTokens() : lightTokens();
    const Tokens &k = m_tokens;

    QPalette p;
    p.setColor(QPalette::Window, k.canvas);
    p.setColor(QPalette::WindowText, k.ink);
    p.setColor(QPalette::Base, k.surface);
    p.setColor(QPalette::AlternateBase, k.sunken);
    p.setColor(QPalette::Text, k.ink);
    p.setColor(QPalette::PlaceholderText, k.faint);
    p.setColor(QPalette::Button, m_dark ? k.sunken : k.surface);
    p.setColor(QPalette::ButtonText, k.ink);
    p.setColor(QPalette::BrightText, k.surface);
    p.setColor(QPalette::Highlight, alpha(k.ivy, m_dark ? 0.35 : 0.22));
    p.setColor(QPalette::HighlightedText, k.ink);
    p.setColor(QPalette::Link, k.ivy);
    p.setColor(QPalette::LinkVisited, k.ivy);
    p.setColor(QPalette::ToolTipBase, k.ink);
    p.setColor(QPalette::ToolTipText, k.canvas);
    p.setColor(QPalette::Light, m_dark ? k.hover : QColor(Qt::white));
    p.setColor(QPalette::Midlight, k.hover);
    p.setColor(QPalette::Mid, k.line);
    p.setColor(QPalette::Dark, k.faint);
    p.setColor(QPalette::Shadow, QColor(0, 0, 0, m_dark ? 160 : 60));
    p.setColor(QPalette::Accent, k.ivy);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, k.faint);
    p.setColor(QPalette::Inactive, QPalette::Highlight, alpha(k.ivy, m_dark ? 0.25 : 0.15));
    QApplication::setPalette(p);
    emit changed();
}

QFont Theme::uiFont(qreal px, int weight) {
    QFont f = QApplication::font();
    f.setPixelSize(qRound(px));
    f.setWeight(QFont::Weight(weight));
    return f;
}

QFont Theme::serif(qreal px, int weight) {
    static const QStringList families = [] {
        QStringList l = available({"Iowan Old Style", "Charter", "Source Serif 4", "Source Serif Pro", "Noto Serif",
                                   "Noto Serif CJK SC", "Noto Serif SC", "Source Han Serif SC", "Songti SC", "STSong",
                                   "SimSun", "Georgia"});
        l << QStringLiteral("serif");
        return l;
    }();
    QFont f;
    f.setFamilies(families);
    f.setStyleHint(QFont::Serif);
    f.setPixelSize(qRound(px));
    f.setWeight(QFont::Weight(weight));
    return f;
}

QFont Theme::mono(qreal px) {
    static const QStringList families = [] {
        QStringList l = available({"SF Mono", "JetBrains Mono", "Cascadia Mono", "Cascadia Code", "Fira Code",
                                   "Source Code Pro", "Menlo", "Consolas", "DejaVu Sans Mono"});
        l << QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        return l;
    }();
    QFont f;
    f.setFamilies(families);
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    f.setPixelSize(qRound(px));
    return f;
}

QColor Theme::tagColor(const QString &name, const QString &color) {
    static const char *names[] = {"green", "blue", "amber", "pink", "purple", "gray"};
    static const char *light[] = {"#3F7A5B", "#4F6A9A", "#A0702C", "#A65A7C", "#7A62A8", "#6F767D"};
    static const char *darkc[] = {"#92C4A6", "#9DB2DC", "#D9B77E", "#DBA6BF", "#BCAAE0", "#A3A9AF"};
    if (color.startsWith(u'#') && color.size() == 7) return QColor(color);
    int idx = -1;
    for (int i = 0; i < 6; ++i)
        if (color == QLatin1String(names[i])) idx = i;
    if (idx < 0) {
        quint32 h = 0;
        for (char32_t c : name.toUcs4()) h = h * 31 + c;
        idx = int(h % 6);
    }
    return QColor(dark() ? darkc[idx] : light[idx]);
}

QColor Theme::alpha(const QColor &c, qreal a) {
    QColor x = c;
    x.setAlphaF(float(a));
    return x;
}

static bool g_reduceMotion = false;

void Theme::setReducedMotion(bool on) { g_reduceMotion = on; }

bool Theme::reducedMotion() {
    if (g_reduceMotion || qEnvironmentVariableIsSet("IVY_REDUCED_MOTION")) return true;
#ifdef Q_OS_WIN
    BOOL animations = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) && !animations) return true;
#endif
    // Qt mirrors the desktop's "disable animations" into its UI effects.
    return !QApplication::isEffectEnabled(Qt::UI_General);
}

QEasingCurve Theme::easeOut() {
    QEasingCurve c(QEasingCurve::Custom);
    c.setCustomType([](qreal t) { return 1 - std::pow(1 - t, 4); });
    return c;
}

QEasingCurve Theme::spring() {
    QEasingCurve c(QEasingCurve::OutBack);
    c.setOvershoot(0.9);
    return c;
}
