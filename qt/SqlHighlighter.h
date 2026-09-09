/* OpenYog — lightweight SQL syntax highlighter for the query editor.
 * Covers the visible need until the Scintilla swap (plan.md Phase 3);
 * colors chosen to stay readable on both light and dark themes. */
#pragma once

#include <QSyntaxHighlighter>

#include <QFont>
#include <QRegularExpression>
#include <QTextCharFormat>

class SqlHighlighter : public QSyntaxHighlighter
{
public:
    explicit SqlHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc)
    {
        QTextCharFormat kw;
        kw.setForeground(QColor(0x2A, 0x5D, 0x9F));
        kw.setFontWeight(QFont::Bold);
        const QStringList keywords = {
            "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","SET",
            "DELETE","CREATE","TABLE","DATABASE","DROP","ALTER","ADD","INDEX",
            "JOIN","INNER","LEFT","RIGHT","OUTER","ON","GROUP","BY","ORDER",
            "LIMIT","OFFSET","AS","AND","OR","NOT","NULL","IN","LIKE","BETWEEN",
            "DISTINCT","COUNT","SUM","AVG","MIN","MAX","SHOW","USE","DESCRIBE",
            "EXPLAIN","IF","EXISTS","PRIMARY","KEY","FOREIGN","REFERENCES",
            "DEFAULT","AUTO_INCREMENT","UNIQUE","CHECK","VIEW","PROCEDURE",
            "FUNCTION","TRIGGER","EVENT","BEGIN","END","COMMIT","ROLLBACK",
            "SET","CHARSET","ENGINE","TEMPORARY"
        };
        for(const QString &k : keywords)
            m_keywords << QRegularExpression(
                QStringLiteral("\\b%1\\b").arg(k),
                QRegularExpression::CaseInsensitiveOption);
        m_keywordFormats.resize(m_keywords.size());
        for(QTextCharFormat &f : m_keywordFormats)
            f = kw;

        QTextCharFormat str;
        str.setForeground(QColor(0xB3, 0x50, 0x00));
        m_rules.append({ QRegularExpression(QStringLiteral("'[^']*'|\"[^\"]*\"")), str });

        QTextCharFormat comment;
        comment.setForeground(QColor(0x6A, 0x99, 0x55));
        m_rules.append({ QRegularExpression(QStringLiteral("--[^\n]*")), comment });
        m_rules.append({ QRegularExpression(QStringLiteral("/\\*.*?\\*/")), comment });

        QTextCharFormat num;
        num.setForeground(QColor(0x7A, 0x5A, 0xC0));
        m_rules.append({ QRegularExpression(QStringLiteral("\\b\\d+(\\.\\d+)?\\b")), num });
    }

protected:
    void highlightBlock(const QString &text) override
    {
        for(int i = 0; i < m_keywords.size(); ++i) {
            auto it = m_keywords[i].globalMatch(text);
            while(it.hasNext()) {
                const auto m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), m_keywordFormats[i]);
            }
        }
        for(const Rule &r : m_rules) {
            auto it = r.re.globalMatch(text);
            while(it.hasNext()) {
                const auto m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), r.format);
            }
        }
    }

private:
    struct Rule { QRegularExpression re; QTextCharFormat format; };

    QVector<Rule> m_rules;
    QVector<QRegularExpression> m_keywords;
    QVector<QTextCharFormat> m_keywordFormats;
};
