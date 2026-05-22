/**
 * @file Sqlite_test.cpp
 * @brief Unit tests for Sqlite-backed node storage, including
 *        listAllNodesByPage() cursor-based (keyset) pagination.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mega/db/sqlite.h>
#include <mega/localpath.h>

#include <filesystem>
#include <mega.h>
#include <set>
#include <sqlite3.h>
#include <stdfs.h>
#include <string>
#include <vector>

using namespace mega;

/**
 * @brief Validate renameDBFiles method
 *
 * Steps:
 *  - Create new data base
 *  - Call to renameDBFiles
 *  - Check if all files has been renamed
 */
#ifdef WIN32
// As DB is opened when files are renamed, windows doesn't allow rename DB files
TEST(Sqlite, DISABLED_renameDB)
{
#else
TEST(Sqlite, renameDB)
{
#endif
    auto pathString{std::filesystem::current_path() / "folder"};

    const MrProper cleanUp(
        [pathString]()
        {
            std::filesystem::remove_all(pathString);
        });

    std::filesystem::create_directory(pathString);
    LocalPath folderPath = LocalPath::fromAbsolutePath(path_u8string(pathString));
    SqliteDbAccess dbAccess{folderPath};

    // Create and open DB
    std::unique_ptr<FileSystemAccess> fsaccess{new FSACCESS_CLASS};
    const std::string dbName{"dbName"};
    LocalPath currentDataBasePath{dbAccess.databasePath(*fsaccess, dbName, DbAccess::DB_VERSION)};
    PrnGen rng;
    constexpr int flags = 0;
    std::unique_ptr<SqliteDbTable> db{dbAccess.open(rng, *fsaccess, dbName, flags, nullptr)};
    if (!db)
    {
        ASSERT_TRUE(false) << "Failure opening DB";
    }

    // Insert elements
    for (int i = 1; i < 10; ++i)
    {
        std::string content = "content " + std::to_string(i);
        db->put(static_cast<uint32_t>((i += DbTable::IDSPACING) | MegaClient::CACHEDUSER),
                static_cast<char*>(content.data()),
                static_cast<unsigned>(content.length()));
    }

    // check if auxiliar files exist
    LocalPath shmPath = currentDataBasePath;
    shmPath.append(LocalPath::fromRelativePath("-shm"));
    bool shmExists = std::filesystem::exists(shmPath.toPath(false));
    EXPECT_TRUE(shmExists) << "Unexpected behavior, -shm file doesn't exist";

    LocalPath walPath = currentDataBasePath;
    walPath.append(LocalPath::fromRelativePath("-wal"));
    bool walExists = std::filesystem::exists(walPath.toPath(false));
    EXPECT_TRUE(walExists) << "Unexpected behavior, -wal file doesn't exist";

    // Determine new path
    const std::string dbNewName{"dbNewName"};
    LocalPath newDataBasePath{dbAccess.databasePath(*fsaccess, dbNewName, DbAccess::DB_VERSION)};

    // Rename DB
    EXPECT_TRUE(dbAccess.renameDBFiles(*fsaccess, currentDataBasePath, newDataBasePath))
        << "Failure to rename files (maybe they are in use)";

    // Verify if auxiliar files exist
    if (shmExists)
    {
        shmPath = newDataBasePath;
        shmPath.append(LocalPath::fromRelativePath("-shm"));
        std::string aux = shmPath.toPath(false);
        EXPECT_TRUE(std::filesystem::exists(aux))
            << "File " << aux << "doesn't exit when it should";
    }

    if (walExists)
    {
        walPath = newDataBasePath;
        walPath.append(LocalPath::fromRelativePath("-wal"));
        std::string aux = walPath.toPath(false);
        EXPECT_TRUE(std::filesystem::exists(aux))
            << "File " << aux << "doesn't exit when it should";
    }
}

#ifdef USE_SQLITE

/**
 * @brief Validate that opening a DB shaped like a previous schema version
 *        runs the column-migration path to completion.
 *
 * Regression guard for DB-migration bugs where a new VIRTUAL column
 * references a base column that does not exist in older on-disk schemas,
 * e.g. "ADD COLUMN fingerprintVirtual32 BLOB AS (getFingerprintExcludingMtime(fp)) VIRTUAL"
 * failing with "no such column: fp" on upgraded databases.
 *
 * Steps:
 *  - Seed an on-disk DB with an older `nodes` schema (base columns only).
 *  - Open via SqliteDbAccess::openTableWithNodes, which runs addAndPopulateColumns.
 *  - Assert the call succeeds and every expected column is present afterwards.
 */
TEST(Sqlite, MigratesOldNodesSchema)
{
    auto dirPath = std::filesystem::current_path() / "nodes_schema_migration_test";

    const MrProper cleanUp(
        [dirPath]()
        {
            std::filesystem::remove_all(dirPath);
        });

    std::filesystem::remove_all(dirPath);
    std::filesystem::create_directory(dirPath);
    LocalPath folderPath = LocalPath::fromAbsolutePath(path_u8string(dirPath));
    SqliteDbAccess dbAccess{folderPath};

    std::unique_ptr<FileSystemAccess> fsaccess{new FSACCESS_CLASS};
    const std::string dbName{"nodes_schema_migration"};
    LocalPath dbLocalPath = dbAccess.databasePath(*fsaccess, dbName, DbAccess::DB_VERSION);
    const std::string dbPathStr = dbLocalPath.toPath(false);

    // sqlite3_open may allocate the handle even on error, and the handle
    // must be released via sqlite3_close regardless. Wrap in unique_ptr so
    // close runs on every exit path (including ASSERT_* aborts).
    using SqliteHandle = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

    auto openSqlite = [](const std::string& path) -> std::pair<SqliteHandle, int>
    {
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open(path.c_str(), &raw);
        return {SqliteHandle{raw, &sqlite3_close}, rc};
    };

    {
        auto [dbGuard, openRc] = openSqlite(dbPathStr);
        ASSERT_EQ(SQLITE_OK, openRc);

        // NOTE: Keep this schema frozen — do not add or remove columns here.
        // It represents a pre-migration `nodes` table on disk, so the whole
        // point of the test is to upgrade *this exact shape* to the current
        // schema. Expanding oldSchema silently weakens the regression guard.
        //
        // oldSchema mirrors the CREATE TABLE `nodes` DDL in src/db/sqlite.cpp
        // with all migration-added columns (the entries in the `newCols` vector
        // inside SqliteDbAccess::openTableWithNodes) removed.
        const char* oldSchema = "CREATE TABLE nodes ("
                                " nodehandle int64 PRIMARY KEY NOT NULL,"
                                " parenthandle int64,"
                                " name text,"
                                " fingerprint BLOB,"
                                " origFingerprint BLOB,"
                                " type tinyint,"
                                " share tinyint,"
                                " fav tinyint,"
                                " ctime int64,"
                                " flags int64,"
                                " counter BLOB NOT NULL,"
                                " node BLOB NOT NULL)";
        char* err = nullptr;
        const int rc = sqlite3_exec(dbGuard.get(), oldSchema, nullptr, nullptr, &err);
        const std::string errStr = err ? err : "";
        sqlite3_free(err);
        ASSERT_EQ(SQLITE_OK, rc) << "Failed to seed old schema: " << errStr;
    }

    // Run the migration through the SDK's open path.
    PrnGen rng;
    std::unique_ptr<DbTable> dbTable{
        dbAccess.openTableWithNodes(rng, *fsaccess, dbName, 0, nullptr)};
    // If this fails, openTableWithNodes could not migrate the old schema
    // to the current one — such as a new VIRTUAL column in `newCols`
    // (SqliteDbAccess::openTableWithNodes in src/db/sqlite.cpp) referencing
    // a base column that older DBs don't have. Do NOT "fix" by adding the
    // base column to oldSchema above — oldSchema is a frozen historical
    // snapshot, the guard only works if it stays one.
    ASSERT_TRUE(dbTable) << "Migration failed — openTableWithNodes() returned null";
    EXPECT_EQ(dbAccess.currentDbVersion, DbAccess::DB_VERSION);
    dbTable.reset(); // release the DB handle before re-opening for assertions

    // Build a reference DB from scratch and compare column sets. This catches
    // the case where someone adds a new entry to newCols in sqlite.cpp but
    // forgets to keep this test in sync — the two sets will diverge.
    auto refDirPath = std::filesystem::current_path() / "nodes_schema_migration_ref";
    const MrProper refCleanUp(
        [refDirPath]()
        {
            std::filesystem::remove_all(refDirPath);
        });
    std::filesystem::remove_all(refDirPath);
    std::filesystem::create_directory(refDirPath);
    LocalPath refFolder = LocalPath::fromAbsolutePath(path_u8string(refDirPath));
    SqliteDbAccess refDbAccess{refFolder};
    LocalPath refDbLocalPath = refDbAccess.databasePath(*fsaccess, dbName, DbAccess::DB_VERSION);

    std::unique_ptr<DbTable> refDbTable{
        refDbAccess.openTableWithNodes(rng, *fsaccess, dbName, 0, nullptr)};
    ASSERT_TRUE(refDbTable);
    refDbTable.reset();

    auto readColumnSet = [&openSqlite](const std::string& path)
    {
        std::set<std::string> cols;
        auto [dbGuard, openRc] = openSqlite(path);
        if (openRc != SQLITE_OK)
            return cols;
        sqlite3_stmt* stmt = nullptr;
        const char* q = "SELECT name FROM pragma_table_xinfo('nodes')";
        if (sqlite3_prepare_v2(dbGuard.get(), q, -1, &stmt, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                cols.emplace(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            }
        }
        sqlite3_finalize(stmt);
        return cols;
    };

    const auto migratedCols = readColumnSet(dbPathStr);
    const auto freshCols = readColumnSet(refDbLocalPath.toPath(false));

    // Guard against false-green: readColumnSet silently returns an empty
    // set on open / prepare failure or if the `nodes` table is absent.
    // Without this, two empty sets would trivially compare equal.
    ASSERT_FALSE(migratedCols.empty()) << "Failed to read columns from migrated DB: " << dbPathStr;
    ASSERT_FALSE(freshCols.empty())
        << "Failed to read columns from fresh DB: " << refDbLocalPath.toPath(false);

    // If this fails, the CREATE TABLE DDL and the `newCols` list in
    // SqliteDbAccess::openTableWithNodes (src/db/sqlite.cpp) are out of
    // sync — such as a new column added to one but not the other. Any
    // new column must appear in both so that fresh-create and migrate
    // paths end up with identical schemas.
    EXPECT_THAT(migratedCols, ::testing::UnorderedElementsAreArray(freshCols));
}

#endif // USE_SQLITE

// ─────────────────────────────────────────────────────────────────────────────
//  SQLite-backed node storage tests (listAllNodesByPage cursor-based pagination)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef USE_SQLITE

#include "utils.h"

#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/nodemanager.h>

namespace fs = std::filesystem;

namespace
{

// Convenience helper for building ListAllNodesParams in tests. Defaults to the
// "Cloud + Vault" scope (explicitAncestors empty, locationScope = 1); tests
// that exercise a specific subtree pass one or more explicit ancestor handles.
ListAllNodesParams makeParams(MimeType_t mime,
                              int order,
                              size_t maxElements,
                              bool excludeSensitive = false,
                              std::optional<NodeSearchCursorOffset> cursor = std::nullopt,
                              std::vector<NodeHandle> explicitAncestors = {},
                              std::vector<NodeHandle> excludeHandles = {},
                              int locationScope = 1)
{
    ListAllNodesParams p;
    p.mimeType = mime;
    p.order = order;
    p.maxElements = maxElements;
    p.excludeSensitive = excludeSensitive;
    p.cursor = std::move(cursor);
    p.explicitAncestors = std::move(explicitAncestors);
    p.excludeHandles = std::move(excludeHandles);
    p.locationScope = locationScope;
    return p;
}

// Collect the handles from a NodeManager-layer result (sharedNode_vector) into
// a std::set for set-membership assertions. Unordered semantics, dedupes
// duplicates if any. Used by Group G filter-semantics tests.
std::set<NodeHandle> handlesOf(const sharedNode_vector& nodes)
{
    std::set<NodeHandle> result;
    for (const auto& n: nodes)
        result.insert(n->nodeHandle());
    return result;
}

// DbTable-layer overload: rows are (handle, serialised) pairs.
std::set<NodeHandle> handlesOf(const std::vector<std::pair<NodeHandle, NodeSerialized>>& rows)
{
    std::set<NodeHandle> result;
    for (const auto& p: rows)
        result.insert(p.first);
    return result;
}

// ─── Per-node metadata captured at insertion time ────────────────────────────
// Used to build NodeSearchCursorOffset without relying on attribute decryption.
struct NodeMeta
{
    std::string name;
    nodetype_t type = FILENODE;
    int64_t size = 0;
    int64_t mtime = 0;
    int label = 0; ///< 0 = unlabelled, 1-7 = colour
    int fav = 0; ///< 0 or 1
    bool sensitive = false; ///< sets the "sen" attribute when true
};

// Attribute name IDs used across multiple test fixtures and test cases.
const nameid kNameId = AttrMap::string2nameid("n");
const nameid kFavId = AttrMap::string2nameid("fav");
const nameid kLabelId = AttrMap::string2nameid("lbl");

// Sort order + page size pair used by parameterised pagination tests.
struct OrderAndPageSize
{
    int order;
    size_t pageSize;
};

// ─── Shared test fixture ───────────────────────────────────────────────────────
/**
 * Base fixture used by ListAllNodesByPageTest and GroupedListAllNodesByPageTest.
 *
 * Dataset (created in SetUp):
 *   ROOT folder
 *   5 sub-folders   "Folder_A" … "Folder_E"
 *   20 file nodes   "file_01.txt" … "file_20.txt"
 *     size  = i * 100  (i = 1 … 20)
 *     mtime = 1'700'000'000 + i
 *     label = i % 4      (0 = unlabelled, 1 / 2 / 3 cycling)
 *     fav   = (i % 5 == 0) ? 1 : 0   (files 5, 10, 15, 20)
 */
class SearchByPageTest: public ::testing::Test
{
protected:
    mega::MegaApp mApp;
    NodeManager::MissingParentNodes mMissingParentNodes;
    std::shared_ptr<MegaClient> mClient;
    fs::path mTestDir;

    uint64_t mNextHandle = 1;
    NodeHandle mRootHandle;
    std::map<handle, NodeMeta> mMeta; ///< Metadata for all inserted nodes

    static constexpr int NUM_FILES = 20;
    static constexpr int NUM_FOLDERS = 5;

    // Handles exposed for the Cloud-Drive / version / sensitive filter tests.
    // These reference the jpg + Vault/Rubbish subtrees built by populateDB().
    NodeHandle hFilesRoot; // alias of mRootHandle, kept for readability
    NodeHandle hVault, hRubbish;
    NodeHandle hNormalFolder, hSensFolder;
    NodeHandle hClean, hSelfSensitive, hHead, hVersionV1, hVersionV2, hUnderSens;
    NodeHandle hVaultFile, hRubbishFile;

    void SetUp() override
    {
        mTestDir = fs::current_path() / "search_by_page_test";
        // Remove any leftover directory from a previous crashed run to avoid
        // SQLite "database is locked" errors caused by stale WAL files.
        fs::remove_all(mTestDir);
        fs::create_directories(mTestDir);

        auto* dbAccess = new SqliteDbAccess(LocalPath::fromAbsolutePath(path_u8string(mTestDir)));

        mClient = mt::makeClient(mApp, dbAccess);
        mClient->sid =
            "AWA5YAbtb4JO-y2zWxmKZpSe5-6XM7CTEkA-3Nv7J4byQUpOazdfSC1ZUFlS-kah76gPKUEkTF9g7MeE";
        mClient->opensctable();

        populateDB();

        if (auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get()))
            sa->createIndexes(/*enableSearch=*/true, /*enableLexi=*/true);
    }

    void TearDown() override
    {
        mClient.reset();
        fs::remove_all(mTestDir);
    }

    // ── Low-level node factory ────────────────────────────────────────────────
    //
    // `isFetching` is forwarded to NodeManager::addNode and defaults to true
    // (historical behaviour). Pass false when building subtrees whose
    // relationships must survive in RAM — notably file-version chains, whose
    // FLAGS_IS_VERSION bit requires the immediate parent pointer to stay live
    // instead of being evicted through the single-slot mNodeToWriteInDb buffer.
    std::shared_ptr<Node> addNode(nodetype_t type,
                                  std::shared_ptr<Node> parent,
                                  const NodeMeta& meta,
                                  bool isFetching = true)
    {
        NodeHandle h = NodeHandle().set6byte(mNextHandle++);
        Node& ref = mt::makeNode(*mClient, type, h, parent.get());
        auto node = std::shared_ptr<Node>(&ref);

        node->attrs.map[kNameId] = meta.name;

        if (type == FILENODE)
        {
            node->size = static_cast<m_off_t>(meta.size);
            node->mtime = static_cast<m_time_t>(meta.mtime);
            node->ctime = node->mtime;
            node->crc[0] = static_cast<int32_t>(mNextHandle);
            node->isvalid = true;
            node->serializefingerprint(&node->attrs.map['c']);
            node->setfingerprint();

            // sizeVirtual in the DB is derived from NodeCounter.storage.
            // Without this, ORDER BY sizeVirtual returns 0 for all files and
            // cursor-based pagination for SIZE sorts would not advance.
            NodeCounter nc;
            nc.files = 1;
            nc.storage = meta.size;
            node->setCounter(nc);
        }

        if (meta.fav)
            node->attrs.map[kFavId] = "1";

        if (meta.label > 0)
            node->attrs.map[kLabelId] = std::to_string(meta.label);

        if (meta.sensitive)
            node->attrs.map[AttrMap::string2nameid("sen")] = "1";

        mClient->mNodeManager.addNode(node,
                                      /*notify=*/false,
                                      isFetching,
                                      mMissingParentNodes);
        mClient->mNodeManager.saveNodeInDb(node.get());

        auto& stored = mMeta[h.as8byte()] = meta;
        stored.type = type;
        return node;
    }

    // ── Dataset construction ──────────────────────────────────────────────────
    //
    // Builds the shared base dataset used by every SearchByPageTest subclass:
    //
    //   CloudDrive (ROOTNODE, mRootHandle/hFilesRoot)
    //   ├── Folder_A..E                           (5 folders)
    //   ├── file_01.txt .. file_20.txt            (20 documents)
    //   ├── normal_folder/                        (non-sensitive)
    //   │   ├── clean.jpg
    //   │   ├── self_sensitive.jpg   [sen=1]
    //   │   └── head.jpg                          (HEAD)
    //   │       ├── head.jpg         (version v1, FILENODE child of FILENODE)
    //   │       └── head.jpg         (version v2)
    //   └── sens_folder/             [sen=1]      (sensitive ancestor)
    //       └── under_sens.jpg
    //   Vault (VAULTNODE) / vault_file.jpg
    //   Rubbish (RUBBISHNODE) / rubbish_file.jpg
    //
    // The .jpg / Vault / Rubbish additions are transparent to pagination tests
    // that query single-mime DOCUMENT — Vault/Rubbish roots fall outside the
    // Cloud-Drive-rooted filter, jpg files don't match MIME_TYPE_DOCUMENT, and
    // version children are always excluded by listAllNodesByPage.
    virtual void populateDB()
    {
        NodeMeta rootMeta{"ROOT", ROOTNODE, 0, 0, 0, 0};
        auto root = addNode(ROOTNODE, nullptr, rootMeta);
        mRootHandle = root->nodeHandle();
        hFilesRoot = mRootHandle;

        const std::string alpha = "ABCDE";
        for (int i = 0; i < NUM_FOLDERS; ++i)
        {
            NodeMeta fm;
            fm.name = "Folder_" + std::string(1, alpha[static_cast<size_t>(i)]);
            fm.type = FOLDERNODE;
            addNode(FOLDERNODE, root, fm);
        }

        // File nodes
        for (int i = 1; i <= NUM_FILES; ++i)
        {
            NodeMeta fm;
            // Zero-padded name so lexicographic order == numeric order
            const std::string pad = (i < 10 ? "0" : "");
            fm.name = "file_" + pad + std::to_string(i) + ".txt";
            fm.size = static_cast<int64_t>(i) * 100;
            fm.mtime = 1'700'000'000LL + i;
            fm.label = i % 4; // 0 = unlabelled, 1/2/3 cycling
            fm.fav = (i % 5 == 0) ? 1 : 0;
            addNode(FILENODE, root, fm);
        }

        // Vault + Rubbish roots. NodeManager auto-registers them via
        // setrootnode_internal so the rootnodes.{vault,rubbish} columns are set.
        auto vault = addNode(VAULTNODE,
                             nullptr,
                             NodeMeta{"Vault", VAULTNODE},
                             /*isFetching=*/false);
        auto rubbish = addNode(RUBBISHNODE,
                               nullptr,
                               NodeMeta{"Rubbish", RUBBISHNODE},
                               /*isFetching=*/false);
        hVault = vault->nodeHandle();
        hRubbish = rubbish->nodeHandle();

        // Folders under Cloud Drive root. sens_folder carries the SENS bit so
        // its descendants must be filtered out when excludeSensitive=true.
        NodeMeta sensFolderMeta{"sens_folder", FOLDERNODE};
        sensFolderMeta.sensitive = true;
        auto normal = addNode(FOLDERNODE,
                              root,
                              NodeMeta{"normal_folder", FOLDERNODE},
                              /*isFetching=*/false);
        auto sens = addNode(FOLDERNODE, root, sensFolderMeta, /*isFetching=*/false);
        hNormalFolder = normal->nodeHandle();
        hSensFolder = sens->nodeHandle();

        const int64_t baseMtime = 1'800'000'000LL;

        // isFetching=false keeps HEAD + version children in RAM so
        // FLAGS_IS_VERSION can still be computed from the parent pointer.
        hClean = addNode(FILENODE,
                         normal,
                         NodeMeta{"clean.jpg", FILENODE, 100, baseMtime + 1},
                         /*isFetching=*/false)
                     ->nodeHandle();

        NodeMeta selfSensMeta{"self_sensitive.jpg", FILENODE, 200, baseMtime + 2};
        selfSensMeta.sensitive = true;
        hSelfSensitive =
            addNode(FILENODE, normal, selfSensMeta, /*isFetching=*/false)->nodeHandle();

        auto head = addNode(FILENODE,
                            normal,
                            NodeMeta{"head.jpg", FILENODE, 300, baseMtime + 3},
                            /*isFetching=*/false);
        hHead = head->nodeHandle();
        hVersionV1 = addNode(FILENODE,
                             head,
                             NodeMeta{"head.jpg", FILENODE, 290, baseMtime + 2},
                             /*isFetching=*/false)
                         ->nodeHandle();
        hVersionV2 = addNode(FILENODE,
                             head,
                             NodeMeta{"head.jpg", FILENODE, 280, baseMtime + 1},
                             /*isFetching=*/false)
                         ->nodeHandle();

        hUnderSens = addNode(FILENODE,
                             sens,
                             NodeMeta{"under_sens.jpg", FILENODE, 400, baseMtime + 4},
                             /*isFetching=*/false)
                         ->nodeHandle();

        hVaultFile = addNode(FILENODE,
                             vault,
                             NodeMeta{"vault_file.jpg", FILENODE, 500, baseMtime + 5},
                             /*isFetching=*/false)
                         ->nodeHandle();
        hRubbishFile = addNode(FILENODE,
                               rubbish,
                               NodeMeta{"rubbish_file.jpg", FILENODE, 600, baseMtime + 6},
                               /*isFetching=*/false)
                           ->nodeHandle();
    }

    // Single call, no limit, no cursor — returns every distinct handle matching
    // @p mime (after Cloud+Vault/version/optional-sensitive filtering).
    std::set<NodeHandle> allMatchesAsSet(MimeType_t mime, int order, bool excludeSensitive) const
    {
        auto nodes = mClient->mNodeManager.listAllNodesByPage(
            makeParams(mime, order, /*maxElements=*/0, excludeSensitive),
            CancelToken{});
        std::set<NodeHandle> out;
        for (const auto& n: nodes)
            out.insert(n->nodeHandle());
        return out;
    }

    // ── Multi-page accumulation helper ─────────────────────────────────────────
    // Accumulates all pages until empty, starting from `startCursor` (default:
    // first page). Bounded to mMeta.size()+2 iterations so that a stuck cursor
    // produces a test failure rather than an infinite hang.
    std::vector<NodeHandle>
        collectAllByPage(int order,
                         size_t pageSize,
                         MimeType_t mimeType,
                         std::optional<NodeSearchCursorOffset> startCursor = std::nullopt,
                         bool excludeSensitive = false) const
    {
        std::vector<NodeHandle> result;
        std::optional<NodeSearchCursorOffset> cursor = startCursor;

        const size_t maxPages = mMeta.size() + 2;
        size_t pageCount = 0;

        while (pageCount < maxPages)
        {
            ++pageCount;
            auto page = mClient->mNodeManager.listAllNodesByPage(
                makeParams(mimeType, order, pageSize, excludeSensitive, cursor),
                CancelToken{});
            if (page.empty())
                break;
            for (const auto& n: page)
                result.push_back(n->nodeHandle());
            cursor = cursorFor(page.back()->nodeHandle(), order);
        }

        if (pageCount >= maxPages)
        {
            ADD_FAILURE() << "collectAllByPage: exceeded " << maxPages
                          << " pages for order=" << order
                          << " – cursor likely not advancing (possible infinite loop)";
        }

        return result;
    }

    // ── Build a cursor anchored at node h for the given sort order ────────────
    NodeSearchCursorOffset cursorFor(NodeHandle h, int order) const
    {
        const NodeMeta& m = mMeta.at(h.as8byte());

        NodeSearchCursorOffset c;
        c.mLastName = m.name;
        c.mLastHandle = h.as8byte();

        switch (order)
        {
            case OrderByClause::SIZE_ASC:
            case OrderByClause::SIZE_DESC:
                c.mLastSize = m.size;
                break;
            case OrderByClause::MTIME_ASC:
            case OrderByClause::MTIME_DESC:
                c.mLastMtime = m.mtime;
                break;
            case OrderByClause::LABEL_ASC:
            case OrderByClause::LABEL_DESC:
                c.mLastLabel = m.label;
                break;
            case OrderByClause::FAV_ASC:
            case OrderByClause::FAV_DESC:
                c.mLastFav = m.fav;
                break;
            default:
                break;
        }
        return c;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  listAllNodesByPage – cursor-based global pagination tests
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Fixture for listAllNodesByPage tests.
 *
 * Inherits the SearchByPageTest dataset (5 folders + 20 .txt files plus the
 * shared jpg / Vault / Rubbish subtrees used by the filter tests) and the
 * cursorFor() helper so cursor fields are always correctly populated.
 * Adds two helpers:
 *   referenceAll()    – single-call with no limit (ground truth for order / count)
 *   collectAllByPage() – multi-page accumulation (verifies no skips / duplicates)
 */
class ListAllNodesByPageTest: public SearchByPageTest
{
protected:
    // Pagination assertions target the 20 .txt files (MIME_TYPE_DOCUMENT);
    // jpg-subtree tests pass MIME_TYPE_PHOTO explicitly.
    static constexpr MimeType_t TEST_MIME = MIME_TYPE_DOCUMENT;

    // Returns every node matching TEST_MIME in `order` in one call (no limit, no cursor).
    std::vector<NodeHandle> referenceAll(int order) const
    {
        auto nodes = mClient->mNodeManager.listAllNodesByPage(
            makeParams(TEST_MIME, order, /*maxElements=*/0),
            CancelToken{});

        std::vector<NodeHandle> result;
        result.reserve(nodes.size());
        for (const auto& n: nodes)
            result.push_back(n->nodeHandle());
        return result;
    }

    // Convenience overload using the fixture's TEST_MIME.
    // `startCursor` defaults to nullopt (first page); pass a cursor to resume mid-sequence.
    std::vector<NodeHandle>
        collectAllByPage(int order,
                         size_t pageSize,
                         std::optional<NodeSearchCursorOffset> startCursor = std::nullopt,
                         bool excludeSensitive = false) const
    {
        return SearchByPageTest::collectAllByPage(order,
                                                  pageSize,
                                                  TEST_MIME,
                                                  startCursor,
                                                  excludeSensitive);
    }

    // ── Shared body for the deletion-between-pages tests (E1, E2). ────────────
    // Fetches page 1 of size `pageSize` under `order`, then physically removes
    // the (pageSize+1)-th node from the DB, then walks the rest using the
    // cursor anchored at the last item of page 1. Asserts the deleted node is
    // absent, every reference[pageSize+1 ..] still appears, and no page-1
    // handle reappears.
    void runDeletionBetweenPages(int order, size_t pageSize) const
    {
        SCOPED_TRACE("order=" + std::to_string(order) + " pageSize=" + std::to_string(pageSize));

        auto page1 =
            mClient->mNodeManager.listAllNodesByPage(makeParams(TEST_MIME, order, pageSize),
                                                     CancelToken{});
        ASSERT_EQ(page1.size(), pageSize);

        const auto fullReference = referenceAll(order);
        ASSERT_EQ(fullReference.size(), static_cast<size_t>(NUM_FILES));

        // Delete the (pageSize+1)-th node — the first of page 2 in offset terms.
        const NodeHandle toDelete = fullReference[pageSize];
        auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
        ASSERT_NE(sa, nullptr);
        ASSERT_TRUE(sa->remove(toDelete)) << "failed to delete node from DB";

        const auto page2Plus =
            collectAllByPage(order, pageSize, cursorFor(page1.back()->nodeHandle(), order));

        const std::set<NodeHandle> page2Set(page2Plus.begin(), page2Plus.end());

        EXPECT_EQ(page2Set.count(toDelete), 0u) << "deleted node appeared in page 2+";

        // Every node after the deleted one in the reference ordering must
        // appear — this is the no-skip guarantee that cursor pagination
        // provides regardless of the cursor predicate variant.
        for (size_t i = pageSize + 1; i < fullReference.size(); ++i)
        {
            EXPECT_EQ(page2Set.count(fullReference[i]), 1u)
                << "node at reference index " << i << " was skipped after deletion";
        }

        for (const auto& n: page1)
        {
            EXPECT_EQ(page2Set.count(n->nodeHandle()), 0u)
                << "page-1 handle " << n->nodeHandle() << " repeated in page 2+";
        }
    }

    // ── Shared body for the DbTable-level invalid-roots rejection tests. ──────
    // G1e (empty), G1e' (UNDEF in list), G1g (size > kListAllMaxRoots) all
    // pin the same contract: false return + empty out vector.
    void assertDbTableRejectsRoots(const std::vector<NodeHandle>& roots,
                                   const std::string& label) const
    {
        SCOPED_TRACE(label);
        auto* table = dynamic_cast<DBTableNodes*>(mClient->sctable.get());
        ASSERT_NE(table, nullptr);

        std::vector<std::pair<NodeHandle, NodeSerialized>> out;
        const bool ok = table->listAllNodesByPage(
            makeParams(MIME_TYPE_PHOTO, OrderByClause::DEFAULT_ASC, /*maxElements=*/0),
            roots,
            out,
            CancelToken{});
        EXPECT_TRUE(out.empty()) << label << " must yield no rows";
        EXPECT_FALSE(ok) << label << " must be rejected (layer contract)";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: assert handles collected by listAllNodesByPage == reference
// ─────────────────────────────────────────────────────────────────────────────

void assertListAllMatchesReference(const std::vector<NodeHandle>& paged,
                                   const std::vector<NodeHandle>& reference,
                                   const std::string& label)
{
    ASSERT_EQ(paged.size(), reference.size()) << label << ": total count mismatch";
    for (size_t i = 0; i < reference.size(); ++i)
    {
        EXPECT_EQ(paged[i], reference[i]) << label << ": handle mismatch at index " << i;
    }
    const std::set<NodeHandle> unique(paged.begin(), paged.end());
    EXPECT_EQ(unique.size(), paged.size()) << label << ": duplicate handles found";
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group A – Input validation (error / guard paths)
// ═══════════════════════════════════════════════════════════════════════════

// A1. MIME_TYPE_UNKNOWN is rejected – returns empty result without crash.
//     The implementation guards against MIME_TYPE_UNKNOWN at the SQLite layer
//     and returns false; the NodeManager must surface an empty vector.
TEST_F(ListAllNodesByPageTest, UnknownMimeType_ReturnsEmpty)
{
    auto nodes = mClient->mNodeManager.listAllNodesByPage(
        makeParams(MIME_TYPE_UNKNOWN, OrderByClause::DEFAULT_ASC, 10),
        CancelToken{});

    EXPECT_TRUE(nodes.empty()) << "MIME_TYPE_UNKNOWN must return an empty result";
}

// A2. Valid MIME type with no matching nodes → empty result (no crash).
//     Exercises the zero-row result path for a valid, non-UNKNOWN type.
TEST_F(ListAllNodesByPageTest, ValidMimeTypeNoMatches_ReturnsEmpty)
{
    // Dataset contains only .txt files; MIME_TYPE_AUDIO has no matches.
    auto nodes = mClient->mNodeManager.listAllNodesByPage(
        makeParams(MIME_TYPE_AUDIO, OrderByClause::DEFAULT_ASC, 10),
        CancelToken{});

    EXPECT_TRUE(nodes.empty()) << "MIME_TYPE_AUDIO with no matching nodes must return empty";
}

// A3. Cursor/order mismatch → returns empty, no crash.
//
//     The implementation validates that the cursor contains the optional field
//     required by the chosen sort order before binding any SQL parameters.
//     When the required field is absent (cursor was built for a different order),
//     bindCursorParamsForListAll() returns false and listAllNodesByPage() returns
//     an empty vector without executing the query.
//
//     Mismatch matrix (cursor built for row, query order is column):
//       DEFAULT cursor → SIZE / MTIME / FAV / LABEL orders  (4 cases)
//       SIZE    cursor → MTIME / FAV / LABEL orders          (3 cases)
//       MTIME   cursor → SIZE / FAV / LABEL orders           (3 cases)
//       FAV     cursor → SIZE / MTIME / LABEL orders         (3 cases)
//       LABEL   cursor → SIZE / MTIME / FAV orders           (3 cases)
//     Total: 16 mismatch cases.
TEST_F(ListAllNodesByPageTest, CursorOrderMismatch_ReturnsEmpty)
{
    // Pick a stable mid-dataset handle (valid for all mMeta lookups).
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_GE(reference.size(), 2u);
    const NodeHandle midHandle = reference[reference.size() / 2];

    struct MismatchCase
    {
        int cursorOrder; ///< order used to build the cursor (sets its optional fields)
        int queryOrder; ///< order passed to listAllNodesByPage (needs a different field)
        const char* label;
    };

    // clang-format off
    const std::vector<MismatchCase> cases = {
        // DEFAULT cursor has no optional fields → fails for any order that needs one
        {OrderByClause::DEFAULT_ASC,  OrderByClause::SIZE_ASC,   "DEFAULT→SIZE"},
        {OrderByClause::DEFAULT_ASC,  OrderByClause::MTIME_ASC,  "DEFAULT→MTIME"},
        {OrderByClause::DEFAULT_ASC,  OrderByClause::FAV_ASC,    "DEFAULT→FAV"},
        {OrderByClause::DEFAULT_ASC,  OrderByClause::LABEL_ASC,  "DEFAULT→LABEL"},
        // SIZE cursor has mLastSize but not mLastMtime / mLastFav / mLastLabel
        {OrderByClause::SIZE_ASC,     OrderByClause::MTIME_ASC,  "SIZE→MTIME"},
        {OrderByClause::SIZE_ASC,     OrderByClause::FAV_ASC,    "SIZE→FAV"},
        {OrderByClause::SIZE_ASC,     OrderByClause::LABEL_ASC,  "SIZE→LABEL"},
        // MTIME cursor has mLastMtime but not mLastSize / mLastFav / mLastLabel
        {OrderByClause::MTIME_ASC,    OrderByClause::SIZE_ASC,   "MTIME→SIZE"},
        {OrderByClause::MTIME_ASC,    OrderByClause::FAV_ASC,    "MTIME→FAV"},
        {OrderByClause::MTIME_ASC,    OrderByClause::LABEL_ASC,  "MTIME→LABEL"},
        // FAV cursor has mLastFav but not mLastSize / mLastMtime / mLastLabel
        {OrderByClause::FAV_ASC,      OrderByClause::SIZE_ASC,   "FAV→SIZE"},
        {OrderByClause::FAV_ASC,      OrderByClause::MTIME_ASC,  "FAV→MTIME"},
        {OrderByClause::FAV_ASC,      OrderByClause::LABEL_ASC,  "FAV→LABEL"},
        // LABEL cursor has mLastLabel but not mLastSize / mLastMtime / mLastFav
        {OrderByClause::LABEL_ASC,    OrderByClause::SIZE_ASC,   "LABEL→SIZE"},
        {OrderByClause::LABEL_ASC,    OrderByClause::MTIME_ASC,  "LABEL→MTIME"},
        {OrderByClause::LABEL_ASC,    OrderByClause::FAV_ASC,    "LABEL→FAV"},
    };
    // clang-format on

    for (const auto& c: cases)
    {
        auto cursor = cursorFor(midHandle, c.cursorOrder);
        auto result = mClient->mNodeManager.listAllNodesByPage(
            makeParams(TEST_MIME, c.queryOrder, 10, /*excludeSensitive=*/false, cursor),
            CancelToken{});
        EXPECT_TRUE(result.empty())
            << c.label << ": expected empty result for cursor/order mismatch";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group B – First-page basics (no cursor)
// ═══════════════════════════════════════════════════════════════════════════

// B1. MIME filter excludes non-file nodes – only .txt files are returned.
TEST_F(ListAllNodesByPageTest, NoCursor_DocumentMimeFilter_ReturnsAllFiles)
{
    auto nodes = mClient->mNodeManager.listAllNodesByPage(
        makeParams(TEST_MIME, OrderByClause::DEFAULT_ASC, /*maxElements=*/0),
        CancelToken{});

    // Dataset has NUM_FILES .txt files; folders/root have no MIME type so are excluded.
    EXPECT_EQ(nodes.size(), static_cast<size_t>(NUM_FILES));

    const auto unique = handlesOf(nodes);
    EXPECT_EQ(unique.size(), nodes.size()) << "duplicate handles in result";
}

// B2. pageSize larger than total – all results fit in one page.
TEST_F(ListAllNodesByPageTest, PageSizeLargerThanTotal_AllInOnePage)
{
    auto page = mClient->mNodeManager.listAllNodesByPage(
        makeParams(TEST_MIME, OrderByClause::DEFAULT_ASC, 1000),
        CancelToken{});

    EXPECT_EQ(page.size(), static_cast<size_t>(NUM_FILES));
}

// B3. maxElements == 0 is the documented "no pagination" sentinel
//     (sqlite.cpp maps it to LIMIT -1). Returns every matching row in a single
//     call. Pins the contract so it cannot silently regress to "empty page".
TEST_F(ListAllNodesByPageTest, MaxElementsZero_ReturnsAllInOnePage)
{
    auto page = mClient->mNodeManager.listAllNodesByPage(
        makeParams(TEST_MIME, OrderByClause::DEFAULT_ASC, /*maxElements=*/0),
        CancelToken{});

    EXPECT_EQ(page.size(), static_cast<size_t>(NUM_FILES));
}

// B4. Cursor past the last element with the unlimited (maxElements == 0)
//     sentinel still terminates with an empty page.
TEST_F(ListAllNodesByPageTest, CursorPastLastElement_MaxElementsZero_ReturnsEmpty)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_FALSE(reference.empty());

    auto cursor = cursorFor(reference.back(), OrderByClause::DEFAULT_ASC);
    auto page = mClient->mNodeManager.listAllNodesByPage(makeParams(TEST_MIME,
                                                                    OrderByClause::DEFAULT_ASC,
                                                                    /*maxElements=*/0,
                                                                    /*excludeSensitive=*/false,
                                                                    cursor),
                                                         CancelToken{});

    EXPECT_TRUE(page.empty()) << "Cursor past last element with maxElements=0 must still be empty";
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group C – Sort order correctness
// ═══════════════════════════════════════════════════════════════════════════

// C1. FAV_DESC – non-fav nodes precede fav nodes in the result.
TEST_F(ListAllNodesByPageTest, FavDesc_AllTypes_NonFavPrecedesFav)
{
    // Dataset: fav=1 for files where i%5==0 (file_05, file_10, file_15, file_20).
    // FAV_DESC (fav ASC) → fav=0 nodes first, then fav=1 nodes.

    const auto reference = referenceAll(OrderByClause::FAV_DESC);
    ASSERT_EQ(reference.size(), static_cast<size_t>(NUM_FILES));

    bool seenFav = false;
    for (const auto& h: reference)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        const bool isFav = node->attrs.map.count(kFavId) && node->attrs.map.at(kFavId) == "1";
        if (isFav)
            seenFav = true;
        if (seenFav)
        {
            EXPECT_TRUE(isFav) << "non-fav node after fav node in FAV_DESC result: "
                               << node->displayname();
        }
    }
}

// C2. LABEL_DESC – label values are non-increasing across the result.
TEST_F(ListAllNodesByPageTest, LabelDesc_AllTypes_LabelsAreNonIncreasing)
{
    // Dataset: label = i%4 cycling (0,1,2,3).
    // LABEL_DESC: ORDER BY label DESC → label 3 group first, then 2, 1, 0.

    const auto reference = referenceAll(OrderByClause::LABEL_DESC);
    ASSERT_EQ(reference.size(), static_cast<size_t>(NUM_FILES));

    int prevLabel = INT_MAX;
    for (const auto& h: reference)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        const auto it = node->attrs.map.find(kLabelId);
        const int lbl = (it != node->attrs.map.end()) ? std::stoi(it->second) : 0;
        // Tie-breaking within a label group is name ASC then nodehandle ASC.
        // This check only verifies that the label never increases across the result.
        EXPECT_LE(lbl, prevLabel) << "label increased at node " << node->displayname();
        prevLabel = lbl;
    }
}

// C3. DEFAULT_DESC – name is non-increasing across the paged result.
//     Validates buildOrderByForListAll/buildCursorWhereForListAll for DEFAULT_DESC
//     without relying on referenceAll() as the oracle.
//     Dataset names are zero-padded so natural-case and lexicographic order agree.
TEST_F(ListAllNodesByPageTest, DefaultDesc_AllTypes_DefaultOrderIsNonIncreasing)
{
    const auto paged = collectAllByPage(OrderByClause::DEFAULT_DESC, 7);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    // DEFAULT_DESC: ORDER BY name DESC, nodehandle DESC.
    // All names in our dataset are unique → the sequence must be strictly decreasing.
    std::string prevName;
    for (const auto& h: paged)
    {
        const std::string& name = mMeta.at(h.as8byte()).name;
        if (!prevName.empty())
        {
            EXPECT_GE(prevName, name)
                << "name increased in DEFAULT_DESC result: prev=" << prevName << " curr=" << name;
        }
        prevName = name;
    }
}

// C4. SIZE_ASC – sizes are non-decreasing across the paged result.
//     Constrains buildOrderByForListAll for SIZE_ASC independently of referenceAll().
TEST_F(ListAllNodesByPageTest, SizeAsc_AllTypes_SizesAreNonDecreasing)
{
    const auto paged = collectAllByPage(OrderByClause::SIZE_ASC, 5);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    // Dataset: size = i*100 (i=1..20), all unique → strictly increasing.
    int64_t prevSize = INT64_MIN;
    for (const auto& h: paged)
    {
        const int64_t size = mMeta.at(h.as8byte()).size;
        EXPECT_LE(prevSize, size) << "size not non-decreasing in SIZE_ASC result";
        prevSize = size;
    }
}

// C5. MTIME_DESC – mtimes are non-increasing across the paged result.
//     Constrains buildOrderByForListAll for MTIME_DESC independently of referenceAll().
TEST_F(ListAllNodesByPageTest, MtimeDesc_AllTypes_MtimeIsNonIncreasing)
{
    const auto paged = collectAllByPage(OrderByClause::MTIME_DESC, 8);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    // Dataset: mtime = 1_700_000_000 + i (i=1..20), all unique → strictly decreasing.
    int64_t prevMtime = INT64_MAX;
    for (const auto& h: paged)
    {
        const int64_t mtime = mMeta.at(h.as8byte()).mtime;
        EXPECT_GE(prevMtime, mtime) << "mtime not non-increasing in MTIME_DESC result";
        prevMtime = mtime;
    }
}

// C6. LABEL_ASC – labelled nodes (label > 0) appear before unlabelled (label = 0),
//     and within the labelled section the label value is non-decreasing.
//     Tests the isZero ASC, label ASC primary keys without using referenceAll().
TEST_F(ListAllNodesByPageTest, LabelAsc_AllTypes_LabeledBeforeUnlabeledAndNonDecreasing)
{
    // LABEL_ASC: ORDER BY (CASE WHEN label=0 THEN 1 ELSE 0 END) ASC, label ASC, name ASC, …
    // labelled (isZero=0) block comes first in non-decreasing label order,
    // then unlabelled (isZero=1, label=0) block.
    const auto paged = collectAllByPage(OrderByClause::LABEL_ASC, 6);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    bool inUnlabeledSection = false;
    int prevNonZeroLabel = 0;

    for (const auto& h: paged)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        const auto it = node->attrs.map.find(kLabelId);
        const int lbl = (it != node->attrs.map.end()) ? std::stoi(it->second) : 0;

        if (lbl == 0)
        {
            inUnlabeledSection = true;
        }
        else
        {
            EXPECT_FALSE(inUnlabeledSection)
                << "labeled node appeared after unlabeled section: " << node->displayname();
            EXPECT_LE(prevNonZeroLabel, lbl)
                << "label decreased within labeled section: prev=" << prevNonZeroLabel
                << " curr=" << lbl << " at " << node->displayname();
            prevNonZeroLabel = lbl;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group D – Pagination boundary conditions
// ═══════════════════════════════════════════════════════════════════════════

// D1. pageSize equals total count → first page is full, second page is empty.
TEST_F(ListAllNodesByPageTest, PageSizeEqualsTotal_SinglePage)
{
    auto page1 = mClient->mNodeManager.listAllNodesByPage(
        makeParams(TEST_MIME, OrderByClause::DEFAULT_ASC, /*maxElements=*/NUM_FILES),
        CancelToken{});
    ASSERT_EQ(page1.size(), static_cast<size_t>(NUM_FILES));

    auto cursor = cursorFor(page1.back()->nodeHandle(), OrderByClause::DEFAULT_ASC);
    auto page2 = mClient->mNodeManager.listAllNodesByPage(makeParams(TEST_MIME,
                                                                     OrderByClause::DEFAULT_ASC,
                                                                     NUM_FILES,
                                                                     /*excludeSensitive=*/false,
                                                                     cursor),
                                                          CancelToken{});

    EXPECT_TRUE(page2.empty()) << "Page after last element must be empty";
}

// D2. pageSize=1 – advancing one node at a time produces no skips and no
//     duplicates; concatenated pages equal the single-call reference.
TEST_F(ListAllNodesByPageTest, PageSizeOne_AllTypes_NoSkipsNoDuplicates)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    const auto paged = collectAllByPage(OrderByClause::DEFAULT_ASC, 1);

    assertListAllMatchesReference(paged, reference, "PageSizeOne ALL DEFAULT_ASC");
}

// D3. Cursor past the last element → next page is empty.
TEST_F(ListAllNodesByPageTest, CursorPastLastElement_ReturnsEmpty)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_FALSE(reference.empty());

    auto cursor = cursorFor(reference.back(), OrderByClause::DEFAULT_ASC);
    auto page = mClient->mNodeManager.listAllNodesByPage(
        makeParams(TEST_MIME, OrderByClause::DEFAULT_ASC, 10, /*excludeSensitive=*/false, cursor),
        CancelToken{});

    EXPECT_TRUE(page.empty()) << "Page after last element must be empty";
}

// D4. Midpoint cursor – tail pages cover exactly reference[splitAt..end].
TEST_F(ListAllNodesByPageTest, MidpointCursor_TailMatchesReference)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_GE(reference.size(), 4u);

    const size_t splitAt = reference.size() / 2;
    const auto tail =
        collectAllByPage(OrderByClause::DEFAULT_ASC,
                         5,
                         cursorFor(reference[splitAt - 1], OrderByClause::DEFAULT_ASC));

    const std::vector<NodeHandle> expected(reference.begin() + static_cast<int>(splitAt),
                                           reference.end());
    ASSERT_EQ(tail.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(tail[i], expected[i]) << "tail index " << i;
}

// D5. DEFAULT_ASC with MIME filter – full paged result equals single-call
//     reference, and every returned node is a FILENODE.
TEST_F(ListAllNodesByPageTest, DefaultAsc_DocumentMime_PaginationMatchesReference)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    const auto paged = collectAllByPage(OrderByClause::DEFAULT_ASC, 8);

    assertListAllMatchesReference(paged, reference, "DEFAULT_ASC DOCUMENT");

    // With MIME_TYPE_DOCUMENT, only file nodes are returned (folders have no MIME type).
    for (const auto& h: paged)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->type, FILENODE) << "non-file node returned with DOCUMENT mime filter";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group E – Stability under concurrent modification
// ═══════════════════════════════════════════════════════════════════════════

// E1. Deletion between pages – cursor-based pagination must not skip any
//     remaining nodes when a node adjacent to the page boundary is deleted
//     before the next page is fetched.
//
//     With offset-based pagination, deleting the first item of page 2 would
//     shift all subsequent items up by one, causing the old first item of page 3
//     to be skipped entirely.  Cursor-based pagination is immune to this because
//     the cursor encodes a sort-key position, not an offset.
TEST_F(ListAllNodesByPageTest, Deletion_BetweenPages_NoSkip)
{
    runDeletionBetweenPages(OrderByClause::DEFAULT_ASC, /*pageSize=*/5);
}

// E2. SizeAsc_DeletionBetweenPages_NoSkip – same guarantee as E1 but using
//     SIZE_ASC, which exercises a different cursor predicate path
//     (sizeVirtual/name/nodehandle triple) vs the DEFAULT name/handle pair.
//
//     DEFAULT_ASC passing does NOT imply SIZE_ASC is also correct because the
//     cursor WHERE clauses are generated by separate branches in
//     buildCursorWhereForListAll and bound by separate branches in
//     bindCursorParamsForListAll.
TEST_F(ListAllNodesByPageTest, SizeAsc_DeletionBetweenPages_NoSkip)
{
    runDeletionBetweenPages(OrderByClause::SIZE_ASC, /*pageSize=*/5);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group F – Exhaustive sort-order coverage (simple MIME path)
// ═══════════════════════════════════════════════════════════════════════════

// F1. All 10 sort orders × distinct page sizes – paged result equals reference.
//     Each page size is chosen to exercise a different boundary condition
//     (non-divisible remainder, exact fit, mid-run, etc.).
TEST_F(ListAllNodesByPageTest, AllOrders_AllTypes_PaginationMatchesReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 7}, // 20 nodes → pages [7,7,6]
        {OrderByClause::DEFAULT_DESC, 6}, // pages [6,6,6,2]
        {OrderByClause::SIZE_ASC, 5}, // pages [5,5,5,5]
        {OrderByClause::SIZE_DESC, 9}, // pages [9,9,2]
        {OrderByClause::MTIME_ASC, 4}, // pages [4,4,4,4,4]
        {OrderByClause::MTIME_DESC, 8}, // pages [8,8,4]
        {OrderByClause::LABEL_ASC, 6}, // pages [6,6,6,2]
        {OrderByClause::LABEL_DESC, 3}, // pages [3,3,3,3,3,3,2]
        {OrderByClause::FAV_ASC, 5}, // pages [5,5,5,5]
        {OrderByClause::FAV_DESC, 7}, // pages [7,7,6]
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceAll(order);
        const auto paged = collectAllByPage(order, pageSize);
        assertListAllMatchesReference(paged,
                                      reference,
                                      "ALL order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group G – Filter semantics (default Cloud+Vault scope / explicit ancestor /
//            versions / sensitive)
//
//  Exercises the filters built on top of cursor-based pagination:
//    1. Default scope (explicitAncestor unset): Cloud + Vault subtrees;
//       Rubbish and inshare subtrees are always excluded.
//    2. File versions (FILENODE whose parent is a FILENODE) are always
//       excluded regardless of MIME filter.
//    3. When excludeSensitive = true, nodes whose own flags or any ancestor
//       (strictly below the matched root) carry FLAGS_IS_MARKED_SENSITIVE are
//       filtered out. The matched root's own SENS flag is intentionally ignored.
//
//  MIME_TYPE_PHOTO result matrix for the shared Cloud jpg subtree:
//    excludeSensitive=false: {clean, self_sensitive, head, under_sens}
//    excludeSensitive=true:  {clean, head}
//  Rubbish files and all versions never appear regardless of the flag.
// ═══════════════════════════════════════════════════════════════════════════

// G1a. Default scope (explicitAncestor unset): Cloud + Vault included; Rubbish
//      always excluded.
TEST_F(ListAllNodesByPageTest, DefaultScope_IncludesVault_ExcludesRubbish)
{
    for (bool exSens: {false, true})
    {
        SCOPED_TRACE(std::string("excludeSensitive=") + (exSens ? "true" : "false"));
        auto got = allMatchesAsSet(MIME_TYPE_PHOTO, OrderByClause::DEFAULT_ASC, exSens);
        EXPECT_EQ(got.count(hVaultFile), 1u)
            << "vault_file.jpg must be included under default scope";
        EXPECT_EQ(got.count(hRubbishFile), 0u)
            << "rubbish_file.jpg must not be included — Rubbish is excluded";
    }
}

// G1a'. Default scope when Vault is not provisioned (rootnodes.vault is UNDEF):
//       resolveListAllRoots must skip the UNDEF slot and fall back to Cloud-only.
//       Models real production accounts that never materialise a Vault rootnode
//       (folder-link sessions, password-manager-only accounts).
TEST_F(ListAllNodesByPageTest, DefaultScope_VaultNotProvisioned_FallsBackToCloudOnly)
{
    // Simulate "no Vault" by clearing the cached rootnode handle. The Vault
    // node still exists in the DB so the IN(?) bind can't accidentally match
    // it as ancestor — but resolveListAllRoots must not push it.
    mClient->mNodeManager.setRootNodeVault(NodeHandle());

    auto got = allMatchesAsSet(MIME_TYPE_PHOTO,
                               OrderByClause::DEFAULT_ASC,
                               /*excludeSensitive=*/false);
    EXPECT_GT(got.count(hClean), 0u) << "Cloud content must still be returned";
    EXPECT_EQ(got.count(hVaultFile), 0u)
        << "Vault content must not appear when rootnodes.vault is UNDEF";
    EXPECT_EQ(got.count(hRubbishFile), 0u)
        << "Rubbish remains excluded regardless of Vault provisioning";
}

// G1b. byLocationHandles (explicit ancestor) — Vault-only subset. Also covers
//      narrowing semantics: default scope is Cloud+Vault, so seeing hClean
//      excluded here proves explicitAncestors narrows to Vault only.
TEST_F(ListAllNodesByPageTest, ExplicitAncestor_VaultHandle_OnlyVaultNodesReturned)
{
    const auto got = handlesOf(
        mClient->mNodeManager.listAllNodesByPage(makeParams(MIME_TYPE_PHOTO,
                                                            OrderByClause::DEFAULT_ASC,
                                                            /*maxElements=*/0,
                                                            /*excludeSensitive=*/false,
                                                            /*cursor=*/std::nullopt,
                                                            /*explicitAncestors=*/{hVault}),
                                                 CancelToken{}));
    EXPECT_EQ(got.count(hVaultFile), 1u) << "vault_file.jpg must be included under hVault";
    EXPECT_EQ(got.count(hClean), 0u)
        << "cloud-drive node must not be included — explicitAncestors must narrow "
           "default Cloud+Vault scope to Vault only";
    EXPECT_EQ(got.count(hRubbishFile), 0u) << "rubbish nodes must not leak";
}

// G1c. byLocationHandles (explicit ancestor) — Cloud-Drive root subset.
TEST_F(ListAllNodesByPageTest, ExplicitAncestor_CloudHandle_OnlyCloudNodesReturned)
{
    const auto got = handlesOf(
        mClient->mNodeManager.listAllNodesByPage(makeParams(MIME_TYPE_PHOTO,
                                                            OrderByClause::DEFAULT_ASC,
                                                            /*maxElements=*/0,
                                                            /*excludeSensitive=*/false,
                                                            /*cursor=*/std::nullopt,
                                                            /*explicitAncestors=*/{hFilesRoot}),
                                                 CancelToken{}));
    EXPECT_EQ(got.count(hClean), 1u) << "clean.jpg under Cloud Drive must be included";
    EXPECT_EQ(got.count(hVaultFile), 0u) << "vault_file.jpg must not leak";
    EXPECT_EQ(got.count(hRubbishFile), 0u) << "rubbish_file.jpg must not leak";
}

// G1d. Sensitive filtering under default scope: a sensitive ancestor in the
//      Cloud subtree hides its descendants; Vault content is unaffected unless
//      separately flagged.
TEST_F(ListAllNodesByPageTest, Sensitive_DefaultScope_CloudSensAncestor_StillFiltered)
{
    const auto got =
        handlesOf(mClient->mNodeManager.listAllNodesByPage(makeParams(MIME_TYPE_PHOTO,
                                                                      OrderByClause::DEFAULT_ASC,
                                                                      /*maxElements=*/0,
                                                                      /*excludeSensitive=*/true),
                                                           CancelToken{}));
    // Under a sensitive Cloud ancestor (hSensFolder): hUnderSens filtered out.
    EXPECT_EQ(got.count(hUnderSens), 0u);
    // Cloud node without sens ancestor: included.
    EXPECT_EQ(got.count(hClean), 1u);
    // Vault file: not under any sens ancestor, must still appear.
    EXPECT_EQ(got.count(hVaultFile), 1u);
}

// G1e-h. DbTable-level tests for the dynamic IN(?,...) machinery. NodeManager
//        exposes at most 2 real slots (Cloud + Vault under default scope); the
//        tests below hit the DbTable virtual directly for full multi-root
//        coverage.
//
// Contract pinned by G1e / G1e' / G1g:
//   DbTable::listAllNodesByPage() rejects invalid root sets (empty, contains
//   UNDEF, or size > kListAllMaxRoots) with `false` and leaves `out` empty. Empty
//   `out` is the observable contract; `false` additionally pins the current
//   layer behaviour so a silent switch to true+empty (also valid externally)
//   is caught.
//
// G1f additionally pins the positive case: a valid root set (size in [1..
// kListAllMaxRoots], no UNDEF) returns ok=true with the union of all reachable
// subtrees — confirming the IN-list machinery emits every row any root reaches
// (with file versions still excluded).
//
// G1h additionally pins that a duplicate handle in the IN-list does not produce
// duplicate rows — a positive (ok=true) case validating structural dedup via
// the EXISTS up-walk.

// G1e. Empty filesRoots → rejected (guard path).
TEST_F(ListAllNodesByPageTest, DbTable_EmptyRoots_ReturnsEmpty)
{
    assertDbTableRejectsRoots(/*roots=*/{}, "empty filesRoots");
}

// G1e'. Any UNDEF in filesRoots → rejected (caller contract).
TEST_F(ListAllNodesByPageTest, DbTable_UndefInRoots_ReturnsEmpty)
{
    assertDbTableRejectsRoots({hFilesRoot, NodeHandle()}, "UNDEF in filesRoots");
}

// G1f. Three roots (Cloud + Vault + Rubbish) — result is the union of the
//      three subtrees (file versions still excluded).
TEST_F(ListAllNodesByPageTest, DbTable_ThreeRoots_UnionReturned)
{
    auto* table = dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    ASSERT_NE(table, nullptr);

    std::vector<std::pair<NodeHandle, NodeSerialized>> out;
    const std::vector<NodeHandle> roots{hFilesRoot, hVault, hRubbish};
    const bool ok = table->listAllNodesByPage(
        makeParams(MIME_TYPE_PHOTO, OrderByClause::DEFAULT_ASC, /*maxElements=*/0),
        roots,
        out,
        CancelToken{});
    ASSERT_TRUE(ok);

    const auto got = handlesOf(out);
    EXPECT_EQ(got.count(hVaultFile), 1u) << "vault subtree must be included";
    EXPECT_EQ(got.count(hRubbishFile), 1u) << "rubbish subtree must be included";
    EXPECT_GT(got.count(hClean), 0u) << "cloud subtree must be included";
    // Versions still excluded regardless of root count.
    EXPECT_EQ(got.count(hVersionV1), 0u);
    EXPECT_EQ(got.count(hVersionV2), 0u);
}

// G1g. filesRoots.size() > kListAllMaxRoots(=3) → rejected. MEGA has 3 rootnodes
//      structurally; sizes beyond that indicate a caller bug. Direct DbTable
//      call because NodeManager::resolveListAllRoots tops out at 2.
TEST_F(ListAllNodesByPageTest, DbTable_TooManyRoots_ReturnsEmpty)
{
    // 4 valid roots — exceeds kListAllMaxRoots=3.
    assertDbTableRejectsRoots({hFilesRoot, hVault, hRubbish, hFilesRoot},
                              "size > kListAllMaxRoots");
}

// G1h. Same handle passed twice — result deduped (not doubled).
//      Dedup is structural: each row of `nodes` is matched at most once by the
//      EXISTS up-walk regardless of how many times a root appears in the
//      IN-list, so duplicates cannot arise — no GROUP BY required.
TEST_F(ListAllNodesByPageTest, DbTable_DuplicateRootSlot_NoDuplicates)
{
    auto* table = dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    ASSERT_NE(table, nullptr);

    std::vector<std::pair<NodeHandle, NodeSerialized>> out;
    // Duplicate the Cloud root in two slots. Expected result count = same as
    // single-Cloud.
    const std::vector<NodeHandle> roots{hFilesRoot, hFilesRoot};
    const bool ok = table->listAllNodesByPage(
        makeParams(MIME_TYPE_PHOTO, OrderByClause::DEFAULT_ASC, /*maxElements=*/0),
        roots,
        out,
        CancelToken{});
    ASSERT_TRUE(ok);

    const auto got = handlesOf(out);
    EXPECT_EQ(got.size(), out.size()) << "duplicate root must not produce duplicate rows";
    EXPECT_GT(out.size(), 0u) << "fixture invariant: cloud subtree must contain ≥ 1 photo";
}

// G2. Versions (FILENODE with FILENODE parent) are always excluded.
TEST_F(ListAllNodesByPageTest, ExcludesFileVersions)
{
    // Sanity: confirm the version bit is set on the in-memory Node. If this
    // fails, the problem is in Node construction / parent linkage, not the SQL.
    auto v1 = mClient->mNodeManager.getNodeByHandle(hVersionV1);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v1->parent, nullptr) << "version node must have its parent pointer set";
    EXPECT_EQ(v1->parent->type, FILENODE)
        << "version's parent must be a FILENODE so FLAGS_IS_VERSION is computed true";
    EXPECT_TRUE(v1->getDBFlagsBitset().test(Node::FLAGS_IS_VERSION))
        << "FLAGS_IS_VERSION should be set for a FILENODE child of a FILENODE";

    for (bool exSens: {false, true})
    {
        SCOPED_TRACE(std::string("excludeSensitive=") + (exSens ? "true" : "false"));
        auto got = allMatchesAsSet(MIME_TYPE_PHOTO, OrderByClause::DEFAULT_ASC, exSens);
        EXPECT_EQ(got.count(hVersionV1), 0u) << "version_v1.jpg returned";
        EXPECT_EQ(got.count(hVersionV2), 0u) << "version_v2.jpg returned";
        EXPECT_EQ(got.count(hHead), 1u) << "HEAD must always be included";
    }
}

// G3. excludeSensitive: node whose own SENS bit is set is excluded.
TEST_F(ListAllNodesByPageTest, Sensitive_NodeDirectlyMarked)
{
    // self_sensitive.jpg: own SENS bit set.
    EXPECT_EQ(allMatchesAsSet(MIME_TYPE_PHOTO,
                              OrderByClause::DEFAULT_ASC,
                              /*excludeSensitive=*/false)
                  .count(hSelfSensitive),
              1u);
    EXPECT_EQ(allMatchesAsSet(MIME_TYPE_PHOTO,
                              OrderByClause::DEFAULT_ASC,
                              /*excludeSensitive=*/true)
                  .count(hSelfSensitive),
              0u);
}

// G4. excludeSensitive: node whose ancestor has SENS bit is excluded.
TEST_F(ListAllNodesByPageTest, Sensitive_AncestorMarked)
{
    // under_sens.jpg: own flag is clean but sens_folder (parent) has SENS bit.
    EXPECT_EQ(allMatchesAsSet(MIME_TYPE_PHOTO,
                              OrderByClause::DEFAULT_ASC,
                              /*excludeSensitive=*/false)
                  .count(hUnderSens),
              1u);
    EXPECT_EQ(allMatchesAsSet(MIME_TYPE_PHOTO,
                              OrderByClause::DEFAULT_ASC,
                              /*excludeSensitive=*/true)
                  .count(hUnderSens),
              0u)
        << "sensitive ancestor must propagate to descendant when filtering";
}

// G5. excludeSensitive: Cloud Drive root's own SENS bit is intentionally ignored.
TEST_F(ListAllNodesByPageTest, Sensitive_FilesRootMarked_DescendantsStillReturned)
{
    // Mark the Cloud Drive root itself as sensitive. The up-walk CTE deliberately
    // stops BEFORE inspecting filesRoot, so descendants must remain visible.
    auto root = mClient->mNodeManager.getNodeByHandle(hFilesRoot);
    ASSERT_NE(root, nullptr);
    root->attrs.map[AttrMap::string2nameid("sen")] = "1";
    mClient->mNodeManager.saveNodeInDb(root.get());

    // Sanity: the attr-map poke + saveNodeInDb must land the SENS bit in the
    // `flags` column the EXISTS walk reads. If this fails, the test below
    // would pass for the wrong reason (no SENS anywhere, not root-ignored).
    ASSERT_TRUE(root->getDBFlagsBitset().test(Node::FLAGS_IS_MARKED_SENSITIVE))
        << "SENS bit did not propagate from attrs.map to flags after saveNodeInDb";

    auto got = allMatchesAsSet(MIME_TYPE_PHOTO,
                               OrderByClause::DEFAULT_ASC,
                               /*excludeSensitive=*/true);
    // clean.jpg and head.jpg are both strictly-non-sensitive and should still
    // appear even though the root above them now carries the SENS bit.
    EXPECT_EQ(got.count(hClean), 1u) << "clean.jpg should remain despite root SENS bit";
    EXPECT_EQ(got.count(hHead), 1u) << "head.jpg should remain despite root SENS bit";
}

// G6. excludeSensitive: filtered set is a strict subset of the unfiltered set.
TEST_F(ListAllNodesByPageTest, Sensitive_ResultIsSubsetWhenEnabled)
{
    auto all = allMatchesAsSet(MIME_TYPE_PHOTO,
                               OrderByClause::DEFAULT_ASC,
                               /*excludeSensitive=*/false);
    auto filtered = allMatchesAsSet(MIME_TYPE_PHOTO,
                                    OrderByClause::DEFAULT_ASC,
                                    /*excludeSensitive=*/true);

    // Strict subset: everything `filtered` returned must be in `all`, and the two
    // sets must not be equal (our dataset contains at least one sensitive hit).
    for (const auto& h: filtered)
        EXPECT_EQ(all.count(h), 1u) << "filtered leaked a handle not in unfiltered set";
    EXPECT_LT(filtered.size(), all.size())
        << "dataset must contain at least one sensitive-excluded node";
}

// ─────────────────────────────────────────────────────────────────────────────
//  TieBreakTest
//  Minimal fixture whose entire dataset shares the same (size, mtime, label, name).
//  Only the nodehandle differs between nodes, so every sort order that reaches the
//  nodehandle tiebreaker is exercised in its pure form.
//
//  Dataset: ROOT + 5 file nodes, all "tied.txt" (MIME_TYPE_DOCUMENT)
//    size  = 500        (constant → SIZE sorts tie after primary key)
//    mtime = 1'900'000'000  (constant → MTIME sorts tie)
//    label = 2          (nonzero → LABEL_ASC isZero=0; all in same label bucket)
//    fav   = 0
//  Handles are assigned sequentially during insertion:
//    mTiedHandles[0] has the smallest handle, mTiedHandles[4] the largest.
// ─────────────────────────────────────────────────────────────────────────────

class TieBreakTest: public SearchByPageTest
{
protected:
    static constexpr MimeType_t TEST_MIME = MIME_TYPE_DOCUMENT;
    static constexpr int64_t kTiedSize = 500;
    static constexpr int64_t kTiedMtime = 1'900'000'000LL;
    static constexpr int kTiedLabel = 2;
    static constexpr int kNumTied = 5;

    std::vector<NodeHandle> mTiedHandles; ///< insertion order = ascending handle order

    void populateDB() override
    {
        auto root = addNode(ROOTNODE, nullptr, NodeMeta{"ROOT", ROOTNODE, 0, 0, 0, 0});
        mRootHandle = root->nodeHandle();

        for (int i = 0; i < kNumTied; ++i)
        {
            NodeMeta m;
            m.name = "tied.txt";
            m.size = kTiedSize;
            m.mtime = kTiedMtime;
            m.label = kTiedLabel;
            m.fav = 0;
            auto node = addNode(FILENODE, root, m);
            mTiedHandles.push_back(node->nodeHandle());
        }
        // mTiedHandles[0].as8byte() < … < mTiedHandles[4].as8byte()
    }

    std::vector<NodeHandle> collectAllByPage(int order, size_t pageSize) const
    {
        return SearchByPageTest::collectAllByPage(order, pageSize, TEST_MIME);
    }

    // Shared assertions for the H-group tiebreaker tests. All tests:
    //   - paginate with pageSize=2 to split inside the tied group,
    //   - check no duplicate handles,
    //   - check the nodehandle tiebreaker advances strictly in the requested
    //     direction, and
    //   - check the full result equals the expected handle sequence
    //     (mTiedHandles for ascending, reversed for descending).
    void assertTiedHandleOrder(const std::vector<NodeHandle>& paged,
                               bool ascending,
                               const std::string& label) const
    {
        SCOPED_TRACE(label);
        ASSERT_EQ(paged.size(), static_cast<size_t>(kNumTied));

        const std::set<NodeHandle> unique(paged.begin(), paged.end());
        ASSERT_EQ(unique.size(), paged.size())
            << "duplicate handles in " << label << " tied result";

        for (size_t i = 1; i < paged.size(); ++i)
        {
            if (ascending)
            {
                EXPECT_GT(paged[i].as8byte(), paged[i - 1].as8byte())
                    << "nodehandle did not advance (ASC) in " << label << " tied sequence at index "
                    << i;
            }
            else
            {
                EXPECT_LT(paged[i].as8byte(), paged[i - 1].as8byte())
                    << "nodehandle did not decrease (DESC) in " << label
                    << " tied sequence at index " << i;
            }
        }

        const std::vector<NodeHandle> expected =
            ascending ? mTiedHandles :
                        std::vector<NodeHandle>(mTiedHandles.rbegin(), mTiedHandles.rend());
        EXPECT_EQ(paged, expected) << label << " tied result does not match expected order";
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Group H – Nodehandle tiebreaker: split at page boundary inside a tied group
//
//  Assertions for each test:
//    1. No duplicate handles across the full result.
//    2. The relative handle order across every consecutive pair is consistent
//       with the sort direction (ASC → strictly increasing, DESC → strictly
//       decreasing). This proves the cursor correctly resumed inside the tied
//       group rather than repeating or skipping entries.
//    3. The full result equals the expected handle sequence.
// ═══════════════════════════════════════════════════════════════════════════

// H1. LABEL_ASC with tied (isZero, label, name) – nodehandle ASC breaks the tie.
//     Exercises buildCursorWhereForListAll (LABEL_ASC, sqlite.cpp:2686-2698):
//       isZero = p1 AND label = p2 AND name = p3 AND nodehandle > p4
//     LABEL_ASC reduces to nodehandle ASC → expected: mTiedHandles[0..4].
TEST_F(TieBreakTest, LabelAsc_TiedLabelAndName_UsesNodehandleTieBreak)
{
    assertTiedHandleOrder(collectAllByPage(OrderByClause::LABEL_ASC, 2),
                          /*ascending=*/true,
                          "LABEL_ASC");
}

// H2. SIZE_DESC with tied (size, name) – nodehandle DESC breaks the tie.
//     Exercises buildCursorWhereForListAll (SIZE_DESC, sqlite.cpp:2646-2652):
//       sizeVirtual = p1 AND name = p2 AND nodehandle < p3
//     SIZE_DESC: (size DESC, name DESC, nodehandle DESC) → reversed handles.
TEST_F(TieBreakTest, SizeDesc_TiedSizeAndName_UsesNodehandleTieBreak)
{
    assertTiedHandleOrder(collectAllByPage(OrderByClause::SIZE_DESC, 2),
                          /*ascending=*/false,
                          "SIZE_DESC");
}

// H3. MTIME_ASC with tied (mtime, name) – nodehandle ASC breaks the tie.
//     Exercises buildCursorWhereForListAll (MTIME_ASC, sqlite.cpp:2654-2660):
//       mtime = p1 AND name = p2 AND nodehandle > p3
//     MTIME_ASC: (mtime ASC, name ASC, nodehandle ASC) → mTiedHandles[0..4].
TEST_F(TieBreakTest, MtimeAsc_TiedMtimeAndName_UsesNodehandleTieBreak)
{
    assertTiedHandleOrder(collectAllByPage(OrderByClause::MTIME_ASC, 2),
                          /*ascending=*/true,
                          "MTIME_ASC");
}

// H4. FAV_DESC with tied (fav, name) – nodehandle ASC breaks the tie.
//     Exercises buildCursorWhereForListAll (FAV_DESC, sqlite.cpp:2678-2684):
//       fav = p1 AND name = p2 AND nodehandle > p3
//
//     This is the "reverse" counterpart to H2 (SIZE_DESC): although both sort
//     orders carry "DESC" in their name, their tiebreaker directions differ.
//     SIZE_DESC uses  nodehandle < p3  (descending),
//     FAV_DESC  uses  nodehandle > p3  (ascending, because fav/name are both ASC
//     in the underlying ORDER BY clause). A bug that swapped > / < for one of
//     them would not be caught by the other test — that's why H4 stays separate
//     from H2 even though both are "DESC" orderings.
TEST_F(TieBreakTest, FavDesc_TiedFavAndName_UsesNodehandleAscTieBreak)
{
    assertTiedHandleOrder(collectAllByPage(OrderByClause::FAV_DESC, 2),
                          /*ascending=*/true,
                          "FAV_DESC");
}

// H5. DEFAULT_ASC with tied name – nodehandle ASC breaks the tie.
//     Exercises buildCursorWhereForListAll (DEFAULT_ASC):
//       (name > p1) OR (name = p1 AND nodehandle > p2)
//     With every name == "tied.txt", reduces to pure nodehandle ASC.
//     Distinct from H1/H3 because the DEFAULT cursor branch carries no
//     leading sort-key field (no isZero/label/mtime/fav predicate term),
//     so a regression in that branch is invisible to the other H tests.
TEST_F(TieBreakTest, DefaultAsc_TiedName_UsesNodehandleTieBreak)
{
    assertTiedHandleOrder(collectAllByPage(OrderByClause::DEFAULT_ASC, 2),
                          /*ascending=*/true,
                          "DEFAULT_ASC");
}

// H6. DEFAULT_DESC with tied name – nodehandle DESC breaks the tie.
//     Exercises buildCursorWhereForListAll (DEFAULT_DESC):
//       (name < p1) OR (name = p1 AND nodehandle < p2)
//     A sign-flip in the DEFAULT_DESC branch (e.g. '>' instead of '<')
//     would not be caught by H2 (SIZE_DESC) because the SIZE cursor uses
//     a different SQL fragment with the sizeVirtual predicate term.
TEST_F(TieBreakTest, DefaultDesc_TiedName_UsesNodehandleDescTieBreak)
{
    assertTiedHandleOrder(collectAllByPage(OrderByClause::DEFAULT_DESC, 2),
                          /*ascending=*/false,
                          "DEFAULT_DESC");
}

// ─────────────────────────────────────────────────────────────────────────────
//  GroupedListAllNodesByPageTest
//  Tests the grouped (UNION ALL CTE) code path used for MIME_TYPE_ALL_DOCS and
//  MIME_TYPE_ALL_VISUAL_MEDIA, plus the simple path for their constituent types
//  (MIME_TYPE_VIDEO, MIME_TYPE_PHOTO) individually.
//
//  Additional dataset (appended on top of the base 20 .txt files):
//    ALL_DOCS members:
//      report_alpha.pdf, notes_delta.pdf  → MIME_TYPE_PDF
//      slides_beta.pptx                  → MIME_TYPE_PRESENTATION
//      sheet_gamma.xlsx                  → MIME_TYPE_SPREADSHEET
//    ALL_VISUAL_MEDIA members:
//      photo_alpha.jpg, photo_beta.jpg, photo_gamma.jpg  → MIME_TYPE_PHOTO
//      video_alpha.mp4, video_beta.mp4, video_gamma.mp4  → MIME_TYPE_VIDEO
// ─────────────────────────────────────────────────────────────────────────────

class GroupedListAllNodesByPageTest: public SearchByPageTest
{
protected:
    // Sensitive nodes used by grouped-path excludeSensitive TEST_Fs. A .mp4
    // covers the VIDEO route of MIME_TYPE_ALL_VISUAL_MEDIA; a .pdf covers the
    // PDF route of MIME_TYPE_ALL_DOCS. The base fixture already contributes a
    // sensitive .jpg (self_sensitive.jpg) for the PHOTO route.
    NodeHandle hVideoSensitive;
    NodeHandle hReportSensitive;

    // Dataset cardinality under the default Cloud+Vault scope (versions excluded,
    // Rubbish excluded). Used by the ASSERT_EQ guard in C1/C2/D1 so a dataset
    // tweak updates one place.
    //
    // PHOTO: Cloud subtree contributes {clean, self_sens, head, under_sens} = 4;
    //        Vault subtree contributes {vault_file} = 1; total filter-subtree = 5.
    //        VIDEO has no entries in the filter subtree.
    static constexpr size_t kBaseFilterSubtreePhotos = 5;
    static constexpr size_t kGroupedFixturePhotos = 3; // photo_alpha/beta/gamma
    static constexpr size_t kGroupedFixtureVideos = 3; // video_alpha/beta/gamma
    static constexpr size_t kGroupedSensitiveVideos = 1; // video_sensitive
    static constexpr size_t kPhotoTotal = kGroupedFixturePhotos + kBaseFilterSubtreePhotos;
    static constexpr size_t kVideoTotal = kGroupedFixtureVideos + kGroupedSensitiveVideos;
    static constexpr size_t kVisualMediaTotal = kPhotoTotal + kVideoTotal;

    void SetUp() override
    {
        SearchByPageTest::SetUp();

        auto root = mClient->mNodeManager.getNodeByHandle(mRootHandle);
        ASSERT_NE(root, nullptr);

        const int64_t baseMtime = 1'800'000'000LL;

        addNode(FILENODE, root, NodeMeta{"report_alpha.pdf", FILENODE, 410, baseMtime + 1, 1, 0});
        addNode(FILENODE, root, NodeMeta{"slides_beta.pptx", FILENODE, 240, baseMtime + 2, 2, 1});
        addNode(FILENODE, root, NodeMeta{"sheet_gamma.xlsx", FILENODE, 510, baseMtime + 3, 3, 0});
        addNode(FILENODE, root, NodeMeta{"notes_delta.pdf", FILENODE, 180, baseMtime + 4, 0, 1});

        addNode(FILENODE, root, NodeMeta{"photo_alpha.jpg", FILENODE, 90, baseMtime + 11, 1, 0});
        addNode(FILENODE, root, NodeMeta{"photo_beta.jpg", FILENODE, 120, baseMtime + 12, 0, 1});
        addNode(FILENODE, root, NodeMeta{"photo_gamma.jpg", FILENODE, 80, baseMtime + 13, 2, 0});
        addNode(FILENODE, root, NodeMeta{"video_alpha.mp4", FILENODE, 900, baseMtime + 21, 0, 1});
        addNode(FILENODE, root, NodeMeta{"video_beta.mp4", FILENODE, 760, baseMtime + 22, 3, 0});
        addNode(FILENODE, root, NodeMeta{"video_gamma.mp4", FILENODE, 830, baseMtime + 23, 1, 1});

        // Sensitive-flagged FILENODEs placed directly under the Cloud Drive root
        // (not inside sens_folder) so that only the node's own SENS bit matters.
        NodeMeta sensPdf{"report_sensitive.pdf", FILENODE, 300, baseMtime + 5, 0, 0};
        sensPdf.sensitive = true;
        hReportSensitive = addNode(FILENODE, root, sensPdf, /*isFetching=*/false)->nodeHandle();

        NodeMeta sensVideo{"video_sensitive.mp4", FILENODE, 800, baseMtime + 24, 0, 0};
        sensVideo.sensitive = true;
        hVideoSensitive = addNode(FILENODE, root, sensVideo, /*isFetching=*/false)->nodeHandle();
    }

    // Ground-truth via searchNodes() (single call, no cursor). When
    // @p excludeSensitive is true, the filter switches NodeSearchFilter's
    // bySensitivity to BoolFilter::onlyTrue, which the searchNodes SQL
    // interprets as "exclude sensitive nodes" (see recent_actions.cpp:400,
    // isValidSensitivity in nodemanager.cpp:102, and nodesCTE up-walk check
    // at sqlite.cpp:2405 — the BoolFilter enum names are counter-intuitive:
    // onlyTrue = drop sensitive rows, onlyFalse = keep only sensitive rows).
    std::vector<NodeHandle> referenceBySearch(int order,
                                              MimeType_t mimeType,
                                              bool excludeSensitive = false) const
    {
        NodeSearchFilter filter;
        // Match listAllNodesByPage's default scope: Cloud + Vault.
        filter.byAncestors({mRootHandle.as8byte(), hVault.as8byte(), UNDEF});
        filter.byNodeType(FILENODE);
        filter.byCategory(mimeType);
        if (excludeSensitive)
            filter.bySensitivity(NodeSearchFilter::BoolFilter::onlyTrue);

        auto nodes =
            mClient->mNodeManager.searchNodes(filter, order, CancelToken{}, NodeSearchPage{0, 0});

        std::vector<NodeHandle> result;
        result.reserve(nodes.size());
        for (const auto& n: nodes)
            result.push_back(n->nodeHandle());
        return result;
    }

    // Sweep all 10 sort orders for @p mime, asserting the paginated walk equals
    // the searchNodes reference and that the reference contains @p expectedCount
    // entries (a fixture-cardinality guard — picks up dataset drift before the
    // ordered comparison fires). Used by the C1/C2 / B1 sweep tests.
    void assertAllOrdersPaginationMatchesSearch(MimeType_t mime,
                                                size_t expectedCount,
                                                const std::vector<OrderAndPageSize>& cases,
                                                const std::string& mimeLabel)
    {
        for (const auto& [order, pageSize]: cases)
        {
            const auto reference = referenceBySearch(order, mime);
            const auto paged = collectAllByPage(order, pageSize, mime);
            ASSERT_EQ(reference.size(), expectedCount)
                << "unexpected " << mimeLabel << " count for order=" << order;
            assertListAllMatchesReference(paged,
                                          reference,
                                          mimeLabel + " order=" + std::to_string(order) +
                                              " pageSize=" + std::to_string(pageSize));
        }
    }
};

// Default per-order page-size grid for the simple-path sweep tests
// (C1: VIDEO, C2: PHOTO). Picked so DEFAULT/MTIME/FAV/LABEL each split the
// 4-/8-node datasets at a non-divisible boundary.
inline const std::vector<OrderAndPageSize> kSimplePathOrderCases = {
    {OrderByClause::DEFAULT_ASC, 2},
    {OrderByClause::DEFAULT_DESC, 3},
    {OrderByClause::SIZE_ASC, 2},
    {OrderByClause::SIZE_DESC, 3},
    {OrderByClause::MTIME_ASC, 2},
    {OrderByClause::MTIME_DESC, 3},
    {OrderByClause::LABEL_ASC, 2},
    {OrderByClause::LABEL_DESC, 3},
    {OrderByClause::FAV_ASC, 2},
    {OrderByClause::FAV_DESC, 3},
};

// ═══════════════════════════════════════════════════════════════════════════
//  Group A – MIME_TYPE_ALL_DOCS (grouped path: UNION ALL CTE over
//            DOCUMENT + PDF + PRESENTATION + SPREADSHEET routes)
//  Dataset: 20 .txt + 3 .pdf (1 sensitive) + 1 .pptx + 1 .xlsx = 25 nodes
//  (A1 uses excludeSensitive=false, so reference and paged both include
//   report_sensitive.pdf. See Group E for the excludeSensitive=true path.)
// ═══════════════════════════════════════════════════════════════════════════

// A1. All 10 sort orders – paged result equals searchNodes reference for each.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_AllDocs_PaginationMatchesSearchReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 7},
        {OrderByClause::DEFAULT_DESC, 6},
        {OrderByClause::SIZE_ASC, 5},
        {OrderByClause::SIZE_DESC, 9},
        {OrderByClause::MTIME_ASC, 4},
        {OrderByClause::MTIME_DESC, 8},
        {OrderByClause::LABEL_ASC, 6},
        {OrderByClause::LABEL_DESC, 3},
        {OrderByClause::FAV_ASC, 5},
        {OrderByClause::FAV_DESC, 7},
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceBySearch(order, MIME_TYPE_ALL_DOCS);
        const auto paged = collectAllByPage(order, pageSize, MIME_TYPE_ALL_DOCS);
        assertListAllMatchesReference(paged,
                                      reference,
                                      "ALL_DOCS order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group B – MIME_TYPE_ALL_VISUAL_MEDIA (grouped path: UNION ALL CTE over
//            PHOTO + VIDEO routes)
//  Dataset: 8 .jpg + 4 .mp4 = kVisualMediaTotal (12) Cloud+Vault-scope nodes
//  (4 jpg from SearchByPageTest's Cloud filter subtree + 1 vault_file jpg +
//   3 photo_* jpg + 3 video_* mp4 + 1 sensitive mp4. Versions of head.jpg and
//   rubbish_file.jpg are excluded by the listAllNodesByPage filter chain.)
// ═══════════════════════════════════════════════════════════════════════════

// B1. All 10 sort orders – paged result equals searchNodes reference for each.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_AllVisualMedia_PaginationMatchesSearchReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 3},
        {OrderByClause::DEFAULT_DESC, 2},
        {OrderByClause::SIZE_ASC, 3},
        {OrderByClause::SIZE_DESC, 2},
        {OrderByClause::MTIME_ASC, 3},
        {OrderByClause::MTIME_DESC, 2},
        {OrderByClause::LABEL_ASC, 3},
        {OrderByClause::LABEL_DESC, 2},
        {OrderByClause::FAV_ASC, 3},
        {OrderByClause::FAV_DESC, 2},
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceBySearch(order, MIME_TYPE_ALL_VISUAL_MEDIA);
        const auto paged = collectAllByPage(order, pageSize, MIME_TYPE_ALL_VISUAL_MEDIA);
        assertListAllMatchesReference(paged,
                                      reference,
                                      "ALL_VISUAL_MEDIA order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group C – Simple path for individual MIME types that belong to groups
//  Verifies that the non-grouped SQL path (mimetypeVirtual = ?) is exercised
//  correctly for MIME_TYPE_VIDEO and MIME_TYPE_PHOTO, which also appear as
//  constituent members inside the ALL_VISUAL_MEDIA grouped path.
// ═══════════════════════════════════════════════════════════════════════════

// C1. MIME_TYPE_VIDEO individually – all 10 sort orders.
//     Video count = 3 non-sensitive .mp4 (video_alpha/beta/gamma) + 1 sensitive
//     .mp4 (video_sensitive, used by grouped excludeSensitive tests) = 4 nodes.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_Video_SimplePathPaginationMatchesSearchReference)
{
    assertAllOrdersPaginationMatchesSearch(MIME_TYPE_VIDEO,
                                           kVideoTotal,
                                           kSimplePathOrderCases,
                                           "VIDEO");
}

// C2. MIME_TYPE_PHOTO individually – all 10 sort orders.
//     Photo count under the default Cloud+Vault scope =
//         3 photo_*.jpg (this fixture) + 4 jpg from SearchByPageTest's Cloud
//         subtree (clean, self_sensitive, head, under_sens) + 1 jpg in Vault
//         (vault_file) = 8 nodes.
//     Versions of head.jpg are always excluded; rubbish_file is filtered out
//     because Rubbish is not part of the default scope.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_Photo_SimplePathPaginationMatchesSearchReference)
{
    assertAllOrdersPaginationMatchesSearch(MIME_TYPE_PHOTO,
                                           kPhotoTotal,
                                           kSimplePathOrderCases,
                                           "PHOTO");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group D – Stability under concurrent modification (grouped MIME path)
// ═══════════════════════════════════════════════════════════════════════════

// D1. AllVisualMedia_DefaultDesc_DeletionBetweenPages_NoSkip
//     Mirrors ListAllNodesByPageTest::Deletion_BetweenPages_NoSkip (E1) but uses
//     the grouped MIME path (MIME_TYPE_ALL_VISUAL_MEDIA → UNION ALL CTE over
//     PHOTO + VIDEO, sqlite.cpp:2878).  A delete between pages must not cause
//     cursor-based pagination to skip any remaining visual-media nodes.
//
//     Dataset under default Cloud+Vault scope: 8 .jpg (3 photo_* + 4 from the
//     Cloud filter subtree + 1 vault_file) + 3 non-sensitive .mp4 + 1 sensitive
//     .mp4 = 12 visual-media nodes. excludeSensitive=false keeps them all.
//     pageSize=3, delete the node at reference[3] (first of page 2), then
//     verify no remaining node is skipped and page-1 entries aren't repeated.
TEST_F(GroupedListAllNodesByPageTest, AllVisualMedia_DefaultDesc_DeletionBetweenPages_NoSkip)
{
    constexpr MimeType_t mime = MIME_TYPE_ALL_VISUAL_MEDIA;
    constexpr int order = OrderByClause::DEFAULT_DESC;
    constexpr size_t pageSize = 3;

    const auto fullReference = referenceBySearch(order, mime);
    ASSERT_EQ(fullReference.size(), kVisualMediaTotal);

    auto page1 =
        mClient->mNodeManager.listAllNodesByPage(makeParams(mime, order, pageSize), CancelToken{});
    ASSERT_EQ(page1.size(), pageSize);

    // Delete the first node of what would be page 2 in offset terms.
    const NodeHandle toDelete = fullReference[pageSize];
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_TRUE(sa->remove(toDelete)) << "failed to delete node from DB";

    const auto page2Plus =
        collectAllByPage(order, pageSize, mime, cursorFor(page1.back()->nodeHandle(), order));

    const std::set<NodeHandle> page2Set(page2Plus.begin(), page2Plus.end());

    EXPECT_EQ(page2Set.count(toDelete), 0u) << "deleted node appeared in page 2+";

    // Every node after the deleted one in the reference ordering must appear.
    for (size_t i = pageSize + 1; i < fullReference.size(); ++i)
    {
        EXPECT_EQ(page2Set.count(fullReference[i]), 1u)
            << "visual-media node at reference index " << i << " was skipped after deletion";
    }

    for (const auto& n: page1)
    {
        EXPECT_EQ(page2Set.count(n->nodeHandle()), 0u) << "page-1 handle repeated in page 2+";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group E – excludeSensitive=true on grouped routes (UNION ALL CTE)
//
//  The simple-path Sensitive_* TEST_Fs (fixture ListAllNodesByPageTest) only
//  exercise one route (PHOTO). buildGroupedListAllQuery stitches the
//  buildUpWalkExists predicate into every constituent SELECT, so a bug in a
//  single route's parameter binding would pass the PHOTO-only coverage. The
//  tests below push the sensitive predicate through both PHOTO+VIDEO routes
//  of ALL_VISUAL_MEDIA and through the PDF route of ALL_DOCS.
// ═══════════════════════════════════════════════════════════════════════════

// E1. ALL_VISUAL_MEDIA with excludeSensitive=true must drop the sensitive photo
//     AND the sensitive video — one per UNION route — while keeping every
//     non-sensitive entry.
TEST_F(GroupedListAllNodesByPageTest, AllVisualMedia_ExcludeSensitive_FiltersBothPhotoAndVideo)
{
    const auto all = allMatchesAsSet(MIME_TYPE_ALL_VISUAL_MEDIA,
                                     OrderByClause::DEFAULT_ASC,
                                     /*excludeSensitive=*/false);
    const auto filtered = allMatchesAsSet(MIME_TYPE_ALL_VISUAL_MEDIA,
                                          OrderByClause::DEFAULT_ASC,
                                          /*excludeSensitive=*/true);

    // Photo route: self_sensitive.jpg + under_sens.jpg (ancestor) removed.
    EXPECT_EQ(filtered.count(hSelfSensitive), 0u) << "self_sensitive.jpg leaked";
    EXPECT_EQ(filtered.count(hUnderSens), 0u) << "under_sens.jpg leaked";
    // Video route: video_sensitive.mp4 removed by the grouped EXISTS predicate.
    EXPECT_EQ(filtered.count(hVideoSensitive), 0u)
        << "video_sensitive.mp4 leaked — grouped path may be missing the "
           "sensitivity EXISTS on the VIDEO route";

    // Strict subset with a strictly smaller size: dataset carries ≥ 3 sensitive-
    // excluded entries, so filtered.size() must be less than all.size().
    for (const auto& h: filtered)
        EXPECT_EQ(all.count(h), 1u) << "filtered leaked a handle not in unfiltered set";
    EXPECT_LT(filtered.size(), all.size())
        << "ALL_VISUAL_MEDIA excludeSensitive=true did not remove any node";
}

// E2. Paginated ALL_VISUAL_MEDIA + excludeSensitive=true must equal the
//     searchNodes reference that applies bySensitivity(onlyTrue) — onlyTrue
//     drops sensitive rows in the search filter (counter-intuitive name; see
//     referenceBySearch comment). Cross-checks that the grouped UNION-ALL path
//     produces the same ordered sequence as the canonical searchNodes walker
//     when sensitivity filtering is on.
TEST_F(GroupedListAllNodesByPageTest, AllVisualMedia_ExcludeSensitive_MatchesSearchReference)
{
    constexpr MimeType_t mime = MIME_TYPE_ALL_VISUAL_MEDIA;
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 3},
        {OrderByClause::SIZE_DESC, 4},
        {OrderByClause::MTIME_ASC, 2},
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceBySearch(order, mime, /*excludeSensitive=*/true);
        const auto paged = collectAllByPage(order,
                                            pageSize,
                                            mime,
                                            /*startCursor=*/std::nullopt,
                                            /*excludeSensitive=*/true);
        assertListAllMatchesReference(paged,
                                      reference,
                                      "ALL_VISUAL_MEDIA excludeSensitive order=" +
                                          std::to_string(order));
    }
}

// E3. ALL_DOCS with excludeSensitive=true must drop a sensitive document
//     (report_sensitive.pdf) while keeping every non-sensitive .txt/.pdf/.pptx/
//     .xlsx. Guards against a PDF-route regression where the EXISTS bind
//     slot drifted relative to the DOCUMENT / PRESENTATION / SPREADSHEET routes.
TEST_F(GroupedListAllNodesByPageTest, AllDocs_ExcludeSensitive_FiltersCorrectly)
{
    const auto all = allMatchesAsSet(MIME_TYPE_ALL_DOCS,
                                     OrderByClause::DEFAULT_ASC,
                                     /*excludeSensitive=*/false);
    const auto filtered = allMatchesAsSet(MIME_TYPE_ALL_DOCS,
                                          OrderByClause::DEFAULT_ASC,
                                          /*excludeSensitive=*/true);

    EXPECT_EQ(all.count(hReportSensitive), 1u)
        << "precondition: ALL_DOCS with excludeSensitive=false must surface "
           "report_sensitive.pdf";
    EXPECT_EQ(filtered.count(hReportSensitive), 0u)
        << "report_sensitive.pdf leaked through the ALL_DOCS PDF route";

    for (const auto& h: filtered)
        EXPECT_EQ(all.count(h), 1u) << "filtered leaked a handle not in unfiltered set";
    EXPECT_EQ(all.size(), filtered.size() + 1u)
        << "ALL_DOCS excludeSensitive=true must remove exactly the sensitive .pdf";
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group H — byExcludeLocationHandles (excSeen accumulator, semantic A)
//
//  Reuses the SearchByPageTest fixture (Cloud + Vault + Rubbish photo subtree
//  built by populateDB). The exclude-list is implemented as a recursive
//  excSeen bit walked alongside sensSeen + a final-row `up.h NOT IN excludes`
//  check; the cases below pin both pieces against a single concrete tree.
// ═══════════════════════════════════════════════════════════════════════════

// H1. Exclude an interior folder: every descendant (direct and grand-) must be
//     dropped. Sibling files outside the excluded folder must remain.
TEST_F(ListAllNodesByPageTest, ByExclude_InteriorFolder_DropsWholeSubtree)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{hNormalFolder});

    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got.count(hClean), 0u) << "clean.jpg is a direct child of the excluded folder";
    EXPECT_EQ(got.count(hHead), 0u) << "head.jpg dropped via excluded ancestor";
    EXPECT_EQ(got.count(hSelfSensitive), 0u) << "self_sensitive.jpg dropped via excluded ancestor";
    // Vault content survives — exclude list is independent of include scope.
    EXPECT_EQ(got.count(hVaultFile), 1u);
}

// H2. Exclude the node itself (not a folder): the initial-row excSeen check
//     `(n.nodehandle IN excludes)` must drop the node directly.
TEST_F(ListAllNodesByPageTest, ByExclude_NodeItself_DroppedByInitialCheck)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{hClean});

    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got.count(hClean), 0u) << "n itself in excludes — must be dropped";
    // Other photos under the same folder must remain.
    EXPECT_EQ(got.count(hHead), 1u);
    EXPECT_EQ(got.count(hVaultFile), 1u);
}

// H3. Semantic A: excluding a root drops its entire subtree even when that
//     root is also the only included ancestor. Combination of byLocationHandles
//     and byExcludeLocationHandles with a deliberate handle overlap.
TEST_F(ListAllNodesByPageTest, ByExclude_MatchedRoot_SemanticA_DropsSubtree)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{hVault},
                        /*excludeHandles=*/{hVault});

    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_TRUE(got.empty())
        << "Excluding the only included root must drop the whole subtree (semantic A); "
           "got "
        << got.size() << " nodes";
}

// H4. Default scope (Cloud + Vault) with exclude={hVault} drops Vault content
//     but keeps Cloud — the user-visible answer to "exclude My Backups
//     folder". Independence from include path is the point.
TEST_F(ListAllNodesByPageTest, ByExclude_DefaultScope_VaultExcluded_KeepsCloud)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{hVault});

    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got.count(hVaultFile), 0u) << "Vault subtree must be dropped — root in exclude list";
    EXPECT_EQ(got.count(hClean), 1u) << "Cloud content unaffected by Vault exclusion";
    EXPECT_EQ(got.count(hRubbishFile), 0u)
        << "Rubbish still excluded by default scope (independent of exclude list)";
}

// H5. Empty exclude list is equivalent to the pre-Phase-2 baseline. Pins the
//     "no-exclude template branch" parity — buildUpWalkExists must not emit
//     the excSeen column / IN-list when numExcludes == 0.
TEST_F(ListAllNodesByPageTest, ByExclude_EmptyList_EquivalentToBaseline)
{
    const auto baseline = allMatchesAsSet(MIME_TYPE_PHOTO,
                                          OrderByClause::DEFAULT_ASC,
                                          /*excludeSensitive=*/false);

    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{});
    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got, baseline)
        << "Empty exclude list must produce a result identical to the no-filter baseline";
}

