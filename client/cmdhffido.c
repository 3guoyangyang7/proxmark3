//-----------------------------------------------------------------------------
// Copyright (C) 2018 Merlok
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// High frequency MIFARE  Plus commands
//-----------------------------------------------------------------------------
//
//  Documentation here:
//
// FIDO Alliance specifications
// https://fidoalliance.org/download/
// FIDO NFC Protocol Specification v1.0
// https://fidoalliance.org/specs/fido-u2f-v1.2-ps-20170411/fido-u2f-nfc-protocol-v1.2-ps-20170411.html
// FIDO U2F Raw Message Formats
// https://fidoalliance.org/specs/fido-u2f-v1.2-ps-20170411/fido-u2f-raw-message-formats-v1.2-ps-20170411.html
//-----------------------------------------------------------------------------


#include "cmdhffido.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <jansson.h>
#include "comms.h"
#include "cmdmain.h"
#include "util.h"
#include "ui.h"
#include "proxmark3.h"
#include "cmdhf14a.h"
#include "mifare.h"
#include "emv/emvcore.h"
#include "emv/emvjson.h"
#include "emv/dump.h"
#include "cliparser/cliparser.h"

static int CmdHelp(const char *Cmd);

int FIDOSelect(bool ActivateField, bool LeaveFieldON, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
	uint8_t data[] = {0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};
	
	return EMVSelect(ActivateField, LeaveFieldON, data, sizeof(data), Result, MaxResultLen, ResultLen, sw, NULL);
}

int FIDOExchange(sAPDU apdu, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
	int res = EMVExchange(true, apdu, Result, MaxResultLen, ResultLen, sw, NULL);
	if (res == 5) // apdu result (sw) not a 0x9000
		res = 0;
	// software chaining
	while (!res && (*sw >> 8) == 0x61) {
		size_t oldlen = *ResultLen;
		res = EMVExchange(true, (sAPDU){0x00, 0xC0, 0x00, 0x00, 0x00, NULL}, &Result[oldlen], MaxResultLen - oldlen, ResultLen, sw, NULL);
		if (res == 5) // apdu result (sw) not a 0x9000
			res = 0;
		
		*ResultLen += oldlen;
		if (*ResultLen > MaxResultLen) 
			return 100;
	}
	return res;
}

int FIDORegister(uint8_t *params, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
	return FIDOExchange((sAPDU){0x00, 0x01, 0x03, 0x00, 64, params}, Result, MaxResultLen, ResultLen, sw);
}

int FIDOAuthentication(uint8_t *params, uint8_t paramslen, uint8_t controlb, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
	return FIDOExchange((sAPDU){0x00, 0x02, controlb, 0x00, paramslen, params}, Result, MaxResultLen, ResultLen, sw);
}

int FIDO2GetInfo(uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
	uint8_t data[] = {0x04};
	return FIDOExchange((sAPDU){0x80, 0x10, 0x00, 0x00, sizeof(data), data}, Result, MaxResultLen, ResultLen, sw);
}

int CmdHFFidoInfo(const char *cmd) {
	
	if (cmd && strlen(cmd) > 0)
		PrintAndLog("WARNING: command don't have any parameters.\n");
	
	// info about 14a part
	CmdHF14AInfo("");

	// FIDO info
	PrintAndLog("--------------------------------------------"); 
	SetAPDULogging(false);
	
	uint8_t buf[APDU_RES_LEN] = {0};
	size_t len = 0;
	uint16_t sw = 0;
	int res = FIDOSelect(true, true, buf, sizeof(buf), &len, &sw);

	if (res) {
		DropField();
		return res;
	}
	
	if (sw != 0x9000) {
		if (sw)
			PrintAndLog("Not a FIDO card! APDU response: %04x - %s", sw, GetAPDUCodeDescription(sw >> 8, sw & 0xff)); 
		else
			PrintAndLog("APDU exchange error. Card returns 0x0000."); 
		
		DropField();
		return 0;
	}
	
	if (!strncmp((char *)buf, "U2F_V2", 7)) {
		if (!strncmp((char *)buf, "FIDO_2_0", 8)) {
			PrintAndLog("FIDO2 authenricator detected. Version: %.*s", len, buf); 
		} else {
			PrintAndLog("FIDO authenricator detected (not standard U2F)."); 
			PrintAndLog("Non U2F authenticator version:"); 
			dump_buffer((const unsigned char *)buf, len, NULL, 0);
		}
	} else {
		PrintAndLog("FIDO U2F authenricator detected. Version: %.*s", len, buf); 
	}

	res = FIDO2GetInfo(buf, sizeof(buf), &len, &sw);
	DropField();
	if (res) {
		return res;
	}
	if (sw != 0x9000) {
		PrintAndLog("FIDO2 version not exists (%04x - %s).", sw, GetAPDUCodeDescription(sw >> 8, sw & 0xff)); 
		
		return 0;
	}

	PrintAndLog("FIDO2 version: (%d)", len); 
	dump_buffer((const unsigned char *)buf, len, NULL, 0);
	
	return 0;
}

