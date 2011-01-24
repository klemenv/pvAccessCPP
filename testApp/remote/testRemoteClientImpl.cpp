
/* testRemoteClientImpl.cpp */
/* Author:  Matej Sekoranja Date: 2011.1.1 */


#include <pvAccess.h>
#include <iostream>
#include <sstream>
#include <showConstructDestruct.h>
#include <lock.h>ƒ
#include <standardPVField.h>
#include <memory>

#include <caConstants.h>
#include <timer.h>
#include <blockingUDP.h>
#include <blockingTCP.h>
#include <namedLockPattern.h>
#include <inetAddressUtil.h>
#include <hexDump.h>
#include <remote.h>
#include <channelSearchManager.h>
#include <clientContextImpl.h>
#include <configuration.h>
#include <beaconHandler.h>
#include <errlog.h>

using namespace epics::pvData;
using namespace epics::pvAccess;

static Status* g_statusOK = getStatusCreate()->getStatusOK();



    static StatusCreate* statusCreate = getStatusCreate();
    static Status* okStatus = g_statusOK;
    static Status* destroyedStatus = statusCreate->createStatus(STATUSTYPE_ERROR, "request destroyed");
    static Status* channelNotConnected = statusCreate->createStatus(STATUSTYPE_ERROR, "channel not connected");
    static Status* otherRequestPendingStatus = statusCreate->createStatus(STATUSTYPE_ERROR, "other request pending");
    static PVDataCreate* pvDataCreate = getPVDataCreate();

// TODO
Status* ChannelImpl::channelDestroyed = statusCreate->createStatus(STATUSTYPE_WARNING, "channel destroyed");
Status* ChannelImpl::channelDisconnected = statusCreate->createStatus(STATUSTYPE_WARNING, "channel disconnected");;

/**
 * Base channel request.
 * @author <a href="mailto:matej.sekoranjaATcosylab.com">Matej Sekoranja</a>
 * @version $Id: BaseRequestImpl.java,v 1.1 2010/05/03 14:45:40 mrkraimer Exp $
 */
class BaseRequestImpl : 
    public DataResponse,
    public SubscriptionRequest,
    public TransportSender {
    protected:
    
	ChannelImpl* m_channel;
	ClientContextImpl* m_context;

	pvAccessID m_ioid;

	Requester* m_requester;

// TODO sync
	volatile bool m_destroyed; 
	volatile bool m_remotelyDestroyed;
	
	/* negative... */
	static const int NULL_REQUEST = -1;
	static const int PURE_DESTROY_REQUEST = -2;

	int32 m_pendingRequest;
	
	Mutex m_mutex;

public:
	BaseRequestImpl(ChannelImpl* channel, Requester* requester) :
	   m_channel(channel), m_context(channel->getContext()),
	   m_requester(requester), m_destroyed(false), m_remotelyDestroyed(false),
	   m_pendingRequest(NULL_REQUEST)
	{
		// register response request
		m_ioid = m_context->registerResponseRequest(this);
		channel->registerResponseRequest(this);
	}

	bool startRequest(int32 qos) {
	    Lock guard(&m_mutex);
	   
    	// we allow pure destroy...
    	if (m_pendingRequest != NULL_REQUEST && qos != PURE_DESTROY_REQUEST)
    		return false;
    	
    	m_pendingRequest = qos;
    	return true;
	}
	
	void stopRequest() {
	   Lock guard(&m_mutex);
       m_pendingRequest = NULL_REQUEST;
	}
	
	int32 getPendingRequest() {
	   Lock guard(&m_mutex);
	   return m_pendingRequest;
	}

	Requester* getRequester() {
		return m_requester;
	}

	pvAccessID getIOID() {
		return m_ioid;
	}

	virtual bool initResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) = 0;
	virtual bool destroyResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) = 0;
	virtual bool normalResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) = 0;
	
	virtual void response(Transport* transport, int8 version, ByteBuffer* payloadBuffer) {
// TODO?
//		try
//		{	
			transport->ensureData(1);
			int8 qos = payloadBuffer->getByte();
			Status* status = statusCreate->deserializeStatus(payloadBuffer, transport);
			if (qos & QOS_INIT)
			{
				initResponse(transport, version, payloadBuffer, qos, status);
			}
			else if (qos & QOS_DESTROY)
			{
				m_remotelyDestroyed = true;

				if (!destroyResponse(transport, version, payloadBuffer, qos, status))
				    cancel();
			}
			else
			{
				normalResponse(transport, version, payloadBuffer, qos, status);
			}
			
			// TODO
			if (status != okStatus)
			 delete status;
	}

	virtual void cancel() {
		destroy();
	}

	virtual void destroy() {
		
		{
    	   Lock guard(&m_mutex);
			if (m_destroyed)
				return;
			m_destroyed = true;
		}

		// unregister response request
		m_context->unregisterResponseRequest(this);
		m_channel->unregisterResponseRequest(this);

		// destroy remote instance
		if (!m_remotelyDestroyed)
		{
// TODO !!!			startRequest(PURE_DESTROY_REQUEST);
/// TODO 	!!! causes crash		m_channel->checkAndGetTransport()->enqueueSendRequest(this);
		}
		
	}
	
	virtual void timeout() {
		cancel();
		// TODO notify?
	}

	void reportStatus(Status* status) {
		// destroy, since channel (parent) was destroyed
		if (status == ChannelImpl::channelDestroyed)
			destroy();
		else if (status == ChannelImpl::channelDisconnected)
			stopRequest();
		// TODO notify?
	}

	virtual void updateSubscription() {
		// default is noop
	}
	
	virtual void lock() {
		// noop
	}
	
	virtual void send(ByteBuffer* buffer, TransportSendControl* control) {
		int8 qos = getPendingRequest();
		if (qos == -1)
			return;
		else if (qos == PURE_DESTROY_REQUEST)
		{
			control->startMessage((int8)15, 8);
			buffer->putInt(m_channel->getServerChannelID());
			buffer->putInt(m_ioid);
		}
		stopRequest();
	}

	virtual void unlock() {
		// noop
	}

};










PVDATA_REFCOUNT_MONITOR_DEFINE(channelProcess);

class ChannelProcessRequestImpl : public BaseRequestImpl, public ChannelProcess
{
    private:
		ChannelProcessRequester* m_callback;
		PVStructure* m_pvRequest;

    private:
    ~ChannelProcessRequestImpl()
    {
        PVDATA_REFCOUNT_MONITOR_DESTRUCT(channelProcess);
    }

    public:
    ChannelProcessRequestImpl(ChannelImpl* channel, ChannelProcessRequester* callback, PVStructure *pvRequest) :
        BaseRequestImpl(channel, callback),
        m_callback(callback), m_pvRequest(pvRequest)
        //(dynamic_cast<PVStructure*>(getPVDataCreate()->createPVField(0, "", pvRequest)))
    {
        PVDATA_REFCOUNT_MONITOR_CONSTRUCT(channelProcess);

        // TODO check for nulls!!!!

		// TODO best-effort support

		// subscribe
//		try {
			resubscribeSubscription(channel->checkAndGetTransport());
/*		} catch (IllegalStateException ise) {
			callback.channelProcessConnect(channelNotConnected, null);
		} catch (CAException e) {
			callback.channelProcessConnect(statusCreate.createStatus(StatusType.ERROR, "failed to sent message over network", e), null);
		}*/
    }

	virtual void send(ByteBuffer* buffer, TransportSendControl* control) {
		int32 pendingRequest = getPendingRequest();
		if (pendingRequest < 0)
		{
			BaseRequestImpl::send(buffer, control);
			return;
		}
		
		control->startMessage((int8)16, 9);
		buffer->putInt(m_channel->getServerChannelID());
		buffer->putInt(m_ioid);
		buffer->putByte((int8)m_pendingRequest);

		if (pendingRequest & QOS_INIT)
		{
			// pvRequest
			m_channel->getTransport()->getIntrospectionRegistry()->serializePVRequest(buffer, control, m_pvRequest);
		}
		
		stopRequest();
	}

	virtual bool destroyResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		m_callback->processDone(status);
		return true;
	}

	virtual bool initResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		m_callback->channelProcessConnect(status, this);
		return true;
	}

	virtual bool normalResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		m_callback->processDone(status);
		return true;
	}

    virtual void process(bool lastRequest)
    {
        // TODO sync
		if (m_destroyed) {
			m_callback->processDone(destroyedStatus);
			return;
		}
		
		if (!startRequest(lastRequest ? QOS_DESTROY : QOS_DEFAULT)) {
			m_callback->processDone(otherRequestPendingStatus);
			return;
		}
		
		//try {
			m_channel->checkAndGetTransport()->enqueueSendRequest(this);
		//} catch (IllegalStateException ise) {
		//	m_callback->processDone(channelNotConnected);
		//}
    }

	virtual void resubscribeSubscription(Transport* transport) {
		startRequest(QOS_INIT);
		transport->enqueueSendRequest(this);
	}

    virtual void destroy()
    {
        BaseRequestImpl::destroy();
        if (m_pvRequest) delete m_pvRequest;
        delete this;
    }

};







PVDATA_REFCOUNT_MONITOR_DEFINE(channelGet);

class ChannelGetImpl : public BaseRequestImpl, public ChannelGet
{
    private:
		ChannelGetRequester* m_channelGetRequester;
		
	    PVStructure* m_pvRequest;
		
		PVStructure* m_data;
		BitSet* m_bitSet;

    private:
    ~ChannelGetImpl()
    {
        PVDATA_REFCOUNT_MONITOR_DESTRUCT(channelGet);
    }

    public:
    ChannelGetImpl(ChannelImpl* channel, ChannelGetRequester* channelGetRequester, PVStructure *pvRequest) :
        BaseRequestImpl(channel, channelGetRequester),
        m_channelGetRequester(channelGetRequester), m_pvRequest(pvRequest),
        //(dynamic_cast<PVStructure*>(getPVDataCreate()->createPVField(0, "", pvRequest))), 
        m_data(0), m_bitSet(0)
    {
        PVDATA_REFCOUNT_MONITOR_CONSTRUCT(channelGet);

		// TODO immediate get, i.e. get data with init message
		// TODO one-time get, i.e. immediate get + lastRequest 

		// subscribe
//		try {
			resubscribeSubscription(m_channel->checkAndGetTransport());
//		} catch (IllegalStateException ise) {
// TODO			m_channelGetRequester->channelGetConnect(channelNotConnected, null, null, null);
//		} catch (CAException caex) {
// TODO			m_channelGetRequester->channelGetConnect(statusCreate.createStatus(StatusType.ERROR, "failed to sent message over network", caex), null, null, null);
//		}

    }

	virtual void send(ByteBuffer* buffer, TransportSendControl* control) {
		int32 pendingRequest = getPendingRequest();
		if (pendingRequest < 0)
		{
			BaseRequestImpl::send(buffer, control);
			return;
		}
		
		control->startMessage((int8)10, 9);
		buffer->putInt(m_channel->getServerChannelID());
		buffer->putInt(m_ioid);
		buffer->putByte((int8)m_pendingRequest);
		
		if (pendingRequest & QOS_INIT)
		{
			// pvRequest
			m_channel->getTransport()->getIntrospectionRegistry()->serializePVRequest(buffer, control, m_pvRequest);
		}
		
		stopRequest();
	}

