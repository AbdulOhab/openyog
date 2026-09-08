/* OpenYog — one server connection = one tab (plan.md Phase 2).
 * Phase 2 skeleton: info header, plain-text query editor, Run button,
 * result grid. Scintilla editor + object browser arrive in Phase 3. */
#pragma once

#include "ConnectionParams.h"
#include "QueryModel.h"    /* brings in mysql/mysql.h (MYSQL typedef) */

#include <QLabel>
#include <QPlainTextEdit>
#include <QTableView>
#include <QWidget>

class ConnectionTab : public QWidget
{
    Q_OBJECT
public:
    explicit ConnectionTab(const ConnectionParams &params, QWidget *parent = nullptr);
    ~ConnectionTab() override;

    bool isConnected() const { return m_conn != nullptr; }
    QString title() const { return m_params.name; }

public slots:
    void runQuery();

private:
    ConnectionParams m_params;
    MYSQL          * m_conn   = nullptr;
    QLabel         * m_info   = nullptr;
    QPlainTextEdit * m_editor = nullptr;
    QueryModel     * m_model  = nullptr;
    QTableView     * m_grid   = nullptr;
    QLabel         * m_status = nullptr;
};
