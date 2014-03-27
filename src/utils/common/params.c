/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "utils/common/params.h"

#include <stdio.h>
#include <stdlib.h>			// free
#include <netinet/in.h>			// in_addr
#include <arpa/inet.h>			// inet_pton
#include <sys/socket.h>			// AF_INET (BSD)

#ifdef LIBIDN
#include <idna.h>
#endif

#include "libknot/libknot.h"
#include "common/errcode.h"		// KNOT_EOK
#include "common/mempattern.h"		// strcdup
#include "common/descriptor.h"		// KNOT_RRTYPE_
#include "utils/common/msg.h"		// WARN
#include "utils/common/resolv.h"	// parse_nameserver
#include "utils/common/token.h"		// token

#define IPV4_REVERSE_DOMAIN	"in-addr.arpa."
#define IPV6_REVERSE_DOMAIN	"ip6.arpa."

char* name_from_idn(const char *idn_name) {
#ifdef LIBIDN
	char *name = NULL;

	int rc = idna_to_ascii_lz(idn_name, &name, 0);
	if (rc != IDNA_SUCCESS) {
		ERR("IDNA (%s)\n", idna_strerror(rc));
		return NULL;
	}

	return name;
#endif
	return strdup(idn_name);
}

void name_to_idn(char **name) {
#ifdef LIBIDN
	char *idn_name = NULL;

	int rc = idna_to_unicode_8zlz(*name, &idn_name, 0);
	if (rc != IDNA_SUCCESS) {
		return;
	}

	free(*name);
	*name = idn_name;
#endif
	return;
}

/*!
 * \brief Checks if string is a prefix of reference string.
 *
 * \param pref		Prefix string.
 * \param pref_len	Prefix length.
 * \param str		Reference string (must have trailing zero).
 *
 * \retval -1		\a pref is not a prefix of \a str.
 * \retval  0<=		number of chars after prefix \a pref in \a str.
 */
static int cmp_prefix(const char *pref, const size_t pref_len,
                      const char *str)
{
	size_t i = 0;
	while (1) {
		// Different characters => NOT prefix.
		if (pref[i] != str[i]) {
			return -1;
		}

		i++;

		// Pref IS a prefix of pref.
		if (i == pref_len) {
			size_t rest = 0;
			while (str[i + rest] != '\0') {
				rest++;
			}
			return rest;
		// Pref is longer then ref => NOT prefix.
		} else if (str[i] == '\0') {
			return -1;
		}
	}
}

/*!
 * \brief Find the best parameter match in table based on prefix equality.
 *
 * \param str		Parameter name to look up.
 * \param str_len	Parameter name length.
 * \param tbl		Parameter table.
 * \param unique	Indication if output is unique result.
 *
 * \retval >=0		looked up parameter position in \a tbl.
 * \retval err		if error.
 */