json_t *OpenJson(int paramnum, char *fname, void* argtable[], bool *err) {	
	json_t *root = NULL;
	json_error_t error;
	*err = false;

	uint8_t jsonname[250] ={0};
	char *cjsonname = (char *)jsonname;
	int jsonnamelen = 0;
	
	// CLIGetStrWithReturn(paramnum, jsonname, &jsonnamelen);
	if (CLIParamStrToBuf(arg_get_str(paramnum), jsonname, sizeof(jsonname), &jsonnamelen))  {
		CLIParserFree();
		return NULL;
	}
	
	// current path + file name
	if (!strstr(cjsonname, ".json"))
		strcat(cjsonname, ".json");
	
	if (jsonnamelen) {
		strcpy(fname, get_my_executable_directory());
		strcat(fname, cjsonname);
		if (access(fname, F_OK) != -1) {
			root = json_load_file(fname, 0, &error);
			if (!root) {
				PrintAndLog("ERROR: json error on line %d: %s", error.line, error.text);
				*err = true;
				return NULL; 
			}
			
			if (!json_is_object(root)) {
				PrintAndLog("ERROR: Invalid json format. root must be an object.");
				json_decref(root);
				*err = true;
				return NULL; 
			}
			
		} else {
			root = json_object();
		}
	}
	return root;
}

