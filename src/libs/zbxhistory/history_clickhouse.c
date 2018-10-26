/*
** Zabbix
** Copyright (C) 2001-2018 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/



#include "common.h"
#include "log.h"
#include "zbxjson.h"
#include "zbxalgo.h"
#include "dbcache.h"
#include "zbxhistory.h"
#include "zbxself.h"
#include "history.h"
#include <stdio.h>
#include <string.h>

/* curl_multi_wait() is supported starting with version 7.28.0 (0x071c00) */
#if defined(HAVE_LIBCURL) && LIBCURL_VERSION_NUM >= 0x071c00

#define		ZBX_HISTORY_STORAGE_DOWN	10000 /* Timeout in milliseconds */
#define		MAX_HISTORY_CLICKHOUSE_FIELDS	5 /* How many fields to parse from clickhouse output */

//const char	*value_type_str[] = {"dbl", "str", "log", "uint", "text"};

extern char	*CONFIG_HISTORY_STORAGE_URL;
extern char *CONFIG_HISTORY_STORAGE_TABLE_NAME;

typedef struct
{
	char	*base_url;

//post_url  here from elastics where it was used for scrolling of data search results, 
//post_url might be used to use some clickhouse options, so i've decided to leave it
	//char	*post_url;
	char	*buf;
	CURL	*handle;
}
zbx_clickhouse_data_t;

typedef struct
{
	unsigned char		initialized;
	zbx_vector_ptr_t	ifaces;

	CURLM			*handle;
}
zbx_clickhouse_writer_t;

static zbx_clickhouse_writer_t	writer;

typedef struct
{
	char	*data;
	size_t	alloc;
	size_t	offset;
}
zbx_httppage_t;

static zbx_httppage_t	page;

static size_t	curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t	r_size = size * nmemb;

	ZBX_UNUSED(userdata);

	zbx_strncpy_alloc(&page.data, &page.alloc, &page.offset, ptr, r_size);

	return r_size;
}

/************************************************************************************
 *                                                                                  *
 * Comments: stub function for avoiding LibCURL to print on the standard output.    *
 *           In case of success, elasticsearch return a JSON, but the HTTP error    *
 *           code is enough                                                         *
 *                                                                                  *
 ************************************************************************************/
static size_t	curl_write_send_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	ZBX_UNUSED(ptr);
	ZBX_UNUSED(userdata);
	return size * nmemb;
}

static history_value_t	history_str2value(char *str, unsigned char value_type)
{
	history_value_t	value;

	switch (value_type)
	{
		case ITEM_VALUE_TYPE_LOG:
			value.log = zbx_malloc(NULL, sizeof(zbx_log_value_t));
			memset(value.log, 0, sizeof(zbx_log_value_t));
			value.log->value = zbx_strdup(NULL, str);
			break;
		case ITEM_VALUE_TYPE_STR:
		case ITEM_VALUE_TYPE_TEXT:
			value.str = zbx_strdup(NULL, str);
			break;
		case ITEM_VALUE_TYPE_FLOAT:
			value.dbl = atof(str);
			break;
		case ITEM_VALUE_TYPE_UINT64:
			ZBX_STR2UINT64(value.ui64, str);
			break;
	}

	return value;
}

static const char	*history_value2str(const ZBX_DC_HISTORY *h)
{
	static char	buffer[MAX_ID_LEN + 1];

	switch (h->value_type)
	{
		case ITEM_VALUE_TYPE_STR:
		case ITEM_VALUE_TYPE_TEXT:
			return h->value.str;
		case ITEM_VALUE_TYPE_LOG:
			return h->value.log->value;
		case ITEM_VALUE_TYPE_FLOAT:
			zbx_snprintf(buffer, sizeof(buffer), ZBX_FS_DBL, h->value.dbl);
			break;
		case ITEM_VALUE_TYPE_UINT64:
			zbx_snprintf(buffer, sizeof(buffer), ZBX_FS_UI64, h->value.ui64);
			break;
	}

	return buffer;
}

