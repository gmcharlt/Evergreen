#include "opensrf/osrf_application.h"
#include "opensrf/osrf_settings.h"
#include "opensrf/utils.h"
#include "objson/object.h"
#include "opensrf/log.h"
#include "oils_utils.h"
#include "oils_constants.h"
#include "oils_event.h"
#include "oils_idl.h"
#include <dbi/dbi.h>

#include <time.h>
#include <stdlib.h>
#include <string.h>

#ifdef RSTORE
#  define MODULENAME "open-ils.reporter-store"
#else
#  define MODULENAME "open-ils.cstore"
#endif

#define PERSIST_NS "http://open-ils.org/spec/opensrf/IDL/persistance/v1"
#define OBJECT_NS "http://open-ils.org/spec/opensrf/IDL/objects/v1"
#define BASE_NS "http://opensrf.org/spec/IDL/base/v1"

int osrfAppChildInit();
int osrfAppInitialize();
void osrfAppChildExit();

int verifyObjectClass ( osrfMethodContext*, jsonObject* );

int beginTransaction ( osrfMethodContext* );
int commitTransaction ( osrfMethodContext* );
int rollbackTransaction ( osrfMethodContext* );

int setSavepoint ( osrfMethodContext* );
int releaseSavepoint ( osrfMethodContext* );
int rollbackSavepoint ( osrfMethodContext* );

int doJSONSearch ( osrfMethodContext* );

int dispatchCRUDMethod ( osrfMethodContext* );
jsonObject* doCreate ( osrfMethodContext*, int* );
jsonObject* doRetrieve ( osrfMethodContext*, int* );
jsonObject* doUpdate ( osrfMethodContext*, int* );
jsonObject* doDelete ( osrfMethodContext*, int* );
jsonObject* doFieldmapperSearch ( osrfMethodContext*, osrfHash*, jsonObject*, int* );
jsonObject* oilsMakeFieldmapperFromResult( dbi_result, osrfHash* );
jsonObject* oilsMakeJSONFromResult( dbi_result );

char* searchWriteSimplePredicate ( const char*, osrfHash*, const char*, const char*, const char* );
char* searchSimplePredicate ( const char*, const char*, osrfHash*, jsonObject* );
char* searchFunctionPredicate ( const char*, osrfHash*, jsonObjectNode* );
char* searchFieldTransform (const char*, osrfHash*, jsonObject*);
char* searchFieldTransformPredicate ( const char*, osrfHash*, jsonObjectNode* );
char* searchBETWEENPredicate ( const char*, osrfHash*, jsonObject* );
char* searchINPredicate ( const char*, osrfHash*, jsonObject*, const char* );
char* searchPredicate ( const char*, osrfHash*, jsonObject* );
char* searchJOIN ( jsonObject*, osrfHash* );
char* searchWHERE ( jsonObject*, osrfHash*, int );
char* buildSELECT ( jsonObject*, jsonObject*, osrfHash*, osrfMethodContext* );

char* SELECT ( osrfMethodContext*, jsonObject*, jsonObject*, jsonObject*, jsonObject*, jsonObject*, jsonObject*, jsonObject* );

void userDataFree( void* );
void sessionDataFree( char*, void* );

dbi_conn writehandle; /* our MASTER db connection */
dbi_conn dbhandle; /* our CURRENT db connection */
osrfHash readHandles;
jsonObject* jsonNULL = NULL; // 

/* called when this process is about to exit */
void osrfAppChildExit() {
	osrfLogDebug(OSRF_LOG_MARK, "Child is exiting, disconnecting from database...");

	if (writehandle) {
		dbi_conn_query(writehandle, "ROLLBACK;");
		dbi_conn_close(writehandle);
		writehandle = NULL;
	}

	if (dbhandle)
		dbi_conn_close(dbhandle);

	// XXX add cleanup of readHandles whenever that gets used

	return;
}

int osrfAppInitialize() {
	growing_buffer* method_name;

	osrfLogInfo(OSRF_LOG_MARK, "Initializing the CStore Server...");
	osrfLogInfo(OSRF_LOG_MARK, "Finding XML file...");

	char* idl_filename = osrf_settings_host_value("/apps/%s/app_settings/IDL", MODULENAME);
	osrfLogInfo(OSRF_LOG_MARK, "Found file:");
	osrfLogInfo(OSRF_LOG_MARK, idl_filename);

	if (!oilsIDLInit( idl_filename )) {
		osrfLogError(OSRF_LOG_MARK, "Problem loading the IDL.  Seacrest out!");
		exit(1);
	}

	// Generic search thingy
	method_name =  buffer_init(64);
	buffer_fadd(method_name, "%s.json_query", MODULENAME);
	osrfAppRegisterMethod( MODULENAME, buffer_data(method_name), "doJSONSearch", "", 1, OSRF_METHOD_STREAMING );

	// first we register all the transaction and savepoint methods
	method_name =  buffer_init(64);
	buffer_fadd(method_name, "%s.transaction.begin", MODULENAME);
	osrfAppRegisterMethod( MODULENAME, buffer_data(method_name), "beginTransaction", "", 0, 0 );

	buffer_reset(method_name);
	buffer_fadd(method_name, "%s.transaction.commit", MODULENAME);
	osrfAppRegisterMethod( MODULENAME, buffer_data(method_name), "commitTransaction", "", 0, 0 );

	buffer_reset(method_name);
	buffer_fadd(method_name, "%s.transaction.rollback", MODULENAME);
	osrfAppRegisterMethod( MODULENAME, buffer_data(method_name), "rollbackTransaction", "", 0, 0 );


	buffer_reset(method_name);
	buffer_fadd(method_name, "%s.savepoint.set", MODULENAME);
	osrfAppRegisterMethod( MODULENAME, buffer_data(method_name), "setSavepoint", "", 1, 0 );

	buffer_reset(method_name);
	buffer_fadd(method_name, "%s.savepoint.release", MODULENAME);
	osrfAppRegisterMethod( MODULENAME, buffer_data(method_name), "releaseSavepoint", "", 1, 0 );

	buffer_reset(method_name);
	buffer_fadd(method_name, "%s.savepoint.rollback", MODULENAME);
	osrfAppRegisterMethod( MODULENAME, buffer_data(method_name), "rollbackSavepoint", "", 1, 0 );

	osrfStringArray* global_methods = osrfNewStringArray(6);

	osrfStringArrayAdd( global_methods, "create" );
	osrfStringArrayAdd( global_methods, "retrieve" );
	osrfStringArrayAdd( global_methods, "update" );
	osrfStringArrayAdd( global_methods, "delete" );
	osrfStringArrayAdd( global_methods, "search" );
	osrfStringArrayAdd( global_methods, "id_list" );

	int c_index = 0; 
	char* classname;
	osrfStringArray* classes = osrfHashKeys( oilsIDL() );
	osrfLogDebug(OSRF_LOG_MARK, "%d classes loaded", classes->size );
	osrfLogDebug(OSRF_LOG_MARK, "At least %d methods will be generated", classes->size * global_methods->size);
	
	while ( (classname = osrfStringArrayGetString(classes, c_index++)) ) {
		osrfLogInfo(OSRF_LOG_MARK, "Generating class methods for %s", classname);
		
		osrfHash* idlClass = osrfHashGet(oilsIDL(), classname);

		if (!osrfStringArrayContains( osrfHashGet(idlClass, "controller"), MODULENAME )) {
			osrfLogInfo(OSRF_LOG_MARK, "%s is not listed as a controller for %s, moving on", MODULENAME, classname);
			continue;
		}

		char* virt = osrfHashGet(idlClass, "virtual");
		if (virt && !strcmp( virt, "true")) {
			osrfLogDebug(OSRF_LOG_MARK, "Class %s is virtual, skipping", classname );
			continue;
		}

		int i = 0; 
		char* method_type;
		char* st_tmp;
		char* _fm;
		char* part;
		osrfHash* method_meta;
		while ( (method_type = osrfStringArrayGetString(global_methods, i++)) ) {
			osrfLogDebug(OSRF_LOG_MARK, "Using files to build %s class methods for %s", method_type, classname);

			if (!osrfHashGet(idlClass, "fieldmapper")) continue;

			method_meta = osrfNewHash();
			osrfHashSet(method_meta, idlClass, "class");

			_fm = strdup( (char*)osrfHashGet(idlClass, "fieldmapper") );
			part = strtok_r(_fm, ":", &st_tmp);

			growing_buffer* method_name =  buffer_init(64);
			buffer_fadd(method_name, "%s.direct.%s", MODULENAME, part);

			while ((part = strtok_r(NULL, ":", &st_tmp))) {
				buffer_fadd(method_name, ".%s", part);
			}
			buffer_fadd(method_name, ".%s", method_type);


			char* method = buffer_data(method_name);
			buffer_free(method_name);
			free(_fm);

			osrfHashSet( method_meta, method, "methodname" );
			osrfHashSet( method_meta, method_type, "methodtype" );

			int flags = 0;
			if (!(strcmp( method_type, "search" )) || !(strcmp( method_type, "id_list" ))) {
				flags = flags | OSRF_METHOD_STREAMING;
			}

			osrfAppRegisterExtendedMethod(
				MODULENAME,
				method,
				"dispatchCRUDMethod",
				"",
				1,
				flags,
				(void*)method_meta
			);
		}
	}

	return 0;
}

/**
 * Connects to the database 
 */
