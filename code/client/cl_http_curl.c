/*
===========================================================================
Copyright (C) 2006 Tony J. White (tjw@tjw.org)

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifdef USE_HTTP

#include "client.h"

#ifdef USE_INTERNAL_CURL_HEADERS
  #include "curl/curl.h"
#else
  #include <curl/curl.h>
#endif

#ifdef __APPLE__
  #define DEFAULT_CURL_LIB "libcurl.dylib"
#else
  #define DEFAULT_CURL_LIB "libcurl.so.4"
  #define ALTERNATE_CURL_LIB "libcurl.so.3"
#endif

#include "../sys/sys_loadlib.h"

cvar_t *cl_cURLLib;

char* (*qcurl_version)(void);

CURL* (*qcurl_easy_init)(void);
CURLcode (*qcurl_easy_setopt)(CURL *curl, CURLoption option, ...);
CURLcode (*qcurl_easy_perform)(CURL *curl);
void (*qcurl_easy_cleanup)(CURL *curl);
CURLcode (*qcurl_easy_getinfo)(CURL *curl, CURLINFO info, ...);
CURL* (*qcurl_easy_duphandle)(CURL *curl);
void (*qcurl_easy_reset)(CURL *curl);
const char *(*qcurl_easy_strerror)(CURLcode);

CURLM* (*qcurl_multi_init)(void);
CURLMcode (*qcurl_multi_add_handle)(CURLM *multi_handle,
                                                CURL *curl_handle);
CURLMcode (*qcurl_multi_remove_handle)(CURLM *multi_handle,
                                                CURL *curl_handle);
CURLMcode (*qcurl_multi_fdset)(CURLM *multi_handle,
                                                fd_set *read_fd_set,
                                                fd_set *write_fd_set,
                                                fd_set *exc_fd_set,
                                                int *max_fd);
CURLMcode (*qcurl_multi_perform)(CURLM *multi_handle,
                                                int *running_handles);
CURLMcode (*qcurl_multi_cleanup)(CURLM *multi_handle);
CURLMsg *(*qcurl_multi_info_read)(CURLM *multi_handle,
                                                int *msgs_in_queue);
const char *(*qcurl_multi_strerror)(CURLMcode);

static void *cURLLib = NULL;
static qboolean cURLSymbolLoadFailed = qfalse;

static CURL *downloadCURL = NULL;
static CURLM *downloadCURLM = NULL;

/*
=================
GPA
=================
*/
static void *GPA(char *str)
{
	void *rv;

	rv = Sys_LoadFunction(cURLLib, str);
	if(!rv)
	{
		Com_Printf("Can't load symbol %s\n", str);
		cURLSymbolLoadFailed = qtrue;
		return NULL;
	}
	else
	{
		Com_DPrintf("Loaded symbol %s (0x%p)\n", str, rv);
        return rv;
	}
}

/*
=================
CL_HTTP_Init
=================
*/
qboolean CL_HTTP_Init()
{
	if(cURLLib)
		return qtrue;

	cl_cURLLib = Cvar_Get("cl_cURLLib", DEFAULT_CURL_LIB, CVAR_ARCHIVE | CVAR_PROTECTED);

	Com_Printf("Loading \"%s\"...", cl_cURLLib->string);
	if(!(cURLLib = Sys_LoadDll(cl_cURLLib->string, qtrue)))
	{
#ifdef ALTERNATE_CURL_LIB
		// On some linux distributions there is no libcurl.so.3, but only libcurl.so.4. That one works too.
		if(!(cURLLib = Sys_LoadDll(ALTERNATE_CURL_LIB, qtrue)))
#endif
			return qfalse;
	}

	cURLSymbolLoadFailed = qfalse;

	qcurl_version = GPA("curl_version");

	qcurl_easy_init = GPA("curl_easy_init");
	qcurl_easy_setopt = GPA("curl_easy_setopt");
	qcurl_easy_perform = GPA("curl_easy_perform");
	qcurl_easy_cleanup = GPA("curl_easy_cleanup");
	qcurl_easy_getinfo = GPA("curl_easy_getinfo");
	qcurl_easy_duphandle = GPA("curl_easy_duphandle");
	qcurl_easy_reset = GPA("curl_easy_reset");
	qcurl_easy_strerror = GPA("curl_easy_strerror");
	
	qcurl_multi_init = GPA("curl_multi_init");
	qcurl_multi_add_handle = GPA("curl_multi_add_handle");
	qcurl_multi_remove_handle = GPA("curl_multi_remove_handle");
	qcurl_multi_fdset = GPA("curl_multi_fdset");
	qcurl_multi_perform = GPA("curl_multi_perform");
	qcurl_multi_cleanup = GPA("curl_multi_cleanup");
	qcurl_multi_info_read = GPA("curl_multi_info_read");
	qcurl_multi_strerror = GPA("curl_multi_strerror");

	if(cURLSymbolLoadFailed)
	{
		CL_HTTP_Shutdown();
		Com_Printf("FAIL One or more symbols not found\n");
		return qfalse;
	}
	Com_Printf("OK\n");

	return qtrue;
}

