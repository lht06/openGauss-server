/* -------------------------------------------------------------------------
 *
 * xml.c
 *	  XML data type support.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/xml.c
 *
 * -------------------------------------------------------------------------
 */

/*
 * Generally, XML type support is only available when libxml use was
 * configured during the build.  But even if that is not done, the
 * type and all the functions are available, but most of them will
 * fail.  For one thing, this avoids having to manage variant catalog
 * installations.  But it also has nice effects such as that you can
 * dump a database containing XML type data even if the server is not
 * linked with libxml.	Thus, make sure xml_out() works even if nothing
 * else does.
 */

/*
 * Notes on memory management:
 *
 * Sometimes libxml allocates global structures in the hope that it can reuse
 * them later on.  This makes it impractical to change the xmlMemSetup
 * functions on-the-fly; that is likely to lead to trying to pfree_ext() chunks
 * allocated with malloc() or vice versa.  Since libxml might be used by
 * loadable modules, eg libperl, our only safe choices are to change the
 * functions at postmaster/backend launch or not at all.  Since we'd rather
 * not activate libxml in sessions that might never use it, the latter choice
 * is the preferred one.  However, for debugging purposes it can be awfully
 * handy to constrain libxml's allocations to be done in a specific palloc
 * context, where they're easy to track.  Therefore there is code here that
 * can be enabled in debug builds to redirect libxml's allocations into a
 * special context LibxmlContext.  It's not recommended to turn this on in
 * a production build because of the possibility of bad interactions with
 * external modules.
 */

#include "postgres.h"
#include "knl/knl_variable.h"

#ifdef USE_LIBXML
#include <libxml/chvalid.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlversion.h>
#include <libxml/xmlwriter.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

/*
 * We used to check for xmlStructuredErrorContext via a configure test; but
 * that doesn't work on Windows, so instead use this grottier method of
 * testing the library version number.
 */
#if LIBXML_VERSION >= 20704
#define HAVE_XMLSTRUCTUREDERRORCONTEXT 1
#endif
#endif /* USE_LIBXML */

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/syscache.h"
#include "utils/xml.h"

/* GUC variables */
THR_LOCAL int xmloption;
#ifdef USE_LIBXML

/* random number to identify PgXmlErrorContext */
#define ERRCXT_MAGIC 68275028

struct PgXmlErrorContext {
    int magic;
    /* strictness argument passed to pg_xml_init */
    PgXmlStrictness strictness;
    /* current error status and accumulated message, if any */
    bool err_occurred;
    StringInfoData err_buf;
    /* previous libxml error handling state (saved by pg_xml_init) */
    xmlStructuredErrorFunc saved_errfunc;
    void* saved_errcxt;
    /* previous libxml entity handler (saved by pg_xml_init) */
    xmlExternalEntityLoader saved_entityfunc;
};

static xmlParserInputPtr xmlPgEntityLoader(const char* URL, const char* ID, xmlParserCtxtPtr ctxt);
static void xml_error_handler(void* data, xmlErrorPtr error);
static void xml_ereport_by_code(int level, int sqlcode, const char* msg, int errcode);
static void chopStringInfoNewlines(StringInfo str);
static void appendStringInfoLineSeparator(StringInfo str);

#ifdef USE_LIBXMLCONTEXT

static MemoryContext LibxmlContext = NULL;

static void xml_memory_init(void);
static void* xml_palloc(size_t size);
static void* xml_repalloc(void* ptr, size_t size);
static void xml_pfree_ext(void* ptr);
static char* xml_pstrdup(const char* string);
#endif /* USE_LIBXMLCONTEXT */

static xmlChar* xml_text2xmlChar(text* in);
static int parse_xml_decl(const xmlChar* str, size_t* lenp, xmlChar** version, xmlChar** encoding, int* standalone);
static bool print_xml_decl(StringInfo buf, const xmlChar* version, pg_enc encoding, int standalone);
static xmlDocPtr xml_parse(text* data, XmlOptionType xmloption_arg, bool preserve_whitespace, int encoding,
    bool can_ignore = false, bool try_another_parse_mode = false);
static text* xml_xmlnodetoxmltype(xmlNodePtr cur);
static int xml_xpathobjtoxmlarray(xmlXPathObjectPtr xpathobj, ArrayBuildState** astate, PgXmlErrorContext *xmlerrcxt);
#endif /* USE_LIBXML */

static StringInfo query_to_xml_internal(const char* query, char* tablename, const char* xmlschema, bool nulls,
    bool tableforest, const char* targetns, bool top_level);
static const char* map_sql_table_to_xmlschema(
    TupleDesc tupdesc, Oid relid, bool nulls, bool tableforest, const char* targetns);
static const char* map_sql_schema_to_xmlschema_types(
    Oid nspid, List* relid_list, bool nulls, bool tableforest, const char* targetns);
static const char* map_sql_catalog_to_xmlschema_types(List* nspid_list);
static const char* map_sql_type_to_xml_name(Oid typeoid, int typmod);
static const char* map_sql_typecoll_to_xmlschema_types(List* tupdesc_list);
static const char* map_sql_type_to_xmlschema_type(Oid typeoid, int typmod);
static void SPI_sql_row_to_xmlelement(
    int rownum, StringInfo result, char* tablename, bool nulls, bool tableforest, const char* targetns, bool top_level);
// Empty string to NULL
#define DEF_TARGETNS ""

#ifdef USE_LIBXML
#define NO_XML_SUPPORT()
#else
#define NO_XML_SUPPORT()                         \
    ereport(ERROR,                               \
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
            errmsg("unsupported XML feature"),   \
            errdetail("This functionality requires the server to be built with libxml support.")))
#endif

/* from SQL/XML:2008 section 4.9 */
#define NAMESPACE_XSD "http://www.w3.org/2001/XMLSchema"
#define NAMESPACE_XSI "http://www.w3.org/2001/XMLSchema-instance"
#define NAMESPACE_SQLXML "http://standards.iso.org/iso/9075/2003/sqlxml"

#ifdef USE_LIBXML

static int xmlChar_to_encoding(const xmlChar* encoding_name)
{
    int encoding = pg_char_to_encoding((const char*)encoding_name);

    if (encoding < 0)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("invalid encoding name \"%s\"", (const char*)encoding_name)));
    return encoding;
}
#endif

/*
 * xml_in uses a plain C string to VARDATA conversion, so for the time being
 * we use the conversion function for the text datatype.
 *
 * This is only acceptable so long as xmltype and text use the same
 * representation.
 */
Datum xml_in(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    char* s = PG_GETARG_CSTRING(0);
    xmltype* vardata = NULL;
    xmlDocPtr doc;

    vardata = (xmltype*)cstring_to_text(s);

    /*
     * Parse the data to check if it is well-formed XML data.  Assume that
     * ERROR occurred if parsing failed.
     */
    doc = xml_parse(vardata, (XmlOptionType)xmloption, true, GetDatabaseEncoding(), fcinfo->can_ignore, true);
    xmlFreeDoc(doc);

    PG_RETURN_XML_P(vardata);
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

#define PG_XML_DEFAULT_VERSION "1.0"

/*
 * xml_out_internal uses a plain VARDATA to C string conversion, so for the
 * time being we use the conversion function for the text datatype.
 *
 * This is only acceptable so long as xmltype and text use the same
 * representation.
 */
static char* xml_out_internal(xmltype* x, pg_enc target_encoding)
{
    char* str = text_to_cstring((text*)x);

#ifdef USE_LIBXML
    size_t len = strlen(str);
    xmlChar* version = NULL;
    int standalone;
    int res_code;

    if ((res_code = parse_xml_decl((xmlChar*)str, &len, &version, NULL, &standalone)) == 0) {
        StringInfoData buf;

        initStringInfo(&buf);

        if (!print_xml_decl(&buf, version, target_encoding, standalone)) {
            /*
             * If we are not going to produce an XML declaration, eat a single
             * newline in the original string to prevent empty first lines in
             * the output.
             */
            if (*(str + len) == '\n')
                len += 1;
        }
        appendStringInfoString(&buf, str + len);

        pfree_ext(str);

        return buf.data;
    }

    xml_ereport_by_code(
        WARNING, ERRCODE_INVALID_XML_CONTENT, "could not parse XML declaration in stored value", res_code);
#endif
    return str;
}

Datum xml_out(PG_FUNCTION_ARGS)
{
    xmltype* x = PG_GETARG_XML_P(0);

    /*
     * xml_out removes the encoding property in all cases.	This is because we
     * cannot control from here whether the datum will be converted to a
     * different client encoding, so we'd do more harm than good by including
     * it.
     */
    PG_RETURN_CSTRING(xml_out_internal(x, (pg_enc)0));
}

Datum xml_recv(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
    xmltype* result = NULL;
    char* str = NULL;
    char* newstr = NULL;
    int nbytes;
    xmlDocPtr doc;
    xmlChar* encodingStr = NULL;
    int encoding;

    /*
     * Read the data in raw format. We don't know yet what the encoding is, as
     * that information is embedded in the xml declaration; so we have to
     * parse that before converting to server encoding.
     */
    nbytes = buf->len - buf->cursor;
    str = (char*)pq_getmsgbytes(buf, nbytes);

    /*
     * We need a null-terminated string to pass to parse_xml_decl().  Rather
     * than make a separate copy, make the temporary result one byte bigger
     * than it needs to be.
     */
    result = (xmltype*)palloc(nbytes + 1 + VARHDRSZ);
    SET_VARSIZE(result, nbytes + VARHDRSZ);
    int rc = memcpy_s(VARDATA(result), nbytes, str, nbytes);
    securec_check(rc, "\0", "\0");
    str = VARDATA(result);
    str[nbytes] = '\0';

    int res_code = parse_xml_decl((const xmlChar*)str, NULL, NULL, &encodingStr, NULL);
    if (res_code != 0)
        xml_ereport_by_code(
            ERROR, ERRCODE_INVALID_XML_CONTENT, "invalid XML content: invalid XML declaration", res_code);

    /*
     * If encoding wasn't explicitly specified in the XML header, treat it as
     * UTF-8, as that's the default in XML. This is different from xml_in(),
     * where the input has to go through the normal client to server encoding
     * conversion.
     */
    encoding = encodingStr ? xmlChar_to_encoding(encodingStr) : PG_UTF8;

    /*
     * Parse the data to check if it is well-formed XML data.  Assume that
     * xml_parse will throw ERROR if not.
     */
    doc = xml_parse(result, (XmlOptionType)xmloption, true, encoding);
    xmlFreeDoc(doc);

    /* Now that we know what we're dealing with, convert to server encoding */
    newstr = (char*)pg_do_encoding_conversion((unsigned char*)str, nbytes, encoding, GetDatabaseEncoding());

    if (newstr != str) {
        pfree_ext(result);
        result = (xmltype*)cstring_to_text(newstr);
        pfree_ext(newstr);
    }

    PG_RETURN_XML_P(result);
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xml_send(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    xmltype* x = PG_GETARG_XML_P(0);
    char* outval = NULL;
    StringInfoData buf;

    /*
     * xml_out_internal doesn't convert the encoding, it just prints the right
     * declaration. pq_sendtext will do the conversion.
     */
    outval = xml_out_internal(x, (pg_enc)pg_get_client_encoding());

    pq_begintypsend(&buf);
    pq_sendtext(&buf, outval, strlen(outval));
    pfree_ext(outval);
    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

#ifdef USE_LIBXML
static void appendStringInfoText(StringInfo str, const text* t)
{
    appendBinaryStringInfo(str, VARDATA(t), VARSIZE(t) - VARHDRSZ);
}
#endif

xmltype* stringinfo_to_xmltype(StringInfo buf)
{
    return (xmltype*)cstring_to_text_with_len(buf->data, buf->len);
}

static xmltype* cstring_to_xmltype(const char* string)
{
    return (xmltype*)cstring_to_text(string);
}

#ifdef USE_LIBXML
static xmltype* xmlBuffer_to_xmltype(xmlBufferPtr buf)
{
    return (xmltype*)cstring_to_text_with_len((const char*)xmlBufferContent(buf), xmlBufferLength(buf));
}
#endif

Datum xmlcomment(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    text* arg = PG_GETARG_TEXT_P(0);
    char* argdata = VARDATA(arg);
    int len = VARSIZE(arg) - VARHDRSZ;
    StringInfoData buf;
    int i;

    /* check for "--" in string or "-" at the end */
    for (i = 1; i < len; i++) {
        if (argdata[i] == '-' && argdata[i - 1] == '-')
            ereport(ERROR, (errcode(ERRCODE_INVALID_XML_COMMENT), errmsg("invalid XML comment")));
    }
    if (len > 0 && argdata[len - 1] == '-')
        ereport(ERROR, (errcode(ERRCODE_INVALID_XML_COMMENT), errmsg("invalid XML comment")));

    initStringInfo(&buf);
    appendStringInfo(&buf, "<!--");
    appendStringInfoText(&buf, arg);
    appendStringInfo(&buf, "-->");

    PG_RETURN_XML_P(stringinfo_to_xmltype(&buf));
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

/*
 * Xmlconcat needs to merge the notations and unparsed entities
 * of the argument values.	Not very important in practice, though.
 */
xmltype* xmlconcat(List* args)
{
#ifdef USE_LIBXML
    int global_standalone = 1;
    xmlChar* global_version = NULL;
    bool global_version_no_value = false;
    StringInfoData buf;
    ListCell* v = NULL;

    initStringInfo(&buf);
    foreach (v, args) {
        xmltype* x = DatumGetXmlP(PointerGetDatum(lfirst(v)));
        size_t len;
        xmlChar* version = NULL;
        int standalone;
        char* str = NULL;

        len = VARSIZE(x) - VARHDRSZ;
        str = text_to_cstring((text*)x);

        int res_code = parse_xml_decl((xmlChar*)str, &len, &version, NULL, &standalone);
        if (res_code != 0)
            xml_ereport_by_code(
                ERROR, ERRCODE_INVALID_XML_CONTENT, "invalid XML content: invalid XML declaration", res_code);

        if (standalone == 0 && global_standalone == 1)
            global_standalone = 0;
        if (standalone < 0)
            global_standalone = -1;

        if (version == NULL)
            global_version_no_value = true;
        else if (global_version == NULL)
            global_version = version;
        else if (xmlStrcmp(version, global_version) != 0)
            global_version_no_value = true;

        appendStringInfoString(&buf, str + len);
        pfree_ext(str);
    }

    if (!global_version_no_value || global_standalone >= 0) {
        StringInfoData buf2;

        initStringInfo(&buf2);

        print_xml_decl(&buf2, (!global_version_no_value) ? global_version : NULL, (pg_enc)0, global_standalone);

        appendStringInfoString(&buf2, buf.data);
        buf = buf2;
    }

    return stringinfo_to_xmltype(&buf);
#else
    NO_XML_SUPPORT();
    return NULL;
#endif
}

/*
 * XMLAGG support
 */
Datum xmlconcat2(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    if (PG_ARGISNULL(0)) {
        if (PG_ARGISNULL(1))
            PG_RETURN_NULL();
        else
            PG_RETURN_XML_P(PG_GETARG_XML_P(1));
    } else if (PG_ARGISNULL(1))
        PG_RETURN_XML_P(PG_GETARG_XML_P(0));
    else
        PG_RETURN_XML_P(xmlconcat(list_make2(PG_GETARG_XML_P(0), PG_GETARG_XML_P(1))));
}

Datum texttoxml(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    text* data = PG_GETARG_TEXT_P(0);

    PG_RETURN_XML_P(xmlparse(data, (XmlOptionType)xmloption, true));
}

Datum xmltotext(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    xmltype* data = PG_GETARG_XML_P(0);

    /* It's actually binary compatible. */
    PG_RETURN_TEXT_P((text*)data);
}

text* xmltotext_with_xmloption(xmltype* data, XmlOptionType xmloption_arg)
{
    if (xmloption_arg == XMLOPTION_DOCUMENT && !xml_is_document(data))
        ereport(ERROR, (errcode(ERRCODE_NOT_AN_XML_DOCUMENT), errmsg("not an XML document")));

    /* It's actually binary compatible, save for the above check. */
    return (text*)data;
}

xmltype* xmlelement(XmlExprState* xmlExpr, ExprContext* econtext)
{
#ifdef USE_LIBXML
    XmlExpr* xexpr = (XmlExpr*)xmlExpr->xprstate.expr;
    xmltype* result = NULL;
    List* named_arg_strings = NIL;
    List* arg_strings = NIL;
    ListCell* arg = NULL;
    ListCell* narg = NULL;
    PgXmlErrorContext* xmlerrcxt = NULL;
    volatile xmlBufferPtr buf = NULL;
    volatile xmlTextWriterPtr writer = NULL;

    /*
     * We first evaluate all the arguments, then start up libxml and create
     * the result.	This avoids issues if one of the arguments involves a call
     * to some other function or subsystem that wants to use libxml on its own
     * terms.
     */
    named_arg_strings = NIL;
    foreach (arg, xmlExpr->named_args) {
        ExprState* e = (ExprState*)lfirst(arg);
        Datum value;
        bool isnull = false;
        char* str = NULL;

        value = ExecEvalExpr(e, econtext, &isnull, NULL);
        if (isnull)
            str = NULL;
        else
            str = map_sql_value_to_xml_value(value, exprType((Node*)e->expr), false);
        named_arg_strings = lappend(named_arg_strings, str);
    }

    arg_strings = NIL;
    foreach (arg, xmlExpr->args) {
        ExprState* e = (ExprState*)lfirst(arg);
        Datum value;
        bool isnull = false;
        char* str = NULL;

        value = ExecEvalExpr(e, econtext, &isnull, NULL);
        /* here we can just forget NULL elements immediately */
        if (!isnull) {
            str = map_sql_value_to_xml_value(value, exprType((Node*)e->expr), true);
            arg_strings = lappend(arg_strings, str);
        }
    }

    /* now safe to run libxml */
    xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

    PG_TRY();
    {
        buf = xmlBufferCreate();
        if (buf == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate xmlBuffer");
        writer = xmlNewTextWriterMemory(buf, 0);
        if (writer == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate xmlTextWriter");

        xmlTextWriterStartElement(writer, (xmlChar*)xexpr->name);

        forboth(arg, named_arg_strings, narg, xexpr->arg_names)
        {
            char* str = (char*)lfirst(arg);
            char* argname = strVal(lfirst(narg));

            if (str != NULL)
                xmlTextWriterWriteAttribute(writer, (xmlChar*)argname, (xmlChar*)str);
        }

        foreach (arg, arg_strings) {
            char* str = (char*)lfirst(arg);

            xmlTextWriterWriteRaw(writer, (xmlChar*)str);
        }

        xmlTextWriterEndElement(writer);

        /* we MUST do this now to flush data out to the buffer ... */
        xmlFreeTextWriter(writer);
        writer = NULL;

        result = xmlBuffer_to_xmltype(buf);
    }
    PG_CATCH();
    {
        if (writer)
            xmlFreeTextWriter(writer);
        if (buf)
            xmlBufferFree(buf);

        pg_xml_done(xmlerrcxt, true);

        PG_RE_THROW();
    }
    PG_END_TRY();

    xmlBufferFree(buf);

    pg_xml_done(xmlerrcxt, false);

    return result;
#else
    NO_XML_SUPPORT();
    return NULL;
#endif
}

xmltype* xmlelementByFlatten(XmlExpr *xexpr,
		   Datum *named_argvalue, bool *named_argnull,
		   Datum *argvalue, bool *argnull)
{
#ifdef USE_LIBXML
    xmltype* result = NULL;
    List* named_arg_strings = NIL;
    List* arg_strings = NIL;
    int i;
    ListCell* arg = NULL;
    ListCell* narg = NULL;
    PgXmlErrorContext* xmlerrcxt = NULL;
    volatile xmlBufferPtr buf = NULL;
    volatile xmlTextWriterPtr writer = NULL;

    /*
	 * All arguments are already evaluated, and their values are passed in the
	 * named_argvalue/named_argnull or argvalue/argnull arrays.  This avoids
	 * issues if one of the arguments involves a call to some other function
	 * or subsystem that wants to use libxml on its own terms.  We examine the
	 * original XmlExpr to identify the numbers and types of the arguments.
 	 */
    named_arg_strings = NIL;
    i = 0;
    foreach(arg, xexpr->named_args)
    {
        Expr *e = (Expr *) lfirst(arg);
        char* str = NULL;

        if (named_argnull[i])
            str = NULL;
        else
            str = map_sql_value_to_xml_value(named_argvalue[i], exprType((Node *)e), false);
        named_arg_strings = lappend(named_arg_strings, str);
        i++;
    }

    i = 0;
    arg_strings = NIL;
    i = 0;
    foreach(arg, xexpr->args)
    {
        Expr *e = (Expr *) lfirst(arg);
        char* str = NULL;

        /* here we can just forget NULL elements immediately */
        if (!argnull[i])
        {
            str = map_sql_value_to_xml_value(argvalue[i], exprType((Node *)e), true);
            arg_strings = lappend(arg_strings, str);
        }
        i++;
    }

    xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

    PG_TRY();
    {
        buf = xmlBufferCreate();
        if (buf == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate xmlBuffer");
        writer = xmlNewTextWriterMemory(buf, 0);
        if (writer == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate xmlTextWriter");

        xmlTextWriterStartElement(writer, (xmlChar*)xexpr->name);

        forboth(arg, named_arg_strings, narg, xexpr->arg_names)
        {
            char* str = (char*)lfirst(arg);
            char* argname = strVal(lfirst(narg));

            if (str != NULL)
                xmlTextWriterWriteAttribute(writer, (xmlChar*)argname, (xmlChar*)str);
        }

        foreach (arg, arg_strings) {
            char* str = (char*)lfirst(arg);

            xmlTextWriterWriteRaw(writer, (xmlChar*)str);
        }

        xmlTextWriterEndElement(writer);

        /* we MUST do this now to flush data out to the buffer ... */
        xmlFreeTextWriter(writer);
        writer = NULL;

        result = xmlBuffer_to_xmltype(buf);
    }
    PG_CATCH();
    {
        if (writer)
            xmlFreeTextWriter(writer);
        if (buf)
            xmlBufferFree(buf);

        pg_xml_done(xmlerrcxt, true);

        PG_RE_THROW();
    }
    PG_END_TRY();

    xmlBufferFree(buf);

    pg_xml_done(xmlerrcxt, false);

    return result;
#else
    NO_XML_SUPPORT();
    return NULL;
#endif
}

xmltype* xmlparse(text* data, XmlOptionType xmloption_arg, bool preserve_whitespace)
{
#ifdef USE_LIBXML
    xmlDocPtr doc;

    doc = xml_parse(data, xmloption_arg, preserve_whitespace, GetDatabaseEncoding());
    xmlFreeDoc(doc);

    return (xmltype*)data;
#else
    NO_XML_SUPPORT();
    return NULL;
#endif
}

xmltype* xmlpi(char* target, text* arg, bool arg_is_null, bool* result_is_null)
{
#ifdef USE_LIBXML
    xmltype* result = NULL;
    StringInfoData buf;

    if (pg_strcasecmp(target, "xml") == 0)
        ereport(ERROR,
            (errcode(ERRCODE_SYNTAX_ERROR), /* really */
                errmsg("invalid XML processing instruction"),
                errdetail("XML processing instruction target name cannot be \"%s\".", target)));

    /*
     * Following the SQL standard, the null check comes after the syntax check
     * above.
     */
    *result_is_null = arg_is_null;
    if (*result_is_null)
        return NULL;

    initStringInfo(&buf);

    appendStringInfo(&buf, "<?%s", target);

    if (arg != NULL) {
        char* string = NULL;

        string = text_to_cstring(arg);
        if (strstr(string, "?>") != NULL)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_XML_PROCESSING_INSTRUCTION),
                    errmsg("invalid XML processing instruction"),
                    errdetail("XML processing instruction cannot contain \"?>\".")));

        appendStringInfoChar(&buf, ' ');
        appendStringInfoString(&buf, string + strspn(string, " "));
        pfree_ext(string);
    }
    appendStringInfoString(&buf, "?>");

    result = stringinfo_to_xmltype(&buf);
    pfree_ext(buf.data);
    return result;
#else
    NO_XML_SUPPORT();
    return NULL;
#endif
}

xmltype* xmlroot(xmltype* data, text* version, int standalone)
{
#ifdef USE_LIBXML
    char* str = NULL;
    size_t len;
    xmlChar* orig_version = NULL;
    int orig_standalone;
    StringInfoData buf;

    len = VARSIZE(data) - VARHDRSZ;
    str = text_to_cstring((text*)data);

    int res_code = parse_xml_decl((xmlChar*)str, &len, &orig_version, NULL, &orig_standalone);
    if (res_code != 0)
        xml_ereport_by_code(
            ERROR, ERRCODE_INVALID_XML_CONTENT, "invalid XML content: invalid XML declaration", res_code);

    if (version != NULL)
        orig_version = xml_text2xmlChar(version);
    else
        orig_version = NULL;

    switch (standalone) {
        case XML_STANDALONE_YES:
            orig_standalone = 1;
            break;
        case XML_STANDALONE_NO:
            orig_standalone = 0;
            break;
        case XML_STANDALONE_NO_VALUE:
            orig_standalone = -1;
            break;
        case XML_STANDALONE_OMITTED:
            /* leave original value */
            break;
        default:
            break;
    }

    initStringInfo(&buf);
    print_xml_decl(&buf, orig_version, (pg_enc)0, orig_standalone);
    appendStringInfoString(&buf, str + len);

    return stringinfo_to_xmltype(&buf);
#else
    NO_XML_SUPPORT();
    return NULL;
#endif
}

/*
 * Validate document (given as string) against DTD (given as external link)
 *
 * This has been removed because it is a security hole: unprivileged users
 * should not be able to use Postgres to fetch arbitrary external files,
 * which unfortunately is exactly what libxml is willing to do with the DTD
 * parameter.
 */
Datum xmlvalidate(PG_FUNCTION_ARGS)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("xmlvalidate is not implemented")));
    return 0;
}

bool xml_is_document(xmltype* arg)
{
#ifdef USE_LIBXML
    bool result = false;
    volatile xmlDocPtr doc = NULL;
    MemoryContext ccxt = CurrentMemoryContext;

    /* We want to catch ereport(INVALID_XML_DOCUMENT) and return false */
    PG_TRY();
    {
        doc = xml_parse((text*)arg, XMLOPTION_DOCUMENT, true, GetDatabaseEncoding());
        result = true;
    }
    PG_CATCH();
    {
        ErrorData* errdata = NULL;
        MemoryContext ecxt;

        ecxt = MemoryContextSwitchTo(ccxt);
        errdata = CopyErrorData();
        if (errdata->sqlerrcode == ERRCODE_INVALID_XML_DOCUMENT) {
            FlushErrorState();
            result = false;
        } else {
            MemoryContextSwitchTo(ecxt);
            PG_RE_THROW();
        }
    }
    PG_END_TRY();

    if (doc)
        xmlFreeDoc(doc);

    return result;
#else  /* not USE_LIBXML */
    NO_XML_SUPPORT();
    return false;
#endif /* not USE_LIBXML */
}

#ifdef USE_LIBXML

/*
 * pg_xml_init_library --- set up for use of libxml
 *
 * This should be called by each function that is about to use libxml
 * facilities but doesn't require error handling.  It initializes libxml
 * and verifies compatibility with the loaded libxml version.  These are
 * once-per-session activities.
 *
 * XmlChar is utf8-char, need make proper tuning (initdb with enc!=utf8 and
 * check)
 */
void pg_xml_init_library(void)
{
    static bool first_time = true;

    if (first_time) {
        /* Stuff we need do only once per session */

        /*
         * Currently, we have no pure UTF-8 support for internals -- check if
         * we can work.
         */
        if (sizeof(char) != sizeof(xmlChar))
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("could not initialize XML library"),
                            errdetail("libxml2 has incompatible char type: sizeof(char)=%d, sizeof(xmlChar)=%d.",
                                      (int)sizeof(char), (int)sizeof(xmlChar))));

#ifdef USE_LIBXMLCONTEXT
        /* Set up libxml's memory allocation our way */
        xml_memory_init();
#endif

        /* Check library compatibility */
        LIBXML_TEST_VERSION;

        first_time = false;
    }
}