	virtual bool destroyResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		// data available
		if (qos & QOS_GET)
			normalResponse(transport, version, payloadBuffer, qos, status);
		return true;
	}

	virtual bool initResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		if (!status->isSuccess())
		{
			m_channelGetRequester->channelGetConnect(status, this, 0, 0);
			return true;
		}

		// create data and its bitSet
		m_data = transport->getIntrospectionRegistry()->deserializeStructureAndCreatePVStructure(payloadBuffer, transport);
		m_bitSet = new BitSet(m_data->getNumberFields());

		// notify
		m_channelGetRequester->channelGetConnect(okStatus, this, m_data, m_bitSet);
		return true;
	}

	virtual bool normalResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		if (!status->isSuccess())
		{
			m_channelGetRequester->getDone(status);
			return true;
		}
		
		// deserialize bitSet and data
		m_bitSet->deserialize(payloadBuffer, transport);
		m_data->deserialize(payloadBuffer, transport, m_bitSet);
		
		m_channelGetRequester->getDone(okStatus);
		return true;
	}

    virtual void get(bool lastRequest) {
        // TODO sync?
        
		if (m_destroyed) {
			m_channelGetRequester->getDone(destroyedStatus);
			return;
		}

		if (!startRequest(lastRequest ? QOS_DESTROY | QOS_GET : QOS_DEFAULT)) {
			m_channelGetRequester->getDone(otherRequestPendingStatus);
			return;
		}
		
		//try {
			m_channel->checkAndGetTransport()->enqueueSendRequest(this);
		//} catch (IllegalStateException ise) {
	//TODO	//	m_channelGetRequester->getDone(channelNotConnected);
		//}
	}
	
	virtual void resubscribeSubscription(Transport* transport) {
		startRequest(QOS_INIT);
		transport->enqueueSendRequest(this);
	}


    virtual void destroy()
    {
        BaseRequestImpl::destroy();
        // TODO sync
        if (m_data) delete m_data;
        if (m_bitSet) delete m_bitSet;
        if (m_pvRequest) delete m_pvRequest;
        delete this;
    }

};











PVDATA_REFCOUNT_MONITOR_DEFINE(channelPut);

class ChannelPutImpl : public BaseRequestImpl, public ChannelPut
{
    private:
		ChannelPutRequester* m_channelPutRequester;
		
	    PVStructure* m_pvRequest;
		
		PVStructure* m_data;
		BitSet* m_bitSet;

    private:
    ~ChannelPutImpl()
    {
        PVDATA_REFCOUNT_MONITOR_DESTRUCT(channelPut);
    }

    public:
    ChannelPutImpl(ChannelImpl* channel, ChannelPutRequester* channelPutRequester, PVStructure *pvRequest) :
        BaseRequestImpl(channel, channelPutRequester),
        m_channelPutRequester(channelPutRequester), m_pvRequest(pvRequest),
        //(dynamic_cast<PVStructure*>(getPVDataCreate()->createPVField(0, "", pvRequest))),
        m_data(0), m_bitSet(0)
    {
        PVDATA_REFCOUNT_MONITOR_CONSTRUCT(channelPut);

		// TODO low-overhead put
		// TODO best-effort put

		// subscribe
//		try {
			resubscribeSubscription(m_channel->checkAndGetTransport());
//		} catch (IllegalStateException ise) {
// TODO			m_channelPutRequester->channelPutConnect(channelNotConnected, null, null, null);
//		} catch (CAException caex) {
// TODO			m_channelPutRequester->channelPutConnect(statusCreate.createStatus(StatusType.ERROR, "failed to sent message over network", caex), null, null, null);
//		}

    }

	virtual void send(ByteBuffer* buffer, TransportSendControl* control) {
		int32 pendingRequest = getPendingRequest();
		if (pendingRequest < 0)
		{
			BaseRequestImpl::send(buffer, control);
			return;
		}
		
		control->startMessage((int8)11, 9);
		buffer->putInt(m_channel->getServerChannelID());
		buffer->putInt(m_ioid);
		buffer->putByte((int8)m_pendingRequest);
		
		if (pendingRequest & QOS_INIT)
		{
			// pvRequest
			m_channel->getTransport()->getIntrospectionRegistry()->serializePVRequest(buffer, control, m_pvRequest);
		}
		else if (!(pendingRequest & QOS_GET))
		{
			// put
			// serialize only what has been changed
			m_bitSet->serialize(buffer, control);
			m_data->serialize(buffer, control, m_bitSet);
		}
		
		stopRequest();
	}

	virtual bool destroyResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		m_channelPutRequester->putDone(status);
		return true;
	}

	virtual bool initResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		if (!status->isSuccess())
		{
			m_channelPutRequester->channelPutConnect(status, this, 0, 0);
			return true;
		}

		// create data and its bitSet
		m_data = transport->getIntrospectionRegistry()->deserializeStructureAndCreatePVStructure(payloadBuffer, transport);
		m_bitSet = new BitSet(m_data->getNumberFields());

		// notify
		m_channelPutRequester->channelPutConnect(okStatus, this, m_data, m_bitSet);
		return true;
	}

	virtual bool normalResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		if (qos & QOS_GET)
		{
			if (!status->isSuccess())
			{
				m_channelPutRequester->getDone(status);
				return true;
			}
			
			m_data->deserialize(payloadBuffer, transport);
			m_channelPutRequester->getDone(status);
    		return true;
		}
		else
		{
		    m_channelPutRequester->putDone(okStatus);
    		return true;
		}
	}

    virtual void get() {
        // TODO sync?
        
		if (m_destroyed) {
			m_channelPutRequester->getDone(destroyedStatus);
			return;
		}

		if (!startRequest(QOS_GET)) {
			m_channelPutRequester->getDone(otherRequestPendingStatus);
			return;
		}
		

//		try {
			m_channel->checkAndGetTransport()->enqueueSendRequest(this);
//		} catch (IllegalStateException ise) {
//			m_channelPutRequester->getDone(channelNotConnected);
//		}
	}

    virtual void put(bool lastRequest) {
        // TODO sync?
        
		if (m_destroyed) {
			m_channelPutRequester->putDone(destroyedStatus);
			return;
		}

		if (!startRequest(lastRequest ? QOS_DESTROY : QOS_DEFAULT)) {
			m_channelPutRequester->putDone(otherRequestPendingStatus);
			return;
		}
		
		//try {
			m_channel->checkAndGetTransport()->enqueueSendRequest(this);
		//} catch (IllegalStateException ise) {
	//TODO	//	m_channelPutRequester->putDone(channelNotConnected);
		//}
	}
	
	virtual void resubscribeSubscription(Transport* transport) {
		startRequest(QOS_INIT);
		transport->enqueueSendRequest(this);
	}


    virtual void destroy()
    {
        BaseRequestImpl::destroy();
        // TODO sync
        if (m_data) delete m_data;
        if (m_bitSet) delete m_bitSet;
        if (m_pvRequest) delete m_pvRequest;
        delete this;
    }

};










PVDATA_REFCOUNT_MONITOR_DEFINE(channelGetField);

// NOTE: this instance is not returned as Request, so it must self-destruct
class ChannelGetFieldRequestImpl : public DataResponse, public TransportSender
{
    private:
        ChannelImpl* m_channel;
	    ClientContextImpl* m_context;
	    pvAccessID m_ioid;
		GetFieldRequester* m_callback;
	    String m_subField;
	    Mutex m_mutex;
	    bool m_destroyed;
		
    private:
    ~ChannelGetFieldRequestImpl()
    {
        PVDATA_REFCOUNT_MONITOR_DESTRUCT(channelGetField);
    }


	public:
	ChannelGetFieldRequestImpl(ChannelImpl* channel, GetFieldRequester* callback, String subField) :
	   m_channel(channel), m_context(channel->getContext()),
	   m_callback(callback), m_subField(subField),
	   m_destroyed(false)
	{
        PVDATA_REFCOUNT_MONITOR_CONSTRUCT(channelGetField);

		// register response request
		m_ioid = m_context->registerResponseRequest(this);
		channel->registerResponseRequest(this);

        // TODO
		// enqueue send request
		//try {
			m_channel->checkAndGetTransport()->enqueueSendRequest(this);
		//} catch (IllegalStateException ise) {
		//	callback.getDone(BaseRequestImpl.channelNotConnected, null);
		//}
	}

	Requester* getRequester() {
		return m_callback;
	}

	pvAccessID getIOID() {
		return m_ioid;
	}

	virtual void lock() {
		// noop
	}
	
	virtual void send(ByteBuffer* buffer, TransportSendControl* control) {
		control->startMessage((int8)17, 8);
		buffer->putInt(m_channel->getServerChannelID());
		buffer->putInt(m_ioid);
		SerializeHelper::serializeString(m_subField, buffer, control);
	}


	virtual void cancel() {
		destroy();
		// TODO notify?
	}

	virtual void timeout() {
		cancel();
	}

	void reportStatus(Status* status) {
		// destroy, since channel (parent) was destroyed
		if (status == ChannelImpl::channelDestroyed)
			destroy();
		// TODO notify?
	}

	virtual void unlock() {
		// noop
	}

    virtual void destroy()
    {
		{
    	   Lock guard(&m_mutex);
			if (m_destroyed)
				return;
			m_destroyed = true;
		}

		// unregister response request
		m_context->unregisterResponseRequest(this);
		m_channel->unregisterResponseRequest(this);
		
		delete this;
    }

	virtual void response(Transport* transport, int8 version, ByteBuffer* payloadBuffer) {
// TODO?
//		try
//		{	
			Status* status = statusCreate->deserializeStatus(payloadBuffer, transport);
			if (status->isSuccess())
			{
				// deserialize Field...
				const Field* field = transport->getIntrospectionRegistry()->deserialize(payloadBuffer, transport);
				m_callback->getDone(status, field);
				field->decReferenceCount();
			}
			else
			{
				m_callback->getDone(status, 0);
			}
			
			// TODO
			if (status != okStatus)
			 delete status;
//        } // TODO guard callback
//		finally
//		{
			// always cancel request
//			cancel();
//		}
        
        cancel();

	}


};














PVDATA_REFCOUNT_MONITOR_DEFINE(channelMonitor);

