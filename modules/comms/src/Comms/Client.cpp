/*+-------------------------------------------------------------------------+
  |                       MultiVehicle simulator (libmvsim)                 |
  |                                                                         |
  | Copyright (C) 2014-2020  Jose Luis Blanco Claraco                       |
  | Copyright (C) 2017  Borys Tymchenko (Odessa Polytechnic University)     |
  | Distributed under 3-clause BSD License                                  |
  |   See COPYING                                                           |
  +-------------------------------------------------------------------------+ */

#include <mrpt/core/exceptions.h>
#include <mrpt/version.h>
#include <mvsim/Comms/Client.h>
#include <mvsim/Comms/common.h>
#include <mvsim/Comms/ports.h>
#include <mvsim/Comms/zmq_monitor.h>
#if MRPT_VERSION >= 0x204
#include <mrpt/system/thread_name.h>
#endif

#include <iostream>
#include <mutex>
#include <shared_mutex>

#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)

#include <google/protobuf/text_format.h>

#include <zmq.hpp>

#include "AdvertiseServiceRequest.pb.h"
#include "AdvertiseTopicRequest.pb.h"
#include "CallService.pb.h"
#include "GenericAnswer.pb.h"
#include "GetServiceInfoAnswer.pb.h"
#include "GetServiceInfoRequest.pb.h"
#include "ListNodesAnswer.pb.h"
#include "ListNodesRequest.pb.h"
#include "ListTopicsAnswer.pb.h"
#include "ListTopicsRequest.pb.h"
#include "RegisterNodeAnswer.pb.h"
#include "RegisterNodeRequest.pb.h"
#include "UnregisterNodeRequest.pb.h"

#endif

using namespace mvsim;

#if defined(MVSIM_HAS_ZMQ)
struct InfoPerAdvertisedTopic
{
	InfoPerAdvertisedTopic(zmq::context_t& c) : context(c) {}

	zmq::context_t& context;

	std::string topicName;
	zmq::socket_t pubSocket = zmq::socket_t(context, ZMQ_PUB);
	std::string endpoint;
	const google::protobuf::Descriptor* descriptor = nullptr;
};

struct InfoPerService
{
	InfoPerService() = default;

	std::string serviceName;
	const google::protobuf::Descriptor* descInput = nullptr;
	const google::protobuf::Descriptor* descOutput = nullptr;
	Client::service_callback_t callback;
};
#endif

struct Client::ZMQImpl
{
#if defined(MVSIM_HAS_ZMQ)
	zmq::context_t context{1, ZMQ_MAX_SOCKETS_DFLT};
	std::optional<zmq::socket_t> mainReqSocket;
	mvsim::SocketMonitor mainReqSocketMonitor;

	std::map<std::string, InfoPerAdvertisedTopic> advertisedTopics;
	std::shared_mutex advertisedTopics_mtx;

	std::optional<zmq::socket_t> srvListenSocket;
	std::map<std::string, InfoPerService> offeredServices;
	std::shared_mutex offeredServices_mtx;
#endif
};

Client::Client()
	: mrpt::system::COutputLogger("mvsim::Client"),
	  zmq_(std::make_unique<Client::ZMQImpl>())
{
}
Client::Client(const std::string& nodeName) : Client() { setName(nodeName); }

Client::~Client() { shutdown(); }

void Client::setName(const std::string& nodeName) { nodeName_ = nodeName; }

bool Client::connected() const
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)
	return zmq_->mainReqSocketMonitor.connected();
#else
	return false;
#endif
}