/*
=================
CL_HTTP_Available
=================
*/
qboolean CL_HTTP_Available()
{
	return cURLLib != NULL;
}

static void CL_cURL_Cleanup(void)
{
	if(downloadCURLM) {
		CURLMcode result;

		if(downloadCURL) {
			result = qcurl_multi_remove_handle(downloadCURLM,
				downloadCURL);
			if(result != CURLM_OK) {
				Com_DPrintf("qcurl_multi_remove_handle failed: %s\n", qcurl_multi_strerror(result));
			}
			qcurl_easy_cleanup(downloadCURL);
		}
		result = qcurl_multi_cleanup(downloadCURLM);
		if(result != CURLM_OK) {
			Com_DPrintf("CL_cURL_Cleanup: qcurl_multi_cleanup failed: %s\n", qcurl_multi_strerror(result));
		}
		downloadCURLM = NULL;
		downloadCURL = NULL;
	}
	else if(downloadCURL) {
		qcurl_easy_cleanup(downloadCURL);
		downloadCURL = NULL;
	}
}

/*
=================
CL_HTTP_Shutdown
=================
*/
void CL_HTTP_Shutdown( void )
{
	CL_cURL_Cleanup();

	if(cURLLib)
	{
		Sys_UnloadLibrary(cURLLib);
		cURLLib = NULL;
	}
	qcurl_easy_init = NULL;
	qcurl_easy_setopt = NULL;
	qcurl_easy_perform = NULL;
	qcurl_easy_cleanup = NULL;
	qcurl_easy_getinfo = NULL;
	qcurl_easy_duphandle = NULL;
	qcurl_easy_reset = NULL;

	qcurl_multi_init = NULL;
	qcurl_multi_add_handle = NULL;
	qcurl_multi_remove_handle = NULL;
	qcurl_multi_fdset = NULL;
	qcurl_multi_perform = NULL;
	qcurl_multi_cleanup = NULL;
	qcurl_multi_info_read = NULL;
	qcurl_multi_strerror = NULL;
}

static int CL_cURL_CallbackProgress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
    curl_off_t ultotal, curl_off_t ulnow)
{
	clc.downloadSize = (int)dltotal;
	Cvar_SetValue( "cl_downloadSize", clc.downloadSize );
	clc.downloadCount = (int)dlnow;
	Cvar_SetValue( "cl_downloadCount", clc.downloadCount );
	return 0;
}

static size_t CL_cURL_CallbackWrite(void *buffer, size_t size, size_t nmemb,
	void *stream)
{
	FS_Write( buffer, size*nmemb, ((fileHandle_t*)stream)[0] );
	return size*nmemb;
}

CURLcode qcurl_easy_setopt_warn(CURL *curl, CURLoption option, ...)
{
	CURLcode result;

	va_list argp;
	va_start(argp, option);

	if(option < CURLOPTTYPE_OBJECTPOINT) {
		long longValue = va_arg(argp, long);
		result = qcurl_easy_setopt(curl, option, longValue);
	} else if(option < CURLOPTTYPE_OFF_T) {
		void *pointerValue = va_arg(argp, void *);
		result = qcurl_easy_setopt(curl, option, pointerValue);
	} else {
		curl_off_t offsetValue = va_arg(argp, curl_off_t);
		result = qcurl_easy_setopt(curl, option, offsetValue);
	}

	if(result != CURLE_OK) {
		Com_DPrintf("qcurl_easy_setopt failed: %s\n", qcurl_easy_strerror(result));
	}
	va_end(argp);

	return result;
}