class ChannelMonitorImpl : public BaseRequestImpl, public Monitor,
public MonitorElement
{
    private:
		MonitorRequester* m_monitorRequester;
		Structure* m_structure;
		bool m_started;
		
	    PVStructure* m_pvRequest;

        // TODO temp
		PVStructure* m_pvStructure;
		BitSet* m_changedBitSet;
		BitSet* m_overrunBitSet;
		
		
		int m_count;
        Mutex m_lock;
        
    private:
    ~ChannelMonitorImpl()
    {
        PVDATA_REFCOUNT_MONITOR_DESTRUCT(channelMonitor);
    }

    public:
    ChannelMonitorImpl(ChannelImpl* channel, MonitorRequester* monitorRequester, PVStructure *pvRequest) :
        BaseRequestImpl(channel, monitorRequester),
        m_monitorRequester(monitorRequester), m_structure(0),
        m_started(false), m_pvRequest(pvRequest),
        //(dynamic_cast<PVStructure*>(getPVDataCreate()->createPVField(0, "", pvRequest))), 
        m_pvStructure(0), m_count(0)
    {
        PVDATA_REFCOUNT_MONITOR_CONSTRUCT(channelMonitor);

        // TODO quques

		// subscribe
//		try {
			resubscribeSubscription(m_channel->checkAndGetTransport());
//		} catch (IllegalStateException ise) {
// TODO			m_monitorRequester->monitorConnect(channelNotConnected, null, null);
//		} catch (CAException caex) {
// TODO			m_monitorRequester->monitorConnect(statusCreate.createStatus(StatusType.ERROR, "failed to sent message over network", caex), null, null);
//		}

    }

	virtual void send(ByteBuffer* buffer, TransportSendControl* control) {
		int32 pendingRequest = getPendingRequest();
		if (pendingRequest < 0)
		{
			BaseRequestImpl::send(buffer, control);
			return;
		}
		
		control->startMessage((int8)13, 9);
		buffer->putInt(m_channel->getServerChannelID());
		buffer->putInt(m_ioid);
		buffer->putByte((int8)m_pendingRequest);
		
		if (pendingRequest & QOS_INIT)
		{
			// pvRequest
			m_channel->getTransport()->getIntrospectionRegistry()->serializePVRequest(buffer, control, m_pvRequest);
		}
		
		stopRequest();
	}

	virtual bool destroyResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		// data available
		// TODO if (qos & QOS_GET)
			normalResponse(transport, version, payloadBuffer, qos, status);
		return true;
	}

	virtual bool initResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		if (!status->isSuccess())
		{
			m_monitorRequester->monitorConnect(status, this, 0);
			return true;
		}

		// create data and its bitSet
		m_structure = const_cast<Structure*>(dynamic_cast<const Structure*>(transport->getIntrospectionRegistry()->deserialize(payloadBuffer, transport)));
        //monitorStrategy->init(structure);
        
        
        // TODO temp
        m_pvStructure = dynamic_cast<PVStructure*>(getPVDataCreate()->createPVField(0, m_structure));
        m_changedBitSet = new BitSet(m_pvStructure->getNumberFields());
        m_overrunBitSet = new BitSet(m_pvStructure->getNumberFields());
                
        
		// notify
		m_monitorRequester->monitorConnect(okStatus, this, m_structure);
		return true;
	}

	virtual bool normalResponse(Transport* transport, int8 version, ByteBuffer* payloadBuffer, int8 qos, Status* status) {
		if (qos & QOS_GET)
		{
			// TODO not supported by IF yet...
		}
		else
		{
			// TODO
			m_changedBitSet->deserialize(payloadBuffer, transport);
	        m_pvStructure->deserialize(payloadBuffer, transport, m_changedBitSet);
	        m_overrunBitSet->deserialize(payloadBuffer, transport);
			m_monitorRequester->monitorEvent(this);
		}
		return true;
	}

	virtual void resubscribeSubscription(Transport* transport) {
		startRequest(QOS_INIT);
		transport->enqueueSendRequest(this);
		if (m_started)
			start();
	}


    virtual void destroy()
    {
        BaseRequestImpl::destroy();
        // TODO sync
        if (m_pvRequest) delete m_pvRequest;
 // uncomment when m_pvStructure not destroyed       if (m_structure) m_structure->decReferenceCount();
        
        // TODO temp
        if (m_pvStructure)
        {
            delete m_pvStructure;
            delete m_overrunBitSet;
            delete m_changedBitSet;
        }
        
        delete this;
    }
    


    // override, since we optimize status
	virtual void response(Transport* transport, int8 version, ByteBuffer* payloadBuffer) {
// TODO?
//		try
//		{	
			transport->ensureData(1);
			int8 qos = payloadBuffer->getByte();
			if (qos & QOS_INIT)
			{
    			Status* status = statusCreate->deserializeStatus(payloadBuffer, transport);
				initResponse(transport, version, payloadBuffer, qos, status);
    			// TODO
    			if (status != okStatus)
    			 delete status;
			}
			else if (qos & QOS_DESTROY)
			{
    			Status* status = statusCreate->deserializeStatus(payloadBuffer, transport);
				m_remotelyDestroyed = true;

				if (!destroyResponse(transport, version, payloadBuffer, qos, status))
				    cancel();
    			// TODO
    			if (status != okStatus)
    			 delete status;
			}
			else
			{
				normalResponse(transport, version, payloadBuffer, qos, okStatus);
			}
			
	}

    virtual Status* start()
    {
        Lock guard(&m_lock);
                
        // TODO sync
 		if (m_destroyed)
			return getStatusCreate()->createStatus(STATUSTYPE_ERROR, "Monitor destroyed.");;
		
		// TODO monitorStrategy.start();
		
		//try {
			// start == process + get
			if (!startRequest(QOS_PROCESS | QOS_GET))
			{
                return getStatusCreate()->createStatus(STATUSTYPE_ERROR, "Other request pending.");
			}
			m_channel->checkAndGetTransport()->enqueueSendRequest(this);
			m_started = true;
            // client needs to delete status, so passing shared OK instance is not right thing to do
            return getStatusCreate()->createStatus(STATUSTYPE_OK, "Monitor started.");
		//} catch (IllegalStateException ise) {
		//	return channelNotConnected;
		//}



    }

    virtual Status* stop()
    {
        Lock guard(&m_lock);
                
        // TODO sync
 		if (m_destroyed)
			return getStatusCreate()->createStatus(STATUSTYPE_ERROR, "Monitor destroyed.");;
		
		//monitorStrategy.stop();
		
		//try {
			// stop == process + no get
			if (!startRequest(QOS_PROCESS))
			{
                return getStatusCreate()->createStatus(STATUSTYPE_ERROR, "Other request pending.");
			}
			m_channel->checkAndGetTransport()->enqueueSendRequest(this);
			m_started = false;
            // client needs to delete status, so passing shared OK instance is not right thing to do
            return getStatusCreate()->createStatus(STATUSTYPE_OK, "Monitor stopped.");
		//} catch (IllegalStateException ise) {
		//	return channelNotConnected;
		//}

    }


    // ============ temp ============

    virtual MonitorElement* poll()
    {
        Lock xx(&m_lock);
        if (m_count)
        {
            return 0;
        }
        else
        {
            m_count++;
            return this;
        }
    }

    virtual void release(MonitorElement* monitorElement)
    {
        Lock xx(&m_lock);
        if (m_count)
            m_count--;
    }

    // ============ MonitorElement ============

    virtual PVStructure* getPVStructure()
    {
        return m_pvStructure;
    }

    virtual BitSet* getChangedBitSet()
    {
        return m_changedBitSet;
    }

    virtual BitSet* getOverrunBitSet()
    {
        return m_overrunBitSet;
    }
    

};







 // TODO consider std::unordered_map
typedef std::map<pvAccessID, ResponseRequest*> IOIDResponseRequestMap;



// TODO log
#define CALLBACK_GUARD(code) try { code } catch(...) { }



        /**
         * @author <a href="mailto:matej.sekoranjaATcosylab.com">Matej Sekoranja</a>
         * @version $Id: AbstractServerResponseHandler.java,v 1.1 2010/05/03 14:45:39 mrkraimer Exp $
         */
        class AbstractClientResponseHandler : public AbstractResponseHandler {
        protected:
            ClientContextImpl* _context;
        public:
            /**
             * @param context
             * @param description
             */
            AbstractClientResponseHandler(ClientContextImpl* context, String description) : 
                AbstractResponseHandler(context, description), _context(context) {
            }

            virtual ~AbstractClientResponseHandler() {
            }
        };

        class NoopResponse :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            /**
             * @param context
             */
            NoopResponse(ClientContextImpl* context, String description) :
                AbstractClientResponseHandler(context, description)
            {
            }

            virtual ~NoopResponse() {
            }
        };


        class BadResponse :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            /**
             * @param context
             */
            BadResponse(ClientContextImpl* context) :
                AbstractClientResponseHandler(context, "Bad response")
            {
            }

            virtual ~BadResponse() {
            }

            virtual void handleResponse(osiSockAddr* responseFrom,
                    Transport* transport, int8 version, int8 command,
                    int payloadSize, epics::pvData::ByteBuffer* payloadBuffer)
                    {
            char ipAddrStr[48];
            ipAddrToDottedIP(&responseFrom->ia, ipAddrStr, sizeof(ipAddrStr));

            errlogSevPrintf(errlogInfo,
                    "Undecipherable message (bad response type %d) from %s.",
                    command, ipAddrStr);
                    }
        };


        class DataResponseHandler :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            /**
             * @param context
             */
            DataResponseHandler(ClientContextImpl* context) :
                AbstractClientResponseHandler(context, "Data response")
            {
            }

            virtual ~DataResponseHandler() {
            }

            virtual void handleResponse(osiSockAddr* responseFrom,
                    Transport* transport, int8 version, int8 command,
                    int payloadSize, epics::pvData::ByteBuffer* payloadBuffer)
                    {
		AbstractClientResponseHandler::handleResponse(responseFrom, transport, version, command, payloadSize, payloadBuffer);
		
		transport->ensureData(4);
		ResponseRequest* rr = _context->getResponseRequest(payloadBuffer->getInt());
		if (rr)
		{
    		DataResponse* nrr = dynamic_cast<DataResponse*>(rr);
    		if (nrr)
        		nrr->response(transport, version, payloadBuffer);		
        }
        }
        };


        class SearchResponseHandler :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            SearchResponseHandler(ClientContextImpl* context) :
                AbstractClientResponseHandler(context, "Search response")
            {
            }

            virtual ~SearchResponseHandler() {
            }

            virtual void handleResponse(osiSockAddr* responseFrom,
                    Transport* transport, int8 version, int8 command,
                    int payloadSize, epics::pvData::ByteBuffer* payloadBuffer)
                    {
		AbstractClientResponseHandler::handleResponse(responseFrom, transport, version, command, payloadSize, payloadBuffer);

		transport->ensureData(5);
		int32 searchSequenceId = payloadBuffer->getInt();
		bool found = payloadBuffer->getByte() != 0;
		if (!found)
			return;

		transport->ensureData((128+2*16)/8);

        osiSockAddr serverAddress;
        serverAddress.ia.sin_family = AF_INET;

		// 128-bit IPv6 address
		/*
		int8* byteAddress = new int8[16];
		for (int i = 0; i < 16; i++) 
		  byteAddress[i] = payloadBuffer->getByte(); };
		*/
		
        // IPv4 compatible IPv6 address expected
        // first 80-bit are 0
        if (payloadBuffer->getLong() != 0) return;
        if (payloadBuffer->getShort() != 0) return;
        if (payloadBuffer->getShort() != (int16)0xFFFF) return;
   
		// accept given address if explicitly specified by sender
        serverAddress.ia.sin_addr.s_addr = htonl(payloadBuffer->getInt());
        if (serverAddress.ia.sin_addr.s_addr == INADDR_ANY)
            serverAddress.ia.sin_addr = responseFrom->ia.sin_addr; 

        serverAddress.ia.sin_port = htons(payloadBuffer->getShort());

		// reads CIDs
		ChannelSearchManager* csm = _context->getChannelSearchManager();
		int16 count = payloadBuffer->getShort();
		for (int i = 0; i < count; i++)
		{
			transport->ensureData(4);
			pvAccessID cid = payloadBuffer->getInt();
			csm->searchResponse(cid, searchSequenceId, version & 0x0F, &serverAddress);
		}

                        
       }
        };


        class BeaconResponseHandler :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            BeaconResponseHandler(ClientContextImpl* context) :
                AbstractClientResponseHandler(context, "Beacon")
            {
            }

            virtual ~BeaconResponseHandler() {
            }

            virtual void handleResponse(osiSockAddr* responseFrom,
                    Transport* transport, int8 version, int8 command,
                    int payloadSize, epics::pvData::ByteBuffer* payloadBuffer)
                    {
    	// reception timestamp
    	TimeStamp timestamp;
    	timestamp.getCurrent();

		AbstractClientResponseHandler::handleResponse(responseFrom, transport, version, command, payloadSize, payloadBuffer);

    	transport->ensureData((2*sizeof(int16)+2*sizeof(int32)+128)/sizeof(int8));

    	int16 sequentalID = payloadBuffer->getShort();
    	TimeStamp startupTimestamp(payloadBuffer->getInt(),payloadBuffer->getInt());

        osiSockAddr serverAddress;
        serverAddress.ia.sin_family = AF_INET;

		// 128-bit IPv6 address
		/*
		int8* byteAddress = new int8[16];
		for (int i = 0; i < 16; i++) 
		  byteAddress[i] = payloadBuffer->getByte(); };
		*/
		
        // IPv4 compatible IPv6 address expected
        // first 80-bit are 0
        if (payloadBuffer->getLong() != 0) return;
        if (payloadBuffer->getShort() != 0) return;
        if (payloadBuffer->getShort() != (int16)0xFFFF) return;
   
		// accept given address if explicitly specified by sender
        serverAddress.ia.sin_addr.s_addr = htonl(payloadBuffer->getInt());
        if (serverAddress.ia.sin_addr.s_addr == INADDR_ANY)
            serverAddress.ia.sin_addr = responseFrom->ia.sin_addr; 

        serverAddress.ia.sin_port = htons(payloadBuffer->getShort());

    	BeaconHandler* beaconHandler = _context->getBeaconHandler(responseFrom);
    	// currently we care only for servers used by this context
    	if (beaconHandler == NULL)
    		return;

    	// extra data
    	PVFieldPtr data = NULL;
    	const FieldConstPtr field = IntrospectionRegistry::deserializeFull(payloadBuffer, transport);
    	if (field != NULL)
    	{
    		data = getPVDataCreate()->createPVField(NULL, field);
    		data->deserialize(payloadBuffer, transport);
    	}

    	// notify beacon handler
    	beaconHandler->beaconNotify(responseFrom, version, &timestamp, &startupTimestamp, sequentalID, data);

       }
        };

        class ConnectionValidationHandler :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            ConnectionValidationHandler(ClientContextImpl* context) :
                AbstractClientResponseHandler(context, "Connection validation")
            {
            }

            virtual ~ConnectionValidationHandler() {
            }

            virtual void handleResponse(osiSockAddr* responseFrom,
                    Transport* transport, int8 version, int8 command,
                    int payloadSize, epics::pvData::ByteBuffer* payloadBuffer)
                    {
		AbstractClientResponseHandler::handleResponse(responseFrom, transport, version, command, payloadSize, payloadBuffer);
		     
				transport->ensureData(8);
		transport->setRemoteTransportReceiveBufferSize(payloadBuffer->getInt());
		transport->setRemoteTransportSocketReceiveBufferSize(payloadBuffer->getInt());

		transport->setRemoteMinorRevision(version);
		
		TransportSender* sender = dynamic_cast<TransportSender*>(transport);
		if (sender)
		  transport->enqueueSendRequest(sender);
		transport->verified();

		              }
        };

        class MessageHandler :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            MessageHandler(ClientContextImpl* context) :
                AbstractClientResponseHandler(context, "Message")
            {
            }

            virtual ~MessageHandler() {
            }

            virtual void handleResponse(osiSockAddr* responseFrom,
                    Transport* transport, int8 version, int8 command,
                    int payloadSize, epics::pvData::ByteBuffer* payloadBuffer)
                    {
		AbstractClientResponseHandler::handleResponse(responseFrom, transport, version, command, payloadSize, payloadBuffer);
		     
		transport->ensureData(5);

		DataResponse* nrr = dynamic_cast<DataResponse*>(_context->getResponseRequest(payloadBuffer->getInt()));
		Requester* requester;
		if (nrr && (requester = nrr->getRequester()))
		{
			 MessageType type = (MessageType)payloadBuffer->getByte();
			 String message = SerializeHelper::deserializeString(payloadBuffer, transport);
			requester->message(message, type); // TODO do we need to guard from exceptions
		}

		            }
        };

        class CreateChannelHandler :  public AbstractClientResponseHandler, private epics::pvData::NoDefaultMethods {
        public:
            CreateChannelHandler(ClientContextImpl* context) :
                AbstractClientResponseHandler(context, "Create channel")
            {
            }

            virtual ~CreateChannelHandler() {
            }

            virtual void handleResponse(osiSockAddr* responseFrom,
                    Transport* transport, int8 version, int8 command,
                    int payloadSize, epics::pvData::ByteBuffer* payloadBuffer)
                    {
		AbstractClientResponseHandler::handleResponse(responseFrom, transport, version, command, payloadSize, payloadBuffer);
		     
		transport->ensureData(8);
		pvAccessID cid = payloadBuffer->getInt();
		pvAccessID sid = payloadBuffer->getInt();
		// TODO... do not destroy OK
		Status* status = transport->getIntrospectionRegistry()->deserializeStatus(payloadBuffer, transport);
		
		ChannelImpl* channel = static_cast<ChannelImpl*>(_context->getChannel(cid));
		if (channel)
		{
			// failed check
			if (!status->isSuccess()) {
				channel->createChannelFailed();
				return;
			}	
			
			//int16 acl = payloadBuffer->getShort();

			channel->connectionCompleted(sid);
		}
		
		// TODO not nice
		if (status != g_statusOK)
		  delete status;

		              }
        };