/*
 * pg_xml_init --- set up for use of libxml and register an error handler
 *
 * This should be called by each function that is about to use libxml
 * facilities and requires error handling.	It initializes libxml with
 * pg_xml_init_library() and establishes our libxml error handler.
 *
 * strictness determines which errors are reported and which are ignored.
 *
 * Calls to this function MUST be followed by a PG_TRY block that guarantees
 * that pg_xml_done() is called during either normal or error exit.
 *
 * This is exported for use by contrib/xml2, as well as other code that might
 * wish to share use of this module's libxml error handler.
 */
PgXmlErrorContext* pg_xml_init(PgXmlStrictness strictness)
{
    PgXmlErrorContext* errcxt = NULL;
    void* new_errcxt = NULL;

    /* Do one-time setup if needed */
    pg_xml_init_library();

    /* Create error handling context structure */
    errcxt = (PgXmlErrorContext*)palloc(sizeof(PgXmlErrorContext));
    errcxt->magic = ERRCXT_MAGIC;
    errcxt->strictness = strictness;
    errcxt->err_occurred = false;
    initStringInfo(&errcxt->err_buf);

    /*
     * Save original error handler and install ours. libxml originally didn't
     * distinguish between the contexts for generic and for structured error
     * handlers.  If we're using an old libxml version, we must thus save the
     * generic error context, even though we're using a structured error
     * handler.
     */
    errcxt->saved_errfunc = xmlStructuredError;

#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
    errcxt->saved_errcxt = xmlStructuredErrorContext;
#else
    errcxt->saved_errcxt = xmlGenericErrorContext;
#endif

    xmlSetStructuredErrorFunc((void*)errcxt, xml_error_handler);

    /*
     * Verify that xmlSetStructuredErrorFunc set the context variable we
     * expected it to.	If not, the error context pointer we just saved is not
     * the correct thing to restore, and since that leaves us without a way to
     * restore the context in pg_xml_done, we must fail.
     *
     * The only known situation in which this test fails is if we compile with
     * headers from a libxml2 that doesn't track the structured error context
     * separately (< 2.7.4), but at runtime use a version that does, or vice
     * versa.  The libxml2 authors did not treat that change as constituting
     * an ABI break, so the LIBXML_TEST_VERSION test in pg_xml_init_library
     * fails to protect us from this.
     */

#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
    new_errcxt = xmlStructuredErrorContext;
#else
    new_errcxt = xmlGenericErrorContext;
#endif

    if (new_errcxt != (void*)errcxt)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("could not set up XML error handler"),
                errhint("This probably indicates that the version of libxml2"
                        " being used is not compatible with the libxml2"
                        " header files that PostgreSQL was built with.")));

    /*
     * Also, install an entity loader to prevent unwanted fetches of external
     * files and URLs.
     */
    errcxt->saved_entityfunc = xmlGetExternalEntityLoader();
    xmlSetExternalEntityLoader(xmlPgEntityLoader);

    return errcxt;
}

/*
 * pg_xml_done --- restore previous libxml error handling
 *
 * Resets libxml's global error-handling state to what it was before
 * pg_xml_init() was called.
 *
 * This routine verifies that all pending errors have been dealt with
 * (in assert-enabled builds, anyway).
 */
void pg_xml_done(PgXmlErrorContext* errcxt, bool isError)
{
    void* cur_errcxt = NULL;

    /* An assert seems like enough protection here */
    Assert(errcxt->magic == ERRCXT_MAGIC);

    /*
     * In a normal exit, there should be no un-handled libxml errors.  But we
     * shouldn't try to enforce this during error recovery, since the longjmp
     * could have been thrown before xml_ereport had a chance to run.
     */
    Assert(!errcxt->err_occurred || isError);

    /*
     * Check that libxml's global state is correct, warn if not.  This is a
     * real test and not an Assert because it has a higher probability of
     * happening.
     */
#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
    cur_errcxt = xmlStructuredErrorContext;
#else
    cur_errcxt = xmlGenericErrorContext;
#endif

    if (cur_errcxt != (void*)errcxt)
        elog(WARNING, "libxml error handling state is out of sync with xml.c");

    /* Restore the saved handlers */
    xmlSetStructuredErrorFunc(errcxt->saved_errcxt, errcxt->saved_errfunc);
    xmlSetExternalEntityLoader(errcxt->saved_entityfunc);

    /*
     * Mark the struct as invalid, just in case somebody somehow manages to
     * call xml_errorHandler or xml_ereport with it.
     */
    errcxt->magic = 0;

    /* Release memory */
    pfree_ext(errcxt->err_buf.data);
    pfree_ext(errcxt);
}

/*
 * pg_xml_error_occurred() --- test the error flag
 */
bool pg_xml_error_occurred(PgXmlErrorContext* errcxt)
{
    return errcxt->err_occurred;
}

/*
 * SQL/XML allows storing "XML documents" or "XML content".  "XML
 * documents" are specified by the XML specification and are parsed
 * easily by libxml.  "XML content" is specified by SQL/XML as the
 * production "XMLDecl? content".  But libxml can only parse the
 * "content" part, so we have to parse the XML declaration ourselves
 * to complete this.
 */

#define SKIP_XML_SPACE(p)       \
    while (xmlIsBlank_ch(*(p))) \
    (p)++

/* Letter | Digit | '.' | '-' | '_' | ':' | CombiningChar | Extender */
/* Beware of multiple evaluations of argument! */
#define PG_XMLISNAMECHAR(c)                                                                                 \
    (xmlIsBaseChar_ch(c) || xmlIsIdeographicQ(c) || xmlIsDigit_ch(c) || c == '.' || c == '-' || c == '_' || \
        c == ':' || xmlIsCombiningQ(c) || xmlIsExtender_ch(c))

/* pnstrdup, but deal with xmlChar not char; len is measured in xmlChars */
static xmlChar* xml_pnstrdup(const xmlChar* str, size_t len)
{
    xmlChar* result = NULL;

    result = (xmlChar*)palloc((len + 1) * sizeof(xmlChar));
    errno_t rc = memcpy_s(result, (len + 1) * sizeof(xmlChar), str, len * sizeof(xmlChar));
    securec_check(rc, "\0", "\0");
    result[len] = 0;
    return result;
}

/* Ditto, except input is char* */
static xmlChar* xml_charpnstrdup(char* str, size_t len)
{
    xmlChar* result = NULL;

    result = (xmlChar*)palloc((len + 1) * sizeof(xmlChar));
    errno_t rc = memcpy_s(result, (len + 1) * sizeof(xmlChar), str, len);
    securec_check(rc, "\0", "\0");
    result[len] = 0;
    return result;
}

/*
 * str is the null-terminated input string.  Remaining arguments are
 * output arguments; each can be NULL if value is not wanted.
 * version and encoding are returned as locally-palloc'd strings.
 * Result is 0 if OK, an error code if not.
 */
