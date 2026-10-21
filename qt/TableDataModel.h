/* OpenYog — TableDataView's model and row-checkbox header, and the small
 * helpers its parts share: the editable grid model (staged edits, inserts,
 * deletes, with per-row state for the amber/green/red rendering) and the
 * left-hand check-column header. Internal to TableDataView*.cpp. */
#pragma once

#include "ConnectionParams.h"

#include <QAbstractButton>
#include <QAbstractTableModel>
#include <QApplication>
#include <QColor>
#include <QFont>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QSet>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionHeader>
#include <QVector>
/* a binary-literal expression for `hex` (already validated: even length,
 * lowercase 0-9a-f) — MySQL and SQLite both accept the x'…' form as-is,
 * but on PostgreSQL that syntax is a *bit-string* literal (type `bit`),
 * not a `bytea` one, so writing it into a binary column either fails to
 * cast or silently does the wrong thing rather than storing the intended
 * bytes. decode('…','hex') is PostgreSQL's own portable binary literal —
 * unlike '\x…'::bytea it doesn't depend on the standard_conforming_strings
 * setting for escaping. */
inline QString hexLiteral(SqlDriverType driver, const QString &hex)
{
    if(driver == SqlDriverType::Postgres)
        return QStringLiteral("decode('%1','hex')").arg(hex);
    return QStringLiteral("x'%1'").arg(hex);
}

/* Does a column's declared type look binary? A heuristic on the raw type
 * name, same style as ConnectionTab's CHAR/TEXT-only LIKE check — the
 * seam carries no portable type-category, just the driver-native string
 * (BLOB/TINYBLOB/MEDIUMBLOB/LONGBLOB/VARBINARY/BINARY for MySQL, BYTEA
 * for PostgreSQL; SQLite's dynamic typing means whatever the column was
 * declared as is all there is, and a BLOB-affinity name lands here) */
inline bool typeLooksBinary(const QString &typeName)
{
    const QString t = typeName.toUpper();
    return t.contains(QStringLiteral("BLOB")) || t.contains(QStringLiteral("BINARY")) ||
           t.contains(QStringLiteral("BYTEA"));
}

/* The row-header and select-all checkboxes are painted by hand: asking the
 * style for PE_IndicatorCheckBox gave a box with no visible frame under some
 * platform themes (the GTK one on XFCE), so they read as bare white squares.
 * A bordered box in the text colour, filled + ticked when checked — same
 * look as the QCheckBox indicators in Theme.cpp, on the header's own palette. */
inline void paintCheckIndicator(QPainter *p, const QRect &r, bool checked, const QPalette &pal)
{
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const bool dark = base.lightness() < 128;
    const QColor border((text.red() * 55 + base.red() * 45) / 100,
                        (text.green() * 55 + base.green() * 45) / 100,
                        (text.blue() * 55 + base.blue() * 45) / 100);
    const QColor accent = dark ? pal.color(QPalette::Highlight) : QColor(0x3B, 0x7D, 0xBB);
    const QColor tick = dark ? pal.color(QPalette::HighlightedText) : QColor(Qt::white);

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
    p->setPen(QPen(checked ? accent : border, 1));
    p->setBrush(checked ? accent : base);
    p->drawRoundedRect(box, 2, 2);
    if(checked) {
        QPen tp(tick, qMax(1.6, r.width() / 8.0));
        tp.setCapStyle(Qt::RoundCap);
        tp.setJoinStyle(Qt::RoundJoin);
        p->setPen(tp);
        p->setBrush(Qt::NoBrush);
        const qreal w = box.width(), h = box.height();
        QPainterPath path;
        path.moveTo(box.left() + w * 0.24, box.top() + h * 0.52);
        path.lineTo(box.left() + w * 0.43, box.top() + h * 0.72);
        path.lineTo(box.left() + w * 0.78, box.top() + h * 0.30);
        p->drawPath(path);
    }
    p->restore();
}

/* ---------------- editable model (staged edits + inserts + deletes) ------- */

class TableDataModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum RowState { Normal, Inserted, Deleted };

    explicit TableDataModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}

    void setGrid(const QStringList &cols, const QVector<QStringList> &rows)
    {
        beginResetModel();
        m_cols = cols;
        m_rows = rows;
        m_orig = rows; /* baseline for dirty tracking */
        m_state = QVector<RowState>(rows.size(), Normal);
        m_expr.clear();
        endResetModel();
        emit pendingChanged();
    }

    /* which columns are binary-typed (drives the <BLOB> display
     * placeholder in data()) — set right after setGrid() from the
     * caller's ColumnInfo; empty/no call means no blob columns */
    void setBlobColumns(const QVector<bool> &cols)
    {
        m_blobCols = cols;
    }

    /* append a blank pending-insert row; returns its index */
    int stageNewRow()
    {
        const int r = m_rows.size();
        const QStringList blank(m_cols.size());
        beginInsertRows({}, r, r);
        m_rows << blank;
        m_orig << blank;
        m_state << Inserted;
        endInsertRows();
        emit pendingChanged();
        return r;
    }

    /* Normal<->Deleted toggle; an Inserted row is just removed */
    void toggleDeleted(int row)
    {
        if(row < 0 || row >= m_rows.size())
            return;
        if(m_state[row] == Inserted) {
            beginRemoveRows({}, row, row);
            m_rows.removeAt(row);
            m_orig.removeAt(row);
            m_state.removeAt(row);
            m_expr.clear(); /* row indices shifted — drop raw exprs */
            endRemoveRows();
        } else {
            m_state[row] = (m_state[row] == Deleted) ? Normal : Deleted;
            emit dataChanged(index(row, 0), index(row, m_cols.size() - 1));
        }
        emit pendingChanged();
    }

    void revertAll()
    {
        beginResetModel();
        for(int r = m_rows.size() - 1; r >= 0; --r)
            if(m_state[r] == Inserted) {
                m_rows.removeAt(r);
                m_orig.removeAt(r);
                m_state.removeAt(r);
            }
        m_rows = m_orig;
        for(auto &s : m_state)
            s = Normal;
        m_expr.clear();
        endResetModel();
        emit pendingChanged();
    }

    bool dirty(int r, int c) const
    {
        return r < m_orig.size() && c < m_orig[r].size() && m_state.value(r) == Normal &&
               m_orig[r][c] != m_rows[r][c];
    }
    RowState rowState(int r) const
    {
        return m_state.value(r, Normal);
    }

    QList<int> rowsWithState(RowState st) const
    {
        QList<int> out;
        for(int r = 0; r < m_state.size(); ++r)
            if(m_state[r] == st)
                out << r;
        return out;
    }
    QList<int> dirtyRows() const /* Normal rows with edits only */
    {
        QList<int> out;
        for(int r = 0; r < m_rows.size(); ++r)
            for(int c = 0; c < m_cols.size(); ++c)
                if(dirty(r, c)) {
                    out << r;
                    break;
                }
        return out;
    }
    int pendingCells() const
    {
        int n = 0;
        for(int r = 0; r < m_rows.size(); ++r)
            for(int c = 0; c < m_cols.size(); ++c)
                if(dirty(r, c))
                    ++n;
        return n;
    }
    /* total staged operations (edits by row + inserts + deletes) */
    int pendingOps() const
    {
        return dirtyRows().size() + rowsWithState(Inserted).size() + rowsWithState(Deleted).size();
    }
    QString cur(int r, int c) const
    {
        return m_rows.value(r).value(c);
    }
    QString orig(int r, int c) const
    {
        return m_orig.value(r).value(c);
    }
    QStringList columns() const
    {
        return m_cols;
    }

    void stage(int row, int col, const QString &value)
    {
        if(row < 0 || row >= m_rows.size() || col < 0 || col >= m_cols.size())
            return;
        m_rows[row][col] = value;
        m_expr.remove({row, col}); /* plain text edit clears any raw expr */
        emit dataChanged(index(row, col), index(row, col),
                         {Qt::DisplayRole, Qt::EditRole, Qt::BackgroundRole, Qt::ForegroundRole});
        emit pendingChanged();
    }

    /* stage a value that must reach SQL verbatim, not string-quoted — e.g. a
     * binary literal x'…' from the Hex tab.  `display` is what the grid shows. */
    void stageExpr(int row, int col, const QString &display, const QString &sqlExpr)
    {
        if(row < 0 || row >= m_rows.size() || col < 0 || col >= m_cols.size())
            return;
        m_rows[row][col] = display;
        m_expr.insert({row, col}, sqlExpr);
        emit dataChanged(index(row, col), index(row, col),
                         {Qt::DisplayRole, Qt::EditRole, Qt::BackgroundRole});
        emit pendingChanged();
    }
    /* raw SQL expression staged for (r,c), or empty if it's a plain value */
    QString exprAt(int r, int c) const
    {
        return m_expr.value({r, c});
    }

    int rowCount(const QModelIndex & = {}) const override
    {
        return m_rows.size();
    }
    int columnCount(const QModelIndex & = {}) const override
    {
        return m_cols.size();
    }

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if(!idx.isValid())
            return {};
        const int r = idx.row(), c = idx.column();
        if(role == Qt::DisplayRole || role == Qt::EditRole) {
            /* untouched binary cells render as a placeholder: query()'s
             * rows went through QString::fromUtf8, so the "real" string is
             * mangled bytes — garbage on screen and useless to read. A
             * staged edit (Text tab, Hex tab's x'…' display) or a typed
             * new-row value still shows what the user put there */
            if(role == Qt::DisplayRole && c < m_blobCols.size() && m_blobCols.at(c) &&
               m_rows[r][c] != QStringLiteral("NULL") && m_state.value(r) == Normal &&
               !m_expr.contains({r, c}) && !dirty(r, c))
                return QStringLiteral("<BLOB>");
            return m_rows[r][c];
        }
        if(role == Qt::BackgroundRole) {
            if(m_state[r] == Inserted)
                return QColor(0xE6, 0xF4, 0xEA); /* green */
            if(m_state[r] == Deleted)
                return QColor(0xFD, 0xE7, 0xE9); /* red   */
            if(dirty(r, c))
                return QColor(0xFF, 0xF3, 0xC4); /* amber */
        }
        if(role == Qt::FontRole && m_state[r] == Deleted) {
            QFont f;
            f.setStrikeOut(true);
            return f;
        }
        if(role == Qt::ForegroundRole && m_rows[r][c] == QStringLiteral("NULL"))
            return QColor(Qt::gray);
        if(role == Qt::ToolTipRole && dirty(r, c))
            return QStringLiteral("was: %1").arg(m_orig[r][c]);
        return {};
    }

    QVariant headerData(int s, Qt::Orientation o, int role) const override
    {
        if(role != Qt::DisplayRole)
            return {};
        if(o == Qt::Vertical) {
            if(m_state.value(s) == Inserted)
                return QStringLiteral("＋");
            if(m_state.value(s) == Deleted)
                return QStringLiteral("✕");
            return s + 1;
        }
        return m_cols.value(s);
    }

    Qt::ItemFlags flags(const QModelIndex &idx) const override
    {
        Qt::ItemFlags f = QAbstractTableModel::flags(idx);
        if(idx.isValid() && m_state.value(idx.row()) == Deleted)
            return f & ~Qt::ItemIsEditable;
        return f | Qt::ItemIsEditable;
    }

    bool setData(const QModelIndex &idx, const QVariant &value, int role) override
    {
        if(role != Qt::EditRole || !idx.isValid())
            return false;
        stage(idx.row(), idx.column(), value.toString());
        return true;
    }

