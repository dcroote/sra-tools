/*===========================================================================
*
*                            PUBLIC DOMAIN NOTICE
*               National Center for Biotechnology Information
*
*  This software/database is a "United States Government Work" under the
*  terms of the United States Copyright Act.  It was written as part of
*  the author's official duties as a United States Government employee and
*  thus cannot be copyrighted.  This software/database is freely available
*  to the public for use. The National Library of Medicine and the U.S.
*  Government have not placed any restriction on its use or reproduction.
*
*  Although all reasonable efforts have been taken to ensure the accuracy
*  and reliability of the software and data, the NLM and the U.S.
*  Government do not and cannot warrant the performance or results that
*  may be obtained by using this software or data. The NLM and the U.S.
*  Government disclaim all warranties, express or implied, including
*  warranties of performance, merchantability or fitness for any particular
*  purpose.
*
*  Please cite the author in any work or product based on this material.
*
* ===========================================================================
*
*/

#include <kapp/main.h>
#include <kapp/args.h>
#include <vdb/manager.h>
#include <vdb/database.h>
#include <vdb/table.h>
#include <vdb/cursor.h>
#include <vdb/schema.h>
#include <vdb/vdb-priv.h>
#include <kdb/manager.h>
#include <kdb/meta.h>
#include <kfs/directory.h>
#include <klib/log.h>
#include <klib/out.h>
#include <klib/rc.h>
#include <klib/data-buffer.h>
#include <klib/printf.h>
#include <sra/sradb.h>

#include <assert.h>
#include <inttypes.h>

#include <sysexits.h>
#include <unistd.h>

typedef struct CellData {
    void const *data;
    uint32_t count;
    uint32_t elem_bits;
} CellData;

static CellData cellData(char const *const colName, int const col, int64_t const row, VCursor const *const curs);
static uint64_t rowCount(VCursor const *const curs, int64_t *const first, uint32_t cid);
static uint32_t addColumn( char const *const name
                         , char const *const type
                         , VCursor const *const curs);
static void openCursor(VCursor const *const curs, char const *const name);
static uint8_t *growFilterBuffer(uint8_t *const out_filter, size_t *const out_filter_count, size_t needed);
static void openRow(uint64_t const row, VCursor const *const out);
static void writeRow(int64_t const row
                    , uint32_t const reads
                    , uint8_t const *out_filter
                    , uint32_t const cid
                    , VCursor *const out);
static void commitRow(uint64_t const row, VCursor *const out);
static void closeRow(uint64_t const row, VCursor *const out);
static void commitCursor(VCursor *const out);
static VDBManager *manager();
static Args *getArgs(int argc, char *argv[]);
static char const *getOptArgValue(int opt, Args *const args);
static char const *getParameter(Args *const args);
static int pathType(VDBManager const *mgr, char const *path);
static VTable const *openInputTable(char const *const name
                                   , VDBManager const *const mgr
                                   , char const **schemaType
                                   , VSchema *schema
                                   );
static VTable const *dbOpenTable(char const *const name
                                , char const *const table
                                , VDBManager const *const mgr
                                , char const **schemaType
                                , VSchema *schema
                                );
static void tblSchemaInfo(VTable const *tbl, char const **name, VSchema *schema);
static void dbSchemaInfo(VDatabase const *db, char const **name, VSchema *schema);
static VTable const *openInput(char const *input, VDBManager const *mgr, bool *noDb, char const **schemaType, VSchema *schema);
static void processTables(VTable *const output, VTable const *const input);
static VTable *createOutput( Args *const args , VDBManager *const mgr, bool noDb, char const *schemaType, VSchema const *schema, char const **tempObjectPath);
static VSchema *makeSchema(VDBManager *mgr);

static void OUT_OF_MEMORY()
{
    LogErr(klogFatal, RC(rcExe, rcFile, rcReading, rcMemory, rcExhausted), "OUT OF MEMORY!!!");
    exit(EX_TEMPFAIL);
}

