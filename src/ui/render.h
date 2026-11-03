// Markdown to HTML for the preview pane and history (guide 9). Raw HTML in
// notes is never rendered and only safe link schemes survive: note content
// is untrusted input. The output targets QTextBrowser's HTML subset.
#pragma once

#include <QString>
#include <functional>

struct RenderOptions {
    std::function<QString(const QString &title)> resolveTitle; // note id or ""
    std::function<QString(const QString &id)> resourceKind;    // image/video/audio/file or ""
    std::function<QString(const QString &id)> resourceName;
    bool interactiveTasks = false;
    // Markup for MarkdownView: task lists as checkbox rows, padded code
    // blocks, the quote bar. Plain HTML (for the clipboard) leaves it off.
    bool forView = false;
    // Keep https images as images (MarkdownView fetches them); otherwise
    // they become links, so pasted HTML loads nothing on its own.
    bool remoteImages = false;
};

QString renderMarkdown(const QString &markdown, const RenderOptions &opts = {});

// Internal link schemes used in rendered HTML.
//   ivy-wiki:<percent-encoded target[#heading]>
//   ivy://note/<id>, ivy://res/<id>
//   ivy-task:<n>
bool isSafeExternalUrl(const QString &url);

