/* OpenYog — unit tests for qt/SchemaSync: reading each dialect's CREATE
 * statements, what differs between two schemas, and the script that makes
 * the target match. */
#include "SchemaSync.h"

#include <QTest>

using namespace SchemaSync;

namespace {

const Change *find(const QList<Change> &changes, Action action, const QString &table,
                   const QString &name = {})
{
    for(const Change &c : changes)
        if(c.action == action && c.table == table && c.name == name)
            return &c;
    return nullptr;
}

QString kSourceEmployees =
    QStringLiteral("CREATE TABLE `employees` (\n"
                   "  `id` int NOT NULL AUTO_INCREMENT,\n"
                   "  `name` varchar(100) NOT NULL,\n"
                   "  `salary` decimal(10,2) DEFAULT NULL,\n"
                   "  `dept_id` int DEFAULT NULL,\n"
                   "  `hired` date DEFAULT NULL,\n"
                   "  `photo` blob,\n"
                   "  PRIMARY KEY (`id`),\n"
                   "  KEY `idx_employees_city` (`name`),\n"
                   "  KEY `fk_emp_dept` (`dept_id`),\n"
                   "  CONSTRAINT `fk_emp_dept` FOREIGN KEY (`dept_id`) REFERENCES `emp_dept` "
                   "(`id`) ON DELETE SET NULL,\n"
                   "  CONSTRAINT `chk_salary` CHECK ((`salary` >= 0))\n"
                   ") ENGINE=InnoDB AUTO_INCREMENT=5 DEFAULT CHARSET=utf8mb4");

QString kTargetEmployees =
    QStringLiteral("CREATE TABLE `employees` (\n"
                   "  `id` int(11) NOT NULL AUTO_INCREMENT,\n"
                   "  `name` varchar(100) NOT NULL,\n"
                   "  `salary` decimal(8,2) DEFAULT NULL,\n"
                   "  `dept_id` int(11) DEFAULT NULL,\n"
                   "  `hired` date DEFAULT NULL,\n"
                   "  `legacy` text,\n"
                   "  PRIMARY KEY (`id`),\n"
                   "  KEY `idx_name` (`name`),\n"
                   "  KEY `fk_emp_dept` (`dept_id`),\n"
                   "  CONSTRAINT `chk_salary` CHECK ((`salary` >= 0))\n"
                   ") ENGINE=InnoDB AUTO_INCREMENT=812 DEFAULT CHARSET=utf8mb4");

} // namespace

class TestSchemaSync : public QObject
{
    Q_OBJECT
private slots:
    void readsMySql()
    {
        const Table t = parseMySql("employees", kSourceEmployees);
        QCOMPARE(t.columns.size(), 6);
        QCOMPARE(t.columns[2].name, QStringLiteral("salary"));
        QCOMPARE(t.columns[2].definition, QStringLiteral("`salary` decimal(10,2) DEFAULT NULL"));
        QCOMPARE(t.keys.size(), 4);
        QCOMPARE(t.keys[0].name, QStringLiteral("PRIMARY"));
        QCOMPARE(t.keys[1].name, QStringLiteral("idx_employees_city"));
        QCOMPARE(t.keys[3].name, QStringLiteral("chk_salary"));
        QCOMPARE(t.foreignKeys.size(), 1);
        QCOMPARE(t.foreignKeys[0].name, QStringLiteral("fk_emp_dept"));
        QCOMPARE(t.options, QStringLiteral("ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"));
        QVERIFY(!t.create.contains("AUTO_INCREMENT=5"));
    }