int osrfAppChildInit() {

	osrfLogDebug(OSRF_LOG_MARK, "Attempting to initialize libdbi...");
	dbi_initialize(NULL);
	osrfLogDebug(OSRF_LOG_MARK, "... libdbi initialized.");

	char* driver	= osrf_settings_host_value("/apps/%s/app_settings/driver", MODULENAME);
	char* user	= osrf_settings_host_value("/apps/%s/app_settings/database/user", MODULENAME);
	char* host	= osrf_settings_host_value("/apps/%s/app_settings/database/host", MODULENAME);
	char* port	= osrf_settings_host_value("/apps/%s/app_settings/database/port", MODULENAME);
	char* db	= osrf_settings_host_value("/apps/%s/app_settings/database/db", MODULENAME);
	char* pw	= osrf_settings_host_value("/apps/%s/app_settings/database/pw", MODULENAME);

	osrfLogDebug(OSRF_LOG_MARK, "Attempting to load the database driver [%s]...", driver);
	writehandle = dbi_conn_new(driver);

	if(!writehandle) {
		osrfLogError(OSRF_LOG_MARK, "Error loading database driver [%s]", driver);
		return -1;
	}
	osrfLogDebug(OSRF_LOG_MARK, "Database driver [%s] seems OK", driver);

	osrfLogInfo(OSRF_LOG_MARK, "%s connecting to database.  host=%s, "
		"port=%s, user=%s, pw=%s, db=%s", MODULENAME, host, port, user, pw, db );

	if(host) dbi_conn_set_option(writehandle, "host", host );
	if(port) dbi_conn_set_option_numeric( writehandle, "port", atoi(port) );
	if(user) dbi_conn_set_option(writehandle, "username", user);
	if(pw) dbi_conn_set_option(writehandle, "password", pw );
	if(db) dbi_conn_set_option(writehandle, "dbname", db );

	free(user);
	free(host);
	free(port);
	free(db);
	free(pw);

	const char* err;
	if (dbi_conn_connect(writehandle) < 0) {
		dbi_conn_error(writehandle, &err);
		osrfLogError( OSRF_LOG_MARK, "Error connecting to database: %s", err);
		return -1;
	}

	osrfLogInfo(OSRF_LOG_MARK, "%s successfully connected to the database", MODULENAME);

	int attr;
	unsigned short type;
	int i = 0; 
	char* classname;
	osrfStringArray* classes = osrfHashKeys( oilsIDL() );
	
	while ( (classname = osrfStringArrayGetString(classes, i++)) ) {
		osrfHash* class = osrfHashGet( oilsIDL(), classname );
		osrfHash* fields = osrfHashGet( class, "fields" );

		char* virt = osrfHashGet(class, "virtual");
		if (virt && !strcmp( virt, "true")) {
			osrfLogDebug(OSRF_LOG_MARK, "Class %s is virtual, skipping", classname );
			continue;
		}

	
		growing_buffer* sql_buf = buffer_init(32);
		buffer_fadd( sql_buf, "SELECT * FROM %s WHERE 1=0;", osrfHashGet(class, "tablename") );

		char* sql = buffer_data(sql_buf);
		buffer_free(sql_buf);
		osrfLogDebug(OSRF_LOG_MARK, "%s Investigatory SQL = %s", MODULENAME, sql);

		dbi_result result = dbi_conn_query(writehandle, sql);
		free(sql);

		if (result) {

			int columnIndex = 1;
			const char* columnName;
			osrfHash* _f;
			while( (columnName = dbi_result_get_field_name(result, columnIndex++)) ) {

				osrfLogInternal(OSRF_LOG_MARK, "Looking for column named [%s]...", (char*)columnName);

				/* fetch the fieldmapper index */
				if( (_f = osrfHashGet(fields, (char*)columnName)) ) {

					osrfLogDebug(OSRF_LOG_MARK, "Found [%s] in IDL hash...", (char*)columnName);

					/* determine the field type and storage attributes */
					type = dbi_result_get_field_type(result, columnName);
					attr = dbi_result_get_field_attribs(result, columnName);

					switch( type ) {

						case DBI_TYPE_INTEGER :

							if ( !osrfHashGet(_f, "primitive") )
								osrfHashSet(_f,"number", "primitive");

							if( attr & DBI_INTEGER_SIZE8 ) 
								osrfHashSet(_f,"INT8", "datatype");
							else 
								osrfHashSet(_f,"INT", "datatype");
							break;

						case DBI_TYPE_DECIMAL :
							if ( !osrfHashGet(_f, "primitive") )
								osrfHashSet(_f,"number", "primitive");

							osrfHashSet(_f,"NUMERIC", "datatype");
							break;

						case DBI_TYPE_STRING :
							if ( !osrfHashGet(_f, "primitive") )
								osrfHashSet(_f,"string", "primitive");
							osrfHashSet(_f,"TEXT", "datatype");
							break;

						case DBI_TYPE_DATETIME :
							if ( !osrfHashGet(_f, "primitive") )
								osrfHashSet(_f,"string", "primitive");

							osrfHashSet(_f,"TIMESTAMP", "datatype");
							break;

						case DBI_TYPE_BINARY :
							if ( !osrfHashGet(_f, "primitive") )
								osrfHashSet(_f,"string", "primitive");

							osrfHashSet(_f,"BYTEA", "datatype");
					}

					osrfLogDebug(
						OSRF_LOG_MARK,
						"Setting [%s] to primitive [%s] and datatype [%s]...",
						(char*)columnName,
						osrfHashGet(_f, "primitive"),
						osrfHashGet(_f, "datatype")
					);
				}
			}
			dbi_result_free(result);
		} else {
			osrfLogDebug(OSRF_LOG_MARK, "No data found for class [%s]...", (char*)classname);
		}
	}

	osrfStringArrayFree(classes);

	return 0;
}

void userDataFree( void* blob ) {
	osrfHashFree( (osrfHash*)blob );
	return;
}

void sessionDataFree( char* key, void* item ) {
	if (!(strcmp(key,"xact_id"))) {
		if (writehandle)
			dbi_conn_query(writehandle, "ROLLBACK;");
		free(item);
	}

	return;
}

int beginTransaction ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	dbi_result result = dbi_conn_query(writehandle, "START TRANSACTION;");
	if (!result) {
		osrfLogError(OSRF_LOG_MARK, "%s: Error starting transaction", MODULENAME );
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "Error starting transaction" );
		return -1;
	} else {
		jsonObject* ret = jsonNewObject(ctx->session->session_id);
		osrfAppRespondComplete( ctx, ret );
		jsonObjectFree(ret);
		
		if (!ctx->session->userData) {
			ctx->session->userData = osrfNewHash();
			((osrfHash*)ctx->session->userData)->freeItem = &sessionDataFree;
		}

		osrfHashSet( (osrfHash*)ctx->session->userData, strdup( ctx->session->session_id ), "xact_id" );
		ctx->session->userDataFree = &userDataFree;
		
	}
	return 0;
}

int setSavepoint ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	char* spName = jsonObjectToSimpleString(jsonObjectGetIndex(ctx->params, 0));

	if (!osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_INTERNALSERVERERROR,
			"osrfMethodException",
			ctx->request,
			"No active transaction -- required for savepoints"
		);
		return -1;
	}

	dbi_result result = dbi_conn_queryf(writehandle, "SAVEPOINT \"%s\";", spName);
	if (!result) {
		osrfLogError(
			OSRF_LOG_MARK,
			"%s: Error creating savepoint %s in transaction %s",
			MODULENAME,
			spName,
			osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )
		);
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "Error creating savepoint" );
		return -1;
	} else {
		jsonObject* ret = jsonNewObject(spName);
		osrfAppRespondComplete( ctx, ret );
		jsonObjectFree(ret);
	}
	return 0;
}

int releaseSavepoint ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	char* spName = jsonObjectToSimpleString(jsonObjectGetIndex(ctx->params, 0));

	if (!osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_INTERNALSERVERERROR,
			"osrfMethodException",
			ctx->request,
			"No active transaction -- required for savepoints"
		);
		return -1;
	}

	dbi_result result = dbi_conn_queryf(writehandle, "RELEASE SAVEPOINT \"%s\";", spName);
	if (!result) {
		osrfLogError(
			OSRF_LOG_MARK,
			"%s: Error releasing savepoint %s in transaction %s",
			MODULENAME,
			spName,
			osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )
		);
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "Error releasing savepoint" );
		return -1;
	} else {
		jsonObject* ret = jsonNewObject(spName);
		osrfAppRespondComplete( ctx, ret );
		jsonObjectFree(ret);
	}
	return 0;
}

int rollbackSavepoint ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	char* spName = jsonObjectToSimpleString(jsonObjectGetIndex(ctx->params, 0));

	if (!osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_INTERNALSERVERERROR,
			"osrfMethodException",
			ctx->request,
			"No active transaction -- required for savepoints"
		);
		return -1;
	}

	dbi_result result = dbi_conn_queryf(writehandle, "ROLLBACK TO SAVEPOINT \"%s\";", spName);
	if (!result) {
		osrfLogError(
			OSRF_LOG_MARK,
			"%s: Error rolling back savepoint %s in transaction %s",
			MODULENAME,
			spName,
			osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )
		);
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "Error rolling back savepoint" );
		return -1;
	} else {
		jsonObject* ret = jsonNewObject(spName);
		osrfAppRespondComplete( ctx, ret );
		jsonObjectFree(ret);
	}
	return 0;
}

int commitTransaction ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	if (!osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "No active transaction to commit" );
		return -1;
	}

	dbi_result result = dbi_conn_query(writehandle, "COMMIT;");
	if (!result) {
		osrfLogError(OSRF_LOG_MARK, "%s: Error committing transaction", MODULENAME );
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "Error committing transaction" );
		return -1;
	} else {
		osrfHashRemove(ctx->session->userData, "xact_id");
		jsonObject* ret = jsonNewObject(ctx->session->session_id);
		osrfAppRespondComplete( ctx, ret );
		jsonObjectFree(ret);
	}
	return 0;
}

int rollbackTransaction ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	if (!osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "No active transaction to roll back" );
		return -1;
	}

	dbi_result result = dbi_conn_query(writehandle, "ROLLBACK;");
	if (!result) {
		osrfLogError(OSRF_LOG_MARK, "%s: Error rolling back transaction", MODULENAME );
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_INTERNALSERVERERROR, "osrfMethodException", ctx->request, "Error rolling back transaction" );
		return -1;
	} else {
		osrfHashRemove(ctx->session->userData, "xact_id");
		jsonObject* ret = jsonNewObject(ctx->session->session_id);
		osrfAppRespondComplete( ctx, ret );
		jsonObjectFree(ret);
	}
	return 0;
}

int dispatchCRUDMethod ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	osrfHash* meta = (osrfHash*) ctx->method->userData;
	osrfHash* class_obj = osrfHashGet( meta, "class" );
	
	int err = 0;

	jsonObject * obj = NULL;
	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "create"))
		obj = doCreate(ctx, &err);

	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "retrieve"))
		obj = doRetrieve(ctx, &err);

	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "update"))
		obj = doUpdate(ctx, &err);

	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "delete"))
		obj = doDelete(ctx, &err);

	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "search")) {

		obj = doFieldmapperSearch(ctx, class_obj, ctx->params, &err);
		if(err) return err;

		jsonObjectNode* cur;
		jsonObjectIterator* itr = jsonNewObjectIterator( obj );
		while ((cur = jsonObjectIteratorNext( itr ))) {
			osrfAppRespond( ctx, jsonObjectClone(cur->item) );
		}
		jsonObjectIteratorFree(itr);
		osrfAppRespondComplete( ctx, NULL );

	} else if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "id_list")) {

		jsonObject* _p = jsonObjectClone( ctx->params );
		if (jsonObjectGetIndex( _p, 1 )) {
			jsonObjectRemoveKey( jsonObjectGetIndex( _p, 1 ), "flesh" );
			jsonObjectRemoveKey( jsonObjectGetIndex( _p, 1 ), "flesh_columns" );
		} else {
			jsonObjectSetIndex( _p, 1, jsonParseString("{}") );
		}

		growing_buffer* sel_list = buffer_init(16);
		buffer_fadd(sel_list, "{ \"%s\":[\"%s\"] }", osrfHashGet( class_obj, "classname" ), osrfHashGet( class_obj, "primarykey" ));
		char* _s = buffer_data(sel_list);
		buffer_free(sel_list);

		jsonObjectSetKey( jsonObjectGetIndex( _p, 1 ), "select", jsonParseString(_s) );
		osrfLogDebug(OSRF_LOG_MARK, "%s: Select qualifer set to [%s]", MODULENAME, _s);
		free(_s);

		obj = doFieldmapperSearch(ctx, class_obj, _p, &err);
		if(err) return err;

		jsonObjectNode* cur;
		jsonObjectIterator* itr = jsonNewObjectIterator( obj );
		while ((cur = jsonObjectIteratorNext( itr ))) {
			osrfAppRespond(
				ctx,
				jsonObjectClone(
					jsonObjectGetIndex(
						cur->item,
						atoi(
							osrfHashGet(
								osrfHashGet(
									osrfHashGet( class_obj, "fields" ),
									osrfHashGet( class_obj, "primarykey")
								),
								"array_position"
							)
						)
					)
				)
			);
		}
		jsonObjectIteratorFree(itr);
		osrfAppRespondComplete( ctx, NULL );
		
	} else {
		osrfAppRespondComplete( ctx, obj );
	}

	jsonObjectFree(obj);

	return err;
}

int verifyObjectClass ( osrfMethodContext* ctx, jsonObject* param ) {
	
	osrfHash* meta = (osrfHash*) ctx->method->userData;
	osrfHash* class = osrfHashGet( meta, "class" );
	
	if ((strcmp( osrfHashGet(class, "classname"), param->classname ))) {

		growing_buffer* msg = buffer_init(128);
		buffer_fadd(
			msg,
			"%s: %s method for type %s was passed a %s",
			MODULENAME,
			osrfHashGet(meta, "methodtype"),
			osrfHashGet(class, "classname"),
			param->classname
		);

		char* m = buffer_data(msg);
		osrfAppSessionStatus( ctx->session, OSRF_STATUS_BADREQUEST, "osrfMethodException", ctx->request, m );

		buffer_free(msg);
		free(m);

		return 0;
	}
	return 1;
}

