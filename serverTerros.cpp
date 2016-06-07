
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <vector>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "local.h"
#include <postgresql/libpq-fe.h>
#include "dbconnectionsettings.h"

using namespace std;

//for log
FILE * logFd = NULL;

//struct to saving data between threads-pools
vector <pool_t *> pool;

//counters
int poolId = 0;

//server listener params
int nPort = SERVER_PORT;
char pcHost[] = SERVER_HOST;
int fdListener;

//definition of thread foo
void * socket64Thread(void * lpParam);
void * supervisor(void * lpParam);

//critical sections
static pthread_mutex_t csDebug = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t csPool = PTHREAD_MUTEX_INITIALIZER;

//for db
PGconn*    connection_DB;
PGresult*  result_DB_transaction;
static pthread_mutex_t dbAccess = PTHREAD_MUTEX_INITIALIZER;

//
// FOOS
//

//
// debug print
//
void log(const char * str, ...) {
    va_list arg;
    char out[SIZE_OF_BUFFER];

    pthread_mutex_lock(&csDebug);

	//glue string with variable raguments
    va_start(arg, str);
    memset(out, 0, SIZE_OF_BUFFER);
    vsprintf(out, str, arg);
    va_end(arg);

	//check log output fd is created
	if (logFd == NULL) {
		char fileName[1024];
		
		time_t ltime;
		struct tm *Tm;
		ltime=time(NULL);
		Tm=localtime(&ltime);
 
		memset(fileName, 0 ,1024);
		sprintf(fileName, "/var/log/serverTerros/log_%04d-%02d-%02d_%02d.%02d.%02d",
            Tm->tm_year+1900,
			Tm->tm_mon+1,
			Tm->tm_mday,
            Tm->tm_hour,
            Tm->tm_min,
            Tm->tm_sec);
		logFd = fopen(fileName, "w");
    }
	
	if (logFd != NULL) {
		fprintf(logFd, "%s\r\n", out);
		fflush(logFd);
	}
	
    printf("%s\r\n", out);

    pthread_mutex_unlock(&csDebug);
}

//
// CONNECT TO DB 
//
void connectDb(void)
{
	connection_DB = PQconnectdb(DB_CONNECTION_SETTINGS);

	/* connecting to postgres data base */
	if ( PQstatus(connection_DB)  == CONNECTION_BAD)
	{
		log("DB: Connection error");
		exit(0);
	}
	else
	{
		log("DB: Connection ok");
	}
}

//
// CLOSE CONNECTION FOO
//

int ShutdownConnection(int sd)
{
	//debug
	log ("try to close socket %d", sd);
	
    // Disallow any further data sends. 
    shutdown(sd, 1);

    // Receive any extra data still sitting on the socket.
    char acReadBuffer[SIZE_OF_BUFFER];
    while (1) {
        int nNewBytes = recv(sd, acReadBuffer, SIZE_OF_BUFFER, 0);
        if (nNewBytes < 0) {
            break;
        }
        else if (nNewBytes != 0) {
			log ("some recv while close");
        }
        else {
            break;
        }
    }

    // Close the socket.
    close(sd) ;

    return 1;
}

//
// DATA BASE ROUTINS
//
int checkEthSnInDb(connection_t& conn);
int saveConnected(char * imei);
int saveDisconnected(char * imei);
int setOnlineState(char * imei, int online);
int saveEventToDb(char * imei, int ev_type, char * ev_time, char * ev_text);
int checkDtssInDb(connection_t& conn, dateTimeStamp **nextDtss);
void saveArchiveAnswerInDb(connection_t& conn);
void saveArchiveAnswerInDbAsEmpty(connection_t& conn);
void saveCurrentAnswerInDb(connection_t& conn);
void setActiveStatusInDb(connection_t& conn);
void resetStatusInDb();
void saveEmptyAndGoToReadTime(connection_t& conn);

//
//
//
int saveEventToDb(char * imei, int ev_type, char * ev_time, char * ev_text)
{
	pthread_mutex_lock(&dbAccess);
	
	int resValue = -1;
	char SQL_request[1024];

	int i = 0;
	int len = strlen(ev_text);
	while (i<len){
		if (ev_text[i] == '\''){
			ev_text[i] = ' ';
		}
		i++;
	}

    memset(SQL_request, 0, 1024);
	if (ev_time == NULL){
		sprintf(SQL_request, "select * from set_modem_event('%s','%d', NULL, '%s')", imei, ev_type, ev_text);
	} else{		
		//move event type 1 up to 3 hours
		//timestamp '2012-01-01 23:00:00' + interval '3 hour'
		if ((ev_type != 9) && (ev_type != 8)){
			sprintf(SQL_request, "select * from set_modem_event('%s','%d', timestamp '%s' + interval '4 hours', '%s')", imei, ev_type, ev_time, ev_text);
		} else {
			sprintf(SQL_request, "select * from set_modem_event('%s','%d', timestamp '%s' + interval '1 hour', '%s')", imei, ev_type, ev_time, ev_text);
		}
	}
	
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if (PQresultStatus(result_DB_transaction) == PGRES_TUPLES_OK){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if( nrows == 1 || nfields == 1){
			resValue = atoi(PQgetvalue(result_DB_transaction, 0, 0));
		}
	}

	PQclear(result_DB_transaction);
	
	pthread_mutex_unlock(&dbAccess);
	
	return resValue;	
}

//
//
//
int saveDisconnected(char * imei){
	int res = 1;
	char event[256];
	memset(event, 0, 256);
	sprintf(event, "disconnected from server");
	res = saveEventToDb(imei, 10, NULL, event);
	return res;
}

//
//
//
int saveConnected(char * imei){
	int res = 1;
	char event[256];
	memset(event, 0, 256);
	sprintf(event, "connected to server");
	res = saveEventToDb(imei, 9, NULL, event);
	return res;
}

//
//
//
int setOnlineState(char * imei, int online){
	
	int resValue = -1;
	int resValue2 = -1;
	char SQL_request[256];

	memset(SQL_request, 0, sizeof(SQL_request));
	sprintf(SQL_request, "select * from set_modem_online('%s','%d')", imei, online);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if (PQresultStatus(result_DB_transaction) == PGRES_TUPLES_OK){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if( nrows == 1 || nfields == 1){
			resValue = atoi(PQgetvalue(result_DB_transaction, 0, 0)) - 1;
		}
	}	
	PQclear(result_DB_transaction);
	
	pthread_mutex_unlock(&dbAccess);
	
	if(online == 0)
		resValue2 = saveDisconnected(imei);	
	else
		resValue2 = saveConnected(imei);
	
	return resValue;
	
}

//foo to save archive hour event
//function modemSaveArchiveHourEvent() {
//	include ("z_php_db_connect.php");
//	$query = "select * from set_modem_event('".$_POST['modem_id']."',11, NULL, 'H mtr for [".$_POST['a_dts']."]')";
//	$result = pg_query($dbconn, $query);
//}	