static void	clickhouse_log_error(CURL *handle, CURLcode error)
{
	long	http_code;

	if (CURLE_HTTP_RETURNED_ERROR == error)
	{
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_code);

		if (0 != page.offset)
		{
			zabbix_log(LOG_LEVEL_ERR, "cannot get values from clickhouse, HTTP error: %ld,", http_code);
		}
		else
			zabbix_log(LOG_LEVEL_ERR, "cannot get values from clickhouse, HTTP error: %ld", http_code);
	}
	else
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot get values from clickhouse: %s", curl_easy_strerror(error));
	}
}

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_close                                                          *
 *                                                                                  *
 * Purpose: closes connection and releases allocated resources                      *
 *                                                                                  *
 * Parameters:  hist - [IN] the history storage interface                           *
 *                                                                                  *
 ************************************************************************************/
static void	clickhouse_close(zbx_history_iface_t *hist)
{
	zbx_clickhouse_data_t	*data = hist->data;

	zbx_free(data->buf);
	//zbx_free(data->post_url);

	if (NULL != data->handle)
	{
		if (NULL != writer.handle)
			curl_multi_remove_handle(writer.handle, data->handle);

		curl_easy_cleanup(data->handle);
		data->handle = NULL;
	}
}

/******************************************************************************************************************
 *                                                                                                                *
 * common sql service support                                                                                     *
 *                                                                                                                *
 ******************************************************************************************************************/



/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_writer_init                                                    *
 *                                                                                  *
 * Purpose: initializes clickhouse writer for a new batch of history values            *
 *                                                                                  *
 ************************************************************************************/
static void	clickhouse_writer_init()
{
	if (0 != writer.initialized)
		return;

	zbx_vector_ptr_create(&writer.ifaces);

	if (NULL == (writer.handle = curl_multi_init()))
	{
		zbx_error("Cannot initialize cURL multi session");
		exit(EXIT_FAILURE);
	}

	writer.initialized = 1;
}

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_writer_release                                                 *
 *                                                                                  *
 * Purpose: releases initialized clickhouse writer by freeing allocated resources and  *
 *          setting its state to uninitialized.                                     *
 *                                                                                  *
 ************************************************************************************/
static void	clickhouse_writer_release()
{
	int	i;

	for (i = 0; i < writer.ifaces.values_num; i++)
		clickhouse_close(writer.ifaces.values[i]);

	curl_multi_cleanup(writer.handle);
	writer.handle = NULL;

	zbx_vector_ptr_destroy(&writer.ifaces);

	writer.initialized = 0;
}

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_writer_add_iface                                               *
 *                                                                                  *
 * Purpose: adds history storage interface to be flushed later                      *
 *                                                                                  *
 * Parameters: db_insert - [IN] bulk insert data                                    *
 *                                                                                  *
 ************************************************************************************/
static void	clickhouse_writer_add_iface(zbx_history_iface_t *hist)
{
	zbx_clickhouse_data_t	*data = hist->data;

	clickhouse_writer_init();

	if (NULL == (data->handle = curl_easy_init()))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot initialize cURL session");
		return;
	}

	//curl_easy_setopt(data->handle, CURLOPT_URL, data->post_url);
	curl_easy_setopt(data->handle, CURLOPT_URL, data->base_url);
	curl_easy_setopt(data->handle, CURLOPT_POST, 1);
	curl_easy_setopt(data->handle, CURLOPT_POSTFIELDS, data->buf);
	curl_easy_setopt(data->handle, CURLOPT_WRITEFUNCTION, curl_write_send_cb);
	curl_easy_setopt(data->handle, CURLOPT_FAILONERROR, 1L);

	curl_multi_add_handle(writer.handle, data->handle);

	zbx_vector_ptr_append(&writer.ifaces, hist);
}

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_writer_flush                                                   *
 *                                                                                  *
 * Purpose: posts historical data to clickhouse storage                                *
 *                                                                                  *
 ************************************************************************************/