jsonObject* doCreate(osrfMethodContext* ctx, int* err ) {

	osrfHash* meta = osrfHashGet( (osrfHash*) ctx->method->userData, "class" );
	jsonObject* target = jsonObjectGetIndex( ctx->params, 0 );
	jsonObject* options = jsonObjectGetIndex( ctx->params, 1 );

	if (!verifyObjectClass(ctx, target)) {
		*err = -1;
		return jsonNULL;
	}

	osrfLogDebug( OSRF_LOG_MARK, "Object seems to be of the correct type" );

	if (!ctx->session || !ctx->session->userData || !osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfLogError( OSRF_LOG_MARK, "No active transaction -- required for CREATE" );

		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_BADREQUEST,
			"osrfMethodException",
			ctx->request,
			"No active transaction -- required for CREATE"
		);
		*err = -1;
		return jsonNULL;
	}

	char* trans_id = osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" );

        // Set the last_xact_id
	osrfHash* last_xact_id;
	if ((last_xact_id = oilsIDLFindPath("/%s/fields/last_xact_id", target->classname))) {
		int index = atoi( osrfHashGet(last_xact_id, "array_position") );
		osrfLogDebug(OSRF_LOG_MARK, "Setting last_xact_id to %s on %s at position %d", trans_id, target->classname, index);
		jsonObjectSetIndex(target, index, jsonNewObject(trans_id));
	}       

	osrfLogDebug( OSRF_LOG_MARK, "There is a transaction running..." );

	dbhandle = writehandle;

	osrfHash* fields = osrfHashGet(meta, "fields");
	char* pkey = osrfHashGet(meta, "primarykey");
	char* seq = osrfHashGet(meta, "sequence");

	growing_buffer* table_buf = buffer_init(128);
	growing_buffer* col_buf = buffer_init(128);
	growing_buffer* val_buf = buffer_init(128);

	buffer_fadd(table_buf,"INSERT INTO %s", osrfHashGet(meta, "tablename"));
	buffer_add(col_buf,"(");
	buffer_add(val_buf,"VALUES (");


	int i = 0;
	int first = 1;
	char* field_name;
	osrfStringArray* field_list = osrfHashKeys( fields );
	while ( (field_name = osrfStringArrayGetString(field_list, i++)) ) {

		osrfHash* field = osrfHashGet( fields, field_name );

		if(!( strcmp( osrfHashGet(osrfHashGet(fields,field_name), "virtual"), "true" ) )) continue;

		jsonObject* field_object = jsonObjectGetIndex( target, atoi(osrfHashGet(field, "array_position")) );

		char* value;
		if (field_object && field_object->classname) {
			value = jsonObjectToSimpleString(
					jsonObjectGetIndex(
						field_object,
						atoi(
							osrfHashGet(
								osrfHashGet(
									oilsIDLFindPath("/%s/fields", field_object->classname),
									(char*)oilsIDLFindPath("/%s/primarykey", field_object->classname)
								),
								"array_position"
							)
						)
					)
				);

		} else {
			value = jsonObjectToSimpleString( field_object );
		}


		if (first) {
			first = 0;
		} else {
			buffer_add(col_buf, ",");
			buffer_add(val_buf, ",");
		}

		buffer_add(col_buf, field_name);

		if (!field_object || field_object->type == JSON_NULL) {
			buffer_add( val_buf, "DEFAULT" );
			
		} else if ( !strcmp(osrfHashGet(field, "primitive"), "number") ) {
			if ( !strcmp(osrfHashGet(field, "datatype"), "INT8") ) {
				buffer_fadd( val_buf, "%lld", atol(value) );
				
			} else if ( !strcmp(osrfHashGet(field, "datatype"), "INT") ) {
				buffer_fadd( val_buf, "%ld", atoll(value) );
				
			} else if ( !strcmp(osrfHashGet(field, "datatype"), "NUMERIC") ) {
				buffer_fadd( val_buf, "%f", atof(value) );
			}
		} else {
			if ( dbi_conn_quote_string(writehandle, &value) ) {
				buffer_fadd( val_buf, "%s", value );

			} else {
				osrfLogError(OSRF_LOG_MARK, "%s: Error quoting string [%s]", MODULENAME, value);
				osrfAppSessionStatus(
					ctx->session,
					OSRF_STATUS_INTERNALSERVERERROR,
					"osrfMethodException",
					ctx->request,
					"Error quoting string -- please see the error log for more details"
				);
				free(value);
				buffer_free(table_buf);
				buffer_free(col_buf);
				buffer_free(val_buf);
				*err = -1;
				return jsonNULL;
			}
		}

		free(value);
		
	}


	buffer_add(col_buf,")");
	buffer_add(val_buf,")");

	growing_buffer* sql = buffer_init(128);
	buffer_fadd(
		sql,
		"%s %s %s;",
		buffer_data(table_buf),
		buffer_data(col_buf),
		buffer_data(val_buf)
	);
	buffer_free(table_buf);
	buffer_free(col_buf);
	buffer_free(val_buf);

	char* query = buffer_data(sql);
	buffer_free(sql);

	osrfLogDebug(OSRF_LOG_MARK, "%s: Insert SQL [%s]", MODULENAME, query);

	
	dbi_result result = dbi_conn_query(writehandle, query);

	jsonObject* obj = NULL;

	if (!result) {
		obj = jsonNewObject(NULL);
		osrfLogError(
			OSRF_LOG_MARK,
			"%s ERROR inserting %s object using query [%s]",
			MODULENAME,
			osrfHashGet(meta, "fieldmapper"),
			query
		);
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_INTERNALSERVERERROR,
			"osrfMethodException",
			ctx->request,
			"INSERT error -- please see the error log for more details"
		);
		*err = -1;
	} else {

		int pos = atoi(osrfHashGet( osrfHashGet(fields, pkey), "array_position" ));
		char* id = jsonObjectToSimpleString(jsonObjectGetIndex(target, pos));
		if (!id) {
			unsigned long long new_id = dbi_conn_sequence_last(writehandle, seq);
			growing_buffer* _id = buffer_init(10);
			buffer_fadd(_id, "%lld", new_id);
			id = buffer_data(_id);
			buffer_free(_id);
		}

		if (	!options
			|| !jsonObjectGetKey( options, "quiet")
			|| strcmp( jsonObjectToSimpleString(jsonObjectGetKey( options, "quiet")), "true" )
		) {

			jsonObject* fake_params = jsonParseString("[]");
			jsonObjectPush(fake_params, jsonParseString("{}"));

			jsonObjectSetKey(
				jsonObjectGetIndex(fake_params, 0),
				osrfHashGet(meta, "primarykey"),
				jsonNewObject(id)
			);

			jsonObject* list = doFieldmapperSearch( ctx,meta, fake_params, err);

			if(*err) {
				jsonObjectFree( fake_params );
				obj = jsonNULL;
			} else {
				obj = jsonObjectClone( jsonObjectGetIndex(list, 0) );
			}

			jsonObjectFree( list );
			jsonObjectFree( fake_params );

		} else {
			obj = jsonNewObject(id);
		}

	}

	free(query);

	return obj;

}


jsonObject* doRetrieve(osrfMethodContext* ctx, int* err ) {

	osrfHash* meta = osrfHashGet( (osrfHash*) ctx->method->userData, "class" );

	jsonObject* obj;

	char* id = jsonObjectToSimpleString(jsonObjectGetIndex(ctx->params, 0));
	jsonObject* order_hash = jsonObjectGetIndex(ctx->params, 1);

	osrfLogDebug(
		OSRF_LOG_MARK,
		"%s retrieving %s object with id %s",
		MODULENAME,
		osrfHashGet(meta, "fieldmapper"),
		id
	);

	jsonObject* fake_params = jsonParseString("[]");
	jsonObjectPush(fake_params, jsonParseString("{}"));

	jsonObjectSetKey(
		jsonObjectGetIndex(fake_params, 0),
		osrfHashGet(meta, "primarykey"),
		jsonParseString(id)
	);

	if (order_hash) jsonObjectPush(fake_params, jsonObjectClone(order_hash) );

	jsonObject* list = doFieldmapperSearch( ctx,meta, fake_params, err);

	if(*err) {
		jsonObjectFree( fake_params );
		return jsonNULL;
	}

	obj = jsonObjectClone( jsonObjectGetIndex(list, 0) );

	jsonObjectFree( list );
	jsonObjectFree( fake_params );

	return obj;
}

char* jsonNumberToDBString ( osrfHash* field, jsonObject* value ) {
	growing_buffer* val_buf = buffer_init(32);

	if ( !strncmp(osrfHashGet(field, "datatype"), "INT", 3) ) {
		if (value->type == JSON_NUMBER) buffer_fadd( val_buf, "%ld", (long)jsonObjectGetNumber(value) );
		else buffer_fadd( val_buf, "%ld", atol(jsonObjectToSimpleString(value)) );

	} else if ( !strcmp(osrfHashGet(field, "datatype"), "NUMERIC") ) {
		if (value->type == JSON_NUMBER) buffer_fadd( val_buf, "%f",  jsonObjectGetNumber(value) );
		else buffer_fadd( val_buf, "%f", atof(jsonObjectToSimpleString(value)) );
	}

	char* pred = buffer_data(val_buf);
	buffer_free(val_buf);

	return pred;
}

char* searchINPredicate (const char* class, osrfHash* field, jsonObject* node, const char* op) {
	growing_buffer* sql_buf = buffer_init(32);
	
	buffer_fadd(
		sql_buf,
		"\"%s\".%s ",
		class,
		osrfHashGet(field, "name")
	);

	if (!op) {
		buffer_add(sql_buf, "IN (");
	} else if (!(strcasecmp(op,"not in"))) {
		buffer_add(sql_buf, "NOT IN (");
	} else {
		buffer_add(sql_buf, "IN (");
	}

	int in_item_index = 0;
	int in_item_first = 1;
	jsonObject* in_item;
	while ( (in_item = jsonObjectGetIndex(node, in_item_index++)) ) {

		if (in_item_first)
			in_item_first = 0;
		else
			buffer_add(sql_buf, ", ");

		if ( !strcmp(osrfHashGet(field, "primitive"), "number") ) {
			char* val = jsonNumberToDBString( field, in_item );
			buffer_fadd( sql_buf, "%s", val );
			free(val);

		} else {
			char* key_string = jsonObjectToSimpleString(in_item);
			if ( dbi_conn_quote_string(dbhandle, &key_string) ) {
				buffer_fadd( sql_buf, "%s", key_string );
				free(key_string);
			} else {
				osrfLogError(OSRF_LOG_MARK, "%s: Error quoting key string [%s]", MODULENAME, key_string);
				free(key_string);
				buffer_free(sql_buf);
				return NULL;
			}
		}
	}

	buffer_add(
		sql_buf,
		")"
	);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);

	return pred;
}