    void mySqlChangesAndScript()
    {
        Schema source{SqlDriverType::Mysql, "shop", {}};
        source.tables << parseMySql("employees", kSourceEmployees)
                      << parseMySql("emp_dept", "CREATE TABLE `emp_dept` (\n"
                                                "  `id` int NOT NULL,\n"
                                                "  PRIMARY KEY (`id`)\n"
                                                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        Schema target{SqlDriverType::Mysql, "shop_copy", {}};
        target.tables << parseMySql("employees", kTargetEmployees)
                      << parseMySql("old_stuff", "CREATE TABLE `old_stuff` (\n"
                                                 "  `x` int DEFAULT NULL\n"
                                                 ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        const QList<Change> changes = compare(source, target);

        /* int(11) against int, and AUTO_INCREMENT=, are no difference */
        QVERIFY(!find(changes, Action::AlterColumn, "employees", "id"));
        QVERIFY(!find(changes, Action::AlterColumn, "employees", "dept_id"));
        QVERIFY(!find(changes, Action::AlterOptions, "employees"));

        const Change *salary = find(changes, Action::AlterColumn, "employees", "salary");
        QVERIFY(salary);
        QCOMPARE(salary->sql,
                 QStringList(
                     "ALTER TABLE `employees` MODIFY COLUMN `salary` decimal(10,2) DEFAULT NULL"));
        const Change *photo = find(changes, Action::AddColumn, "employees", "photo");
        QVERIFY(photo);
        QCOMPARE(photo->sql,
                 QStringList("ALTER TABLE `employees` ADD COLUMN `photo` blob AFTER `hired`"));
        const Change *legacy = find(changes, Action::DropColumn, "employees", "legacy");
        QVERIFY(legacy && !legacy->pickedByDefault);
        QCOMPARE(legacy->sql, QStringList("ALTER TABLE `employees` DROP COLUMN `legacy`"));
        QCOMPARE(find(changes, Action::AddKey, "employees", "idx_employees_city")->sql,
                 QStringList("ALTER TABLE `employees` ADD KEY `idx_employees_city` (`name`)"));
        QCOMPARE(find(changes, Action::DropKey, "employees", "idx_name")->sql,
                 QStringList("ALTER TABLE `employees` DROP INDEX `idx_name`"));
        QCOMPARE(find(changes, Action::AddForeignKey, "employees", "fk_emp_dept")->sql,
                 QStringList("ALTER TABLE `employees` ADD CONSTRAINT `fk_emp_dept` FOREIGN KEY "
                             "(`dept_id`) REFERENCES `emp_dept` (`id`) ON DELETE SET NULL"));
        const Change *create = find(changes, Action::CreateTable, "emp_dept");
        QVERIFY(create && create->pickedByDefault);
        const Change *drop = find(changes, Action::DropTable, "old_stuff");
        QVERIFY(drop && !drop->pickedByDefault);
        QCOMPARE(drop->sql, QStringList("DROP TABLE `old_stuff`"));
        QCOMPARE(changes.size(), 8);

        const QString sql = script(changes, source, target);
        QVERIFY(sql.contains("USE `shop_copy`;\nSET FOREIGN_KEY_CHECKS = 0;"));
        QVERIFY(sql.trimmed().endsWith("SET FOREIGN_KEY_CHECKS = 1;"));
        /* drops before adds; the missing table before the key pointing at it */
        QVERIFY(sql.indexOf("DROP INDEX `idx_name`") < sql.indexOf("ADD COLUMN `photo`"));
        QVERIFY(sql.indexOf("DROP COLUMN `legacy`") < sql.indexOf("MODIFY COLUMN `salary`"));
        QVERIFY(sql.indexOf("CREATE TABLE `emp_dept`") <
                sql.indexOf("ADD CONSTRAINT `fk_emp_dept`"));
        QVERIFY(sql.contains("-- employees: add column photo\n"));
    }

    void alteredKeysDropFirst()
    {
        Schema source{SqlDriverType::Mysql, "a", {}}, target{SqlDriverType::Mysql, "b", {}};
        source.tables << parseMySql("t", "CREATE TABLE `t` (\n  `x` int,\n  `y` int,\n"
                                         "  UNIQUE KEY `k` (`x`,`y`)\n) ENGINE=InnoDB");
        target.tables << parseMySql("t", "CREATE TABLE `t` (\n  `x` int,\n  `y` int,\n"
                                         "  KEY `k` (`x`)\n) ENGINE=InnoDB");
        const QList<Change> changes = compare(source, target);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0].action, Action::AlterKey);
        QCOMPARE(changes[0].prepare, QStringList("ALTER TABLE `t` DROP INDEX `k`"));
        QCOMPARE(changes[0].sql, QStringList("ALTER TABLE `t` ADD UNIQUE KEY `k` (`x`,`y`)"));
        const QString sql = script(changes, source, target);
        QVERIFY(sql.indexOf("DROP INDEX `k`") < sql.indexOf("ADD UNIQUE KEY"));
    }

