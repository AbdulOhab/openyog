/* OpenYog — connection dialog (plan.md Phase 2).
 * Prefills from the last saved connection; saving is done by the caller via
 * ConnectionStore (wyIni-backed, SQLyog-style INI). */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>

class QLineEdit;
class QSpinBox;

class ConnectionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    ConnectionParams params() const;

private:
    void loadLastUsed();

    QLineEdit *m_name     = nullptr;
    QLineEdit *m_host     = nullptr;
    QSpinBox  *m_port     = nullptr;
    QLineEdit *m_user     = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_database = nullptr;
};
