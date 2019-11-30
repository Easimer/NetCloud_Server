#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"
#include "protocol.h"
#include "stdio_nc.h"
#include "packet_signing.h"

void HandleFileExists(Client& cli, Packet_File_Generic_Path* pkt) {
	int res;
	FILE* f;
	char* filename;
	Packet_File_Write_Result pktResult;
	pktResult.hdr.len = sizeof(pktResult);
	pktResult.hdr.cmd = CMD_EXISTS;
	printf("Processing file existence request from user %ld\n", cli.userID);
	if(AuthenticateClientPacket(pkt, pkt->hdr.len, cli)) {
		const char* pktPath = (const char*)(pkt + 1);
		filename = new char[pkt->cubFileName + 1];
		assert(filename);
		memcpy(filename, pktPath, pkt->cubFileName);
		filename[pkt->cubFileName] = 0;
		printf("Client %ld is looking for file '%s'\n", cli.userID, filename);
		f = fopen_nc(filename, "rb", cli.userID, cli.appID);
		// TODO: Do bounds check
		if(f) {
			pktResult.result = 0x01;
			fclose(f);
			printf("File '%s' has been written\n", filename);
		} else {
			pktResult.result = 0x00;
		}
		delete[] filename;
	} else {
		pktResult.result = 0x00;
		printf("Failed to auth write request\n");
	}

	printf("Sending existence request result\n");
	SignServerPacket(pktResult, cli.sessionKey);
	res = send(cli.socket, &pktResult, sizeof(pktResult), 0);
	assert(res == sizeof(pktResult));
}
