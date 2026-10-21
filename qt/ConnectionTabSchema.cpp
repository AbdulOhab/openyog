/* ConnectionTab, schema changes: create / alter / rename / drop / truncate for tables,
 * databases, views and routines, plus the column, index and foreign-key
 * editors.
 * The class is declared in ConnectionTab.h; ConnectionTab.cpp holds the
 * core (editors, query execution, results, object browser). */
#include "ConnectionTab.h"
#include "TableDataView.h"

#include "CreateTableDialog.h"
#include "SchemaSql.h"
#include "IndexDialog.h"
#include "ForeignKeyDialog.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>

namespace {

/* DbResultSet renders a SQL NULL as the literal string "NULL" (matches the
 * existing QueryModel/result-grid convention); callers that need "no value"
 * semantics instead (e.g. an optional column default fed into generated DDL)
 * must convert back — this undoes that sentinel where it matters. */
QString orEmpty(const QString &v)
{
    return v == QStringLiteral("NULL") ? QString() : v;
}

} // namespace

void ConnectionTab::promptCreateTable(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    CreateTableDialog dlg(db, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    if(sql.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create Table"),
                             QStringLiteral("Nothing to create — a table name and at least "
                                            "one column are required."));
        return;
    }
    execDdl(sql); /* execDdl already refreshes the object browser on success */
}

void ConnectionTab::dropTable(const QString &database, const QString &table)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(table.isEmpty() ||
       QMessageBox::question(this, QStringLiteral("Drop Table"),
                             QStringLiteral("Permanently DROP table `%1`.`%2`?").arg(db, table)) !=
           QMessageBox::Yes)
        return;
    execDdl(QStringLiteral("DROP TABLE %1").arg(m_conn->qualify(db, table)));
    if(m_tableData->loadedTable() == table)
        m_tableData->clear();
}

void ConnectionTab::truncateTable(const QString &database, const QString &table)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(table.isEmpty() ||
       QMessageBox::question(this, QStringLiteral("Truncate Table"),
                             QStringLiteral("Delete ALL rows of `%1`.`%2`?").arg(db, table)) !=
           QMessageBox::Yes)
        return;
    execDdl(m_conn->sqlTruncateTable(db, table));
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, table); /* empty grid */
}

/* ---- schema objects: View / Procedure / Function / Trigger / Event ------- */

void ConnectionTab::createSchemaObject(const QString &database, const QString &objType)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create %1").arg(objType),
                             QStringLiteral("Select a database first."));
        return;
    }
    if(m_params.driverType == SqlDriverType::Postgres && objType == QStringLiteral("EVENT")) {
        QMessageBox::information(
            this, QStringLiteral("Create Event"),
            QStringLiteral("PostgreSQL has no built-in scheduled-event feature."));
        return;
    }
    const QString nice = objType.left(1) + objType.mid(1).toLower();
    /* SQLite has none of these three — no stored procedures, no CREATE
     * FUNCTION (user-defined functions there are registered through the C
     * API, not SQL DDL), no event scheduler. Without this guard,
     * SchemaSql::createTemplate() falls into its generic "MySQL/SQLite"
     * branch and opens an editor tab pre-filled with MySQL-only syntax
     * that just fails with a bare syntax error on Run — View and Trigger
     * are genuinely fine on SQLite and stay unguarded. */
    if(m_params.driverType == SqlDriverType::Sqlite &&
       (objType == QStringLiteral("PROCEDURE") || objType == QStringLiteral("FUNCTION") ||
        objType == QStringLiteral("EVENT"))) {
        QMessageBox::information(this, QStringLiteral("Create %1").arg(nice),
                                 QStringLiteral("SQLite has no stored procedures, functions, or "
                                                "scheduled events — only tables, views, indexes "
                                                "and triggers."));
        return;
    }
    /* SQLyog opens the DDL in a new query-editor tab, not a modal dialog */
    openEditorWithSql(
        QStringLiteral("Create %1").arg(nice),
        SchemaSql::editorText(objType, db, QString(),
                              SchemaSql::createTemplate(objType, db, m_params.driverType), true,
                              m_params.driverType));
}