char* searchValueTransform( jsonObject* array ) {
	growing_buffer* sql_buf = buffer_init(32);

	char* val = NULL;
	int func_item_index = 0;
	int func_item_first = 2;
	jsonObject* func_item;
	while ( (func_item = jsonObjectGetIndex(array, func_item_index++)) ) {

		val = jsonObjectToSimpleString(func_item);

		if (func_item_first == 2) {
			buffer_fadd(sql_buf, "%s( ", val);
			free(val);
			func_item_first--;
			continue;
		}

		if (func_item_first)
			func_item_first--;
		else
			buffer_add(sql_buf, ", ");

		if ( dbi_conn_quote_string(dbhandle, &val) ) {
			buffer_fadd( sql_buf, "%s", val );
			free(val);
		} else {
			osrfLogError(OSRF_LOG_MARK, "%s: Error quoting key string [%s]", MODULENAME, val);
			free(val);
			buffer_free(sql_buf);
			return NULL;
		}
	}

	buffer_add(
		sql_buf,
		" )"
	);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);

	return pred;
}

char* searchFunctionPredicate (const char* class, osrfHash* field, jsonObjectNode* node) {
	growing_buffer* sql_buf = buffer_init(32);

	char* val = searchValueTransform(node->item);
	
	buffer_fadd(
		sql_buf,
		"\"%s\".%s %s %s",
		class,
		osrfHashGet(field, "name"),
		node->key,
		val
	);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);
	free(val);

	return pred;
}

char* searchFieldTransform (const char* class, osrfHash* field, jsonObject* node) {
	growing_buffer* sql_buf = buffer_init(32);
	
	char* field_transform = jsonObjectToSimpleString( jsonObjectGetKey( node, "transform" ) );

	buffer_fadd(
		sql_buf,
		"%s(\"%s\".%s)",
		field_transform,
		class,
		osrfHashGet(field, "name")
	);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);
	free(field_transform);

	return pred;
}

char* searchFieldTransformPredicate (const char* class, osrfHash* field, jsonObjectNode* node) {
	growing_buffer* sql_buf = buffer_init(32);
	
	char* field_transform = searchFieldTransform( class, field, node->item );
	char* value = NULL;

	if (jsonObjectGetKey( node->item, "value" )->type == JSON_ARRAY) {
		value = searchValueTransform(jsonObjectGetKey( node->item, "value" ));
	} else if (jsonObjectGetKey( node->item, "value" )->type != JSON_NULL) {
		if ( !strcmp(osrfHashGet(field, "primitive"), "number") ) {
			value = jsonNumberToDBString( field, jsonObjectGetKey( node->item, "value" ) );
		} else {
			value = jsonObjectToSimpleString(jsonObjectGetKey( node->item, "value" ));
			if ( !dbi_conn_quote_string(dbhandle, &value) ) {
				osrfLogError(OSRF_LOG_MARK, "%s: Error quoting key string [%s]", MODULENAME, value);
				free(value);
				return NULL;
			}
		}
	}

	buffer_fadd(
		sql_buf,
		"%s %s %s",
		field_transform,
		node->key,
		value
	);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);
	free(field_transform);

	return pred;
}

char* searchSimplePredicate (const char* orig_op, const char* class, osrfHash* field, jsonObject* node) {

	char* val = NULL;

	if (node->type != JSON_NULL) {
		if ( !strcmp(osrfHashGet(field, "primitive"), "number") ) {
			val = jsonNumberToDBString( field, node );
		} else {
			val = jsonObjectToSimpleString(node);
		}
	}

	char* pred = searchWriteSimplePredicate( class, field, osrfHashGet(field, "name"), orig_op, val );

	if (val) free(val);

	return pred;
}

char* searchWriteSimplePredicate ( const char* class, osrfHash* field, const char* left, const char* orig_op, const char* right ) {

	char* val = NULL;
	char* op = NULL;
	if (right == NULL) {
		val = strdup("NULL");

		if (strcmp( orig_op, "=" ))
			op = strdup("IS NOT");
		else
			op = strdup("IS");

	} else if ( !strcmp(osrfHashGet(field, "primitive"), "number") ) {
		val = strdup(right);
		op = strdup(orig_op);

	} else {
		val = strdup(right);
		if ( !dbi_conn_quote_string(dbhandle, &val) ) {
			osrfLogError(OSRF_LOG_MARK, "%s: Error quoting key string [%s]", MODULENAME, val);
			free(val);
			return NULL;
		}
		op = strdup(orig_op);
	}

	growing_buffer* sql_buf = buffer_init(16);
	buffer_fadd( sql_buf, "\"%s\".%s %s %s", class, left, op, val );
	free(val);
	free(op);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);

	return pred;

}

char* searchBETWEENPredicate (const char* class, osrfHash* field, jsonObject* node) {

	char* x_string;
	char* y_string;

	if ( !strcmp(osrfHashGet(field, "primitive"), "number") ) {
		x_string = jsonNumberToDBString(field, jsonObjectGetIndex(node,0));
		y_string = jsonNumberToDBString(field, jsonObjectGetIndex(node,1));

	} else {
		x_string = jsonObjectToSimpleString(jsonObjectGetIndex(node,0));
		y_string = jsonObjectToSimpleString(jsonObjectGetIndex(node,1));
		if ( !(dbi_conn_quote_string(dbhandle, &x_string) && dbi_conn_quote_string(dbhandle, &y_string)) ) {
			osrfLogError(OSRF_LOG_MARK, "%s: Error quoting key strings [%s] and [%s]", MODULENAME, x_string, y_string);
			free(x_string);
			free(y_string);
			return NULL;
		}
	}

	growing_buffer* sql_buf = buffer_init(32);
	buffer_fadd( sql_buf, "%s BETWEEN %s AND %s", osrfHashGet(field, "name"), x_string, y_string );
	free(x_string);
	free(y_string);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);

	return pred;
}

char* searchPredicate ( const char* class, osrfHash* field, jsonObject* node ) {

	char* pred = NULL;
	if (node->type == JSON_ARRAY) { // equality IN search
		pred = searchINPredicate( class, field, node, NULL );
	} else if (node->type == JSON_HASH) { // non-equality search
		jsonObjectNode* pred_node;
		jsonObjectIterator* pred_itr = jsonNewObjectIterator( node );
		while ( (pred_node = jsonObjectIteratorNext( pred_itr )) ) {
			if ( !(strcasecmp( pred_node->key,"between" )) )
				pred = searchBETWEENPredicate( class, field, pred_node->item );
			else if ( !(strcasecmp( pred_node->key,"in" )) || !(strcasecmp( pred_node->key,"not in" )) )
				pred = searchINPredicate( class, field, pred_node->item, pred_node->key );
			else if ( pred_node->item->type == JSON_ARRAY )
				pred = searchFunctionPredicate( class, field, pred_node );
			else if ( pred_node->item->type == JSON_HASH )
				pred = searchFieldTransformPredicate( class, field, pred_node );
			else 
				pred = searchSimplePredicate( pred_node->key, class, field, pred_node->item );

			break;
		}
	} else if (node->type == JSON_NULL) { // IS NULL search
		growing_buffer* _p = buffer_init(16);
		buffer_fadd(
			_p,
			"\"%s\".%s IS NULL",
			class,
			osrfHashGet(field, "name")
		);
		pred = buffer_data(_p);
		buffer_free(_p);
	} else { // equality search
		pred = searchSimplePredicate( "=", class, field, node );
	}

	return pred;

}


/*

join : {
	acn : {
		field : record,
		fkey : id
		type : left
		filter_op : or
		filter : { ... },
		join : {
			acp : {
				field : call_number,
				fkey : id,
				filter : { ... },
			},
		},
	},
	mrd : {
		field : record,
		type : inner
		fkey : id,
		filter : { ... },
	}
}

*/

char* searchJOIN ( jsonObject* join_hash, osrfHash* leftmeta ) {

	growing_buffer* join_buf = buffer_init(128);
	char* leftclass = osrfHashGet(leftmeta, "classname");

	jsonObjectNode* snode = NULL;
	jsonObjectIterator* search_itr = jsonNewObjectIterator( join_hash );
	while ( (snode = jsonObjectIteratorNext( search_itr )) ) {
		osrfHash* idlClass = osrfHashGet( oilsIDL(), snode->key );

		char* class = osrfHashGet(idlClass, "classname");
		char* table = osrfHashGet(idlClass, "tablename");

		char* type = jsonObjectToSimpleString( jsonObjectGetKey( snode->item, "type" ) );
		char* filter_op = jsonObjectToSimpleString( jsonObjectGetKey( snode->item, "filter_op" ) );
		char* fkey = jsonObjectToSimpleString( jsonObjectGetKey( snode->item, "fkey" ) );
		char* field = jsonObjectToSimpleString( jsonObjectGetKey( snode->item, "field" ) );

		jsonObject* filter = jsonObjectGetKey( snode->item, "filter" );
		jsonObject* join_filter = jsonObjectGetKey( snode->item, "join" );

		if (field && !fkey) {
			fkey = (char*)oilsIDLFindPath("/%s/links/%s/key", class, field);
			if (!fkey) {
				osrfLogError(
					OSRF_LOG_MARK,
					"%s: JOIN failed.  No link defined from %s.%s to %s",
					MODULENAME,
					class,
					field,
					leftclass
				);
				buffer_free(join_buf);
				return NULL;
			}
			fkey = strdup( fkey );

		} else if (!field && fkey) {
			field = (char*)oilsIDLFindPath("/%s/links/%s/key", leftclass, fkey );
			if (!field) {
				osrfLogError(
					OSRF_LOG_MARK,
					"%s: JOIN failed.  No link defined from %s.%s to %s",
					MODULENAME,
					leftclass,
					fkey,
					class
				);
				buffer_free(join_buf);
				return NULL;
			}
			field = strdup( field );

		} else if (!field && !fkey) {
			osrfHash* _links = oilsIDLFindPath("/%s/links", leftclass);

			int i = 0;
			osrfStringArray* keys = osrfHashKeys( _links );
			while ( (fkey = osrfStringArrayGetString(keys, i++)) ) {
				fkey = strdup(osrfStringArrayGetString(keys, i++));
				if ( !strcmp( (char*)oilsIDLFindPath("/%s/links/%s/class", leftclass, fkey), class) ) {
					field = strdup( (char*)oilsIDLFindPath("/%s/links/%s/key", leftclass, fkey) );
					break;
				} else {
					free(fkey);
				}
			}
			osrfStringArrayFree(keys);

			if (!field && !fkey) {
				_links = oilsIDLFindPath("/%s/links", class);

				i = 0;
				keys = osrfHashKeys( _links );
				while ( (field = osrfStringArrayGetString(keys, i++)) ) {
					field = strdup(osrfStringArrayGetString(keys, i++));
					if ( !strcmp( (char*)oilsIDLFindPath("/%s/links/%s/class", class, field), class) ) {
						fkey = strdup( (char*)oilsIDLFindPath("/%s/links/%s/key", class, field) );
						break;
					} else {
						free(field);
					}
				}
				osrfStringArrayFree(keys);
			}

			if (!field && !fkey) {
				osrfLogError(
					OSRF_LOG_MARK,
					"%s: JOIN failed.  No link defined between %s and %s",
					MODULENAME,
					leftclass,
					class
				);
				buffer_free(join_buf);
				return NULL;
			}

		}

		if (type) {
			if ( !strcasecmp(type,"left") ) {
				buffer_add(join_buf, " LEFT JOIN");
			} else if ( !strcasecmp(type,"right") ) {
				buffer_add(join_buf, " RIGHT JOIN");
			} else if ( !strcasecmp(type,"full") ) {
				buffer_add(join_buf, " FULL JOIN");
			} else {
				buffer_add(join_buf, " INNER JOIN");
			}
		} else {
			buffer_add(join_buf, " INNER JOIN");
		}

		buffer_fadd(join_buf, " %s AS \"%s\" ON ( \"%s\".%s = \"%s\".%s", table, class, class, field, leftclass, fkey);

		if (filter) {
			if (filter_op) {
				if (!strcasecmp("or",filter_op)) {
					buffer_add( join_buf, " OR " );
				} else {
					buffer_add( join_buf, " AND " );
				}
			} else {
				buffer_add( join_buf, " AND " );
			}

			char* jpred = searchWHERE( filter, idlClass, 0 );
			buffer_fadd( join_buf, " %s", jpred );
			free(jpred);
		}

		buffer_add(join_buf, " ) ");
		
		if (join_filter) {
			char* jpred = searchJOIN( join_filter, idlClass );
			buffer_fadd( join_buf, " %s", jpred );
			free(jpred);
		}

		free(type);
		free(filter_op);
		free(fkey);
		free(field);
	}

	char* join_string = buffer_data(join_buf);
	buffer_free(join_buf);
	return join_string;
}