static int parse_xml_decl(const xmlChar* str, size_t* lenp, xmlChar** version, xmlChar** encoding, int* standalone)
{
    const xmlChar* p = NULL;
    const xmlChar* save_p = NULL;
    size_t len;
    int utf8char;
    int utf8len;

    /*
     * Only initialize libxml.	We don't need error handling here, but we do
     * need to make sure libxml is initialized before calling any of its
     * functions.  Note that this is safe (and a no-op) if caller has already
     * done pg_xml_init().
     */
    pg_xml_init_library();

    /* Initialize output arguments to "not present" */
    if (version != NULL)
        *version = NULL;
    if (encoding != NULL)
        *encoding = NULL;
    if (standalone != NULL)
        *standalone = -1;

    p = str;

    if (xmlStrncmp(p, (xmlChar*)"<?xml", 5) != 0)
        goto finished;

    /* if next char is name char, it's a PI like <?xml-stylesheet ...?> */
    utf8len = strlen((const char*)(p + 5));
    utf8char = xmlGetUTF8Char(p + 5, &utf8len);
    if (PG_XMLISNAMECHAR(utf8char))
        goto finished;

    p += 5;

    /* version */
    if (!xmlIsBlank_ch(*(p)))
        return XML_ERR_SPACE_REQUIRED;
    SKIP_XML_SPACE(p);
    if (xmlStrncmp(p, (xmlChar*)"version", 7) != 0)
        return XML_ERR_VERSION_MISSING;
    p += 7;
    SKIP_XML_SPACE(p);
    if (*p != '=')
        return XML_ERR_VERSION_MISSING;
    p += 1;
    SKIP_XML_SPACE(p);

    if (*p == '\'' || *p == '"') {
        const xmlChar* q = NULL;

        q = xmlStrchr(p + 1, *p);
        if (q == NULL)
            return XML_ERR_VERSION_MISSING;

        if (version != NULL)
            *version = xml_pnstrdup(p + 1, q - p - 1);
        p = q + 1;
    } else
        return XML_ERR_VERSION_MISSING;

    /* encoding */
    save_p = p;
    SKIP_XML_SPACE(p);
    if (xmlStrncmp(p, (xmlChar*)"encoding", 8) == 0) {
        if (!xmlIsBlank_ch(*(save_p)))
            return XML_ERR_SPACE_REQUIRED;
        p += 8;
        SKIP_XML_SPACE(p);
        if (*p != '=')
            return XML_ERR_MISSING_ENCODING;
        p += 1;
        SKIP_XML_SPACE(p);

        if (*p == '\'' || *p == '"') {
            const xmlChar* q = NULL;

            q = xmlStrchr(p + 1, *p);
            if (q == NULL)
                return XML_ERR_MISSING_ENCODING;

            if (encoding != NULL)
                *encoding = xml_pnstrdup(p + 1, q - p - 1);
            p = q + 1;
        } else
            return XML_ERR_MISSING_ENCODING;
    } else {
        p = save_p;
    }

    /* standalone */
    save_p = p;
    SKIP_XML_SPACE(p);
    if (xmlStrncmp(p, (xmlChar*)"standalone", 10) == 0) {
        if (!xmlIsBlank_ch(*(save_p)))
            return XML_ERR_SPACE_REQUIRED;
        p += 10;
        SKIP_XML_SPACE(p);
        if (*p != '=')
            return XML_ERR_STANDALONE_VALUE;
        p += 1;
        SKIP_XML_SPACE(p);
        if (xmlStrncmp(p, (xmlChar*)"'yes'", 5) == 0 || xmlStrncmp(p, (xmlChar*)"\"yes\"", 5) == 0) {
            if (standalone != NULL)
                *standalone = 1;
            p += 5;
        } else if (xmlStrncmp(p, (xmlChar*)"'no'", 4) == 0 || xmlStrncmp(p, (xmlChar*)"\"no\"", 4) == 0) {
            if (standalone != NULL)
                *standalone = 0;
            p += 4;
        } else
            return XML_ERR_STANDALONE_VALUE;
    } else {
        p = save_p;
    }

    SKIP_XML_SPACE(p);
    if (xmlStrncmp(p, (xmlChar*)"?>", 2) != 0)
        return XML_ERR_XMLDECL_NOT_FINISHED;
    p += 2;

finished:
    len = p - str;

    for (p = str; p < str + len; p++)
        if (*p > 127)
            return XML_ERR_INVALID_CHAR;

    if (lenp != NULL)
        *lenp = len;

    return XML_ERR_OK;
}

/*
 * Write an XML declaration.  On output, we adjust the XML declaration
 * as follows.	(These rules are the moral equivalent of the clause
 * "Serialization of an XML value" in the SQL standard.)
 *
 * We try to avoid generating an XML declaration if possible.  This is
 * so that you don't get trivial things like xml '<foo/>' resulting in
 * '<?xml version="1.0"?><foo/>', which would surely be annoying.  We
 * must provide a declaration if the standalone property is specified
 * or if we include an encoding declaration.  If we have a
 * declaration, we must specify a version (XML requires this).
 * Otherwise we only make a declaration if the version is not "1.0",
 * which is the default version specified in SQL:2003.
 */
static bool print_xml_decl(StringInfo buf, const xmlChar* version, pg_enc encoding, int standalone)
{
    if ((version && strcmp((const char*)version, PG_XML_DEFAULT_VERSION) != 0) || (encoding && encoding != PG_UTF8) ||
        standalone != -1) {
        appendStringInfoString(buf, "<?xml");

        if (version != NULL)
            appendStringInfo(buf, " version=\"%s\"", version);
        else
            appendStringInfo(buf, " version=\"%s\"", PG_XML_DEFAULT_VERSION);

        if (encoding && encoding != PG_UTF8) {
            /*
             * XXX might be useful to convert this to IANA names (ISO-8859-1
             * instead of LATIN1 etc.); needs field experience
             */
            appendStringInfo(buf, " encoding=\"%s\"", pg_encoding_to_char(encoding));
        }

        if (standalone == 1)
            appendStringInfoString(buf, " standalone=\"yes\"");
        else if (standalone == 0)
            appendStringInfoString(buf, " standalone=\"no\"");
        appendStringInfoString(buf, "?>");

        return true;
    } else
        return false;
}

static xmlDocPtr try_xml_parse_on_document_mode(text* data, bool preserve_whitespace, int encoding)
{
    volatile xmlDocPtr doc = NULL;
    bool is_error_on_document = false;
    PG_TRY();
    {
        doc = xml_parse(data, XMLOPTION_DOCUMENT, preserve_whitespace, encoding, false);
    }
    PG_CATCH();
    {
        is_error_on_document = true;
        FlushErrorState();
    }
    PG_END_TRY();

    if (doc != NULL && !is_error_on_document) {
        return doc;
    }

    if (doc != NULL)
        xmlFreeDoc(doc);

    return NULL;
}

/*
 * Convert a C string to XML internal representation
 *
 * Note: it is caller's responsibility to xmlFreeDoc() the result,
 * else a permanent memory leak will ensue!
 *
 * Maybe libxml2's xmlreader is better? (do not construct DOM,
 * yet do not use SAX - see xmlreader.c)
 */
static xmlDocPtr xml_parse(text* data, XmlOptionType xmloption_arg, bool preserve_whitespace, int encoding,
                           bool can_ignore, bool try_another_parse_mode)
{
    int32 len;
    xmlChar* string = NULL;
    xmlChar* utf8string = NULL;
    PgXmlErrorContext* xmlerrcxt = NULL;
    volatile xmlParserCtxtPtr ctxt = NULL;
    volatile xmlDocPtr doc = NULL;
    volatile bool ignore_fail = false;

    len = VARSIZE(data) - VARHDRSZ; /* will be useful later */
    string = xml_text2xmlChar(data);

    utf8string = pg_do_encoding_conversion(string, len, encoding, PG_UTF8);

    /* Start up libxml and its parser */
    xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_WELLFORMED);

    /* Use a TRY block to ensure we clean up correctly */
    PG_TRY();
    {
        xmlInitParser();

        ctxt = xmlNewParserCtxt();
        if (ctxt == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate parser context");

        if (xmloption_arg == XMLOPTION_DOCUMENT) {
            /*
             * Note, that here we try to apply DTD defaults
             * (XML_PARSE_DTDATTR) according to SQL/XML:2008 GR 10.16.7.d:
             * 'Default values defined by internal DTD are applied'. As for
             * external DTDs, we try to support them too, (see SQL/XML:2008 GR
             * 10.16.7.e)
             */
            doc = xmlCtxtReadDoc(ctxt, utf8string, NULL, "UTF-8",
                                 XML_PARSE_NOENT | XML_PARSE_DTDATTR | (preserve_whitespace ? 0 : XML_PARSE_NOBLANKS));
            if (doc == NULL || xmlerrcxt->err_occurred) {
                xml_ereport(xmlerrcxt, can_ignore ? WARNING : ERROR, ERRCODE_INVALID_XML_DOCUMENT,
                            "invalid XML document");
                /* if invalid content value error is ignorable, report warning and return 'null' */
                ignore_fail = true;
                goto ignorable_error_handle;
            }
        } else {
            int res_code;
            size_t count;
            xmlChar* version = NULL;
            int standalone;

            res_code = parse_xml_decl(utf8string, &count, &version, NULL, &standalone);
            if (res_code != 0) {
                if (can_ignore) {
                    xml_ereport(xmlerrcxt, WARNING, ERRCODE_INVALID_XML_DOCUMENT, "invalid XML document");
                    /* if invalid content value error is ignorable, report warning and return 'null' */
                    ignore_fail = true;
                    goto ignorable_error_handle;
                } else {
                    xml_ereport_by_code(ERROR, ERRCODE_INVALID_XML_CONTENT,
                                        "invalid XML content: invalid XML declaration", res_code);
                }
            }

            doc = xmlNewDoc(version);
            Assert(doc->encoding == NULL);
            doc->encoding = xmlStrdup((const xmlChar*)"UTF-8");
            doc->standalone = standalone;

            res_code = xmlParseBalancedChunkMemory(doc, NULL, NULL, 0, utf8string + count, NULL);
            if (res_code != 0 || xmlerrcxt->err_occurred) {
                /*
                 *  we will try to use use XMLOPTION_DOCUMENT to parse again when we use XMLOPTION_CONTENT failed
                 */
                if (try_another_parse_mode) {
                    if (doc != NULL) {
                        xmlFreeDoc(doc);
                        doc = NULL;
                    }
                    doc = try_xml_parse_on_document_mode(data, preserve_whitespace, encoding);
                    if (doc != NULL) {
                        xmlFreeParserCtxt(ctxt);
                        pg_xml_done(xmlerrcxt, true);
                        return doc;
                    }
                }
                xml_ereport(xmlerrcxt, can_ignore ? WARNING : ERROR, ERRCODE_INVALID_XML_CONTENT,
                            "invalid XML content");
                /* if invalid content value error is ignorable, report warning and return 'null' */
                ignore_fail = true;
                goto ignorable_error_handle;
            }
        }

ignorable_error_handle:
        /* no more code add here */
        ;
    }
    PG_CATCH();
    {
        if (doc != NULL)
            xmlFreeDoc(doc);
        if (ctxt != NULL)
            xmlFreeParserCtxt(ctxt);

        pg_xml_done(xmlerrcxt, true);

        PG_RE_THROW();
    }
    PG_END_TRY();

    if (ignore_fail) {
        if (doc != NULL)
            xmlFreeDoc(doc);
        text *new_data = cstring_to_text("null");
        xmlChar *new_utf8string = pg_do_encoding_conversion(xml_text2xmlChar(new_data),
                                                            VARSIZE(new_data) - VARHDRSZ, encoding, PG_UTF8);
        doc = xmlCtxtReadDoc(
            ctxt, new_utf8string, NULL, "UTF-8",
            XML_PARSE_NOENT | XML_PARSE_DTDATTR | (preserve_whitespace ? 0 : XML_PARSE_NOBLANKS));
    }

    xmlFreeParserCtxt(ctxt);
    pg_xml_done(xmlerrcxt, ignore_fail);
    return doc;
}

/*
 * xmlChar<->text conversions
 */
static xmlChar* xml_text2xmlChar(text* in)
{
    return (xmlChar*)text_to_cstring(in);
}

#ifdef USE_LIBXMLCONTEXT

/*
 * Manage the special context used for all libxml allocations (but only
 * in special debug builds; see notes at top of file)
 */
static void xml_memory_init(void)
{
    /* Create memory context if not there already */
    if (LibxmlContext == NULL)
        LibxmlContext = AllocSetContextCreate(t_thrd.top_mem_cxt,
            "LibxmlContext",
            ALLOCSET_DEFAULT_MINSIZE,
            ALLOCSET_DEFAULT_INITSIZE,
            ALLOCSET_DEFAULT_MAXSIZE);

    /* Re-establish the callbacks even if already set */
    xmlMemSetup(xml_pfree, xml_palloc, xml_repalloc, xml_pstrdup);
}

/*
 * Wrappers for memory management functions
 */
static void* xml_palloc(size_t size)
{
    return MemoryContextAlloc(LibxmlContext, size);
}

static void* xml_repalloc(void* ptr, size_t size)
{
    return repalloc(ptr, size);
}

static void xml_pfree_ext(void* ptr)
{
    /* At least some parts of libxml assume xmlFree(NULL) is allowed */
    if (ptr != NULL)
        pfree_ext(ptr);
}

static char* xml_pstrdup(const char* string)
{
    return MemoryContextStrdup(LibxmlContext, string);
}
#endif /* USE_LIBXMLCONTEXT */

/*
 * xmlPgEntityLoader --- entity loader callback function
 *
 * Silently prevent any external entity URL from being loaded.  We don't want
 * to throw an error, so instead make the entity appear to expand to an empty
 * string.
 *
 * We would prefer to allow loading entities that exist in the system's
 * global XML catalog; but the available libxml2 APIs make that a complex
 * and fragile task.  For now, just shut down all external access.
 */
static xmlParserInputPtr xmlPgEntityLoader(const char* URL, const char* ID, xmlParserCtxtPtr ctxt)
{
    return xmlNewStringInputStream(ctxt, (const xmlChar*)"");
}

/*
 * xml_ereport --- report an XML-related error
 *
 * The "msg" is the SQL-level message; some can be adopted from the SQL/XML
 * standard.  This function adds libxml's native error message, if any, as
 * detail.
 *
 * This is exported for modules that want to share the core libxml error
 * handler.  Note that pg_xml_init() *must* have been called previously.
 */
void xml_ereport(PgXmlErrorContext* errcxt, int level, int sqlcode, const char* msg)
{
    char* detail = NULL;

    /* Defend against someone passing us a bogus context struct */
    if (errcxt->magic != ERRCXT_MAGIC)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_INVALID_XML_ERROR_CONTEXT),
                    errmsg("xml_ereport called with invalid PgXmlErrorContext"))));

    /* Flag that the current libxml error has been reported */
    errcxt->err_occurred = false;

    /* Include detail only if we have some text from libxml */
    if (errcxt->err_buf.len > 0)
        detail = errcxt->err_buf.data;
    else
        detail = NULL;

    ereport(level, (errcode(sqlcode), errmsg_internal("%s", msg), detail ? errdetail_internal("%s", detail) : 0));
}

static void xml_append_errbuf(xmlErrorPtr error, const xmlChar* name, StringInfo errorBuf)
{
    if (error->line > 0)
        appendStringInfo(errorBuf, "line %d: ", error->line);
    if (name != NULL)
        appendStringInfo(errorBuf, "element %s: ", name);
    if (error->message != NULL)
        appendStringInfoString(errorBuf, error->message);
    else
        appendStringInfoString(errorBuf, "(no message provided)");

    return;
}

/*
 * Error handler for libxml errors and warnings
 */
