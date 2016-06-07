
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "local.h"

//unsigned char convertDecToHex (int dec){
//    return ((dec / 10) * 0x10) + (dec % 10);
//}

extern void log(const char * str, ...);

void resetBuffers(connection_t& conn){
	conn.nCharsInBuffer = 0;
	conn.nCharsOutBuffer = 0;
	conn.waitSize = 0;
	memset(conn.inBuffer, 0, SIZE_OF_BUFFER);
	memset(conn.outBuffer, 0, SIZE_OF_BUFFER);
}

void addSn(connection_t& conn){
    char snAsText[32];
    memset(snAsText, 0, 32);
    sprintf(snAsText, "0x%d", conn.terrosSn);
    
    int decToHexSn = strtoul(snAsText, NULL, 16);

    conn.outBuffer[3] = ((decToHexSn >> 24) & 0xFF);
    conn.outBuffer[2] = ((decToHexSn >> 16) & 0xFF);
    conn.outBuffer[1] = ((decToHexSn >> 8)  & 0xFF);
    conn.outBuffer[0] = (decToHexSn & 0xFF);
}

void getCrc(unsigned char * buf, int size, unsigned char * crc1, unsigned char * crc2){
    (*crc1) = 0x0; //xor
    (*crc2) = 0x0; //summ256

    for (int i=0; i<size; i++){
        (*crc1) = (*crc1) ^ buf[i];
        (*crc2) = ((*crc2) + buf[i]) & 0xFF;
    }
}

void addCrc(connection_t& conn){
	unsigned char crc1;
	unsigned char crc2;
	getCrc(conn.outBuffer, conn.nCharsOutBuffer-2, &crc1, &crc2);
	conn.outBuffer[conn.nCharsOutBuffer-2] = crc1;
	conn.outBuffer[conn.nCharsOutBuffer-1] = crc2;
}

int checkErrorCode (connection_t& conn){
	//check error Codes
	switch (conn.inBuffer[4]){
		case 0xF0:
		case 0xF1:
		case 0xEF:
		case 0xFE:
			log("TERROS PROTO: RECV ERROR BYTE %02x", conn.inBuffer[4]);
			return 1;
			break;
	} 
	
	return 0;
}

int checkCrc(connection_t& conn){
	unsigned char crc1;
	unsigned char crc2;
	
	if (conn.nCharsInBuffer < conn.waitSize)
		if (checkErrorCode(conn))
			return -1;
	
	//check crc
	getCrc(conn.inBuffer, conn.nCharsInBuffer-2, &crc1, &crc2);
	if ((conn.inBuffer[conn.nCharsInBuffer-2] == crc1) && (conn.inBuffer[conn.nCharsInBuffer-1] == crc2)){
		return 1;
	}
	
	return 0;
}

void setFrom(void * setToVariable, unsigned char * bufferFrom, int bufferOffset, int variablesSize){
	if (variablesSize > 0){
		memcpy(setToVariable, &bufferFrom[bufferOffset], variablesSize);
		//if (variablesSize == 4){
		//	printf ("SET VALUE %f\r\n", *(float *)setToVariable);
		//}
	}
}

//
// shift DTS up to 1 hour
//
void TERROS_ShiftDtsUpTo1Hour(dateTimeStamp& dts){
	dts.t.h++;
	if (dts.t.h == 24){
		dts.t.h = 0;
		dts.d.d++;
		
		int maxDays = 31;
		switch (dts.d.m){
			case 2:
				if ((dts.d.y%4) == 0){
					maxDays = 29;
				} else {
					maxDays = 28;
				}
				break;
				
			case 1:
			case 3:
			case 5:
			case 7:
			case 8:
			case 10:
			case 12:
				maxDays = 31;
				break;
			
			case 4:
			case 6:
			case 9:
			case 11:
			default:
				maxDays=30;
				break;
		}
		
		if (dts.d.d > maxDays){
			dts.d.d = 1;
			dts.d.m++;
			if (dts.d.m>12){
				dts.d.m = 1;
				dts.d.y++;
			}
		}
	}
}


