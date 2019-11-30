#include "NetCloud.h"

#include <assert.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "AdvApi32.lib")

#include <openssl/engine.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <netcloud/protocol.h>

enum class NCState {
    LoggedOut, // Before sending the login packet
    SentLogin,
    AnswerSent,
    Operation,
};

static void SignBytes(HMAC_MD hmac, const void* buf, unsigned len, const Session_Key& session) {
    unsigned int cubMD = sizeof(HMAC_MD);
    assert(hmac && buf && session);

    Log()->printf("NetCloud: H(sha256, session, %d, buf, %d, hmac, &%d)", sizeof(Session_Key), len, cubMD);
    HMAC(EVP_sha256(), session, sizeof(Session_Key), (unsigned char*)buf, len, hmac, &cubMD);
}

static void CalculateHMAC(HMAC_MD hmac, const Packet_Header* pkt, const Session_Key& session) {
    assert(hmac && pkt && session);

    SignBytes(hmac, pkt, pkt->len, session);
}

static bool CheckHMAC(Packet_Header* pkt, const Session_Key& session) {
    bool ret;
    HMAC_MD original;
    unsigned int cubMD = sizeof(HMAC_MD);
    // Copy original hmac
    // Calculate hmac
    // compare

    memcpy(original, pkt->hmac, sizeof(HMAC_MD));
    memset(pkt->hmac, 0, sizeof(HMAC_MD));

    HMAC(EVP_sha256(), session, sizeof(Session_Key), (unsigned char*)pkt, pkt->len, pkt->hmac, &cubMD);

    ret = memcmp(pkt->hmac, original, sizeof(HMAC_MD)) == 0;

    memcpy(pkt->hmac, original, sizeof(HMAC_MD));

    return ret;
}

template<typename T>
static bool AuthenticateServerPacket(T& packet, const Session_Key& session) {
    return CheckHMAC(&packet.hdr, session);
}

template<>
static bool AuthenticateServerPacket(const Packet_Login& pkt, const Session_Key& session) { return true; }

template<typename T>
static void SignClientPacket(T& packet, const Session_Key& session) {
    memset(packet.hdr.hmac, 0, HMAC_LEN);
    CalculateHMAC(packet.hdr.hmac, &packet.hdr, session);
}

template<>
static void SignClientPacket(Packet_File_Generic_Path& packet, const Session_Key& session) {
    unsigned int cubMD = 32;
    HMAC_CTX ctx;
    memset(packet.hdr.hmac, 0, HMAC_LEN);

    HMAC_CTX_init(&ctx);
    HMAC_Init_ex(&ctx, session, SESSION_KEY_LEN, EVP_sha256(), NULL);
    HMAC_Update(&ctx, (unsigned char*)&packet, sizeof(packet));
    HMAC_Update(&ctx, (unsigned char*)((&packet) + 1), packet.cubFileName);
    HMAC_Final(&ctx, packet.hdr.hmac, &cubMD);
    HMAC_CTX_cleanup(&ctx);
}

static void PrintBytes(Session_Key bytes) {
    char buf[256];
    buf[0] = 0;
    for (int i = 0; i < SESSION_KEY_LEN; i++) {
        char buf2[32] = { 0 };
        snprintf(buf2, 32, "%x:", bytes[i]);
        strcat(buf, buf2);
    }
    Log()->printf("NetCloud: Session key: %s\n", buf);
}

static void PrintBytes(const char* label, HMAC_MD bytes) {
    char buf[256];
    buf[0] = 0;
    for (int i = 0; i < SESSION_KEY_LEN; i++) {
        char buf2[32] = { 0 };
        snprintf(buf2, 32, "%x:", bytes[i]);
        strcat(buf, buf2);
    }
    Log()->printf("NetCloud: %s: %s\n", label, buf);
}

static void CreateSessionKey(Session_Key& session, const uint8 shared[64], const char* userKey) {
    unsigned int cubMD = sizeof(Session_Key);
    assert(session && shared && userKey);

    HMAC(EVP_sha256(), userKey, strlen(userKey), shared, 64, session, &cubMD);
}

class CNetCloudSession : public INetCloudSession {
public:
    CNetCloudSession() :
        m_hSocket(INVALID_SOCKET),
        m_pchKey(NULL),
        m_iUserID(0),
        m_state(NCState::LoggedOut) {

        ENGINE_load_builtin_engines();
        ENGINE_register_all_complete();
        Log()->printf("OpenSSL engine init done");
    }

    virtual void Release() override {
        Logout();
        delete this;
    }