//foo to save archive day event
//function modemSaveArchiveDayEvent() {
//	include ("z_php_db_connect.php");
//	$query = "select * from set_modem_event('".$_POST['modem_id']."',11, NULL, 'D mtr for [".$_POST['a_dts']."]')";
//	$result = pg_query($dbconn, $query);
//}	


//
//
//
int checkEthSnInDb(connection_t& conn){

	pthread_mutex_lock(&dbAccess);
	
	int resValue = -1;
	char SQL_request[512];
	char checkSn[10];
	
	//check minimum len
	if (conn.nCharsInBuffer < 9){
		pthread_mutex_unlock(&dbAccess);
		return 0;
	}
	else {
		memset(checkSn, 0, 10);
		memcpy(checkSn, conn.inBuffer, 9);
	}
	
	log ("check presence of modem with SN [%s]", checkSn);
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select * from check_connected_modemEth('%s')", checkSn);
	result_DB_transaction  = PQexec(connection_DB, SQL_request);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK ){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if((nrows == 1) && (nfields == 1)){
			resValue = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
		}
	}
	
	PQclear(result_DB_transaction);
	
	//check modem was not found
	if (resValue == -1) {
		pthread_mutex_unlock(&dbAccess);
		log ("eth modem not found");
		return 0;
		
	} else {
		conn.ethDbId = resValue;
		memset(conn.ethSn, 0, ETH_SN_SIZE+1);
		memcpy(conn.ethSn, conn.inBuffer, ETH_SN_SIZE);
	}
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select c.id, cp.address, cp.tarifs from \"COUNTER\" c, \"COUNTER_PARAM\" cp where c.id_modem = %d and c.id_counter_param = cp.id and cp.id_type = 41", resValue);
	result_DB_transaction  = PQexec(connection_DB, SQL_request);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK )
	{
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		
		if((nrows == 1) && (nfields == 3))
		{
			conn.terrosDbId = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
			conn.terrosSn = atoi( PQgetvalue( result_DB_transaction, 0, 1 ) );
			
			if (strstr(PQgetvalue( result_DB_transaction, 0, 2 ), "T1") !=NULL ) conn.terrosPipes+=0x1;
			if (strstr(PQgetvalue( result_DB_transaction, 0, 2 ), "T2") !=NULL ) conn.terrosPipes+=0x2;
			if (strstr(PQgetvalue( result_DB_transaction, 0, 2 ), "T3") !=NULL ) conn.terrosPipes+=0x4;
			if (strstr(PQgetvalue( result_DB_transaction, 0, 2 ), "T4") !=NULL ) conn.terrosPipes+=0x8;
		}
	}
	
	PQclear(result_DB_transaction);

	//debug
	//if (conn.terrosSn != 3217)
	//	return 0;
		
	log ("---\r\nNEW CONNECTION:\r\nISM SN %s\r\nTEROSS SN %d\r\nTEROSS ID %lu\r\nTEROSS PM %d\r\n---", conn.ethSn, conn.terrosSn, conn.terrosDbId, conn.terrosPipes);
	
	setOnlineState(conn.ethSn, 1);
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select * from server_status_terros_set('%s', '%d', %d, %d, %d);", conn.ethSn, conn.terrosSn, 2, conn.sd, 0);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK ){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if((nrows == 1) && (nfields == 1)){
			resValue = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
		}
	}
	
	
	pthread_mutex_unlock(&dbAccess);
	
	return 1;
}

//
//
//
int checkDtssInDb(connection_t& conn, dateTimeStamp **nextDtss){
	
	pthread_mutex_lock(&dbAccess);
	
	char SQL_request[512];
	
	for (int archIndex=0; archIndex<3;archIndex++){
		for (int pipeindex=0; pipeindex<4;pipeindex++){		
			memset( SQL_request, 0, sizeof( SQL_request ) );
			switch (archIndex){
				case 0: sprintf(SQL_request, "select * from get_next_q_time_hour_w_pipe('%s', %d); ", conn.ethSn, (pipeindex+1)); break;
				case 1: sprintf(SQL_request, "select * from get_next_q_time_day_w_pipe('%s', %d);  ", conn.ethSn, (pipeindex+1)); break;
				case 2: sprintf(SQL_request, "select * from get_next_q_time_month_w_pipe('%s', %d);", conn.ethSn, (pipeindex+1)); break;
			}
			result_DB_transaction = PQexec(connection_DB, SQL_request);
			if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK ){
				int nrows = PQntuples( result_DB_transaction );
				int nfields = PQnfields( result_DB_transaction );
				
				//printf ("<<<<checkDtssInDb [arch: %d pipe: %d]>>>>\r\n", archIndex, pipeindex);
				//printf ("PQgetvalue %s\r\n", PQgetvalue( result_DB_transaction, 0, 0 ));
				if((nrows == 1) && (nfields == 1)){
					int y;
					int m;
					int d;
					int h;
					int mn;
					int sc;
					if (sscanf(PQgetvalue( result_DB_transaction, 0, 0), "%d-%d-%d %d:%d:%d", &y, &m, &d, &h, &mn, &sc) == 6){
						(*nextDtss)[((archIndex*4)+pipeindex)].d.y = y-2000;
						(*nextDtss)[((archIndex*4)+pipeindex)].d.m = m;
						(*nextDtss)[((archIndex*4)+pipeindex)].d.d = d;
						(*nextDtss)[((archIndex*4)+pipeindex)].t.h = h;
						(*nextDtss)[((archIndex*4)+pipeindex)].t.m = mn;
						(*nextDtss)[((archIndex*4)+pipeindex)].t.s = sc;
					} else {
						//log ("error parsing dateStamp from Database: sscnaf not ok");
						pthread_mutex_unlock(&dbAccess);
						return 0;
					}
				} else {
					//log ("error parsing dateStamp from Database: count of db fields not ok");
					pthread_mutex_unlock(&dbAccess);
					return 0;
				}
			} else {
				//log ("error parsing dateStamp from Database: tupples not ok");
				pthread_mutex_unlock(&dbAccess);
				return 0;
			}
			
			PQclear(result_DB_transaction);
			
		}
	}

	pthread_mutex_unlock(&dbAccess);
	
	return 1;
}

