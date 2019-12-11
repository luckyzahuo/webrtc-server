﻿/*
 * MIT License
 *
 * Copyright (c) 2016-2019 xiongziliang <771730766@qq.com>
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "RtmpPusher.h"
#include "Rtmp/utils.h"
#include "Util/util.h"
#include "Util/onceToken.h"
#include "Thread/ThreadPool.h"
using namespace toolkit;
using namespace mediakit::Client;

namespace mediakit {

static int kSockFlags = SOCKET_DEFAULE_FLAGS | FLAG_MORE;

RtmpPusher::RtmpPusher(const EventPoller::Ptr &poller,const RtmpMediaSource::Ptr &src) : TcpClient(poller){
	_pMediaSrc=src;
}

RtmpPusher::~RtmpPusher() {
	teardown();
	DebugL << endl;
}
void RtmpPusher::teardown() {
	if (alive()) {
		_strApp.clear();
		_strStream.clear();
		_strTcUrl.clear();
		{
			lock_guard<recursive_mutex> lck(_mtxOnResultCB);
			_mapOnResultCB.clear();
		}
        {
            lock_guard<recursive_mutex> lck(_mtxOnStatusCB);
            _dqOnStatusCB.clear();
        }
		_pPublishTimer.reset();
        reset();
        shutdown(SockException(Err_shutdown,"teardown"));
	}
}

void RtmpPusher::onPublishResult(const SockException &ex) {
	if(_pPublishTimer){
		//播放结果回调
		_pPublishTimer.reset();
		if(_onPublished){
			_onPublished(ex);
		}
	} else {
		//播放成功后异常断开回调
		if(_onShutdown){
			_onShutdown(ex);
		}
	}

	if(ex){
		teardown();
	}
}

void RtmpPusher::publish(const string &strUrl)  {
	teardown();
	string strHost = FindField(strUrl.data(), "://", "/");
	_strApp = 	FindField(strUrl.data(), (strHost + "/").data(), "/");
    _strStream = FindField(strUrl.data(), (strHost + "/" + _strApp + "/").data(), NULL);
    _strTcUrl = string("rtmp://") + strHost + "/" + _strApp;

    if (!_strApp.size() || !_strStream.size()) {
        onPublishResult(SockException(Err_other,"rtmp url非法"));
        return;
    }
	DebugL << strHost << " " << _strApp << " " << _strStream;

	auto iPort = atoi(FindField(strHost.data(), ":", NULL).data());
	if (iPort <= 0) {
        //rtmp 默认端口1935
		iPort = 1935;
	} else {
        //服务器域名
		strHost = FindField(strHost.data(), NULL, ":");
	}

    weak_ptr<RtmpPusher> weakSelf = dynamic_pointer_cast<RtmpPusher>(shared_from_this());
    float playTimeOutSec = (*this)[kTimeoutMS].as<int>() / 1000.0;
    _pPublishTimer.reset( new Timer(playTimeOutSec,  [weakSelf]() {
        auto strongSelf=weakSelf.lock();
        if(!strongSelf) {
            return false;
        }
        strongSelf->onPublishResult(SockException(Err_timeout,"publish rtmp timeout"));
        return false;
    },getPoller()));

    if(!(*this)[kNetAdapter].empty()){
        setNetAdapter((*this)[kNetAdapter]);
    }

	startConnect(strHost, iPort);
}

void RtmpPusher::onErr(const SockException &ex){
	onPublishResult(ex);
}
void RtmpPusher::onConnect(const SockException &err){
	if(err) {
		onPublishResult(err);
		return;
	}
	//推流器不需要多大的接收缓存，节省内存占用
	_sock->setReadBuffer(std::make_shared<BufferRaw>(1 * 1024));

	weak_ptr<RtmpPusher> weakSelf = dynamic_pointer_cast<RtmpPusher>(shared_from_this());
	startClientSession([weakSelf](){
        auto strongSelf=weakSelf.lock();
        if(!strongSelf) {
            return;
        }

        strongSelf->sendChunkSize(60000);
        strongSelf->send_connect();
	});
}
void RtmpPusher::onRecv(const Buffer::Ptr &pBuf){
	try {
		onParseRtmp(pBuf->data(), pBuf->size());
	} catch (exception &e) {
		SockException ex(Err_other, e.what());
		onPublishResult(ex);
	}
}


inline void RtmpPusher::send_connect() {
	AMFValue obj(AMF_OBJECT);
	obj.set("app", _strApp);
	obj.set("type", "nonprivate");
	obj.set("tcUrl", _strTcUrl);
	obj.set("swfUrl", _strTcUrl);
	sendInvoke("connect", obj);
	addOnResultCB([this](AMFDecoder &dec){
		//TraceL << "connect result";
		dec.load<AMFValue>();
		auto val = dec.load<AMFValue>();
		auto level = val["level"].as_string();
		auto code = val["code"].as_string();
		if(level != "status"){
			throw std::runtime_error(StrPrinter <<"connect 失败:" << level << " " << code << endl);
		}
		send_createStream();
	});
}

inline void RtmpPusher::send_createStream() {
	AMFValue obj(AMF_NULL);
	sendInvoke("createStream", obj);
	addOnResultCB([this](AMFDecoder &dec){
		//TraceL << "createStream result";
		dec.load<AMFValue>();
		_ui32StreamId = dec.load<int>();
		send_publish();
	});
}
inline void RtmpPusher::send_publish() {
	AMFEncoder enc;
	enc << "publish" << ++_iReqID << nullptr << _strStream << _strApp ;
	sendRequest(MSG_CMD, enc.data());

	addOnStatusCB([this](AMFValue &val) {
		auto level = val["level"].as_string();
		auto code = val["code"].as_string();
		if(level != "status") {
			throw std::runtime_error(StrPrinter <<"publish 失败:" << level << " " << code << endl);
		}
		//start send media
		send_metaData();
	});
}

inline void RtmpPusher::send_metaData(){
    auto src = _pMediaSrc.lock();
    if (!src) {
        throw std::runtime_error("the media source was released");
    }

    AMFEncoder enc;
    enc << "@setDataFrame" << "onMetaData" <<  src->getMetaData();
    sendRequest(MSG_DATA, enc.data());
    
    src->getConfigFrame([&](const RtmpPacket::Ptr &pkt){
        sendRtmp(pkt->typeId, _ui32StreamId, pkt, pkt->timeStamp, pkt->chunkId );
    });
    
    _pRtmpReader = src->getRing()->attach(getPoller());
    weak_ptr<RtmpPusher> weakSelf = dynamic_pointer_cast<RtmpPusher>(shared_from_this());
    _pRtmpReader->setReadCB([weakSelf](const RtmpPacket::Ptr &pkt){
    	auto strongSelf = weakSelf.lock();
    	if(!strongSelf) {
    		return;
    	}
    	strongSelf->sendRtmp(pkt->typeId, strongSelf->_ui32StreamId, pkt, pkt->timeStamp, pkt->chunkId);
    });
    _pRtmpReader->setDetachCB([weakSelf](){
        auto strongSelf = weakSelf.lock();
        if(strongSelf){
            strongSelf->onPublishResult(SockException(Err_other,"媒体源被释放"));
        }
    });
    onPublishResult(SockException(Err_success,"success"));
	//提高发送性能
	(*this) << SocketFlags(kSockFlags);
	SockUtil::setNoDelay(_sock->rawFD(),false);
}
void RtmpPusher::onCmd_result(AMFDecoder &dec){
	auto iReqId = dec.load<int>();
	lock_guard<recursive_mutex> lck(_mtxOnResultCB);
	auto it = _mapOnResultCB.find(iReqId);
	if(it != _mapOnResultCB.end()){
		it->second(dec);
		_mapOnResultCB.erase(it);
	}else{
		WarnL << "unhandled _result";
	}
}
void RtmpPusher::onCmd_onStatus(AMFDecoder &dec) {
	AMFValue val;
	while(true){
		val = dec.load<AMFValue>();
		if(val.type() == AMF_OBJECT){
			break;
		}
	}
	if(val.type() != AMF_OBJECT){
		throw std::runtime_error("onStatus:the result object was not found");
	}

    lock_guard<recursive_mutex> lck(_mtxOnStatusCB);
	if(_dqOnStatusCB.size()){
		_dqOnStatusCB.front()(val);
		_dqOnStatusCB.pop_front();
	}else{
		auto level = val["level"];
		auto code = val["code"].as_string();
		if(level.type() == AMF_STRING){
			if(level.as_string() != "status"){
				throw std::runtime_error(StrPrinter <<"onStatus 失败:" << level.as_string() << " " << code << endl);
			}
		}
    }
}

void RtmpPusher::onRtmpChunk(RtmpPacket &chunkData) {
	switch (chunkData.typeId) {
		case MSG_CMD:
		case MSG_CMD3: {
			typedef void (RtmpPusher::*rtmpCMDHandle)(AMFDecoder &dec);
			static unordered_map<string, rtmpCMDHandle> g_mapCmd;
			static onceToken token([]() {
				g_mapCmd.emplace("_error",&RtmpPusher::onCmd_result);
				g_mapCmd.emplace("_result",&RtmpPusher::onCmd_result);
				g_mapCmd.emplace("onStatus",&RtmpPusher::onCmd_onStatus);
			}, []() {});

			AMFDecoder dec(chunkData.strBuf, 0);
			std::string type = dec.load<std::string>();
			auto it = g_mapCmd.find(type);
			if(it != g_mapCmd.end()){
				auto fun = it->second;
				(this->*fun)(dec);
			}else{
				WarnL << "can not support cmd:" << type;
			}
		}
			break;
		default:
			//WarnL << "unhandled message:" << (int) chunkData.typeId << hexdump(chunkData.strBuf.data(), chunkData.strBuf.size());
			break;
		}
}


} /* namespace mediakit */

