/*
 * inetAddressUtil.h
 *
 *  Created on: Nov 12, 2010
 *      Author: Miha Vitorovic
 */

#ifndef INETADDRESSUTIL_H_
#define INETADDRESSUTIL_H_

/* uporabim lahko:
 * EPICSv3 osiSock.h  kjer je definiran osiSockDiscoverBroadcastAddresses
 *
 * Kako se ga uporablja je v
 * epics/base/src/ca/iocinf.cpp   funkcija configureChannelAccessAddressList
 *
 * razišči kako se to priredi za IPv6
 *
 */

#include <pvType.h>
#include <byteBuffer.h>
#include <osiSock.h>
#include <vector>

using namespace epics::pvData;

namespace epics {
    namespace pvAccess {

        typedef std::vector<osiSockAddr*> InetAddrVector;

        /**
         * returns a vector containing all the IPv4 broadcast addresses
         * on this machine. IPv6 doesn't have a local broadcast address.
         *
         * TODO Check implementation/rewrite this
         */
        InetAddrVector* getBroadcastAddresses(SOCKET sock);

        /**
         * Encode IPv4 address as IPv6 address.
         * @param buffer byte-buffer where to put encoded data.
         * @param address address to encode.
         */
        void
        encodeAsIPv6Address(ByteBuffer* buffer, const osiSockAddr* address);

        /**
         * Convert an integer into an IPv4 INET address.
         * @param addr integer representation of a given address.
         * @return IPv4 INET address.
         */
        osiSockAddr* intToIPv4Address(int32 addr);

        /**
         * Convert an IPv4 INET address to an integer.
         * @param addr  IPv4 INET address.
         * @return integer representation of a given address.
         */
        int32 ipv4AddressToInt(const osiSockAddr& addr);

        /**
         * Parse space delimited addresss[:port] string and return array of <code>InetSocketAddress</code>.
         * @param list  space delimited addresss[:port] string.
         * @param defaultPort   port take if not specified.
         * @param appendList    list to be appended.
         * @return  array of <code>InetSocketAddress</code>.
         */
        InetAddrVector* getSocketAddressList(String list, int defaultPort,
                const InetAddrVector* appendList = NULL);

        const String inetAddressToString(const osiSockAddr *addr,
                bool displayHex = false);

    }
}

#endif /* INETADDRESSUTIL_H_ */