/**
 * CA response handler - main handler which dispatches responses to appripriate handlers.
 * @author <a href="mailto:matej.sekoranjaATcosylab.com">Matej Sekoranja</a>
 * @version $Id: ClientResponseHandler.java,v 1.1 2010/05/03 14:45:40 mrkraimer Exp $
 */
class ClientResponseHandler : public ResponseHandler, private epics::pvData::NoDefaultMethods {
    private:

	/**
	 * Table of response handlers for each command ID.
	 */
	ResponseHandler** m_handlerTable;

	/*
	 * Context instance is part of the response handler now
	 */
	//ClientContextImpl* m_context;

	public:

	virtual ~ClientResponseHandler() {
	   delete m_handlerTable[ 0];
	   delete m_handlerTable[ 1];
	   delete m_handlerTable[ 2];
	   delete m_handlerTable[ 3];
	   delete m_handlerTable[ 4];
	   delete m_handlerTable[ 5];
	   delete m_handlerTable[ 6];
	   delete m_handlerTable[ 7];
	   delete m_handlerTable[ 8];
	   delete m_handlerTable[ 9];
	   delete m_handlerTable[18];
	   
	   delete[] m_handlerTable;
	}

	/**
	 * @param context
	 */
	ClientResponseHandler(ClientContextImpl* context) {
		ResponseHandler* badResponse = new BadResponse(context);
		ResponseHandler* dataResponse = new DataResponseHandler(context);

        // TODO free!!!
		#define HANDLER_COUNT 28
		m_handlerTable = new ResponseHandler*[HANDLER_COUNT];
		m_handlerTable[ 0] = new BeaconResponseHandler(context), /*  0 */
		m_handlerTable[ 1] = new ConnectionValidationHandler(context), /*  1 */
		m_handlerTable[ 2] = new NoopResponse(context, "Echo"), /*  2 */
		m_handlerTable[ 3] = new NoopResponse(context, "Search"), /*  3 */
		m_handlerTable[ 4] = new SearchResponseHandler(context), /*  4 */
		m_handlerTable[ 5] = new NoopResponse(context, "Introspection search"), /*  5 */
		m_handlerTable[ 6] = dataResponse; /*  6 - introspection search */
		m_handlerTable[ 7] = new CreateChannelHandler(context), /*  7 */
		m_handlerTable[ 8] = new NoopResponse(context, "Destroy channel"), /*  8 */ // TODO it might be useful to implement this...
		m_handlerTable[ 9] = badResponse; /*  9 */
		m_handlerTable[10] = dataResponse; /* 10 - get response */
		m_handlerTable[11] = dataResponse; /* 11 - put response */
		m_handlerTable[12] = dataResponse; /* 12 - put-get response */
		m_handlerTable[13] = dataResponse; /* 13 - monitor response */
		m_handlerTable[14] = dataResponse; /* 14 - array response */
		m_handlerTable[15] = badResponse; /* 15 - cancel request */
		m_handlerTable[16] = dataResponse; /* 16 - process response */
		m_handlerTable[17] = dataResponse; /* 17 - get field response */
		m_handlerTable[18] = new MessageHandler(context), /* 18 - message to Requester */
		m_handlerTable[19] = badResponse; // TODO new MultipleDataResponseHandler(context), /* 19 - grouped monitors */
		m_handlerTable[20] = dataResponse; /* 20 - RPC response */
		m_handlerTable[21] = badResponse; /* 21 */
		m_handlerTable[22] = badResponse; /* 22 */
		m_handlerTable[23] = badResponse; /* 23 */
		m_handlerTable[24] = badResponse; /* 24 */
		m_handlerTable[25] = badResponse; /* 25 */
		m_handlerTable[26] = badResponse; /* 26 */
		m_handlerTable[27] = badResponse; /* 27 */
	}

    virtual void handleResponse(osiSockAddr* responseFrom,
                        Transport* transport, int8 version, int8 command,
                        int payloadSize, ByteBuffer* payloadBuffer)
    {
		if (command < 0 || command >= HANDLER_COUNT)
		{
			// TODO context.getLogger().fine("Invalid (or unsupported) command: " + command + ".");
			std::cout << "Invalid (or unsupported) command: " << command << "." << std::endl;
			// TODO remove debug output
			char buf[100];
			sprintf(buf, "Invalid CA header %d its payload buffer", command);
			hexDump(buf, (const int8*)(payloadBuffer->getArray()), payloadBuffer->getPosition(), payloadSize);
			return;
		}

		// delegate
		m_handlerTable[command]->handleResponse(responseFrom, transport, version, command, payloadSize, payloadBuffer);
	}
};





PVDATA_REFCOUNT_MONITOR_DEFINE(channel);


/**
 * Context state enum.
 */
enum ContextState {
	/**
	 * State value of non-initialized context.
	 */
	CONTEXT_NOT_INITIALIZED,

	/**
	 * State value of initialized context.
	 */
	CONTEXT_INITIALIZED,

	/**
	 * State value of destroyed context.
	 */
	CONTEXT_DESTROYED
};


class TestClientContextImpl : public ClientContextImpl
{






/**
 * Implementation of CAJ JCA <code>Channel</code>.
 * @author <a href="mailto:matej.sekoranjaATcosylab.com">Matej Sekoranja</a>
 */
class TestChannelImpl : public ChannelImpl {
    private:

    	/**
    	 * Context.
    	 */
    	ClientContextImpl* m_context;

    	/**
    	 * Client channel ID.
    	 */
    	pvAccessID m_channelID;

    	/**
    	 * Channel name.
    	 */
        String m_name;

    	/**
    	 * Channel requester.
    	 */
         ChannelRequester* m_requester;

    	/**
    	 * Process priority.
    	 */
    	short m_priority;

    	/**
    	 * List of fixed addresses, if <code<0</code> name resolution will be used.
    	 */
    	InetAddrVector* m_addresses;

    	/**
    	 * Connection status.
    	 */
    	ConnectionState m_connectionState;

    	/**
    	 * List of all channel's pending requests (keys are subscription IDs).
    	 */
    	IOIDResponseRequestMap m_responseRequests;

        /**
         * Mutex for response requests.
         */
        Mutex m_responseRequestsMutex;
         
    	/**
    	 * Allow reconnection flag.
    	 */
    	bool m_allowCreation;

    	/**
    	 * Reference counting.
    	 * NOTE: synced on <code>m_channelMutex</code>.
    	 */
    	int m_references;

    	/* ****************** */
    	/* CA protocol fields */
    	/* ****************** */

    	/**
    	 * Server transport.
    	 */
    	Transport* m_transport;

    	/**
    	 * Server channel ID.
    	 */
    	pvAccessID m_serverChannelID;

        /**
         * Context sync. mutex.
         */
        Mutex m_channelMutex;

        /**
         * Flag indicting what message to send.
         */
	   bool m_issueCreateMessage;

    private:
    ~TestChannelImpl() 
    {
        PVDATA_REFCOUNT_MONITOR_DESTRUCT(channel);
    }

    public:

	/**
	 * Constructor.
	 * @param context
	 * @param name
	 * @param listener
	 * @throws CAException
	 */
    TestChannelImpl(
            ClientContextImpl* context,
            pvAccessID channelID,
            String name,
			ChannelRequester* requester,
			short priority,
			InetAddrVector* addresses) :
        m_context(context),
        m_channelID(channelID),
        m_name(name),
        m_requester(requester),
        m_priority(priority),
        m_addresses(addresses),
        m_connectionState(NEVER_CONNECTED),
        m_allowCreation(true),
        m_references(1),
        m_transport(0),
        m_serverChannelID(0xFFFFFFFF),
        m_issueCreateMessage(true)
    {
        PVDATA_REFCOUNT_MONITOR_CONSTRUCT(channel);

		// register before issuing search request
		m_context->registerChannel(this);

		// connect
		connect();
    }

    virtual void destroy()
    {
        destroy(false); //TODO guard
        if (m_addresses) delete m_addresses;
        delete this;
    };

    virtual String getRequesterName()
    {
        return getChannelName();
    };

    virtual void message(String message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << messageTypeName[messageType] << ")" << std::endl;
    }

    virtual ChannelProvider* getProvider()
    {
        return m_context->getProvider();
    }

	// NOTE: synchronization guarantees that <code>transport</code> is non-<code>null</code> and <code>state == CONNECTED</code>.
    virtual epics::pvData::String getRemoteAddress()
    {
	   Lock guard(&m_channelMutex);
		if (m_connectionState != CONNECTED) {
		  static String emptyString;
			return emptyString;
		}
		else
		{
		  return inetAddressToString(*m_transport->getRemoteAddress());
		}
	}

    virtual epics::pvData::String getChannelName()
    {
        return m_name;
    }

    virtual ChannelRequester* getChannelRequester()
    {
        return m_requester;
    }

    virtual ConnectionState getConnectionState()
    {
	   Lock guard(&m_channelMutex);
       return m_connectionState;
    }

    virtual bool isConnected()
    {
        return getConnectionState() == CONNECTED;
    }

    virtual AccessRights getAccessRights(epics::pvData::PVField *pvField)
    {
        return readWrite;
    }

	/**
	 * Get client channel ID.
	 * @return client channel ID.
	 */
	pvAccessID getChannelID() {
		return m_channelID;
	}
	
	virtual ClientContextImpl* getContext() {
	   return m_context;
	}

	virtual pvAccessID getSearchInstanceID() {
		return m_channelID;
	}

	virtual String getSearchInstanceName() {
	   return m_name;
	}

	virtual pvAccessID getServerChannelID() {
	   Lock guard(&m_channelMutex);
	   return m_serverChannelID;
	}

	virtual void registerResponseRequest(ResponseRequest* responseRequest)
	{
	   Lock guard(&m_responseRequestsMutex);
	   m_responseRequests[responseRequest->getIOID()] = responseRequest;
	}

	virtual void unregisterResponseRequest(ResponseRequest* responseRequest)
	{
	   Lock guard(&m_responseRequestsMutex);
	   m_responseRequests.erase(responseRequest->getIOID());
	}

	void connect() {
	   Lock guard(&m_channelMutex);
		// if not destroyed...
		if (m_connectionState == DESTROYED)
			throw std::runtime_error("Channel destroyed.");
		else if (m_connectionState != CONNECTED)
			initiateSearch();
	}

	void disconnect() {
	   Lock guard(&m_channelMutex);
		// if not destroyed...
		if (m_connectionState == DESTROYED)
			throw std::runtime_error("Channel destroyed.");
		else if (m_connectionState == CONNECTED)
			disconnect(false, true);
	}

	/**
	 * Create a channel, i.e. submit create channel request to the server.
	 * This method is called after search is complete.
	 * @param transport
	 */
	void createChannel(Transport* transport)
	{
	   Lock guard(&m_channelMutex);

		// do not allow duplicate creation to the same transport
		if (!m_allowCreation)
			return;
		m_allowCreation = false;

		// check existing transport
		if (m_transport && m_transport != transport)
		{
			disconnectPendingIO(false);

			ReferenceCountingTransport* rct = dynamic_cast<ReferenceCountingTransport*>(m_transport);
			if (rct) rct->release(this);
		}
		else if (m_transport == transport)
		{
			// request to sent create request to same transport, ignore
			// this happens when server is slower (processing search requests) than client generating it
			return;
		}

		m_transport = transport;
		m_transport->enqueueSendRequest(this);
	}

	virtual void cancel() {
		// noop
	}

	virtual void timeout() {
		createChannelFailed();
	}

	/**
	 * Create channel failed.
	 */
	virtual void createChannelFailed()
	{
	   Lock guard(&m_channelMutex);

		cancel();
		// ... and search again
		initiateSearch();
	}

	/**
	 * Called when channel created succeeded on the server.
	 * <code>sid</code> might not be valid, this depends on protocol revision.
	 * @param sid
	 */
	virtual void connectionCompleted(pvAccessID sid/*,  rights*/)
	{
	   Lock guard(&m_channelMutex);

        bool allOK = false;
		try
		{
			// do this silently
			if (m_connectionState == DESTROYED)
				return;

			// store data
			m_serverChannelID = sid;
			//setAccessRights(rights);

			// user might create monitors in listeners, so this has to be done before this can happen
			// however, it would not be nice if events would come before connection event is fired
			// but this cannot happen since transport (TCP) is serving in this thread
			resubscribeSubscriptions();
			setConnectionState(CONNECTED);
			allOK = true;
		}
		catch (...) {
		  // noop
            // TODO at least log something??
		}

		if (!allOK)
		{
			// end connection request
			cancel();
		}
	}

	/**
	 * @param force force destruction regardless of reference count
	 */
	void destroy(bool force) {
	   Lock guard(&m_channelMutex);
		if (m_connectionState == DESTROYED)
			throw std::runtime_error("Channel already destroyed.");

		// do destruction via context
		m_context->destroyChannel(this, force);

	}

	/**
	 * Increment reference.
	 */
	void acquire() {
	   Lock guard(&m_channelMutex);
		m_references++;
	}

	/**
	 * Actual destroy method, to be called <code>CAJContext</code>.
	 * @param force force destruction regardless of reference count
	 * @throws CAException
	 * @throws IllegalStateException
	 * @throws IOException
	 */
	void destroyChannel(bool force) {
	   Lock guard(&m_channelMutex);

		if (m_connectionState == DESTROYED)
			throw std::runtime_error("Channel already destroyed.");

		m_references--;
		if (m_references > 0 && !force)
			return;

		// stop searching...
		m_context->getChannelSearchManager()->unregisterChannel(this);
		cancel();

		disconnectPendingIO(true);

		if (m_connectionState == CONNECTED)
		{
			disconnect(false, true);
		}
		else if (m_transport)
		{
			// unresponsive state, do not forget to release transport
			ReferenceCountingTransport* rct = dynamic_cast<ReferenceCountingTransport*>(m_transport);
			if (rct) rct->release(this);
			m_transport = 0;
		}

		setConnectionState(DESTROYED);

		// unregister
		m_context->unregisterChannel(this);
	}

	/**
	 * Disconnected notification.
	 * @param initiateSearch	flag to indicate if searching (connect) procedure should be initiated
	 * @param remoteDestroy		issue channel destroy request.
	 */
	void disconnect(bool initiateSearch, bool remoteDestroy) {
	   Lock guard(&m_channelMutex);

		if (m_connectionState != CONNECTED && !m_transport)
			return;

		if (!initiateSearch) {
			// stop searching...
			m_context->getChannelSearchManager()->unregisterChannel(this);
			cancel();
		}
		setConnectionState(DISCONNECTED);

		disconnectPendingIO(false);

		// release transport
		if (m_transport)
		{
			if (remoteDestroy) {
				m_issueCreateMessage = false;
				// TODO !!! this causes problems.. since qnqueueSendRequest is added and this instance deleted
				//m_transport->enqueueSendRequest(this);
			}

			ReferenceCountingTransport* rct = dynamic_cast<ReferenceCountingTransport*>(m_transport);
			if (rct) rct->release(this);
			m_transport = 0;
		}

		if (initiateSearch)
			this->initiateSearch();

	}

	/**
	 * Initiate search (connect) procedure.
	 */
	void initiateSearch()
	{
	   Lock guard(&m_channelMutex);

	   m_allowCreation = true;

		if (!m_addresses)
			m_context->getChannelSearchManager()->registerChannel(this);
			/* TODO
		else
			// TODO not only first
			// TODO minor version
			// TODO what to do if there is no channel, do not search in a loop!!! do this in other thread...!
			searchResponse(CAConstants.CA_MINOR_PROTOCOL_REVISION, addresses[0]);
			*/
	}

	virtual void searchResponse(int8 minorRevision, osiSockAddr* serverAddress) {
	   Lock guard(&m_channelMutex);
		Transport* transport = m_transport;
		if (transport)
		{
			// multiple defined PV or reconnect request (same server address)
			if (sockAddrAreIdentical(transport->getRemoteAddress(), serverAddress))
			{
				m_requester->message("More than one channel with name '" + m_name +
				  			   "' detected, additional response from: " + inetAddressToString(*serverAddress), warningMessage);
				return;
			}
		}

		transport = m_context->getTransport(this, serverAddress, minorRevision, m_priority);
		if (!transport)
		{
			createChannelFailed();
			return;
		}

		// create channel
		createChannel(transport);
	}

	virtual void transportClosed() {
		disconnect(true, false);
	}

	virtual void transportChanged() {
		initiateSearch();
	}

	virtual Transport* checkAndGetTransport()
	{
	   Lock guard(&m_channelMutex);
	   // TODO C-fy
		if (m_connectionState == DESTROYED)
			throw std::runtime_error("Channel destroyed.");
		else if (m_connectionState != CONNECTED)
			throw  std::runtime_error("Channel not connected.");
		return m_transport;		// TODO transport can be 0 !!!!!!!!!!
	}

	virtual Transport* getTransport()
	{
	   Lock guard(&m_channelMutex);
		return m_transport;
	}

	virtual void transportResponsive(Transport* transport) {
	   Lock guard(&m_channelMutex);
		if (m_connectionState == DISCONNECTED)
		{
			updateSubscriptions();

			// reconnect using existing IDs, data
			connectionCompleted(m_serverChannelID/*, accessRights*/);
		}
	}

	void transportUnresponsive() {
	   Lock guard(&m_channelMutex);
		if (m_connectionState == CONNECTED)
		{
			// NOTE: 2 types of disconnected state - distinguish them
			setConnectionState(DISCONNECTED);

			// ... CA notifies also w/ no access rights callback, although access right are not changed
		}
	}

	/**
	 * Set connection state and if changed, notifies listeners.
	 * @param newState	state to set.
	 */
	void setConnectionState(ConnectionState connectionState)
	{
	   Lock guard(&m_channelMutex);
		if (m_connectionState != connectionState)
		{
			m_connectionState = connectionState;

			//bool connectionStatusToReport = (connectionState == CONNECTED);
			//if (connectionStatusToReport != lastReportedConnectionState)
			{
				//lastReportedConnectionState = connectionStatusToReport;
				// TODO via dispatcher ?!!!
				m_requester->channelStateChange(this, connectionState);
			}
		}
	}

	virtual void lock() {
		// noop
	}


