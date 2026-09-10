#include "SchemaSql.h"

#include <QRegularExpression>

namespace SchemaSql
{

QString stripDefiner(const QString &ddl)
{
    QString s = ddl;
    /* DEFINER=`user`@`host`  (the usual SHOW CREATE form) */
    s.remove(QRegularExpression(
        QStringLiteral("DEFINER\\s*=\\s*`[^`]*`@`[^`]*`\\s*")));
    /* fallback: DEFINER=user@host with no backticks */
    s.remove(QRegularExpression(
        QStringLiteral("DEFINER\\s*=\\s*[^ \\t]+@[^ \\t]+\\s*")));
    return s;
}

QString createTemplate(const QString &objType, const QString &db)
{
    const QString d = QString(db).replace('`', QStringLiteral("``"));
    if(objType == QStringLiteral("VIEW"))
        return QStringLiteral("CREATE VIEW `%1`.`new_view` AS\nSELECT 1 AS n;").arg(d);
    if(objType == QStringLiteral("PROCEDURE"))
        return QStringLiteral(
            "CREATE PROCEDURE `%1`.`new_proc`(IN arg1 INT)\nBEGIN\n  \nEND").arg(d);
    if(objType == QStringLiteral("FUNCTION"))
        return QStringLiteral(
            "CREATE FUNCTION `%1`.`new_func`(arg1 INT)\n  RETURNS INT\n"
            "  DETERMINISTIC\nBEGIN\n  RETURN arg1;\nEND").arg(d);
    if(objType == QStringLiteral("TRIGGER"))
        return QStringLiteral(
            "CREATE TRIGGER `%1`.`new_trigger`\n  BEFORE INSERT ON `some_table`\n"
            "  FOR EACH ROW\nBEGIN\n  \nEND").arg(d);
    if(objType == QStringLiteral("EVENT"))
        return QStringLiteral(
            "CREATE EVENT `%1`.`new_event`\n  ON SCHEDULE EVERY 1 DAY\n"
            "  DO\nBEGIN\n  \nEND").arg(d);
    return QString();
}

int showCreateColumn(const QString &objType)
{
    if(objType == QStringLiteral("VIEW"))  return 1;
    if(objType == QStringLiteral("EVENT")) return 3;
    return 2;   /* PROCEDURE / FUNCTION / TRIGGER */
}

QString editorText(const QString &objType, const QString &db, const QString &name,
                   const QString &createSql, bool create)
{
    const QString d = QString(db).replace('`', QStringLiteral("``"));
    const QString n = QString(name).replace('`', QStringLiteral("``"));
    QString body = createSql.trimmed();
    /* drop a trailing ; the SHOW CREATE / template may carry */
    while(body.endsWith(QLatin1Char(';')))
        body.chop(1);

    if(objType == QStringLiteral("VIEW")) {
        if(!create) {
            static const QRegularExpression lead(
                QStringLiteral("^\\s*CREATE\\s+(?:OR\\s+REPLACE\\s+)?"),
                QRegularExpression::CaseInsensitiveOption);
            body.replace(lead, QStringLiteral("CREATE OR REPLACE "));
        }
        return body + QLatin1Char(';') + QLatin1Char('\n');
    }

    /* routines / triggers / events: compound body → DELIMITER, and an alter
     * drops the old object first */
    QString out = QStringLiteral("DELIMITER $$\n\n");
    if(!create)
        out += QStringLiteral("DROP %1 IF EXISTS `%2`.`%3`$$\n\n")
                   .arg(objType, d, n);
    out += body + QStringLiteral("$$\n\nDELIMITER ;\n");
    return out;
}

} // namespace SchemaSql
