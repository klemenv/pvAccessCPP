/*******************************************************************
 * This implementation was adpoted from:
 *
 * Copyright (C) 1996, 1997, 1998, 1999, 2000 Florian Schintke
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA02111-1307  USA
 *
 * F.Schintke, the author of the original code, has authorized to
 * distribute these files under LGPL License.
 *
 * ----------------------
 * Implementation of the UN*X wildcards
 * Supported wild-characters: '*', '?'; sets: [a-z], '!' negation
 * Examples:
 *	 '[a-g]l*i?n' matches 'florian'
 *	 '[!abc]*e' matches 'smile'
 *	 '[-z] matches 'a'
 *
 */

#ifndef WILDCARD_H
#define WILDCARD_H

#include <shareLib.h>

namespace epics { namespace pvAccess {

/**
 * Class which implements UNIX style wildcards and tests to see
 * if strings match the wildcard.
 */
class epicsShareClass Wildcard
{
    
  public:
    
    /** 
     * This function implements the UN*X wildcards.
     * @param wildcard Wildcard to be used.
     * @param test Value which we want to see if it matches the wildcard.
     * @return 0 if wildcard does not match *test. 1 - if wildcard 
     * matches test.
     */
    static int wildcardfit (const char *wildcard, const char *test);
    
  private:
    /**
     * Scans a set of characters and returns 0 if the set mismatches at this
     * position in the teststring and 1 if it is matching
     * wildcard is set to the closing ] and test is unmodified if mismatched
     * and otherwise the char pointer is pointing to the next character
     * @param wildcard UNIX style wildcard to be used
     * @param test String we will test against the wildcard.
     * @return 0 if the set mismatches. 1 otherwise.
     */
    static int set (const char **wildcard, const char **test);
    
    /**
     * Scans an asterisk.
     * @param wildcard UNIX style wildcard to be used
     * @param test String we will test against the wildcard.
     * @return ???
     */
    static int asterisk (const char **wildcard, const char **test);
};

} }


#endif