static void xml_error_handler(void* data, xmlErrorPtr error)
{
    PgXmlErrorContext* xmlerrcxt = (PgXmlErrorContext*)data;
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr)error->ctxt;
    xmlParserInputPtr input = (ctxt != NULL) ? ctxt->input : NULL;
    xmlNodePtr node = (xmlNodePtr)error->node;
    const xmlChar* name = (node != NULL && node->type == XML_ELEMENT_NODE) ? node->name : NULL;
    int domain = error->domain;
    int level = error->level;
    StringInfo errorBuf;

    /*
     * Defend against someone passing us a bogus context struct.
     *
     * We force a backend exit if this check fails because longjmp'ing out of
     * libxml would likely render it unsafe to use further.
     */
    if (xmlerrcxt->magic != ERRCXT_MAGIC) {
        ereport(FATAL,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_INVALID_XML_ERROR_CONTEXT),
                    errmsg("xml_errorHandler called with invalid PgXmlErrorContext"))));
    }

    /* ----------
     * Older libxml versions report some errors differently.
     * First, some errors were previously reported as coming from the parser
     * domain but are now reported as coming from the namespace domain.
     * Second, some warnings were upgraded to errors.
     * We attempt to compensate for that here.
     * ----------
     */
    switch (error->code) {
        case XML_WAR_NS_URI:
            level = XML_ERR_ERROR;
            domain = XML_FROM_NAMESPACE;
            break;

        case XML_ERR_NS_DECL_ERROR:
        case XML_WAR_NS_URI_RELATIVE:
        case XML_WAR_NS_COLUMN:
        case XML_NS_ERR_XML_NAMESPACE:
        case XML_NS_ERR_UNDEFINED_NAMESPACE:
        case XML_NS_ERR_QNAME:
        case XML_NS_ERR_ATTRIBUTE_REDEFINED:
        case XML_NS_ERR_EMPTY:
            domain = XML_FROM_NAMESPACE;
            break;
        default:
            break;
    }

    /* Decide whether to act on the error or not */
    switch (domain) {
        case XML_FROM_PARSER:
        case XML_FROM_NONE:
        case XML_FROM_MEMORY:
        case XML_FROM_IO:
            /*
             * Suppress warnings about undeclared entities.  We need to do
             * this to avoid problems due to not loading DTD definitions.
             */
            if (error->code == XML_WAR_UNDECLARED_ENTITY)
                return;

            /* Otherwise, accept error regardless of the parsing purpose */
            break;

        default:
            /* Ignore error if only doing well-formedness check */
            if (xmlerrcxt->strictness == PG_XML_STRICTNESS_WELLFORMED)
                return;
            break;
    }

    /* Prepare error message in errorBuf */
    errorBuf = makeStringInfo();
    xml_append_errbuf(error, name, errorBuf);

    /*
     * Append context information to errorBuf.
     *
     * xmlParserPrintFileContext() uses libxml's "generic" error handler to
     * write the context.  Since we don't want to duplicate libxml
     * functionality here, we set up a generic error handler temporarily.
     *
     * We use appendStringInfo() directly as libxml's generic error handler.
     * This should work because it has essentially the same signature as
     * libxml expects, namely (void *ptr, const char *msg, ...).
     */
    if (input != NULL) {
        xmlGenericErrorFunc errFuncSaved = xmlGenericError;
        void* errCtxSaved = xmlGenericErrorContext;

        xmlSetGenericErrorFunc((void*)errorBuf, (xmlGenericErrorFunc)appendStringInfo);

        /* Add context information to errorBuf */
        appendStringInfoLineSeparator(errorBuf);

        xmlParserPrintFileContext(input);

        /* Restore generic error func */
        xmlSetGenericErrorFunc(errCtxSaved, errFuncSaved);
    }

    /* Get rid of any trailing newlines in errorBuf */
    chopStringInfoNewlines(errorBuf);

    /*
     * Legacy error handling mode.	err_occurred is never set, we just add the
     * message to err_buf.	This mode exists because the xml2 contrib module
     * uses our error-handling infrastructure, but we don't want to change its
     * behaviour since it's deprecated anyway.  This is also why we don't
     * distinguish between notices, warnings and errors here --- the old-style
     * generic error handler wouldn't have done that either.
     */
    if (xmlerrcxt->strictness == PG_XML_STRICTNESS_LEGACY) {
        appendStringInfoLineSeparator(&xmlerrcxt->err_buf);
        appendStringInfoString(&xmlerrcxt->err_buf, errorBuf->data);

        pfree_ext(errorBuf->data);
        pfree_ext(errorBuf);
        return;
    }

    /*
     * We don't want to ereport() here because that'd probably leave libxml in
     * an inconsistent state.  Instead, we remember the error and ereport()
     * from xml_ereport().
     *
     * Warnings and notices can be reported immediately since they won't cause
     * a longjmp() out of libxml.
     */
    if (level >= XML_ERR_ERROR) {
        appendStringInfoLineSeparator(&xmlerrcxt->err_buf);
        appendStringInfoString(&xmlerrcxt->err_buf, errorBuf->data);

        xmlerrcxt->err_occurred = true;
    } else if (level >= XML_ERR_WARNING) {
        ereport(WARNING, (errmsg_internal("%s", errorBuf->data)));
    } else {
        ereport(NOTICE, (errmsg_internal("%s", errorBuf->data)));
    }

    pfree_ext(errorBuf->data);
    pfree_ext(errorBuf);
}

/*
 * Wrapper for "ereport" function for XML-related errors.  The "msg"
 * is the SQL-level message; some can be adopted from the SQL/XML
 * standard.  This function uses "code" to create a textual detail
 * message.  At the moment, we only need to cover those codes that we
 * may raise in this file.
 */
static void xml_ereport_by_code(int level, int sqlcode, const char* msg, int code)
{
    const char* det = NULL;

    switch (code) {
        case XML_ERR_INVALID_CHAR:
            det = gettext_noop("Invalid character value.");
            break;
        case XML_ERR_SPACE_REQUIRED:
            det = gettext_noop("Space required.");
            break;
        case XML_ERR_STANDALONE_VALUE:
            det = gettext_noop("standalone accepts only 'yes' or 'no'.");
            break;
        case XML_ERR_VERSION_MISSING:
            det = gettext_noop("Malformed declaration: missing version.");
            break;
        case XML_ERR_MISSING_ENCODING:
            det = gettext_noop("Missing encoding in text declaration.");
            break;
        case XML_ERR_XMLDECL_NOT_FINISHED:
            det = gettext_noop("Parsing XML declaration: '?>' expected.");
            break;
        default:
            det = gettext_noop("Unrecognized libxml error code: %d.");
            break;
    }

    ereport(level, (errcode(sqlcode), errmsg_internal("%s", msg), errdetail(det, code)));
}

/*
 * Remove all trailing newlines from a StringInfo string
 */
static void chopStringInfoNewlines(StringInfo str)
{
    while (str->len > 0 && str->data[str->len - 1] == '\n')
        str->data[--str->len] = '\0';
}

/*
 * Append a newline after removing any existing trailing newlines
 */
static void appendStringInfoLineSeparator(StringInfo str)
{
    chopStringInfoNewlines(str);
    if (str->len > 0)
        appendStringInfoChar(str, '\n');
}

/*
 * Convert one char in the current server encoding to a Unicode codepoint.
 */
static pg_wchar sqlchar_to_unicode(char* s)
{
    char* utf8string = NULL;
    pg_wchar ret[2]; /* need space for trailing zero */

    utf8string = (char*)pg_do_encoding_conversion((unsigned char*)s, pg_mblen(s), GetDatabaseEncoding(), PG_UTF8);

    pg_encoding_mb2wchar_with_len(PG_UTF8, utf8string, ret, pg_encoding_mblen(PG_UTF8, utf8string));

    if (utf8string != s)
        pfree_ext(utf8string);

    return ret[0];
}

static bool is_valid_xml_namefirst(pg_wchar c)
{
    /* (Letter | '_' | ':') */
    return (xmlIsBaseCharQ(c) || xmlIsIdeographicQ(c) || c == '_' || c == ':');
}

static bool is_valid_xml_namechar(pg_wchar c)
{
    /* Letter | Digit | '.' | '-' | '_' | ':' | CombiningChar | Extender */
    return (xmlIsBaseCharQ(c) || xmlIsIdeographicQ(c) || xmlIsDigitQ(c) || c == '.' || c == '-' || c == '_' ||
            c == ':' || xmlIsCombiningQ(c) || xmlIsExtenderQ(c));
}
#endif /* USE_LIBXML */

/*
 * Map SQL identifier to XML name; see SQL/XML:2008 section 9.1.
 */
char* map_sql_identifier_to_xml_name(char* ident, bool fully_escaped, bool escape_period)
{
#ifdef USE_LIBXML
    StringInfoData buf;
    char* p = NULL;

    /*
     * SQL/XML doesn't make use of this case anywhere, so it's probably a
     * mistake.
     */
    Assert(fully_escaped || !escape_period);

    initStringInfo(&buf);

    for (p = ident; *p; p += pg_mblen(p)) {
        if (*p == ':' && (p == ident || fully_escaped))
            appendStringInfo(&buf, "_x003A_");
        else if (*p == '_' && *(p + 1) == 'x')
            appendStringInfo(&buf, "_x005F_");
        else if (fully_escaped && p == ident && pg_strncasecmp(p, "xml", 3) == 0) {
            if (*p == 'x')
                appendStringInfo(&buf, "_x0078_");
            else
                appendStringInfo(&buf, "_x0058_");
        } else if (escape_period && *p == '.')
            appendStringInfo(&buf, "_x002E_");
        else {
            pg_wchar u = sqlchar_to_unicode(p);

            if ((p == ident) ? !is_valid_xml_namefirst(u) : !is_valid_xml_namechar(u))
                appendStringInfo(&buf, "_x%04X_", (unsigned int)u);
            else
                appendBinaryStringInfo(&buf, p, pg_mblen(p));
        }
    }

    return buf.data;
#else  /* not USE_LIBXML */
    NO_XML_SUPPORT();
    return NULL;
#endif /* not USE_LIBXML */
}

/*
 * Map a Unicode codepoint into the current server encoding.
 */
static char* unicode_to_sqlchar(pg_wchar c)
{
    unsigned char utf8string[5]; /* need room for trailing zero */
    char* result = NULL;
    errno_t rc = 0;

    rc = memset_s(utf8string, sizeof(utf8string), 0, sizeof(utf8string));
    securec_check(rc, "\0", "\0");

    unicode_to_utf8(c, utf8string);

    result = (char*)pg_do_encoding_conversion(
        utf8string, pg_encoding_mblen(PG_UTF8, (char*)utf8string), PG_UTF8, GetDatabaseEncoding());
    /* if pg_do_encoding_conversion didn't strdup, we must */
    if (result == (char*)utf8string)
        result = pstrdup(result);
    return result;
}

/*
 * Map XML name to SQL identifier; see SQL/XML:2008 section 9.3.
 */
char* map_xml_name_to_sql_identifier(char* name)
{
    StringInfoData buf;
    char* p = NULL;

    initStringInfo(&buf);

    for (p = name; *p; p += pg_mblen(p)) {
        if (*p == '_' && *(p + 1) == 'x' && isxdigit((unsigned char)*(p + 2)) && isxdigit((unsigned char)*(p + 3)) &&
            isxdigit((unsigned char)*(p + 4)) && isxdigit((unsigned char)*(p + 5)) && *(p + 6) == '_') {
            unsigned int u;

            errno_t ss_rc = sscanf_s(p + 2, "%X", &u);
            securec_check_for_sscanf_s(ss_rc, 1, "\0", "\0");

            appendStringInfoString(&buf, unicode_to_sqlchar(u));
            p += 6;
        } else
            appendBinaryStringInfo(&buf, p, pg_mblen(p));
    }

    return buf.data;
}

/*
 * Map SQL value to XML value; see SQL/XML:2008 section 9.8.
 *
 * When xml_escape_strings is true, then certain characters in string
 * values are replaced by entity references (&lt; etc.), as specified
 * in SQL/XML:2008 section 9.8 GR 9) a) iii).	This is normally what is
 * wanted.	The false case is mainly useful when the resulting value
 * is used with xmlTextWriterWriteAttribute() to write out an
 * attribute, because that function does the escaping itself.
 */
char* map_sql_value_to_xml_value(Datum value, Oid type, bool xml_escape_strings)
{
    if (type_is_array_domain(type)) {
        ArrayType* array = NULL;
        Oid elmtype;
        int16 elmlen;
        bool elmbyval = false;
        char elmalign;
        int num_elems;
        Datum* elem_values = NULL;
        bool* elem_nulls = NULL;
        StringInfoData buf;
        int i;

        array = DatumGetArrayTypeP(value);
        elmtype = ARR_ELEMTYPE(array);
        get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

        deconstruct_array(array, elmtype, elmlen, elmbyval, elmalign, &elem_values, &elem_nulls, &num_elems);

        initStringInfo(&buf);

        for (i = 0; i < num_elems; i++) {
            if (elem_nulls[i])
                continue;
            appendStringInfoString(&buf, "<element>");
            appendStringInfoString(&buf, map_sql_value_to_xml_value(elem_values[i], elmtype, true));
            appendStringInfoString(&buf, "</element>");
        }

        pfree_ext(elem_values);
        pfree_ext(elem_nulls);

        return buf.data;
    } else {
        Oid typeOut;
        bool isvarlena = false;
        char* str = NULL;

        /*
         * Special XSD formatting for some data types
         */
        switch (type) {
            case BOOLOID:
                if (DatumGetBool(value))
                    return "true";
                else
                    return "false";

            case DATEOID: {
                DateADT date;
                struct pg_tm tm;
                char buf[MAXDATELEN + 1];

                date = DatumGetDateADT(value);
                /* XSD doesn't support infinite values */
                if (DATE_NOT_FINITE(date))
                    ereport(ERROR,
                        (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                            errmsg("date out of range"),
                            errdetail("XML does not support infinite date values.")));
                j2date(date + POSTGRES_EPOCH_JDATE, &(tm.tm_year), &(tm.tm_mon), &(tm.tm_mday));
                EncodeDateOnly(&tm, USE_XSD_DATES, buf);

                return pstrdup(buf);
            }

            case TIMESTAMPOID: {
                Timestamp timestamp;
                struct pg_tm tm;
                fsec_t fsec;
                char buf[MAXDATELEN + 1];

                timestamp = DatumGetTimestamp(value);

                /* XSD doesn't support infinite values */
                if (TIMESTAMP_NOT_FINITE(timestamp))
                    ereport(ERROR,
                        (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                            errmsg("timestamp out of range"),
                            errdetail("XML does not support infinite timestamp values.")));
                else if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, NULL) == 0)
                    EncodeDateTime(&tm, fsec, false, 0, NULL, USE_XSD_DATES, buf);
                else
                    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE), errmsg("timestamp out of range")));

                return pstrdup(buf);
            }

            case TIMESTAMPTZOID: {
                TimestampTz timestamp;
                struct pg_tm tm;
                int tz;
                fsec_t fsec;
                const char* tzn = NULL;
                char buf[MAXDATELEN + 1];

                timestamp = DatumGetTimestamp(value);

                /* XSD doesn't support infinite values */
                if (TIMESTAMP_NOT_FINITE(timestamp))
                    ereport(ERROR,
                        (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                            errmsg("timestamp out of range"),
                            errdetail("XML does not support infinite timestamp values.")));
                else if (timestamp2tm(timestamp, &tz, &tm, &fsec, &tzn, NULL) == 0)
                    EncodeDateTime(&tm, fsec, true, tz, tzn, USE_XSD_DATES, buf);
                else
                    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE), errmsg("timestamp out of range")));

                return pstrdup(buf);
            }

#ifdef USE_LIBXML
            case BYTEAOID: {
                bytea* bstr = DatumGetByteaPP(value);
                PgXmlErrorContext* xmlerrcxt = NULL;
                volatile xmlBufferPtr buf = NULL;
                volatile xmlTextWriterPtr writer = NULL;
                char* result = NULL;

                xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

                PG_TRY();
                {
                    buf = xmlBufferCreate();
                    if (buf == NULL || xmlerrcxt->err_occurred)
                        xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate xmlBuffer");
                    writer = xmlNewTextWriterMemory(buf, 0);
                    if (writer == NULL || xmlerrcxt->err_occurred)
                        xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate xmlTextWriter");

                    if (u_sess->attr.attr_common.xmlbinary == XMLBINARY_BASE64)
                        xmlTextWriterWriteBase64(writer, VARDATA_ANY(bstr), 0, VARSIZE_ANY_EXHDR(bstr));
                    else
                        xmlTextWriterWriteBinHex(writer, VARDATA_ANY(bstr), 0, VARSIZE_ANY_EXHDR(bstr));

                    /* we MUST do this now to flush data out to the buffer */
                    xmlFreeTextWriter(writer);
                    writer = NULL;

                    result = pstrdup((const char*)xmlBufferContent(buf));
                }
                PG_CATCH();
                {
                    if (writer)
                        xmlFreeTextWriter(writer);
                    if (buf)
                        xmlBufferFree(buf);

                    pg_xml_done(xmlerrcxt, true);

                    PG_RE_THROW();
                }
                PG_END_TRY();

                xmlBufferFree(buf);

                pg_xml_done(xmlerrcxt, false);

                return result;
            }
#endif /* USE_LIBXML */
            default:
                break;
        }

        /*
         * otherwise, just use the type's native text representation
         */
        getTypeOutputInfo(type, &typeOut, &isvarlena);
        str = OidOutputFunctionCall(typeOut, value);

        /* ... exactly as-is for XML, and when escaping is not wanted */
        if (type == XMLOID || !xml_escape_strings)
            return str;

        /* otherwise, translate special characters as needed */
        return escape_xml(str);
    }
}

/*
 * Escape characters in text that have special meanings in XML.
 *
 * Returns a palloc'd string.
 *
 * NB: this is intentionally not dependent on libxml.
 */
char* escape_xml(const char* str)
{
    StringInfoData buf;
    const char* p = NULL;
    int charlen = 0;

    initStringInfo(&buf);
    p = str;
    while (*p) {
        charlen = pg_mblen(p);
        switch (*p) {
            case '&':
                appendStringInfoString(&buf, "&amp;");
                break;
            case '<':
                appendStringInfoString(&buf, "&lt;");
                break;
            case '>':
                appendStringInfoString(&buf, "&gt;");
                break;
            case '\r':
                appendStringInfoString(&buf, "&#x0d;");
                break;
            default: {
                for (int i = 0; i < charlen; i++)
                    appendStringInfoCharMacro(&buf, p[i]);
                break;
            }
        }
        p += charlen;
    }
    return buf.data;
}

static char* _SPI_strdup(const char* s)
{
    size_t len = strlen(s) + 1;
    char* ret = (char*)SPI_palloc(len);

    errno_t rc = memcpy_s(ret, len, s, len);
    securec_check(rc, "\0", "\0");
    return ret;
}

