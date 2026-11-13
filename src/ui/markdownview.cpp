#include "ui/markdownview.h"

#include "ui/remoteimages.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include <QContextMenuEvent>
#include <QMenu>

#include <QFontMetricsF>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyle>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCursor>

// --- MarkdownView ---

MarkdownView::MarkdownView(QWidget *parent) : QTextBrowser(parent) {
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(28);
    document()->setIndentWidth(24);
    setFont(Theme::uiFont(m_px));
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(80);
    connect(&m_resizeTimer, &QTimer::timeout, this, [this] {
        if (m_hasMarkdown) setMarkdown(m_markdown, m_options, m_title);
    });
    restyle();
    connect(Theme::instance(), &Theme::changed, this, &MarkdownView::restyle);
    connect(RemoteImages::instance(), &RemoteImages::ready, this, [this](const QString &url) {
        const auto it = m_waiting.constFind(url);
        if (it == m_waiting.constEnd()) return;
        const QUrl name = *it;
        // Keep the URL registered so an explicit retry also updates failures.
        // Replace the placeholder the document cached, then lay out again.
        document()->addResource(QTextDocument::ImageResource, name, loadResource(QTextDocument::ImageResource, name));
        document()->markContentsDirty(0, document()->characterCount());
    });
}

void MarkdownView::setBaseSize(int px) {
    if (m_px == px) return;
    m_px = px;
    setFont(Theme::uiFont(px));
    restyle();
}

void MarkdownView::restyle() {
    // Sizes and spacing from the web client's .prose-ivy (app.css).
    const Tokens &k = Theme::t();
    const qreal px = m_px;
    const QString serif = Theme::serif(16).families().value(0, QStringLiteral("serif"));
    const QString mono = Theme::mono(13).families().value(0, QStringLiteral("monospace"));
    auto c = [](const QColor &x) { return x.name(QColor::HexArgb); };
    QString css = QStringLiteral(
        "body{color:%1;font-size:%17px;}"
        "p{margin-top:16px;margin-bottom:16px;}"
        "h1,h2,h3{font-family:'%2';font-weight:600;color:%1;}"
        "h1{font-size:%3px;margin-top:40px;margin-bottom:12px;}"
        "h2{font-size:%4px;margin-top:36px;margin-bottom:8px;}"
        "h3{font-size:%5px;margin-top:28px;margin-bottom:8px;}"
        "h4,h5,h6{font-size:%6px;font-weight:600;margin-top:24px;margin-bottom:6px;}"
        "ul,ol{margin-top:16px;margin-bottom:16px;}"
        "li{margin-top:6px;margin-bottom:6px;}"
        "a{color:%7;text-decoration:none;} a.missing{color:%8;}"
        "code{font-family:'%9';font-size:%10px;background-color:%11;}"
        "pre{font-family:'%9';font-size:%10px;margin:0px;}"
        "table.c{margin-top:20px;margin-bottom:20px;} td.c{background-color:%11;padding:16px 20px;}"
        "table.q{margin-top:20px;margin-bottom:20px;} td.qb{background-color:%12;} td.qt{padding-left:18px;color:%13;font-family:'%2';}"
        "table.t{margin-top:12px;margin-bottom:12px;} td.tb{padding-top:3px;padding-bottom:3px;}"
        "td.tm{padding-top:3px;padding-bottom:3px;color:%14;}"
        "td.ti{padding-top:3px;padding-bottom:3px;} td.td{padding-top:3px;padding-bottom:3px;color:%8;text-decoration:line-through;}"
        "table.g{margin-top:20px;margin-bottom:20px;font-size:%17px;border-collapse:collapse;}"
        "td.g,th.g{padding:8px 12px;border-bottom:1px solid %15;}"
        "th.g{color:%8;font-weight:500;}"
        "table.hr{margin-top:24px;margin-bottom:24px;} td.hr{border-bottom:1px solid %15;font-size:1px;}"
        ".mark{background-color:%16;} .file{color:%7;}");
    css = css.arg(k.ink.name(), serif, QString::number(qRound(px * 1.6)), QString::number(qRound(px * 1.3)), QString::number(qRound(px * 1.1)),
                  QString::number(px), k.ivy.name(), k.muted.name(), mono)
              .arg(QString::number(qRound(px * 0.85)), k.codeBg.name(), c(Theme::alpha(k.ivy, 0.35)), k.ink2.name(), k.faint.name(),
                   k.line.name(), c(Theme::alpha(k.warn, 0.22)), QString::number(m_px));
    document()->setDefaultStyleSheet(css);
    QPalette p = palette();
    p.setColor(QPalette::Base, k.surface);
    p.setColor(QPalette::Text, k.ink);
    setPalette(p);
    if (m_hasMarkdown) setMarkdown(m_markdown, m_options, m_title);
}

