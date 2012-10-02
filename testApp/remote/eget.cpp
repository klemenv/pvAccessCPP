#include <iostream>
#include <pv/clientFactory.h>
#include <pv/pvAccess.h>

#include <stdio.h>
#include <epicsStdlib.h>
#include <epicsGetopt.h>
#include <epicsThread.h>
#include <pv/logger.h>
#include <pv/lock.h>

#include <vector>
#include <string>
#include <ostream>
#include <sstream>
#include <iomanip>
#include <map>

#include <pv/event.h>
#include <epicsExit.h>

using namespace std;
using namespace std::tr1;
using namespace epics::pvData;
using namespace epics::pvAccess;

class RequesterImpl : public Requester,
     public std::tr1::enable_shared_from_this<RequesterImpl>
{
public:

    virtual String getRequesterName()
    {
        return "RequesterImpl";
    };

    virtual void message(String const & message,MessageType messageType)
    {
        std::cout << "[" << getRequesterName() << "] message(" << message << ", " << getMessageTypeName(messageType) << ")" << std::endl;
    }
};


/// terse mode functions

void convertStructure(StringBuilder buffer,PVStructure *data,int notFirst);
void convertArray(StringBuilder buffer,PVScalarArray * pv,int notFirst);
void convertStructureArray(StringBuilder buffer,PVStructureArray * pvdata,int notFirst);








void convertToString(StringBuilder buffer,PVField * pv,int notFirst)
{
    Type type = pv->getField()->getType();
    if(type==structure) {
        convertStructure(buffer,static_cast<PVStructure*>(pv),notFirst);
        return;
    }
    if(type==scalarArray) {
        convertArray(buffer,static_cast<PVScalarArray *>(pv),notFirst);
        *buffer += "\t";
        return;
    }
    if(type==structureArray) {
    	convertStructureArray(
            buffer,static_cast<PVStructureArray*>(pv),notFirst);
        *buffer += "\t";
        return;
    }
    
    // scalar stringification
    std::stringstream sstream;
    sstream << std::boolalpha << *pv;
    *buffer += sstream.str();
    *buffer += "\t";
}

void convertStructure(StringBuilder buffer,PVStructure *data,int notFirst)
{
    PVFieldPtrArray fieldsData = data->getPVFields();
	int length = data->getStructure()->getNumberFields();
	for(int i=0; i<length; i++) {
		PVFieldPtr fieldField = fieldsData[i];
		convertToString(buffer,fieldField.get(),notFirst + 1);
	}
}

void convertArray(StringBuilder buffer,PVScalarArray * pv,int /*notFirst*/)
{
    // array stringification
    std::stringstream sstream;
    sstream << std::boolalpha << *pv;
    *buffer += sstream.str();
}

void convertStructureArray(StringBuilder buffer,
    PVStructureArray * pvdata,int notFirst)
{
    int length = pvdata->getLength();
    if(length<=0) {
        return;
    }
    StructureArrayData data = StructureArrayData();
    pvdata->get(0, length, data);
    for (int i = 0; i < length; i++) {
        PVStructurePtr pvStructure = data.data[i];
        if (pvStructure == 0) {
            *buffer += "null";
        } else {
            convertToString(buffer,pvStructure.get(),notFirst+1);
        }
    }
}











/* Converts a hex character to its integer value */
char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/* Converts an integer value to its hex character*/
char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