static int	clickhouse_writer_flush()
{
	const char		*__function_name = "clickhouse_writer_flush";

	struct curl_slist	*curl_headers = NULL;
	int			i, running, previous, msgnum;
	CURLMsg			*msg;
	zbx_vector_ptr_t	retries;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (0 == writer.initialized)
		return SUCCEED;

	zbx_vector_ptr_create(&retries);

	curl_headers = curl_slist_append(curl_headers, "Content-Type: application/x-ndjson");

	for (i = 0; i < writer.ifaces.values_num; i++)
	{
		zbx_history_iface_t	*hist = (zbx_history_iface_t *)writer.ifaces.values[i];
		zbx_clickhouse_data_t	*data = hist->data;

		curl_easy_setopt(data->handle, CURLOPT_HTTPHEADER, curl_headers);

		zabbix_log(LOG_LEVEL_DEBUG, "sending %s", data->buf);
	}

try_again:
	previous = 0;

	do
	{
		int		fds;
		CURLMcode	code;

		if (CURLM_OK != (code = curl_multi_perform(writer.handle, &running)))
		{
			zabbix_log(LOG_LEVEL_ERR, "cannot perform on curl multi handle: %s", curl_multi_strerror(code));
			break;
		}

		if (CURLM_OK != (code = curl_multi_wait(writer.handle, NULL, 0, ZBX_HISTORY_STORAGE_DOWN, &fds)))
		{
			zabbix_log(LOG_LEVEL_ERR, "cannot wait on curl multi handle: %s", curl_multi_strerror(code));
			break;
		}

		if (previous == running)
			continue;

		while (NULL != (msg = curl_multi_info_read(writer.handle, &msgnum)))
		{
			/* If the error is due to malformed data, there is no sense on re-trying to send. */
			/* That's why we actually check for transport and curl errors separately */
			if (CURLE_HTTP_RETURNED_ERROR == msg->data.result)
			{
				long int	err;

				curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &err);

				zabbix_log(LOG_LEVEL_ERR, "cannot send data to clickhouse, HTTP error %ld",  err);
			}
			else if (CURLE_OK != msg->data.result)
			{
				zabbix_log(LOG_LEVEL_WARNING, "%s: %s", "cannot send to clickhouse",
						curl_easy_strerror(msg->data.result));

				/* If the error is due to curl internal problems or unrelated */
				/* problems with HTTP, we put the handle in a retry list and */
				/* remove it from the current execution loop */
				zbx_vector_ptr_append(&retries, msg->easy_handle);
				curl_multi_remove_handle(writer.handle, msg->easy_handle);
			}
		}

		previous = running;
	}
	while (running);

	/* We check if we have handles to retry. If yes, we put them back in the multi */
	/* handle and go to the beginning of the do while() for try sending the data again */
	/* after sleeping for ZBX_HISTORY_STORAGE_DOWN / 1000 (seconds) */
	if (0 < retries.values_num)
	{
		for (i = 0; i < retries.values_num; i++)
			curl_multi_add_handle(writer.handle, retries.values[i]);

		zbx_vector_ptr_clear(&retries);

		sleep(ZBX_HISTORY_STORAGE_DOWN / 1000);
		goto try_again;
	}

	curl_slist_free_all(curl_headers);

	zbx_vector_ptr_destroy(&retries);

	clickhouse_writer_release();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
	return SUCCEED;
}

/******************************************************************************************************************
 *                                                                                                                *
 * history interface support                                                                                      *
 *                                                                                                                *
 ******************************************************************************************************************/

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_destroy                                                        *
 *                                                                                  *
 * Purpose: destroys history storage interface                                      *
 *                                                                                  *
 * Parameters:  hist - [IN] the history storage interface                           *
 *                                                                                  *
 ************************************************************************************/
static void	clickhouse_destroy(zbx_history_iface_t *hist)
{
	zbx_clickhouse_data_t	*data = hist->data;

	clickhouse_close(hist);

	zbx_free(data->base_url);
	zbx_free(data);
}

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_get_values                                                     *
 *                                                                                  *
 * Purpose: gets item history data from history storage                             *
 *                                                                                  *
 * Parameters:  hist    - [IN] the history storage interface                        *
 *              itemid  - [IN] the itemid                                           *
 *              start   - [IN] the period start timestamp                           *
 *              count   - [IN] the number of values to read                         *
 *              end     - [IN] the period end timestamp                             *
 *              values  - [OUT] the item history data values                        *
 *                                                                                  *
 * Return value: SUCCEED - the history data were read successfully                  *
 *               FAIL - otherwise                                                   *
 *                                                                                  *
 * Comments: This function reads <count> values from ]<start>,<end>] interval or    *
 *           all values from the specified interval if count is zero.               *
 *                                                                                  *
 ************************************************************************************/

