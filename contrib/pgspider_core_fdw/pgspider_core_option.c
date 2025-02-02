/*-------------------------------------------------------------------------
 *
 * pgspider_core_option.c
 *		  FDW option handling for pgspider_core_fdw
 *
 * Portions Copyright (c) 2018-2021, TOSHIBA CORPORATION
 *
 * IDENTIFICATION
 *		  contrib/pgspider_core_fdw/pgspider_core_option.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgspider_core_fdw.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "utils/hsearch.h"

#include "libpq-fe.h"


void
			SpdFdwCreateSpi(char *sql_text, int expect_ret);


/*
 * Describes the valid options for objects that this wrapper uses.
 */
typedef struct SpdFdwOption
{
	const char *keyword;
	Oid			optcontext;		/* OID of catalog in which option may appear */
	bool		is_libpq_opt;	/* true if it's used in libpq */
}			SpdFdwOption;

/*
 * Valid options for pgspider_core_fdw.
 * Allocated and filled in InitSpdFdwOptions.
 */
static SpdFdwOption * spd_fdw_options;

/*
 * Valid options for libpq.
 * Allocated and filled in InitSpdFdwOptions.
 */
static PQconninfoOption *libpq_options;

/*
 * Helper functions
 */
static void InitSpdFdwOptions(void);
static bool is_valid_option(const char *keyword, Oid context);
static bool is_libpq_option(const char *keyword);


typedef struct hashkey
{
	int			num;
}			HashKey;

typedef struct list_column
{
	char	   *column_name;
	char	   *column_type;
}			list_column;


typedef struct list_tables
{
	char	   *dist_table_name;	/* source table name */
	char	   *db_name;		/* */
	char	   *table_name;		/* */
	List	   *columns;
}			list_tables;

typedef struct list_ds
{
	char	   *datasource;
	char	   *driver;
	char	   *host;
	char	   *port;
	char	   *user;
	char	   *pass;
	List	   *listtables;		/* list of tables */
}			list_ds;



/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses pgspider_core_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
PG_FUNCTION_INFO_V1(pgspider_core_fdw_validator);

Datum
pgspider_core_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *cell;

	/* Build our options lists if we didn't yet. */
	InitSpdFdwOptions();

	/*
	 * Check that only options supported by spd_fdw, and allowed for the
	 * current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			SpdFdwOption *opt;
			StringInfoData buf;

			initStringInfo(&buf);
			for (opt = spd_fdw_options; opt->keyword; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->keyword);
			}
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid optiona \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s",
							 buf.data)));
		}

		/*
		 * Validate option value, when we can do so without any context.
		 */
		if (strcmp(def->defname, "use_remote_estimate") == 0 ||
			strcmp(def->defname, "updatable") == 0)
		{
			/* these accept only boolean values */
			(void) defGetBoolean(def);
		}
		else if (strcmp(def->defname, "fdw_startup_cost") == 0 ||
				 strcmp(def->defname, "fdw_tuple_cost") == 0)
		{
			/* these must have a non-negative numeric value */
			double		val;
			char	   *endp;

			val = strtod(defGetString(def), &endp);
			if (*endp || val < 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("%s requires a non-negative numeric value",
								def->defname)));
		}
	}

	PG_RETURN_VOID();
}

/*
 * Initialize option lists.
 */