signals:
    void pendingChanged();

private:
    QStringList m_cols;
    QVector<QStringList> m_rows;
    QVector<QStringList> m_orig;
    QVector<RowState> m_state;
    QVector<bool> m_blobCols;              /* per-column, from setBlobColumns() */
    QMap<QPair<int, int>, QString> m_expr; /* (r,c) → verbatim SQL expr */
};

/* ---------------- row-select checkbox column ----------------------------- *
 * SQLyog's leftmost grid column is a checkbox (DataView tracks m_checkcount
 * and copy / export / delete act on the checked rows).  A vertical header
 * that paints a checkbox beside each row number gives the same behaviour
 * without shifting every data column by one.                               */

class RowCheckHeader : public QHeaderView
{
    Q_OBJECT
public:
    explicit RowCheckHeader(QWidget *parent = nullptr) : QHeaderView(Qt::Vertical, parent)
    {
        setSectionsClickable(true);
        setSectionResizeMode(QHeaderView::Fixed);
        setDefaultSectionSize(24);
        setFixedWidth(24);
    }

    QList<int> checkedRows() const
    {
        QList<int> out(m_checked.cbegin(), m_checked.cend());
        std::sort(out.begin(), out.end());
        return out;
    }

    void clearChecks()
    {
        if(m_checked.isEmpty())
            return;
        m_checked.clear();
        viewport()->update();
        emit checkedChanged();
    }