void ConnectionTab::alterSchemaObject(const QString &database, const QString &objType,
                                      const QString &name)
{
    if(!m_conn || name.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    const QString nice = objType.left(1) + objType.mid(1).toLower();

    QString error;
    const QString ddl = m_conn->showCreate(objType, db, name, &error);
    if(ddl.isEmpty() && !error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Alter %1").arg(nice), error);
        return;
    }
    if(ddl.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Alter %1").arg(nice),
                             QStringLiteral("Could not read the object's DDL."));
        return;
    }
    openEditorWithSql(QStringLiteral("Alter %1 `%2`").arg(nice, name),
                      SchemaSql::editorText(objType, db, name, SchemaSql::stripDefiner(ddl), false,
                                            m_params.driverType));
}

void ConnectionTab::dropSchemaObject(const QString &database, const QString &objType,
                                     const QString &name)
{
    if(!m_conn || name.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    const QString nice = objType.left(1) + objType.mid(1).toLower();
    if(QMessageBox::question(
           this, QStringLiteral("Drop %1").arg(nice),
           QStringLiteral("Permanently DROP %1 %2?").arg(nice, m_conn->qualify(db, name))) !=
       QMessageBox::Yes)
        return;
    /* PostgreSQL's DROP TRIGGER needs "ON table", not a db-qualified name —
     * pulled from pg_trigger since dropSchemaObject() has no table
     * parameter of its own to pass in */
    if(m_params.driverType == SqlDriverType::Postgres && objType == QStringLiteral("TRIGGER")) {
        DbResultSet rs;
        m_conn->query(
            QStringLiteral("SELECT c.relname FROM pg_trigger t "
                           "JOIN pg_class c ON c.oid = t.tgrelid "
                           "JOIN pg_namespace n ON n.oid = c.relnamespace "
                           "WHERE NOT t.tgisinternal AND n.nspname = '%1' AND t.tgname = '%2'")
                .arg(db, name),
            &rs, nullptr);
        if(rs.rows.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("Drop Trigger"),
                QStringLiteral("Could not find the table this trigger belongs to."));
            return;
        }
        execDdl(QStringLiteral("DROP TRIGGER IF EXISTS %1 ON %2")
                    .arg(m_conn->quoteIdent(name), m_conn->qualify(db, rs.rows.first().value(0))));
        return;
    }
    execDdl(QStringLiteral("DROP %1 IF EXISTS %2").arg(objType, m_conn->qualify(db, name)));
}

/* ---- database-level operations ----------------------------------------- */

