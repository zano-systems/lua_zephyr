/**
 * @file luaz_msg_descr.c
 * @brief Descriptor-based Lua <-> C struct conversion for zbus messages.
 *
 * Provides lua_msg_descr_to_table and lua_msg_descr_from_table helper
 * functions used by lua_zbus.c via the channel user_data descriptor lookup.
 */

#include <luaz_msg_descr.h>
#include <lauxlib.h>
#include <string.h>
#include <zephyr/kernel.h>

/**
 * @brief Encode C struct fields into a Lua table (push to stack).
 *
 * Iterates over @p fields, reads each value from @p base + offset,
 * and pushes a new Lua table with the corresponding key/value pairs.
 * Nested LUA_MSG_TYPE_OBJECT fields recurse into sub-descriptors.
 *
 * @param L            Lua state.
 * @param fields       Array of field descriptors.
 * @param field_count  Number of fields.
 * @param base         Base pointer to the C struct data.
 */
void lua_msg_descr_to_table(lua_State *L, const struct lua_msg_field_descr *fields,
			    size_t field_count, const void *base)
{
	/* Presized: one hash allocation instead of a rehash at each power of two. */
	lua_createtable(L, 0, (int)field_count);

	for (size_t i = 0; i < field_count; i++) {
		const struct lua_msg_field_descr *f = &fields[i];
		const void *ptr = (const uint8_t *)base + f->offset;

		switch (f->type) {
		case LUA_MSG_TYPE_INT: {
			lua_Integer val = 0;

			switch (f->size) {
			case 1:
				val = *(const int8_t *)ptr;
				break;
			case 2:
				val = *(const int16_t *)ptr;
				break;
			case 4:
				val = *(const int32_t *)ptr;
				break;
			case 8:
				val = *(const int64_t *)ptr;
				break;
			}
			lua_pushinteger(L, val);
			break;
		}
		case LUA_MSG_TYPE_UINT: {
			lua_Integer val = 0;

			switch (f->size) {
			case 1:
				val = *(const uint8_t *)ptr;
				break;
			case 2:
				val = *(const uint16_t *)ptr;
				break;
			case 4:
				val = *(const uint32_t *)ptr;
				break;
			case 8:
				val = (lua_Integer)(*(const uint64_t *)ptr);
				break;
			}
			lua_pushinteger(L, val);
			break;
		}
		case LUA_MSG_TYPE_NUMBER: {
			lua_Number val = 0;

			if (f->size == sizeof(float)) {
				val = *(const float *)ptr;
			} else {
				val = *(const double *)ptr;
			}
			lua_pushnumber(L, val);
			break;
		}
		case LUA_MSG_TYPE_STRING:
			lua_pushstring(L, *(const char *const *)ptr);
			break;
		case LUA_MSG_TYPE_STRING_BUF:
			lua_pushstring(L, (const char *)ptr);
			break;
		case LUA_MSG_TYPE_BOOL:
			lua_pushboolean(L, *(const bool *)ptr);
			break;
		case LUA_MSG_TYPE_OBJECT:
			lua_msg_descr_to_table(L, f->sub_fields, f->sub_field_count, ptr);
			break;
		}

		lua_setfield(L, -2, f->field_name);
	}
}

/**
 * @brief Decode a Lua table into C struct fields.
 *
 * For each descriptor in @p fields, reads the named key from the Lua table
 * at @p table_idx and writes the converted value into @p base + offset.
 * Missing keys (nil) are silently skipped.
 *
 * @param L            Lua state.
 * @param fields       Array of field descriptors.
 * @param field_count  Number of fields.
 * @param base         Base pointer to the C struct to populate.
 * @param table_idx    Absolute Lua stack index of the source table.
 */
void lua_msg_descr_from_table(lua_State *L, const struct lua_msg_field_descr *fields,
			      size_t field_count, void *base, int table_idx)
{
	for (size_t i = 0; i < field_count; i++) {
		const struct lua_msg_field_descr *f = &fields[i];
		void *ptr = (uint8_t *)base + f->offset;

		lua_getfield(L, table_idx, f->field_name);

		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			continue;
		}

		switch (f->type) {
		case LUA_MSG_TYPE_INT: {
			lua_Integer val = lua_tointeger(L, -1);

			switch (f->size) {
			case 1:
				*(int8_t *)ptr = (int8_t)val;
				break;
			case 2:
				*(int16_t *)ptr = (int16_t)val;
				break;
			case 4:
				*(int32_t *)ptr = (int32_t)val;
				break;
			case 8:
				*(int64_t *)ptr = (int64_t)val;
				break;
			}
			break;
		}
		case LUA_MSG_TYPE_UINT: {
			lua_Integer val = lua_tointeger(L, -1);

			switch (f->size) {
			case 1:
				*(uint8_t *)ptr = (uint8_t)val;
				break;
			case 2:
				*(uint16_t *)ptr = (uint16_t)val;
				break;
			case 4:
				*(uint32_t *)ptr = (uint32_t)val;
				break;
			case 8:
				*(uint64_t *)ptr = (uint64_t)val;
				break;
			}
			break;
		}
		case LUA_MSG_TYPE_NUMBER: {
			lua_Number val = lua_tonumber(L, -1);

			if (f->size == sizeof(float)) {
				*(float *)ptr = (float)val;
			} else {
				*(double *)ptr = (double)val;
			}
			break;
		}
		case LUA_MSG_TYPE_STRING:
			*(const char **)ptr = lua_tostring(L, -1);
			break;
		case LUA_MSG_TYPE_STRING_BUF: {
			const char *s = lua_tostring(L, -1);

			if (s) {
				strncpy((char *)ptr, s, f->size - 1);
				((char *)ptr)[f->size - 1] = '\0';
			}
			break;
		}
		case LUA_MSG_TYPE_BOOL:
			*(bool *)ptr = lua_toboolean(L, -1);
			break;
		case LUA_MSG_TYPE_OBJECT: {
			int nested_idx = lua_absindex(L, -1);

			lua_msg_descr_from_table(L, f->sub_fields, f->sub_field_count, ptr,
						 nested_idx);
			break;
		}
		}

		lua_pop(L, 1);
	}
}
