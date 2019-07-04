#include "postgres.h"
#include "strings.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "commands/defrem.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "catalog/dependency.h"
#include "utils/fmgroids.h"

#include "clickhousedb_fdw.h"

static HTAB *custom_objects_cache = NULL;
static HTAB *custom_columns_cache = NULL;

static HTAB *
create_custom_objects_cache(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(CustomObjectDef);

	return hash_create("clickhouse_fdw custom functions", 20, &ctl, HASH_ELEM);
}

static void
invalidate_custom_columns_cache(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	CustomColumnInfo *entry;

	hash_seq_init(&status, custom_columns_cache);
	while ((entry = (CustomColumnInfo *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(custom_columns_cache,
						(void *) &entry->relid,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

static HTAB *
create_custom_columns_cache(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid) + sizeof(int);
	ctl.entrysize = sizeof(CustomColumnInfo);

	CacheRegisterSyscacheCallback(ATTNUM,
								  invalidate_custom_columns_cache,
								  (Datum) 0);

	return hash_create("clickhouse_fdw custom functions", 20, &ctl, HASH_ELEM | HASH_BLOBS);
}

CustomObjectDef *checkForCustomFunction(Oid funcid)
{
	CustomObjectDef	*entry;

	if (is_builtin(funcid))
	{
		switch (funcid)
		{
			case F_TIMESTAMP_TRUNC:
			case F_TIMESTAMPTZ_TRUNC:
				break;
			default:
				return NULL;
		}
	}

	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	entry = hash_search(custom_objects_cache, (void *) &funcid, HASH_FIND, NULL);
	while (!entry)
	{
		Oid			extoid;
		char	   *extname;
		char	   *proname;

		entry = hash_search(custom_objects_cache, (void *) &funcid, HASH_ENTER, NULL);
		entry->cf_type = CF_USUAL;
		entry->custom_name[0] = '\0';
		entry->context = NULL;

		if (funcid == F_TIMESTAMPTZ_TRUNC || funcid == F_TIMESTAMP_TRUNC)
		{
			entry->cf_type = CF_DATE_TRUNC;
			entry->custom_name[0] = '\1';
			break;
		}

		extoid = getExtensionOfObject(ProcedureRelationId, funcid);
		extname = get_extension_name(extoid);
		if (extname)
		{
			HeapTuple	proctup;
			Form_pg_proc procform;

			proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(proctup))
				elog(ERROR, "cache lookup failed for function %u", funcid);

			procform = (Form_pg_proc) GETSTRUCT(proctup);

			if (strcmp(extname, "istore") == 0)
			{
				if (strcmp(NameStr(procform->proname), "sum") == 0)
				{
					entry->cf_type = CF_ISTORE_SUM;
					strcpy(entry->custom_name, "sumMap");
				}
				if (strcmp(NameStr(procform->proname), "sum_up") == 0)
				{
					entry->cf_type = CF_ISTORE_SUM_UP;
					strcpy(entry->custom_name, "arraySum");
				}
				else if (strcmp(NameStr(procform->proname), "istore_seed") == 0)
				{
					entry->cf_type = CF_ISTORE_SEED;
					entry->custom_name[0] = '\1';	/* complex */
				}
				else if (strcmp(NameStr(procform->proname), "accumulate") == 0)
				{
					entry->cf_type = CF_ISTORE_ACCUMULATE;
					entry->custom_name[0] = '\1';	/* complex */
				}
				else if (strcmp(NameStr(procform->proname), "slice") == 0)
				{
					entry->cf_type = CF_UNSHIPPABLE;
					entry->custom_name[0] = '\0';	/* complex */
				}
			}
			else if (strcmp(extname, "ajtime") == 0)
			{
				if (strcmp(NameStr(procform->proname), "ajtime_to_timestamp") == 0)
				{
					entry->cf_type = CF_AJTIME_TO_TIMESTAMP;
					strcpy(entry->custom_name, "");
				}
				else if (strcmp(NameStr(procform->proname), "ajtime_pl_interval") == 0)
				{
					entry->cf_type = CF_AJTIME_PL_INTERVAL;
					strcpy(entry->custom_name, "addSeconds");
				}
				else if (strcmp(NameStr(procform->proname), "ajtime_mi_interval") == 0)
				{
					entry->cf_type = CF_AJTIME_MI_INTERVAL;
					strcpy(entry->custom_name, "subtractSeconds");
				}
				else if (strcmp(NameStr(procform->proname), "day_diff") == 0)
				{
					entry->cf_type = CF_AJTIME_DAY_DIFF;
					strcpy(entry->custom_name, "toInt32");
				}
			}
			else if (strcmp(extname, "ajbool") == 0)
			{
				if (strcmp(NameStr(procform->proname), "ajbool_out") == 0)
					entry->cf_type = CF_AJBOOL_OUT;
			}
			ReleaseSysCache(proctup);
			pfree(extname);
		}

		break;
	}

	return entry;
}

CustomObjectDef *checkForCustomType(Oid typeoid)
{
	const char *proname;

	CustomObjectDef	*entry;
	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (is_builtin(typeoid))
		return NULL;

	entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_FIND, NULL);
	if (!entry)
	{
		Oid extoid = getExtensionOfObject(TypeRelationId, typeoid);
		char *extname = get_extension_name(extoid);

		entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_ENTER, NULL);
		entry->cf_type = CF_USUAL;
		entry->custom_name[0] = '\0';

		if (extname)
		{
			if (strcmp(extname, "istore") == 0)
			{
				entry->cf_type = CF_ISTORE_TYPE; /* bigistore or istore */
				strcpy(entry->custom_name, "Tuple(Array(Int32), Array(Int64))");
			}
			else if (strcmp(extname, "ajtime") == 0)
			{
				entry->cf_type = CF_AJTIME_TYPE; /* ajtime */
				strcpy(entry->custom_name, "DateTime");
			}
			pfree(extname);
		}
	}

	return entry;
}

CustomObjectDef *checkForCustomOperator(Oid opoid, Form_pg_operator form)
{
	HeapTuple	tuple = NULL;
	const char *proname;

	CustomObjectDef	*entry;
	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (is_builtin(opoid))
	{
		switch (opoid) {
			/* timestamptz + interval */
			case F_TIMESTAMPTZ_PL_INTERVAL:
				break;
			default:
				return NULL;
		}
	}

	if (!form)
	{
		tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(opoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for operator %u", opoid);
		form = (Form_pg_operator) GETSTRUCT(tuple);
	}

	entry = hash_search(custom_objects_cache, (void *) &opoid, HASH_FIND, NULL);
	if (!entry)
	{
		entry = hash_search(custom_objects_cache, (void *) &opoid, HASH_ENTER, NULL);
		entry->cf_type = CF_USUAL;

		if (opoid == F_TIMESTAMPTZ_PL_INTERVAL)
			entry->cf_type = CF_TIMESTAMPTZ_PL_INTERVAL;
		else
		{
			Oid		extoid = getExtensionOfObject(OperatorRelationId, opoid);
			char   *extname = get_extension_name(extoid);

			if (extname)
			{
				if (strcmp(extname, "ajtime") == 0)
					entry->cf_type = CF_AJTIME_OPERATOR;
				else if (strcmp(extname, "istore") == 0)
				{
					if (form && strcmp(NameStr(form->oprname), "->") == 0)
						entry->cf_type = CF_ISTORE_FETCHVAL;
				}
				else if (strcmp(extname, "ajbool") == 0)
					entry->cf_type = CF_AJBOOL_OPERATOR;

				pfree(extname);
			}
		}
	}

	if (tuple)
		ReleaseSysCache(tuple);

	return entry;
}

/*
 * Parse options from foreign table and apply them to fpinfo.
 *
 * New options might also require tweaking merge_fdw_options().
 */
void
ApplyCustomTableOptions(CHFdwRelationInfo *fpinfo, Oid relid)
{
	ListCell	*lc;
	TupleDesc	tupdesc;
	int			attnum;
	Relation	rel;
	List	   *options;

	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		if (strcmp(def->defname, "engine") == 0)
		{
			static char *expected = "collapsingmergetree";
			char *val = defGetString(def);
			if (strncasecmp(val, expected, strlen(expected)) == 0)
			{
				char   *start = index(val, '('),
					   *end = rindex(val, ')');

				fpinfo->ch_table_engine = CH_COLLAPSING_MERGE_TREE;
				if (start == end)
				{
					strcpy(fpinfo->ch_table_sign_field, "sign");
					continue;
				}

				if (end - start > NAMEDATALEN)
					elog(ERROR, "invalid format of ClickHouse engine");

				strncpy(fpinfo->ch_table_sign_field, start + 1, end - start - 1);
				fpinfo->ch_table_sign_field[end - start] = '\0';
			}
		}
	}

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	rel = heap_open(relid, NoLock);
	tupdesc = RelationGetDescr(rel);

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		bool				found;
		CustomObjectDef	   *cdef;
		CustomColumnInfo	entry_key,
						   *entry;
		custom_object_type	cf_type = CF_ISTORE_ARR;

		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
		entry_key.relid = relid;
		entry_key.varattno = attnum;

		entry = hash_search(custom_columns_cache,
				(void *) &entry_key.relid, HASH_ENTER, &found);
		if (found)
			continue;

		entry->relid = relid;
		entry->varattno = attnum;
		entry->table_engine = fpinfo->ch_table_engine;
		entry->coltype = CF_USUAL;
		strcpy(entry->colname, NameStr(attr->attname));
		strcpy(entry->signfield, fpinfo->ch_table_sign_field);

		/* If a column has the column_name FDW option, use that value */
		options = GetForeignColumnOptions(relid, attnum);
		foreach (lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "column_name") == 0)
			{
				strncpy(entry->colname, defGetString(def), NAMEDATALEN);
				entry->colname[NAMEDATALEN - 1] = '\0';
			}
			else if (strcmp(def->defname, "arrays") == 0)
				cf_type = CF_ISTORE_ARR;
			else if (strcmp(def->defname, "keys") == 0)
				cf_type = CF_ISTORE_COL;
		}

		cdef = checkForCustomType(attr->atttypid);
		if (cdef && cdef->cf_type == CF_ISTORE_TYPE)
			entry->coltype = cf_type;
	}
	heap_close(rel, NoLock);
}

/* Get foreign relation options */
CustomColumnInfo *
GetCustomColumnInfo(Oid relid, uint16 varattno)
{
	CustomColumnInfo	entry_key,
					   *entry;

	entry_key.relid = relid;
	entry_key.varattno = varattno;

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	entry = hash_search(custom_columns_cache,
			(void *) &entry_key.relid, HASH_FIND, NULL);

	return entry;
}