static CellData cellData(char const *const colName, int const col, int64_t const row, VCursor const *const curs)
{
    uint32_t bit_offset = 0;
    CellData result = { 0, 0, 0 };
    rc_t rc = VCursorCellDataDirect(curs, row, col, &result.elem_bits, &result.data, &bit_offset, &result.count);
    if (rc) {
        pLogErr(klogFatal, rc, "Failed to read $(col) at row ($row)", "col=%s,row=%ld", colName, row);
        exit(EX_DATAERR);
    }
    assert(bit_offset == 0);
    assert((result.elem_bits & 0x7) == 0);
    return result;
}

static uint64_t rowCount(VCursor const *const curs, int64_t *const first, uint32_t cid)
{
    uint64_t count = 0;
    rc_t const rc = VCursorIdRange(curs, cid, first, &count);
    assert(rc == 0);
    return count;
}

static uint32_t addColumn( char const *const name
                         , char const *const type
                         , VCursor const *const curs)
{
    uint32_t cid = 0;
    rc_t const rc = VCursorAddColumn(curs, &cid, "(%s)%s", type, name);
    if (rc == 0)
        return cid;

    pLogErr(klogFatal, rc, "Failed to open $(name) column", "name=%s", name);
    exit(EX_NOINPUT);
}

static void openCursor(VCursor const *const curs, char const *const name)
{
    rc_t const rc = VCursorOpen(curs);
    if (rc) {
        pLogErr(klogFatal, rc, "Failed to open $(name) cursor", "name=%s", name);
        exit(EX_NOINPUT);
    }
}

static VTable const *openTable(char const *const name, VDBManager const *const mgr)
{
    VTable const *in = NULL;
    rc_t const rc = VDBManagerOpenTableRead(mgr, &in, NULL, "%s", name);
    if (rc == 0)
        return in;

    LogErr(klogFatal, rc, "can't open input table");
    exit(EX_SOFTWARE);
}

static VTable const *openInputTable(char const *const name, VDBManager const *const mgr, char const **schemaType, VSchema *schema)
{
    VTable const *const in = openTable(name, mgr);
    tblSchemaInfo(in, schemaType, schema);
    return in;
}

static VDatabase const *openDatabase(char const *const input, VDBManager const *const mgr)
{
    VDatabase const *db = NULL;
    rc_t const rc = VDBManagerOpenDBRead(mgr, &db, NULL, "%s", input);
    if (rc == 0)
        return db;
        
    LogErr(klogFatal, rc, "can't open input database");
    exit(EX_SOFTWARE);
}

static void getSchemaInfo(KMetadata const *const meta, char const **type, VSchema *schema)
{
    KMDataNode const *root = NULL;
    KMDataNode const *node = NULL;
    size_t valueLen = 0;
    char *value = NULL;
    {
        rc_t const rc = KMetadataOpenNodeRead(meta, &root, NULL);
        KMetadataRelease(meta);
        if (rc != 0) {
            LogErr(klogFatal, rc, "can't get database metadata");
            exit(EX_SOFTWARE);
        }
    }
    {
        rc_t const rc = KMDataNodeOpenNodeRead(root, &node, "schema");
        KMDataNodeRelease(root);
        if (rc != 0) {
            LogErr(klogFatal, rc, "can't get database schema");
            exit(EX_SOFTWARE);
        }
    }
    {
        extern rc_t KMDataNodeAddr(const KMDataNode *self, const void **addr, size_t *size);
        rc_t const rc = KMDataNodeAddr(node, (const void **)&value, &valueLen);
        if (rc != 0) {
            LogErr(klogFatal, rc, "can't get database schema");
            exit(EX_SOFTWARE);
        }
    }
    {
        rc_t const rc = VSchemaParseText(schema, NULL, value, valueLen);
        if (rc != 0) {
            LogErr(klogFatal, rc, "can't get database schema");
            exit(EX_SOFTWARE);
        }
    }
    {
        char dummy = 0;
        rc_t const rc = KMDataNodeReadAttr(node, "name", &dummy, 0, &valueLen);
        assert(GetRCObject(rc) == (int)rcBuffer && GetRCState(rc) == (int)rcInsufficient);
        if (!(GetRCObject(rc) == (int)rcBuffer && GetRCState(rc) == (int)rcInsufficient)) {
            LogErr(klogFatal, rc, "can't get database schema");
            exit(EX_SOFTWARE);
        }
    }
    value = malloc(valueLen + 1);
    if (value == NULL)
        OUT_OF_MEMORY();
    {
        rc_t const rc = KMDataNodeReadAttr(node, "name", value, valueLen + 1, &valueLen);
        KMDataNodeRelease(node);
        if (rc != 0) {
            LogErr(klogFatal, rc, "can't get database schema");
            exit(EX_SOFTWARE);
        }
    }
    value[valueLen] = '\0';
    *type = value;
    pLogMsg(klogInfo, "Schema type is $(type)", "type=%s", value);
}

