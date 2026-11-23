// The rendered note: the split preview and the history view.
#pragma once

#include "ui/render.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QUrl>
#include <QTimer>
#include <QTextBrowser>
#include <functional>

// Rendered Markdown laid out like the web client's .prose-ivy: its spacing,
// line height, checkboxes, code blocks, quotes and tables, within what
// QTextBrowser can draw. Loads ivy://res images through `image`,
// https images through RemoteImages.
class MarkdownView : public QTextBrowser {
    Q_OBJECT
public:
    explicit MarkdownView(QWidget *parent = nullptr);
    void setBaseSize(int px);
    void setColumnWidth(int maxWidth); // 0 = fill viewport (split/history); >0 = centered column (read mode)
    QRect textColumn() const;
    // Renders and shows the Markdown; keeps the scroll position.
    void setMarkdown(const QString &markdown, RenderOptions opts, const QString &title = QString());
    void refreshAttachment(const QString &id);
    void clearImageCache();
    std::function<QImage(const QString &id)> image;
    std::function<QImage(const QString &id)> cachedImage; // non-triggering; falls back to `image`
    std::function<QSize(const QString &id)> imageMeta;    // natural size or invalid

signals:
    void columnChanged();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    QVariant loadResource(int type, const QUrl &name) override;

private:
    void restyle();
    void updateMargins();
    int maxImageWidth() const;
    QImage cachedImageFor(const QString &id) const;
    QImage scaledImageFor(const QString &id, int maxWidth) const;
    void loadVisibleImages();
    void flushPendingAttachments();
    int m_px = 16;
    int m_maxColumn = 0;
    bool m_hasMarkdown = false;
    QString m_markdown, m_title;
    RenderOptions m_options;
    QTimer m_resizeTimer;
    QTimer m_refreshTimer;
    QHash<QString, QUrl> m_waiting;            // remote images shown as placeholders
    QHash<QString, QUrl> m_waitingAttachments; // attachment images shown as placeholders
    QSet<QString> m_sizedPlaceholders;         // attachment ids whose <img> tag already reserves exact width/height
    QSet<QString> m_pendingAttachments;
    mutable QCache<QString, QImage> m_scaledCache;
};