void CL_HTTP_BeginDownload( const char *remoteURL )
{
	CURLMcode result;

	CL_cURL_Cleanup();

	downloadCURL = qcurl_easy_init();
	if(!downloadCURL) {
		Com_Error(ERR_DROP, "CL_HTTP_BeginDownload: qcurl_easy_init() "
			"failed");
		return;
	}

	if(com_developer->integer)
		qcurl_easy_setopt_warn(downloadCURL, CURLOPT_VERBOSE, 1);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_URL, remoteURL);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_TRANSFERTEXT, 0);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_REFERER, va("ioQ3://%s",
		NET_AdrToString(clc.serverAddress)));
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_USERAGENT, va("%s %s",
		Q3_VERSION, qcurl_version()));
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_WRITEFUNCTION,
		CL_cURL_CallbackWrite);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_WRITEDATA, &clc.download);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_NOPROGRESS, 0);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_XFERINFOFUNCTION,
		CL_cURL_CallbackProgress);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_PROGRESSDATA, NULL);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_FAILONERROR, 1);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_FOLLOWLOCATION, 1);
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_MAXREDIRS, 5);
#if CURL_AT_LEAST_VERSION(7,85,0)
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_PROTOCOLS_STR, "http,https");
#else
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
	qcurl_easy_setopt_warn(downloadCURL, CURLOPT_BUFFERSIZE, CURL_MAX_READ_SIZE);
	downloadCURLM = qcurl_multi_init();
	if(!downloadCURLM) {
		qcurl_easy_cleanup(downloadCURL);
		downloadCURL = NULL;
		Com_Error(ERR_DROP, "CL_HTTP_BeginDownload: qcurl_multi_init() "
			"failed");
		return;
	}
	result = qcurl_multi_add_handle(downloadCURLM, downloadCURL);
	if(result != CURLM_OK) {
		qcurl_easy_cleanup(downloadCURL);
		downloadCURL = NULL;
		Com_Error(ERR_DROP,"CL_HTTP_BeginDownload: qcurl_multi_add_handle() failed: %s", qcurl_multi_strerror(result));
		return;
	}
}

qboolean CL_HTTP_PerformDownload(void)
{
	CURLMcode res;
	CURLMsg *msg;
	int c;
	int i = 0;

	res = qcurl_multi_perform(downloadCURLM, &c);
	while(res == CURLM_CALL_MULTI_PERFORM && i < 100) {
		res = qcurl_multi_perform(downloadCURLM, &c);
		i++;
	}
	if(res == CURLM_CALL_MULTI_PERFORM)
		return qfalse;
	msg = qcurl_multi_info_read(downloadCURLM, &c);
	if(msg == NULL) {
		return qfalse;
	}
	if(msg->msg != CURLMSG_DONE || msg->data.result != CURLE_OK) {
		long code;

		qcurl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE,
			&code);	
		Com_Error(ERR_DROP, "Download Error: %s Code: %ld URL: %s",
			qcurl_easy_strerror(msg->data.result),
			code, clc.downloadURL);
	}

	return qtrue;
}

/* -----------------------------------------------------------------------
 * Demo streaming via HTTP/HTTPS
 * The received bytes are written directly to a caller-supplied FILE*.
 * The caller opens that same file with FS_FOpenSysFileRead and plays it
 * with di.streaming = qtrue so playback starts as soon as data arrives.
 * ----------------------------------------------------------------------- */

static CURL  *demoCURL  = NULL;
static CURLM *demoCURLM = NULL;
static FILE  *demoWriteFile = NULL;

static size_t CL_cURL_DemoStreamWrite(void *buffer, size_t size, size_t nmemb,
	void *userdata)
{
	FILE *f = (FILE *)userdata;
	size_t written = fwrite(buffer, size, nmemb, f);
	fflush(f);
	return written;
}