    void setAllChecked(bool on)
    {
        m_checked.clear();
        if(on && model())
            for(int r = 0; r < model()->rowCount(); ++r)
                m_checked.insert(r);
        viewport()->update();
        emit checkedChanged();
    }

    void setRowChecked(int row, bool on) /* selftest helper */
    {
        if(on)
            m_checked.insert(row);
        else
            m_checked.remove(row);
        viewport()->update();
        emit checkedChanged();
    }

    /* select-all state for the header/corner checkbox */
    Qt::CheckState allState() const
    {
        const int n = model() ? model()->rowCount() : 0;
        if(n == 0 || m_checked.isEmpty())
            return Qt::Unchecked;
        return m_checked.size() >= n ? Qt::Checked : Qt::PartiallyChecked;
    }

    /* SQLyog puts a select-all checkbox in the grid corner (where the two
     * headers meet) — paint one onto QTableView's corner button and make it
     * toggle every row instead of "select all cells" */
    void attachCornerButton(QAbstractButton *b)
    {
        m_corner = b;
        b->setText(QString());
        b->installEventFilter(this);
        connect(this, &RowCheckHeader::checkedChanged, b, qOverload<>(&QWidget::update));
    }

signals:
    void checkedChanged();

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if(o == m_corner) {
            switch(e->type()) {
                case QEvent::Paint: {
                    QPainter p(m_corner);
                    const QRect br = m_corner->rect();
                    /* same themed grey as the header sections */
                    QStyleOptionHeader ho;
                    ho.initFrom(m_corner);
                    ho.rect = br;
                    ho.position = QStyleOptionHeader::OnlyOneSection;
                    m_corner->style()->drawControl(QStyle::CE_Header, &ho, &p, m_corner);

                    const int sz = 14;
                    paintCheckIndicator(
                        &p,
                        QRect(br.center().x() - sz / 2 + 1, br.center().y() - sz / 2 + 1, sz, sz),
                        allState() == Qt::Checked, m_corner->palette());
                    return true; /* swallow the default corner paint */
                }
                case QEvent::MouseButtonRelease:
                    setAllChecked(allState() != Qt::Checked); /* all → none, else all */
                    return true;                              /* swallow "select all cells" */
                case QEvent::MouseButtonPress:
                case QEvent::MouseButtonDblClick:
                    return true;
                default:
                    break;
            }
        }
        return QHeaderView::eventFilter(o, e);
    }

    void paintSection(QPainter *p, const QRect &rect, int logical) const override
    {
        if(!rect.isValid())
            return;
        p->save();

        /* themed header background (no text — we place our own) */
        QStyleOptionHeader ho;
        ho.initFrom(this);
        ho.rect = rect;
        ho.section = logical;
        ho.orientation = Qt::Vertical;
        ho.text.clear();
        style()->drawControl(QStyle::CE_Header, &ho, p, this);

        /* just a checkbox, centred — no row number (SQLyog's leftmost column) */
        paintCheckIndicator(p, checkboxRect(rect), m_checked.contains(logical), palette());

        p->restore();
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const int logical = logicalIndexAt(e->pos());
        if(logical >= 0) {
            const int y = sectionViewportPosition(logical);
            const QRect sec(0, y, width(), sectionSize(logical));
            if(checkboxRect(sec).adjusted(-3, -3, 3, 3).contains(e->pos())) {
                if(!m_checked.remove(logical))
                    m_checked.insert(logical);
                viewport()->update();
                emit checkedChanged();
                return; /* don't start a row selection */
            }
        }
        QHeaderView::mousePressEvent(e);
    }

private:
    static QRect checkboxRect(const QRect &sec)
    {
        const int sz = 13;
        return QRect(sec.center().x() - sz / 2, sec.center().y() - sz / 2, sz, sz);
    }

    QSet<int> m_checked;
    QAbstractButton *m_corner = nullptr;
};
