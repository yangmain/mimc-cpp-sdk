#ifndef MIMC_CPP_SDK_CONSTANT_H
#define MIMC_CPP_SDK_CONSTANT_H

#include <google/protobuf/message.h>
#include <mimc/mimc.pb.h>
#include <log4cplus/logger.h>

enum PacketPayloadType {
    THRIFT = 1,
    PROTOBUF = 2,
    XML = 3,
};

enum OnlineStatus {
	Offline,
	Online
};

enum MessageDirection {
	C2S_SINGLE_DIRECTION,
	C2S_DOUBLE_DIRECTION
};

struct waitToSendContent
{
	std::string cmd;
	MessageDirection type;
	google::protobuf::Message * message;
};

struct waitToTimeoutContent
{
	mimc::MIMCPacket * mimcPacket;
	long timestamp;
};

const short HEADER_MAGIC = 0xc2fe;
const short HEADER_VERSION = 0x0005;
const short BODY_HEADER_PAYLOADTYPE = PROTOBUF;
const unsigned int BODY_PAYLOAD_CONN_VERSION = 106;
const int BODY_PAYLOAD_CONN_SDK = 33;
const char* const FE_DOMAIN = "app.chat.xiaomi.net";
const int FE_PORT = 5222;
const int LOGIN_TIMEOUT = 10;
const int CONNECT_TIMEOUT = 5;
const int RESETSOCK_TIMEOUT = 5;
const int HTTP_TIMEOUT = CONNECT_TIMEOUT;
const int SEND_TIMEOUT = CONNECT_TIMEOUT * 2;
const int PING_TIMEINTERVAL = 15;

const char* const BODY_CLIENTHEADER_CMD_CONN = "CONN";
const char* const BODY_CLIENTHEADER_CMD_BIND = "BIND";
const char* const BODY_CLIENTHEADER_CMD_PING = "PING";
const char* const BODY_CLIENTHEADER_CMD_UNBIND = "UBND";
const char* const BODY_CLIENTHEADER_CMD_SECMSG = "SECMSG";
const char* const BODY_CLIENTHEADER_CMD_KICK = "KICK";

const int BODY_CLIENTHEADER_CIPHER_NONE = 0;
const int BODY_CLIENTHEADER_CIPHER_RC4 = 1;
const int BODY_CLIENTHEADER_CIPHER_AES = 2;

const int HEADER_LENGTH = 8;
const int BODY_HEADER_LENGTH = 8;
const int HEADER_MAGIC_OFFSET = 0;
const int HEADER_VERSION_OFFSET = 2;
const int HEADER_BODYLEN_OFFSET = 4;
const int BODY_OFFSET = 8;
const int BODY_HEADER_PAYLOADTYPE_OFFSET = 0;
const int BODY_HEADER_HEADERLEN_OFFSET = 2;
const int BODY_HEADER_PAYLOADLEN_OFFSET = 4;

const int BODY_CRC_LEN = 4;

const int MIMC_CHID = 9;

const int MIMC_MAX_PAYLOAD_SIZE = 10 * 1024;

const char * const MIMC_SERVER = "xiaomi.com";

const char * const NO_KICK = "0";
const char * const METHOD = "XIAOMI-PASS";

const log4cplus::Logger LOGGER = log4cplus::Logger::getRoot(); 

#endif //MIMC_CPP_SDK_CONSTANT_H