void Client::connect()
{
	using namespace std::string_literals;
	ASSERTMSG_(
		!zmq_->mainReqSocket || !zmq_->mainReqSocket->connected(),
		"Client is already running.");

#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)

	zmq_->mainReqSocket.emplace(zmq_->context, ZMQ_REQ);

	// Monitor to listen on ZMQ socket events:
	zmq_->mainReqSocketMonitor.monitor(zmq_->mainReqSocket.value());

	zmq_->mainReqSocket->connect(
		"tcp://"s + serverHostAddress_ + ":"s +
		std::to_string(MVSIM_PORTNO_MAIN_REP));

	// Let the server know about this new node:
	doRegisterClient();

	// Create listening socket for services:
	zmq_->srvListenSocket.emplace(zmq_->context, ZMQ_REP);
	zmq_->srvListenSocket->bind("tcp://0.0.0.0:*"s);

	if (!zmq_->srvListenSocket->connected())
		THROW_EXCEPTION("Error binding service listening socket.");

	ASSERTMSG_(
		!serviceInvokerThread_.joinable(),
		"Client service thread is already running!");

	serviceInvokerThread_ =
		std::thread(&Client::internalServiceServingThread, this);

#if MRPT_VERSION >= 0x204
	mrpt::system::thread_name("services_"s + nodeName_, serviceInvokerThread_);
#endif

#else
	THROW_EXCEPTION(
		"MVSIM needs building with ZMQ and PROTOBUF to enable "
		"client/server");
#endif
}

void Client::shutdown() noexcept
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)

	if (!zmq_->mainReqSocket->connected()) return;

	try
	{
		MRPT_LOG_DEBUG_STREAM("Unregistering from server.");
		doUnregisterClient();
	}
	catch (const std::exception& e)
	{
		MRPT_LOG_ERROR_STREAM(
			"shutdown: Exception: " << mrpt::exception_to_str(e));
	}

#if ZMQ_VERSION >= ZMQ_MAKE_VERSION(4, 4, 0)
	zmq_->context.shutdown();
#else
	// Missing shutdown() in older versions:
	zmq_ctx_shutdown(zmq_->context.operator void*());
#endif

	if (serviceInvokerThread_.joinable()) serviceInvokerThread_.join();

#endif
}

void Client::doRegisterClient()
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)
	auto& s = *zmq_->mainReqSocket;

	mvsim_msgs::RegisterNodeRequest rnq;
	rnq.set_nodename(nodeName_);
	mvsim::sendMessage(rnq, s);

	//  Get the reply.
	const zmq::message_t reply = mvsim::receiveMessage(s);

	mvsim_msgs::RegisterNodeAnswer rna;
	mvsim::parseMessage(reply, rna);
	if (!rna.success())
	{
		THROW_EXCEPTION_FMT(
			"Server did not allow registering node: %s",
			rna.errormessage().c_str());
	}
	MRPT_LOG_DEBUG("Successfully registered in the server.");
#else
	THROW_EXCEPTION("MVSIM built without ZMQ");
#endif
}

void Client::doUnregisterClient()
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)
	auto& s = *zmq_->mainReqSocket;

	mvsim_msgs::UnregisterNodeRequest rnq;
	rnq.set_nodename(nodeName_);
	mvsim::sendMessage(rnq, s);

	//  Get the reply.
	const zmq::message_t reply = mvsim::receiveMessage(s);

	mvsim_msgs::GenericAnswer rna;
	mvsim::parseMessage(reply, rna);
	if (!rna.success())
	{
		THROW_EXCEPTION_FMT(
			"Server answered an error unregistering node: %s",
			rna.errormessage().c_str());
	}
	MRPT_LOG_DEBUG("Successfully unregistered in the server.");
#else
	THROW_EXCEPTION("MVSIM built without ZMQ");
#endif
}

std::vector<Client::InfoPerNode> Client::requestListOfNodes()
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)
	auto& s = *zmq_->mainReqSocket;

	mvsim_msgs::ListNodesRequest req;
	mvsim::sendMessage(req, s);

	//  Get the reply.
	const zmq::message_t reply = mvsim::receiveMessage(s);

	mvsim_msgs::ListNodesAnswer lna;
	mvsim::parseMessage(reply, lna);

	std::vector<Client::InfoPerNode> nodes;
	nodes.resize(lna.nodes_size());

	for (int i = 0; i < lna.nodes_size(); i++)
	{
		nodes[i].name = lna.nodes(i);
	}
	return nodes;
