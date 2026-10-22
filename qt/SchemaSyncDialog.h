/* OpenYog — Powertools ▸ Schema Synchronization Tool: pick a source and a
 * target database among the open connections (of the same kind), compare
 * them, tick the differences to bring over, and take the script that does
 * it to the target's query editor or a file.
 *
 * It never changes a database itself: the script is there to be read and
 * run by hand. Dropping a table, a column, a view, routine or trigger, and
 * table option changes, start unticked. SchemaSync does the comparing and writing; this is the picking. */
#pragma once

#include "SchemaSync.h"

#include <QDialog>

#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class SqlEditor;

class SchemaSyncDialog : public QDialog
{
    Q_OBJECT
public:
    /* an open connection, as a side to compare */
    struct Endpoint
    {
        QString label;
        SqlDriverType driver = SqlDriverType::Mysql;
        std::function<QStringList()> databases;
        std::function<QString()> currentDatabase;
        std::function<SchemaSync::Schema(const QString &db)> load;
        std::function<void(const QString &sql)> openInEditor;
    };

    SchemaSyncDialog(QList<Endpoint> endpoints, QWidget *parent = nullptr);

    /* the --schemasync selftest's way in: pick both sides and compare */
    void compareForTest(int source, const QString &sourceDb, int target, const QString &targetDb);
    QString scriptText() const;

private:
    void fillDatabases(QComboBox *databases, int endpoint, const QString &select);
    void compareNow();
    void updateScript();
    QList<SchemaSync::Change> pickedChanges() const;
    void saveScript();

    QList<Endpoint> m_endpoints;
    SchemaSync::Schema m_source, m_target;
    QList<SchemaSync::Change> m_changes;
    QList<QTreeWidgetItem *> m_changeItems; /* per change */
    bool m_filling = false;

    QComboBox *m_sourceConn = nullptr, *m_sourceDb = nullptr;
    QComboBox *m_targetConn = nullptr, *m_targetDb = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_tree = nullptr;
    SqlEditor *m_script = nullptr;
    QPushButton *m_open = nullptr, *m_save = nullptr;
    int m_targetEndpoint = -1;
};
