/* OpenYog — read-only table model over a MariaDB result set.
 * The plan.md porting map calls this the QTableView counterpart of the
 * upstream custom CustGrid (Phase 4 adds inline editing on top). */
#pragma once

#include <QAbstractTableModel>
#include <QStringList>
#include <QVector>

#include "db/IDbConnection.h"

class QueryModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit QueryModel(QObject *parent = nullptr);

    /* Executes one statement. Returns false and fills *message on failure.
     * For statements without a result set, returns true with a human
     * message (affected rows) and clears the grid. */
    bool execute(IDbConnection *conn, const QString &query, QString *message);

    /* Loads a pre-collected result set (used by the threaded executor so
     * rows are marshalled to the GUI thread, not fetched there). */
    void setResultSet(const QStringList &headers, const QVector<QStringList> &rows);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &idx, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    QStringList m_cols;
    QVector<QStringList> m_rows;
};