/*
 * SQL to XML mapping functions
 *
 * What follows below was at one point intentionally organized so that
 * you can read along in the SQL/XML standard. The functions are
 * mostly split up the way the clauses lay out in the standards
 * document, and the identifiers are also aligned with the standard
 * text.  Unfortunately, SQL/XML:2006 reordered the clauses
 * differently than SQL/XML:2003, so the order below doesn't make much
 * sense anymore.
 *
 * There are many things going on there:
 *
 * There are two kinds of mappings: Mapping SQL data (table contents)
 * to XML documents, and mapping SQL structure (the "schema") to XML
 * Schema.	And there are functions that do both at the same time.
 *
 * Then you can map a database, a schema, or a table, each in both
 * ways.  This breaks down recursively: Mapping a database invokes
 * mapping schemas, which invokes mapping tables, which invokes
 * mapping rows, which invokes mapping columns, although you can't
 * call the last two from the outside.	Because of this, there are a
 * number of xyz_internal() functions which are to be called both from
 * the function manager wrapper and from some upper layer in a
 * recursive call.
 *
 * See the documentation about what the common function arguments
 * nulls, tableforest, and targetns mean.
 *
 * Some style guidelines for XML output: Use double quotes for quoting
 * XML attributes.	Indent XML elements by two spaces, but remember
 * that a lot of code is called recursively at different levels, so
 * it's better not to indent rather than create output that indents
 * and outdents weirdly.  Add newlines to make the output look nice.
 */

/*
 * Visibility of objects for XML mappings; see SQL/XML:2008 section
 * 4.10.8.
 */

/*
 * Given a query, which must return type oid as first column, produce
 * a list of Oids with the query results.
 */
static List* query_to_oid_list(const char* query)
{
    int i;
    List* list = NIL;

    SPI_execute(query, true, 0);

    for (i = 0; i < (int)SPI_processed; i++) {
        Datum oid;
        bool isnull = false;

        oid = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            list = lappend_oid(list, DatumGetObjectId(oid));
    }

    return list;
}

static List* schema_get_xml_visible_tables(Oid nspid)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT oid FROM pg_catalog.pg_class WHERE relnamespace = %u AND relkind IN ('r', 'v', 'm') AND "
        "pg_catalog.has_table_privilege (oid, 'SELECT') ORDER BY relname;",
        nspid);

    return query_to_oid_list(query.data);
}

/*
 * Including the system schemas is probably not useful for a database
 * mapping.
 */
#define XML_VISIBLE_SCHEMAS_EXCLUDE "(nspname ~ '^pg_' OR nspname = 'information_schema')"

#define XML_VISIBLE_SCHEMAS                                                                             \
    "SELECT oid FROM pg_catalog.pg_namespace WHERE pg_catalog.has_schema_privilege (oid, 'USAGE') AND " \
    "NOT " XML_VISIBLE_SCHEMAS_EXCLUDE

static List* database_get_xml_visible_schemas(void)
{
    return query_to_oid_list(XML_VISIBLE_SCHEMAS " ORDER BY nspname;");
}

static List* database_get_xml_visible_tables(void)
{
    /* At the moment there is no order required here. */
    return query_to_oid_list(
        "SELECT oid FROM pg_catalog.pg_class WHERE relkind IN ('r', 'v', 'm') AND pg_catalog.has_table_privilege "
        "(pg_class.oid, 'SELECT') AND relnamespace IN (" XML_VISIBLE_SCHEMAS ");");
}

/*
 * Map SQL table to XML and/or XML Schema document; see SQL/XML:2008
 * section 9.11.
 */

static StringInfo table_to_xml_internal(
    Oid relid, const char* xmlschema, bool nulls, bool tableforest, const char* targetns, bool top_level)
{
    StringInfoData query;

    initStringInfo(&query);
    appendStringInfo(
        &query, "SELECT * FROM %s", DatumGetCString(DirectFunctionCall1(regclassout, ObjectIdGetDatum(relid))));
    return query_to_xml_internal(query.data, get_rel_name(relid), xmlschema, nulls, tableforest, targetns, top_level);
}

Datum table_to_xml(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    Oid relid = PG_GETARG_OID(0);
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    PG_RETURN_XML_P(stringinfo_to_xmltype(table_to_xml_internal(relid, NULL, nulls, tableforest, targetns, true)));
}

Datum query_to_xml(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    char* query = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    PG_RETURN_XML_P(
        stringinfo_to_xmltype(query_to_xml_internal(query, NULL, NULL, nulls, tableforest, targetns, true)));
}

Datum cursor_to_xml(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    int32 count = PG_GETARG_INT32(1);
    bool nulls = PG_GETARG_BOOL(2);
    bool tableforest = PG_GETARG_BOOL(3);
    const char* targetns = NULL;

    StringInfoData result;
    Portal portal;
    int i;
    int rc = 0;

    if (PG_ARGISNULL(4))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(4));

    initStringInfo(&result);

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    portal = SPI_cursor_find(name);
    if (portal == NULL)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_CURSOR), errmsg("cursor \"%s\" does not exist", name)));

    SPI_cursor_fetch(portal, true, count);
    for (i = 0; i < (int)SPI_processed; i++)
        SPI_sql_row_to_xmlelement(i, &result, NULL, nulls, tableforest, targetns, true);

    SPI_STACK_LOG("finish", portal->sourceText, NULL);
    SPI_finish();

    PG_RETURN_XML_P(stringinfo_to_xmltype(&result));
}

/*
 * Write the start tag of the root element of a data mapping.
 *
 * top_level means that this is the very top level of the eventual
 * output.	For example, when the user calls table_to_xml, then a call
 * with a table name to this function is the top level.  When the user
 * calls database_to_xml, then a call with a schema name to this
 * function is not the top level.  If top_level is false, then the XML
 * namespace declarations are omitted, because they supposedly already
 * appeared earlier in the output.	Repeating them is not wrong, but
 * it looks ugly.
 */
static void xmldata_root_element_start(
    StringInfo result, const char* eltname, const char* xmlschema, const char* targetns, bool top_level)
{
    /* This isn't really wrong but currently makes no sense. */
    Assert(top_level || !xmlschema);

    appendStringInfo(result, "<%s", eltname);
    if (top_level) {
        appendStringInfoString(result, " xmlns:xsi=\"" NAMESPACE_XSI "\"");
        if (strlen(targetns) > 0)
            appendStringInfo(result, " xmlns=\"%s\"", targetns);
    }
    if (xmlschema != NULL) {
        if (strlen(targetns) > 0)
            appendStringInfo(result, " xsi:schemaLocation=\"%s #\"", targetns);
        else
            appendStringInfo(result, " xsi:noNamespaceSchemaLocation=\"#\"");
    }
    appendStringInfo(result, ">\n\n");
}

static void xmldata_root_element_end(StringInfo result, const char* eltname)
{
    appendStringInfo(result, "</%s>\n", eltname);
}

static StringInfo query_to_xml_internal(const char* query, char* tablename, const char* xmlschema, bool nulls,
    bool tableforest, const char* targetns, bool top_level)
{
    StringInfo result;
    char* xmltn = NULL;
    int i;
    int rc = 0;

    if (tablename != NULL)
        xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
    else
        xmltn = "table";

    result = makeStringInfo();

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("invalid query")));

    if (!tableforest)
        xmldata_root_element_start(result, xmltn, xmlschema, targetns, top_level);

    if (xmlschema != NULL)
        appendStringInfo(result, "%s\n\n", xmlschema);

    for (i = 0; i < (int)SPI_processed; i++)
        SPI_sql_row_to_xmlelement(i, result, tablename, nulls, tableforest, targetns, top_level);

    if (!tableforest)
        xmldata_root_element_end(result, xmltn);

    SPI_STACK_LOG("finish", query, NULL);
    SPI_finish();

    return result;
}

Datum table_to_xmlschema(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    Oid relid = PG_GETARG_OID(0);
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;
    const char* result = NULL;
    Relation rel;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    rel = heap_open(relid, AccessShareLock);
    result = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls, tableforest, targetns);
    heap_close(rel, NoLock);

    PG_RETURN_XML_P(cstring_to_xmltype(result));
}

Datum query_to_xmlschema(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    char* query = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;
    const char* result = NULL;
    SPIPlanPtr plan;
    Portal portal;
    int rc = 0;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
        ereport(ERROR,
            (errmodule(MOD_OPT), (errcode(ERRCODE_SPI_PREPARE_FAILURE), errmsg("SPI_prepare(\"%s\") failed", query))));

    if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CURSOR_OPEN_FAILURE), errmsg("SPI_cursor_open(\"%s\") failed", query))));

    result = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc, InvalidOid, nulls, tableforest, targetns));
    SPI_cursor_close(portal);
    SPI_STACK_LOG("finish", portal->sourceText, NULL);
    SPI_finish();

    PG_RETURN_XML_P(cstring_to_xmltype(result));
}

Datum cursor_to_xmlschema(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;
    const char* xmlschema = NULL;
    Portal portal;
    int rc = 0;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    portal = SPI_cursor_find(name);
    if (portal == NULL)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_CURSOR), errmsg("cursor \"%s\" does not exist", name)));

    xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc, InvalidOid, nulls, tableforest, targetns));
    SPI_STACK_LOG("finish", portal->sourceText, NULL);
    SPI_finish();

    PG_RETURN_XML_P(cstring_to_xmltype(xmlschema));
}

Datum table_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    Oid relid = PG_GETARG_OID(0);
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;
    Relation rel;
    const char* xmlschema = NULL;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    rel = heap_open(relid, AccessShareLock);
    xmlschema = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls, tableforest, targetns);
    heap_close(rel, NoLock);

    PG_RETURN_XML_P(stringinfo_to_xmltype(table_to_xml_internal(relid, xmlschema, nulls, tableforest, targetns, true)));
}

Datum query_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    char* query = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;

    const char* xmlschema = NULL;
    SPIPlanPtr plan;
    Portal portal;
    int rc = 0;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
        ereport(ERROR,
            (errmodule(MOD_OPT), (errcode(ERRCODE_SPI_PREPARE_FAILURE), errmsg("SPI_prepare(\"%s\") failed", query))));

    if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CURSOR_OPEN_FAILURE), errmsg("SPI_cursor_open(\"%s\") failed", query))));

    xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc, InvalidOid, nulls, tableforest, targetns));
    SPI_cursor_close(portal);
    SPI_STACK_LOG("finish", portal->sourceText, NULL);
    SPI_finish();

    PG_RETURN_XML_P(
        stringinfo_to_xmltype(query_to_xml_internal(query, NULL, xmlschema, nulls, tableforest, targetns, true)));
}

/*
 * Map SQL schema to XML and/or XML Schema document; see SQL/XML:2008
 * sections 9.13, 9.14.
 */

static StringInfo schema_to_xml_internal(
    Oid nspid, const char* xmlschema, bool nulls, bool tableforest, const char* targetns, bool top_level)
{
    StringInfo result;
    char* xmlsn = NULL;
    List* relid_list = NIL;
    ListCell* cell = NULL;
    int rc = 0;

    xmlsn = map_sql_identifier_to_xml_name(get_namespace_name(nspid), true, false);
    result = makeStringInfo();

    xmldata_root_element_start(result, xmlsn, xmlschema, targetns, top_level);

    if (xmlschema != NULL)
        appendStringInfo(result, "%s\n\n", xmlschema);

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    relid_list = schema_get_xml_visible_tables(nspid);

    SPI_STACK_LOG("push", NULL, NULL);
    SPI_push();

    foreach (cell, relid_list) {
        Oid relid = lfirst_oid(cell);
        StringInfo subres;

        subres = table_to_xml_internal(relid, NULL, nulls, tableforest, targetns, false);

        appendStringInfoString(result, subres->data);
        appendStringInfoChar(result, '\n');
    }

    SPI_STACK_LOG("pop", NULL, NULL);
    SPI_pop();
    SPI_STACK_LOG("finish", NULL, NULL);
    SPI_finish();

    xmldata_root_element_end(result, xmlsn);

    return result;
}

Datum schema_to_xml(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    Name name = PG_GETARG_NAME(0);
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;

    char* schemaname = NULL;
    Oid nspid;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    schemaname = NameStr(*name);

    if (schemaname == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("schema name is NULL.")));
    }

    nspid = LookupExplicitNamespace(schemaname);

    PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xml_internal(nspid, NULL, nulls, tableforest, targetns, true)));
}

/*
 * Write the start element of the root element of an XML Schema mapping.
 */
static void xsd_schema_element_start(StringInfo result, const char* targetns)
{
    appendStringInfoString(result,
        "<xsd:schema\n"
        "    xmlns:xsd=\"" NAMESPACE_XSD "\"");
    if (strlen(targetns) > 0)
        appendStringInfo(result,
            "\n"
            "    targetNamespace=\"%s\"\n"
            "    elementFormDefault=\"qualified\"",
            targetns);
    appendStringInfoString(result, ">\n\n");
}

static void xsd_schema_element_end(StringInfo result)
{
    appendStringInfoString(result, "</xsd:schema>");
}

static StringInfo schema_to_xmlschema_internal(
    const char* schemaname, bool nulls, bool tableforest, const char* targetns)
{
    Oid nspid;
    List* relid_list = NIL;
    List* tupdesc_list = NIL;
    ListCell* cell = NULL;
    StringInfo result;
    int rc = 0;

    result = makeStringInfo();

    if (schemaname == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("schema name is NULL.")));
    }

    nspid = LookupExplicitNamespace(schemaname);

    xsd_schema_element_start(result, targetns);

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    relid_list = schema_get_xml_visible_tables(nspid);

    tupdesc_list = NIL;
    foreach (cell, relid_list) {
        Relation rel;

        rel = heap_open(lfirst_oid(cell), AccessShareLock);
        tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
        heap_close(rel, NoLock);
    }

    appendStringInfoString(result, map_sql_typecoll_to_xmlschema_types(tupdesc_list));

    appendStringInfoString(result, map_sql_schema_to_xmlschema_types(nspid, relid_list, nulls, tableforest, targetns));

    xsd_schema_element_end(result);

    SPI_STACK_LOG("finish", NULL, NULL);
    SPI_finish();

    return result;
}

Datum schema_to_xmlschema(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    Name name = PG_GETARG_NAME(0);
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xmlschema_internal(NameStr(*name), nulls, tableforest, targetns)));
}

Datum schema_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
    /*
     * If you want to remove the macro, please fix the issue caused by
     * NULL input of first param. Check input of text_to_cstring? or
     * modify the code below? or set proisstrict in pg_proc?
     */
    NO_XML_SUPPORT();

    Name name = PG_GETARG_NAME(0);
    bool nulls = PG_GETARG_BOOL(1);
    bool tableforest = PG_GETARG_BOOL(2);
    const char* targetns = NULL;
    char* schemaname = NULL;
    Oid nspid;
    StringInfo xmlschema;

    if (PG_ARGISNULL(3))
        targetns = DEF_TARGETNS;
    else
        targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

    schemaname = NameStr(*name);

    if (schemaname == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("schema name is NULL.")));
    }

    nspid = LookupExplicitNamespace(schemaname);

    xmlschema = schema_to_xmlschema_internal(schemaname, nulls, tableforest, targetns);

    PG_RETURN_XML_P(
        stringinfo_to_xmltype(schema_to_xml_internal(nspid, xmlschema->data, nulls, tableforest, targetns, true)));
}

/*
 * Map SQL database to XML and/or XML Schema document; see SQL/XML:2008
 * sections 9.16, 9.17.
 */

static StringInfo database_to_xml_internal(const char* xmlschema, bool nulls, bool tableforest, const char* targetns)
{
    StringInfo result;
    List* nspid_list = NIL;
    ListCell* cell = NULL;
    char* xmlcn = NULL;
    int rc = 0;

    xmlcn = map_sql_identifier_to_xml_name(get_and_check_db_name(u_sess->proc_cxt.MyDatabaseId, true), true, false);
    result = makeStringInfo();

    xmldata_root_element_start(result, xmlcn, xmlschema, targetns, true);

    if (xmlschema != NULL)
        appendStringInfo(result, "%s\n\n", xmlschema);

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    nspid_list = database_get_xml_visible_schemas();

    SPI_STACK_LOG("push", NULL, NULL);
    SPI_push();

    foreach (cell, nspid_list) {
        Oid nspid = lfirst_oid(cell);
        StringInfo subres;

        subres = schema_to_xml_internal(nspid, NULL, nulls, tableforest, targetns, false);

        appendStringInfoString(result, subres->data);
        appendStringInfoChar(result, '\n');
    }

    SPI_STACK_LOG("pop", NULL, NULL);
    SPI_pop();
    SPI_STACK_LOG("finish", NULL, NULL);
    SPI_finish();

    xmldata_root_element_end(result, xmlcn);

    return result;
}

Datum database_to_xml(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    bool nulls = PG_GETARG_BOOL(0);
    bool tableforest = PG_GETARG_BOOL(1);
    const char* targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

    PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xml_internal(NULL, nulls, tableforest, targetns)));
}

static StringInfo database_to_xmlschema_internal(bool nulls, bool tableforest, const char* targetns)
{
    List* relid_list = NIL;
    List* nspid_list = NIL;
    List* tupdesc_list = NIL;
    ListCell* cell = NULL;
    StringInfo result;
    int rc = 0;

    result = makeStringInfo();

    xsd_schema_element_start(result, targetns);

    /*
     * Connect to SPI manager
     */
    SPI_STACK_LOG("connect", NULL, NULL);
    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                    errmsg("SPI_connect failed: %s", SPI_result_code_string(rc)))));

    relid_list = database_get_xml_visible_tables();
    nspid_list = database_get_xml_visible_schemas();

    tupdesc_list = NIL;
    foreach (cell, relid_list) {
        Relation rel;

        rel = heap_open(lfirst_oid(cell), AccessShareLock);
        tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
        heap_close(rel, NoLock);
    }

    appendStringInfoString(result, map_sql_typecoll_to_xmlschema_types(tupdesc_list));

    appendStringInfoString(result, map_sql_catalog_to_xmlschema_types(nspid_list));

    xsd_schema_element_end(result);

    SPI_STACK_LOG("finish", NULL, NULL);
    SPI_finish();

    return result;
}

