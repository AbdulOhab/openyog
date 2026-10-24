/* OpenYog — a MongoDB connection's tab: what a ConnectionTab is for an SQL
 * server, for documents.
 *
 * On the left the databases and, under each, its collections and views. A
 * collection opens on the right: a find (filter, projection, sort, a page
 * of documents at a time, with the count), an aggregation pipeline, or a
 * database command. The documents show as a grid, one column per top-level
 * field, and as text in the shell's notation. A document is edited, added
 * or copied as text (MongoJson keeps every value's BSON type), deleted with
 * the selection, and a collection's indexes managed; a collection exports
 * to and imports from a file of documents (one per line, as mongoexport
 * writes them, or a JSON array).
 *
 * Finding, aggregating, commands, edits, export and import run on a worker
 * thread through the shared MongoClient, so a slow server never freezes the
 * window; the index dialog's few quick calls wait in place. */
#pragma once

#include "ConnectionParams.h"
#include "MongoJson.h"

#include <QWidget>

#include <functional>
#include <memory>

class MongoClient;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

class MongoTab : public QWidget
{
    Q_OBJECT
public:
    MongoTab(const ConnectionParams &params, std::shared_ptr<MongoClient> client,
             QWidget *parent = nullptr);
    ~MongoTab() override;

    QString title() const
    {
        return m_params.name;
    }
    const ConnectionParams &params() const
    {
        return m_params;
    }

    /* reads the databases (and the open ones' collections) again */
    void reloadDatabases();
    /* the next (+1) or previous (-1) page of a find */
    void page(int direction);

    /* what the selftests drive and read */
    enum class Mode { Find, Aggregate, Command };
    void openCollection(const QString &db, const QString &collection);
    void setFind(const QString &filter, const QString &projection = {}, const QString &sort = {},
                 int limit = 50);
    void setMode(Mode mode, const QString &text = {}); /* text: the pipeline or command */
    void run();
    bool waitForIdle(int timeoutMs = 30000);
    QStringList gridForTest() const;
    QString jsonForTest() const;
    QString pageText() const;
    QString statusText() const;
    QStringList treeForTest() const;
    /* the editors' own paths, without their dialogs */
    bool saveDocumentForTest(int row, const QString &text, QString *error);
    bool insertDocumentForTest(const QString &text, QString *error);
    bool deleteRowsForTest(const QList<int> &rows, QString *error);

signals:
    void statusMessage(const QString &message);

private:
    struct Result
    {
        QList<QByteArray> documents; /* canonical Extended JSON */
        qint64 total = -1;           /* a find's count, -1: unknown */
        QStringList names;           /* databases or collections */
        QStringList types;           /* the collections' types */
        QString message;             /* what happened, for the status line */
        QString error;
    };
    using Work = std::function<Result(MongoClient &)>;
    void async(const QString &busy, Work work, std::function<void(const Result &)> done);

    void loadCollections(QTreeWidgetItem *dbItem);
    void showTreeMenu(const QPoint &pos);
    void showGridMenu(const QPoint &pos);
    void showDocuments(const Result &r, bool editable);
    void updatePaging();
    void updateActions();
    void modeChanged();

    void editDocument(int row);
    void insertDocument(const QString &initial = {});
    void deleteSelected();
    void manageIndexes();
    void exportCollection();
    void importCollection();
    void createCollection(const QString &db);
    void dropCollection(const QString &db, const QString &collection);
    void dropDatabase(const QString &db);

    /* the parsed text of a document to save, or an error */
    bool saveDocument(int row, const QString &text, const std::function<void(bool)> &then,
                      QString *error);
    bool insertText(const QString &text, const std::function<void(bool)> &then, QString *error);
    bool deleteRows(const QList<int> &rows, const std::function<void(bool)> &then, QString *error);
    QList<int> selectedRows() const;
    void setStatus(const QString &text, bool error = false);

    ConnectionParams m_params;
    std::shared_ptr<MongoClient> m_client;
    QString m_db, m_collection;
    Mode m_mode = Mode::Find;
    qint64 m_skip = 0, m_total = -1;
    bool m_editable = false;
    int m_pending = 0;
    std::vector<MongoJson::Value> m_docs; /* the grid's rows */
    QList<QByteArray> m_raw;              /* and as they came */

    QToolBar *m_toolbar = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_where = nullptr;
    QComboBox *m_modeBox = nullptr;
    QStackedWidget *m_queryStack = nullptr;
    QLineEdit *m_filter = nullptr, *m_projection = nullptr, *m_sort = nullptr;
    QSpinBox *m_limit = nullptr;
    QPlainTextEdit *m_pipeline = nullptr, *m_command = nullptr;
    QPushButton *m_runButton = nullptr;
    QTabWidget *m_results = nullptr;
    QTableWidget *m_grid = nullptr;
    QPlainTextEdit *m_json = nullptr;
    QPushButton *m_prev = nullptr, *m_next = nullptr;
    QLabel *m_pageLabel = nullptr, *m_status = nullptr;
    QAction *m_insertAction = nullptr, *m_editAction = nullptr, *m_deleteAction = nullptr,
            *m_indexAction = nullptr, *m_importAction = nullptr, *m_exportAction = nullptr;
};