void MarkdownView::setMarkdown(const QString &markdown, RenderOptions opts, const QString &title) {
    m_markdown = markdown;
    m_options = opts;
    m_title = title;
    m_hasMarkdown = true;
    opts.forView = true;
    opts.remoteImages = true;
    QString html = renderMarkdown(markdown, opts);
    if (!title.isNull()) html = QStringLiteral("<h1>%1</h1>").arg(title.toHtmlEscaped()) + html;
    // The box images follow the theme; a new name per theme skips the cache.
    html.replace(QLatin1String("src=\"ivy-check:"), Theme::dark() ? QLatin1String("src=\"ivy-check:d") : QLatin1String("src=\"ivy-check:l"));
    const int scroll = verticalScrollBar()->value();
    m_waiting.clear();
    setHtml(html);
    // Line height as on the web: 1.85 em for text, 1.7 for code, snug
    // headings. Qt scales the font's own height (tall for CJK fonts), so the
    // factor is worked out per block. Lines holding an image stay natural.
    QTextCursor cur(document());
    cur.beginEditBlock();
    bool first = true;
    const QFont base = document()->defaultFont();
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        QTextBlockFormat bf;
        const QTextBlockFormat now = b.blockFormat();
        bool image = false;
        QFont font = b.charFormat().font().resolve(base);
        if (b.begin() != b.end()) font = b.begin().fragment().charFormat().font().resolve(base);
        for (auto it = b.begin(); !it.atEnd() && !image; ++it) image = it.fragment().charFormat().isImageFormat();
        const qreal size = font.pixelSize() > 0 ? font.pixelSize() : font.pointSizeF() * logicalDpiY() / 72.0;
        const qreal em = now.headingLevel() > 0 && now.headingLevel() <= 2 ? 1.375 : now.nonBreakableLines() ? 1.7 : 1.85;
        const qreal natural = QFontMetricsF(font).height();
        const int h = image || natural <= 0 ? 100 : std::max(100, int(std::round(100 * em * size / natural)));
        bf.setLineHeight(h, QTextBlockFormat::ProportionalHeight);
        if (first) bf.setTopMargin(0); // like .prose-ivy > :first-child
        first = false;
        cur.setPosition(b.position());
        cur.mergeBlockFormat(bf);
    }
    cur.endEditBlock();
    verticalScrollBar()->setValue(scroll);
}

int MarkdownView::maxImageWidth() const {
    // Leave room for the scroll bar a tall image is about to bring.
    int w = viewport()->width() - 2 * int(document()->documentMargin()) - 4;
    if (!verticalScrollBar()->isVisible()) w -= style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    return std::max(1, w);
}

void MarkdownView::contextMenuEvent(QContextMenuEvent *event) {
    QMenu *menu = createStandardContextMenu(event->pos());
    menu->addSeparator();
    menu->addAction(T("重新加载图片"), RemoteImages::instance(), &RemoteImages::retryFailed);
    menu->exec(event->globalPos());
    delete menu;
}

void MarkdownView::resizeEvent(QResizeEvent *event) {
    QTextBrowser::resizeEvent(event);
    if (event->size().width() != event->oldSize().width() && m_hasMarkdown) m_resizeTimer.start();
}

QVariant MarkdownView::loadResource(int type, const QUrl &name) {
    if (type != QTextDocument::ImageResource) return {};
    const qreal dpr = devicePixelRatioF();
    if (name.scheme() == QLatin1String("ivy-check")) {
        // The same box the live editor draws.
        const Tokens &k = Theme::t();
        const bool done = name.path().endsWith(u'1');
        const qreal s = 16;
        QImage img(QSize(18, 20) * dpr, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box(1, 4, s - 1, s - 1);
        p.setPen(done ? Qt::NoPen : QPen(k.faint, 1.4));
        p.setBrush(done ? k.ivy : Qt::transparent);
        p.drawRoundedRect(box, 3, 3);
        if (done) {
            QPainterPath tick;
            tick.moveTo(box.left() + s * 0.26, box.top() + s * 0.52);
            tick.lineTo(box.left() + s * 0.44, box.top() + s * 0.70);
            tick.lineTo(box.left() + s * 0.76, box.top() + s * 0.32);
            p.setPen(QPen(k.surface, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(tick);
        }
        return img;
    }
    if (name.scheme() == QLatin1String("ivy") && name.host() == QLatin1String("res")) {
        QImage img = image ? image(name.path().mid(1)) : QImage();
        if (img.isNull()) {
            img = QImage(320, 120, QImage::Format_ARGB32_Premultiplied);
            img.fill(Theme::t().sunken);
        }
        const int maxW = maxImageWidth();
        if (img.width() > maxW) img = img.scaledToWidth(maxW, Qt::SmoothTransformation);
        return img;
    }
    if (name.scheme() == QLatin1String("https") || name.scheme() == QLatin1String("http")) {
        // Fetched in the background; a placeholder holds the place until then.
        const QString url = name.toString();
        QImage img = RemoteImages::instance()->image(url);
        if (img.isNull()) {
            m_waiting.insert(url, name);
            img = QImage(320, 120, QImage::Format_ARGB32_Premultiplied);
            img.fill(Theme::t().sunken);
            QPainter painter(&img);
            painter.setPen(Theme::t().muted);
            painter.setFont(Theme::uiFont(13));
            painter.drawText(img.rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
                             RemoteImages::instance()->failed(url) ? T("图片加载失败，右键可重试") : T("图片加载中…"));
        }
        const int maxW = maxImageWidth();
        if (img.width() > maxW) img = img.scaledToWidth(maxW, Qt::SmoothTransformation);
        return img;
    }
    // Nothing else is fetched: no file access.
    return {};
}
