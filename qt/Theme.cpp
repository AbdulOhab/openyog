#include "Theme.h"

#include "CommonHelper.h"   /* port shim: wyString */
#include "wyIni.h"

#include <QApplication>
#include <QDir>
#include <QPalette>
#include <QStandardPaths>
#include <QStyle>

/* Palette + metrics come from the upstream "Flat" theme and the frame code.
 * All values are documented in xnote/2026-09-09-ui-shell-spec.md:
 *   SQLyog blue   #3B7DBB   strips, splitters, result/table tab bars
 *   selection     #89BCED / wash #E8F2FA
 *   toolbar bg    #F5F5F5   menu/conn-tab bg #FFFFFF
 *   menu text     #424242 active / #A2A2A2 disabled
 * Object names set by the widgets so one sheet can style each strip:
 *   QTabWidget#connTabs          connection tabs  — white strip, blue underline
 *   QTabWidget#editorTabs        Query/History    — white strip
 *   QTabWidget#resultTabs        Messages/Data/…  — solid blue strip
 *   QTabWidget#connectDialogTabs ConnectionDialog's driver tabs — same as #connTabs
 *   QLabel#infoStrip      nag-bar replacement — solid blue
 *   QFrame#limitStrip     bottom LIMIT combo row — solid blue          */

namespace {
QString themePath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(".");
    return dir.filePath("OpenYog.ini");
}

const char *kLightSheet = R"QSS(
* { font-size: 9pt; }

