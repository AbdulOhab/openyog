#include "Theme.h"

#include "CommonHelper.h"   /* port shim: wyString */
#include "wyIni.h"

#include <QApplication>
#include <QDir>
#include <QPalette>
#include <QStandardPaths>
#include <QStyle>

namespace {
QString themePath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(".");
    return dir.filePath("OpenYog.ini");
}
} // namespace

QString Theme::load()
{
    wyString value;
    wyIni::IniGetString("UserInterface", "theme", "light", &value,
                        themePath().toUtf8());
    return (value.GetString() == QStringLiteral("dark"))
               ? QStringLiteral("dark") : QStringLiteral("light");
}

void Theme::save(const QString &theme)
{
    wyIni::IniWriteString("UserInterface", "theme", theme.toUtf8(),
                          themePath().toUtf8());
}

void Theme::apply(QApplication &app, const QString &theme)
{
    if(theme == QStringLiteral("dark")) {
        QPalette p;
        const QColor window(0x2B, 0x2B, 0x2B), base(0x1E, 0x1E, 0x1E),
                     text(0xD4, 0xD4, 0xD4), button(0x3C, 0x3F, 0x41),
                     disabled(0x80, 0x80, 0x80), highlight(0x2A, 0x5D, 0x9F);
        p.setColor(QPalette::Window,          window);
        p.setColor(QPalette::WindowText,      text);
        p.setColor(QPalette::Base,            base);
        p.setColor(QPalette::AlternateBase,   window);
        p.setColor(QPalette::Text,            text);
        p.setColor(QPalette::Button,          button);
        p.setColor(QPalette::ButtonText,      text);
        p.setColor(QPalette::ToolTipBase,     button);
        p.setColor(QPalette::ToolTipText,     text);
        p.setColor(QPalette::PlaceholderText, disabled);
        p.setColor(QPalette::Highlight,       highlight);
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Disabled, QPalette::Text,        disabled);
        p.setColor(QPalette::Disabled, QPalette::ButtonText,  disabled);
        p.setColor(QPalette::Disabled, QPalette::WindowText,  disabled);
        app.setPalette(p);
        app.setStyleSheet(QStringLiteral(
            "QTabWidget::pane { border: 1px solid #555555; }"
            "QTabBar { background: #3C3F41; }"
            "QTabBar::tab { background: #3C3F41; color: #D4D4D4; "
            "  padding: 3px 12px; margin-right: 1px; }"
            "QTabBar::tab:selected { background: #2A5D9F; color: white; }"));
    } else {
        /* explicit light palette — never derive from the system style, which
         * may itself be dark (that's the "toggle stays dark" bug) */
        QPalette p;
        const QColor window(0xF0, 0xF0, 0xF0), base(Qt::white),
                     text(0x1E, 0x1E, 0x1E), button(0xF0, 0xF0, 0xF0),
                     disabled(0x90, 0x90, 0x90), highlight(0x2A, 0x5D, 0x9F);
        p.setColor(QPalette::Window,          window);
        p.setColor(QPalette::WindowText,      text);
        p.setColor(QPalette::Base,            base);
        p.setColor(QPalette::AlternateBase,   QColor(0xF7, 0xF7, 0xF7));
        p.setColor(QPalette::Text,            text);
        p.setColor(QPalette::Button,          button);
        p.setColor(QPalette::ButtonText,      text);
        p.setColor(QPalette::ToolTipBase,     QColor(0xFF, 0xFF, 0xDC));
        p.setColor(QPalette::ToolTipText,     text);
        p.setColor(QPalette::PlaceholderText, disabled);
        p.setColor(QPalette::Highlight,       highlight);
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Disabled, QPalette::Text,        disabled);
        p.setColor(QPalette::Disabled, QPalette::ButtonText,  disabled);
        p.setColor(QPalette::Disabled, QPalette::WindowText,  disabled);
        app.setPalette(p);
        app.setStyleSheet(QStringLiteral(
            "QTabWidget::pane { border: 1px solid #8FA8C8; }"
            "QTabBar { background: #4A7EBB; }"
            "QTabBar::tab { background: #4A7EBB; color: white; "
            "  padding: 3px 12px; margin-right: 1px; }"
            "QTabBar::tab:selected { background: #EAF0F8; color: black; }"));
    }
}