    void postgresAcrossSchemas()
    {
        /* PostgresConnection::showCreate("TABLE") / ("FOREIGN KEYS") */
        const QString create =
            "CREATE TABLE \"public\".\"employees\" (\n"
            "  \"id\" serial NOT NULL,\n"
            "  \"name\" text NOT NULL,\n"
            "  \"salary\" numeric(10,2) DEFAULT 0,\n"
            "  \"dept_id\" integer,\n"
            "  PRIMARY KEY (\"id\"),\n"
            "  CONSTRAINT \"employees_name_key\" UNIQUE (name)\n"
            ");\n"
            "CREATE INDEX idx_employees_city ON public.employees USING btree (name)";
        const QString fks =
            "ALTER TABLE \"public\".\"employees\" ADD CONSTRAINT \"employees_dept_id_fkey\" "
            "FOREIGN KEY (dept_id) REFERENCES emp_dept(id) ON DELETE SET NULL";
        const Table s = parsePostgres("employees", create, fks);
        QCOMPARE(s.columns.size(), 4);
        QCOMPARE(s.columns[1].definition, QStringLiteral("\"name\" text NOT NULL"));
        QCOMPARE(s.keys.size(), 3);
        QCOMPARE(s.keys[1].name, QStringLiteral("employees_name_key"));
        QCOMPARE(s.keys[2].name, QStringLiteral("idx_employees_city"));
        QCOMPARE(s.foreignKeys.size(), 1);
        QCOMPARE(
            s.foreignKeys[0].definition,
            QStringLiteral("FOREIGN KEY (dept_id) REFERENCES emp_dept(id) ON DELETE SET NULL"));

        Schema source{SqlDriverType::Postgres, "public", {s}};
        /* the target: the table without its index and key, salary text and
         * nullable name, plus the same unique key written for its schema */
        const Table t = parsePostgres("employees",
                                      "CREATE TABLE \"staging\".\"employees\" (\n"
                                      "  \"id\" serial NOT NULL,\n"
                                      "  \"name\" text,\n"
                                      "  \"salary\" text,\n"
                                      "  \"dept_id\" integer,\n"
                                      "  PRIMARY KEY (\"id\"),\n"
                                      "  CONSTRAINT \"employees_name_key\" UNIQUE (name)\n"
                                      ")",
                                      {});
        Schema target{SqlDriverType::Postgres, "staging", {t}};
        const QList<Change> changes = compare(source, target);

        QCOMPARE(find(changes, Action::AlterColumn, "employees", "name")->sql,
                 QStringList(
                     "ALTER TABLE \"staging\".\"employees\" ALTER COLUMN \"name\" SET NOT NULL"));
        QCOMPARE(find(changes, Action::AlterColumn, "employees", "salary")->sql,
                 QStringList({"ALTER TABLE \"staging\".\"employees\" ALTER COLUMN \"salary\" TYPE "
                              "numeric(10,2) USING \"salary\"::numeric(10,2)",
                              "ALTER TABLE \"staging\".\"employees\" ALTER COLUMN \"salary\" SET "
                              "DEFAULT 0"}));
        QCOMPARE(
            find(changes, Action::AddKey, "employees", "idx_employees_city")->sql,
            QStringList(
                "CREATE INDEX idx_employees_city ON \"staging\".employees USING btree (name)"));
        /* the referenced table qualified with the target schema */
        QCOMPARE(find(changes, Action::AddForeignKey, "employees", "employees_dept_id_fkey")->sql,
                 QStringList("ALTER TABLE \"staging\".\"employees\" ADD CONSTRAINT "
                             "\"employees_dept_id_fkey\" FOREIGN KEY (dept_id) REFERENCES "
                             "\"staging\".emp_dept(id) ON DELETE SET NULL"));
        QCOMPARE(changes.size(), 4);

        /* a missing table is created in the target schema */
        Schema empty{SqlDriverType::Postgres, "staging", {}};
        const QList<Change> create2 = compare(source, empty);
        QCOMPARE(create2.size(), 1);
        QCOMPARE(create2[0].sql.size(), 2);
        QVERIFY(create2[0].sql[0].startsWith("CREATE TABLE \"staging\".\"employees\" ("));
        QCOMPARE(
            create2[0].sql[1],
            QStringLiteral(
                "CREATE INDEX idx_employees_city ON \"staging\".employees USING btree (name)"));
        const QString sql = script(create2, source, empty);
        QVERIFY(sql.contains("BEGIN;"));
        QVERIFY(sql.trimmed().endsWith("COMMIT;"));

        /* the primary key's constraint by PostgreSQL's default name */
        Schema noPk{
            SqlDriverType::Postgres,
            "public",
            {parsePostgres("t", "CREATE TABLE \"public\".\"t\" (\n  \"a\" integer\n)", {})}};
        Schema withPk{SqlDriverType::Postgres,
                      "public",
                      {parsePostgres("t",
                                     "CREATE TABLE \"public\".\"t\" (\n  \"a\" integer NOT NULL,\n"
                                     "  PRIMARY KEY (\"a\")\n)",
                                     {})}};
        QCOMPARE(find(compare(noPk, withPk), Action::DropKey, "t", "PRIMARY")->sql,
                 QStringList("ALTER TABLE \"public\".\"t\" DROP CONSTRAINT \"t_pkey\""));
    }