int best_param(const char *str, const size_t str_len, const param_t *tbl,
               bool *unique)
{
	if (str == NULL || str_len == 0 || tbl == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	int best_pos = -1;
	int best_match = INT_MAX;
	size_t matches = 0;
	for (int i = 0; tbl[i].name != NULL; i++) {
		int ret = cmp_prefix(str, str_len, tbl[i].name);
		switch (ret) {
		case -1:
			continue;
		case 0:
			best_pos = i;
			best_match = 0;
			matches = 1;
			break;
		default:
			if (ret < best_match) {
				best_pos = i;
				best_match = ret;
			}
			matches++;
		}
	}

	switch (matches) {
	case 0:
		return KNOT_ENOTSUP;
	case 1:
		*unique = true;
		return best_pos;
	default:
		*unique = false;
		return best_pos;
	}
}

char* get_reverse_name(const char *name)
{
	struct in_addr	addr4;
	struct in6_addr	addr6;
	int		ret;
	char		buf[128] = "\0";

	if (name == NULL) {
		DBG_NULL;
		return NULL;
	}

	// Check name for IPv4 address, IPv6 address or other.
	if (inet_pton(AF_INET, name, &addr4) == 1) {
		uint32_t num = ntohl(addr4.s_addr);

		// Create IPv4 reverse FQD name.
		ret = snprintf(buf, sizeof(buf), "%u.%u.%u.%u.%s",
		               (num >>  0) & 0xFF, (num >>  8) & 0xFF,
		               (num >> 16) & 0xFF, (num >> 24) & 0xFF,
		               IPV4_REVERSE_DOMAIN);
		if (ret < 0 || (size_t)ret >= sizeof(buf)) {
			return NULL;
		}

		return strdup(buf);
	} else if (inet_pton(AF_INET6, name, &addr6) == 1) {
		char	*pos = buf;
		size_t  len = sizeof(buf);
		uint8_t left, right;

		// Create IPv6 reverse name.
		for (int i = 15; i >= 0; i--) {
			left = ((addr6.s6_addr)[i] & 0xF0) >> 4;
			right = (addr6.s6_addr)[i] & 0x0F;

			ret = snprintf(pos, len, "%x.%x.", right, left);
			if (ret < 0 || (size_t)ret >= len) {
				return NULL;
			}

			pos += ret;
			len -= ret;
		}

		// Add IPv6 reverse domain.
		ret = snprintf(pos, len, "%s", IPV6_REVERSE_DOMAIN);
		if (ret < 0 || (size_t)ret >= len) {
			return NULL;
		}

		return strdup(buf);
	} else {
		return NULL;
	}
}

char* get_fqd_name(const char *name)
{
	char *fqd_name = NULL;

	if (name == NULL) {
		DBG_NULL;
		return NULL;
	}

	size_t name_len = strlen(name);

	// If the name is FQDN, make a copy.
	if (name[name_len - 1] == '.') {
		fqd_name = strdup(name);
	// Else make a copy and append a trailing dot.
	} else {
		fqd_name = malloc(name_len + 2);
		if (fqd_name != NULL) {
			strncpy(fqd_name, name, name_len + 2);
			fqd_name[name_len] = '.';
			fqd_name[name_len + 1] = 0;
		}
	}

	return fqd_name;
}

int params_parse_class(const char *value, uint16_t *rclass)
{
	if (value == NULL || rclass == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	if (knot_rrclass_from_string(value, rclass) == 0) {
		return KNOT_EOK;
	} else {
		return KNOT_EINVAL;
	}
}

int params_parse_type(const char *value, uint16_t *rtype, uint32_t *xfr_serial)
{
	if (value == NULL || rtype == NULL || xfr_serial == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	size_t param_pos = strcspn(value, "=");

	// There is no additional parameter.
	if (param_pos == strlen(value)) {
		if (knot_rrtype_from_string(value, rtype) != 0) {
			return KNOT_EINVAL;
		}

		// IXFR requires serial parameter.
		if (*rtype == KNOT_RRTYPE_IXFR) {
			DBG("SOA serial is required for IXFR query\n");
			return KNOT_EINVAL;
		}
	} else {
		char *type_char = strndup(value, param_pos);

		if (knot_rrtype_from_string(type_char, rtype) != 0) {
			free(type_char);
			return KNOT_EINVAL;
		}

		free(type_char);

		// Additional parameter is accepted for IXFR only.
		if (*rtype == KNOT_RRTYPE_IXFR) {
			const char *param_str = value + 1 + param_pos;
			char *end;

			// Convert string to serial.
			unsigned long long serial = strtoull(param_str, &end, 10);

			// Check for bad serial string.
			if (end == param_str || *end != '\0' ||
			    serial > UINT32_MAX) {
				DBG("bad SOA serial %s\n", param_str);
				return KNOT_EINVAL;
			}

			*xfr_serial = serial;
		} else {
			char buf[64] = "";
			knot_rrtype_to_string(*rtype, buf, sizeof(buf));
			DBG("type %s can't have a parameter\n", buf);
			return KNOT_EINVAL;
		}
	}

	return KNOT_EOK;
}

int params_parse_server(const char *value, list_t *servers, const char *def_port)
{
	if (value == NULL || servers == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	// Add specified nameserver.
	srv_info_t *server = parse_nameserver(value, def_port);
	if (server == NULL) {
		ERR("bad nameserver %s\n", value);
		return KNOT_EINVAL;
	}
	add_tail(servers, (node_t *)server);

	return KNOT_EOK;
}

int params_parse_wait(const char *value, int32_t *dst)
{
	char *end;

	if (value == NULL || dst == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	/* Convert string to number. */
	long long num = strtoll(value, &end, 10);

	/* Check for bad string (empty or incorrect). */
	if (end == value || *end != '\0') {
		ERR("bad time value %s\n", value);
		return KNOT_EINVAL;
	} else if (num < 1) {
		num = 1;
		WARN("time %s is too short, using %lld instead\n", value, num);
	/* Reduce maximal value. Poll takes signed int in milliseconds. */
	} else if (num > INT32_MAX) {
		num = INT32_MAX / 1000;
		WARN("time %s is too big, using %lld instead\n", value, num);
	}

	*dst = num;

	return KNOT_EOK;
}

int params_parse_num(const char *value, uint32_t *dst)
{
	char *end;

	if (value == NULL || dst == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	// Convert string to number.
	long long num = strtoll(value, &end, 10);

	// Check for bad string.
	if (end == value || *end != '\0') {
		ERR("bad number %s\n", value);
		return KNOT_EINVAL;
	}

	if (num > UINT32_MAX) {
		num = UINT32_MAX;
		WARN("number %s is too big, using %lld instead\n", value, num);
	} else if (num < 0) {
		num = 0;
		WARN("number %s is too small, using %lld instead\n", value, num);
	}

	*dst = num;

	return KNOT_EOK;
}

int params_parse_tsig(const char *value, knot_key_params_t *key_params)
{
	if (value == NULL || key_params == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	/* Invalidate previous key. */
	if (key_params->name) {
		ERR("Key specified multiple times.\n");
		return KNOT_EINVAL;
	}

	char *h = strdup(value);
	if (!h) {
		return KNOT_ENOMEM;
	}

	/* Separate to avoid multiple allocs. */
	char *k = NULL, *s = NULL;
	if ((k = (char*)strchr(h, ':'))) { /* Second part - NAME|SECRET */
		*k++ = '\0';               /* String separator */
		s = (char*)strchr(k, ':'); /* Thirt part - |SECRET */
	}

	/* Determine algorithm. */
	key_params->algorithm = KNOT_TSIG_ALG_HMAC_MD5;
	if (s) {
		*s++ = '\0';               /* Last part separator */
		knot_lookup_table_t *alg = NULL;
		alg = knot_lookup_by_name(knot_tsig_alg_names, h);
		if (alg) {
			DBG("%s: parsed algorithm '%s'\n", __func__, h);
			key_params->algorithm = alg->id;
		} else {
			ERR("invalid TSIG algorithm name '%s'\n", h);
			free(h);
			return KNOT_EINVAL;
		}
	} else {
		s = k; /* Ignore first part, push down. */
		k = h;
	}

	if (!s) {
		ERR("invalid key option format, use [hmac:]keyname:secret\n");
		free(h);
		return KNOT_EINVAL;
	}

	/* Set key name and secret. */
	key_params->name = knot_dname_from_str(k);
	knot_dname_to_lower(key_params->name);
	int r = knot_binary_from_base64(s, &key_params->secret);
	if (r != KNOT_EOK) {
		free(h);
		return r;
	}

	DBG("%s: parsed name '%s'\n", __func__, k);
	DBG("%s: parsed secret '%s'\n", __func__, s);
	free(h);

	return KNOT_EOK;
}

int params_parse_keyfile(const char *value, knot_key_params_t *key_params)
{
	if (value == NULL || key_params == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	if (key_params->name) {
		ERR("Key specified multiple times.\n");
		return KNOT_EINVAL;
	}

	int result = knot_load_key_params(value, key_params);
	if (result != KNOT_EOK) {
		ERR("could not read key file: %s\n", knot_strerror(result));
		return KNOT_EINVAL;
	}

	return KNOT_EOK;
}
