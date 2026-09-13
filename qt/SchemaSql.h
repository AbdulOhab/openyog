/* OpenYog — DDL helpers for the schema-object editors (Create/Alter View,
 * Stored Procedure, Function, Trigger, Event).
 *
 * SQLyog opens these in a new query-editor tab (not a modal dialog): the tab
 * is seeded with a CREATE skeleton (new) or the object's SHOW CREATE DDL with
 * DEFINER stripped (alter), the user edits, then Executes.  MySQL has no ALTER
 * for routines/triggers/events, so "alter" text is DROP … IF EXISTS + CREATE,
 * wrapped in DELIMITER $$ … $$ so the editor's statement splitter keeps the
 * compound body intact.
 *
 * PostgreSQL differs enough to need its own branches throughout: identifiers
 * are double-quoted, not backtick-quoted; FUNCTION/PROCEDURE bodies are
 * dollar-quoted ($body$ … $body$, chosen to never collide with the DELIMITER
 * $$ wrapper around them) and pg_get_functiondef() already returns a
 * ready-to-run "CREATE OR REPLACE" statement, so an alter just runs it as-is
 * (no DROP needed, unlike MySQL); and a TRIGGER is two objects, not one — the
 * trigger itself plus the function it calls — so its CREATE template is two
 * statements (still one DELIMITER-wrapped chunk: libpq's PQexec natively runs
 * a semicolon-separated multi-statement string as one implicit transaction),
 * and altering one needs its table name (Postgres's DROP TRIGGER syntax is
 * "DROP TRIGGER name ON table", not "DROP TRIGGER db.name") pulled out of the
 * DDL text itself via editorText()'s own regex, since ConnectionTab's
 * alterSchemaObject() has no table parameter to pass in directly. PostgreSQL
 * has no CREATE EVENT equivalent at all — ConnectionTab guards that case
 * before ever calling in here, the same way it already guards other
 * SQLite/PostgreSQL capability gaps. */
#pragma once

#include "ConnectionParams.h"

#include <QString>

namespace SchemaSql
{
/* strip a leading `DEFINER=`user`@`host`` clause from a SHOW CREATE result
 * (a MySQL-only concept; a no-op on other backends' DDL text) */
QString stripDefiner(const QString &ddl);

/* a CREATE skeleton for a new object. objType: VIEW / PROCEDURE / FUNCTION /
 * TRIGGER / EVENT (EVENT is MySQL-only — callers must not reach here with
 * it for another driver) */
QString createTemplate(const QString &objType, const QString &db,
                       DriverType driver = DriverType::Mysql);

/* index of the "Create …" column in MySQL's SHOW CREATE <obj> (VIEW 1,
 * EVENT 3, PROCEDURE/FUNCTION/TRIGGER 2) — MySQL-only, unused by the
 * showCreate()-based path other drivers go through */
int showCreateColumn(const QString &objType);

/* full editor-tab text.  create==true → just the CREATE (template or supplied).
 * create==false → an alter: views get CREATE OR REPLACE on every driver;
 * MySQL/SQLite routines/triggers get DROP … IF EXISTS + CREATE inside
 * DELIMITER $$; PostgreSQL functions/procedures run their already-"CREATE OR
 * REPLACE" DDL as-is, and PostgreSQL triggers get DROP TRIGGER IF EXISTS
 * name ON <table pulled from the DDL> + CREATE TRIGGER. `createSql` is the
 * body (template for new, stripped SHOW CREATE / showCreate() text for
 * alter). */
QString editorText(const QString &objType, const QString &db, const QString &name,
                   const QString &createSql, bool create,
                   DriverType driver = DriverType::Mysql);
} // namespace SchemaSql