// H6. Multiple exclude handles (up to kListAllMaxExcludes=3): chain accumulates
//     OR over all entries. Excluding both Cloud and Vault folders simultaneously
//     leaves Rubbish-only — pinned via locationScope=
//     LOCATION_CLOUD_DRIVE_VAULT_AND_RUBBISH.
TEST_F(ListAllNodesByPageTest, ByExclude_MultipleHandles_OrAccumulated)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{hFilesRoot, hVault},
                        /*locationScope=*/2 /* CLOUD+VAULT+RUBBISH */);

    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got.count(hClean), 0u);
    EXPECT_EQ(got.count(hVaultFile), 0u);
    EXPECT_EQ(got.count(hRubbishFile), 1u)
        << "Rubbish must remain — neither Cloud nor Vault root excludes it";
}

// H7. DbTable-level rejection: oversize exclude list (> kListAllMaxExcludes=3)
//     short-circuits with ok=false and empty out — same contract as filesRoots.
TEST_F(ListAllNodesByPageTest, DbTable_TooManyExcludes_ReturnsEmpty)
{
    auto* table = dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    ASSERT_NE(table, nullptr);

    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{hClean, hHead, hVault, hRubbish}); // 4 entries

    std::vector<std::pair<NodeHandle, NodeSerialized>> out;
    const bool ok = table->listAllNodesByPage(p,
                                              /*roots=*/{hFilesRoot},
                                              out,
                                              CancelToken{});
    EXPECT_FALSE(ok);
    EXPECT_TRUE(out.empty());
}

