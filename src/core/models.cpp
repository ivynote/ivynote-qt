#include "core/models.h"

#include <QJsonArray>

namespace {

qint64 num(const QJsonObject &o, const char *k) { return qint64(o.value(QLatin1String(k)).toDouble()); }
QString str(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)).toString(); }
bool flag(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)).toBool(); }

StampMap stamps(const QJsonObject &o) {
    StampMap m;
    const QJsonObject h = o.value(QLatin1String("hlc")).toObject();
    for (auto it = h.begin(); it != h.end(); ++it) m.insert(it.key(), it.value().toString());
    return m;
}

void putStamps(QJsonObject &o, const StampMap &m) {
    if (m.isEmpty()) return;
    QJsonObject h;
    for (auto it = m.begin(); it != m.end(); ++it) h.insert(it.key(), it.value());
    o.insert(QStringLiteral("hlc"), h);
}

} // namespace

QJsonObject Note::toJson() const {
    QJsonObject o{{"id", id}, {"folderId", folderId}, {"title", title}, {"body", body},
                  {"tags", QJsonArray::fromStringList(tags)}, {"pinned", pinned},
                  {"rev", double(rev)}, {"seq", double(seq)}, {"trashedAt", double(trashedAt)},
                  {"deleted", deleted}, {"words", words}, {"createdAt", double(createdAt)},
                  {"updatedAt", double(updatedAt)}};
    putStamps(o, hlc);
    return o;
}

Note Note::fromJson(const QJsonObject &o) {
    Note n;
    n.id = str(o, "id");
    n.folderId = str(o, "folderId");
    n.title = str(o, "title");
    n.body = str(o, "body");
    for (const auto &v : o.value(QLatin1String("tags")).toArray()) n.tags << v.toString();
    n.pinned = flag(o, "pinned");
    n.rev = num(o, "rev");
    n.seq = num(o, "seq");
    n.trashedAt = num(o, "trashedAt");
    n.deleted = flag(o, "deleted");
    n.words = int(num(o, "words"));
    n.hlc = stamps(o);
    n.createdAt = num(o, "createdAt");
    n.updatedAt = num(o, "updatedAt");
    return n;
}

QJsonObject Folder::toJson() const {
    QJsonObject o{{"id", id}, {"parentId", parentId}, {"name", name}, {"sort", sort},
                  {"seq", double(seq)}, {"deleted", deleted}, {"createdAt", double(createdAt)},
                  {"updatedAt", double(updatedAt)}};
    putStamps(o, hlc);
    return o;
}

Folder Folder::fromJson(const QJsonObject &o) {
    Folder f;
    f.id = str(o, "id");
    f.parentId = str(o, "parentId");
    f.name = str(o, "name");
    f.sort = o.value(QLatin1String("sort")).toDouble();
    f.seq = num(o, "seq");
    f.deleted = flag(o, "deleted");
    f.hlc = stamps(o);
    f.createdAt = num(o, "createdAt");
    f.updatedAt = num(o, "updatedAt");
    return f;
}

QJsonObject Tag::toJson() const {
    QJsonObject o{{"id", id}, {"name", name}, {"color", color}, {"seq", double(seq)},
                  {"deleted", deleted}, {"createdAt", double(createdAt)}, {"updatedAt", double(updatedAt)}};
    putStamps(o, hlc);
    return o;
}

Tag Tag::fromJson(const QJsonObject &o) {
    Tag t;
    t.id = str(o, "id");
    t.name = str(o, "name");
    t.color = str(o, "color");
    t.seq = num(o, "seq");
    t.deleted = flag(o, "deleted");
    t.hlc = stamps(o);
    t.createdAt = num(o, "createdAt");
    t.updatedAt = num(o, "updatedAt");
    return t;
}

QJsonObject Attachment::toJson() const {
    return QJsonObject{{"id", id}, {"name", name}, {"mime", mime}, {"kind", kind},
                       {"size", double(size)}, {"width", width}, {"height", height},
                       {"sha256", sha256}, {"storage", storage}, {"hasThumb", hasThumb},
                       {"refs", refs}, {"orphanSince", double(orphanSince)},
                       {"trashedAt", double(trashedAt)}, {"trashReason", trashReason},
                       {"public", isPublic}, {"publicUrl", publicUrl}, {"seq", double(seq)},
                       {"deleted", deleted}, {"createdAt", double(createdAt)},
                       {"updatedAt", double(updatedAt)}};
}

Attachment Attachment::fromJson(const QJsonObject &o) {
    Attachment a;
    a.id = str(o, "id");
    a.name = str(o, "name");
    a.mime = str(o, "mime");
    a.kind = str(o, "kind");
    if (a.kind.isEmpty()) a.kind = kindOfMime(a.mime);
    a.size = num(o, "size");
    a.width = int(num(o, "width"));
    a.height = int(num(o, "height"));
    a.sha256 = str(o, "sha256");
    a.storage = str(o, "storage");
    a.hasThumb = flag(o, "hasThumb");
    a.refs = int(num(o, "refs"));
    a.orphanSince = num(o, "orphanSince");
    a.trashedAt = num(o, "trashedAt");
    a.trashReason = str(o, "trashReason");
    a.isPublic = flag(o, "public");
    a.publicUrl = str(o, "publicUrl");
    a.seq = num(o, "seq");
    a.deleted = flag(o, "deleted");
    a.createdAt = num(o, "createdAt");
    a.updatedAt = num(o, "updatedAt");
    return a;
}