/* Returns a url-encoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_encode(char *str) {
  char *pstr = str, *buf = (char*)malloc(strlen(str) * 3 + 1), *pbuf = buf;
  bool firstEquals = true;
  while (*pstr) {
    if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~') 
      *pbuf++ = *pstr;
    else if (*pstr == ' ') 
      *pbuf++ = '+';
    else if (*pstr == '=' && firstEquals)
    { 
      firstEquals = false;
      *pbuf++ = '=';
    }
    else 
      *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
    pstr++;
  }
  *pbuf = '\0';
  return buf;
}



void formatNTAny(std::ostream& o, PVStructurePtr const & pvStruct)
{
    PVFieldPtr value = pvStruct->getSubField("value");
    if (value.get() == 0)
    {
    	std::cerr << "no 'value' column in NTAny" << std::endl;
        return;
    }

    o << *value;
}

void formatNTScalar(std::ostream& o, PVStructurePtr const & pvStruct)
{
    PVScalarPtr value = dynamic_pointer_cast<PVScalar>(pvStruct->getSubField("value"));
    if (value.get() == 0)
    {
    	std::cerr << "no scalar_t 'value' column in NTScalar" << std::endl;
        return;
    }

    o << *value;
}

void formatNTScalarArray(std::ostream& o, PVStructurePtr const & pvStruct)
{
    PVScalarArrayPtr value = dynamic_pointer_cast<PVScalarArray>(pvStruct->getSubField("value"));
    if (value.get() == 0)
    {
    	std::cerr << "no scalar_t[] 'value' column in NTScalarArray" << std::endl;
        return;
    }

    o << *value;
}

void formatNTTable(std::ostream& o, PVStructurePtr const & pvStruct)
{
    PVStringArrayPtr labels = dynamic_pointer_cast<PVStringArray>(pvStruct->getScalarArrayField("labels", pvString));
    if (labels.get() == 0)
    {
    	std::cerr << "no string[] 'labels' column in NTTable" << std::endl;
        return;
    }

    size_t numColumns = labels->getLength();
    if ((pvStruct->getPVFields().size()-1) < numColumns)
    {
    	std::cerr << "malformed NTTable, not enough of columns - " << numColumns << " column(s) expected" << std::endl;
		return;
    }
    
    // next numColumns fields are columns
    size_t maxValues = 0;
    vector<PVScalarArrayPtr> columnData;
    PVFieldPtrArray fields = pvStruct->getPVFields();
    for (size_t i = 0; i < numColumns; i++)
    {
        // TODO we relay on field ordering here (labels, <columns>)
    	PVScalarArrayPtr array = dynamic_pointer_cast<PVScalarArray>(fields[i+1]);
    	if (array.get() == 0)
    	{
        	std::cerr << "malformed NTTable, " << (i+1+1) << ". field is not scalar_t[]" << std::endl;
    		return;
    	}
    	size_t arrayLength = array->getLength();
    	if (maxValues < arrayLength) maxValues = arrayLength;
        columnData.push_back(array);
    }



    o << std::left;

    // first print labels
   	StringArrayData data;
    labels->get(0, numColumns, data);
    for (size_t i = 0; i < numColumns; i++)
    {
    	o << std::setw(16) << data.data[i];
    }
    o << std::endl;

    // then values
    for (size_t r = 0; r < maxValues; r++)
    {
        for (size_t i = 0; i < numColumns; i++)
        {
        	o << std::setw(16);
        	if (r < columnData[i]->getLength())
        		columnData[i]->dumpValue(o, r);
        	else
        		o << "";
        }
        o << std::endl;
    }

}    


void formatNTMatrix(std::ostream& o, PVStructurePtr const & pvStruct)
{
    PVDoubleArrayPtr value = dynamic_pointer_cast<PVDoubleArray>(pvStruct->getScalarArrayField("value", pvDouble));
    if (value.get() == 0)
    {
    	std::cerr << "no double[] 'value' column in NTMatrix" << std::endl;
        return;
    }

    int32 rows, cols;

    PVIntArrayPtr dim = dynamic_pointer_cast<PVIntArray>(pvStruct->getScalarArrayField("dim", pvInt));
    if (dim.get() != 0)
    {
		// dim[] = { rows, columns }
		size_t dims = dim->getLength();
		if (dims != 1 && dims != 2)
		{
			std::cerr << "malformed NTMatrix, dim[] must contain 1 or 2 elements instead of  " << dims << std::endl;
			return;
		}

	    IntArrayData data;
	    dim->get(0, dims, data);
	    rows = data.data[0];
	    cols = (dims == 2) ? data.data[1] : 1;

		if (rows <= 0 || cols <= 0)
		{
			std::cerr << "malformed NTMatrix, dim[] must contain elements >= 0" << std::endl;
			return;
		}
    }
    else
    {
    	rows = value->getLength();
    	cols = 1;
    }

    o << std::left;

    size_t len = static_cast<size_t>(rows*cols);
    if (len != value->getLength())
    {
		std::cerr << "malformed NTMatrix, values[] must contain " << len << " elements instead of  " << value->getLength() << std::endl;
		return;
    }

    size_t ix = 0;
    for (int32 r = 0; r < rows; r++)
    {
    	for (int32 c = 0; c < cols; c++)
    	{
        	o << std::setw(16);
        	value->dumpValue(o, ix++);
    	}
        o << std::endl;
    }
}

typedef void(*NTFormatterFunc)(std::ostream& o, PVStructurePtr const & pvStruct);
typedef map<String, NTFormatterFunc> NTFormatterLUTMap;
NTFormatterLUTMap ntFormatterLUT;

void initializeNTFormatterLUT()
{
	ntFormatterLUT["NTScalar"] = formatNTScalar;
	ntFormatterLUT["NTScalarArray"] = formatNTScalarArray;
	ntFormatterLUT["NTTable"] = formatNTTable;
	ntFormatterLUT["NTMatrix"] = formatNTMatrix;
	ntFormatterLUT["NTAny"] = formatNTAny;

	// StandardPV "support"
	ntFormatterLUT["scalar_t"] = formatNTScalar;
	ntFormatterLUT["scalarArray_t"] = formatNTScalarArray;
}

void toNTString(std::ostream& o, PVFieldPtr const & pv)
{
	static bool lutInitialized = false;
	if (!lutInitialized)
	{
		initializeNTFormatterLUT();
		lutInitialized = true;
	}

    Type type = pv->getField()->getType();
    if (type==structure)
    {
        PVStructurePtr pvStruct = static_pointer_cast<PVStructure>(pv);
        {
            String id = pvStruct->getField()->getID();

            NTFormatterLUTMap::const_iterator formatter = ntFormatterLUT.find(id);
            if (formatter != ntFormatterLUT.end())
            {
            	(formatter->second)(o, pvStruct);
            }
            else
            {
                std::cerr << "unsupported normative type" << std::endl;
                String buffer;
                pv->toString(&buffer);
                o << buffer;
            }

            return;
        }
    }
    
    // no ID, just dump
    String buffer;
    pv->toString(&buffer);
    o << buffer;
}








#define DEFAULT_TIMEOUT 3.0
#define DEFAULT_REQUEST "field(value)"

double timeOut = DEFAULT_TIMEOUT;
string request(DEFAULT_REQUEST);

enum PrintMode { ValueOnlyMode, StructureMode, TerseMode };
PrintMode mode = ValueOnlyMode;


void usage (void)
{
    fprintf (stderr, "\nUsage: eget [options] [<PV name>... | -s <service name>]\n\n"
    "  -h: Help: Print this message\n"
    "\noptions:\n"
    "  -s <service name>:   RPC based service name\n"
    "  -p <service param>:  Service parameter in form 'name=value'\n"
    "  -r <pv request>:     Request, specifies what fields to return and options, default is '%s'\n"
    "  -w <sec>:            Wait time, specifies timeout, default is %f second(s)\n"
    "  -t:                  Terse mode - print only value, without name\n"
    "  -d:                  Enable debug output\n"
    "  -c:                  Wait for clean shutdown and report used instance count (for expert users)"
    "\n\nexamples:\n\n"
"#! Get the value of the PV corr:li32:53:bdes\n"
"> eget corr:li32:53:bdes\n"
"\n"
"#! Get the table of all correctors from the rdb service\n"
"> eget -s rdbService -p entity=swissfel:devicenames\n"
"\n"
"#! Get the archive history of quad345:hist between 2 times, from the archive service\n"
"> eget -s archiveService -p entity=quad345:hist -p starttime=2012-02-12T10:04:56 -p endtime=2012-02-01T10:04:56\n"
"\n"
             , DEFAULT_REQUEST, DEFAULT_TIMEOUT);
}



class ChannelGetRequesterImpl : public ChannelGetRequester
{
    private:
    ChannelGet::shared_pointer m_channelGet;
    PVStructure::shared_pointer m_pvStructure;
    BitSet::shared_pointer m_bitSet;
    Mutex m_pointerMutex;
    Event m_event;
    String m_channelName;

    public:
    
    ChannelGetRequesterImpl(String channelName) : m_channelName(channelName) {};
    
    virtual String getRequesterName()
    {
        return "ChannelGetRequesterImpl";
    };

    virtual void message(String const & message,MessageType messageType)
    {
        std::cerr << "[" << getRequesterName() << "] message(" << message << ", " << getMessageTypeName(messageType) << ")" << std::endl;
    }

    virtual void channelGetConnect(const epics::pvData::Status& status,ChannelGet::shared_pointer const & channelGet,
                                   epics::pvData::PVStructure::shared_pointer const & pvStructure, 
                                   epics::pvData::BitSet::shared_pointer const & bitSet)
    {
        if (status.isSuccess())
        {
            // show warning
            if (!status.isOK())
            {
                std::cerr << "[" << m_channelName << "] channel get create: " << status.toString() << std::endl;
            }
            
            // assign smart pointers
            {
                Lock lock(m_pointerMutex);
                m_channelGet = channelGet;
                m_pvStructure = pvStructure;
                m_bitSet = bitSet;
            }
            
            channelGet->get(true);
        }
        else
        {
            std::cerr << "[" << m_channelName << "] failed to create channel get: " << status.toString() << std::endl;
        }
    }

    virtual void getDone(const epics::pvData::Status& status)
    {
        if (status.isSuccess())
        {
            // show warning
            if (!status.isOK())
            {
                std::cerr << "[" << m_channelName << "] channel get: " << status.toString() << std::endl;
            }

            // access smart pointers
            {
                Lock lock(m_pointerMutex);
                {
                    // needed since we access the data
                    ScopedLock dataLock(m_channelGet);
    
                    if (mode == ValueOnlyMode)
                    {
                    	PVFieldPtr pvField = m_pvStructure->getSubField("value");
                    	if (pvField.get())
                    	{
                    		PVScalarArrayPtr pvScalarArray = std::tr1::dynamic_pointer_cast<PVScalarArray>(pvField);
                    		if (pvScalarArray.get())
                    		{
                    			size_t len = pvScalarArray->getLength();
                    			for (size_t i = 0; i < len; i++)
                    			{
                        			(pvScalarArray.get())->dumpValue(std::cout, i) << std::endl;
                    			}
                    		}
                    		else
                    		{
                    			std::cout << *(pvField.get()) << std::endl;
                    		}
                    	}
                    	else
                    	{
                    		// do a structure mode, as fallback
                    		std::cerr << "no 'value' field" << std::endl;
                            String str;
                            m_pvStructure->toString(&str);
                            std::cout << str << std::endl;
                    	}
                    }
                    else if (mode == TerseMode)
                    {
                        String str;
                        convertToString(&str, m_pvStructure.get(), 0);
                        std::cout << str << std::endl;
                    }
                    else //if (mode == StructureMode)
                    {
                        String str;
                        m_pvStructure->toString(&str);
                        std::cout << str << std::endl;
                    }
                } 
                // this is OK since calle holds also owns it
                m_channelGet.reset();
            }
            
            m_event.signal();
            
        }
        else
        {
            std::cerr << "[" << m_channelName << "] failed to get: " << status.toString() << std::endl;
            {
                Lock lock(m_pointerMutex);
                // this is OK since calle holds also owns it
                m_channelGet.reset();
            }
        }
        
    }

    bool waitUntilGet(double timeOut)
    {
        return m_event.wait(timeOut);
    }
};

class ChannelRPCRequesterImpl : public ChannelRPCRequester
{
    private:
    ChannelRPC::shared_pointer m_channelRPC;
    Mutex m_pointerMutex;
    Event m_event;
    Event m_connectionEvent;
    String m_channelName;

    public:
    
    ChannelRPCRequesterImpl(String channelName) : m_channelName(channelName) {};
    
    virtual String getRequesterName()
    {
        return "ChannelRPCRequesterImpl";
    };

    virtual void message(String const & message,MessageType messageType)
    {
        std::cerr << "[" << getRequesterName() << "] message(" << message << ", " << getMessageTypeName(messageType) << ")" << std::endl;
    }

    virtual void channelRPCConnect(const epics::pvData::Status& status,ChannelRPC::shared_pointer const & channelRPC)
    {
        if (status.isSuccess())
        {
            // show warning
            if (!status.isOK())
            {
                std::cerr << "[" << m_channelName << "] channel RPC create: " << status.toString() << std::endl;
            }
            
            // assign smart pointers
            {
                Lock lock(m_pointerMutex);
                m_channelRPC = channelRPC;
            }
            
            m_connectionEvent.signal();
        }
        else
        {
            std::cerr << "[" << m_channelName << "] failed to create channel get: " << status.toString() << std::endl;
        }
    }

    virtual void requestDone (const epics::pvData::Status &status, epics::pvData::PVStructure::shared_pointer const &pvResponse)
    {
        if (status.isSuccess())
        {
            // show warning
            if (!status.isOK())
            {
                std::cerr << "[" << m_channelName << "] channel RPC: " << status.toString() << std::endl;
            }

            // access smart pointers
            {
                Lock lock(m_pointerMutex);

                toNTString(std::cout, pvResponse);
                std::cout << std::endl;

                // this is OK since calle holds also owns it
                m_channelRPC.reset();
            }
            
            m_event.signal();
            
        }
        else
        {
            std::cerr << "[" << m_channelName << "] failed to RPC: " << status.toString() << std::endl;
            {
                Lock lock(m_pointerMutex);
                // this is OK since calle holds also owns it
                m_channelRPC.reset();
            }
        }
        
    }
    
    /*
    void request(epics::pvData::PVStructure::shared_pointer const &pvRequest)
    {
        Lock lock(m_pointerMutex);
        m_channelRPC->request(pvRequest, false);
    }
    */

    bool waitUntilRPC(double timeOut)
    {
        return m_event.wait(timeOut);
    }

    bool waitUntilConnected(double timeOut)
    {
        return m_connectionEvent.wait(timeOut);
    }
};