void ConnectionTab::dropDatabase(const QString &database)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(m_params.driverType == SqlDriverType::Sqlite) {
        /* a SQLite "database" IS the open file — there's no DROP DATABASE
         * statement, and dropping the file out from under the very
         * connection reading it isn't something a DDL call can safely do
         * anyway. Empty Database (drop every object, keep the file) is the
         * closest real equivalent; actually removing the file is a
         * filesystem operation the user does after closing the tab. */
        QMessageBox::information(this, QStringLiteral("Drop Database"),
                                 QStringLiteral("A SQLite connection's database is the open file "
                                                "itself — there's nothing to DROP while it's "
                                                "connected. Use Empty Database to drop every "
                                                "table/view/trigger and keep the file, or close "
                                                "this connection and delete the file."));
        return;
    }
    if(QMessageBox::warning(
           this, QStringLiteral("Drop Database"),
           QStringLiteral("Permanently DROP database `%1` and everything in it?").arg(db),
           QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    /* PostgreSQL: "database" here means schema (see PostgresConnection.h —
     * a connection can't reach another actual Postgres database at all,
     * let alone drop one), and there's no DROP DATABASE for that; CASCADE
     * is required since a non-empty schema otherwise refuses to drop. */
    execDdl(m_params.driverType == SqlDriverType::Postgres
                ? QStringLiteral("DROP SCHEMA %1 CASCADE").arg(m_conn->quoteIdent(db))
                : QStringLiteral("DROP DATABASE `%1`").arg(db));
}

void ConnectionTab::truncateDatabase(const QString &database)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(QMessageBox::warning(this, QStringLiteral("Truncate Database"),
                            QStringLiteral("DROP every table, view, routine, trigger and event "
                                           "in `%1`?  (the empty database is kept)")
                                .arg(db),
                            QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::No) != QMessageBox::Yes)
        return;

    if(m_params.driverType == SqlDriverType::Postgres) {
        /* no per-schema charset/collation to preserve (that's a whole-
         * database property in Postgres) — just drop and recreate empty */
        const QString qdb = m_conn->quoteIdent(db);
        if(execDdl(QStringLiteral("DROP SCHEMA %1 CASCADE").arg(qdb)))
            execDdl(QStringLiteral("CREATE SCHEMA %1").arg(qdb));
        return;
    }
    if(m_params.driverType == SqlDriverType::Sqlite) {
        /* SQLite has no CREATE/DROP DATABASE at all — "truncate the
         * database, keep it empty" just means drop every object directly;
         * views first (harmless either order, but avoids a moment where a
         * view outlives the table it reads), then tables (dropping a table
         * also drops its own triggers/indexes automatically in SQLite) */
        for(const QString &v : m_conn->listTables(db, QStringLiteral("VIEW")))
            execDdl(QStringLiteral("DROP VIEW %1").arg(m_conn->qualify(db, v)));
        for(const QString &t : m_conn->listTables(db, QStringLiteral("BASE TABLE")))
            execDdl(QStringLiteral("DROP TABLE %1").arg(m_conn->qualify(db, t)));
        return;
    }

    /* read the current charset/collation so the recreated db keeps them */
    QString charset = QStringLiteral("utf8mb4"), collation;
    {
        DbResultSet rs;
        if(m_conn->query(QStringLiteral("SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME "
                                        "FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='%1'")
                             .arg(QString(db).replace('\'', QStringLiteral("''"))),
                         &rs, nullptr) &&
           !rs.rows.isEmpty()) {
            if(!orEmpty(rs.rows.first().value(0)).isEmpty())
                charset = rs.rows.first().value(0);
            collation = orEmpty(rs.rows.first().value(1));
        }
    }
    const QString bq = QString(db).replace('`', QStringLiteral("``"));
    QString create = QStringLiteral("CREATE DATABASE `%1` CHARACTER SET %2").arg(bq, charset);
    if(!collation.isEmpty())
        create += QStringLiteral(" COLLATE %1").arg(collation);
    if(execDdl(QStringLiteral("DROP DATABASE `%1`").arg(bq)))
        execDdl(create);
}

void ConnectionTab::emptyDatabase(const QString &database)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(QMessageBox::warning(
           this, QStringLiteral("Empty Database"),
           QStringLiteral("TRUNCATE every base table in `%1`?  All rows are lost.").arg(db),
           QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    const QStringList tables = m_conn->listTables(db, QStringLiteral("BASE TABLE"));
    execDdl(m_conn->sqlFkChecks(false));
    for(const QString &t : tables)
        execDdl(m_conn->sqlTruncateTable(db, t));
    execDdl(m_conn->sqlFkChecks(true));
    if(!m_tableData->loadedTable().isEmpty() && tables.contains(m_tableData->loadedTable()))
        m_tableData->load(m_conn, db, m_tableData->loadedTable());
}

void ConnectionTab::promptAlterDatabase(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(m_params.driverType == SqlDriverType::Postgres) {
        QMessageBox::information(this, QStringLiteral("Alter Database"),
                                 QStringLiteral("Character set/collation are whole-database "
                                                "properties in PostgreSQL, fixed at creation — "
                                                "there's nothing here to alter for a schema."));
        return;
    }
    if(m_params.driverType == SqlDriverType::Sqlite) {
        QMessageBox::information(this, QStringLiteral("Alter Database"),
                                 QStringLiteral("SQLite has no per-database character set or "
                                                "collation to alter — text encoding is fixed "
                                                "(UTF-8/16) for the whole file at creation, and "
                                                "collations are attached per-column/-index, not "
                                                "to the database as a whole."));
        return;
    }

    QString curCharset, curCollation;
    {
        DbResultSet rs;
        if(m_conn->query(QStringLiteral("SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME "
                                        "FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='%1'")
                             .arg(QString(db).replace('\'', QStringLiteral("''"))),
                         &rs, nullptr) &&
           !rs.rows.isEmpty()) {
            curCharset = orEmpty(rs.rows.first().value(0));
            curCollation = orEmpty(rs.rows.first().value(1));
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Alter Database — `%1`").arg(db));
    auto *form = new QFormLayout(&dlg);
    auto *charsetEdit = new QLineEdit(curCharset, &dlg);
    auto *collationEdit = new QLineEdit(curCollation, &dlg);
    form->addRow(QStringLiteral("Character set"), charsetEdit);
    form->addRow(QStringLiteral("Collation"), collationEdit);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if(dlg.exec() != QDialog::Accepted)
        return;

    QString sql =
        QStringLiteral("ALTER DATABASE `%1`").arg(QString(db).replace('`', QStringLiteral("``")));
    if(!charsetEdit->text().trimmed().isEmpty())
        sql += QStringLiteral(" CHARACTER SET %1").arg(charsetEdit->text().trimmed());
    if(!collationEdit->text().trimmed().isEmpty())
        sql += QStringLiteral(" COLLATE %1").arg(collationEdit->text().trimmed());
    execDdl(sql);
}

void ConnectionTab::promptRenameTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Table"),
                                               QStringLiteral("New name for `%1`:").arg(table),
                                               QLineEdit::Normal, table, &ok);
    if(!ok || name.trimmed().isEmpty() || name == table)
        return;
    /* MySQL: RENAME TABLE db.old TO db.new. Neither PostgreSQL nor SQLite
     * has that statement — ALTER TABLE ... RENAME TO new does the same job
     * on both, within the same schema (there's no cross-schema form to
     * worry about since this function never changes db, only the name). */
    execDdl(
        m_params.driverType != SqlDriverType::Mysql
            ? QStringLiteral("ALTER TABLE %1 RENAME TO %2")
                  .arg(m_conn->qualify(db, table), m_conn->quoteIdent(name.trimmed()))
            : QStringLiteral("RENAME TABLE `%1`.`%2` TO `%1`.`%3`").arg(db, table, name.trimmed()));
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, name.trimmed());
}