void saveArchiveAnswerInDb(connection_t& conn){
	
	pthread_mutex_lock(&dbAccess);
	
	int resValue = 0;
	char archName[32];
	char timeStamp[64];
	char dbRequest[2048];
	memset(archName, 0, 32);
	memset(timeStamp, 0, 64);
	memset(dbRequest, 0, 2048);
	
	switch (conn.requestedArch){
		case 0: sprintf(archName,  "hour"); break;
		case 1: sprintf(archName,   "day"); break;
		case 2: sprintf(archName, "month"); break;
	}
	
	sprintf (timeStamp, "%04d-%02d-%02d %02d:%02d:%02d",
		conn.terrosAskDts.d.y+2000,
		conn.terrosAskDts.d.m,
		conn.terrosAskDts.d.d,
		conn.terrosAskDts.t.h,
		conn.terrosAskDts.t.m,
		conn.terrosAskDts.t.s);
	
	sprintf (dbRequest, "select * from save_q_meterages_%s('%s',1,0,0,0,0,'%f','%f','%f','%f','0','%f','%f','%f','%f','%f','%s',%d)",
		archName,
		conn.ethSn,
		conn.terrosAnswer.archive.t1,
		conn.terrosAnswer.archive.t2, 
		conn.terrosAnswer.archive.V1,
		conn.terrosAnswer.archive.V2,
		conn.terrosAnswer.archive.M1,
		conn.terrosAnswer.archive.M2,
		conn.terrosAnswer.archive.M3,
		conn.terrosAnswer.archive.Q,
		conn.terrosAnswer.archive.timeOfQcalc,
		timeStamp,
		(conn.iterPipeArch+1));
	
	//log ("\r\nsaveArchiveAnswerInDb\r\nDB REQ: [%s]\r\n", dbRequest);	
			
	result_DB_transaction = PQexec(connection_DB, dbRequest);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK ){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if((nrows == 1) && (nfields == 1)){
			resValue = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
		}
	}
	
	PQclear(result_DB_transaction);
	
	//if  (resValue == 1)
	//	log ("[ISM %s, TERROS id %04u] saveDataInDb (real) [arch: '%d' pipe: '%d'] Ok", conn.ethSn, conn.terrosSn,  conn.requestedArch, conn.iterPipeArch);
	//else 
	//	log ("[ISM %s, TERROS id %04u] saveDataInDb (real) [arch: '%d' pipe: '%d'] ERROR", conn.ethSn, conn.terrosSn,  conn.requestedArch, conn.iterPipeArch);
	
	pthread_mutex_unlock(&dbAccess);
}

void saveArchiveAnswerInDbAsEmpty(connection_t& conn){

	pthread_mutex_lock(&dbAccess);
	
	int resValue = 0;
	char archName[32];
	char timeStamp[64];
	char dbRequest[2048];
	memset(archName, 0, 32);
	memset(timeStamp, 0, 64);
	memset(dbRequest, 0, 2048);
	
	switch (conn.requestedArch){
		case 0: sprintf(archName,  "hour"); break;
		case 1: sprintf(archName,   "day"); break;
		case 2: sprintf(archName, "month"); break;
	}
	
	sprintf (timeStamp, "%04d-%02d-%02d %02d:%02d:%02d",
		conn.terrosAskDts.d.y+2000,
		conn.terrosAskDts.d.m,
		conn.terrosAskDts.d.d,
		conn.terrosAskDts.t.h,
		conn.terrosAskDts.t.m,
		conn.terrosAskDts.t.s);
	
	sprintf (dbRequest, "select * from save_q_meterages_%s('%s',0,0,0,0,0,'0','0','0','0','0','0','0','0','0','0','%s',%d)",
		archName,
		conn.ethSn,
		timeStamp,
		(conn.iterPipeArch+1));
			
			
	//log ("DB REQ: [%s]", dbRequest);
			
	result_DB_transaction = PQexec(connection_DB, dbRequest);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK ){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if((nrows == 1) && (nfields == 1)){
			resValue = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
		}
	}
	
	PQclear(result_DB_transaction);
	
	//if  (resValue == 1)
	//	log ("[ISM %s, TERROS id %04u] saveDataInDb (empty) [arch: '%d' pipe: '%d'] Ok", conn.ethSn, conn.terrosSn,  conn.requestedArch, conn.iterPipeArch);
	//else 
	//	log ("[ISM %s, TERROS id %04u] saveDataInDb (empty) [arch: '%d' pipe: '%d'] ERROR", conn.ethSn, conn.terrosSn,  conn.requestedArch, conn.iterPipeArch);

	pthread_mutex_unlock(&dbAccess);
}

void saveCurrentAnswerInDb(connection_t& conn){

	pthread_mutex_lock(&dbAccess);
	
	int resValue = 0;
	char timeStamp[64];
	char dbRequest[2048];
	
	memset(timeStamp, 0, 64);
	memset(dbRequest, 0, 2048);
	
	sprintf (timeStamp, "%04d-%02d-%02d %02d:%02d:%02d",
		conn.terrosDts.d.y+2000,
		conn.terrosDts.d.m,
		conn.terrosDts.d.d,
		conn.terrosDts.t.h,
		conn.terrosDts.t.m,
		conn.terrosDts.t.s);
	
	sprintf (dbRequest, "select * from save_q_meterages_current('%s','%f','%f','%f','%f','%f','%f',1,2,2,1,2,'%s',%d,%d,%d,0,0,0,0,%d)",
		conn.ethSn,
		conn.terrosAnswer.current.G1,
		conn.terrosAnswer.current.G2,
		conn.terrosAnswer.current.G3,			
		conn.terrosAnswer.current.T1,
		conn.terrosAnswer.current.T2,
		conn.terrosAnswer.current.T3,
		timeStamp,
		conn.verMod,
		conn.verHi,
		conn.verLo,
		(conn.iterPipeCurrent+1));
	

	result_DB_transaction = PQexec(connection_DB, dbRequest);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK ){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if((nrows == 1) && (nfields == 1)){
			resValue = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
		}
	}
	
	PQclear(result_DB_transaction);
	
	//if  (resValue == 1)
	//	log ("[ISM %s, TERROS id %04u] saveDataInDb [arch: 'C', pipe: '%d'] Ok", conn.ethSn, conn.terrosSn, conn.iterPipeCurrent);
	//else 
	//	log ("[ISM %s, TERROS id %04u] saveDataInDb [arch: 'C', pipe: '%d'] ERROR", conn.ethSn, conn.terrosSn, conn.iterPipeCurrent);

	pthread_mutex_unlock(&dbAccess);
}

void setActiveStatusInDb(connection_t& conn){
	
	pthread_mutex_lock(&dbAccess);
	
	char SQL_request[512];
	int resValue;
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select * from server_status_terros_set('%s', '%d', %d, %d, %d);", conn.ethSn, conn.terrosSn, conn.terrosState, conn.sd, conn.iterPipeCurrent+conn.iterPipeArch);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK ){
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );
		if((nrows == 1) && (nfields == 1)){
			resValue = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
		}
	}
	
	PQclear(result_DB_transaction);		

	pthread_mutex_unlock(&dbAccess);	
}

//
//
//
void resetStatusInDb(){

	pthread_mutex_lock(&dbAccess);
	
	char SQL_request[512];
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "update \"SERVER_STATUS_TERROS\" set socket_id=-1;");
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	//PQclear(result_DB_transaction);	
	
	pthread_mutex_unlock(&dbAccess);
	
	log ("reset db items done");
}

void saveEmptyAndGoToReadTime(connection_t& conn){
	//record was not found
	saveArchiveAnswerInDbAsEmpty(conn);
	conn.terrosAnswer.archive.readyParts = 0;
	conn.iterMeteragesArch = 0;
	
	conn.writeNotRead = 1;
	conn.terrosState = NEED_TO_GET_TERROS_DTS;
}

//
// PROCESS READED BUF
//

