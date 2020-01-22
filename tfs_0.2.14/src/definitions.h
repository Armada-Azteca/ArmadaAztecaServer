//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
// various definitions needed by most files
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////

#ifndef __OTSERV_DEFINITIONS_H__
#define __OTSERV_DEFINITIONS_H__

#define CLIENT_VERSION_MIN 960
#define CLIENT_VERSION_MAX 961
#define CLIENT_VERSION_STR "9.61"

#ifdef _WIN32
#ifndef WIN32
#define WIN32
#endif
#endif

#undef MULTI_SQL_DRIVERS
#define SQL_DRIVERS __USE_SQLITE__+__USE_MYSQL__

#if SQL_DRIVERS > 1
#define MULTI_SQL_DRIVERS
#endif

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <direct.h>
#else /* Not _WIN32 */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif /* _WIN32 */

#ifdef _WIN32
#ifdef _MSC_VER
#define mkdir(dirname, mode) _mkdir(dirname)
#else /* Not _MSC_VER */
#define mkdir(dirname, mode) _mkdir(dirname)
#endif /* _MSC_VER */
#endif /* _WIN32 */

#ifndef __USE_TEMPLATES__
#define toString(str) std::string(#str)
#endif

#ifndef WIN32
#ifndef _CONSOLE
#define _CONSOLE
#endif
#endif

#ifdef XML_GCC_FREE
	#define xmlFreeOTSERV(s)	free(s)
#else
	#define xmlFreeOTSERV(s)	xmlFree(s)
#endif

#ifdef __DEBUG_EXCEPTION_REPORT__
	#define DEBUG_REPORT int *a = NULL; *a = 1;
#else
	#ifdef __EXCEPTION_TRACER__
		#define DEBUG_REPORT ExceptionHandler::dumpStack();
	#else
		#define DEBUG_REPORT
	#endif
#endif

enum passwordType_t
{
	PASSWORD_TYPE_PLAIN = 0,
	PASSWORD_TYPE_MD5,
	PASSWORD_TYPE_SHA1
};

#if defined __WINDOWS__ || defined WIN32

#if defined _MSC_VER && defined NDEBUG
	#define _SECURE_SCL 0
	#define HAS_ITERATOR_DEBUGGING 0
#endif

#ifndef __FUNCTION__
	#define	__FUNCTION__ __func__
#endif

#define OTSYS_THREAD_RETURN void

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif

//Windows 2000	0x0500
//Windows XP	0x0501
//Windows 2003	0x0502
//Windows Vista	0x0600
#define _WIN32_WINNT 0x0501

#ifdef __GNUC__
	#include <stdint.h>
	#include <string.h>
	#if __GNUC__ >= 4
		#include <tr1/unordered_map>
		#include <tr1/unordered_set>
		#define OTSERV_HASH_MAP std::tr1::unordered_map
		#define OTSERV_HASH_SET std::tr1::unordered_set
	#else
		#include <ext/hash_map>
		#include <ext/hash_set>
		#define OTSERV_HASH_MAP __gnu_cxx::hash_map
		#define OTSERV_HASH_SET __gnu_cxx::hash_set
	#endif
	#include <assert.h>
	#include <time.h>

	#define ATOI64 atoll
#else
	typedef unsigned long long uint64_t;

	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#include <hash_map>
	#include <hash_set>
	#include <limits>
	#include <assert.h>
	#define OTSERV_HASH_MAP stdext::hash_map
	#define OTSERV_HASH_SET stdext::hash_set

	#include <cstring>
	inline int strcasecmp(const char *s1, const char *s2)
	{
		return ::_stricmp(s1, s2);
	}

	inline int strncasecmp(const char *s1, const char *s2, size_t n)
	{
		return ::_strnicmp(s1, s2, n);
	}

	//#if VISUALC_VERSION >= 10
	#if defined(_MSC_VER) && _MSC_VER >= 10
		#include <stdint.h>
	#else
		typedef signed long long int64_t;
		// Int is 4 bytes on all x86 and x86-64 platforms
		typedef unsigned int uint32_t;
		typedef signed int int32_t;
		typedef unsigned short uint16_t;
		typedef signed short int16_t;
		typedef unsigned char uint8_t;
		typedef signed char int8_t;
	#endif
	#define ATOI64 _atoi64

	#pragma warning(disable:4786) // msvc too long debug names in stl
	#pragma warning(disable:4250) // 'class1' : inherits 'class2::member' via dominance
	#pragma warning(disable:4244) //'argument' : conversion from 'type1' to 'type2', possible loss of data
	#pragma warning(disable:4267) //'var' : conversion from 'size_t' to 'type', possible loss of data
#endif

//*nix systems
#else
	#define OTSYS_THREAD_RETURN void*

	#include <stdint.h>
	#include <string.h>
	#if __GNUC__ >= 4
		#include <tr1/unordered_map>
		#include <tr1/unordered_set>
		#define OTSERV_HASH_MAP std::tr1::unordered_map
		#define OTSERV_HASH_SET std::tr1::unordered_set
	#else
		#include <ext/hash_map>
		#include <ext/hash_set>
		#define OTSERV_HASH_MAP __gnu_cxx::hash_map
		#define OTSERV_HASH_SET __gnu_cxx::hash_set
	#endif
	#include <assert.h>
	#include <time.h>

	#define ATOI64 atoll

#endif

#endif