	virtual void send(ByteBuffer* buffer, TransportSendControl* control) {
	   m_channelMutex.lock();
	   bool issueCreateMessage = m_issueCreateMessage;
	   m_channelMutex.unlock();

		if (issueCreateMessage)
		{
			control->startMessage((int8)7, 2+4);

			// count
			buffer->putShort((int16)1);
			// array of CIDs and names
			buffer->putInt(m_channelID);
			SerializeHelper::serializeString(m_name, buffer, control);
			// send immediately
			// TODO
			control->flush(true);
		}
		else
		{
			control->startMessage((int8)8, 4+4);
			// SID
	   m_channelMutex.lock();
	   pvAccessID sid = m_serverChannelID;
	   m_channelMutex.unlock();
			buffer->putInt(sid);
			// CID
			buffer->putInt(m_channelID);
			// send immediately
			// TODO
			control->flush(true);
		}
	}

	virtual void unlock() {
		// noop
	}


	/**
	 * Disconnects (destroys) all channels pending IO.
	 * @param destroy	<code>true</code> if channel is being destroyed.
	 */
	void disconnectPendingIO(bool destroy)
	{
// TODO
	}

	/**
	 * Resubscribe subscriptions.
	 */
	// TODO to be called from non-transport thread !!!!!!
	void resubscribeSubscriptions()
	{
// TODO
	}

	/**
	 * Update subscriptions.
	 */
	// TODO to be called from non-transport thread !!!!!!
	void updateSubscriptions()
	{
// TODO
	}

	virtual void getField(GetFieldRequester *requester,epics::pvData::String subField)
    {
		new ChannelGetFieldRequestImpl(this, requester, subField);
    }

    virtual ChannelProcess* createChannelProcess(
            ChannelProcessRequester *channelProcessRequester,
            epics::pvData::PVStructure *pvRequest)
    {
		return new ChannelProcessRequestImpl(this, channelProcessRequester, pvRequest);
    }

    virtual ChannelGet* createChannelGet(
            ChannelGetRequester *channelGetRequester,
            epics::pvData::PVStructure *pvRequest)
    {
    	return new ChannelGetImpl(this, channelGetRequester, pvRequest);
    }

    virtual ChannelPut* createChannelPut(
            ChannelPutRequester *channelPutRequester,
            epics::pvData::PVStructure *pvRequest)
    {
    	return new ChannelPutImpl(this, channelPutRequester, pvRequest);
    }

    virtual ChannelPutGet* createChannelPutGet(
            ChannelPutGetRequester *channelPutGetRequester,
            epics::pvData::PVStructure *pvRequest)
    {
    	// TODO return new ChannelPutGetImpl(this, channelPutGetRequester, pvRequest);
    	return 0;
    }

    virtual ChannelRPC* createChannelRPC(ChannelRPCRequester *channelRPCRequester,
            epics::pvData::PVStructure *pvRequest)
    {
        // TODO
        return 0;
    }

    virtual epics::pvData::Monitor* createMonitor(
            epics::pvData::MonitorRequester *monitorRequester,
            epics::pvData::PVStructure *pvRequest)
    {
    	return new ChannelMonitorImpl(this, monitorRequester, pvRequest);	
    }

    virtual ChannelArray* createChannelArray(
            ChannelArrayRequester *channelArrayRequester,
            epics::pvData::PVStructure *pvRequest)
    {
        // TODO
        return 0;
    }



    virtual void printInfo() {
        String info;
        printInfo(&info);
        std::cout << info.c_str() << std::endl;
    }

    virtual void printInfo(epics::pvData::StringBuilder out) {
        //Lock lock(&m_channelMutex);
        //std::ostringstream ostr;
        //static String emptyString;

		out->append(  "CHANNEL  : "); out->append(m_name);
		out->append("\nSTATE    : "); out->append(ConnectionStateNames[m_connectionState]);
		if (m_connectionState == CONNECTED)
		{
       		out->append("\nADDRESS  : "); out->append(getRemoteAddress());
			//out->append("\nRIGHTS   : "); out->append(getAccessRights());
		}
		out->append("\n");
    }
};


    class ChannelProviderImpl;

    class ChannelImplFind : public ChannelFind
    {
        public:
        ChannelImplFind(ChannelProvider* provider) : m_provider(provider)
        {
        }

        virtual void destroy()
        {
            // one instance for all, do not delete at all
        }

        virtual ChannelProvider* getChannelProvider()
        {
            return m_provider;
        };

        virtual void cancelChannelFind()
        {
            throw std::runtime_error("not supported");
        }

        private:

        // only to be destroyed by it
        friend class ChannelProviderImpl;
        virtual ~ChannelImplFind() {}

        ChannelProvider* m_provider;
    };

    class ChannelProviderImpl : public ChannelProvider {
        public:

        ChannelProviderImpl(ClientContextImpl* context) :
            m_context(context) {
        }

        virtual epics::pvData::String getProviderName()
        {
            return "ChannelProviderImpl";
        }

        virtual void destroy()
        {
            delete this;
        }

        virtual ChannelFind* channelFind(
            epics::pvData::String channelName,
            ChannelFindRequester *channelFindRequester)
        {
            m_context->checkChannelName(channelName);

    		if (!channelFindRequester)
    			throw std::runtime_error("null requester");

            std::auto_ptr<Status> errorStatus(getStatusCreate()->createStatus(STATUSTYPE_ERROR, "not implemented", 0));
            channelFindRequester->channelFindResult(errorStatus.get(), 0, false);
            return 0;
        }

        virtual Channel* createChannel(
            epics::pvData::String channelName,
            ChannelRequester *channelRequester,
            short priority)
        {
            return createChannel(channelName, channelRequester, priority, emptyString);
        }

        virtual Channel* createChannel(
            epics::pvData::String channelName,
            ChannelRequester *channelRequester,
            short priority,
            epics::pvData::String address)
        {
            // TODO support addressList
    		Channel* channel = m_context->createChannelInternal(channelName, channelRequester, priority, 0);
    		if (channel)
          	     channelRequester->channelCreated(g_statusOK, channel);
    	    return channel;

    		// NOTE it's up to internal code to respond w/ error to requester and return 0 in case of errors
        }

        private:
        ~ChannelProviderImpl() {};

        /* TODO static*/ String emptyString;
        ClientContextImpl* m_context;
    };

    public:
    TestClientContextImpl() : 
    	m_addressList(""), m_autoAddressList(true), m_connectionTimeout(30.0f), m_beaconPeriod(15.0f),
    	m_broadcastPort(CA_BROADCAST_PORT), m_receiveBufferSize(MAX_TCP_RECV), m_timer(0),
    	m_broadcastTransport(0), m_searchTransport(0), m_connector(0), m_transportRegistry(0),
    	m_namedLocker(0), m_lastCID(0), m_lastIOID(0), m_channelSearchManager(0),
        m_version(new Version("CA Client", "cpp", 0, 0, 0, 1)),
        m_provider(new ChannelProviderImpl(this)),
        m_contextState(CONTEXT_NOT_INITIALIZED), m_configuration(new SystemConfigurationImpl())
    {
        loadConfiguration();
    }

    virtual Configuration* getConfiguration() {
/*
TODO
		final ConfigurationProvider configurationProvider = ConfigurationFactory.getProvider();
		Configuration config = configurationProvider.getConfiguration("pvAccess-client");
		if (config == null)
			config = configurationProvider.getConfiguration("system");
		return config;
*/
        return m_configuration;
    }

    virtual Version* getVersion() {
        return m_version;
    }

    virtual ChannelProvider* getProvider() {
        Lock lock(&m_contextMutex);
        return m_provider;
    }

    virtual Timer* getTimer()
    {
        Lock lock(&m_contextMutex);
        return m_timer;
    }

    virtual TransportRegistry* getTransportRegistry()
    {
        Lock lock(&m_contextMutex);
        return m_transportRegistry;
    }

    virtual BlockingUDPTransport* getSearchTransport()
    {
        Lock lock(&m_contextMutex);
        return m_searchTransport;
    }

    virtual void initialize() {
        Lock lock(&m_contextMutex);

		if (m_contextState == CONTEXT_DESTROYED)
			throw std::runtime_error("Context destroyed.");
		else if (m_contextState == CONTEXT_INITIALIZED)
			throw std::runtime_error("Context already initialized.");

		internalInitialize();

		m_contextState = CONTEXT_INITIALIZED;
    }

    virtual void printInfo() {
        String info;
        printInfo(&info);
        std::cout << info.c_str() << std::endl;
    }

    virtual void printInfo(epics::pvData::StringBuilder out) {
        Lock lock(&m_contextMutex);
        std::ostringstream ostr;
        static String emptyString;

		out->append(  "CLASS : ::epics::pvAccess::ClientContextImpl");
		out->append("\nVERSION : "); out->append(m_version->getVersionString());
		out->append("\nADDR_LIST : "); ostr << m_addressList; out->append(ostr.str()); ostr.str(emptyString);
		out->append("\nAUTO_ADDR_LIST : ");  out->append(m_autoAddressList ? "true" : "false");
		out->append("\nCONNECTION_TIMEOUT : "); ostr << m_connectionTimeout; out->append(ostr.str()); ostr.str(emptyString);
		out->append("\nBEACON_PERIOD : "); ostr << m_beaconPeriod; out->append(ostr.str()); ostr.str(emptyString);
		out->append("\nBROADCAST_PORT : "); ostr << m_broadcastPort; out->append(ostr.str()); ostr.str(emptyString);
		out->append("\nRCV_BUFFER_SIZE : "); ostr << m_receiveBufferSize; out->append(ostr.str()); ostr.str(emptyString);
		out->append("\nSTATE : ");
		switch (m_contextState)
		{
			case CONTEXT_NOT_INITIALIZED:
				out->append("CONTEXT_NOT_INITIALIZED");
				break;
			case CONTEXT_INITIALIZED:
				out->append("CONTEXT_INITIALIZED");
				break;
			case CONTEXT_DESTROYED:
				out->append("CONTEXT_DESTROYED");
				break;
			default:
				out->append("UNKNOWN");
		}
		out->append("\n");
    }

    virtual void destroy()
    {
       m_contextMutex.lock();

		if (m_contextState == CONTEXT_DESTROYED)
		{
		    m_contextMutex.unlock();
			throw std::runtime_error("Context already destroyed.");
		}

		// go into destroyed state ASAP
		m_contextState =  CONTEXT_DESTROYED;

		internalDestroy();
    }

    virtual void dispose()
    {
        destroy();
    }

    private:
    ~TestClientContextImpl() {};

    void loadConfiguration() {
		m_addressList = m_configuration->getPropertyAsString("EPICS4_CA_ADDR_LIST", m_addressList);
		m_autoAddressList = m_configuration->getPropertyAsBoolean("EPICS4_CA_AUTO_ADDR_LIST", m_autoAddressList);
		m_connectionTimeout = m_configuration->getPropertyAsFloat("EPICS4_CA_CONN_TMO", m_connectionTimeout);
		m_beaconPeriod = m_configuration->getPropertyAsFloat("EPICS4_CA_BEACON_PERIOD", m_beaconPeriod);
		m_broadcastPort = m_configuration->getPropertyAsInteger("EPICS4_CA_BROADCAST_PORT", m_broadcastPort);
		m_receiveBufferSize = m_configuration->getPropertyAsInteger("EPICS4_CA_MAX_ARRAY_BYTES", m_receiveBufferSize);
    }

    void internalInitialize() {

		m_timer = new Timer("pvAccess-client timer", lowPriority);
		m_connector = new BlockingTCPConnector(this, m_receiveBufferSize, m_beaconPeriod);
		m_transportRegistry = new TransportRegistry();
		m_namedLocker = new NamedLockPattern<String>();

		// setup UDP transport
		initializeUDPTransport();
		// TODO what if initialization failed!!!

		// setup search manager
		m_channelSearchManager = new ChannelSearchManager(this);
    }

