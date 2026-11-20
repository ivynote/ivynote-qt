#include "core/diff.h"

#include <QRegularExpression>
#include <algorithm>
#include <vector>

namespace diff {

namespace {

const QRegularExpression &conflictRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(^(.*?)\s*(?:[（(]\s*(?:冲突副本|conflict\s*copy)[^）)]*[）)])\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

void diffInline(const QString &oldStr, const QString &newStr, QList<Span> &oldSpans, QList<Span> &newSpans) {
    const qsizetype m = oldStr.size();
    const qsizetype n = newStr.size();
    qsizetype prefix = 0;
    while (prefix < m && prefix < n && oldStr[prefix] == newStr[prefix]) ++prefix;
    qsizetype suffix = 0;
    while (suffix < (m - prefix) && suffix < (n - prefix) && oldStr[m - 1 - suffix] == newStr[n - 1 - suffix]) ++suffix;
    if (prefix > 0) {
        oldSpans.append({Kind::Same, oldStr.left(prefix)});
        newSpans.append({Kind::Same, newStr.left(prefix)});
    }
    if (m - prefix - suffix > 0) oldSpans.append({Kind::Del, oldStr.mid(prefix, m - prefix - suffix)});
    if (n - prefix - suffix > 0) newSpans.append({Kind::Add, newStr.mid(prefix, n - prefix - suffix)});
    if (suffix > 0) {
        oldSpans.append({Kind::Same, oldStr.right(suffix)});
        newSpans.append({Kind::Same, newStr.right(suffix)});
    }
}

QStringList splitLines(QString s) {
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    s.replace(u'\r', u'\n');
    return s.split(u'\n');
}

} // namespace

bool isConflictNote(const QString &title) {
    return conflictRe().match(title.trimmed()).hasMatch();
}

QString conflictBaseTitle(const QString &title) {
    const auto m = conflictRe().match(title.trimmed());
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

QList<const Note *> findConflictCounterparts(const Note &current, const QHash<QString, Note> &all) {
    const QString curTitle = current.title.trimmed();
    QString base = conflictBaseTitle(curTitle);
    if (base.isEmpty()) base = curTitle;
    if (base.isEmpty()) return {};
    const QString baseLower = base.toLower();
    QList<const Note *> hits;
    for (const Note &n : all) {
        if (n.id == current.id || n.deleted || n.trashed()) continue;
        const QString t = n.title.trimmed();
        if (t.toLower() == baseLower || conflictBaseTitle(t).toLower() == baseLower) hits.append(&n);
    }
    std::sort(hits.begin(), hits.end(), [](const Note *a, const Note *b) { return a->updatedAt > b->updatedAt; });
    return hits;
}

Result compute(const QString &oldText, const QString &newText) {
    Result out;
    const QStringList a = splitLines(oldText);
    const QStringList b = splitLines(newText);

    qsizetype start = 0;
    while (start < a.size() && start < b.size() && a[start] == b[start]) ++start;
    qsizetype endA = a.size();
    qsizetype endB = b.size();
    while (endA > start && endB > start && a[endA - 1] == b[endB - 1]) {
        --endA;
        --endB;
    }

    QList<Line> ops;
    ops.reserve(a.size() + b.size());
    for (qsizetype i = 0; i < start; ++i)
        ops.append({Kind::Same, int(i + 1), int(i + 1), a[i], {}});

    const qsizetype na = endA - start;
    const qsizetype mb = endB - start;
    if (na > 0 || mb > 0) {
        if (na * mb <= 400000) {
            const qsizetype w = mb + 1;
            std::vector<int> dp(size_t((na + 1) * (mb + 1)), 0);
            for (qsizetype i = na - 1; i >= 0; --i) {
                for (qsizetype j = mb - 1; j >= 0; --j) {
                    if (a[start + i] == b[start + j]) dp[size_t(i * w + j)] = dp[size_t((i + 1) * w + (j + 1))] + 1;
                    else dp[size_t(i * w + j)] = std::max(dp[size_t((i + 1) * w + j)], dp[size_t(i * w + (j + 1))]);
                }
            }
            qsizetype i = 0, j = 0;
            while (i < na || j < mb) {
                if (i < na && j < mb && a[start + i] == b[start + j]) {
                    ops.append({Kind::Same, int(start + i + 1), int(start + j + 1), a[start + i], {}});
                    ++i;
                    ++j;
                } else if (i < na && (j >= mb || dp[size_t((i + 1) * w + j)] >= dp[size_t(i * w + (j + 1))])) {
                    ops.append({Kind::Del, int(start + i + 1), 0, a[start + i], {}});
                    ++i;
                } else {
                    ops.append({Kind::Add, 0, int(start + j + 1), b[start + j], {}});
                    ++j;
                }
            }
        } else {
            for (qsizetype i = 0; i < na; ++i) ops.append({Kind::Del, int(start + i + 1), 0, a[start + i], {}});
            for (qsizetype j = 0; j < mb; ++j) ops.append({Kind::Add, 0, int(start + j + 1), b[start + j], {}});
        }
    }

    for (qsizetype i = 0; i < a.size() - endA; ++i)
        ops.append({Kind::Same, int(endA + i + 1), int(endB + i + 1), a[endA + i], {}});

    qsizetype idx = 0;
    while (idx < ops.size()) {
        if (ops[idx].kind == Kind::Same) {
            out.rows.append({ops[idx], ops[idx]});
            out.unified.append(ops[idx]);
            ++out.unchanged;
            ++idx;
            continue;
        }
        QList<Line> dels, adds;
        while (idx < ops.size() && ops[idx].kind != Kind::Same) {
            if (ops[idx].kind == Kind::Del) {
                dels.append(ops[idx]);
                ++out.removed;
            } else {
                adds.append(ops[idx]);
                ++out.added;
            }
            ++idx;
        }
        const qsizetype pairs = std::max(dels.size(), adds.size());
        for (qsizetype p = 0; p < pairs; ++p) {
            std::optional<Line> l, r;
            if (p < dels.size()) l = dels[p];
            if (p < adds.size()) r = adds[p];
            if (l && r) {
                diffInline(l->text, r->text, l->spans, r->spans);
                dels[p].spans = l->spans;
                adds[p].spans = r->spans;
            }
            out.rows.append({l, r});
        }
        for (const Line &l : dels) out.unified.append(l);
        for (const Line &r : adds) out.unified.append(r);
    }

    out.identical = out.added == 0 && out.removed == 0 && oldText == newText;
    return out;
}

} // namespace diff
