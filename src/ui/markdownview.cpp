#include "ui/markdownview.h"

#include "ui/remoteimages.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QRegularExpression>
#include <QTextDocumentFragment>
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
#include <QTextTable>
#include <QLocale>
#include <cmath>

namespace {
QImage viewSunkenFill() {
    static QColor cachedColor;
    static QImage cached;
    const QColor c = Theme::t().sunken;
    if (cached.isNull() || cachedColor != c) {
        cachedColor = c;
        cached = QImage(16, 16, QImage::Format_ARGB32_Premultiplied);
        cached.fill(c);
    }
    return cached;
}

QImage viewDefaultPlaceholder() {
    static QColor cachedColor;
    static QImage cached;
    const QColor c = Theme::t().sunken;
    if (cached.isNull() || cachedColor != c) {
        cachedColor = c;
        cached = QImage(320, 120, QImage::Format_ARGB32_Premultiplied);
        cached.fill(c);
    }
    return cached;
}
} // namespace

// --- MarkdownView ---

MarkdownView::MarkdownView(QWidget *parent) : QTextBrowser(parent) {
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(28);
    document()->setIndentWidth(24);
    setFont(Theme::uiFont(m_px));
    m_scaledCache.setMaxCost(64 * 1024);
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(80);
    connect(&m_resizeTimer, &QTimer::timeout, this, [this] {
        if (m_hasMarkdown) setMarkdown(m_markdown, m_options, m_title);
    });
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(16);
    connect(&m_refreshTimer, &QTimer::timeout, this, &MarkdownView::flushPendingAttachments);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &MarkdownView::loadVisibleImages);
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

void MarkdownView::setColumnWidth(int maxWidth) {
    if (m_maxColumn == maxWidth) return;
    m_maxColumn = maxWidth;
    document()->setDocumentMargin(m_maxColumn > 0 ? 2 : 28);
    updateMargins();
    if (m_hasMarkdown) setMarkdown(m_markdown, m_options, m_title);
}

void MarkdownView::updateMargins() {
    if (m_maxColumn > 0) {
        const int side = std::max(24, (width() - m_maxColumn) / 2);
        setViewportMargins(side, 0, side, 0);
    } else {
        setViewportMargins(0, 0, 0, 0);
    }
    emit columnChanged();
}