int CmdHFFidoRegister(const char *cmd) {
	uint8_t data[64] = {0};
	int chlen = 0;
	uint8_t cdata[250] = {0};
	int applen = 0;
	uint8_t adata[250] = {0};
	json_t *root = NULL;
	
	CLIParserInit("hf fido reg", 
		"Initiate a U2F token registration. Needs two 32-byte hash number. \nchallenge parameter (32b) and application parameter (32b).", 
		"Usage:\n\thf fido reg -> execute command with 2 parameters, filled 0x00\n"
			"\thf fido reg 000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f 000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f -> execute command with parameters"
			"\thf fido reg -p s0 s1 -> execute command with plain parameters");

	void* argtable[] = {
		arg_param_begin,
		arg_lit0("aA",  "apdu",     "show APDU reqests and responses"),
		arg_lit0("vV",  "verbose",  "show technical data"),
		arg_lit0("pP",  "plain",    "send plain ASCII to challenge and application parameters instead of HEX"),
		arg_str0("jJ",  "json",		"fido.json", "JSON input / output file name for parameters."),
		arg_str0(NULL,  NULL,       "<HEX/ASCII challenge parameter (32b HEX/1..16 chars)>", NULL),
		arg_str0(NULL,  NULL,       "<HEX/ASCII application parameter (32b HEX/1..16 chars)>", NULL),
		arg_param_end
	};
	CLIExecWithReturn(cmd, argtable, true);
	
	bool APDULogging = arg_get_lit(1);
	bool verbose = arg_get_lit(2);
	bool paramsPlain = arg_get_lit(3);

	char fname[250] = {0};
	bool err;
	root = OpenJson(4, fname, argtable, &err);
	if(err)
		return 1;
	if (root) {	
		size_t jlen;
		JsonLoadBufAsHex(root, "$.ChallengeParam", data, 32, &jlen);
		JsonLoadBufAsHex(root, "$.ApplicationParam", &data[32], 32, &jlen);
	}
	
	if (paramsPlain) {
		memset(cdata, 0x00, 32);
		CLIGetStrWithReturn(5, cdata, &chlen);
		if (chlen && chlen > 16) {
			PrintAndLog("ERROR: challenge parameter length in ASCII mode must be less than 16 chars instead of: %d", chlen);
			return 1;
		}
	} else {
		CLIGetHexWithReturn(5, cdata, &chlen);
		if (chlen && chlen != 32) {
			PrintAndLog("ERROR: challenge parameter length must be 32 bytes only.");
			return 1;
		}
	}
	if (chlen)
		memmove(data, cdata, 32);
	
	
	if (paramsPlain) {
		memset(adata, 0x00, 32);
		CLIGetStrWithReturn(6, adata, &applen);
		if (applen && applen > 16) {
			PrintAndLog("ERROR: application parameter length in ASCII mode must be less than 16 chars instead of: %d", applen);
			return 1;
		}
	} else {
		CLIGetHexWithReturn(6, adata, &applen);
		if (applen && applen != 32) {
			PrintAndLog("ERROR: application parameter length must be 32 bytes only.");
			return 1;
		}
	}
	if (applen)
		memmove(&data[32], adata, 32);
	
	CLIParserFree();	
	
	SetAPDULogging(APDULogging);

	// challenge parameter [32 bytes] - The challenge parameter is the SHA-256 hash of the Client Data, a stringified JSON data structure that the FIDO Client prepares
	// application parameter [32 bytes] - The application parameter is the SHA-256 hash of the UTF-8 encoding of the application identity
	
	uint8_t buf[2048] = {0};
	size_t len = 0;
	uint16_t sw = 0;

	DropField();
	int res = FIDOSelect(true, true, buf, sizeof(buf), &len, &sw);

	if (res) {
		PrintAndLog("Can't select authenticator. res=%x. Exit...", res);
		DropField();
		return res;
	}
	
	if (sw != 0x9000) {
		PrintAndLog("Can't select FIDO application. APDU response status: %04x - %s", sw, GetAPDUCodeDescription(sw >> 8, sw & 0xff)); 
		DropField();
		return 2;
	}

	res = FIDORegister(data, buf,  sizeof(buf), &len, &sw);
	DropField();
	if (res) {
		PrintAndLog("Can't execute register command. res=%x. Exit...", res);
		return res;
	}
	
	if (sw != 0x9000) {
		PrintAndLog("ERROR execute register command. APDU response status: %04x - %s", sw, GetAPDUCodeDescription(sw >> 8, sw & 0xff)); 
		return 3;
	}
	
	PrintAndLog("");
	if (APDULogging)
		PrintAndLog("---------------------------------------------------------------");
	PrintAndLog("data len: %d", len);
	if (verbose) {
		PrintAndLog("--------------data----------------------");
		dump_buffer((const unsigned char *)buf, len, NULL, 0);
		PrintAndLog("--------------data----------------------");
	}

	if (buf[0] != 0x05) {
		PrintAndLog("ERROR: First byte must be 0x05, but it %2x", buf[0]);
		return 5;
	}
	PrintAndLog("User public key: %s", sprint_hex(&buf[1], 65));
	
	uint8_t keyHandleLen = buf[66];
	PrintAndLog("Key handle[%d]: %s", keyHandleLen, sprint_hex(&buf[67], keyHandleLen));
	
	int derp = 67 + keyHandleLen;
	int derLen = (buf[derp + 2] << 8) + buf[derp + 3] + 4;
	// needs to decode DER certificate
	if (verbose) {
		PrintAndLog("DER certificate[%d]:------------------DER-------------------", derLen);
		dump_buffer_simple((const unsigned char *)&buf[67 + keyHandleLen], derLen, NULL);
		PrintAndLog("\n----------------DER---------------------");
	} else {
		PrintAndLog("DER certificate[%d]: %s...", derLen, sprint_hex(&buf[derp], 20));
	}
	
	
	int hashp = 1 + 65 + 1 + keyHandleLen + derLen;
	PrintAndLog("Hash[%d]: %s", len - hashp, sprint_hex(&buf[hashp], len - hashp));
	
	// check ANSI X9.62 format ECDSA signature (on P-256)
	
	PrintAndLog("\nauth command: ");
	printf("hf fido auth %s%s", paramsPlain?"-p ":"", sprint_hex_inrow(&buf[67], keyHandleLen));
	if(chlen || applen)
		printf(" %s", paramsPlain?(char *)cdata:sprint_hex_inrow(cdata, 32));
	if(applen)
		printf(" %s", paramsPlain?(char *)adata:sprint_hex_inrow(adata, 32));
	printf("\n");
	
	if (root) {
		JsonSaveBufAsHex(root, "ChallengeParam", data, 32);
		JsonSaveBufAsHex(root, "ApplicationParam", &data[32], 32);
		JsonSaveInt(root, "KeyHandleLen", keyHandleLen);
		JsonSaveBufAsHexCompact(root, "KeyHandle", &buf[67], keyHandleLen);
		JsonSaveBufAsHexCompact(root, "DER", &buf[67 + keyHandleLen], derLen);
	
		res = json_dump_file(root, fname, JSON_INDENT(2));
		if (res) {
			PrintAndLog("ERROR: can't save the file: %s", fname);
			return 200;
		}
		PrintAndLog("File `%s` saved.", fname);
		
		// free json object
		json_decref(root);
	}
	
	return 0;
};

