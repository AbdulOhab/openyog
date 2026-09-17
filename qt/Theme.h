/* OpenYog — application theme (light/dark), persisted via wyIni in
 * ~/.config/OpenYog/OpenYog.ini [UserInterface] theme=… (same INI engine
 * SQLyog used for its own settings). */
#pragma once

#include <QString>
class QApplication;

namespace Theme {
QString load(); /* "light" | "dark" | "twilight" */
void save(const QString &theme);
void apply(QApplication &app, const QString &theme);
} // namespace Theme