class ChannelRequesterImpl : public ChannelRequester
{
private:
    Event m_event;    
    
public:
    
    virtual String getRequesterName()
    {
        return "ChannelRequesterImpl";
    };

    virtual void message(String const & message,MessageType messageType)
    {
        std::cerr << "[" << getRequesterName() << "] message(" << message << ", " << getMessageTypeName(messageType) << ")" << std::endl;
    }

    virtual void channelCreated(const epics::pvData::Status& status, Channel::shared_pointer const & channel)
    {
        if (status.isSuccess())
        {
            // show warning
            if (!status.isOK())
            {
                std::cerr << "[" << channel->getChannelName() << "] channel create: " << status.toString() << std::endl;
            }
        }
        else
        {
            std::cerr << "[" << channel->getChannelName() << "] failed to create a channel: " << status.toString() << std::endl;
        }
    }

    virtual void channelStateChange(Channel::shared_pointer const & /*channel*/, Channel::ConnectionState connectionState)
    {
        if (connectionState == Channel::CONNECTED)
        {
            m_event.signal();
        }
        /*
        else if (connectionState != Channel::DESTROYED)
        {
            std::cout << "[" << channel->getChannelName() << "] channel state change: "  << Channel::ConnectionStateNames[connectionState] << std::endl;
        }
        */
    }
    