    void sqliteWhatItCanAndCant()
    {
        const Table s = parseSqlite("employees",
                                    "CREATE TABLE employees (\n"
                                    "  id INTEGER PRIMARY KEY,\n"
                                    "  name TEXT NOT NULL,\n"
                                    "  city TEXT,\n"
                                    "  salary REAL CHECK (salary >= 0),\n"
                                    "  dept_id INTEGER REFERENCES emp_dept(id)\n"
                                    ")",
                                    {"CREATE INDEX idx_employees_city ON employees(city)"});
        QCOMPARE(s.columns.size(), 5);
        QCOMPARE(s.columns[3].definition, QStringLiteral("salary REAL CHECK (salary >= 0)"));
        QCOMPARE(s.keys.size(), 1);
        QCOMPARE(s.keys[0].name, QStringLiteral("idx_employees_city"));
        QVERIFY(s.create.endsWith("CREATE INDEX idx_employees_city ON employees(city)"));

        const Table t = parseSqlite("employees",
                                    "CREATE TABLE \"employees\" (\n"
                                    "  id INTEGER PRIMARY KEY,\n"
                                    "  name TEXT,\n"
                                    "  salary REAL CHECK (salary >= 0),\n"
                                    "  dept_id INTEGER REFERENCES emp_dept(id),\n"
                                    "  FOREIGN KEY (dept_id) REFERENCES emp_dept(id)\n"
                                    ")",
                                    {"CREATE INDEX \"idx_old\" ON employees(name)"});
        QCOMPARE(t.foreignKeys.size(), 1);

        Schema source{SqlDriverType::Sqlite, "main", {s}},
            target{SqlDriverType::Sqlite, "main", {t}};
        const QList<Change> changes = compare(source, target);
        QCOMPARE(find(changes, Action::AddColumn, "employees", "city")->sql,
                 QStringList("ALTER TABLE \"employees\" ADD COLUMN city TEXT"));
        const Change *name = find(changes, Action::AlterColumn, "employees", "name");
        QVERIFY(name && name->sql.isEmpty() && name->note.contains("rebuilt"));
        QCOMPARE(find(changes, Action::AddKey, "employees", "idx_employees_city")->sql,
                 QStringList("CREATE INDEX idx_employees_city ON employees(city)"));
        QCOMPARE(find(changes, Action::DropKey, "employees", "idx_old")->sql,
                 QStringList("DROP INDEX \"idx_old\""));
        const Change *fk = find(changes, Action::DropForeignKey, "employees", "(foreign key 1)");
        QVERIFY(fk && fk->sql.isEmpty() && !fk->note.isEmpty());

        const QString sql = script(changes, source, target);
        QVERIFY(sql.contains("-- Not done here:\n--   employees: change column name ("));
    }