void ConnectionTab::promptDropColumn(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    const QString qualified = m_conn->qualify(db, table);

    QStringList cols;
    for(const QStringList &row : m_conn->listColumns(db, table).rows)
        cols << row.value(0);
    if(cols.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Drop Column"),
                                 QStringLiteral("No columns found in %1.").arg(qualified));
        return;
    }
    bool ok = false;
    const QString col = QInputDialog::getItem(
        this, QStringLiteral("Drop Column"),
        QStringLiteral("Column to drop from %1:").arg(qualified), cols, 0, false, &ok);
    if(!ok || col.isEmpty())
        return;
    if(QMessageBox::question(this, QStringLiteral("Drop Column"),
                             QStringLiteral("Drop column `%1` from %2? This cannot be undone.")
                                 .arg(col, qualified)) != QMessageBox::Yes)
        return;
    execDdl(
        QStringLiteral("ALTER TABLE %1 DROP COLUMN %2").arg(qualified, m_conn->quoteIdent(col)));
}

void ConnectionTab::promptManageIndexes(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    QList<IndexDialog::IndexDef> indexes;
    const auto findIx = [&](const QString &n) -> IndexDialog::IndexDef * {
        for(auto &ix : indexes)
            if(ix.name == n)
                return &ix;
        return nullptr;
    };
    /* canonical SHOW INDEX shape: Non_unique(1) Key_name(2) Column_name(4) */
    for(const QStringList &row : m_conn->listIndexes(db, table).rows) {
        const QString name = row.value(2);
        const QString col = row.value(4);
        IndexDialog::IndexDef *ix = findIx(name);
        if(!ix) {
            IndexDialog::IndexDef nd;
            nd.name = name;
            nd.unique = row.value(1) == QStringLiteral("0");
            nd.primary = name == QStringLiteral("PRIMARY");
            indexes << nd;
            ix = &indexes.last();
        }
        ix->columns << col;
    }

    QStringList cols;
    for(const QStringList &row : m_conn->listColumns(db, table).rows)
        cols << row.value(0);

    IndexDialog dlg(db, table, indexes, cols, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Manage Indexes"),
                                 QStringLiteral("No changes to apply."));
        return;
    }
    execDdl(sql);
}