/*

{ +class : { -or|-and : { field : { op : value }, ... }, ... }, ... }

*/
char* searchWHERE ( jsonObject* search_hash, osrfHash* meta, int opjoin_type ) {

	growing_buffer* sql_buf = buffer_init(128);

	jsonObjectNode* node = NULL;

	int first = 1;
	jsonObjectIterator* search_itr = jsonNewObjectIterator( search_hash );
	while ( (node = jsonObjectIteratorNext( search_itr )) ) {

		if (first) {
			first = 0;
		} else {
			if (opjoin_type == 1) buffer_add(sql_buf, " OR ");
			else buffer_add(sql_buf, " AND ");
		}

		if ( !strncmp("+",node->key,1) ) {
			char* subpred = searchWHERE( node->item, osrfHashGet( oilsIDL(), node->key + 1 ), 0);
			buffer_fadd(sql_buf, "( %s )", subpred);
			free(subpred);
		} else if ( !strcasecmp("-or",node->key) ) {
			char* subpred = searchWHERE( node->item, meta, 1);
			buffer_fadd(sql_buf, "( %s )", subpred);
			free(subpred);
		} else if ( !strcasecmp("-and",node->key) ) {
			char* subpred = searchWHERE( node->item, meta, 0);
			buffer_fadd(sql_buf, "( %s )", subpred);
			free(subpred);
		} else {

			char* class = osrfHashGet(meta, "classname");
			osrfHash* fields = osrfHashGet(meta, "fields");
			osrfHash* field = osrfHashGet( fields, node->key );

			if (!field) {
				osrfLogError(
					OSRF_LOG_MARK,
					"%s: Attempt to reference non-existant column %s on table %s",
					MODULENAME,
					node->key,
					osrfHashGet(meta, "tablename")
				);
				buffer_free(sql_buf);
				return NULL;
			}

			char* subpred = searchPredicate( class, field, node->item );
			buffer_add( sql_buf, subpred );
			free(subpred);
		}
	}

	jsonObjectIteratorFree(search_itr);

	char* pred = buffer_data(sql_buf);
	buffer_free(sql_buf);

	return pred;
}

char* SELECT (
		/* method context */ osrfMethodContext* ctx,
		
		/* SELECT   */ jsonObject* selhash,
		/* FROM     */ jsonObject* join_hash,
		/* WHERE    */ jsonObject* search_hash,
		/* GROUP BY */ jsonObject* order_hash,
		/* ORDER BY */ jsonObject* group_hash,
		/* LIMIT    */ jsonObject* limit,
		/* OFFSET   */ jsonObject* offset
) {
	// in case we don't get a select list
	jsonObject* defaultselhash = NULL;

	// general tmp objects
	jsonObject* __tmp = NULL;
	jsonObjectNode* selclass = NULL;
	jsonObjectNode* selfield = NULL;
	jsonObjectNode* snode = NULL;
	jsonObjectNode* onode = NULL;
	char* string = NULL;
	int first = 1;

	// return variable for the SQL
	char* sql = NULL;

	// the core search class
	char* core_class = NULL;

	// metadata about the core search class
	osrfHash* core_meta = NULL;
	osrfHash* core_fields = NULL;
	osrfHash* idlClass = NULL;

	// the query buffer
	growing_buffer* sql_buf = buffer_init(128);

	// temp buffer for the SELECT list
	growing_buffer* select_buf = buffer_init(128);
	growing_buffer* order_buf = buffer_init(128);

	// punt if there's no core class
	if (!join_hash || ( join_hash->type == JSON_HASH && !join_hash->size ))
		return NULL;

	// get the core class -- the only key of the top level FROM clause, or a string
	if (join_hash->type == JSON_HASH) {
		jsonObjectIterator* tmp_itr = jsonNewObjectIterator( join_hash );
		snode = jsonObjectIteratorNext( tmp_itr );
		
		core_class = strdup( snode->key );
		join_hash = snode->item;

		jsonObjectIteratorFree( tmp_itr );
		snode = NULL;

	} else if (join_hash->type == JSON_STRING) {
		core_class = jsonObjectToSimpleString( join_hash );
		join_hash = NULL;
	}

	// punt if we don't know about the core class
	if (!(core_meta = osrfHashGet( oilsIDL(), core_class )))
		return NULL;

	core_fields = osrfHashGet(core_meta, "fields");

	// if the select list is empty, or the core class field list is '*',
	// build the default select list ...
	if (!selhash) {
		selhash = defaultselhash = jsonParseString( "{}" );
		jsonObjectSetKey( selhash, core_class, jsonParseString( "[]" ) );
	} else if ( (__tmp = jsonObjectGetKey( selhash, core_class )) && __tmp->type == JSON_STRING ) {
		char* __x = jsonObjectToSimpleString( __tmp );
		if (!strncmp( "*", __x, 1 )) {
			jsonObjectRemoveKey( selhash, core_class );
			jsonObjectSetKey( selhash, core_class, jsonParseString( "[]" ) );
		}
		free(__x);
	}

	// ... and if we /are/ building the default list, do that
	if ( (__tmp = jsonObjectGetKey(selhash,core_class)) && !__tmp->size ) {
		
		int i = 0;
		char* field;

		osrfStringArray* keys = osrfHashKeys( core_fields );
		while ( (field = osrfStringArrayGetString(keys, i++)) ) {
			if ( strncasecmp( "true", osrfHashGet( osrfHashGet( core_fields, field ), "virtual" ), 4 ) )
				jsonObjectPush( __tmp, jsonNewObject( field ) );
		}
		osrfStringArrayFree(keys);
	}

	// Now we build the acutal select list
	first = 1;
	jsonObjectIterator* selclass_itr = jsonNewObjectIterator( selhash );
	while ( (selclass = jsonObjectIteratorNext( selclass_itr )) ) {

		// round trip through the idl, just to be safe
		idlClass = osrfHashGet( oilsIDL(), selclass->key );
		if (!idlClass) continue;
		char* cname = osrfHashGet(idlClass, "classname");

		// make sure the target relation is in the join tree
		if (strcmp(core_class,cname)) {
			if (!join_hash) continue;

			jsonObject* found =  jsonObjectFindPath(join_hash, "//%s", cname);
			if (!found->size) {
				jsonObjectFree(found);
				continue;
			}

			jsonObjectFree(found);
		}

		// stitch together the column list ...
		jsonObjectIterator* select_itr = jsonNewObjectIterator( selclass->item );
		while ( (selfield = jsonObjectIteratorNext( select_itr )) ) {

			// ... if it's a sstring, just toss it on the pile
			if (selfield->item->type == JSON_STRING) {

				// again, just to be safe
				osrfHash* field = osrfHashGet( osrfHashGet( idlClass, "fields" ), jsonObjectToSimpleString(selfield->item) );
				if (!field) continue;
				char* fname = osrfHashGet(field, "name");

				if (first) {
					first = 0;
				} else {
					buffer_add(select_buf, ",");
				}

				buffer_fadd(select_buf, " \"%s\".%s", cname, fname);

			// ... but it could be an object, in which case we check for a Field Transform
			} else {

				char* __column = jsonObjectToSimpleString( jsonObjectGetKey( selfield->item, "column" ) );
				char* __alias = NULL;

				// again, just to be safe
				osrfHash* field = osrfHashGet( osrfHashGet( idlClass, "fields" ), __column );
				if (!field) continue;
				char* fname = osrfHashGet(field, "name");

				if (first) {
					first = 0;
				} else {
					buffer_add(select_buf, ",");
				}

				if ((__tmp = jsonObjectGetKey( selfield->item, "alias" ))) {
					__alias = jsonObjectToSimpleString( __tmp );
				} else {
					__alias = strdup(__column);
				}

				if (jsonObjectGetKey( selfield->item, "transform" )) {
					free(__column);
					__column = searchFieldTransform(cname, field, selfield->item);
					buffer_fadd(select_buf, " %s AS \"%s\"", __column, __alias);
				} else {
					buffer_fadd(select_buf, " \"%s\".%s AS \"%s\"", cname, fname, __alias);
				}

				free(__column);
				free(__alias);
			}
		}

	}

	char* col_list = buffer_data(select_buf);
	buffer_free(select_buf);

	// Put it all together
	buffer_fadd(sql_buf, "SELECT %s FROM %s AS \"%s\" ", col_list, osrfHashGet(core_meta, "tablename"), core_class );
	free(col_list);

	// Now, walk the join tree and add that clause
	if ( join_hash ) {
		char* join_clause = searchJOIN( join_hash, core_meta );
		buffer_add(sql_buf, join_clause);
		free(join_clause);
	}

	if ( search_hash ) {
		buffer_add(sql_buf, " WHERE ");

		// and it's on the the WHERE clause
		char* pred = searchWHERE( search_hash, core_meta, 0 );
		if (!pred) {
			osrfAppSessionStatus(
				ctx->session,
				OSRF_STATUS_INTERNALSERVERERROR,
				"osrfMethodException",
				ctx->request,
				"Severe query error -- see error log for more details"
			);
			free(core_class);
			buffer_free(sql_buf);
			if (defaultselhash) jsonObjectFree(defaultselhash);
			return NULL;
		} else {
			buffer_add(sql_buf, pred);
			free(pred);
		}
	}

	first = 1;
	jsonObjectIterator* class_itr = jsonNewObjectIterator( order_hash );
	while ( (snode = jsonObjectIteratorNext( class_itr )) ) {

		if (!jsonObjectGetKey(selhash,snode->key))
			continue;

		if ( snode->item->type == JSON_HASH ) {

			jsonObjectIterator* order_itr = jsonNewObjectIterator( snode->item );
			while ( (onode = jsonObjectIteratorNext( order_itr )) ) {

				if (!oilsIDLFindPath( "/%s/fields/%s", snode->key, onode->key ))
					continue;

				char* direction = NULL;
				if ( onode->item->type == JSON_HASH ) {
					if ( jsonObjectGetKey( onode->item, "transform" ) ) {
						string = searchFieldTransform(
							snode->key,
							oilsIDLFindPath( "/%s/fields/%s", snode->key, onode->key ),
							onode->item
						);
					} else {
						growing_buffer* field_buf = buffer_init(16);
						buffer_fadd(field_buf, "\"%s\".%s", snode->key, onode->key);
						string = buffer_data(field_buf);
						buffer_free(field_buf);
					}

					if ( (__tmp = jsonObjectGetKey( onode->item, "direction" )) ) {
						direction = jsonObjectToSimpleString(__tmp);
						if (!strncasecmp(direction, "d", 1)) {
							free(direction);
							direction = " DESC";
						} else {
							free(direction);
							direction = " ASC";
						}
					}

				} else {
					string = strdup(onode->key);
					direction = jsonObjectToSimpleString(onode->item);
					if (!strncasecmp(direction, "d", 1)) {
						free(direction);
						direction = " DESC";
					} else {
						free(direction);
						direction = " ASC";
					}
				}

				if (first) {
					first = 0;
				} else {
					buffer_add(order_buf, ", ");
				}

				buffer_add(order_buf, string);
				free(string);

				if (direction) {
					buffer_add(order_buf, direction);
				}

			}

		} else if ( snode->item->type == JSON_ARRAY ) {

			jsonObjectIterator* order_itr = jsonNewObjectIterator( snode->item );
			while ( (onode = jsonObjectIteratorNext( order_itr )) ) {

				char* _f = jsonObjectToSimpleString( onode->item );

				if (!oilsIDLFindPath( "/%s/fields/%s", snode->key, _f))
					continue;

				if (first) {
					first = 0;
				} else {
					buffer_add(order_buf, ", ");
				}

				buffer_add(order_buf, _f);
				free(_f);

			}

		// IT'S THE OOOOOOOOOOOLD STYLE!
		} else {
			osrfLogError(OSRF_LOG_MARK, "%s: Possible SQL injection attempt; direct order by is not allowed", MODULENAME);
			osrfAppSessionStatus(
				ctx->session,
				OSRF_STATUS_INTERNALSERVERERROR,
				"osrfMethodException",
				ctx->request,
				"Severe query error -- see error log for more details"
			);

			free(core_class);
			buffer_free(order_buf);
			buffer_free(sql_buf);
			if (defaultselhash) jsonObjectFree(defaultselhash);
			return NULL;
		}

	}

	string = buffer_data(order_buf);
	buffer_free(order_buf);

	if (strlen(string)) {
		buffer_fadd(
			sql_buf,
			" ORDER BY %s",
			string
		);
	}

	free(string);

	if ( limit ){
		string = jsonObjectToSimpleString(limit);
		buffer_fadd( sql_buf, " LIMIT %d", atoi(string) );
		free(string);
	}

	if (offset) {
		string = jsonObjectToSimpleString(offset);
		buffer_fadd( sql_buf, " OFFSET %d", atoi(string) );
		free(string);
	}

	buffer_add(sql_buf, ";");

	sql = buffer_data(sql_buf);

	free(core_class);
	buffer_free(sql_buf);
	if (defaultselhash) jsonObjectFree(defaultselhash);

	return sql;

}