	/**
	 * Initialized UDP transport (broadcast socket and repeater connection).
	 */
	bool initializeUDPTransport() {

		// quary broadcast addresses of all IFs
        SOCKET socket = epicsSocketCreate(AF_INET, SOCK_DGRAM, 0);
        if (socket == INVALID_SOCKET) return false;
        auto_ptr<InetAddrVector> broadcastAddresses(getBroadcastAddresses(socket, m_broadcastPort));
        epicsSocketDestroy (socket);

		// set broadcast address list
		if (!m_addressList.empty())
		{
			// if auto is true, add it to specified list
			InetAddrVector* appendList = 0;
			if (m_autoAddressList)
				appendList = broadcastAddresses.get();

			auto_ptr<InetAddrVector> list(getSocketAddressList(m_addressList, m_broadcastPort, appendList));
			if (list.get() && list->size()) {
			    // delete old list and take ownership of a new one
			    broadcastAddresses = list;
			}
		}

		// where to bind (listen) address
        osiSockAddr listenLocalAddress;
        listenLocalAddress.ia.sin_family = AF_INET;
        listenLocalAddress.ia.sin_port = htons(m_broadcastPort);
        listenLocalAddress.ia.sin_addr.s_addr = htonl(INADDR_ANY);

		auto_ptr<BlockingUDPConnector> broadcastConnector(new BlockingUDPConnector(true, true));
		m_broadcastTransport = (BlockingUDPTransport*)broadcastConnector->connect(
					0, new ClientResponseHandler(this),
					listenLocalAddress, CA_MINOR_PROTOCOL_REVISION,
					CA_DEFAULT_PRIORITY);
		if (!m_broadcastTransport)
		     return false;
		m_broadcastTransport->setBroadcastAddresses(broadcastAddresses.get());

		// undefined address
        osiSockAddr undefinedAddress;
        undefinedAddress.ia.sin_family = AF_INET;
        undefinedAddress.ia.sin_port = htons(0);
        undefinedAddress.ia.sin_addr.s_addr = htonl(INADDR_ANY);

		auto_ptr<BlockingUDPConnector> searchConnector(new BlockingUDPConnector(false, true));
		m_searchTransport = (BlockingUDPTransport*)searchConnector->connect(
									0, new ClientResponseHandler(this),
									undefinedAddress, CA_MINOR_PROTOCOL_REVISION,
									CA_DEFAULT_PRIORITY);
		if (!m_searchTransport)
		     return false;
		m_searchTransport->setBroadcastAddresses(broadcastAddresses.get());
		
        // become active
		m_broadcastTransport->start();
		m_searchTransport->start();

        return true;
	}

    void internalDestroy() {

		// stop searching
		if (m_channelSearchManager)
			delete m_channelSearchManager; //->destroy();

		// stop timer
		if (m_timer)
			delete m_timer;

		//
		// cleanup
		//

		// this will also close all CA transports
		destroyAllChannels();

		// TODO destroy !!!
		if (m_broadcastTransport)
			delete m_broadcastTransport; //->destroy(true);
		if (m_searchTransport)
			delete m_searchTransport; //->destroy(true);

		if (m_namedLocker) delete m_namedLocker;
		if (m_transportRegistry) delete m_transportRegistry;
		if (m_connector) delete m_connector;
		if (m_configuration) delete m_configuration;

        m_provider->destroy();
        delete m_version;
		m_contextMutex.unlock();
        delete this;
    }

    void destroyAllChannels() {
        // TODO
    }

	/**
	 * Check channel name.
	 */
	void checkChannelName(String& name) {
		if (name.empty())
			throw std::runtime_error("null or empty channel name");
		else if (name.length() > UNREASONABLE_CHANNEL_NAME_LENGTH)
			throw std::runtime_error("name too long");
	}

	/**
	 * Check context state and tries to establish necessary state.
	 */
	void checkState() {
        Lock lock(&m_contextMutex); // TODO check double-lock?!!!

		if (m_contextState == CONTEXT_DESTROYED)
			throw std::runtime_error("Context destroyed.");
		else if (m_contextState == CONTEXT_NOT_INITIALIZED)
			initialize();
	}

	/**
	 * Register channel.
	 * @param channel
	 */
	void registerChannel(ChannelImpl* channel)
	{
	   Lock guard(&m_cidMapMutex);
	   m_channelsByCID[channel->getChannelID()] = channel;
	}

	/**
	 * Unregister channel.
	 * @param channel
	 */
	void unregisterChannel(ChannelImpl* channel)
	{
	   Lock guard(&m_cidMapMutex);
       m_channelsByCID.erase(channel->getChannelID());
	}

	/**
	 * Searches for a channel with given channel ID.
	 * @param channelID CID.
	 * @return channel with given CID, <code>0</code> if non-existent.
	 */
	ChannelImpl* getChannel(pvAccessID channelID)
	{
	   Lock guard(&m_cidMapMutex);
	   CIDChannelMap::iterator it = m_channelsByCID.find(channelID);
	   return (it == m_channelsByCID.end() ? 0 : it->second);
	}

	/**
	 * Generate Client channel ID (CID).
	 * @return Client channel ID (CID).
	 */
	pvAccessID generateCID()
	{
        Lock guard(&m_cidMapMutex);

        // search first free (theoretically possible loop of death)
        while (m_channelsByCID.find(++m_lastCID) != m_channelsByCID.end());
        // reserve CID
        m_channelsByCID[m_lastCID] = 0;
        return m_lastCID;
	}

	/**
	 * Free generated channel ID (CID).
	 */
	void freeCID(int cid)
	{
        Lock guard(&m_cidMapMutex);
        m_channelsByCID.erase(cid);
	}


	/**
	 * Searches for a response request with given channel IOID.
	 * @param ioid	I/O ID.
	 * @return request response with given I/O ID.
	 */
	ResponseRequest* getResponseRequest(pvAccessID ioid)
	{
	   Lock guard(&m_ioidMapMutex);
	   IOIDResponseRequestMap::iterator it = m_pendingResponseRequests.find(ioid);
	   return (it == m_pendingResponseRequests.end() ? 0 : it->second);
	}

	/**
	 * Register response request.
	 * @param request request to register.
	 * @return request ID (IOID).
	 */
	pvAccessID registerResponseRequest(ResponseRequest* request)
	{
	   Lock guard(&m_ioidMapMutex);
	   pvAccessID ioid = generateIOID();
	   m_pendingResponseRequests[ioid] = request;
	   return ioid;
	}

	/**
	 * Unregister response request.
	 * @param request
	 * @return removed object, can be <code>null</code>
	 */
	ResponseRequest* unregisterResponseRequest(ResponseRequest* request)
	{
	   Lock guard(&m_ioidMapMutex);
	   IOIDResponseRequestMap::iterator it = m_pendingResponseRequests.find(request->getIOID());
	   if (it == m_pendingResponseRequests.end())
	       return 0;

       ResponseRequest* retVal = it->second;
	   m_pendingResponseRequests.erase(it);
	   return retVal;
	}

	/**
	 * Generate IOID.
	 * @return IOID. 
	 */
	pvAccessID generateIOID()
	{
	   Lock guard(&m_ioidMapMutex);


        // search first free (theoretically possible loop of death)
        while (m_pendingResponseRequests.find(++m_lastIOID) != m_pendingResponseRequests.end());
        // reserve IOID
        m_pendingResponseRequests[m_lastIOID] = 0;
        return m_lastIOID;
	}

	/**
	 * Called each time beacon anomaly is detected. 
	 */
	void beaconAnomalyNotify()
	{
		if (m_channelSearchManager)
			m_channelSearchManager->beaconAnomalyNotify();
	}

	/**
	 * Get (and if necessary create) beacon handler.
	 * @param responseFrom remote source address of received beacon.	
	 * @return beacon handler for particular server.
	 */
	BeaconHandler* getBeaconHandler(osiSockAddr* responseFrom)
	{
	   // TODO delete handlers
	   Lock guard(&m_beaconMapMutex);
	   AddressBeaconHandlerMap::iterator it = m_beaconHandlers.find(*responseFrom);
	   BeaconHandler* handler;
	   if (it == m_beaconHandlers.end())
	   {
	       handler = new BeaconHandler(this, responseFrom);
	       m_beaconHandlers[*responseFrom] = handler;
	   }
	   else
	       handler = it->second;
	   return handler;
	}

	/**
	 * Get, or create if necessary, transport of given server address.
	 * @param serverAddress	required transport address
	 * @param priority process priority.
	 * @return transport for given address
	 */
	Transport* getTransport(TransportClient* client, osiSockAddr* serverAddress, int16 minorRevision, int16 priority)
	{
		try
		{
			return m_connector->connect(client, new ClientResponseHandler(this), *serverAddress, minorRevision, priority);
		}
		catch (...)
		{
		  // TODO log
		  printf("failed to get transport\n");
		  return 0;
		}
	}

		/**
	 * Internal create channel.
	 */
	// TODO no minor version with the addresses
	// TODO what if there is an channel with the same name, but on different host!
	ChannelImpl* createChannelInternal(String name, ChannelRequester* requester, short priority,
			InetAddrVector* addresses) { // TODO addresses

		checkState();
		checkChannelName(name);

		if (requester == 0)
			throw std::runtime_error("null requester");

		if (priority < ChannelProvider::PRIORITY_MIN || priority > ChannelProvider::PRIORITY_MAX)
			throw std::range_error("priority out of bounds");

		bool lockAcquired = true; // TODO namedLocker->acquireSynchronizationObject(name, LOCK_TIMEOUT);
		if (lockAcquired)
		{
			try
			{
		  	    pvAccessID cid = generateCID();
				return new TestChannelImpl(this, cid, name, requester, priority, addresses);
			}
			catch(...) {
			 // TODO
			 return 0;
			}
			// TODO namedLocker.releaseSynchronizationObject(name);
		}
		else
		{
		  // TODO is this OK?
			throw std::runtime_error("Failed to obtain synchronization lock for '" + name + "', possible deadlock.");
		}
	}

	/**
	 * Destroy channel.
	 * @param channel
	 * @param force
	 * @throws CAException
	 * @throws IllegalStateException
	 */
	void destroyChannel(ChannelImpl* channel, bool force) {

		String name = channel->getChannelName();
		bool lockAcquired = true; //namedLocker->acquireSynchronizationObject(name, LOCK_TIMEOUT);
		if (lockAcquired)
		{
			try
			{
				channel->destroyChannel(force);
			}
			catch(...) {
			 // TODO
			}
			// TODO	namedLocker->releaseSynchronizationObject(channel.getChannelName());
		}
		else
		{
		  // TODO is this OK?
		  throw std::runtime_error("Failed to obtain synchronization lock for '" + name + "', possible deadlock.");
		}
	}

	/**
	 * Get channel search manager.
	 * @return channel search manager.
	 */
	ChannelSearchManager* getChannelSearchManager() {
		return m_channelSearchManager;
	}

	/**
	 * A space-separated list of broadcast address for process variable name resolution.
	 * Each address must be of the form: ip.number:port or host.name:port
	 */
	String m_addressList;

	/**
	 * Define whether or not the network interfaces should be discovered at runtime.
	 */
	bool m_autoAddressList;

	/**
	 * If the context doesn't see a beacon from a server that it is connected to for
	 * connectionTimeout seconds then a state-of-health message is sent to the server over TCP/IP.
	 * If this state-of-health message isn't promptly replied to then the context will assume that
	 * the server is no longer present on the network and disconnect.
	 */
	float m_connectionTimeout;

	/**
	 * Period in second between two beacon signals.
	 */
	float m_beaconPeriod;

	/**
	 * Broadcast (beacon, search) port number to listen to.
	 */
	int m_broadcastPort;

	/**
	 * Receive buffer size (max size of payload).
	 */
	int m_receiveBufferSize;

	/**
	 * Timer.
	 */
	Timer* m_timer;