QString typeName(EntityType t) {
    switch (t) {
    case EntityType::Note: return QStringLiteral("note");
    case EntityType::Folder: return QStringLiteral("folder");
    case EntityType::Tag: return QStringLiteral("tag");
    }
    return {};
}

bool parseType(const QString &s, EntityType *out) {
    if (s == QLatin1String("note")) *out = EntityType::Note;
    else if (s == QLatin1String("folder")) *out = EntityType::Folder;
    else if (s == QLatin1String("tag")) *out = EntityType::Tag;
    else return false;
    return true;
}

QJsonObject OutboxEntry::toJson() const {
    QJsonObject o{{"key", key}, {"type", typeName(type)}, {"id", id},
                  {"op", isDelete ? "delete" : "upsert"}, {"fields", fields}, {"hlc", hlc},
                  {"version", version}, {"device", device}, {"queuedAt", queuedAt}};
    if (hasBaseRev) o.insert(QStringLiteral("baseRev"), double(baseRev));
    return o;
}

OutboxEntry OutboxEntry::fromJson(const QJsonObject &o) {
    OutboxEntry e;
    parseType(str(o, "type"), &e.type);
    e.id = str(o, "id");
    e.key = str(o, "key");
    if (e.key.isEmpty()) e.key = typeName(e.type) + u':' + e.id;
    e.isDelete = str(o, "op") == QLatin1String("delete");
    e.fields = o.value(QLatin1String("fields")).toObject();
    e.hlc = str(o, "hlc");
    e.queuedAt = str(o, "queuedAt");
    if (e.queuedAt.isEmpty()) e.queuedAt = e.hlc;
    e.hasBaseRev = o.contains(QLatin1String("baseRev"));
    e.baseRev = num(o, "baseRev");
    e.version = int(num(o, "version"));
    e.device = str(o, "device");
    return e;
}

QJsonObject OutboxEntry::toChange() const {
    QJsonObject c{{"type", typeName(type)}, {"id", id}, {"op", isDelete ? "delete" : "upsert"},
                  {"hlc", hlc}, {"device", device}};
    if (!isDelete) c.insert(QStringLiteral("fields"), fields);
    // baseRev only travels with a body, as the server reads it for merging.
    if (hasBaseRev && !isDelete && fields.contains(QLatin1String("body")))
        c.insert(QStringLiteral("baseRev"), double(baseRev));
    return c;
}

ChangeResult ChangeResult::fromJson(const QJsonObject &o) {
    ChangeResult r;
    r.type = str(o, "type");
    r.id = str(o, "id");
    r.ok = flag(o, "ok");
    r.error = str(o, "error");
    r.seq = num(o, "seq");
    r.rev = num(o, "rev");
    r.merged = flag(o, "merged");
    r.hasBody = o.contains(QLatin1String("body"));
    r.body = str(o, "body");
    // The server omits an empty merged body (omitempty): merged means adopt "".
    if (r.merged && !r.hasBody) r.hasBody = true;
    r.conflictCopy = str(o, "conflictCopy");
    return r;
}

PullResponse PullResponse::fromJson(const QJsonObject &o) {
    PullResponse p;
    p.seq = num(o, "seq");
    for (const auto &v : o.value(QLatin1String("folders")).toArray()) p.folders << Folder::fromJson(v.toObject());
    for (const auto &v : o.value(QLatin1String("tags")).toArray()) p.tags << Tag::fromJson(v.toObject());
    for (const auto &v : o.value(QLatin1String("notes")).toArray()) p.notes << Note::fromJson(v.toObject());
    for (const auto &v : o.value(QLatin1String("attachments")).toArray()) p.attachments << Attachment::fromJson(v.toObject());
    return p;
}

QJsonObject PendingUpload::toJson() const {
    return QJsonObject{{"id", id}, {"name", name}, {"mime", mime}, {"createdAt", double(createdAt)}};
}

PendingUpload PendingUpload::fromJson(const QJsonObject &o) {
    PendingUpload u;
    u.id = str(o, "id");
    u.name = str(o, "name");
    u.mime = str(o, "mime");
    u.createdAt = num(o, "createdAt");
    return u;
}

QJsonObject User::toJson() const {
    return QJsonObject{{"id", id}, {"username", username}, {"email", email},
                       {"displayName", displayName}, {"role", role}, {"status", status},
                       {"aiAvailable", aiAvailable}};
}

User User::fromJson(const QJsonObject &o) {
    User u;
    u.id = str(o, "id");
    u.username = str(o, "username");
    u.email = str(o, "email");
    u.displayName = str(o, "displayName");
    u.role = str(o, "role");
    u.status = str(o, "status");
    u.aiAvailable = flag(o, "aiAvailable");
    return u;
}

ServerInfo ServerInfo::fromJson(const QJsonObject &o) {
    ServerInfo i;
    i.siteName = str(o, "siteName");
    i.allowRegistration = flag(o, "allowRegistration");
    i.hasUsers = flag(o, "hasUsers");
    i.version = str(o, "version");
    i.maxUploadMB = int(num(o, "maxUploadMB"));
    if (i.maxUploadMB <= 0) i.maxUploadMB = 200;
    i.storageMode = str(o, "storageMode");
    i.cdn = flag(o, "cdn");
    return i;
}

QString kindOfMime(const QString &mime) {
    if (mime.startsWith(QLatin1String("image/"))) return QStringLiteral("image");
    if (mime.startsWith(QLatin1String("video/"))) return QStringLiteral("video");
    if (mime.startsWith(QLatin1String("audio/"))) return QStringLiteral("audio");
    return QStringLiteral("file");
}