void ConnectionTab::promptManageForeignKeys(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    QList<ForeignKeyDialog::FkDef> fks;
    const auto find = [&](const QString &n) -> ForeignKeyDialog::FkDef * {
        for(auto &f : fks)
            if(f.name == n)
                return &f;
        return nullptr;
    };
    /* canonical shape (qt/db/IDbConnection.h): Name(0) Column(1) Ref_table(2)
     * Ref_column(3) On_update(4) On_delete(5), one row per column */
    for(const QStringList &row : m_conn->listForeignKeys(db, table).rows) {
        const QString name = row.value(0);
        ForeignKeyDialog::FkDef *f = find(name);
        if(!f) {
            ForeignKeyDialog::FkDef nf;
            nf.name = name;
            nf.refTable = row.value(2);
            nf.onDelete =
                row.value(5) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT") : row.value(5);
            nf.onUpdate =
                row.value(4) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT") : row.value(4);
            fks << nf;
            f = &fks.last();
        }
        f->columns << row.value(1);
        f->refColumns << row.value(3);
    }

    QStringList cols, tables;
    for(const QStringList &row : m_conn->listColumns(db, table).rows)
        cols << row.value(0);
    tables = m_conn->listTables(db, QStringLiteral("BASE TABLE"));

    ForeignKeyDialog dlg(db, table, fks, cols, tables, m_conn, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    const QString limitation = dlg.limitation();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Foreign Keys"),
                                 limitation.isEmpty() ? QStringLiteral("No changes to apply.")
                                                      : limitation);
        return;
    }
    if(!limitation.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Foreign Keys"), limitation);
    execDdl(sql);
}

void ConnectionTab::promptAlterTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    /* canonical shape via listColumns(): Field(0) Type(1) Null(2) Key(3)
     * Default(4) Extra(5) Comment(6) — portable across backends, unlike the
     * raw "SHOW FULL COLUMNS" this used to send (MySQL-only syntax, and it
     * additionally broke on every SQLite connection via the same empty-db
     * qualification bug already fixed elsewhere: db is always "" there). */
    const DbResultSet colRs = m_conn->listColumns(db, table);
    if(colRs.rows.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Alter Table"),
                             QStringLiteral("Could not read columns of `%1`.`%2`.").arg(db, table));
        return;
    }
    QList<CreateTableDialog::ColumnDef> cols;
    for(const QStringList &row : colRs.rows) {
        CreateTableDialog::ColumnDef c;
        c.name = row.value(0);
        QString type = row.value(1).trimmed();
        c.isUnsigned = type.contains(QStringLiteral(" unsigned"), Qt::CaseInsensitive);
        type.remove(QStringLiteral(" unsigned"), Qt::CaseInsensitive);
        type.remove(QStringLiteral(" zerofill"), Qt::CaseInsensitive);
        const int lp = type.indexOf('(');
        if(lp >= 0 && type.endsWith(')')) {
            c.length = type.mid(lp + 1, type.size() - lp - 2);
            c.type = type.left(lp).toUpper();
        } else {
            c.type = type.toUpper();
        }
        c.notNull = row.value(2) == QStringLiteral("NO");
        c.pk = row.value(3) == QStringLiteral("PRI");
        c.def = orEmpty(row.value(4));
        c.autoInc = row.value(5).contains(QStringLiteral("auto_increment"), Qt::CaseInsensitive);
        c.comment = orEmpty(row.value(6));
        cols << c;
    }
    if(cols.isEmpty())
        return;

    QString engine, charset;
    if(m_params.driverType == SqlDriverType::Mysql) {
        /* ENGINE/TABLE_COLLATION: MySQL-only information_schema columns —
         * PostgreSQL's information_schema.tables has neither (no storage
         * engines, no per-table charset), so this stays MySQL-only rather
         * than sending a query guaranteed to fail there */
        DbResultSet rs;
        if(m_conn->query(QStringLiteral("SELECT ENGINE, SUBSTRING_INDEX(TABLE_COLLATION,'_',1) "
                                        "FROM information_schema.TABLES "
                                        "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='%2'")
                             .arg(db, table),
                         &rs, nullptr) &&
           !rs.rows.isEmpty()) {
            engine = orEmpty(rs.rows.first().value(0));
            charset = orEmpty(rs.rows.first().value(1));
        }
    }

    CreateTableDialog dlg(db, table, cols, engine, charset, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    const QString limitation = dlg.alterLimitation();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Alter Table"),
                                 limitation.isEmpty() ? QStringLiteral("No changes to apply.")
                                                      : limitation);
        return;
    }
    if(!limitation.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Alter Table"), limitation);
    execDdl(sql);
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, table);
}