	/**
	 * Broadcast transport needed to listen for broadcasts.
	 */
	BlockingUDPTransport* m_broadcastTransport;

	/**
	 * UDP transport needed for channel searches.
	 */
	BlockingUDPTransport* m_searchTransport;

	/**
	 * CA connector (creates CA virtual circuit).
	 */
	BlockingTCPConnector* m_connector;

	/**
	 * CA transport (virtual circuit) registry.
	 * This registry contains all active transports - connections to CA servers.
	 */
	TransportRegistry* m_transportRegistry;

	/**
	 * Context instance.
	 */
	NamedLockPattern<String>* m_namedLocker;

	/**
	 * Context instance.
	 */
	static const int LOCK_TIMEOUT = 20 * 1000;	// 20s

	/**
	 * Map of channels (keys are CIDs).
	 */
	 // TODO consider std::unordered_map
	typedef std::map<pvAccessID, ChannelImpl*> CIDChannelMap;
	CIDChannelMap m_channelsByCID;

    /**
     *  CIDChannelMap mutex.
     */
    Mutex m_cidMapMutex;

	/**
	 * Last CID cache.
	 */
	pvAccessID m_lastCID;

	/**
	 * Map of pending response requests (keys are IOID).
	 */
	 // TODO consider std::unordered_map
	typedef std::map<pvAccessID, ResponseRequest*> IOIDResponseRequestMap;
	IOIDResponseRequestMap m_pendingResponseRequests;

    /**
     *  IOIDResponseRequestMap mutex.
     */
    Mutex m_ioidMapMutex;

	/**
	 * Last IOID cache.
	 */
	pvAccessID m_lastIOID;

	/**
	 * Channel search manager.
	 * Manages UDP search requests.
	 */
	ChannelSearchManager* m_channelSearchManager;

	/**
	 * Beacon handler map.
	 */
	 // TODO consider std::unordered_map
	typedef std::map<osiSockAddr, BeaconHandler*, comp_osiSock_lt> AddressBeaconHandlerMap;
	AddressBeaconHandlerMap m_beaconHandlers;

    /**
     *  IOIDResponseRequestMap mutex.
     */
    Mutex m_beaconMapMutex;

	/**
	 * Version.
	 */
    Version* m_version;

	/**
	 * Provider implementation.
	 */
    ChannelProviderImpl* m_provider;

    /**
     * Context state.
     */
    ContextState m_contextState;

    /**
     * Context sync. mutex.
     */
    Mutex m_contextMutex;

    friend class ChannelProviderImpl;

    Configuration* m_configuration;
};


class ChannelFindRequesterImpl : public ChannelFindRequester
{
    virtual void channelFindResult(epics::pvData::Status *status,ChannelFind *channelFind,bool wasFound)
    {
        std::cout << "[ChannelFindRequesterImpl] channelFindResult("
                  << status->toString() << ", ..., " << wasFound << ")" << std::endl;
    }
};

class ChannelRequesterImpl : public ChannelRequester
{
    virtual String getRequesterName()
    {
        return "ChannelRequesterImpl";
    };

    virtual void message(String message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << messageTypeName[messageType] << ")" << std::endl;
    }

    virtual void channelCreated(epics::pvData::Status* status, Channel *channel)
    {
        std::cout << "channelCreated(" << status->toString() << ", "
                  << (channel ? channel->getChannelName() : "(null)") << ")" << std::endl;
    }

    virtual void channelStateChange(Channel *c, ConnectionState connectionState)
    {
        std::cout << "channelStateChange(" << c->getChannelName() << ", " << ConnectionStateNames[connectionState] << ")" << std::endl;
    }
};

class GetFieldRequesterImpl : public GetFieldRequester
{
    virtual String getRequesterName()
    {
        return "GetFieldRequesterImpl";
    };

    virtual void message(String message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << messageTypeName[messageType] << ")" << std::endl;
    }

    virtual void getDone(epics::pvData::Status *status,epics::pvData::FieldConstPtr field)
    {
        std::cout << "getDone(" << status->toString() << ", ";
        if (field)
        {
            String str;
            field->toString(&str);
            std::cout << str;
        }
        else
            std::cout << "(null)";
        std::cout << ")" << std::endl;
    }
};

class ChannelGetRequesterImpl : public ChannelGetRequester
{
    ChannelGet *m_channelGet;
    epics::pvData::PVStructure *m_pvStructure;
    epics::pvData::BitSet *m_bitSet;

    virtual String getRequesterName()
    {
        return "ChannelGetRequesterImpl";
    };

    virtual void message(String message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << messageTypeName[messageType] << ")" << std::endl;
    }

    virtual void channelGetConnect(epics::pvData::Status *status,ChannelGet *channelGet,
            epics::pvData::PVStructure *pvStructure,epics::pvData::BitSet *bitSet)
    {
        std::cout << "channelGetConnect(" << status->toString() << ")" << std::endl;
        if (pvStructure)
        {
        	String st;
        	pvStructure->toString(&st);
            std::cout << st << std::endl;
        }
        
        // TODO sync
        m_channelGet = channelGet;
        m_pvStructure = pvStructure;
        m_bitSet = bitSet;
    }

    virtual void getDone(epics::pvData::Status *status)
    {
        std::cout << "getDone(" << status->toString() << ")" << std::endl;
        if (m_pvStructure)
        {
            String str;
            m_pvStructure->toString(&str);
            std::cout << str;
            std::cout << std::endl;
        }
    }
};

class ChannelPutRequesterImpl : public ChannelPutRequester
{
    ChannelPut *m_channelPut;
    epics::pvData::PVStructure *m_pvStructure;
    epics::pvData::BitSet *m_bitSet;

    virtual String getRequesterName()
    {
        return "ChannelPutRequesterImpl";
    };

    virtual void message(String message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << messageTypeName[messageType] << ")" << std::endl;
    }

    virtual void channelPutConnect(epics::pvData::Status *status,ChannelPut *channelPut,
            epics::pvData::PVStructure *pvStructure,epics::pvData::BitSet *bitSet)
    {
        std::cout << "channelPutConnect(" << status->toString() << ")" << std::endl;

        // TODO sync
        m_channelPut = channelPut;
        m_pvStructure = pvStructure;
        m_bitSet = bitSet;
    }

    virtual void getDone(epics::pvData::Status *status)
    {
        std::cout << "getDone(" << status->toString() << ")" << std::endl;
        if (m_pvStructure)
        {
            String str;
            m_pvStructure->toString(&str);
            std::cout << str;
            std::cout << std::endl;
        }
    }

    virtual void putDone(epics::pvData::Status *status)
    {
        std::cout << "putDone(" << status->toString() << ")" << std::endl;
        if (m_pvStructure)
        {
            String str;
            m_pvStructure->toString(&str);
            std::cout << str;
            std::cout << std::endl;
        }
    }

};


class MonitorRequesterImpl : public MonitorRequester
{
    virtual String getRequesterName()
    {
        return "MonitorRequesterImpl";
    };

    virtual void message(String message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << messageTypeName[messageType] << ")" << std::endl;
    }

    virtual void monitorConnect(Status* status, Monitor* monitor, Structure* structure)
    {
        std::cout << "monitorConnect(" << status->toString() << ")" << std::endl;
        if (structure)
        {
            String str;
            structure->toString(&str);
            std::cout << str << std::endl;
        }
    }

    virtual void monitorEvent(Monitor* monitor)
    {
        std::cout << "monitorEvent" << std::endl;

        MonitorElement* element = monitor->poll();

        String str("changed/overrun ");
        element->getChangedBitSet()->toString(&str);
        str += '/';
        element->getOverrunBitSet()->toString(&str);
        str += '\n';
        element->getPVStructure()->toString(&str);
        std::cout << str << std::endl;

        monitor->release(element);
    }

    virtual void unlisten(Monitor* monitor)
    {
        std::cout << "unlisten" << std::endl;
    }
};


class ChannelProcessRequesterImpl : public ChannelProcessRequester
{
    ChannelProcess *m_channelProcess;

    virtual String getRequesterName()
    {
        return "ProcessRequesterImpl";
    };

    virtual void message(String message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << messageTypeName[messageType] << ")" << std::endl;
    }

    virtual void channelProcessConnect(epics::pvData::Status *status,ChannelProcess *channelProcess)
    {
        std::cout << "channelProcessConnect(" << status->toString() << ")" << std::endl;

        // TODO sync
        m_channelProcess = channelProcess;
    }

    virtual void processDone(epics::pvData::Status *status)
    {
        std::cout << "processDone(" << status->toString() << ")" << std::endl;
    }

};


int main(int argc,char *argv[])
{
    TestClientContextImpl* context = new TestClientContextImpl();
    context->printInfo();

    context->initialize();
    context->printInfo();

    epicsThreadSleep ( 1.0 );

    //ChannelFindRequesterImpl findRequester;
    //context->getProvider()->channelFind("something", &findRequester);

    ChannelRequesterImpl channelRequester;
    Channel* channel = context->getProvider()->createChannel("structureArrayTest", &channelRequester);

    epicsThreadSleep ( 1.0 );

    channel->printInfo();
/*
    GetFieldRequesterImpl getFieldRequesterImpl;
    channel->getField(&getFieldRequesterImpl, "");
    epicsThreadSleep ( 1.0 );

    ChannelProcessRequesterImpl channelProcessRequester;
    ChannelProcess* channelProcess = channel->createChannelProcess(&channelProcessRequester, 0);
    epicsThreadSleep ( 1.0 );
    channelProcess->process(false);
    epicsThreadSleep ( 1.0 );
    channelProcess->destroy();
    epicsThreadSleep ( 1.0 );

    ChannelGetRequesterImpl channelGetRequesterImpl;
    PVStructure* pvRequest = getCreateRequest()->createRequest("field(value, timeStamp)",&channelGetRequesterImpl);
    ChannelGet* channelGet = channel->createChannelGet(&channelGetRequesterImpl, pvRequest);
    epicsThreadSleep ( 3.0 );
    channelGet->get(false);
    epicsThreadSleep ( 3.0 );
    channelGet->destroy();
    epicsThreadSleep ( 1.0 );


    ChannelPutRequesterImpl channelPutRequesterImpl;
    pvRequest = getCreateRequest()->createRequest("field(value, timeStamp)",&channelPutRequesterImpl);
    ChannelPut* channelPut = channel->createChannelPut(&channelPutRequesterImpl, pvRequest);
    epicsThreadSleep ( 1.0 );
    channelPut->get();
    epicsThreadSleep ( 1.0 );
    channelPut->put(false);
    epicsThreadSleep ( 1.0 );
    channelPut->destroy();
*/


    MonitorRequesterImpl monitorRequesterImpl;
    PVStructure* pvRequest = getCreateRequest()->createRequest("field(value, timeStamp)",&monitorRequesterImpl);
    Monitor* monitor = channel->createMonitor(&monitorRequesterImpl, pvRequest);

    epicsThreadSleep( 1.0 );

    Status* status = monitor->start();
    std::cout << "monitor->start() = " << status->toString() << std::endl;
    delete status;

    epicsThreadSleep( 3.0 );

    status = monitor->stop();
    std::cout << "monitor->stop() = " << status->toString() << std::endl;
    delete status;


    monitor->destroy();
    
    
    epicsThreadSleep ( 3.0 );
    printf("Destroying channel... \n");
    channel->destroy();
    printf("done.\n");

    epicsThreadSleep ( 3.0 );

    printf("Destroying context... \n");
    context->destroy();
    printf("done.\n");

    std::cout << "-----------------------------------------------------------------------" << std::endl;
    getShowConstructDestruct()->constuctDestructTotals(stdout);
    return(0);
}
