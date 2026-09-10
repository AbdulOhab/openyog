/* OpenYog — DDL helpers for the schema-object editors (Create/Alter View,
 * Stored Procedure, Function, Trigger, Event).
 *
 * SQLyog opens these in a new query-editor tab (not a modal dialog): the tab
 * is seeded with a CREATE skeleton (new) or the object's SHOW CREATE DDL with
 * DEFINER stripped (alter), the user edits, then Executes.  MySQL has no ALTER
 * for routines/triggers/events, so "alter" text is DROP … IF EXISTS + CREATE,
 * wrapped in DELIMITER $$ … $$ so the editor's statement splitter keeps the
 * compound body intact. */
#pragma once

#include <QString>

namespace SchemaSql
{
/* strip a leading `DEFINER=`user`@`host`` clause from a SHOW CREATE result */
QString stripDefiner(const QString &ddl);

/* a CREATE skeleton for a new object. objType: VIEW / PROCEDURE / FUNCTION /
 * TRIGGER / EVENT */
QString createTemplate(const QString &objType, const QString &db);

/* index of the "Create …" column in SHOW CREATE <obj> (VIEW 1, EVENT 3,
 * PROCEDURE/FUNCTION/TRIGGER 2) */
int showCreateColumn(const QString &objType);

/* full editor-tab text.  create==true → just the CREATE (template or supplied).
 * create==false → an alter: views get CREATE OR REPLACE; the rest get
 * DROP … IF EXISTS + CREATE inside DELIMITER $$. `createSql` is the body
 * (template for new, stripped SHOW CREATE for alter). */
QString editorText(const QString &objType, const QString &db, const QString &name,
                   const QString &createSql, bool create);
} // namespace SchemaSql