int processRead(connection_t& conn) {

	//log ("processRead, state = %d", conn.terrosState);
	int parserResult = 0;
	
	//check needed lenght
	/*
	if (conn.nCharsInBuffer >= conn.waitSize){
	*/
		//update status in database
		if (conn.terrosState > NEED_TO_GET_TERROS_SN){
			setActiveStatusInDb(conn); 
		}
		
		//if wait len is allocated in input
		switch (conn.terrosState){
		
			case NEED_TO_GET_ETH_SN:
				//if whole serial recieved
				//log ("processRead, state = NEED_TO_GET_ETH_SN");
				if (checkEthSnInDb(conn)){
					memcpy(conn.ethSn, conn.inBuffer, ETH_SN_SIZE);
					conn.writeNotRead = 1;
					conn.terrosState = NEED_TO_GET_TERROS_SN;
					return 1;
					
				} //else {
					//to do 
					//journal error
			      // return 0;
				//}
				break;
			
			case NEED_TO_GET_TERROS_SN:
				//check driver result
				//log ("processRead, state = NEED_TO_GET_TERROS_SN");
				if(TERROS_responseVersion(conn) == 1) {
					conn.writeNotRead = 1;
					conn.terrosState = NEED_TO_GET_TERROS_DTS;
				}
				break;
				
			case NEED_TO_GET_TERROS_DTS:
				//check driver result
				//log ("processRead, state =  NEED_TO_GET_TERROS_DTS");
				if(TERROS_responseCurrentDts(conn) == 1){
					
					//reset previously saved amswer
					memset(&conn.terrosAnswer, 0, sizeof(terrosAnswerData));
					
					//reser stamps
					memset(&conn.terrosAskDts, 0, sizeof(dateTimeStamp));
					memset(&conn.terrosRecordDts, 0, sizeof(dateTimeStamp));
					
					//set to read current measures on specified pipe (if will be needed)
					conn.iterPipeCurrent = 0;
					conn.iterMeteragesCurrent = 7;
					
					#if 1
					//get last DB Dtss and check what we can erad at this time
					dateTimeStamp * nextDts; //4 pipes x 3 type of meterages [M1 M2 M3 M4 D1 D2 D3 D4 H1 H2 H3 H4]
					int compareResult = 0;
					int checkDtssResult = 0;
					int pipeIndex = 0;
					
					//allocate pointer for 12 dtss
					nextDts = NULL;
					nextDts = (dateTimeStamp*)malloc(sizeof(dateTimeStamp)*12); //array for 12 elements;
					
					//check last meterage dtss for all archives and pipes
					checkDtssResult = checkDtssInDb(conn, &nextDts);
					if (checkDtssResult){
						//loop on pipes
						for (pipeIndex=0; pipeIndex<4; pipeIndex++){
							//if we need to ask on selected pipe
							if (conn.terrosPipes & (1<<pipeIndex)){
								//loop on archives
								int archTypeIndex=0;
								for (; archTypeIndex<3; archTypeIndex++){
									int dtsIndex = (archTypeIndex*4) + pipeIndex;
									
									//shift DTS to compare date correct
									dateTimeStamp shiftedDts;
									memset(&shiftedDts, 0, sizeof(dateTimeStamp));
									memcpy(&shiftedDts, &nextDts[dtsIndex], sizeof(dateTimeStamp));
									if (archTypeIndex == 0){
										TERROS_ShiftDtsUpTo1Hour(shiftedDts);
									}
									
									/*
									log ("processRead, state = NEED_TO_GET_TERROS_DTS, next check dts [%02d-%02d-%02d %02d:%02d:%02d] shifted [%02d-%02d-%02d %02d:%02d:%02d] arch %d", 
										nextDts[dtsIndex].d.d, nextDts[dtsIndex].d.m, nextDts[dtsIndex].d.y, 
										nextDts[dtsIndex].t.h, nextDts[dtsIndex].t.m, nextDts[dtsIndex].t.s, 
										shiftedDts.d.d, shiftedDts.d.m, shiftedDts.d.y, 
										shiftedDts.t.h, shiftedDts.t.m, shiftedDts.t.s, archTypeIndex);
									*/
									
									//check we need to read new with archTypeIndex meterage on selected pip
									compareResult = TERROS_DtsComapre(conn.terrosDts, shiftedDts, archTypeIndex);
									if (compareResult > 0){
										//log ("processRead, state = NEED_TO_GET_TERROS_DTS, dts compare return 'TRUE' [arch: %d, pipe %d]\r\n", archTypeIndex, (pipeIndex+1));
									
										//reset previously saved amswer
										memset(&conn.terrosAnswer, 0, sizeof(terrosAnswerData));
										
										//set to read specified archive on specified pipe
										conn.requestedArch = archTypeIndex;
										conn.iterPipeArch = pipeIndex;
										memcpy(&conn.terrosAskDts, &nextDts[dtsIndex], sizeof(dateTimeStamp));
										
										//reset iterators
										conn.iterMeteragesArch = 0;
										conn.terrosAnswer.archive.readyParts = 0;
										
										//part 0
										memset(&conn.terrosAnswer.archive.recordDts, 0, sizeof(dateTimeStamp));
										conn.terrosAnswer.archive.integrationTime = 0;
										//part 1
										conn.terrosAnswer.archive.t1 = 0;
										conn.terrosAnswer.archive.t2 = 0;
										conn.terrosAnswer.archive.t3 = 0;
										conn.terrosAnswer.archive.P1 = 0;
										conn.terrosAnswer.archive.P2 = 0;
										conn.terrosAnswer.archive.P3 = 0;
										conn.terrosAnswer.archive.t4 = 0;
										memset(conn.terrosAnswer.archive.maskOfEvenst, 0, sizeof(unsigned char)*10);
										//part2
										conn.terrosAnswer.archive.M1 = 0;
										conn.terrosAnswer.archive.M2 = 0;
										conn.terrosAnswer.archive.M3 = 0;
										conn.terrosAnswer.archive.V1 = 0;
										conn.terrosAnswer.archive.V2 = 0;
										conn.terrosAnswer.archive.maskOfErrors = 0;
										conn.terrosAnswer.archive.Q = 0;
										conn.terrosAnswer.archive.deltaQ = 0;
										conn.terrosAnswer.archive.timeOfQcalc = 0;
										conn.terrosAnswer.archive.timeOfDiscard = 0;
										conn.terrosAnswer.archive.timeOfTdtMin = 0;
										conn.terrosAnswer.archive.timeOfTGGmax = 0;
										conn.terrosAnswer.archive.timeOfTGGmin = 0;
										conn.terrosAnswer.archive.leakeage = 0;
										conn.terrosAnswer.archive.podmes = 0;
										
										//free
										free (nextDts);
										nextDts = NULL;
										
										//goto send command
										conn.writeNotRead = 1;
										if (archTypeIndex == 0){
											conn.terrosState = NEED_TO_GET_TERROS_ARCH_POINTERS;
										} else {
											conn.terrosState = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX;
										}
										
										//exit to process specified request
										return 1; 
										
									} else {
										//log ("processRead, state = NEED_TO_GET_TERROS_DTS, dts compare return 'FALSE' [arch: %d, pipe %d]\r\n", archTypeIndex, (pipeIndex+1));
									}
								}
							}
						}
					}
					
					//free
					free(nextDts);
					nextDts = NULL;
					
					#endif
					
					//we exit from loop, so no more dates was find to ask srchives
					//so we can read only current meterages
					conn.writeNotRead = 1;
					conn.terrosState = NEED_TO_GET_TERROS_CURRENT;
				}
				break;
			
			case NEED_TO_GET_TERROS_ARCH_POINTERS:
				//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_POINTERS");
				if (TERRROS_responseArchivePointers(conn) == 1){
					//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_POINTERS, overflow [%d] current pointer [%04x] max pointer [%04x]", conn.archiveOverflow, conn.archiveCurrentIndex, conn.archiveMaxRecords);
					
					conn.searchCounter = 0;
					conn.terrosRecordIndex = conn.lastFoundIndex; //or 0;
					conn.writeNotRead = 1;
					conn.terrosState = NEED_TO_CHECK_TERROS_ARCH_RECORD_DATA;					
				}
				break;
			
			case NEED_TO_CHECK_TERROS_ARCH_RECORD_DATA:
				parserResult = TERRROS_responseArchiveRecordDataCheck(conn);
				if (parserResult == 1){
					//fix 60 seconds
					if (conn.terrosRecordCheckDts.t.s == 60){
						conn.terrosRecordCheckDts.t.s = 0;
						conn.terrosRecordCheckDts.t.m++;
					}
					
					//fix 60 minutes
					if (conn.terrosRecordCheckDts.t.m == 60){
						conn.terrosRecordCheckDts.t.m = 0;
						conn.terrosRecordCheckDts.t.h++;
					}
					
					//move 1 hour back to have clock time from 0 to 23, not from 1to 23
					conn.terrosRecordCheckDts.t.h = conn.terrosRecordCheckDts.t.h - 1;
					
					/*
					log ("processRead, state = NEED_TO_CHECK_TERROS_ARCH_RECORD_DATA, record [%04x] found [%02d-%02d-%02d %02d:%02d:%02d] asked [%02d-%02d-%02d %02d:%02d:%02d]", 
						conn.terrosRecordIndex, 
						conn.terrosRecordCheckDts.d.d, conn.terrosRecordCheckDts.d.m, conn.terrosRecordCheckDts.d.y, 
						conn.terrosRecordCheckDts.t.h, conn.terrosRecordCheckDts.t.m, conn.terrosRecordCheckDts.t.s, 
						conn.terrosAskDts.d.d, conn.terrosAskDts.d.m, conn.terrosAskDts.d.y, 
						conn.terrosAskDts.t.h, conn.terrosAskDts.t.m, conn.terrosAskDts.t.s);
					*/
					
					//check interested stamp of record
					int compareResult = TERROS_DtsComapre(conn.terrosRecordCheckDts, conn.terrosAskDts, conn.requestedArch);
					if (compareResult == 0){
						//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, record [%04x] valid [YES], go to reading", conn.terrosRecordIndex);
						
						//try to get data from found address and save last found index
						conn.lastFoundIndex = conn.terrosRecordIndex;
						conn.writeNotRead = 1;
						conn.terrosState = NEED_TO_GET_TERROS_ARCH_RECORD_DATA;
						return 1;
					}
				}
				
				if(parserResult != 0) {
					//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, record [%04x] valid [NO], skip reading", conn.terrosRecordIndex);
					
					//increment search counter
					conn.searchCounter++;
										
					//dependind overflow of arhive  select max index
					int maxIndex = 0;
					if (conn.archiveOverflow == 0){
						maxIndex = conn.archiveCurrentIndex;
					} else {
						maxIndex = conn.archiveMaxRecords;
					}
					
					//check search counter action to stop searching process
					if (conn.searchCounter > maxIndex){
						//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, stop searchin: save empty, full archive was scanned");
						
						//if we can't move index more - save empty
						conn.lastFoundIndex = 0;
						saveEmptyAndGoToReadTime(conn);
						return 1;
					}
					
					//if we still can move index move it to found next record
					if (conn.terrosRecordIndex < maxIndex){
						conn.terrosRecordIndex++;
					} else {
						conn.terrosRecordIndex = 0;
					}
					
				} else {
					//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, answer format error, need to repeat command");
					
				}
				
				//ask again
				conn.writeNotRead = 1;
				conn.terrosState = NEED_TO_CHECK_TERROS_ARCH_RECORD_DATA;
				
				break;
			
			case NEED_TO_GET_TERROS_ARCH_RECORD_INDEX:
				//check driver result
				//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX");
				parserResult = TERROS_responseArchiveRecordIndex(conn);
				if (parserResult == 1){
					//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, index found [%04x] [dts is %02d-%02d-%02d %02d:%02d:%02d]", 
					//	conn.terrosRecordIndex, 
					//	conn.terrosRecordDts.d.d, conn.terrosRecordDts.d.m, conn.terrosRecordDts.d.y, 
					//	conn.terrosRecordDts.t.h, conn.terrosRecordDts.t.m, conn.terrosRecordDts.t.s);

					//check DTS for found archive record
					int compareResult = TERROS_DtsComapre(conn.terrosRecordDts, conn.terrosAskDts, conn.requestedArch);
					if (compareResult == 0){
						//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, record found by dateTime stamp is valid [YES], go to reading]");
						
						//try to get data from found address
						conn.writeNotRead = 1;
						conn.terrosState = NEED_TO_GET_TERROS_ARCH_RECORD_DATA;
						
					} else {
						//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, record found by dateTime stamp valid [NO], skip reading]");
						saveEmptyAndGoToReadTime(conn);
					}
					
				} else if (parserResult == 0){
					//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, request error, goto read current time"); 
					
					//error in answer
					conn.writeNotRead = 1;
					conn.terrosState = NEED_TO_GET_TERROS_DTS;
					
				} else {
					//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX, record not found, goto read current"); 
					saveEmptyAndGoToReadTime(conn);
					
					//reset previously saved amswer
					memset(&conn.terrosAnswer, 0, sizeof(terrosAnswerData));
					
					//set to read current measures on specified pipe (if will be needed)
					conn.iterPipeCurrent = 0;
					conn.iterMeteragesCurrent = 7;
					conn.terrosAnswer.current.readyParts = 0;
					
					//but go to read current meterages
					conn.terrosState = NEED_TO_GET_TERROS_CURRENT;
				}
				break;
					
			case NEED_TO_GET_TERROS_ARCH_RECORD_DATA:
				//check driver result
				//log ("processRead, state = NEED_TO_GET_TERROS_ARCH_RECORD_DATA");
				if (TERRROS_responseArchiveRecordData(conn) == 1){
					
					//change meterage type to read next
					conn.iterMeteragesArch++;
					
					//check we read all meterages type by pipe
					if (conn.iterMeteragesArch < 3) {
						conn.writeNotRead = 1;
						conn.terrosState = NEED_TO_GET_TERROS_ARCH_RECORD_DATA;
												
					} else {
						
						//check all parts was aquested in answer
						if (conn.terrosAnswer.archive.readyParts == 3){
							//save answer in database
							saveArchiveAnswerInDb(conn);
							conn.terrosAnswer.archive.readyParts = 0;
							conn.iterMeteragesArch = 0;
						}
						
						//change state to ask another pipes or meterages
						conn.writeNotRead = 1;
						conn.terrosState = NEED_TO_GET_TERROS_DTS;
					}
							
				}
				break;
			
			case NEED_TO_GET_TERROS_CURRENT:
				//check driver result
				//log ("processRead, state = NEED_TO_GET_TERROS_CURRENT, pipe [%u]", conn.iterPipeCurrent);
				if (TERRROS_responseCurrentData(conn) == 1){
					
					//change meterage type to read next
					conn.iterMeteragesCurrent++;
					
					//check we read all meterages type by pipe
					if (conn.iterMeteragesCurrent >= 14) {
						
						//check all parts was aquested in answer
						if (conn.terrosAnswer.current.readyParts >= 7){
							//save answer in database
							saveCurrentAnswerInDb(conn);
							
							//reset answer struct fields counter
							conn.terrosAnswer.current.readyParts = 0;
						}
						
						//change pipes and states
						conn.iterMeteragesCurrent = 7;
						
						//go to read next pipe
						conn.iterPipeCurrent++;
						
						//check all pipes was readed
						if (conn.iterPipeCurrent == 4){
							conn.iterPipeCurrent = 0;
							
							//if all 4 pipes was requested - go to read DTS
							conn.writeNotRead = 1;
							conn.terrosState = NEED_TO_GET_TERROS_DTS;
							return 1;
						} 
					}
					
					//change state to ask another pipes or meterages
					conn.writeNotRead = 1;
					conn.terrosState = NEED_TO_GET_TERROS_CURRENT;
				}
				break;
		}
	
	/*	
	} else {
	
		if (checkErrorCode(conn)){
			log ("processRead, state = ERROR ANSWER");
			log ("processRead, state will be changed to current meterages");
			conn.writeNotRead = 1;
			conn.terrosState = NEED_TO_GET_TERROS_CURRENT;
		}
	}
	*/
	return 1;
}