    void mySqlObjects()
    {
        Schema source, target;
        source.database = "shop";
        target.database = "shop_copy";
        /* the same view, but for its definer and the database naming its tables */
        source.objects << Object{"VIEW", "v_same",
                                 "CREATE ALGORITHM=UNDEFINED DEFINER=`root`@`%` SQL SECURITY "
                                 "DEFINER VIEW `v_same` AS select `shop`.`t`.`a` AS `a` from "
                                 "`shop`.`t`"};
        target.objects << Object{"VIEW", "v_same",
                                 "CREATE ALGORITHM=UNDEFINED DEFINER=`app`@`localhost` SQL "
                                 "SECURITY DEFINER VIEW `v_same` AS select `shop_copy`.`t`.`a` AS "
                                 "`a` from `shop_copy`.`t`"};
        source.objects << Object{"VIEW", "v_changed",
                                 "CREATE DEFINER=`root`@`%` VIEW `v_changed` AS select 2 AS `x`"};
        target.objects << Object{"VIEW", "v_changed",
                                 "CREATE DEFINER=`root`@`%` VIEW `v_changed` AS select 1 AS `x`"};
        source.objects << Object{"PROCEDURE", "p_new",
                                 "CREATE DEFINER=`root`@`%` PROCEDURE `p_new`()\nBEGIN\n  SELECT "
                                 "1;\n  SELECT 2;\nEND"};
        source.objects << Object{"TRIGGER", "trg",
                                 "CREATE DEFINER=`root`@`%` TRIGGER `trg` BEFORE INSERT ON `t` FOR "
                                 "EACH ROW SET NEW.a = 1"};
        target.objects << Object{"TRIGGER", "trg",
                                 "CREATE DEFINER=`root`@`%` TRIGGER `trg` BEFORE INSERT ON `t` FOR "
                                 "EACH ROW SET NEW.a = 2"};
        /* a function and a procedure don't share names */
        target.objects << Object{"FUNCTION", "p_new",
                                 "CREATE FUNCTION `p_new`() RETURNS int RETURN 1"};

        const QList<Change> changes = compare(source, target);
        QCOMPARE(changes.size(), 4); /* not v_same */
        const Change *view = find(changes, Action::AlterObject, "v_changed");
        QVERIFY(view);
        QCOMPARE(view->kind, QStringLiteral("VIEW"));
        QCOMPARE(view->sql, QStringList{"CREATE OR REPLACE VIEW `v_changed` AS select 2 AS `x`"});
        QVERIFY(view->prepare.isEmpty());
        QCOMPARE(describe(*view), QStringLiteral("change view"));

        const Change *proc = find(changes, Action::CreateObject, "p_new");
        QVERIFY(proc && proc->compound && proc->pickedByDefault);
        const Change *trigger = find(changes, Action::AlterObject, "trg");
        QVERIFY(trigger);
        QCOMPARE(trigger->prepare, QStringList{"DROP TRIGGER IF EXISTS `trg`"});
        const Change *function = find(changes, Action::DropObject, "p_new");
        QVERIFY(function && !function->pickedByDefault);
        QCOMPARE(function->sql, QStringList{"DROP FUNCTION IF EXISTS `p_new`"});

        const QString sql = script(changes, source, target);
        /* drops first; then routines, views, triggers; compound bodies whole */
        QVERIFY(sql.indexOf("DROP FUNCTION") < sql.indexOf("CREATE PROCEDURE"));
        QVERIFY(sql.indexOf("DROP TRIGGER") < sql.indexOf("CREATE PROCEDURE"));
        QVERIFY(sql.indexOf("CREATE PROCEDURE") < sql.indexOf("CREATE OR REPLACE VIEW"));
        QVERIFY(sql.indexOf("CREATE OR REPLACE VIEW") < sql.indexOf("CREATE TRIGGER"));
        QVERIFY2(sql.contains("DELIMITER $$\nCREATE PROCEDURE `p_new`()\nBEGIN\n  SELECT 1;\n  "
                              "SELECT 2;\nEND\n$$\nDELIMITER ;"),
                 qPrintable(sql));
        QVERIFY(!sql.contains("DEFINER=`"));
    }

    void viewsAfterTheViewsTheyRead()
    {
        const QList<Object> views =
            orderViews({{"VIEW", "a_top", "CREATE VIEW a_top AS SELECT * FROM m_mid"},
                        {"VIEW", "m_mid", "CREATE VIEW m_mid AS SELECT * FROM \"z_base\""},
                        {"VIEW", "z_base", "CREATE VIEW z_base AS SELECT 1 AS m_middle"},
                        {"VIEW", "loop1", "CREATE VIEW loop1 AS SELECT * FROM loop2"},
                        {"VIEW", "loop2", "CREATE VIEW loop2 AS SELECT * FROM loop1"}});
        QStringList names;
        for(const Object &v : views)
            names << v.name;
        QCOMPARE(names.size(), 5);
        QVERIFY(names.indexOf("z_base") < names.indexOf("m_mid"));
        QVERIFY(names.indexOf("m_mid") < names.indexOf("a_top"));
    }