qboolean CL_HTTP_BeginDemoStream(const char *url, FILE *outFile)
{
	CURLMcode result;

	/* clean up any previous demo stream */
	CL_HTTP_AbortDemoStream();

	if (!outFile) {
		Com_Printf("^1CL_HTTP_BeginDemoStream: NULL output file\n");
		return qfalse;
	}

	demoCURL = qcurl_easy_init();
	if (!demoCURL) {
		Com_Printf("^1CL_HTTP_BeginDemoStream: qcurl_easy_init() failed\n");
		return qfalse;
	}

	demoWriteFile = outFile;

	if (com_developer->integer)
		qcurl_easy_setopt(demoCURL, CURLOPT_VERBOSE, 1L);
	qcurl_easy_setopt(demoCURL, CURLOPT_URL,              url);
	qcurl_easy_setopt(demoCURL, CURLOPT_TRANSFERTEXT,     0L);
	qcurl_easy_setopt(demoCURL, CURLOPT_USERAGENT,        va("%s %s", Q3_VERSION, qcurl_version()));
	qcurl_easy_setopt(demoCURL, CURLOPT_WRITEFUNCTION,    CL_cURL_DemoStreamWrite);
	qcurl_easy_setopt(demoCURL, CURLOPT_WRITEDATA,        demoWriteFile);
	qcurl_easy_setopt(demoCURL, CURLOPT_NOPROGRESS,       1L);
	qcurl_easy_setopt(demoCURL, CURLOPT_FAILONERROR,      1L);
	/* Follow redirects so URL shorteners (is.gd, bit.ly, etc.) yield the actual file. */
	qcurl_easy_setopt(demoCURL, CURLOPT_FOLLOWLOCATION,   1L);
	qcurl_easy_setopt(demoCURL, CURLOPT_MAXREDIRS,        10L);
#if CURL_AT_LEAST_VERSION(7,85,0)
	qcurl_easy_setopt(demoCURL, CURLOPT_PROTOCOLS_STR,    "http,https");
#else
	qcurl_easy_setopt(demoCURL, CURLOPT_PROTOCOLS,
		(long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	qcurl_easy_setopt(demoCURL, CURLOPT_BUFFERSIZE, (long)CURL_MAX_READ_SIZE);

	demoCURLM = qcurl_multi_init();
	if (!demoCURLM) {
		qcurl_easy_cleanup(demoCURL);
		demoCURL = NULL;
		Com_Printf("^1CL_HTTP_BeginDemoStream: qcurl_multi_init() failed\n");
		return qfalse;
	}

	result = qcurl_multi_add_handle(demoCURLM, demoCURL);
	if (result != CURLM_OK) {
		qcurl_easy_cleanup(demoCURL);
		demoCURL = NULL;
		qcurl_multi_cleanup(demoCURLM);
		demoCURLM = NULL;
		Com_Printf("^1CL_HTTP_BeginDemoStream: qcurl_multi_add_handle() failed: %s\n",
			qcurl_multi_strerror(result));
		return qfalse;
	}

	return qtrue;
}

qboolean CL_HTTP_PollDemoStream(void)
{
	CURLMcode res;
	CURLMsg   *msg;
	int        running, c;

	if (!demoCURLM)
		return qtrue; /* nothing active, treat as done */

	running = 0;
	res = qcurl_multi_perform(demoCURLM, &running);
	if (res == CURLM_CALL_MULTI_PERFORM) {
		/* need to call again immediately; do so a few more times */
		int i;
		for (i = 0; i < 8 && res == CURLM_CALL_MULTI_PERFORM; i++)
			res = qcurl_multi_perform(demoCURLM, &running);
	}

	msg = qcurl_multi_info_read(demoCURLM, &c);
	if (msg == NULL)
		return qfalse; /* still in progress */

	if (msg->msg == CURLMSG_DONE) {
		if (msg->data.result != CURLE_OK) {
			long code = 0;
			qcurl_easy_getinfo(demoCURL, CURLINFO_RESPONSE_CODE, &code);
			Com_Printf("^1HTTP demo stream error: %s (HTTP %ld)\n",
				qcurl_easy_strerror(msg->data.result), code);
		}
	}

	/* transfer finished (success or error) */
	if (demoWriteFile) {
		fflush(demoWriteFile);
	}
	qcurl_multi_remove_handle(demoCURLM, demoCURL);
	qcurl_easy_cleanup(demoCURL);
	demoCURL = NULL;
	qcurl_multi_cleanup(demoCURLM);
	demoCURLM = NULL;
	demoWriteFile = NULL;
	return qtrue;
}

void CL_HTTP_AbortDemoStream(void)
{
	if (demoCURLM) {
		if (demoCURL) {
			qcurl_multi_remove_handle(demoCURLM, demoCURL);
			qcurl_easy_cleanup(demoCURL);
			demoCURL = NULL;
		}
		qcurl_multi_cleanup(demoCURLM);
		demoCURLM = NULL;
	} else if (demoCURL) {
		qcurl_easy_cleanup(demoCURL);
		demoCURL = NULL;
	}
	if (demoWriteFile) {
		fflush(demoWriteFile);
		demoWriteFile = NULL;
	}
}

#endif /* USE_HTTP */