// H8. DbTable-level rejection: UNDEF in exclude list short-circuits.
TEST_F(ListAllNodesByPageTest, DbTable_UndefInExcludes_ReturnsEmpty)
{
    auto* table = dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    ASSERT_NE(table, nullptr);

    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{hClean, NodeHandle()});

    std::vector<std::pair<NodeHandle, NodeSerialized>> out;
    const bool ok = table->listAllNodesByPage(p,
                                              /*roots=*/{hFilesRoot},
                                              out,
                                              CancelToken{});
    EXPECT_FALSE(ok);
    EXPECT_TRUE(out.empty());
}

// H9. Pagination across an exclude boundary. Drop hSelfSensitive via exclude;
//     two pages of size 2 must produce no skips/duplicates.
TEST_F(ListAllNodesByPageTest, ByExclude_CursorAcrossExcludeBoundary_NoSkipsNoDuplicates)
{
    // Reference: single full-set call without exclude, then strip hSelfSensitive
    // — same expected output as the paged exclude run.
    std::vector<NodeHandle> reference;
    {
        auto refParams = makeParams(MIME_TYPE_PHOTO,
                                    OrderByClause::DEFAULT_ASC,
                                    /*maxElements=*/0,
                                    /*excludeSensitive=*/false);
        auto refNodes = mClient->mNodeManager.listAllNodesByPage(refParams, CancelToken{});
        for (const auto& n: refNodes)
            if (n->nodeHandle() != hSelfSensitive)
                reference.push_back(n->nodeHandle());
    }

    // Now collect the same set page-by-page with pageSize=2 + exclude.
    std::vector<NodeHandle> paged;
    std::optional<NodeSearchCursorOffset> cursor;
    const size_t maxIters = mMeta.size() + 2;
    size_t i = 0;
    for (; i < maxIters; ++i)
    {
        auto p = makeParams(MIME_TYPE_PHOTO,
                            OrderByClause::DEFAULT_ASC,
                            /*maxElements=*/2,
                            /*excludeSensitive=*/false,
                            cursor,
                            /*explicitAncestors=*/{},
                            /*excludeHandles=*/{hSelfSensitive});
        auto nodes = mClient->mNodeManager.listAllNodesByPage(p, CancelToken{});
        if (nodes.empty())
            break;
        for (const auto& n: nodes)
            paged.push_back(n->nodeHandle());
        const auto* lastNode = nodes.back().get();
        NodeSearchCursorOffset c;
        c.mLastName = lastNode->displayname() ? lastNode->displayname() : "";
        c.mLastHandle = lastNode->nodeHandle().as8byte();
        cursor = c;
    }

    ASSERT_LT(i, maxIters) << "Pagination did not terminate within " << maxIters
                           << " iterations — likely an infinite-pagination regression";

    // Order-preserving equivalence to reference.
    EXPECT_EQ(paged, reference)
        << "Pagination across an excluded row must skip it without losing or duplicating others";
}

