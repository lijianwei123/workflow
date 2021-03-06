/*
  Copyright (c) 2019 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Authors: Li Yingxin (liyingxin@sogou-inc.com)
*/

#include "mysql_types.h"
#include "mysql_byteorder.h"
#include "MySQLResult.h"

namespace protocol
{

MySQLField::MySQLField(const void *buf, mysql_field_t *field)
{
	const char *p = (const char *)buf;

	this->name = p + field->name_offset;
	this->name_length = field->name_length;	

	this->org_name = p + field->org_name_offset;
	this->org_name_length = field->org_name_length;

	this->table = p + field->table_offset;
	this->table_length = field->table_length;

	this->org_table = p + field->org_table_offset;
	this->org_table_length = field->org_table_length;

	this->db = p + field->db_offset;
	this->db_length = field->db_length;

	this->catalog = p + field->catalog_offset;
	this->catalog_length = field->catalog_length;

	if (field->def_offset == (size_t)-1 && field->def_length == 0)
	{
		this->def = NULL;
		this->def_length = 0;
	} else {
		this->def = p + field->def_offset;
		this->def_length = field->def_length;
	}

	this->flags = field->flags;
	this->decimals = field->decimals;
	this->charsetnr = field->charsetnr;
	this->data_type = field->data_type;
}

MySQLResultCursor::MySQLResultCursor()
{
	this->init();
}

void MySQLResultCursor::init()
{
	this->current_field = 0;
	this->current_row = 0;
	this->field_count = 0;
	this->fields = NULL;
	this->parser = NULL;
	this->status = MYSQL_STATUS_NOT_INIT;
}

MySQLResultCursor::MySQLResultCursor(MySQLResponse *resp)
{
	this->init(resp);
}

void MySQLResultCursor::reset(MySQLResponse *resp)
{
	this->clear();
	this->init(resp);
}

void MySQLResultCursor::fetch_result_set(const struct __mysql_result_set *result_set)
{
	const char *buf = (const char *)this->parser->buf;

	this->field_count = result_set->field_count;
	this->start = buf + result_set->rows_begin_offset;
	this->pos = this->start;
	this->end = buf + result_set->rows_end_offset;
	this->row_count = result_set->row_count;

	this->fields = new MySQLField *[this->field_count];
	for (int i = 0; i < this->field_count; i++)
		this->fields[i] = new MySQLField(this->parser->buf, result_set->fields[i]);
}

void MySQLResultCursor::init(MySQLResponse *resp)
{
	this->current_field = 0;
	this->current_row = 0;
	this->field_count = 0;
	this->fields = NULL;
	this->parser = resp->get_parser();

	if (!list_empty(&this->parser->result_set_list))
	{
		struct __mysql_result_set *result_set;

		mysql_result_set_cursor_init(&this->cursor, this->parser);
		mysql_result_set_cursor_next(&result_set, &this->cursor);

		this->fetch_result_set(result_set);
		this->status = MYSQL_STATUS_GET_RESULT;
	}
	else if (this->parser->packet_type == MYSQL_PACKET_ERROR)
		this->status = MYSQL_STATUS_ERROR;
	else if (this->parser->packet_type == MYSQL_PACKET_OK)
		this->status = MYSQL_STATUS_OK;
	else
		this->status = MYSQL_STATUS_NOT_INIT;
}

bool MySQLResultCursor::next_result_set()
{
	if (this->status != MYSQL_STATUS_GET_RESULT &&
			this->status != MYSQL_STATUS_END)
		return false;

	struct __mysql_result_set *result_set;
	if (mysql_result_set_cursor_next(&result_set, &this->cursor) == 0)
	{
		for (int i = 0; i < this->field_count; i++)
			delete this->fields[i];

		delete []this->fields;

		this->current_field = 0;
		this->current_row = 0;

		this->fetch_result_set(result_set);
		this->status = MYSQL_STATUS_GET_RESULT;
		return true;
	}
	else
	{
		this->status = MYSQL_STATUS_END;
		return false;
	}
}

bool MySQLResultCursor::fetch_row(std::vector<MySQLCell>& row_arr)
{
	if (this->status != MYSQL_STATUS_GET_RESULT)
		return false;

	unsigned long long len;
	const char *data;
	int data_type;

	const char *p = (const char *)this->pos;
	const char *end = (const char *)this->end;
	
	row_arr.clear();

	for (int i = 0; i < this->field_count; i++)
	{
		data_type = this->fields[i]->get_data_type();
		if (*(const unsigned char *)p == MYSQL_PACKET_HEADER_NULL)
		{
			data = NULL;
			len = 0;
			p++;
			data_type = MYSQL_TYPE_NULL;
		} else if (decode_string(&data, &len, &p, end) == false) {
			this->status = MYSQL_STATUS_ERROR;
			return false;
		}

		row_arr.emplace_back(data, len, data_type);
	}

	if (++this->current_row == this->row_count)
		this->status = MYSQL_STATUS_END;

	this->pos = p;

	return true;
}

bool MySQLResultCursor::fetch_row(std::map<std::string, MySQLCell>& row_map)
{
	return this->fetch_row<std::map<std::string, MySQLCell>>(row_map);
}

bool MySQLResultCursor::fetch_row(std::unordered_map<std::string, MySQLCell>& row_map)
{
	return this->fetch_row<std::unordered_map<std::string, MySQLCell>>(row_map);
}

bool MySQLResultCursor::fetch_row_nocopy(const void **data, size_t *len, int *data_type)
{
	if (this->status != MYSQL_STATUS_GET_RESULT)
		return false;

	unsigned long long cell_len;
	const char *cell_data;

	const char *p = (const char *)this->pos;
	const char *end = (const char *)this->end;

	for (int i = 0; i < this->field_count; i++)
	{	
		if (*(const unsigned char *)p == MYSQL_PACKET_HEADER_NULL)
		{
			cell_data = NULL;
			cell_len = 0;
			p++;
		} else if (decode_string(&cell_data, &cell_len, &p, end) == false) {
			this->status = MYSQL_STATUS_ERROR;
			return false;
		}
		data[i] = cell_data;
		len[i] = cell_len;
		data_type[i] = this->fields[i]->get_data_type();
	}

	this->pos = p;

	if (++this->current_row == this->row_count)
		this->status = MYSQL_STATUS_END;

	return true;
}

bool MySQLResultCursor::fetch_all(std::vector<std::vector<MySQLCell>>& rows)
{
	if (this->status != MYSQL_STATUS_GET_RESULT)
		return false;

	unsigned long long len;
	const char *data;
	int data_type;

	const char *p = (const char *)this->pos;
	const char *end = (const char *)this->end;

	rows.clear();

	for (int i = this->current_row; i < this->row_count; i++)
	{
		std::vector<MySQLCell> tmp;
		for (int j = 0; j < this->field_count; j++)
		{
			data_type = this->fields[j]->get_data_type();
			if (*(const unsigned char *)p == MYSQL_PACKET_HEADER_NULL)
			{
				data = NULL;
				len = 0;
				p++;
				data_type = MYSQL_TYPE_NULL;
			} else if (decode_string(&data, &len, &p, end) == false) {
				this->status = MYSQL_STATUS_ERROR;
				return false;
			}

			tmp.emplace_back(data, len, data_type);
		}
		rows.emplace_back(std::move(tmp));
	}

	this->current_row = this->row_count;
	this->status = MYSQL_STATUS_END;
	this->pos = p;

	return true;
}

void MySQLResultCursor::first_result_set()
{
	if (this->status != MYSQL_STATUS_GET_RESULT &&
			this->status != MYSQL_STATUS_END)
		return;

	mysql_result_set_cursor_rewind(&this->cursor);
	struct __mysql_result_set *result_set;
	if (mysql_result_set_cursor_next(&result_set, &this->cursor) == 0)
	{
		for (int i = 0; i < this->field_count; i++)
			delete this->fields[i];

		delete []this->fields;

		this->current_field = 0;
		this->current_row = 0;

		this->fetch_result_set(result_set);
		this->status = MYSQL_STATUS_GET_RESULT;
	}
}

void MySQLResultCursor::rewind()
{
	if (this->status != MYSQL_STATUS_GET_RESULT &&
			this->status != MYSQL_STATUS_END)
		return;

	this->current_field = 0;
	this->current_row = 0;
	this->pos = this->start;
	this->status = MYSQL_STATUS_GET_RESULT;
}

}