    virtual NetCloudResult Login(unsigned long long userID, const char* userKey, unsigned appID) override {
        struct addrinfo hints;
        struct addrinfo *addr, *ptr;
        int res;
        Packet_Login pktLogin;
        Packet_Auth_Challenge pktChallenge;
        Packet_Auth_Answer pktAnswer;
        Packet_Auth_Result pktResult;
        // Assuming that CDebugLog already initialized WSA

        assert(m_state == NCState::LoggedOut);

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        res = getaddrinfo("steamworks.easimer.net", NETCLOUD_PORT, &hints, &addr);
        if (res != 0) {
            Log()->fprintf(log_urgent, "NetCloud: failed to resolve steamworks.easimer.net\n");
            return NetCloudResult::Network;
        }

        for (ptr = addr; ptr; ptr = ptr->ai_next) {
            m_hSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (m_hSocket == INVALID_SOCKET) {
                Log()->fprintf(log_urgent, "NetCloud: failed to open socket\n");
                return NetCloudResult::Network;
            }

            res = connect(m_hSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (res == SOCKET_ERROR) {
                closesocket(m_hSocket);
                m_hSocket = INVALID_SOCKET;
                continue;
            }
            break;
        }

        freeaddrinfo(addr);

        if (m_hSocket == INVALID_SOCKET) {
            Log()->fprintf(log_urgent, "NetCloud: couldn't connect to steamworks.easimer.net (can't find a working protocol!)\n");
            return NetCloudResult::Network;
        }

        Log()->printf("NetCloud: connected!\n");

        m_iUserID = userID;
        m_pchKey = new char[strlen(userKey) + 1];
        memcpy(m_pchKey, userKey, strlen(userKey) + 1);

        // Send a CMD_LOGIN
        pktLogin.hdr.cmd = CMD_LOGIN;
        pktLogin.hdr.len = sizeof(pktLogin);
        memset(pktLogin.hdr.hmac, 0, 32);
        pktLogin.userID = userID;
        pktLogin.appID = appID;
        send(m_hSocket, (char*)&pktLogin, sizeof(pktLogin), 0);

        m_state = NCState::SentLogin;

        // Receive challenge
        recv(m_hSocket, (char*)&pktChallenge, sizeof(pktChallenge), 0);
        // Create session key
        CreateSessionKey(m_sessionKey, pktChallenge.shared, m_pchKey);
        PrintBytes(m_sessionKey);
        // Sign the challenge
        SignBytes(pktAnswer.answer, pktChallenge.challenge, 32, m_sessionKey);
        PrintBytes("Challenge answer", pktAnswer.answer);
        // Send response
        pktAnswer.hdr.cmd = CMD_AUTH;
        pktAnswer.hdr.len = sizeof(pktAnswer);
        SignClientPacket(pktAnswer, m_sessionKey);
        send(m_hSocket, (char*)&pktAnswer, sizeof(pktAnswer), 0);
        m_state = NCState::AnswerSent;

        // Receive auth result
        recv(m_hSocket, (char*)&pktResult, sizeof(pktResult), 0);
        assert(pktResult.hdr.cmd == CMD_AUTHRES);
        if (AuthenticateServerPacket(pktResult, m_sessionKey)) {
            if (pktResult.result) {
                Log()->fprintf(log_urgent, "NetCloud: received auth result: OK");
                m_state = NCState::Operation;
            } else {
                Log()->fprintf(log_urgent, "NetCloud: received auth result: fail");
                m_state = NCState::LoggedOut;
                return NetCloudResult::Unauthorized;
            }
        } else {
            Log()->fprintf(log_urgent, "NetCloud: received auth result, but couldn't auth it!! (res=%u)", pktResult.result);
            m_state = NCState::LoggedOut;
            return NetCloudResult::Network;
        }

        return NetCloudResult::OK;
    }

    virtual NetCloudResult Logout() override {
        if (m_hSocket) {
            closesocket(m_hSocket);
            m_hSocket = INVALID_SOCKET;
        }
        if (m_pchKey) {
            delete[] m_pchKey;
        }
        m_iUserID = 0;
        m_state = NCState::LoggedOut;
        return NetCloudResult::OK;
    }

    virtual NetCloudResult FileWrite(const char* pchFile, const void* pvData, int cubData) override {
        if (m_state == NCState::Operation) {
            unsigned int cubMD = 32;
            int res;
            HMAC_CTX ctx;
            Packet_File_Write wr;
            Packet_File_Write_Result wrr;
            wr.hdr.cmd = CMD_WRITE;
            wr.cubFileName = strlen(pchFile);
            wr.cubFileContents = cubData;
            wr.hdr.len = sizeof(wr) + wr.cubFileName + wr.cubFileContents;
            memset(wr.hdr.hmac, 0, HMAC_LEN);

            Log()->printf("NetCloud: FileWrite: calculating HMAC\n");

            HMAC_CTX_init(&ctx);
            HMAC_Init_ex(&ctx, m_sessionKey, SESSION_KEY_LEN, EVP_sha256(), NULL);
            HMAC_Update(&ctx, (unsigned char*)&wr, sizeof(wr));
            HMAC_Update(&ctx, (unsigned char*)pchFile, wr.cubFileName);
            HMAC_Update(&ctx, (unsigned char*)pvData, wr.cubFileContents);
            HMAC_Final(&ctx, wr.hdr.hmac, &cubMD);
            HMAC_CTX_cleanup(&ctx);

            Log()->printf("NetCloud: FileWrite: calculated HMAC\n");

            // Send file write request
            res  = send(m_hSocket, (char*)&wr, sizeof(wr), 0);
            res += send(m_hSocket, (char*)pchFile, wr.cubFileName, 0);
            res += send(m_hSocket, (char*)pvData, wr.cubFileContents, 0);

            Log()->printf("NetCloud: FileWrite: sent request %d\n", res);
            PrintBytes("NetCloud: FileWrite", wr.hdr.hmac);

            // Receive file write confirmation
            res = recv(m_hSocket, (char*)&wrr, sizeof(wrr), 0);

            Log()->printf("NetCloud: FileWrite: received result\n");

            if (AuthenticateServerPacket(wrr, m_sessionKey)) {
                return wrr.result == 0x01 ? NetCloudResult::OK : NetCloudResult::Fail;
            } else {
                return NetCloudResult::Fail;
            }
        } else {
            Log()->printf("NetCloud: FileWrite failed: not in Operating state!\n");
            return NetCloudResult::Fail;
        }
    }

    virtual NetCloudResult FileRead(const char* pchFile, void* pvData, int* cubData) override {
        if (m_state == NCState::Operation) {
            unsigned int cubMD = 32;
            HMAC_MD hmacCalculatedResult, hmacResultOriginal;
            int res;
            HMAC_CTX ctx;
            Packet_File_Read rr;
            Packet_File_Read_Result rrr;

            rr.hdr.cmd = CMD_READ;
            rr.maxReadBytes = *cubData;
            rr.cubFileName = strlen(pchFile);
            rr.hdr.len = sizeof(rr) + rr.cubFileName;
            memset(rr.hdr.hmac, 0, HMAC_LEN);

            HMAC_CTX_init(&ctx);
            HMAC_Init_ex(&ctx, m_sessionKey, SESSION_KEY_LEN, EVP_sha256(), NULL);
            HMAC_Update(&ctx, (unsigned char*)&rr, sizeof(rr));
            HMAC_Update(&ctx, (unsigned char*)pchFile, rr.cubFileName);
            HMAC_Final(&ctx, rr.hdr.hmac, &cubMD);
            HMAC_CTX_cleanup(&ctx);

            // Send file write request
            res  = send(m_hSocket, (char*)&rr, sizeof(rr), 0);
            res += send(m_hSocket, (char*)pchFile, rr.cubFileName, 0);

            Log()->printf("NetCloud: FileRead: sent request %d\n", res);
            PrintBytes("NetCloud: FileRead", rr.hdr.hmac);

            // Receive file read result header
            res = recv(m_hSocket, (char*)&rrr, sizeof(rrr), 0);

            // Manual authentication
            memcpy(hmacResultOriginal, rrr.hdr.hmac, HMAC_LEN);
            memset(rrr.hdr.hmac, 0, HMAC_LEN);
            HMAC_CTX_init(&ctx);
            HMAC_Init_ex(&ctx, m_sessionKey, SESSION_KEY_LEN, EVP_sha256(), NULL);
            HMAC_Update(&ctx, (unsigned char*)&rrr, sizeof(rrr));

            int32 cubRecvLeft = rrr.readBytes;
            assert(rrr.readBytes <= *cubData);
            char* bufRecv = (char*)pvData;

            while (cubRecvLeft > 0) {
                res = recv(m_hSocket, bufRecv, cubRecvLeft, 0);
                if (res > 0) {
                    HMAC_Update(&ctx, (unsigned char*)bufRecv, res);
                    cubRecvLeft -= res;
                    bufRecv += res;
                } else {
                    // TODO: error handling
                }
            }

            HMAC_Final(&ctx, hmacCalculatedResult, &cubMD);
            HMAC_CTX_cleanup(&ctx);

            if (memcmp(hmacCalculatedResult, hmacResultOriginal, HMAC_LEN) == 0) {
                *cubData = rrr.readBytes;

                Log()->printf("NetCloud: FileRead: success (%d bytes)", rrr.readBytes);
                return NetCloudResult::OK;
            } else {
                *cubData = -1;
                Log()->printf("NetCloud: FileRead: failure");
                return NetCloudResult::Fail;
            }
        } else {
            Log()->printf("NetCloud: FileWrite failed: not in Operating state!\n");
            return NetCloudResult::Fail;
        }
    }

    virtual NetCloudResult FileForget(bool* pResult, const char* pchFile) override {
        return NetCloudResult();
    }

    NetCloudResult SendGenericPathCommand(int cmd, const char* pchFile) {
            HMAC_CTX ctx;
            unsigned int cubMD = 32;
            int res;
            Packet_File_Generic_Path pktReq;
            assert(pchFile);

            pktReq.hdr.cmd = cmd;
            pktReq.cubFileName = strlen(pchFile);
            pktReq.hdr.len = sizeof(pktReq) + pktReq.cubFileName;

            memset(pktReq.hdr.hmac, 0, HMAC_LEN);

            HMAC_CTX_init(&ctx);
            HMAC_Init_ex(&ctx, m_sessionKey, SESSION_KEY_LEN, EVP_sha256(), NULL);
            HMAC_Update(&ctx, (unsigned char*)&pktReq, sizeof(pktReq));
            HMAC_Update(&ctx, (unsigned char*)pchFile, pktReq.cubFileName);
            HMAC_Final(&ctx, pktReq.hdr.hmac, &cubMD);
            HMAC_CTX_cleanup(&ctx);

            PrintBytes("NetCloud: FileExists HMAC", pktReq.hdr.hmac);
            
            res  = send(m_hSocket, (char*)&pktReq, sizeof(pktReq), 0);
            res += send(m_hSocket, (char*)pchFile, pktReq.cubFileName, 0);
            assert(res == sizeof(pktReq) + pktReq.cubFileName);

            if (res == sizeof(pktReq) + pktReq.cubFileName) {
                return NetCloudResult::OK;
            } else {
                return NetCloudResult::Network;
            }
    }

    template<typename T>
    NetCloudResult ReceiveFixedSizePacket(T* pktResult) {
        int res;
        res = recv(m_hSocket, (char*)pktResult, sizeof(*pktResult), 0);
        assert(res == sizeof(*pktResult));
        if (res == sizeof(*pktResult)) {
            if (AuthenticateServerPacket(*pktResult, m_sessionKey)) {
                return NetCloudResult::OK;
            } else {
                return NetCloudResult::Unauthorized;
            }
        } else {
            return NetCloudResult::Network;
        }
    }

    NetCloudResult ReceiveGenericResult(Packet_General_Result* pktResult) {
        return ReceiveFixedSizePacket(pktResult);
    }

    virtual NetCloudResult FileDelete(bool* pResult, const char* pchFile) override {
        NetCloudResult res;
        if (m_state == NCState::Operation) {
            Packet_General_Result pktResult;
            
            res = SendGenericPathCommand(CMD_EXISTS, pchFile);

            if(res == NetCloudResult::OK) {
                res = ReceiveGenericResult(&pktResult);

                if (res == NetCloudResult::OK) {
                    *pResult = pktResult.result == 0x01;
                    return NetCloudResult::OK;
                } else {
                    return res;
                }
            } else {
                return res;
            }
        } else {
            return NetCloudResult::Fail;
        }
    }

    virtual NetCloudResult FileExists(bool* pResult, const char* pchFile) override {
        NetCloudResult res;
        if (m_state == NCState::Operation) {
            Packet_General_Result pktResult;
            
            Log()->printf("NetCloud: looking for '%s'", pchFile);
            res = SendGenericPathCommand(CMD_EXISTS, pchFile);

            if(res == NetCloudResult::OK) {
                res = ReceiveGenericResult(&pktResult);

                if (res == NetCloudResult::OK) {
                    Log()->printf("NetCloud: received result");
                    *pResult = pktResult.result == 0x01;
                    return NetCloudResult::OK;
                } else {
                    Log()->printf("NetCloud: didn't receive result");
                    return res;
                }
            } else {
                Log()->printf("NetCloud: couldn't send existence check");
                return res;
            }
        } else {
            return NetCloudResult::Fail;
        }
    }

    virtual NetCloudResult GetFileSize(int* result, const char* pchFile) override {
        NetCloudResult res;
        if (m_state == NCState::Operation) {
            Packet_File_Size_Result pktResult;
            
            res = SendGenericPathCommand(CMD_SIZE, pchFile);

            if(res == NetCloudResult::OK) {
                res = ReceiveFixedSizePacket(&pktResult);

                if (res == NetCloudResult::OK) {
                    *result = pktResult.fileLength;
                    return NetCloudResult::OK;
                } else {
                    return res;
                }
            } else {
                return res;
            }
        } else {
            return NetCloudResult::Fail;
        }
    }

private:
    SOCKET m_hSocket;
    char* m_pchKey;
    uint64 m_iUserID;
    Session_Key m_sessionKey;
    NCState m_state;
};

INetCloudSession* CreateNetCloudSession() {
    return new CNetCloudSession;
}
