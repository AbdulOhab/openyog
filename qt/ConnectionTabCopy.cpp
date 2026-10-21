/* ConnectionTab, copying databases and tables: Copy Table / Copy Database on the same
 * server, Copy Table(s) To Different Host, SQLite file copies, and the
 * MySQL/SQLite → PostgreSQL copy path.
 * The class is declared in ConnectionTab.h; ConnectionTab.cpp holds the
 * core (editors, query execution, results, object browser). */
#include "ConnectionTab.h"

#include "SqlDump.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QPlainTextEdit>
#include <QApplication>

#include <QFileDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QRegularExpression>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QFile>
#include <QVBoxLayout>
#include <utility>

void ConnectionTab::promptCopyTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString srcDb = database.isEmpty() ? defaultDb() : database;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Duplicate Table `%1`").arg(table));
    auto *name = new QLineEdit(table + QStringLiteral("_copy"), &dlg);
    auto *targetDb = new QComboBox(&dlg);
    targetDb->addItems(m_databases.isEmpty() ? QStringList{srcDb} : m_databases);
    targetDb->setCurrentText(srcDb);
    auto *wantStructure = new QCheckBox(QStringLiteral("Structure"), &dlg);
    auto *wantData = new QCheckBox(QStringLiteral("Data"), &dlg);
    wantStructure->setChecked(true);
    wantData->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("New table name"), name);
    form->addRow(QStringLiteral("Target database"), targetDb);
    form->addRow(QString(), wantStructure);
    form->addRow(QString(), wantData);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Duplicate"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString tgt = name->text().trimmed();
    const QString tgtDb = targetDb->currentText();
    if(tgt.isEmpty())
        return;
    const QString src = m_conn->qualify(srcDb, table);
    const QString dst = m_conn->qualify(tgtDb, tgt);

    if(wantStructure->isChecked()) {
        bool ok;
        if(m_params.driverType == SqlDriverType::Postgres) {
            /* PostgreSQL needs the parenthesized LIKE-clause form, and
             * INCLUDING ALL to get the same completeness a bare MySQL LIKE
             * gives by default (otherwise only column definitions copy,
             * not indexes/defaults/constraints) */
            ok = execDdl(QStringLiteral("CREATE TABLE %1 (LIKE %2 INCLUDING ALL)").arg(dst, src));
        } else if(m_params.driverType == SqlDriverType::Sqlite) {
            /* SQLite has no LIKE clause at all — showCreate("TABLE") for
             * SQLite replays the table's own original CREATE TABLE text
             * (sqlite_master.sql, byte-for-byte), so retarget just its
             * table name to the destination and replay that. This copies
             * every column/default/constraint/CHECK the original DDL had,
             * but not secondary indexes — those are each their own
             * separate CREATE INDEX statement in sqlite_master, not part
             * of CREATE TABLE's own text, and aren't reconstructed here. */
            QString err;
            QString ddl = m_conn->showCreate(QStringLiteral("TABLE"), srcDb, table, &err);
            static const QRegularExpression nameRe(
                QStringLiteral("^(CREATE\\s+TABLE\\s+(?:IF\\s+NOT\\s+EXISTS\\s+)?)"
                               "(?:\"[^\"]+\"|`[^`]+`|\\[[^\\]]+\\]|\\w+)"),
                QRegularExpression::CaseInsensitiveOption);
            ddl.replace(nameRe, QStringLiteral("\\1") + dst);
            ok = !ddl.isEmpty() && execDdl(ddl);
            if(!ok)
                m_messages->appendPlainText(QStringLiteral("Duplicate Table failed: ") +
                                            (err.isEmpty() ? ddl : err));
        } else {
            ok = execDdl(QStringLiteral("CREATE TABLE %1 LIKE %2").arg(dst, src));
        }
        if(!ok)
            return;
    }
    if(wantData->isChecked())
        execDdl(QStringLiteral("INSERT INTO %1 SELECT * FROM %2").arg(dst, src));
}

