/* OpenYog — the left-side Object Browser: filter box + tree
 * (connection → databases → Tables/Views/Stored Procs/Functions/Triggers/Events),
 * mirroring upstream ObjectBrowser.cpp's structure. Lazy-loads children on
 * expand; folders other than Tables arrive in later phases. */
#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QWidget>

class IDbConnection;

class ObjectBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit ObjectBrowser(QWidget *parent = nullptr);

    void setConnectionLabel(const QString &label);
    void loadDatabases(IDbConnection *conn, const QString &currentDb);

    /* [db, table] of the currently selected table item, else empty */
    QStringList currentTableInfo() const;

    void collapseTree();   /* Edit ▸ Collapse All in Object Browser */

signals:
    void databaseActivated(const QString &db);      /* double click → USE */
    void tableActivated(const QString &db, const QString &table); /* → SELECT */
    void dropTableRequested(const QString &db, const QString &table);
    void truncateTableRequested(const QString &db, const QString &table);
    void createTableRequested(const QString &db);
    void alterTableRequested(const QString &db, const QString &table);
    void renameTableRequested(const QString &db, const QString &table);
    void copyTableRequested(const QString &db, const QString &table);
    void manageIndexesRequested(const QString &db, const QString &table);
    void manageForeignKeysRequested(const QString &db, const QString &table);
    void copyDatabaseRequested(const QString &db);
    void importCsvRequested(const QString &db, const QString &table);
    void importXmlRequested(const QString &db, const QString &table);
    void exportTableRequested(const QString &db, const QString &table);
    void dumpDatabaseRequested(const QString &db);
    /* schema objects — objType is the SQL keyword: VIEW / PROCEDURE / FUNCTION
     * / TRIGGER / EVENT */
    void createObjectRequested(const QString &db, const QString &objType);
    void alterObjectRequested(const QString &db, const QString &objType,
                              const QString &name);
    void dropObjectRequested(const QString &db, const QString &objType,
                             const QString &name);
    void dropDatabaseRequested(const QString &db);
    void truncateDatabaseRequested(const QString &db);
    void emptyDatabaseRequested(const QString &db);
    void alterDatabaseRequested(const QString &db);
    void statusMessage(const QString &text);

private slots:
    void onItemExpanded(QTreeWidgetItem *item);
    void applyFilter(const QString &text);

private:
    void copyCreateTable(const QString &db, const QString &table);
    void copyColumnNames(QTreeWidgetItem *tableItem);

    enum ItemRole { RoleKind = Qt::UserRole + 1, RoleName };
    enum Kind { KindConnection, KindDatabase, KindFolder, KindTable };

    QLabel        *m_filterLabel = nullptr;
    QLineEdit     *m_filter = nullptr;
    QTreeWidget   *m_tree   = nullptr;
    IDbConnection *m_conn   = nullptr;
};