// H10. Cache-key separation: same (mimeType, order, hasCursor, excludeSensitive,
//      numRoots) but different numExcludes must produce different prepared
//      statements. Verifies computeListAllCacheId(numExcludes) is wired in.
//      We assert it indirectly: a back-to-back call sequence with then without
//      excludes must succeed (not crash from a stale-stmt parameter mismatch).
TEST_F(ListAllNodesByPageTest, ByExclude_CacheKey_DistinctAcross_NumExcludes)
{
    auto p_with = makeParams(MIME_TYPE_PHOTO,
                             OrderByClause::DEFAULT_ASC,
                             /*maxElements=*/0,
                             /*excludeSensitive=*/false,
                             /*cursor=*/std::nullopt,
                             /*explicitAncestors=*/{},
                             /*excludeHandles=*/{hVault});
    auto p_without = makeParams(MIME_TYPE_PHOTO,
                                OrderByClause::DEFAULT_ASC,
                                /*maxElements=*/0,
                                /*excludeSensitive=*/false,
                                /*cursor=*/std::nullopt,
                                /*explicitAncestors=*/{},
                                /*excludeHandles=*/{});

    const auto a = handlesOf(mClient->mNodeManager.listAllNodesByPage(p_with, CancelToken{}));
    const auto b = handlesOf(mClient->mNodeManager.listAllNodesByPage(p_without, CancelToken{}));
    const auto a2 = handlesOf(mClient->mNodeManager.listAllNodesByPage(p_with, CancelToken{}));
    const auto b2 = handlesOf(mClient->mNodeManager.listAllNodesByPage(p_without, CancelToken{}));
    EXPECT_EQ(a, a2) << "Same params with excludes must be deterministic across repeated calls";
    EXPECT_EQ(b, b2) << "Same params without excludes must be deterministic across repeated calls";
    EXPECT_NE(a, b) << "Differently-shaped statements must not share a prepared stmt cache slot";
    EXPECT_LT(a.size(), b.size())
        << "exclude={vault} must drop at least the vault subtree compared to no exclude";
}