void ConnectionTab::promptCopyDatabase(const QString &database)
{
    if(!m_conn)
        return;

    /* SQLite has no server/multi-database concept the MySQL dialog below
     * assumes (host/port/user, CREATE DATABASE, information_schema FKs) —
     * a SQLite "database" just *is* the file. "Copy" means: duplicate the
     * file (schema + data, in one shot — a plain file copy is not just an
     * equivalent to the MySQL path, it's more complete, since it carries
     * indexes/views/triggers with no per-object-type code needed), or
     * recreate just the schema (CREATE TABLE/VIEW from showCreate()) into
     * a fresh file when data isn't wanted. */
    if(m_params.driverType == SqlDriverType::Sqlite) {
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Copy Database"));
        auto *path = new QLineEdit(m_params.filePath + QStringLiteral("_copy.sqlite"), &dlg);
        auto *browse = new QPushButton(QStringLiteral("Browse…"), &dlg);
        connect(browse, &QPushButton::clicked, &dlg, [&] {
            /* unlike the "open/create a file" browse button on the SQLite
             * connect tab, picking a target *here* really does mean
             * overwrite — keep QFileDialog's default confirm-overwrite
             * prompt instead of suppressing it. */
            const QString p = QFileDialog::getSaveFileName(
                &dlg, QStringLiteral("Copy Database As"), path->text(),
                QStringLiteral("SQLite database (*.sqlite *.db *.sqlite3)"));
            if(!p.isEmpty())
                path->setText(p);
        });
        auto *pathRow = new QHBoxLayout;
        pathRow->addWidget(path, 1);
        pathRow->addWidget(browse);
        auto *wantData = new QCheckBox(QStringLiteral("Copy table data"), &dlg);
        wantData->setChecked(true);
        auto *form = new QFormLayout;
        form->addRow(QStringLiteral("Copy to file"), pathRow);
        form->addRow(QString(), wantData);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy"));
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        auto *lay = new QVBoxLayout(&dlg);
        lay->addLayout(form);
        lay->addWidget(buttons);
        if(dlg.exec() != QDialog::Accepted)
            return;

        const QString target = path->text().trimmed();
        if(target.isEmpty() || target == m_params.filePath)
            return;
        /* the Browse… button's own save dialog already confirms an
         * overwrite, but a path typed directly into the field skips that —
         * ask here too before the upcoming QFile::remove() */
        if(QFile::exists(target) &&
           QMessageBox::question(this, QStringLiteral("Copy Database"),
                                 QStringLiteral("%1 already exists. Overwrite it?").arg(target)) !=
               QMessageBox::Yes)
            return;

        QApplication::setOverrideCursor(Qt::WaitCursor);
        QString err;
        const bool ok = copySqliteFileTo(target, wantData->isChecked(), &err);
        QApplication::restoreOverrideCursor();
        m_messages->setPlainText(ok ? QStringLiteral("Copied database to %1").arg(target)
                                    : QStringLiteral("Copy failed:\n%1").arg(err));
        m_resultTabs->setCurrentWidget(m_messages);
        return;
    }

    const QString srcDb = database.isEmpty() ? defaultDb() : database;
    if(srcDb.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Copy Database"),
                                 QStringLiteral("Select a database first."));
        return;
    }

    /* Postgres has no cross-database browsing at all (see the schema/
     * database design note elsewhere in this file) — a connection can
     * never reach a sibling physical database the way the "different
     * target host" path below assumes, so "Copy Database" for Postgres is
     * always a same-connection schema-to-schema copy; the target
     * host/port/user fields (meaningless here) are omitted entirely rather
     * than shown and silently ignored. */
    const bool isPg = m_params.driverType == SqlDriverType::Postgres;

    QDialog dlg(this);
    dlg.setWindowTitle(
        QStringLiteral("Copy %1 `%2`")
            .arg(isPg ? QStringLiteral("Schema") : QStringLiteral("Database"), srcDb));
    auto *tHost = new QLineEdit(m_params.host, &dlg);
    auto *tPort = new QSpinBox(&dlg);
    tPort->setRange(1, 65535);
    tPort->setValue(m_params.port);
    tPort->setLocale(QLocale::c());
    auto *tUser = new QLineEdit(m_params.user, &dlg);
    auto *tPass = new QLineEdit(m_params.password, &dlg);
    tPass->setEchoMode(QLineEdit::Password);
    auto *name = new QLineEdit(srcDb + QStringLiteral("_copy"), &dlg);
    auto *wantData = new QCheckBox(QStringLiteral("Copy table data"), &dlg);
    wantData->setChecked(true);
    auto *dropFirst =
        new QCheckBox(QStringLiteral("Drop target %1 first if it exists")
                          .arg(isPg ? QStringLiteral("schema") : QStringLiteral("database")),
                      &dlg);
    auto *wantRoutines =
        new QCheckBox(QStringLiteral("Also copy views, routines, triggers, events"), &dlg);
    wantRoutines->setChecked(true);
    auto *form = new QFormLayout;
    if(!isPg) {
        form->addRow(QStringLiteral("Target host"), tHost);
        form->addRow(QStringLiteral("Target port"), tPort);
        form->addRow(QStringLiteral("Target user"), tUser);
        form->addRow(QStringLiteral("Target password"), tPass);
    }
    form->addRow(isPg ? QStringLiteral("New schema name") : QStringLiteral("New database name"),
                 name);
    form->addRow(QString(), wantData);
    form->addRow(QString(), wantRoutines);
    form->addRow(QString(), dropFirst);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(
        isPg ? QStringLiteral("Copies the schema within this connection "
                              "(CREATE TABLE … LIKE … INCLUDING ALL, plus foreign keys, "
                              "views, routines and triggers).")
             : QStringLiteral(
                   "Same host + port + user → fast CREATE … LIKE copy; a different target "
                   "streams a dump over a fresh connection. DEFINER clauses are stripped."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString tgt = name->text().trimmed();
    if(tgt.isEmpty())
        return;
    const bool sameServer =
        isPg || (tHost->text().trimmed() == m_params.host && tPort->value() == m_params.port &&
                 tUser->text().trimmed() == m_params.user);
    if(sameServer && tgt == srcDb) {
        QMessageBox::information(
            this, QStringLiteral("Copy Database"),
            QStringLiteral("Target must differ from the source on the same %1.")
                .arg(isPg ? QStringLiteral("connection") : QStringLiteral("server")));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    bool ok;
    if(isPg) {
        ok = copyDatabaseToPostgres(srcDb, tgt, wantData->isChecked(), dropFirst->isChecked(),
                                    wantRoutines->isChecked(), &err);
    } else if(sameServer) {
        ok = copyDatabaseTo(srcDb, tgt, wantData->isChecked(), dropFirst->isChecked(),
                            wantRoutines->isChecked(), &err);
    } else {
        ConnectionParams tp;
        tp.host = tHost->text().trimmed();
        tp.user = tUser->text().trimmed();
        tp.password = tPass->text();
        tp.port = tPort->value();
        IDbConnection *dst = dbDriverFor(SqlDriverType::Mysql)->connect(tp, &err);
        if(!dst) {
            err = QStringLiteral("target connect failed: %1").arg(err);
            ok = false;
        } else {
            const QString tq = QString(tgt).replace('`', QStringLiteral("``"));
            if(dropFirst->isChecked())
                dst->query(QStringLiteral("DROP DATABASE IF EXISTS `%1`").arg(tq), nullptr,
                           nullptr);
            dst->query(QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1` "
                                      "CHARACTER SET utf8mb4")
                           .arg(tq),
                       nullptr, nullptr);
            dst->query(QStringLiteral("USE `%1`").arg(tq), nullptr, nullptr);
            SqlDump::Options opt;
            opt.data = wantData->isChecked();
            opt.routines = wantRoutines->isChecked();
            ok = SqlDump::forEachStatement(
                m_conn, srcDb, {}, opt,
                [&](const QString &stmt) {
                    QString stmtError;
                    if(dst->query(stmt, nullptr, &stmtError))
                        return true;
                    err = QStringLiteral("%1\n  at: %2").arg(stmtError, stmt.left(120));
                    return false;
                },
                err.isEmpty() ? &err : nullptr);
            delete dst;
        }
    }
    QApplication::restoreOverrideCursor();

    m_messages->setPlainText(ok ? QStringLiteral("Copied `%1` → %2`%3`.")
                                      .arg(srcDb,
                                           sameServer ? QString()
                                                      : QStringLiteral("%1:%2/")
                                                            .arg(tHost->text().trimmed())
                                                            .arg(tPort->value()),
                                           tgt)
                                : QStringLiteral("Copy failed:\n%1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
    refreshBrowser();
}

bool ConnectionTab::copySqliteFileTo(const QString &target, bool withData, QString *error)
{
    if(!m_conn || m_params.driverType != SqlDriverType::Sqlite || target.isEmpty()) {
        if(error)
            *error = QStringLiteral("bad source/target");
        return false;
    }
    QFile::remove(target);
    if(withData) {
        if(QFile::copy(m_params.filePath, target))
            return true;
        if(error)
            *error = QStringLiteral("file copy failed");
        return false;
    }
    ConnectionParams tp;
    tp.driverType = SqlDriverType::Sqlite;
    tp.filePath = target;
    IDbConnection *dst = dbDriverFor(SqlDriverType::Sqlite)->connect(tp, error);
    if(!dst)
        return false;
    bool ok = true;
    for(const QString &kind : {QStringLiteral("TABLE"), QStringLiteral("VIEW")}) {
        if(!ok)
            break;
        for(const QString &name : m_conn->listTables(
                QStringLiteral("main"),
                kind == QStringLiteral("TABLE") ? QStringLiteral("BASE TABLE") : kind)) {
            QString ddlErr;
            const QString ddl = m_conn->showCreate(kind, QStringLiteral("main"), name, &ddlErr);
            if(ddl.isEmpty() || !dst->query(ddl, nullptr, error)) {
                ok = false;
                if(error && error->isEmpty())
                    *error = ddlErr;
                break;
            }
        }
    }
    delete dst;
    return ok;
}

void ConnectionTab::promptCopyTableToHost(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    /* the target here is always a fresh MySQL connection (see the dialog's
     * own label below) and the structure statement below is whatever
     * m_conn->showCreate() hands back — the *source's own* dialect. That's
     * fine when the source is MySQL too, but a Postgres/SQLite CREATE
     * TABLE (double-quoted identifiers, SERIAL/TEXT type differences, no
     * ENGINE=/CHARSET= clause, etc.) sent to a MySQL server the way this
     * always did would just fail with a syntax error rather than copying
     * anything — untested and unguarded until this fix. Unlike Copy
     * Database (which grew three genuinely separate per-driver code
     * paths — copyDatabaseTo()/copyDatabaseToPostgres()/the SQLite branch
     * in promptCopyDatabase()), a real cross-dialect DDL translator for
     * this one is a bigger job than a bug fix; guarding it is the same
     * defensive choice already made everywhere else in this file (Flush,
     * Show, Set Autocommit, …) rather than sending SQL nobody asked for. */
    if(m_params.driverType != SqlDriverType::Mysql) {
        QMessageBox::information(this, QStringLiteral("Copy Table To Different Host"),
                                 QStringLiteral("This only supports a MySQL/MariaDB source right "
                                                "now — the target is always MySQL, and copying "
                                                "a %1 table's structure across dialects isn't "
                                                "implemented. Use Database ▸ Copy Database or "
                                                "Backup Table(s) As SQL Dump instead.")
                                     .arg(m_params.driverType == SqlDriverType::Postgres
                                              ? QStringLiteral("PostgreSQL")
                                              : QStringLiteral("SQLite")));
        return;
    }
    const QString srcDb = database.isEmpty() ? defaultDb() : database;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Copy Table `%1` To Different Host/Database").arg(table));
    /* driver is guaranteed MySQL past the guard above, so this can just
     * prefill from the source tab's own connection */
    auto *tHost = new QLineEdit(m_params.host, &dlg);
    auto *tPort = new QSpinBox(&dlg);
    tPort->setRange(1, 65535);
    tPort->setValue(m_params.port);
    tPort->setLocale(QLocale::c());
    auto *tUser = new QLineEdit(m_params.user, &dlg);
    auto *tPass = new QLineEdit(m_params.password, &dlg);
    tPass->setEchoMode(QLineEdit::Password);
    auto *tDb = new QLineEdit(srcDb, &dlg);
    auto *wantData = new QCheckBox(QStringLiteral("Copy table data"), &dlg);
    wantData->setChecked(true);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Target host"), tHost);
    form->addRow(QStringLiteral("Target port"), tPort);
    form->addRow(QStringLiteral("Target user"), tUser);
    form->addRow(QStringLiteral("Target password"), tPass);
    form->addRow(QStringLiteral("Target database"), tDb);
    form->addRow(QString(), wantData);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(
        QStringLiteral("The table keeps its name on the target — copy, then rename there "
                       "if you need a different one. The target is always MySQL (there's "
                       "no \"host\" to speak of for a SQLite file)."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString tgtDb = tDb->text().trimmed();
    if(tgtDb.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    ConnectionParams tp;
    tp.driverType = SqlDriverType::Mysql;
    tp.host = tHost->text().trimmed();
    tp.port = tPort->value();
    tp.user = tUser->text().trimmed();
    tp.password = tPass->text();
    QString err;
    IDbConnection *dst = dbDriverFor(SqlDriverType::Mysql)->connect(tp, &err);
    bool ok = dst != nullptr;
    if(ok) {
        const QString tb = QString(tgtDb).replace('`', QStringLiteral("``"));
        dst->query(QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1`").arg(tb), nullptr, nullptr);
        dst->query(QStringLiteral("USE `%1`").arg(tb), nullptr, nullptr);
        SqlDump::Options opt;
        opt.data = wantData->isChecked();
        opt.routines = false;
        ok = SqlDump::forEachStatement(
            m_conn, srcDb, {table}, opt,
            [&](const QString &stmt) {
                QString stmtErr;
                if(dst->query(stmt, nullptr, &stmtErr))
                    return true;
                err = QStringLiteral("%1\n  at: %2").arg(stmtErr, stmt.left(120));
                return false;
            },
            err.isEmpty() ? &err : nullptr);
    } else {
        err = QStringLiteral("target connect failed: %1").arg(err);
    }
    delete dst;
    QApplication::restoreOverrideCursor();
    m_messages->setPlainText(
        ok ? QStringLiteral("Copied `%1` to %2:%3/`%4`").arg(table, tp.host).arg(tp.port).arg(tgtDb)
           : QStringLiteral("Copy failed:\n%1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
}

bool ConnectionTab::copyDatabaseTo(const QString &srcDb, const QString &tgtDb, bool withData,
                                   bool dropFirst, bool withRoutines, QString *error)
{
    if(!m_conn || srcDb.isEmpty() || tgtDb.isEmpty() || srcDb == tgtDb) {
        if(error)
            *error = QStringLiteral("bad source/target");
        return false;
    }
    const QString sb = QString(srcDb).replace('`', QStringLiteral("``"));

    /* one-row helper: run `sql`, return column `col` of the first row */
    const auto oneRow = [&](const QString &sql, int col) -> QString {
        DbResultSet rs;
        if(m_conn->query(sql, &rs, nullptr) && !rs.rows.isEmpty())
            return rs.rows.first().value(col);
        return {};
    };
    /* names from a single-column query */
    const auto nameList = [&](const QString &sql, int col) {
        QStringList out;
        DbResultSet rs;
        if(m_conn->query(sql, &rs, nullptr))
            for(const QStringList &row : rs.rows)
                out << row.value(col);
        return out;
    };
    static const QRegularExpression kDefiner(QStringLiteral("DEFINER=`[^`]*`@`[^`]*` "));
    const auto retarget = [&](QString ddl) {
        return ddl.remove(kDefiner).replace(QStringLiteral("`%1`.").arg(srcDb),
                                            QStringLiteral("`%1`.").arg(tgtDb));
    };

    const auto bq = [](QString s) { return s.replace('`', QStringLiteral("``")); };
    const QString tb = bq(tgtDb);

    /* base tables — a failing SHOW here (e.g. no such source db) must abort,
     * not silently "succeed" with an empty target */
    QStringList tables;
    {
        DbResultSet rs;
        if(!m_conn->query(
               QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='BASE TABLE'").arg(sb),
               &rs, error))
            return false;
        for(const QStringList &row : rs.rows)
            tables << row.value(0);
    }

    QStringList stmts;
    if(dropFirst)
        stmts << QStringLiteral("DROP DATABASE IF EXISTS `%1`").arg(tb);
    stmts << QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1`").arg(tb);
    stmts << QStringLiteral("SET FOREIGN_KEY_CHECKS=0");
    for(const QString &t : std::as_const(tables)) {
        const QString tq = bq(t);
        stmts << QStringLiteral("CREATE TABLE `%1`.`%2` LIKE `%3`.`%2`").arg(tb, tq, sb);
        if(withData)
            stmts
                << QStringLiteral("INSERT INTO `%1`.`%2` SELECT * FROM `%3`.`%2`").arg(tb, tq, sb);
    }

    /* CREATE TABLE … LIKE does not carry FK constraints (MariaDB) — copy them
     * explicitly from information_schema once every table exists (FK checks
     * are off, so create order is irrelevant) */
    {
        struct Fk
        {
            QString name, tbl, refTbl, onDel, onUpd;
            QStringList cols, refCols;
        };
        QList<Fk> fks;
        DbResultSet rs;
        if(m_conn->query(
               QStringLiteral("SELECT k.CONSTRAINT_NAME, k.TABLE_NAME, k.COLUMN_NAME, "
                              "k.REFERENCED_TABLE_NAME, k.REFERENCED_COLUMN_NAME, "
                              "r.DELETE_RULE, r.UPDATE_RULE "
                              "FROM information_schema.KEY_COLUMN_USAGE k "
                              "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
                              "  ON r.CONSTRAINT_SCHEMA=k.CONSTRAINT_SCHEMA "
                              "  AND r.CONSTRAINT_NAME=k.CONSTRAINT_NAME "
                              "WHERE k.TABLE_SCHEMA='%1' AND k.REFERENCED_TABLE_NAME IS NOT NULL "
                              "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION")
                   .arg(sb),
               &rs, nullptr)) {
            for(const QStringList &row : rs.rows) {
                const QString name = row.value(0);
                Fk *f = nullptr;
                for(auto &e : fks)
                    if(e.name == name && e.tbl == row.value(1)) {
                        f = &e;
                        break;
                    }
                if(!f) {
                    fks << Fk{name,
                              row.value(1),
                              row.value(3),
                              row.value(5) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT")
                                                                     : row.value(5),
                              row.value(6) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT")
                                                                     : row.value(6),
                              {},
                              {}};
                    f = &fks.last();
                }
                f->cols << row.value(2);
                f->refCols << row.value(4);
            }
        }
        for(const Fk &f : std::as_const(fks)) {
            const auto btlist = [&](const QStringList &l) {
                QStringList o;
                for(const QString &c : l)
                    o << QStringLiteral("`%1`").arg(bq(c));
                return o.join(QStringLiteral(", "));
            };
            stmts << QStringLiteral("ALTER TABLE `%1`.`%2` ADD CONSTRAINT `%3` FOREIGN KEY (%4) "
                                    "REFERENCES `%1`.`%5` (%6) ON DELETE %7 ON UPDATE %8")
                         .arg(tb, bq(f.tbl), bq(f.name), btlist(f.cols), bq(f.refTbl),
                              btlist(f.refCols), f.onDel, f.onUpd);
        }
    }

    if(withRoutines) {
        stmts << QStringLiteral("USE `%1`").arg(tb);

        for(const QString &v : nameList(
                QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='VIEW'").arg(sb), 0)) {
            QString ddl =
                retarget(oneRow(QStringLiteral("SHOW CREATE VIEW `%1`.`%2`").arg(srcDb, v), 1));
            ddl.replace(QStringLiteral(" VIEW `%1` ").arg(v),
                        QStringLiteral(" VIEW `%1`.`%2` ").arg(tgtDb, v));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        /* procedures + functions */
        struct R
        {
            QString name, type;
        };
        QList<R> routines;
        {
            DbResultSet rs;
            if(m_conn->query(QStringLiteral("SELECT ROUTINE_NAME, ROUTINE_TYPE FROM "
                                            "information_schema.ROUTINES WHERE ROUTINE_SCHEMA='%1'")
                                 .arg(sb),
                             &rs, nullptr))
                for(const QStringList &row : rs.rows)
                    routines << R{row.value(0), row.value(1)};
        }
        for(const R &rt : std::as_const(routines)) {
            const bool proc = rt.type == QStringLiteral("PROCEDURE");
            QString ddl = retarget(
                oneRow(QStringLiteral("SHOW CREATE %1 `%2`.`%3`")
                           .arg(proc ? QStringLiteral("PROCEDURE") : QStringLiteral("FUNCTION"),
                                srcDb, rt.name),
                       2));
            ddl.replace(QStringLiteral("%1 `%2`").arg(proc ? QStringLiteral("PROCEDURE")
                                                           : QStringLiteral("FUNCTION"),
                                                      rt.name),
                        QStringLiteral("%1 `%2`.`%3`")
                            .arg(proc ? QStringLiteral("PROCEDURE") : QStringLiteral("FUNCTION"),
                                 tgtDb, rt.name));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        /* triggers: SHOW TRIGGERS = Trigger,Event,Table,Statement,Timing,… */
        {
            DbResultSet rs;
            if(m_conn->query(QStringLiteral("SHOW TRIGGERS FROM `%1`").arg(sb), &rs, nullptr))
                for(const QStringList &row : rs.rows)
                    stmts << QStringLiteral("CREATE TRIGGER `%1`.`%2` %3 %4 ON `%1`.`%5` "
                                            "FOR EACH ROW %6")
                                 .arg(tgtDb, row.value(0), row.value(4), row.value(1), row.value(2),
                                      row.value(3));
        }

        for(const QString &e : nameList(QStringLiteral("SHOW EVENTS FROM `%1`").arg(sb), 1)) {
            QString ddl =
                retarget(oneRow(QStringLiteral("SHOW CREATE EVENT `%1`.`%2`").arg(srcDb, e), 3));
            ddl.replace(QStringLiteral(" EVENT `%1` ").arg(e),
                        QStringLiteral(" EVENT `%1`.`%2` ").arg(tgtDb, e));
            if(!ddl.isEmpty())
                stmts << ddl;
        }
    }

    stmts << QStringLiteral("SET FOREIGN_KEY_CHECKS=1");

    bool ok = true;
    for(const QString &s : std::as_const(stmts)) {
        QString stmtError;
        if(!m_conn->query(s, nullptr, &stmtError)) {
            if(error)
                *error = QStringLiteral("%1\n  at: %2").arg(stmtError, s);
            ok = false;
            break;
        }
    }
    m_conn->query(QStringLiteral("SET FOREIGN_KEY_CHECKS=1"), nullptr, nullptr);
    /* the "USE `tgt`" statement left the browsing connection on the target db;
     * put it back on this tab's database */
    if(!m_params.database.isEmpty())
        m_conn->query(QStringLiteral("USE `%1`").arg(m_params.database), nullptr, nullptr);
    return ok;
}

bool ConnectionTab::copyDatabaseToPostgres(const QString &srcSchema, const QString &tgtSchema,
                                           bool withData, bool dropFirst, bool withRoutines,
                                           QString *error)
{
    if(!m_conn || srcSchema.isEmpty() || tgtSchema.isEmpty() || srcSchema == tgtSchema) {
        if(error)
            *error = QStringLiteral("bad source/target");
        return false;
    }
    const QString sq = m_conn->quoteIdent(srcSchema);
    const QString tq = m_conn->quoteIdent(tgtSchema);

    /* blanket-retargets any *qualified* reference the catalog printed with
     * the source schema (mirrors copyDatabaseTo()'s backtick db-prefix
     * replace) to the target instead — pg_get_viewdef/functiondef/
     * triggerdef only double-quote an identifier that actually needs it,
     * so a plain lowercase schema name like this comes back *unquoted*
     * (`oy_src.t`, not `"oy_src".t`) far more often than not; matching only
     * the quoted spelling would silently leave the copy's views/functions/
     * triggers pointing at the *original* schema's objects instead of the
     * copies just created (same quoted-vs-bare lesson as the trigger-table
     * regex fix in SchemaSql.cpp). \b keeps a schema name from matching
     * inside a longer one (oy_src vs oy_src2). */
    const QRegularExpression schemaRef(
        QStringLiteral("(?:\"%1\"|\\b%1\\b)\\.").arg(QRegularExpression::escape(srcSchema)));
    const auto retarget = [&](QString ddl) {
        return ddl.replace(schemaRef, QStringLiteral("%1.").arg(tq));
    };

    const QStringList tables = m_conn->listTables(srcSchema, QStringLiteral("BASE TABLE"));

    QStringList stmts;
    if(dropFirst)
        stmts << QStringLiteral("DROP SCHEMA IF EXISTS %1 CASCADE").arg(tq);
    stmts << QStringLiteral("CREATE SCHEMA IF NOT EXISTS %1").arg(tq);
    for(const QString &t : tables) {
        const QString tt = m_conn->quoteIdent(t);
        /* LIKE ... INCLUDING ALL carries columns, defaults, NOT NULL,
         * identity, indexes, constraints and comments — but never foreign
         * keys, regardless of INCLUDING ALL (a documented Postgres LIKE
         * limitation); those are added explicitly below. A legacy `serial`
         * column (as opposed to this app's own GENERATED ... AS IDENTITY,
         * which copies cleanly with its own independent sequence) is a
         * plain integer DEFAULT nextval('src.seq'::regclass) — LIKE carries
         * that expression text verbatim, so the copy's column keeps
         * drawing values from the *source* schema's sequence rather than
         * getting an independent one; not fixed up here (narrow, only
         * affects externally-created tables using the legacy style). */
        stmts << QStringLiteral("CREATE TABLE %1.%2 (LIKE %3.%2 INCLUDING ALL)").arg(tq, tt, sq);
        if(withData)
            stmts << QStringLiteral("INSERT INTO %1.%2 SELECT * FROM %3.%2").arg(tq, tt, sq);
    }

    /* foreign keys: not carried by LIKE, added once every table exists.
     * Unlike the MySQL branch above, this can't reuse
     * information_schema.KEY_COLUMN_USAGE's REFERENCED_TABLE_NAME/
     * REFERENCED_COLUMN_NAME columns — those are a MySQL-only extension to
     * that view, absent from the ANSI-standard (and Postgres's) version —
     * so this reads pg_constraint directly instead, unnesting conkey/confkey
     * together (WITH ORDINALITY keeps each FK's column pairs and multi-
     * column order intact, same as ORDER BY ORDINAL_POSITION did above). */
    {
        struct Fk
        {
            QString name, tbl, refTbl, onDel, onUpd;
            QStringList cols, refCols;
        };
        QList<Fk> fks;
        DbResultSet rs;
        if(m_conn->query(
               QStringLiteral(
                   "SELECT con.conname, cl.relname, refcl.relname, "
                   "  CASE con.confdeltype WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' "
                   "    WHEN 'd' THEN 'SET DEFAULT' WHEN 'r' THEN 'RESTRICT' ELSE 'NO ACTION' END, "
                   "  CASE con.confupdtype WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' "
                   "    WHEN 'd' THEN 'SET DEFAULT' WHEN 'r' THEN 'RESTRICT' ELSE 'NO ACTION' END, "
                   "  a.attname, af.attname "
                   "FROM pg_constraint con "
                   "JOIN pg_class cl ON cl.oid=con.conrelid "
                   "JOIN pg_namespace n ON n.oid=cl.relnamespace "
                   "JOIN pg_class refcl ON refcl.oid=con.confrelid "
                   "JOIN unnest(con.conkey, con.confkey) WITH ORDINALITY AS u(ck, cfk, ord) ON "
                   "true "
                   "JOIN pg_attribute a ON a.attrelid=con.conrelid AND a.attnum=u.ck "
                   "JOIN pg_attribute af ON af.attrelid=con.confrelid AND af.attnum=u.cfk "
                   "WHERE con.contype='f' AND n.nspname='%1' "
                   "ORDER BY con.conname, u.ord")
                   .arg(srcSchema),
               &rs, nullptr)) {
            for(const QStringList &row : rs.rows) {
                const QString name = row.value(0);
                Fk *f = nullptr;
                for(auto &e : fks)
                    if(e.name == name && e.tbl == row.value(1)) {
                        f = &e;
                        break;
                    }
                if(!f) {
                    fks << Fk{name, row.value(1), row.value(2), row.value(3), row.value(4), {}, {}};
                    f = &fks.last();
                }
                f->cols << row.value(5);
                f->refCols << row.value(6);
            }
        }
        for(const Fk &f : std::as_const(fks)) {
            const auto qlist = [&](const QStringList &l) {
                QStringList o;
                for(const QString &c : l)
                    o << m_conn->quoteIdent(c);
                return o.join(QStringLiteral(", "));
            };
            stmts << QStringLiteral("ALTER TABLE %1.%2 ADD CONSTRAINT %3 FOREIGN KEY (%4) "
                                    "REFERENCES %1.%5 (%6) ON DELETE %7 ON UPDATE %8")
                         .arg(tq, m_conn->quoteIdent(f.tbl), m_conn->quoteIdent(f.name),
                              qlist(f.cols), m_conn->quoteIdent(f.refTbl), qlist(f.refCols),
                              f.onDel, f.onUpd);
        }
    }

    /* the connection's schema before this call, so the "SET search_path"
     * done below for view/routine/trigger creation can be put back after */
    const QString savedSearchPath =
        m_currentSchema.isEmpty() ? QStringLiteral("public") : m_currentSchema;

    if(withRoutines) {
        /* narrow search_path to just the target for this section, mirroring
         * copyDatabaseTo()'s "USE `tgt`" line — see the retarget() comment */
        stmts << QStringLiteral("SET search_path TO %1").arg(tq);

        for(const QString &v : m_conn->listTables(srcSchema, QStringLiteral("VIEW"))) {
            QString err;
            const QString ddl =
                retarget(m_conn->showCreate(QStringLiteral("VIEW"), srcSchema, v, &err));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        for(const QString &kind : {QStringLiteral("FUNCTION"), QStringLiteral("PROCEDURE")}) {
            DbResultSet rs;
            if(m_conn->query(QStringLiteral("SELECT ROUTINE_NAME FROM information_schema.ROUTINES "
                                            "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_TYPE='%2'")
                                 .arg(srcSchema, kind),
                             &rs, nullptr)) {
                for(const QStringList &row : rs.rows) {
                    QString err;
                    const QString ddl =
                        retarget(m_conn->showCreate(kind, srcSchema, row.value(0), &err));
                    if(!ddl.isEmpty())
                        stmts << ddl;
                }
            }
        }

        /* trigger *functions* are ordinary pg_proc entries, already copied
         * by the FUNCTION loop above (Postgres has no separate "trigger
         * body" object) — this only needs the CREATE TRIGGER statements
         * themselves, which reference them by the now-copied name */
        for(const QString &tr : m_conn->listTriggers(srcSchema)) {
            QString err;
            const QString ddl =
                retarget(m_conn->showCreate(QStringLiteral("TRIGGER"), srcSchema, tr, &err));
            if(!ddl.isEmpty())
                stmts << ddl;
        }
    }

    bool ok = true;
    for(const QString &s : std::as_const(stmts)) {
        QString stmtError;
        if(!m_conn->query(s, nullptr, &stmtError)) {
            if(error)
                *error = QStringLiteral("%1\n  at: %2").arg(stmtError, s);
            ok = false;
            break;
        }
    }
    if(withRoutines)
        m_conn->query(
            QStringLiteral("SET search_path TO %1").arg(m_conn->quoteIdent(savedSearchPath)),
            nullptr, nullptr);
    return ok;
}
