/*
 * schemas.c : implementation of the XML Schema handling and
 *             schema validity checking
 *
 * See Copyright for the status of this software.
 *
 * Daniel Veillard <veillard@redhat.com>
 */

/*
 * @todo 
 *   - when types are redefined in includes, check that all
 *     types in the redef list are equal
 *     -> need a type equality operation.
 *   - if we don't intend to use the schema for schemas, we
 *     need to validate all schema attributes (ref, type, name)
 *     against their types.
 *   - Eliminate item creation for: ??
 *
 * URGENT TODO:
 *   - For xsi-driven schema acquisition, augment the IDCs after every
 *     acquisition episode (xmlSchemaAugmentIDC).
 *
 * NOTES:
 *   - Elimated item creation for: <restriction>, <extension>,
 *     <simpleContent>, <complexContent>, <list>, <union>
 *
 * PROBLEMS:
 *   - http://lists.w3.org/Archives/Public/www-xml-schema-comments/2005JulSep/0337.html
 *     IDC XPath expression and chameleon includes: the targetNamespace is changed, so
 *     XPath will have trouble to resolve to this namespace, since not known.
 *
 *
 * CONSTRAINTS:
 *
 * Schema Component Constraint:
 *   All Group Limited (cos-all-limited)
 *   Status: complete
 *   (1.2)
 *     In xmlSchemaGroupDefReferenceTermFixup() and
 *   (2)
 *     In xmlSchemaParseModelGroup()
 *     TODO: Actually this should go to component-level checks,
 *     but is done here due to performance. Move it to an other layer
 *     is schema construction via an API is implemented.
 */
#define IN_LIBXML
#include "libxml.h"
#pragma hdrstop

#ifdef LIBXML_SCHEMAS_ENABLED

#include <libxml/xmlschemas.h>
#include <libxml/schemasInternals.h>
#include <libxml/xmlschemastypes.h>
#include <libxml/xmlautomata.h>
#include <libxml/xmlregexp.h>
//#include <libxml/dict.h>
#ifdef LIBXML_PATTERN_ENABLED
	#include <libxml/pattern.h>
#endif
#ifdef LIBXML_READER_ENABLED
	#include <libxml/xmlreader.h>
#endif

/* #define DEBUG 1 */
/* #define DEBUG_CONTENT 1 */
/* #define DEBUG_TYPE 1 */
/* #define DEBUG_CONTENT_REGEXP 1 */
/* #define DEBUG_AUTOMATA 1 */
/* #define DEBUG_IDC */
/* #define DEBUG_IDC_NODE_TABLE */
/* #define WXS_ELEM_DECL_CONS_ENABLED */

#ifdef DEBUG_IDC
 #ifndef DEBUG_IDC_NODE_TABLE
  #define DEBUG_IDC_NODE_TABLE
 #endif
#endif

/* #define ENABLE_PARTICLE_RESTRICTION 1 */

#define ENABLE_REDEFINE

/* #define ENABLE_NAMED_LOCALS */

/* #define ENABLE_IDC_NODE_TABLES_TEST */

#define DUMP_CONTENT_MODEL

#ifdef LIBXML_READER_ENABLED
/* #define XML_SCHEMA_READER_ENABLED */
#endif

#define UNBOUNDED (1 << 30)
#define TODO xmlGenericError(0, "Unimplemented block at %s:%d\n", __FILE__, __LINE__);
#define XML_SCHEMAS_NO_NAMESPACE (const xmlChar*)"##"

/*
 * The XML Schemas namespaces
 */
static const xmlChar * xmlSchemaNs = (const xmlChar*)"http://www.w3.org/2001/XMLSchema";
static const xmlChar * xmlSchemaInstanceNs = (const xmlChar*)"http://www.w3.org/2001/XMLSchema-instance";
static const xmlChar * xmlNamespaceNs = (const xmlChar*)"http://www.w3.org/2000/xmlns/";
/*
 * Come casting macros.
 */
#define ACTXT_CAST (xmlSchemaAbstractCtxtPtr)
#define PCTXT_CAST (xmlSchemaParserCtxtPtr)
#define VCTXT_CAST (xmlSchemaValidCtxtPtr)
#define WXS_BASIC_CAST (xmlSchemaBasicItem *) 
#define WXS_TREE_CAST (xmlSchemaTreeItem *)
#define WXS_PTC_CAST (xmlSchemaParticlePtr)
#define WXS_TYPE_CAST (xmlSchemaTypePtr)
#define WXS_ELEM_CAST (xmlSchemaElementPtr)
#define WXS_ATTR_GROUP_CAST (xmlSchemaAttributeGroupPtr)
#define WXS_ATTR_CAST (xmlSchemaAttributePtr)
#define WXS_ATTR_USE_CAST (xmlSchemaAttributeUsePtr)
#define WXS_ATTR_PROHIB_CAST (xmlSchemaAttributeUseProhibPtr)
#define WXS_MODEL_GROUPDEF_CAST (xmlSchemaModelGroupDefPtr)
#define WXS_MODEL_GROUP_CAST (xmlSchemaModelGroupPtr)
#define WXS_IDC_CAST (xmlSchemaIDCPtr)
#define WXS_QNAME_CAST (xmlSchemaQNameRefPtr)
#define WXS_LIST_CAST (xmlSchemaItemListPtr)

/*
 * Macros to query common properties of components.
 */
#define WXS_ITEM_NODE(i) xmlSchemaGetComponentNode(WXS_BASIC_CAST(i))
#define WXS_ITEM_TYPE_NAME(i) xmlSchemaGetComponentTypeStr(WXS_BASIC_CAST(i))
/*
 * Macros for element declarations.
 */
#define WXS_ELEM_TYPEDEF(e) (e)->subtypes
#define WXS_SUBST_HEAD(item) (item)->refDecl
/*
 * Macros for attribute declarations.
 */
#define WXS_ATTR_TYPEDEF(a) (a)->subtypes
/*
 * Macros for attribute uses.
 */
#define WXS_ATTRUSE_DECL(au) WXS_ATTR_CAST(WXS_ATTR_USE_CAST(au))->attrDecl
#define WXS_ATTRUSE_TYPEDEF(au) WXS_ATTR_TYPEDEF(WXS_ATTRUSE_DECL(WXS_ATTR_USE_CAST au))
#define WXS_ATTRUSE_DECL_NAME(au) (WXS_ATTRUSE_DECL(au))->name
#define WXS_ATTRUSE_DECL_TNS(au) (WXS_ATTRUSE_DECL(au))->targetNamespace
/*
 * Macros for attribute groups.
 */
#define WXS_ATTR_GROUP_HAS_REFS(ag) ((WXS_ATTR_GROUP_CAST(ag))->flags & XML_SCHEMAS_ATTRGROUP_HAS_REFS)
#define WXS_ATTR_GROUP_EXPANDED(ag) ((WXS_ATTR_GROUP_CAST(ag))->flags & XML_SCHEMAS_ATTRGROUP_WILDCARD_BUILDED)
/*
 * Macros for particles.
 */
#define WXS_PARTICLE(p) WXS_PTC_CAST(p)
#define WXS_PARTICLE_TERM(p) (WXS_PARTICLE(p))->children
#define WXS_PARTICLE_TERM_AS_ELEM(p) (WXS_ELEM_CAST WXS_PARTICLE_TERM(p))
#define WXS_PARTICLE_MODEL(p) WXS_MODEL_GROUP_CAST WXS_PARTICLE(p)->children
/*
 * Macros for model groups definitions.
 */
#define WXS_MODELGROUPDEF_MODEL(mgd) (WXS_MODEL_GROUP_CAST(mgd))->children
/*
 * Macros for model groups.
 */
#define WXS_IS_MODEL_GROUP(i) (((i)->type == XML_SCHEMA_TYPE_SEQUENCE) || ((i)->type == XML_SCHEMA_TYPE_CHOICE) || ((i)->type == XML_SCHEMA_TYPE_ALL))
#define WXS_MODELGROUP_PARTICLE(mg) WXS_PTC_CAST(mg)->children
/*
 * Macros for schema buckets.
 */
#define WXS_IS_BUCKET_INCREDEF(t) (((t) == XML_SCHEMA_SCHEMA_INCLUDE) || ((t) == XML_SCHEMA_SCHEMA_REDEFINE))
#define WXS_IS_BUCKET_IMPMAIN(t) (((t) == XML_SCHEMA_SCHEMA_MAIN) || ((t) == XML_SCHEMA_SCHEMA_IMPORT))
#define WXS_IMPBUCKET(b) ((xmlSchemaImportPtr)(b))
#define WXS_INCBUCKET(b) ((xmlSchemaIncludePtr)(b))
/*
 * Macros for complex/simple types.
 */
#define WXS_IS_ANYTYPE(i) (((i)->type == XML_SCHEMA_TYPE_BASIC) && ((WXS_TYPE_CAST(i))->builtInType == XML_SCHEMAS_ANYTYPE))
#define WXS_IS_COMPLEX(i) (((i)->type == XML_SCHEMA_TYPE_COMPLEX) || ((i)->builtInType == XML_SCHEMAS_ANYTYPE))
#define WXS_IS_SIMPLE(item) ((item->type == XML_SCHEMA_TYPE_SIMPLE) || ((item->type == XML_SCHEMA_TYPE_BASIC) && (item->builtInType != XML_SCHEMAS_ANYTYPE)))
#define WXS_IS_ANY_SIMPLE_TYPE(i) (((i)->type == XML_SCHEMA_TYPE_BASIC) && ((i)->builtInType == XML_SCHEMAS_ANYSIMPLETYPE))
#define WXS_IS_RESTRICTION(t)     ((t)->flags & XML_SCHEMAS_TYPE_DERIVATION_METHOD_RESTRICTION)
#define WXS_IS_EXTENSION(t)       ((t)->flags & XML_SCHEMAS_TYPE_DERIVATION_METHOD_EXTENSION)
#define WXS_IS_TYPE_NOT_FIXED(i)  (((i)->type != XML_SCHEMA_TYPE_BASIC) && (((i)->flags & XML_SCHEMAS_TYPE_INTERNAL_RESOLVED) == 0))
#define WXS_IS_TYPE_NOT_FIXED_1(item) (((item)->type != XML_SCHEMA_TYPE_BASIC) && (((item)->flags & XML_SCHEMAS_TYPE_FIXUP_1) == 0))
#define WXS_TYPE_IS_GLOBAL(t) ((t)->flags & XML_SCHEMAS_TYPE_GLOBAL)
#define WXS_TYPE_IS_LOCAL(t) (((t)->flags & XML_SCHEMAS_TYPE_GLOBAL) == 0)
/*
 * Macros for exclusively for complex types.
 */
#define WXS_HAS_COMPLEX_CONTENT(item) (oneof3(item->contentType, XML_SCHEMA_CONTENT_MIXED, XML_SCHEMA_CONTENT_EMPTY, XML_SCHEMA_CONTENT_ELEMENTS))
#define WXS_HAS_SIMPLE_CONTENT(item)  (oneof2(item->contentType, XML_SCHEMA_CONTENT_SIMPLE, XML_SCHEMA_CONTENT_BASIC))
#define WXS_HAS_MIXED_CONTENT(item)   (item->contentType == XML_SCHEMA_CONTENT_MIXED)
#define WXS_EMPTIABLE(t) (xmlSchemaIsParticleEmptiable(WXS_PTC_CAST(t)->subtypes))
#define WXS_TYPE_CONTENTTYPE(t) (t)->subtypes
#define WXS_TYPE_PARTICLE(t) WXS_PTC_CAST(t)->subtypes
#define WXS_TYPE_PARTICLE_TERM(t) WXS_PARTICLE_TERM(WXS_TYPE_PARTICLE(t))
/*
 * Macros for exclusively for simple types.
 */
#define WXS_LIST_ITEMTYPE(t) (t)->subtypes
#define WXS_IS_ATOMIC(t) (t->flags & XML_SCHEMAS_TYPE_VARIETY_ATOMIC)
#define WXS_IS_LIST(t) (t->flags & XML_SCHEMAS_TYPE_VARIETY_LIST)
#define WXS_IS_UNION(t) (t->flags & XML_SCHEMAS_TYPE_VARIETY_UNION)
/*
 * Misc parser context macros.
 */
#define WXS_CONSTRUCTOR(ctx) (ctx)->constructor
#define WXS_HAS_BUCKETS(ctx) ((WXS_CONSTRUCTOR((ctx))->buckets != NULL) && (WXS_CONSTRUCTOR((ctx))->buckets->nbItems > 0))
#define WXS_SUBST_GROUPS(ctx) WXS_CONSTRUCTOR((ctx))->substGroups
#define WXS_BUCKET(ctx) WXS_CONSTRUCTOR((ctx))->bucket
#define WXS_SCHEMA(ctx) (ctx)->schema
#define WXS_ADD_LOCAL(ctx, item) xmlSchemaAddItemSize(&(WXS_BUCKET(ctx)->locals), 10, item)
#define WXS_ADD_GLOBAL(ctx, item) xmlSchemaAddItemSize(&(WXS_BUCKET(ctx)->globals), 5, item)
#define WXS_ADD_PENDING(ctx, item) xmlSchemaAddItemSize(&((ctx)->constructor->pending), 10, item)
/*
 * xmlSchemaItemList macros.
 */
#define WXS_ILIST_IS_EMPTY(l) ((l == NULL) || ((l)->nbItems == 0))
// 
// Misc macros.
// 
static int FASTCALL _IsSchema(const xmlNode * pNode, const char * pType)
{
	return (pNode && pNode->ns && sstreq(pNode->name, pType) && sstreq(pNode->ns->href, xmlSchemaNs));
}

//#define IS_SCHEMA(node, type) (node && node->ns && sstreq(node->name, type) && sstreq(node->ns->href, xmlSchemaNs))
#define IS_SCHEMA(P_Node, type) _IsSchema(P_Node, type)

// @sobolev #define FREE_AND_NULL_Removed(str) if((str) != NULL) { SAlloc::F((xmlChar*)(str)); str = NULL; }
/*
 * Since we put the default/fixed values into the dict, we can
 * use pointer comparison for those values.
 * REMOVED: (sstreq((v1), (v2)))
 */
#define WXS_ARE_DEFAULT_STR_EQUAL(v1, v2) ((v1) == (v2))
#define INODE_NILLED(item) (item->flags & XML_SCHEMA_ELEM_INFO_NILLED)
#define CAN_PARSE_SCHEMA(b) (((b)->doc != NULL) && ((b)->parsed == 0))
#define HFAILURE if(res == -1) goto exit_failure;
#define HERROR if(res != 0) goto exit_error;
#define HSTOP(ctx) if((ctx)->stop) goto exit;
/*
 * Some flags used for various schema constraints.
 */
#define SUBSET_RESTRICTION  1<<0
#define SUBSET_EXTENSION    1<<1
#define SUBSET_SUBSTITUTION 1<<2
#define SUBSET_LIST         1<<3
#define SUBSET_UNION        1<<4

typedef struct _xmlSchemaNodeInfo xmlSchemaNodeInfo;
typedef xmlSchemaNodeInfo * xmlSchemaNodeInfoPtr;

typedef struct _xmlSchemaItemList xmlSchemaItemList;
typedef xmlSchemaItemList * xmlSchemaItemListPtr;
struct _xmlSchemaItemList {
	void ** items; /* used for dynamic addition of schemata */
	int nbItems; /* used for dynamic addition of schemata */
	int sizeItems; /* used for dynamic addition of schemata */
};

#define XML_SCHEMA_CTXT_PARSER 1
#define XML_SCHEMA_CTXT_VALIDATOR 2

typedef struct _xmlSchemaAbstractCtxt xmlSchemaAbstractCtxt;
typedef xmlSchemaAbstractCtxt * xmlSchemaAbstractCtxtPtr;
struct _xmlSchemaAbstractCtxt {
	int type; /* E.g. XML_SCHEMA_CTXT_VALIDATOR */
};

typedef struct _xmlSchemaBucket xmlSchemaBucket;
typedef xmlSchemaBucket * xmlSchemaBucketPtr;

#define XML_SCHEMA_SCHEMA_MAIN 0
#define XML_SCHEMA_SCHEMA_IMPORT 1
#define XML_SCHEMA_SCHEMA_INCLUDE 2
#define XML_SCHEMA_SCHEMA_REDEFINE 3

/**
 * xmlSchemaSchemaRelation:
 *
 * Used to create a graph of schema relationships.
 */
typedef struct _xmlSchemaSchemaRelation xmlSchemaSchemaRelation;
typedef xmlSchemaSchemaRelation * xmlSchemaSchemaRelationPtr;
struct _xmlSchemaSchemaRelation {
	xmlSchemaSchemaRelationPtr next;
	int type; /* E.g. XML_SCHEMA_SCHEMA_IMPORT */
	const xmlChar * importNamespace;
	xmlSchemaBucketPtr bucket;
};

#define XML_SCHEMA_BUCKET_MARKED 1<<0
#define XML_SCHEMA_BUCKET_COMPS_ADDED 1<<1

struct _xmlSchemaBucket {
	int type;
	int flags;
	const xmlChar * schemaLocation;
	const xmlChar * origTargetNamespace;
	const xmlChar * targetNamespace;
	xmlDocPtr doc;
	xmlSchemaSchemaRelationPtr relations;
	int located;
	int parsed;
	int imported;
	int preserveDoc;
	xmlSchemaItemListPtr globals; /* Global components. */
	xmlSchemaItemListPtr locals; /* Local components. */
};
/**
 * xmlSchemaImport:
 * (extends xmlSchemaBucket)
 *
 * Reflects a schema. Holds some information
 * about the schema and its toplevel components. Duplicate
 * toplevel components are not checked at this level.
 */
//typedef struct _xmlSchemaImport xmlSchemaImport;

struct xmlSchemaImport {
	int type; /* Main OR import OR include. */
	int flags;
	const xmlChar * schemaLocation; /* The URI of the schema document. */
	/* For chameleon includes, @origTargetNamespace will be NULL */
	const xmlChar * origTargetNamespace;
	/*
	 * For chameleon includes, @targetNamespace will be the
	 * targetNamespace of the including schema.
	 */
	const xmlChar * targetNamespace;
	xmlDocPtr doc; /* The schema node-tree. */
	xmlSchemaSchemaRelationPtr relations; // @relations will hold any included/imported/redefined schemas
	int located;
	int parsed;
	int imported;
	int preserveDoc;
	xmlSchemaItemListPtr globals;
	xmlSchemaItemListPtr locals;
	xmlSchemaPtr schema; // The imported schema
};

typedef xmlSchemaImport * xmlSchemaImportPtr;
/*
 * (extends xmlSchemaBucket)
 */
//typedef struct _xmlSchemaInclude xmlSchemaInclude;

struct xmlSchemaInclude {
	int    type;
	int    flags;
	const  xmlChar * schemaLocation;
	const  xmlChar * origTargetNamespace;
	const  xmlChar * targetNamespace;
	xmlDoc * doc;
	xmlSchemaSchemaRelationPtr relations;
	int    located;
	int    parsed;
	int    imported;
	int    preserveDoc;
	xmlSchemaItemListPtr globals; /* Global components. */
	xmlSchemaItemListPtr locals; /* Local components. */
	xmlSchemaImportPtr ownerImport; // The owning main or import schema bucket
};

typedef xmlSchemaInclude * xmlSchemaIncludePtr;

/**
 * xmlSchemaBasicItem:
 *
 * The abstract base type for schema components.
 */
//typedef struct _xmlSchemaBasicItem xmlSchemaBasicItem;
struct xmlSchemaBasicItem {
	xmlSchemaTypeType type;
};

//typedef xmlSchemaBasicItem * xmlSchemaBasicItemPtr;
/**
 * xmlSchemaAnnotItem:
 *
 * The abstract base type for annotated schema components.
 * (Extends xmlSchemaBasicItem)
 */
//typedef struct _xmlSchemaAnnotItem xmlSchemaAnnotItem;
struct xmlSchemaAnnotItem {
	xmlSchemaTypeType type;
	xmlSchemaAnnot * annot;
};

//typedef xmlSchemaAnnotItem * xmlSchemaAnnotItemPtr;
/**
 * xmlSchemaTreeItem:
 *
 * The abstract base type for tree-like structured schema components.
 * (Extends xmlSchemaAnnotItem)
 */
//typedef struct _xmlSchemaTreeItem xmlSchemaTreeItem;
struct xmlSchemaTreeItem {
	xmlSchemaTypeType type;
	xmlSchemaAnnot * annot;
	xmlSchemaTreeItem * next;
	xmlSchemaTreeItem * children;
};

//typedef xmlSchemaTreeItem * xmlSchemaTreeItemPtr;

#define XML_SCHEMA_ATTR_USE_FIXED 1<<0
/**
 * xmlSchemaAttributeUsePtr:
 *
 * The abstract base type for tree-like structured schema components.
 * (Extends xmlSchemaTreeItem)
 */
typedef struct _xmlSchemaAttributeUse xmlSchemaAttributeUse;
typedef xmlSchemaAttributeUse * xmlSchemaAttributeUsePtr;
struct _xmlSchemaAttributeUse {
	xmlSchemaTypeType type;
	xmlSchemaAnnotPtr annot;
	xmlSchemaAttributeUsePtr next; /* The next attr. use. */
	/*
	 * The attr. decl. OR a QName-ref. to an attr. decl. OR
	 * a QName-ref. to an attribute group definition.
	 */
	xmlSchemaAttributePtr attrDecl;

	int flags;
	xmlNode * P_Node;
	int occurs; /* required, optional */
	const xmlChar * defValue;
	xmlSchemaValPtr defVal;
};

/**
 * xmlSchemaAttributeUseProhibPtr:
 *
 * A helper component to reflect attribute prohibitions.
 * (Extends xmlSchemaBasicItem)
 */
typedef struct _xmlSchemaAttributeUseProhib xmlSchemaAttributeUseProhib;
typedef xmlSchemaAttributeUseProhib * xmlSchemaAttributeUseProhibPtr;
struct _xmlSchemaAttributeUseProhib {
	xmlSchemaTypeType type; /* == XML_SCHEMA_EXTRA_ATTR_USE_PROHIB */
	xmlNode * P_Node;
	const xmlChar * name;
	const xmlChar * targetNamespace;
	int isRef;
};

/**
 * xmlSchemaRedef:
 */
//typedef struct _xmlSchemaRedef xmlSchemaRedef;

struct xmlSchemaRedef {
	xmlSchemaRedef * next;
	xmlSchemaBasicItem * item; // The redefining component.
	xmlSchemaBasicItem * reference;  // The referencing component. 
	xmlSchemaBasicItem * target;  // The to-be-redefined component.
	const xmlChar * refName; /* The name of the to-be-redefined component. */
	const xmlChar * refTargetNs; // The target namespace of the to-be-redefined comp.
	xmlSchemaBucket * targetBucket; // The redefined schema. 
};

typedef xmlSchemaRedef * xmlSchemaRedefPtr;
/**
 * xmlSchemaConstructionCtxt:
 */
typedef struct _xmlSchemaConstructionCtxt xmlSchemaConstructionCtxt;
typedef xmlSchemaConstructionCtxt * xmlSchemaConstructionCtxtPtr;
struct _xmlSchemaConstructionCtxt {
	xmlSchemaPtr mainSchema; /* The main schema. */
	xmlSchemaBucketPtr mainBucket; /* The main schema bucket */
	xmlDict * dict;
	xmlSchemaItemListPtr buckets; /* List of schema buckets. */
	/* xmlSchemaItemListPtr relations; */ /* List of schema relations. */
	xmlSchemaBucketPtr bucket; /* The current schema bucket */
	xmlSchemaItemListPtr pending; // All Components of all schemas that need to be fixed. 
	xmlHashTable * substGroups;
	xmlSchemaRedefPtr redefs;
	xmlSchemaRedefPtr lastRedef;
};

#define XML_SCHEMAS_PARSE_ERROR         1
#define SCHEMAS_PARSE_OPTIONS XML_PARSE_NOENT

struct _xmlSchemaParserCtxt {
	int    type;
	void * errCtxt;        /* user specific error context */
	xmlSchemaValidityErrorFunc F_Error; /* the callback in case of errors */
	xmlSchemaValidityWarningFunc F_Warning;   /* the callback in case of warning */
	int    err;
	int    nberrors;
	xmlStructuredErrorFunc serror;
	xmlSchemaConstructionCtxtPtr constructor;
	int    ownsConstructor; /* @todo Move this to parser *flags*. */
	/* xmlSchemaPtr topschema;	*/
	/* xmlHashTablePtr namespaces;  */
	xmlSchemaPtr schema;    /* The main schema in use */
	int    counter;
	const  xmlChar * URL;
	xmlDocPtr doc;
	int    preserve;           /* Whether the doc should be freed  */
	const  char * buffer;
	int    size;
	/*
	 * Used to build complex element content models
	 */
	xmlAutomataPtr am;
	xmlAutomataStatePtr start;
	xmlAutomataStatePtr end;
	xmlAutomataStatePtr state;
	xmlDict * dict;        /* dictionnary for interned string names */
	xmlSchemaTypePtr ctxtType; /* The current context simple/complex type */
	int    options;
	xmlSchemaValidCtxtPtr vctxt;
	int    isS4S;
	int    isRedefine;
	int    xsiAssemble;
	int    stop; /* If the parser should stop; i.e. a critical error. */
	const  xmlChar * targetNamespace;
	xmlSchemaBucketPtr redefined; /* The schema to be redefined. */
	xmlSchemaRedefPtr redef; /* Used for redefinitions. */
	int    redefCounter; /* Used for redefinitions. */
	xmlSchemaItemListPtr attrProhibs;
};

/**
 * xmlSchemaQNameRef:
 *
 * A component reference item (not a schema component)
 * (Extends xmlSchemaBasicItem)
 */
//typedef struct _xmlSchemaQNameRef xmlSchemaQNameRef;

struct xmlSchemaQNameRef {
	xmlSchemaTypeType type;
	xmlSchemaBasicItem * item; // The resolved referenced item
	xmlSchemaTypeType itemType;
	const xmlChar * name;
	const xmlChar * targetNamespace;
	xmlNode * P_Node;
};

typedef xmlSchemaQNameRef * xmlSchemaQNameRefPtr;
/**
 * xmlSchemaParticle:
 *
 * A particle component.
 * (Extends xmlSchemaTreeItem)
 */
//typedef struct _xmlSchemaParticle xmlSchemaParticle;

struct xmlSchemaParticle {
	xmlSchemaTypeType type;
	xmlSchemaAnnot * annot;
	xmlSchemaTreeItem * next; // next particle 
	xmlSchemaTreeItem * children; // the "term" (e.g. a model group, a group definition, a XML_SCHEMA_EXTRA_QNAMEREF (if a reference), etc.)
	int    minOccurs;
	int    maxOccurs;
	xmlNode * P_Node;
};

typedef xmlSchemaParticle * xmlSchemaParticlePtr;
/**
 * xmlSchemaModelGroup:
 *
 * A model group component.
 * (Extends xmlSchemaTreeItem)
 */
//typedef struct _xmlSchemaModelGroup xmlSchemaModelGroup;

struct xmlSchemaModelGroup {
	xmlSchemaTypeType type; /* XML_SCHEMA_TYPE_SEQUENCE, XML_SCHEMA_TYPE_CHOICE, XML_SCHEMA_TYPE_ALL */
	xmlSchemaAnnot * annot;
	xmlSchemaTreeItem * next; // not used 
	xmlSchemaTreeItem * children; // first particle (OR "element decl" OR "wildcard") 
	xmlNode * P_Node;
};

typedef xmlSchemaModelGroup * xmlSchemaModelGroupPtr;

#define XML_SCHEMA_MODEL_GROUP_DEF_MARKED 1<<0
#define XML_SCHEMA_MODEL_GROUP_DEF_REDEFINED 1<<1
/**
 * xmlSchemaModelGroupDef:
 *
 * A model group definition component.
 * (Extends xmlSchemaTreeItem)
 */
//typedef struct _xmlSchemaModelGroupDef xmlSchemaModelGroupDef;

struct xmlSchemaModelGroupDef {
	xmlSchemaTypeType type; /* XML_SCHEMA_TYPE_GROUP */
	xmlSchemaAnnotPtr annot;
	xmlSchemaTreeItem * next; // not used 
	xmlSchemaTreeItem * children; // the "model group" 
	const xmlChar * name;
	const xmlChar * targetNamespace;
	xmlNode * P_Node;
	int flags;
};

typedef xmlSchemaModelGroupDef * xmlSchemaModelGroupDefPtr;

typedef struct _xmlSchemaIDC xmlSchemaIDC;
typedef xmlSchemaIDC * xmlSchemaIDCPtr;

/**
 * xmlSchemaIDCSelect:
 *
 * The identity-constraint "field" and "selector" item, holding the
 * XPath expression.
 */
typedef struct _xmlSchemaIDCSelect xmlSchemaIDCSelect;
typedef xmlSchemaIDCSelect * xmlSchemaIDCSelectPtr;
struct _xmlSchemaIDCSelect {
	xmlSchemaIDCSelectPtr next;
	xmlSchemaIDCPtr idc;
	int index; /* an index position if significant for IDC key-sequences */
	const xmlChar * xpath; /* the XPath expression */
	void * xpathComp; /* the compiled XPath expression */
};

/**
 * xmlSchemaIDC:
 *
 * The identity-constraint definition component.
 * (Extends xmlSchemaAnnotItem)
 */
struct _xmlSchemaIDC {
	xmlSchemaTypeType type;
	xmlSchemaAnnotPtr annot;
	xmlSchemaIDCPtr next;
	xmlNode * P_Node;
	const xmlChar * name;
	const xmlChar * targetNamespace;
	xmlSchemaIDCSelectPtr selector;
	xmlSchemaIDCSelectPtr fields;
	int nbFields;
	xmlSchemaQNameRefPtr ref;
};

/**
 * xmlSchemaIDCAug:
 *
 * The augmented IDC information used for validation.
 */
typedef struct _xmlSchemaIDCAug xmlSchemaIDCAug;
typedef xmlSchemaIDCAug * xmlSchemaIDCAugPtr;
struct _xmlSchemaIDCAug {
	xmlSchemaIDCAugPtr next; /* next in a list */
	xmlSchemaIDCPtr def; /* the IDC definition */
	int keyrefDepth; // the lowest tree level to which IDC tables need to be bubbled upwards 
};
/**
 * xmlSchemaPSVIIDCKeySequence:
 *
 * The key sequence of a node table item.
 */
//typedef struct _xmlSchemaPSVIIDCKey xmlSchemaPSVIIDCKey;

struct xmlSchemaPSVIIDCKey {
	xmlSchemaType * type;
	xmlSchemaVal  * val;
};

typedef xmlSchemaPSVIIDCKey * xmlSchemaPSVIIDCKeyPtr;
/**
 * xmlSchemaPSVIIDCNode:
 *
 * The node table item of a node table.
 */
//typedef struct _xmlSchemaPSVIIDCNode xmlSchemaPSVIIDCNode;

struct xmlSchemaPSVIIDCNode {
	xmlNode * P_Node;
	xmlSchemaPSVIIDCKeyPtr * keys;
	int nodeLine;
	int nodeQNameID;
};

typedef xmlSchemaPSVIIDCNode * xmlSchemaPSVIIDCNodePtr;
/**
 * xmlSchemaPSVIIDCBinding:
 *
 * The identity-constraint binding item of the [identity-constraint table].
 */
//typedef struct _xmlSchemaPSVIIDCBinding xmlSchemaPSVIIDCBinding;

struct xmlSchemaPSVIIDCBinding {
	xmlSchemaPSVIIDCBinding * next; /* next binding of a specific node */
	xmlSchemaIDC * definition; /* the IDC definition */
	xmlSchemaPSVIIDCNode ** nodeTable; /* array of key-sequences */
	int nbNodes; /* number of entries in the node table */
	int sizeNodes; /* size of the node table */
	xmlSchemaItemList * dupls;
};

typedef xmlSchemaPSVIIDCBinding * xmlSchemaPSVIIDCBindingPtr;

#define XPATH_STATE_OBJ_TYPE_IDC_SELECTOR 1
#define XPATH_STATE_OBJ_TYPE_IDC_FIELD 2

#define XPATH_STATE_OBJ_MATCHES -2
#define XPATH_STATE_OBJ_BLOCKED -3

typedef struct _xmlSchemaIDCMatcher xmlSchemaIDCMatcher;
typedef xmlSchemaIDCMatcher * xmlSchemaIDCMatcherPtr;

/**
 * xmlSchemaIDCStateObj:
 *
 * The state object used to evaluate XPath expressions.
 */
//typedef struct _xmlSchemaIDCStateObj xmlSchemaIDCStateObj;

struct xmlSchemaIDCStateObj {
	int type;
	xmlSchemaIDCStateObj * next; /* next if in a list */
	int depth; /* depth of creation */
	int * history; /* list of (depth, state-id) tuples */
	int nbHistory;
	int sizeHistory;
	xmlSchemaIDCMatcher * matcher; /* the correspondent field/selector matcher */
	xmlSchemaIDCSelect  * sel;
	void * xpathCtxt;
};

typedef xmlSchemaIDCStateObj * xmlSchemaIDCStateObjPtr;

#define IDC_MATCHER 0

/**
 * xmlSchemaIDCMatcher:
 *
 * Used to evaluate IDC selectors (and fields).
 */
struct _xmlSchemaIDCMatcher {
	int type;
	int depth; /* the tree depth at creation time */
	xmlSchemaIDCMatcherPtr next; /* next in the list */
	xmlSchemaIDCMatcherPtr nextCached; /* next in the cache list */
	xmlSchemaIDCAugPtr aidc; /* the augmented IDC item */
	int idcType;
	xmlSchemaPSVIIDCKeyPtr ** keySeqs; /* the key-sequences of the target elements */
	int sizeKeySeqs;
	xmlSchemaItemList * targets; /* list of target-node (xmlSchemaPSVIIDCNodePtr) entries */
};

/*
 * Element info flags.
 */
#define XML_SCHEMA_NODE_INFO_FLAG_OWNED_NAMES  1<<0
#define XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES 1<<1
#define XML_SCHEMA_ELEM_INFO_NILLED            1<<2
#define XML_SCHEMA_ELEM_INFO_LOCAL_TYPE        1<<3

#define XML_SCHEMA_NODE_INFO_VALUE_NEEDED      1<<4
#define XML_SCHEMA_ELEM_INFO_EMPTY             1<<5
#define XML_SCHEMA_ELEM_INFO_HAS_CONTENT       1<<6

#define XML_SCHEMA_ELEM_INFO_HAS_ELEM_CONTENT  1<<7
#define XML_SCHEMA_ELEM_INFO_ERR_BAD_CONTENT  1<<8
#define XML_SCHEMA_NODE_INFO_ERR_NOT_EXPECTED  1<<9
#define XML_SCHEMA_NODE_INFO_ERR_BAD_TYPE  1<<10

/**
 * xmlSchemaNodeInfo:
 *
 * Holds information of an element node.
 */
struct _xmlSchemaNodeInfo {
	int nodeType;
	xmlNode * P_Node;
	int nodeLine;
	const xmlChar * localName;
	const xmlChar * nsName;
	const xmlChar * value;
	xmlSchemaValPtr val; /* the pre-computed value if any */
	xmlSchemaTypePtr typeDef; /* the complex/simple type definition if any */
	int flags; /* combination of node info flags */
	int valNeeded;
	int normVal;
	xmlSchemaElementPtr decl; /* the element/attribute declaration */
	int depth;
	xmlSchemaPSVIIDCBindingPtr idcTable; /* the table of PSVI IDC bindings for the scope element*/
	xmlSchemaIDCMatcherPtr idcMatchers; /* the IDC matchers for the scope element */
	xmlRegExecCtxtPtr regexCtxt;
	const xmlChar ** nsBindings; /* Namespace bindings on this element */
	int nbNsBindings;
	int sizeNsBindings;
	int hasKeyrefs;
	int appliedXPath; /* Indicates that an XPath has been applied. */
};

#define XML_SCHEMAS_ATTR_UNKNOWN 1
#define XML_SCHEMAS_ATTR_ASSESSED 2
#define XML_SCHEMAS_ATTR_PROHIBITED 3
#define XML_SCHEMAS_ATTR_ERR_MISSING 4
#define XML_SCHEMAS_ATTR_INVALID_VALUE 5
#define XML_SCHEMAS_ATTR_ERR_NO_TYPE 6
#define XML_SCHEMAS_ATTR_ERR_FIXED_VALUE 7
#define XML_SCHEMAS_ATTR_DEFAULT 8
#define XML_SCHEMAS_ATTR_VALIDATE_VALUE 9
#define XML_SCHEMAS_ATTR_ERR_WILD_STRICT_NO_DECL 10
#define XML_SCHEMAS_ATTR_HAS_ATTR_USE 11
#define XML_SCHEMAS_ATTR_HAS_ATTR_DECL 12
#define XML_SCHEMAS_ATTR_WILD_SKIP 13
#define XML_SCHEMAS_ATTR_WILD_LAX_NO_DECL 14
#define XML_SCHEMAS_ATTR_ERR_WILD_DUPLICATE_ID 15
#define XML_SCHEMAS_ATTR_ERR_WILD_AND_USE_ID 16
#define XML_SCHEMAS_ATTR_META 17
/*
 * @metaType values of xmlSchemaAttrInfo.
 */
#define XML_SCHEMA_ATTR_INFO_META_XSI_TYPE 1
#define XML_SCHEMA_ATTR_INFO_META_XSI_NIL 2
#define XML_SCHEMA_ATTR_INFO_META_XSI_SCHEMA_LOC 3
#define XML_SCHEMA_ATTR_INFO_META_XSI_NO_NS_SCHEMA_LOC 4
#define XML_SCHEMA_ATTR_INFO_META_XMLNS 5

//typedef struct _xmlSchemaAttrInfo xmlSchemaAttrInfo;

struct xmlSchemaAttrInfo {
	int    nodeType;
	xmlNode * P_Node;
	int    nodeLine;
	const xmlChar * localName;
	const xmlChar * nsName;
	const xmlChar * value;
	xmlSchemaVal  * val; /* the pre-computed value if any */
	xmlSchemaType * typeDef; /* the complex/simple type definition if any */
	int    flags; /* combination of node info flags */
	xmlSchemaAttribute * decl; /* the attribute declaration */
	xmlSchemaAttributeUse * use; /* the attribute use */
	int    state;
	int    metaType;
	const xmlChar * vcValue; /* the value constraint value */
	xmlSchemaNodeInfo * parent;
};

typedef xmlSchemaAttrInfo * xmlSchemaAttrInfoPtr;

#define XML_SCHEMA_VALID_CTXT_FLAG_STREAM 1
/**
 * xmlSchemaValidCtxt:
 *
 * A Schemas validation context
 */
struct _xmlSchemaValidCtxt {
	int    type;
	void * errCtxt;        /* user specific data block */
	xmlSchemaValidityErrorFunc error; /* the callback in case of errors */
	xmlSchemaValidityWarningFunc warning; /* the callback in case of warning */
	xmlStructuredErrorFunc serror;
	xmlSchema * schema;    /* The schema in use */
	xmlDoc * doc;
	xmlParserInputBuffer * input;
	xmlCharEncoding enc;
	xmlSAXHandler * sax;
	xmlParserCtxt * parserCtxt;
	void * user_data; /* @todo What is this for? */
	char * filename;
	int    err;
	int    nberrors;
	xmlNode * P_Node;
	xmlNode * cur;
	/* xmlSchemaTypePtr type; */
	xmlRegExecCtxt * regexp;
	xmlSchemaVal * value;
	int    valueWS;
	int    options;
	xmlNode * validationRoot;
	xmlSchemaParserCtxtPtr pctxt;
	int    xsiAssemble;
	int    depth;
	xmlSchemaNodeInfoPtr * elemInfos; /* array of element informations */
	int    sizeElemInfos;
	xmlSchemaNodeInfoPtr inode; /* the current element information */
	xmlSchemaIDCAugPtr aidcs; /* a list of augmented IDC informations */
	xmlSchemaIDCStateObjPtr xpathStates; /* first active state object. */
	xmlSchemaIDCStateObjPtr xpathStatePool; /* first stored state object. */
	xmlSchemaIDCMatcherPtr idcMatcherCache; /* Cache for IDC matcher objects. */
	xmlSchemaPSVIIDCNodePtr * idcNodes; /* list of all IDC node-table entries*/
	int    nbIdcNodes;
	int    sizeIdcNodes;
	xmlSchemaPSVIIDCKeyPtr * idcKeys; /* list of all IDC node-table entries */
	int    nbIdcKeys;
	int    sizeIdcKeys;
	int    flags;
	xmlDict * dict;
#ifdef LIBXML_READER_ENABLED
	xmlTextReader * reader;
#endif
	xmlSchemaAttrInfoPtr * attrInfos;
	int    nbAttrInfos;
	int    sizeAttrInfos;
	int    skipDepth;
	xmlSchemaItemListPtr nodeQNames;
	int    hasKeyrefs;
	int    createIDCNodeTables;
	int    psviExposeIDCNodeTables;
	xmlSchemaValidityLocatorFunc locFunc; // Locator for error reporting in streaming mode 
	void * locCtxt;
};
/**
 * xmlSchemaSubstGroup:
 *
 *
 */
//typedef struct _xmlSchemaSubstGroup xmlSchemaSubstGroup;

struct xmlSchemaSubstGroup {
	xmlSchemaElement  * head;
	xmlSchemaItemList * members;
};

//typedef xmlSchemaSubstGroup * xmlSchemaSubstGroupPtr;

/************************************************************************
*									*
*			Some predeclarations				*
*									*
************************************************************************/

static int xmlSchemaParseInclude(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node);
static int xmlSchemaParseRedefine(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node);
static int FASTCALL xmlSchemaTypeFixup(xmlSchemaTypePtr type, xmlSchemaAbstractCtxtPtr ctxt);
static const xmlChar * xmlSchemaFacetTypeToString(xmlSchemaTypeType type);
static int xmlSchemaParseImport(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node);
static int xmlSchemaCheckFacetValues(xmlSchemaTypePtr typeDecl, xmlSchemaParserCtxtPtr ctxt);
static void xmlSchemaClearValidCtxt(xmlSchemaValidCtxtPtr vctxt);
static xmlSchemaWhitespaceValueType xmlSchemaGetWhiteSpaceFacetValue(xmlSchemaTypePtr type);
static xmlSchemaTreeItem * xmlSchemaParseModelGroup(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, xmlSchemaTypeType type, int withParticle);
static const xmlChar * xmlSchemaGetComponentTypeStr(xmlSchemaBasicItem * item);
static xmlSchemaTypeLinkPtr xmlSchemaGetUnionSimpleTypeMemberTypes(xmlSchemaTypePtr type);
static void xmlSchemaInternalErr(xmlSchemaAbstractCtxtPtr actxt, const char * funcName, const char * message);
static int xmlSchemaCheckCOSSTDerivedOK(xmlSchemaAbstractCtxtPtr ctxt, xmlSchemaTypePtr type, xmlSchemaTypePtr baseType, int subset);
static void xmlSchemaCheckElementDeclComponent(xmlSchemaElementPtr elemDecl, xmlSchemaParserCtxtPtr ctxt);
static void xmlSchemaComponentListFree(xmlSchemaItemListPtr list);
static xmlSchemaQNameRefPtr xmlSchemaParseAttributeGroupRef(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node);

/************************************************************************
*									*
*			Helper functions			        *
*									*
************************************************************************/

/**
 * xmlSchemaItemTypeToStr:
 * @type: the type of the schema item
 *
 * Returns the component name of a schema item.
 */
static const xmlChar * xmlSchemaItemTypeToStr(xmlSchemaTypeType type)
{
	switch(type) {
		case XML_SCHEMA_TYPE_BASIC: return(BAD_CAST "simple type definition");
		case XML_SCHEMA_TYPE_SIMPLE: return(BAD_CAST "simple type definition");
		case XML_SCHEMA_TYPE_COMPLEX: return(BAD_CAST "complex type definition");
		case XML_SCHEMA_TYPE_ELEMENT: return(BAD_CAST "element declaration");
		case XML_SCHEMA_TYPE_ATTRIBUTE_USE: return(BAD_CAST "attribute use");
		case XML_SCHEMA_TYPE_ATTRIBUTE: return(BAD_CAST "attribute declaration");
		case XML_SCHEMA_TYPE_GROUP: return(BAD_CAST "model group definition");
		case XML_SCHEMA_TYPE_ATTRIBUTEGROUP: return(BAD_CAST "attribute group definition");
		case XML_SCHEMA_TYPE_NOTATION: return(BAD_CAST "notation declaration");
		case XML_SCHEMA_TYPE_SEQUENCE: return(BAD_CAST "model group (sequence)");
		case XML_SCHEMA_TYPE_CHOICE: return(BAD_CAST "model group (choice)");
		case XML_SCHEMA_TYPE_ALL: return(BAD_CAST "model group (all)");
		case XML_SCHEMA_TYPE_PARTICLE: return(BAD_CAST "particle");
		case XML_SCHEMA_TYPE_IDC_UNIQUE: return(BAD_CAST "unique identity-constraint");
		/* return(BAD_CAST "IDC (unique)"); */
		case XML_SCHEMA_TYPE_IDC_KEY: return(BAD_CAST "key identity-constraint");
		/* return(BAD_CAST "IDC (key)"); */
		case XML_SCHEMA_TYPE_IDC_KEYREF: return(BAD_CAST "keyref identity-constraint");
		/* return(BAD_CAST "IDC (keyref)"); */
		case XML_SCHEMA_TYPE_ANY: return(BAD_CAST "wildcard (any)");
		case XML_SCHEMA_EXTRA_QNAMEREF: return(BAD_CAST "[helper component] QName reference");
		case XML_SCHEMA_EXTRA_ATTR_USE_PROHIB: return(BAD_CAST "[helper component] attribute use prohibition");
		default: return(BAD_CAST "Not a schema component");
	}
}

/**
 * xmlSchemaGetComponentTypeStr:
 * @type: the type of the schema item
 *
 * Returns the component name of a schema item.
 */
static const xmlChar * xmlSchemaGetComponentTypeStr(xmlSchemaBasicItem * item)
{
	switch(item->type) {
		case XML_SCHEMA_TYPE_BASIC:
		    if(WXS_IS_COMPLEX(WXS_TYPE_CAST item))
			    return(BAD_CAST "complex type definition");
		    else
			    return(BAD_CAST "simple type definition");
		default:
		    return(xmlSchemaItemTypeToStr(item->type));
	}
}

/**
 * xmlSchemaGetComponentNode:
 * @item: a schema component
 *
 * Returns node associated with the schema component.
 * NOTE that such a node need not be available; plus, a component's
 * node need not to reflect the component directly, since there is no
 * one-to-one relationship between the XML Schema representation and
 * the component representation.
 */
static xmlNode * xmlSchemaGetComponentNode(xmlSchemaBasicItem * item)
{
	switch(item->type) {
		case XML_SCHEMA_TYPE_ELEMENT: return (((xmlSchemaElementPtr)item)->P_Node);
		case XML_SCHEMA_TYPE_ATTRIBUTE: return (((xmlSchemaAttributePtr)item)->P_Node);
		case XML_SCHEMA_TYPE_COMPLEX:
		case XML_SCHEMA_TYPE_SIMPLE: return (((xmlSchemaTypePtr)item)->P_Node);
		case XML_SCHEMA_TYPE_ANY:
		case XML_SCHEMA_TYPE_ANY_ATTRIBUTE: return (((xmlSchemaWildcardPtr)item)->P_Node);
		case XML_SCHEMA_TYPE_PARTICLE:      return (((xmlSchemaParticlePtr)item)->P_Node);
		case XML_SCHEMA_TYPE_SEQUENCE:
		case XML_SCHEMA_TYPE_CHOICE:
		case XML_SCHEMA_TYPE_ALL: return (((xmlSchemaModelGroupPtr)item)->P_Node);
		case XML_SCHEMA_TYPE_GROUP: return (((xmlSchemaModelGroupDefPtr)item)->P_Node);
		case XML_SCHEMA_TYPE_ATTRIBUTEGROUP: return (((xmlSchemaAttributeGroupPtr)item)->P_Node);
		case XML_SCHEMA_TYPE_IDC_UNIQUE:
		case XML_SCHEMA_TYPE_IDC_KEY:
		case XML_SCHEMA_TYPE_IDC_KEYREF: return (((xmlSchemaIDCPtr)item)->P_Node);
		case XML_SCHEMA_EXTRA_QNAMEREF: return(((xmlSchemaQNameRefPtr)item)->P_Node);
		/* @todo What to do with NOTATIONs?
		   case XML_SCHEMA_TYPE_NOTATION:
		    return (((xmlSchemaNotationPtr) item)->node);
		 */
		case XML_SCHEMA_TYPE_ATTRIBUTE_USE: return (((xmlSchemaAttributeUsePtr)item)->P_Node);
		default: return 0;
	}
}

#if 0
/**
 * xmlSchemaGetNextComponent:
 * @item: a schema component
 *
 * Returns the next sibling of the schema component.
 */
static xmlSchemaBasicItemPtr xmlSchemaGetNextComponent(xmlSchemaBasicItemPtr item)
{
	switch(item->type) {
		case XML_SCHEMA_TYPE_ELEMENT: return ((xmlSchemaBasicItem *)((xmlSchemaElementPtr)item)->next);
		case XML_SCHEMA_TYPE_ATTRIBUTE: return ((xmlSchemaBasicItem *)((xmlSchemaAttributePtr)item)->next);
		case XML_SCHEMA_TYPE_COMPLEX:
		case XML_SCHEMA_TYPE_SIMPLE: return ((xmlSchemaBasicItem *)((xmlSchemaTypePtr)item)->next);
		case XML_SCHEMA_TYPE_ANY:
		case XML_SCHEMA_TYPE_ANY_ATTRIBUTE: return 0;
		case XML_SCHEMA_TYPE_PARTICLE: return ((xmlSchemaBasicItem *)((xmlSchemaParticlePtr)item)->next);
		case XML_SCHEMA_TYPE_SEQUENCE:
		case XML_SCHEMA_TYPE_CHOICE:
		case XML_SCHEMA_TYPE_ALL: return 0;
		case XML_SCHEMA_TYPE_GROUP: return 0;
		case XML_SCHEMA_TYPE_ATTRIBUTEGROUP: return ((xmlSchemaBasicItem *)((xmlSchemaAttributeGroupPtr)item)->next);
		case XML_SCHEMA_TYPE_IDC_UNIQUE:
		case XML_SCHEMA_TYPE_IDC_KEY:
		case XML_SCHEMA_TYPE_IDC_KEYREF: return ((xmlSchemaBasicItem *)((xmlSchemaIDCPtr)item)->next);
		default: return 0;
	}
}

#endif

/**
 * xmlSchemaFormatQName:
 * @buf: the string buffer
 * @namespaceName:  the namespace name
 * @localName: the local name
 *
 * Returns the given QName in the format "{namespaceName}localName" or
 * just "localName" if @namespaceName is NULL.
 *
 * Returns the localName if @namespaceName is NULL, a formatted
 * string otherwise.
 */
static const xmlChar* xmlSchemaFormatQName(xmlChar ** buf, const xmlChar * namespaceName, const xmlChar * localName)
{
	ZFREE(*buf);
	if(namespaceName) {
		*buf = sstrdup(BAD_CAST "{");
		*buf = xmlStrcat(*buf, namespaceName);
		*buf = xmlStrcat(*buf, BAD_CAST "}");
	}
	if(localName) {
		if(namespaceName == NULL)
			return(localName);
		*buf = xmlStrcat(*buf, localName);
	}
	else {
		*buf = xmlStrcat(*buf, BAD_CAST "(NULL)");
	}
	return ((const xmlChar*)*buf);
}

static const xmlChar* xmlSchemaFormatQNameNs(xmlChar ** buf, xmlNs * ns, const xmlChar * localName)
{
	if(ns)
		return (xmlSchemaFormatQName(buf, ns->href, localName));
	else
		return (xmlSchemaFormatQName(buf, NULL, localName));
}

static const xmlChar * xmlSchemaGetComponentName(xmlSchemaBasicItem * item)
{
	switch(item->type) {
		case XML_SCHEMA_TYPE_ELEMENT: return (((xmlSchemaElementPtr)item)->name);
		case XML_SCHEMA_TYPE_ATTRIBUTE: return (((xmlSchemaAttributePtr)item)->name);
		case XML_SCHEMA_TYPE_ATTRIBUTEGROUP: return (((xmlSchemaAttributeGroupPtr)item)->name);
		case XML_SCHEMA_TYPE_BASIC:
		case XML_SCHEMA_TYPE_SIMPLE:
		case XML_SCHEMA_TYPE_COMPLEX: return (((xmlSchemaTypePtr)item)->name);
		case XML_SCHEMA_TYPE_GROUP: return (((xmlSchemaModelGroupDefPtr)item)->name);
		case XML_SCHEMA_TYPE_IDC_KEY:
		case XML_SCHEMA_TYPE_IDC_UNIQUE:
		case XML_SCHEMA_TYPE_IDC_KEYREF: return (((xmlSchemaIDCPtr)item)->name);
		case XML_SCHEMA_TYPE_ATTRIBUTE_USE:
		    if(WXS_ATTRUSE_DECL(item)) {
			    return xmlSchemaGetComponentName(WXS_BASIC_CAST WXS_ATTRUSE_DECL(item));
		    }
		    else
			    return 0;
		case XML_SCHEMA_EXTRA_QNAMEREF: return (((xmlSchemaQNameRefPtr)item)->name);
		case XML_SCHEMA_TYPE_NOTATION:  return (((xmlSchemaNotationPtr)item)->name);
		default:
		    /*
		     * Other components cannot have names.
		     */
		    break;
	}
	return 0;
}

#define xmlSchemaGetQNameRefName(r) (WXS_QNAME_CAST(r))->name
#define xmlSchemaGetQNameRefTargetNs(r) (WXS_QNAME_CAST(r))->targetNamespace
/*
   static const xmlChar *
   xmlSchemaGetQNameRefName(void *ref)
   {
    return(((xmlSchemaQNameRefPtr) ref)->name);
   }

   static const xmlChar *
   xmlSchemaGetQNameRefTargetNs(void *ref)
   {
    return(((xmlSchemaQNameRefPtr) ref)->targetNamespace);
   }
 */

static const xmlChar * xmlSchemaGetComponentTargetNs(xmlSchemaBasicItem * item)
{
	switch(item->type) {
		case XML_SCHEMA_TYPE_ELEMENT: return (((xmlSchemaElementPtr)item)->targetNamespace);
		case XML_SCHEMA_TYPE_ATTRIBUTE: return (((xmlSchemaAttributePtr)item)->targetNamespace);
		case XML_SCHEMA_TYPE_ATTRIBUTEGROUP: return (((xmlSchemaAttributeGroupPtr)item)->targetNamespace);
		case XML_SCHEMA_TYPE_BASIC: return (BAD_CAST "http://www.w3.org/2001/XMLSchema");
		case XML_SCHEMA_TYPE_SIMPLE:
		case XML_SCHEMA_TYPE_COMPLEX: return (((xmlSchemaTypePtr)item)->targetNamespace);
		case XML_SCHEMA_TYPE_GROUP: return (((xmlSchemaModelGroupDefPtr)item)->targetNamespace);
		case XML_SCHEMA_TYPE_IDC_KEY:
		case XML_SCHEMA_TYPE_IDC_UNIQUE:
		case XML_SCHEMA_TYPE_IDC_KEYREF: return (((xmlSchemaIDCPtr)item)->targetNamespace);
		case XML_SCHEMA_TYPE_ATTRIBUTE_USE:
		    if(WXS_ATTRUSE_DECL(item)) {
			    return(xmlSchemaGetComponentTargetNs(WXS_BASIC_CAST WXS_ATTRUSE_DECL(item)));
		    }
		    /* @todo Will returning NULL break something? */
		    break;
		case XML_SCHEMA_EXTRA_QNAMEREF:
		    return (((xmlSchemaQNameRefPtr)item)->targetNamespace);
		case XML_SCHEMA_TYPE_NOTATION:
		    return (((xmlSchemaNotationPtr)item)->targetNamespace);
		default:
		    /*
		     * Other components cannot have names.
		     */
		    break;
	}
	return 0;
}

static const xmlChar* xmlSchemaGetComponentQName(xmlChar ** buf, void * item)
{
	return (xmlSchemaFormatQName(buf, xmlSchemaGetComponentTargetNs((xmlSchemaBasicItem *)item), xmlSchemaGetComponentName((xmlSchemaBasicItem *)item)));
}

static const xmlChar* xmlSchemaGetComponentDesignation(xmlChar ** buf, void * item)
{
	xmlChar * str = NULL;
	*buf = xmlStrcat(*buf, WXS_ITEM_TYPE_NAME(item));
	*buf = xmlStrcat(*buf, BAD_CAST " '");
	*buf = xmlStrcat(*buf, xmlSchemaGetComponentQName(&str, (xmlSchemaBasicItem *)item));
	*buf = xmlStrcat(*buf, BAD_CAST "'");
	ZFREE(str);
	return(*buf);
}

static const xmlChar* xmlSchemaGetIDCDesignation(xmlChar ** buf, xmlSchemaIDCPtr idc)
{
	return(xmlSchemaGetComponentDesignation(buf, idc));
}

/**
 * xmlSchemaWildcardPCToString:
 * @pc: the type of processContents
 *
 * Returns a string representation of the type of
 * processContents.
 */
static const xmlChar * xmlSchemaWildcardPCToString(int pc)
{
	switch(pc) {
		case XML_SCHEMAS_ANY_SKIP: return (BAD_CAST "skip");
		case XML_SCHEMAS_ANY_LAX: return (BAD_CAST "lax");
		case XML_SCHEMAS_ANY_STRICT: return (BAD_CAST "strict");
		default: return (BAD_CAST "invalid process contents");
	}
}

/**
 * xmlSchemaGetCanonValueWhtspExt:
 * @val: the precomputed value
 * @retValue: the returned value
 * @ws: the whitespace type of the value
 *
 * Get a the canonical representation of the value.
 * The caller has to free the returned retValue.
 *
 * Returns 0 if the value could be built and -1 in case of
 *         API errors or if the value type is not supported yet.
 */
static int xmlSchemaGetCanonValueWhtspExt(xmlSchemaValPtr val, xmlSchemaWhitespaceValueType ws, xmlChar ** retValue)
{
	int list;
	xmlSchemaValType valType;
	const xmlChar * value;
	xmlChar * value2 = NULL;
	if((retValue == NULL) || (val == NULL))
		return -1;
	list = xmlSchemaValueGetNext(val) ? 1 : 0;
	*retValue = NULL;
	do {
		value = NULL;
		valType = xmlSchemaGetValType(val);
		switch(valType) {
			case XML_SCHEMAS_STRING:
			case XML_SCHEMAS_NORMSTRING:
			case XML_SCHEMAS_ANYSIMPLETYPE:
			    value = xmlSchemaValueGetAsString(val);
			    if(value) {
				    if(ws == XML_SCHEMA_WHITESPACE_COLLAPSE)
					    value2 = xmlSchemaCollapseString(value);
				    else if(ws == XML_SCHEMA_WHITESPACE_REPLACE)
					    value2 = xmlSchemaWhiteSpaceReplace(value);
				    if(value2)
					    value = value2;
			    }
			    break;
			default:
			    if(xmlSchemaGetCanonValue(val, &value2) == -1) {
				    SAlloc::F((xmlChar*)value2);
				    goto internal_error;
			    }
			    value = value2;
		}
		if(*retValue == NULL)
			if(!value) {
				if(!list)
					*retValue = sstrdup(BAD_CAST "");
			}
			else
				*retValue = sstrdup(value);
		else if(value) {
			/* List. */
			*retValue = xmlStrcat((xmlChar*)*retValue, BAD_CAST " ");
			*retValue = xmlStrcat((xmlChar*)*retValue, value);
		}
		ZFREE(value2);
		val = xmlSchemaValueGetNext(val);
	} while(val);

	return 0;
internal_error:
	SAlloc::F((xmlChar*)(*retValue));
	SAlloc::F((xmlChar*)value2);
	return -1;
}

/**
 * xmlSchemaFormatItemForReport:
 * @buf: the string buffer
 * @itemDes: the designation of the item
 * @itemName: the name of the item
 * @item: the item as an object
 * @itemNode: the node of the item
 * @local: the local name
 * @parsing: if the function is used during the parse
 *
 * Returns a representation of the given item used
 * for error reports.
 *
 * The following order is used to build the resulting
 * designation if the arguments are not NULL:
 * 1a. If itemDes not NULL -> itemDes
 * 1b. If (itemDes not NULL) and (itemName not NULL)
 *     -> itemDes + itemName
 * 2. If the preceding was NULL and (item not NULL) -> item
 * 3. If the preceding was NULL and (itemNode not NULL) -> itemNode
 *
 * If the itemNode is an attribute node, the name of the attribute
 * will be appended to the result.
 *
 * Returns the formatted string and sets @buf to the resulting value.
 */
static xmlChar* xmlSchemaFormatItemForReport(xmlChar ** buf, const xmlChar * itemDes, xmlSchemaBasicItem * item, xmlNode * itemNode)
{
	xmlChar * str = NULL;
	int named = 1;
	SAlloc::F(*buf);
	*buf = NULL;
	if(itemDes) {
		*buf = sstrdup(itemDes);
	}
	else if(item) {
		switch(item->type) {
			case XML_SCHEMA_TYPE_BASIC: {
			    xmlSchemaTypePtr type = WXS_TYPE_CAST item;
			    if(WXS_IS_ATOMIC(type))
				    *buf = sstrdup(BAD_CAST "atomic type 'xs:");
			    else if(WXS_IS_LIST(type))
				    *buf = sstrdup(BAD_CAST "list type 'xs:");
			    else if(WXS_IS_UNION(type))
				    *buf = sstrdup(BAD_CAST "union type 'xs:");
			    else
				    *buf = sstrdup(BAD_CAST "simple type 'xs:");
			    *buf = xmlStrcat(*buf, type->name);
			    *buf = xmlStrcat(*buf, BAD_CAST "'");
		    }
		    break;
			case XML_SCHEMA_TYPE_SIMPLE: {
			    xmlSchemaTypePtr type = WXS_TYPE_CAST item;
			    if(type->flags & XML_SCHEMAS_TYPE_GLOBAL) {
				    *buf = sstrdup(BAD_CAST "");
			    }
			    else {
				    *buf = sstrdup(BAD_CAST "local ");
			    }
			    if(WXS_IS_ATOMIC(type))
				    *buf = xmlStrcat(*buf, BAD_CAST "atomic type");
			    else if(WXS_IS_LIST(type))
				    *buf = xmlStrcat(*buf, BAD_CAST "list type");
			    else if(WXS_IS_UNION(type))
				    *buf = xmlStrcat(*buf, BAD_CAST "union type");
			    else
				    *buf = xmlStrcat(*buf, BAD_CAST "simple type");
			    if(type->flags & XML_SCHEMAS_TYPE_GLOBAL) {
				    *buf = xmlStrcat(*buf, BAD_CAST " '");
				    *buf = xmlStrcat(*buf, type->name);
				    *buf = xmlStrcat(*buf, BAD_CAST "'");
			    }
		    }
		    break;
			case XML_SCHEMA_TYPE_COMPLEX: {
			    xmlSchemaTypePtr type = WXS_TYPE_CAST item;
			    if(type->flags & XML_SCHEMAS_TYPE_GLOBAL)
				    *buf = sstrdup(BAD_CAST "");
			    else
				    *buf = sstrdup(BAD_CAST "local ");
			    *buf = xmlStrcat(*buf, BAD_CAST "complex type");
			    if(type->flags & XML_SCHEMAS_TYPE_GLOBAL) {
				    *buf = xmlStrcat(*buf, BAD_CAST " '");
				    *buf = xmlStrcat(*buf, type->name);
				    *buf = xmlStrcat(*buf, BAD_CAST "'");
			    }
		    }
		    break;
			case XML_SCHEMA_TYPE_ATTRIBUTE_USE: {
			    xmlSchemaAttributeUsePtr ause = WXS_ATTR_USE_CAST item;
			    *buf = sstrdup(BAD_CAST "attribute use ");
			    if(WXS_ATTRUSE_DECL(ause)) {
				    *buf = xmlStrcat(*buf, BAD_CAST "'");
				    *buf = xmlStrcat(*buf, xmlSchemaGetComponentQName(&str, WXS_ATTRUSE_DECL(ause)));
				    ZFREE(str);
				    *buf = xmlStrcat(*buf, BAD_CAST "'");
			    }
			    else {
				    *buf = xmlStrcat(*buf, BAD_CAST "(unknown)");
			    }
		    }
		    break;
			case XML_SCHEMA_TYPE_ATTRIBUTE: {
			    xmlSchemaAttributePtr attr = (xmlSchemaAttributePtr)item;
			    *buf = sstrdup(BAD_CAST "attribute decl.");
			    *buf = xmlStrcat(*buf, BAD_CAST " '");
			    *buf = xmlStrcat(*buf, xmlSchemaFormatQName(&str, attr->targetNamespace, attr->name));
			    ZFREE(str);
			    *buf = xmlStrcat(*buf, BAD_CAST "'");
		    }
		    break;
			case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
			    xmlSchemaGetComponentDesignation(buf, item);
			    break;
			case XML_SCHEMA_TYPE_ELEMENT: {
			    xmlSchemaElementPtr elem = (xmlSchemaElementPtr)item;
			    *buf = sstrdup(BAD_CAST "element decl.");
			    *buf = xmlStrcat(*buf, BAD_CAST " '");
			    *buf = xmlStrcat(*buf, xmlSchemaFormatQName(&str, elem->targetNamespace, elem->name));
			    *buf = xmlStrcat(*buf, BAD_CAST "'");
		    }
		    break;
			case XML_SCHEMA_TYPE_IDC_UNIQUE:
			case XML_SCHEMA_TYPE_IDC_KEY:
			case XML_SCHEMA_TYPE_IDC_KEYREF:
			    if(item->type == XML_SCHEMA_TYPE_IDC_UNIQUE)
				    *buf = sstrdup(BAD_CAST "unique '");
			    else if(item->type == XML_SCHEMA_TYPE_IDC_KEY)
				    *buf = sstrdup(BAD_CAST "key '");
			    else
				    *buf = sstrdup(BAD_CAST "keyRef '");
			    *buf = xmlStrcat(*buf, ((xmlSchemaIDCPtr)item)->name);
			    *buf = xmlStrcat(*buf, BAD_CAST "'");
			    break;
			case XML_SCHEMA_TYPE_ANY:
			case XML_SCHEMA_TYPE_ANY_ATTRIBUTE:
			    *buf = sstrdup(xmlSchemaWildcardPCToString(((xmlSchemaWildcardPtr)item)->processContents));
			    *buf = xmlStrcat(*buf, BAD_CAST " wildcard");
			    break;
			case XML_SCHEMA_FACET_MININCLUSIVE:
			case XML_SCHEMA_FACET_MINEXCLUSIVE:
			case XML_SCHEMA_FACET_MAXINCLUSIVE:
			case XML_SCHEMA_FACET_MAXEXCLUSIVE:
			case XML_SCHEMA_FACET_TOTALDIGITS:
			case XML_SCHEMA_FACET_FRACTIONDIGITS:
			case XML_SCHEMA_FACET_PATTERN:
			case XML_SCHEMA_FACET_ENUMERATION:
			case XML_SCHEMA_FACET_WHITESPACE:
			case XML_SCHEMA_FACET_LENGTH:
			case XML_SCHEMA_FACET_MAXLENGTH:
			case XML_SCHEMA_FACET_MINLENGTH:
			    *buf = sstrdup(BAD_CAST "facet '");
			    *buf = xmlStrcat(*buf, xmlSchemaFacetTypeToString(item->type));
			    *buf = xmlStrcat(*buf, BAD_CAST "'");
			    break;
			case XML_SCHEMA_TYPE_GROUP: {
			    *buf = sstrdup(BAD_CAST "model group def.");
			    *buf = xmlStrcat(*buf, BAD_CAST " '");
			    *buf = xmlStrcat(*buf, xmlSchemaGetComponentQName(&str, item));
			    *buf = xmlStrcat(*buf, BAD_CAST "'");
			    ZFREE(str);
		    }
		    break;
			case XML_SCHEMA_TYPE_SEQUENCE:
			case XML_SCHEMA_TYPE_CHOICE:
			case XML_SCHEMA_TYPE_ALL:
			case XML_SCHEMA_TYPE_PARTICLE:
			    *buf = sstrdup(WXS_ITEM_TYPE_NAME(item));
			    break;
			case XML_SCHEMA_TYPE_NOTATION: {
			    *buf = sstrdup(WXS_ITEM_TYPE_NAME(item));
			    *buf = xmlStrcat(*buf, BAD_CAST " '");
			    *buf = xmlStrcat(*buf, xmlSchemaGetComponentQName(&str, item));
			    *buf = xmlStrcat(*buf, BAD_CAST "'");
			    ZFREE(str);
		    }
			default:
			    named = 0;
		}
	}
	else
		named = 0;
	if(!named && itemNode) {
		xmlNode * elem = (itemNode->type == XML_ATTRIBUTE_NODE) ? itemNode->parent : itemNode;
		*buf = sstrdup(BAD_CAST "Element '");
		if(elem->ns) {
			*buf = xmlStrcat(*buf, xmlSchemaFormatQName(&str, elem->ns->href, elem->name));
			ZFREE(str);
		}
		else
			*buf = xmlStrcat(*buf, elem->name);
		*buf = xmlStrcat(*buf, BAD_CAST "'");
	}
	if(itemNode && (itemNode->type == XML_ATTRIBUTE_NODE)) {
		*buf = xmlStrcat(*buf, BAD_CAST ", attribute '");
		if(itemNode->ns) {
			*buf = xmlStrcat(*buf, xmlSchemaFormatQName(&str, itemNode->ns->href, itemNode->name));
			ZFREE(str);
		}
		else
			*buf = xmlStrcat(*buf, itemNode->name);
		*buf = xmlStrcat(*buf, BAD_CAST "'");
	}
	ZFREE(str);
	return (*buf);
}

/**
 * xmlSchemaFormatFacetEnumSet:
 * @buf: the string buffer
 * @type: the type holding the enumeration facets
 *
 * Builds a string consisting of all enumeration elements.
 *
 * Returns a string of all enumeration elements.
 */
static const xmlChar * xmlSchemaFormatFacetEnumSet(xmlSchemaAbstractCtxtPtr actxt,
    xmlChar ** buf, xmlSchemaTypePtr type)
{
	xmlSchemaFacetPtr facet;
	xmlSchemaWhitespaceValueType ws;
	xmlChar * value = NULL;
	int res, found = 0;
	SAlloc::F(*buf);
	*buf = NULL;
	do {
		/*
		 * Use the whitespace type of the base type.
		 */
		ws = xmlSchemaGetWhiteSpaceFacetValue(type->baseType);
		for(facet = type->facets; facet; facet = facet->next) {
			if(facet->type == XML_SCHEMA_FACET_ENUMERATION) {
				found = 1;
				res = xmlSchemaGetCanonValueWhtspExt(facet->val, ws, &value);
				if(res == -1) {
					xmlSchemaInternalErr(actxt, "xmlSchemaFormatFacetEnumSet", "compute the canonical lexical representation");
					SAlloc::F(*buf);
					*buf = NULL;
					return 0;
				}
				else {
					*buf = *buf ? xmlStrcat(*buf, BAD_CAST ", '") : sstrdup(BAD_CAST "'");
					*buf = xmlStrcat(*buf, BAD_CAST value);
					*buf = xmlStrcat(*buf, BAD_CAST "'");
					if(value) {
						SAlloc::F((xmlChar*)value);
						value = NULL;
					}
				}
			}
		}
		/*
		 * The enumeration facet of a type restricts the enumeration
		 * facet of the ancestor type; i.e., such restricted enumerations
		 * do not belong to the set of the given type. Thus we break
		 * on the first found enumeration.
		 */
		if(found)
			break;
		type = type->baseType;
	} while(type && (type->type != XML_SCHEMA_TYPE_BASIC));
	return ((const xmlChar*)*buf);
}

/************************************************************************
*									*
*			Error functions				        *
*									*
************************************************************************/

#if 0
static void xmlSchemaErrMemory(const char * msg)
{
	__xmlSimpleError(XML_FROM_SCHEMASP, XML_ERR_NO_MEMORY, NULL, NULL, msg);
}

#endif

static void xmlSchemaPSimpleErr(const char * msg)
{
	__xmlSimpleError(XML_FROM_SCHEMASP, XML_ERR_NO_MEMORY, NULL, NULL, msg);
}

/**
 * xmlSchemaPErrMemory:
 * @node: a context node
 * @extra:  extra informations
 *
 * Handle an out of memory condition
 */
static void FASTCALL xmlSchemaPErrMemory(xmlSchemaParserCtxtPtr ctxt, const char * extra, xmlNode * P_Node)
{
	if(ctxt)
		ctxt->nberrors++;
	__xmlSimpleError(XML_FROM_SCHEMASP, XML_ERR_NO_MEMORY, P_Node, NULL, extra);
}
/**
 * xmlSchemaPErr:
 * @ctxt: the parsing context
 * @node: the context node
 * @error: the error code
 * @msg: the error message
 * @str1: extra data
 * @str2: extra data
 *
 * Handle a parser error
 */
static void xmlSchemaPErr(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, int error, const char * msg, const xmlChar * str1, const xmlChar * str2)
{
	xmlGenericErrorFunc channel = NULL;
	xmlStructuredErrorFunc schannel = NULL;
	void * data = NULL;
	if(ctxt) {
		ctxt->nberrors++;
		ctxt->err = error;
		channel = ctxt->F_Error;
		data = ctxt->errCtxt;
		schannel = ctxt->serror;
	}
	__xmlRaiseError(schannel, channel, data, ctxt, P_Node, XML_FROM_SCHEMASP,
	    error, XML_ERR_ERROR, NULL, 0, (const char*)str1, (const char*)str2, NULL, 0, 0, msg, str1, str2);
}

/**
 * xmlSchemaPErr2:
 * @ctxt: the parsing context
 * @node: the context node
 * @node: the current child
 * @error: the error code
 * @msg: the error message
 * @str1: extra data
 * @str2: extra data
 *
 * Handle a parser error
 */
static void xmlSchemaPErr2(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, xmlNode * child, int error, const char * msg, const xmlChar * str1, const xmlChar * str2)
{
	xmlSchemaPErr(ctxt, NZOR(child, P_Node), error, msg, str1, str2);
}

/**
 * xmlSchemaPErrExt:
 * @ctxt: the parsing context
 * @node: the context node
 * @error: the error code
 * @strData1: extra data
 * @strData2: extra data
 * @strData3: extra data
 * @msg: the message
 * @str1:  extra parameter for the message display
 * @str2:  extra parameter for the message display
 * @str3:  extra parameter for the message display
 * @str4:  extra parameter for the message display
 * @str5:  extra parameter for the message display
 *
 * Handle a parser error
 */
static void xmlSchemaPErrExt(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, int error, const xmlChar * strData1, const xmlChar * strData2,
    const xmlChar * strData3, const char * msg, const xmlChar * str1, const xmlChar * str2, const xmlChar * str3, const xmlChar * str4, const xmlChar * str5)
{
	xmlGenericErrorFunc channel = NULL;
	xmlStructuredErrorFunc schannel = NULL;
	void * data = NULL;
	if(ctxt) {
		ctxt->nberrors++;
		ctxt->err = error;
		channel = ctxt->F_Error;
		data = ctxt->errCtxt;
		schannel = ctxt->serror;
	}
	__xmlRaiseError(schannel, channel, data, ctxt, P_Node, XML_FROM_SCHEMASP,
	    error, XML_ERR_ERROR, NULL, 0, (const char*)strData1, (const char*)strData2,
	    (const char*)strData3, 0, 0, msg, str1, str2, str3, str4, str5);
}

/************************************************************************
*									*
*			Allround error functions			*
*									*
************************************************************************/

/**
 * xmlSchemaVTypeErrMemory:
 * @node: a context node
 * @extra:  extra informations
 *
 * Handle an out of memory condition
 */
static void FASTCALL xmlSchemaVErrMemory(xmlSchemaValidCtxtPtr ctxt, const char * extra, xmlNode * P_Node)
{
	if(ctxt) {
		ctxt->nberrors++;
		ctxt->err = XML_SCHEMAV_INTERNAL;
	}
	__xmlSimpleError(XML_FROM_SCHEMASV, XML_ERR_NO_MEMORY, P_Node, NULL, extra);
}

static void xmlSchemaPSimpleInternalErr(xmlNode * P_Node, const char * msg, const xmlChar * str)
{
	__xmlSimpleError(XML_FROM_SCHEMASP, XML_SCHEMAP_INTERNAL, P_Node, msg, (const char*)str);
}

#define WXS_ERROR_TYPE_ERROR 1
#define WXS_ERROR_TYPE_WARNING 2
/**
 * xmlSchemaErr3:
 * @ctxt: the validation context
 * @node: the context node
 * @error: the error code
 * @msg: the error message
 * @str1: extra data
 * @str2: extra data
 * @str3: extra data
 *
 * Handle a validation error
 */
static void xmlSchemaErr4Line(xmlSchemaAbstractCtxtPtr ctxt, xmlErrorLevel errorLevel,
    int error, xmlNode * P_Node, int line, const char * msg, const xmlChar * str1, const xmlChar * str2, const xmlChar * str3, const xmlChar * str4)
{
	xmlStructuredErrorFunc schannel = NULL;
	xmlGenericErrorFunc channel = NULL;
	void * data = NULL;
	if(ctxt) {
		if(ctxt->type == XML_SCHEMA_CTXT_VALIDATOR) {
			xmlSchemaValidCtxtPtr vctxt = (xmlSchemaValidCtxtPtr)ctxt;
			const char * file = NULL;
			int col = 0;
			if(errorLevel != XML_ERR_WARNING) {
				vctxt->nberrors++;
				vctxt->err = error;
				channel = vctxt->error;
			}
			else {
				channel = vctxt->warning;
			}
			schannel = vctxt->serror;
			data = vctxt->errCtxt;
			//
			// Error node. If we specify a line number, then do not channel any node to the error function.
			//
			if(line == 0) {
				if(!P_Node && (vctxt->depth >= 0) && vctxt->inode) {
					P_Node = vctxt->inode->P_Node;
				}
				/*
				 * Get filename and line if no node-tree.
				 */
				if(!P_Node && vctxt->parserCtxt && vctxt->parserCtxt->input) {
					file = vctxt->parserCtxt->input->filename;
					line = vctxt->parserCtxt->input->line;
					col = vctxt->parserCtxt->input->col;
				}
			}
			else {
				/*
				 * Override the given node's (if any) position
				 * and channel only the given line number.
				 */
				P_Node = NULL;
				/*
				 * Get filename.
				 */
				if(vctxt->doc)
					file = (const char*)vctxt->doc->URL;
				else if(vctxt->parserCtxt && vctxt->parserCtxt->input)
					file = vctxt->parserCtxt->input->filename;
			}
			if(vctxt->locFunc) {
				if((file == NULL) || (line == 0)) {
					unsigned long l;
					const char * f;
					vctxt->locFunc(vctxt->locCtxt, &f, &l);
					SETIFZ(file, f);
					SETIFZ(line, (int)l);
				}
			}
			if(!file && vctxt->filename)
				file = vctxt->filename;
			__xmlRaiseError(schannel, channel, data, ctxt, P_Node, XML_FROM_SCHEMASV,
			    error, errorLevel, file, line, (const char*)str1, (const char*)str2, (const char*)str3, 0, col, msg, str1, str2, str3, str4);
		}
		else if(ctxt->type == XML_SCHEMA_CTXT_PARSER) {
			xmlSchemaParserCtxtPtr pctxt = (xmlSchemaParserCtxtPtr)ctxt;
			if(errorLevel != XML_ERR_WARNING) {
				pctxt->nberrors++;
				pctxt->err = error;
				channel = pctxt->F_Error;
			}
			else {
				channel = pctxt->F_Warning;
			}
			schannel = pctxt->serror;
			data = pctxt->errCtxt;
			__xmlRaiseError(schannel, channel, data, ctxt, P_Node, XML_FROM_SCHEMASP, error,
			    errorLevel, NULL, 0, (const char*)str1, (const char*)str2, (const char*)str3, 0, 0, msg, str1, str2, str3, str4);
		}
		else {
			TODO
		}
	}
}

/**
 * xmlSchemaErr3:
 * @ctxt: the validation context
 * @node: the context node
 * @error: the error code
 * @msg: the error message
 * @str1: extra data
 * @str2: extra data
 * @str3: extra data
 *
 * Handle a validation error
 */
static void xmlSchemaErr3(xmlSchemaAbstractCtxtPtr actxt, int error, xmlNode * P_Node, const char * msg, const xmlChar * str1, const xmlChar * str2, const xmlChar * str3)
{
	xmlSchemaErr4Line(actxt, XML_ERR_ERROR, error, P_Node, 0, msg, str1, str2, str3, 0);
}

static void xmlSchemaErr4(xmlSchemaAbstractCtxtPtr actxt, int error, xmlNode * P_Node, const char * msg, const xmlChar * str1, const xmlChar * str2, const xmlChar * str3, const xmlChar * str4)
{
	xmlSchemaErr4Line(actxt, XML_ERR_ERROR, error, P_Node, 0, msg, str1, str2, str3, str4);
}

static void xmlSchemaErr(xmlSchemaAbstractCtxtPtr actxt, int error, xmlNode * P_Node, const char * msg, const xmlChar * str1, const xmlChar * str2)
{
	xmlSchemaErr4(actxt, error, P_Node, msg, str1, str2, 0, 0);
}

static xmlChar * xmlSchemaFormatNodeForError(xmlChar ** msg, xmlSchemaAbstractCtxtPtr actxt, xmlNode * P_Node)
{
	xmlChar * str = NULL;
	*msg = NULL;
	if(P_Node && (P_Node->type != XML_ELEMENT_NODE) && (P_Node->type != XML_ATTRIBUTE_NODE)) {
		/*
		 * Don't try to format other nodes than element and
		 * attribute nodes.
		 * Play save and return an empty string.
		 */
		*msg = sstrdup(BAD_CAST "");
		return(*msg);
	}
	if(P_Node) {
		/*
		 * Work on tree nodes.
		 */
		if(P_Node->type == XML_ATTRIBUTE_NODE) {
			xmlNode * elem = P_Node->parent;
			*msg = sstrdup(BAD_CAST "Element '");
			if(elem->ns)
				*msg = xmlStrcat(*msg, xmlSchemaFormatQName(&str, elem->ns->href, elem->name));
			else
				*msg = xmlStrcat(*msg, xmlSchemaFormatQName(&str, NULL, elem->name));
			ZFREE(str);
			*msg = xmlStrcat(*msg, BAD_CAST "', ");
			*msg = xmlStrcat(*msg, BAD_CAST "attribute '");
		}
		else {
			*msg = sstrdup(BAD_CAST "Element '");
		}
		if(P_Node->ns)
			*msg = xmlStrcat(*msg, xmlSchemaFormatQName(&str, P_Node->ns->href, P_Node->name));
		else
			*msg = xmlStrcat(*msg, xmlSchemaFormatQName(&str, NULL, P_Node->name));
		ZFREE(str);
		*msg = xmlStrcat(*msg, BAD_CAST "': ");
	}
	else if(actxt->type == XML_SCHEMA_CTXT_VALIDATOR) {
		xmlSchemaValidCtxtPtr vctxt = (xmlSchemaValidCtxtPtr)actxt;
		/*
		 * Work on node infos.
		 */
		if(vctxt->inode->nodeType == XML_ATTRIBUTE_NODE) {
			xmlSchemaNodeInfoPtr ielem = vctxt->elemInfos[vctxt->depth];
			*msg = sstrdup(BAD_CAST "Element '");
			*msg = xmlStrcat(*msg, xmlSchemaFormatQName(&str, ielem->nsName, ielem->localName));
			ZFREE(str);
			*msg = xmlStrcat(*msg, BAD_CAST "', ");
			*msg = xmlStrcat(*msg, BAD_CAST "attribute '");
		}
		else {
			*msg = sstrdup(BAD_CAST "Element '");
		}
		*msg = xmlStrcat(*msg, xmlSchemaFormatQName(&str, vctxt->inode->nsName, vctxt->inode->localName));
		ZFREE(str);
		*msg = xmlStrcat(*msg, BAD_CAST "': ");
	}
	else if(actxt->type == XML_SCHEMA_CTXT_PARSER) {
		/*
		 * Hmm, no node while parsing?
		 * Return an empty string, in case NULL will break something.
		 */
		*msg = sstrdup(BAD_CAST "");
	}
	else {
		TODO
		return 0;
	}
	/*
	 * VAL TODO: The output of the given schema component is currently
	 * disabled.
	 */
#if 0
	if(type && (xmlSchemaIsGlobalItem(type))) {
		*msg = xmlStrcat(*msg, BAD_CAST " [");
		*msg = xmlStrcat(*msg, xmlSchemaFormatItemForReport(&str, NULL, type, NULL, 0));
		ZFREE(str);
		*msg = xmlStrcat(*msg, BAD_CAST "]");
	}
#endif
	return (*msg);
}

static void xmlSchemaInternalErr2(xmlSchemaAbstractCtxtPtr actxt,
    const char * funcName, const char * message, const xmlChar * str1, const xmlChar * str2)
{
	if(actxt) {
		xmlChar * msg = sstrdup(BAD_CAST "Internal error: ");
		msg = xmlStrcat(msg, BAD_CAST funcName);
		msg = xmlStrcat(msg, BAD_CAST ", ");
		msg = xmlStrcat(msg, BAD_CAST message);
		msg = xmlStrcat(msg, BAD_CAST ".\n");
		if(actxt->type == XML_SCHEMA_CTXT_VALIDATOR)
			xmlSchemaErr(actxt, XML_SCHEMAV_INTERNAL, NULL, (const char*)msg, str1, str2);
		else if(actxt->type == XML_SCHEMA_CTXT_PARSER)
			xmlSchemaErr(actxt, XML_SCHEMAP_INTERNAL, NULL, (const char*)msg, str1, str2);
		ZFREE(msg);
	}
}

static void xmlSchemaInternalErr(xmlSchemaAbstractCtxtPtr actxt, const char * funcName, const char * message)
{
	xmlSchemaInternalErr2(actxt, funcName, message, 0, 0);
}

#if 0
static void xmlSchemaPInternalErr(xmlSchemaParserCtxtPtr pctxt,
    const char * funcName,
    const char * message,
    const xmlChar * str1,
    const xmlChar * str2)
{
	xmlSchemaInternalErr2(ACTXT_CAST pctxt, funcName, message, str1, str2);
}

#endif

static void xmlSchemaCustomErr4(xmlSchemaAbstractCtxtPtr actxt, xmlParserErrors error,
    xmlNode * P_Node, xmlSchemaBasicItem * item, const char * message, const xmlChar * str1, const xmlChar * str2, const xmlChar * str3, const xmlChar * str4)
{
	xmlChar * msg = NULL;
	if(!P_Node && item && (actxt->type == XML_SCHEMA_CTXT_PARSER)) {
		P_Node = WXS_ITEM_NODE(item);
		xmlSchemaFormatItemForReport(&msg, NULL, item, 0);
		msg = xmlStrcat(msg, BAD_CAST ": ");
	}
	else
		xmlSchemaFormatNodeForError(&msg, actxt, P_Node);
	msg = xmlStrcat(msg, (const xmlChar*)message);
	msg = xmlStrcat(msg, BAD_CAST ".\n");
	xmlSchemaErr4(actxt, error, P_Node, (const char*)msg, str1, str2, str3, str4);
	ZFREE(msg);
}

static void xmlSchemaCustomErr(xmlSchemaAbstractCtxtPtr actxt, xmlParserErrors error, xmlNode * P_Node,
    xmlSchemaBasicItem * item, const char * message, const xmlChar * str1, const xmlChar * str2)
{
	xmlSchemaCustomErr4(actxt, error, P_Node, item, message, str1, str2, 0, 0);
}

static void xmlSchemaCustomWarning(xmlSchemaAbstractCtxtPtr actxt, xmlParserErrors error,
    xmlNode * P_Node, xmlSchemaTypePtr type ATTRIBUTE_UNUSED, const char * message,
    const xmlChar * str1, const xmlChar * str2, const xmlChar * str3)
{
	xmlChar * msg = NULL;
	xmlSchemaFormatNodeForError(&msg, actxt, P_Node);
	msg = xmlStrcat(msg, (const xmlChar*)message);
	msg = xmlStrcat(msg, BAD_CAST ".\n");
	/* URGENT TODO: Set the error code to something sane. */
	xmlSchemaErr4Line(actxt, XML_ERR_WARNING, error, P_Node, 0, (const char*)msg, str1, str2, str3, 0);
	ZFREE(msg);
}

static void xmlSchemaKeyrefErr(xmlSchemaValidCtxtPtr vctxt, xmlParserErrors error,
    xmlSchemaPSVIIDCNodePtr idcNode, xmlSchemaTypePtr type ATTRIBUTE_UNUSED,
    const char * message, const xmlChar * str1, const xmlChar * str2)
{
	xmlChar * msg = NULL, * qname = NULL;
	msg = sstrdup(BAD_CAST "Element '%s': ");
	msg = xmlStrcat(msg, (const xmlChar*)message);
	msg = xmlStrcat(msg, BAD_CAST ".\n");
	xmlSchemaErr4Line(ACTXT_CAST vctxt, XML_ERR_ERROR,
	    error, NULL, idcNode->nodeLine, (const char*)msg, xmlSchemaFormatQName(&qname,
		    (xmlChar *)vctxt->nodeQNames->items[idcNode->nodeQNameID +1],
		    (xmlChar *)vctxt->nodeQNames->items[idcNode->nodeQNameID]), str1, str2, 0);
	ZFREE(qname);
	ZFREE(msg);
}

static int xmlSchemaEvalErrorNodeType(xmlSchemaAbstractCtxtPtr actxt, xmlNode * P_Node)
{
	if(P_Node)
		return (P_Node->type);
	if((actxt->type == XML_SCHEMA_CTXT_VALIDATOR) && ((xmlSchemaValidCtxtPtr)actxt)->inode)
		return ( ((xmlSchemaValidCtxtPtr)actxt)->inode->nodeType);
	return -1;
}

static int xmlSchemaIsGlobalItem(xmlSchemaTypePtr item)
{
	switch(item->type) {
		case XML_SCHEMA_TYPE_COMPLEX:
		case XML_SCHEMA_TYPE_SIMPLE:
		    if(item->flags & XML_SCHEMAS_TYPE_GLOBAL)
			    return 1;
		    break;
		case XML_SCHEMA_TYPE_GROUP:
		    return 1;
		case XML_SCHEMA_TYPE_ELEMENT:
		    if(((xmlSchemaElementPtr)item)->flags & XML_SCHEMAS_ELEM_GLOBAL)
			    return 1;
		    break;
		case XML_SCHEMA_TYPE_ATTRIBUTE:
		    if(((xmlSchemaAttributePtr)item)->flags & XML_SCHEMAS_ATTR_GLOBAL)
			    return 1;
		    break;
		/* Note that attribute groups are always global. */
		default:
		    return 1;
	}
	return 0;
}

static void xmlSchemaSimpleTypeErr(xmlSchemaAbstractCtxtPtr actxt, xmlParserErrors error, xmlNode * P_Node,
    const xmlChar * value, xmlSchemaTypePtr type, int displayValue)
{
	xmlChar * msg = NULL;
	xmlSchemaFormatNodeForError(&msg, actxt, P_Node);
	if(displayValue || (xmlSchemaEvalErrorNodeType(actxt, P_Node) == XML_ATTRIBUTE_NODE))
		msg = xmlStrcat(msg, BAD_CAST "'%s' is not a valid value of ");
	else
		msg = xmlStrcat(msg, BAD_CAST "The character content is not a valid value of ");
	if(!xmlSchemaIsGlobalItem(type))
		msg = xmlStrcat(msg, BAD_CAST "the local ");
	else
		msg = xmlStrcat(msg, BAD_CAST "the ");
	if(WXS_IS_ATOMIC(type))
		msg = xmlStrcat(msg, BAD_CAST "atomic type");
	else if(WXS_IS_LIST(type))
		msg = xmlStrcat(msg, BAD_CAST "list type");
	else if(WXS_IS_UNION(type))
		msg = xmlStrcat(msg, BAD_CAST "union type");
	if(xmlSchemaIsGlobalItem(type)) {
		xmlChar * str = NULL;
		msg = xmlStrcat(msg, BAD_CAST " '");
		if(type->builtInType != 0) {
			msg = xmlStrcat(msg, BAD_CAST "xs:");
			msg = xmlStrcat(msg, type->name);
		}
		else
			msg = xmlStrcat(msg, xmlSchemaFormatQName(&str, type->targetNamespace, type->name));
		msg = xmlStrcat(msg, BAD_CAST "'");
		ZFREE(str);
	}
	msg = xmlStrcat(msg, BAD_CAST ".\n");
	if(displayValue || (xmlSchemaEvalErrorNodeType(actxt, P_Node) == XML_ATTRIBUTE_NODE))
		xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, 0);
	else
		xmlSchemaErr(actxt, error, P_Node, (const char*)msg, 0, 0);
	ZFREE(msg);
}

static const xmlChar * xmlSchemaFormatErrorNodeQName(xmlChar ** str, xmlSchemaNodeInfoPtr ni, xmlNode * P_Node)
{
	if(P_Node) {
		if(P_Node->ns)
			return (xmlSchemaFormatQName(str, P_Node->ns->href, P_Node->name));
		else
			return (xmlSchemaFormatQName(str, NULL, P_Node->name));
	}
	else if(ni)
		return (xmlSchemaFormatQName(str, ni->nsName, ni->localName));
	return 0;
}

static void xmlSchemaIllegalAttrErr(xmlSchemaAbstractCtxtPtr actxt, xmlParserErrors error, xmlSchemaAttrInfoPtr ni, xmlNode * P_Node)
{
	xmlChar * msg = NULL, * str = NULL;
	xmlSchemaFormatNodeForError(&msg, actxt, P_Node);
	msg = xmlStrcat(msg, BAD_CAST "The attribute '%s' is not allowed.\n");
	xmlSchemaErr(actxt, error, P_Node, (const char*)msg, xmlSchemaFormatErrorNodeQName(&str, (xmlSchemaNodeInfoPtr)ni, P_Node), 0);
	ZFREE(str);
	ZFREE(msg);
}

static void xmlSchemaComplexTypeErr(xmlSchemaAbstractCtxtPtr actxt, xmlParserErrors error, xmlNode * P_Node,
    xmlSchemaTypePtr type ATTRIBUTE_UNUSED, const char * message, int nbval, int nbneg, xmlChar ** values)
{
	xmlChar * str = NULL, * msg = NULL;
	xmlChar * localName, * nsName;
	const xmlChar * cur, * end;
	int i;
	xmlSchemaFormatNodeForError(&msg, actxt, P_Node);
	msg = xmlStrcat(msg, (const xmlChar*)message);
	msg = xmlStrcat(msg, BAD_CAST ".");
	/*
	 * Note that is does not make sense to report that we have a
	 * wildcard here, since the wildcard might be unfolded into
	 * multiple transitions.
	 */
	if(nbval + nbneg > 0) {
		if(nbval + nbneg > 1)
			str = sstrdup(BAD_CAST " Expected is one of ( ");
		else
			str = sstrdup(BAD_CAST " Expected is ( ");
		nsName = NULL;
		for(i = 0; i < nbval + nbneg; i++) {
			cur = values[i];
			if(!cur)
				continue;
			if((cur[0] == 'n') && (cur[1] == 'o') && (cur[2] == 't') && (cur[3] == ' ')) {
				cur += 4;
				str = xmlStrcat(str, BAD_CAST "##other");
			}
			/*
			 * Get the local name.
			 */
			localName = NULL;
			end = cur;
			if(*end == '*') {
				localName = sstrdup(BAD_CAST "*");
				end++;
			}
			else {
				while((*end != 0) && (*end != '|'))
					end++;
				localName = xmlStrncat(localName, BAD_CAST cur, end - cur);
			}
			if(*end != 0) {
				end++;
				/*
				 * Skip "*|*" if they come with negated expressions, since
				 * they represent the same negated wildcard.
				 */
				if((nbneg == 0) || (*end != '*') || (*localName != '*')) {
					/*
					 * Get the namespace name.
					 */
					cur = end;
					if(*end == '*') {
						nsName = sstrdup(BAD_CAST "{*}");
					}
					else {
						while(*end != 0)
							end++;
						if(i >= nbval)
							nsName = sstrdup(BAD_CAST "{##other:");
						else
							nsName = sstrdup(BAD_CAST "{");
						nsName = xmlStrncat(nsName, BAD_CAST cur, end - cur);
						nsName = xmlStrcat(nsName, BAD_CAST "}");
					}
					str = xmlStrcat(str, BAD_CAST nsName);
					ZFREE(nsName);
				}
				else {
					ZFREE(localName);
					continue;
				}
			}
			str = xmlStrcat(str, BAD_CAST localName);
			ZFREE(localName);
			if(i < nbval + nbneg -1)
				str = xmlStrcat(str, BAD_CAST ", ");
		}
		str = xmlStrcat(str, BAD_CAST " ).\n");
		msg = xmlStrcat(msg, BAD_CAST str);
		ZFREE(str);
	}
	else
		msg = xmlStrcat(msg, BAD_CAST "\n");
	xmlSchemaErr(actxt, error, P_Node, (const char*)msg, 0, 0);
	SAlloc::F(msg);
}

static void xmlSchemaFacetErr(xmlSchemaAbstractCtxtPtr actxt, xmlParserErrors error, xmlNode * P_Node, const xmlChar * value,
    unsigned long length, xmlSchemaTypePtr type, xmlSchemaFacetPtr facet, const char * message, const xmlChar * str1, const xmlChar * str2)
{
	xmlChar * str = NULL, * msg = NULL;
	xmlSchemaTypeType facetType;
	int nodeType = xmlSchemaEvalErrorNodeType(actxt, P_Node);
	xmlSchemaFormatNodeForError(&msg, actxt, P_Node);
	if(error == XML_SCHEMAV_CVC_ENUMERATION_VALID) {
		facetType = XML_SCHEMA_FACET_ENUMERATION;
		// If enumerations are validated, one must not expect the facet to be given.
	}
	else
		facetType = facet->type;
	msg = xmlStrcat(msg, BAD_CAST "[");
	msg = xmlStrcat(msg, BAD_CAST "facet '");
	msg = xmlStrcat(msg, xmlSchemaFacetTypeToString(facetType));
	msg = xmlStrcat(msg, BAD_CAST "'] ");
	if(message == NULL) {
		/*
		 * Use a default message.
		 */
		if(oneof3(facetType, XML_SCHEMA_FACET_LENGTH, XML_SCHEMA_FACET_MINLENGTH, XML_SCHEMA_FACET_MAXLENGTH)) {
			char len[25], actLen[25];
			// FIXME, TODO: What is the max expected string length of the this value?
			if(nodeType == XML_ATTRIBUTE_NODE)
				msg = xmlStrcat(msg, BAD_CAST "The value '%s' has a length of '%s'; ");
			else
				msg = xmlStrcat(msg, BAD_CAST "The value has a length of '%s'; ");
			snprintf(len, 24, "%lu", xmlSchemaGetFacetValueAsULong(facet));
			snprintf(actLen, 24, "%lu", length);
			if(facetType == XML_SCHEMA_FACET_LENGTH)
				msg = xmlStrcat(msg, BAD_CAST "this differs from the allowed length of '%s'.\n");
			else if(facetType == XML_SCHEMA_FACET_MAXLENGTH)
				msg = xmlStrcat(msg, BAD_CAST "this exceeds the allowed maximum length of '%s'.\n");
			else if(facetType == XML_SCHEMA_FACET_MINLENGTH)
				msg = xmlStrcat(msg, BAD_CAST "this underruns the allowed minimum length of '%s'.\n");
			if(nodeType == XML_ATTRIBUTE_NODE)
				xmlSchemaErr3(actxt, error, P_Node, (const char*)msg, value, (const xmlChar*)actLen, (const xmlChar*)len);
			else
				xmlSchemaErr(actxt, error, P_Node, (const char*)msg, (const xmlChar*)actLen, (const xmlChar*)len);
		}
		else if(facetType == XML_SCHEMA_FACET_ENUMERATION) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' is not an element of the set {%s}.\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, xmlSchemaFormatFacetEnumSet(actxt, &str, type));
		}
		else if(facetType == XML_SCHEMA_FACET_PATTERN) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' is not accepted by the pattern '%s'.\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, facet->value);
		}
		else if(facetType == XML_SCHEMA_FACET_MININCLUSIVE) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' is less than the minimum value allowed ('%s').\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, facet->value);
		}
		else if(facetType == XML_SCHEMA_FACET_MAXINCLUSIVE) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' is greater than the maximum value allowed ('%s').\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, facet->value);
		}
		else if(facetType == XML_SCHEMA_FACET_MINEXCLUSIVE) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' must be greater than '%s'.\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, facet->value);
		}
		else if(facetType == XML_SCHEMA_FACET_MAXEXCLUSIVE) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' must be less than '%s'.\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, facet->value);
		}
		else if(facetType == XML_SCHEMA_FACET_TOTALDIGITS) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' has more digits than are allowed ('%s').\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, facet->value);
		}
		else if(facetType == XML_SCHEMA_FACET_FRACTIONDIGITS) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' has more fractional digits than are allowed ('%s').\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, facet->value);
		}
		else if(nodeType == XML_ATTRIBUTE_NODE) {
			msg = xmlStrcat(msg, BAD_CAST "The value '%s' is not facet-valid.\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, value, 0);
		}
		else {
			msg = xmlStrcat(msg, BAD_CAST "The value is not facet-valid.\n");
			xmlSchemaErr(actxt, error, P_Node, (const char*)msg, 0, 0);
		}
	}
	else {
		msg = xmlStrcat(msg, (const xmlChar*)message);
		msg = xmlStrcat(msg, BAD_CAST ".\n");
		xmlSchemaErr(actxt, error, P_Node, (const char*)msg, str1, str2);
	}
	ZFREE(str);
	SAlloc::F(msg);
}

#define VERROR(err, type, msg) xmlSchemaCustomErr(ACTXT_CAST vctxt, (xmlParserErrors)err, NULL, type, msg, 0, 0);
#define VERROR_INT(func, msg) xmlSchemaInternalErr(ACTXT_CAST vctxt, func, msg);
#define PERROR_INT(func, msg) xmlSchemaInternalErr(ACTXT_CAST pctxt, func, msg);
#define PERROR_INT2(func, msg) xmlSchemaInternalErr(ACTXT_CAST ctxt, func, msg);
#define AERROR_INT(func, msg) xmlSchemaInternalErr(actxt, func, msg);

/**
 * xmlSchemaPMissingAttrErr:
 * @ctxt: the schema validation context
 * @ownerDes: the designation of  the owner
 * @ownerName: the name of the owner
 * @ownerItem: the owner as a schema object
 * @ownerElem: the owner as an element node
 * @node: the parent element node of the missing attribute node
 * @type: the corresponding type of the attribute node
 *
 * Reports an illegal attribute.
 */
static void xmlSchemaPMissingAttrErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlSchemaBasicItem * ownerItem,
	xmlNode * ownerElem, const char * name, const char * message)
{
	xmlChar * des = NULL;
	xmlSchemaFormatItemForReport(&des, NULL, ownerItem, ownerElem);
	if(message)
		xmlSchemaPErr(ctxt, ownerElem, error, "%s: %s.\n", BAD_CAST des, BAD_CAST message);
	else
		xmlSchemaPErr(ctxt, ownerElem, error, "%s: The attribute '%s' is required but missing.\n", BAD_CAST des, BAD_CAST name);
	ZFREE(des);
}

/**
 * xmlSchemaPResCompAttrErr:
 * @ctxt: the schema validation context
 * @error: the error code
 * @ownerDes: the designation of  the owner
 * @ownerItem: the owner as a schema object
 * @ownerElem: the owner as an element node
 * @name: the name of the attribute holding the QName
 * @refName: the referenced local name
 * @refURI: the referenced namespace URI
 * @message: optional message
 *
 * Used to report QName attribute values that failed to resolve
 * to schema components.
 */
static void xmlSchemaPResCompAttrErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlSchemaBasicItem * ownerItem,
    xmlNode * ownerElem, const char * name, const xmlChar * refName, const xmlChar * refURI, xmlSchemaTypeType refType, const char * refTypeStr)
{
	xmlChar * des = NULL, * strA = NULL;
	xmlSchemaFormatItemForReport(&des, NULL, ownerItem, ownerElem);
	SETIFZ(refTypeStr, (const char*)xmlSchemaItemTypeToStr(refType));
	xmlSchemaPErrExt(ctxt, ownerElem, error, NULL, NULL, NULL,
	    "%s, attribute '%s': The QName value '%s' does not resolve to a(n) %s.\n", BAD_CAST des, BAD_CAST name,
	    xmlSchemaFormatQName(&strA, refURI, refName), BAD_CAST refTypeStr, 0);
	ZFREE(des);
	ZFREE(strA);
}
/**
 * xmlSchemaPCustomAttrErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @ownerDes: the designation of the owner
 * @ownerItem: the owner as a schema object
 * @attr: the illegal attribute node
 *
 * Reports an illegal attribute during the parse.
 */
static void xmlSchemaPCustomAttrErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlChar ** ownerDes,
    xmlSchemaBasicItem * ownerItem, xmlAttrPtr attr, const char * msg)
{
	xmlChar * des = NULL;
	if(ownerDes == NULL)
		xmlSchemaFormatItemForReport(&des, NULL, ownerItem, attr->parent);
	else if(*ownerDes == NULL) {
		xmlSchemaFormatItemForReport(ownerDes, NULL, ownerItem, attr->parent);
		des = *ownerDes;
	}
	else
		des = *ownerDes;
	if(!attr) {
		xmlSchemaPErrExt(ctxt, NULL, error, NULL, NULL, NULL, "%s, attribute '%s': %s.\n", BAD_CAST des, (const xmlChar*)"Unknown", (const xmlChar*)msg, 0, 0);
	}
	else {
		xmlSchemaPErrExt(ctxt, (xmlNode *)attr, error, NULL, NULL, NULL, "%s, attribute '%s': %s.\n", BAD_CAST des, attr->name, (const xmlChar*)msg, 0, 0);
	}
	if(ownerDes == NULL)
		ZFREE(des);
}

/**
 * xmlSchemaPIllegalAttrErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @ownerDes: the designation of the attribute's owner
 * @ownerItem: the attribute's owner item
 * @attr: the illegal attribute node
 *
 * Reports an illegal attribute during the parse.
 */
static void xmlSchemaPIllegalAttrErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlSchemaBasicItem * ownerComp ATTRIBUTE_UNUSED, xmlAttrPtr attr)
{
	xmlChar * strA = NULL, * strB = NULL;
	xmlSchemaFormatNodeForError(&strA, ACTXT_CAST ctxt, attr->parent);
	xmlSchemaErr4(ACTXT_CAST ctxt, error, (xmlNode *)attr, "%sThe attribute '%s' is not allowed.\n", BAD_CAST strA, xmlSchemaFormatQNameNs(&strB, attr->ns, attr->name), NULL, 0);
	ZFREE(strA);
	ZFREE(strB);
}

/**
 * xmlSchemaPCustomErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @itemDes: the designation of the schema item
 * @item: the schema item
 * @itemElem: the node of the schema item
 * @message: the error message
 * @str1: an optional param for the error message
 * @str2: an optional param for the error message
 * @str3: an optional param for the error message
 *
 * Reports an error during parsing.
 */
static void xmlSchemaPCustomErrExt(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error,
    xmlSchemaBasicItem * item, xmlNode * itemElem, const char * message, const xmlChar * str1, const xmlChar * str2, const xmlChar * str3)
{
	xmlChar * des = NULL, * msg = NULL;
	xmlSchemaFormatItemForReport(&des, NULL, item, itemElem);
	msg = sstrdup(BAD_CAST "%s: ");
	msg = xmlStrcat(msg, (const xmlChar*)message);
	msg = xmlStrcat(msg, BAD_CAST ".\n");
	if((itemElem == NULL) && item)
		itemElem = WXS_ITEM_NODE(item);
	xmlSchemaPErrExt(ctxt, itemElem, error, NULL, NULL, NULL, (const char*)msg, BAD_CAST des, str1, str2, str3, 0);
	ZFREE(des);
	ZFREE(msg);
}

/**
 * xmlSchemaPCustomErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @itemDes: the designation of the schema item
 * @item: the schema item
 * @itemElem: the node of the schema item
 * @message: the error message
 * @str1: the optional param for the error message
 *
 * Reports an error during parsing.
 */
static void xmlSchemaPCustomErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlSchemaBasicItem * item,
    xmlNode * itemElem, const char * message, const xmlChar * str1)
{
	xmlSchemaPCustomErrExt(ctxt, error, item, itemElem, message, str1, 0, 0);
}
/**
 * xmlSchemaPAttrUseErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @itemDes: the designation of the schema type
 * @item: the schema type
 * @itemElem: the node of the schema type
 * @attr: the invalid schema attribute
 * @message: the error message
 * @str1: the optional param for the error message
 *
 * Reports an attribute use error during parsing.
 */
static void xmlSchemaPAttrUseErr4(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlNode * P_Node,
    xmlSchemaBasicItem * ownerItem, const xmlSchemaAttributeUsePtr attruse, const char * message,
    const xmlChar * str1, const xmlChar * str2, const xmlChar * str3, const xmlChar * str4)
{
	xmlChar * str = NULL, * msg = NULL;
	xmlSchemaFormatItemForReport(&msg, NULL, ownerItem, 0);
	msg = xmlStrcat(msg, BAD_CAST ", ");
	msg = xmlStrcat(msg, BAD_CAST xmlSchemaFormatItemForReport(&str, NULL, WXS_BASIC_CAST attruse, NULL));
	ZFREE(str);
	msg = xmlStrcat(msg, BAD_CAST ": ");
	msg = xmlStrcat(msg, (const xmlChar*)message);
	msg = xmlStrcat(msg, BAD_CAST ".\n");
	xmlSchemaErr4(ACTXT_CAST ctxt, error, P_Node, (const char*)msg, str1, str2, str3, str4);
	SAlloc::F(msg);
}

/**
 * xmlSchemaPIllegalFacetAtomicErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @type: the schema type
 * @baseType: the base type of type
 * @facet: the illegal facet
 *
 * Reports an illegal facet for atomic simple types.
 */
static void xmlSchemaPIllegalFacetAtomicErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlSchemaTypePtr type, xmlSchemaTypePtr baseType, xmlSchemaFacetPtr facet)
{
	xmlChar * des = NULL, * strT = NULL;
	xmlSchemaFormatItemForReport(&des, NULL, WXS_BASIC_CAST type, type->P_Node);
	xmlSchemaPErrExt(ctxt, type->P_Node, error, NULL, NULL, NULL,
	    "%s: The facet '%s' is not allowed on types derived from the type %s.\n",
	    BAD_CAST des, xmlSchemaFacetTypeToString(facet->type), xmlSchemaFormatItemForReport(&strT, NULL, WXS_BASIC_CAST baseType, NULL), NULL, 0);
	ZFREE(des);
	ZFREE(strT);
}

/**
 * xmlSchemaPIllegalFacetListUnionErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @itemDes: the designation of the schema item involved
 * @item: the schema item involved
 * @facet: the illegal facet
 *
 * Reports an illegal facet for <list> and <union>.
 */
static void xmlSchemaPIllegalFacetListUnionErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlSchemaTypePtr type, xmlSchemaFacetPtr facet)
{
	xmlChar * des = NULL;
	xmlSchemaFormatItemForReport(&des, NULL, WXS_BASIC_CAST type, type->P_Node);
	xmlSchemaPErr(ctxt, type->P_Node, error, "%s: The facet '%s' is not allowed.\n", BAD_CAST des, xmlSchemaFacetTypeToString(facet->type));
	ZFREE(des);
}

/**
 * xmlSchemaPMutualExclAttrErr:
 * @ctxt: the schema validation context
 * @error: the error code
 * @elemDes: the designation of the parent element node
 * @attr: the bad attribute node
 * @type: the corresponding type of the attribute node
 *
 * Reports an illegal attribute.
 */
static void xmlSchemaPMutualExclAttrErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error,
    xmlSchemaBasicItem * ownerItem, xmlAttrPtr attr, const char * name1, const char * name2)
{
	xmlChar * des = NULL;
	xmlSchemaFormatItemForReport(&des, NULL, WXS_BASIC_CAST ownerItem, attr->parent);
	xmlSchemaPErrExt(ctxt, (xmlNode *)attr, error, NULL, NULL, NULL, "%s: The attributes '%s' and '%s' are mutually exclusive.\n", BAD_CAST des, BAD_CAST name1, BAD_CAST name2, 0, 0);
	ZFREE(des);
}
/**
 * xmlSchemaPSimpleTypeErr:
 * @ctxt:  the schema validation context
 * @error: the error code
 * @type: the type specifier
 * @ownerDes: the designation of the owner
 * @ownerItem: the schema object if existent
 * @node: the validated node
 * @value: the validated value
 *
 * Reports a simple type validation error.
 * @todo Should this report the value of an element as well?
 */
static void xmlSchemaPSimpleTypeErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error,
    xmlSchemaBasicItem * ownerItem ATTRIBUTE_UNUSED, xmlNode * P_Node, xmlSchemaTypePtr type,
    const char * expected, const xmlChar * value, const char * message, const xmlChar * str1, const xmlChar * str2)
{
	xmlChar * msg = NULL;
	xmlSchemaFormatNodeForError(&msg, ACTXT_CAST ctxt, P_Node);
	if(message == NULL) {
		/*
		 * Use default messages.
		 */
		if(type) {
			if(P_Node->type == XML_ATTRIBUTE_NODE)
				msg = xmlStrcat(msg, BAD_CAST "'%s' is not a valid value of ");
			else
				msg = xmlStrcat(msg, BAD_CAST "The character content is not a valid value of ");
			if(!xmlSchemaIsGlobalItem(type))
				msg = xmlStrcat(msg, BAD_CAST "the local ");
			else
				msg = xmlStrcat(msg, BAD_CAST "the ");

			if(WXS_IS_ATOMIC(type))
				msg = xmlStrcat(msg, BAD_CAST "atomic type");
			else if(WXS_IS_LIST(type))
				msg = xmlStrcat(msg, BAD_CAST "list type");
			else if(WXS_IS_UNION(type))
				msg = xmlStrcat(msg, BAD_CAST "union type");

			if(xmlSchemaIsGlobalItem(type)) {
				xmlChar * str = NULL;
				msg = xmlStrcat(msg, BAD_CAST " '");
				if(type->builtInType != 0) {
					msg = xmlStrcat(msg, BAD_CAST "xs:");
					msg = xmlStrcat(msg, type->name);
				}
				else
					msg = xmlStrcat(msg, xmlSchemaFormatQName(&str, type->targetNamespace, type->name));
				msg = xmlStrcat(msg, BAD_CAST "'.");
				ZFREE(str);
			}
		}
		else {
			if(P_Node->type == XML_ATTRIBUTE_NODE)
				msg = xmlStrcat(msg, BAD_CAST "The value '%s' is not valid.");
			else
				msg = xmlStrcat(msg, BAD_CAST "The character content is not valid.");
		}
		if(expected) {
			msg = xmlStrcat(msg, BAD_CAST " Expected is '");
			msg = xmlStrcat(msg, BAD_CAST expected);
			msg = xmlStrcat(msg, BAD_CAST "'.\n");
		}
		else
			msg = xmlStrcat(msg, BAD_CAST "\n");
		if(P_Node->type == XML_ATTRIBUTE_NODE)
			xmlSchemaPErr(ctxt, P_Node, error, (const char*)msg, value, 0);
		else
			xmlSchemaPErr(ctxt, P_Node, error, (const char*)msg, 0, 0);
	}
	else {
		msg = xmlStrcat(msg, BAD_CAST message);
		msg = xmlStrcat(msg, BAD_CAST ".\n");
		xmlSchemaPErrExt(ctxt, P_Node, error, NULL, NULL, NULL, (const char*)msg, str1, str2, NULL, 0, 0);
	}
	/* Cleanup. */
	ZFREE(msg);
}

/**
 * xmlSchemaPContentErr:
 * @ctxt: the schema parser context
 * @error: the error code
 * @onwerDes: the designation of the holder of the content
 * @ownerItem: the owner item of the holder of the content
 * @ownerElem: the node of the holder of the content
 * @child: the invalid child node
 * @message: the optional error message
 * @content: the optional string describing the correct content
 *
 * Reports an error concerning the content of a schema element.
 */
static void xmlSchemaPContentErr(xmlSchemaParserCtxtPtr ctxt, xmlParserErrors error, xmlSchemaBasicItem * ownerItem,
    xmlNode * ownerElem, xmlNode * child, const char * message, const char * content)
{
	xmlChar * des = NULL;
	xmlSchemaFormatItemForReport(&des, NULL, ownerItem, ownerElem);
	if(message)
		xmlSchemaPErr2(ctxt, ownerElem, child, error, "%s: %s.\n", BAD_CAST des, BAD_CAST message);
	else if(content)
		xmlSchemaPErr2(ctxt, ownerElem, child, error, "%s: The content is not valid. Expected is %s.\n", BAD_CAST des, BAD_CAST content);
	else
		xmlSchemaPErr2(ctxt, ownerElem, child, error, "%s: The content is not valid.\n", BAD_CAST des, 0);
	ZFREE(des);
}

/************************************************************************
*									*
*			Streamable error functions                      *
*									*
************************************************************************/

/************************************************************************
*									*
*			Validation helper functions			*
*									*
************************************************************************/

/************************************************************************
*									*
*			Allocation functions				*
*									*
************************************************************************/

/**
 * xmlSchemaNewSchemaForParserCtxt:
 * @ctxt:  a schema validation context
 *
 * Allocate a new Schema structure.
 *
 * Returns the newly allocated structure or NULL in case or error
 */
static xmlSchemaPtr xmlSchemaNewSchema(xmlSchemaParserCtxtPtr ctxt)
{
	xmlSchemaPtr ret = (xmlSchemaPtr)SAlloc::M(sizeof(xmlSchema));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating schema", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchema));
	ret->dict = ctxt->dict;
	xmlDictReference(ret->dict);
	return ret;
}

/**
 * xmlSchemaNewFacet:
 *
 * Allocate a new Facet structure.
 *
 * Returns the newly allocated structure or NULL in case or error
 */
xmlSchemaFacetPtr xmlSchemaNewFacet()
{
	xmlSchemaFacetPtr ret;

	ret = (xmlSchemaFacetPtr)SAlloc::M(sizeof(xmlSchemaFacet));
	if(!ret) {
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaFacet));

	return ret;
}

/**
 * xmlSchemaNewAnnot:
 * @ctxt:  a schema validation context
 * @node:  a node
 *
 * Allocate a new annotation structure.
 *
 * Returns the newly allocated structure or NULL in case or error
 */
static xmlSchemaAnnotPtr xmlSchemaNewAnnot(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node)
{
	xmlSchemaAnnotPtr ret;

	ret = (xmlSchemaAnnotPtr)SAlloc::M(sizeof(xmlSchemaAnnot));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating annotation", P_Node);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaAnnot));
	ret->content = P_Node;
	return ret;
}

static xmlSchemaItemListPtr xmlSchemaItemListCreate()
{
	xmlSchemaItemListPtr ret = (xmlSchemaItemListPtr)SAlloc::M(sizeof(xmlSchemaItemList));
	if(!ret) {
		xmlSchemaPErrMemory(NULL, "allocating an item list structure", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaItemList));
	return ret;
}

static void xmlSchemaItemListClear(xmlSchemaItemListPtr list)
{
	if(list) {
		SAlloc::F(list->items);
		list->items = NULL;
		list->nbItems = 0;
		list->sizeItems = 0;
	}
}

static int xmlSchemaItemListAdd(xmlSchemaItemListPtr list, void * item)
{
	if(list->items == NULL) {
		list->items = (void**)SAlloc::M(20 * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "allocating new item list", 0);
			return -1;
		}
		list->sizeItems = 20;
	}
	else if(list->sizeItems <= list->nbItems) {
		list->sizeItems *= 2;
		list->items = (void**)SAlloc::R(list->items, list->sizeItems * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "growing item list", 0);
			list->sizeItems = 0;
			return -1;
		}
	}
	list->items[list->nbItems++] = item;
	return 0;
}

static int xmlSchemaItemListAddSize(xmlSchemaItemListPtr list, int initialSize, void * item)
{
	if(list->items == NULL) {
		if(initialSize <= 0)
			initialSize = 1;
		list->items = (void**)SAlloc::M(initialSize * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "allocating new item list", 0);
			return -1;
		}
		list->sizeItems = initialSize;
	}
	else if(list->sizeItems <= list->nbItems) {
		list->sizeItems *= 2;
		list->items = (void**)SAlloc::R(list->items, list->sizeItems * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "growing item list", 0);
			list->sizeItems = 0;
			return -1;
		}
	}
	list->items[list->nbItems++] = item;
	return 0;
}

static int xmlSchemaItemListInsert(xmlSchemaItemListPtr list, void * item, int idx)
{
	if(list->items == NULL) {
		list->items = (void**)SAlloc::M(20 * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "allocating new item list", 0);
			return -1;
		}
		list->sizeItems = 20;
	}
	else if(list->sizeItems <= list->nbItems) {
		list->sizeItems *= 2;
		list->items = (void**)SAlloc::R(list->items, list->sizeItems * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "growing item list", 0);
			list->sizeItems = 0;
			return -1;
		}
	}
	/*
	 * Just append if the index is greater/equal than the item count.
	 */
	if(idx >= list->nbItems) {
		list->items[list->nbItems++] = item;
	}
	else {
		int i;
		for(i = list->nbItems; i > idx; i--)
			list->items[i] = list->items[i-1];
		list->items[idx] = item;
		list->nbItems++;
	}
	return 0;
}

#if 0 /* enable if ever needed */
static int xmlSchemaItemListInsertSize(xmlSchemaItemListPtr list, int initialSize, void * item, int idx)
{
	if(list->items == NULL) {
		if(initialSize <= 0)
			initialSize = 1;
		list->items = (void**)SAlloc::M(initialSize * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "allocating new item list", 0);
			return -1;
		}
		list->sizeItems = initialSize;
	}
	else if(list->sizeItems <= list->nbItems) {
		list->sizeItems *= 2;
		list->items = (void**)SAlloc::R(list->items, list->sizeItems * sizeof(void *));
		if(list->items == NULL) {
			xmlSchemaPErrMemory(NULL, "growing item list", 0);
			list->sizeItems = 0;
			return -1;
		}
	}
	/*
	 * Just append if the index is greater/equal than the item count.
	 */
	if(idx >= list->nbItems) {
		list->items[list->nbItems++] = item;
	}
	else {
		int i;
		for(i = list->nbItems; i > idx; i--)
			list->items[i] = list->items[i-1];
		list->items[idx] = item;
		list->nbItems++;
	}
	return 0;
}

#endif

static int xmlSchemaItemListRemove(xmlSchemaItemListPtr list, int idx)
{
	int i;
	if((list->items == NULL) || (idx >= list->nbItems)) {
		xmlSchemaPSimpleErr("Internal error: xmlSchemaItemListRemove, index error.\n");
		return -1;
	}
	if(list->nbItems == 1) {
		/* @todo Really free the list? */
		SAlloc::F(list->items);
		list->items = NULL;
		list->nbItems = 0;
		list->sizeItems = 0;
	}
	else if(list->nbItems -1 == idx) {
		list->nbItems--;
	}
	else {
		for(i = idx; i < list->nbItems -1; i++)
			list->items[i] = list->items[i+1];
		list->nbItems--;
	}
	return 0;
}
/**
 * @annot:  a schema type structure
 *
 * Deallocate a annotation structure
 */
static void FASTCALL xmlSchemaItemListFree(xmlSchemaItemList * pList)
{
	if(pList) {
		SAlloc::F(pList->items);
		SAlloc::F(pList);
	}
}

static void xmlSchemaBucketFree(xmlSchemaBucket * pBucket)
{
	if(pBucket) {
		if(pBucket->globals) {
			xmlSchemaComponentListFree(pBucket->globals);
			xmlSchemaItemListFree(pBucket->globals);
		}
		if(pBucket->locals) {
			xmlSchemaComponentListFree(pBucket->locals);
			xmlSchemaItemListFree(pBucket->locals);
		}
		if(pBucket->relations) {
			xmlSchemaSchemaRelation * prev;
			xmlSchemaSchemaRelation * cur = pBucket->relations;
			do {
				prev = cur;
				cur = cur->next;
				SAlloc::F(prev);
			} while(cur);
		}
		if((!pBucket->preserveDoc) && pBucket->doc) {
			xmlFreeDoc(pBucket->doc);
		}
		if(pBucket->type == XML_SCHEMA_SCHEMA_IMPORT) {
			if(WXS_IMPBUCKET(pBucket)->schema)
				xmlSchemaFree(WXS_IMPBUCKET(pBucket)->schema);
		}
		SAlloc::F(pBucket);
	}
}

static xmlSchemaBucketPtr xmlSchemaBucketCreate(xmlSchemaParserCtxtPtr pctxt, int type, const xmlChar * targetNamespace)
{
	xmlSchemaBucketPtr ret;
	int size;
	xmlSchemaPtr mainSchema;
	if(WXS_CONSTRUCTOR(pctxt)->mainSchema == NULL) {
		PERROR_INT("xmlSchemaBucketCreate", "no main schema on constructor");
		return 0;
	}
	mainSchema = WXS_CONSTRUCTOR(pctxt)->mainSchema;
	/* Create the schema bucket. */
	if(WXS_IS_BUCKET_INCREDEF(type))
		size = sizeof(xmlSchemaInclude);
	else
		size = sizeof(xmlSchemaImport);
	ret = (xmlSchemaBucketPtr)SAlloc::M(size);
	if(!ret) {
		xmlSchemaPErrMemory(NULL, "allocating schema bucket", 0);
		return 0;
	}
	memzero(ret, size);
	ret->targetNamespace = targetNamespace;
	ret->type = type;
	ret->globals = xmlSchemaItemListCreate();
	if(ret->globals == NULL) {
		SAlloc::F(ret);
		return 0;
	}
	ret->locals = xmlSchemaItemListCreate();
	if(ret->locals == NULL) {
		SAlloc::F(ret);
		return 0;
	}
	/*
	 * The following will assure that only the first bucket is marked as
	 * XML_SCHEMA_SCHEMA_MAIN and it points to the *main* schema.
	 * For each following import buckets an xmlSchema will be created.
	 * An xmlSchema will be created for every distinct targetNamespace.
	 * We assign the targetNamespace to the schemata here.
	 */
	if(!WXS_HAS_BUCKETS(pctxt)) {
		if(WXS_IS_BUCKET_INCREDEF(type)) {
			PERROR_INT("xmlSchemaBucketCreate", "first bucket but it's an include or redefine");
			xmlSchemaBucketFree(ret);
			return 0;
		}
		// Force the type to be XML_SCHEMA_SCHEMA_MAIN. 
		ret->type = XML_SCHEMA_SCHEMA_MAIN;
		// Point to the *main* schema. 
		WXS_CONSTRUCTOR(pctxt)->mainBucket = ret;
		WXS_IMPBUCKET(ret)->schema = mainSchema;
		/*
		 * Ensure that the main schema gets a targetNamespace.
		 */
		mainSchema->targetNamespace = targetNamespace;
	}
	else {
		if(type == XML_SCHEMA_SCHEMA_MAIN) {
			PERROR_INT("xmlSchemaBucketCreate", "main bucket but it's not the first one");
			xmlSchemaBucketFree(ret);
			return 0;
		}
		else if(type == XML_SCHEMA_SCHEMA_IMPORT) {
			/*
			 * Create a schema for imports and assign the targetNamespace.
			 */
			WXS_IMPBUCKET(ret)->schema = xmlSchemaNewSchema(pctxt);
			if(WXS_IMPBUCKET(ret)->schema == NULL) {
				xmlSchemaBucketFree(ret);
				return 0;
			}
			WXS_IMPBUCKET(ret)->schema->targetNamespace = targetNamespace;
		}
	}
	if(WXS_IS_BUCKET_IMPMAIN(type)) {
		int res;
		/*
		 * Imports go into the "schemasImports" slot of the main *schema*.
		 * Note that we create an import entry for the main schema as well; i.e.,
		 * even if there's only one schema, we'll get an import.
		 */
		if(mainSchema->schemasImports == NULL) {
			mainSchema->schemasImports = xmlHashCreateDict(5, WXS_CONSTRUCTOR(pctxt)->dict);
			if(mainSchema->schemasImports == NULL) {
				xmlSchemaBucketFree(ret);
				return 0;
			}
		}
		if(targetNamespace == NULL)
			res = xmlHashAddEntry(mainSchema->schemasImports, XML_SCHEMAS_NO_NAMESPACE, ret);
		else
			res = xmlHashAddEntry(mainSchema->schemasImports, targetNamespace, ret);
		if(res != 0) {
			PERROR_INT("xmlSchemaBucketCreate", "failed to add the schema bucket to the hash");
			xmlSchemaBucketFree(ret);
			return 0;
		}
	}
	else {
		/* Set the @ownerImport of an include bucket. */
		if(WXS_IS_BUCKET_IMPMAIN(WXS_CONSTRUCTOR(pctxt)->bucket->type))
			WXS_INCBUCKET(ret)->ownerImport = WXS_IMPBUCKET(WXS_CONSTRUCTOR(pctxt)->bucket);
		else
			WXS_INCBUCKET(ret)->ownerImport = WXS_INCBUCKET(WXS_CONSTRUCTOR(pctxt)->bucket)->ownerImport;

		/* Includes got into the "includes" slot of the *main* schema. */
		if(mainSchema->includes == NULL) {
			mainSchema->includes = xmlSchemaItemListCreate();
			if(mainSchema->includes == NULL) {
				xmlSchemaBucketFree(ret);
				return 0;
			}
		}
		xmlSchemaItemListAdd((xmlSchemaItemListPtr)mainSchema->includes, ret);
	}
	/*
	 * Add to list of all buckets; this is used for lookup
	 * during schema construction time only.
	 */
	if(xmlSchemaItemListAdd(WXS_CONSTRUCTOR(pctxt)->buckets, ret) == -1)
		return 0;
	return ret;
}

static int xmlSchemaAddItemSize(xmlSchemaItemListPtr * list, int initialSize, void * item)
{
	if(*list == NULL) {
		*list = xmlSchemaItemListCreate();
		if(*list == NULL)
			return -1;
	}
	xmlSchemaItemListAddSize(*list, initialSize, item);
	return 0;
}
/**
 * @annot:  a schema type structure
 *
 * Deallocate a annotation structure
 */
static void FASTCALL xmlSchemaFreeAnnot(xmlSchemaAnnot * pAnnot)
{
	if(pAnnot) {
		if(pAnnot->next == NULL) {
			SAlloc::F(pAnnot);
		}
		else {
			do {
				xmlSchemaAnnot * p_prev = pAnnot;
				pAnnot = pAnnot->next;
				SAlloc::F(p_prev);
			} while(pAnnot);
		}
	}
}

/**
 * xmlSchemaFreeNotation:
 * @schema:  a schema notation structure
 *
 * Deallocate a Schema Notation structure.
 */
static void xmlSchemaFreeNotation(xmlSchemaNotationPtr nota)
{
	SAlloc::F(nota);
}
/**
 * @attr:  an attribute declaration
 *
 * Deallocates an attribute declaration structure.
 */
static void xmlSchemaFreeAttribute(xmlSchemaAttribute * pAttr)
{
	if(pAttr) {
		xmlSchemaFreeAnnot(pAttr->annot);
		xmlSchemaFreeValue(pAttr->defVal);
		SAlloc::F(pAttr);
	}
}

/**
 * xmlSchemaFreeAttributeUse:
 * @use:  an attribute use
 *
 * Deallocates an attribute use structure.
 */
static void xmlSchemaFreeAttributeUse(xmlSchemaAttributeUsePtr use)
{
	if(use) {
		xmlSchemaFreeAnnot(use->annot);
		xmlSchemaFreeValue(use->defVal);
		SAlloc::F(use);
	}
}

/**
 * xmlSchemaFreeAttributeUseProhib:
 * @prohib:  an attribute use prohibition
 *
 * Deallocates an attribute use structure.
 */
static void xmlSchemaFreeAttributeUseProhib(xmlSchemaAttributeUseProhibPtr prohib)
{
	SAlloc::F(prohib);
}
/**
 * set:  a schema wildcard namespace
 *
 * Deallocates a list of wildcard constraint structures.
 */
static void FASTCALL xmlSchemaFreeWildcardNsSet(xmlSchemaWildcardNsPtr pSet)
{
	while(pSet) {
		xmlSchemaWildcardNs * p_next = pSet->next;
		SAlloc::F(pSet);
		pSet = p_next;
	}
}
/**
 * xmlSchemaFreeWildcard:
 * @wildcard:  a wildcard structure
 *
 * Deallocates a wildcard structure.
 */
void xmlSchemaFreeWildcard(xmlSchemaWildcardPtr wildcard)
{
	if(wildcard) {
		xmlSchemaFreeAnnot(wildcard->annot);
		xmlSchemaFreeWildcardNsSet(wildcard->nsSet);
		SAlloc::F(wildcard->negNsSet);
		SAlloc::F(wildcard);
	}
}

/**
 * xmlSchemaFreeAttributeGroup:
 * @schema:  a schema attribute group structure
 *
 * Deallocate a Schema Attribute Group structure.
 */
static void xmlSchemaFreeAttributeGroup(xmlSchemaAttributeGroupPtr attrGr)
{
	if(attrGr) {
		xmlSchemaFreeAnnot(attrGr->annot);
		xmlSchemaItemListFree(WXS_LIST_CAST attrGr->attrUses);
		SAlloc::F(attrGr);
	}
}

/**
 * xmlSchemaFreeQNameRef:
 * @item: a QName reference structure
 *
 * Deallocatea a QName reference structure.
 */
static void xmlSchemaFreeQNameRef(xmlSchemaQNameRefPtr item)
{
	SAlloc::F(item);
}

/**
 * xmlSchemaFreeTypeLinkList:
 * @alink: a type link
 *
 * Deallocate a list of types.
 */
static void xmlSchemaFreeTypeLinkList(xmlSchemaTypeLink * link)
{
	while(link) {
		xmlSchemaTypeLink * next = link->next;
		SAlloc::F(link);
		link = next;
	}
}

static void xmlSchemaFreeIDCStateObjList(xmlSchemaIDCStateObj * sto)
{
	while(sto) {
		xmlSchemaIDCStateObj * next = sto->next;
		SAlloc::F(sto->history);
		xmlFreeStreamCtxt((xmlStreamCtxtPtr)sto->xpathCtxt);
		SAlloc::F(sto);
		sto = next;
	}
}

/**
 * xmlSchemaFreeIDC:
 * @idc: a identity-constraint definition
 *
 * Deallocates an identity-constraint definition.
 */
static void xmlSchemaFreeIDC(xmlSchemaIDCPtr idcDef)
{
	if(idcDef) {
		xmlSchemaIDCSelectPtr cur, prev;
		xmlSchemaFreeAnnot(idcDef->annot);
		/* Selector */
		if(idcDef->selector) {
			xmlFreePattern((xmlPatternPtr)idcDef->selector->xpathComp);
			SAlloc::F(idcDef->selector);
		}
		/* Fields */
		if(idcDef->fields) {
			cur = idcDef->fields;
			do {
				prev = cur;
				cur = cur->next;
				xmlFreePattern((xmlPatternPtr)prev->xpathComp);
				SAlloc::F(prev);
			} while(cur);
		}
		SAlloc::F(idcDef);
	}
}

/**
 * xmlSchemaFreeElement:
 * @schema:  a schema element structure
 *
 * Deallocate a Schema Element structure.
 */
static void xmlSchemaFreeElement(xmlSchemaElementPtr elem)
{
	if(elem) {
		xmlSchemaFreeAnnot(elem->annot);
		xmlRegFreeRegexp(elem->contModel);
		xmlSchemaFreeValue(elem->defVal);
		SAlloc::F(elem);
	}
}
/**
 * xmlSchemaFreeFacet:
 * @facet:  a schema facet structure
 *
 * Deallocate a Schema Facet structure.
 */
void xmlSchemaFreeFacet(xmlSchemaFacetPtr facet)
{
	if(facet) {
		xmlSchemaFreeValue(facet->val);
		xmlRegFreeRegexp(facet->regexp);
		xmlSchemaFreeAnnot(facet->annot);
		SAlloc::F(facet);
	}
}
/**
 * xmlSchemaFreeType:
 * @type:  a schema type structure
 *
 * Deallocate a Schema Type structure.
 */
void xmlSchemaFreeType(xmlSchemaType * pType)
{
	if(pType) {
		xmlSchemaFreeAnnot(pType->annot);
		for(xmlSchemaFacet * p_facet = pType->facets; p_facet;) {
			xmlSchemaFacet * next = p_facet->next;
			xmlSchemaFreeFacet(p_facet);
			p_facet = next;
		}
		xmlSchemaItemListFree((xmlSchemaItemListPtr)pType->attrUses);
		xmlSchemaFreeTypeLinkList(pType->memberTypes);
		if(pType->facetSet) {
			xmlSchemaFacetLink * link = pType->facetSet;
			do {
				xmlSchemaFacetLink * next = link->next;
				SAlloc::F(link);
				link = next;
			} while(link);
		}
		xmlRegFreeRegexp(pType->contModel);
		SAlloc::F(pType);
	}
}
/**
 * xmlSchemaFreeModelGroupDef:
 * @item:  a schema model group definition
 *
 * Deallocates a schema model group definition.
 */
static void xmlSchemaFreeModelGroupDef(xmlSchemaModelGroupDefPtr item)
{
	if(item) {
		xmlSchemaFreeAnnot(item->annot);
		SAlloc::F(item);
	}
}

/**
 * xmlSchemaFreeModelGroup:
 * @item:  a schema model group
 *
 * Deallocates a schema model group structure.
 */
static void xmlSchemaFreeModelGroup(xmlSchemaModelGroupPtr item)
{
	if(item) {
		xmlSchemaFreeAnnot(item->annot);
		SAlloc::F(item);
	}
}

static void xmlSchemaComponentListFree(xmlSchemaItemListPtr list)
{
	if(list || list->nbItems) {
		xmlSchemaTreeItem * item;
		xmlSchemaTreeItem ** items = (xmlSchemaTreeItem **)list->items;
		int i;
		for(i = 0; i < list->nbItems; i++) {
			item = items[i];
			if(item == NULL)
				continue;
			switch(item->type) {
				case XML_SCHEMA_TYPE_SIMPLE:
				case XML_SCHEMA_TYPE_COMPLEX:
				    xmlSchemaFreeType((xmlSchemaTypePtr)item);
				    break;
				case XML_SCHEMA_TYPE_ATTRIBUTE:
				    xmlSchemaFreeAttribute((xmlSchemaAttributePtr)item);
				    break;
				case XML_SCHEMA_TYPE_ATTRIBUTE_USE:
				    xmlSchemaFreeAttributeUse((xmlSchemaAttributeUsePtr)item);
				    break;
				case XML_SCHEMA_EXTRA_ATTR_USE_PROHIB:
				    xmlSchemaFreeAttributeUseProhib(
				    (xmlSchemaAttributeUseProhibPtr)item);
				    break;
				case XML_SCHEMA_TYPE_ELEMENT:
				    xmlSchemaFreeElement((xmlSchemaElementPtr)item);
				    break;
				case XML_SCHEMA_TYPE_PARTICLE:
				    xmlSchemaFreeAnnot(item->annot);
				    SAlloc::F(item);
				    break;
				case XML_SCHEMA_TYPE_SEQUENCE:
				case XML_SCHEMA_TYPE_CHOICE:
				case XML_SCHEMA_TYPE_ALL:
				    xmlSchemaFreeModelGroup((xmlSchemaModelGroupPtr)item);
				    break;
				case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
				    xmlSchemaFreeAttributeGroup(
				    (xmlSchemaAttributeGroupPtr)item);
				    break;
				case XML_SCHEMA_TYPE_GROUP:
				    xmlSchemaFreeModelGroupDef(
				    (xmlSchemaModelGroupDefPtr)item);
				    break;
				case XML_SCHEMA_TYPE_ANY:
				case XML_SCHEMA_TYPE_ANY_ATTRIBUTE:
				    xmlSchemaFreeWildcard((xmlSchemaWildcardPtr)item);
				    break;
				case XML_SCHEMA_TYPE_IDC_KEY:
				case XML_SCHEMA_TYPE_IDC_UNIQUE:
				case XML_SCHEMA_TYPE_IDC_KEYREF:
				    xmlSchemaFreeIDC((xmlSchemaIDCPtr)item);
				    break;
				case XML_SCHEMA_TYPE_NOTATION:
				    xmlSchemaFreeNotation((xmlSchemaNotationPtr)item);
				    break;
				case XML_SCHEMA_EXTRA_QNAMEREF:
				    xmlSchemaFreeQNameRef((xmlSchemaQNameRefPtr)item);
				    break;
				default: {
				    /* @todo This should never be hit. */
				    xmlSchemaPSimpleInternalErr(NULL, "Internal error: xmlSchemaComponentListFree, unexpected component type '%s'\n", (const xmlChar*)WXS_ITEM_TYPE_NAME(item));
			    }
			    break;
			}
		}
		list->nbItems = 0;
	}
}

/**
 * xmlSchemaFree:
 * @schema:  a schema structure
 *
 * Deallocate a Schema structure.
 */
void FASTCALL xmlSchemaFree(xmlSchema * schema)
{
	if(schema) {
		/* @volatiles is not used anymore :-/ */
		if(schema->volatiles)
			TODO
			/*
			* Note that those slots are not responsible for freeing
			* schema components anymore; this will now be done by
			* the schema buckets.
			*/
			xmlHashFree(schema->notaDecl, 0);
		xmlHashFree(schema->attrDecl, 0);
		xmlHashFree(schema->attrgrpDecl, 0);
		xmlHashFree(schema->elemDecl, 0);
		xmlHashFree(schema->typeDecl, 0);
		xmlHashFree(schema->groupDecl, 0);
		xmlHashFree(schema->idcDef, 0);
		xmlHashFree(schema->schemasImports, (xmlHashDeallocator)xmlSchemaBucketFree);
		if(schema->includes) {
			xmlSchemaItemListPtr list = (xmlSchemaItemListPtr)schema->includes;
			for(int i = 0; i < list->nbItems; i++) {
				xmlSchemaBucketFree((xmlSchemaBucketPtr)list->items[i]);
			}
			xmlSchemaItemListFree(list);
		}
		xmlSchemaFreeAnnot(schema->annot);
		// Never free the doc here, since this will be done by the buckets
		xmlDictFree(schema->dict);
		SAlloc::F(schema);
	}
}

/************************************************************************
*									*
*			Debug functions					*
*									*
************************************************************************/

#ifdef LIBXML_OUTPUT_ENABLED

static void xmlSchemaTypeDump(xmlSchemaTypePtr type, FILE * output); /* forward */
/**
 * xmlSchemaElementDump:
 * @elem:  an element
 * @output:  the file output
 *
 * Dump the element
 */
static void xmlSchemaElementDump(xmlSchemaElementPtr elem, FILE * output, const xmlChar * name ATTRIBUTE_UNUSED,
    const xmlChar * pNamespace ATTRIBUTE_UNUSED, const xmlChar * context ATTRIBUTE_UNUSED)
{
	if(elem) {
		fprintf(output, "Element");
		if(elem->flags & XML_SCHEMAS_ELEM_GLOBAL)
			fprintf(output, " (global)");
		fprintf(output, ": '%s' ", elem->name);
		if(pNamespace)
			fprintf(output, "ns '%s'", pNamespace);
		fprintf(output, "\n");
#if 0
		if((elem->minOccurs != 1) || (elem->maxOccurs != 1)) {
			fprintf(output, "  min %d ", elem->minOccurs);
			if(elem->maxOccurs >= UNBOUNDED)
				fprintf(output, "max: unbounded\n");
			else if(elem->maxOccurs != 1)
				fprintf(output, "max: %d\n", elem->maxOccurs);
			else
				fprintf(output, "\n");
		}
#endif
		/*
		 * Misc other properties.
		 */
		if((elem->flags & XML_SCHEMAS_ELEM_NILLABLE) || (elem->flags & XML_SCHEMAS_ELEM_ABSTRACT) || (elem->flags & XML_SCHEMAS_ELEM_FIXED) || (elem->flags & XML_SCHEMAS_ELEM_DEFAULT)) {
			fprintf(output, "  props: ");
			if(elem->flags & XML_SCHEMAS_ELEM_FIXED)
				fprintf(output, "[fixed] ");
			if(elem->flags & XML_SCHEMAS_ELEM_DEFAULT)
				fprintf(output, "[default] ");
			if(elem->flags & XML_SCHEMAS_ELEM_ABSTRACT)
				fprintf(output, "[abstract] ");
			if(elem->flags & XML_SCHEMAS_ELEM_NILLABLE)
				fprintf(output, "[nillable] ");
			fprintf(output, "\n");
		}
		// 
		// Default/fixed value.
		// 
		if(elem->value)
			fprintf(output, "  value: '%s'\n", elem->value);
		// 
		// Type.
		// 
		if(elem->namedType) {
			fprintf(output, "  type: '%s' ", elem->namedType);
			if(elem->namedTypeNs)
				fprintf(output, "ns '%s'\n", elem->namedTypeNs);
			else
				fprintf(output, "\n");
		}
		else if(elem->subtypes) {
			// Dump local types.
			xmlSchemaTypeDump(elem->subtypes, output);
		}
		// 
		// Substitution group.
		// 
		if(elem->substGroup) {
			fprintf(output, "  substitutionGroup: '%s' ", elem->substGroup);
			if(elem->substGroupNs)
				fprintf(output, "ns '%s'\n", elem->substGroupNs);
			else
				fprintf(output, "\n");
		}
	}
}

/**
 * xmlSchemaAnnotDump:
 * @output:  the file output
 * @annot:  a annotation
 *
 * Dump the annotation
 */
static void xmlSchemaAnnotDump(FILE * output, xmlSchemaAnnotPtr annot)
{
	if(annot) {
		xmlChar * content = xmlNodeGetContent(annot->content);
		if(content) {
			fprintf(output, "  Annot: %s\n", content);
			SAlloc::F(content);
		}
		else
			fprintf(output, "  Annot: empty\n");
	}
}

/**
 * xmlSchemaContentModelDump:
 * @particle: the schema particle
 * @output: the file output
 * @depth: the depth used for intentation
 *
 * Dump a SchemaType structure
 */
static void xmlSchemaContentModelDump(xmlSchemaParticlePtr particle, FILE * output, int depth)
{
	xmlChar * str = NULL;
	xmlSchemaTreeItem * term;
	char shift[100];
	int i;
	if(particle == NULL)
		return;
	for(i = 0; ((i < depth) && (i < 25)); i++)
		shift[2 * i] = shift[2 * i + 1] = ' ';
	shift[2 * i] = shift[2 * i + 1] = 0;
	fprintf(output, "%s", shift);
	if(particle->children == NULL) {
		fprintf(output, "MISSING particle term\n");
		return;
	}
	term = particle->children;
	if(term == NULL) {
		fprintf(output, "(NULL)");
	}
	else {
		switch(term->type) {
			case XML_SCHEMA_TYPE_ELEMENT:
			    fprintf(output, "ELEM '%s'", xmlSchemaFormatQName(&str, ((xmlSchemaElementPtr)term)->targetNamespace, ((xmlSchemaElementPtr)term)->name));
			    ZFREE(str);
			    break;
			case XML_SCHEMA_TYPE_SEQUENCE:
			    fprintf(output, "SEQUENCE");
			    break;
			case XML_SCHEMA_TYPE_CHOICE:
			    fprintf(output, "CHOICE");
			    break;
			case XML_SCHEMA_TYPE_ALL:
			    fprintf(output, "ALL");
			    break;
			case XML_SCHEMA_TYPE_ANY:
			    fprintf(output, "ANY");
			    break;
			default:
			    fprintf(output, "UNKNOWN\n");
			    return;
		}
	}
	if(particle->minOccurs != 1)
		fprintf(output, " min: %d", particle->minOccurs);
	if(particle->maxOccurs >= UNBOUNDED)
		fprintf(output, " max: unbounded");
	else if(particle->maxOccurs != 1)
		fprintf(output, " max: %d", particle->maxOccurs);
	fprintf(output, "\n");
	if(term && ((term->type == XML_SCHEMA_TYPE_SEQUENCE) || (term->type == XML_SCHEMA_TYPE_CHOICE) || (term->type == XML_SCHEMA_TYPE_ALL)) && term->children) {
		xmlSchemaContentModelDump((xmlSchemaParticlePtr)term->children, output, depth +1);
	}
	if(particle->next)
		xmlSchemaContentModelDump((xmlSchemaParticlePtr)particle->next, output, depth);
}

/**
 * xmlSchemaAttrUsesDump:
 * @uses:  attribute uses list
 * @output:  the file output
 *
 * Dumps a list of attribute use components.
 */
static void xmlSchemaAttrUsesDump(xmlSchemaItemListPtr uses, FILE * output)
{
	xmlSchemaAttributeUsePtr use;
	xmlSchemaAttributeUseProhibPtr prohib;
	xmlSchemaQNameRefPtr ref;
	const xmlChar * name, * tns;
	xmlChar * str = NULL;
	int i;
	if((uses == NULL) || (uses->nbItems == 0))
		return;
	fprintf(output, "  attributes:\n");
	for(i = 0; i < uses->nbItems; i++) {
		use = (xmlSchemaAttributeUsePtr)uses->items[i];
		if(use->type == XML_SCHEMA_EXTRA_ATTR_USE_PROHIB) {
			fprintf(output, "  [prohibition] ");
			prohib = (xmlSchemaAttributeUseProhibPtr)use;
			name = prohib->name;
			tns = prohib->targetNamespace;
		}
		else if(use->type == XML_SCHEMA_EXTRA_QNAMEREF) {
			fprintf(output, "  [reference] ");
			ref = (xmlSchemaQNameRefPtr)use;
			name = ref->name;
			tns = ref->targetNamespace;
		}
		else {
			fprintf(output, "  [use] ");
			name = WXS_ATTRUSE_DECL_NAME(use);
			tns = WXS_ATTRUSE_DECL_TNS(use);
		}
		fprintf(output, "'%s'\n", (const char*)xmlSchemaFormatQName(&str, tns, name));
		ZFREE(str);
	}
}

/**
 * xmlSchemaTypeDump:
 * @output:  the file output
 * @type:  a type structure
 *
 * Dump a SchemaType structure
 */
static void xmlSchemaTypeDump(xmlSchemaTypePtr type, FILE * output)
{
	if(type == NULL) {
		fprintf(output, "Type: NULL\n");
		return;
	}
	fprintf(output, "Type: ");
	if(type->name)
		fprintf(output, "'%s' ", type->name);
	else
		fprintf(output, "(no name) ");
	if(type->targetNamespace)
		fprintf(output, "ns '%s' ", type->targetNamespace);
	switch(type->type) {
		case XML_SCHEMA_TYPE_BASIC:
		    fprintf(output, "[basic] ");
		    break;
		case XML_SCHEMA_TYPE_SIMPLE:
		    fprintf(output, "[simple] ");
		    break;
		case XML_SCHEMA_TYPE_COMPLEX:
		    fprintf(output, "[complex] ");
		    break;
		case XML_SCHEMA_TYPE_SEQUENCE:
		    fprintf(output, "[sequence] ");
		    break;
		case XML_SCHEMA_TYPE_CHOICE:
		    fprintf(output, "[choice] ");
		    break;
		case XML_SCHEMA_TYPE_ALL:
		    fprintf(output, "[all] ");
		    break;
		case XML_SCHEMA_TYPE_UR:
		    fprintf(output, "[ur] ");
		    break;
		case XML_SCHEMA_TYPE_RESTRICTION:
		    fprintf(output, "[restriction] ");
		    break;
		case XML_SCHEMA_TYPE_EXTENSION:
		    fprintf(output, "[extension] ");
		    break;
		default:
		    fprintf(output, "[unknown type %d] ", type->type);
		    break;
	}
	fprintf(output, "content: ");
	switch(type->contentType) {
		case XML_SCHEMA_CONTENT_UNKNOWN:
		    fprintf(output, "[unknown] ");
		    break;
		case XML_SCHEMA_CONTENT_EMPTY:
		    fprintf(output, "[empty] ");
		    break;
		case XML_SCHEMA_CONTENT_ELEMENTS:
		    fprintf(output, "[element] ");
		    break;
		case XML_SCHEMA_CONTENT_MIXED:
		    fprintf(output, "[mixed] ");
		    break;
		case XML_SCHEMA_CONTENT_MIXED_OR_ELEMENTS:
		    /* not used. */
		    break;
		case XML_SCHEMA_CONTENT_BASIC:
		    fprintf(output, "[basic] ");
		    break;
		case XML_SCHEMA_CONTENT_SIMPLE:
		    fprintf(output, "[simple] ");
		    break;
		case XML_SCHEMA_CONTENT_ANY:
		    fprintf(output, "[any] ");
		    break;
	}
	fprintf(output, "\n");
	if(type->base) {
		fprintf(output, "  base type: '%s'", type->base);
		if(type->baseNs)
			fprintf(output, " ns '%s'\n", type->baseNs);
		else
			fprintf(output, "\n");
	}
	if(type->attrUses)
		xmlSchemaAttrUsesDump((xmlSchemaItemListPtr)type->attrUses, output);
	if(type->annot)
		xmlSchemaAnnotDump(output, type->annot);
#ifdef DUMP_CONTENT_MODEL
	if((type->type == XML_SCHEMA_TYPE_COMPLEX) && type->subtypes) {
		xmlSchemaContentModelDump((xmlSchemaParticlePtr)type->subtypes, output, 1);
	}
#endif
}

/**
 * xmlSchemaDump:
 * @output:  the file output
 * @schema:  a schema structure
 *
 * Dump a Schema structure.
 */
void xmlSchemaDump(FILE * output, xmlSchemaPtr schema)
{
	if(output) {
		if(!schema)
			fprintf(output, "Schemas: NULL\n");
		else {
			fprintf(output, "Schemas: ");
			if(schema->name)
				fprintf(output, "%s, ", schema->name);
			else
				fprintf(output, "no name, ");
			if(schema->targetNamespace)
				fprintf(output, "%s", (const char*)schema->targetNamespace);
			else
				fprintf(output, "no target namespace");
			fprintf(output, "\n");
			if(schema->annot)
				xmlSchemaAnnotDump(output, schema->annot);
			xmlHashScan(schema->typeDecl, (xmlHashScanner)xmlSchemaTypeDump, output);
			xmlHashScanFull(schema->elemDecl, (xmlHashScannerFull)xmlSchemaElementDump, output);
		}
	}
}

#ifdef DEBUG_IDC_NODE_TABLE
/**
 * xmlSchemaDebugDumpIDCTable:
 * @vctxt: the WXS validation context
 *
 * Displays the current IDC table for debug purposes.
 */
static void xmlSchemaDebugDumpIDCTable(FILE * output,
    const xmlChar * namespaceName,
    const xmlChar * localName,
    xmlSchemaPSVIIDCBindingPtr bind)
{
	xmlChar * str = NULL;
	const xmlChar * value;
	xmlSchemaPSVIIDCNodePtr tab;
	xmlSchemaPSVIIDCKeyPtr key;
	int i, j, res;
	fprintf(output, "IDC: TABLES on '%s'\n", xmlSchemaFormatQName(&str, namespaceName, localName));
	ZFREE(str);
	if(bind == NULL)
		return;
	do {
		fprintf(output, "IDC:   BINDING '%s' (%d)\n", xmlSchemaGetComponentQName(&str, bind->definition), bind->nbNodes);
		ZFREE(str);
		for(i = 0; i < bind->nbNodes; i++) {
			tab = bind->nodeTable[i];
			fprintf(output, "         ( ");
			for(j = 0; j < bind->definition->nbFields; j++) {
				key = tab->keys[j];
				if(key && key->val) {
					res = xmlSchemaGetCanonValue(key->val, &value);
					if(res >= 0)
						fprintf(output, "'%s' ", value);
					else
						fprintf(output, "CANON-VALUE-FAILED ");
					if(res == 0)
						ZFREE(value);
						}
						else if(key)
							fprintf(output, "(no val), ");
						else
							fprintf(output, "(key missing), ");
				}
				fprintf(output, ")\n");
			}
			if(bind->dupls && bind->dupls->nbItems) {
				fprintf(output, "IDC:     dupls (%d):\n", bind->dupls->nbItems);
				for(i = 0; i < bind->dupls->nbItems; i++) {
					tab = bind->dupls->items[i];
					fprintf(output, "         ( ");
					for(j = 0; j < bind->definition->nbFields; j++) {
						key = tab->keys[j];
						if(key && key->val) {
							res = xmlSchemaGetCanonValue(key->val, &value);
							if(res >= 0)
								fprintf(output, "'%s' ", value);
							else
								fprintf(output, "CANON-VALUE-FAILED ");
							if(res == 0)
								ZFREE(value);
								}
								else if(key)
									fprintf(output, "(no val), ");
								else
									fprintf(output, "(key missing), ");
						}
						fprintf(output, ")\n");
					}
				}
				bind = bind->next;
			}
			while(bind) ;
		}
#endif /* DEBUG_IDC */
#endif /* LIBXML_OUTPUT_ENABLED */

/************************************************************************
*									*
*			Utilities					*
*									*
************************************************************************/

/**
 * xmlSchemaGetPropNode:
 * @node: the element node
 * @name: the name of the attribute
 *
 * Seeks an attribute with a name of @name in
 * no namespace.
 *
 * Returns the attribute or NULL if not present.
 */
static xmlAttr * FASTCALL xmlSchemaGetPropNode(xmlNode * P_Node, const char * name)
{
	if(P_Node && name) {
		for(xmlAttr * prop = P_Node->properties; prop; prop = prop->next)
			if(!prop->ns && sstreq(prop->name, BAD_CAST name))
				return prop;
	}
	return 0;
}
/**
 * xmlSchemaGetPropNodeNs:
 * @node: the element node
 * @uri: the uri
 * @name: the name of the attribute
 *
 * Seeks an attribute with a local name of @name and
 * a namespace URI of @uri.
 *
 * Returns the attribute or NULL if not present.
 */
static xmlAttrPtr xmlSchemaGetPropNodeNs(xmlNode * P_Node, const char * uri, const char * name)
{
	if(P_Node && name) {
		for(xmlAttr * prop = P_Node->properties; prop; prop = prop->next) {
			if(prop->ns && sstreq(prop->name, BAD_CAST name) && sstreq(prop->ns->href, BAD_CAST uri))
				return prop;
		}
	}
	return 0;
}

static const xmlChar * FASTCALL xmlSchemaGetNodeContent(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node)
{
	const xmlChar * ret;
	xmlChar * val = xmlNodeGetContent(P_Node);
	SETIFZ(val, sstrdup((xmlChar*)""));
	ret = xmlDictLookupSL(ctxt->dict, val);
	SAlloc::F(val);
	return ret;
}

static const xmlChar * xmlSchemaGetNodeContentNoDict(xmlNode * P_Node)
{
	return (const xmlChar*)xmlNodeGetContent(P_Node);
}

/**
 * xmlSchemaGetProp:
 * @ctxt: the parser context
 * @node: the node
 * @name: the property name
 *
 * Read a attribute value and internalize the string
 *
 * Returns the string or NULL if not present.
 */
static const xmlChar * xmlSchemaGetProp(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, const char * name)
{
	const xmlChar * ret = 0;
	xmlChar * val = xmlGetNoNsProp(P_Node, BAD_CAST name);
	if(val) {
		ret = xmlDictLookupSL(ctxt->dict, val);
		SAlloc::F(val);
	}
	return ret;
}

/************************************************************************
*									*
*			Parsing functions				*
*									*
************************************************************************/

#define WXS_FIND_GLOBAL_ITEM(slot, RetType) \
	if(sstreq(nsName, schema->targetNamespace)) { \
		ret = (RetType)xmlHashLookup(schema->slot, name); \
		if(ret) goto exit; \
	} \
	if(xmlHashSize(schema->schemasImports) > 1) { \
		xmlSchemaImportPtr import = (xmlSchemaImportPtr)xmlHashLookup(schema->schemasImports, nsName ? nsName : XML_SCHEMAS_NO_NAMESPACE); \
		if(import == NULL) goto exit; \
		ret = (RetType)xmlHashLookup(import->schema->slot, name); \
	}

/**
 * xmlSchemaGetElem:
 * @schema:  the schema context
 * @name:  the element name
 * @ns:  the element namespace
 *
 * Lookup a global element declaration in the schema.
 *
 * Returns the element declaration or NULL if not found.
 */
static xmlSchemaElementPtr xmlSchemaGetElem(xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaElementPtr ret = NULL;
	if((name == NULL) || (schema == NULL))
		return 0;
	if(schema) {
		WXS_FIND_GLOBAL_ITEM(elemDecl, xmlSchemaElementPtr)
	}
exit:
#ifdef DEBUG
	if(!ret) {
		if(nsName == NULL)
			fprintf(stderr, "Unable to lookup element decl. %s", name);
		else
			fprintf(stderr, "Unable to lookup element decl. %s:%s", name, nsName);
	}
#endif
	return ret;
}

/**
 * xmlSchemaGetType:
 * @schema:  the main schema
 * @name:  the type's name
 * nsName:  the type's namespace
 *
 * Lookup a type in the schemas or the predefined types
 *
 * Returns the group definition or NULL if not found.
 */
static xmlSchemaTypePtr xmlSchemaGetType(xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaTypePtr ret = NULL;
	if(!name)
		return 0;
	/* First try the built-in types. */
	if(nsName && sstreq(nsName, xmlSchemaNs)) {
		ret = xmlSchemaGetPredefinedType(name, nsName);
		if(ret)
			goto exit;
		/*
		 * Note that we try the parsed schemas as well here
		 * since one might have parsed the S4S, which contain more
		 * than the built-in types.
		 * @todo Can we optimize this?
		 */
	}
	if(schema) {
		WXS_FIND_GLOBAL_ITEM(typeDecl, xmlSchemaTypePtr)
	}
exit:
#ifdef DEBUG
	if(!ret) {
		if(nsName == NULL)
			fprintf(stderr, "Unable to lookup type %s", name);
		else
			fprintf(stderr, "Unable to lookup type %s:%s", name, nsName);
	}
#endif
	return ret;
}

/**
 * xmlSchemaGetAttributeDecl:
 * @schema:  the context of the schema
 * @name:  the name of the attribute
 * @ns:  the target namespace of the attribute
 *
 * Lookup a an attribute in the schema or imported schemas
 *
 * Returns the attribute declaration or NULL if not found.
 */
static xmlSchemaAttributePtr xmlSchemaGetAttributeDecl(xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaAttributePtr ret = NULL;
	if((name == NULL) || (schema == NULL))
		return 0;
	if(schema) {
		WXS_FIND_GLOBAL_ITEM(attrDecl, xmlSchemaAttributePtr)
	}
exit:
#ifdef DEBUG
	if(!ret) {
		if(nsName == NULL)
			fprintf(stderr, "Unable to lookup attribute %s", name);
		else
			fprintf(stderr, "Unable to lookup attribute %s:%s", name, nsName);
	}
#endif
	return ret;
}

/**
 * xmlSchemaGetAttributeGroup:
 * @schema:  the context of the schema
 * @name:  the name of the attribute group
 * @ns:  the target namespace of the attribute group
 *
 * Lookup a an attribute group in the schema or imported schemas
 *
 * Returns the attribute group definition or NULL if not found.
 */
static xmlSchemaAttributeGroupPtr xmlSchemaGetAttributeGroup(xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaAttributeGroupPtr ret = NULL;
	if((name == NULL) || (schema == NULL))
		return 0;
	if(schema) {
		WXS_FIND_GLOBAL_ITEM(attrgrpDecl, xmlSchemaAttributeGroupPtr)
	}
exit:
	/* @todo 
	   if ((ret != NULL) && (ret->redef != NULL)) {
	 * Return the last redefinition. *
	    ret = ret->redef;
	   }
	 */
#ifdef DEBUG
	if(!ret) {
		if(nsName == NULL)
			fprintf(stderr, "Unable to lookup attribute group %s", name);
		else
			fprintf(stderr, "Unable to lookup attribute group %s:%s", name, nsName);
	}
#endif
	return ret;
}

/**
 * xmlSchemaGetGroup:
 * @schema:  the context of the schema
 * @name:  the name of the group
 * @ns:  the target namespace of the group
 *
 * Lookup a group in the schema or imported schemas
 *
 * Returns the group definition or NULL if not found.
 */
static xmlSchemaModelGroupDefPtr xmlSchemaGetGroup(xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaModelGroupDefPtr ret = NULL;
	if((name == NULL) || (schema == NULL))
		return 0;
	if(schema) {
		WXS_FIND_GLOBAL_ITEM(groupDecl, xmlSchemaModelGroupDefPtr)
	}
exit:

#ifdef DEBUG
	if(!ret) {
		if(nsName == NULL)
			fprintf(stderr, "Unable to lookup group %s", name);
		else
			fprintf(stderr, "Unable to lookup group %s:%s", name, nsName);
	}
#endif
	return ret;
}

static xmlSchemaNotationPtr xmlSchemaGetNotation(xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaNotationPtr ret = NULL;
	if((name == NULL) || (schema == NULL))
		return 0;
	if(schema) {
		WXS_FIND_GLOBAL_ITEM(notaDecl, xmlSchemaNotationPtr)
	}
exit:
	return ret;
}

static xmlSchemaIDCPtr xmlSchemaGetIDC(xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaIDCPtr ret = NULL;
	if((name == NULL) || (schema == NULL))
		return 0;
	if(schema) {
		WXS_FIND_GLOBAL_ITEM(idcDef, xmlSchemaIDCPtr)
	}
exit:
	return ret;
}

/**
 * xmlSchemaGetNamedComponent:
 * @schema:  the schema
 * @name:  the name of the group
 * @ns:  the target namespace of the group
 *
 * Lookup a group in the schema or imported schemas
 *
 * Returns the group definition or NULL if not found.
 */
static xmlSchemaBasicItem * xmlSchemaGetNamedComponent(xmlSchemaPtr schema, xmlSchemaTypeType itemType, const xmlChar * name, const xmlChar * targetNs)
{
	switch(itemType) {
		case XML_SCHEMA_TYPE_GROUP:
		    return ((xmlSchemaBasicItem *)xmlSchemaGetGroup(schema, name, targetNs));
		case XML_SCHEMA_TYPE_ELEMENT:
		    return ((xmlSchemaBasicItem *)xmlSchemaGetElem(schema, name, targetNs));
		default:
		    TODO
		    return 0;
	}
}

/************************************************************************
*									*
*			Parsing functions				*
*									*
************************************************************************/

#define IS_BLANK_NODE(n)						\
	(((n)->type == XML_TEXT_NODE) && (xmlSchemaIsBlank((n)->content, -1)))

/**
 * xmlSchemaIsBlank:
 * @str:  a string
 * @len: the length of the string or -1
 *
 * Check if a string is ignorable
 *
 * Returns 1 if the string is NULL or made of blanks chars, 0 otherwise
 */
static int xmlSchemaIsBlank(xmlChar * str, int len)
{
	if(!str)
		return 1;
	if(len < 0) {
		while(*str != 0) {
			if(!(IS_BLANK_CH(*str)))
				return 0;
			str++;
		}
	}
	else while((*str != 0) && (len != 0)) {
			if(!(IS_BLANK_CH(*str)))
				return 0;
			str++;
			len--;
		}

	return 1;
}

#define WXS_COMP_NAME(c, t) ((t)(c))->name
#define WXS_COMP_TNS(c, t) ((t)(c))->targetNamespace
/*
 * xmlSchemaFindRedefCompInGraph:
 * ATTENTION TODO: This uses pointer comp. for strings.
 */
static xmlSchemaBasicItem * xmlSchemaFindRedefCompInGraph(xmlSchemaBucketPtr bucket, xmlSchemaTypeType type, const xmlChar * name, const xmlChar * nsName)
{
	xmlSchemaBasicItem * ret;
	int i;
	if((bucket == NULL) || (name == NULL))
		return 0;
	if((bucket->globals == NULL) || (bucket->globals->nbItems == 0))
		goto subschemas;
	/*
	 * Search in global components.
	 */
	for(i = 0; i < bucket->globals->nbItems; i++) {
		ret = (xmlSchemaBasicItem *)bucket->globals->items[i];
		if(ret->type == type) {
			switch(type) {
				case XML_SCHEMA_TYPE_COMPLEX:
				case XML_SCHEMA_TYPE_SIMPLE:
				    if((WXS_COMP_NAME(ret, xmlSchemaTypePtr) == name) &&
				    (WXS_COMP_TNS(ret, xmlSchemaTypePtr) ==
					    nsName)) {
					    return ret;
				    }
				    break;
				case XML_SCHEMA_TYPE_GROUP:
				    if((WXS_COMP_NAME(ret,
						    xmlSchemaModelGroupDefPtr) == name) &&
				    (WXS_COMP_TNS(ret,
						    xmlSchemaModelGroupDefPtr) == nsName)) {
					    return ret;
				    }
				    break;
				case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
				    if((WXS_COMP_NAME(ret,
						    xmlSchemaAttributeGroupPtr) == name) &&
				    (WXS_COMP_TNS(ret,
						    xmlSchemaAttributeGroupPtr) == nsName)) {
					    return ret;
				    }
				    break;
				default:
				    /* Should not be hit. */
				    return 0;
			}
		}
	}
subschemas:
	/*
	 * Process imported/included schemas.
	 */
	if(bucket->relations) {
		xmlSchemaSchemaRelationPtr rel = bucket->relations;
		/*
		 * @todo Marking the bucket will not avoid multiple searches
		 * in the same schema, but avoids at least circularity.
		 */
		bucket->flags |= XML_SCHEMA_BUCKET_MARKED;
		do {
			if(rel->bucket && ((rel->bucket->flags & XML_SCHEMA_BUCKET_MARKED) == 0)) {
				ret = xmlSchemaFindRedefCompInGraph(rel->bucket, type, name, nsName);
				if(ret)
					return ret;
			}
			rel = rel->next;
		} while(rel);
		bucket->flags ^= XML_SCHEMA_BUCKET_MARKED;
	}
	return 0;
}

/**
 * xmlSchemaAddNotation:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @name:  the item name
 *
 * Add an XML schema annotation declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaNotationPtr xmlSchemaAddNotation(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema,
    const xmlChar * name, const xmlChar * nsName,
    xmlNode * P_Node ATTRIBUTE_UNUSED)
{
	xmlSchemaNotationPtr ret = NULL;

	if(!ctxt || (schema == NULL) || (name == NULL))
		return 0;

	ret = (xmlSchemaNotationPtr)SAlloc::M(sizeof(xmlSchemaNotation));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "add annotation", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaNotation));
	ret->type = XML_SCHEMA_TYPE_NOTATION;
	ret->name = name;
	ret->targetNamespace = nsName;
	/* @todo do we need the node to be set?
	 * ret->node = node;*/
	WXS_ADD_GLOBAL(ctxt, ret);
	return ret;
}

/**
 * xmlSchemaAddAttribute:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @name:  the item name
 * @namespace:  the namespace
 *
 * Add an XML schema Attrribute declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaAttributePtr xmlSchemaAddAttribute(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema,
    const xmlChar * name, const xmlChar * nsName,
    xmlNode * P_Node, int topLevel)
{
	xmlSchemaAttributePtr ret = NULL;
	if(!ctxt || (schema == NULL))
		return 0;
	ret = (xmlSchemaAttributePtr)SAlloc::M(sizeof(xmlSchemaAttribute));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating attribute", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaAttribute));
	ret->type = XML_SCHEMA_TYPE_ATTRIBUTE;
	ret->P_Node = P_Node;
	ret->name = name;
	ret->targetNamespace = nsName;

	if(topLevel)
		WXS_ADD_GLOBAL(ctxt, ret);
	else
		WXS_ADD_LOCAL(ctxt, ret);
	WXS_ADD_PENDING(ctxt, ret);
	return ret;
}

/**
 * xmlSchemaAddAttributeUse:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @name:  the item name
 * @namespace:  the namespace
 *
 * Add an XML schema Attrribute declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaAttributeUsePtr xmlSchemaAddAttributeUse(xmlSchemaParserCtxtPtr pctxt, xmlNode * P_Node)
{
	xmlSchemaAttributeUsePtr ret = NULL;
	if(pctxt == NULL)
		return 0;

	ret = (xmlSchemaAttributeUsePtr)SAlloc::M(sizeof(xmlSchemaAttributeUse));
	if(!ret) {
		xmlSchemaPErrMemory(pctxt, "allocating attribute", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaAttributeUse));
	ret->type = XML_SCHEMA_TYPE_ATTRIBUTE_USE;
	ret->P_Node = P_Node;

	WXS_ADD_LOCAL(pctxt, ret);
	return ret;
}

/*
 * xmlSchemaAddRedef:
 *
 * Adds a redefinition information. This is used at a later stage to:
 * resolve references to the redefined components and to check constraints.
 */
static xmlSchemaRedefPtr xmlSchemaAddRedef(xmlSchemaParserCtxtPtr pctxt,
    xmlSchemaBucketPtr targetBucket, void * item, const xmlChar * refName, const xmlChar * refTargetNs)
{
	xmlSchemaRedefPtr ret;
	ret = (xmlSchemaRedefPtr)SAlloc::M(sizeof(xmlSchemaRedef));
	if(!ret) {
		xmlSchemaPErrMemory(pctxt, "allocating redefinition info", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaRedef));
	ret->item = (xmlSchemaBasicItem *)item;
	ret->targetBucket = targetBucket;
	ret->refName = refName;
	ret->refTargetNs = refTargetNs;
	if(WXS_CONSTRUCTOR(pctxt)->redefs == NULL)
		WXS_CONSTRUCTOR(pctxt)->redefs = ret;
	else
		WXS_CONSTRUCTOR(pctxt)->lastRedef->next = ret;
	WXS_CONSTRUCTOR(pctxt)->lastRedef = ret;
	return ret;
}

/**
 * xmlSchemaAddAttributeGroupDefinition:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @name:  the item name
 * @nsName:  the target namespace
 * @node: the corresponding node
 *
 * Add an XML schema Attrribute Group definition.
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaAttributeGroupPtr xmlSchemaAddAttributeGroupDefinition(xmlSchemaParserCtxtPtr pctxt,
    xmlSchemaPtr schema ATTRIBUTE_UNUSED, const xmlChar * name, const xmlChar * nsName, xmlNode * P_Node)
{
	xmlSchemaAttributeGroupPtr ret = NULL;
	if((pctxt == NULL) || (name == NULL))
		return 0;
	ret = (xmlSchemaAttributeGroupPtr)SAlloc::M(sizeof(xmlSchemaAttributeGroup));
	if(!ret) {
		xmlSchemaPErrMemory(pctxt, "allocating attribute group", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaAttributeGroup));
	ret->type = XML_SCHEMA_TYPE_ATTRIBUTEGROUP;
	ret->name = name;
	ret->targetNamespace = nsName;
	ret->P_Node = P_Node;
	/* @todo Remove the flag. */
	ret->flags |= XML_SCHEMAS_ATTRGROUP_GLOBAL;
	if(pctxt->isRedefine) {
		pctxt->redef = xmlSchemaAddRedef(pctxt, pctxt->redefined, ret, name, nsName);
		if(pctxt->redef == NULL) {
			SAlloc::F(ret);
			return 0;
		}
		pctxt->redefCounter = 0;
	}
	WXS_ADD_GLOBAL(pctxt, ret);
	WXS_ADD_PENDING(pctxt, ret);
	return ret;
}

/**
 * xmlSchemaAddElement:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @name:  the type name
 * @namespace:  the type namespace
 *
 * Add an XML schema Element declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaElementPtr xmlSchemaAddElement(xmlSchemaParserCtxtPtr ctxt, const xmlChar * name, const xmlChar * nsName, xmlNode * P_Node, int topLevel)
{
	xmlSchemaElementPtr ret = NULL;
	if(!ctxt || (name == NULL))
		return 0;
	ret = (xmlSchemaElementPtr)SAlloc::M(sizeof(xmlSchemaElement));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating element", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaElement));
	ret->type = XML_SCHEMA_TYPE_ELEMENT;
	ret->name = name;
	ret->targetNamespace = nsName;
	ret->P_Node = P_Node;
	if(topLevel)
		WXS_ADD_GLOBAL(ctxt, ret);
	else
		WXS_ADD_LOCAL(ctxt, ret);
	WXS_ADD_PENDING(ctxt, ret);
	return ret;
}

/**
 * xmlSchemaAddType:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @name:  the item name
 * @namespace:  the namespace
 *
 * Add an XML schema item
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaTypePtr xmlSchemaAddType(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema,
    xmlSchemaTypeType type,
    const xmlChar * name, const xmlChar * nsName,
    xmlNode * P_Node, int topLevel)
{
	xmlSchemaTypePtr ret = NULL;

	if(!ctxt || (schema == NULL))
		return 0;

	ret = (xmlSchemaTypePtr)SAlloc::M(sizeof(xmlSchemaType));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating type", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaType));
	ret->type = type;
	ret->name = name;
	ret->targetNamespace = nsName;
	ret->P_Node = P_Node;
	if(topLevel) {
		if(ctxt->isRedefine) {
			ctxt->redef = xmlSchemaAddRedef(ctxt, ctxt->redefined,
			    ret, name, nsName);
			if(ctxt->redef == NULL) {
				SAlloc::F(ret);
				return 0;
			}
			ctxt->redefCounter = 0;
		}
		WXS_ADD_GLOBAL(ctxt, ret);
	}
	else
		WXS_ADD_LOCAL(ctxt, ret);
	WXS_ADD_PENDING(ctxt, ret);
	return ret;
}

static xmlSchemaQNameRefPtr xmlSchemaNewQNameRef(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypeType refType, const xmlChar * refName, const xmlChar * refNs)
{
	xmlSchemaQNameRefPtr ret = (xmlSchemaQNameRefPtr)SAlloc::M(sizeof(xmlSchemaQNameRef));
	if(!ret) {
		xmlSchemaPErrMemory(pctxt, "allocating QName reference item", 0);
		return 0;
	}
	ret->P_Node = NULL;
	ret->type = XML_SCHEMA_EXTRA_QNAMEREF;
	ret->name = refName;
	ret->targetNamespace = refNs;
	ret->item = NULL;
	ret->itemType = refType;
	/*
	 * Store the reference item in the schema.
	 */
	WXS_ADD_LOCAL(pctxt, ret);
	return ret;
}

static xmlSchemaAttributeUseProhibPtr xmlSchemaAddAttributeUseProhib(xmlSchemaParserCtxtPtr pctxt)
{
	xmlSchemaAttributeUseProhibPtr ret = (xmlSchemaAttributeUseProhibPtr)SAlloc::M(sizeof(xmlSchemaAttributeUseProhib));
	if(!ret) {
		xmlSchemaPErrMemory(pctxt, "allocating attribute use prohibition", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaAttributeUseProhib));
	ret->type = XML_SCHEMA_EXTRA_ATTR_USE_PROHIB;
	WXS_ADD_LOCAL(pctxt, ret);
	return ret;
}

/**
 * xmlSchemaAddModelGroup:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @type: the "compositor" type of the model group
 * @node: the node in the schema doc
 *
 * Adds a schema model group
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaModelGroupPtr xmlSchemaAddModelGroup(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlSchemaTypeType type, xmlNode * P_Node)
{
	xmlSchemaModelGroupPtr ret = NULL;
	if(!ctxt || (schema == NULL))
		return 0;
	ret = (xmlSchemaModelGroupPtr)SAlloc::M(sizeof(xmlSchemaModelGroup));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating model group component", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaModelGroup));
	ret->type = type;
	ret->P_Node = P_Node;
	WXS_ADD_LOCAL(ctxt, ret);
	if(oneof2(type, XML_SCHEMA_TYPE_SEQUENCE, XML_SCHEMA_TYPE_CHOICE))
		WXS_ADD_PENDING(ctxt, ret);
	return ret;
}

/**
 * xmlSchemaAddParticle:
 * @ctxt:  a schema parser context
 * @schema:  the schema being built
 * @node: the corresponding node in the schema doc
 * @min: the minOccurs
 * @max: the maxOccurs
 *
 * Adds an XML schema particle component.
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaParticlePtr xmlSchemaAddParticle(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, int min, int max)
{
	xmlSchemaParticlePtr ret = NULL;
	if(!ctxt)
		return 0;
#ifdef DEBUG
	fprintf(stderr, "Adding particle component\n");
#endif
	ret = (xmlSchemaParticlePtr)SAlloc::M(sizeof(xmlSchemaParticle));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating particle component", 0);
		return 0;
	}
	ret->type = XML_SCHEMA_TYPE_PARTICLE;
	ret->annot = NULL;
	ret->P_Node = P_Node;
	ret->minOccurs = min;
	ret->maxOccurs = max;
	ret->next = NULL;
	ret->children = NULL;

	WXS_ADD_LOCAL(ctxt, ret);
	/*
	 * Note that addition to pending components will be done locally
	 * to the specific parsing function, since the most particles
	 * need not to be fixed up (i.e. the reference to be resolved).
	 * REMOVED: WXS_ADD_PENDING(ctxt, ret);
	 */
	return ret;
}

/**
 * xmlSchemaAddModelGroupDefinition:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @name:  the group name
 *
 * Add an XML schema Group definition
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaModelGroupDefPtr xmlSchemaAddModelGroupDefinition(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, const xmlChar * name, const xmlChar * nsName, xmlNode * P_Node)
{
	xmlSchemaModelGroupDefPtr ret = NULL;
	if(!ctxt || (schema == NULL) || (name == NULL))
		return 0;
	ret = (xmlSchemaModelGroupDefPtr)SAlloc::M(sizeof(xmlSchemaModelGroupDef));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "adding group", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaModelGroupDef));
	ret->name = name;
	ret->type = XML_SCHEMA_TYPE_GROUP;
	ret->P_Node = P_Node;
	ret->targetNamespace = nsName;
	if(ctxt->isRedefine) {
		ctxt->redef = xmlSchemaAddRedef(ctxt, ctxt->redefined, ret, name, nsName);
		if(ctxt->redef == NULL) {
			SAlloc::F(ret);
			return 0;
		}
		ctxt->redefCounter = 0;
	}
	WXS_ADD_GLOBAL(ctxt, ret);
	WXS_ADD_PENDING(ctxt, ret);
	return ret;
}

/**
 * xmlSchemaNewWildcardNs:
 * @ctxt:  a schema validation context
 *
 * Creates a new wildcard namespace constraint.
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaWildcardNsPtr xmlSchemaNewWildcardNsConstraint(xmlSchemaParserCtxtPtr ctxt)
{
	xmlSchemaWildcardNsPtr ret = (xmlSchemaWildcardNsPtr)SAlloc::M(sizeof(xmlSchemaWildcardNs));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "creating wildcard namespace constraint", 0);
		return 0;
	}
	ret->value = NULL;
	ret->next = NULL;
	return ret;
}

static xmlSchemaIDCPtr xmlSchemaAddIDC(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema,
    const xmlChar * name, const xmlChar * nsName, int category, xmlNode * P_Node)
{
	xmlSchemaIDCPtr ret = NULL;
	if(!ctxt || (schema == NULL) || (name == NULL))
		return 0;
	ret = (xmlSchemaIDCPtr)SAlloc::M(sizeof(xmlSchemaIDC));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "allocating an identity-constraint definition", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaIDC));
	/* The target namespace of the parent element declaration. */
	ret->targetNamespace = nsName;
	ret->name = name;
	ret->type = (xmlSchemaTypeType)category;
	ret->P_Node = P_Node;
	WXS_ADD_GLOBAL(ctxt, ret);
	/*
	 * Only keyrefs need to be fixup up.
	 */
	if(category == XML_SCHEMA_TYPE_IDC_KEYREF)
		WXS_ADD_PENDING(ctxt, ret);
	return ret;
}

/**
 * xmlSchemaAddWildcard:
 * @ctxt:  a schema validation context
 * @schema: a schema
 *
 * Adds a wildcard.
 * It corresponds to a xsd:anyAttribute and xsd:any.
 *
 * Returns the new struture or NULL in case of error
 */
static xmlSchemaWildcardPtr xmlSchemaAddWildcard(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlSchemaTypeType type, xmlNode * P_Node)
{
	xmlSchemaWildcardPtr ret = NULL;
	if(!ctxt || (schema == NULL))
		return 0;
	ret = (xmlSchemaWildcardPtr)SAlloc::M(sizeof(xmlSchemaWildcard));
	if(!ret) {
		xmlSchemaPErrMemory(ctxt, "adding wildcard", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaWildcard));
	ret->type = type;
	ret->P_Node = P_Node;
	WXS_ADD_LOCAL(ctxt, ret);
	return ret;
}

static void xmlSchemaSubstGroupFree(xmlSchemaSubstGroup * group)
{
	if(group) {
		xmlSchemaItemListFree(group->members);
		SAlloc::F(group);
	}
}

static xmlSchemaSubstGroup * xmlSchemaSubstGroupAdd(xmlSchemaParserCtxtPtr pctxt, xmlSchemaElementPtr head)
{
	xmlSchemaSubstGroup * ret;
	// Init subst group hash. 
	if(WXS_SUBST_GROUPS(pctxt) == NULL) {
		WXS_SUBST_GROUPS(pctxt) = xmlHashCreateDict(10, pctxt->dict);
		if(WXS_SUBST_GROUPS(pctxt) == NULL)
			return 0;
	}
	/* Create a new substitution group. */
	ret = (xmlSchemaSubstGroup *)SAlloc::M(sizeof(xmlSchemaSubstGroup));
	if(!ret) {
		xmlSchemaPErrMemory(NULL, "allocating a substitution group container", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaSubstGroup));
	ret->head = head;
	/* Create list of members. */
	ret->members = xmlSchemaItemListCreate();
	if(ret->members == NULL) {
		xmlSchemaSubstGroupFree(ret);
		return 0;
	}
	/* Add subst group to hash. */
	if(xmlHashAddEntry2(WXS_SUBST_GROUPS(pctxt), head->name, head->targetNamespace, ret) != 0) {
		PERROR_INT("xmlSchemaSubstGroupAdd", "failed to add a new substitution container");
		xmlSchemaSubstGroupFree(ret);
		return 0;
	}
	return ret;
}

static xmlSchemaSubstGroup * xmlSchemaSubstGroupGet(xmlSchemaParserCtxtPtr pctxt, xmlSchemaElementPtr head)
{
	if(WXS_SUBST_GROUPS(pctxt) == NULL)
		return 0;
	return (xmlSchemaSubstGroup *)xmlHashLookup2(WXS_SUBST_GROUPS(pctxt), head->name, head->targetNamespace);
}

/**
 * xmlSchemaAddElementSubstitutionMember:
 * @pctxt:  a schema parser context
 * @head:  the head of the substitution group
 * @member: the new member of the substitution group
 *
 * Allocate a new annotation structure.
 *
 * Returns the newly allocated structure or NULL in case or error
 */
static int xmlSchemaAddElementSubstitutionMember(xmlSchemaParserCtxtPtr pctxt, xmlSchemaElementPtr head, xmlSchemaElementPtr member)
{
	xmlSchemaSubstGroup * substGroup = NULL;
	if((pctxt == NULL) || (head == NULL) || (member == NULL))
		return -1;
	substGroup = xmlSchemaSubstGroupGet(pctxt, head);
	if(substGroup == NULL)
		substGroup = xmlSchemaSubstGroupAdd(pctxt, head);
	if(substGroup == NULL)
		return -1;
	if(xmlSchemaItemListAdd(substGroup->members, member) == -1)
		return -1;
	return 0;
}

/************************************************************************
*									*
*		Utilities for parsing					*
*									*
************************************************************************/

/**
 * xmlSchemaPValAttrNodeQNameValue:
 * @ctxt:  a schema parser context
 * @schema: the schema context
 * @ownerDes: the designation of the parent element
 * @ownerItem: the parent as a schema object
 * @value:  the QName value
 * @local: the resulting local part if found, the attribute value otherwise
 * @uri:  the resulting namespace URI if found
 *
 * Extracts the local name and the URI of a QName value and validates it.
 * This one is intended to be used on attribute values that
 * should resolve to schema components.
 *
 * Returns 0, in case the QName is valid, a positive error code
 * if not valid and -1 if an internal error occurs.
 */
static int xmlSchemaPValAttrNodeQNameValue(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlSchemaBasicItem * ownerItem,
    xmlAttrPtr attr, const xmlChar * value, const xmlChar ** uri, const xmlChar ** local) 
{
	const xmlChar * pref;
	xmlNs * ns;
	int len, ret;
	*uri = NULL;
	*local = NULL;
	ret = xmlValidateQName(value, 1);
	if(ret > 0) {
		xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, ownerItem, (xmlNode *)attr,
		    xmlSchemaGetBuiltInType(XML_SCHEMAS_QNAME), NULL, value, NULL, 0, 0);
		*local = value;
		return (ctxt->err);
	}
	else if(ret < 0)
		return -1;
	if(!strchr((char*)value, ':')) {
		ns = xmlSearchNs(attr->doc, attr->parent, 0);
		if(ns)
			*uri = xmlDictLookupSL(ctxt->dict, ns->href);
		else if(schema->flags & XML_SCHEMAS_INCLUDING_CONVERT_NS) {
			/* @todo move XML_SCHEMAS_INCLUDING_CONVERT_NS to the parser context. */
			/*
			 * This one takes care of included schemas with no target namespace.
			 */
			*uri = ctxt->targetNamespace;
		}
		*local = xmlDictLookupSL(ctxt->dict, value);
		return 0;
	}
	/*
	 * At this point xmlSplitQName3 has to return a local name.
	 */
	*local = xmlSplitQName3(value, &len);
	*local = xmlDictLookupSL(ctxt->dict, *local);
	pref = xmlDictLookup(ctxt->dict, value, len);
	ns = xmlSearchNs(attr->doc, attr->parent, pref);
	if(ns == NULL) {
		xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, ownerItem, (xmlNode *)attr,
		    xmlSchemaGetBuiltInType(XML_SCHEMAS_QNAME), NULL, value,
		    "The value '%s' of simple type 'xs:QName' has no corresponding namespace declaration in scope", value, 0);
		return (ctxt->err);
	}
	else {
		*uri = xmlDictLookupSL(ctxt->dict, ns->href);
	}
	return 0;
}

/**
 * xmlSchemaPValAttrNodeQName:
 * @ctxt:  a schema parser context
 * @schema: the schema context
 * @ownerDes: the designation of the owner element
 * @ownerItem: the owner as a schema object
 * @attr:  the attribute node
 * @local: the resulting local part if found, the attribute value otherwise
 * @uri:  the resulting namespace URI if found
 *
 * Extracts and validates the QName of an attribute value.
 * This one is intended to be used on attribute values that
 * should resolve to schema components.
 *
 * Returns 0, in case the QName is valid, a positive error code
 * if not valid and -1 if an internal error occurs.
 */
static int xmlSchemaPValAttrNodeQName(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlSchemaBasicItem * ownerItem,
    xmlAttrPtr attr, const xmlChar ** uri, const xmlChar ** local)
{
	const xmlChar * value = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
	return (xmlSchemaPValAttrNodeQNameValue(ctxt, schema, ownerItem, attr, value, uri, local));
}
/**
 * xmlSchemaPValAttrQName:
 * @ctxt:  a schema parser context
 * @schema: the schema context
 * @ownerDes: the designation of the parent element
 * @ownerItem: the owner as a schema object
 * @ownerElem:  the parent node of the attribute
 * @name:  the name of the attribute
 * @local: the resulting local part if found, the attribute value otherwise
 * @uri:  the resulting namespace URI if found
 *
 * Extracts and validates the QName of an attribute value.
 *
 * Returns 0, in case the QName is valid, a positive error code
 * if not valid and -1 if an internal error occurs.
 */
static int xmlSchemaPValAttrQName(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlSchemaBasicItem * ownerItem,
    xmlNode * ownerElem, const char * name, const xmlChar ** uri, const xmlChar ** local)
{
	xmlAttr * attr = xmlSchemaGetPropNode(ownerElem, name);
	if(!attr) {
		*local = NULL;
		*uri = NULL;
		return 0;
	}
	return (xmlSchemaPValAttrNodeQName(ctxt, schema, ownerItem, attr, uri, local));
}
/**
 * xmlSchemaPValAttrID:
 * @ctxt:  a schema parser context
 * @schema: the schema context
 * @ownerDes: the designation of the parent element
 * @ownerItem: the owner as a schema object
 * @ownerElem:  the parent node of the attribute
 * @name:  the name of the attribute
 *
 * Extracts and validates the ID of an attribute value.
 *
 * Returns 0, in case the ID is valid, a positive error code
 * if not valid and -1 if an internal error occurs.
 */
static int xmlSchemaPValAttrNodeID(xmlSchemaParserCtxtPtr ctxt, xmlAttrPtr attr)
{
	int ret;
	const xmlChar * value;

	if(!attr)
		return 0;
	value = xmlSchemaGetNodeContentNoDict((xmlNode *)attr);
	ret = xmlValidateNCName(value, 1);
	if(ret == 0) {
		/*
		 * NOTE: the IDness might have already be declared in the DTD
		 */
		if(attr->atype != XML_ATTRIBUTE_ID) {
			xmlIDPtr res;
			/*
			 * @todo Use xmlSchemaStrip here; it's not exported at this
			 * moment.
			 */
			xmlChar * strip = xmlSchemaCollapseString(value);
			if(strip) {
				SAlloc::F((xmlChar*)value);
				value = strip;
			}
			res = xmlAddID(NULL, attr->doc, value, attr);
			if(res == NULL) {
				ret = XML_SCHEMAP_S4S_ATTR_INVALID_VALUE;
				xmlSchemaPSimpleTypeErr(ctxt,
				    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
				    NULL, (xmlNode *)attr,
				    xmlSchemaGetBuiltInType(XML_SCHEMAS_ID),
				    NULL, NULL, "Duplicate value '%s' of simple "
				    "type 'xs:ID'", value, 0);
			}
			else
				attr->atype = XML_ATTRIBUTE_ID;
		}
	}
	else if(ret > 0) {
		ret = XML_SCHEMAP_S4S_ATTR_INVALID_VALUE;
		xmlSchemaPSimpleTypeErr(ctxt,
		    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
		    NULL, (xmlNode *)attr,
		    xmlSchemaGetBuiltInType(XML_SCHEMAS_ID),
		    NULL, NULL, "The value '%s' of simple type 'xs:ID' is "
		    "not a valid 'xs:NCName'",
		    value, 0);
	}
	SAlloc::F((xmlChar*)value);
	return ret;
}

static int xmlSchemaPValAttrID(xmlSchemaParserCtxtPtr ctxt, xmlNode * ownerElem, const xmlChar * name)
{
	xmlAttr * attr;
	attr = xmlSchemaGetPropNode(ownerElem, (const char*)name);
	if(!attr)
		return 0;
	return(xmlSchemaPValAttrNodeID(ctxt, attr));
}

/**
 * xmlGetMaxOccurs:
 * @ctxt:  a schema validation context
 * @node:  a subtree containing XML Schema informations
 *
 * Get the maxOccurs property
 *
 * Returns the default if not found, or the value
 */
static int xmlGetMaxOccurs(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, int min, int max, int def, const char * expected)
{
	const xmlChar * val, * cur;
	int ret = 0;
	xmlAttrPtr attr = xmlSchemaGetPropNode(P_Node, "maxOccurs");
	if(!attr)
		return (def);
	val = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
	if(sstreq(val, "unbounded")) {
		if(max != UNBOUNDED) {
			xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, /* XML_SCHEMAP_INVALID_MINOCCURS, */
			    NULL, (xmlNode *)attr, NULL, expected, val, NULL, 0, 0);
			return (def);
		}
		else
			return (UNBOUNDED);  /* encoding it with -1 might be another option */
	}

	cur = val;
	while(IS_BLANK_CH(*cur))
		cur++;
	if(*cur == 0) {
		xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, /* XML_SCHEMAP_INVALID_MINOCCURS, */
		    NULL, (xmlNode *)attr, NULL, expected, val, NULL, 0, 0);
		return (def);
	}
	while((*cur >= '0') && (*cur <= '9')) {
		ret = ret * 10 + (*cur - '0');
		cur++;
	}
	while(IS_BLANK_CH(*cur))
		cur++;
	/*
	 * @todo Restrict the maximal value to Integer.
	 */
	if((*cur != 0) || (ret < min) || ((max != -1) && (ret > max))) {
		xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, /* XML_SCHEMAP_INVALID_MINOCCURS, */
		    NULL, (xmlNode *)attr, NULL, expected, val, NULL, 0, 0);
		return (def);
	}
	return ret;
}

/**
 * xmlGetMinOccurs:
 * @ctxt:  a schema validation context
 * @node:  a subtree containing XML Schema informations
 *
 * Get the minOccurs property
 *
 * Returns the default if not found, or the value
 */
static int xmlGetMinOccurs(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, int min, int max, int def, const char * expected)
{
	const xmlChar * val, * cur;
	int ret = 0;
	xmlAttr * attr;

	attr = xmlSchemaGetPropNode(P_Node, "minOccurs");
	if(!attr)
		return (def);
	val = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
	cur = val;
	while(IS_BLANK_CH(*cur))
		cur++;
	if(*cur == 0) {
		xmlSchemaPSimpleTypeErr(ctxt,
		    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
		    /* XML_SCHEMAP_INVALID_MINOCCURS, */
		    NULL, (xmlNode *)attr, NULL, expected,
		    val, NULL, 0, 0);
		return (def);
	}
	while((*cur >= '0') && (*cur <= '9')) {
		ret = ret * 10 + (*cur - '0');
		cur++;
	}
	while(IS_BLANK_CH(*cur))
		cur++;
	/*
	 * @todo Restrict the maximal value to Integer.
	 */
	if((*cur != 0) || (ret < min) || ((max != -1) && (ret > max))) {
		xmlSchemaPSimpleTypeErr(ctxt,
		    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
		    /* XML_SCHEMAP_INVALID_MINOCCURS, */
		    NULL, (xmlNode *)attr, NULL, expected,
		    val, NULL, 0, 0);
		return (def);
	}
	return ret;
}

/**
 * xmlSchemaPGetBoolNodeValue:
 * @ctxt:  a schema validation context
 * @ownerDes:  owner designation
 * @ownerItem:  the owner as a schema item
 * @node: the node holding the value
 *
 * Converts a boolean string value into 1 or 0.
 *
 * Returns 0 or 1.
 */
static int xmlSchemaPGetBoolNodeValue(xmlSchemaParserCtxtPtr ctxt, xmlSchemaBasicItem * ownerItem, xmlNode * P_Node)
{
	int res = 0;
	xmlChar * value = xmlNodeGetContent(P_Node);
	/*
	 * 3.2.2.1 Lexical representation
	 * An instance of a datatype that is defined as `boolean`
	 * can have the following legal literals {true, false, 1, 0}.
	 */
	if(sstreq(value, "true"))
		res = 1;
	else if(sstreq(value, "false"))
		res = 0;
	else if(sstreq(value, "1"))
		res = 1;
	else if(sstreq(value, "0"))
		res = 0;
	else {
		xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_INVALID_BOOLEAN, ownerItem, P_Node, xmlSchemaGetBuiltInType(XML_SCHEMAS_BOOLEAN), NULL, BAD_CAST value, NULL, 0, 0);
	}
	SAlloc::F(value);
	return (res);
}

/**
 * xmlGetBooleanProp:
 * @ctxt:  a schema validation context
 * @node:  a subtree containing XML Schema informations
 * @name:  the attribute name
 * @def:  the default value
 *
 * Evaluate if a boolean property is set
 *
 * Returns the default if not found, 0 if found to be false,
 * 1 if found to be true
 */
static int xmlGetBooleanProp(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, const char * name, int def)
{
	const xmlChar * val;
	val = xmlSchemaGetProp(ctxt, P_Node, name);
	if(!val)
		return (def);
	/*
	 * 3.2.2.1 Lexical representation
	 * An instance of a datatype that is defined as `boolean`
	 * can have the following legal literals {true, false, 1, 0}.
	 */
	if(sstreq(val, "true"))
		def = 1;
	else if(sstreq(val, "false"))
		def = 0;
	else if(sstreq(val, "1"))
		def = 1;
	else if(sstreq(val, "0"))
		def = 0;
	else {
		xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_INVALID_BOOLEAN, NULL,
		    (xmlNode *)xmlSchemaGetPropNode(P_Node, name), xmlSchemaGetBuiltInType(XML_SCHEMAS_BOOLEAN), NULL, val, NULL, 0, 0);
	}
	return (def);
}

/************************************************************************
*									*
*		Shema extraction from an Infoset			*
*									*
************************************************************************/
static xmlSchemaTypePtr xmlSchemaParseSimpleType(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, int topLevel);
static xmlSchemaTypePtr xmlSchemaParseComplexType(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, int topLevel);
static xmlSchemaTypePtr xmlSchemaParseRestriction(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, xmlSchemaTypeType parentType);
static xmlSchemaBasicItem * xmlSchemaParseLocalAttribute(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node, xmlSchemaItemListPtr uses, int parentType);
static xmlSchemaTypePtr xmlSchemaParseList(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node);
static xmlSchemaWildcardPtr xmlSchemaParseAnyAttribute(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node);
/**
 * xmlSchemaPValAttrNodeValue:
 *
 * @ctxt:  a schema parser context
 * @ownerDes: the designation of the parent element
 * @ownerItem: the schema object owner if existent
 * @attr:  the schema attribute node being validated
 * @value: the value
 * @type: the built-in type to be validated against
 *
 * Validates a value against the given built-in type.
 * This one is intended to be used internally for validation
 * of schema attribute values during parsing of the schema.
 *
 * Returns 0 if the value is valid, a positive error code
 * number otherwise and -1 in case of an internal or API error.
 */
static int xmlSchemaPValAttrNodeValue(xmlSchemaParserCtxtPtr pctxt, xmlSchemaBasicItem * ownerItem, xmlAttrPtr attr, const xmlChar * value, xmlSchemaTypePtr type)
{
	int ret = 0;
	/*
	 * NOTE: Should we move this to xmlschematypes.c? Hmm, but this
	 * one is really meant to be used internally, so better not.
	 */
	if((pctxt == NULL) || (type == NULL) || (attr == NULL))
		return -1;
	if(type->type != XML_SCHEMA_TYPE_BASIC) {
		PERROR_INT("xmlSchemaPValAttrNodeValue", "the given type is not a built-in type");
		return -1;
	}
	switch(type->builtInType) {
		case XML_SCHEMAS_NCNAME:
		case XML_SCHEMAS_QNAME:
		case XML_SCHEMAS_ANYURI:
		case XML_SCHEMAS_TOKEN:
		case XML_SCHEMAS_LANGUAGE:
		    ret = xmlSchemaValPredefTypeNode(type, value, NULL, (xmlNode *)attr);
		    break;
		default: {
		    PERROR_INT("xmlSchemaPValAttrNodeValue", "validation using the given type is not supported while parsing a schema");
		    return -1;
	    }
	}
	/*
	 * @todo Should we use the S4S error codes instead?
	 */
	if(ret < 0) {
		PERROR_INT("xmlSchemaPValAttrNodeValue", "failed to validate a schema attribute value");
		return -1;
	}
	else if(ret > 0) {
		if(WXS_IS_LIST(type))
			ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_2;
		else
			ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_1;
		xmlSchemaPSimpleTypeErr(pctxt, (xmlParserErrors)ret, ownerItem, (xmlNode *)attr, type, NULL, value, NULL, 0, 0);
	}
	return ret;
}

/**
 * xmlSchemaPValAttrNode:
 *
 * @ctxt:  a schema parser context
 * @ownerDes: the designation of the parent element
 * @ownerItem: the schema object owner if existent
 * @attr:  the schema attribute node being validated
 * @type: the built-in type to be validated against
 * @value: the resulting value if any
 *
 * Extracts and validates a value against the given built-in type.
 * This one is intended to be used internally for validation
 * of schema attribute values during parsing of the schema.
 *
 * Returns 0 if the value is valid, a positive error code
 * number otherwise and -1 in case of an internal or API error.
 */
static int xmlSchemaPValAttrNode(xmlSchemaParserCtxtPtr ctxt, xmlSchemaBasicItem * ownerItem, xmlAttrPtr attr, xmlSchemaTypePtr type, const xmlChar ** value)
{
	const xmlChar * val;
	if(!ctxt || (type == NULL) || (attr == NULL))
		return -1;
	val = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
	if(value)
		*value = val;
	return (xmlSchemaPValAttrNodeValue(ctxt, ownerItem, attr, val, type));
}

/**
 * xmlSchemaPValAttr:
 *
 * @ctxt:  a schema parser context
 * @node: the element node of the attribute
 * @ownerDes: the designation of the parent element
 * @ownerItem: the schema object owner if existent
 * @ownerElem: the owner element node
 * @name:  the name of the schema attribute node
 * @type: the built-in type to be validated against
 * @value: the resulting value if any
 *
 * Extracts and validates a value against the given built-in type.
 * This one is intended to be used internally for validation
 * of schema attribute values during parsing of the schema.
 *
 * Returns 0 if the value is valid, a positive error code
 * number otherwise and -1 in case of an internal or API error.
 */
static int xmlSchemaPValAttr(xmlSchemaParserCtxtPtr ctxt, xmlSchemaBasicItem * ownerItem, xmlNode * ownerElem,
    const char * name, xmlSchemaTypePtr type, const xmlChar ** value)
{
	xmlAttr * attr;
	if(!ctxt || (type == NULL)) {
		ASSIGN_PTR(value, 0);
		return -1;
	}
	if(type->type != XML_SCHEMA_TYPE_BASIC) {
		ASSIGN_PTR(value, 0);
		xmlSchemaPErr(ctxt, ownerElem, XML_SCHEMAP_INTERNAL, "Internal error: xmlSchemaPValAttr, the given type '%s' is not a built-in type.\n", type->name, 0);
		return -1;
	}
	attr = xmlSchemaGetPropNode(ownerElem, name);
	if(!attr) {
		ASSIGN_PTR(value, 0);
		return 0;
	}
	return (xmlSchemaPValAttrNode(ctxt, ownerItem, attr, type, value));
}

static int xmlSchemaCheckReference(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema ATTRIBUTE_UNUSED,
    xmlNode * P_Node, xmlAttrPtr attr, const xmlChar * namespaceName)
{
	/* @todo Pointer comparison instead? */
	if(sstreq(pctxt->targetNamespace, namespaceName))
		return 0;
	if(sstreq(xmlSchemaNs, namespaceName))
		return 0;
	/*
	 * Check if the referenced namespace was <import>ed.
	 */
	if(WXS_BUCKET(pctxt)->relations) {
		xmlSchemaSchemaRelationPtr rel = WXS_BUCKET(pctxt)->relations;
		do {
			if(WXS_IS_BUCKET_IMPMAIN(rel->type) && sstreq(namespaceName, rel->importNamespace))
				return 0;
			rel = rel->next;
		} while(rel);
	}
	/*
	 * No matching <import>ed namespace found.
	 */
	{
		xmlNode * n = attr ? (xmlNode *)attr : P_Node;
		if(namespaceName == NULL)
			xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_SRC_RESOLVE, n, NULL,
			    "References from this schema to components in no namespace are not allowed, since not indicated by an import statement", 0, 0);
		else
			xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_SRC_RESOLVE, n, NULL,
			    "References from this schema to components in the namespace '%s' are not allowed, since not indicated by an import statement", namespaceName, 0);
	}
	return (XML_SCHEMAP_SRC_RESOLVE);
}

/**
 * xmlSchemaParseLocalAttributes:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 * @type:  the hosting type where the attributes will be anchored
 *
 * Parses attribute uses and attribute declarations and
 * attribute group references.
 */
static int xmlSchemaParseLocalAttributes(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * * child, xmlSchemaItemListPtr * list, int parentType, int * hasRefs)
{
	void * item;
	while((IS_SCHEMA((*child), "attribute")) || (IS_SCHEMA((*child), "attributeGroup"))) {
		if(IS_SCHEMA((*child), "attribute")) {
			item = xmlSchemaParseLocalAttribute(ctxt, schema, *child, *list, parentType);
		}
		else {
			item = xmlSchemaParseAttributeGroupRef(ctxt, schema, *child);
			if(item && hasRefs)
				*hasRefs = 1;
		}
		if(item) {
			if(*list == NULL) {
				// @todo Customize grow factor
				*list = xmlSchemaItemListCreate();
				if(*list == NULL)
					return -1;
			}
			if(xmlSchemaItemListAddSize(*list, 2, item) == -1)
				return -1;
		}
		*child = (*child)->next;
	}
	return 0;
}
/**
 * xmlSchemaParseAnnotation:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Attrribute declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns -1 in case of error, 0 if the declaration is improper and
 *         1 in case of success.
 */
static xmlSchemaAnnotPtr xmlSchemaParseAnnotation(xmlSchemaParserCtxtPtr ctxt, xmlNode * P_Node, int needed)
{
	xmlSchemaAnnotPtr ret;
	xmlNode * child = NULL;
	xmlAttr * attr;
	int barked = 0;
	/*
	 * INFO: S4S completed.
	 */
	/*
	 * id = ID
	 * {any attributes with non-schema namespace . . .}>
	 * Content: (appinfo | documentation)*
	 */
	if(!ctxt || !P_Node)
		return 0;
	if(needed)
		ret = xmlSchemaNewAnnot(ctxt, P_Node);
	else
		ret = NULL;
	attr = P_Node->properties;
	while(attr) {
		if(((attr->ns == NULL) && (!sstreq(attr->name, "id"))) || (attr->ns && sstreq(attr->ns->href, xmlSchemaNs))) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	while(child) {
		if(IS_SCHEMA(child, "appinfo")) {
			/* @todo make available the content of "appinfo". */
			/*
			 * source = anyURI
			 * {any attributes with non-schema namespace . . .}>
			 * Content: ({any})*
			 */
			attr = child->properties;
			while(attr) {
				if((!attr->ns && !sstreq(attr->name, "source")) || (attr->ns && sstreq(attr->ns->href, xmlSchemaNs))) {
					xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
				}
				attr = attr->next;
			}
			xmlSchemaPValAttr(ctxt, NULL, child, "source", xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI), 0);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "documentation")) {
			/* @todo make available the content of "documentation". */
			/*
			 * source = anyURI
			 * {any attributes with non-schema namespace . . .}>
			 * Content: ({any})*
			 */
			attr = child->properties;
			while(attr) {
				if(attr->ns == NULL) {
					if(!sstreq(attr->name, "source")) {
						xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
					}
				}
				else {
					if(sstreq(attr->ns->href, xmlSchemaNs) || (sstreq(attr->name, "lang") && (!sstreq(attr->ns->href, XML_XML_NAMESPACE)))) {
						xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
					}
				}
				attr = attr->next;
			}
			/*
			 * Attribute "xml:lang".
			 */
			attr = xmlSchemaGetPropNodeNs(child, (const char*)XML_XML_NAMESPACE, "lang");
			if(attr)
				xmlSchemaPValAttrNode(ctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_LANGUAGE), 0);
			child = child->next;
		}
		else {
			if(!barked)
				xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(appinfo | documentation)*");
			barked = 1;
			child = child->next;
		}
	}
	return ret;
}
/**
 * xmlSchemaParseFacet:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Facet declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns the new type structure or NULL in case of error
 */
static xmlSchemaFacetPtr xmlSchemaParseFacet(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaFacet * facet = 0;
	xmlNode * child = NULL;
	if(ctxt && schema && P_Node) {
		facet = xmlSchemaNewFacet();
		if(!facet) {
			xmlSchemaPErrMemory(ctxt, "allocating facet", P_Node);
		}
		else {
			facet->P_Node = P_Node;
			const xmlChar * value = xmlSchemaGetProp(ctxt, P_Node, "value");
			if(!value) {
				xmlSchemaPErr2(ctxt, P_Node, child, XML_SCHEMAP_FACET_NO_VALUE, "Facet %s has no value\n", P_Node->name, 0);
				xmlSchemaFreeFacet(facet);
				return 0;
			}
			if(IS_SCHEMA(P_Node, "minInclusive")) {
				facet->type = XML_SCHEMA_FACET_MININCLUSIVE;
			}
			else if(IS_SCHEMA(P_Node, "minExclusive")) {
				facet->type = XML_SCHEMA_FACET_MINEXCLUSIVE;
			}
			else if(IS_SCHEMA(P_Node, "maxInclusive")) {
				facet->type = XML_SCHEMA_FACET_MAXINCLUSIVE;
			}
			else if(IS_SCHEMA(P_Node, "maxExclusive")) {
				facet->type = XML_SCHEMA_FACET_MAXEXCLUSIVE;
			}
			else if(IS_SCHEMA(P_Node, "totalDigits")) {
				facet->type = XML_SCHEMA_FACET_TOTALDIGITS;
			}
			else if(IS_SCHEMA(P_Node, "fractionDigits")) {
				facet->type = XML_SCHEMA_FACET_FRACTIONDIGITS;
			}
			else if(IS_SCHEMA(P_Node, "pattern")) {
				facet->type = XML_SCHEMA_FACET_PATTERN;
			}
			else if(IS_SCHEMA(P_Node, "enumeration")) {
				facet->type = XML_SCHEMA_FACET_ENUMERATION;
			}
			else if(IS_SCHEMA(P_Node, "whiteSpace")) {
				facet->type = XML_SCHEMA_FACET_WHITESPACE;
			}
			else if(IS_SCHEMA(P_Node, "length")) {
				facet->type = XML_SCHEMA_FACET_LENGTH;
			}
			else if(IS_SCHEMA(P_Node, "maxLength")) {
				facet->type = XML_SCHEMA_FACET_MAXLENGTH;
			}
			else if(IS_SCHEMA(P_Node, "minLength")) {
				facet->type = XML_SCHEMA_FACET_MINLENGTH;
			}
			else {
				xmlSchemaPErr2(ctxt, P_Node, child, XML_SCHEMAP_UNKNOWN_FACET_TYPE, "Unknown facet type %s\n", P_Node->name, 0);
				xmlSchemaFreeFacet(facet);
				return 0;
			}
			xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
			facet->value = value;
			if((facet->type != XML_SCHEMA_FACET_PATTERN) && (facet->type != XML_SCHEMA_FACET_ENUMERATION)) {
				const xmlChar * fixed = xmlSchemaGetProp(ctxt, P_Node, "fixed");
				if(fixed) {
					if(sstreq(fixed, "true"))
						facet->fixed = 1;
				}
			}
			child = P_Node->children;
			if(IS_SCHEMA(child, "annotation")) {
				facet->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
				child = child->next;
			}
			if(child)
				xmlSchemaPErr2(ctxt, P_Node, child, XML_SCHEMAP_UNKNOWN_FACET_CHILD, "Facet %s has unexpected child content\n", P_Node->name, 0);
		}
	}
	return facet;
}
/**
 * xmlSchemaParseWildcardNs:
 * @ctxt:  a schema parser context
 * @wildc:  the wildcard, already created
 * @node:  a subtree containing XML Schema informations
 *
 * Parses the attribute "processContents" and "namespace"
 * of a xsd:anyAttribute and xsd:any.
 * *WARNING* this interface is highly subject to change
 *
 * Returns 0 if everything goes fine, a positive error code
 * if something is not valid and -1 if an internal error occurs.
 */
static int xmlSchemaParseWildcardNs(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema ATTRIBUTE_UNUSED, xmlSchemaWildcardPtr wildc, xmlNode * P_Node)
{
	const xmlChar * ns, * dictnsItem;
	int ret = 0;
	xmlChar * nsItem;
	xmlSchemaWildcardNsPtr tmp, lastNs = NULL;
	xmlAttr * attr;
	const xmlChar * pc = xmlSchemaGetProp(ctxt, P_Node, "processContents");
	if((pc == NULL) || (sstreq(pc, "strict"))) {
		wildc->processContents = XML_SCHEMAS_ANY_STRICT;
	}
	else if(sstreq(pc, "skip")) {
		wildc->processContents = XML_SCHEMAS_ANY_SKIP;
	}
	else if(sstreq(pc, "lax")) {
		wildc->processContents = XML_SCHEMAS_ANY_LAX;
	}
	else {
		xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
		    NULL, P_Node, NULL, "(strict | skip | lax)", pc, NULL, 0, 0);
		wildc->processContents = XML_SCHEMAS_ANY_STRICT;
		ret = XML_SCHEMAP_S4S_ATTR_INVALID_VALUE;
	}
	/*
	 * Build the namespace constraints.
	 */
	attr = xmlSchemaGetPropNode(P_Node, "namespace");
	ns = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
	if(!attr || (sstreq(ns, "##any")))
		wildc->any = 1;
	else if(sstreq(ns, "##other")) {
		wildc->negNsSet = xmlSchemaNewWildcardNsConstraint(ctxt);
		if(wildc->negNsSet == NULL) {
			return -1;
		}
		wildc->negNsSet->value = ctxt->targetNamespace;
	}
	else {
		const xmlChar * end;
		const xmlChar * cur = ns;
		do {
			while(IS_BLANK_CH(*cur))
				cur++;
			end = cur;
			while((*end != 0) && (!(IS_BLANK_CH(*end))))
				end++;
			if(end == cur)
				break;
			nsItem = xmlStrndup(cur, end - cur);
			if((sstreq(nsItem, "##other")) || (sstreq(nsItem, "##any"))) {
				xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_WILDCARD_INVALID_NS_MEMBER,
				    NULL, (xmlNode *)attr, NULL,
				    "((##any | ##other) | List of (xs:anyURI | (##targetNamespace | ##local)))", nsItem, NULL, 0, 0);
				ret = XML_SCHEMAP_WILDCARD_INVALID_NS_MEMBER;
			}
			else {
				if(sstreq(nsItem, "##targetNamespace")) {
					dictnsItem = ctxt->targetNamespace;
				}
				else if(sstreq(nsItem, "##local")) {
					dictnsItem = NULL;
				}
				else {
					// Validate the item (anyURI).
					xmlSchemaPValAttrNodeValue(ctxt, NULL, attr, nsItem, xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI));
					dictnsItem = xmlDictLookupSL(ctxt->dict, nsItem);
				}
				//
				// Avoid dublicate namespaces.
				//
				tmp = wildc->nsSet;
				while(tmp) {
					if(dictnsItem == tmp->value)
						break;
					tmp = tmp->next;
				}
				if(!tmp) {
					tmp = xmlSchemaNewWildcardNsConstraint(ctxt);
					if(!tmp) {
						SAlloc::F(nsItem);
						return -1;
					}
					tmp->value = dictnsItem;
					tmp->next = NULL;
					if(wildc->nsSet == NULL)
						wildc->nsSet = tmp;
					else if(lastNs)
						lastNs->next = tmp;
					lastNs = tmp;
				}
			}
			SAlloc::F(nsItem);
			cur = end;
		} while(*cur != 0);
	}
	return ret;
}

static int xmlSchemaPCheckParticleCorrect_2(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr item ATTRIBUTE_UNUSED,
    xmlNode * P_Node, int minOccurs, int maxOccurs) 
{
	if((maxOccurs == 0) && ( minOccurs == 0))
		return 0;
	if(maxOccurs != UNBOUNDED) {
		/*
		 * @todo Maybe we should better not create the particle,
		 * if min/max is invalid, since it could confuse the build of the
		 * content model.
		 */
		/*
		 * 3.9.6 Schema Component Constraint: Particle Correct
		 *
		 */
		if(maxOccurs < 1) {
			/*
			 * 2.2 {max occurs} must be greater than or equal to 1.
			 */
			xmlSchemaPCustomAttrErr(ctxt, XML_SCHEMAP_P_PROPS_CORRECT_2_2, NULL, NULL,
			    xmlSchemaGetPropNode(P_Node, "maxOccurs"), "The value must be greater than or equal to 1");
			return (XML_SCHEMAP_P_PROPS_CORRECT_2_2);
		}
		else if(minOccurs > maxOccurs) {
			/*
			 * 2.1 {min occurs} must not be greater than {max occurs}.
			 */
			xmlSchemaPCustomAttrErr(ctxt, XML_SCHEMAP_P_PROPS_CORRECT_2_1, NULL, NULL,
			    xmlSchemaGetPropNode(P_Node, "minOccurs"), "The value must not be greater than the value of 'maxOccurs'");
			return (XML_SCHEMAP_P_PROPS_CORRECT_2_1);
		}
	}
	return 0;
}

/**
 * xmlSchemaParseAny:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * Parsea a XML schema <any> element. A particle and wildcard
 * will be created (except if minOccurs==maxOccurs==0, in this case
 * nothing will be created).
 * *WARNING* this interface is highly subject to change
 *
 * Returns the particle or NULL in case of error or if minOccurs==maxOccurs==0
 */
static xmlSchemaParticlePtr xmlSchemaParseAny(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaParticlePtr particle;
	xmlNode * child = NULL;
	xmlSchemaWildcardPtr wild;
	int min, max;
	xmlAttr * attr;
	xmlSchemaAnnotPtr annot = NULL;
	if(!ctxt || !schema || !P_Node)
		return 0;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if(!sstreq(attr->name, "id") && !sstreq(attr->name, "minOccurs") && !sstreq(attr->name, "maxOccurs") && !sstreq(attr->name, "namespace") && !sstreq(attr->name, "processContents")) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * minOccurs/maxOccurs.
	 */
	max = xmlGetMaxOccurs(ctxt, P_Node, 0, UNBOUNDED, 1, "(xs:nonNegativeInteger | unbounded)");
	min = xmlGetMinOccurs(ctxt, P_Node, 0, -1, 1, "xs:nonNegativeInteger");
	xmlSchemaPCheckParticleCorrect_2(ctxt, NULL, P_Node, min, max);
	/*
	 * Create & parse the wildcard.
	 */
	wild = xmlSchemaAddWildcard(ctxt, schema, XML_SCHEMA_TYPE_ANY, P_Node);
	if(wild == NULL)
		return 0;
	xmlSchemaParseWildcardNs(ctxt, schema, wild, P_Node);
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
	/*
	 * No component if minOccurs==maxOccurs==0.
	 */
	if((min == 0) && (max == 0)) {
		// Don't free the wildcard, since it's already on the list
		return 0;
	}
	/*
	 * Create the particle.
	 */
	particle = xmlSchemaAddParticle(ctxt, P_Node, min, max);
	if(particle == NULL)
		return 0;
	particle->annot = annot;
	particle->children = (xmlSchemaTreeItem *)wild;
	return (particle);
}
/**
 * xmlSchemaParseNotation:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Notation declaration
 *
 * Returns the new structure or NULL in case of error
 */
static xmlSchemaNotationPtr xmlSchemaParseNotation(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	const xmlChar * name;
	xmlSchemaNotationPtr ret;
	xmlNode * child = NULL;
	if(!ctxt || !schema || !P_Node)
		return 0;
	name = xmlSchemaGetProp(ctxt, P_Node, "name");
	if(!name) {
		xmlSchemaPErr2(ctxt, P_Node, child, XML_SCHEMAP_NOTATION_NO_NAME, "Notation has no name\n", 0, 0);
		return 0;
	}
	ret = xmlSchemaAddNotation(ctxt, schema, name, ctxt->targetNamespace, P_Node);
	if(ret) {
		xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
		child = P_Node->children;
		if(IS_SCHEMA(child, "annotation")) {
			ret->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
			child = child->next;
		}
		if(child)
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
	}
	return ret;
}

/**
 * xmlSchemaParseAnyAttribute:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema AnyAttrribute declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns a wildcard or NULL.
 */
static xmlSchemaWildcardPtr xmlSchemaParseAnyAttribute(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaWildcardPtr ret;
	xmlNode * child = NULL;
	xmlAttr * attr;
	if(!ctxt || !schema || !P_Node)
		return 0;
	ret = xmlSchemaAddWildcard(ctxt, schema, XML_SCHEMA_TYPE_ANY_ATTRIBUTE, P_Node);
	if(!ret) {
		return 0;
	}
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "namespace")) && (!sstreq(attr->name, "processContents"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * Parse the namespace list.
	 */
	if(xmlSchemaParseWildcardNs(ctxt, schema, ret, P_Node) != 0)
		return 0;
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		ret->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
	return ret;
}
/**
 * xmlSchemaParseAttribute:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Attrribute declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns the attribute declaration.
 */
static xmlSchemaBasicItem * xmlSchemaParseLocalAttribute(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema,
    xmlNode * P_Node, xmlSchemaItemListPtr uses, int parentType)
{
	const xmlChar * attrValue, * name = NULL, * ns = NULL;
	xmlSchemaAttributeUsePtr use = NULL;
	xmlNode * child = NULL;
	xmlAttr * attr;
	const xmlChar * tmpNs = NULL, * tmpName = NULL, * defValue = NULL;
	int isRef = 0, occurs = XML_SCHEMAS_ATTR_USE_OPTIONAL;
	int nberrors, hasForm = 0, defValueType = 0;

#define WXS_ATTR_DEF_VAL_DEFAULT 1
#define WXS_ATTR_DEF_VAL_FIXED 2

	/*
	 * 3.2.3 Constraints on XML Representations of Attribute Declarations
	 */
	if(!pctxt || !schema || !P_Node)
		return 0;
	attr = xmlSchemaGetPropNode(P_Node, "ref");
	if(attr) {
		if(xmlSchemaPValAttrNodeQName(pctxt, schema, NULL, attr, &tmpNs, &tmpName) != 0) {
			return 0;
		}
		if(xmlSchemaCheckReference(pctxt, schema, P_Node, attr, tmpNs) != 0)
			return 0;
		isRef = 1;
	}
	nberrors = pctxt->nberrors;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if(isRef) {
				if(sstreq(attr->name, "id")) {
					xmlSchemaPValAttrNodeID(pctxt, attr);
					goto attr_next;
				}
				else if(sstreq(attr->name, "ref")) {
					goto attr_next;
				}
			}
			else {
				if(sstreq(attr->name, "name")) {
					goto attr_next;
				}
				else if(sstreq(attr->name, "id")) {
					xmlSchemaPValAttrNodeID(pctxt, attr);
					goto attr_next;
				}
				else if(sstreq(attr->name, "type")) {
					xmlSchemaPValAttrNodeQName(pctxt, schema, NULL, attr, &tmpNs, &tmpName);
					goto attr_next;
				}
				else if(sstreq(attr->name, "form")) {
					/*
					 * Evaluate the target namespace
					 */
					hasForm = 1;
					attrValue = xmlSchemaGetNodeContent(pctxt,
					    (xmlNode *)attr);
					if(sstreq(attrValue, "qualified")) {
						ns = pctxt->targetNamespace;
					}
					else if(!sstreq(attrValue, "unqualified")) {
						xmlSchemaPSimpleTypeErr(pctxt,
						    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
						    NULL, (xmlNode *)attr,
						    NULL, "(qualified | unqualified)",
						    attrValue, NULL, 0, 0);
					}
					goto attr_next;
				}
			}
			if(sstreq(attr->name, "use")) {
				attrValue = xmlSchemaGetNodeContent(pctxt, (xmlNode *)attr);
				/* @todo Maybe we need to normalize the value beforehand. */
				if(sstreq(attrValue, "optional"))
					occurs = XML_SCHEMAS_ATTR_USE_OPTIONAL;
				else if(sstreq(attrValue, "prohibited"))
					occurs = XML_SCHEMAS_ATTR_USE_PROHIBITED;
				else if(sstreq(attrValue, "required"))
					occurs = XML_SCHEMAS_ATTR_USE_REQUIRED;
				else {
					xmlSchemaPSimpleTypeErr(pctxt,
					    XML_SCHEMAP_INVALID_ATTR_USE,
					    NULL, (xmlNode *)attr,
					    NULL, "(optional | prohibited | required)",
					    attrValue, NULL, 0, 0);
				}
				goto attr_next;
			}
			else if(sstreq(attr->name, "default")) {
				/*
				 * 3.2.3 : 1
				 * default and fixed must not both be present.
				 */
				if(defValue) {
					xmlSchemaPMutualExclAttrErr(pctxt,
					    XML_SCHEMAP_SRC_ATTRIBUTE_1,
					    NULL, attr, "default", "fixed");
				}
				else {
					defValue = xmlSchemaGetNodeContent(pctxt, (xmlNode *)attr);
					defValueType = WXS_ATTR_DEF_VAL_DEFAULT;
				}
				goto attr_next;
			}
			else if(sstreq(attr->name, "fixed")) {
				/*
				 * 3.2.3 : 1
				 * default and fixed must not both be present.
				 */
				if(defValue) {
					xmlSchemaPMutualExclAttrErr(pctxt, XML_SCHEMAP_SRC_ATTRIBUTE_1, NULL, attr, "default", "fixed");
				}
				else {
					defValue = xmlSchemaGetNodeContent(pctxt, (xmlNode *)attr);
					defValueType = WXS_ATTR_DEF_VAL_FIXED;
				}
				goto attr_next;
			}
		}
		else if(!sstreq(attr->ns->href, xmlSchemaNs))
			goto attr_next;
		xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
attr_next:
		attr = attr->next;
	}
	/*
	 * 3.2.3 : 2
	 * If default and use are both present, use must have
	 * the actual value optional.
	 */
	if((defValueType == WXS_ATTR_DEF_VAL_DEFAULT) &&
	    (occurs != XML_SCHEMAS_ATTR_USE_OPTIONAL)) {
		xmlSchemaPSimpleTypeErr(pctxt,
		    XML_SCHEMAP_SRC_ATTRIBUTE_2,
		    NULL, P_Node, NULL,
		    "(optional | prohibited | required)", NULL,
		    "The value of the attribute 'use' must be 'optional' "
		    "if the attribute 'default' is present",
		    NULL, 0);
	}
	/*
	 * We want correct attributes.
	 */
	if(nberrors != pctxt->nberrors)
		return 0;
	if(!isRef) {
		xmlSchemaAttributePtr attrDecl;

		/* @todo move XML_SCHEMAS_QUALIF_ATTR to the parser. */
		if((!hasForm) && (schema->flags & XML_SCHEMAS_QUALIF_ATTR))
			ns = pctxt->targetNamespace;
		/*
		 * 3.2.6 Schema Component Constraint: xsi: Not Allowed
		 * @todo Move this to the component layer.
		 */
		if(sstreq(ns, xmlSchemaInstanceNs)) {
			xmlSchemaCustomErr(ACTXT_CAST pctxt,
			    XML_SCHEMAP_NO_XSI,
			    P_Node, NULL,
			    "The target namespace must not match '%s'",
			    xmlSchemaInstanceNs, 0);
		}
		attr = xmlSchemaGetPropNode(P_Node, "name");
		if(!attr) {
			xmlSchemaPMissingAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_MISSING,
			    NULL, P_Node, "name", 0);
			return 0;
		}
		if(xmlSchemaPValAttrNode(pctxt, NULL, attr,
			    xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &name) != 0) {
			return 0;
		}
		/*
		 * 3.2.6 Schema Component Constraint: xmlns Not Allowed
		 * @todo Move this to the component layer.
		 */
		if(sstreq(name, "xmlns")) {
			xmlSchemaPSimpleTypeErr(pctxt,
			    XML_SCHEMAP_NO_XMLNS,
			    NULL, (xmlNode *)attr,
			    xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), NULL, NULL,
			    "The value of the attribute must not match 'xmlns'",
			    NULL, 0);
			return 0;
		}
		if(occurs == XML_SCHEMAS_ATTR_USE_PROHIBITED)
			goto check_children;
		/*
		 * Create the attribute use component.
		 */
		use = xmlSchemaAddAttributeUse(pctxt, P_Node);
		if(use == NULL)
			return 0;
		use->occurs = occurs;
		/*
		 * Create the attribute declaration.
		 */
		attrDecl = xmlSchemaAddAttribute(pctxt, schema, name, ns, P_Node, 0);
		if(attrDecl == NULL)
			return 0;
		if(tmpName) {
			attrDecl->typeName = tmpName;
			attrDecl->typeNs = tmpNs;
		}
		use->attrDecl = attrDecl;
		/*
		 * Value constraint.
		 */
		if(defValue) {
			attrDecl->defValue = defValue;
			if(defValueType == WXS_ATTR_DEF_VAL_FIXED)
				attrDecl->flags |= XML_SCHEMAS_ATTR_FIXED;
		}
	}
	else if(occurs != XML_SCHEMAS_ATTR_USE_PROHIBITED) {
		xmlSchemaQNameRefPtr ref;

		/*
		 * Create the attribute use component.
		 */
		use = xmlSchemaAddAttributeUse(pctxt, P_Node);
		if(use == NULL)
			return 0;
		/*
		 * We need to resolve the reference at later stage.
		 */
		WXS_ADD_PENDING(pctxt, use);
		use->occurs = occurs;
		/*
		 * Create a QName reference to the attribute declaration.
		 */
		ref = xmlSchemaNewQNameRef(pctxt, XML_SCHEMA_TYPE_ATTRIBUTE,
		    tmpName, tmpNs);
		if(ref == NULL)
			return 0;
		/*
		 * Assign the reference. This will be substituted for the
		 * referenced attribute declaration when the QName is resolved.
		 */
		use->attrDecl = WXS_ATTR_CAST ref;
		/*
		 * Value constraint.
		 */
		if(defValue)
			use->defValue = defValue;
		if(defValueType == WXS_ATTR_DEF_VAL_FIXED)
			use->flags |= XML_SCHEMA_ATTR_USE_FIXED;
	}

check_children:
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(occurs == XML_SCHEMAS_ATTR_USE_PROHIBITED) {
		xmlSchemaAttributeUseProhibPtr prohib;
		if(IS_SCHEMA(child, "annotation")) {
			xmlSchemaParseAnnotation(pctxt, child, 0);
			child = child->next;
		}
		if(child)
			xmlSchemaPContentErr(pctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
		/*
		 * Check for pointlessness of attribute prohibitions.
		 */
		if(parentType == XML_SCHEMA_TYPE_ATTRIBUTEGROUP) {
			xmlSchemaCustomWarning(ACTXT_CAST pctxt, XML_SCHEMAP_WARN_ATTR_POINTLESS_PROH, P_Node, NULL,
			    "Skipping attribute use prohibition, since it is pointless inside an <attributeGroup>", NULL, 0, 0);
			return 0;
		}
		else if(parentType == XML_SCHEMA_TYPE_EXTENSION) {
			xmlSchemaCustomWarning(ACTXT_CAST pctxt, XML_SCHEMAP_WARN_ATTR_POINTLESS_PROH, P_Node, NULL,
			    "Skipping attribute use prohibition, since it is pointless when extending a type", NULL, 0, 0);
			return 0;
		}
		if(!isRef) {
			tmpName = name;
			tmpNs = ns;
		}
		/*
		 * Check for duplicate attribute prohibitions.
		 */
		if(uses) {
			for(int i = 0; i < uses->nbItems; i++) {
				use = (xmlSchemaAttributeUsePtr)uses->items[i];
				if((use->type == XML_SCHEMA_EXTRA_ATTR_USE_PROHIB) && (tmpName == (WXS_ATTR_PROHIB_CAST use)->name) && (tmpNs == (WXS_ATTR_PROHIB_CAST use)->targetNamespace)) {
					xmlChar * str = NULL;
					xmlSchemaCustomWarning(ACTXT_CAST pctxt, XML_SCHEMAP_WARN_ATTR_POINTLESS_PROH,
					    P_Node, NULL, "Skipping duplicate attribute use prohibition '%s'", xmlSchemaFormatQName(&str, tmpNs, tmpName), NULL, 0);
					ZFREE(str);
					return 0;
				}
			}
		}
		/*
		 * Create the attribute prohibition helper component.
		 */
		prohib = xmlSchemaAddAttributeUseProhib(pctxt);
		if(prohib == NULL)
			return 0;
		prohib->P_Node = P_Node;
		prohib->name = tmpName;
		prohib->targetNamespace = tmpNs;
		if(isRef) {
			/*
			 * We need at least to resolve to the attribute declaration.
			 */
			WXS_ADD_PENDING(pctxt, prohib);
		}
		return(WXS_BASIC_CAST prohib);
	}
	else {
		if(IS_SCHEMA(child, "annotation")) {
			/*
			 * @todo Should this go into the attr decl?
			 */
			use->annot = xmlSchemaParseAnnotation(pctxt, child, 1);
			child = child->next;
		}
		if(isRef) {
			if(child) {
				if(IS_SCHEMA(child, "simpleType"))
					/*
					 * 3.2.3 : 3.2
					 * If ref is present, then all of <simpleType>,
					 * form and type must be absent.
					 */
					xmlSchemaPContentErr(pctxt, XML_SCHEMAP_SRC_ATTRIBUTE_3_2, NULL, P_Node, child, NULL, "(annotation?)");
				else
					xmlSchemaPContentErr(pctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
			}
		}
		else {
			if(IS_SCHEMA(child, "simpleType")) {
				if(WXS_ATTRUSE_DECL(use)->typeName) {
					/*
					 * 3.2.3 : 4
					 * type and <simpleType> must not both be present.
					 */
					xmlSchemaPContentErr(pctxt, XML_SCHEMAP_SRC_ATTRIBUTE_4, NULL, P_Node, child,
					    "The attribute 'type' and the <simpleType> child are mutually exclusive", 0);
				}
				else
					WXS_ATTRUSE_TYPEDEF(use) = xmlSchemaParseSimpleType(pctxt, schema, child, 0);
				child = child->next;
			}
			if(child)
				xmlSchemaPContentErr(pctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, simpleType?)");
		}
	}
	return (WXS_BASIC_CAST use);
}

static xmlSchemaAttributePtr xmlSchemaParseGlobalAttribute(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	const xmlChar * attrValue;
	xmlSchemaAttributePtr ret;
	xmlNode * child = NULL;
	xmlAttr * attr;
	/*
	 * Note that the w3c spec assumes the schema to be validated with schema
	 * for schemas beforehand.
	 *
	 * 3.2.3 Constraints on XML Representations of Attribute Declarations
	 */
	if(!pctxt || !schema || !P_Node)
		return 0;
	/*
	 * 3.2.3 : 3.1
	 * One of ref or name must be present, but not both
	 */
	attr = xmlSchemaGetPropNode(P_Node, "name");
	if(!attr) {
		xmlSchemaPMissingAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "name", 0);
		return 0;
	}
	if(xmlSchemaPValAttrNode(pctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &attrValue) != 0) {
		return 0;
	}
	/*
	 * 3.2.6 Schema Component Constraint: xmlns Not Allowed
	 * @todo Move this to the component layer.
	 */
	if(sstreq(attrValue, "xmlns")) {
		xmlSchemaPSimpleTypeErr(pctxt, XML_SCHEMAP_NO_XMLNS, NULL, (xmlNode *)attr,
		    xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), NULL, NULL, "The value of the attribute must not match 'xmlns'", NULL, 0);
		return 0;
	}
	/*
	 * 3.2.6 Schema Component Constraint: xsi: Not Allowed
	 * @todo Move this to the component layer.
	 *       Or better leave it here and add it to the component layer
	 *       if we have a schema construction API.
	 */
	if(sstreq(pctxt->targetNamespace, xmlSchemaInstanceNs)) {
		xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_NO_XSI, P_Node, NULL,
		    "The target namespace must not match '%s'", xmlSchemaInstanceNs, 0);
	}
	ret = xmlSchemaAddAttribute(pctxt, schema, attrValue, pctxt->targetNamespace, P_Node, 1);
	if(!ret)
		return 0;
	ret->flags |= XML_SCHEMAS_ATTR_GLOBAL;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "default")) && (!sstreq(attr->name, "fixed")) && (!sstreq(attr->name, "name")) && (!sstreq(attr->name, "type"))) {
				xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrQName(pctxt, schema, NULL, P_Node, "type", &ret->typeNs, &ret->typeName);
	xmlSchemaPValAttrID(pctxt, P_Node, BAD_CAST "id");
	/*
	 * Attribute "fixed".
	 */
	ret->defValue = xmlSchemaGetProp(pctxt, P_Node, "fixed");
	if(ret->defValue)
		ret->flags |= XML_SCHEMAS_ATTR_FIXED;
	/*
	 * Attribute "default".
	 */
	attr = xmlSchemaGetPropNode(P_Node, "default");
	if(attr) {
		/*
		 * 3.2.3 : 1
		 * default and fixed must not both be present.
		 */
		if(ret->flags & XML_SCHEMAS_ATTR_FIXED) {
			xmlSchemaPMutualExclAttrErr(pctxt, XML_SCHEMAP_SRC_ATTRIBUTE_1, WXS_BASIC_CAST ret, attr, "default", "fixed");
		}
		else
			ret->defValue = xmlSchemaGetNodeContent(pctxt, (xmlNode *)attr);
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		ret->annot = xmlSchemaParseAnnotation(pctxt, child, 1);
		child = child->next;
	}
	if(IS_SCHEMA(child, "simpleType")) {
		if(ret->typeName) {
			/*
			 * 3.2.3 : 4
			 * type and <simpleType> must not both be present.
			 */
			xmlSchemaPContentErr(pctxt, XML_SCHEMAP_SRC_ATTRIBUTE_4, NULL, P_Node, child,
			    "The attribute 'type' and the <simpleType> child are mutually exclusive", 0);
		}
		else
			ret->subtypes = xmlSchemaParseSimpleType(pctxt, schema, child, 0);
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(pctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, simpleType?)");
	return ret;
}

/**
 * xmlSchemaParseAttributeGroupRef:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * Parse an attribute group definition reference.
 * Note that a reference to an attribute group does not
 * correspond to any component at all.
 * *WARNING* this interface is highly subject to change
 *
 * Returns the attribute group or NULL in case of error.
 */
static xmlSchemaQNameRefPtr xmlSchemaParseAttributeGroupRef(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaQNameRefPtr ret;
	xmlNode * child = NULL;
	xmlAttr * attr;
	const xmlChar * refNs = NULL, * ref = NULL;
	if(!pctxt || !schema || !P_Node)
		return 0;
	attr = xmlSchemaGetPropNode(P_Node, "ref");
	if(!attr) {
		xmlSchemaPMissingAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "ref", 0);
		return 0;
	}
	xmlSchemaPValAttrNodeQName(pctxt, schema, NULL, attr, &refNs, &ref);
	if(xmlSchemaCheckReference(pctxt, schema, P_Node, attr, refNs) != 0)
		return 0;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "ref")) && (!sstreq(attr->name, "id"))) {
				xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	/* Attribute ID */
	xmlSchemaPValAttrID(pctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * @todo We do not have a place to store the annotation, do we?
		 */
		xmlSchemaParseAnnotation(pctxt, child, 0);
		child = child->next;
	}
	if(child) {
		xmlSchemaPContentErr(pctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
	}
	/*
	 * Handle attribute group redefinitions.
	 */
	if(pctxt->isRedefine && pctxt->redef && (pctxt->redef->item->type == XML_SCHEMA_TYPE_ATTRIBUTEGROUP) && (ref == pctxt->redef->refName) && (refNs == pctxt->redef->refTargetNs)) {
		/*
		 * SPEC src-redefine:
		 * (7.1) "If it has an <attributeGroup> among its contents
		 * the `actual value` of whose ref [attribute] is the same
		 * as the `actual value` of its own name attribute plus
		 * target namespace, then it must have exactly one such group."
		 */
		if(pctxt->redefCounter != 0) {
			xmlChar * str = NULL;
			xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_SRC_REDEFINE, P_Node, NULL,
			    "The redefining attribute group definition '%s' must not contain more than one reference to the redefined definition",
			    xmlSchemaFormatQName(&str, refNs, ref), 0);
			ZFREE(str);
			return 0;
		}
		pctxt->redefCounter++;
		/*
		 * URGENT TODO: How to ensure that the reference will not be
		 * handled by the normal component resolution mechanism?
		 */
		ret = xmlSchemaNewQNameRef(pctxt, XML_SCHEMA_TYPE_ATTRIBUTEGROUP, ref, refNs);
		if(!ret)
			return 0;
		ret->P_Node = P_Node;
		pctxt->redef->reference = WXS_BASIC_CAST ret;
	}
	else {
		/*
		 * Create a QName-reference helper component. We will substitute this
		 * component for the attribute uses of the referenced attribute group
		 * definition.
		 */
		ret = xmlSchemaNewQNameRef(pctxt,
		    XML_SCHEMA_TYPE_ATTRIBUTEGROUP, ref, refNs);
		if(!ret)
			return 0;
		ret->P_Node = P_Node;
		/* Add to pending items, to be able to resolve the reference. */
		WXS_ADD_PENDING(pctxt, ret);
	}
	return ret;
}

/**
 * xmlSchemaParseAttributeGroupDefinition:
 * @pctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Attribute Group declaration
 * *WARNING* this interface is highly subject to change
 *
 * Returns the attribute group definition or NULL in case of error.
 */
static xmlSchemaAttributeGroupPtr xmlSchemaParseAttributeGroupDefinition(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	const xmlChar * name;
	xmlSchemaAttributeGroupPtr ret;
	xmlNode * child = NULL;
	xmlAttr * attr;
	int hasRefs = 0;
	if(!pctxt || !schema || !P_Node)
		return 0;
	attr = xmlSchemaGetPropNode(P_Node, "name");
	if(!attr) {
		xmlSchemaPMissingAttrErr(pctxt,
		    XML_SCHEMAP_S4S_ATTR_MISSING,
		    NULL, P_Node, "name", 0);
		return 0;
	}
	/*
	 * The name is crucial, exit if invalid.
	 */
	if(xmlSchemaPValAttrNode(pctxt,
		    NULL, attr,
		    xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &name) != 0) {
		return 0;
	}
	ret = xmlSchemaAddAttributeGroupDefinition(pctxt, schema,
	    name, pctxt->targetNamespace, P_Node);
	if(!ret)
		return 0;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "name")) && (!sstreq(attr->name, "id"))) {
				xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	/* Attribute ID */
	xmlSchemaPValAttrID(pctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		ret->annot = xmlSchemaParseAnnotation(pctxt, child, 1);
		child = child->next;
	}
	/*
	 * Parse contained attribute decls/refs.
	 */
	if(xmlSchemaParseLocalAttributes(pctxt, schema, &child, (xmlSchemaItemListPtr*)&(ret->attrUses), XML_SCHEMA_TYPE_ATTRIBUTEGROUP, &hasRefs) == -1)
		return 0;
	if(hasRefs)
		ret->flags |= XML_SCHEMAS_ATTRGROUP_HAS_REFS;
	/*
	 * Parse the attribute wildcard.
	 */
	if(IS_SCHEMA(child, "anyAttribute")) {
		ret->attributeWildcard = xmlSchemaParseAnyAttribute(pctxt, schema, child);
		child = child->next;
	}
	if(child) {
		xmlSchemaPContentErr(pctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL,
		    "(annotation?, ((attribute | attributeGroup)*, anyAttribute?))");
	}
	return ret;
}
/**
 * xmlSchemaPValAttrFormDefault:
 * @value:  the value
 * @flags: the flags to be modified
 * @flagQualified: the specific flag for "qualified"
 *
 * Returns 0 if the value is valid, 1 otherwise.
 */
static int xmlSchemaPValAttrFormDefault(const xmlChar * value, int * flags, int flagQualified)
{
	if(sstreq(value, "qualified")) {
		if((*flags & flagQualified) == 0)
			*flags |= flagQualified;
	}
	else if(!sstreq(value, "unqualified"))
		return 1;
	return 0;
}
/**
 * xmlSchemaPValAttrBlockFinal:
 * @value:  the value
 * @flags: the flags to be modified
 * @flagAll: the specific flag for "#all"
 * @flagExtension: the specific flag for "extension"
 * @flagRestriction: the specific flag for "restriction"
 * @flagSubstitution: the specific flag for "substitution"
 * @flagList: the specific flag for "list"
 * @flagUnion: the specific flag for "union"
 *
 * Validates the value of the attribute "final" and "block". The value
 * is converted into the specified flag values and returned in @flags.
 *
 * Returns 0 if the value is valid, 1 otherwise.
 */
static int xmlSchemaPValAttrBlockFinal(const xmlChar * value, int * flags, int flagAll, int flagExtension,
    int flagRestriction, int flagSubstitution, int flagList, int flagUnion)
{
	int ret = 0;
	/*
	 * @todo This does not check for dublicate entries.
	 */
	if((flags == NULL) || (value == NULL))
		return -1;
	if(value[0] == 0)
		return 0;
	if(sstreq(value, "#all")) {
		if(flagAll != -1)
			*flags |= flagAll;
		else {
			if(flagExtension != -1)
				*flags |= flagExtension;
			if(flagRestriction != -1)
				*flags |= flagRestriction;
			if(flagSubstitution != -1)
				*flags |= flagSubstitution;
			if(flagList != -1)
				*flags |= flagList;
			if(flagUnion != -1)
				*flags |= flagUnion;
		}
	}
	else {
		const xmlChar * end, * cur = value;
		xmlChar * item;
		do {
			while(IS_BLANK_CH(*cur))
				cur++;
			end = cur;
			while((*end != 0) && (!(IS_BLANK_CH(*end))))
				end++;
			if(end == cur)
				break;
			item = xmlStrndup(cur, end - cur);
			if(sstreq(item, "extension")) {
				if(flagExtension != -1) {
					if((*flags & flagExtension) == 0)
						*flags |= flagExtension;
				}
				else
					ret = 1;
			}
			else if(sstreq(item, "restriction")) {
				if(flagRestriction != -1) {
					if((*flags & flagRestriction) == 0)
						*flags |= flagRestriction;
				}
				else
					ret = 1;
			}
			else if(sstreq(item, "substitution")) {
				if(flagSubstitution != -1) {
					if((*flags & flagSubstitution) == 0)
						*flags |= flagSubstitution;
				}
				else
					ret = 1;
			}
			else if(sstreq(item, "list")) {
				if(flagList != -1) {
					if((*flags & flagList) == 0)
						*flags |= flagList;
				}
				else
					ret = 1;
			}
			else if(sstreq(item, "union")) {
				if(flagUnion != -1) {
					if((*flags & flagUnion) == 0)
						*flags |= flagUnion;
				}
				else
					ret = 1;
			}
			else
				ret = 1;
			SAlloc::F(item);
			cur = end;
		} while((ret == 0) && (*cur != 0));
	}
	return ret;
}

static int xmlSchemaCheckCSelectorXPath(xmlSchemaParserCtxtPtr ctxt, xmlSchemaIDCPtr idc, xmlSchemaIDCSelectPtr selector, xmlAttrPtr attr, int isField)
{
	xmlNode * P_Node;
	/*
	 * c-selector-xpath:
	 * Schema Component Constraint: Selector Value OK
	 *
	 * @todo 1 The {selector} must be a valid XPath expression, as defined
	 * in [XPath].
	 */
	if(selector == NULL) {
		xmlSchemaPErr(ctxt, idc->P_Node, XML_SCHEMAP_INTERNAL, "Internal error: xmlSchemaCheckCSelectorXPath, the selector is not specified.\n", 0, 0);
		return -1;
	}
	P_Node = attr ? (xmlNode *)attr : idc->P_Node;
	if(selector->xpath == NULL) {
		xmlSchemaPCustomErr(ctxt,
		    /* @todo Adjust error code. */
		    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, NULL, P_Node, "The XPath expression of the selector is not valid", 0);
		return (XML_SCHEMAP_S4S_ATTR_INVALID_VALUE);
	}
	else {
		const xmlChar ** nsArray = NULL;
		xmlNs ** nsList = NULL;
		/*
		 * Compile the XPath expression.
		 */
		/*
		 * @todo We need the array of in-scope namespaces for compilation.
		 * @todo Call xmlPatterncompile with different options for selector/field.
		 */
		nsList = attr ? xmlGetNsList(attr->doc, attr->parent) : 0;
		/*
		 * Build an array of prefixes and namespaces.
		 */
		if(nsList) {
			int i, count = 0;
			for(i = 0; nsList[i]; i++)
				count++;
			nsArray = (const xmlChar**)SAlloc::M((count * 2 + 1) * sizeof(const xmlChar *));
			if(nsArray == NULL) {
				xmlSchemaPErrMemory(ctxt, "allocating a namespace array", 0);
				SAlloc::F(nsList);
				return -1;
			}
			for(i = 0; i < count; i++) {
				nsArray[2 * i] = nsList[i]->href;
				nsArray[2 * i + 1] = nsList[i]->prefix;
			}
			nsArray[count * 2] = NULL;
			SAlloc::F(nsList);
		}
		/*
		 * @todo Differentiate between "selector" and "field".
		 */
		if(isField)
			selector->xpathComp = (void*)xmlPatterncompile(selector->xpath,
			    NULL, XML_PATTERN_XSFIELD, nsArray);
		else
			selector->xpathComp = (void*)xmlPatterncompile(selector->xpath,
			    NULL, XML_PATTERN_XSSEL, nsArray);
		SAlloc::F((xmlChar**)nsArray);
		if(selector->xpathComp == NULL) {
			xmlSchemaPCustomErr(ctxt,
			    /* @todo Adjust error code? */
			    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
			    NULL, P_Node,
			    "The XPath expression '%s' could not be "
			    "compiled", selector->xpath);
			return (XML_SCHEMAP_S4S_ATTR_INVALID_VALUE);
		}
	}
	return 0;
}

#define ADD_ANNOTATION(annot)	\
	xmlSchemaAnnotPtr cur = item->annot; \
	if(item->annot == NULL) {  \
		item->annot = annot;	\
		return (annot);		\
	}			    \
	cur = item->annot;	    \
	if(cur->next) {	   \
		cur = cur->next;	\
	}			    \
	cur->next = annot;

/**
 * xmlSchemaAssignAnnotation:
 * @item: the schema component
 * @annot: the annotation
 *
 * Adds the annotation to the given schema component.
 *
 * Returns the given annotaion.
 */
static xmlSchemaAnnotPtr xmlSchemaAddAnnotation(xmlSchemaAnnotItem * annItem, xmlSchemaAnnotPtr annot)
{
	if((annItem == NULL) || (annot == NULL))
		return 0;
	switch(annItem->type) {
		case XML_SCHEMA_TYPE_ELEMENT: {
		    xmlSchemaElementPtr item = (xmlSchemaElementPtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_ATTRIBUTE: {
		    xmlSchemaAttributePtr item = (xmlSchemaAttributePtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_ANY_ATTRIBUTE:
		case XML_SCHEMA_TYPE_ANY: {
		    xmlSchemaWildcardPtr item = (xmlSchemaWildcardPtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_PARTICLE:
		case XML_SCHEMA_TYPE_IDC_KEY:
		case XML_SCHEMA_TYPE_IDC_KEYREF:
		case XML_SCHEMA_TYPE_IDC_UNIQUE: {
		    xmlSchemaAnnotItem * item = (xmlSchemaAnnotItem * )annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_ATTRIBUTEGROUP: {
		    xmlSchemaAttributeGroupPtr item = (xmlSchemaAttributeGroupPtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_NOTATION: {
		    xmlSchemaNotationPtr item = (xmlSchemaNotationPtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_FACET_MININCLUSIVE:
		case XML_SCHEMA_FACET_MINEXCLUSIVE:
		case XML_SCHEMA_FACET_MAXINCLUSIVE:
		case XML_SCHEMA_FACET_MAXEXCLUSIVE:
		case XML_SCHEMA_FACET_TOTALDIGITS:
		case XML_SCHEMA_FACET_FRACTIONDIGITS:
		case XML_SCHEMA_FACET_PATTERN:
		case XML_SCHEMA_FACET_ENUMERATION:
		case XML_SCHEMA_FACET_WHITESPACE:
		case XML_SCHEMA_FACET_LENGTH:
		case XML_SCHEMA_FACET_MAXLENGTH:
		case XML_SCHEMA_FACET_MINLENGTH: {
		    xmlSchemaFacetPtr item = (xmlSchemaFacetPtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_SIMPLE:
		case XML_SCHEMA_TYPE_COMPLEX: {
		    xmlSchemaTypePtr item = (xmlSchemaTypePtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_GROUP: {
		    xmlSchemaModelGroupDefPtr item = (xmlSchemaModelGroupDefPtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		case XML_SCHEMA_TYPE_SEQUENCE:
		case XML_SCHEMA_TYPE_CHOICE:
		case XML_SCHEMA_TYPE_ALL: {
		    xmlSchemaModelGroupPtr item = (xmlSchemaModelGroupPtr)annItem;
		    ADD_ANNOTATION(annot)
	    }
	    break;
		default:
		    xmlSchemaPCustomErr(NULL,
		    XML_SCHEMAP_INTERNAL,
		    NULL, NULL,
		    "Internal error: xmlSchemaAddAnnotation, "
		    "The item is not a annotated schema component", 0);
		    break;
	}
	return (annot);
}

/**
 * xmlSchemaParseIDCSelectorAndField:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * Parses a XML Schema identity-contraint definition's
 * <selector> and <field> elements.
 *
 * Returns the parsed identity-constraint definition.
 */
static xmlSchemaIDCSelectPtr xmlSchemaParseIDCSelectorAndField(xmlSchemaParserCtxtPtr ctxt, xmlSchemaIDCPtr idc, xmlNode * P_Node, int isField)
{
	xmlSchemaIDCSelectPtr item;
	xmlNode * child = NULL;
	/*
	 * Check for illegal attributes.
	 */
	xmlAttrPtr attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "xpath"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	/*
	 * Create the item.
	 */
	item = (xmlSchemaIDCSelectPtr)SAlloc::M(sizeof(xmlSchemaIDCSelect));
	if(item == NULL) {
		xmlSchemaPErrMemory(ctxt, "allocating a 'selector' of an identity-constraint definition", 0);
		return 0;
	}
	memzero(item, sizeof(xmlSchemaIDCSelect));
	/*
	 * Attribute "xpath" (mandatory).
	 */
	attr = xmlSchemaGetPropNode(P_Node, "xpath");
	if(!attr) {
		xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "name", 0);
	}
	else {
		item->xpath = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
		/*
		 * URGENT TODO: "field"s have an other syntax than "selector"s.
		 */
		if(xmlSchemaCheckCSelectorXPath(ctxt, idc, item, attr, isField) == -1) {
			xmlSchemaPErr(ctxt, (xmlNode *)attr, XML_SCHEMAP_INTERNAL, "Internal error: xmlSchemaParseIDCSelectorAndField, validating the XPath expression of a IDC selector.\n", 0, 0);
		}
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * Add the annotation to the parent IDC.
		 */
		xmlSchemaAddAnnotation((xmlSchemaAnnotItem *)idc, xmlSchemaParseAnnotation(ctxt, child, 1));
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
	return (item);
}

/**
 * xmlSchemaParseIDC:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * Parses a XML Schema identity-contraint definition.
 *
 * Returns the parsed identity-constraint definition.
 */
static xmlSchemaIDCPtr xmlSchemaParseIDC(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, xmlSchemaTypeType idcCategory, const xmlChar * targetNamespace)
{
	xmlSchemaIDCPtr item = NULL;
	xmlNode * child = NULL;
	xmlAttr * attr;
	const xmlChar * name = NULL;
	xmlSchemaIDCSelectPtr field = NULL, lastField = NULL;

	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "name")) && ((idcCategory != XML_SCHEMA_TYPE_IDC_KEYREF) || (!sstreq(attr->name, "refer")))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	/*
	 * Attribute "name" (mandatory).
	 */
	attr = xmlSchemaGetPropNode(P_Node, "name");
	if(!attr) {
		xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "name", 0);
		return 0;
	}
	else if(xmlSchemaPValAttrNode(ctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &name) != 0) {
		return 0;
	}
	/* Create the component. */
	item = xmlSchemaAddIDC(ctxt, schema, name, targetNamespace, idcCategory, P_Node);
	if(item == NULL)
		return 0;
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	if(idcCategory == XML_SCHEMA_TYPE_IDC_KEYREF) {
		/*
		 * Attribute "refer" (mandatory).
		 */
		attr = xmlSchemaGetPropNode(P_Node, "refer");
		if(!attr) {
			xmlSchemaPMissingAttrErr(ctxt,
			    XML_SCHEMAP_S4S_ATTR_MISSING,
			    NULL, P_Node,
			    "refer", 0);
		}
		else {
			/*
			 * Create a reference item.
			 */
			item->ref = xmlSchemaNewQNameRef(ctxt, XML_SCHEMA_TYPE_IDC_KEY,
			    NULL, 0);
			if(item->ref == NULL)
				return 0;
			xmlSchemaPValAttrNodeQName(ctxt, schema, NULL, attr, &(item->ref->targetNamespace), &(item->ref->name));
			xmlSchemaCheckReference(ctxt, schema, P_Node, attr, item->ref->targetNamespace);
		}
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		item->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	if(child == NULL) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_MISSING, NULL, P_Node, child, "A child element is missing", "(annotation?, (selector, field+))");
	}
	/*
	 * Child element <selector>.
	 */
	if(IS_SCHEMA(child, "selector")) {
		item->selector = xmlSchemaParseIDCSelectorAndField(ctxt, item, child, 0);
		child = child->next;
		/*
		 * Child elements <field>.
		 */
		if(IS_SCHEMA(child, "field")) {
			do {
				field = xmlSchemaParseIDCSelectorAndField(ctxt, item, child, 1);
				if(field) {
					field->index = item->nbFields;
					item->nbFields++;
					if(lastField)
						lastField->next = field;
					else
						item->fields = field;
					lastField = field;
				}
				child = child->next;
			} while(IS_SCHEMA(child, "field"));
		}
		else
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (selector, field+))");
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (selector, field+))");
	return (item);
}

/**
 * xmlSchemaParseElement:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 * @topLevel: indicates if this is global declaration
 *
 * Parses a XML schema element declaration.
 * *WARNING* this interface is highly subject to change
 *
 * Returns the element declaration or a particle; NULL in case
 * of an error or if the particle has minOccurs==maxOccurs==0.
 */
static xmlSchemaBasicItem * xmlSchemaParseElement(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, int * isElemRef, int topLevel)
{
	xmlSchemaElementPtr decl = NULL;
	xmlSchemaParticlePtr particle = NULL;
	xmlSchemaAnnotPtr annot = NULL;
	xmlNode * child = NULL;
	xmlAttrPtr attr, nameAttr;
	int min, max, isRef = 0;
	xmlChar * des = NULL;
	/* 3.3.3 Constraints on XML Representations of Element Declarations */
	/* @todo Complete implementation of 3.3.6 */
	if(!ctxt || !schema || !P_Node)
		return 0;
	ASSIGN_PTR(isElemRef, 0);
	/*
	 * If we get a "ref" attribute on a local <element> we will assume it's
	 * a reference - even if there's a "name" attribute; this seems to be more
	 * robust.
	 */
	nameAttr = xmlSchemaGetPropNode(P_Node, "name");
	attr = xmlSchemaGetPropNode(P_Node, "ref");
	if((topLevel) || (attr == NULL)) {
		if(nameAttr == NULL) {
			xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "name", 0);
			return 0;
		}
	}
	else
		isRef = 1;
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	/*
	 * Skip particle part if a global declaration.
	 */
	if(topLevel)
		goto declaration_part;
	/*
	 * The particle part ==================================================
	 */
	min = xmlGetMinOccurs(ctxt, P_Node, 0, -1, 1, "xs:nonNegativeInteger");
	max = xmlGetMaxOccurs(ctxt, P_Node, 0, UNBOUNDED, 1, "(xs:nonNegativeInteger | unbounded)");
	xmlSchemaPCheckParticleCorrect_2(ctxt, NULL, P_Node, min, max);
	particle = xmlSchemaAddParticle(ctxt, P_Node, min, max);
	if(particle == NULL)
		goto return_null;
	/* ret->flags |= XML_SCHEMAS_ELEM_REF; */
	if(isRef) {
		const xmlChar * refNs = NULL, * ref = NULL;
		xmlSchemaQNameRefPtr refer = NULL;
		/*
		 * The reference part =============================================
		 */
		ASSIGN_PTR(isElemRef, 1);
		xmlSchemaPValAttrNodeQName(ctxt, schema, NULL, attr, &refNs, &ref);
		xmlSchemaCheckReference(ctxt, schema, P_Node, attr, refNs);
		/*
		 * SPEC (3.3.3 : 2.1) "One of ref or name must be present, but not both"
		 */
		if(nameAttr) {
			xmlSchemaPMutualExclAttrErr(ctxt, XML_SCHEMAP_SRC_ELEMENT_2_1, NULL, nameAttr, "ref", "name");
		}
		/*
		 * Check for illegal attributes.
		 */
		attr = P_Node->properties;
		while(attr) {
			if(attr->ns == NULL) {
				if(sstreq(attr->name, "ref") || sstreq(attr->name, "name") || sstreq(attr->name, "id") || sstreq(attr->name, "maxOccurs") || sstreq(attr->name, "minOccurs")) {
					attr = attr->next;
					continue;
				}
				else {
					/* SPEC (3.3.3 : 2.2) */
					xmlSchemaPCustomAttrErr(ctxt, XML_SCHEMAP_SRC_ELEMENT_2_2, NULL, NULL, attr,
					    "Only the attributes 'minOccurs', 'maxOccurs' and 'id' are allowed in addition to 'ref'");
					break;
				}
			}
			else if(sstreq(attr->ns->href, xmlSchemaNs)) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
			attr = attr->next;
		}
		/*
		 * No children except <annotation> expected.
		 */
		if(child)
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
		if((min == 0) && (max == 0))
			goto return_null;
		/*
		 * Create the reference item and attach it to the particle.
		 */
		refer = xmlSchemaNewQNameRef(ctxt, XML_SCHEMA_TYPE_ELEMENT, ref, refNs);
		if(refer == NULL)
			goto return_null;
		particle->children = (xmlSchemaTreeItem *)refer;
		particle->annot = annot;
		/*
		 * Add the particle to pending components, since the reference
		 * need to be resolved.
		 */
		WXS_ADD_PENDING(ctxt, particle);
		return ((xmlSchemaBasicItem *)particle);
	}
	/*
	 * The declaration part ===============================================
	 */
declaration_part:
	{
		const xmlChar * ns = NULL, * fixed, * name, * attrValue;
		xmlSchemaIDCPtr curIDC = NULL, lastIDC = NULL;
		if(xmlSchemaPValAttrNode(ctxt, NULL, nameAttr, xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &name) != 0)
			goto return_null;
		/*
		 * Evaluate the target namespace.
		 */
		if(topLevel) {
			ns = ctxt->targetNamespace;
		}
		else {
			attr = xmlSchemaGetPropNode(P_Node, "form");
			if(attr) {
				attrValue = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
				if(sstreq(attrValue, "qualified")) {
					ns = ctxt->targetNamespace;
				}
				else if(!sstreq(attrValue, "unqualified")) {
					xmlSchemaPSimpleTypeErr(ctxt,
					    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
					    NULL, (xmlNode *)attr,
					    NULL, "(qualified | unqualified)",
					    attrValue, NULL, 0, 0);
				}
			}
			else if(schema->flags & XML_SCHEMAS_QUALIF_ELEM)
				ns = ctxt->targetNamespace;
		}
		decl = xmlSchemaAddElement(ctxt, name, ns, P_Node, topLevel);
		if(decl == NULL) {
			goto return_null;
		}
		/*
		 * Check for illegal attributes.
		 */
		attr = P_Node->properties;
		while(attr) {
			if(attr->ns == NULL) {
				if((!sstreq(attr->name, "name")) && (!sstreq(attr->name, "type")) && (!sstreq(attr->name, "id")) &&
				    (!sstreq(attr->name, "default")) && (!sstreq(attr->name, "fixed")) && (!sstreq(attr->name, "block")) && (!sstreq(attr->name, "nillable"))) {
					if(topLevel == 0) {
						if((!sstreq(attr->name, "maxOccurs")) && (!sstreq(attr->name, "minOccurs")) && (!sstreq(attr->name, "form"))) {
							xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
						}
					}
					else if((!sstreq(attr->name, "final")) && (!sstreq(attr->name, "abstract")) && (!sstreq(attr->name, "substitutionGroup"))) {
						xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
					}
				}
			}
			else if(sstreq(attr->ns->href, xmlSchemaNs)) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
			attr = attr->next;
		}
		/*
		 * Extract/validate attributes.
		 */
		if(topLevel) {
			/*
			 * Process top attributes of global element declarations here.
			 */
			decl->flags |= XML_SCHEMAS_ELEM_GLOBAL;
			decl->flags |= XML_SCHEMAS_ELEM_TOPLEVEL;
			xmlSchemaPValAttrQName(ctxt, schema,
			    NULL, P_Node, "substitutionGroup",
			    &(decl->substGroupNs), &(decl->substGroup));
			if(xmlGetBooleanProp(ctxt, P_Node, "abstract", 0))
				decl->flags |= XML_SCHEMAS_ELEM_ABSTRACT;
			/*
			 * Attribute "final".
			 */
			attr = xmlSchemaGetPropNode(P_Node, "final");
			if(!attr) {
				if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_EXTENSION)
					decl->flags |= XML_SCHEMAS_ELEM_FINAL_EXTENSION;
				if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_RESTRICTION)
					decl->flags |= XML_SCHEMAS_ELEM_FINAL_RESTRICTION;
			}
			else {
				attrValue = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
				if(xmlSchemaPValAttrBlockFinal(attrValue, &(decl->flags),
					    -1,
					    XML_SCHEMAS_ELEM_FINAL_EXTENSION,
					    XML_SCHEMAS_ELEM_FINAL_RESTRICTION, -1, -1, -1) != 0) {
					xmlSchemaPSimpleTypeErr(ctxt,
					    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
					    NULL, (xmlNode *)attr,
					    NULL, "(#all | List of (extension | restriction))",
					    attrValue, NULL, 0, 0);
				}
			}
		}
		/*
		 * Attribute "block".
		 */
		attr = xmlSchemaGetPropNode(P_Node, "block");
		if(!attr) {
			/*
			 * Apply default "block" values.
			 */
			if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_RESTRICTION)
				decl->flags |= XML_SCHEMAS_ELEM_BLOCK_RESTRICTION;
			if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_EXTENSION)
				decl->flags |= XML_SCHEMAS_ELEM_BLOCK_EXTENSION;
			if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_SUBSTITUTION)
				decl->flags |= XML_SCHEMAS_ELEM_BLOCK_SUBSTITUTION;
		}
		else {
			attrValue = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
			if(xmlSchemaPValAttrBlockFinal(attrValue, &(decl->flags), -1, XML_SCHEMAS_ELEM_BLOCK_EXTENSION, 
				XML_SCHEMAS_ELEM_BLOCK_RESTRICTION, XML_SCHEMAS_ELEM_BLOCK_SUBSTITUTION, -1, -1) != 0) {
				xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, NULL, (xmlNode *)attr,
				    NULL, "(#all | List of (extension | restriction | substitution))", attrValue, NULL, 0, 0);
			}
		}
		if(xmlGetBooleanProp(ctxt, P_Node, "nillable", 0))
			decl->flags |= XML_SCHEMAS_ELEM_NILLABLE;
		attr = xmlSchemaGetPropNode(P_Node, "type");
		if(attr) {
			xmlSchemaPValAttrNodeQName(ctxt, schema, NULL, attr, &(decl->namedTypeNs), &(decl->namedType));
			xmlSchemaCheckReference(ctxt, schema, P_Node, attr, decl->namedTypeNs);
		}
		decl->value = xmlSchemaGetProp(ctxt, P_Node, "default");
		attr = xmlSchemaGetPropNode(P_Node, "fixed");
		if(attr) {
			fixed = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
			if(decl->value) {
				/*
				 * 3.3.3 : 1
				 * default and fixed must not both be present.
				 */
				xmlSchemaPMutualExclAttrErr(ctxt, XML_SCHEMAP_SRC_ELEMENT_1, NULL, attr, "default", "fixed");
			}
			else {
				decl->flags |= XML_SCHEMAS_ELEM_FIXED;
				decl->value = fixed;
			}
		}
		/*
		 * And now for the children...
		 */
		if(IS_SCHEMA(child, "complexType")) {
			/*
			 * 3.3.3 : 3
			 * "type" and either <simpleType> or <complexType> are mutually
			 * exclusive
			 */
			if(decl->namedType) {
				xmlSchemaPContentErr(ctxt, XML_SCHEMAP_SRC_ELEMENT_3, NULL, P_Node, child,
				    "The attribute 'type' and the <complexType> child are mutually exclusive", 0);
			}
			else
				WXS_ELEM_TYPEDEF(decl) = xmlSchemaParseComplexType(ctxt, schema, child, 0);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "simpleType")) {
			/*
			 * 3.3.3 : 3
			 * "type" and either <simpleType> or <complexType> are
			 * mutually exclusive
			 */
			if(decl->namedType) {
				xmlSchemaPContentErr(ctxt, XML_SCHEMAP_SRC_ELEMENT_3, NULL, P_Node, child,
				    "The attribute 'type' and the <simpleType> child are mutually exclusive", 0);
			}
			else
				WXS_ELEM_TYPEDEF(decl) = xmlSchemaParseSimpleType(ctxt, schema, child, 0);
			child = child->next;
		}
		while((IS_SCHEMA(child, "unique")) || (IS_SCHEMA(child, "key")) || (IS_SCHEMA(child, "keyref"))) {
			if(IS_SCHEMA(child, "unique")) {
				curIDC = xmlSchemaParseIDC(ctxt, schema, child, XML_SCHEMA_TYPE_IDC_UNIQUE, decl->targetNamespace);
			}
			else if(IS_SCHEMA(child, "key")) {
				curIDC = xmlSchemaParseIDC(ctxt, schema, child, XML_SCHEMA_TYPE_IDC_KEY, decl->targetNamespace);
			}
			else if(IS_SCHEMA(child, "keyref")) {
				curIDC = xmlSchemaParseIDC(ctxt, schema, child, XML_SCHEMA_TYPE_IDC_KEYREF, decl->targetNamespace);
			}
			if(lastIDC)
				lastIDC->next = curIDC;
			else
				decl->idcs = (void*)curIDC;
			lastIDC = curIDC;
			child = child->next;
		}
		if(child) {
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, 
				"(annotation?, ((simpleType | complexType)?, (unique | key | keyref)*))");
		}
		decl->annot = annot;
	}
	/*
	 * NOTE: Element Declaration Representation OK 4. will be checked at a
	 * different layer.
	 */
	ZFREE(des);
	if(topLevel)
		return ((xmlSchemaBasicItem *)decl);
	else {
		particle->children = (xmlSchemaTreeItem *)decl;
		return ((xmlSchemaBasicItem *)particle);
	}
return_null:
	ZFREE(des);
	if(annot) {
		if(particle)
			particle->annot = NULL;
		if(decl)
			decl->annot = NULL;
		xmlSchemaFreeAnnot(annot);
	}
	return 0;
}

/**
 * xmlSchemaParseUnion:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Union definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns -1 in case of internal error, 0 in case of success and a positive
 * error code otherwise.
 */
static int xmlSchemaParseUnion(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaTypePtr type;
	xmlNode * child = NULL;
	xmlAttr * attr;
	const xmlChar * cur = NULL;
	if(!ctxt || !schema || !P_Node)
		return -1;
	/* Not a component, don't create it. */
	type = ctxt->ctxtType;
	/*
	 * Mark the simple type as being of variety "union".
	 */
	type->flags |= XML_SCHEMAS_TYPE_VARIETY_UNION;
	/*
	 * SPEC (Base type) (2) "If the <list> or <union> alternative is chosen,
	 * then the `simple ur-type definition`."
	 */
	type->baseType = xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYSIMPLETYPE);
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "memberTypes"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * Attribute "memberTypes". This is a list of QNames.
	 * @todo Check the value to contain anything.
	 */
	attr = xmlSchemaGetPropNode(P_Node, "memberTypes");
	if(attr) {
		const xmlChar * end;
		xmlChar * tmp;
		const xmlChar * localName, * nsName;
		xmlSchemaTypeLinkPtr link, lastLink = NULL;
		xmlSchemaQNameRefPtr ref;
		cur = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
		type->base = cur;
		do {
			while(IS_BLANK_CH(*cur))
				cur++;
			end = cur;
			while((*end != 0) && (!(IS_BLANK_CH(*end))))
				end++;
			if(end == cur)
				break;
			tmp = xmlStrndup(cur, end - cur);
			if(xmlSchemaPValAttrNodeQNameValue(ctxt, schema, NULL, attr, BAD_CAST tmp, &nsName, &localName) == 0) {
				/*
				 * Create the member type link.
				 */
				link = (xmlSchemaTypeLinkPtr)SAlloc::M(sizeof(xmlSchemaTypeLink));
				if(link == NULL) {
					xmlSchemaPErrMemory(ctxt, "xmlSchemaParseUnion, allocating a type link", 0);
					return -1;
				}
				link->type = NULL;
				link->next = NULL;
				if(lastLink == NULL)
					type->memberTypes = link;
				else
					lastLink->next = link;
				lastLink = link;
				/*
				 * Create a reference item.
				 */
				ref = xmlSchemaNewQNameRef(ctxt, XML_SCHEMA_TYPE_SIMPLE, localName, nsName);
				if(ref == NULL) {
					ZFREE(tmp);
					return -1;
				}
				/*
				 * Assign the reference to the link, it will be resolved
				 * later during fixup of the union simple type.
				 */
				link->type = (xmlSchemaTypePtr)ref;
			}
			ZFREE(tmp);
			cur = end;
		} while(*cur != 0);
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * Add the annotation to the simple type ancestor.
		 */
		xmlSchemaAddAnnotation((xmlSchemaAnnotItem *)type, xmlSchemaParseAnnotation(ctxt, child, 1));
		child = child->next;
	}
	if(IS_SCHEMA(child, "simpleType")) {
		xmlSchemaTypePtr subtype, last = NULL;
		/*
		 * Anchor the member types in the "subtypes" field of the
		 * simple type.
		 */
		while(IS_SCHEMA(child, "simpleType")) {
			subtype = (xmlSchemaTypePtr)xmlSchemaParseSimpleType(ctxt, schema, child, 0);
			if(subtype) {
				if(last == NULL) {
					type->subtypes = subtype;
					last = subtype;
				}
				else {
					last->next = subtype;
					last = subtype;
				}
				last->next = NULL;
			}
			child = child->next;
		}
	}
	if(child) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, simpleType*)");
	}
	if((attr == NULL) && (type->subtypes == NULL)) {
		/*
		 * src-union-memberTypes-or-simpleTypes
		 * Either the memberTypes [attribute] of the <union> element must
		 * be non-empty or there must be at least one simpleType [child].
		 */
		xmlSchemaPCustomErr(ctxt,
		    XML_SCHEMAP_SRC_UNION_MEMBERTYPES_OR_SIMPLETYPES,
		    NULL, P_Node,
		    "Either the attribute 'memberTypes' or "
		    "at least one <simpleType> child must be present", 0);
	}
	return 0;
}

/**
 * xmlSchemaParseList:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema List definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns -1 in case of error, 0 if the declaration is improper and
 *         1 in case of success.
 */
static xmlSchemaTypePtr xmlSchemaParseList(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaTypePtr type;
	xmlNode * child = NULL;
	xmlAttr * attr;

	if(!ctxt || !schema || !P_Node)
		return 0;
	/* Not a component, don't create it. */
	type = ctxt->ctxtType;
	/*
	 * Mark the type as being of variety "list".
	 */
	type->flags |= XML_SCHEMAS_TYPE_VARIETY_LIST;
	/*
	 * SPEC (Base type) (2) "If the <list> or <union> alternative is chosen,
	 * then the `simple ur-type definition`."
	 */
	type->baseType = xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYSIMPLETYPE);
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "itemType"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * Attribute "itemType". NOTE that we will use the "ref" and "refNs"
	 * fields for holding the reference to the itemType.
	 *
	 * REVAMP TODO: Use the "base" and "baseNs" fields, since we will remove
	 * the "ref" fields.
	 */
	xmlSchemaPValAttrQName(ctxt, schema, NULL,
	    P_Node, "itemType", &(type->baseNs), &(type->base));
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		xmlSchemaAddAnnotation((xmlSchemaAnnotItem *)type, xmlSchemaParseAnnotation(ctxt, child, 1));
		child = child->next;
	}
	if(IS_SCHEMA(child, "simpleType")) {
		/*
		 * src-list-itemType-or-simpleType
		 * Either the itemType [attribute] or the <simpleType> [child] of
		 * the <list> element must be present, but not both.
		 */
		if(type->base) {
			xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_SIMPLE_TYPE_1, NULL, P_Node,
			    "The attribute 'itemType' and the <simpleType> child are mutually exclusive", 0);
		}
		else {
			type->subtypes = xmlSchemaParseSimpleType(ctxt, schema, child, 0);
		}
		child = child->next;
	}
	else if(type->base == NULL) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_SIMPLE_TYPE_1, NULL, P_Node,
		    "Either the attribute 'itemType' or the <simpleType> child must be present", 0);
	}
	if(child) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, simpleType?)");
	}
	if(!type->base && !type->subtypes && (xmlSchemaGetPropNode(P_Node, "itemType") == NULL)) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_SIMPLE_TYPE_1, NULL, P_Node,
		    "Either the attribute 'itemType' or the <simpleType> child must be present", 0);
	}
	return 0;
}

/**
 * xmlSchemaParseSimpleType:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Simple Type definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns -1 in case of error, 0 if the declaration is improper and
 * 1 in case of success.
 */
static xmlSchemaTypePtr xmlSchemaParseSimpleType(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, int topLevel)
{
	xmlSchemaTypePtr type, oldCtxtType;
	xmlNode * child = NULL;
	const xmlChar * attrValue = NULL;
	xmlAttr * attr;
	int hasRestriction = 0;
	if(!ctxt || !schema || !P_Node)
		return 0;
	if(topLevel) {
		attr = xmlSchemaGetPropNode(P_Node, "name");
		if(!attr) {
			xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "name", 0);
			return 0;
		}
		else {
			if(xmlSchemaPValAttrNode(ctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &attrValue) != 0)
				return 0;
			/*
			 * Skip built-in types.
			 */
			if(ctxt->isS4S) {
				xmlSchemaTypePtr biType;
				if(ctxt->isRedefine) {
					/*
					 * REDEFINE: Disallow redefinition of built-in-types.
					 * @todo It seems that the spec does not say anything
					 * about this case.
					 */
					xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_REDEFINE, NULL, P_Node,
					    "Redefinition of built-in simple types is not supported", 0);
					return 0;
				}
				biType = xmlSchemaGetPredefinedType(attrValue, xmlSchemaNs);
				if(biType)
					return (biType);
			}
		}
	}
	/*
	 * TargetNamespace:
	 * SPEC "The `actual value` of the targetNamespace [attribute]
	 * of the <schema> ancestor element information item if present,
	 * otherwise `absent`.
	 */
	if(topLevel == 0) {
#ifdef ENABLE_NAMED_LOCALS
		char buf[40];
#endif
		/*
		 * Parse as local simple type definition.
		 */
#ifdef ENABLE_NAMED_LOCALS
		snprintf(buf, 39, "#ST%d", ctxt->counter++ + 1);
		type = xmlSchemaAddType(ctxt, schema, XML_SCHEMA_TYPE_SIMPLE, xmlDictLookupSL(ctxt->dict, (const xmlChar*)buf), ctxt->targetNamespace, node, 0);
#else
		type = xmlSchemaAddType(ctxt, schema, XML_SCHEMA_TYPE_SIMPLE, NULL, ctxt->targetNamespace, P_Node, 0);
#endif
		if(type == NULL)
			return 0;
		type->type = XML_SCHEMA_TYPE_SIMPLE;
		type->contentType = XML_SCHEMA_CONTENT_SIMPLE;
		/*
		 * Check for illegal attributes.
		 */
		attr = P_Node->properties;
		while(attr) {
			if(attr->ns == NULL) {
				if(!sstreq(attr->name, "id")) {
					xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
				}
			}
			else if(sstreq(attr->ns->href, xmlSchemaNs)) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
			attr = attr->next;
		}
	}
	else {
		/*
		 * Parse as global simple type definition.
		 *
		 * Note that attrValue is the value of the attribute "name" here.
		 */
		type = xmlSchemaAddType(ctxt, schema, XML_SCHEMA_TYPE_SIMPLE,
		    attrValue, ctxt->targetNamespace, P_Node, 1);
		if(type == NULL)
			return 0;
		type->type = XML_SCHEMA_TYPE_SIMPLE;
		type->contentType = XML_SCHEMA_CONTENT_SIMPLE;
		type->flags |= XML_SCHEMAS_TYPE_GLOBAL;
		/*
		 * Check for illegal attributes.
		 */
		attr = P_Node->properties;
		while(attr) {
			if(attr->ns == NULL) {
				if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "name")) && (!sstreq(attr->name, "final"))) {
					xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
				}
			}
			else if(sstreq(attr->ns->href, xmlSchemaNs)) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
			attr = attr->next;
		}
		/*
		 * Attribute "final".
		 */
		attr = xmlSchemaGetPropNode(P_Node, "final");
		if(!attr) {
			if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_RESTRICTION)
				type->flags |= XML_SCHEMAS_TYPE_FINAL_RESTRICTION;
			if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_LIST)
				type->flags |= XML_SCHEMAS_TYPE_FINAL_LIST;
			if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_UNION)
				type->flags |= XML_SCHEMAS_TYPE_FINAL_UNION;
		}
		else {
			attrValue = xmlSchemaGetProp(ctxt, P_Node, "final");
			if(xmlSchemaPValAttrBlockFinal(attrValue, &(type->flags),
				    -1, -1, XML_SCHEMAS_TYPE_FINAL_RESTRICTION, -1,
				    XML_SCHEMAS_TYPE_FINAL_LIST,
				    XML_SCHEMAS_TYPE_FINAL_UNION) != 0) {
				xmlSchemaPSimpleTypeErr(ctxt,
				    XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
				    WXS_BASIC_CAST type, (xmlNode *)attr,
				    NULL, "(#all | List of (list | union | restriction)",
				    attrValue, NULL, 0, 0);
			}
		}
	}
	type->targetNamespace = ctxt->targetNamespace;
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	oldCtxtType = ctxt->ctxtType;

	ctxt->ctxtType = type;

	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		type->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	if(child == NULL) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_MISSING, NULL, P_Node, child, NULL, "(annotation?, (restriction | list | union))");
	}
	else if(IS_SCHEMA(child, "restriction")) {
		xmlSchemaParseRestriction(ctxt, schema, child,
		    XML_SCHEMA_TYPE_SIMPLE);
		hasRestriction = 1;
		child = child->next;
	}
	else if(IS_SCHEMA(child, "list")) {
		xmlSchemaParseList(ctxt, schema, child);
		child = child->next;
	}
	else if(IS_SCHEMA(child, "union")) {
		xmlSchemaParseUnion(ctxt, schema, child);
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (restriction | list | union))");
	/*
	 * REDEFINE: SPEC src-redefine (5)
	 * "Within the [children], each <simpleType> must have a
	 * <restriction> among its [children] ... the `actual value` of whose
	 * base [attribute] must be the same as the `actual value` of its own
	 * name attribute plus target namespace;"
	 */
	if(topLevel && ctxt->isRedefine && (!hasRestriction)) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_REDEFINE,
		    NULL, P_Node, "This is a redefinition, thus the "
		    "<simpleType> must have a <restriction> child", 0);
	}

	ctxt->ctxtType = oldCtxtType;
	return (type);
}

/**
 * xmlSchemaParseModelGroupDefRef:
 * @ctxt:  the parser context
 * @schema: the schema being built
 * @node:  the node
 *
 * Parses a reference to a model group definition.
 *
 * We will return a particle component with a qname-component or
 * NULL in case of an error.
 */
static xmlSchemaTreeItem * xmlSchemaParseModelGroupDefRef(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaParticlePtr item;
	xmlNode * child = NULL;
	xmlAttr * attr;
	const xmlChar * ref = NULL, * refNs = NULL;
	int min, max;

	if(!ctxt || !schema || !P_Node)
		return 0;

	attr = xmlSchemaGetPropNode(P_Node, "ref");
	if(!attr) {
		xmlSchemaPMissingAttrErr(ctxt,
		    XML_SCHEMAP_S4S_ATTR_MISSING,
		    NULL, P_Node, "ref", 0);
		return 0;
	}
	else if(xmlSchemaPValAttrNodeQName(ctxt, schema, NULL, attr, &refNs, &ref) != 0) {
		return 0;
	}
	xmlSchemaCheckReference(ctxt, schema, P_Node, attr, refNs);
	min = xmlGetMinOccurs(ctxt, P_Node, 0, -1, 1, "xs:nonNegativeInteger");
	max = xmlGetMaxOccurs(ctxt, P_Node, 0, UNBOUNDED, 1, "(xs:nonNegativeInteger | unbounded)");
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "ref")) && (!sstreq(attr->name, "id")) && (!sstreq(attr->name, "minOccurs")) && (!sstreq(attr->name, "maxOccurs"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	item = xmlSchemaAddParticle(ctxt, P_Node, min, max);
	if(item == NULL)
		return 0;
	/*
	 * Create a qname-reference and set as the term; it will be substituted
	 * for the model group after the reference has been resolved.
	 */
	item->children = (xmlSchemaTreeItem *)
	    xmlSchemaNewQNameRef(ctxt, XML_SCHEMA_TYPE_GROUP, ref, refNs);
	xmlSchemaPCheckParticleCorrect_2(ctxt, item, P_Node, min, max);
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	/* @todo Is annotation even allowed for a model group reference? */
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * @todo What to do exactly with the annotation?
		 */
		item->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
	/*
	 * Corresponds to no component at all if minOccurs==maxOccurs==0.
	 */
	if((min == 0) && (max == 0))
		return 0;
	return ((xmlSchemaTreeItem *)item);
}

/**
 * xmlSchemaParseModelGroupDefinition:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * Parses a XML schema model group definition.
 *
 * Note that the contraint src-redefine (6.2) can't be applied until
 * references have been resolved. So we will do this at the
 * component fixup level.
 *
 * *WARNING* this interface is highly subject to change
 *
 * Returns -1 in case of error, 0 if the declaration is improper and
 *         1 in case of success.
 */
static xmlSchemaModelGroupDefPtr xmlSchemaParseModelGroupDefinition(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlSchemaModelGroupDefPtr item;
	xmlNode * child = NULL;
	xmlAttr * attr;
	const xmlChar * name;

	if(!ctxt || !schema || !P_Node)
		return 0;

	attr = xmlSchemaGetPropNode(P_Node, "name");
	if(!attr) {
		xmlSchemaPMissingAttrErr(ctxt,
		    XML_SCHEMAP_S4S_ATTR_MISSING,
		    NULL, P_Node,
		    "name", 0);
		return 0;
	}
	else if(xmlSchemaPValAttrNode(ctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &name) != 0) {
		return 0;
	}
	item = xmlSchemaAddModelGroupDefinition(ctxt, schema, name, ctxt->targetNamespace, P_Node);
	if(item == NULL)
		return 0;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "name")) && (!sstreq(attr->name, "id"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		item->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	if(IS_SCHEMA(child, "all")) {
		item->children = xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_ALL, 0);
		child = child->next;
	}
	else if(IS_SCHEMA(child, "choice")) {
		item->children = xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_CHOICE, 0);
		child = child->next;
	}
	else if(IS_SCHEMA(child, "sequence")) {
		item->children = xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_SEQUENCE, 0);
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (all | choice | sequence)?)");
	return item;
}
/**
 * xmlSchemaCleanupDoc:
 * @ctxt:  a schema validation context
 * @node:  the root of the document.
 *
 * removes unwanted nodes in a schemas document tree
 */
static void xmlSchemaCleanupDoc(xmlSchemaParserCtxtPtr ctxt, xmlNode * root)
{
	if(ctxt && root) {
		/*
		 * Remove all the blank text nodes
		 */
		xmlNode * p_delete = NULL;
		xmlNode * cur = root;
		while(cur) {
			if(p_delete) {
				xmlUnlinkNode(p_delete);
				xmlFreeNode(p_delete);
				p_delete = NULL;
			}
			if(cur->type == XML_TEXT_NODE) {
				if(IS_BLANK_NODE(cur)) {
					if(xmlNodeGetSpacePreserve(cur) != 1) {
						p_delete = cur;
					}
				}
			}
			else if((cur->type != XML_ELEMENT_NODE) && (cur->type != XML_CDATA_SECTION_NODE)) {
				p_delete = cur;
				goto skip_children;
			}
			/*
			 * Skip to next node
			 */
			if(cur->children) {
				if((cur->children->type != XML_ENTITY_DECL) && (cur->children->type != XML_ENTITY_REF_NODE) && (cur->children->type != XML_ENTITY_NODE)) {
					cur = cur->children;
					continue;
				}
			}
	skip_children:
			if(cur->next) {
				cur = cur->next;
				continue;
			}
			do {
				cur = cur->parent;
				if(!cur)
					break;
				if(cur == root) {
					cur = NULL;
					break;
				}
				if(cur->next) {
					cur = cur->next;
					break;
				}
			} while(cur);
		}
		if(p_delete) {
			xmlUnlinkNode(p_delete);
			xmlFreeNode(p_delete);
			p_delete = NULL;
		}
	}
}

static void xmlSchemaClearSchemaDefaults(xmlSchemaPtr schema)
{
	if(schema->flags & XML_SCHEMAS_QUALIF_ELEM)
		schema->flags ^= XML_SCHEMAS_QUALIF_ELEM;
	if(schema->flags & XML_SCHEMAS_QUALIF_ATTR)
		schema->flags ^= XML_SCHEMAS_QUALIF_ATTR;
	if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_EXTENSION)
		schema->flags ^= XML_SCHEMAS_FINAL_DEFAULT_EXTENSION;
	if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_RESTRICTION)
		schema->flags ^= XML_SCHEMAS_FINAL_DEFAULT_RESTRICTION;
	if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_LIST)
		schema->flags ^= XML_SCHEMAS_FINAL_DEFAULT_LIST;
	if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_UNION)
		schema->flags ^= XML_SCHEMAS_FINAL_DEFAULT_UNION;
	if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_EXTENSION)
		schema->flags ^= XML_SCHEMAS_BLOCK_DEFAULT_EXTENSION;
	if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_RESTRICTION)
		schema->flags ^= XML_SCHEMAS_BLOCK_DEFAULT_RESTRICTION;
	if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_SUBSTITUTION)
		schema->flags ^= XML_SCHEMAS_BLOCK_DEFAULT_SUBSTITUTION;
}

static int xmlSchemaParseSchemaElement(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlAttr * attr;
	const xmlChar * val;
	int res = 0, oldErrs = ctxt->nberrors;
	/*
	 * Those flags should be moved to the parser context flags,
	 * since they are not visible at the component level. I.e.
	 * they are used if processing schema *documents* only.
	 */
	res = xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	HFAILURE;
	/*
	 * Since the version is of type xs:token, we won't bother to
	 * check it.
	 */
	/* REMOVED:
	   attr = xmlSchemaGetPropNode(node, "version");
	   if (attr != NULL) {
	    res = xmlSchemaPValAttrNode(ctxt, NULL, NULL, attr,
	        xmlSchemaGetBuiltInType(XML_SCHEMAS_TOKEN), &val);
	    HFAILURE;
	   }
	 */
	attr = xmlSchemaGetPropNode(P_Node, "targetNamespace");
	if(attr) {
		res = xmlSchemaPValAttrNode(ctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI), 0);
		HFAILURE;
		if(res != 0) {
			ctxt->stop = XML_SCHEMAP_S4S_ATTR_INVALID_VALUE;
			goto exit;
		}
	}
	attr = xmlSchemaGetPropNode(P_Node, "elementFormDefault");
	if(attr) {
		val = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
		res = xmlSchemaPValAttrFormDefault(val, &schema->flags, XML_SCHEMAS_QUALIF_ELEM);
		HFAILURE;
		if(res != 0) {
			xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_ELEMFORMDEFAULT_VALUE, NULL, (xmlNode *)attr, NULL, "(qualified | unqualified)", val, NULL, 0, 0);
		}
	}
	attr = xmlSchemaGetPropNode(P_Node, "attributeFormDefault");
	if(attr) {
		val = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
		res = xmlSchemaPValAttrFormDefault(val, &schema->flags, XML_SCHEMAS_QUALIF_ATTR);
		HFAILURE;
		if(res != 0) {
			xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_ATTRFORMDEFAULT_VALUE, NULL, (xmlNode *)attr, NULL, "(qualified | unqualified)", val, NULL, 0, 0);
		}
	}
	attr = xmlSchemaGetPropNode(P_Node, "finalDefault");
	if(attr) {
		val = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
		res = xmlSchemaPValAttrBlockFinal(val, &(schema->flags), -1, XML_SCHEMAS_FINAL_DEFAULT_EXTENSION,
		    XML_SCHEMAS_FINAL_DEFAULT_RESTRICTION, -1, XML_SCHEMAS_FINAL_DEFAULT_LIST, XML_SCHEMAS_FINAL_DEFAULT_UNION);
		HFAILURE;
		if(res != 0) {
			xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, NULL, (xmlNode *)attr, NULL,
			    "(#all | List of (extension | restriction | list | union))", val, NULL, 0, 0);
		}
	}
	attr = xmlSchemaGetPropNode(P_Node, "blockDefault");
	if(attr) {
		val = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
		res = xmlSchemaPValAttrBlockFinal(val, &(schema->flags), -1, XML_SCHEMAS_BLOCK_DEFAULT_EXTENSION,
		    XML_SCHEMAS_BLOCK_DEFAULT_RESTRICTION, XML_SCHEMAS_BLOCK_DEFAULT_SUBSTITUTION, -1, -1);
		HFAILURE;
		if(res != 0) {
			xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, NULL, (xmlNode *)attr, NULL,
			    "(#all | List of (extension | restriction | substitution))", val, NULL, 0, 0);
		}
	}
exit:
	if(oldErrs != ctxt->nberrors)
		res = ctxt->err;
	return res;
exit_failure:
	return -1;
}

/**
 * xmlSchemaParseSchemaTopLevel:
 * @ctxt:  a schema validation context
 * @schema:  the schemas
 * @nodes:  the list of top level nodes
 *
 * Returns the internal XML Schema structure built from the resource or
 *         NULL in case of error
 */
static int xmlSchemaParseSchemaTopLevel(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * nodes)
{
	xmlNode * child;
	xmlSchemaAnnotPtr annot;
	int res = 0, oldErrs, tmpOldErrs;

	if(!ctxt || (schema == NULL) || (nodes == NULL))
		return -1;

	oldErrs = ctxt->nberrors;
	child = nodes;
	while((IS_SCHEMA(child, "include")) || (IS_SCHEMA(child, "import")) || (IS_SCHEMA(child, "redefine")) || (IS_SCHEMA(child, "annotation"))) {
		if(IS_SCHEMA(child, "annotation")) {
			annot = xmlSchemaParseAnnotation(ctxt, child, 1);
			if(schema->annot == NULL)
				schema->annot = annot;
			else
				xmlSchemaFreeAnnot(annot);
		}
		else if(IS_SCHEMA(child, "import")) {
			tmpOldErrs = ctxt->nberrors;
			res = xmlSchemaParseImport(ctxt, schema, child);
			HFAILURE;
			HSTOP(ctxt);
			if(tmpOldErrs != ctxt->nberrors)
				goto exit;
		}
		else if(IS_SCHEMA(child, "include")) {
			tmpOldErrs = ctxt->nberrors;
			res = xmlSchemaParseInclude(ctxt, schema, child);
			HFAILURE;
			HSTOP(ctxt);
			if(tmpOldErrs != ctxt->nberrors)
				goto exit;
		}
		else if(IS_SCHEMA(child, "redefine")) {
			tmpOldErrs = ctxt->nberrors;
			res = xmlSchemaParseRedefine(ctxt, schema, child);
			HFAILURE;
			HSTOP(ctxt);
			if(tmpOldErrs != ctxt->nberrors)
				goto exit;
		}
		child = child->next;
	}
	/*
	 * URGENT TODO: Change the functions to return int results.
	 * We need especially to catch internal errors.
	 */
	while(child) {
		if(IS_SCHEMA(child, "complexType")) {
			xmlSchemaParseComplexType(ctxt, schema, child, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "simpleType")) {
			xmlSchemaParseSimpleType(ctxt, schema, child, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "element")) {
			xmlSchemaParseElement(ctxt, schema, child, NULL, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "attribute")) {
			xmlSchemaParseGlobalAttribute(ctxt, schema, child);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "attributeGroup")) {
			xmlSchemaParseAttributeGroupDefinition(ctxt, schema, child);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "group")) {
			xmlSchemaParseModelGroupDefinition(ctxt, schema, child);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "notation")) {
			xmlSchemaParseNotation(ctxt, schema, child);
			child = child->next;
		}
		else {
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, child->parent, child, NULL, 
				"((include | import | redefine | annotation)*, (((simpleType | complexType | group | attributeGroup) | element | attribute | notation), annotation*)*)");
			child = child->next;
		}
		while(IS_SCHEMA(child, "annotation")) {
			/*
			 * @todo We should add all annotations.
			 */
			annot = xmlSchemaParseAnnotation(ctxt, child, 1);
			if(schema->annot == NULL)
				schema->annot = annot;
			else
				xmlSchemaFreeAnnot(annot);
			child = child->next;
		}
	}
exit:
	ctxt->ctxtType = NULL;
	if(oldErrs != ctxt->nberrors)
		res = ctxt->err;
	return res;
exit_failure:
	return -1;
}

static xmlSchemaSchemaRelationPtr xmlSchemaSchemaRelationCreate()
{
	xmlSchemaSchemaRelationPtr ret;

	ret = (xmlSchemaSchemaRelationPtr)
	    SAlloc::M(sizeof(xmlSchemaSchemaRelation));
	if(!ret) {
		xmlSchemaPErrMemory(NULL, "allocating schema relation", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaSchemaRelation));
	return ret;
}

#if 0
static void xmlSchemaSchemaRelationFree(xmlSchemaSchemaRelationPtr rel)
{
	SAlloc::F(rel);
}

#endif

static void xmlSchemaRedefListFree(xmlSchemaRedefPtr redef)
{
	while(redef) {
		xmlSchemaRedefPtr prev = redef;
		redef = redef->next;
		SAlloc::F(prev);
	}
}

static void xmlSchemaConstructionCtxtFree(xmlSchemaConstructionCtxtPtr con)
{
	/*
	 * After the construction context has been freed, there will be
	 * no schema graph available any more. Only the schema buckets
	 * will stay alive, which are put into the "schemasImports" and
	 * "includes" slots of the xmlSchema.
	 */
	xmlSchemaItemListFree(con->buckets);
	xmlSchemaItemListFree(con->pending);
	xmlHashFree(con->substGroups, (xmlHashDeallocator)xmlSchemaSubstGroupFree);
	xmlSchemaRedefListFree(con->redefs);
	xmlDictFree(con->dict);
	SAlloc::F(con);
}

static xmlSchemaConstructionCtxtPtr xmlSchemaConstructionCtxtCreate(xmlDict * dict)
{
	xmlSchemaConstructionCtxt * ret = (xmlSchemaConstructionCtxt *)SAlloc::M(sizeof(xmlSchemaConstructionCtxt));
	if(!ret) {
		xmlSchemaPErrMemory(NULL, "allocating schema construction context", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaConstructionCtxt));
	ret->buckets = xmlSchemaItemListCreate();
	if(ret->buckets == NULL) {
		xmlSchemaPErrMemory(NULL, "allocating list of schema buckets", 0);
		SAlloc::F(ret);
		return 0;
	}
	ret->pending = xmlSchemaItemListCreate();
	if(ret->pending == NULL) {
		xmlSchemaPErrMemory(NULL, "allocating list of pending global components", 0);
		xmlSchemaConstructionCtxtFree(ret);
		return 0;
	}
	ret->dict = dict;
	xmlDictReference(dict);
	return ret;
}

static xmlSchemaParserCtxtPtr xmlSchemaParserCtxtCreate()
{
	xmlSchemaParserCtxtPtr ret = (xmlSchemaParserCtxtPtr)SAlloc::M(sizeof(xmlSchemaParserCtxt));
	if(!ret) {
		xmlSchemaPErrMemory(NULL, "allocating schema parser context", 0);
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaParserCtxt));
	ret->type = XML_SCHEMA_CTXT_PARSER;
	ret->attrProhibs = xmlSchemaItemListCreate();
	if(ret->attrProhibs == NULL) {
		SAlloc::F(ret);
		return 0;
	}
	return ret;
}

/**
 * xmlSchemaNewParserCtxtUseDict:
 * @URL:  the location of the schema
 * @dict: the dictionary to be used
 *
 * Create an XML Schemas parse context for that file/resource expected
 * to contain an XML Schemas file.
 *
 * Returns the parser context or NULL in case of error
 */
static xmlSchemaParserCtxtPtr xmlSchemaNewParserCtxtUseDict(const char * URL, xmlDict * dict)
{
	xmlSchemaParserCtxt * ret = xmlSchemaParserCtxtCreate();
	if(ret) {
		ret->dict = dict;
		xmlDictReference(dict);
		if(URL)
			ret->URL = xmlDictLookupSL(dict, (const xmlChar*)URL);
	}
	return ret;
}

static int xmlSchemaCreatePCtxtOnVCtxt(xmlSchemaValidCtxtPtr vctxt)
{
	if(vctxt->pctxt == NULL) {
		if(vctxt->schema)
			vctxt->pctxt = xmlSchemaNewParserCtxtUseDict("*", vctxt->schema->dict);
		else
			vctxt->pctxt = xmlSchemaNewParserCtxt("*");
		if(vctxt->pctxt == NULL) {
			VERROR_INT("xmlSchemaCreatePCtxtOnVCtxt", "failed to create a temp. parser context");
			return -1;
		}
		/* @todo Pass user data. */
		xmlSchemaSetParserErrors(vctxt->pctxt, vctxt->error, vctxt->warning, vctxt->errCtxt);
		xmlSchemaSetParserStructuredErrors(vctxt->pctxt, vctxt->serror, vctxt->errCtxt);
	}
	return 0;
}

/**
 * xmlSchemaGetSchemaBucket:
 * @pctxt: the schema parser context
 * @schemaLocation: the URI of the schema document
 *
 * Returns a schema bucket if it was already parsed.
 *
 * Returns a schema bucket if it was already parsed from
 *         @schemaLocation, NULL otherwise.
 */
static xmlSchemaBucketPtr xmlSchemaGetSchemaBucket(xmlSchemaParserCtxtPtr pctxt, const xmlChar * schemaLocation)
{
	xmlSchemaBucketPtr cur;
	xmlSchemaItemListPtr list = pctxt->constructor->buckets;
	if(list->nbItems == 0)
		return 0;
	else {
		for(int i = 0; i < list->nbItems; i++) {
			cur = (xmlSchemaBucketPtr)list->items[i];
			/* Pointer comparison! */
			if(cur->schemaLocation == schemaLocation)
				return cur;
		}
	}
	return 0;
}

static xmlSchemaBucketPtr xmlSchemaGetChameleonSchemaBucket(xmlSchemaParserCtxtPtr pctxt, const xmlChar * schemaLocation, const xmlChar * targetNamespace)
{
	xmlSchemaBucketPtr cur;
	xmlSchemaItemListPtr list = pctxt->constructor->buckets;
	if(list->nbItems == 0)
		return 0;
	else {
		for(int i = 0; i < list->nbItems; i++) {
			cur = (xmlSchemaBucketPtr)list->items[i];
			/* Pointer comparison! */
			if((cur->origTargetNamespace == NULL) && (cur->schemaLocation == schemaLocation) && (cur->targetNamespace == targetNamespace))
				return cur;
		}
	}
	return 0;
}

#define IS_BAD_SCHEMA_DOC(b) (((b)->doc == NULL) && ((b)->schemaLocation != NULL))

static xmlSchemaBucketPtr xmlSchemaGetSchemaBucketByTNS(xmlSchemaParserCtxtPtr pctxt, const xmlChar * targetNamespace, int imported)
{
	xmlSchemaBucketPtr cur;
	xmlSchemaItemListPtr list = pctxt->constructor->buckets;
	if(list->nbItems == 0)
		return 0;
	else {
		for(int i = 0; i < list->nbItems; i++) {
			cur = (xmlSchemaBucketPtr)list->items[i];
			if((!IS_BAD_SCHEMA_DOC(cur)) && (cur->origTargetNamespace == targetNamespace) && ((imported && cur->imported) || ((!imported) && (!cur->imported))))
				return cur;
		}
	}
	return 0;
}

static int xmlSchemaParseNewDocWithContext(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlSchemaBucketPtr bucket)
{
	int oldFlags;
	xmlDocPtr oldDoc;
	xmlNode * P_Node;
	int ret, oldErrs;
	xmlSchemaBucketPtr oldbucket = pctxt->constructor->bucket;
	/*
	 * Save old values; reset the *main* schema.
	 * URGENT TODO: This is not good; move the per-document information
	 * to the parser. Get rid of passing the main schema to the
	 * parsing functions.
	 */
	oldFlags = schema->flags;
	oldDoc = schema->doc;
	if(schema->flags != 0)
		xmlSchemaClearSchemaDefaults(schema);
	schema->doc = bucket->doc;
	pctxt->schema = schema;
	/*
	 * Keep the current target namespace on the parser *not* on the
	 * main schema.
	 */
	pctxt->targetNamespace = bucket->targetNamespace;
	WXS_CONSTRUCTOR(pctxt)->bucket = bucket;
	if(bucket->targetNamespace && sstreq(bucket->targetNamespace, xmlSchemaNs)) {
		// We are parsing the schema for schemas!
		pctxt->isS4S = 1;
	}
	/* Mark it as parsed, even if parsing fails. */
	bucket->parsed++;
	/* Compile the schema doc. */
	P_Node = xmlDocGetRootElement(bucket->doc);
	ret = xmlSchemaParseSchemaElement(pctxt, schema, P_Node);
	if(ret != 0)
		goto exit;
	/* An empty schema; just get out. */
	if(P_Node->children == NULL)
		goto exit;
	oldErrs = pctxt->nberrors;
	ret = xmlSchemaParseSchemaTopLevel(pctxt, schema, P_Node->children);
	if(ret != 0)
		goto exit;
	/*
	 * @todo Not nice, but I'm not 100% sure we will get always an error
	 * as a result of the obove functions; so better rely on pctxt->err
	 * as well.
	 */
	if((ret == 0) && (oldErrs != pctxt->nberrors)) {
		ret = pctxt->err;
		goto exit;
	}

exit:
	WXS_CONSTRUCTOR(pctxt)->bucket = oldbucket;
	/* Restore schema values. */
	schema->doc = oldDoc;
	schema->flags = oldFlags;
	return ret;
}

static int xmlSchemaParseNewDoc(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlSchemaBucketPtr bucket)
{
	xmlSchemaParserCtxtPtr newpctxt;
	int res = 0;
	if(bucket == NULL)
		return 0;
	if(bucket->parsed) {
		PERROR_INT("xmlSchemaParseNewDoc", "reparsing a schema doc");
		return -1;
	}
	if(bucket->doc == NULL) {
		PERROR_INT("xmlSchemaParseNewDoc", "parsing a schema doc, but there's no doc");
		return -1;
	}
	if(pctxt->constructor == NULL) {
		PERROR_INT("xmlSchemaParseNewDoc", "no constructor");
		return -1;
	}
	/* Create and init the temporary parser context. */
	newpctxt = xmlSchemaNewParserCtxtUseDict((const char*)bucket->schemaLocation, pctxt->dict);
	if(newpctxt == NULL)
		return -1;
	newpctxt->constructor = pctxt->constructor;
	/*
	 * @todo Can we avoid that the parser knows about the main schema?
	 * It would be better if he knows about the current schema bucket only.
	 */
	newpctxt->schema = schema;
	xmlSchemaSetParserErrors(newpctxt, pctxt->F_Error, pctxt->F_Warning, pctxt->errCtxt);
	xmlSchemaSetParserStructuredErrors(newpctxt, pctxt->serror, pctxt->errCtxt);
	newpctxt->counter = pctxt->counter;
	res = xmlSchemaParseNewDocWithContext(newpctxt, schema, bucket);
	/* Channel back errors and cleanup the temporary parser context. */
	if(res != 0)
		pctxt->err = res;
	pctxt->nberrors += newpctxt->nberrors;
	pctxt->counter = newpctxt->counter;
	newpctxt->constructor = NULL;
	// Free the parser context. 
	xmlSchemaFreeParserCtxt(newpctxt);
	return res;
}

static void xmlSchemaSchemaRelationAddChild(xmlSchemaBucketPtr bucket, xmlSchemaSchemaRelationPtr rel)
{
	xmlSchemaSchemaRelation * cur = bucket->relations;
	if(!cur) {
		bucket->relations = rel;
	}
	else {
		while(cur->next)
			cur = cur->next;
		cur->next = rel;
	}
}

static const xmlChar * xmlSchemaBuildAbsoluteURI(xmlDict * dict, const xmlChar* location, xmlNode * ctxtNode)
{
	/*
	 * Build an absolue location URI.
	 */
	if(location) {
		if(ctxtNode == NULL)
			return location;
		else {
			xmlChar * URI;
			const xmlChar * ret = NULL;
			xmlChar * base = xmlNodeGetBase(ctxtNode->doc, ctxtNode);
			if(base == NULL) {
				URI = xmlBuildURI(location, ctxtNode->doc->URL);
			}
			else {
				URI = xmlBuildURI(location, base);
				SAlloc::F(base);
			}
			if(URI) {
				ret = xmlDictLookupSL(dict, URI);
				SAlloc::F(URI);
				return ret;
			}
		}
	}
	return 0;
}

/**
 * xmlSchemaAddSchemaDoc:
 * @pctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * Parse an included (and to-be-redefined) XML schema document.
 *
 * Returns 0 on success, a positive error code on errors and
 *         -1 in case of an internal or API error.
 */

static int xmlSchemaAddSchemaDoc(xmlSchemaParserCtxtPtr pctxt, int type/* import or include or redefine */,
    const xmlChar * schemaLocation, xmlDocPtr schemaDoc, const char * schemaBuffer, int schemaBufferLen, xmlNode * invokingNode,
    const xmlChar * sourceTargetNamespace, const xmlChar * importNamespace, xmlSchemaBucketPtr * bucket)
{
	const xmlChar * targetNamespace = NULL;
	xmlSchemaSchemaRelationPtr relation = NULL;
	xmlDocPtr doc = NULL;
	int res = 0, err = 0, located = 0, preserveDoc = 0;
	xmlSchemaBucket * bkt = NULL;
	ASSIGN_PTR(bucket, NULL);
	switch(type) {
		case XML_SCHEMA_SCHEMA_IMPORT:
		case XML_SCHEMA_SCHEMA_MAIN: err = XML_SCHEMAP_SRC_IMPORT; break;
		case XML_SCHEMA_SCHEMA_INCLUDE: err = XML_SCHEMAP_SRC_INCLUDE; break;
		case XML_SCHEMA_SCHEMA_REDEFINE: err = XML_SCHEMAP_SRC_REDEFINE; break;
	}
	/* Special handling for the main schema:
	 * skip the location and relation logic and just parse the doc.
	 * We need just a bucket to be returned in this case.
	 */
	if((type == XML_SCHEMA_SCHEMA_MAIN) || (!WXS_HAS_BUCKETS(pctxt)))
		goto doc_load;
	// Note that we expect the location to be an absulute URI. 
	if(schemaLocation) {
		bkt = xmlSchemaGetSchemaBucket(pctxt, schemaLocation);
		if(bkt && (pctxt->constructor->bucket == bkt)) {
			// Report self-imports/inclusions/redefinitions. 
			xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)err, invokingNode, NULL, "The schema must not import/include/redefine itself", 0, 0);
			goto exit;
		}
	}
	// 
	// Create a relation for the graph of schemas.
	// 
	relation = xmlSchemaSchemaRelationCreate();
	if(!relation)
		return -1;
	xmlSchemaSchemaRelationAddChild(pctxt->constructor->bucket, relation);
	relation->type = type;
	// 
	// Save the namespace import information.
	// 
	if(WXS_IS_BUCKET_IMPMAIN(type)) {
		relation->importNamespace = importNamespace;
		if(!schemaLocation) {
			// 
			// No location; this is just an import of the namespace.
			// Note that we don't assign a bucket to the relation in this case.
			// 
			goto exit;
		}
		targetNamespace = importNamespace;
	}
	// Did we already fetch the doc? 
	if(bkt) {
		if((WXS_IS_BUCKET_IMPMAIN(type)) && (!bkt->imported)) {
			// 
			// We included/redefined and then try to import a schema,
			// but the new location provided for import was different.
			// 
			SETIFZ(schemaLocation, BAD_CAST "in_memory_buffer");
			if(!sstreq(schemaLocation, bkt->schemaLocation)) {
				xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)err,
				    invokingNode, NULL, "The schema document '%s' cannot be imported, since it was already included or redefined",
				    schemaLocation, 0);
				goto exit;
			}
		}
		else if((!WXS_IS_BUCKET_IMPMAIN(type)) && (bkt->imported)) {
			// 
			// We imported and then try to include/redefine a schema,
			// but the new location provided for the include/redefine was different.
			// 
			SETIFZ(schemaLocation, BAD_CAST "in_memory_buffer");
			if(!sstreq(schemaLocation, bkt->schemaLocation)) {
				xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)err, invokingNode, NULL,
				    "The schema document '%s' cannot be included or redefined, since it was already imported", schemaLocation, 0);
				goto exit;
			}
		}
	}
	if(WXS_IS_BUCKET_IMPMAIN(type)) {
		/*
		 * Given that the schemaLocation [attribute] is only a hint, it is open
		 * to applications to ignore all but the first <import> for a given
		 * namespace, regardless of the `actual value` of schemaLocation, but
		 * such a strategy risks missing useful information when new
		 * schemaLocations are offered.
		 *
		 * We will use the first <import> that comes with a location.
		 * Further <import>s *with* a location, will result in an error.
		 * @todo Better would be to just report a warning here, but
		 * we'll try it this way until someone complains.
		 *
		 * Schema Document Location Strategy:
		 * 3 Based on the namespace name, identify an existing schema document,
		 * either as a resource which is an XML document or a <schema> element
		 * information item, in some local schema repository;
		 * 5 Attempt to resolve the namespace name to locate such a resource.
		 *
		 * NOTE: (3) and (5) are not supported.
		 */
		if(bkt) {
			relation->bucket = bkt;
			goto exit;
		}
		bkt = xmlSchemaGetSchemaBucketByTNS(pctxt, importNamespace, 1);
		if(bkt) {
			relation->bucket = bkt;
			if(bkt->schemaLocation == NULL) {
				bkt->schemaLocation = schemaLocation; // First given location of the schema; load the doc
			}
			else {
				if(!sstreq(schemaLocation, bkt->schemaLocation)) {
					// 
					// Additional location given; just skip it.
					// URGENT TODO: We should report a warning here.
					// res = XML_SCHEMAP_SRC_IMPORT;
					// 
					SETIFZ(schemaLocation, BAD_CAST "in_memory_buffer");
					xmlSchemaCustomWarning(ACTXT_CAST pctxt, XML_SCHEMAP_WARN_SKIP_SCHEMA, invokingNode, NULL,
					    "Skipping import of schema located at '%s' for the namespace '%s', since this namespace was already imported with the schema located at '%s'",
					    schemaLocation, importNamespace, bkt->schemaLocation);
				}
				goto exit;
			}
		}
		// 
		// No bucket + first location: load the doc and create a bucket.
		// 
	}
	else {
		// <include> and <redefine> 
		if(bkt) {
			if(!bkt->origTargetNamespace && (bkt->targetNamespace != sourceTargetNamespace)) {
				/*
				 * Chameleon include/redefine: skip loading only if it was
				 * aleady build for the targetNamespace of the including schema.
				 */
				/*
				 * URGENT TODO: If the schema is a chameleon-include then copy
				 * the components into the including schema and modify the
				 * targetNamespace of those components, do nothing otherwise.
				 * NOTE: This is currently worked-around by compiling the
				 * chameleon for every destinct including targetNamespace; thus
				 * not performant at the moment.
				 * @todo Check when the namespace in wildcards for chameleons
				 * needs to be converted: before we built wildcard intersections
				 * or after.
				 *   Answer: after!
				 */
				xmlSchemaBucket * chamel = xmlSchemaGetChameleonSchemaBucket(pctxt, schemaLocation, sourceTargetNamespace);
				if(chamel) {
					// A fitting chameleon was already parsed; NOP
					relation->bucket = chamel;
					goto exit;
				}
				/*
				 * We need to parse the chameleon again for a different targetNamespace.
				 * CHAMELEON TODO: Optimize this by only parsing the
				 * chameleon once, and then copying the components to the new targetNamespace.
				 */
				bkt = NULL;
			}
			else {
				relation->bucket = bkt;
				goto exit;
			}
		}
	}
	if(bkt && bkt->doc) {
		PERROR_INT("xmlSchemaAddSchemaDoc", "trying to load a schema doc, but a doc is already assigned to the schema bucket");
		goto exit_failure;
	}
doc_load:
	// 
	// Load the document.
	// 
	if(schemaDoc) {
		doc = schemaDoc;
		// Don' free this one, since it was provided by the caller. 
		preserveDoc = 1;
		// @todo Does the context or the doc hold the location? 
		schemaLocation = schemaDoc->URL ? xmlDictLookupSL(pctxt->dict, schemaDoc->URL) : BAD_CAST "in_memory_buffer";
	}
	else if(schemaLocation || schemaBuffer) {
		xmlParserCtxt * parserCtxt = xmlNewParserCtxt();
		if(!parserCtxt) {
			xmlSchemaPErrMemory(NULL, "xmlSchemaGetDoc, allocating a parser context", 0);
			goto exit_failure;
		}
		if(pctxt->dict && parserCtxt->dict) {
			// 
			// @todo Do we have to burden the schema parser dict with all the content of the schema doc?
			// 
			xmlDictFree(parserCtxt->dict);
			parserCtxt->dict = pctxt->dict;
			xmlDictReference(parserCtxt->dict);
		}
		if(schemaLocation) {
			// Parse from file
			doc = xmlCtxtReadFile(parserCtxt, (const char*)schemaLocation, NULL, SCHEMAS_PARSE_OPTIONS);
		}
		else if(schemaBuffer) {
			// Parse from memory buffer
			doc = xmlCtxtReadMemory(parserCtxt, schemaBuffer, schemaBufferLen, NULL, NULL, SCHEMAS_PARSE_OPTIONS);
			schemaLocation = BAD_CAST "in_memory_buffer";
			if(doc)
				doc->URL = sstrdup(schemaLocation);
		}
		// 
		// For <import>:
		// 2.1 The referent is (a fragment of) a resource which is an
		// XML document (see clause 1.1), which in turn corresponds to
		// a <schema> element information item in a well-formed information
		// set, which in turn corresponds to a valid schema.
		// @todo (2.1) fragments of XML documents are not supported.
		// 
		// 2.2 The referent is a <schema> element information item in
		// a well-formed information set, which in turn corresponds to a valid schema.
		// @todo (2.2) is not supported.
		// 
		if(!doc) {
			xmlErrorPtr lerr = xmlGetLastError();
			// 
			// Check if this a parser error, or if the document could just not be located.
			// @todo Try to find specific error codes to react only on localisation failures.
			// 
			if(!lerr || lerr->domain != XML_FROM_IO) {
				/*
				 * We assume a parser error here.
				 */
				located = 1;
				/* @todo Error code ?? */
				res = XML_SCHEMAP_SRC_IMPORT_2_1;
				xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)res, invokingNode, NULL,
				    "Failed to parse the XML resource '%s'", schemaLocation, 0);
			}
		}
		xmlFreeParserCtxt(parserCtxt);
		if(!doc && located)
			goto exit_error;
	}
	else {
		xmlSchemaPErr(pctxt, NULL, XML_SCHEMAP_NOTHING_TO_PARSE,
		    "No information for parsing was provided with the given schema parser context.\n", NULL, 0);
		goto exit_failure;
	}
	/*
	 * Preprocess the document.
	 */
	if(doc) {
		xmlNode * docElem = NULL;
		located = 1;
		docElem = xmlDocGetRootElement(doc);
		if(docElem == NULL) {
			xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_NOROOT, invokingNode, NULL,
			    "The document '%s' has no document element", schemaLocation, 0);
			goto exit_error;
		}
		/*
		 * Remove all the blank text nodes.
		 */
		xmlSchemaCleanupDoc(pctxt, docElem);
		/*
		 * Check the schema's top level element.
		 */
		if(!IS_SCHEMA(docElem, "schema")) {
			xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_NOT_SCHEMA, invokingNode, NULL,
			    "The XML document '%s' is not a schema document", schemaLocation, 0);
			goto exit_error;
		}
		// Note that we don't apply a type check for the targetNamespace value here.
		targetNamespace = xmlSchemaGetProp(pctxt, docElem, "targetNamespace");
	}
	// after_doc_loading: 
	if(!bkt && located) {
		// Only create a bucket if the schema was located. 
		bkt = xmlSchemaBucketCreate(pctxt, type, targetNamespace);
		if(!bkt)
			goto exit_failure;
	}
	if(bkt) {
		bkt->schemaLocation = schemaLocation;
		bkt->located = located;
		if(doc) {
			bkt->doc = doc;
			bkt->targetNamespace = targetNamespace;
			bkt->origTargetNamespace = targetNamespace;
			if(preserveDoc)
				bkt->preserveDoc = 1;
		}
		if(WXS_IS_BUCKET_IMPMAIN(type))
			bkt->imported++;
		// Add it to the graph of schemas.
		if(relation)
			relation->bucket = bkt;
	}
exit:
	//
	// Return the bucket explicitely; this is needed for the main schema.
	//
	ASSIGN_PTR(bucket, bkt);
	return 0;
exit_error:
	if(doc && (!preserveDoc)) {
		xmlFreeDoc(doc);
		if(bkt)
			bkt->doc = NULL;
	}
	return pctxt->err;
exit_failure:
	if(doc && (!preserveDoc)) {
		xmlFreeDoc(doc);
		if(bkt)
			bkt->doc = NULL;
	}
	return -1;
}
/**
 * xmlSchemaParseImport:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Import definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns 0 in case of success, a positive error code if
 * not valid and -1 in case of an internal error.
 */
static int xmlSchemaParseImport(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	xmlNode * child;
	const xmlChar * namespaceName = NULL, * schemaLocation = NULL;
	const xmlChar * thisTargetNamespace;
	xmlAttr * attr;
	int ret = 0;
	xmlSchemaBucketPtr bucket = NULL;
	if(!pctxt || !schema || !P_Node)
		return -1;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "namespace")) && (!sstreq(attr->name, "schemaLocation"))) {
				xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	/*
	 * Extract and validate attributes.
	 */
	if(xmlSchemaPValAttr(pctxt, NULL, P_Node, "namespace", xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI), &namespaceName) != 0) {
		xmlSchemaPSimpleTypeErr(pctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, NULL, P_Node,
		    xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI), NULL, namespaceName, NULL, 0, 0);
		return (pctxt->err);
	}
	if(xmlSchemaPValAttr(pctxt, NULL, P_Node, "schemaLocation", xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI), &schemaLocation) != 0) {
		xmlSchemaPSimpleTypeErr(pctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE,
		    NULL, P_Node, xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI), NULL, schemaLocation, NULL, 0, 0);
		return (pctxt->err);
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * the annotation here is simply discarded ...
		 * @todo really?
		 */
		child = child->next;
	}
	if(child) {
		xmlSchemaPContentErr(pctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?)");
	}
	/*
	 * Apply additional constraints.
	 *
	 * Note that it is important to use the original @targetNamespace
	 * (or none at all), to rule out imports of schemas _with_ a
	 * @targetNamespace if the importing schema is a chameleon schema
	 * (with no @targetNamespace).
	 */
	thisTargetNamespace = WXS_BUCKET(pctxt)->origTargetNamespace;
	if(namespaceName) {
		/*
		 * 1.1 If the namespace [attribute] is present, then its `actual value`
		 * must not match the `actual value` of the enclosing <schema>'s
		 * targetNamespace [attribute].
		 */
		if(sstreq(thisTargetNamespace, namespaceName)) {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_SRC_IMPORT_1_1, NULL, P_Node,
			    "The value of the attribute 'namespace' must not match the target namespace '%s' of the importing schema", thisTargetNamespace);
			return (pctxt->err);
		}
	}
	else {
		/*
		 * 1.2 If the namespace [attribute] is not present, then the enclosing
		 * <schema> must have a targetNamespace [attribute].
		 */
		if(!thisTargetNamespace) {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_SRC_IMPORT_1_2, NULL, P_Node,
			    "The attribute 'namespace' must be existent if the importing schema has no target namespace", NULL);
			return pctxt->err;
		}
	}
	/*
	 * Locate and acquire the schema document.
	 */
	if(schemaLocation)
		schemaLocation = xmlSchemaBuildAbsoluteURI(pctxt->dict, schemaLocation, P_Node);
	ret = xmlSchemaAddSchemaDoc(pctxt, XML_SCHEMA_SCHEMA_IMPORT, schemaLocation, NULL, NULL, 0, P_Node, thisTargetNamespace, namespaceName, &bucket);
	if(ret == 0) {
		// 
		// For <import>: "It is *not* an error for the application
		// schema reference strategy to fail."
		// So just don't parse if no schema document was found.
		// Note that we will get no bucket if the schema could not be
		// located or if there was no schemaLocation.
		// 
		if(!bucket && schemaLocation) {
			xmlSchemaCustomWarning(ACTXT_CAST pctxt, XML_SCHEMAP_WARN_UNLOCATED_SCHEMA, P_Node, NULL,
				"Failed to locate a schema at location '%s'. Skipping the import", schemaLocation, 0, 0);
		}
		if(bucket && CAN_PARSE_SCHEMA(bucket))
			ret = xmlSchemaParseNewDoc(pctxt, schema, bucket);
	}
	return ret;
}

static int xmlSchemaParseIncludeOrRedefineAttrs(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node,
    xmlChar ** schemaLocation, int type)
{
	xmlAttr * attr;
	if(!pctxt || !schema || !P_Node || !schemaLocation)
		return -1;
	*schemaLocation = NULL;
	/*
	 * Check for illegal attributes.
	 * Applies for both <include> and <redefine>.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(!attr->ns) {
			if(!sstreq(attr->name, "id") && !sstreq(attr->name, "schemaLocation")) {
				xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(pctxt, P_Node, BAD_CAST "id");
	/*
	 * Preliminary step, extract the URI-Reference and make an URI
	 * from the base.
	 */
	/*
	 * Attribute "schemaLocation" is mandatory.
	 */
	attr = xmlSchemaGetPropNode(P_Node, "schemaLocation");
	if(attr) {
		xmlChar * base = NULL;
		xmlChar * uri = NULL;
		if(xmlSchemaPValAttrNode(pctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYURI), (const xmlChar**)schemaLocation) != 0)
			goto exit_error;
		base = xmlNodeGetBase(P_Node->doc, P_Node);
		if(base == NULL) {
			uri = xmlBuildURI(*schemaLocation, P_Node->doc->URL);
		}
		else {
			uri = xmlBuildURI(*schemaLocation, base);
			SAlloc::F(base);
		}
		if(uri == NULL) {
			PERROR_INT("xmlSchemaParseIncludeOrRedefine", "could not build an URI from the schemaLocation")
			goto exit_failure;
		}
		(*schemaLocation) = (xmlChar*)xmlDictLookupSL(pctxt->dict, uri);
		SAlloc::F(uri);
	}
	else {
		xmlSchemaPMissingAttrErr(pctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "schemaLocation", 0);
		goto exit_error;
	}
	/*
	 * Report self-inclusion and self-redefinition.
	 */
	if(sstreq(*schemaLocation, pctxt->URL)) {
		if(type == XML_SCHEMA_SCHEMA_REDEFINE) {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_SRC_REDEFINE, NULL, P_Node, "The schema document '%s' cannot redefine itself.", *schemaLocation);
		}
		else {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_SRC_INCLUDE, NULL, P_Node, "The schema document '%s' cannot include itself.", *schemaLocation);
		}
		goto exit_error;
	}
	return 0;
exit_error:
	return(pctxt->err);
exit_failure:
	return -1;
}

static int xmlSchemaParseIncludeOrRedefine(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node, int type)
{
	xmlNode * child = NULL;
	const xmlChar * schemaLocation = NULL;
	int res = 0; /* hasRedefinitions = 0 */
	int isChameleon = 0, wasChameleon = 0;
	xmlSchemaBucketPtr bucket = NULL;
	if(!pctxt || !schema || !P_Node)
		return -1;
	/*
	 * Parse attributes. Note that the returned schemaLocation will
	 * be already converted to an absolute URI.
	 */
	res = xmlSchemaParseIncludeOrRedefineAttrs(pctxt, schema, P_Node, (xmlChar**)(&schemaLocation), type);
	if(res != 0)
		return res;
	/*
	 * Load and add the schema document.
	 */
	res = xmlSchemaAddSchemaDoc(pctxt, type, schemaLocation, NULL, NULL, 0, P_Node, pctxt->targetNamespace, NULL, &bucket);
	if(res != 0)
		return res;
	/*
	 * If we get no schema bucket back, then this means that the schema
	 * document could not be located or was broken XML or was not
	 * a schema document.
	 */
	if((bucket == NULL) || (bucket->doc == NULL)) {
		if(type == XML_SCHEMA_SCHEMA_INCLUDE) {
			/*
			 * WARNING for <include>:
			 * We will raise an error if the schema cannot be located
			 * for inclusions, since the that was the feedback from the
			 * schema people. I.e. the following spec piece will *not* be
			 * satisfied:
			 * SPEC src-include: "It is not an error for the `actual value` of the
			 * schemaLocation [attribute] to fail to resolve it all, in which
			 * case no corresponding inclusion is performed.
			 * So do we need a warning report here?"
			 */
			res = XML_SCHEMAP_SRC_INCLUDE;
			xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)res, P_Node, NULL, "Failed to load the document '%s' for inclusion", schemaLocation, 0);
		}
		else {
			/*
			 * NOTE: This was changed to raise an error even if no redefinitions
			 * are specified.
			 *
			 * SPEC src-redefine (1)
			 * "If there are any element information items among the [children]
			 * other than <annotation> then the `actual value` of the
			 * schemaLocation [attribute] must successfully resolve."
			 * @todo Ask the WG if a the location has always to resolve
			 * here as well!
			 */
			res = XML_SCHEMAP_SRC_REDEFINE;
			xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)res, P_Node, NULL, "Failed to load the document '%s' for redefinition", schemaLocation, 0);
		}
	}
	else {
		/*
		 * Check targetNamespace sanity before parsing the new schema.
		 * @todo Note that we won't check further content if the
		 * targetNamespace was bad.
		 */
		if(bucket->origTargetNamespace) {
			/*
			 * SPEC src-include (2.1)
			 * "SII has a targetNamespace [attribute], and its `actual
			 * value` is identical to the `actual value` of the targetNamespace
			 * [attribute] of SII' (which must have such an [attribute])."
			 */
			if(pctxt->targetNamespace == NULL) {
				xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_SRC_INCLUDE, P_Node, NULL,
				    "The target namespace of the included/redefined schema '%s' has to be absent, since the including/redefining schema has no target namespace",
				    schemaLocation, 0);
				goto exit_error;
			}
			else if(!sstreq(bucket->origTargetNamespace, pctxt->targetNamespace)) {
				/* @todo Change error function. */
				xmlSchemaPCustomErrExt(pctxt, XML_SCHEMAP_SRC_INCLUDE, NULL, P_Node,
				    "The target namespace '%s' of the included/redefined schema '%s' differs from '%s' of the including/redefining schema",
				    bucket->origTargetNamespace, schemaLocation, pctxt->targetNamespace);
				goto exit_error;
			}
		}
		else if(pctxt->targetNamespace) {
			/*
			 * Chameleons: the original target namespace will
			 * differ from the resulting namespace.
			 */
			isChameleon = 1;
			if(bucket->parsed && bucket->origTargetNamespace) {
				xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_SRC_INCLUDE, P_Node, NULL,
				    "The target namespace of the included/redefined schema '%s' has to be absent or the same as the including/redefining schema's target namespace",
				    schemaLocation, 0);
				goto exit_error;
			}
			bucket->targetNamespace = pctxt->targetNamespace;
		}
	}
	/*
	 * Parse the schema.
	 */
	if(bucket && (!bucket->parsed) && bucket->doc) {
		if(isChameleon) {
			/* @todo Get rid of this flag on the schema itself. */
			if((schema->flags & XML_SCHEMAS_INCLUDING_CONVERT_NS) == 0) {
				schema->flags |= XML_SCHEMAS_INCLUDING_CONVERT_NS;
			}
			else
				wasChameleon = 1;
		}
		xmlSchemaParseNewDoc(pctxt, schema, bucket);
		/* Restore chameleon flag. */
		if(isChameleon && (!wasChameleon))
			schema->flags ^= XML_SCHEMAS_INCLUDING_CONVERT_NS;
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(type == XML_SCHEMA_SCHEMA_REDEFINE) {
		/*
		 * Parse (simpleType | complexType | group | attributeGroup))*
		 */
		pctxt->redefined = bucket;
		/*
		 * How to proceed if the redefined schema was not located?
		 */
		pctxt->isRedefine = 1;
		while(IS_SCHEMA(child, "annotation") ||
		    IS_SCHEMA(child, "simpleType") ||
		    IS_SCHEMA(child, "complexType") ||
		    IS_SCHEMA(child, "group") ||
		    IS_SCHEMA(child, "attributeGroup")) {
			if(IS_SCHEMA(child, "annotation")) {
				/*
				 * @todo discard or not?
				 */
			}
			else if(IS_SCHEMA(child, "simpleType")) {
				xmlSchemaParseSimpleType(pctxt, schema, child, 1);
			}
			else if(IS_SCHEMA(child, "complexType")) {
				xmlSchemaParseComplexType(pctxt, schema, child, 1);
				/* hasRedefinitions = 1; */
			}
			else if(IS_SCHEMA(child, "group")) {
				/* hasRedefinitions = 1; */
				xmlSchemaParseModelGroupDefinition(pctxt,
				    schema, child);
			}
			else if(IS_SCHEMA(child, "attributeGroup")) {
				/* hasRedefinitions = 1; */
				xmlSchemaParseAttributeGroupDefinition(pctxt, schema,
				    child);
			}
			child = child->next;
		}
		pctxt->redefined = NULL;
		pctxt->isRedefine = 0;
	}
	else {
		if(IS_SCHEMA(child, "annotation")) {
			/*
			 * @todo discard or not?
			 */
			child = child->next;
		}
	}
	if(child) {
		res = XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED;
		if(type == XML_SCHEMA_SCHEMA_REDEFINE) {
			xmlSchemaPContentErr(pctxt, (xmlParserErrors)res, NULL, P_Node, child, NULL,
			    "(annotation | (simpleType | complexType | group | attributeGroup))*");
		}
		else {
			xmlSchemaPContentErr(pctxt, (xmlParserErrors)res, NULL, P_Node, child, NULL, "(annotation?)");
		}
	}
	return res;

exit_error:
	return(pctxt->err);
}

static int xmlSchemaParseRedefine(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
#ifndef ENABLE_REDEFINE
	TODO
	return 0;
#endif
	return xmlSchemaParseIncludeOrRedefine(pctxt, schema, P_Node, XML_SCHEMA_SCHEMA_REDEFINE);
}

static int xmlSchemaParseInclude(xmlSchemaParserCtxtPtr pctxt, xmlSchemaPtr schema, xmlNode * P_Node)
{
	return xmlSchemaParseIncludeOrRedefine(pctxt, schema, P_Node, XML_SCHEMA_SCHEMA_INCLUDE);
}
/**
 * xmlSchemaParseModelGroup:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 * @type: the "compositor" type
 * @particleNeeded: if a a model group with a particle
 *
 * parse a XML schema Sequence definition.
 * Applies parts of:
 *   Schema Representation Constraint:
 *     Redefinition Constraints and Semantics (src-redefine)
 *     (6.1), (6.1.1), (6.1.2)
 *
 *   Schema Component Constraint:
 *     All Group Limited (cos-all-limited) (2)
 *     TODO: Actually this should go to component-level checks,
 *     but is done here due to performance. Move it to an other layer
 *     is schema construction via an API is implemented.
 *
 * *WARNING* this interface is highly subject to change
 *
 * Returns -1 in case of error, 0 if the declaration is improper and
 *         1 in case of success.
 */
static xmlSchemaTreeItem * xmlSchemaParseModelGroup(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, xmlSchemaTypeType type, int withParticle)
{
	xmlSchemaModelGroupPtr item;
	xmlSchemaParticlePtr particle = NULL;
	xmlNode * child = NULL;
	xmlAttr * attr;
	int min = 1, max = 1, isElemRef, hasRefs = 0;
	if(!ctxt || !schema || !P_Node)
		return 0;
	/*
	 * Create a model group with the given compositor.
	 */
	item = xmlSchemaAddModelGroup(ctxt, schema, type, P_Node);
	if(item == NULL)
		return 0;
	if(withParticle) {
		if(type == XML_SCHEMA_TYPE_ALL) {
			min = xmlGetMinOccurs(ctxt, P_Node, 0, 1, 1, "(0 | 1)");
			max = xmlGetMaxOccurs(ctxt, P_Node, 1, 1, 1, "1");
		}
		else {
			/* choice + sequence */
			min = xmlGetMinOccurs(ctxt, P_Node, 0, -1, 1, "xs:nonNegativeInteger");
			max = xmlGetMaxOccurs(ctxt, P_Node, 0, UNBOUNDED, 1, "(xs:nonNegativeInteger | unbounded)");
		}
		xmlSchemaPCheckParticleCorrect_2(ctxt, NULL, P_Node, min, max);
		/*
		 * Create a particle
		 */
		particle = xmlSchemaAddParticle(ctxt, P_Node, min, max);
		if(!particle)
			return 0;
		particle->children = (xmlSchemaTreeItem *)item;
		/*
		 * Check for illegal attributes.
		 */
		attr = P_Node->properties;
		while(attr) {
			if(!attr->ns) {
				if(!sstreq(attr->name, "id") && !sstreq(attr->name, "maxOccurs") && !sstreq(attr->name, "minOccurs")) {
					xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
				}
			}
			else if(sstreq(attr->ns->href, xmlSchemaNs)) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
			attr = attr->next;
		}
	}
	else {
		/*
		 * Check for illegal attributes.
		 */
		attr = P_Node->properties;
		while(attr) {
			if(attr->ns == NULL) {
				if(!sstreq(attr->name, "id")) {
					xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
				}
			}
			else if(sstreq(attr->ns->href, xmlSchemaNs)) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
			attr = attr->next;
		}
	}

	/*
	 * Extract and validate attributes.
	 */
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		item->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	if(type == XML_SCHEMA_TYPE_ALL) {
		xmlSchemaParticlePtr part, last = NULL;
		while(IS_SCHEMA(child, "element")) {
			part = (xmlSchemaParticlePtr)xmlSchemaParseElement(ctxt, schema, child, &isElemRef, 0);
			/*
			 * SPEC cos-all-limited (2)
			 * "The {max occurs} of all the particles in the {particles}
			 * of the ('all') group must be 0 or 1.
			 */
			if(part) {
				if(isElemRef)
					hasRefs++;
				if(part->minOccurs > 1) {
					xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_COS_ALL_LIMITED, NULL, child, "Invalid value for minOccurs (must be 0 or 1)", NULL);
					/* Reset to 1. */
					part->minOccurs = 1;
				}
				if(part->maxOccurs > 1) {
					xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_COS_ALL_LIMITED, NULL, child, "Invalid value for maxOccurs (must be 0 or 1)", NULL);
					/* Reset to 1. */
					part->maxOccurs = 1;
				}
				if(last == NULL)
					item->children = (xmlSchemaTreeItem *)part;
				else
					last->next = (xmlSchemaTreeItem *)part;
				last = part;
			}
			child = child->next;
		}
		if(child)
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (annotation?, element*)");
	}
	else {
		/* choice + sequence */
		xmlSchemaTreeItem * part = NULL;
		xmlSchemaTreeItem * last = NULL;
		while((IS_SCHEMA(child, "element")) || (IS_SCHEMA(child, "group")) || (IS_SCHEMA(child, "any")) || (IS_SCHEMA(child, "choice")) || (IS_SCHEMA(child, "sequence"))) {
			if(IS_SCHEMA(child, "element")) {
				part = (xmlSchemaTreeItem *)xmlSchemaParseElement(ctxt, schema, child, &isElemRef, 0);
				if(part && isElemRef)
					hasRefs++;
			}
			else if(IS_SCHEMA(child, "group")) {
				part = xmlSchemaParseModelGroupDefRef(ctxt, schema, child);
				if(part)
					hasRefs++;
				/*
				 * Handle redefinitions.
				 */
				if(ctxt->isRedefine && ctxt->redef && (ctxt->redef->item->type == XML_SCHEMA_TYPE_GROUP) && part && part->children) {
					if((xmlSchemaGetQNameRefName(part->children) == ctxt->redef->refName) && (xmlSchemaGetQNameRefTargetNs(part->children) == ctxt->redef->refTargetNs)) {
						/*
						 * SPEC src-redefine:
						 * (6.1) "If it has a <group> among its contents at
						 * some level the `actual value` of whose ref
						 * [attribute] is the same as the `actual value` of
						 * its own name attribute plus target namespace, then
						 * all of the following must be true:"
						 * (6.1.1) "It must have exactly one such group."
						 */
						if(ctxt->redefCounter != 0) {
							xmlChar * str = NULL;
							xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_SRC_REDEFINE, child, NULL,
							    "The redefining model group definition '%s' must not contain more than one reference to the redefined definition",
							    xmlSchemaFormatQName(&str, ctxt->redef->refTargetNs, ctxt->redef->refName), NULL);
							ZFREE(str);
							part = NULL;
						}
						else if(((WXS_PARTICLE(part))->minOccurs != 1) || ((WXS_PARTICLE(part))->maxOccurs != 1)) {
							xmlChar * str = NULL;
							/*
							 * SPEC src-redefine:
							 * (6.1.2) "The `actual value` of both that
							 * group's minOccurs and maxOccurs [attribute]
							 * must be 1 (or `absent`).
							 */
							xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_SRC_REDEFINE, child, NULL,
							    "The redefining model group definition '%s' must not contain a reference to the redefined definition with a maxOccurs/minOccurs other than 1",
							    xmlSchemaFormatQName(&str, ctxt->redef->refTargetNs, ctxt->redef->refName), NULL);
							ZFREE(str);
							part = NULL;
						}
						ctxt->redef->reference = WXS_BASIC_CAST part;
						ctxt->redefCounter++;
					}
				}
			}
			else if(IS_SCHEMA(child, "any")) {
				part = (xmlSchemaTreeItem *)xmlSchemaParseAny(ctxt, schema, child);
			}
			else if(IS_SCHEMA(child, "choice")) {
				part = xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_CHOICE, 1);
			}
			else if(IS_SCHEMA(child, "sequence")) {
				part = xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_SEQUENCE, 1);
			}
			if(part) {
				if(last == NULL)
					item->children = part;
				else
					last->next = part;
				last = part;
			}
			child = child->next;
		}
		if(child)
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (element | group | choice | sequence | any)*)");
	}
	if((max == 0) && (min == 0))
		return 0;
	if(hasRefs) {
		/*
		 * We need to resolve references.
		 */
		WXS_ADD_PENDING(ctxt, item);
	}
	if(withParticle)
		return ((xmlSchemaTreeItem *)particle);
	else
		return ((xmlSchemaTreeItem *)item);
}
/**
 * xmlSchemaParseRestriction:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Restriction definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns the type definition or NULL in case of error
 */
static xmlSchemaTypePtr xmlSchemaParseRestriction(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, xmlSchemaTypeType parentType)
{
	xmlSchemaTypePtr type;
	xmlNode * child = NULL;
	xmlAttr * attr;

	if(!ctxt || !schema || !P_Node)
		return 0;
	/* Not a component, don't create it. */
	type = ctxt->ctxtType;
	type->flags |= XML_SCHEMAS_TYPE_DERIVATION_METHOD_RESTRICTION;

	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "base"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	/*
	 * Extract and validate attributes.
	 */
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * Attribute
	 */
	/*
	 * Extract the base type. The "base" attribute is mandatory if inside
	 * a complex type or if redefining.
	 *
	 * SPEC (1.2) "...otherwise (<restriction> has no <simpleType> "
	 * among its [children]), the simple type definition which is
	 * the {content type} of the type definition `resolved` to by
	 * the `actual value` of the base [attribute]"
	 */
	if(xmlSchemaPValAttrQName(ctxt, schema, NULL, P_Node, "base", &(type->baseNs), &(type->base)) == 0) {
		if(!type->base && (type->type == XML_SCHEMA_TYPE_COMPLEX)) {
			xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "base", 0);
		}
		else if((ctxt->isRedefine) && (type->flags & XML_SCHEMAS_TYPE_GLOBAL)) {
			if(!type->base) {
				xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "base", 0);
			}
			else if(!sstreq(type->base, type->name) || !sstreq(type->baseNs, type->targetNamespace)) {
				xmlChar * str1 = NULL;
				xmlChar * str2 = NULL;
				/*
				 * REDEFINE: SPEC src-redefine (5)
				 * "Within the [children], each <simpleType> must have a
				 * <restriction> among its [children] ... the `actual value` of
				 * whose base [attribute] must be the same as the `actual value`
				 * of its own name attribute plus target namespace;"
				 */
				xmlSchemaPCustomErrExt(ctxt, XML_SCHEMAP_SRC_REDEFINE, NULL, P_Node, 
					"This is a redefinition, but the QName value '%s' of the 'base' attribute does not match the type's designation '%s'",
				    xmlSchemaFormatQName(&str1, type->baseNs, type->base), xmlSchemaFormatQName(&str2, type->targetNamespace, type->name), 0);
				ZFREE(str1);
				ZFREE(str2);
				// Avoid confusion and erase the values
				type->base = NULL;
				type->baseNs = NULL;
			}
		}
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * Add the annotation to the simple type ancestor.
		 */
		xmlSchemaAddAnnotation((xmlSchemaAnnotItem *)type, xmlSchemaParseAnnotation(ctxt, child, 1));
		child = child->next;
	}
	if(parentType == XML_SCHEMA_TYPE_SIMPLE) {
		/*
		 * Corresponds to <simpleType><restriction><simpleType>.
		 */
		if(IS_SCHEMA(child, "simpleType")) {
			if(type->base) {
				/*
				 * src-restriction-base-or-simpleType
				 * Either the base [attribute] or the simpleType [child] of the
				 * <restriction> element must be present, but not both.
				 */
				xmlSchemaPContentErr(ctxt, XML_SCHEMAP_SRC_RESTRICTION_BASE_OR_SIMPLETYPE,
				    NULL, P_Node, child, "The attribute 'base' and the <simpleType> child are mutually exclusive", 0);
			}
			else {
				type->baseType = (xmlSchemaTypePtr)xmlSchemaParseSimpleType(ctxt, schema, child, 0);
			}
			child = child->next;
		}
		else if(!type->base) {
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_SRC_RESTRICTION_BASE_OR_SIMPLETYPE, NULL, P_Node, child,
			    "Either the attribute 'base' or a <simpleType> child must be present", 0);
		}
	}
	else if(parentType == XML_SCHEMA_TYPE_COMPLEX_CONTENT) {
		/*
		 * Corresponds to <complexType><complexContent><restriction>...
		 * followed by:
		 *
		 * Model groups <all>, <choice> and <sequence>.
		 */
		if(IS_SCHEMA(child, "all")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_ALL, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "choice")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_CHOICE, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "sequence")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_SEQUENCE, 1);
			child = child->next;
			/*
			 * Model group reference <group>.
			 */
		}
		else if(IS_SCHEMA(child, "group")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroupDefRef(ctxt, schema, child);
			/*
			 * Note that the reference will be resolved in xmlSchemaResolveTypeReferences();
			 */
			child = child->next;
		}
	}
	else if(parentType == XML_SCHEMA_TYPE_SIMPLE_CONTENT) {
		/*
		 * Corresponds to <complexType><simpleContent><restriction>...
		 *
		 * "1.1 the simple type definition corresponding to the <simpleType>
		 * among the [children] of <restriction> if there is one;"
		 */
		if(IS_SCHEMA(child, "simpleType")) {
			/*
			 * We will store the to-be-restricted simple type in type->contentTypeDef *temporarily*.
			 */
			type->contentTypeDef = (xmlSchemaTypePtr)xmlSchemaParseSimpleType(ctxt, schema, child, 0);
			if(type->contentTypeDef == NULL)
				return 0;
			child = child->next;
		}
	}
	if(oneof2(parentType, XML_SCHEMA_TYPE_SIMPLE, XML_SCHEMA_TYPE_SIMPLE_CONTENT)) {
		xmlSchemaFacetPtr facet, lastfacet = NULL;
		/*
		 * Corresponds to <complexType><simpleContent><restriction>...
		 * <simpleType><restriction>...
		 */

		/*
		 * Add the facets to the simple type ancestor.
		 */
		/*
		 * @todo Datatypes: 4.1.3 Constraints on XML Representation of
		 * Simple Type Definition Schema Representation Constraint:
		 * *Single Facet Value*
		 */
		while((IS_SCHEMA(child, "minInclusive")) || (IS_SCHEMA(child, "minExclusive")) || (IS_SCHEMA(child, "maxInclusive")) ||
		    (IS_SCHEMA(child, "maxExclusive")) || (IS_SCHEMA(child, "totalDigits")) || (IS_SCHEMA(child, "fractionDigits")) ||
		    (IS_SCHEMA(child, "pattern")) || (IS_SCHEMA(child, "enumeration")) || (IS_SCHEMA(child, "whiteSpace")) ||
		    (IS_SCHEMA(child, "length")) || (IS_SCHEMA(child, "maxLength")) || (IS_SCHEMA(child, "minLength"))) {
			facet = xmlSchemaParseFacet(ctxt, schema, child);
			if(facet) {
				if(lastfacet == NULL)
					type->facets = facet;
				else
					lastfacet->next = facet;
				lastfacet = facet;
				lastfacet->next = NULL;
			}
			child = child->next;
		}
		/*
		 * Create links for derivation and validation.
		 */
		if(type->facets) {
			xmlSchemaFacetLinkPtr facetLink, lastFacetLink = NULL;
			facet = type->facets;
			do {
				facetLink = (xmlSchemaFacetLinkPtr)SAlloc::M(sizeof(xmlSchemaFacetLink));
				if(facetLink == NULL) {
					xmlSchemaPErrMemory(ctxt, "allocating a facet link", 0);
					SAlloc::F(facetLink);
					return 0;
				}
				facetLink->facet = facet;
				facetLink->next = NULL;
				if(lastFacetLink == NULL)
					type->facetSet = facetLink;
				else
					lastFacetLink->next = facetLink;
				lastFacetLink = facetLink;
				facet = facet->next;
			} while(facet);
		}
	}
	if(type->type == XML_SCHEMA_TYPE_COMPLEX) {
		/*
		 * Attribute uses/declarations.
		 */
		if(xmlSchemaParseLocalAttributes(ctxt, schema, &child, (xmlSchemaItemListPtr*)&(type->attrUses), XML_SCHEMA_TYPE_RESTRICTION, 0) == -1)
			return 0;
		/*
		 * Attribute wildcard.
		 */
		if(IS_SCHEMA(child, "anyAttribute")) {
			type->attributeWildcard = xmlSchemaParseAnyAttribute(ctxt, schema, child);
			child = child->next;
		}
	}
	if(child) {
		if(parentType == XML_SCHEMA_TYPE_COMPLEX_CONTENT) {
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL,
			    "annotation?, (group | all | choice | sequence)?, ((attribute | attributeGroup)*, anyAttribute?))");
		}
		else if(parentType == XML_SCHEMA_TYPE_SIMPLE_CONTENT) {
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL,
			    "(annotation?, (simpleType?, (minExclusive | minInclusive | maxExclusive | maxInclusive | totalDigits | fractionDigits | "
			    "length | minLength | maxLength | enumeration | whiteSpace | pattern)*)?, ((attribute | attributeGroup)*, anyAttribute?))");
		}
		else {
			/* Simple type */
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL,
			    "(annotation?, (simpleType?, (minExclusive | minInclusive | maxExclusive | maxInclusive | totalDigits | fractionDigits | "
			    "length | minLength | maxLength | enumeration | whiteSpace | pattern)*))");
		}
	}
	return 0;
}
/**
 * xmlSchemaParseExtension:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * Parses an <extension>, which is found inside a
 * <simpleContent> or <complexContent>.
 * *WARNING* this interface is highly subject to change.
 *
 * @todo Returns the type definition or NULL in case of error
 */
static xmlSchemaTypePtr xmlSchemaParseExtension(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, xmlSchemaTypeType parentType)
{
	xmlSchemaTypePtr type;
	xmlNode * child = NULL;
	xmlAttr * attr;
	if(!ctxt || !schema || !P_Node)
		return 0;
	/* Not a component, don't create it. */
	type = ctxt->ctxtType;
	type->flags |= XML_SCHEMAS_TYPE_DERIVATION_METHOD_EXTENSION;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if(!sstreq(attr->name, "id") && !sstreq(attr->name, "base")) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * Attribute "base" - mandatory.
	 */
	if((xmlSchemaPValAttrQName(ctxt, schema, NULL, P_Node, "base", &(type->baseNs), &(type->base)) == 0) && (type->base == NULL)) {
		xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "base", 0);
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * Add the annotation to the type ancestor.
		 */
		xmlSchemaAddAnnotation((xmlSchemaAnnotItem *)type, xmlSchemaParseAnnotation(ctxt, child, 1));
		child = child->next;
	}
	if(parentType == XML_SCHEMA_TYPE_COMPLEX_CONTENT) {
		/*
		 * Corresponds to <complexType><complexContent><extension>... and:
		 *
		 * Model groups <all>, <choice>, <sequence> and <group>.
		 */
		if(IS_SCHEMA(child, "all")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_ALL, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "choice")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_CHOICE, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "sequence")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_SEQUENCE, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "group")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroupDefRef(ctxt, schema, child);
			/*
			 * Note that the reference will be resolved in
			 * xmlSchemaResolveTypeReferences();
			 */
			child = child->next;
		}
	}
	if(child) {
		/*
		 * Attribute uses/declarations.
		 */
		if(xmlSchemaParseLocalAttributes(ctxt, schema, &child, (xmlSchemaItemListPtr*)&(type->attrUses), XML_SCHEMA_TYPE_EXTENSION, NULL) == -1)
			return 0;
		/*
		 * Attribute wildcard.
		 */
		if(IS_SCHEMA(child, "anyAttribute")) {
			ctxt->ctxtType->attributeWildcard = xmlSchemaParseAnyAttribute(ctxt, schema, child);
			child = child->next;
		}
	}
	if(child) {
		if(parentType == XML_SCHEMA_TYPE_COMPLEX_CONTENT) {
			/* Complex content extension. */
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL,
			    "(annotation?, ((group | all | choice | sequence)?, ((attribute | attributeGroup)*, anyAttribute?)))");
		}
		else {
			/* Simple content extension. */
			xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL,
			    "(annotation?, ((attribute | attributeGroup)*, anyAttribute?))");
		}
	}
	return 0;
}

/**
 * xmlSchemaParseSimpleContent:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema SimpleContent definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns the type definition or NULL in case of error
 */
static int xmlSchemaParseSimpleContent(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, int * hasRestrictionOrExtension)
{
	xmlSchemaTypePtr type;
	xmlNode * child = NULL;
	xmlAttr * attr;
	if(!ctxt || !schema || !P_Node || !hasRestrictionOrExtension)
		return -1;
	*hasRestrictionOrExtension = 0;
	/* Not a component, don't create it. */
	type = ctxt->ctxtType;
	type->contentType = XML_SCHEMA_CONTENT_SIMPLE;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if((!sstreq(attr->name, "id"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * Add the annotation to the complex type ancestor.
		 */
		xmlSchemaAddAnnotation((xmlSchemaAnnotItem *)type, xmlSchemaParseAnnotation(ctxt, child, 1));
		child = child->next;
	}
	if(child == NULL) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_MISSING, NULL, P_Node, NULL, NULL, "(annotation?, (restriction | extension))");
	}
	if(child == NULL) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_MISSING, NULL, P_Node, NULL, NULL, "(annotation?, (restriction | extension))");
	}
	if(IS_SCHEMA(child, "restriction")) {
		xmlSchemaParseRestriction(ctxt, schema, child, XML_SCHEMA_TYPE_SIMPLE_CONTENT);
		(*hasRestrictionOrExtension) = 1;
		child = child->next;
	}
	else if(IS_SCHEMA(child, "extension")) {
		xmlSchemaParseExtension(ctxt, schema, child, XML_SCHEMA_TYPE_SIMPLE_CONTENT);
		(*hasRestrictionOrExtension) = 1;
		child = child->next;
	}
	if(child)
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (restriction | extension))");
	return 0;
}

/**
 * xmlSchemaParseComplexContent:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema ComplexContent definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns the type definition or NULL in case of error
 */
static int xmlSchemaParseComplexContent(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, int * hasRestrictionOrExtension)
{
	xmlSchemaTypePtr type;
	xmlNode * child = NULL;
	xmlAttr * attr;
	if(!ctxt || !schema || !P_Node || !hasRestrictionOrExtension)
		return -1;
	*hasRestrictionOrExtension = 0;
	/* Not a component, don't create it. */
	type = ctxt->ctxtType;
	/*
	 * Check for illegal attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(!attr->ns) {
			if((!sstreq(attr->name, "id")) && (!sstreq(attr->name, "mixed"))) {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
	/*
	 * Set the 'mixed' on the complex type ancestor.
	 */
	if(xmlGetBooleanProp(ctxt, P_Node, "mixed", 0)) {
		if((type->flags & XML_SCHEMAS_TYPE_MIXED) == 0)
			type->flags |= XML_SCHEMAS_TYPE_MIXED;
	}
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		/*
		 * Add the annotation to the complex type ancestor.
		 */
		xmlSchemaAddAnnotation((xmlSchemaAnnotItem *)type, xmlSchemaParseAnnotation(ctxt, child, 1));
		child = child->next;
	}
	if(child == NULL) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_MISSING, NULL, P_Node, NULL, NULL, "(annotation?, (restriction | extension))");
	}
	if(child == NULL) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_MISSING, NULL, P_Node, NULL, NULL, "(annotation?, (restriction | extension))");
	}
	if(IS_SCHEMA(child, "restriction")) {
		xmlSchemaParseRestriction(ctxt, schema, child, XML_SCHEMA_TYPE_COMPLEX_CONTENT);
		(*hasRestrictionOrExtension) = 1;
		child = child->next;
	}
	else if(IS_SCHEMA(child, "extension")) {
		xmlSchemaParseExtension(ctxt, schema, child, XML_SCHEMA_TYPE_COMPLEX_CONTENT);
		(*hasRestrictionOrExtension) = 1;
		child = child->next;
	}
	if(child) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, "(annotation?, (restriction | extension))");
	}
	return 0;
}

/**
 * xmlSchemaParseComplexType:
 * @ctxt:  a schema validation context
 * @schema:  the schema being built
 * @node:  a subtree containing XML Schema informations
 *
 * parse a XML schema Complex Type definition
 * *WARNING* this interface is highly subject to change
 *
 * Returns the type definition or NULL in case of error
 */
static xmlSchemaTypePtr xmlSchemaParseComplexType(xmlSchemaParserCtxtPtr ctxt, xmlSchemaPtr schema, xmlNode * P_Node, int topLevel)
{
	xmlSchemaTypePtr type, ctxtType;
	xmlNode * child = NULL;
	const xmlChar * name = NULL;
	xmlAttr * attr;
	const xmlChar * attrValue;
#ifdef ENABLE_NAMED_LOCALS
	char buf[40];
#endif
	int final = 0, block = 0, hasRestrictionOrExtension = 0;
	if(!ctxt || !schema || !P_Node)
		return 0;
	ctxtType = ctxt->ctxtType;
	if(topLevel) {
		attr = xmlSchemaGetPropNode(P_Node, "name");
		if(!attr) {
			xmlSchemaPMissingAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_MISSING, NULL, P_Node, "name", 0);
			return 0;
		}
		else if(xmlSchemaPValAttrNode(ctxt, NULL, attr, xmlSchemaGetBuiltInType(XML_SCHEMAS_NCNAME), &name) != 0) {
			return 0;
		}
	}
	if(topLevel == 0) {
		/*
		 * Parse as local complex type definition.
		 */
#ifdef ENABLE_NAMED_LOCALS
		snprintf(buf, 39, "#CT%d", ctxt->counter++ + 1);
		type = xmlSchemaAddType(ctxt, schema,
		    XML_SCHEMA_TYPE_COMPLEX,
		    xmlDictLookupSL(ctxt->dict, (const xmlChar*)buf),
		    ctxt->targetNamespace, node, 0);
#else
		type = xmlSchemaAddType(ctxt, schema, XML_SCHEMA_TYPE_COMPLEX, NULL, ctxt->targetNamespace, P_Node, 0);
#endif
		if(type == NULL)
			return 0;
		name = type->name;
		type->P_Node = P_Node;
		type->type = XML_SCHEMA_TYPE_COMPLEX;
		/*
		 * @todo We need the target namespace.
		 */
	}
	else {
		/*
		 * Parse as global complex type definition.
		 */
		type = xmlSchemaAddType(ctxt, schema, XML_SCHEMA_TYPE_COMPLEX, name, ctxt->targetNamespace, P_Node, 1);
		if(type == NULL)
			return 0;
		type->P_Node = P_Node;
		type->type = XML_SCHEMA_TYPE_COMPLEX;
		type->flags |= XML_SCHEMAS_TYPE_GLOBAL;
	}
	type->targetNamespace = ctxt->targetNamespace;
	/*
	 * Handle attributes.
	 */
	attr = P_Node->properties;
	while(attr) {
		if(attr->ns == NULL) {
			if(sstreq(attr->name, "id")) {
				/*
				 * Attribute "id".
				 */
				xmlSchemaPValAttrID(ctxt, P_Node, BAD_CAST "id");
			}
			else if(sstreq(attr->name, "mixed")) {
				/*
				 * Attribute "mixed".
				 */
				if(xmlSchemaPGetBoolNodeValue(ctxt, NULL, (xmlNode *)attr))
					type->flags |= XML_SCHEMAS_TYPE_MIXED;
			}
			else if(topLevel) {
				/*
				 * Attributes of global complex type definitions.
				 */
				if(sstreq(attr->name, "name")) {
					/* Pass. */
				}
				else if(sstreq(attr->name, "abstract")) {
					/*
					 * Attribute "abstract".
					 */
					if(xmlSchemaPGetBoolNodeValue(ctxt, NULL, (xmlNode *)attr))
						type->flags |= XML_SCHEMAS_TYPE_ABSTRACT;
				}
				else if(sstreq(attr->name, "final")) {
					/*
					 * Attribute "final".
					 */
					attrValue = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
					if(xmlSchemaPValAttrBlockFinal(attrValue, &(type->flags), -1,
						XML_SCHEMAS_TYPE_FINAL_EXTENSION, XML_SCHEMAS_TYPE_FINAL_RESTRICTION, -1, -1, -1) != 0) {
						xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, NULL, (xmlNode *)attr, NULL, "(#all | List of (extension | restriction))",
						    attrValue, NULL, 0, 0);
					}
					else
						final = 1;
				}
				else if(sstreq(attr->name, "block")) {
					/*
					 * Attribute "block".
					 */
					attrValue = xmlSchemaGetNodeContent(ctxt, (xmlNode *)attr);
					if(xmlSchemaPValAttrBlockFinal(attrValue, &(type->flags), -1, XML_SCHEMAS_TYPE_BLOCK_EXTENSION, XML_SCHEMAS_TYPE_BLOCK_RESTRICTION, -1, -1, -1) != 0) {
						xmlSchemaPSimpleTypeErr(ctxt, XML_SCHEMAP_S4S_ATTR_INVALID_VALUE, NULL, (xmlNode *)attr, NULL,
						    "(#all | List of (extension | restriction)) ", attrValue, NULL, 0, 0);
					}
					else
						block = 1;
				}
				else {
					xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
				}
			}
			else {
				xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
			}
		}
		else if(sstreq(attr->ns->href, xmlSchemaNs)) {
			xmlSchemaPIllegalAttrErr(ctxt, XML_SCHEMAP_S4S_ATTR_NOT_ALLOWED, NULL, attr);
		}
		attr = attr->next;
	}
	if(!block) {
		/*
		 * Apply default "block" values.
		 */
		if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_RESTRICTION)
			type->flags |= XML_SCHEMAS_TYPE_BLOCK_RESTRICTION;
		if(schema->flags & XML_SCHEMAS_BLOCK_DEFAULT_EXTENSION)
			type->flags |= XML_SCHEMAS_TYPE_BLOCK_EXTENSION;
	}
	if(!final) {
		/*
		 * Apply default "block" values.
		 */
		if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_RESTRICTION)
			type->flags |= XML_SCHEMAS_TYPE_FINAL_RESTRICTION;
		if(schema->flags & XML_SCHEMAS_FINAL_DEFAULT_EXTENSION)
			type->flags |= XML_SCHEMAS_TYPE_FINAL_EXTENSION;
	}
	/*
	 * And now for the children...
	 */
	child = P_Node->children;
	if(IS_SCHEMA(child, "annotation")) {
		type->annot = xmlSchemaParseAnnotation(ctxt, child, 1);
		child = child->next;
	}
	ctxt->ctxtType = type;
	if(IS_SCHEMA(child, "simpleContent")) {
		/*
		 * <complexType><simpleContent>...
		 * 3.4.3 : 2.2
		 * Specifying mixed='true' when the <simpleContent>
		 * alternative is chosen has no effect
		 */
		if(type->flags & XML_SCHEMAS_TYPE_MIXED)
			type->flags ^= XML_SCHEMAS_TYPE_MIXED;
		xmlSchemaParseSimpleContent(ctxt, schema, child, &hasRestrictionOrExtension);
		child = child->next;
	}
	else if(IS_SCHEMA(child, "complexContent")) {
		/*
		 * <complexType><complexContent>...
		 */
		type->contentType = XML_SCHEMA_CONTENT_EMPTY;
		xmlSchemaParseComplexContent(ctxt, schema, child,
		    &hasRestrictionOrExtension);
		child = child->next;
	}
	else {
		/*
		 * E.g <complexType><sequence>... or <complexType><attribute>... etc.
		 *
		 * SPEC
		 * "...the third alternative (neither <simpleContent> nor
		 * <complexContent>) is chosen. This case is understood as shorthand
		 * for complex content restricting the `ur-type definition`, and the
		 * details of the mappings should be modified as necessary.
		 */
		type->baseType = xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYTYPE);
		type->flags |= XML_SCHEMAS_TYPE_DERIVATION_METHOD_RESTRICTION;
		/*
		 * Parse model groups.
		 */
		if(IS_SCHEMA(child, "all")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_ALL, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "choice")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_CHOICE, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "sequence")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroup(ctxt, schema, child, XML_SCHEMA_TYPE_SEQUENCE, 1);
			child = child->next;
		}
		else if(IS_SCHEMA(child, "group")) {
			type->subtypes = (xmlSchemaTypePtr)xmlSchemaParseModelGroupDefRef(ctxt, schema, child);
			/*
			 * Note that the reference will be resolved in
			 * xmlSchemaResolveTypeReferences();
			 */
			child = child->next;
		}
		/*
		 * Parse attribute decls/refs.
		 */
		if(xmlSchemaParseLocalAttributes(ctxt, schema, &child, (xmlSchemaItemListPtr*)&(type->attrUses), XML_SCHEMA_TYPE_RESTRICTION, NULL) == -1)
			return 0;
		/*
		 * Parse attribute wildcard.
		 */
		if(IS_SCHEMA(child, "anyAttribute")) {
			type->attributeWildcard = xmlSchemaParseAnyAttribute(ctxt, schema, child);
			child = child->next;
		}
	}
	if(child) {
		xmlSchemaPContentErr(ctxt, XML_SCHEMAP_S4S_ELEM_NOT_ALLOWED, NULL, P_Node, child, NULL, 
			"(annotation?, (simpleContent | complexContent | ((group | all | choice | sequence)?, ((attribute | attributeGroup)*, anyAttribute?))))");
	}
	/*
	 * REDEFINE: SPEC src-redefine (5)
	 */
	if(topLevel && ctxt->isRedefine && (!hasRestrictionOrExtension)) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_REDEFINE, NULL, P_Node, 
			"This is a redefinition, thus the <complexType> must have a <restriction> or <extension> grand-child", 0);
	}
	ctxt->ctxtType = ctxtType;
	return (type);
}

/************************************************************************
*									*
*			Validating using Schemas			*
*									*
************************************************************************/

/************************************************************************
*									*
*			Reading/Writing Schemas				*
*									*
************************************************************************/

#if 0 /* Will be enabled if it is clear what options are needed. */
/**
 * xmlSchemaParserCtxtSetOptions:
 * @ctxt:	a schema parser context
 * @options: a combination of xmlSchemaParserOption
 *
 * Sets the options to be used during the parse.
 *
 * Returns 0 in case of success, -1 in case of an
 * API error.
 */
static int xmlSchemaParserCtxtSetOptions(xmlSchemaParserCtxtPtr ctxt, int options)
{
	int i;
	if(!ctxt)
		return -1;
	/*
	 * WARNING: Change the start value if adding to the
	 * xmlSchemaParseOption.
	 */
	for(i = 1; i < (int)sizeof(int) * 8; i++) {
		if(options & 1<<i) {
			return -1;
		}
	}
	ctxt->options = options;
	return 0;
}

/**
 * xmlSchemaValidCtxtGetOptions:
 * @ctxt: a schema parser context
 *
 * Returns the option combination of the parser context.
 */
static int xmlSchemaParserCtxtGetOptions(xmlSchemaParserCtxtPtr ctxt)
{
	return ctxt ? ctxt->options : -1;
}

#endif

/**
 * xmlSchemaNewParserCtxt:
 * @URL:  the location of the schema
 *
 * Create an XML Schemas parse context for that file/resource expected
 * to contain an XML Schemas file.
 *
 * Returns the parser context or NULL in case of error
 */
xmlSchemaParserCtxtPtr xmlSchemaNewParserCtxt(const char * URL)
{
	xmlSchemaParserCtxtPtr ret = 0;
	if(URL) {
		ret = xmlSchemaParserCtxtCreate();
		if(ret) {
			ret->dict = xmlDictCreate();
			ret->URL = xmlDictLookupSL(ret->dict, (const xmlChar*)URL);
		}
	}
	return ret;
}

/**
 * xmlSchemaNewMemParserCtxt:
 * @buffer:  a pointer to a char array containing the schemas
 * @size:  the size of the array
 *
 * Create an XML Schemas parse context for that memory buffer expected
 * to contain an XML Schemas file.
 *
 * Returns the parser context or NULL in case of error
 */
xmlSchemaParserCtxtPtr xmlSchemaNewMemParserCtxt(const char * buffer, int size)
{
	xmlSchemaParserCtxtPtr ret;
	if((buffer == NULL) || (size <= 0))
		return 0;
	ret = xmlSchemaParserCtxtCreate();
	if(!ret)
		return 0;
	ret->buffer = buffer;
	ret->size = size;
	ret->dict = xmlDictCreate();
	return ret;
}

/**
 * xmlSchemaNewDocParserCtxt:
 * @doc:  a preparsed document tree
 *
 * Create an XML Schemas parse context for that document.
 * NB. The document may be modified during the parsing process.
 *
 * Returns the parser context or NULL in case of error
 */
xmlSchemaParserCtxtPtr xmlSchemaNewDocParserCtxt(xmlDocPtr doc)
{
	xmlSchemaParserCtxtPtr ret;
	if(!doc)
		return 0;
	ret = xmlSchemaParserCtxtCreate();
	if(!ret)
		return 0;
	ret->doc = doc;
	ret->dict = xmlDictCreate();
	/* The application has responsibility for the document */
	ret->preserve = 1;
	return ret;
}

/**
 * xmlSchemaFreeParserCtxt:
 * @ctxt:  the schema parser context
 *
 * Free the resources associated to the schema parser context
 */
void xmlSchemaFreeParserCtxt(xmlSchemaParserCtxtPtr ctxt)
{
	if(ctxt) {
		if(ctxt->doc && !ctxt->preserve)
			xmlFreeDoc(ctxt->doc);
		xmlSchemaFreeValidCtxt(ctxt->vctxt);
		if(ctxt->ownsConstructor && ctxt->constructor) {
			xmlSchemaConstructionCtxtFree(ctxt->constructor);
			ctxt->constructor = NULL;
			ctxt->ownsConstructor = 0;
		}
		xmlSchemaItemListFree(ctxt->attrProhibs);
		xmlDictFree(ctxt->dict);
		SAlloc::F(ctxt);
	}
}

/************************************************************************
*									*
*			Building the content models			*
*									*
************************************************************************/

/**
 * xmlSchemaBuildContentModelForSubstGroup:
 *
 * Returns 1 if nillable, 0 otherwise
 */
static int xmlSchemaBuildContentModelForSubstGroup(xmlSchemaParserCtxtPtr pctxt, xmlSchemaParticlePtr particle, int counter, xmlAutomataStatePtr end)
{
	xmlAutomataStatePtr start, tmp;
	xmlSchemaElement * member;
	xmlSchemaSubstGroup * substGroup;
	int i;
	int ret = 0;
	xmlSchemaElement * elemDecl = (xmlSchemaElement *)particle->children;
	/*
	 * Wrap the substitution group with a CHOICE.
	 */
	start = pctxt->state;
	SETIFZ(end, xmlAutomataNewState(pctxt->am));
	substGroup = xmlSchemaSubstGroupGet(pctxt, elemDecl);
	if(substGroup == NULL) {
		xmlSchemaPErr(pctxt, WXS_ITEM_NODE(particle), XML_SCHEMAP_INTERNAL,
		    "Internal error: xmlSchemaBuildContentModelForSubstGroup, "
		    "declaration is marked having a subst. group but none "
		    "available.\n", elemDecl->name, 0);
		return 0;
	}
	if(counter >= 0) {
		/*
		 * NOTE that we put the declaration in, even if it's abstract.
		 * However, an error will be raised during *validation* if an element
		 * information item shall be validated against an abstract element
		 * declaration.
		 */
		tmp = xmlAutomataNewCountedTrans(pctxt->am, start, NULL, counter);
		xmlAutomataNewTransition2(pctxt->am, tmp, end, elemDecl->name, elemDecl->targetNamespace, elemDecl);
		/*
		 * Add subst. group members.
		 */
		for(i = 0; i < substGroup->members->nbItems; i++) {
			member = (xmlSchemaElementPtr)substGroup->members->items[i];
			xmlAutomataNewTransition2(pctxt->am, tmp, end, member->name, member->targetNamespace, member);
		}
	}
	else if(particle->maxOccurs == 1) {
		/*
		 * NOTE that we put the declaration in, even if it's abstract,
		 */
		xmlAutomataNewEpsilon(pctxt->am, xmlAutomataNewTransition2(pctxt->am, start, NULL, elemDecl->name, elemDecl->targetNamespace, elemDecl), end);
		/*
		 * Add subst. group members.
		 */
		for(i = 0; i < substGroup->members->nbItems; i++) {
			member = (xmlSchemaElementPtr)substGroup->members->items[i];
			/*
			 * NOTE: This fixes bug #341150. xmlAutomataNewOnceTrans2()
			 *  was incorrectly used instead of xmlAutomataNewTransition2()
			 *  (seems like a copy&paste bug from the XML_SCHEMA_TYPE_ALL
			 *  section in xmlSchemaBuildAContentModel() ).
			 * @todo Check if xmlAutomataNewOnceTrans2() was instead
			 *  intended for the above "counter" section originally. I.e.,
			 *  check xs:all with subst-groups.
			 *
			 * tmp = xmlAutomataNewOnceTrans2(pctxt->am, start, NULL,
			 *	               member->name, member->targetNamespace,
			 *		       1, 1, member);
			 */
			tmp = xmlAutomataNewTransition2(pctxt->am, start, NULL,
			    member->name, member->targetNamespace, member);
			xmlAutomataNewEpsilon(pctxt->am, tmp, end);
		}
	}
	else {
		xmlAutomataStatePtr hop;
		int maxOccurs = particle->maxOccurs == UNBOUNDED ? UNBOUNDED : particle->maxOccurs - 1;
		int minOccurs = particle->minOccurs < 1 ? 0 : particle->minOccurs - 1;
		counter = xmlAutomataNewCounter(pctxt->am, minOccurs, maxOccurs);
		hop = xmlAutomataNewState(pctxt->am);
		xmlAutomataNewEpsilon(pctxt->am, xmlAutomataNewTransition2(pctxt->am, start, NULL, elemDecl->name, elemDecl->targetNamespace, elemDecl), hop);
		/*
		 * Add subst. group members.
		 */
		for(i = 0; i < substGroup->members->nbItems; i++) {
			member = (xmlSchemaElementPtr)substGroup->members->items[i];
			xmlAutomataNewEpsilon(pctxt->am, xmlAutomataNewTransition2(pctxt->am, start, NULL, member->name, member->targetNamespace, member), hop);
		}
		xmlAutomataNewCountedTrans(pctxt->am, hop, start, counter);
		xmlAutomataNewCounterTrans(pctxt->am, hop, end, counter);
	}
	if(particle->minOccurs == 0) {
		xmlAutomataNewEpsilon(pctxt->am, start, end);
		ret = 1;
	}
	pctxt->state = end;
	return ret;
}

/**
 * xmlSchemaBuildContentModelForElement:
 *
 * Returns 1 if nillable, 0 otherwise
 */
static int xmlSchemaBuildContentModelForElement(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr particle)
{
	int ret = 0;
	if(((xmlSchemaElementPtr)particle->children)->flags & XML_SCHEMAS_ELEM_SUBST_GROUP_HEAD) {
		/*
		 * Substitution groups.
		 */
		ret = xmlSchemaBuildContentModelForSubstGroup(ctxt, particle, -1, 0);
	}
	else {
		xmlAutomataStatePtr start;
		xmlSchemaElementPtr elemDecl = (xmlSchemaElementPtr)particle->children;
		if(elemDecl->flags & XML_SCHEMAS_ELEM_ABSTRACT)
			return 0;
		if(particle->maxOccurs == 1) {
			start = ctxt->state;
			ctxt->state = xmlAutomataNewTransition2(ctxt->am, start, NULL, elemDecl->name, elemDecl->targetNamespace, elemDecl);
		}
		else if((particle->maxOccurs >= UNBOUNDED) && (particle->minOccurs < 2)) {
			/* Special case. */
			start = ctxt->state;
			ctxt->state = xmlAutomataNewTransition2(ctxt->am, start, NULL, elemDecl->name, elemDecl->targetNamespace, elemDecl);
			ctxt->state = xmlAutomataNewTransition2(ctxt->am, ctxt->state, ctxt->state, elemDecl->name, elemDecl->targetNamespace, elemDecl);
		}
		else {
			int counter;
			int maxOccurs = particle->maxOccurs == UNBOUNDED ? UNBOUNDED : particle->maxOccurs - 1;
			int minOccurs = particle->minOccurs < 1 ? 0 : particle->minOccurs - 1;
			start = xmlAutomataNewEpsilon(ctxt->am, ctxt->state, 0);
			counter = xmlAutomataNewCounter(ctxt->am, minOccurs, maxOccurs);
			ctxt->state = xmlAutomataNewTransition2(ctxt->am, start, NULL, elemDecl->name, elemDecl->targetNamespace, elemDecl);
			xmlAutomataNewCountedTrans(ctxt->am, ctxt->state, start, counter);
			ctxt->state = xmlAutomataNewCounterTrans(ctxt->am, ctxt->state, NULL, counter);
		}
		if(particle->minOccurs == 0) {
			xmlAutomataNewEpsilon(ctxt->am, start, ctxt->state);
			ret = 1;
		}
	}
	return ret;
}

/**
 * xmlSchemaBuildAContentModel:
 * @ctxt:  the schema parser context
 * @particle:  the particle component
 * @name:  the complex type's name whose content is being built
 *
 * Create the automaton for the {content type} of a complex type.
 *
 * Returns 1 if the content is nillable, 0 otherwise
 */
static int xmlSchemaBuildAContentModel(xmlSchemaParserCtxtPtr pctxt, xmlSchemaParticlePtr particle)
{
	int ret = 0, tmp2;
	if(particle == NULL) {
		PERROR_INT("xmlSchemaBuildAContentModel", "particle is NULL");
		return 1;
	}
	if(particle->children == NULL) {
		/*
		 * Just return in this case. A missing "term" of the particle
		 * might arise due to an invalid "term" component.
		 */
		return 1;
	}

	switch(particle->children->type) {
		case XML_SCHEMA_TYPE_ANY: {
		    xmlAutomataStatePtr start, end;
		    xmlSchemaWildcardPtr wild;
		    xmlSchemaWildcardNsPtr ns;

		    wild = (xmlSchemaWildcardPtr)particle->children;

		    start = pctxt->state;
		    end = xmlAutomataNewState(pctxt->am);

		    if(particle->maxOccurs == 1) {
			    if(wild->any == 1) {
				    /*
				     * We need to add both transitions:
				     *
				     * 1. the {"*", "*"} for elements in a namespace.
				     */
				    pctxt->state = xmlAutomataNewTransition2(pctxt->am, start, NULL, BAD_CAST "*", BAD_CAST "*", wild);
				    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, end);
				    /*
				     * 2. the {"*"} for elements in no namespace.
				     */
				    pctxt->state = xmlAutomataNewTransition2(pctxt->am, start, NULL, BAD_CAST "*", NULL, wild);
				    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, end);
			    }
			    else if(wild->nsSet) {
				    ns = wild->nsSet;
				    do {
					    pctxt->state = start;
					    pctxt->state = xmlAutomataNewTransition2(pctxt->am, pctxt->state, NULL, BAD_CAST "*", ns->value, wild);
					    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, end);
					    ns = ns->next;
				    } while(ns);
			    }
			    else if(wild->negNsSet) {
				    pctxt->state = xmlAutomataNewNegTrans(pctxt->am, start, end, BAD_CAST "*", wild->negNsSet->value, wild);
			    }
		    }
		    else {
			    int counter;
			    xmlAutomataStatePtr hop;
			    int maxOccurs = particle->maxOccurs == UNBOUNDED ? UNBOUNDED : particle->maxOccurs - 1;
			    int minOccurs = particle->minOccurs < 1 ? 0 : particle->minOccurs - 1;
			    counter = xmlAutomataNewCounter(pctxt->am, minOccurs, maxOccurs);
			    hop = xmlAutomataNewState(pctxt->am);
			    if(wild->any == 1) {
				    pctxt->state = xmlAutomataNewTransition2(pctxt->am, start, NULL, BAD_CAST "*", BAD_CAST "*", wild);
				    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, hop);
				    pctxt->state = xmlAutomataNewTransition2(pctxt->am, start, NULL, BAD_CAST "*", NULL, wild);
				    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, hop);
			    }
			    else if(wild->nsSet) {
				    ns = wild->nsSet;
				    do {
					    pctxt->state = xmlAutomataNewTransition2(pctxt->am, start, NULL, BAD_CAST "*", ns->value, wild);
					    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, hop);
					    ns = ns->next;
				    } while(ns);
			    }
			    else if(wild->negNsSet) {
				    pctxt->state = xmlAutomataNewNegTrans(pctxt->am, start, hop, BAD_CAST "*", wild->negNsSet->value, wild);
			    }
			    xmlAutomataNewCountedTrans(pctxt->am, hop, start, counter);
			    xmlAutomataNewCounterTrans(pctxt->am, hop, end, counter);
		    }
		    if(particle->minOccurs == 0) {
			    xmlAutomataNewEpsilon(pctxt->am, start, end);
			    ret = 1;
		    }
		    pctxt->state = end;
		    break;
	    }
		case XML_SCHEMA_TYPE_ELEMENT:
		    ret = xmlSchemaBuildContentModelForElement(pctxt, particle);
		    break;
		case XML_SCHEMA_TYPE_SEQUENCE: {
		    xmlSchemaTreeItem * sub;
		    ret = 1;
		    /*
		     * If max and min occurances are default (1) then
		     * simply iterate over the particles of the <sequence>.
		     */
		    if((particle->minOccurs == 1) && (particle->maxOccurs == 1)) {
			    sub = particle->children->children;
			    while(sub) {
				    tmp2 = xmlSchemaBuildAContentModel(pctxt, (xmlSchemaParticlePtr)sub);
				    if(tmp2 != 1) 
						ret = 0;
				    sub = sub->next;
			    }
		    }
		    else {
			    xmlAutomataStatePtr oldstate = pctxt->state;
			    if(particle->maxOccurs >= UNBOUNDED) {
				    if(particle->minOccurs > 1) {
					    xmlAutomataStatePtr tmp;
					    int counter;
					    pctxt->state = xmlAutomataNewEpsilon(pctxt->am, oldstate, 0);
					    oldstate = pctxt->state;
					    counter = xmlAutomataNewCounter(pctxt->am, particle->minOccurs - 1, UNBOUNDED);
					    sub = particle->children->children;
					    while(sub) {
						    tmp2 = xmlSchemaBuildAContentModel(pctxt, (xmlSchemaParticlePtr)sub);
						    if(tmp2 != 1) 
								ret = 0;
						    sub = sub->next;
					    }
					    tmp = pctxt->state;
					    xmlAutomataNewCountedTrans(pctxt->am, tmp, oldstate, counter);
					    pctxt->state = xmlAutomataNewCounterTrans(pctxt->am, tmp, NULL, counter);
					    if(ret == 1)
						    xmlAutomataNewEpsilon(pctxt->am, oldstate, pctxt->state);
				    }
				    else {
					    pctxt->state = xmlAutomataNewEpsilon(pctxt->am, oldstate, 0);
					    oldstate = pctxt->state;
					    sub = particle->children->children;
					    while(sub) {
						    tmp2 = xmlSchemaBuildAContentModel(pctxt, (xmlSchemaParticlePtr)sub);
						    if(tmp2 != 1) 
								ret = 0;
						    sub = sub->next;
					    }
					    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, oldstate);
					    /*
					     * epsilon needed to block previous trans from
					     * being allowed to enter back from another
					     * construct
					     */
					    pctxt->state = xmlAutomataNewEpsilon(pctxt->am, pctxt->state, 0);
					    if(particle->minOccurs == 0) {
						    xmlAutomataNewEpsilon(pctxt->am, oldstate, pctxt->state);
						    ret = 1;
					    }
				    }
			    }
			    else if((particle->maxOccurs > 1) || (particle->minOccurs > 1)) {
				    xmlAutomataStatePtr tmp;
				    int counter;
				    pctxt->state = xmlAutomataNewEpsilon(pctxt->am, oldstate, 0);
				    oldstate = pctxt->state;
				    counter = xmlAutomataNewCounter(pctxt->am, particle->minOccurs - 1, particle->maxOccurs - 1);
				    sub = particle->children->children;
				    while(sub) {
					    tmp2 = xmlSchemaBuildAContentModel(pctxt, (xmlSchemaParticlePtr)sub);
					    if(tmp2 != 1) 
							ret = 0;
					    sub = sub->next;
				    }
				    tmp = pctxt->state;
				    xmlAutomataNewCountedTrans(pctxt->am, tmp, oldstate, counter);
				    pctxt->state = xmlAutomataNewCounterTrans(pctxt->am, tmp, NULL, counter);
				    if((particle->minOccurs == 0) || (ret == 1)) {
					    xmlAutomataNewEpsilon(pctxt->am, oldstate, pctxt->state);
					    ret = 1;
				    }
			    }
			    else {
				    sub = particle->children->children;
				    while(sub) {
					    tmp2 = xmlSchemaBuildAContentModel(pctxt, (xmlSchemaParticlePtr)sub);
					    if(tmp2 != 1) 
							ret = 0;
					    sub = sub->next;
				    }
				    /*
				     * epsilon needed to block previous trans from
				     * being allowed to enter back from another
				     * construct
				     */
				    pctxt->state = xmlAutomataNewEpsilon(pctxt->am, pctxt->state, 0);
				    if(particle->minOccurs == 0) {
					    xmlAutomataNewEpsilon(pctxt->am, oldstate, pctxt->state);
					    ret = 1;
				    }
			    }
		    }
		    break;
	    }
		case XML_SCHEMA_TYPE_CHOICE: {
		    xmlSchemaTreeItem * sub;
		    xmlAutomataStatePtr start, end;
		    ret = 0;
		    start = pctxt->state;
		    end = xmlAutomataNewState(pctxt->am);
		    /*
		     * iterate over the subtypes and remerge the end with an
		     * epsilon transition
		     */
		    if(particle->maxOccurs == 1) {
			    sub = particle->children->children;
			    while(sub) {
				    pctxt->state = start;
				    tmp2 = xmlSchemaBuildAContentModel(pctxt, (xmlSchemaParticlePtr)sub);
				    if(tmp2 == 1) 
						ret = 1;
				    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, end);
				    sub = sub->next;
			    }
		    }
		    else {
			    int counter;
			    xmlAutomataStatePtr hop, base;
			    int maxOccurs = particle->maxOccurs == UNBOUNDED ? UNBOUNDED : particle->maxOccurs - 1;
			    int minOccurs = particle->minOccurs < 1 ? 0 : particle->minOccurs - 1;
			    /*
			     * use a counter to keep track of the number of transtions
			     * which went through the choice.
			     */
			    counter = xmlAutomataNewCounter(pctxt->am, minOccurs, maxOccurs);
			    hop = xmlAutomataNewState(pctxt->am);
			    base = xmlAutomataNewState(pctxt->am);
			    sub = particle->children->children;
			    while(sub) {
				    pctxt->state = base;
				    tmp2 = xmlSchemaBuildAContentModel(pctxt, (xmlSchemaParticlePtr)sub);
				    if(tmp2 == 1) 
						ret = 1;
				    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, hop);
				    sub = sub->next;
			    }
			    xmlAutomataNewEpsilon(pctxt->am, start, base);
			    xmlAutomataNewCountedTrans(pctxt->am, hop, base, counter);
			    xmlAutomataNewCounterTrans(pctxt->am, hop, end, counter);
			    if(ret == 1)
				    xmlAutomataNewEpsilon(pctxt->am, base, end);
		    }
		    if(particle->minOccurs == 0) {
			    xmlAutomataNewEpsilon(pctxt->am, start, end);
			    ret = 1;
		    }
		    pctxt->state = end;
		    break;
	    }
		case XML_SCHEMA_TYPE_ALL: {
		    xmlAutomataStatePtr start, tmp;
		    xmlSchemaParticlePtr sub;
		    xmlSchemaElementPtr elemDecl;
		    ret = 1;
		    sub = (xmlSchemaParticlePtr)particle->children->children;
		    if(sub == NULL)
			    break;
		    ret = 0;
		    start = pctxt->state;
		    tmp = xmlAutomataNewState(pctxt->am);
		    xmlAutomataNewEpsilon(pctxt->am, pctxt->state, tmp);
		    pctxt->state = tmp;
		    while(sub) {
			    pctxt->state = tmp;
			    elemDecl = (xmlSchemaElementPtr)sub->children;
			    if(elemDecl == NULL) {
				    PERROR_INT("xmlSchemaBuildAContentModel", "<element> particle has no term");
				    return ret;
			    }
			    ;
			    /*
			     * NOTE: The {max occurs} of all the particles in the
			     * {particles} of the group must be 0 or 1; this is
			     * already ensured during the parse of the content of
			     * <all>.
			     */
			    if(elemDecl->flags & XML_SCHEMAS_ELEM_SUBST_GROUP_HEAD) {
				    /*
				     * This is an abstract group, we need to share
				     * the same counter for all the element transitions
				     * derived from the group
				     */
				    int counter = xmlAutomataNewCounter(pctxt->am, sub->minOccurs, sub->maxOccurs);
				    xmlSchemaBuildContentModelForSubstGroup(pctxt, sub, counter, pctxt->state);
			    }
			    else {
				    if((sub->minOccurs == 1) && (sub->maxOccurs == 1)) {
					    xmlAutomataNewOnceTrans2(pctxt->am, pctxt->state, pctxt->state, elemDecl->name, elemDecl->targetNamespace, 1, 1, elemDecl);
				    }
				    else if((sub->minOccurs == 0) && (sub->maxOccurs == 1)) {
					    xmlAutomataNewCountTrans2(pctxt->am, pctxt->state, pctxt->state, elemDecl->name, elemDecl->targetNamespace, 0, 1, elemDecl);
				    }
			    }
			    sub = (xmlSchemaParticlePtr)sub->next;
		    }
		    pctxt->state = xmlAutomataNewAllTrans(pctxt->am, pctxt->state, NULL, 0);
		    if(particle->minOccurs == 0) {
			    xmlAutomataNewEpsilon(pctxt->am, start, pctxt->state);
			    ret = 1;
		    }
		    break;
	    }
		case XML_SCHEMA_TYPE_GROUP:
		    /*
		     * If we hit a model group definition, then this means that
		     * it was empty, thus was not substituted for the containing
		     * model group. Just do nothing in this case.
		     * @todo But the group should be substituted and not occur at
		     * all in the content model at this point. Fix this.
		     */
		    ret = 1;
		    break;
		default:
		    xmlSchemaInternalErr2(ACTXT_CAST pctxt,
		    "xmlSchemaBuildAContentModel",
		    "found unexpected term of type '%s' in content model",
		    WXS_ITEM_TYPE_NAME(particle->children), 0);
		    return ret;
	}
	return ret;
}

/**
 * xmlSchemaBuildContentModel:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 * @name:  the element name
 *
 * Builds the content model of the complex type.
 */
static void xmlSchemaBuildContentModel(xmlSchemaTypePtr type, xmlSchemaParserCtxtPtr ctxt)
{
	if((type->type != XML_SCHEMA_TYPE_COMPLEX) || type->contModel || ((type->contentType != XML_SCHEMA_CONTENT_ELEMENTS) && (type->contentType != XML_SCHEMA_CONTENT_MIXED)))
		return;
#ifdef DEBUG_CONTENT
	xmlGenericError(0, "Building content model for %s\n", name);
#endif
	ctxt->am = NULL;
	ctxt->am = xmlNewAutomata();
	if(ctxt->am == NULL) {
		xmlGenericError(0, "Cannot create automata for complex type %s\n", type->name);
		return;
	}
	ctxt->state = xmlAutomataGetInitState(ctxt->am);
	/*
	 * Build the automaton.
	 */
	xmlSchemaBuildAContentModel(ctxt, WXS_TYPE_PARTICLE(type));
	xmlAutomataSetFinalState(ctxt->am, ctxt->state);
	type->contModel = xmlAutomataCompile(ctxt->am);
	if(type->contModel == NULL) {
		xmlSchemaPCustomErr(ctxt,
		    XML_SCHEMAP_INTERNAL,
		    WXS_BASIC_CAST type, type->P_Node,
		    "Failed to compile the content model", 0);
	}
	else if(xmlRegexpIsDeterminist(type->contModel) != 1) {
		xmlSchemaPCustomErr(ctxt,
		    XML_SCHEMAP_NOT_DETERMINISTIC,
		    /* XML_SCHEMAS_ERR_NOTDETERMINIST, */
		    WXS_BASIC_CAST type, type->P_Node,
		    "The content model is not determinist", 0);
	}
	else {
#ifdef DEBUG_CONTENT_REGEXP
		xmlGenericError(0, "Content model of %s:\n", type->name);
		xmlRegexpPrint(stderr, type->contModel);
#endif
	}
	ctxt->state = NULL;
	xmlFreeAutomata(ctxt->am);
	ctxt->am = NULL;
}

/**
 * xmlSchemaResolveElementReferences:
 * @elem:  the schema element context
 * @ctxt:  the schema parser context
 *
 * Resolves the references of an element declaration
 * or particle, which has an element declaration as it's
 * term.
 */
static void xmlSchemaResolveElementReferences(xmlSchemaElement * elemDecl, xmlSchemaParserCtxtPtr ctxt)
{
	if(!ctxt || (elemDecl == NULL) || (elemDecl && (elemDecl->flags & XML_SCHEMAS_ELEM_INTERNAL_RESOLVED)))
		return;
	elemDecl->flags |= XML_SCHEMAS_ELEM_INTERNAL_RESOLVED;

	if((elemDecl->subtypes == NULL) && elemDecl->namedType) {
		xmlSchemaTypePtr type;
		/* (type definition) ... otherwise the type definition `resolved`
		 * to by the `actual value` of the type [attribute] ...
		 */
		type = xmlSchemaGetType(ctxt->schema, elemDecl->namedType, elemDecl->namedTypeNs);
		if(type == NULL) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, WXS_BASIC_CAST elemDecl, elemDecl->P_Node,
			    "type", elemDecl->namedType, elemDecl->namedTypeNs, XML_SCHEMA_TYPE_BASIC, "type definition");
		}
		else
			elemDecl->subtypes = type;
	}
	if(elemDecl->substGroup) {
		/*
		 * FIXME TODO: Do we need a new field in _xmlSchemaElement for
		 * substitutionGroup?
		 */
		xmlSchemaElement * substHead = xmlSchemaGetElem(ctxt->schema, elemDecl->substGroup, elemDecl->substGroupNs);
		if(substHead == NULL) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, WXS_BASIC_CAST elemDecl, NULL,
			    "substitutionGroup", elemDecl->substGroup, elemDecl->substGroupNs, XML_SCHEMA_TYPE_ELEMENT, 0);
		}
		else {
			xmlSchemaResolveElementReferences(substHead, ctxt);
			/*
			 * Set the "substitution group affiliation".
			 * NOTE that now we use the "refDecl" field for this.
			 */
			WXS_SUBST_HEAD(elemDecl) = substHead;
			/*
			 * The type definitions is set to:
			 * SPEC "...the {type definition} of the element
			 * declaration `resolved` to by the `actual value`
			 * of the substitutionGroup [attribute], if present"
			 */
			SETIFZ(elemDecl->subtypes, substHead->subtypes);
		}
	}
	/*
	 * SPEC "The definition of anyType serves as the default type definition
	 * for element declarations whose XML representation does not specify one."
	 */
	if((elemDecl->subtypes == NULL) && (elemDecl->namedType == NULL) && (elemDecl->substGroup == NULL))
		elemDecl->subtypes = xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYTYPE);
}

/**
 * xmlSchemaResolveUnionMemberTypes:
 * @ctxt:  the schema parser context
 * @type:  the schema simple type definition
 *
 * Checks and builds the "member type definitions" property of the union
 * simple type. This handles part (1), part (2) is done in
 * xmlSchemaFinishMemberTypeDefinitionsProperty()
 *
 * Returns -1 in case of an internal error, 0 otherwise.
 */
static int xmlSchemaResolveUnionMemberTypes(xmlSchemaParserCtxtPtr ctxt,
    xmlSchemaTypePtr type)
{
	xmlSchemaTypeLinkPtr link, lastLink, newLink;
	xmlSchemaTypePtr memberType;

	/*
	 * SPEC (1) "If the <union> alternative is chosen, then [Definition:]
	 * define the explicit members as the type definitions `resolved`
	 * to by the items in the `actual value` of the memberTypes [attribute],
	 * if any, followed by the type definitions corresponding to the
	 * <simpleType>s among the [children] of <union>, if any."
	 */
	/*
	 * Resolve references.
	 */
	link = type->memberTypes;
	lastLink = NULL;
	while(link) {
		const xmlChar * name = ((xmlSchemaQNameRefPtr)link->type)->name;
		const xmlChar * nsName = ((xmlSchemaQNameRefPtr)link->type)->targetNamespace;
		memberType = xmlSchemaGetType(ctxt->schema, name, nsName);
		if((memberType == NULL) || (!WXS_IS_SIMPLE(memberType))) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, WXS_BASIC_CAST type, type->P_Node, "memberTypes", name, nsName, XML_SCHEMA_TYPE_SIMPLE, 0);
			/*
			 * Remove the member type link.
			 */
			if(lastLink == NULL)
				type->memberTypes = link->next;
			else
				lastLink->next = link->next;
			newLink = link;
			link = link->next;
			SAlloc::F(newLink);
		}
		else {
			link->type = memberType;
			lastLink = link;
			link = link->next;
		}
	}
	/*
	 * Add local simple types,
	 */
	memberType = type->subtypes;
	while(memberType) {
		link = (xmlSchemaTypeLinkPtr)SAlloc::M(sizeof(xmlSchemaTypeLink));
		if(link == NULL) {
			xmlSchemaPErrMemory(ctxt, "allocating a type link", 0);
			return -1;
		}
		link->type = memberType;
		link->next = NULL;
		if(lastLink == NULL)
			type->memberTypes = link;
		else
			lastLink->next = link;
		lastLink = link;
		memberType = memberType->next;
	}
	return 0;
}

/**
 * xmlSchemaIsDerivedFromBuiltInType:
 * @ctxt:  the schema parser context
 * @type:  the type definition
 * @valType: the value type
 *
 *
 * Returns 1 if the type has the given value type, or
 * is derived from such a type.
 */
static int xmlSchemaIsDerivedFromBuiltInType(xmlSchemaTypePtr type, int valType)
{
	if(type == NULL)
		return 0;
	if(WXS_IS_COMPLEX(type))
		return 0;
	if(type->type == XML_SCHEMA_TYPE_BASIC) {
		if(type->builtInType == valType)
			return 1;
		if((type->builtInType == XML_SCHEMAS_ANYSIMPLETYPE) ||
		    (type->builtInType == XML_SCHEMAS_ANYTYPE))
			return 0;
		return(xmlSchemaIsDerivedFromBuiltInType(type->subtypes, valType));
	}
	return(xmlSchemaIsDerivedFromBuiltInType(type->subtypes, valType));
}

#if 0
/**
 * xmlSchemaIsDerivedFromBuiltInType:
 * @ctxt:  the schema parser context
 * @type:  the type definition
 * @valType: the value type
 *
 *
 * Returns 1 if the type has the given value type, or
 * is derived from such a type.
 */
static int xmlSchemaIsUserDerivedFromBuiltInType(xmlSchemaTypePtr type, int valType)
{
	if(type == NULL)
		return 0;
	if(WXS_IS_COMPLEX(type))
		return 0;
	if(type->type == XML_SCHEMA_TYPE_BASIC) {
		if(type->builtInType == valType)
			return 1;
		return 0;
	}
	else
		return(xmlSchemaIsDerivedFromBuiltInType(type->subtypes, valType));

	return 0;
}

static xmlSchemaTypePtr xmlSchemaQueryBuiltInType(xmlSchemaTypePtr type)
{
	if(type == NULL)
		return 0;
	if(WXS_IS_COMPLEX(type))
		return 0;
	if(type->type == XML_SCHEMA_TYPE_BASIC)
		return(type);
	return(xmlSchemaQueryBuiltInType(type->subtypes));
}

#endif

/**
 * xmlSchemaGetPrimitiveType:
 * @type:  the simpleType definition
 *
 * Returns the primitive type of the given type or
 * NULL in case of error.
 */
static xmlSchemaTypePtr xmlSchemaGetPrimitiveType(xmlSchemaTypePtr type)
{
	while(type) {
		/*
		 * Note that anySimpleType is actually not a primitive type
		 * but we need that here.
		 */
		if((type->builtInType == XML_SCHEMAS_ANYSIMPLETYPE) || (type->flags & XML_SCHEMAS_TYPE_BUILTIN_PRIMITIVE))
			return (type);
		type = type->baseType;
	}
	return 0;
}

#if 0
/**
 * xmlSchemaGetBuiltInTypeAncestor:
 * @type:  the simpleType definition
 *
 * Returns the primitive type of the given type or
 * NULL in case of error.
 */
static xmlSchemaTypePtr xmlSchemaGetBuiltInTypeAncestor(xmlSchemaTypePtr type)
{
	if(WXS_IS_LIST(type) || WXS_IS_UNION(type))
		return 0;
	while(type) {
		if(type->type == XML_SCHEMA_TYPE_BASIC)
			return (type);
		type = type->baseType;
	}

	return 0;
}

#endif

/**
 * xmlSchemaCloneWildcardNsConstraints:
 * @ctxt:  the schema parser context
 * @dest:  the destination wildcard
 * @source: the source wildcard
 *
 * Clones the namespace constraints of source
 * and assignes them to dest.
 * Returns -1 on internal error, 0 otherwise.
 */
static int xmlSchemaCloneWildcardNsConstraints(xmlSchemaParserCtxtPtr ctxt,
    xmlSchemaWildcardPtr dest,
    xmlSchemaWildcardPtr source)
{
	xmlSchemaWildcardNsPtr cur, tmp, last;

	if((source == NULL) || (dest == NULL))
		return -1;
	dest->any = source->any;
	cur = source->nsSet;
	last = NULL;
	while(cur) {
		tmp = xmlSchemaNewWildcardNsConstraint(ctxt);
		if(!tmp)
			return -1;
		tmp->value = cur->value;
		if(last == NULL)
			dest->nsSet = tmp;
		else
			last->next = tmp;
		last = tmp;
		cur = cur->next;
	}
	xmlSchemaFreeWildcardNsSet(dest->negNsSet);
	if(source->negNsSet) {
		dest->negNsSet = xmlSchemaNewWildcardNsConstraint(ctxt);
		if(dest->negNsSet == NULL)
			return -1;
		dest->negNsSet->value = source->negNsSet->value;
	}
	else
		dest->negNsSet = NULL;
	return 0;
}

/**
 * xmlSchemaUnionWildcards:
 * @ctxt:  the schema parser context
 * @completeWild:  the first wildcard
 * @curWild: the second wildcard
 *
 * Unions the namespace constraints of the given wildcards.
 * @completeWild will hold the resulting union.
 * Returns a positive error code on failure, -1 in case of an
 * internal error, 0 otherwise.
 */
static int xmlSchemaUnionWildcards(xmlSchemaParserCtxtPtr ctxt, xmlSchemaWildcardPtr completeWild, xmlSchemaWildcardPtr curWild)
{
	xmlSchemaWildcardNsPtr cur, curB, tmp;
	/*
	 * 1 If O1 and O2 are the same value, then that value must be the
	 * value.
	 */
	if((completeWild->any == curWild->any) && ((completeWild->nsSet == NULL) == (curWild->nsSet == NULL)) &&
	    ((completeWild->negNsSet == NULL) == (curWild->negNsSet == NULL))) {
		if((completeWild->negNsSet == NULL) || (completeWild->negNsSet->value == curWild->negNsSet->value)) {
			if(completeWild->nsSet) {
				int found = 0;
				/*
				 * Check equality of sets.
				 */
				cur = completeWild->nsSet;
				while(cur) {
					found = 0;
					curB = curWild->nsSet;
					while(curB != NULL) {
						if(cur->value == curB->value) {
							found = 1;
							break;
						}
						curB = curB->next;
					}
					if(!found)
						break;
					cur = cur->next;
				}
				if(found)
					return 0;
			}
			else
				return 0;
		}
	}
	/*
	 * 2 If either O1 or O2 is any, then any must be the value
	 */
	if(completeWild->any != curWild->any) {
		if(completeWild->any == 0) {
			completeWild->any = 1;
			xmlSchemaFreeWildcardNsSet(completeWild->nsSet);
			completeWild->nsSet = NULL;
			ZFREE(completeWild->negNsSet);
		}
		return 0;
	}
	/*
	 * 3 If both O1 and O2 are sets of (namespace names or `absent`),
	 * then the union of those sets must be the value.
	 */
	if(completeWild->nsSet && curWild->nsSet) {
		int found;
		xmlSchemaWildcardNsPtr start;
		cur = curWild->nsSet;
		start = completeWild->nsSet;
		while(cur) {
			found = 0;
			curB = start;
			while(curB != NULL) {
				if(cur->value == curB->value) {
					found = 1;
					break;
				}
				curB = curB->next;
			}
			if(!found) {
				tmp = xmlSchemaNewWildcardNsConstraint(ctxt);
				if(!tmp)
					return -1;
				tmp->value = cur->value;
				tmp->next = completeWild->nsSet;
				completeWild->nsSet = tmp;
			}
			cur = cur->next;
		}
		return 0;
	}
	/*
	 * 4 If the two are negations of different values (namespace names
	 * or `absent`), then a pair of not and `absent` must be the value.
	 */
	if(completeWild->negNsSet && curWild->negNsSet && (completeWild->negNsSet->value != curWild->negNsSet->value)) {
		completeWild->negNsSet->value = NULL;
		return 0;
	}
	/*
	 * 5.
	 */
	if((completeWild->negNsSet && completeWild->negNsSet->value && curWild->nsSet) || (curWild->negNsSet && curWild->negNsSet->value && completeWild->nsSet)) {
		int nsFound, absentFound = 0;
		if(completeWild->nsSet) {
			cur = completeWild->nsSet;
			curB = curWild->negNsSet;
		}
		else {
			cur = curWild->nsSet;
			curB = completeWild->negNsSet;
		}
		nsFound = 0;
		while(cur) {
			if(cur->value == NULL)
				absentFound = 1;
			else if(cur->value == curB->value)
				nsFound = 1;
			if(nsFound && absentFound)
				break;
			cur = cur->next;
		}

		if(nsFound && absentFound) {
			/*
			 * 5.1 If the set S includes both the negated namespace
			 * name and `absent`, then any must be the value.
			 */
			completeWild->any = 1;
			xmlSchemaFreeWildcardNsSet(completeWild->nsSet);
			completeWild->nsSet = NULL;
			ZFREE(completeWild->negNsSet);
		}
		else if(nsFound && (!absentFound)) {
			/*
			 * 5.2 If the set S includes the negated namespace name
			 * but not `absent`, then a pair of not and `absent` must be the value.
			 */
			xmlSchemaFreeWildcardNsSet(completeWild->nsSet);
			completeWild->nsSet = NULL;
			if(completeWild->negNsSet == NULL) {
				completeWild->negNsSet = xmlSchemaNewWildcardNsConstraint(ctxt);
				if(completeWild->negNsSet == NULL)
					return -1;
			}
			completeWild->negNsSet->value = NULL;
		}
		else if((!nsFound) && absentFound) {
			/*
			 * 5.3 If the set S includes `absent` but not the negated
			 * namespace name, then the union is not expressible.
			 */
			xmlSchemaPErr(ctxt, completeWild->P_Node, XML_SCHEMAP_UNION_NOT_EXPRESSIBLE,
			    "The union of the wilcard is not expressible.\n", 0, 0);
			return(XML_SCHEMAP_UNION_NOT_EXPRESSIBLE);
		}
		else if((!nsFound) && (!absentFound)) {
			/*
			 * 5.4 If the set S does not include either the negated namespace
			 * name or `absent`, then whichever of O1 or O2 is a pair of not
			 * and a namespace name must be the value.
			 */
			if(completeWild->negNsSet == NULL) {
				xmlSchemaFreeWildcardNsSet(completeWild->nsSet);
				completeWild->nsSet = NULL;
				completeWild->negNsSet = xmlSchemaNewWildcardNsConstraint(ctxt);
				if(completeWild->negNsSet == NULL)
					return -1;
				completeWild->negNsSet->value = curWild->negNsSet->value;
			}
		}
		return 0;
	}
	/*
	 * 6.
	 */
	if((completeWild->negNsSet && (completeWild->negNsSet->value == NULL) && curWild->nsSet) ||
	    (curWild->negNsSet && (curWild->negNsSet->value == NULL) && completeWild->nsSet)) {
		cur = completeWild->nsSet ? completeWild->nsSet : curWild->nsSet;
		while(cur) {
			if(cur->value == NULL) {
				//
				// 6.1 If the set S includes `absent`, then any must be the value.
				//
				completeWild->any = 1;
				xmlSchemaFreeWildcardNsSet(completeWild->nsSet);
				completeWild->nsSet = NULL;
				ZFREE(completeWild->negNsSet);
				return 0;
			}
			cur = cur->next;
		}
		if(completeWild->negNsSet == NULL) {
			//
			// 6.2 If the set S does not include `absent`, then a pair of not
			// and `absent` must be the value.
			//
			xmlSchemaFreeWildcardNsSet(completeWild->nsSet);
			completeWild->nsSet = NULL;
			completeWild->negNsSet = xmlSchemaNewWildcardNsConstraint(ctxt);
			if(completeWild->negNsSet == NULL)
				return -1;
			completeWild->negNsSet->value = NULL;
		}
		return 0;
	}
	return 0;
}

/**
 * xmlSchemaIntersectWildcards:
 * @ctxt:  the schema parser context
 * @completeWild:  the first wildcard
 * @curWild: the second wildcard
 *
 * Intersects the namespace constraints of the given wildcards.
 * @completeWild will hold the resulting intersection.
 * Returns a positive error code on failure, -1 in case of an
 * internal error, 0 otherwise.
 */
static int xmlSchemaIntersectWildcards(xmlSchemaParserCtxtPtr ctxt, xmlSchemaWildcardPtr completeWild, xmlSchemaWildcardPtr curWild)
{
	xmlSchemaWildcardNsPtr cur, curB, prev,  tmp;
	/*
	 * 1 If O1 and O2 are the same value, then that value must be the
	 * value.
	 */
	if((completeWild->any == curWild->any) && ((completeWild->nsSet == NULL) == (curWild->nsSet == NULL)) &&
	    ((completeWild->negNsSet == NULL) == (curWild->negNsSet == NULL))) {
		if((completeWild->negNsSet == NULL) || (completeWild->negNsSet->value == curWild->negNsSet->value)) {
			if(completeWild->nsSet) {
				int found = 0;
				/*
				 * Check equality of sets.
				 */
				cur = completeWild->nsSet;
				while(cur) {
					found = 0;
					curB = curWild->nsSet;
					while(curB) {
						if(cur->value == curB->value) {
							found = 1;
							break;
						}
						curB = curB->next;
					}
					if(!found)
						break;
					cur = cur->next;
				}
				if(found)
					return 0;
			}
			else
				return 0;
		}
	}
	/*
	 * 2 If either O1 or O2 is any, then the other must be the value.
	 */
	if((completeWild->any != curWild->any) && (completeWild->any)) {
		if(xmlSchemaCloneWildcardNsConstraints(ctxt, completeWild, curWild) == -1)
			return -1;
		return 0;
	}
	/*
	 * 3 If either O1 or O2 is a pair of not and a value (a namespace
	 * name or `absent`) and the other is a set of (namespace names or
	 * `absent`), then that set, minus the negated value if it was in
	 * the set, minus `absent` if it was in the set, must be the value.
	 */
	if((completeWild->negNsSet && curWild->nsSet) || (curWild->negNsSet && completeWild->nsSet)) {
		const xmlChar * neg;
		if(completeWild->nsSet == NULL) {
			neg = completeWild->negNsSet->value;
			if(xmlSchemaCloneWildcardNsConstraints(ctxt, completeWild, curWild) == -1)
				return -1;
		}
		else
			neg = curWild->negNsSet->value;
		/*
		 * Remove absent and negated.
		 */
		prev = NULL;
		cur = completeWild->nsSet;
		while(cur) {
			if(cur->value == NULL) {
				if(prev == NULL)
					completeWild->nsSet = cur->next;
				else
					prev->next = cur->next;
				SAlloc::F(cur);
				break;
			}
			prev = cur;
			cur = cur->next;
		}
		if(neg) {
			prev = NULL;
			cur = completeWild->nsSet;
			while(cur) {
				if(cur->value == neg) {
					if(prev == NULL)
						completeWild->nsSet = cur->next;
					else
						prev->next = cur->next;
					SAlloc::F(cur);
					break;
				}
				prev = cur;
				cur = cur->next;
			}
		}

		return 0;
	}
	/*
	 * 4 If both O1 and O2 are sets of (namespace names or `absent`),
	 * then the intersection of those sets must be the value.
	 */
	if(completeWild->nsSet && curWild->nsSet) {
		int found;
		cur = completeWild->nsSet;
		prev = NULL;
		while(cur) {
			found = 0;
			curB = curWild->nsSet;
			while(curB) {
				if(cur->value == curB->value) {
					found = 1;
					break;
				}
				curB = curB->next;
			}
			if(!found) {
				if(prev == NULL)
					completeWild->nsSet = cur->next;
				else
					prev->next = cur->next;
				tmp = cur->next;
				SAlloc::F(cur);
				cur = tmp;
				continue;
			}
			prev = cur;
			cur = cur->next;
		}

		return 0;
	}
	/* 5 If the two are negations of different namespace names,
	 * then the intersection is not expressible
	 */
	if(completeWild->negNsSet && curWild->negNsSet &&
	    (completeWild->negNsSet->value != curWild->negNsSet->value) &&
	    completeWild->negNsSet->value && curWild->negNsSet->value) {
		xmlSchemaPErr(ctxt, completeWild->P_Node, XML_SCHEMAP_INTERSECTION_NOT_EXPRESSIBLE,
		    "The intersection of the wilcard is not expressible.\n", NULL, 0);
		return(XML_SCHEMAP_INTERSECTION_NOT_EXPRESSIBLE);
	}
	/*
	 * 6 If the one is a negation of a namespace name and the other
	 * is a negation of `absent`, then the one which is the negation
	 * of a namespace name must be the value.
	 */
	if(completeWild->negNsSet && curWild->negNsSet &&
	    (completeWild->negNsSet->value != curWild->negNsSet->value) &&
	    (completeWild->negNsSet->value == NULL)) {
		completeWild->negNsSet->value =  curWild->negNsSet->value;
	}
	return 0;
}

/**
 * xmlSchemaIsWildcardNsConstraintSubset:
 * @ctxt:  the schema parser context
 * @sub:  the first wildcard
 * @super: the second wildcard
 *
 * Schema Component Constraint: Wildcard Subset (cos-ns-subset)
 *
 * Returns 0 if the namespace constraint of @sub is an intensional
 * subset of @super, 1 otherwise.
 */
static int xmlSchemaCheckCOSNSSubset(xmlSchemaWildcardPtr sub,
    xmlSchemaWildcardPtr super)
{
	/*
	 * 1 super must be any.
	 */
	if(super->any)
		return 0;
	/*
	 * 2.1 sub must be a pair of not and a namespace name or `absent`.
	 * 2.2 super must be a pair of not and the same value.
	 */
	if(sub->negNsSet && super->negNsSet && (sub->negNsSet->value == super->negNsSet->value))
		return 0;
	/*
	 * 3.1 sub must be a set whose members are either namespace names or `absent`.
	 */
	if(sub->nsSet) {
		/*
		 * 3.2.1 super must be the same set or a superset thereof.
		 */
		if(super->nsSet) {
			xmlSchemaWildcardNs * curB;
			int found = 0;
			xmlSchemaWildcardNs * cur = sub->nsSet;
			while(cur) {
				found = 0;
				curB = super->nsSet;
				while(curB != NULL) {
					if(cur->value == curB->value) {
						found = 1;
						break;
					}
					curB = curB->next;
				}
				if(!found)
					return 1;
				cur = cur->next;
			}
			if(found)
				return 0;
		}
		else if(super->negNsSet) {
			/*
			 * 3.2.2 super must be a pair of not and a namespace name or
			 * `absent` and that value must not be in sub's set.
			 */
			xmlSchemaWildcardNsPtr cur = sub->nsSet;
			while(cur) {
				if(cur->value == super->negNsSet->value)
					return 1;
				cur = cur->next;
			}
			return 0;
		}
	}
	return 1;
}

static int xmlSchemaGetEffectiveValueConstraint(xmlSchemaAttributeUsePtr attruse, int * fixed, const xmlChar ** value, xmlSchemaValPtr * val)
{
	*fixed = 0;
	*value = NULL;
	ASSIGN_PTR(val, NULL);
	if(attruse->defValue) {
		*value = attruse->defValue;
		ASSIGN_PTR(val, attruse->defVal);
		if(attruse->flags & XML_SCHEMA_ATTR_USE_FIXED)
			*fixed = 1;
		return 1;
	}
	else if(attruse->attrDecl && attruse->attrDecl->defValue) {
		*value = attruse->attrDecl->defValue;
		ASSIGN_PTR(val, attruse->attrDecl->defVal);
		if(attruse->attrDecl->flags & XML_SCHEMAS_ATTR_FIXED)
			*fixed = 1;
		return 1;
	}
	return 0;
}
/**
 * xmlSchemaCheckCVCWildcardNamespace:
 * @wild:  the wildcard
 * @ns:  the namespace
 *
 * Validation Rule: Wildcard allows Namespace Name
 * (cvc-wildcard-namespace)
 *
 * Returns 0 if the given namespace matches the wildcard,
 * 1 otherwise and -1 on API errors.
 */
static int xmlSchemaCheckCVCWildcardNamespace(xmlSchemaWildcardPtr wild, const xmlChar* ns)
{
	if(wild == NULL)
		return -1;
	if(wild->any)
		return 0;
	else if(wild->nsSet) {
		xmlSchemaWildcardNsPtr cur = wild->nsSet;
		while(cur) {
			if(sstreq(cur->value, ns))
				return 0;
			cur = cur->next;
		}
	}
	else if(wild->negNsSet && ns && (!sstreq(wild->negNsSet->value, ns)))
		return 0;
	return 1;
}

#define XML_SCHEMA_ACTION_DERIVE 0
#define XML_SCHEMA_ACTION_REDEFINE 1
#define WXS_ACTION_STR(a)          ((a) == XML_SCHEMA_ACTION_DERIVE) ? (const xmlChar*)"base" : (const xmlChar*)"redefined"

/*
 * Schema Component Constraint:
 *   Derivation Valid (Restriction, Complex)
 *   derivation-ok-restriction (2) - (4)
 *
 * ATTENTION:
 * In XML Schema 1.1 this will be:
 * Validation Rule:
 *     Checking complex type subsumption (practicalSubsumption) (1, 2 and 3)
 *
 */
static int xmlSchemaCheckDerivationOKRestriction2to4(xmlSchemaParserCtxtPtr pctxt, int action,
    xmlSchemaBasicItem * item, xmlSchemaBasicItem * baseItem, xmlSchemaItemListPtr uses, xmlSchemaItemListPtr baseUses,
    xmlSchemaWildcardPtr wild, xmlSchemaWildcardPtr baseWild)
{
	xmlSchemaAttributeUsePtr cur = NULL, bcur;
	int i, j, found; /* err = 0; */
	const xmlChar * bEffValue;
	int effFixed;
	if(uses) {
		for(i = 0; i < uses->nbItems; i++) {
			cur = (xmlSchemaAttributeUsePtr)uses->items[i];
			found = 0;
			if(baseUses == NULL)
				goto not_found;
			for(j = 0; j < baseUses->nbItems; j++) {
				bcur = (xmlSchemaAttributeUsePtr)baseUses->items[j];
				if((WXS_ATTRUSE_DECL_NAME(cur) == WXS_ATTRUSE_DECL_NAME(bcur)) && (WXS_ATTRUSE_DECL_TNS(cur) == WXS_ATTRUSE_DECL_TNS(bcur))) {
					/*
					 * (2.1) "If there is an attribute use in the {attribute
					 * uses} of the {base type definition} (call this B) whose
					 * {attribute declaration} has the same {name} and {target
					 * namespace}, then  all of the following must be true:"
					 */
					found = 1;
					if((cur->occurs == XML_SCHEMAS_ATTR_USE_OPTIONAL) && (bcur->occurs == XML_SCHEMAS_ATTR_USE_REQUIRED)) {
						xmlChar * str = NULL;
						/*
						 * (2.1.1) "one of the following must be true:"
						 * (2.1.1.1) "B's {required} is false."
						 * (2.1.1.2) "R's {required} is true."
						 */
						xmlSchemaPAttrUseErr4(pctxt, XML_SCHEMAP_DERIVATION_OK_RESTRICTION_2_1_1,
						    WXS_ITEM_NODE(item), item, cur,
						    "The 'optional' attribute use is inconsistent with the corresponding 'required' attribute use of the %s %s",
						    WXS_ACTION_STR(action),
						    xmlSchemaGetComponentDesignation(&str, baseItem),
						    NULL, 0);
						ZFREE(str);
						/* err = pctxt->err; */
					}
					else if(xmlSchemaCheckCOSSTDerivedOK(ACTXT_CAST pctxt, WXS_ATTRUSE_TYPEDEF(cur), WXS_ATTRUSE_TYPEDEF(bcur), 0) != 0) {
						xmlChar * strA = NULL, * strB = NULL, * strC = NULL;

						/*
						 * SPEC (2.1.2) "R's {attribute declaration}'s
						 * {type definition} must be validly derived from
						 * B's {type definition} given the empty set as
						 * defined in Type Derivation OK (Simple) ($3.14.6)."
						 */
						xmlSchemaPAttrUseErr4(pctxt, XML_SCHEMAP_DERIVATION_OK_RESTRICTION_2_1_2, WXS_ITEM_NODE(item), item, cur,
						    "The attribute declaration's %s is not validly derived from the corresponding %s of the attribute declaration in the %s %s",
						    xmlSchemaGetComponentDesignation(&strA, WXS_ATTRUSE_TYPEDEF(cur)),
						    xmlSchemaGetComponentDesignation(&strB, WXS_ATTRUSE_TYPEDEF(bcur)),
						    WXS_ACTION_STR(action), xmlSchemaGetComponentDesignation(&strC, baseItem));
						/* xmlSchemaGetComponentDesignation(&str, baseItem), */
						ZFREE(strA);
						ZFREE(strB);
						ZFREE(strC);
						/* err = pctxt->err; */
					}
					else {
						/*
						 * 2.1.3 [Definition:]  Let the effective value
						 * constraint of an attribute use be its {value
						 * constraint}, if present, otherwise its {attribute
						 * declaration}'s {value constraint} .
						 */
						xmlSchemaGetEffectiveValueConstraint(bcur, &effFixed, &bEffValue, 0);
						/*
						 * 2.1.3 ... one of the following must be true
						 *
						 * 2.1.3.1 B's `effective value constraint` is
						 * `absent` or default.
						 */
						if(bEffValue && (effFixed == 1)) {
							const xmlChar * rEffValue = NULL;
							xmlSchemaGetEffectiveValueConstraint(bcur, &effFixed, &rEffValue, 0);
							/*
							 * 2.1.3.2 R's `effective value constraint` is
							 * fixed with the same string as B's.
							 * MAYBE TODO: Compare the computed values.
							 *       Hmm, it says "same string" so
							 *       string-equality might really be sufficient.
							 */
							if((effFixed == 0) || (!WXS_ARE_DEFAULT_STR_EQUAL(rEffValue, bEffValue))) {
								xmlChar * str = NULL;
								xmlSchemaPAttrUseErr4(pctxt, XML_SCHEMAP_DERIVATION_OK_RESTRICTION_2_1_3, WXS_ITEM_NODE(item), item, cur,
								    "The effective value constraint of the attribute use is inconsistent with its correspondent in the %s %s",
								    WXS_ACTION_STR(action), xmlSchemaGetComponentDesignation(&str, baseItem), 0, 0);
								ZFREE(str);
								/* err = pctxt->err; */
							}
						}
					}
					break;
				}
			}
not_found:
			if(!found) {
				/*
				 * (2.2) "otherwise the {base type definition} must have an
				 * {attribute wildcard} and the {target namespace} of the
				 * R's {attribute declaration} must be `valid` with respect
				 * to that wildcard, as defined in Wildcard allows Namespace
				 * Name ($3.10.4)."
				 */
				if((baseWild == NULL) ||
				    (xmlSchemaCheckCVCWildcardNamespace(baseWild,
						    (WXS_ATTRUSE_DECL(cur))->targetNamespace) != 0)) {
					xmlChar * str = NULL;

					xmlSchemaPAttrUseErr4(pctxt,
					    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_2_2,
					    WXS_ITEM_NODE(item), item, cur,
					    "Neither a matching attribute use, "
					    "nor a matching wildcard exists in the %s %s",
					    WXS_ACTION_STR(action),
					    xmlSchemaGetComponentDesignation(&str, baseItem),
					    NULL, 0);
					ZFREE(str);
					/* err = pctxt->err; */
				}
			}
		}
	}
	/*
	 * SPEC derivation-ok-restriction (3):
	 * (3) "For each attribute use in the {attribute uses} of the {base type
	 * definition} whose {required} is true, there must be an attribute
	 * use with an {attribute declaration} with the same {name} and
	 * {target namespace} as its {attribute declaration} in the {attribute
	 * uses} of the complex type definition itself whose {required} is true.
	 */
	if(baseUses) {
		for(j = 0; j < baseUses->nbItems; j++) {
			bcur = (xmlSchemaAttributeUsePtr)baseUses->items[j];
			if(bcur->occurs != XML_SCHEMAS_ATTR_USE_REQUIRED)
				continue;
			found = 0;
			if(uses) {
				for(i = 0; i < uses->nbItems; i++) {
					cur = (xmlSchemaAttributeUsePtr)uses->items[i];
					if((WXS_ATTRUSE_DECL_NAME(cur) == WXS_ATTRUSE_DECL_NAME(bcur)) && (WXS_ATTRUSE_DECL_TNS(cur) == WXS_ATTRUSE_DECL_TNS(bcur))) {
						found = 1;
						break;
					}
				}
			}
			if(!found) {
				xmlChar * strA = NULL, * strB = NULL;
				xmlSchemaCustomErr4(ACTXT_CAST pctxt, XML_SCHEMAP_DERIVATION_OK_RESTRICTION_3,
				    NULL, item,
				    "A matching attribute use for the "
				    "'required' %s of the %s %s is missing",
				    xmlSchemaGetComponentDesignation(&strA, bcur),
				    WXS_ACTION_STR(action), xmlSchemaGetComponentDesignation(&strB, baseItem), 0);
				ZFREE(strA);
				ZFREE(strB);
			}
		}
	}
	/*
	 * derivation-ok-restriction (4)
	 */
	if(wild) {
		/*
		 * (4) "If there is an {attribute wildcard}, all of the
		 * following must be true:"
		 */
		if(baseWild == NULL) {
			xmlChar * str = NULL;

			/*
			 * (4.1) "The {base type definition} must also have one."
			 */
			xmlSchemaCustomErr4(ACTXT_CAST pctxt,
			    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_4_1,
			    NULL, item,
			    "The %s has an attribute wildcard, "
			    "but the %s %s '%s' does not have one",
			    WXS_ITEM_TYPE_NAME(item),
			    WXS_ACTION_STR(action),
			    WXS_ITEM_TYPE_NAME(baseItem),
			    xmlSchemaGetComponentQName(&str, baseItem));
			ZFREE(str);
			return(pctxt->err);
		}
		else if((baseWild->any == 0) &&
		    xmlSchemaCheckCOSNSSubset(wild, baseWild)) {
			xmlChar * str = NULL;
			/*
			 * (4.2) "The complex type definition's {attribute wildcard}'s
			 * {namespace constraint} must be a subset of the {base type
			 * definition}'s {attribute wildcard}'s {namespace constraint},
			 * as defined by Wildcard Subset ($3.10.6)."
			 */
			xmlSchemaCustomErr4(ACTXT_CAST pctxt,
			    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_4_2,
			    NULL, item,
			    "The attribute wildcard is not a valid "
			    "subset of the wildcard in the %s %s '%s'",
			    WXS_ACTION_STR(action),
			    WXS_ITEM_TYPE_NAME(baseItem),
			    xmlSchemaGetComponentQName(&str, baseItem),
			    NULL);
			ZFREE(str);
			return(pctxt->err);
		}
		/* 4.3 Unless the {base type definition} is the `ur-type
		 * definition`, the complex type definition's {attribute
		 * wildcard}'s {process contents} must be identical to or
		 * stronger than the {base type definition}'s {attribute
		 * wildcard}'s {process contents}, where strict is stronger
		 * than lax is stronger than skip.
		 */
		if((!WXS_IS_ANYTYPE(baseItem)) && (wild->processContents < baseWild->processContents)) {
			xmlChar * str = NULL;
			xmlSchemaCustomErr4(ACTXT_CAST pctxt, XML_SCHEMAP_DERIVATION_OK_RESTRICTION_4_3, NULL, baseItem,
			    "The {process contents} of the attribute wildcard is weaker than the one in the %s %s '%s'",
			    WXS_ACTION_STR(action), WXS_ITEM_TYPE_NAME(baseItem), xmlSchemaGetComponentQName(&str, baseItem),
			    NULL);
			ZFREE(str);
			return(pctxt->err);
		}
	}
	return 0;
}

static int xmlSchemaExpandAttributeGroupRefs(xmlSchemaParserCtxtPtr pctxt, xmlSchemaBasicItem * item, xmlSchemaWildcardPtr * completeWild,
    xmlSchemaItemListPtr list, xmlSchemaItemListPtr prohibs);
/**
 * xmlSchemaFixupTypeAttributeUses:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 *
 * Builds the wildcard and the attribute uses on the given complex type.
 * Returns -1 if an internal error occurs, 0 otherwise.
 *
 * ATTENTION TODO: Experimantally this uses pointer comparisons for
 * strings, so recheck this if we start to hardcode some schemata, since
 * they might not be in the same dict.
 * NOTE: It is allowed to "extend" the xs:anyType type.
 */
static int xmlSchemaFixupTypeAttributeUses(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	xmlSchemaTypePtr baseType = NULL;
	xmlSchemaAttributeUsePtr use;
	xmlSchemaItemListPtr uses, baseUses, prohibs = NULL;
	if(type->baseType == NULL) {
		PERROR_INT("xmlSchemaFixupTypeAttributeUses", "no base type");
		return -1;
	}
	baseType = type->baseType;
	if(WXS_IS_TYPE_NOT_FIXED(baseType))
		if(xmlSchemaTypeFixup(baseType, ACTXT_CAST pctxt) == -1)
			return -1;
	uses = (xmlSchemaItemListPtr)type->attrUses;
	baseUses = (xmlSchemaItemListPtr)baseType->attrUses;
	/*
	 * Expand attribute group references. And build the 'complete'
	 * wildcard, i.e. intersect multiple wildcards.
	 * Move attribute prohibitions into a separate list.
	 */
	if(uses) {
		if(WXS_IS_RESTRICTION(type)) {
			/*
			 * This one will transfer all attr. prohibitions
			 * into pctxt->attrProhibs.
			 */
			if(xmlSchemaExpandAttributeGroupRefs(pctxt, WXS_BASIC_CAST type, &(type->attributeWildcard), uses, pctxt->attrProhibs) == -1) {
				PERROR_INT("xmlSchemaFixupTypeAttributeUses", "failed to expand attributes");
			}
			if(pctxt->attrProhibs->nbItems != 0)
				prohibs = pctxt->attrProhibs;
		}
		else {
			if(xmlSchemaExpandAttributeGroupRefs(pctxt, WXS_BASIC_CAST type, &(type->attributeWildcard), uses, NULL) == -1) {
				PERROR_INT("xmlSchemaFixupTypeAttributeUses", "failed to expand attributes");
			}
		}
	}
	/*
	 * Inherit the attribute uses of the base type.
	 */
	if(baseUses) {
		int i, j;
		xmlSchemaAttributeUseProhibPtr pro;
		if(WXS_IS_RESTRICTION(type)) {
			xmlSchemaAttributeUsePtr tmp;
			int usesCount = uses ? uses->nbItems : 0;
			// Restriction
			for(i = 0; i < baseUses->nbItems; i++) {
				use = (xmlSchemaAttributeUsePtr)baseUses->items[i];
				if(prohibs) {
					/*
					 * Filter out prohibited uses.
					 */
					for(j = 0; j < prohibs->nbItems; j++) {
						pro = (xmlSchemaAttributeUseProhibPtr)prohibs->items[j];
						if((WXS_ATTRUSE_DECL_NAME(use) == pro->name) && (WXS_ATTRUSE_DECL_TNS(use) == pro->targetNamespace)) {
							goto inherit_next;
						}
					}
				}
				if(usesCount) {
					/*
					 * Filter out existing uses.
					 */
					for(j = 0; j < usesCount; j++) {
						tmp = (xmlSchemaAttributeUsePtr)uses->items[j];
						if((WXS_ATTRUSE_DECL_NAME(use) == WXS_ATTRUSE_DECL_NAME(tmp)) && (WXS_ATTRUSE_DECL_TNS(use) == WXS_ATTRUSE_DECL_TNS(tmp))) {
							goto inherit_next;
						}
					}
				}
				if(uses == NULL) {
					type->attrUses = xmlSchemaItemListCreate();
					if(type->attrUses == NULL)
						goto exit_failure;
					uses = (xmlSchemaItemListPtr)type->attrUses;
				}
				xmlSchemaItemListAddSize(uses, 2, use);
inherit_next: {}
			}
		}
		else {
			/* Extension. */
			for(i = 0; i < baseUses->nbItems; i++) {
				use = (xmlSchemaAttributeUsePtr)baseUses->items[i];
				if(uses == NULL) {
					type->attrUses = xmlSchemaItemListCreate();
					if(type->attrUses == NULL)
						goto exit_failure;
					uses = (xmlSchemaItemListPtr)type->attrUses;
				}
				xmlSchemaItemListAddSize(uses, baseUses->nbItems, use);
			}
		}
	}
	/*
	 * Shrink attr. uses.
	 */
	if(uses) {
		if(uses->nbItems == 0) {
			xmlSchemaItemListFree(uses);
			type->attrUses = NULL;
		}
		/*
		 * @todo We could shrink the size of the array
		 * to fit the actual number of items.
		 */
	}
	/*
	 * Compute the complete wildcard.
	 */
	if(WXS_IS_EXTENSION(type)) {
		if(baseType->attributeWildcard) {
			/*
			 * (3.2.2.1) "If the `base wildcard` is non-`absent`, then
			 * the appropriate case among the following:"
			 */
			if(type->attributeWildcard) {
				/*
				 * Union the complete wildcard with the base wildcard.
				 * SPEC {attribute wildcard}
				 * (3.2.2.1.2) "otherwise a wildcard whose {process contents}
				 * and {annotation} are those of the `complete wildcard`,
				 * and whose {namespace constraint} is the intensional union
				 * of the {namespace constraint} of the `complete wildcard`
				 * and of the `base wildcard`, as defined in Attribute
				 * Wildcard Union ($3.10.6)."
				 */
				if(xmlSchemaUnionWildcards(pctxt, type->attributeWildcard,
					    baseType->attributeWildcard) == -1)
					goto exit_failure;
			}
			else {
				/*
				 * (3.2.2.1.1) "If the `complete wildcard` is `absent`,
				 * then the `base wildcard`."
				 */
				type->attributeWildcard = baseType->attributeWildcard;
			}
		}
		else {
			/*
			 * (3.2.2.2) "otherwise (the `base wildcard` is `absent`) the
			 * `complete wildcard`"
			 * NOOP
			 */
		}
	}
	else {
		/*
		 * SPEC {attribute wildcard}
		 * (3.1) "If the <restriction> alternative is chosen, then the
		 * `complete wildcard`;"
		 * NOOP
		 */
	}

	return 0;

exit_failure:
	return -1;
}

/**
 * xmlSchemaTypeFinalContains:
 * @schema:  the schema
 * @type:  the type definition
 * @final: the final
 *
 * Evaluates if a type definition contains the given "final".
 * This does take "finalDefault" into account as well.
 *
 * Returns 1 if the type does containt the given "final",
 * 0 otherwise.
 */
static int xmlSchemaTypeFinalContains(xmlSchemaTypePtr type, int final)
{
	if(type == NULL)
		return 0;
	if(type->flags & final)
		return 1;
	else
		return 0;
}

/**
 * xmlSchemaGetUnionSimpleTypeMemberTypes:
 * @type:  the Union Simple Type
 *
 * Returns a list of member types of @type if existing,
 * returns NULL otherwise.
 */
static xmlSchemaTypeLinkPtr xmlSchemaGetUnionSimpleTypeMemberTypes(xmlSchemaTypePtr type)
{
	while(type && (type->type == XML_SCHEMA_TYPE_SIMPLE)) {
		if(type->memberTypes)
			return (type->memberTypes);
		else
			type = type->baseType;
	}
	return 0;
}

/**
 * xmlSchemaGetParticleTotalRangeMin:
 * @particle: the particle
 *
 * Schema Component Constraint: Effective Total Range
 * (all and sequence) + (choice)
 *
 * Returns the minimun Effective Total Range.
 */
static int xmlSchemaGetParticleTotalRangeMin(xmlSchemaParticlePtr particle)
{
	if((particle->children == NULL) || (particle->minOccurs == 0))
		return 0;
	if(particle->children->type == XML_SCHEMA_TYPE_CHOICE) {
		int min = -1, cur;
		xmlSchemaParticlePtr part = (xmlSchemaParticlePtr)particle->children->children;
		if(part == NULL)
			return 0;
		while(part) {
			if((part->children->type == XML_SCHEMA_TYPE_ELEMENT) || (part->children->type == XML_SCHEMA_TYPE_ANY))
				cur = part->minOccurs;
			else
				cur = xmlSchemaGetParticleTotalRangeMin(part);
			if(cur == 0)
				return 0;
			if((min > cur) || (min == -1))
				min = cur;
			part = (xmlSchemaParticlePtr)part->next;
		}
		return (particle->minOccurs * min);
	}
	else {
		/* <all> and <sequence> */
		int sum = 0;
		xmlSchemaParticlePtr part = (xmlSchemaParticlePtr)particle->children->children;
		if(part == NULL)
			return 0;
		do {
			if((part->children->type == XML_SCHEMA_TYPE_ELEMENT) || (part->children->type == XML_SCHEMA_TYPE_ANY))
				sum += part->minOccurs;
			else
				sum += xmlSchemaGetParticleTotalRangeMin(part);
			part = (xmlSchemaParticlePtr)part->next;
		} while(part);
		return (particle->minOccurs * sum);
	}
}

#if 0
/**
 * xmlSchemaGetParticleTotalRangeMax:
 * @particle: the particle
 *
 * Schema Component Constraint: Effective Total Range
 * (all and sequence) + (choice)
 *
 * Returns the maximum Effective Total Range.
 */
static int xmlSchemaGetParticleTotalRangeMax(xmlSchemaParticlePtr particle)
{
	if((particle->children == NULL) ||
	    (particle->children->children == NULL))
		return 0;
	if(particle->children->type == XML_SCHEMA_TYPE_CHOICE) {
		int max = -1, cur;
		xmlSchemaParticlePtr part =
		    (xmlSchemaParticlePtr)particle->children->children;

		for(; part; part = (xmlSchemaParticlePtr)part->next) {
			if(part->children == NULL)
				continue;
			if((part->children->type == XML_SCHEMA_TYPE_ELEMENT) ||
			    (part->children->type == XML_SCHEMA_TYPE_ANY))
				cur = part->maxOccurs;
			else
				cur = xmlSchemaGetParticleTotalRangeMax(part);
			if(cur == UNBOUNDED)
				return (UNBOUNDED);
			if((max < cur) || (max == -1))
				max = cur;
		}
		/* @todo Handle overflows? */
		return (particle->maxOccurs * max);
	}
	else {
		/* <all> and <sequence> */
		int sum = 0, cur;
		xmlSchemaParticlePtr part =
		    (xmlSchemaParticlePtr)particle->children->children;

		for(; part; part = (xmlSchemaParticlePtr)part->next) {
			if(part->children == NULL)
				continue;
			if((part->children->type == XML_SCHEMA_TYPE_ELEMENT) ||
			    (part->children->type == XML_SCHEMA_TYPE_ANY))
				cur = part->maxOccurs;
			else
				cur = xmlSchemaGetParticleTotalRangeMax(part);
			if(cur == UNBOUNDED)
				return (UNBOUNDED);
			if((cur > 0) && (particle->maxOccurs == UNBOUNDED))
				return (UNBOUNDED);
			sum += cur;
		}
		/* @todo Handle overflows? */
		return (particle->maxOccurs * sum);
	}
}

#endif

/**
 * xmlSchemaIsParticleEmptiable:
 * @particle: the particle
 *
 * Schema Component Constraint: Particle Emptiable
 * Checks whether the given particle is emptiable.
 *
 * Returns 1 if emptiable, 0 otherwise.
 */
static int xmlSchemaIsParticleEmptiable(xmlSchemaParticlePtr particle)
{
	/*
	 * SPEC (1) "Its {min occurs} is 0."
	 */
	if((particle == NULL) || (particle->minOccurs == 0) ||
	    (particle->children == NULL))
		return 1;
	/*
	 * SPEC (2) "Its {term} is a group and the minimum part of the
	 * effective total range of that group, [...] is 0."
	 */
	if(WXS_IS_MODEL_GROUP(particle->children)) {
		if(xmlSchemaGetParticleTotalRangeMin(particle) == 0)
			return 1;
	}
	return 0;
}

/**
 * xmlSchemaCheckCOSSTDerivedOK:
 * @actxt: a context
 * @type:  the derived simple type definition
 * @baseType:  the base type definition
 * @subset: the subset of ('restriction', ect.)
 *
 * Schema Component Constraint:
 * Type Derivation OK (Simple) (cos-st-derived-OK)
 *
 * Checks wheter @type can be validly
 * derived from @baseType.
 *
 * Returns 0 on success, an positive error code otherwise.
 */
static int xmlSchemaCheckCOSSTDerivedOK(xmlSchemaAbstractCtxtPtr actxt,
    xmlSchemaTypePtr type,
    xmlSchemaTypePtr baseType,
    int subset)
{
	/*
	 * 1 They are the same type definition.
	 * @todo The identy check might have to be more complex than this.
	 */
	if(type == baseType)
		return 0;
	/*
	 * 2.1 restriction is not in the subset, or in the {final}
	 * of its own {base type definition};
	 *
	 * NOTE that this will be used also via "xsi:type".
	 *
	 * @todo Revise this, it looks strange. How can the "type"
	 * not be fixed or *in* fixing?
	 */
	if(WXS_IS_TYPE_NOT_FIXED(type))
		if(xmlSchemaTypeFixup(type, actxt) == -1)
			return -1;
	if(WXS_IS_TYPE_NOT_FIXED(baseType))
		if(xmlSchemaTypeFixup(baseType, actxt) == -1)
			return -1;
	if((subset & SUBSET_RESTRICTION) ||
	    (xmlSchemaTypeFinalContains(type->baseType,
			    XML_SCHEMAS_TYPE_FINAL_RESTRICTION))) {
		return (XML_SCHEMAP_COS_ST_DERIVED_OK_2_1);
	}
	/* 2.2 */
	if(type->baseType == baseType) {
		/*
		 * 2.2.1 D's `base type definition` is B.
		 */
		return 0;
	}
	/*
	 * 2.2.2 D's `base type definition` is not the `ur-type definition`
	 * and is validly derived from B given the subset, as defined by this
	 * constraint.
	 */
	if((!WXS_IS_ANYTYPE(type->baseType)) &&
	    (xmlSchemaCheckCOSSTDerivedOK(actxt, type->baseType,
			    baseType, subset) == 0)) {
		return 0;
	}
	/*
	 * 2.2.3 D's {variety} is list or union and B is the `simple ur-type
	 * definition`.
	 */
	if(WXS_IS_ANY_SIMPLE_TYPE(baseType) &&
	    (WXS_IS_LIST(type) || WXS_IS_UNION(type))) {
		return 0;
	}
	/*
	 * 2.2.4 B's {variety} is union and D is validly derived from a type
	 * definition in B's {member type definitions} given the subset, as
	 * defined by this constraint.
	 *
	 * NOTE: This seems not to involve built-in types, since there is no
	 * built-in Union Simple Type.
	 */
	if(WXS_IS_UNION(baseType)) {
		xmlSchemaTypeLinkPtr cur;

		cur = baseType->memberTypes;
		while(cur) {
			if(WXS_IS_TYPE_NOT_FIXED(cur->type))
				if(xmlSchemaTypeFixup(cur->type, actxt) == -1)
					return -1;
			if(xmlSchemaCheckCOSSTDerivedOK(actxt,
				    type, cur->type, subset) == 0) {
				/*
				 * It just has to be validly derived from at least one
				 * member-type.
				 */
				return 0;
			}
			cur = cur->next;
		}
	}
	return (XML_SCHEMAP_COS_ST_DERIVED_OK_2_2);
}

/**
 * xmlSchemaCheckTypeDefCircularInternal:
 * @pctxt:  the schema parser context
 * @ctxtType:  the type definition
 * @ancestor: an ancestor of @ctxtType
 *
 * Checks st-props-correct (2) + ct-props-correct (3).
 * Circular type definitions are not allowed.
 *
 * Returns XML_SCHEMAP_ST_PROPS_CORRECT_2 if the given type is
 * circular, 0 otherwise.
 */
static int xmlSchemaCheckTypeDefCircularInternal(xmlSchemaParserCtxtPtr pctxt,
    xmlSchemaTypePtr ctxtType,
    xmlSchemaTypePtr ancestor)
{
	int ret;

	if((ancestor == NULL) || (ancestor->type == XML_SCHEMA_TYPE_BASIC))
		return 0;

	if(ctxtType == ancestor) {
		xmlSchemaPCustomErr(pctxt,
		    XML_SCHEMAP_ST_PROPS_CORRECT_2,
		    WXS_BASIC_CAST ctxtType, WXS_ITEM_NODE(ctxtType),
		    "The definition is circular", 0);
		return (XML_SCHEMAP_ST_PROPS_CORRECT_2);
	}
	if(ancestor->flags & XML_SCHEMAS_TYPE_MARKED) {
		/*
		 * Avoid inifinite recursion on circular types not yet checked.
		 */
		return 0;
	}
	ancestor->flags |= XML_SCHEMAS_TYPE_MARKED;
	ret = xmlSchemaCheckTypeDefCircularInternal(pctxt, ctxtType,
	    ancestor->baseType);
	ancestor->flags ^= XML_SCHEMAS_TYPE_MARKED;
	return ret;
}

/**
 * xmlSchemaCheckTypeDefCircular:
 * @item:  the complex/simple type definition
 * @ctxt:  the parser context
 * @name:  the name
 *
 * Checks for circular type definitions.
 */
static void xmlSchemaCheckTypeDefCircular(xmlSchemaTypePtr item, xmlSchemaParserCtxtPtr ctxt)
{
	if((item == NULL) || (item->type == XML_SCHEMA_TYPE_BASIC) || (item->baseType == NULL))
		return;
	xmlSchemaCheckTypeDefCircularInternal(ctxt, item, item->baseType);
}
/*
 * Simple Type Definition Representation OK (src-simple-type) 4
 *
 * "4 Circular union type definition is disallowed. That is, if the
 * <union> alternative is chosen, there must not be any entries in the
 * memberTypes [attribute] at any depth which resolve to the component
 * corresponding to the <simpleType>."
 *
 * Note that this should work on the *representation* of a component,
 * thus assumes any union types in the member types not being yet
 * substituted. At this stage we need the variety of the types
 * to be already computed.
 */
static int xmlSchemaCheckUnionTypeDefCircularRecur(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr ctxType, xmlSchemaTypeLinkPtr members)
{
	xmlSchemaTypePtr memberType;
	xmlSchemaTypeLinkPtr member = members;
	while(member) {
		memberType = member->type;
		while(memberType && (memberType->type != XML_SCHEMA_TYPE_BASIC)) {
			if(memberType == ctxType) {
				xmlSchemaPCustomErr(pctxt,
				    XML_SCHEMAP_SRC_SIMPLE_TYPE_4,
				    WXS_BASIC_CAST ctxType, NULL,
				    "The union type definition is circular", 0);
				return (XML_SCHEMAP_SRC_SIMPLE_TYPE_4);
			}
			if((WXS_IS_UNION(memberType)) &&
			    ((memberType->flags & XML_SCHEMAS_TYPE_MARKED) == 0)) {
				int res;
				memberType->flags |= XML_SCHEMAS_TYPE_MARKED;
				res = xmlSchemaCheckUnionTypeDefCircularRecur(pctxt,
				    ctxType,
				    xmlSchemaGetUnionSimpleTypeMemberTypes(memberType));
				memberType->flags ^= XML_SCHEMAS_TYPE_MARKED;
				if(res != 0)
					return res;
			}
			memberType = memberType->baseType;
		}
		member = member->next;
	}
	return 0;
}

static int xmlSchemaCheckUnionTypeDefCircular(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	if(!WXS_IS_UNION(type))
		return 0;
	return xmlSchemaCheckUnionTypeDefCircularRecur(pctxt, type, type->memberTypes);
}

/**
 * xmlSchemaResolveTypeReferences:
 * @item:  the complex/simple type definition
 * @ctxt:  the parser context
 * @name:  the name
 *
 * Resolvese type definition references
 */
static void xmlSchemaResolveTypeReferences(xmlSchemaTypePtr typeDef, xmlSchemaParserCtxtPtr ctxt)
{
	if(typeDef == NULL)
		return;
	/*
	 * Resolve the base type.
	 */
	if(typeDef->baseType == NULL) {
		typeDef->baseType = xmlSchemaGetType(ctxt->schema,
		    typeDef->base, typeDef->baseNs);
		if(typeDef->baseType == NULL) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, WXS_BASIC_CAST typeDef, typeDef->P_Node, "base", typeDef->base, typeDef->baseNs, XML_SCHEMA_TYPE_SIMPLE, 0);
			return;
		}
	}
	if(WXS_IS_SIMPLE(typeDef)) {
		if(WXS_IS_UNION(typeDef)) {
			/*
			 * Resolve the memberTypes.
			 */
			xmlSchemaResolveUnionMemberTypes(ctxt, typeDef);
			return;
		}
		else if(WXS_IS_LIST(typeDef)) {
			/*
			 * Resolve the itemType.
			 */
			if((typeDef->subtypes == NULL) && typeDef->base) {
				typeDef->subtypes = xmlSchemaGetType(ctxt->schema, typeDef->base, typeDef->baseNs);
				if((typeDef->subtypes == NULL) || (!WXS_IS_SIMPLE(typeDef->subtypes))) {
					typeDef->subtypes = NULL;
					xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, WXS_BASIC_CAST typeDef, typeDef->P_Node,
					    "itemType", typeDef->base, typeDef->baseNs, XML_SCHEMA_TYPE_SIMPLE, 0);
				}
			}
			return;
		}
	}
	/*
	 * The ball of letters below means, that if we have a particle
	 * which has a QName-helper component as its {term}, we want
	 * to resolve it...
	 */
	else if(WXS_TYPE_CONTENTTYPE(typeDef) && ((WXS_TYPE_CONTENTTYPE(typeDef))->type == XML_SCHEMA_TYPE_PARTICLE) &&
	    WXS_TYPE_PARTICLE_TERM(typeDef) && ((WXS_TYPE_PARTICLE_TERM(typeDef))->type == XML_SCHEMA_EXTRA_QNAMEREF)) {
		xmlSchemaQNameRefPtr ref = WXS_QNAME_CAST WXS_TYPE_PARTICLE_TERM(typeDef);
		xmlSchemaModelGroupDefPtr groupDef;
		/*
		 * URGENT TODO: Test this.
		 */
		WXS_TYPE_PARTICLE_TERM(typeDef) = NULL;
		/*
		 * Resolve the MG definition reference.
		 */
		groupDef = WXS_MODEL_GROUPDEF_CAST xmlSchemaGetNamedComponent(ctxt->schema, ref->itemType, ref->name, ref->targetNamespace);
		if(groupDef == NULL) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, NULL, WXS_ITEM_NODE(WXS_TYPE_PARTICLE(typeDef)),
			    "ref", ref->name, ref->targetNamespace, ref->itemType, NULL);
			/* Remove the particle. */
			WXS_TYPE_CONTENTTYPE(typeDef) = NULL;
		}
		else if(WXS_MODELGROUPDEF_MODEL(groupDef) == NULL)
			/* Remove the particle. */
			WXS_TYPE_CONTENTTYPE(typeDef) = NULL;
		else {
			/*
			 * Assign the MG definition's {model group} to the
			 * particle's {term}.
			 */
			WXS_TYPE_PARTICLE_TERM(typeDef) = WXS_MODELGROUPDEF_MODEL(groupDef);
			if(WXS_MODELGROUPDEF_MODEL(groupDef)->type == XML_SCHEMA_TYPE_ALL) {
				/*
				 * SPEC cos-all-limited (1.2)
				 * "1.2 the {term} property of a particle with
				 * {max occurs}=1 which is part of a pair which constitutes
				 * the {content type} of a complex type definition."
				 */
				if((WXS_TYPE_PARTICLE(typeDef))->maxOccurs != 1) {
					xmlSchemaCustomErr(ACTXT_CAST ctxt,
					    /* @todo error code */
					    XML_SCHEMAP_COS_ALL_LIMITED,
					    WXS_ITEM_NODE(WXS_TYPE_PARTICLE(typeDef)), NULL,
					    "The particle's {max occurs} must be 1, since the reference resolves to an 'all' model group",
					    NULL, 0);
				}
			}
		}
	}
}

/**
 * xmlSchemaCheckSTPropsCorrect:
 * @ctxt:  the schema parser context
 * @type:  the simple type definition
 *
 * Checks st-props-correct.
 *
 * Returns 0 if the properties are correct,
 * if not, a positive error code and -1 on internal
 * errors.
 */
static int xmlSchemaCheckSTPropsCorrect(xmlSchemaParserCtxtPtr ctxt, xmlSchemaTypePtr type)
{
	xmlSchemaTypePtr baseType = type->baseType;
	xmlChar * str = NULL;
	/* STATE: error funcs converted. */
	/*
	 * Schema Component Constraint: Simple Type Definition Properties Correct
	 *
	 * NOTE: This is somehow redundant, since we actually built a simple type
	 * to have all the needed information; this acts as an self test.
	 */
	/* Base type: If the datatype has been `derived` by `restriction`
	 * then the Simple Type Definition component from which it is `derived`,
	 * otherwise the Simple Type Definition for anySimpleType ($4.1.6).
	 */
	if(baseType == NULL) {
		/*
		 * @todo Think about: "modulo the impact of Missing
		 * Sub-components ($5.3)."
		 */
		xmlSchemaPCustomErr(ctxt,
		    XML_SCHEMAP_ST_PROPS_CORRECT_1,
		    WXS_BASIC_CAST type, NULL,
		    "No base type existent", 0);
		return (XML_SCHEMAP_ST_PROPS_CORRECT_1);
	}
	if(!WXS_IS_SIMPLE(baseType)) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_ST_PROPS_CORRECT_1, WXS_BASIC_CAST type, NULL,
		    "The base type '%s' is not a simple type", xmlSchemaGetComponentQName(&str, baseType));
		ZFREE(str);
		return (XML_SCHEMAP_ST_PROPS_CORRECT_1);
	}
	if((WXS_IS_LIST(type) || WXS_IS_UNION(type)) && (WXS_IS_RESTRICTION(type) == 0) && ((!WXS_IS_ANY_SIMPLE_TYPE(baseType)) && (baseType->type != XML_SCHEMA_TYPE_SIMPLE))) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_ST_PROPS_CORRECT_1, WXS_BASIC_CAST type, NULL,
		    "A type, derived by list or union, must have the simple ur-type definition as base type, not '%s'", xmlSchemaGetComponentQName(&str, baseType));
		ZFREE(str);
		return (XML_SCHEMAP_ST_PROPS_CORRECT_1);
	}
	/*
	 * Variety: One of {atomic, list, union}.
	 */
	if((!WXS_IS_ATOMIC(type)) && (!WXS_IS_UNION(type)) && (!WXS_IS_LIST(type))) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_ST_PROPS_CORRECT_1, WXS_BASIC_CAST type, NULL, "The variety is absent", 0);
		return (XML_SCHEMAP_ST_PROPS_CORRECT_1);
	}
	/* @todo Finish this. Hmm, is this finished? */

	/*
	 * 3 The {final} of the {base type definition} must not contain restriction.
	 */
	if(xmlSchemaTypeFinalContains(baseType, XML_SCHEMAS_TYPE_FINAL_RESTRICTION)) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_ST_PROPS_CORRECT_3, WXS_BASIC_CAST type, NULL,
		    "The 'final' of its base type '%s' must not contain 'restriction'", xmlSchemaGetComponentQName(&str, baseType));
		ZFREE(str);
		return (XML_SCHEMAP_ST_PROPS_CORRECT_3);
	}
	/*
	 * 2 All simple type definitions must be derived ultimately from the `simple
	 * ur-type definition` (so circular definitions are disallowed). That is, it
	 * must be possible to reach a built-in primitive datatype or the `simple
	 * ur-type definition` by repeatedly following the {base type definition}.
	 *
	 * NOTE: this is done in xmlSchemaCheckTypeDefCircular().
	 */
	return 0;
}

/**
 * xmlSchemaCheckCOSSTRestricts:
 * @ctxt:  the schema parser context
 * @type:  the simple type definition
 *
 * Schema Component Constraint:
 * Derivation Valid (Restriction, Simple) (cos-st-restricts)

 * Checks if the given @type (simpleType) is derived validly by restriction.
 * STATUS:
 *
 * Returns -1 on internal errors, 0 if the type is validly derived,
 * a positive error code otherwise.
 */
static int xmlSchemaCheckCOSSTRestricts(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	xmlChar * str = NULL;
	if(type->type != XML_SCHEMA_TYPE_SIMPLE) {
		PERROR_INT("xmlSchemaCheckCOSSTRestricts", "given type is not a user-derived simpleType");
		return -1;
	}
	if(WXS_IS_ATOMIC(type)) {
		xmlSchemaTypePtr primitive;
		/*
		 * 1.1 The {base type definition} must be an atomic simple
		 * type definition or a built-in primitive datatype.
		 */
		if(!WXS_IS_ATOMIC(type->baseType)) {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_1_1,
			    WXS_BASIC_CAST type, NULL, "The base type '%s' is not an atomic simple type",
			    xmlSchemaGetComponentQName(&str, type->baseType));
			ZFREE(str);
			return (XML_SCHEMAP_COS_ST_RESTRICTS_1_1);
		}
		/* 1.2 The {final} of the {base type definition} must not contain
		 * restriction.
		 */
		/* OPTIMIZE TODO : This is already done in xmlSchemaCheckStPropsCorrect */
		if(xmlSchemaTypeFinalContains(type->baseType, XML_SCHEMAS_TYPE_FINAL_RESTRICTION)) {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_1_2, WXS_BASIC_CAST type, NULL,
			    "The final of its base type '%s' must not contain 'restriction'", xmlSchemaGetComponentQName(&str, type->baseType));
			ZFREE(str);
			return (XML_SCHEMAP_COS_ST_RESTRICTS_1_2);
		}
		/*
		 * 1.3.1 DF must be an allowed constraining facet for the {primitive
		 * type definition}, as specified in the appropriate subsection of 3.2
		 * Primitive datatypes.
		 */
		if(type->facets) {
			xmlSchemaFacetPtr facet;
			int ok = 1;
			primitive = xmlSchemaGetPrimitiveType(type);
			if(primitive == NULL) {
				PERROR_INT("xmlSchemaCheckCOSSTRestricts", "failed to get primitive type");
				return -1;
			}
			facet = type->facets;
			do {
				if(xmlSchemaIsBuiltInTypeFacet(primitive, facet->type) == 0) {
					ok = 0;
					xmlSchemaPIllegalFacetAtomicErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_1_3_1, type, primitive, facet);
				}
				facet = facet->next;
			} while(facet);
			if(ok == 0)
				return (XML_SCHEMAP_COS_ST_RESTRICTS_1_3_1);
		}
		/*
		 * SPEC (1.3.2) "If there is a facet of the same kind in the {facets}
		 * of the {base type definition} (call this BF),then the DF's {value}
		 * must be a valid restriction of BF's {value} as defined in
		 * [XML Schemas: Datatypes]."
		 *
		 * NOTE (1.3.2) Facet derivation constraints are currently handled in
		 * xmlSchemaDeriveAndValidateFacets()
		 */
	}
	else if(WXS_IS_LIST(type)) {
		xmlSchemaTypePtr itemType = NULL;
		itemType = type->subtypes;
		if((itemType == NULL) || (!WXS_IS_SIMPLE(itemType))) {
			PERROR_INT("xmlSchemaCheckCOSSTRestricts", "failed to evaluate the item type");
			return -1;
		}
		if(WXS_IS_TYPE_NOT_FIXED(itemType))
			xmlSchemaTypeFixup(itemType, ACTXT_CAST pctxt);
		/*
		 * 2.1 The {item type definition} must have a {variety} of atomic or
		 * union (in which case all the {member type definitions}
		 * must be atomic).
		 */
		if((!WXS_IS_ATOMIC(itemType)) && (!WXS_IS_UNION(itemType))) {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_1, WXS_BASIC_CAST type, NULL,
			    "The item type '%s' does not have a variety of atomic or union", xmlSchemaGetComponentQName(&str, itemType));
			ZFREE(str);
			return (XML_SCHEMAP_COS_ST_RESTRICTS_2_1);
		}
		else if(WXS_IS_UNION(itemType)) {
			xmlSchemaTypeLinkPtr member = itemType->memberTypes;
			while(member) {
				if(!WXS_IS_ATOMIC(member->type)) {
					xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_1, WXS_BASIC_CAST type, NULL,
					    "The item type is a union type, but the member type '%s' of this item type is not atomic",
					    xmlSchemaGetComponentQName(&str, member->type));
					ZFREE(str);
					return (XML_SCHEMAP_COS_ST_RESTRICTS_2_1);
				}
				member = member->next;
			}
		}
		if(WXS_IS_ANY_SIMPLE_TYPE(type->baseType)) {
			xmlSchemaFacetPtr facet;
			/*
			 * This is the case if we have: <simpleType><list ..
			 */
			/*
			 * 2.3.1
			 * 2.3.1.1 The {final} of the {item type definition} must not
			 * contain list.
			 */
			if(xmlSchemaTypeFinalContains(itemType, XML_SCHEMAS_TYPE_FINAL_LIST)) {
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_3_1_1, WXS_BASIC_CAST type, NULL,
				    "The final of its item type '%s' must not contain 'list'", xmlSchemaGetComponentQName(&str, itemType));
				ZFREE(str);
				return (XML_SCHEMAP_COS_ST_RESTRICTS_2_3_1_1);
			}
			/*
			 * 2.3.1.2 The {facets} must only contain the whiteSpace
			 * facet component.
			 * OPTIMIZE TODO: the S4S already disallows any facet
			 * to be specified.
			 */
			if(type->facets) {
				facet = type->facets;
				do {
					if(facet->type != XML_SCHEMA_FACET_WHITESPACE) {
						xmlSchemaPIllegalFacetListUnionErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_3_1_2, type, facet);
						return (XML_SCHEMAP_COS_ST_RESTRICTS_2_3_1_2);
					}
					facet = facet->next;
				} while(facet);
			}
			/*
			 * MAYBE TODO: (Hmm, not really) Datatypes states:
			 * A `list` datatype can be `derived` from an `atomic` datatype
			 * whose `lexical space` allows space (such as string or anyURI)or
			 * a `union` datatype any of whose {member type definitions}'s
			 * `lexical space` allows space.
			 */
		}
		else {
			/*
			 * This is the case if we have: <simpleType><restriction ...
			 * I.e. the variety of "list" is inherited.
			 */
			/*
			 * 2.3.2
			 * 2.3.2.1 The {base type definition} must have a {variety} of list.
			 */
			if(!WXS_IS_LIST(type->baseType)) {
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_1, WXS_BASIC_CAST type, NULL,
				    "The base type '%s' must be a list type", xmlSchemaGetComponentQName(&str, type->baseType));
				ZFREE(str);
				return (XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_1);
			}
			/*
			 * 2.3.2.2 The {final} of the {base type definition} must not
			 * contain restriction.
			 */
			if(xmlSchemaTypeFinalContains(type->baseType, XML_SCHEMAS_TYPE_FINAL_RESTRICTION)) {
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_2, WXS_BASIC_CAST type, NULL,
				    "The 'final' of the base type '%s' must not contain 'restriction'", xmlSchemaGetComponentQName(&str, type->baseType));
				ZFREE(str);
				return (XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_2);
			}
			/*
			 * 2.3.2.3 The {item type definition} must be validly derived
			 * from the {base type definition}'s {item type definition} given
			 * the empty set, as defined in Type Derivation OK (Simple) ($3.14.6).
			 */
			{
				xmlSchemaTypePtr baseItemType = type->baseType->subtypes;
				if((baseItemType == NULL) || (!WXS_IS_SIMPLE(baseItemType))) {
					PERROR_INT("xmlSchemaCheckCOSSTRestricts", "failed to eval the item type of a base type");
					return -1;
				}
				if((itemType != baseItemType) && (xmlSchemaCheckCOSSTDerivedOK(ACTXT_CAST pctxt, itemType, baseItemType, 0) != 0)) {
					xmlChar * strBIT = NULL, * strBT = NULL;
					xmlSchemaPCustomErrExt(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_3,
					    WXS_BASIC_CAST type, NULL,
					    "The item type '%s' is not validly derived from the item type '%s' of the base type '%s'",
					    xmlSchemaGetComponentQName(&str, itemType), xmlSchemaGetComponentQName(&strBIT, baseItemType),
					    xmlSchemaGetComponentQName(&strBT, type->baseType));
					ZFREE(str);
					ZFREE(strBIT);
					ZFREE(strBT);
					return (XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_3);
				}
			}
			if(type->facets) {
				int ok = 1;
				/*
				 * 2.3.2.4 Only length, minLength, maxLength, whiteSpace, pattern
				 * and enumeration facet components are allowed among the {facets}.
				 */
				xmlSchemaFacet * facet = type->facets;
				do {
					switch(facet->type) {
						case XML_SCHEMA_FACET_LENGTH:
						case XML_SCHEMA_FACET_MINLENGTH:
						case XML_SCHEMA_FACET_MAXLENGTH:
						case XML_SCHEMA_FACET_WHITESPACE:
						/*
						 * @todo 2.5.1.2 List datatypes
						 * The value of `whiteSpace` is fixed to the value collapse.
						 */
						case XML_SCHEMA_FACET_PATTERN:
						case XML_SCHEMA_FACET_ENUMERATION:
						    break;
						default: {
						    xmlSchemaPIllegalFacetListUnionErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_4, type, facet);
						    /*
						     * We could return, but it's nicer to report all
						     * invalid facets.
						     */
						    ok = 0;
					    }
					}
					facet = facet->next;
				} while(facet);
				if(ok == 0)
					return (XML_SCHEMAP_COS_ST_RESTRICTS_2_3_2_4);
				/*
				 * SPEC (2.3.2.5) (same as 1.3.2)
				 *
				 * NOTE (2.3.2.5) This is currently done in
				 * xmlSchemaDeriveAndValidateFacets()
				 */
			}
		}
	}
	else if(WXS_IS_UNION(type)) {
		/*
		 * 3.1 The {member type definitions} must all have {variety} of
		 * atomic or list.
		 */
		xmlSchemaTypeLink * member = type->memberTypes;
		while(member) {
			if(WXS_IS_TYPE_NOT_FIXED(member->type))
				xmlSchemaTypeFixup(member->type, ACTXT_CAST pctxt);
			if((!WXS_IS_ATOMIC(member->type)) && (!WXS_IS_LIST(member->type))) {
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_3_1, WXS_BASIC_CAST type, NULL,
				    "The member type '%s' is neither an atomic, nor a list type", xmlSchemaGetComponentQName(&str, member->type));
				ZFREE(str);
				return (XML_SCHEMAP_COS_ST_RESTRICTS_3_1);
			}
			member = member->next;
		}
		/*
		 * 3.3.1 If the {base type definition} is the `simple ur-type
		 * definition`
		 */
		if(type->baseType->builtInType == XML_SCHEMAS_ANYSIMPLETYPE) {
			/*
			 * 3.3.1.1 All of the {member type definitions} must have a
			 * {final} which does not contain union.
			 */
			member = type->memberTypes;
			while(member) {
				if(xmlSchemaTypeFinalContains(member->type, XML_SCHEMAS_TYPE_FINAL_UNION)) {
					xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_3_3_1, WXS_BASIC_CAST type, NULL,
					    "The 'final' of member type '%s' contains 'union'", xmlSchemaGetComponentQName(&str, member->type));
					ZFREE(str);
					return (XML_SCHEMAP_COS_ST_RESTRICTS_3_3_1);
				}
				member = member->next;
			}
			/*
			 * 3.3.1.2 The {facets} must be empty.
			 */
			if(type->facetSet) {
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_3_3_1_2, WXS_BASIC_CAST type, NULL, "No facets allowed", 0);
				return (XML_SCHEMAP_COS_ST_RESTRICTS_3_3_1_2);
			}
		}
		else {
			/*
			 * 3.3.2.1 The {base type definition} must have a {variety} of union.
			 * I.e. the variety of "list" is inherited.
			 */
			if(!WXS_IS_UNION(type->baseType)) {
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_1, WXS_BASIC_CAST type, NULL,
				    "The base type '%s' is not a union type", xmlSchemaGetComponentQName(&str, type->baseType));
				ZFREE(str);
				return (XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_1);
			}
			/*
			 * 3.3.2.2 The {final} of the {base type definition} must not contain restriction.
			 */
			if(xmlSchemaTypeFinalContains(type->baseType, XML_SCHEMAS_TYPE_FINAL_RESTRICTION)) {
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_2, WXS_BASIC_CAST type, NULL,
				    "The 'final' of its base type '%s' must not contain 'restriction'",
				    xmlSchemaGetComponentQName(&str, type->baseType));
				ZFREE(str);
				return (XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_2);
			}
			/*
			 * 3.3.2.3 The {member type definitions}, in order, must be validly
			 * derived from the corresponding type definitions in the {base
			 * type definition}'s {member type definitions} given the empty set,
			 * as defined in Type Derivation OK (Simple) ($3.14.6).
			 */
			{
				xmlSchemaTypeLinkPtr baseMember;
				/*
				 * OPTIMIZE: if the type is restricting, it has no local defined
				 * member types and inherits the member types of the base type;
				 * thus a check for equality can be skipped.
				 */
				/*
				 * Even worse: I cannot see a scenario where a restricting
				 * union simple type can have other member types as the member
				 * types of it's base type. This check seems not necessary with
				 * respect to the derivation process in libxml2.
				 * But necessary if constructing types with an API.
				 */
				if(type->memberTypes) {
					member = type->memberTypes;
					baseMember = xmlSchemaGetUnionSimpleTypeMemberTypes(type->baseType);
					if((member == NULL) && baseMember) {
						PERROR_INT("xmlSchemaCheckCOSSTRestricts", "different number of member types in base");
					}
					while(member) {
						if(baseMember == NULL) {
							PERROR_INT("xmlSchemaCheckCOSSTRestricts", "different number of member types in base");
						}
						else if((member->type != baseMember->type) && (xmlSchemaCheckCOSSTDerivedOK(ACTXT_CAST pctxt, member->type, baseMember->type, 0) != 0)) {
							xmlChar * strBMT = NULL, * strBT = NULL;
							xmlSchemaPCustomErrExt(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_3, WXS_BASIC_CAST type, NULL,
							    "The member type %s is not validly derived from its corresponding member type %s of the base type %s",
							    xmlSchemaGetComponentQName(&str, member->type), xmlSchemaGetComponentQName(&strBMT, baseMember->type),
							    xmlSchemaGetComponentQName(&strBT, type->baseType));
							ZFREE(str);
							ZFREE(strBMT);
							ZFREE(strBT);
							return (XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_3);
						}
						member = member->next;
						if(baseMember)
							baseMember = baseMember->next;
					}
				}
			}
			/*
			 * 3.3.2.4 Only pattern and enumeration facet components are
			 * allowed among the {facets}.
			 */
			if(type->facets) {
				int ok = 1;
				xmlSchemaFacet * facet = type->facets;
				do {
					if((facet->type != XML_SCHEMA_FACET_PATTERN) && (facet->type != XML_SCHEMA_FACET_ENUMERATION)) {
						xmlSchemaPIllegalFacetListUnionErr(pctxt, XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_4, type, facet);
						ok = 0;
					}
					facet = facet->next;
				} while(facet);
				if(ok == 0)
					return (XML_SCHEMAP_COS_ST_RESTRICTS_3_3_2_4);
			}
			/*
			 * SPEC (3.3.2.5) (same as 1.3.2)
			 *
			 * NOTE (3.3.2.5) This is currently done in
			 * xmlSchemaDeriveAndValidateFacets()
			 */
		}
	}

	return 0;
}

/**
 * xmlSchemaCheckSRCSimpleType:
 * @ctxt:  the schema parser context
 * @type:  the simple type definition
 *
 * Checks crc-simple-type constraints.
 *
 * Returns 0 if the constraints are satisfied,
 * if not a positive error code and -1 on internal
 * errors.
 */
#if 0
static int xmlSchemaCheckSRCSimpleType(xmlSchemaParserCtxtPtr ctxt, xmlSchemaTypePtr type)
{
	/*
	 * src-simple-type.1 The corresponding simple type definition, if any,
	 * must satisfy the conditions set out in Constraints on Simple Type
	 * Definition Schema Components ($3.14.6).
	 */
	if(WXS_IS_RESTRICTION(type)) {
		/*
		 * src-simple-type.2 "If the <restriction> alternative is chosen,
		 * either it must have a base [attribute] or a <simpleType> among its
		 * [children], but not both."
		 * NOTE: This is checked in the parse function of <restriction>.
		 */
		/*
		 *
		 */
	}
	else if(WXS_IS_LIST(type)) {
		/* src-simple-type.3 "If the <list> alternative is chosen, either it must have
		 * an itemType [attribute] or a <simpleType> among its [children],
		 * but not both."
		 *
		 * NOTE: This is checked in the parse function of <list>.
		 */
	}
	else if(WXS_IS_UNION(type)) {
		/*
		 * src-simple-type.4 is checked in xmlSchemaCheckUnionTypeDefCircular().
		 */
	}
	return 0;
}

#endif

static int xmlSchemaCreateVCtxtOnPCtxt(xmlSchemaParserCtxtPtr ctxt)
{
	if(!ctxt->vctxt) {
		ctxt->vctxt = xmlSchemaNewValidCtxt(NULL);
		if(!ctxt->vctxt) {
			xmlSchemaPErr(ctxt, NULL, XML_SCHEMAP_INTERNAL, "Internal error: xmlSchemaCreateVCtxtOnPCtxt, failed to create a temp. validation context.\n", 0, 0);
			return -1;
		}
		else {
			// @todo Pass user data.
			xmlSchemaSetValidErrors(ctxt->vctxt, ctxt->F_Error, ctxt->F_Warning, ctxt->errCtxt);
			xmlSchemaSetValidStructuredErrors(ctxt->vctxt, ctxt->serror, ctxt->errCtxt);
		}
	}
	return 0;
}

static int xmlSchemaVCheckCVCSimpleType(xmlSchemaAbstractCtxtPtr actxt,
    xmlNode * P_Node, xmlSchemaTypePtr type, const xmlChar * value, xmlSchemaValPtr * retVal,
    int fireErrors, int normalize, int isNormalized);

/**
 * xmlSchemaParseCheckCOSValidDefault:
 * @pctxt:  the schema parser context
 * @type:  the simple type definition
 * @value: the default value
 * @node: an optional node (the holder of the value)
 *
 * Schema Component Constraint: Element Default Valid (Immediate)
 * (cos-valid-default)
 * This will be used by the parser only. For the validator there's
 * an other version.
 *
 * Returns 0 if the constraints are satisfied,
 * if not, a positive error code and -1 on internal
 * errors.
 */
static int xmlSchemaParseCheckCOSValidDefault(xmlSchemaParserCtxtPtr pctxt, xmlNode * P_Node, xmlSchemaTypePtr type, const xmlChar * value, xmlSchemaValPtr * val)
{
	int ret = 0;
	/*
	 * cos-valid-default:
	 * Schema Component Constraint: Element Default Valid (Immediate)
	 * For a string to be a valid default with respect to a type
	 * definition the appropriate case among the following must be true:
	 */
	if WXS_IS_COMPLEX(type) {
		/*
		 * Complex type.
		 *
		 * SPEC (2.1) "its {content type} must be a simple type definition
		 * or mixed."
		 * SPEC (2.2.2) "If the {content type} is mixed, then the {content
		 * type}'s particle must be `emptiable` as defined by
		 * Particle Emptiable ($3.9.6)."
		 */
		if((!WXS_HAS_SIMPLE_CONTENT(type)) &&
		    ((!WXS_HAS_MIXED_CONTENT(type)) || (!WXS_EMPTIABLE(type)))) {
			/* NOTE that this covers (2.2.2) as well. */
			xmlSchemaPCustomErr(pctxt,
			    XML_SCHEMAP_COS_VALID_DEFAULT_2_1,
			    WXS_BASIC_CAST type, type->P_Node,
			    "For a string to be a valid default, the type definition "
			    "must be a simple type or a complex type with mixed content "
			    "and a particle emptiable", 0);
			return(XML_SCHEMAP_COS_VALID_DEFAULT_2_1);
		}
	}
	/*
	 * 1 If the type definition is a simple type definition, then the string
	 * must be `valid` with respect to that definition as defined by String
	 * Valid ($3.14.4).
	 *
	 * AND
	 *
	 * 2.2.1 If the {content type} is a simple type definition, then the
	 * string must be `valid` with respect to that simple type definition
	 * as defined by String Valid ($3.14.4).
	 */
	if(WXS_IS_SIMPLE(type))
		ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST pctxt, P_Node, type, value, val, 1, 1, 0);
	else if(WXS_HAS_SIMPLE_CONTENT(type))
		ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST pctxt, P_Node, type->contentTypeDef, value, val, 1, 1, 0);
	else
		return ret;
	if(ret < 0) {
		PERROR_INT("xmlSchemaParseCheckCOSValidDefault", "calling xmlSchemaVCheckCVCSimpleType()");
	}
	return ret;
}

/**
 * xmlSchemaCheckCTPropsCorrect:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 **.(4.6) Constraints on Complex Type Definition Schema Components
 * Schema Component Constraint:
 * Complex Type Definition Properties Correct (ct-props-correct)
 * STATUS: (seems) complete
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckCTPropsCorrect(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	/*
	 * @todo Correct the error code; XML_SCHEMAP_SRC_CT_1 is used temporarily.
	 *
	 * SPEC (1) "The values of the properties of a complex type definition must
	 * be as described in the property tableau in The Complex Type Definition
	 * Schema Component ($3.4.1), modulo the impact of Missing
	 * Sub-components ($5.3)."
	 */
	if(type->baseType && (WXS_IS_SIMPLE(type->baseType)) && (WXS_IS_EXTENSION(type) == 0)) {
		/*
		 * SPEC (2) "If the {base type definition} is a simple type definition,
		 * the {derivation method} must be extension."
		 */
		xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_SRC_CT_1, NULL, WXS_BASIC_CAST type,
		    "If the base type is a simple type, the derivation method must be 'extension'", 0, 0);
		return (XML_SCHEMAP_SRC_CT_1);
	}
	/*
	 * SPEC (3) "Circular definitions are disallowed, except for the `ur-type
	 * definition`. That is, it must be possible to reach the `ur-type
	 * definition` by repeatedly following the {base type definition}."
	 *
	 * NOTE (3) is done in xmlSchemaCheckTypeDefCircular().
	 */
	/*
	 * NOTE that (4) and (5) need the following:
	 *   - attribute uses need to be already inherited (apply attr. prohibitions)
	 *   - attribute group references need to be expanded already
	 *   - simple types need to be typefixed already
	 */
	if(type->attrUses && (((xmlSchemaItemListPtr)type->attrUses)->nbItems > 1)) {
		xmlSchemaItemListPtr uses = (xmlSchemaItemListPtr)type->attrUses;
		xmlSchemaAttributeUsePtr use, tmp;
		int i, j, hasId = 0;
		for(i = uses->nbItems -1; i >= 0; i--) {
			use = (xmlSchemaAttributeUsePtr)uses->items[i];
			/*
			 * SPEC ct-props-correct
			 * (4) "Two distinct attribute declarations in the
			 * {attribute uses} must not have identical {name}s and
			 * {target namespace}s."
			 */
			if(i > 0) {
				for(j = i -1; j >= 0; j--) {
					tmp = (xmlSchemaAttributeUsePtr)uses->items[j];
					if((WXS_ATTRUSE_DECL_NAME(use) == WXS_ATTRUSE_DECL_NAME(tmp)) && (WXS_ATTRUSE_DECL_TNS(use) == WXS_ATTRUSE_DECL_TNS(tmp))) {
						xmlChar * str = NULL;
						xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_AG_PROPS_CORRECT, NULL, WXS_BASIC_CAST type,
						    "Duplicate %s", xmlSchemaGetComponentDesignation(&str, use), 0);
						ZFREE(str);
						/*
						 * Remove the duplicate.
						 */
						if(xmlSchemaItemListRemove(uses, i) == -1)
							goto exit_failure;
						goto next_use;
					}
				}
			}
			/*
			 * SPEC ct-props-correct
			 * (5) "Two distinct attribute declarations in the
			 * {attribute uses} must not have {type definition}s which
			 * are or are derived from ID."
			 */
			if(WXS_ATTRUSE_TYPEDEF(use)) {
				if(xmlSchemaIsDerivedFromBuiltInType(WXS_ATTRUSE_TYPEDEF(use), XML_SCHEMAS_ID)) {
					if(hasId) {
						xmlChar * str = NULL;
						xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_AG_PROPS_CORRECT, NULL, WXS_BASIC_CAST type,
						    "There must not exist more than one attribute declaration of type 'xs:ID' "
						    "(or derived from 'xs:ID'). The %s violates this constraint",
						    xmlSchemaGetComponentDesignation(&str, use), NULL);
						ZFREE(str);
						if(xmlSchemaItemListRemove(uses, i) == -1)
							goto exit_failure;
					}

					hasId = 1;
				}
			}
next_use: {}
		}
	}
	return 0;
exit_failure:
	return -1;
}

static int xmlSchemaAreEqualTypes(xmlSchemaTypePtr typeA,
    xmlSchemaTypePtr typeB)
{
	/*
	 * @todo This should implement component-identity
	 * in the future.
	 */
	if((typeA == NULL) || (typeB == NULL))
		return 0;
	return (typeA == typeB);
}

/**
 * xmlSchemaCheckCOSCTDerivedOK:
 * @ctxt:  the schema parser context
 * @type:  the to-be derived complex type definition
 * @baseType:  the base complex type definition
 * @set: the given set
 *
 * Schema Component Constraint:
 * Type Derivation OK (Complex) (cos-ct-derived-ok)
 *
 * STATUS: completed
 *
 * Returns 0 if the constraints are satisfied, or 1
 * if not.
 */
static int xmlSchemaCheckCOSCTDerivedOK(xmlSchemaAbstractCtxtPtr actxt,
    xmlSchemaTypePtr type,
    xmlSchemaTypePtr baseType,
    int set)
{
	int equal = xmlSchemaAreEqualTypes(type, baseType);
	/* @todo Error codes. */
	/*
	 * SPEC "For a complex type definition (call it D, for derived)
	 * to be validly derived from a type definition (call this
	 * B, for base) given a subset of {extension, restriction}
	 * all of the following must be true:"
	 */
	if(!equal) {
		/*
		 * SPEC (1) "If B and D are not the same type definition, then the
		 * {derivation method} of D must not be in the subset."
		 */
		if(((set & SUBSET_EXTENSION) && (WXS_IS_EXTENSION(type))) ||
		    ((set & SUBSET_RESTRICTION) && (WXS_IS_RESTRICTION(type))))
			return 1;
	}
	else {
		/*
		 * SPEC (2.1) "B and D must be the same type definition."
		 */
		return 0;
	}
	/*
	 * SPEC (2.2) "B must be D's {base type definition}."
	 */
	if(type->baseType == baseType)
		return 0;
	/*
	 * SPEC (2.3.1) "D's {base type definition} must not be the `ur-type
	 * definition`."
	 */
	if(WXS_IS_ANYTYPE(type->baseType))
		return 1;

	if(WXS_IS_COMPLEX(type->baseType)) {
		/*
		 * SPEC (2.3.2.1) "If D's {base type definition} is complex, then it
		 * must be validly derived from B given the subset as defined by this
		 * constraint."
		 */
		return (xmlSchemaCheckCOSCTDerivedOK(actxt, type->baseType,
			    baseType, set));
	}
	else {
		/*
		 * SPEC (2.3.2.2) "If D's {base type definition} is simple, then it
		 * must be validly derived from B given the subset as defined in Type
		 * Derivation OK (Simple) ($3.14.6).
		 */
		return (xmlSchemaCheckCOSSTDerivedOK(actxt, type->baseType,
			    baseType, set));
	}
}

/**
 * xmlSchemaCheckCOSDerivedOK:
 * @type:  the derived simple type definition
 * @baseType:  the base type definition
 *
 * Calls:
 * Type Derivation OK (Simple) AND Type Derivation OK (Complex)
 *
 * Checks wheter @type can be validly derived from @baseType.
 *
 * Returns 0 on success, an positive error code otherwise.
 */
static int xmlSchemaCheckCOSDerivedOK(xmlSchemaAbstractCtxtPtr actxt,
    xmlSchemaTypePtr type,
    xmlSchemaTypePtr baseType,
    int set)
{
	if(WXS_IS_SIMPLE(type))
		return (xmlSchemaCheckCOSSTDerivedOK(actxt, type, baseType, set));
	else
		return (xmlSchemaCheckCOSCTDerivedOK(actxt, type, baseType, set));
}

/**
 * xmlSchemaCheckCOSCTExtends:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 * (3.4.6) Constraints on Complex Type Definition Schema Components
 * Schema Component Constraint:
 * Derivation Valid (Extension) (cos-ct-extends)
 *
 * STATUS:
 *   missing:
 *     (1.5)
 *     (1.4.3.2.2.2) "Particle Valid (Extension)"
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckCOSCTExtends(xmlSchemaParserCtxtPtr ctxt, xmlSchemaTypePtr type)
{
	xmlSchemaTypePtr base = type->baseType;
	/*
	 * @todo Correct the error code; XML_SCHEMAP_COS_CT_EXTENDS_1_1 is used
	 * temporarily only.
	 */
	/*
	 * SPEC (1) "If the {base type definition} is a complex type definition,
	 * then all of the following must be true:"
	 */
	if(WXS_IS_COMPLEX(base)) {
		/*
		 * SPEC (1.1) "The {final} of the {base type definition} must not
		 * contain extension."
		 */
		if(base->flags & XML_SCHEMAS_TYPE_FINAL_EXTENSION) {
			xmlSchemaPCustomErr(ctxt,
			    XML_SCHEMAP_COS_CT_EXTENDS_1_1,
			    WXS_BASIC_CAST type, NULL,
			    "The 'final' of the base type definition "
			    "contains 'extension'", 0);
			return (XML_SCHEMAP_COS_CT_EXTENDS_1_1);
		}

		/*
		 * ATTENTION: The constrains (1.2) and (1.3) are not applied,
		 * since they are automatically satisfied through the
		 * inheriting mechanism.
		 * Note that even if redefining components, the inheriting mechanism
		 * is used.
		 */
#if 0
		/*
		 * SPEC (1.2) "Its {attribute uses} must be a subset of the {attribute
		 * uses}
		 * of the complex type definition itself, that is, for every attribute
		 * use in the {attribute uses} of the {base type definition}, there
		 * must be an attribute use in the {attribute uses} of the complex
		 * type definition itself whose {attribute declaration} has the same
		 * {name}, {target namespace} and {type definition} as its attribute
		 * declaration"
		 */
		if(base->attrUses) {
			int i, j, found;
			xmlSchemaAttributeUsePtr use, buse;
			for(i = 0; i < (WXS_LIST_CAST base->attrUses)->nbItems; i++) {
				buse = (WXS_LIST_CAST base->attrUses)->items[i];
				found = 0;
				if(type->attrUses) {
					use = (WXS_LIST_CAST type->attrUses)->items[j];
					for(j = 0; j < (WXS_LIST_CAST type->attrUses)->nbItems; j++) {
						if((WXS_ATTRUSE_DECL_NAME(use) ==
							    WXS_ATTRUSE_DECL_NAME(buse)) &&
						    (WXS_ATTRUSE_DECL_TNS(use) ==
							    WXS_ATTRUSE_DECL_TNS(buse)) &&
						    (WXS_ATTRUSE_TYPEDEF(use) ==
							    WXS_ATTRUSE_TYPEDEF(buse))
						    {
							    found = 1;
							    break;
						    }
						    }
						    }
						    if(!found) {
							    xmlChar * str = NULL;
							    xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_COS_CT_EXTENDS_1_2, NULL, WXS_BASIC_CAST type,
						                    /*
						                     * @todo The report does not indicate that also the type needs to be the same.
						                     */
								    "This type is missing a matching correspondent for its {base type}'s %s in its {attribute uses}",
								    xmlSchemaGetComponentDesignation(&str, buse->children),
								    NULL);
							    ZFREE(str);
						    }
						    }
						    }
						    /*
						     * SPEC (1.3) "If it has an {attribute wildcard}, the complex type
						     * definition must also have one, and the base type definition's
						     * {attribute  wildcard}'s {namespace constraint} must be a subset
						     * of the complex  type definition's {attribute wildcard}'s
						     *{namespace
						     * constraint}, as defined by Wildcard Subset ($3.10.6)."
						     */

						    /*
						     * MAYBE TODO: Enable if ever needed. But this will be needed only
						     * if created the type via a schema construction API.
						     */
						    if(base->attributeWildcard) {
							    if(type->attributeWilcard == NULL) {
								    xmlChar * str = NULL;
								    xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_COS_CT_EXTENDS_1_3, NULL, type,
									    "The base %s has an attribute wildcard, but this type is missing an attribute wildcard",
									    xmlSchemaGetComponentDesignation(&str, base));
								    ZFREE(str);
							    }
							    else if(xmlSchemaCheckCOSNSSubset(base->attributeWildcard, type->attributeWildcard)) {
								    xmlChar * str = NULL;
								    xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_COS_CT_EXTENDS_1_3, NULL, type,
									    "The attribute wildcard is not a valid superset of the one in the base %s",
									    xmlSchemaGetComponentDesignation(&str, base));
								    ZFREE(str);
							    }
						    }
#endif
		/*
		 * SPEC (1.4) "One of the following must be true:"
		 */
		if(type->contentTypeDef && (type->contentTypeDef == base->contentTypeDef)) {
			 // SPEC (1.4.1) "The {content type} of the {base type definition}
			 // and the {content type} of the complex type definition itself
			 // must be the same simple type definition" 
			 // PASS
		}
		else if((type->contentType == XML_SCHEMA_CONTENT_EMPTY) && (base->contentType == XML_SCHEMA_CONTENT_EMPTY) ) {
			/*
			 * SPEC (1.4.2) "The {content type} of both the {base type
			 * definition} and the complex type definition itself must
			 * be empty."
			 * PASS
			 */
		}
		else {
			/*
			 * SPEC (1.4.3) "All of the following must be true:"
			 */
			if(type->subtypes == NULL) {
				/*
				 * SPEC 1.4.3.1 The {content type} of the complex type
				 * definition itself must specify a particle.
				 */
				xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_COS_CT_EXTENDS_1_1, WXS_BASIC_CAST type, NULL, "The content type must specify a particle", 0);
				return (XML_SCHEMAP_COS_CT_EXTENDS_1_1);
			}
			/*
			 * SPEC (1.4.3.2) "One of the following must be true:"
			 */
			if(base->contentType == XML_SCHEMA_CONTENT_EMPTY) {
				/*
				 * SPEC (1.4.3.2.1) "The {content type} of the {base type
				 * definition} must be empty.
				 * PASS
				 */
			}
			else {
				/*
				 * SPEC (1.4.3.2.2) "All of the following must be true:"
				 */
				if((type->contentType != base->contentType) || ((type->contentType != XML_SCHEMA_CONTENT_MIXED) && (type->contentType != XML_SCHEMA_CONTENT_ELEMENTS))) {
					/*
					 * SPEC (1.4.3.2.2.1) "Both {content type}s must be mixed
					 * or both must be element-only."
					 */
					xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_COS_CT_EXTENDS_1_1, WXS_BASIC_CAST type, NULL,
					    "The content type of both, the type and its base type, must either 'mixed' or 'element-only'", 0);
					return (XML_SCHEMAP_COS_CT_EXTENDS_1_1);
				}
				/*
				 * URGENT TODO SPEC (1.4.3.2.2.2) "The particle of the
				 * complex type definition must be a `valid extension`
				 * of the {base type definition}'s particle, as defined
				 * in Particle Valid (Extension) ($3.9.6)."
				 *
				 * NOTE that we won't check "Particle Valid (Extension)",
				 * since it is ensured by the derivation process in
				 * xmlSchemaTypeFixup(). We need to implement this when heading
				 * for a construction API
				 * @todo !! This is needed to be checked if redefining a type !!
				 */
			}
			/*
			 * URGENT TODO (1.5)
			 */
		}
	}
	else {
		/*
		 * SPEC (2) "If the {base type definition} is a simple type definition,
		 * then all of the following must be true:"
		 */
		if(type->contentTypeDef != base) {
			/*
			 * SPEC (2.1) "The {content type} must be the same simple type
			 * definition."
			 */
			xmlSchemaPCustomErr(ctxt,
			    XML_SCHEMAP_COS_CT_EXTENDS_1_1,
			    WXS_BASIC_CAST type, NULL,
			    "The content type must be the simple base type", 0);
			return (XML_SCHEMAP_COS_CT_EXTENDS_1_1);
		}
		if(base->flags & XML_SCHEMAS_TYPE_FINAL_EXTENSION) {
			/*
			 * SPEC (2.2) "The {final} of the {base type definition} must not
			 * contain extension"
			 * NOTE that this is the same as (1.1).
			 */
			xmlSchemaPCustomErr(ctxt,
			    XML_SCHEMAP_COS_CT_EXTENDS_1_1,
			    WXS_BASIC_CAST type, NULL,
			    "The 'final' of the base type definition "
			    "contains 'extension'", 0);
			return (XML_SCHEMAP_COS_CT_EXTENDS_1_1);
		}
	}
	return 0;
}

/**
 * xmlSchemaCheckDerivationOKRestriction:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 * (3.4.6) Constraints on Complex Type Definition Schema Components
 * Schema Component Constraint:
 * Derivation Valid (Restriction, Complex) (derivation-ok-restriction)
 *
 * STATUS:
 *   missing:
 *     (5.4.2) ???
 *
 * ATTENTION:
 * In XML Schema 1.1 this will be:
 * Validation Rule: Checking complex type subsumption
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckDerivationOKRestriction(xmlSchemaParserCtxtPtr ctxt, xmlSchemaTypePtr type)
{
	xmlSchemaTypePtr base;
	/*
	 * @todo Correct the error code; XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1 is used
	 * temporarily only.
	 */
	base = type->baseType;
	if(!WXS_IS_COMPLEX(base)) {
		xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1,
		    type->P_Node, WXS_BASIC_CAST type, "The base type must be a complex type", 0, 0);
		return(ctxt->err);
	}
	if(base->flags & XML_SCHEMAS_TYPE_FINAL_RESTRICTION) {
		/*
		 * SPEC (1) "The {base type definition} must be a complex type
		 * definition whose {final} does not contain restriction."
		 */
		xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1,
		    type->P_Node, WXS_BASIC_CAST type, "The 'final' of the base type definition contains 'restriction'", 0, 0);
		return (ctxt->err);
	}
	/*
	 * SPEC (2), (3) and (4)
	 * Those are handled in a separate function, since the
	 * same constraints are needed for redefinition of
	 * attribute groups as well.
	 */
	if(xmlSchemaCheckDerivationOKRestriction2to4(ctxt, XML_SCHEMA_ACTION_DERIVE,
	    WXS_BASIC_CAST type, WXS_BASIC_CAST base, (xmlSchemaItemListPtr)type->attrUses, (xmlSchemaItemListPtr)base->attrUses,
	    type->attributeWildcard, base->attributeWildcard) == -1) {
		return -1;
	}
	/*
	 * SPEC (5) "One of the following must be true:"
	 */
	if(base->builtInType == XML_SCHEMAS_ANYTYPE) {
		/*
		 * SPEC (5.1) "The {base type definition} must be the
		 * `ur-type definition`."
		 * PASS
		 */
	}
	else if((type->contentType == XML_SCHEMA_CONTENT_SIMPLE) || (type->contentType == XML_SCHEMA_CONTENT_BASIC)) {
		/*
		 * SPEC (5.2.1) "The {content type} of the complex type definition
		 * must be a simple type definition"
		 *
		 * SPEC (5.2.2) "One of the following must be true:"
		 */
		if((base->contentType == XML_SCHEMA_CONTENT_SIMPLE) || (base->contentType == XML_SCHEMA_CONTENT_BASIC)) {
			int err;
			/*
			 * SPEC (5.2.2.1) "The {content type} of the {base type
			 * definition} must be a simple type definition from which
			 * the {content type} is validly derived given the empty
			 * set as defined in Type Derivation OK (Simple) ($3.14.6)."
			 *
			 * ATTENTION TODO: This seems not needed if the type implicitely
			 * derived from the base type.
			 *
			 */
			err = xmlSchemaCheckCOSSTDerivedOK(ACTXT_CAST ctxt,
			    type->contentTypeDef, base->contentTypeDef, 0);
			if(err != 0) {
				xmlChar * strA = NULL, * strB = NULL;

				if(err == -1)
					return -1;
				xmlSchemaCustomErr(ACTXT_CAST ctxt,
				    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1,
				    NULL, WXS_BASIC_CAST type,
				    "The {content type} %s is not validly derived from the "
				    "base type's {content type} %s",
				    xmlSchemaGetComponentDesignation(&strA,
					    type->contentTypeDef),
				    xmlSchemaGetComponentDesignation(&strB,
					    base->contentTypeDef));
				ZFREE(strA);
				ZFREE(strB);
				return(ctxt->err);
			}
		}
		else if((base->contentType == XML_SCHEMA_CONTENT_MIXED) &&
		    (xmlSchemaIsParticleEmptiable(
				    (xmlSchemaParticlePtr)base->subtypes))) {
			/*
			 * SPEC (5.2.2.2) "The {base type definition} must be mixed
			 * and have a particle which is `emptiable` as defined in
			 * Particle Emptiable ($3.9.6)."
			 * PASS
			 */
		}
		else {
			xmlSchemaPCustomErr(ctxt,
			    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1,
			    WXS_BASIC_CAST type, NULL,
			    "The content type of the base type must be either "
			    "a simple type or 'mixed' and an emptiable particle", 0);
			return (ctxt->err);
		}
	}
	else if(type->contentType == XML_SCHEMA_CONTENT_EMPTY) {
		/*
		 * SPEC (5.3.1) "The {content type} of the complex type itself must
		 * be empty"
		 */
		if(base->contentType == XML_SCHEMA_CONTENT_EMPTY) {
			/*
			 * SPEC (5.3.2.1) "The {content type} of the {base type
			 * definition} must also be empty."
			 * PASS
			 */
		}
		else if(((base->contentType == XML_SCHEMA_CONTENT_ELEMENTS) ||
			    (base->contentType == XML_SCHEMA_CONTENT_MIXED)) &&
		    xmlSchemaIsParticleEmptiable(
			    (xmlSchemaParticlePtr)base->subtypes)) {
			/*
			 * SPEC (5.3.2.2) "The {content type} of the {base type
			 * definition} must be elementOnly or mixed and have a particle
			 * which is `emptiable` as defined in Particle Emptiable ($3.9.6)."
			 * PASS
			 */
		}
		else {
			xmlSchemaPCustomErr(ctxt,
			    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1,
			    WXS_BASIC_CAST type, NULL,
			    "The content type of the base type must be either "
			    "empty or 'mixed' (or 'elements-only') and an emptiable "
			    "particle", 0);
			return (ctxt->err);
		}
	}
	else if((type->contentType == XML_SCHEMA_CONTENT_ELEMENTS) ||
	    WXS_HAS_MIXED_CONTENT(type)) {
		/*
		 * SPEC (5.4.1.1) "The {content type} of the complex type definition
		 * itself must be element-only"
		 */
		if(WXS_HAS_MIXED_CONTENT(type) && (!WXS_HAS_MIXED_CONTENT(base))) {
			/*
			 * SPEC (5.4.1.2) "The {content type} of the complex type
			 * definition itself and of the {base type definition} must be
			 * mixed"
			 */
			xmlSchemaPCustomErr(ctxt,
			    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1,
			    WXS_BASIC_CAST type, NULL,
			    "If the content type is 'mixed', then the content type of the "
			    "base type must also be 'mixed'", 0);
			return (ctxt->err);
		}
		/*
		 * SPEC (5.4.2) "The particle of the complex type definition itself
		 * must be a `valid restriction` of the particle of the {content
		 * type} of the {base type definition} as defined in Particle Valid
		 * (Restriction) ($3.9.6).
		 *
		 * URGENT TODO: (5.4.2)
		 */
	}
	else {
		xmlSchemaPCustomErr(ctxt,
		    XML_SCHEMAP_DERIVATION_OK_RESTRICTION_1,
		    WXS_BASIC_CAST type, NULL,
		    "The type is not a valid restriction of its base type", 0);
		return (ctxt->err);
	}
	return 0;
}

/**
 * xmlSchemaCheckCTComponent:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 * (3.4.6) Constraints on Complex Type Definition Schema Components
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckCTComponent(xmlSchemaParserCtxtPtr ctxt, xmlSchemaTypePtr type)
{
	/*
	 * Complex Type Definition Properties Correct
	 */
	int ret = xmlSchemaCheckCTPropsCorrect(ctxt, type);
	if(ret != 0)
		return ret;
	if(WXS_IS_EXTENSION(type))
		ret = xmlSchemaCheckCOSCTExtends(ctxt, type);
	else
		ret = xmlSchemaCheckDerivationOKRestriction(ctxt, type);
	return ret;
}
/**
 * xmlSchemaCheckSRCCT:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 * (3.4.3) Constraints on XML Representations of Complex Type Definitions:
 * Schema Representation Constraint:
 * Complex Type Definition Representation OK (src-ct)
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckSRCCT(xmlSchemaParserCtxtPtr ctxt, xmlSchemaTypePtr type)
{
	xmlSchemaTypePtr base;
	int ret = 0;
	/*
	 * @todo Adjust the error codes here, as I used
	 * XML_SCHEMAP_SRC_CT_1 only yet.
	 */
	base = type->baseType;
	if(!WXS_HAS_SIMPLE_CONTENT(type)) {
		/*
		 * 1 If the <complexContent> alternative is chosen, the type definition
		 * `resolved` to by the `actual value` of the base [attribute]
		 * must be a complex type definition;
		 */
		if(!WXS_IS_COMPLEX(base)) {
			xmlChar * str = NULL;
			xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_CT_1, WXS_BASIC_CAST type, type->P_Node,
			    "If using <complexContent>, the base type is expected to be a complex type. The base type '%s' is a simple type",
			    xmlSchemaFormatQName(&str, base->targetNamespace, base->name));
			ZFREE(str);
			return (XML_SCHEMAP_SRC_CT_1);
		}
	}
	else {
		/*
		 * SPEC
		 * 2 If the <simpleContent> alternative is chosen, all of the
		 * following must be true:
		 * 2.1 The type definition `resolved` to by the `actual value` of the
		 * base [attribute] must be one of the following:
		 */
		if(WXS_IS_SIMPLE(base)) {
			if(WXS_IS_EXTENSION(type) == 0) {
				xmlChar * str = NULL;
				/*
				 * 2.1.3 only if the <extension> alternative is also
				 * chosen, a simple type definition.
				 */
				/* @todo Change error code to ..._SRC_CT_2_1_3. */
				xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_CT_1, WXS_BASIC_CAST type, NULL,
				    "If using <simpleContent> and <restriction>, the base type must be a complex type. The base type '%s' is a simple type",
				    xmlSchemaFormatQName(&str, base->targetNamespace, base->name));
				ZFREE(str);
				return (XML_SCHEMAP_SRC_CT_1);
			}
		}
		else {
			/* Base type is a complex type. */
			if((base->contentType == XML_SCHEMA_CONTENT_SIMPLE) ||
			    (base->contentType == XML_SCHEMA_CONTENT_BASIC)) {
				/*
				 * 2.1.1 a complex type definition whose {content type} is a
				 * simple type definition;
				 * PASS
				 */
				if(base->contentTypeDef == NULL) {
					xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_INTERNAL, WXS_BASIC_CAST type, NULL, "Internal error: xmlSchemaCheckSRCCT, '%s', base type has no content type",
					    type->name);
					return -1;
				}
			}
			else if((base->contentType == XML_SCHEMA_CONTENT_MIXED) && (WXS_IS_RESTRICTION(type))) {
				/*
				 * 2.1.2 only if the <restriction> alternative is also
				 * chosen, a complex type definition whose {content type}
				 * is mixed and a particle emptiable.
				 */
				if(!xmlSchemaIsParticleEmptiable((xmlSchemaParticlePtr)base->subtypes)) {
					ret = XML_SCHEMAP_SRC_CT_1;
				}
				else
				/*
				 * Attention: at this point the <simpleType> child is in
				 * ->contentTypeDef (put there during parsing).
				 */
				if(type->contentTypeDef == NULL) {
					xmlChar * str = NULL;
					/*
					 * 2.2 If clause 2.1.2 above is satisfied, then there
					 * must be a <simpleType> among the [children] of
					 * <restriction>.
					 */
					/* @todo Change error code to ..._SRC_CT_2_2. */
					xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_CT_1, WXS_BASIC_CAST type, NULL,
					    "A <simpleType> is expected among the children of <restriction>, if <simpleContent> is used and the base type '%s' is a complex type",
					    xmlSchemaFormatQName(&str, base->targetNamespace, base->name));
					ZFREE(str);
					return (XML_SCHEMAP_SRC_CT_1);
				}
			}
			else {
				ret = XML_SCHEMAP_SRC_CT_1;
			}
		}
		if(ret > 0) {
			xmlChar * str = NULL;
			if(WXS_IS_RESTRICTION(type)) {
				xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_CT_1, WXS_BASIC_CAST type, NULL,
				    "If <simpleContent> and <restriction> is used, the base type must be a simple type or a complex type with mixed content and particle emptiable. The base type '%s' is none of those",
				    xmlSchemaFormatQName(&str, base->targetNamespace, base->name));
			}
			else {
				xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_CT_1, WXS_BASIC_CAST type, NULL,
				    "If <simpleContent> and <extension> is used, the base type must be a simple type. The base type '%s' is a complex type",
				    xmlSchemaFormatQName(&str, base->targetNamespace, base->name));
			}
			ZFREE(str);
		}
	}
	/*
	 * SPEC (3) "The corresponding complex type definition component must
	 * satisfy the conditions set out in Constraints on Complex Type
	 * Definition Schema Components ($3.4.6);"
	 * NOTE (3) will be done in xmlSchemaTypeFixup().
	 */
	/*
	 * SPEC (4) If clause 2.2.1 or clause 2.2.2 in the correspondence specification
	 * above for {attribute wildcard} is satisfied, the intensional
	 * intersection must be expressible, as defined in Attribute Wildcard
	 * Intersection ($3.10.6).
	 * NOTE (4) is done in xmlSchemaFixupTypeAttributeUses().
	 */
	return ret;
}

#ifdef ENABLE_PARTICLE_RESTRICTION
/**
 * xmlSchemaCheckParticleRangeOK:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Occurrence Range OK (range-ok)
 *
 * STATUS: complete
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckParticleRangeOK(int rmin, int rmax,
    int bmin, int bmax)
{
	if(rmin < bmin)
		return 1;
	if((bmax != UNBOUNDED) &&
	    (rmax > bmax))
		return 1;
	return 0;
}

/**
 * xmlSchemaCheckRCaseNameAndTypeOK:
 * @ctxt:  the schema parser context
 * @r: the restricting element declaration particle
 * @b: the base element declaration particle
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Particle Restriction OK (Elt:Elt -- NameAndTypeOK)
 * (rcase-NameAndTypeOK)
 *
 * STATUS:
 *   MISSING (3.2.3)
 *   CLARIFY: (3.2.2)
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckRCaseNameAndTypeOK(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr r, xmlSchemaParticlePtr b)
{
	xmlSchemaElementPtr elemR, elemB;
	/* @todo Error codes (rcase-NameAndTypeOK). */
	elemR = (xmlSchemaElementPtr)r->children;
	elemB = (xmlSchemaElementPtr)b->children;
	/*
	 * SPEC (1) "The declarations' {name}s and {target namespace}s are
	 * the same."
	 */
	if((elemR != elemB) &&
	    ((!sstreq(elemR->name, elemB->name)) ||
		    (!sstreq(elemR->targetNamespace, elemB->targetNamespace))))
		return 1;
	/*
	 * SPEC (2) "R's occurrence range is a valid restriction of B's
	 * occurrence range as defined by Occurrence Range OK ($3.9.6)."
	 */
	if(xmlSchemaCheckParticleRangeOK(r->minOccurs, r->maxOccurs,
		    b->minOccurs, b->maxOccurs) != 0)
		return 1;
	/*
	 * SPEC (3.1) "Both B's declaration's {scope} and R's declaration's
	 * {scope} are global."
	 */
	if(elemR == elemB)
		return 0;
	/*
	 * SPEC (3.2.1) "Either B's {nillable} is true or R's {nillable} is false."
	 */
	if(((elemB->flags & XML_SCHEMAS_ELEM_NILLABLE) == 0) &&
	    (elemR->flags & XML_SCHEMAS_ELEM_NILLABLE))
		return 1;
	/*
	 * SPEC (3.2.2) "either B's declaration's {value constraint} is absent,
	 * or is not fixed, or R's declaration's {value constraint} is fixed
	 * with the same value."
	 */
	if(elemB->value && (elemB->flags & XML_SCHEMAS_ELEM_FIXED) &&
	    ((elemR->value == NULL) ||
		    ((elemR->flags & XML_SCHEMAS_ELEM_FIXED) == 0) ||
	            /* @todo Equality of the initial value or normalized or canonical? */
		    (!sstreq(elemR->value, elemB->value))))
		return 1;
	/*
	 * @todo SPEC (3.2.3) "R's declaration's {identity-constraint
	 * definitions} is a subset of B's declaration's {identity-constraint
	 * definitions}, if any."
	 */
	if(elemB->idcs) {
		/* @todo */
	}
	/*
	 * SPEC (3.2.4) "R's declaration's {disallowed substitutions} is a
	 * superset of B's declaration's {disallowed substitutions}."
	 */
	if(((elemB->flags & XML_SCHEMAS_ELEM_BLOCK_EXTENSION) && ((elemR->flags & XML_SCHEMAS_ELEM_BLOCK_EXTENSION) == 0)) ||
	    ((elemB->flags & XML_SCHEMAS_ELEM_BLOCK_RESTRICTION) && ((elemR->flags & XML_SCHEMAS_ELEM_BLOCK_RESTRICTION) == 0)) ||
	    ((elemB->flags & XML_SCHEMAS_ELEM_BLOCK_SUBSTITUTION) && ((elemR->flags & XML_SCHEMAS_ELEM_BLOCK_SUBSTITUTION) == 0)))
		return 1;
	/*
	 * SPEC (3.2.5) "R's {type definition} is validly derived given
	 * {extension, list, union} from B's {type definition}"
	 *
	 * BADSPEC TODO: What's the point of adding "list" and "union" to the
	 * set, if the corresponding constraints handle "restriction" and
	 * "extension" only?
	 *
	 */
	{
		int set = 0;
		set |= SUBSET_EXTENSION;
		set |= SUBSET_LIST;
		set |= SUBSET_UNION;
		if(xmlSchemaCheckCOSDerivedOK(ACTXT_CAST ctxt, elemR->subtypes, elemB->subtypes, set) != 0)
			return 1;
	}
	return 0;
}

/**
 * xmlSchemaCheckRCaseNSCompat:
 * @ctxt:  the schema parser context
 * @r: the restricting element declaration particle
 * @b: the base wildcard particle
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Particle Derivation OK (Elt:Any -- NSCompat)
 * (rcase-NSCompat)
 *
 * STATUS: complete
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckRCaseNSCompat(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr r, xmlSchemaParticlePtr b)
{
	/* @todo Error codes (rcase-NSCompat). */
	/*
	 * SPEC "For an element declaration particle to be a `valid restriction`
	 * of a wildcard particle all of the following must be true:"
	 *
	 * SPEC (1) "The element declaration's {target namespace} is `valid`
	 * with respect to the wildcard's {namespace constraint} as defined by
	 * Wildcard allows Namespace Name ($3.10.4)."
	 */
	if(xmlSchemaCheckCVCWildcardNamespace((xmlSchemaWildcardPtr)b->children, ((xmlSchemaElementPtr)r->children)->targetNamespace) != 0)
		return 1;
	/*
	 * SPEC (2) "R's occurrence range is a valid restriction of B's
	 * occurrence range as defined by Occurrence Range OK ($3.9.6)."
	 */
	if(xmlSchemaCheckParticleRangeOK(r->minOccurs, r->maxOccurs, b->minOccurs, b->maxOccurs) != 0)
		return 1;
	return 0;
}

/**
 * xmlSchemaCheckRCaseRecurseAsIfGroup:
 * @ctxt:  the schema parser context
 * @r: the restricting element declaration particle
 * @b: the base model group particle
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Particle Derivation OK (Elt:All/Choice/Sequence -- RecurseAsIfGroup)
 * (rcase-RecurseAsIfGroup)
 *
 * STATUS: TODO
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckRCaseRecurseAsIfGroup(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr r, xmlSchemaParticlePtr b)
{
	/* @todo Error codes (rcase-RecurseAsIfGroup). */
	TODO
	return 0;
}

/**
 * xmlSchemaCheckRCaseNSSubset:
 * @ctxt:  the schema parser context
 * @r: the restricting wildcard particle
 * @b: the base wildcard particle
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Particle Derivation OK (Any:Any -- NSSubset)
 * (rcase-NSSubset)
 *
 * STATUS: complete
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckRCaseNSSubset(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr r, xmlSchemaParticlePtr b, int isAnyTypeBase)
{
	/* @todo Error codes (rcase-NSSubset). */
	/*
	 * SPEC (1) "R's occurrence range is a valid restriction of B's
	 * occurrence range as defined by Occurrence Range OK ($3.9.6)."
	 */
	if(xmlSchemaCheckParticleRangeOK(r->minOccurs, r->maxOccurs, b->minOccurs, b->maxOccurs))
		return 1;
	/*
	 * SPEC (2) "R's {namespace constraint} must be an intensional subset
	 * of B's {namespace constraint} as defined by Wildcard Subset ($3.10.6)."
	 */
	if(xmlSchemaCheckCOSNSSubset((xmlSchemaWildcardPtr)r->children, (xmlSchemaWildcardPtr)b->children))
		return 1;
	/*
	 * SPEC (3) "Unless B is the content model wildcard of the `ur-type
	 * definition`, R's {process contents} must be identical to or stronger
	 * than B's {process contents}, where strict is stronger than lax is
	 * stronger than skip."
	 */
	if(!isAnyTypeBase) {
		if(((xmlSchemaWildcardPtr)r->children)->processContents < ((xmlSchemaWildcardPtr)b->children)->processContents)
			return 1;
	}
	return 0;
}
/**
 * xmlSchemaCheckCOSParticleRestrict:
 * @ctxt:  the schema parser context
 * @type:  the complex type definition
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Particle Valid (Restriction) (cos-particle-restrict)
 *
 * STATUS: TODO
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckCOSParticleRestrict(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr r, xmlSchemaParticlePtr b)
{
	int ret = 0;
	/*part = WXS_TYPE_PARTICLE(type);
	   basePart = WXS_TYPE_PARTICLE(base);
	 */
	TODO
	/*
	 * SPEC (1) "They are the same particle."
	 */
	if(r == b)
		return 0;
	return 0;
}
#if 0
/**
 * xmlSchemaCheckRCaseNSRecurseCheckCardinality:
 * @ctxt:  the schema parser context
 * @r: the model group particle
 * @b: the base wildcard particle
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Particle Derivation OK (All/Choice/Sequence:Any --
 *                         NSRecurseCheckCardinality)
 * (rcase-NSRecurseCheckCardinality)
 *
 * STATUS: TODO: subst-groups
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckRCaseNSRecurseCheckCardinality(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr r, xmlSchemaParticlePtr b)
{
	xmlSchemaParticlePtr part;
	/* @todo Error codes (rcase-NSRecurseCheckCardinality). */
	if((r->children == NULL) || (r->children->children == NULL))
		return -1;
	/*
	 * SPEC "For a group particle to be a `valid restriction` of a
	 * wildcard particle..."
	 *
	 * SPEC (1) "Every member of the {particles} of the group is a `valid
	 * restriction` of the wildcard as defined by
	 * Particle Valid (Restriction) ($3.9.6)."
	 */
	part = (xmlSchemaParticlePtr)r->children->children;
	do {
		if(xmlSchemaCheckCOSParticleRestrict(ctxt, part, b))
			return 1;
		part = (xmlSchemaParticlePtr)part->next;
	} while(part);
	/*
	 * SPEC (2) "The effective total range of the group [...] is a
	 * valid restriction of B's occurrence range as defined by
	 * Occurrence Range OK ($3.9.6)."
	 */
	if(xmlSchemaCheckParticleRangeOK(
		    xmlSchemaGetParticleTotalRangeMin(r),
		    xmlSchemaGetParticleTotalRangeMax(r),
		    b->minOccurs, b->maxOccurs) != 0)
		return 1;
	return 0;
}

#endif

/**
 * xmlSchemaCheckRCaseRecurse:
 * @ctxt:  the schema parser context
 * @r: the <all> or <sequence> model group particle
 * @b: the base <all> or <sequence> model group particle
 *
 * (3.9.6) Constraints on Particle Schema Components
 * Schema Component Constraint:
 * Particle Derivation OK (All:All,Sequence:Sequence --
                           Recurse)
 * (rcase-Recurse)
 *
 * STATUS:  ?
 * @todo subst-groups
 *
 * Returns 0 if the constraints are satisfied, a positive
 * error code if not and -1 if an internal error occured.
 */
static int xmlSchemaCheckRCaseRecurse(xmlSchemaParserCtxtPtr ctxt, xmlSchemaParticlePtr r, xmlSchemaParticlePtr b)
{
	/* xmlSchemaParticlePtr part; */
	/* @todo Error codes (rcase-Recurse). */
	if((r->children == NULL) || (b->children == NULL) ||
	    (r->children->type != b->children->type))
		return -1;
	/*
	 * SPEC "For an all or sequence group particle to be a `valid
	 * restriction` of another group particle with the same {compositor}..."
	 *
	 * SPEC (1) "R's occurrence range is a valid restriction of B's
	 * occurrence range as defined by Occurrence Range OK ($3.9.6)."
	 */
	if(xmlSchemaCheckParticleRangeOK(r->minOccurs, r->maxOccurs, b->minOccurs, b->maxOccurs))
		return 1;
	return 0;
}

#endif

#define FACET_RESTR_MUTUAL_ERR(fac1, fac2) \
	xmlSchemaPCustomErrExt(pctxt, XML_SCHEMAP_INVALID_FACET_VALUE, WXS_BASIC_CAST fac1, fac1->P_Node, \
	    "It is an error for both '%s' and '%s' to be specified on the same type definition", \
	    BAD_CAST xmlSchemaFacetTypeToString(fac1->type), BAD_CAST xmlSchemaFacetTypeToString(fac2->type), 0);
#define FACET_RESTR_ERR(fac1, msg) \
	xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_INVALID_FACET_VALUE, WXS_BASIC_CAST fac1, fac1->P_Node, msg, 0);
#define FACET_RESTR_FIXED_ERR(fac) \
	xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_INVALID_FACET_VALUE, \
	    WXS_BASIC_CAST fac, fac->P_Node, "The base type's facet is 'fixed', thus the value must not differ", 0);

static void xmlSchemaDeriveFacetErr(xmlSchemaParserCtxtPtr pctxt,
    xmlSchemaFacetPtr facet1, xmlSchemaFacetPtr facet2, int lessGreater, int orEqual, int ofBase)
{
	xmlChar * msg = sstrdup(BAD_CAST "'");
	msg = xmlStrcat(msg, xmlSchemaFacetTypeToString(facet1->type));
	msg = xmlStrcat(msg, BAD_CAST "' has to be");
	if(lessGreater == 0)
		msg = xmlStrcat(msg, BAD_CAST " equal to");
	if(lessGreater == 1)
		msg = xmlStrcat(msg, BAD_CAST " greater than");
	else
		msg = xmlStrcat(msg, BAD_CAST " less than");
	if(orEqual)
		msg = xmlStrcat(msg, BAD_CAST " or equal to");
	msg = xmlStrcat(msg, BAD_CAST " '");
	msg = xmlStrcat(msg, xmlSchemaFacetTypeToString(facet2->type));
	if(ofBase)
		msg = xmlStrcat(msg, BAD_CAST "' of the base type");
	else
		msg = xmlStrcat(msg, BAD_CAST "'");
	xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_INVALID_FACET_VALUE, WXS_BASIC_CAST facet1, NULL, (const char*)msg, 0);
	SAlloc::F(msg);
}

/*
 * xmlSchemaDeriveAndValidateFacets:
 *
 * Schema Component Constraint: Simple Type Restriction (Facets)
 * (st-restrict-facets)
 */
static int xmlSchemaDeriveAndValidateFacets(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	xmlSchemaTypePtr base = type->baseType;
	xmlSchemaFacetLinkPtr link, cur, last = NULL;
	xmlSchemaFacetPtr facet, bfacet,
	    flength = NULL, ftotdig = NULL, ffracdig = NULL,
	    fmaxlen = NULL, fminlen = NULL, /* facets of the current type */
	    fmininc = NULL, fmaxinc = NULL,
	    fminexc = NULL, fmaxexc = NULL,
	    bflength = NULL, bftotdig = NULL, bffracdig = NULL,
	    bfmaxlen = NULL, bfminlen = NULL, /* facets of the base type */
	    bfmininc = NULL, bfmaxinc = NULL,
	    bfminexc = NULL, bfmaxexc = NULL;
	int res; /* err = 0, fixedErr; */

	/*
	 * SPEC st-restrict-facets 1:
	 * "The {variety} of R is the same as that of B."
	 */
	/*
	 * SPEC st-restrict-facets 2:
	 * "If {variety} is atomic, the {primitive type definition}
	 * of R is the same as that of B."
	 *
	 * NOTE: we leave 1 & 2 out for now, since this will be
	 * satisfied by the derivation process.
	 * CONSTRUCTION TODO: Maybe needed if using a construction API.
	 */
	/*
	 * SPEC st-restrict-facets 3:
	 * "The {facets} of R are the union of S and the {facets}
	 * of B, eliminating duplicates. To eliminate duplicates,
	 * when a facet of the same kind occurs in both S and the
	 * {facets} of B, the one in the {facets} of B is not
	 * included, with the exception of enumeration and pattern
	 * facets, for which multiple occurrences with distinct values
	 * are allowed."
	 */

	if((type->facetSet == NULL) && (base->facetSet == NULL))
		return 0;
	last = type->facetSet;
	if(last)
		while(last->next)
			last = last->next;
	for(cur = type->facetSet; cur; cur = cur->next) {
		facet = cur->facet;
		switch(facet->type) {
			case XML_SCHEMA_FACET_LENGTH: flength = facet; break;
			case XML_SCHEMA_FACET_MINLENGTH: fminlen = facet; break;
			case XML_SCHEMA_FACET_MININCLUSIVE: fmininc = facet; break;
			case XML_SCHEMA_FACET_MINEXCLUSIVE: fminexc = facet; break;
			case XML_SCHEMA_FACET_MAXLENGTH: fmaxlen = facet; break;
			case XML_SCHEMA_FACET_MAXINCLUSIVE: fmaxinc = facet; break;
			case XML_SCHEMA_FACET_MAXEXCLUSIVE: fmaxexc = facet; break;
			case XML_SCHEMA_FACET_TOTALDIGITS: ftotdig = facet; break;
			case XML_SCHEMA_FACET_FRACTIONDIGITS: ffracdig = facet; break;
			default: break;
		}
	}
	for(cur = base->facetSet; cur; cur = cur->next) {
		facet = cur->facet;
		switch(facet->type) {
			case XML_SCHEMA_FACET_LENGTH: bflength = facet; break;
			case XML_SCHEMA_FACET_MINLENGTH: bfminlen = facet; break;
			case XML_SCHEMA_FACET_MININCLUSIVE: bfmininc = facet; break;
			case XML_SCHEMA_FACET_MINEXCLUSIVE: bfminexc = facet; break;
			case XML_SCHEMA_FACET_MAXLENGTH: bfmaxlen = facet; break;
			case XML_SCHEMA_FACET_MAXINCLUSIVE: bfmaxinc = facet; break;
			case XML_SCHEMA_FACET_MAXEXCLUSIVE: bfmaxexc = facet; break;
			case XML_SCHEMA_FACET_TOTALDIGITS: bftotdig = facet; break;
			case XML_SCHEMA_FACET_FRACTIONDIGITS: bffracdig = facet; break;
			default: break;
		}
	}
	/*
	 * length and minLength or maxLength (2.2) + (3.2)
	 */
	if(flength && (fminlen || fmaxlen)) {
		FACET_RESTR_ERR(flength, "It is an error for both 'length' and either of 'minLength' or 'maxLength' to be specified on the same type definition")
	}
	/*
	 * Mutual exclusions in the same derivation step.
	 */
	if((fmaxinc) && (fmaxexc)) {
		/*
		 * SCC "maxInclusive and maxExclusive"
		 */
		FACET_RESTR_MUTUAL_ERR(fmaxinc, fmaxexc)
	}
	if((fmininc) && (fminexc)) {
		/*
		 * SCC "minInclusive and minExclusive"
		 */
		FACET_RESTR_MUTUAL_ERR(fmininc, fminexc)
	}
	if(flength && bflength) {
		/*
		 * SCC "length valid restriction"
		 * The values have to be equal.
		 */
		res = xmlSchemaCompareValues(flength->val, bflength->val);
		if(res == -2)
			goto internal_error;
		if(res != 0)
			xmlSchemaDeriveFacetErr(pctxt, flength, bflength, 0, 0, 1);
		if((res != 0) && (bflength->fixed)) {
			FACET_RESTR_FIXED_ERR(flength)
		}
	}
	if(fminlen && bfminlen) {
		/*
		 * SCC "minLength valid restriction"
		 * minLength >= BASE minLength
		 */
		res = xmlSchemaCompareValues(fminlen->val, bfminlen->val);
		if(res == -2)
			goto internal_error;
		if(res == -1)
			xmlSchemaDeriveFacetErr(pctxt, fminlen, bfminlen, 1, 1, 1);
		if((res != 0) && (bfminlen->fixed)) {
			FACET_RESTR_FIXED_ERR(fminlen)
		}
	}
	if(fmaxlen && bfmaxlen) {
		/*
		 * SCC "maxLength valid restriction"
		 * maxLength <= BASE minLength
		 */
		res = xmlSchemaCompareValues(fmaxlen->val, bfmaxlen->val);
		if(res == -2)
			goto internal_error;
		if(res == 1)
			xmlSchemaDeriveFacetErr(pctxt, fmaxlen, bfmaxlen, -1, 1, 1);
		if((res != 0) && (bfmaxlen->fixed)) {
			FACET_RESTR_FIXED_ERR(fmaxlen)
		}
	}
	/*
	 * SCC "length and minLength or maxLength"
	 */
	SETIFZ(flength, bflength);
	if(flength) {
		SETIFZ(fminlen, bfminlen);
		if(fminlen) {
			// (1.1) length >= minLength 
			res = xmlSchemaCompareValues(flength->val, fminlen->val);
			if(res == -2)
				goto internal_error;
			if(res == -1)
				xmlSchemaDeriveFacetErr(pctxt, flength, fminlen, 1, 1, 0);
		}
		SETIFZ(fmaxlen, bfmaxlen);
		if(fmaxlen) {
			// (2.1) length <= maxLength 
			res = xmlSchemaCompareValues(flength->val, fmaxlen->val);
			if(res == -2)
				goto internal_error;
			if(res == 1)
				xmlSchemaDeriveFacetErr(pctxt, flength, fmaxlen, -1, 1, 0);
		}
	}
	if(fmaxinc) {
		/*
		 * "maxInclusive"
		 */
		if(fmininc) {
			/* SCC "maxInclusive >= minInclusive" */
			res = xmlSchemaCompareValues(fmaxinc->val, fmininc->val);
			if(res == -2)
				goto internal_error;
			if(res == -1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxinc, fmininc, 1, 1, 0);
			}
		}
		/*
		 * SCC "maxInclusive valid restriction"
		 */
		if(bfmaxinc) {
			/* maxInclusive <= BASE maxInclusive */
			res = xmlSchemaCompareValues(fmaxinc->val, bfmaxinc->val);
			if(res == -2)
				goto internal_error;
			if(res == 1)
				xmlSchemaDeriveFacetErr(pctxt, fmaxinc, bfmaxinc, -1, 1, 1);
			if((res != 0) && (bfmaxinc->fixed)) {
				FACET_RESTR_FIXED_ERR(fmaxinc)
			}
		}
		if(bfmaxexc) {
			/* maxInclusive < BASE maxExclusive */
			res = xmlSchemaCompareValues(fmaxinc->val, bfmaxexc->val);
			if(res == -2)
				goto internal_error;
			if(res != -1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxinc, bfmaxexc, -1, 0, 1);
			}
		}
		if(bfmininc) {
			/* maxInclusive >= BASE minInclusive */
			res = xmlSchemaCompareValues(fmaxinc->val, bfmininc->val);
			if(res == -2)
				goto internal_error;
			if(res == -1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxinc, bfmininc, 1, 1, 1);
			}
		}
		if(bfminexc) {
			/* maxInclusive > BASE minExclusive */
			res = xmlSchemaCompareValues(fmaxinc->val, bfminexc->val);
			if(res == -2)
				goto internal_error;
			if(res != 1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxinc, bfminexc, 1, 0, 1);
			}
		}
	}
	if(fmaxexc) {
		/*
		 * "maxExclusive >= minExclusive"
		 */
		if(fminexc) {
			res = xmlSchemaCompareValues(fmaxexc->val, fminexc->val);
			if(res == -2)
				goto internal_error;
			if(res == -1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxexc, fminexc, 1, 1, 0);
			}
		}
		/*
		 * "maxExclusive valid restriction"
		 */
		if(bfmaxexc) {
			/* maxExclusive <= BASE maxExclusive */
			res = xmlSchemaCompareValues(fmaxexc->val, bfmaxexc->val);
			if(res == -2)
				goto internal_error;
			if(res == 1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxexc, bfmaxexc, -1, 1, 1);
			}
			if((res != 0) && (bfmaxexc->fixed)) {
				FACET_RESTR_FIXED_ERR(fmaxexc)
			}
		}
		if(bfmaxinc) {
			/* maxExclusive <= BASE maxInclusive */
			res = xmlSchemaCompareValues(fmaxexc->val, bfmaxinc->val);
			if(res == -2)
				goto internal_error;
			if(res == 1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxexc, bfmaxinc, -1, 1, 1);
			}
		}
		if(bfmininc) {
			/* maxExclusive > BASE minInclusive */
			res = xmlSchemaCompareValues(fmaxexc->val, bfmininc->val);
			if(res == -2)
				goto internal_error;
			if(res != 1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxexc, bfmininc, 1, 0, 1);
			}
		}
		if(bfminexc) {
			/* maxExclusive > BASE minExclusive */
			res = xmlSchemaCompareValues(fmaxexc->val, bfminexc->val);
			if(res == -2)
				goto internal_error;
			if(res != 1) {
				xmlSchemaDeriveFacetErr(pctxt, fmaxexc, bfminexc, 1, 0, 1);
			}
		}
	}
	if(fminexc) {
		/*
		 * "minExclusive < maxInclusive"
		 */
		if(fmaxinc) {
			res = xmlSchemaCompareValues(fminexc->val, fmaxinc->val);
			if(res == -2)
				goto internal_error;
			if(res != -1) {
				xmlSchemaDeriveFacetErr(pctxt, fminexc, fmaxinc, -1, 0, 0);
			}
		}
		/*
		 * "minExclusive valid restriction"
		 */
		if(bfminexc) {
			/* minExclusive >= BASE minExclusive */
			res = xmlSchemaCompareValues(fminexc->val, bfminexc->val);
			if(res == -2)
				goto internal_error;
			if(res == -1) {
				xmlSchemaDeriveFacetErr(pctxt, fminexc, bfminexc, 1, 1, 1);
			}
			if((res != 0) && (bfminexc->fixed)) {
				FACET_RESTR_FIXED_ERR(fminexc)
			}
		}
		if(bfmaxinc) {
			/* minExclusive <= BASE maxInclusive */
			res = xmlSchemaCompareValues(fminexc->val, bfmaxinc->val);
			if(res == -2)
				goto internal_error;
			if(res == 1) {
				xmlSchemaDeriveFacetErr(pctxt, fminexc, bfmaxinc, -1, 1, 1);
			}
		}
		if(bfmininc) {
			/* minExclusive >= BASE minInclusive */
			res = xmlSchemaCompareValues(fminexc->val, bfmininc->val);
			if(res == -2)
				goto internal_error;
			if(res == -1) {
				xmlSchemaDeriveFacetErr(pctxt, fminexc, bfmininc, 1, 1, 1);
			}
		}
		if(bfmaxexc) {
			/* minExclusive < BASE maxExclusive */
			res = xmlSchemaCompareValues(fminexc->val, bfmaxexc->val);
			if(res == -2)
				goto internal_error;
			if(res != -1) {
				xmlSchemaDeriveFacetErr(pctxt, fminexc, bfmaxexc, -1, 0, 1);
			}
		}
	}
	if(fmininc) {
		/*
		 * "minInclusive < maxExclusive"
		 */
		if(fmaxexc) {
			res = xmlSchemaCompareValues(fmininc->val, fmaxexc->val);
			if(res == -2)
				goto internal_error;
			if(res != -1) {
				xmlSchemaDeriveFacetErr(pctxt, fmininc, fmaxexc, -1, 0, 0);
			}
		}
		/*
		 * "minExclusive valid restriction"
		 */
		if(bfmininc) {
			/* minInclusive >= BASE minInclusive */
			res = xmlSchemaCompareValues(fmininc->val, bfmininc->val);
			if(res == -2)
				goto internal_error;
			if(res == -1) {
				xmlSchemaDeriveFacetErr(pctxt, fmininc, bfmininc, 1, 1, 1);
			}
			if((res != 0) && (bfmininc->fixed)) {
				FACET_RESTR_FIXED_ERR(fmininc)
			}
		}
		if(bfmaxinc) {
			/* minInclusive <= BASE maxInclusive */
			res = xmlSchemaCompareValues(fmininc->val, bfmaxinc->val);
			if(res == -2)
				goto internal_error;
			if(res == 1) {
				xmlSchemaDeriveFacetErr(pctxt, fmininc, bfmaxinc, -1, 1, 1);
			}
		}
		if(bfminexc) {
			/* minInclusive > BASE minExclusive */
			res = xmlSchemaCompareValues(fmininc->val, bfminexc->val);
			if(res == -2)
				goto internal_error;
			if(res != 1)
				xmlSchemaDeriveFacetErr(pctxt, fmininc, bfminexc, 1, 0, 1);
		}
		if(bfmaxexc) {
			/* minInclusive < BASE maxExclusive */
			res = xmlSchemaCompareValues(fmininc->val, bfmaxexc->val);
			if(res == -2)
				goto internal_error;
			if(res != -1)
				xmlSchemaDeriveFacetErr(pctxt, fmininc, bfmaxexc, -1, 0, 1);
		}
	}
	if(ftotdig && bftotdig) {
		//
		// SCC " totalDigits valid restriction" totalDigits <= BASE totalDigits
		//
		res = xmlSchemaCompareValues(ftotdig->val, bftotdig->val);
		if(res == -2)
			goto internal_error;
		if(res == 1)
			xmlSchemaDeriveFacetErr(pctxt, ftotdig, bftotdig, -1, 1, 1);
		if((res != 0) && (bftotdig->fixed)) {
			FACET_RESTR_FIXED_ERR(ftotdig)
		}
	}
	if(ffracdig && bffracdig) {
		/*
		 * SCC  "fractionDigits valid restriction"
		 * fractionDigits <= BASE fractionDigits
		 */
		res = xmlSchemaCompareValues(ffracdig->val, bffracdig->val);
		if(res == -2)
			goto internal_error;
		if(res == 1)
			xmlSchemaDeriveFacetErr(pctxt, ffracdig, bffracdig, -1, 1, 1);
		if((res != 0) && (bffracdig->fixed)) {
			FACET_RESTR_FIXED_ERR(ffracdig)
		}
	}
	/*
	 * SCC "fractionDigits less than or equal to totalDigits"
	 */
	SETIFZ(ftotdig, bftotdig);
	SETIFZ(ffracdig, bffracdig);
	if(ftotdig && ffracdig) {
		res = xmlSchemaCompareValues(ffracdig->val, ftotdig->val);
		if(res == -2)
			goto internal_error;
		if(res == 1)
			xmlSchemaDeriveFacetErr(pctxt, ffracdig, ftotdig, -1, 1, 0);
	}
	/*
	 * *Enumerations* won' be added here, since only the first set
	 * of enumerations in the ancestor-or-self axis is used
	 * for validation, plus we need to use the base type of those
	 * enumerations for whitespace.
	 *
	 * *Patterns*: won't be add here, since they are ORed at
	 * type level and ANDed at ancestor level. This will
	 * happed during validation by walking the base axis
	 * of the type.
	 */
	for(cur = base->facetSet; cur; cur = cur->next) {
		bfacet = cur->facet;
		/*
		 * Special handling of enumerations and patterns.
		 * @todo hmm, they should not appear in the set, so remove this.
		 */
		if(oneof2(bfacet->type, XML_SCHEMA_FACET_PATTERN, XML_SCHEMA_FACET_ENUMERATION))
			continue;
		/*
		 * Search for a duplicate facet in the current type.
		 */
		link = type->facetSet;
		/* err = 0; */
		/* fixedErr = 0; */
		while(link) {
			facet = link->facet;
			if(facet->type == bfacet->type) {
				switch(facet->type) {
					case XML_SCHEMA_FACET_WHITESPACE:
					    /*
					     * The whitespace must be stronger.
					     */
					    if(facet->whitespace < bfacet->whitespace) {
						    FACET_RESTR_ERR(facet, "The 'whitespace' value has to be equal to or stronger than the 'whitespace' value of the base type")
					    }
					    if((bfacet->fixed) && (facet->whitespace != bfacet->whitespace)) {
						    FACET_RESTR_FIXED_ERR(facet)
					    }
					    break;
					default:
					    break;
				}
				/* Duplicate found. */
				break;
			}
			link = link->next;
		}
		/*
		 * If no duplicate was found: add the base types's facet
		 * to the set.
		 */
		if(link == NULL) {
			link = (xmlSchemaFacetLinkPtr)SAlloc::M(sizeof(xmlSchemaFacetLink));
			if(link == NULL) {
				xmlSchemaPErrMemory(pctxt, "deriving facets, creating a facet link", 0);
				return -1;
			}
			link->facet = cur->facet;
			link->next = NULL;
			if(last == NULL)
				type->facetSet = link;
			else
				last->next = link;
			last = link;
		}
	}
	return 0;
internal_error:
	PERROR_INT("xmlSchemaDeriveAndValidateFacets", "an error occured");
	return -1;
}

static int xmlSchemaFinishMemberTypeDefinitionsProperty(xmlSchemaParserCtxtPtr pctxt,
    xmlSchemaTypePtr type)
{
	xmlSchemaTypeLinkPtr link, lastLink, prevLink, subLink, newLink;
	/*
	 * The actual value is then formed by replacing any union type
	 * definition in the `explicit members` with the members of their
	 * {member type definitions}, in order.
	 *
	 * @todo There's a bug entry at
	 * "http://lists.w3.org/Archives/Public/www-xml-schema-comments/2005JulSep/0287.html"
	 * which indicates that we'll keep the union types the future.
	 */
	link = type->memberTypes;
	while(link) {
		if(WXS_IS_TYPE_NOT_FIXED(link->type))
			xmlSchemaTypeFixup(link->type, ACTXT_CAST pctxt);
		if(WXS_IS_UNION(link->type)) {
			subLink = xmlSchemaGetUnionSimpleTypeMemberTypes(link->type);
			if(subLink) {
				link->type = subLink->type;
				if(subLink->next) {
					lastLink = link->next;
					subLink = subLink->next;
					prevLink = link;
					while(subLink) {
						newLink = (xmlSchemaTypeLinkPtr)SAlloc::M(sizeof(xmlSchemaTypeLink));
						if(newLink == NULL) {
							xmlSchemaPErrMemory(pctxt, "allocating a type link", NULL);
							return -1;
						}
						newLink->type = subLink->type;
						prevLink->next = newLink;
						prevLink = newLink;
						newLink->next = lastLink;

						subLink = subLink->next;
					}
				}
			}
		}
		link = link->next;
	}
	return 0;
}

static void xmlSchemaTypeFixupOptimFacets(xmlSchemaTypePtr type)
{
	int needVal = 0, normVal = 0;
	int has = (type->baseType->flags & XML_SCHEMAS_TYPE_HAS_FACETS) ? 1 : 0;
	if(has) {
		needVal = (type->baseType->flags & XML_SCHEMAS_TYPE_FACETSNEEDVALUE) ? 1 : 0;
		normVal = (type->baseType->flags & XML_SCHEMAS_TYPE_NORMVALUENEEDED) ? 1 : 0;
	}
	if(type->facets) {
		xmlSchemaFacetPtr fac;
		for(fac = type->facets; fac; fac = fac->next) {
			switch(fac->type) {
				case XML_SCHEMA_FACET_WHITESPACE:
				    break;
				case XML_SCHEMA_FACET_PATTERN:
				    normVal = 1;
				    has = 1;
				    break;
				case XML_SCHEMA_FACET_ENUMERATION:
				    needVal = 1;
				    normVal = 1;
				    has = 1;
				    break;
				default:
				    has = 1;
				    break;
			}
		}
	}
	if(normVal)
		type->flags |= XML_SCHEMAS_TYPE_NORMVALUENEEDED;
	if(needVal)
		type->flags |= XML_SCHEMAS_TYPE_FACETSNEEDVALUE;
	if(has)
		type->flags |= XML_SCHEMAS_TYPE_HAS_FACETS;

	if(has && (!needVal) && WXS_IS_ATOMIC(type)) {
		xmlSchemaTypePtr prim = xmlSchemaGetPrimitiveType(type);
		/*
		 * OPTIMIZE VAL TODO: Some facets need a computed value.
		 */
		if((prim->builtInType != XML_SCHEMAS_ANYSIMPLETYPE) &&
		    (prim->builtInType != XML_SCHEMAS_STRING)) {
			type->flags |= XML_SCHEMAS_TYPE_FACETSNEEDVALUE;
		}
	}
}

static int xmlSchemaTypeFixupWhitespace(xmlSchemaTypePtr type)
{
	/*
	 * Evaluate the whitespace-facet value.
	 */
	if(WXS_IS_LIST(type)) {
		type->flags |= XML_SCHEMAS_TYPE_WHITESPACE_COLLAPSE;
		return 0;
	}
	else if(WXS_IS_UNION(type))
		return 0;

	if(type->facetSet) {
		xmlSchemaFacetLinkPtr lin;
		for(lin = type->facetSet; lin != NULL; lin = lin->next) {
			if(lin->facet->type == XML_SCHEMA_FACET_WHITESPACE) {
				switch(lin->facet->whitespace) {
					case XML_SCHEMAS_FACET_PRESERVE: type->flags |= XML_SCHEMAS_TYPE_WHITESPACE_PRESERVE; break;
					case XML_SCHEMAS_FACET_REPLACE: type->flags |= XML_SCHEMAS_TYPE_WHITESPACE_REPLACE; break;
					case XML_SCHEMAS_FACET_COLLAPSE: type->flags |= XML_SCHEMAS_TYPE_WHITESPACE_COLLAPSE; break;
					default: return -1;
				}
				return 0;
			}
		}
	}
	/*
	 * For all `atomic` datatypes other than string (and types `derived`
	 * by `restriction` from it) the value of whiteSpace is fixed to
	 * collapse
	 */
	{
		xmlSchemaTypePtr anc;

		for(anc = type->baseType; anc && anc->builtInType != XML_SCHEMAS_ANYTYPE; anc = anc->baseType) {
			if(anc->type == XML_SCHEMA_TYPE_BASIC) {
				if(anc->builtInType == XML_SCHEMAS_NORMSTRING) {
					type->flags |= XML_SCHEMAS_TYPE_WHITESPACE_REPLACE;
				}
				else if((anc->builtInType == XML_SCHEMAS_STRING) ||
				    (anc->builtInType == XML_SCHEMAS_ANYSIMPLETYPE)) {
					type->flags |= XML_SCHEMAS_TYPE_WHITESPACE_PRESERVE;
				}
				else
					type->flags |= XML_SCHEMAS_TYPE_WHITESPACE_COLLAPSE;
				break;
			}
		}
	}
	return 0;
}

static int xmlSchemaFixupSimpleTypeStageOne(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	if(type->type != XML_SCHEMA_TYPE_SIMPLE)
		return 0;
	if(!WXS_IS_TYPE_NOT_FIXED_1(type))
		return 0;
	type->flags |= XML_SCHEMAS_TYPE_FIXUP_1;
	if(WXS_IS_LIST(type)) {
		/*
		 * Corresponds to <simpleType><list>...
		 */
		if(type->subtypes == NULL) {
			/*
			 * This one is really needed, so get out.
			 */
			PERROR_INT("xmlSchemaFixupSimpleTypeStageOne", "list type has no item-type assigned");
			return -1;
		}
	}
	else if(WXS_IS_UNION(type)) {
		/*
		 * Corresponds to <simpleType><union>...
		 */
		if(type->memberTypes == NULL) {
			/*
			 * This one is really needed, so get out.
			 */
			PERROR_INT("xmlSchemaFixupSimpleTypeStageOne", "union type has no member-types assigned");
			return -1;
		}
	}
	else {
		/*
		 * Corresponds to <simpleType><restriction>...
		 */
		if(type->baseType == NULL) {
			PERROR_INT("xmlSchemaFixupSimpleTypeStageOne", "type has no base-type assigned");
			return -1;
		}
		if(WXS_IS_TYPE_NOT_FIXED_1(type->baseType))
			if(xmlSchemaFixupSimpleTypeStageOne(pctxt, type->baseType) == -1)
				return -1;
		/*
		 * Variety
		 * If the <restriction> alternative is chosen, then the
		 * {variety} of the {base type definition}.
		 */
		if(WXS_IS_ATOMIC(type->baseType))
			type->flags |= XML_SCHEMAS_TYPE_VARIETY_ATOMIC;
		else if(WXS_IS_LIST(type->baseType)) {
			type->flags |= XML_SCHEMAS_TYPE_VARIETY_LIST;
			/*
			 * Inherit the itemType.
			 */
			type->subtypes = type->baseType->subtypes;
		}
		else if(WXS_IS_UNION(type->baseType)) {
			type->flags |= XML_SCHEMAS_TYPE_VARIETY_UNION;
			/*
			 * NOTE that we won't assign the memberTypes of the base,
			 * since this will make trouble when freeing them; we will
			 * use a lookup function to access them instead.
			 */
		}
	}
	return 0;
}

#ifdef DEBUG_TYPE
static void xmlSchemaDebugFixedType(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	if(type->node) {
		xmlGenericError(0, "Type of %s : %s:%d :", name, type->node->doc->URL, xmlGetLineNo(type->node));
	}
	else {
		xmlGenericError(0, "Type of %s :", name);
	}
	if((WXS_IS_SIMPLE(type)) || (WXS_IS_COMPLEX(type))) {
		switch(type->contentType) {
			case XML_SCHEMA_CONTENT_SIMPLE:
			    xmlGenericError(0, "simple\n");
			    break;
			case XML_SCHEMA_CONTENT_ELEMENTS:
			    xmlGenericError(0, "elements\n");
			    break;
			case XML_SCHEMA_CONTENT_UNKNOWN:
			    xmlGenericError(0, "unknown !!!\n");
			    break;
			case XML_SCHEMA_CONTENT_EMPTY:
			    xmlGenericError(0, "empty\n");
			    break;
			case XML_SCHEMA_CONTENT_MIXED:
			    if(xmlSchemaIsParticleEmptiable((xmlSchemaParticlePtr)type->subtypes))
				    xmlGenericError(0, "mixed as emptiable particle\n");
			    else
				    xmlGenericError(0, "mixed\n");
			    break;
			/* Removed, since not used. */
			/*
			   case XML_SCHEMA_CONTENT_MIXED_OR_ELEMENTS:
			   xmlGenericError(0, "mixed or elems\n");
			   break;
			 */
			case XML_SCHEMA_CONTENT_BASIC:
			    xmlGenericError(0, "basic\n");
			    break;
			default:
			    xmlGenericError(0, "not registered !!!\n");
			    break;
		}
	}
}

#endif

/*
 * 3.14.6 Constraints on Simple Type Definition Schema Components
 */
static int xmlSchemaFixupSimpleTypeStageTwo(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	int res, olderrs = pctxt->nberrors;
	if(type->type != XML_SCHEMA_TYPE_SIMPLE)
		return -1;
	if(!WXS_IS_TYPE_NOT_FIXED(type))
		return 0;
	type->flags |= XML_SCHEMAS_TYPE_INTERNAL_RESOLVED;
	type->contentType = XML_SCHEMA_CONTENT_SIMPLE;
	if(type->baseType == NULL) {
		PERROR_INT("xmlSchemaFixupSimpleTypeStageTwo", "missing baseType");
		goto exit_failure;
	}
	if(WXS_IS_TYPE_NOT_FIXED(type->baseType))
		xmlSchemaTypeFixup(type->baseType, ACTXT_CAST pctxt);
	/*
	 * If a member type of a union is a union itself, we need to substitute
	 * that member type for its member types.
	 * NOTE that this might change in WXS 1.1; i.e. we will keep the union
	 * types in WXS 1.1.
	 */
	if(type->memberTypes && (xmlSchemaFinishMemberTypeDefinitionsProperty(pctxt, type) == -1))
		return -1;
	/*
	 * SPEC src-simple-type 1
	 * "The corresponding simple type definition, if any, must satisfy
	 * the conditions set out in Constraints on Simple Type Definition
	 * Schema Components ($3.14.6)."
	 */
	/*
	 * Schema Component Constraint: Simple Type Definition Properties Correct
	 * (st-props-correct)
	 */
	res = xmlSchemaCheckSTPropsCorrect(pctxt, type);
	HFAILURE HERROR
	/*
	 * Schema Component Constraint: Derivation Valid (Restriction, Simple)
	 * (cos-st-restricts)
	 */
	    res = xmlSchemaCheckCOSSTRestricts(pctxt, type);
	HFAILURE HERROR
	/*
	 * @todo Removed the error report, since it got annoying to get an
	 * extra error report, if anything failed until now.
	 * Enable this if needed.
	 *
	 * xmlSchemaPErr(ctxt, type->node,
	 *    XML_SCHEMAP_SRC_SIMPLE_TYPE_1,
	 *    "Simple type '%s' does not satisfy the constraints "
	 *    "on simple type definitions.\n",
	 *    type->name, 0);
	 */
	/*
	 * Schema Component Constraint: Simple Type Restriction (Facets)
	 * (st-restrict-facets)
	 */
	    res = xmlSchemaCheckFacetValues(type, pctxt);
	HFAILURE HERROR
	if(type->facetSet || type->baseType->facetSet) {
		res = xmlSchemaDeriveAndValidateFacets(pctxt, type);
		HFAILURE HERROR
	}
	/*
	 * Whitespace value.
	 */
	res = xmlSchemaTypeFixupWhitespace(type);
	HFAILURE HERROR xmlSchemaTypeFixupOptimFacets(type);

exit_error:
#ifdef DEBUG_TYPE
	xmlSchemaDebugFixedType(pctxt, type);
#endif
	if(olderrs != pctxt->nberrors)
		return(pctxt->err);
	return 0;

exit_failure:
#ifdef DEBUG_TYPE
	xmlSchemaDebugFixedType(pctxt, type);
#endif
	return -1;
}

static int xmlSchemaFixupComplexType(xmlSchemaParserCtxtPtr pctxt, xmlSchemaTypePtr type)
{
	int res = 0, olderrs = pctxt->nberrors;
	xmlSchemaTypePtr baseType = type->baseType;
	if(!WXS_IS_TYPE_NOT_FIXED(type))
		return 0;
	type->flags |= XML_SCHEMAS_TYPE_INTERNAL_RESOLVED;
	if(baseType == NULL) {
		PERROR_INT("xmlSchemaFixupComplexType", "missing baseType");
		goto exit_failure;
	}
	/*
	 * Fixup the base type.
	 */
	if(WXS_IS_TYPE_NOT_FIXED(baseType))
		xmlSchemaTypeFixup(baseType, ACTXT_CAST pctxt);
	if(baseType->flags & XML_SCHEMAS_TYPE_INTERNAL_INVALID) {
		/*
		 * Skip fixup if the base type is invalid.
		 * @todo Generate a warning!
		 */
		return 0;
	}
	/*
	 * This basically checks if the base type can be derived.
	 */
	res = xmlSchemaCheckSRCCT(pctxt, type);
	HFAILURE HERROR
	/*
	 * Fixup the content type.
	 */
	if(type->contentType == XML_SCHEMA_CONTENT_SIMPLE) {
		/*
		 * Corresponds to <complexType><simpleContent>...
		 */
		if((WXS_IS_COMPLEX(baseType)) && baseType->contentTypeDef && (WXS_IS_RESTRICTION(type))) {
			xmlSchemaTypePtr contentBase, content;
#ifdef ENABLE_NAMED_LOCALS
			char buf[30];
			const xmlChar * tmpname;
#endif
			/*
			 * SPEC (1) If <restriction> + base type is <complexType>,
			 * "whose own {content type} is a simple type..."
			 */
			if(type->contentTypeDef) {
				/*
				 * SPEC (1.1) "the simple type definition corresponding to the
				 * <simpleType> among the [children] of <restriction> if there
				 * is one;"
				 * Note that this "<simpleType> among the [children]" was put
				 * into ->contentTypeDef during parsing.
				 */
				contentBase = type->contentTypeDef;
				type->contentTypeDef = NULL;
			}
			else {
				/*
				 * (1.2) "...otherwise (<restriction> has no <simpleType>
				 * among its [children]), the simple type definition which
				 * is the {content type} of the ... base type."
				 */
				contentBase = baseType->contentTypeDef;
			}
			/*
			 * SPEC
			 * "... a simple type definition which restricts the simple
			 * type definition identified in clause 1.1 or clause 1.2
			 * with a set of facet components"
			 *
			 * Create the anonymous simple type, which will be the content
			 * type of the complex type.
			 */
#ifdef ENABLE_NAMED_LOCALS
			snprintf(buf, 29, "#scST%d", ++(pctxt->counter));
			tmpname = xmlDictLookupSL(pctxt->dict, BAD_CAST buf);
			content = xmlSchemaAddType(pctxt, pctxt->schema,
			    XML_SCHEMA_TYPE_SIMPLE, tmpname, type->targetNamespace,
			    type->node, 0);
#else
			content = xmlSchemaAddType(pctxt, pctxt->schema, XML_SCHEMA_TYPE_SIMPLE, NULL, type->targetNamespace, type->P_Node, 0);
#endif
			if(content == NULL)
				goto exit_failure;
			/*
			 * We will use the same node as for the <complexType>
			 * to have it somehow anchored in the schema doc.
			 */
			content->type = XML_SCHEMA_TYPE_SIMPLE;
			content->baseType = contentBase;
			/*
			 * Move the facets, previously anchored on the complexType during parsing.
			 */
			content->facets = type->facets;
			type->facets = NULL;
			content->facetSet = type->facetSet;
			type->facetSet = NULL;
			type->contentTypeDef = content;
			if(WXS_IS_TYPE_NOT_FIXED(contentBase))
				xmlSchemaTypeFixup(contentBase, ACTXT_CAST pctxt);
			/*
			 * Fixup the newly created type. We don't need to check
			 * for circularity here.
			 */
			res = xmlSchemaFixupSimpleTypeStageOne(pctxt, content);
			HFAILURE HERROR
			    res = xmlSchemaFixupSimpleTypeStageTwo(pctxt, content);
			HFAILURE HERROR
		}
		else if((WXS_IS_COMPLEX(baseType)) && (baseType->contentType == XML_SCHEMA_CONTENT_MIXED) && (WXS_IS_RESTRICTION(type))) {
			/*
			 * SPEC (2) If <restriction> + base is a mixed <complexType> with
			 * an emptiable particle, then a simple type definition which
			 * restricts the <restriction>'s <simpleType> child.
			 */
			if((type->contentTypeDef == NULL) || (type->contentTypeDef->baseType == NULL)) {
				/*
				 * @todo Check if this ever happens.
				 */
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_INTERNAL, WXS_BASIC_CAST type, NULL,
				    "Internal error: xmlSchemaTypeFixup, complex type '%s': the <simpleContent><restriction> "
				    "is missing a <simpleType> child, but was not catched by xmlSchemaCheckSRCCT()", type->name);
				goto exit_failure;
			}
		}
		else if((WXS_IS_COMPLEX(baseType)) && WXS_IS_EXTENSION(type)) {
			/*
			 * SPEC (3) If <extension> + base is <complexType> with
			 * <simpleType> content, "...then the {content type} of that
			 * complex type definition"
			 */
			if(baseType->contentTypeDef == NULL) {
				/*
				 * @todo Check if this ever happens. xmlSchemaCheckSRCCT
				 * should have catched this already.
				 */
				xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_INTERNAL, WXS_BASIC_CAST type, NULL,
				    "Internal error: xmlSchemaTypeFixup, complex type '%s': the <extension>ed base type is a complex type with no simple content type",
				    type->name);
				goto exit_failure;
			}
			type->contentTypeDef = baseType->contentTypeDef;
		}
		else if((WXS_IS_SIMPLE(baseType)) && WXS_IS_EXTENSION(type)) {
			/*
			 * SPEC (4) <extension> + base is <simpleType>
			 * "... then that simple type definition"
			 */
			type->contentTypeDef = baseType;
		}
		else {
			/*
			 * @todo Check if this ever happens.
			 */
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_INTERNAL, WXS_BASIC_CAST type, NULL,
			    "Internal error: xmlSchemaTypeFixup, complex type '%s' with <simpleContent>: unhandled derivation case", type->name);
			goto exit_failure;
		}
	}
	else {
		int dummySequence = 0;
		xmlSchemaParticlePtr particle = (xmlSchemaParticlePtr)type->subtypes;
		/*
		 * Corresponds to <complexType><complexContent>...
		 *
		 * NOTE that the effective mixed was already set during parsing of
		 * <complexType> and <complexContent>; its flag value is
		 * XML_SCHEMAS_TYPE_MIXED.
		 *
		 * Compute the "effective content":
		 * (2.1.1) + (2.1.2) + (2.1.3)
		 */
		if((particle == NULL) ||
		    ((particle->type == XML_SCHEMA_TYPE_PARTICLE) &&
			    ((particle->children->type == XML_SCHEMA_TYPE_ALL) ||
				    (particle->children->type == XML_SCHEMA_TYPE_SEQUENCE) ||
				    ((particle->children->type == XML_SCHEMA_TYPE_CHOICE) &&
					    (particle->minOccurs == 0))) &&
			    ( ((xmlSchemaTreeItem *)particle->children)->children == NULL))) {
			if(type->flags & XML_SCHEMAS_TYPE_MIXED) {
				/*
				 * SPEC (2.1.4) "If the `effective mixed` is true, then
				 * a particle whose properties are as follows:..."
				 *
				 * Empty sequence model group with
				 * minOccurs/maxOccurs = 1 (i.e. a "particle emptiable").
				 * NOTE that we sill assign it the <complexType> node to
				 * somehow anchor it in the doc.
				 */
				if(!particle || (particle->children->type != XML_SCHEMA_TYPE_SEQUENCE)) {
					/*
					 * Create the particle.
					 */
					particle = xmlSchemaAddParticle(pctxt, type->P_Node, 1, 1);
					if(particle == NULL)
						goto exit_failure;
					/*
					 * Create the model group.
					 */ /* URGENT TODO: avoid adding to pending items. */
					particle->children = (xmlSchemaTreeItem *)xmlSchemaAddModelGroup(pctxt, pctxt->schema, XML_SCHEMA_TYPE_SEQUENCE, type->P_Node);
					if(particle->children == NULL)
						goto exit_failure;
					type->subtypes = (xmlSchemaTypePtr)particle;
				}
				dummySequence = 1;
				type->contentType = XML_SCHEMA_CONTENT_ELEMENTS;
			}
			else {
				/*
				 * SPEC (2.1.5) "otherwise empty"
				 */
				type->contentType = XML_SCHEMA_CONTENT_EMPTY;
			}
		}
		else {
			/*
			 * SPEC (2.2) "otherwise the particle corresponding to the
			 * <all>, <choice>, <group> or <sequence> among the
			 * [children]."
			 */
			type->contentType = XML_SCHEMA_CONTENT_ELEMENTS;
		}
		/*
		 * Compute the "content type".
		 */
		if(WXS_IS_RESTRICTION(type)) {
			/*
			 * SPEC (3.1) "If <restriction>..."
			 * (3.1.1) + (3.1.2) */
			if(type->contentType != XML_SCHEMA_CONTENT_EMPTY) {
				if(type->flags & XML_SCHEMAS_TYPE_MIXED)
					type->contentType = XML_SCHEMA_CONTENT_MIXED;
			}
		}
		else {
			/*
			 * SPEC (3.2) "If <extension>..."
			 */
			if(type->contentType == XML_SCHEMA_CONTENT_EMPTY) {
				/*
				 * SPEC (3.2.1)
				 * "If the `effective content` is empty, then the
				 *  {content type} of the [...] base ..."
				 */
				type->contentType = baseType->contentType;
				type->subtypes = baseType->subtypes;
				/*
				 * Fixes bug #347316:
				 * This is the case when the base type has a simple
				 * type definition as content.
				 */
				type->contentTypeDef = baseType->contentTypeDef;
				/*
				 * NOTE that the effective mixed is ignored here.
				 */
			}
			else if(baseType->contentType == XML_SCHEMA_CONTENT_EMPTY) {
				/*
				 * SPEC (3.2.2)
				 */
				if(type->flags & XML_SCHEMAS_TYPE_MIXED)
					type->contentType = XML_SCHEMA_CONTENT_MIXED;
			}
			else {
				/*
				 * SPEC (3.2.3)
				 */
				if(type->flags & XML_SCHEMAS_TYPE_MIXED)
					type->contentType = XML_SCHEMA_CONTENT_MIXED;
				/*
				 * "A model group whose {compositor} is sequence and whose
				 * {particles} are..."
				 */
				if(WXS_TYPE_PARTICLE(type) && WXS_TYPE_PARTICLE_TERM(type) && ((WXS_TYPE_PARTICLE_TERM(type))->type == XML_SCHEMA_TYPE_ALL)) {
					/*
					 * SPEC cos-all-limited (1)
					 */
					xmlSchemaCustomErr(ACTXT_CAST pctxt, /* @todo error code */ XML_SCHEMAP_COS_ALL_LIMITED, WXS_ITEM_NODE(type), NULL,
					    "The type has an 'all' model group in its {content type} and thus cannot be derived from "
					    "a non-empty type, since this would produce a 'sequence' model group containing the 'all' "
					    "model group; 'all' model groups are not allowed to appear inside other model groups", NULL, 0);
				}
				else if(WXS_TYPE_PARTICLE(baseType) && WXS_TYPE_PARTICLE_TERM(baseType) && ((WXS_TYPE_PARTICLE_TERM(baseType))->type == XML_SCHEMA_TYPE_ALL)) {
					/*
					 * SPEC cos-all-limited (1)
					 */
					xmlSchemaCustomErr(ACTXT_CAST pctxt, /* @todo error code */ XML_SCHEMAP_COS_ALL_LIMITED, WXS_ITEM_NODE(type), NULL,
					    "A type cannot be derived by extension from a type which has an 'all' model group in its "
					    "{content type}, since this would produce a 'sequence' model group containing the 'all' "
					    "model group; 'all' model groups are not allowed to appear inside other model groups", NULL, 0);
				}
				else if(!dummySequence) {
					xmlSchemaTreeItem * effectiveContent = (xmlSchemaTreeItem *)type->subtypes;
					/*
					 * Create the particle.
					 */
					particle = xmlSchemaAddParticle(pctxt, type->P_Node, 1, 1);
					if(particle == NULL)
						goto exit_failure;
					/*
					 * Create the "sequence" model group.
					 */
					particle->children = (xmlSchemaTreeItem *)xmlSchemaAddModelGroup(pctxt, pctxt->schema, XML_SCHEMA_TYPE_SEQUENCE, type->P_Node);
					if(particle->children == NULL)
						goto exit_failure;
					WXS_TYPE_CONTENTTYPE(type) = (xmlSchemaTypePtr)particle;
					/*
					 * SPEC "the particle of the {content type} of
					 * the ... base ..."
					 * Create a duplicate of the base type's particle
					 * and assign its "term" to it.
					 */
					particle->children->children = (xmlSchemaTreeItem *)xmlSchemaAddParticle(pctxt,
					    type->P_Node, ((xmlSchemaParticlePtr)baseType->subtypes)->minOccurs, ((xmlSchemaParticlePtr)baseType->subtypes)->maxOccurs);
					if(particle->children->children == NULL)
						goto exit_failure;
					particle = (xmlSchemaParticlePtr)
					    particle->children->children;
					particle->children =
					    ((xmlSchemaParticlePtr)baseType->subtypes)->children;
					/*
					 * SPEC "followed by the `effective content`."
					 */
					particle->next = effectiveContent;
					/*
					 * This all will result in:
					 * new-particle
					 *   --> new-sequence(
					 *         new-particle
					 *           --> base-model,
					 *         this-particle
					 *	        --> this-model
					 *	    )
					 */
				}
				else {
					/*
					 * This is the case when there is already an empty
					 * <sequence> with minOccurs==maxOccurs==1.
					 * Just add the base types's content type.
					 * NOTE that, although we miss to add an intermediate
					 * <sequence>, this should produce no difference to
					 * neither the regex compilation of the content model,
					 * nor to the complex type contraints.
					 */
					particle->children->children = (xmlSchemaTreeItem *)baseType->subtypes;
				}
			}
		}
	}
	/*
	 * Now fixup attribute uses:
	 *   - expand attr. group references
	 *     - intersect attribute wildcards
	 *   - inherit attribute uses of the base type
	 *   - inherit or union attr. wildcards if extending
	 *   - apply attr. use prohibitions if restricting
	 */
	res = xmlSchemaFixupTypeAttributeUses(pctxt, type);
	HFAILURE HERROR
	/*
	 * Apply the complex type component constraints; this will not
	 * check attributes, since this is done in
	 * xmlSchemaFixupTypeAttributeUses().
	 */
	    res = xmlSchemaCheckCTComponent(pctxt, type);
	HFAILURE HERROR

#ifdef DEBUG_TYPE
	xmlSchemaDebugFixedType(pctxt, type);
#endif
	if(olderrs != pctxt->nberrors)
		return(pctxt->err);
	else
		return 0;

exit_error:
	type->flags |= XML_SCHEMAS_TYPE_INTERNAL_INVALID;
#ifdef DEBUG_TYPE
	xmlSchemaDebugFixedType(pctxt, type);
#endif
	return(pctxt->err);

exit_failure:
	type->flags |= XML_SCHEMAS_TYPE_INTERNAL_INVALID;
#ifdef DEBUG_TYPE
	xmlSchemaDebugFixedType(pctxt, type);
#endif
	return -1;
}
/**
 * xmlSchemaTypeFixup:
 * @typeDecl:  the schema type definition
 * @ctxt:  the schema parser context
 *
 * Fixes the content model of the type.
 * URGENT TODO: We need an int result!
 */
static int FASTCALL xmlSchemaTypeFixup(xmlSchemaTypePtr type, xmlSchemaAbstractCtxtPtr actxt)
{
	if(type) {
		if(actxt->type != XML_SCHEMA_CTXT_PARSER) {
			AERROR_INT("xmlSchemaTypeFixup", "this function needs a parser context");
			return -1;
		}
		else if(!WXS_IS_TYPE_NOT_FIXED(type))
			return 0;
		else if(type->type == XML_SCHEMA_TYPE_COMPLEX)
			return xmlSchemaFixupComplexType(PCTXT_CAST actxt, type);
		else if(type->type == XML_SCHEMA_TYPE_SIMPLE)
			return xmlSchemaFixupSimpleTypeStageTwo(PCTXT_CAST actxt, type);
	}
	return 0;
}

/**
 * xmlSchemaCheckFacet:
 * @facet:  the facet
 * @typeDecl:  the schema type definition
 * @pctxt:  the schema parser context or NULL
 * @name: the optional name of the type
 *
 * Checks and computes the values of facets.
 *
 * Returns 0 if valid, a positive error code if not valid and
 *         -1 in case of an internal or API error.
 */
int xmlSchemaCheckFacet(xmlSchemaFacetPtr facet, xmlSchemaTypePtr typeDecl, xmlSchemaParserCtxtPtr pctxt, const xmlChar * name ATTRIBUTE_UNUSED)
{
	int ret = 0;
	if((facet == NULL) || (typeDecl == NULL))
		return -1;
	//
	// @todo will the parser context be given if used from the relaxNG module?
	//
	const int ctxtGiven = pctxt ? 1 : 0;
	switch(facet->type) {
		case XML_SCHEMA_FACET_MININCLUSIVE:
		case XML_SCHEMA_FACET_MINEXCLUSIVE:
		case XML_SCHEMA_FACET_MAXINCLUSIVE:
		case XML_SCHEMA_FACET_MAXEXCLUSIVE:
		case XML_SCHEMA_FACET_ENUMERATION: {
		    /*
		     * Okay we need to validate the value at that point.
		     */
		    xmlSchemaTypePtr base;

		    /* 4.3.5.5 Constraints on enumeration Schema Components
		     * Schema Component Constraint: enumeration valid restriction
		     * It is an `error` if any member of {value} is not in the
		     * `value space` of {base type definition}.
		     *
		     * minInclusive, maxInclusive, minExclusive, maxExclusive:
		     * The value `must` be in the
		     * `value space` of the `base type`.
		     */
		    /*
		     * This function is intended to deliver a compiled value
		     * on the facet. In this implementation of XML Schemata the
		     * type holding a facet, won't be a built-in type.
		     * Thus to ensure that other API
		     * calls (relaxng) do work, if the given type is a built-in
		     * type, we will assume that the given built-in type *is
		     * already* the base type.
		     */
		    if(typeDecl->type != XML_SCHEMA_TYPE_BASIC) {
			    base = typeDecl->baseType;
			    if(base == NULL) {
				    PERROR_INT("xmlSchemaCheckFacet", "a type user derived type has no base type");
				    return -1;
			    }
		    }
		    else
			    base = typeDecl;

		    if(!ctxtGiven) {
			    /*
			     * A context is needed if called from RelaxNG.
			     */
			    pctxt = xmlSchemaNewParserCtxt("*");
			    if(pctxt == NULL)
				    return -1;
		    }
		    /*
		     * NOTE: This call does not check the content nodes,
		     * since they are not available:
		     * facet->node is just the node holding the facet
		     * definition, *not* the attribute holding the *value*
		     * of the facet.
		     */
		    ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST pctxt, facet->P_Node, base, facet->value, &(facet->val), 1, 1, 0);
		    if(ret != 0) {
			    if(ret < 0) {
				    /* No error message for RelaxNG. */
				    if(ctxtGiven) {
					    xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_INTERNAL, facet->P_Node, NULL,
						    "Internal error: xmlSchemaCheckFacet, failed to validate the value '%s' of the facet '%s' against the base type",
						    facet->value, xmlSchemaFacetTypeToString(facet->type));
				    }
				    goto internal_error;
			    }
			    ret = XML_SCHEMAP_INVALID_FACET_VALUE;
			    /* No error message for RelaxNG. */
			    if(ctxtGiven) {
				    xmlChar * str = NULL;
				    xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)ret, facet->P_Node, WXS_BASIC_CAST facet,
					    "The value '%s' of the facet does not validate against the base type '%s'",
					    facet->value, xmlSchemaFormatQName(&str, base->targetNamespace, base->name));
				    ZFREE(str);
			    }
			    goto exit;
		    }
		    else if(facet->val == NULL) {
			    if(ctxtGiven) {
				    PERROR_INT("xmlSchemaCheckFacet", "value was not computed");
			    }
			    TODO
		    }
		    break;
	    }
		case XML_SCHEMA_FACET_PATTERN:
		    facet->regexp = xmlRegexpCompile(facet->value);
		    if(facet->regexp == NULL) {
			    ret = XML_SCHEMAP_REGEXP_INVALID;
			    /* No error message for RelaxNG. */
			    if(ctxtGiven) {
				    xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)ret, facet->P_Node, WXS_BASIC_CAST typeDecl,
				    "The value '%s' of the facet 'pattern' is not a valid regular expression",
				    facet->value, 0);
			    }
		    }
		    break;
		case XML_SCHEMA_FACET_TOTALDIGITS:
		case XML_SCHEMA_FACET_FRACTIONDIGITS:
		case XML_SCHEMA_FACET_LENGTH:
		case XML_SCHEMA_FACET_MAXLENGTH:
		case XML_SCHEMA_FACET_MINLENGTH:
		    if(facet->type == XML_SCHEMA_FACET_TOTALDIGITS) {
			    ret = xmlSchemaValidatePredefinedType(xmlSchemaGetBuiltInType(XML_SCHEMAS_PINTEGER), facet->value, &(facet->val));
		    }
		    else {
			    ret = xmlSchemaValidatePredefinedType(xmlSchemaGetBuiltInType(XML_SCHEMAS_NNINTEGER), facet->value, &(facet->val));
		    }
		    if(ret != 0) {
			    if(ret < 0) {
				    /* No error message for RelaxNG. */
				    if(ctxtGiven) {
					    PERROR_INT("xmlSchemaCheckFacet", "validating facet value");
				    }
				    goto internal_error;
			    }
			    ret = XML_SCHEMAP_INVALID_FACET_VALUE;
			    /* No error message for RelaxNG. */
			    if(ctxtGiven) {
				    /* error code */
				    xmlSchemaCustomErr4(ACTXT_CAST pctxt, (xmlParserErrors)ret, facet->P_Node, WXS_BASIC_CAST typeDecl,
						"The value '%s' of the facet '%s' is not a valid '%s'",
						facet->value, xmlSchemaFacetTypeToString(facet->type),
						(facet->type != XML_SCHEMA_FACET_TOTALDIGITS) ? BAD_CAST "nonNegativeInteger" : BAD_CAST "positiveInteger",
						NULL);
			    }
		    }
		    break;

		case XML_SCHEMA_FACET_WHITESPACE: {
		    if(sstreq(facet->value, BAD_CAST "preserve"))
			    facet->whitespace = XML_SCHEMAS_FACET_PRESERVE;
		    else if(sstreq(facet->value, BAD_CAST "replace"))
			    facet->whitespace = XML_SCHEMAS_FACET_REPLACE;
		    else if(sstreq(facet->value, BAD_CAST "collapse"))
			    facet->whitespace = XML_SCHEMAS_FACET_COLLAPSE;
		    else {
			    ret = XML_SCHEMAP_INVALID_FACET_VALUE;
			    // No error message for RelaxNG. 
			    if(ctxtGiven) {
				    // error was previously: XML_SCHEMAP_INVALID_WHITE_SPACE 
				    xmlSchemaCustomErr(ACTXT_CAST pctxt, (xmlParserErrors)ret, facet->P_Node, WXS_BASIC_CAST typeDecl,
					    "The value '%s' of the facet 'whitespace' is not valid", facet->value, 0);
			    }
		    }
	    }
		default:
		    break;
	}
exit:
	if(!ctxtGiven)
		xmlSchemaFreeParserCtxt(pctxt);
	return ret;
internal_error:
	if(!ctxtGiven)
		xmlSchemaFreeParserCtxt(pctxt);
	return -1;
}

/**
 * xmlSchemaCheckFacetValues:
 * @typeDecl:  the schema type definition
 * @ctxt:  the schema parser context
 *
 * Checks the default values types, especially for facets
 */
static int xmlSchemaCheckFacetValues(xmlSchemaTypePtr typeDecl, xmlSchemaParserCtxtPtr pctxt)
{
	int res, olderrs = pctxt->nberrors;
	const xmlChar * name = typeDecl->name;
	// NOTE: It is intended to use the facets list, instead of facetSet.
	if(typeDecl->facets) {
		xmlSchemaFacetPtr facet = typeDecl->facets;
		/*
		 * Temporarily assign the "schema" to the validation context
		 * of the parser context. This is needed for NOTATION validation.
		 */
		if(pctxt->vctxt == NULL) {
			if(xmlSchemaCreateVCtxtOnPCtxt(pctxt) == -1)
				return -1;
		}
		pctxt->vctxt->schema = pctxt->schema;
		while(facet) {
			res = xmlSchemaCheckFacet(facet, typeDecl, pctxt, name);
			HFAILURE
			    facet = facet->next;
		}
		pctxt->vctxt->schema = NULL;
	}
	if(olderrs != pctxt->nberrors)
		return(pctxt->err);
	return 0;
exit_failure:
	return -1;
}

/**
 * xmlSchemaGetCircModelGrDefRef:
 * @ctxtMGroup: the searched model group
 * @selfMGroup: the second searched model group
 * @particle: the first particle
 *
 * This one is intended to be used by
 * xmlSchemaCheckGroupDefCircular only.
 *
 * Returns the particle with the circular model group definition reference,
 * otherwise NULL.
 */
static xmlSchemaTreeItem * xmlSchemaGetCircModelGrDefRef(xmlSchemaModelGroupDefPtr groupDef, xmlSchemaTreeItem * particle)
{
	xmlSchemaTreeItem * circ = NULL;
	xmlSchemaModelGroupDefPtr gdef;
	for(; particle; particle = particle->next) {
		xmlSchemaTreeItem * term = particle->children;
		if(term) {
			switch(term->type) {
				case XML_SCHEMA_TYPE_GROUP:
					gdef = (xmlSchemaModelGroupDefPtr)term;
					if(gdef == groupDef)
						return (particle);
					// 
					// Mark this model group definition to avoid infinite
					// recursion on circular references not yet examined.
					// 
					if(gdef->flags & XML_SCHEMA_MODEL_GROUP_DEF_MARKED)
						continue;
					if(gdef->children) {
						gdef->flags |= XML_SCHEMA_MODEL_GROUP_DEF_MARKED;
						circ = xmlSchemaGetCircModelGrDefRef(groupDef,
						gdef->children->children);
						gdef->flags ^= XML_SCHEMA_MODEL_GROUP_DEF_MARKED;
						if(circ)
							return (circ);
					}
					break;
				case XML_SCHEMA_TYPE_SEQUENCE:
				case XML_SCHEMA_TYPE_CHOICE:
				case XML_SCHEMA_TYPE_ALL:
					circ = xmlSchemaGetCircModelGrDefRef(groupDef, term->children);
					if(circ)
						return (circ);
					break;
				default:
					break;
			}
		}
	}
	return 0;
}

/**
 * xmlSchemaCheckGroupDefCircular:
 * @item:  the model group definition
 * @ctxt:  the parser context
 * @name:  the name
 *
 * Checks for circular references to model group definitions.
 */
static void xmlSchemaCheckGroupDefCircular(xmlSchemaModelGroupDefPtr item, xmlSchemaParserCtxtPtr ctxt)
{
	/*
	 * Schema Component Constraint: Model Group Correct
	 * 2 Circular groups are disallowed. That is, within the {particles}
	 * of a group there must not be at any depth a particle whose {term}
	 * is the group itself.
	 */
	if((item == NULL) || (item->type != XML_SCHEMA_TYPE_GROUP) || (item->children == NULL))
		return;
	{
		xmlSchemaTreeItem * circ = xmlSchemaGetCircModelGrDefRef(item, item->children->children);
		if(circ) {
			xmlChar * str = NULL;
			/*
			 * @todo The error report is not adequate: this constraint
			 * is defined for model groups but not definitions, but since
			 * there cannot be any circular model groups without a model group
			 * definition (if not using a construction API), we check those
			 * defintions only.
			 */
			xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_MG_PROPS_CORRECT_2, NULL, WXS_ITEM_NODE(circ),
			    "Circular reference to the model group definition '%s' defined", xmlSchemaFormatQName(&str, item->targetNamespace, item->name));
			ZFREE(str);
			/*
			 * NOTE: We will cut the reference to avoid further
			 * confusion of the processor. This is a fatal error.
			 */
			circ->children = NULL;
		}
	}
}

/**
 * xmlSchemaModelGroupToModelGroupDefFixup:
 * @ctxt:  the parser context
 * @mg:  the model group
 *
 * Assigns the model group of model group definitions to the "term"
 * of the referencing particle.
 * In xmlSchemaResolveModelGroupParticleReferences the model group
 * definitions were assigned to the "term", since needed for the
 * circularity check.
 *
 * Schema Component Constraint:
 *     All Group Limited (cos-all-limited) (1.2)
 */
static void xmlSchemaModelGroupToModelGroupDefFixup(xmlSchemaParserCtxtPtr ctxt ATTRIBUTE_UNUSED, xmlSchemaModelGroupPtr mg)
{
	xmlSchemaParticlePtr particle = WXS_MODELGROUP_PARTICLE(mg);
	while(particle) {
		if((WXS_PARTICLE_TERM(particle) == NULL) || ((WXS_PARTICLE_TERM(particle))->type != XML_SCHEMA_TYPE_GROUP)) {
			particle = WXS_PTC_CAST particle->next;
			continue;
		}
		if(WXS_MODELGROUPDEF_MODEL(WXS_PARTICLE_TERM(particle)) == NULL) {
			/*
			 * @todo Remove the particle.
			 */
			WXS_PARTICLE_TERM(particle) = NULL;
			particle = WXS_PTC_CAST particle->next;
			continue;
		}
		/*
		 * Assign the model group to the {term} of the particle.
		 */
		WXS_PARTICLE_TERM(particle) = WXS_TREE_CAST WXS_MODELGROUPDEF_MODEL(WXS_PARTICLE_TERM(particle));
		particle = WXS_PTC_CAST particle->next;
	}
}

/**
 * xmlSchemaCheckAttrGroupCircularRecur:
 * @ctxtGr: the searched attribute group
 * @attr: the current attribute list to be processed
 *
 * This one is intended to be used by
 * xmlSchemaCheckAttrGroupCircular only.
 *
 * Returns the circular attribute grou reference, otherwise NULL.
 */
static xmlSchemaQNameRefPtr FASTCALL xmlSchemaCheckAttrGroupCircularRecur(xmlSchemaAttributeGroupPtr ctxtGr, xmlSchemaItemListPtr list)
{
	//
	// We will search for an attribute group reference which references the context attribute group.
	//
	for(int i = 0; i < list->nbItems; i++) {
		xmlSchemaQNameRefPtr ref = (xmlSchemaQNameRefPtr)list->items[i];
		if((ref->type == XML_SCHEMA_EXTRA_QNAMEREF) && (ref->itemType == XML_SCHEMA_TYPE_ATTRIBUTEGROUP) && ref->item) {
			xmlSchemaAttributeGroupPtr gr = WXS_ATTR_GROUP_CAST ref->item;
			if(gr == ctxtGr)
				return(ref);
			if(gr->flags & XML_SCHEMAS_ATTRGROUP_MARKED)
				continue;
			/*
			 * Mark as visited to avoid infinite recursion on
			 * circular references not yet examined.
			 */
			if((gr->attrUses) && (gr->flags & XML_SCHEMAS_ATTRGROUP_HAS_REFS)) {
				gr->flags |= XML_SCHEMAS_ATTRGROUP_MARKED;
				xmlSchemaQNameRefPtr circ = xmlSchemaCheckAttrGroupCircularRecur(ctxtGr, (xmlSchemaItemListPtr)gr->attrUses);
				gr->flags ^= XML_SCHEMAS_ATTRGROUP_MARKED;
				if(circ)
					return (circ);
			}
		}
	}
	return 0;
}

/**
 * xmlSchemaCheckAttrGroupCircular:
 * attrGr:  the attribute group definition
 * @ctxt:  the parser context
 * @name:  the name
 *
 * Checks for circular references of attribute groups.
 */
static int xmlSchemaCheckAttrGroupCircular(xmlSchemaAttributeGroupPtr attrGr, xmlSchemaParserCtxtPtr ctxt)
{
	/*
	 * Schema Representation Constraint:
	 * Attribute Group Definition Representation OK
	 * 3 Circular group reference is disallowed outside <redefine>.
	 * That is, unless this element information item's parent is
	 * <redefine>, then among the [children], if any, there must
	 * not be an <attributeGroup> with ref [attribute] which resolves
	 * to the component corresponding to this <attributeGroup>. Indirect
	 * circularity is also ruled out. That is, when QName resolution
	 * (Schema Document) ($3.15.3) is applied to a `QName` arising from
	 * any <attributeGroup>s with a ref [attribute] among the [children],
	 * it must not be the case that a `QName` is encountered at any depth
	 * which resolves to the component corresponding to this <attributeGroup>.
	 */
	if(attrGr->attrUses == NULL)
		return 0;
	else if((attrGr->flags & XML_SCHEMAS_ATTRGROUP_HAS_REFS) == 0)
		return 0;
	else {
		xmlSchemaQNameRefPtr circ = xmlSchemaCheckAttrGroupCircularRecur(attrGr, (xmlSchemaItemListPtr)attrGr->attrUses);
		if(circ) {
			xmlChar * str = NULL;
			/*
			 * @todo Report the referenced attr group as QName.
			 */
			xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_SRC_ATTRIBUTE_GROUP_3, NULL, WXS_ITEM_NODE(WXS_BASIC_CAST circ),
			    "Circular reference to the attribute group '%s' defined", xmlSchemaGetComponentQName(&str, attrGr));
			ZFREE(str);
			/*
			 * NOTE: We will cut the reference to avoid further
			 * confusion of the processor.
			 * BADSPEC TODO: The spec should define how to process in this case.
			 */
			circ->item = NULL;
			return(ctxt->err);
		}
	}
	return 0;
}

static int xmlSchemaAttributeGroupExpandRefs(xmlSchemaParserCtxtPtr pctxt,
    xmlSchemaAttributeGroupPtr attrGr);

/**
 * xmlSchemaExpandAttributeGroupRefs:
 * @pctxt: the parser context
 * @node: the node of the component holding the attribute uses
 * @completeWild: the intersected wildcard to be returned
 * @list: the attribute uses
 *
 * Substitutes contained attribute group references
 * for their attribute uses. Wilcards are intersected.
 * Attribute use prohibitions are removed from the list
 * and returned via the @prohibs list.
 * Pointlessness of attr. prohibs, if a matching attr. decl
 * is existent a well, are checked.
 */
static int xmlSchemaExpandAttributeGroupRefs(xmlSchemaParserCtxtPtr pctxt, xmlSchemaBasicItem * item,
    xmlSchemaWildcardPtr * completeWild, xmlSchemaItemListPtr list, xmlSchemaItemListPtr prohibs)
{
	xmlSchemaAttributeGroupPtr gr;
	xmlSchemaAttributeUsePtr use;
	xmlSchemaItemListPtr sublist;
	int i, j;
	int created = (*completeWild == NULL) ? 0 : 1;
	if(prohibs)
		prohibs->nbItems = 0;
	for(i = 0; i < list->nbItems; i++) {
		use = (xmlSchemaAttributeUsePtr)list->items[i];
		if(use->type == XML_SCHEMA_EXTRA_ATTR_USE_PROHIB) {
			if(prohibs == NULL) {
				PERROR_INT("xmlSchemaExpandAttributeGroupRefs", "unexpected attr prohibition found");
				return -1;
			}
			/*
			 * Remove from attribute uses.
			 */
			if(xmlSchemaItemListRemove(list, i) == -1)
				return -1;
			i--;
			/*
			 * Note that duplicate prohibitions were already
			 * handled at parsing time.
			 */
			/*
			 * Add to list of prohibitions.
			 */
			xmlSchemaItemListAddSize(prohibs, 2, use);
			continue;
		}
		if((use->type == XML_SCHEMA_EXTRA_QNAMEREF) && ((WXS_QNAME_CAST use)->itemType == XML_SCHEMA_TYPE_ATTRIBUTEGROUP)) {
			if((WXS_QNAME_CAST use)->item == NULL)
				return -1;
			gr = WXS_ATTR_GROUP_CAST(WXS_QNAME_CAST use)->item;
			/*
			 * Expand the referenced attr. group.
			 * @todo remove this, this is done in a previous step, so
			 * already done here.
			 */
			if((gr->flags & XML_SCHEMAS_ATTRGROUP_WILDCARD_BUILDED) == 0) {
				if(xmlSchemaAttributeGroupExpandRefs(pctxt, gr) == -1)
					return -1;
			}
			/*
			 * Build the 'complete' wildcard; i.e. intersect multiple
			 * wildcards.
			 */
			if(gr->attributeWildcard) {
				if(*completeWild == NULL) {
					*completeWild = gr->attributeWildcard;
				}
				else {
					if(!created) {
						/*
						 * Copy the first encountered wildcard as context,
						 * except for the annotation.
						 *
						 * Although the complete wildcard might not correspond
						 * to any node in the schema, we will anchor it on
						 * the node of the owner component.
						 */
						xmlSchemaWildcard * tmpWild =  xmlSchemaAddWildcard(pctxt, pctxt->schema, XML_SCHEMA_TYPE_ANY_ATTRIBUTE, WXS_ITEM_NODE(item));
						if(tmpWild == NULL)
							return -1;
						if(xmlSchemaCloneWildcardNsConstraints(pctxt, tmpWild, *completeWild) == -1)
							return -1;
						tmpWild->processContents = (*completeWild)->processContents;
						*completeWild = tmpWild;
						created = 1;
					}
					if(xmlSchemaIntersectWildcards(pctxt, *completeWild, gr->attributeWildcard) == -1)
						return -1;
				}
			}
			/*
			 * Just remove the reference if the referenced group does not
			 * contain any attribute uses.
			 */
			sublist = ((xmlSchemaItemListPtr)gr->attrUses);
			if((sublist == NULL) || sublist->nbItems == 0) {
				if(xmlSchemaItemListRemove(list, i) == -1)
					return -1;
				i--;
				continue;
			}
			/*
			 * Add the attribute uses.
			 */
			list->items[i] = sublist->items[0];
			if(sublist->nbItems != 1) {
				for(j = 1; j < sublist->nbItems; j++) {
					i++;
					if(xmlSchemaItemListInsert(list, sublist->items[j], i) == -1)
						return -1;
				}
			}
		}
	}
	/*
	 * Handle pointless prohibitions of declared attributes.
	 */
	if(prohibs && prohibs->nbItems && list->nbItems) {
		for(i = prohibs->nbItems -1; i >= 0; i--) {
			xmlSchemaAttributeUseProhib * prohib = (xmlSchemaAttributeUseProhib *)prohibs->items[i];
			for(j = 0; j < list->nbItems; j++) {
				use = (xmlSchemaAttributeUsePtr)list->items[j];
				if((prohib->name == WXS_ATTRUSE_DECL_NAME(use)) && (prohib->targetNamespace == WXS_ATTRUSE_DECL_TNS(use))) {
					xmlChar * str = NULL;
					xmlSchemaCustomWarning(ACTXT_CAST pctxt, XML_SCHEMAP_WARN_ATTR_POINTLESS_PROH, prohib->P_Node, NULL,
					    "Skipping pointless attribute use prohibition '%s', since a corresponding attribute use exists already in the type definition",
					    xmlSchemaFormatQName(&str, prohib->targetNamespace, prohib->name), NULL, 0);
					ZFREE(str);
					// Remove the prohibition.
					if(xmlSchemaItemListRemove(prohibs, i) == -1)
						return -1;
					break;
				}
			}
		}
	}
	return 0;
}

/**
 * xmlSchemaAttributeGroupExpandRefs:
 * @pctxt:  the parser context
 * @attrGr:  the attribute group definition
 *
 * Computation of:
 * {attribute uses} property
 * {attribute wildcard} property
 *
 * Substitutes contained attribute group references
 * for their attribute uses. Wilcards are intersected.
 */
static int xmlSchemaAttributeGroupExpandRefs(xmlSchemaParserCtxtPtr pctxt, xmlSchemaAttributeGroupPtr attrGr)
{
	if((attrGr->attrUses == NULL) || (attrGr->flags & XML_SCHEMAS_ATTRGROUP_WILDCARD_BUILDED))
		return 0;
	attrGr->flags |= XML_SCHEMAS_ATTRGROUP_WILDCARD_BUILDED;
	if(xmlSchemaExpandAttributeGroupRefs(pctxt, WXS_BASIC_CAST attrGr, &(attrGr->attributeWildcard), (xmlSchemaItemListPtr)attrGr->attrUses, NULL) == -1)
		return -1;
	return 0;
}

/**
 * xmlSchemaAttributeGroupExpandRefs:
 * @pctxt:  the parser context
 * @attrGr:  the attribute group definition
 *
 * Substitutes contained attribute group references
 * for their attribute uses. Wilcards are intersected.
 *
 * Schema Component Constraint:
 *    Attribute Group Definition Properties Correct (ag-props-correct)
 */
static int xmlSchemaCheckAGPropsCorrect(xmlSchemaParserCtxtPtr pctxt, xmlSchemaAttributeGroupPtr attrGr)
{
	/*
	 * SPEC ag-props-correct
	 * (1) "The values of the properties of an attribute group definition
	 * must be as described in the property tableau in The Attribute
	 * Group Definition Schema Component ($3.6.1), modulo the impact of
	 * Missing Sub-components ($5.3);"
	 */
	if(attrGr->attrUses && (WXS_LIST_CAST attrGr->attrUses)->nbItems > 1) {
		xmlSchemaItemListPtr uses = WXS_LIST_CAST attrGr->attrUses;
		xmlSchemaAttributeUsePtr use, tmp;
		int i, j, hasId = 0;
		for(i = uses->nbItems -1; i >= 0; i--) {
			use = (xmlSchemaAttributeUsePtr)uses->items[i];
			/*
			 * SPEC ag-props-correct
			 * (2) "Two distinct members of the {attribute uses} must not have
			 * {attribute declaration}s both of whose {name}s match and whose
			 * {target namespace}s are identical."
			 */
			if(i > 0) {
				for(j = i -1; j >= 0; j--) {
					tmp = (xmlSchemaAttributeUsePtr)uses->items[j];
					if((WXS_ATTRUSE_DECL_NAME(use) == WXS_ATTRUSE_DECL_NAME(tmp)) && (WXS_ATTRUSE_DECL_TNS(use) == WXS_ATTRUSE_DECL_TNS(tmp))) {
						xmlChar * str = NULL;
						xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_AG_PROPS_CORRECT, attrGr->P_Node, WXS_BASIC_CAST attrGr,
							"Duplicate %s", xmlSchemaGetComponentDesignation(&str, use), NULL);
						ZFREE(str);
						/*
						 * Remove the duplicate.
						 */
						if(xmlSchemaItemListRemove(uses, i) == -1)
							return -1;
						goto next_use;
					}
				}
			}
			/*
			 * SPEC ag-props-correct
			 * (3) "Two distinct members of the {attribute uses} must not have
			 * {attribute declaration}s both of whose {type definition}s are or
			 * are derived from ID."
			 * @todo Does 'derived' include member-types of unions?
			 */
			if(WXS_ATTRUSE_TYPEDEF(use)) {
				if(xmlSchemaIsDerivedFromBuiltInType(WXS_ATTRUSE_TYPEDEF(use), XML_SCHEMAS_ID)) {
					if(hasId) {
						xmlChar * str = NULL;
						xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_AG_PROPS_CORRECT, attrGr->P_Node, WXS_BASIC_CAST attrGr,
						    "There must not exist more than one attribute declaration of type 'xs:ID' (or derived from 'xs:ID'). The %s violates this constraint",
						    xmlSchemaGetComponentDesignation(&str, use), NULL);
						ZFREE(str);
						if(xmlSchemaItemListRemove(uses, i) == -1)
							return -1;
					}
					hasId = 1;
				}
			}
next_use: {}
		}
	}
	return 0;
}
/**
 * xmlSchemaResolveAttrGroupReferences:
 * @attrgrpDecl:  the schema attribute definition
 * @ctxt:  the schema parser context
 * @name:  the attribute name
 *
 * Resolves references to attribute group definitions.
 */
static int xmlSchemaResolveAttrGroupReferences(xmlSchemaQNameRefPtr ref, xmlSchemaParserCtxtPtr ctxt)
{
	if(!ref->item) {
		xmlSchemaAttributeGroup * group = xmlSchemaGetAttributeGroup(ctxt->schema, ref->name, ref->targetNamespace);
		if(group == NULL) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, NULL, ref->P_Node, "ref", ref->name, ref->targetNamespace, ref->itemType, 0);
			return ctxt->err;
		}
		ref->item = WXS_BASIC_CAST group;
	}
	return 0;
}
/**
 * xmlSchemaCheckAttrPropsCorrect:
 * @item:  an schema attribute declaration/use
 * @ctxt:  a schema parser context
 * @name:  the name of the attribute
 *
 *
 * Schema Component Constraint:
 *    Attribute Declaration Properties Correct (a-props-correct)
 *
 * Validates the value constraints of an attribute declaration/use.
 * NOTE that this needs the simle type definitions to be already
 *   builded and checked.
 */
static int xmlSchemaCheckAttrPropsCorrect(xmlSchemaParserCtxtPtr pctxt, xmlSchemaAttributePtr attr)
{
	/*
	 * SPEC a-props-correct (1)
	 * "The values of the properties of an attribute declaration must
	 * be as described in the property tableau in The Attribute
	 * Declaration Schema Component ($3.2.1), modulo the impact of
	 * Missing Sub-components ($5.3)."
	 */
	if(WXS_ATTR_TYPEDEF(attr)) {
		if(attr->defValue) {
			int ret;
			/*
			 * SPEC a-props-correct (3)
			 * "If the {type definition} is or is derived from ID then there
			 * must not be a {value constraint}."
			 */
			if(xmlSchemaIsDerivedFromBuiltInType(WXS_ATTR_TYPEDEF(attr), XML_SCHEMAS_ID)) {
				xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_A_PROPS_CORRECT_3, NULL, WXS_BASIC_CAST attr,
					"Value constraints are not allowed if the type definition is or is derived from xs:ID", NULL, 0);
				return(pctxt->err);
			}
			/*
			 * SPEC a-props-correct (2)
			 * "if there is a {value constraint}, the canonical lexical
			 * representation of its value must be `valid` with respect
			 * to the {type definition} as defined in String Valid ($3.14.4)."
			 * @todo Don't care about the *canonical* stuff here, this requirement
			 * will be removed in WXS 1.1 anyway.
			 */
			ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST pctxt, attr->P_Node, WXS_ATTR_TYPEDEF(attr), attr->defValue, &(attr->defVal), 1, 1, 0);
			if(ret != 0) {
				if(ret < 0) {
					PERROR_INT("xmlSchemaCheckAttrPropsCorrect", "calling xmlSchemaVCheckCVCSimpleType()");
					return -1;
				}
				xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_A_PROPS_CORRECT_2,
					NULL, WXS_BASIC_CAST attr, "The value of the value constraint is not valid", 0, 0);
				return(pctxt->err);
			}
		}
	}
	return 0;
}

static xmlSchemaElementPtr xmlSchemaCheckSubstGroupCircular(xmlSchemaElementPtr elemDecl, xmlSchemaElementPtr ancestor)
{
	xmlSchemaElementPtr ret;
	if(WXS_SUBST_HEAD(ancestor) == NULL)
		return 0;
	if(WXS_SUBST_HEAD(ancestor) == elemDecl)
		return (ancestor);
	if(WXS_SUBST_HEAD(ancestor)->flags & XML_SCHEMAS_ELEM_CIRCULAR)
		return 0;
	WXS_SUBST_HEAD(ancestor)->flags |= XML_SCHEMAS_ELEM_CIRCULAR;
	ret = xmlSchemaCheckSubstGroupCircular(elemDecl, WXS_SUBST_HEAD(ancestor));
	WXS_SUBST_HEAD(ancestor)->flags ^= XML_SCHEMAS_ELEM_CIRCULAR;
	return ret;
}

/**
 * xmlSchemaCheckElemPropsCorrect:
 * @ctxt:  a schema parser context
 * @decl: the element declaration
 * @name:  the name of the attribute
 *
 * Schema Component Constraint:
 * Element Declaration Properties Correct (e-props-correct)
 *
 * STATUS:
 *   missing: (6)
 */
static int xmlSchemaCheckElemPropsCorrect(xmlSchemaParserCtxtPtr pctxt, xmlSchemaElementPtr elemDecl)
{
	int ret = 0;
	xmlSchemaTypePtr typeDef = WXS_ELEM_TYPEDEF(elemDecl);
	/*
	 * SPEC (1) "The values of the properties of an element declaration
	 * must be as described in the property tableau in The Element
	 * Declaration Schema Component ($3.3.1), modulo the impact of Missing
	 * Sub-components ($5.3)."
	 */
	if(WXS_SUBST_HEAD(elemDecl)) {
		xmlSchemaElementPtr head = WXS_SUBST_HEAD(elemDecl), circ;
		xmlSchemaCheckElementDeclComponent(head, pctxt);
		/*
		 * SPEC (3) "If there is a non-`absent` {substitution group
		 * affiliation}, then {scope} must be global."
		 */
		if((elemDecl->flags & XML_SCHEMAS_ELEM_GLOBAL) == 0) {
			xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_E_PROPS_CORRECT_3, WXS_BASIC_CAST elemDecl, NULL,
			    "Only global element declarations can have a substitution group affiliation", 0);
			ret = XML_SCHEMAP_E_PROPS_CORRECT_3;
		}
		/*
		 * @todo SPEC (6) "Circular substitution groups are disallowed.
		 * That is, it must not be possible to return to an element declaration
		 * by repeatedly following the {substitution group affiliation}
		 * property."
		 */
		if(head == elemDecl)
			circ = head;
		else if(WXS_SUBST_HEAD(head))
			circ = xmlSchemaCheckSubstGroupCircular(head, head);
		else
			circ = NULL;
		if(circ) {
			xmlChar * strA = NULL, * strB = NULL;
			xmlSchemaPCustomErrExt(pctxt, XML_SCHEMAP_E_PROPS_CORRECT_6, WXS_BASIC_CAST circ, NULL,
			    "The element declaration '%s' defines a circular substitution group to element declaration '%s'",
			    xmlSchemaGetComponentQName(&strA, circ), xmlSchemaGetComponentQName(&strB, head), NULL);
			ZFREE(strA);
			ZFREE(strB);
			ret = XML_SCHEMAP_E_PROPS_CORRECT_6;
		}
		/*
		 * SPEC (4) "If there is a {substitution group affiliation},
		 * the {type definition}
		 * of the element declaration must be validly derived from the {type
		 * definition} of the {substitution group affiliation}, given the value
		 * of the {substitution group exclusions} of the {substitution group
		 * affiliation}, as defined in Type Derivation OK (Complex) ($3.4.6)
		 * (if the {type definition} is complex) or as defined in
		 * Type Derivation OK (Simple) ($3.14.6) (if the {type definition} is
		 * simple)."
		 *
		 * NOTE: {substitution group exclusions} means the values of the
		 * attribute "final".
		 */

		if(typeDef != WXS_ELEM_TYPEDEF(WXS_SUBST_HEAD(elemDecl))) {
			int set = 0;
			if(head->flags & XML_SCHEMAS_ELEM_FINAL_EXTENSION)
				set |= SUBSET_EXTENSION;
			if(head->flags & XML_SCHEMAS_ELEM_FINAL_RESTRICTION)
				set |= SUBSET_RESTRICTION;
			if(xmlSchemaCheckCOSDerivedOK(ACTXT_CAST pctxt, typeDef, WXS_ELEM_TYPEDEF(head), set) != 0) {
				xmlChar * strA = NULL, * strB = NULL, * strC = NULL;
				ret = XML_SCHEMAP_E_PROPS_CORRECT_4;
				xmlSchemaPCustomErrExt(pctxt, XML_SCHEMAP_E_PROPS_CORRECT_4, WXS_BASIC_CAST elemDecl, NULL,
				    "The type definition '%s' was either rejected by the substitution group affiliation '%s', or not validly derived from its type definition '%s'",
				    xmlSchemaGetComponentQName(&strA, typeDef), xmlSchemaGetComponentQName(&strB, head), xmlSchemaGetComponentQName(&strC, WXS_ELEM_TYPEDEF(head)));
				ZFREE(strA);
				ZFREE(strB);
				ZFREE(strC);
			}
		}
	}
	/*
	 * SPEC (5) "If the {type definition} or {type definition}'s
	 * {content type}
	 * is or is derived from ID then there must not be a {value constraint}.
	 * Note: The use of ID as a type definition for elements goes beyond
	 * XML 1.0, and should be avoided if backwards compatibility is desired"
	 */
	if(elemDecl->value && ((WXS_IS_SIMPLE(typeDef) && xmlSchemaIsDerivedFromBuiltInType(typeDef, XML_SCHEMAS_ID)) || (WXS_IS_COMPLEX(typeDef) &&
		WXS_HAS_SIMPLE_CONTENT(typeDef) && xmlSchemaIsDerivedFromBuiltInType(typeDef->contentTypeDef, XML_SCHEMAS_ID)))) {
		ret = XML_SCHEMAP_E_PROPS_CORRECT_5;
		xmlSchemaPCustomErr(pctxt, XML_SCHEMAP_E_PROPS_CORRECT_5, WXS_BASIC_CAST elemDecl, NULL,
		    "The type definition (or type definition's content type) is or is derived from ID; value constraints are not allowed in conjunction with such a type definition", 0);
	}
	else if(elemDecl->value) {
		int vcret;
		xmlNode * p_node = NULL;
		/*
		 * SPEC (2) "If there is a {value constraint}, the canonical lexical
		 * representation of its value must be `valid` with respect to the
		 * {type definition} as defined in Element Default Valid (Immediate)
		 * ($3.3.6)."
		 */
		if(typeDef == NULL) {
			xmlSchemaPErr(pctxt, elemDecl->P_Node, XML_SCHEMAP_INTERNAL,
			    "Internal error: xmlSchemaCheckElemPropsCorrect, type is missing... skipping validation of the value constraint", 0, 0);
			return -1;
		}
		if(elemDecl->P_Node) {
			if(elemDecl->flags & XML_SCHEMAS_ELEM_FIXED)
				p_node = (xmlNode *)xmlHasProp(elemDecl->P_Node, BAD_CAST "fixed");
			else
				p_node = (xmlNode *)xmlHasProp(elemDecl->P_Node, BAD_CAST "default");
		}
		vcret = xmlSchemaParseCheckCOSValidDefault(pctxt, p_node, typeDef, elemDecl->value, &(elemDecl->defVal));
		if(vcret != 0) {
			if(vcret < 0) {
				PERROR_INT("xmlSchemaElemCheckValConstr", "failed to validate the value constraint of an element declaration");
				return -1;
			}
			return (vcret);
		}
	}

	return ret;
}

/**
 * xmlSchemaCheckElemSubstGroup:
 * @ctxt:  a schema parser context
 * @decl: the element declaration
 * @name:  the name of the attribute
 *
 * Schema Component Constraint:
 * Substitution Group (cos-equiv-class)
 *
 * In Libxml2 the subst. groups will be precomputed, in terms of that
 * a list will be built for each subst. group head, holding all direct
 * referents to this head.
 * NOTE that this function needs:
 *   1. circular subst. groups to be checked beforehand
 *   2. the declaration's type to be derived from the head's type
 *
 * STATUS:
 *
 */
static void xmlSchemaCheckElemSubstGroup(xmlSchemaParserCtxtPtr ctxt,
    xmlSchemaElementPtr elemDecl)
{
	if((WXS_SUBST_HEAD(elemDecl) == NULL) ||
	    /* SPEC (1) "Its {abstract} is false." */
	    (elemDecl->flags & XML_SCHEMAS_ELEM_ABSTRACT))
		return;
	{
		xmlSchemaElementPtr head;
		xmlSchemaTypePtr headType, type;
		int set, methSet;
		/*
		 * SPEC (2) "It is validly substitutable for HEAD subject to HEAD's
		 * {disallowed substitutions} as the blocking constraint, as defined in
		 * Substitution Group OK (Transitive) ($3.3.6)."
		 */
		for(head = WXS_SUBST_HEAD(elemDecl); head; head = WXS_SUBST_HEAD(head)) {
			set = 0;
			methSet = 0;
			/*
			 * The blocking constraints.
			 */
			if(head->flags & XML_SCHEMAS_ELEM_BLOCK_SUBSTITUTION)
				continue;
			headType = head->subtypes;
			type = elemDecl->subtypes;
			if(headType == type)
				goto add_member;
			if(head->flags & XML_SCHEMAS_ELEM_BLOCK_RESTRICTION)
				set |= XML_SCHEMAS_TYPE_BLOCK_RESTRICTION;
			if(head->flags & XML_SCHEMAS_ELEM_BLOCK_EXTENSION)
				set |= XML_SCHEMAS_TYPE_BLOCK_EXTENSION;
			/*
			 * SPEC: Substitution Group OK (Transitive) (2.3)
			 * "The set of all {derivation method}s involved in the
			 * derivation of D's {type definition} from C's {type definition}
			 * does not intersect with the union of the blocking constraint,
			 * C's {prohibited substitutions} (if C is complex, otherwise the
			 * empty set) and the {prohibited substitutions} (respectively the
			 * empty set) of any intermediate {type definition}s in the
			 * derivation of D's {type definition} from C's {type definition}."
			 */
			/*
			 * OPTIMIZE TODO: Optimize this a bit, since, if traversing the
			 * subst.head axis, the methSet does not need to be computed for
			 * the full depth over and over.
			 */
			/*
			 * The set of all {derivation method}s involved in the derivation
			 */
			while(type && (type != headType)) {
				if((WXS_IS_EXTENSION(type)) && ((methSet & XML_SCHEMAS_TYPE_BLOCK_RESTRICTION) == 0))
					methSet |= XML_SCHEMAS_TYPE_BLOCK_EXTENSION;
				if(WXS_IS_RESTRICTION(type) && ((methSet & XML_SCHEMAS_TYPE_BLOCK_RESTRICTION) == 0))
					methSet |= XML_SCHEMAS_TYPE_BLOCK_RESTRICTION;
				type = type->baseType;
			}
			/*
			 * The {prohibited substitutions} of all intermediate types +
			 * the head's type.
			 */
			type = elemDecl->subtypes->baseType;
			while(type) {
				if(WXS_IS_COMPLEX(type)) {
					if((type->flags & XML_SCHEMAS_TYPE_BLOCK_EXTENSION) && ((set & XML_SCHEMAS_TYPE_BLOCK_EXTENSION) == 0))
						set |= XML_SCHEMAS_TYPE_BLOCK_EXTENSION;
					if((type->flags & XML_SCHEMAS_TYPE_BLOCK_RESTRICTION) && ((set & XML_SCHEMAS_TYPE_BLOCK_RESTRICTION) == 0))
						set |= XML_SCHEMAS_TYPE_BLOCK_RESTRICTION;
				}
				else
					break;
				if(type == headType)
					break;
				type = type->baseType;
			}
			if((set != 0) && (((set & XML_SCHEMAS_TYPE_BLOCK_EXTENSION) &&
					    (methSet & XML_SCHEMAS_TYPE_BLOCK_EXTENSION)) ||
				    ((set & XML_SCHEMAS_TYPE_BLOCK_RESTRICTION) && (methSet & XML_SCHEMAS_TYPE_BLOCK_RESTRICTION)))) {
				continue;
			}
add_member:
			xmlSchemaAddElementSubstitutionMember(ctxt, head, elemDecl);
			if((head->flags & XML_SCHEMAS_ELEM_SUBST_GROUP_HEAD) == 0)
				head->flags |= XML_SCHEMAS_ELEM_SUBST_GROUP_HEAD;
		}
	}
}

#ifdef WXS_ELEM_DECL_CONS_ENABLED /* enable when finished */
/**
 * xmlSchemaCheckElementDeclComponent
 * @pctxt: the schema parser context
 * @ctxtComponent: the context component (an element declaration)
 * @ctxtParticle: the first particle of the context component
 * @searchParticle: the element declaration particle to be analysed
 *
 * Schema Component Constraint: Element Declarations Consistent
 */
static int xmlSchemaCheckElementDeclConsistent(xmlSchemaParserCtxtPtr pctxt,
    xmlSchemaBasicItemPtr ctxtComponent,
    xmlSchemaParticlePtr ctxtParticle,
    xmlSchemaParticlePtr searchParticle,
    xmlSchemaParticlePtr curParticle,
    int search)
{
	return 0;

	int ret = 0;
	xmlSchemaParticlePtr cur = curParticle;
	if(curParticle == NULL) {
		return 0;
	}
	if(WXS_PARTICLE_TERM(curParticle) == NULL) {
		/*
		 * Just return in this case. A missing "term" of the particle
		 * might arise due to an invalid "term" component.
		 */
		return 0;
	}
	while(cur) {
		switch(WXS_PARTICLE_TERM(cur)->type) {
			case XML_SCHEMA_TYPE_ANY:
			    break;
			case XML_SCHEMA_TYPE_ELEMENT:
			    if(search == 0) {
				    ret = xmlSchemaCheckElementDeclConsistent(pctxt,
				    ctxtComponent, ctxtParticle, cur, ctxtParticle, 1);
				    if(ret != 0)
					    return ret;
			    }
			    else {
				    xmlSchemaElementPtr elem =
				    WXS_ELEM_CAST(WXS_PARTICLE_TERM(cur));
				    /*
				     * SPEC Element Declarations Consistent:
				     * "If the {particles} contains, either directly,
				     * indirectly (that is, within the {particles} of a
				     * contained model group, recursively) or `implicitly`
				     * two or more element declaration particles with
				     * the same {name} and {target namespace}, then
				     * all their type definitions must be the same
				     * top-level definition [...]"
				     */
				    if(sstreq(WXS_PARTICLE_TERM_AS_ELEM(cur)->name,
					    WXS_PARTICLE_TERM_AS_ELEM(searchParticle)->name) &&
				    sstreq(WXS_PARTICLE_TERM_AS_ELEM(cur)->targetNamespace,
					    WXS_PARTICLE_TERM_AS_ELEM(searchParticle)->targetNamespace)) {
					    xmlChar * strA = NULL, * strB = NULL;

					    xmlSchemaCustomErr(ACTXT_CAST pctxt,
					    /* @todo error code */
					    XML_SCHEMAP_COS_NONAMBIG,
					    WXS_ITEM_NODE(cur), NULL,
					    "In the content model of %s, there are multiple "
					    "element declarations for '%s' with different "
					    "type definitions",
					    xmlSchemaGetComponentDesignation(&strA,
						    ctxtComponent),
					    xmlSchemaFormatQName(&strB,
						    WXS_PARTICLE_TERM_AS_ELEM(cur)->targetNamespace,
						    WXS_PARTICLE_TERM_AS_ELEM(cur)->name));
					    ZFREE(strA);
					    ZFREE(strB);
					    return(XML_SCHEMAP_COS_NONAMBIG);
				    }
			    }
			    break;
			case XML_SCHEMA_TYPE_SEQUENCE: {
			    break;
		    }
			case XML_SCHEMA_TYPE_CHOICE: {
			    /*
			       xmlSchemaTreeItemPtr sub;

			       sub = WXS_PARTICLE_TERM(particle)->children;  (xmlSchemaParticlePtr)
			       while (sub != NULL) {
			        ret = xmlSchemaCheckElementDeclConsistent(pctxt, ctxtComponent,
			            ctxtParticle, ctxtElem);
			        if (ret != 0)
			            return ret;
			        sub = sub->next;
			       }
			     */
			    break;
		    }
			case XML_SCHEMA_TYPE_ALL:
			    break;
			case XML_SCHEMA_TYPE_GROUP:
			    break;
			default:
			    xmlSchemaInternalErr2(ACTXT_CAST pctxt,
			    "xmlSchemaCheckElementDeclConsistent",
			    "found unexpected term of type '%s' in content model",
			    WXS_ITEM_TYPE_NAME(WXS_PARTICLE_TERM(cur)), 0);
			    return -1;
		}
		cur = (xmlSchemaParticlePtr)cur->next;
	}

exit:
	return ret;
}

#endif

/**
 * xmlSchemaCheckElementDeclComponent
 * @item:  an schema element declaration/particle
 * @ctxt:  a schema parser context
 * @name:  the name of the attribute
 *
 * Validates the value constraints of an element declaration.
 * Adds substitution group members.
 */
static void xmlSchemaCheckElementDeclComponent(xmlSchemaElementPtr elemDecl, xmlSchemaParserCtxtPtr ctxt)
{
	if(elemDecl && !(elemDecl->flags & XML_SCHEMAS_ELEM_INTERNAL_CHECKED)) {
		elemDecl->flags |= XML_SCHEMAS_ELEM_INTERNAL_CHECKED;
		if(xmlSchemaCheckElemPropsCorrect(ctxt, elemDecl) == 0)
			xmlSchemaCheckElemSubstGroup(ctxt, elemDecl);  // Adds substitution group members.
	}
}
/**
 * xmlSchemaResolveModelGroupParticleReferences:
 * @particle:  a particle component
 * @ctxt:  a parser context
 *
 * Resolves references of a model group's {particles} to
 * model group definitions and to element declarations.
 */
static void xmlSchemaResolveModelGroupParticleReferences(xmlSchemaParserCtxtPtr ctxt, xmlSchemaModelGroupPtr mg)
{
	xmlSchemaParticlePtr particle = WXS_MODELGROUP_PARTICLE(mg);
	xmlSchemaQNameRefPtr ref;
	xmlSchemaBasicItem * refItem;
	/*
	 * URGENT TODO: Test this.
	 */
	while(particle) {
		if((WXS_PARTICLE_TERM(particle) == NULL) || ((WXS_PARTICLE_TERM(particle))->type != XML_SCHEMA_EXTRA_QNAMEREF)) {
			goto next_particle;
		}
		ref = WXS_QNAME_CAST WXS_PARTICLE_TERM(particle);
		/*
		 * Resolve the reference.
		 * NULL the {term} by default.
		 */
		particle->children = NULL;
		refItem = xmlSchemaGetNamedComponent(ctxt->schema, ref->itemType, ref->name, ref->targetNamespace);
		if(refItem == NULL) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, NULL, WXS_ITEM_NODE(particle), "ref", ref->name, ref->targetNamespace, ref->itemType, 0);
			/* @todo remove the particle. */
			goto next_particle;
		}
		if(refItem->type == XML_SCHEMA_TYPE_GROUP) {
			if(WXS_MODELGROUPDEF_MODEL(refItem) == NULL)
				/* @todo remove the particle. */
				goto next_particle;
			/*
			 * NOTE that we will assign the model group definition
			 * itself to the "term" of the particle. This will ease
			 * the check for circular model group definitions. After
			 * that the "term" will be assigned the model group of the
			 * model group definition.
			 */
			if((WXS_MODELGROUPDEF_MODEL(refItem))->type == XML_SCHEMA_TYPE_ALL) {
				/*
				 * SPEC cos-all-limited (1)
				 * SPEC cos-all-limited (1.2)
				 * "It appears only as the value of one or both of the
				 * following properties:"
				 * (1.1) "the {model group} property of a model group
				 *        definition."
				 * (1.2) "the {term} property of a particle [... of] the "
				 * {content type} of a complex type definition."
				 */
				xmlSchemaCustomErr(ACTXT_CAST ctxt, /* @todo error code */
				    XML_SCHEMAP_COS_ALL_LIMITED, WXS_ITEM_NODE(particle), NULL,
				    "A model group definition is referenced, but it contains an 'all' model group, which cannot be contained by model groups", NULL, 0);
				/* @todo remove the particle. */
				goto next_particle;
			}
			particle->children = (xmlSchemaTreeItem *)refItem;
		}
		else {
			/*
			 * @todo Are referenced element declarations the only
			 * other components we expect here?
			 */
			particle->children = (xmlSchemaTreeItem *)refItem;
		}
next_particle:
		particle = WXS_PTC_CAST particle->next;
	}
}

static int FASTCALL xmlSchemaAreValuesEqual(xmlSchemaVal * x, xmlSchemaVal * y)
{
	while(x) {
		// Same types
		xmlSchemaType * tx = xmlSchemaGetBuiltInType(xmlSchemaGetValType(x));
		xmlSchemaType * ty = xmlSchemaGetBuiltInType(xmlSchemaGetValType(y));
		xmlSchemaType * ptx = xmlSchemaGetPrimitiveType(tx);
		xmlSchemaType * pty = xmlSchemaGetPrimitiveType(ty);
		// 
		// (1) if a datatype T' is `derived` by `restriction` from an
		// atomic datatype T then the `value space` of T' is a subset of the `value space` of T.
		// 
		// (2) if datatypes T' and T'' are `derived` by `restriction`
		// from a common atomic ancestor T then the `value space`s of T' and T'' may overlap.
		// 
		if(ptx != pty)
			return 0;
		// 
		// We assume computed values to be normalized, so do a fast
		// string comparison for string based types.
		// 
		if((ptx->builtInType == XML_SCHEMAS_STRING) || WXS_IS_ANY_SIMPLE_TYPE(ptx)) {
			if(!sstreq(xmlSchemaValueGetAsString(x), xmlSchemaValueGetAsString(y)))
				return 0;
		}
		else {
			int ret = xmlSchemaCompareValuesWhtsp(x, XML_SCHEMA_WHITESPACE_PRESERVE, y, XML_SCHEMA_WHITESPACE_PRESERVE);
			if(ret == -2)
				return -1;
			else if(ret != 0)
				return 0;
		}
		// 
		// Lists
		// 
		x = xmlSchemaValueGetNext(x);
		if(x) {
			y = xmlSchemaValueGetNext(y);
			if(y == NULL)
				return 0;
		}
		else if(xmlSchemaValueGetNext(y))
			return 0;
		else
			return 1;
	}
	return 0;
}

/**
 * xmlSchemaResolveAttrUseReferences:
 * @item:  an attribute use
 * @ctxt:  a parser context
 *
 * Resolves the referenced attribute declaration.
 */
static int xmlSchemaResolveAttrUseReferences(xmlSchemaAttributeUsePtr ause,
    xmlSchemaParserCtxtPtr ctxt)
{
	if(!ctxt || (ause == NULL))
		return -1;
	if((ause->attrDecl == NULL) ||
	    (ause->attrDecl->type != XML_SCHEMA_EXTRA_QNAMEREF))
		return 0;

	{
		xmlSchemaQNameRefPtr ref = WXS_QNAME_CAST ause->attrDecl;

		/*
		 * @todo Evaluate, what errors could occur if the declaration is not
		 * found.
		 */
		ause->attrDecl = xmlSchemaGetAttributeDecl(ctxt->schema, ref->name, ref->targetNamespace);
		if(ause->attrDecl == NULL) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE,
			    WXS_BASIC_CAST ause, ause->P_Node, "ref", ref->name, ref->targetNamespace, XML_SCHEMA_TYPE_ATTRIBUTE, 0);
			return(ctxt->err);
		}
	}
	return 0;
}

/**
 * xmlSchemaCheckAttrUsePropsCorrect:
 * @ctxt:  a parser context
 * @use:  an attribute use
 *
 * Schema Component Constraint:
 * Attribute Use Correct (au-props-correct)
 *
 */
static int xmlSchemaCheckAttrUsePropsCorrect(xmlSchemaParserCtxtPtr ctxt, xmlSchemaAttributeUsePtr use)
{
	if(!ctxt || (use == NULL))
		return -1;
	if((use->defValue == NULL) || (WXS_ATTRUSE_DECL(use) == NULL) || ((WXS_ATTRUSE_DECL(use))->type != XML_SCHEMA_TYPE_ATTRIBUTE))
		return 0;
	/*
	 * SPEC au-props-correct (1)
	 * "The values of the properties of an attribute use must be as
	 * described in the property tableau in The Attribute Use Schema
	 * Component ($3.5.1), modulo the impact of Missing
	 * Sub-components ($5.3)."
	 */

	if((WXS_ATTRUSE_DECL(use))->defValue && ((WXS_ATTRUSE_DECL(use))->flags & XML_SCHEMAS_ATTR_FIXED) && ((use->flags & XML_SCHEMA_ATTR_USE_FIXED) == 0)) {
		xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_AU_PROPS_CORRECT_2, WXS_BASIC_CAST use, NULL,
		    "The attribute declaration has a 'fixed' value constraint , thus the attribute use must also have a 'fixed' value constraint", NULL);
		return(ctxt->err);
	}
	/*
	 * Compute and check the value constraint's value.
	 */
	if(use->defVal && WXS_ATTRUSE_TYPEDEF(use)) {
		int ret;
		/*
		 * @todo The spec seems to be missing a check of the
		 * value constraint of the attribute use. We will do it here.
		 */
		/*
		 * SPEC a-props-correct (3)
		 */
		if(xmlSchemaIsDerivedFromBuiltInType(WXS_ATTRUSE_TYPEDEF(use), XML_SCHEMAS_ID)) {
			xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_AU_PROPS_CORRECT, NULL, WXS_BASIC_CAST use,
			    "Value constraints are not allowed if the type definition is or is derived from xs:ID", NULL, 0);
			return(ctxt->err);
		}
		ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST ctxt, use->P_Node, WXS_ATTRUSE_TYPEDEF(use), use->defValue, &(use->defVal), 1, 1, 0);
		if(ret != 0) {
			if(ret < 0) {
				PERROR_INT2("xmlSchemaCheckAttrUsePropsCorrect", "calling xmlSchemaVCheckCVCSimpleType()");
				return -1;
			}
			xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_AU_PROPS_CORRECT, NULL, WXS_BASIC_CAST use,
			    "The value of the value constraint is not valid", NULL, 0);
			return(ctxt->err);
		}
	}
	/*
	 * SPEC au-props-correct (2)
	 * "If the {attribute declaration} has a fixed
	 * {value constraint}, then if the attribute use itself has a
	 * {value constraint}, it must also be fixed and its value must match
	 * that of the {attribute declaration}'s {value constraint}."
	 */
	if((WXS_ATTRUSE_DECL(use))->defVal && (((WXS_ATTRUSE_DECL(use))->flags & XML_SCHEMA_ATTR_USE_FIXED) == 0)) {
		if(!xmlSchemaAreValuesEqual(use->defVal, (WXS_ATTRUSE_DECL(use))->defVal)) {
			xmlSchemaPCustomErr(ctxt, XML_SCHEMAP_AU_PROPS_CORRECT_2, WXS_BASIC_CAST use, NULL,
			    "The 'fixed' value constraint of the attribute use must match the attribute declaration's value constraint '%s'",
			    (WXS_ATTRUSE_DECL(use))->defValue);
		}
		return(ctxt->err);
	}
	return 0;
}

/**
 * xmlSchemaResolveAttrTypeReferences:
 * @item:  an attribute declaration
 * @ctxt:  a parser context
 *
 * Resolves the referenced type definition component.
 */
static int xmlSchemaResolveAttrTypeReferences(xmlSchemaAttributePtr item, xmlSchemaParserCtxtPtr ctxt)
{
	/*
	 * The simple type definition corresponding to the <simpleType> element
	 * information item in the [children], if present, otherwise the simple
	 * type definition `resolved` to by the `actual value` of the type
	 * [attribute], if present, otherwise the `simple ur-type definition`.
	 */
	if(item->flags & XML_SCHEMAS_ATTR_INTERNAL_RESOLVED)
		return 0;
	item->flags |= XML_SCHEMAS_ATTR_INTERNAL_RESOLVED;
	if(item->subtypes)
		return 0;
	if(item->typeName) {
		xmlSchemaTypePtr type = xmlSchemaGetType(ctxt->schema, item->typeName, item->typeNs);
		if((type == NULL) || (!WXS_IS_SIMPLE(type))) {
			xmlSchemaPResCompAttrErr(ctxt, XML_SCHEMAP_SRC_RESOLVE, WXS_BASIC_CAST item, item->P_Node,
			    "type", item->typeName, item->typeNs, XML_SCHEMA_TYPE_SIMPLE, 0);
			return(ctxt->err);
		}
		else
			item->subtypes = type;
	}
	else {
		/*
		 * The type defaults to the xs:anySimpleType.
		 */
		item->subtypes = xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYSIMPLETYPE);
	}
	return 0;
}

/**
 * xmlSchemaResolveIDCKeyReferences:
 * @idc:  the identity-constraint definition
 * @ctxt:  the schema parser context
 * @name:  the attribute name
 *
 * Resolve keyRef references to key/unique IDCs.
 * Schema Component Constraint:
 *   Identity-constraint Definition Properties Correct (c-props-correct)
 */
static int xmlSchemaResolveIDCKeyReferences(xmlSchemaIDCPtr idc, xmlSchemaParserCtxtPtr pctxt)
{
	if(idc->type != XML_SCHEMA_TYPE_IDC_KEYREF)
		return 0;
	if(idc->ref->name) {
		idc->ref->item = (xmlSchemaBasicItem *)xmlSchemaGetIDC(pctxt->schema, idc->ref->name, idc->ref->targetNamespace);
		if(idc->ref->item == NULL) {
			/*
			 * @todo It is actually not an error to fail to resolve
			 * at this stage. BUT we need to be that strict!
			 */
			xmlSchemaPResCompAttrErr(pctxt, XML_SCHEMAP_SRC_RESOLVE, WXS_BASIC_CAST idc, idc->P_Node,
			    "refer", idc->ref->name, idc->ref->targetNamespace, XML_SCHEMA_TYPE_IDC_KEY, 0);
			return(pctxt->err);
		}
		else if(idc->ref->item->type == XML_SCHEMA_TYPE_IDC_KEYREF) {
			/*
			 * SPEC c-props-correct (1)
			 */
			xmlSchemaCustomErr(ACTXT_CAST pctxt,
			    XML_SCHEMAP_C_PROPS_CORRECT,
			    NULL, WXS_BASIC_CAST idc,
			    "The keyref references a keyref",
			    NULL, 0);
			idc->ref->item = NULL;
			return(pctxt->err);
		}
		else {
			if(idc->nbFields != ((xmlSchemaIDCPtr)idc->ref->item)->nbFields) {
				xmlChar * str = NULL;
				xmlSchemaIDC * refer = (xmlSchemaIDCPtr)idc->ref->item;
				/*
				 * SPEC c-props-correct(2)
				 * "If the {identity-constraint category} is keyref,
				 * the cardinality of the {fields} must equal that of
				 * the {fields} of the {referenced key}.
				 */
				xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_C_PROPS_CORRECT, NULL, WXS_BASIC_CAST idc,
				    "The cardinality of the keyref differs from the cardinality of the referenced key/unique '%s'",
				    xmlSchemaFormatQName(&str, refer->targetNamespace, refer->name), NULL);
				ZFREE(str);
				return(pctxt->err);
			}
		}
	}
	return 0;
}

static int xmlSchemaResolveAttrUseProhibReferences(xmlSchemaAttributeUseProhibPtr prohib, xmlSchemaParserCtxtPtr pctxt)
{
	if(xmlSchemaGetAttributeDecl(pctxt->schema, prohib->name, prohib->targetNamespace) == NULL) {
		xmlSchemaPResCompAttrErr(pctxt, XML_SCHEMAP_SRC_RESOLVE, NULL, prohib->P_Node,
		    "ref", prohib->name, prohib->targetNamespace, XML_SCHEMA_TYPE_ATTRIBUTE, 0);
		return XML_SCHEMAP_SRC_RESOLVE;
	}
	return 0;
}

#define WXS_REDEFINED_TYPE(c) (((xmlSchemaTypePtr)item)->flags & XML_SCHEMAS_TYPE_REDEFINED)
#define WXS_REDEFINED_MODEL_GROUP_DEF(c) (((xmlSchemaModelGroupDefPtr)item)->flags & XML_SCHEMA_MODEL_GROUP_DEF_REDEFINED)
#define WXS_REDEFINED_ATTR_GROUP(c) (((xmlSchemaAttributeGroupPtr)item)->flags & XML_SCHEMAS_ATTRGROUP_REDEFINED)

static int xmlSchemaCheckSRCRedefineFirst(xmlSchemaParserCtxtPtr pctxt)
{
	int err = 0;
	xmlSchemaRedefPtr redef = WXS_CONSTRUCTOR(pctxt)->redefs;
	xmlSchemaBasicItem * prev;
	xmlSchemaBasicItem * item;
	int wasRedefined;
	if(redef == NULL)
		return 0;
	do {
		item = redef->item;
		/*
		 * First try to locate the redefined component in the
		 * schema graph starting with the redefined schema.
		 * NOTE: According to this schema bug entry:
		 *   http://lists.w3.org/Archives/Public/www-xml-schema-comments/2005OctDec/0019.html
		 *   it's not clear if the referenced component needs to originate
		 *   from the <redefine>d schema _document_ or the schema; the latter
		 *   would include all imported and included sub-schemas of the
		 *   <redefine>d schema. Currenlty we latter approach is used.
		 *   SUPPLEMENT: It seems that the WG moves towards the latter
		 *   approach, so we are doing it right.
		 *
		 */
		prev = xmlSchemaFindRedefCompInGraph(redef->targetBucket, item->type, redef->refName, redef->refTargetNs);
		if(prev == NULL) {
			xmlChar * str = NULL;
			xmlNode * P_Node;
			/*
			 * SPEC src-redefine:
			 * (6.2.1) "The `actual value` of its own name attribute plus
			 * target namespace must successfully `resolve` to a model
			 * group definition in I."
			 * (7.2.1) "The `actual value` of its own name attribute plus
			 * target namespace must successfully `resolve` to an attribute
			 * group definition in I."

			 *
			 * Note that, if we are redefining with the use of references
			 * to components, the spec assumes the src-resolve to be used;
			 * but this won't assure that we search only *inside* the
			 * redefined schema.
			 */
			if(redef->reference)
				P_Node = WXS_ITEM_NODE(redef->reference);
			else
				P_Node = WXS_ITEM_NODE(item);
			xmlSchemaCustomErr(ACTXT_CAST pctxt,
			    /*
			     * @todo error code.
			     * Probably XML_SCHEMAP_SRC_RESOLVE, if this is using the
			     * reference kind.
			     */
			    XML_SCHEMAP_SRC_REDEFINE, P_Node, NULL, "The %s '%s' to be redefined could not be found in the redefined schema",
					WXS_ITEM_TYPE_NAME(item), xmlSchemaFormatQName(&str, redef->refTargetNs, redef->refName));
			ZFREE(str);
			err = pctxt->err;
			redef = redef->next;
			continue;
		}
		/*
		 * @todo Obtaining and setting the redefinition state is really
		 * clumsy.
		 */
		wasRedefined = 0;
		switch(item->type) {
			case XML_SCHEMA_TYPE_COMPLEX:
			case XML_SCHEMA_TYPE_SIMPLE:
			    if((WXS_TYPE_CAST prev)->flags &
			    XML_SCHEMAS_TYPE_REDEFINED) {
				    wasRedefined = 1;
				    break;
			    }
			    /* Mark it as redefined. */
			    (WXS_TYPE_CAST prev)->flags |= XML_SCHEMAS_TYPE_REDEFINED;
			    /*
			     * Assign the redefined type to the
			     * base type of the redefining type.
			     * @todo How
			     */
			    ((xmlSchemaTypePtr)item)->baseType =
			    (xmlSchemaTypePtr)prev;
			    break;
			case XML_SCHEMA_TYPE_GROUP:
			    if((WXS_MODEL_GROUPDEF_CAST prev)->flags &
			    XML_SCHEMA_MODEL_GROUP_DEF_REDEFINED) {
				    wasRedefined = 1;
				    break;
			    }
			    /* Mark it as redefined. */
			    (WXS_MODEL_GROUPDEF_CAST prev)->flags |=
			    XML_SCHEMA_MODEL_GROUP_DEF_REDEFINED;
			    if(redef->reference != NULL) {
				    /*
				     * Overwrite the QName-reference with the
				     * referenced model group def.
				     */
				    (WXS_PTC_CAST redef->reference)->children =
				    WXS_TREE_CAST prev;
			    }
			    redef->target = prev;
			    break;
			case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
			    if((WXS_ATTR_GROUP_CAST prev)->flags &
			    XML_SCHEMAS_ATTRGROUP_REDEFINED) {
				    wasRedefined = 1;
				    break;
			    }
			    (WXS_ATTR_GROUP_CAST prev)->flags |=
			    XML_SCHEMAS_ATTRGROUP_REDEFINED;
			    if(redef->reference != NULL) {
				    /*
				     * Assign the redefined attribute group to the
				     * QName-reference component.
				     * This is the easy case, since we will just
				     * expand the redefined group.
				     */
				    (WXS_QNAME_CAST redef->reference)->item = prev;
				    redef->target = NULL;
			    }
			    else {
				    /*
				     * This is the complicated case: we need
				     * to apply src-redefine (7.2.2) at a later
				     * stage, i.e. when attribute group references
				     * have beed expanded and simple types have
				     * beed fixed.
				     */
				    redef->target = prev;
			    }
			    break;
			default:
			    PERROR_INT("xmlSchemaResolveRedefReferences", "Unexpected redefined component type");
			    return -1;
		}
		if(wasRedefined) {
			xmlChar * str = NULL;
			xmlNode * P_Node =  redef->reference ? WXS_ITEM_NODE(redef->reference) : WXS_ITEM_NODE(redef->item);
			xmlSchemaCustomErr(ACTXT_CAST pctxt,
			    /* @todo error code. */
			    XML_SCHEMAP_SRC_REDEFINE, P_Node, NULL,
			    "The referenced %s was already redefined. Multiple redefinition of the same component is not supported",
			    xmlSchemaGetComponentDesignation(&str, prev), 0);
			ZFREE(str);
			err = pctxt->err;
			redef = redef->next;
			continue;
		}
		redef = redef->next;
	} while(redef != NULL);

	return(err);
}

static int xmlSchemaCheckSRCRedefineSecond(xmlSchemaParserCtxtPtr pctxt)
{
	int err = 0;
	xmlSchemaRedef * redef = WXS_CONSTRUCTOR(pctxt)->redefs;
	xmlSchemaBasicItem * item;
	if(redef == NULL)
		return 0;
	do {
		if(redef->target == NULL) {
			redef = redef->next;
			continue;
		}
		item = redef->item;

		switch(item->type) {
			case XML_SCHEMA_TYPE_SIMPLE:
			case XML_SCHEMA_TYPE_COMPLEX:
			    /*
			     * Since the spec wants the {name} of the redefined
			     * type to be 'absent', we'll NULL it.
			     */
			    (WXS_TYPE_CAST redef->target)->name = NULL;

			    /*
			     * @todo Seems like there's nothing more to do. The normal
			     * inheritance mechanism is used. But not 100% sure.
			     */
			    break;
			case XML_SCHEMA_TYPE_GROUP:
			    /*
			     * URGENT TODO:
			     * SPEC src-redefine:
			     * (6.2.2) "The {model group} of the model group definition
			     * which corresponds to it per XML Representation of Model
			     * Group Definition Schema Components ($3.7.2) must be a
			     * `valid restriction` of the {model group} of that model
			     * group definition in I, as defined in Particle Valid
			     * (Restriction) ($3.9.6)."
			     */
			    break;
			case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
			    /*
			     * SPEC src-redefine:
			     * (7.2.2) "The {attribute uses} and {attribute wildcard} of
			     * the attribute group definition which corresponds to it
			     * per XML Representation of Attribute Group Definition Schema
			     * Components ($3.6.2) must be `valid restrictions` of the
			     * {attribute uses} and {attribute wildcard} of that attribute
			     * group definition in I, as defined in clause 2, clause 3 and
			     * clause 4 of Derivation Valid (Restriction, Complex)
			     * ($3.4.6) (where references to the base type definition are
			     * understood as references to the attribute group definition
			     * in I)."
			     */
			    err = xmlSchemaCheckDerivationOKRestriction2to4(pctxt, XML_SCHEMA_ACTION_REDEFINE, item, redef->target,
					(xmlSchemaItemListPtr)(WXS_ATTR_GROUP_CAST item)->attrUses, (xmlSchemaItemListPtr)(WXS_ATTR_GROUP_CAST redef->target)->attrUses,
					(WXS_ATTR_GROUP_CAST item)->attributeWildcard, (WXS_ATTR_GROUP_CAST redef->target)->attributeWildcard);
			    if(err == -1)
				    return -1;
			    break;
			default:
			    break;
		}
		redef = redef->next;
	} while(redef != NULL);
	return 0;
}

static int xmlSchemaAddComponents(xmlSchemaParserCtxtPtr pctxt, xmlSchemaBucketPtr bucket)
{
	xmlSchemaBasicItem * item;
	int err;
	xmlHashTable ** table;
	const xmlChar * name;
	int i;
#define WXS_GET_GLOBAL_HASH(c, slot) { table = (WXS_IS_BUCKET_IMPMAIN((c)->type)) ? &(WXS_IMPBUCKET((c))->schema->slot) : &(WXS_INCBUCKET((c))->ownerImport->schema->slot); }
	/*
	 * Add global components to the schema's hash tables.
	 * This is the place where duplicate components will be
	 * detected.
	 * @todo I think normally we should support imports of the
	 *   same namespace from multiple locations. We don't do currently,
	 *   but if we do then according to:
	 *   http://www.w3.org/Bugs/Public/show_bug.cgi?id=2224
	 *   we would need, if imported directly, to import redefined
	 *   components as well to be able to catch clashing components.
	 *   (I hope I'll still know what this means after some months :-()
	 */
	if(bucket == NULL)
		return -1;
	if(bucket->flags & XML_SCHEMA_BUCKET_COMPS_ADDED)
		return 0;
	bucket->flags |= XML_SCHEMA_BUCKET_COMPS_ADDED;
	for(i = 0; i < bucket->globals->nbItems; i++) {
		item = (xmlSchemaBasicItem *)bucket->globals->items[i];
		table = NULL;
		switch(item->type) {
			case XML_SCHEMA_TYPE_COMPLEX:
			case XML_SCHEMA_TYPE_SIMPLE:
			    if(WXS_REDEFINED_TYPE(item))
				    continue;
			    name = (WXS_TYPE_CAST item)->name;
			    WXS_GET_GLOBAL_HASH(bucket, typeDecl)
			    break;
			case XML_SCHEMA_TYPE_ELEMENT:
			    name = (WXS_ELEM_CAST item)->name;
			    WXS_GET_GLOBAL_HASH(bucket, elemDecl)
			    break;
			case XML_SCHEMA_TYPE_ATTRIBUTE:
			    name = (WXS_ATTR_CAST item)->name;
			    WXS_GET_GLOBAL_HASH(bucket, attrDecl)
			    break;
			case XML_SCHEMA_TYPE_GROUP:
			    if(WXS_REDEFINED_MODEL_GROUP_DEF(item))
				    continue;
			    name = (WXS_MODEL_GROUPDEF_CAST item)->name;
			    WXS_GET_GLOBAL_HASH(bucket, groupDecl)
			    break;
			case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
			    if(WXS_REDEFINED_ATTR_GROUP(item))
				    continue;
			    name = (WXS_ATTR_GROUP_CAST item)->name;
			    WXS_GET_GLOBAL_HASH(bucket, attrgrpDecl)
			    break;
			case XML_SCHEMA_TYPE_IDC_KEY:
			case XML_SCHEMA_TYPE_IDC_UNIQUE:
			case XML_SCHEMA_TYPE_IDC_KEYREF:
			    name = (WXS_IDC_CAST item)->name;
			    WXS_GET_GLOBAL_HASH(bucket, idcDef)
			    break;
			case XML_SCHEMA_TYPE_NOTATION:
			    name = ((xmlSchemaNotationPtr)item)->name;
			    WXS_GET_GLOBAL_HASH(bucket, notaDecl)
			    break;
			default:
			    PERROR_INT("xmlSchemaAddComponents", "Unexpected global component type");
			    continue;
		}
		if(*table == NULL) {
			*table = xmlHashCreateDict(10, pctxt->dict);
			if(*table == NULL) {
				PERROR_INT("xmlSchemaAddComponents", "failed to create a component hash table");
				return -1;
			}
		}
		err = xmlHashAddEntry(*table, name, item);
		if(err != 0) {
			xmlChar * str = NULL;
			xmlSchemaCustomErr(ACTXT_CAST pctxt, XML_SCHEMAP_REDEFINED_TYPE,
			    WXS_ITEM_NODE(item), WXS_BASIC_CAST item, "A global %s '%s' does already exist",
			    WXS_ITEM_TYPE_NAME(item), xmlSchemaGetComponentQName(&str, item));
			ZFREE(str);
		}
	}
	/*
	 * Process imported/included schemas.
	 */
	if(bucket->relations != NULL) {
		xmlSchemaSchemaRelationPtr rel = bucket->relations;
		do {
			if(rel->bucket && ((rel->bucket->flags & XML_SCHEMA_BUCKET_COMPS_ADDED) == 0)) {
				if(xmlSchemaAddComponents(pctxt, rel->bucket) == -1)
					return -1;
			}
			rel = rel->next;
		} while(rel != NULL);
	}
	return 0;
}

static int xmlSchemaFixupComponents(xmlSchemaParserCtxtPtr pctxt, xmlSchemaBucketPtr rootBucket)
{
	xmlSchemaConstructionCtxtPtr con = pctxt->constructor;
	xmlSchemaTreeItem * item;
	xmlSchemaTreeItem ** items;
	int nbItems, i, ret = 0;
	xmlSchemaBucketPtr oldbucket = con->bucket;
	xmlSchemaElementPtr elemDecl;
#define FIXHFAILURE if(pctxt->err == XML_SCHEMAP_INTERNAL) goto exit_failure;
	if(!con->pending || !con->pending->nbItems)
		return 0;
	/*
	 * Since xmlSchemaFixupComplexType() will create new particles
	 * (local components), and those particle components need a bucket
	 * on the constructor, we'll assure here that the constructor has
	 * a bucket.
	 * @todo Think about storing locals _only_ on the main bucket.
	 */
	SETIFZ(con->bucket, rootBucket);
	/* @todo 
	 * SPEC (src-redefine):
	 * (6.2) "If it has no such self-reference, then all of the
	 * following must be true:"

	 * (6.2.2) The {model group} of the model group definition which
	 * corresponds to it per XML Representation of Model Group
	 * Definition Schema Components ($3.7.2) must be a `valid
	 * restriction` of the {model group} of that model group definition
	 * in I, as defined in Particle Valid (Restriction) ($3.9.6)."
	 */
	xmlSchemaCheckSRCRedefineFirst(pctxt);
	/*
	 * Add global components to the schemata's hash tables.
	 */
	xmlSchemaAddComponents(pctxt, rootBucket);
	pctxt->ctxtType = NULL;
	items = (xmlSchemaTreeItem **)con->pending->items;
	nbItems = con->pending->nbItems;
	/*
	 * Now that we have parsed *all* the schema document(s) and converted
	 * them to schema components, we can resolve references, apply component
	 * constraints, create the FSA from the content model, etc.
	 */
	/*
	 * Resolve references of..
	 *
	 * 1. element declarations:
	 *   - the type definition
	 *   - the substitution group affiliation
	 * 2. simple/complex types:
	 *   - the base type definition
	 *   - the memberTypes of union types
	 *   - the itemType of list types
	 * 3. attributes declarations and attribute uses:
	 *   - the type definition
	 *   - if an attribute use, then the attribute declaration
	 * 4. attribute group references:
	 *   - the attribute group definition
	 * 5. particles:
	 *   - the term of the particle (e.g. a model group)
	 * 6. IDC key-references:
	 *   - the referenced IDC 'key' or 'unique' definition
	 * 7. Attribute prohibitions which had a "ref" attribute.
	 */
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_ELEMENT:
			    xmlSchemaResolveElementReferences((xmlSchemaElementPtr)item, pctxt);
			    FIXHFAILURE;
			    break;
			case XML_SCHEMA_TYPE_COMPLEX:
			case XML_SCHEMA_TYPE_SIMPLE:
			    xmlSchemaResolveTypeReferences((xmlSchemaTypePtr)item, pctxt);
			    FIXHFAILURE;
			    break;
			case XML_SCHEMA_TYPE_ATTRIBUTE:
			    xmlSchemaResolveAttrTypeReferences((xmlSchemaAttributePtr)item, pctxt);
			    FIXHFAILURE;
			    break;
			case XML_SCHEMA_TYPE_ATTRIBUTE_USE:
			    xmlSchemaResolveAttrUseReferences((xmlSchemaAttributeUsePtr)item, pctxt);
			    FIXHFAILURE;
			    break;
			case XML_SCHEMA_EXTRA_QNAMEREF:
			    if((WXS_QNAME_CAST item)->itemType == XML_SCHEMA_TYPE_ATTRIBUTEGROUP) {
				    xmlSchemaResolveAttrGroupReferences(WXS_QNAME_CAST item, pctxt);
			    }
			    FIXHFAILURE;
			    break;
			case XML_SCHEMA_TYPE_SEQUENCE:
			case XML_SCHEMA_TYPE_CHOICE:
			case XML_SCHEMA_TYPE_ALL:
			    xmlSchemaResolveModelGroupParticleReferences(pctxt, WXS_MODEL_GROUP_CAST item);
			    FIXHFAILURE;
			    break;
			case XML_SCHEMA_TYPE_IDC_KEY:
			case XML_SCHEMA_TYPE_IDC_UNIQUE:
			case XML_SCHEMA_TYPE_IDC_KEYREF:
			    xmlSchemaResolveIDCKeyReferences((xmlSchemaIDCPtr)item, pctxt);
			    FIXHFAILURE;
			    break;
			case XML_SCHEMA_EXTRA_ATTR_USE_PROHIB:
			    /*
			     * Handle attribue prohibition which had a
			     * "ref" attribute.
			     */
			    xmlSchemaResolveAttrUseProhibReferences(WXS_ATTR_PROHIB_CAST item, pctxt);
			    FIXHFAILURE;
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;

	/*
	 * Now that all references are resolved we
	 * can check for circularity of...
	 * 1. the base axis of type definitions
	 * 2. nested model group definitions
	 * 3. nested attribute group definitions
	 * @todo check for circual substitution groups.
	 */
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		/*
		 * Let's better stop on the first error here.
		 */
		switch(item->type) {
			case XML_SCHEMA_TYPE_COMPLEX:
			case XML_SCHEMA_TYPE_SIMPLE:
			    xmlSchemaCheckTypeDefCircular((xmlSchemaTypePtr)item, pctxt);
			    FIXHFAILURE;
			    if(pctxt->nberrors != 0)
				    goto exit_error;
			    break;
			case XML_SCHEMA_TYPE_GROUP:
			    xmlSchemaCheckGroupDefCircular((xmlSchemaModelGroupDefPtr)item, pctxt);
			    FIXHFAILURE;
			    if(pctxt->nberrors != 0)
				    goto exit_error;
			    break;
			case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
			    xmlSchemaCheckAttrGroupCircular((xmlSchemaAttributeGroupPtr)item, pctxt);
			    FIXHFAILURE;
			    if(pctxt->nberrors != 0)
				    goto exit_error;
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	/*
	 * Model group definition references:
	 * Such a reference is reflected by a particle at the component
	 * level. Until now the 'term' of such particles pointed
	 * to the model group definition; this was done, in order to
	 * ease circularity checks. Now we need to set the 'term' of
	 * such particles to the model group of the model group definition.
	 */
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_SEQUENCE:
			case XML_SCHEMA_TYPE_CHOICE:
			    xmlSchemaModelGroupToModelGroupDefFixup(pctxt, WXS_MODEL_GROUP_CAST item);
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Expand attribute group references of attribute group definitions.
	// 
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
			    if((!WXS_ATTR_GROUP_EXPANDED(item)) && WXS_ATTR_GROUP_HAS_REFS(item)) {
				    xmlSchemaAttributeGroupExpandRefs(pctxt, WXS_ATTR_GROUP_CAST item);
					FIXHFAILURE;
			    }
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	/*
	 * First compute the variety of simple types. This is needed as
	 * a seperate step, since otherwise we won't be able to detect
	 * circular union types in all cases.
	 */
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_SIMPLE:
			    if(WXS_IS_TYPE_NOT_FIXED_1((xmlSchemaTypePtr)item)) {
				    xmlSchemaFixupSimpleTypeStageOne(pctxt, (xmlSchemaTypePtr)item);
					FIXHFAILURE;
			    }
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Detect circular union types. Note that this needs the variety to be already computed.
	// 
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_SIMPLE:
			    if(((xmlSchemaTypePtr)item)->memberTypes) {
				    xmlSchemaCheckUnionTypeDefCircular(pctxt, (xmlSchemaTypePtr)item);
				    FIXHFAILURE;
			    }
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Do the complete type fixup for simple types.
	// 
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_SIMPLE:
			    if(WXS_IS_TYPE_NOT_FIXED(WXS_TYPE_CAST item)) {
				    xmlSchemaFixupSimpleTypeStageTwo(pctxt, WXS_TYPE_CAST item);
				    FIXHFAILURE;
			    }
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// At this point we need build and check all simple types.
	// 
	// Apply contraints for attribute declarations.
	// 
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_ATTRIBUTE:
			    xmlSchemaCheckAttrPropsCorrect(pctxt, WXS_ATTR_CAST item);
			    FIXHFAILURE;
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Apply constraints for attribute uses.
	// 
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_ATTRIBUTE_USE:
			    if(((xmlSchemaAttributeUsePtr)item)->defValue) {
				    xmlSchemaCheckAttrUsePropsCorrect(pctxt, WXS_ATTR_USE_CAST item);
				    FIXHFAILURE;
			    }
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Apply constraints for attribute group definitions.
	// 
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_ATTRIBUTEGROUP:
			    if((WXS_ATTR_GROUP_CAST item)->attrUses && ((WXS_LIST_CAST(WXS_ATTR_GROUP_CAST item)->attrUses)->nbItems > 1)) {
				    xmlSchemaCheckAGPropsCorrect(pctxt, WXS_ATTR_GROUP_CAST item);
				    FIXHFAILURE;
			    }
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Apply constraints for redefinitions.
	// 
	if(WXS_CONSTRUCTOR(pctxt)->redefs)
		xmlSchemaCheckSRCRedefineSecond(pctxt);
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Complex types are builded and checked.
	// 
	for(i = 0; i < nbItems; i++) {
		item = (xmlSchemaTreeItem *)con->pending->items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_COMPLEX:
			    if(WXS_IS_TYPE_NOT_FIXED(WXS_TYPE_CAST item)) {
				    xmlSchemaFixupComplexType(pctxt, WXS_TYPE_CAST item);
				    FIXHFAILURE;
			    }
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	/*
	 * The list could have changed, since xmlSchemaFixupComplexType()
	 * will create particles and model groups in some cases.
	 */
	items = (xmlSchemaTreeItem **)con->pending->items;
	nbItems = con->pending->nbItems;
	/*
	 * Apply some constraints for element declarations.
	 */
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_ELEMENT:
			    elemDecl = (xmlSchemaElementPtr)item;
			    if((elemDecl->flags & XML_SCHEMAS_ELEM_INTERNAL_CHECKED) == 0) {
				    xmlSchemaCheckElementDeclComponent((xmlSchemaElementPtr)elemDecl, pctxt); 
					FIXHFAILURE;
			    }
#ifdef WXS_ELEM_DECL_CONS_ENABLED
			    /*
			     * Schema Component Constraint: Element Declarations Consistent
			     * Apply this constraint to local types of element declarations.
			     */
			    if(WXS_ELEM_TYPEDEF(elemDecl) && (WXS_IS_COMPLEX(WXS_ELEM_TYPEDEF(elemDecl))) && (WXS_TYPE_IS_LOCAL(WXS_ELEM_TYPEDEF(elemDecl)))) {
				    xmlSchemaCheckElementDeclConsistent(pctxt, WXS_BASIC_CAST elemDecl, WXS_TYPE_PARTICLE(WXS_ELEM_TYPEDEF(elemDecl)), NULL, NULL, 0);
			    }
#endif
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// Finally we can build the automaton from the content model of complex types.
	// 
	for(i = 0; i < nbItems; i++) {
		item = items[i];
		switch(item->type) {
			case XML_SCHEMA_TYPE_COMPLEX:
			    xmlSchemaBuildContentModel((xmlSchemaTypePtr)item, pctxt);
			    // FIXHFAILURE; 
			    break;
			default:
			    break;
		}
	}
	if(pctxt->nberrors != 0)
		goto exit_error;
	// 
	// URGENT TODO: cos-element-consistent
	// 
	goto exit;
exit_error:
	ret = pctxt->err;
	goto exit;
exit_failure:
	ret = -1;
exit:
	// 
	// Reset the constructor. This is needed for XSI acquisition, since
	// those items will be processed over and over again for every XSI if not cleared here.
	// 
	con->bucket = oldbucket;
	con->pending->nbItems = 0;
	xmlHashFree(con->substGroups, (xmlHashDeallocator)xmlSchemaSubstGroupFree);
	con->substGroups = NULL;
	xmlSchemaRedefListFree(con->redefs);
	con->redefs = NULL;
	return ret;
}
/**
 * xmlSchemaParse:
 * @ctxt:  a schema validation context
 *
 * parse a schema definition resource and build an internal
 * XML Shema struture which can be used to validate instances.
 *
 * Returns the internal XML Schema structure built from the resource or NULL in case of error
 */
xmlSchemaPtr xmlSchemaParse(xmlSchemaParserCtxtPtr ctxt)
{
	xmlSchemaPtr mainSchema = NULL;
	xmlSchemaBucketPtr bucket = NULL;
	int res;
	// 
	// This one is used if the schema to be parsed was specified via
	// the API; i.e. not automatically by the validated instance document.
	// 
	xmlSchemaInitTypes();
	if(!ctxt)
		return 0;
	// @todo Init the context. Is this all we need?
	ctxt->nberrors = 0;
	ctxt->err = 0;
	ctxt->counter = 0;
	// Create the *main* schema. 
	mainSchema = xmlSchemaNewSchema(ctxt);
	if(mainSchema == NULL)
		goto exit_failure;
	// 
	// Create the schema constructor.
	// 
	if(ctxt->constructor == NULL) {
		ctxt->constructor = xmlSchemaConstructionCtxtCreate(ctxt->dict);
		if(ctxt->constructor == NULL)
			return 0;
		// Take ownership of the constructor to be able to free it. 
		ctxt->ownsConstructor = 1;
	}
	ctxt->constructor->mainSchema = mainSchema;
	// 
	// Locate and add the schema document.
	// 
	res = xmlSchemaAddSchemaDoc(ctxt, XML_SCHEMA_SCHEMA_MAIN, ctxt->URL, ctxt->doc, ctxt->buffer, ctxt->size, NULL, NULL, NULL, &bucket);
	if(res == -1)
		goto exit_failure;
	if(res != 0)
		goto exit;
	if(bucket == NULL) {
		// @todo Error code, actually we failed to *locate* the schema
		if(ctxt->URL)
			xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_FAILED_LOAD, NULL, NULL, "Failed to locate the main schema resource at '%s'", ctxt->URL, 0);
		else
			xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAP_FAILED_LOAD, NULL, NULL, "Failed to locate the main schema resource", 0, 0);
		goto exit;
	}
	// Then do the parsing for good
	if(xmlSchemaParseNewDocWithContext(ctxt, mainSchema, bucket) == -1)
		goto exit_failure;
	if(ctxt->nberrors)
		goto exit;
	mainSchema->doc = bucket->doc;
	mainSchema->preserve = ctxt->preserve;
	ctxt->schema = mainSchema;
	if(xmlSchemaFixupComponents(ctxt, WXS_CONSTRUCTOR(ctxt)->mainBucket) == -1)
		goto exit_failure;
	// 
	// @todo This is not nice, since we cannot distinguish from the result if there was an internal error or not.
	// 
exit:
	if(ctxt->nberrors != 0) {
		if(mainSchema) {
			xmlSchemaFree(mainSchema);
			mainSchema = NULL;
		}
		if(ctxt->constructor) {
			xmlSchemaConstructionCtxtFree(ctxt->constructor);
			ctxt->constructor = NULL;
			ctxt->ownsConstructor = 0;
		}
	}
	ctxt->schema = NULL;
	return mainSchema;
exit_failure:
	// 
	// Quite verbose, but should catch internal errors, which were not communitated.
	// 
	if(mainSchema) {
		xmlSchemaFree(mainSchema);
		mainSchema = NULL;
	}
	if(ctxt->constructor) {
		xmlSchemaConstructionCtxtFree(ctxt->constructor);
		ctxt->constructor = NULL;
		ctxt->ownsConstructor = 0;
	}
	PERROR_INT2("xmlSchemaParse", "An internal error occured");
	ctxt->schema = NULL;
	return 0;
}
/**
 * xmlSchemaSetParserErrors:
 * @ctxt:  a schema validation context
 * @err:  the error callback
 * @warn:  the warning callback
 * @ctx:  contextual data for the callbacks
 *
 * Set the callback functions used to handle errors for a validation context
 */
void xmlSchemaSetParserErrors(xmlSchemaParserCtxtPtr ctxt, xmlSchemaValidityErrorFunc err, xmlSchemaValidityWarningFunc warn, void * ctx)
{
	if(ctxt) {
		ctxt->F_Error = err;
		ctxt->F_Warning = warn;
		ctxt->errCtxt = ctx;
		xmlSchemaSetValidErrors(ctxt->vctxt, err, warn, ctx);
	}
}
/**
 * xmlSchemaSetParserStructuredErrors:
 * @ctxt:  a schema parser context
 * @serror:  the structured error function
 * @ctx: the functions context
 *
 * Set the structured error callback
 */
void xmlSchemaSetParserStructuredErrors(xmlSchemaParserCtxtPtr ctxt, xmlStructuredErrorFunc serror, void * ctx)
{
	if(ctxt) {
		ctxt->serror = serror;
		ctxt->errCtxt = ctx;
		if(ctxt->vctxt)
			xmlSchemaSetValidStructuredErrors(ctxt->vctxt, serror, ctx);
	}
}
/**
 * xmlSchemaGetParserErrors:
 * @ctxt:  a XMl-Schema parser context
 * @err: the error callback result
 * @warn: the warning callback result
 * @ctx: contextual data for the callbacks result
 *
 * Get the callback information used to handle errors for a parser context
 *
 * Returns -1 in case of failure, 0 otherwise
 */
int xmlSchemaGetParserErrors(xmlSchemaParserCtxtPtr ctxt, xmlSchemaValidityErrorFunc * err, xmlSchemaValidityWarningFunc * warn, void ** ctx)
{
	if(!ctxt)
		return -1;
	ASSIGN_PTR(err, ctxt->F_Error);
	ASSIGN_PTR(warn, ctxt->F_Warning);
	ASSIGN_PTR(ctx, ctxt->errCtxt);
	return 0;
}
/**
 * xmlSchemaFacetTypeToString:
 * @type:  the facet type
 *
 * Convert the xmlSchemaTypeType to a char string.
 *
 * Returns the char string representation of the facet type if the
 *     type is a facet and an "Internal Error" string otherwise.
 */
static const xmlChar * xmlSchemaFacetTypeToString(xmlSchemaTypeType type)
{
	switch(type) {
		case XML_SCHEMA_FACET_PATTERN: return (BAD_CAST "pattern");
		case XML_SCHEMA_FACET_MAXEXCLUSIVE: return (BAD_CAST "maxExclusive");
		case XML_SCHEMA_FACET_MAXINCLUSIVE: return (BAD_CAST "maxInclusive");
		case XML_SCHEMA_FACET_MINEXCLUSIVE: return (BAD_CAST "minExclusive");
		case XML_SCHEMA_FACET_MININCLUSIVE: return (BAD_CAST "minInclusive");
		case XML_SCHEMA_FACET_WHITESPACE: return (BAD_CAST "whiteSpace");
		case XML_SCHEMA_FACET_ENUMERATION: return (BAD_CAST "enumeration");
		case XML_SCHEMA_FACET_LENGTH: return (BAD_CAST "length");
		case XML_SCHEMA_FACET_MAXLENGTH: return (BAD_CAST "maxLength");
		case XML_SCHEMA_FACET_MINLENGTH: return (BAD_CAST "minLength");
		case XML_SCHEMA_FACET_TOTALDIGITS: return (BAD_CAST "totalDigits");
		case XML_SCHEMA_FACET_FRACTIONDIGITS: return (BAD_CAST "fractionDigits");
		default: break;
	}
	return (BAD_CAST "Internal Error");
}

static xmlSchemaWhitespaceValueType xmlSchemaGetWhiteSpaceFacetValue(xmlSchemaTypePtr type)
{
	/*
	 * The normalization type can be changed only for types which are derived
	 * from xsd:string.
	 */
	if(type->type == XML_SCHEMA_TYPE_BASIC) {
		/*
		 * Note that we assume a whitespace of preserve for anySimpleType.
		 */
		if((type->builtInType == XML_SCHEMAS_STRING) || (type->builtInType == XML_SCHEMAS_ANYSIMPLETYPE))
			return(XML_SCHEMA_WHITESPACE_PRESERVE);
		else if(type->builtInType == XML_SCHEMAS_NORMSTRING)
			return(XML_SCHEMA_WHITESPACE_REPLACE);
		else {
			/*
			 * For all `atomic` datatypes other than string (and types `derived`
			 * by `restriction` from it) the value of whiteSpace is fixed to
			 * collapse
			 * Note that this includes built-in list datatypes.
			 */
			return(XML_SCHEMA_WHITESPACE_COLLAPSE);
		}
	}
	else if(WXS_IS_LIST(type)) {
		/*
		 * For list types the facet "whiteSpace" is fixed to "collapse".
		 */
		return (XML_SCHEMA_WHITESPACE_COLLAPSE);
	}
	else if(WXS_IS_UNION(type)) {
		return (XML_SCHEMA_WHITESPACE_UNKNOWN);
	}
	else if(WXS_IS_ATOMIC(type)) {
		if(type->flags & XML_SCHEMAS_TYPE_WHITESPACE_PRESERVE)
			return (XML_SCHEMA_WHITESPACE_PRESERVE);
		else if(type->flags & XML_SCHEMAS_TYPE_WHITESPACE_REPLACE)
			return (XML_SCHEMA_WHITESPACE_REPLACE);
		else
			return (XML_SCHEMA_WHITESPACE_COLLAPSE);
	}
	return (xmlSchemaWhitespaceValueType)(-1);
}

/************************************************************************
*									*
*			Simple type validation				*
*									*
************************************************************************/

/************************************************************************
*									*
*			DOM Validation code				*
*									*
************************************************************************/

/**
 * xmlSchemaAssembleByLocation:
 * @pctxt:  a schema parser context
 * @vctxt:  a schema validation context
 * @schema: the existing schema
 * @node: the node that fired the assembling
 * @nsName: the namespace name of the new schema
 * @location: the location of the schema
 *
 * Expands an existing schema by an additional schema.
 *
 * Returns 0 if the new schema is correct, a positive error code
 * number otherwise and -1 in case of an internal or API error.
 */
static int xmlSchemaAssembleByLocation(xmlSchemaValidCtxtPtr vctxt, xmlSchemaPtr schema, xmlNode * P_Node, const xmlChar * nsName, const xmlChar * location)
{
	int ret = 0;
	xmlSchemaParserCtxtPtr pctxt;
	xmlSchemaBucketPtr bucket = NULL;
	if((vctxt == NULL) || (schema == NULL))
		return -1;
	if(vctxt->pctxt == NULL) {
		VERROR_INT("xmlSchemaAssembleByLocation", "no parser context available");
		return -1;
	}
	pctxt = vctxt->pctxt;
	if(pctxt->constructor == NULL) {
		PERROR_INT("xmlSchemaAssembleByLocation", "no constructor");
		return -1;
	}
	/*
	 * Acquire the schema document.
	 */
	location = xmlSchemaBuildAbsoluteURI(pctxt->dict, location, P_Node);
	/*
	 * Note that we pass XML_SCHEMA_SCHEMA_IMPORT here;
	 * the process will automatically change this to
	 * XML_SCHEMA_SCHEMA_MAIN if it is the first schema document.
	 */
	ret = xmlSchemaAddSchemaDoc(pctxt, XML_SCHEMA_SCHEMA_IMPORT, location, NULL, NULL, 0, P_Node, NULL, nsName, &bucket);
	if(ret != 0)
		return ret;
	if(bucket == NULL) {
		/*
		 * Generate a warning that the document could not be located.
		 */
		xmlSchemaCustomWarning(ACTXT_CAST vctxt, XML_SCHEMAV_MISC, P_Node, NULL, "The document at location '%s' could not be acquired", location, 0, 0);
		return ret;
	}
	/*
	 * The first located schema will be handled as if all other
	 * schemas imported by XSI were imported by this first schema.
	 */
	if(bucket && (WXS_CONSTRUCTOR(pctxt)->bucket == NULL))
		WXS_CONSTRUCTOR(pctxt)->bucket = bucket;
	//
	// @todo Is this handled like an import? I.e. is it not an error if the schema cannot be located?
	//
	if((bucket == NULL) || (!CAN_PARSE_SCHEMA(bucket)))
		return 0;
	// 
	// We will reuse the parser context for every schema imported directly via XSI. So reset the context.
	// 
	pctxt->nberrors = 0;
	pctxt->err = 0;
	pctxt->doc = bucket->doc;
	ret = xmlSchemaParseNewDocWithContext(pctxt, schema, bucket);
	if(ret == -1) {
		pctxt->doc = NULL;
		goto exit_failure;
	}
	/* Paranoid error channelling. */
	if((ret == 0) && (pctxt->nberrors != 0))
		ret = pctxt->err;
	if(pctxt->nberrors == 0) {
		// Only bother to fixup pending components, if there was no error yet.
		// For every XSI acquired schema (and its sub-schemata) we will fixup the components.
		xmlSchemaFixupComponents(pctxt, bucket);
		ret = pctxt->err;
		// Not nice, but we need somehow to channel the schema parser error to the validation context.
		if(ret && !vctxt->err)
			vctxt->err = ret;
		vctxt->nberrors += pctxt->nberrors;
	}
	else
		vctxt->nberrors += pctxt->nberrors; // Add to validation error sum
	pctxt->doc = NULL;
	return ret;
exit_failure:
	pctxt->doc = NULL;
	return -1;
}

static xmlSchemaAttrInfo * FASTCALL xmlSchemaGetMetaAttrInfo(xmlSchemaValidCtxt * vctxt, int metaType)
{
	if(vctxt->nbAttrInfos) {
		for(int i = 0; i < vctxt->nbAttrInfos; i++) {
			xmlSchemaAttrInfo * iattr = vctxt->attrInfos[i];
			if(iattr->metaType == metaType)
				return iattr;
		}
	}
	return 0;
}
/**
 * xmlSchemaAssembleByXSI:
 * @vctxt:  a schema validation context
 *
 * Expands an existing schema by an additional schema using
 * the xsi:schemaLocation or xsi:noNamespaceSchemaLocation attribute
 * of an instance. If xsi:noNamespaceSchemaLocation is used, @noNamespace
 * must be set to 1.
 *
 * Returns 0 if the new schema is correct, a positive error code
 * number otherwise and -1 in case of an internal or API error.
 */
static int xmlSchemaAssembleByXSI(xmlSchemaValidCtxtPtr vctxt)
{
	const xmlChar * cur, * end;
	const xmlChar * nsname = NULL, * location;
	int count = 0;
	int ret = 0;
	xmlSchemaAttrInfoPtr iattr;
	/*
	 * Parse the value; we will assume an even number of values
	 * to be given (this is how Xerces and XSV work).
	 *
	 * URGENT TODO: !! This needs to work for both
	 * @noNamespaceSchemaLocation AND @schemaLocation on the same
	 * element !!
	 */
	iattr = xmlSchemaGetMetaAttrInfo(vctxt, XML_SCHEMA_ATTR_INFO_META_XSI_SCHEMA_LOC);
	if(iattr == NULL)
		iattr = xmlSchemaGetMetaAttrInfo(vctxt, XML_SCHEMA_ATTR_INFO_META_XSI_NO_NS_SCHEMA_LOC);
	if(iattr == NULL)
		return 0;
	cur = iattr->value;
	do {
		/*
		 * @todo Move the string parsing mechanism away from here.
		 */
		if(iattr->metaType == XML_SCHEMA_ATTR_INFO_META_XSI_SCHEMA_LOC) {
			/*
			 * Get the namespace name.
			 */
			while(IS_BLANK_CH(*cur))
				cur++;
			end = cur;
			while((*end != 0) && (!(IS_BLANK_CH(*end))))
				end++;
			if(end == cur)
				break;
			count++; /* @todo Don't use the schema's dict. */
			nsname = xmlDictLookup(vctxt->schema->dict, cur, end - cur);
			cur = end;
		}
		/*
		 * Get the URI.
		 */
		while(IS_BLANK_CH(*cur))
			cur++;
		end = cur;
		while((*end != 0) && (!(IS_BLANK_CH(*end))))
			end++;
		if(end == cur) {
			if(iattr->metaType ==
			    XML_SCHEMA_ATTR_INFO_META_XSI_SCHEMA_LOC) {
				/*
				 * If using @schemaLocation then tuples are expected.
				 * I.e. the namespace name *and* the document's URI.
				 */
				xmlSchemaCustomWarning(ACTXT_CAST vctxt, XML_SCHEMAV_MISC, iattr->P_Node, NULL,
				    "The value must consist of tuples: the target namespace name and the document's URI", NULL, 0, 0);
			}
			break;
		}
		count++; /* @todo Don't use the schema's dict. */
		location = xmlDictLookup(vctxt->schema->dict, cur, end - cur);
		cur = end;
		ret = xmlSchemaAssembleByLocation(vctxt, vctxt->schema,
		    iattr->P_Node, nsname, location);
		if(ret == -1) {
			VERROR_INT("xmlSchemaAssembleByXSI", "assembling schemata");
			return -1;
		}
	} while(*cur != 0);
	return ret;
}

static const xmlChar * xmlSchemaLookupNamespace(xmlSchemaValidCtxtPtr vctxt, const xmlChar * prefix)
{
	if(vctxt->sax) {
		int i, j;
		xmlSchemaNodeInfoPtr inode;
		for(i = vctxt->depth; i >= 0; i--) {
			if(vctxt->elemInfos[i]->nbNsBindings != 0) {
				inode = vctxt->elemInfos[i];
				for(j = 0; j < inode->nbNsBindings * 2; j += 2) {
					if(sstreq(prefix, inode->nsBindings[j]))
						return (inode->nsBindings[j+1]); // Note that the namespace bindings are already in a string dict.
				}
			}
		}
		return 0;
#ifdef LIBXML_READER_ENABLED
	}
	else if(vctxt->reader) {
		xmlChar * nsName = xmlTextReaderLookupNamespace(vctxt->reader, prefix);
		if(nsName) {
			const xmlChar * ret = xmlDictLookupSL(vctxt->dict, nsName);
			SAlloc::F(nsName);
			return ret;
		}
		else
			return 0;
#endif
	}
	else {
		xmlNs * ns;
		if(!vctxt->inode->P_Node || !vctxt->inode->P_Node->doc) {
			VERROR_INT("xmlSchemaLookupNamespace", "no node or node's doc avaliable");
			return 0;
		}
		ns = xmlSearchNs(vctxt->inode->P_Node->doc, vctxt->inode->P_Node, prefix);
		return ns ? ns->href : 0;
	}
}
/*
 * This one works on the schema of the validation context.
 */
static int xmlSchemaValidateNotation(xmlSchemaValidCtxtPtr vctxt, xmlSchemaPtr schema, xmlNode * P_Node,
    const xmlChar * value, xmlSchemaValPtr * val, int valNeeded)
{
	int ret;
	if(vctxt && (vctxt->schema == NULL)) {
		VERROR_INT("xmlSchemaValidateNotation", "a schema is needed on the validation context");
		return -1;
	}
	ret = xmlValidateQName(value, 1);
	if(ret != 0)
		return ret;
	{
		xmlChar * prefix = NULL;
		xmlChar * localName = xmlSplitQName2(value, &prefix);
		if(prefix) {
			const xmlChar * nsName = NULL;
			if(vctxt)
				nsName = xmlSchemaLookupNamespace(vctxt, BAD_CAST prefix);
			else if(P_Node) {
				xmlNs * ns = xmlSearchNs(P_Node->doc, P_Node, prefix);
				if(ns)
					nsName = ns->href;
			}
			else {
				SAlloc::F(prefix);
				SAlloc::F(localName);
				return 1;
			}
			if(nsName == NULL) {
				SAlloc::F(prefix);
				SAlloc::F(localName);
				return 1;
			}
			if(xmlSchemaGetNotation(schema, localName, nsName) != NULL) {
				if((valNeeded) && (val != NULL)) {
					(*val) = xmlSchemaNewNOTATIONValue(sstrdup(localName), sstrdup(nsName));
					if(*val == NULL)
						ret = -1;
				}
			}
			else
				ret = 1;
			SAlloc::F(prefix);
			SAlloc::F(localName);
		}
		else {
			if(xmlSchemaGetNotation(schema, value, NULL)) {
				if(valNeeded && val) {
					(*val) = xmlSchemaNewNOTATIONValue(BAD_CAST sstrdup(value), 0);
					if(*val == NULL)
						ret = -1;
				}
			}
			else
				return 1;
		}
	}
	return ret;
}

static int xmlSchemaVAddNodeQName(xmlSchemaValidCtxtPtr vctxt, const xmlChar* lname, const xmlChar* nsname)
{
	int i;
	lname = xmlDictLookupSL(vctxt->dict, lname);
	if(lname == NULL)
		return -1;
	if(nsname) {
		nsname = xmlDictLookupSL(vctxt->dict, nsname);
		if(nsname == NULL)
			return -1;
	}
	for(i = 0; i < vctxt->nodeQNames->nbItems; i += 2) {
		if((vctxt->nodeQNames->items [i] == lname) && (vctxt->nodeQNames->items[i +1] == nsname))
			/* Already there */
			return(i);
	}
	/* Add new entry. */
	i = vctxt->nodeQNames->nbItems;
	xmlSchemaItemListAdd(vctxt->nodeQNames, (void*)lname);
	xmlSchemaItemListAdd(vctxt->nodeQNames, (void*)nsname);
	return(i);
}

/************************************************************************
*									*
*  Validation of identity-constraints (IDC)                            *
*									*
************************************************************************/

/**
 * xmlSchemaAugmentIDC:
 * @idcDef: the IDC definition
 *
 * Creates an augmented IDC definition item.
 *
 * Returns the item, or NULL on internal errors.
 */
static void xmlSchemaAugmentIDC(xmlSchemaIDCPtr idcDef, xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaIDCAugPtr aidc = (xmlSchemaIDCAugPtr)SAlloc::M(sizeof(xmlSchemaIDCAug));
	if(aidc == NULL) {
		xmlSchemaVErrMemory(vctxt, "xmlSchemaAugmentIDC: allocating an augmented IDC definition", 0);
		return;
	}
	aidc->keyrefDepth = -1;
	aidc->def = idcDef;
	aidc->next = NULL;
	if(vctxt->aidcs == NULL)
		vctxt->aidcs = aidc;
	else {
		aidc->next = vctxt->aidcs;
		vctxt->aidcs = aidc;
	}
	/*
	 * Save if we have keyrefs at all.
	 */
	if((vctxt->hasKeyrefs == 0) &&
	    (idcDef->type == XML_SCHEMA_TYPE_IDC_KEYREF))
		vctxt->hasKeyrefs = 1;
}

/**
 * xmlSchemaAugmentImportedIDC:
 * @imported: the imported schema
 *
 * Creates an augmented IDC definition for the imported schema.
 */
static void xmlSchemaAugmentImportedIDC(xmlSchemaImportPtr imported, xmlSchemaValidCtxtPtr vctxt) 
{
	if(imported->schema->idcDef) {
		xmlHashScan(imported->schema->idcDef, (xmlHashScanner)xmlSchemaAugmentIDC, vctxt);
	}
}

/**
 * xmlSchemaIDCNewBinding:
 * @idcDef: the IDC definition of this binding
 *
 * Creates a new IDC binding.
 *
 * Returns the new IDC binding, NULL on internal errors.
 */
static xmlSchemaPSVIIDCBindingPtr xmlSchemaIDCNewBinding(xmlSchemaIDCPtr idcDef)
{
	xmlSchemaPSVIIDCBindingPtr ret = (xmlSchemaPSVIIDCBindingPtr)SAlloc::M(sizeof(xmlSchemaPSVIIDCBinding));
	if(!ret) {
		xmlSchemaVErrMemory(NULL, "allocating a PSVI IDC binding item", 0);
	}
	else {
		memzero(ret, sizeof(xmlSchemaPSVIIDCBinding));
		ret->definition = idcDef;
	}
	return ret;
}

/**
 * xmlSchemaIDCStoreNodeTableItem:
 * @vctxt: the WXS validation context
 * @item: the IDC node table item
 *
 * The validation context is used to store IDC node table items.
 * They are stored to avoid copying them if IDC node-tables are merged
 * with corresponding parent IDC node-tables (bubbling).
 *
 * Returns 0 if succeeded, -1 on internal errors.
 */
static int xmlSchemaIDCStoreNodeTableItem(xmlSchemaValidCtxtPtr vctxt, xmlSchemaPSVIIDCNodePtr item)
{
	/*
	 * Add to gobal list.
	 */
	if(vctxt->idcNodes == NULL) {
		vctxt->idcNodes = (xmlSchemaPSVIIDCNodePtr*)SAlloc::M(20 * sizeof(xmlSchemaPSVIIDCNodePtr));
		if(vctxt->idcNodes == NULL) {
			xmlSchemaVErrMemory(vctxt, "allocating the IDC node table item list", 0);
			return -1;
		}
		vctxt->sizeIdcNodes = 20;
	}
	else if(vctxt->sizeIdcNodes <= vctxt->nbIdcNodes) {
		vctxt->sizeIdcNodes *= 2;
		vctxt->idcNodes = (xmlSchemaPSVIIDCNodePtr*)SAlloc::R(vctxt->idcNodes, vctxt->sizeIdcNodes * sizeof(xmlSchemaPSVIIDCNodePtr));
		if(vctxt->idcNodes == NULL) {
			xmlSchemaVErrMemory(vctxt, "re-allocating the IDC node table item list", 0);
			return -1;
		}
	}
	vctxt->idcNodes[vctxt->nbIdcNodes++] = item;

	return 0;
}

/**
 * xmlSchemaIDCStoreKey:
 * @vctxt: the WXS validation context
 * @item: the IDC key
 *
 * The validation context is used to store an IDC key.
 *
 * Returns 0 if succeeded, -1 on internal errors.
 */
static int xmlSchemaIDCStoreKey(xmlSchemaValidCtxtPtr vctxt, xmlSchemaPSVIIDCKeyPtr key)
{
	/*
	 * Add to gobal list.
	 */
	if(vctxt->idcKeys == NULL) {
		vctxt->idcKeys = (xmlSchemaPSVIIDCKeyPtr*)SAlloc::M(40 * sizeof(xmlSchemaPSVIIDCKeyPtr));
		if(vctxt->idcKeys == NULL) {
			xmlSchemaVErrMemory(vctxt, "allocating the IDC key storage list", 0);
			return -1;
		}
		vctxt->sizeIdcKeys = 40;
	}
	else if(vctxt->sizeIdcKeys <= vctxt->nbIdcKeys) {
		vctxt->sizeIdcKeys *= 2;
		vctxt->idcKeys = (xmlSchemaPSVIIDCKeyPtr*)SAlloc::R(vctxt->idcKeys, vctxt->sizeIdcKeys * sizeof(xmlSchemaPSVIIDCKeyPtr));
		if(vctxt->idcKeys == NULL) {
			xmlSchemaVErrMemory(vctxt, "re-allocating the IDC key storage list", 0);
			return -1;
		}
	}
	vctxt->idcKeys[vctxt->nbIdcKeys++] = key;
	return 0;
}

/**
 * xmlSchemaIDCAppendNodeTableItem:
 * @bind: the IDC binding
 * @ntItem: the node-table item
 *
 * Appends the IDC node-table item to the binding.
 *
 * Returns 0 on success and -1 on internal errors.
 */
static int xmlSchemaIDCAppendNodeTableItem(xmlSchemaPSVIIDCBindingPtr bind, xmlSchemaPSVIIDCNodePtr ntItem)
{
	if(bind->nodeTable == NULL) {
		bind->sizeNodes = 10;
		bind->nodeTable = (xmlSchemaPSVIIDCNodePtr*)SAlloc::M(10 * sizeof(xmlSchemaPSVIIDCNodePtr));
		if(bind->nodeTable == NULL) {
			xmlSchemaVErrMemory(NULL, "allocating an array of IDC node-table items", 0);
			return -1;
		}
	}
	else if(bind->sizeNodes <= bind->nbNodes) {
		bind->sizeNodes *= 2;
		bind->nodeTable = (xmlSchemaPSVIIDCNodePtr*)SAlloc::R(bind->nodeTable, bind->sizeNodes * sizeof(xmlSchemaPSVIIDCNodePtr));
		if(bind->nodeTable == NULL) {
			xmlSchemaVErrMemory(NULL, "re-allocating an array of IDC node-table items", 0);
			return -1;
		}
	}
	bind->nodeTable[bind->nbNodes++] = ntItem;
	return 0;
}

/**
 * xmlSchemaIDCAcquireBinding:
 * @vctxt: the WXS validation context
 * @matcher: the IDC matcher
 *
 * Looks up an PSVI IDC binding, for the IDC definition and
 * of the given matcher. If none found, a new one is created
 * and added to the IDC table.
 *
 * Returns an IDC binding or NULL on internal errors.
 */
static xmlSchemaPSVIIDCBindingPtr xmlSchemaIDCAcquireBinding(xmlSchemaValidCtxtPtr vctxt, xmlSchemaIDCMatcherPtr matcher)
{
	xmlSchemaNodeInfo * ielem = vctxt->elemInfos[matcher->depth];
	if(ielem->idcTable == NULL) {
		ielem->idcTable = xmlSchemaIDCNewBinding(matcher->aidc->def);
		return ielem->idcTable;
	}
	else {
		xmlSchemaPSVIIDCBinding * bind = ielem->idcTable;
		do {
			if(bind->definition == matcher->aidc->def)
				return bind;
			else if(bind->next == NULL) {
				bind->next = xmlSchemaIDCNewBinding(matcher->aidc->def);
				return bind->next;
			}
			else 
				bind = bind->next;
		} while(bind);
	}
	return 0;
}

static xmlSchemaItemListPtr xmlSchemaIDCAcquireTargetList(xmlSchemaValidCtxtPtr vctxt ATTRIBUTE_UNUSED, xmlSchemaIDCMatcherPtr matcher)
{
	SETIFZ(matcher->targets, xmlSchemaItemListCreate());
	return matcher->targets;
}

/**
 * xmlSchemaIDCFreeKey:
 * @key: the IDC key
 *
 * Frees an IDC key together with its compiled value.
 */
static void xmlSchemaIDCFreeKey(xmlSchemaPSVIIDCKeyPtr key)
{
	if(key) {
		xmlSchemaFreeValue(key->val);
		SAlloc::F(key);
	}
}
/**
 * xmlSchemaIDCFreeBinding:
 *
 * Frees an IDC binding. Note that the node table-items
 * are not freed.
 */
static void xmlSchemaIDCFreeBinding(xmlSchemaPSVIIDCBindingPtr bind)
{
	if(bind) {
		SAlloc::F(bind->nodeTable);
		xmlSchemaItemListFree(bind->dupls);
		SAlloc::F(bind);
	}
}

/**
 * xmlSchemaIDCFreeIDCTable:
 * @bind: the first IDC binding in the list
 *
 * Frees an IDC table, i.e. all the IDC bindings in the list.
 */
static void xmlSchemaIDCFreeIDCTable(xmlSchemaPSVIIDCBindingPtr bind)
{
	while(bind) {
		xmlSchemaPSVIIDCBinding * prev = bind;
		bind = bind->next;
		xmlSchemaIDCFreeBinding(prev);
	}
}
/**
 * xmlSchemaIDCFreeMatcherList:
 * @matcher: the first IDC matcher in the list
 *
 * Frees a list of IDC matchers.
 */
static void xmlSchemaIDCFreeMatcherList(xmlSchemaIDCMatcherPtr matcher)
{
	while(matcher) {
		xmlSchemaIDCMatcher * next = matcher->next;
		if(matcher->keySeqs) {
			int i;
			for(i = 0; i < matcher->sizeKeySeqs; i++)
				SAlloc::F(matcher->keySeqs[i]);
			SAlloc::F(matcher->keySeqs);
		}
		if(matcher->targets) {
			if(matcher->idcType == XML_SCHEMA_TYPE_IDC_KEYREF) {
				int i;
				xmlSchemaPSVIIDCNodePtr idcNode;
				/*
				 * Node-table items for keyrefs are not stored globally
				 * to the validation context, since they are not bubbled.
				 * We need to free them here.
				 */
				for(i = 0; i < matcher->targets->nbItems; i++) {
					idcNode = (xmlSchemaPSVIIDCNodePtr)matcher->targets->items[i];
					SAlloc::F(idcNode->keys);
					SAlloc::F(idcNode);
				}
			}
			xmlSchemaItemListFree(matcher->targets);
		}
		SAlloc::F(matcher);
		matcher = next;
	}
}

/**
 * xmlSchemaIDCReleaseMatcherList:
 * @vctxt: the WXS validation context
 * @matcher: the first IDC matcher in the list
 *
 * Caches a list of IDC matchers for reuse.
 */
static void xmlSchemaIDCReleaseMatcherList(xmlSchemaValidCtxtPtr vctxt, xmlSchemaIDCMatcherPtr matcher)
{
	while(matcher) {
		xmlSchemaIDCMatcher * next = matcher->next;
		if(matcher->keySeqs) {
			int i;
			/*
			 * Don't free the array, but only the content.
			 */
			for(i = 0; i < matcher->sizeKeySeqs; i++) {
				SAlloc::F(matcher->keySeqs[i]);
				matcher->keySeqs[i] = NULL;
			}
		}
		if(matcher->targets) {
			if(matcher->idcType == XML_SCHEMA_TYPE_IDC_KEYREF) {
				int i;
				xmlSchemaPSVIIDCNodePtr idcNode;
				/*
				 * Node-table items for keyrefs are not stored globally
				 * to the validation context, since they are not bubbled.
				 * We need to free them here.
				 */
				for(i = 0; i < matcher->targets->nbItems; i++) {
					idcNode = (xmlSchemaPSVIIDCNodePtr)matcher->targets->items[i];
					SAlloc::F(idcNode->keys);
					SAlloc::F(idcNode);
				}
			}
			xmlSchemaItemListFree(matcher->targets);
			matcher->targets = NULL;
		}
		matcher->next = NULL;
		/*
		 * Cache the matcher.
		 */
		if(vctxt->idcMatcherCache)
			matcher->nextCached = vctxt->idcMatcherCache;
		vctxt->idcMatcherCache = matcher;
		matcher = next;
	}
}

/**
 * xmlSchemaIDCAddStateObject:
 * @vctxt: the WXS validation context
 * @matcher: the IDC matcher
 * @sel: the XPath information
 * @parent: the parent "selector" state object if any
 * @type: "selector" or "field"
 *
 * Creates/reuses and activates state objects for the given
 * XPath information; if the XPath expression consists of unions,
 * multiple state objects are created for every unioned expression.
 *
 * Returns 0 on success and -1 on internal errors.
 */
static int xmlSchemaIDCAddStateObject(xmlSchemaValidCtxtPtr vctxt, xmlSchemaIDCMatcherPtr matcher, xmlSchemaIDCSelectPtr sel, int type)
{
	xmlSchemaIDCStateObjPtr sto;
	/*
	 * Reuse the state objects from the pool.
	 */
	if(vctxt->xpathStatePool) {
		sto = vctxt->xpathStatePool;
		vctxt->xpathStatePool = sto->next;
		sto->next = NULL;
	}
	else {
		/*
		 * Create a new state object.
		 */
		sto = (xmlSchemaIDCStateObjPtr)SAlloc::M(sizeof(xmlSchemaIDCStateObj));
		if(sto == NULL) {
			xmlSchemaVErrMemory(NULL, "allocating an IDC state object", 0);
			return -1;
		}
		memzero(sto, sizeof(xmlSchemaIDCStateObj));
	}
	/*
	 * Add to global list.
	 */
	if(vctxt->xpathStates)
		sto->next = vctxt->xpathStates;
	vctxt->xpathStates = sto;
	/*
	 * Free the old xpath validation context.
	 */
	xmlFreeStreamCtxt((xmlStreamCtxtPtr)sto->xpathCtxt);
	/*
	 * Create a new XPath (pattern) validation context.
	 */
	sto->xpathCtxt = (void*)xmlPatternGetStreamCtxt((xmlPatternPtr)sel->xpathComp);
	if(sto->xpathCtxt == NULL) {
		VERROR_INT("xmlSchemaIDCAddStateObject", "failed to create an XPath validation context");
		return -1;
	}
	sto->type = type;
	sto->depth = vctxt->depth;
	sto->matcher = matcher;
	sto->sel = sel;
	sto->nbHistory = 0;
#ifdef DEBUG_IDC
	xmlGenericError(0, "IDC:   STO push '%s'\n", sto->sel->xpath);
#endif
	return 0;
}

/**
 * xmlSchemaXPathEvaluate:
 * @vctxt: the WXS validation context
 * @nodeType: the nodeType of the current node
 *
 * Evaluates all active XPath state objects.
 *
 * Returns the number of IC "field" state objects which resolved to
 * this node, 0 if none resolved and -1 on internal errors.
 */
static int xmlSchemaXPathEvaluate(xmlSchemaValidCtxtPtr vctxt, xmlElementType nodeType)
{
	xmlSchemaIDCStateObjPtr sto, head = NULL, first;
	int res, resolved = 0, depth = vctxt->depth;
	if(vctxt->xpathStates == NULL)
		return 0;
	if(nodeType == XML_ATTRIBUTE_NODE)
		depth++;
#ifdef DEBUG_IDC
	{
		xmlChar * str = NULL;
		xmlGenericError(0, "IDC: EVAL on %s, depth %d, type %d\n",
			xmlSchemaFormatQName(&str, vctxt->inode->nsName, vctxt->inode->localName), depth, nodeType);
		ZFREE(str);
	}
#endif
	/*
	 * Process all active XPath state objects.
	 */
	first = vctxt->xpathStates;
	sto = first;
	while(sto != head) {
#ifdef DEBUG_IDC
		if(sto->type == XPATH_STATE_OBJ_TYPE_IDC_SELECTOR)
			xmlGenericError(0, "IDC:   ['%s'] selector '%s'\n", sto->matcher->aidc->def->name, sto->sel->xpath);
		else
			xmlGenericError(0, "IDC:   ['%s'] field '%s'\n", sto->matcher->aidc->def->name, sto->sel->xpath);
#endif
		if(nodeType == XML_ELEMENT_NODE)
			res = xmlStreamPush((xmlStreamCtxtPtr)sto->xpathCtxt, vctxt->inode->localName, vctxt->inode->nsName);
		else
			res = xmlStreamPushAttr((xmlStreamCtxtPtr)sto->xpathCtxt, vctxt->inode->localName, vctxt->inode->nsName);
		if(res == -1) {
			VERROR_INT("xmlSchemaXPathEvaluate", "calling xmlStreamPush()");
			return -1;
		}
		if(res == 0)
			goto next_sto;
		/*
		 * Full match.
		 */
#ifdef DEBUG_IDC
		xmlGenericError(0, "IDC:     MATCH\n");
#endif
		/*
		 * Register a match in the state object history.
		 */
		if(sto->history == NULL) {
			sto->history = (int*)SAlloc::M(5 * sizeof(int));
			if(sto->history == NULL) {
				xmlSchemaVErrMemory(NULL, "allocating the state object history", 0);
				return -1;
			}
			sto->sizeHistory = 5;
		}
		else if(sto->sizeHistory <= sto->nbHistory) {
			sto->sizeHistory *= 2;
			sto->history = (int*)SAlloc::R(sto->history,
			    sto->sizeHistory * sizeof(int));
			if(sto->history == NULL) {
				xmlSchemaVErrMemory(NULL, "re-allocating the state object history", 0);
				return -1;
			}
		}
		sto->history[sto->nbHistory++] = depth;

#ifdef DEBUG_IDC
		xmlGenericError(0, "IDC:       push match '%d'\n", vctxt->depth);
#endif

		if(sto->type == XPATH_STATE_OBJ_TYPE_IDC_SELECTOR) {
			xmlSchemaIDCSelectPtr sel;
			/*
			 * Activate state objects for the IDC fields of
			 * the IDC selector.
			 */
#ifdef DEBUG_IDC
			xmlGenericError(0, "IDC:     activating field states\n");
#endif
			sel = sto->matcher->aidc->def->fields;
			while(sel) {
				if(xmlSchemaIDCAddStateObject(vctxt, sto->matcher, sel, XPATH_STATE_OBJ_TYPE_IDC_FIELD) == -1)
					return -1;
				sel = sel->next;
			}
		}
		else if(sto->type == XPATH_STATE_OBJ_TYPE_IDC_FIELD) {
			/*
			 * An IDC key node was found by the IDC field.
			 */
#ifdef DEBUG_IDC
			xmlGenericError(0, "IDC:     key found\n");
#endif
			/*
			 * Notify that the character value of this node is
			 * needed.
			 */
			if(resolved == 0) {
				if((vctxt->inode->flags &
					    XML_SCHEMA_NODE_INFO_VALUE_NEEDED) == 0)
					vctxt->inode->flags |= XML_SCHEMA_NODE_INFO_VALUE_NEEDED;
			}
			resolved++;
		}
next_sto:
		if(sto->next == NULL) {
			/*
			 * Evaluate field state objects created on this node as well.
			 */
			head = first;
			sto = vctxt->xpathStates;
		}
		else
			sto = sto->next;
	}
	return (resolved);
}

static const xmlChar * xmlSchemaFormatIDCKeySequence(xmlSchemaValidCtxtPtr vctxt, xmlChar ** buf, xmlSchemaPSVIIDCKeyPtr * seq, int count)
{
	int i, res;
	xmlChar * value = NULL;
	*buf = sstrdup(BAD_CAST "[");
	for(i = 0; i < count; i++) {
		*buf = xmlStrcat(*buf, BAD_CAST "'");
		res = xmlSchemaGetCanonValueWhtspExt(seq[i]->val, xmlSchemaGetWhiteSpaceFacetValue(seq[i]->type), &value);
		if(res == 0)
			*buf = xmlStrcat(*buf, BAD_CAST value);
		else {
			VERROR_INT("xmlSchemaFormatIDCKeySequence", "failed to compute a canonical value");
			*buf = xmlStrcat(*buf, BAD_CAST "???");
		}
		if(i < count -1)
			*buf = xmlStrcat(*buf, BAD_CAST "', ");
		else
			*buf = xmlStrcat(*buf, BAD_CAST "'");
		SAlloc::F(value);
		value = NULL;
	}
	*buf = xmlStrcat(*buf, BAD_CAST "]");
	return (BAD_CAST *buf);
}

/**
 * xmlSchemaXPathPop:
 * @vctxt: the WXS validation context
 *
 * Pops all XPath states.
 *
 * Returns 0 on success and -1 on internal errors.
 */
static int xmlSchemaXPathPop(xmlSchemaValidCtxtPtr vctxt)
{
	if(vctxt->xpathStates) {
		xmlSchemaIDCStateObjPtr sto = vctxt->xpathStates;
		do {
			int res = xmlStreamPop((xmlStreamCtxtPtr)sto->xpathCtxt);
			if(res == -1)
				return -1;
			sto = sto->next;
		} while(sto);
	}
	return 0;
}

/**
 * xmlSchemaXPathProcessHistory:
 * @vctxt: the WXS validation context
 * @type: the simple/complex type of the current node if any at all
 * @val: the precompiled value
 *
 * Processes and pops the history items of the IDC state objects.
 * IDC key-sequences are validated/created on IDC bindings.
 *
 * Returns 0 on success and -1 on internal errors.
 */
static int xmlSchemaXPathProcessHistory(xmlSchemaValidCtxtPtr vctxt, int depth)
{
	xmlSchemaIDCStateObjPtr sto, nextsto;
	int res, matchDepth;
	xmlSchemaPSVIIDCKeyPtr key = NULL;
	xmlSchemaTypePtr type = vctxt->inode->typeDef, simpleType = NULL;
	if(vctxt->xpathStates == NULL)
		return 0;
	sto = vctxt->xpathStates;
#ifdef DEBUG_IDC
	{
		xmlChar * str = NULL;
		xmlGenericError(0, "IDC: BACK on %s, depth %d\n", xmlSchemaFormatQName(&str, vctxt->inode->nsName,
			    vctxt->inode->localName), vctxt->depth);
		ZFREE(str);
	}
#endif
	/*
	 * Evaluate the state objects.
	 */
	while(sto != NULL) {
		res = xmlStreamPop((xmlStreamCtxtPtr)sto->xpathCtxt);
		if(res == -1) {
			VERROR_INT("xmlSchemaXPathProcessHistory", "calling xmlStreamPop()");
			return -1;
		}
#ifdef DEBUG_IDC
		xmlGenericError(0, "IDC:   stream pop '%s'\n", sto->sel->xpath);
#endif
		if(sto->nbHistory == 0)
			goto deregister_check;
		matchDepth = sto->history[sto->nbHistory -1];

		/*
		 * Only matches at the current depth are of interest.
		 */
		if(matchDepth != depth) {
			sto = sto->next;
			continue;
		}
		if(sto->type == XPATH_STATE_OBJ_TYPE_IDC_FIELD) {
			/*
			 * NOTE: According to
			 *   http://www.w3.org/Bugs/Public/show_bug.cgi?id=2198
			 *   ... the simple-content of complex types is also allowed.
			 */

			if(WXS_IS_COMPLEX(type)) {
				if(WXS_HAS_SIMPLE_CONTENT(type)) {
					/*
					 * Sanity check for complex types with simple content.
					 */
					simpleType = type->contentTypeDef;
					if(simpleType == NULL) {
						VERROR_INT("xmlSchemaXPathProcessHistory", "field resolves to a CT with simple content but the CT is missing the ST definition");
						return -1;
					}
				}
				else
					simpleType = NULL;
			}
			else
				simpleType = type;
			if(simpleType == NULL) {
				xmlChar * str = NULL;

				/*
				 * Not qualified if the field resolves to a node of non
				 * simple type.
				 */
				xmlSchemaCustomErr(ACTXT_CAST vctxt,
				    XML_SCHEMAV_CVC_IDC, NULL,
				    WXS_BASIC_CAST sto->matcher->aidc->def,
				    "The XPath '%s' of a field of %s does evaluate to a node of "
				    "non-simple type",
				    sto->sel->xpath,
				    xmlSchemaGetIDCDesignation(&str, sto->matcher->aidc->def));
				ZFREE(str);
				sto->nbHistory--;
				goto deregister_check;
			}
			if((key == NULL) && (vctxt->inode->val == NULL)) {
				/*
				 * Failed to provide the normalized value; maybe
				 * the value was invalid.
				 */
				VERROR(XML_SCHEMAV_CVC_IDC, WXS_BASIC_CAST sto->matcher->aidc->def,
				    "Warning: No precomputed value available, the value was either invalid or something strange happend");
				sto->nbHistory--;
				goto deregister_check;
			}
			else {
				xmlSchemaIDCMatcherPtr matcher = sto->matcher;
				xmlSchemaPSVIIDCKeyPtr * keySeq;
				int pos, idx;
				/*
				 * The key will be anchored on the matcher's list of
				 * key-sequences. The position in this list is determined
				 * by the target node's depth relative to the matcher's
				 * depth of creation (i.e. the depth of the scope element).
				 *
				 * Element        Depth    Pos   List-entries
				 * <scope>          0              NULL
				 *   <bar>          1              NULL
				 *     <target/>    2       2      target
				 *   <bar>
				 * </scope>
				 *
				 * The size of the list is only dependant on the depth of
				 * the tree.
				 * An entry will be NULLed in selector_leave, i.e. when
				 * we hit the target's
				 */
				pos = sto->depth - matcher->depth;
				idx = sto->sel->index;
				/*
				 * Create/grow the array of key-sequences.
				 */
				if(matcher->keySeqs == NULL) {
					matcher->sizeKeySeqs = (pos > 9) ? (pos * 2) : 10;
					matcher->keySeqs = (xmlSchemaPSVIIDCKeyPtr**)SAlloc::M(matcher->sizeKeySeqs * sizeof(xmlSchemaPSVIIDCKeyPtr *));
					if(matcher->keySeqs == NULL) {
						xmlSchemaVErrMemory(NULL, "allocating an array of key-sequences", 0);
						return -1;
					}
					memzero(matcher->keySeqs, matcher->sizeKeySeqs * sizeof(xmlSchemaPSVIIDCKeyPtr *));
				}
				else if(pos >= matcher->sizeKeySeqs) {
					int i = matcher->sizeKeySeqs;
					matcher->sizeKeySeqs *= 2;
					matcher->keySeqs = (xmlSchemaPSVIIDCKeyPtr**)SAlloc::R(matcher->keySeqs, matcher->sizeKeySeqs * sizeof(xmlSchemaPSVIIDCKeyPtr *));
					if(matcher->keySeqs == NULL) {
						xmlSchemaVErrMemory(NULL, "reallocating an array of key-sequences", 0);
						return -1;
					}
					/*
					 * The array needs to be NULLed.
					 * @todo Use memset?
					 */
					for(; i < matcher->sizeKeySeqs; i++)
						matcher->keySeqs[i] = NULL;
				}
				/*
				 * Get/create the key-sequence.
				 */
				keySeq = matcher->keySeqs[pos];
				if(keySeq == NULL) {
					goto create_sequence;
				}
				else if(keySeq[idx] != NULL) {
					xmlChar * str = NULL;
					/*
					 * cvc-identity-constraint:
					 * 3 For each node in the `target node set` all
					 * of the {fields}, with that node as the context
					 * node, evaluate to either an empty node-set or
					 * a node-set with exactly one member, which must
					 * have a simple type.
					 *
					 * The key was already set; report an error.
					 */
					xmlSchemaCustomErr(ACTXT_CAST vctxt,
					    XML_SCHEMAV_CVC_IDC, NULL,
					    WXS_BASIC_CAST matcher->aidc->def,
					    "The XPath '%s' of a field of %s evaluates to a "
					    "node-set with more than one member",
					    sto->sel->xpath,
					    xmlSchemaGetIDCDesignation(&str, matcher->aidc->def));
					ZFREE(str);
					sto->nbHistory--;
					goto deregister_check;
				}
				else
					goto create_key;

create_sequence:
				/*
				 * Create a key-sequence.
				 */
				keySeq = (xmlSchemaPSVIIDCKeyPtr*)SAlloc::M(matcher->aidc->def->nbFields * sizeof(xmlSchemaPSVIIDCKeyPtr));
				if(keySeq == NULL) {
					xmlSchemaVErrMemory(NULL, "allocating an IDC key-sequence", 0);
					return -1;
				}
				memzero(keySeq, matcher->aidc->def->nbFields * sizeof(xmlSchemaPSVIIDCKeyPtr));
				matcher->keySeqs[pos] = keySeq;
create_key:
				/*
				 * Create a key once per node only.
				 */
				if(key == NULL) {
					key = (xmlSchemaPSVIIDCKeyPtr)SAlloc::M(sizeof(xmlSchemaPSVIIDCKey));
					if(key == NULL) {
						xmlSchemaVErrMemory(NULL, "allocating a IDC key", 0);
						SAlloc::F(keySeq);
						matcher->keySeqs[pos] = NULL;
						return -1;
					}
					/*
					 * Consume the compiled value.
					 */
					key->type = simpleType;
					key->val = vctxt->inode->val;
					vctxt->inode->val = NULL;
					/*
					 * Store the key in a global list.
					 */
					if(xmlSchemaIDCStoreKey(vctxt, key) == -1) {
						xmlSchemaIDCFreeKey(key);
						return -1;
					}
				}
				keySeq[idx] = key;
			}
		}
		else if(sto->type == XPATH_STATE_OBJ_TYPE_IDC_SELECTOR) {
			xmlSchemaPSVIIDCKeyPtr ** keySeq = NULL;
			/* xmlSchemaPSVIIDCBindingPtr bind; */
			xmlSchemaPSVIIDCNodePtr ntItem;
			xmlSchemaIDCMatcherPtr matcher;
			xmlSchemaIDCPtr idc;
			xmlSchemaItemListPtr targets;
			int pos, i, j, nbKeys;
			/*
			 * Here we have the following scenario:
			 * An IDC 'selector' state object resolved to a target node,
			 * during the time this target node was in the
			 * ancestor-or-self axis, the 'field' state object(s) looked
			 * out for matching nodes to create a key-sequence for this
			 * target node. Now we are back to this target node and need
			 * to put the key-sequence, together with the target node
			 * itself, into the node-table of the corresponding IDC
			 * binding.
			 */
			matcher = sto->matcher;
			idc = matcher->aidc->def;
			nbKeys = idc->nbFields;
			pos = depth - matcher->depth;
			/*
			 * Check if the matcher has any key-sequences at all, plus
			 * if it has a key-sequence for the current target node.
			 */
			if((matcher->keySeqs == NULL) ||
			    (matcher->sizeKeySeqs <= pos)) {
				if(idc->type == XML_SCHEMA_TYPE_IDC_KEY)
					goto selector_key_error;
				else
					goto selector_leave;
			}

			keySeq = &(matcher->keySeqs[pos]);
			if(*keySeq == NULL) {
				if(idc->type == XML_SCHEMA_TYPE_IDC_KEY)
					goto selector_key_error;
				else
					goto selector_leave;
			}

			for(i = 0; i < nbKeys; i++) {
				if((*keySeq)[i] == NULL) {
					/*
					 * Not qualified, if not all fields did resolve.
					 */
					if(idc->type == XML_SCHEMA_TYPE_IDC_KEY) {
						/*
						 * All fields of a "key" IDC must resolve.
						 */
						goto selector_key_error;
					}
					goto selector_leave;
				}
			}
			/*
			 * All fields did resolve.
			 */

			/*
			 * 4.1 If the {identity-constraint category} is unique(/key),
			 * then no two members of the `qualified node set` have
			 * `key-sequences` whose members are pairwise equal, as
			 * defined by Equal in [XML Schemas: Datatypes].
			 *
			 * Get the IDC binding from the matcher and check for
			 * duplicate key-sequences.
			 */
#if 0
			bind = xmlSchemaIDCAcquireBinding(vctxt, matcher);
#endif
			targets = xmlSchemaIDCAcquireTargetList(vctxt, matcher);
			if((idc->type != XML_SCHEMA_TYPE_IDC_KEYREF) &&
			    (targets->nbItems != 0)) {
				xmlSchemaPSVIIDCKeyPtr ckey, bkey, * bkeySeq;

				i = 0;
				res = 0;
				/*
				 * Compare the key-sequences, key by key.
				 */
				do {
					bkeySeq =
					    ((xmlSchemaPSVIIDCNodePtr)targets->items[i])->keys;
					for(j = 0; j < nbKeys; j++) {
						ckey = (*keySeq)[j];
						bkey = bkeySeq[j];
						res = xmlSchemaAreValuesEqual(ckey->val, bkey->val);
						if(res == -1) {
							return -1;
						}
						else if(res == 0) {
							/*
							 * One of the keys differs, so the key-sequence
							 * won't be equal; get out.
							 */
							break;
						}
					}
					if(res == 1) {
						/*
						 * Duplicate key-sequence found.
						 */
						break;
					}
					i++;
				} while(i < targets->nbItems);
				if(i != targets->nbItems) {
					xmlChar * str = NULL, * strB = NULL;
					/*
					 * @todo Try to report the key-sequence.
					 */
					xmlSchemaCustomErr(ACTXT_CAST vctxt,
					    XML_SCHEMAV_CVC_IDC, NULL,
					    WXS_BASIC_CAST idc,
					    "Duplicate key-sequence %s in %s",
					    xmlSchemaFormatIDCKeySequence(vctxt, &str,
						    (*keySeq), nbKeys),
					    xmlSchemaGetIDCDesignation(&strB, idc));
					ZFREE(str);
					ZFREE(strB);
					goto selector_leave;
				}
			}
			/*
			 * Add a node-table item to the IDC binding.
			 */
			ntItem = (xmlSchemaPSVIIDCNodePtr)SAlloc::M(sizeof(xmlSchemaPSVIIDCNode));
			if(ntItem == NULL) {
				xmlSchemaVErrMemory(NULL, "allocating an IDC node-table item", 0);
				SAlloc::F(*keySeq);
				*keySeq = NULL;
				return -1;
			}
			memzero(ntItem, sizeof(xmlSchemaPSVIIDCNode));
			/*
			 * Store the node-table item in a global list.
			 */
			if(idc->type != XML_SCHEMA_TYPE_IDC_KEYREF) {
				if(xmlSchemaIDCStoreNodeTableItem(vctxt, ntItem) == -1) {
					SAlloc::F(ntItem);
					SAlloc::F(*keySeq);
					*keySeq = NULL;
					return -1;
				}
				ntItem->nodeQNameID = -1;
			}
			else {
				/*
				 * Save a cached QName for this node on the IDC node, to be
				 * able to report it, even if the node is not saved.
				 */
				ntItem->nodeQNameID = xmlSchemaVAddNodeQName(vctxt,
				    vctxt->inode->localName, vctxt->inode->nsName);
				if(ntItem->nodeQNameID == -1) {
					SAlloc::F(ntItem);
					SAlloc::F(*keySeq);
					*keySeq = NULL;
					return -1;
				}
			}
			/*
			 * Init the node-table item: Save the node, position and
			 * consume the key-sequence.
			 */
			ntItem->P_Node = vctxt->P_Node;
			ntItem->nodeLine = vctxt->inode->nodeLine;
			ntItem->keys = *keySeq;
			*keySeq = NULL;
#if 0
			if(xmlSchemaIDCAppendNodeTableItem(bind, ntItem) == -1)
#endif
			if(xmlSchemaItemListAdd(targets, ntItem) == -1) {
				if(idc->type == XML_SCHEMA_TYPE_IDC_KEYREF) {
					/*
					 * Free the item, since keyref items won't be
					 * put on a global list.
					 */
					SAlloc::F(ntItem->keys);
					SAlloc::F(ntItem);
				}
				return -1;
			}

			goto selector_leave;
selector_key_error:
			{
				xmlChar * str = NULL;
				/*
				 * 4.2.1 (KEY) The `target node set` and the
				 * `qualified node set` are equal, that is, every
				 * member of the `target node set` is also a member
				 * of the `qualified node set` and vice versa.
				 */
				xmlSchemaCustomErr(ACTXT_CAST vctxt, XML_SCHEMAV_CVC_IDC, NULL,
				    WXS_BASIC_CAST idc, "Not all fields of %s evaluate to a node", xmlSchemaGetIDCDesignation(&str, idc), 0);
				ZFREE(str);
			}
selector_leave:
			/*
			 * Free the key-sequence if not added to the IDC table.
			 */
			if(keySeq && *keySeq) {
				SAlloc::F(*keySeq);
				*keySeq = NULL;
			}
		} /* if selector */

		sto->nbHistory--;

deregister_check:
		/*
		 * Deregister state objects if they reach the depth of creation.
		 */
		if((sto->nbHistory == 0) && (sto->depth == depth)) {
#ifdef DEBUG_IDC
			xmlGenericError(0, "IDC:   STO pop '%s'\n", sto->sel->xpath);
#endif
			if(vctxt->xpathStates != sto) {
				VERROR_INT("xmlSchemaXPathProcessHistory", "The state object to be removed is not the first in the list");
			}
			nextsto = sto->next;
			/*
			 * Unlink from the list of active XPath state objects.
			 */
			vctxt->xpathStates = sto->next;
			sto->next = vctxt->xpathStatePool;
			/*
			 * Link it to the pool of reusable state objects.
			 */
			vctxt->xpathStatePool = sto;
			sto = nextsto;
		}
		else
			sto = sto->next;
	} /* while (sto != NULL) */
	return 0;
}

/**
 * xmlSchemaIDCRegisterMatchers:
 * @vctxt: the WXS validation context
 * @elemDecl: the element declaration
 *
 * Creates helper objects to evaluate IDC selectors/fields
 * successively.
 *
 * Returns 0 if OK and -1 on internal errors.
 */
static int xmlSchemaIDCRegisterMatchers(xmlSchemaValidCtxtPtr vctxt, xmlSchemaElementPtr elemDecl)
{
	xmlSchemaIDCMatcherPtr matcher, last = NULL;
	xmlSchemaIDCPtr refIdc;
	xmlSchemaIDCAugPtr aidc;
	xmlSchemaIDCPtr idc = (xmlSchemaIDCPtr)elemDecl->idcs;
	if(idc == NULL)
		return 0;
#ifdef DEBUG_IDC
	{
		xmlChar * str = NULL;
		xmlGenericError(0, "IDC: REGISTER on %s, depth %d\n",
			(char*)xmlSchemaFormatQName(&str, vctxt->inode->nsName, vctxt->inode->localName), vctxt->depth);
		ZFREE(str);
	}
#endif
	if(vctxt->inode->idcMatchers != NULL) {
		VERROR_INT("xmlSchemaIDCRegisterMatchers", "The chain of IDC matchers is expected to be empty");
		return -1;
	}
	do {
		if(idc->type == XML_SCHEMA_TYPE_IDC_KEYREF) {
			/*
			 * Since IDCs bubbles are expensive we need to know the
			 * depth at which the bubbles should stop; this will be
			 * the depth of the top-most keyref IDC. If no keyref
			 * references a key/unique IDC, the keyrefDepth will
			 * be -1, indicating that no bubbles are needed.
			 */
			refIdc = (xmlSchemaIDCPtr)idc->ref->item;
			if(refIdc != NULL) {
				/*
				 * Remember that we have keyrefs on this node.
				 */
				vctxt->inode->hasKeyrefs = 1;
				/*
				 * Lookup the referenced augmented IDC info.
				 */
				aidc = vctxt->aidcs;
				while(aidc != NULL) {
					if(aidc->def == refIdc)
						break;
					aidc = aidc->next;
				}
				if(aidc == NULL) {
					VERROR_INT("xmlSchemaIDCRegisterMatchers", "Could not find an augmented IDC item for an IDC definition");
					return -1;
				}
				if((aidc->keyrefDepth == -1) ||
				    (vctxt->depth < aidc->keyrefDepth))
					aidc->keyrefDepth = vctxt->depth;
			}
		}
		/*
		 * Lookup the augmented IDC item for the IDC definition.
		 */
		aidc = vctxt->aidcs;
		while(aidc != NULL) {
			if(aidc->def == idc)
				break;
			aidc = aidc->next;
		}
		if(aidc == NULL) {
			VERROR_INT("xmlSchemaIDCRegisterMatchers", "Could not find an augmented IDC item for an IDC definition");
			return -1;
		}
		/*
		 * Create an IDC matcher for every IDC definition.
		 */
		if(vctxt->idcMatcherCache != NULL) {
			/*
			 * Reuse a cached matcher.
			 */
			matcher = vctxt->idcMatcherCache;
			vctxt->idcMatcherCache = matcher->nextCached;
			matcher->nextCached = NULL;
		}
		else {
			matcher = (xmlSchemaIDCMatcherPtr)SAlloc::M(sizeof(xmlSchemaIDCMatcher));
			if(matcher == NULL) {
				xmlSchemaVErrMemory(vctxt, "allocating an IDC matcher", 0);
				return -1;
			}
			memzero(matcher, sizeof(xmlSchemaIDCMatcher));
		}
		if(last == NULL)
			vctxt->inode->idcMatchers = matcher;
		else
			last->next = matcher;
		last = matcher;
		matcher->type = IDC_MATCHER;
		matcher->depth = vctxt->depth;
		matcher->aidc = aidc;
		matcher->idcType = aidc->def->type;
#ifdef DEBUG_IDC
		xmlGenericError(0, "IDC:   register matcher\n");
#endif
		/*
		 * Init the automaton state object.
		 */
		if(xmlSchemaIDCAddStateObject(vctxt, matcher, idc->selector, XPATH_STATE_OBJ_TYPE_IDC_SELECTOR) == -1)
			return -1;
		idc = idc->next;
	} while(idc != NULL);
	return 0;
}

static int xmlSchemaIDCFillNodeTables(xmlSchemaValidCtxtPtr vctxt, xmlSchemaNodeInfoPtr ielem)
{
	xmlSchemaPSVIIDCBindingPtr bind;
	int res, i, j, k, nbTargets, nbFields, nbDupls, nbNodeTable;
	xmlSchemaPSVIIDCKeyPtr * keys, * ntkeys;
	xmlSchemaPSVIIDCNodePtr * targets, * dupls;
	xmlSchemaIDCMatcherPtr matcher = ielem->idcMatchers;
	/* vctxt->createIDCNodeTables */
	while(matcher != NULL) {
		/*
		 * Skip keyref IDCs and empty IDC target-lists.
		 */
		if((matcher->aidc->def->type == XML_SCHEMA_TYPE_IDC_KEYREF) ||
		    WXS_ILIST_IS_EMPTY(matcher->targets)) {
			matcher = matcher->next;
			continue;
		}
		/*
		 * If we _want_ the IDC node-table to be created in any case
		 * then do so. Otherwise create them only if keyrefs need them.
		 */
		if((!vctxt->createIDCNodeTables) && ((matcher->aidc->keyrefDepth == -1) || (matcher->aidc->keyrefDepth > vctxt->depth))) {
			matcher = matcher->next;
			continue;
		}
		/*
		 * Get/create the IDC binding on this element for the IDC definition.
		 */
		bind = xmlSchemaIDCAcquireBinding(vctxt, matcher);
		if(!WXS_ILIST_IS_EMPTY(bind->dupls)) {
			dupls = (xmlSchemaPSVIIDCNodePtr*)bind->dupls->items;
			nbDupls = bind->dupls->nbItems;
		}
		else {
			dupls = NULL;
			nbDupls = 0;
		}
		nbNodeTable = (bind->nodeTable != NULL) ? bind->nbNodes : 0;
		if((nbNodeTable == 0) && (nbDupls == 0)) {
			/*
			 * Transfer all IDC target-nodes to the IDC node-table.
			 */
			bind->nodeTable = (xmlSchemaPSVIIDCNodePtr*)matcher->targets->items;
			bind->sizeNodes = matcher->targets->sizeItems;
			bind->nbNodes = matcher->targets->nbItems;
			matcher->targets->items = NULL;
			matcher->targets->sizeItems = 0;
			matcher->targets->nbItems = 0;
		}
		else {
			/*
			 * Compare the key-sequences and add to the IDC node-table.
			 */
			nbTargets = matcher->targets->nbItems;
			targets = (xmlSchemaPSVIIDCNodePtr*)matcher->targets->items;
			nbFields = matcher->aidc->def->nbFields;
			i = 0;
			do {
				keys = targets[i]->keys;
				if(nbDupls) {
					/*
					 * Search in already found duplicates first.
					 */
					j = 0;
					do {
						if(nbFields == 1) {
							res = xmlSchemaAreValuesEqual(keys[0]->val, dupls[j]->keys[0]->val);
							if(res == -1)
								goto internal_error;
							if(res == 1) {
								/*
								 * Equal key-sequence.
								 */
								goto next_target;
							}
						}
						else {
							res = 0;
							ntkeys = dupls[j]->keys;
							for(k = 0; k < nbFields; k++) {
								res = xmlSchemaAreValuesEqual(keys[k]->val, ntkeys[k]->val);
								if(res == -1)
									goto internal_error;
								if(res == 0) {
									/*
									 * One of the keys differs.
									 */
									break;
								}
							}
							if(res == 1) {
								/*
								 * Equal key-sequence found.
								 */
								goto next_target;
							}
						}
						j++;
					} while(j < nbDupls);
				}
				if(nbNodeTable) {
					j = 0;
					do {
						if(nbFields == 1) {
							res = xmlSchemaAreValuesEqual(keys[0]->val, bind->nodeTable[j]->keys[0]->val);
							if(res == -1)
								goto internal_error;
							if(res == 0) {
								/*
								 * The key-sequence differs.
								 */
								goto next_node_table_entry;
							}
						}
						else {
							res = 0;
							ntkeys = bind->nodeTable[j]->keys;
							for(k = 0; k < nbFields; k++) {
								res = xmlSchemaAreValuesEqual(keys[k]->val, ntkeys[k]->val);
								if(res == -1)
									goto internal_error;
								if(res == 0) {
									/*
									 * One of the keys differs.
									 */
									goto next_node_table_entry;
								}
							}
						}
						/*
						 * Add the duplicate to the list of duplicates.
						 */
						if(bind->dupls == NULL) {
							bind->dupls = xmlSchemaItemListCreate();
							if(bind->dupls == NULL)
								goto internal_error;
						}
						if(xmlSchemaItemListAdd(bind->dupls, bind->nodeTable[j]) == -1)
							goto internal_error;
						/*
						 * Remove the duplicate entry from the IDC node-table.
						 */
						bind->nodeTable[j] = bind->nodeTable[bind->nbNodes -1];
						bind->nbNodes--;

						goto next_target;

next_node_table_entry:
						j++;
					} while(j < nbNodeTable);
				}
				/*
				 * If everything is fine, then add the IDC target-node to
				 * the IDC node-table.
				 */
				if(xmlSchemaIDCAppendNodeTableItem(bind, targets[i]) == -1)
					goto internal_error;

next_target:
				i++;
			} while(i < nbTargets);
		}
		matcher = matcher->next;
	}
	return 0;
internal_error:
	return -1;
}

/**
 * xmlSchemaBubbleIDCNodeTables:
 * @depth: the current tree depth
 *
 * Merges IDC bindings of an element at @depth into the corresponding IDC
 * bindings of its parent element. If a duplicate note-table entry is found,
 * both, the parent node-table entry and child entry are discarded from the
 * node-table of the parent.
 *
 * Returns 0 if OK and -1 on internal errors.
 */
static int xmlSchemaBubbleIDCNodeTables(xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaPSVIIDCBindingPtr * parTable, parBind = NULL; /* parent IDC bindings. */
	xmlSchemaPSVIIDCNodePtr P_Node, parNode = NULL, * dupls, * parNodes; /* node-table entries. */
	xmlSchemaIDCAugPtr aidc;
	int i, j, k, ret = 0, nbFields, oldNum, oldDupls;
	xmlSchemaPSVIIDCBindingPtr bind = vctxt->inode->idcTable; /* IDC bindings of the current node. */
	if(bind == NULL) {
		/* Fine, no table, no bubbles. */
		return 0;
	}
	parTable = &(vctxt->elemInfos[vctxt->depth -1]->idcTable);
	/*
	 * Walk all bindings; create new or add to existing bindings.
	 * Remove duplicate key-sequences.
	 */
	while(bind != NULL) {
		if((bind->nbNodes == 0) && WXS_ILIST_IS_EMPTY(bind->dupls))
			goto next_binding;
		/*
		 * Check if the key/unique IDC table needs to be bubbled.
		 */
		if(!vctxt->createIDCNodeTables) {
			aidc = vctxt->aidcs;
			do {
				if(aidc->def == bind->definition) {
					if((aidc->keyrefDepth == -1) || (aidc->keyrefDepth >= vctxt->depth)) {
						goto next_binding;
					}
					break;
				}
				aidc = aidc->next;
			} while(aidc != NULL);
		}
		RVALUEPTR(parBind, parTable);
		/*
		 * Search a matching parent binding for the
		 * IDC definition.
		 */
		while(parBind != NULL) {
			if(parBind->definition == bind->definition)
				break;
			parBind = parBind->next;
		}
		if(parBind != NULL) {
			/*
			 * Compare every node-table entry of the child node,
			 * i.e. the key-sequence within, ...
			 */
			oldNum = parBind->nbNodes; /* Skip newly added items. */
			if(!WXS_ILIST_IS_EMPTY(parBind->dupls)) {
				oldDupls = parBind->dupls->nbItems;
				dupls = (xmlSchemaPSVIIDCNodePtr*)parBind->dupls->items;
			}
			else {
				dupls = NULL;
				oldDupls = 0;
			}
			parNodes = parBind->nodeTable;
			nbFields = bind->definition->nbFields;
			for(i = 0; i < bind->nbNodes; i++) {
				P_Node = bind->nodeTable[i];
				if(!P_Node)
					continue;
				/*
				 * ...with every key-sequence of the parent node, already
				 * evaluated to be a duplicate key-sequence.
				 */
				if(oldDupls) {
					j = 0;
					while(j < oldDupls) {
						if(nbFields == 1) {
							ret = xmlSchemaAreValuesEqual(P_Node->keys[0]->val, dupls[j]->keys[0]->val);
							if(ret == -1)
								goto internal_error;
							if(ret == 0) {
								j++;
								continue;
							}
						}
						else {
							parNode = dupls[j];
							for(k = 0; k < nbFields; k++) {
								ret = xmlSchemaAreValuesEqual(P_Node->keys[k]->val, parNode->keys[k]->val);
								if(ret == -1)
									goto internal_error;
								if(ret == 0)
									break;
							}
						}
						if(ret == 1)
							/* Duplicate found. */
							break;
						j++;
					}
					if(j != oldDupls) {
						/* Duplicate found. Skip this entry. */
						continue;
					}
				}
				/*
				 * ... and with every key-sequence of the parent node.
				 */
				if(oldNum) {
					j = 0;
					while(j < oldNum) {
						parNode = parNodes[j];
						if(nbFields == 1) {
							ret = xmlSchemaAreValuesEqual(P_Node->keys[0]->val, parNode->keys[0]->val);
							if(ret == -1)
								goto internal_error;
							if(ret == 0) {
								j++;
								continue;
							}
						}
						else {
							for(k = 0; k < nbFields; k++) {
								ret = xmlSchemaAreValuesEqual(P_Node->keys[k]->val, parNode->keys[k]->val);
								if(ret == -1)
									goto internal_error;
								if(ret == 0)
									break;
							}
						}
						if(ret == 1)
							/* Duplicate found. */
							break;
						j++;
					}
					if(j != oldNum) {
						/*
						 * Handle duplicates. Move the duplicate in
						 * the parent's node-table to the list of
						 * duplicates.
						 */
						oldNum--;
						parBind->nbNodes--;
						/*
						 * Move last old item to pos of duplicate.
						 */
						parNodes[j] = parNodes[oldNum];
						if(parBind->nbNodes != oldNum) {
							/*
							 * If new items exist, move last new item to
							 * last of old items.
							 */
							parNodes[oldNum] = parNodes[parBind->nbNodes];
						}
						if(parBind->dupls == NULL) {
							parBind->dupls = xmlSchemaItemListCreate();
							if(parBind->dupls == NULL)
								goto internal_error;
						}
						xmlSchemaItemListAdd(parBind->dupls, parNode);
					}
					else {
						/*
						 * Add the node-table entry (node and key-sequence) of
						 * the child node to the node table of the parent node.
						 */
						if(parBind->nodeTable == NULL) {
							parBind->nodeTable = (xmlSchemaPSVIIDCNodePtr*)SAlloc::M(10 * sizeof(xmlSchemaPSVIIDCNodePtr));
							if(parBind->nodeTable == NULL) {
								xmlSchemaVErrMemory(NULL, "allocating IDC list of node-table items", 0);
								goto internal_error;
							}
							parBind->sizeNodes = 1;
						}
						else if(parBind->nbNodes >= parBind->sizeNodes) {
							parBind->sizeNodes *= 2;
							parBind->nodeTable = (xmlSchemaPSVIIDCNodePtr*)SAlloc::R(parBind->nodeTable, parBind->sizeNodes * sizeof(xmlSchemaPSVIIDCNodePtr));
							if(parBind->nodeTable == NULL) {
								xmlSchemaVErrMemory(NULL, "re-allocating IDC list of node-table items", 0);
								goto internal_error;
							}
						}
						parNodes = parBind->nodeTable;
						/*
						 * Append the new node-table entry to the 'new node-table
						 * entries' section.
						 */
						parNodes[parBind->nbNodes++] = P_Node;
					}
				}
			}
		}
		else {
			/*
			 * No binding for the IDC was found: create a new one and
			 * copy all node-tables.
			 */
			parBind = xmlSchemaIDCNewBinding(bind->definition);
			if(parBind == NULL)
				goto internal_error;

			/*
			 * @todo Hmm, how to optimize the initial number of
			 * allocated entries?
			 */
			if(bind->nbNodes != 0) {
				/*
				 * Add all IDC node-table entries.
				 */
				if(!vctxt->psviExposeIDCNodeTables) {
					/*
					 * Just move the entries.
					 * NOTE: this is quite save here, since
					 * all the keyref lookups have already been
					 * performed.
					 */
					parBind->nodeTable = bind->nodeTable;
					bind->nodeTable = NULL;
					parBind->sizeNodes = bind->sizeNodes;
					bind->sizeNodes = 0;
					parBind->nbNodes = bind->nbNodes;
					bind->nbNodes = 0;
				}
				else {
					/*
					 * Copy the entries.
					 */
					parBind->nodeTable = (xmlSchemaPSVIIDCNodePtr*)SAlloc::M(bind->nbNodes * sizeof(xmlSchemaPSVIIDCNodePtr));
					if(parBind->nodeTable == NULL) {
						xmlSchemaVErrMemory(NULL, "allocating an array of IDC node-table items", 0);
						xmlSchemaIDCFreeBinding(parBind);
						goto internal_error;
					}
					parBind->sizeNodes = bind->nbNodes;
					parBind->nbNodes = bind->nbNodes;
					memcpy(parBind->nodeTable, bind->nodeTable,
					    bind->nbNodes * sizeof(xmlSchemaPSVIIDCNodePtr));
				}
			}
			if(bind->dupls) {
				/*
				 * Move the duplicates.
				 */
				xmlSchemaItemListFree(parBind->dupls);
				parBind->dupls = bind->dupls;
				bind->dupls = NULL;
			}
			if(parTable != NULL) {
				if(*parTable == NULL)
					*parTable = parBind;
				else {
					parBind->next = *parTable;
					*parTable = parBind;
				}
			}
		}

next_binding:
		bind = bind->next;
	}
	return 0;

internal_error:
	return -1;
}

/**
 * xmlSchemaCheckCVCIDCKeyRef:
 * @vctxt: the WXS validation context
 * @elemDecl: the element declaration
 *
 * Check the cvc-idc-keyref constraints.
 */
static int xmlSchemaCheckCVCIDCKeyRef(xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaPSVIIDCBindingPtr bind;
	xmlSchemaIDCMatcherPtr matcher = vctxt->inode->idcMatchers;
	/*
	 * Find a keyref.
	 */
	while(matcher != NULL) {
		if((matcher->idcType == XML_SCHEMA_TYPE_IDC_KEYREF) && matcher->targets && matcher->targets->nbItems) {
			int i, j, k, res, nbFields, hasDupls;
			xmlSchemaPSVIIDCKeyPtr * refKeys, * keys;
			xmlSchemaPSVIIDCNodePtr refNode = NULL;
			nbFields = matcher->aidc->def->nbFields;
			/*
			 * Find the IDC node-table for the referenced IDC key/unique.
			 */
			bind = vctxt->inode->idcTable;
			while(bind != NULL) {
				if((xmlSchemaIDCPtr)matcher->aidc->def->ref->item == bind->definition)
					break;
				bind = bind->next;
			}
			hasDupls = (bind && bind->dupls && bind->dupls->nbItems) ? 1 : 0;
			/*
			 * Search for a matching key-sequences.
			 */
			for(i = 0; i < matcher->targets->nbItems; i++) {
				res = 0;
				refNode = (xmlSchemaPSVIIDCNodePtr)matcher->targets->items[i];
				if(bind != NULL) {
					refKeys = refNode->keys;
					for(j = 0; j < bind->nbNodes; j++) {
						keys = bind->nodeTable[j]->keys;
						for(k = 0; k < nbFields; k++) {
							res = xmlSchemaAreValuesEqual(keys[k]->val, refKeys[k]->val);
							if(res == 0)
								break;
							else if(res == -1) {
								return -1;
							}
						}
						if(res == 1) {
							/*
							 * Match found.
							 */
							break;
						}
					}
					if((res == 0) && hasDupls) {
						/*
						 * Search in duplicates
						 */
						for(j = 0; j < bind->dupls->nbItems; j++) {
							keys = ((xmlSchemaPSVIIDCNodePtr)bind->dupls->items[j])->keys;
							for(k = 0; k < nbFields; k++) {
								res = xmlSchemaAreValuesEqual(keys[k]->val, refKeys[k]->val);
								if(res == 0)
									break;
								else if(res == -1) {
									return -1;
								}
							}
							if(res == 1) {
								/*
								 * Match in duplicates found.
								 */
								xmlChar * str = NULL, * strB = NULL;
								xmlSchemaKeyrefErr(vctxt, XML_SCHEMAV_CVC_IDC, refNode,
								    (xmlSchemaTypePtr)matcher->aidc->def, "More than one match found for key-sequence %s of keyref '%s'",
								    xmlSchemaFormatIDCKeySequence(vctxt, &str, refNode->keys, nbFields),
								    xmlSchemaGetComponentQName(&strB, matcher->aidc->def));
								ZFREE(str);
								ZFREE(strB);
								break;
							}
						}
					}
				}

				if(res == 0) {
					xmlChar * str = NULL, * strB = NULL;
					xmlSchemaKeyrefErr(vctxt, XML_SCHEMAV_CVC_IDC, refNode,
					    (xmlSchemaTypePtr)matcher->aidc->def,
					    "No match found for key-sequence %s of keyref '%s'",
					    xmlSchemaFormatIDCKeySequence(vctxt, &str, refNode->keys, nbFields),
					    xmlSchemaGetComponentQName(&strB, matcher->aidc->def));
					ZFREE(str);
					ZFREE(strB);
				}
			}
		}
		matcher = matcher->next;
	}
	/* @todo Return an error if any error encountered. */
	return 0;
}

/************************************************************************
*									*
*			XML Reader validation code                      *
*									*
************************************************************************/

static xmlSchemaAttrInfoPtr xmlSchemaGetFreshAttrInfo(xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaAttrInfoPtr iattr;
	/*
	 * Grow/create list of attribute infos.
	 */
	if(vctxt->attrInfos == NULL) {
		vctxt->attrInfos = (xmlSchemaAttrInfoPtr*)SAlloc::M(sizeof(xmlSchemaAttrInfoPtr));
		vctxt->sizeAttrInfos = 1;
		if(vctxt->attrInfos == NULL) {
			xmlSchemaVErrMemory(vctxt, "allocating attribute info list", 0);
			return 0;
		}
	}
	else if(vctxt->sizeAttrInfos <= vctxt->nbAttrInfos) {
		vctxt->sizeAttrInfos++;
		vctxt->attrInfos = (xmlSchemaAttrInfoPtr*)SAlloc::R(vctxt->attrInfos, vctxt->sizeAttrInfos * sizeof(xmlSchemaAttrInfoPtr));
		if(vctxt->attrInfos == NULL) {
			xmlSchemaVErrMemory(vctxt, "re-allocating attribute info list", 0);
			return 0;
		}
	}
	else {
		iattr = vctxt->attrInfos[vctxt->nbAttrInfos++];
		if(iattr->localName != NULL) {
			VERROR_INT("xmlSchemaGetFreshAttrInfo", "attr info not cleared");
			return 0;
		}
		iattr->nodeType = XML_ATTRIBUTE_NODE;
		return (iattr);
	}
	/*
	 * Create an attribute info.
	 */
	iattr = (xmlSchemaAttrInfoPtr)SAlloc::M(sizeof(xmlSchemaAttrInfo));
	if(iattr == NULL) {
		xmlSchemaVErrMemory(vctxt, "creating new attribute info", 0);
		return 0;
	}
	memzero(iattr, sizeof(xmlSchemaAttrInfo));
	iattr->nodeType = XML_ATTRIBUTE_NODE;
	vctxt->attrInfos[vctxt->nbAttrInfos++] = iattr;
	return (iattr);
}

static int xmlSchemaValidatorPushAttribute(xmlSchemaValidCtxtPtr vctxt, xmlNode * attrNode, int nodeLine,
    const xmlChar * localName, const xmlChar * nsName, int ownedNames, xmlChar * value, int ownedValue)
{
	xmlSchemaAttrInfoPtr attr = xmlSchemaGetFreshAttrInfo(vctxt);
	if(!attr) {
		VERROR_INT("xmlSchemaPushAttribute", "calling xmlSchemaGetFreshAttrInfo()");
		return -1;
	}
	attr->P_Node = attrNode;
	attr->nodeLine = nodeLine;
	attr->state = XML_SCHEMAS_ATTR_UNKNOWN;
	attr->localName = localName;
	attr->nsName = nsName;
	if(ownedNames)
		attr->flags |= XML_SCHEMA_NODE_INFO_FLAG_OWNED_NAMES;
	/*
	 * Evaluate if it's an XSI attribute.
	 */
	if(nsName != NULL) {
		if(sstreq(localName, "nil")) {
			if(sstreq(attr->nsName, xmlSchemaInstanceNs)) {
				attr->metaType = XML_SCHEMA_ATTR_INFO_META_XSI_NIL;
			}
		}
		else if(sstreq(localName, "type")) {
			if(sstreq(attr->nsName, xmlSchemaInstanceNs)) {
				attr->metaType = XML_SCHEMA_ATTR_INFO_META_XSI_TYPE;
			}
		}
		else if(sstreq(localName, "schemaLocation")) {
			if(sstreq(attr->nsName, xmlSchemaInstanceNs)) {
				attr->metaType = XML_SCHEMA_ATTR_INFO_META_XSI_SCHEMA_LOC;
			}
		}
		else if(sstreq(localName, "noNamespaceSchemaLocation")) {
			if(sstreq(attr->nsName, xmlSchemaInstanceNs)) {
				attr->metaType = XML_SCHEMA_ATTR_INFO_META_XSI_NO_NS_SCHEMA_LOC;
			}
		}
		else if(sstreq(attr->nsName, xmlNamespaceNs)) {
			attr->metaType = XML_SCHEMA_ATTR_INFO_META_XMLNS;
		}
	}
	attr->value = value;
	if(ownedValue)
		attr->flags |= XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES;
	if(attr->metaType != 0)
		attr->state = XML_SCHEMAS_ATTR_META;
	return 0;
}

/**
 * xmlSchemaClearElemInfo:
 * @vctxt: the WXS validation context
 * @ielem: the element information item
 */
static void xmlSchemaClearElemInfo(xmlSchemaValidCtxt * vctxt, xmlSchemaNodeInfo * ielem)
{
	ielem->hasKeyrefs = 0;
	ielem->appliedXPath = 0;
	if(ielem->flags & XML_SCHEMA_NODE_INFO_FLAG_OWNED_NAMES) {
		SAlloc::F((void *)ielem->localName); // @badcast
		ielem->localName = 0;
		SAlloc::F((void *)ielem->nsName); // @badcast
		ielem->nsName = 0;
	}
	else {
		ielem->localName = NULL;
		ielem->nsName = NULL;
	}
	if(ielem->flags & XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES) {
		SAlloc::F((void *)ielem->value); // @badcast
		ielem->value = 0;
	}
	else {
		ielem->value = NULL;
	}
	if(ielem->val) {
		// PSVI TODO: Be careful not to free it when the value is exposed via PSVI.
		xmlSchemaFreeValue(ielem->val);
		ielem->val = NULL;
	}
	if(ielem->idcMatchers != NULL) {
		/*
		 * REVISIT OPTIMIZE TODO: Use a pool of IDC matchers.
		 *   Does it work?
		 */
		xmlSchemaIDCReleaseMatcherList(vctxt, ielem->idcMatchers);
#if 0
		xmlSchemaIDCFreeMatcherList(ielem->idcMatchers);
#endif
		ielem->idcMatchers = NULL;
	}
	if(ielem->idcTable) {
		/*
		 * OPTIMIZE TODO: Use a pool of IDC tables??.
		 */
		xmlSchemaIDCFreeIDCTable(ielem->idcTable);
		ielem->idcTable = NULL;
	}
	if(ielem->regexCtxt) {
		xmlRegFreeExecCtxt(ielem->regexCtxt);
		ielem->regexCtxt = NULL;
	}
	if(ielem->nsBindings) {
		SAlloc::F((xmlChar**)ielem->nsBindings);
		ielem->nsBindings = NULL;
		ielem->nbNsBindings = 0;
		ielem->sizeNsBindings = 0;
	}
}

/**
 * xmlSchemaGetFreshElemInfo:
 * @vctxt: the schema validation context
 *
 * Creates/reuses and initializes the element info item for
 * the currect tree depth.
 *
 * Returns the element info item or NULL on API or internal errors.
 */
static xmlSchemaNodeInfoPtr xmlSchemaGetFreshElemInfo(xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaNodeInfoPtr info = NULL;
	if(vctxt->depth > vctxt->sizeElemInfos) {
		VERROR_INT("xmlSchemaGetFreshElemInfo", "inconsistent depth encountered");
		return 0;
	}
	if(vctxt->elemInfos == NULL) {
		vctxt->elemInfos = (xmlSchemaNodeInfoPtr*)SAlloc::M(10 * sizeof(xmlSchemaNodeInfoPtr));
		if(vctxt->elemInfos == NULL) {
			xmlSchemaVErrMemory(vctxt, "allocating the element info array", 0);
			return 0;
		}
		memzero(vctxt->elemInfos, 10 * sizeof(xmlSchemaNodeInfoPtr));
		vctxt->sizeElemInfos = 10;
	}
	else if(vctxt->sizeElemInfos <= vctxt->depth) {
		int i = vctxt->sizeElemInfos;
		vctxt->sizeElemInfos *= 2;
		vctxt->elemInfos = (xmlSchemaNodeInfoPtr*)SAlloc::R(vctxt->elemInfos, vctxt->sizeElemInfos * sizeof(xmlSchemaNodeInfoPtr));
		if(vctxt->elemInfos == NULL) {
			xmlSchemaVErrMemory(vctxt, "re-allocating the element info array", 0);
			return 0;
		}
		/*
		 * We need the new memory to be NULLed.
		 * @todo Use memset instead?
		 */
		for(; i < vctxt->sizeElemInfos; i++)
			vctxt->elemInfos[i] = NULL;
	}
	else
		info = vctxt->elemInfos[vctxt->depth];
	if(info == NULL) {
		info = (xmlSchemaNodeInfoPtr)SAlloc::M(sizeof(xmlSchemaNodeInfo));
		if(info == NULL) {
			xmlSchemaVErrMemory(vctxt, "allocating an element info", 0);
			return 0;
		}
		vctxt->elemInfos[vctxt->depth] = info;
	}
	else {
		if(info->localName != NULL) {
			VERROR_INT("xmlSchemaGetFreshElemInfo", "elem info has not been cleared");
			return 0;
		}
	}
	memzero(info, sizeof(xmlSchemaNodeInfo));
	info->nodeType = XML_ELEMENT_NODE;
	info->depth = vctxt->depth;
	return (info);
}

#define ACTIVATE_ATTRIBUTE(item) vctxt->inode = (xmlSchemaNodeInfoPtr)item;
#define ACTIVATE_ELEM vctxt->inode = vctxt->elemInfos[vctxt->depth];
#define ACTIVATE_PARENT_ELEM vctxt->inode = vctxt->elemInfos[vctxt->depth -1];

static int xmlSchemaValidateFacets(xmlSchemaAbstractCtxtPtr actxt, xmlNode * P_Node, xmlSchemaTypePtr type,
    xmlSchemaValType valType, const xmlChar * value, xmlSchemaValPtr val, unsigned long length, int fireErrors)
{
	int ret, error = 0;
	xmlSchemaTypePtr tmpType;
	xmlSchemaFacetLinkPtr facetLink;
	xmlSchemaFacetPtr facet;
	unsigned long len = 0;
	xmlSchemaWhitespaceValueType ws;
	/*
	 * In Libxml2, derived built-in types have currently no explicit facets.
	 */
	if(type->type == XML_SCHEMA_TYPE_BASIC)
		return 0;
	/*
	 * NOTE: Do not jump away, if the facetSet of the given type is
	 * empty: until now, "pattern" and "enumeration" facets of the
	 * *base types* need to be checked as well.
	 */
	if(type->facetSet == NULL)
		goto pattern_and_enum;

	if(!WXS_IS_ATOMIC(type)) {
		if(WXS_IS_LIST(type))
			goto WXS_IS_LIST;
		else
			goto pattern_and_enum;
	}
	/*
	 * Whitespace handling is only of importance for string-based
	 * types.
	 */
	tmpType = xmlSchemaGetPrimitiveType(type);
	if((tmpType->builtInType == XML_SCHEMAS_STRING) || WXS_IS_ANY_SIMPLE_TYPE(tmpType)) 
		ws = xmlSchemaGetWhiteSpaceFacetValue(type);
	else
		ws = XML_SCHEMA_WHITESPACE_COLLAPSE;
	/*
	 * If the value was not computed (for string or anySimpleType based types), then use the provided type.
	 */
	if(val)
		valType = xmlSchemaGetValType(val);
	ret = 0;
	for(facetLink = type->facetSet; facetLink != NULL; facetLink = facetLink->next) {
		/*
		 * Skip the pattern "whiteSpace": it is used to
		 * format the character content beforehand.
		 */
		switch(facetLink->facet->type) {
			case XML_SCHEMA_FACET_WHITESPACE:
			case XML_SCHEMA_FACET_PATTERN:
			case XML_SCHEMA_FACET_ENUMERATION:
			    continue;
			case XML_SCHEMA_FACET_LENGTH:
			case XML_SCHEMA_FACET_MINLENGTH:
			case XML_SCHEMA_FACET_MAXLENGTH:
			    ret = xmlSchemaValidateLengthFacetWhtsp(facetLink->facet, valType, value, val, &len, ws);
			    break;
			default:
			    ret = xmlSchemaValidateFacetWhtsp(facetLink->facet, ws, valType, value, val, ws);
			    break;
		}
		if(ret < 0) {
			AERROR_INT("xmlSchemaValidateFacets", "validating against a atomic type facet");
			return -1;
		}
		else if(ret > 0) {
			if(fireErrors)
				xmlSchemaFacetErr(actxt, (xmlParserErrors)ret, P_Node, value, len, type, facetLink->facet, NULL, 0, 0);
			else
				return ret;
			SETIFZ(error, ret);
		}
		ret = 0;
	}

WXS_IS_LIST:
	if(!WXS_IS_LIST(type))
		goto pattern_and_enum;
	/*
	 * "length", "minLength" and "maxLength" of list types.
	 */
	ret = 0;
	for(facetLink = type->facetSet; facetLink != NULL; facetLink = facetLink->next) {
		switch(facetLink->facet->type) {
			case XML_SCHEMA_FACET_LENGTH:
			case XML_SCHEMA_FACET_MINLENGTH:
			case XML_SCHEMA_FACET_MAXLENGTH:
			    ret = xmlSchemaValidateListSimpleTypeFacet(facetLink->facet, value, length, 0);
			    break;
			default:
			    continue;
		}
		if(ret < 0) {
			AERROR_INT("xmlSchemaValidateFacets", "validating against a list type facet");
			return -1;
		}
		else if(ret > 0) {
			if(fireErrors)
				xmlSchemaFacetErr(actxt, (xmlParserErrors)ret, P_Node, value, length, type, facetLink->facet, NULL, 0, 0);
			else
				return ret;
			SETIFZ(error, ret);
		}
		ret = 0;
	}

pattern_and_enum:
	if(error >= 0) {
		int found = 0;
		/*
		 * Process enumerations. Facet values are in the value space
		 * of the defining type's base type. This seems to be a bug in the
		 * XML Schema 1.0 spec. Use the whitespace type of the base type.
		 * Only the first set of enumerations in the ancestor-or-self axis
		 * is used for validation.
		 */
		ret = 0;
		tmpType = type;
		do {
			for(facet = tmpType->facets; facet != NULL; facet = facet->next) {
				if(facet->type != XML_SCHEMA_FACET_ENUMERATION)
					continue;
				found = 1;
				ret = xmlSchemaAreValuesEqual(facet->val, val);
				if(ret == 1)
					break;
				else if(ret < 0) {
					AERROR_INT("xmlSchemaValidateFacets", "validating against an enumeration facet");
					return -1;
				}
			}
			if(ret != 0)
				break;
			/*
			 * Break on the first set of enumerations. Any additional
			 *  enumerations which might be existent on the ancestors
			 *  of the current type are restricted by this set; thus
			 *  *must* *not* be taken into account.
			 */
			if(found)
				break;
			tmpType = tmpType->baseType;
		} while((tmpType != NULL) &&
		    (tmpType->type != XML_SCHEMA_TYPE_BASIC));
		if(found && (ret == 0)) {
			ret = XML_SCHEMAV_CVC_ENUMERATION_VALID;
			if(fireErrors) {
				xmlSchemaFacetErr(actxt, (xmlParserErrors)ret, P_Node, value, 0, type, NULL, NULL, 0, 0);
			}
			else
				return ret;
			SETIFZ(error, ret);
		}
	}

	if(error >= 0) {
		int found;
		/*
		 * Process patters. Pattern facets are ORed at type level
		 * and ANDed if derived. Walk the base type axis.
		 */
		tmpType = type;
		facet = NULL;
		do {
			found = 0;
			for(facetLink = tmpType->facetSet; facetLink != NULL; facetLink = facetLink->next) {
				if(facetLink->facet->type != XML_SCHEMA_FACET_PATTERN)
					continue;
				found = 1;
				/*
				 * NOTE that for patterns, @value needs to be the
				 * normalized vaule.
				 */
				ret = xmlRegexpExec(facetLink->facet->regexp, value);
				if(ret == 1)
					break;
				else if(ret < 0) {
					AERROR_INT("xmlSchemaValidateFacets", "validating against a pattern facet");
					return -1;
				}
				else {
					/*
					 * Save the last non-validating facet.
					 */
					facet = facetLink->facet;
				}
			}
			if(found && (ret != 1)) {
				ret = XML_SCHEMAV_CVC_PATTERN_VALID;
				if(fireErrors) {
					xmlSchemaFacetErr(actxt, (xmlParserErrors)ret, P_Node, value, 0, type, facet, NULL, 0, 0);
				}
				else
					return ret;
				SETIFZ(error, ret);
				break;
			}
			tmpType = tmpType->baseType;
		} while(tmpType && (tmpType->type != XML_SCHEMA_TYPE_BASIC));
	}
	return (error);
}

static xmlChar * FASTCALL xmlSchemaNormalizeValue(xmlSchemaTypePtr type, const xmlChar * value)
{
	switch(xmlSchemaGetWhiteSpaceFacetValue(type)) {
		case XML_SCHEMA_WHITESPACE_COLLAPSE: return (xmlSchemaCollapseString(value));
		case XML_SCHEMA_WHITESPACE_REPLACE: return (xmlSchemaWhiteSpaceReplace(value));
		default: return 0;
	}
}

static int xmlSchemaValidateQName(xmlSchemaValidCtxtPtr vctxt, const xmlChar * value, xmlSchemaValPtr * val, int valNeeded)
{
	const xmlChar * nsName;
	xmlChar * local, * prefix = NULL;
	int ret = xmlValidateQName(value, 1);
	if(ret != 0) {
		if(ret == -1) {
			VERROR_INT("xmlSchemaValidateQName", "calling xmlValidateQName()");
			return -1;
		}
		return( XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_1);
	}
	/*
	 * NOTE: xmlSplitQName2 will always return a duplicated
	 * strings.
	 */
	local = xmlSplitQName2(value, &prefix);
	SETIFZ(local, sstrdup(value));
	/*
	 * OPTIMIZE TODO: Use flags for:
	 *  - is there any namespace binding?
	 *  - is there a default namespace?
	 */
	nsName = xmlSchemaLookupNamespace(vctxt, prefix);
	if(prefix) {
		SAlloc::F(prefix);
		/*
		 * A namespace must be found if the prefix is
		 * NOT NULL.
		 */
		if(nsName == NULL) {
			ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_1;
			xmlSchemaCustomErr(ACTXT_CAST vctxt, (xmlParserErrors)ret, NULL,
			    WXS_BASIC_CAST xmlSchemaGetBuiltInType(XML_SCHEMAS_QNAME),
			    "The QName value '%s' has no corresponding namespace declaration in scope", value, 0);
			SAlloc::F(local);
			return ret;
		}
	}
	if(valNeeded && val)
		*val = xmlSchemaNewQNameValue(BAD_CAST sstrdup(nsName), BAD_CAST local);
	else
		SAlloc::F(local);
	return 0;
}
/*
 * cvc-simple-type
 */
static int xmlSchemaVCheckCVCSimpleType(xmlSchemaAbstractCtxtPtr actxt, xmlNode * P_Node, xmlSchemaTypePtr type,
    const xmlChar * value, xmlSchemaValPtr * retVal, int fireErrors, int normalize, int isNormalized)
{
	int ret = 0;
	int valNeeded = (retVal) ? 1 : 0;
	xmlSchemaValPtr val = NULL;
	// xmlSchemaWhitespaceValueType ws; 
	xmlChar * normValue = NULL;
#define NORMALIZE(atype) \
	if((!isNormalized) && (normalize || (type->flags & XML_SCHEMAS_TYPE_NORMVALUENEEDED))) { \
		normValue = xmlSchemaNormalizeValue(atype, value); \
		if(normValue) \
			value = normValue; \
		isNormalized = 1; \
	}
	if(retVal && (*retVal != NULL)) {
		xmlSchemaFreeValue(*retVal);
		*retVal = NULL;
	}
	/*
	 * 3.14.4 Simple Type Definition Validation Rules
	 * Validation Rule: String Valid
	 */
	/*
	 * 1 It is schema-valid with respect to that definition as defined
	 * by Datatype Valid in [XML Schemas: Datatypes].
	 */
	/*
	 * 2.1 If The definition is ENTITY or is validly derived from ENTITY given
	 * the empty set, as defined in Type Derivation OK (Simple) ($3.14.6), then
	 * the string must be a `declared entity name`.
	 */
	/*
	 * 2.2 If The definition is ENTITIES or is validly derived from ENTITIES
	 * given the empty set, as defined in Type Derivation OK (Simple) ($3.14.6),
	 * then every whitespace-delimited substring of the string must be a `declared
	 * entity name`.
	 */
	/*
	 * 2.3 otherwise no further condition applies.
	 */
	if((!valNeeded) && (type->flags & XML_SCHEMAS_TYPE_FACETSNEEDVALUE))
		valNeeded = 1;
	if(!value)
		value = BAD_CAST "";
	if(WXS_IS_ANY_SIMPLE_TYPE(type) || WXS_IS_ATOMIC(type)) {
		xmlSchemaTypePtr biType; /* The built-in type. */
		/*
		 * SPEC (1.2.1) "if {variety} is `atomic` then the string must `match`
		 * a literal in the `lexical space` of {base type definition}"
		 */
		/*
		 * Whitespace-normalize.
		 */
		NORMALIZE(type);
		if(type->type != XML_SCHEMA_TYPE_BASIC) {
			/*
			 * Get the built-in type.
			 */
			biType = type->baseType;
			while(biType && (biType->type != XML_SCHEMA_TYPE_BASIC))
				biType = biType->baseType;
			if(biType == NULL) {
				AERROR_INT("xmlSchemaVCheckCVCSimpleType", "could not get the built-in type");
				goto internal_error;
			}
		}
		else
			biType = type;
		/*
		 * NOTATIONs need to be processed here, since they need
		 * to lookup in the hashtable of NOTATION declarations of the schema.
		 */
		if(actxt->type == XML_SCHEMA_CTXT_VALIDATOR) {
			switch(biType->builtInType) {
				case XML_SCHEMAS_NOTATION:
				    ret = xmlSchemaValidateNotation((xmlSchemaValidCtxtPtr)actxt, ((xmlSchemaValidCtxtPtr)actxt)->schema, NULL, value, &val, valNeeded);
				    break;
				case XML_SCHEMAS_QNAME:
				    ret = xmlSchemaValidateQName((xmlSchemaValidCtxtPtr)actxt, value, &val, valNeeded);
				    break;
				default:
				    /* ws = xmlSchemaGetWhiteSpaceFacetValue(type); */
				    if(valNeeded)
					    ret = xmlSchemaValPredefTypeNodeNoNorm(biType, value, &val, P_Node);
				    else
					    ret = xmlSchemaValPredefTypeNodeNoNorm(biType, value, NULL, P_Node);
				    break;
			}
		}
		else if(actxt->type == XML_SCHEMA_CTXT_PARSER) {
			switch(biType->builtInType) {
				case XML_SCHEMAS_NOTATION:
				    ret = xmlSchemaValidateNotation(NULL, ((xmlSchemaParserCtxtPtr)actxt)->schema, P_Node, value, &val, valNeeded);
				    break;
				default:
				    /* ws = xmlSchemaGetWhiteSpaceFacetValue(type); */
				    if(valNeeded)
					    ret = xmlSchemaValPredefTypeNodeNoNorm(biType, value, &val, P_Node);
				    else
					    ret = xmlSchemaValPredefTypeNodeNoNorm(biType, value, NULL, P_Node);
				    break;
			}
		}
		else {
			/*
			 * Validation via a public API is not implemented yet.
			 */
			TODO
			goto internal_error;
		}
		if(ret != 0) {
			if(ret < 0) {
				AERROR_INT("xmlSchemaVCheckCVCSimpleType", "validating against a built-in type");
				goto internal_error;
			}
			if(WXS_IS_LIST(type))
				ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_2;
			else
				ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_1;
		}
		if(!ret && type->flags & XML_SCHEMAS_TYPE_HAS_FACETS) {
			/*
			 * Check facets.
			 */
			ret = xmlSchemaValidateFacets(actxt, P_Node, type, (xmlSchemaValType)biType->builtInType, value, val, 0, fireErrors);
			if(ret) {
				if(ret < 0) {
					AERROR_INT("xmlSchemaVCheckCVCSimpleType", "validating facets of atomic simple type");
					goto internal_error;
				}
				if(WXS_IS_LIST(type))
					ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_2;
				else
					ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_1;
			}
		}
		if(fireErrors && (ret > 0))
			xmlSchemaSimpleTypeErr(actxt, (xmlParserErrors)ret, P_Node, value, type, 1);
	}
	else if(WXS_IS_LIST(type)) {
		xmlSchemaTypePtr itemType;
		const xmlChar * cur, * end;
		xmlChar * tmpValue = NULL;
		unsigned long len = 0;
		xmlSchemaValPtr prevVal = NULL, curVal = NULL;
		/* 1.2.2 if {variety} is `list` then the string must be a sequence
		 * of white space separated tokens, each of which `match`es a literal
		 * in the `lexical space` of {item type definition}
		 */
		/*
		 * Note that XML_SCHEMAS_TYPE_NORMVALUENEEDED will be set if
		 * the list type has an enum or pattern facet.
		 */
		NORMALIZE(type);
		/*
		 * VAL TODO: Optimize validation of empty values.
		 * VAL TODO: We do not have computed values for lists.
		 */
		itemType = WXS_LIST_ITEMTYPE(type);
		cur = value;
		do {
			while(IS_BLANK_CH(*cur))
				cur++;
			end = cur;
			while((*end != 0) && (!(IS_BLANK_CH(*end))))
				end++;
			if(end == cur)
				break;
			tmpValue = xmlStrndup(cur, end - cur);
			len++;
			if(valNeeded)
				ret = xmlSchemaVCheckCVCSimpleType(actxt, P_Node, itemType, tmpValue, &curVal, fireErrors, 0, 1);
			else
				ret = xmlSchemaVCheckCVCSimpleType(actxt, P_Node, itemType, tmpValue, NULL, fireErrors, 0, 1);
			ZFREE(tmpValue);
			if(curVal != NULL) {
				/*
				 * Add to list of computed values.
				 */
				if(!val)
					val = curVal;
				else
					xmlSchemaValueAppend(prevVal, curVal);
				prevVal = curVal;
				curVal = NULL;
			}
			if(ret != 0) {
				if(ret < 0) {
					AERROR_INT("xmlSchemaVCheckCVCSimpleType", "validating an item of list simple type");
					goto internal_error;
				}
				ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_2;
				break;
			}
			cur = end;
		} while(*cur != 0);
		ZFREE(tmpValue);
		if((ret == 0) && (type->flags & XML_SCHEMAS_TYPE_HAS_FACETS)) {
			/*
			 * Apply facets (pattern, enumeration).
			 */
			ret = xmlSchemaValidateFacets(actxt, P_Node, type, XML_SCHEMAS_UNKNOWN, value, val, len, fireErrors);
			if(ret != 0) {
				if(ret < 0) {
					AERROR_INT("xmlSchemaVCheckCVCSimpleType", "validating facets of list simple type");
					goto internal_error;
				}
				ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_2;
			}
		}
		if(fireErrors && (ret > 0)) {
			/*
			 * Report the normalized value.
			 */
			normalize = 1;
			NORMALIZE(type);
			xmlSchemaSimpleTypeErr(actxt, (xmlParserErrors)ret, P_Node, value, type, 1);
		}
	}
	else if(WXS_IS_UNION(type)) {
		xmlSchemaTypeLinkPtr memberLink;
		/*
		 * @todo For all datatypes `derived` by `union`  whiteSpace does
		 * not apply directly; however, the normalization behavior of `union`
		 * types is controlled by the value of whiteSpace on that one of the
		 * `memberTypes` against which the `union` is successfully validated.
		 *
		 * This means that the value is normalized by the first validating
		 * member type, then the facets of the union type are applied. This
		 * needs changing of the value!
		 */

		/*
		 * 1.2.3 if {variety} is `union` then the string must `match` a
		 * literal in the `lexical space` of at least one member of
		 * {member type definitions}
		 */
		memberLink = xmlSchemaGetUnionSimpleTypeMemberTypes(type);
		if(memberLink == NULL) {
			AERROR_INT("xmlSchemaVCheckCVCSimpleType", "union simple type has no member types");
			goto internal_error;
		}
		/*
		 * Always normalize union type values, since we currently
		 * cannot store the whitespace information with the value
		 * itself; otherwise a later value-comparison would be
		 * not possible.
		 */
		while(memberLink) {
			if(valNeeded)
				ret = xmlSchemaVCheckCVCSimpleType(actxt, P_Node, memberLink->type, value, &val, 0, 1, 0);
			else
				ret = xmlSchemaVCheckCVCSimpleType(actxt, P_Node, memberLink->type, value, NULL, 0, 1, 0);
			if(ret <= 0)
				break;
			memberLink = memberLink->next;
		}
		if(ret != 0) {
			if(ret < 0) {
				AERROR_INT("xmlSchemaVCheckCVCSimpleType", "validating members of union simple type");
				goto internal_error;
			}
			ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_3;
		}
		/*
		 * Apply facets (pattern, enumeration).
		 */
		if((ret == 0) && (type->flags & XML_SCHEMAS_TYPE_HAS_FACETS)) {
			/*
			 * The normalization behavior of `union` types is controlled by
			 * the value of whiteSpace on that one of the `memberTypes`
			 * against which the `union` is successfully validated.
			 */
			NORMALIZE(memberLink->type);
			ret = xmlSchemaValidateFacets(actxt, P_Node, type, XML_SCHEMAS_UNKNOWN, value, val, 0, fireErrors);
			if(ret != 0) {
				if(ret < 0) {
					AERROR_INT("xmlSchemaVCheckCVCSimpleType", "validating facets of union simple type");
					goto internal_error;
				}
				ret = XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_3;
			}
		}
		if(fireErrors && (ret > 0))
			xmlSchemaSimpleTypeErr(actxt, (xmlParserErrors)ret, P_Node, value, type, 1);
	}
	SAlloc::F(normValue);
	if(ret == 0) {
		if(retVal != NULL)
			*retVal = val;
		else
			xmlSchemaFreeValue(val);
	}
	else
		xmlSchemaFreeValue(val);
	return ret;
internal_error:
	SAlloc::F(normValue);
	xmlSchemaFreeValue(val);
	return -1;
}

static int xmlSchemaVExpandQName(xmlSchemaValidCtxtPtr vctxt, const xmlChar * value, const xmlChar ** nsName, const xmlChar ** localName)
{
	int ret = 0;
	if((nsName == NULL) || (localName == NULL))
		return -1;
	*nsName = NULL;
	*localName = NULL;
	ret = xmlValidateQName(value, 1);
	if(ret == -1)
		return -1;
	if(ret > 0) {
		xmlSchemaSimpleTypeErr(ACTXT_CAST vctxt, XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_1, NULL,
		    value, xmlSchemaGetBuiltInType(XML_SCHEMAS_QNAME), 1);
		return 1;
	}
	{
		xmlChar * local = NULL;
		xmlChar * prefix;
		/*
		 * NOTE: xmlSplitQName2 will return a duplicated
		 * string.
		 */
		local = xmlSplitQName2(value, &prefix);
		if(local == NULL)
			*localName = xmlDictLookupSL(vctxt->dict, value);
		else {
			*localName = xmlDictLookupSL(vctxt->dict, local);
			SAlloc::F(local);
		}
		*nsName = xmlSchemaLookupNamespace(vctxt, prefix);
		if(prefix) {
			SAlloc::F(prefix);
			/*
			 * A namespace must be found if the prefix is NOT NULL.
			 */
			if(*nsName == NULL) {
				xmlSchemaCustomErr(ACTXT_CAST vctxt,
				    XML_SCHEMAV_CVC_DATATYPE_VALID_1_2_1, NULL,
				    WXS_BASIC_CAST xmlSchemaGetBuiltInType(XML_SCHEMAS_QNAME),
				    "The QName value '%s' has no "
				    "corresponding namespace declaration in scope",
				    value, 0);
				return (2);
			}
		}
	}
	return 0;
}

static int xmlSchemaProcessXSIType(xmlSchemaValidCtxtPtr vctxt, xmlSchemaAttrInfoPtr iattr, xmlSchemaTypePtr * localType, xmlSchemaElementPtr elemDecl)
{
	int ret = 0;
	/*
	 * cvc-elt (3.3.4) : (4)
	 * AND
	 * Schema-Validity Assessment (Element) (cvc-assess-elt)
	 *   (1.2.1.2.1) - (1.2.1.2.4)
	 * Handle 'xsi:type'.
	 */
	if(localType == NULL)
		return -1;
	*localType = NULL;
	if(iattr == NULL)
		return 0;
	else {
		const xmlChar * nsName = NULL, * local = NULL;
		/*
		 * @todo We should report a *warning* that the type was overriden
		 * by the instance.
		 */
		ACTIVATE_ATTRIBUTE(iattr);
		/*
		 * (cvc-elt) (3.3.4) : (4.1)
		 * (cvc-assess-elt) (1.2.1.2.2)
		 */
		ret = xmlSchemaVExpandQName(vctxt, iattr->value, &nsName, &local);
		if(ret != 0) {
			if(ret < 0) {
				VERROR_INT("xmlSchemaValidateElementByDeclaration", "calling xmlSchemaQNameExpand() to validate the attribute 'xsi:type'");
				goto internal_error;
			}
			goto exit;
		}
		/*
		 * (cvc-elt) (3.3.4) : (4.2)
		 * (cvc-assess-elt) (1.2.1.2.3)
		 */
		*localType = xmlSchemaGetType(vctxt->schema, local, nsName);
		if(*localType == NULL) {
			xmlChar * str = NULL;
			xmlSchemaCustomErr(ACTXT_CAST vctxt, XML_SCHEMAV_CVC_ELT_4_2, NULL,
			    WXS_BASIC_CAST xmlSchemaGetBuiltInType(XML_SCHEMAS_QNAME),
			    "The QName value '%s' of the xsi:type attribute does not resolve to a type definition",
			    xmlSchemaFormatQName(&str, nsName, local), 0);
			ZFREE(str);
			ret = vctxt->err;
			goto exit;
		}
		if(elemDecl != NULL) {
			int set = 0;

			/*
			 * SPEC cvc-elt (3.3.4) : (4.3) (Type Derivation OK)
			 * "The `local type definition` must be validly
			 * derived from the {type definition} given the union of
			 * the {disallowed substitutions} and the {type definition}'s
			 * {prohibited substitutions}, as defined in
			 * Type Derivation OK (Complex) ($3.4.6)
			 * (if it is a complex type definition),
			 * or given {disallowed substitutions} as defined in Type
			 * Derivation OK (Simple) ($3.14.6) (if it is a simple type
			 * definition)."
			 *
			 * {disallowed substitutions}: the "block" on the element decl.
			 * {prohibited substitutions}: the "block" on the type def.
			 */
			/*
			 * OPTIMIZE TODO: We could map types already evaluated
			 * to be validly derived from other types to avoid checking
			 * this over and over for the same types.
			 */
			if((elemDecl->flags & XML_SCHEMAS_ELEM_BLOCK_EXTENSION) || (elemDecl->subtypes->flags & XML_SCHEMAS_TYPE_BLOCK_EXTENSION))
				set |= SUBSET_EXTENSION;

			if((elemDecl->flags & XML_SCHEMAS_ELEM_BLOCK_RESTRICTION) || (elemDecl->subtypes->flags & XML_SCHEMAS_TYPE_BLOCK_RESTRICTION))
				set |= SUBSET_RESTRICTION;

			/*
			 * REMOVED and CHANGED since this produced a parser context
			 * which adds to the string dict of the schema. So this would
			 * change the schema and we don't want this. We don't need
			 * the parser context anymore.
			 *
			 * if ((vctxt->pctxt == NULL) &&
			 *	(xmlSchemaCreatePCtxtOnVCtxt(vctxt) == -1))
			 *	    return -1;
			 */

			if(xmlSchemaCheckCOSDerivedOK(ACTXT_CAST vctxt, *localType, elemDecl->subtypes, set) != 0) {
				xmlChar * str = NULL;
				xmlSchemaCustomErr(ACTXT_CAST vctxt, XML_SCHEMAV_CVC_ELT_4_3, NULL, NULL,
				    "The type definition '%s', specified by xsi:type, is blocked or not validly derived from the type definition of the element declaration",
				    xmlSchemaFormatQName(&str, (*localType)->targetNamespace, (*localType)->name), 0);
				ZFREE(str);
				ret = vctxt->err;
				*localType = NULL;
			}
		}
	}
exit:
	ACTIVATE_ELEM;
	return ret;
internal_error:
	ACTIVATE_ELEM;
	return -1;
}

static int xmlSchemaValidateElemDecl(xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaElementPtr elemDecl = vctxt->inode->decl;
	xmlSchemaTypePtr actualType;
	/*
	 * cvc-elt (3.3.4) : 1
	 */
	if(elemDecl == NULL) {
		VERROR(XML_SCHEMAV_CVC_ELT_1, NULL, "No matching declaration available");
		return (vctxt->err);
	}
	actualType = WXS_ELEM_TYPEDEF(elemDecl);
	/*
	 * cvc-elt (3.3.4) : 2
	 */
	if(elemDecl->flags & XML_SCHEMAS_ELEM_ABSTRACT) {
		VERROR(XML_SCHEMAV_CVC_ELT_2, NULL, "The element declaration is abstract");
		return (vctxt->err);
	}
	if(actualType == NULL) {
		VERROR(XML_SCHEMAV_CVC_TYPE_1, NULL, "The type definition is absent");
		return (XML_SCHEMAV_CVC_TYPE_1);
	}
	if(vctxt->nbAttrInfos != 0) {
		int ret;
		xmlSchemaAttrInfoPtr iattr;
		/*
		 * cvc-elt (3.3.4) : 3
		 * Handle 'xsi:nil'.
		 */
		iattr = xmlSchemaGetMetaAttrInfo(vctxt, XML_SCHEMA_ATTR_INFO_META_XSI_NIL);
		if(iattr) {
			ACTIVATE_ATTRIBUTE(iattr);
			/*
			 * Validate the value.
			 */
			ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST vctxt, NULL, xmlSchemaGetBuiltInType(XML_SCHEMAS_BOOLEAN), iattr->value, &(iattr->val), 1, 0, 0);
			ACTIVATE_ELEM;
			if(ret < 0) {
				VERROR_INT("xmlSchemaValidateElemDecl", "calling xmlSchemaVCheckCVCSimpleType() to validate the attribute 'xsi:nil'");
				return -1;
			}
			if(ret == 0) {
				if((elemDecl->flags & XML_SCHEMAS_ELEM_NILLABLE) == 0) {
					/*
					 * cvc-elt (3.3.4) : 3.1
					 */
					VERROR(XML_SCHEMAV_CVC_ELT_3_1, NULL, "The element is not 'nillable'");
					/* Does not return an error on purpose. */
				}
				else {
					if(xmlSchemaValueGetAsBoolean(iattr->val)) {
						/*
						 * cvc-elt (3.3.4) : 3.2.2
						 */
						if((elemDecl->flags & XML_SCHEMAS_ELEM_FIXED) && (elemDecl->value != NULL)) {
							VERROR(XML_SCHEMAV_CVC_ELT_3_2_2, NULL, "The element cannot be 'nilled' because there is a fixed value constraint defined for it");
							/* Does not return an error on purpose. */
						}
						else
							vctxt->inode->flags |= XML_SCHEMA_ELEM_INFO_NILLED;
					}
				}
			}
		}
		/*
		 * cvc-elt (3.3.4) : 4
		 * Handle 'xsi:type'.
		 */
		iattr = xmlSchemaGetMetaAttrInfo(vctxt, XML_SCHEMA_ATTR_INFO_META_XSI_TYPE);
		if(iattr) {
			xmlSchemaTypePtr localType = NULL;
			ret = xmlSchemaProcessXSIType(vctxt, iattr, &localType, elemDecl);
			if(ret != 0) {
				if(ret == -1) {
					VERROR_INT("xmlSchemaValidateElemDecl", "calling xmlSchemaProcessXSIType() to process the attribute 'xsi:type'");
					return -1;
				}
				/* Does not return an error on purpose. */
			}
			if(localType != NULL) {
				vctxt->inode->flags |= XML_SCHEMA_ELEM_INFO_LOCAL_TYPE;
				actualType = localType;
			}
		}
	}
	/*
	 * IDC: Register identity-constraint XPath matchers.
	 */
	if((elemDecl->idcs != NULL) && (xmlSchemaIDCRegisterMatchers(vctxt, elemDecl) == -1))
		return -1;
	/*
	 * No actual type definition.
	 */
	if(actualType == NULL) {
		VERROR(XML_SCHEMAV_CVC_TYPE_1, NULL, "The type definition is absent");
		return (XML_SCHEMAV_CVC_TYPE_1);
	}
	/*
	 * Remember the actual type definition.
	 */
	vctxt->inode->typeDef = actualType;
	return 0;
}

static int xmlSchemaVAttributesSimple(xmlSchemaValidCtxtPtr vctxt)
{
	int ret = 0;
	/*
	 * SPEC cvc-type (3.1.1)
	 * "The attributes of must be empty, excepting those whose namespace
	 * name is identical to http://www.w3.org/2001/XMLSchema-instance and
	 * whose local name is one of type, nil, schemaLocation or
	 * noNamespaceSchemaLocation."
	 */
	if(vctxt->nbAttrInfos) {
		for(int i = 0; i < vctxt->nbAttrInfos; i++) {
			xmlSchemaAttrInfoPtr iattr = vctxt->attrInfos[i];
			if(!iattr->metaType) {
				ACTIVATE_ATTRIBUTE(iattr)
				xmlSchemaIllegalAttrErr(ACTXT_CAST vctxt, XML_SCHEMAV_CVC_TYPE_3_1_1, iattr, 0);
				ret = XML_SCHEMAV_CVC_TYPE_3_1_1;
			}
		}
		ACTIVATE_ELEM
	}
	return ret;
}

/*
 * Cleanup currently used attribute infos.
 */
static void xmlSchemaClearAttrInfos(xmlSchemaValidCtxtPtr vctxt)
{
	if(vctxt->nbAttrInfos) {
		for(int i = 0; i < vctxt->nbAttrInfos; i++) {
			xmlSchemaAttrInfoPtr attr = vctxt->attrInfos[i];
			if(attr->flags & XML_SCHEMA_NODE_INFO_FLAG_OWNED_NAMES) {
				SAlloc::F((xmlChar*)attr->localName);
				SAlloc::F((xmlChar*)attr->nsName);
			}
			if(attr->flags & XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES) {
				SAlloc::F((xmlChar*)attr->value);
			}
			xmlSchemaFreeValue(attr->val);
			attr->val = NULL;
			memzero(attr, sizeof(xmlSchemaAttrInfo));
		}
		vctxt->nbAttrInfos = 0;
	}
}

/*
 * 3.4.4 Complex Type Definition Validation Rules
 *   Element Locally Valid (Complex Type) (cvc-complex-type)
 * 3.2.4 Attribute Declaration Validation Rules
 *   Validation Rule: Attribute Locally Valid (cvc-attribute)
 *   Attribute Locally Valid (Use) (cvc-au)
 *
 * Only "assessed" attribute information items will be visible to
 * IDCs. I.e. not "lax" (without declaration) and "skip" wild attributes.
 */
static int xmlSchemaVAttributesComplex(xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaTypePtr type = vctxt->inode->typeDef;
	xmlSchemaItemListPtr attrUseList;
	xmlSchemaAttributeUsePtr attrUse = NULL;
	xmlSchemaAttributePtr attrDecl = NULL;
	xmlSchemaAttrInfoPtr iattr, tmpiattr;
	int i, j, found, nbAttrs, nbUses;
	int xpathRes = 0, res, wildIDs = 0, fixed;
	xmlNode * defAttrOwnerElem = NULL;
	/*
	 * SPEC (cvc-attribute)
	 * (1) "The declaration must not be `absent` (see Missing
	 * Sub-components ($5.3) for how this can fail to be
	 * the case)."
	 * (2) "Its {type definition} must not be absent."
	 *
	 * NOTE (1) + (2): This is not handled here, since we currently do not
	 * allow validation against schemas which have missing sub-components.
	 *
	 * SPEC (cvc-complex-type)
	 * (3) "For each attribute information item in the element information
	 * item's [attributes] excepting those whose [namespace name] is
	 * identical to http://www.w3.org/2001/XMLSchema-instance and whose
	 * [local name] is one of type, nil, schemaLocation or
	 * noNamespaceSchemaLocation, the appropriate case among the following
	 * must be true:
	 *
	 */
	attrUseList = (xmlSchemaItemListPtr)type->attrUses;
	/*
	 * @nbAttrs is the number of attributes present in the instance.
	 */
	nbAttrs = vctxt->nbAttrInfos;
	if(attrUseList != NULL)
		nbUses = attrUseList->nbItems;
	else
		nbUses = 0;
	for(i = 0; i < nbUses; i++) {
		found = 0;
		attrUse = (xmlSchemaAttributeUsePtr)attrUseList->items[i];
		attrDecl = WXS_ATTRUSE_DECL(attrUse);
		for(j = 0; j < nbAttrs; j++) {
			iattr = vctxt->attrInfos[j];
			/*
			 * SPEC (cvc-complex-type) (3)
			 * Skip meta attributes.
			 */
			if(iattr->metaType)
				continue;
			if(iattr->localName[0] != attrDecl->name[0])
				continue;
			if(!sstreq(iattr->localName, attrDecl->name))
				continue;
			if(!sstreq(iattr->nsName, attrDecl->targetNamespace))
				continue;
			found = 1;
			/*
			 * SPEC (cvc-complex-type)
			 * (3.1) "If there is among the {attribute uses} an attribute
			 * use with an {attribute declaration} whose {name} matches
			 * the attribute information item's [local name] and whose
			 * {target namespace} is identical to the attribute information
			 * item's [namespace name] (where an `absent` {target namespace}
			 * is taken to be identical to a [namespace name] with no value),
			 * then the attribute information must be `valid` with respect
			 * to that attribute use as per Attribute Locally Valid (Use)
			 * ($3.5.4). In this case the {attribute declaration} of that
			 * attribute use is the `context-determined declaration` for the
			 * attribute information item with respect to Schema-Validity
			 * Assessment (Attribute) ($3.2.4) and
			 * Assessment Outcome (Attribute) ($3.2.5).
			 */
			iattr->state = XML_SCHEMAS_ATTR_ASSESSED;
			iattr->use = attrUse;
			/*
			 * Context-determined declaration.
			 */
			iattr->decl = attrDecl;
			iattr->typeDef = attrDecl->subtypes;
			break;
		}
		if(found)
			continue;
		if(attrUse->occurs == XML_SCHEMAS_ATTR_USE_REQUIRED) {
			/*
			 * Handle non-existent, required attributes.
			 *
			 * SPEC (cvc-complex-type)
			 * (4) "The {attribute declaration} of each attribute use in
			 * the {attribute uses} whose {required} is true matches one
			 * of the attribute information items in the element information
			 * item's [attributes] as per clause 3.1 above."
			 */
			tmpiattr = xmlSchemaGetFreshAttrInfo(vctxt);
			if(tmpiattr == NULL) {
				VERROR_INT("xmlSchemaVAttributesComplex", "calling xmlSchemaGetFreshAttrInfo()");
				return -1;
			}
			tmpiattr->state = XML_SCHEMAS_ATTR_ERR_MISSING;
			tmpiattr->use = attrUse;
			tmpiattr->decl = attrDecl;
		}
		else if((attrUse->occurs == XML_SCHEMAS_ATTR_USE_OPTIONAL) && ((attrUse->defValue != NULL) || (attrDecl->defValue != NULL))) {
			/*
			 * Handle non-existent, optional, default/fixed attributes.
			 */
			tmpiattr = xmlSchemaGetFreshAttrInfo(vctxt);
			if(tmpiattr == NULL) {
				VERROR_INT("xmlSchemaVAttributesComplex", "calling xmlSchemaGetFreshAttrInfo()");
				return -1;
			}
			tmpiattr->state = XML_SCHEMAS_ATTR_DEFAULT;
			tmpiattr->use = attrUse;
			tmpiattr->decl = attrDecl;
			tmpiattr->typeDef = attrDecl->subtypes;
			tmpiattr->localName = attrDecl->name;
			tmpiattr->nsName = attrDecl->targetNamespace;
		}
	}

	if(vctxt->nbAttrInfos == 0)
		return 0;
	/*
	 * Validate against the wildcard.
	 */
	if(type->attributeWildcard != NULL) {
		/*
		 * SPEC (cvc-complex-type)
		 * (3.2.1) "There must be an {attribute wildcard}."
		 */
		for(i = 0; i < nbAttrs; i++) {
			iattr = vctxt->attrInfos[i];
			/*
			 * SPEC (cvc-complex-type) (3)
			 * Skip meta attributes.
			 */
			if(iattr->state != XML_SCHEMAS_ATTR_UNKNOWN)
				continue;
			/*
			 * SPEC (cvc-complex-type)
			 * (3.2.2) "The attribute information item must be `valid` with
			 * respect to it as defined in Item Valid (Wildcard) ($3.10.4)."
			 *
			 * SPEC Item Valid (Wildcard) (cvc-wildcard)
			 * "... its [namespace name] must be `valid` with respect to
			 * the wildcard constraint, as defined in Wildcard allows
			 * Namespace Name ($3.10.4)."
			 */
			if(xmlSchemaCheckCVCWildcardNamespace(type->attributeWildcard,
				    iattr->nsName) == 0) {
				/*
				 * Handle processContents.
				 *
				 * SPEC (cvc-wildcard):
				 * processContents | context-determined declaration:
				 * "strict"          "mustFind"
				 * "lax"             "none"
				 * "skip"            "skip"
				 */
				if(type->attributeWildcard->processContents == XML_SCHEMAS_ANY_SKIP) {
					/*
					 * context-determined declaration = "skip"
					 *
					 * SPEC PSVI Assessment Outcome (Attribute)
					 * [validity] = "notKnown"
					 * [validation attempted] = "none"
					 */
					iattr->state = XML_SCHEMAS_ATTR_WILD_SKIP;
					continue;
				}
				/*
				 * Find an attribute declaration.
				 */
				iattr->decl = xmlSchemaGetAttributeDecl(vctxt->schema, iattr->localName, iattr->nsName);
				if(iattr->decl != NULL) {
					iattr->state = XML_SCHEMAS_ATTR_ASSESSED;
					/*
					 * SPEC (cvc-complex-type)
					 * (5) "Let [Definition:]  the wild IDs be the set of
					 * all attribute information item to which clause 3.2
					 * applied and whose `validation` resulted in a
					 * `context-determined declaration` of mustFind or no
					 * `context-determined declaration` at all, and whose
					 * [local name] and [namespace name] resolve (as
					 * defined by QName resolution (Instance) ($3.15.4)) to
					 * an attribute declaration whose {type definition} is
					 * or is derived from ID. Then all of the following
					 * must be true:"
					 */
					iattr->typeDef = WXS_ATTR_TYPEDEF(iattr->decl);
					if(xmlSchemaIsDerivedFromBuiltInType(iattr->typeDef, XML_SCHEMAS_ID)) {
						/*
						 * SPEC (5.1) "There must be no more than one
						 * item in `wild IDs`."
						 */
						if(wildIDs != 0) {
							/* VAL TODO */
							iattr->state = XML_SCHEMAS_ATTR_ERR_WILD_DUPLICATE_ID;
							TODO
							continue;
						}
						wildIDs++;
						/*
						 * SPEC (cvc-complex-type)
						 * (5.2) "If `wild IDs` is non-empty, there must not
						 * be any attribute uses among the {attribute uses}
						 * whose {attribute declaration}'s {type definition}
						 * is or is derived from ID."
						 */
						if(attrUseList != NULL) {
							for(j = 0; j < attrUseList->nbItems; j++) {
								if(xmlSchemaIsDerivedFromBuiltInType(WXS_ATTRUSE_TYPEDEF(attrUseList->items[j]), XML_SCHEMAS_ID)) {
									/* URGENT VAL TODO: implement */
									iattr->state = XML_SCHEMAS_ATTR_ERR_WILD_AND_USE_ID;
									TODO
									break;
								}
							}
						}
					}
				}
				else if(type->attributeWildcard->processContents ==
				    XML_SCHEMAS_ANY_LAX) {
					iattr->state = XML_SCHEMAS_ATTR_WILD_LAX_NO_DECL;
					/*
					 * SPEC PSVI Assessment Outcome (Attribute)
					 * [validity] = "notKnown"
					 * [validation attempted] = "none"
					 */
				}
				else {
					iattr->state = XML_SCHEMAS_ATTR_ERR_WILD_STRICT_NO_DECL;
				}
			}
		}
	}
	if(vctxt->nbAttrInfos == 0)
		return 0;

	/*
	 * Get the owner element; needed for creation of default attributes.
	 * This fixes bug #341337, reported by David Grohmann.
	 */
	if(vctxt->options & XML_SCHEMA_VAL_VC_I_CREATE) {
		xmlSchemaNodeInfoPtr ielem = vctxt->elemInfos[vctxt->depth];
		if(ielem && ielem->P_Node && ielem->P_Node->doc)
			defAttrOwnerElem = ielem->P_Node;
	}
	/*
	 * Validate values, create default attributes, evaluate IDCs.
	 */
	for(i = 0; i < vctxt->nbAttrInfos; i++) {
		iattr = vctxt->attrInfos[i];
		/*
		 * VAL TODO: Note that we won't try to resolve IDCs to
		 * "lax" and "skip" validated attributes. Check what to
		 * do in this case.
		 */
		if((iattr->state != XML_SCHEMAS_ATTR_ASSESSED) && (iattr->state != XML_SCHEMAS_ATTR_DEFAULT))
			continue;
		/*
		 * VAL TODO: What to do if the type definition is missing?
		 */
		if(iattr->typeDef == NULL) {
			iattr->state = XML_SCHEMAS_ATTR_ERR_NO_TYPE;
			continue;
		}
		ACTIVATE_ATTRIBUTE(iattr);
		fixed = 0;
		xpathRes = 0;
		if(vctxt->xpathStates != NULL) {
			/*
			 * Evaluate IDCs.
			 */
			xpathRes = xmlSchemaXPathEvaluate(vctxt, XML_ATTRIBUTE_NODE);
			if(xpathRes == -1) {
				VERROR_INT("xmlSchemaVAttributesComplex", "calling xmlSchemaXPathEvaluate()");
				goto internal_error;
			}
		}

		if(iattr->state == XML_SCHEMAS_ATTR_DEFAULT) {
			/*
			 * Default/fixed attributes.
			 * We need the value only if we need to resolve IDCs or
			 * will create default attributes.
			 */
			if((xpathRes) || (defAttrOwnerElem)) {
				if(iattr->use->defValue != NULL) {
					iattr->value = (xmlChar*)iattr->use->defValue;
					iattr->val = iattr->use->defVal;
				}
				else {
					iattr->value = (xmlChar*)iattr->decl->defValue;
					iattr->val = iattr->decl->defVal;
				}
				/*
				 * IDCs will consume the precomputed default value,
				 * so we need to clone it.
				 */
				if(iattr->val == NULL) {
					VERROR_INT("xmlSchemaVAttributesComplex", "default/fixed value on an attribute use was not precomputed");
					goto internal_error;
				}
				iattr->val = xmlSchemaCopyValue(iattr->val);
				if(iattr->val == NULL) {
					VERROR_INT("xmlSchemaVAttributesComplex", "calling xmlSchemaCopyValue()");
					goto internal_error;
				}
			}
			/*
			 * PSVI: Add the default attribute to the current element.
			 * VAL TODO: Should we use the *normalized* value? This currently
			 *   uses the *initial* value.
			 */

			if(defAttrOwnerElem) {
				const xmlChar * value = iattr->value;
				/*
				 * Normalize the value.
				 */
				xmlChar * normValue = xmlSchemaNormalizeValue(iattr->typeDef, iattr->value);
				if(normValue != NULL)
					value = BAD_CAST normValue;
				if(iattr->nsName == NULL) {
					if(xmlNewProp(defAttrOwnerElem, iattr->localName, value) == NULL) {
						VERROR_INT("xmlSchemaVAttributesComplex", "callling xmlNewProp()");
						SAlloc::F(normValue);
						goto internal_error;
					}
				}
				else {
					xmlNs * ns = xmlSearchNsByHref(defAttrOwnerElem->doc, defAttrOwnerElem, iattr->nsName);
					if(ns == NULL) {
						xmlChar prefix[12];
						int counter = 0;
						/*
						 * Create a namespace declaration on the validation
						 * root node if no namespace declaration is in scope.
						 */
						do {
							snprintf((char*)prefix, 12, "p%d", counter++);
							ns = xmlSearchNs(defAttrOwnerElem->doc, defAttrOwnerElem, BAD_CAST prefix);
							if(counter > 1000) {
								VERROR_INT("xmlSchemaVAttributesComplex", "could not compute a ns prefix for a default/fixed attribute");
								SAlloc::F(normValue);
								goto internal_error;
							}
						} while(ns != NULL);
						ns = xmlNewNs(vctxt->validationRoot, iattr->nsName, BAD_CAST prefix);
					}
					/*
					 * @todo 
					 *
					 *http://lists.w3.org/Archives/Public/www-xml-schema-comments/2005JulSep/0406.html
					 * If we have QNames: do we need to ensure there's a
					 * prefix defined for the QName?
					 */
					xmlNewNsProp(defAttrOwnerElem, ns, iattr->localName, value);
				}
				SAlloc::F(normValue);
			}
			/*
			 * Go directly to IDC evaluation.
			 */
			goto eval_idcs;
		}
		/*
		 * Validate the value.
		 */
		/*
			* Free last computed value; just for safety reasons.
			*/
		xmlSchemaFreeValue(vctxt->value);
		vctxt->value = NULL;
		/*
		 * Note that the attribute *use* can be unavailable, if
		 * the attribute was a wild attribute.
		 */
		if((iattr->decl->flags & XML_SCHEMAS_ATTR_FIXED) || ((iattr->use != NULL) && (iattr->use->flags & XML_SCHEMAS_ATTR_FIXED)))
			fixed = 1;
		else
			fixed = 0;
		/*
		 * SPEC (cvc-attribute)
		 * (3) "The item's `normalized value` must be locally `valid`
		 * with respect to that {type definition} as per
		 * String Valid ($3.14.4)."
		 *
		 * VAL TODO: Do we already have the
		 * "normalized attribute value" here?
		 */
		if(xpathRes || fixed) {
			iattr->flags |= XML_SCHEMA_NODE_INFO_VALUE_NEEDED;
			/*
			 * Request a computed value.
			 */
			res = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST vctxt, iattr->P_Node, iattr->typeDef, iattr->value, &(iattr->val), 1, 1, 0);
		}
		else {
			res = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST vctxt, iattr->P_Node, iattr->typeDef, iattr->value, NULL, 1, 0, 0);
		}

		if(res != 0) {
			if(res == -1) {
				VERROR_INT("xmlSchemaVAttributesComplex", "calling xmlSchemaStreamValidateSimpleTypeValue()");
				goto internal_error;
			}
			iattr->state = XML_SCHEMAS_ATTR_INVALID_VALUE;
			/*
			 * SPEC PSVI Assessment Outcome (Attribute)
			 * [validity] = "invalid"
			 */
			goto eval_idcs;
		}

		if(fixed) {
			/*
			 * SPEC Attribute Locally Valid (Use) (cvc-au)
			 * "For an attribute information item to be `valid`
			 * with respect to an attribute use its *normalized*
			 * value must match the *canonical* lexical
			 * representation of the attribute use's {value
			 * constraint}value, if it is present and fixed."
			 *
			 * VAL TODO: The requirement for the *canonical* value
			 * will be removed in XML Schema 1.1.
			 */
			/*
			 * SPEC Attribute Locally Valid (cvc-attribute)
			 * (4) "The item's *actual* value must match the *value* of
			 * the {value constraint}, if it is present and fixed."
			 */
			if(iattr->val == NULL) {
				/* VAL TODO: A value was not precomputed. */
				TODO
				goto eval_idcs;
			}
			if(iattr->use && iattr->use->defValue) {
				if(iattr->use->defVal == NULL) {
					/* VAL TODO: A default value was not precomputed. */
					TODO
					goto eval_idcs;
				}
				iattr->vcValue = iattr->use->defValue;
				/*
				   if (xmlSchemaCompareValuesWhtsp(attr->val,
				    (xmlSchemaWhitespaceValueType) ws,
				    attr->use->defVal,
				    (xmlSchemaWhitespaceValueType) ws) != 0) {
				 */
				if(!xmlSchemaAreValuesEqual(iattr->val, iattr->use->defVal))
					iattr->state = XML_SCHEMAS_ATTR_ERR_FIXED_VALUE;
			}
			else {
				if(iattr->decl->defVal == NULL) {
					/* VAL TODO: A default value was not precomputed. */
					TODO
					goto eval_idcs;
				}
				iattr->vcValue = iattr->decl->defValue;
				/*
				   if (xmlSchemaCompareValuesWhtsp(attr->val,
				    (xmlSchemaWhitespaceValueType) ws,
				    attrDecl->defVal,
				    (xmlSchemaWhitespaceValueType) ws) != 0) {
				 */
				if(!xmlSchemaAreValuesEqual(iattr->val, iattr->decl->defVal))
					iattr->state = XML_SCHEMAS_ATTR_ERR_FIXED_VALUE;
			}
			/*
			 * [validity] = "valid"
			 */
		}
eval_idcs:
		/*
		 * Evaluate IDCs.
		 */
		if(xpathRes) {
			if(xmlSchemaXPathProcessHistory(vctxt, vctxt->depth +1) == -1) {
				VERROR_INT("xmlSchemaVAttributesComplex", "calling xmlSchemaXPathEvaluate()");
				goto internal_error;
			}
		}
		else if(vctxt->xpathStates)
			xmlSchemaXPathPop(vctxt);
	}

	/*
	 * Report errors.
	 */
	for(i = 0; i < vctxt->nbAttrInfos; i++) {
		iattr = vctxt->attrInfos[i];
		if(oneof4(iattr->state, XML_SCHEMAS_ATTR_META, XML_SCHEMAS_ATTR_ASSESSED, XML_SCHEMAS_ATTR_WILD_SKIP, XML_SCHEMAS_ATTR_WILD_LAX_NO_DECL))
			continue;
		ACTIVATE_ATTRIBUTE(iattr);
		switch(iattr->state) {
			case XML_SCHEMAS_ATTR_ERR_MISSING: {
			    xmlChar * str = NULL;
			    ACTIVATE_ELEM;
			    xmlSchemaCustomErr(ACTXT_CAST vctxt, XML_SCHEMAV_CVC_COMPLEX_TYPE_4, NULL, NULL,
				    "The attribute '%s' is required but missing", xmlSchemaFormatQName(&str, iattr->decl->targetNamespace, iattr->decl->name), NULL);
			    ZFREE(str);
			    break;
		    }
			case XML_SCHEMAS_ATTR_ERR_NO_TYPE:
			    VERROR(XML_SCHEMAV_CVC_ATTRIBUTE_2, NULL, "The type definition is absent");
			    break;
			case XML_SCHEMAS_ATTR_ERR_FIXED_VALUE:
			    xmlSchemaCustomErr(ACTXT_CAST vctxt, XML_SCHEMAV_CVC_AU, NULL, NULL, "The value '%s' does not match the fixed value constraint '%s'", iattr->value, iattr->vcValue);
			    break;
			case XML_SCHEMAS_ATTR_ERR_WILD_STRICT_NO_DECL:
			    VERROR(XML_SCHEMAV_CVC_WILDCARD, NULL, "No matching global attribute declaration available, but demanded by the strict wildcard");
			    break;
			case XML_SCHEMAS_ATTR_UNKNOWN:
			    if(iattr->metaType)
				    break;
			    /*
			     * MAYBE VAL TODO: One might report different error messages
			     * for the following errors.
			     */
			    if(type->attributeWildcard == NULL) {
				    xmlSchemaIllegalAttrErr(ACTXT_CAST vctxt,
				    XML_SCHEMAV_CVC_COMPLEX_TYPE_3_2_1, iattr, 0);
			    }
			    else {
				    xmlSchemaIllegalAttrErr(ACTXT_CAST vctxt,
				    XML_SCHEMAV_CVC_COMPLEX_TYPE_3_2_2, iattr, 0);
			    }
			    break;
			default:
			    break;
		}
	}

	ACTIVATE_ELEM;
	return 0;
internal_error:
	ACTIVATE_ELEM;
	return -1;
}

static int xmlSchemaValidateElemWildcard(xmlSchemaValidCtxtPtr vctxt, int * skip)
{
	xmlSchemaWildcardPtr wild = (xmlSchemaWildcardPtr)vctxt->inode->decl;
	/*
	 * The namespace of the element was already identified to be
	 * matching the wildcard.
	 */
	if((skip == NULL) || (wild == NULL) || (wild->type != XML_SCHEMA_TYPE_ANY)) {
		VERROR_INT("xmlSchemaValidateElemWildcard", "bad arguments");
		return -1;
	}
	*skip = 0;
	if(wild->processContents == XML_SCHEMAS_ANY_SKIP) {
		/*
		 * URGENT VAL TODO: Either we need to position the stream to the
		 * next sibling, or walk the whole subtree.
		 */
		*skip = 1;
		return 0;
	}
	{
		xmlSchemaElementPtr decl = xmlSchemaGetElem(vctxt->schema, vctxt->inode->localName, vctxt->inode->nsName);
		if(decl) {
			vctxt->inode->decl = decl;
			return 0;
		}
	}
	if(wild->processContents == XML_SCHEMAS_ANY_STRICT) {
		/* VAL TODO: Change to proper error code. */
		VERROR(XML_SCHEMAV_CVC_ELT_1, NULL, /* WXS_BASIC_CAST wild */ "No matching global element declaration available, but demanded by the strict wildcard");
		return (vctxt->err);
	}
	if(vctxt->nbAttrInfos) {
		/*
		 * SPEC Validation Rule: Schema-Validity Assessment (Element)
		 * (1.2.1.2.1) - (1.2.1.2.3 )
		 *
		 * Use the xsi:type attribute for the type definition.
		 */
		xmlSchemaAttrInfoPtr iattr = xmlSchemaGetMetaAttrInfo(vctxt, XML_SCHEMA_ATTR_INFO_META_XSI_TYPE);
		if(iattr) {
			if(xmlSchemaProcessXSIType(vctxt, iattr, &(vctxt->inode->typeDef), NULL) == -1) {
				VERROR_INT("xmlSchemaValidateElemWildcard", "calling xmlSchemaProcessXSIType() to process the attribute 'xsi:nil'");
				return -1;
			}
			/*
			 * Don't return an error on purpose.
			 */
			return 0;
		}
	}
	/*
	 * SPEC Validation Rule: Schema-Validity Assessment (Element)
	 *
	 * Fallback to "anyType".
	 */
	vctxt->inode->typeDef =
	    xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYTYPE);
	return 0;
}

/*
 * xmlSchemaCheckCOSValidDefault:
 *
 * This will be called if: not nilled, no content and a default/fixed
 * value is provided.
 */

static int xmlSchemaCheckCOSValidDefault(xmlSchemaValidCtxtPtr vctxt,
    const xmlChar * value,
    xmlSchemaValPtr * val)
{
	int ret = 0;
	xmlSchemaNodeInfoPtr inode = vctxt->inode;

	/*
	 * cos-valid-default:
	 * Schema Component Constraint: Element Default Valid (Immediate)
	 * For a string to be a valid default with respect to a type
	 * definition the appropriate case among the following must be true:
	 */
	if WXS_IS_COMPLEX(inode->typeDef) {
		/*
		 * Complex type.
		 *
		 * SPEC (2.1) "its {content type} must be a simple type definition
		 * or mixed."
		 * SPEC (2.2.2) "If the {content type} is mixed, then the {content
		 * type}'s particle must be `emptiable` as defined by
		 * Particle Emptiable ($3.9.6)."
		 */
		if((!WXS_HAS_SIMPLE_CONTENT(inode->typeDef)) && ((!WXS_HAS_MIXED_CONTENT(inode->typeDef)) || (!WXS_EMPTIABLE(inode->typeDef)))) {
			ret = XML_SCHEMAP_COS_VALID_DEFAULT_2_1;
			/* NOTE that this covers (2.2.2) as well. */
			VERROR(ret, NULL, "For a string to be a valid default, the type definition must be a simple type or a complex type with simple content or mixed content and a particle emptiable");
			return ret;
		}
	}
	/*
	 * 1 If the type definition is a simple type definition, then the string
	 * must be `valid` with respect to that definition as defined by String
	 * Valid ($3.14.4).
	 *
	 * AND
	 *
	 * 2.2.1 If the {content type} is a simple type definition, then the
	 * string must be `valid` with respect to that simple type definition
	 * as defined by String Valid ($3.14.4).
	 */
	if(WXS_IS_SIMPLE(inode->typeDef)) {
		ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST vctxt, NULL, inode->typeDef, value, val, 1, 1, 0);
	}
	else if(WXS_HAS_SIMPLE_CONTENT(inode->typeDef)) {
		ret = xmlSchemaVCheckCVCSimpleType(ACTXT_CAST vctxt, NULL, inode->typeDef->contentTypeDef, value, val, 1, 1, 0);
	}
	if(ret < 0) {
		VERROR_INT("xmlSchemaCheckCOSValidDefault", "calling xmlSchemaVCheckCVCSimpleType()");
	}
	return ret;
}

static void xmlSchemaVContentModelCallback(xmlSchemaValidCtxtPtr vctxt ATTRIBUTE_UNUSED,
    const xmlChar * name ATTRIBUTE_UNUSED, xmlSchemaElementPtr item, xmlSchemaNodeInfoPtr inode)
{
	inode->decl = item;
#ifdef DEBUG_CONTENT
	{
		xmlChar * str = NULL;
		if(item->type == XML_SCHEMA_TYPE_ELEMENT) {
			xmlGenericError(0, "AUTOMATON callback for '%s' [declaration]\n", xmlSchemaFormatQName(&str, inode->localName, inode->nsName));
		}
		else {
			xmlGenericError(0, "AUTOMATON callback for '%s' [wildcard]\n", xmlSchemaFormatQName(&str, inode->localName, inode->nsName));
		}
		ZFREE(str);
	}
#endif
}

static int xmlSchemaValidatorPushElem(xmlSchemaValidCtxtPtr vctxt)
{
	vctxt->inode = xmlSchemaGetFreshElemInfo(vctxt);
	if(vctxt->inode == NULL) {
		VERROR_INT("xmlSchemaValidatorPushElem", "calling xmlSchemaGetFreshElemInfo()");
		return -1;
	}
	vctxt->nbAttrInfos = 0;
	return 0;
}

static int xmlSchemaVCheckINodeDataType(xmlSchemaValidCtxtPtr vctxt, xmlSchemaNodeInfoPtr inode, xmlSchemaTypePtr type, const xmlChar * value)
{
	if(inode->flags & XML_SCHEMA_NODE_INFO_VALUE_NEEDED)
		return (xmlSchemaVCheckCVCSimpleType(ACTXT_CAST vctxt, NULL, type, value, &(inode->val), 1, 1, 0));
	else
		return (xmlSchemaVCheckCVCSimpleType(ACTXT_CAST vctxt, NULL, type, value, NULL, 1, 0, 0));
}
/*
 * Process END of element.
 */
static int xmlSchemaValidatorPopElem(xmlSchemaValidCtxtPtr vctxt)
{
	int ret = 0;
	xmlSchemaNodeInfoPtr inode = vctxt->inode;
	if(vctxt->nbAttrInfos != 0)
		xmlSchemaClearAttrInfos(vctxt);
	if(inode->flags & XML_SCHEMA_NODE_INFO_ERR_NOT_EXPECTED) {
		/*
		 * This element was not expected;
		 * we will not validate child elements of broken parents.
		 * Skip validation of all content of the parent.
		 */
		vctxt->skipDepth = vctxt->depth -1;
		goto end_elem;
	}
	if((inode->typeDef == NULL) || (inode->flags & XML_SCHEMA_NODE_INFO_ERR_BAD_TYPE)) {
		/*
		 * 1. the type definition might be missing if the element was
		 *    error prone
		 * 2. it might be abstract.
		 */
		goto end_elem;
	}
	/*
	 * Check the content model.
	 */
	if((inode->typeDef->contentType == XML_SCHEMA_CONTENT_MIXED) || (inode->typeDef->contentType == XML_SCHEMA_CONTENT_ELEMENTS)) {
		/*
		 * Workaround for "anyType".
		 */
		if(inode->typeDef->builtInType == XML_SCHEMAS_ANYTYPE)
			goto character_content;
		if((inode->flags & XML_SCHEMA_ELEM_INFO_ERR_BAD_CONTENT) == 0) {
			xmlChar * values[10];
			int terminal, nbval = 10, nbneg;
			if(inode->regexCtxt == NULL) {
				/*
				 * Create the regex context.
				 */
				inode->regexCtxt = xmlRegNewExecCtxt(inode->typeDef->contModel,
				    (xmlRegExecCallbacks)xmlSchemaVContentModelCallback, vctxt);
				if(inode->regexCtxt == NULL) {
					VERROR_INT("xmlSchemaValidatorPopElem", "failed to create a regex context");
					goto internal_error;
				}
#ifdef DEBUG_AUTOMATA
				xmlGenericError(0, "AUTOMATON create on '%s'\n", inode->localName);
#endif
			}

			/*
			 * Do not check further content if the node has been nilled
			 */
			if(INODE_NILLED(inode)) {
				ret = 0;
#ifdef DEBUG_AUTOMATA
				xmlGenericError(0, "AUTOMATON succeeded on nilled '%s'\n", inode->localName);
#endif
				goto skip_nilled;
			}

			/*
			 * Get hold of the still expected content, since a further
			 * call to xmlRegExecPushString() will loose this information.
			 */
			xmlRegExecNextValues(inode->regexCtxt, &nbval, &nbneg, &values[0], &terminal);
			ret = xmlRegExecPushString(inode->regexCtxt, 0, 0);
			if((ret<0) || ((ret==0) && (!INODE_NILLED(inode)))) {
				/*
				 * Still missing something.
				 */
				ret = 1;
				inode->flags |= XML_SCHEMA_ELEM_INFO_ERR_BAD_CONTENT;
				xmlSchemaComplexTypeErr(ACTXT_CAST vctxt, XML_SCHEMAV_ELEMENT_CONTENT, NULL, NULL, "Missing child element(s)",
				    nbval, nbneg, values);
#ifdef DEBUG_AUTOMATA
				xmlGenericError(0, "AUTOMATON missing ERROR on '%s'\n", inode->localName);
#endif
			}
			else {
				/*
				 * Content model is satisfied.
				 */
				ret = 0;
#ifdef DEBUG_AUTOMATA
				xmlGenericError(0, "AUTOMATON succeeded on '%s'\n", inode->localName);
#endif
			}
		}
	}

skip_nilled:
	if(inode->typeDef->contentType == XML_SCHEMA_CONTENT_ELEMENTS)
		goto end_elem;
character_content:
	xmlSchemaFreeValue(vctxt->value);
	vctxt->value = NULL;
	/*
	 * Check character content.
	 */
	if(inode->decl == NULL) {
		/*
		 * Speedup if no declaration exists.
		 */
		if(WXS_IS_SIMPLE(inode->typeDef)) {
			ret = xmlSchemaVCheckINodeDataType(vctxt, inode, inode->typeDef, inode->value);
		}
		else if(WXS_HAS_SIMPLE_CONTENT(inode->typeDef)) {
			ret = xmlSchemaVCheckINodeDataType(vctxt, inode, inode->typeDef->contentTypeDef, inode->value);
		}
		if(ret < 0) {
			VERROR_INT("xmlSchemaValidatorPopElem", "calling xmlSchemaVCheckCVCSimpleType()");
			goto internal_error;
		}
		goto end_elem;
	}
	/*
	 * cvc-elt (3.3.4) : 5
	 * The appropriate case among the following must be true:
	 */
	/*
	 * cvc-elt (3.3.4) : 5.1
	 * If the declaration has a {value constraint},
	 * the item has neither element nor character [children] and
	 * clause 3.2 has not applied, then all of the following must be true:
	 */
	if(inode->decl->value && (inode->flags & XML_SCHEMA_ELEM_INFO_EMPTY) && (!INODE_NILLED(inode))) {
		/*
		 * cvc-elt (3.3.4) : 5.1.1
		 * If the `actual type definition` is a `local type definition`
		 * then the canonical lexical representation of the {value constraint}
		 * value must be a valid default for the `actual type definition` as
		 * defined in Element Default Valid (Immediate) ($3.3.6).
		 */
		/*
		 * NOTE: 'local' above means types acquired by xsi:type.
		 * NOTE: Although the *canonical* value is stated, it is not
		 * relevant if canonical or not. Additionally XML Schema 1.1
		 * will removed this requirement as well.
		 */
		if(inode->flags & XML_SCHEMA_ELEM_INFO_LOCAL_TYPE) {
			ret = xmlSchemaCheckCOSValidDefault(vctxt, inode->decl->value, &(inode->val));
			if(ret != 0) {
				if(ret < 0) {
					VERROR_INT("xmlSchemaValidatorPopElem", "calling xmlSchemaCheckCOSValidDefault()");
					goto internal_error;
				}
				goto end_elem;
			}
			/*
			 * Stop here, to avoid redundant validation of the value
			 * (see following).
			 */
			goto default_psvi;
		}
		/*
		 * cvc-elt (3.3.4) : 5.1.2
		 * The element information item with the canonical lexical
		 * representation of the {value constraint} value used as its
		 * `normalized value` must be `valid` with respect to the
		 * `actual type definition` as defined by Element Locally Valid (Type)
		 * ($3.3.4).
		 */
		if(WXS_IS_SIMPLE(inode->typeDef)) {
			ret = xmlSchemaVCheckINodeDataType(vctxt, inode, inode->typeDef, inode->decl->value);
		}
		else if(WXS_HAS_SIMPLE_CONTENT(inode->typeDef)) {
			ret = xmlSchemaVCheckINodeDataType(vctxt, inode, inode->typeDef->contentTypeDef, inode->decl->value);
		}
		if(ret != 0) {
			if(ret < 0) {
				VERROR_INT("xmlSchemaValidatorPopElem", "calling xmlSchemaVCheckCVCSimpleType()");
				goto internal_error;
			}
			goto end_elem;
		}

default_psvi:
		/*
		 * PSVI: Create a text node on the instance element.
		 */
		if((vctxt->options & XML_SCHEMA_VAL_VC_I_CREATE) && inode->P_Node) {
			xmlNode * textChild;
			xmlChar * normValue;
			/*
			 * VAL TODO: Normalize the value.
			 */
			normValue = xmlSchemaNormalizeValue(inode->typeDef, inode->decl->value);
			if(normValue) {
				textChild = xmlNewText(BAD_CAST normValue);
				SAlloc::F(normValue);
			}
			else
				textChild = xmlNewText(inode->decl->value);
			if(textChild == NULL) {
				VERROR_INT("xmlSchemaValidatorPopElem", "calling xmlNewText()");
				goto internal_error;
			}
			else
				xmlAddChild(inode->P_Node, textChild);
		}
	}
	else if(!INODE_NILLED(inode)) {
		/*
		 * 5.2.1 The element information item must be `valid` with respect
		 * to the `actual type definition` as defined by Element Locally
		 * Valid (Type) ($3.3.4).
		 */
		if(WXS_IS_SIMPLE(inode->typeDef)) {
			/*
			 * SPEC (cvc-type) (3.1)
			 * "If the type definition is a simple type definition, ..."
			 * (3.1.3) "If clause 3.2 of Element Locally Valid
			 * (Element) ($3.3.4) did not apply, then the `normalized value`
			 * must be `valid` with respect to the type definition as defined
			 * by String Valid ($3.14.4).
			 */
			ret = xmlSchemaVCheckINodeDataType(vctxt, inode, inode->typeDef, inode->value);
		}
		else if(WXS_HAS_SIMPLE_CONTENT(inode->typeDef)) {
			/*
			 * SPEC (cvc-type) (3.2) "If the type definition is a complex type
			 * definition, then the element information item must be
			 * `valid` with respect to the type definition as per
			 * Element Locally Valid (Complex Type) ($3.4.4);"
			 *
			 * SPEC (cvc-complex-type) (2.2)
			 * "If the {content type} is a simple type definition, ...
			 * the `normalized value` of the element information item is
			 * `valid` with respect to that simple type definition as
			 * defined by String Valid ($3.14.4)."
			 */
			ret = xmlSchemaVCheckINodeDataType(vctxt, inode, inode->typeDef->contentTypeDef, inode->value);
		}
		if(ret != 0) {
			if(ret < 0) {
				VERROR_INT("xmlSchemaValidatorPopElem", "calling xmlSchemaVCheckCVCSimpleType()");
				goto internal_error;
			}
			goto end_elem;
		}
		/*
		 * 5.2.2 If there is a fixed {value constraint} and clause 3.2 has
		 * not applied, all of the following must be true:
		 */
		if(inode->decl->value && (inode->decl->flags & XML_SCHEMAS_ELEM_FIXED)) {
			/*
			 * @todo We will need a computed value, when comparison is
			 * done on computed values.
			 */
			/*
			 * 5.2.2.1 The element information item must have no element
			 * information item [children].
			 */
			if(inode->flags & XML_SCHEMA_ELEM_INFO_HAS_ELEM_CONTENT) {
				ret = XML_SCHEMAV_CVC_ELT_5_2_2_1;
				VERROR(ret, NULL, "The content must not containt element nodes since there is a fixed value constraint");
				goto end_elem;
			}
			else {
				/*
				 * 5.2.2.2 The appropriate case among the following must
				 * be true:
				 */
				if(WXS_HAS_MIXED_CONTENT(inode->typeDef)) {
					/*
					 * 5.2.2.2.1 If the {content type} of the `actual type
					 * definition` is mixed, then the *initial value* of the
					 * item must match the canonical lexical representation
					 * of the {value constraint} value.
					 *
					 * ... the *initial value* of an element information
					 * item is the string composed of, in order, the
					 * [character code] of each character information item in
					 * the [children] of that element information item.
					 */
					if(!sstreq(inode->value, inode->decl->value)) {
						/*
						 * VAL TODO: Report invalid & expected values as well.
						 * VAL TODO: Implement the canonical stuff.
						 */
						ret = XML_SCHEMAV_CVC_ELT_5_2_2_2_1;
						xmlSchemaCustomErr(ACTXT_CAST vctxt, (xmlParserErrors)ret, NULL, NULL,
						    "The initial value '%s' does not match the fixed value constraint '%s'",
						    inode->value, inode->decl->value);
						goto end_elem;
					}
				}
				else if(WXS_HAS_SIMPLE_CONTENT(inode->typeDef)) {
					/*
					 * 5.2.2.2.2 If the {content type} of the `actual type
					 * definition` is a simple type definition, then the
					 * *actual value* of the item must match the canonical
					 * lexical representation of the {value constraint} value.
					 */
					/*
					 * VAL TODO: *actual value* is the normalized value, impl.
					 *           this.
					 * VAL TODO: Report invalid & expected values as well.
					 * VAL TODO: Implement a comparison with the computed values.
					 */
					if(!sstreq(inode->value, inode->decl->value)) {
						ret = XML_SCHEMAV_CVC_ELT_5_2_2_2_2;
						xmlSchemaCustomErr(ACTXT_CAST vctxt, (xmlParserErrors)ret, NULL, NULL,
						    "The actual value '%s' does not match the fixed value constraint '%s'",
						    inode->value, inode->decl->value);
						goto end_elem;
					}
				}
			}
		}
	}

end_elem:
	if(vctxt->depth < 0) {
		/* @todo raise error? */
		return 0;
	}
	if(vctxt->depth == vctxt->skipDepth)
		vctxt->skipDepth = -1;
	/*
	 * Evaluate the history of XPath state objects.
	 */
	if(inode->appliedXPath && (xmlSchemaXPathProcessHistory(vctxt, vctxt->depth) == -1))
		goto internal_error;
	/*
	 * MAYBE TODO:
	 * SPEC (6) "The element information item must be `valid` with
	 * respect to each of the {identity-constraint definitions} as per
	 * Identity-constraint Satisfied ($3.11.4)."
	 */
	/*
	 * PSVI TODO: If we expose IDC node-tables via PSVI then the tables
	 *   need to be built in any case.
	 *   We will currently build IDC node-tables and bubble them only if
	 *   keyrefs do exist.
	 */

	/*
	 * Add the current IDC target-nodes to the IDC node-tables.
	 */
	if(inode->idcMatchers && (vctxt->hasKeyrefs || vctxt->createIDCNodeTables)) {
		if(xmlSchemaIDCFillNodeTables(vctxt, inode) == -1)
			goto internal_error;
	}
	/*
	 * Validate IDC keyrefs.
	 */
	if(vctxt->inode->hasKeyrefs)
		if(xmlSchemaCheckCVCIDCKeyRef(vctxt) == -1)
			goto internal_error;
	/*
	 * Merge/free the IDC table.
	 */
	if(inode->idcTable) {
#ifdef DEBUG_IDC_NODE_TABLE
		xmlSchemaDebugDumpIDCTable(stdout,
		    inode->nsName,
		    inode->localName,
		    inode->idcTable);
#endif
		if((vctxt->depth > 0) && (vctxt->hasKeyrefs || vctxt->createIDCNodeTables)) {
			/*
			 * Merge the IDC node table with the table of the parent node.
			 */
			if(xmlSchemaBubbleIDCNodeTables(vctxt) == -1)
				goto internal_error;
		}
	}
	/*
	 * Clear the current ielem.
	 * VAL TODO: Don't free the PSVI IDC tables if they are
	 * requested for the PSVI.
	 */
	xmlSchemaClearElemInfo(vctxt, inode);
	/*
	 * Skip further processing if we are on the validation root.
	 */
	if(vctxt->depth == 0) {
		vctxt->depth--;
		vctxt->inode = NULL;
		return 0;
	}
	/*
	 * Reset the keyrefDepth if needed.
	 */
	if(vctxt->aidcs) {
		xmlSchemaIDCAug * aidc = vctxt->aidcs;
		do {
			if(aidc->keyrefDepth == vctxt->depth) {
				/*
				 * A 'keyrefDepth' of a key/unique IDC matches the current
				 * depth, this means that we are leaving the scope of the
				 * top-most keyref IDC which refers to this IDC.
				 */
				aidc->keyrefDepth = -1;
			}
			aidc = aidc->next;
		} while(aidc);
	}
	vctxt->depth--;
	vctxt->inode = vctxt->elemInfos[vctxt->depth];
	/*
	 * VAL TODO: 7 If the element information item is the `validation root`, it must be
	 * `valid` per Validation Root Valid (ID/IDREF) ($3.3.4).
	 */
	return ret;

internal_error:
	vctxt->err = -1;
	return -1;
}

/*
 * 3.4.4 Complex Type Definition Validation Rules
 * Validation Rule: Element Locally Valid (Complex Type) (cvc-complex-type)
 */
static int xmlSchemaValidateChildElem(xmlSchemaValidCtxtPtr vctxt)
{
	xmlSchemaNodeInfoPtr pielem;
	xmlSchemaTypePtr ptype;
	int ret = 0;
	if(vctxt->depth <= 0) {
		VERROR_INT("xmlSchemaValidateChildElem", "not intended for the validation root");
		return -1;
	}
	pielem = vctxt->elemInfos[vctxt->depth -1];
	if(pielem->flags & XML_SCHEMA_ELEM_INFO_EMPTY)
		pielem->flags ^= XML_SCHEMA_ELEM_INFO_EMPTY;
	/*
	 * Handle 'nilled' elements.
	 */
	if(INODE_NILLED(pielem)) {
		/*
		 * SPEC (cvc-elt) (3.3.4) : (3.2.1)
		 */
		ACTIVATE_PARENT_ELEM;
		ret = XML_SCHEMAV_CVC_ELT_3_2_1;
		VERROR(ret, NULL, "Neither character nor element content is allowed, because the element was 'nilled'");
		ACTIVATE_ELEM;
		goto unexpected_elem;
	}
	ptype = pielem->typeDef;
	if(ptype->builtInType == XML_SCHEMAS_ANYTYPE) {
		/*
		 * Workaround for "anyType": we have currently no content model
		 * assigned for "anyType", so handle it explicitely.
		 * "anyType" has an unbounded, lax "any" wildcard.
		 */
		vctxt->inode->decl = xmlSchemaGetElem(vctxt->schema, vctxt->inode->localName, vctxt->inode->nsName);
		if(vctxt->inode->decl == NULL) {
			xmlSchemaAttrInfoPtr iattr;
			/*
			 * Process "xsi:type".
			 * SPEC (cvc-assess-elt) (1.2.1.2.1) - (1.2.1.2.3)
			 */
			iattr = xmlSchemaGetMetaAttrInfo(vctxt, XML_SCHEMA_ATTR_INFO_META_XSI_TYPE);
			if(iattr) {
				ret = xmlSchemaProcessXSIType(vctxt, iattr, &(vctxt->inode->typeDef), 0);
				if(ret != 0) {
					if(ret == -1) {
						VERROR_INT("xmlSchemaValidateChildElem", "calling xmlSchemaProcessXSIType() to process the attribute 'xsi:nil'");
						return -1;
					}
					return ret;
				}
			}
			else {
				/*
				 * Fallback to "anyType".
				 *
				 * SPEC (cvc-assess-elt)
				 * "If the item cannot be `strictly assessed`, [...]
				 * an element information item's schema validity may be laxly
				 * assessed if its `context-determined declaration` is not
				 * skip by `validating` with respect to the `ur-type
				 * definition` as per Element Locally Valid (Type) ($3.3.4)."
				 */
				vctxt->inode->typeDef =
				    xmlSchemaGetBuiltInType(XML_SCHEMAS_ANYTYPE);
			}
		}
		return 0;
	}

	switch(ptype->contentType) {
		case XML_SCHEMA_CONTENT_EMPTY:
		    /*
		     * SPEC (2.1) "If the {content type} is empty, then the
		     * element information item has no character or element
		     * information item [children]."
		     */
		    ACTIVATE_PARENT_ELEM
		    ret = XML_SCHEMAV_CVC_COMPLEX_TYPE_2_1;
		    VERROR(ret, NULL, "Element content is not allowed, because the content type is empty");
		    ACTIVATE_ELEM
		    goto unexpected_elem;
		    break;

		case XML_SCHEMA_CONTENT_MIXED:
		case XML_SCHEMA_CONTENT_ELEMENTS: {
		    xmlRegExecCtxtPtr regexCtxt;
		    xmlChar * values[10];
		    int terminal, nbval = 10, nbneg;

		    /* VAL TODO: Optimized "anyType" validation.*/

		    if(ptype->contModel == NULL) {
			    VERROR_INT("xmlSchemaValidateChildElem", "type has elem content but no content model");
			    return -1;
		    }
		    /*
		     * Safety belf for evaluation if the cont. model was already
		     * examined to be invalid.
		     */
		    if(pielem->flags & XML_SCHEMA_ELEM_INFO_ERR_BAD_CONTENT) {
			    VERROR_INT("xmlSchemaValidateChildElem", "validating elem, but elem content is already invalid");
			    return -1;
		    }
		    regexCtxt = pielem->regexCtxt;
		    if(regexCtxt == NULL) {
			    /*
			     * Create the regex context.
			     */
			    regexCtxt = xmlRegNewExecCtxt(ptype->contModel, (xmlRegExecCallbacks)xmlSchemaVContentModelCallback, vctxt);
			    if(regexCtxt == NULL) {
				    VERROR_INT("xmlSchemaValidateChildElem", "failed to create a regex context");
				    return -1;
			    }
			    pielem->regexCtxt = regexCtxt;
#ifdef DEBUG_AUTOMATA
			    xmlGenericError(0, "AUTOMATA create on '%s'\n", pielem->localName);
#endif
		    }

		    /*
		     * SPEC (2.4) "If the {content type} is element-only or mixed,
		     * then the sequence of the element information item's
		     * element information item [children], if any, taken in
		     * order, is `valid` with respect to the {content type}'s
		     * particle, as defined in Element Sequence Locally Valid
		     * (Particle) ($3.9.4)."
		     */
		    ret = xmlRegExecPushString2(regexCtxt, vctxt->inode->localName, vctxt->inode->nsName, vctxt->inode);
#ifdef DEBUG_AUTOMATA
		    if(ret < 0)
			    xmlGenericError(0, "AUTOMATON push ERROR for '%s' on '%s'\n", vctxt->inode->localName, pielem->localName);
		    else
			    xmlGenericError(0, "AUTOMATON push OK for '%s' on '%s'\n", vctxt->inode->localName, pielem->localName);
#endif
		    if(vctxt->err == XML_SCHEMAV_INTERNAL) {
			    VERROR_INT("xmlSchemaValidateChildElem", "calling xmlRegExecPushString2()");
				return -1;
		    }
		    if(ret < 0) {
			    xmlRegExecErrInfo(regexCtxt, NULL, &nbval, &nbneg, &values[0], &terminal);
			    xmlSchemaComplexTypeErr(ACTXT_CAST vctxt, XML_SCHEMAV_ELEMENT_CONTENT, NULL, NULL, "This element is not expected", nbval, nbneg, values);
			    ret = vctxt->err;
			    goto unexpected_elem;
		    }
		    else
			    ret = 0;
	    }
	    break;
		case XML_SCHEMA_CONTENT_SIMPLE:
		case XML_SCHEMA_CONTENT_BASIC:
		    ACTIVATE_PARENT_ELEM
		    if(WXS_IS_COMPLEX(ptype)) {
			    /*
			     * SPEC (cvc-complex-type) (2.2)
			     * "If the {content type} is a simple type definition, then
			     * the element information item has no element information
			     * item [children], ..."
			     */
			    ret = XML_SCHEMAV_CVC_COMPLEX_TYPE_2_2;
			    VERROR(ret, NULL, "Element content is not allowed, because the content type is a simple type definition");
		    }
		    else {
			    /*
			     * SPEC (cvc-type) (3.1.2) "The element information item must
			     * have no element information item [children]."
			     */
			    ret = XML_SCHEMAV_CVC_TYPE_3_1_2;
			    VERROR(ret, NULL, "Element content is not allowed, because the type definition is simple");
		    }
		    ACTIVATE_ELEM
			ret = vctxt->err;
		    goto unexpected_elem;
		    break;

		default:
		    break;
	}
	return ret;
unexpected_elem:
	/*
	 * Pop this element and set the skipDepth to skip
	 * all further content of the parent element.
	 */
	vctxt->skipDepth = vctxt->depth;
	vctxt->inode->flags |= XML_SCHEMA_NODE_INFO_ERR_NOT_EXPECTED;
	pielem->flags |= XML_SCHEMA_ELEM_INFO_ERR_BAD_CONTENT;
	return ret;
}

#define XML_SCHEMA_PUSH_TEXT_PERSIST 1
#define XML_SCHEMA_PUSH_TEXT_CREATED 2
#define XML_SCHEMA_PUSH_TEXT_VOLATILE 3

static int xmlSchemaVPushText(xmlSchemaValidCtxtPtr vctxt, int nodeType, const xmlChar * value, int len, int mode, int * consumed)
{
	/*
	 * Unfortunately we have to duplicate the text sometimes.
	 * OPTIMIZE: Maybe we could skip it, if:
	 *   1. content type is simple
	 *   2. whitespace is "collapse"
	 *   3. it consists of whitespace only
	 *
	 * Process character content.
	 */
	ASSIGN_PTR(consumed, 0);
	if(INODE_NILLED(vctxt->inode)) {
		/*
		 * SPEC cvc-elt (3.3.4 - 3.2.1)
		 * "The element information item must have no character or
		 * element information item [children]."
		 */
		VERROR(XML_SCHEMAV_CVC_ELT_3_2_1, NULL, "Neither character nor element content is allowed because the element is 'nilled'");
		return (vctxt->err);
	}
	/*
	 * SPEC (2.1) "If the {content type} is empty, then the
	 * element information item has no character or element
	 * information item [children]."
	 */
	if(vctxt->inode->typeDef->contentType == XML_SCHEMA_CONTENT_EMPTY) {
		VERROR(XML_SCHEMAV_CVC_COMPLEX_TYPE_2_1, NULL, "Character content is not allowed, because the content type is empty");
		return (vctxt->err);
	}

	if(vctxt->inode->typeDef->contentType == XML_SCHEMA_CONTENT_ELEMENTS) {
		if((nodeType != XML_TEXT_NODE) || (!xmlSchemaIsBlank((xmlChar*)value, len))) {
			/*
			 * SPEC cvc-complex-type (2.3)
			 * "If the {content type} is element-only, then the
			 * element information item has no character information
			 * item [children] other than those whose [character
			 * code] is defined as a white space in [XML 1.0 (Second
			 * Edition)]."
			 */
			VERROR(XML_SCHEMAV_CVC_COMPLEX_TYPE_2_3, NULL, "Character content other than whitespace is not allowed because the content type is 'element-only'");
			return (vctxt->err);
		}
		return 0;
	}

	if((value == NULL) || (value[0] == 0))
		return 0;
	/*
	 * Save the value.
	 * NOTE that even if the content type is *mixed*, we need the
	 * *initial value* for default/fixed value constraints.
	 */
	if((vctxt->inode->typeDef->contentType == XML_SCHEMA_CONTENT_MIXED) && ((vctxt->inode->decl == NULL) || (vctxt->inode->decl->value == NULL)))
		return 0;

	if(vctxt->inode->value == NULL) {
		/*
		 * Set the value.
		 */
		switch(mode) {
			case XML_SCHEMA_PUSH_TEXT_PERSIST:
			    /*
			     * When working on a tree.
			     */
			    vctxt->inode->value = value;
			    break;
			case XML_SCHEMA_PUSH_TEXT_CREATED:
			    /*
			     * When working with the reader.
			     * The value will be freed by the element info.
			     */
			    vctxt->inode->value = value;
				ASSIGN_PTR(consumed, 1);
			    vctxt->inode->flags |= XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES;
			    break;
			case XML_SCHEMA_PUSH_TEXT_VOLATILE:
			    /*
			     * When working with SAX.
			     * The value will be freed by the element info.
			     */
			    if(len != -1)
				    vctxt->inode->value = BAD_CAST xmlStrndup(value, len);
			    else
				    vctxt->inode->value = BAD_CAST sstrdup(value);
			    vctxt->inode->flags |=
			    XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES;
			    break;
			default:
			    break;
		}
	}
	else {
		if(len < 0)
			len = sstrlen(value);
		/*
		 * Concat the value.
		 */
		if(vctxt->inode->flags & XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES) {
			vctxt->inode->value = BAD_CAST xmlStrncat((xmlChar*)vctxt->inode->value, value, len);
		}
		else {
			vctxt->inode->value = BAD_CAST xmlStrncatNew(vctxt->inode->value, value, len);
			vctxt->inode->flags |= XML_SCHEMA_NODE_INFO_FLAG_OWNED_VALUES;
		}
	}
	return 0;
}

static int xmlSchemaValidateElem(xmlSchemaValidCtxtPtr vctxt)
{
	int ret = 0;
	if((vctxt->skipDepth != -1) && (vctxt->depth >= vctxt->skipDepth)) {
		VERROR_INT("xmlSchemaValidateElem", "in skip-state");
		goto internal_error;
	}
	if(vctxt->xsiAssemble) {
		/*
		 * We will stop validation if there was an error during
		 * dynamic schema construction.
		 * Note that we simply set @skipDepth to 0, this could
		 * mean that a streaming document via SAX would be
		 * still read to the end but it won't be validated any more.
		 * @todo If we are sure how to stop the validation at once
		 *   for all input scenarios, then this should be changed to
		 *   instantly stop the validation.
		 */
		ret = xmlSchemaAssembleByXSI(vctxt);
		if(ret != 0) {
			if(ret == -1)
				goto internal_error;
			vctxt->skipDepth = 0;
			return ret;
		}
		/*
		 * Augment the IDC definitions for the main schema and all imported ones
		 * NOTE: main schema is the first in the imported list
		 */
		xmlHashScan(vctxt->schema->schemasImports, (xmlHashScanner)xmlSchemaAugmentImportedIDC, vctxt);
	}
	if(vctxt->depth > 0) {
		/*
		 * Validate this element against the content model
		 * of the parent.
		 */
		ret = xmlSchemaValidateChildElem(vctxt);
		if(ret != 0) {
			if(ret < 0) {
				VERROR_INT("xmlSchemaValidateElem", "calling xmlSchemaStreamValidateChildElement()");
				goto internal_error;
			}
			goto exit;
		}
		if(vctxt->depth == vctxt->skipDepth)
			goto exit;
		if((vctxt->inode->decl == NULL) && (vctxt->inode->typeDef == NULL)) {
			VERROR_INT("xmlSchemaValidateElem", "the child element was valid but neither the declaration nor the type was set");
			goto internal_error;
		}
	}
	else {
		/*
		 * Get the declaration of the validation root.
		 */
		vctxt->inode->decl = xmlSchemaGetElem(vctxt->schema, vctxt->inode->localName, vctxt->inode->nsName);
		if(!vctxt->inode->decl) {
			ret = XML_SCHEMAV_CVC_ELT_1;
			VERROR(ret, NULL, "No matching global declaration available for the validation root");
			goto exit;
		}
	}
	if(!vctxt->inode->decl)
		goto type_validation;
	if(vctxt->inode->decl->type == XML_SCHEMA_TYPE_ANY) {
		int skip;
		/*
		 * Wildcards.
		 */
		ret = xmlSchemaValidateElemWildcard(vctxt, &skip);
		if(ret != 0) {
			if(ret < 0) {
				VERROR_INT("xmlSchemaValidateElem", "calling xmlSchemaValidateElemWildcard()");
				goto internal_error;
			}
			goto exit;
		}
		if(skip) {
			vctxt->skipDepth = vctxt->depth;
			goto exit;
		}
		/*
		 * The declaration might be set by the wildcard validation,
		 * when the processContents is "lax" or "strict".
		 */
		if(vctxt->inode->decl->type != XML_SCHEMA_TYPE_ELEMENT) {
			/*
			 * Clear the "decl" field to not confuse further processing.
			 */
			vctxt->inode->decl = NULL;
			goto type_validation;
		}
	}
	/*
	 * Validate against the declaration.
	 */
	ret = xmlSchemaValidateElemDecl(vctxt);
	if(ret != 0) {
		if(ret < 0) {
			VERROR_INT("xmlSchemaValidateElem", "calling xmlSchemaValidateElemDecl()");
			goto internal_error;
		}
		goto exit;
	}
	/*
	 * Validate against the type definition.
	 */
type_validation:
	if(vctxt->inode->typeDef == NULL) {
		vctxt->inode->flags |= XML_SCHEMA_NODE_INFO_ERR_BAD_TYPE;
		ret = XML_SCHEMAV_CVC_TYPE_1;
		VERROR(ret, NULL, "The type definition is absent");
		goto exit;
	}
	if(vctxt->inode->typeDef->flags & XML_SCHEMAS_TYPE_ABSTRACT) {
		vctxt->inode->flags |= XML_SCHEMA_NODE_INFO_ERR_BAD_TYPE;
		ret = XML_SCHEMAV_CVC_TYPE_2;
		VERROR(ret, NULL, "The type definition is abstract");
		goto exit;
	}
	/*
	 * Evaluate IDCs. Do it here, since new IDC matchers are registered
	 * during validation against the declaration. This must be done
	 * _before_ attribute validation.
	 */
	if(vctxt->xpathStates) {
		ret = xmlSchemaXPathEvaluate(vctxt, XML_ELEMENT_NODE);
		vctxt->inode->appliedXPath = 1;
		if(ret == -1) {
			VERROR_INT("xmlSchemaValidateElem", "calling xmlSchemaXPathEvaluate()");
			goto internal_error;
		}
	}
	/*
	 * Validate attributes.
	 */
	if(WXS_IS_COMPLEX(vctxt->inode->typeDef)) {
		if((vctxt->nbAttrInfos != 0) || (vctxt->inode->typeDef->attrUses != NULL)) {
			ret = xmlSchemaVAttributesComplex(vctxt);
		}
	}
	else if(vctxt->nbAttrInfos != 0) {
		ret = xmlSchemaVAttributesSimple(vctxt);
	}
	/*
	 * Clear registered attributes.
	 */
	if(vctxt->nbAttrInfos != 0)
		xmlSchemaClearAttrInfos(vctxt);
	if(ret == -1) {
		VERROR_INT("xmlSchemaValidateElem", "calling attributes validation");
		goto internal_error;
	}
	/*
	 * Don't return an error if attributes are invalid on purpose.
	 */
	ret = 0;
exit:
	if(ret != 0)
		vctxt->skipDepth = vctxt->depth;
	return ret;
internal_error:
	return -1;
}

#ifdef XML_SCHEMA_READER_ENABLED
static int xmlSchemaVReaderWalk(xmlSchemaValidCtxtPtr vctxt)
{
	const int WHTSP = 13, SIGN_WHTSP = 14, END_ELEM = 15;
	int depth, nodeType, ret = 0, consumed;
	xmlSchemaNodeInfoPtr ielem;
	vctxt->depth = -1;
	ret = xmlTextReaderRead(vctxt->reader);
	/*
	 * Move to the document element.
	 */
	while(ret == 1) {
		nodeType = xmlTextReaderNodeType(vctxt->reader);
		if(nodeType == XML_ELEMENT_NODE)
			goto root_found;
		ret = xmlTextReaderRead(vctxt->reader);
	}
	goto exit;

root_found:

	do {
		depth = xmlTextReaderDepth(vctxt->reader);
		nodeType = xmlTextReaderNodeType(vctxt->reader);

		if(nodeType == XML_ELEMENT_NODE) {
			vctxt->depth++;
			if(xmlSchemaValidatorPushElem(vctxt) == -1) {
				VERROR_INT("xmlSchemaVReaderWalk", "calling xmlSchemaValidatorPushElem()");
				goto internal_error;
			}
			ielem = vctxt->inode;
			ielem->localName = xmlTextReaderLocalName(vctxt->reader);
			ielem->nsName = xmlTextReaderNamespaceUri(vctxt->reader);
			ielem->flags |= XML_SCHEMA_NODE_INFO_FLAG_OWNED_NAMES;
			/*
			 * Is the element empty?
			 */
			ret = xmlTextReaderIsEmptyElement(vctxt->reader);
			if(ret == -1) {
				VERROR_INT("xmlSchemaVReaderWalk", "calling xmlTextReaderIsEmptyElement()");
				goto internal_error;
			}
			if(ret) {
				ielem->flags |= XML_SCHEMA_ELEM_INFO_EMPTY;
			}
			/*
			 * Register attributes.
			 */
			vctxt->nbAttrInfos = 0;
			ret = xmlTextReaderMoveToFirstAttribute(vctxt->reader);
			if(ret == -1) {
				VERROR_INT("xmlSchemaVReaderWalk", "calling xmlTextReaderMoveToFirstAttribute()");
				goto internal_error;
			}
			if(ret == 1) {
				do {
					/*
					 * VAL TODO: How do we know that the reader works on a
					 * node tree, to be able to pass a node here?
					 */
					if(xmlSchemaValidatorPushAttribute(vctxt, NULL, (const xmlChar*)xmlTextReaderLocalName(vctxt->reader),
						    xmlTextReaderNamespaceUri(vctxt->reader), 1, xmlTextReaderValue(vctxt->reader), 1) == -1) {
						VERROR_INT("xmlSchemaVReaderWalk", "calling xmlSchemaValidatorPushAttribute()");
						goto internal_error;
					}
					ret = xmlTextReaderMoveToNextAttribute(vctxt->reader);
					if(ret == -1) {
						VERROR_INT("xmlSchemaVReaderWalk", "calling xmlTextReaderMoveToFirstAttribute()");
						goto internal_error;
					}
				} while(ret == 1);
				/*
				 * Back to element position.
				 */
				ret = xmlTextReaderMoveToElement(vctxt->reader);
				if(ret == -1) {
					VERROR_INT("xmlSchemaVReaderWalk", "calling xmlTextReaderMoveToElement()");
					goto internal_error;
				}
			}
			/*
			 * Validate the element.
			 */
			ret = xmlSchemaValidateElem(vctxt);
			if(ret != 0) {
				if(ret == -1) {
					VERROR_INT("xmlSchemaVReaderWalk", "calling xmlSchemaValidateElem()");
					goto internal_error;
				}
				goto exit;
			}
			if(vctxt->depth == vctxt->skipDepth) {
				int curDepth;
				/*
				 * Skip all content.
				 */
				if((ielem->flags & XML_SCHEMA_ELEM_INFO_EMPTY) == 0) {
					ret = xmlTextReaderRead(vctxt->reader);
					curDepth = xmlTextReaderDepth(vctxt->reader);
					while((ret == 1) && (curDepth != depth)) {
						ret = xmlTextReaderRead(vctxt->reader);
						curDepth = xmlTextReaderDepth(vctxt->reader);
					}
					if(ret < 0) {
						/*
						 * VAL TODO: A reader error occured; what to do here?
						 */
						ret = 1;
						goto exit;
					}
				}
				goto leave_elem;
			}
			/*
			 * READER VAL TODO: Is an END_ELEM really never called
			 * if the elem is empty?
			 */
			if(ielem->flags & XML_SCHEMA_ELEM_INFO_EMPTY)
				goto leave_elem;
		}
		else if(nodeType == END_ELEM) {
			/*
			 * Process END of element.
			 */
leave_elem:
			ret = xmlSchemaValidatorPopElem(vctxt);
			if(ret != 0) {
				if(ret < 0) {
					VERROR_INT("xmlSchemaVReaderWalk", "calling xmlSchemaValidatorPopElem()");
					goto internal_error;
				}
				goto exit;
			}
			ielem = (vctxt->depth >= 0) ? vctxt->inode : 0;
		}
		else if(oneof4(nodeType, XML_TEXT_NODE, XML_CDATA_SECTION_NODE, WHTSP, SIGN_WHTSP)) {
			/*
			 * Process character content.
			 */
			xmlChar * value;
			if((nodeType == WHTSP) || (nodeType == SIGN_WHTSP))
				nodeType = XML_TEXT_NODE;
			value = xmlTextReaderValue(vctxt->reader);
			ret = xmlSchemaVPushText(vctxt, nodeType, BAD_CAST value, -1, XML_SCHEMA_PUSH_TEXT_CREATED, &consumed);
			if(!consumed)
				SAlloc::F(value);
			if(ret == -1) {
				VERROR_INT("xmlSchemaVReaderWalk", "calling xmlSchemaVPushText()");
				goto internal_error;
			}
		}
		else if((nodeType == XML_ENTITY_NODE) || (nodeType == XML_ENTITY_REF_NODE)) {
			// VAL TODO: What to do with entities?
			TODO
		}
		/*
		 * Read next node.
		 */
		ret = xmlTextReaderRead(vctxt->reader);
	} while(ret == 1);

exit:
	return ret;
internal_error:
	return -1;
}

#endif

/************************************************************************
*									*
*			SAX validation handlers				*
*									*
************************************************************************/

/*
 * Process text content.
 */
static void xmlSchemaSAXHandleText(void * ctx, const xmlChar * ch, int len)
{
	if(ctx) {
		xmlSchemaValidCtxt * vctxt = (xmlSchemaValidCtxt *)ctx;
		if(vctxt->depth >= 0) {
			if((vctxt->skipDepth != -1) && (vctxt->depth >= vctxt->skipDepth))
				return;
			if(vctxt->inode->flags & XML_SCHEMA_ELEM_INFO_EMPTY)
				vctxt->inode->flags ^= XML_SCHEMA_ELEM_INFO_EMPTY;
			if(xmlSchemaVPushText(vctxt, XML_TEXT_NODE, ch, len, XML_SCHEMA_PUSH_TEXT_VOLATILE, NULL) == -1) {
				VERROR_INT("xmlSchemaSAXHandleCDataSection", "calling xmlSchemaVPushText()");
				vctxt->err = -1;
				xmlStopParser(vctxt->parserCtxt);
			}
		}
	}
}
/*
 * Process CDATA content.
 */
static void xmlSchemaSAXHandleCDataSection(void * ctx, const xmlChar * ch, int len)
{
	if(ctx) {
		xmlSchemaValidCtxtPtr vctxt = (xmlSchemaValidCtxtPtr)ctx;
		if(vctxt->depth >= 0) {
			if((vctxt->skipDepth != -1) && (vctxt->depth >= vctxt->skipDepth))
				return;
			if(vctxt->inode->flags & XML_SCHEMA_ELEM_INFO_EMPTY)
				vctxt->inode->flags ^= XML_SCHEMA_ELEM_INFO_EMPTY;
			if(xmlSchemaVPushText(vctxt, XML_CDATA_SECTION_NODE, ch, len, XML_SCHEMA_PUSH_TEXT_VOLATILE, NULL) == -1) {
				VERROR_INT("xmlSchemaSAXHandleCDataSection", "calling xmlSchemaVPushText()");
				vctxt->err = -1;
				xmlStopParser(vctxt->parserCtxt);
			}
		}
	}
}

static void xmlSchemaSAXHandleReference(void * ctx ATTRIBUTE_UNUSED, const xmlChar * name ATTRIBUTE_UNUSED)
{
	xmlSchemaValidCtxtPtr vctxt = (xmlSchemaValidCtxtPtr)ctx;
	if(vctxt->depth < 0)
		return;
	if((vctxt->skipDepth != -1) && (vctxt->depth >= vctxt->skipDepth))
		return;
	// SAX VAL TODO: What to do here? 
	TODO
}

static void xmlSchemaSAXHandleStartElementNs(void * ctx, const xmlChar * localname, const xmlChar * prefix ATTRIBUTE_UNUSED,
    const xmlChar * URI, int nb_namespaces, const xmlChar ** namespaces, int nb_attributes,
    int nb_defaulted ATTRIBUTE_UNUSED, const xmlChar ** attributes)
{
	xmlSchemaValidCtxtPtr vctxt = (xmlSchemaValidCtxtPtr)ctx;
	int ret;
	xmlSchemaNodeInfoPtr ielem;
	int i, j;
	/*
	 * SAX VAL TODO: What to do with nb_defaulted?
	 */
	/*
	 * Skip elements if inside a "skip" wildcard or invalid.
	 */
	vctxt->depth++;
	if((vctxt->skipDepth != -1) && (vctxt->depth >= vctxt->skipDepth))
		return;
	/*
	 * Push the element.
	 */
	if(xmlSchemaValidatorPushElem(vctxt) == -1) {
		VERROR_INT("xmlSchemaSAXHandleStartElementNs", "calling xmlSchemaValidatorPushElem()");
		goto internal_error;
	}
	ielem = vctxt->inode;
	/*
	 * @todo Is this OK?
	 */
	ielem->nodeLine = xmlSAX2GetLineNumber(vctxt->parserCtxt);
	ielem->localName = localname;
	ielem->nsName = URI;
	ielem->flags |= XML_SCHEMA_ELEM_INFO_EMPTY;
	/*
	 * Register namespaces on the elem info.
	 */
	if(nb_namespaces != 0) {
		/*
		 * Although the parser builds its own namespace list,
		 * we have no access to it, so we'll use an own one.
		 */
		for(i = 0, j = 0; i < nb_namespaces; i++, j += 2) {
			/*
			 * Store prefix and namespace name.
			 */
			if(ielem->nsBindings == NULL) {
				ielem->nsBindings = (const xmlChar**)SAlloc::M(10 * sizeof(const xmlChar *));
				if(ielem->nsBindings == NULL) {
					xmlSchemaVErrMemory(vctxt, "allocating namespace bindings for SAX validation", 0);
					goto internal_error;
				}
				ielem->nbNsBindings = 0;
				ielem->sizeNsBindings = 5;
			}
			else if(ielem->sizeNsBindings <= ielem->nbNsBindings) {
				ielem->sizeNsBindings *= 2;
				ielem->nsBindings = (const xmlChar**)SAlloc::R((void*)ielem->nsBindings, ielem->sizeNsBindings * 2 * sizeof(const xmlChar *));
				if(ielem->nsBindings == NULL) {
					xmlSchemaVErrMemory(vctxt, "re-allocating namespace bindings for SAX validation", 0);
					goto internal_error;
				}
			}
			ielem->nsBindings[ielem->nbNsBindings * 2] = namespaces[j];
			if(namespaces[j+1][0] == 0) {
				/*
				 * Handle xmlns="".
				 */
				ielem->nsBindings[ielem->nbNsBindings * 2 + 1] = NULL;
			}
			else
				ielem->nsBindings[ielem->nbNsBindings * 2 + 1] = namespaces[j+1];
			ielem->nbNsBindings++;
		}
	}
	/*
	 * Register attributes.
	 * SAX VAL TODO: We are not adding namespace declaration attributes yet.
	 */
	if(nb_attributes != 0) {
		for(j = 0, i = 0; i < nb_attributes; i++, j += 5) {
			// Duplicate the value.
			xmlChar * value = xmlStrndup(attributes[j+3], attributes[j+4] - attributes[j+3]);
			/*
			 * @todo Set the node line.
			 */
			ret = xmlSchemaValidatorPushAttribute(vctxt, NULL, ielem->nodeLine, attributes[j], attributes[j+2], 0, value, 1);
			if(ret == -1) {
				VERROR_INT("xmlSchemaSAXHandleStartElementNs", "calling xmlSchemaValidatorPushAttribute()");
				goto internal_error;
			}
		}
	}
	/*
	 * Validate the element.
	 */
	ret = xmlSchemaValidateElem(vctxt);
	if(ret != 0) {
		if(ret == -1) {
			VERROR_INT("xmlSchemaSAXHandleStartElementNs", "calling xmlSchemaValidateElem()");
			goto internal_error;
		}
		goto exit;
	}

exit:
	return;
internal_error:
	vctxt->err = -1;
	xmlStopParser(vctxt->parserCtxt);
	return;
}

static void xmlSchemaSAXHandleEndElementNs(void * ctx, const xmlChar * localname ATTRIBUTE_UNUSED, const xmlChar * prefix ATTRIBUTE_UNUSED, const xmlChar * URI ATTRIBUTE_UNUSED)
{
	xmlSchemaValidCtxtPtr vctxt = (xmlSchemaValidCtxtPtr)ctx;
	int res;
	/*
	 * Skip elements if inside a "skip" wildcard or if invalid.
	 */
	if(vctxt->skipDepth != -1) {
		if(vctxt->depth > vctxt->skipDepth) {
			vctxt->depth--;
			return;
		}
		else
			vctxt->skipDepth = -1;
	}
	/*
	 * SAX VAL TODO: Just a temporary check.
	 */
	if((!sstreq(vctxt->inode->localName, localname)) || (!sstreq(vctxt->inode->nsName, URI))) {
		VERROR_INT("xmlSchemaSAXHandleEndElementNs", "elem pop mismatch");
	}
	res = xmlSchemaValidatorPopElem(vctxt);
	if(res != 0) {
		if(res < 0) {
			VERROR_INT("xmlSchemaSAXHandleEndElementNs", "calling xmlSchemaValidatorPopElem()");
			goto internal_error;
		}
		goto exit;
	}
exit:
	return;
internal_error:
	vctxt->err = -1;
	xmlStopParser(vctxt->parserCtxt);
	return;
}

/************************************************************************
*									*
*			Validation interfaces				*
*									*
************************************************************************/

/**
 * xmlSchemaNewValidCtxt:
 * @schema:  a precompiled XML Schemas
 *
 * Create an XML Schemas validation context based on the given schema.
 *
 * Returns the validation context or NULL in case of error
 */
xmlSchemaValidCtxtPtr xmlSchemaNewValidCtxt(xmlSchemaPtr schema)
{
	xmlSchemaValidCtxt * ret = (xmlSchemaValidCtxt *)SAlloc::M(sizeof(xmlSchemaValidCtxt));
	if(!ret) {
		xmlSchemaVErrMemory(NULL, "allocating validation context", 0);
	}
	else {
		memzero(ret, sizeof(xmlSchemaValidCtxt));
		ret->type = XML_SCHEMA_CTXT_VALIDATOR;
		ret->dict = xmlDictCreate();
		ret->nodeQNames = xmlSchemaItemListCreate();
		ret->schema = schema;
	}
	return ret;
}
/**
 * xmlSchemaValidateSetFilename:
 * @vctxt: the schema validation context
 * @filename: the file name
 *
 * Workaround to provide file error reporting information when this is
 * not provided by current APIs
 */
void xmlSchemaValidateSetFilename(xmlSchemaValidCtxtPtr vctxt, const char * filename) 
{
	if(vctxt) {
		SAlloc::F(vctxt->filename);
		vctxt->filename = sstrdup(filename);
	}
}
/**
 * xmlSchemaClearValidCtxt:
 * @vctxt: the schema validation context
 *
 * Free the resources associated to the schema validation context;
 * leaves some fields alive intended for reuse of the context.
 */
static void xmlSchemaClearValidCtxt(xmlSchemaValidCtxtPtr vctxt)
{
	if(vctxt) {
		/*
		 * @todo Should we clear the flags?
		 *   Might be problematic if one reuses the context
		 *   and assumes that the options remain the same.
		 */
		vctxt->flags = 0;
		vctxt->validationRoot = NULL;
		vctxt->doc = NULL;
	#ifdef LIBXML_READER_ENABLED
		vctxt->reader = NULL;
	#endif
		vctxt->hasKeyrefs = 0;
		xmlSchemaFreeValue(vctxt->value);
		vctxt->value = NULL;
		/*
		 * Augmented IDC information.
		 */
		if(vctxt->aidcs) {
			xmlSchemaIDCAugPtr cur = vctxt->aidcs, next;
			do {
				next = cur->next;
				SAlloc::F(cur);
				cur = next;
			} while(cur);
			vctxt->aidcs = NULL;
		}
		if(vctxt->idcMatcherCache) {
			for(xmlSchemaIDCMatcher * matcher = vctxt->idcMatcherCache; matcher;) {
				xmlSchemaIDCMatcher * tmp = matcher;
				matcher = matcher->nextCached;
				xmlSchemaIDCFreeMatcherList(tmp);
			}
			vctxt->idcMatcherCache = NULL;
		}
		if(vctxt->idcNodes) {
			for(int i = 0; i < vctxt->nbIdcNodes; i++) {
				xmlSchemaPSVIIDCNode * item = vctxt->idcNodes[i];
				SAlloc::F(item->keys);
				SAlloc::F(item);
			}
			SAlloc::F(vctxt->idcNodes);
			vctxt->idcNodes = NULL;
			vctxt->nbIdcNodes = 0;
			vctxt->sizeIdcNodes = 0;
		}
		/*
		 * Note that we won't delete the XPath state pool here.
		 */
		xmlSchemaFreeIDCStateObjList(vctxt->xpathStates);
		vctxt->xpathStates = NULL;
		/*
		 * Attribute info.
		 */
		if(vctxt->nbAttrInfos != 0) {
			xmlSchemaClearAttrInfos(vctxt);
		}
		/*
		 * Element info.
		 */
		if(vctxt->elemInfos) {
			for(int i = 0; i < vctxt->sizeElemInfos; i++) {
				xmlSchemaNodeInfo * ei = vctxt->elemInfos[i];
				if(ei == NULL)
					break;
				xmlSchemaClearElemInfo(vctxt, ei);
			}
		}
		xmlSchemaItemListClear(vctxt->nodeQNames);
		/* Recreate the dict. */
		xmlDictFree(vctxt->dict);
		/*
		 * @todo Is is save to recreate it? Do we have a scenario where the user provides the dict?
		 */
		vctxt->dict = xmlDictCreate();
		ZFREE(vctxt->filename);
	}
}
/**
 * xmlSchemaFreeValidCtxt:
 * @ctxt:  the schema validation context
 *
 * Free the resources associated to the schema validation context
 */
void xmlSchemaFreeValidCtxt(xmlSchemaValidCtxtPtr ctxt)
{
	if(ctxt) {
		xmlSchemaFreeValue(ctxt->value);
		xmlSchemaFreeParserCtxt(ctxt->pctxt);
		if(ctxt->idcNodes) {
			for(int i = 0; i < ctxt->nbIdcNodes; i++) {
				xmlSchemaPSVIIDCNode * item = ctxt->idcNodes[i];
				SAlloc::F(item->keys);
				SAlloc::F(item);
			}
			SAlloc::F(ctxt->idcNodes);
		}
		if(ctxt->idcKeys) {
			for(int i = 0; i < ctxt->nbIdcKeys; i++)
				xmlSchemaIDCFreeKey(ctxt->idcKeys[i]);
			SAlloc::F(ctxt->idcKeys);
		}
		xmlSchemaFreeIDCStateObjList(ctxt->xpathStates);
		ctxt->xpathStates = NULL;
		xmlSchemaFreeIDCStateObjList(ctxt->xpathStatePool);
		ctxt->xpathStatePool = NULL;
		/*
		 * Augmented IDC information.
		 */
		if(ctxt->aidcs) {
			xmlSchemaIDCAug * cur = ctxt->aidcs;
			do {
				xmlSchemaIDCAug * next = cur->next;
				SAlloc::F(cur);
				cur = next;
			} while(cur);
		}
		if(ctxt->attrInfos) {
			int i;
			// Just a paranoid call to the cleanup. 
			if(ctxt->nbAttrInfos != 0)
				xmlSchemaClearAttrInfos(ctxt);
			for(i = 0; i < ctxt->sizeAttrInfos; i++) {
				xmlSchemaAttrInfo * attr = ctxt->attrInfos[i];
				SAlloc::F(attr);
			}
			SAlloc::F(ctxt->attrInfos);
		}
		if(ctxt->elemInfos) {
			for(int i = 0; i < ctxt->sizeElemInfos; i++) {
				xmlSchemaNodeInfo * ei = ctxt->elemInfos[i];
				if(ei == NULL)
					break;
				xmlSchemaClearElemInfo(ctxt, ei);
				SAlloc::F(ei);
			}
			SAlloc::F(ctxt->elemInfos);
		}
		xmlSchemaItemListFree(ctxt->nodeQNames);
		xmlDictFree(ctxt->dict);
		SAlloc::F(ctxt->filename);
		SAlloc::F(ctxt);
	}
}
/**
 * xmlSchemaIsValid:
 * @ctxt: the schema validation context
 *
 * Check if any error was detected during validation.
 *
 * Returns 1 if valid so far, 0 if errors were detected, and -1 in case of internal error.
 */
int xmlSchemaIsValid(xmlSchemaValidCtxtPtr ctxt)
{
	return ctxt ? (ctxt->err == 0) : -1;
}
/**
 * xmlSchemaSetValidErrors:
 * @ctxt:  a schema validation context
 * @err:  the error function
 * @warn: the warning function
 * @ctx: the functions context
 *
 * Set the error and warning callback informations
 */
void xmlSchemaSetValidErrors(xmlSchemaValidCtxtPtr ctxt, xmlSchemaValidityErrorFunc err, xmlSchemaValidityWarningFunc warn, void * ctx)
{
	if(ctxt) {
		ctxt->error = err;
		ctxt->warning = warn;
		ctxt->errCtxt = ctx;
		if(ctxt->pctxt)
			xmlSchemaSetParserErrors(ctxt->pctxt, err, warn, ctx);
	}
}
/**
 * xmlSchemaSetValidStructuredErrors:
 * @ctxt:  a schema validation context
 * @serror:  the structured error function
 * @ctx: the functions context
 *
 * Set the structured error callback
 */
void xmlSchemaSetValidStructuredErrors(xmlSchemaValidCtxtPtr ctxt, xmlStructuredErrorFunc serror, void * ctx)
{
	if(ctxt) {
		ctxt->serror = serror;
		ctxt->error = NULL;
		ctxt->warning = NULL;
		ctxt->errCtxt = ctx;
		if(ctxt->pctxt)
			xmlSchemaSetParserStructuredErrors(ctxt->pctxt, serror, ctx);
	}
}
/**
 * xmlSchemaGetValidErrors:
 * @ctxt: a XML-Schema validation context
 * @err: the error function result
 * @warn: the warning function result
 * @ctx: the functions context result
 *
 * Get the error and warning callback informations
 *
 * Returns -1 in case of error and 0 otherwise
 */
int xmlSchemaGetValidErrors(xmlSchemaValidCtxtPtr ctxt, xmlSchemaValidityErrorFunc * err, xmlSchemaValidityWarningFunc * warn, void ** ctx)
{
	if(!ctxt)
		return -1;
	ASSIGN_PTR(err, ctxt->error);
	ASSIGN_PTR(warn, ctxt->warning);
	ASSIGN_PTR(ctx, ctxt->errCtxt);
	return 0;
}
/**
 * xmlSchemaSetValidOptions:
 * @ctxt:	a schema validation context
 * @options: a combination of xmlSchemaValidOption
 *
 * Sets the options to be used during the validation.
 *
 * Returns 0 in case of success, -1 in case of an API error.
 */
int xmlSchemaSetValidOptions(xmlSchemaValidCtxtPtr ctxt, int options)
{
	if(!ctxt)
		return -1;
	else {
		// 
		// WARNING: Change the start value if adding to the xmlSchemaValidOption.
		// @todo Is there an other, more easy to maintain, way?
		// 
		for(int i = 1; i < (int)sizeof(int) * 8; i++) {
			if(options & 1<<i)
				return -1;
		}
		ctxt->options = options;
		return 0;
	}
}
/**
 * xmlSchemaValidCtxtGetOptions:
 * @ctxt: a schema validation context
 *
 * Get the validation context options.
 *
 * Returns the option combination or -1 on error.
 */
int xmlSchemaValidCtxtGetOptions(xmlSchemaValidCtxtPtr ctxt)
{
	return ctxt ? (ctxt->options) : -1;
}

static int xmlSchemaVDocWalk(xmlSchemaValidCtxtPtr vctxt)
{
	xmlAttr * attr;
	int ret = 0;
	xmlSchemaNodeInfoPtr ielem = NULL;
	xmlNode * P_Node;
	xmlNode * valRoot;
	const xmlChar * nsName;
	/* DOC VAL TODO: Move this to the start function. */
	valRoot = (vctxt->validationRoot != NULL) ? vctxt->validationRoot : xmlDocGetRootElement(vctxt->doc);
	if(valRoot == NULL) {
		/* VAL TODO: Error code? */
		VERROR(1, NULL, "The document has no document element");
		return 1;
	}
	vctxt->depth = -1;
	vctxt->validationRoot = valRoot;
	P_Node = valRoot;
	while(P_Node) {
		if((vctxt->skipDepth != -1) && (vctxt->depth >= vctxt->skipDepth))
			goto next_sibling;
		if(P_Node->type == XML_ELEMENT_NODE) {
			/*
			 * Init the node-info.
			 */
			vctxt->depth++;
			if(xmlSchemaValidatorPushElem(vctxt) == -1)
				goto internal_error;
			ielem = vctxt->inode;
			ielem->P_Node = P_Node;
			ielem->nodeLine = P_Node->line;
			ielem->localName = P_Node->name;
			if(P_Node->ns)
				ielem->nsName = P_Node->ns->href;
			ielem->flags |= XML_SCHEMA_ELEM_INFO_EMPTY;
			// 
			// Register attributes.
			// DOC VAL TODO: We do not register namespace declaration attributes yet.
			// 
			vctxt->nbAttrInfos = 0;
			if(P_Node->properties) {
				attr = P_Node->properties;
				do {
					nsName = attr->ns ? attr->ns->href : NULL;
					ret = xmlSchemaValidatorPushAttribute(vctxt, (xmlNode *)attr,
						// Note that we give it the line number of the parent element.
					    ielem->nodeLine, attr->name, nsName, 0, xmlNodeListGetString(attr->doc, attr->children, 1), 1);
					if(ret == -1) {
						VERROR_INT("xmlSchemaDocWalk", "calling xmlSchemaValidatorPushAttribute()");
						goto internal_error;
					}
					attr = attr->next;
				} while(attr);
			}
			/*
			 * Validate the element.
			 */
			ret = xmlSchemaValidateElem(vctxt);
			if(ret != 0) {
				if(ret == -1) {
					VERROR_INT("xmlSchemaDocWalk", "calling xmlSchemaValidateElem()");
					goto internal_error;
				}
				else
					goto leave_node; // Don't stop validation; just skip the content of this element.
			}
			if((vctxt->skipDepth != -1) && (vctxt->depth >= vctxt->skipDepth))
				goto leave_node;
		}
		else if(oneof2(P_Node->type, XML_TEXT_NODE, XML_CDATA_SECTION_NODE)) {
			/*
			 * Process character content.
			 */
			if(ielem && (ielem->flags & XML_SCHEMA_ELEM_INFO_EMPTY))
				ielem->flags ^= XML_SCHEMA_ELEM_INFO_EMPTY;
			ret = xmlSchemaVPushText(vctxt, P_Node->type, P_Node->content, -1, XML_SCHEMA_PUSH_TEXT_PERSIST, 0);
			if(ret < 0) {
				VERROR_INT("xmlSchemaVDocWalk", "calling xmlSchemaVPushText()");
				goto internal_error;
			}
			/*
			 * DOC VAL TODO: Should we skip further validation of the element content here?
			 */
		}
		else if(oneof2(P_Node->type, XML_ENTITY_NODE, XML_ENTITY_REF_NODE)) {
			/*
			 * DOC VAL TODO: What to do with entities?
			 */
			VERROR_INT("xmlSchemaVDocWalk", "there is at least one entity reference in the node-tree "
			    "currently being validated. Processing of entities with "
			    "this XML Schema processor is not supported (yet). Please substitute entities before validation.");
			goto internal_error;
		}
		else {
			goto leave_node;
			/*
			 * DOC VAL TODO: XInclude nodes, etc.
			 */
		}
		// Walk the doc.
		if(P_Node->children) {
			P_Node = P_Node->children;
			continue;
		}
leave_node:
		if(P_Node->type == XML_ELEMENT_NODE) {
			// Leaving the scope of an element.
			if(P_Node != vctxt->inode->P_Node) {
				VERROR_INT("xmlSchemaVDocWalk", "element position mismatch");
				goto internal_error;
			}
			ret = xmlSchemaValidatorPopElem(vctxt);
			if(ret != 0) {
				if(ret < 0) {
					VERROR_INT("xmlSchemaVDocWalk", "calling xmlSchemaValidatorPopElem()");
					goto internal_error;
				}
			}
			if(P_Node == valRoot)
				goto exit;
		}
next_sibling:
		if(P_Node->next)
			P_Node = P_Node->next;
		else {
			P_Node = P_Node->parent;
			goto leave_node;
		}
	}
exit:
	return ret;
internal_error:
	return -1;
}

static int xmlSchemaPreRun(xmlSchemaValidCtxtPtr vctxt) 
{
	/*
	 * Some initialization.
	 */
	vctxt->err = 0;
	vctxt->nberrors = 0;
	vctxt->depth = -1;
	vctxt->skipDepth = -1;
	vctxt->xsiAssemble = 0;
	vctxt->hasKeyrefs = 0;
#ifdef ENABLE_IDC_NODE_TABLES_TEST
	vctxt->createIDCNodeTables = 1;
#else
	vctxt->createIDCNodeTables = 0;
#endif
	//
	// Create a schema + parser if necessary.
	//
	if(vctxt->schema == NULL) {
		xmlSchemaParserCtxt * pctxt;
		vctxt->xsiAssemble = 1;
		// 
		// If not schema was given then we will create a schema dynamically using XSI schema locations.
		// 
		// Create the schema parser context.
		// 
		if(!vctxt->pctxt && (xmlSchemaCreatePCtxtOnVCtxt(vctxt) == -1))
			return -1;
		pctxt = vctxt->pctxt;
		pctxt->xsiAssemble = 1;
		// Create the schema.
		vctxt->schema = xmlSchemaNewSchema(pctxt);
		if(vctxt->schema == NULL)
			return -1;
		// Create the schema construction context.
		pctxt->constructor = xmlSchemaConstructionCtxtCreate(pctxt->dict);
		if(pctxt->constructor == NULL)
			return -1;
		pctxt->constructor->mainSchema = vctxt->schema;
		// Take ownership of the constructor to be able to free it.
		pctxt->ownsConstructor = 1;
	}
	/*
	 * Augment the IDC definitions for the main schema and all imported ones
	 * NOTE: main schema if the first in the imported list
	 */
	xmlHashScan(vctxt->schema->schemasImports, (xmlHashScanner)xmlSchemaAugmentImportedIDC, vctxt);
	return 0;
}

static void xmlSchemaPostRun(xmlSchemaValidCtxtPtr vctxt) 
{
	if(vctxt->xsiAssemble) {
		if(vctxt->schema) {
			xmlSchemaFree(vctxt->schema);
			vctxt->schema = NULL;
		}
	}
	xmlSchemaClearValidCtxt(vctxt);
}

static int xmlSchemaVStart(xmlSchemaValidCtxtPtr vctxt)
{
	int ret = 0;
	if(xmlSchemaPreRun(vctxt) < 0)
		return -1;
	if(vctxt->doc) {
		// Tree validation.
		ret = xmlSchemaVDocWalk(vctxt);
#ifdef LIBXML_READER_ENABLED
	}
	else if(vctxt->reader) {
		// XML Reader validation.
#ifdef XML_SCHEMA_READER_ENABLED
		ret = xmlSchemaVReaderWalk(vctxt);
#endif
#endif
	}
	else if(vctxt->sax && vctxt->parserCtxt) {
		// SAX validation.
		ret = xmlParseDocument(vctxt->parserCtxt);
	}
	else {
		VERROR_INT("xmlSchemaVStart", "no instance to validate");
		ret = -1;
	}
	xmlSchemaPostRun(vctxt);
	SETIFZ(ret, vctxt->err);
	return ret;
}
/**
 * xmlSchemaValidateOneElement:
 * @ctxt:  a schema validation context
 * @elem:  an element node
 *
 * Validate a branch of a tree, starting with the given @elem.
 *
 * Returns 0 if the element and its subtree is valid, a positive error
 * code number otherwise and -1 in case of an internal or API error.
 */
int xmlSchemaValidateOneElement(xmlSchemaValidCtxtPtr ctxt, xmlNode * elem)
{
	int    result = -1;
	if(ctxt && elem && elem->type == XML_ELEMENT_NODE) {
		if(ctxt->schema) {
			ctxt->doc = elem->doc;
			ctxt->P_Node = elem;
			ctxt->validationRoot = elem;
			result = xmlSchemaVStart(ctxt);
		}
	}
	return result;
}
/**
 * xmlSchemaValidateDoc:
 * @ctxt:  a schema validation context
 * @doc:  a parsed document tree
 *
 * Validate a document tree in memory.
 *
 * Returns 0 if the document is schemas valid, a positive error code
 *     number otherwise and -1 in case of internal or API error.
 */
int xmlSchemaValidateDoc(xmlSchemaValidCtxtPtr ctxt, xmlDocPtr doc)
{
	int    result = -1;
	if(ctxt && doc) {
		ctxt->doc = doc;
		ctxt->P_Node = xmlDocGetRootElement(doc);
		if(!ctxt->P_Node) {
			xmlSchemaCustomErr(ACTXT_CAST ctxt, XML_SCHEMAV_DOCUMENT_ELEMENT_MISSING, (xmlNode *)doc, NULL, "The document has no document element", 0, 0);
			result = ctxt->err;
		}
		else {
			ctxt->validationRoot = ctxt->P_Node;
			result = xmlSchemaVStart(ctxt);
		}
	}
	return result;
}
/************************************************************************
*									*
*		Function and data for SAX streaming API			*
*									*
************************************************************************/
//typedef struct _xmlSchemaSplitSAXData xmlSchemaSplitSAXData;

struct xmlSchemaSplitSAXData {
	xmlSAXHandler * user_sax;
	void * user_data;
	xmlSchemaValidCtxt * ctxt;
	xmlSAXHandler * schemas_sax;
};

typedef xmlSchemaSplitSAXData * xmlSchemaSplitSAXDataPtr;

#define XML_SAX_PLUG_MAGIC 0xdc43ba21

struct _xmlSchemaSAXPlug {
	uint   magic;
	// the original callbacks informations 
	xmlSAXHandler ** user_sax_ptr;
	xmlSAXHandler * user_sax;
	void ** user_data_ptr;
	void * user_data;
	// the block plugged back and validation informations 
	xmlSAXHandler schemas_sax;
	xmlSchemaValidCtxtPtr ctxt;
};

/* All those functions just bounces to the user provided SAX handlers */
static void internalSubsetSplit(void * ctx, const xmlChar * name, const xmlChar * ExternalID, const xmlChar * SystemID)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->internalSubset)
		ctxt->user_sax->internalSubset(ctxt->user_data, name, ExternalID, SystemID);
}

static int isStandaloneSplit(void * ctx)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	return (ctxt && ctxt->user_sax && ctxt->user_sax->isStandalone) ? ctxt->user_sax->isStandalone(ctxt->user_data) : 0;
}

static int hasInternalSubsetSplit(void * ctx)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	return (ctxt && ctxt->user_sax && ctxt->user_sax->hasInternalSubset) ? ctxt->user_sax->hasInternalSubset(ctxt->user_data) : 0;
}

static int hasExternalSubsetSplit(void * ctx)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	return (ctxt && ctxt->user_sax && ctxt->user_sax->hasExternalSubset) ? ctxt->user_sax->hasExternalSubset(ctxt->user_data) : 0;
}

static void externalSubsetSplit(void * ctx, const xmlChar * name, const xmlChar * ExternalID, const xmlChar * SystemID)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->externalSubset)
		ctxt->user_sax->externalSubset(ctxt->user_data, name, ExternalID, SystemID);
}

static xmlParserInputPtr resolveEntitySplit(void * ctx, const xmlChar * publicId, const xmlChar * systemId)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	return (ctxt && ctxt->user_sax && ctxt->user_sax->resolveEntity) ? ctxt->user_sax->resolveEntity(ctxt->user_data, publicId, systemId) : 0;
}

static xmlEntity * getEntitySplit(void * ctx, const xmlChar * name)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	return (ctxt && ctxt->user_sax && ctxt->user_sax->getEntity) ? ctxt->user_sax->getEntity(ctxt->user_data, name) : 0;
}

static xmlEntity * getParameterEntitySplit(void * ctx, const xmlChar * name)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	return (ctxt && ctxt->user_sax && ctxt->user_sax->getParameterEntity) ? ctxt->user_sax->getParameterEntity(ctxt->user_data, name) : 0;
}

static void entityDeclSplit(void * ctx, const xmlChar * name, int type, const xmlChar * publicId, const xmlChar * systemId, xmlChar * content)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->entityDecl)
		ctxt->user_sax->entityDecl(ctxt->user_data, name, type, publicId, systemId, content);
}

static void attributeDeclSplit(void * ctx, const xmlChar * elem, const xmlChar * name, int type, int def, const xmlChar * defaultValue, xmlEnumeration * tree)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->attributeDecl)
		ctxt->user_sax->attributeDecl(ctxt->user_data, elem, name, type, def, defaultValue, tree);
	else
		xmlFreeEnumeration(tree);
}

static void elementDeclSplit(void * ctx, const xmlChar * name, int type, xmlElementContentPtr content)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->elementDecl)
		ctxt->user_sax->elementDecl(ctxt->user_data, name, type, content);
}

static void notationDeclSplit(void * ctx, const xmlChar * name, const xmlChar * publicId, const xmlChar * systemId)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->notationDecl)
		ctxt->user_sax->notationDecl(ctxt->user_data, name, publicId, systemId);
}

static void unparsedEntityDeclSplit(void * ctx, const xmlChar * name, const xmlChar * publicId, const xmlChar * systemId, const xmlChar * notationName)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->unparsedEntityDecl)
		ctxt->user_sax->unparsedEntityDecl(ctxt->user_data, name, publicId, systemId, notationName);
}

static void setDocumentLocatorSplit(void * ctx, xmlSAXLocatorPtr loc)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->setDocumentLocator)
		ctxt->user_sax->setDocumentLocator(ctxt->user_data, loc);
}

static void startDocumentSplit(void * ctx)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->startDocument)
		ctxt->user_sax->startDocument(ctxt->user_data);
}

static void endDocumentSplit(void * ctx)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->endDocument)
		ctxt->user_sax->endDocument(ctxt->user_data);
}

static void processingInstructionSplit(void * ctx, const xmlChar * target, const xmlChar * data)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->processingInstruction)
		ctxt->user_sax->processingInstruction(ctxt->user_data, target, data);
}

static void commentSplit(void * ctx, const xmlChar * value)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->comment)
		ctxt->user_sax->comment(ctxt->user_data, value);
}
/*
 * Varargs error callbacks to the user application, harder ...
 */
static void XMLCDECL warningSplit(void * ctx, const char * msg ATTRIBUTE_UNUSED, ...) 
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->warning) {
		TODO
	}
}

static void XMLCDECL errorSplit(void * ctx, const char * msg ATTRIBUTE_UNUSED, ...)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->error) {
		TODO
	}
}

static void XMLCDECL fatalErrorSplit(void * ctx, const char * msg ATTRIBUTE_UNUSED, ...)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt && ctxt->user_sax && ctxt->user_sax->fatalError) {
		TODO
	}
}
/*
 * Those are function where both the user handler and the schemas handler
 * need to be called.
 */
static void charactersSplit(void * ctx, const xmlChar * ch, int len)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt) {
		if(ctxt->user_sax && ctxt->user_sax->characters)
			ctxt->user_sax->characters(ctxt->user_data, ch, len);
		xmlSchemaSAXHandleText(ctxt->ctxt, ch, len);
	}
}

static void ignorableWhitespaceSplit(void * ctx, const xmlChar * ch, int len)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt) {
		if(ctxt->user_sax && ctxt->user_sax->ignorableWhitespace)
			ctxt->user_sax->ignorableWhitespace(ctxt->user_data, ch, len);
		xmlSchemaSAXHandleText(ctxt->ctxt, ch, len);
	}
}

static void cdataBlockSplit(void * ctx, const xmlChar * value, int len)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt) {
		if(ctxt->user_sax && ctxt->user_sax->cdataBlock)
			ctxt->user_sax->cdataBlock(ctxt->user_data, value, len);
		xmlSchemaSAXHandleCDataSection(ctxt->ctxt, value, len);
	}
}

static void referenceSplit(void * ctx, const xmlChar * name)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt) {
		if(ctxt->user_sax && ctxt->user_sax->reference)
			ctxt->user_sax->reference(ctxt->user_data, name);
		if(ctxt->ctxt)
			xmlSchemaSAXHandleReference(ctxt->user_data, name);
	}
}

static void startElementNsSplit(void * ctx, const xmlChar * localname, const xmlChar * prefix, const xmlChar * URI,
    int nb_namespaces, const xmlChar ** namespaces, int nb_attributes, int nb_defaulted, const xmlChar ** attributes)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt) {
		if(ctxt->user_sax && ctxt->user_sax->startElementNs)
			ctxt->user_sax->startElementNs(ctxt->user_data, localname, prefix, URI, nb_namespaces, namespaces, nb_attributes, nb_defaulted, attributes);
		if(ctxt->ctxt)
			xmlSchemaSAXHandleStartElementNs(ctxt->ctxt, localname, prefix, URI, nb_namespaces, namespaces, nb_attributes, nb_defaulted, attributes);
	}
}

static void endElementNsSplit(void * ctx, const xmlChar * localname, const xmlChar * prefix, const xmlChar * URI)
{
	xmlSchemaSAXPlugPtr ctxt = (xmlSchemaSAXPlugPtr)ctx;
	if(ctxt) {
		if(ctxt->user_sax && ctxt->user_sax->endElementNs)
			ctxt->user_sax->endElementNs(ctxt->user_data, localname, prefix, URI);
		if(ctxt->ctxt)
			xmlSchemaSAXHandleEndElementNs(ctxt->ctxt, localname, prefix, URI);
	}
}
/**
 * xmlSchemaSAXPlug:
 * @ctxt:  a schema validation context
 * @sax:  a pointer to the original xmlSAXHandlerPtr
 * @user_data:  a pointer to the original SAX user data pointer
 *
 * Plug a SAX based validation layer in a SAX parsing event flow.
 * The original @saxptr and @dataptr data are replaced by new pointers
 * but the calls to the original will be maintained.
 *
 * Returns a pointer to a data structure needed to unplug the validation layer
 *         or NULL in case of errors.
 */
xmlSchemaSAXPlugPtr xmlSchemaSAXPlug(xmlSchemaValidCtxtPtr ctxt, xmlSAXHandlerPtr * sax, void ** user_data)
{
	xmlSchemaSAXPlugPtr ret;
	xmlSAXHandler * old_sax;
	if(!ctxt || (sax == NULL) || (user_data == NULL))
		return 0;
	/*
	 * We only allow to plug into SAX2 event streams
	 */
	old_sax = *sax;
	if(old_sax && (old_sax->initialized != XML_SAX2_MAGIC))
		return 0;
	if(old_sax && !old_sax->startElementNs && !old_sax->endElementNs && (old_sax->startElement || old_sax->endElement))
		return 0;
	/*
	 * everything seems right allocate the local data needed for that layer
	 */
	ret = (xmlSchemaSAXPlugPtr)SAlloc::M(sizeof(xmlSchemaSAXPlugStruct));
	if(!ret) {
		return 0;
	}
	memzero(ret, sizeof(xmlSchemaSAXPlugStruct));
	ret->magic = XML_SAX_PLUG_MAGIC;
	ret->schemas_sax.initialized = XML_SAX2_MAGIC;
	ret->ctxt = ctxt;
	ret->user_sax_ptr = sax;
	ret->user_sax = old_sax;
	if(old_sax == NULL) {
		/*
		 * go direct, no need for the split block and functions.
		 */
		ret->schemas_sax.startElementNs = xmlSchemaSAXHandleStartElementNs;
		ret->schemas_sax.endElementNs = xmlSchemaSAXHandleEndElementNs;
		/*
		 * Note that we use the same text-function for both, to prevent
		 * the parser from testing for ignorable whitespace.
		 */
		ret->schemas_sax.ignorableWhitespace = xmlSchemaSAXHandleText;
		ret->schemas_sax.characters = xmlSchemaSAXHandleText;
		ret->schemas_sax.cdataBlock = xmlSchemaSAXHandleCDataSection;
		ret->schemas_sax.reference = xmlSchemaSAXHandleReference;
		ret->user_data = ctxt;
		*user_data = ctxt;
	}
	else {
		// 
		// for each callback unused by Schemas initialize it to the Split
		// routine only if non NULL in the user block, this can speed up things at the SAX level.
		// 
		if(old_sax->internalSubset)
			ret->schemas_sax.internalSubset = internalSubsetSplit;
		if(old_sax->isStandalone)
			ret->schemas_sax.isStandalone = isStandaloneSplit;
		if(old_sax->hasInternalSubset)
			ret->schemas_sax.hasInternalSubset = hasInternalSubsetSplit;
		if(old_sax->hasExternalSubset)
			ret->schemas_sax.hasExternalSubset = hasExternalSubsetSplit;
		if(old_sax->resolveEntity)
			ret->schemas_sax.resolveEntity = resolveEntitySplit;
		if(old_sax->getEntity)
			ret->schemas_sax.getEntity = getEntitySplit;
		if(old_sax->entityDecl)
			ret->schemas_sax.entityDecl = entityDeclSplit;
		if(old_sax->notationDecl)
			ret->schemas_sax.notationDecl = notationDeclSplit;
		if(old_sax->attributeDecl)
			ret->schemas_sax.attributeDecl = attributeDeclSplit;
		if(old_sax->elementDecl)
			ret->schemas_sax.elementDecl = elementDeclSplit;
		if(old_sax->unparsedEntityDecl)
			ret->schemas_sax.unparsedEntityDecl = unparsedEntityDeclSplit;
		if(old_sax->setDocumentLocator)
			ret->schemas_sax.setDocumentLocator = setDocumentLocatorSplit;
		if(old_sax->startDocument)
			ret->schemas_sax.startDocument = startDocumentSplit;
		if(old_sax->endDocument)
			ret->schemas_sax.endDocument = endDocumentSplit;
		if(old_sax->processingInstruction)
			ret->schemas_sax.processingInstruction = processingInstructionSplit;
		if(old_sax->comment)
			ret->schemas_sax.comment = commentSplit;
		if(old_sax->warning)
			ret->schemas_sax.warning = warningSplit;
		if(old_sax->error)
			ret->schemas_sax.error = errorSplit;
		if(old_sax->fatalError)
			ret->schemas_sax.fatalError = fatalErrorSplit;
		if(old_sax->getParameterEntity)
			ret->schemas_sax.getParameterEntity = getParameterEntitySplit;
		if(old_sax->externalSubset)
			ret->schemas_sax.externalSubset = externalSubsetSplit;
		// 
		// the 6 schemas callback have to go to the splitter functions
		// Note that we use the same text-function for ignorableWhitespace
		// if possible, to prevent the parser from testing for ignorable whitespace.
		// 
		ret->schemas_sax.characters = charactersSplit;
		ret->schemas_sax.ignorableWhitespace = (old_sax->ignorableWhitespace && old_sax->ignorableWhitespace != old_sax->characters) ?
			ignorableWhitespaceSplit : charactersSplit;
		ret->schemas_sax.cdataBlock = cdataBlockSplit;
		ret->schemas_sax.reference = referenceSplit;
		ret->schemas_sax.startElementNs = startElementNsSplit;
		ret->schemas_sax.endElementNs = endElementNsSplit;
		ret->user_data_ptr = user_data;
		ret->user_data = *user_data;
		*user_data = ret;
	}
	/*
	 * plug the pointers back.
	 */
	*sax = &(ret->schemas_sax);
	ctxt->sax = *sax;
	ctxt->flags |= XML_SCHEMA_VALID_CTXT_FLAG_STREAM;
	xmlSchemaPreRun(ctxt);
	return ret;
}
/**
 * xmlSchemaSAXUnplug:
 * @plug:  a data structure returned by xmlSchemaSAXPlug
 *
 * Unplug a SAX based validation layer in a SAX parsing event flow.
 * The original pointers used in the call are restored.
 *
 * Returns 0 in case of success and -1 in case of failure.
 */
int xmlSchemaSAXUnplug(xmlSchemaSAXPlugPtr plug)
{
	if(!plug || plug->magic != XML_SAX_PLUG_MAGIC)
		return -1;
	else {
		xmlSAXHandler ** sax;
		plug->magic = 0;
		xmlSchemaPostRun(plug->ctxt);
		// restore the data 
		sax = plug->user_sax_ptr;
		*sax = plug->user_sax;
		if(plug->user_sax) {
			void ** user_data = plug->user_data_ptr;
			*user_data = plug->user_data;
		}
		// free and return 
		SAlloc::F(plug);
		return 0;
	}
}
/**
 * xmlSchemaValidateSetLocator:
 * @vctxt: a schema validation context
 * @f: the locator function pointer
 * @ctxt: the locator context
 *
 * Allows to set a locator function to the validation context,
 * which will be used to provide file and line information since
 * those are not provided as part of the SAX validation flow
 * Setting @f to NULL disable the locator.
 */

void xmlSchemaValidateSetLocator(xmlSchemaValidCtxtPtr vctxt, xmlSchemaValidityLocatorFunc f, void * ctxt)
{
	if(vctxt) {
		vctxt->locFunc = f;
		vctxt->locCtxt = ctxt;
	}
}

/**
 * xmlSchemaValidateStreamLocator:
 * @ctx: the xmlTextReaderPtr used
 * @file: returned file information
 * @line: returned line information
 *
 * Internal locator function for the readers
 *
 * Returns 0 in case the Schema validation could be (des)activated and
 *         -1 in case of error.
 */
static int xmlSchemaValidateStreamLocator(void * ctx, const char ** file, unsigned long * line)
{
	xmlParserCtxt * ctxt;
	if(!ctx || ((file == NULL) && (line == NULL)))
		return -1;
	ASSIGN_PTR(file, NULL);
	ASSIGN_PTR(line, 0);
	ctxt = (xmlParserCtxtPtr)ctx;
	if(ctxt->input) {
		ASSIGN_PTR(file, ctxt->input->filename);
		ASSIGN_PTR(line, ctxt->input->line);
		return 0;
	}
	return -1;
}
/**
 * xmlSchemaValidateStream:
 * @ctxt:  a schema validation context
 * @input:  the input to use for reading the data
 * @enc:  an optional encoding information
 * @sax:  a SAX handler for the resulting events
 * @user_data:  the context to provide to the SAX handler.
 *
 * Validate an input based on a flow of SAX event from the parser
 * and forward the events to the @sax handler with the provided @user_data
 * the user provided @sax handler must be a SAX2 one.
 *
 * Returns 0 if the document is schemas valid, a positive error code
 *     number otherwise and -1 in case of internal or API error.
 */
int xmlSchemaValidateStream(xmlSchemaValidCtxtPtr ctxt, xmlParserInputBufferPtr input, xmlCharEncoding enc, xmlSAXHandlerPtr sax, void * user_data)
{
	xmlSchemaSAXPlugPtr plug = NULL;
	xmlSAXHandler * old_sax = NULL;
	xmlParserCtxt * pctxt = NULL;
	xmlParserInput * inputStream = NULL;
	int ret;
	if(!ctxt || (input == NULL))
		return -1;
	/*
	 * prepare the parser
	 */
	pctxt = xmlNewParserCtxt();
	if(pctxt == NULL)
		return -1;
	old_sax = pctxt->sax;
	pctxt->sax = sax;
	pctxt->userData = user_data;
#if 0
	if(options)
		xmlCtxtUseOptions(pctxt, options);
#endif
	pctxt->linenumbers = 1;
	xmlSchemaValidateSetLocator(ctxt, xmlSchemaValidateStreamLocator, pctxt);
	inputStream = xmlNewIOInputStream(pctxt, input, enc);
	if(inputStream == NULL) {
		ret = -1;
		goto done;
	}
	inputPush(pctxt, inputStream);
	ctxt->parserCtxt = pctxt;
	ctxt->input = input;
	/*
	 * Plug the validation and launch the parsing
	 */
	plug = xmlSchemaSAXPlug(ctxt, &(pctxt->sax), &(pctxt->userData));
	if(plug == NULL) {
		ret = -1;
		goto done;
	}
	ctxt->input = input;
	ctxt->enc = enc;
	ctxt->sax = pctxt->sax;
	ctxt->flags |= XML_SCHEMA_VALID_CTXT_FLAG_STREAM;
	ret = xmlSchemaVStart(ctxt);
	if((ret == 0) && (!ctxt->parserCtxt->wellFormed)) {
		ret = ctxt->parserCtxt->errNo;
		if(ret == 0)
			ret = 1;
	}
done:
	ctxt->parserCtxt = NULL;
	ctxt->sax = NULL;
	ctxt->input = NULL;
	if(plug) {
		xmlSchemaSAXUnplug(plug);
	}
	// cleanup 
	if(pctxt) {
		pctxt->sax = old_sax;
		xmlFreeParserCtxt(pctxt);
	}
	return ret;
}

/**
 * xmlSchemaValidateFile:
 * @ctxt: a schema validation context
 * @filename: the URI of the instance
 * @options: a future set of options, currently unused
 *
 * Do a schemas validation of the given resource, it will use the
 * SAX streamable validation internally.
 *
 * Returns 0 if the document is valid, a positive error code
 *     number otherwise and -1 in case of an internal or API error.
 */
int xmlSchemaValidateFile(xmlSchemaValidCtxtPtr ctxt, const char * filename, int options ATTRIBUTE_UNUSED)
{
	int ret;
	xmlParserInputBufferPtr input;
	if(!ctxt || (filename == NULL))
		return -1;
	input = xmlParserInputBufferCreateFilename(filename, XML_CHAR_ENCODING_NONE);
	if(!input)
		return -1;
	ret = xmlSchemaValidateStream(ctxt, input, XML_CHAR_ENCODING_NONE, 0, 0);
	return ret;
}
/**
 * xmlSchemaValidCtxtGetParserCtxt:
 * @ctxt: a schema validation context
 *
 * allow access to the parser context of the schema validation context
 *
 * Returns the parser context of the schema validation context or NULL in case of error.
 */
xmlParserCtxtPtr xmlSchemaValidCtxtGetParserCtxt(xmlSchemaValidCtxtPtr ctxt)
{
	return ctxt ? ctxt->parserCtxt : 0;
}

#define bottom_xmlschemas
#include "elfgcchack.h"
#endif /* LIBXML_SCHEMAS_ENABLED */