static void
InitSpdFdwOptions(void)
{
	int			num_libpq_opts;
	PQconninfoOption *lopt;
	SpdFdwOption *popt;

	/* non-libpq FDW-specific FDW options */
	static const SpdFdwOption non_libpq_options[] = {
		{"schema_name", ForeignTableRelationId, false},
		{"dbname", ForeignTableRelationId, false},
		{"table_name", ForeignTableRelationId, false},
		{"column_name", AttributeRelationId, false},
		/* use_remote_estimate is available on both server and table */
		{"use_remote_estimate", ForeignServerRelationId, false},
		{"use_remote_estimate", ForeignTableRelationId, false},
		/* cost factors */
		{"fdw_startup_cost", ForeignServerRelationId, false},
		{"fdw_tuple_cost", ForeignServerRelationId, false},
		/* updatable is available on both server and table */
		{"updatable", ForeignServerRelationId, false},
		{"updatable", ForeignTableRelationId, false},
		{"config_file", ForeignServerRelationId, false},
		{"config_file", ForeignTableRelationId, false},
		{NULL, InvalidOid, false}
	};

	/* Prevent redundant initialization. */
	if (spd_fdw_options)
		return;

	/*
	 * Get list of valid libpq options.
	 *
	 * To avoid unnecessary work, we get the list once and use it throughout
	 * the lifetime of this backend process.  We don't need to care about
	 * memory context issues, because PQconndefaults allocates with malloc.
	 */
	libpq_options = PQconndefaults();
	if (!libpq_options)			/* assume reason for failure is OOM */
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("could not get libpq's default connection options")));

	/* Count how many libpq options are available. */
	num_libpq_opts = 0;
	for (lopt = libpq_options; lopt->keyword; lopt++)
		num_libpq_opts++;

	/*
	 * Construct an array which consists of all valid options for spd_fdw, by
	 * appending FDW-specific options to libpq options.
	 *
	 * We use plain malloc here to allocate spd_fdw_options because it lives
	 * as long as the backend process does.  Besides, keeping libpq_options in
	 * memory allows us to avoid copying every keyword string.
	 */
	spd_fdw_options = (SpdFdwOption *)
		malloc(sizeof(SpdFdwOption) * num_libpq_opts +
			   sizeof(non_libpq_options));
	if (spd_fdw_options == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	popt = spd_fdw_options;
	for (lopt = libpq_options; lopt->keyword; lopt++)
	{
		/* Hide debug options, as well as settings we override internally. */
		if (strchr(lopt->dispchar, 'D') ||
			strcmp(lopt->keyword, "fallback_application_name") == 0 ||
			strcmp(lopt->keyword, "client_encoding") == 0)
			continue;

		/* We don't have to copy keyword string, as described above. */
		popt->keyword = lopt->keyword;

		/*
		 * "user" and any secret options are allowed only on user mappings.
		 * Everything else is a server option.
		 */
		if (strcmp(lopt->keyword, "username") == 0 || strcmp(lopt->keyword, "user") == 0 || strchr(lopt->dispchar, '*'))
			popt->optcontext = UserMappingRelationId;
		else
			popt->optcontext = ForeignServerRelationId;
		popt->is_libpq_opt = true;

		popt++;
	}

	/* Append FDW-specific options and spd terminator. */
	memcpy(popt, non_libpq_options, sizeof(non_libpq_options));
}

/*
 * Check whether the given option is one of the valid spd_fdw options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
is_valid_option(const char *keyword, Oid context)
{
	SpdFdwOption *opt;

	Assert(spd_fdw_options);	/* must be initialized already */

	for (opt = spd_fdw_options; opt->keyword; opt++)
	{
		ereport(DEBUG1,
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
				 errmsg("option name = \"%s\" key word = %s", opt->keyword, keyword)));
		if (context == opt->optcontext && strcmp(opt->keyword, keyword) == 0)
			return true;
	}

	return false;
}

/*
 * Check whether the given option is one of the valid libpq options.
 */
static bool
is_libpq_option(const char *keyword)
{
	SpdFdwOption *opt;

	Assert(spd_fdw_options);	/* must be initialized already */

	for (opt = spd_fdw_options; opt->keyword; opt++)
	{
		if (opt->is_libpq_opt && strcmp(opt->keyword, keyword) == 0)
			return true;
	}

	return false;
}

/*
 * Generate key-value arrays which include only libpq options from the
 * given list (which can contain any kind of options).  Caller must have
 * allocated large-enough arrays.  Returns number of options found.
 */
int
spdExtractConnectionOptions(List *defelems, const char **keywords,
							const char **values)
{
	ListCell   *lc;
	int			i;

	/* Build our options lists if we didn't yet. */
	InitSpdFdwOptions();

	i = 0;
	foreach(lc, defelems)
	{
		DefElem    *d = (DefElem *) lfirst(lc);

		if (is_libpq_option(d->defname))
		{
			keywords[i] = d->defname;
			values[i] = defGetString(d);
			i++;
		}
	}
	return i;
}
