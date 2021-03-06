/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvAccessCPP is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#ifdef __vxworks
#include <taskLib.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <process.h>
#endif

#include <sstream>
#include <time.h>
#include <stdlib.h>

#include <pv/responseHandlers.h>
#include <pv/remote.h>
#include <pv/hexDump.h>
#include <pv/serializationHelper.h>

#include <pv/byteBuffer.h>

#include <osiSock.h>
#include <osiProcess.h>
#include <pv/logger.h>

#include <pv/pvAccessMB.h>
#include <pv/rpcServer.h>
#include <pv/security.h>

using std::string;
using std::ostringstream;
using std::hex;

using std::tr1::dynamic_pointer_cast;
using std::tr1::static_pointer_cast;

using namespace epics::pvData;

namespace epics {
namespace pvAccess {

// TODO this is a copy from clientContextImpl.cpp
            static PVDataCreatePtr pvDataCreate = getPVDataCreate();


    		static BitSet::shared_pointer createBitSetFor(
    		          PVStructure::shared_pointer const & pvStructure,
    		          BitSet::shared_pointer const & existingBitSet)
    		{
    			int pvStructureSize = pvStructure->getNumberFields();
    			if (existingBitSet.get() && static_cast<int32>(existingBitSet->size()) >= pvStructureSize)
    			{
    				// clear existing BitSet
    				// also necessary if larger BitSet is reused
    				existingBitSet->clear();
    				return existingBitSet;
    			}
    			else
    				return BitSet::shared_pointer(new BitSet(pvStructureSize));
    		}
    
    		static PVField::shared_pointer reuseOrCreatePVField(
    		          Field::const_shared_pointer const & field,
    		          PVField::shared_pointer const & existingPVField)
    		{
    			if (existingPVField.get() && *field == *existingPVField->getField())
    			     return existingPVField;
    		    else
    		         return pvDataCreate->createPVField(field);
    		}



void ServerBadResponse::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer)
{
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	char ipAddrStr[48];
	ipAddrToDottedIP(&responseFrom->ia, ipAddrStr, sizeof(ipAddrStr));

	LOG(logLevelInfo,
			"Undecipherable message (bad response type %d) from %s.",
			command, ipAddrStr);

}

ServerResponseHandler::ServerResponseHandler(ServerContextImpl::shared_pointer const & context)
{
    MB_INIT;                

    ResponseHandler::shared_pointer badResponse(new ServerBadResponse(context));
    m_handlerTable.resize(CMD_CANCEL_REQUEST+1);
                
    m_handlerTable[CMD_BEACON].reset(new ServerNoopResponse(context, "Beacon")); /*  0 */
    m_handlerTable[CMD_CONNECTION_VALIDATION].reset(new ServerConnectionValidationHandler(context)); /*  1 */
    m_handlerTable[CMD_ECHO].reset(new ServerEchoHandler(context)); /*  2 */
    m_handlerTable[CMD_SEARCH].reset(new ServerSearchHandler(context)); /*  3 */
    m_handlerTable[CMD_SEARCH_RESPONSE] = badResponse;
    m_handlerTable[CMD_AUTHNZ].reset(new AuthNZHandler(context.get())); /*  5 */
    m_handlerTable[CMD_ACL_CHANGE] = badResponse; /*  6 - access right change */
    m_handlerTable[CMD_CREATE_CHANNEL].reset(new ServerCreateChannelHandler(context)); /*  7 */
    m_handlerTable[CMD_DESTROY_CHANNEL].reset(new ServerDestroyChannelHandler(context)); /*  8 */ 
    m_handlerTable[CMD_CONNECTION_VALIDATED] = badResponse; /*  9 */
    
    m_handlerTable[CMD_GET].reset(new ServerGetHandler(context)); /* 10 - get response */
    m_handlerTable[CMD_PUT].reset(new ServerPutHandler(context)); /* 11 - put response */
    m_handlerTable[CMD_PUT_GET].reset(new ServerPutGetHandler(context)); /* 12 - put-get response */
    m_handlerTable[CMD_MONITOR].reset(new ServerMonitorHandler(context)); /* 13 - monitor response */
    m_handlerTable[CMD_ARRAY].reset(new ServerArrayHandler(context)); /* 14 - array response */
    m_handlerTable[CMD_DESTROY_REQUEST].reset(new ServerDestroyRequestHandler(context)); /* 15 - destroy request */
    m_handlerTable[CMD_PROCESS].reset(new ServerProcessHandler(context)); /* 16 - process response */
    m_handlerTable[CMD_GET_FIELD].reset(new ServerGetFieldHandler(context)); /* 17 - get field response */
    m_handlerTable[CMD_MESSAGE] = badResponse; /* 18 - message to Requester */
    m_handlerTable[CMD_MULTIPLE_DATA] = badResponse; /* 19 - grouped monitors */
    m_handlerTable[CMD_RPC].reset(new ServerRPCHandler(context)); /* 20 - RPC response */
    m_handlerTable[CMD_CANCEL_REQUEST].reset(new ServerCancelRequestHandler(context)); /* 21 - cancel request */
}

void ServerResponseHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer)
{
	if(command<0||command>=(int8)m_handlerTable.size())
	{
		LOG(logLevelDebug,
				"Invalid (or unsupported) command: %x.", (0xFF&command));
				
		// TODO remove debug output
		std::ostringstream name;
		name<<"Invalid PVA header "<<hex<<(int)(0xFF&command);
		name<<", its payload buffer";

		hexDump(name.str(), (const int8*)payloadBuffer->getArray(),
				payloadBuffer->getPosition(), payloadSize);
		return;
	}

	// delegate
	m_handlerTable[command]->handleResponse(responseFrom, transport,
			version, command, payloadSize, payloadBuffer);
}

void ServerConnectionValidationHandler::handleResponse(
		osiSockAddr* responseFrom, Transport::shared_pointer const & transport, int8 version,
		int8 command, size_t payloadSize,
		ByteBuffer* payloadBuffer)
{
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

    transport->setRemoteRevision(version);

    transport->ensureData(4+2+2);
	transport->setRemoteTransportReceiveBufferSize(payloadBuffer->getInt());
    // TODO clientIntrospectionRegistryMaxSize
    /* int clientIntrospectionRegistryMaxSize = */ payloadBuffer->getShort();
    // TODO connectionQoS
    /* int16 connectionQoS = */ payloadBuffer->getShort();

    // authNZ
    std::string securityPluginName = SerializeHelper::deserializeString(payloadBuffer, transport.get());

    // optional authNZ plug-in initialization data
    PVField::shared_pointer data;
    if (payloadBuffer->getRemaining())
        data = SerializationHelper::deserializeFull(payloadBuffer, transport.get());

    struct {
        std::string securityPluginName;
        PVField::shared_pointer data;
    } initData = { securityPluginName, data };

    transport->authNZInitialize(&initData);
}






void ServerEchoHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer)
{
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

    // send back
	TransportSender::shared_pointer echoReply(new EchoTransportSender(responseFrom));
	transport->enqueueSendRequest(echoReply);
}

/****************************************************************************************/

std::string ServerSearchHandler::SUPPORTED_PROTOCOL = "tcp";

ServerSearchHandler::ServerSearchHandler(ServerContextImpl::shared_pointer const & context) :
        AbstractServerResponseHandler(context, "Search request"), _providers(context->getChannelProviders())
{
    // initialize random seed with some random value
    srand ( time(NULL) );
}

void ServerSearchHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer)
{
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

    transport->ensureData(4+1+3+16+2);

    size_t startPosition = payloadBuffer->getPosition();

	const int32 searchSequenceId = payloadBuffer->getInt();
	const int8 qosCode = payloadBuffer->getByte();

    // reserved part
    payloadBuffer->getByte();
    payloadBuffer->getShort();

    osiSockAddr responseAddress;
    responseAddress.ia.sin_family = AF_INET;

    // 128-bit IPv6 address
    if (!decodeAsIPv6Address(payloadBuffer, &responseAddress)) return;

    // accept given address if explicitly specified by sender
    if (responseAddress.ia.sin_addr.s_addr == INADDR_ANY)
        responseAddress.ia.sin_addr = responseFrom->ia.sin_addr;

    // NOTE: htons might be a macro (e.g. vxWorks)
    int16 port = payloadBuffer->getShort();
    responseAddress.ia.sin_port = htons(port);

    size_t protocolsCount = SerializeHelper::readSize(payloadBuffer, transport.get());
    bool allowed = (protocolsCount == 0);
    for (size_t i = 0; i < protocolsCount; i++)
    {
        string protocol = SerializeHelper::deserializeString(payloadBuffer, transport.get());
        if (SUPPORTED_PROTOCOL == protocol)
            allowed = true;
    }

    // NOTE: we do not stop reading the buffer

    transport->ensureData(2);
	const int32 count = payloadBuffer->getShort() & 0xFFFF;

    // TODO DoS attack?
	const bool responseRequired = (QOS_REPLY_REQUIRED & qosCode) != 0;

    // TODO bloom filter or similar server selection (by GUID)

    //
    // locally broadcast if unicast (qosCode & 0x80 == 0x80)
    //
    if ((qosCode & 0x80) == 0x80)
    {
        BlockingUDPTransport::shared_pointer bt = _context->getLocalMulticastTransport();
        if (bt)
        {
            // clear unicast flag
            payloadBuffer->put(startPosition+4, (int8)(qosCode & ~0x80));

            // update response address
            payloadBuffer->setPosition(startPosition+8);
            encodeAsIPv6Address(payloadBuffer, &responseAddress);

            payloadBuffer->setPosition(payloadBuffer->getLimit());		// send will call flip()

            bt->send(payloadBuffer);
            return;
        }
    }

    if (count > 0)
    {
        for (int32 i = 0; i < count; i++)
        {
            transport->ensureData(4);
            const int32 cid = payloadBuffer->getInt();
            const string name = SerializeHelper::deserializeString(payloadBuffer, transport.get());
            // no name check here...

            if (allowed)
            {
                // TODO object pool!!!
                int providerCount = _providers.size();
                std::tr1::shared_ptr<ServerChannelFindRequesterImpl> tp(new ServerChannelFindRequesterImpl(_context, providerCount));
                tp->set(name, searchSequenceId, cid, responseAddress, responseRequired, false);
                // TODO use std::make_shared
                ChannelFindRequester::shared_pointer spr = tp;

                for (int i = 0; i < providerCount; i++)
                  _providers[i]->channelFind(name, spr);
            }
        }
    }
    else
    {
        if (allowed)
        {
            // TODO constant
            #define MAX_SERVER_SEARCH_RESPONSE_DELAY_MS 100
            double period = (rand() % MAX_SERVER_SEARCH_RESPONSE_DELAY_MS)/(double)1000;

            std::tr1::shared_ptr<ServerChannelFindRequesterImpl> tp(new ServerChannelFindRequesterImpl(_context, 1));
            tp->set("", searchSequenceId, 0, responseAddress, true, true);

            // TODO use std::make_shared
            TimerCallback::shared_pointer tc = tp;
            _context->getTimer()->scheduleAfterDelay(tc, period);
        }
    }
}

