#include "TableDataView.h"
#include "wyString.h"

#include <QColor>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>

#include <cstring>

#include <mysql/mysql.h>

/* ---------------- editable model ---------------- */

class TableDataModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit TableDataModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setGrid(const QStringList &cols, const QVector<QStringList> &rows)
    {
        beginResetModel();
        m_cols = cols;
        m_rows = rows;
        endResetModel();
    }

    void setCell(int row, int col, const QString &value)
    {
        m_rows[row][col] = value;
        emit dataChanged(index(row, col), index(row, col));
    }

    void removeRow_(int row)
    {
        beginRemoveRows({}, row, row);
        m_rows.removeAt(row);
        endRemoveRows();
    }

    int rowCount(const QModelIndex & = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return m_cols.size(); }

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if(!idx.isValid())
            return {};
        if(role == Qt::DisplayRole || role == Qt::EditRole)
            return m_rows[idx.row()][idx.column()];
        if(role == Qt::ForegroundRole && m_rows[idx.row()][idx.column()] == "NULL")
            return QColor(Qt::gray);
        return {};
    }

    QVariant headerData(int s, Qt::Orientation o, int role) const override
    {
        if(role != Qt::DisplayRole)
            return {};
        if(o == Qt::Horizontal)
            return m_cols.value(s);
        return s + 1;
    }

    Qt::ItemFlags flags(const QModelIndex &idx) const override
    {
        return QAbstractTableModel::flags(idx) | Qt::ItemIsEditable;
    }

    bool setData(const QModelIndex &idx, const QVariant &value, int role) override
    {
        if(role != Qt::EditRole || !idx.isValid())
            return false;
        emit cellEdited(idx.row(), idx.column(),
                        m_rows[idx.row()][idx.column()], value.toString());
        return true;
    }

signals:
    void cellEdited(int row, int col, const QString &oldValue, const QString &newValue);

private:
    QStringList          m_cols;
    QVector<QStringList> m_rows;
};

/* ---------------- the view ---------------- */

TableDataView::TableDataView(QWidget *parent)
    : QWidget(parent)
{
    m_label = new QLabel(
        QStringLiteral("Double-click a table in the Object Browser to open it."),
        this);

    m_model = new TableDataModel(this);
    m_grid  = new QTableView(this);
    m_grid->setModel(m_model);
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->setAlternatingRowColors(true);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_label);
    layout->addWidget(m_grid, 1);

    connect(m_model, &TableDataModel::cellEdited, this,
            [this](int row, int col, const QString &oldV, const QString &newV) {
        applyCellEdit(row, col, oldV, newV);
    });
    connect(m_grid, &QTableView::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        if(!m_valid || !m_grid->indexAt(pos).isValid())
            return;
        QMenu menu(this);
        menu.addAction(QStringLiteral("&Delete Row"), this,
                       &TableDataView::deleteSelectedRow);
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Add Row"), this, &TableDataView::addRow);
        menu.exec(m_grid->viewport()->mapToGlobal(pos));
    });
}

void TableDataView::load(MYSQL *conn, const QString &db, const QString &table)
{
    m_conn = conn;
    m_db = db;
    m_table = table;
    reload();
}

void TableDataView::editCell(int row, int col, const QString &value)
{
    m_model->setData(m_model->index(row, col), value, Qt::EditRole);
}

void TableDataView::clear()
{
    m_valid = false;
    m_model->setGrid({}, {});
    m_label->setText(
        QStringLiteral("Double-click a table in the Object Browser to open it."));
}

void TableDataView::reload()
{
    /* columns + keys, upstream-style: SHOW COLUMNS + SHOW KEYS */
    QStringList pkeys;
    wyString keys;
    keys.Sprintf("SHOW KEYS FROM `%s`.`%s`", m_db.toUtf8().constData(),
                 m_table.toUtf8().constData());
    if(m_conn && mysql_query(m_conn, keys.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                /* row: Table, Non_unique(1), Key_name(2), Seq(3), Column(4)… */
                if(row[1] && row[2] && row[4]
                   && strcmp(row[1], "0") == 0
                   && strcmp(row[2], "PRIMARY") == 0)
                    pkeys << QString::fromUtf8(row[4]);
            }
            mysql_free_result(res);
        }
    }

    m_columns.clear();
    QStringList selects;   /* column list in SELECT-* order */
    wyString cols;
    cols.Sprintf("SHOW COLUMNS FROM `%s`.`%s`", m_db.toUtf8().constData(),
                 m_table.toUtf8().constData());
    if(m_conn && mysql_query(m_conn, cols.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                if(!row[0])
                    continue;
                m_columns << QString::fromUtf8(row[0]);
                selects << row[0];
            }
            mysql_free_result(res);
        }
    }
    if(m_columns.isEmpty()) {
        clear();
        emit statusMessage(QStringLiteral("cannot read columns of %1.%2")
                               .arg(m_db, m_table));
        return;
    }

    m_pkColumns.clear();
    for(int i = 0; i < m_columns.size(); ++i)
        if(pkeys.contains(m_columns[i]))
            m_pkColumns << i;
    m_hasPrimary = !m_pkColumns.isEmpty();   /* upstream: PK only, else all-cols */

    wyString data;
    data.Sprintf("SELECT * FROM `%s`.`%s` LIMIT 1000",
                 m_db.toUtf8().constData(), m_table.toUtf8().constData());
    QStringList header;
    QVector<QStringList> rows;
    if(m_conn && mysql_query(m_conn, data.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            for(unsigned int i = 0; i < mysql_num_fields(res); ++i) {
                MYSQL_FIELD *f = mysql_fetch_field(res);
                header << QString::fromUtf8(f->name);
            }
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                QStringList r;
                for(unsigned int i = 0; i < mysql_num_fields(res); ++i)
                    r << (row[i] ? QString::fromUtf8(row[i])
                                 : QStringLiteral("NULL"));
                rows << r;
            }
            mysql_free_result(res);
        }
    }
    m_model->setGrid(header, rows);
    m_valid = true;

    m_label->setText(QStringLiteral("%1.%2  —  %3 row(s)%4")
        .arg(m_db, m_table).arg(rows.size())
        .arg(m_hasPrimary ? QString()
                          : QStringLiteral("  ⚠ no primary key: row identity "
                                           "= entire row")));
    emit statusMessage(
        QStringLiteral("Opened %1.%2 (%3 rows)").arg(m_db, m_table)
            .arg(rows.size()));
}

