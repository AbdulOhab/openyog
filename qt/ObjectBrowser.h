/* OpenYog — the left-side Object Browser: filter box + tree
 * (connection → databases → Tables/Views/Stored Procs/Functions/Triggers/Events),
 * mirroring upstream ObjectBrowser.cpp's structure. Lazy-loads children on
 * expand; folders other than Tables arrive in later phases. */
#pragma once

#include <QLineEdit>
#include <QTreeWidget>
#include <QWidget>

#include <mysql/mysql.h>   /* typedef struct st_mysql MYSQL — no forward decl */

class ObjectBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit ObjectBrowser(QWidget *parent = nullptr);

    void setConnectionLabel(const QString &label);
    void loadDatabases(MYSQL *conn, const QString &currentDb);

signals:
    void databaseActivated(const QString &db);      /* double click → USE */
    void tableActivated(const QString &db, const QString &table); /* → SELECT */
    void statusMessage(const QString &text);

private slots:
    void onItemExpanded(QTreeWidgetItem *item);
    void applyFilter(const QString &text);

private:
    enum ItemRole { RoleKind = Qt::UserRole + 1, RoleName };
    enum Kind { KindConnection, KindDatabase, KindFolder, KindTable };

    QLineEdit    *m_filter = nullptr;
    QTreeWidget  *m_tree   = nullptr;
    MYSQL        *m_conn   = nullptr;
};