ServerChannelFindRequesterImpl::ServerChannelFindRequesterImpl(ServerContextImpl::shared_pointer const & context,
                                                               int32 expectedResponseCount) :
                                                _guid(context->getGUID()),
                                                _sendTo(),
												_wasFound(false),
												_context(context),
												_expectedResponseCount(expectedResponseCount),
                                                _responseCount(0),
                                                _serverSearch(false)
												{}

void ServerChannelFindRequesterImpl::clear()
{
	Lock guard(_mutex);
	_wasFound = false;
	_responseCount = 0;
    _serverSearch = false;
}

void ServerChannelFindRequesterImpl::callback()
{
    channelFindResult(Status::Ok, ChannelFind::shared_pointer(), false);
}

void ServerChannelFindRequesterImpl::timerStopped()
{
    // noop
}

ServerChannelFindRequesterImpl* ServerChannelFindRequesterImpl::set(std::string name, int32 searchSequenceId, int32 cid, osiSockAddr const & sendTo,
                                                                    bool responseRequired, bool serverSearch)
{
	Lock guard(_mutex);
	_name = name;
	_searchSequenceId = searchSequenceId;
	_cid = cid;
	_sendTo = sendTo;
	_responseRequired = responseRequired;
    _serverSearch = serverSearch;
	return this;
}

std::map<string, std::tr1::weak_ptr<ChannelProvider> > ServerSearchHandler::s_channelNameToProvider;

void ServerChannelFindRequesterImpl::channelFindResult(const Status& /*status*/, ChannelFind::shared_pointer const & channelFind, bool wasFound)
{
	// TODO status
	Lock guard(_mutex);
	
	_responseCount++;
	if (_responseCount > _expectedResponseCount)
	{
	   if ((_responseCount+1) == _expectedResponseCount)
	   {
    	   LOG(logLevelDebug,"[ServerChannelFindRequesterImpl::channelFindResult] More responses received than expected fpr channel '%s'!", _name.c_str());
	   }
	   return;
	}
	
	if (wasFound && _wasFound)
	{
	   LOG(logLevelDebug,"[ServerChannelFindRequesterImpl::channelFindResult] Channel '%s' is hosted by different channel providers!", _name.c_str());
	   return;
	}
	
	if (wasFound || (_responseRequired && (_responseCount == _expectedResponseCount)))
	{
	   if (wasFound && _expectedResponseCount > 1)
	   {
        ServerSearchHandler::s_channelNameToProvider[_name] = channelFind->getChannelProvider();
	   }

		_wasFound = wasFound;
		TransportSender::shared_pointer thisSender = shared_from_this();
		_context->getBroadcastTransport()->enqueueSendRequest(thisSender);
	}
}

void ServerChannelFindRequesterImpl::lock()
{
	// noop
}

void ServerChannelFindRequesterImpl::unlock()
{
	// noop
}

void ServerChannelFindRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
    control->startMessage((int8)4, 12+4+16+2);

	Lock guard(_mutex);
    buffer->put(_guid.value, 0, sizeof(_guid.value));
	buffer->putInt(_searchSequenceId);

	// NOTE: is it possible (very likely) that address is any local address ::ffff:0.0.0.0
	encodeAsIPv6Address(buffer, _context->getServerInetAddress());
	buffer->putShort((int16)_context->getServerPort());

    SerializeHelper::serializeString(ServerSearchHandler::SUPPORTED_PROTOCOL, buffer, control);

    control->ensureBuffer(1);
    buffer->putByte(_wasFound ? (int8)1 : (int8)0);

    if (!_serverSearch)
    {
        // TODO for now we do not gather search responses
        buffer->putShort((int16)1);
        buffer->putInt(_cid);
    }
    else
    {
        buffer->putShort((int16)0);
    }

    control->setRecipient(_sendTo);
}

/****************************************************************************************/

class ChannelListRequesterImpl :
        public ChannelListRequester
{
public:
    POINTER_DEFINITIONS(ChannelListRequesterImpl);

    PVStringArray::const_svector channelNames;
    Status status;

    virtual void channelListResult(
            const epics::pvData::Status& status,
            ChannelFind::shared_pointer const & channelFind,
            PVStringArray::const_svector const & channelNames,
            bool hasDynamic)
    {
        epics::pvData::Lock lock(_waitMutex);

        this->status = status;
        this->channelNames = channelNames;

        _waitEvent.signal();
    }

    bool waitForCompletion(int32 timeoutSec) {
        return _waitEvent.wait(timeoutSec);
    }

    void resetEvent() {
        _waitEvent.tryWait();
    }

private:
    epics::pvData::Mutex _waitMutex;
    epics::pvData::Event _waitEvent;

};

// TODO move out to a separate class
class ServerRPCService : public RPCService {

private:
    static int32 TIMEOUT_SEC;

    static Structure::const_shared_pointer helpStructure;
    static Structure::const_shared_pointer channelListStructure;
    static Structure::const_shared_pointer infoStructure;

    static std::string helpString;

    ServerContextImpl::shared_pointer m_serverContext;

    // s1 starts with s2 check
    static bool starts_with(const string& s1, const string& s2) {
        return s2.size() <= s1.size() && s1.compare(0, s2.size(), s2) == 0;
    }

public:

    ServerRPCService(ServerContextImpl::shared_pointer const & context) :
        m_serverContext(context)
    {
    }

    virtual epics::pvData::PVStructure::shared_pointer request(
        epics::pvData::PVStructure::shared_pointer const & arguments
    )
    {
        // NTURI support
        PVStructure::shared_pointer args(
                    (starts_with(arguments->getStructure()->getID(), "epics:nt/NTURI:1.")) ?
                        arguments->getSubField<PVStructure>("query") :
                        arguments
                        );

        // help support
        if (args->getSubField("help"))
        {
            PVStructure::shared_pointer help = getPVDataCreate()->createPVStructure(helpStructure);
            help->getSubFieldT<PVString>("value")->put(helpString);
            return help;
        }

        PVString::shared_pointer opField = args->getSubField<PVString>("op");
        if (!opField)
            throw RPCRequestException(Status::STATUSTYPE_ERROR, "unspecified 'string op' field");

        string op = opField->get();
        if (op == "channels")
        {
            PVStructure::shared_pointer result =
                getPVDataCreate()->createPVStructure(channelListStructure);
            PVStringArray::shared_pointer allChannelNames = result->getSubFieldT<PVStringArray>("value");

            ChannelListRequesterImpl::shared_pointer listListener(new ChannelListRequesterImpl());
            std::vector<ChannelProvider::shared_pointer>& providers = m_serverContext->getChannelProviders();

            size_t providerCount = providers.size();
            for (size_t i = 0; i < providerCount; i++)
            {
                providers[i]->channelList(listListener);
                if (!listListener->waitForCompletion(TIMEOUT_SEC))
                    throw RPCRequestException(Status::STATUSTYPE_ERROR, "failed to fetch channel list due to timeout");

                Status& status = listListener->status;
                if (!status.isSuccess())
                {
                    string errorMessage = "failed to fetch channel list: " + status.getMessage();
                    if (!status.getStackDump().empty())
                         errorMessage += "\n" + status.getStackDump();
                    if (providerCount == 1)
                        throw RPCRequestException(Status::STATUSTYPE_ERROR, errorMessage);
                    else
                    {
                        LOG(logLevelDebug, "%s: %s", providers[i]->getProviderName().c_str(), errorMessage.c_str());
                    }
                }

                // optimization
                if (providerCount == 1)
                {
                    allChannelNames->replace(listListener->channelNames);
                }
                else
                {
                    PVStringArray::svector list(allChannelNames->reuse());
                    std::copy(listListener->channelNames.begin(), listListener->channelNames.end(),
                              back_inserter(list));
                    allChannelNames->replace(freeze(list));
                }

                listListener->resetEvent();
            }

            return result;
        }
        else if (op == "info")
        {
            PVStructure::shared_pointer result =
                getPVDataCreate()->createPVStructure(infoStructure);

            // TODO cache hostname in InetAddressUtil
            char buffer[256];
            std::string hostName("localhost");
            if (gethostname(buffer, sizeof(buffer)) == 0)
                hostName = buffer;

            std::stringstream ret;
            ret << EPICS_PVA_MAJOR_VERSION << '.' <<
                   EPICS_PVA_MINOR_VERSION << '.' <<
                   EPICS_PVA_MAINTENANCE_VERSION;
            if (EPICS_PVA_DEVELOPMENT_FLAG)
                ret << "-SNAPSHOT";

            result->getSubFieldT<PVString>("version")->put(ret.str());
            result->getSubFieldT<PVString>("implLang")->put("cpp");
            result->getSubFieldT<PVString>("host")->put(hostName);

            std::stringstream sspid;
#ifdef __vxworks
            sspid << taskIdSelf();
#elif defined(_WIN32)
            sspid << _getpid();
#else
            sspid << getpid();
#endif
            result->getSubFieldT<PVString>("process")->put(sspid.str());

            char timeText[64];
            epicsTimeToStrftime(timeText, 64, "%Y-%m-%dT%H:%M:%S.%03f", &m_serverContext->getStartTime());

            result->getSubFieldT<PVString>("startTime")->put(timeText);


            return result;
        }
        else
            throw RPCRequestException(Status::STATUSTYPE_ERROR, "unsupported operation '" + op + "'.");
    }
};

int32 ServerRPCService::TIMEOUT_SEC = 3;
Structure::const_shared_pointer ServerRPCService::helpStructure =
        getFieldCreate()->createFieldBuilder()->
            setId("epics:nt/NTScalar:1.0")->
            add("value", pvString)->
            createStructure();

Structure::const_shared_pointer ServerRPCService::channelListStructure =
        getFieldCreate()->createFieldBuilder()->
            setId("epics:nt/NTScalarArray:1.0")->
            addArray("value", pvString)->
            createStructure();

Structure::const_shared_pointer ServerRPCService::infoStructure =
        getFieldCreate()->createFieldBuilder()->
                add("process", pvString)->
                add("startTime", pvString)->
                add("version", pvString)->
                add("implLang", pvString)->
                add("host", pvString)->
//                add("os", pvString)->
//                add("arch", pvString)->
//                add("CPUs", pvInt)->
                createStructure();


std::string ServerRPCService::helpString =
        "pvAccess server RPC service.\n"
        "arguments:\n"
        "\tstring op\toperation to execute\n"
        "\n"
        "\toperations:\n"
        "\t\tinfo\t\treturns some information about the server\n"
        "\t\tchannels\treturns a list of 'static' channels the server can provide\n"
//        "\t\t\t (no arguments)\n"
        "\n";

std::string ServerCreateChannelHandler::SERVER_CHANNEL_NAME = "server";

void ServerCreateChannelHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer)
{
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// TODO for not only one request at the time is supported, i.e. dataCount == 1
	transport->ensureData((sizeof(int32)+sizeof(int16))/sizeof(int8));
	const int16 count = payloadBuffer->getShort();
	if (count != 1)
	{
		THROW_BASE_EXCEPTION("only 1 supported for now");
	}
	const pvAccessID cid = payloadBuffer->getInt();

	string channelName = SerializeHelper::deserializeString(payloadBuffer, transport.get());
	if (channelName.size() == 0)
	{

		char host[100];
		sockAddrToA(&transport->getRemoteAddress()->sa,host,100);
		LOG(logLevelDebug,"Zero length channel name, disconnecting client: %s", host);
		disconnect(transport);
		return;
	}
	else if (channelName.size() > MAX_CHANNEL_NAME_LENGTH)
	{
		char host[100];
		sockAddrToA(&transport->getRemoteAddress()->sa,host,100);
		LOG(logLevelDebug,"Unreasonable channel name length, disconnecting client: %s", host);
		disconnect(transport);
		return;
	}

    SecuritySession::shared_pointer securitySession = transport->getSecuritySession();
    ChannelSecuritySession::shared_pointer css;
    try {
        css = securitySession->createChannelSession(channelName);
        if (!css)
            throw SecurityException("null channelSecuritySession");
    } catch (SecurityException& se) {
        // TODO use std::make_shared
        std::tr1::shared_ptr<ServerChannelRequesterImpl> tp(new ServerChannelRequesterImpl(transport, channelName, cid, css));
        ChannelRequester::shared_pointer cr = tp;

        Status asStatus(Status::STATUSTYPE_ERROR,
                        string("Insufficient rights to create a channel: ") + se.what());
        cr->channelCreated(asStatus, Channel::shared_pointer());
        return;
    }

    if (channelName == SERVER_CHANNEL_NAME)
    {
        // TODO singleton!!!
        ServerRPCService::shared_pointer serverRPCService(new ServerRPCService(_context));

        // TODO use std::make_shared
        std::tr1::shared_ptr<ServerChannelRequesterImpl> tp(new ServerChannelRequesterImpl(transport, channelName, cid, css));
        ChannelRequester::shared_pointer cr = tp;
        Channel::shared_pointer serverChannel = createRPCChannel(ChannelProvider::shared_pointer(), channelName, cr, serverRPCService);
        cr->channelCreated(Status::Ok, serverChannel);
    }
    else
    {
        if (_providers.size() == 1)
            ServerChannelRequesterImpl::create(_providers[0], transport, channelName, cid, css);
        else
            ServerChannelRequesterImpl::create(ServerSearchHandler::s_channelNameToProvider[channelName].lock(), transport, channelName, cid, css);     // TODO !!!!
    }
}

void ServerCreateChannelHandler::disconnect(Transport::shared_pointer const & transport)
{
	transport->close();
}

ServerChannelRequesterImpl::ServerChannelRequesterImpl(Transport::shared_pointer const & transport,
    const string channelName, const pvAccessID cid, ChannelSecuritySession::shared_pointer const & css) :
    _serverChannel(),
    _transport(transport),
    _channelName(channelName),
    _cid(cid),
    _css(css),
    _status(),
    _mutex()
{
}

ChannelRequester::shared_pointer ServerChannelRequesterImpl::create(
    ChannelProvider::shared_pointer const & provider, Transport::shared_pointer const & transport,
    const string channelName, const pvAccessID cid, ChannelSecuritySession::shared_pointer const & css)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerChannelRequesterImpl> tp(new ServerChannelRequesterImpl(transport, channelName, cid, css));
    ChannelRequester::shared_pointer cr = tp;
    // TODO exception guard and report error back
	provider->createChannel(channelName, cr, transport->getPriority());
	return cr;
}

void ServerChannelRequesterImpl::channelCreated(const Status& status, Channel::shared_pointer const & channel)
{
    if(Transport::shared_pointer transport = _transport.lock())
    {
		ServerChannel::shared_pointer serverChannel;
		try
		{
		    if (status.isSuccess())
		    {
    			// NOTE: we do not explicitly check if transport OK
    			ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);
    			if (!casTransport.get())
    			     THROW_BASE_EXCEPTION("transport is unable to host channels");
    
    			//
    			// create a new channel instance
    			//
    			pvAccessID sid = casTransport->preallocateChannelSID();
    			try
    			{
                    serverChannel.reset(new ServerChannelImpl(channel, _cid, sid, _css));
    
    				// ack allocation and register
    				casTransport->registerChannel(sid, serverChannel);
    
    			} catch (...)
    			{
    				// depreallocate and rethrow
    				casTransport->depreallocateChannelSID(sid);
    				throw;
    			}
		    }
            else
            {
                if (_css)
                    _css->close();
            }

				
			{
        	Lock guard(_mutex);
	        _status = status;
	        _serverChannel = serverChannel;
            }

        	TransportSender::shared_pointer thisSender = shared_from_this();
        	transport->enqueueSendRequest(thisSender);
		}
		catch (std::exception& e)
		{
			LOG(logLevelDebug, "Exception caught when creating channel: %s", _channelName.c_str());
			{
			 Lock guard(_mutex);
            _status = Status(Status::STATUSTYPE_FATAL,  "failed to create channel", e.what());
			}
        	TransportSender::shared_pointer thisSender = shared_from_this();
        	transport->enqueueSendRequest(thisSender);
			// TODO make sure that serverChannel gets destroyed
            if (_css)
                _css->close();
        }
		catch (...)
		{
			LOG(logLevelDebug, "Exception caught when creating channel: %s", _channelName.c_str());
			{
			 Lock guard(_mutex);
			_status = Status(Status::STATUSTYPE_FATAL,  "failed to create channel");
			}
        	TransportSender::shared_pointer thisSender = shared_from_this();
        	transport->enqueueSendRequest(thisSender);
			// TODO make sure that serverChannel gets destroyed
            if (_css)
                _css->close();
        }
    }
}

void ServerChannelRequesterImpl::channelStateChange(Channel::shared_pointer const & /*channel*/, const Channel::ConnectionState /*isConnected*/)
{
	// TODO should we notify remote side?
}

string ServerChannelRequesterImpl::getRequesterName()
{
	std::stringstream name;
	name << "ServerChannelRequesterImpl/" << _channelName << "[" << _cid << "]"; 
	return name.str();
}

void ServerChannelRequesterImpl::message(std::string const & message, MessageType messageType)
{
	LOG(logLevelDebug, "[%s] %s", getMessageTypeName(messageType).c_str(), message.c_str());
}

void ServerChannelRequesterImpl::lock()
{
	//noop
}

void ServerChannelRequesterImpl::unlock()
{
	//noop
}

void ServerChannelRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	ServerChannel::shared_pointer serverChannel;
	Status status;
	{
		Lock guard(_mutex);
		serverChannel = _serverChannel.lock();
		status = _status;
	}

	// error response
	if (serverChannel.get() == NULL)
	{
		createChannelFailedResponse(buffer, control, status);
	}
	// OK
	else if (Transport::shared_pointer transport = _transport.lock())
	{
	   ServerChannelImpl::shared_pointer serverChannelImpl = dynamic_pointer_cast<ServerChannelImpl>(serverChannel);
		control->startMessage((int8)CMD_CREATE_CHANNEL, 2*sizeof(int32)/sizeof(int8));
		buffer->putInt(serverChannelImpl->getCID());
		buffer->putInt(serverChannelImpl->getSID());
		status.serialize(buffer, control);
	}
}


void ServerChannelRequesterImpl::createChannelFailedResponse(ByteBuffer* buffer, TransportSendControl* control, const Status& status)
{
	if (Transport::shared_pointer transport = _transport.lock())
	{
        control->startMessage((int8)CMD_CREATE_CHANNEL, 2*sizeof(int32)/sizeof(int8));
    	buffer->putInt(_cid);
    	buffer->putInt(-1);
		status.serialize(buffer, control);
	}
}

/****************************************************************************************/

void ServerDestroyChannelHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer)
{
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);


	transport->ensureData(2*sizeof(int32)/sizeof(int8));
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID cid = payloadBuffer->getInt();

	// get channel by SID
	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
	if (channel.get() == NULL)
	{
		if (!transport->isClosed())
		{
			char host[100];
			sockAddrToA(&responseFrom->sa,host,100);
			LOG(logLevelDebug, "Trying to destroy a channel that no longer exists (SID: %d, CID %d, client: %s).", sid, cid, host);
		}
		return;
	}

	// destroy
	channel->destroy();

	// .. and unregister
	casTransport->unregisterChannel(sid);

	// send response back
	TransportSender::shared_pointer sr(new ServerDestroyChannelHandlerTransportSender(cid, sid));
	transport->enqueueSendRequest(sr);
}

/****************************************************************************************/

void ServerGetHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer)
{
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8)+1);
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	// mode
	const int8 qosCode = payloadBuffer->getByte();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
	if (channel.get() == NULL)
	{
		BaseChannelRequester::sendFailureMessage((int8)CMD_GET, transport, ioid, qosCode, BaseChannelRequester::badCIDStatus);
		return;
	}

	const bool init = (QOS_INIT & qosCode) != 0;
	if (init)
	{
		// pvRequest
		PVStructure::shared_pointer pvRequest(SerializationHelper::deserializePVRequest(payloadBuffer, transport.get()));

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeCreateChannelGet(ioid, pvRequest);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_GET, transport, ioid, qosCode, asStatus);
            return;
        }

        // create...
		ServerChannelGetRequesterImpl::create(_context, channel, ioid, transport, pvRequest);
	}
	else
	{
        MB_INC_AUTO_ID(channelGet);
        MB_POINT(channelGet, 3, "server channelGet->deserialize request (start)");
        
		const bool lastRequest = (QOS_DESTROY & qosCode) != 0;

		ServerChannelGetRequesterImpl::shared_pointer request = static_pointer_cast<ServerChannelGetRequesterImpl>(channel->getRequest(ioid));
        if (!request)
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_GET, transport, ioid, qosCode, BaseChannelRequester::badIOIDStatus);
			return;
		}

		if (!request->startRequest(qosCode))
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_GET, transport, ioid, qosCode, BaseChannelRequester::otherRequestPendingStatus);
			return;
		}

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeGet(ioid);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_GET, transport, ioid, qosCode, asStatus);
            if (lastRequest)
                request->destroy();
            return;
        }

        MB_POINT(channelGet, 4, "server channelGet->deserialize request (end)");
        
        ChannelGet::shared_pointer channelGet = request->getChannelGet();
        if (lastRequest)
            channelGet->lastRequest();
        channelGet->get();
	}
}

#define INIT_EXCEPTION_GUARD(cmd, code) \
    try { \
 	    code; \
    } \
    catch (std::exception &e) { \
        Status status(Status::STATUSTYPE_FATAL, e.what()); \
	    BaseChannelRequester::sendFailureMessage((int8)cmd, _transport, _ioid, (int8)QOS_INIT, status); \
	    destroy(); \
    } \
    catch (...) { \
        Status status(Status::STATUSTYPE_FATAL, "unknown exception caught"); \
	    BaseChannelRequester::sendFailureMessage((int8)cmd, _transport, _ioid, (int8)QOS_INIT, status); \
	    destroy(); \
    }

#define DESERIALIZE_EXCEPTION_GUARD(code) \
    try { \
 	    code; \
    } \
    catch (std::exception &e) { \
        Status status(Status::STATUSTYPE_ERROR, e.what()); \
	    BaseChannelRequester::sendFailureMessage((int8)command, transport, ioid, qosCode, status); \
	    throw; \
    } \
    catch (...) { \
        Status status(Status::STATUSTYPE_ERROR, "unknown exception caught"); \
	    BaseChannelRequester::sendFailureMessage((int8)command, transport, ioid, qosCode, status); \
	    throw; \
    }

