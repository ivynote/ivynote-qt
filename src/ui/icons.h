// Line icons on a 24 grid, stroke 1.75 with round caps (guide 10.7); paths
// from the web client's Icon.svelte, tinted on demand.
#pragma once

#include <QColor>
#include <QIcon>
#include <QPainterPath>
#include <QPixmap>

namespace icons {

QIcon icon(const QString &name, const QColor &color);
QPixmap pixmap(const QString &name, const QColor &color, int size, qreal dpr = 1.0);
// Icon in the theme's muted color, with an ink-colored active state.
QIcon themed(const QString &name);

// The ivy leaf (viewBox 0 0 64 64): outline and veins as separate paths.
QPainterPath leafOutline();
QList<QPainterPath> leafVeins();
QIcon appIcon();

} // namespace icons