static void dbSchemaInfo(VDatabase const *const db, char const **name, VSchema *schema)
{
    KMetadata const *meta = NULL;
    {
        rc_t const rc = VDatabaseOpenMetadataRead(db, &meta);
        if (rc != 0) {
            LogErr(klogFatal, rc, "can't get database metadata");
            exit(EX_SOFTWARE);
        }
    }
    getSchemaInfo(meta, name, schema);
}

static void tblSchemaInfo(VTable const *const tbl, char const **name, VSchema *schema)
{
    KMetadata const *meta = NULL;
    {
        rc_t const rc = VTableOpenMetadataRead(tbl, &meta);
        if (rc != 0) {
            LogErr(klogFatal, rc, "can't get database metadata");
            exit(EX_SOFTWARE);
        }
    }
    getSchemaInfo(meta, name, schema);
}

static VTable const *dbOpenTable(char const *const name
                                , char const *const table
                                , VDBManager const *const mgr
                                , char const **schemaType
                                , VSchema *schema
                                )
{
    VDatabase const *db = openDatabase(name, mgr);
    VTable const *in = NULL;
    rc_t const rc = VDatabaseOpenTableRead(db, &in, "%s", table);
    dbSchemaInfo(db, schemaType, schema);
    VDatabaseRelease(db);
    if (rc == 0)
        return in;

    LogErr(klogFatal, rc, "can't open input table");
    exit(EX_NOINPUT);
}

#define PATH_TYPE_ISA_DATABASE(TYPE) ((TYPE | kptAlias) == (kptDatabase | kptAlias))
#define PATH_TYPE_ISA_TABLE(TYPE) ((TYPE | kptAlias) == (kptTable | kptAlias))

static VDBManager *manager()
{
    VDBManager *mgr = NULL;
    rc_t const rc = VDBManagerMakeUpdate(&mgr, NULL);
    if (rc == 0)
        return mgr;
    
    LogErr(klogFatal, rc, "VDBManagerMake failed!");
    exit(EX_TEMPFAIL);
}

static int pathType(VDBManager const *mgr, char const *path)
{
    KDBManager const *kmgr = 0;
    rc_t rc = VDBManagerOpenKDBManagerRead(mgr, &kmgr);
    if (rc == 0) {
        int const type = KDBManagerPathType(kmgr, "%s", path);
        KDBManagerRelease(kmgr);
        return type;
    }
    LogErr(klogFatal, rc, "VDBManagerOpenKDBManager failed!");
    exit(EX_TEMPFAIL);
}

static void openRow(uint64_t const row, VCursor const *const out)
{
    rc_t const rc = VCursorOpenRow(out);
    if (rc) {
        pLogErr(klogFatal, rc, "Failed to open a new row $(row)", "row=%lu", row);
        exit(EX_IOERR);
    }
}

static void commitRow(uint64_t const row, VCursor *const out)
{
    rc_t const rc = VCursorCommitRow(out);
    if (rc) {
        pLogErr(klogFatal, rc, "Failed to commit row $(row)", "row=%"PRIu64, row);
        exit(EX_IOERR);
    }
}

