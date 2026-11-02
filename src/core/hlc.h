// Hybrid logical clock compatible with the server:
// "<13-digit ms>-<4-hex counter>-<node>", compared as plain strings.
#pragma once

#include <QString>
#include <functional>

class Hlc {
public:
    explicit Hlc(QString node, std::function<qint64()> now = {});

    QString tick();
    void observe(const QString &ts);

    static QString format(qint64 wall, int counter, const QString &node);
    static bool parse(const QString &ts, qint64 *wall, int *counter, QString *node = nullptr);

private:
    QString m_node;
    std::function<qint64()> m_now;
    qint64 m_wall = 0;
    int m_counter = 0;
};