static int	clickhouse_get_values(zbx_history_iface_t *hist, zbx_uint64_t itemid, int start, int count, int end,
		zbx_vector_history_record_t *values)
{
	const char		*__function_name = "clickhouse_get_values";
	//static int first_run=0;
	

	zbx_clickhouse_data_t	*data = hist->data;

	int			ret=SUCCEED;
	int			i;

	CURLcode		err;
	struct curl_slist	*curl_headers = NULL;

	char	*sql_buffer=NULL;
	size_t			buf_alloc = 0, buf_offset = 0;

	zbx_history_record_t	hr;


	//if (0 == first_run) 
	//	first_run = time(NULL);
	
	//just for testing to avoid history thread startup hummering
	return ret;
	
	//fix to prevent ValueCache filling on zabbix server startup
	//in case when there are lots of items it's usually faster to 
	//fill them via polling and not to kill the database with millions 
	//of requests
	
	//if (time(NULL)-first_run < ZBX_VALUECACHE_FILL_TIME) return (ret);
	

	//remove this after fixing segv on zabbix 4+
	//but in reality, the server DOES NOT HAVE TO GO TO SLOW DB to get last year's average
	//it better to plan your checks right or increase ValueCache mem size
	//return (ret);

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() Data request: we are asked for item%ld starting:%d ending:%d, count:%d", __function_name,itemid,start,end,count);


	//HACK FIX TODO make it proper
	//i see no reason to hold data in the value cache older then 1 day long 
	//sure there must NOT be triggers with such a demand
	//this is dirty workaround, has to fix like in ZABBIX history sql module
	//trying to select data for several periods starting from day and extending 
	//to month, but i am sure for triggering and alerting data from more then one 
	//day long isn't really important
	//
	//to make things proper remove next condition, i put it here to resolver
	//clickhouse overload problem but it was something else, amyway i've decided to leave it here

	//if (end - start > 86400) {
	  //  start=end-7*86400;
	//} 


	if (NULL == (data->handle = curl_easy_init()))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot initialize cURL session");
		return FAIL;
	}

	zbx_snprintf_alloc(&sql_buffer, &buf_alloc, &buf_offset, 
		"SELECT  toUInt32(clock),ns,value,value_dbl,value_str FROM %s WHERE itemid=%ld ",
		CONFIG_HISTORY_STORAGE_TABLE_NAME,itemid);


	if (1 == end-start) {

		zbx_snprintf_alloc(&sql_buffer, &buf_alloc, &buf_offset, "AND clock = %d ", end);

	} else {
		if (0 < start) {
			zbx_snprintf_alloc(&sql_buffer, &buf_alloc, &buf_offset, "AND clock > %d ", start);
		}
		if (0 < end ) {
			zbx_snprintf_alloc(&sql_buffer, &buf_alloc, &buf_offset, "AND clock <= %d ", end);
		}
	}

	zbx_snprintf_alloc(&sql_buffer, &buf_alloc, &buf_offset, "ORDER BY clock DESC ");

	if (0<count) 
	{
	    zbx_snprintf_alloc(&sql_buffer, &buf_alloc, &buf_offset, "LIMIT %d", count);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "sending query to clickhouse: %s", sql_buffer);


	curl_easy_setopt(data->handle, CURLOPT_URL, data->base_url);
	curl_easy_setopt(data->handle, CURLOPT_POSTFIELDS, sql_buffer);
	curl_easy_setopt(data->handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(data->handle, CURLOPT_HTTPHEADER, curl_headers);
	curl_easy_setopt(data->handle, CURLOPT_FAILONERROR, 1L);


	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(data->handle)))
	{
		clickhouse_log_error(data->handle, err);
		goto out;
	}


	//curl_easy_setopt(data->handle, CURLOPT_URL, data->post_url);
	curl_easy_setopt(data->handle, CURLOPT_URL, data->base_url);
	zabbix_log(LOG_LEVEL_DEBUG, "recieved from clickhouse: %s", page.data);
		
	
	char *end_str;
	if (NULL !=page.data && page.data[0]!=0) {

	    //zabbix_log(LOG_LEVEL_DEBUG, "Parcing line by line");
	    int line_count=0, field_count=0;


	    char *line_ptr = strtok_r(page.data, "\n", &end_str);

	    while (line_ptr != NULL)
	    {
			char *end_field;
			char *field_ptr = strtok_r(line_ptr, "\t", &end_field);
			char *fields[MAX_HISTORY_CLICKHOUSE_FIELDS];

			zabbix_log(LOG_LEVEL_DEBUG, "Parsing line '%s'", line_ptr);

			for (i=0; i++; i<field_count)  fields[i]=NULL; 

			while (field_ptr != NULL && MAX_HISTORY_CLICKHOUSE_FIELDS>field_count) 
			{	
				fields[field_count++]=field_ptr;
				field_ptr = strtok_r(NULL, "\t", &end_field);
			}
			
			//the fields order  must be in sync with SQL query above
			//OR TODO: make it via some proper interface, perhaps JSON or whatever clickhouse supports to 
			//be able to distingiosh wich value is from what field name, not depending on the order in SQL request


			zabbix_log(LOG_LEVEL_TRACE, "Parsed line %d clock:'%s', ns:'%s', value:'%s', value_dbl:'%s' '",line_count, fields[0],fields[1],fields[2],fields[3]);

			if (NULL != fields[4]) 
			{
				//we've got at least three fields
				hr.timestamp.sec = atoi(fields[0]);
				hr.timestamp.ns = atoi(fields[1]);
				switch (hist->value_type)
				{
					case ITEM_VALUE_TYPE_UINT64:
						zabbix_log(LOG_LEVEL_TRACE, "Parsed  as UINT64 %s",fields[2]);
			    		hr.value = history_str2value(fields[2], hist->value_type);
						break;

					case ITEM_VALUE_TYPE_FLOAT: 
						zabbix_log(LOG_LEVEL_TRACE, "Parsed  as DBL field %s",fields[3]);
			    		hr.value = history_str2value(fields[3], hist->value_type);
						break;
					case ITEM_VALUE_TYPE_STR:
					case ITEM_VALUE_TYPE_TEXT:
						//!!!! for some reason there are major memory leak when reading 
						//string values from history.
						//remove the following goto statement if you really need it
						goto out;

						zabbix_log(LOG_LEVEL_TRACE, "Parsed  as STR/TEXT type %s",fields[4]);
						hr.value = history_str2value(fields[4], hist->value_type);
					case ITEM_VALUE_TYPE_LOG:
						//todo: if i ever need this, but for now there is no log write to clickhouse
						goto out;
				}				
				//adding to zabbix vector
				zbx_vector_history_record_append_ptr(values, &hr);

				ret=SUCCEED;
				line_count++;
			} else {
				zabbix_log(LOG_LEVEL_DEBUG, "Skipping the result, not enough fields");
			}
			
			line_ptr = strtok_r(NULL, "\n", &end_str);
	    }
	    page.data[0]=0;	
	} else 
	{
	    zabbix_log(LOG_LEVEL_DEBUG, "No data from clickhouse");
	    ret = SUCCEED;
	}


