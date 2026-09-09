/* OpenYog — one server connection = one tab, laid out like SQLyog:
 *   ┌───────────────┬──────────────────────────────────┐
 *   │ Object Browser│ [Query 1][History][+]  (editor)  │
 *   │ (filter+tree) ├──────────────────────────────────┤
 *   │               │ [1_Messages][Result][Info]       │
 *   └───────────────┴──────────────────────────────────┘
 * Mirrors upstream FrameWindow/DataView structure (Phase 3 in plan.md). */
#pragma once

#include "ConnectionParams.h"
#include "CodeEditor.h"
#include "QueryModel.h"

#include <QLabel>
#include <QTableView>
#include <QTabWidget>
#include <QWidget>

class ObjectBrowser;
class QPlainTextEdit;

class ConnectionTab : public QWidget
{
    Q_OBJECT
public:
    explicit ConnectionTab(const ConnectionParams &params, QWidget *parent = nullptr);
    ~ConnectionTab() override;

    bool isConnected() const { return m_conn != nullptr; }
    QString title() const { return m_params.name; }
    QStringList databases() const { return m_databases; }
    QString currentDatabase() const { return m_params.database; }
    QString hostLabel() const
    {
        return m_params.host + ':' + QString::number(m_params.port);
    }

public slots:
    void runQuery();
    void useDatabase(const QString &db);

signals:
    void databasesChanged(const QStringList &dbs, const QString &current);
    void executed(const QString &info);      /* "Exec: 0.01 sec" etc. */

private:
    void logHistory(const QString &sql);

    ConnectionParams   m_params;
    MYSQL            * m_conn       = nullptr;

    ObjectBrowser    * m_browser    = nullptr;
    QTabWidget       * m_editorTabs = nullptr;
    CodeEditor       * m_editor     = nullptr;
    QPlainTextEdit   * m_history    = nullptr;

    QTabWidget       * m_resultTabs = nullptr;
    QPlainTextEdit   * m_messages   = nullptr;
    QueryModel       * m_model      = nullptr;
    QTableView       * m_grid       = nullptr;
    QLabel           * m_info       = nullptr;
    QStringList        m_databases;

    double             m_execSecs   = 0.0;
};