static void closeRow(uint64_t const row, VCursor *const out)
{
    rc_t const rc = VCursorCloseRow(out);
    if (rc) {
        pLogErr(klogFatal, rc, "Failed to close row $(row)", "row=%"PRIu64, row);
        exit(EX_IOERR);
    }
}

static void commitCursor(VCursor *const out)
{
    rc_t const rc = VCursorCommit(out);
    if (rc) {
        LogErr(klogFatal, rc, "Failed to commit cursor");
        exit(EX_IOERR);
    }
}

static void writeRow(int64_t const row
                    , uint32_t const reads
                    , uint8_t const *out_filter
                    , uint32_t const cid
                    , VCursor *const out)
{
    rc_t const rc = VCursorWrite(out, cid, 8, out_filter, 0, reads);
    if (rc) {
        pLogErr(klogFatal, rc, "Failed to write row $(row)", "row=%"PRIu64, row);
        exit(EX_IOERR);
    }
}

static KDirectory *rootDir()
{
    KDirectory *ndir = NULL;
    rc_t const rc = KDirectoryNativeDir(&ndir);
    if (rc) {
        LogErr(klogFatal, rc, "Can't get a directory!!!");
        exit(EX_SOFTWARE);
    }
    return ndir;
}

static KDirectory *openDirUpdate(char const *const path)
{
    KDirectory *ndir = rootDir();
    KDirectory *result = NULL;
    rc_t const rc = KDirectoryOpenDirUpdate(ndir, &result, false, "%s", path);
    KDirectoryRelease(ndir);
    if (rc) {
        pLogErr(klogFatal, rc, "Can't get directory $(path)", "path=%s", path);
        exit(EX_SOFTWARE);
    }
    return result;
}

static KDirectory const *openDirRead(char const *const path)
{
    KDirectory *ndir = rootDir();
    KDirectory const *result = NULL;
    rc_t const rc = KDirectoryOpenDirRead(ndir, &result, false, "%s", path);
    KDirectoryRelease(ndir);
    if (rc) {
        pLogErr(klogFatal, rc, "Can't get directory $(path)", "path=%s", path);
        exit(EX_SOFTWARE);
    }
    return result;
}

static rc_t copyPhysicalColumn(char const *const to, char const *const from, char const *const localPath)
{
    KDirectory const *const srcdir = openDirRead(from);
    KDirectory *const dstdir = openDirUpdate(to);
    rc_t const rc = KDirectoryCopy(srcdir, dstdir, true, localPath, localPath);

    KDirectoryRelease(srcdir);
    KDirectoryRelease(dstdir);

    return rc;
}

static KMDataNode const *openNodeRead(VTable const *const tbl, char const *const path)
{
    KMetadata const *meta = NULL;
    KMDataNode const *node = NULL;
    rc_t rc = VTableOpenMetadataRead(tbl, &meta);

    if (rc) {
        LogErr(klogFatal, rc, "can't open table metadata!!!");
        exit(EX_SOFTWARE);
    }
    
    rc = KMetadataOpenNodeRead(meta, &node, path);
    KMetadataRelease(meta);
    if (rc) {
        LogErr(klogFatal, rc, "can't get table metadata!!!");
        exit(EX_SOFTWARE);
    }
    
    return node;
}

static KMDataNode *openNodeUpdate(VTable *const tbl, char const *const path)
{
    KMetadata *meta = NULL;
    KMDataNode *node = NULL;
    rc_t rc = VTableOpenMetadataUpdate(tbl, &meta);

    if (rc) {
        LogErr(klogFatal, rc, "can't open table metadata!!!");
        exit(EX_SOFTWARE);
    }
    
    rc = KMetadataOpenNodeUpdate(meta, &node, path);
    KMetadataRelease(meta);
    if (rc) {
        LogErr(klogFatal, rc, "can't get table metadata!!!");
        exit(EX_DATAERR);
    }
    
    return node;
}