    void postgresObjectsAcrossSchemas()
    {
        Schema source, target;
        source.driver = target.driver = SqlDriverType::Postgres;
        source.database = "public";
        target.database = "sync_target";
        source.objects << Object{"FUNCTION", "f",
                                 "CREATE OR REPLACE FUNCTION public.f()\n RETURNS trigger\n "
                                 "LANGUAGE plpgsql\nAS $function$\nBEGIN\n  RETURN NEW;\nEND;\n"
                                 "$function$\n"};
        target.objects << Object{"FUNCTION", "f",
                                 "CREATE OR REPLACE FUNCTION sync_target.f()\n RETURNS trigger\n "
                                 "LANGUAGE plpgsql\nAS $function$\nBEGIN\n  RETURN NULL;\nEND;\n"
                                 "$function$\n"};
        source.objects << Object{"VIEW", "v",
                                 "CREATE VIEW \"public\".\"v\" AS\n SELECT e.id\n   FROM employees "
                                 "e;"};
        target.objects << Object{"VIEW", "v",
                                 "CREATE VIEW \"sync_target\".\"v\" AS\n SELECT e.id\n   FROM "
                                 "sync_target.employees e;"};
        source.objects << Object{"TRIGGER", "trg",
                                 "CREATE TRIGGER trg BEFORE INSERT ON public.employees FOR EACH "
                                 "ROW EXECUTE FUNCTION f()"};
        target.objects << Object{"TRIGGER", "old_trg",
                                 "CREATE TRIGGER old_trg BEFORE UPDATE ON sync_target.employees "
                                 "FOR EACH ROW EXECUTE FUNCTION sync_target.f()"};

        const QList<Change> changes = compare(source, target);
        QCOMPARE(changes.size(), 3); /* the view is the same one */
        const Change *f = find(changes, Action::AlterObject, "f");
        QVERIFY(f && f->prepare.isEmpty() && !f->compound);
        QVERIFY2(f->sql.value(0).startsWith("CREATE OR REPLACE FUNCTION \"sync_target\".f()"),
                 qPrintable(f->sql.value(0)));
        const Change *trg = find(changes, Action::CreateObject, "trg");
        QVERIFY(trg);
        QCOMPARE(trg->sql, QStringList{"CREATE TRIGGER trg BEFORE INSERT ON \"sync_target\"."
                                       "employees FOR EACH ROW EXECUTE FUNCTION f()"});
        const Change *old = find(changes, Action::DropObject, "old_trg");
        QVERIFY(old);
        QCOMPARE(old->sql, QStringList{"DROP TRIGGER IF EXISTS \"old_trg\" ON "
                                       "sync_target.employees"});

        /* unqualified names in the CREATEs resolve in the target schema */
        const QString sql = script(changes, source, target);
        QVERIFY(sql.contains("BEGIN;\nSELECT set_config('search_path', '\"sync_target\", ' || "
                             "current_setting('search_path'), true);\n"));
        QVERIFY(!sql.contains("DELIMITER"));
        QVERIFY(sql.indexOf("FUNCTION \"sync_target\".f()") < sql.indexOf("CREATE TRIGGER"));
    }

    void sqliteObjects()
    {
        Schema source, target;
        source.driver = target.driver = SqlDriverType::Sqlite;
        source.database = target.database = "main";
        source.objects << Object{"VIEW", "v", "CREATE VIEW v AS SELECT 2"}
                       << Object{"TRIGGER", "t",
                                 "CREATE TRIGGER t AFTER INSERT ON x BEGIN SELECT 1; SELECT 2; "
                                 "END"};
        target.objects << Object{"VIEW", "v", "CREATE VIEW v AS SELECT 1"};
        const QList<Change> changes = compare(source, target);
        QCOMPARE(changes.size(), 2);
        const Change *v = find(changes, Action::AlterObject, "v");
        QVERIFY(v);
        QCOMPARE(v->prepare, QStringList{"DROP VIEW IF EXISTS \"v\""}); /* no OR REPLACE */
        QCOMPARE(v->sql, QStringList{"CREATE VIEW v AS SELECT 2"});
        QVERIFY(find(changes, Action::CreateObject, "t")->compound);
    }

    void identicalSchemasMatch()
    {
        Schema a{SqlDriverType::Mysql, "a", {parseMySql("employees", kSourceEmployees)}};
        Schema b{SqlDriverType::Mysql, "b", {parseMySql("employees", kSourceEmployees)}};
        QVERIFY(compare(a, b).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSchemaSync)
#include "tst_schemasync.moc"