#else
	THROW_EXCEPTION("MVSIM built without ZMQ");
#endif
}

std::vector<Client::InfoPerTopic> Client::requestListOfTopics()
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)
	auto& s = *zmq_->mainReqSocket;

	mvsim_msgs::ListTopicsRequest req;
	mvsim::sendMessage(req, s);

	//  Get the reply.
	const zmq::message_t reply = mvsim::receiveMessage(s);

	mvsim_msgs::ListTopicsAnswer lta;
	mvsim::parseMessage(reply, lta);

	std::vector<Client::InfoPerTopic> topics;
	topics.resize(lta.topics_size());

	for (int i = 0; i < lta.topics_size(); i++)
	{
		const auto& t = lta.topics(i);
		auto& dst = topics[i];

		dst.name = t.name();
		dst.type = t.type();

		ASSERT_EQUAL_(t.endpoint_size(), t.publishername_size());
		dst.endpoints.resize(t.endpoint_size());
		dst.publishers.resize(t.endpoint_size());

		for (int k = 0; k < t.endpoint_size(); k++)
		{
			dst.publishers[k] = t.publishername(k);
			dst.endpoints[k] = t.endpoint(k);
		}
	}
	return topics;
#else
	THROW_EXCEPTION("MVSIM built without ZMQ");
#endif
}

void Client::doAdvertiseTopic(
	const std::string& topicName,
	const google::protobuf::Descriptor* descriptor)
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)

	auto& advTopics = zmq_->advertisedTopics;

	std::unique_lock<std::shared_mutex> lck(zmq_->advertisedTopics_mtx);

	if (advTopics.find(topicName) != advTopics.end())
		THROW_EXCEPTION_FMT(
			"Topic `%s` already registered for publication in this same "
			"client (!)",
			topicName.c_str());

	// the ctor of InfoPerAdvertisedTopic automatically creates a ZMQ_PUB
	// socket in pubSocket
	InfoPerAdvertisedTopic& ipat =
		advTopics.emplace_hint(advTopics.begin(), topicName, zmq_->context)
			->second;

	lck.unlock();

	// Bind the PUBLISH socket:
	ipat.pubSocket.bind("tcp://0.0.0.0:*");
	if (!ipat.pubSocket.connected())
		THROW_EXCEPTION("Could not bind publisher socket");

	// Retrieve assigned TCP port:
	char assignedPort[100];
	size_t assignedPortLen = sizeof(assignedPort);
	ipat.pubSocket.getsockopt(
		ZMQ_LAST_ENDPOINT, assignedPort, &assignedPortLen);
	assignedPort[assignedPortLen] = '\0';

	ipat.endpoint = assignedPort;
	ipat.topicName = topicName;	 // redundant in container, but handy.
	ipat.descriptor = descriptor;

	MRPT_LOG_DEBUG_FMT(
		"Advertising topic `%s` [%s] on endpoint `%s`", topicName.c_str(),
		descriptor->full_name().c_str(), ipat.endpoint.c_str());

	// MRPT_LOG_INFO_STREAM("Type: " << descriptor->DebugString());

	mvsim_msgs::AdvertiseTopicRequest req;
	req.set_topicname(ipat.topicName);
	req.set_endpoint(ipat.endpoint);
	req.set_topictypename(ipat.descriptor->full_name());
	req.set_nodename(nodeName_);

	mvsim::sendMessage(req, *zmq_->mainReqSocket);

	//  Get the reply.
	const zmq::message_t reply = mvsim::receiveMessage(*zmq_->mainReqSocket);
	mvsim_msgs::GenericAnswer ans;
	mvsim::parseMessage(reply, ans);

	if (!ans.success())
		THROW_EXCEPTION_FMT(
			"Error registering topic `%s` in server: `%s`", topicName.c_str(),
			ans.errormessage().c_str());

