/* vim: set textwidth=80 tabstop=4: */
/* Copyright (c) 2010 Timothy Pearson (kb9vqf@pearsoncomputing.net)
 * Copyright (c) 2008 Michael Rasmussen (mir@datanom.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "lock-carddav-object.h"
#include "options-carddav-server.h"
#include <glib.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * A static literal string containing the lock query.
 */
static char* lock_query =
"<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
"<D:lockinfo xmlns:D=\"DAV:\">"
"  <D:lockscope><D:exclusive/></D:lockscope>"
"  <D:locktype><D:write/></D:locktype>"
"</D:lockinfo>";

/**
 * Function which requests a lock on a calendar resource
 * @param URI The resource to request lock on.
 * @param settings @see carddav_settings
 * @param error A pointer to carddav_error. @see carddav_error
 * @return The Lock-Token or NULL in case of error
 */
gchar* carddav_lock_object(
		gchar* URI, carddav_settings* settings, carddav_error* error) {
	CURL* curl;
	CURLcode res = 0;
	char error_buf[CURL_ERROR_SIZE];
	struct config_data data;
	struct MemoryStruct chunk;
	struct MemoryStruct headers;
	struct curl_slist *http_header = NULL;
	gchar* lock_token = NULL;
	gchar* url;

	if (! carddav_lock_support(settings, error))
		return lock_token;
	chunk.memory = NULL; /* we expect realloc(NULL, size) to work */
	chunk.size = 0;    /* no data at this point */
	headers.memory = NULL;
	headers.size = 0;

	curl = get_curl(settings);
	if (!curl) {
		error->code = -1;
		error->str = g_strdup("Could not initialize libcurl");
		g_free(settings->file);
		settings->file = NULL;
		return lock_token;
	}

	http_header = curl_slist_append(http_header,
			"Content-Type: application/xml; charset=\"utf-8\"");
	http_header = curl_slist_append(http_header, "Timeout: Second-300");
	http_header = curl_slist_append(http_header, "Expect:");
	http_header = curl_slist_append(http_header, "Transfer-Encoding:");
	http_header = curl_slist_append(http_header, "Connection: close");
	data.trace_ascii = settings->trace_ascii;
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http_header);
	/* send all data to this function  */
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	/* we pass our 'chunk' struct to the callback function */
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	/* send all data to this function  */
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,	WriteHeaderCallback);
	/* we pass our 'headers' struct to the callback function */
	curl_easy_setopt(curl, CURLOPT_WRITEHEADER, (void *)&headers);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, (char *) &error_buf);
	if (settings->debug) {
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, my_trace);
		curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &data);
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	}
	if (settings->usehttps) {
		url = g_strdup_printf("https://%s", URI);
	} else {
		url = g_strdup_printf("http://%s", URI);
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	g_free(url);
	/* enable uploading */
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, lock_query);
	curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE, strlen(lock_query));
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "LOCK");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 1);
	curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
	res = curl_easy_perform(curl);
	curl_slist_free_all(http_header);
	if (res != 0) {
		error->code = -1;
		error->str = g_strdup_printf("%s", error_buf);
		g_free(settings->file);
		settings->file = NULL;
	}
	else {
		long code;
		res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		if (code != 200) {
			gchar* status = get_tag("status", chunk.memory);
			if (status && strstr(status, "423") != NULL) {
				error->code = 423;
				error->str = g_strdup(status);
			}
			else {
				error->code = code;
				error->str = g_strdup(chunk.memory);
			}
			g_free(status);
		}
		else {
			lock_token = get_response_header(
						"Lock-Token", headers.memory, FALSE);
		}
	}
	if (chunk.memory)
		free(chunk.memory);
	if (headers.memory)
		free(headers.memory);
	curl_easy_cleanup(curl);
	return lock_token;
}

/**
 * Function which requests to have a lock removed from a calendar resource
 * @param lock_token A previous acquired Lock-Token
 * @param URI The resource to request unlock on.
 * @param settings @see carddav_settings
 * @param error A pointer to carddav_error. @see carddav_error
 * @return False in case the lock could not be removed. True otherwise.
 */