out:
	clickhouse_close(hist);
	curl_slist_free_all(curl_headers);
	zbx_free(sql_buffer);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
	return SUCCEED;
}

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_add_values                                                     *
 *                                                                                  *
 * Purpose: sends history data to the storage                                       *
 *                                                                                  *
 * Parameters:  hist    - [IN] the history storage interface                        *
 *              history - [IN] the history data vector (may have mixed value types) *
 *                                                                                  *
 ************************************************************************************/
static int	clickhouse_add_values(zbx_history_iface_t *hist, const zbx_vector_ptr_t *history)
{
	const char	*__function_name = "clickhouse_add_values";

	char *tmp_buffer=NULL;	
	size_t tmp_alloc=0, tmp_offset=0;

	zbx_clickhouse_data_t	*data = hist->data;
	int			i, num = 0;
	ZBX_DC_HISTORY		*h;

	size_t			buf_alloc = 0, buf_offset = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
	

	zbx_snprintf_alloc(&tmp_buffer,&tmp_alloc,&tmp_offset,"INSERT INTO %s VALUES ", CONFIG_HISTORY_STORAGE_TABLE_NAME );

	for (i = 0; i < history->values_num; i++)
	{
		h = (ZBX_DC_HISTORY *)history->values[i];

		if (hist->value_type != h->value_type)
			continue;

		
		 if (ITEM_VALUE_TYPE_UINT64 == h->value_type) {
		    //zabbix_log(LOG_LEVEL_DEBUG, "Parsing value as UIN64 type");
		    zbx_snprintf_alloc(&tmp_buffer,&tmp_alloc,&tmp_offset,"(CAST(%d as date) ,%ld,%d,%d,%ld,0,''),",
					h->ts.sec,h->itemid,h->ts.sec,h->ts.ns,h->value.ui64);
		}

		 if (ITEM_VALUE_TYPE_FLOAT == h->value_type) {
		    //zabbix_log(LOG_LEVEL_DEBUG, "Parsing value as float type");
		    zbx_snprintf_alloc(&tmp_buffer,&tmp_alloc,&tmp_offset,"(CAST(%d as date) ,%ld,%d,%d,0,%f,''),",
					h->ts.sec,h->itemid,h->ts.sec,h->ts.ns,h->value.dbl);
		}

		 if (ITEM_VALUE_TYPE_STR == h->value_type || 
		 	 ITEM_VALUE_TYPE_TEXT == h->value_type ) {
		    //zabbix_log(LOG_LEVEL_DEBUG, "Parsing value as string or text type");
		    zbx_snprintf_alloc(&tmp_buffer,&tmp_alloc,&tmp_offset,"(CAST(%d as date) ,%ld,%d,%d,0,0,'%s'),",
					h->ts.sec,h->itemid,h->ts.sec,h->ts.ns,h->value.str);
		}

		if (ITEM_VALUE_TYPE_LOG == h->value_type)
		{
			const zbx_log_value_t	*log;
			log = h->value.log;
		}

		num++;
	}

	if (num > 0)
	{ 
		zbx_snprintf_alloc(&data->buf, &buf_alloc, &buf_offset, "%s\n", tmp_buffer);
		zabbix_log(LOG_LEVEL_DEBUG, "will insert to clickhouse: %s",data->buf);
	
		//data->post_url = zbx_dsprintf(NULL, "%s", data->base_url);
		clickhouse_writer_add_iface(hist);
	}

	zbx_free(tmp_buffer);
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
	
	return num;
}

