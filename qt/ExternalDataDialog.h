/* OpenYog — Tools ▸ Import External Data: a wizard that brings rows from a
 * CSV file, an Excel workbook or another open connection into a table of
 * the current connection.
 *
 *   1. Source: the file (separator, encoding, header row) or sheet, or the
 *      connection, database and table or query; the first rows shown.
 *   2. Target: a database, then a new table or an existing one (emptied
 *      first if asked).
 *   3. Columns: which source column goes where; for a new table each
 *      column's type, guessed from the data (or carried over from the
 *      source table) and editable.
 *   4. Import: on a worker thread with connections of its own, with
 *      progress and Cancel; nothing changes unless every row went in.
 *
 * ExternalData does the reading and writing; this is the choosing. */
#pragma once

#include "ConnectionParams.h"
#include "ExternalData.h"

#include <QWizard>

#include <atomic>
#include <functional>
#include <memory>

class IDbConnection;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QTableWidget;

class ExternalDataDialog : public QWizard
{
    Q_OBJECT
public:
    /* an open SQL connection, as the target or a source */
    struct Connection
    {
        QString label;
        SqlDriverType driver = SqlDriverType::Mysql;
        std::function<QStringList()> databases;
        std::function<QString()> currentDatabase;
        /* the tab's own connection for `db`, for lists and previews */
        std::function<IDbConnection *(const QString &db)> connection;
        /* how to open one of its own to `db`, for the import */
        std::function<ConnectionParams(const QString &db)> params;
    };

    ExternalDataDialog(Connection target, QList<Connection> sources, QWidget *parent = nullptr);
    ~ExternalDataDialog() override;

    /* the selftests' way through */
    void setCsvSourceForTest(const QString &file);
    void setExcelSourceForTest(const QString &file, const QString &sheet);
    void setDatabaseSourceForTest(int connection, const QString &db, const QString &table);
    void setNewTableForTest(const QString &db, const QString &table);
    void setExistingTableForTest(const QString &db, const QString &table, bool emptyFirst);
    /* goes to the columns page and reads its grid: "source -> target type" */
    QStringList mappingForTest();
    /* imports and waits; the result line */
    QString importForTest();

signals:
    /* rows went into db.table: the browser shows it */
    void imported(const QString &database, const QString &table);

protected:
    void initializePage(int id) override;
    bool validateCurrentPage() override;
    void reject() override;

private:
    enum Page { SourcePage, TargetPage, ColumnsPage, RunPage };
    enum Kind { Csv, Excel, Database };

    QWizardPage *makeSourcePage();
    QWizardPage *makeTargetPage();
    QWizardPage *makeColumnsPage();
    QWizardPage *makeRunPage();

    Kind kind() const;
    /* the chosen source, read on any thread */
    ExternalData::Reader reader(bool forWorker) const;
    QString sourceName() const; /* a default name for a new table */
    void sourceChanged();
    void refreshPreview();
    void fillSourceDatabases();
    void fillSourceTables();
    void fillTargetTables();
    void loadSheets();
    void buildMapping();
    ExternalData::Plan plan(QString *error) const;
    void startImport();

    Connection m_target;
    QList<Connection> m_sources;
    QStringList m_columns;                   /* the source's, as last read */
    QList<ExternalData::ColumnType> m_types; /* per source column, for a new table */
    QList<bool> m_binary;                    /* per source column */
    bool m_running = false, m_done = false;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QString m_result;

    /* source */
    QButtonGroup *m_kindGroup = nullptr;
    QStackedWidget *m_kindStack = nullptr;
    QLineEdit *m_csvFile = nullptr, *m_excelFile = nullptr;
    QComboBox *m_separator = nullptr, *m_encoding = nullptr, *m_sheet = nullptr;
    QCheckBox *m_csvHeader = nullptr, *m_excelHeader = nullptr, *m_emptyIsNull = nullptr;
    QComboBox *m_sourceConn = nullptr, *m_sourceDb = nullptr, *m_sourceTable = nullptr;
    QRadioButton *m_useQuery = nullptr;
    QPlainTextEdit *m_query = nullptr;
    QTableWidget *m_preview = nullptr;
    QLabel *m_previewNote = nullptr;
    /* target */
    QComboBox *m_targetDb = nullptr, *m_existingTable = nullptr;
    QRadioButton *m_newTable = nullptr, *m_existing = nullptr;
    QLineEdit *m_tableName = nullptr;
    QCheckBox *m_emptyFirst = nullptr;
    /* columns */
    QTableWidget *m_mapping = nullptr;
    QLabel *m_mappingNote = nullptr;
    /* run */
    QLabel *m_summary = nullptr, *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_start = nullptr;
};