char* buildSELECT ( jsonObject* search_hash, jsonObject* order_hash, osrfHash* meta, osrfMethodContext* ctx ) {

	osrfHash* fields = osrfHashGet(meta, "fields");
	char* core_class = osrfHashGet(meta, "classname");

	jsonObject* join_hash = jsonObjectGetKey( order_hash, "join" );

	jsonObjectNode* node = NULL;
	jsonObjectNode* snode = NULL;
	jsonObjectNode* onode = NULL;
	jsonObject* _tmp = NULL;
	jsonObject* selhash = NULL;
	jsonObject* defaultselhash = NULL;

	growing_buffer* sql_buf = buffer_init(128);
	growing_buffer* select_buf = buffer_init(128);

	if ( !(selhash = jsonObjectGetKey( order_hash, "select" )) ) {
		defaultselhash = jsonParseString( "{}" );
		selhash = defaultselhash;
	}
	
	if ( !jsonObjectGetKey(selhash,core_class) ) {
		jsonObjectSetKey( selhash, core_class, jsonParseString( "[]" ) );
		jsonObject* flist = jsonObjectGetKey( selhash, core_class );
		
		int i = 0;
		char* field;

		osrfStringArray* keys = osrfHashKeys( fields );
		while ( (field = osrfStringArrayGetString(keys, i++)) ) {
			if ( strcasecmp( "true", osrfHashGet( osrfHashGet( fields, field ), "virtual" ) ) )
				jsonObjectPush( flist, jsonNewObject( field ) );
		}
		osrfStringArrayFree(keys);
	}

	int first = 1;
	jsonObjectIterator* class_itr = jsonNewObjectIterator( selhash );
	while ( (snode = jsonObjectIteratorNext( class_itr )) ) {

		osrfHash* idlClass = osrfHashGet( oilsIDL(), snode->key );
		if (!idlClass) continue;
		char* cname = osrfHashGet(idlClass, "classname");

		if (strcmp(core_class,snode->key)) {
			if (!join_hash) continue;

			jsonObject* found =  jsonObjectFindPath(join_hash, "//%s", snode->key);
			if (!found->size) {
				jsonObjectFree(found);
				continue;
			}

			jsonObjectFree(found);
		}

		jsonObjectIterator* select_itr = jsonNewObjectIterator( snode->item );
		while ( (node = jsonObjectIteratorNext( select_itr )) ) {
			osrfHash* field = osrfHashGet( osrfHashGet( idlClass, "fields" ), jsonObjectToSimpleString(node->item) );
			char* fname = osrfHashGet(field, "name");

			if (!field) continue;

			if (first) {
				first = 0;
			} else {
				buffer_add(select_buf, ",");
			}

			buffer_fadd(select_buf, " \"%s\".%s", cname, fname);
		}
	}

	char* col_list = buffer_data(select_buf);
	buffer_free(select_buf);

	buffer_fadd(sql_buf, "SELECT %s FROM %s AS \"%s\"", col_list, osrfHashGet(meta, "tablename"), core_class );

	if ( join_hash ) {
		char* join_clause = searchJOIN( join_hash, meta );
		buffer_fadd(sql_buf, " %s", join_clause);
		free(join_clause);
	}

	buffer_add(sql_buf, " WHERE ");

	char* pred = searchWHERE( search_hash, meta, 0 );
	if (!pred) {
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_INTERNALSERVERERROR,
				"osrfMethodException",
				ctx->request,
				"Severe query error -- see error log for more details"
			);
		buffer_free(sql_buf);
		return NULL;
	} else {
		buffer_add(sql_buf, pred);
		free(pred);
	}

	if (order_hash) {
		char* string = NULL;
		if ( (_tmp = jsonObjectGetKey( order_hash, "order_by" )) ){

			growing_buffer* order_buf = buffer_init(128);

			first = 1;
			jsonObjectIterator* class_itr = jsonNewObjectIterator( _tmp );
			while ( (snode = jsonObjectIteratorNext( class_itr )) ) {

				if (!jsonObjectGetKey(selhash,snode->key))
					continue;

				if ( snode->item->type == JSON_HASH ) {

					jsonObjectIterator* order_itr = jsonNewObjectIterator( snode->item );
					while ( (onode = jsonObjectIteratorNext( order_itr )) ) {

						if (!oilsIDLFindPath( "/%s/fields/%s", snode->key, onode->key ))
							continue;

						char* direction = NULL;
						if ( onode->item->type == JSON_HASH ) {
							if ( jsonObjectGetKey( onode->item, "transform" ) ) {
								string = searchFieldTransform(
									snode->key,
									oilsIDLFindPath( "/%s/fields/%s", snode->key, onode->key ),
									onode->item
								);
							} else {
								growing_buffer* field_buf = buffer_init(16);
								buffer_fadd(field_buf, "\"%s\".%s", snode->key, onode->key);
								string = buffer_data(field_buf);
								buffer_free(field_buf);
							}

							if ( (_tmp = jsonObjectGetKey( onode->item, "direction" )) ) {
								direction = jsonObjectToSimpleString(_tmp);
								if (!strncasecmp(direction, "d", 1)) {
									free(direction);
									direction = " DESC";
								} else {
									free(direction);
									direction = " ASC";
								}
							}

						} else {
							string = strdup(onode->key);
							direction = jsonObjectToSimpleString(onode->item);
							if (!strncasecmp(direction, "d", 1)) {
								free(direction);
								direction = " DESC";
							} else {
								free(direction);
								direction = " ASC";
							}
						}

						if (first) {
							first = 0;
						} else {
							buffer_add(order_buf, ", ");
						}

						buffer_add(order_buf, string);
						free(string);

						if (direction) {
							buffer_add(order_buf, direction);
						}

					}

				} else {
					string = jsonObjectToSimpleString(snode->item);
					buffer_add(order_buf, string);
					free(string);
					break;
				}

			}

			string = buffer_data(order_buf);
			buffer_free(order_buf);

			if (strlen(string)) {
				buffer_fadd(
					sql_buf,
					" ORDER BY %s",
					string
				);
			}

			free(string);
		}

		if ( (_tmp = jsonObjectGetKey( order_hash, "limit" )) ){
			string = jsonObjectToSimpleString(_tmp);
			buffer_fadd(
				sql_buf,
				" LIMIT %d",
				atoi(string)
			);
			free(string);
		}

		_tmp = jsonObjectGetKey( order_hash, "offset" );
		if (_tmp) {
			string = jsonObjectToSimpleString(_tmp);
			buffer_fadd(
				sql_buf,
				" OFFSET %d",
				atoi(string)
			);
			free(string);
		}
	}

	buffer_add(sql_buf, ";");

	char* sql = buffer_data(sql_buf);
	buffer_free(sql_buf);
	if (defaultselhash) jsonObjectFree(defaultselhash);

	return sql;
}

int doJSONSearch ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);
	osrfLogDebug(OSRF_LOG_MARK, "Recieved query request");

	int err = 0;

	// XXX for now...
	dbhandle = writehandle;

	jsonObject* hash = jsonObjectGetIndex(ctx->params, 0);

	osrfLogDebug(OSRF_LOG_MARK, "Building SQL ...");
	char* sql = SELECT(
			ctx,
			jsonObjectGetKey( hash, "select" ),
			jsonObjectGetKey( hash, "from" ),
			jsonObjectGetKey( hash, "where" ),
			jsonObjectGetKey( hash, "group_by" ),
			jsonObjectGetKey( hash, "order_by" ),
			jsonObjectGetKey( hash, "limit" ),
			jsonObjectGetKey( hash, "offset" )
	);

	if (!sql) {
		err = -1;
		return err;
	}
	
	osrfLogDebug(OSRF_LOG_MARK, "%s SQL =  %s", MODULENAME, sql);
	dbi_result result = dbi_conn_query(dbhandle, sql);

	if(result) {
		osrfLogDebug(OSRF_LOG_MARK, "Query returned with no errors");

		if (dbi_result_first_row(result)) {
			/* JSONify the result */
			osrfLogDebug(OSRF_LOG_MARK, "Query returned at least one row");

			do {
				osrfAppRespond( ctx, oilsMakeJSONFromResult( result ) );
			} while (dbi_result_next_row(result));

			osrfAppRespondComplete( ctx, NULL );
		} else {
			osrfLogDebug(OSRF_LOG_MARK, "%s returned no results for query %s", MODULENAME, sql);
		}

		/* clean up the query */
		dbi_result_free(result); 

	} else {
		err = -1;
		osrfLogError(OSRF_LOG_MARK, "%s: Error with query [%s]", MODULENAME, sql);
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_INTERNALSERVERERROR,
			"osrfMethodException",
			ctx->request,
			"Severe query error -- see error log for more details"
		);
		free(sql);
		return err;

	}

	free(sql);
	return err;
}