/************************************************************************************
 *                                                                                  *
 * Function: clickhouse_flush                                                          *
 *                                                                                  *
 * Purpose: flushes the history data to storage                                     *
 *                                                                                  *
 * Parameters:  hist    - [IN] the history storage interface                        *
 *                                                                                  *
 * Comments: This function will try to flush the data until it succeeds or          *
 *           unrecoverable error occurs                                             *
 *                                                                                  *
 ************************************************************************************/
static int	clickhouse_flush(zbx_history_iface_t *hist)
{
	ZBX_UNUSED(hist);

	return clickhouse_writer_flush();
}

/************************************************************************************
 *                                                                                  *
 * Function: zbx_history_clickhouse_init                                               *
 *                                                                                  *
 * Purpose: initializes history storage interface                                   *
 *                                                                                  *
 * Parameters:  hist       - [IN] the history storage interface                     *
 *              value_type - [IN] the target value type                             *
 *              error      - [OUT] the error message                                *
 *                                                                                  *
 * Return value: SUCCEED - the history storage interface was initialized            *
 *               FAIL    - otherwise                                                *
 *                                                                                  *
 ************************************************************************************/
int	zbx_history_clickhouse_init(zbx_history_iface_t *hist, unsigned char value_type, char **error)
{
	zbx_clickhouse_data_t	*data;

	if (0 != curl_global_init(CURL_GLOBAL_ALL))
	{
		*error = zbx_strdup(*error, "Cannot initialize cURL library");
		return FAIL;
	}

	data = zbx_malloc(NULL, sizeof(zbx_clickhouse_data_t));
	memset(data, 0, sizeof(zbx_clickhouse_data_t));
	data->base_url = zbx_strdup(NULL, CONFIG_HISTORY_STORAGE_URL);
	zbx_rtrim(data->base_url, "/");
	data->buf = NULL;
	//data->post_url = NULL;
	data->handle = NULL;

	hist->value_type = value_type;
	hist->data = data;
	hist->destroy = clickhouse_destroy;
	hist->add_values = clickhouse_add_values;
	hist->flush = clickhouse_flush;
	hist->get_values = clickhouse_get_values;
	hist->requires_trends = 0;

	return SUCCEED;
}

#else

int	zbx_history_clickhouse_init(zbx_history_iface_t *hist, unsigned char value_type, char **error)
{
	ZBX_UNUSED(hist);
	ZBX_UNUSED(value_type);

	*error = zbx_strdup(*error, "cURL library support >= 7.28.0 is required for clickhouse history backend");
	return FAIL;
}

#endif
