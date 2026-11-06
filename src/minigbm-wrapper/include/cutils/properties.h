/* NDK shim for the libcutils property API used by gbm_mesa_wrapper.cpp. */
#pragma once
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

#define PROPERTY_VALUE_MAX PROP_VALUE_MAX

static inline int property_get(const char *key, char *value, const char *default_value)
{
	int len = __system_property_get(key, value);
	if (len > 0)
		return len;
	if (default_value) {
		len = (int)strnlen(default_value, PROPERTY_VALUE_MAX - 1);
		memcpy(value, default_value, len);
		value[len] = '\0';
		return len;
	}
	value[0] = '\0';
	return 0;
}

static inline bool property_get_bool(const char *key, bool default_value)
{
	char v[PROPERTY_VALUE_MAX];
	if (__system_property_get(key, v) < 1)
		return default_value;
	if (!strcmp(v, "1") || !strcmp(v, "y") || !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "true"))
		return true;
	if (!strcmp(v, "0") || !strcmp(v, "n") || !strcmp(v, "no") || !strcmp(v, "off") || !strcmp(v, "false"))
		return false;
	return default_value;
}
