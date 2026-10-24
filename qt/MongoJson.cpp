/* OpenYog — shell notation and canonical Extended JSON; see MongoJson.h. */
#include "MongoJson.h"

#include <QDateTime>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <limits>

namespace MongoJson {

Value Value::string(const QString &s)
{
    Value v;
    v.type = String;
    v.text = s;
    return v;
}

Value Value::number(const QString &literal)
{
    Value v;
    v.type = Number;
    v.text = literal;
    return v;
}

Value Value::object(std::vector<std::pair<QString, Value>> members)
{
    Value v;
    v.type = Object;
    v.members = std::move(members);
    return v;
}

Value Value::array(std::vector<Value> items)
{
    Value v;
    v.type = Array;
    v.items = std::move(items);
    return v;
}

const Value *Value::member(const QString &key) const
{
    for(const auto &m : members)
        if(m.first == key)
            return &m.second;
    return nullptr;
}

QString newObjectId()
{
    static const quint64 random = QRandomGenerator::system()->generate64();
    static std::atomic<quint32> counter{QRandomGenerator::system()->generate()};
    const quint32 seconds = quint32(QDateTime::currentSecsSinceEpoch());
    const quint32 n = counter++ & 0xffffff;
    QByteArray bytes;
    for(int shift = 24; shift >= 0; shift -= 8)
        bytes += char((seconds >> shift) & 0xff);
    for(int shift = 32; shift >= 0; shift -= 8)
        bytes += char((random >> shift) & 0xff);
    for(int shift = 16; shift >= 0; shift -= 8)
        bytes += char((n >> shift) & 0xff);
    return QString::fromLatin1(bytes.toHex());
}

namespace {

QString shortestDouble(double d);

Value wrap(const QString &key, Value inner)
{
    return Value::object({{key, std::move(inner)}});
}

Value dateValue(qint64 ms)
{
    return wrap(QStringLiteral("$date"),
                wrap(QStringLiteral("$numberLong"), Value::string(QString::number(ms))));
}

/* BSON wants a regex's options sorted, and knows only these */
QString regexOptions(const QString &flags)
{
    QString out;
    for(const QChar c : QStringLiteral("ilmsux"))
        if(flags.contains(c))
            out += c;
    return out;
}

Value regexValue(const QString &pattern, const QString &flags)
{
    return wrap(QStringLiteral("$regularExpression"),
                Value::object({{QStringLiteral("pattern"), Value::string(pattern)},
                               {QStringLiteral("options"), Value::string(regexOptions(flags))}}));
}

Value binaryValue(const QByteArray &bytes, int subType)
{
    return wrap(
        QStringLiteral("$binary"),
        Value::object(
            {{QStringLiteral("base64"), Value::string(QString::fromLatin1(bytes.toBase64()))},
             {QStringLiteral("subType"),
              Value::string(QStringLiteral("%1").arg(subType, 2, 16, QLatin1Char('0')))}}));
}

bool isIdentifierStart(QChar c)
{
    return c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

bool isIdentifierPart(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

class Parser
{
public:
    explicit Parser(const QString &text) : m_s(text) {}

    bool parse(Value *out, QString *error)
    {
        skipSpace();
        bool ok = value(out);
        if(ok) {
            skipSpace();
            if(m_i < m_s.size())
                ok = fail(QStringLiteral("unexpected '%1' after the value").arg(m_s[m_i]));
        }
        if(!ok && error) {
            int line = 1, column = 1;
            for(int i = 0; i < m_errorAt && i < m_s.size(); ++i) {
                if(m_s[i] == QLatin1Char('\n')) {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            *error = QStringLiteral("%1:%2: %3").arg(line).arg(column).arg(m_error);
        }
        return ok;
    }

private:
    bool fail(const QString &what)
    {
        if(m_error.isEmpty()) {
            m_error = what;
            m_errorAt = m_i;
        }
        return false;
    }
    QChar peek(int ahead = 0) const
    {
        return m_i + ahead < m_s.size() ? m_s[m_i + ahead] : QChar();
    }
    bool atEnd() const
    {
        return m_i >= m_s.size();
    }

    void skipSpace()
    {
        while(!atEnd()) {
            if(peek().isSpace()) {
                ++m_i;
            } else if(peek() == QLatin1Char('/') && peek(1) == QLatin1Char('/')) {
                while(!atEnd() && peek() != QLatin1Char('\n'))
                    ++m_i;
            } else if(peek() == QLatin1Char('/') && peek(1) == QLatin1Char('*')) {
                const int end = int(m_s.indexOf(QStringLiteral("*/"), m_i + 2));
                m_i = end < 0 ? int(m_s.size()) : end + 2;
            } else {
                break;
            }
        }
    }

    bool expect(QChar c)
    {
        skipSpace();
        if(peek() != c)
            return fail(atEnd() ? QStringLiteral("'%1' expected at the end").arg(c)
                                : QStringLiteral("'%1' expected, not '%2'").arg(c, peek()));
        ++m_i;
        return true;
    }

    QString identifier()
    {
        const int start = m_i;
        while(!atEnd() && isIdentifierPart(peek()))
            ++m_i;
        return m_s.mid(start, m_i - start);
    }

    bool value(Value *out)
    {
        skipSpace();
        if(atEnd())
            return fail(QStringLiteral("a value expected"));
        const QChar c = peek();
        if(c == QLatin1Char('{'))
            return object(out);
        if(c == QLatin1Char('['))
            return array(out);
        if(c == QLatin1Char('"') || c == QLatin1Char('\''))
            return string(out);
        if(c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+') ||
           (c == QLatin1Char('.') && peek(1).isDigit()))
            return number(out);
        if(c == QLatin1Char('/'))
            return regex(out);
        if(isIdentifierStart(c))
            return word(out);
        return fail(QStringLiteral("a value expected, not '%1'").arg(c));
    }

    bool object(Value *out)
    {
        ++m_i; /* { */
        *out = Value::object();
        for(;;) {
            skipSpace();
            if(peek() == QLatin1Char('}')) {
                ++m_i;
                return true;
            }
            QString key;
            if(peek() == QLatin1Char('"') || peek() == QLatin1Char('\'')) {
                Value k;
                if(!string(&k))
                    return false;
                key = k.text;
            } else {
                /* a bare key: an identifier, digits, or a dotted path */
                const int start = m_i;
                while(!atEnd() && (isIdentifierPart(peek()) || peek() == QLatin1Char('.')))
                    ++m_i;
                key = m_s.mid(start, m_i - start);
                if(key.isEmpty())
                    return fail(
                        atEnd() ? QStringLiteral("'}' expected at the end")
                                : QStringLiteral("a key or '}' expected, not '%1'").arg(peek()));
            }
            if(!expect(QLatin1Char(':')))
                return false;
            Value v;
            if(!value(&v))
                return false;
            out->members.emplace_back(key, std::move(v));
            skipSpace();
            if(peek() == QLatin1Char(',')) {
                ++m_i;
                continue;
            }
            if(peek() != QLatin1Char('}'))
                return fail(atEnd() ? QStringLiteral("'}' expected at the end")
                                    : QStringLiteral("',' or '}' expected, not '%1'").arg(peek()));
        }
    }

    bool array(Value *out)
    {
        ++m_i; /* [ */
        *out = Value::array();
        for(;;) {
            skipSpace();
            if(peek() == QLatin1Char(']')) {
                ++m_i;
                return true;
            }
            Value v;
            if(!value(&v))
                return false;
            out->items.push_back(std::move(v));
            skipSpace();
            if(peek() == QLatin1Char(',')) {
                ++m_i;
                continue;
            }
            if(peek() != QLatin1Char(']'))
                return fail(atEnd() ? QStringLiteral("']' expected at the end")
                                    : QStringLiteral("',' or ']' expected, not '%1'").arg(peek()));
        }
    }

    bool string(Value *out)
    {
        const QChar quote = peek();
        ++m_i;
        QString s;
        for(;;) {
            if(atEnd())
                return fail(QStringLiteral("unterminated string"));
            const QChar c = m_s[m_i++];
            if(c == quote)
                break;
            if(c != QLatin1Char('\\')) {
                s += c;
                continue;
            }
            if(atEnd())
                return fail(QStringLiteral("unterminated string"));
            const QChar e = m_s[m_i++];
            switch(e.unicode()) {
                case 'n':
                    s += QLatin1Char('\n');
                    break;
                case 't':
                    s += QLatin1Char('\t');
                    break;
                case 'r':
                    s += QLatin1Char('\r');
                    break;
                case 'b':
                    s += QLatin1Char('\b');
                    break;
                case 'f':
                    s += QLatin1Char('\f');
                    break;
                case '0':
                    s += QChar(0);
                    break;
                case 'u':
                case 'x': {
                    const int digits = e == QLatin1Char('u') ? 4 : 2;
                    bool ok = false;
                    const uint code = m_s.mid(m_i, digits).toUInt(&ok, 16);
                    if(!ok || m_i + digits > m_s.size())
                        return fail(QStringLiteral("bad \\%1 escape").arg(e));
                    s += QChar(char16_t(code));
                    m_i += digits;
                    break;
                }
                default:
                    s += e; /* \" \' \\ \/ and anything else: itself */
            }
        }
        *out = Value::string(s);
        return true;
    }

    bool number(Value *out)
    {
        const int start = m_i;
        if(peek() == QLatin1Char('-') || peek() == QLatin1Char('+'))
            ++m_i;
        if(isIdentifierStart(peek())) { /* -Infinity */
            const QString w = identifier();
            if(w != QStringLiteral("Infinity"))
                return fail(QStringLiteral("a number expected"));
            *out = Value::number(m_s[start] == QLatin1Char('-') ? QStringLiteral("-Infinity")
                                                                : QStringLiteral("Infinity"));
            return true;
        }
        bool digits = false;
        while(peek().isDigit()) {
            ++m_i;
            digits = true;
        }
        if(peek() == QLatin1Char('.')) {
            ++m_i;
            while(peek().isDigit()) {
                ++m_i;
                digits = true;
            }
        }
        if(!digits)
            return fail(QStringLiteral("a number expected"));
        if(peek() == QLatin1Char('e') || peek() == QLatin1Char('E')) {
            ++m_i;
            if(peek() == QLatin1Char('-') || peek() == QLatin1Char('+'))
                ++m_i;
            if(!peek().isDigit())
                return fail(QStringLiteral("an exponent expected"));
            while(peek().isDigit())
                ++m_i;
        }
        QString literal = m_s.mid(start, m_i - start);
        if(literal.startsWith(QLatin1Char('+')))
            literal.remove(0, 1);
        *out = Value::number(literal);
        return true;
    }

    /* /pattern/flags: up to the first '/' that isn't escaped or in a [class] */
    bool regex(Value *out)
    {
        ++m_i;
        QString pattern;
        bool inClass = false;
        for(;;) {
            if(atEnd() || peek() == QLatin1Char('\n'))
                return fail(QStringLiteral("unterminated regular expression"));
            const QChar c = m_s[m_i++];
            if(c == QLatin1Char('\\') && !atEnd()) {
                pattern += c;
                pattern += m_s[m_i++];
                continue;
            }
            if(c == QLatin1Char('['))
                inClass = true;
            else if(c == QLatin1Char(']'))
                inClass = false;
            else if(c == QLatin1Char('/') && !inClass)
                break;
            pattern += c;
        }
        *out = regexValue(pattern, identifier());
        return true;
    }

    bool arguments(std::vector<Value> *args)
    {
        skipSpace();
        if(peek() != QLatin1Char('('))
            return true; /* MinKey, Date… without parentheses */
        ++m_i;
        for(;;) {
            skipSpace();
            if(peek() == QLatin1Char(')')) {
                ++m_i;
                return true;
            }
            Value v;
            if(!value(&v))
                return false;
            args->push_back(std::move(v));
            skipSpace();
            if(peek() == QLatin1Char(',')) {
                ++m_i;
                continue;
            }
            if(peek() != QLatin1Char(')'))
                return fail(QStringLiteral("',' or ')' expected"));
        }
    }

    /* an argument's text, string or number alike */
    static QString argText(const std::vector<Value> &args, size_t i)
    {
        return i < args.size() && (args[i].type == Value::String || args[i].type == Value::Number)
                   ? args[i].text
                   : QString();
    }

    bool word(Value *out)
    {
        const int start = m_i;
        QString name = identifier();
        if(name == QStringLiteral("true") || name == QStringLiteral("false")) {
            out->type = Value::Bool;
            out->boolean = name == QStringLiteral("true");
            return true;
        }
        if(name == QStringLiteral("null")) {
            *out = Value();
            return true;
        }
        if(name == QStringLiteral("undefined")) {
            Value t;
            t.type = Value::Bool;
            t.boolean = true;
            *out = wrap(QStringLiteral("$undefined"), t);
            return true;
        }
        if(name == QStringLiteral("NaN") || name == QStringLiteral("Infinity")) {
            *out = Value::number(name);
            return true;
        }
        if(name == QStringLiteral("new")) {
            skipSpace();
            name = identifier();
        }
        std::vector<Value> args;
        if(!arguments(&args))
            return false;
        const QString a0 = argText(args, 0);

        if(name == QStringLiteral("ObjectId")) {
            static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{24}$"));
            if(args.empty()) {
                *out = wrap(QStringLiteral("$oid"), Value::string(newObjectId()));
                return true;
            }
            if(!hex.match(a0).hasMatch())
                return failAt(start, QStringLiteral("ObjectId needs 24 hex digits"));
            *out = wrap(QStringLiteral("$oid"), Value::string(a0.toLower()));
            return true;
        }
        if(name == QStringLiteral("ISODate") || name == QStringLiteral("Date")) {
            if(args.empty()) {
                *out = dateValue(QDateTime::currentMSecsSinceEpoch());
                return true;
            }
            if(args[0].type == Value::Number) {
                bool ok = false;
                const qint64 ms = qint64(a0.toDouble(&ok));
                if(!ok)
                    return failAt(start, QStringLiteral("a date in milliseconds expected"));
                *out = dateValue(ms);
                return true;
            }
            /* no offset given: UTC, as the shell takes it */
            QString s = a0.trimmed();
            static const QRegularExpression zone(QStringLiteral("(Z|[+-]\\d\\d:?\\d\\d)$"));
            const bool zoned = zone.match(s).hasMatch() && s.contains(QLatin1Char('T'));
            QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
            if(!dt.isValid())
                return failAt(start, QStringLiteral("not an ISO date: %1").arg(s));
            if(!zoned)
                dt = QDateTime(dt.date(), dt.time(), QTimeZone::UTC);
            *out = dateValue(dt.toMSecsSinceEpoch());
            return true;
        }
        if(name == QStringLiteral("NumberLong") || name == QStringLiteral("Long") ||
           name == QStringLiteral("NumberInt") || name == QStringLiteral("Int32")) {
            const bool isLong = name.contains(QStringLiteral("Long"));
            bool ok = false;
            const qint64 n = a0.trimmed().toLongLong(&ok);
            if(!ok || (!isLong && (n < std::numeric_limits<qint32>::min() ||
                                   n > std::numeric_limits<qint32>::max())))
                return failAt(start, QStringLiteral("%1 needs a whole number%2")
                                         .arg(name, isLong ? QString()
                                                           : QStringLiteral(" that fits 32 bits")));
            *out = wrap(isLong ? QStringLiteral("$numberLong") : QStringLiteral("$numberInt"),
                        Value::string(QString::number(n)));
            return true;
        }
        if(name == QStringLiteral("NumberDecimal") || name == QStringLiteral("Decimal128")) {
            if(a0.trimmed().isEmpty())
                return failAt(start, QStringLiteral("%1 needs a number").arg(name));
            *out = wrap(QStringLiteral("$numberDecimal"), Value::string(a0.trimmed()));
            return true;
        }
        if(name == QStringLiteral("Double")) {
            bool ok = false;
            a0.toDouble(&ok);
            if(!ok)
                return failAt(start, QStringLiteral("Double needs a number"));
            *out =
                wrap(QStringLiteral("$numberDouble"), Value::string(shortestDouble(a0.toDouble())));
            return true;
        }
        if(name == QStringLiteral("Timestamp")) {
            QString t = a0, i = argText(args, 1);
            if(!args.empty() && args[0].isObject()) { /* Timestamp({t: …, i: …}) */
                const Value *tv = args[0].member(QStringLiteral("t"));
                const Value *iv = args[0].member(QStringLiteral("i"));
                t = tv ? tv->text : QString();
                i = iv ? iv->text : QString();
            }
            bool okT = false, okI = false;
            t.toUInt(&okT);
            i.toUInt(&okI);
            if(!okT || !okI)
                return failAt(start, QStringLiteral("Timestamp needs seconds and an increment"));
            *out = wrap(QStringLiteral("$timestamp"),
                        Value::object({{QStringLiteral("t"), Value::number(t)},
                                       {QStringLiteral("i"), Value::number(i)}}));
            return true;
        }
        if(name == QStringLiteral("BinData") || name == QStringLiteral("HexData")) {
            bool ok = false;
            const int sub = a0.toInt(&ok);
            const QString data = argText(args, 1);
            if(!ok || sub < 0 || sub > 255)
                return failAt(start, QStringLiteral("%1 needs a subtype and data").arg(name));
            *out = binaryValue(name == QStringLiteral("BinData")
                                   ? QByteArray::fromBase64(data.toLatin1())
                                   : QByteArray::fromHex(data.toLatin1()),
                               sub);
            return true;
        }
        if(name == QStringLiteral("UUID")) {
            QString hex = a0;
            hex.remove(QLatin1Char('-'));
            static const QRegularExpression uuid(QStringLiteral("^[0-9a-fA-F]{32}$"));
            if(!uuid.match(hex).hasMatch())
                return failAt(start, QStringLiteral("UUID needs 32 hex digits"));
            *out = binaryValue(QByteArray::fromHex(hex.toLatin1()), 4);
            return true;
        }
        if(name == QStringLiteral("MinKey") || name == QStringLiteral("MaxKey")) {
            *out = wrap(name == QStringLiteral("MinKey") ? QStringLiteral("$minKey")
                                                         : QStringLiteral("$maxKey"),
                        Value::number(QStringLiteral("1")));
            return true;
        }
        if(name == QStringLiteral("RegExp")) {
            *out = regexValue(a0, argText(args, 1));
            return true;
        }
        return failAt(start, QStringLiteral("unknown name '%1'").arg(name));
    }

    bool failAt(int at, const QString &what)
    {
        m_i = at;
        return fail(what);
    }

    QString m_s;
    int m_i = 0;
    QString m_error;
    int m_errorAt = 0;
};

QString quoted(const QString &s)
{
    QString out(QLatin1Char('"'));
    for(const QChar c : s) {
        switch(c.unicode()) {
            case '"':
                out += QStringLiteral("\\\"");
                break;
            case '\\':
                out += QStringLiteral("\\\\");
                break;
            case '\n':
                out += QStringLiteral("\\n");
                break;
            case '\r':
                out += QStringLiteral("\\r");
                break;
            case '\t':
                out += QStringLiteral("\\t");
                break;
            default:
                if(c.unicode() < 0x20)
                    out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
                else
                    out += c;
        }
    }
    return out + QLatin1Char('"');
}

/* the fewest digits that read back as the same double (libbson writes 0.1
 * as 0.10000000000000000555) */
QString shortestDouble(double d)
{
    if(std::isnan(d))
        return QStringLiteral("NaN");
    if(std::isinf(d))
        return d < 0 ? QStringLiteral("-Infinity") : QStringLiteral("Infinity");
    const QString s = QString::number(d, 'g', QLocale::FloatingPointShortest);
    /* a whole double keeps a point, so it reads back as a double */
    return s.contains(QLatin1Char('.')) || s.contains(QLatin1Char('e')) ? s
                                                                        : s + QStringLiteral(".0");
}

/* the BSON type a plain number gets, as canonical Extended JSON */
QString canonicalNumber(const QString &literal)
{
    if(literal == QStringLiteral("Infinity") || literal == QStringLiteral("-Infinity") ||
       literal == QStringLiteral("NaN"))
        return QStringLiteral("{\"$numberDouble\": \"%1\"}").arg(literal);
    const bool fraction = literal.contains(QLatin1Char('.')) ||
                          literal.contains(QLatin1Char('e')) || literal.contains(QLatin1Char('E'));
    if(!fraction) {
        bool ok = false;
        const qint64 n = literal.toLongLong(&ok);
        if(ok && n >= std::numeric_limits<qint32>::min() && n <= std::numeric_limits<qint32>::max())
            return QStringLiteral("{\"$numberInt\": \"%1\"}").arg(n);
        if(ok)
            return QStringLiteral("{\"$numberLong\": \"%1\"}").arg(n);
    }
    return QStringLiteral("{\"$numberDouble\": \"%1\"}").arg(shortestDouble(literal.toDouble()));
}

/* inside these wrappers numbers are plain JSON numbers, not typed */
bool rawNumbersInside(const Value &v)
{
    if(!v.isObject() || v.members.size() != 1)
        return false;
    const QString &k = v.members.front().first;
    return k == QStringLiteral("$timestamp") || k == QStringLiteral("$minKey") ||
           k == QStringLiteral("$maxKey");
}

void writeCanonical(const Value &v, QString &out, bool raw)
{
    switch(v.type) {
        case Value::Null:
            out += QStringLiteral("null");
            return;
        case Value::Bool:
            out += v.boolean ? QStringLiteral("true") : QStringLiteral("false");
            return;
        case Value::String:
            out += quoted(v.text);
            return;
        case Value::Number:
            out += raw ? v.text : canonicalNumber(v.text);
            return;
        case Value::Array:
            out += QLatin1Char('[');
            for(size_t i = 0; i < v.items.size(); ++i) {
                if(i)
                    out += QStringLiteral(", ");
                writeCanonical(v.items[i], out, false);
            }
            out += QLatin1Char(']');
            return;
        case Value::Object: {
            const bool inner = raw || rawNumbersInside(v);
            out += QLatin1Char('{');
            for(size_t i = 0; i < v.members.size(); ++i) {
                if(i)
                    out += QStringLiteral(", ");
                out += quoted(v.members[i].first) + QStringLiteral(": ");
                writeCanonical(v.members[i].second, out, inner);
            }
            out += QLatin1Char('}');
            return;
        }
    }
}

QString memberText(const Value &v, const QString &key)
{
    const Value *m = v.member(key);
    return m ? m->text : QString();
}

QString isoDate(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

/* a wrapper the shell has a notation for, or an empty string */
QString shellWrapper(const Value &v)
{
    if(!v.isObject() || v.members.empty() || v.members.size() > 2)
        return {};
    const QString &k = v.members.front().first;
    const Value &inner = v.members.front().second;
    if(v.members.size() == 1) {
        if(k == QStringLiteral("$oid") && inner.type == Value::String)
            return QStringLiteral("ObjectId(\"%1\")").arg(inner.text);
        if(k == QStringLiteral("$date")) {
            if(inner.isObject() && inner.member(QStringLiteral("$numberLong")))
                return QStringLiteral("ISODate(\"%1\")")
                    .arg(isoDate(memberText(inner, QStringLiteral("$numberLong")).toLongLong()));
            if(inner.type == Value::String)
                return QStringLiteral("ISODate(\"%1\")").arg(inner.text);
            if(inner.type == Value::Number)
                return QStringLiteral("ISODate(\"%1\")")
                    .arg(isoDate(qint64(inner.text.toDouble())));
        }
        if(k == QStringLiteral("$numberInt") && inner.type == Value::String)
            return inner.text;
        if(k == QStringLiteral("$numberLong") && inner.type == Value::String)
            return QStringLiteral("NumberLong(%1)").arg(inner.text);
        if(k == QStringLiteral("$numberDouble") && inner.type == Value::String)
            return inner.text.contains(QStringLiteral("Inf")) || inner.text == QStringLiteral("NaN")
                       ? inner.text
                       : shortestDouble(inner.text.toDouble());
        if(k == QStringLiteral("$numberDecimal") && inner.type == Value::String)
            return QStringLiteral("NumberDecimal(\"%1\")").arg(inner.text);
        if(k == QStringLiteral("$binary") && inner.isObject()) {
            const QByteArray bytes =
                QByteArray::fromBase64(memberText(inner, QStringLiteral("base64")).toLatin1());
            const int sub = memberText(inner, QStringLiteral("subType")).toInt(nullptr, 16);
            if(sub == 4 && bytes.size() == 16) {
                const QString h = QString::fromLatin1(bytes.toHex());
                return QStringLiteral("UUID(\"%1-%2-%3-%4-%5\")")
                    .arg(h.mid(0, 8), h.mid(8, 4), h.mid(12, 4), h.mid(16, 4), h.mid(20));
            }
            return QStringLiteral("BinData(%1, \"%2\")")
                .arg(sub)
                .arg(QString::fromLatin1(bytes.toBase64()));
        }
        if(k == QStringLiteral("$timestamp") && inner.isObject())
            return QStringLiteral("Timestamp(%1, %2)")
                .arg(memberText(inner, QStringLiteral("t")),
                     memberText(inner, QStringLiteral("i")));
        if(k == QStringLiteral("$regularExpression") && inner.isObject()) {
            const QString p = memberText(inner, QStringLiteral("pattern"));
            const QString o = memberText(inner, QStringLiteral("options"));
            /* a literal only when it reads back unchanged */
            if(!p.isEmpty() && !p.contains(QLatin1Char('/')) && !p.contains(QLatin1Char('\n')) &&
               !p.startsWith(QLatin1Char('*')))
                return QLatin1Char('/') + p + QLatin1Char('/') + o;
            return QStringLiteral("RegExp(%1, %2)").arg(quoted(p), quoted(o));
        }
        if(k == QStringLiteral("$minKey"))
            return QStringLiteral("MinKey()");
        if(k == QStringLiteral("$maxKey"))
            return QStringLiteral("MaxKey()");
        if(k == QStringLiteral("$undefined"))
            return QStringLiteral("undefined");
    }
    return {};
}

QString key(const QString &k)
{
    static const QRegularExpression bare(QStringLiteral("^[A-Za-z_$][A-Za-z0-9_$]*$"));
    return bare.match(k).hasMatch() ? k : quoted(k);
}

bool inlineValue(const Value &v)
{
    return (!v.isObject() && !v.isArray()) || !shellWrapper(v).isEmpty();
}

void writeShell(const Value &v, QString &out, bool pretty, int depth)
{
    const QString wrapped = shellWrapper(v);
    if(!wrapped.isEmpty()) {
        out += wrapped;
        return;
    }
    const QString pad = pretty ? QString((depth + 1) * 2, QLatin1Char(' ')) : QString();
    const QString close = pretty ? QString(depth * 2, QLatin1Char(' ')) : QString();
    switch(v.type) {
        case Value::Null:
            out += QStringLiteral("null");
            return;
        case Value::Bool:
            out += v.boolean ? QStringLiteral("true") : QStringLiteral("false");
            return;
        case Value::String:
            out += quoted(v.text);
            return;
        case Value::Number:
            out += v.text;
            return;
        case Value::Array: {
            if(v.items.empty()) {
                out += QStringLiteral("[]");
                return;
            }
            /* a short list of plain values stays on one line */
            QString line;
            if(pretty && std::all_of(v.items.begin(), v.items.end(), inlineValue)) {
                writeShell(v, line, false, 0);
                if(line.size() + depth * 2 <= 76) {
                    out += line;
                    return;
                }
            }
            out += pretty ? QStringLiteral("[\n") : QStringLiteral("[");
            for(size_t i = 0; i < v.items.size(); ++i) {
                out += pad;
                writeShell(v.items[i], out, pretty, depth + 1);
                if(i + 1 < v.items.size())
                    out += pretty ? QStringLiteral(",") : QStringLiteral(", ");
                if(pretty)
                    out += QLatin1Char('\n');
            }
            out += close + QLatin1Char(']');
            return;
        }
        case Value::Object: {
            if(v.members.empty()) {
                out += QStringLiteral("{}");
                return;
            }
            out += pretty ? QStringLiteral("{\n") : QStringLiteral("{");
            for(size_t i = 0; i < v.members.size(); ++i) {
                out += pad + key(v.members[i].first) + QStringLiteral(": ");
                writeShell(v.members[i].second, out, pretty, depth + 1);
                if(i + 1 < v.members.size())
                    out += pretty ? QStringLiteral(",") : QStringLiteral(", ");
                if(pretty)
                    out += QLatin1Char('\n');
            }
            out += close + QLatin1Char('}');
            return;
        }
    }
}

} // namespace

bool parse(const QString &text, Value *out, QString *error)
{
    return Parser(text).parse(out, error);
}

bool parseDocument(const QString &text, Value *out, QString *error)
{
    if(text.trimmed().isEmpty()) {
        *out = Value::object();
        return true;
    }
    if(!parse(text, out, error))
        return false;
    if(out->isObject())
        return true;
    if(error)
        *error = QStringLiteral("a document ({…}) expected");
    return false;
}

bool parsePipeline(const QString &text, Value *out, QString *error)
{
    if(text.trimmed().isEmpty()) {
        *out = Value::array();
        return true;
    }
    if(!parse(text, out, error))
        return false;
    if(out->isObject())
        *out = Value::array({*out});
    if(!out->isArray()) {
        if(error)
            *error = QStringLiteral("a pipeline ([{…}, …]) expected");
        return false;
    }
    for(size_t i = 0; i < out->items.size(); ++i)
        if(!out->items[i].isObject()) {
            if(error)
                *error = QStringLiteral("stage %1 isn't a document ({…})").arg(i + 1);
            return false;
        }
    return true;
}

QByteArray toCanonical(const Value &value)
{
    QString out;
    writeCanonical(value, out, false);
    return out.toUtf8();
}

QString toShell(const Value &value, bool pretty)
{
    QString out;
    writeShell(value, out, pretty, 0);
    return out;
}

QString cell(const Value &value, int maxLength)
{
    QString s = value.type == Value::String ? value.text : toShell(value, false);
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if(s.size() > maxLength)
        s = s.left(maxLength - 1) + QStringLiteral("…");
    return s;
}

} // namespace MongoJson
