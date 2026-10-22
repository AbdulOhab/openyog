/* OpenYog — Powertools ▸ Database Synchronization Wizard: pick a source and
 * a target database among the open connections (of the same kind), compare
 * the rows of the tables they share, tick what to bring over per table
 * (inserts, updates, deletes), and take the script to the target's query
 * editor or a file.
 *
 * Comparing and writing run on a worker thread with connections of their
 * own, so the window stays usable on big tables and can be cancelled. It
 * never changes a database itself; deletes start unticked. DataSync does
 * the comparing and writing; this is the picking. */
#pragma once

#include "ConnectionParams.h"
#include "DataSync.h"

#include <QDialog>

#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

class DataSyncDialog : public QDialog
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
        /* how to open a connection of its own to `db`, for the worker */
        std::function<ConnectionParams(const QString &db)> params;
        std::function<void(const QString &sql)> openInEditor;
    };

    DataSyncDialog(QList<Endpoint> endpoints, QWidget *parent = nullptr);
    ~DataSyncDialog() override;

    /* the --datasync selftest's way in: pick both sides, compare, and wait
     * for the worker; then what the window shows, a line per table */
    void compareForTest(int source, const QString &sourceDb, int target, const QString &targetDb);
    QStringList shownForTest() const;

protected:
    void reject() override;

private:
    void fillDatabases(QComboBox *databases, int endpoint, const QString &select);
    void compareNow();
    void showResults(const QList<DataSync::TableResult> &results);
    QList<DataSync::Pick> picks() const;
    void writeScript(const QString &file); /* empty: into the target's editor */
    void setBusy(bool busy, const QString &status = {});
    void updateButtons();

    QList<Endpoint> m_endpoints;
    QList<DataSync::TableResult> m_results;
    ConnectionParams m_sourceParams, m_targetParams;
    QString m_sourceDb, m_targetDb;
    int m_targetEndpoint = -1;
    bool m_busy = false;
    std::shared_ptr<std::atomic_bool> m_cancel;

    QComboBox *m_sourceConn = nullptr, *m_sourceDbBox = nullptr;
    QComboBox *m_targetConn = nullptr, *m_targetDbBox = nullptr;
    QPushButton *m_compare = nullptr, *m_cancelButton = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_open = nullptr, *m_save = nullptr;
};