Datum database_to_xmlschema(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    bool nulls = PG_GETARG_BOOL(0);
    bool tableforest = PG_GETARG_BOOL(1);
    const char* targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

    PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xmlschema_internal(nulls, tableforest, targetns)));
}

Datum database_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
    NO_XML_SUPPORT();

    bool nulls = PG_GETARG_BOOL(0);
    bool tableforest = PG_GETARG_BOOL(1);
    const char* targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));
    StringInfo xmlschema;

    xmlschema = database_to_xmlschema_internal(nulls, tableforest, targetns);

    PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xml_internal(xmlschema->data, nulls, tableforest, targetns)));
}

/*
 * Map a multi-part SQL name to an XML name; see SQL/XML:2008 section
 * 9.2.
 */
static char* map_multipart_sql_identifier_to_xml_name(char* a, char* b, char* c, char* d)
{
    StringInfoData result;

    initStringInfo(&result);

    if (a != NULL)
        appendStringInfo(&result, "%s", map_sql_identifier_to_xml_name(a, true, true));
    if (b != NULL)
        appendStringInfo(&result, ".%s", map_sql_identifier_to_xml_name(b, true, true));
    if (c != NULL)
        appendStringInfo(&result, ".%s", map_sql_identifier_to_xml_name(c, true, true));
    if (d != NULL)
        appendStringInfo(&result, ".%s", map_sql_identifier_to_xml_name(d, true, true));

    return result.data;
}

/*
 * Map an SQL table to an XML Schema document; see SQL/XML:2008
 * section 9.11.
 *
 * Map an SQL table to XML Schema data types; see SQL/XML:2008 section
 * 9.9.
 */
static const char* map_sql_table_to_xmlschema(
    TupleDesc tupdesc, Oid relid, bool nulls, bool tableforest, const char* targetns)
{
    int i;
    char* xmltn = NULL;
    char* tabletypename = NULL;
    char* rowtypename = NULL;
    StringInfoData result;

    initStringInfo(&result);

    if (OidIsValid(relid)) {
        HeapTuple tuple;
        Form_pg_class reltuple;

        tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
        if (!HeapTupleIsValid(tuple)) {
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relid))));
        }

        reltuple = (Form_pg_class)GETSTRUCT(tuple);

        xmltn = map_sql_identifier_to_xml_name(NameStr(reltuple->relname), true, false);

        tabletypename = map_multipart_sql_identifier_to_xml_name("TableType",
            get_database_name(u_sess->proc_cxt.MyDatabaseId),
            get_namespace_name(reltuple->relnamespace),
            NameStr(reltuple->relname));

        rowtypename = map_multipart_sql_identifier_to_xml_name("RowType",
            get_database_name(u_sess->proc_cxt.MyDatabaseId),
            get_namespace_name(reltuple->relnamespace),
            NameStr(reltuple->relname));

        ReleaseSysCache(tuple);
    } else {
        if (tableforest)
            xmltn = "row";
        else
            xmltn = "table";

        tabletypename = "TableType";
        rowtypename = "RowType";
    }

    xsd_schema_element_start(&result, targetns);

    appendStringInfoString(&result, map_sql_typecoll_to_xmlschema_types(list_make1(tupdesc)));

    appendStringInfo(&result,
        "<xsd:complexType name=\"%s\">\n"
        "  <xsd:sequence>\n",
        rowtypename);

    for (i = 0; i < tupdesc->natts; i++) {
        if (tupdesc->attrs[i].attisdropped)
            continue;
        appendStringInfo(&result,
            "    <xsd:element name=\"%s\" type=\"%s\"%s></xsd:element>\n",
            map_sql_identifier_to_xml_name(NameStr(tupdesc->attrs[i].attname), true, false),
            map_sql_type_to_xml_name(tupdesc->attrs[i].atttypid, -1),
            nulls ? " nillable=\"true\"" : " minOccurs=\"0\"");
    }

    appendStringInfoString(&result,
        "  </xsd:sequence>\n"
        "</xsd:complexType>\n\n");

    if (!tableforest) {
        appendStringInfo(&result,
            "<xsd:complexType name=\"%s\">\n"
            "  <xsd:sequence>\n"
            "    <xsd:element name=\"row\" type=\"%s\" minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n"
            "  </xsd:sequence>\n"
            "</xsd:complexType>\n\n",
            tabletypename,
            rowtypename);

        appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n", xmltn, tabletypename);
    } else
        appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n", xmltn, rowtypename);

    xsd_schema_element_end(&result);

    return result.data;
}

/*
 * Map an SQL schema to XML Schema data types; see SQL/XML:2008
 * section 9.12.
 */
static const char* map_sql_schema_to_xmlschema_types(
    Oid nspid, List* relid_list, bool nulls, bool tableforest, const char* targetns)
{
    char* dbname = NULL;
    char* nspname = NULL;
    char* xmlsn = NULL;
    char* schematypename = NULL;
    StringInfoData result;
    ListCell* cell = NULL;

    dbname = get_database_name(u_sess->proc_cxt.MyDatabaseId);
    nspname = get_namespace_name(nspid);

    initStringInfo(&result);

    xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);

    schematypename = map_multipart_sql_identifier_to_xml_name("SchemaType", dbname, nspname, NULL);

    appendStringInfo(&result, "<xsd:complexType name=\"%s\">\n", schematypename);
    if (!tableforest)
        appendStringInfoString(&result, "  <xsd:all>\n");
    else
        appendStringInfoString(&result, "  <xsd:sequence>\n");

    foreach (cell, relid_list) {
        Oid relid = lfirst_oid(cell);
        char* relname = get_rel_name(relid);
        char* xmltn = map_sql_identifier_to_xml_name(relname, true, false);
        char* tabletypename = map_multipart_sql_identifier_to_xml_name(
            (char*)(tableforest ? "RowType" : "TableType"), dbname, nspname, relname);

        if (!tableforest)
            appendStringInfo(&result, "    <xsd:element name=\"%s\" type=\"%s\"/>\n", xmltn, tabletypename);
        else
            appendStringInfo(&result,
                "    <xsd:element name=\"%s\" type=\"%s\" minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n",
                xmltn,
                tabletypename);
    }

    if (!tableforest)
        appendStringInfoString(&result, "  </xsd:all>\n");
    else
        appendStringInfoString(&result, "  </xsd:sequence>\n");
    appendStringInfoString(&result, "</xsd:complexType>\n\n");

    appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n", xmlsn, schematypename);

    return result.data;
}

/*
 * Map an SQL catalog to XML Schema data types; see SQL/XML:2008
 * section 9.15.
 */
static const char* map_sql_catalog_to_xmlschema_types(List* nspid_list)
{
    char* dbname = NULL;
    char* xmlcn = NULL;
    char* catalogtypename = NULL;
    StringInfoData result;
    ListCell* cell = NULL;

    dbname = get_database_name(u_sess->proc_cxt.MyDatabaseId);

    initStringInfo(&result);

    xmlcn = map_sql_identifier_to_xml_name(dbname, true, false);

    catalogtypename = map_multipart_sql_identifier_to_xml_name("CatalogType", dbname, NULL, NULL);

    appendStringInfo(&result, "<xsd:complexType name=\"%s\">\n", catalogtypename);
    appendStringInfoString(&result, "  <xsd:all>\n");

    foreach (cell, nspid_list) {
        Oid nspid = lfirst_oid(cell);
        char* nspname = get_namespace_name(nspid);
        char* xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);
        char* schematypename = map_multipart_sql_identifier_to_xml_name("SchemaType", dbname, nspname, NULL);

        appendStringInfo(&result, "    <xsd:element name=\"%s\" type=\"%s\"/>\n", xmlsn, schematypename);
    }

    appendStringInfoString(&result, "  </xsd:all>\n");
    appendStringInfoString(&result, "</xsd:complexType>\n\n");

    appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n", xmlcn, catalogtypename);

    return result.data;
}

/*
 * Map an SQL data type to an XML name; see SQL/XML:2008 section 9.4.
 */
static const char* map_sql_type_to_xml_name(Oid typeoid, int typmod)
{
    StringInfoData result;

    initStringInfo(&result);

    switch (typeoid) {
        case BPCHAROID:
            if (typmod == -1)
                appendStringInfo(&result, "CHAR");
            else
                appendStringInfo(&result, "CHAR_%d", typmod - VARHDRSZ);
            break;
        case VARCHAROID:
            if (typmod == -1)
                appendStringInfo(&result, "VARCHAR");
            else
                appendStringInfo(&result, "VARCHAR_%d", typmod - VARHDRSZ);
            break;
        case NUMERICOID:
            if (typmod == -1)
                appendStringInfo(&result, "NUMERIC");
            else
                appendStringInfo(&result,
                    "NUMERIC_%d_%d",
                    ((unsigned int)(typmod - VARHDRSZ) >> 16) & 0xffff,
                    (unsigned int)(typmod - VARHDRSZ) & 0xffff);
            break;
        case INT4OID:
            appendStringInfo(&result, "INTEGER");
            break;
        case INT2OID:
            appendStringInfo(&result, "SMALLINT");
            break;
        case INT1OID:
            appendStringInfo(&result, "TINYINT");
            break;
        case INT8OID:
            appendStringInfo(&result, "BIGINT");
            break;
        case FLOAT4OID:
            appendStringInfo(&result, "REAL");
            break;
        case FLOAT8OID:
            appendStringInfo(&result, "DOUBLE");
            break;
        case BOOLOID:
            appendStringInfo(&result, "BOOLEAN");
            break;
        case TIMEOID:
            if (typmod == -1)
                appendStringInfo(&result, "TIME");
            else
                appendStringInfo(&result, "TIME_%d", typmod);
            break;
        case TIMETZOID:
            if (typmod == -1)
                appendStringInfo(&result, "TIME_WTZ");
            else
                appendStringInfo(&result, "TIME_WTZ_%d", typmod);
            break;
        case TIMESTAMPOID:
            if (typmod == -1)
                appendStringInfo(&result, "TIMESTAMP");
            else
                appendStringInfo(&result, "TIMESTAMP_%d", typmod);
            break;
        case TIMESTAMPTZOID:
            if (typmod == -1)
                appendStringInfo(&result, "TIMESTAMP_WTZ");
            else
                appendStringInfo(&result, "TIMESTAMP_WTZ_%d", typmod);
            break;
        case SMALLDATETIMEOID:
            appendStringInfo(&result, "SMALLDATETIME");
            break;
        case DATEOID:
            appendStringInfo(&result, "DATE");
            break;
        case XMLOID:
            appendStringInfo(&result, "XML");
            break;
        default: {
            HeapTuple tuple;
            Form_pg_type typtuple;

            tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeoid));
            if (!HeapTupleIsValid(tuple)) {
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for type %u", typeoid))));
            }

            typtuple = (Form_pg_type)GETSTRUCT(tuple);

            appendStringInfoString(&result,
                map_multipart_sql_identifier_to_xml_name(
                    (char*)((typtuple->typtype == TYPTYPE_DOMAIN) ? "Domain" : "UDT"),
                    get_database_name(u_sess->proc_cxt.MyDatabaseId),
                    get_namespace_name(typtuple->typnamespace),
                    NameStr(typtuple->typname)));

            ReleaseSysCache(tuple);
        } break;
    }

    return result.data;
}

/*
 * Map a collection of SQL data types to XML Schema data types; see
 * SQL/XML:2008 section 9.7.
 */
static const char* map_sql_typecoll_to_xmlschema_types(List* tupdesc_list)
{
    List* uniquetypes = NIL;
    int i;
    StringInfoData result;
    ListCell* cell0 = NULL;

    /* extract all column types used in the set of TupleDescs */
    foreach (cell0, tupdesc_list) {
        TupleDesc tupdesc = (TupleDesc)lfirst(cell0);

        for (i = 0; i < tupdesc->natts; i++) {
            if (tupdesc->attrs[i].attisdropped)
                continue;
            uniquetypes = list_append_unique_oid(uniquetypes, tupdesc->attrs[i].atttypid);
        }
    }

    /* add base types of domains */
    foreach (cell0, uniquetypes) {
        Oid typid = lfirst_oid(cell0);
        Oid basetypid = getBaseType(typid);

        if (basetypid != typid)
            uniquetypes = list_append_unique_oid(uniquetypes, basetypid);
    }

    /* Convert to textual form */
    initStringInfo(&result);

    foreach (cell0, uniquetypes) {
        appendStringInfo(&result, "%s\n", map_sql_type_to_xmlschema_type(lfirst_oid(cell0), -1));
    }

    return result.data;
}

/*
 * Map an SQL data type to a named XML Schema data type; see
 * SQL/XML:2008 sections 9.5 and 9.6.
 *
 * (The distinction between 9.5 and 9.6 is basically that 9.6 adds
 * a name attribute, which this function does.	The name-less version
 * 9.5 doesn't appear to be required anywhere.)
 */
static const char* map_sql_type_to_xmlschema_type(Oid typeoid, int typmod)
{
    StringInfoData result;
    const char* typname = map_sql_type_to_xml_name(typeoid, typmod);

    initStringInfo(&result);

    if (typeoid == XMLOID) {
        appendStringInfo(&result,
            "<xsd:complexType mixed=\"true\">\n"
            "  <xsd:sequence>\n"
            "    <xsd:any name=\"element\" minOccurs=\"0\" maxOccurs=\"unbounded\" processContents=\"skip\"/>\n"
            "  </xsd:sequence>\n"
            "</xsd:complexType>\n");
    } else {
        appendStringInfo(&result, "<xsd:simpleType name=\"%s\">\n", typname);

        switch (typeoid) {
            case BPCHAROID:
            case VARCHAROID:
            case TEXTOID:
                appendStringInfo(&result, "  <xsd:restriction base=\"xsd:string\">\n");
                if (typmod != -1)
                    appendStringInfo(&result, "    <xsd:maxLength value=\"%d\"/>\n", typmod - VARHDRSZ);
                appendStringInfo(&result, "  </xsd:restriction>\n");
                break;

            case BYTEAOID:
                appendStringInfo(&result,
                    "  <xsd:restriction base=\"xsd:%s\">\n"
                    "  </xsd:restriction>\n",
                    (u_sess->attr.attr_common.xmlbinary == XMLBINARY_BASE64) ? "base64Binary" : "hexBinary");
                break;

            case NUMERICOID:
                if (typmod != -1)
                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"xsd:decimal\">\n"
                        "    <xsd:totalDigits value=\"%d\"/>\n"
                        "    <xsd:fractionDigits value=\"%d\"/>\n"
                        "  </xsd:restriction>\n",
                        ((unsigned int)(typmod - VARHDRSZ) >> 16) & 0xffff,
                        (short)((typmod - VARHDRSZ) & 0xffff));
                break;

            case INT2OID:
                appendStringInfo(&result,
                    "  <xsd:restriction base=\"xsd:short\">\n"
                    "    <xsd:maxInclusive value=\"%d\"/>\n"
                    "    <xsd:minInclusive value=\"%d\"/>\n"
                    "  </xsd:restriction>\n",
                    SHRT_MAX,
                    SHRT_MIN);
                break;

            case INT4OID:
                appendStringInfo(&result,
                    "  <xsd:restriction base=\"xsd:int\">\n"
                    "    <xsd:maxInclusive value=\"%d\"/>\n"
                    "    <xsd:minInclusive value=\"%d\"/>\n"
                    "  </xsd:restriction>\n",
                    INT_MAX,
                    INT_MIN);
                break;

            case INT8OID:
                appendStringInfo(&result,
                    "  <xsd:restriction base=\"xsd:long\">\n"
                    "    <xsd:maxInclusive value=\"" INT64_FORMAT "\"/>\n"
                    "    <xsd:minInclusive value=\"" INT64_FORMAT "\"/>\n"
                    "  </xsd:restriction>\n",
                    (((uint64)1) << (sizeof(int64) * 8 - 1)) - 1,
                    (((uint64)1) << (sizeof(int64) * 8 - 1)));
                break;

            case FLOAT4OID:
                appendStringInfo(&result, "  <xsd:restriction base=\"xsd:float\"></xsd:restriction>\n");
                break;

            case FLOAT8OID:
                appendStringInfo(&result, "  <xsd:restriction base=\"xsd:double\"></xsd:restriction>\n");
                break;

            case BOOLOID:
                appendStringInfo(&result, "  <xsd:restriction base=\"xsd:boolean\"></xsd:restriction>\n");
                break;

            case TIMEOID:
            case TIMETZOID: {
                const char* tz = ((typeoid == TIMETZOID) ? "(+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

                if (typmod == -1)
                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"xsd:time\">\n"
                        "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
                        "  </xsd:restriction>\n",
                        tz);
                else if (typmod == 0)
                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"xsd:time\">\n"
                        "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
                        "  </xsd:restriction>\n",
                        tz);
                else
                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"xsd:time\">\n"
                        "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
                        "  </xsd:restriction>\n",
                        typmod - VARHDRSZ,
                        tz);
                break;
            }

            case TIMESTAMPOID:
            case TIMESTAMPTZOID: {
                const char* tz = ((typeoid == TIMESTAMPTZOID) ? "(+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

                if (typmod == -1)
                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"xsd:dateTime\">\n"
                        "    <xsd:pattern "
                        "value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
                        "  </xsd:restriction>\n",
                        tz);
                else if (typmod == 0)
                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"xsd:dateTime\">\n"
                        "    <xsd:pattern "
                        "value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
                        "  </xsd:restriction>\n",
                        tz);
                else
                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"xsd:dateTime\">\n"
                        "    <xsd:pattern "
                        "value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
                        "  </xsd:restriction>\n",
                        typmod - VARHDRSZ,
                        tz);
                break;
            }

            case DATEOID:
                appendStringInfo(&result,
                    "  <xsd:restriction base=\"xsd:date\">\n"
                    "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}\"/>\n"
                    "  </xsd:restriction>\n");
                break;

            default:
                if (get_typtype(typeoid) == TYPTYPE_DOMAIN) {
                    Oid base_typeoid;
                    int32 base_typmod = -1;

                    base_typeoid = getBaseTypeAndTypmod(typeoid, &base_typmod);

                    appendStringInfo(&result,
                        "  <xsd:restriction base=\"%s\"/>\n",
                        map_sql_type_to_xml_name(base_typeoid, base_typmod));
                }
                break;
        }
        appendStringInfo(&result, "</xsd:simpleType>\n");
    }

    return result.data;
}