QMainWindow, QMenuBar, QStatusBar, QToolBar { background: #FFFFFF; }

QMenuBar { border-bottom: 1px solid #D9D9D9; }
QMenuBar::item { padding: 3px 8px; background: transparent; color: #424242; }
QMenuBar::item:selected { background: #E8F2FA; }
QMenu { background: #FFFFFF; border: 1px solid #B9B9B9; }
/* padding-left used to be 24px, same as the right — a deliberate stand-in
 * indent for where an icon would eventually go, back when every item was
 * plain text. Session 83 gave most items a real ~16px QAction icon, which
 * Qt draws *inside* that left padding box rather than before it — so the
 * old 24px became a redundant blank strip in front of the icon itself
 * (icon at x=24 instead of near x=6). Right stays generous: it's still
 * genuinely empty space before the shortcut-key text, unaffected by icons. */
QMenu::item { padding: 3px 24px 3px 6px; color: #424242; }
QMenu::item:disabled { color: #A2A2A2; }
QMenu::item:selected { background: #89BCED; color: #1E1E1E; }
QMenu::separator { height: 1px; background: #E0E0E0; margin: 3px 0; }

QToolBar { border: 0; border-bottom: 1px solid #D9D9D9; padding: 2px; spacing: 2px; }
QToolBar::separator { width: 1px; background: #D0D0D0; margin: 2px 4px; }
QToolButton { border: 1px solid transparent; border-radius: 2px; padding: 2px; }
QToolButton:hover { background: #E8F2FA; border-color: #89BCED; }
QToolButton:pressed, QToolButton:checked { background: #89BCED; }

QTabWidget::pane { border: 0; }
QTabBar::tab { padding: 3px 12px; margin: 0; border: 0; }
QTabBar::tab:!selected { margin-top: 0; }

/* connection tabs — white strip with the SQLyog blue underline */
QTabWidget#connTabs > QTabBar { background: #FFFFFF; qproperty-drawBase: 0;
    border-bottom: 2px solid #3B7DBB; }
QTabWidget#connTabs > QTabBar::tab { background: #F0F0F0; color: #3B7DBB;
    border: 1px solid #C8C8C8; border-bottom: 0; margin-right: 2px;
    padding: 3px 10px; }
QTabWidget#connTabs > QTabBar::tab:selected { background: #FFFFFF;
    color: #1E1E1E; }

/* ConnectionDialog's own driver tabs (MySQL/SQLite/PostgreSQL/HTTP/SSH/
 * SSL/Advanced) — same treatment as #connTabs above; without a name of
 * its own this QTabWidget fell through to the bare border:0 rule a few
 * lines up with no selected-state color at all, so which tab was active
 * (or that these were clickable tabs rather than plain text) wasn't
 * visible on the light theme */
QTabWidget#connectDialogTabs > QTabBar { background: #FFFFFF; qproperty-drawBase: 0;
    border-bottom: 2px solid #3B7DBB; }
/* no margin-right between tabs: with one, the 2px gap next to a *selected*
 * tab (fill #FFFFFF, same as the tab bar's own background) had no color
 * contrast against that background, so the thin border line marking the
 * gap's edges was easy to miss — the whole gap+tab-interior read as one
 * oversized blank patch next to whichever tab was selected, even though
 * every gap was the same 2px. Butting the 1px borders together instead
 * always leaves a visible seam, selected or not. */
QTabWidget#connectDialogTabs > QTabBar::tab { background: #F0F0F0; color: #3B7DBB;
    border: 1px solid #C8C8C8; border-bottom: 0; border-right: 0;
    padding: 3px 10px; }
QTabWidget#connectDialogTabs > QTabBar::tab:last { border-right: 1px solid #C8C8C8; }
QTabWidget#connectDialogTabs > QTabBar::tab:selected { background: #FFFFFF;
    color: #1E1E1E; font-weight: bold; }

/* editor tabs (Query N / History) — white strip */
QTabWidget#editorTabs > QTabBar { background: #FFFFFF; qproperty-drawBase: 0;
    border-bottom: 1px solid #C8D6E5; }
QTabWidget#editorTabs > QTabBar::tab { background: #FFFFFF; color: #3B7DBB;
    border-right: 1px solid #E0E0E0; padding: 3px 14px; }
QTabWidget#editorTabs > QTabBar::tab:selected { background: #FFFFFF;
    color: #1E1E1E; border-bottom: 2px solid #3B7DBB; }

/* result tabs (Messages / Table Data / Info + result grids) — solid blue */
QTabWidget#resultTabs > QTabBar { background: #3B7DBB; qproperty-drawBase: 0; }
QTabWidget#resultTabs > QTabBar::tab { background: #3B7DBB; color: #FFFFFF;
    border-right: 1px solid #5A93C8; padding: 3px 14px; }
QTabWidget#resultTabs > QTabBar::tab:selected { background: #FFFFFF;
    color: #000000; }

QLabel#infoStrip { background: #3B7DBB; color: #FFFFFF; padding: 2px 8px; }
QFrame#limitStrip { background: #3B7DBB; }
QFrame#limitStrip QComboBox { min-width: 90px; }

QSplitter::handle { background: #3B7DBB; }
QSplitter::handle:horizontal { width: 4px; }
QSplitter::handle:vertical { height: 4px; }

QTreeView { background: #FFFFFF; border: 0; outline: 0; }
QTreeView::item { height: 18px; }
QTreeView::item:selected { background: #89BCED; color: #1E1E1E; }
/* deliberately NOT styling QTreeView::branch:selected — as soon as any
 * rule targets ::branch, Qt switches that whole sub-control to CSS-driven
 * painting, and since no explicit closed/open images were ever defined,
 * the expand/collapse arrow simply stopped being drawn for a selected,
 * collapsed row (open rows still had a visible arrow, which is what made
 * this easy to miss — https://.../ the classic Qt::branch pitfall).
 * Leaving ::branch unstyled restores the native arrow in every state and
 * still looks right: this style's branch area already renders transparent
 * over the tree's own white background, so it reads as a clean edge next
 * to the blue ::item:selected fill rather than a mismatched seam. */

QLabel#obFilterLabel { color: #606060; padding: 2px 2px 0 2px; }

QTableView { background: #FFFFFF; gridline-color: #E2E2E2;
    selection-background-color: #89BCED; selection-color: #1E1E1E; }
QHeaderView::section { background: #F0F0F0; color: #424242;
    border: 0; border-right: 1px solid #D6D6D6; border-bottom: 1px solid #D6D6D6;
    padding: 3px 6px; }

QStatusBar { border-top: 1px solid #D9D9D9; }
QStatusBar QLabel { color: #424242; }
QStatusBar::item { border: 0; }
)QSS";

const char *kDarkTabSheet = R"QSS(
QTabWidget::pane { border: none; }
QTabBar { background: #3C3F41; }
QTabBar::tab { background: #3C3F41; color: #C8C8C8; padding: 3px 12px;
    margin-right: 1px; font-size: 9pt; }
QTabBar::tab:selected { background: #2A5D9F; color: white; }
QLabel#infoStrip { background: #2A5D9F; color: white; padding: 2px 8px; }
QFrame#limitStrip { background: #2A5D9F; }
QSplitter::handle { background: #2A5D9F; }
QSplitter::handle:horizontal { width: 4px; }
QSplitter::handle:vertical { height: 4px; }
QTreeView::item { height: 18px; }
)QSS";

/* Palette from the upstream include/twilight_theme.xml (Dark.xml is an
 * empty stub upstream — our "dark" theme above is this project's own
 * design — but Twilight actually ships real colors, so this one is a
 * faithful port): a dusky navy base with a warm gold accent, in place of
 * the dark theme's neutral grey + blue. Its decimal color attributes are
 * Windows COLORREF (BGR) — converted to RGB hex here; its 0x-prefixed
 * attributes are already plain RGB (cross-checked against Flat.xml, whose
 * hsplitter/selected values match this project's known-correct SQLyog
 * blue #3B7DBB / #89BCED verbatim as plain hex, not BGR-swapped). */
const char *kTwilightTabSheet = R"QSS(
QTabWidget::pane { border: none; }
QTabBar { background: #293955; }
QTabBar::tab { background: #293955; color: #A5B1C9; padding: 3px 12px;
    margin-right: 1px; font-size: 9pt; }
QTabBar::tab:selected { background: #FCE198; color: #293955; }
QLabel#infoStrip { background: #3A5278; color: #FCE198; padding: 2px 8px; }
QFrame#limitStrip { background: #3A5278; }
QSplitter::handle { background: #3A5278; }
QSplitter::handle:horizontal { width: 4px; }
QSplitter::handle:vertical { height: 4px; }
QTreeView::item { height: 18px; }
)QSS";
} // namespace

QString Theme::load()
{
    wyString value;
    wyIni::IniGetString("UserInterface", "theme", "light", &value,
                        themePath().toUtf8());
    const QString v = QString::fromUtf8(value.GetString());
    if(v == QStringLiteral("dark") || v == QStringLiteral("twilight"))
        return v;
    return QStringLiteral("light");
}

void Theme::save(const QString &theme)
{
    wyIni::IniWriteString("UserInterface", "theme", theme.toUtf8(),
                          themePath().toUtf8());
}

void Theme::apply(QApplication &app, const QString &theme)
{
    if(theme == QStringLiteral("twilight")) {
        QPalette p;
        const QColor window(0x29, 0x39, 0x55), base(0x21, 0x2E, 0x44),
                     text(0xE9, 0xEC, 0xEE), button(0x3A, 0x52, 0x78),
                     disabled(0x7A, 0x86, 0x9C), highlight(0xE5, 0xC3, 0x65);
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
        p.setColor(QPalette::HighlightedText, QColor(0x29, 0x39, 0x55));
        p.setColor(QPalette::Disabled, QPalette::Text,        disabled);
        p.setColor(QPalette::Disabled, QPalette::ButtonText,  disabled);
        p.setColor(QPalette::Disabled, QPalette::WindowText,  disabled);
        app.setPalette(p);
        app.setStyleSheet(QString::fromUtf8(kTwilightTabSheet));
    } else if(theme == QStringLiteral("dark")) {
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
        app.setStyleSheet(QString::fromUtf8(kDarkTabSheet));
    } else {
        /* explicit light palette — never derive from the system style, which
         * may itself be dark (that's the "toggle stays dark" bug) */
        QPalette p;
        const QColor window(0xFF, 0xFF, 0xFF), base(Qt::white),
                     text(0x1E, 0x1E, 0x1E), button(0xFF, 0xFF, 0xFF),
                     disabled(0xA2, 0xA2, 0xA2), highlight(0x89, 0xBC, 0xED);
        p.setColor(QPalette::Window,          window);
        p.setColor(QPalette::WindowText,      text);
        p.setColor(QPalette::Base,            base);
        p.setColor(QPalette::AlternateBase,   QColor(0xF5, 0xF9, 0xFD));
        p.setColor(QPalette::Text,            text);
        p.setColor(QPalette::Button,          button);
        p.setColor(QPalette::ButtonText,      text);
        p.setColor(QPalette::ToolTipBase,     QColor(0xFF, 0xFF, 0xDC));
        p.setColor(QPalette::ToolTipText,     text);
        p.setColor(QPalette::PlaceholderText, disabled);
        p.setColor(QPalette::Highlight,       highlight);
        p.setColor(QPalette::HighlightedText, QColor(0x1E, 0x1E, 0x1E));
        p.setColor(QPalette::Disabled, QPalette::Text,        disabled);
        p.setColor(QPalette::Disabled, QPalette::ButtonText,  disabled);
        p.setColor(QPalette::Disabled, QPalette::WindowText,  disabled);
        app.setPalette(p);
        app.setStyleSheet(QString::fromUtf8(kLightSheet));
    }
}
