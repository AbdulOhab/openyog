/* OpenYog — the text MongoDB documents are read and written as in the Mongo
 * tab: the shell's own notation (mongosh / the legacy mongo shell), and the
 * canonical Extended JSON libbson takes and gives.
 *
 * What the user types — a filter, a sort, a pipeline, a document to insert
 * or an edited one — may be strict JSON or shell notation:
 *
 *     {name: 'Alice', age: {$gte: 30}, _id: ObjectId("65a…"),
 *      hired: ISODate("2024-01-15"), n: NumberLong(5), tag: /^a/i}
 *
 * (unquoted keys, single quotes, trailing commas, comments, the shell's
 * helpers and regex literals). parse() reads it into a Value with every
 * helper turned into its Extended JSON wrapper ({"$oid": …}), and
 * toCanonical() writes canonical Extended JSON for bson_new_from_json(),
 * deciding each plain number's BSON type the way the shell does: an integer
 * that fits is an int32, a bigger one an int64, anything with a point or an
 * exponent a double.
 *
 * Documents from the server come as canonical Extended JSON, which parse()
 * reads too; toShell() writes them back in shell notation, keeping each
 * value's type (an int64 is NumberLong(5), a whole double 5.0), so an edited
 * document goes back with the types it came with. Objects keep their
 * members' order: a document's field order is part of it.
 *
 * Plain Qt, no libbson: the Mongo tab's libbson calls only ever see
 * canonical Extended JSON. */
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace MongoJson {

struct Value
{
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool boolean = false;
    QString text;             /* a String's text; a Number's literal, as written */
    std::vector<Value> items; /* Array */
    std::vector<std::pair<QString, Value>> members; /* Object, in order */

    static Value string(const QString &s);
    static Value number(const QString &literal);
    static Value object(std::vector<std::pair<QString, Value>> members = {});
    static Value array(std::vector<Value> items = {});

    /* an Object's member; nullptr when it has none of that name */
    const Value *member(const QString &key) const;
    bool isObject() const
    {
        return type == Object;
    }
    bool isArray() const
    {
        return type == Array;
    }
};

/* shell notation or JSON; false with "line:column: what" in *error */
bool parse(const QString &text, Value *out, QString *error = nullptr);
/* parse() for a document: empty text is {} and anything but an object is
 * an error */
bool parseDocument(const QString &text, Value *out, QString *error = nullptr);
/* a pipeline: an array of stages (objects), or a single stage */
bool parsePipeline(const QString &text, Value *out, QString *error = nullptr);

/* canonical Extended JSON, for bson_new_from_json() */
QByteArray toCanonical(const Value &value);

/* shell notation; indented over several lines when `pretty` */
QString toShell(const Value &value, bool pretty = true);

/* one line for a grid cell: a string as it is, everything else in shell
 * notation, cut to `maxLength` with "…" */
QString cell(const Value &value, int maxLength = 200);

/* a new ObjectId's 24 hex digits: 4 bytes of seconds since the epoch, then
 * 5 random bytes kept for the process, then a 3-byte counter (the BSON
 * spec's layout) */
QString newObjectId();

} // namespace MongoJson