ServerChannelGetRequesterImpl::ServerChannelGetRequesterImpl(ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel, const pvAccessID ioid, Transport::shared_pointer const & transport) :
		BaseChannelRequester(context, channel, ioid, transport)

{
}

ChannelGetRequester::shared_pointer ServerChannelGetRequesterImpl::create(ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel, const pvAccessID ioid, Transport::shared_pointer const & transport,
PVStructure::shared_pointer const & pvRequest)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerChannelGetRequesterImpl> tp(new ServerChannelGetRequesterImpl(context, channel, ioid, transport));
    ChannelGetRequester::shared_pointer thisPointer = tp;
    static_cast<ServerChannelGetRequesterImpl*>(thisPointer.get())->activate(pvRequest);
    return thisPointer;
}

void ServerChannelGetRequesterImpl::activate(PVStructure::shared_pointer const & pvRequest)
{
	startRequest(QOS_INIT);
	ChannelGetRequester::shared_pointer thisPointer = shared_from_this();
	Destroyable::shared_pointer thisDestroyable = shared_from_this();
	_channel->registerRequest(_ioid, thisDestroyable);
    INIT_EXCEPTION_GUARD(CMD_GET, _channelGet = _channel->getChannel()->createChannelGet(thisPointer, pvRequest));
}

void ServerChannelGetRequesterImpl::channelGetConnect(const Status& status, ChannelGet::shared_pointer const & channelGet, Structure::const_shared_pointer const & structure)
{
	{
		Lock guard(_mutex);
		_status = status;
		_channelGet = channelGet;

        if (_status.isSuccess())
        {
           _pvStructure = std::tr1::static_pointer_cast<PVStructure>(reuseOrCreatePVField(structure, _pvStructure));
           _bitSet = createBitSetFor(_pvStructure, _bitSet);
        }
    }

	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);

	// self-destruction
	if (!status.isSuccess())
	{
		destroy();
	}
}

void ServerChannelGetRequesterImpl::getDone(const Status& status, ChannelGet::shared_pointer const & /*channelGet*/,
    PVStructure::shared_pointer const & pvStructure, BitSet::shared_pointer const & bitSet)
{
    MB_POINT(channelGet, 5, "server channelGet->getDone()");
	{
		Lock guard(_mutex);
		_status = status;
        if (_status.isSuccess())
        {
            *_bitSet = *bitSet;
            _pvStructure->copyUnchecked(*pvStructure, *_bitSet);
        }
	}

    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelGetRequesterImpl::destroy()
{
	// keep a reference to ourselves as the owner
	// could release its reference and we don't want to be
	// destroyed prematurely
	shared_pointer self(shared_from_this());

    // hold a reference to channelGet so that _channelGet.reset()
    // does not call ~ChannelGet (external code) while we are holding a lock
    ChannelGet::shared_pointer channelGet = _channelGet;
	{
		Lock guard(_mutex);
		_channel->unregisterRequest(_ioid);

        // asCheck
        _channel->getChannelSecuritySession()->release(_ioid);

        if (_channelGet)
		{
			_channelGet->destroy();
            _channelGet.reset();
        }
	}
}

ChannelGet::shared_pointer ServerChannelGetRequesterImpl::getChannelGet()
{
	return _channelGet;
}

void ServerChannelGetRequesterImpl::lock()
{
	// noop
}

void ServerChannelGetRequesterImpl::unlock()
{
	// noop
}

// TODO get rid of all these mutex-es
void ServerChannelGetRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	const int32 request = getPendingRequest();

    ChannelGet::shared_pointer channelGet;
    {
        Lock guard(_mutex);
        channelGet = _channelGet;
        // we must respond to QOS_INIT (e.g. creation error)
        if (!channelGet && !(request & QOS_INIT))
            return;
    }

	control->startMessage((int8)CMD_GET, sizeof(int32)/sizeof(int8) + 1);
	buffer->putInt(_ioid);
	buffer->put((int8)request);
	{
		Lock guard(_mutex);
		_status.serialize(buffer, control);
	}

    // TODO !!!
    // if we call stopRequest() below (the second one, commented out), we might be too late
    // since between last serialization data and stopRequest() a buffer can be already flushed
    // (i.e. in case of directSerialize)
    // if we call it here, then a bad client can issue another request just after stopRequest() was called
	stopRequest();
	
	if (_status.isSuccess())
	{
		if (request & QOS_INIT)
		{
			Lock guard(_mutex);
            control->cachedSerialize(_pvStructure->getStructure(), buffer);
		}
		else
		{
            MB_POINT(channelGet, 6, "server channelGet->serialize response (start)");
            {
                ScopedLock lock(channelGet);
    		    
    			_bitSet->serialize(buffer, control);
    			_pvStructure->serialize(buffer, control, _bitSet.get());
            }
            MB_POINT(channelGet, 7, "server channelGet->serialize response (end)");
		}
	}

	//stopRequest();

	// lastRequest
	if (request & QOS_DESTROY)
	{
		destroy();
	}
}
/****************************************************************************************/
void ServerPutHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);


	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8)+1);
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	// mode
	const int8 qosCode = payloadBuffer->getByte();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		BaseChannelRequester::sendFailureMessage((int8)CMD_PUT, transport, ioid, qosCode, BaseChannelRequester::badCIDStatus);
		return;
	}

	const bool init = (QOS_INIT & qosCode) != 0;
	if (init)
	{
		// pvRequest
		PVStructure::shared_pointer pvRequest(SerializationHelper::deserializePVRequest(payloadBuffer, transport.get()));

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeCreateChannelPut(ioid, pvRequest);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_PUT, transport, ioid, qosCode, asStatus);
            return;
        }

        // create...
		ServerChannelPutRequesterImpl::create(_context, channel, ioid, transport, pvRequest);
	}
	else
	{
		const bool lastRequest = (QOS_DESTROY & qosCode) != 0;
		const bool get = (QOS_GET & qosCode) != 0;

		ServerChannelPutRequesterImpl::shared_pointer request = static_pointer_cast<ServerChannelPutRequesterImpl>(channel->getRequest(ioid));
        if (!request.get())
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_PUT, transport, ioid, qosCode, BaseChannelRequester::badIOIDStatus);
			return;
		}

		if (!request->startRequest(qosCode))
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_PUT, transport, ioid, qosCode, BaseChannelRequester::otherRequestPendingStatus);
			return;
		}

	    ChannelPut::shared_pointer channelPut = request->getChannelPut();

        if (lastRequest)
            channelPut->lastRequest();

		if (get)
		{
            // asCheck
            Status asStatus = channel->getChannelSecuritySession()->authorizeGet(ioid);
            if (!asStatus.isSuccess())
            {
                BaseChannelRequester::sendFailureMessage((int8)CMD_PUT, transport, ioid, qosCode, asStatus);
                if (lastRequest)
                    request->destroy();
                return;
            }

            channelPut->get();
		}
		else
		{
			// deserialize bitSet and do a put
			
			{
    			ScopedLock lock(channelPut);     // TODO not needed if put is processed by the same thread
    			BitSet::shared_pointer putBitSet = request->getPutBitSet();
    			PVStructure::shared_pointer putPVStructure = request->getPutPVStructure();

      		    DESERIALIZE_EXCEPTION_GUARD(
    		        putBitSet->deserialize(payloadBuffer, transport.get());
    		        putPVStructure->deserialize(payloadBuffer, transport.get(), putBitSet.get());
    		    );
    		    
    		    lock.unlock();

                // asCheck
                Status asStatus = channel->getChannelSecuritySession()->authorizePut(ioid, putPVStructure, putBitSet);
                if (!asStatus.isSuccess())
                {
                    BaseChannelRequester::sendFailureMessage((int8)CMD_PUT, transport, ioid, qosCode, asStatus);
                    if (lastRequest)
                        request->destroy();
                    return;
                }

                channelPut->put(putPVStructure, putBitSet);
			}
		}
	}
}

ServerChannelPutRequesterImpl::ServerChannelPutRequesterImpl(ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport):
		BaseChannelRequester(context, channel, ioid, transport)
{
}

ChannelPutRequester::shared_pointer ServerChannelPutRequesterImpl::create(ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport, PVStructure::shared_pointer const & pvRequest)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerChannelPutRequesterImpl> tp(new ServerChannelPutRequesterImpl(context, channel, ioid, transport));
    ChannelPutRequester::shared_pointer thisPointer = tp;
    static_cast<ServerChannelPutRequesterImpl*>(thisPointer.get())->activate(pvRequest);
    return thisPointer;
}

void ServerChannelPutRequesterImpl::activate(PVStructure::shared_pointer const & pvRequest)
{
	startRequest(QOS_INIT);
	ChannelPutRequester::shared_pointer thisPointer = shared_from_this();
	Destroyable::shared_pointer thisDestroyable = shared_from_this();
	_channel->registerRequest(_ioid, thisDestroyable);
    INIT_EXCEPTION_GUARD(CMD_PUT, _channelPut = _channel->getChannel()->createChannelPut(thisPointer, pvRequest));
}

void ServerChannelPutRequesterImpl::channelPutConnect(const Status& status, ChannelPut::shared_pointer const & channelPut, Structure::const_shared_pointer const & structure)
{
	{
		Lock guard(_mutex);
		_status = status;
		_channelPut = channelPut;
        if (_status.isSuccess())
        {
           _pvStructure = std::tr1::static_pointer_cast<PVStructure>(reuseOrCreatePVField(structure, _pvStructure));
           _bitSet = createBitSetFor(_pvStructure, _bitSet);
        }
    }
	
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);

	// self-destruction
	if (!status.isSuccess())
	{
		destroy();
	}
}