#else
	THROW_EXCEPTION("MVSIM built without ZMQ & PROTOBUF");
#endif
}

void Client::doAdvertiseService(
	const std::string& serviceName, const google::protobuf::Descriptor* descIn,
	const google::protobuf::Descriptor* descOut, service_callback_t callback)
{
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)

	std::unique_lock<std::shared_mutex> lck(zmq_->offeredServices_mtx);

	auto& services = zmq_->offeredServices;

	if (services.find(serviceName) != services.end())
		THROW_EXCEPTION_FMT(
			"Service `%s` already registered in this same client!",
			serviceName.c_str());

	InfoPerService& ips = services[serviceName];

	lck.unlock();

	// Retrieve assigned TCP port:
	char assignedPort[100];
	size_t assignedPortLen = sizeof(assignedPort);
	zmq_->srvListenSocket->getsockopt(
		ZMQ_LAST_ENDPOINT, assignedPort, &assignedPortLen);
	assignedPort[assignedPortLen] = '\0';

	ips.serviceName = serviceName;	// redundant in container, but handy.
	ips.callback = callback;
	ips.descInput = descIn;
	ips.descOutput = descOut;

	MRPT_LOG_DEBUG_FMT(
		"Advertising service `%s` [%s->%s] on endpoint `%s`",
		serviceName.c_str(), descIn->full_name().c_str(),
		descOut->full_name().c_str(), assignedPort);

	mvsim_msgs::AdvertiseServiceRequest req;
	req.set_servicename(ips.serviceName);
	req.set_endpoint(assignedPort);
	req.set_inputtypename(ips.descInput->full_name());
	req.set_outputtypename(ips.descOutput->full_name());
	req.set_nodename(nodeName_);

	mvsim::sendMessage(req, *zmq_->mainReqSocket);

	//  Get the reply.
	const zmq::message_t reply = mvsim::receiveMessage(*zmq_->mainReqSocket);
	mvsim_msgs::GenericAnswer ans;
	mvsim::parseMessage(reply, ans);

	if (!ans.success())
		THROW_EXCEPTION_FMT(
			"Error registering service `%s` in server: `%s`",
			serviceName.c_str(), ans.errormessage().c_str());

#else
	THROW_EXCEPTION("MVSIM built without ZMQ & PROTOBUF");
#endif
}

void Client::publishTopic(
	const std::string& topicName, const google::protobuf::Message& msg)
{
	MRPT_START
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)
	ASSERTMSG_(
		zmq_ && zmq_->mainReqSocket && zmq_->mainReqSocket->connected(),
		"Client not connected to Server");

	std::shared_lock<std::shared_mutex> lck(zmq_->advertisedTopics_mtx);
	auto itIpat = zmq_->advertisedTopics.find(topicName);

	ASSERTMSG_(
		itIpat != zmq_->advertisedTopics.end(),
		mrpt::format(
			"Topic `%s` cannot been registered. Missing former call to "
			"advertiseTopic()?",
			topicName.c_str()));

	lck.unlock();

	auto& ipat = itIpat->second;

	ASSERTMSG_(
		msg.GetDescriptor() == ipat.descriptor,
		mrpt::format(
			"Topic `%s` has type `%s`, but expected `%s` from former call "
			"to "
			"advertiseTopic()?",
			topicName.c_str(), msg.GetDescriptor()->name().c_str(),
			ipat.descriptor->name().c_str()));

	ASSERT_(ipat.pubSocket.connected());

	mvsim::sendMessage(msg, ipat.pubSocket);

#if 0
	MRPT_LOG_DEBUG_FMT(
		"Published on topic `%s`: %s", topicName.c_str(),
		msg.DebugString().c_str());