//
// PROCESS WRITEN BUF
//

int processWrite(connection_t& conn) {

	//if write buffer is empty and no previous writes less
	if ((conn.nCharsOutBuffer == 0) && (conn.writeNotRead == 1)){
		
		//depending reader
		switch (conn.terrosState){
			case NEED_TO_GET_ETH_SN:
				//no write
				break;
			
			case NEED_TO_GET_TERROS_SN:
				//log ("processWrite, state = NEED_TO_GET_TERROS_SN");
				TERROS_requestVersion(conn);
				break;
				
			case NEED_TO_GET_TERROS_DTS:
				//log ("processWrite, state = NEED_TO_GET_TERROS_DTS");
				TERROS_requestCurrentDts(conn);
				break;
			
			case NEED_TO_GET_TERROS_ARCH_RECORD_INDEX:
				//log ("processWrite, state = NEED_TO_GET_TERROS_ARCH_RECORD_INDEX");
				TERROS_requestArchiveRecordIndex(conn);
				break;
			
			case NEED_TO_GET_TERROS_ARCH_POINTERS:
				//log ("processWrite, state = NEED_TO_GET_TERROS_ARCH_POINTERS, pipe [%u]", conn.iterPipeCurrent);
				TERRROS_requestArchivePointers(conn);
				break;
				
			case NEED_TO_CHECK_TERROS_ARCH_RECORD_DATA:
				//log ("processWrite, state = NEED_TO_CHECK_TERROS_ARCH_RECORD_DATA, pipe [%u]", conn.iterPipeCurrent);
				TERRROS_requestArchiveRecordDataCheck(conn);
				break;
			
			case NEED_TO_GET_TERROS_ARCH_RECORD_DATA:
				//log ("processWrite, state = NEED_TO_GET_TERROS_ARCH_RECORD_DATA, pipe [%u]", conn.iterPipeCurrent);
				TERRROS_requestArchiveRecordData(conn);
				break;
				
			case NEED_TO_GET_TERROS_CURRENT:
				//log ("processWrite, state = NEED_TO_GET_TERROS_CURRENT, pipe [%u]", conn.iterPipeCurrent);
				TERRROS_requestCurrentData(conn);
				break;
			
			default:
				log ("processWrite, state = unknown. quit");
				return 0;
				break;
		}
	}
	
	return 1;
}