void ServerChannelPutRequesterImpl::putDone(const Status& status, ChannelPut::shared_pointer const & /*channelPut*/)
{
	{
		Lock guard(_mutex);
		_status = status;
	}
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelPutRequesterImpl::getDone(const Status& status, ChannelPut::shared_pointer const & /*channelPut*/, PVStructure::shared_pointer const & pvStructure, BitSet::shared_pointer const & bitSet)
{
	{
		Lock guard(_mutex);
		_status = status;
        if (_status.isSuccess())
        {
            *_bitSet = *bitSet;
            _pvStructure->copyUnchecked(*pvStructure, *_bitSet);
        }
    }
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelPutRequesterImpl::lock()
{
	//noop
}

void ServerChannelPutRequesterImpl::unlock()
{
	//noop
}

void ServerChannelPutRequesterImpl::destroy()
{
	// keep a reference to ourselves as the owner
	// could release its reference and we don't want to be
	// destroyed prematurely
	shared_pointer self(shared_from_this());

    // hold a reference to channelGet so that _channelPut.reset()
    // does not call ~ChannelPut (external code) while we are holding a lock
    ChannelPut::shared_pointer channelPut = _channelPut;
    {
		Lock guard(_mutex);
		_channel->unregisterRequest(_ioid);

        // asCheck
        _channel->getChannelSecuritySession()->release(_ioid);

        if (_channelPut)
		{
			_channelPut->destroy();
            _channelPut.reset();
        }
	}
}

ChannelPut::shared_pointer ServerChannelPutRequesterImpl::getChannelPut()
{
	//Lock guard(_mutex);
	return _channelPut;
}

BitSet::shared_pointer ServerChannelPutRequesterImpl::getPutBitSet()
{
	//Lock guard(_mutex);
    return _bitSet;
}

PVStructure::shared_pointer ServerChannelPutRequesterImpl::getPutPVStructure()
{
	//Lock guard(_mutex);
    return _pvStructure;
}

void ServerChannelPutRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	const int32 request = getPendingRequest();

    ChannelPut::shared_pointer channelPut;
    {
        Lock guard(_mutex);
        channelPut = _channelPut;
        // we must respond to QOS_INIT (e.g. creation error)
        if (!channelPut && !(request & QOS_INIT))
            return;
    }

    control->startMessage((int32)CMD_PUT, sizeof(int32)/sizeof(int8) + 1);
	buffer->putInt(_ioid);
	buffer->putByte((int8)request);
	{
		Lock guard(_mutex);
		_status.serialize(buffer, control);
	}

	if (_status.isSuccess())
	{
		if ((QOS_INIT & request) != 0)
		{
			Lock guard(_mutex);
            control->cachedSerialize(_pvStructure->getStructure(), buffer);
		}
		else if ((QOS_GET & request) != 0)
		{
            ScopedLock lock(channelPut);
    		_bitSet->serialize(buffer, control);
			_pvStructure->serialize(buffer, control, _bitSet.get());
		}
	}

	stopRequest();

	// lastRequest
	if ((QOS_DESTROY & request) != 0)
		destroy();
}


/****************************************************************************************/
void ServerPutGetHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8)+1);
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	// mode
	const int8 qosCode = payloadBuffer->getByte();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		BaseChannelRequester::sendFailureMessage((int8)CMD_PUT_GET, transport, ioid, qosCode, BaseChannelRequester::badCIDStatus);
		return;
	}

	const bool init = (QOS_INIT & qosCode) != 0;
	if (init)
	{
		// pvRequest
		PVStructure::shared_pointer pvRequest(SerializationHelper::deserializePVRequest(payloadBuffer, transport.get()));

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeCreateChannelPutGet(ioid, pvRequest);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_PUT_GET, transport, ioid, qosCode, asStatus);
            return;
        }

        // create...
		ServerChannelPutGetRequesterImpl::create(_context, channel, ioid, transport, pvRequest);
	}
	else
	{
		const bool lastRequest = (QOS_DESTROY & qosCode) != 0;
		const bool getGet = (QOS_GET & qosCode) != 0;
		const bool getPut = (QOS_GET_PUT & qosCode) != 0;

		ServerChannelPutGetRequesterImpl::shared_pointer request = static_pointer_cast<ServerChannelPutGetRequesterImpl>(channel->getRequest(ioid));
        if (!request.get())
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_PUT_GET, transport, ioid, qosCode, BaseChannelRequester::badIOIDStatus);
			return;
		}

		if (!request->startRequest(qosCode))
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_PUT_GET, transport, ioid, qosCode, BaseChannelRequester::otherRequestPendingStatus);
			return;
		}

    	ChannelPutGet::shared_pointer channelPutGet = request->getChannelPutGet();
        if (lastRequest)
            channelPutGet->lastRequest();

		if (getGet)
		{
            // asCheck
            Status asStatus = channel->getChannelSecuritySession()->authorizeGet(ioid);
            if (!asStatus.isSuccess())
            {
                BaseChannelRequester::sendFailureMessage((int8)CMD_PUT_GET, transport, ioid, qosCode, asStatus);
                if (lastRequest)
                    request->destroy();
                return;
            }

            channelPutGet->getGet();
		}
		else if(getPut)
		{
            // asCheck
            Status asStatus = channel->getChannelSecuritySession()->authorizeGet(ioid);
            if (!asStatus.isSuccess())
            {
                BaseChannelRequester::sendFailureMessage((int8)CMD_PUT_GET, transport, ioid, qosCode, asStatus);
                if (lastRequest)
                    request->destroy();
                return;
            }

            channelPutGet->getPut();
		}
		else
		{
			// deserialize bitSet and do a put
			{
    			ScopedLock lock(channelPutGet);  // TODO not necessary if read is done in putGet
    			BitSet::shared_pointer putBitSet = request->getPutGetBitSet();
    			PVStructure::shared_pointer putPVStructure = request->getPutGetPVStructure();

      		    DESERIALIZE_EXCEPTION_GUARD(
    		        putBitSet->deserialize(payloadBuffer, transport.get());
    		        putPVStructure->deserialize(payloadBuffer, transport.get(), putBitSet.get());
    		    );
    		    
    		    lock.unlock();

                // asCheck
                Status asStatus = channel->getChannelSecuritySession()->authorizePutGet(ioid, putPVStructure, putBitSet);
                if (!asStatus.isSuccess())
                {
                    BaseChannelRequester::sendFailureMessage((int8)CMD_PUT_GET, transport, ioid, qosCode, asStatus);
                    if (lastRequest)
                        request->destroy();
                    return;
                }

                channelPutGet->putGet(putPVStructure, putBitSet);
			}
		}
	}
}

ServerChannelPutGetRequesterImpl::ServerChannelPutGetRequesterImpl(ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport):
		BaseChannelRequester(context, channel, ioid, transport), _channelPutGet(), _pvPutStructure(), _pvGetStructure()
{
}

ChannelPutGetRequester::shared_pointer ServerChannelPutGetRequesterImpl::create(ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport,PVStructure::shared_pointer const & pvRequest)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerChannelPutGetRequesterImpl> tp(new ServerChannelPutGetRequesterImpl(context, channel, ioid, transport));
    ChannelPutGetRequester::shared_pointer thisPointer = tp;
    static_cast<ServerChannelPutGetRequesterImpl*>(thisPointer.get())->activate(pvRequest);
    return thisPointer;
}

void ServerChannelPutGetRequesterImpl::activate(PVStructure::shared_pointer const & pvRequest)
{
	startRequest(QOS_INIT);
	ChannelPutGetRequester::shared_pointer thisPointer = shared_from_this();
	Destroyable::shared_pointer thisDestroyable = shared_from_this();
	_channel->registerRequest(_ioid, thisDestroyable);
    INIT_EXCEPTION_GUARD(CMD_PUT_GET, _channelPutGet = _channel->getChannel()->createChannelPutGet(thisPointer, pvRequest));
}

void ServerChannelPutGetRequesterImpl::channelPutGetConnect(const Status& status, ChannelPutGet::shared_pointer const & channelPutGet,
		Structure::const_shared_pointer const & putStructure, Structure::const_shared_pointer const & getStructure)
{
	{
		Lock guard(_mutex);
		_status = status;
		_channelPutGet = channelPutGet;
        if (_status.isSuccess())
        {
           _pvPutStructure = std::tr1::static_pointer_cast<PVStructure>(reuseOrCreatePVField(putStructure, _pvPutStructure));
           _pvPutBitSet = createBitSetFor(_pvPutStructure, _pvPutBitSet);

           _pvGetStructure = std::tr1::static_pointer_cast<PVStructure>(reuseOrCreatePVField(getStructure, _pvGetStructure));
           _pvGetBitSet = createBitSetFor(_pvGetStructure, _pvGetBitSet);
        }
    }
	
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);

	// self-destruction
	if (!status.isSuccess())
	{
		destroy();
	}
}

void ServerChannelPutGetRequesterImpl::getGetDone(const Status& status, ChannelPutGet::shared_pointer const & /*channelPutGet*/,
    PVStructure::shared_pointer const & pvStructure, BitSet::shared_pointer const & bitSet)
{
	{
		Lock guard(_mutex);
		_status = status;
        if (_status.isSuccess())
        {
            *_pvGetBitSet = *bitSet;
            _pvGetStructure->copyUnchecked(*pvStructure, *_pvGetBitSet);
        }
	}
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelPutGetRequesterImpl::getPutDone(const Status& status, ChannelPutGet::shared_pointer const & /*channelPutGet*/,
    PVStructure::shared_pointer const & pvStructure, BitSet::shared_pointer const & bitSet)
{
	{
		Lock guard(_mutex);
		_status = status;
        if (_status.isSuccess())
        {
            *_pvPutBitSet = *bitSet;
            _pvPutStructure->copyUnchecked(*pvStructure, *_pvPutBitSet);
        }
    }
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelPutGetRequesterImpl::putGetDone(const Status& status, ChannelPutGet::shared_pointer const & /*channelPutGet*/,
    PVStructure::shared_pointer const & pvStructure, BitSet::shared_pointer const & bitSet)
{
	{
		Lock guard(_mutex);
		_status = status;
        if (_status.isSuccess())
        {
            *_pvGetBitSet = *bitSet;
            _pvGetStructure->copyUnchecked(*pvStructure, *_pvGetBitSet);
        }
    }
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelPutGetRequesterImpl::lock()
{
	// noop
}

void ServerChannelPutGetRequesterImpl::unlock()
{
	// noop
}

void ServerChannelPutGetRequesterImpl::destroy()
{
	// keep a reference to ourselves as the owner
	// could release its reference and we don't want to be
	// destroyed prematurely
	shared_pointer self(shared_from_this());

    // hold a reference to channelPutGet so that _channelPutGet.reset()
    // does not call ~ChannelPutGet (external code) while we are holding a lock
    ChannelPutGet::shared_pointer channelPutGet = _channelPutGet;
    {
		Lock guard(_mutex);
		_channel->unregisterRequest(_ioid);

        // asCheck
        _channel->getChannelSecuritySession()->release(_ioid);

        if (_channelPutGet)
		{
			_channelPutGet->destroy();
            _channelPutGet.reset();
        }
	}
}

ChannelPutGet::shared_pointer ServerChannelPutGetRequesterImpl::getChannelPutGet()
{
	//Lock guard(_mutex);
	return _channelPutGet;
}

PVStructure::shared_pointer ServerChannelPutGetRequesterImpl::getPutGetPVStructure()
{
	//Lock guard(_mutex);
    return _pvPutStructure;
}

BitSet::shared_pointer ServerChannelPutGetRequesterImpl::getPutGetBitSet()
{
	//Lock guard(_mutex);
    return _pvPutBitSet;
}

void ServerChannelPutGetRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	const int32 request = getPendingRequest();

    ChannelPutGet::shared_pointer channelPutGet;
    {
        Lock guard(_mutex);
        channelPutGet = _channelPutGet;
        // we must respond to QOS_INIT (e.g. creation error)
        if (!channelPutGet && !(request & QOS_INIT))
            return;
    }

    control->startMessage((int32)12, sizeof(int32)/sizeof(int8) + 1);
	buffer->putInt(_ioid);
	buffer->putByte((int8)request);
	{
		Lock guard(_mutex);
		_status.serialize(buffer, control);
	}

	if (_status.isSuccess())
	{
		if ((QOS_INIT & request) != 0)
		{
			Lock guard(_mutex);
            control->cachedSerialize(_pvPutStructure->getStructure(), buffer);
            control->cachedSerialize(_pvGetStructure->getStructure(), buffer);
		}
		else if ((QOS_GET & request) != 0)
		{
			Lock guard(_mutex);
			_pvGetBitSet->serialize(buffer, control);
			_pvGetStructure->serialize(buffer, control, _pvGetBitSet.get());
		}
		else if ((QOS_GET_PUT & request) != 0)
		{
            ScopedLock lock(channelPutGet);
			//Lock guard(_mutex);
			_pvPutBitSet->serialize(buffer, control);
			_pvPutStructure->serialize(buffer, control, _pvPutBitSet.get());
		}
		else
		{
            ScopedLock lock(channelPutGet);
			//Lock guard(_mutex);
			_pvGetBitSet->serialize(buffer, control);
			_pvGetStructure->serialize(buffer, control, _pvGetBitSet.get());
		}
	}

	stopRequest();

	// lastRequest
	if ((QOS_DESTROY & request) != 0)
		destroy();
}

/****************************************************************************************/
void ServerMonitorHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8)+1);
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	// mode
	const int8 qosCode = payloadBuffer->getByte();
	
	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		BaseChannelRequester::sendFailureMessage((int8)CMD_MONITOR, transport, ioid, qosCode, BaseChannelRequester::badCIDStatus);
		return;
	}

	const bool init = (QOS_INIT & qosCode) != 0;
	if (init)
	{
		// pvRequest
		PVStructure::shared_pointer pvRequest(SerializationHelper::deserializePVRequest(payloadBuffer, transport.get()));

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeCreateMonitor(ioid, pvRequest);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_MONITOR, transport, ioid, qosCode, asStatus);
            return;
        }

        // create...
		ServerMonitorRequesterImpl::create(_context, channel, ioid, transport, pvRequest);

        // pipelining monitor (i.e. w/ flow control)
        const bool ack = (QOS_GET_PUT & qosCode) != 0;
        if (ack)
        {
            transport->ensureData(4);
            int32 nfree = payloadBuffer->getInt();
            ServerMonitorRequesterImpl::shared_pointer request = static_pointer_cast<ServerMonitorRequesterImpl>(channel->getRequest(ioid));

            Monitor::shared_pointer mp = request->getChannelMonitor();
            PipelineMonitor* pmp = dynamic_cast<PipelineMonitor*>(mp.get());
            if (pmp)
                pmp->reportRemoteQueueStatus(nfree);
        }

	}
	else
	{
        const bool lastRequest = (QOS_DESTROY & qosCode) != 0;
        const bool get = (QOS_GET & qosCode) != 0;
        const bool process = (QOS_PROCESS & qosCode) != 0;
        const bool ack = (QOS_GET_PUT & qosCode) != 0;

        ServerMonitorRequesterImpl::shared_pointer request = static_pointer_cast<ServerMonitorRequesterImpl>(channel->getRequest(ioid));
        if (!request.get())
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_MONITOR, transport, ioid, qosCode, BaseChannelRequester::badIOIDStatus);
			return;
		}

        if (ack)
        {
            transport->ensureData(4);
            int32 nfree = payloadBuffer->getInt();
            Monitor::shared_pointer mp = request->getChannelMonitor();
            PipelineMonitor* pmp = dynamic_cast<PipelineMonitor*>(mp.get());
            if (pmp)
                pmp->reportRemoteQueueStatus(nfree);
            return;
            // note: not possible to ack and destroy
        }

        /*
		if (!request->startRequest(qosCode))
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_MONITOR, transport, ioid, qosCode, BaseChannelRequester::otherRequestPendingStatus);
			return;
		}
		*/

        // TODO for now we do a get check
        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeGet(ioid);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_MONITOR, transport, ioid, qosCode, asStatus);
            if (lastRequest)
                request->destroy();
            return;
        }

		if (process)
		{
			if (get)
				request->getChannelMonitor()->start();
			else
				request->getChannelMonitor()->stop();
			//request.stopRequest();
		}
		else if (get)
		{
			// not supported
		}

		if (lastRequest)
			request->destroy();
	}
}

