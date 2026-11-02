// Synced entities and their JSON form, as the server sends them.
// Unknown fields are ignored and missing ones read as zero values, so a newer
// server does not break this client.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

using StampMap = QHash<QString, QString>; // field -> HLC timestamp

struct Note {
    QString id;
    QString folderId; // "" = root
    QString title;
    QString body;
    QStringList tags; // tag ids
    bool pinned = false;
    qint64 rev = 0;
    qint64 seq = 0;
    qint64 trashedAt = 0;
    bool deleted = false;
    int words = 0;
    StampMap hlc;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;

    bool trashed() const { return trashedAt > 0; }
    QJsonObject toJson() const;
    static Note fromJson(const QJsonObject &o);
};

struct Folder {
    QString id;
    QString parentId;
    QString name;
    double sort = 0;
    qint64 seq = 0;
    bool deleted = false;
    StampMap hlc;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;

    QJsonObject toJson() const;
    static Folder fromJson(const QJsonObject &o);
};

struct Tag {
    QString id;
    QString name;
    QString color; // "" or green/blue/amber/pink/purple/gray or #rrggbb
    qint64 seq = 0;
    bool deleted = false;
    StampMap hlc;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;

    QJsonObject toJson() const;
    static Tag fromJson(const QJsonObject &o);
};

struct Attachment {
    QString id;
    QString name;
    QString mime;
    QString kind; // image | video | audio | file
    qint64 size = 0;
    int width = 0;
    int height = 0;
    QString sha256;
    QString storage;
    bool hasThumb = false;
    int refs = 0;
    qint64 orphanSince = 0;
    qint64 trashedAt = 0;
    QString trashReason;
    bool isPublic = false;
    QString publicUrl;
    qint64 seq = 0;
    bool deleted = false;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;

    QJsonObject toJson() const;
    static Attachment fromJson(const QJsonObject &o);
};

enum class EntityType { Note, Folder, Tag };
QString typeName(EntityType t);
bool parseType(const QString &s, EntityType *out);

// A pending local change waiting to be pushed. One per entity.
struct OutboxEntry {
    QString key; // "<type>:<id>"
    EntityType type = EntityType::Note;
    QString id;
    bool isDelete = false;
    QJsonObject fields;
    QString hlc;
    QString queuedAt; // HLC of the first edit: entries are pushed in this order
    bool hasBaseRev = false;
    qint64 baseRev = 0;
    int version = 0; // bumped on every local edit
    QString device;
    QString blocked; // why the server refused it; not persisted across restarts

    QJsonObject toJson() const;             // storage form
    static OutboxEntry fromJson(const QJsonObject &o);
    QJsonObject toChange() const;           // wire form for /sync/push
};

struct ChangeResult {
    QString type;
    QString id;
    bool ok = false;
    QString error;
    qint64 seq = 0;
    qint64 rev = 0;
    bool merged = false;
    bool hasBody = false;
    QString body;
    QString conflictCopy;

    static ChangeResult fromJson(const QJsonObject &o);
};

struct PullResponse {
    qint64 seq = 0;
    QList<Folder> folders;
    QList<Tag> tags;
    QList<Note> notes;
    QList<Attachment> attachments;

    static PullResponse fromJson(const QJsonObject &o);
};

// A file inserted while offline, waiting to be uploaded. Its content lives
// in the vault's uploads directory.
struct PendingUpload {
    QString id; // local-xxxx
    QString name;
    QString mime;
    qint64 createdAt = 0;
    QString failed; // refused by the server this run

    QJsonObject toJson() const;
    static PendingUpload fromJson(const QJsonObject &o);
};

struct User {
    QString id;
    QString username;
    QString email;
    QString displayName;
    QString role;
    QString status;
    bool aiAvailable = false;

    QJsonObject toJson() const;
    static User fromJson(const QJsonObject &o);
};

struct ServerInfo {
    QString siteName;
    bool allowRegistration = false;
    bool hasUsers = false;
    QString version;
    int maxUploadMB = 200;
    QString storageMode;
    bool cdn = false;

    static ServerInfo fromJson(const QJsonObject &o);
};

QString kindOfMime(const QString &mime);