//
// READ FOO
//

int ReadData(connection_t& conn) 
{
    int nBytes = recv(conn.sd, conn.inBuffer + conn.nCharsInBuffer, SIZE_OF_BUFFER - conn.nCharsInBuffer, 0);
    if (nBytes <= 0) {
		log ("Socket was closed by client / error read");
        return 0;
    }
    
    // We read some bytes.  Advance the buffer size counter.
    conn.nCharsInBuffer += nBytes;
	conn.rx += nBytes;
	
	//int i =0;
	//printf ("RX [%04d] ", conn.nCharsInBuffer);
	//for (;i<conn.nCharsInBuffer; i++){
	//	printf ("%02x ", conn.inBuffer[i]);
	//}
	//printf ("\n");
	
	//set last action time
	conn.lastAction = time(NULL);

    return 1;
}

//
// WRITE FOO
// 

int WriteData(connection_t& conn) {

	//if something to write
	if (conn.nCharsOutBuffer > 0){
	
		//int i =0;
		//printf ("TX [%04d] ", conn.nCharsOutBuffer);
		//for (;i<conn.nCharsOutBuffer; i++){
		//	printf ("%02x ", conn.outBuffer[i]);
		//}
		//printf ("\n");
	
		int nBytes = send(conn.sd, conn.outBuffer, conn.nCharsOutBuffer, 0);
		if (nBytes <= 0) {
			log ("Socket was closed by client / error write");
			return 0;
		}
		
		if (nBytes == conn.nCharsOutBuffer) {
			// Everything got sent, so take a shortcut on clearing buffer.
			memset(conn.outBuffer, 0, SIZE_OF_BUFFER);
			conn.nCharsOutBuffer = 0;
		}
		else {
			// We sent part of the buffer's data.  Remove that data from the buffer.
			conn.nCharsOutBuffer -= nBytes;
			memmove(conn.outBuffer, conn.outBuffer + nBytes, conn.nCharsOutBuffer);
		}
		
		//set last action time
		conn.lastAction = time(NULL);
		conn.tx += nBytes;
		
		//change state
		if (conn.nCharsOutBuffer == 0){
			conn.writeNotRead = 0;
		}
	}
    
    return 1;
}

//
// THREAD OF SUPERVISOR
//