jsonObject* doFieldmapperSearch ( osrfMethodContext* ctx, osrfHash* meta, jsonObject* params, int* err ) {

	// XXX for now...
	dbhandle = writehandle;

	osrfHash* links = osrfHashGet(meta, "links");
	osrfHash* fields = osrfHashGet(meta, "fields");
	char* core_class = osrfHashGet(meta, "classname");
	char* pkey = osrfHashGet(meta, "primarykey");

	jsonObject* _tmp;
	jsonObject* obj;
	jsonObject* search_hash = jsonObjectGetIndex(params, 0);
	jsonObject* order_hash = jsonObjectGetIndex(params, 1);

	char* sql = buildSELECT( search_hash, order_hash, meta, ctx );
	if (!sql) {
		*err = -1;
		return NULL;
	}
	
	osrfLogDebug(OSRF_LOG_MARK, "%s SQL =  %s", MODULENAME, sql);
	dbi_result result = dbi_conn_query(dbhandle, sql);

	osrfHash* dedup = osrfNewHash();
	jsonObject* res_list = jsonParseString("[]");
	if(result) {
		osrfLogDebug(OSRF_LOG_MARK, "Query returned with no errors");

		if (dbi_result_first_row(result)) {
			/* JSONify the result */
			osrfLogDebug(OSRF_LOG_MARK, "Query returned at least one row");
			do {
				obj = oilsMakeFieldmapperFromResult( result, meta );
				int pkey_pos = atoi( osrfHashGet( osrfHashGet( fields, pkey ), "array_position" ) );
				char* pkey_val = jsonObjectToSimpleString( jsonObjectGetIndex( obj, pkey_pos ) );
				if ( osrfHashGet( dedup, pkey_val ) ) {
					jsonObjectFree(obj);
				} else {
					osrfHashSet( dedup, pkey_val, pkey_val );
					jsonObjectPush(res_list, obj);
				}
			} while (dbi_result_next_row(result));
		} else {
			osrfLogDebug(OSRF_LOG_MARK, "%s returned no results for query %s", MODULENAME, sql);
		}

		/* clean up the query */
		dbi_result_free(result); 

	} else {
		osrfLogError(OSRF_LOG_MARK, "%s: Error retrieving %s with query [%s]", MODULENAME, osrfHashGet(meta, "fieldmapper"), sql);
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_INTERNALSERVERERROR,
			"osrfMethodException",
			ctx->request,
			"Severe query error -- see error log for more details"
		);
		*err = -1;
		free(sql);
		jsonObjectFree(res_list);
		return jsonNULL;

	}

	free(sql);

	if (res_list->size && order_hash) {
		_tmp = jsonObjectGetKey( order_hash, "flesh" );
		if (_tmp) {
			int x = (int)jsonObjectGetNumber(_tmp);

			jsonObject* flesh_blob = NULL;
			if ((flesh_blob = jsonObjectGetKey( order_hash, "flesh_fields" )) && x > 0) {

				flesh_blob = jsonObjectClone( flesh_blob );
				jsonObject* flesh_fields = jsonObjectGetKey( flesh_blob, core_class );

				osrfStringArray* link_fields = NULL;

				if (flesh_fields) {
					if (flesh_fields->size == 1) {
						char* _t = jsonObjectToSimpleString( jsonObjectGetIndex( flesh_fields, 0 ) );
						if (!strcmp(_t,"*")) link_fields = osrfHashKeys( links );
						free(_t);
					}

					if (!link_fields) {
						jsonObjectNode* _f;
						link_fields = osrfNewStringArray(1);
						jsonObjectIterator* _i = jsonNewObjectIterator( flesh_fields );
						while ((_f = jsonObjectIteratorNext( _i ))) {
							osrfStringArrayAdd( link_fields, jsonObjectToSimpleString( _f->item ) );
						}
					}
				}

				jsonObjectNode* cur;
				jsonObjectIterator* itr = jsonNewObjectIterator( res_list );
				while ((cur = jsonObjectIteratorNext( itr ))) {

					int i = 0;
					char* link_field;
					
					while ( (link_field = osrfStringArrayGetString(link_fields, i++)) ) {

						osrfLogDebug(OSRF_LOG_MARK, "Starting to flesh %s", link_field);

						osrfHash* kid_link = osrfHashGet(links, link_field);
						if (!kid_link) continue;

						osrfHash* field = osrfHashGet(fields, link_field);
						if (!field) continue;

						osrfHash* value_field = field;

						osrfHash* kid_idl = osrfHashGet(oilsIDL(), osrfHashGet(kid_link, "class"));
						if (!kid_idl) continue;

						if (!(strcmp( osrfHashGet(kid_link, "reltype"), "has_many" ))) { // has_many
							value_field = osrfHashGet( fields, osrfHashGet(meta, "primarykey") );
						}
							
						if (!(strcmp( osrfHashGet(kid_link, "reltype"), "might_have" ))) { // might_have
							value_field = osrfHashGet( fields, osrfHashGet(meta, "primarykey") );
						}

						osrfStringArray* link_map = osrfHashGet( kid_link, "map" );

						if (link_map->size > 0) {
							jsonObject* _kid_key = jsonParseString("[]");
							jsonObjectPush(
								_kid_key,
								jsonNewObject( osrfStringArrayGetString( link_map, 0 ) )
							);

							jsonObjectSetKey(
								flesh_blob,
								osrfHashGet(kid_link, "class"),
								_kid_key
							);
						};

						osrfLogDebug(
							OSRF_LOG_MARK,
							"Link field: %s, remote class: %s, fkey: %s, reltype: %s",
							osrfHashGet(kid_link, "field"),
							osrfHashGet(kid_link, "class"),
							osrfHashGet(kid_link, "key"),
							osrfHashGet(kid_link, "reltype")
						);

						jsonObject* fake_params = jsonParseString("[]");
						jsonObjectPush(fake_params, jsonParseString("{}")); // search hash
						jsonObjectPush(fake_params, jsonParseString("{}")); // order/flesh hash

						osrfLogDebug(OSRF_LOG_MARK, "Creating dummy params object...");

						char* search_key =
						jsonObjectToSimpleString(
							jsonObjectGetIndex(
								cur->item,
								atoi( osrfHashGet(value_field, "array_position") )
							)
						);

						if (!search_key) {
							osrfLogDebug(OSRF_LOG_MARK, "Nothing to search for!");
							continue;
						}
							
						jsonObjectSetKey(
							jsonObjectGetIndex(fake_params, 0),
							osrfHashGet(kid_link, "key"),
							jsonNewObject( search_key )
						);

						free(search_key);


						jsonObjectSetKey(
							jsonObjectGetIndex(fake_params, 1),
							"flesh",
							jsonNewNumberObject( (double)(x - 1 + link_map->size) )
						);

						if (flesh_blob)
							jsonObjectSetKey( jsonObjectGetIndex(fake_params, 1), "flesh_fields", jsonObjectClone(flesh_blob) );

						if (jsonObjectGetKey(order_hash, "order_by")) {
							jsonObjectSetKey(
								jsonObjectGetIndex(fake_params, 1),
								"order_by",
								jsonObjectClone(jsonObjectGetKey(order_hash, "order_by"))
							);
						}

						if (jsonObjectGetKey(order_hash, "select")) {
							jsonObjectSetKey(
								jsonObjectGetIndex(fake_params, 1),
								"select",
								jsonObjectClone(jsonObjectGetKey(order_hash, "select"))
							);
						}

						jsonObject* kids = doFieldmapperSearch(ctx, kid_idl, fake_params, err);

						if(*err) {
							jsonObjectFree( fake_params );
							osrfStringArrayFree(link_fields);
							jsonObjectIteratorFree(itr);
							jsonObjectFree(res_list);
							return jsonNULL;
						}

						osrfLogDebug(OSRF_LOG_MARK, "Search for %s return %d linked objects", osrfHashGet(kid_link, "class"), kids->size);

						jsonObject* X = NULL;
						if ( link_map->size > 0 && kids->size > 0 ) {
							X = kids;
							kids = jsonParseString("[]");

							jsonObjectNode* _k_node;
							jsonObjectIterator* _k = jsonNewObjectIterator( X );
							while ((_k_node = jsonObjectIteratorNext( _k ))) {
								jsonObjectPush(
									kids,
									jsonObjectClone(
										jsonObjectGetIndex(
											_k_node->item,
											(unsigned long)atoi(
												osrfHashGet(
													osrfHashGet(
														osrfHashGet(
															osrfHashGet(
																oilsIDL(),
																osrfHashGet(kid_link, "class")
															),
															"fields"
														),
														osrfStringArrayGetString( link_map, 0 )
													),
													"array_position"
												)
											)
										)
									)
								);
							}
						}

						if (!(strcmp( osrfHashGet(kid_link, "reltype"), "has_a" )) || !(strcmp( osrfHashGet(kid_link, "reltype"), "might_have" ))) {
							osrfLogDebug(OSRF_LOG_MARK, "Storing fleshed objects in %s", osrfHashGet(kid_link, "field"));
							jsonObjectSetIndex(
								cur->item,
								(unsigned long)atoi( osrfHashGet( field, "array_position" ) ),
								jsonObjectClone( jsonObjectGetIndex(kids, 0) )
							);
						}

						if (!(strcmp( osrfHashGet(kid_link, "reltype"), "has_many" ))) { // has_many
							osrfLogDebug(OSRF_LOG_MARK, "Storing fleshed objects in %s", osrfHashGet(kid_link, "field"));
							jsonObjectSetIndex(
								cur->item,
								(unsigned long)atoi( osrfHashGet( field, "array_position" ) ),
								jsonObjectClone( kids )
							);
						}

						if (X) {
							jsonObjectFree(kids);
							kids = X;
						}

						jsonObjectFree( kids );
						jsonObjectFree( fake_params );

						osrfLogDebug(OSRF_LOG_MARK, "Fleshing of %s complete", osrfHashGet(kid_link, "field"));
						osrfLogDebug(OSRF_LOG_MARK, "%s", jsonObjectToJSON(cur->item));

					}
				}
				jsonObjectFree( flesh_blob );
				osrfStringArrayFree(link_fields);
				jsonObjectIteratorFree(itr);
			}
		}
	}

	return res_list;
}