gboolean carddav_unlock_object(gchar* lock_token, gchar* URI, 
			carddav_settings* settings, carddav_error* error) {
	CURL* curl;
	CURLcode res = 0;
	char error_buf[CURL_ERROR_SIZE];
	struct config_data data;
	struct MemoryStruct chunk;
	struct MemoryStruct headers;
	struct curl_slist *http_header = NULL;
	gboolean result = FALSE;
	gchar* url;

	if (! carddav_lock_support(settings, error))
		return result;
	chunk.memory = NULL; /* we expect realloc(NULL, size) to work */
	chunk.size = 0;    /* no data at this point */
	headers.memory = NULL;
	headers.size = 0;

	curl = get_curl(settings);
	if (!curl) {
		error->code = -1;
		error->str = g_strdup("Could not initialize libcurl");
		g_free(settings->file);
		settings->file = NULL;
		return TRUE;
	}

	http_header = curl_slist_append(http_header, 
			g_strdup_printf("Lock-Token: %s", lock_token));
	http_header = curl_slist_append(http_header, "Expect:");
	http_header = curl_slist_append(http_header, "Transfer-Encoding:");
	http_header = curl_slist_append(http_header, "Connection: close");
	data.trace_ascii = settings->trace_ascii;
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http_header);
	/* send all data to this function  */
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	/* we pass our 'chunk' struct to the callback function */
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	/* send all data to this function  */
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,	WriteHeaderCallback);
	/* we pass our 'headers' struct to the callback function */
	curl_easy_setopt(curl, CURLOPT_WRITEHEADER, (void *)&headers);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, (char *) &error_buf);
	if (settings->debug) {
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, my_trace);
		curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &data);
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	}
	if (settings->usehttps) {
		url = g_strdup_printf("https://%s", URI);
	} else {
		url = g_strdup_printf("http://%s", URI);
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	g_free(url);
	/* enable uploading */
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "UNLOCK");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 1);
	curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
	res = curl_easy_perform(curl);
	curl_slist_free_all(http_header);
	if (res != 0) {
		error->code = -1;
		error->str = g_strdup_printf("%s", error_buf);
		g_free(settings->file);
		settings->file = NULL;
	}
	else {
		long code;
		res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		if (code != 204) {
			error->code = code;
			error->str = g_strdup(chunk.memory);
		}
		else {
			result = TRUE;
		}
	}
	if (chunk.memory)
		free(chunk.memory);
	if (headers.memory)
		free(headers.memory);
	curl_easy_cleanup(curl);
	return result;
}

/**
 * Function to test whether the server supports locking or not. Searching
 * for PROP LOCK. If LOCK is present then according to RFC4791 PROP UNLOCK
 * must also be present.
 * @param settings @see carddav_settings
 * @param error A pointer to carddav_error. @see carddav_error
 * @return True if locking is supported by the server. False otherwise
 */
gboolean carddav_lock_support(carddav_settings* settings, carddav_error* error) {
	gboolean found = FALSE;
	gchar* url = NULL;
	gchar* mystr = NULL;
	runtime_info* info;
	
	info = carddav_get_runtime_info();
	info->options->verify_ssl_certificate = settings->verify_ssl_certificate;
	info->options->custom_cacert = g_strdup(settings->custom_cacert);
	if (settings->usehttps) {
		mystr =  g_strdup("https://");
	} else {
		mystr =  g_strdup("http://");
	}
	

	if (settings->username && settings->password) {
		url = g_strdup_printf("%s%s:%s@%s",
				mystr, settings->username, settings->password, settings->url);
	}
	else if (settings->username) {
		url = g_strdup_printf("%s%s@%s", 
				mystr, settings->username, settings->url);
	}
	else {
		url = g_strdup_printf("%s%s", mystr, settings->url);
	}
	gchar** options = carddav_get_server_options(url, info);
	g_free(url);
	gchar** tmp = options;
	carddav_free_runtime_info(&info);
	while (*options) {
		if (strcmp(*options++, "LOCK") == 0) {
			found = TRUE;
			break;
		}
	}
	g_strfreev(tmp);			
	g_free(mystr);
	return found;
}