#endif

#else
	THROW_EXCEPTION("MVSIM built without ZMQ & PROTOBUF");
#endif
	MRPT_END
}

void Client::internalServiceServingThread()
{
	using namespace std::string_literals;

#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)
	try
	{
		MRPT_LOG_INFO_STREAM(
			"[" << nodeName_ << "] Client service thread started.");

		zmq::socket_t& s = *zmq_->srvListenSocket;

		for (;;)
		{
			//  Wait for next request from client:
			zmq::message_t m = mvsim::receiveMessage(s);

			// parse it:
			mvsim_msgs::CallService csMsg;
			mvsim::parseMessage(m, csMsg);

			std::shared_lock<std::shared_mutex> lck(zmq_->offeredServices_mtx);
			const auto& srvName = csMsg.servicename();

			auto itSrv = zmq_->offeredServices.find(srvName);
			if (itSrv == zmq_->offeredServices.end())
			{
				// Error: unknown service:
				mvsim_msgs::GenericAnswer ans;
				ans.set_success(false);
				ans.set_errormessage(mrpt::format(
					"Requested unknown service `%s`", srvName.c_str()));
				MRPT_LOG_ERROR_STREAM(ans.errormessage());

				mvsim::sendMessage(ans, s);
				continue;
			}

			InfoPerService& ips = itSrv->second;

			// MRPT_TODO("Check input descriptor?");

			auto outMsgPtr = ips.callback(csMsg.serializedinput());

			// Send response:
			mvsim::sendMessage(*outMsgPtr, s);
		}
	}
	catch (const zmq::error_t& e)
	{
		if (e.num() == ETERM)
		{
			// This simply means someone called
			// requestMainThreadTermination(). Just exit silently.
			MRPT_LOG_INFO_STREAM(
				"internalServiceServingThread about to exit for ZMQ term "
				"signal.");
		}
		else
		{
			MRPT_LOG_ERROR_STREAM(
				"internalServiceServingThread: ZMQ error: " << e.what());
		}
	}
	catch (const std::exception& e)
	{
		MRPT_LOG_ERROR_STREAM(
			"internalServiceServingThread: Exception: "
			<< mrpt::exception_to_str(e));
	}
	MRPT_LOG_DEBUG_STREAM("internalServiceServingThread quitted.");

#endif
}

void Client::doCallService(
	const std::string& serviceName, const google::protobuf::Message& input,
	google::protobuf::Message& output)
{
	MRPT_START
#if defined(MVSIM_HAS_ZMQ) && defined(MVSIM_HAS_PROTOBUF)

	// 1) Request to the server who is serving this service:
	// TODO: Cache?
	std::string srvEndpoint;
	{
		zmq::socket_t& s = *zmq_->mainReqSocket;

		mvsim_msgs::GetServiceInfoRequest gsi;
		gsi.set_servicename(serviceName);
		mvsim::sendMessage(gsi, s);

		auto m = mvsim::receiveMessage(s);
		mvsim_msgs::GetServiceInfoAnswer gsia;
		mvsim::parseMessage(m, gsia);

		if (!gsia.success())
			THROW_EXCEPTION_FMT(
				"Error requesting information about service `%s`: %s",
				serviceName.c_str(), gsia.errormessage().c_str());

		srvEndpoint = gsia.serviceendpoint();
	}

	// 2) Connect to the service offerrer and request the execution:
	zmq::socket_t srvReqSock(zmq_->context, ZMQ_REQ);
	srvReqSock.connect(srvEndpoint);

	mvsim_msgs::CallService csMsg;
	csMsg.set_servicename(serviceName);
	csMsg.set_serializedinput(input.SerializeAsString());

	mvsim::sendMessage(csMsg, srvReqSock);

	const auto m = mvsim::receiveMessage(srvReqSock);
	mvsim::parseMessage(m, output);
#endif
	MRPT_END
}