jsonObject* doUpdate(osrfMethodContext* ctx, int* err ) {

	osrfHash* meta = osrfHashGet( (osrfHash*) ctx->method->userData, "class" );
	jsonObject* target = jsonObjectGetIndex(ctx->params, 0);

	if (!verifyObjectClass(ctx, target)) {
		*err = -1;
		return jsonNULL;
	}

	if (!osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_BADREQUEST,
			"osrfMethodException",
			ctx->request,
			"No active transaction -- required for UPDATE"
		);
		*err = -1;
		return jsonNULL;
	}

	dbhandle = writehandle;

	char* trans_id = osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" );

        // Set the last_xact_id
	osrfHash* last_xact_id;
	if ((last_xact_id = oilsIDLFindPath("/%s/fields/last_xact_id", target->classname))) {
		int index = atoi( osrfHashGet(last_xact_id, "array_position") );
		osrfLogDebug(OSRF_LOG_MARK, "Setting last_xact_id to %s on %s at position %d", trans_id, target->classname, index);
		jsonObjectSetIndex(target, index, jsonNewObject(trans_id));
	}       

	char* pkey = osrfHashGet(meta, "primarykey");
	osrfHash* fields = osrfHashGet(meta, "fields");

	char* id =
		jsonObjectToSimpleString(
			jsonObjectGetIndex(
				target,
				atoi( osrfHashGet( osrfHashGet( fields, pkey ), "array_position" ) )
			)
		);

	osrfLogDebug(
		OSRF_LOG_MARK,
		"%s updating %s object with %s = %s",
		MODULENAME,
		osrfHashGet(meta, "fieldmapper"),
		pkey,
		id
	);

	growing_buffer* sql = buffer_init(128);
	buffer_fadd(sql,"UPDATE %s SET", osrfHashGet(meta, "tablename"));

	int i = 0;
	int first = 1;
	char* field_name;
	osrfStringArray* field_list = osrfHashKeys( fields );
	while ( (field_name = osrfStringArrayGetString(field_list, i++)) ) {

		osrfHash* field = osrfHashGet( fields, field_name );

		if(!( strcmp( field_name, pkey ) )) continue;
		if(!( strcmp( osrfHashGet(osrfHashGet(fields,field_name), "virtual"), "true" ) )) continue;

		jsonObject* field_object = jsonObjectGetIndex( target, atoi(osrfHashGet(field, "array_position")) );

		char* value;
		if (field_object && field_object->classname) {
			value = jsonObjectToSimpleString(
					jsonObjectGetIndex(
						field_object,
						atoi(
							osrfHashGet(
								osrfHashGet(
									oilsIDLFindPath("/%s/fields", field_object->classname),
									(char*)oilsIDLFindPath("/%s/primarykey", field_object->classname)
								),
								"array_position"
							)
						)
					)
				);

		} else {
			value = jsonObjectToSimpleString( field_object );
		}

		osrfLogDebug( OSRF_LOG_MARK, "Updating %s object with %s = %s", osrfHashGet(meta, "fieldmapper"), field_name, value);

		if (!field_object || field_object->type == JSON_NULL) {
			if ( !(!( strcmp( osrfHashGet(meta, "classname"), "au" ) ) && !( strcmp( field_name, "passwd" ) )) ) { // arg at the special case!
				if (first) first = 0;
				else buffer_add(sql, ",");
				buffer_fadd( sql, " %s = NULL", field_name );
			}
			
		} else if ( !strcmp(osrfHashGet(field, "primitive"), "number") ) {
			if (first) first = 0;
			else buffer_add(sql, ",");

			if ( !strncmp(osrfHashGet(field, "datatype"), "INT", (size_t)3) ) {
				buffer_fadd( sql, " %s = %ld", field_name, atol(value) );
			} else if ( !strcmp(osrfHashGet(field, "datatype"), "NUMERIC") ) {
				buffer_fadd( sql, " %s = %f", field_name, atof(value) );
			}

			osrfLogDebug( OSRF_LOG_MARK, "%s is of type %s", field_name, osrfHashGet(field, "datatype"));

		} else {
			if ( dbi_conn_quote_string(dbhandle, &value) ) {
				if (first) first = 0;
				else buffer_add(sql, ",");
				buffer_fadd( sql, " %s = %s", field_name, value );

			} else {
				osrfLogError(OSRF_LOG_MARK, "%s: Error quoting string [%s]", MODULENAME, value);
				osrfAppSessionStatus(
					ctx->session,
					OSRF_STATUS_INTERNALSERVERERROR,
					"osrfMethodException",
					ctx->request,
					"Error quoting string -- please see the error log for more details"
				);
				free(value);
				free(id);
				buffer_free(sql);
				*err = -1;
				return jsonNULL;
			}
		}

		free(value);
		
	}

	jsonObject* obj = jsonParseString(id);

	if ( strcmp( osrfHashGet( osrfHashGet( osrfHashGet(meta, "fields"), pkey ), "primitive" ), "number" ) )
		dbi_conn_quote_string(dbhandle, &id);

	buffer_fadd( sql, " WHERE %s = %s;", pkey, id );

	char* query = buffer_data(sql);
	buffer_free(sql);

	osrfLogDebug(OSRF_LOG_MARK, "%s: Update SQL [%s]", MODULENAME, query);

	dbi_result result = dbi_conn_query(dbhandle, query);
	free(query);

	if (!result) {
		jsonObjectFree(obj);
		obj = jsonNewObject(NULL);
		osrfLogError(
			OSRF_LOG_MARK,
			"%s ERROR updating %s object with %s = %s",
			MODULENAME,
			osrfHashGet(meta, "fieldmapper"),
			pkey,
			id
		);
	}

	free(id);

	return obj;
}

jsonObject* doDelete(osrfMethodContext* ctx, int* err ) {

	osrfHash* meta = osrfHashGet( (osrfHash*) ctx->method->userData, "class" );

	if (!osrfHashGet( (osrfHash*)ctx->session->userData, "xact_id" )) {
		osrfAppSessionStatus(
			ctx->session,
			OSRF_STATUS_BADREQUEST,
			"osrfMethodException",
			ctx->request,
			"No active transaction -- required for DELETE"
		);
		*err = -1;
		return jsonNULL;
	}

	dbhandle = writehandle;

	jsonObject* obj;

	char* pkey = osrfHashGet(meta, "primarykey");

	char* id;
	if (jsonObjectGetIndex(ctx->params, 0)->classname) {
		if (!verifyObjectClass(ctx, jsonObjectGetIndex( ctx->params, 0 ))) {
			*err = -1;
			return jsonNULL;
		}

		id = jsonObjectToSimpleString(
			jsonObjectGetIndex(
				jsonObjectGetIndex(ctx->params, 0),
				atoi( osrfHashGet( osrfHashGet( osrfHashGet(meta, "fields"), pkey ), "array_position") )
			)
		);
	} else {
		id = jsonObjectToSimpleString(jsonObjectGetIndex(ctx->params, 0));
	}

	osrfLogDebug(
		OSRF_LOG_MARK,
		"%s deleting %s object with %s = %s",
		MODULENAME,
		osrfHashGet(meta, "fieldmapper"),
		pkey,
		id
	);

	obj = jsonParseString(id);

	if ( strcmp( osrfHashGet( osrfHashGet( osrfHashGet(meta, "fields"), pkey ), "primitive" ), "number" ) )
		dbi_conn_quote_string(writehandle, &id);

	dbi_result result = dbi_conn_queryf(writehandle, "DELETE FROM %s WHERE %s = %s;", osrfHashGet(meta, "tablename"), pkey, id);

	if (!result) {
		jsonObjectFree(obj);
		obj = jsonNewObject(NULL);
		osrfLogError(
			OSRF_LOG_MARK,
			"%s ERROR deleting %s object with %s = %s",
			MODULENAME,
			osrfHashGet(meta, "fieldmapper"),
			pkey,
			id
		);
	}

	free(id);

	return obj;

}


jsonObject* oilsMakeFieldmapperFromResult( dbi_result result, osrfHash* meta) {
	if(!(result && meta)) return jsonNULL;

	jsonObject* object = jsonParseString("[]");
	jsonObjectSetClass(object, osrfHashGet(meta, "classname"));

	osrfHash* fields = osrfHashGet(meta, "fields");

	osrfLogInternal(OSRF_LOG_MARK, "Setting object class to %s ", object->classname);

	osrfHash* _f;
	time_t _tmp_dt;
	char dt_string[256];
	struct tm gmdt;

	int fmIndex;
	int columnIndex = 1;
	int attr;
	unsigned short type;
	const char* columnName;

	/* cycle through the column list */
	while( (columnName = dbi_result_get_field_name(result, columnIndex++)) ) {

		osrfLogInternal(OSRF_LOG_MARK, "Looking for column named [%s]...", (char*)columnName);

		fmIndex = -1; // reset the position
		
		/* determine the field type and storage attributes */
		type = dbi_result_get_field_type(result, columnName);
		attr = dbi_result_get_field_attribs(result, columnName);

		/* fetch the fieldmapper index */
		if( (_f = osrfHashGet(fields, (char*)columnName)) ) {
			char* virt = (char*)osrfHashGet(_f, "virtual");
			char* pos = (char*)osrfHashGet(_f, "array_position");

			if ( !virt || !pos || !(strcmp( virt, "true" )) ) continue;

			fmIndex = atoi( pos );
			osrfLogInternal(OSRF_LOG_MARK, "... Found column at position [%s]...", pos);
		} else {
			continue;
		}

		if (dbi_result_field_is_null(result, columnName)) {
			jsonObjectSetIndex( object, fmIndex, jsonNewObject(NULL) );
		} else {

			switch( type ) {

				case DBI_TYPE_INTEGER :

					if( attr & DBI_INTEGER_SIZE8 ) 
						jsonObjectSetIndex( object, fmIndex, 
							jsonNewNumberObject(dbi_result_get_longlong(result, columnName)));
					else 
						jsonObjectSetIndex( object, fmIndex, 
							jsonNewNumberObject(dbi_result_get_long(result, columnName)));

					break;

				case DBI_TYPE_DECIMAL :
					jsonObjectSetIndex( object, fmIndex, 
							jsonNewNumberObject(dbi_result_get_double(result, columnName)));
					break;

				case DBI_TYPE_STRING :


					jsonObjectSetIndex(
						object,
						fmIndex,
						jsonNewObject( dbi_result_get_string(result, columnName) )
					);

					break;

				case DBI_TYPE_DATETIME :

					memset(dt_string, '\0', 256);
					memset(&gmdt, '\0', sizeof(gmdt));
					memset(&_tmp_dt, '\0', sizeof(_tmp_dt));

					_tmp_dt = dbi_result_get_datetime(result, columnName);

					localtime_r( &_tmp_dt, &gmdt );

					if (!(attr & DBI_DATETIME_DATE)) {
						strftime(dt_string, 255, "%T", &gmdt);
					} else if (!(attr & DBI_DATETIME_TIME)) {
						strftime(dt_string, 255, "%F", &gmdt);
					} else {
						strftime(dt_string, 255, "%FT%T%z", &gmdt);
					}

					jsonObjectSetIndex( object, fmIndex, jsonNewObject(dt_string) );

					break;

				case DBI_TYPE_BINARY :
					osrfLogError( OSRF_LOG_MARK, 
						"Can't do binary at column %s : index %d", columnName, columnIndex - 1);
			}
		}
	}

	return object;
}
jsonObject* oilsMakeJSONFromResult( dbi_result result ) {
	if(!result) return jsonNULL;

	jsonObject* object = jsonParseString("{}");

	time_t _tmp_dt;
	char dt_string[256];
	struct tm gmdt;

	int fmIndex;
	int columnIndex = 1;
	int attr;
	unsigned short type;
	const char* columnName;

	/* cycle through the column list */
	while( (columnName = dbi_result_get_field_name(result, columnIndex++)) ) {

		osrfLogInternal(OSRF_LOG_MARK, "Looking for column named [%s]...", (char*)columnName);

		fmIndex = -1; // reset the position
		
		/* determine the field type and storage attributes */
		type = dbi_result_get_field_type(result, columnName);
		attr = dbi_result_get_field_attribs(result, columnName);

		if (dbi_result_field_is_null(result, columnName)) {
			jsonObjectSetKey( object, columnName, jsonNewObject(NULL) );
		} else {

			switch( type ) {

				case DBI_TYPE_INTEGER :

					if( attr & DBI_INTEGER_SIZE8 ) 
						jsonObjectSetKey( object, columnName, jsonNewNumberObject(dbi_result_get_longlong(result, columnName)) );
					else 
						jsonObjectSetKey( object, columnName, jsonNewNumberObject(dbi_result_get_long(result, columnName)) );
					break;

				case DBI_TYPE_DECIMAL :
					jsonObjectSetKey( object, columnName, jsonNewNumberObject(dbi_result_get_double(result, columnName)) );
					break;

				case DBI_TYPE_STRING :
					jsonObjectSetKey( object, columnName, jsonNewObject(dbi_result_get_string(result, columnName)) );
					break;

				case DBI_TYPE_DATETIME :

					memset(dt_string, '\0', 256);
					memset(&gmdt, '\0', sizeof(gmdt));
					memset(&_tmp_dt, '\0', sizeof(_tmp_dt));

					_tmp_dt = dbi_result_get_datetime(result, columnName);

					localtime_r( &_tmp_dt, &gmdt );

					if (!(attr & DBI_DATETIME_DATE)) {
						strftime(dt_string, 255, "%T", &gmdt);
					} else if (!(attr & DBI_DATETIME_TIME)) {
						strftime(dt_string, 255, "%F", &gmdt);
					} else {
						strftime(dt_string, 255, "%FT%T%z", &gmdt);
					}

					jsonObjectSetKey( object, columnName, jsonNewObject(dt_string) );
					break;

				case DBI_TYPE_BINARY :
					osrfLogError( OSRF_LOG_MARK, 
						"Can't do binary at column %s : index %d", columnName, columnIndex - 1);
			}
		}
	}

	return object;
}