/*
 * Map an SQL row to an XML element, taking the row from the active
 * SPI cursor.	See also SQL/XML:2008 section 9.10.
 */
static void SPI_sql_row_to_xmlelement(
    int rownum, StringInfo result, char* tablename, bool nulls, bool tableforest, const char* targetns, bool top_level)
{
    int i;
    char* xmltn = NULL;

    if (tablename != NULL)
        xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
    else {
        if (tableforest)
            xmltn = "row";
        else
            xmltn = "table";
    }

    if (tableforest)
        xmldata_root_element_start(result, xmltn, NULL, targetns, top_level);
    else
        appendStringInfoString(result, "<row>\n");

    for (i = 1; i <= SPI_tuptable->tupdesc->natts; i++) {
        char* colname = NULL;
        Datum colval;
        bool isnull = false;

        colname = map_sql_identifier_to_xml_name(SPI_fname(SPI_tuptable->tupdesc, i), true, false);
        colval = SPI_getbinval(SPI_tuptable->vals[rownum], SPI_tuptable->tupdesc, i, &isnull);
        if (isnull) {
            if (nulls)
                appendStringInfo(result, "  <%s xsi:nil=\"true\"/>\n", colname);
        } else
            appendStringInfo(result,
                "  <%s>%s</%s>\n",
                colname,
                map_sql_value_to_xml_value(colval, SPI_gettypeid(SPI_tuptable->tupdesc, i), true),
                colname);
    }

    if (tableforest) {
        xmldata_root_element_end(result, xmltn);
        appendStringInfoChar(result, '\n');
    } else
        appendStringInfoString(result, "</row>\n\n");
}

/*
 * XPath related functions
 */

#ifdef USE_LIBXML

/*
 * Convert XML node to text (dump subtree in case of element,
 * return value otherwise)
 */
static text* xml_xmlnodetoxmltype(xmlNodePtr cur)
{
    xmltype* result = NULL;

    if (cur->type == XML_ELEMENT_NODE) {
        xmlBufferPtr buf;

        buf = xmlBufferCreate();
        PG_TRY();
        {
            xmlNodeDump(buf, NULL, cur, 0, 1);
            result = xmlBuffer_to_xmltype(buf);
        }
        PG_CATCH();
        {
            xmlBufferFree(buf);
            PG_RE_THROW();
        }
        PG_END_TRY();
        xmlBufferFree(buf);
    } else {
        xmlChar* str = NULL;

        str = xmlXPathCastNodeToString(cur);
        PG_TRY();
        {
            /* Here we rely on XML having the same representation as TEXT */
            char* escaped = escape_xml((char*)str);

            result = (xmltype*)cstring_to_text(escaped);
            pfree_ext(escaped);
        }
        PG_CATCH();
        {
            xmlFree(str);
            PG_RE_THROW();
        }
        PG_END_TRY();
        xmlFree(str);
    }

    return result;
}

/*
 * Convert an XML XPath object (the result of evaluating an XPath expression)
 * to an array of xml values, which is returned at *astate.  The function
 * result value is the number of elements in the array.
 *
 * If "astate" is NULL then we don't generate the array value, but we still
 * return the number of elements it would have had.
 *
 * Nodesets are converted to an array containing the nodes' textual
 * representations.  Primitive values (float, double, string) are converted
 * to a single-element array containing the value's string representation.
 */
static int xml_xpathobjtoxmlarray(xmlXPathObjectPtr xpathobj, ArrayBuildState** astate, PgXmlErrorContext *xmlerrcxt)
{
    int result = 0;
    Datum datum;
    Oid datumtype;
    char* result_str = NULL;

    if (astate != NULL)
        *astate = NULL;

    switch (xpathobj->type) {
        case XPATH_NODESET:
            if (xpathobj->nodesetval != NULL) {
                result = xpathobj->nodesetval->nodeNr;
                if (astate != NULL) {
                    int i;

                    for (i = 0; i < result; i++) {
                        datum = PointerGetDatum(xml_xmlnodetoxmltype(xpathobj->nodesetval->nodeTab[i]));
                        *astate = accumArrayResult(*astate, datum, false, XMLOID, CurrentMemoryContext);
                    }
                }
            }
            return result;

        case XPATH_BOOLEAN:
            if (astate == NULL)
                return 1;
            datum = BoolGetDatum(xpathobj->boolval);
            datumtype = BOOLOID;
            break;

        case XPATH_NUMBER:
            if (astate == NULL)
                return 1;
            datum = Float8GetDatum(xpathobj->floatval);
            datumtype = FLOAT8OID;
            break;

        case XPATH_STRING:
            if (astate == NULL)
                return 1;
            datum = CStringGetDatum((char*)xpathobj->stringval);
            datumtype = CSTRINGOID;
            break;

        default:
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    (errcode(ERRCODE_CHECK_VIOLATION),
                        errmsg("xpath expression result type %d is unsupported", xpathobj->type))));

            return 0; /* keep compiler quiet */
    }

    /* Common code for scalar-value cases */
    result_str = map_sql_value_to_xml_value(datum, datumtype, true);
    datum = PointerGetDatum(cstring_to_xmltype(result_str));
    *astate = accumArrayResult(*astate, datum, false, XMLOID, CurrentMemoryContext);
    return 1;
}

/*
 * Common code for xpath() and xmlexists()
 *
 * Evaluate XPath expression and return number of nodes in res_items
 * and array of XML values in astate.  Either of those pointers can be
 * NULL if the corresponding result isn't wanted.
 *
 * It is up to the user to ensure that the XML passed is in fact
 * an XML document - XPath doesn't work easily on fragments without
 * a context node being known.
 */
static void xpath_internal(
    text* xpath_expr_text, xmltype* data, ArrayType* namespaces, int* res_nitems, ArrayBuildState** astate)
{
    PgXmlErrorContext* xmlerrcxt = NULL;
    volatile xmlParserCtxtPtr ctxt = NULL;
    volatile xmlDocPtr doc = NULL;
    volatile xmlXPathContextPtr xpathctx = NULL;
    volatile xmlXPathCompExprPtr xpathcomp = NULL;
    volatile xmlXPathObjectPtr xpathobj = NULL;
    char* datastr = NULL;
    int32 len;
    int32 xpath_len;
    xmlChar* string = NULL;
    xmlChar* xpath_expr = NULL;
    int i;
    int ndim;
    Datum* ns_names_uris = NULL;
    bool* ns_names_uris_nulls = NULL;
    int ns_count;
    size_t xmldecl_len = 0;
    /*
     * Namespace mappings are passed as text[].  If an empty array is passed
     * (ndim = 0, "0-dimensional"), then there are no namespace mappings.
     * Else, a 2-dimensional array with length of the second axis being equal
     * to 2 should be passed, i.e., every subarray contains 2 elements, the
     * first element defining the name, the second one the URI.  Example:
     * ARRAY[ARRAY['myns', 'http://example.com'], ARRAY['myns2',
     * 'http://example2.com']].
     */
    ndim = namespaces ? ARR_NDIM(namespaces) : 0;
    if (ndim != 0) {
        int* dims = NULL;

        dims = ARR_DIMS(namespaces);

        if (ndim != 2 || dims[1] != 2)
            ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                    errmsg("invalid array for XML namespace mapping"),
                    errdetail("The array must be two-dimensional with length of the second axis equal to 2.")));

        Assert(ARR_ELEMTYPE(namespaces) == TEXTOID);

        deconstruct_array(namespaces, TEXTOID, -1, false, 'i', &ns_names_uris, &ns_names_uris_nulls, &ns_count);

        Assert((ns_count % 2) == 0); /* checked above */
        ns_count /= 2;               /* count pairs only */
    } else {
        ns_names_uris = NULL;
        ns_names_uris_nulls = NULL;
        ns_count = 0;
    }

    datastr = VARDATA(data);
    len = VARSIZE(data) - VARHDRSZ;
    xpath_len = VARSIZE(xpath_expr_text) - VARHDRSZ;
    if (xpath_len == 0)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("empty XPath expression")));

    string = xml_charpnstrdup(datastr, len);
    xpath_expr = xml_charpnstrdup(VARDATA(xpath_expr_text), xpath_len);

    if (GetDatabaseEncoding() == PG_UTF8) {
        parse_xml_decl(string, &xmldecl_len, NULL, NULL, NULL);
    }
    xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

    PG_TRY();
    {
        xmlInitParser();

        /*
         * redundant XML parsing (two parsings for the same value during one
         * command execution are possible)
         */
        ctxt = xmlNewParserCtxt();
        if (ctxt == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate parser context");
        doc = xmlCtxtReadMemory(ctxt, (char*)string + xmldecl_len, len - xmldecl_len, NULL, NULL, 0);
        if (doc == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT, "could not parse XML document");
        xpathctx = xmlXPathNewContext(doc);
        if (xpathctx == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate XPath context");
        xpathctx->node = xmlDocGetRootElement(doc);
        if (xpathctx->node == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNDEFINED_OBJECT, "could not find root XML element");

        /* register namespaces, if any */
        if (ns_count > 0) {
            for (i = 0; i < ns_count; i++) {
                char* ns_name = NULL;
                char* ns_uri = NULL;

                if (ns_names_uris_nulls[i * 2] || ns_names_uris_nulls[i * 2 + 1])
                    ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            errmsg("neither namespace name nor URI may be null")));
                ns_name = TextDatumGetCString(ns_names_uris[i * 2]);
                ns_uri = TextDatumGetCString(ns_names_uris[i * 2 + 1]);
                if (xmlXPathRegisterNs(xpathctx, (xmlChar*)ns_name, (xmlChar*)ns_uri) != 0)
                    ereport(ERROR, /* is this an internal error??? */
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg(
                                "could not register XML namespace with name \"%s\" and URI \"%s\"", ns_name, ns_uri)));
            }
        }

        xpathcomp = xmlXPathCompile(xpath_expr);
        if (xpathcomp == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNDEFINED_FILE, "invalid XPath expression");

        /*
         * Version 2.6.27 introduces a function named
         * xmlXPathCompiledEvalToBoolean, which would be enough for xmlexists,
         * but we can derive the existence by whether any nodes are returned,
         * thereby preventing a library version upgrade and keeping the code
         * the same.
         */
        xpathobj = xmlXPathCompiledEval(xpathcomp, xpathctx);
        if (xpathobj == NULL || xmlerrcxt->err_occurred)
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNEXPECTED_NULL_VALUE, "could not create XPath object");

        /*
         * Extract the results as requested.
         */
        if (res_nitems != NULL)
            *res_nitems = xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
        else
            (void)xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
    }
    PG_CATCH();
    {
        if (xpathobj)
            xmlXPathFreeObject(xpathobj);
        if (xpathcomp)
            xmlXPathFreeCompExpr(xpathcomp);
        if (xpathctx)
            xmlXPathFreeContext(xpathctx);
        if (doc)
            xmlFreeDoc(doc);
        if (ctxt)
            xmlFreeParserCtxt(ctxt);

        pg_xml_done(xmlerrcxt, true);

        PG_RE_THROW();
    }
    PG_END_TRY();

    xmlXPathFreeObject(xpathobj);
    xmlXPathFreeCompExpr(xpathcomp);
    xmlXPathFreeContext(xpathctx);
    xmlFreeDoc(doc);
    xmlFreeParserCtxt(ctxt);

    pg_xml_done(xmlerrcxt, false);
}

static xmlChar* buildXpathAbsolutePath(text* xpath_expr_text)
{
    xmlChar* xpath_expr = NULL;

    int xpath_len = VARSIZE(xpath_expr_text) - VARHDRSZ;
    if (xpath_len == 0)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("empty XPath expression")));

    xpath_expr = xml_charpnstrdup(VARDATA(xpath_expr_text), xpath_len);

    return xpath_expr;
}
/*
 * Common code for xmltype_extract(), xmltype_xmlsequence() and xmltype_existsnode()
 *
 * Evaluate XPath expression and return number of nodes in res_items
 * and array of XML values in astate.  Either of those pointers can be
 * NULL if the corresponding result isn't wanted.
 *
 * It is up to the user to ensure that the XML passed is in fact
 * an XML document - XPath doesn't work easily on fragments without
 * a context node being known.
 */
static void extract_internal(
    xmltype* data, text* xpath_expr_text, ArrayType* namespaces, int* res_nitems, ArrayBuildState** astate)
{
    PgXmlErrorContext* xmlerrcxt = NULL;
    volatile xmlDocPtr doc = NULL;
    volatile xmlXPathContextPtr xpathctx = NULL;
    volatile xmlXPathObjectPtr xpathobj = NULL;
    xmlChar* xpath_expr = NULL;
    int i = 0;
    int ndim = 0;
    Datum* ns_names_uris = NULL;
    bool* ns_names_uris_nulls = NULL;
    int ns_count = 0;

    ndim = namespaces ? ARR_NDIM(namespaces) : 0;
    if (ndim != 0) {
        int* dim = NULL;
        dim = ARR_DIMS(namespaces);
        if (ndim != 2 && dim[1] != 2) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                    errmsg("invalid array for XML namespaces mapping"),
                    errdetail("The array mast be two-dimensional with the length of second axis equal to 2.")));
        } else {
            ns_names_uris = NULL;
            ns_names_uris_nulls = NULL;
            ns_count = 0;
        }

        Assert(ARR_ELEMTYPE(namespaces) == TEXTOID);
        deconstruct_array(namespaces, TEXTOID, -1, false, 'i', &ns_names_uris, &ns_names_uris_nulls, &ns_count);

        Assert(ns_count % 2 == 0);
        ns_count /= 2;
    }

    xpath_expr = buildXpathAbsolutePath(xpath_expr_text);

    xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

    PG_TRY();
    {
        doc = xml_parse(data, XMLOPTION_DOCUMENT, true, GetDatabaseEncoding());

        xpathctx = xmlXPathNewContext(doc);
        if (xpathctx == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate XPath context");
        }
        xpathctx->node = xmlDocGetRootElement(doc);
        if (xpathctx->node == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNDEFINED_OBJECT, "could not find root XML element");
        }

        if (ns_count > 0) {
            for (i = 0; i < ns_count; i++) {
                char* ns_name = NULL;
                char* ns_uri = NULL;

                if (ns_names_uris_nulls[i * 2] || ns_names_uris_nulls[i * 2 + 1]) {
                    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            errmsg("neither namespace name nor URI may be null")));
                }
                ns_name = TextDatumGetCString(ns_names_uris_nulls[i * 2]);
                ns_uri = TextDatumGetCString(ns_names_uris_nulls[i * 2 + 1]);
                if (xmlXPathRegisterNs(xpathctx, (xmlChar*)ns_name, (xmlChar*)ns_uri) != 0) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("could not register XML namespace with name \"%s\" and \
                                URI \"%s\"", ns_name, ns_uri)));
                }
            }
        }
        xpathobj = xmlXPathEvalExpression(xpath_expr, xpathctx);
        if (xpathobj == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_OPERATION, "could not create XPath object");
        }

        if (res_nitems != NULL) {
            *res_nitems = xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
        } else {
            (void)xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
        }
    }
    PG_CATCH();
    {
        if (xpathobj) {
            xmlXPathFreeObject(xpathobj);
        }
        if (xpathctx) {
            xmlXPathFreeContext(xpathctx);
        }
        if (doc) {
            xmlFreeDoc(doc);
        }

        pg_xml_done(xmlerrcxt, true);

        PG_RE_THROW();
    }
    PG_END_TRY();

    xmlXPathFreeObject(xpathobj);
    xmlXPathFreeContext(xpathctx);
    xmlFreeDoc(doc);

    pg_xml_done(xmlerrcxt, false);
}

/*
 * Get xmltype node value
 */