ServerMonitorRequesterImpl::ServerMonitorRequesterImpl(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport):
        BaseChannelRequester(context, channel, ioid, transport), _channelMonitor(), _structure(), _unlisten(false)
{
}

MonitorRequester::shared_pointer ServerMonitorRequesterImpl::create(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport,PVStructure::shared_pointer const & pvRequest)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerMonitorRequesterImpl> tp(new ServerMonitorRequesterImpl(context, channel, ioid, transport));
    MonitorRequester::shared_pointer thisPointer = tp;
    static_cast<ServerMonitorRequesterImpl*>(thisPointer.get())->activate(pvRequest);
    return thisPointer;
}

void ServerMonitorRequesterImpl::activate(PVStructure::shared_pointer const & pvRequest)
{
    startRequest(QOS_INIT);
	MonitorRequester::shared_pointer thisPointer = shared_from_this();
	Destroyable::shared_pointer thisDestroyable = shared_from_this();
	_channel->registerRequest(_ioid, thisDestroyable);
    INIT_EXCEPTION_GUARD(CMD_MONITOR, _channelMonitor = _channel->getChannel()->createMonitor(thisPointer, pvRequest));
}

void ServerMonitorRequesterImpl::monitorConnect(const Status& status, Monitor::shared_pointer const & monitor, epics::pvData::StructureConstPtr const & structure)
{
	{
		Lock guard(_mutex);
		_status = status;
		_channelMonitor = monitor;
		_structure = structure;
	}
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);

	// self-destruction
	if (!status.isSuccess())
	{
		destroy();
	}
}

void ServerMonitorRequesterImpl::unlisten(Monitor::shared_pointer const & /*monitor*/)
{
    {
        Lock guard(_mutex);
        _unlisten = true;
    }
    TransportSender::shared_pointer thisSender = shared_from_this();
    _transport->enqueueSendRequest(thisSender);
}

void ServerMonitorRequesterImpl::monitorEvent(Monitor::shared_pointer const & /*monitor*/)
{
	// TODO !!! if queueSize==0, monitor.poll() has to be called and returned NOW (since there is no cache)
	//sendEvent(transport);

	// TODO implement via TransportSender
	/*
		// initiate submit to dispatcher queue, if necessary
		synchronized (register) {
			if (register.getAndSet(true))
				eventConsumer.consumeEvents(this);
		}*/
	// TODO
	// multiple ((BlockingServerTCPTransport)transport).enqueueMonitorSendRequest(this);
	TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerMonitorRequesterImpl::lock()
{
	//noop
}

void ServerMonitorRequesterImpl::unlock()
{
	//noop
}

void ServerMonitorRequesterImpl::destroy()
{
	// keep a reference to ourselves as the owner
	// could release its reference and we don't want to be
	// destroyed prematurely
	shared_pointer self(shared_from_this());

    // hold a reference to channelMonitor so that _channelMonitor.reset()
    // does not call ~Monitor (external code) while we are holding a lock
    Monitor::shared_pointer monitor = _channelMonitor;
    {
		Lock guard(_mutex);
		_channel->unregisterRequest(_ioid);

        // asCheck
        _channel->getChannelSecuritySession()->release(_ioid);

        if (_channelMonitor)
		{
			_channelMonitor->destroy();
            _channelMonitor.reset();
        }
	}
}

Monitor::shared_pointer ServerMonitorRequesterImpl::getChannelMonitor()
{
	//Lock guard(_mutex);
	return _channelMonitor;
}

void ServerMonitorRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	const int32 request = getPendingRequest();

	if ((QOS_INIT & request) != 0)
	{
		control->startMessage((int32)CMD_MONITOR, sizeof(int32)/sizeof(int8) + 1);
		buffer->putInt(_ioid);
		buffer->putByte((int8)request);

		{
			Lock guard(_mutex);
			_status.serialize(buffer, control);
		}

		if (_status.isSuccess())
		{
		    // valid due to _mutex lock above
			control->cachedSerialize(_structure, buffer);
		}
		stopRequest();
		startRequest(QOS_DEFAULT);
	}
	else
	{
		Monitor::shared_pointer monitor = _channelMonitor;
        if (!monitor)
            return;

        // TODO asCheck ?

		MonitorElement::shared_pointer element = monitor->poll();
        if (element.get())
		{
			control->startMessage((int8)CMD_MONITOR, sizeof(int32)/sizeof(int8) + 1);
			buffer->putInt(_ioid);
			buffer->putByte((int8)request);

			// changedBitSet and data, if not notify only (i.e. queueSize == -1)
			BitSet::shared_pointer changedBitSet = element->changedBitSet;
            if (changedBitSet)
			{
				changedBitSet->serialize(buffer, control);
				element->pvStructurePtr->serialize(buffer, control, changedBitSet.get());

				// overrunBitset
				element->overrunBitSet->serialize(buffer, control);
			}

			monitor->release(element);
			
			// TODO if we try to proces several monitors at once, then fairness suffers
			// TODO compbine several monitors into one message (reduces payload)
        	TransportSender::shared_pointer thisSender = shared_from_this();
        	_transport->enqueueSendRequest(thisSender);
		}
        else
        {
            // TODO CAS
            bool unlisten;
            Lock guard(_mutex);
            unlisten = _unlisten;
            _unlisten = false;
            guard.unlock();

            if (unlisten)
            {
                control->startMessage((int8)CMD_MONITOR, sizeof(int32)/sizeof(int8) + 1);
                buffer->putInt(_ioid);
                buffer->putByte((int8)QOS_DESTROY);
                Status::Ok.serialize(buffer, control);
            }
        }
		
	}
}

/****************************************************************************************/
void ServerArrayHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8)+1);
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	// mode
	const int8 qosCode = payloadBuffer->getByte();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, BaseChannelRequester::badCIDStatus);
		return;
	}

	const bool init = (QOS_INIT & qosCode) != 0;
	if (init)
	{
		// pvRequest
		PVStructure::shared_pointer pvRequest(SerializationHelper::deserializePVRequest(payloadBuffer, transport.get()));

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeCreateChannelArray(ioid, pvRequest);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, asStatus);
            return;
        }

        // create...
		ServerChannelArrayRequesterImpl::create(_context, channel, ioid, transport, pvRequest);
	}
	else
	{
		const bool lastRequest = (QOS_DESTROY & qosCode) != 0;
		const bool get = (QOS_GET & qosCode) != 0;
		const bool setLength = (QOS_GET_PUT & qosCode) != 0;
		const bool getLength = (QOS_PROCESS & qosCode) != 0;

		ServerChannelArrayRequesterImpl::shared_pointer request = static_pointer_cast<ServerChannelArrayRequesterImpl>(channel->getRequest(ioid));
        if (!request.get())
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, BaseChannelRequester::badIOIDStatus);
			return;
		}

		if (!request->startRequest(qosCode))
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, BaseChannelRequester::otherRequestPendingStatus);
			return;
		}

   	    ChannelArray::shared_pointer channelArray = request->getChannelArray();
        if (lastRequest)
            channelArray->lastRequest();
            
		if (get)
		{
            size_t offset = SerializeHelper::readSize(payloadBuffer, transport.get());
            size_t count = SerializeHelper::readSize(payloadBuffer, transport.get());
            size_t stride = SerializeHelper::readSize(payloadBuffer, transport.get());

            // asCheck
            Status asStatus = channel->getChannelSecuritySession()->authorizeGet(ioid);
            if (!asStatus.isSuccess())
            {
                BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, asStatus);
                if (lastRequest)
                    request->destroy();
                return;
            }

			request->getChannelArray()->getArray(offset, count, stride);
		}
		else if (setLength)
		{
            size_t length = SerializeHelper::readSize(payloadBuffer, transport.get());

            // asCheck
            Status asStatus = channel->getChannelSecuritySession()->authorizeSetLength(ioid);
            if (!asStatus.isSuccess())
            {
                BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, asStatus);
                if (lastRequest)
                    request->destroy();
                return;
            }

            request->getChannelArray()->setLength(length);
		}
		else if (getLength)
		{
            // asCheck
            Status asStatus = channel->getChannelSecuritySession()->authorizeGet(ioid);
            if (!asStatus.isSuccess())
            {
                BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, asStatus);
                if (lastRequest)
                    request->destroy();
                return;
            }

            request->getChannelArray()->getLength();
		}
		else
		{
			// deserialize data to put
            size_t offset;
            size_t stride;
    	    PVArray::shared_pointer array = request->getPVArray();
			{
    			ScopedLock lock(channelArray);   // TODO not needed if read by the same thread
    			
    			DESERIALIZE_EXCEPTION_GUARD(
    			    offset = SerializeHelper::readSize(payloadBuffer, transport.get());
    			    stride = SerializeHelper::readSize(payloadBuffer, transport.get());
    			    array->deserialize(payloadBuffer, transport.get());
    			);
			}

            // asCheck
            Status asStatus = channel->getChannelSecuritySession()->authorizePut(ioid, array);
            if (!asStatus.isSuccess())
            {
                BaseChannelRequester::sendFailureMessage((int8)CMD_ARRAY, transport, ioid, qosCode, asStatus);
                if (lastRequest)
                    request->destroy();
                return;
            }

			channelArray->putArray(array, offset, array->getLength(), stride);
		}
	}
}