int CmdHFFidoAuthenticate(const char *cmd) {
	uint8_t data[512] = {0};
	uint8_t hdata[250] = {0};
	int hdatalen = 0;
	uint8_t keyHandleLen = 0;
	json_t *root = NULL;
	
	CLIParserInit("hf fido auth", 
		"Initiate a U2F token authentication. Needs key handle and two 32-byte hash number. \nkey handle(var 0..255), challenge parameter (32b) and application parameter (32b).", 
		"Usage:\n\thf fido auth 000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f -> execute command with 2 parameters, filled 0x00 and key handle\n"
			"\thf fido auth 000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f "
				"000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f 000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f -> execute command with parameters");

	void* argtable[] = {
		arg_param_begin,
		arg_lit0("aA",  "apdu",     "show APDU reqests and responses"),
		arg_lit0("vV",  "verbose",  "show technical data"),
		arg_lit0("pP",  "plain",    "send plain ASCII to challenge and application parameters instead of HEX"),
		arg_rem("default mode:",    "dont-enforce-user-presence-and-sign"),
		arg_lit0("uU",  "user",     "mode: enforce-user-presence-and-sign"),
		arg_lit0("cC",  "check",    "mode: check-only"),
		arg_str0("jJ",  "json",		"fido.json", "JSON input / output file name for parameters."),
		arg_str0(NULL,  NULL,       "<HEX key handle (var 0..255b)>", NULL),
		arg_str0(NULL,  NULL,       "<HEX/ASCII challenge parameter (32b HEX/1..16 chars)>", NULL),
		arg_str0(NULL,  NULL,       "<HEX/ASCII application parameter (32b HEX/1..16 chars)>", NULL),
		arg_param_end
	};
	CLIExecWithReturn(cmd, argtable, true);
	
	bool APDULogging = arg_get_lit(1);
	//bool verbose = arg_get_lit(2);
	bool paramsPlain = arg_get_lit(3);
	uint8_t controlByte = 0x08;
	if (arg_get_lit(5))
		controlByte = 0x03;
	if (arg_get_lit(6))
		controlByte = 0x07;

	char fname[250] = {0};
	bool err;
	root = OpenJson(7, fname, argtable, &err);
	if(err)
		return 1;
	if (root) {	
		size_t jlen;
		JsonLoadBufAsHex(root, "$.ChallengeParam", data, 32, &jlen);
		JsonLoadBufAsHex(root, "$.ApplicationParam", &data[32], 32, &jlen);
		JsonLoadBufAsHex(root, "$.KeyHandle", &data[65], 512 - 67, &jlen);
		keyHandleLen = jlen & 0xff;
		data[64] = keyHandleLen;
	} 

	CLIGetHexWithReturn(8, hdata, &hdatalen);
	if (hdatalen > 255) {
		PrintAndLog("ERROR: application parameter length must be less than 255.");
		return 1;
	}
	if (hdatalen) {
		keyHandleLen = hdatalen;
		data[64] = keyHandleLen;
		memmove(&data[65], hdata, keyHandleLen);
	}

	if (paramsPlain) {
		memset(hdata, 0x00, 32);
		CLIGetStrWithReturn(9, hdata, &hdatalen);
		if (hdatalen && hdatalen > 16) {
			PrintAndLog("ERROR: challenge parameter length in ASCII mode must be less than 16 chars instead of: %d", hdatalen);
			return 1;
		}
	} else {
		CLIGetHexWithReturn(9, hdata, &hdatalen);
		if (hdatalen && hdatalen != 32) {
			PrintAndLog("ERROR: challenge parameter length must be 32 bytes only.");
			return 1;
		}
	}
	if (hdatalen)
		memmove(data, hdata, 32);

	if (paramsPlain) {
		memset(hdata, 0x00, 32);
		CLIGetStrWithReturn(10, hdata, &hdatalen);
		if (hdatalen && hdatalen > 16) {
			PrintAndLog("ERROR: application parameter length in ASCII mode must be less than 16 chars instead of: %d", hdatalen);
			return 1;
		}
	} else {
		CLIGetHexWithReturn(10, hdata, &hdatalen);
		if (hdatalen && hdatalen != 32) {
			PrintAndLog("ERROR: application parameter length must be 32 bytes only.");
			return 1;
		}
	}
	if (hdatalen)
		memmove(&data[32], hdata, 32);

	CLIParserFree();	
	
	SetAPDULogging(APDULogging);

	// (in parameter) conrtol byte 0x07 - check only, 0x03 - user presense + cign. 0x08 - sign only
 	// challenge parameter [32 bytes]
	// application parameter [32 bytes]
	// key handle length [1b] = N
	// key handle [N]

	uint8_t datalen = 32 + 32 + 1 + keyHandleLen;
	
	uint8_t buf[2048] = {0};
	size_t len = 0;
	uint16_t sw = 0;

	DropField();
	int res = FIDOSelect(true, true, buf, sizeof(buf), &len, &sw);

	if (res) {
		PrintAndLog("Can't select authenticator. res=%x. Exit...", res);
		DropField();
		return res;
	}
	
	if (sw != 0x9000) {
		PrintAndLog("Can't select FIDO application. APDU response status: %04x - %s", sw, GetAPDUCodeDescription(sw >> 8, sw & 0xff)); 
		DropField();
		return 2;
	}

	res = FIDOAuthentication(data, datalen, controlByte,  buf,  sizeof(buf), &len, &sw);
	DropField();
	if (res) {
		PrintAndLog("Can't execute authentication command. res=%x. Exit...", res);
		return res;
	}
	
	if (sw != 0x9000) {
		PrintAndLog("ERROR execute authentication command. APDU response status: %04x - %s", sw, GetAPDUCodeDescription(sw >> 8, sw & 0xff)); 
		return 3;
	}
	
	PrintAndLog("---------------------------------------------------------------");
	PrintAndLog("User presence: %s", (buf[0]?"verified":"not verified"));
	uint32_t cntr =  (uint32_t)bytes_to_num(&buf[1], 4);
	PrintAndLog("Counter: %d", cntr);
	PrintAndLog("Hash[%d]: %s", len - 5, sprint_hex(&buf[5], len - 5));

	if (root) {
		JsonSaveBufAsHex(root, "ChallengeParam", data, 32);
		JsonSaveBufAsHex(root, "ApplicationParam", &data[32], 32);
		JsonSaveInt(root, "KeyHandleLen", keyHandleLen);
		JsonSaveBufAsHexCompact(root, "KeyHandle", &data[65], keyHandleLen);
		JsonSaveInt(root, "Counter", cntr);
	
		res = json_dump_file(root, fname, JSON_INDENT(2));
		if (res) {
			PrintAndLog("ERROR: can't save the file: %s", fname);
			return 200;
		}
		PrintAndLog("File `%s` saved.", fname);
		
		// free json object
		json_decref(root);
	}
	return 0;
};

static command_t CommandTable[] =
{
  {"help",             CmdHelp,					1, "This help."},
  {"info",  	       CmdHFFidoInfo,			0, "Info about FIDO tag."},
  {"reg",  	  	 	   CmdHFFidoRegister,		0, "FIDO U2F Registration Message."},
  {"auth",  	       CmdHFFidoAuthenticate,	0, "FIDO U2F Authentication Message."},
  {NULL,               NULL,					0, NULL}
};

int CmdHFFido(const char *Cmd) {
	(void)WaitForResponseTimeout(CMD_ACK,NULL,100);
	CmdsParse(CommandTable, Cmd);
	return 0;
}

int CmdHelp(const char *Cmd) {
  CmdsHelp(CommandTable);
  return 0;
}
