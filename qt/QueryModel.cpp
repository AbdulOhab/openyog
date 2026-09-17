#include "QueryModel.h"

#include <QColor>

QueryModel::QueryModel(QObject *parent) : QAbstractTableModel(parent) {}

bool QueryModel::execute(IDbConnection *conn, const QString &query, QString *message)
{
    beginResetModel();
    m_cols.clear();
    m_rows.clear();

    DbResultSet rs;
    if(!conn->query(query, &rs, message)) {
        endResetModel();
        return false;
    }
    m_cols = rs.headers;
    m_rows = rs.rows;

    endResetModel();
    return true;
}

void QueryModel::setResultSet(const QStringList &headers, const QVector<QStringList> &rows)
{
    beginResetModel();
    m_cols = headers;
    m_rows = rows;
    endResetModel();
}

int QueryModel::rowCount(const QModelIndex &) const
{
    return (int)m_rows.size();
}
int QueryModel::columnCount(const QModelIndex &) const
{
    return (int)m_cols.size();
}

QVariant QueryModel::data(const QModelIndex &idx, int role) const
{
    if(!idx.isValid())
        return {};
    const QString &v = m_rows[idx.row()][idx.column()];
    if(role == Qt::DisplayRole || role == Qt::EditRole)
        return v;
    if(role == Qt::ForegroundRole && v == QStringLiteral("NULL"))
        return QColor(Qt::gray);
    return {};
}

QVariant QueryModel::headerData(int section, Qt::Orientation o, int role) const
{
    if(role != Qt::DisplayRole)
        return {};
    if(o == Qt::Horizontal)
        return m_cols.value(section);
    return section + 1;
}