ServerChannelArrayRequesterImpl::ServerChannelArrayRequesterImpl(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport):
		BaseChannelRequester(context, channel, ioid, transport)
{
}

ChannelArrayRequester::shared_pointer ServerChannelArrayRequesterImpl::create(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport,PVStructure::shared_pointer const & pvRequest)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerChannelArrayRequesterImpl> tp(new ServerChannelArrayRequesterImpl(context, channel, ioid, transport));
    ChannelArrayRequester::shared_pointer thisPointer = tp;
    static_cast<ServerChannelArrayRequesterImpl*>(thisPointer.get())->activate(pvRequest);
    return thisPointer;
}

void ServerChannelArrayRequesterImpl::activate(PVStructure::shared_pointer const & pvRequest)
{
	startRequest(QOS_INIT);
	ChannelArrayRequester::shared_pointer thisPointer = shared_from_this();
	Destroyable::shared_pointer thisDestroyable = shared_from_this();
	_channel->registerRequest(_ioid, thisDestroyable);
    INIT_EXCEPTION_GUARD(CMD_ARRAY, _channelArray = _channel->getChannel()->createChannelArray(thisPointer, pvRequest));
}

void ServerChannelArrayRequesterImpl::channelArrayConnect(const Status& status, ChannelArray::shared_pointer const & channelArray, Array::const_shared_pointer const & array)
{
    if (status.isSuccess() && array->getArraySizeType() == Array::fixed)
    {
        Lock guard(_mutex);
        _status = Status(Status::STATUSTYPE_ERROR, "fixed sized array returned as a ChannelArray array instance");
        _channelArray.reset();
        _pvArray.reset();
    }
    else
    {
		Lock guard(_mutex);
		_status = status;
		_channelArray = channelArray;
        if (_status.isSuccess())
        {
           _pvArray = std::tr1::static_pointer_cast<PVArray>(reuseOrCreatePVField(array, _pvArray));
        }
    }
	
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);

	// self-destruction
	if (!status.isSuccess())
	{
		destroy();
	}
}

void ServerChannelArrayRequesterImpl::getArrayDone(const Status& status, ChannelArray::shared_pointer const & /*channelArray*/, PVArray::shared_pointer const & pvArray)
{
	{
		Lock guard(_mutex);
		_status = status;
        if (_status.isSuccess())
        {
            _pvArray->copyUnchecked(*pvArray);
        }
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelArrayRequesterImpl::putArrayDone(const Status& status, ChannelArray::shared_pointer const & /*channelArray*/)
{
	{
		Lock guard(_mutex);
		_status = status;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelArrayRequesterImpl::setLengthDone(const Status& status, ChannelArray::shared_pointer const & /*channelArray*/)
{
	{
		Lock guard(_mutex);
		_status = status;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelArrayRequesterImpl::getLengthDone(const Status& status, ChannelArray::shared_pointer const & /*channelArray*/,
    size_t length)
{
	{
		Lock guard(_mutex);
		_status = status;
		_length = length;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelArrayRequesterImpl::lock()
{
	// noop
}

void ServerChannelArrayRequesterImpl::unlock()
{
	// noop
}

void ServerChannelArrayRequesterImpl::destroy()
{
	// keep a reference to ourselves as the owner
	// could release its reference and we don't want to be
	// destroyed prematurely
	shared_pointer self(shared_from_this());

    // hold a reference to channelArray so that _channelArray.reset()
    // does not call ~ChannelArray (external code) while we are holding a lock
    ChannelArray::shared_pointer channelArray = _channelArray;
    {
		Lock guard(_mutex);
		_channel->unregisterRequest(_ioid);

        // asCheck
        _channel->getChannelSecuritySession()->release(_ioid);

        if (_channelArray)
		{
			_channelArray->destroy();
            _channelArray.reset();
        }
	}
}

ChannelArray::shared_pointer ServerChannelArrayRequesterImpl::getChannelArray()
{
	//Lock guard(_mutex);
	return _channelArray;
}

PVArray::shared_pointer ServerChannelArrayRequesterImpl::getPVArray()
{
	//Lock guard(_mutex);
    return _pvArray;
}

void ServerChannelArrayRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	const int32 request = getPendingRequest();

    ChannelArray::shared_pointer channelArray;
    {
        Lock guard(_mutex);
        channelArray = _channelArray;
        // we must respond to QOS_INIT (e.g. creation error)
        if (!channelArray && !(request & QOS_INIT))
            return;
    }

    control->startMessage((int32)CMD_ARRAY, sizeof(int32)/sizeof(int8) + 1);
	buffer->putInt(_ioid);
	buffer->putByte((int8)request);
	{
		Lock guard(_mutex);
		_status.serialize(buffer, control);
	}

	if (_status.isSuccess())
	{
		if ((QOS_GET & request) != 0)
		{
			//Lock guard(_mutex);
            ScopedLock lock(channelArray);
			_pvArray->serialize(buffer, control, 0, _pvArray->getLength());
		}
		else if ((QOS_PROCESS & request) != 0)
		{
			//Lock guard(_mutex);
            SerializeHelper::writeSize(_length, buffer, control);
		}
		else if ((QOS_INIT & request) != 0)
		{
			Lock guard(_mutex);
            control->cachedSerialize(_pvArray->getArray(), buffer);
		}
	}

	stopRequest();

	// lastRequest
	if ((QOS_DESTROY & request) != 0)
		destroy();
}

/****************************************************************************************/
void ServerDestroyRequestHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8));
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		failureResponse(transport, ioid, BaseChannelRequester::badCIDStatus);
		return;
	}

	Destroyable::shared_pointer request = channel->getRequest(ioid);
    if (!request.get())
	{
		failureResponse(transport, ioid, BaseChannelRequester::badIOIDStatus);
		return;
	}

	// destroy
	request->destroy();

	// ... and remove from channel
	channel->unregisterRequest(ioid);
}

void ServerDestroyRequestHandler::failureResponse(Transport::shared_pointer const & transport, pvAccessID ioid, const Status& errorStatus)
{
	BaseChannelRequester::message(transport, ioid, errorStatus.getMessage(), warningMessage);
}

/****************************************************************************************/
void ServerCancelRequestHandler::handleResponse(osiSockAddr* responseFrom,
        Transport::shared_pointer const & transport, int8 version, int8 command,
        size_t payloadSize, ByteBuffer* payloadBuffer) {
    AbstractServerResponseHandler::handleResponse(responseFrom,
            transport, version, command, payloadSize, payloadBuffer);

    // NOTE: we do not explicitly check if transport is OK
    ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

    transport->ensureData(2*sizeof(int32)/sizeof(int8));
    const pvAccessID sid = payloadBuffer->getInt();
    const pvAccessID ioid = payloadBuffer->getInt();

    ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
    {
        failureResponse(transport, ioid, BaseChannelRequester::badCIDStatus);
        return;
    }

    Destroyable::shared_pointer request = channel->getRequest(ioid);
    if (!request.get())
    {
        failureResponse(transport, ioid, BaseChannelRequester::badIOIDStatus);
        return;
    }

    ChannelRequest::shared_pointer cr = dynamic_pointer_cast<ChannelRequest>(request);
    if (!cr.get())
    {
        failureResponse(transport, ioid, BaseChannelRequester::notAChannelRequestStatus);
        return;
    }

    // cancel
    cr->cancel();

}

void ServerCancelRequestHandler::failureResponse(Transport::shared_pointer const & transport, pvAccessID ioid, const Status& errorStatus)
{
    BaseChannelRequester::message(transport, ioid, errorStatus.getMessage(), warningMessage);
}

/****************************************************************************************/
void ServerProcessHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8)+1);
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	// mode
	const int8 qosCode = payloadBuffer->getByte();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		BaseChannelRequester::sendFailureMessage((int8)CMD_PROCESS, transport, ioid, qosCode, BaseChannelRequester::badCIDStatus);
		return;
	}

	const bool init = (QOS_INIT & qosCode) != 0;
	if (init)
	{
		// pvRequest
		PVStructure::shared_pointer pvRequest(SerializationHelper::deserializePVRequest(payloadBuffer, transport.get()));

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeCreateChannelProcess(ioid, pvRequest);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_PROCESS, transport, ioid, qosCode, asStatus);
            return;
        }

        // create...
		ServerChannelProcessRequesterImpl::create(_context, channel, ioid, transport, pvRequest);
	}
	else
	{
		const bool lastRequest = (QOS_DESTROY & qosCode) != 0;

		ServerChannelProcessRequesterImpl::shared_pointer request = static_pointer_cast<ServerChannelProcessRequesterImpl>(channel->getRequest(ioid));
        if (!request.get())
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_PROCESS, transport, ioid, qosCode, BaseChannelRequester::badIOIDStatus);
			return;
		}

		if (!request->startRequest(qosCode))
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_PROCESS, transport, ioid, qosCode, BaseChannelRequester::otherRequestPendingStatus);
			return;
		}

        if (lastRequest)
            request->getChannelProcess()->lastRequest();

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeProcess(ioid);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_PROCESS, transport, ioid, qosCode, asStatus);
            if (lastRequest)
                request->destroy();
            return;
        }

		request->getChannelProcess()->process();
	}
}

ServerChannelProcessRequesterImpl::ServerChannelProcessRequesterImpl(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport):
		BaseChannelRequester(context, channel, ioid, transport), _channelProcess()
{
}

ChannelProcessRequester::shared_pointer ServerChannelProcessRequesterImpl::create(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport,PVStructure::shared_pointer const & pvRequest)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerChannelProcessRequesterImpl> tp(new ServerChannelProcessRequesterImpl(context, channel, ioid, transport));
    ChannelProcessRequester::shared_pointer thisPointer = tp;
    static_cast<ServerChannelProcessRequesterImpl*>(thisPointer.get())->activate(pvRequest);
    return thisPointer;
}

