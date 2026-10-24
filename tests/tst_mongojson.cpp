/* OpenYog — unit tests for qt/MongoJson: shell notation in, canonical
 * Extended JSON out, and documents back in shell notation with their types
 * and field order. */
#include "MongoJson.h"

#include <QRegularExpression>
#include <QTest>

using namespace MongoJson;

namespace {

QByteArray canonical(const QString &shell)
{
    Value v;
    QString error;
    if(!parse(shell, &v, &error))
        return "ERROR " + error.toUtf8();
    return toCanonical(v);
}

QString shell(const QByteArray &canonicalJson, bool pretty = false)
{
    Value v;
    QString error;
    if(!parse(QString::fromUtf8(canonicalJson), &v, &error))
        return QStringLiteral("ERROR ") + error;
    return toShell(v, pretty);
}

} // namespace

class TestMongoJson : public QObject
{
    Q_OBJECT
private slots:
    void shellNotation()
    {
        /* unquoted keys, single quotes, trailing commas, comments, operators */
        QCOMPARE(canonical("{name: 'Alice', age: {$gte: 30,}, // who\n 'a.b': [1, 2,]}"),
                 QByteArray(R"({"name": "Alice", "age": {"$gte": {"$numberInt": "30"}}, )"
                            R"("a.b": [{"$numberInt": "1"}, {"$numberInt": "2"}]})"));
        QCOMPARE(canonical(R"({"strict": "json", "n": null, "t": true})"),
                 QByteArray(R"({"strict": "json", "n": null, "t": true})"));
        QCOMPARE(canonical("{s: 'it\\'s \\u0995 \"q\"\\n'}"),
                 /* (not a raw string: moc reads its apostrophe as a char literal) */
                 QByteArray("{\"s\": \"it's ক \\\"q\\\"\\n\"}"));
        /* bare dotted paths, for filters on embedded fields */
        QCOMPARE(canonical("{address.city: 'Dhaka'}"), QByteArray(R"({"address.city": "Dhaka"})"));
    }

    void numberTypes()
    {
        QCOMPARE(
            canonical("[5, -7, 2147483648, 9223372036854775807, 5.0, 1.5e3, 0.1]"),
            QByteArray(R"([{"$numberInt": "5"}, {"$numberInt": "-7"}, )"
                       R"({"$numberLong": "2147483648"}, {"$numberLong": "9223372036854775807"}, )"
                       R"({"$numberDouble": "5.0"}, {"$numberDouble": "1500.0"}, )"
                       R"({"$numberDouble": "0.1"}])"));
        QCOMPARE(canonical("[NumberLong(5), NumberLong('6'), NumberInt(7), Long(8), Int32(9)]"),
                 QByteArray(R"([{"$numberLong": "5"}, {"$numberLong": "6"}, {"$numberInt": "7"}, )"
                            R"({"$numberLong": "8"}, {"$numberInt": "9"}])"));
        QCOMPARE(canonical("[NumberDecimal('1.10'), Double(2), Infinity, -Infinity, NaN]"),
                 QByteArray(R"([{"$numberDecimal": "1.10"}, {"$numberDouble": "2.0"}, )"
                            R"({"$numberDouble": "Infinity"}, {"$numberDouble": "-Infinity"}, )"
                            R"({"$numberDouble": "NaN"}])"));
        QVERIFY(canonical("NumberInt(3000000000)").startsWith("ERROR 1:1: NumberInt needs"));
    }