QString TableDataView::quoteValue(const QString &v)
{
    if(v == QStringLiteral("NULL"))
        return QStringLiteral("NULL");
    QString s = v;
    s.replace('\\', QStringLiteral("\\\\"));
    s.replace('\'', QStringLiteral("\\'"));
    return '\'' + s + '\'';
}

QString TableDataView::rowMatchClause(int row) const
{
    QStringList conds;
    const auto addCol = [&](int col) {
        const QString v = m_model->index(row, col).data().toString();
        if(v == QStringLiteral("NULL"))
            conds << QStringLiteral("`%1` IS NULL").arg(m_columns[col]);
        else
            conds << QStringLiteral("`%1` = %2").arg(m_columns[col],
                                                     quoteValue(v));
    };
    if(m_hasPrimary)
        for(int col : m_pkColumns)
            addCol(col);
    else
        for(int col = 0; col < m_columns.size(); ++col)
            addCol(col);
    return conds.join(QStringLiteral(" and "));
}

void TableDataView::applyCellEdit(int row, int col,
                                  const QString &oldValue, const QString &newValue)
{
    if(!m_valid || !m_conn)
        return;
    const QString colName = m_columns[col];

    /* WHERE uses the OLD value of the edited column too (no-PK tables) */
    QStringList conds;
    const auto addOld = [&](int c) {
        const QString v = (c == col) ? oldValue
                                     : m_model->index(row, c).data().toString();
        if(v == QStringLiteral("NULL"))
            conds << QStringLiteral("`%1` IS NULL").arg(m_columns[c]);
        else
            conds << QStringLiteral("`%1` = %2").arg(m_columns[c],
                                                     quoteValue(v));
    };
    if(m_hasPrimary)
        for(int c : m_pkColumns)
            addOld(c);
    else
        for(int c = 0; c < m_columns.size(); ++c)
            addOld(c);
    if(conds.isEmpty())
        return;

    QString setClause;
    if(newValue == QStringLiteral("NULL"))
        setClause = QStringLiteral("`%1` = NULL").arg(colName);
    else
        setClause = QStringLiteral("`%1` = %2").arg(colName, quoteValue(newValue));

    wyString q;
    q.Sprintf("UPDATE `%s`.`%s` SET %s WHERE %s LIMIT 1",
              m_db.toUtf8().constData(), m_table.toUtf8().constData(),
              setClause.toUtf8().constData(), conds.join(" and ").toUtf8().constData());

    if(mysql_query(m_conn, q.GetString()) != 0) {
        emit statusMessage(QStringLiteral("UPDATE failed: ")
                           + mysql_error(m_conn));
        m_model->setCell(row, col, oldValue);   /* revert */
        return;
    }
    m_model->setCell(row, col, newValue);
    emit statusMessage(QStringLiteral("1 row updated"));
}

void TableDataView::deleteSelectedRow()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid() || !m_conn)
        return;
    const int row = idx.row();

    wyString q;
    q.Sprintf("DELETE FROM `%s`.`%s` WHERE %s LIMIT 1",
              m_db.toUtf8().constData(), m_table.toUtf8().constData(),
              rowMatchClause(row).toUtf8().constData());

    if(mysql_query(m_conn, q.GetString()) != 0) {
        emit statusMessage(QStringLiteral("DELETE failed: ")
                           + mysql_error(m_conn));
        return;
    }
    m_model->removeRow_(row);
    emit statusMessage(QStringLiteral("1 row deleted"));
}

void TableDataView::addRow()
{
    if(!m_valid || !m_conn)
        return;
    wyString q;
    q.Sprintf("INSERT INTO `%s`.`%s` () VALUES ()",
              m_db.toUtf8().constData(), m_table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        emit statusMessage(QStringLiteral("INSERT failed: ")
                           + mysql_error(m_conn));
        return;
    }
    reload();   /* picks up defaults/auto-increment from the server */
}

#include "TableDataView.moc"
