#include "QueryModel.h"

#include "wyString.h"       /* core string class: query text path */
#include <mysql/mysql.h>

QueryModel::QueryModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

bool QueryModel::execute(MYSQL *conn, const QString &query, QString *message)
{
    beginResetModel();
    m_cols.clear();
    m_rows.clear();

    /* query text goes through the ported core string class, UTF-8 */
    wyString q;
    q.SetAs(query.toUtf8().constData());

    bool ok = mysql_query(conn, q.GetString()) == 0;
    if(!ok) {
        endResetModel();
        if(message) *message = QString::fromUtf8(mysql_error(conn));
        return false;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if(res) {
        const unsigned int n = mysql_num_fields(res);
        MYSQL_FIELD *fields = mysql_fetch_fields(res);
        for(unsigned int i = 0; i < n; ++i)
            m_cols << QString::fromUtf8(fields[i].name);

        while(MYSQL_ROW row = mysql_fetch_row(res)) {
            QStringList r;
            for(unsigned int i = 0; i < n; ++i)
                r << (row[i] ? QString::fromUtf8(row[i]) : QStringLiteral("NULL"));
            m_rows << r;
        }
        mysql_free_result(res);
        if(message)
            *message = QStringLiteral("%1 row(s) in result set").arg(m_rows.size());
    } else if(mysql_field_count(conn) == 0) {
        if(message)
            *message = QStringLiteral("OK, %1 row(s) affected")
                           .arg((long long)mysql_affected_rows(conn));
    } else {
        endResetModel();
        if(message) *message = QString::fromUtf8(mysql_error(conn));
        return false;
    }

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

int QueryModel::rowCount(const QModelIndex &) const { return (int)m_rows.size(); }
int QueryModel::columnCount(const QModelIndex &) const { return (int)m_cols.size(); }

QVariant QueryModel::data(const QModelIndex &idx, int role) const
{
    if(!idx.isValid() || role != Qt::DisplayRole)
        return {};
    return m_rows[idx.row()][idx.column()];
}

QVariant QueryModel::headerData(int section, Qt::Orientation o, int role) const
{
    if(role != Qt::DisplayRole)
        return {};
    if(o == Qt::Horizontal)
        return m_cols.value(section);
    return section + 1;
}