void * supervisor(void * lpParam) 
{ 
	//debug
	log ("supervisor was started");
	
	//sleep to system reaction
	usleep(100);

	//goto loop
	while (1){
		
		pthread_mutex_lock(&csPool);
		unsigned int poolIndex = 0;
		int totalClients = 0;

		//system("cls");

		while (poolIndex < pool.size()){

			int count = 0;
			for (int socketIndex = 0; socketIndex < MAX_COUNT_BY_THREAD; socketIndex++){
				if (pool.at(poolIndex)->clients[socketIndex].sd != -1){
					count++;
				}
			}

			if (pool.at(poolIndex)->threadState == 0){
				log ("remove died thread-pool id %d", pool.at(poolIndex)->id);

				//close all still opened clients
				for (int socketIndex = 0; socketIndex < MAX_COUNT_BY_THREAD; socketIndex++){
					if (pool.at(poolIndex)->clients[socketIndex].sd != -1){
						setOnlineState(pool.at(poolIndex)->clients[socketIndex].ethSn, 0);
						ShutdownConnection(pool.at(poolIndex)->clients[socketIndex].sd);
						pool.at(poolIndex)->clients[socketIndex].sd = -1;
						setActiveStatusInDb(pool.at(poolIndex)->clients[socketIndex]);
						
					}
				}
			
				//free pool_t struct associated with pool
				pthread_mutex_destroy(&pool.at(poolIndex)->atom);
				free (&*pool.at(poolIndex));
				pool.erase(pool.begin()+poolIndex);
			}
			else {
				totalClients = totalClients + count;
				poolIndex++;
			}
		}
		
		//log ("TOTAL CLIENTS: [%d] POOL SIZE[%d]", totalClients, pool.size());

		pthread_mutex_unlock(&csPool);

		sleep(5);
	}

	log ("supervisor thread die");

    return 0;
}

//
// THREAD SOCKET-SELECT BY 64 FOO
//

void * socket64Thread(void * lpParam) 
{ 
	pool_t * myPool = (pool_t *)lpParam;

	//debug
	log ("ThreadPool %d was started", myPool->id);

	//setup active thread state
	myPool->threadState = 1;

	//sleep to system reaction
	usleep(100);

	//goto loop
	while (1) {
		fd_set ReadFDs, WriteFDs, ExceptFDs;
		int count = 0;

		pthread_mutex_lock(&myPool->atom);

		//setup fd sets
        FD_ZERO(&ReadFDs);
		FD_ZERO(&WriteFDs);
		FD_ZERO(&ExceptFDs);

		//setup fd for sockets
		int lastFds = 0;
		for (int socketIndex = 0; socketIndex < MAX_COUNT_BY_THREAD; socketIndex++){
			//check valid useable socket
			if (myPool->clients[socketIndex].sd != -1){
				//search highest fd desc num
				if (myPool->clients[socketIndex].sd > lastFds){
					lastFds = myPool->clients[socketIndex].sd;
				}
				
				//fix current time
				time_t nowTime = time(NULL);

				//view about last pack time
				double duration = difftime(nowTime, myPool->clients[socketIndex].lastAction);
				if (duration > MAX_SILINCE_TIME){
					log ("ThreadPool %d / close timeouted socket %d", myPool->id, myPool->clients[socketIndex].sd);
					setOnlineState(myPool->clients[socketIndex].ethSn, 0);
					ShutdownConnection(myPool->clients[socketIndex].sd);
					myPool->clients[socketIndex].sd = -1;
					myPool->clients[socketIndex].terrosState=0;
					setActiveStatusInDb(myPool->clients[socketIndex]);

				} else {

					FD_SET(myPool->clients[socketIndex].sd, &ReadFDs);
					FD_SET(myPool->clients[socketIndex].sd, &WriteFDs);
					FD_SET(myPool->clients[socketIndex].sd, &ExceptFDs);

					//calculate live sockets
					count++;
				}
			}
		}

		usleep(1000);
		
		//if some sockets still available
		if (count > 0){

			//get select on fd sets
			int selectResult = select(MAX_COUNT_BY_THREAD, &ReadFDs, &WriteFDs, &ExceptFDs, NULL);
			if (selectResult > 0) {

				//setup fd for sockets
				for (int socketIndex = 0; socketIndex < MAX_COUNT_BY_THREAD; socketIndex++){

					//check valid useable socket
					if (myPool->clients[socketIndex].sd != -1){

						int bOK = 1;

						// See if this socket's flag is set in any of the FD sets.
						if (FD_ISSET(myPool->clients[socketIndex].sd, &ExceptFDs)) {
							bOK = 0;
							FD_CLR(myPool->clients[socketIndex].sd, &ExceptFDs);
						}
						else {
							if (FD_ISSET(myPool->clients[socketIndex].sd, &ReadFDs)) {
								bOK = ReadData(myPool->clients[socketIndex]);
								FD_CLR(myPool->clients[socketIndex].sd, &ReadFDs);
								if (bOK){
									bOK = processRead(myPool->clients[socketIndex]);
								}
							}
							if (FD_ISSET(myPool->clients[socketIndex].sd, &WriteFDs)) {
								if (bOK){
									bOK = processWrite(myPool->clients[socketIndex]);
									if (bOK){	
										bOK = WriteData(myPool->clients[socketIndex]);	
									}
								}
								FD_CLR(myPool->clients[socketIndex].sd, &WriteFDs);
							}
						}

						if (bOK == 0) {
							log ("ThreadPool %d close errored socket %d", myPool->id, myPool->clients[socketIndex].sd);
							setOnlineState(myPool->clients[socketIndex].ethSn, 0);
							ShutdownConnection(myPool->clients[socketIndex].sd);
							myPool->clients[socketIndex].sd = -1;
							myPool->clients[socketIndex].terrosState=0;
							setActiveStatusInDb(myPool->clients[socketIndex]);
						}
					}
				}

			}
			else if (selectResult < 0) {
				log ("ThreadPool %d select error / thread exit", myPool->id);
				//close all associated sockets
				for (int socketIndex = 0; socketIndex < MAX_COUNT_BY_THREAD; socketIndex++){
					if (myPool->clients[socketIndex].sd != -1){
						setOnlineState(myPool->clients[socketIndex].ethSn, 0);
						ShutdownConnection(myPool->clients[socketIndex].sd);
						myPool->clients[socketIndex].sd = -1;
						myPool->clients[socketIndex].terrosState=0;
						setActiveStatusInDb(myPool->clients[socketIndex]);
					}
				}
				pthread_mutex_unlock(&myPool->atom);
				break;
			}
		}
		else {
			log ("ThreadPool %d nothing to select / thread exit", myPool->id);
			pthread_mutex_unlock(&myPool->atom);
			break;
		}

		pthread_mutex_unlock(&myPool->atom);

		usleep(1000);
    }

	log ("ThreadPool %d die", myPool->id);

	//setup died thread state
	myPool->threadState = 0;

    return 0; 
} 

//
//
//

int poolAllocate(){

	int retCode = -1;
	pool_t * poolTmp = NULL;

	//lock pool atomic
	pthread_mutex_lock(&csPool);

	//create new pool
	poolTmp = (pool_t *) malloc (sizeof (pool_t));
	if (poolTmp  != NULL){

		//reset pool data
		poolTmp->id = (++poolId);

		//init pool critical section
		pthread_mutex_init(&poolTmp->atom, NULL);
		
		//reset sockets
		for (int index = 0; index < MAX_COUNT_BY_THREAD; index++){
			poolTmp->clients[index].sd = -1;
		}

		//create new pool element
		pool.push_back(poolTmp);

		//ret value set
		retCode = 0;
	}

	//release pool atomic
	pthread_mutex_unlock(&csPool);

	return retCode;
}

