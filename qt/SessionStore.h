/* OpenYog — what the app remembers between runs on its own, in
 * <AppDataLocation>/session.json: each saved connection's query tabs
 * (title, text, caret), which connections were open when it last quit, and
 * whether to reopen them on startup (Preferences).
 *
 * Separate from File ▸ Save Session, an explicit file listing connections
 * only. Keyed by the connection's saved name — the same key ConnectionStore
 * uses — so reopening a connection brings its tabs back, and the startup
 * reconnect can find its parameters and password. */
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace SessionStore {

struct EditorState
{
    QString title;
    QString text;
    int cursor = 0; /* QString offset */
};

/* a connection's query tabs as last saved; empty if none */
QList<EditorState> editors(const QString &connection);

/* replaces them; tabs with no text aren't kept, and a connection left with
 * none is dropped from the file */
void setEditors(const QString &connection, const QList<EditorState> &editors);

QStringList openConnections();
void setOpenConnections(const QStringList &names);

/* reopen the last session's connections (and so their tabs) on startup;
 * on unless switched off in Preferences */
bool restoreOnStartup();
void setRestoreOnStartup(bool on);

} // namespace SessionStore
