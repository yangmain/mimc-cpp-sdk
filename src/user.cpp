#include <mimc/user.h>
#include <mimc/connection.h>
#include <mimc/packet_manager.h>
#include <mimc/serverfetcher.h>
#include <mimc/p2p_callsession.h>
#include <mimc/utils.h>
#include <mimc/threadsafe_queue.h>
#include <mimc/rts_connection_handler.h>
#include <mimc/rts_stream_handler.h>
#include <mimc/rts_send_data.h>
#include <mimc/rts_send_signal.h>
#include <mimc/ims_push_service.pb.h>
#include <mimc/rts_signal.pb.h>
#include <mimc/rts_context.h>
#include <XMDTransceiver.h>
#include <json-c/json.h>
#include <fstream>
#include <stdlib.h>
#include <thread>
#include <chrono>

#ifdef _WIN32
#else
#include <unistd.h>
#endif // _WIN32

User::User(int64_t appId, std::string appAccount, std::string resource, std::string cachePath) 
	:audioStreamConfig(ACK_TYPE, ACK_STREAM_WAIT_TIME_MS, false), videoStreamConfig(FEC_TYPE, ACK_STREAM_WAIT_TIME_MS, false) {
	XMDLoggerWrapper::instance()->setXMDLogLevel(XMD_INFO);
	this->appId = appId;
	this->appAccount = appAccount;
	this->testPacketLoss = 0;
	this->permitLogin = false;
	this->onlineStatus = Offline;
	this->tokenInvalid = false;
	this->addressInvalid = false;
	this->tokenRequestStatus = -1;
	this->tokenFetchSucceed = false;
	this->serverFetchSucceed = false;
	this->bindRelayResponse = NULL;
	this->resetRelayLinkState();

	if (resource == "") {
		resource = "cpp_default";
	}
	this->cacheExist = false;
	if (cachePath == "") {
		char buffer[MAXPATHLEN];
		Utils::getCwd(buffer, MAXPATHLEN);
		cachePath = buffer;
	}
	if (cachePath.back() == '/') {
		cachePath.pop_back();
	}
	this->cachePath = cachePath + '/' + Utils::int2str(appId) + '/' + appAccount + '/' + resource;
	XMDLoggerWrapper::instance()->info("cachePath is %s", this->cachePath.c_str());
	this->cacheFile = this->cachePath + '/' + "mimc.info";
	createCacheFileIfNotExist(this);
	if (resource == "cpp_default") {
		this->resource = Utils::generateRandomString(8);
	} else {
		this->resource = resource;
	}

	this->lastLoginTimestamp = 0;
	this->lastCreateConnTimestamp = 0;
	this->lastPingTimestamp = 0;
	this->tokenFetcher = NULL;
	this->statusHandler = NULL;
	this->messageHandler = NULL;
	this->rtsCallEventHandler = NULL;
	this->rtsConnectionHandler = NULL;
	this->rtsStreamHandler = NULL;
	this->packetManager = new PacketManager();

	this->xmdSendBufferSize = 0;
	this->xmdRecvBufferSize = 0;

	this->currentCalls = new std::map<uint64_t, P2PCallSession>();
	this->onlaunchCalls = new std::map<uint64_t, pthread_t>();

	this->xmdTranseiver = NULL;
	this->maxCallNum = 1;

	this->mutex_0 = PTHREAD_RWLOCK_INITIALIZER;
	this->mutex_1 = PTHREAD_MUTEX_INITIALIZER;

	this->conn = new Connection();
	this->conn->setUser(this);

	pthread_attr_t attr1;
	pthread_attr_init(&attr1);
	pthread_attr_setdetachstate(&attr1, PTHREAD_CREATE_DETACHED);
	pthread_create(&sendThread, &attr1, sendPacket, (void *)(this->conn));
	pthread_attr_destroy(&attr1);

	pthread_attr_t attr2;
	pthread_attr_init(&attr2);
	pthread_attr_setdetachstate(&attr2, PTHREAD_CREATE_DETACHED);
	pthread_create(&receiveThread, &attr2, receivePacket, (void *)(this->conn));
	pthread_attr_destroy(&attr2);

	pthread_attr_t attr3;
	pthread_attr_init(&attr3);
	pthread_attr_setdetachstate(&attr3, PTHREAD_CREATE_DETACHED);
	pthread_create(&checkThread, &attr3, checkTimeout, (void *)(this->conn));
	pthread_attr_destroy(&attr3);

}

User::~User() {
#ifndef __ANDROID__
	pthread_cancel(sendThread);
	pthread_cancel(receiveThread);
	pthread_cancel(checkThread);
#else
	if(pthread_kill(sendThread, 0) != ESRCH)
    {
        pthread_kill(sendThread, SIGTERM);
    }

	if(pthread_kill(receiveThread, 0) != ESRCH)
    {
        pthread_kill(receiveThread, SIGTERM);
    }

	if(pthread_kill(checkThread, 0) != ESRCH)
    {
        pthread_kill(checkThread, SIGTERM);
    }
#endif

	if (this->xmdTranseiver) {
		this->xmdTranseiver->stop();
		this->xmdTranseiver->join();
	}

	delete this->packetManager;
	delete this->currentCalls;
	delete this->onlaunchCalls;
	delete this->xmdTranseiver;
	this->conn->resetSock();
	delete this->conn;
	delete this->rtsConnectionHandler;
	delete this->rtsStreamHandler;
}

#ifdef __ANDROID__
void User::handle_quit(int signo)
{
    pthread_exit(NULL);   
}
#endif

