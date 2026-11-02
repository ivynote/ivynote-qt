// The rendered note: the split preview and the history view.
#pragma once

#include "ui/render.h"

#include <QImage>
#include <QTextBrowser>
#include <functional>

// Rendered Markdown laid out like the web client's .prose-ivy: its spacing,
// line height, checkboxes, code blocks, quotes and tables, within what
// QTextBrowser can draw. Loads ivy://res images through `image`.
class MarkdownView : public QTextBrowser {
    Q_OBJECT
public:
    explicit MarkdownView(QWidget *parent = nullptr);
    void setBaseSize(int px);
    // Renders and shows the Markdown; keeps the scroll position.
    void setMarkdown(const QString &markdown, RenderOptions opts, const QString &title = QString());
    std::function<QImage(const QString &id)> image;

protected:
    QVariant loadResource(int type, const QUrl &name) override;

private:
    void restyle();
    int m_px = 16;
};