// H11. excludeHandles and excludeSensitive walk independent accumulator bits
//      (excSeen vs sensSeen) in buildUpWalkExists. Pin orthogonality so a
//      future refactor that merges or reorders them gets caught.
//
//      With the fixture's normal_folder subtree:
//        - hClean / hSelfSensitive / hHead under hNormalFolder.
//        - hSelfSensitive carries its own SENS bit; hUnderSens inherits SENS
//          via hSensFolder; hVaultFile is unaffected by either filter.
//      Expected drops with both filters enabled simultaneously:
//        - hClean       → dropped by exclude=normal_folder (clean's ancestor)
//        - hSelfSensitive → dropped by both
//        - hHead        → dropped by exclude (also under normal_folder)
//        - hUnderSens   → dropped by sensSeen (sens_folder ancestor)
//      Surviving: hVaultFile.
TEST_F(ListAllNodesByPageTest, ByExclude_AndExcludeSensitive_AreOrthogonal)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/true,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{hNormalFolder});

    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got.count(hClean), 0u) << "dropped by excludeHandles";
    EXPECT_EQ(got.count(hHead), 0u) << "dropped by excludeHandles";
    EXPECT_EQ(got.count(hSelfSensitive), 0u) << "dropped by both filters";
    EXPECT_EQ(got.count(hUnderSens), 0u) << "dropped by excludeSensitive";
    EXPECT_EQ(got.count(hVaultFile), 1u) << "unaffected by either filter";
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group I — locationScope (LOCATION_*)
//
//  Verifies that resolveListAllRoots maps each enum value to the right
//  rootnode set. SQL plumbing reuses the existing numRoots IN-list machinery
//  (already covered by G1*); these tests pin the NodeManager layer.
// ═══════════════════════════════════════════════════════════════════════════