//
//
//

int poolTryAppend(int poolIndex, int sd){

	int retCode = -1;

	//enter critical of pool
	pthread_mutex_lock(&pool.at(poolIndex)->atom);

	//if in some pool we have a free space
	for(int clientIndex=0; clientIndex < MAX_COUNT_BY_THREAD; clientIndex++){
		if(pool.at(poolIndex)->clients[clientIndex].sd == -1){

			//add client socket
			pool.at(poolIndex)->clients[clientIndex].sd = sd;

			//setup input buffer
			memset(pool.at(poolIndex)->clients[clientIndex].inBuffer, 0, SIZE_OF_BUFFER);
			pool.at(poolIndex)->clients[clientIndex].nCharsInBuffer = 0;
			
			//setup output buffer
			memset(pool.at(poolIndex)->clients[clientIndex].outBuffer, 0, SIZE_OF_BUFFER);
			pool.at(poolIndex)->clients[clientIndex].nCharsOutBuffer = 0;

			//setup last activity time
			pool.at(poolIndex)->clients[clientIndex].lastAction = time(NULL);
			pool.at(poolIndex)->clients[clientIndex].rx = 0;
			pool.at(poolIndex)->clients[clientIndex].tx = 0;
			
			//setup state machine initial points
			pool.at(poolIndex)->clients[clientIndex].terrosState = NEED_TO_GET_ETH_SN;
			
			//reset serials
			pool.at(poolIndex)->clients[clientIndex].ethDbId = -1;
			pool.at(poolIndex)->clients[clientIndex].terrosSn = -1;
			memset(pool.at(poolIndex)->clients[clientIndex].ethSn, 0, ETH_SN_SIZE+1);
			
			//reset terros private data
			memset(&pool.at(poolIndex)->clients[clientIndex].terrosDts, 0, sizeof (dateTimeStamp));
			memset(&pool.at(poolIndex)->clients[clientIndex].terrosAskDts, 0, sizeof (dateTimeStamp));
			memset(&pool.at(poolIndex)->clients[clientIndex].terrosRecordDts, 0, sizeof (dateTimeStamp));

			//reset terros variables
			pool.at(poolIndex)->clients[clientIndex].requestedArch = 0;
			pool.at(poolIndex)->clients[clientIndex].terrosRecordIndex = 0;
			pool.at(poolIndex)->clients[clientIndex].terrosPipes = 0;
			pool.at(poolIndex)->clients[clientIndex].iterPipeArch = 0;
			pool.at(poolIndex)->clients[clientIndex].iterMeteragesArch = 0;
			pool.at(poolIndex)->clients[clientIndex].iterPipeCurrent = 0;
			pool.at(poolIndex)->clients[clientIndex].iterMeteragesCurrent = 0;
			pool.at(poolIndex)->clients[clientIndex].requestedArch = 0;
			
			//set waitSize to len of ETH serial
			pool.at(poolIndex)->clients[clientIndex].waitSize = ETH_SN_SIZE;
			pool.at(poolIndex)->clients[clientIndex].writeNotRead = 0;
			
			pool.at(poolIndex)->clients[clientIndex].lastFoundIndex = 0;
			
			//mark the socket as non-blocking, for safety.
			int opts = fcntl(sd,F_GETFL);
            opts = (opts | O_NONBLOCK);
            fcntl(sd,F_SETFL,opts);

			//exit loop
			retCode = 0;
			break;
		}
	} 

	//leave critical
	pthread_mutex_unlock(&pool.at(poolIndex)->atom);

	return retCode;
}

//
// ACCEPT AND CREATE NEW THREADS
//

void AcceptConnections(int ListeningSocket)
{
    sockaddr_in sinRemote;
	socklen_t socklen;
    socklen = sizeof(struct sockaddr_in);
        
	//loop in acceptor 
    while (1) {

		//get new connection
        int sd = accept(ListeningSocket, (struct sockaddr *) &sinRemote, &socklen);
        if (sd != -1) {
			
			bool appended = false;
			bool threadon = false;

			//first enter
			if (pool.size() == 0){
				//allocate memory
				if (poolAllocate() < 0){
					log ("can't create new pool / memory error / exit");
					return;
				}

				//setup flag to create new thread
				threadon = true;
			}

			//found free pool to append new connection
			for (unsigned int poolIndex = 0; poolIndex < pool.size(); poolIndex++){
				if (poolTryAppend(poolIndex, sd) == 0){
					appended = true;
					break;
				}
			}

			//check we can't append - all pool are full, so create new
			if (!appended){
				
				//allocate memory
				if (poolAllocate() < 0){
					log ("can't create new pool / memory error / exit");
					return;
				}

				//try to append to something free
				for (unsigned int poolIndex = 0; poolIndex < pool.size(); poolIndex++){
					if (poolTryAppend(pool.size()-1, sd) == 0){
						appended = true;
						threadon = true;
						break;
					}
				}

				//check exception
				if (!appended){
					log ("new pool was created, but appending was failed / exit");
					return;
				}
			}

			//check we need to create new thread
			if (threadon){
                int tid, tdr;
                pthread_t th;
                tid = pthread_create(&th, NULL, socket64Thread, &*pool.at(pool.size()-1));
				tdr = pthread_detach(th);
			}

            
        }
        else {
			log ("accept new connection was failed");
            return;
        }
	}
}

//
// SETUP SOCK
//

int SetUpListener()
{
    u_long nInterfaceAddr = inet_addr(SERVER_HOST);
    if (nInterfaceAddr != INADDR_NONE) {
        int sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd > 0) {
			log("Main listener(fd=%d) created!", sd);
            int on=1;
            setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            log("Listener: option [reuse Addres] Ok");
			
            sockaddr_in sinInterface;
            sinInterface.sin_family = AF_INET;
            sinInterface.sin_addr.s_addr = nInterfaceAddr;
            sinInterface.sin_port = htons(SERVER_PORT);
            if (bind(sd, (sockaddr*)&sinInterface, sizeof(sockaddr_in)) == 0) {
                log("Listener: binded [%s] Ok", SERVER_HOST);
				fdListener = sd;
                listen(sd, 0); //SOMAXCONN);
                log("Start to listen port: %d!", SERVER_PORT);
                return sd;
            }
            else {
				log ("bind failed");
            }
        }
    }

    return -1;
}

//
// DO SOCK
//

void DoSock()
{
	//setup listener
    int ListeningSocket = SetUpListener();
    if (ListeningSocket == -1) {
		log ("error creating listener");
        return;
    }

	//setup supervisor thread
    pthread_t th;
    log ("call create supervisor thread");
    pthread_create(&th, NULL, supervisor, 0);
    
    //process asseptor loop
    while (1) {
        log ("wait acceptor");
        AcceptConnections(ListeningSocket);
    }
}

//
// MAIN
//

int main(int argc, char* argv[])
{
	//connect to database
	connectDb();
	
	//reset status
	resetStatusInDb();
	
	//do sock function
	DoSock();

    return 0;
}

