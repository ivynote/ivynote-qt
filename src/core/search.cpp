#include "core/search.h"

#include "core/markdown.h"
#include "core/models.h"

#include <QRegularExpression>
#include <algorithm>

namespace {

int occurrences(const QString &s, const QString &t) {
    int n = 0;
    for (qsizetype i = s.indexOf(t); i >= 0; i = s.indexOf(t, i + t.size())) n++;
    return n;
}

void snippetFor(const QString &body, const QStringList &terms, SearchHit &hit) {
    const int width = 80;
    const QString plain = md::excerpt(body, -1);
    const QString lower = plain.toLower();
    qsizetype pos = -1;
    for (const QString &t : terms) {
        const qsizetype i = lower.indexOf(t);
        if (i >= 0 && (pos < 0 || i < pos)) pos = i;
    }
    const qsizetype start = pos < 0 ? 0 : std::max<qsizetype>(0, pos - 20);
    QString s = plain.mid(start, width);
    if (start > 0) s.prepend(QStringLiteral("…"));
    if (start + width < plain.size()) s.append(QStringLiteral("…"));
    const QString sl = s.toLower();
    for (const QString &t : terms)
        for (qsizetype i = sl.indexOf(t); i >= 0; i = sl.indexOf(t, i + t.size()))
            hit.marks.append({int(i), int(i + t.size())});
    std::sort(hit.marks.begin(), hit.marks.end());
    hit.snippet = s;
}

} // namespace

QList<SearchHit> searchNotes(const QList<const Note *> &notes, const QString &query, qint64 now, int limit) {
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList terms = query.toLower().split(ws, Qt::SkipEmptyParts);
    QList<SearchHit> hits;
    if (terms.isEmpty()) return hits;
    for (const Note *n : notes) {
        if (n->trashed() || n->deleted) continue;
        const QString title = n->title.toLower();
        const QString body = n->body.toLower();
        double score = 0;
        bool ok = true;
        for (const QString &t : terms) {
            const bool inTitle = title.contains(t);
            const bool inBody = body.contains(t);
            if (!inTitle && !inBody) {
                ok = false;
                break;
            }
            if (inTitle) score += title == t ? 12 : title.startsWith(t) ? 8 : 5;
            if (inBody) score += 1 + std::min(3.0, occurrences(body, t) / 2.0);
        }
        if (!ok) continue;
        score += std::max(0.0, 1.0 - double(now - n->updatedAt) / (365.0 * 86400000.0));
        SearchHit hit;
        hit.note = n;
        hit.score = score;
        snippetFor(n->body, terms, hit);
        hits << hit;
    }
    std::sort(hits.begin(), hits.end(), [](const SearchHit &a, const SearchHit &b) {
        if (a.score != b.score) return a.score > b.score;
        return a.note->updatedAt > b.note->updatedAt;
    });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}
