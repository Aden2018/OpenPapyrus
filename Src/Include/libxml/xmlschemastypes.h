/*
 * Summary: implementation of XML Schema Datatypes
 * Description: module providing the XML Schema Datatypes implementation
 *         both definition and validity checking
 *
 * Copy: See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#ifndef __XML_SCHEMA_TYPES_H__
#define __XML_SCHEMA_TYPES_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_SCHEMAS_ENABLED

#include <libxml/schemasInternals.h>
#include <libxml/xmlschemas.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	XML_SCHEMA_WHITESPACE_UNKNOWN = 0,
	XML_SCHEMA_WHITESPACE_PRESERVE = 1,
	XML_SCHEMA_WHITESPACE_REPLACE = 2,
	XML_SCHEMA_WHITESPACE_COLLAPSE = 3
} xmlSchemaWhitespaceValueType;

XMLPUBFUN void XMLCALL xmlSchemaInitTypes();
XMLPUBFUN void XMLCALL xmlSchemaCleanupTypes();
XMLPUBFUN xmlSchemaTypePtr XMLCALL xmlSchemaGetPredefinedType(const xmlChar * name, const xmlChar * ns);
XMLPUBFUN int XMLCALL xmlSchemaValidatePredefinedType(xmlSchemaType * type, const xmlChar * value, xmlSchemaValPtr * val);
XMLPUBFUN int XMLCALL xmlSchemaValPredefTypeNode(xmlSchemaType * type, const xmlChar * value, xmlSchemaValPtr * val, xmlNode * P_Node);
XMLPUBFUN int XMLCALL xmlSchemaValidateFacet(xmlSchemaTypePtr base, xmlSchemaFacetPtr facet, const xmlChar * value, xmlSchemaValPtr val);
XMLPUBFUN int XMLCALL xmlSchemaValidateFacetWhtsp(xmlSchemaFacetPtr facet, xmlSchemaWhitespaceValueType fws, xmlSchemaValType valType, 
	const xmlChar * value, xmlSchemaValPtr val, xmlSchemaWhitespaceValueType ws);
XMLPUBFUN void /*XMLCALL*/FASTCALL xmlSchemaFreeValue(xmlSchemaVal * pVal);
XMLPUBFUN xmlSchemaFacetPtr XMLCALL xmlSchemaNewFacet();
XMLPUBFUN int XMLCALL xmlSchemaCheckFacet(xmlSchemaFacetPtr facet, xmlSchemaTypePtr typeDecl, xmlSchemaParserCtxtPtr ctxt, const xmlChar * name);
XMLPUBFUN void /*XMLCALL*/FASTCALL xmlSchemaFreeFacet(xmlSchemaFacetPtr facet);
XMLPUBFUN int /*XMLCALL*/FASTCALL xmlSchemaCompareValues(xmlSchemaValPtr x, xmlSchemaValPtr y);
XMLPUBFUN xmlSchemaTypePtr XMLCALL xmlSchemaGetBuiltInListSimpleTypeItemType(xmlSchemaType * type);
XMLPUBFUN int XMLCALL xmlSchemaValidateListSimpleTypeFacet(xmlSchemaFacetPtr facet,
    const xmlChar * value, ulong actualLen, ulong * expectedLen);
XMLPUBFUN xmlSchemaTypePtr /*XMLCALL*/FASTCALL xmlSchemaGetBuiltInType(xmlSchemaValType type);
XMLPUBFUN int XMLCALL xmlSchemaIsBuiltInTypeFacet(xmlSchemaType * type, int facetType);
XMLPUBFUN xmlChar * XMLCALL xmlSchemaCollapseString(const xmlChar * value);
XMLPUBFUN xmlChar * XMLCALL xmlSchemaWhiteSpaceReplace(const xmlChar * value);
XMLPUBFUN ulong XMLCALL xmlSchemaGetFacetValueAsULong(xmlSchemaFacetPtr facet);
XMLPUBFUN int XMLCALL xmlSchemaValidateLengthFacet(xmlSchemaType * type, xmlSchemaFacetPtr facet, const xmlChar * value,
    xmlSchemaValPtr val, ulong * length);
XMLPUBFUN int XMLCALL xmlSchemaValidateLengthFacetWhtsp(xmlSchemaFacetPtr facet, xmlSchemaValType valType, const xmlChar * value,
    xmlSchemaValPtr val, ulong * length, xmlSchemaWhitespaceValueType ws);
XMLPUBFUN int XMLCALL xmlSchemaValPredefTypeNodeNoNorm(xmlSchemaType * type, const xmlChar * value, xmlSchemaValPtr * val, xmlNode * P_Node);
XMLPUBFUN int XMLCALL xmlSchemaGetCanonValue(xmlSchemaVal * val, xmlChar ** retValue);
XMLPUBFUN int XMLCALL xmlSchemaGetCanonValueWhtsp(xmlSchemaVal * val, xmlChar ** retValue, xmlSchemaWhitespaceValueType ws);
XMLPUBFUN int XMLCALL xmlSchemaValueAppend(xmlSchemaValPtr prev, xmlSchemaValPtr cur);
XMLPUBFUN xmlSchemaValPtr XMLCALL xmlSchemaValueGetNext(xmlSchemaValPtr cur);
XMLPUBFUN const xmlChar * XMLCALL xmlSchemaValueGetAsString(xmlSchemaValPtr val);
XMLPUBFUN int XMLCALL xmlSchemaValueGetAsBoolean(xmlSchemaValPtr val);
XMLPUBFUN xmlSchemaValPtr XMLCALL xmlSchemaNewStringValue(xmlSchemaValType type, const xmlChar * value);
XMLPUBFUN xmlSchemaValPtr XMLCALL xmlSchemaNewNOTATIONValue(const xmlChar * name, const xmlChar * ns);
XMLPUBFUN xmlSchemaValPtr XMLCALL xmlSchemaNewQNameValue(const xmlChar * namespaceName, const xmlChar * localName);
XMLPUBFUN int XMLCALL xmlSchemaCompareValuesWhtsp(xmlSchemaValPtr x, xmlSchemaWhitespaceValueType xws, xmlSchemaValPtr y, xmlSchemaWhitespaceValueType yws);
XMLPUBFUN xmlSchemaValPtr XMLCALL xmlSchemaCopyValue(xmlSchemaValPtr val);
XMLPUBFUN xmlSchemaValType XMLCALL xmlSchemaGetValType(xmlSchemaValPtr val);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_SCHEMAS_ENABLED */
#endif /* __XML_SCHEMA_TYPES_H__ */