//
// return 0 if A=B, return 1 if A > B, return -1 if A < B
//
int TERROS_DtsComapre(dateTimeStamp dtsA, dateTimeStamp dtsB, int archTypeIndex){
	int dateA = ((dtsA.d.y * 10000) + (dtsA.d.m * 100) + (dtsA.d.d));
    int dateB = ((dtsB.d.y * 10000) + (dtsB.d.m * 100) + (dtsB.d.d));
	
    if(dateA == dateB){
	
		//check time part only for hour archive
		if (archTypeIndex == 0){
			//check time
			int timeA = ((dtsA.t.h * 10000) + (dtsA.t.m * 100) + (dtsA.t.s));
			int timeB = ((dtsB.t.h * 10000) + (dtsB.t.m * 100) + (dtsB.t.s));
			if (timeA == timeB) {
				return 0; //time eqals
				
			} else if (timeA > timeB){
				return 1; 
			} 
			
			return -1;
			
		} else {
			return 0; //dates equals
		}
		
    } else if (dateA > dateB) {
		return 1; //date A great than date B
	} 
	
	return -1; //date A lower than date B
}

void TERROS_requestVersion(connection_t& conn){
	resetBuffers(conn);
	addSn(conn);
	conn.outBuffer[4] = 0x0;
	conn.nCharsOutBuffer = 16;
	conn.waitSize = 16;
	addCrc(conn);
}

int TERROS_responseVersion(connection_t& conn){
	int result = checkCrc(conn);
	if (result == 1){
	
		unsigned char version[7];
		char tmp[3];
		memset(version, 0, 7);
		memcpy(version, &conn.inBuffer[5], 5);
		switch (conn.inBuffer[10]){
			case 0x0:  version[6] = 'M'; conn.verMod = 0x0;   break;
			case 0x11: version[6] = 'B'; conn.verMod = 0x11;  break;
			default:   version[6] = '-'; conn.verMod = 0xFF;  break;
		}
		
		memset(tmp, 0, 3);
		memcpy (tmp, &conn.inBuffer[5], 2);
		conn.verHi = atoi (tmp);
		
		memset(tmp, 0, 3);
		memcpy (tmp, &conn.inBuffer[8], 2);
		conn.verLo = atoi (tmp);
		
		//to do
		//use version
	}
	
	return result;
}

void TERROS_requestCurrentDts(connection_t& conn){
	resetBuffers(conn);
	addSn(conn);
	conn.outBuffer[4] = 13;
	conn.outBuffer[5] =  3;
	conn.nCharsOutBuffer = 16;
	conn.waitSize = 16;
	addCrc(conn);
}

int TERROS_responseCurrentDts(connection_t& conn){
	int result = checkCrc(conn);
	if (result == 1){
	
		conn.terrosDts.d.d = conn.inBuffer[6];
		conn.terrosDts.d.m = conn.inBuffer[7];
		conn.terrosDts.d.y = conn.inBuffer[8];
		conn.terrosDts.t.h = conn.inBuffer[9];
		conn.terrosDts.t.m = conn.inBuffer[10];
		conn.terrosDts.t.s = conn.inBuffer[11];		
	}
	
	return result;
}

void TERRROS_requestArchivePointers(connection_t& conn){
	resetBuffers(conn);
	addSn(conn);
	conn.outBuffer[4] = 14;
	conn.outBuffer[5] = conn.requestedArch;
	conn.nCharsOutBuffer = 16;
	conn.waitSize = 16;
	addCrc(conn);
}

int TERRROS_responseArchivePointers(connection_t& conn){
	int result = checkCrc(conn);
	if (result == 1){
		
		if ((conn.inBuffer[5] & 0x80) == 0x80)
			conn.archiveOverflow = 1;
		else 
			conn.archiveOverflow = 0;
			
		conn.archiveCurrentIndex = ((conn.inBuffer[7]<<8) + conn.inBuffer[6]);
		conn.archiveMaxRecords = ((conn.inBuffer[9]<<8) + conn.inBuffer[8]);
	}
	
	return result;
}

void TERROS_requestArchiveRecordIndex(connection_t& conn){
	resetBuffers(conn);
	addSn(conn);
	conn.outBuffer[4]  = 18;
	conn.outBuffer[5]  = conn.requestedArch;
	conn.outBuffer[6]  = conn.terrosAskDts.d.d;
	conn.outBuffer[7]  = conn.terrosAskDts.d.m;
	conn.outBuffer[8]  = conn.terrosAskDts.d.y;
	conn.outBuffer[9]  = conn.terrosAskDts.t.h; //?
	conn.outBuffer[10] = conn.terrosAskDts.t.m; //?
	conn.outBuffer[11] = conn.terrosAskDts.t.s; //?
	conn.nCharsOutBuffer = 16;
	conn.waitSize = 16;
	addCrc(conn);
}

