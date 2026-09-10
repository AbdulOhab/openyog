/* OpenYog — "Export result as…" dialog: format picker + per-format options +
 * a file chooser. Used by the query-result grid and the Table Data pane
 * (SQLyog's ExportData wizard, condensed to one pane). */
#pragma once

#include <QDialog>

#include "ResultExport.h"

class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;

class ExportDialog : public QDialog
{
    Q_OBJECT
public:
    ExportDialog(const QString &suggestedBaseName, const QString &sqlTable,
                 int rowCount, bool haveSelection, QWidget *parent = nullptr);

    QString                 path() const;
    ResultExport::Format    format() const;
    ResultExport::Options   options() const;
    bool                    selectionOnly() const;
    bool                    includeStructure() const;   /* SQL: CREATE TABLE too */

private slots:
    void syncForFormat();
    void pickFile();

private:
    QComboBox *m_format = nullptr;
    QLineEdit *m_path = nullptr;
    QLineEdit *m_delim = nullptr;
    QLineEdit *m_quote = nullptr;
    QLineEdit *m_null = nullptr;
    QLineEdit *m_sqlTable = nullptr;
    QCheckBox *m_header = nullptr;
    QCheckBox *m_bom = nullptr;
    QCheckBox *m_crlf = nullptr;
    QCheckBox *m_selOnly = nullptr;
    QCheckBox *m_structure = nullptr;
    QLabel    *m_delimLabel = nullptr;
    QLabel    *m_quoteLabel = nullptr;
    QLabel    *m_sqlTableLabel = nullptr;
    QString    m_base;
};