QRect MarkdownView::textColumn() const {
    if (m_maxColumn > 0) {
        const int side = viewportMargins().left();
        return {side, 0, std::max(0, width() - 2 * side), height()};
    }
    const int margin = int(document()->documentMargin());
    return {margin, 0, std::max(0, width() - 2 * margin), height()};
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
    css += QStringLiteral(
        ".task-due{font-size:%1px;color:%2;background-color:%3;text-decoration:none;}"
        ".task-due-today{font-size:%1px;color:%4;background-color:%5;text-decoration:none;}"
        ".task-due-overdue{font-size:%1px;color:%6;background-color:%7;text-decoration:none;}"
        ".task-repeat{font-size:%1px;color:%4;background-color:%8;text-decoration:none;}"
        ".task-done{font-size:%1px;color:%9;text-decoration:none;}")
               .arg(QString::number(std::max(12, m_px - 2)), k.muted.name(), c(Theme::alpha(k.line, 0.55)), k.ivy.name(),
                    c(Theme::alpha(k.ivy, 0.14)), k.danger.name(), c(Theme::alpha(k.danger, 0.12)), c(Theme::alpha(k.ivy, 0.10)), k.faint.name());
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
    m_refreshTimer.stop();
    m_pendingAttachments.clear();
    m_waiting.clear();
    m_waitingAttachments.clear();
    m_sizedPlaceholders.clear();
    if (cachedImage && imageMeta && html.contains(QLatin1String("src=\"ivy://res/"))) {
        static const QRegularExpression resImg(QStringLiteral(R"RE((<img src="ivy://res/([A-Za-z0-9_-]{8,64})"[^>]*>))RE"));
        const int maxW = maxImageWidth();
        QString out;
        qsizetype last = 0;
        for (auto it = resImg.globalMatch(html); it.hasNext();) {
            const auto m = it.next();
            out += html.mid(last, m.capturedStart() - last);
            QString tag = m.captured(1);
            const QString id = m.captured(2);
            if (cachedImage(id).isNull()) {
                QSize sz = imageMeta(id);
                if (sz.isValid() && !sz.isEmpty()) {
                    if (sz.width() > maxW) sz = sz.scaled(maxW, 100000, Qt::KeepAspectRatio);
                    m_sizedPlaceholders.insert(id);
                    tag.chop(1);
                    tag += QStringLiteral(" width=\"%1\" height=\"%2\">").arg(sz.width()).arg(sz.height());
                }
            }
            out += tag;
            last = m.capturedEnd();
        }
        if (last > 0) {
            out += html.mid(last);
            html = std::move(out);
        }
    }
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
    if (!m_waitingAttachments.isEmpty() || !m_waiting.isEmpty())
        QTimer::singleShot(0, this, &MarkdownView::loadVisibleImages);
}

void MarkdownView::clearImageCache() {
    m_scaledCache.clear();
    m_pendingAttachments.clear();
    m_refreshTimer.stop();
}

void MarkdownView::refreshAttachment(const QString &id) {
    if (id.isEmpty() || !m_waitingAttachments.contains(id)) return;
    m_pendingAttachments.insert(id);
    if (!m_refreshTimer.isActive()) m_refreshTimer.start();
}

QImage MarkdownView::cachedImageFor(const QString &id) const {
    if (cachedImage) return cachedImage(id);
    return image ? image(id) : QImage();
}

QImage MarkdownView::scaledImageFor(const QString &id, int maxWidth) const {
    const int targetW = std::max(1, maxWidth);
    const QString key = id + u'@' + QString::number(targetW);
    if (const QImage *cached = m_scaledCache.object(key)) return *cached;
    QImage img = cachedImageFor(id);
    if (img.isNull()) return {};
    if (img.width() > targetW) img = img.scaledToWidth(targetW, Qt::SmoothTransformation);
    m_scaledCache.insert(key, new QImage(img), std::max<qsizetype>(1, img.sizeInBytes() / 1024));
    return img;
}

void MarkdownView::flushPendingAttachments() {
    if (m_pendingAttachments.isEmpty()) return;
    const QSet<QString> ids = std::move(m_pendingAttachments);
    m_pendingAttachments.clear();
    const int maxW = maxImageWidth();
    bool updated = false;
    bool needRelayout = false;
    for (const QString &id : ids) {
        const auto it = m_waitingAttachments.constFind(id);
        if (it == m_waitingAttachments.constEnd()) continue;
        const QImage img = scaledImageFor(id, maxW);
        if (img.isNull()) continue;
        document()->addResource(QTextDocument::ImageResource, *it, img);
        m_waitingAttachments.remove(id);
        if (!m_sizedPlaceholders.remove(id)) needRelayout = true;
        updated = true;
    }
    if (needRelayout) document()->markContentsDirty(0, document()->characterCount());
    else if (updated) viewport()->update();
}

void MarkdownView::loadVisibleImages() {
    if (m_waitingAttachments.isEmpty() && m_waiting.isEmpty()) return;
    QAbstractTextDocumentLayout *layout = document()->documentLayout();
    const QPointF off(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    const QRectF visible = QRectF(viewport()->rect()).translated(-off);
    const qreal vh = std::max<qreal>(200.0, viewport()->height());
    const qreal topBound = visible.top() - vh * 0.5;
    const qreal bottomBound = visible.bottom() + vh;

    QTextBlock first = cursorForPosition(QPoint(0, 0)).block();
    while (first.isValid() && first.previous().isValid() && layout->blockBoundingRect(first).top() > topBound)
        first = first.previous();

    int budget = 12;
    for (QTextBlock b = first; b.isValid() && budget > 0; b = b.next()) {
        const QRectF br = layout->blockBoundingRect(b);
        if (br.top() > bottomBound) break;
        if (br.bottom() < topBound) continue;
        for (auto it = b.begin(); !it.atEnd() && budget > 0; ++it) {
            const QTextCharFormat cf = it.fragment().charFormat();
            if (!cf.isImageFormat()) continue;
            const QString name = cf.toImageFormat().name();
            if (name.startsWith(QLatin1String("ivy://res/"))) {
                const QString id = name.mid(10);
                if (m_waitingAttachments.contains(id) && image) {
                    const QImage img = image(id);
                    if (!img.isNull()) refreshAttachment(id);
                    --budget;
                }
            } else if (m_waiting.contains(name)) {
                RemoteImages::instance()->image(name);
                --budget;
            }
        }
    }
}

void MarkdownView::paintEvent(QPaintEvent *event) {
    loadVisibleImages();
    QTextBrowser::paintEvent(event);
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
    auto *copyRich = menu->addAction(T("复制为富文本"), this, [this] {
        const auto selection = textCursor().selection();
        auto *mime = new QMimeData;
        mime->setHtml(selection.toHtml());
        mime->setText(selection.toPlainText());
        QApplication::clipboard()->setMimeData(mime);
    });
    copyRich->setEnabled(textCursor().hasSelection());
    const auto cursor = textCursor();
    if (const auto *table = cursor.currentTable(); table && cursor.hasComplexSelection()) {
        int row, rows, column, columns;
        cursor.selectedTableCells(&row, &rows, &column, &columns);
        long double sum = 0;
        int count = 0;
        for (int r = row; r < row + rows; ++r) {
            for (int c = column; c < column + columns; ++c) {
                const auto cell = table->cellAt(r, c);
                if (!cell.isValid() || cell.row() != r || cell.column() != c) continue;
                auto content = cell.firstCursorPosition();
                content.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                bool ok = false;
                const double value = content.selectedText().trimmed().toDouble(&ok);
                if (ok && std::isfinite(value)) {
                    sum += value;
                    ++count;
                }
            }
        }
        if (count && std::isfinite(double(sum))) {
            const QLocale locale;
            const QString total = locale.toString(double(sum), 'g', 15);
            const QString average = locale.toString(double(sum / count), 'g', 15);
            menu->addAction(T("复制求和结果（%1）").arg(total), this, [total] { QApplication::clipboard()->setText(total); });
            menu->addAction(T("复制平均值（%1）").arg(average), this, [average] { QApplication::clipboard()->setText(average); });
        }
    }
    menu->addAction(T("重新加载图片"), RemoteImages::instance(), &RemoteImages::retryFailed);
    menu->exec(event->globalPos());
    delete menu;
}

void MarkdownView::resizeEvent(QResizeEvent *event) {
    QTextBrowser::resizeEvent(event);
    if (m_maxColumn > 0) updateMargins();
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
        const QString id = name.path().mid(1);
        const int maxW = maxImageWidth();
        const QImage img = scaledImageFor(id, maxW);
        if (!img.isNull()) return img;
        m_waitingAttachments.insert(id, name);
        if (m_sizedPlaceholders.contains(id)) return viewSunkenFill();
        QImage ph = viewDefaultPlaceholder();
        if (ph.width() > maxW) ph = ph.scaledToWidth(maxW, Qt::SmoothTransformation);
        return ph;
    }
    if (name.scheme() == QLatin1String("https") || name.scheme() == QLatin1String("http")) {
        // Fetched in the background; a placeholder holds the place until then.
        const QString url = name.toString();
        QImage img = RemoteImages::instance()->cachedImage(url);
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