static char* xpath_extractvalue(xmltype* data, text* xpath_expr_text, ArrayType* namespaces)
{
    PgXmlErrorContext* xmlerrcxt = NULL;
    volatile xmlDocPtr doc = NULL;
    volatile xmlXPathContextPtr xpathctx = NULL;
    volatile xmlXPathObjectPtr xpathobj = NULL;
    xmlChar* xpath_expr = NULL;
    char* result = NULL;
    xmlChar* value = NULL;
    int i = 0;
    int ndim = 0;
    Datum* ns_names_uris = NULL;
    bool* ns_names_uris_nulls = NULL;
    int ns_count = 0;
    int div_num = 2;

    ndim = namespaces ? ARR_NDIM(namespaces) : 0;
    if (ndim != 0) {
        int* dim = NULL;
        dim = ARR_DIMS(namespaces);
        if (ndim != 2 && dim[1] != 2) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                    errmsg("invalid array for XML namespaces mapping"),
                    errdetail("The array mast be two-dimensional with the length of second axis equal to 2.")));
        } else {
            ns_names_uris = NULL;
            ns_names_uris_nulls = NULL;
            ns_count = 0;
        }

        Assert(ARR_ELEMTYPE(namespaces) == TEXTOID);
        deconstruct_array(namespaces, TEXTOID, -1, false, 'i', &ns_names_uris, &ns_names_uris_nulls, &ns_count);

        Assert(ns_count % div_num == 0);
        ns_count /= div_num;
    }

    xpath_expr = buildXpathAbsolutePath(xpath_expr_text);

    xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);
    PG_TRY();
    {
        doc = xml_parse(data, XMLOPTION_DOCUMENT, true, GetDatabaseEncoding());

        xpathctx = xmlXPathNewContext(doc);
        if (xpathctx == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate XPath context");
        }
        xpathctx->node = xmlDocGetRootElement(doc);
        if (xpathctx->node == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNDEFINED_OBJECT, "could not find root XML element");
        }
        if (ns_count > 0) {
            for (i = 0; i < ns_count; i++) {
                char* ns_name = NULL;
                char* ns_uri = NULL;

                if (ns_names_uris_nulls[i * 2] || ns_names_uris_nulls[i * 2 + 1]) {
                    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            errmsg("neither namespace name nor URI may be null")));
                }
                ns_name = TextDatumGetCString(ns_names_uris_nulls[i * 2]);
                ns_uri = TextDatumGetCString(ns_names_uris_nulls[i * 2 + 1]);
                if (xmlXPathRegisterNs(xpathctx, (xmlChar*)ns_name, (xmlChar*)ns_uri) != 0) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("could not register XML namespace with name \"%s\" and URI \"%s\"", ns_name, ns_uri)));
                }
            }
        }

        xpathobj = xmlXPathEvalExpression(xpath_expr, xpathctx);
        if (xpathobj == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_OPERATION, "could not create XPath object");
        }

        if (xpathobj->type == XPATH_NODESET && xpathobj->nodesetval != NULL) {
            if (xpathobj->nodesetval->nodeNr > 1) {
                xml_ereport(xmlerrcxt,
                    ERROR, ERRCODE_UNEXPECTED_NULL_VALUE, "EXTRACTVALUE returns value of only one node.");
            } else if (xpathobj->nodesetval->nodeNr == 1) {
                xmlNodePtr node = xpathobj->nodesetval->nodeTab[0];
                value = xmlXPathCastNodeToString(node);
                if (value) {
                    result = pstrdup((char*)value);
                    xmlFree(value);
                    value = NULL;
                }
            }
        }
    }
    PG_CATCH();
    {
        if (xpathobj) {
            xmlXPathFreeObject(xpathobj);
        }
        if (xpathctx) {
            xmlXPathFreeContext(xpathctx);
        }
        if (doc) {
            xmlFreeDoc(doc);
        }

        pg_xml_done(xmlerrcxt, true);

        PG_RE_THROW();
    }
    PG_END_TRY();

    xmlXPathFreeObject(xpathobj);
    xmlXPathFreeContext(xpathctx);
    xmlFreeDoc(doc);

    pg_xml_done(xmlerrcxt, false);

    return result;
}

static char* appendNewChildXml(xmltype* rootxml, text* xpath_expr_text,
                               ArrayType* namespaces, text* appendData_text)
{
    PgXmlErrorContext* xmlerrcxt = NULL;
    xmlDocPtr doc = NULL;
    xmlChar* xpathStr = NULL;
    xmlNodePtr curNode = NULL;
    xmlDocPtr appendDoc = NULL;
    xmlNodePtr appendNode = NULL;
    int i = 0;
    int nodeSize = 0;
    xmlChar* resdata = NULL;
    int rescount = 0;
    xmlXPathContextPtr xpathctx = NULL;
    xmlXPathObjectPtr xpathObj = NULL;
    Datum* ns_names_uris = NULL;
    bool* ns_names_uris_nulls = NULL;
    char* res = NULL;
    int ns_count = 0;
    int ndim;

    xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

    xpathStr = buildXpathAbsolutePath(xpath_expr_text);
    ndim = namespaces ? ARR_NDIM(namespaces) : 0;
    if (ndim != 0) {
        int* dim = NULL;
        dim = ARR_DIMS(namespaces);
        if (ndim != 2 || dim[1] != 2) {
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                    errmsg("invalid array for XML namespace mapping"),
                    errdetail("The array mast be two-dimensional with the length of second axis equal to 2.")));
        }
        Assert(ARR_ELEMTYPE(namespaces) == TEXTOID);
        deconstruct_array(namespaces, TEXTOID, -1, false, 'i', &ns_names_uris, &ns_names_uris_nulls, &ns_count);

        Assert(ns_count % 2 == 0);
        ns_count /= 2;
    } else {
        ns_names_uris = NULL;
        ns_names_uris_nulls = NULL;
        ns_count = 0;
    }

    PG_TRY();
    {
        xmlKeepBlanksDefault(0);
        doc = xml_parse(rootxml, XMLOPTION_DOCUMENT, true, GetDatabaseEncoding());

        xpathctx = xmlXPathNewContext(doc);
        if (xpathctx == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY, "could not allocate XPath context");
        }
        xpathctx->node = xmlDocGetRootElement(doc);
        if (xpathctx->node == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNDEFINED_OBJECT, "could not find root XML element");
        }

        if (ns_count > 0) {
            for (i = 0; i < ns_count; i++) {
                char* ns_name = NULL;
                char* ns_uri = NULL;

                if (ns_names_uris_nulls[i * 2] || ns_names_uris_nulls[i * 2 + 1]) {
                    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            errmsg("neither namespace name nor URI may be null")));
                }
                ns_name = TextDatumGetCString(ns_names_uris[i * 2]);
                ns_uri = TextDatumGetCString(ns_names_uris[i * 2 + 1]);
                if (xmlXPathRegisterNs(xpathctx, (xmlChar*)ns_name, (xmlChar*)ns_uri) != 0) {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("could not register XML namespace with name \"%s\" and URI \"%s\"", ns_name, ns_uri)));
                }
            }
        }

        xpathObj = xmlXPathEvalExpression(xpathStr, xpathctx);
        if (xpathObj == NULL || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_OPERATION, "could not create XPath object");
        }
        char* appendstr = text_to_cstring(appendData_text);
        if (appendstr[0] == '<') {
            appendDoc = xml_parse(appendData_text, XMLOPTION_DOCUMENT, true, GetDatabaseEncoding());
            appendNode = xmlDocGetRootElement(appendDoc);
            if (appendNode == NULL || xmlerrcxt->err_occurred) {
                xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNDEFINED_OBJECT, "could not find root XML element");
            }
        } else {
            appendNode = xmlNewText((xmlChar*)appendstr);
            if (appendNode == NULL || xmlerrcxt->err_occurred) {
                xml_ereport(xmlerrcxt, ERROR, ERRCODE_UNDEFINED_OBJECT, "could not parse XML content");
            }
        }

        nodeSize = (xpathObj->nodesetval) ? xpathObj->nodesetval->nodeNr : 0;
        for (i = 0; i < nodeSize; i++) {
            curNode = xpathObj->nodesetval->nodeTab[i];
            xmlNodePtr resNode = xmlAddChildList(curNode, xmlCopyNode(appendNode, 1));
            if (resNode == NULL || xmlerrcxt->err_occurred) {
                xml_ereport(xmlerrcxt, ERROR, ERRCODE_OPERATE_FAILED, "append child xml failed");
            }
        }
        xmlDocDumpFormatMemory(doc, &resdata, &rescount, 1);
        if (rescount == 0 || xmlerrcxt->err_occurred) {
            xml_ereport(xmlerrcxt, ERROR, ERRCODE_OPERATE_FAILED, "xml document dump failed");
        }
    }

    PG_CATCH();
    {
        if (xpathObj) {
            xmlXPathFreeObject(xpathObj);
        }
        if (xpathctx) {
            xmlXPathFreeContext(xpathctx);
        }
        if (appendDoc) {
            xmlFreeDoc(appendDoc);
        } else if (appendNode) {
            xmlFreeNodeList(appendNode);
        }
        if (resdata) {
            xmlFree(resdata);
        }
        if (doc) {
            xmlFreeDoc(doc);
        }

        pg_xml_done(xmlerrcxt, true);

        PG_RE_THROW();
    }
    PG_END_TRY();
    res = pstrdup((char*)resdata);
    xmlFree(resdata);
    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathctx);
    if (appendDoc) {
        xmlFreeDoc(appendDoc);
    } else if (appendNode) {
        xmlFreeNodeList(appendNode);
    }
    xmlFreeDoc(doc);

    pg_xml_done(xmlerrcxt, false);

    return res;
}

#endif /* USE_LIBXML */

static void XMLTYPE_SUPPORT()
{
    if (u_sess->attr.attr_sql.sql_compatibility != A_FORMAT) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION),
                errmsg("Functions for xmltype are only supported in database which dbcompatibility = 'A'.")));
    }
}

Datum xmltype_extract(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    if (PG_ARGISNULL(1)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("Invalid XPATH expression")));
    }
    text* xpath_expr_text = PG_GETARG_TEXT_P(1);
    xmltype* data = PG_GETARG_XML_P(0);
    ArrayType* namespaces = PG_GETARG_ARRAYTYPE_P(2);
    int res_nitems = 0;
    ArrayBuildState* astate = NULL;

    extract_internal(data, xpath_expr_text, namespaces, &res_nitems, &astate);
    if (res_nitems == 0) {
        PG_RETURN_NULL();
    } else {
        StringInfoData buf;
        initStringInfo(&buf);

        for (int i = 0; i < astate->nelems; ++i) {
            if (astate->dnulls && astate->dnulls[i]) {
                continue;
            } else {
                xmltype* xml = (xmltype*)astate->dvalues[i];
                char* xmlchar = text_to_cstring((text*)xml);
                appendStringInfo(&buf, "%s\n", xmlchar);
            }
        }
        buf.data[buf.len - 1] = '\0';
        buf.len--;
        PG_RETURN_XML_P(stringinfo_to_xmltype(&buf));
    }
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xmltype_extractvalue(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    text* xpath_expr_text = PG_GETARG_TEXT_P(1);
    xmltype* data = PG_GETARG_XML_P(0);
    ArrayType* namespaces = PG_GETARG_ARRAYTYPE_P(2);

    char* resvalue = NULL;
    resvalue = xpath_extractvalue(data, xpath_expr_text, namespaces);
    if (resvalue == NULL) {
        PG_RETURN_NULL();
    }

    PG_RETURN_CSTRING(cstring_to_text((char*)resvalue));
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xmltype_xmlsequence(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    if (PG_ARGISNULL(0)) {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(XMLOID));
    }
    xmltype* data = PG_GETARG_XML_P(0);
    ArrayBuildState* astate = NULL;
    MemoryContext oldcontext = CurrentMemoryContext;

    PG_TRY();
    {
        xmlDocPtr doc = NULL;
        doc = xml_parse(data, XMLOPTION_DOCUMENT, true, GetDatabaseEncoding());
        xmlFreeDoc(doc);
        astate = accumArrayResult(astate, PointerGetDatum(data), false, XMLOID, CurrentMemoryContext);
        if (astate->nelems == 0) {
            PG_RETURN_ARRAYTYPE_P(construct_empty_array(XMLOID));
        } else {
            PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate, CurrentMemoryContext));
        }
    }
    PG_CATCH();
    {
        FlushErrorState();
        MemoryContextSwitchTo(oldcontext);
        int res_nitems = 0;
        StringInfoData buf;
        initStringInfo(&buf);

        char* node_open = "<begin>";
        char* node_close = "</begin>";
        appendStringInfo(&buf, "%s", node_open);
        appendStringInfo(&buf, "%s", text_to_cstring((text*)data));
        appendStringInfo(&buf, "%s", node_close);

        xmltype* xmlbuf = stringinfo_to_xmltype(&buf);

        char* xpath_root_node = "/begin/*";
        text* xpath_root_path = cstring_to_text(xpath_root_node);
        extract_internal(xmlbuf, xpath_root_path, NULL, &res_nitems, &astate);
        if (res_nitems == 0) {
            PG_RETURN_ARRAYTYPE_P(construct_empty_array(XMLOID));
        } else {
            PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate, CurrentMemoryContext));
        }
    }
    PG_END_TRY();
    return 0;
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xmltype_xmlsequence_array(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(0));
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xmltype_existsnode(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    text* xpath_expr_text = PG_GETARG_TEXT_P(1);
    xmltype* data = PG_GETARG_XML_P(0);
    int res_nitems = 0;

    extract_internal(data, xpath_expr_text, NULL, &res_nitems, NULL);

    if (res_nitems > 0) {
        PG_RETURN_UINT8(1);
    } else {
        PG_RETURN_UINT8(0);
    }
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xmltype_getstringval(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    xmltype* data = PG_GETARG_XML_P(0);
    PG_RETURN_VARCHAR_P((VarChar*)data);
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xmltype_getstringval_array(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    ArrayType* v = PG_GETARG_ARRAYTYPE_P(0);
    if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM) {
        PG_RETURN_NULL();
    }
    VarChar* resvalue;
    FunctionCallInfoData info;

    InitFunctionCallInfoData(info, fcinfo->flinfo, 2, InvalidOid, NULL, NULL);
    info.arg[0] = fcinfo->arg[0];
    info.arg[1] = CStringGetTextDatum(" \n");
    info.argnull[0] = false;
    info.argnull[1] = false;

    resvalue = (VarChar*)DatumGetPointer(array_to_text(&info));
    PG_RETURN_VARCHAR_P(resvalue);
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

Datum xmltype_appendchildxml(PG_FUNCTION_ARGS)
{
    XMLTYPE_SUPPORT();
#ifdef USE_LIBXML
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_XML_DOCUMENT),
                errmsg("invaild xml tag, got no context")));
    }
    if (PG_ARGISNULL(1)) {
        PG_RETURN_NULL();
    }
    if (PG_ARGISNULL(2)) {
        PG_RETURN_XML_P(PG_GETARG_XML_P(0));
    }
    xmltype* data = PG_GETARG_XML_P(0);
    text* xpath_expr_text = PG_GETARG_TEXT_P(1);
    text* appendData = PG_GETARG_TEXT_P(2);
    ArrayType* namespaces = NULL;

    if (!PG_ARGISNULL(3)) {
        namespaces = PG_GETARG_ARRAYTYPE_P(3);
    }
    char* res = appendNewChildXml(data, xpath_expr_text, namespaces, appendData);

    PG_RETURN_XML_P(cstring_to_xmltype(res));
#else
    NO_XML_SUPPORT();
    return 0;
#endif /* USE_LIBXML */
}
/*
 * Evaluate XPath expression and return array of XML values.
 *
 * As we have no support of XQuery sequences yet, this function seems
 * to be the most useful one (array of XML functions plays a role of
 * some kind of substitution for XQuery sequences).
 */
Datum xpath(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    text* xpath_expr_text = PG_GETARG_TEXT_P(0);
    xmltype* data = PG_GETARG_XML_P(1);
    ArrayType* namespaces = PG_GETARG_ARRAYTYPE_P(2);
    int res_nitems;
    ArrayBuildState* astate = NULL;

    xpath_internal(xpath_expr_text, data, namespaces, &res_nitems, &astate);

    if (res_nitems == 0)
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(XMLOID));
    else
        PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate, CurrentMemoryContext));
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

/*
 * Determines if the node specified by the supplied XPath exists
 * in a given XML document, returning a boolean.
 */
Datum xmlexists(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    text* xpath_expr_text = PG_GETARG_TEXT_P(0);
    xmltype* data = PG_GETARG_XML_P(1);
    int res_nitems;

    xpath_internal(xpath_expr_text, data, NULL, &res_nitems, NULL);

    PG_RETURN_BOOL(res_nitems > 0);
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

/*
 * Determines if the node specified by the supplied XPath exists
 * in a given XML document, returning a boolean. Differs from
 * xmlexists as it supports namespaces and is not defined in SQL/XML.
 */
Datum xpath_exists(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    text* xpath_expr_text = PG_GETARG_TEXT_P(0);
    xmltype* data = PG_GETARG_XML_P(1);
    ArrayType* namespaces = PG_GETARG_ARRAYTYPE_P(2);
    int res_nitems;

    xpath_internal(xpath_expr_text, data, namespaces, &res_nitems, NULL);

    PG_RETURN_BOOL(res_nitems > 0);
#else
    NO_XML_SUPPORT();
    return 0;
#endif
}

/*
 * Functions for checking well-formed-ness
 */

#ifdef USE_LIBXML
static bool wellformed_xml(text* data, XmlOptionType xmloption_arg)
{
    bool result = false;
    volatile xmlDocPtr doc = NULL;

    /* We want to catch any exceptions and return false */
    PG_TRY();
    {
        doc = xml_parse(data, xmloption_arg, true, GetDatabaseEncoding());
        result = true;
    }
    PG_CATCH();
    {
        FlushErrorState();
        result = false;
    }
    PG_END_TRY();

    if (doc)
        xmlFreeDoc(doc);

    return result;
}
#endif

Datum xml_is_well_formed(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    text* data = PG_GETARG_TEXT_P(0);

    PG_RETURN_BOOL(wellformed_xml(data, (XmlOptionType)xmloption));
#else
    NO_XML_SUPPORT();
    return 0;
#endif /* not USE_LIBXML */
}

Datum xml_is_well_formed_document(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    text* data = PG_GETARG_TEXT_P(0);

    PG_RETURN_BOOL(wellformed_xml(data, XMLOPTION_DOCUMENT));
#else
    NO_XML_SUPPORT();
    return 0;
#endif /* not USE_LIBXML */
}

Datum xml_is_well_formed_content(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
    text* data = PG_GETARG_TEXT_P(0);

    PG_RETURN_BOOL(wellformed_xml(data, XMLOPTION_CONTENT));
#else
    NO_XML_SUPPORT();
    return 0;
#endif /* not USE_LIBXML */
}