void ServerChannelProcessRequesterImpl::activate(PVStructure::shared_pointer const & pvRequest)
{
	startRequest(QOS_INIT);
	ChannelProcessRequester::shared_pointer thisPointer = shared_from_this();
	Destroyable::shared_pointer thisDestroyable = shared_from_this();
	_channel->registerRequest(_ioid, thisDestroyable);
    INIT_EXCEPTION_GUARD(CMD_PROCESS, _channelProcess = _channel->getChannel()->createChannelProcess(thisPointer, pvRequest));
}

void ServerChannelProcessRequesterImpl::channelProcessConnect(const Status& status, ChannelProcess::shared_pointer const & channelProcess)
{
	{
		Lock guard(_mutex);
		_status = status;
		_channelProcess = channelProcess;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);

	// self-destruction
	if (!status.isSuccess())
	{
		destroy();
	}
}

void ServerChannelProcessRequesterImpl::processDone(const Status& status, ChannelProcess::shared_pointer const & /*channelProcess*/)
{
	{
		Lock guard(_mutex);
		_status = status;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelProcessRequesterImpl::lock()
{
	// noop
}

void ServerChannelProcessRequesterImpl::unlock()
{
	// noop
}

void ServerChannelProcessRequesterImpl::destroy()
{
	// keep a reference to ourselves as the owner
	// could release its reference and we don't want to be
	// destroyed prematurely
	shared_pointer self(shared_from_this());

	{
		Lock guard(_mutex);
		_channel->unregisterRequest(_ioid);

        // asCheck
        _channel->getChannelSecuritySession()->release(_ioid);

        if (_channelProcess.get())
		{
			_channelProcess->destroy();
		}
	}
	// TODO
	_channelProcess.reset();
}

ChannelProcess::shared_pointer ServerChannelProcessRequesterImpl::getChannelProcess()
{
	//Lock guard(_mutex);
	return _channelProcess;
}

void ServerChannelProcessRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	const int32 request = getPendingRequest();

	control->startMessage((int32)CMD_PROCESS, sizeof(int32)/sizeof(int8) + 1);
	buffer->putInt(_ioid);
	buffer->putByte((int8)request);
	{
		Lock guard(_mutex);
		_status.serialize(buffer, control);
	}

	stopRequest();

	// lastRequest
	if ((QOS_DESTROY & request) != 0)
	{
		destroy();
	}
}


/****************************************************************************************/
void ServerGetFieldHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8));
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		getFieldFailureResponse(transport, ioid, BaseChannelRequester::badCIDStatus);
		return;
	}

	string subField = SerializeHelper::deserializeString(payloadBuffer, transport.get());

	// issue request
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerGetFieldRequesterImpl> tp(new ServerGetFieldRequesterImpl(_context, channel, ioid, transport));
    GetFieldRequester::shared_pointer gfr = tp;

    // asCheck
    Status asStatus = channel->getChannelSecuritySession()->authorizeGetField(ioid, subField);
    if (!asStatus.isSuccess())
    {
        gfr->getDone(asStatus, FieldConstPtr());
        return;
    }

    // TODO exception check
	channel->getChannel()->getField(gfr, subField);
}

void ServerGetFieldHandler::getFieldFailureResponse(Transport::shared_pointer const & transport, const pvAccessID ioid, const Status& errorStatus)
{
    TransportSender::shared_pointer sender(new ServerGetFieldHandlerTransportSender(ioid,errorStatus,transport));
	transport->enqueueSendRequest(sender);
}

ServerGetFieldRequesterImpl::ServerGetFieldRequesterImpl(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
        const pvAccessID ioid, Transport::shared_pointer const & transport) :
		BaseChannelRequester(context, channel, ioid, transport), _field()
{
}

void ServerGetFieldRequesterImpl::getDone(const Status& status, FieldConstPtr const & field)
{
	{
		Lock guard(_mutex);
		_status = status;
		_field = field;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerGetFieldRequesterImpl::lock()
{
	//noop
}

void ServerGetFieldRequesterImpl::unlock()
{
	//noop
}

void ServerGetFieldRequesterImpl::destroy()
{
}

void ServerGetFieldRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	control->startMessage((int8)CMD_GET_FIELD, sizeof(int32)/sizeof(int8));
	buffer->putInt(_ioid);
	{
		Lock guard(_mutex);
		_status.serialize(buffer, control);
		if (_status.isSuccess())
		  control->cachedSerialize(_field, buffer);
	}
}

/****************************************************************************************/
void ServerRPCHandler::handleResponse(osiSockAddr* responseFrom,
		Transport::shared_pointer const & transport, int8 version, int8 command,
		size_t payloadSize, ByteBuffer* payloadBuffer) {
	AbstractServerResponseHandler::handleResponse(responseFrom,
			transport, version, command, payloadSize, payloadBuffer);

	// NOTE: we do not explicitly check if transport is OK
	ChannelHostingTransport::shared_pointer casTransport = dynamic_pointer_cast<ChannelHostingTransport>(transport);

	transport->ensureData(2*sizeof(int32)/sizeof(int8)+1);
	const pvAccessID sid = payloadBuffer->getInt();
	const pvAccessID ioid = payloadBuffer->getInt();

	// mode
	const int8 qosCode = payloadBuffer->getByte();

	ServerChannelImpl::shared_pointer channel = static_pointer_cast<ServerChannelImpl>(casTransport->getChannel(sid));
    if (!channel.get())
	{
		BaseChannelRequester::sendFailureMessage((int8)CMD_RPC, transport, ioid, qosCode, BaseChannelRequester::badCIDStatus);
		return;
	}

	const bool init = (QOS_INIT & qosCode) != 0;
	if (init)
	{
		// pvRequest
		PVStructure::shared_pointer pvRequest(SerializationHelper::deserializePVRequest(payloadBuffer, transport.get()));

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeCreateChannelRPC(ioid, pvRequest);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_RPC, transport, ioid, qosCode, asStatus);
            return;
        }

        // create...
		ServerChannelRPCRequesterImpl::create(_context, channel, ioid, transport, pvRequest);
	}
	else
	{
		const bool lastRequest = (QOS_DESTROY & qosCode) != 0;

		ServerChannelRPCRequesterImpl::shared_pointer request = static_pointer_cast<ServerChannelRPCRequesterImpl>(channel->getRequest(ioid));
        if (!request.get())
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_RPC, transport, ioid, qosCode, BaseChannelRequester::badIOIDStatus);
			return;
		}

		if (!request->startRequest(qosCode))
		{
			BaseChannelRequester::sendFailureMessage((int8)CMD_RPC, transport, ioid, qosCode, BaseChannelRequester::otherRequestPendingStatus);
			return;
		}

		// deserialize put data
		ChannelRPC::shared_pointer channelRPC = request->getChannelRPC();
		// pvArgument
		PVStructure::shared_pointer pvArgument;
		
        DESERIALIZE_EXCEPTION_GUARD(
            pvArgument = SerializationHelper::deserializeStructureFull(payloadBuffer, transport.get());
        );
        
        if (lastRequest)
            channelRPC->lastRequest();

        // asCheck
        Status asStatus = channel->getChannelSecuritySession()->authorizeRPC(ioid, pvArgument);
        if (!asStatus.isSuccess())
        {
            BaseChannelRequester::sendFailureMessage((int8)CMD_RPC, transport, ioid, qosCode, asStatus);
            if (lastRequest)
                request->destroy();
            return;
        }

        channelRPC->request(pvArgument);
	}
}

ServerChannelRPCRequesterImpl::ServerChannelRPCRequesterImpl(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport):
        BaseChannelRequester(context, channel, ioid, transport),
        _channelRPC(), _pvResponse()

{
}

ChannelRPCRequester::shared_pointer ServerChannelRPCRequesterImpl::create(
        ServerContextImpl::shared_pointer const & context, ServerChannelImpl::shared_pointer const & channel,
		const pvAccessID ioid, Transport::shared_pointer const & transport, PVStructure::shared_pointer const & pvRequest)
{
    // TODO use std::make_shared
    std::tr1::shared_ptr<ServerChannelRPCRequesterImpl> tp(new ServerChannelRPCRequesterImpl(context, channel, ioid, transport));
    ChannelRPCRequester::shared_pointer thisPointer = tp;
    static_cast<ServerChannelRPCRequesterImpl*>(thisPointer.get())->activate(pvRequest);
    return thisPointer;
}

void ServerChannelRPCRequesterImpl::activate(PVStructure::shared_pointer const & pvRequest)
{
	startRequest(QOS_INIT);
	ChannelRPCRequester::shared_pointer thisPointer = shared_from_this();
	Destroyable::shared_pointer thisDestroyable = shared_from_this();
	_channel->registerRequest(_ioid, thisDestroyable);
    INIT_EXCEPTION_GUARD(CMD_RPC, _channelRPC = _channel->getChannel()->createChannelRPC(thisPointer, pvRequest));
}

void ServerChannelRPCRequesterImpl::channelRPCConnect(const Status& status, ChannelRPC::shared_pointer const & channelRPC)
{
	{
		Lock guard(_mutex);
		_status = status;
		_channelRPC = channelRPC;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);

	// self-destruction
	if (!status.isSuccess())
	{
		destroy();
	}
}

void ServerChannelRPCRequesterImpl::requestDone(const Status& status, ChannelRPC::shared_pointer const & /*channelRPC*/, PVStructure::shared_pointer const & pvResponse)
{
	{
		Lock guard(_mutex);
		_status = status;
		_pvResponse = pvResponse;
	}
    TransportSender::shared_pointer thisSender = shared_from_this();
	_transport->enqueueSendRequest(thisSender);
}

void ServerChannelRPCRequesterImpl::lock()
{
	// noop
}

void ServerChannelRPCRequesterImpl::unlock()
{
	// noop
}

void ServerChannelRPCRequesterImpl::destroy()
{
	// keep a reference to ourselves as the owner
	// could release its reference and we don't want to be
	// destroyed prematurely
	shared_pointer self(shared_from_this());

	{
		Lock guard(_mutex);
		_channel->unregisterRequest(_ioid);

        // asCheck
        _channel->getChannelSecuritySession()->release(_ioid);

        if (_channelRPC.get())
		{
			_channelRPC->destroy();
		}
	}
	// TODO
	_channelRPC.reset();
}

ChannelRPC::shared_pointer ServerChannelRPCRequesterImpl::getChannelRPC()
{
	//Lock guard(_mutex);
	return _channelRPC;
}

void ServerChannelRPCRequesterImpl::send(ByteBuffer* buffer, TransportSendControl* control)
{
	const int32 request = getPendingRequest();

	control->startMessage((int32)CMD_RPC, sizeof(int32)/sizeof(int8) + 1);
	buffer->putInt(_ioid);
	buffer->putByte((int8)request);
	
	{
    	Lock guard(_mutex);
		_status.serialize(buffer, control);

    	if (_status.isSuccess())
    	{
    		if ((QOS_INIT & request) != 0)
    		{
    		    // noop
    		}
    		else
    		{
    			SerializationHelper::serializeStructureFull(buffer, control, _pvResponse);
    		}
    	}
	}
	
	stopRequest();

	// lastRequest
	if ((QOS_DESTROY & request) != 0)
		destroy();
}

}
}