    bool waitUntilConnected(double timeOut)
    {
        return m_event.wait(timeOut);
    }
};

/*+**************************************************************************
 *
 * Function:	main
 *
 * Description:	eget main()
 * 		Evaluate command line options, set up CA, connect the
 * 		channels, print the data as requested
 *
 * Arg(s) In:	[options] <pv-name>...
 *
 * Arg(s) Out:	none
 *
 * Return(s):	Standard return code (0=success, 1=error)
 *
 **************************************************************************-*/

int main (int argc, char *argv[])
{
    int opt;                    /* getopt() current option */
    bool debug = false;
    bool cleanupAndReport = false;

    Requester::shared_pointer requester(new RequesterImpl());
    
    bool serviceRequest = false;
    string service;
    string urlEncodedRequest;
    vector< pair<string,string> > parameters;

    setvbuf(stdout,NULL,_IOLBF,BUFSIZ);    /* Set stdout to line buffering */

    while ((opt = getopt(argc, argv, ":hr:s:p:w:tdc")) != -1) {
        switch (opt) {
        case 'h':               /* Print usage */
            usage();
            return 0;
        case 'w':               /* Set CA timeout value */
            if(epicsScanDouble(optarg, &timeOut) != 1)
            {
                fprintf(stderr, "'%s' is not a valid timeout value "
                        "- ignored. ('cainfo -h' for help.)\n", optarg);
                timeOut = DEFAULT_TIMEOUT;
            }
            break;
        case 'r':               /* Set pvRequest value */
            request = optarg;
            mode = StructureMode;
            break;          
        case 'p':               /* Service parameters */
        {   
            string param = optarg;
            size_t eqPos = param.find('=');
            if (eqPos==string::npos)
            {
                fprintf(stderr, "Parameter not specified in name=value form. ('eget -h' for help.)\n");
                return 1;
            }
            parameters.push_back(pair<string,string>(param.substr(0, eqPos), param.substr(eqPos+1, string::npos)));    
            if (urlEncodedRequest.size())
                urlEncodedRequest += '&';    
            char* encoded = url_encode(optarg);
            urlEncodedRequest += encoded;
            free(encoded);
            break;
        }
        case 's':               /* Service name */
            service = optarg;
            serviceRequest = true;
            break;
        case 't':               /* Terse mode */
            mode = TerseMode;
            break;
        case 'd':               /* Debug log level */
            debug = true;
            break;
        case 'c':               /* Clean-up and report used instance count */
            cleanupAndReport = true;
            break;
        case '?':
            fprintf(stderr,
                    "Unrecognized option: '-%c'. ('eget -h' for help.)\n",
                    optopt);
            return 1;
        case ':':
            fprintf(stderr,
                    "Option '-%c' requires an argument. ('eget -h' for help.)\n",
                    optopt);
            return 1;
        default :
            usage();
            return 1;
        }
    }

    int nPvs = argc - optind;       /* Remaining arg list are PV names */
    if (nPvs < 1 && !serviceRequest)
    {
        fprintf(stderr, "No PV name(s) specified. ('eget -h' for help.)\n");
        return 1;
    }
    
    if (nPvs > 0 && serviceRequest)
    {
        fprintf(stderr, "PV name(s) specified and service query requested. ('eget -h' for help.)\n");
        return 1;
    }

    SET_LOG_LEVEL(debug ? logLevelDebug : logLevelError);

    bool allOK = true;


    // PVs mode
    if (!serviceRequest)
    {
        vector<string> pvs;     /* Array of PV structures */
        for (int n = 0; optind < argc; n++, optind++)
            pvs.push_back(argv[optind]);       /* Copy PV names from command line */
        
        PVStructure::shared_pointer pvRequest;
        pvRequest = getCreateRequest()->createRequest(request,requester);
        if(pvRequest.get()==0) {
        	fprintf(stderr, "failed to parse request string\n");
            return 1;
        }
        
        ClientFactory::start();
        ChannelProvider::shared_pointer provider = getChannelAccess()->getProvider("pvAccess");
    
        // first connect to all, this allows resource (e.g. TCP connection) sharing
        vector<Channel::shared_pointer> channels(nPvs);
        for (int n = 0; n < nPvs; n++)
        {
            shared_ptr<ChannelRequesterImpl> channelRequesterImpl(new ChannelRequesterImpl()); 
            channels[n] = provider->createChannel(pvs[n], channelRequesterImpl);
        }
        
        // for now a simple iterating sync implementation, guarantees order
        for (int n = 0; n < nPvs; n++)
        {
            /*
            shared_ptr<ChannelRequesterImpl> channelRequesterImpl(new ChannelRequesterImpl()); 
            Channel::shared_pointer channel = provider->createChannel(pvs[n], channelRequesterImpl);
            */
            
            Channel::shared_pointer channel = channels[n];
            shared_ptr<ChannelRequesterImpl> channelRequesterImpl = dynamic_pointer_cast<ChannelRequesterImpl>(channel->getChannelRequester());
                
            if (channelRequesterImpl->waitUntilConnected(timeOut))
            {
                shared_ptr<ChannelGetRequesterImpl> getRequesterImpl(new ChannelGetRequesterImpl(channel->getChannelName()));
                ChannelGet::shared_pointer channelGet = channel->createChannelGet(getRequesterImpl, pvRequest);
                allOK &= getRequesterImpl->waitUntilGet(timeOut);
            }
            else
            {
                allOK = false;
                channel->destroy();
                std::cerr << "[" << channel->getChannelName() << "] connection timeout" << std::endl;
            }
        }    
    
        ClientFactory::stop();
    }
    // service RPC mode
    else
    {
    	/*
        std::cerr << "service            : " << service << std::endl;
        std::cerr << "parameters         : " << std::endl;

        vector< pair<string, string> >::iterator iter = parameters.begin();
        for (; iter != parameters.end(); iter++)
            std::cerr << "    " << iter->first << " = " << iter->second << std::endl;
        //std::cerr << "encoded URL request: '" << urlEncodedRequest << "'" << std::endl;
        */

        // TODO simply empty?
        PVStructure::shared_pointer pvRequest;
        pvRequest = getCreateRequest()->createRequest(request,requester);
        if(pvRequest.get()==NULL) {
        	fprintf(stderr, "failed to parse request string\n");
            return 1;
        }
        
        int i = 0;
        StringArray fieldNames(parameters.size());
        FieldConstPtrArray fields(parameters.size());
        for (vector< pair<string, string> >::iterator iter = parameters.begin();
             iter != parameters.end();
             iter++, i++)
        {
        	fieldNames[i] = iter->first;
            fields[i] = getFieldCreate()->createScalar(pvString);
        }
        PVStructure::shared_pointer args(
            new PVStructure(getFieldCreate()->createStructure(fieldNames, fields)));
        for (vector< pair<string, string> >::iterator iter = parameters.begin();
             iter != parameters.end();
             iter++)
        {
            args->getStringField(iter->first)->put(iter->second);
        }

        ClientFactory::start();
        ChannelProvider::shared_pointer provider = getChannelAccess()->getProvider("pvAccess");
        
        shared_ptr<ChannelRequesterImpl> channelRequesterImpl(new ChannelRequesterImpl()); 
        Channel::shared_pointer channel = provider->createChannel(service, channelRequesterImpl);
        
        if (channelRequesterImpl->waitUntilConnected(timeOut))
        {
            shared_ptr<ChannelRPCRequesterImpl> rpcRequesterImpl(new ChannelRPCRequesterImpl(channel->getChannelName()));
            ChannelRPC::shared_pointer channelRPC = channel->createChannelRPC(rpcRequesterImpl, pvRequest);

            if (rpcRequesterImpl->waitUntilConnected(timeOut))
            {
				channelRPC->request(args, true);
				allOK &= rpcRequesterImpl->waitUntilRPC(timeOut);
			}
            else
            {
                allOK = false;
                channel->destroy();
                std::cerr << "[" << channel->getChannelName() << "] RPC create timeout" << std::endl;
            }
        }
        else
        {
            allOK = false;
            channel->destroy();
            std::cerr << "[" << channel->getChannelName() << "] connection timeout" << std::endl;
        }
    
        ClientFactory::stop();
    }

    if (cleanupAndReport)
    {
        // TODO implement wait on context
        epicsThreadSleep ( 3.0 );
        //std::cerr << "-----------------------------------------------------------------------" << std::endl;
        //epicsExitCallAtExits();
    }

    return allOK ? 0 : 1;
}