// I1. LOCATION_CLOUD_DRIVE: only Cloud rootnode is walked; Vault and Rubbish
//     subtree photos must be absent.
TEST_F(ListAllNodesByPageTest, LocationScope_CloudDriveOnly_ExcludesVaultAndRubbish)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{},
                        /*locationScope=*/0 /* LOCATION_CLOUD_DRIVE */);
    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got.count(hClean), 1u);
    EXPECT_EQ(got.count(hVaultFile), 0u);
    EXPECT_EQ(got.count(hRubbishFile), 0u);
}

// I2. LOCATION_CLOUD_DRIVE_VAULT_AND_RUBBISH: all three rootnodes walked.
TEST_F(ListAllNodesByPageTest, LocationScope_AllRootnodes_IncludesRubbish)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{},
                        /*excludeHandles=*/{},
                        /*locationScope=*/2 /* CLOUD+VAULT+RUBBISH */);
    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    EXPECT_EQ(got.count(hClean), 1u);
    EXPECT_EQ(got.count(hVaultFile), 1u);
    EXPECT_EQ(got.count(hRubbishFile), 1u)
        << "Rubbish subtree photos must surface when locationScope == ALL";
}

// I3. explicitAncestors wins over locationScope: when both are set, the list
//     takes precedence and locationScope is silently ignored. Pinned by
//     setting explicitAncestors={Vault} with locationScope=CLOUD_DRIVE_ONLY:
//     the Cloud-only scope must NOT narrow further.
TEST_F(ListAllNodesByPageTest, LocationScope_IgnoredWhenExplicitAncestorsPresent)
{
    auto p = makeParams(MIME_TYPE_PHOTO,
                        OrderByClause::DEFAULT_ASC,
                        /*maxElements=*/0,
                        /*excludeSensitive=*/false,
                        /*cursor=*/std::nullopt,
                        /*explicitAncestors=*/{hVault},
                        /*excludeHandles=*/{},
                        /*locationScope=*/0 /* LOCATION_CLOUD_DRIVE */);
    const auto got = handlesOf(mClient->mNodeManager.listAllNodesByPage(p, CancelToken{}));

    // Vault content must appear despite locationScope saying "Cloud only".
    EXPECT_EQ(got.count(hVaultFile), 1u);
    // Cloud content must NOT appear because explicitAncestors didn't include it.
    EXPECT_EQ(got.count(hClean), 0u);
}

} // anonymous namespace

#endif // USE_SQLITE
