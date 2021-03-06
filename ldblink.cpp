#include "Vertica.h"
#include "StringParsers.h"

using namespace Vertica;
using namespace std;

#include <sql.h>
#include <time.h>
#include <sqlext.h>
#include <iostream>
#include <fstream>
  
#define DBLINK_CIDS			"/usr/local/etc/dblink.cids"	// Default Connection identifiers config file FIX: add a param
#define MAXCNAMELEN			128								// Max column name length
#define DEF_ROWSET 			100								// Default rowset
#define MAX_ROWSET 			1000							// Default rowset
#define MAX_NUMERIC_CHARLEN 128								// Max NUMERIC size in characters

SQLHENV Oenv = 0 ;				// ODBC Environment handle
SQLHENV Ocon = 0 ;				// ODBC Connection  handle
SQLHSTMT Ost = 0 ;				// ODBC Statement  handle
bool is_select = false ;		// Command is a SELECT
std::string query = "" ;		// set in Factory/getReturnType, used (non-DQL) in processPartition
SQLUSMALLINT Oncol = 0 ;		// Number of result set columns
SQLSMALLINT *Odt = 0 ;			// Result set Data Type Array pointer
SQLSMALLINT *Odd = 0 ;			// Result set Decimals Array pointer
SQLULEN *Ors = 0 ;				// Result Set Column size pointer
size_t *desz = 0 ;				// Data Element Size Array pointer
SQLULEN nfr ;					// Number of fetched rows
SizedColumnTypes colInfo ;		// Set in getReturnType factory, used in processPartition

enum DBs {
    GENERIC = 0,
    POSTGRES,
    VERTICA,
    SQLSERVER,
    TERADATA,
    ORACLE,
    MYSQL
};

class DBLink : public TransformFunction
{

	DBs dbt ;
	SQLPOINTER *Ores ;     // result array pointers pointer
    SQLLEN **Olen ;        // length array pointers pointer
	StringParsers parser ;
	size_t rowset ;			// Fetch rowset

	virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
	{
		SQLRETURN Oret = 0 ;
		SQLHSTMT Ostmt = 0 ;
		SQLCHAR Obuff[64];

		memset(&Obuff[0], 0, sizeof(Obuff));

		// Check the DBMS we are connecting to:
		if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_STMT, Ocon, &Ostmt))){
			vt_report_error(201, "DBLINK. Error allocating Statement Handle");                                                                                              
		}
		if (!SQL_SUCCEEDED(Oret=SQLGetInfo(Ocon, SQL_DBMS_NAME,                                                                                                                
        	(SQLPOINTER)Obuff, (SQLSMALLINT)sizeof(Obuff), NULL))) {
			vt_report_error(202, "DBLINK. Error getting remote DBMS Name");
    	}
		if ( !strcmp((char *)Obuff, "Oracle") ) {
			dbt = ORACLE ;
		} else {
			dbt = GENERIC ;
		}
        (void)SQLFreeHandle(SQL_HANDLE_STMT, Ostmt);     

		// Read/Set rowset Param:
		ParamReader params = srvInterface.getParamReader();
		if( params.containsParameter("rowset") ) {
			vint rowset_param = params.getIntRef("rowset") ;
			if ( rowset_param < 1 || rowset_param > MAX_ROWSET ) {
				vt_report_error(203, "DBLINK. Error rowset (%zd) out of range[1,%d]", rowset_param, MAX_ROWSET);
			} else {
				rowset = (size_t) rowset_param ;
			}
		} else {
			rowset = DEF_ROWSET ;
		}
	}

    virtual void cancel(ServerInterface &srvInterface)
    {   
		SQLRETURN Oret = 0 ;
		if ( Ost ) {
			if (!SQL_SUCCEEDED(Oret=SQLCancel(Ost)))
				vt_report_error(301, "DBLINK. Error canceling SQL statement");
        }
    }   

    virtual void destroy(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {   
		if ( Odt ) 
			free(Odt) ;
		if ( Odd )
			free(Odd) ;
		if ( Ors ) 
			free(Ors);
		if ( desz ) 
			free(desz);
		if ( Ost )
        	(void)SQLFreeHandle(SQL_HANDLE_STMT, Ost);     
		if ( Ocon )
        	(void)SQLFreeHandle(SQL_HANDLE_DBC, Ocon);     
		if ( Oenv )
        	(void)SQLFreeHandle(SQL_HANDLE_ENV, Oenv);     
    }   

    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader & inputReader,
                                  PartitionWriter & outputWriter)
	{
		SQLRETURN Oret = 0 ;
		SQLPOINTER Odp = 0 ; // Data Element Pointer
		SQLULEN    Odl = 0 ; // Data Element Length

		try
		{
			if ( is_select ) {

				// Allocate memory for Result Set and length array pointers:
				Ores = (SQLPOINTER *)srvInterface.allocator->alloc(Oncol * sizeof(SQLPOINTER)) ;
				Olen = (SQLLEN **)srvInterface.allocator->alloc(Oncol * sizeof(SQLLEN *)) ;        

				// Allocate space for each column and bind it:
				for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
					Olen[j] = (SQLLEN *)srvInterface.allocator->alloc(sizeof(SQLLEN) * rowset);
					switch(Odt[j]) {
						case SQL_SMALLINT:
						case SQL_INTEGER:
						case SQL_TINYINT:
						case SQL_BIGINT:
							if ( dbt == ORACLE ) 
								desz[j] = (size_t)(Ors[j] + 1) ;
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, (dbt==ORACLE) ? SQL_C_CHAR : SQL_C_SBIGINT, Ores[j], desz[j], Olen[j]))){
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							}
							break ;
						case SQL_REAL:
						case SQL_DOUBLE:
						case SQL_FLOAT:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_DOUBLE, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_NUMERIC:
						case SQL_DECIMAL:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_CHAR:
						case SQL_WCHAR:
						case SQL_VARCHAR:
						case SQL_WVARCHAR:
						case SQL_LONGVARCHAR:
						case SQL_WLONGVARCHAR:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_TYPE_TIME:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_TIME, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_TYPE_DATE:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_DATE, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_TYPE_TIMESTAMP:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_TIMESTAMP, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_BIT:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_BIT, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_BINARY:
						case SQL_VARBINARY:
						case SQL_LONGVARBINARY:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_BINARY, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_INTERVAL_YEAR_TO_MONTH:
							// FIX: support this data type
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_INTERVAL_YEAR_TO_MONTH, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break ;
						case SQL_INTERVAL_DAY_TO_SECOND:
							// FIX: support this data type
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_INTERVAL_DAY_TO_SECOND, Ores[j], desz[j], Olen[j])))
								vt_report_error(401, "DBLINK. Error binding col %u", j);
							break;
					}
				}
				// Set Statement attributes:
				if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0))) {
					vt_report_error(402, "DBLINK. Error setting statement attribute SQL_ATTR_ROW_BIND_TYPE");
				}
				if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)rowset, 0))) {
					vt_report_error(402, "DBLINK. Error setting statement attribute SQL_ATTR_ROW_ARRAY_SIZE");
				}
				if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROWS_FETCHED_PTR, &nfr, 0))) {
					vt_report_error(402, "DBLINK. Error setting statement attribute SQL_ATTR_ROWS_FETCHED_PTR");
				}

				// Execute Stateent:
				if (!SQL_SUCCEEDED(Oret=SQLExecute(Ost)) && Oret != SQL_NO_DATA ) {
					vt_report_error(403, "DBLINK. Error executing the statement");
				}

				// Fetch loop:
				while ( SQL_SUCCEEDED(Oret=SQLFetch(Ost)) && !isCanceled() ) {
					for ( unsigned int i = 0 ; i < nfr ; i++, outputWriter.next() ) {
						for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
							Odp = (SQLPOINTER)((uint8_t *)Ores[j] + desz[j] * i) ;
							Odl = Olen[j][i] ;
							
							if ( (int)Odl == (int)SQL_NULL_DATA ) {
								outputWriter.setNull(j) ;
								continue ;
							}

							
							switch(Odt[j]) {
								case SQL_SMALLINT:
								case SQL_INTEGER:
								case SQL_TINYINT:
								case SQL_BIGINT:
									if ( dbt == ORACLE ) {
										outputWriter.setInt(j, ((int)Odl == SQL_NTS) ? vint_null : (vint)atoll((char *)Odp) ) ;
									} else {
										outputWriter.setInt(j, *(SQLBIGINT *)Odp) ;
									}
									break ;
								case SQL_REAL:
								case SQL_DOUBLE:
								case SQL_FLOAT:
									outputWriter.setFloat(j, *(SQLDOUBLE *)Odp) ;
									break ;
								case SQL_NUMERIC:
								case SQL_DECIMAL:
									{
										std::string rejectReason = "Unrecognized remote database format" ;
										if ( *(char *)Odp == '\0' ) { // some DBs might use empty strings for NUMERIC nulls
											outputWriter.setNull(j) ;
										} else {

											if (!parser.parseNumeric((char*)Odp, (size_t)Odl, j,
													outputWriter.getNumericRef(j), colInfo.getColumnType(j), rejectReason)) {
												vt_report_error(404, "DBLINK. Error parsing Numeric: '%s'", (char*)Odp);
											}
										}
										break ;
									}
								case SQL_CHAR:
								case SQL_WCHAR:
								case SQL_VARCHAR:
								case SQL_WVARCHAR:
								case SQL_LONGVARCHAR:
								case SQL_WLONGVARCHAR:
								case SQL_BINARY:
								case SQL_VARBINARY:
								case SQL_LONGVARBINARY:
									if ( (int)Odl == SQL_NTS ) 
										Odl = (SQLULEN)strnlen((char *)Odp , desz[j]);
									outputWriter.getStringRef(j).copy((char *)Odp, Odl ) ;
									break ;
								case SQL_TYPE_TIME:
									{
										SQL_TIME_STRUCT &st = *(SQL_TIME_STRUCT *)Odp ;
										outputWriter.setTime(j, getTimeFromUnixTime(st.second + st.minute * 60 + st.hour * 3600 ) );
										break ;
									}
								case SQL_TYPE_DATE:
									{
										SQL_DATE_STRUCT &sd = *(SQL_DATE_STRUCT *)Odp ;
										struct tm d = { 0, 0, 0, sd.day, sd.month - 1, sd.year - 1900, 0, 0, -1 } ;
										time_t utime = mktime ( &d ) ;
										outputWriter.setDate(j, getDateFromUnixTime(utime + d.tm_gmtoff) ) ;
										break ;
									}
								case SQL_TYPE_TIMESTAMP:
									{
										SQL_TIMESTAMP_STRUCT &ss = *(SQL_TIMESTAMP_STRUCT *)Odp ;
										struct tm ts = { ss.second, ss.minute, ss.hour, ss.day, ss.month - 1, ss.year - 1900, 0, 0, -1 } ;
										time_t utime = mktime ( &ts ) ;
										outputWriter.setTimestamp(j, getTimestampFromUnixTime(utime + ts.tm_gmtoff) + ss.fraction / 1000 ) ;
										break ;
									}
								case SQL_BIT:
									outputWriter.setBool(j, *(SQLCHAR *)Odp == SQL_TRUE ? VTrue : VFalse);
									break ;
								case SQL_INTERVAL_YEAR_TO_MONTH: // Vertica stores these Intervals as durations in months
									{
                        				SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)Odp;
										if (intv.interval_type != SQL_IS_YEAR_TO_MONTH) {
											vt_report_error(405, "DBLINK. Unsupported INTERVAL data type. Expecting SQL_IS_YEAR_TO_MONTH");
										}
										Interval ret = (  (intv.intval.year_month.year*MONTHS_PER_YEAR)
														+ (intv.intval.year_month.month))
														* (intv.interval_sign == SQL_TRUE ? -1 : 1);
										outputWriter.setInterval(j, ret);
										break ;
									}
								case SQL_INTERVAL_DAY_TO_SECOND: // Vertica stores these Intervals as durations in microseconds
									{
                        				SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)Odp;
										if (intv.interval_type != SQL_IS_DAY_TO_SECOND) {
											vt_report_error(406, "DBLINK. Unsupported INTERVAL data type. Expecting SQL_IS_DAY_TO_SECOND");
										}
										
										Interval ret = (  (intv.intval.day_second.day*usPerDay)
														+ (intv.intval.day_second.hour*usPerHour)
														+ (intv.intval.day_second.minute*usPerMinute)
														+ (intv.intval.day_second.second*usPerSecond)
														+ (intv.intval.day_second.fraction/1000))
														* (intv.interval_sign == SQL_TRUE ? -1 : 1);
										outputWriter.setInterval(j, ret);
										break ;
									}
								default:
									vt_report_error(407, "DBLINK. Unsupported data type for column %u", j);
									break ;
							}
						}
					}
				}
			} else {
				if (!SQL_SUCCEEDED(Oret=SQLExecDirect (Ost, (SQLCHAR *)query.c_str(), SQL_NTS))) {
					vt_report_error(408, "DBLINK. Error executing <%s>", query.c_str());
				}
				outputWriter.setInt(0, (vint)Oret) ;
				outputWriter.next() ;
			}
		}
		catch (exception& e)
		{
			vt_report_error(400, "Exception while processing partition: [%s]", e.what());
		}
	}
};