int TERROS_responseArchiveRecordIndex(connection_t& conn){
	int searchResult = -1;
	int result = checkCrc(conn);
	if (result == 1){
	
		searchResult = conn.inBuffer[5];
		if (searchResult == 0){
			conn.terrosRecordIndex = (conn.inBuffer[7]<<8) + conn.inBuffer[6];
			conn.terrosRecordDts.d.d = conn.inBuffer[8];
			conn.terrosRecordDts.d.m = conn.inBuffer[9];
			conn.terrosRecordDts.d.y = conn.inBuffer[10];
			conn.terrosRecordDts.t.h = conn.inBuffer[11];
			conn.terrosRecordDts.t.m = conn.inBuffer[12];
			conn.terrosRecordDts.t.s = conn.inBuffer[13];
		} else {
			result =-2;
		}
	}
	
	return result;
}

void TERRROS_requestArchiveRecordDataCheck(connection_t& conn){
	resetBuffers(conn);
	addSn(conn);
	conn.outBuffer[4] = 15;
	conn.outBuffer[5] = (conn.requestedArch & 0xFF);
	
	conn.outBuffer[6] = (conn.terrosRecordIndex & 0xFF);
	conn.outBuffer[7] = (conn.terrosRecordIndex >> 8) & 0xFF;
	
	conn.outBuffer[8] = (conn.iterMeteragesArch << 2) + conn.iterPipeArch;
	conn.nCharsOutBuffer = 16;
	conn.waitSize = 72;
	addCrc(conn);
}

int TERRROS_responseArchiveRecordDataCheck(connection_t& conn){
	int result = checkCrc(conn);
	if (result == 1){
	
		//depending type of asked meterages
		switch (conn.iterMeteragesArch){
			case 0:
				setFrom(&conn.terrosRecordCheckDts.d.d,	conn.inBuffer, 5,   1);
				setFrom(&conn.terrosRecordCheckDts.d.m, conn.inBuffer, 6,   1);
				setFrom(&conn.terrosRecordCheckDts.d.y,	conn.inBuffer, 7,   1);
				setFrom(&conn.terrosRecordCheckDts.t.h,	conn.inBuffer, 8,   1);
				setFrom(&conn.terrosRecordCheckDts.t.m, conn.inBuffer, 9,   1);
				setFrom(&conn.terrosRecordCheckDts.t.s, conn.inBuffer, 10,  1);
				break;
		}
	}
	
	return result;
}

void TERRROS_requestArchiveRecordData(connection_t& conn){
	resetBuffers(conn);
	addSn(conn);
	conn.outBuffer[4] = 15;
	conn.outBuffer[5] = (conn.requestedArch & 0xFF);
	conn.outBuffer[6] = (conn.terrosRecordIndex & 0xFF);
	conn.outBuffer[7] = (conn.terrosRecordIndex >> 8) & 0xFF;
	conn.outBuffer[8] = (conn.iterMeteragesArch << 2) + conn.iterPipeArch;
	conn.nCharsOutBuffer = 16;
	conn.waitSize = 72;
	addCrc(conn);
}