void* User::sendPacket(void *arg) {
#ifndef __ANDROID__
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
#else
	signal(SIGTERM, handle_quit);
#endif

	XMDLoggerWrapper::instance()->info("sendThread start to run");
	Connection *conn = (Connection *)arg;
	User * user = conn->getUser();
	unsigned char * packetBuffer = NULL;
	int packet_size = 0;
	MessageDirection msgType;

	while (true)
	 {
		msgType = C2S_DOUBLE_DIRECTION;
		if (conn->getState() == NOT_CONNECTED) {

			time_t now = time(NULL);
			if (now - user->lastCreateConnTimestamp <= CONNECT_TIMEOUT) {
				//usleep(10000);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			if (user->getTestPacketLoss() >= 100) {
				//usleep(10000);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			if (!fetchToken(user) || !fetchServerAddr(user)) {
				//usleep(1000);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			user->lastCreateConnTimestamp = time(NULL);
			XMDLoggerWrapper::instance()->info("Prepare to connect");
			if (!conn->connect()) {
				XMDLoggerWrapper::instance()->error("In sendPacket, socket connect failed, user is %s", user->getAppAccount().c_str());
				continue;
			}
			XMDLoggerWrapper::instance()->info("Socket connected");
			conn->setState(SOCK_CONNECTED);

			packet_size = user->getPacketManager()->encodeConnectionPacket(packetBuffer, conn);
			if (packet_size < 0) {

				continue;
			}
		}
		if (conn->getState() == SOCK_CONNECTED) {
			//usleep(5000);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (conn->getState() == HANDSHAKE_CONNECTED) {
			time_t now = time(NULL);
			if (user->getOnlineStatus() == Offline) {
				if (now - user->lastLoginTimestamp > LOGIN_TIMEOUT || user->tokenInvalid) {
					if (user->permitLogin && fetchToken(user)) {
						packet_size = user->getPacketManager()->encodeBindPacket(packetBuffer, conn);
						if (packet_size < 0) {
							continue;
						}
						user->lastLoginTimestamp = time(NULL);
					}
				}
			} else {
				struct waitToSendContent obj;
				if (user->getPacketManager()->packetsWaitToSend.pop(obj)) {
					msgType = obj.type;
					if (obj.cmd == BODY_CLIENTHEADER_CMD_SECMSG) {
						packet_size = user->getPacketManager()->encodeSecMsgPacket(packetBuffer, conn, obj.message);
						if (packet_size < 0) {

							continue;
						}
					}
					else if (obj.cmd == BODY_CLIENTHEADER_CMD_UNBIND) {
						packet_size = user->getPacketManager()->encodeUnBindPacket(packetBuffer, conn);
						if (packet_size < 0) {

							continue;
						}
					}
				} else {
					if (now - user->lastPingTimestamp > PING_TIMEINTERVAL) {
						packet_size = user->getPacketManager()->encodePingPacket(packetBuffer, conn);
						if (packet_size < 0) {

							continue;
						}

					}
				}
			}
		}

		if (packetBuffer == NULL) {
			//usleep(10000);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		if (msgType == C2S_DOUBLE_DIRECTION) {
			conn->trySetNextResetTs();
		}

		user->lastPingTimestamp = time(NULL);

		if (user->getTestPacketLoss() < 100) {
			int ret = conn->writen((void *)packetBuffer, packet_size);
			if (ret != packet_size) {
				XMDLoggerWrapper::instance()->error("In sendPacket, ret != packet_size, ret=%d, packet_size=%d", ret, packet_size);
				conn->resetSock();
			}
			else {

			}
		}

		delete[] packetBuffer;
		packetBuffer = NULL;
	}

	return NULL;
}

void* User::receivePacket(void *arg) {
#ifndef __ANDROID__
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
#else
	signal(SIGTERM, handle_quit);
#endif

	XMDLoggerWrapper::instance()->info("receiveThread start to run");
	Connection *conn = (Connection *)arg;
	User * user = conn->getUser();
	
	while (true)
	{
		if (conn->getState() == NOT_CONNECTED) {
			//usleep(20000);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}

		unsigned char * packetHeaderBuffer = new unsigned char[HEADER_LENGTH];
		memset(packetHeaderBuffer, 0, HEADER_LENGTH);

		int ret = conn->readn((void *)packetHeaderBuffer, HEADER_LENGTH);
		if (ret != HEADER_LENGTH) {

			conn->resetSock();

			delete[] packetHeaderBuffer;
			packetHeaderBuffer = NULL;

			continue;
		}
		else {
			int body_len = user->getPacketManager()->char2int(packetHeaderBuffer, HEADER_BODYLEN_OFFSET) + BODY_CRC_LEN;
			unsigned char * packetBodyBuffer = new unsigned char[body_len];
			memset(packetBodyBuffer, 0, body_len);
			ret = conn->readn((void *)packetBodyBuffer, body_len);
			if (ret != body_len) {

				conn->resetSock();

				delete[] packetBodyBuffer;
				packetBodyBuffer = NULL;

				continue;
			}
			else {
				if (user->getTestPacketLoss() >= 100) {
					delete[] packetHeaderBuffer;
					packetHeaderBuffer = NULL;

					delete[] packetBodyBuffer;
					packetBodyBuffer = NULL;

					continue;
				}
				conn->clearNextResetSockTs();
				int packet_size = HEADER_LENGTH + body_len;
				unsigned char *packetBuffer = new unsigned char[packet_size];
				memset(packetBuffer, 0, packet_size);
				memmove(packetBuffer, packetHeaderBuffer, HEADER_LENGTH);
				memmove(packetBuffer + HEADER_LENGTH, packetBodyBuffer, body_len);

				delete[] packetHeaderBuffer;
				packetHeaderBuffer = NULL;

				delete[] packetBodyBuffer;
				packetBodyBuffer = NULL;

				ret = user->getPacketManager()->decodePacketAndHandle(packetBuffer, conn);
				delete[] packetBuffer;
				packetBuffer = NULL;
				if (ret < 0) {
					conn->resetSock();
				}
			}
		}
	}

	return NULL;
}

void* User::checkTimeout(void *arg) {
#ifndef __ANDROID__
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
#else
	signal(SIGTERM, handle_quit);
#endif

	XMDLoggerWrapper::instance()->info("checkThread start to run");
	Connection *conn = (Connection *)arg;
	User * user = conn->getUser();
	
	while (true)
	 {
		if ((conn->getNextResetSockTs() > 0) && (time(NULL) - conn->getNextResetSockTs() > 0)) {
			XMDLoggerWrapper::instance()->info("In checkTimeout, packet recv timeout");
			conn->resetSock();
		}
		user->getPacketManager()->checkMessageSendTimeout(user);
		user->rtsScanAndCallBack();
		user->relayConnScanAndCallBack();
		RtsSendData::sendPingRelayRequest(user);
		//sleep(1);
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	return NULL;
}

void User::relayConnScanAndCallBack() {
	if (this->relayLinkState != BEING_CREATED) {
		return;
	}
	if (time(NULL) - this->latestLegalRelayLinkStateTs < RELAY_CONN_TIMEOUT) {
		return;
	}

	if (this->relayConnId != 0) {
		this->xmdTranseiver->closeConnection(relayConnId);
	}
	pthread_rwlock_wrlock(&mutex_0);
	for (std::map<uint64_t, P2PCallSession>::const_iterator iter = currentCalls->begin(); iter != currentCalls->end(); iter++) {
		uint64_t callId = iter->first;
		XMDLoggerWrapper::instance()->error("In relayConnScanAndCallBack >= RELAY_CONN_TIMEOUT, callId=%llu", callId);
		const P2PCallSession& callSession = iter->second;
		if (callSession.getCallState() == WAIT_SEND_UPDATE_REQUEST) {
			RtsSendSignal::sendByeRequest(this, callId, UPDATE_TIMEOUT);
		}
	}
	this->currentCalls->clear();
	pthread_rwlock_unlock(&mutex_0);
	this->resetRelayLinkState();
}

void User::rtsScanAndCallBack() {
	pthread_rwlock_wrlock(&mutex_0);
	if (this->currentCalls->empty()) {
		pthread_rwlock_unlock(&mutex_0);
		return;
	}

	for (std::map<uint64_t, P2PCallSession>::const_iterator iter = this->currentCalls->begin(); iter != this->currentCalls->end();) {
		const uint64_t& callId = iter->first;
		const P2PCallSession& callSession = iter->second;
		const CallState& callState = callSession.getCallState();
		if (callState == RUNNING) {
			XMDLoggerWrapper::instance()->info("In rtsScanAndCallBack, callId %llu state RUNNING ping callCenter, user is %s", callId, appAccount.c_str());
			RtsSendSignal::pingCallCenter(this, callId);
			iter++;
			continue;
		}

		if (time(NULL) - callSession.getLatestLegalCallStateTs() < RTS_CHECK_TIMEOUT) {
			iter++;
			continue;
		}

		if (callState == WAIT_CREATE_RESPONSE) {
			if (time(NULL) - callSession.getLatestLegalCallStateTs() >= RTS_CALL_TIMEOUT) {
				XMDLoggerWrapper::instance()->info("In rtsScanAndCallBack, callId %llu state WAIT_CREATE_RESPONSE is timeout, user is %s", callId, appAccount.c_str());
				this->currentCalls->erase(iter++);
				rtsCallEventHandler->onAnswered(callId, false, DIAL_CALL_TIMEOUT);
			} else {
				iter++;
			}
		} else if (callState == WAIT_INVITEE_RESPONSE) {
			if (time(NULL) - callSession.getLatestLegalCallStateTs() >= RTS_CALL_TIMEOUT) {
				XMDLoggerWrapper::instance()->info("In rtsScanAndCallBack, callId %llu state WAIT_INVITEE_RESPONSE is timeout, user is %s", callId, appAccount.c_str());
				if (this->onlaunchCalls->count(callId) > 0) {
					pthread_t onlaunchCallThread = this->onlaunchCalls->at(callId);
				#ifndef __ANDROID__
					pthread_cancel(onlaunchCallThread);
				#else
					if(pthread_kill(onlaunchCallThread, 0) != ESRCH)
    				{
        				pthread_kill(onlaunchCallThread, SIGTERM);
    				}
    			#endif
					this->onlaunchCalls->erase(callId);
				}
				this->currentCalls->erase(iter++);
				rtsCallEventHandler->onClosed(callId, INVITE_RESPONSE_TIMEOUT);
			} else {
				iter++;
			}
		} else if (callState == WAIT_UPDATE_RESPONSE) {
			XMDLoggerWrapper::instance()->info("In rtsScanAndCallBack, callId %llu state WAIT_UPDATE_RESPONSE is timeout, user is %s", callId, appAccount.c_str());
			RtsSendSignal::sendByeRequest(this, callId, UPDATE_TIMEOUT);
			this->currentCalls->erase(iter++);
			rtsCallEventHandler->onClosed(callId, UPDATE_TIMEOUT);
		} else {
			iter++;
		}
		RtsSendData::closeRelayConnWhenNoCall(this);
	}

	pthread_rwlock_unlock(&mutex_0);
}

void* User::onLaunched(void *arg) {
#ifndef __ANDROID__
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
#else
	signal(SIGTERM, handle_quit);
#endif
	onLaunchedParam* param = (onLaunchedParam*)arg;
	User* user = param->user;
	uint64_t callId = param->callId;
	mimc::UserInfo fromUser;
	std::string appContent;

	pthread_cleanup_push(cleanup, (void*)(&user->getCallsRwlock()));
	pthread_rwlock_rdlock(&user->getCallsRwlock());
	const P2PCallSession& p2pCallSession = user->getCurrentCalls()->at(callId);
	fromUser = p2pCallSession.getPeerUser();
	appContent = p2pCallSession.getAppContent();
	pthread_rwlock_unlock(&user->getCallsRwlock());
	pthread_cleanup_pop(0);

	LaunchedResponse userResponse = user->getRTSCallEventHandler()->onLaunched(callId, fromUser.appaccount(), appContent, fromUser.resource());

	pthread_cleanup_push(cleanup, (void*)(&user->getCallsRwlock()));
	pthread_rwlock_wrlock(&user->getCallsRwlock());
	if (user->getCurrentCalls()->count(callId) == 0) {
		pthread_rwlock_unlock(&user->getCallsRwlock());
		return 0;
	}
	P2PCallSession& p2pCallSession = user->getCurrentCalls()->at(callId);
	if (!userResponse.isAccepted()) {
		RtsSendSignal::sendInviteResponse(user, callId, p2pCallSession.getCallType(), mimc::PEER_REFUSE, userResponse.getDesc());
		user->getCurrentCalls()->erase(callId);
		RtsSendData::closeRelayConnWhenNoCall(user);
		user->getOnlaunchCalls()->erase(callId);
		pthread_rwlock_unlock(&user->getCallsRwlock());
		return 0;
	}
	RtsSendSignal::sendInviteResponse(user, callId, p2pCallSession.getCallType(), mimc::SUCC, userResponse.getDesc());
	p2pCallSession.setCallState(RUNNING);
	p2pCallSession.setLatestLegalCallStateTs(time(NULL));
	pthread_rwlock_unlock(&user->getCallsRwlock());
	pthread_cleanup_pop(0);

	delete param;
	param = NULL;
	//发送打洞请求

	user->getOnlaunchCalls()->erase(callId);
	return 0;
}

void User::cleanup(void *arg) {
	pthread_rwlock_unlock((pthread_rwlock_t *)arg);
}

void User::createCacheFileIfNotExist(User * user) {
	if (Utils::createDirIfNotExist(user->getCachePath())) {
		std::fstream file;
		file.open(user->getCacheFile().c_str());
		if (file) {
			user->cacheExist = true;
			if (file.is_open()) {
				file.close();
			}
		} else {
			XMDLoggerWrapper::instance()->info("cacheFile is not exist, create now");
			file.open(user->getCacheFile().c_str(), std::ios::out);
			if (file) {
				XMDLoggerWrapper::instance()->info("cacheFile create succeed");
				user->cacheExist = true;
				if (file.is_open()) {
					file.close();
				}
			}
		}
	}
}

bool User::fetchToken(User * user) {
	if (user->tokenFetchSucceed && !user->tokenInvalid) {
		return true;
	}
	const char* str = NULL;
	int str_size = 0;
	if (user->cacheExist) {
		std::ifstream file(user->getCacheFile(), std::ios::in | std::ios::ate);
		long size = file.tellg();
		file.seekg(0, std::ios::beg);
		user->tokenRequestStatus = -1;
		char* localStr = new char[size];
		file.read(localStr, size);
		file.close();
		json_object* pobj_local = NULL;
		if (!user->tokenInvalid && size>0 && user->parseToken(localStr, pobj_local)) {
			json_object_put(pobj_local);
			delete[] localStr;
			return true;
		} else {
			if (user->getTokenFetcher() == NULL) {
				json_object_put(pobj_local);
				delete[] localStr;
				return false;
			}
			user->tokenRequestStatus = 0;
			std::string remoteStr = user->getTokenFetcher()->fetchToken();
			user->tokenRequestStatus = 1;
			json_object* pobj_remote;
			if (user->parseToken(remoteStr.c_str(), pobj_remote)) {
				user->tokenInvalid = false;
				std::ofstream file(user->getCacheFile(), std::ios::out | std::ios::ate);
				if (file.is_open()) {
					if (pobj_local == NULL) {
						str = remoteStr.c_str();
						str_size = remoteStr.size();
					} else {
						json_object* dataobj = json_object_new_object();
						char s[21] = {0};
						json_object_object_add(dataobj, "appId", json_object_new_string(Utils::ltoa(user->getAppId(), s)));
						json_object_object_add(dataobj, "appPackage", json_object_new_string(user->getAppPackage().c_str()));
						json_object_object_add(dataobj, "appAccount", json_object_new_string(user->getAppAccount().c_str()));
						json_object_object_add(dataobj, "miChid", json_object_new_int(user->getChid()));
						json_object_object_add(dataobj, "miUserId", json_object_new_string(Utils::ltoa(user->getUuid(), s)));
						json_object_object_add(dataobj, "miUserSecurityKey", json_object_new_string(user->getSecurityKey().c_str()));
						json_object_object_add(dataobj, "token", json_object_new_string(user->getToken().c_str()));
						json_object_object_add(dataobj, "regionBucket", json_object_new_int(user->getRegionBucket()));
						json_object_object_add(dataobj, "feDomainName", json_object_new_string(user->getFeDomain().c_str()));
						json_object_object_add(dataobj, "relayDomainName", json_object_new_string(user->getRelayDomain().c_str()));
						json_object_object_add(pobj_local, "code", json_object_new_int(200));
						json_object_object_add(pobj_local, "data", dataobj);

						str = json_object_get_string(pobj_local);
						str_size = strlen(str);
					}
					file.write(str, str_size);
					file.close();
				}
				json_object_put(pobj_local);
				json_object_put(pobj_remote);
				delete[] localStr;
				return true;
			} else {
				json_object_put(pobj_local);
				json_object_put(pobj_remote);
				delete[] localStr;
				return false;
			}
		}
	} else {
		if (user->getTokenFetcher() == NULL) {
			return false;
		}
		user->tokenRequestStatus = 0;
		std::string remoteStr = user->getTokenFetcher()->fetchToken();
		user->tokenRequestStatus = 1;
		json_object* pobj_remote;
		if (user->parseToken(remoteStr.c_str(), pobj_remote)) {
			user->tokenInvalid = false;
			createCacheFileIfNotExist(user);
			if (user->cacheExist) {
				std::ofstream file(user->getCacheFile(), std::ios::out | std::ios::ate);
				if (file.is_open()) {
					str = remoteStr.c_str();
					str_size = remoteStr.size();
					file.write(str, str_size);
					file.close();
				}
			}
			json_object_put(pobj_remote);
			return true;
		} else {
			json_object_put(pobj_remote);
			return false;
		}
	}
}

bool User::fetchServerAddr(User* user) {
	pthread_mutex_lock(&user->mutex_1);
	if (user->serverFetchSucceed && !user->addressInvalid) {
		pthread_mutex_unlock(&user->mutex_1);
		return true;
	}
	if (user->feDomain == "" || user->relayDomain == "") {
		XMDLoggerWrapper::instance()->error("User::fetchServerAddr, feDomain or relayDomain is empty");
		pthread_mutex_unlock(&user->mutex_1);
		return false;
	}
	const char* str = NULL;
	int str_size = 0;
	if (user->cacheExist) {
		std::ifstream file(user->getCacheFile(), std::ios::in | std::ios::ate);
		long size = file.tellg();
		file.seekg(0, std::ios::beg);
		char* localStr = new char[size];
		file.read(localStr, size);
		file.close();
		json_object* pobj_local;
		if (user->parseServerAddr(localStr, pobj_local) && !user->addressInvalid) {
			json_object_put(pobj_local);
			pthread_mutex_unlock(&user->mutex_1);
			delete[] localStr;
			return true;
		} else {
			std::string remoteStr = ServerFetcher::fetchServerAddr(RESOLVER_URL, user->feDomain + "," + user->relayDomain);
			json_object* pobj_remote;
			if (user->parseServerAddr(remoteStr.c_str(), pobj_remote)) {
				user->addressInvalid = false;
				std::ofstream file(user->getCacheFile(), std::ios::out | std::ios::ate);
				if (file.is_open()) {
					if (pobj_local == NULL) {
						str = remoteStr.c_str();
						str_size = remoteStr.size();
					} else {
						json_object* feAddrArray = json_object_new_array();
						for (std::vector<std::string>::const_iterator iter = user->feAddresses.begin(); iter != user->feAddresses.end(); iter++) {
							std::string feAddr = *iter;
							json_object_array_add(feAddrArray, json_object_new_string(feAddr.c_str()));
						}
						json_object* relayAddrArray = json_object_new_array();
						for (std::vector<std::string>::const_iterator iter = user->relayAddresses.begin(); iter != user->relayAddresses.end(); iter++) {
							std::string relayAddr = *iter;
							json_object_array_add(relayAddrArray, json_object_new_string(relayAddr.c_str()));
						}

						json_object_object_add(pobj_local, user->feDomain.c_str(), feAddrArray);
						json_object_object_add(pobj_local, user->relayDomain.c_str(), relayAddrArray);

						str = json_object_get_string(pobj_local);
						str_size = strlen(str);
					}
					file.write(str, str_size);
					file.close();
				}
				json_object_put(pobj_local);
				json_object_put(pobj_remote);
				pthread_mutex_unlock(&user->mutex_1);

				delete[] localStr;
				return true;
			} else {
				json_object_put(pobj_local);
				json_object_put(pobj_remote);
				pthread_mutex_unlock(&user->mutex_1);

				delete[] localStr;
				return false;
			}
		}
	} else {
		std::string remoteStr = ServerFetcher::fetchServerAddr(RESOLVER_URL, user->feDomain + "," + user->relayDomain);
		json_object* pobj_remote;
		if (user->parseServerAddr(remoteStr.c_str(), pobj_remote)) {
			user->addressInvalid = false;
			createCacheFileIfNotExist(user);
			if (user->cacheExist) {
				std::ofstream file(user->getCacheFile(), std::ios::out | std::ios::ate);
				if (file.is_open()) {
					str = remoteStr.c_str();
					str_size = remoteStr.size();
					file.write(str, str_size);
					file.close();
				}
			}
			json_object_put(pobj_remote);
			pthread_mutex_unlock(&user->mutex_1);

			return true;
		} else {
			json_object_put(pobj_remote);
			pthread_mutex_unlock(&user->mutex_1);

			return false;
		}
	}
}

bool User::parseToken(const char* str, json_object*& pobj) {
	tokenFetchSucceed = false;
	pobj = json_tokener_parse(str);

	if (pobj == NULL) {
		XMDLoggerWrapper::instance()->info("User::parseToken, json_tokener_parse failed, pobj is NULL, user is %s", appAccount.c_str());
		return tokenFetchSucceed;
	}

	json_object* retobj = NULL;
	json_object_object_get_ex(pobj, "code", &retobj);
	int retCode = json_object_get_int(retobj);

	if (retCode != 200) {
		XMDLoggerWrapper::instance()->info("User::parseToken, parse failed, retCode is %d, user is %s", retCode, appAccount.c_str());
		return tokenFetchSucceed;
	}

	json_object* dataobj = NULL;
	json_object_object_get_ex(pobj, "data", &dataobj);
	json_object* dataitem_obj = NULL;
	const char* pstr = NULL;
	json_object_object_get_ex(dataobj, "appId", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	}
	int64_t appId = atoll(pstr);
	if (appId != this->appId) {
		return tokenFetchSucceed;
	}
	
	json_object_object_get_ex(dataobj, "appAccount", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	}
	std::string appAccount = pstr;
	if (appAccount != this->appAccount) {
		return tokenFetchSucceed;
	}
		
	json_object_object_get_ex(dataobj, "appPackage", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	} else {
		this->appPackage = pstr;
	}
	
	json_object_object_get_ex(dataobj, "miChid", &dataitem_obj);
	this->chid = json_object_get_int(dataitem_obj);
	
	json_object_object_get_ex(dataobj, "miUserId", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	} else {
		this->uuid = atoll(pstr);
	}
	
	json_object_object_get_ex(dataobj, "miUserSecurityKey", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	} else {
		this->securityKey = pstr;
	}
	
	json_object_object_get_ex(dataobj, "token", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	} else {
		this->token = pstr;
	}
	
	json_object_object_get_ex(dataobj, "regionBucket", &dataitem_obj);
	this->regionBucket = json_object_get_int(dataitem_obj);
	
	json_object_object_get_ex(dataobj, "feDomainName", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	} else {
		this->feDomain = pstr;
	}
	
	json_object_object_get_ex(dataobj, "relayDomainName", &dataitem_obj);
	pstr = json_object_get_string(dataitem_obj);
	if (!pstr) {
		return tokenFetchSucceed;
	} else {
		this->relayDomain = pstr;
	}
	
	tokenFetchSucceed = true;

	XMDLoggerWrapper::instance()->info("User::parseToken, parse succeed, user is %s", appAccount.c_str());
	
	return tokenFetchSucceed;
}

bool User::parseServerAddr(const char* str, json_object*& pobj) {
	serverFetchSucceed = false;

	pobj = json_tokener_parse(str);

	if (pobj == NULL) {
		XMDLoggerWrapper::instance()->info("User::parseServerAddr, json_tokener_parse failed, pobj is NULL");
	}

	json_object* dataobj = NULL;
	json_object_object_get_ex(pobj, this->feDomain.c_str(), &dataobj);
	if (dataobj && json_object_get_type(dataobj) == json_type_array) {
		int arraySize = json_object_array_length(dataobj);
		if (arraySize == 0) {
			return serverFetchSucceed;
		}
		this->feAddresses.clear();
		for (int i = 0; i < arraySize; i++) {
			json_object* dataitem_obj = json_object_array_get_idx(dataobj, i);
			const char* feAddress = json_object_get_string(dataitem_obj);
			this->feAddresses.push_back(feAddress);
		}
	} else {
		return serverFetchSucceed;
	}

	json_object_object_get_ex(pobj, this->relayDomain.c_str(), &dataobj);
	if (dataobj && json_object_get_type(dataobj) == json_type_array) {
		int arraySize = json_object_array_length(dataobj);
		if (arraySize == 0) {
			return serverFetchSucceed;
		}
		this->relayAddresses.clear();
		for (int i = 0; i < arraySize; i++) {
			json_object* dataitem_obj = json_object_array_get_idx(dataobj, i);
			const char* relayAddress = json_object_get_string(dataitem_obj);
			this->relayAddresses.push_back(relayAddress);
		}
	} else {
		return serverFetchSucceed;
	}

	serverFetchSucceed = true;

	if (serverFetchSucceed) {
		XMDLoggerWrapper::instance()->info("User::parseServerAddr, serverFetchSucceed is true, user is %s", appAccount.c_str());
	} else {
		XMDLoggerWrapper::instance()->info("User::parseServerAddr, serverFetchSucceed is false, user is %s", appAccount.c_str());
	}

	return serverFetchSucceed;
}

std::string User::join(const std::map<std::string, std::string>& kvs) const {
	if (kvs.empty()) {
		return "";
	}

	std::ostringstream oss;
	for (std::map<std::string, std::string>::const_iterator iter = kvs.begin(); iter != kvs.end(); iter++) {
		const std::string& key = iter->first;
		const std::string& value = iter->second;
		oss << key << ":" << value;
		if (iter != kvs.end()) {
			oss << ",";
		}
	}

	return oss.str();
}

std::string User::sendMessage(const std::string & toAppAccount, const std::string & payload, const std::string & bizType, const bool isStore) {
	if (this->messageHandler == NULL) {
		XMDLoggerWrapper::instance()->error("In sendMessage, messageHandler is not registered!");
		return "";
	}

	if (this->onlineStatus == Offline || toAppAccount == "" || payload == "" || payload.size() > MIMC_MAX_PAYLOAD_SIZE) {
		return "";
	}

	mimc::MIMCUser *from, *to;
	from = new mimc::MIMCUser();
	from->set_appid(this->appId);
	from->set_appaccount(this->appAccount);
	from->set_uuid(this->uuid);
	from->set_resource(this->resource);

	to = new mimc::MIMCUser();
	to->set_appid(this->appId);
	to->set_appaccount(toAppAccount);

	mimc::MIMCP2PMessage * message = new mimc::MIMCP2PMessage();
	message->set_allocated_from(from);
	message->set_allocated_to(to);
	message->set_payload(std::string(payload));
	message->set_biztype(bizType);
	message->set_isstore(isStore);

	int message_size = message->ByteSize();
	char * messageBytes = new char[message_size];
	memset(messageBytes, 0, message_size);
	message->SerializeToArray(messageBytes, message_size);
	std::string messageBytesStr(messageBytes, message_size);

	delete message;
	message = NULL;

	std::string packetId = this->packetManager->createPacketId();
	mimc::MIMCPacket * packet = new mimc::MIMCPacket();
	packet->set_packetid(packetId);
	packet->set_package(this->appPackage);
	packet->set_type(mimc::P2P_MESSAGE);
	packet->set_payload(messageBytesStr);
	packet->set_timestamp(time(NULL));

	delete[] messageBytes;
	messageBytes = NULL;

	MIMCMessage mimcMessage(packetId, packet->sequence(), this->appAccount, this->resource, toAppAccount, "", payload, bizType, packet->timestamp());
	(this->packetManager->packetsWaitToTimeout).insert(std::pair<std::string, MIMCMessage>(packetId, mimcMessage));

	struct waitToSendContent mimc_obj;
	mimc_obj.cmd = BODY_CLIENTHEADER_CMD_SECMSG;
	mimc_obj.type = C2S_DOUBLE_DIRECTION;
	mimc_obj.message = packet;
	(this->packetManager->packetsWaitToSend).push(mimc_obj);

	return packetId;
}

std::string User::sendGroupMessage(const int64_t topicId, const std::string & payload, const std::string & bizType, const bool isStore) {
	if (this->messageHandler == NULL) {
		XMDLoggerWrapper::instance()->error("In sendGroupMessage, messageHandler is not registered!");
		return "";
	}

	if (this->onlineStatus == Offline || payload == "" || payload.size() > MIMC_MAX_PAYLOAD_SIZE) {
		return "";
	}

	mimc::MIMCUser *from;
	from = new mimc::MIMCUser();
	from->set_appid(this->appId);
	from->set_appaccount(this->appAccount);
	from->set_uuid(this->uuid);
	from->set_resource(this->resource);

	mimc::MIMCGroup* to = new mimc::MIMCGroup();
	to->set_appid(this->appId);
	to->set_topicid(topicId);

	mimc::MIMCP2TMessage* message = new mimc::MIMCP2TMessage();
	message->set_allocated_from(from);
	message->set_allocated_to(to);
	message->set_payload(std::string(payload));
	message->set_isstore(isStore);
	message->set_biztype(bizType);

	int message_size = message->ByteSize();
	char * messageBytes = new char[message_size];
	memset(messageBytes, 0, message_size);
	message->SerializeToArray(messageBytes, message_size);
	std::string messageBytesStr(messageBytes, message_size);

	delete message;
	message = NULL;

	std::string packetId = this->packetManager->createPacketId();
	mimc::MIMCPacket * packet = new mimc::MIMCPacket();
	packet->set_packetid(packetId);
	packet->set_package(this->appPackage);
	packet->set_type(mimc::P2T_MESSAGE);
	packet->set_payload(messageBytesStr);
	packet->set_timestamp(time(NULL));

	delete[] messageBytes;
	messageBytes = NULL;

	MIMCGroupMessage mimcGroupMessage(packetId, packet->sequence(), this->appAccount, this->resource,topicId, payload, bizType, packet->timestamp());
	(this->packetManager->groupPacketWaitToTimeout).insert(std::pair<std::string, MIMCGroupMessage>(packetId, mimcGroupMessage));

	struct waitToSendContent mimc_obj;
	mimc_obj.cmd = BODY_CLIENTHEADER_CMD_SECMSG;
	mimc_obj.type = C2S_DOUBLE_DIRECTION;
	mimc_obj.message = packet;
	(this->packetManager->packetsWaitToSend).push(mimc_obj);

	return packetId;
}


uint64_t User::dialCall(const std::string & toAppAccount, const std::string & appContent, const std::string & toResource) {
	if (this->rtsCallEventHandler == NULL) {
		XMDLoggerWrapper::instance()->error("In dialCall, rtsCallEventHandler is not registered!");
		return 0;
	}
	if (toAppAccount == "") {
		XMDLoggerWrapper::instance()->error("In dialCall, toAppAccount can not be empty!");
		return 0;
	}
	if (this->getOnlineStatus() == Offline) {
		XMDLoggerWrapper::instance()->warn("In dialCall, user:%lld is offline!", uuid);
		return 0;
	}

	pthread_rwlock_wrlock(&mutex_0);
	if (currentCalls->size() == maxCallNum) {
		XMDLoggerWrapper::instance()->warn("In dialCall, currentCalls size has reached maxCallNum %d!", maxCallNum);
		pthread_rwlock_unlock(&mutex_0);
		return 0;
	}

	this->checkToRunXmdTranseiver();

	mimc::UserInfo toUser;
	toUser.set_appid(this->appId);
	toUser.set_appaccount(toAppAccount);
	if (toResource != "") {
		toUser.set_resource(toResource);
	}

	//判断是否是同一个call
	for (std::map<uint64_t, P2PCallSession>::const_iterator iter = currentCalls->begin(); iter != currentCalls->end(); iter++) {
		const P2PCallSession& callSession = iter->second;
		const mimc::UserInfo& session_peerUser = callSession.getPeerUser();
		if (toUser.appid() == session_peerUser.appid() && toUser.appaccount() == session_peerUser.appaccount() && toUser.resource() == session_peerUser.resource()
		        && appContent == callSession.getAppContent()) {
			XMDLoggerWrapper::instance()->warn("In dialCall, the call has connected!");
			pthread_rwlock_unlock(&mutex_0);
			return 0;
		}
	}

	uint64_t callId;
	do {
		callId = Utils::generateRandomLong();
	} while (currentCalls->count(callId) > 0 || callId == 0);

	XMDLoggerWrapper::instance()->info("In dialCall, callId is %llu", callId);
	if (this->relayLinkState == NOT_CREATED) {

		uint64_t relayConnId = RtsSendData::createRelayConn(this);
		XMDLoggerWrapper::instance()->info("In dialCall, relayConnId is %llu", relayConnId);
		if (relayConnId == 0) {
			XMDLoggerWrapper::instance()->error("In dialCall, launch create relay conn failed!");

			pthread_rwlock_unlock(&mutex_0);
			return 0;

		}
		currentCalls->insert(std::pair<uint64_t, P2PCallSession>(callId, P2PCallSession(callId, toUser, mimc::SINGLE_CALL, WAIT_SEND_CREATE_REQUEST, time(NULL), true, appContent)));
		pthread_rwlock_unlock(&mutex_0);
		return callId;
	} else if (this->relayLinkState == BEING_CREATED) {

		currentCalls->insert(std::pair<uint64_t, P2PCallSession>(callId, P2PCallSession(callId, toUser, mimc::SINGLE_CALL, WAIT_SEND_CREATE_REQUEST, time(NULL), true, appContent)));
		pthread_rwlock_unlock(&mutex_0);
		return callId;
	} else if (this->relayLinkState == SUCC_CREATED) {

		currentCalls->insert(std::pair<uint64_t, P2PCallSession>(callId, P2PCallSession(callId, toUser, mimc::SINGLE_CALL, WAIT_CREATE_RESPONSE, time(NULL), true, appContent)));
		RtsSendSignal::sendCreateRequest(this, callId);
		pthread_rwlock_unlock(&mutex_0);
		return callId;
	} else {
		pthread_rwlock_unlock(&mutex_0);
		return 0;

	}
}

int User::sendRtsData(uint64_t callId, const std::string & data, const RtsDataType dataType, const RtsChannelType channelType, const std::string& ctx, const bool canBeDropped, const DataPriority priority, const unsigned int resendCount) {
	int dataId = -1;
	if (data.size() > RTS_MAX_PAYLOAD_SIZE) {
		return dataId;
	}
	if (dataType != AUDIO && dataType != VIDEO && dataType != FILEDATA) {
		return dataId;
	}

	mimc::PKT_TYPE pktType = mimc::USER_DATA_AUDIO;
	if (dataType == VIDEO) {
		pktType = mimc::USER_DATA_VIDEO;
	} else if (dataType == FILEDATA) {
		pktType = mimc::USER_DATA_FILE;
	}

	pthread_rwlock_rdlock(&mutex_0);
	if (currentCalls->count(callId) == 0) {
		pthread_rwlock_unlock(&mutex_0);
		return dataId;
	}

	const P2PCallSession& callSession = currentCalls->at(callId);
	if (callSession.getCallState() != RUNNING) {
		pthread_rwlock_unlock(&mutex_0);
		return dataId;
	}
	if (onlineStatus == Offline) {
		pthread_rwlock_unlock(&mutex_0);
		return dataId;
	}

	RtsContext* rtsContext = new RtsContext(callId, ctx);

	if (channelType == RELAY) {
		dataId = RtsSendData::sendRtsDataByRelay(this, callId, data, pktType, (void *)rtsContext, canBeDropped, priority, resendCount);
	}
	pthread_rwlock_unlock(&mutex_0);
	return dataId;
}

void User::closeCall(uint64_t callId, std::string byeReason) {
	pthread_rwlock_wrlock(&mutex_0);
	if (currentCalls->count(callId) == 0) {
		pthread_rwlock_unlock(&mutex_0);
		return;
	}

	XMDLoggerWrapper::instance()->info("In closeCall, callId is %llu, byeReason is %s", callId, byeReason.c_str());
	RtsSendSignal::sendByeRequest(this, callId, byeReason);
	if (onlaunchCalls->count(callId) > 0) {
		pthread_t onlaunchCallThread = onlaunchCalls->at(callId);
	#ifndef __ANDROID__
		pthread_cancel(onlaunchCallThread);
	#else
		if(pthread_kill(onlaunchCallThread, 0) != ESRCH)
    	{
        	pthread_kill(onlaunchCallThread, SIGTERM);
    	}
    #endif
		onlaunchCalls->erase(callId);
	}
	currentCalls->erase(callId);
	RtsSendData::closeRelayConnWhenNoCall(this);
	rtsCallEventHandler->onClosed(callId, "CLOSED_INITIATIVELY");
	pthread_rwlock_unlock(&mutex_0);
}

bool User::login() {
	if (onlineStatus == Online) {
		return true;
	}
	if (tokenFetcher == NULL || statusHandler == NULL) {
		XMDLoggerWrapper::instance()->warn("login failed, user must register tokenFetcher and statusHandler first!");
		return false;
	}
	if (messageHandler == NULL && rtsCallEventHandler == NULL) {
		XMDLoggerWrapper::instance()->warn("login failed, user must register messageHandler or rtsCallEventHandler first!");
		return false;
	}
	permitLogin = true;
	return true;
}

bool User::logout() {
	if (onlineStatus == Offline) {
		return true;
	}

	pthread_rwlock_wrlock(&mutex_0);
	for (std::map<uint64_t, P2PCallSession>::const_iterator iter = currentCalls->begin(); iter != currentCalls->end(); iter++) {
		uint64_t callId = iter->first;
		const P2PCallSession& callSession = iter->second;
		if (onlaunchCalls->count(callId) > 0) {
			pthread_t onlaunchCallThread = onlaunchCalls->at(callId);
		#ifndef __ANDROID__
			pthread_cancel(onlaunchCallThread);
		#else
			if(pthread_kill(onlaunchCallThread, 0) != ESRCH)
    		{
        		pthread_kill(onlaunchCallThread, SIGTERM);
    		}
    	#endif
			onlaunchCalls->erase(callId);
		}
		if (callSession.getCallState() >= RUNNING) {
			RtsSendSignal::sendByeRequest(this, callId, "CLIENT LOGOUT");
			rtsCallEventHandler->onClosed(callId, "CLIENT LOGOUT");
		}
	}

	struct waitToSendContent logout_obj;
	logout_obj.cmd = BODY_CLIENTHEADER_CMD_UNBIND;
	logout_obj.type = C2S_DOUBLE_DIRECTION;
	logout_obj.message = NULL;
	packetManager->packetsWaitToSend.push(logout_obj);

	currentCalls->clear();
	RtsSendData::closeRelayConnWhenNoCall(this);

	permitLogin = false;

	pthread_rwlock_unlock(&mutex_0);
	return true;
}

void User::resetRelayLinkState() {
	XMDLoggerWrapper::instance()->info("In resetRelayLinkState, relayConnId to be reset is %llu", this->relayConnId);
	this->relayConnId = 0;
	this->relayControlStreamId = 0;
	this->relayAudioStreamId = 0;
	this->relayVideoStreamId = 0;
	this->relayFileStreamId = 0;
	this->relayLinkState = NOT_CREATED;
	this->latestLegalRelayLinkStateTs = time(NULL);
	delete this->bindRelayResponse;
	this->bindRelayResponse = NULL;
}

void User::handleXMDConnClosed(uint64_t connId, ConnCloseType type) {
	if (connId == this->relayConnId) {
		XMDLoggerWrapper::instance()->warn("XMDConnection(RELAY) is closed abnormally, connId is %llu, ConnCloseType is %d", connId, type);
		resetRelayLinkState();
	} else {
		pthread_rwlock_rdlock(&mutex_0);
		for (std::map<uint64_t, P2PCallSession>::iterator iter = currentCalls->begin(); iter != currentCalls->end(); iter++) {
			uint64_t callId = iter->first;
			P2PCallSession& callSession = iter->second;
			if (connId == callSession.getP2PIntranetConnId()) {
				XMDLoggerWrapper::instance()->warn("XMDConnection(P2PIntranet) is closed abnormally, connId is %llu, ConnCloseType is %d, callId is %llu", connId, type, callId);
				callSession.resetP2PIntranetConn();
			} else if (connId == callSession.getP2PInternetConnId()) {
				XMDLoggerWrapper::instance()->warn("XMDConnection(P2PInternet) is closed abnormally, connId is %llu, ConnCloseType is %d, callId is %llu", connId, type, callId);
				callSession.resetP2PInternetConn();
			}
		}
		pthread_rwlock_unlock(&mutex_0);
	}

	checkAndCloseCalls();
}

void User::checkAndCloseCalls() {
	if (this->relayConnId != 0) {
		return;
	}

	pthread_rwlock_wrlock(&mutex_0);
	for (std::map<uint64_t, P2PCallSession>::iterator iter = currentCalls->begin(); iter != currentCalls->end();) {
		uint64_t callId = iter->first;
		const P2PCallSession& callSession = currentCalls->at(callId);
		if (callSession.getP2PIntranetConnId() == 0 && callSession.getP2PInternetConnId() == 0) {
			if (onlaunchCalls->count(callId) > 0) {
				pthread_t onlaunchCallThread = onlaunchCalls->at(callId);
			#ifndef __ANDROID__
				pthread_cancel(onlaunchCallThread);
			#else
				if(pthread_kill(onlaunchCallThread, 0) != ESRCH)
    			{
        			pthread_kill(onlaunchCallThread, SIGTERM);
    			}
    		#endif
				onlaunchCalls->erase(callId);
			}
			if (callSession.getCallState() >= RUNNING) {
				RtsSendSignal::sendByeRequest(this, callId, ALL_DATA_CHANNELS_CLOSED);
				rtsCallEventHandler->onClosed(callId, ALL_DATA_CHANNELS_CLOSED);
			}
			currentCalls->erase(iter++);
		} else {
			iter++;
		}
	}
	pthread_rwlock_unlock(&mutex_0);
}

uint64_t User::getP2PIntranetConnId(uint64_t callId) {
	pthread_rwlock_rdlock(&mutex_0);
	if (currentCalls->count(callId) == 0) {
		pthread_rwlock_unlock(&mutex_0);
		return 0;
	}
	const P2PCallSession& callSession = currentCalls->at(callId);
	pthread_rwlock_unlock(&mutex_0);
	return callSession.getP2PIntranetConnId();
}

uint64_t User::getP2PInternetConnId(uint64_t callId) {
	pthread_rwlock_rdlock(&mutex_0);
	if (currentCalls->count(callId) == 0) {
		pthread_rwlock_unlock(&mutex_0);
		return 0;
	}
	const P2PCallSession& callSession = currentCalls->at(callId);
	pthread_rwlock_unlock(&mutex_0);
	return callSession.getP2PInternetConnId();
}

void User::checkToRunXmdTranseiver() {
	if (!this->xmdTranseiver) {
		this->xmdTranseiver = new XMDTransceiver(1);
		this->xmdTranseiver->start();
		this->rtsConnectionHandler = new RtsConnectionHandler(this);
		this->rtsStreamHandler = new RtsStreamHandler(this);
		this->xmdTranseiver->registerConnHandler(this->rtsConnectionHandler);
		this->xmdTranseiver->registerStreamHandler(this->rtsStreamHandler);
		this->xmdTranseiver->SetAckPacketResendIntervalMicroSecond(500);
		this->xmdTranseiver->setTestPacketLoss(this->testPacketLoss);
		if (this->xmdSendBufferSize > 0) {
			this->xmdTranseiver->setSendBufferSize(this->xmdSendBufferSize);
		}
		if (this->xmdRecvBufferSize > 0) {
			this->xmdTranseiver->setRecvBufferSize(this->xmdRecvBufferSize);
		}
		this->xmdTranseiver->run();
		//usleep(100000);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void User::initAudioStreamConfig(const RtsStreamConfig& audioStreamConfig) {
	this->audioStreamConfig = audioStreamConfig;
}

void User::initVideoStreamConfig(const RtsStreamConfig& videoStreamConfig) {
	this->videoStreamConfig = videoStreamConfig;
}

void User::setSendBufferSize(int size) {
	if (size > 0) {
		if(this->xmdTranseiver) {
			this->xmdTranseiver->setSendBufferSize(size);
		} else {
			this->xmdSendBufferSize = size;
		}
	}
}

void User::setRecvBufferSize(int size) {
	if (size > 0) {
		if(this->xmdTranseiver) {
			this->xmdTranseiver->setRecvBufferSize(size);
		} else {
			this->xmdRecvBufferSize = size;
		}
	}
}

int User::getSendBufferSize() {
	return this->xmdTranseiver ? this->xmdTranseiver->getSendBufferSize() : 0;
}

int User::getRecvBufferSize() {
	return this->xmdTranseiver ? this->xmdTranseiver->getRecvBufferSize() : 0;
}

float User::getSendBufferUsageRate() {
	return this->xmdTranseiver ? this->xmdTranseiver->getSendBufferUsageRate() : 0;
}

float User::getRecvBufferUsageRate() {
	return this->xmdTranseiver ? this->xmdTranseiver->getRecvBufferUsageRate() : 0;
}

void User::clearSendBuffer() {
	if(this->xmdTranseiver) {
		this->xmdTranseiver->clearSendBuffer();
	}
}

void User::clearRecvBuffer() {
	if(this->xmdTranseiver) {
		this->xmdTranseiver->clearRecvBuffer();
	}
}

void User::setTestPacketLoss(int testPacketLoss) {
	this->testPacketLoss = testPacketLoss;
	if(this->xmdTranseiver) {
		this->xmdTranseiver->setTestPacketLoss(testPacketLoss);
	}
}
