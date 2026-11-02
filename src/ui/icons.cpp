#include "ui/icons.h"

#include "ui/theme.h"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>

namespace icons {

namespace {

const QHash<QString, QString> &paths() {
    static const QHash<QString, QString> p{
        {"note", R"(<path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"/><path d="M14 3v6h6M8 13h8M8 17h5"/>)"},
        {"file", R"(<path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"/><path d="M14 3v6h6"/>)"},
        {"folder", R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>)"},
        {"folder-fill", R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" fill="FILL"/>)"},
        {"hash", R"(<path d="M5 9h14M5 15h14M10 4 8 20M16 4l-2 16"/>)"},
        {"trash", R"(<path d="M4 7h16M10 11v6M14 11v6M5 7l1 12a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2l1-12M9 7V4h6v3"/>)"},
        {"search", R"(<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>)"},
        {"plus", R"(<path d="M12 5v14M5 12h14"/>)"},
        {"settings", R"(<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/>)"},
        {"disclosure", R"(<path d="M9.5 7v10l7.5-5z" fill="STROKE" stroke="none"/>)"},
        {"chevron-down", R"(<path d="m6 9 6 6 6-6"/>)"},
        {"chevron-left", R"(<path d="m15 6-6 6 6 6"/>)"},
        {"chevron-right", R"(<path d="m9 6 6 6-6 6"/>)"},
        {"more", R"(<circle cx="5" cy="12" r="1.2"/><circle cx="12" cy="12" r="1.2"/><circle cx="19" cy="12" r="1.2"/>)"},
        {"history", R"(<path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5M12 7v5l3 2"/>)"},
        {"share", R"(<path d="M4 12v7a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-7M16 6l-4-4-4 4M12 2v13"/>)"},
        {"link", R"(<path d="M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7"/><path d="M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7"/>)"},
        {"backlink", R"(<path d="M9 14 4 9l5-5"/><path d="M4 9h10.5a5.5 5.5 0 0 1 0 11H11"/>)"},
        {"sort", R"(<path d="M3 6h18M6 12h12M10 18h4"/>)"},
        {"check", R"(<path d="m5 12 5 5L20 7"/>)"},
        {"x", R"(<path d="M18 6 6 18M6 6l12 12"/>)"},
        {"cloud", R"(<path d="M17.5 19a4.5 4.5 0 1 0-1.3-8.8A6 6 0 0 0 4.5 12 4 4 0 0 0 6 19.8z"/>)"},
        {"cloud-off", R"(<path d="M2 2l20 20"/><path d="M5.8 5.8A6 6 0 0 0 4.5 12 4 4 0 0 0 6 19.8h11M20.7 16.5A4.5 4.5 0 0 0 16.2 10.2a6 6 0 0 0-6.7-4.6"/>)"},
        {"refresh", R"(<path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/>)"},
        {"copy", R"(<rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/>)"},
        {"download", R"(<path d="M12 3v12M7 10l5 5 5-5M5 21h14"/>)"},
        {"upload", R"(<path d="M12 21V9M7 14l5-5 5 5M5 3h14"/>)"},
        {"pin", R"(<path d="M12 17v5M9 10.8V5h6v5.8l2.4 3.2H6.6z"/><path d="M8 3h8"/>)"},
        {"move", R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="m11 13h6M14 10l3 3-3 3"/>)"},
        {"edit", R"(<path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>)"},
        {"image", R"(<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="m21 16-5-5-9 9"/>)"},
        {"todo", R"(<rect x="4" y="4" width="16" height="16" rx="3"/><path d="m8.5 12 2.5 2.5 4.5-5"/>)"},
        {"list", R"(<path d="M9 6h11M9 12h11M9 18h11"/><circle cx="4.5" cy="6" r="1"/><circle cx="4.5" cy="12" r="1"/><circle cx="4.5" cy="18" r="1"/>)"},
        {"heading", R"(<path d="M6 4v16M18 4v16M6 12h12"/>)"},
        {"bold", R"(<path d="M7 5h6a3.5 3.5 0 0 1 0 7H7zM7 12h7a3.5 3.5 0 0 1 0 7H7z"/>)"},
        {"italic", R"(<path d="M14 5h-4M14 19h-4M15 5 9 19"/>)"},
        {"code", R"(<path d="m16 18 6-6-6-6M8 6l-6 6 6 6"/>)"},
        {"quote", R"(<path d="M7 17c-2 0-3-1.5-3-3.5S5.5 9 9 7M17 17c-2 0-3-1.5-3-3.5S15.5 9 19 7"/>)"},
        {"video", R"(<rect x="3" y="5" width="13" height="14" rx="2"/><path d="m16 10 5-3v10l-5-3"/>)"},
        {"audio", R"(<path d="M9 18V5l11-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="17" cy="16" r="3"/>)"},
        {"paperclip", R"(<path d="m21 11-8.5 8.5a5 5 0 0 1-7-7L14 4a3.5 3.5 0 0 1 5 5l-8.5 8.5a2 2 0 0 1-3-3L15 7"/>)"},
        {"alert", R"(<path d="M12 9v4M12 17h.01"/><path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/>)"},
        {"info", R"(<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>)"},
        {"lock", R"(<rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>)"},
        {"globe", R"(<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>)"},
        {"eye", R"(<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>)"},
        {"sidebar", R"(<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M9 4v16"/>)"},
        {"user", R"(<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>)"},
        {"key", R"(<circle cx="8" cy="15" r="4"/><path d="m10.8 12.2 9.2-9.2M17 6l3 3M14 9l2 2"/>)"},
        {"database", R"(<ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v14c0 1.7 3.6 3 8 3s8-1.3 8-3V5M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3"/>)"},
        {"logout", R"(<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9"/>)"},
        {"sun", R"(<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>)"},
        {"moon", R"(<path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/>)"},
        {"monitor", R"(<rect x="2" y="4" width="20" height="13" rx="2"/><path d="M8 21h8M12 17v4"/>)"},
        {"restore", R"(<path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/>)"},
        {"tag", R"(<path d="M20.6 13.4 13.4 20.6a2 2 0 0 1-2.8 0L3 13V3h10l7.6 7.6a2 2 0 0 1 0 2.8z"/><circle cx="7.5" cy="7.5" r="1.5"/>)"},
        {"outline", R"(<path d="M4 6h16M8 12h12M12 18h8"/>)"},
        {"strike", R"(<path d="M4 12h16M16 6.5A4 4 0 0 0 12 5c-2.5 0-4 1.3-4 3.2 0 1.4 1 2.3 2.6 2.8M8 17.5A4.4 4.4 0 0 0 12 19c2.6 0 4.3-1.3 4.3-3.3"/>)"},
        {"vault", R"(<rect x="3" y="4" width="18" height="16" rx="3"/><path d="M8 4v16"/><path d="M12 9h5M12 13h5"/>)"},
        {"split", R"(<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M12 4v16"/>)"},
        {"zap", R"(<path d="M13 2 3 14h9l-1 8 10-12h-9z"/>)"},
        {"server", R"(<rect x="3" y="4" width="18" height="7" rx="2"/><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M7 7.5h.01M7 16.5h.01"/>)"},
    };
    return p;
}

QPainterPath parsePath(const QString &d) {
    // Enough of SVG path syntax for the leaf: absolute M, C, Z.
    QPainterPath path;
    static const QRegularExpression tok(QStringLiteral(R"([MCZmcz]|-?\d*\.?\d+)"));
    QList<QString> t;
    for (auto it = tok.globalMatch(d); it.hasNext();) t << it.next().captured(0);
    QChar cmd;
    int i = 0;
    auto num = [&] { return t.value(i++).toDouble(); };
    while (i < t.size()) {
        if (t[i].size() == 1 && t[i][0].isLetter()) cmd = t[i++][0].toUpper();
        if (cmd == u'M') {
            const double x = num(), y = num();
            path.moveTo(x, y);
            cmd = u'C';
            if (i < t.size() && t[i][0].isLetter()) continue;
        } else if (cmd == u'C') {
            const double a = num(), b = num(), c = num(), e = num(), x = num(), y = num();
            path.cubicTo(a, b, c, e, x, y);
        } else if (cmd == u'Z') {
            path.closeSubpath();
            cmd = QChar();
        } else {
            i++;
        }
    }
    return path;
}

} // namespace

QPixmap pixmap(const QString &name, const QColor &color, int size, qreal dpr) {
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(name, color.name(QColor::HexArgb)).arg(size).arg(dpr);
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    QString inner = paths().value(name);
    const QString stroke = color.name(QColor::HexRgb);
    inner.replace(QStringLiteral("STROKE"), stroke);
    QColor fill = color;
    fill.setAlphaF(0.16f);
    inner.replace(QStringLiteral("fill=\"FILL\""), QStringLiteral("fill=\"%1\" fill-opacity=\"0.16\"").arg(stroke));
    const QString svg = QStringLiteral(
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="%1" stroke-opacity="%2" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round">%3</svg>)")
                            .arg(stroke).arg(color.alphaF()).arg(inner);
    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();
    cache.insert(key, pm);
    return pm;
}

QIcon icon(const QString &name, const QColor &color) {
    QIcon i;
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    for (int s : {16, 20, 24, 32}) i.addPixmap(pixmap(name, color, s, dpr));
    return i;
}

QIcon themed(const QString &name) {
    const Tokens &k = Theme::t();
    QIcon i;
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    for (int s : {16, 18, 20, 24}) {
        i.addPixmap(pixmap(name, k.muted, s, dpr), QIcon::Normal);
        i.addPixmap(pixmap(name, k.ink, s, dpr), QIcon::Active);
        i.addPixmap(pixmap(name, k.faint, s, dpr), QIcon::Disabled);
        i.addPixmap(pixmap(name, k.ivy, s, dpr), QIcon::Selected);
    }
    return i;
}

QPainterPath leafOutline() {
    static const QPainterPath p = parsePath(QStringLiteral(
        "M14.93 3.2C23.29 1.42 30.04 5.51 34.13 13.51C38.4 9.78 43.56 7.29 47.47 7.29C51.02 7.29 54.4 9.07 57.24 10.93C54.93 14.4 51.91 16.53 48.71 17.07C53.33 19.73 57.6 23.11 59.2 26.31C60.8 29.69 59.91 34.13 58.49 37.16C54.93 37.87 52.8 36.27 51.02 35.29C49.42 34.4 47.82 33.87 46.4 34.31C44.27 39.82 42.49 45.87 40 50.67C37.33 55.64 32.36 59.38 28 62.58C24.53 57.96 20.98 53.16 19.56 47.82C18.13 42.49 17.78 36.27 17.24 30.76C14.58 29.51 11.2 30.04 8.89 30.67C6.76 31.29 4.8 30.93 4.09 29.51C6.04 25.24 8.36 20.44 11.2 17.42C13.51 14.93 17.24 13.69 20.8 12.8C18.49 10.13 19.11 8.36 18.67 6.93C18.13 5.33 16.71 3.91 14.93 3.2Z"));
    return p;
}

QList<QPainterPath> leafVeins() {
    static const QList<QPainterPath> v = {
        parsePath(QStringLiteral("M34.31 19.38C30.76 32 28.8 44.98 28.44 56.71")),
        parsePath(QStringLiteral("M34.31 19.38C30.4 14.4 27.02 9.96 23.73 8.27")),
        parsePath(QStringLiteral("M34.31 19.38C39.11 15.29 43.38 12.62 47.11 12.53")),
        parsePath(QStringLiteral("M34.31 19.38C25.6 18.13 16.89 21.33 9.42 27.02")),
        parsePath(QStringLiteral("M34.31 19.38C43.2 21.51 51.56 25.6 55.64 31.47")),
    };
    return v;
}

QIcon appIcon() { return QIcon(QStringLiteral(":/icons/app.svg")); }

} // namespace icons