int TERRROS_responseArchiveRecordData(connection_t& conn){
	int result = checkCrc(conn);
	if (result == 1){
	
		//depending type of asked meterages
		switch (conn.iterMeteragesArch){
			case 0:
				setFrom(&conn.terrosAnswer.archive.recordDts.d.d,	conn.inBuffer, 5,   1);
				setFrom(&conn.terrosAnswer.archive.recordDts.d.m, 	conn.inBuffer, 6,   1);
				setFrom(&conn.terrosAnswer.archive.recordDts.d.y,	conn.inBuffer, 7,   1);
				setFrom(&conn.terrosAnswer.archive.recordDts.t.h,	conn.inBuffer, 8,   1);
				setFrom(&conn.terrosAnswer.archive.recordDts.t.m, 	conn.inBuffer, 9,   1);
				setFrom(&conn.terrosAnswer.archive.recordDts.t.s, 	conn.inBuffer, 10,  1);
				setFrom(&conn.terrosAnswer.archive.integrationTime, conn.inBuffer, 11,  4);
				
				conn.terrosAnswer.archive.readyParts++;
				break;
				
			case 1:
				setFrom(&conn.terrosAnswer.archive.t1,				conn.inBuffer, 5,   4);
				setFrom(&conn.terrosAnswer.archive.t2,				conn.inBuffer, 9,   4);
				setFrom(&conn.terrosAnswer.archive.t3,				conn.inBuffer, 13,  4);
				setFrom(&conn.terrosAnswer.archive.P1,				conn.inBuffer, 17,  4);
				setFrom(&conn.terrosAnswer.archive.P2,				conn.inBuffer, 21,  4);
				setFrom(&conn.terrosAnswer.archive.P3,				conn.inBuffer, 25,  4);
				setFrom(&conn.terrosAnswer.archive.t4,				conn.inBuffer, 29,  4);
				setFrom(&conn.terrosAnswer.archive.maskOfEvenst, 	conn.inBuffer, 33, 10);
				
				conn.terrosAnswer.archive.readyParts++;
				break;
				
			case 2:
				setFrom(&conn.terrosAnswer.archive.M1,				conn.inBuffer, 5,   4);
				setFrom(&conn.terrosAnswer.archive.M2,				conn.inBuffer, 9,   4);
				setFrom(&conn.terrosAnswer.archive.M3,				conn.inBuffer, 13,  4);
				setFrom(&conn.terrosAnswer.archive.V1,				conn.inBuffer, 17,  4);
				setFrom(&conn.terrosAnswer.archive.V2,				conn.inBuffer, 21,  4);
				setFrom(&conn.terrosAnswer.archive.maskOfErrors, 	conn.inBuffer, 25,  1);
				setFrom(&conn.terrosAnswer.archive.Q,				conn.inBuffer, 26,  4);
				setFrom(&conn.terrosAnswer.archive.deltaQ,			conn.inBuffer, 30,  4);
				setFrom(&conn.terrosAnswer.archive.timeOfQcalc,		conn.inBuffer, 34,  4);
				setFrom(&conn.terrosAnswer.archive.timeOfDiscard,	conn.inBuffer, 48,  4);
				setFrom(&conn.terrosAnswer.archive.timeOfTdtMin,	conn.inBuffer, 42,  4);
				setFrom(&conn.terrosAnswer.archive.timeOfTGGmax,	conn.inBuffer, 46,  4);
				setFrom(&conn.terrosAnswer.archive.timeOfTGGmin,	conn.inBuffer, 50,  4);
				setFrom(&conn.terrosAnswer.archive.leakeage,		conn.inBuffer, 54,  4);
				setFrom(&conn.terrosAnswer.archive.podmes,			conn.inBuffer, 58,  4);
				
				conn.terrosAnswer.archive.readyParts++;
				break;
		}		
	}
	
	return result;
}

void TERRROS_requestCurrentData(connection_t& conn){
	resetBuffers(conn);
	addSn(conn);
	conn.outBuffer[4] = 16;
	conn.outBuffer[5] = 0;
	conn.outBuffer[6] = conn.iterPipeCurrent;
	conn.outBuffer[7] = conn.iterMeteragesCurrent;
	conn.nCharsOutBuffer = 16;
	conn.waitSize = 16;
	addCrc(conn);
}

int TERRROS_responseCurrentData(connection_t& conn){
	int result = checkCrc(conn);
	if (result == 1){
		
		//depending type of asked meterages
		switch (conn.iterMeteragesCurrent){
			case 7:
				setFrom(&conn.terrosAnswer.current.G1, conn.inBuffer, 8, 4);
				//printf ("VALUE G1: %f\r\n", conn.terrosAnswer.current.G1);
				conn.terrosAnswer.current.readyParts++;
				break;
				
			case 8:
				setFrom(&conn.terrosAnswer.current.G2, conn.inBuffer, 8, 4);
				//printf ("VALUE G2: %f\r\n", conn.terrosAnswer.current.G2);
				conn.terrosAnswer.current.readyParts++;
				break;
				
			case 9:
				setFrom(&conn.terrosAnswer.current.G3, conn.inBuffer, 8, 4);
				//printf ("VALUE G3: %f\r\n", conn.terrosAnswer.current.G3);
				conn.terrosAnswer.current.readyParts++;
				break;
			
			case 10:
				setFrom(&conn.terrosAnswer.current.W,  conn.inBuffer, 8, 4);
				//printf ("VALUE  W: %f\r\n", conn.terrosAnswer.current.W);
				conn.terrosAnswer.current.readyParts++;
				break;
			
			case 11:
				setFrom(&conn.terrosAnswer.current.T1, conn.inBuffer, 8, 4);
				//printf ("VALUE T1: %f\r\n", conn.terrosAnswer.current.T1);
				conn.terrosAnswer.current.readyParts++;
				break;
			
			case 12:
				setFrom(&conn.terrosAnswer.current.T2, conn.inBuffer, 8, 4);
				//printf ("VALUE T2: %f\r\n", conn.terrosAnswer.current.T2);
				conn.terrosAnswer.current.readyParts++;
				break;
			
			case 13:
				setFrom(&conn.terrosAnswer.current.T3, conn.inBuffer, 8, 4);
				//printf ("VALUE T3: %f\r\n", conn.terrosAnswer.current.T3);
				conn.terrosAnswer.current.readyParts++;
				break;
		}
	}
	
	return result;
}

//EOF