class DBLinkFactory : public TransformFunctionFactory
{
	virtual void getPrototype(ServerInterface &srvInterface,
                              ColumnTypes &argTypes,
                              ColumnTypes &returnType )
	{
		returnType.addAny();
	}
	virtual void getReturnType(ServerInterface &srvInterface,
                               const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes )
	{
		SQLRETURN Oret = 0 ;
		SQLSMALLINT Onamel = 0 ;
		SQLSMALLINT Onull = 0 ;
		SQLCHAR Ocname[MAXCNAMELEN] ;
		std::string cid = "" ;
		std::string cid_name = "" ;
		std::string cid_value = "" ;
		bool connect = false ;

		// Read Params:
		ParamReader params = srvInterface.getParamReader();
		if( params.containsParameter("cid") ) {
			cid = params.getStringRef("cid").str() ;
		} else if( params.containsParameter("connect") ) {
			connect = true ;
			cid = params.getStringRef("connect").str() ;
		} else {
			vt_report_error(101, "DBLINK. Missing connection parameters");
		}
		if( params.containsParameter("query") ) {
			query = params.getStringRef("query").str() ;
		} else {
			vt_report_error(102, "DBLINK. Missing query parameter");
		}

		// Check connection parameters
		if ( connect ) { 	// old VFQ connect style:
			if ( cid[0] == '@' ) {
				std::ifstream cids(cid.substr(1)) ;
				if ( cids.is_open() ) {
					std::stringstream ssFile;
					ssFile << cids.rdbuf() ;
					cid_value = ssFile.str() ;
					cid_value.erase(std::remove(cid_value.begin(), cid_value.end(), '\n'), cid_value.end());
				} else {
					vt_report_error(103, "DBLINK. Error reading <%s>", cid.substr(1).c_str());
				}
			} else {
				cid_value = cid ;
			}
		} else {			// new CID connect style:
			std::ifstream cids(DBLINK_CIDS) ;
			if ( cids.is_open() ) {
				std::string cline ;
				size_t pos ;
				while ( getline(cids, cline) ) {
					if ( cline[0] == '#' || cline.empty() )
						continue ;	// skip empty lines & comments
					if ( ( pos = cline.find(":") ) != std::string::npos ) {
						cid_name = cline.substr(0, pos) ;
						if ( cid_name == cid )
							cid_value = cline.substr(pos + 1, std::string::npos ) ;
					} else {
						continue ;	// skip malformed lines
					}
				}
				cids.close() ;
			} else {
				vt_report_error(104, "DBLINK. Error reading <%s>", DBLINK_CIDS);
			}
			if ( cid_value.empty() ) {
				vt_report_error(105, "DBLINK. Error finding CID <%s> in <%s>", cid.c_str(), DBLINK_CIDS);
			}
		}

		// Check if "query" is a script file name:
        if ( query[0] == '@' ) {
            std::ifstream qscript(query.substr(1)) ;
			if ( qscript.is_open() ) {
				std::stringstream ssFile;
				ssFile << qscript.rdbuf() ;
				query = ssFile.str() ;
			} else {
				vt_report_error(106, "DBLINK. Error reading query from <%s>", query.substr(1).c_str());
			}
		}

		// ODBC Connection:
		if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_ENV, (SQLHANDLE)SQL_NULL_HANDLE, &Oenv))){
			vt_report_error(107, "DBLINK. Error allocating Environment Handle");                                                                                              
		}
		if (!SQL_SUCCEEDED(Oret=SQLSetEnvAttr(Oenv, SQL_ATTR_ODBC_VERSION, (void *) SQL_OV_ODBC3, 0))){
			vt_report_error(108, "DBLINK. Error setting SQL_OV_ODBC3");
		}
		if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_DBC, Oenv, &Ocon))){
			vt_report_error(109, "DBLINK. Error allocating Connection Handle");                                                                                              
		}
		if (!SQL_SUCCEEDED(Oret=SQLDriverConnect(Ocon, NULL, (SQLCHAR *)cid_value.c_str(), SQL_NTS, NULL, 0, NULL, SQL_DRIVER_COMPLETE))){
			vt_report_error(110, "DBLINK. Error connecting to <%s>", cid.c_str());
		}

		// Determine Statement type:
		query.erase(0, query.find_first_not_of(" \n\t\r")) ;
		if ( !strncasecmp(query.c_str(), "SELECT", 6) )
			is_select = true ;

		// ODBC Statement execution:
		if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_STMT, Ocon, &Ost))){
			vt_report_error(111, "DBLINK. Error allocating Statement Handle");                                                                                              
		}
		if ( is_select ) {
			if (!SQL_SUCCEEDED(Oret=SQLPrepare(Ost, (SQLCHAR *)query.c_str(), SQL_NTS))) {
				vt_report_error(112, "DBLINK. Error preparing the statement");
    		}
			if (!SQL_SUCCEEDED(Oret=SQLNumResultCols(Ost, (SQLSMALLINT *)&Oncol))) {
				vt_report_error(115, "DBLINK. Error finding the number of resulting columns");
			}
			if ( (Odt = (SQLSMALLINT *)calloc ((size_t)Oncol, sizeof(SQLSMALLINT))) == (void *)NULL ) {
				vt_report_error(116, "DBLINK. Error allocating data types array");
    		}
			if ( (Ors = (SQLULEN *)calloc ((size_t)Oncol, sizeof(SQLULEN))) == (void *)NULL ) {
				vt_report_error(117, "DBLINK. Error allocating result set columns size array");
    		}
			if ( (desz = (size_t *)calloc ((size_t)Oncol, sizeof(size_t))) == (void *)NULL ) {
				vt_report_error(118, "DBLINK. Error allocating data element size array");
    		}
			if ( (Odd = (SQLSMALLINT *)calloc ((size_t)Oncol, sizeof(SQLSMALLINT))) == (void *)NULL ) {
				vt_report_error(119, "DBLINK. Error allocating result set decimal size array");
    		}
			for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
				if ( !SQL_SUCCEEDED(Oret=SQLDescribeCol(Ost, (SQLUSMALLINT)(j+1),
                		Ocname, (SQLSMALLINT) MAXCNAMELEN, &Onamel,
                		&Odt[j], &Ors[j], &Odd[j], &Onull))) {
					vt_report_error(120, "DBLINK. Error getting description for column %u", j);
				}
				std::string cname((char *)Ocname);
				switch(Odt[j]) {
					case SQL_SMALLINT:
					case SQL_INTEGER:
					case SQL_TINYINT:
					case SQL_BIGINT:
						// we change this later on if the remote db is Oracle 
						desz[j] = sizeof(vint) ;
						outputTypes.addInt(cname) ;
						break ;
					case SQL_REAL:
					case SQL_DOUBLE:
					case SQL_FLOAT:
						desz[j] = sizeof(vfloat) ;
						outputTypes.addFloat(cname) ;
						break ;
					case SQL_NUMERIC:
					case SQL_DECIMAL:
						desz[j] = MAX_NUMERIC_CHARLEN ;
						outputTypes.addNumeric((int32)Ors[j], (int32)Odd[j], cname) ;
						break ;
					case SQL_CHAR:
					case SQL_WCHAR:
						desz[j] = (size_t)(Ors[j] + 1) ;
						if ( !Ors[j] )
							Ors[j] = 1 ;
						outputTypes.addChar((int32)Ors[j], cname) ;
						break ;
					case SQL_VARCHAR:
					case SQL_WVARCHAR:
						desz[j] = (size_t)(Ors[j] + 1) ;
						if ( !Ors[j] )
							Ors[j] = 1 ;
						outputTypes.addVarchar((int32)Ors[j], cname) ;
						break ;
					case SQL_LONGVARCHAR:
					case SQL_WLONGVARCHAR:
						desz[j] = (size_t)(Ors[j] + 1) ;
						if ( !Ors[j] )
							Ors[j] = 1 ;
						outputTypes.addLongVarchar((int32)Ors[j], cname) ;
						break ;
					case SQL_TYPE_TIME:
						desz[j] = sizeof(SQL_TIME_STRUCT) ;
						outputTypes.addTime((int32)Odd[j], cname) ;
						break ;
					case SQL_TYPE_DATE:
						desz[j] = sizeof(SQL_DATE_STRUCT) ;
						outputTypes.addDate(cname) ;
						break ;
					case SQL_TYPE_TIMESTAMP:
						desz[j] = sizeof(SQL_TIMESTAMP_STRUCT) ;
						outputTypes.addTimestamp((int32)Odd[j], cname) ;
						break ;
					case SQL_BIT:
						desz[j] = 1 ;
						outputTypes.addBool(cname) ;
						break ;
					case SQL_BINARY:
						desz[j] = (size_t)(Ors[j] + 1) ;
						outputTypes.addBinary((int32)Ors[j], cname) ;
						break ;
					case SQL_VARBINARY:
						desz[j] = (size_t)(Ors[j] + 1) ;
						outputTypes.addVarbinary((int32)Ors[j], cname) ;
						break ;
					case SQL_LONGVARBINARY:
						desz[j] = (size_t)(Ors[j] + 1) ;
						outputTypes.addLongVarbinary((int32)Ors[j], cname) ;
						break ;
					case SQL_INTERVAL_YEAR_TO_MONTH:
						desz[j] = sizeof(SQL_INTERVAL_STRUCT) ;
						outputTypes.addIntervalYM(INTERVAL_YEAR2MONTH, cname) ;
						break ;
					case SQL_INTERVAL_DAY_TO_SECOND:			
						desz[j] = sizeof(SQL_INTERVAL_STRUCT) ;
						outputTypes.addInterval((int32)Odd[j], INTERVAL_DAY2SECOND, cname) ;
						break ;
					default:
						vt_report_error(121, "DBLINK. Unsupported data type for column %u", j);
				}
        	}
			colInfo = outputTypes ;
		} else {
			outputTypes.addInt("dblink") ;
		}
	}
    virtual void getParameterType(ServerInterface &srvInterface,
								  SizedColumnTypes &parameterTypes)
	{
		parameterTypes.addVarchar(1024, "cid");
		parameterTypes.addVarchar(1024, "connect");
		parameterTypes.addVarchar(65000, "query");
		parameterTypes.addInt("rowset");
	}

	virtual TransformFunction *createTransformFunction( ServerInterface &srvInterface )
	{
		return vt_createFuncObject<DBLink>(srvInterface.allocator);
	}
};
RegisterFactory(DBLinkFactory);

RegisterLibrary (
	"Maurizio Felici",
	__DATE__,
	"0.1.0",
	"11.0.1",
	"maurizio.felici@vertica.com",
	"DBLINK: run SQL on other databases",
	"",
	""
);
