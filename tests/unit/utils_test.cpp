/**
 * (c) 2019 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "megafs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mega/base64.h>
#include <mega/db.h>
#include <mega/db/sqlite.h>
#include <mega/filesystem.h>
#include <mega/json.h>
#include <mega/process.h>
#include <mega/scoped_helpers.h>
#include <mega/utils.h>

TEST(utils, readLines)
{
    static const std::string input =
        "\r"
        "\n"
        "     \r"
        "  a\r\n"
        "b\n"
        "c\r"
        "  d  \r"
        "     \n"
        "efg\n";
    static const std::vector<std::string> expected = {
        "  a",
        "b",
        "c",
        "  d  ",
        "efg"
    };

    std::vector<std::string> output;

    ASSERT_TRUE(::mega::readLines(input, output));
    ASSERT_EQ(output.size(), expected.size());
    ASSERT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
}

TEST(Filesystem, EscapesControlCharactersIfNecessary)
{
    using namespace mega;

    FSACCESS_CLASS fsAccess;

    // Cloud should never receive unescaped control characters.
    // If it does, make sure we escape accordingly.
    const string input("\0\r\n", 3);

    // Most restrictive escaping policy.
    {
        string name = input;

        fsAccess.escapefsincompatible(&name, FS_UNKNOWN);

        ASSERT_EQ(name, "%00%0d%0a");
    }

    // Least restrictive escaping policy.
    {
        string name = input;

        fsAccess.escapefsincompatible(&name, FS_EXT);

        ASSERT_EQ(name, "%00\r\n");
    }
}

TEST(Filesystem, EscapesReservedCharacters)
{
    using namespace mega;

    // All of these characters will be escaped.
    string name = "\\/:?\"<>|*";   // not % anymore (for now)

    // Generate expected result.
    ostringstream osstream;

    for (auto character : name)
    {
        osstream << "%"
                 << std::hex
                 << std::setfill('0')
                 << std::setw(2)
                 << +character;
    }

    // Use most restrictive escaping policy.
    FSACCESS_CLASS fsAccess;
    fsAccess.escapefsincompatible(&name, FS_UNKNOWN);

    // Was the string correctly escaped?
    ASSERT_EQ(name, osstream.str());
}

TEST(Filesystem, UnescapesEscapedCharacters)
{
    using namespace mega;

    FSACCESS_CLASS fsAccess;

    // All of these characters will be escaped.
    string name = "%\\/:?\"<>|*";
    fsAccess.escapefsincompatible(&name, FS_UNKNOWN);

    // Everything will be unescaped except for control characters.
    fsAccess.unescapefsincompatible(&name);

    // Was the string correctly unescaped?
    ASSERT_STREQ(name.c_str(), "%\\/:?\"<>|*");
}


TEST(CharacterSet, IterateUtf8)
{
    using ::mega::unicodeCodepointIterator;

    // Single code-unit.
    {
        auto it = unicodeCodepointIterator("abc");

        EXPECT_FALSE(it.end());
        EXPECT_EQ(it.get(), 'a');
        EXPECT_EQ(it.get(), 'b');
        EXPECT_EQ(it.get(), 'c');
        EXPECT_TRUE(it.end());
        EXPECT_EQ(it.get(), '\0');
    }

    // Multiple code-unit.
    {
        auto it = unicodeCodepointIterator("q\xf0\x90\x80\x80r");

        EXPECT_FALSE(it.end());
        EXPECT_EQ(it.get(), 'q');
        EXPECT_EQ(it.get(), 0x10000);
        EXPECT_EQ(it.get(), 'r');
        EXPECT_TRUE(it.end());
        EXPECT_EQ(it.get(), '\0');
    }
}

TEST(CharacterSet, IterateUtf16)
{
    using mega::unicodeCodepointIterator;

    // Single code-unit.
    {
        auto it = unicodeCodepointIterator(L"abc");

        EXPECT_FALSE(it.end());
        EXPECT_EQ(it.get(), L'a');
        EXPECT_EQ(it.get(), L'b');
        EXPECT_EQ(it.get(), L'c');
        EXPECT_TRUE(it.end());
        EXPECT_EQ(it.get(), L'\0');
    }

    // Multiple code-unit.
    {
        auto it = unicodeCodepointIterator(L"q\xd800\xdc00r");

        EXPECT_FALSE(it.end());
        EXPECT_EQ(it.get(), L'q');
        EXPECT_EQ(it.get(), 0x10000);
        EXPECT_EQ(it.get(), L'r');
        EXPECT_TRUE(it.end());
        EXPECT_EQ(it.get(), L'\0');
    }
}

using namespace mega;
using namespace std;

// Disambiguate between Microsoft's FileSystemType.
using ::mega::FileSystemType;

class ComparatorTest
  : public ::testing::Test
{
public:
    template<typename T, typename U>
    int compare(const T& lhs, const U& rhs) const
    {
        return compareUtf(lhs, true, rhs, true, false);
    }

    template<typename T, typename U>
    int ciCompare(const T& lhs, const U& rhs) const
    {
        return compareUtf(lhs, true, rhs, true, true);
    }

    LocalPath fromAbsPath(const string& s)
    {
        return LocalPath::fromAbsolutePath(s);
    }

    LocalPath fromRelPath(const string& s)
    {
        return LocalPath::fromRelativePath(s);
    }

}; // ComparatorTest

TEST_F(ComparatorTest, CompareLocalPaths)
{
    LocalPath lhs;
    LocalPath rhs;

    // Case insensitive
    {
        // Make sure basic characters are uppercased.
        lhs = fromRelPath("abc");
        rhs = fromRelPath("ABC");

        EXPECT_EQ(ciCompare(lhs, rhs), 0);
        EXPECT_EQ(ciCompare(rhs, lhs), 0);

        // Make sure comparison invariants are not violated.
        lhs = fromRelPath("abc");
        rhs = fromRelPath("ABCD");

        EXPECT_LT(ciCompare(lhs, rhs), 0);
        EXPECT_GT(ciCompare(rhs, lhs), 0);

        // Make sure escapes are decoded.
        lhs = fromRelPath("a%30b");
        rhs = fromRelPath("A0B");

        EXPECT_EQ(ciCompare(lhs, rhs), 0);
        EXPECT_EQ(ciCompare(rhs, lhs), 0);

        // Make sure decoded characters are uppercased.
        lhs = fromRelPath("%61%62%63");
        rhs = fromRelPath("ABC");

        EXPECT_EQ(ciCompare(lhs, rhs), 0);
        EXPECT_EQ(ciCompare(rhs, lhs), 0);

        // Invalid escapes are left as-is.
        lhs = fromRelPath("a%qb%");
        rhs = fromRelPath("A%qB%");

        EXPECT_EQ(ciCompare(lhs, rhs), 0);
        EXPECT_EQ(ciCompare(rhs, lhs), 0);
    }

    // Case sensitive
    {
        // Basic comparison.
        lhs = fromRelPath("abc");

        EXPECT_EQ(compare(lhs, lhs), 0);

        // Make sure characters are not uppercased.
        rhs = fromRelPath("ABC");

        EXPECT_NE(compare(lhs, rhs), 0);
        EXPECT_NE(compare(rhs, lhs), 0);

        // Make sure comparison invariants are not violated.
        lhs = fromRelPath("abc");
        rhs = fromRelPath("abcd");

        EXPECT_LT(compare(lhs, rhs), 0);
        EXPECT_GT(compare(rhs, lhs), 0);

        // Make sure escapes are decoded.
        lhs = fromRelPath("a%30b");
        rhs = fromRelPath("a0b");

        EXPECT_EQ(compare(lhs, rhs), 0);
        EXPECT_EQ(compare(rhs, lhs), 0);

        // Invalid escapes are left as-is.
        lhs = fromRelPath("a%qb%");

        EXPECT_EQ(compare(lhs, lhs), 0);

#ifdef _WIN32
        // Non-UNC prefixes should be skipped.
        lhs = fromAbsPath("\\\\?\\C:\\");
        rhs = fromAbsPath("C:\\");

        EXPECT_EQ(compare(lhs, rhs), 0);
        EXPECT_EQ(compare(rhs, lhs), 0);

        lhs = fromAbsPath("\\\\.\\C:\\");
        rhs = fromAbsPath("C:\\");

        EXPECT_EQ(compare(lhs, rhs), 0);
        EXPECT_EQ(compare(rhs, lhs), 0);

        //// Prefixes should only be removed from absolute paths.
        //lhs = fromAbsPath("\\\\?\\X");
        //rhs = fromAbsPath("X");

        //EXPECT_NE(compare(lhs, rhs), 0);
        //EXPECT_NE(compare(rhs, lhs), 0);
#endif // _WIN32
    }

    // Filesystem-specific
    {
        lhs = fromRelPath("a\7%30b%31c");
        rhs = fromRelPath("A%070B1C");

    }
}

TEST_F(ComparatorTest, CompareLocalPathAgainstString)
{
    LocalPath lhs;
    string rhs;

    // Case insensitive
    {
        // Simple comparison.
        lhs = fromRelPath("abc");
        rhs = "ABC";

        EXPECT_EQ(ciCompare(lhs, rhs), 0);

        // Invariants.
        lhs = fromRelPath("abc");
        rhs = "abcd";

        EXPECT_LT(ciCompare(lhs, rhs), 0);

        lhs = fromRelPath("abcd");
        rhs = "abc";

        EXPECT_GT(ciCompare(lhs, rhs), 0);

        // All local escapes are decoded.
        lhs = fromRelPath("a%30b%31c");
        rhs = "A0b1C";

        EXPECT_EQ(ciCompare(lhs, rhs), 0);

        // Escapes are uppercased.
        lhs = fromRelPath("%61%62%63");
        rhs = "ABC";

        EXPECT_EQ(ciCompare(lhs, rhs), 0);

        // Invalid escapes are left as-is.
        lhs = fromRelPath("a%qb%");
        rhs = "A%QB%";

        EXPECT_EQ(ciCompare(lhs, rhs), 0);
    }

    // Case sensitive
    {
        // Simple comparison.
        lhs = fromRelPath("abc");
        rhs = "abc";

        EXPECT_EQ(compare(lhs, rhs), 0);

        // Invariants.
        rhs = "abcd";

        EXPECT_LT(compare(lhs, rhs), 0);

        lhs = fromRelPath("abcd");
        rhs = "abc";

        EXPECT_GT(compare(lhs, rhs), 0);

        // All local escapes are decoded.
        lhs = fromRelPath("a%30b%31c");
        rhs = "a0b1c";

        EXPECT_EQ(compare(lhs, rhs), 0);

        // Invalid escapes left as-is.
        lhs = fromRelPath("a%qb%r");
        rhs = "a%qb%r";

        EXPECT_EQ(compare(lhs, rhs), 0);

#ifdef _WIN32
        // Non-UNC prefixes should be skipped.
        lhs = fromAbsPath("\\\\?\\C:\\");
        rhs = "C:\\";

        EXPECT_EQ(compare(lhs, rhs), 0);
        EXPECT_EQ(compare(rhs, lhs), 0);

        lhs = fromAbsPath("\\\\.\\C:\\");
        rhs = "C:\\";

        EXPECT_EQ(compare(lhs, rhs), 0);
        EXPECT_EQ(compare(rhs, lhs), 0);

        //// Prefixes should only be removed from absolute paths.
        //lhs = fromAbsPath("\\\\?\\X");
        //rhs = "X";

        //EXPECT_NE(compare(lhs, rhs), 0);
        //EXPECT_NE(compare(rhs, lhs), 0);
#endif // _WIN32
    }

    // Filesystem-specific
    {
        lhs = fromRelPath("a\7%30b%31c");
        rhs = "A%070B1C";

    }
}

TEST(Conversion, HexVal)
{
    // Decimal [0-9]
    for (int i = 0x30; i < 0x3a; ++i)
    {
        EXPECT_EQ(hexval(i), i - 0x30);
    }

    // Lowercase hexadecimal [a-f]
    for (int i = 0x41; i < 0x47; ++i)
    {
        EXPECT_EQ(hexval(i), i - 0x37);
    }

    // Uppercase hexadeimcal [A-F]
    for (int i = 0x61; i < 0x67; ++i)
    {
        EXPECT_EQ(hexval(i), i - 0x57);
    }
}

TEST(URLCodec, Escape)
{
    string input = "abc123!@#$%^&*()";
    string output;

    URLCodec::escape(&input, &output);
    EXPECT_EQ(output, "abc123%21%40%23%24%25%5e%26%2a%28%29");

    string input2 = "EF字幕组 编织记忆 stitchers S02E10.mp4";
    string output2;

    URLCodec::escape(&input2, &output2);
    EXPECT_EQ(output2, "EF%e5%ad%97%e5%b9%95%e7%bb%84%20%e7%bc%96%e7%bb%87%e8%ae%b0%e5%bf%86%20stitchers%20S02E10.mp4");
}


TEST(URLCodec, Unescape)
{
    string input = "a%4a%4Bc";
    string output;

    URLCodec::unescape(&input, &output);
    EXPECT_EQ(output, "aJKc");
}

TEST(URLCodec, UnescapeInvalidEscape)
{
    string input;
    string output;

    // First character is invalid.
    input = "a%qbc";
    URLCodec::unescape(&input, &output);
    EXPECT_EQ(output, "a%qbc");

    // Second character is invalid.
    input = "a%bqc";
    URLCodec::unescape(&input, &output);
    EXPECT_EQ(output, "a%bqc");
}

TEST(URLCodec, UnescapeShortEscape)
{
    string input;
    string output;

    // No hex digits.
    input = "a%";
    URLCodec::unescape(&input, &output);
    EXPECT_EQ(output, "a%");

    // Single hex digit.
    input = "a%a";
    URLCodec::unescape(&input, &output);
    EXPECT_EQ(output, "a%a");
}


TEST(Filesystem, isContainingPathOf)
{
    using namespace mega;

#ifdef _WIN32
#define SEP "\\"
#else // _WIN32
#define SEP "/"
#endif // ! _WIN32

    LocalPath lhs;
    LocalPath rhs;
    size_t pos;

    // lhs does not contain rhs.
    constexpr const size_t sentinel = std::numeric_limits<size_t>::max();
    pos = sentinel;
    lhs = LocalPath::fromRelativePath("a" SEP "b");
    rhs = LocalPath::fromRelativePath("a" SEP "c");

    EXPECT_FALSE(lhs.isContainingPathOf(rhs, &pos));
    EXPECT_EQ(pos, sentinel);

    // lhs does not contain rhs.
    // they do, however, share a common prefix.
    pos = sentinel;
    lhs = LocalPath::fromRelativePath("a");
    rhs = LocalPath::fromRelativePath("ab");

    EXPECT_FALSE(lhs.isContainingPathOf(rhs, &pos));
    EXPECT_EQ(pos, sentinel);

    // lhs contains rhs.
    // no trailing separator.
    pos = sentinel;
    lhs = LocalPath::fromRelativePath("a");
    rhs = LocalPath::fromRelativePath("a" SEP "b");

    EXPECT_TRUE(lhs.isContainingPathOf(rhs, &pos));
    EXPECT_EQ(pos, 2u);

    // trailing separator.
    pos = sentinel;
    lhs = LocalPath::fromRelativePath("a" SEP);
    rhs = LocalPath::fromRelativePath("a" SEP "b");

    EXPECT_TRUE(lhs.isContainingPathOf(rhs, &pos));
    EXPECT_EQ(pos, 2u);

    // lhs contains itself.
    pos = sentinel;
    lhs = LocalPath::fromRelativePath("a" SEP "b");

    EXPECT_TRUE(lhs.isContainingPathOf(lhs, &pos));
    EXPECT_EQ(pos, 3u);

#ifdef _WIN32
    // case insensitive.
    pos = sentinel;
    lhs = LocalPath::fromRelativePath("a" SEP "B");
    rhs = LocalPath::fromRelativePath("A" SEP "b");

    EXPECT_TRUE(lhs.isContainingPathOf(rhs, &pos));
    EXPECT_EQ(pos, 3u);
#endif // _WIN32

#undef SEP
}

class SqliteDBTest
  : public ::testing::Test
{
public:
        SqliteDBTest()
          : Test()
          , fsAccess()
          , name("test")
          , rng()
          , rootPath(LocalPath::fromAbsolutePath("."))
        {
            // Get the current path.
            bool result = fsAccess.cwd(rootPath);
            if (!result)
                assert(result);

            // Create temporary DB root path.
            rootPath.appendWithSeparator(
                LocalPath::fromRelativePath("db"), false);

            // Make sure our root path is clear.
            fsAccess.emptydirlocal(rootPath);
            fsAccess.rmdirlocal(rootPath);

            // Create root path.
            result = fsAccess.mkdirlocal(rootPath, false, true);
            if (!result)
                assert(result);
        }

        ~SqliteDBTest()
        {
            // Remove temporary root path.
            fsAccess.emptydirlocal(rootPath);

            bool result = fsAccess.rmdirlocal(rootPath);
            if (!result)
                assert(result);
        }

        FSACCESS_CLASS fsAccess;
        string name;
        PrnGen rng;
        LocalPath rootPath;
}; // SqliteDBTest

TEST_F(SqliteDBTest, CreateCurrent)
{
    SqliteDbAccess dbAccess(rootPath);

    // Assume databases are in legacy format until proven otherwise.
    EXPECT_EQ(dbAccess.currentDbVersion, DbAccess::LEGACY_DB_VERSION);

    // Create a new database.
    DbTablePtr dbTable(dbAccess.openTableWithNodes(rng, fsAccess, name, 0, nullptr));

    // Was the database created successfully?
    ASSERT_TRUE(!!dbTable);

    // New databases should not be in the legacy format.
    EXPECT_EQ(dbAccess.currentDbVersion, DbAccess::DB_VERSION);

}

TEST_F(SqliteDBTest, OpenCurrent)
{
    // Create a dummy database.
    {
        SqliteDbAccess dbAccess(rootPath);

        EXPECT_EQ(dbAccess.currentDbVersion, DbAccess::LEGACY_DB_VERSION);

        DbTablePtr dbTable(dbAccess.openTableWithNodes(rng, fsAccess, name, 0, nullptr));
        ASSERT_TRUE(!!dbTable);

        EXPECT_EQ(dbAccess.currentDbVersion, DbAccess::DB_VERSION);
    }

    // Open the database.
    SqliteDbAccess dbAccess(rootPath);

    EXPECT_EQ(dbAccess.currentDbVersion, DbAccess::LEGACY_DB_VERSION);

    DbTablePtr dbTable(dbAccess.openTableWithNodes(rng, fsAccess, name, 0, nullptr));
    EXPECT_TRUE(!!dbTable);

    EXPECT_EQ(dbAccess.currentDbVersion, DbAccess::DB_VERSION);
}

TEST_F(SqliteDBTest, ProbeCurrent)
{
    SqliteDbAccess dbAccess(rootPath);

    // Create dummy database.
    {
        auto dbFile =
          dbAccess.databasePath(fsAccess,
                                name,
                                DbAccess::DB_VERSION);

        auto fileAccess = fsAccess.newfileaccess(false);
        EXPECT_TRUE(fileAccess->fopen(dbFile, false, true, FSLogging::logOnError));
    }

    EXPECT_TRUE(dbAccess.probe(fsAccess, name));
}

TEST_F(SqliteDBTest, ProbeLegacy)
{
    SqliteDbAccess dbAccess(rootPath);

    // Create dummy database.
    {
        auto dbFile =
          dbAccess.databasePath(fsAccess,
                                name,
                                DbAccess::LEGACY_DB_VERSION);

        auto fileAccess = fsAccess.newfileaccess(false);
        EXPECT_TRUE(fileAccess->fopen(dbFile, false, true, FSLogging::logOnError));
    }

    EXPECT_TRUE(dbAccess.probe(fsAccess, name));
}

TEST_F(SqliteDBTest, ProbeNone)
{
    SqliteDbAccess dbAccess(rootPath);
    EXPECT_FALSE(dbAccess.probe(fsAccess, name));
}


TEST_F(SqliteDBTest, RootPath)
{
    SqliteDbAccess dbAccess(rootPath);
    EXPECT_EQ(dbAccess.rootPath(), rootPath);
}

#ifdef WIN32
#define SEP "\\"
#else // WIN32
#define SEP "/"
#endif // ! WIN32

TEST(LocalPath, AppendWithSeparator)
{
    LocalPath source;
    LocalPath target = LocalPath::fromRelativePath("");

    // Doesn't add a separator if the target is empty.
    source = LocalPath::fromRelativePath("a");
    target.appendWithSeparator(source, false);

    EXPECT_EQ(target.toPath(false), "a");

    // Doesn't add a separator if the source begins with one.
    source = LocalPath::fromRelativePath(SEP "b");
    target = LocalPath::fromRelativePath("a");

    target.appendWithSeparator(source, true);
    EXPECT_EQ(target.toPath(false), "a" SEP "b");

    // Doesn't add a separator if the target ends with one.
    source = LocalPath::fromRelativePath("b");
    target = LocalPath::fromRelativePath("a" SEP);

    target.appendWithSeparator(source, true);
    EXPECT_EQ(target.toPath(false), "a" SEP "b");

    // Adds a separator when:
    // - source doesn't begin with one.
    // - target doesn't end with one.
    target = LocalPath::fromRelativePath("a");

    target.appendWithSeparator(source, true);
    EXPECT_EQ(target.toPath(false), "a" SEP "b");
}

TEST(LocalPath, PrependWithSeparator)
{
    LocalPath source;
    LocalPath target;

    // No separator if target is empty.
    source = LocalPath::fromRelativePath("b");

    target.prependWithSeparator(source);
    EXPECT_EQ(target.toPath(false), "b");

    // No separator if target begins with separator.
    target = LocalPath::fromRelativePath(SEP "a");

    target.prependWithSeparator(source);
    EXPECT_EQ(target.toPath(false), "b" SEP "a");

    // No separator if source ends with separator.
    source = LocalPath::fromRelativePath("b" SEP);
    target = LocalPath::fromRelativePath("a");

    target.prependWithSeparator(source);
    EXPECT_EQ(target.toPath(false), "b" SEP "a");
}

#undef SEP

TEST(JSONWriter, arg_stringWithEscapes)
{
    JSONWriter writer;
    writer.arg_stringWithEscapes("ke", "\"\\");
    EXPECT_EQ(writer.getstring(), "\"ke\":\"\\\"\\\\\"");
}

TEST(JSONWriter, escape)
{
    class Writer
      : public JSONWriter
    {
    public:
        using JSONWriter::escape;
    };

    Writer writer;
    string input = "\"\\";
    string expected = "\\\"\\\\";

    EXPECT_EQ(writer.escape(input.c_str(), input.size()), expected);
}

TEST(JSON, stripWhitespace)
{
    auto input = string(" a\rb\n c\r{\"a\":\"q\\r \\\" s\"\n} x y\n z\n");
    auto expected = string("abc{\"a\":\"q\\r \\\" s\"}xyz");
    auto computed = JSON::stripWhitespace(input);

    ASSERT_EQ(computed, expected);

    input = "{\"a\":\"bcde";
    expected = "{\"a\":\"";
    computed = JSON::stripWhitespace(input);

    ASSERT_EQ(computed, expected);
}

TEST(Utils, replace_char)
{
    ASSERT_EQ(Utils::replace(string(""), '*', '@'), "");
    ASSERT_EQ(Utils::replace(string("*"), '*', '@'), "@");
    ASSERT_EQ(Utils::replace(string("**"), '*', '@'), "@@");
    ASSERT_EQ(Utils::replace(string("*aa"), '*', '@'), "@aa");
    ASSERT_EQ(Utils::replace(string("*aa*bb*"), '*', '@'), "@aa@bb@");
    ASSERT_EQ(Utils::replace(string("sd*"), '*', '@'), "sd@");
    ASSERT_EQ(Utils::replace(string("*aa**bb*"), '*', '@'), "@aa@@bb@");
}

TEST(Utils, replace_string)
{
    ASSERT_EQ(Utils::replace(string(""), "*", "@"), "");
    ASSERT_EQ(Utils::replace(string("*"), "*", "@"), "@");
    ASSERT_EQ(Utils::replace(string("**"), "*", "@"), "@@");
    ASSERT_EQ(Utils::replace(string("*aa"), "*", "@"), "@aa");
    ASSERT_EQ(Utils::replace(string("*aa*bb*"), "*", "@"), "@aa@bb@");
    ASSERT_EQ(Utils::replace(string("sd*"), "*", "@"), "sd@");
    ASSERT_EQ(Utils::replace(string("*aa**bb*"), "*", "@"), "@aa@@bb@");
    ASSERT_EQ(Utils::replace(string("*aa**bb*"), "*", "@"), "@aa@@bb@");

    ASSERT_EQ(Utils::replace(string(""), "", "@"), "");
    ASSERT_EQ(Utils::replace(string("abc"), "", "@"), "abc");
}

TEST(Utils, natural_sorting)
{
    // Comparison between symbols
    ASSERT_EQ(naturalsorting_compare("!", "!"), 0);
    ASSERT_GT(naturalsorting_compare("@", "!"), 0);
    ASSERT_LT(naturalsorting_compare("#", "$"), 0);

    // Comparison between symbols and numbers
    ASSERT_LT(naturalsorting_compare("#", "0"), 0);
    ASSERT_LT(naturalsorting_compare("!", "9"), 0);
    ASSERT_GT(naturalsorting_compare("9", "#"), 0);

    // Comparison between symbols and letters
    ASSERT_LT(naturalsorting_compare("&", "a"), 0);
    ASSERT_LT(naturalsorting_compare("!", "Z"), 0);
    ASSERT_GT(naturalsorting_compare("a", "#"), 0);

    // Comparison between numbers and letters
    ASSERT_LT(naturalsorting_compare("9", "a"), 0);
    ASSERT_GT(naturalsorting_compare("a", "1"), 0);
    ASSERT_LT(naturalsorting_compare("1", "A"), 0);

    // Comparison between symbols and letters (case no sensitive)
    ASSERT_EQ(naturalsorting_compare("A", "a"), 0);
    ASSERT_GT(naturalsorting_compare("B", "a"), 0);
    ASSERT_LT(naturalsorting_compare("a", "C"), 0);

    // Comparison between strings containing letters and numbers
    ASSERT_GT(naturalsorting_compare("a1", "a0"), 0);
    ASSERT_LT(naturalsorting_compare("a1", "a2"), 0);

    // Comparison between strings containing letters and symbols
    ASSERT_LT(naturalsorting_compare("a!", "a#"), 0);
    ASSERT_LT(naturalsorting_compare("a#", "a@"), 0);

    // Comparison between strings containing letters, numbers and symbols
    ASSERT_LT(naturalsorting_compare("1a!", "1a#"), 0);
    ASSERT_LT(naturalsorting_compare("!a1", "a1#"), 0);
    ASSERT_LT(naturalsorting_compare("!a1", "1#a"), 0);
    ASSERT_GT(naturalsorting_compare("a1!", "1a#"), 0);
    ASSERT_GT(naturalsorting_compare("a!1", "1a#"), 0);
    ASSERT_GT(naturalsorting_compare("2a!", "1a#"), 0);
    ASSERT_EQ(naturalsorting_compare("1a&", "1a&"), 0);

    // Comparison between strings with different lengths
    ASSERT_GT(naturalsorting_compare("abc", "ab"), 0);
    ASSERT_LT(naturalsorting_compare("ab", "abc"), 0);

    // Comparison between strings containing white spaces
    ASSERT_LT(naturalsorting_compare("a ", "a!"), 0);
    ASSERT_GT(naturalsorting_compare("a#", "a "), 0);

    // Comparison between numbers of different lengths
    ASSERT_GT(naturalsorting_compare("10", "2"), 0);
    ASSERT_GT(naturalsorting_compare("100", "20"), 0);

    // Comparison between numbers containing zeros at the beginning
    ASSERT_LT(naturalsorting_compare("0", "00"), 0);
    ASSERT_LT(naturalsorting_compare("00", "000"), 0);
    ASSERT_LT(naturalsorting_compare("00123", "123"), 0);
    ASSERT_LT(naturalsorting_compare("00123", "124"), 0);
    ASSERT_GT(naturalsorting_compare("0124", "00123"), 0);
}

TEST(RemotePath, nextPathComponent)
{
    // Absolute path.
    {
        RemotePath path("/a/b/");

        RemotePath component;
        size_t index = 0;

        ASSERT_TRUE(path.nextPathComponent(index, component));
        ASSERT_EQ(component, "a");

        ASSERT_TRUE(path.nextPathComponent(index, component));
        ASSERT_EQ(component, "b");

        ASSERT_FALSE(path.nextPathComponent(index, component));
        ASSERT_TRUE(component.empty());

        // Sanity.
        path = RemotePath("/");

        index = 0;

        ASSERT_FALSE(path.nextPathComponent(index, component));
        ASSERT_TRUE(component.empty());
    }

    // Relative path.
    {
        RemotePath path("a/b/");

        RemotePath component;
        size_t index = 0;

        ASSERT_TRUE(path.nextPathComponent(index, component));
        ASSERT_EQ(component, "a");

        ASSERT_TRUE(path.nextPathComponent(index, component));
        ASSERT_EQ(component, "b");

        ASSERT_FALSE(path.nextPathComponent(index, component));
        ASSERT_TRUE(component.empty());

        // Sanity.
        path = RemotePath("");

        index = 0;

        ASSERT_FALSE(path.nextPathComponent(index, component));
        ASSERT_TRUE(component.empty());
    }
}

class TooLongNameTest
    : public ::testing::Test
{
public:
    TooLongNameTest()
      : Test()
      , mPrefixName(LocalPath::fromRelativePath("d"))
      , mPrefixPath()
    {
    }

    LocalPath Append(const LocalPath& prefix, const string& name) const
    {
        LocalPath path = prefix;

        path.appendWithSeparator(
          LocalPath::fromRelativeName(name, mFsAccess, FS_UNKNOWN),
          false);

        return path;
    }

    LocalPath AppendLongName(const LocalPath& prefix, char character) const
    {
        // Representative limit.
        //
        // True limit depends on specific filesystem.
        constexpr size_t MAX_COMPONENT_LENGTH = 255;

        string name(MAX_COMPONENT_LENGTH + 1, character);

        return Append(prefix, name);
    }

    bool CreateDummyFile(const LocalPath& path)
    {
        ::mega::byte data = 0x21;

        auto fileAccess = mFsAccess.newfileaccess(false);

        return fileAccess->fopen(path, false, true, FSLogging::logOnError)
               && fileAccess->fwrite(&data, 1, 0);
    }

    void SetUp() override
    {
        // Flag should initially be clear.
        ASSERT_FALSE(mFsAccess.target_name_too_long);

        // Retrieve the current working directory.
        ASSERT_TRUE(mFsAccess.cwd(mPrefixPath));

        // Compute absolute path to "container" directory.
        mPrefixPath.appendWithSeparator(mPrefixName, false);

        // Remove container directory.
        mFsAccess.emptydirlocal(mPrefixPath);
        mFsAccess.rmdirlocal(mPrefixPath);

        // Create container directory.
        ASSERT_TRUE(mFsAccess.mkdirlocal(mPrefixPath, false, true));
    }

    void TearDown() override
    {
        // Destroy container directory.
        mFsAccess.emptydirlocal(mPrefixPath);
        mFsAccess.rmdirlocal(mPrefixPath);
    }

    FSACCESS_CLASS mFsAccess;
    LocalPath mPrefixName;
    LocalPath mPrefixPath;
}; // TooLongNameTest

TEST_F(TooLongNameTest, Copy)
{
    // Absolute
    {
        auto source = Append(mPrefixPath, "s");
        auto target = AppendLongName(mPrefixPath, 'u');

        ASSERT_TRUE(CreateDummyFile(source));

        ASSERT_FALSE(mFsAccess.copylocal(source, target, 0));
        ASSERT_TRUE(mFsAccess.target_name_too_long);

        // Legitimate "bad path" error should clear the flag.
        target = Append(mPrefixPath, "u");
        target = Append(target, "v");

        ASSERT_FALSE(mFsAccess.copylocal(source, target, 0));
        ASSERT_FALSE(mFsAccess.target_name_too_long);
    }
}

TEST_F(TooLongNameTest, CreateDirectory)
{
    // Absolute
    {
        auto path = AppendLongName(mPrefixPath, 'x');

        ASSERT_FALSE(mFsAccess.mkdirlocal(path, false, true));
        ASSERT_TRUE(mFsAccess.target_name_too_long);

        // A legitimate "bad path" error should clear the flag.
        path = Append(mPrefixPath, "x");
        path = Append(path, "y");

        ASSERT_FALSE(mFsAccess.mkdirlocal(path, false, true));
        ASSERT_FALSE(mFsAccess.target_name_too_long);
    }
}

TEST_F(TooLongNameTest, Rename)
{
    // Absolute
    {
        auto source = Append(mPrefixPath, "q");
        auto target = AppendLongName(mPrefixPath, 'r');

        ASSERT_TRUE(mFsAccess.mkdirlocal(source, false, true));

        ASSERT_FALSE(mFsAccess.renamelocal(source, target, false));
        ASSERT_TRUE(mFsAccess.target_name_too_long);

        // Legitimate "bad path" error should clear the flag.
        target = Append(mPrefixPath, "u");
        target = Append(target, "v");

        ASSERT_FALSE(mFsAccess.renamelocal(source, target, false));
        ASSERT_FALSE(mFsAccess.target_name_too_long);
    }
}

class ProcessTest
    : public ::testing::Test
{
public:
    ProcessTest()
        : Test()
    {
    }
};

#ifdef WIN32
string dirCommand = "dir";
string shellCommand = "cmd";
#else
string dirCommand = "ls";
string shellCommand = "sh";
#endif

TEST_F(ProcessTest, Poll)
{
    Process p;
    string out;
    string error;
    bool ok = p.run(vector<string>{dirCommand}, unordered_map<string, string>(), [&](const unsigned char* data, size_t len) {out.append((const char*)(data), len); }, [&](const unsigned char* data, size_t len) {error.append((const char*)(data), len); });
    ASSERT_TRUE(ok) << "run failed" << endl;
    while (p.isAlive()) {
        if (!p.poll())
            usleep(100000);
    }
    p.flush();
    ASSERT_FALSE(out.empty()) << "no output received";
    ASSERT_TRUE(error.empty()) << "error received";
}

TEST_F(ProcessTest, Wait)
{
    Process p;
    string out;
    string error;
    bool ok = p.run(vector<string>{dirCommand}, unordered_map<string, string>(), [&](const unsigned char* data, size_t len) {out.append((const char*)(data), len); }, [&](const unsigned char* data, size_t len) {error.append((const char*)(data), len); });
    ASSERT_TRUE(ok) << "run failed" << endl;
    p.wait();
    ASSERT_FALSE(out.empty()) << "no output received";
    ASSERT_TRUE(error.empty()) << "error received";
}

TEST_F(ProcessTest, RunError)
{
    Process p;
    string out;
    string error;
    bool ok = p.run(vector<string>{"this-command-does-not-exist", "tmp"}, unordered_map<string, string>(), [&](const unsigned char* data, size_t len) {out.append((const char*)(data), len); }, [&](const unsigned char* data, size_t len) {error.append((const char*)(data), len); });
    // ok posix
    // fails windows
    ok = p.wait();
    ASSERT_FALSE(ok) << "run ok!" << endl;
}

TEST_F(ProcessTest, WaitNonRedirect)
{
    Process p;
    bool ok = p.run(vector<string>{dirCommand});
    ASSERT_TRUE(ok) << "run failed" << endl;
    ok = p.wait();
    ASSERT_TRUE(ok) << "program failed" << endl;
}

TEST_F(ProcessTest, ErrorNonRedirect)
{
    Process p;
    bool ok = p.run(vector<string>{dirCommand, "/file-does-not-exist"});
    ASSERT_TRUE(ok) << "run failed" << endl;
    ok = p.wait();
    ASSERT_FALSE(ok) << "program ok" << endl;
}

class SprintfTest
    : public ::testing::Test
{
};

TEST_F(SprintfTest, nulTerminateWhenBufferFull)
{
    std::string countToSix("123456");
    // g++ detects if we don't use a variable

    std::string buf(countToSix.size(), 'x');

    // with macro commented out
    snprintf(buf.data(), 3, "%s", countToSix.data());
    ASSERT_EQ(buf[0], '1');
    ASSERT_EQ(buf[1], '2');
    ASSERT_EQ(buf[2], '\0');
}

TEST_F(SprintfTest, Multiple)
{
    std::string buffer(7, '\x0');

    std::string aToH("ABCDEFGH");
    std::string countToFour("1234");

    snprintf(buffer.data(), buffer.size(), "%s", countToFour.data());

    snprintf(&buffer[countToFour.size()], buffer.size() - countToFour.size(), "%s", aToH.data());

    ASSERT_EQ(buffer[0], '1');
    ASSERT_EQ(buffer[1], '2');
    ASSERT_EQ(buffer[2], '3');
    ASSERT_EQ(buffer[3], '4');
    ASSERT_EQ(buffer[4], 'A');
    ASSERT_EQ(buffer[5], 'B');
    ASSERT_EQ(buffer[6], '\0');
}

TEST_F(SprintfTest, ResizeAndPrint) {

    unsigned int price = 120;
    string sprice;
    sprice.resize(128);
    snprintf(const_cast<char*>(sprice.data()), sprice.length(), "%.2f", price / 100.0);
    replace(sprice.begin(), sprice.end(), ',', '.');
    // sprince = "1.20\0\0\0\..."
    ASSERT_EQ((string)sprice.c_str(), "1.20");
}

TEST(extensionOf, fails_when_extension_contains_invalid_characters)
{
    using ::mega::extensionOf;

    std::string computed;

    // Characters below '.'
    ASSERT_FALSE(extensionOf(std::string("a.-"), computed));
    ASSERT_TRUE(computed.empty());

    // Characters above 'z'.
    ASSERT_FALSE(extensionOf(std::string("a.{"), computed));
    ASSERT_TRUE(computed.empty());
}

TEST(extensionOf, fails_when_extension_isnt_present)
{
    using ::mega::extensionOf;

    std::string computed;

    // No extension.
    ASSERT_FALSE(extensionOf(std::string("a"), computed));
    ASSERT_TRUE(computed.empty());

    // Empty string.
    ASSERT_FALSE(extensionOf(std::string(), computed));
    ASSERT_TRUE(computed.empty());
}

TEST(extensionOf, succeeds)
{
    using ::mega::extensionOf;

    std::string computed;

    // Multicharacter extension.
    ASSERT_TRUE(extensionOf(std::string("a.BcD"), computed));
    ASSERT_EQ(computed, ".bcd");

    // Single character extension.
    ASSERT_TRUE(extensionOf(std::wstring(L".a"), computed));
    ASSERT_EQ(computed, ".a");

    // Empty extension.
    ASSERT_TRUE(extensionOf(std::string("."), computed));
    ASSERT_EQ(computed, ".");
}

TEST(fromHex, fails_when_empty_string)
{
    EXPECT_FALSE(fromHex<short>(nullptr, nullptr).second);
    EXPECT_FALSE(fromHex<short>("").second);
}

TEST(fromHex, fails_when_invalid_character)
{
    EXPECT_FALSE(fromHex<short>('q').second);
    EXPECT_FALSE(fromHex<short>('_').second);
}

TEST(fromHex, fails_when_out_of_range)
{
    EXPECT_FALSE(fromHex<signed char>("80").second);
    EXPECT_FALSE(fromHex<short>("8000").second);
    EXPECT_FALSE(fromHex<unsigned char>("100").second);
    EXPECT_FALSE(fromHex<unsigned short>("10000").second);
}

TEST(fromHex, succeeds)
{
    auto s8 = fromHex<signed char>("7f");
    EXPECT_TRUE(s8.second);
    EXPECT_EQ(s8.first, 0x7f);

    auto s16 = fromHex<short>("7fff");
    EXPECT_TRUE(s16.second);
    EXPECT_EQ(s16.first, 0x7fff);

    auto u8 = fromHex<unsigned char>("ff");
    EXPECT_TRUE(u8.second);
    EXPECT_EQ(u8.first, 0xff);

    auto u16 = fromHex<unsigned short>("ffff");
    EXPECT_TRUE(u16.second);
    EXPECT_EQ(u16.first, 0xffff);
}

TEST(Split, no_delimiter)
{
    auto input  = std::string();
    auto result = split(input, '.');

    // Empty string.
    EXPECT_EQ(result.first.first, input.data());
    EXPECT_FALSE(result.first.second);
    EXPECT_FALSE(result.second.first);
    EXPECT_FALSE(result.second.second);

    // No delimiter.
    input  = "abc";
    result = split(input, '.');

    EXPECT_EQ(result.first.first, input.data());
    EXPECT_EQ(result.first.second, input.size());
    EXPECT_FALSE(result.second.first);
    EXPECT_FALSE(result.second.second);
}

TEST(Split, with_delimiter)
{
    auto input  = std::string("a.");
    auto result = split(input, '.');

    // Delimiter only.
    EXPECT_EQ(result.first.first, input.data());
    EXPECT_EQ(result.first.second, 1u);
    EXPECT_EQ(result.second.first, &input[1]);
    EXPECT_EQ(result.second.second, 1u);

    // Delimiter and tail.
    input  = "abc.qrs";
    result = split(input, '.');

    EXPECT_EQ(result.first.first,  input.data());
    EXPECT_EQ(result.first.second, 3u);
    EXPECT_EQ(result.second.first, &input[3]);
    EXPECT_EQ(result.second.second, 4u);
}

TEST(EscapeWildCars, UseCases)
{
    EXPECT_EQ(escapeWildCards("hello"), "hello");
    EXPECT_EQ(escapeWildCards("hel*lo"), "hel\\*lo");
    EXPECT_EQ(escapeWildCards("*hello*"), "\\*hello\\*");
    EXPECT_EQ(escapeWildCards("\\*hello*"), "\\*hello\\*");
    EXPECT_EQ(escapeWildCards("\\*hello\\*"), "\\*hello\\*");
    EXPECT_EQ(escapeWildCards("hel\\\\*lo"), "hel\\\\\\*lo");
}

TEST(ScopedHelpers, ScopedDestructor)
{
    // So we can test various binding styles.
    struct Functor
    {
        void memberNoArguments() {}

        void memberWithArguments(std::string) {}

        static void rawNoArguments() {}

        static void rawWithArguments(std::string) {}
    }; // Functor

    // Make sure we can bind raw function pointers.
    {
        auto x = makeScopedDestructor(&Functor::rawNoArguments);

        // This also tests that convertible arguments are allowed.
        auto y = makeScopedDestructor(&Functor::rawWithArguments, "Test");
    }

    // Make sure we can bind member function pointers.
    {
        auto f = Functor();
        auto x = makeScopedDestructor(&Functor::memberNoArguments, &f);
        auto y = makeScopedDestructor(&Functor::memberWithArguments, &f, "Test");
    }

    // Make sure we can bind lambda functions.
    auto x = 0;

    // Lambda without parameters.
    {
        auto y = makeScopedDestructor(
            [&x]()
            {
                ++x;
            });
    }

    // Make sure the destructor was executed.
    EXPECT_EQ(x, 1);

    // Lambda with parameters.
    {
        auto y = makeScopedDestructor(
            [&x](int v)
            {
                x += v;
            },
            3);

        // Make sure convertible arguments are accepted.
        auto z = makeScopedDestructor([](std::string) {}, "Test");
    }

    // Make sure destructor was executed.
    EXPECT_EQ(x, 4);
}

TEST(ScopedHelpers, ScopedValue)
{
    const std::string originalValue = "before";
    std::string value = originalValue;

    {
        const std::string expectedValue = "After";

        // Also tests that conertible arguments are accepted.
        auto guard = makeScopedValue(value, expectedValue.c_str());

        // Make sure value's value was changed.
        ASSERT_EQ(value, expectedValue);
    }

    // Make sure value's value was restored.
    ASSERT_EQ(value, originalValue);
}

TEST(ScopedHelpers, MakePtrFrom)
{
    struct Dummy
    {
        static void destructor(Dummy* dummy)
        {
            delete dummy;
        }
    }; // Dummy

    // Shared pointer, default deleter.
    {
        auto x = makeSharedFrom(new Dummy);

        // Verify type signature.
        static_assert(std::is_same_v<decltype(x), std::shared_ptr<Dummy>>);
    }

    // Shared pointer, custom deleter.
    {
        auto x = makeSharedFrom(new Dummy, &Dummy::destructor);

        // Verify type signature.
        static_assert(std::is_same_v<decltype(x), std::shared_ptr<Dummy>>);

        // Verify deleter.
        auto d = std::get_deleter<void (*)(Dummy*)>(x);
        ASSERT_TRUE(d);
        EXPECT_EQ(*d, &Dummy::destructor);
    }

    // Unique pointer, default deleter.
    {
        auto x = makeUniqueFrom(new Dummy);

        // Verify type signature.
        using ComputedType = decltype(x);
        using ExpectedType = std::unique_ptr<Dummy, std::default_delete<Dummy>>;

        static_assert(std::is_same_v<ComputedType, ExpectedType>);
    }

    // Unique pointer, custom deleter.
    {
        auto x = makeUniqueFrom(new Dummy, &Dummy::destructor);

        using ComputedType = decltype(x);
        using ExpectedType = std::unique_ptr<Dummy, void (*)(Dummy*)>;

        static_assert(std::is_same_v<ComputedType, ExpectedType>);
    }
}

TEST(LikeCompare, ExactMatch)
{
    ASSERT_TRUE(likeCompare("hello", "hello"));
    ASSERT_TRUE(likeCompare("he1lo", "he1lo"));
    ASSERT_TRUE(likeCompare("hélloé", "hélloé"));
    ASSERT_TRUE(likeCompare("你好", "你好"));

    ASSERT_FALSE(likeCompare("hello1", "hello"));
    ASSERT_FALSE(likeCompare("helo", "he1lo"));
    ASSERT_FALSE(likeCompare("héllo", "hélloé"));
    ASSERT_FALSE(likeCompare("你好", "你好!"));
}

TEST(LikeCompare, MatchOne)
{
    ASSERT_TRUE(likeCompare("hell?", "hello"));
    ASSERT_TRUE(likeCompare("héll?é", "hélloé"));
    ASSERT_TRUE(likeCompare("你?", "你好"));

    ASSERT_FALSE(likeCompare("hello?", "hello"));
    ASSERT_FALSE(likeCompare("hel?o", "he1lo"));
    ASSERT_FALSE(likeCompare("héll?", "hélloé"));
    ASSERT_FALSE(likeCompare("你?", "你好!"));
}

TEST(LikeCompare, MatchAll)
{
    ASSERT_TRUE(likeCompare("h*o", "hello"));
    ASSERT_TRUE(likeCompare("*é", "hélloé"));
    ASSERT_TRUE(likeCompare("*", "你好"));

    ASSERT_FALSE(likeCompare("he1*lo", "hello"));
    ASSERT_FALSE(likeCompare("*你", "你好!"));
}

TEST(LikeCompare, CaseInsensitiveMatch)
{
    ASSERT_TRUE(likeCompare("HELLO", "hello"));
    ASSERT_TRUE(likeCompare("HÉllOé", "hélloé"));
}

TEST(LikeCompare, AccentInsensitiveMatch)
{
    ASSERT_TRUE(likeCompare("HÉllOé", "HElloe"));
    ASSERT_TRUE(likeCompare("façade", "facade"));
    ASSERT_TRUE(likeCompare("nghiÃªn", "nghiAªn"));
}

// \\* is \* in c++ string. It is the escaping of the character * in the pattern, which makes it
// match only the single character *.
TEST(LikeCompare, EscapeMatch)
{
    ASSERT_TRUE(likeCompare("H\\*Elloe", "H*Elloe"));
    ASSERT_TRUE(likeCompare("\\*你*", "*你好!"));

    ASSERT_FALSE(likeCompare("H\\*", "H*Elloe"));
    ASSERT_FALSE(likeCompare("\\*你", "**你"));
}

TEST(LikeCompare, CombinedMatch)
{
    ASSERT_TRUE(likeCompare("HÉ?l*e", "heLloé"));
    ASSERT_TRUE(likeCompare("你ç?*", "你c好!"));

    ASSERT_FALSE(likeCompare("HÉ?l*e\\*", "heLloé"));
}

TEST(NaturalSorting, Numbers)
{
    static const std::vector<std::string> input =
        {"123", "0123", "00123", "234", "0234", "00234", "00", "0", "000"}; // input

    static const std::vector<std::string> expected =
        {"0", "00", "000", "00123", "0123", "123", "00234", "0234", "234"}; // expected

    std::vector<std::string> computed = input;

    std::sort(computed.begin(), computed.end(), NaturalSortingComparator());

    EXPECT_EQ(computed, expected);
}

class CreateIdFromName: public testing::TestWithParam<uint64_t>
{
public:
    static constexpr uint64_t compileTimeSeed()
    {
        uint64_t s = 0;
        for (const auto c: __TIME__)
        {
            s <<= 8;
            s |= static_cast<uint64_t>(c);
        }
        return s;
    }
};

TEST_P(CreateIdFromName, ValidateNewImplementation)
{
    static constexpr uint64_t seed = CreateIdFromName::compileTimeSeed();
    static constexpr string_view validChars{"!#$%&*+0123456789?^_abcdefghijklmnopqrstuvwxyz~"};
    static constexpr char n[8]{validChars[seed % validChars.size()],
                               validChars[seed * 2 % validChars.size()],
                               validChars[seed * 3 % validChars.size()],
                               validChars[seed * 4 % validChars.size()],
                               validChars[seed * 5 % validChars.size()],
                               validChars[seed * 6 % validChars.size()],
                               validChars[seed * 7 % validChars.size()],
                               validChars[seed * 8 % validChars.size()]};

    const uint64_t nameSize = GetParam();
    switch (nameSize)
    {
        case 1:
        {
            static constexpr char name[]{n[0], 0};
            static_assert(makeNameid(name) == MAKENAMEID1(n[0]));
            ASSERT_EQ(makeNameid(string{name}), MAKENAMEID1(n[0]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr), MAKENAMEID1(n[0]))
                << "Failed for \"" << name << '"';
            break;
        }
        case 2:
        {
            static constexpr char name[]{n[0], n[1], 0};
            static_assert(makeNameid(name) == MAKENAMEID2(n[0], n[1]));
            ASSERT_EQ(makeNameid(string{name}), MAKENAMEID2(n[0], n[1]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr), MAKENAMEID2(n[0], n[1]))
                << "Failed for \"" << name << '"';
            break;
        }
        case 3:
        {
            static constexpr char name[]{n[0], n[1], n[2], 0};
            static_assert(makeNameid(name) == MAKENAMEID3(n[0], n[1], n[2]));
            ASSERT_EQ(makeNameid(string{name}), MAKENAMEID3(n[0], n[1], n[2]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr), MAKENAMEID3(n[0], n[1], n[2]))
                << "Failed for \"" << name << '"';
            break;
        }
        case 4:
        {
            static constexpr char name[]{n[0], n[1], n[2], n[3], 0};
            static_assert(makeNameid(name) == MAKENAMEID4(n[0], n[1], n[2], n[3]));
            ASSERT_EQ(makeNameid(string{name}), MAKENAMEID4(n[0], n[1], n[2], n[3]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr), MAKENAMEID4(n[0], n[1], n[2], n[3]))
                << "Failed for \"" << name << '"';
            break;
        }
        case 5:
        {
            static constexpr char name[]{n[0], n[1], n[2], n[3], n[4], 0};
            static_assert(makeNameid(name) == MAKENAMEID5(n[0], n[1], n[2], n[3], n[4]));
            ASSERT_EQ(makeNameid(string{name}), MAKENAMEID5(n[0], n[1], n[2], n[3], n[4]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr), MAKENAMEID5(n[0], n[1], n[2], n[3], n[4]))
                << "Failed for \"" << name << '"';
            break;
        }
        case 6:
        {
            static constexpr char name[]{n[0], n[1], n[2], n[3], n[4], n[5], 0};
            static_assert(makeNameid(name) == MAKENAMEID6(n[0], n[1], n[2], n[3], n[4], n[5]));
            ASSERT_EQ(makeNameid(string{name}), MAKENAMEID6(n[0], n[1], n[2], n[3], n[4], n[5]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr), MAKENAMEID6(n[0], n[1], n[2], n[3], n[4], n[5]))
                << "Failed for \"" << name << '"';
            break;
        }
        case 7:
        {
            static constexpr char name[]{n[0], n[1], n[2], n[3], n[4], n[5], n[6], 0};
            static_assert(makeNameid(name) ==
                          MAKENAMEID7(n[0], n[1], n[2], n[3], n[4], n[5], n[6]));
            ASSERT_EQ(makeNameid(string{name}),
                      MAKENAMEID7(n[0], n[1], n[2], n[3], n[4], n[5], n[6]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr),
                      MAKENAMEID7(n[0], n[1], n[2], n[3], n[4], n[5], n[6]))
                << "Failed for \"" << name << '"';
            break;
        }
        case 8:
        {
            static constexpr char name[]{n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], 0};
            static_assert(makeNameid(name) ==
                          MAKENAMEID8(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]));
            ASSERT_EQ(makeNameid(string{name}),
                      MAKENAMEID8(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]))
                << "Failed for \"" << name << '"';
            static const char* constCharPtr = name;
            ASSERT_EQ(makeNameid(constCharPtr),
                      MAKENAMEID8(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]))
                << "Failed for \"" << name << '"';
            break;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(NameidTests, CreateIdFromName, testing::Values(1, 2, 3, 4, 5, 6, 7, 8));

// Test class Range
TEST(RangeTest, ValidRange)
{
    // Range from 2 to 5 -> expect iteration over 2, 3, 4
    Range r(2, 5);
    std::vector<unsigned> values(std::begin(r), std::end(r));
    EXPECT_THAT(values, testing::ElementsAre(2, 3, 4));
}

TEST(RangeTest, EmptyRangeWhenStartEqualsToEnd)
{
    // Range from 5 to 5 -> empty range
    Range r(5, 5);

    EXPECT_TRUE(r.empty());
}

TEST(RangeTest, EmptyRangeWhenStartGreaterThanEnd)
{
    // Range from 6 to 5 -> empty range
    Range r(6, 5);
    EXPECT_TRUE(r.empty());

    unsigned count = 0;
    for ([[maybe_unused]] const auto val: r)
    {
        ++count;
    }

    EXPECT_EQ(count, 0);
}

TEST(RangeTest, OverloadRangeToZeroStart)
{
    // range(5) -> Range(0, 5)
    auto r = range(5);

    std::vector<unsigned> values;
    for (const auto val: r)
    {
        values.push_back(val);
    }

    EXPECT_THAT(values, testing::ElementsAre(0, 1, 2, 3, 4));
}

TEST(RangeTest, VerifySingleElementRange)
{
    // Range(7, 8) should iterate exactly once
    auto r = range(7, 8);

    unsigned count = 0;
    unsigned valueCollected = 0;
    for (const auto val: r)
    {
        ++count;
        valueCollected = val;
    }

    EXPECT_EQ(count, 1);
    EXPECT_EQ(valueCollected, 7u);
}

struct FileAccessTest: ::testing::Test
{
    FileAccessTest():
        Test(),
        mFSAccess(),
        mName(LocalPath::fromAbsolutePath("file"))
    {}

    // Called before any test in the fixture is executed.
    void SetUp() override
    {
        // Remove the file if it's present after a previous test run.
        ASSERT_TRUE(mFSAccess.unlinklocal(mName) || !mFSAccess.target_exists);
    }

    // Convenience.
    ::mega::FSLogging NO_LOGGING = ::mega::FSLogging::noLogging;

    // So we can get our hands on a FileAccess instance.
    FSACCESS_CLASS mFSAccess;

    // The name of our test file.
    ::mega::LocalPath mName;
}; // FileAccessTest

TEST_F(FileAccessTest, OpenForReadWriteSucceeds)
{
    // So we can open a file.
    auto fileAccess = mFSAccess.newfileaccess(false);

    // Sanity.
    ASSERT_TRUE(fileAccess);

    // Opening for reading and writing should create a new file if necessary.
    EXPECT_TRUE(fileAccess->fopen(mName, true, true, NO_LOGGING));
}

TEST_F(FileAccessTest, OpenEquivalence)
{
    auto fileAccess0 = mFSAccess.newfileaccess(false);
    auto fileAccess1 = mFSAccess.newfileaccess(false);

    // Sanity.
    ASSERT_TRUE(fileAccess0);
    ASSERT_TRUE(fileAccess1);

    // Create a new file.
    ASSERT_TRUE(fileAccess0->fopen(mName, true, true, NO_LOGGING));

    // Open an existing file.
    EXPECT_TRUE(fileAccess1->fopen(mName, true, true, NO_LOGGING));

    // Convenience.
    auto& lhs = *fileAccess0;
    auto& rhs = *fileAccess1;

    // Make sure selected state is equivalent.
    EXPECT_EQ(lhs.fopenSucceeded, rhs.fopenSucceeded);
    EXPECT_EQ(lhs.size, rhs.size);
    EXPECT_EQ(lhs.mtime, rhs.mtime);
    EXPECT_EQ(lhs.fsid, rhs.fsid);
    EXPECT_EQ(lhs.type, rhs.type);
    EXPECT_EQ(lhs.mIsSymLink, rhs.mIsSymLink);
}