static void copyNodeValue(KMDataNode *const dst, KMDataNode const *const src)
{
    extern rc_t KMDataNodeAddr(const KMDataNode *self, const void **addr, size_t *size);
    void const *data = NULL;
    size_t data_size = 0;
    rc_t rc = KMDataNodeAddr(src, &data, &data_size);
    if (rc) {
        LogErr(klogFatal, rc, "can't read metadata");
        exit(EX_DATAERR);
    }

    rc = KMDataNodeWrite(dst, data, data_size);
    KMDataNodeRelease(src);
    KMDataNodeRelease(dst);
    if (rc) {
        LogErr(klogFatal, rc, "can't write metadata");
        exit(EX_DATAERR);
    }
}

static VTable *openUpdate(VDBManager *const mgr, char const *const name, bool const noDb)
{
    VTable *tbl = NULL;

    if (noDb) {
        rc_t const rc = VDBManagerOpenTableUpdate(mgr, &tbl, NULL, "%s", name);
        if (rc) {
            LogErr(klogFatal, rc, "can't open table for update");
            exit(EX_DATAERR);
        }
    }
    else {
        VDatabase *db = NULL;
        rc_t rc = VDBManagerOpenDBUpdate(mgr, &db, NULL, "%s", name);
        if (rc) {
            LogErr(klogFatal, rc, "can't open database for update");
            exit(EX_DATAERR);
        }
        rc = VDatabaseOpenTableUpdate(db, &tbl, "SEQUENCE");
        VDatabaseRelease(db);
        if (rc) {
            LogErr(klogFatal, rc, "can't open table for update");
            exit(EX_DATAERR);
        }
    }
    return tbl;
}

static VTable const *openRead(VDBManager const *const mgr, char const *const name, bool const noDb)
{

    if (noDb) {
        return openTable(name, mgr);
    }
    else {
        VTable const *tbl = NULL;
        VDatabase const *db = NULL;
        rc_t rc = VDBManagerOpenDBRead(mgr, &db, NULL, "%s", name);
        if (rc) {
            LogErr(klogFatal, rc, "can't open database for read");
            exit(EX_DATAERR);
        }
        rc = VDatabaseOpenTableRead(db, &tbl, "SEQUENCE");
        VDatabaseRelease(db);
        if (rc) {
            LogErr(klogFatal, rc, "can't open table for read");
            exit(EX_DATAERR);
        }
        return tbl;
    }
}

static void dropColumn(VTable *const tbl, char const *const name)
{
    rc_t const rc = VTableDropColumn(tbl, "%s", name);
    if (rc && !(GetRCObject(rc) == (int)rcPath && GetRCState(rc) == (int)rcNotFound)) {
        LogErr(klogInfo, rc, "can't drop RD_FILTER column");
        exit(EX_SOFTWARE);
    }
}

static void removeTempDir(char const *const temp)
{
    char *dup = strdup(temp);
    KDirectory *ndir = rootDir();
    rc_t rc;
    
    assert(dup != NULL);
    if (dup == NULL)
        OUT_OF_MEMORY();
    
    /* temp looks like /tmp/mkf.XXXXXX/out
     * so remove last path element
     */
    rc = KDirectoryResolvePath(ndir, true, dup, strlen(temp), "%s/../", temp);
    if (rc) {
        LogErr(klogFatal, rc, "can't get temp object directory");
        exit(EX_DATAERR);
    }
    
    rc = KDirectoryRemove(ndir, true, "%s", dup);
    KDirectoryRelease(ndir);
    if (GetRCState(rc) == (int)rcBusy && GetRCObject(rc) == (int)rcPath) {
        pLogErr(klogWarn, rc, "failed to delete temp object directory $(path), remove it manually", "path=%s", dup);
    }
    else if (rc) {
        LogErr(klogFatal, rc, "failed to delete temp object directory");
        exit(EX_DATAERR);
    }
    else {
        pLogMsg(klogInfo, "Deleted temp object directory $(temp)", "temp=%s", dup);
    }
    free(dup);
}