    void helpers()
    {
        QCOMPARE(canonical(R"(ObjectId("65A1B2C3D4E5F60718293A4B"))"),
                 QByteArray(R"({"$oid": "65a1b2c3d4e5f60718293a4b"})"));
        QCOMPARE(canonical(R"(ISODate("2024-01-15T10:30:00.250Z"))"),
                 QByteArray(R"({"$date": {"$numberLong": "1705314600250"}})"));
        /* no offset: UTC, like the shell; a date alone is its midnight */
        QCOMPARE(canonical(R"(new Date("2024-01-15"))"),
                 QByteArray(R"({"$date": {"$numberLong": "1705276800000"}})"));
        QCOMPARE(canonical(R"(ISODate("2024-01-15T16:00:00+06:00"))"),
                 QByteArray(R"({"$date": {"$numberLong": "1705312800000"}})"));
        QCOMPARE(canonical("new Date(0)"), QByteArray(R"({"$date": {"$numberLong": "0"}})"));
        QCOMPARE(canonical("Timestamp(1700000000, 3)"),
                 QByteArray(R"({"$timestamp": {"t": 1700000000, "i": 3}})"));
        QCOMPARE(canonical("[MinKey, MaxKey()]"),
                 QByteArray(R"([{"$minKey": 1}, {"$maxKey": 1}])"));
        QCOMPARE(canonical(R"(UUID("0e3f5c1a-2b4d-4e6f-8a9b-0c1d2e3f4a5b"))"),
                 R"({"$binary": {"base64": ")" +
                     QByteArray::fromHex("0e3f5c1a2b4d4e6f8a9b0c1d2e3f4a5b").toBase64() +
                     R"(", "subType": "04"}})");
        QCOMPARE(canonical(R"(BinData(0, "AAEC"))"),
                 QByteArray(R"({"$binary": {"base64": "AAEC", "subType": "00"}})"));
        /* regex options sorted, the ones BSON doesn't know dropped */
        QCOMPARE(canonical("{n: /^a[/]b\\/c/mgi}"),
                 QByteArray(R"({"n": {"$regularExpression": {"pattern": "^a[/]b\\/c", )"
                            R"("options": "im"}}})"));
        QVERIFY(QRegularExpression(QStringLiteral("^\\{\"\\$oid\": \"[0-9a-f]{24}\"\\}$"))
                    .match(QString::fromLatin1(canonical("ObjectId()")))
                    .hasMatch());
        QVERIFY(canonical("ObjectId('xyz')").contains("ObjectId needs 24 hex digits"));
        QVERIFY(canonical("Frob(1)").contains("unknown name 'Frob'"));
    }

    void errorsSayWhere()
    {
        QCOMPARE(canonical("{a: 1,\n  b: }"), QByteArray("ERROR 2:6: a value expected, not '}'"));
        QCOMPARE(canonical("{a: 1"), QByteArray("ERROR 1:6: '}' expected at the end"));
        QCOMPARE(canonical("{a: 'x}"), QByteArray("ERROR 1:8: unterminated string"));
        QCOMPARE(canonical("{a: 1} 2"), QByteArray("ERROR 1:8: unexpected '2' after the value"));
        Value v;
        QString error;
        QVERIFY(!parseDocument("[1]", &v, &error));
        QVERIFY(parseDocument("  ", &v, &error) && v.isObject() && v.members.empty());
        QVERIFY(parsePipeline("{$match: {}}", &v) && v.items.size() == 1);
        QVERIFY(!parsePipeline("[{$match: {}}, 3]", &v, &error));
        QCOMPARE(error, QStringLiteral("stage 2 isn't a document ({…})"));
    }

    void documentsBackInShellNotation()
    {
        /* what libbson's canonical Extended JSON looks like */
        const QByteArray doc =
            R"({"_id": {"$oid": "65a1b2c3d4e5f60718293a4b"}, "name": "রহিম", )"
            R"("age": {"$numberInt": "30"}, "visits": {"$numberLong": "5"}, )"
            R"("score": {"$numberDouble": "4.0"}, "ratio": {"$numberDouble": "0.10000000000000000555"}, )"
            R"("price": {"$numberDecimal": "9.99"}, )"
            R"("hired": {"$date": {"$numberLong": "1705314600250"}}, )"
            R"("tags": ["a", "b"], "loc": {"city": "Dhaka", "zip": null}, )"
            R"("ts": {"$timestamp": {"t": 1700000000, "i": 3}}, "re": {"$regularExpression": )"
            R"({"pattern": "^a", "options": "i"}}, "bin": {"$binary": {"base64": "AAEC", "subType": "00"}}})";
        const QString one = shell(doc);
        QCOMPARE(
            one,
            QStringLiteral(
                "{_id: ObjectId(\"65a1b2c3d4e5f60718293a4b\"), name: \"রহিম\", age: 30, "
                "visits: NumberLong(5), score: 4.0, ratio: 0.1, price: NumberDecimal(\"9.99\"), "
                "hired: ISODate(\"2024-01-15T10:30:00.250Z\"), tags: [\"a\", \"b\"], "
                "loc: {city: \"Dhaka\", zip: null}, ts: Timestamp(1700000000, 3), re: /^a/i, "
                "bin: BinData(0, \"AAEC\")}"));
        /* and back: the same types, the same order */
        QCOMPARE(canonical(one),
                 canonical(QString::fromUtf8(doc)).replace("0.10000000000000000555", "0.1"));
        const QString pretty = shell(doc, true);
        QVERIFY(pretty.startsWith("{\n  _id: ObjectId(\"65a1b2c3d4e5f60718293a4b\"),\n  name:"));
        QVERIFY(pretty.contains("\n  tags: [\"a\", \"b\"],\n"));
        QVERIFY(pretty.contains("\n  loc: {\n    city: \"Dhaka\",\n    zip: null\n  },\n"));
        QCOMPARE(canonical(pretty), canonical(one));
        /* keys that aren't identifiers stay quoted */
        QCOMPARE(shell(R"({"a b": 1, "$set": 2, "x.y": 3})"),
                 QStringLiteral("{\"a b\": 1, $set: 2, \"x.y\": 3}"));
        /* a regex with a slash: written so it reads back the same */
        QCOMPARE(shell(R"({"$regularExpression": {"pattern": "a/b", "options": ""}})"),
                 QStringLiteral("RegExp(\"a/b\", \"\")"));
        QCOMPARE(canonical(shell(R"({"$regularExpression": {"pattern": "a/b", "options": ""}})")),
                 QByteArray(R"({"$regularExpression": {"pattern": "a/b", "options": ""}})"));
    }

    void cells()
    {
        Value v;
        QVERIFY(parse(R"({"s": "two\nlines", "n": {"$numberInt": "5"}, "o": {"a": [1, 2]}})", &v));
        QCOMPARE(cell(*v.member("s")), QStringLiteral("two lines"));
        QCOMPARE(cell(*v.member("n")), QStringLiteral("5"));
        QCOMPARE(cell(*v.member("o")), QStringLiteral("{a: [1, 2]}"));
        QCOMPARE(cell(Value::string(QString(300, QLatin1Char('x'))), 10),
                 QStringLiteral("xxxxxxxxx…"));
    }

    void objectIdsAreNew()
    {
        const QString a = newObjectId(), b = newObjectId();
        QCOMPARE(a.size(), 24);
        QVERIFY(a != b);
        QCOMPARE(a.mid(8, 10), b.mid(8, 10)); /* the process's random part */
    }
};

QTEST_GUILESS_MAIN(TestMongoJson)
#include "tst_mongojson.moc"
